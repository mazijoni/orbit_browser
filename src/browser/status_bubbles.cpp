#include "browser/status_bubbles.h"

#include <unordered_map>

#include "app/application.h"
#include "browser/tab.h"
#include "ui/browser_window.h"

namespace orbit::status_bubbles {
namespace {

std::unordered_map<HWND, HWND>& Owners() {  // bubble -> the tab host it showed over
  static std::unordered_map<HWND, HWND> owners;
  return owners;
}

// Chromium's status bubble: a click-through, never-activated tool popup of a
// Chromium widget, owned by an Orbit window, one line high.
bool IsStatusBubble(HWND hwnd) {
  wchar_t cls[32] = {};
  ::GetClassNameW(hwnd, cls, 32);
  if (std::wstring(cls) != L"Chrome_WidgetWin_1") return false;
  HWND owner = ::GetWindow(hwnd, GW_OWNER);
  wchar_t owner_cls[32] = {};
  if (!owner || !::GetClassNameW(owner, owner_cls, 32) || std::wstring(owner_cls) != L"OrbitBrowserWindow") return false;
  const LONG_PTR ex = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  constexpr LONG_PTR kBubble = WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
  if ((ex & kBubble) != kBubble) return false;
  RECT rc;
  ::GetWindowRect(hwnd, &rc);
  return rc.bottom - rc.top <= ::MulDiv(48, static_cast<int>(::GetDpiForWindow(hwnd)), 96);
}

// The tab host under the pointer (the hovered page) in the bubble's window,
// else that window's active tab's.
HWND HostForBubble(HWND bubble) {
  HWND owner = ::GetWindow(bubble, GW_OWNER);
  POINT pt;
  ::GetCursorPos(&pt);
  for (HWND h = ::WindowFromPoint(pt); h; h = ::GetParent(h)) {
    if (Tab::FromHost(h)) {
      if (::GetAncestor(h, GA_ROOT) == owner) return h;
      break;
    }
  }
  Application* app = Application::Get();
  if (!app) return nullptr;
  for (const auto& w : app->windows()) {
    if (w->hwnd() == owner && w->active_tab()) return w->active_tab()->host();
  }
  return nullptr;
}

void CALLBACK OnShow(HWINEVENTHOOK, DWORD, HWND hwnd, LONG object, LONG child, DWORD, DWORD) {
  if (object != OBJID_WINDOW || child != CHILDID_SELF || !hwnd || !IsStatusBubble(hwnd)) return;
  if (HWND host = HostForBubble(hwnd)) Owners()[hwnd] = host;
}

}  // namespace

void Install() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  // Out of context: delivered on this (the UI) thread's message loop.
  ::SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, &OnShow, ::GetCurrentProcessId(), 0,
                    WINEVENT_OUTOFCONTEXT);
}

void HideFor(HWND tab_host) {
  if (!tab_host) return;
  for (auto it = Owners().begin(); it != Owners().end();) {
    if (!::IsWindow(it->first)) {
      it = Owners().erase(it);
      continue;
    }
    if (it->second == tab_host && ::IsWindowVisible(it->first)) ::ShowWindow(it->first, SW_HIDE);
    ++it;
  }
}

}  // namespace orbit::status_bubbles
