#include "ui/browser_window.h"

#include <commctrl.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>

#include "app/application.h"
#include "app/resource.h"
#include "browser/downloads.h"
#include "browser/profile.h"
#include "browser/tab.h"
#include "core/log.h"
#include "core/url_fixup.h"
#include "include/cef_client.h"
#include "ui/graphics.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace orbit {
namespace {

constexpr wchar_t kWindowClass[] = L"OrbitBrowserWindow";
constexpr wchar_t kPopupClass[] = L"OrbitFloatingUrlBar";
constexpr UINT_PTR kSpinnerTimer = 1;
constexpr UINT_PTR kRevealTimer = 2;
constexpr UINT kSpinnerIntervalMs = 60;
constexpr UINT kRevealDelayMs = 350;
constexpr int kAddressId = 100;
constexpr UINT kAccessibleInvokeMessage = WM_APP + 1;  // wParam: kind, lParam: key
constexpr UINT kHidePopupMessage = WM_APP + 2;
constexpr float kInner = 7.f;            // sidebar content inset (toolbox padding + tab margin)
constexpr float kButton = 28.f;          // sidebar toolbar button
constexpr float kFolderIndent = 12.f;    // --zen-folder-indent
constexpr float kFloatingBarHeight = 52.f;
constexpr float kFloatingBarMaxWidth = 720.f;

// windowsx.h is avoided: its macros (GetFirstChild, ...) collide with CEF headers.
int LParamX(LPARAM lp) { return static_cast<int>(static_cast<short>(LOWORD(lp))); }
int LParamY(LPARAM lp) { return static_cast<int>(static_cast<short>(HIWORD(lp))); }

HINSTANCE ModuleInstance() { return reinterpret_cast<HINSTANCE>(&__ImageBase); }

enum MenuId : UINT {
  kCmdNewTabBelow = 1,
  kCmdReload,
  kCmdDuplicate,
  kCmdPin,
  kCmdProtect,
  kCmdMute,
  kCmdDiscard,
  kCmdClose,
  kCmdCloseOthers,
  kCmdCloseBelow,
  kCmdReopen,
  kCmdNewWindow,
  kCmdNewPrivateWindow,
  kCmdNewTab,
  kCmdZoomIn,
  kCmdZoomOut,
  kCmdZoomReset,
  kCmdDevTools,
  kCmdToggleSidebar,
  kCmdCompactMode,
  kCmdExit,
  kCmdRename,
  kCmdDelete,
  kCmdMoveUp,
  kCmdMoveDown,
  kCmdNewWorkspace,
  kCmdNewProfile,
  kCmdRenameProfile,
  kCmdDeleteProfile,
  kCmdMoveToNewWindow,
  kCmdNewGroup,
  kCmdRemoveFromGroup,
  kCmdToggleGroup,
  kCmdUngroup,
  kCmdCloseGroup,
  kCmdRenameGroup,
  kCmdProfiles,
  kCmdBookmarks,
  kCmdHistory,
  kCmdDownloads,
  kCmdSettings,
  kCmdClearTabs,
  kCmdNewTabGroup,
  kCmdWorkspaceBase = 1000,   // + workspace index
  kCmdWindowBase = 2000,      // + window index
  kCmdProfileBase = 3000,     // + profile index
  kCmdColorBase = 4000,       // + accent index
  kCmdGroupBase = 5000,       // + group index
};

constexpr const wchar_t* kColorNames[] = {L"Periwinkle", L"Teal", L"Amber", L"Rose",
                                          L"Violet",     L"Sky",  L"Moss",  L"Gold"};

bool IsNewTabUrl(const std::wstring& url) {
  return url.empty() || url == Application::kNewTabUrl;
}

// Zen trims the scheme and a lone trailing slash from the displayed URL.
std::wstring DisplayUrl(const std::wstring& url) {
  std::wstring text = url;
  for (const wchar_t* scheme : {L"https://", L"http://"}) {
    if (text.rfind(scheme, 0) == 0) {
      text.erase(0, wcslen(scheme));
      break;
    }
  }
  if (text.rfind(L"www.", 0) == 0) text.erase(0, 4);
  const size_t slash = text.find(L'/');
  if (slash != std::wstring::npos && slash + 1 == text.size()) text.pop_back();
  return text;
}

struct PromptState {
  const wchar_t* title;
  const wchar_t* label;
  std::wstring* value;
};

INT_PTR CALLBACK PromptProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_INITDIALOG: {
      auto* state = reinterpret_cast<PromptState*>(lp);
      ::SetWindowLongPtrW(dlg, DWLP_USER, lp);
      ::SetWindowTextW(dlg, state->title);
      ::SetDlgItemTextW(dlg, IDC_PROMPT_LABEL, state->label);
      ::SetDlgItemTextW(dlg, IDC_PROMPT_EDIT, state->value->c_str());
      ::SendDlgItemMessageW(dlg, IDC_PROMPT_EDIT, EM_SETSEL, 0, -1);
      ::SendDlgItemMessageW(dlg, IDC_PROMPT_EDIT, EM_LIMITTEXT, 120, 0);
      return TRUE;
    }
    case WM_COMMAND:
      if (LOWORD(wp) == IDOK) {
        auto* state = reinterpret_cast<PromptState*>(::GetWindowLongPtrW(dlg, DWLP_USER));
        wchar_t buffer[128] = {};
        ::GetDlgItemTextW(dlg, IDC_PROMPT_EDIT, buffer, 128);
        *state->value = buffer;
        ::EndDialog(dlg, IDOK);
        return TRUE;
      }
      if (LOWORD(wp) == IDCANCEL) {
        ::EndDialog(dlg, IDCANCEL);
        return TRUE;
      }
      break;
  }
  return FALSE;
}

}  // namespace

BrowserWindow::BrowserWindow(Profile* profile) : profile_(profile) {
  Application* app = Application::Get();
  palette_ = ui::MakePalette(app->dark_mode(), app->accent_color());
  Store& store = app->store();
  if (auto w = store.GetPref(Store::kGlobalScope, L"sidebar_width")) {
    sidebar_width_ = std::clamp(static_cast<float>(_wtof(w->c_str())),
                                ui::metrics::kSidebarMinWidth, ui::metrics::kSidebarMaxWidth);
  }
  collapsed_ = store.GetPref(Store::kGlobalScope, L"sidebar_compact") == L"1";
  compact_mode_ = store.GetPref(Store::kGlobalScope, L"sidebar_hidden") == L"1";
}

BrowserWindow::~BrowserWindow() {
  if (popup_) {
    ::SetWindowLongPtrW(popup_, GWLP_USERDATA, 0);
    ::DestroyWindow(popup_);
  }
  if (hwnd_) {
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    ::DestroyWindow(hwnd_);
  }
  if (address_font_) ::DeleteObject(address_font_);
  if (popup_font_) ::DeleteObject(popup_font_);
  if (address_brush_) ::DeleteObject(address_brush_);
  if (popup_brush_) ::DeleteObject(popup_brush_);
}

bool BrowserWindow::Create(const WindowRecord* placement) {
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &BrowserWindow::WndProc;
    wc.hInstance = ModuleInstance();
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = ::LoadIconW(ModuleInstance(), MAKEINTRESOURCEW(IDI_ORBIT));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = kWindowClass;
    if (!::RegisterClassExW(&wc)) return false;
    WNDCLASSEXW pc{sizeof(pc)};
    pc.style = CS_DROPSHADOW;
    pc.lpfnWndProc = &BrowserWindow::PopupProc;
    pc.hInstance = ModuleInstance();
    pc.hCursor = ::LoadCursorW(nullptr, IDC_IBEAM);
    pc.lpszClassName = kPopupClass;
    if (!::RegisterClassExW(&pc)) return false;
    registered = true;
  }

  // Default size: 1280x820 DIPs on the primary monitor's DPI.
  const UINT system_dpi = ::GetDpiForSystem();
  int w = ::MulDiv(1280, static_cast<int>(system_dpi), 96);
  int h = ::MulDiv(820, static_cast<int>(system_dpi), 96);
  hwnd_ = ::CreateWindowExW(0, kWindowClass, L"Orbit",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                            w, h, nullptr, nullptr, ModuleInstance(), this);
  if (!hwnd_) {
    ORBIT_LOG_ERROR("CreateWindowEx failed (%lu)", ::GetLastError());
    return false;
  }
  dpi_ = ::GetDpiForWindow(hwnd_);
  // Recompute the frame now that WM_NCCALCSIZE removes the caption.
  ::SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
  ::DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
  accessibility_ = std::make_unique<AccessibilityHost>(this);

  // Invisible label directly before the EDIT in z-order: Windows' accessibility
  // proxy names an edit control after the static text that precedes it.
  address_label_ = ::CreateWindowExW(0, L"STATIC", L"Address and search bar", WS_CHILD | SS_LEFT,
                                     0, 0, 0, 0, hwnd_, nullptr, ModuleInstance(), nullptr);
  address_ = ::CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
                               0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddressId)),
                               ModuleInstance(), nullptr);
  ::SetWindowSubclass(address_, &BrowserWindow::AddressProc, 1,
                      reinterpret_cast<DWORD_PTR>(this));
  ::SendMessageW(address_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"Search or enter address"));
  ::SendMessageW(address_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);

  popup_ = ::CreateWindowExW(WS_EX_TOOLWINDOW, kPopupClass, L"Address and search bar", WS_POPUP,
                             0, 0, 0, 0, hwnd_, nullptr, ModuleInstance(), this);
  if (popup_) {
    ::DwmSetWindowAttribute(popup_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    popup_label_ = ::CreateWindowExW(0, L"STATIC", L"Address and search bar", WS_CHILD | SS_LEFT,
                                     0, 0, 0, 0, popup_, nullptr, ModuleInstance(), nullptr);
  }

  tooltip_ = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                               WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr,
                               ModuleInstance(), nullptr);
  TTTOOLINFOW ti{sizeof(ti)};
  ti.uFlags = TTF_SUBCLASS;
  ti.hwnd = hwnd_;
  ti.uId = 1;
  ti.lpszText = LPSTR_TEXTCALLBACKW;
  ::SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
  ::SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 420);

  ApplyTheme();

  int show = SW_SHOWNORMAL;
  if (placement && placement->w > 0 && placement->h > 0) {
    RECT r{placement->x, placement->y, placement->x + placement->w,
           placement->y + placement->h};
    if (::MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
      WINDOWPLACEMENT wp{sizeof(wp)};
      wp.rcNormalPosition = r;
      wp.showCmd = SW_HIDE;
      ::SetWindowPlacement(hwnd_, &wp);
    }
    if (placement->maximized) show = SW_SHOWMAXIMIZED;
  }
  Layout();
  ::ShowWindow(hwnd_, show);
  ::UpdateWindow(hwnd_);
  return true;
}

void BrowserWindow::Destroy() {
  if (hwnd_) ::DestroyWindow(hwnd_);
}

void BrowserWindow::ApplyTheme() {
  Application* app = Application::Get();
  palette_ = ui::MakePalette(app->dark_mode(), app->accent_color());
  theme_strength_ = 0.45f;
  if (auto s = app->store().GetPref(Store::kGlobalScope, L"theme_strength")) {
    theme_strength_ = std::clamp(static_cast<float>(_wtof(s->c_str())), 0.f, 1.f);
  }
  if (profile_->is_private()) {
    // Zen's private windows: a near-black, purple-tinted surface, always dark.
    palette_ = ui::MakePalette(true, profile_->record().color);
    palette_.chrome_bg = ui::ColorFromRgb(0x0F0D12);
    theme_strength_ = 0.f;
  }
  const BOOL dark = palette_.dark ? TRUE : FALSE;
  ::DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  if (popup_) ::DwmSetWindowAttribute(popup_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  if (address_font_) ::DeleteObject(address_font_);
  address_font_ = ::CreateFontW(-static_cast<int>(std::lround(px(13.5f))), 0, 0, 0, FW_MEDIUM,
                                FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                ui::Graphics::Get().ui_font_family());
  if (popup_font_) ::DeleteObject(popup_font_);
  popup_font_ = ::CreateFontW(-static_cast<int>(std::lround(px(16.f))), 0, 0, 0, FW_NORMAL,
                              FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                              ui::Graphics::Get().ui_font_family());
  ::SendMessageW(address_, WM_SETFONT,
                 reinterpret_cast<WPARAM>(popup_visible_ ? popup_font_ : address_font_), TRUE);
  favicons_.clear();
  popup_rt_.Reset();
  popup_brush_d2d_.Reset();
  RefreshBackdrop();
  Layout();
}

void BrowserWindow::RefreshBackdrop() {
  const uint32_t color = workspace_ ? workspace_->record().color : ui::kAccentChoices[0];
  backdrop_ = ui::MakeBackdrop(palette_, color, theme_strength_);
  const COLORREF border = ui::ToColorRef(backdrop_.from);
  ::DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof(border));
  UpdateAddressColors();
  Invalidate();
}

D2D1_COLOR_F BrowserWindow::BackdropAt(float x, float y) const {
  // The gradient runs from the top-left to the bottom-right corner.
  const float w = std::max(Width(), 1.f);
  const float h = std::max(Height(), 1.f);
  const float t = std::clamp((x * w + y * h) / (w * w + h * h), 0.f, 1.f);
  return ui::Mix(backdrop_.to, backdrop_.from, t);
}

D2D1_COLOR_F BrowserWindow::UrlFieldColor() const {
  const bool focused = ::GetFocus() == address_ && !popup_visible_;
  if (focused) return palette_.field_bg;
  const D2D1_COLOR_F under = BackdropAt((address_rect_.left + address_rect_.right) / 2.f,
                                        (address_rect_.top + address_rect_.bottom) / 2.f);
  return ui::Over(palette_.element_bg, under);
}

void BrowserWindow::UpdateAddressColors() {
  if (address_brush_) ::DeleteObject(address_brush_);
  address_brush_ = ::CreateSolidBrush(ui::ToColorRef(UrlFieldColor()));
  if (popup_brush_) ::DeleteObject(popup_brush_);
  popup_brush_ = ::CreateSolidBrush(ui::ToColorRef(palette_.field_bg));
  if (address_) ::InvalidateRect(address_, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// Geometry

float BrowserWindow::Width() const {
  RECT rc;
  ::GetClientRect(hwnd_, &rc);
  return dip(static_cast<float>(rc.right));
}

float BrowserWindow::Height() const {
  RECT rc;
  ::GetClientRect(hwnd_, &rc);
  return dip(static_cast<float>(rc.bottom));
}

float BrowserWindow::SidebarWidth() const {
  if (chrome_hidden() || compact_mode_) return 0.f;
  return collapsed_ ? ui::metrics::kSidebarCompactWidth : sidebar_width_;
}

float BrowserWindow::SidebarPanelWidth() const {
  if (chrome_hidden()) return 0.f;
  if (compact_mode_ && !sidebar_revealed_) return 0.f;
  return collapsed_ ? ui::metrics::kSidebarCompactWidth : sidebar_width_;
}

bool BrowserWindow::sidebar_visible() const { return SidebarPanelWidth() > 0.f; }

RECT BrowserWindow::ToPx(const D2D1_RECT_F& r) const {
  return RECT{static_cast<LONG>(std::lround(px(r.left))), static_cast<LONG>(std::lround(px(r.top))),
              static_cast<LONG>(std::lround(px(r.right))),
              static_cast<LONG>(std::lround(px(r.bottom)))};
}

D2D1_RECT_F BrowserWindow::ContentRectDip() const {
  const float W = Width();
  const float H = Height();
  if (chrome_hidden()) return {0, 0, W, H};
  using ui::metrics::kSeparation;
  const float left = compact_mode_ ? kSeparation : SidebarWidth();
  return {left, kSeparation, std::max(left + 1.f, W - kSeparation),
          std::max(kSeparation + 1.f, H - kSeparation)};
}

RECT BrowserWindow::ContentRectPx() const {
  if (chrome_hidden()) {
    RECT rc;
    ::GetClientRect(hwnd_, &rc);
    return rc;
  }
  RECT r = ToPx(ContentRectDip());
  // Reveals move the card instead of resizing it, so the page does not
  // re-layout while the pointer passes; the part pushed past the window edge
  // is clipped for as long as the reveal lasts.
  int dx = 0;
  int dy = 0;
  if (compact_mode_ && sidebar_revealed_) {
    dx = static_cast<int>(std::lround(px(SidebarPanelWidth() + 4.f - ui::metrics::kSeparation)));
  }
  if (caption_revealed_) dy = static_cast<int>(std::lround(px(ui::metrics::kCaptionHeight)));
  ::OffsetRect(&r, dx, dy);
  return r;
}

int BrowserWindow::ContentCornerPx() const {
  if (chrome_hidden()) return 0;
  return static_cast<int>(std::lround(px(ui::metrics::kContentRadius)));
}

void BrowserWindow::Layout() {
  using namespace ui::metrics;
  const float W = Width();
  const float H = Height();
  const float sw = SidebarPanelWidth();
  const bool visible = sw > 0.f;

  address_rect_ = site_info_ = star_ = ws_actions_ = {};
  menu_ = toggle_ = back_ = forward_ = reload_ = ws_indicator_ = {};
  list_ = footer_ = downloads_ = create_new_ = panel_ = {};
  workspace_dots_.clear();
  first_dot_workspace_ = 0;

  // Window controls, flush with the top-right corner while revealed.
  caption_min_ = caption_max_ = caption_close_ = {};
  if (caption_revealed_ && !chrome_hidden()) {
    caption_close_ = {W - kCaptionButtonWidth, 0, W, kCaptionHeight};
    caption_max_ = {caption_close_.left - kCaptionButtonWidth, 0, caption_close_.left,
                    kCaptionHeight};
    caption_min_ = {caption_max_.left - kCaptionButtonWidth, 0, caption_max_.left,
                    kCaptionHeight};
  }

  const auto& workspaces = profile_->workspaces();
  if (visible) {
    panel_ = {0, 0, sw, H};
    if (compact_mode_) panel_ = {4.f, 4.f, sw - 2.f, H - 4.f};  // floating card
    const float l = kInner;
    const float r = sw - kInner;
    if (!collapsed_) {
      // Top row: menu and compact-mode toggle; navigation on the right.
      const float by = (kTopRowHeight - kButton) / 2.f;
      menu_ = {l, by, l + kButton, by + kButton};
      toggle_ = {menu_.right + 1, by, menu_.right + 1 + kButton, by + kButton};
      reload_ = {r - kButton, by, r, by + kButton};
      forward_ = {reload_.left - 1 - kButton, by, reload_.left - 1, by + kButton};
      back_ = {forward_.left - 1 - kButton, by, forward_.left - 1, by + kButton};
      float y = kTopRowHeight;
      // Address bar.
      address_rect_ = {l, y, r, y + kUrlBarHeight};
      site_info_ = {address_rect_.left + 4, address_rect_.top + 5, address_rect_.left + 30,
                    address_rect_.bottom - 5};
      if (!profile_->is_private()) {
        star_ = {address_rect_.right - 30, address_rect_.top + 5, address_rect_.right - 4,
                 address_rect_.bottom - 5};
      }
      y = address_rect_.bottom + 4;
      // Workspace indicator: the space's name, with an actions button on hover.
      ws_indicator_ = {l, y, r, y + kWorkspaceIndicatorHeight - 6};
      ws_actions_ = {r - 30, ws_indicator_.top + 5, r - 4, ws_indicator_.bottom - 5};
      y = ws_indicator_.bottom + 2;
      // Footer: downloads, workspace switcher, create-new.
      footer_ = {0, H - kFooterHeight, sw, H};
      const float fy = H - kFooterHeight + (kFooterHeight - kIconButton) / 2.f - 2.f;
      downloads_ = {l, fy, l + kIconButton, fy + kIconButton};
      create_new_ = {r - kIconButton, fy, r, fy + kIconButton};
      const float room = create_new_.left - downloads_.right - 8.f;
      const int fit = std::max(1, static_cast<int>(room / (kIconButton + 3.f)));
      const int count = static_cast<int>(workspaces.size());
      const int shown = std::min(count, fit);
      int active = 0;
      for (int i = 0; i < count; ++i) {
        if (workspaces[static_cast<size_t>(i)].get() == workspace_) active = i;
      }
      first_dot_workspace_ = std::clamp(active - shown / 2, 0, count - shown);
      const float total = shown * kIconButton + (shown - 1) * 3.f;
      float x = (downloads_.right + create_new_.left - total) / 2.f;
      for (int i = 0; i < shown; ++i) {
        workspace_dots_.push_back({x, fy, x + kIconButton, fy + kIconButton});
        x += kIconButton + 3.f;
      }
      list_ = {0, y, sw, footer_.top - 2.f};
    } else {
      // Collapsed: one centered column.
      const float cx = sw / 2.f;
      float y = 6.f;
      auto next = [&](D2D1_RECT_F& rect) {
        rect = {cx - 16.f, y, cx + 16.f, y + 32.f};
        y += 34.f;
      };
      next(menu_);
      next(toggle_);
      next(back_);
      next(forward_);
      next(reload_);
      y += 2.f;
      next(ws_indicator_);
      const float footer_h = 3 * 34.f + 8.f;
      footer_ = {0, H - footer_h, sw, H};
      float fy = H - footer_h + 4.f;
      downloads_ = {cx - 16.f, fy, cx + 16.f, fy + 32.f};
      fy += 34.f;
      workspace_dots_.push_back({cx - 16.f, fy, cx + 16.f, fy + 32.f});
      for (int i = 0; i < static_cast<int>(workspaces.size()); ++i) {
        if (workspaces[static_cast<size_t>(i)].get() == workspace_) first_dot_workspace_ = i;
      }
      fy += 34.f;
      create_new_ = {cx - 16.f, fy, cx + 16.f, fy + 32.f};
      list_ = {0, y + 2.f, sw, footer_.top - 2.f};
    }
  }

  if (address_ && !popup_visible_) {
    if (address_rect_.right <= address_rect_.left) {
      ::ShowWindow(address_, SW_HIDE);
    } else {
      const float right = star_.right > star_.left ? star_.left - 2 : address_rect_.right - 10;
      const RECT field = ToPx({site_info_.right + 2, address_rect_.top, right,
                               address_rect_.bottom});
      const int edit_h = static_cast<int>(std::lround(px(19.f)));
      const int top = (field.top + field.bottom - edit_h) / 2;
      ::SetWindowPos(address_, nullptr, field.left, top, field.right - field.left, edit_h,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
  }
  if (popup_visible_) PositionFloatingUrlBar();
  if (tooltip_) {
    TTTOOLINFOW ti{sizeof(ti)};
    ti.hwnd = hwnd_;
    ti.uId = 1;
    ::GetClientRect(hwnd_, &ti.rect);
    ::SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
  }
  ClampScroll();
  PlaceContent();
  Invalidate();
}

void BrowserWindow::PlaceContent() {
  if (Tab* tab = active_tab()) tab->Show(ContentRectPx(), ContentCornerPx());
}

int BrowserWindow::VisibleRowCapacity() const {
  return std::max(1, static_cast<int>((list_.bottom - list_.top) / ui::metrics::kRowHeight));
}

float BrowserWindow::PeripheryTop() const {
  return workspace_ ? static_cast<float>(workspace_->PinnedCount()) * ui::metrics::kRowHeight
                    : 0.f;
}

float BrowserWindow::PeripheryHeight() const {
  const bool has_pinned = workspace_ && workspace_->PinnedCount() > 0;
  return (has_pinned ? ui::metrics::kPinnedSeparatorHeight : 0.f) + ui::metrics::kRowHeight;
}

float BrowserWindow::ListContentHeight() const {
  const auto rows = Rows();
  const float periphery_end = PeripheryTop() + PeripheryHeight();
  if (rows.empty()) return periphery_end;
  return std::max(rows.back().top + ui::metrics::kRowHeight, periphery_end);
}

std::vector<BrowserWindow::Row> BrowserWindow::Rows() const {
  using ui::metrics::kRowHeight;
  std::vector<Row> rows;
  if (!workspace_) return rows;
  const auto& tabs = workspace_->tabs();
  const int pinned = workspace_->PinnedCount();
  float y = 0.f;
  int64_t current_group = 0;
  bool collapsed = false;
  for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
    if (i == pinned) y += PeripheryHeight();  // separator + "New Tab"
    const int64_t g = tabs[static_cast<size_t>(i)]->record().group_id;
    if (g != current_group) {
      current_group = g;
      collapsed = false;
      if (g) {
        const TabGroupRecord* group = workspace_->FindGroup(g);
        collapsed = group && group->collapsed;
        rows.push_back({true, -1, g, y});
        y += kRowHeight;
      }
    }
    if (g && collapsed) continue;
    rows.push_back({false, i, g, y});
    y += kRowHeight;
  }
  return rows;
}

D2D1_RECT_F BrowserWindow::RowRect(const Row& row) const {
  using namespace ui::metrics;
  const float top = list_.top + row.top - scroll_;
  const float sw = SidebarPanelWidth();
  if (collapsed_) {
    const float l = (sw - 48.f) / 2.f;
    return {l, top + kRowInset, l + 48.f, top + kRowHeight - kRowInset};
  }
  // Group members are indented under their folder.
  const float indent = (!row.is_group && row.group_id) ? kFolderIndent : 0.f;
  return {kInner + indent, top + kRowInset, sw - kInner, top + kRowHeight - kRowInset};
}

D2D1_RECT_F BrowserWindow::NewTabRect() const {
  using namespace ui::metrics;
  if (!sidebar_visible()) return {};
  const bool has_pinned = workspace_ && workspace_->PinnedCount() > 0;
  const Row row{false, -1, 0, PeripheryTop() + (has_pinned ? kPinnedSeparatorHeight : 0.f)};
  return RowRect(row);
}

D2D1_RECT_F BrowserWindow::ClearRect() const {
  using namespace ui::metrics;
  if (!sidebar_visible() || collapsed_ || !workspace_ || workspace_->PinnedCount() == 0) return {};
  const float top = list_.top + PeripheryTop() - scroll_;
  const float r = SidebarPanelWidth() - kInner;
  return {r - 62.f, top + 2.f, r, top + kPinnedSeparatorHeight - 2.f};
}

int BrowserWindow::RowOfTab(const std::vector<Row>& rows, int tab_index) const {
  for (size_t r = 0; r < rows.size(); ++r) {
    if (!rows[r].is_group && rows[r].tab == tab_index) return static_cast<int>(r);
  }
  // Hidden in a collapsed group: the group's header stands in for it.
  if (workspace_ && tab_index >= 0 && tab_index < static_cast<int>(workspace_->tabs().size())) {
    const int64_t g = workspace_->tabs()[static_cast<size_t>(tab_index)]->record().group_id;
    for (size_t r = 0; r < rows.size(); ++r) {
      if (rows[r].is_group && rows[r].group_id == g) return static_cast<int>(r);
    }
  }
  return -1;
}

int BrowserWindow::GroupIndex(int64_t group_id) const {
  if (!workspace_) return -1;
  const auto& groups = workspace_->groups();
  for (size_t i = 0; i < groups.size(); ++i) {
    if (groups[i].id == group_id) return static_cast<int>(i);
  }
  return -1;
}

D2D1_RECT_F BrowserWindow::TabRowRect(int index) const {
  const auto rows = Rows();
  const int r = RowOfTab(rows, index);
  return r >= 0 ? RowRect(rows[static_cast<size_t>(r)]) : D2D1_RECT_F{};
}

void BrowserWindow::ClampScroll() {
  const float view = list_.bottom - list_.top;
  scroll_ = std::clamp(scroll_, 0.f, std::max(0.f, ListContentHeight() - view));
}

void BrowserWindow::EnsureTabVisible(int index) {
  if (index < 0) return;
  const D2D1_RECT_F r = TabRowRect(index);
  if (r.top < list_.top) scroll_ -= list_.top - r.top;
  if (r.bottom > list_.bottom) scroll_ += r.bottom - list_.bottom;
  ClampScroll();
}

bool BrowserWindow::IsCaptionHit(const Hit& hit) const {
  return hit.kind == HitKind::kCaptionMin || hit.kind == HitKind::kCaptionMax ||
         hit.kind == HitKind::kCaptionClose;
}

BrowserWindow::Hit BrowserWindow::HitTest(float x, float y) const {
  if (chrome_hidden()) return {};
  if (ui::Contains(caption_close_, x, y)) return {HitKind::kCaptionClose, 0};
  if (ui::Contains(caption_max_, x, y)) return {HitKind::kCaptionMax, 0};
  if (ui::Contains(caption_min_, x, y)) return {HitKind::kCaptionMin, 0};
  const float sw = SidebarPanelWidth();
  if (sw <= 0.f) return {};
  if (!collapsed_ && std::fabs(x - sw) <= 3.f) return {HitKind::kSidebarEdge, 0};
  if (x >= sw) return {};
  if (ui::Contains(menu_, x, y)) return {HitKind::kMenu, 0};
  if (ui::Contains(toggle_, x, y)) return {HitKind::kSidebarToggle, 0};
  if (ui::Contains(back_, x, y)) return {HitKind::kBack, 0};
  if (ui::Contains(forward_, x, y)) return {HitKind::kForward, 0};
  if (ui::Contains(reload_, x, y)) return {HitKind::kReload, 0};
  if (ui::Contains(site_info_, x, y)) return {HitKind::kSiteInfo, 0};
  if (ui::Contains(star_, x, y)) return {HitKind::kStar, 0};
  if (!collapsed_ && ui::Contains(ws_actions_, x, y)) return {HitKind::kWorkspaceActions, 0};
  if (ui::Contains(ws_indicator_, x, y)) return {HitKind::kWorkspaceIndicator, 0};
  if (ui::Contains(downloads_, x, y)) return {HitKind::kDownloads, 0};
  if (ui::Contains(create_new_, x, y)) return {HitKind::kCreateNew, 0};
  for (size_t i = 0; i < workspace_dots_.size(); ++i) {
    if (ui::Contains(workspace_dots_[i], x, y)) {
      return {HitKind::kWorkspace, first_dot_workspace_ + static_cast<int>(i)};
    }
  }
  if (workspace_ && y >= list_.top && y < list_.bottom) {
    if (ui::Contains(ClearRect(), x, y)) return {HitKind::kClearTabs, 0};
    const D2D1_RECT_F nt = NewTabRect();
    if (y >= nt.top - ui::metrics::kRowInset && y < nt.bottom + ui::metrics::kRowInset &&
        x >= nt.left && x < nt.right) {
      return {HitKind::kNewTab, 0};
    }
    for (const Row& row : Rows()) {
      const D2D1_RECT_F r = RowRect(row);
      if (y < r.top - ui::metrics::kRowInset || y >= r.bottom + ui::metrics::kRowInset ||
          x < r.left - kFolderIndent || x >= r.right) {
        continue;
      }
      if (row.is_group) return {HitKind::kGroup, GroupIndex(row.group_id)};
      if (!collapsed_ && x >= r.right - 30.f) return {HitKind::kTabClose, row.tab};
      return {HitKind::kTab, row.tab};
    }
    if (y >= list_.top + ListContentHeight() - scroll_) return {HitKind::kTabListEmpty, 0};
  }
  return {};
}

LRESULT BrowserWindow::HitTestNonClient(LPARAM lp) {
  POINT pt{LParamX(lp), LParamY(lp)};
  ::ScreenToClient(hwnd_, &pt);
  if (chrome_hidden()) return HTCLIENT;
  RECT client;
  ::GetClientRect(hwnd_, &client);
  // The caption is gone, so the top resize border lives inside the client area.
  if (!::IsZoomed(hwnd_)) {
    const int border = ::GetSystemMetricsForDpi(SM_CYFRAME, dpi_) +
                       ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
    if (pt.y < border) {
      if (pt.x < border * 2) return HTTOPLEFT;
      if (pt.x >= client.right - border * 2) return HTTOPRIGHT;
      return HTTOP;
    }
  }
  const Hit hit = HitTest(dip(static_cast<float>(pt.x)), dip(static_cast<float>(pt.y)));
  switch (hit.kind) {
    case HitKind::kCaptionMin:
      return HTMINBUTTON;
    case HitKind::kCaptionMax:
      return HTMAXBUTTON;  // keeps Windows 11 snap layouts on hover
    case HitKind::kCaptionClose:
      return HTCLOSE;
    case HitKind::kNone:
      return HTCAPTION;  // empty chrome drags the window (Zen's draggable sidebar)
    default:
      return HTCLIENT;
  }
}

// ---------------------------------------------------------------------------
// Reveal (window controls, compact-mode sidebar)

void BrowserWindow::UpdateReveal(float x, float y) {
  if (chrome_hidden()) return;
  using namespace ui::metrics;
  const D2D1_RECT_F content = ContentRectDip();
  bool schedule = false;
  if (!caption_revealed_) {
    if (y < kSeparation && x >= content.left - kSeparation) SetCaptionRevealed(true);
  } else if (y > kCaptionHeight + kSeparation || x < content.left - kSeparation) {
    schedule = true;
  }
  if (compact_mode_) {
    if (!sidebar_revealed_) {
      if (x < kSeparation) SetSidebarRevealed(true);
    } else if (x > SidebarPanelWidth() + kSeparation) {
      schedule = true;
    }
  }
  if (schedule) ::SetTimer(hwnd_, kRevealTimer, kRevealDelayMs, nullptr);
}

void BrowserWindow::SetCaptionRevealed(bool revealed) {
  if (caption_revealed_ == revealed) return;
  caption_revealed_ = revealed;
  Layout();
}

void BrowserWindow::SetSidebarRevealed(bool revealed) {
  if (sidebar_revealed_ == revealed) return;
  sidebar_revealed_ = revealed;
  if (!revealed) {
    hover_ = {};
    if (::GetFocus() == address_ && !popup_visible_) FocusContent();
  }
  Layout();
}

// ---------------------------------------------------------------------------
// Model notifications

Tab* BrowserWindow::active_tab() const { return workspace_ ? workspace_->active_tab() : nullptr; }

void BrowserWindow::OnTabChanged(Tab* tab) {
  if (tab == active_tab()) {
    UpdateAddressBar(false);
    UpdateTitle();
  }
  UpdateSpinnerTimer();
  Invalidate();
}

void BrowserWindow::OnTabRemoving(Tab* tab) {
  favicons_.erase(tab);
  if (tab == focus_guard_tab_) focus_guard_tab_ = nullptr;
  if (!workspace_ || tab != workspace_->active_tab()) return;
  const auto& tabs = workspace_->tabs();
  const int index = workspace_->IndexOf(tab);
  Tab* next = nullptr;
  if (index + 1 < static_cast<int>(tabs.size())) {
    next = tabs[static_cast<size_t>(index) + 1].get();
  } else if (index > 0) {
    next = tabs[static_cast<size_t>(index) - 1].get();
  }
  tab->Hide();
  workspace_->set_active_tab(nullptr);
  if (next) ActivateTab(next);
}

void BrowserWindow::OnTabsChanged() {
  if (closing_ || !workspace_) return;
  if (workspace_->tabs().empty()) {
    NewTab();
    return;
  }
  if (!workspace_->active_tab()) ActivateTab(workspace_->tabs().front().get());
  focus_row_ = std::min(focus_row_, static_cast<int>(Rows().size()) - 1);
  Layout();
}

void BrowserWindow::OnContentFocused(Tab* tab) {
  // A freshly created browser window takes focus once; while the address bar
  // was just opened for that tab, give the focus back instead of closing it.
  if (tab && tab == focus_guard_tab_ && ::GetTickCount64() < focus_guard_until_ms_) {
    focus_guard_tab_ = nullptr;
    ::SetFocus(address_);
    return;
  }
  focus_guard_tab_ = nullptr;
  if (popup_visible_) HideFloatingUrlBar(true);
  if (address_editing_) {
    address_editing_ = false;
    UpdateAddressBar(true);
  }
  Invalidate();
}

// ---------------------------------------------------------------------------
// Workspaces and tabs

void BrowserWindow::ShowWorkspace(Workspace* workspace, int64_t active_tab_id) {
  if (!workspace || workspace == workspace_) return;
  if (workspace->window() && workspace->window() != this) return;  // shown elsewhere
  if (workspace_) {
    for (const auto& t : workspace_->tabs()) t->Hide();
    workspace_->set_window(nullptr);
  }
  workspace_ = workspace;
  workspace_->set_window(this);
  // Live tabs hosted by another window move here so that closing that window
  // cannot unload them behind this one's back.
  for (const auto& t : workspace_->tabs()) {
    if (t->window() && t->window() != this) t->Reparent(this);
  }
  scroll_ = 0.f;
  focus_row_ = -1;
  RefreshBackdrop();
  Layout();

  Tab* target = nullptr;
  const auto& tabs = workspace_->tabs();
  for (const auto& t : tabs) {
    if (active_tab_id && t->id() == active_tab_id) target = t.get();
  }
  if (!target) target = workspace_->active_tab();
  if (!target && !tabs.empty()) {
    target = std::max_element(tabs.begin(), tabs.end(), [](const auto& a, const auto& b) {
               return a->last_active_ms() < b->last_active_ms();
             })->get();
  }
  workspace_->set_active_tab(nullptr);
  if (target) {
    ActivateTab(target);
  } else {
    NewTab();
  }
  Application::Get()->ScheduleSave();
  Invalidate();
}

Tab* BrowserWindow::OpenTab(const std::wstring& url, bool activate) {
  if (!workspace_) return nullptr;
  TabRecord rec;
  rec.url = url;
  Tab* tab = workspace_->InsertTab(std::make_unique<Tab>(workspace_, rec), -1);
  if (activate) {
    ActivateTab(tab);
  } else {
    tab->EnsureBrowser(this);
  }
  Layout();
  return tab;
}

Tab* BrowserWindow::OpenTabNear(Tab* opener, const std::wstring& url, bool activate) {
  if (!workspace_) return nullptr;
  TabRecord rec;
  rec.url = url;
  const int index = opener ? workspace_->IndexOf(opener) + 1 : -1;
  Tab* tab = workspace_->InsertTab(std::make_unique<Tab>(workspace_, rec), index);
  if (opener && opener->record().group_id && !opener->pinned()) {
    workspace_->SetTabGroup(tab, opener->record().group_id);
  }
  if (activate) {
    ActivateTab(tab);
  } else {
    tab->EnsureBrowser(this);
  }
  Layout();
  return tab;
}

Tab* BrowserWindow::CreatePopupTab(Tab* opener, const std::wstring& url, bool activate,
                                   CefWindowInfo& info) {
  if (!workspace_) return nullptr;
  TabRecord rec;
  rec.url = url;
  const int index = opener ? workspace_->IndexOf(opener) + 1 : -1;
  Tab* tab = workspace_->InsertTab(std::make_unique<Tab>(workspace_, rec), index);
  if (opener && opener->record().group_id && !opener->pinned()) {
    workspace_->SetTabGroup(tab, opener->record().group_id);
  }
  tab->PrepareForPopup(this, info);
  if (activate) ActivateTab(tab);
  Layout();
  return tab;
}

void BrowserWindow::ActivateTab(Tab* tab) {
  if (!workspace_ || !tab || workspace_->IndexOf(tab) < 0) return;
  Tab* previous = workspace_->active_tab();
  if (previous == tab) {
    FocusContent();
    return;
  }
  if (previous) previous->Hide();
  if (const TabGroupRecord* g = workspace_->FindGroup(tab->record().group_id);
      g && g->collapsed) {
    TabGroupRecord expanded = *g;
    expanded.collapsed = false;
    workspace_->UpdateGroup(expanded);
  }
  workspace_->set_active_tab(tab);
  tab->TouchActive();
  tab->EnsureBrowser(this);
  tab->Show(ContentRectPx(), ContentCornerPx());
  EnsureTabVisible(workspace_->IndexOf(tab));
  if (popup_visible_) HideFloatingUrlBar(false);
  address_editing_ = false;
  UpdateAddressBar(true);
  UpdateTitle();
  if (IsNewTabUrl(tab->url()) && address_rect_.right > address_rect_.left) {
    FocusAddressBar();
  } else {
    FocusContent();
  }
  UpdateSpinnerTimer();
  Application::Get()->ScheduleSave();
  Invalidate();
}

void BrowserWindow::NewTab() {
  OpenTab(Application::kNewTabUrl, true);
  // Zen opens the floating URL bar for a new tab.
  ShowFloatingUrlBar();
}

void BrowserWindow::CloseTab(Tab* tab) {
  if (tab) tab->RequestClose();
}

void BrowserWindow::CloseOtherTabs(Tab* keep) {
  std::vector<Tab*> victims;
  for (const auto& t : workspace_->tabs()) {
    if (t.get() != keep && !t->pinned()) victims.push_back(t.get());
  }
  for (Tab* t : victims) t->RequestClose();
}

void BrowserWindow::CloseTabsBelow(Tab* tab) {
  std::vector<Tab*> victims;
  const auto& tabs = workspace_->tabs();
  for (size_t i = static_cast<size_t>(workspace_->IndexOf(tab)) + 1; i < tabs.size(); ++i) {
    if (!tabs[i]->pinned()) victims.push_back(tabs[i].get());
  }
  for (Tab* t : victims) t->RequestClose();
}

void BrowserWindow::ClearUnpinnedTabs() {
  // Zen's "Clear" button: closes every tab below the pinned section.
  if (!workspace_) return;
  std::vector<Tab*> victims;
  for (const auto& t : workspace_->tabs()) {
    if (!t->pinned()) victims.push_back(t.get());
  }
  for (Tab* t : victims) t->RequestClose();
}

void BrowserWindow::DuplicateTab(Tab* tab) { OpenTabNear(tab, tab->url(), true); }

void BrowserWindow::TogglePin(Tab* tab) {
  if (!tab->pinned()) workspace_->SetTabGroup(tab, 0);
  tab->mutable_record().pinned = !tab->pinned();
  // Re-insert at the boundary between pinned and regular tabs.
  const int target = tab->pinned() ? workspace_->PinnedCount() - 1 : workspace_->PinnedCount();
  workspace_->MoveTab(tab, target);
  Layout();
}

void BrowserWindow::MoveTabToWorkspace(Tab* tab, Workspace* target) {
  if (!target || target == workspace_ || target->profile() != profile_) return;
  if (target->window() && target->window() != this) {
    MoveTabToWindow(tab, target->window());
    return;
  }
  OnTabRemoving(tab);
  std::unique_ptr<Tab> owned = workspace_->RemoveTab(tab);
  owned->Hide();
  Tab* moved = target->InsertTab(std::move(owned), -1);
  moved->MarkDirty();  // persists the new workspace_id
  OnTabsChanged();
}

void BrowserWindow::MoveTabToWindow(Tab* tab, BrowserWindow* target) {
  // Tabs may only move between windows of the same profile: a Chromium
  // browser's request context (storage partition) is fixed at creation.
  if (!target || target == this || target->profile_ != profile_ || !target->workspace_) return;
  OnTabRemoving(tab);
  std::unique_ptr<Tab> owned = workspace_->RemoveTab(tab);
  owned->Hide();
  Tab* moved = target->workspace_->InsertTab(std::move(owned), -1);
  moved->MarkDirty();
  if (moved->has_browser() || moved->load_state() == Tab::LoadState::kCreating) {
    moved->Reparent(target);
  }
  target->ActivateTab(moved);
  target->Layout();
  ::SetForegroundWindow(target->hwnd_);
  OnTabsChanged();
}

void BrowserWindow::SelectRelativeTab(int delta) {
  if (!workspace_ || workspace_->tabs().empty()) return;
  const int n = static_cast<int>(workspace_->tabs().size());
  const int index = workspace_->IndexOf(active_tab());
  const int next = ((index < 0 ? 0 : index) + delta + n) % n;
  ActivateTab(workspace_->tabs()[static_cast<size_t>(next)].get());
}

void BrowserWindow::SelectTabIndex(int index) {
  if (!workspace_ || workspace_->tabs().empty()) return;
  const int n = static_cast<int>(workspace_->tabs().size());
  index = (index < 0 || index >= n) ? n - 1 : index;
  ActivateTab(workspace_->tabs()[static_cast<size_t>(index)].get());
}

void BrowserWindow::SelectRelativeWorkspace(int delta) {
  const auto& workspaces = profile_->workspaces();
  const int n = static_cast<int>(workspaces.size());
  if (n < 2) return;
  int index = 0;
  for (int i = 0; i < n; ++i) {
    if (workspaces[static_cast<size_t>(i)].get() == workspace_) index = i;
  }
  // Skip workspaces shown in other windows; stop at the ends like Zen.
  for (int i = index + delta; i >= 0 && i < n; i += delta) {
    Workspace* ws = workspaces[static_cast<size_t>(i)].get();
    if (!ws->window() || ws->window() == this) {
      SwitchWorkspace(ws);
      return;
    }
  }
}

void BrowserWindow::SwitchWorkspace(Workspace* target) {
  if (!target || target == workspace_) return;
  if (target->window() && target->window() != this) {
    BrowserWindow* other = target->window();
    if (::IsIconic(other->hwnd_)) ::ShowWindow(other->hwnd_, SW_RESTORE);
    ::SetForegroundWindow(other->hwnd_);
    return;
  }
  ShowWorkspace(target, 0);
}

void BrowserWindow::NewWorkspace() {
  if (profile_->is_private()) return;
  std::wstring name = L"Space " + std::to_wstring(profile_->workspaces().size() + 1);
  if (!PromptText(L"New workspace", L"Workspace name:", name) || name.empty()) return;
  const size_t n = profile_->workspaces().size();
  const uint32_t color = ui::kAccentChoices[n % std::size(ui::kAccentChoices)];
  if (Workspace* ws = Application::Get()->CreateWorkspace(profile_, name, color)) {
    SwitchWorkspace(ws);
  }
}

void BrowserWindow::RenameWorkspace(Workspace* ws) {
  std::wstring name = ws->record().name;
  if (!PromptText(L"Rename workspace", L"Workspace name:", name) || name.empty()) return;
  ws->mutable_record().name = name;
  if (!profile_->is_private()) Application::Get()->store().UpdateWorkspace(ws->record());
  Invalidate();
}

void BrowserWindow::DeleteWorkspace(Workspace* ws) {
  if (profile_->workspaces().size() <= 1) {
    ::MessageBoxW(hwnd_, L"A profile needs at least one workspace.", L"Orbit",
                  MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (ws->window() && ws->window() != this) return;
  const std::wstring msg = L"Delete workspace \"" + ws->record().name + L"\" and close its " +
                           std::to_wstring(ws->tabs().size()) + L" tab(s)?";
  if (::MessageBoxW(hwnd_, msg.c_str(), L"Delete workspace", MB_OKCANCEL | MB_ICONWARNING) !=
      IDOK) {
    return;
  }
  if (ws == workspace_) {
    for (const auto& other : profile_->workspaces()) {
      if (other.get() != ws && !other->window()) {
        ShowWorkspace(other.get(), 0);
        break;
      }
    }
    if (ws == workspace_) return;  // every other workspace is open elsewhere
  }
  // Free live browsers; the rows go away with the workspace (ON DELETE CASCADE).
  for (const auto& t : ws->tabs()) t->Unload(false);
  if (!profile_->is_private()) Application::Get()->store().DeleteWorkspace(ws->id());
  Application::Get()->RetireWorkspace(ws);
  Layout();
}


// ---------------------------------------------------------------------------
// Address bar and focus

void BrowserWindow::GuardAddressFocus() {
  Tab* tab = active_tab();
  if (tab && !tab->has_browser()) {
    focus_guard_tab_ = tab;
    focus_guard_until_ms_ = ::GetTickCount64() + 2000;
  }
}

void BrowserWindow::FocusAddressBar() {
  GuardAddressFocus();
  if (address_rect_.right <= address_rect_.left || chrome_hidden()) {
    // The sidebar's address bar is not on screen: use the floating one.
    ShowFloatingUrlBar();
    return;
  }
  if (popup_visible_) HideFloatingUrlBar(false);
  ::SetFocus(address_);
  ::SendMessageW(address_, EM_SETSEL, 0, -1);
  Invalidate();
}

void BrowserWindow::FocusContent() {
  if (Tab* tab = active_tab()) tab->Focus();
}

void BrowserWindow::UpdateAddressBar(bool force) {
  RefreshBookmarkState();
  if (address_editing_ && !force) return;
  Tab* tab = active_tab();
  std::wstring text = tab ? tab->url() : std::wstring();
  if (IsNewTabUrl(text)) text.clear();
  // Unfocused, the sidebar shows the trimmed URL; editing shows all of it.
  if (::GetFocus() != address_) text = DisplayUrl(text);
  wchar_t current[2048] = {};
  ::GetWindowTextW(address_, current, 2048);
  if (text != current) {
    address_programmatic_ = true;
    ::SetWindowTextW(address_, text.c_str());
    address_programmatic_ = false;
  }
  address_editing_ = false;
}

void BrowserWindow::NavigateFromAddressBar() {
  const int len = ::GetWindowTextLengthW(address_);
  std::wstring text(static_cast<size_t>(len), L'\0');
  ::GetWindowTextW(address_, text.data(), len + 1);
  const OmniboxResult result = FixupOmniboxInput(text, Application::Get()->search_template());
  if (result.kind == OmniboxResult::Kind::kNone) return;
  if (popup_visible_) HideFloatingUrlBar(false);
  Tab* tab = active_tab();
  if (!tab) tab = OpenTab(result.url, true);
  address_editing_ = false;
  tab->Navigate(result.url);
  FocusContent();
  UpdateAddressBar(true);
}

void BrowserWindow::UpdateTitle() {
  Tab* tab = active_tab();
  std::wstring title = tab ? tab->DisplayTitle() : L"";
  title += profile_->is_private() ? L" — Orbit (Private)" : L" — Orbit";
  ::SetWindowTextW(hwnd_, title.c_str());
}

bool BrowserWindow::PromptText(const wchar_t* title, const wchar_t* label, std::wstring& value) {
  PromptState state{title, label, &value};
  return ::DialogBoxParamW(ModuleInstance(), MAKEINTRESOURCEW(IDD_PROMPT), hwnd_, &PromptProc,
                           reinterpret_cast<LPARAM>(&state)) == IDOK;
}

LRESULT CALLBACK BrowserWindow::AddressProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                            UINT_PTR, DWORD_PTR ref) {
  auto* self = reinterpret_cast<BrowserWindow*>(ref);
  switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      const bool ctrl = ::GetKeyState(VK_CONTROL) < 0;
      const bool shift = ::GetKeyState(VK_SHIFT) < 0;
      const bool alt = ::GetKeyState(VK_MENU) < 0;
      if (wp == VK_RETURN && !alt) {
        self->NavigateFromAddressBar();
        return 0;
      }
      if (wp == VK_ESCAPE) {
        self->address_editing_ = false;
        if (self->popup_visible_) self->HideFloatingUrlBar(false);
        self->FocusContent();
        self->UpdateAddressBar(true);
        return 0;
      }
      if (self->HandleAccelerator(static_cast<int>(wp), ctrl, shift, alt)) return 0;
      break;
    }
    case WM_CHAR:
      if (wp == L'\r' || wp == 0x1B) return 0;  // handled on keydown; avoid the beep
      if (wp == 0x7F) {                          // Ctrl+Backspace: delete previous word
        DWORD start = 0, end = 0;
        ::SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&start),
                       reinterpret_cast<LPARAM>(&end));
        if (start == end && start > 0) {
          const int len = ::GetWindowTextLengthW(hwnd);
          std::wstring text(static_cast<size_t>(len), L'\0');
          ::GetWindowTextW(hwnd, text.data(), len + 1);
          DWORD pos = start;
          while (pos > 0 && text[pos - 1] == L' ') --pos;
          while (pos > 0 && text[pos - 1] != L' ' && text[pos - 1] != L'/' &&
                 text[pos - 1] != L'.') {
            --pos;
          }
          ::SendMessageW(hwnd, EM_SETSEL, pos, start);
        }
        ::SendMessageW(hwnd, WM_CLEAR, 0, 0);
        return 0;
      }
      break;
    case WM_LBUTTONDOWN:
      if (::GetFocus() != hwnd) {
        const LRESULT r = ::DefSubclassProc(hwnd, msg, wp, lp);
        ::SendMessageW(hwnd, EM_SETSEL, 0, -1);  // first click selects the URL
        return r;
      }
      break;
    case WM_SETFOCUS: {
      const LRESULT r = ::DefSubclassProc(hwnd, msg, wp, lp);
      if (!self->address_editing_) {
        self->UpdateAddressBar(true);  // full URL while editing
        ::SendMessageW(hwnd, EM_SETSEL, 0, -1);
      }
      self->UpdateAddressColors();
      self->Invalidate();
      return r;
    }
    case WM_KILLFOCUS: {
      const LRESULT r = ::DefSubclassProc(hwnd, msg, wp, lp);
      if (self->popup_visible_) ::PostMessageW(self->hwnd_, kHidePopupMessage, 0, 0);
      if (!self->address_editing_) self->UpdateAddressBar(true);  // back to the trimmed URL
      self->UpdateAddressColors();
      self->Invalidate();
      return r;
    }
    case WM_NCDESTROY:
      ::RemoveWindowSubclass(hwnd, &BrowserWindow::AddressProc, 1);
      break;
  }
  return ::DefSubclassProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Floating URL bar

void BrowserWindow::ShowFloatingUrlBar() {
  if (!popup_ || closing_) return;
  GuardAddressFocus();
  if (!popup_visible_) {
    popup_visible_ = true;
    ::SetParent(address_, popup_);
    // Keep the EDIT right after its label in z-order (accessible name).
    ::SetWindowPos(address_, popup_label_, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ::SendMessageW(address_, WM_SETFONT, reinterpret_cast<WPARAM>(popup_font_), FALSE);
    PositionFloatingUrlBar();
    ::ShowWindow(popup_, SW_SHOW);
    ::ShowWindow(address_, SW_SHOW);
    Invalidate();  // the sidebar's field is empty while the text floats
  }
  ::SetForegroundWindow(popup_);
  ::SetFocus(address_);
  ::SendMessageW(address_, EM_SETSEL, 0, -1);
}

void BrowserWindow::HideFloatingUrlBar(bool restore_text) {
  if (!popup_visible_) return;
  popup_visible_ = false;
  const bool had_focus = ::GetFocus() == address_;
  ::ShowWindow(popup_, SW_HIDE);
  ::SetParent(address_, hwnd_);
  ::SetWindowPos(address_, address_label_, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  ::SendMessageW(address_, WM_SETFONT, reinterpret_cast<WPARAM>(address_font_), FALSE);
  if (had_focus) FocusContent();
  if (restore_text) address_editing_ = false;
  Layout();
  UpdateAddressBar(true);
}

void BrowserWindow::PositionFloatingUrlBar() {
  const D2D1_RECT_F c = ContentRectDip();
  const float w = std::min((c.right - c.left) * 0.9f, kFloatingBarMaxWidth);
  const float h = kFloatingBarHeight;
  const float left = (c.left + c.right - w) / 2.f;
  const float top = c.top + (c.bottom - c.top) * 0.2f;
  RECT r = ToPx({left, top, left + w, top + h});
  POINT origin{r.left, r.top};
  ::ClientToScreen(hwnd_, &origin);
  const int pw = r.right - r.left;
  const int ph = r.bottom - r.top;
  ::SetWindowPos(popup_, nullptr, origin.x, origin.y, pw, ph, SWP_NOZORDER | SWP_NOACTIVATE);
  if (popup_rt_) popup_rt_->Resize(D2D1::SizeU(static_cast<UINT32>(pw), static_cast<UINT32>(ph)));
  const int edit_h = static_cast<int>(std::lround(px(24.f)));
  const int edit_l = static_cast<int>(std::lround(px(46.f)));
  const int edit_r = static_cast<int>(std::lround(px(18.f)));
  ::SetWindowPos(address_, nullptr, edit_l, (ph - edit_h) / 2, std::max(1, pw - edit_l - edit_r),
                 edit_h, SWP_NOZORDER | SWP_NOACTIVATE);
  ::InvalidateRect(popup_, nullptr, FALSE);
}

LRESULT CALLBACK BrowserWindow::PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wp, lp);
  }
  auto* self = reinterpret_cast<BrowserWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self) return ::DefWindowProcW(hwnd, msg, wp, lp);
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      ::BeginPaint(hwnd, &ps);
      self->PaintPopup();
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_CTLCOLOREDIT:
    case WM_COMMAND:
      return self->HandleMessage(msg, wp, lp);
    case WM_LBUTTONDOWN:
      ::SetFocus(self->address_);
      return 0;
    case WM_MOUSEACTIVATE:
      return MA_ACTIVATE;
    case WM_NCDESTROY:
      self->popup_rt_.Reset();
      self->popup_brush_d2d_.Reset();
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Keyboard

bool BrowserWindow::HandleAccelerator(int vk, bool ctrl, bool shift, bool alt) {
  Tab* tab = active_tab();
  // Ctrl+Alt is AltGr on many keyboard layouts; never treat it as a shortcut.
  if (ctrl && alt) return false;

  if (ctrl && !shift) {
    switch (vk) {
      case 'L':
      case 'K':
        FocusAddressBar();
        return true;
      case 'T':
        NewTab();
        return true;
      case 'W':
      case VK_F4:
        CloseTab(tab);
        return true;
      case 'N':
        OpenNormalWindow();
        return true;
      case 'R':
        if (tab) tab->Reload(false);
        return true;
      case VK_F5:
        if (tab) tab->Reload(true);
        return true;
      case 'D':
        ToggleBookmark();
        return true;
      case 'H':
        OpenInternalPage(kPageHistory);
        return true;
      case 'J':
        OpenInternalPage(kPageDownloads);
        return true;
      case VK_TAB:
      case VK_NEXT:
        SelectRelativeTab(1);
        return true;
      case VK_PRIOR:
        SelectRelativeTab(-1);
        return true;
      case VK_OEM_PLUS:
      case VK_ADD:
        if (tab) tab->Zoom(CEF_ZOOM_COMMAND_IN);
        return true;
      case VK_OEM_MINUS:
      case VK_SUBTRACT:
        if (tab) tab->Zoom(CEF_ZOOM_COMMAND_OUT);
        return true;
      case '0':
      case VK_NUMPAD0:
        if (tab) tab->Zoom(CEF_ZOOM_COMMAND_RESET);
        return true;
      default:
        if (vk >= '1' && vk <= '8') {
          SelectTabIndex(vk - '1');
          return true;
        }
        if (vk == '9') {
          SelectTabIndex(-1);
          return true;
        }
        return false;
    }
  }
  if (ctrl && shift) {
    switch (vk) {
      case 'T':
        Application::Get()->ReopenClosedTab(this);
        return true;
      case 'N':
        Application::Get()->OpenPrivateWindow();
        return true;
      case 'R':
        if (tab) tab->Reload(true);
        return true;
      case VK_TAB:
        SelectRelativeTab(-1);
        return true;
      case 'S':
        ToggleCompact();
        return true;
      case 'C':
        ToggleCompactMode();
        return true;
      case 'O':
        OpenInternalPage(kPageBookmarks);
        return true;
      case VK_NEXT:
        SelectRelativeWorkspace(1);
        return true;
      case VK_PRIOR:
        SelectRelativeWorkspace(-1);
        return true;
      case 'I':
        if (tab && tab->browser()) {
          CefWindowInfo info;
          info.SetAsPopup(hwnd_, L"Developer Tools");
          tab->browser()->GetHost()->ShowDevTools(info, nullptr, CefBrowserSettings(),
                                                  CefPoint());
        }
        return true;
      default:
        return false;
    }
  }
  if (alt && !ctrl) {
    switch (vk) {
      case VK_LEFT:
        if (tab) tab->GoBack();
        return true;
      case VK_RIGHT:
        if (tab) tab->GoForward();
        return true;
      case 'D':
        FocusAddressBar();
        return true;
      default:
        return false;
    }
  }
  if (!ctrl && !alt) {
    switch (vk) {
      case VK_F5:
        if (tab) tab->Reload(shift);
        return true;
      case VK_F6:
        if (::GetFocus() == address_) {
          if (popup_visible_) HideFloatingUrlBar(true);
          FocusContent();
        } else {
          FocusAddressBar();
        }
        return true;
      case VK_F11:
        ToggleWindowFullscreen();
        return true;
      case VK_F12:
        return HandleAccelerator('I', true, true, false);
      case VK_BROWSER_BACK:
        if (tab) tab->GoBack();
        return true;
      case VK_BROWSER_FORWARD:
        if (tab) tab->GoForward();
        return true;
      case VK_ESCAPE:
        if (content_fullscreen_ && tab && tab->browser()) {
          tab->browser()->GetHost()->ExitFullscreen(true);
          return true;
        }
        return false;
      default:
        return false;
    }
  }
  return false;
}

bool BrowserWindow::OnKeyDown(int vk) {
  // Sidebar keyboard navigation (the main window itself has focus). Rows are
  // tabs and group headers; Enter activates a tab or toggles a group.
  const auto rows = Rows();
  if (rows.empty()) return false;
  const int n = static_cast<int>(rows.size());
  if (focus_row_ < 0 || focus_row_ >= n) {
    focus_row_ = std::max(0, RowOfTab(rows, workspace_->IndexOf(active_tab())));
  }
  const Row& row = rows[static_cast<size_t>(focus_row_)];
  Tab* tab = row.is_group ? nullptr : workspace_->tabs()[static_cast<size_t>(row.tab)].get();
  switch (vk) {
    case VK_UP:
      focus_row_ = std::max(0, focus_row_ - 1);
      break;
    case VK_DOWN:
      focus_row_ = std::min(n - 1, focus_row_ + 1);
      break;
    case VK_HOME:
      focus_row_ = 0;
      break;
    case VK_END:
      focus_row_ = n - 1;
      break;
    case VK_LEFT:
    case VK_RIGHT:
      if (row.is_group) {
        const TabGroupRecord* g = workspace_->FindGroup(row.group_id);
        if (g && g->collapsed == (vk == VK_RIGHT)) ToggleGroup(row.group_id);
      }
      break;
    case VK_RETURN:
    case VK_SPACE:
      if (tab) {
        ActivateTab(tab);
      } else {
        ToggleGroup(row.group_id);
      }
      return true;
    case VK_DELETE:
      if (tab) CloseTab(tab);
      return true;
    case VK_APPS: {
      const D2D1_RECT_F r = RowRect(row);
      POINT pt{static_cast<LONG>(px(r.left + 20)), static_cast<LONG>(px(r.bottom))};
      ::ClientToScreen(hwnd_, &pt);
      if (tab) {
        ShowTabMenu(tab, pt);
      } else {
        ShowGroupMenu(row.group_id, pt);
      }
      return true;
    }
    default:
      return false;
  }
  const auto after = Rows();
  if (focus_row_ < static_cast<int>(after.size())) {
    const D2D1_RECT_F r = RowRect(after[static_cast<size_t>(focus_row_)]);
    if (r.top < list_.top) scroll_ -= list_.top - r.top;
    if (r.bottom > list_.bottom) scroll_ += r.bottom - list_.bottom;
    ClampScroll();
  }
  Invalidate();
  NotifyAccessibleFocus();
  return true;
}

// ---------------------------------------------------------------------------
// Fullscreen

void BrowserWindow::SetContentFullscreen(bool fullscreen) {
  if (content_fullscreen_ == fullscreen) return;
  const bool was_hidden = chrome_hidden();
  content_fullscreen_ = fullscreen;
  if (was_hidden != chrome_hidden()) SetChromeFullscreen(chrome_hidden());
}

void BrowserWindow::ToggleWindowFullscreen() {
  const bool was_hidden = chrome_hidden();
  window_fullscreen_ = !window_fullscreen_;
  if (!window_fullscreen_ && content_fullscreen_) {
    if (Tab* tab = active_tab(); tab && tab->browser()) {
      tab->browser()->GetHost()->ExitFullscreen(true);
    }
    content_fullscreen_ = false;
  }
  if (was_hidden != chrome_hidden()) SetChromeFullscreen(chrome_hidden());
}

void BrowserWindow::SetChromeFullscreen(bool on) {
  caption_revealed_ = false;
  sidebar_revealed_ = false;
  if (on) {
    saved_placement_.length = sizeof(saved_placement_);
    ::GetWindowPlacement(hwnd_, &saved_placement_);
    saved_style_ = ::GetWindowLongPtrW(hwnd_, GWL_STYLE);
    ::SetWindowLongPtrW(hwnd_, GWL_STYLE, saved_style_ & ~(WS_CAPTION | WS_THICKFRAME));
    MONITORINFO mi{sizeof(mi)};
    ::GetMonitorInfoW(::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
    ::SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                   mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                   SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
  } else {
    ::SetWindowLongPtrW(hwnd_, GWL_STYLE, saved_style_);
    ::SetWindowPlacement(hwnd_, &saved_placement_);
    ::SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                   SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
  }
  Layout();
}

// ---------------------------------------------------------------------------
// Close

void BrowserWindow::BeginClose() {
  if (closing_) return;
  closing_ = true;
  if (popup_visible_) HideFloatingUrlBar(false);
  ::ShowWindow(hwnd_, SW_HIDE);
  if (workspace_) workspace_->set_window(nullptr);
  for (const auto& ws : profile_->workspaces()) {
    for (const auto& t : ws->tabs()) {
      if (t->window() == this) t->Unload(false);
    }
  }
}

WindowRecord BrowserWindow::ToRecord() const {
  WindowRecord rec;
  rec.profile_id = profile_->id();
  rec.workspace_id = workspace_ ? workspace_->id() : 0;
  rec.active_tab_id = active_tab() ? active_tab()->id() : 0;
  WINDOWPLACEMENT wp{sizeof(wp)};
  if (chrome_hidden()) {
    wp = saved_placement_;
  } else {
    ::GetWindowPlacement(hwnd_, &wp);
  }
  rec.x = wp.rcNormalPosition.left;
  rec.y = wp.rcNormalPosition.top;
  rec.w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
  rec.h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
  rec.maximized = wp.showCmd == SW_SHOWMAXIMIZED || ::IsZoomed(hwnd_);
  return rec;
}

// ---------------------------------------------------------------------------
// Window procedure

LRESULT CALLBACK BrowserWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    auto* self = static_cast<BrowserWindow*>(cs->lpCreateParams);
    self->hwnd_ = hwnd;
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    return ::DefWindowProcW(hwnd, msg, wp, lp);
  }
  auto* self = reinterpret_cast<BrowserWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self) return ::DefWindowProcW(hwnd, msg, wp, lp);
  if (msg == WM_NCDESTROY) {
    if (self->accessibility_) self->accessibility_->Detach();
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    self->hwnd_ = nullptr;
    const LRESULT r = ::DefWindowProcW(hwnd, msg, wp, lp);
    Application::Get()->OnWindowDestroyed(self);  // deletes |self|
    return r;
  }
  return self->HandleMessage(msg, wp, lp);
}

LRESULT BrowserWindow::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_NCCALCSIZE:
      if (wp && (::GetWindowLongPtrW(hwnd_, GWL_STYLE) & WS_THICKFRAME)) {
        // Remove the caption but keep the (invisible) resize borders and the
        // DWM shadow. A maximized window's frame hangs off-screen, so trim it.
        auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
        RECT& r = params->rgrc[0];
        const int fx = ::GetSystemMetricsForDpi(SM_CXFRAME, dpi_) +
                       ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
        const int fy = ::GetSystemMetricsForDpi(SM_CYFRAME, dpi_) +
                       ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
        r.left += fx;
        r.right -= fx;
        r.bottom -= fy;
        if (::IsZoomed(hwnd_)) r.top += fy;
        return 0;
      }
      break;
    case WM_NCHITTEST: {
      const LRESULT hit = ::DefWindowProcW(hwnd_, msg, wp, lp);
      if (hit != HTCLIENT) return hit;
      return HitTestNonClient(lp);
    }
    case WM_NCACTIVATE:
      // Nothing to repaint in the non-client area; -1 suppresses the default.
      return ::DefWindowProcW(hwnd_, msg, wp, -1);
    case WM_NCMOUSEMOVE: {
      if (!tracking_nc_mouse_) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd_, 0};
        ::TrackMouseEvent(&tme);
        tracking_nc_mouse_ = true;
      }
      POINT pt{LParamX(lp), LParamY(lp)};
      ::ScreenToClient(hwnd_, &pt);
      const float x = dip(static_cast<float>(pt.x));
      const float y = dip(static_cast<float>(pt.y));
      UpdateReveal(x, y);
      const Hit hit = HitTest(x, y);
      const Hit shown = IsCaptionHit(hit) ? hit : Hit{};
      if (!(shown == hover_)) {
        hover_ = shown;
        UpdateTooltip({});
        Invalidate();
      }
      break;
    }
    case WM_NCMOUSELEAVE:
      tracking_nc_mouse_ = false;
      if (IsCaptionHit(hover_)) {
        hover_ = {};
        Invalidate();
      }
      ::SetTimer(hwnd_, kRevealTimer, kRevealDelayMs, nullptr);
      break;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
      if (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE) {
        pressed_ = {wp == HTMINBUTTON   ? HitKind::kCaptionMin
                    : wp == HTMAXBUTTON ? HitKind::kCaptionMax
                                        : HitKind::kCaptionClose,
                    0};
        Invalidate();
        return 0;
      }
      break;
    case WM_NCLBUTTONUP:
      if (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE) {
        const Hit pressed = pressed_;
        pressed_ = {};
        Invalidate();
        if (pressed.kind == HitKind::kCaptionMin && wp == HTMINBUTTON) {
          ::ShowWindow(hwnd_, SW_MINIMIZE);
        } else if (pressed.kind == HitKind::kCaptionMax && wp == HTMAXBUTTON) {
          ::ShowWindow(hwnd_, ::IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
        } else if (pressed.kind == HitKind::kCaptionClose && wp == HTCLOSE) {
          ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
        return 0;
      }
      break;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      ::BeginPaint(hwnd_, &ps);
      Paint();
      ::EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      if (rt_) rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
      Layout();
      UpdateAddressColors();
      return 0;
    case WM_MOVE:
      if (Tab* tab = active_tab(); tab && tab->browser()) {
        tab->browser()->GetHost()->NotifyMoveOrResizeStarted();
      }
      if (popup_visible_) PositionFloatingUrlBar();
      return 0;
    case WM_EXITSIZEMOVE:
      Application::Get()->ScheduleSave();
      return 0;
    case WM_DPICHANGED: {
      dpi_ = HIWORD(wp);
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      if (rt_) rt_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
      ::SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
      ApplyTheme();  // recreates the DPI-dependent address bar font
      return 0;
    }
    case WM_GETMINMAXINFO: {
      auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
      mmi->ptMinTrackSize = {static_cast<LONG>(px(640)), static_cast<LONG>(px(420))};
      return 0;
    }
    case WM_SETTINGCHANGE:
      if (lp && std::wstring_view(reinterpret_cast<const wchar_t*>(lp)) == L"ImmersiveColorSet") {
        ApplyTheme();
      }
      return 0;
    case WM_CTLCOLOREDIT:
      if (reinterpret_cast<HWND>(lp) == address_) {
        HDC dc = reinterpret_cast<HDC>(wp);
        const bool floating = popup_visible_;
        const D2D1_COLOR_F bg = floating ? palette_.field_bg : UrlFieldColor();
        // Zen dims the URL to 70% until the field is being edited.
        const bool editing = ::GetFocus() == address_;
        const D2D1_COLOR_F fg = ui::Over(editing ? palette_.text : palette_.text_secondary, bg);
        ::SetTextColor(dc, ui::ToColorRef(fg));
        ::SetBkColor(dc, ui::ToColorRef(bg));
        return reinterpret_cast<LRESULT>(floating ? popup_brush_ : address_brush_);
      }
      break;
    case WM_COMMAND:
      if (LOWORD(wp) == kAddressId && HIWORD(wp) == EN_CHANGE && !address_programmatic_) {
        address_editing_ = true;
      }
      return 0;
    case WM_NOTIFY: {
      auto* hdr = reinterpret_cast<NMHDR*>(lp);
      if (hdr->hwndFrom == tooltip_ && hdr->code == TTN_GETDISPINFOW) {
        static std::wstring text;
        text = TooltipText();
        reinterpret_cast<NMTTDISPINFOW*>(lp)->lpszText = text.data();
        return 0;
      }
      break;
    }
    case WM_MOUSEMOVE:
      OnMouseMove(dip(static_cast<float>(LParamX(lp))),
                  dip(static_cast<float>(LParamY(lp))), wp);
      return 0;
    case WM_MOUSELEAVE:
      tracking_mouse_ = false;
      if (sidebar_hovered_) {
        sidebar_hovered_ = false;
        UpdateAddressColors();
      }
      if (!(hover_ == Hit{}) && !IsCaptionHit(hover_)) {
        hover_ = {};
        Invalidate();
      }
      UpdateTooltip({});
      ::SetTimer(hwnd_, kRevealTimer, kRevealDelayMs, nullptr);
      return 0;
    case WM_LBUTTONDOWN:
      OnLButtonDown(dip(static_cast<float>(LParamX(lp))),
                    dip(static_cast<float>(LParamY(lp))));
      return 0;
    case WM_LBUTTONUP:
      OnLButtonUp(dip(static_cast<float>(LParamX(lp))),
                  dip(static_cast<float>(LParamY(lp))));
      return 0;
    case WM_LBUTTONDBLCLK:
      OnLButtonDoubleClick(dip(static_cast<float>(LParamX(lp))),
                           dip(static_cast<float>(LParamY(lp))));
      return 0;
    case WM_MBUTTONUP: {
      const Hit hit = HitTest(dip(static_cast<float>(LParamX(lp))),
                              dip(static_cast<float>(LParamY(lp))));
      if ((hit.kind == HitKind::kTab || hit.kind == HitKind::kTabClose) && workspace_) {
        CloseTab(workspace_->tabs()[static_cast<size_t>(hit.index)].get());
      }
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (reinterpret_cast<HWND>(lp) != hwnd_) {
        dragging_tab_ = false;
        resizing_sidebar_ = false;
        pressed_ = {};
        Invalidate();
      }
      return 0;
    case WM_CONTEXTMENU: {
      POINT pt{LParamX(lp), LParamY(lp)};
      if (pt.x == -1 && pt.y == -1) {
        OnKeyDown(VK_APPS);
        return 0;
      }
      POINT client = pt;
      ::ScreenToClient(hwnd_, &client);
      const float x = dip(static_cast<float>(client.x));
      if (HitTestNonClient(lp) == HTCAPTION && x >= SidebarPanelWidth()) {
        break;  // the window's system menu
      }
      OnContextMenu(pt.x, pt.y, x, dip(static_cast<float>(client.y)));
      return 0;
    }
    case WM_MOUSEWHEEL:
      OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wp));
      return 0;
    case WM_MOUSEHWHEEL:
      OnMouseHWheel(GET_WHEEL_DELTA_WPARAM(wp));
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        POINT pt;
        ::GetCursorPos(&pt);
        ::ScreenToClient(hwnd_, &pt);
        const Hit hit = HitTest(dip(static_cast<float>(pt.x)), dip(static_cast<float>(pt.y)));
        const bool resize = resizing_sidebar_ || hit.kind == HitKind::kSidebarEdge;
        ::SetCursor(::LoadCursorW(nullptr, resize ? IDC_SIZEWE : IDC_ARROW));
        return TRUE;
      }
      break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      const bool ctrl = ::GetKeyState(VK_CONTROL) < 0;
      const bool shift = ::GetKeyState(VK_SHIFT) < 0;
      const bool alt = ::GetKeyState(VK_MENU) < 0;
      if (HandleAccelerator(static_cast<int>(wp), ctrl, shift, alt)) return 0;
      if (msg == WM_KEYDOWN && OnKeyDown(static_cast<int>(wp))) return 0;
      break;
    }
    case WM_SETFOCUS:
      Invalidate();
      NotifyAccessibleFocus();
      return 0;
    case WM_GETOBJECT:
      if (accessibility_) return accessibility_->HandleGetObject(hwnd_, wp, lp);
      break;
    case kAccessibleInvokeMessage:
      HandleAccessibleInvoke(static_cast<int>(wp), static_cast<intptr_t>(lp));
      return 0;
    case kHidePopupMessage:
      // The floating bar lost focus (clicked elsewhere, window switch, ...).
      if (popup_visible_ && ::GetFocus() != address_) HideFloatingUrlBar(true);
      return 0;
    case WM_KILLFOCUS:
      Invalidate();
      return 0;
    case WM_ACTIVATE:
      if (LOWORD(wp) != WA_INACTIVE && !closing_ && !popup_visible_) {
        // Restore focus to the page (or the address bar on a new tab page).
        Tab* tab = active_tab();
        if (tab && IsNewTabUrl(tab->url()) && address_rect_.right > address_rect_.left) {
          FocusAddressBar();
        } else {
          FocusContent();
        }
      }
      return 0;
    case WM_TIMER:
      if (wp == kSpinnerTimer) {
        spinner_phase_ = std::fmod(spinner_phase_ + 0.12f, 1.f);
        Invalidate();
      } else if (wp == kRevealTimer) {
        ::KillTimer(hwnd_, kRevealTimer);
        POINT pt;
        ::GetCursorPos(&pt);
        const bool inside = ::WindowFromPoint(pt) == hwnd_ || ::IsChild(hwnd_, ::WindowFromPoint(pt));
        ::ScreenToClient(hwnd_, &pt);
        const float x = dip(static_cast<float>(pt.x));
        const float y = dip(static_cast<float>(pt.y));
        using namespace ui::metrics;
        const bool menu_open = ::GetCapture() != nullptr && ::GetCapture() != hwnd_;
        if (caption_revealed_ && !IsCaptionHit(pressed_) && !menu_open &&
            (!inside || y > kCaptionHeight + kSeparation)) {
          SetCaptionRevealed(false);
        }
        const bool editing_here = ::GetFocus() == address_ && !popup_visible_;
        if (sidebar_revealed_ && !dragging_tab_ && !resizing_sidebar_ && !editing_here &&
            !menu_open && (!inside || x > SidebarPanelWidth() + kSeparation)) {
          SetSidebarRevealed(false);
        }
      }
      return 0;
    case WM_CLOSE:
      Application::Get()->OnWindowCloseRequested(this);
      return 0;
    case WM_QUERYENDSESSION:
      Application::Get()->SaveNow();
      return TRUE;
    case WM_ENDSESSION:
      if (wp) Application::Get()->SaveNow();
      return 0;
  }
  return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Mouse

void BrowserWindow::OnMouseMove(float x, float y, WPARAM keys) {
  if (!tracking_mouse_) {
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
    ::TrackMouseEvent(&tme);
    tracking_mouse_ = true;
  }
  UpdateReveal(x, y);
  const bool over_sidebar = x < SidebarPanelWidth();
  if (over_sidebar != sidebar_hovered_) {
    sidebar_hovered_ = over_sidebar;
    UpdateAddressColors();
    Invalidate();
  }
  if (resizing_sidebar_) {
    sidebar_width_ = std::clamp(x, ui::metrics::kSidebarMinWidth, ui::metrics::kSidebarMaxWidth);
    Layout();
    return;
  }
  if ((keys & MK_LBUTTON) && pressed_.kind == HitKind::kTab) {
    POINT now;
    ::GetCursorPos(&now);
    ::ScreenToClient(hwnd_, &now);
    if (!dragging_tab_ && (std::abs(now.x - press_px_.x) > ::GetSystemMetrics(SM_CXDRAG) ||
                           std::abs(now.y - press_px_.y) > ::GetSystemMetrics(SM_CYDRAG))) {
      dragging_tab_ = true;
      drag_from_ = pressed_.index;
    }
    if (dragging_tab_ && workspace_) {
      const auto rows = Rows();
      int index = static_cast<int>(rows.size());
      for (size_t r = 0; r < rows.size(); ++r) {
        const D2D1_RECT_F rect = RowRect(rows[r]);
        if (y < (rect.top + rect.bottom) / 2.f) {
          index = static_cast<int>(r);
          break;
        }
      }
      // Auto-scroll when dragging near the list edges.
      if (y < list_.top + 12.f) scroll_ -= 6.f;
      if (y > list_.bottom - 12.f) scroll_ += 6.f;
      ClampScroll();
      drop_index_ = index;
      Invalidate();
      return;
    }
  }
  const Hit hit = HitTest(x, y);
  if (!(hit == hover_)) {
    hover_ = hit;
    UpdateTooltip(hit);
    Invalidate();
  }
}

void BrowserWindow::OnLButtonDown(float x, float y) {
  pressed_ = HitTest(x, y);
  ::GetCursorPos(&press_px_);
  ::ScreenToClient(hwnd_, &press_px_);
  if (pressed_.kind == HitKind::kNone) return;
  ::SetCapture(hwnd_);
  if (pressed_.kind == HitKind::kSidebarEdge) resizing_sidebar_ = true;
  Invalidate();
}

void BrowserWindow::OnLButtonDoubleClick(float x, float y) {
  // Double-clicking the empty space below the tabs opens a tab; any other
  // double click counts as a second ordinary click.
  if (HitTest(x, y).kind == HitKind::kTabListEmpty) {
    NewTab();
    return;
  }
  OnLButtonDown(x, y);
}

void BrowserWindow::OnLButtonUp(float x, float y) {
  const Hit pressed = pressed_;
  const bool was_dragging = dragging_tab_;
  const bool was_resizing = resizing_sidebar_;
  pressed_ = {};
  dragging_tab_ = false;
  resizing_sidebar_ = false;
  if (::GetCapture() == hwnd_) ::ReleaseCapture();

  if (was_resizing) {
    Application::Get()->store().SetPref(Store::kGlobalScope, L"sidebar_width",
                                        std::to_wstring(static_cast<int>(sidebar_width_)));
    return;
  }
  if (was_dragging && workspace_ && drag_from_ >= 0 &&
      drag_from_ < static_cast<int>(workspace_->tabs().size())) {
    // Dropping onto a group member joins that group; dropping anywhere else
    // (including just above a group header) leaves any group.
    Tab* tab = workspace_->tabs()[static_cast<size_t>(drag_from_)].get();
    const auto rows = Rows();
    const int n_tabs = static_cast<int>(workspace_->tabs().size());
    int target = n_tabs;
    int64_t group = 0;
    if (drop_index_ >= 0 && drop_index_ < static_cast<int>(rows.size())) {
      const Row& at = rows[static_cast<size_t>(drop_index_)];
      if (at.is_group) {
        for (int i = 0; i < n_tabs; ++i) {  // before the group's first member
          if (workspace_->tabs()[static_cast<size_t>(i)]->record().group_id == at.group_id) {
            target = i;
            break;
          }
        }
      } else {
        target = at.tab;
        group = at.group_id;
      }
    }
    if (target > drag_from_) --target;  // index after removal
    workspace_->SetTabGroup(tab, group);
    workspace_->MoveTab(tab, target);
    drag_from_ = drop_index_ = -1;
    Layout();
    return;
  }
  drag_from_ = drop_index_ = -1;
  if (HitTest(x, y) == pressed) Click(pressed);
  Invalidate();
}

void BrowserWindow::OnMouseWheel(int delta) {
  POINT pt;
  ::GetCursorPos(&pt);
  ::ScreenToClient(hwnd_, &pt);
  const float y = dip(static_cast<float>(pt.y));
  const float x = dip(static_cast<float>(pt.x));
  if (x < SidebarPanelWidth() && y >= list_.top && y < list_.bottom) {
    scroll_ -= static_cast<float>(delta) / WHEEL_DELTA * ui::metrics::kRowHeight * 2.f;
    ClampScroll();
    Invalidate();
  }
}

void BrowserWindow::OnMouseHWheel(int delta) {
  // Horizontal scrolling over the sidebar swipes between workspaces (Zen).
  POINT pt;
  ::GetCursorPos(&pt);
  ::ScreenToClient(hwnd_, &pt);
  if (dip(static_cast<float>(pt.x)) >= SidebarPanelWidth()) return;
  hwheel_accum_ += static_cast<float>(delta);
  if (std::fabs(hwheel_accum_) >= WHEEL_DELTA * 1.5f) {
    SelectRelativeWorkspace(hwheel_accum_ > 0 ? 1 : -1);
    hwheel_accum_ = 0.f;
  }
}

void BrowserWindow::Click(const Hit& hit) {
  Tab* tab = active_tab();
  POINT below{};
  auto anchor = [&](const D2D1_RECT_F& r) {
    below = {static_cast<LONG>(px(r.left)), static_cast<LONG>(px(r.bottom + 2))};
    ::ClientToScreen(hwnd_, &below);
    return below;
  };
  auto above = [&](const D2D1_RECT_F& r) {
    // Footer menus open upwards (TPM_BOTTOMALIGN is applied by the caller's
    // position; Windows flips menus that would leave the work area).
    POINT p{static_cast<LONG>(px(r.left)), static_cast<LONG>(px(r.top - 2))};
    ::ClientToScreen(hwnd_, &p);
    return p;
  };
  switch (hit.kind) {
    case HitKind::kBack:
      if (tab) tab->GoBack();
      break;
    case HitKind::kForward:
      if (tab) tab->GoForward();
      break;
    case HitKind::kReload:
      if (tab && tab->loading()) {
        tab->Stop();
      } else if (tab) {
        tab->Reload(::GetKeyState(VK_SHIFT) < 0);
      }
      break;
    case HitKind::kSiteInfo: {
      if (!tab) break;
      const wchar_t* text = L"";
      switch (tab->security()) {
        case SecurityLevel::kSecure:
          text = L"Connection is secure. The certificate is valid and the page is served over HTTPS.";
          break;
        case SecurityLevel::kInsecure:
          text = L"Connection is not secure. Information you send or receive on this page could be read or changed by others.";
          break;
        case SecurityLevel::kDangerous:
          text = L"Not secure. This page has certificate errors or runs insecure content. Do not enter passwords or payment information.";
          break;
        case SecurityLevel::kInternal:
          text = L"This is a built-in Orbit page.";
          break;
        case SecurityLevel::kNone:
          text = L"No site information is available for this page.";
          break;
      }
      ::MessageBoxW(hwnd_, text, L"Site information", MB_OK | MB_ICONINFORMATION);
      break;
    }
    case HitKind::kStar:
      ToggleBookmark();
      break;
    case HitKind::kMenu:
      ShowMainMenu(anchor(menu_));
      break;
    case HitKind::kSidebarToggle:
      ToggleCompactMode();
      break;
    case HitKind::kWorkspaceIndicator:
    case HitKind::kWorkspaceActions:
      ShowWorkspaceMenu(-1, anchor(hit.kind == HitKind::kWorkspaceActions ? ws_actions_
                                                                          : ws_indicator_));
      break;
    case HitKind::kWorkspace:
      if (collapsed_) {
        ShowWorkspaceMenu(-1, above(workspace_dots_.front()));
      } else if (hit.index < static_cast<int>(profile_->workspaces().size())) {
        SwitchWorkspace(profile_->workspaces()[static_cast<size_t>(hit.index)].get());
      }
      break;
    case HitKind::kCreateNew:
      ShowCreateMenu(above(create_new_));
      break;
    case HitKind::kDownloads:
      OpenInternalPage(kPageDownloads);
      break;
    case HitKind::kNewTab:
      NewTab();
      break;
    case HitKind::kClearTabs:
      ClearUnpinnedTabs();
      break;
    case HitKind::kTab:
      if (workspace_) ActivateTab(workspace_->tabs()[static_cast<size_t>(hit.index)].get());
      break;
    case HitKind::kTabClose:
      if (workspace_) CloseTab(workspace_->tabs()[static_cast<size_t>(hit.index)].get());
      break;
    case HitKind::kGroup:
      if (workspace_ && hit.index >= 0) {
        ToggleGroup(workspace_->groups()[static_cast<size_t>(hit.index)].id);
      }
      break;
    case HitKind::kCaptionMin:
    case HitKind::kCaptionMax:
    case HitKind::kCaptionClose:  // handled as non-client buttons
    case HitKind::kSidebarEdge:
    case HitKind::kTabListEmpty:
    case HitKind::kNone:
      break;
  }
}

void BrowserWindow::OnContextMenu(int screen_x, int screen_y, float x, float y) {
  const Hit hit = HitTest(x, y);
  const POINT pt{screen_x, screen_y};
  switch (hit.kind) {
    case HitKind::kTab:
    case HitKind::kTabClose:
      ShowTabMenu(workspace_->tabs()[static_cast<size_t>(hit.index)].get(), pt);
      break;
    case HitKind::kWorkspace:
      ShowWorkspaceMenu(collapsed_ ? -1 : hit.index, pt);
      break;
    case HitKind::kGroup:
      if (hit.index >= 0) ShowGroupMenu(workspace_->groups()[static_cast<size_t>(hit.index)].id, pt);
      break;
    default:
      if (x < SidebarPanelWidth()) ShowWorkspaceMenu(-1, pt);
      break;
  }
}

// ---------------------------------------------------------------------------
// Menus

void BrowserWindow::ShowTabMenu(Tab* tab, POINT pt) {
  if (!tab || !workspace_) return;
  HMENU menu = ::CreatePopupMenu();
  HMENU ws_menu = ::CreatePopupMenu();
  HMENU win_menu = ::CreatePopupMenu();
  const auto& workspaces = profile_->workspaces();
  for (size_t i = 0; i < workspaces.size(); ++i) {
    UINT flags = MF_STRING;
    if (workspaces[i].get() == workspace_) flags |= MF_GRAYED | MF_CHECKED;
    ::AppendMenuW(ws_menu, flags, kCmdWorkspaceBase + i, workspaces[i]->record().name.c_str());
  }
  const auto& windows = Application::Get()->windows();
  for (size_t i = 0; i < windows.size(); ++i) {
    BrowserWindow* w = windows[i].get();
    if (w == this || w->profile_ != profile_ || w->closing_ || !w->workspace_) continue;
    std::wstring label = L"Window: " + w->workspace_->record().name;
    ::AppendMenuW(win_menu, MF_STRING, kCmdWindowBase + i, label.c_str());
  }
  ::AppendMenuW(win_menu, MF_STRING, kCmdMoveToNewWindow, L"New window");
  HMENU group_menu = ::CreatePopupMenu();
  ::AppendMenuW(group_menu, MF_STRING, kCmdNewGroup, L"New folder…");
  const auto& groups = workspace_->groups();
  if (!groups.empty()) ::AppendMenuW(group_menu, MF_SEPARATOR, 0, nullptr);
  for (size_t i = 0; i < groups.size(); ++i) {
    const UINT flags = MF_STRING | (groups[i].id == tab->record().group_id ? MF_CHECKED : 0);
    ::AppendMenuW(group_menu, flags, kCmdGroupBase + i, groups[i].name.c_str());
  }

  ::AppendMenuW(menu, MF_STRING, kCmdNewTabBelow, L"New tab below");
  ::AppendMenuW(menu, MF_STRING, kCmdReload, L"Reload");
  ::AppendMenuW(menu, MF_STRING, kCmdDuplicate, L"Duplicate");
  ::AppendMenuW(menu, MF_STRING, kCmdPin, tab->pinned() ? L"Unpin tab" : L"Pin tab");
  ::AppendMenuW(menu, MF_STRING | (tab->muted() ? MF_CHECKED : 0), kCmdMute, L"Mute site");
  ::AppendMenuW(menu, MF_STRING | (tab->is_protected() ? MF_CHECKED : 0), kCmdProtect,
                L"Never put to sleep");
  const bool can_discard = tab->has_browser() && tab != active_tab();
  ::AppendMenuW(menu, MF_STRING | (can_discard ? 0 : MF_GRAYED), kCmdDiscard,
                L"Put to sleep (unload)");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_POPUP | (profile_->is_private() ? MF_GRAYED : 0),
                reinterpret_cast<UINT_PTR>(ws_menu), L"Move to space");
  ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(win_menu), L"Move to window");
  ::AppendMenuW(menu, MF_POPUP | (tab->pinned() ? MF_GRAYED : 0),
                reinterpret_cast<UINT_PTR>(group_menu), L"Add to folder");
  if (tab->record().group_id) {
    ::AppendMenuW(menu, MF_STRING, kCmdRemoveFromGroup, L"Remove from folder");
  }
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kCmdClose, L"Close\tCtrl+W");
  ::AppendMenuW(menu, MF_STRING, kCmdCloseOthers, L"Close other tabs");
  ::AppendMenuW(menu, MF_STRING, kCmdCloseBelow, L"Close tabs below");
  ::AppendMenuW(menu, MF_STRING | (profile_->is_private() ? MF_GRAYED : 0), kCmdReopen,
                L"Reopen closed tab\tCtrl+Shift+T");

  const UINT cmd = static_cast<UINT>(::TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr));
  ::DestroyMenu(menu);  // destroys submenus too
  // |tab| may have been closed by Chromium while the menu was open.
  if (workspace_->IndexOf(tab) < 0) return;

  if (cmd >= kCmdWorkspaceBase && cmd < kCmdWorkspaceBase + workspaces.size()) {
    MoveTabToWorkspace(tab, workspaces[cmd - kCmdWorkspaceBase].get());
    return;
  }
  if (cmd >= kCmdWindowBase && cmd < kCmdWindowBase + windows.size()) {
    MoveTabToWindow(tab, windows[cmd - kCmdWindowBase].get());
    return;
  }
  if (cmd >= kCmdGroupBase && cmd < kCmdGroupBase + workspace_->groups().size()) {
    workspace_->SetTabGroup(tab, workspace_->groups()[cmd - kCmdGroupBase].id);
    Layout();
    return;
  }
  if (cmd == kCmdNewGroup) {
    NewGroupWithTab(tab);
    return;
  }
  if (cmd == kCmdRemoveFromGroup) {
    workspace_->SetTabGroup(tab, 0);
    Layout();
    return;
  }
  switch (cmd) {
    case kCmdNewTabBelow:
      OpenTabNear(tab, Application::kNewTabUrl, true);
      ShowFloatingUrlBar();
      break;
    case kCmdReload:
      tab->Reload(false);
      break;
    case kCmdDuplicate:
      DuplicateTab(tab);
      break;
    case kCmdPin:
      TogglePin(tab);
      break;
    case kCmdMute:
      tab->SetMuted(!tab->muted());
      break;
    case kCmdProtect:
      tab->mutable_record().is_protected = !tab->is_protected();
      Invalidate();
      break;
    case kCmdDiscard:
      tab->Unload(/*mark_discarded=*/true);
      break;
    case kCmdMoveToNewWindow:
      if (!profile_->is_private()) {
        Workspace* ws = Application::Get()->FreeWorkspaceFor(profile_);
        if (BrowserWindow* w = Application::Get()->OpenWindow(profile_, ws, nullptr)) {
          MoveTabToWindow(tab, w);
        }
      }
      break;
    case kCmdClose:
      CloseTab(tab);
      break;
    case kCmdCloseOthers:
      CloseOtherTabs(tab);
      break;
    case kCmdCloseBelow:
      CloseTabsBelow(tab);
      break;
    case kCmdReopen:
      Application::Get()->ReopenClosedTab(this);
      break;
    default:
      break;
  }
}

void BrowserWindow::ShowWorkspaceMenu(int index, POINT pt) {
  const auto& workspaces = profile_->workspaces();
  Workspace* target = (index >= 0 && index < static_cast<int>(workspaces.size()))
                          ? workspaces[static_cast<size_t>(index)].get()
                          : workspace_;
  if (!target) return;
  HMENU menu = ::CreatePopupMenu();
  for (size_t i = 0; i < workspaces.size(); ++i) {
    UINT flags = MF_STRING | (workspaces[i].get() == workspace_ ? MF_CHECKED : 0);
    std::wstring label = workspaces[i]->record().name;
    if (workspaces[i]->window() && workspaces[i]->window() != this) label += L"  (other window)";
    ::AppendMenuW(menu, flags, kCmdWorkspaceBase + i, label.c_str());
  }
  const UINT disabled = profile_->is_private() ? MF_GRAYED : 0;
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING | disabled, kCmdNewWorkspace, L"Create space…");
  std::wstring rename = L"Rename \"" + target->record().name + L"\"…";
  ::AppendMenuW(menu, MF_STRING | disabled, kCmdRename, rename.c_str());
  HMENU colors = ::CreatePopupMenu();
  for (size_t i = 0; i < std::size(ui::kAccentChoices); ++i) {
    const UINT checked = target->record().color == ui::kAccentChoices[i] ? MF_CHECKED : 0;
    ::AppendMenuW(colors, MF_STRING | checked, kCmdColorBase + i, kColorNames[i]);
  }
  ::AppendMenuW(menu, MF_POPUP | disabled, reinterpret_cast<UINT_PTR>(colors), L"Theme color");
  ::AppendMenuW(menu, MF_STRING | disabled, kCmdMoveUp, L"Move earlier");
  ::AppendMenuW(menu, MF_STRING | disabled, kCmdMoveDown, L"Move later");
  ::AppendMenuW(menu, MF_STRING | disabled, kCmdDelete, L"Delete space…");

  const UINT cmd = static_cast<UINT>(::TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr));
  ::DestroyMenu(menu);
  if (cmd >= kCmdWorkspaceBase && cmd < kCmdWorkspaceBase + workspaces.size()) {
    SwitchWorkspace(workspaces[cmd - kCmdWorkspaceBase].get());
    return;
  }
  if (cmd >= kCmdColorBase && cmd < kCmdColorBase + std::size(ui::kAccentChoices)) {
    target->mutable_record().color = ui::kAccentChoices[cmd - kCmdColorBase];
    Application::Get()->store().UpdateWorkspace(target->record());
    if (target == workspace_) RefreshBackdrop();
    Invalidate();
    return;
  }
  const int pos = [&] {
    for (size_t i = 0; i < workspaces.size(); ++i) {
      if (workspaces[i].get() == target) return static_cast<int>(i);
    }
    return 0;
  }();
  switch (cmd) {
    case kCmdNewWorkspace:
      NewWorkspace();
      break;
    case kCmdRename:
      RenameWorkspace(target);
      break;
    case kCmdMoveUp:
      profile_->MoveWorkspace(target, pos - 1);
      Layout();
      break;
    case kCmdMoveDown:
      profile_->MoveWorkspace(target, pos + 1);
      Layout();
      break;
    case kCmdDelete:
      DeleteWorkspace(target);
      break;
    default:
      break;
  }
}

void BrowserWindow::ToggleGroup(int64_t group_id) {
  const TabGroupRecord* g = workspace_ ? workspace_->FindGroup(group_id) : nullptr;
  if (!g) return;
  TabGroupRecord updated = *g;
  updated.collapsed = !updated.collapsed;
  // Collapsing the group that holds the active tab keeps that tab selected
  // (its page stays visible); the header then stands in for it.
  workspace_->UpdateGroup(updated);
  focus_row_ = std::min(focus_row_, static_cast<int>(Rows().size()) - 1);
  Layout();
}

void BrowserWindow::NewGroupWithTab(Tab* tab) {
  if (!workspace_ || tab->pinned()) return;
  std::wstring name = L"Folder " + std::to_wstring(workspace_->groups().size() + 1);
  if (!PromptText(L"New folder", L"Folder name:", name) || name.empty()) return;
  if (workspace_->IndexOf(tab) < 0) return;  // closed while the prompt was open
  const uint32_t color =
      ui::kAccentChoices[(workspace_->groups().size() + 1) % std::size(ui::kAccentChoices)];
  workspace_->CreateGroup(name, color, {tab});
  Layout();
}

void BrowserWindow::ShowGroupMenu(int64_t group_id, POINT pt) {
  const TabGroupRecord* g = workspace_ ? workspace_->FindGroup(group_id) : nullptr;
  if (!g) return;
  HMENU menu = ::CreatePopupMenu();
  ::AppendMenuW(menu, MF_STRING, kCmdToggleGroup, g->collapsed ? L"Expand" : L"Collapse");
  ::AppendMenuW(menu, MF_STRING, kCmdRenameGroup, L"Rename…");
  HMENU colors = ::CreatePopupMenu();
  for (size_t i = 0; i < std::size(ui::kAccentChoices); ++i) {
    const UINT checked = g->color == ui::kAccentChoices[i] ? MF_CHECKED : 0;
    ::AppendMenuW(colors, MF_STRING | checked, kCmdColorBase + i, kColorNames[i]);
  }
  ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(colors), L"Color");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kCmdUngroup, L"Unpack folder");
  ::AppendMenuW(menu, MF_STRING, kCmdCloseGroup, L"Close tabs in folder");
  const UINT cmd = static_cast<UINT>(::TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr));
  ::DestroyMenu(menu);
  g = workspace_->FindGroup(group_id);  // may be gone if its tabs closed meanwhile
  if (!g) return;
  TabGroupRecord updated = *g;
  if (cmd >= kCmdColorBase && cmd < kCmdColorBase + std::size(ui::kAccentChoices)) {
    updated.color = ui::kAccentChoices[cmd - kCmdColorBase];
    workspace_->UpdateGroup(updated);
  } else if (cmd == kCmdToggleGroup) {
    ToggleGroup(group_id);
  } else if (cmd == kCmdRenameGroup) {
    std::wstring name = updated.name;
    if (PromptText(L"Rename folder", L"Folder name:", name) && !name.empty()) {
      updated.name = name;
      workspace_->UpdateGroup(updated);
    }
  } else if (cmd == kCmdUngroup) {
    workspace_->Ungroup(group_id);
  } else if (cmd == kCmdCloseGroup) {
    std::vector<Tab*> victims;
    for (const auto& t : workspace_->tabs()) {
      if (t->record().group_id == group_id) victims.push_back(t.get());
    }
    for (Tab* t : victims) t->RequestClose();
  }
  Layout();
}

void BrowserWindow::ShowProfileMenu(POINT pt) {
  Application* app = Application::Get();
  const auto& profiles = app->profiles();
  HMENU menu = ::CreatePopupMenu();
  for (size_t i = 0; i < profiles.size(); ++i) {
    const UINT flags = MF_STRING | (profiles[i].get() == profile_ ? MF_CHECKED : 0);
    std::wstring label = L"Open window: " + profiles[i]->record().name;
    ::AppendMenuW(menu, flags, kCmdProfileBase + i, label.c_str());
  }
  const UINT disabled = profile_->is_private() ? MF_GRAYED : 0;
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kCmdNewProfile, L"New profile…");
  ::AppendMenuW(menu, MF_STRING | disabled, kCmdRenameProfile, L"Rename this profile…");
  HMENU colors = ::CreatePopupMenu();
  for (size_t i = 0; i < std::size(ui::kAccentChoices); ++i) {
    const UINT checked = profile_->record().color == ui::kAccentChoices[i] ? MF_CHECKED : 0;
    wchar_t label[32];
    swprintf_s(label, L"Color %zu", i + 1);
    ::AppendMenuW(colors, MF_STRING | checked, kCmdColorBase + i, label);
  }
  ::AppendMenuW(menu, MF_POPUP | disabled, reinterpret_cast<UINT_PTR>(colors), L"Profile color");
  ::AppendMenuW(menu, MF_STRING | disabled | (profiles.size() <= 1 ? MF_GRAYED : 0),
                kCmdDeleteProfile, L"Delete this profile…");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kCmdNewPrivateWindow, L"New private window\tCtrl+Shift+N");

  const UINT cmd = static_cast<UINT>(::TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr));
  ::DestroyMenu(menu);
  if (cmd >= kCmdProfileBase && cmd < kCmdProfileBase + profiles.size()) {
    Profile* target = profiles[cmd - kCmdProfileBase].get();
    app->OpenWindow(target, app->FreeWorkspaceFor(target), nullptr);
    return;
  }
  if (cmd >= kCmdColorBase && cmd < kCmdColorBase + std::size(ui::kAccentChoices)) {
    profile_->mutable_record().color = ui::kAccentChoices[cmd - kCmdColorBase];
    app->store().UpdateProfile(profile_->record());
    Invalidate();
    return;
  }
  switch (cmd) {
    case kCmdNewProfile: {
      std::wstring name = L"Profile " + std::to_wstring(profiles.size() + 1);
      if (!PromptText(L"New profile", L"Profile name:", name) || name.empty()) break;
      const uint32_t color = ui::kAccentChoices[profiles.size() % std::size(ui::kAccentChoices)];
      if (Profile* p = app->CreateProfile(name, color)) {
        app->OpenWindow(p, app->FreeWorkspaceFor(p), nullptr);
      }
      break;
    }
    case kCmdRenameProfile: {
      std::wstring name = profile_->record().name;
      if (!PromptText(L"Rename profile", L"Profile name:", name) || name.empty()) break;
      profile_->mutable_record().name = name;
      app->store().UpdateProfile(profile_->record());
      Invalidate();
      break;
    }
    case kCmdDeleteProfile: {
      const std::wstring msg =
          L"Delete profile \"" + profile_->record().name +
          L"\"?\n\nAll of its windows will close, and its cookies, site data, history, "
          L"bookmarks and workspaces will be permanently deleted.";
      if (::MessageBoxW(hwnd_, msg.c_str(), L"Delete profile", MB_OKCANCEL | MB_ICONWARNING) ==
          IDOK) {
        app->DeleteProfile(profile_);  // closes this window
      }
      break;
    }
    case kCmdNewPrivateWindow:
      app->OpenPrivateWindow();
      break;
    default:
      break;
  }
}

void BrowserWindow::ShowMainMenu(POINT pt) {
  Tab* tab = active_tab();
  HMENU menu = ::CreatePopupMenu();
  ::AppendMenuW(menu, MF_STRING, kCmdNewTab, L"New tab\tCtrl+T");
  ::AppendMenuW(menu, MF_STRING, kCmdNewWindow, L"New window\tCtrl+N");
  ::AppendMenuW(menu, MF_STRING, kCmdNewPrivateWindow, L"New private window\tCtrl+Shift+N");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kCmdBookmarks, L"Bookmarks\tCtrl+Shift+O");
  ::AppendMenuW(menu, MF_STRING, kCmdHistory, L"History\tCtrl+H");
  ::AppendMenuW(menu, MF_STRING, kCmdDownloads, L"Downloads\tCtrl+J");
  std::wstring profiles = L"Profile: " + profile_->record().name + L"…";
  ::AppendMenuW(menu, MF_STRING, kCmdProfiles, profiles.c_str());
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kCmdZoomIn, L"Zoom in\tCtrl++");
  ::AppendMenuW(menu, MF_STRING, kCmdZoomOut, L"Zoom out\tCtrl+-");
  ::AppendMenuW(menu, MF_STRING, kCmdZoomReset, L"Reset zoom\tCtrl+0");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING | (compact_mode_ ? MF_CHECKED : 0), kCmdCompactMode,
                L"Compact mode\tCtrl+Shift+C");
  ::AppendMenuW(menu, MF_STRING | (collapsed_ ? MF_CHECKED : 0), kCmdToggleSidebar,
                L"Collapse sidebar\tCtrl+Shift+S");
  ::AppendMenuW(menu, MF_STRING | (tab && tab->browser() ? 0 : MF_GRAYED), kCmdDevTools,
                L"Developer tools\tF12");
  ::AppendMenuW(menu, MF_STRING, kCmdSettings, L"Settings");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit");

  const UINT cmd = static_cast<UINT>(::TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr));
  ::DestroyMenu(menu);
  tab = active_tab();
  switch (cmd) {
    case kCmdNewTab:
      NewTab();
      break;
    case kCmdNewWindow:
      OpenNormalWindow();
      break;
    case kCmdNewPrivateWindow:
      Application::Get()->OpenPrivateWindow();
      break;
    case kCmdBookmarks:
      OpenInternalPage(kPageBookmarks);
      break;
    case kCmdHistory:
      OpenInternalPage(kPageHistory);
      break;
    case kCmdDownloads:
      OpenInternalPage(kPageDownloads);
      break;
    case kCmdSettings:
      OpenInternalPage(kPageSettings);
      break;
    case kCmdProfiles:
      ShowProfileMenu(pt);
      break;
    case kCmdZoomIn:
      if (tab) tab->Zoom(CEF_ZOOM_COMMAND_IN);
      break;
    case kCmdZoomOut:
      if (tab) tab->Zoom(CEF_ZOOM_COMMAND_OUT);
      break;
    case kCmdZoomReset:
      if (tab) tab->Zoom(CEF_ZOOM_COMMAND_RESET);
      break;
    case kCmdCompactMode:
      ToggleCompactMode();
      break;
    case kCmdToggleSidebar:
      ToggleCompact();
      break;
    case kCmdDevTools:
      HandleAccelerator('I', true, true, false);
      break;
    case kCmdExit:
      Application::Get()->CloseAllWindows();
      break;
    default:
      break;
  }
}

void BrowserWindow::ShowCreateMenu(POINT pt) {
  // Zen's "+" in the sidebar footer.
  HMENU menu = ::CreatePopupMenu();
  const UINT disabled = profile_->is_private() ? MF_GRAYED : 0;
  ::AppendMenuW(menu, MF_STRING, kCmdNewTab, L"New tab\tCtrl+T");
  ::AppendMenuW(menu, MF_STRING | (active_tab() && !active_tab()->pinned() ? 0 : MF_GRAYED),
                kCmdNewTabGroup, L"New folder…");
  ::AppendMenuW(menu, MF_STRING | disabled, kCmdNewWorkspace, L"Create space…");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kCmdNewWindow, L"New window\tCtrl+N");
  ::AppendMenuW(menu, MF_STRING, kCmdNewPrivateWindow, L"New private window\tCtrl+Shift+N");
  const UINT cmd = static_cast<UINT>(::TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_BOTTOMALIGN, pt.x, pt.y, 0,
      hwnd_, nullptr));
  ::DestroyMenu(menu);
  switch (cmd) {
    case kCmdNewTab:
      NewTab();
      break;
    case kCmdNewTabGroup:
      if (Tab* tab = active_tab()) NewGroupWithTab(tab);
      break;
    case kCmdNewWorkspace:
      NewWorkspace();
      break;
    case kCmdNewWindow:
      OpenNormalWindow();
      break;
    case kCmdNewPrivateWindow:
      Application::Get()->OpenPrivateWindow();
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Tooltips

void BrowserWindow::UpdateTooltip(const Hit& hit) {
  if (!tooltip_ || hit == tooltip_hit_) return;
  tooltip_hit_ = hit;
  ::SendMessageW(tooltip_, TTM_POP, 0, 0);
  // Shrink the tool to the hovered element so moving to another element
  // re-arms the tooltip delay.
  D2D1_RECT_F r{};
  switch (hit.kind) {
    case HitKind::kBack: r = back_; break;
    case HitKind::kForward: r = forward_; break;
    case HitKind::kReload: r = reload_; break;
    case HitKind::kSiteInfo: r = site_info_; break;
    case HitKind::kStar: r = star_; break;
    case HitKind::kMenu: r = menu_; break;
    case HitKind::kSidebarToggle: r = toggle_; break;
    case HitKind::kWorkspaceIndicator: r = ws_indicator_; break;
    case HitKind::kWorkspaceActions: r = ws_actions_; break;
    case HitKind::kWorkspace: {
      const size_t i = static_cast<size_t>(hit.index - first_dot_workspace_);
      if (i < workspace_dots_.size()) r = workspace_dots_[i];
      break;
    }
    case HitKind::kCreateNew: r = create_new_; break;
    case HitKind::kDownloads: r = downloads_; break;
    case HitKind::kNewTab: r = NewTabRect(); break;
    case HitKind::kClearTabs: r = ClearRect(); break;
    case HitKind::kTab:
    case HitKind::kTabClose: r = TabRowRect(hit.index); break;
    case HitKind::kGroup: {
      for (const Row& row : Rows()) {
        if (row.is_group && GroupIndex(row.group_id) == hit.index) r = RowRect(row);
      }
      break;
    }
    default: break;
  }
  TTTOOLINFOW ti{sizeof(ti)};
  ti.hwnd = hwnd_;
  ti.uId = 1;
  ti.rect = ToPx(r);
  ::SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
}

std::wstring BrowserWindow::TooltipText() const {
  switch (tooltip_hit_.kind) {
    case HitKind::kBack: return L"Back (Alt+Left)";
    case HitKind::kForward: return L"Forward (Alt+Right)";
    case HitKind::kReload: {
      Tab* tab = active_tab();
      return tab && tab->loading() ? L"Stop loading" : L"Reload (Ctrl+R)";
    }
    case HitKind::kSiteInfo: return L"View site information";
    case HitKind::kStar: return bookmarked_ ? L"Remove bookmark (Ctrl+D)" : L"Bookmark this page (Ctrl+D)";
    case HitKind::kMenu: return L"Menu";
    case HitKind::kSidebarToggle:
      return compact_mode_ ? L"Turn off compact mode (Ctrl+Shift+C)"
                           : L"Compact mode: hide the sidebar (Ctrl+Shift+C)";
    case HitKind::kWorkspaceIndicator:
    case HitKind::kWorkspaceActions:
      return workspace_ ? L"Space: " + workspace_->record().name : L"";
    case HitKind::kWorkspace: {
      if (collapsed_) return workspace_ ? L"Space: " + workspace_->record().name : L"";
      const auto& ws = profile_->workspaces();
      const size_t i = static_cast<size_t>(tooltip_hit_.index);
      return i < ws.size() ? ws[i]->record().name : L"";
    }
    case HitKind::kCreateNew: return L"Create new…";
    case HitKind::kDownloads: {
      const int active = Application::Get()->downloads().ActiveCount(profile_);
      if (active > 0) {
        const int pct = Application::Get()->downloads().ActivePercent(profile_);
        std::wstring text = std::to_wstring(active) + L" download(s) in progress";
        if (pct >= 0) text += L" — " + std::to_wstring(pct) + L"%";
        return text;
      }
      return L"Downloads (Ctrl+J)";
    }
    case HitKind::kNewTab: return L"New tab (Ctrl+T)";
    case HitKind::kClearTabs: return L"Close all unpinned tabs";
    case HitKind::kTab: {
      if (!workspace_) return L"";
      const size_t i = static_cast<size_t>(tooltip_hit_.index);
      if (i >= workspace_->tabs().size()) return L"";
      Tab* t = workspace_->tabs()[i].get();
      std::wstring text = t->DisplayTitle();
      if (t->is_discarded()) text += L"\n(Sleeping — reloads when selected)";
      return text;
    }
    case HitKind::kTabClose: return L"Close tab (Ctrl+W)";
    case HitKind::kGroup: {
      if (!workspace_ || tooltip_hit_.index < 0) return L"";
      const auto& g = workspace_->groups()[static_cast<size_t>(tooltip_hit_.index)];
      return g.name + L" — " + std::to_wstring(workspace_->GroupSize(g.id)) +
             L" tab(s). Click to " + (g.collapsed ? L"expand" : L"collapse") + L".";
    }
    default: return L"";
  }
}

void BrowserWindow::SetCompact(bool compact) {
  if (collapsed_ == compact) return;
  collapsed_ = compact;
  if (::GetFocus() == address_ && !popup_visible_) FocusContent();
  Layout();
}

void BrowserWindow::SetCompactMode(bool hidden) {
  if (compact_mode_ == hidden) return;
  compact_mode_ = hidden;
  sidebar_revealed_ = false;
  if (::GetFocus() == address_ && !popup_visible_) FocusContent();
  Layout();
}

void BrowserWindow::ToggleCompact() {
  Application::Get()->store().SetPref(Store::kGlobalScope, L"sidebar_compact",
                                      collapsed_ ? L"0" : L"1");
  Application::Get()->ApplySidebarModeToAllWindows(!collapsed_);
}

void BrowserWindow::ToggleCompactMode() {
  Application::Get()->store().SetPref(Store::kGlobalScope, L"sidebar_hidden",
                                      compact_mode_ ? L"0" : L"1");
  Application::Get()->ApplyCompactModeToAllWindows(!compact_mode_);
}

void BrowserWindow::OnDownloadsChanged() { Invalidate(); }

void BrowserWindow::OnBookmarksChanged() {
  RefreshBookmarkState();
  Invalidate();
}

void BrowserWindow::RefreshBookmarkState() {
  Tab* tab = active_tab();
  bookmarked_ = tab && !profile_->is_private() && !IsNewTabUrl(tab->url()) &&
                Application::Get()->store().FindBookmarkByUrl(profile_->id(), tab->url());
}

void BrowserWindow::ToggleBookmark() {
  Tab* tab = active_tab();
  if (!tab || profile_->is_private() || IsNewTabUrl(tab->url())) return;
  Store& store = Application::Get()->store();
  if (auto existing = store.FindBookmarkByUrl(profile_->id(), tab->url())) {
    store.DeleteBookmark(profile_->id(), existing->id);
  } else {
    BookmarkRecord b;
    b.profile_id = profile_->id();
    b.title = tab->DisplayTitle();
    b.url = tab->url();
    store.AddBookmark(b);
  }
  for (const auto& w : Application::Get()->windows()) {
    if (w->profile_ == profile_) w->OnBookmarksChanged();
  }
}

// ---------------------------------------------------------------------------
// Accessibility

std::vector<AccessibleItem> BrowserWindow::AccessibleItems() const {
  std::vector<AccessibleItem> items;
  if (!hwnd_ || chrome_hidden()) return items;
  using Role = AccessibleItem::Role;
  auto add = [&](HitKind kind, intptr_t key, Role role, std::wstring name, D2D1_RECT_F rect,
                 bool enabled = true) {
    if (rect.right <= rect.left) return;
    AccessibleItem item;
    item.kind = static_cast<int>(kind);
    item.key = key;
    item.role = role;
    item.name = std::move(name);
    item.rect_dip = rect;
    item.enabled = enabled;
    items.push_back(std::move(item));
  };
  Tab* tab = active_tab();
  // Sidebar top row (the address bar is a native EDIT and is exposed by Windows).
  add(HitKind::kMenu, 0, Role::kButton, L"Menu", menu_);
  add(HitKind::kSidebarToggle, 0, Role::kButton,
      compact_mode_ ? L"Turn off compact mode" : L"Compact mode", toggle_);
  add(HitKind::kBack, 0, Role::kButton, L"Back", back_, tab && tab->can_go_back());
  add(HitKind::kForward, 0, Role::kButton, L"Forward", forward_, tab && tab->can_go_forward());
  add(HitKind::kReload, 0, Role::kButton, tab && tab->loading() ? L"Stop" : L"Reload", reload_,
      tab != nullptr);
  std::wstring site = L"Site information";
  if (tab) {
    switch (tab->security()) {
      case SecurityLevel::kSecure: site += L": connection is secure"; break;
      case SecurityLevel::kInsecure: site += L": not secure"; break;
      case SecurityLevel::kDangerous: site += L": dangerous"; break;
      case SecurityLevel::kInternal: site += L": Orbit page"; break;
      case SecurityLevel::kNone: break;
    }
  }
  add(HitKind::kSiteInfo, 0, Role::kButton, site, site_info_);
  add(HitKind::kStar, 0, Role::kButton, bookmarked_ ? L"Remove bookmark" : L"Bookmark this page",
      star_);
  if (workspace_) {
    add(HitKind::kWorkspaceIndicator, 0, Role::kButton, L"Space: " + workspace_->record().name,
        ws_indicator_);
  }
  add(HitKind::kNewTab, 0, Role::kButton, L"New tab", NewTabRect());
  add(HitKind::kClearTabs, 0, Role::kButton, L"Close all unpinned tabs", ClearRect());
  // Tabs and groups, in sidebar order.
  const bool sidebar_focus = ::GetFocus() == hwnd_;
  const auto rows = sidebar_visible() ? Rows() : std::vector<Row>{};
  for (size_t r = 0; r < rows.size(); ++r) {
    const Row& row = rows[r];
    const D2D1_RECT_F rect = RowRect(row);
    if (row.is_group) {
      const TabGroupRecord* g = workspace_->FindGroup(row.group_id);
      if (!g) continue;
      add(HitKind::kGroup, static_cast<intptr_t>(row.group_id), Role::kGroup,
          L"Folder " + g->name + L", " + std::to_wstring(workspace_->GroupSize(g->id)) +
              L" tabs",
          rect);
      items.back().expanded = !g->collapsed;
    } else {
      Tab* t = workspace_->tabs()[static_cast<size_t>(row.tab)].get();
      std::wstring name = t->DisplayTitle();
      if (t->pinned()) name += L", pinned";
      if (t->loading()) name += L", loading";
      if (t->is_discarded()) name += L", sleeping";
      if (t->muted()) name += L", muted";
      if (t->media_capture()) name += L", using camera or microphone";
      add(HitKind::kTab, reinterpret_cast<intptr_t>(t), Role::kTab, std::move(name), rect);
      items.back().selected = t == tab;
    }
    items.back().focused = sidebar_focus && focus_row_ == static_cast<int>(r);
  }
  // Footer.
  add(HitKind::kDownloads, 0, Role::kButton, L"Downloads", downloads_);
  const auto& workspaces = profile_->workspaces();
  for (size_t i = 0; i < workspace_dots_.size(); ++i) {
    const size_t w = static_cast<size_t>(first_dot_workspace_) + i;
    if (w >= workspaces.size()) break;
    Workspace* ws = workspaces[w].get();
    add(HitKind::kWorkspace, reinterpret_cast<intptr_t>(ws), Role::kButton,
        L"Space: " + ws->record().name, workspace_dots_[i]);
    items.back().selected = ws == workspace_;
  }
  add(HitKind::kCreateNew, 0, Role::kButton, L"Create new", create_new_);
  return items;
}

void BrowserWindow::AccessibleInvoke(int kind, intptr_t key) {
  ::PostMessageW(hwnd_, kAccessibleInvokeMessage, static_cast<WPARAM>(kind),
                 static_cast<LPARAM>(key));
}

void BrowserWindow::HandleAccessibleInvoke(int kind, intptr_t key) {
  // Resolve the identity again: the item may have changed since the post.
  const auto k = static_cast<HitKind>(kind);
  Hit hit{k, 0};
  switch (k) {
    case HitKind::kTab:
      hit.index = workspace_ ? workspace_->IndexOf(reinterpret_cast<Tab*>(key)) : -1;
      break;
    case HitKind::kGroup:
      hit.index = GroupIndex(static_cast<int64_t>(key));
      break;
    case HitKind::kWorkspace: {
      hit.index = -1;
      const auto& ws = profile_->workspaces();
      for (size_t i = 0; i < ws.size(); ++i) {
        if (reinterpret_cast<intptr_t>(ws[i].get()) == key) hit.index = static_cast<int>(i);
      }
      break;
    }
    default:
      break;
  }
  if (hit.index < 0) return;
  Click(hit);
}

void BrowserWindow::AccessibleFocus(int kind, intptr_t key) {
  const auto rows = Rows();
  for (size_t r = 0; r < rows.size(); ++r) {
    const bool match =
        (kind == static_cast<int>(HitKind::kGroup) && rows[r].is_group &&
         rows[r].group_id == static_cast<int64_t>(key)) ||
        (kind == static_cast<int>(HitKind::kTab) && !rows[r].is_group &&
         reinterpret_cast<intptr_t>(workspace_->tabs()[static_cast<size_t>(rows[r].tab)].get()) ==
             key);
    if (match) {
      focus_row_ = static_cast<int>(r);
      ::SetFocus(hwnd_);
      Invalidate();
      NotifyAccessibleFocus();
      return;
    }
  }
}

void BrowserWindow::NotifyAccessibleFocus() {
  if (!accessibility_ || ::GetFocus() != hwnd_) return;
  for (const auto& item : AccessibleItems()) {
    if (item.focused) {
      accessibility_->NotifyFocus(item.kind, item.key);
      return;
    }
  }
}

void BrowserWindow::OpenNormalWindow() {
  Application* app = Application::Get();
  Profile* target = profile_->is_private() ? app->profiles().front().get() : profile_;
  app->OpenWindow(target, app->FreeWorkspaceFor(target), nullptr);
}

void BrowserWindow::OpenInternalPage(int page) {
  static constexpr const wchar_t* kPages[] = {L"orbit://downloads/", L"orbit://bookmarks/",
                                              L"orbit://history/", L"orbit://settings/"};
  const std::wstring url = kPages[page];
  // Reuse an existing tab showing the page instead of piling up duplicates.
  for (const auto& t : workspace_->tabs()) {
    if (t->url().rfind(url, 0) == 0) {
      ActivateTab(t.get());
      return;
    }
  }
  OpenTab(url, true);
}

void BrowserWindow::UpdateSpinnerTimer() {
  bool any_loading = false;
  if (workspace_) {
    for (const auto& t : workspace_->tabs()) any_loading = any_loading || t->loading();
  }
  if (any_loading && !spinner_running_) {
    ::SetTimer(hwnd_, kSpinnerTimer, kSpinnerIntervalMs, nullptr);
    spinner_running_ = true;
  } else if (!any_loading && spinner_running_) {
    ::KillTimer(hwnd_, kSpinnerTimer);
    spinner_running_ = false;
  }
}

void BrowserWindow::Invalidate() {
  if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace orbit
