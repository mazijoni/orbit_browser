#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "include/cef_browser.h"
#include "persistence/store.h"

namespace orbit {

class BrowserWindow;
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
  void TouchActive();

  CefRefPtr<CefBrowser> browser() const { return browser_; }
  HWND host() const { return host_; }
  BrowserWindow* window() const { return window_; }

  // Creates the Chromium browser (if needed) as a hidden child of |window|.
  void EnsureBrowser(BrowserWindow* window);
  // Prepares a host for a popup that Chromium will create for us; fills |info|.
  void PrepareForPopup(BrowserWindow* window, CefWindowInfo& info);
  // Moves the host HWND under another top-level window.
  void Reparent(BrowserWindow* window);
  // Places the host at |bounds_px| with rounded corners of |corner_px|.
  void Show(const RECT& bounds_px, int corner_px);
  void Hide();
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
  // Frees the browser and renderer but keeps the tab (discard / window close).
  // |mark_discarded| shows the tab as discarded in the UI.
  void Unload(bool mark_discarded);

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

  static Tab* FromHost(HWND host);

 private:
  void CreateHost(BrowserWindow* window);
  void DestroyHost();
  void NotifyChanged();
  static LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

  Workspace* workspace_;
  TabRecord record_;
  bool dirty_ = false;

  BrowserWindow* window_ = nullptr;
  HWND host_ = nullptr;
  SIZE region_size_{};  // size the host's rounded window region was made for
  int region_corner_ = -1;
  CefRefPtr<CefBrowser> browser_;
  LoadState load_state_ = LoadState::kUnloaded;
  bool pending_unload_ = false;
  bool unloading_ = false;
  bool discarded_ = false;
  bool close_requested_ = false;

  bool loading_ = false;
  double progress_ = 0.0;
  bool can_go_back_ = false;
  bool can_go_forward_ = false;
  bool crashed_ = false;
  bool media_capture_ = false;
  bool muted_ = false;
  int last_error_ = 0;
  ULONGLONG media_query_sent_at_ = 0;
  bool media_reported_playing_ = false;
  SecurityLevel security_ = SecurityLevel::kNone;
  std::wstring title_live_;
  FaviconPixels favicon_;
  std::wstring pending_url_;  // URL to load once a newly created browser exists
};

}  // namespace orbit
