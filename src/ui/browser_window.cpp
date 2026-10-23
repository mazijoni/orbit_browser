#include "ui/browser_window.h"

#include "browser/library_client.h"

#include <commctrl.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cwctype>

#include "app/application.h"
#include "app/installer.h"
#include "app/resource.h"
#include "browser/downloads.h"
#include "browser/profile.h"
#include "browser/tab.h"
#include "browser/browser_client.h"
#include "browser/extension_bridge.h"
#include "browser/extension_popup.h"
#include "browser/find_bar.h"
#include "app/orbit_app.h"
#include "browser/boost_apply.h"
#include "include/cef_parser.h"
#include "browser/text_fetch.h"
#include "core/live_folders.h"
#include "core/log.h"
#include "core/strings.h"
#include "core/url_fixup.h"
#include "include/cef_client.h"
#include "ui/graphics.h"
#include "ui/menu.h"
#include "app/self_test.h"
#include "ui/share.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace orbit {
namespace {

constexpr wchar_t kWindowClass[] = L"OrbitBrowserWindow";
constexpr wchar_t kPopupClass[] = L"OrbitFloatingUrlBar";
constexpr wchar_t kOverlayClass[] = L"OrbitCompactSidebar";
constexpr wchar_t kGlanceDimClass[] = L"OrbitGlanceBackground";
constexpr wchar_t kGlanceBarClass[] = L"OrbitGlanceButtons";
constexpr wchar_t kLibraryHostClass[] = L"OrbitLibrary";
constexpr wchar_t kSiteDataClass[] = L"OrbitSiteData";
constexpr uint32_t kLibrarySlideMs = 320;  // spring (stiffness 720, damping 47, mass 1.2)
// .zen-glance-sidebar-container: 12px padding and gap, 34px round buttons.
constexpr float kGlancePad = 12.f;
constexpr float kGlanceButton = 34.f;
constexpr UINT_PTR kSpinnerTimer = 1;
constexpr UINT_PTR kCaptionHideTimer = 2;
constexpr UINT_PTR kSidebarHideTimer = 3;
constexpr UINT_PTR kAnimTimer = 4;
constexpr UINT kSpinnerIntervalMs = 60;
constexpr UINT kAnimFrameMs = 15;
constexpr uint32_t kSpaceSwitchMs = 250;  // zen.workspaces.switch-animation-duration
constexpr UINT_PTR kMediaCardTimer = 5;
constexpr uint32_t kCompactSlideMs = 100;  // ZenCompactMode.animateCompactMode (0.1s)
constexpr uint32_t kFolderAnimMs = 180;    // ZenFolders #folderAnimationDuration
constexpr uint32_t kGlanceAnimMs = 350;    // zen.glance.animation-duration
constexpr UINT kCompactFlashMs = 800;      // zen.view.compact.toolbar-flash-popup.duration
constexpr UINT_PTR kSwipeEndTimer = 9;
constexpr UINT kSwipeEndMs = 120;          // no more wheel events: the fingers lifted
constexpr float kSwipePxPerNotch = 70.f;   // one wheel notch drags this far (DIPs)
constexpr float kSwipeSuccess = 0.25f;     // ZenSpacesSwipe.SUCCESS_THRESHOLD
constexpr uint32_t kCaptionRevealMs = 150;  // --zen-hidden-toolbar-transition
// zen.view.compact.toolbar-hide-after-hover.duration / sidebar-keep-hover.duration
constexpr UINT kCaptionHideDelayMs = 1000;
constexpr UINT kSidebarHideDelayMs = 150;
constexpr int kAddressId = 100;
constexpr UINT kAccessibleInvokeMessage = WM_APP + 1;  // wParam: kind, lParam: key
constexpr UINT kHidePopupMessage = WM_APP + 2;
constexpr UINT kCancelRenameMessage = WM_APP + 4;
constexpr float kInner = 7.f;            // sidebar content inset (toolbox padding + tab margin)
constexpr float kButton = 28.f;          // sidebar toolbar button
constexpr float kFolderIndent = 12.f;    // --zen-folder-indent
constexpr float kFloatingBarHeight = 56.f;
// ZenUIManager.updateTabsToolbar: width min(innerWidth / 1.5, 750); the bar is
// centered vertically as if it were at least 333 px tall (room for results).
constexpr float kFloatingBarMaxWidth = 750.f;
constexpr float kFloatingBarMinLayoutHeight = 333.f;
constexpr ULONGLONG kNewTabSearchKeepMs = 45000;
constexpr float kBreakoutBarHeight = 44.f;
constexpr float kBreakoutMaxWidth = 640.f;  // :root[zen-single-toolbar] #urlbar[open] width: 40rem
constexpr float kResultRowHeight = 45.f;    // --urlbarview-row-min-height
constexpr float kResultsPadding = 12.f;     // --urlbarView-results-padding

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
  kCmdManageSpaces,
  kCmdCompactHideSidebar,
  kCmdCompactHideToolbar,
  kCmdCompactHideBoth,
  kCmdEditTheme,
  kCmdSpaceRouting,
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
  kCmdExtensions,
  kCmdDefaultBrowser,
  kCmdSettings,
  kCmdClearTabs,
  kCmdNewTabGroup,
  kCmdResetPinned,
  kCmdReplacePinnedUrl,
  kCmdEditPinnedUrl,
  kCmdChangeLabel,
  kCmdAddEssential,
  kCmdRemoveEssential,
  kCmdChangeIcon,
  kCmdUnloadSpace,
  kCmdUnloadOtherSpaces,
  kCmdSplitTabs,
  kCmdSplitOut,
  kCmdNewSplit,
  kCmdTabsRight,
  kCmdFolderIcon,
  kCmdFolderUnload,
  kCmdSubfolder,
  kCmdFolderToSpace,
  kCmdSpaceNewProfile,
  kCmdLinkedShow,
  kCmdLinkedBringBack,
  kCmdLinkedClose,
  kCmdWorkspaceBase = 1000,   // + workspace index
  kCmdWindowBase = 2000,      // + window index
  kCmdProfileBase = 3000,     // + profile index
  kCmdColorBase = 4000,       // + accent index
  kCmdGroupBase = 5000,       // + group index
  kCmdFolderSpaceBase = 6000, // + workspace index (Change Space)
  // Live folders: 7000.. options, 7100.. repositories, 7900.. "New Live Folder".
  kCmdLiveRefresh = 7000,
  kCmdLiveFeedUrl,
  kCmdLiveAuthorMe,
  kCmdLiveAssignedMe,
  kCmdLiveReviewRequested,
  kCmdLiveIncludeDrafts,
  kCmdLiveIntervalBase = 7010,   // + index into kLiveIntervals
  kCmdLiveItemLimitBase = 7020,  // + index into kLiveItemLimits
  kCmdLiveTimeRangeBase = 7030,  // + index into kLiveTimeRanges
  kCmdLiveRepoBase = 7100,       // + index into the sorted repositories
  kCmdLiveRepoEnd = 7800,
  kCmdNewLivePulls = 7900,
  kCmdNewLiveIssues,
  kCmdNewLiveRss,
  kCmdSpaceProfileBase = 8000,  // + profile index ("Profile ▸" in the space menu)
};

// Tabs move between normal profiles, and within one private session.
bool CanShareTabs(const Profile* a, const Profile* b) {
  return a == b || (!a->is_private() && !b->is_private());
}

constexpr const wchar_t* kColorNames[] = {L"Periwinkle", L"Teal", L"Amber", L"Rose",
                                          L"Violet",     L"Sky",  L"Moss",  L"Gold"};

bool IsNewTabUrl(const std::wstring& url) {
  return url.empty() || url == Application::kNewTabUrl;
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
  if (auto w = store.GetPref(Store::kGlobalScope, L"devtools_width")) {
    devtools_width_ = std::clamp(static_cast<float>(_wtof(w->c_str())), 240.f, 2000.f);
  }
  compact_mode_ = store.GetPref(Store::kGlobalScope, L"sidebar_hidden") == L"1";
}

BrowserWindow::~BrowserWindow() {
  toast_.reset();
  if (popup_) {
    ::SetWindowLongPtrW(popup_, GWLP_USERDATA, 0);
    ::DestroyWindow(popup_);
  }
  if (overlay_) ::SetWindowLongPtrW(overlay_, GWLP_USERDATA, 0);  // destroyed with hwnd_
  if (glance_dim_) ::SetWindowLongPtrW(glance_dim_, GWLP_USERDATA, 0);
  if (glance_bar_) ::SetWindowLongPtrW(glance_bar_, GWLP_USERDATA, 0);
  if (library_client_) library_client_->Detach();
  if (site_client_) site_client_->Detach();
  if (site_popup_) {
    ::SetWindowLongPtrW(site_popup_, GWLP_USERDATA, 0);
    ::DestroyWindow(site_popup_);
  }
  if (theme_client_) theme_client_->Detach();
  if (theme_popup_) {
    ::SetWindowLongPtrW(theme_popup_, GWLP_USERDATA, 0);
    ::DestroyWindow(theme_popup_);
  }
  if (boost_client_) boost_client_->Detach();
  if (boost_popup_) {
    ::SetWindowLongPtrW(boost_popup_, GWLP_USERDATA, 0);
    ::DestroyWindow(boost_popup_);
  }
  if (hwnd_) {
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    ::DestroyWindow(hwnd_);
  }
  if (address_font_) ::DeleteObject(address_font_);
  if (popup_font_) ::DeleteObject(popup_font_);
  if (rename_font_) ::DeleteObject(rename_font_);
  if (rename_brush_) ::DeleteObject(rename_brush_);
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
    WNDCLASSEXW oc{sizeof(oc)};
    oc.style = CS_DBLCLKS;
    oc.lpfnWndProc = &BrowserWindow::OverlayProc;
    oc.hInstance = ModuleInstance();
    oc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    oc.lpszClassName = kOverlayClass;
    if (!::RegisterClassExW(&oc)) return false;
    WNDCLASSEXW gd{sizeof(gd)};
    gd.lpfnWndProc = &BrowserWindow::GlanceDimProc;
    gd.hInstance = ModuleInstance();
    gd.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    gd.lpszClassName = kGlanceDimClass;
    if (!::RegisterClassExW(&gd)) return false;
    WNDCLASSEXW gb{sizeof(gb)};
    gb.lpfnWndProc = &BrowserWindow::GlanceBarProc;
    gb.hInstance = ModuleInstance();
    gb.hCursor = ::LoadCursorW(nullptr, IDC_HAND);
    gb.lpszClassName = kGlanceBarClass;
    if (!::RegisterClassExW(&gb)) return false;
    WNDCLASSEXW lc{sizeof(lc)};
    lc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
      if (msg == WM_SIZE) {
        // Keep the page filling the panel.
        if (HWND child = ::GetWindow(hwnd, GW_CHILD)) {
          ::SetWindowPos(child, nullptr, 0, 0, LOWORD(lp), HIWORD(lp), SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
      }
      if (msg == WM_ERASEBKGND) return 1;
      return ::DefWindowProcW(hwnd, msg, wp, lp);
    };
    lc.hInstance = ModuleInstance();
    lc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    lc.lpszClassName = kLibraryHostClass;
    if (!::RegisterClassExW(&lc)) return false;
    WNDCLASSEXW sd{sizeof(sd)};
    sd.style = CS_DROPSHADOW;
    sd.lpfnWndProc = &BrowserWindow::SiteDataProc;
    sd.hInstance = ModuleInstance();
    sd.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    sd.lpszClassName = kSiteDataClass;
    if (!::RegisterClassExW(&sd)) return false;
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
  toast_ = std::make_unique<ui::Toast>(hwnd_);
  overlay_ = ::CreateWindowExW(0, kOverlayClass, L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                               0, 0, 0, 0, hwnd_, nullptr, ModuleInstance(), this);

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
  rename_edit_ = ::CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 0, 0,
                                   hwnd_, nullptr, ModuleInstance(), nullptr);
  ::SetWindowSubclass(rename_edit_, &BrowserWindow::RenameProc, 2,
                      reinterpret_cast<DWORD_PTR>(this));
  ::SendMessageW(rename_edit_, EM_LIMITTEXT, 200, 0);

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
  palette_ = BasePalette();
  {
    Store& store = app->store();
    auto on = [&](const wchar_t* key, bool fallback) {
      const auto v = store.GetPref(Store::kGlobalScope, key);
      return v ? *v == L"1" : fallback;
    };
    newtab_button_ = on(L"newtab_button", true);
    newtab_button_top_ = on(L"newtab_button_top", true);
    ctrl_tab_skip_pending_ = on(L"ctrl_tab_skip_pending", false);
    grey_out_inactive_ = on(L"grey_out_inactive", true);
    urlbar_wide_ = on(L"urlbar_wide", false);
    right_side_ = on(L"tabs_right", false);
    multi_toolbar_ = !on(L"single_toolbar", true);  // zen.view.use-single-toolbar
    compact_hide_sidebar_ = on(L"compact_hide_sidebar", true);   // zen.view.compact.hide-tabbar
    compact_hide_toolbar_ = on(L"compact_hide_toolbar", false);  // zen.view.compact.hide-toolbar
    if (!compact_hide_sidebar_ && !compact_hide_toolbar_) compact_hide_sidebar_ = true;
    const std::wstring behavior =
        store.GetPref(Store::kGlobalScope, L"urlbar_behavior").value_or(L"floating-on-type");
    urlbar_behavior_ = behavior == L"normal" || behavior == L"float" ? behavior : L"floating-on-type";
  }
  theme_strength_ = 0.45f;
  if (auto s = app->store().GetPref(Store::kGlobalScope, L"theme_strength")) {
    theme_strength_ = std::clamp(static_cast<float>(_wtof(s->c_str())), 0.f, 1.f);
  }
  if (profile_->is_private()) theme_strength_ = 0.f;
  const BOOL dark = palette_.dark ? TRUE : FALSE;
  ::DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  if (popup_) ::DwmSetWindowAttribute(popup_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  if (address_font_) ::DeleteObject(address_font_);
  address_font_ = ::CreateFontW(-static_cast<int>(std::lround(px(13.5f))), 0, 0, 0, FW_MEDIUM,
                                FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                ui::Graphics::Get().ui_font_family());
  if (rename_font_) ::DeleteObject(rename_font_);
  rename_font_ = ::CreateFontW(-static_cast<int>(std::lround(px(13.5f))), 0, 0, 0, FW_NORMAL, FALSE,
                               FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                               ui::Graphics::Get().ui_font_family());
  if (rename_edit_) ::SendMessageW(rename_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(rename_font_), TRUE);
  if (popup_font_) ::DeleteObject(popup_font_);
  popup_font_ = ::CreateFontW(-static_cast<int>(std::lround(px(16.f))), 0, 0, 0, FW_NORMAL,
                              FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                              ui::Graphics::Get().ui_font_family());
  ::SendMessageW(address_, WM_SETFONT,
                 reinterpret_cast<WPARAM>(popup_visible_ ? popup_font_ : address_font_), TRUE);
  favicons_.clear();
  ext_icons_.clear();
  popup_rt_.Reset();
  popup_brush_d2d_.Reset();
  RefreshBackdrop();
  Layout();
}

ui::Palette BrowserWindow::BasePalette() const {
  Application* app = Application::Get();
  if (profile_->is_private()) {
    // Zen's private windows: a near-black, purple-tinted surface, always dark.
    ui::Palette p = ui::MakePalette(true, profile_->record().color);
    p.chrome_bg = ui::ColorFromRgb(0x0F0D12);
    return p;
  }
  return ui::MakePalette(app->dark_mode(), app->accent_color());
}

void BrowserWindow::RefreshBackdrop() {
  const uint32_t color = workspace_ ? workspace_->record().color : ui::kAccentChoices[0];
  // A theme from the gradient picker decides the colours, the text (light
  // or dark: zen-should-be-dark-mode) and the accent; else the space colour.
  std::optional<orbit::SpaceTheme> theme;
  if (workspace_ && !profile_->is_private() && !workspace_->record().theme.empty()) {
    theme = orbit::ParseSpaceTheme(workspace_->record().theme);
  }
  const bool was_dark = palette_.dark;
  palette_ = BasePalette();
  if (theme) {
    Application* app = Application::Get();
    const uint32_t a = app->accent_color();
    const orbit::ThemePaint paint = orbit::BuildThemePaint(
        *theme, app->dark_mode(),
        {static_cast<float>((a >> 16) & 0xFF), static_cast<float>((a >> 8) & 0xFF), static_cast<float>(a & 0xFF)});
    palette_ = ui::MakePalette(paint.dark, ui::RgbToUint(paint.accent));
    palette_.chrome_bg = ui::ColorFromRgbF(paint.base);
    backdrop_.themed = true;
    backdrop_.paint = paint;
    const float W = std::max(Width(), 1.f), H = std::max(Height(), 1.f);
    backdrop_.from = ui::ColorFromRgbF(orbit::ThemeColorAt(paint, 0, 0, W, H));
    backdrop_.to = ui::ColorFromRgbF(orbit::ThemeColorAt(paint, W, H, W, H));
  } else {
    backdrop_ = ui::MakeBackdrop(palette_, color, theme_strength_);
  }
  if (palette_.dark != was_dark) {
    const BOOL dark = palette_.dark ? TRUE : FALSE;
    ::DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  }
  const COLORREF border = ui::ToColorRef(backdrop_.from);
  ::DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof(border));
  UpdateAddressColors();
  Invalidate();
}

D2D1_COLOR_F BrowserWindow::BackdropAt(float x, float y) const {
  // The gradient runs from the top-left to the bottom-right corner.
  const float w = std::max(Width(), 1.f);
  const float h = std::max(Height(), 1.f);
  if (backdrop_.themed) return ui::ColorFromRgbF(orbit::ThemeColorAt(backdrop_.paint, x, y, w, h));
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
  if (chrome_hidden() || CompactHidesSidebar()) return 0.f;
  return collapsed_ ? ui::metrics::kSidebarCompactWidth : sidebar_width_;
}

float BrowserWindow::SidebarPanelWidth() const {
  if (chrome_hidden()) return 0.f;
  if (CompactHidesSidebar() && !sidebar_revealed_ && !compact_hiding_) return 0.f;
  return collapsed_ ? ui::metrics::kSidebarCompactWidth : sidebar_width_;
}

bool BrowserWindow::sidebar_visible() const { return SidebarPanelWidth() > 0.f; }

float BrowserWindow::SidebarLeft() const {
  return right_side_ ? Width() - SidebarPanelWidth() : 0.f;
}

bool BrowserWindow::InSidebar(float x) const {
  const float left = SidebarLeft();
  return x >= left && x < left + SidebarPanelWidth();
}

void BrowserWindow::ToggleTabsOnRight() {
  // cmd_zenToggleTabsOnRight.
  Application::Get()->store().SetPref(Store::kGlobalScope, L"tabs_right", right_side_ ? L"0" : L"1");
  Application::Get()->ApplyThemeToAllWindows();
}

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
  const float top = multi_toolbar() && !CompactHidesToolbar() ? ui::metrics::kToolbarHeight : kSeparation;
  if (right_side_) {
    const float right = CompactHidesSidebar() ? W - kSeparation : W - SidebarWidth();
    return {kSeparation, top, std::max(kSeparation + 1.f, right), std::max(top + 1.f, H - kSeparation)};
  }
  const float left = CompactHidesSidebar() ? kSeparation : SidebarWidth();
  return {left, top, std::max(left + 1.f, W - kSeparation), std::max(top + 1.f, H - kSeparation)};
}

RECT BrowserWindow::ContentRectPx() const {
  if (chrome_hidden()) {
    RECT rc;
    ::GetClientRect(hwnd_, &rc);
    return rc;
  }
  RECT r = ToPx(ContentRectDip());
  // The window-controls reveal moves the card instead of resizing it, so the
  // page does not re-layout while the pointer passes; the part pushed past
  // the window edge is clipped for as long as the reveal lasts. The compact
  // sidebar floats over the page (overlay_) and moves nothing.
  const float reveal = caption_reveal_.Value(::GetTickCount64());
  if (reveal > 0.f) {
    // A hidden toolbar comes down whole; otherwise the window-control strip.
    const float strip = CompactHidesToolbar() ? ui::metrics::kToolbarHeight : ui::metrics::kCaptionHeight;
    ::OffsetRect(&r, 0, static_cast<int>(std::lround(px(strip * reveal))));
  }
  // The library pushes the page right by its width beyond the sidebar's
  // (webOffset in ZenLibrary.openProgress).
  if (const float lib = library_slide_.Value(::GetTickCount64()); lib > 0.f) {
    const float side = CompactHidesSidebar() ? ui::metrics::kSeparation : SidebarWidth();
    const int shift = static_cast<int>(std::lround(px(lib * (LibraryWidth() - side))));
    ::OffsetRect(&r, right_side_ ? -shift : shift, 0);
  }
  return r;
}

int BrowserWindow::ContentCornerPx() const {
  if (chrome_hidden()) return 0;
  return static_cast<int>(std::lround(px(ui::metrics::kContentRadius)));
}

void BrowserWindow::Layout() {
  {
    // The pinned separator collapses / opens when the last pinned tab goes
    // or the first arrives (a space switch shows the new space as it is).
    const float target = workspace_ && workspace_->PinnedCount() > 0 ? 1.f : 0.f;
    const uint64_t now = ::GetTickCount64();
    if (pinned_sep_space_ != workspace_) {
      pinned_sep_.Jump(target);
    } else if (pinned_sep_.target() != target) {
      pinned_sep_.Start(target, now, ui::AnimMs(80), 0, orbit::Transition::Curve::kEaseInOut);
      UpdateAnimationTimer(now);
    }
    pinned_sep_space_ = workspace_;
  }
  using namespace ui::metrics;
  const float W = Width();
  const float H = Height();
  const float sw = SidebarPanelWidth();
  const bool visible = sw > 0.f;

  address_rect_ = site_info_ = star_ = copy_url_ = ext_btn_ = ws_actions_ = essentials_rect_ = linked_back_ = {};
  ext_pin_rects_.clear();
  ext_pin_ids_.clear();
  menu_ = toggle_ = back_ = forward_ = reload_ = ws_indicator_ = {};
  list_ = footer_ = downloads_ = create_new_ = panel_ = media_card_ = {};
  workspace_dots_.clear();
  first_dot_workspace_ = 0;

  // Window controls, flush with the top-right corner while revealed.
  caption_min_ = caption_max_ = caption_close_ = {};
  if (caption_revealed_ && !chrome_hidden() && right_side_) {
    // Tabs on the right: the window controls move to the other corner.
    caption_min_ = {0, 0, kCaptionButtonWidth, kCaptionHeight};
    caption_max_ = {caption_min_.right, 0, caption_min_.right + kCaptionButtonWidth, kCaptionHeight};
    caption_close_ = {caption_max_.right, 0, caption_max_.right + kCaptionButtonWidth, kCaptionHeight};
  } else if (caption_revealed_ && !chrome_hidden()) {
    caption_close_ = {W - kCaptionButtonWidth, 0, W, kCaptionHeight};
    caption_max_ = {caption_close_.left - kCaptionButtonWidth, 0, caption_close_.left,
                    kCaptionHeight};
    caption_min_ = {caption_max_.left - kCaptionButtonWidth, 0, caption_max_.left,
                    kCaptionHeight};
  }

  toolbar_ = {};
  if (toolbar_shown()) {
    // #nav-bar: back, forward, reload | the URL field | compact, menu |
    // window controls. It spans the whole top edge so the sidebar sits below
    // it, rather than starting at the sidebar's right edge.
    toolbar_ = {0.f, 0.f, W, kToolbarHeight};
    const float by = (kToolbarHeight - kButton) / 2.f;
    float start = toolbar_.left;
    float end = toolbar_.right;
    if (right_side_) {
      caption_min_ = {start, 0, start + kCaptionButtonWidth, kToolbarHeight};
      caption_max_ = {caption_min_.right, 0, caption_min_.right + kCaptionButtonWidth, kToolbarHeight};
      caption_close_ = {caption_max_.right, 0, caption_max_.right + kCaptionButtonWidth, kToolbarHeight};
      start = caption_close_.right + 4.f;
    } else {
      caption_close_ = {W - kCaptionButtonWidth, 0, W, kToolbarHeight};
      caption_max_ = {caption_close_.left - kCaptionButtonWidth, 0, caption_close_.left, kToolbarHeight};
      caption_min_ = {caption_max_.left - kCaptionButtonWidth, 0, caption_max_.left, kToolbarHeight};
      end = caption_min_.left - 4.f;
    }
    // The extensions button and the pinned extensions sit at the left end
    // (at most a third of the bar), before the centred group.
    if (ExtensionsButtonShown()) {
      start = LayoutExtensionButtons(start + 8.f, (kToolbarHeight - kButton) / 2.f,
                                     start + (end - start) / 3.f) - 4.f;
    }
    // One centred group: back, forward, reload, the URL field (at most 560
    // wide, or 800 with the "wide URL bar" setting), compact, menu.
    constexpr float kGap = 6.f;
    const float left_buttons = 3.f * kButton + 2.f;
    const float right_buttons = 2.f * kButton + 1.f;
    const float span = std::max(0.f, end - start - 16.f);
    const float w = std::clamp(span - left_buttons - right_buttons - 2.f * kGap, 0.f,
                               urlbar_wide_ ? 800.f : 560.f);
    const float group = left_buttons + right_buttons + (w > 80.f ? w + 2.f * kGap : kGap);
    const float gx = start + 8.f + std::max(0.f, (span - group) / 2.f);
    back_ = {gx, by, gx + kButton, by + kButton};
    forward_ = {back_.right + 1, by, back_.right + 1 + kButton, by + kButton};
    reload_ = {forward_.right + 1, by, forward_.right + 1 + kButton, by + kButton};
    const float al = reload_.right + kGap;
    const float after = w > 80.f ? al + w + kGap : al;
    toggle_ = {after, by, after + kButton, by + kButton};
    menu_ = {toggle_.right + 1, by, toggle_.right + 1 + kButton, by + kButton};
    const float ay = (kToolbarHeight - kUrlBarHeight) / 2.f;
    if (w > 80.f) {
      address_rect_ = {al, ay, al + w, ay + kUrlBarHeight};
      site_info_ = {address_rect_.left + 4, address_rect_.top + 5, address_rect_.left + 30, address_rect_.bottom - 5};
      if (!profile_->is_private()) {
        star_ = {address_rect_.right - 30, address_rect_.top + 5, address_rect_.right - 4, address_rect_.bottom - 5};
      }
      const float actions_left = star_.right > star_.left ? star_.left : address_rect_.right - 4;
      copy_url_ = {actions_left - 26, address_rect_.top + 5, actions_left, address_rect_.bottom - 5};
    }
  }

  const std::vector<Workspace*> workspaces = Spaces();
  if (visible) {
    // With multiple toolbars the nav bar spans the top; the sidebar sits
    // below it so the revealed tab list never covers back / forward / reload.
    const float panel_top = toolbar_shown() ? ui::metrics::kToolbarHeight : 0.f;
    panel_ = {0, panel_top, sw, H};
    if (CompactHidesSidebar()) {
      // Floating card: it overhangs the page card by 4 px at the bottom
      // (H - 4 vs H - kSeparation) and now by the same at the top (the page
      // starts at the toolbar's bottom or kSeparation).
      const float page_top = toolbar_shown() ? kToolbarHeight : kSeparation;
      panel_ = {4.f, page_top - 4.f, sw - 2.f, H - 4.f};
    }
    const float l = kInner;
    const float r = sw - kInner;
    if (!collapsed_) {
      float y = panel_.top + kSeparation;
      if (!multi_toolbar()) {
        // Top row: menu and compact-mode toggle; navigation on the right.
        const float by = (kTopRowHeight - kButton) / 2.f;
        menu_ = {l, by, l + kButton, by + kButton};
        toggle_ = {menu_.right + 1, by, menu_.right + 1 + kButton, by + kButton};
        reload_ = {r - kButton, by, r, by + kButton};
        forward_ = {reload_.left - 1 - kButton, by, reload_.left - 1, by + kButton};
        back_ = {forward_.left - 1 - kButton, by, forward_.left - 1, by + kButton};
        // The extensions button and the pinned extensions, after the toggle.
        if (ExtensionsButtonShown()) ext_btn_ = {toggle_.right + 1, by, toggle_.right + 1 + kButton, by + kButton};
        y = kTopRowHeight;
        // Address bar.
        address_rect_ = {l, y, r, y + kUrlBarHeight};
        site_info_ = {address_rect_.left + 4, address_rect_.top + 5, address_rect_.left + 30,
                      address_rect_.bottom - 5};
        if (!profile_->is_private()) {
          star_ = {address_rect_.right - 30, address_rect_.top + 5, address_rect_.right - 4,
                   address_rect_.bottom - 5};
        }
        // zen.urlbar.single-toolbar-show-copy-url: a copy button among the
        // page actions, hidden for pages that are not http(s).
        const float actions_left = star_.right > star_.left ? star_.left : address_rect_.right - 4;
        copy_url_ = {actions_left - 26, address_rect_.top + 5, actions_left, address_rect_.bottom - 5};
        y = address_rect_.bottom + 4;
        // Pinned extensions: the top row has no room, so a slim row of their own.
        if (ExtensionsButtonShown() && LayoutPinnedExtensions(l, y, r) > l) y += kButton + 4;
      }
      // #zen-essentials: the space's grid of icon tiles.
      essentials_rect_ = {};
      if (Workspace* ess = essentials(); ess && !ess->tabs().empty()) {
        essentials_rect_ = {l, y, r, y};  // seeds EssentialRects()'s top
        const auto rects = EssentialRects();
        float bottom = y;
        for (const auto& er : rects) bottom = std::max(bottom, er.bottom);
        essentials_rect_ = {l, y, r, bottom};
        y = bottom + 4.f;
      }
      // Workspace indicator: the space's name, with an actions button on hover.
      ws_indicator_ = {l, y, r, y + kWorkspaceIndicatorHeight - 6};
      ws_actions_ = {r - 30, ws_indicator_.top + 5, r - 4, ws_indicator_.bottom - 5};
      // A linked window: "Linked to <parent>" after a back arrow.
      if (workspace_ && workspace_->is_linked() && workspace_->parent()) {
        linked_back_ = {l + 4, ws_indicator_.top + 5, l + 30, ws_indicator_.bottom - 5};
      }
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
        if (workspaces[static_cast<size_t>(i)] == workspace_) active = i;
      }
      first_dot_workspace_ = std::clamp(active - shown / 2, 0, count - shown);
      const float total = shown * kIconButton + (shown - 1) * 3.f;
      float x = (downloads_.right + create_new_.left - total) / 2.f;
      for (int i = 0; i < shown; ++i) {
        workspace_dots_.push_back({x, fy, x + kIconButton, fy + kIconButton});
        x += kIconButton + 3.f;
      }
      media_card_ = {};
      if (media_tab_) {
        // .zen-media-card above the footer.
        media_card_ = {l, footer_.top - 82.f, sw - kInner, footer_.top - 6.f};
        list_ = {0, y, sw, media_card_.top - 4.f};
      } else {
        list_ = {0, y, sw, footer_.top - 2.f};
      }
    } else {
      // Collapsed: one centered column.
      const float cx = sw / 2.f;
      float y = panel_.top + 6.f;
      auto next = [&](D2D1_RECT_F& rect) {
        rect = {cx - 16.f, y, cx + 16.f, y + 32.f};
        y += 34.f;
      };
      if (!multi_toolbar()) {
        next(menu_);
        next(toggle_);
        if (ExtensionsButtonShown()) next(ext_btn_);
        next(back_);
        next(forward_);
        next(reload_);
        y += 2.f;
      }
      essentials_rect_ = {};
      if (Workspace* ess = essentials(); ess && !ess->tabs().empty()) {
        const float top = y;
        y += static_cast<float>(ess->tabs().size()) * 40.f;
        essentials_rect_ = {cx - 24.f, top, cx + 24.f, y};
        y += 4.f;
      }
      next(ws_indicator_);
      const float footer_h = 3 * 34.f + 8.f;
      footer_ = {0, H - footer_h, sw, H};
      float fy = H - footer_h + 4.f;
      downloads_ = {cx - 16.f, fy, cx + 16.f, fy + 32.f};
      fy += 34.f;
      workspace_dots_.push_back({cx - 16.f, fy, cx + 16.f, fy + 32.f});
      for (int i = 0; i < static_cast<int>(workspaces.size()); ++i) {
        if (workspaces[static_cast<size_t>(i)] == workspace_) first_dot_workspace_ = i;
      }
      fy += 34.f;
      create_new_ = {cx - 16.f, fy, cx + 16.f, fy + 32.f};
      list_ = {0, y + 2.f, sw, footer_.top - 2.f};
    }
  }

  // Tabs on the right: everything above was laid out from x = 0.
  if (visible && right_side_) {
    const float dx = SidebarLeft();
    for (D2D1_RECT_F* rect : {&address_rect_, &site_info_, &star_, &copy_url_, &ext_btn_, &ws_actions_, &linked_back_,
                              &essentials_rect_, &menu_, &toggle_, &back_, &forward_, &reload_,
                              &ws_indicator_, &list_, &footer_, &downloads_, &create_new_, &panel_,
                              &media_card_}) {
      const bool in_toolbar = multi_toolbar() && (rect == &address_rect_ || rect == &site_info_ ||
                                                  rect == &star_ || rect == &copy_url_ || rect == &ext_btn_ || rect == &menu_ ||
                                                  rect == &toggle_ || rect == &back_ || rect == &forward_ ||
                                                  rect == &reload_);
      if (rect->right > rect->left && !in_toolbar) {
        rect->left += dx;
        rect->right += dx;
      }
    }
    for (auto& dot : workspace_dots_) {
      dot.left += dx;
      dot.right += dx;
    }
    if (!multi_toolbar()) {
      for (auto& pin : ext_pin_rects_) {
        pin.left += dx;
        pin.right += dx;
      }
    }
  }
  UpdateOverlay();
  if (address_ && !popup_visible_) {
    if (address_rect_.right <= address_rect_.left) {
      ::ShowWindow(address_, SW_HIDE);
    } else {
      const float right = copy_url_.left - 2;
      RECT field = ToPx({site_info_.right + 2, address_rect_.top, right, address_rect_.bottom});
      if (AddressHost() != hwnd_) {
        const POINT o = OverlayOriginPx();
        ::OffsetRect(&field, -o.x, -o.y);
      }
      const int edit_h = static_cast<int>(std::lround(px(19.f)));
      const int top = (field.top + field.bottom - edit_h) / 2;
      ::SetWindowPos(address_, nullptr, field.left, top, field.right - field.left, edit_h,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
  }
  if (popup_visible_) PositionFloatingUrlBar();
  if (renaming()) PositionRenameEditor();
  if (tooltip_) {
    TTTOOLINFOW ti{sizeof(ti)};
    ti.hwnd = hwnd_;
    ti.uId = 1;
    ::GetClientRect(hwnd_, &ti.rect);
    ::SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
  }
  ClampScroll();
  PlaceContent();
  FindBar::Reposition(this);  // the find bar follows the page area
  Invalidate();
}

void BrowserWindow::PlaceContent() { ShowContent(); }

const Workspace::SplitView* BrowserWindow::ActiveSplit() const {
  Tab* tab = active_tab();
  if (!workspace_ || !tab || active_essential_ || content_fullscreen_) return nullptr;
  return workspace_->SplitOf(tab);
}

std::vector<BrowserWindow::PaneRect> BrowserWindow::SplitPanes(
    std::vector<SplitHandle>* handles) const {
  std::vector<PaneRect> out;
  const Workspace::SplitView* split = ActiveSplit();
  if (!split) return out;
  const RECT content = ContentRectPx();
  const SplitRect bounds{dip(static_cast<float>(content.left)), dip(static_cast<float>(content.top)),
                         dip(static_cast<float>(content.right)),
                         dip(static_cast<float>(content.bottom))};
  std::vector<SplitPane> panes;
  // --zen-split-column-gap: the element separation (+1px).
  LayoutSplit(split->root, bounds, ui::metrics::kSeparation, &panes, handles);
  for (const SplitPane& pane : panes) {
    if (Tab* t = workspace_->SplitTab(pane.key)) {
      out.push_back({t, {pane.rect.left, pane.rect.top, pane.rect.right, pane.rect.bottom}});
    }
  }
  return out;
}

void BrowserWindow::ShowContent() {
  std::vector<Tab*> visible;
  std::vector<RECT> rects;
  Tab* devtools_tab = nullptr;
  RECT devtools_rect{};
  if (glance_ && active_tab() == glance_->tab) {
    visible = {glance_->parent, glance_->tab};
    rects = {ContentRectPx(), ToPx(GlanceRectNow(::GetTickCount64()))};
  } else if (Tab* tab = active_tab()) {
    const auto panes = SplitPanes();
    if (panes.empty()) {
      visible.push_back(tab);
      RECT page = ContentRectPx(), tools{};
      if (DevToolsLayout(ContentRectPx(), &page, &tools)) devtools_tab = tab;
      rects.push_back(page);
      devtools_rect = tools;
    } else {
      for (const PaneRect& pane : panes) {
        visible.push_back(pane.tab);
        rects.push_back(ToPx(pane.rect));
      }
    }
  }
  // Hide what is no longer on screen (tabs that still exist somewhere).
  for (Tab* t : content_shown_) {
    if (std::find(visible.begin(), visible.end(), t) != visible.end()) continue;
    const bool alive = (workspace_ && workspace_->IndexOf(t) >= 0) ||
                       (essentials() && essentials()->IndexOf(t) >= 0);
    if (alive && t->window() == this) t->Hide();
  }
  const bool glancing = glance_ && active_tab() == glance_->tab;
  for (size_t i = 0; i < visible.size(); ++i) {
    if (visible[i] != active_tab()) visible[i]->EnsureBrowser(this);  // panes load with the split
    // Stacking: the parent, its fade, then the glance (Show raises each).
    if (glancing && i == 1) PlaceGlanceChrome(true);
    visible[i]->Show(rects[i], ContentCornerPx());
    if (visible[i] == devtools_tab) {
      visible[i]->ShowDevToolsPane(devtools_rect, ContentCornerPx());
    } else {
      visible[i]->HideDevToolsPane();
    }
  }
  if (glancing && glance_bar_ && glance_animating()) {
    ::ShowWindow(glance_bar_, SW_HIDE);  // the buttons come in once it has landed
  } else if (glancing && glance_bar_) {
    ::SetWindowPos(glance_bar_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  } else if (!glancing) {
    PlaceGlanceChrome(false);
  }
  content_shown_ = std::move(visible);
  MoveOverlay();  // tab hosts raise themselves; the card stays above them
}

int BrowserWindow::VisibleRowCapacity() const {
  return std::max(1, static_cast<int>((list_.bottom - list_.top) / ui::metrics::kRowHeight));
}

float BrowserWindow::PeripheryTop() const {
  if (!workspace_) return 0.f;
  int visible = 0;
  const bool folded = PinnedSectionCollapsed();
  for (const auto& t : workspace_->tabs()) {
    if (t->pinned() && (!folded || t.get() == active_tab())) ++visible;
  }
  return static_cast<float>(visible) * ui::metrics::kRowHeight;
}

bool BrowserWindow::PinnedSectionCollapsed() const {
  if (!workspace_ || workspace_->PinnedCount() == 0) return false;
  // Rows() asks on every paint and hit test: read the stored state once.
  auto it = pins_collapsed_.find(workspace_);
  if (it == pins_collapsed_.end()) {
    const bool stored = !profile_->is_private() &&
                        Application::Get()->store().GetPref(
                            Store::kGlobalScope,
                            L"pins_collapsed:" + std::to_wstring(workspace_->id())) == L"1";
    it = pins_collapsed_.emplace(workspace_, stored).first;
  }
  return it->second;
}

void BrowserWindow::TogglePinnedSection() {
  if (!workspace_ || workspace_->PinnedCount() == 0) return;
  CaptureRows();
  const bool folded = !PinnedSectionCollapsed();
  pins_collapsed_[workspace_] = folded;
  if (!profile_->is_private()) {
    Application::Get()->store().SetPref(Store::kGlobalScope,
                                        L"pins_collapsed:" + std::to_wstring(workspace_->id()),
                                        folded ? L"1" : L"0");
  }
  focus_row_ = -1;
  Layout();
  AnimateRows();
}

BrowserWindow::RowKey BrowserWindow::KeyOf(const Row& row) const {
  if (row.is_group) return {true, static_cast<intptr_t>(row.group_id)};
  if (row.linked) return {true, -static_cast<intptr_t>(row.linked)};  // folder ids are positive
  return {false, reinterpret_cast<intptr_t>(workspace_->tabs()[static_cast<size_t>(row.tab)].get())};
}

void BrowserWindow::CaptureRows() {
  captured_rows_ = workspace_ ? Rows() : std::vector<Row>{};
  captured_keys_.clear();
  for (const Row& row : captured_rows_) captured_keys_.push_back(KeyOf(row));
  periphery_from_ = PeripheryTop();
}

void BrowserWindow::AnimateRows() {
  row_from_.clear();
  row_ghosts_.clear();
  const uint32_t ms = ui::AnimMs(kFolderAnimMs);
  if (!workspace_ || ms == 0 || captured_rows_.empty()) {
    captured_rows_.clear();
    captured_keys_.clear();
    row_anim_.Jump(1.f);
    return;
  }
  std::map<RowKey, float> now_top;
  std::map<RowKey, int> now_depth;
  for (const Row& row : Rows()) {
    now_top[KeyOf(row)] = row.top;
    now_depth[KeyOf(row)] = row.depth;
  }
  indent_from_.clear();
  for (size_t i = 0; i < captured_rows_.size(); ++i) {
    const auto d = now_depth.find(captured_keys_[i]);
    if (d != now_depth.end() && d->second != captured_rows_[i].depth) {
      indent_from_[captured_keys_[i]] = captured_rows_[i].depth;
    }
  }
  if (!indent_from_.empty()) {
    indent_anim_.Jump(0.f);
    indent_anim_.Start(1.f, ::GetTickCount64(), ui::AnimMs(100), 0, orbit::Transition::Curve::kEaseInOut);
  }
  for (size_t i = 0; i < captured_rows_.size(); ++i) {
    const Row& row = captured_rows_[i];
    const RowKey key = captured_keys_[i];
    if (now_top.count(key)) {
      row_from_[key] = row.top;
      continue;
    }
    RowGhost ghost{row, key, {false, 0}};
    for (size_t j = i + 1; j < captured_rows_.size(); ++j) {
      const RowKey next = captured_keys_[j];
      if (now_top.count(next)) {
        ghost.next = next;
        break;
      }
    }
    row_ghosts_.push_back(ghost);
  }
  captured_rows_.clear();
  captured_keys_.clear();
  const uint64_t now = ::GetTickCount64();
  row_anim_.Jump(0.f);
  row_anim_.Start(1.f, now, ms, 0, orbit::Transition::Curve::kEaseInOut);
  UpdateAnimationTimer(now);
  Invalidate();
}

float BrowserWindow::PinnedSeparatorShown() const {
  return std::clamp(pinned_sep_.Value(::GetTickCount64()), 0.f, 1.f);
}

float BrowserWindow::PeripheryHeight() const {
  return ui::metrics::kPinnedSeparatorHeight * PinnedSeparatorShown() +
         (newtab_button_ && newtab_button_top_ ? ui::metrics::kRowHeight : 0.f);
}

float BrowserWindow::ListContentHeight() const {
  const auto rows = Rows();
  const float periphery_end = PeripheryTop() + PeripheryHeight();
  float end = rows.empty() ? periphery_end
                           : std::max(rows.back().top + ui::metrics::kRowHeight, periphery_end);
  if (newtab_button_ && !newtab_button_top_) end += ui::metrics::kRowHeight;  // after the tabs
  return end;
}

std::vector<BrowserWindow::Row> BrowserWindow::Rows() const {
  using ui::metrics::kRowHeight;
  std::vector<Row> rows;
  if (!workspace_) return rows;
  const auto& tabs = workspace_->tabs();
  const int pinned = workspace_->PinnedCount();
  const bool folded = PinnedSectionCollapsed();
  float y = 0.f;
  bool periphery = false;
  std::vector<int64_t> open;  // the folders whose headers lead to the current tab
  int64_t last_split = 0;
  auto folded_folder = [&](int64_t id) {
    const TabGroupRecord* g = workspace_->FindGroup(id);
    return g && g->collapsed;
  };
  for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
    if (i == pinned) {
      y += PeripheryHeight();  // separator + "New Tab"
      periphery = true;
    }
    const Tab* tab = tabs[static_cast<size_t>(i)].get();
    if (glance_ && tab == glance_->tab) continue;  // in its parent's row
    if (i < pinned && folded && tab != active_tab()) continue;
    // Folder headers (ZenFolders, nested): one per folder entered; nothing
    // inside a collapsed folder shows, not even its subfolders.
    const std::vector<int64_t> chain = workspace_->GroupChain(tab);
    size_t common = 0;
    while (common < open.size() && common < chain.size() && open[common] == chain[common]) ++common;
    open.resize(common);
    bool hidden = std::any_of(open.begin(), open.end(), folded_folder);
    for (size_t d = common; d < chain.size(); ++d) {
      open.push_back(chain[d]);
      if (hidden) continue;
      rows.push_back({true, -1, chain[d], y, 0, static_cast<int>(d)});
      y += kRowHeight;
      hidden = folded_folder(chain[d]);
    }
    if (hidden || tab->empty_placeholder()) continue;  // [zen-empty-tab]
    // A split view's members share one row (tab-group[split-view-group]).
    const Workspace::SplitView* split = workspace_->SplitOf(tab);
    if (split && split->id == last_split) continue;
    last_split = split ? split->id : 0;
    rows.push_back({false, i, chain.empty() ? 0 : chain.back(), y, split ? split->id : 0,
                    static_cast<int>(chain.size())});
    y += kRowHeight;
  }
  // The space's linked windows, as collapsed "temp folder" rows at the end.
  for (Workspace* linked : LinkedChildren()) {
    if (!periphery) {
      y += PeripheryHeight();
      periphery = true;
    }
    Row row;
    row.top = y;
    row.linked = linked->id();
    rows.push_back(row);
    y += kRowHeight;
  }
  return rows;
}

D2D1_RECT_F BrowserWindow::RowRect(const Row& row) const {
  using namespace ui::metrics;
  const float top = list_.top + row.top - scroll_;
  const float sw = SidebarPanelWidth();
  const float sx = SidebarLeft();
  if (collapsed_) {
    const float l = sx + (sw - 48.f) / 2.f;
    return {l, top + kRowInset, l + 48.f, top + kRowHeight - kRowInset};
  }
  // Folder contents are indented under their folder, one step per level.
  const float indent = static_cast<float>(row.depth) * kFolderIndent;
  return {sx + kInner + indent, top + kRowInset, sx + sw - kInner, top + kRowHeight - kRowInset};
}

D2D1_RECT_F BrowserWindow::NewTabRect() const {
  using namespace ui::metrics;
  if (!sidebar_visible() || !newtab_button_) return {};
  if (!newtab_button_top_) {
    // zen.view.show-newtab-button-top = false: the row follows the tabs.
    const Row row{false, -1, 0, ListContentHeight() - kRowHeight};
    return RowRect(row);
  }
  const Row row{false, -1, 0, PeripheryTop() + kPinnedSeparatorHeight * PinnedSeparatorShown()};
  return RowRect(row);
}

D2D1_RECT_F BrowserWindow::ClearRect() const {
  using namespace ui::metrics;
  if (!sidebar_visible() || collapsed_ || !workspace_ || workspace_->PinnedCount() == 0) return {};
  const float top = list_.top + PeripheryTop() - scroll_;
  const float r = SidebarLeft() + SidebarPanelWidth() - kInner;
  return {r - 62.f, top + 2.f, r, top + kPinnedSeparatorHeight - 2.f};
}

int BrowserWindow::RowOfTab(const std::vector<Row>& rows, int tab_index) const {
  for (size_t r = 0; r < rows.size(); ++r) {
    if (!rows[r].is_group && rows[r].tab == tab_index) return static_cast<int>(r);
  }
  // A split member: its split's row.
  if (workspace_ && tab_index >= 0 && tab_index < static_cast<int>(workspace_->tabs().size())) {
    if (const auto* split = workspace_->SplitOf(workspace_->tabs()[static_cast<size_t>(tab_index)].get())) {
      for (size_t r = 0; r < rows.size(); ++r) {
        if (rows[r].split == split->id) return static_cast<int>(r);
      }
    }
  }
  // Hidden in a collapsed folder: the nearest shown folder header stands in.
  if (workspace_ && tab_index >= 0 && tab_index < static_cast<int>(workspace_->tabs().size())) {
    const auto chain = workspace_->GroupChain(workspace_->tabs()[static_cast<size_t>(tab_index)].get());
    for (auto g = chain.rbegin(); g != chain.rend(); ++g) {
      for (size_t r = 0; r < rows.size(); ++r) {
        if (rows[r].is_group && rows[r].group_id == *g) return static_cast<int>(r);
      }
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
  if (r < 0) return {};
  const Row& row = rows[static_cast<size_t>(r)];
  D2D1_RECT_F rect = RowRect(row);
  if (row.split && !collapsed_) {
    // The member's segment of the split row.
    const int n = SplitMemberCount(row);
    const int k = index - row.tab;
    if (n > 0 && k >= 0 && k < n) {
      const float w = (rect.right - rect.left) / static_cast<float>(n);
      rect = {rect.left + w * static_cast<float>(k), rect.top,
              rect.left + w * static_cast<float>(k + 1), rect.bottom};
    }
  }
  return rect;
}

int BrowserWindow::SplitMemberCount(const Row& row) const {
  if (!row.split || !workspace_) return 0;
  for (const auto& split : workspace_->splits()) {
    if (split.id == row.split) return static_cast<int>(workspace_->SplitMembers(split).size());
  }
  return 0;
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
  {
    std::vector<SplitHandle> handles;
    if (!SplitPanes(&handles).empty() && !overlay_visible_) {
      for (size_t i = 0; i < handles.size(); ++i) {
        const auto& r = handles[i].rect;
        if (x >= r.left - 1.f && x < r.right + 1.f && y >= r.top && y < r.bottom) {
          return {HitKind::kSplitHandle, static_cast<int>(i)};
        }
      }
    }
  }
  {
    RECT page{}, tools{};
    if (SplitPanes().empty() && DevToolsLayout(ContentRectPx(), &page, &tools)) {
      const float left = dip(static_cast<float>(page.right)), right = dip(static_cast<float>(tools.left));
      if (x >= left - 2.f && x <= right + 2.f && y >= dip(static_cast<float>(tools.top)) &&
          y < dip(static_cast<float>(tools.bottom))) {
        return {HitKind::kDevToolsEdge, 0};
      }
    }
  }
  if (multi_toolbar() && ui::Contains(toolbar_, x, y)) {
    if (ui::Contains(menu_, x, y)) return {HitKind::kMenu, 0};
    if (ui::Contains(toggle_, x, y)) return {HitKind::kSidebarToggle, 0};
    if (ui::Contains(back_, x, y)) return {HitKind::kBack, 0};
    if (ui::Contains(forward_, x, y)) return {HitKind::kForward, 0};
    if (ui::Contains(reload_, x, y)) return {HitKind::kReload, 0};
    if (ui::Contains(site_info_, x, y)) return {HitKind::kSiteInfo, 0};
    if (ui::Contains(star_, x, y)) return {HitKind::kStar, 0};
    if (ui::Contains(ext_btn_, x, y)) return {HitKind::kExtensions, 0};
    for (size_t i = 0; i < ext_pin_rects_.size(); ++i) {
      if (ui::Contains(ext_pin_rects_[i], x, y)) return {HitKind::kExtensionPin, static_cast<int>(i)};
    }
    if (CanCopyUrlFromButton() && ui::Contains(copy_url_, x, y)) return {HitKind::kCopyUrl, 0};
    return {};  // the rest of the toolbar drags the window
  }
  const float sw = SidebarPanelWidth();
  if (sw <= 0.f) return {};
  const float edge = right_side_ ? SidebarLeft() : sw;  // the side facing the page
  if (!collapsed_ && std::fabs(x - edge) <= 3.f) return {HitKind::kSidebarEdge, 0};
  if (!InSidebar(x)) return {};
  if (ui::Contains(menu_, x, y)) return {HitKind::kMenu, 0};
  if (ui::Contains(toggle_, x, y)) return {HitKind::kSidebarToggle, 0};
  if (ui::Contains(back_, x, y)) return {HitKind::kBack, 0};
  if (ui::Contains(forward_, x, y)) return {HitKind::kForward, 0};
  if (ui::Contains(reload_, x, y)) return {HitKind::kReload, 0};
  if (ui::Contains(site_info_, x, y)) return {HitKind::kSiteInfo, 0};
  if (ui::Contains(star_, x, y)) return {HitKind::kStar, 0};
  if (ui::Contains(ext_btn_, x, y)) return {HitKind::kExtensions, 0};
  for (size_t i = 0; i < ext_pin_rects_.size(); ++i) {
    if (ui::Contains(ext_pin_rects_[i], x, y)) return {HitKind::kExtensionPin, static_cast<int>(i)};
  }
  if (CanCopyUrlFromButton() && ui::Contains(copy_url_, x, y)) return {HitKind::kCopyUrl, 0};
  if (ui::Contains(essentials_rect_, x, y)) {
    const auto rects = EssentialRects();
    for (size_t i = 0; i < rects.size(); ++i) {
      if (ui::Contains(rects[i], x, y)) return {HitKind::kEssential, static_cast<int>(i)};
    }
    return {};
  }
  if (ui::Contains(linked_back_, x, y)) return {HitKind::kLinkedBack, 0};
  if (!collapsed_ && ui::Contains(ws_actions_, x, y)) return {HitKind::kWorkspaceActions, 0};
  if (ui::Contains(ws_indicator_, x, y)) return {HitKind::kWorkspaceIndicator, 0};
  if (ui::Contains(media_card_, x, y)) {
    const D2D1_RECT_F& m = media_card_;
    const float row = m.bottom - 30.f;
    if (y >= row) {
      if (x < m.left + 36.f) return {HitKind::kMediaFocus, 0};
      if (x >= m.right - 36.f) return {HitKind::kMediaMute, 0};
      const float mid = (m.left + m.right) / 2.f;
      if (std::fabs(x - mid) < 18.f) return {HitKind::kMediaPlayPause, 0};
      return {HitKind::kMediaFocus, 0};
    }
    if (x >= m.right - 26.f && y < m.top + 26.f) return {HitKind::kMediaClose, 0};
    if (y >= row - 12.f) return {HitKind::kMediaSeek, 0};
    return {HitKind::kMediaFocus, 0};
  }
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
    if (nt.right > nt.left && y >= nt.top - ui::metrics::kRowInset &&
        y < nt.bottom + ui::metrics::kRowInset && x >= nt.left && x < nt.right) {
      return {HitKind::kNewTab, 0};
    }
    for (const Row& row : Rows()) {
      const D2D1_RECT_F r = RowRect(row);
      if (y < r.top - ui::metrics::kRowInset || y >= r.bottom + ui::metrics::kRowInset ||
          x < r.left - kFolderIndent || x >= r.right) {
        continue;
      }
      if (row.is_group) {
        if (ui::Contains(LiveErrorRect(row), x, y)) return {HitKind::kLiveError, GroupIndex(row.group_id)};
        return {HitKind::kGroup, GroupIndex(row.group_id)};
      }
      if (row.linked) return {HitKind::kLinked, LinkedIndex(row.linked)};
      if (row.split) {
        // Each segment of a split row is its tab (no close button in a split).
        const int n = std::max(1, SplitMemberCount(row));
        int k = 0;
        if (collapsed_) {
          const int active = workspace_->IndexOf(active_tab()) - row.tab;
          k = active >= 0 && active < n ? active : 0;
        } else {
          k = static_cast<int>((x - r.left) / ((r.right - r.left) / static_cast<float>(n)));
        }
        return {HitKind::kTab, row.tab + std::clamp(k, 0, n - 1)};
      }
      Tab* t = workspace_->tabs()[static_cast<size_t>(row.tab)].get();
      if (!collapsed_ && t->pin_changed() && x < r.left + 36.f) return {HitKind::kTabResetPin, row.tab};
      if (!collapsed_ && x >= r.right - 30.f) {
        // Pinned tabs have no close button; a loaded one offers "unload".
        if (!t->pinned() || t->has_browser()) return {HitKind::kTabClose, row.tab};
      }
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
  const bool over_page = right_side_ ? x <= content.right + kSeparation : x >= content.left - kSeparation;
  const float strip = CompactHidesToolbar() ? kToolbarHeight : kCaptionHeight;
  if (multi_toolbar() && !CompactHidesToolbar()) {
    // The toolbar carries the window controls.
  } else if (!caption_revealed_) {
    if (y < kSeparation && (over_page || CompactHidesToolbar())) SetCaptionRevealed(true);
  } else if (y > strip + kSeparation || (!over_page && !CompactHidesToolbar())) {
    ::SetTimer(hwnd_, kCaptionHideTimer, kCaptionHideDelayMs, nullptr);
  } else {
    ::KillTimer(hwnd_, kCaptionHideTimer);
  }
  if (CompactHidesSidebar()) {
    const float W = Width();
    if (!sidebar_revealed_) {
      if (right_side_ ? x > W - kSeparation : x < kSeparation) SetSidebarRevealed(true);
    } else if (right_side_ ? x < W - SidebarPanelWidth() - kSeparation
                           : x > SidebarPanelWidth() + kSeparation) {
      ::SetTimer(hwnd_, kSidebarHideTimer, kSidebarHideDelayMs, nullptr);
    } else {
      ::KillTimer(hwnd_, kSidebarHideTimer);
    }
  }
}

void BrowserWindow::SetCaptionRevealed(bool revealed) {
  if (caption_revealed_ == revealed) return;
  caption_revealed_ = revealed;
  const uint64_t now = ::GetTickCount64();
  caption_reveal_.Start(revealed ? 1.f : 0.f, now, ui::AnimMs(kCaptionRevealMs));
  caption_moving_ = caption_reveal_.Running(now);
  UpdateAnimationTimer(now);
  Layout();
}

void BrowserWindow::FlashSidebarIfNecessary() {
  // ZenCompactMode.flashSidebarIfNecessary (on tab select): with
  // zen.view.compact.toolbar-flash-popup, the hidden sidebar shows for
  // 800 ms; not during a glance animation or in a split view.
  if (!CompactHidesSidebar() || !PrefEnabled(L"compact_flash", false) || glance_animating() ||
      !SplitPanes().empty()) {
    return;
  }
  if (!sidebar_revealed_) {
    SetSidebarRevealed(true);
    flash_reveal_ = true;
  }
  // The usual hide check runs then: a pointer over the sidebar keeps it.
  ::SetTimer(hwnd_, kSidebarHideTimer, kCompactFlashMs, nullptr);
}

void BrowserWindow::SetSidebarRevealed(bool revealed) {
  if (sidebar_revealed_ == revealed) return;
  sidebar_revealed_ = revealed;
  flash_reveal_ = false;
  if (CompactHidesSidebar()) {
    const uint64_t now = ::GetTickCount64();
    const uint32_t ms = ui::AnimMs(kCompactSlideMs);
    compact_hiding_ = !revealed && ms > 0;
    compact_slide_.Start(revealed ? 1.f : 0.f, now, ms, 0, orbit::Transition::Curve::kSpring);
    UpdateAnimationTimer(now);
  }
  if (!revealed) {
    hover_ = {};
    if (::GetFocus() == address_ && !popup_visible_) FocusContent();
  }
  Layout();
}

// ---------------------------------------------------------------------------
// Model notifications

Tab* BrowserWindow::active_tab() const {
  if (active_essential_) return active_essential_;
  return workspace_ ? workspace_->active_tab() : nullptr;
}

bool BrowserWindow::DevToolsLayout(const RECT& content, RECT* page, RECT* tools) const {
  Tab* tab = active_tab();
  if (!tab || !tab->devtools_open() || (glance_ && tab == glance_->tab) || content_fullscreen_) return false;
  const int total = content.right - content.left;
  const int gap = static_cast<int>(std::lround(px(ui::metrics::kSeparation)));
  const int min_w = static_cast<int>(std::lround(px(240.f)));
  if (total < 2 * min_w + gap) return false;
  const int w = std::clamp(static_cast<int>(std::lround(px(devtools_width_))), min_w, total - min_w - gap);
  *tools = {content.right - w, content.top, content.right, content.bottom};
  *page = {content.left, content.top, tools->left - gap, content.bottom};
  return true;
}

RECT BrowserWindow::DevToolsRectPx() const {
  RECT page{}, tools{};
  if (!SplitPanes().empty() || !DevToolsLayout(ContentRectPx(), &page, &tools)) return {};
  return tools;
}

void BrowserWindow::OnDevToolsChanged(Tab* tab) {
  if (closing_) return;
  if (tab == active_tab()) ShowContent();
  Invalidate();
}

void BrowserWindow::ToggleDevTools() {
  Tab* tab = active_tab();
  if (!tab) return;
  if (tab->devtools_open() || tab->devtools_alive()) {
    tab->CloseDevTools();
  } else if (tab->browser()) {
    tab->ShowDevTools(this);
  }
  ShowContent();
}

void BrowserWindow::InspectElement(Tab* tab, int x, int y) {
  if (!tab || !tab->browser() || tab != active_tab()) return;
  const CefPoint at(x, y);
  tab->ShowDevTools(this, &at);
  ShowContent();
}

void BrowserWindow::OnTabChanged(Tab* tab) {
  UpdateMediaCard();
  if (tab == active_tab()) {
    UpdateAddressBar(false);
    UpdateTitle();
  }
  UpdateSpinnerTimer();
  Invalidate();
}

void BrowserWindow::OnTabRemoving(Tab* tab) {
  // Whatever runs below (selecting the next tab) must not keep this tab.
  struct ForgetMediaCard {
    BrowserWindow* w;
    Tab* t;
    ~ForgetMediaCard() {
      if (w->media_tab_ == t) w->media_tab_ = nullptr;
      if (w->media_dismissed_ == t) w->media_dismissed_ = nullptr;
    }
  } forget{this, tab};
  if (tab == media_tab_ || tab == media_dismissed_) {
    if (tab == media_dismissed_) media_dismissed_ = nullptr;
    media_tab_ = nullptr;
    Layout();
  }
  if (glance_ && (tab == glance_->tab || tab == glance_->parent)) {
    // The glance's owner is its parent, so closing it selects the parent.
    glance_.reset();
    PlaceGlanceChrome(false);
    Layout();
  }
  std::erase(content_shown_, tab);
  favicons_.erase(tab);
  pinned_favicons_.erase(tab);
  if (tab == rename_tab_) CancelRename();
  if (tab == focus_guard_tab_) focus_guard_tab_ = nullptr;
  // Nothing may keep pointing at a tab that leaves this window.
  for (Workspace* ws : Application::Get()->AllContainers()) {
    for (const auto& t : ws->tabs()) {
      if (t->owner() == tab) t->set_owner(nullptr);
    }
  }
  for (auto it = last_essential_.begin(); it != last_essential_.end();) {
    it = it->second == tab ? last_essential_.erase(it) : std::next(it);
  }
  if (tab == active_essential_) {
    active_essential_ = nullptr;
    tab->Hide();
    Tab* back = workspace_ ? workspace_->active_tab() : nullptr;
    if (workspace_) workspace_->set_active_tab(nullptr);
    if (!back && workspace_ && !workspace_->tabs().empty()) {
      back = TabToSelectOnClose(tab);
    }
    if (back) ActivateTab(back);
    return;
  }
  if (!workspace_ || tab != workspace_->active_tab()) return;
  Tab* next = TabToSelectOnClose(tab);
  tab->set_owner(nullptr);
  tab->Hide();
  workspace_->set_active_tab(nullptr);
  if (next) ActivateTab(next);
}

bool BrowserWindow::PrefEnabled(const wchar_t* key, bool fallback) const {
  const auto value = Application::Get()->store().GetPref(Store::kGlobalScope, key);
  return value ? *value == L"1" : fallback;
}

Tab* BrowserWindow::TabToSelectOnClose(Tab* closing) const {
  // Tabbrowser._findTabToBlurTo as patched by Zen: the owner first
  // (browser.tabs.selectOwnerOnClose), then, with
  // zen.tabs.select-recently-used-on-close, the most recently used tab,
  // preferring tabs that are not sleeping; otherwise the next/previous tab.
  const auto& tabs = workspace_->tabs();
  if (Tab* owner = closing->owner();
      owner && workspace_->IndexOf(owner) >= 0 && !owner->close_requested()) {
    return owner;
  }
  std::vector<Tab*> remaining;
  for (const auto& t : tabs) {
    if (t.get() != closing && !t->close_requested()) remaining.push_back(t.get());
  }
  if (remaining.empty()) return nullptr;
  if (PrefEnabled(L"select_recent_on_close", true)) {
    std::vector<Tab*> awake;
    for (Tab* t : remaining) {
      if (!t->is_discarded()) awake.push_back(t);
    }
    const auto& pool = awake.empty() ? remaining : awake;
    return *std::max_element(pool.begin(), pool.end(), [](Tab* a, Tab* b) {
      return a->last_active_ms() < b->last_active_ms();
    });
  }
  const int index = workspace_->IndexOf(closing);
  for (int i = index + 1; i < static_cast<int>(tabs.size()); ++i) {
    if (!tabs[static_cast<size_t>(i)]->close_requested()) return tabs[static_cast<size_t>(i)].get();
  }
  for (int i = index - 1; i >= 0; --i) {
    if (!tabs[static_cast<size_t>(i)]->close_requested()) return tabs[static_cast<size_t>(i)].get();
  }
  return nullptr;
}

void BrowserWindow::OnTabMovedAway(Tab* tab) {
  if (tab != active_essential_ || closing_) return;
  active_essential_ = nullptr;
  Tab* back = workspace_ ? workspace_->active_tab() : nullptr;
  if (workspace_) workspace_->set_active_tab(nullptr);
  if (back) {
    ActivateTab(back);
  } else if (workspace_ && !workspace_->tabs().empty()) {
    ActivateTab(workspace_->tabs().front().get());
  } else {
    OpenEmptySpaceTab();
  }
  Layout();
}

Workspace* BrowserWindow::essentials() const {
  return workspace_ ? workspace_->essentials_space()->essentials() : nullptr;
}

bool BrowserWindow::IsEssential(const Tab* tab) const {
  const Workspace* grid = essentials();
  return grid && tab && grid->IndexOf(tab) >= 0;
}

void BrowserWindow::RevealTab(Tab* tab) {
  Workspace* ws = tab ? tab->workspace() : nullptr;
  if (!ws) return;
  if (IsEssential(tab)) {
    ActivateTab(tab);  // in this window's grid
    return;
  }
  Workspace* space = ws->is_essentials() ? ws->owner() : ws;
  if (!space) return;
  if (space != workspace_) SwitchWorkspace(space);
  if (BrowserWindow* w = space->window()) w->ActivateTab(tab);
}

std::vector<Workspace*> BrowserWindow::Spaces() const {
  if (profile_->is_private()) {
    std::vector<Workspace*> own;
    for (const auto& ws : profile_->workspaces()) own.push_back(ws.get());
    return own;
  }
  std::vector<Workspace*> spaces = Application::Get()->Spaces();
  // Linked spaces are listed only in their own window, after their parent.
  if (linked_space_) {
    auto at = std::find(spaces.begin(), spaces.end(), linked_space_->parent());
    spaces.insert(at == spaces.end() ? at : at + 1, linked_space_);
  }
  return spaces;
}

std::vector<Workspace*> BrowserWindow::LinkedChildren() const {
  std::vector<Workspace*> out;
  if (!workspace_ || workspace_->is_linked() || workspace_->is_essentials()) return out;
  for (const auto& ls : workspace_->profile()->linked_spaces()) {
    if (ls->parent() == workspace_) out.push_back(ls.get());
  }
  return out;
}

int BrowserWindow::LinkedIndex(int64_t linked_id) const {
  const auto children = LinkedChildren();
  for (size_t i = 0; i < children.size(); ++i) {
    if (children[i]->id() == linked_id) return static_cast<int>(i);
  }
  return -1;
}

void BrowserWindow::AdoptLinkedSpace(Workspace* ws) {
  if (!ws || !ws->is_linked() || !ws->parent() || Application::Get()->WindowOwningLinked(ws)) return;
  linked_space_ = ws;
  Layout();
}

void BrowserWindow::ForgetSpace(Workspace* ws) {
  pins_collapsed_.erase(ws);
  last_essential_.erase(ws);
  if (linked_space_ == ws) linked_space_ = nullptr;
  if (theme_space_id_ && ws->id() == theme_space_id_) CloseThemePicker();
}

void BrowserWindow::DetachWorkspace() {
  CloseGlance();
  CancelRename();
  for (Tab* t : content_shown_) t->Hide();
  content_shown_.clear();
  if (active_essential_) {
    active_essential_->Hide();
    active_essential_ = nullptr;
  }
  if (workspace_) {
    for (const auto& t : workspace_->tabs()) t->Hide();
    workspace_->set_window(nullptr);
    workspace_ = nullptr;
  }
  linked_space_ = nullptr;
  media_tab_ = media_dismissed_ = nullptr;
}

BrowserWindow* BrowserWindow::TearOffTab(Tab* tab, std::optional<POINT> screen) {
  if (!tab || !workspace_ || workspace_->IndexOf(tab) < 0 || profile_->is_private() || closing_ ||
      IsGlanceTab(tab) || tab->empty_placeholder()) {
    return nullptr;
  }
  Application* app = Application::Get();
  // Tearing off from a linked window links to the same parent (no chains).
  Workspace* parent = workspace_->is_linked() && workspace_->parent() ? workspace_->parent() : workspace_;
  Workspace* linked = app->CreateLinkedSpace(parent);
  if (!linked) return nullptr;
  // As big as this window; where the drag ended, or a little offset.
  WINDOWPLACEMENT wp{sizeof(wp)};
  ::GetWindowPlacement(hwnd_, &wp);
  RECT bounds;
  ::GetWindowRect(hwnd_, &bounds);
  WindowRecord place;
  place.w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
  place.h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
  if (screen) {
    place.x = screen->x - static_cast<int>(std::lround(px(80.f)));
    place.y = screen->y - static_cast<int>(std::lround(px(16.f)));
  } else {
    const int step = static_cast<int>(std::lround(px(40.f)));
    place.x = (::IsZoomed(hwnd_) ? wp.rcNormalPosition.left : bounds.left) + step;
    place.y = (::IsZoomed(hwnd_) ? wp.rcNormalPosition.top : bounds.top) + step;
  }
  // The tab moves first, so the new window opens on it: its live browser is
  // reparented there and keeps its page (state, history, scroll).
  OnTabRemoving(tab);
  std::unique_ptr<Tab> owned = workspace_->RemoveTab(tab);
  owned->Hide();
  Tab* moved = linked->InsertTab(std::move(owned), -1);
  moved->MarkDirty();
  linked->set_active_tab(moved);
  place.active_tab_id = moved->id();
  BrowserWindow* window = app->OpenWindow(profile_, linked, &place);
  if (!window) {
    // No window: the tab comes back.
    std::unique_ptr<Tab> back = linked->RemoveTab(moved);
    workspace_->InsertTab(std::move(back), -1);
    app->ReturnLinkedSpace(linked, false, nullptr);
    OnTabsChanged();
    return nullptr;
  }
  OnTabsChanged();
  ::SetForegroundWindow(window->hwnd_);
  return window;
}

void BrowserWindow::FocusLinkedSpace(Workspace* linked) {
  if (!linked) return;
  Application* app = Application::Get();
  BrowserWindow* w = linked->window() ? linked->window() : app->WindowOwningLinked(linked);
  if (!w) {
    w = app->OpenWindow(linked->profile(), linked, nullptr);
    if (!w) return;
  } else if (w->workspace_ != linked) {
    w->SwitchWorkspace(linked);
  }
  if (::IsIconic(w->hwnd_)) ::ShowWindow(w->hwnd_, SW_RESTORE);
  ::SetForegroundWindow(w->hwnd_);
}

void BrowserWindow::ShowLinkedMenu(Workspace* linked, POINT pt) {
  if (!linked) return;
  HMENU menu = ::CreatePopupMenu();
  ::AppendMenuW(menu, MF_STRING, kCmdLinkedShow, L"Show Window");
  ::AppendMenuW(menu, MF_STRING, kCmdLinkedBringBack, L"Bring Tabs Back");
  ::AppendMenuW(menu, MF_STRING, kCmdLinkedClose, L"Close Window");
  const int64_t id = linked->id();
  const UINT cmd = static_cast<UINT>(ui::TrackMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, hwnd_, palette_));
  ::DestroyMenu(menu);
  Application* app = Application::Get();
  linked = app->FindSpace(id);  // it may have gone while the menu was open
  if (!linked || !linked->is_linked()) return;
  switch (cmd) {
    case kCmdLinkedShow:
      FocusLinkedSpace(linked);
      break;
    case kCmdLinkedBringBack:
      app->BringBackLinkedSpace(linked);
      break;
    case kCmdLinkedClose:
      // As closing it: its tabs come back in a folder.
      if (BrowserWindow* w = app->WindowOwningLinked(linked)) {
        ::PostMessageW(w->hwnd_, WM_CLOSE, 0, 0);
      } else {
        app->ReturnLinkedSpace(linked, /*into_folder=*/true, nullptr);
      }
      break;
    default:
      break;
  }
}

Workspace* BrowserWindow::FindSpace(int64_t id) const {
  return profile_->is_private() ? profile_->FindWorkspace(id) : Application::Get()->FindSpace(id);
}

std::vector<Workspace*> BrowserWindow::TabContainers() const {
  if (profile_->is_private()) return profile_->containers();
  std::vector<Workspace*> out;
  for (const auto& p : Application::Get()->profiles()) {
    for (Workspace* ws : p->containers()) out.push_back(ws);
  }
  return out;
}

std::wstring BrowserWindow::SpaceLabel(const Workspace* ws) const {
  const auto& profiles = Application::Get()->profiles();
  if (ws->profile()->is_private() || profiles.empty() || ws->profile() == profiles.front().get()) {
    return ws->record().name;
  }
  return ws->record().name + L" (" + ws->profile()->record().name + L")";
}

void BrowserWindow::OnProfileChanged() {
  // The panels show pages of the previous profile (its history, downloads,
  // site data, boosts): they close and are built again in the new one.
  if (library_open_ || library_browser_ || library_creating_) {
    library_open_ = false;
    library_slide_.Jump(0.f);
    PlaceLibrary();
    if (library_browser_) library_browser_->GetHost()->CloseBrowser(true);
  }
  CloseSiteData();
  CloseBoostEditor();
  CloseThemePicker();
  suggest_text_.clear();
  suggest_results_.clear();
}

bool BrowserWindow::MoveSpaceToProfile(Workspace* ws, Profile* profile) {
  if (!ws || !profile || !Application::Get()->MoveSpaceToProfile(ws, profile)) return false;
  ShowToast({L"space-profile", ws->record().name + L" now uses " + profile->record().name +
                                   L": its tabs reload signed out",
             L"", L"", 4000});
  return true;
}

void BrowserWindow::OnSpaceProfileChanged(Workspace* ws) {
  (void)ws;
  if (closing_ || !workspace_) return;
  if (workspace_->profile() != profile_) {
    profile_ = workspace_->profile();
    OnProfileChanged();
    RefreshBackdrop();
    UpdateAddressBar(true);
    UpdateTitle();
  }
  Layout();  // the switcher's profile badges
}

void BrowserWindow::ForgetProfile(Profile* profile) {
  // Pointers this window keeps to the profile's spaces and tabs.
  if (linked_space_ && linked_space_->profile() == profile) linked_space_ = nullptr;
  for (Workspace* ws : profile->containers()) {
    pins_collapsed_.erase(ws);
    last_essential_.erase(ws);
    for (const auto& t : ws->tabs()) {
      favicons_.erase(t.get());
      pinned_favicons_.erase(t.get());
      overlay_favicons_.erase(t.get());
      overlay_pinned_favicons_.erase(t.get());
      if (media_tab_ == t.get()) media_tab_ = nullptr;
      if (media_dismissed_ == t.get()) media_dismissed_ = nullptr;
      if (focus_guard_tab_ == t.get()) focus_guard_tab_ = nullptr;
      if (rename_tab_ == t.get()) CancelRename();
    }
  }
  for (Workspace* ws : Application::Get()->AllContainers()) {
    for (const auto& t : ws->tabs()) {
      if (t->owner() && t->owner()->profile() == profile) t->set_owner(nullptr);
    }
  }
}

bool BrowserWindow::CanAddToEssentials(const Tab* tab) const {
  // canEssentialBeAdded: up to zen.tabs.essentials.max; tabs in a folder offer
  // no "Add to Essentials" (the menu item is hidden for them).
  if (!tab || IsEssential(tab) || tab->record().group_id) return false;
  const Workspace* ess = essentials();
  return !ess || static_cast<int>(ess->tabs().size()) < kMaxEssentials;
}

bool BrowserWindow::AddToEssentials(Tab* tab) {
  if (!CanAddToEssentials(tab) || !workspace_ || workspace_->IndexOf(tab) < 0) return false;
  Workspace* ess = workspace_->essentials_space()->EnsureEssentials();
  if (!ess) return false;
  const bool was_active = tab == active_tab();
  std::unique_ptr<Tab> owned = workspace_->RemoveTab(tab);
  if (was_active) {
    // The space remembers another tab for later; the essential stays on screen.
    Tab* fallback = nullptr;
    for (const auto& t : workspace_->tabs()) {
      if (!fallback || t->last_active_ms() > fallback->last_active_ms()) fallback = t.get();
    }
    workspace_->set_active_tab(fallback);
  }
  owned->mutable_record().pinned = true;
  owned->set_group(0);
  if (owned->pinned_url().empty()) owned->SetPinnedUrl(owned->url());
  Tab* moved = ess->InsertTab(std::move(owned), -1);
  moved->MarkDirty();
  if (was_active) active_essential_ = moved;
  Layout();
  return true;
}

void BrowserWindow::RemoveFromEssentials(Tab* tab) {
  // removeEssentials(tab, unpin = true): back into the current space, unpinned.
  Workspace* ess = essentials();
  if (!ess || ess->IndexOf(tab) < 0 || !workspace_) return;
  const bool was_active = tab == active_essential_;
  for (BrowserWindow* w : Application::Get()->WindowsShowing(ess)) {
    if (w != this) w->OnTabMovedAway(tab);
  }
  std::unique_ptr<Tab> owned = ess->RemoveTab(tab);
  owned->mutable_record().pinned = false;
  owned->SetPinnedUrl(L"");
  Tab* moved = workspace_->InsertTab(std::move(owned), workspace_->PinnedCount());
  moved->MarkDirty();
  if (moved->window() && moved->window() != this) moved->Reparent(this);
  if (was_active) {
    active_essential_ = nullptr;
    workspace_->set_active_tab(moved);
  }
  Layout();
}

void BrowserWindow::OnTabsChanged() {
  if (closing_ || !workspace_) return;
  if (workspace_->tabs().empty()) {
    OpenEmptySpaceTab();
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
  // A click into another pane of the split selects its tab (onTabSelect).
  if (tab && tab != active_tab()) {
    const auto* split = ActiveSplit();
    if (split && workspace_->SplitOf(tab) == split) ActivateTab(tab);
  }
  Invalidate();
}

// ---------------------------------------------------------------------------
// Workspaces and tabs

void BrowserWindow::ShowWorkspace(Workspace* workspace, int64_t active_tab_id, bool start_empty) {
  if (!workspace || workspace == workspace_) return;
  if (workspace->window() && workspace->window() != this) return;  // shown elsewhere
  CloseGlance();
  if (workspace_) {
    for (const auto& t : workspace_->tabs()) t->Hide();
    workspace_->set_window(nullptr);
    last_essential_[workspace_] = active_essential_;
  }
  if (active_essential_) {
    active_essential_->Hide();
    active_essential_ = nullptr;
  }
  workspace_ = workspace;
  workspace_->set_window(this);
  if (workspace_->is_linked()) linked_space_ = workspace_;
  if (workspace_->profile() != profile_) {
    profile_ = workspace_->profile();
    OnProfileChanged();
  }
  // Live tabs hosted by another window move here so that closing that window
  // cannot unload them behind this one's back.
  for (const auto& t : workspace_->tabs()) {
    if (t->window() && t->window() != this) t->Reparent(this);
  }
  scroll_ = 0.f;
  focus_row_ = -1;
  row_anim_.Jump(1.f);
  row_ghosts_.clear();
  RefreshBackdrop();
  Layout();

  Tab* target = nullptr;
  const auto& tabs = workspace_->tabs();
  for (const auto& t : tabs) {
    if (active_tab_id && t->id() == active_tab_id) target = t.get();
  }
  if (!target) target = workspace_->active_tab();
  if (start_empty) target = nullptr;
  if (!target && !tabs.empty() && !start_empty) {
    target = std::max_element(tabs.begin(), tabs.end(), [](const auto& a, const auto& b) {
               return a->last_active_ms() < b->last_active_ms();
             })->get();
  }
  // lastSelectedWorkspaceTabs: a space left on an Essential returns to it.
  if (auto it = last_essential_.find(workspace_);
      it != last_essential_.end() && it->second && IsEssential(it->second) &&
      !active_tab_id && !start_empty) {
    Tab* remembered = workspace_->active_tab();
    ActivateTab(it->second);
    if (remembered) workspace_->set_active_tab(remembered);
  } else {
    workspace_->set_active_tab(nullptr);
    if (target) {
      ActivateTab(target);
    } else {
      OpenEmptySpaceTab();
    }
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
  DropEmptyPlaceholder(tab);
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
    tab->set_owner(opener);
    ActivateTab(tab);
  } else {
    tab->EnsureBrowser(this);
  }
  DropEmptyPlaceholder(tab);
  Layout();
  return tab;
}

Tab* BrowserWindow::OpenRoutedTab(const std::wstring& url, bool activate, bool external, Tab* opener) {
  Workspace* target = Application::Get()->RouteTarget(profile_, url, external);
  if (!target || target == workspace_) {
    return opener ? OpenTabNear(opener, url, activate) : OpenTab(url, activate);
  }
  ShowToast({L"space-routed", L"New tab opened in " + target->record().name, L"", L"", 3000});
  if (BrowserWindow* other = target->window(); other && other != this) {
    return other->OpenTab(url, false);  // that space is on screen in another window
  }
  TabRecord rec;
  rec.url = url;
  rec.title = url;
  return target->InsertTab(std::make_unique<Tab>(target, rec), -1);
}

Tab* BrowserWindow::CreatePopupTab(Tab* opener, const std::wstring& url, bool activate,
                                   CefWindowInfo& info, bool as_glance) {
  if (!workspace_) return nullptr;
  // A glance needs an opener in this space that is not itself a glance.
  as_glance = as_glance && opener && workspace_->IndexOf(opener) >= 0 && !IsGlanceTab(opener);
  TabRecord rec;
  rec.url = url;
  const int index = opener ? workspace_->IndexOf(opener) + 1 : -1;
  Tab* tab = workspace_->InsertTab(std::make_unique<Tab>(workspace_, rec), index);
  if (opener && opener->record().group_id && !opener->pinned()) {
    workspace_->SetTabGroup(tab, opener->record().group_id);
  }
  tab->PrepareForPopup(this, info);
  if (as_glance) {
    // As OpenGlance: the popup page floats over its opener; closing it
    // (window.close(), the glance's own close button) returns to the opener.
    CloseGlance();
    if (active_tab() != opener) ActivateTab(opener);
    glance_ = GlanceState{opener, tab};
    ActivateTab(tab);
    tab->set_owner(opener);
    DropEmptyPlaceholder(tab);
    Layout();
    UpdateAnimationTimer(::GetTickCount64());
    return tab;
  }
  if (activate) {
    tab->set_owner(opener);
    ActivateTab(tab);
  }
  DropEmptyPlaceholder(tab);
  Layout();
  return tab;
}

void BrowserWindow::ActivateTab(Tab* tab) {
  const bool essential = IsEssential(tab);
  if (!workspace_ || !tab || (!essential && workspace_->IndexOf(tab) < 0)) return;
  Tab* previous = active_tab();
  if (previous == tab) {
    FocusContent();
    return;
  }
  // Selecting anything but the glance (its parent included) closes it.
  std::optional<GlanceState> dropped;
  if (glance_ && tab != glance_->tab) {
    dropped = glance_;
    glance_.reset();
    PlaceGlanceChrome(false);
  }
  if (previous && !dropped) previous->set_owner(nullptr);  // ShowContent hides it
  if (essential) {
    // The space keeps its own active tab underneath (it shows again when the
    // user picks it or switches back).
    active_essential_ = tab;
  } else {
    active_essential_ = nullptr;
    for (int64_t id : workspace_->GroupChain(tab)) {
      if (const TabGroupRecord* g = workspace_->FindGroup(id); g && g->collapsed) {
        TabGroupRecord expanded = *g;
        expanded.collapsed = false;
        workspace_->UpdateGroup(expanded);
      }
    }
    workspace_->set_active_tab(tab);
  }
  tab->TouchActive();
  tab->EnsureBrowser(this);
  ShowContent();
  FindBar::OnActiveTabChanged(this);  // a find bar searches one page
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
  // onTabSelect: the card hides at once for its own tab and comes back 500 ms
  // after the user leaves it.
  if (previous && previous->media().state != Tab::MediaState::State::kNone) {
    media_hidden_until_ = ::GetTickCount64() + 500;
    ::SetTimer(hwnd_, kMediaCardTimer, 520, nullptr);
  }
  UpdateMediaCard();
  Application::Get()->ScheduleSave();
  FlashSidebarIfNecessary();  // _handleTabSelect
  Invalidate();
  if (dropped && workspace_->IndexOf(dropped->tab) >= 0) {
    dropped->tab->set_owner(nullptr);
    CloseTab(dropped->tab);
  }
}

void BrowserWindow::OpenEmptySpaceTab() {
  // Zen keeps an empty tab for an empty space (zen.urlbar.open-on-startup
  // opens the URL bar on it); here a new-tab page stands in for it.
  Tab* placeholder = OpenTab(Application::kNewTabUrl, true);
  if (placeholder) placeholder->set_empty_placeholder(true);
  ShowFloatingUrlBar(/*new_tab=*/false);
  Layout();
}

void BrowserWindow::DropEmptyPlaceholder(Tab* except) {
  if (!workspace_) return;
  std::vector<Tab*> placeholders;
  for (const auto& t : workspace_->tabs()) {
    if (t.get() != except && t->empty_placeholder() && IsNewTabUrl(t->url())) {
      placeholders.push_back(t.get());
    }
  }
  for (Tab* t : placeholders) t->RequestClose();  // new-tab pages are not remembered
}

void BrowserWindow::NewTab() {
  // zen.urlbar.replace-newtab (ZenUrlbarController.handleNewTab): a new tab
  // is not created yet; the floating URL bar opens and Enter creates it.
  // Pressing Ctrl+T again closes the bar.
  if (new_tab_mode()) {
    CancelAddressBar();
    return;
  }
  if (urlbar_behavior_ == L"normal" && address_rect_.right > address_rect_.left) {
    ShowFloatingUrlBar(/*new_tab=*/true, /*select_all=*/true, PopupAnchor::kSidebar);
  } else {
    ShowFloatingUrlBar(/*new_tab=*/true);
  }
}

void BrowserWindow::CloseTab(Tab* tab) {
  if (tab) tab->RequestClose();
}

void BrowserWindow::CloseOtherTabs(Tab* keep) {
  std::vector<Tab*> victims;
  for (const auto& t : workspace_->tabs()) {
    if (t.get() != keep && !t->pinned()) victims.push_back(t.get());
  }
  Application::Get()->CloseTabsTogether(victims);
}

void BrowserWindow::CloseTabsBelow(Tab* tab) {
  std::vector<Tab*> victims;
  const auto& tabs = workspace_->tabs();
  for (size_t i = static_cast<size_t>(workspace_->IndexOf(tab)) + 1; i < tabs.size(); ++i) {
    if (!tabs[i]->pinned()) victims.push_back(tabs[i].get());
  }
  Application::Get()->CloseTabsTogether(victims);
}

bool BrowserWindow::ClearUnpinnedTabs() {
  // ZenSpaceManager.closeAllUnpinnedTabs (the "Clear" button, Ctrl+Shift+K).
  // #getClosableTabs spares the selected tab and tabs playing audio or using
  // the camera/microphone, unless that would leave nothing to close.
  if (!workspace_) return false;
  std::vector<Tab*> unpinned;
  for (const auto& t : workspace_->tabs()) {
    if (!t->pinned()) unpinned.push_back(t.get());
  }
  std::vector<Tab*> closable;
  for (Tab* t : unpinned) {
    if (t == active_tab() || t->media_capture() || t->media_reported_playing()) continue;
    closable.push_back(t);
  }
  if (closable.empty()) closable = unpinned;
  if (closable.empty()) return false;
  Application::Get()->CloseTabsTogether(closable);
  ShowToast({L"tabs-closed", L"Tabs Closed! Use", L"Ctrl+Shift+T", L"to undo.", 2000});
  return true;
}

void BrowserWindow::DuplicateTab(Tab* tab) { OpenTabNear(tab, tab->url(), true); }

void BrowserWindow::TogglePin(Tab* tab) {
  CaptureRows();  // rows glide to their new places (the pinned separator moves with them)
  if (workspace_ && !tab->pinned()) workspace_->RemoveFromSplit(tab);  // splits hold no pinned tabs
  if (!tab->pinned()) workspace_->SetTabGroup(tab, 0);
  tab->mutable_record().pinned = !tab->pinned();
  // Pinning remembers the page the tab belongs to (Zen's initial state).
  tab->SetPinnedUrl(tab->pinned() ? tab->url() : std::wstring());
  // Re-insert at the boundary between pinned and regular tabs.
  const int target = tab->pinned() ? workspace_->PinnedCount() - 1 : workspace_->PinnedCount();
  workspace_->MoveTab(tab, target);
  Layout();
  AnimateRows();
}

void BrowserWindow::MoveTabToWorkspace(Tab* tab, Workspace* target) {
  if (!target || !workspace_ || target == workspace_ || workspace_->IndexOf(tab) < 0 ||
      target->is_essentials() || !CanShareTabs(profile_, target->profile())) {
    return;
  }
  if (target->window() && target->window() != this) {
    MoveTabToWindow(tab, target->window());
    return;
  }
  // A Chromium browser keeps the storage it was created with: a tab that
  // changes profile unloads and loads again there (signed out).
  const bool other_profile = target->profile() != profile_;
  OnTabRemoving(tab);
  std::unique_ptr<Tab> owned = workspace_->RemoveTab(tab);
  owned->Hide();
  Tab* moved = target->InsertTab(std::move(owned), -1);
  moved->MarkDirty();  // persists the new workspace_id
  if (other_profile) {
    moved->UnloadForProfileChange();
    ShowToast({L"tab-profile", L"Moved to " + target->profile()->record().name + L": it reloads signed out",
               L"", L"", 4000});
  }
  OnTabsChanged();
}

void BrowserWindow::MoveTabToWindow(Tab* tab, BrowserWindow* target) {
  if (!target || target == this || !target->workspace_ || !workspace_ || workspace_->IndexOf(tab) < 0 ||
      !CanShareTabs(profile_, target->profile_)) {
    return;
  }
  const bool other_profile = target->profile_ != profile_;
  OnTabRemoving(tab);
  std::unique_ptr<Tab> owned = workspace_->RemoveTab(tab);
  owned->Hide();
  Tab* moved = target->workspace_->InsertTab(std::move(owned), -1);
  moved->MarkDirty();
  if (moved->has_browser() || moved->load_state() != Tab::LoadState::kUnloaded) {
    moved->Reparent(target);  // the page keeps its state (same profile)
  }
  // Another profile: the browser's storage is fixed, so it loads again there.
  if (other_profile) moved->UnloadForProfileChange();
  target->ActivateTab(moved);
  target->DropEmptyPlaceholder(moved);
  target->Layout();
  if (other_profile) {
    target->ShowToast({L"tab-profile", L"Moved to " + target->profile_->record().name +
                                           L": it reloads signed out",
                       L"", L"", 4000});
  }
  ::SetForegroundWindow(target->hwnd_);
  OnTabsChanged();
}

void BrowserWindow::SelectRelativeTab(int delta) {
  // advanceSelectedTab over the visible strip: the Essentials, then the
  // space's tabs. zen.tabs.ctrl-tab.ignore-essential-tabs keeps it within
  // the group the current tab is in.
  if (!workspace_) return;
  std::vector<Tab*> tabs;
  const bool within = PrefEnabled(L"ctrl_tab_ignore_essentials", false);
  const bool from_essential = active_essential_ != nullptr;
  if (const Workspace* ess = essentials(); ess && (!within || from_essential)) {
    for (const auto& t : ess->tabs()) tabs.push_back(t.get());
  }
  if (!within || !from_essential) {
    for (const auto& t : workspace_->tabs()) tabs.push_back(t.get());
  }
  if (tabs.empty()) return;
  const int n = static_cast<int>(tabs.size());
  const auto at = std::find(tabs.begin(), tabs.end(), active_tab());
  const int index = at == tabs.end() ? 0 : static_cast<int>(at - tabs.begin());
  for (int step = 1; step <= n; ++step) {
    Tab* t = tabs[static_cast<size_t>(((index + delta * step) % n + n) % n)];
    if (t->empty_placeholder() || (glance_ && t == glance_->tab)) continue;
    // zen.tabs.ctrl-tab.ignore-pending-tabs: skip tabs that are not loaded.
    if (ctrl_tab_skip_pending_ && t != active_tab() &&
        (t->is_discarded() || t->load_state() == Tab::LoadState::kUnloaded)) {
      continue;
    }
    ActivateTab(t);
    return;
  }
}

void BrowserWindow::SelectTabIndex(int index) {
  if (!workspace_ || workspace_->tabs().empty()) return;
  const int n = static_cast<int>(workspace_->tabs().size());
  index = (index < 0 || index >= n) ? n - 1 : index;
  ActivateTab(workspace_->tabs()[static_cast<size_t>(index)].get());
}

void BrowserWindow::SelectRelativeWorkspace(int delta) {
  const std::vector<Workspace*> workspaces = Spaces();
  const int n = static_cast<int>(workspaces.size());
  if (n < 2) return;
  int index = 0;
  for (int i = 0; i < n; ++i) {
    if (workspaces[static_cast<size_t>(i)] == workspace_) index = i;
  }
  // zen.workspaces.wrap-around-navigation (default on): past the last space
  // comes the first. Spaces shown in other windows are skipped.
  for (int step = 1; step < n; ++step) {
    const int i = ((index + delta * step) % n + n) % n;
    Workspace* ws = workspaces[static_cast<size_t>(i)];
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
  const std::vector<Workspace*> spaces = Spaces();
  int from = -1;
  int to = -1;
  for (size_t i = 0; i < spaces.size(); ++i) {
    if (spaces[i] == workspace_) from = static_cast<int>(i);
    if (spaces[i] == target) to = static_cast<int>(i);
  }
  if (from >= 0 && to >= 0) {
    BeginSpaceSwitchAnimation(orbit::SpaceSwitchStep(from, to, static_cast<int>(spaces.size())));
  }
  ShowWorkspace(target, 0);
}

void BrowserWindow::BeginSpaceSwitchAnimation(int step) {
  space_snapshot_.Reset();
  space_slide_.Jump(1.f);
  const uint32_t duration = ui::AnimMs(kSpaceSwitchMs);
  // The sidebar paints on the compact overlay's device while that shows.
  const bool on_overlay = overlay_visible_;
  if (duration == 0 || step == 0 || !workspace_ || chrome_hidden() || !sidebar_visible() ||
      (on_overlay ? !overlay_rt_ : !EnsureRenderTarget())) {
    return;
  }
  const D2D1_RECT_F area = SpaceArea();
  if (area.right <= area.left || area.bottom <= area.top) return;
  auto lend_overlay_resources = [this] {
    rt_.Swap(overlay_rt_);
    brush_.Swap(overlay_brush_);
    favicons_.swap(overlay_favicons_);
    pinned_favicons_.swap(overlay_pinned_favicons_);
    ext_icons_.swap(overlay_ext_icons_);
  };
  if (on_overlay) lend_overlay_resources();
  // A compatible target shares the device, so brush_ and the favicon bitmaps
  // can paint into it.
  Microsoft::WRL::ComPtr<ID2D1BitmapRenderTarget> target;
  bool ok = SUCCEEDED(rt_->CreateCompatibleRenderTarget(
      D2D1::SizeF(area.right - area.left, area.bottom - area.top), &target));
  if (ok) {
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0.f));
    target->SetTransform(D2D1::Matrix3x2F::Translation(-area.left, -area.top));
    PaintSpaceArea(target.Get());
    ok = SUCCEEDED(target->EndDraw()) && SUCCEEDED(target->GetBitmap(&space_snapshot_));
  }
  if (on_overlay) lend_overlay_resources();  // and back
  if (!ok) {
    space_snapshot_.Reset();
    return;
  }
  space_snapshot_rect_ = area;
  space_old_backdrop_ = backdrop_;
  space_step_ = step > 0 ? 1 : -1;
  const uint64_t now = ::GetTickCount64();
  space_slide_.Jump(swipe_handoff_);  // a swipe already moved it part of the way
  space_slide_.Start(1.f, now, duration, 0, orbit::Transition::Curve::kSpring);
  UpdateAnimationTimer(now);
}

bool BrowserWindow::space_switch_animating() const {
  return space_snapshot_ && space_slide_.Running(::GetTickCount64());
}

void BrowserWindow::UpdateAnimationTimer(uint64_t now) {
  const bool running = ws_actions_fade_.Running(now) || splitter_fade_.Running(now) ||
                       space_slide_.Running(now) || compact_slide_.Running(now) ||
                       compact_hiding_ || caption_moving_ || row_anim_.Running(now) ||
                       library_slide_.Running(now) || glance_animating() || swipe_back_.Running(now) ||
                       indent_anim_.Running(now) || pinned_sep_.Running(now) ||
                       // until a closing panel is hidden (and its page released)
                       (library_host_ && !library_open_ && ::IsWindowVisible(library_host_)) ||
                       std::any_of(footer_fades_.begin(), footer_fades_.end(),
                                   [now](const auto& f) { return f.second.Running(now); });
  if (running == anim_timer_running_) return;
  anim_timer_running_ = running;
  if (running) {
    ::SetTimer(hwnd_, kAnimTimer, kAnimFrameMs, nullptr);
  } else {
    ::KillTimer(hwnd_, kAnimTimer);
  }
}

// ---------------------------------------------------------------------------
// Library panel (ZenLibrary)

float BrowserWindow::LibraryWidth() const {
  // Zen: clamp(418px, var(--zen-library-content-width), 95vw). Orbit keeps it
  // closer to the sidebar's scale: 440, at most 95% of the window.
  return std::clamp(440.f, 340.f, std::max(340.f, Width() * 0.95f));
}

int BrowserWindow::library_browser_id() const {
  return library_browser_ ? library_browser_->GetIdentifier() : 0;
}

void BrowserWindow::ToggleLibrary() {
  if (library_open_) {
    CloseLibrary();
  } else {
    OpenLibrary();
  }
}

void BrowserWindow::ToggleLibrarySection(const std::wstring& section) {
  Store& store = Application::Get()->store();
  const bool same = store.GetPref(Store::kGlobalScope, L"library_last_tab").value_or(L"history") == section;
  if (library_open_ && same) {
    CloseLibrary();
    return;
  }
  store.SetPref(Store::kGlobalScope, L"library_last_tab", section);
  if (library_browser_) {
    const std::wstring js = L"window.orbitShowSection && orbitShowSection('" + section + L"')";
    library_browser_->GetMainFrame()->ExecuteJavaScript(js, "", 0);
  }
  OpenLibrary();
}

void BrowserWindow::OpenLibrary() {
  if (library_open_ || closing_ || chrome_hidden()) return;
  CloseGlance();
  if (!library_host_) {
    library_host_ = ::CreateWindowExW(0, kLibraryHostClass, L"Library",
                                      WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0,
                                      hwnd_, nullptr, ModuleInstance(), nullptr);
  }
  library_open_ = true;
  PlaceLibrary();
  if (!library_browser_ && !library_creating_) {
    // zen.library.last-tab; the page takes the backdrop's colours.
    const std::wstring last =
        Application::Get()->store().GetPref(Store::kGlobalScope, L"library_last_tab").value_or(L"history");
    auto rgb = [](const D2D1_COLOR_F& c) {
      auto byte = [](float v) { return static_cast<unsigned>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
      return (byte(c.r) << 16) | (byte(c.g) << 8) | byte(c.b);
    };
    wchar_t colors[64];
    swprintf(colors, 64, L"&bg=%06X&bg2=%06X", rgb(backdrop_.from), rgb(backdrop_.to));
    const std::wstring url = L"orbit://library/?section=" + last + colors + (right_side_ ? L"&right=1" : L"");
    RECT rc;
    ::GetClientRect(library_host_, &rc);
    CefWindowInfo info;
    info.SetAsChild(library_host_, CefRect(0, 0, rc.right, rc.bottom));
    info.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
    CefBrowserSettings settings;
    library_client_ = new LibraryClient(this, LibraryClient::kLibrary);
    library_creating_ = CefBrowserHost::CreateBrowser(info, library_client_, url, settings, nullptr,
                                                      profile_->request_context());
  }
  const uint64_t now = ::GetTickCount64();
  library_slide_.Start(1.f, now, ui::AnimMs(kLibrarySlideMs), 0, orbit::Transition::Curve::kSpring);
  UpdateAnimationTimer(now);
  if (library_browser_) {
    // Reopened before its page was released: show current data.
    library_browser_->GetMainFrame()->ExecuteJavaScript("window.orbitRefresh && orbitRefresh()", "", 0);
    library_browser_->GetHost()->SetFocus(true);
  }
  Layout();
}

void BrowserWindow::CloseLibrary() {
  if (!library_open_) return;
  library_open_ = false;
  const uint64_t now = ::GetTickCount64();
  library_slide_.Start(0.f, now, ui::AnimMs(kLibrarySlideMs), 0, orbit::Transition::Curve::kSpring);
  UpdateAnimationTimer(now);
  PlaceLibrary();
  Layout();
  FocusContent();
}

void BrowserWindow::PlaceLibrary() {
  if (!library_host_) return;
  const float p = library_slide_.Value(::GetTickCount64());
  if (!library_open_ && p <= 0.f) {
    ::ShowWindow(library_host_, SW_HIDE);
    // Closed for good: the page is rebuilt next time (Zen unmounts it too).
    if (library_browser_ && !library_slide_.Running(::GetTickCount64())) {
      library_browser_->GetHost()->CloseBrowser(true);
    }
    return;
  }
  const float w = LibraryWidth();
  const float W = Width();
  const RECT r = right_side_ ? ToPx({W - p * w, 0.f, W - p * w + w, Height()})
                             : ToPx({-(1.f - p) * w, 0.f, p * w, Height()});
  ::SetWindowPos(library_host_, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void BrowserWindow::OnLibraryCreated(CefRefPtr<CefBrowser> browser) {
  library_creating_ = false;
  library_browser_ = browser;
  if (HWND child = browser->GetHost()->GetWindowHandle(); child && library_host_) {
    RECT rc;
    ::GetClientRect(library_host_, &rc);
    ::SetWindowPos(child, nullptr, 0, 0, rc.right, rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
  }
  if (!library_open_ || closing_) {
    browser->GetHost()->CloseBrowser(true);  // closed again while it was being created
  } else {
    browser->GetHost()->SetFocus(true);
  }
}

void BrowserWindow::OnLibraryClosed(CefRefPtr<CefBrowser> browser) {
  if (library_browser_ && library_browser_->IsSame(browser)) library_browser_ = nullptr;
  library_creating_ = false;
  if (closing_) Application::Get()->OnWindowBrowserClosed();
}

void BrowserWindow::LibraryShowSpace(int64_t space_id, int tab_index) {
  // ZenLibrarySpacesSection: a space card switches to it; a tab row also
  // selects that tab.
  Workspace* target = FindSpace(space_id);
  if (!target) return;
  CloseLibrary();
  if (target != workspace_) SwitchWorkspace(target);
  if (workspace_ != target) return;  // shown in another window
  if (tab_index >= 0 && tab_index < static_cast<int>(target->tabs().size())) {
    ActivateTab(target->tabs()[static_cast<size_t>(tab_index)].get());
  }
}

// ---------------------------------------------------------------------------
// Media card (ZenMediaController)

void BrowserWindow::UpdateMediaCard() {
  // The most recently started media of a tab in this window's profile that
  // is not the one on screen.
  Tab* best = nullptr;
  ULONGLONG best_start = 0;
  const bool waiting = ::GetTickCount64() < media_hidden_until_;
  for (Workspace* ws : TabContainers()) {
    for (const auto& t : ws->tabs()) {
      if (t->close_requested()) continue;  // about to go away
      const Tab::MediaState m = t->media();
      if (m.state == Tab::MediaState::State::kNone || t.get() == active_tab()) continue;
      if (t.get() == media_dismissed_) {
        if (m.state != Tab::MediaState::State::kPlaying || m.started_at <= media_dismissed_start_) continue;
        media_dismissed_ = nullptr;  // it plays again
      }
      if (!best || m.started_at > best_start) {
        best = t.get();
        best_start = m.started_at;
      }
    }
  }
  if (waiting) best = nullptr;
  if (collapsed_ || !sidebar_visible()) best = nullptr;  // only the expanded sidebar has room
  if (best != media_tab_) {
    media_tab_ = best;
    Layout();
  } else if (media_tab_) {
    Invalidate();
  }
}

void BrowserWindow::MediaCommand(const char* command) {
  Tab* tab = media_tab_;
  if (!tab) return;
  const std::string cmd = command;
  if (cmd == "focus") {
    RevealTab(tab);  // .zen-media-focus-button: show the tab that plays
    return;
  }
  if (cmd == "mute") {
    tab->SetMuted(!tab->muted());
    Invalidate();
    return;
  }
  if (cmd == "close") {
    // onClose: pause it and hide the card until it plays again.
    if (tab->media().state == Tab::MediaState::State::kPlaying) MediaCommand("toggle");
    media_dismissed_ = tab;
    media_dismissed_start_ = tab->media().started_at;
    UpdateMediaCard();
    return;
  }
  CefRefPtr<CefBrowser> browser = tab->browser();
  if (!browser) return;
  std::string js;
  if (cmd == "toggle") {
    js = tab->media().state == Tab::MediaState::State::kPlaying
             ? "window.__orbitPausedMedia = Array.from(document.querySelectorAll('video,audio'))"
               ".filter(m => !m.paused); window.__orbitPausedMedia.forEach(m => m.pause());"
             : "(window.__orbitPausedMedia || Array.from(document.querySelectorAll('video,audio'))"
               ".filter(m => m.paused && m.currentTime > 0).slice(0, 1)).forEach(m => m.play());";
  } else if (cmd.rfind("seek:", 0) == 0) {
    const double frac = std::clamp(atof(cmd.c_str() + 5), 0.0, 1.0);
    js = "Array.from(document.querySelectorAll('video,audio')).filter(m => m.currentTime > 0 && isFinite(m.duration))"
         ".forEach(m => m.currentTime = m.duration * " + std::to_string(frac) + ");";
  } else {
    return;
  }
  std::vector<CefString> frames;
  browser->GetFrameIdentifiers(frames);
  for (const auto& id : frames) {
    if (CefRefPtr<CefFrame> frame = browser->GetFrameByIdentifier(id)) frame->ExecuteJavaScript(js, "", 0);
  }
}

void BrowserWindow::SetSpaceColor(uint32_t color) {
  if (!workspace_) return;
  workspace_->mutable_record().color = color;
  if (!profile_->is_private()) Application::Get()->store().UpdateWorkspace(workspace_->record());
  RefreshBackdrop();
  Invalidate();
}

void BrowserWindow::LibraryNewSpace() {
  CloseLibrary();
  NewWorkspace();
}

// ---------------------------------------------------------------------------
// Site data panel (ZenSiteDataPanel)

void BrowserWindow::OpenSiteData() {
  Tab* tab = active_tab();
  if (!tab || closing_) return;
  if (site_data_open()) {
    CloseSiteData();  // the button toggles it
    return;
  }
  if (!site_popup_) {
    site_popup_ = ::CreateWindowExW(WS_EX_TOOLWINDOW, kSiteDataClass, L"Site information", WS_POPUP,
                                    0, 0, 0, 0, hwnd_, nullptr, ModuleInstance(), this);
    if (!site_popup_) return;
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    ::DwmSetWindowAttribute(site_popup_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    const BOOL dark = palette_.dark ? TRUE : FALSE;
    ::DwmSetWindowAttribute(site_popup_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  }
  // Below the site-info button, like Zen's panel under the identity box.
  POINT at{static_cast<LONG>(std::lround(px(site_info_.left))),
           static_cast<LONG>(std::lround(px(address_rect_.bottom + 6.f)))};
  ::ClientToScreen(hwnd_, &at);
  const int w = static_cast<int>(std::lround(px(320.f)));
  const int hgt = static_cast<int>(std::lround(px(420.f)));
  ::SetWindowPos(site_popup_, HWND_TOP, at.x, at.y, w, hgt, SWP_SHOWWINDOW);
  if (site_browser_) {
    site_browser_->GetMainFrame()->LoadURL("orbit://sitedata/");  // the current tab's data
  } else if (!site_creating_) {
    RECT rc;
    ::GetClientRect(site_popup_, &rc);
    CefWindowInfo info;
    info.SetAsChild(site_popup_, CefRect(0, 0, rc.right, rc.bottom));
    info.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
    CefBrowserSettings settings;
    site_client_ = new LibraryClient(this, LibraryClient::kSiteData);
    site_creating_ = CefBrowserHost::CreateBrowser(info, site_client_, "orbit://sitedata/", settings,
                                                   nullptr, profile_->request_context());
  }
  ::SetForegroundWindow(site_popup_);
  if (site_browser_) site_browser_->GetHost()->SetFocus(true);
}

void BrowserWindow::OpenThemePicker(Workspace* ws) {
  // openThemePickerForWorkspace: next to the sidebar's top, for |ws|.
  if (!ws || closing_ || profile_->is_private()) return;
  theme_space_id_ = ws->id();
  if (!theme_popup_) {
    theme_popup_ = ::CreateWindowExW(WS_EX_TOOLWINDOW, kSiteDataClass, L"Edit Theme", WS_POPUP, 0, 0, 0, 0,
                                     hwnd_, nullptr, ModuleInstance(), this);
    if (!theme_popup_) return;
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    ::DwmSetWindowAttribute(theme_popup_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
  }
  const BOOL dark = palette_.dark ? TRUE : FALSE;
  ::DwmSetWindowAttribute(theme_popup_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  // position: "topright topleft" (left of the sidebar with tabs on the right).
  const int w = static_cast<int>(std::lround(px(380.f)));
  const int hgt = static_cast<int>(std::lround(px(500.f)));
  const float edge = right_side_ ? SidebarLeft() - 10.f - 380.f : SidebarLeft() + SidebarPanelWidth() + 10.f;
  POINT at{static_cast<LONG>(std::lround(px(std::max(edge, 0.f)))), static_cast<LONG>(std::lround(px(8.f)))};
  ::ClientToScreen(hwnd_, &at);
  ::SetWindowPos(theme_popup_, HWND_TOP, at.x, at.y, w, hgt, SWP_SHOWWINDOW);
  if (theme_browser_) {
    theme_browser_->GetMainFrame()->ExecuteJavaScript("window.orbitReload && orbitReload()", "", 0);
  } else if (!theme_creating_) {
    RECT rc;
    ::GetClientRect(theme_popup_, &rc);
    CefWindowInfo info;
    info.SetAsChild(theme_popup_, CefRect(0, 0, rc.right, rc.bottom));
    info.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
    CefBrowserSettings settings;
    theme_client_ = new LibraryClient(this, LibraryClient::kTheme);
    theme_creating_ = CefBrowserHost::CreateBrowser(info, theme_client_, "orbit://theme/", settings, nullptr,
                                                    profile_->request_context());
  }
  ::SetForegroundWindow(theme_popup_);
  if (theme_browser_) theme_browser_->GetHost()->SetFocus(true);
}

void BrowserWindow::OpenBoostEditor() {
  // openBoostWindow for the active tab's site (canBoostSite: http(s) only).
  Tab* tab = active_tab();
  const std::wstring domain = tab ? BoostDomain(tab->url()) : std::wstring();
  if (!tab || domain.empty() || closing_ || profile_->is_private() || !tab->browser()) return;
  if (boost_editor_open() && boost_domain_ != domain) SetBoostPickMode(L"off");
  boost_domain_ = domain;
  boost_tab_browser_ = tab->browser()->GetIdentifier();
  if (!boost_popup_) {
    boost_popup_ = ::CreateWindowExW(WS_EX_TOOLWINDOW, kSiteDataClass, L"Boost", WS_POPUP, 0, 0, 0, 0, hwnd_,
                                     nullptr, ModuleInstance(), this);
    if (!boost_popup_) return;
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    ::DwmSetWindowAttribute(boost_popup_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
  }
  const BOOL dark = palette_.dark ? TRUE : FALSE;
  ::DwmSetWindowAttribute(boost_popup_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  POINT at{static_cast<LONG>(std::lround(px(site_info_.left))), static_cast<LONG>(std::lround(px(address_rect_.bottom + 6.f)))};
  ::ClientToScreen(hwnd_, &at);
  ::SetWindowPos(boost_popup_, HWND_TOP, at.x, at.y, static_cast<int>(std::lround(px(320.f))),
                 static_cast<int>(std::lround(px(640.f))), SWP_SHOWWINDOW);
  if (boost_browser_) {
    boost_browser_->GetMainFrame()->ExecuteJavaScript("window.orbitReload && orbitReload()", "", 0);
  } else if (!boost_creating_) {
    RECT rc;
    ::GetClientRect(boost_popup_, &rc);
    CefWindowInfo info;
    info.SetAsChild(boost_popup_, CefRect(0, 0, rc.right, rc.bottom));
    info.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
    CefBrowserSettings settings;
    boost_client_ = new LibraryClient(this, LibraryClient::kBoost);
    boost_creating_ = CefBrowserHost::CreateBrowser(info, boost_client_, "orbit://boost/", settings, nullptr,
                                                    profile_->request_context());
  }
}

void BrowserWindow::CloseBoostEditor() {
  if (!boost_popup_) return;
  SetBoostPickMode(L"off");
  ::ShowWindow(boost_popup_, SW_HIDE);
  if (boost_browser_) boost_browser_->GetHost()->CloseBrowser(true);
}

std::wstring BrowserWindow::boost_editor_title() const {
  return boost_client_ ? boost_client_->title() : std::wstring();
}

Tab* BrowserWindow::boost_tab() const {
  return boost_tab_browser_ ? Application::Get()->client()->FindTab(boost_tab_browser_) : nullptr;
}

void BrowserWindow::SetBoostPickMode(const std::wstring& mode) {
  // zen-boost-zap / zen-boost-focus: the page's picker follows.
  const std::wstring next = mode == L"zap" || mode == L"focus" ? mode : L"off";
  boost_pick_mode_ = next;
  Tab* tab = boost_tab();
  if (!tab || !tab->browser()) return;
  CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kBoostPickModeMessage);
  message->GetArgumentList()->SetString(0, next);
  tab->browser()->GetMainFrame()->SendProcessMessage(PID_RENDERER, message);
}

void BrowserWindow::OnBoostPicked(Tab* tab, const std::wstring& mode, const std::wstring& selector) {
  // Only the page the editor is open for, in the mode the editor chose.
  if (!tab || tab != boost_tab() || boost_pick_mode_ == L"off") return;
  if (mode == L"off") {
    SetBoostPickMode(L"off");
  } else if (mode == boost_pick_mode_ && IsSafeBoostSelector(selector) && BoostDomain(tab->url()) == boost_domain_) {
    Boost boost = LoadBoostForSite(profile_, boost_domain_).value_or(Boost{});
    auto& list = mode == L"zap" ? boost.zap : boost.focus;
    if (std::find(list.begin(), list.end(), selector) == list.end()) list.push_back(selector);
    if (mode == L"focus") SetBoostPickMode(L"off");  // one focus target at a time
    SaveBoostForSite(profile_, boost_domain_, boost);
  }
  if (boost_browser_) boost_browser_->GetMainFrame()->ExecuteJavaScript("window.orbitReload && orbitReload()", "", 0);
}

void BrowserWindow::OnBoostEditorCreated(CefRefPtr<CefBrowser> browser) {
  boost_creating_ = false;
  boost_browser_ = browser;
  if (HWND child = browser->GetHost()->GetWindowHandle(); child && boost_popup_) {
    RECT rc;
    ::GetClientRect(boost_popup_, &rc);
    ::SetWindowPos(child, nullptr, 0, 0, rc.right, rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
  }
  if (!boost_editor_open() || closing_) browser->GetHost()->CloseBrowser(true);
}

void BrowserWindow::OnBoostEditorClosed(CefRefPtr<CefBrowser> browser) {
  if (boost_browser_ && boost_browser_->IsSame(browser)) boost_browser_ = nullptr;
  boost_creating_ = false;
  if (closing_) Application::Get()->OnWindowBrowserClosed();
}

std::wstring BrowserWindow::theme_picker_title() const {
  return theme_client_ ? theme_client_->title() : std::wstring();
}

Workspace* BrowserWindow::theme_editing_space() const {
  return theme_space_id_ ? FindSpace(theme_space_id_) : nullptr;
}

void BrowserWindow::CloseThemePicker() {
  // handlePanelClose: what was picked is stored.
  if (!theme_popup_) return;
  ::ShowWindow(theme_popup_, SW_HIDE);
  if (Workspace* ws = theme_editing_space()) Application::Get()->store().UpdateWorkspace(ws->record());
  if (theme_browser_) theme_browser_->GetHost()->CloseBrowser(true);  // rebuilt when reopened
}

void BrowserWindow::OnThemePickerCreated(CefRefPtr<CefBrowser> browser) {
  theme_creating_ = false;
  theme_browser_ = browser;
  if (HWND child = browser->GetHost()->GetWindowHandle(); child && theme_popup_) {
    RECT rc;
    ::GetClientRect(theme_popup_, &rc);
    ::SetWindowPos(child, nullptr, 0, 0, rc.right, rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
  }
  if (!theme_picker_open() || closing_) {
    browser->GetHost()->CloseBrowser(true);
  } else {
    browser->GetHost()->SetFocus(true);
  }
}

void BrowserWindow::OnThemePickerClosed(CefRefPtr<CefBrowser> browser) {
  if (theme_browser_ && theme_browser_->IsSame(browser)) theme_browser_ = nullptr;
  theme_creating_ = false;
  if (closing_) {
    Application::Get()->OnWindowBrowserClosed();
  } else if (theme_picker_open() && !theme_browser_) {
    // Reopened while the old browser was still closing (ExecuteJavaScript went
    // to the dying browser): build the new one now.
    OpenThemePicker(theme_editing_space());
  }
}

std::wstring BrowserWindow::site_data_title() const {
  return site_client_ ? site_client_->title() : std::wstring();
}

void BrowserWindow::CloseSiteData() {
  if (!site_popup_) return;
  ::ShowWindow(site_popup_, SW_HIDE);
  if (site_browser_) site_browser_->GetHost()->CloseBrowser(true);  // rebuilt when reopened
}

void BrowserWindow::OnSiteDataCreated(CefRefPtr<CefBrowser> browser) {
  site_creating_ = false;
  site_browser_ = browser;
  if (HWND child = browser->GetHost()->GetWindowHandle(); child && site_popup_) {
    RECT rc;
    ::GetClientRect(site_popup_, &rc);
    ::SetWindowPos(child, nullptr, 0, 0, rc.right, rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
  }
  if (!site_data_open() || closing_) {
    browser->GetHost()->CloseBrowser(true);
  } else {
    browser->GetHost()->SetFocus(true);
  }
}

void BrowserWindow::OnSiteDataClosed(CefRefPtr<CefBrowser> browser) {
  if (site_browser_ && site_browser_->IsSame(browser)) site_browser_ = nullptr;
  site_creating_ = false;
  if (closing_) Application::Get()->OnWindowBrowserClosed();
}

LRESULT CALLBACK BrowserWindow::SiteDataProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wp, lp);
  }
  auto* self = reinterpret_cast<BrowserWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self) return ::DefWindowProcW(hwnd, msg, wp, lp);
  switch (msg) {
    case WM_SIZE:
      if (HWND child = ::GetWindow(hwnd, GW_CHILD)) {
        ::SetWindowPos(child, nullptr, 0, 0, LOWORD(lp), HIWORD(lp), SWP_NOZORDER | SWP_NOACTIVATE);
      }
      return 0;
    case WM_ERASEBKGND: {
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      HBRUSH brush = ::CreateSolidBrush(ui::ToColorRef(self->palette_.content_bg));
      ::FillRect(reinterpret_cast<HDC>(wp), &rc, brush);
      ::DeleteObject(brush);
      return 1;
    }
    case WM_ACTIVATE:
      // A panel: it goes away when the user clicks elsewhere.
      if (LOWORD(wp) == WA_INACTIVE) ::PostMessageW(hwnd, WM_APP + 7, 0, 0);
      return 0;
    case WM_CLOSE:
      // Alt+F4 on a panel closes the panel (its browser first), never by
      // destroying the popup under it.
      if (hwnd == self->theme_popup_) {
        self->CloseThemePicker();
      } else if (hwnd == self->boost_popup_) {
        self->CloseBoostEditor();
      } else {
        self->CloseSiteData();
      }
      return 0;
    case WM_APP + 7:
      if (::GetForegroundWindow() == hwnd) return 0;
      if (hwnd == self->boost_popup_) return 0;  // stays while the page is clicked (zap)
      if (hwnd == self->theme_popup_) {
        self->CloseThemePicker();
      } else {
        self->CloseSiteData();
      }
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Glance (ZenGlanceManager)

D2D1_RECT_F BrowserWindow::GlanceRectNow(uint64_t now) const {
  const D2D1_RECT_F to = GlanceRect();
  if (!glance_ || (!glance_->anim_start && !glance_->close_start)) return to;
  const bool opening = glance_->anim_start != 0;
  const uint64_t start = opening ? glance_->anim_start : glance_->close_start;
  const float t = static_cast<float>(now - start) / static_cast<float>(kGlanceAnimMs);
  const D2D1_RECT_F& f = glance_->from;
  const ArcBox link{(f.left + f.right) / 2.f, (f.top + f.bottom) / 2.f, f.right - f.left, f.bottom - f.top};
  const ArcBox page{(to.left + to.right) / 2.f, (to.top + to.bottom) / 2.f, to.right - to.left,
                    to.bottom - to.top};
  // easing: ease-in-out (opening) / ease-out (closing) over the eased
  // keyframes; the host keeps a few pixels.
  const float u = std::min(t, 1.f);
  const ArcBox box = opening ? GlanceArcAt(link, page, Height(), EaseInOut(u), true)
                             : GlanceArcAt(page, link, Height(), 1.f - (1.f - u) * (1.f - u), false);
  const float w = std::max(box.w, 8.f), h = std::max(box.h, 8.f);
  return {box.cx - w / 2.f, box.cy - h / 2.f, box.cx + w / 2.f, box.cy + h / 2.f};
}

D2D1_RECT_F BrowserWindow::GlanceRect() const {
  // .browserContainer in the glance overlay: 80% wide, full height inside
  // the element separation, centred; the buttons sit to its right.
  const RECT c = ContentRectPx();
  const D2D1_RECT_F content{dip(static_cast<float>(c.left)), dip(static_cast<float>(c.top)),
                            dip(static_cast<float>(c.right)), dip(static_cast<float>(c.bottom))};
  const float w = (content.right - content.left) * 0.8f;
  const float left = content.left + ((content.right - content.left) - w) / 2.f;
  return {left, content.top + ui::metrics::kSeparation, left + w,
          content.bottom - ui::metrics::kSeparation};
}

void BrowserWindow::StartWindowDrag(double moved_px) {
  if (chrome_hidden() || closing_) return;
  if (::IsZoomed(hwnd_) && moved_px < 50.0) return;  // SNAPPED_DRAG_START_THRESHOLD_PX
  POINT pt;
  ::GetCursorPos(&pt);
  // As a caption press: the system move loop takes it from here.
  ::ReleaseCapture();
  ::SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(pt.x, pt.y));
}

void BrowserWindow::OpenGlance(Tab* parent, const std::wstring& url, std::optional<GlanceOrigin> origin) {
  if (!workspace_ || !parent) return;
  if (IsGlanceTab(parent)) {
    parent->Navigate(url);  // a glance inside the glance replaces its page
    return;
  }
  if (workspace_->IndexOf(parent) < 0) {
    OpenTabNear(parent, url, true);  // Essentials: an ordinary tab
    return;
  }
  CloseGlance();
  if (active_tab() != parent) ActivateTab(parent);
  Tab* tab = OpenTabNear(parent, url, false);
  if (!tab) return;
  glance_ = GlanceState{parent, tab};
  if (origin && ui::AnimMs(kGlanceAnimMs) > 0) {
    // #setupGlancePositioning: the link's rect in this window.
    const RECT c = ContentRectPx();
    const float z = static_cast<float>(std::clamp(origin->zoom, 0.25, 5.0));
    const float left = dip(static_cast<float>(c.left)) + static_cast<float>(origin->x) * z;
    const float top = dip(static_cast<float>(c.top)) + static_cast<float>(origin->y) * z;
    glance_->from = {left, top, left + static_cast<float>(origin->width) * z,
                     top + static_cast<float>(origin->height) * z};
    glance_->has_origin = true;
    glance_->anim_start = ::GetTickCount64();
  }
  ActivateTab(tab);
  tab->set_owner(parent);  // closing it returns to the parent
  Layout();
  UpdateAnimationTimer(::GetTickCount64());
}

void BrowserWindow::CloseGlanceAnimated() {
  if (!glance_ || glance_->close_start) return;
  if (!glance_->has_origin || active_tab() != glance_->tab || ui::AnimMs(kGlanceAnimMs) == 0) {
    CloseGlance();
    return;
  }
  glance_->anim_start = 0;
  glance_->close_start = ::GetTickCount64();
  if (glance_bar_) ::ShowWindow(glance_bar_, SW_HIDE);  // #animateSidebarButtons
  UpdateAnimationTimer(glance_->close_start);
}

void BrowserWindow::CloseGlance() {
  if (!glance_) return;
  // Selecting the parent drops (and closes) the glance; see ActivateTab.
  if (active_tab() == glance_->tab) {
    ActivateTab(glance_->parent);
  } else {
    Tab* tab = glance_->tab;
    glance_.reset();
    PlaceGlanceChrome(false);
    CloseTab(tab);
  }
}

void BrowserWindow::ExpandGlance() {
  // fullyOpenGlance: the glance becomes an ordinary tab after its parent.
  if (!glance_) return;
  Tab* tab = glance_->tab;
  glance_.reset();
  PlaceGlanceChrome(false);
  tab->set_owner(nullptr);
  ShowContent();
  Layout();
  EnsureTabVisible(workspace_->IndexOf(tab));
}

void BrowserWindow::SplitGlance() {
  // splitGlance: the parent and the glance side by side.
  if (!glance_) return;
  const GlanceState g = *glance_;
  if (g.parent->pinned()) {
    ExpandGlance();
    return;
  }
  glance_.reset();
  PlaceGlanceChrome(false);
  g.tab->set_owner(nullptr);
  if (const auto* split = workspace_->SplitOf(g.parent)) {
    if (!workspace_->AddToSplit(split->id, g.tab)) {
      ShowToast({L"split-limit", L"Can't add more panels to the split view!"});
    }
  } else {
    workspace_->CreateSplit({g.parent, g.tab}, SplitGrid::kGrid);
  }
  ShowContent();
  Layout();
}

void BrowserWindow::EnsureGlanceWindows() {
  if (glance_dim_) return;
  glance_dim_ = ::CreateWindowExW(WS_EX_LAYERED, kGlanceDimClass, L"", WS_CHILD | WS_CLIPSIBLINGS,
                                  0, 0, 0, 0, hwnd_, nullptr, ModuleInstance(), this);
  // The parent shows through at 30% (opacity: [1, 0.3]).
  if (glance_dim_) ::SetLayeredWindowAttributes(glance_dim_, 0, 179, LWA_ALPHA);
  glance_bar_ = ::CreateWindowExW(0, kGlanceBarClass, L"Glance", WS_CHILD | WS_CLIPSIBLINGS, 0, 0,
                                  0, 0, hwnd_, nullptr, ModuleInstance(), this);
}

void BrowserWindow::PlaceGlanceChrome(bool show) {
  if (!show) {
    if (glance_dim_) ::ShowWindow(glance_dim_, SW_HIDE);
    if (glance_bar_) ::ShowWindow(glance_bar_, SW_HIDE);
    glance_bar_hover_ = -1;
    return;
  }
  EnsureGlanceWindows();
  if (!glance_dim_ || !glance_bar_) return;
  const RECT content = ContentRectPx();
  ::SetWindowPos(glance_dim_, HWND_TOP, content.left, content.top, content.right - content.left,
                 content.bottom - content.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  ::InvalidateRect(glance_dim_, nullptr, TRUE);
  const D2D1_RECT_F g = GlanceRect();
  const int w = static_cast<int>(std::lround(px(kGlancePad * 2.f + kGlanceButton)));
  const int h = static_cast<int>(std::lround(px(kGlancePad * 4.f + kGlanceButton * 3.f)));
  const float bar_w = kGlancePad * 2.f + kGlanceButton;
  // left: 100% (right: 100% with tabs on the right).
  const float bar_x = right_side_ ? g.left - bar_w : g.right;
  const RECT at = ToPx({bar_x, g.top + 15.f - kGlancePad, bar_x, g.top});
  // Three round buttons: the window is clipped to them.
  HRGN region = ::CreateRectRgn(0, 0, 0, 0);
  for (int i = 0; i < 3; ++i) {
    const int x = static_cast<int>(std::lround(px(kGlancePad)));
    const int y = static_cast<int>(std::lround(px(kGlancePad + i * (kGlanceButton + kGlancePad))));
    const int d = static_cast<int>(std::lround(px(kGlanceButton)));
    HRGN circle = ::CreateEllipticRgn(x, y, x + d + 1, y + d + 1);
    ::CombineRgn(region, region, circle, RGN_OR);
    ::DeleteObject(circle);
  }
  ::SetWindowRgn(glance_bar_, region, FALSE);  // takes ownership
  ::SetWindowPos(glance_bar_, HWND_TOP, at.left, at.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  ::InvalidateRect(glance_bar_, nullptr, FALSE);
}

int BrowserWindow::GlanceButtonAt(int x, int y) const {
  const float fx = dip(static_cast<float>(x)) - kGlancePad - kGlanceButton / 2.f;
  for (int i = 0; i < 3; ++i) {
    const float fy = dip(static_cast<float>(y)) - (kGlancePad + i * (kGlanceButton + kGlancePad)) -
                     kGlanceButton / 2.f;
    if (fx * fx + fy * fy <= kGlanceButton * kGlanceButton / 4.f) return i;
  }
  return -1;
}

void BrowserWindow::PaintGlanceBar(HDC dc) {
  using namespace ui;
  RECT rc;
  ::GetClientRect(glance_bar_, &rc);
  Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target;
  const auto props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
      static_cast<float>(dpi_), static_cast<float>(dpi_));
  if (FAILED(Graphics::Get().d2d()->CreateDCRenderTarget(&props, &target)) ||
      FAILED(target->BindDC(dc, &rc))) {
    return;
  }
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  target->CreateSolidColorBrush(palette_.text, &brush);
  target->BeginDraw();
  // Outside the circles is clipped by the region; fill with the page colour.
  target->Clear(palette_.content_bg);
  // light-dark(rgb(24,24,24), rgb(231,231,231)) 96% with the space colour;
  // the icon takes the space colour (--zen-primary-color).
  const D2D1_COLOR_F base = palette_.dark ? ColorFromRgb(0xE7E7E7) : ColorFromRgb(0x181818);
  const D2D1_COLOR_F hover = palette_.dark ? ColorFromRgb(0xCCCCCC) : ColorFromRgb(0x292929);
  const wchar_t glyphs[] = {glyph::kClose, glyph::kOpenInNew, glyph::kOpenPane};
  for (int i = 0; i < 3; ++i) {
    const float top = kGlancePad + static_cast<float>(i) * (kGlanceButton + kGlancePad);
    const D2D1_RECT_F r{kGlancePad, top, kGlancePad + kGlanceButton, top + kGlanceButton};
    brush->SetColor(Mix(i == glance_bar_hover_ ? hover : base, palette_.accent, 0.96f));
    target->FillEllipse(D2D1::Ellipse({(r.left + r.right) / 2.f, (r.top + r.bottom) / 2.f},
                                      kGlanceButton / 2.f, kGlanceButton / 2.f),
                        brush.Get());
    brush->SetColor(palette_.accent);
    DrawGlyph(target.Get(), Graphics::Get().icons_small(), glyphs[i], Inset(r, 9.f, 9.f),
              brush.Get());
  }
  target->EndDraw();
}

LRESULT CALLBACK BrowserWindow::GlanceDimProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wp, lp);
  }
  auto* self = reinterpret_cast<BrowserWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self) return ::DefWindowProcW(hwnd, msg, wp, lp);
  switch (msg) {
    case WM_ERASEBKGND: {
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      // The backdrop the parent fades into.
      HBRUSH brush = ::CreateSolidBrush(ui::ToColorRef(self->BackdropAt(
          self->dip(static_cast<float>(rc.right)) / 2.f, self->dip(static_cast<float>(rc.bottom)) / 2.f)));
      ::FillRect(reinterpret_cast<HDC>(wp), &rc, brush);
      ::DeleteObject(brush);
      return 1;
    }
    case WM_LBUTTONDOWN:
      self->CloseGlanceAnimated();  // onOverlayClick: a click beside the glance
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK BrowserWindow::GlanceBarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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
      HDC dc = ::BeginPaint(hwnd, &ps);
      self->PaintGlanceBar(dc);
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_MOUSEMOVE: {
      const int hot = self->GlanceButtonAt(LParamX(lp), LParamY(lp));
      if (hot != self->glance_bar_hover_) {
        self->glance_bar_hover_ = hot;
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&tme);
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      self->glance_bar_hover_ = -1;
      ::InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_LBUTTONUP:
      switch (self->GlanceButtonAt(LParamX(lp), LParamY(lp))) {
        case 0: self->CloseGlanceAnimated(); break;   // cmd_zenGlanceClose
        case 1: self->ExpandGlance(); break;  // cmd_zenGlanceExpand
        case 2: self->SplitGlance(); break;   // cmd_zenGlanceSplit
        default: break;
      }
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Split view (ZenViewSplitter)

void BrowserWindow::SplitWithNext(SplitGrid grid) {
  Tab* tab = active_tab();
  if (!workspace_ || !tab || active_essential_ || tab->pinned()) return;
  // In a split: the same tabs in the new layout (splitTabs on the group).
  if (const auto* split = workspace_->SplitOf(tab)) {
    workspace_->RelayoutSplit(split->id, grid);
    ShowContent();
    Invalidate();
    return;
  }
  // toggleShortcut: the selected tab and the next visible one (wrapping).
  const auto& tabs = workspace_->tabs();
  const int n = static_cast<int>(tabs.size());
  const int at = workspace_->IndexOf(tab);
  Tab* next = nullptr;
  for (int step = 1; step < n && !next; ++step) {
    Tab* t = tabs[static_cast<size_t>((at + step) % n)].get();
    if (!t->pinned() && !t->empty_placeholder() && !workspace_->SplitOf(t)) next = t;
  }
  if (!next) return;
  if (workspace_->CreateSplit({tab, next}, grid)) Layout();
}

void BrowserWindow::UnsplitCurrent() {
  if (const auto* split = ActiveSplit()) {
    workspace_->RemoveSplit(split->id);
    Layout();
  }
}

void BrowserWindow::NewEmptySplit() {
  // createEmptySplit: a new tab next to the current one, in a split with it,
  // and the URL bar for it.
  Tab* tab = active_tab();
  if (!workspace_ || !tab || active_essential_ || tab->pinned()) return;
  if (const auto* split = workspace_->SplitOf(tab);
      split && workspace_->SplitMembers(*split).size() >= Workspace::kMaxSplitTabs) {
    ShowToast({L"split-limit", L"Can't add more panels to the split view!"});  // zen-split-view-limit-toast
    return;
  }
  Tab* fresh = OpenTabNear(tab, Application::kNewTabUrl, false);
  if (!fresh) return;
  if (const auto* split = workspace_->SplitOf(tab)) {
    workspace_->AddToSplit(split->id, fresh);
  } else {
    workspace_->CreateSplit({tab, fresh}, SplitGrid::kGrid);
  }
  ActivateTab(fresh);  // a new-tab page focuses the address bar
  Layout();
}

void BrowserWindow::SplitTabWithCurrent(Tab* tab) {
  Tab* current = active_tab();
  if (!workspace_ || !tab || tab->pinned()) return;
  if (tab == current || !current || current->pinned() || active_essential_) {
    // contextSplitTabs with one tab: an empty split beside it.
    if (tab != current) ActivateTab(tab);
    NewEmptySplit();
    return;
  }
  if (const auto* split = workspace_->SplitOf(current)) {
    if (!workspace_->AddToSplit(split->id, tab)) {
      ShowToast({L"split-limit", L"Can't add more panels to the split view!"});
      return;
    }
  } else if (!workspace_->CreateSplit({current, tab}, SplitGrid::kGrid)) {
    return;
  }
  Layout();
}

void BrowserWindow::SplitLinkInNewTab(Tab* from, const std::wstring& url) {
  if (!workspace_ || workspace_->IndexOf(from) < 0 || from->pinned()) {
    OpenTabNear(from, url, true);  // pinned tabs and Essentials cannot split
    return;
  }
  const auto* split = workspace_->SplitOf(from);
  if (split && workspace_->SplitMembers(*split).size() >= Workspace::kMaxSplitTabs) {
    ShowToast({L"split-limit", L"Can't add more panels to the split view!"});
    return;
  }
  Tab* fresh = OpenTabNear(from, url, false);
  if (!fresh) return;
  if (split) {
    workspace_->AddToSplit(split->id, fresh);
  } else {
    workspace_->CreateSplit({from, fresh}, SplitGrid::kGrid);
  }
  ActivateTab(fresh);
  Layout();
}

void BrowserWindow::SplitOut(Tab* tab) {
  if (!workspace_ || !workspace_->SplitOf(tab)) return;
  workspace_->RemoveFromSplit(tab);
  ActivateTab(tab);  // removeTabFromSplit selects the tab that left
  Layout();
}

void BrowserWindow::NewWorkspace() {
  if (profile_->is_private()) return;
  std::wstring name = L"Space " + std::to_wstring(Spaces().size() + 1);
  if (!PromptText(L"New workspace", L"Workspace name:", name) || name.empty()) return;
  const size_t n = Spaces().size();
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
  if (Spaces().size() <= 1) {
    ::MessageBoxW(hwnd_, L"Orbit needs at least one space.", L"Orbit", MB_OK | MB_ICONINFORMATION);
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
    for (Workspace* other : Spaces()) {
      if (other != ws && !other->window()) {
        ShowWorkspace(other, 0);
        break;
      }
    }
    if (ws == workspace_) return;  // every other workspace is open elsewhere
  }
  // Free live browsers; the rows go away with the workspace (ON DELETE
  // CASCADE), its Essentials too.
  for (const auto& t : ws->tabs()) t->Unload(false);
  if (Workspace* grid = ws->essentials()) {
    for (const auto& t : grid->tabs()) t->Unload(false);
  }
  if (!ws->profile()->is_private()) Application::Get()->store().DeleteWorkspace(ws->id());
  pins_collapsed_.erase(ws);
  last_essential_.erase(ws);
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

void BrowserWindow::OpenLocation() {
  // zen.urlbar.behavior: "float" and "floating-on-type" float the bar when it
  // is opened from the keyboard; "normal" opens it in place in the sidebar.
  if (urlbar_behavior_ == L"normal" && address_rect_.right > address_rect_.left) {
    ShowFloatingUrlBar(/*new_tab=*/false, /*select_all=*/true, PopupAnchor::kSidebar);
  } else {
    ShowFloatingUrlBar(/*new_tab=*/false);
  }
}

void BrowserWindow::TypeInAddressBar(const std::wstring& text) {
  ::SetWindowTextW(address_, text.c_str());  // EN_CHANGE marks it as edited
  ::SendMessageW(address_, EM_SETSEL, text.size(), text.size());
}

void BrowserWindow::SubmitAddressBar() { NavigateFromAddressBar(); }

void BrowserWindow::CancelAddressBar() {
  address_editing_ = false;
  if (popup_visible_) HideFloatingUrlBar(false);
  FocusContent();
  UpdateAddressBar(true);
}

void BrowserWindow::FocusAddressBar() {
  GuardAddressFocus();
  if (address_rect_.right <= address_rect_.left || chrome_hidden()) {
    // The sidebar's address bar is not on screen: use the floating one.
    ShowFloatingUrlBar(/*new_tab=*/false);
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
  if (::GetFocus() != address_) text = SidebarDisplayUrl(text);
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
  NavigateTo(result.url);
}

void BrowserWindow::NavigateTo(const std::wstring& url) {
  if (new_tab_mode()) {
    new_tab_navigated_ = true;
    HideFloatingUrlBar(false);
    address_editing_ = false;
    OpenRoutedTab(url, /*activate=*/true, /*external=*/false);
    FocusContent();
    return;
  }
  if (popup_visible_) HideFloatingUrlBar(false);
  Tab* tab = active_tab();
  if (!tab) tab = OpenTab(url, true);
  address_editing_ = false;
  tab->Navigate(url);
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
        if (self->popup_visible_ && !self->results_.empty()) {
          self->ActivateUrlResult(self->selected_result_);
        } else {
          self->NavigateFromAddressBar();
        }
        return 0;
      }
      if (self->popup_visible_ && !self->results_.empty() &&
          (wp == VK_DOWN || wp == VK_UP || (wp == VK_TAB && !ctrl))) {
        self->MoveUrlResultSelection(wp == VK_UP || (wp == VK_TAB && shift) ? -1 : 1);
        return 0;
      }
      if (wp == VK_ESCAPE) {
        // A row filled the input: Escape first restores what was typed.
        if (self->popup_visible_ && self->selected_result_ != 0 && !self->results_.empty()) {
          self->selected_result_ = 0;
          self->results_first_ = 0;
          self->FillInputFromSelection();
          ::InvalidateRect(self->popup_, nullptr, FALSE);
          return 0;
        }
        self->CancelAddressBar();
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
      // zen.urlbar.behavior = float: a click floats the bar too.
      if (self->urlbar_behavior_ == L"float" && !self->popup_visible_) {
        self->ShowFloatingUrlBar(/*new_tab=*/false);
        return 0;
      }
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

void BrowserWindow::ShowFloatingUrlBar(bool new_tab, bool select_all, PopupAnchor anchor) {
  if (!popup_ || closing_) return;
  GuardAddressFocus();
  if (popup_visible_ && (new_tab_mode_ != new_tab || popup_anchor_ != anchor)) {
    HideFloatingUrlBar(true);
  }
  new_tab_mode_ = new_tab;
  new_tab_navigated_ = false;
  popup_anchor_ = anchor;
  DWORD sel_start = 0, sel_end = 0;
  ::SendMessageW(address_, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_start),
                 reinterpret_cast<LPARAM>(&sel_end));
  if (!popup_visible_) {
    popup_visible_ = true;
    ::SetParent(address_, popup_);
    // Keep the EDIT right after its label in z-order (accessible name).
    ::SetWindowPos(address_, popup_label_, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    // The floating bar uses a larger font (font-size: 1.15em); a breakout
    // keeps the sidebar field's.
    ::SendMessageW(address_, WM_SETFONT,
                   reinterpret_cast<WPARAM>(anchor == PopupAnchor::kCentered ? popup_font_
                                                                             : address_font_),
                   FALSE);
    PositionFloatingUrlBar();
    ::ShowWindow(popup_, SW_SHOW);
    ::ShowWindow(address_, SW_SHOW);
    Invalidate();  // the sidebar's field is empty while the text floats
  }
  ::SetForegroundWindow(popup_);
  ::SetFocus(address_);
  if (new_tab_mode_) {
    // An empty bar, or the search that was typed less than 45 s ago.
    std::wstring text;
    if (::GetTickCount64() - last_new_tab_search_at_ < kNewTabSearchKeepMs) {
      text = last_new_tab_search_;
    }
    address_programmatic_ = true;
    ::SetWindowTextW(address_, text.c_str());
    address_programmatic_ = false;
    address_editing_ = !text.empty();
  }
  if (select_all) {
    ::SendMessageW(address_, EM_SETSEL, 0, -1);
  } else {
    ::SendMessageW(address_, EM_SETSEL, sel_start, sel_end);
  }
  UpdateUrlResults();
  Invalidate();  // "+ New Tab" shows as selected while the bar is for a new tab
}

float BrowserWindow::UrlBarInputHeight() const {
  return popup_anchor_ == PopupAnchor::kCentered ? kFloatingBarHeight : kBreakoutBarHeight;
}

void BrowserWindow::UpdateUrlResults() {
  std::wstring text;
  if (popup_visible_ && address_editing_) {
    const int len = ::GetWindowTextLengthW(address_);
    text.assign(static_cast<size_t>(len), L'\0');
    ::GetWindowTextW(address_, text.data(), len + 1);
  }
  OmniboxSources sources;
  if (!text.empty()) {
    // Open tabs (of every normal profile), most recently used first.
    std::vector<Tab*> tabs;
    for (Workspace* ws : TabContainers()) {
      for (const auto& t : ws->tabs()) tabs.push_back(t.get());
    }
    std::sort(tabs.begin(), tabs.end(),
              [](Tab* a, Tab* b) { return a->last_active_ms() > b->last_active_ms(); });
    for (Tab* t : tabs) {
      if (!IsNewTabUrl(t->url())) {
        sources.open_tabs.push_back({t->url(), t->DisplayTitle(), reinterpret_cast<intptr_t>(t)});
      }
    }
    // In new-tab mode no tab is current; otherwise the active one is.
    if (!new_tab_mode_ && active_tab()) {
      sources.current_tab_key = reinterpret_cast<intptr_t>(active_tab());
    }
    if (!profile_->is_private()) {
      Store& store = Application::Get()->store();
      for (const auto& b : store.LoadBookmarks(profile_->id())) {
        if (!b.is_folder) sources.bookmarks.push_back({b.url, b.title, 0});
      }
      // The store matches one substring; query with the longest word and let
      // BuildOmniboxRows require the others.
      std::wstring longest;
      std::wstring word;
      for (wchar_t ch : text + L" ") {
        if (ch == L' ') {
          if (word.size() > longest.size()) longest = word;
          word.clear();
        } else {
          word += ch;
        }
      }
      FILETIME ft;
      ::GetSystemTimeAsFileTime(&ft);
      const int64_t now = static_cast<int64_t>(
          ((static_cast<uint64_t>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) / 10000) - 11644473600000ULL);
      for (const auto& v : store.QueryHistoryFrecent(profile_->id(), longest, now, 50)) {
        sources.history.push_back({v.url, v.title, 0});
      }
    }
    // browser.search.suggest.enabled (off by default in Zen; never private).
    if (!profile_->is_private() && PrefEnabled(L"search_suggestions", false)) {
      if (suggest_text_ == text) {
        sources.search_suggestions = suggest_results_;
      } else {
        RequestSearchSuggestions(text);
      }
    }
  }
  if (!filling_input_) typed_text_ = text;
  if (PrefEnabled(L"quick_actions", true)) sources.actions = QuickActions();  // zen.urlbar.suggestions.quick-actions
  results_ = BuildOmniboxRows(text, Application::Get()->search_template(), sources);
  selected_result_ = 0;
  results_first_ = 0;
  hover_result_ = -1;
  if (popup_visible_) PositionFloatingUrlBar();
}

void BrowserWindow::MoveUrlResultSelection(int delta) {
  if (results_.empty()) return;
  const int n = static_cast<int>(results_.size());
  selected_result_ = ((selected_result_ + delta) % n + n) % n;
  // Keep it in view (#urlbar-results scrolls past five rows).
  const int visible = static_cast<int>(kVisibleOmniboxRows);
  if (selected_result_ < results_first_) results_first_ = selected_result_;
  if (selected_result_ >= results_first_ + visible) results_first_ = selected_result_ - visible + 1;
  FillInputFromSelection();
  ::InvalidateRect(popup_, nullptr, FALSE);
}

void BrowserWindow::FillInputFromSelection() {
  // Firefox fills the input with the selected result; the first row (and
  // going back to it) shows what was typed.
  if (selected_result_ < 0 || selected_result_ >= static_cast<int>(results_.size())) return;
  const std::wstring text = OmniboxRowInputText(results_[static_cast<size_t>(selected_result_)],
                                                static_cast<size_t>(selected_result_), typed_text_);
  address_programmatic_ = true;
  filling_input_ = true;
  ::SetWindowTextW(address_, text.c_str());
  ::SendMessageW(address_, EM_SETSEL, text.size(), text.size());
  filling_input_ = false;
  address_programmatic_ = false;
}

std::wstring BrowserWindow::address_text() const {
  const int len = ::GetWindowTextLengthW(address_);
  std::wstring text(static_cast<size_t>(len), L'\0');
  ::GetWindowTextW(address_, text.data(), len + 1);
  return text;
}

void BrowserWindow::ScrollUrlResults(int rows) {
  const int max_first = std::max(0, static_cast<int>(results_.size()) - static_cast<int>(kVisibleOmniboxRows));
  results_first_ = std::clamp(results_first_ + rows, 0, max_first);
  ::InvalidateRect(popup_, nullptr, FALSE);
}

void BrowserWindow::RequestSearchSuggestions(const std::wstring& text) {
  // One request in flight per text; the answer rebuilds the rows if the
  // input still says the same.
  const std::wstring url = SearchSuggestUrl(Application::Get()->search_template(), text);
  if (url.empty() || text == suggest_pending_) return;
  const OmniboxResult fixed = FixupOmniboxInput(text, Application::Get()->search_template());
  if (fixed.kind != OmniboxResult::Kind::kSearch) return;
  suggest_pending_ = text;
  BrowserWindow* self = this;
  FetchText(profile_, url, L"application/json", /*cookies=*/false, [self, text](int status, const std::string& body) {
    Application* app = Application::Get();
    const bool alive = std::any_of(app->windows().begin(), app->windows().end(),
                                   [self](const auto& w) { return w.get() == self && !w->closing(); });
    if (!alive) return;
    if (self->suggest_pending_ == text) self->suggest_pending_.clear();
    std::vector<std::wstring> list;
    if (status == 200) {
      // OpenSearch suggestions: ["query", ["a", "b", ...], ...]
      CefRefPtr<CefValue> v = CefParseJSON(body, JSON_PARSER_RFC);
      CefRefPtr<CefListValue> top = v && v->GetType() == VTYPE_LIST ? v->GetList() : nullptr;
      CefRefPtr<CefListValue> items = top && top->GetSize() > 1 && top->GetType(1) == VTYPE_LIST ? top->GetList(1) : nullptr;
      for (size_t i = 0; items && i < items->GetSize() && list.size() < 10; ++i) {
        if (items->GetType(i) == VTYPE_STRING) list.push_back(items->GetString(i).ToWString().substr(0, 200));
      }
    }
    self->SetSearchSuggestionsForTesting(text, std::move(list));
  });
}

void BrowserWindow::SetSearchSuggestionsForTesting(const std::wstring& text, std::vector<std::wstring> list) {
  suggest_text_ = text;
  suggest_results_ = std::move(list);
  if (popup_visible_ && address_editing_ && typed_text_ == text && selected_result_ == 0) UpdateUrlResults();
}

int BrowserWindow::UrlResultAt(float y) const {
  const float top = UrlBarInputHeight() + 4.f;
  if (y < top) return -1;
  const int index = results_first_ + static_cast<int>((y - top) / kResultRowHeight);
  return index < static_cast<int>(results_.size()) ? index : -1;
}

void BrowserWindow::ActivateUrlResult(int index) {
  if (index < 0 || index >= static_cast<int>(results_.size())) return;
  const OmniboxRow row = results_[static_cast<size_t>(index)];
  if (row.kind == OmniboxRow::Kind::kAction) {
    if (new_tab_mode_) new_tab_navigated_ = true;
    HideFloatingUrlBar(true);
    RunQuickAction(row.action_id);
    return;
  }
  if (row.kind == OmniboxRow::Kind::kSwitchTab) {
    // Switch to Tab: never opens anything; the bar closes over the tab.
    for (Workspace* ws : TabContainers()) {
      for (const auto& t : ws->tabs()) {
        if (reinterpret_cast<intptr_t>(t.get()) != row.tab_key) continue;
        if (new_tab_mode_) new_tab_navigated_ = true;
        HideFloatingUrlBar(true);
        RevealTab(t.get());
        return;
      }
    }
    return;  // the tab closed meanwhile
  }
  NavigateTo(row.url);
}

void BrowserWindow::HideFloatingUrlBar(bool restore_text) {
  if (!popup_visible_) return;
  if (new_tab_mode_) {
    if (new_tab_navigated_) {
      last_new_tab_search_.clear();
    } else {
      const int len = ::GetWindowTextLengthW(address_);
      last_new_tab_search_.assign(static_cast<size_t>(len), L'\0');
      ::GetWindowTextW(address_, last_new_tab_search_.data(), len + 1);
      last_new_tab_search_at_ = ::GetTickCount64();
    }
    new_tab_mode_ = false;
    restore_text = true;  // back to the current tab's URL
  }
  popup_visible_ = false;
  results_.clear();
  const bool had_focus = ::GetFocus() == address_;
  ::ShowWindow(popup_, SW_HIDE);
  ::SetParent(address_, AddressHost());
  ::SetWindowPos(address_, address_label_, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  ::SendMessageW(address_, WM_SETFONT, reinterpret_cast<WPARAM>(address_font_), FALSE);
  if (had_focus) FocusContent();
  if (restore_text) address_editing_ = false;
  Layout();
  UpdateAddressBar(true);
}

void BrowserWindow::PositionFloatingUrlBar() {
  const float W = Width();
  const float H = Height();
  const float input_h = UrlBarInputHeight();
  const float results_h =
      results_.empty() ? 0.f
                       : 4.f + kResultRowHeight * static_cast<float>(std::min(results_.size(), kVisibleOmniboxRows)) +
                             kResultsPadding - 2.f;
  float w = std::min(W / 1.5f, kFloatingBarMaxWidth);
  float left = (W - w) / 2.f;
  float top = std::max(0.f, H / 2.f - kFloatingBarMinLayoutHeight / 2.f);
  if (popup_anchor_ == PopupAnchor::kSidebar) {
    // Breakout: grows from the sidebar field, min(90%, 40rem) wide.
    w = std::min(W * 0.9f, kBreakoutMaxWidth);
    left = std::max(0.f, address_rect_.left - 4.f);
    top = std::max(0.f, address_rect_.top - 4.f);
  }
  const float h = input_h + results_h;
  RECT r = ToPx({left, top, left + w, top + h});
  POINT origin{r.left, r.top};
  ::ClientToScreen(hwnd_, &origin);
  const int pw = r.right - r.left;
  const int ph = r.bottom - r.top;
  ::SetWindowPos(popup_, nullptr, origin.x, origin.y, pw, ph, SWP_NOZORDER | SWP_NOACTIVATE);
  if (popup_rt_) popup_rt_->Resize(D2D1::SizeU(static_cast<UINT32>(pw), static_cast<UINT32>(ph)));
  const bool centered = popup_anchor_ == PopupAnchor::kCentered;
  const int edit_h = static_cast<int>(std::lround(px(centered ? 24.f : 19.f)));
  const int edit_l = static_cast<int>(std::lround(px(centered ? 46.f : 38.f)));
  const int edit_r = static_cast<int>(std::lround(px(18.f)));
  const int input_px = static_cast<int>(std::lround(px(input_h)));
  ::SetWindowPos(address_, nullptr, edit_l, (input_px - edit_h) / 2,
                 std::max(1, pw - edit_l - edit_r), edit_h, SWP_NOZORDER | SWP_NOACTIVATE);
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
    case WM_LBUTTONDOWN: {
      const int row = self->UrlResultAt(self->dip(static_cast<float>(LParamY(lp))));
      if (row >= 0) {
        self->ActivateUrlResult(row);
      } else {
        ::SetFocus(self->address_);
      }
      return 0;
    }
    case WM_MOUSEMOVE: {
      const int row = self->UrlResultAt(self->dip(static_cast<float>(LParamY(lp))));
      if (row != self->hover_result_) {
        self->hover_result_ = row;
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&tme);
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      self->hover_result_ = -1;
      ::InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_MOUSEWHEEL:
      self->ScrollUrlResults(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        POINT pt;
        ::GetCursorPos(&pt);
        ::ScreenToClient(hwnd, &pt);
        const bool on_row = self->UrlResultAt(self->dip(static_cast<float>(pt.y))) >= 0;
        ::SetCursor(::LoadCursorW(nullptr, on_row ? IDC_HAND : IDC_IBEAM));
        return TRUE;
      }
      break;
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
  // Customized shortcuts (Zen's Keyboard Shortcuts pane): the pressed chord
  // may now run another action, or be one that was moved away or disabled.
  const auto r = Application::Get()->shortcuts().Resolve({vk, ctrl, shift, alt});
  if (r.blocked) return false;
  if (!r.pass) {
    if (r.dispatch.empty()) return RunShortcutAction(r.action);
    return DispatchAccelerator(r.dispatch.vk, r.dispatch.ctrl, r.dispatch.shift, r.dispatch.alt);
  }
  return DispatchAccelerator(vk, ctrl, shift, alt);
}

std::vector<OmniboxAction> BrowserWindow::QuickActions() const {
  // ZenUBGlobalActions, with Orbit's commands; each listed only when it can run.
  Tab* tab = active_tab();
  const bool web = tab && !IsNewTabUrl(tab->url());
  const bool in_space = tab && !active_essential_;
  std::vector<OmniboxAction> out = {
      {L"compact-mode", L"Toggle Compact Mode", L""},
      {L"library", L"Open Library", L""},
      {L"new-split", L"New Split View", L""},
      {L"new-folder", L"New Folder", L""},
      {L"collapse-folders", L"Collapse All Folders", L""},
      {L"expand-folders", L"Expand All Folders", L""},
      {L"settings", L"Settings", L""},
      {L"private-window", L"Open Private Window", L""},
      {L"new-window", L"Open New Window", L""},
      {L"new-blank-window", L"New Blank Window", L""},
      {L"next-space", L"Next Space", L""},
      {L"previous-space", L"Previous Space", L""},
      {L"reopen-tab", L"Reopen Closed Tab", L""},
      {L"next-tab", L"Next Tab", L""},
      {L"previous-tab", L"Previous Tab", L""},
      {L"theme-system", L"Switch to Automatic Appearance", L""},
      {L"theme-light", L"Switch to Light Mode", L""},
      {L"theme-dark", L"Switch to Dark Mode", L""},
      {L"tabs-right", L"Toggle Tabs on right", L""},
  };
  if (!profile_->is_private()) {
    out.push_back({L"create-space", L"New Space", L""});
    out.push_back({L"space-routing", L"Open Space Routing", L""});
  }
  if (ActiveSplit()) out.push_back({L"unsplit", L"Expand current view", L""});
  if (web) {
    out.push_back({L"copy-url", L"Copy Current URL", L""});
    out.push_back({L"close-tab", L"Close Tab", L""});
    out.push_back({L"duplicate-tab", L"Duplicate Current Tab", L""});
    out.push_back({L"reload", L"Reload Tab", L""});
    out.push_back({L"hard-reload", L"Reload Tab Without Cache", L""});
    out.push_back({L"print", L"Print", L""});
  }
  if (in_space && web) {
    out.push_back(tab->pinned() ? OmniboxAction{L"toggle-pin", L"Unpin Tab", L""}
                                : OmniboxAction{L"toggle-pin", L"Pin Tab", L""});
    if (tab->pinned() && tab->pin_changed()) out.push_back({L"reset-pinned-tab", L"Reset Pinned Tab", L""});
    if (CanAddToEssentials(tab)) out.push_back({L"add-essential", L"Add to Essentials", L""});
  }
  if (active_essential_) out.push_back({L"remove-essential", L"Remove from Essentials", L""});
  // "Focus on <space>" for every other space (matched by its name).
  for (Workspace* ws : Spaces()) {
    if (ws == workspace_) continue;
    out.push_back({L"space:" + std::to_wstring(ws->id()), L"Focus on " + ws->record().name,
                   ws->record().name});
  }
  return out;
}

void BrowserWindow::RunQuickAction(const std::wstring& id) {
  Tab* tab = active_tab();
  Application* app = Application::Get();
  if (id.rfind(L"space:", 0) == 0) {
    if (Workspace* ws = FindSpace(_wtoi64(id.c_str() + 6))) SwitchWorkspace(ws);
  } else if (id == L"compact-mode") {
    ToggleCompactMode();
  } else if (id == L"library") {
    OpenLibrary();
  } else if (id == L"new-split") {
    NewEmptySplit();
  } else if (id == L"unsplit") {
    UnsplitCurrent();
  } else if (id == L"new-folder") {
    if (tab && !tab->pinned() && !active_essential_) NewGroupWithTab(tab);
  } else if (id == L"collapse-folders" || id == L"expand-folders") {
    if (!workspace_) return;
    CaptureRows();
    for (TabGroupRecord g : workspace_->groups()) {
      g.collapsed = id == L"collapse-folders";
      workspace_->UpdateGroup(g);
    }
    Layout();
    AnimateRows();
  } else if (id == L"settings") {
    OpenInternalPage(kPageSettings);
  } else if (id == L"tabs-right") {
    ToggleTabsOnRight();
  } else if (id == L"space-routing") {
    OpenTab(L"orbit://settings/#newRoute", true);
  } else if (id == L"print") {
    if (tab && tab->browser()) tab->browser()->GetHost()->Print();
  } else if (id == L"theme-system" || id == L"theme-light" || id == L"theme-dark") {
    app->store().SetPref(Store::kGlobalScope, L"theme", id.substr(6));
    app->ApplyThemeToAllWindows();
  } else if (id == L"add-essential") {
    if (tab) AddToEssentials(tab);
  } else if (id == L"remove-essential") {
    if (active_essential_) RemoveFromEssentials(active_essential_);
  } else if (!RunShortcutAction(id)) {
    // Everything else is a keyboard-shortcut action: run its default chord.
    if (const ShortcutAction* a = FindShortcutAction(id); a && !a->key.empty()) {
      DispatchAccelerator(a->key.vk, a->key.ctrl, a->key.shift, a->key.alt);
    }
  }
}

bool BrowserWindow::RunShortcutAction(const std::wstring& id) {
  // Actions Zen ships without a default key.
  Tab* tab = active_tab();
  if (id.rfind(L"space-", 0) == 0) {
    const int n = _wtoi(id.c_str() + 6);
    const std::vector<Workspace*> spaces = Spaces();
    if (n < 1 || n > static_cast<int>(spaces.size())) return true;
    SwitchWorkspace(spaces[static_cast<size_t>(n - 1)]);
  } else if (id == L"find") {
    FindBar::Open(this);
  } else if (id == L"find-next") {
    FindBar::FindNext(this, true);
  } else if (id == L"find-previous") {
    FindBar::FindNext(this, false);
  } else if (id == L"extensions") {
    if (!profile_->is_private()) OpenInternalPage(kPageExtensions);
  } else if (id == L"duplicate-tab") {
    if (tab) DuplicateTab(tab);
  } else if (id == L"reset-pinned-tab") {
    if (tab && tab->pinned()) ResetPinnedTab(tab, false);
  } else if (id == L"create-space") {
    NewWorkspace();
  } else if (id == L"show-sidebar") {
    if (CompactHidesSidebar()) SetSidebarRevealed(true);
  } else {
    return false;
  }
  return true;
}

std::wstring BrowserWindow::MenuLabel(const wchar_t* label, const wchar_t* action) {
  const std::wstring key = FormatChord(Application::Get()->shortcuts().Effective(action));
  return key.empty() ? std::wstring(label) : std::wstring(label) + L"\t" + key;
}

bool BrowserWindow::DispatchAccelerator(int vk, bool ctrl, bool shift, bool alt) {
  Tab* tab = active_tab();
  if (ctrl && shift && alt && vk == 'C') return CopyCurrentUrl(/*markdown=*/true);  // Zen: Ctrl+Shift+Alt+C
  // Ctrl+Alt is AltGr on many keyboard layouts, so it is not used for
  // shortcuts, except for keys AltGr cannot type with: Zen's space
  // navigation on Ctrl+Alt+Left/Right.
  if (alt && shift && !ctrl && vk == 'L') {
    ToggleLibrary();  // Zen: zen-library-toggle (Alt+Shift+L)
    return true;
  }
  if (ctrl && alt) {
    if (shift) return false;
    switch (vk) {
      case VK_LEFT:
      case VK_RIGHT:
        SelectRelativeWorkspace(vk == VK_RIGHT ? 1 : -1);
        return true;
      // Zen's split view shortcuts (ZenKeyboardShortcuts, accel+alt).
      case 'G':
        SplitWithNext(SplitGrid::kGrid);
        return true;
      case 'V':
        SplitWithNext(SplitGrid::kVertical);
        return true;
      case 'H':
        SplitWithNext(SplitGrid::kHorizontal);
        return true;
      case 'U':
        UnsplitCurrent();
        return true;
      default:
        return false;
    }
  }

  if (ctrl && !shift) {
    switch (vk) {
      case 'O':
        if (!glance_) return false;
        ExpandGlance();  // Zen: cmd_zenGlanceExpand
        return true;
      case 'F':
        FindBar::Open(this);
        return true;
      case 'L':
      case 'K':
        OpenLocation();
        return true;
      case 'T':
        NewTab();
        return true;
      case 'W':
      case VK_F4:
        CloseTabShortcut(tab);
        return true;
      case 'N':
        OpenNormalWindow();
        return true;
      case 'R':
        if (tab) tab->Reload(false);
        return true;
      case 'S':
        ToggleCompactMode();  // Zen: cmd_toggleCompactModeIgnoreHover
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
        // Zen: new unsynced (blank) window. Orbit windows never share tabs,
        // so every window is unsynced.
        OpenNormalWindow();
        return true;
      case 'P':
        Application::Get()->OpenPrivateWindow();  // Zen/Firefox: Ctrl+Shift+P
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
      case 'O':
        OpenInternalPage(kPageBookmarks);
        return true;
      case 'C':
        CopyCurrentUrl();  // Zen: cmd_zenCopyCurrentURL
        return true;
      case 'K':
        ClearUnpinnedTabs();  // Zen: cmd_zenCloseUnpinnedTabs
        return true;
      case VK_MULTIPLY:
      case '8':  // Shift+8 is "*" on US layouts
        NewEmptySplit();  // Zen: cmd_zenNewEmptySplit (accel+shift+*)
        return true;
      case 'A':
        if (!profile_->is_private()) OpenInternalPage(kPageExtensions);  // Zen: the add-ons manager
        return true;
      case 'D':
        TogglePinActiveTab();  // Zen: cmd_zenTogglePinTab
        return true;
      case 'I':
        ToggleDevTools();
        return true;
      default:
        return false;
    }
  }
  if (alt && !ctrl) {
    switch (vk) {
      case VK_LEFT:
        GoBack();
        return true;
      case VK_RIGHT:
        if (tab) tab->GoForward();
        return true;
      case 'D':
        OpenLocation();
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
          CancelAddressBar();
        } else {
          OpenLocation();
        }
        return true;
      case VK_F3:
        FindBar::FindNext(this, !shift);
        return true;
      case VK_F11:
        ToggleWindowFullscreen();
        return true;
      case VK_F12:
        return DispatchAccelerator('I', true, true, false);
      case VK_BROWSER_BACK:
        GoBack();
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
  if (row.linked) {
    const auto children = LinkedChildren();
    const int k = LinkedIndex(row.linked);
    Workspace* linked = k >= 0 ? children[static_cast<size_t>(k)] : nullptr;
    if (linked && (vk == VK_RETURN || vk == VK_SPACE)) {
      FocusLinkedSpace(linked);
      return true;
    }
    if (linked && vk == VK_APPS) {
      const D2D1_RECT_F r = RowRect(row);
      POINT pt{static_cast<LONG>(px(r.left + 20)), static_cast<LONG>(px(r.bottom))};
      ::ClientToScreen(hwnd_, &pt);
      ShowLinkedMenu(linked, pt);
      return true;
    }
  }
  Tab* tab = row.is_group || row.linked ? nullptr : workspace_->tabs()[static_cast<size_t>(row.tab)].get();
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
      } else if (row.is_group) {
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
      } else if (row.is_group) {
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
  if (library_browser_) {
    library_open_ = false;
    library_browser_->GetHost()->CloseBrowser(true);
  }
  CloseSiteData();
  CloseThemePicker();  // stores what was picked; its browser must close too
  CloseBoostEditor();
  if (glance_) {
    // ZenGlanceManager.onUnload: an open glance is not kept for next time.
    Tab* tab = glance_->tab;
    if (workspace_) workspace_->set_active_tab(glance_->parent);
    glance_.reset();
    PlaceGlanceChrome(false);
    tab->RequestClose();
  }
  ::ShowWindow(hwnd_, SW_HIDE);
  if (workspace_) workspace_->set_window(nullptr);
  for (Workspace* ws : Application::Get()->AllContainers()) {
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
  rec.linked_id = linked_space_ ? linked_space_->id() : 0;
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
      if (caption_revealed_) ::SetTimer(hwnd_, kCaptionHideTimer, kCaptionHideDelayMs, nullptr);
      if (sidebar_revealed_) ::SetTimer(hwnd_, kSidebarHideTimer, kSidebarHideDelayMs, nullptr);
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
      if (toast_) toast_->Reposition();
      return 0;
    case WM_MOVE:
      if (Tab* tab = active_tab(); tab && tab->browser()) {
        tab->browser()->GetHost()->NotifyMoveOrResizeStarted();
        if (tab->devtools_open()) ShowContent();  // their pane is a popup over the page area
      }
      if (popup_visible_) PositionFloatingUrlBar();
      if (toast_) toast_->Reposition();
      return 0;
    case WM_EXITSIZEMOVE:
      Application::Get()->ScheduleSave();
      return 0;
    case WM_DPICHANGED: {
      dpi_ = HIWORD(wp);
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      if (rt_) rt_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
      // The compact-mode card has its own target, created at the old DPI:
      // it is rebuilt (with its region and favicon bitmaps) at the new one.
      overlay_rt_.Reset();
      overlay_brush_.Reset();
      overlay_favicons_.clear();
      overlay_pinned_favicons_.clear();
    overlay_ext_icons_.clear();
      overlay_size_ = {};
      grain_brush_.Reset();
      grain_rt_ = nullptr;
      space_snapshot_.Reset();
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
      if (reinterpret_cast<HWND>(lp) == rename_edit_) {
        // #tab-label-input is transparent: match what is under the editor.
        HDC dc = reinterpret_cast<HDC>(wp);
        ::SetTextColor(dc, ui::ToColorRef(ui::Over(palette_.text, rename_bg_)));
        ::SetBkColor(dc, ui::ToColorRef(rename_bg_));
        return reinterpret_cast<LRESULT>(rename_brush_);
      }
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
        if (!popup_visible_ && ::GetFocus() == address_) {
          ShowFloatingUrlBar(/*new_tab=*/false, /*select_all=*/false, PopupAnchor::kSidebar);
        } else {
          UpdateUrlResults();
        }
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
      if (caption_revealed_) ::SetTimer(hwnd_, kCaptionHideTimer, kCaptionHideDelayMs, nullptr);
      if (sidebar_revealed_) ::SetTimer(hwnd_, kSidebarHideTimer, kSidebarHideDelayMs, nullptr);
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
      if (hit.kind == HitKind::kEssential && essentials()) {
        PinnedCloseShortcut(essentials()->tabs()[static_cast<size_t>(hit.index)].get(),
                            /*close_if_unloaded=*/true);
        return 0;
      }
      if ((hit.kind == HitKind::kTab || hit.kind == HitKind::kTabClose) && workspace_) {
        Tab* t = workspace_->tabs()[static_cast<size_t>(hit.index)].get();
        // zen.pinned-tab-manager.wheel-close-if-pending: a middle click closes
        // a pinned tab only once it is unloaded; otherwise it unloads/resets.
        if (t->pinned()) {
          PinnedCloseShortcut(t, /*close_if_unloaded=*/true);
        } else {
          CloseTab(t);
        }
      }
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (reinterpret_cast<HWND>(lp) != hwnd_) {
        if (split_drag_ && workspace_) workspace_->SaveSplit(split_drag_->split_id);
        split_drag_.reset();
        dragging_tab_ = false;
        resizing_sidebar_ = false;
        resizing_devtools_ = false;
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
      if (HitTestNonClient(lp) == HTCAPTION && !InSidebar(x)) {
        break;  // the window's system menu
      }
      OnContextMenu(pt.x, pt.y, x, dip(static_cast<float>(client.y)));
      return 0;
    }
    case WM_MOUSEWHEEL:
      OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wp));
      return 0;
    case WM_MOUSEHWHEEL: {
      POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
      ::ScreenToClient(hwnd_, &pt);
      OnMouseHWheel(GET_WHEEL_DELTA_WPARAM(wp), dip(static_cast<float>(pt.x)));
      return 0;
    }
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        POINT pt;
        ::GetCursorPos(&pt);
        ::ScreenToClient(hwnd_, &pt);
        const Hit hit = HitTest(dip(static_cast<float>(pt.x)), dip(static_cast<float>(pt.y)));
        const bool resize = resizing_sidebar_ || hit.kind == HitKind::kSidebarEdge || resizing_devtools_ ||
                            hit.kind == HitKind::kDevToolsEdge;
        LPCWSTR cursor = resize ? IDC_SIZEWE : IDC_ARROW;
        if (split_drag_) {
          cursor = split_drag_->vertical ? IDC_SIZEWE : IDC_SIZENS;
        } else if (hit.kind == HitKind::kSplitHandle) {
          std::vector<SplitHandle> handles;
          SplitPanes(&handles);
          if (static_cast<size_t>(hit.index) < handles.size()) {
            cursor = handles[static_cast<size_t>(hit.index)].vertical ? IDC_SIZEWE : IDC_SIZENS;
          }
        }
        ::SetCursor(::LoadCursorW(nullptr, cursor));
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
    case kRecreateTabMessage: {
      // A tab reset to its pinned page gets a fresh browser; a tab that moved
      // to another profile (wParam 1) only when it is on screen.
      Tab* t = reinterpret_cast<Tab*>(lp);
      if (workspace_ && (workspace_->IndexOf(t) >= 0 || IsEssential(t)) && !t->has_browser() && !closing_) {
        const bool shown = t == active_tab() ||
                           std::find(content_shown_.begin(), content_shown_.end(), t) != content_shown_.end();
        if (wp == 1 && !shown) return 0;
        t->EnsureBrowser(this);
        if (shown) ShowContent();
      }
      return 0;
    }
    case kCancelRenameMessage:
      if (renaming() && ::GetFocus() != rename_edit_) CancelRename();  // blur cancels (#halt)
      return 0;
    case kHidePopupMessage:
      // The floating bar lost focus (clicked elsewhere, window switch, ...).
      if (popup_visible_ && ::GetFocus() != address_) HideFloatingUrlBar(true);
      return 0;
    case WM_KILLFOCUS:
      Invalidate();
      return 0;
    case WM_ACTIVATEAPP:
      if (!wp) SetWindowActive(false);  // another application took the foreground
      break;
    case WM_ACTIVATE: {
      // The floating URL bar is our own popup: activating it keeps the window active.
      const HWND other = reinterpret_cast<HWND>(lp);
      SetWindowActive(LOWORD(wp) != WA_INACTIVE || (other && (other == popup_ || ::GetWindow(other, GW_OWNER) == hwnd_)));
    }
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
      if (wp == kMediaCardTimer) {
        ::KillTimer(hwnd_, kMediaCardTimer);
        UpdateMediaCard();
      } else if (wp == kAnimTimer) {
        if (compact_hiding_ && !compact_slide_.Running(::GetTickCount64())) {
          compact_hiding_ = false;  // slid out: drop the card's layout
          Layout();
        } else {
          MoveOverlay();
        }
        if (caption_moving_) {
          // Move-only: the page card keeps its size while it glides.
          caption_moving_ = caption_reveal_.Running(::GetTickCount64());
          PlaceContent();
        }
        if (glance_animating()) {
          const uint64_t now = ::GetTickCount64();
          if (glance_->close_start && now - glance_->close_start >= kGlanceAnimMs) {
            glance_->close_start = 0;
            CloseGlance();  // #finalizeGlanceClosing
          } else {
            if (glance_->anim_start && now - glance_->anim_start >= kGlanceAnimMs) glance_->anim_start = 0;
            ShowContent();  // the glance moves along its arc (or lands)
          }
        }
        if (library_host_ && (library_slide_.Running(::GetTickCount64()) ||
                              ::IsWindowVisible(library_host_))) {
          PlaceLibrary();
          PlaceContent();
        }
        Invalidate();
      } else if (wp == kSwipeEndTimer) {
        EndSwipe();
      } else if (wp == kSpinnerTimer) {
        spinner_phase_ = std::fmod(spinner_phase_ + 0.12f, 1.f);
        Invalidate();
      } else if (wp == kCaptionHideTimer || wp == kSidebarHideTimer) {
        ::KillTimer(hwnd_, wp);
        POINT pt;
        ::GetCursorPos(&pt);
        const HWND under = ::WindowFromPoint(pt);
        const bool inside = under == hwnd_ || ::IsChild(hwnd_, under);
        ::ScreenToClient(hwnd_, &pt);
        const float x = dip(static_cast<float>(pt.x));
        const float y = dip(static_cast<float>(pt.y));
        using namespace ui::metrics;
        const bool menu_open = ::GetCapture() != nullptr && ::GetCapture() != hwnd_;
        if (wp == kCaptionHideTimer && caption_revealed_ && !IsCaptionHit(pressed_) &&
            !menu_open && (!inside || y > (CompactHidesToolbar() ? kToolbarHeight : kCaptionHeight) + kSeparation) &&
            !(CompactHidesToolbar() && ::GetFocus() == address_)) {
          SetCaptionRevealed(false);
        }
        const bool editing_here = ::GetFocus() == address_ && !popup_visible_;
        if (wp == kSidebarHideTimer && sidebar_revealed_ && !dragging_tab_ &&
            !resizing_sidebar_ && !editing_here && !menu_open &&
            (!inside || (right_side_ ? x < Width() - SidebarPanelWidth() - kSeparation
                                     : x > SidebarPanelWidth() + kSeparation))) {
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
  if (split_drag_ && (keys & MK_LBUTTON) && workspace_) {
    if (Workspace::SplitView* split = workspace_->FindSplit(split_drag_->split_id)) {
      const float moved = (split_drag_->vertical ? x : y) - split_drag_->origin;
      ResizeSplit(split->root, split_drag_->path, split_drag_->index, split_drag_->start,
                  moved / split_drag_->extent * 100.f,
                  7.f /* zen.splitView.min-resize-width */);
      ShowContent();
      Invalidate();
    }
    return;
  }
  if (!tracking_mouse_) {
    // Forwarded overlay input tracks the overlay, which is under the pointer.
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, forwarding_mouse_ ? overlay_ : hwnd_, 0};
    ::TrackMouseEvent(&tme);
    tracking_mouse_ = true;
  }
  UpdateReveal(x, y);
  const bool over_sidebar = InSidebar(x);
  if (over_sidebar != sidebar_hovered_) {
    sidebar_hovered_ = over_sidebar;
    UpdateAddressColors();
    Invalidate();
  }
  if (resizing_sidebar_) {
    sidebar_width_ = std::clamp(right_side_ ? Width() - x : x, ui::metrics::kSidebarMinWidth,
                                ui::metrics::kSidebarMaxWidth);
    Layout();
    return;
  }
  if (resizing_devtools_) {
    const RECT content = ContentRectPx();
    devtools_width_ = std::max(240.f, dip(static_cast<float>(content.right)) - x);
    ShowContent();
    return;
  }
  if ((keys & MK_LBUTTON) && pressed_.kind == HitKind::kWorkspace && !collapsed_ && workspace_) {
    if (!reordering_spaces_ && std::fabs(px(x) - static_cast<float>(press_px_.x)) > px(5.f)) reordering_spaces_ = true;
    if (reordering_spaces_) {
      // The dragged icon goes before or after the icon under the pointer
      // (by its halves), as the toolbarbuttons are re-inserted in Zen.
      const std::vector<Workspace*> spaces = Spaces();
      const int from = pressed_.index;
      if (from >= 0 && from < static_cast<int>(spaces.size())) {
        Workspace* dragged = spaces[static_cast<size_t>(from)];
        for (size_t i = 0; i < workspace_dots_.size(); ++i) {
          const int index = first_dot_workspace_ + static_cast<int>(i);
          const D2D1_RECT_F& r = workspace_dots_[i];
          if (index == from || x <= r.left || x >= r.right) continue;
          const bool before = x < (r.left + r.right) / 2.f;
          int to = before ? index : index + 1;
          if (to > from) --to;
          if (to != from) {
            Application::Get()->MoveSpace(dragged, to);
            pressed_.index = to;
            Layout();
          }
          break;
        }
      }
      Invalidate();
      return;
    }
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
      const Hit over = HitTest(x, y);
      drop_linked_ = over.kind == HitKind::kLinked ? over.index : -1;
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
  // Where the press happened (the message's position, in client pixels).
  press_px_ = {static_cast<LONG>(std::lround(px(x))), static_cast<LONG>(std::lround(px(y)))};
  if (pressed_.kind == HitKind::kNone) return;
  ::SetCapture(hwnd_);
  if (pressed_.kind == HitKind::kSplitHandle) {
    // handleSplitterMouseDown: sizes are replayed from the drag's start.
    std::vector<SplitHandle> handles;
    SplitPanes(&handles);
    const Workspace::SplitView* split = ActiveSplit();
    if (split && static_cast<size_t>(pressed_.index) < handles.size()) {
      const SplitHandle& handle = handles[static_cast<size_t>(pressed_.index)];
      SplitNode root = split->root;
      const SplitNode* node = SplitNodeAt(root, handle.path);
      const RECT content = ContentRectPx();
      const SplitRect bounds{dip(static_cast<float>(content.left)), dip(static_cast<float>(content.top)),
                             dip(static_cast<float>(content.right)),
                             dip(static_cast<float>(content.bottom))};
      const SplitRect nr = SplitNodeRect(split->root, bounds, ui::metrics::kSeparation, handle.path);
      if (node && !node->leaf()) {
        SplitDrag drag;
        drag.split_id = split->id;
        drag.path = handle.path;
        drag.index = handle.index;
        drag.vertical = handle.vertical;
        for (const auto& child : node->children) drag.start.push_back(child.size);
        drag.origin = handle.vertical ? x : y;
        const float gaps = ui::metrics::kSeparation * static_cast<float>(node->children.size() - 1);
        drag.extent = std::max(1.f, (handle.vertical ? nr.right - nr.left : nr.bottom - nr.top) - gaps);
        split_drag_ = std::move(drag);
      }
    }
  }
  if (pressed_.kind == HitKind::kSidebarEdge) resizing_sidebar_ = true;
  if (pressed_.kind == HitKind::kDevToolsEdge) resizing_devtools_ = true;
  Invalidate();
}

void BrowserWindow::OnLButtonDoubleClick(float x, float y) {
  // Double-clicking the empty space below the tabs opens a tab; any other
  // double click counts as a second ordinary click.
  const Hit hit = HitTest(x, y);
  if (hit.kind == HitKind::kTabListEmpty) {
    NewTab();
    return;
  }
  if (!collapsed_ && hit.kind == HitKind::kTab && workspace_) {
    StartRenameTab(workspace_->tabs()[static_cast<size_t>(hit.index)].get());
    return;
  }
  if (!collapsed_ && hit.kind == HitKind::kWorkspaceIndicator && workspace_) {
    if (workspace_->PinnedCount() == 0) {
      StartRenameWorkspace(workspace_);
    } else {
      TogglePinnedSection();  // the second click of a double click
    }
    return;
  }
  OnLButtonDown(x, y);
}

void BrowserWindow::OnLButtonUp(float x, float y) {
  if (split_drag_) {
    const int64_t id = split_drag_->split_id;
    split_drag_.reset();
    pressed_ = {};
    if (::GetCapture() == hwnd_) ::ReleaseCapture();
    if (workspace_) workspace_->SaveSplit(id);
    return;
  }
  if (resizing_devtools_) {
    resizing_devtools_ = false;
    pressed_ = {};
    if (::GetCapture() == hwnd_) ::ReleaseCapture();
    Application::Get()->store().SetPref(Store::kGlobalScope, L"devtools_width",
                                        std::to_wstring(static_cast<int>(devtools_width_)));
    for (const auto& w : Application::Get()->windows()) {
      w->devtools_width_ = devtools_width_;
      if (w.get() != this) w->ShowContent();
    }
    return;
  }
  const Hit pressed = pressed_;
  const bool was_dragging = dragging_tab_;
  const bool was_resizing = resizing_sidebar_;
  if (reordering_spaces_) {
    reordering_spaces_ = false;
    pressed_ = {};
    if (::GetCapture() == hwnd_) ::ReleaseCapture();
    Invalidate();
    return;  // the order is already stored; no switch
  }
  pressed_ = {};
  dragging_tab_ = false;
  resizing_sidebar_ = false;
  if (::GetCapture() == hwnd_) ::ReleaseCapture();

  if (was_resizing) {
    Application::Get()->store().SetPref(Store::kGlobalScope, L"sidebar_width",
                                        std::to_wstring(static_cast<int>(sidebar_width_)));
    return;
  }
  const int dropped_on_linked = drop_linked_;
  drop_linked_ = -1;
  if (was_dragging && workspace_ && drag_from_ >= 0 &&
      drag_from_ < static_cast<int>(workspace_->tabs().size())) {
    Tab* tab = workspace_->tabs()[static_cast<size_t>(drag_from_)].get();
    // Released outside this window: over another Orbit window, the tab moves
    // into its space; anywhere else it tears off into a linked window.
    POINT screen;
    ::GetCursorPos(&screen);
    const HWND under = ::WindowFromPoint(screen);
    HWND root = under ? ::GetAncestor(under, GA_ROOT) : nullptr;
    for (HWND owner = root ? ::GetWindow(root, GW_OWNER) : nullptr; owner; owner = ::GetWindow(owner, GW_OWNER)) {
      root = owner;  // a popup of a window (its URL bar, a panel) counts as that window
    }
    if (root != hwnd_) {
      BrowserWindow* target = nullptr;
      for (const auto& w : Application::Get()->windows()) {
        if (w.get() != this && !w->closing() && w->hwnd() == root) target = w.get();
      }
      drag_from_ = drop_index_ = -1;
      if (target) {
        MoveTabToWindow(tab, target);
      } else if (!IsGlanceTab(tab)) {
        TearOffTab(tab, screen);
      }
      Layout();
      return;
    }
    // Onto a linked window's row: into that window.
    if (const auto children = LinkedChildren();
        dropped_on_linked >= 0 && dropped_on_linked < static_cast<int>(children.size())) {
      Workspace* linked = children[static_cast<size_t>(dropped_on_linked)];
      drag_from_ = drop_index_ = -1;
      FocusLinkedSpace(linked);
      if (BrowserWindow* w = linked->window()) MoveTabToWindow(tab, w);
      Layout();
      return;
    }
    // Dropping onto a group member joins that group; dropping anywhere else
    // (including just above a group header) leaves any group.
    const auto rows = Rows();
    const int n_tabs = static_cast<int>(workspace_->tabs().size());
    int target = n_tabs;
    int64_t group = 0;
    if (drop_index_ >= 0 && drop_index_ < static_cast<int>(rows.size())) {
      const Row& at = rows[static_cast<size_t>(drop_index_)];
      if (at.linked) {
        // Before a linked row: the end of the tabs.
      } else if (at.is_group) {
        for (int i = 0; i < n_tabs; ++i) {  // before the folder's first item
          if (workspace_->InGroup(workspace_->tabs()[static_cast<size_t>(i)].get(), at.group_id)) {
            target = i;
            break;
          }
        }
        if (const TabGroupRecord* g = workspace_->FindGroup(at.group_id)) group = g->parent_id;
      } else {
        target = at.tab;
        group = at.group_id;
      }
    }
    if (const auto* split = workspace_->SplitOf(tab)) {
      const int64_t split_id = split->id;
      const int n = static_cast<int>(workspace_->SplitMembers(*split).size());
      workspace_->SetTabGroup(tab, group);
      workspace_->MoveSplit(split_id, target > drag_from_ ? target - n : target);
      drag_from_ = drop_index_ = -1;
      Layout();
      return;
    }
    if (target > drag_from_) --target;  // index after removal
    workspace_->SetTabGroup(tab, group);
    workspace_->MoveTab(tab, target);
    DropFolderPlaceholder(group);
    drag_from_ = drop_index_ = -1;
    Layout();
    return;
  }
  drag_from_ = drop_index_ = -1;
  if (HitTest(x, y) == pressed) Click(pressed);
  Invalidate();
}

void BrowserWindow::SetWindowActive(bool active) {
  if (window_active_ == active) return;
  window_active_ = active;
  if (grey_out_inactive_) Invalidate();
}

void BrowserWindow::OnMouseWheel(int delta) {
  POINT pt;
  ::GetCursorPos(&pt);
  ::ScreenToClient(hwnd_, &pt);
  const float y = dip(static_cast<float>(pt.y));
  const float x = dip(static_cast<float>(pt.x));
  // zen.workspaces.scroll-modifier-key = ctrl: Ctrl+wheel over the sidebar
  // switches spaces (down = next), at most once per 200 ms.
  if (InSidebar(x) && ::GetKeyState(VK_CONTROL) < 0) {
    const ULONGLONG now = ::GetTickCount64();
    if (now - last_space_scroll_ >= 200 && delta != 0) {
      last_space_scroll_ = now;
      SelectRelativeWorkspace(delta < 0 ? 1 : -1);
    }
    return;
  }
  if (InSidebar(x) && y >= list_.top && y < list_.bottom) {
    scroll_ -= static_cast<float>(delta) / WHEEL_DELTA * ui::metrics::kRowHeight * 2.f;
    ClampScroll();
    Invalidate();
  }
}

void BrowserWindow::OnMouseHWheel(int delta, float x) {
  // ZenSpacesSwipe: horizontal scrolling over the sidebar drags the space
  // strip; letting go decides (EndSwipe).
  if (!InSidebar(x) || profile_->is_private() || chrome_hidden() || space_switch_animating()) return;
  const uint64_t now = ::GetTickCount64();
  if (!swiping_) {
    swiping_ = true;
    swipe_last_ = swipe_back_.Running(now) ? swipe_back_.Value(now) : 0.f;
    swipe_back_.Jump(0.f);
  }
  // Scrolling right (positive) moves toward the next space: the strip goes left.
  const float d = -static_cast<float>(delta) / WHEEL_DELTA * kSwipePxPerNotch;
  const float strip = std::max(SidebarPanelWidth(), 1.f);
  float translate = swipe_last_ + d;
  // #handleSwipeUpdate: harder to pull the further it is.
  const float force = std::min(1.f, 1.f - std::fabs(translate) / (strip * 4.5f));
  if (force > 0.5f) {
    translate *= force;
    swipe_last_ = d + (translate - d) * 0.5f;
  } else {
    translate = swipe_last_;
  }
  swipe_last_ = translate;
  swipe_dx_ = translate;
  ::SetTimer(hwnd_, kSwipeEndTimer, kSwipeEndMs, nullptr);
  Invalidate();
}

float BrowserWindow::swipe_offset() const {
  if (swiping_) return swipe_dx_;
  return swipe_back_.Value(::GetTickCount64());
}

void BrowserWindow::EndSwipe() {
  // MozSwipeGesture: past a quarter of the strip it switches (the slide
  // continues from where the drag left it); otherwise it springs back.
  ::KillTimer(hwnd_, kSwipeEndTimer);
  if (!swiping_) return;
  swiping_ = false;
  const float strip = std::max(SidebarPanelWidth(), 1.f);
  const float progress = std::fabs(swipe_dx_) / strip;
  const uint64_t now = ::GetTickCount64();
  const int count = static_cast<int>(Spaces().size());
  if (progress >= kSwipeSuccess && count > 1) {
    swipe_handoff_ = std::min(progress, 0.9f);
    swipe_dx_ = 0.f;
    SelectRelativeWorkspace(swipe_last_ < 0.f ? 1 : -1);
    swipe_handoff_ = 0.f;
  } else {
    swipe_back_.Jump(swipe_dx_);
    swipe_back_.Start(0.f, now, ui::AnimMs(200), 0, orbit::Transition::Curve::kSpring);
    swipe_dx_ = 0.f;
  }
  UpdateAnimationTimer(now);
  Invalidate();
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
      GoBack();
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
    case HitKind::kSiteInfo:
      if (tab) OpenSiteData();  // ZenSiteDataPanel
      break;
    case HitKind::kStar:
      ToggleBookmark();
      break;
    case HitKind::kCopyUrl:
      CopyCurrentUrl();
      break;
    case HitKind::kExtensions:
      ShowExtensionsMenu();
      break;
    case HitKind::kExtensionPin:
      if (const ExtensionInfo* e = PinnedExtension(hit.index)) {
        const ExtensionInfo chosen = *e;
        OpenExtensionAction(chosen, ext_pin_rects_[static_cast<size_t>(hit.index)]);
      }
      break;
    case HitKind::kMenu:
      ShowMainMenu(anchor(menu_));
      break;
    case HitKind::kSidebarToggle:
      ToggleCompactMode();
      break;
    case HitKind::kWorkspaceIndicator:
      if (!collapsed_ && workspace_ && workspace_->PinnedCount() > 0) {
        TogglePinnedSection();
      } else if (collapsed_) {
        ShowWorkspaceMenu(-1, anchor(ws_indicator_));
      }
      break;
    case HitKind::kWorkspaceActions:
      ShowWorkspaceMenu(-1, anchor(ws_actions_));
      break;
    case HitKind::kWorkspace:
      if (collapsed_) {
        ShowWorkspaceMenu(-1, above(workspace_dots_.front()));
      } else if (const auto spaces = Spaces(); hit.index < static_cast<int>(spaces.size())) {
        SwitchWorkspace(spaces[static_cast<size_t>(hit.index)]);
      }
      break;
    case HitKind::kCreateNew:
      ShowCreateMenu(above(create_new_));
      break;
    case HitKind::kMediaFocus:
      MediaCommand("focus");
      break;
    case HitKind::kMediaPlayPause:
      MediaCommand("toggle");
      break;
    case HitKind::kMediaMute:
      MediaCommand("mute");
      break;
    case HitKind::kMediaClose:
      MediaCommand("close");
      break;
    case HitKind::kMediaSeek: {
      POINT pt;
      ::GetCursorPos(&pt);
      ::ScreenToClient(hwnd_, &pt);
      const float frac = std::clamp((dip(static_cast<float>(pt.x)) - media_card_.left - 10.f) /
                                        (media_card_.right - media_card_.left - 20.f),
                                    0.f, 1.f);
      MediaCommand(("seek:" + std::to_string(frac)).c_str());
      break;
    }
    case HitKind::kDownloads:
      ToggleLibrary();  // zen-library-button (zen.library.enabled) replaces downloads-button
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
      if (workspace_) CloseTabShortcut(workspace_->tabs()[static_cast<size_t>(hit.index)].get());
      break;
    case HitKind::kEssential:
      if (Workspace* ess = essentials();
          ess && hit.index < static_cast<int>(ess->tabs().size())) {
        ActivateTab(ess->tabs()[static_cast<size_t>(hit.index)].get());
      }
      break;
    case HitKind::kTabResetPin:
      if (workspace_) {
        ResetPinnedTab(workspace_->tabs()[static_cast<size_t>(hit.index)].get(),
                       /*separate_current_page=*/::GetKeyState(VK_CONTROL) < 0);
      }
      break;
    case HitKind::kGroup:
      if (workspace_ && hit.index >= 0) {
        ToggleGroup(workspace_->groups()[static_cast<size_t>(hit.index)].id);
      }
      break;
    case HitKind::kLiveError:
      if (workspace_ && hit.index >= 0) {
        LiveErrorAction(workspace_->groups()[static_cast<size_t>(hit.index)].id);
      }
      break;
    case HitKind::kLinked:
      if (const auto children = LinkedChildren(); hit.index >= 0 && hit.index < static_cast<int>(children.size())) {
        FocusLinkedSpace(children[static_cast<size_t>(hit.index)]);
      }
      break;
    case HitKind::kLinkedBack:
      if (workspace_ && workspace_->is_linked()) Application::Get()->BringBackLinkedSpace(workspace_);
      break;
    case HitKind::kCaptionMin:
    case HitKind::kCaptionMax:
    case HitKind::kCaptionClose:  // handled as non-client buttons
    case HitKind::kSidebarEdge:
    case HitKind::kDevToolsEdge:
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
    case HitKind::kEssential:
      ShowEssentialMenu(essentials()->tabs()[static_cast<size_t>(hit.index)].get(), pt);
      break;
    case HitKind::kGroup:
    case HitKind::kLiveError:
      if (hit.index >= 0) ShowGroupMenu(workspace_->groups()[static_cast<size_t>(hit.index)].id, pt);
      break;
    case HitKind::kLinked:
      if (const auto children = LinkedChildren(); hit.index >= 0 && hit.index < static_cast<int>(children.size())) {
        ShowLinkedMenu(children[static_cast<size_t>(hit.index)], pt);
      }
      break;
    default:
      if (InSidebar(x)) ShowWorkspaceMenu(-1, pt);
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
  const std::vector<Workspace*> workspaces = Spaces();
  for (size_t i = 0; i < workspaces.size(); ++i) {
    UINT flags = MF_STRING;
    if (workspaces[i] == workspace_) flags |= MF_GRAYED | MF_CHECKED;
    ::AppendMenuW(ws_menu, flags, kCmdWorkspaceBase + i, SpaceLabel(workspaces[i]).c_str());
  }
  const auto& windows = Application::Get()->windows();
  for (size_t i = 0; i < windows.size(); ++i) {
    BrowserWindow* w = windows[i].get();
    if (w == this || !CanShareTabs(w->profile_, profile_) || w->closing_ || !w->workspace_) continue;
    std::wstring label = L"Window: " + SpaceLabel(w->workspace_);
    ::AppendMenuW(win_menu, MF_STRING, kCmdWindowBase + i, label.c_str());
  }
  ::AppendMenuW(win_menu, MF_STRING | (profile_->is_private() || tab->empty_placeholder() ? MF_GRAYED : 0),
                kCmdMoveToNewWindow, L"New window");
  HMENU group_menu = ::CreatePopupMenu();
  ::AppendMenuW(group_menu, MF_STRING, kCmdNewGroup, L"New folder…");
  const auto& groups = workspace_->groups();
  if (!groups.empty()) ::AppendMenuW(group_menu, MF_SEPARATOR, 0, nullptr);
  for (size_t i = 0; i < groups.size(); ++i) {
    if (Application::Get()->live_folders().IsLive(groups[i].id)) continue;
    const UINT flags = MF_STRING | (groups[i].id == tab->record().group_id ? MF_CHECKED : 0);
    ::AppendMenuW(group_menu, flags, kCmdGroupBase + i, groups[i].name.c_str());
  }

  ::AppendMenuW(menu, MF_STRING, kCmdNewTabBelow, L"New tab below");
  ::AppendMenuW(menu, MF_STRING, kCmdReload, L"Reload");
  ::AppendMenuW(menu, MF_STRING, kCmdDuplicate, L"Duplicate");
  // tab-zen-split-tabs: "Split Out Tab" for a split member, else "Add Split
  // View..." (hidden for tabs that cannot split, as contextCanSplitTabs).
  if (workspace_->SplitOf(tab)) {
    ::AppendMenuW(menu, MF_STRING, kCmdSplitOut, L"Split Out Tab");
  } else if (!tab->pinned() && !tab->empty_placeholder()) {
    ::AppendMenuW(menu, MF_STRING, kCmdSplitTabs, L"Add Split View...");
  }
  ::AppendMenuW(menu, MF_STRING | (collapsed_ ? MF_GRAYED : 0), kCmdChangeLabel, L"Change Label\u2026");
  ::AppendMenuW(menu, MF_STRING, kCmdPin, tab->pinned() ? L"Unpin tab" : L"Pin tab");
  if (!tab->record().group_id) {
    const Workspace* ess = essentials();
    const std::wstring label = L"Add to Essentials\t" +
                               std::to_wstring(ess ? ess->tabs().size() : 0) + L" / " +
                               std::to_wstring(kMaxEssentials);
    ::AppendMenuW(menu, MF_STRING | (CanAddToEssentials(tab) ? 0 : MF_GRAYED), kCmdAddEssential,
                  label.c_str());
  }
  if (tab->pinned()) {
    // ZenPinnedTabManager._insertItemsIntoTabContextMenu.
    HMENU edit_pinned = ::CreatePopupMenu();
    ::AppendMenuW(edit_pinned, MF_STRING | (tab->pin_changed() ? 0 : MF_GRAYED),
                  kCmdReplacePinnedUrl, L"Replace with Current URL");
    ::AppendMenuW(edit_pinned, MF_STRING, kCmdEditPinnedUrl, L"Edit\u2026");
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(edit_pinned), L"Edit Pinned URL");
    ::AppendMenuW(menu, MF_STRING, kCmdResetPinned, L"Reset Pinned Tab");
  }
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
  ::AppendMenuW(menu, MF_STRING, kCmdClose, MenuLabel(L"Close", L"close-tab").c_str());
  ::AppendMenuW(menu, MF_STRING, kCmdCloseOthers, L"Close other tabs");
  ::AppendMenuW(menu, MF_STRING, kCmdCloseBelow, L"Close tabs below");
  ::AppendMenuW(menu, MF_STRING | (profile_->is_private() ? MF_GRAYED : 0), kCmdReopen,
                MenuLabel(L"Reopen closed tab", L"reopen-tab").c_str());

  const UINT cmd = static_cast<UINT>(ui::TrackMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, hwnd_, palette_));
  ::DestroyMenu(menu);  // destroys submenus too
  // |tab| may have been closed by Chromium while the menu was open.
  if (workspace_->IndexOf(tab) < 0) return;

  if (cmd >= kCmdWorkspaceBase && cmd < kCmdWorkspaceBase + workspaces.size()) {
    MoveTabToWorkspace(tab, workspaces[cmd - kCmdWorkspaceBase]);
    return;
  }
  if (cmd >= kCmdWindowBase && cmd < kCmdWindowBase + windows.size()) {
    MoveTabToWindow(tab, windows[cmd - kCmdWindowBase].get());
    return;
  }
  if (cmd >= kCmdGroupBase && cmd < kCmdGroupBase + workspace_->groups().size()) {
    CaptureRows();
    const int64_t folder = workspace_->groups()[cmd - kCmdGroupBase].id;
    workspace_->SetTabGroup(tab, folder);
    DropFolderPlaceholder(folder);
    Layout();
    AnimateRows();
    return;
  }
  if (cmd == kCmdNewGroup) {
    NewGroupWithTab(tab);
    return;
  }
  if (cmd == kCmdRemoveFromGroup) {
    CaptureRows();
    workspace_->SetTabGroup(tab, 0);
    Layout();
    AnimateRows();
    return;
  }
  switch (cmd) {
    case kCmdNewTabBelow:
      OpenTabNear(tab, Application::kNewTabUrl, true);
      ShowFloatingUrlBar(/*new_tab=*/false);
      break;
    case kCmdReload:
      tab->Reload(false);
      break;
    case kCmdDuplicate:
      DuplicateTab(tab);
      break;
    case kCmdSplitTabs:
      SplitTabWithCurrent(tab);
      break;
    case kCmdSplitOut:
      SplitOut(tab);
      break;
    case kCmdPin:
      TogglePin(tab);
      break;
    case kCmdResetPinned:
      ResetPinnedTab(tab, false);
      break;
    case kCmdChangeLabel:
      StartRenameTab(tab);
      break;
    case kCmdAddEssential:
      AddToEssentials(tab);
      break;
    case kCmdReplacePinnedUrl:
      ReplacePinnedUrlWithCurrent(tab);
      break;
    case kCmdEditPinnedUrl: {
      std::wstring text = tab->pinned_url();
      if (PromptText(L"Edit Pinned URL", L"Enter the URL this pinned tab should point to:", text) &&
          workspace_->IndexOf(tab) >= 0) {
        SetPinnedUrlFromText(tab, text);
      }
      break;
    }
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
      TearOffTab(tab);  // a linked window space
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

void BrowserWindow::ShowEssentialMenu(Tab* tab, POINT pt) {
  // The tab context menu as Zen trims it for Essentials: no Close, "Reset
  // Essential Tab", "Edit Essential URL", "Remove from Essentials".
  if (!tab) return;
  HMENU menu = ::CreatePopupMenu();
  ::AppendMenuW(menu, MF_STRING, kCmdReload, L"Reload");
  ::AppendMenuW(menu, MF_STRING, kCmdDuplicate, L"Duplicate");
  ::AppendMenuW(menu, MF_STRING | (tab->muted() ? MF_CHECKED : 0), kCmdMute, L"Mute site");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  HMENU edit = ::CreatePopupMenu();
  ::AppendMenuW(edit, MF_STRING | (tab->pin_changed() ? 0 : MF_GRAYED), kCmdReplacePinnedUrl,
                L"Replace with Current URL");
  ::AppendMenuW(edit, MF_STRING, kCmdEditPinnedUrl, L"Edit\u2026");
  ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), L"Edit Essential URL");
  ::AppendMenuW(menu, MF_STRING, kCmdResetPinned, L"Reset Essential Tab");
  ::AppendMenuW(menu, MF_STRING | (tab->has_browser() ? 0 : MF_GRAYED), kCmdDiscard,
                L"Put to sleep (unload)");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kCmdRemoveEssential, L"Remove from Essentials");
  const UINT cmd = static_cast<UINT>(ui::TrackMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, hwnd_, palette_));
  ::DestroyMenu(menu);
  if (!IsEssential(tab)) return;  // closed meanwhile
  switch (cmd) {
    case kCmdReload:
      tab->Reload(false);
      break;
    case kCmdDuplicate:
      OpenTab(tab->url(), true);
      break;
    case kCmdMute:
      tab->SetMuted(!tab->muted());
      break;
    case kCmdReplacePinnedUrl:
      ReplacePinnedUrlWithCurrent(tab);
      break;
    case kCmdEditPinnedUrl: {
      std::wstring text = tab->pinned_url();
      if (PromptText(L"Edit Essential URL", L"Enter the URL this essential tab should point to:",
                     text) &&
          IsEssential(tab)) {
        SetPinnedUrlFromText(tab, text);
      }
      break;
    }
    case kCmdResetPinned:
      ResetPinnedTab(tab, false);
      break;
    case kCmdDiscard:
      if (tab == active_tab()) {
        PinnedCloseShortcut(tab, false);
      } else {
        tab->Unload(/*mark_discarded=*/true);
      }
      break;
    case kCmdRemoveEssential:
      RemoveFromEssentials(tab);
      break;
    default:
      break;
  }
  Invalidate();
}

std::vector<D2D1_RECT_F> BrowserWindow::EssentialRects() const {
  std::vector<D2D1_RECT_F> out;
  const Workspace* ess = essentials();
  if (!ess || ess->tabs().empty() || !sidebar_visible()) return out;
  const int n = static_cast<int>(ess->tabs().size());
  const float sw = SidebarPanelWidth();
  if (collapsed_) {
    // Collapsed sidebar: one column (grid-template-columns: 1fr).
    const float cx = SidebarLeft() + sw / 2.f;
    float y = essentials_rect_.top;
    for (int i = 0; i < n; ++i) {
      out.push_back({cx - 24.f, y + 2.f, cx + 24.f, y + 38.f});
      y += 40.f;
    }
    return out;
  }
  // repeat(auto-fit, minmax(max(23.7%, 50px), 1fr)), gap 4px, 46px rows.
  constexpr float kGap = 4.f;
  constexpr float kTileH = 44.f;
  const float l = SidebarLeft() + kInner;
  const float aw = sw - 2 * kInner;
  const float min_w = std::max(aw * 0.237f, 50.f);
  int cols = std::max(1, static_cast<int>((aw + kGap) / (min_w + kGap)));
  cols = std::min(cols, n);  // auto-fit: empty tracks collapse, tiles stretch
  const float tile_w = (aw - kGap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
  // Layout() seeds essentials_rect_.top with the sidebar's running y, which is
  // right for both single and multiple toolbars (address_rect_ is in the
  // toolbar in the latter).
  const float top = essentials_rect_.top + 2.f;
  for (int i = 0; i < n; ++i) {
    const int row = i / cols;
    const int col = i % cols;
    const float x = l + static_cast<float>(col) * (tile_w + kGap);
    const float y = top + static_cast<float>(row) * (kTileH + kGap);
    out.push_back({x, y, x + tile_w, y + kTileH});
  }
  return out;
}

void BrowserWindow::ShowWorkspaceMenu(int index, POINT pt) {
  const std::vector<Workspace*> workspaces = Spaces();
  Workspace* target = (index >= 0 && index < static_cast<int>(workspaces.size()))
                          ? workspaces[static_cast<size_t>(index)]
                          : workspace_;
  if (!target) return;
  HMENU menu = ::CreatePopupMenu();
  for (size_t i = 0; i < workspaces.size(); ++i) {
    UINT flags = MF_STRING | (workspaces[i] == workspace_ ? MF_CHECKED : 0);
    std::wstring label = SpaceLabel(workspaces[i]);
    if (workspaces[i]->window() && workspaces[i]->window() != this) label += L"  (other window)";
    ::AppendMenuW(menu, flags, kCmdWorkspaceBase + i, label.c_str());
  }
  const UINT disabled = profile_->is_private() ? MF_GRAYED : 0;
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING | disabled, kCmdRename, L"Change Name\u2026");
  ::AppendMenuW(menu, MF_STRING | disabled, kCmdChangeIcon, L"Change Icon\u2026");
  ::AppendMenuW(menu, MF_STRING | disabled, kCmdEditTheme, L"Edit Theme\u2026");
  // Zen's container picker in the space editor: the profile (storage) the
  // space's tabs use.
  const auto& profiles = Application::Get()->profiles();
  HMENU profile_menu = ::CreatePopupMenu();
  for (size_t i = 0; i < profiles.size(); ++i) {
    const UINT checked = profiles[i].get() == target->profile() ? MF_CHECKED : 0;
    ::AppendMenuW(profile_menu, MF_STRING | checked, kCmdSpaceProfileBase + i, profiles[i]->record().name.c_str());
  }
  ::AppendMenuW(profile_menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(profile_menu, MF_STRING, kCmdSpaceNewProfile, L"New profile\u2026");
  ::AppendMenuW(menu, MF_POPUP | (disabled || target->is_linked() ? MF_GRAYED : 0),
                reinterpret_cast<UINT_PTR>(profile_menu), L"Profile");
  // #zenWorkspaceMoreActions: Manage Spaces…, Unload Space, Unload All Other
  // Spaces | Create Space, Delete Space | Space Routing Settings (Orbit adds
  // Move Earlier / Later).
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING | (chrome_hidden() ? MF_GRAYED : 0), kCmdManageSpaces, L"Manage Spaces\u2026");
  ::AppendMenuW(menu, MF_STRING, kCmdUnloadSpace, L"Unload Space");
  ::AppendMenuW(menu, MF_STRING, kCmdUnloadOtherSpaces, L"Unload All Other Spaces");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING | disabled, kCmdNewWorkspace, L"Create Space");
  const UINT not_linked = target->is_linked() ? MF_GRAYED : 0;
  ::AppendMenuW(menu, MF_STRING | disabled | not_linked, kCmdMoveUp, L"Move Earlier");
  ::AppendMenuW(menu, MF_STRING | disabled | not_linked, kCmdMoveDown, L"Move Later");
  ::AppendMenuW(menu, MF_STRING | disabled | not_linked, kCmdDelete, L"Delete Space");
  if (target->is_linked() && target->parent()) {
    ::AppendMenuW(menu, MF_STRING, kCmdLinkedBringBack, L"Bring Tabs Back");
  }
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING | disabled, kCmdSpaceRouting, L"Space Routing Settings");

  const UINT cmd = static_cast<UINT>(ui::TrackMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, hwnd_, palette_));
  ::DestroyMenu(menu);
  if (cmd >= kCmdWorkspaceBase && cmd < kCmdWorkspaceBase + workspaces.size()) {
    SwitchWorkspace(workspaces[cmd - kCmdWorkspaceBase]);
    return;
  }
  if (cmd >= kCmdSpaceProfileBase && cmd < kCmdSpaceProfileBase + profiles.size()) {
    MoveSpaceToProfile(target, profiles[cmd - kCmdSpaceProfileBase].get());
    return;
  }
  if (cmd == kCmdSpaceNewProfile) {
    std::wstring name = L"Profile " + std::to_wstring(profiles.size() + 1);
    if (!PromptText(L"New profile", L"Profile name:", name) || name.empty()) return;
    const uint32_t color = ui::kAccentChoices[profiles.size() % std::size(ui::kAccentChoices)];
    if (Profile* p = Application::Get()->CreateProfile(name, color, /*with_space=*/false)) {
      MoveSpaceToProfile(target, p);
    }
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
      if (workspaces[i] == target) return static_cast<int>(i);
    }
    return 0;
  }();
  switch (cmd) {
    case kCmdNewWorkspace:
      NewWorkspace();
      break;
    case kCmdRename:
      // Inline on the indicator when it shows this space, as Zen does.
      if (target == workspace_ && !collapsed_ && sidebar_visible()) {
        StartRenameWorkspace(target);
      } else {
        RenameWorkspace(target);
      }
      break;
    case kCmdChangeIcon: {
      std::wstring icon = target->record().icon;
      if (PromptText(L"Change Icon",
                     L"Type an emoji or a character (Win+. opens the emoji picker). "
                     L"Leave empty for no icon:",
                     icon)) {
        SetWorkspaceIcon(target, icon);
      }
      break;
    }
    case kCmdUnloadSpace:
      UnloadWorkspaceTabs(target);
      break;
    case kCmdManageSpaces:
      ToggleLibrarySection(L"spaces");  // cmd_zenManageWorkspaces
      break;
    case kCmdEditTheme:
      OpenThemePicker(target);  // context_zenChangeWorkspaceTheme
      break;
    case kCmdSpaceRouting:
      RunQuickAction(L"space-routing");  // cmd_zenOpenSpaceRoutingSettings
      break;
    case kCmdUnloadOtherSpaces:
      UnloadOtherWorkspaces(target);
      break;
    case kCmdMoveUp:
      Application::Get()->MoveSpace(target, pos - 1);
      Layout();
      break;
    case kCmdMoveDown:
      Application::Get()->MoveSpace(target, pos + 1);
      Layout();
      break;
    case kCmdDelete:
      if (!target->is_linked()) DeleteWorkspace(target);
      break;
    case kCmdLinkedBringBack:
      if (target->is_linked()) Application::Get()->BringBackLinkedSpace(target);
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
  CaptureRows();
  // Collapsing the group that holds the active tab keeps that tab selected
  // (its page stays visible); the header then stands in for it.
  workspace_->UpdateGroup(updated);
  focus_row_ = std::min(focus_row_, static_cast<int>(Rows().size()) - 1);
  Layout();
  AnimateRows();
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

static std::wstring FirstGrapheme(const std::wstring& icon);

void BrowserWindow::ShowGroupMenu(int64_t group_id, POINT pt) {
  // #zenFolderActions: Rename Folder…, Change Icon… | Unload All Tabs, New
  // Subfolder… | Change Space, Convert folder to Space | Unpack Folder,
  // Delete Folder (Orbit keeps Collapse and Color after them).
  const TabGroupRecord* g = workspace_ ? workspace_->FindGroup(group_id) : nullptr;
  if (!g) return;
  HMENU menu = ::CreatePopupMenu();
  const bool live = Application::Get()->live_folders().IsLive(group_id);
  if (live) {
    AppendLiveFolderMenu(menu, group_id);
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  }
  ::AppendMenuW(menu, MF_STRING, kCmdRenameGroup, L"Rename Folder\u2026");
  ::AppendMenuW(menu, MF_STRING, kCmdFolderIcon, L"Change Icon\u2026");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kCmdFolderUnload, L"Unload All Tabs");
  const bool can_nest = workspace_->GroupDepth(group_id) < Workspace::kMaxFolderDepth - 1 && !live;
  ::AppendMenuW(menu, MF_STRING | (can_nest ? 0 : MF_GRAYED), kCmdSubfolder, L"New Subfolder\u2026");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  HMENU spaces = ::CreatePopupMenu();
  const std::vector<Workspace*> workspaces = Spaces();
  for (size_t i = 0; i < workspaces.size(); ++i) {
    const UINT flags = MF_STRING | (workspaces[i] == workspace_ ? MF_GRAYED | MF_CHECKED : 0);
    ::AppendMenuW(spaces, flags, kCmdFolderSpaceBase + i, SpaceLabel(workspaces[i]).c_str());
  }
  // A live folder stays where it is (its items are tied to this folder).
  const UINT disabled = profile_->is_private() || live ? MF_GRAYED : 0;
  ::AppendMenuW(menu, MF_POPUP | disabled, reinterpret_cast<UINT_PTR>(spaces), L"Change Space");
  ::AppendMenuW(menu, MF_STRING | disabled, kCmdFolderToSpace, L"Convert folder to Space");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kCmdUngroup, L"Unpack Folder");
  ::AppendMenuW(menu, MF_STRING, kCmdCloseGroup, L"Delete Folder");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kCmdToggleGroup, g->collapsed ? L"Expand" : L"Collapse");
  HMENU colors = ::CreatePopupMenu();
  for (size_t i = 0; i < std::size(ui::kAccentChoices); ++i) {
    const UINT checked = g->color == ui::kAccentChoices[i] ? MF_CHECKED : 0;
    ::AppendMenuW(colors, MF_STRING | checked, kCmdColorBase + i, kColorNames[i]);
  }
  ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(colors), L"Color");
  const UINT cmd = static_cast<UINT>(ui::TrackMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, hwnd_, palette_));
  ::DestroyMenu(menu);
  g = workspace_->FindGroup(group_id);  // may be gone if its tabs closed meanwhile
  if (!g) return;
  TabGroupRecord updated = *g;
  if (cmd >= kCmdLiveRefresh && cmd < kCmdLiveRepoEnd) {
    RunLiveFolderCommand(group_id, cmd);
  } else if (cmd >= kCmdColorBase && cmd < kCmdColorBase + std::size(ui::kAccentChoices)) {
    updated.color = ui::kAccentChoices[cmd - kCmdColorBase];
    workspace_->UpdateGroup(updated);
  } else if (cmd >= kCmdFolderSpaceBase && cmd < kCmdFolderSpaceBase + workspaces.size()) {
    MoveFolderToSpace(group_id, workspaces[cmd - kCmdFolderSpaceBase]);
  } else if (cmd == kCmdToggleGroup) {
    ToggleGroup(group_id);
  } else if (cmd == kCmdRenameGroup) {
    std::wstring name = updated.name;
    if (PromptText(L"Rename Folder", L"Folder name:", name) && !name.empty()) {
      updated.name = name;
      workspace_->UpdateGroup(updated);
      ShowToast({L"folder-renamed", L"Folder has been successfully renamed!", L"", L"", 3000});
    }
  } else if (cmd == kCmdFolderIcon) {
    std::wstring icon = updated.icon;
    if (PromptText(L"Change Icon", L"An emoji for this folder (empty for the folder icon):", icon)) {
      updated.icon = FirstGrapheme(icon);
      workspace_->UpdateGroup(updated);
    }
  } else if (cmd == kCmdFolderUnload) {
    UnloadFolder(group_id);
  } else if (cmd == kCmdSubfolder) {
    NewSubfolder(group_id);
  } else if (cmd == kCmdFolderToSpace) {
    FolderToSpace(group_id);
  } else if (cmd == kCmdUngroup) {
    UnpackFolder(group_id);
  } else if (cmd == kCmdCloseGroup) {
    DeleteFolder(group_id);
  }
  Layout();
}

namespace {

constexpr int64_t kMinuteMs = 60 * 1000;
constexpr int64_t kHourMs = 60 * kMinuteMs;
constexpr int64_t kLiveIntervals[] = {15 * kMinuteMs, 30 * kMinuteMs, kHourMs,
                                      2 * kHourMs,    4 * kHourMs,    8 * kHourMs};
constexpr int kLiveItemLimits[] = {5, 10, 25, 50};
constexpr int64_t kLiveTimeRanges[] = {0, kHourMs, 6 * kHourMs, 12 * kHourMs, 24 * kHourMs,
                                       72 * kHourMs};

std::wstring IntervalLabel(int64_t ms) {
  if (ms < kHourMs) return std::to_wstring(ms / kMinuteMs) + L" minutes";
  const int64_t h = ms / kHourMs;
  return h == 1 ? L"1 hour" : std::to_wstring(h) + L" hours";
}

std::wstring TimeRangeLabel(int64_t ms) {
  if (!ms) return L"All time";
  if (ms == kHourMs) return L"Last hour";
  if (ms < 48 * kHourMs) return L"Last " + std::to_wstring(ms / kHourMs) + L" hours";
  return L"Last " + std::to_wstring(ms / (24 * kHourMs)) + L" days";
}

std::vector<std::wstring> LiveRepoList(const LiveFolderState& s) {
  std::set<std::wstring> all = s.repos;
  all.insert(s.repo_excludes.begin(), s.repo_excludes.end());
  return {all.begin(), all.end()};  // sorted (localeCompare, roughly)
}

int64_t NowUnixMs() {
  FILETIME ft;
  ::GetSystemTimeAsFileTime(&ft);
  const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
  return static_cast<int64_t>(t / 10000 - 11644473600000ULL);
}

}  // namespace

void BrowserWindow::AppendLiveFolderMenu(HMENU menu, int64_t group_id) {
  // ZenLiveFoldersUI.buildContextMenu: "Live Folder Options".
  const auto* e = Application::Get()->live_folders().Find(group_id);
  if (!e) return;
  const LiveFolderState& s = e->state;
  HMENU options = ::CreatePopupMenu();
  const std::wstring status = !s.last_error.empty()
                                  ? LiveErrorLabel(s.last_error)
                                  : L"Last fetch: " + FormatTimeAgo(s.last_fetched, NowUnixMs());
  ::AppendMenuW(options, MF_STRING | MF_GRAYED, 0, status.c_str());
  HMENU intervals = ::CreatePopupMenu();
  for (size_t i = 0; i < std::size(kLiveIntervals); ++i) {
    const UINT checked = s.interval_ms == kLiveIntervals[i] ? MF_CHECKED : 0;
    ::AppendMenuW(intervals, MF_STRING | checked, kCmdLiveIntervalBase + i,
                  IntervalLabel(kLiveIntervals[i]).c_str());
  }
  ::AppendMenuW(options, MF_POPUP, reinterpret_cast<UINT_PTR>(intervals), L"Fetch Interval");
  ::AppendMenuW(options, MF_STRING | (e->fetching ? MF_GRAYED : 0), kCmdLiveRefresh, L"Refresh");
  ::AppendMenuW(options, MF_SEPARATOR, 0, nullptr);
  if (s.kind == LiveFolderState::Kind::kRss) {
    ::AppendMenuW(options, MF_STRING, kCmdLiveFeedUrl, L"Feed URL");
    HMENU limits = ::CreatePopupMenu();
    for (size_t i = 0; i < std::size(kLiveItemLimits); ++i) {
      const UINT checked = s.max_items == kLiveItemLimits[i] ? MF_CHECKED : 0;
      ::AppendMenuW(limits, MF_STRING | checked, kCmdLiveItemLimitBase + i,
                    (std::to_wstring(kLiveItemLimits[i]) + L" items").c_str());
    }
    ::AppendMenuW(options, MF_POPUP, reinterpret_cast<UINT_PTR>(limits), L"Item Limit");
    HMENU ranges = ::CreatePopupMenu();
    for (size_t i = 0; i < std::size(kLiveTimeRanges); ++i) {
      const UINT checked = s.time_range_ms == kLiveTimeRanges[i] ? MF_CHECKED : 0;
      ::AppendMenuW(ranges, MF_STRING | checked, kCmdLiveTimeRangeBase + i,
                    TimeRangeLabel(kLiveTimeRanges[i]).c_str());
      if (i == 0) ::AppendMenuW(ranges, MF_SEPARATOR, 0, nullptr);
    }
    ::AppendMenuW(options, MF_POPUP, reinterpret_cast<UINT_PTR>(ranges), L"Time Range");
  } else {
    const bool pulls = s.kind == LiveFolderState::Kind::kGithubPulls;
    ::AppendMenuW(options, MF_STRING | (s.author_me ? MF_CHECKED : 0), kCmdLiveAuthorMe, L"Created by Me");
    ::AppendMenuW(options, MF_STRING | (s.assigned_me ? MF_CHECKED : 0), kCmdLiveAssignedMe, L"Assigned to Me");
    if (pulls) {
      ::AppendMenuW(options, MF_STRING | (s.review_requested ? MF_CHECKED : 0), kCmdLiveReviewRequested,
                    L"Review Requests");
      ::AppendMenuW(options, MF_STRING | (s.include_drafts ? MF_CHECKED : 0), kCmdLiveIncludeDrafts,
                    L"Include Draft Pull Requests");
    }
    ::AppendMenuW(options, MF_SEPARATOR, 0, nullptr);
    HMENU repos = ::CreatePopupMenu();
    const auto list = LiveRepoList(s);
    for (size_t i = 0; i < list.size() && kCmdLiveRepoBase + i < kCmdLiveRepoEnd; ++i) {
      const UINT checked = s.repo_excludes.count(list[i]) ? 0 : MF_CHECKED;
      ::AppendMenuW(repos, MF_STRING | checked, kCmdLiveRepoBase + i, list[i].c_str());
    }
    if (!list.empty()) ::AppendMenuW(repos, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(repos, MF_STRING | MF_GRAYED, 0,
                  L"This list is generated based on your currently active pull requests.");
    ::AppendMenuW(options, MF_POPUP | (list.empty() ? MF_GRAYED : 0), reinterpret_cast<UINT_PTR>(repos),
                  L"Repositories");
  }
  ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(options), L"Live Folder Options");
}

void BrowserWindow::RunLiveFolderCommand(int64_t group_id, UINT cmd) {
  // nsZenLiveFolderProvider.onOptionTrigger and the providers' overrides.
  LiveFolders& live = Application::Get()->live_folders();
  const auto* e = live.Find(group_id);
  if (!e) return;
  if (cmd == kCmdLiveRefresh) {
    live.Refresh(group_id);
  } else if (cmd == kCmdLiveFeedUrl) {
    std::wstring url = e->state.url;
    if (!PromptText(L"Feed URL", L"Please enter the feed URL", url)) return;
    url = std::wstring(TrimWhitespace(url));
    if (!StartsWithNoCase(url, L"http://") && !StartsWithNoCase(url, L"https://")) {
      ShowToast({L"live-folder-invalid", L"Failed to create the Live Folder",
                 L"The feed URL is invalid. Check the address and try again", L"", 6000});
      return;
    }
    live.Update(group_id, [&](LiveFolderState& s) { s.url = url; });
  } else if (cmd >= kCmdLiveIntervalBase && cmd < kCmdLiveIntervalBase + std::size(kLiveIntervals)) {
    const int64_t ms = kLiveIntervals[cmd - kCmdLiveIntervalBase];
    live.Update(group_id, [ms](LiveFolderState& s) { s.interval_ms = ms; });
  } else if (cmd >= kCmdLiveItemLimitBase && cmd < kCmdLiveItemLimitBase + std::size(kLiveItemLimits)) {
    const int n = kLiveItemLimits[cmd - kCmdLiveItemLimitBase];
    live.Update(group_id, [n](LiveFolderState& s) { s.max_items = n; });
  } else if (cmd >= kCmdLiveTimeRangeBase && cmd < kCmdLiveTimeRangeBase + std::size(kLiveTimeRanges)) {
    const int64_t ms = kLiveTimeRanges[cmd - kCmdLiveTimeRangeBase];
    live.Update(group_id, [ms](LiveFolderState& s) { s.time_range_ms = ms; });
  } else if (cmd == kCmdLiveAuthorMe) {
    live.Update(group_id, [](LiveFolderState& s) { s.author_me = !s.author_me; });
  } else if (cmd == kCmdLiveAssignedMe) {
    live.Update(group_id, [](LiveFolderState& s) { s.assigned_me = !s.assigned_me; });
  } else if (cmd == kCmdLiveReviewRequested) {
    live.Update(group_id, [](LiveFolderState& s) { s.review_requested = !s.review_requested; });
  } else if (cmd == kCmdLiveIncludeDrafts) {
    live.Update(group_id, [](LiveFolderState& s) { s.include_drafts = !s.include_drafts; });
  } else if (cmd >= kCmdLiveRepoBase && cmd < kCmdLiveRepoEnd) {
    const auto list = LiveRepoList(e->state);
    const size_t i = cmd - kCmdLiveRepoBase;
    if (i >= list.size()) return;
    const std::wstring repo = list[i];
    live.Update(group_id, [&repo](LiveFolderState& s) {
      if (!s.repo_excludes.erase(repo)) s.repo_excludes.insert(repo);
    });
  }
  Invalidate();
}

void BrowserWindow::LiveErrorAction(int64_t group_id) {
  // onActionButtonClick: retry, or sign in to GitHub.
  const auto* e = Application::Get()->live_folders().Find(group_id);
  if (!e) return;
  if (e->state.last_error == kLiveGithubNoAuth) {
    OpenTab(L"https://github.com/login", /*activate=*/true);
  } else {
    Application::Get()->live_folders().Refresh(group_id);
  }
}

D2D1_RECT_F BrowserWindow::LiveErrorRect(const Row& row) const {
  if (!row.is_group || collapsed_ || !workspace_) return {};
  const auto* e = Application::Get()->live_folders().Find(row.group_id);
  if (!e || e->state.last_error.empty()) return {};
  // Sized to its label: a warning glyph, the text and some padding.
  const std::wstring label = LiveErrorLabel(e->state.last_error);
  float text = 7.f * static_cast<float>(label.size());
  Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
  auto& g = ui::Graphics::Get();
  if (SUCCEEDED(g.dwrite()->CreateTextLayout(label.c_str(), static_cast<UINT32>(label.size()),
                                             g.text_small(), 400.f, 40.f, &layout))) {
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    text = m.width;
  }
  const D2D1_RECT_F r = RowRect(row);
  // Too narrow for the name and the label: only the warning button (the
  // reason is its tooltip, as on Zen's icon button).
  const float width = r.right - r.left - 44.f - (text + 34.f) >= 90.f ? text + 34.f : 24.f;
  return {r.right - 4.f - width, r.top + 4.f, r.right - 4.f, r.bottom - 4.f};
}

void BrowserWindow::NewLiveFolder(LiveFolderState::Kind kind) {
  if (!workspace_ || profile_->is_private()) return;
  std::wstring url;
  if (kind == LiveFolderState::Kind::kRss) {
    // promptForFeedUrl.
    if (!PromptText(L"New Live Folder", L"Please enter the feed URL", url)) return;
    url = std::wstring(TrimWhitespace(url));
    const size_t host = url.find(L"://");
    if ((!StartsWithNoCase(url, L"http://") && !StartsWithNoCase(url, L"https://")) ||
        host == std::wstring::npos || host + 3 >= url.size()) {
      ShowToast({L"live-folder-invalid", L"Failed to create the Live Folder",
                 L"The feed URL is invalid. Check the address and try again", L"", 6000});
      return;
    }
  }
  BrowserWindow* self = this;
  Application::Get()->live_folders().Create(workspace_, kind, url, [self](int64_t id) {
    // #maybeShowPromotion: once.
    Application* app = Application::Get();
    const bool alive = std::any_of(app->windows().begin(), app->windows().end(),
                                   [self](const auto& w) { return w.get() == self && !w->closing(); });
    if (!id || !alive || app->store().GetPref(Store::kGlobalScope, L"live_folders_promo_shown")) return;
    app->store().SetPref(Store::kGlobalScope, L"live_folders_promo_shown", L"1");
    self->ShowToast({L"live-folder-created", L"Live Folder Created!",
                     L"Latest content from your RSS feeds or GitHub pull requests will appear here "
                     L"automatically.",
                     L"", 8000});
  });
}

void BrowserWindow::AnimateDownload() {
  // #handleNewDownload: only in the active window, from the last click (the
  // pointer, when it is over this window).
  if (::GetForegroundWindow() != hwnd_) return;
  POINT p;
  RECT wr;
  if (!::GetCursorPos(&p) || !::GetWindowRect(hwnd_, &wr) || !::PtInRect(&wr, p)) return;
  AnimateDownloadFrom(p);
}

void BrowserWindow::AnimateDownloadFrom(POINT start) {
  if (!PrefEnabled(L"download_animation", true) || ui::AnimMs(1000) == 0) return;
  if (!download_flight_) download_flight_ = std::make_unique<ui::DownloadFlight>(hwnd_);
  // #determineEndPosition: the library button, or the box at the corner.
  const bool button = sidebar_visible() && downloads_.right > downloads_.left;
  POINT end;
  if (button) {
    const float scale = static_cast<float>(::GetDpiForWindow(hwnd_)) / 96.f;
    end = {static_cast<LONG>((downloads_.left + downloads_.right) / 2.f * scale),
           static_cast<LONG>((downloads_.top + downloads_.bottom) / 2.f * scale)};
    ::ClientToScreen(hwnd_, &end);
  } else {
    end = ui::DownloadFlight::BoxTarget(hwnd_, right_side_);
  }
  ui::DownloadFlight::Style style;
  style.surface = ui::Mix(palette_.chrome_bg, palette_.text, 0.12f);
  style.ink = palette_.text;
  style.accent = palette_.accent;
  style.dark = palette_.dark;
  download_flight_->Fly(start, end, !button, right_side_, style, 1000);
}

void BrowserWindow::SetTabPinned(Tab* tab, bool pinned) {
  if (tab && tab->pinned() != pinned) TogglePin(tab);
}

void BrowserWindow::MoveTabToFolderAnimated(Tab* tab, int64_t folder) {
  if (!workspace_ || workspace_->IndexOf(tab) < 0) return;
  CaptureRows();
  workspace_->SetTabGroup(tab, folder);
  DropFolderPlaceholder(folder);
  Layout();
  AnimateRows();
}

void BrowserWindow::SetSpaceTheme(Workspace* ws, const std::optional<orbit::SpaceTheme>& theme, bool save) {
  // nsZenThemePicker.updateCurrentWorkspace: live while editing, stored
  // when |save| (the panel closes / a drag ends).
  if (!ws || profile_->is_private()) return;
  ws->mutable_record().theme = theme ? orbit::SerializeSpaceTheme(*theme) : std::wstring();
  if (theme) {
    for (const orbit::ThemeDot& d : theme->dots) {
      if (d.primary) ws->mutable_record().color = (static_cast<uint32_t>(d.r) << 16) | (d.g << 8) | d.b;
    }
  }
  if (save) Application::Get()->store().UpdateWorkspace(ws->record());
  for (const auto& w : Application::Get()->windows()) {
    if (w->workspace_ == ws) w->RefreshBackdrop();
  }
}

void BrowserWindow::DropFolderPlaceholder(int64_t group_id) {
  // A real tab arrived: the folder's empty tab is no longer needed (a live
  // folder keeps it: its items come and go).
  if (!group_id || !workspace_ || Application::Get()->live_folders().IsLive(group_id)) return;
  std::vector<Tab*> empties;
  bool real = false;
  for (const auto& t : workspace_->tabs()) {
    if (t->record().group_id != group_id) continue;
    if (t->empty_placeholder()) {
      empties.push_back(t.get());
    } else {
      real = true;
    }
  }
  if (!real) return;
  for (Tab* t : empties) Application::Get()->RemoveTab(t, false);
}

void BrowserWindow::NewSubfolder(int64_t parent_id) {
  // createSubfolder: open every folder above it, then a new "Subfolder".
  if (!workspace_) return;
  for (int64_t id = parent_id; id;) {
    const TabGroupRecord* g = workspace_->FindGroup(id);
    if (!g) break;
    if (g->collapsed) {
      TabGroupRecord open = *g;
      open.collapsed = false;
      workspace_->UpdateGroup(open);
    }
    id = g->parent_id;
  }
  std::wstring name = L"Subfolder";
  if (!PromptText(L"New Subfolder", L"Folder name:", name) || name.empty()) return;
  CaptureRows();
  workspace_->CreateSubfolder(parent_id, name, nullptr);
  Layout();
  AnimateRows();
}

void BrowserWindow::UnloadFolder(int64_t group_id) {
  // zen-folders-unload-folder: every loaded tab in it (the one on screen stays).
  for (Tab* t : workspace_->GroupTabs(group_id)) {
    if (t != active_tab() && t->has_browser()) t->Unload(/*mark_discarded=*/true);
  }
  Invalidate();
}

void BrowserWindow::MoveFolderToSpace(int64_t group_id, Workspace* target) {
  // context_zenChangeFolderSpace: the folder (and its subfolders) moves (to a
  // space of another profile too: its tabs load again there).
  if (!target || target == workspace_ || target->is_linked() || !CanShareTabs(profile_, target->profile())) {
    return;
  }
  const TabGroupRecord* root = workspace_->FindGroup(group_id);
  if (!root) return;
  // Recreate the folder tree in the target space, outermost first.
  std::map<int64_t, int64_t> ids;  // old -> new
  std::vector<int64_t> pending{group_id};
  std::vector<TabGroupRecord> tree;
  while (!pending.empty()) {
    const int64_t id = pending.front();
    pending.erase(pending.begin());
    if (const TabGroupRecord* g = workspace_->FindGroup(id)) {
      tree.push_back(*g);
      for (const auto& child : workspace_->groups()) {
        if (child.parent_id == id) pending.push_back(child.id);
      }
    }
  }
  for (const TabGroupRecord& g : tree) {
    const int64_t parent = g.id == group_id ? 0 : ids[g.parent_id];
    const int64_t made = target->CreateGroup(g.name, g.color, {}, parent);
    if (!made) return;
    TabGroupRecord copy = *target->FindGroup(made);
    copy.icon = g.icon;
    copy.collapsed = g.collapsed;
    target->UpdateGroup(copy);
    ids[g.id] = made;
  }
  const auto tabs = workspace_->GroupTabs(group_id);
  std::vector<std::pair<Tab*, int64_t>> moved;
  for (Tab* t : tabs) moved.push_back({t, ids[t->record().group_id]});
  for (auto& [t, folder] : moved) {
    MoveTabToWorkspace(t, target);
    target->SetTabGroup(t, folder);
  }
  target->PruneEmptyGroups();
  Layout();
}

void BrowserWindow::FolderToSpace(int64_t group_id) {
  // #convertFolderToSpace: a new space named after the folder; its tabs
  // become the space's pinned tabs; the folder goes away.
  if (profile_->is_private()) return;
  const TabGroupRecord* g = workspace_->FindGroup(group_id);
  if (!g) return;
  const std::wstring name = g->name;
  const std::wstring icon = g->icon;
  const uint32_t color = g->color;
  Workspace* space = Application::Get()->CreateWorkspace(profile_, name, color);
  if (!space) return;
  space->mutable_record().icon = icon;
  Application::Get()->store().UpdateWorkspace(space->record());
  std::vector<Tab*> tabs;
  for (Tab* t : workspace_->GroupTabs(group_id)) {
    if (t->empty_placeholder()) {
      Application::Get()->RemoveTab(t, false);
    } else {
      tabs.push_back(t);
    }
  }
  for (Tab* t : tabs) {
    MoveTabToWorkspace(t, space);
    space->SetTabGroup(t, 0);
    t->mutable_record().pinned = true;
    if (t->pinned_url().empty()) t->SetPinnedUrl(t->url());
    t->MarkDirty();
  }
  for (Tab* t : tabs) space->MoveTab(t, space->PinnedCount());
  workspace_->PruneEmptyGroups();
  SwitchWorkspace(space);
  Layout();
}

void BrowserWindow::UnpackFolder(int64_t group_id) {
  // context_zenFolderUnpack: its tabs and subfolders move up a level; its
  // empty tab goes away.
  CaptureRows();
  for (Tab* t : workspace_->GroupTabs(group_id)) {
    if (t->empty_placeholder() && t->record().group_id == group_id) Application::Get()->RemoveTab(t, false);
  }
  workspace_->Ungroup(group_id);
  Layout();
  AnimateRows();
}

void BrowserWindow::DeleteFolder(int64_t group_id) {
  // context_zenFolderDelete: the folder and everything in it.
  std::vector<Tab*> victims = workspace_->GroupTabs(group_id);
  std::vector<Tab*> real;
  for (Tab* t : victims) {
    if (t->empty_placeholder()) {
      Application::Get()->RemoveTab(t, false);
    } else {
      real.push_back(t);
    }
  }
  Application::Get()->CloseTabsTogether(real);
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
  ::AppendMenuW(menu, MF_STRING, kCmdNewPrivateWindow, MenuLabel(L"New private window", L"private-window").c_str());

  const UINT cmd = static_cast<UINT>(ui::TrackMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, hwnd_, palette_));
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
          L"\"?\n\nIts spaces and their tabs, and its cookies, site data, history and "
          L"bookmarks will be permanently deleted.";
      if (::MessageBoxW(hwnd_, msg.c_str(), L"Delete profile", MB_OKCANCEL | MB_ICONWARNING) ==
          IDOK) {
        app->DeleteProfile(profile_);  // this window moves to a space of another profile
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
  ::AppendMenuW(menu, MF_STRING, kCmdNewTab, MenuLabel(L"New tab", L"new-tab").c_str());
  ::AppendMenuW(menu, MF_STRING, kCmdNewWindow, MenuLabel(L"New window", L"new-window").c_str());
  ::AppendMenuW(menu, MF_STRING, kCmdNewPrivateWindow, MenuLabel(L"New private window", L"private-window").c_str());
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kCmdBookmarks, MenuLabel(L"Bookmarks", L"bookmarks").c_str());
  ::AppendMenuW(menu, MF_STRING, kCmdHistory, MenuLabel(L"History", L"history").c_str());
  ::AppendMenuW(menu, MF_STRING, kCmdDownloads, MenuLabel(L"Downloads", L"downloads").c_str());
  if (!profile_->is_private()) {
    ::AppendMenuW(menu, MF_STRING, kCmdExtensions, MenuLabel(L"Extensions", L"extensions").c_str());
  }
  if (!installer::IsDefaultBrowser()) {
    ::AppendMenuW(menu, MF_STRING, kCmdDefaultBrowser, L"Make Orbit the default browser");
  }
  std::wstring profiles = L"Profile: " + profile_->record().name + L"…";
  ::AppendMenuW(menu, MF_STRING, kCmdProfiles, profiles.c_str());
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kCmdZoomIn, MenuLabel(L"Zoom in", L"zoom-in").c_str());
  ::AppendMenuW(menu, MF_STRING, kCmdZoomOut, MenuLabel(L"Zoom out", L"zoom-out").c_str());
  ::AppendMenuW(menu, MF_STRING, kCmdZoomReset, MenuLabel(L"Reset zoom", L"zoom-reset").c_str());
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  if (multi_toolbar()) {
    // zen-context-menu-compact-mode: enable | hide sidebar / toolbar / both.
    HMENU compact = ::CreatePopupMenu();
    ::AppendMenuW(compact, MF_STRING | (compact_mode_ ? MF_CHECKED : 0), kCmdCompactMode,
                  MenuLabel(L"Enable compact mode", L"compact-mode").c_str());
    ::AppendMenuW(compact, MF_SEPARATOR, 0, nullptr);
    const bool sb = compact_hide_sidebar_, tb = compact_hide_toolbar_;
    ::AppendMenuW(compact, MF_STRING | (sb && !tb ? MF_CHECKED : 0), kCmdCompactHideSidebar, L"Hide sidebar");
    ::AppendMenuW(compact, MF_STRING | (tb && !sb ? MF_CHECKED : 0), kCmdCompactHideToolbar, L"Hide toolbar");
    ::AppendMenuW(compact, MF_STRING | (sb && tb ? MF_CHECKED : 0), kCmdCompactHideBoth, L"Hide both");
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(compact), L"Compact mode");
  } else {
    ::AppendMenuW(menu, MF_STRING | (compact_mode_ ? MF_CHECKED : 0), kCmdCompactMode,
                  MenuLabel(L"Compact mode", L"compact-mode").c_str());
  }
  ::AppendMenuW(menu, MF_STRING | (collapsed_ ? MF_CHECKED : 0), kCmdToggleSidebar,
                MenuLabel(L"Collapse sidebar", L"collapse-sidebar").c_str());
  ::AppendMenuW(menu, MF_STRING | (right_side_ ? MF_CHECKED : 0), kCmdTabsRight, L"Tabs on the right");
  ::AppendMenuW(menu, MF_STRING | (tab && tab->browser() ? 0 : MF_GRAYED), kCmdDevTools,
                MenuLabel(L"Developer tools", L"devtools").c_str());
  ::AppendMenuW(menu, MF_STRING, kCmdSettings, L"Settings");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit");

  const UINT cmd = static_cast<UINT>(ui::TrackMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, hwnd_, palette_));
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
    case kCmdExtensions:
      OpenInternalPage(kPageExtensions);
      break;
    case kCmdDefaultBrowser:
      installer::MakeDefaultBrowser();
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
    case kCmdCompactHideSidebar:
      SetCompactHides(true, false);
      break;
    case kCmdCompactHideToolbar:
      SetCompactHides(false, true);
      break;
    case kCmdCompactHideBoth:
      SetCompactHides(true, true);
      break;
    case kCmdTabsRight:
      ToggleTabsOnRight();
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
  // Zen's "Create New" popup (zen-panels/popups.inc #zenCreateNewPopup):
  // Create Space, Create Folder, New Split, New Tab.
  HMENU menu = ::CreatePopupMenu();
  const UINT disabled = profile_->is_private() ? MF_GRAYED : 0;
  ::AppendMenuW(menu, MF_STRING | disabled, kCmdNewWorkspace, L"Create Space");
  ::AppendMenuW(menu, MF_STRING | (active_tab() && !active_tab()->pinned() ? 0 : MF_GRAYED),
                kCmdNewTabGroup, L"Create Folder");
  // zen-panel-ui-live-folder-create (the tab strip's context menu in Zen).
  HMENU live = ::CreatePopupMenu();
  ::AppendMenuW(live, MF_STRING, kCmdNewLivePulls, L"Pull Requests");
  ::AppendMenuW(live, MF_STRING, kCmdNewLiveIssues, L"Issues");
  ::AppendMenuW(live, MF_STRING, kCmdNewLiveRss, L"RSS Feed");
  ::AppendMenuW(menu, MF_POPUP | disabled, reinterpret_cast<UINT_PTR>(live), L"New Live Folder");
  ::AppendMenuW(menu, MF_STRING | (active_tab() && !active_tab()->pinned() && !active_essential_ ? 0 : MF_GRAYED),
                kCmdNewSplit, L"New Split");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kCmdNewTab, L"New Tab\tCtrl+T");
  const UINT cmd = static_cast<UINT>(ui::TrackMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_BOTTOMALIGN, pt.x, pt.y, hwnd_, palette_));
  ::DestroyMenu(menu);
  switch (cmd) {
    case kCmdNewTab:
      NewTab();
      break;
    case kCmdNewTabGroup:
      if (Tab* tab = active_tab()) NewGroupWithTab(tab);
      break;
    case kCmdNewSplit:
      NewEmptySplit();
      break;
    case kCmdNewWorkspace:
      NewWorkspace();
      break;
    case kCmdNewLivePulls:
      NewLiveFolder(LiveFolderState::Kind::kGithubPulls);
      break;
    case kCmdNewLiveIssues:
      NewLiveFolder(LiveFolderState::Kind::kGithubIssues);
      break;
    case kCmdNewLiveRss:
      NewLiveFolder(LiveFolderState::Kind::kRss);
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
    case HitKind::kCopyUrl: r = copy_url_; break;
    case HitKind::kExtensions: r = ext_btn_; break;
    case HitKind::kExtensionPin:
      if (hit.index >= 0 && hit.index < static_cast<int>(ext_pin_rects_.size())) r = ext_pin_rects_[hit.index];
      break;
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
    case HitKind::kTabClose:
    case HitKind::kTabResetPin: r = TabRowRect(hit.index); break;
    case HitKind::kEssential: {
      const auto rects = EssentialRects();
      if (static_cast<size_t>(hit.index) < rects.size()) r = rects[static_cast<size_t>(hit.index)];
      break;
    }
    case HitKind::kGroup: {
      for (const Row& row : Rows()) {
        if (row.is_group && GroupIndex(row.group_id) == hit.index) r = RowRect(row);
      }
      break;
    }
    case HitKind::kLiveError: {
      for (const Row& row : Rows()) {
        if (row.is_group && GroupIndex(row.group_id) == hit.index) r = LiveErrorRect(row);
      }
      break;
    }
    case HitKind::kLinked: {
      for (const Row& row : Rows()) {
        if (row.linked && LinkedIndex(row.linked) == hit.index) r = RowRect(row);
      }
      break;
    }
    case HitKind::kLinkedBack: r = linked_back_; break;
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
    case HitKind::kBack: return BackClosesTab() ? L"Close tab (Alt+Left)" : L"Back (Alt+Left)";
    case HitKind::kForward: return L"Forward (Alt+Right)";
    case HitKind::kReload: {
      Tab* tab = active_tab();
      return tab && tab->loading() ? L"Stop loading" : L"Reload (Ctrl+R)";
    }
    case HitKind::kSiteInfo: return L"View site information";
    case HitKind::kStar: return bookmarked_ ? L"Remove bookmark (Ctrl+D)" : L"Bookmark this page (Ctrl+D)";
    case HitKind::kCopyUrl: return L"Copy URL (Ctrl+Shift+C)";
    case HitKind::kExtensions: return L"Extensions";
    case HitKind::kExtensionPin: {
      const ExtensionInfo* e = PinnedExtension(tooltip_hit_.index);
      return e ? FromUtf8(e->action_title.empty() ? e->name : e->action_title) : std::wstring();
    }
    case HitKind::kMenu: return L"Menu";
    case HitKind::kSidebarToggle:
      return compact_mode_ ? L"Turn off compact mode (Ctrl+S)"
                           : L"Compact mode (Ctrl+S)";
    case HitKind::kWorkspaceIndicator:
      if (!collapsed_ && workspace_ && workspace_->PinnedCount() > 0) {
        return PinnedSectionCollapsed() ? L"Show pinned tabs" : L"Hide pinned tabs";
      }
      return workspace_ ? L"Space: " + workspace_->record().name : L"";
    case HitKind::kWorkspaceActions:
      return workspace_ ? L"Space: " + workspace_->record().name : L"";
    case HitKind::kWorkspace: {
      if (collapsed_) return workspace_ ? L"Space: " + workspace_->record().name : L"";
      const auto ws = Spaces();
      const size_t i = static_cast<size_t>(tooltip_hit_.index);
      return i < ws.size() ? SpaceLabel(ws[i]) : L"";
    }
    case HitKind::kCreateNew: return L"Create New";
    case HitKind::kDownloads: {
      const int active = Application::Get()->downloads().ActiveCount(profile_);
      if (active > 0) {
        const int pct = Application::Get()->downloads().ActivePercent(profile_);
        std::wstring text = std::to_wstring(active) + L" download(s) in progress";
        if (pct >= 0) text += L" — " + std::to_wstring(pct) + L"%";
        return text;
      }
      return L"Library (Alt+Shift+L)";
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
    case HitKind::kTabClose: {
      if (!workspace_) return L"";
      const size_t i = static_cast<size_t>(tooltip_hit_.index);
      if (i < workspace_->tabs().size() && workspace_->tabs()[i]->pinned()) {
        return L"Unload and switch to tab";
      }
      return L"Close tab (Ctrl+W)";
    }
    case HitKind::kEssential: {
      const Workspace* ess = essentials();
      const size_t i = static_cast<size_t>(tooltip_hit_.index);
      return ess && i < ess->tabs().size() ? ess->tabs()[i]->DisplayTitle() : L"";
    }
    case HitKind::kTabResetPin:
      return ::GetKeyState(VK_CONTROL) < 0 ? L"Separate from pinned tab"
                                           : L"Back to pinned url";
    case HitKind::kLiveError: {
      if (!workspace_ || tooltip_hit_.index < 0) return L"";
      const auto* e = Application::Get()->live_folders().Find(
          workspace_->groups()[static_cast<size_t>(tooltip_hit_.index)].id);
      return e ? LiveErrorTooltip(e->state.last_error) : L"";
    }
    case HitKind::kLinked: {
      const auto children = LinkedChildren();
      const size_t i = static_cast<size_t>(tooltip_hit_.index);
      if (i >= children.size()) return L"";
      int n = 0;
      for (const auto& t : children[i]->tabs()) n += !t->empty_placeholder();
      return children[i]->record().name + L" \u2014 " + std::to_wstring(n) +
             L" tab(s) in a linked window. Click to show it.";
    }
    case HitKind::kLinkedBack:
      return workspace_ && workspace_->parent()
                 ? L"Bring the tabs back to " + workspace_->parent()->record().name + L" and close this window"
                 : L"";
    case HitKind::kGroup: {
      if (!workspace_ || tooltip_hit_.index < 0) return L"";
      const auto& g = workspace_->groups()[static_cast<size_t>(tooltip_hit_.index)];
      return g.name + L" — " + std::to_wstring(workspace_->GroupSize(g.id)) +
             L" tab(s). Click to " + (g.collapsed ? L"expand" : L"collapse") + L".";
    }
    default: return L"";
  }
}

void BrowserWindow::SetCompactHides(bool sidebar, bool toolbar) {
  // hideSidebar / hideToolbar / hideBoth: also turns compact mode on.
  Store& store = Application::Get()->store();
  store.SetPref(Store::kGlobalScope, L"compact_hide_sidebar", sidebar ? L"1" : L"0");
  store.SetPref(Store::kGlobalScope, L"compact_hide_toolbar", toolbar ? L"1" : L"0");
  Application::Get()->ApplyThemeToAllWindows();
  if (!compact_mode_) ToggleCompactMode();
  SetSidebarRevealed(false);
  SetCaptionRevealed(false);
  Layout();
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

bool BrowserWindow::BackClosesTab() const {
  // ZenCommonUtils.shouldCloseTabOnBack: only tabs opened from another tab,
  // not pinned, and not the empty new-tab page.
  Tab* tab = active_tab();
  return tab && !tab->can_go_back() && tab->owner() && !tab->pinned() &&
         !IsNewTabUrl(tab->url()) && PrefEnabled(L"close_on_back_no_history", true);
}

void BrowserWindow::GoBack() {
  Tab* tab = active_tab();
  if (!tab) return;
  if (tab->can_go_back()) {
    tab->GoBack();
  } else if (BackClosesTab()) {
    CloseTab(tab);  // selection falls back to the owner
  }
}

void BrowserWindow::CloseTabShortcut(Tab* tab) {
  if (!tab) return;
  if (tab->pinned()) {
    PinnedCloseShortcut(tab, /*close_if_unloaded=*/false);  // _initClosePinnedTabShortcut
  } else {
    CloseTab(tab);
  }
}

void BrowserWindow::PinnedCloseShortcut(Tab* tab, bool close_if_unloaded) {
  // ZenPinnedTabManager.onCloseTabShortcut with
  // zen.pinned-tab-manager.close-shortcut-behavior (default reset-unload-switch).
  if (!tab || !tab->pinned() ||
      (workspace_->IndexOf(tab) < 0 && !IsEssential(tab))) {
    return;
  }
  std::wstring behavior = L"reset-unload-switch";
  if (auto pref = Application::Get()->store().GetPref(Store::kGlobalScope, L"pinned_close_behavior")) {
    behavior = *pref;
  }
  if (behavior == L"close") {
    CloseTab(tab);
    return;
  }
  const bool unload = behavior.find(L"unload") != std::wstring::npos;
  const bool reset = behavior.find(L"reset") != std::wstring::npos;
  const bool do_switch = behavior.find(L"switch") != std::wstring::npos;
  if (unload && close_if_unloaded && !tab->has_browser() &&
      tab->load_state() == Tab::LoadState::kUnloaded) {
    CloseTab(tab);  // already unloaded: the middle click closes it
    return;
  }
  if (do_switch && tab == active_tab()) {
    if (Tab* next = TabToSelectOnClose(tab)) ActivateTab(next);
  }
  // Unload first: resetting a tab that is already closing only rewrites its
  // URL, while resetting a live tab would recreate its browser.
  if (unload) tab->Unload(/*mark_discarded=*/false);
  if (reset) tab->ResetToPinned();
  Layout();
}

void BrowserWindow::ResetPinnedTab(Tab* tab, bool separate_current_page) {
  if (!tab || !tab->pinned()) return;
  // Accel+click on the reset button keeps the current page in its own tab.
  if (separate_current_page && tab->pin_changed()) OpenTabNear(tab, tab->url(), false);
  tab->ResetToPinned();
  ActivateTab(tab);
  Layout();
}

void BrowserWindow::ReplacePinnedUrlWithCurrent(Tab* tab) {
  if (!tab || !tab->pinned()) return;
  tab->SetPinnedUrl(tab->url());
  ShowToast({L"pinned-replaced", L"Pinned tab URL has been replaced with the current URL!", L"",
             L"", 2000});
  Invalidate();
}

bool BrowserWindow::SetPinnedUrlFromText(Tab* tab, const std::wstring& text) {
  if (!tab || !tab->pinned()) return false;
  const OmniboxResult result = FixupOmniboxInput(text, Application::Get()->search_template());
  if (result.kind != OmniboxResult::Kind::kUrl) {
    ShowToast({L"pinned-url-invalid", L"That doesn't look like a valid URL.", L"", L"", 2000});
    return false;
  }
  if (result.url == tab->pinned_url()) return true;  // unchanged
  tab->SetPinnedUrl(result.url);
  ShowToast({L"pinned-url-edited", L"Pinned tab URL has been updated!", L"", L"", 2000});
  Invalidate();
  return true;
}

void BrowserWindow::StartRenameTab(Tab* tab) {
  if (!tab || collapsed_ || !sidebar_visible() || workspace_->IndexOf(tab) < 0) return;
  CancelRename();
  rename_tab_ = tab;
  EnsureTabVisible(workspace_->IndexOf(tab));
  ::SetWindowTextW(rename_edit_, tab->DisplayTitle().c_str());
  PositionRenameEditor();
  ::ShowWindow(rename_edit_, SW_SHOW);
  ::SetFocus(rename_edit_);
  ::SendMessageW(rename_edit_, EM_SETSEL, 0, -1);
  Invalidate();
}

void BrowserWindow::StartRenameWorkspace(Workspace* ws) {
  if (!ws || ws != workspace_ || collapsed_ || !sidebar_visible() || profile_->is_private()) return;
  CancelRename();
  rename_workspace_ = ws;
  ::SetWindowTextW(rename_edit_, ws->record().name.c_str());
  PositionRenameEditor();
  ::ShowWindow(rename_edit_, SW_SHOW);
  ::SetFocus(rename_edit_);
  ::SendMessageW(rename_edit_, EM_SETSEL, 0, -1);
  Invalidate();
}

void BrowserWindow::PositionRenameEditor() {
  D2D1_RECT_F r{};
  if (rename_tab_) {
    const D2D1_RECT_F row = TabRowRect(workspace_->IndexOf(rename_tab_));
    r = {row.left + 34.f + (rename_tab_->pin_changed() ? 6.f : 0.f), row.top + 6.f,
         row.right - 8.f, row.bottom - 6.f};
  } else if (rename_workspace_) {
    r = {ws_indicator_.left + 6.f, ws_indicator_.top + 6.f, ws_indicator_.right - 6.f,
         ws_indicator_.bottom - 6.f};
  }
  if (r.right <= r.left) {
    ::ShowWindow(rename_edit_, SW_HIDE);
    return;
  }
  // The row's own surface (selected tab, or the indicator's hover fill).
  const D2D1_COLOR_F under = BackdropAt((r.left + r.right) / 2.f, (r.top + r.bottom) / 2.f);
  D2D1_COLOR_F surface = rename_tab_ ? palette_.selected : palette_.hover;
  if (rename_tab_ && rename_tab_ != active_tab()) surface = palette_.hover;
  rename_bg_ = ui::Over(surface, under);
  if (rename_brush_) ::DeleteObject(rename_brush_);
  rename_brush_ = ::CreateSolidBrush(ui::ToColorRef(rename_bg_));
  ::InvalidateRect(rename_edit_, nullptr, TRUE);
  const RECT px_rect = ToHostPx(r);
  ::SetWindowPos(rename_edit_, HWND_TOP, px_rect.left, px_rect.top, px_rect.right - px_rect.left,
                 px_rect.bottom - px_rect.top, SWP_NOACTIVATE);
}

void BrowserWindow::CommitRename() {
  if (!renaming()) return;
  const int len = ::GetWindowTextLengthW(rename_edit_);
  std::wstring raw(static_cast<size_t>(len), L'\0');
  ::GetWindowTextW(rename_edit_, raw.data(), len + 1);
  // input.value.replace(/\s+/g, " ").trim()
  std::wstring name;
  bool space = false;
  for (wchar_t ch : raw) {
    if (std::iswspace(ch)) {
      space = !name.empty();
    } else {
      if (space) name += L' ';
      space = false;
      name += ch;
    }
  }
  Tab* tab = rename_tab_;
  Workspace* ws = rename_workspace_;
  rename_tab_ = nullptr;
  rename_workspace_ = nullptr;
  ::ShowWindow(rename_edit_, SW_HIDE);
  if (tab && workspace_->IndexOf(tab) >= 0) {
    const bool changed = !name.empty() && name != tab->DisplayTitle();
    if (changed || (!tab->custom_title().empty() && !name.empty())) {
      tab->SetCustomTitle(name);
      ShowToast({L"tab-renamed", L"Tab has been successfully renamed!", L"", L"", 2000});
    } else if (name.empty()) {
      tab->SetCustomTitle(L"");  // back to the page title
    }
    FocusContent();
  } else if (ws && !name.empty() && name != ws->record().name) {
    ws->mutable_record().name = name;
    Application::Get()->store().UpdateWorkspace(ws->record());
    ShowToast({L"workspace-renamed", L"Workspace has been successfully renamed!", L"", L"", 2000});
  }
  Invalidate();
}

void BrowserWindow::CancelRename() {
  if (!rename_tab_ && !rename_workspace_) return;
  rename_tab_ = nullptr;
  rename_workspace_ = nullptr;
  const bool had_focus = ::GetFocus() == rename_edit_;
  ::ShowWindow(rename_edit_, SW_HIDE);
  if (had_focus) FocusContent();
  Invalidate();
}

LRESULT CALLBACK BrowserWindow::RenameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR,
                                           DWORD_PTR ref) {
  auto* self = reinterpret_cast<BrowserWindow*>(ref);
  switch (msg) {
    case WM_KEYDOWN:
      if (wp == VK_RETURN) {
        self->CommitRename();
        return 0;
      }
      if (wp == VK_ESCAPE) {
        self->CancelRename();
        return 0;
      }
      break;
    case WM_CHAR:
      if (wp == L'\r' || wp == 0x1B) return 0;  // handled on keydown
      break;
    case WM_KILLFOCUS:
      ::PostMessageW(self->hwnd_, kCancelRenameMessage, 0, 0);
      break;
    case WM_NCDESTROY:
      ::RemoveWindowSubclass(hwnd, &BrowserWindow::RenameProc, 2);
      break;
  }
  return ::DefSubclassProc(hwnd, msg, wp, lp);
}

// One symbol: the first user-perceived character (a surrogate pair and any
// joiners/variation selectors that follow it).
static std::wstring FirstGrapheme(const std::wstring& icon) {
  std::wstring trimmed;
  for (size_t i = 0; i < icon.size(); ++i) {
    const wchar_t ch = icon[i];
    if (trimmed.empty() && std::iswspace(ch)) continue;
    const bool continuation = (ch >= 0xDC00 && ch <= 0xDFFF) || ch == 0x200D ||
                              (ch >= 0xFE00 && ch <= 0xFE0F) ||
                              (!trimmed.empty() && trimmed.back() == 0x200D);
    if (!trimmed.empty() && !continuation) break;
    trimmed += ch;
    if (trimmed.size() >= 16) break;
  }
  return trimmed;
}

void BrowserWindow::SetWorkspaceIcon(Workspace* ws, const std::wstring& icon) {
  if (!ws) return;
  const std::wstring trimmed = FirstGrapheme(icon);
  ws->mutable_record().icon = trimmed;
  if (!profile_->is_private()) Application::Get()->store().UpdateWorkspace(ws->record());
  Layout();
}

int BrowserWindow::UnloadWorkspaceTabs(Workspace* ws) {
  // ZenSpaceManager.unloadWorkspace: every loaded tab of the space; the tab
  // on screen stays (it cannot be hidden from under the user).
  if (!ws) return 0;
  int count = 0;
  for (const auto& t : ws->tabs()) {
    if (t.get() == active_tab() || !t->has_browser()) continue;
    t->Unload(/*mark_discarded=*/true);
    ++count;
  }
  Invalidate();
  return count;
}

int BrowserWindow::UnloadOtherWorkspaces(Workspace* keep) {
  int count = 0;
  for (Workspace* ws : Spaces()) {
    if (ws == keep) continue;
    if (ws->window() && ws->window() != this) continue;  // on screen in another window
    count += UnloadWorkspaceTabs(ws);
  }
  return count;
}

void BrowserWindow::TogglePinActiveTab() {
  if (Tab* tab = active_tab()) TogglePin(tab);
}

void BrowserWindow::ShowToast(ui::Toast::Content content) {
  if (!toast_) return;
  ui::Toast::Style style;
  // --zen-primary-color: the space's theme color (private windows use the
  // profile's accent).
  style.primary = palette_.accent;
  if (workspace_ && !profile_->is_private()) style.primary = ui::ColorFromRgb(workspace_->record().color);
  if (palette_.dark) style.primary = ui::Mix(style.primary, ui::ColorFromRgb(0x000000), 0.75f);
  style.dark = palette_.dark;
  toast_->Show(content, style);
}

bool BrowserWindow::ExtensionsButtonShown() const {
  // Always there in a normal window, so extensions can be found and added.
  return !profile_->is_private() && !chrome_hidden();
}

void BrowserWindow::OnExtensionsChanged() {
  Layout();
  Invalidate();
}

float BrowserWindow::LayoutExtensionButtons(float x, float y, float limit) {
  ext_btn_ = {x, y, x + kButton, y + kButton};
  return LayoutPinnedExtensions(ext_btn_.right + 1.f, y, limit);
}

float BrowserWindow::LayoutPinnedExtensions(float x, float y, float limit) {
  const ExtensionBridge* bridge = Application::Get()->FindExtensions(profile_);
  if (!bridge) return x;
  for (const std::string& id : Application::Get()->PinnedExtensions(profile_)) {
    const auto it = std::find_if(bridge->extensions().begin(), bridge->extensions().end(),
                                 [&](const ExtensionInfo& e) { return e.id == id && e.enabled; });
    if (it == bridge->extensions().end()) continue;  // removed or turned off
    if (x + kButton > limit) break;                   // the rest stay in the menu
    ext_pin_rects_.push_back({x, y, x + kButton, y + kButton});
    ext_pin_ids_.push_back(id);
    x += kButton + 1.f;
  }
  return x;
}

const ExtensionInfo* BrowserWindow::PinnedExtension(int index) const {
  if (index < 0 || index >= static_cast<int>(ext_pin_ids_.size())) return nullptr;
  const ExtensionBridge* bridge = Application::Get()->FindExtensions(profile_);
  if (!bridge) return nullptr;
  for (const ExtensionInfo& e : bridge->extensions()) {
    if (e.id == ext_pin_ids_[static_cast<size_t>(index)]) return &e;
  }
  return nullptr;
}

void BrowserWindow::OpenExtensionAction(const ExtensionInfo& extension, const D2D1_RECT_F& anchor) {
  if (extension.popup.empty()) {
    // Without a popup Chrome would send the extension a click Orbit cannot
    // deliver; its options are the next best thing.
    if (!extension.options_url.empty()) {
      OpenTab(FromUtf8(extension.options_url), true);
    } else {
      ui::Toast::Content content;
      content.id = L"extension-no-popup";
      content.text = FromUtf8(extension.name) + L" has no menu here";
      ShowToast(content);
    }
    return;
  }
  POINT below{static_cast<LONG>(px(anchor.left)), static_cast<LONG>(px(anchor.bottom + 6))};
  ::ClientToScreen(hwnd_, &below);
  ExtensionPopup::Open(this, extension, below);
}

// The extensions button: Orbit's own menu of the installed extensions (open
// one's popup), pinning them to the top bar, the Extensions page and the
// Web Store.
void BrowserWindow::ShowExtensionsMenu() {
  Application* app = Application::Get();
  const ExtensionBridge* bridge = app->Extensions(profile_);  // starts it if needed
  constexpr UINT kFirst = 100, kPinFirst = 400, kManage = 90, kStore = 91;
  HMENU menu = ::CreatePopupMenu();
  HMENU pins = ::CreatePopupMenu();
  std::vector<ExtensionInfo> shown;
  const std::vector<std::string> pinned = app->PinnedExtensions(profile_);
  if (bridge) {
    for (const ExtensionInfo& e : bridge->extensions()) {
      if (!e.enabled) continue;
      shown.push_back(e);
      const UINT i = static_cast<UINT>(shown.size() - 1);
      ::AppendMenuW(menu, MF_STRING, kFirst + i, FromUtf8(e.name).c_str());
      const bool is_pinned = std::find(pinned.begin(), pinned.end(), e.id) != pinned.end();
      ::AppendMenuW(pins, MF_STRING | (is_pinned ? MF_CHECKED : 0), kPinFirst + i, FromUtf8(e.name).c_str());
    }
  }
  if (shown.empty()) {
    ::AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No extensions yet");
  } else {
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(pins), L"Pin to toolbar");
  }
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kManage, L"Manage extensions");
  ::AppendMenuW(menu, MF_STRING, kStore, L"Get extensions");
  POINT at{static_cast<LONG>(px(ext_btn_.left)), static_cast<LONG>(px(ext_btn_.bottom + 2))};
  ::ClientToScreen(hwnd_, &at);
  const UINT cmd = static_cast<UINT>(
      ui::TrackMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, at.x, at.y, hwnd_, palette_));
  if (shown.empty()) ::DestroyMenu(pins);
  ::DestroyMenu(menu);  // with the submenu
  if (cmd == kManage) {
    OpenInternalPage(kPageExtensions);
  } else if (cmd == kStore) {
    OpenTab(L"https://chromewebstore.google.com/category/extensions", true);
  } else if (cmd >= kPinFirst && cmd - kPinFirst < shown.size()) {
    const ExtensionInfo& e = shown[cmd - kPinFirst];
    const bool is_pinned = std::find(pinned.begin(), pinned.end(), e.id) != pinned.end();
    app->SetExtensionPinned(profile_, e.id, !is_pinned);
  } else if (cmd >= kFirst && cmd - kFirst < shown.size()) {
    OpenExtensionAction(shown[cmd - kFirst], ext_btn_);
  }
}

bool BrowserWindow::CanCopyUrlFromButton() const {
  Tab* tab = active_tab();
  return tab && tab->url().rfind(L"http", 0) == 0;
}

bool BrowserWindow::CopyCurrentUrl(bool markdown) {
  // ZenCommonUtils.copyCurrentURLToClipboard: copies what the address bar
  // holds (typed text first, then the page URL) and confirms with a toast.
  Tab* tab = active_tab();
  if (!tab) return false;
  std::wstring url = tab->url();
  if (address_editing_) {
    const int len = ::GetWindowTextLengthW(address_);
    std::wstring typed(static_cast<size_t>(len), L'\0');
    ::GetWindowTextW(address_, typed.data(), len + 1);
    if (!typed.empty()) url = typed;
  }
  if (IsNewTabUrl(url)) return false;
  url = StripTrackingParams(url);  // zenStrippedURI (stripForCopyOrShare)
  const std::wstring plain = url;
  if (markdown) url = L"[" + tab->DisplayTitle() + L"](" + url + L")";
  bool open = false;  // another application may hold the clipboard for a moment
  for (int attempt = 0; attempt < 20 && !(open = ::OpenClipboard(hwnd_)); ++attempt) ::Sleep(10);
  if (!open) {
    ORBIT_LOG_WARNING("copy URL: clipboard is busy");
    return false;
  }
  bool ok = false;
  ::EmptyClipboard();
  const size_t bytes = (url.size() + 1) * sizeof(wchar_t);
  if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
    if (void* dst = ::GlobalLock(mem)) {
      memcpy(dst, url.c_str(), bytes);
      ::GlobalUnlock(mem);
      ok = ::SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
    }
    if (!ok) ::GlobalFree(mem);
  }
  ::CloseClipboard();
  if (ok) {
    ui::Toast::Content toast{L"copy-url", markdown ? L"Copied current URL as Markdown!" : L"Copied current URL!",
                             L"", L"", 3000};
    // canShare: a share button for web links (Windows share sheet).
    if (plain.rfind(L"http", 0) == 0) {
      toast.button_glyph = 0xE72D;  // Share
      toast.button_label = L"Share";
      const std::wstring title = tab->DisplayTitle();
      toast.on_button = [this, plain, title] { ShareUrl(plain, title); };
    }
    ShowToast(toast);
  }
  return ok;
}

void BrowserWindow::ShareUrl(const std::wstring& url, const std::wstring& title) {
  if (self_test::IsActive()) {
    last_share_url_ = url;  // the share sheet would block the run
    return;
  }
  if (!ui::ShareWebLink(hwnd_, url, title)) {
    ShowToast({L"share-failed", L"Sharing is not available", L"", L"", 3000});
  }
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
  add(HitKind::kBack, 0, Role::kButton, L"Back", back_,
      tab && (tab->can_go_back() || BackClosesTab()));
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
  if (CanCopyUrlFromButton()) add(HitKind::kCopyUrl, 0, Role::kButton, L"Copy URL", copy_url_);
  if (ext_btn_.right > ext_btn_.left) add(HitKind::kExtensions, 0, Role::kButton, L"Extensions", ext_btn_);
  for (size_t i = 0; i < ext_pin_rects_.size(); ++i) {
    if (const ExtensionInfo* e = PinnedExtension(static_cast<int>(i))) {
      add(HitKind::kExtensionPin, static_cast<int>(i), Role::kButton, FromUtf8(e->name), ext_pin_rects_[i]);
    }
  }
  if (workspace_) {
    const bool linked = workspace_->is_linked() && workspace_->parent();
    add(HitKind::kWorkspaceIndicator, 0, Role::kButton,
        linked ? L"Linked to " + workspace_->parent()->record().name : L"Space: " + workspace_->record().name,
        ws_indicator_);
    if (workspace_->PinnedCount() > 0) items.back().expanded = !PinnedSectionCollapsed();
    if (linked) {
      add(HitKind::kLinkedBack, 0, Role::kButton,
          L"Bring tabs back to " + workspace_->parent()->record().name, linked_back_);
    }
  }
  if (const Workspace* ess = essentials()) {
    const auto rects = EssentialRects();
    for (size_t i = 0; i < rects.size() && i < ess->tabs().size(); ++i) {
      Tab* t = ess->tabs()[i].get();
      add(HitKind::kEssential, reinterpret_cast<intptr_t>(t), Role::kTab,
          t->DisplayTitle() + L", essential", rects[i]);
      items.back().selected = t == tab;
    }
  }
  if (const D2D1_RECT_F nt = NewTabRect(); nt.right > nt.left) {
    add(HitKind::kNewTab, 0, Role::kButton, L"New tab", nt);
  }
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
      const auto* live = Application::Get()->live_folders().Find(g->id);
      add(HitKind::kGroup, static_cast<intptr_t>(row.group_id), Role::kGroup,
          std::wstring(live ? L"Live folder " : L"Folder ") + g->name + L", " +
              std::to_wstring(workspace_->GroupSize(g->id)) + L" tabs",
          rect);
      items.back().expanded = !g->collapsed;
      if (const D2D1_RECT_F err = LiveErrorRect(row); err.right > err.left) {
        add(HitKind::kLiveError, static_cast<intptr_t>(row.group_id), Role::kButton,
            LiveErrorTooltip(live->state.last_error), err);
      }
    } else if (row.linked) {
      const auto children = LinkedChildren();
      const int k = LinkedIndex(row.linked);
      if (k < 0) continue;
      Workspace* ls = children[static_cast<size_t>(k)];
      int n = 0;
      for (const auto& t : ls->tabs()) n += !t->empty_placeholder();
      add(HitKind::kLinked, reinterpret_cast<intptr_t>(ls), Role::kButton,
          L"Linked window " + ls->record().name + L", " + std::to_wstring(n) + (n == 1 ? L" tab" : L" tabs"),
          rect);
    } else if (row.split) {
      const int n = SplitMemberCount(row);
      for (int k = 0; k < n; ++k) {
        Tab* t = workspace_->tabs()[static_cast<size_t>(row.tab + k)].get();
        add(HitKind::kTab, reinterpret_cast<intptr_t>(t), Role::kTab,
            t->DisplayTitle() + L", in split view", TabRowRect(row.tab + k));
        items.back().selected = t == tab;
        if (k + 1 < n) items.back().focused = false;
      }
    } else {
      Tab* t = workspace_->tabs()[static_cast<size_t>(row.tab)].get();
      std::wstring name = t->DisplayTitle();
      if (!t->record().sublabel.empty()) name += L", " + t->record().sublabel;
      if (t->pinned()) name += L", pinned";
      if (t->loading()) name += L", loading";
      if (t->is_discarded()) name += L", sleeping";
      if (t->muted()) name += L", muted";
      if (t->media_capture()) name += L", using camera or microphone";
      if (t->pin_changed()) name += L", away from its pinned page";
      add(HitKind::kTab, reinterpret_cast<intptr_t>(t), Role::kTab, std::move(name), rect);
      items.back().selected = t == tab;
    }
    items.back().focused = sidebar_focus && focus_row_ == static_cast<int>(r);
  }
  // Footer.
  add(HitKind::kDownloads, 0, Role::kButton, L"Library", downloads_);
  const std::vector<Workspace*> workspaces = Spaces();
  for (size_t i = 0; i < workspace_dots_.size(); ++i) {
    const size_t w = static_cast<size_t>(first_dot_workspace_) + i;
    if (w >= workspaces.size()) break;
    Workspace* ws = workspaces[w];
    add(HitKind::kWorkspace, reinterpret_cast<intptr_t>(ws), Role::kButton,
        L"Space: " + SpaceLabel(ws), workspace_dots_[i]);
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
    case HitKind::kLiveError:
      hit.index = GroupIndex(static_cast<int64_t>(key));
      break;
    case HitKind::kEssential:
      hit.index = essentials()
                      ? essentials()->IndexOf(reinterpret_cast<Tab*>(key))
                      : -1;
      break;
    case HitKind::kLinked: {
      hit.index = -1;
      const auto children = LinkedChildren();
      for (size_t i = 0; i < children.size(); ++i) {
        if (reinterpret_cast<intptr_t>(children[i]) == key) hit.index = static_cast<int>(i);
      }
      break;
    }
    case HitKind::kWorkspace: {
      hit.index = -1;
      const auto ws = Spaces();
      for (size_t i = 0; i < ws.size(); ++i) {
        if (reinterpret_cast<intptr_t>(ws[i]) == key) hit.index = static_cast<int>(i);
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
        (kind == static_cast<int>(HitKind::kTab) && !rows[r].is_group && rows[r].tab >= 0 &&
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
                                              L"orbit://history/", L"orbit://settings/",
                                              L"orbit://extensions/"};
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
  if (overlay_visible_) ::InvalidateRect(overlay_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Compact-mode overlay (fix 6)

bool BrowserWindow::overlay_shown() const {
  return CompactHidesSidebar() && !chrome_hidden() && sidebar_visible();
}

HWND BrowserWindow::SidebarHost() const { return overlay_visible_ ? overlay_ : hwnd_; }

HWND BrowserWindow::AddressHost() const { return multi_toolbar() ? hwnd_ : SidebarHost(); }

POINT BrowserWindow::OverlayOriginPx() const {
  const RECT r = ToPx(panel_);
  return {r.left, r.top};
}

RECT BrowserWindow::ToHostPx(const D2D1_RECT_F& r) const {
  RECT out = ToPx(r);
  if (overlay_visible_) {
    const POINT o = OverlayOriginPx();
    ::OffsetRect(&out, -o.x, -o.y);
  }
  return out;
}

void BrowserWindow::UpdateOverlay() {
  if (!overlay_) return;
  if (!CompactHidesSidebar()) {
    compact_hiding_ = false;
    compact_slide_.Jump(0.f);
  }
  const bool show = overlay_shown();
  if (show) {
    const RECT r = ToPx(panel_);
    const SIZE size{r.right - r.left, r.bottom - r.top};
    if (size.cx != overlay_size_.cx || size.cy != overlay_size_.cy) {
      overlay_size_ = size;
      // The card's rounded corners (10px), as a window region.
      const int d = 2 * static_cast<int>(std::lround(px(10.f)));
      ::SetWindowRgn(overlay_, ::CreateRoundRectRgn(0, 0, size.cx + 1, size.cy + 1, d, d), TRUE);
      if (overlay_rt_) {
        overlay_rt_->Resize(D2D1::SizeU(static_cast<UINT32>(size.cx),
                                        static_cast<UINT32>(size.cy)));
      }
    }
  }
  if (show != overlay_visible_) {
    overlay_visible_ = show;
    // A space-switch snapshot belongs to the device that painted it.
    space_snapshot_.Reset();
    space_slide_.Jump(1.f);
    // The sidebar's EDITs ride on the card while it shows (the label stays
    // right before the address EDIT for its accessible name).
    const HWND host = SidebarHost();
    ::SetParent(address_label_, AddressHost());
    if (!popup_visible_) {
      ::SetParent(address_, AddressHost());
      ::SetWindowPos(address_, address_label_, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    ::SetParent(rename_edit_, host);
    if (!show) ::ShowWindow(overlay_, SW_HIDE);
  }
  if (show) {
    MoveOverlay();
    ::InvalidateRect(overlay_, nullptr, FALSE);
  }
}

void BrowserWindow::MoveOverlay() {
  if (!overlay_visible_) return;
  const POINT o = OverlayOriginPx();
  // margin-left: -width -> 0 (the card enters from the window's left edge).
  const float p = compact_slide_.Value(::GetTickCount64());
  const int dx = right_side_
                     ? static_cast<int>(std::lround(px((1.f - p) * (Width() - panel_.left + 8.f))))
                     : -static_cast<int>(std::lround(px((1.f - p) * (panel_.right + 8.f))));
  ::SetWindowPos(overlay_, HWND_TOP, o.x + dx, o.y, overlay_size_.cx, overlay_size_.cy,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void BrowserWindow::PaintOverlay() {
  if (!overlay_visible_) return;
  if (!overlay_rt_) {
    const auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        static_cast<float>(dpi_), static_cast<float>(dpi_));
    const auto hwnd_props = D2D1::HwndRenderTargetProperties(
        overlay_, D2D1::SizeU(static_cast<UINT32>(overlay_size_.cx),
                              static_cast<UINT32>(overlay_size_.cy)));
    if (FAILED(ui::Graphics::Get().d2d()->CreateHwndRenderTarget(props, hwnd_props,
                                                                 &overlay_rt_))) {
      ORBIT_LOG_ERROR("CreateHwndRenderTarget (overlay) failed");
      return;
    }
    overlay_rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    overlay_rt_->CreateSolidColorBrush(palette_.text, &overlay_brush_);
    overlay_favicons_.clear();
    overlay_pinned_favicons_.clear();
    overlay_ext_icons_.clear();
  }
  // PaintSidebar and the favicon caches use rt_/brush_: lend them the
  // overlay's resources for this frame.
  rt_.Swap(overlay_rt_);
  brush_.Swap(overlay_brush_);
  favicons_.swap(overlay_favicons_);
  pinned_favicons_.swap(overlay_pinned_favicons_);
    ext_icons_.swap(overlay_ext_icons_);
  ID2D1RenderTarget* rt = rt_.Get();
  rt->BeginDraw();
  rt->SetTransform(D2D1::Matrix3x2F::Translation(-panel_.left, -panel_.top));
  PaintBackdrop(rt);
  PaintSidebar(rt);
  rt->SetTransform(D2D1::Matrix3x2F::Identity());
  const HRESULT hr = rt->EndDraw();
  rt_.Swap(overlay_rt_);
  brush_.Swap(overlay_brush_);
  favicons_.swap(overlay_favicons_);
  pinned_favicons_.swap(overlay_pinned_favicons_);
    ext_icons_.swap(overlay_ext_icons_);
  if (hr == D2DERR_RECREATE_TARGET) {
    overlay_rt_.Reset();
    overlay_brush_.Reset();
    overlay_favicons_.clear();
    overlay_pinned_favicons_.clear();
    overlay_ext_icons_.clear();
    space_snapshot_.Reset();
    ::InvalidateRect(overlay_, nullptr, FALSE);
  }
}

LRESULT BrowserWindow::ForwardOverlayMouse(UINT msg, WPARAM wp, LPARAM lp) {
  POINT pt{LParamX(lp), LParamY(lp)};
  ::MapWindowPoints(overlay_, hwnd_, &pt, 1);
  forwarding_mouse_ = true;
  const LRESULT result = HandleMessage(msg, wp, MAKELPARAM(pt.x, pt.y));
  forwarding_mouse_ = false;
  return result;
}

LRESULT CALLBACK BrowserWindow::OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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
      self->PaintOverlay();
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      if (self->overlay_rt_) self->overlay_rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
      // Empty chrome drags the window and its top edge resizes it, as on the
      // main window (HitTestNonClient), so hand those to the frame.
      POINT screen{LParamX(lp), LParamY(lp)};
      ::ClientToScreen(hwnd, &screen);
      const LRESULT nc = self->HitTestNonClient(MAKELPARAM(screen.x, screen.y));
      if (nc != HTCLIENT) {
        ::ReleaseCapture();
        ::SendMessageW(self->hwnd_, msg == WM_LBUTTONDOWN ? WM_NCLBUTTONDOWN : WM_NCLBUTTONDBLCLK,
                       static_cast<WPARAM>(nc), MAKELPARAM(screen.x, screen.y));
        return 0;
      }
      return self->ForwardOverlayMouse(msg, wp, lp);
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
      return self->ForwardOverlayMouse(msg, wp, lp);
    case WM_MOUSELEAVE:
    case WM_MOUSEWHEEL:  // screen coordinates
    case WM_MOUSEHWHEEL:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_COMMAND:
      return self->HandleMessage(msg, wp, lp);
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        POINT pt;
        ::GetCursorPos(&pt);
        const LRESULT nc = self->HitTestNonClient(MAKELPARAM(pt.x, pt.y));
        if (nc == HTTOP || nc == HTTOPLEFT) {
          ::SetCursor(::LoadCursorW(nullptr, nc == HTTOP ? IDC_SIZENS : IDC_SIZENWSE));
          return TRUE;
        }
        return self->HandleMessage(msg, reinterpret_cast<WPARAM>(self->hwnd_), lp);
      }
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace orbit
