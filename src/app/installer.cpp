#include "app/installer.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "version.h"  // generated
#include "core/log.h"

namespace orbit::installer {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kProgId[] = L"OrbitHTML";
constexpr wchar_t kClientKey[] = L"Software\\Clients\\StartMenuInternet\\Orbit";
constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Orbit";
constexpr wchar_t kAppPathsKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\orbit.exe";

std::filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR path = nullptr;
  std::filesystem::path out;
  if (SUCCEEDED(::SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path))) out = path;
  ::CoTaskMemFree(path);
  return out;
}

std::filesystem::path CurrentExe() {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (n < buffer.size()) {
      buffer.resize(n);
      return buffer;
    }
    buffer.resize(buffer.size() * 2);
  }
}

bool SetString(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value) {
  return ::RegSetKeyValueW(root, key.c_str(), name, REG_SZ, value.c_str(),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) ==
         ERROR_SUCCESS;
}

bool SetDword(HKEY root, const std::wstring& key, const wchar_t* name, DWORD value) {
  return ::RegSetKeyValueW(root, key.c_str(), name, REG_DWORD, &value, sizeof(value)) ==
         ERROR_SUCCESS;
}

std::wstring Quote(const std::filesystem::path& p) { return L"\"" + p.wstring() + L"\""; }

bool Register(const std::filesystem::path& exe) {
  const std::wstring icon = exe.wstring() + L",0";
  const std::wstring open = Quote(exe) + L" \"%1\"";
  bool ok = true;
  // ProgID used for http/https/.html associations.
  const std::wstring progid = std::wstring(L"Software\\Classes\\") + kProgId;
  ok &= SetString(HKEY_CURRENT_USER, progid, nullptr, L"Orbit HTML Document");
  ok &= SetString(HKEY_CURRENT_USER, progid + L"\\DefaultIcon", nullptr, icon);
  ok &= SetString(HKEY_CURRENT_USER, progid + L"\\shell\\open\\command", nullptr, open);
  ok &= SetString(HKEY_CURRENT_USER, progid + L"\\Application", L"ApplicationName", L"Orbit");
  ok &= SetString(HKEY_CURRENT_USER, progid + L"\\Application", L"AppUserModelId",
                  kAppUserModelId);
  // Browser capabilities (shown in Settings > Apps > Default apps).
  const std::wstring client = kClientKey;
  const std::wstring caps = client + L"\\Capabilities";
  ok &= SetString(HKEY_CURRENT_USER, client, nullptr, L"Orbit");
  ok &= SetString(HKEY_CURRENT_USER, client + L"\\DefaultIcon", nullptr, icon);
  ok &= SetString(HKEY_CURRENT_USER, client + L"\\shell\\open\\command", nullptr, Quote(exe));
  ok &= SetString(HKEY_CURRENT_USER, caps, L"ApplicationName", L"Orbit");
  ok &= SetString(HKEY_CURRENT_USER, caps, L"ApplicationDescription",
                  L"A fast, private Chromium browser with vertical tabs and workspaces.");
  ok &= SetString(HKEY_CURRENT_USER, caps, L"ApplicationIcon", icon);
  for (const wchar_t* scheme : {L"http", L"https"}) {
    ok &= SetString(HKEY_CURRENT_USER, caps + L"\\URLAssociations", scheme, kProgId);
  }
  for (const wchar_t* ext : {L".htm", L".html", L".shtml", L".xhtml", L".svg", L".pdf"}) {
    ok &= SetString(HKEY_CURRENT_USER, caps + L"\\FileAssociations", ext, kProgId);
  }
  ok &= SetString(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", L"Orbit",
                  caps);
  // "Run" dialog / App Paths.
  ok &= SetString(HKEY_CURRENT_USER, kAppPathsKey, nullptr, exe.wstring());
  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return ok;
}

void Unregister() {
  ::RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + kProgId).c_str());
  ::RegDeleteTreeW(HKEY_CURRENT_USER, kClientKey);
  ::RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", L"Orbit");
  ::RegDeleteTreeW(HKEY_CURRENT_USER, kAppPathsKey);
  ::RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

std::filesystem::path ShortcutPath() {
  return KnownFolder(FOLDERID_Programs) / L"Orbit.lnk";
}

bool CreateShortcut(const std::filesystem::path& exe) {
  ComPtr<IShellLinkW> link;
  if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link)))) {
    return false;
  }
  link->SetPath(exe.c_str());
  link->SetWorkingDirectory(exe.parent_path().c_str());
  link->SetDescription(L"Orbit browser");
  link->SetIconLocation(exe.c_str(), 0);
  ComPtr<IPropertyStore> props;
  if (SUCCEEDED(link.As(&props))) {
    PROPVARIANT pv;
    if (SUCCEEDED(::InitPropVariantFromString(kAppUserModelId, &pv))) {
      props->SetValue(PKEY_AppUserModel_ID, pv);
      props->Commit();
      ::PropVariantClear(&pv);
    }
  }
  ComPtr<IPersistFile> file;
  return SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Save(ShortcutPath().c_str(), TRUE));
}

uint64_t DirectorySizeKb(const std::filesystem::path& dir) {
  uint64_t total = 0;
  std::error_code ec;
  for (const auto& e : std::filesystem::recursive_directory_iterator(dir, ec)) {
    if (e.is_regular_file(ec)) total += e.file_size(ec);
  }
  return total / 1024;
}

bool CopyTree(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::error_code ec;
  std::filesystem::create_directories(to, ec);
  for (const auto& e : std::filesystem::recursive_directory_iterator(from, ec)) {
    const auto rel = std::filesystem::relative(e.path(), from, ec);
    const std::wstring name = e.path().filename().wstring();
    // Never ship test binaries or debug symbols in an installation.
    if (name.starts_with(L"orbit_unit_tests") || e.path().extension() == L".pdb" ||
        e.path().extension() == L".ilk") {
      continue;
    }
    if (e.is_directory(ec)) {
      std::filesystem::create_directories(to / rel, ec);
    } else if (e.is_regular_file(ec)) {
      std::filesystem::copy_file(e.path(), to / rel,
                                 std::filesystem::copy_options::overwrite_existing, ec);
      if (ec) {
        ORBIT_LOG_ERROR("install: copy failed: %s", ec.message().c_str());
        return false;
      }
    }
  }
  return true;
}

int Message(const wchar_t* text, UINT flags) {
  return ::MessageBoxW(nullptr, text, L"Orbit Setup", flags);
}

bool OrbitRunning() {
  // Any visible Orbit window means files are in use.
  return ::FindWindowExW(nullptr, nullptr, L"OrbitBrowserWindow", nullptr) != nullptr;
}

int Install(bool quiet) {
  const std::filesystem::path source_exe = CurrentExe();
  const std::filesystem::path target_dir = InstallDirectory();
  const std::filesystem::path target_exe = target_dir / L"orbit.exe";
  std::error_code ec;
  if (OrbitRunning()) {
    if (!quiet) Message(L"Please close Orbit before installing.", MB_OK | MB_ICONWARNING);
    return 3;
  }
  if (!std::filesystem::equivalent(source_exe.parent_path(), target_dir, ec)) {
    if (!CopyTree(source_exe.parent_path(), target_dir)) {
      if (!quiet) Message(L"Installation failed while copying files.", MB_OK | MB_ICONERROR);
      return 1;
    }
    // Sandboxed (LPAC) processes need read access to the binaries.
    std::wstring cmd = L"icacls \"" + target_dir.wstring() + L"\" /grant *S-1-15-2-2:(OI)(CI)(RX) /Q";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                         nullptr, &si, &pi)) {
      ::WaitForSingleObject(pi.hProcess, 30000);
      ::CloseHandle(pi.hThread);
      ::CloseHandle(pi.hProcess);
    }
  }
  bool ok = Register(target_exe) && CreateShortcut(target_exe);
  const std::wstring uninstall = kUninstallKey;
  ok &= SetString(HKEY_CURRENT_USER, uninstall, L"DisplayName", L"Orbit");
  ok &= SetString(HKEY_CURRENT_USER, uninstall, L"DisplayVersion", ORBIT_VERSION_WSTR);
  ok &= SetString(HKEY_CURRENT_USER, uninstall, L"Publisher", L"Orbit Project");
  ok &= SetString(HKEY_CURRENT_USER, uninstall, L"InstallLocation", target_dir.wstring());
  ok &= SetString(HKEY_CURRENT_USER, uninstall, L"DisplayIcon", target_exe.wstring() + L",0");
  ok &= SetString(HKEY_CURRENT_USER, uninstall, L"UninstallString",
                  Quote(target_exe) + L" --uninstall");
  ok &= SetString(HKEY_CURRENT_USER, uninstall, L"QuietUninstallString",
                  Quote(target_exe) + L" --uninstall --quiet");
  ok &= SetDword(HKEY_CURRENT_USER, uninstall, L"NoModify", 1);
  ok &= SetDword(HKEY_CURRENT_USER, uninstall, L"NoRepair", 1);
  ok &= SetDword(HKEY_CURRENT_USER, uninstall, L"EstimatedSize",
                 static_cast<DWORD>(DirectorySizeKb(target_dir)));
  if (!ok) {
    if (!quiet) Message(L"Orbit was copied, but registration was incomplete.", MB_OK | MB_ICONWARNING);
    return 1;
  }
  if (!quiet) {
    const int choice = Message(
        L"Orbit is installed.\n\nWindows only lets you choose the default browser yourself. "
        L"Open Default apps settings now?",
        MB_YESNO | MB_ICONINFORMATION);
    if (choice == IDYES) {
      ::ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps?registeredAppUser=Orbit",
                      nullptr, nullptr, SW_SHOWNORMAL);
    }
  }
  return 0;
}

int Uninstall(bool quiet) {
  if (OrbitRunning()) {
    if (!quiet) Message(L"Please close Orbit before uninstalling.", MB_OK | MB_ICONWARNING);
    return 3;
  }
  if (!quiet && Message(L"Remove Orbit from this computer?", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
    return 4;
  }
  bool delete_data = false;
  if (!quiet) {
    delete_data = Message(L"Also delete your Orbit browsing data (profiles, history, bookmarks, "
                          L"cookies)?\n\nChoose No to keep it for a later reinstall.",
                          MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
  }
  Unregister();
  std::error_code ec;
  std::filesystem::remove(ShortcutPath(), ec);
  const std::filesystem::path dir = InstallDirectory();
  const std::filesystem::path data = KnownFolder(FOLDERID_LocalAppData) / L"Orbit";
  // The running executable cannot delete itself: hand the removal to a
  // short-lived cmd process that waits for this process to exit.
  std::wstring cmd = L"cmd.exe /c ping 127.0.0.1 -n 3 > nul & rmdir /s /q \"" + dir.wstring() + L"\"";
  if (delete_data) cmd += L" & rmdir /s /q \"" + data.wstring() + L"\"";
  const std::filesystem::path exe_dir = CurrentExe().parent_path();
  if (std::filesystem::equivalent(exe_dir, dir, ec)) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                         KnownFolder(FOLDERID_LocalAppData).c_str(), &si, &pi)) {
      ::CloseHandle(pi.hThread);
      ::CloseHandle(pi.hProcess);
    }
  } else if (delete_data) {
    std::filesystem::remove_all(data, ec);
  }
  if (!quiet) Message(L"Orbit has been removed.", MB_OK | MB_ICONINFORMATION);
  return 0;
}

bool HasToken(const std::wstring& command_line, const wchar_t* token) {
  int argc = 0;
  LPWSTR* argv = ::CommandLineToArgvW(command_line.c_str(), &argc);
  bool found = false;
  for (int i = 1; argv && i < argc; ++i) found = found || _wcsicmp(argv[i], token) == 0;
  ::LocalFree(argv);
  return found;
}

}  // namespace

std::filesystem::path InstallDirectory() {
  return KnownFolder(FOLDERID_UserProgramFiles) / L"Orbit";
}

std::optional<int> HandleCommandLine(const wchar_t* command_line) {
  const std::wstring cl = command_line ? command_line : L"";
  const bool install = HasToken(cl, L"--install");
  const bool uninstall = HasToken(cl, L"--uninstall");
  const bool reg = HasToken(cl, L"--register");
  if (!install && !uninstall && !reg) return std::nullopt;
  const bool quiet = HasToken(cl, L"--quiet");
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int code = 0;
  if (uninstall) {
    code = Uninstall(quiet);
  } else if (install) {
    code = Install(quiet);
  } else {
    code = Register(CurrentExe()) ? 0 : 1;
  }
  ::CoUninitialize();
  return code;
}

}  // namespace orbit::installer
