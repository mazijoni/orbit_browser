#pragma once

#include <windows.h>

#include <d2d1.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "persistence/store.h"
#include "ui/accessibility.h"
#include "ui/theme.h"

class CefWindowInfo;

namespace orbit {

class Profile;
class Tab;
class Workspace;

// A top-level browser window laid out like Zen Browser's single-toolbar mode:
// a frameless window whose vertical sidebar holds the navigation buttons, the
// address bar, the workspace indicator, the tab list and a footer with the
// workspace switcher; the web content sits in a rounded card beside it. The
// window controls appear when the pointer reaches the top edge.
//
// The chrome is drawn with Direct2D into this HWND; the address bar is a
// native EDIT control (moved into a floating popup for the centered "floating
// URL bar") and each tab's web content is a child HWND owned by its Tab. A
// window shows exactly one profile and one workspace at a time.
//
// Ownership: owned by Application. Threading: UI thread only.
class BrowserWindow {
 public:
  explicit BrowserWindow(Profile* profile);
  ~BrowserWindow();
  BrowserWindow(const BrowserWindow&) = delete;
  BrowserWindow& operator=(const BrowserWindow&) = delete;

  bool Create(const WindowRecord* placement);
  void Destroy();

  HWND hwnd() const { return hwnd_; }
  Profile* profile() const { return profile_; }
  Workspace* workspace() const { return workspace_; }
  const ui::Palette& palette() const { return palette_; }
  bool closing() const { return closing_; }
  // Where the active tab's content is placed, in client pixels.
  RECT ContentRectPx() const;
  // Corner radius of the content card in pixels (0 in full screen).
  int ContentCornerPx() const;
  WindowRecord ToRecord() const;

  // Workspaces and tabs.
  void ShowWorkspace(Workspace* workspace, int64_t active_tab_id);
  Tab* OpenTab(const std::wstring& url, bool activate);
  Tab* OpenTabNear(Tab* opener, const std::wstring& url, bool activate);
  // Inserts a tab for a Chromium-created popup and points |info| at its host.
  Tab* CreatePopupTab(Tab* opener, const std::wstring& url, bool activate, CefWindowInfo& info);
  void ActivateTab(Tab* tab);
  Tab* active_tab() const;

  // Notifications from the model.
  void OnTabChanged(Tab* tab);
  void OnTabRemoving(Tab* tab);
  void OnTabsChanged();
  void OnContentFocused(Tab* tab);

  bool HandleAccelerator(int vk, bool ctrl, bool shift, bool alt);
  bool AddressBarHasFocus() const { return address_ && ::GetFocus() == address_; }
  void SetContentFullscreen(bool fullscreen);
  void BeginClose();
  void ApplyTheme();
  // Collapsed sidebar: icons only (Zen's "zen.view.sidebar-expanded" off).
  void SetCompact(bool compact);
  // Compact mode: the sidebar is hidden and slides in from the left edge.
  void SetCompactMode(bool hidden);

  // UI Automation (see ui/accessibility.h).
  std::vector<AccessibleItem> AccessibleItems() const;
  void AccessibleInvoke(int kind, intptr_t key);  // posted to the window
  void AccessibleFocus(int kind, intptr_t key);
  void OnDownloadsChanged();
  void OnBookmarksChanged();

 private:
  enum class HitKind {
    kNone,
    kBack,
    kForward,
    kReload,
    kSiteInfo,
    kStar,
    kMenu,
    kSidebarToggle,   // compact mode
    kCaptionMin,
    kCaptionMax,
    kCaptionClose,
    kWorkspaceIndicator,
    kWorkspaceActions,
    kWorkspace,       // footer switcher; index = position in profile()->workspaces()
    kCreateNew,
    kDownloads,
    kNewTab,
    kClearTabs,
    kTab,
    kTabClose,
    kGroup,  // index = position in workspace()->groups()
    kSidebarEdge,
    kTabListEmpty,  // empty space below the tabs (double-click opens a tab)
  };
  struct Hit {
    HitKind kind = HitKind::kNone;
    int index = -1;
    bool operator==(const Hit& o) const { return kind == o.kind && index == o.index; }
  };
  enum InternalPage { kPageDownloads, kPageBookmarks, kPageHistory, kPageSettings };

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  static LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  static LRESULT CALLBACK AddressProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                      UINT_PTR id, DWORD_PTR ref);
  LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
  LRESULT HitTestNonClient(LPARAM lp);

  // Layout (DIPs unless noted).
  void Layout();
  void PlaceContent();  // moves the active tab's host (reveal offsets)
  float Width() const;
  float Height() const;
  // Width the sidebar takes from the content (0 in compact mode).
  float SidebarWidth() const;
  // Width of the sidebar as drawn (the revealed panel in compact mode).
  float SidebarPanelWidth() const;
  bool sidebar_visible() const;
  bool chrome_hidden() const { return content_fullscreen_ || window_fullscreen_; }
  D2D1_RECT_F ContentRectDip() const;  // layout rect, before reveal offsets
  // Sidebar list rows: pinned tabs, then regular tabs with a header row per
  // tab group (collapsed groups show only the header). Between the two
  // sections sit the pinned separator (with "Clear") and the "New Tab" row.
  struct Row {
    bool is_group = false;
    int tab = -1;           // tab index (tab rows)
    int64_t group_id = 0;   // group header, or the group a tab row belongs to
    float top = 0.f;        // offset from the top of the list, before scrolling
  };
  std::vector<Row> Rows() const;
  float PeripheryTop() const;     // list offset of the separator / new tab block
  float PeripheryHeight() const;
  float ListContentHeight() const;
  D2D1_RECT_F RowRect(const Row& row) const;
  D2D1_RECT_F NewTabRect() const;
  D2D1_RECT_F ClearRect() const;
  int RowOfTab(const std::vector<Row>& rows, int tab_index) const;
  int GroupIndex(int64_t group_id) const;
  D2D1_RECT_F TabRowRect(int index) const;
  int VisibleRowCapacity() const;
  float px(float dip) const { return dip * static_cast<float>(dpi_) / 96.f; }
  float dip(float pixels) const { return pixels * 96.f / static_cast<float>(dpi_); }
  RECT ToPx(const D2D1_RECT_F& r) const;
  Hit HitTest(float x, float y) const;
  bool IsCaptionHit(const Hit& hit) const;
  void ClampScroll();
  void EnsureTabVisible(int index);

  // Reveal of the window controls and of the compact-mode sidebar.
  void UpdateReveal(float x, float y);
  void SetCaptionRevealed(bool revealed);
  void SetSidebarRevealed(bool revealed);

  // Painting.
  void Paint();
  bool EnsureRenderTarget();
  void PaintBackdrop(ID2D1RenderTarget* rt);
  void PaintContentFrame(ID2D1RenderTarget* rt);
  void PaintCaption(ID2D1RenderTarget* rt);
  void PaintSidebar(ID2D1RenderTarget* rt);
  void PaintUrlBar(ID2D1RenderTarget* rt);
  void PaintFooter(ID2D1RenderTarget* rt);
  void PaintTabRow(ID2D1RenderTarget* rt, int index, const D2D1_RECT_F& row, bool focused,
                   int64_t group_id);
  void PaintGroupRow(ID2D1RenderTarget* rt, const Row& row, const D2D1_RECT_F& rect,
                     bool focused);
  void PaintIconButton(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, wchar_t glyph, Hit hit,
                       bool enabled, const D2D1_COLOR_F* color = nullptr);
  void PaintSoftShadow(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, float radius,
                       float spread, float offset_y, float alpha);
  void PaintPopup();
  ID2D1Bitmap* FaviconBitmap(Tab* tab);
  void RefreshBackdrop();  // workspace color or theme strength changed
  D2D1_COLOR_F BackdropAt(float x, float y) const;
  D2D1_COLOR_F UrlFieldColor() const;  // opaque, so the EDIT can match it
  void Invalidate();
  void UpdateSpinnerTimer();

  // Input.
  void OnMouseMove(float x, float y, WPARAM keys);
  void OnLButtonDown(float x, float y);
  void OnLButtonUp(float x, float y);
  void OnLButtonDoubleClick(float x, float y);
  void OnContextMenu(int screen_x, int screen_y, float x, float y);
  void OnMouseWheel(int delta);
  void OnMouseHWheel(int delta);
  bool OnKeyDown(int vk);
  void Click(const Hit& hit);
  void ShowTabMenu(Tab* tab, POINT screen);
  void ShowWorkspaceMenu(int index, POINT screen);
  void ShowGroupMenu(int64_t group_id, POINT screen);
  void ToggleGroup(int64_t group_id);
  void NewGroupWithTab(Tab* tab);
  void ShowProfileMenu(POINT screen);
  void ShowMainMenu(POINT screen);
  void ShowCreateMenu(POINT screen);
  void UpdateTooltip(const Hit& hit);
  std::wstring TooltipText() const;

  // Commands.
  void NewTab();
  void CloseTab(Tab* tab);
  void CloseOtherTabs(Tab* keep);
  void CloseTabsBelow(Tab* tab);
  void ClearUnpinnedTabs();
  void DuplicateTab(Tab* tab);
  void TogglePin(Tab* tab);
  void MoveTabToWorkspace(Tab* tab, Workspace* target);
  void MoveTabToWindow(Tab* tab, BrowserWindow* target);
  void SelectRelativeTab(int delta);
  void SelectTabIndex(int index);
  void SelectRelativeWorkspace(int delta);
  void FocusAddressBar();
  void GuardAddressFocus();
  void FocusContent();
  void SwitchWorkspace(Workspace* workspace);
  void NewWorkspace();
  void RenameWorkspace(Workspace* workspace);
  void DeleteWorkspace(Workspace* workspace);
  void ToggleCompact();
  void ToggleCompactMode();
  void ToggleBookmark();
  void RefreshBookmarkState();
  void ToggleWindowFullscreen();
  void SetChromeFullscreen(bool on);
  void NavigateFromAddressBar();
  void UpdateAddressBar(bool force);
  void UpdateAddressColors();
  void UpdateTitle();
  bool PromptText(const wchar_t* title, const wchar_t* label, std::wstring& value);
  void OpenNormalWindow();
  void OpenInternalPage(int page);

  // Floating URL bar (Zen's centered command bar, shown for new tabs and
  // whenever the sidebar's own address bar is not visible).
  void ShowFloatingUrlBar();
  void HideFloatingUrlBar(bool restore_text);
  void PositionFloatingUrlBar();
  bool floating_url_bar() const { return popup_visible_; }

  void HandleAccessibleInvoke(int kind, intptr_t key);
  void NotifyAccessibleFocus();

  HWND hwnd_ = nullptr;
  std::unique_ptr<AccessibilityHost> accessibility_;
  HWND address_ = nullptr;
  HWND address_label_ = nullptr;  // names the EDIT for accessibility
  HWND popup_ = nullptr;  // floating URL bar (owned top-level window)
  HWND popup_label_ = nullptr;
  HWND tooltip_ = nullptr;
  HFONT address_font_ = nullptr;
  HFONT popup_font_ = nullptr;
  HBRUSH address_brush_ = nullptr;
  HBRUSH popup_brush_ = nullptr;
  UINT dpi_ = 96;

  Profile* profile_;
  Workspace* workspace_ = nullptr;
  ui::Palette palette_;
  ui::Backdrop backdrop_{};
  float theme_strength_ = 0.45f;
  bool closing_ = false;

  // Sidebar state.
  float sidebar_width_ = ui::metrics::kSidebarDefaultWidth;
  bool collapsed_ = false;      // icons only
  bool compact_mode_ = false;   // hidden until the pointer reaches the left edge
  bool sidebar_revealed_ = false;
  bool caption_revealed_ = false;
  bool sidebar_hovered_ = false;
  float scroll_ = 0.f;
  int focus_row_ = -1;  // keyboard focus: index into Rows() (-1 = none)

  // Interaction state.
  Hit hover_;
  Hit pressed_;
  POINT press_px_{};
  bool dragging_tab_ = false;
  int drag_from_ = -1;
  int drop_index_ = -1;  // insertion point as an index into Rows()
  bool resizing_sidebar_ = false;
  bool tracking_mouse_ = false;
  bool tracking_nc_mouse_ = false;
  Hit tooltip_hit_;
  float hwheel_accum_ = 0.f;

  // Address bar.
  bool address_editing_ = false;
  bool bookmarked_ = false;  // active tab's URL is bookmarked (cached for painting)
  bool address_programmatic_ = false;
  bool popup_visible_ = false;
  Tab* focus_guard_tab_ = nullptr;  // see OnContentFocused
  ULONGLONG focus_guard_until_ms_ = 0;

  // Fullscreen.
  bool content_fullscreen_ = false;
  bool window_fullscreen_ = false;
  WINDOWPLACEMENT saved_placement_{sizeof(WINDOWPLACEMENT)};
  LONG_PTR saved_style_ = 0;

  // Spinner animation (only runs while a visible tab is loading).
  bool spinner_running_ = false;
  float spinner_phase_ = 0.f;

  // Direct2D resources (recreated on device loss).
  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> rt_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> popup_rt_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> popup_brush_d2d_;
  struct CachedFavicon {
    uint32_t generation = 0;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
  };
  std::unordered_map<const Tab*, CachedFavicon> favicons_;

  // Cached layout rects (DIPs).
  D2D1_RECT_F address_rect_{}, back_{}, forward_{}, reload_{}, site_info_{}, menu_{}, star_{},
      toggle_{}, caption_min_{}, caption_max_{}, caption_close_{}, ws_indicator_{},
      ws_actions_{}, list_{}, footer_{}, downloads_{}, create_new_{}, panel_{};
  std::vector<D2D1_RECT_F> workspace_dots_;
  int first_dot_workspace_ = 0;  // workspace index shown by workspace_dots_[0]
};

}  // namespace orbit
