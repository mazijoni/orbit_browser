#pragma once

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "include/cef_browser.h"
#include "persistence/store.h"

namespace orbit {

class BrowserWindow;
class DevToolsClient;
class Profile;
class Workspace;

enum class SecurityLevel { kNone, kSecure, kInsecure, kDangerous, kInternal };

struct FaviconPixels {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> bgra;  // premultiplied BGRA
  uint32_t generation = 0;    // bumped on change so views can cache bitmaps
};

// One sidebar entry. A Tab exists independently of its Chromium browser: an
// unloaded (restored-but-not-yet-shown or discarded) tab keeps its URL, title
// and favicon but owns no CefBrowser and no renderer process.
//
// Threading: UI thread only. Ownership: owned by its Workspace (or, while a
// browser close is in flight, by the Application's closing list).
class Tab {
 public:
  enum class LoadState { kUnloaded, kCreating, kLive, kClosing };

  Tab(Workspace* workspace, TabRecord record);
  ~Tab();
  Tab(const Tab&) = delete;
  Tab& operator=(const Tab&) = delete;

  // Model.
  int64_t id() const { return record_.id; }
  Workspace* workspace() const { return workspace_; }
  void set_workspace(Workspace* workspace) { workspace_ = workspace; }
  Profile* profile() const;
  const TabRecord& record() const { return record_; }
  TabRecord& mutable_record() { MarkDirty(); return record_; }
  bool dirty() const { return dirty_; }
  void ClearDirty() { dirty_ = false; }
  void MarkDirty();
  void set_position(int position) { record_.position = position; }
  void set_group(int64_t group_id) { record_.group_id = group_id; }

  const std::wstring& url() const { return record_.url; }
  std::wstring DisplayTitle() const;
  bool pinned() const { return record_.pinned; }
  // Zen's pinned tab state: the page a pinned tab belongs to.
  const std::wstring& pinned_url() const { return record_.pinned_url; }
  void SetPinnedUrl(const std::wstring& url);
  // True when a pinned tab has navigated away from its pinned page.
  bool pin_changed() const;
  // The favicon of the pinned page, captured while the tab showed it.
  const FaviconPixels& pinned_favicon() const { return pinned_favicon_; }
  // Back to the pinned page with a fresh history (Zen #resetTabToStoredState):
  // a live browser is closed and recreated at the pinned URL.
  void ResetToPinned();
  // Zen's "Change Label…": a user label shown instead of the page title;
  // empty restores the page title.
  const std::wstring& custom_title() const { return record_.custom_title; }
  void SetCustomTitle(const std::wstring& title);
  bool is_protected() const { return record_.is_protected; }

  // Live state (reflects real browser events).
  LoadState load_state() const { return load_state_; }
  bool has_browser() const { return browser_ != nullptr; }
  bool is_discarded() const { return discarded_; }
  bool loading() const { return loading_; }
  double progress() const { return progress_; }
  bool can_go_back() const { return can_go_back_; }
  bool can_go_forward() const { return can_go_forward_; }
  SecurityLevel security() const { return security_; }
  bool crashed() const { return crashed_; }
  bool media_capture() const { return media_capture_; }
  const FaviconPixels& favicon() const { return favicon_; }
  int64_t last_active_ms() const { return record_.last_active; }
  // Firefox's tab.owner: the tab this one was opened from in the foreground.
  // Cleared when the user switches away from this tab (non-owning; the window
  // clears it when the owner goes away).
  Tab* owner() const { return owner_; }
  // Zen's hidden empty tab (zen-empty-tab): the new-tab page that stands in
  // for an empty space. Not listed in the sidebar; dropped once another tab
  // opens, and an ordinary tab once it navigates somewhere.
  bool empty_placeholder() const { return empty_placeholder_; }
  void set_empty_placeholder(bool value) { empty_placeholder_ = value; }
  void set_owner(Tab* owner) { owner_ = owner; }
  void TouchActive();

  CefRefPtr<CefBrowser> browser() const { return browser_; }
  HWND host() const { return host_; }
  BrowserWindow* window() const { return window_; }
  // The window showing this tab: the hosting window if it has one, else its
  // space's window, else (for an Essential, whose grid has no window) the
  // window of the space that owns the grid.
  BrowserWindow* HostWindow() const;

  // Creates the Chromium browser (if needed) as a hidden child of |window|.
  void EnsureBrowser(BrowserWindow* window);
  // Prepares a host for a popup that Chromium will create for us; fills |info|.
  void PrepareForPopup(BrowserWindow* window, CefWindowInfo& info);
  // Moves the host HWND under another top-level window.
  void Reparent(BrowserWindow* window);
  // Places the host at |bounds_px| with rounded corners of |corner_px|.
  void Show(const RECT& bounds_px, int corner_px);
  void Hide();  // the page and its DevTools

  // DevTools docked beside the page, in a pane of the tab's window (F12,
  // Ctrl+Shift+I, "Inspect" in the page menu). |inspect_at|: the element
  // under that point (view coordinates) is selected.
  void ShowDevTools(BrowserWindow* window, const CefPoint* inspect_at = nullptr);
  void CloseDevTools();
  bool devtools_open() const { return devtools_host_ != nullptr; }
  bool devtools_alive() const { return devtools_browser_ != nullptr || devtools_creating_; }
  CefRefPtr<CefBrowser> devtools_browser() const { return devtools_browser_; }
  BrowserWindow* devtools_window() const { return devtools_window_; }
  void ShowDevToolsPane(const RECT& bounds_px, int corner_px);
  void HideDevToolsPane();
  void OnDevToolsCreated(CefRefPtr<CefBrowser> browser);
  void OnDevToolsClosing();  // DoClose: its host goes
  void OnDevToolsClosed();
  void Focus();

  void Navigate(const std::wstring& url);
  void GoBack();
  void GoForward();
  void Reload(bool ignore_cache);
  void Stop();
  void Zoom(cef_zoom_command_t command);
  void SetMuted(bool muted);
  bool muted() const { return muted_; }

  // User-initiated close; honours beforeunload. The tab leaves the sidebar
  // when Chromium confirms the close (DoClose).
  void RequestClose();
  bool close_requested() const { return close_requested_; }
  // Marks the tab as part of a multi-tab close so it is reopened together.
  void set_close_batch(int64_t batch) { close_batch_ = batch; }
  int64_t close_batch() const { return close_batch_; }
  // Frees the browser and renderer but keeps the tab (discard / window close).
  // |mark_discarded| shows the tab as discarded in the UI.
  void Unload(bool mark_discarded);
  // The tab now belongs to another profile: a browser keeps the storage it
  // was created with, so it closes; a tab on screen loads again at once (in
  // the new profile's storage), the others when selected.
  void UnloadForProfileChange();

  // Callbacks from BrowserClient.
  void OnAfterCreated(CefRefPtr<CefBrowser> browser);
  // Returns true if the tab should be removed from its workspace.
  bool OnDoClose();
  void OnBeforeClose();
  void OnAddressChange(const std::wstring& url);
  void OnTitleChange(const std::wstring& title);
  void OnLoadingStateChange(bool loading, bool can_back, bool can_forward);
  void OnLoadingProgress(double progress);
  void OnFaviconUrl(const std::wstring& url);
  void OnFaviconImage(int width, int height, std::vector<uint8_t> bgra);
  void OnRenderProcessTerminated();
  void OnMediaAccessChange(bool video, bool audio);
  void OnLoadError(int error_code);
  // Last main-frame network error (net::Error, e.g. -201 for an expired
  // certificate); 0 after a successful load.
  int last_error() const { return last_error_; }
  void UpdateSecurityState();

  // Sleep policy support: asks every frame's renderer whether media is
  // playing; replies arrive through OnMediaReply.
  void QueryMediaPlayback();
  void OnMediaReply(bool playing);
  ULONGLONG media_query_sent_at() const { return media_query_sent_at_; }
  bool media_reported_playing() const { return media_reported_playing_; }
  void ResetMediaQuery() { media_query_sent_at_ = 0; media_reported_playing_ = false; }

  // Media card (ZenMediaController): what the tab's frames report playing.
  struct MediaState {
    enum class State { kNone, kPlaying, kPaused };
    State state = State::kNone;
    std::wstring title;
    std::wstring artist;
    double time = 0.0;
    double duration = 0.0;
    ULONGLONG updated_at = 0;   // GetTickCount64 of the report
    ULONGLONG started_at = 0;   // when it last started playing
  };
  void OnMediaState(const std::string& frame_id, const MediaState& state);
  // The frame playing (else paused) most recently.
  MediaState media() const;
  void ForgetMedia() { media_frames_.clear(); }

  static Tab* FromHost(HWND host);
  // Hosts waiting for their Chrome-style widget to finish closing; a window
  // must not be destroyed while any remain (its child widget would crash).
  static int PendingHostDestroys();
  // Destroys |host| (a parent window of a Chrome-style browser) once that
  // browser's widget inside it has gone. Counted in PendingHostDestroys().
  static void DestroyHostWhenGone(HWND host);
  // Tells the page which colour scheme to prefer (Orbit's theme).
  void ApplyWebColorScheme();

 private:
  void CreateHost(BrowserWindow* window);
  void DestroyHost();
  void NotifyChanged();
  static LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

  Workspace* workspace_;
  TabRecord record_;
  bool dirty_ = false;

  BrowserWindow* window_ = nullptr;
  CefRefPtr<CefRegistration> color_scheme_session_;
  HWND host_ = nullptr;
  SIZE region_size_{};  // size the host's rounded window region was made for
  int region_corner_ = -1;
  CefRefPtr<CefBrowser> browser_;
  LoadState load_state_ = LoadState::kUnloaded;
  bool pending_unload_ = false;
  bool unloading_ = false;
  bool discarded_ = false;
  bool close_requested_ = false;
  int64_t close_batch_ = 0;
  Tab* owner_ = nullptr;
  bool empty_placeholder_ = false;
  FaviconPixels pinned_favicon_;
  bool recreate_after_close_ = false;
  HWND devtools_host_ = nullptr;
  BrowserWindow* devtools_window_ = nullptr;
  CefRefPtr<CefBrowser> devtools_browser_;
  CefRefPtr<DevToolsClient> devtools_client_;
  bool devtools_creating_ = false;
  SIZE devtools_region_size_{};
  bool reload_if_shown_ = false;  // UnloadForProfileChange

  bool loading_ = false;
  double progress_ = 0.0;
  bool can_go_back_ = false;
  bool can_go_forward_ = false;
  bool crashed_ = false;
  bool media_capture_ = false;
  bool muted_ = false;
  int last_error_ = 0;
  ULONGLONG media_query_sent_at_ = 0;
  std::map<std::string, MediaState> media_frames_;
  bool media_reported_playing_ = false;
  SecurityLevel security_ = SecurityLevel::kNone;
  std::wstring title_live_;
  FaviconPixels favicon_;
  std::wstring pending_url_;  // URL to load once a newly created browser exists
};

}  // namespace orbit
