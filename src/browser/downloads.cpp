#include "browser/downloads.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <fstream>

#include "app/application.h"
#include "browser/profile.h"
#include "core/log.h"
#include "core/strings.h"
#include "ui/browser_window.h"

namespace orbit {

std::filesystem::path DownloadManager::DefaultDirectory() {
  PWSTR path = nullptr;
  std::filesystem::path dir;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_CREATE, nullptr, &path))) {
    dir = path;
  }
  ::CoTaskMemFree(path);
  return dir;
}

std::filesystem::path DownloadManager::Directory() const {
  auto pref = Application::Get()->store().GetPref(Store::kGlobalScope, L"download_dir");
  std::error_code ec;
  if (pref && !pref->empty() && std::filesystem::is_directory(*pref, ec)) return *pref;
  return DefaultDirectory();
}

std::filesystem::path DownloadManager::UniquePath(const std::filesystem::path& dir,
                                                  const std::wstring& file_name) const {
  // Only the final component of the suggested name is used, so a crafted
  // Content-Disposition cannot escape the download directory.
  std::filesystem::path name = std::filesystem::path(file_name).filename();
  if (name.empty() || name == L"." || name == L"..") name = L"download";
  const std::wstring stem = name.stem().wstring();
  const std::wstring ext = name.extension().wstring();
  auto in_use = [&](const std::filesystem::path& p) {
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) return true;
    for (const auto& [id, live] : live_) {
      if (_wcsicmp(live.item.record.path.c_str(), p.c_str()) == 0) return true;
    }
    return false;
  };
  std::filesystem::path candidate = dir / name;
  for (int n = 1; in_use(candidate) && n < 10000; ++n) {
    candidate = dir / (stem + L" (" + std::to_wstring(n) + L")" + ext);
  }
  return candidate;
}

void DownloadManager::OnBeforeDownload(Profile* profile, CefRefPtr<CefDownloadItem> item,
                                       const CefString& suggested_name,
                                       CefRefPtr<CefBeforeDownloadCallback> callback) {
  Application* app = Application::Get();
  const bool ask = app->store().GetPref(Store::kGlobalScope, L"download_ask") == L"1";
  const std::filesystem::path target = UniquePath(Directory(), suggested_name.ToWString());

  Live live;
  live.profile = profile;
  live.item.is_private = !profile || profile->is_private();
  DownloadRecord& rec = live.item.record;
  rec.profile_id = profile ? profile->id() : 0;
  rec.url = item->GetURL().ToWString();
  rec.path = target.wstring();
  rec.mime_type = item->GetMimeType().ToWString();
  rec.total_bytes = item->GetTotalBytes();
  rec.started_at = NowUnixMillis();
  if (live.item.is_private) {
    rec.id = next_session_id_--;
  } else {
    rec.id = app->store().InsertDownload(rec);
  }
  live_[item->GetId()] = std::move(live);
  callback->Continue(target.wstring(), ask);
  ORBIT_LOG_INFO("download %u started", item->GetId());
  NotifyWindows(profile);
}

void DownloadManager::OnDownloadUpdated(CefRefPtr<CefDownloadItem> item,
                                        CefRefPtr<CefDownloadItemCallback> callback) {
  auto it = live_.find(item->GetId());
  if (it == live_.end()) return;
  Live& live = it->second;
  const Profile* owner = live.profile;
  live.control = callback;
  DownloadRecord& rec = live.item.record;
  const DownloadState before = rec.state;
  const std::wstring full_path = item->GetFullPath().ToWString();
  if (!full_path.empty()) rec.path = full_path;  // the user may have chosen another path
  rec.received_bytes = item->GetReceivedBytes();
  rec.total_bytes = item->GetTotalBytes();
  live.item.speed = item->GetCurrentSpeed();
  live.item.percent = item->GetPercentComplete();
  if (item->IsComplete()) {
    rec.state = DownloadState::kComplete;
  } else if (item->IsCanceled()) {
    rec.state = DownloadState::kCancelled;
  } else if (item->IsInterrupted()) {
    rec.state = DownloadState::kInterrupted;
  } else if (item->IsPaused()) {
    rec.state = DownloadState::kPaused;
  } else {
    rec.state = DownloadState::kInProgress;
  }

  const bool finished = rec.state == DownloadState::kComplete ||
                        rec.state == DownloadState::kCancelled ||
                        rec.state == DownloadState::kInterrupted;
  const ULONGLONG now = ::GetTickCount64();
  if (finished) {
    rec.ended_at = NowUnixMillis();
    OnFinished(live);
    Persist(live);
    live_.erase(it);  // the stored record is now authoritative
  } else if (rec.state != before || now - live.last_persist > 2000) {
    live.last_persist = now;
    Persist(live);
  }
  NotifyWindows(owner);
}

void DownloadManager::OnFinished(Live& live) {
  const DownloadRecord& rec = live.item.record;
  if (rec.state == DownloadState::kComplete) {
    if (!EnsureMarkOfTheWeb(rec.path, rec.url, live.item.is_private)) {
      ORBIT_LOG_WARNING("could not verify Mark-of-the-Web on a completed download");
    }
    if (live.item.is_private) {
      // Session-only record: keep it visible until the private window closes.
      private_done_.push_back({live.profile, live.item});
    }
  }
  ORBIT_LOG_INFO("download finished (state %d)", static_cast<int>(rec.state));
}

void DownloadManager::Persist(Live& live) {
  if (live.item.is_private || live.item.record.id <= 0) return;
  Application::Get()->store().UpdateDownload(live.item.record);
}

void DownloadManager::NotifyWindows(const Profile* profile) {
  for (const auto& w : Application::Get()->windows()) {
    if (w->profile() == profile) w->OnDownloadsChanged();
  }
}

std::vector<DownloadManager::Item> DownloadManager::List(Profile* profile) {
  std::vector<Item> out;
  for (const auto& [id, live] : live_) {
    if (live.profile == profile) out.push_back(live.item);
  }
  if (profile && profile->is_private()) {
    for (const auto& done : private_done_) {
      if (done.profile == profile) out.push_back(done.item);
    }
  } else if (profile) {
    for (auto& rec : Application::Get()->store().LoadDownloads(profile->id(), 200)) {
      const bool is_live = std::any_of(out.begin(), out.end(),
                                       [&](const Item& i) { return i.record.id == rec.id; });
      if (is_live) continue;
      Item item;
      item.record = std::move(rec);
      out.push_back(std::move(item));
    }
  }
  std::sort(out.begin(), out.end(), [](const Item& a, const Item& b) {
    return a.record.started_at > b.record.started_at;
  });
  return out;
}

int DownloadManager::ActiveCount(const Profile* profile) const {
  int n = 0;
  for (const auto& [id, live] : live_) {
    if (live.profile == profile) ++n;
  }
  return n;
}

int DownloadManager::ActivePercent(const Profile* profile) const {
  int64_t total = 0, received = 0;
  for (const auto& [id, live] : live_) {
    if (live.profile != profile) continue;
    if (live.item.record.total_bytes <= 0) return -1;
    total += live.item.record.total_bytes;
    received += live.item.record.received_bytes;
  }
  return total > 0 ? static_cast<int>(received * 100 / total) : -1;
}

DownloadManager::Live* DownloadManager::FindLive(const Profile* profile, int64_t id) {
  for (auto& [cef_id, live] : live_) {
    if (live.profile == profile && live.item.record.id == id) return &live;
  }
  return nullptr;
}

std::optional<DownloadManager::Item> DownloadManager::FindItem(Profile* profile, int64_t id) {
  if (Live* live = FindLive(profile, id)) return live->item;
  for (const auto& done : private_done_) {
    if (done.profile == profile && done.item.record.id == id) return done.item;
  }
  if (!profile || profile->is_private()) return std::nullopt;
  for (auto& rec : Application::Get()->store().LoadDownloads(profile->id(), 500)) {
    if (rec.id == id) {
      Item item;
      item.record = std::move(rec);
      return item;
    }
  }
  return std::nullopt;
}

bool DownloadManager::Pause(Profile* profile, int64_t id) {
  Live* live = FindLive(profile, id);
  if (!live || !live->control) return false;
  live->control->Pause();
  return true;
}

bool DownloadManager::Resume(Profile* profile, int64_t id) {
  Live* live = FindLive(profile, id);
  if (!live || !live->control) return false;
  live->control->Resume();
  return true;
}

bool DownloadManager::Cancel(Profile* profile, int64_t id) {
  Live* live = FindLive(profile, id);
  if (!live || !live->control) return false;
  live->control->Cancel();
  return true;
}

bool DownloadManager::Open(Profile* profile, int64_t id, HWND owner) {
  const std::optional<Item> item = FindItem(profile, id);
  if (!item || item->record.state != DownloadState::kComplete) return false;
  const std::filesystem::path path = item->record.path;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) return false;
  if (IsExecutableFileType(path)) {
    const std::wstring content = L"\"" + path.filename().wstring() +
                                 L"\" can run programs on your computer. Only open it if you "
                                 L"trust where it came from.";
    TASKDIALOGCONFIG config{sizeof(config)};
    config.hwndParent = owner;
    config.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
    config.pszWindowTitle = L"Orbit";
    config.pszMainIcon = TD_WARNING_ICON;
    config.pszMainInstruction = L"Open this file?";
    config.pszContent = content.c_str();
    const TASKDIALOG_BUTTON buttons[] = {{IDOK, L"Open"}, {IDCANCEL, L"Cancel"}};
    config.pButtons = buttons;
    config.cButtons = 2;
    config.nDefaultButton = IDCANCEL;
    int choice = IDCANCEL;
    if (FAILED(::TaskDialogIndirect(&config, &choice, nullptr, nullptr)) || choice != IDOK) {
      return false;
    }
  }
  // The shell applies SmartScreen / attachment policies based on the MOTW.
  SHELLEXECUTEINFOW info{sizeof(info)};
  info.fMask = SEE_MASK_NOASYNC;
  info.hwnd = owner;
  info.lpVerb = L"open";
  info.lpFile = path.c_str();
  info.nShow = SW_SHOWNORMAL;
  return ::ShellExecuteExW(&info) != FALSE;
}

bool DownloadManager::ShowInFolder(Profile* profile, int64_t id) {
  const std::optional<Item> item = FindItem(profile, id);
  if (!item) return false;
  PIDLIST_ABSOLUTE pidl = ::ILCreateFromPathW(item->record.path.c_str());
  if (!pidl) {
    // File gone: open the folder instead.
    const std::wstring dir = std::filesystem::path(item->record.path).parent_path().wstring();
    return reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr,
                                                     nullptr, SW_SHOWNORMAL)) > 32;
  }
  const HRESULT hr = ::SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
  ::ILFree(pidl);
  return SUCCEEDED(hr);
}

bool DownloadManager::Remove(Profile* profile, int64_t id) {
  if (FindLive(profile, id)) return false;  // cancel first
  if (profile && profile->is_private()) {
    std::erase_if(private_done_, [&](const PrivateDone& d) {
      return d.profile == profile && d.item.record.id == id;
    });
    return true;
  }
  return profile && Application::Get()->store().DeleteDownload(profile->id(), id);
}

void DownloadManager::ForgetProfile(const Profile* profile) {
  std::erase_if(private_done_, [&](const PrivateDone& d) { return d.profile == profile; });
  for (auto& [id, live] : live_) {
    if (live.profile == profile) live.profile = nullptr;  // keep tracking, detached
  }
}

bool IsExecutableFileType(const std::filesystem::path& file) {
  static constexpr const wchar_t* kTypes[] = {
      L".exe", L".msi", L".msix", L".appx", L".bat", L".cmd", L".com", L".scr", L".pif",
      L".ps1", L".psm1", L".vbs", L".vbe", L".js", L".jse", L".wsf", L".wsh", L".hta",
      L".jar", L".lnk", L".url", L".reg", L".cpl", L".msc", L".application", L".appref-ms",
      L".dll", L".sys", L".inf", L".iso", L".img", L".vhd", L".vhdx"};
  const std::wstring ext = ToLowerAscii(file.extension().wstring());
  for (const wchar_t* t : kTypes) {
    if (ext == t) return true;
  }
  return false;
}

bool EnsureMarkOfTheWeb(const std::filesystem::path& file, const std::wstring& source_url,
                        bool is_private) {
  const std::wstring stream = file.wstring() + L":Zone.Identifier";
  if (::GetFileAttributesW(stream.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
  // Chromium normally writes this itself; add it if it is missing. Private
  // downloads do not record their source (matching Chrome's incognito mode).
  std::string content = "[ZoneTransfer]\r\nZoneId=3\r\n";
  const std::wstring lower = ToLowerAscii(source_url);
  if (!is_private && (lower.starts_with(L"http://") || lower.starts_with(L"https://"))) {
    content += "HostUrl=" + ToUtf8(source_url) + "\r\n";
  } else {
    content += "HostUrl=about:internet\r\n";
  }
  HANDLE h = ::CreateFileW(stream.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL ok = ::WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &written,
                              nullptr);
  ::CloseHandle(h);
  return ok && written == content.size();
}

}  // namespace orbit
