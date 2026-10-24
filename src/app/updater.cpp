#include "app/updater.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "app/application.h"
#include "core/json.h"
#include "core/log.h"
#include "core/strings.h"
#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"
#include "persistence/store.h"
#include "ui/browser_window.h"
#include "ui/toast.h"

namespace orbit::updater {
namespace {

constexpr unsigned long long kDayMs = 24ull * 60 * 60 * 1000;
constexpr uint64_t kMaxSetupBytes = 600ull * 1024 * 1024;

std::mutex g_mutex;  // guards everything below
Status g_status;
std::wstring g_asset_url;
std::string g_asset_sha256;  // lowercase hex; empty when GitHub listed none
std::filesystem::path g_setup;
bool g_run_setup = false;

unsigned long long NowMs() {
  FILETIME ft;
  ::GetSystemTimeAsFileTime(&ft);
  return ((static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000;
}

std::string Hex(const unsigned char* data, size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < n; ++i) {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 15];
  }
  return out;
}

std::string LowerAscii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

// One HTTPS GET; |sink| receives the body in pieces (false aborts). WinHTTP
// follows redirects (GitHub's asset links redirect to its download host),
// never from HTTPS to HTTP.
bool HttpsGet(const std::wstring& url, const wchar_t* accept, const std::function<bool(const char*, size_t)>& sink,
              const std::function<void(uint64_t)>& on_length, std::string* error) {
  URL_COMPONENTS parts{sizeof(parts)};
  wchar_t host[256] = {}, path[2048] = {};
  parts.lpszHostName = host;
  parts.dwHostNameLength = 256;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 2048;
  if (!::WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
    *error = "bad address";
    return false;
  }
  const std::wstring agent = L"Orbit/" + FromUtf8(ORBIT_VERSION);
  HINTERNET session = ::WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    *error = "no network";
    return false;
  }
  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
  ::WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
  ::WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
  HINTERNET connect = ::WinHttpConnect(session, host, parts.nPort, 0);
  HINTERNET request = connect ? ::WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                              : nullptr;
  bool ok = false;
  if (request) {
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    ::WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    const std::wstring headers = std::wstring(L"Accept: ") + accept;
    if (::WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        ::WinHttpReceiveResponse(request, nullptr)) {
      DWORD status = 0, size = sizeof(status);
      ::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
      if (status == 200) {
        wchar_t length[32] = {};
        DWORD length_size = sizeof(length);
        if (on_length && ::WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                               length, &length_size, WINHTTP_NO_HEADER_INDEX)) {
          on_length(_wcstoui64(length, nullptr, 10));
        }
        ok = true;
        std::vector<char> buffer(64 * 1024);
        for (;;) {
          DWORD read = 0;
          if (!::WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            *error = "the connection was interrupted";
            ok = false;
            break;
          }
          if (read == 0) break;
          if (!sink(buffer.data(), read)) {
            ok = false;
            break;
          }
        }
      } else {
        // 404: no published release yet (or the repository is private).
        *error = status == 404 ? "no published release found" : "GitHub answered " + std::to_string(status);
      }
    } else {
      *error = "could not reach GitHub";
    }
  }
  if (request) ::WinHttpCloseHandle(request);
  if (connect) ::WinHttpCloseHandle(connect);
  ::WinHttpCloseHandle(session);
  return ok;
}

void ShowFoundToast(const std::string& version) {
  Application* app = Application::Get();
  if (!app) return;
  for (const auto& w : app->windows()) {
    if (w->closing() || w->profile()->is_private()) continue;
    ui::Toast::Content content;
    content.id = L"update-available";
    content.text = L"Orbit " + FromUtf8(version) + L" is available";
    content.timeout_ms = 10000;
    content.button_label = L"Update";
    content.on_button = [window_id = w->hwnd()] {
      for (const auto& win : Application::Get()->windows()) {
        if (win->hwnd() == window_id && !win->closing()) win->OpenSettingsPage();
      }
    };
    w->ShowToast(content);
    break;
  }
}

}  // namespace

const char* StateName(State state) {
  switch (state) {
    case State::kIdle: return "idle";
    case State::kChecking: return "checking";
    case State::kUpToDate: return "up-to-date";
    case State::kAvailable: return "available";
    case State::kDownloading: return "downloading";
    case State::kReady: return "ready";
    case State::kError: return "error";
  }
  return "idle";
}

bool IsNewer(const std::string& candidate, const std::string& current) {
  auto parse = [](std::string v) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    std::vector<long> parts;
    size_t at = 0;
    while (parts.size() < 4) {
      const size_t dot = v.find('.', at);
      parts.push_back(std::strtol(v.substr(at, dot == std::string::npos ? std::string::npos : dot - at).c_str(),
                                  nullptr, 10));
      if (dot == std::string::npos) break;
      at = dot + 1;
    }
    parts.resize(4, 0);
    return parts;
  };
  return parse(candidate) > parse(current);
}

Status Current() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_status;
}

void Check(bool quiet) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_status.state == State::kChecking || g_status.state == State::kDownloading ||
        g_status.state == State::kReady) {
      return;
    }
    g_status = Status{State::kChecking};
  }
  if (Application* app = Application::Get()) {
    app->store().SetPref(Store::kGlobalScope, L"update_checked_at", std::to_wstring(NowMs()));
  }
  std::thread([quiet] {
    std::string body, error;
    const std::wstring url = std::wstring(L"https://api.github.com/repos/") + kRepository + L"/releases/latest";
    const bool ok = HttpsGet(
        url, L"application/vnd.github+json",
        [&body](const char* data, size_t n) {
          body.append(data, n);
          return body.size() < 4 * 1024 * 1024;
        },
        nullptr, &error);
    Status s;
    std::wstring asset_url;
    std::string sha;
    if (!ok) {
      s.state = State::kError;
      s.error = error.empty() ? "the answer was too large" : error;
    } else if (auto json = ParseJson(body); !json || !json->is_object()) {
      s.state = State::kError;
      s.error = "unexpected answer from GitHub";
    } else {
      const std::string tag = json->Str("tag_name");
      s.latest = !tag.empty() && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
      s.notes_url = json->Str("html_url");
      if (const JsonValue* assets = json->Get("assets"); assets && assets->is_array()) {
        for (const JsonValue& a : assets->array) {
          // GitHub turns the spaces of "Orbit Browser Setup.exe" into dots.
          const std::string name = a.Str("name");
          if (name.size() < 4 || name.substr(name.size() - 4) != ".exe" || name.find("Setup") == std::string::npos) {
            continue;
          }
          asset_url = FromUtf8(a.Str("browser_download_url"));
          const std::string digest = a.Str("digest");
          if (digest.rfind("sha256:", 0) == 0) sha = LowerAscii(digest.substr(7));
          break;
        }
      }
      if (!IsNewer(s.latest, ORBIT_VERSION)) {
        s.state = State::kUpToDate;
      } else if (asset_url.rfind(L"https://", 0) != 0) {
        s.state = State::kError;
        s.error = "the release has no setup program";
      } else {
        s.state = State::kAvailable;
      }
    }
    ORBIT_LOG_INFO("update check: %s, latest %s%s%s", StateName(s.state), s.latest.c_str(),
                   s.error.empty() ? "" : ", ", s.error.c_str());
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_status = s;
      g_asset_url = asset_url;
      g_asset_sha256 = sha;
    }
    if (quiet && s.state == State::kAvailable) {
      CefPostTask(TID_UI, base::BindOnce([](std::string v) { ShowFoundToast(v); }, s.latest));
    }
  }).detach();
}

void MaybeCheckInBackground() {
  Application* app = Application::Get();
  if (!app) return;
  Store& store = app->store();
  if (store.GetPref(Store::kGlobalScope, L"update_auto_check").value_or(L"1") != L"1") return;
  const unsigned long long now = NowMs();
  const unsigned long long last =
      std::wcstoull(store.GetPref(Store::kGlobalScope, L"update_checked_at").value_or(L"0").c_str(), nullptr, 10);
  if (last && now >= last && now - last < kDayMs) return;
  // Not during startup: the windows come first.
  CefPostDelayedTask(TID_UI, base::BindOnce([] {
                       if (Application::Get()) Check(true);
                     }),
                     20000);
}

void Download() {
  std::wstring url;
  std::string sha;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool retry = g_status.state == State::kError && !g_asset_url.empty() && !g_status.latest.empty() &&
                       IsNewer(g_status.latest, ORBIT_VERSION);
    if (g_status.state != State::kAvailable && !retry) return;
    url = g_asset_url;
    sha = g_asset_sha256;
    g_status.state = State::kDownloading;
    g_status.progress = 0;
    g_status.error.clear();
  }
  wchar_t temp[MAX_PATH] = {};
  ::GetTempPathW(MAX_PATH, temp);
  const std::filesystem::path dir = std::filesystem::path(temp) / L"OrbitUpdate";
  std::thread([url, sha, dir] {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / L"Orbit Browser Setup.exe";
    FILE* out = _wfopen(file.c_str(), L"wb");
    std::string error;
    bool ok = out != nullptr;
    if (!ok) error = "could not save the download";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
      ::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    }
    uint64_t total = 0, received = 0;
    if (ok) {
      ok = HttpsGet(
          url, L"application/octet-stream",
          [&](const char* data, size_t n) {
            received += n;
            if (received > kMaxSetupBytes || std::fwrite(data, 1, n, out) != n) {
              error = "could not save the download";
              return false;
            }
            if (hash) {
              ::BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data)), static_cast<ULONG>(n), 0);
            }
            if (total) {
              std::lock_guard<std::mutex> lock(g_mutex);
              g_status.progress = static_cast<int>(received * 100 / total);
            }
            return true;
          },
          [&total](uint64_t length) { total = length; }, &error);
    }
    if (out) std::fclose(out);
    unsigned char digest[32] = {};
    const bool hashed = hash && BCRYPT_SUCCESS(::BCryptFinishHash(hash, digest, 32, 0));
    if (hash) ::BCryptDestroyHash(hash);
    if (alg) ::BCryptCloseAlgorithmProvider(alg, 0);
    if (ok && total && received != total) {
      ok = false;
      error = "the download is incomplete";
    }
    if (ok && !sha.empty() && (!hashed || Hex(digest, 32) != sha)) {
      ok = false;
      error = "the download does not match the release's checksum";
    }
    if (!ok) std::filesystem::remove(file, ec);
    ORBIT_LOG_INFO("update download: %s%s", ok ? "ready" : "failed, ", ok ? "" : error.c_str());
    std::lock_guard<std::mutex> lock(g_mutex);
    if (ok) {
      g_setup = file;
      g_status.state = State::kReady;
      g_status.progress = 100;
    } else {
      g_status.state = State::kError;
      g_status.error = error.empty() ? "the download failed" : error;
    }
  }).detach();
}

void InstallAndRestart() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_status.state != State::kReady || g_setup.empty()) return;
    g_run_setup = true;
  }
  Application* app = Application::Get();
  if (!app) return;
  // The restarted Orbit shows the tabs that were open, not the empty start tab.
  app->store().SetPref(Store::kGlobalScope, L"restore_selected_once", L"1");
  ORBIT_LOG_INFO("update: closing to install");
  app->CloseAllWindows();
}

void RunPendingInstaller() {
  std::filesystem::path setup;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_run_setup) return;
    setup = g_setup;
  }
  // Silent setup; /relaunch=1 makes it start Orbit when it is done.
  std::wstring command = L"\"" + setup.wstring() + L"\" /SILENT /SP- /SUPPRESSMSGBOXES /NOCANCEL /relaunch=1";
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (::CreateProcessW(setup.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                       setup.parent_path().c_str(), &si, &pi)) {
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    ORBIT_LOG_INFO("update: setup started");
  } else {
    ORBIT_LOG_ERROR("update: could not start the setup (%lu)", ::GetLastError());
  }
}

}  // namespace orbit::updater
