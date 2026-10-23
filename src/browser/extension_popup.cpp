#include "browser/extension_popup.h"

#include <dwmapi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "app/application.h"
#include "browser/extension_bridge.h"
#include "browser/profile.h"
#include "browser/tab.h"
#include "core/log.h"
#include "core/strings.h"
#include "include/cef_browser.h"
#include "include/base/cef_callback.h"
#include "include/cef_parser.h"
#include "include/wrapper/cef_closure_task.h"
#include "ui/browser_window.h"

namespace orbit {
namespace {

constexpr wchar_t kPopupClass[] = L"OrbitExtensionPopup";
constexpr wchar_t kPopupProp[] = L"OrbitExtPopup";
constexpr char kSizePrefix[] = "ORBIT-POP:";
constexpr int kStartWidth = 300;   // CSS px; grows to the page's content
constexpr int kStartHeight = 80;
constexpr int kMaxWidth = 800;     // Chrome's limits for extension popups
constexpr int kMaxHeight = 600;

CefRefPtr<ExtensionPopup>& Current() {
  static CefRefPtr<ExtensionPopup> current;
  return current;
}

// An Open() that arrived while the previous popup was still closing.
struct PendingOpen {
  BrowserWindow* window = nullptr;
  std::string extension_id;
  std::string popup;
  POINT at{};
  bool valid = false;
};
PendingOpen& Pending() {
  static PendingOpen pending;
  return pending;
}

// Reports the page's size now and twice more while it settles.
constexpr char kMeasureScript[] = R"js((function () {
  const send = () => {
    const d = document.documentElement, b = document.body;
    const w = Math.max(d.scrollWidth, b ? b.scrollWidth : 0, d.offsetWidth);
    const h = Math.max(d.scrollHeight, b ? b.scrollHeight : 0, d.offsetHeight);
    console.log('ORBIT-POP:' + Math.ceil(w) + ',' + Math.ceil(h));
  };
  send();
  setTimeout(send, 250);
  setTimeout(send, 800);
})())js";

}  // namespace

bool ExtensionPopup::IsPopupPath(const std::string& path) {
  if (path.empty() || path.size() > 512 || path.find("..") != std::string::npos) return false;
  if (path.find("://") != std::string::npos || path.find('\\') != std::string::npos) return false;
  return std::all_of(path.begin(), path.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '/' || c == '.' || c == '_' || c == '-' || c == '?' || c == '=' ||
           c == '&' || c == '%' || c == '#' || c == '+';
  });
}

bool ExtensionPopup::Alive() { return Current() != nullptr || Pending().valid; }

std::wstring ExtensionPopup::CurrentTitle() { return Current() ? Current()->title_ : std::wstring(); }
HWND ExtensionPopup::CurrentWindow() { return Current() ? Current()->popup_ : nullptr; }

void ExtensionPopup::CloseCurrent() {
  Pending().valid = false;
  CefRefPtr<ExtensionPopup> popup = Current();
  if (!popup || popup->closing_) return;
  popup->closing_ = true;
  if (popup->browser_) popup->browser_->GetHost()->CloseBrowser(true);
  // A popup still being created is closed in OnAfterCreated.
}

ExtensionPopup::ExtensionPopup(BrowserWindow* window, std::string extension_id)
    : window_(window), extension_id_(std::move(extension_id)) {}

LRESULT CALLBACK ExtensionPopup::PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND: {
      // A page that paints no background of its own shows white, as in Chrome.
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      ::FillRect(reinterpret_cast<HDC>(wp), &rc, static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH)));
      return 1;
    }
    case WM_SIZE:
      if (HWND child = ::GetWindow(hwnd, GW_CHILD)) {
        ::SetWindowPos(child, nullptr, 0, 0, LOWORD(lp), HIWORD(lp), SWP_NOZORDER | SWP_NOACTIVATE);
      }
      return 0;
    case WM_MOUSEACTIVATE:
      return MA_ACTIVATE;
    case WM_ACTIVATE:
      if (LOWORD(wp) == WA_INACTIVE) {
        // Closes when another application (or Orbit's own window) takes the
        // focus; a dialog the extension opens keeps it.
        HWND next = reinterpret_cast<HWND>(lp);
        if (!next || ::GetAncestor(next, GA_ROOTOWNER) != ::GetAncestor(hwnd, GA_ROOT)) {
          CefPostTask(TID_UI, base::BindOnce([]() { ExtensionPopup::CloseCurrent(); }));
        }
      }
      return 0;
    default:
      return ::DefWindowProcW(hwnd, msg, wp, lp);
  }
}

void ExtensionPopup::Open(BrowserWindow* window, const ExtensionInfo& extension, POINT screen_at) {
  if (!window || !IsPopupPath(extension.popup) || !ExtensionBridge::IsExtensionId(extension.id)) return;
  CefRefPtr<ExtensionPopup> existing = Current();
  if (existing) {
    // The button toggles; another extension replaces it once it has closed.
    const bool same = existing->extension_id_ == extension.id;
    CloseCurrent();
    if (same) return;
    PendingOpen& p = Pending();
    p = {window, extension.id, extension.popup, screen_at, true};
    return;
  }

  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &ExtensionPopup::PopupProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kPopupClass;
    ::RegisterClassExW(&wc);
    registered = true;
  }
  CefRefPtr<ExtensionPopup> popup = new ExtensionPopup(window, extension.id);
  const UINT dpi = ::GetDpiForWindow(window->hwnd());
  const int w = ::MulDiv(kStartWidth, dpi, 96);
  const int h = ::MulDiv(kStartHeight, dpi, 96);
  popup->popup_ = ::CreateWindowExW(WS_EX_TOOLWINDOW, kPopupClass, L"", WS_POPUP | WS_CLIPCHILDREN, screen_at.x,
                                    screen_at.y, w, h, window->hwnd(), nullptr, ::GetModuleHandleW(nullptr), nullptr);
  if (!popup->popup_) return;
  ::SetPropW(popup->popup_, kPopupProp, reinterpret_cast<HANDLE>(popup.get()));
  const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
  ::DwmSetWindowAttribute(popup->popup_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
  const BOOL dark = window->palette().dark ? TRUE : FALSE;
  ::DwmSetWindowAttribute(popup->popup_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  ::ShowWindow(popup->popup_, SW_SHOW);

  RECT rc;
  ::GetClientRect(popup->popup_, &rc);
  CefWindowInfo info;
  info.SetAsChild(popup->popup_, CefRect(0, 0, rc.right, rc.bottom));
  info.runtime_style = CEF_RUNTIME_STYLE_CHROME;
  CefBrowserSettings settings;
  std::string path = extension.popup;
  if (path[0] == '/') path.erase(0, 1);
  const std::string url = "chrome-extension://" + extension.id + "/" + path;
  Current() = popup;
  if (!CefBrowserHost::CreateBrowser(info, popup, url, settings, nullptr, window->profile()->request_context())) {
    ORBIT_LOG_ERROR("extension popup: CreateBrowser failed");
    ::DestroyWindow(popup->popup_);
    popup->popup_ = nullptr;
    Current() = nullptr;
  }
}

bool ExtensionPopup::OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, int,
                                   const CefString& target_url, const CefString&,
                                   cef_window_open_disposition_t, bool, const CefPopupFeatures&, CefWindowInfo&,
                                   CefRefPtr<CefClient>&, CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&,
                                   bool*) {
  OnOpenURLFromTab(browser, nullptr, target_url, CEF_WOD_NEW_FOREGROUND_TAB, true);
  return true;  // never a window of its own
}

bool ExtensionPopup::OnOpenURLFromTab(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, const CefString& target_url,
                                      cef_window_open_disposition_t, bool) {
  const std::string url = target_url.ToString();
  const bool ok = url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0 ||
                  url.rfind("chrome-extension://" + extension_id_ + "/", 0) == 0;
  BrowserWindow* window = window_;
  CloseCurrent();
  if (ok && window) window->OpenTab(FromUtf8(url), true);
  return true;
}

void ExtensionPopup::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  browser_ = browser;
  if (closing_) browser->GetHost()->CloseBrowser(true);
}

void ExtensionPopup::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  browser_ = nullptr;
  if (popup_) {
    ::RemovePropW(popup_, kPopupProp);
    ::ShowWindow(popup_, SW_HIDE);
    Tab::DestroyHostWhenGone(popup_);
    popup_ = nullptr;
  }
  CefRefPtr<ExtensionPopup> self(this);
  if (Current().get() == this) Current() = nullptr;
  PendingOpen pending = Pending();
  Pending().valid = false;
  if (pending.valid && pending.window) {
    // Open the one that was waiting.
    for (const auto& w : Application::Get()->windows()) {
      if (w.get() != pending.window || w->closing()) continue;
      if (ExtensionBridge* bridge = Application::Get()->Extensions(w->profile())) {
        for (const ExtensionInfo& e : bridge->extensions()) {
          if (e.id == pending.extension_id) {
            ExtensionInfo copy = e;
            Open(pending.window, copy, pending.at);
          }
        }
      }
    }
  }
  if (Application* app = Application::Get()) app->OnWindowBrowserClosed();
}

void ExtensionPopup::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int) {
  if (!frame->IsMain() || closing_) return;
  frame->ExecuteJavaScript(kMeasureScript, "", 0);
}

bool ExtensionPopup::OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t, const CefString& message,
                                      const CefString&, int) {
  if (!browser_ || !browser->IsSame(browser_)) return false;
  const std::string text = message.ToString();
  if (text.rfind(kSizePrefix, 0) != 0) return false;
  int w = 0, h = 0;
  if (sscanf_s(text.c_str() + sizeof(kSizePrefix) - 1, "%d,%d", &w, &h) == 2) Resize(w, h);
  return true;
}

void ExtensionPopup::Resize(int width_css, int height_css) {
  if (!popup_ || closing_) return;
  const UINT dpi = ::GetDpiForWindow(popup_);
  const int w = ::MulDiv(std::clamp(width_css, 25, kMaxWidth), dpi, 96);
  const int h = ::MulDiv(std::clamp(height_css, 25, kMaxHeight), dpi, 96);
  RECT now;
  ::GetWindowRect(popup_, &now);
  if (now.right - now.left == w && now.bottom - now.top == h) return;
  // Keep it on the screen it is on.
  const HMONITOR monitor = ::MonitorFromWindow(popup_, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{sizeof(mi)};
  ::GetMonitorInfoW(monitor, &mi);
  int x = now.left, y = now.top;
  x = std::max<int>(mi.rcWork.left, std::min<int>(x, mi.rcWork.right - w));
  y = std::max<int>(mi.rcWork.top, std::min<int>(y, mi.rcWork.bottom - h));
  ::SetWindowPos(popup_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

bool ExtensionPopup::OnKeyEvent(CefRefPtr<CefBrowser>, const CefKeyEvent& event, CefEventHandle) {
  if (event.type == KEYEVENT_RAWKEYDOWN && event.windows_key_code == VK_ESCAPE) {
    CloseCurrent();
    return true;
  }
  return false;
}

}  // namespace orbit
