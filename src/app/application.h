#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "include/cef_base.h"
#include "persistence/database.h"
#include "persistence/store.h"
#include "ui/theme.h"

namespace orbit {

class BrowserClient;
class DownloadManager;
class PermissionManager;
class BrowserWindow;
class Profile;
class Tab;
class Workspace;

// Browser-process controller. Lives on the UI thread from CefInitialize until
// after the message loop exits. Owns the database, profiles (and through them
// workspaces and tabs) and all windows.
class Application {
 public:
  static constexpr wchar_t kNewTabUrl[] = L"orbit://newtab/";
  static constexpr wchar_t kDefaultSearch[] = L"https://duckduckgo.com/?q={query}";

  static Application* Get();

  explicit Application(std::filesystem::path user_data_dir);
  ~Application();
  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  // Opens the database. Called before CefInitialize.
  bool Initialize();
  // Called from OnContextInitialized: restores the previous session and
  // opens |urls| (command-line arguments) in the first window.
  void Start(const std::vector<std::wstring>& urls);
  // Called from OnAlreadyRunningAppRelaunch with the new launch's arguments.
  void OnRelaunch(const std::vector<std::wstring>& urls);
  // Called after the message loop exits, before CefShutdown.
  void Shutdown();

  const std::filesystem::path& user_data_dir() const { return user_data_dir_; }
  std::filesystem::path ProfileDir(const std::wstring& uuid) const;
  int exit_code() const { return exit_code_; }
  void set_exit_code(int code) { exit_code_ = code; }
  Store& store() { return *store_; }
  BrowserClient* client() const;
  DownloadManager& downloads() { return *downloads_; }
  PermissionManager& permissions() { return *permissions_; }

  // History: records a completed main-frame navigation of |tab| (skipped for
  // private profiles and non-web URLs).
  void RecordVisit(Tab* tab);
  void UpdateVisitTitle(Tab* tab);
  // Re-reads theme/accent prefs and repaints every window.
  void ApplyThemeToAllWindows();
  void ApplySidebarModeToAllWindows(bool compact);
  void ApplyCompactModeToAllWindows(bool hidden);

  // Preferences.
  std::wstring search_template();
  ui::ThemeMode theme_mode();
  uint32_t accent_color();
  bool dark_mode();

  // Profiles.
  const std::vector<std::unique_ptr<Profile>>& profiles() const { return profiles_; }
  Profile* FindProfile(int64_t id) const;
  // Also finds private profiles (which have negative, session-only ids).
  Profile* FindAnyProfile(int64_t id) const;
  Profile* CreateProfile(const std::wstring& name, uint32_t color);
  Workspace* CreateWorkspace(Profile* profile, const std::wstring& name, uint32_t color);
  // Removes a workspace object; tabs whose browsers are still closing are kept
  // alive until Chromium reports them closed.
  void RetireWorkspace(Workspace* workspace);
  // Closes the profile's windows, then deletes its rows and (on next start,
  // once Chromium has released it) its data directory.
  void DeleteProfile(Profile* profile);

  // Windows.
  BrowserWindow* OpenWindow(Profile* profile, Workspace* workspace,
                            const WindowRecord* placement);
  BrowserWindow* OpenPrivateWindow();
  const std::vector<std::unique_ptr<BrowserWindow>>& windows() const { return windows_; }
  void OnWindowCloseRequested(BrowserWindow* window);
  void CloseAllWindows();
  void OnWindowDestroyed(BrowserWindow* window);
  // A workspace not shown in any window, or a new one if all are in use.
  Workspace* FreeWorkspaceFor(Profile* profile);

  // Tab lifecycle.
  // Removes |tab| from its workspace immediately (no browser close needed).
  void RemoveTab(Tab* tab, bool remember);
  // Chromium confirmed a user close: move the tab out of its workspace and
  // keep it alive until OnBrowserClosed.
  void DetachClosingTab(Tab* tab);
  void OnBrowserClosed(Tab* tab);
  bool ReopenClosedTab(BrowserWindow* window);

  // Inactive-tab policy ("put to sleep after N minutes"); re-reads the pref.
  void UpdateSleepPolicy();
  // One policy pass with an explicit idle threshold (also used by tests).
  // Tabs are first queried for media playback and only put to sleep on a
  // later pass if no frame reported playing. Returns tabs put to sleep.
  int ApplySleepPolicy(int64_t idle_ms);

  // Persistence: tab metadata and window placement are flushed on a short
  // debounce instead of on every engine callback.
  void ScheduleSave();
  void SaveNow();

 private:
  void LoadProfiles();
  void DeletePendingProfileDirectories();
  void FinishProfileDeletions();
  void RememberClosedTab(Tab* tab);
  void CheckPendingWindowCloses();
  void MaybeQuit();
  void SleepPolicyTick(uint64_t generation);
  std::vector<WindowRecord> CollectWindowRecords() const;

  std::filesystem::path user_data_dir_;
  Database db_;
  std::unique_ptr<Store> store_;
  CefRefPtr<BrowserClient> client_;
  std::unique_ptr<DownloadManager> downloads_;
  std::unique_ptr<PermissionManager> permissions_;
  std::vector<std::unique_ptr<Profile>> profiles_;
  std::vector<std::unique_ptr<Profile>> private_profiles_;
  std::vector<std::unique_ptr<BrowserWindow>> windows_;
  std::vector<std::unique_ptr<Tab>> closing_tabs_;
  std::vector<Profile*> profiles_pending_delete_;
  bool save_scheduled_ = false;
  bool quitting_ = false;
  bool started_ = false;
  int64_t next_private_id_ = -1;
  int sleep_minutes_ = 0;
  uint64_t sleep_generation_ = 0;  // invalidates stale scheduled ticks
  int exit_code_ = 0;
};

}  // namespace orbit
