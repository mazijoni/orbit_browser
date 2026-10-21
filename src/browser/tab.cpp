#include "browser/tab.h"

#include "app/application.h"
#include "browser/browser_client.h"
#include "browser/profile.h"
#include "core/log.h"
#include "core/strings.h"
#include "include/base/cef_callback.h"
#include "include/cef_request_context.h"
#include "include/wrapper/cef_closure_task.h"
#include "ui/browser_window.h"

namespace orbit {
namespace {

constexpr wchar_t kHostClass[] = L"OrbitTabHost";

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

Tab::~Tab() { DestroyHost(); }

Profile* Tab::profile() const { return workspace_ ? workspace_->profile() : nullptr; }

void Tab::MarkDirty() {
  dirty_ = true;
  Application::Get()->ScheduleSave();
}

std::wstring Tab::DisplayTitle() const {
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
  return reinterpret_cast<Tab*>(::GetWindowLongPtrW(host, GWLP_USERDATA));
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
      Tab* tab = reinterpret_cast<Tab*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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

void Tab::CreateHost(BrowserWindow* window) {
  RegisterHostClass();
  const RECT rc = window->ContentRectPx();
  host_ = ::CreateWindowExW(0, kHostClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                            rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                            window->hwnd(), nullptr, ::GetModuleHandleW(nullptr), nullptr);
  ::SetWindowLongPtrW(host_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  ::SetWindowLongPtrW(host_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Tab::HostProc));
  window_ = window;
}

void Tab::DestroyHost() {
  if (host_) {
    ::SetWindowLongPtrW(host_, GWLP_USERDATA, 0);
    ::DestroyWindow(host_);
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
  info.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
  CefBrowserSettings settings;
  const std::wstring url = record_.url.empty() ? Application::kNewTabUrl : record_.url;
  if (!CefBrowserHost::CreateBrowser(info, Application::Get()->client(), url, settings, nullptr,
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
  info.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
  load_state_ = LoadState::kCreating;
}

void Tab::Reparent(BrowserWindow* window) {
  if (window_ == window) return;
  window_ = window;
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

void Tab::RequestClose() {
  close_requested_ = true;
  switch (load_state_) {
    case LoadState::kLive:
      browser_->GetHost()->CloseBrowser(false);
      break;
    case LoadState::kCreating:
      pending_unload_ = true;  // closed in OnAfterCreated, then removed in OnDoClose
      break;
    case LoadState::kClosing:
      break;
    case LoadState::kUnloaded:
      Application::Get()->RemoveTab(this, /*remember=*/true);  // deletes |this|
      break;
  }
}

void Tab::Unload(bool mark_discarded) {
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

void Tab::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  browser_ = browser;
  load_state_ = LoadState::kLive;
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
  // Tear down the host asynchronously: destroying it here makes Chromium run
  // OnBeforeClose re-entrantly, before the caller has finished processing
  // DoClose (which could delete this Tab underneath it).
  if (host_) {
    HWND host = host_;
    host_ = nullptr;
    ::ShowWindow(host, SW_HIDE);
    ::SetWindowLongPtrW(host, GWLP_USERDATA, 0);
    CefPostTask(TID_UI, base::BindOnce(
                            [](HWND h) {
                              if (::IsWindow(h)) ::DestroyWindow(h);
                            },
                            host));
  }
  return close_requested_ && !unloading_;
}

void Tab::OnBeforeClose() {
  browser_ = nullptr;
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
      scheme == L"devtools") {
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

void Tab::OnMediaReply(bool playing) {
  if (playing) media_reported_playing_ = true;
}

void Tab::NotifyChanged() {
  if (BrowserWindow* w = workspace_ ? workspace_->window() : nullptr) w->OnTabChanged(this);
}

}  // namespace orbit
