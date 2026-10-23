#include "browser/tab.h"

#include "app/application.h"
#include "browser/browser_client.h"
#include "browser/profile.h"
#include "core/log.h"
#include "include/cef_devtools_message_observer.h"
#include "core/strings.h"
#include "core/url_fixup.h"
#include "include/base/cef_callback.h"
#include "include/cef_request_context.h"
#include "include/wrapper/cef_closure_task.h"
#include "ui/browser_window.h"

namespace orbit {
namespace {

constexpr wchar_t kHostClass[] = L"OrbitTabHost";
// A window property, not GWLP_USERDATA: Chromium reads the parent's user data.
constexpr wchar_t kHostProp[] = L"OrbitTab";
// Chrome style gives extensions (Alloy has no extension system).
constexpr cef_runtime_style_t kTabRuntimeStyle = CEF_RUNTIME_STYLE_CHROME;
constexpr wchar_t kDevToolsHostClass[] = L"OrbitDevToolsHost";

LRESULT CALLBACK DevToolsHostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_SIZE) {
    if (HWND child = ::GetWindow(hwnd, GW_CHILD)) {
      ::SetWindowPos(child, nullptr, 0, 0, LOWORD(lp), HIWORD(lp), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return 0;
  }
  if (msg == WM_ERASEBKGND) return 1;
  // CEF closes DevTools by posting WM_CLOSE to their top-level window (this
  // host, which is why it is an owned popup and not a child of the browser
  // window); the tab destroys the host once DevTools have closed.
  if (msg == WM_CLOSE) {
    ::ShowWindow(hwnd, SW_HIDE);
    return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterDevToolsHostClass() {
  static bool registered = false;
  if (registered) return;
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = &DevToolsHostProc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
  wc.lpszClassName = kDevToolsHostClass;
  ::RegisterClassExW(&wc);
  registered = true;
}

void RegisterHostClass() {
  static bool registered = false;
  if (registered) return;
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = DefWindowProcW;  // replaced per-instance in CreateHost
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kHostClass;
  ::RegisterClassExW(&wc);
  registered = true;
}

// Certificate status bits that make a connection untrustworthy; revocation
// "unable to check" bits are informational in Chromium and excluded.
constexpr uint32_t kCertErrorMask =
    0xFFFFu & ~static_cast<uint32_t>(CERT_STATUS_NO_REVOCATION_MECHANISM |
                                     CERT_STATUS_UNABLE_TO_CHECK_REVOCATION);

class FaviconCallback : public CefDownloadImageCallback {
 public:
  explicit FaviconCallback(int browser_id) : browser_id_(browser_id) {}
  void OnDownloadImageFinished(const CefString& image_url, int http_status_code,
                               CefRefPtr<CefImage> image) override {
    if (!image || image->IsEmpty()) return;
    Tab* tab = Application::Get()->client()->FindTab(browser_id_);
    if (!tab || tab->record().favicon_url != image_url.ToWString()) return;
    int w = 0, h = 0;
    CefRefPtr<CefBinaryValue> bits =
        image->GetAsBitmap(2.0f, CEF_COLOR_TYPE_BGRA_8888, CEF_ALPHA_TYPE_PREMULTIPLIED, w, h);
    if (!bits || w <= 0 || h <= 0) return;
    std::vector<uint8_t> pixels(bits->GetSize());
    bits->GetData(pixels.data(), pixels.size(), 0);
    tab->OnFaviconImage(w, h, std::move(pixels));
  }

 private:
  const int browser_id_;
  IMPLEMENT_REFCOUNTING(FaviconCallback);
};

}  // namespace

Tab::Tab(Workspace* workspace, TabRecord record)
    : workspace_(workspace), record_(std::move(record)) {}

// The DevTools browser of one tab. It is not a tab: BrowserClient would close
// it, so it has a client of its own that reports to the tab.
class DevToolsClient : public CefClient, public CefLifeSpanHandler, public CefKeyboardHandler {
 public:
  explicit DevToolsClient(Tab* tab) : tab_(tab) {}
  void Detach() { tab_ = nullptr; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    if (tab_) {
      tab_->OnDevToolsCreated(browser);
    } else {
      browser->GetHost()->CloseBrowser(true);
    }
  }
  bool DoClose(CefRefPtr<CefBrowser>) override {
    if (tab_) tab_->OnDevToolsClosing();
    return true;  // never let CEF close the top-level window
  }
  void OnBeforeClose(CefRefPtr<CefBrowser>) override {
    if (tab_) tab_->OnDevToolsClosed();
  }
  bool OnPreKeyEvent(CefRefPtr<CefBrowser>, const CefKeyEvent& event, CefEventHandle, bool*) override {
    // F12 and Ctrl+Shift+I close it from inside, as in Chrome.
    if (!tab_ || event.type != KEYEVENT_RAWKEYDOWN) return false;
    const bool ctrl = (event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0;
    const bool shift = (event.modifiers & EVENTFLAG_SHIFT_DOWN) != 0;
    const bool alt = (event.modifiers & EVENTFLAG_ALT_DOWN) != 0;
    const bool toggle = (event.windows_key_code == VK_F12 && !ctrl && !shift && !alt) ||
                        (event.windows_key_code == 'I' && ctrl && shift && !alt);
    if (!toggle) return false;
    tab_->CloseDevTools();
    return true;
  }

 private:
  Tab* tab_;
  IMPLEMENT_REFCOUNTING(DevToolsClient);
};

Tab::~Tab() {
  if (devtools_client_) devtools_client_->Detach();
  DestroyHost();
  if (devtools_host_) ::DestroyWindow(devtools_host_);
}

void Tab::ShowDevTools(BrowserWindow* window, const CefPoint* inspect_at) {
  if (!browser_ || !window) return;
  if (!devtools_host_) {
    RegisterDevToolsHostClass();
    // An owned popup over the window's page area (see DevToolsHostProc).
    devtools_host_ = ::CreateWindowExW(WS_EX_TOOLWINDOW, kDevToolsHostClass, L"DevTools",
                                       WS_POPUP | WS_CLIPCHILDREN, 0, 0, 400, 300, window->hwnd(), nullptr,
                                       ::GetModuleHandleW(nullptr), nullptr);
    devtools_window_ = window;
    devtools_region_size_ = {};
  }
  if (!devtools_client_) devtools_client_ = new DevToolsClient(this);
  RECT rc;
  ::GetClientRect(devtools_host_, &rc);
  CefWindowInfo info;
  info.SetAsChild(devtools_host_, CefRect(0, 0, rc.right, rc.bottom));
  info.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
  if (!devtools_browser_) devtools_creating_ = true;
  browser_->GetHost()->ShowDevTools(info, devtools_client_, CefBrowserSettings(),
                                    inspect_at ? *inspect_at : CefPoint());
}

void Tab::CloseDevTools() {
  if (browser_ && devtools_alive()) browser_->GetHost()->CloseDevTools();
  if (devtools_host_) ::ShowWindow(devtools_host_, SW_HIDE);
  BrowserWindow* w = devtools_window_;
  if (!devtools_alive()) OnDevToolsClosed();
  if (w) w->OnDevToolsChanged(this);
}

void Tab::ShowDevToolsPane(const RECT& client, int corner_px) {
  if (!devtools_host_ || !devtools_window_) return;
  RECT r = client;  // the window's client coordinates, to the screen
  ::MapWindowPoints(devtools_window_->hwnd(), nullptr, reinterpret_cast<POINT*>(&r), 2);
  const SIZE size{r.right - r.left, r.bottom - r.top};
  if (size.cx != devtools_region_size_.cx || size.cy != devtools_region_size_.cy) {
    devtools_region_size_ = size;
    HRGN region = corner_px > 0 ? ::CreateRoundRectRgn(0, 0, size.cx + 1, size.cy + 1, corner_px * 2, corner_px * 2)
                                : nullptr;
    ::SetWindowRgn(devtools_host_, region, ::IsWindowVisible(devtools_host_));
  }
  ::SetWindowPos(devtools_host_, nullptr, r.left, r.top, size.cx, size.cy,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOZORDER);
}

void Tab::HideDevToolsPane() {
  if (devtools_host_) ::ShowWindow(devtools_host_, SW_HIDE);
}

void Tab::OnDevToolsCreated(CefRefPtr<CefBrowser> browser) {
  devtools_creating_ = false;
  devtools_browser_ = browser;
  if (devtools_host_) {
    if (HWND child = browser->GetHost()->GetWindowHandle()) {
      RECT rc;
      ::GetClientRect(devtools_host_, &rc);
      ::SetWindowPos(child, nullptr, 0, 0, rc.right, rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }
  if (devtools_window_) devtools_window_->OnDevToolsChanged(this);
}

void Tab::OnDevToolsClosing() {
  // As the page's host: hidden now, destroyed outside the CEF callback.
  if (HWND host = devtools_host_) {
    devtools_host_ = nullptr;
    ::ShowWindow(host, SW_HIDE);
    CefPostTask(TID_UI, base::BindOnce(
                            [](HWND h) {
                              if (::IsWindow(h)) ::DestroyWindow(h);
                            },
                            host));
  }
}

void Tab::OnDevToolsClosed() {
  devtools_browser_ = nullptr;
  devtools_creating_ = false;
  OnDevToolsClosing();
  BrowserWindow* w = devtools_window_;
  devtools_window_ = nullptr;
  if (w) {
    w->OnDevToolsChanged(this);
    Application::Get()->OnWindowBrowserClosed();  // a closing window may wait for it
  }
}

void Tab::SetPinnedUrl(const std::wstring& url) {
  if (record_.pinned_url == url) return;
  record_.pinned_url = url;
  pinned_favicon_ = {};
  if (!url.empty() && PinnedUrlMatches(record_.url, url)) pinned_favicon_ = favicon_;
  MarkDirty();
  NotifyChanged();
}

bool Tab::pin_changed() const {
  return record_.pinned && !record_.pinned_url.empty() &&
         !PinnedUrlMatches(record_.url, record_.pinned_url);
}

void Tab::ResetToPinned() {
  if (!record_.pinned || record_.pinned_url.empty()) return;
  record_.url = record_.pinned_url;
  favicon_ = pinned_favicon_;
  ++favicon_.generation;
  MarkDirty();
  if (load_state_ == LoadState::kLive || load_state_ == LoadState::kCreating) {
    // CEF cannot rewrite session history; a new browser starts with one entry.
    recreate_after_close_ = true;
    Unload(/*mark_discarded=*/false);
  }
  NotifyChanged();
}

Profile* Tab::profile() const { return workspace_ ? workspace_->profile() : nullptr; }

void Tab::MarkDirty() {
  dirty_ = true;
  Application::Get()->ScheduleSave();
}

void Tab::SetCustomTitle(const std::wstring& title) {
  if (record_.custom_title == title) return;
  record_.custom_title = title;
  MarkDirty();
  NotifyChanged();
}

std::wstring Tab::DisplayTitle() const {
  if (!record_.custom_title.empty()) return record_.custom_title;
  if (!record_.title.empty()) return record_.title;
  if (record_.url.empty() || record_.url == Application::kNewTabUrl) return L"New Tab";
  return record_.url;
}

void Tab::TouchActive() {
  record_.last_active = NowUnixMillis();
  MarkDirty();
}

Tab* Tab::FromHost(HWND host) {
  wchar_t cls[32] = {};
  if (!host || !::GetClassNameW(host, cls, 32) || std::wstring_view(cls) != kHostClass) {
    return nullptr;
  }
  return reinterpret_cast<Tab*>(::GetPropW(host, kHostProp));
}

LRESULT CALLBACK Tab::HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SIZE: {
      // Keep the Chromium child window filling the host.
      if (HWND child = ::GetWindow(hwnd, GW_CHILD)) {
        ::SetWindowPos(child, nullptr, 0, 0, LOWORD(lp), HIWORD(lp),
                       SWP_NOZORDER | SWP_NOACTIVATE);
      }
      return 0;
    }
    case WM_ERASEBKGND: {
      Tab* tab = reinterpret_cast<Tab*>(::GetPropW(hwnd, kHostProp));
      if (tab && tab->window_) {
        RECT rc;
        ::GetClientRect(hwnd, &rc);
        HBRUSH brush = ::CreateSolidBrush(ui::ToColorRef(tab->window_->palette().content_bg));
        ::FillRect(reinterpret_cast<HDC>(wp), &rc, brush);
        ::DeleteObject(brush);
      }
      return 1;
    }
    default:
      return ::DefWindowProcW(hwnd, msg, wp, lp);
  }
}

BrowserWindow* Tab::HostWindow() const {
  if (window_) return window_;
  if (!workspace_) return nullptr;
  if (BrowserWindow* w = workspace_->window()) return w;
  if (workspace_->is_essentials() && workspace_->owner()) return workspace_->owner()->window();
  return nullptr;
}

void Tab::CreateHost(BrowserWindow* window) {
  RegisterHostClass();
  const RECT rc = window->ContentRectPx();
  host_ = ::CreateWindowExW(0, kHostClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                            rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                            window->hwnd(), nullptr, ::GetModuleHandleW(nullptr), nullptr);
  ::SetPropW(host_, kHostProp, reinterpret_cast<HANDLE>(this));
  ::SetWindowLongPtrW(host_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Tab::HostProc));
  window_ = window;
}

namespace {
int g_pending_host_destroys = 0;

// Destroys |host| once the Chrome-style widget inside has gone (destroying its
// parent earlier crashes in libcef); gives up waiting after about 2 s.
void DestroyHostWhenEmpty(HWND host, int tries) {
  if (::IsWindow(host) && ::GetWindow(host, GW_CHILD) && tries < 40) {
    CefPostDelayedTask(TID_UI, base::BindOnce(&DestroyHostWhenEmpty, host, tries + 1), 50);
    return;
  }
  if (::IsWindow(host)) ::DestroyWindow(host);
  --g_pending_host_destroys;
  if (Application* app = Application::Get()) app->OnWindowBrowserClosed();
}
}  // namespace

int Tab::PendingHostDestroys() { return g_pending_host_destroys; }

void Tab::DestroyHostWhenGone(HWND host) {
  ++g_pending_host_destroys;
  CefPostTask(TID_UI, base::BindOnce(&DestroyHostWhenEmpty, host, 0));
}

void Tab::DestroyHost() {
  if (host_) {
    ::RemovePropW(host_, kHostProp);
    ::ShowWindow(host_, SW_HIDE);
    DestroyHostWhenGone(host_);
    host_ = nullptr;
  }
}

void Tab::EnsureBrowser(BrowserWindow* window) {
  if (load_state_ == LoadState::kLive || load_state_ == LoadState::kCreating) {
    if (window_ != window) Reparent(window);
    pending_unload_ = false;
    return;
  }
  if (load_state_ == LoadState::kClosing) return;  // will be recreated on next activation
  if (!host_) CreateHost(window);
  RECT rc;
  ::GetClientRect(host_, &rc);
  CefWindowInfo info;
  info.SetAsChild(host_, CefRect(0, 0, rc.right, rc.bottom));
  info.runtime_style = kTabRuntimeStyle;
  CefBrowserSettings settings;
  const std::wstring url = record_.url.empty() ? Application::kNewTabUrl : record_.url;
  if (!CefBrowserHost::CreateBrowser(info, Application::Get()->client(), url, settings,
                                     Application::Get()->RendererInfo(),
                                     profile()->request_context())) {
    ORBIT_LOG_ERROR("CreateBrowser failed for tab %lld", record_.id);
    return;
  }
  load_state_ = LoadState::kCreating;
  discarded_ = false;
  crashed_ = false;
  NotifyChanged();
}

void Tab::PrepareForPopup(BrowserWindow* window, CefWindowInfo& info) {
  if (!host_) CreateHost(window);
  RECT rc;
  ::GetClientRect(host_, &rc);
  info.SetAsChild(host_, CefRect(0, 0, rc.right, rc.bottom));
  info.runtime_style = kTabRuntimeStyle;
  load_state_ = LoadState::kCreating;
}

void Tab::Reparent(BrowserWindow* window) {
  if (window_ == window) return;
  BrowserWindow* old = window_;
  window_ = window;
  if (devtools_host_) {
    ::ShowWindow(devtools_host_, SW_HIDE);
    ::SetWindowLongPtrW(devtools_host_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(window->hwnd()));
    devtools_window_ = window;
  }
  // An Essential can be shown by the window of its space and by the windows
  // of the spaces linked to it; the window that loses it falls back to its
  // own space.
  if (old && old != window) old->OnTabMovedAway(this);
  if (host_) {
    ::ShowWindow(host_, SW_HIDE);
    ::SetParent(host_, window->hwnd());
  }
}

void Tab::Show(const RECT& r, int corner_px) {
  if (!host_) return;
  const SIZE size{r.right - r.left, r.bottom - r.top};
  if (size.cx != region_size_.cx || size.cy != region_size_.cy || corner_px != region_corner_) {
    // The content card's rounded corners. Chromium's window is a child of the
    // host, so the host's region clips it too.
    region_size_ = size;
    region_corner_ = corner_px;
    HRGN region = corner_px > 0 ? ::CreateRoundRectRgn(0, 0, size.cx + 1, size.cy + 1,
                                                       corner_px * 2, corner_px * 2)
                                : nullptr;
    ::SetWindowRgn(host_, region, ::IsWindowVisible(host_));  // takes ownership
  }
  ::SetWindowPos(host_, HWND_TOP, r.left, r.top, size.cx, size.cy,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
}

void Tab::Hide() {
  if (host_) ::ShowWindow(host_, SW_HIDE);
  HideDevToolsPane();
}

void Tab::Focus() {
  if (browser_) browser_->GetHost()->SetFocus(true);
}

void Tab::Navigate(const std::wstring& url) {
  record_.url = url;
  MarkDirty();
  if (browser_) {
    browser_->GetMainFrame()->LoadURL(url);
  } else if (load_state_ == LoadState::kCreating) {
    pending_url_ = url;
  } else if (BrowserWindow* w = workspace_ ? workspace_->window() : nullptr) {
    EnsureBrowser(w);
  }
  NotifyChanged();
}

void Tab::GoBack() {
  if (browser_) browser_->GoBack();
}
void Tab::GoForward() {
  if (browser_) browser_->GoForward();
}
void Tab::Reload(bool ignore_cache) {
  if (!browser_) {
    if (BrowserWindow* w = workspace_ ? workspace_->window() : nullptr) EnsureBrowser(w);
    return;
  }
  if (ignore_cache) {
    browser_->ReloadIgnoreCache();
  } else {
    browser_->Reload();
  }
}
void Tab::Stop() {
  if (browser_) browser_->StopLoad();
}
void Tab::Zoom(cef_zoom_command_t command) {
  if (browser_ && browser_->GetHost()->CanZoom(command)) browser_->GetHost()->Zoom(command);
}

void Tab::SetMuted(bool muted) {
  muted_ = muted;
  if (browser_) browser_->GetHost()->SetAudioMuted(muted);
  NotifyChanged();
}

void Tab::NoteClosedByPage() {
  if (close_requested_ || unloading_ || recreate_after_close_ || reload_if_shown_ ||
      load_state_ == LoadState::kClosing) {
    return;  // Orbit asked for this close itself
  }
  close_requested_ = true;
}

void Tab::RequestClose() {
  close_requested_ = true;
  if (devtools_alive() && browser_) browser_->GetHost()->CloseDevTools();
  switch (load_state_) {
    case LoadState::kLive:
      // Chrome style never calls DoClose, where the tab used to leave its
      // space; do that now so it is gone from the session at once, then close.
      if (OnDoClose()) Application::Get()->DetachClosingTab(this);
      browser_->GetHost()->CloseBrowser(true);
      break;
    case LoadState::kCreating:
      pending_unload_ = true;  // closed in OnAfterCreated, then removed in OnBeforeClose
      break;
    case LoadState::kClosing:
      break;
    case LoadState::kUnloaded:
      Application::Get()->RemoveTab(this, /*remember=*/true);  // deletes |this|
      break;
  }
}

void Tab::Unload(bool mark_discarded) {
  if (devtools_alive() && browser_) browser_->GetHost()->CloseDevTools();
  switch (load_state_) {
    case LoadState::kLive:
      unloading_ = true;
      discarded_ = mark_discarded;
      load_state_ = LoadState::kClosing;
      browser_->GetHost()->CloseBrowser(true);
      break;
    case LoadState::kCreating:
      pending_unload_ = true;
      discarded_ = mark_discarded;
      break;
    case LoadState::kClosing:
    case LoadState::kUnloaded:
      break;
  }
  NotifyChanged();
}

void Tab::UnloadForProfileChange() {
  const bool closing_for_unload = load_state_ == LoadState::kClosing && !close_requested_;
  if (load_state_ == LoadState::kLive || load_state_ == LoadState::kCreating || closing_for_unload) {
    reload_if_shown_ = true;
    Unload(/*mark_discarded=*/false);
  }
}

namespace {
// Keeps a DevTools session attached so an emulation setting outlives the call
// that made it. Receives nothing of interest.
class KeepDevToolsSession : public CefDevToolsMessageObserver {
 public:
  IMPLEMENT_REFCOUNTING(KeepDevToolsSession);
};
}  // namespace

void Tab::ApplyWebColorScheme() {
  if (!browser_) return;
  if (!color_scheme_session_) {
    color_scheme_session_ = browser_->GetHost()->AddDevToolsMessageObserver(new KeepDevToolsSession());
  }
  CefRefPtr<CefDictionaryValue> feature = CefDictionaryValue::Create();
  feature->SetString("name", "prefers-color-scheme");
  feature->SetString("value", Application::Get()->dark_mode() ? "dark" : "light");
  CefRefPtr<CefListValue> features = CefListValue::Create();
  features->SetDictionary(0, feature);
  CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
  params->SetList("features", features);
  browser_->GetHost()->ExecuteDevToolsMethod(0, "Emulation.setEmulatedMedia", params);
}

void Tab::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  browser_ = browser;
  load_state_ = LoadState::kLive;
  ApplyWebColorScheme();
  if (host_) {
    if (HWND child = browser->GetHost()->GetWindowHandle()) {
      RECT rc;
      ::GetClientRect(host_, &rc);
      ::SetWindowPos(child, nullptr, 0, 0, rc.right, rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }
  if (pending_unload_) {
      pending_unload_ = false;
    if (close_requested_) {
      browser_->GetHost()->CloseBrowser(true);
    } else {
      Unload(discarded_);
    }
    return;
  }
  if (muted_) browser_->GetHost()->SetAudioMuted(true);
  if (!pending_url_.empty()) {
    browser_->GetMainFrame()->LoadURL(pending_url_);
    pending_url_.clear();
  }
  NotifyChanged();
}

bool Tab::OnDoClose() {
  load_state_ = LoadState::kClosing;
  // Chrome style closes its own widget inside the host; the host is only
  // destroyed (late, see DestroyHost) from OnBeforeClose. Destroying it here,
  // as Alloy style allowed, crashes libcef.
  return close_requested_ && !unloading_;
}

void Tab::OnBeforeClose() {
  if (recreate_after_close_ || reload_if_shown_) {
    // Recreated by the window outside this CEF callback (wParam 1: only if
    // it is on screen there).
    const WPARAM only_if_shown = recreate_after_close_ ? 0 : 1;
    recreate_after_close_ = false;
    reload_if_shown_ = false;
    BrowserWindow* w = HostWindow();
    if (w && !close_requested_) {
      ::PostMessageW(w->hwnd(), BrowserWindow::kRecreateTabMessage, only_if_shown,
                     reinterpret_cast<LPARAM>(this));
    }
  }
  browser_ = nullptr;
  media_frames_.clear();  // nothing plays without a browser
  load_state_ = LoadState::kUnloaded;
  unloading_ = false;
  loading_ = false;
  progress_ = 0.0;
  window_ = nullptr;
  media_capture_ = false;
  DestroyHost();
  NotifyChanged();
}

void Tab::OnAddressChange(const std::wstring& url) {
  if (url != Application::kNewTabUrl) empty_placeholder_ = false;
  if (record_.url != url) {
    record_.url = url;
    MarkDirty();
  }
  UpdateSecurityState();
  NotifyChanged();
}

void Tab::OnTitleChange(const std::wstring& title) {
  // Chromium reports the URL as title while no <title> exists; keep it empty.
  const std::wstring value = (title == record_.url) ? std::wstring() : title;
  if (record_.title != value) {
    record_.title = value;
    MarkDirty();
    if (!loading_ && !value.empty()) Application::Get()->UpdateVisitTitle(this);
  }
  NotifyChanged();
}

void Tab::OnLoadingStateChange(bool loading, bool can_back, bool can_forward) {
  loading_ = loading;
  can_go_back_ = can_back;
  can_go_forward_ = can_forward;
  if (loading) {
    crashed_ = false;
    last_error_ = 0;
  }
  if (!loading) progress_ = 1.0;
  UpdateSecurityState();
  NotifyChanged();
}

void Tab::OnLoadingProgress(double progress) {
  progress_ = progress;
  NotifyChanged();
}

void Tab::OnFaviconUrl(const std::wstring& url) {
  if (url == record_.favicon_url && !favicon_.bgra.empty()) return;  // already have it
  if (url != record_.favicon_url && !favicon_.bgra.empty()) {
    // A different icon (or none): drop the previous page's image so a failed
    // download (e.g. a missing /favicon.ico) shows the default icon instead.
    favicon_.bgra.clear();
    favicon_.width = favicon_.height = 0;
    ++favicon_.generation;
    NotifyChanged();
  }
  record_.favicon_url = url;
  MarkDirty();
  if (browser_ && !url.empty()) {
    browser_->GetHost()->DownloadImage(url, true, 32, false,
                                       new FaviconCallback(browser_->GetIdentifier()));
  }
}

void Tab::OnFaviconImage(int width, int height, std::vector<uint8_t> bgra) {
  favicon_.width = width;
  favicon_.height = height;
  favicon_.bgra = std::move(bgra);
  ++favicon_.generation;
  if (record_.pinned && !pin_changed()) pinned_favicon_ = favicon_;
  NotifyChanged();
}

void Tab::OnRenderProcessTerminated() {
  crashed_ = true;
  loading_ = false;
  NotifyChanged();
}

void Tab::OnLoadError(int error_code) {
  last_error_ = error_code;
  security_ = SecurityLevel::kDangerous;
  NotifyChanged();
}

void Tab::OnMediaAccessChange(bool video, bool audio) {
  media_capture_ = video || audio;
  NotifyChanged();
}

void Tab::UpdateSecurityState() {
  if (last_error_ != 0) return;  // keep the error state until the next load
  const std::wstring scheme = ToLowerAscii(record_.url.substr(0, record_.url.find(L':')));
  SecurityLevel level = SecurityLevel::kNone;
  if (scheme == L"orbit" || scheme == L"about" || scheme == L"chrome" ||
      scheme == L"chrome-extension" || scheme == L"devtools") {
    level = SecurityLevel::kInternal;
  } else if (scheme == L"http") {
    level = SecurityLevel::kInsecure;
  } else if (scheme == L"https" && browser_) {
    level = SecurityLevel::kInsecure;
    CefRefPtr<CefNavigationEntry> entry = browser_->GetHost()->GetVisibleNavigationEntry();
    CefRefPtr<CefSSLStatus> ssl = entry ? entry->GetSSLStatus() : nullptr;
    if (ssl && ssl->IsSecureConnection()) {
      if (static_cast<uint32_t>(ssl->GetCertStatus()) & kCertErrorMask) {
        level = SecurityLevel::kDangerous;
      } else if (ssl->GetContentStatus() & SSL_CONTENT_RAN_INSECURE_CONTENT) {
        level = SecurityLevel::kDangerous;
      } else if (ssl->GetContentStatus() & SSL_CONTENT_DISPLAYED_INSECURE_CONTENT) {
        level = SecurityLevel::kInsecure;
      } else {
        level = SecurityLevel::kSecure;
      }
    }
  }
  security_ = level;
}

void Tab::QueryMediaPlayback() {
  media_query_sent_at_ = ::GetTickCount64();
  media_reported_playing_ = false;
  if (!browser_) return;
  std::vector<CefString> frames;
  browser_->GetFrameIdentifiers(frames);
  for (const auto& id : frames) {
    if (CefRefPtr<CefFrame> frame = browser_->GetFrameByIdentifier(id)) {
      frame->SendProcessMessage(PID_RENDERER, CefProcessMessage::Create("orbit.media-query"));
    }
  }
}

void Tab::OnMediaState(const std::string& frame_id, const MediaState& state) {
  if (state.state == MediaState::State::kNone) {
    media_frames_.erase(frame_id);
  } else {
    MediaState s = state;
    const auto it = media_frames_.find(frame_id);
    const bool was_playing = it != media_frames_.end() && it->second.state == MediaState::State::kPlaying;
    s.started_at = s.state == MediaState::State::kPlaying && !was_playing
                       ? s.updated_at
                       : (it != media_frames_.end() ? it->second.started_at : s.updated_at);
    media_frames_[frame_id] = s;
  }
  NotifyChanged();
}

Tab::MediaState Tab::media() const {
  MediaState best;
  for (const auto& [id, s] : media_frames_) {
    const bool better = best.state == MediaState::State::kNone ||
                        (s.state == MediaState::State::kPlaying && best.state != MediaState::State::kPlaying) ||
                        (s.state == best.state && s.started_at > best.started_at);
    if (better) best = s;
  }
  return best;
}

void Tab::OnMediaReply(bool playing) {
  if (playing) media_reported_playing_ = true;
}

void Tab::NotifyChanged() {
  if (BrowserWindow* w = HostWindow()) w->OnDevToolsChanged(this);
}

}  // namespace orbit
