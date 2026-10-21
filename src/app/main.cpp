// Entry point. orbit.exe is CEF's bootstrap executable (renamed); it loads
// orbit.dll and calls RunWinMain with the sandbox information it created, so
// all Chromium sub-processes run sandboxed. The same entry point serves the
// browser process and every sub-process.

#include <windows.h>
#include <shlobj.h>

#include <share.h>

#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>

#include "app/application.h"
#include "app/installer.h"
#include "app/orbit_app.h"
#include "core/log.h"
#include "core/strings.h"
#include "include/cef_app.h"
#include "include/cef_command_line.h"
#include "include/cef_sandbox_win.h"

namespace {

std::filesystem::path DefaultUserDataDir() {
  PWSTR local = nullptr;
  std::filesystem::path dir;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local))) {
    dir = std::filesystem::path(local) / L"Orbit" / L"User Data";
  }
  ::CoTaskMemFree(local);
  return dir;
}

// Application log: one line per message in "<User Data>\orbit.log".
FILE* g_log_file = nullptr;
std::mutex g_log_mutex;

void FileSink(orbit::log::Level level, const char* file, int line, const char* message) {
  static constexpr const char* kNames[] = {"VERBOSE", "INFO", "WARNING", "ERROR"};
  SYSTEMTIME t;
  ::GetLocalTime(&t);
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (!g_log_file) return;
  std::fprintf(g_log_file, "%04d-%02d-%02d %02d:%02d:%02d.%03d %s %s:%d %s\n", t.wYear, t.wMonth,
               t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
               kNames[static_cast<int>(level)], file, line, message);
  std::fflush(g_log_file);
}

}  // namespace

int RunWinMain(HINSTANCE instance, LPWSTR, int, void* sandbox_info, cef_version_info_t*) {
  // Setup commands (--install / --uninstall / --register) never start CEF.
  if (auto code = orbit::installer::HandleCommandLine(::GetCommandLineW())) return *code;

  CefMainArgs main_args(instance);
  CefRefPtr<orbit::OrbitApp> app = new orbit::OrbitApp();

  // Sub-processes return here.
  const int exit_code = CefExecuteProcess(main_args, app, sandbox_info);
  if (exit_code >= 0) return exit_code;

  ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  // Groups windows on the taskbar with the Start-menu shortcut.
  ::SetCurrentProcessExplicitAppUserModelID(orbit::installer::kAppUserModelId);
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  CefRefPtr<CefCommandLine> command_line = CefCommandLine::CreateCommandLine();
  command_line->InitFromString(::GetCommandLineW());
  std::filesystem::path user_data = command_line->HasSwitch("user-data-dir")
                                        ? std::filesystem::path(
                                              command_line->GetSwitchValue("user-data-dir")
                                                  .ToWString())
                                        : DefaultUserDataDir();
  if (user_data.empty()) return 1;
  user_data = std::filesystem::absolute(user_data);
  std::error_code ec;
  std::filesystem::create_directories(user_data, ec);

  // Append with shared access: a second launch (forwarded to the running
  // instance below) must not truncate the running instance's log.
  g_log_file = _wfsopen((user_data / L"orbit.log").c_str(), L"a", _SH_DENYNO);
  orbit::log::SetSink(&FileSink);
  if (command_line->HasSwitch("orbit-verbose-log")) {
    orbit::log::SetMinLevel(orbit::log::Level::kVerbose);
  }

  CefSettings settings;
  CefString(&settings.root_cache_path) = user_data.wstring();
  CefString(&settings.log_file) = (user_data / L"chromium.log").wstring();
  settings.log_severity = LOGSEVERITY_WARNING;
  settings.persist_session_cookies = false;

  // Only the first instance for a user-data directory touches the database;
  // later launches go straight to CefInitialize, which forwards their command
  // line to the running instance and returns false.
  std::wstring mutex_name = L"Local\\OrbitBrowser-" +
                            std::to_wstring(std::hash<std::wstring>{}(
                                orbit::ToLowerAscii(user_data.wstring())));
  HANDLE instance_mutex = ::CreateMutexW(nullptr, FALSE, mutex_name.c_str());
  const bool primary = instance_mutex && ::GetLastError() != ERROR_ALREADY_EXISTS;

  auto application = std::make_unique<orbit::Application>(user_data);
  if (primary && !application->Initialize()) {
    ::MessageBoxW(nullptr, L"Orbit could not open its profile database.", L"Orbit",
                  MB_OK | MB_ICONERROR);
    return 1;
  }

  if (!CefInitialize(main_args, settings, app, sandbox_info)) {
    // Another instance owns this user-data directory; CEF has already
    // forwarded our command line to it (OnAlreadyRunningAppRelaunch).
    const int code = CefGetExitCode();
    ORBIT_LOG_INFO("CefInitialize returned false (exit code %d)", code);
    application.reset();
    return code;
  }

  if (!primary) {
    // Should not happen: CEF's process singleton normally catches this first.
    CefShutdown();
    return 0;
  }
  CefRunMessageLoop();

  application->Shutdown();
  const int result = application->exit_code();
  application.reset();
  CefShutdown();
  ORBIT_LOG_INFO("clean shutdown");
  orbit::log::SetSink(nullptr);
  if (g_log_file) std::fclose(g_log_file);
  ::CoUninitialize();
  if (instance_mutex) ::CloseHandle(instance_mutex);
  return result;
}
