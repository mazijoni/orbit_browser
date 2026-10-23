#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <map>

#include "include/cef_base.h"
#include "include/cef_values.h"
#include "browser/live_folders.h"
#include "core/shortcuts.h"
#include "persistence/database.h"
#include "persistence/store.h"
#include "ui/theme.h"

namespace orbit {

class BrowserClient;
class DownloadManager;
class ExtensionBridge;
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
  LiveFolders& live_folders() { return live_folders_; }

  // Keyboard shortcut overrides (prefs "shortcut:<id>": a chord, or "none").
  const ShortcutMap& shortcuts() const { return shortcuts_; }
  void SetShortcut(const std::wstring& id, const KeyChord& chord);  // persisted
  void ResetShortcut(const std::wstring& id);                        // "" = all
  // While the settings page records a chord, its keys reach the page instead
  // of running shortcuts (expires after 15 s).
  void SetShortcutRecording(bool on);
  // Space routing (prefs "space_routes", "space_route_external"): the space
  // a new tab for |url| belongs in, if a rule (or the external default) says so.
  Workspace* RouteTarget(Profile* profile, const std::wstring& url, bool external);
  bool ShortcutRecording() const;
  BrowserClient* client() const;
  DownloadManager& downloads() { return *downloads_; }
  PermissionManager& permissions() { return *permissions_; }

  // History: records a completed main-frame navigation of |tab| (skipped for
  // private profiles and non-web URLs).
  void RecordVisit(Tab* tab);
  void UpdateVisitTitle(Tab* tab);
  // Re-reads theme/accent prefs and repaints every window.
  void ApplyThemeToAllWindows();
  // zen.glance.enabled / zen.glance.activation-method: "alt", "ctrl",
  // "shift", or "" when glance is off. Read by every new browser's renderer.
  std::wstring GlanceTrigger();
  // Extra info for a new browser's renderer (OrbitApp::OnBrowserCreated).
  CefRefPtr<CefDictionaryValue> RendererInfo();
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
  // |with_space|: also a "Home" space (a profile may have none: its spaces
  // can be moved to other profiles).
  Profile* CreateProfile(const std::wstring& name, uint32_t color, bool with_space = true);
  Workspace* CreateWorkspace(Profile* profile, const std::wstring& name, uint32_t color);

  // Spaces and profiles (Zen's container per space): one window switches
  // between the spaces of every normal profile, listed in one order.
  std::vector<Workspace*> Spaces() const;
  Workspace* FindSpace(int64_t id) const;  // a space or linked space of a normal profile
  void MoveSpace(Workspace* space, int new_index);  // in Spaces(), stored
  // Moves |space| (its Essentials and linked spaces too) to |profile|. Its
  // tabs are unloaded and load again, signed out, in the new storage.
  bool MoveSpaceToProfile(Workspace* space, Profile* profile);
  // Every tab container of every profile, private ones included.
  std::vector<Workspace*> AllContainers() const;
  // Removes a workspace object; tabs whose browsers are still closing are kept
  // alive until Chromium reports them closed. Its linked spaces become spaces.
  void RetireWorkspace(Workspace* workspace);

  // Linked window spaces (Orbit's answer to Zen's window sync): a tab torn
  // off into its own window gets a temporary space linked to the space it
  // came from (same profile, so the same history, passwords and cookies).
  Workspace* CreateLinkedSpace(Workspace* parent);
  // The window that owns |linked| (shows it, or switched away from it).
  BrowserWindow* WindowOwningLinked(const Workspace* linked) const;
  // Its tabs go back to the parent space (in a folder named after it when
  // |into_folder|) and the linked space is deleted; |closing| (the linked
  // window, or null) closes.
  void ReturnLinkedSpace(Workspace* linked, bool into_folder, BrowserWindow* closing);
  // "Bring tabs back" / the linked window's back arrow: the tabs return
  // loose and the linked window closes (or, when the parent space is not on
  // screen anywhere, turns back into the parent's window).
  void BringBackLinkedSpace(Workspace* linked);
  // Closes the profile's windows, then deletes its rows and (on next start,
  // once Chromium has released it) its data directory.
  void DeleteProfile(Profile* profile);

  // The profile's window onto Chrome's extensions (a hidden browser), started
  // on first use. Null for private profiles, which run no extensions.
  ExtensionBridge* Extensions(Profile* profile);
  // The profile's bridge if one exists (never starts one).
  ExtensionBridge* FindExtensions(const Profile* profile) const;
  // The profile's extensions changed: its windows refresh their toolbar button.
  void OnExtensionsChanged(Profile* profile);
  // The extensions pinned to the toolbar, in order (per profile).
  std::vector<std::string> PinnedExtensions(const Profile* profile);
  void SetExtensionPinned(Profile* profile, const std::string& id, bool pinned);
  // An extension was just installed in |profile| (Orbit's toast replaces Chrome's bubble).
  void OnExtensionAdded(Profile* profile, const std::string& name);
  // True once a profile's extensions directory holds an installed extension.
  static bool HasInstalledExtensions(const Profile& profile);

  // Windows.
  // |start_empty|: the restored space opens on an empty tab instead of its
  // last selected one (zen.workspaces.continue-where-left-off off).
  BrowserWindow* OpenWindow(Profile* profile, Workspace* workspace,
                            const WindowRecord* placement, bool start_empty = false);
  BrowserWindow* OpenPrivateWindow();
  const std::vector<std::unique_ptr<BrowserWindow>>& windows() const { return windows_; }
  void OnWindowCloseRequested(BrowserWindow* window);
  void CloseAllWindows();
  void OnWindowDestroyed(BrowserWindow* window);
  // A closing window's own browser (its library panel) has closed.
  void OnWindowBrowserClosed() { CheckPendingWindowCloses(); }
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
  // The windows that display |ws|: its window, or for an Essentials grid the
  // window of its space and the windows of the spaces linked to that space.
  std::vector<BrowserWindow*> WindowsShowing(Workspace* ws) const;
  // Closes |tabs| as one batch: reopening restores all of them at once.
  void CloseTabsTogether(const std::vector<Tab*>& tabs);

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
  void NormalizeSpaceOrder();  // positions 0..n-1 in Spaces() order, stored
  void DeletePendingProfileDirectories();
  void FinishProfileDeletions();
  void RememberClosedTab(Tab* tab);
  void CheckPendingWindowCloses();
  // Closes |profile|'s extension bridge when |except| is its last open window
  // (or every bridge when null and no window remains); true while any is alive.
  bool CloseExtensionBridges(Profile* profile, const BrowserWindow* except);
  void MaybeQuit();
  void SleepPolicyTick(uint64_t generation);
  std::vector<WindowRecord> CollectWindowRecords() const;

  std::filesystem::path user_data_dir_;
  Database db_;
  std::unique_ptr<Store> store_;
  ShortcutMap shortcuts_;
  LiveFolders live_folders_;
  unsigned long long shortcut_recording_until_ = 0;
  void LoadShortcuts();
  CefRefPtr<BrowserClient> client_;
  std::unique_ptr<DownloadManager> downloads_;
  std::unique_ptr<PermissionManager> permissions_;
  std::vector<std::unique_ptr<Profile>> profiles_;
  std::vector<std::unique_ptr<Profile>> private_profiles_;
  std::vector<std::unique_ptr<BrowserWindow>> windows_;
  std::map<Profile*, CefRefPtr<ExtensionBridge>> extension_bridges_;
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
