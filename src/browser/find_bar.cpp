#include "browser/find_bar.h"

#include <dwmapi.h>

#include <algorithm>

#include "app/application.h"
#include "browser/profile.h"
#include "browser/tab.h"
#include "core/log.h"
#include "include/base/cef_callback.h"
#include "include/cef_browser.h"
#include "include/wrapper/cef_closure_task.h"
#include "ui/browser_window.h"

namespace orbit {
namespace {

constexpr wchar_t kBarClass[] = L"OrbitFindBar";
constexpr wchar_t kBarProp[] = L"OrbitFindBarWindow";
constexpr int kWidthDip = 420;
constexpr int kHeightDip = 52;
constexpr int kMarginDip = 14;

CefRefPtr<FindBar>& Current() {
  static CefRefPtr<FindBar> current;
  return current;
}

struct SearchState {
  CefRefPtr<CefBrowser> target;  // the page being searched
  std::wstring text;
  int count = 0;
  int active = 0;
  // Chrome style reports the number of matches but not which one is active
  // (ordinal 0); then the bar counts the steps itself.
  bool ordinals_reported = false;
};
SearchState& Search_() {
  static SearchState state;
  return state;
}

void ClearHighlights() {
  SearchState& st = Search_();
  if (st.target) st.target->GetHost()->StopFinding(true);
  st = SearchState{};
}

}  // namespace

bool FindBar::Alive() { return Current() != nullptr; }

bool FindBar::IsOpenFor(const BrowserWindow* window) {
  return Current() && Current()->window_ == window && !Current()->closing_;
}

CefRefPtr<CefBrowser> FindBar::BrowserForTest() { return Current() ? Current()->browser_ : nullptr; }

const std::wstring& FindBar::LastText() { return Search_().text; }

FindBar::State FindBar::CurrentState() {
  State s;
  s.open = Current() && !Current()->closing_;
  s.count = Search_().count;
  s.active = Search_().active;
  return s;
}

void FindBar::OnResult(CefRefPtr<CefBrowser> browser, int identifier, int count, int active) {
  SearchState& st = Search_();
  if (!st.target || !browser->IsSame(st.target)) return;  // CEF numbers the requests itself
  st.count = count;
  if (active > 0) {
    st.active = active;
    st.ordinals_reported = true;
  } else if (count > 0 && st.active == 0) {
    st.active = 1;  // a new search starts on the first match
  }
  if (count == 0) st.active = 0;
  if (st.active > count) st.active = count;
}

LRESULT CALLBACK FindBar::BarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND: {
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      BrowserWindow* window = Current() ? Current()->window_ : nullptr;
      const COLORREF color = window ? ui::ToColorRef(window->palette().content_bg) : RGB(32, 32, 32);
      HBRUSH brush = ::CreateSolidBrush(color);
      ::FillRect(reinterpret_cast<HDC>(wp), &rc, brush);
      ::DeleteObject(brush);
      return 1;
    }
    case WM_SIZE:
      if (HWND child = ::GetWindow(hwnd, GW_CHILD)) {
        ::SetWindowPos(child, nullptr, 0, 0, LOWORD(lp), HIWORD(lp), SWP_NOZORDER | SWP_NOACTIVATE);
      }
      return 0;
    case WM_CLOSE:
      FindBar::Close();
      return 0;
    default:
      return ::DefWindowProcW(hwnd, msg, wp, lp);
  }
}

void FindBar::Place() {
  if (!popup_ || !window_) return;
  const RECT content = window_->ContentRectPx();
  const UINT dpi = ::GetDpiForWindow(window_->hwnd());
  const int w = ::MulDiv(kWidthDip, dpi, 96);
  const int h = ::MulDiv(kHeightDip, dpi, 96);
  const int margin = ::MulDiv(kMarginDip, dpi, 96);
  POINT at{std::max<LONG>(content.left, content.right - w - margin), content.top + margin};
  ::ClientToScreen(window_->hwnd(), &at);
  ::SetWindowPos(popup_, HWND_TOP, at.x, at.y, std::min<int>(w, content.right - content.left - margin), h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void FindBar::Open(BrowserWindow* window) {
  if (!window || window->profile() == nullptr || !window->active_tab() || !window->active_tab()->browser()) return;
  if (CefRefPtr<FindBar> existing = Current()) {
    if (existing->window_ == window && !existing->closing_) {
      // Already open here: focus its text field again.
      if (existing->browser_) {
        existing->browser_->GetHost()->SetFocus(true);
        existing->browser_->GetMainFrame()->ExecuteJavaScript("window.orbitFocusFind && orbitFocusFind()", "", 0);
      }
      return;
    }
    Close();
    return;  // opened again by the next call once it has closed
  }
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = &FindBar::BarProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kBarClass;
    ::RegisterClassExW(&wc);
    registered = true;
  }
  CefRefPtr<FindBar> bar = new FindBar(window);
  bar->popup_ = ::CreateWindowExW(WS_EX_TOOLWINDOW, kBarClass, L"Find in page", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 10,
                                  10, window->hwnd(), nullptr, ::GetModuleHandleW(nullptr), nullptr);
  if (!bar->popup_) return;
  const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
  ::DwmSetWindowAttribute(bar->popup_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
  const BOOL dark = window->palette().dark ? TRUE : FALSE;
  ::DwmSetWindowAttribute(bar->popup_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  Current() = bar;
  bar->Place();
  ClearHighlights();
  Search_().target = window->active_tab()->browser();
  RECT rc;
  ::GetClientRect(bar->popup_, &rc);
  CefWindowInfo info;
  info.SetAsChild(bar->popup_, CefRect(0, 0, rc.right, rc.bottom));
  info.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
  CefBrowserSettings settings;
  if (!CefBrowserHost::CreateBrowser(info, bar, "orbit://find/", settings, nullptr,
                                     window->profile()->request_context())) {
    ::DestroyWindow(bar->popup_);
    bar->popup_ = nullptr;
    Current() = nullptr;
  }
}

void FindBar::Close() {
  CefRefPtr<FindBar> bar = Current();
  ClearHighlights();
  if (!bar || bar->closing_) return;
  bar->closing_ = true;
  if (bar->popup_) ::ShowWindow(bar->popup_, SW_HIDE);
  if (bar->browser_) bar->browser_->GetHost()->CloseBrowser(true);  // else closed in OnAfterCreated
}

void FindBar::Reposition(BrowserWindow* window) {
  if (IsOpenFor(window)) Current()->Place();
}

void FindBar::OnActiveTabChanged(BrowserWindow* window) {
  if (!IsOpenFor(window)) return;
  Tab* tab = window->active_tab();
  SearchState& st = Search_();
  if (!tab || !tab->browser() || !st.target || !tab->browser()->IsSame(st.target)) Close();
}

void FindBar::Search(const std::wstring& text, bool forward, bool match_case, bool next) {
  CefRefPtr<FindBar> bar = Current();
  SearchState& st = Search_();
  if (!bar || bar->closing_ || !bar->window_) return;
  Tab* tab = bar->window_->active_tab();
  if (!tab || !tab->browser()) return;
  if (!st.target || !tab->browser()->IsSame(st.target)) {
    if (st.target) st.target->GetHost()->StopFinding(true);
    st.target = tab->browser();
    next = false;
  }
  if (text.empty()) {
    st.target->GetHost()->StopFinding(true);
    st.text.clear();
    st.count = st.active = 0;
    return;
  }
  if (!next || text != st.text) {
    st.count = st.active = 0;
    next = false;
  }
  if (next && st.count > 0 && !st.ordinals_reported) {
    st.active = forward ? st.active % st.count + 1 : (st.active + st.count - 2) % st.count + 1;
  }
  st.text = text;
  st.target->GetHost()->Find(text, forward, match_case, next);
}

void FindBar::FindNext(BrowserWindow* window, bool forward) {
  if (!window) return;
  SearchState& st = Search_();
  if (!IsOpenFor(window) || st.text.empty()) {
    Open(window);  // F3 with no bar opens it
    return;
  }
  Search(st.text, forward, false, true);
}

void FindBar::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  browser_ = browser;
  if (HWND child = browser->GetHost()->GetWindowHandle(); child && popup_) {
    RECT rc;
    ::GetClientRect(popup_, &rc);
    ::SetWindowPos(child, nullptr, 0, 0, rc.right, rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
  }
  if (closing_) {
    browser->GetHost()->CloseBrowser(true);
    return;
  }
  browser->GetHost()->SetFocus(true);
}

bool FindBar::DoClose(CefRefPtr<CefBrowser> browser) {
  if (HWND child = browser->GetHost()->GetWindowHandle()) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](HWND h) {
                              if (::IsWindow(h)) ::DestroyWindow(h);
                            },
                            child));
  }
  return true;
}

void FindBar::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  browser_ = nullptr;
  if (popup_) {
    HWND popup = popup_;
    popup_ = nullptr;
    // Alloy style: its window is destroyed asynchronously (not inside this callback).
    CefPostTask(TID_UI, base::BindOnce(
                            [](HWND h) {
                              if (::IsWindow(h)) ::DestroyWindow(h);
                            },
                            popup));
  }
  if (Current().get() == this) Current() = nullptr;
  if (Application* app = Application::Get()) app->OnWindowBrowserClosed();
}

bool FindBar::OnKeyEvent(CefRefPtr<CefBrowser>, const CefKeyEvent& event, CefEventHandle) {
  if (event.type == KEYEVENT_RAWKEYDOWN && event.windows_key_code == VK_ESCAPE) {
    Close();
    return true;
  }
  return false;
}

bool FindBar::OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> request, bool, bool) {
  return request->GetURL().ToString().rfind("orbit://find/", 0) != 0;
}

}  // namespace orbit
