#pragma once

#include <windows.h>

#include <d2d1.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "core/live_folders.h"
#include "core/motion.h"
#include "core/omnibox_results.h"
#include "core/split_layout.h"
#include "browser/profile.h"
#include "include/cef_browser.h"
#include "persistence/store.h"
#include "ui/accessibility.h"
#include "ui/theme.h"
#include "ui/download_flight.h"
#include "ui/toast.h"

class CefWindowInfo;

namespace orbit {

class Profile;
class Tab;
struct FaviconPixels;
struct ExtensionInfo;
class Workspace;
class LibraryClient;

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
  // Posted by a tab whose browser was closed for a reset (lParam: Tab*).
  static constexpr UINT kRecreateTabMessage = WM_APP + 3;

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
  // The spaces this window switches between: those of every normal profile
  // (one order; Zen's container per space), or a private window's own. The
  // window's profile is always its current space's.
  std::vector<Workspace*> Spaces() const;
  // |start_empty|: open on an empty tab even when the space has tabs.
  void ShowWorkspace(Workspace* workspace, int64_t active_tab_id, bool start_empty = false);
  // "Profile ▸" in the space menu: |ws| moves to |profile| (its tabs reload,
  // signed out, in that profile's storage). Shows a toast.
  bool MoveSpaceToProfile(Workspace* ws, Profile* profile);
  void OnSpaceProfileChanged(Workspace* ws);  // some space moved to another profile
  void ForgetProfile(Profile* profile);       // its spaces and tabs are about to go
  void ForgetSpace(Workspace* ws);            // |ws| is about to be deleted
  void SwitchToSpace(Workspace* ws) { SwitchWorkspace(ws); }

  // Linked window spaces (instead of Zen's window sync). "Move to → New
  // window" and a tab dragged out of every Orbit window tear the tab off
  // into a new window with a temporary space linked to the current one (its
  // parent): same profile, the parent's Essentials, the tab's live page kept.
  // |screen|: where the drag ended (the new window opens there).
  BrowserWindow* TearOffTab(Tab* tab, std::optional<POINT> screen = std::nullopt);
  // The linked space this window was made for; it stays this window's while
  // the window shows another space (until its tabs go back).
  Workspace* linked_space() const { return linked_space_; }
  void AdoptLinkedSpace(Workspace* ws);  // session restore
  // The linked spaces of the current space, shown as "temp folder" rows.
  std::vector<Workspace*> LinkedChildren() const;
  // Focuses the window of |linked| (showing it there).
  void FocusLinkedSpace(Workspace* linked);
  // The window is about to close and its space goes away first (a linked
  // window's tabs go back to their parent): it lets go of what it shows.
  void DetachWorkspace();
  // "Move to space" / "Move to window": across profiles too (the tab unloads
  // and loads again in the new profile), never into or out of private ones.
  void MoveTabToWorkspace(Tab* tab, Workspace* target);
  void MoveTabToWindow(Tab* tab, BrowserWindow* target);
  Tab* OpenTab(const std::wstring& url, bool activate);
  Tab* OpenTabNear(Tab* opener, const std::wstring& url, bool activate);
  // A new tab that space routing may send to another space (links opened in
  // new tabs, the new-tab URL bar, links from other applications). A routed
  // tab waits there unloaded; a toast says where it went.
  Tab* OpenRoutedTab(const std::wstring& url, bool activate, bool external, Tab* opener = nullptr);
  // Inserts a tab for a Chromium-created popup and points |info| at its host.
  // |as_glance|: shown as a glance over |opener| (sign-in popups) instead of a tab.
  Tab* CreatePopupTab(Tab* opener, const std::wstring& url, bool activate, CefWindowInfo& info,
                      bool as_glance = false);
  void ActivateTab(Tab* tab);
  Tab* active_tab() const;

  // Notifications from the model.
  void OnTabChanged(Tab* tab);
  void OnTabRemoving(Tab* tab);
  void OnTabsChanged();
  void OnContentFocused(Tab* tab);
  void OnTabMovedAway(Tab* tab);  // another window took one of our tabs
  void OnDevToolsChanged(Tab* tab);  // its DevTools opened, closed or loaded

  // DevTools, docked to the right of the page (Chrome's default): F12 and
  // Ctrl+Shift+I toggle them for the current tab; "Inspect" in the page menu
  // selects the element under the pointer. The divider resizes the pane.
  void ToggleDevTools();
  void InspectElement(Tab* tab, int x, int y);
  RECT DevToolsRectPx() const;  // empty when no pane shows (tests)
  RECT DevToolsPageRectPx() const;  // the page beside docked DevTools (tests)

  // Zen Essentials (ZenPinnedTabManager.addToEssentials / removeEssentials),
  // one grid per space (zen.workspaces.separate-essentials, always on).
  static constexpr int kMaxEssentials = 12;  // zen.tabs.essentials.max
  // The grid on screen: the current space's (a linked space shows its
  // parent's). Null when that space has none yet.
  Workspace* essentials() const;
  bool IsEssential(const Tab* tab) const;  // in that grid
  bool CanAddToEssentials(const Tab* tab) const;
  bool AddToEssentials(Tab* tab);
  void RemoveFromEssentials(Tab* tab);
  // ZenSpace collapsiblePins: clicking the space name folds the pinned
  // section (the selected pinned tab stays visible). Persisted per space.
  bool PinnedSectionCollapsed() const;
  void TogglePinnedSection();
  // Split view (ZenViewSplitter). Ctrl+Alt+G/V/H: the current tab with the
  // next one (or a new layout for the current split); Ctrl+Alt+U unsplits.
  void SplitWithNext(SplitGrid grid);
  void UnsplitCurrent();
  void NewEmptySplit();  // Ctrl+Shift+*, "New Split": a new tab beside the current one
  // "Add Split View..." on another tab: it joins the current tab's split (or
  // a new one with the current tab).
  void SplitTabWithCurrent(Tab* tab);
  void SplitOut(Tab* tab);  // "Split Out Tab"
  // splitLinkInNewTab: the link opens in a new tab split with |from|.
  void SplitLinkInNewTab(Tab* from, const std::wstring& url);

  // Glance (ZenGlanceManager): Alt+click on a link (or "Open Link in
  // Glance") previews it in a tab over the current one. Escape, a click
  // beside it or its close button close it; "expand" keeps it as a normal
  // tab; "split" puts it in a split view with its parent.
  // zen.view.drag-window-from-content: the page's top strip moved |moved_px|
  // with the left button down. Maximized/snapped windows need 50 px.
  void StartWindowDrag(double moved_px);
  // A link's rect in its page (CSS px) and the page zoom: the glance grows
  // out of it (ZenGlanceManager #lastLinkClickData).
  struct GlanceOrigin {
    double x = 0, y = 0, width = 0, height = 0;
    double zoom = 1;
  };
  void OpenGlance(Tab* parent, const std::wstring& url,
                  std::optional<GlanceOrigin> origin = std::nullopt);
  bool glance_animating() const { return glance_ && (glance_->anim_start || glance_->close_start); }
  void CloseGlance();
  // #animateGlanceClosing: the glance shrinks back into its link first
  // (Escape, the close button, a click beside it); immediate without one.
  void CloseGlanceAnimated();
  bool glance_closing() const { return glance_ && glance_->close_start != 0; }
  void ExpandGlance();
  void SplitGlance();
  bool IsGlanceTab(const Tab* tab) const { return glance_ && glance_->tab == tab; }
  Tab* glance_tab() const { return glance_ ? glance_->tab : nullptr; }
  D2D1_RECT_F GlanceRect() const;  // DIPs

  // Library panel (ZenLibrary, Alt+Shift+L, the footer's first button): an
  // orbit://library/ page that slides in from the left over the sidebar,
  // pushing the page right (openProgress). Escape closes it.
  void ToggleLibrary();
  void OpenLibrary();
  // ZenLibrary.toggle(section): opens on |section| (spaces / downloads /
  // history); already open on it, it closes.
  void ToggleLibrarySection(const std::wstring& section);
  void CloseLibrary();
  bool library_open() const { return library_open_; }
  int library_browser_id() const;
  CefRefPtr<CefBrowser> library_browser() const { return library_browser_; }  // tests
  void OnLibraryCreated(CefRefPtr<CefBrowser> browser);
  void OnLibraryClosed(CefRefPtr<CefBrowser> browser);
  bool library_alive() const {
    return library_browser_ != nullptr || library_creating_ || site_browser_ != nullptr || site_creating_ ||
           theme_browser_ != nullptr || theme_creating_ || boost_browser_ != nullptr || boost_creating_;
  }
  void LibraryShowSpace(int64_t space_id, int tab_index);
  void SetSpaceColor(uint32_t color);  // the current space's theme colour
  // ZenMediaController: the tab whose media card shows (null: none).
  Tab* media_card_tab() const { return media_tab_; }
  void MediaCommand(const char* command);  // "toggle", "seek:<fraction>", "mute", "focus", "close"

  // Site data panel (ZenSiteDataPanel): the site-info button opens a popup
  // with the page's security, its permissions and site-data actions.
  void OpenSiteData();
  void CloseSiteData();
  void ToggleBookmarkActive() { ToggleBookmark(); }  // the panel's Bookmark button
  bool site_data_open() const { return site_popup_ && ::IsWindowVisible(site_popup_); }
  int site_data_browser_id() const { return site_browser_ ? site_browser_->GetIdentifier() : 0; }
  CefRefPtr<CefBrowser> site_data_browser() const { return site_browser_; }  // tests
  std::wstring site_data_title() const;  // tests
  void OnSiteDataCreated(CefRefPtr<CefBrowser> browser);
  void OnSiteDataClosed(CefRefPtr<CefBrowser> browser);

  // Theme picker (nsZenThemePicker, "Edit Theme…"): a popup beside the
  // sidebar hosting orbit://theme/ for one space, shown without switching to it.
  void OpenThemePicker(Workspace* ws);
  void CloseThemePicker();
  bool theme_picker_open() const { return theme_popup_ && ::IsWindowVisible(theme_popup_); }
  int theme_picker_browser_id() const { return theme_browser_ ? theme_browser_->GetIdentifier() : 0; }
  CefRefPtr<CefBrowser> theme_picker_browser() const { return theme_browser_; }  // tests
  std::wstring theme_picker_title() const;
  Workspace* theme_editing_space() const;
  void OnThemePickerCreated(CefRefPtr<CefBrowser> browser);
  void OnThemePickerClosed(CefRefPtr<CefBrowser> browser);

  // Boost editor (ZenBoostsEditor): a panel by the site-info button for the
  // active tab's site; it stays while the page is clicked (zap / focus).
  void OpenBoostEditor();
  void CloseBoostEditor();
  bool boost_editor_open() const { return boost_popup_ && ::IsWindowVisible(boost_popup_); }
  int boost_editor_browser_id() const { return boost_browser_ ? boost_browser_->GetIdentifier() : 0; }
  CefRefPtr<CefBrowser> boost_editor_browser() const { return boost_browser_; }  // tests
  std::wstring boost_editor_title() const;
  const std::wstring& boost_domain() const { return boost_domain_; }
  Tab* boost_tab() const;
  // The page picker: "zap", "focus" or "off".
  void SetBoostPickMode(const std::wstring& mode);
  const std::wstring& boost_pick_mode() const { return boost_pick_mode_; }
  void OnBoostPicked(Tab* tab, const std::wstring& mode, const std::wstring& selector);
  void OnBoostEditorCreated(CefRefPtr<CefBrowser> browser);
  void OnBoostEditorClosed(CefRefPtr<CefBrowser> browser);
  void LibraryNewSpace();
  // The panes on screen (DIPs) and the dividers between them; empty
  // when the current tab is not in a split.
  struct PaneRect {
    Tab* tab = nullptr;
    D2D1_RECT_F rect{};
  };
  std::vector<PaneRect> SplitPanes(std::vector<SplitHandle>* handles = nullptr) const;
  // G1: the space-switch slide is running (tests and diagnostics).
  bool space_switch_animating() const;
  bool rows_animating() const { return row_anim_.Running(::GetTickCount64()); }
  // The URL bar is open and floats in the middle (not in the sidebar).
  bool url_bar_floating() const { return popup_visible_ && popup_anchor_ == PopupAnchor::kCentered; }
  bool url_bar_open() const { return popup_visible_; }
  void ChooseUrlResult(int index) { ActivateUrlResult(index); }  // as Enter on that row
  bool greyed_out() const { return grey_out_inactive_ && !window_active_; }
  bool tabs_on_right() const { return right_side_; }
  // zen.view.use-single-toolbar = false ("Multiple toolbars", and Zen's
  // "Collapsed toolbar"): navigation and the URL field live in a toolbar
  // above the page, with the window controls; the sidebar holds the space.
  bool multi_toolbar() const { return multi_toolbar_ && !chrome_hidden(); }
  D2D1_RECT_F toolbar_rect() const { return toolbar_; }  // tests
  D2D1_RECT_F extensions_button_rect() const { return ext_btn_; }  // tests
  std::vector<std::string> pinned_extension_ids() const { return ext_pin_ids_; }  // tests
  D2D1_RECT_F back_button_rect() const { return back_; }  // tests
  D2D1_RECT_F essentials_rect() const { return essentials_rect_; }  // tests
  std::vector<D2D1_RECT_F> essential_tile_rects() const { return EssentialRects(); }  // tests
  void ForceLayoutForTest() { Layout(); }  // tests
  D2D1_RECT_F address_rect() const { return address_rect_; }  // tests
  bool reordering_spaces() const { return reordering_spaces_; }
  std::vector<D2D1_RECT_F> space_icon_rects() const { return workspace_dots_; }  // tests (DIPs)
  // ZenSpacesSwipe: horizontal scrolling over the sidebar drags the space.
  float swipe_offset() const;  // DIPs the strip is dragged by now (tests)
  bool swiping() const { return swiping_; }
  const std::wstring& last_share_url() const { return last_share_url_; }  // self-tests
  POINT toast_button_center() const { return toast_ ? toast_->button_center() : POINT{-1, -1}; }
  HWND toast_hwnd() const { return toast_ ? toast_->hwnd() : nullptr; }
  // The gradient picker's result for |ws| (applied live; |save| stores it).
  void SetSpaceTheme(Workspace* ws, const std::optional<orbit::SpaceTheme>& theme, bool save);
  bool space_themed() const { return backdrop_.themed; }
  D2D1_COLOR_F backdrop_at(float x, float y) const { return BackdropAt(x, y); }  // tests
  bool sidebar_flashing() const { return CompactHidesSidebar() && sidebar_revealed_ && flash_reveal_; }
  // zen.view.compact.hide-tabbar / hide-toolbar: with multiple toolbars,
  // compact mode hides the sidebar, the toolbar or both.
  bool CompactHidesSidebar() const { return compact_mode_ && (compact_hide_sidebar_ || !multi_toolbar()); }
  bool CompactHidesToolbar() const { return compact_mode_ && compact_hide_toolbar_ && multi_toolbar(); }
  // The toolbar shows (always, or revealed from the top edge when hidden).
  bool toolbar_shown() const { return multi_toolbar() && (!CompactHidesToolbar() || caption_revealed_); }
  void SetCompactHides(bool sidebar, bool toolbar);
  // Self-tests: the indent and pinned separator transitions.
  bool indent_animating() const { return !indent_from_.empty() && indent_anim_.Running(::GetTickCount64()); }
  float pinned_separator_shown() const { return PinnedSeparatorShown(); }
  void SetTabPinned(Tab* tab, bool pinned);
  // "Move to folder" as the tab menu does it (with the row tween).
  void MoveTabToFolderAnimated(Tab* tab, int64_t folder);
  // ZenDownloadAnimation (zen.downloads.download-animation, pref
  // "download_animation"): a download started from a click in this window.
  void AnimateDownload();
  void AnimateDownloadFrom(POINT screen);  // self-tests: an explicit start
  bool download_animating() const { return download_flight_ && download_flight_->flying(); }
  bool download_box_visible() const { return download_flight_ && download_flight_->box_visible(); }
  // ZenFolders' folder menu actions (public for the self-tests).
  void UnloadFolder(int64_t group_id);
  void FolderToSpace(int64_t group_id);
  void MoveFolderToSpace(int64_t group_id, Workspace* target);
  void UnpackFolder(int64_t group_id);
  void DeleteFolder(int64_t group_id);
  // ZenLiveFoldersManager.createFolder from the "New Live Folder" menu (RSS
  // asks for the feed URL).
  void NewLiveFolder(LiveFolderState::Kind kind);
  void DropFolderPlaceholder(int64_t group_id);  // a real tab joined it
  float sidebar_left() const { return SidebarLeft(); }  // tests
  void ToggleTabsOnRight();
  Tab* active_essential() const { return active_essential_; }

  // Applies the user's shortcut overrides, then DispatchAccelerator.
  bool HandleAccelerator(int vk, bool ctrl, bool shift, bool alt);
  bool AddressBarHasFocus() const { return address_ && ::GetFocus() == address_; }
  // Zen commands (also reachable from shortcuts and controls).
  // markdown: "[title](url)" (copyCurrentURLAsMarkdownToClipboard).
  bool CopyCurrentUrl(bool markdown = false);
  bool ClearUnpinnedTabs();
  void GoBack();               // Back, or closes an owned tab with no history
  bool BackClosesTab() const;  // zen.tabs.close-on-back-with-no-history
  void TogglePinActiveTab();   // Ctrl+Shift+D
  // Zen pinned tab manager.
  void CloseTabShortcut(Tab* tab);  // Ctrl+W, the tab's close/unload button
  void PinnedCloseShortcut(Tab* tab, bool close_if_unloaded);
  void ResetPinnedTab(Tab* tab, bool separate_current_page);
  void ReplacePinnedUrlWithCurrent(Tab* tab);
  // Returns false (and shows Zen's "invalid URL" toast) for input that is
  // not an address.
  bool SetPinnedUrlFromText(Tab* tab, const std::wstring& text);
  // Inline renaming (ZenTabRenamer): Enter commits, Escape or blur cancels.
  void StartRenameTab(Tab* tab);
  void StartRenameWorkspace(Workspace* ws);
  bool renaming() const { return rename_edit_ && ::IsWindowVisible(rename_edit_); }
  void CommitRename();
  void CancelRename();
  // Spaces.
  void SetWorkspaceIcon(Workspace* ws, const std::wstring& icon);
  int UnloadWorkspaceTabs(Workspace* ws);       // "Unload Space"
  int UnloadOtherWorkspaces(Workspace* keep);   // "Unload All Other Spaces"
  // Address bar input, as the user produces it (also used by self-tests).
  void OpenLocation();                         // Ctrl+L: the floating URL bar
  void TypeInAddressBar(const std::wstring& text);
  void SubmitAddressBar();                     // Enter
  void CancelAddressBar();                     // Escape
  void FocusAddressBarForTest() { FocusAddressBar(); }  // like a click in the sidebar field
  bool floating_url_bar_open() const { return popup_visible_; }
  bool new_tab_mode() const { return popup_visible_ && new_tab_mode_; }
  // The urlbar view's rows for the current input (self-tests read these).
  const std::vector<OmniboxRow>& url_results() const { return results_; }
  int first_visible_url_result() const { return results_first_; }
  std::wstring address_text() const;  // what the address field shows (tests)
  void MoveUrlSelectionForTesting(int delta) { MoveUrlResultSelection(delta); }
  // Search suggestions for |text| as if the engine had answered (tests).
  void SetSearchSuggestionsForTesting(const std::wstring& text, std::vector<std::wstring> list);
  int selected_url_result() const { return selected_result_; }
  bool url_bar_breakout() const { return popup_visible_ && popup_anchor_ == PopupAnchor::kSidebar; }
  // The id of the toast currently shown, or empty (self-tests).
  std::wstring visible_toast_id() const { return toast_ ? toast_->visible_id() : L""; }
  // A page (|tab|) entered or left HTML fullscreen (a video's fullscreen button).
  void SetContentFullscreen(Tab* tab, bool fullscreen);
  // DevTools moved to another side (or their own window): lay the page out again.
  void OnDevToolsDockChanged() { ShowContent(); }
  // Leaves page fullscreen: the page is told, the window comes back.
  void ExitContentFullscreen();
  bool content_fullscreen() const { return content_fullscreen_; }
  Tab* fullscreen_tab() const { return fullscreen_tab_; }
  // Sidebar and toolbar hidden (page or window fullscreen); self-tests.
  bool chrome_hidden_for_test() const { return content_fullscreen_ || window_fullscreen_; }
  void BeginClose();
  void ApplyTheme();
  // Collapsed sidebar: icons only (Zen's "zen.view.sidebar-expanded" off).
  void SetCompact(bool compact);
  // Compact mode: the sidebar is hidden and slides in from the left edge.
  void SetCompactMode(bool hidden);
  bool compact_mode() const { return compact_mode_; }

  // UI Automation (see ui/accessibility.h).
  std::vector<AccessibleItem> AccessibleItems() const;
  bool ExtensionsButtonShown() const;
  void ShowExtensionsMenu();
  // Lays out the extensions button and the pinned extensions from |x|
  // (left to right) while they fit before |limit|; returns where they end.
  float LayoutExtensionButtons(float x, float y, float limit);
  float LayoutPinnedExtensions(float x, float y, float limit);  // the pins only
  const ExtensionInfo* PinnedExtension(int index) const;
  void OpenExtensionAction(const ExtensionInfo& extension, const D2D1_RECT_F& anchor);
  void AccessibleInvoke(int kind, intptr_t key);  // posted to the window
  void AccessibleFocus(int kind, intptr_t key);
  void OnDownloadsChanged();
  void OnBookmarksChanged();
  // The profile's extension list changed: the extensions button appears or goes.
  void OnExtensionsChanged();
  // Zen's toast in the window's top-right corner.
  void ShowToast(ui::Toast::Content content);
  // Opens (or switches to) orbit://settings/.
  void OpenSettingsPage();

 private:
  enum class HitKind {
    kNone,
    kBack,
    kForward,
    kReload,
    kSiteInfo,
    kStar,
    kCopyUrl,
    kExtensions,      // the extensions button: lists extension actions
    kExtensionPin,    // a pinned extension's toolbar button (index into ext_pin_ids_)
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
    kTabResetPin,  // the pinned tab's "back to pinned url" button
    kEssential,    // index = position in essentials()
    kGroup,  // index = position in workspace()->groups()
    kSidebarEdge,
    kTabListEmpty,  // empty space below the tabs (double-click opens a tab)
    kSplitHandle,   // a split view divider; index into SplitGeometry's handles
    kMediaFocus,    // media card: show its tab
    kMediaPlayPause,
    kMediaMute,
    kMediaClose,
    kMediaSeek,
    kLiveError,  // a live folder's error label; index = position in groups()
    kLinked,      // a linked window's folder row; index = position in LinkedChildren()
    kLinkedBack,  // the linked window's "back to the parent" arrow
    kDevToolsEdge,  // the divider between the page and its DevTools
  };
  struct Hit {
    HitKind kind = HitKind::kNone;
    int index = -1;
    bool operator==(const Hit& o) const { return kind == o.kind && index == o.index; }
  };
  enum InternalPage { kPageDownloads, kPageBookmarks, kPageHistory, kPageSettings, kPageExtensions };

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  static LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  static LRESULT CALLBACK GlanceDimProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  static LRESULT CALLBACK GlanceBarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  static LRESULT CALLBACK AddressProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                      UINT_PTR id, DWORD_PTR ref);
  static LRESULT CALLBACK RenameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                     UINT_PTR id, DWORD_PTR ref);
  void PositionRenameEditor();
  void DrawWorkspaceIcon(ID2D1RenderTarget* rt, const std::wstring& icon, const D2D1_RECT_F& r,
                         float size, float opacity);
  LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
  LRESULT HitTestNonClient(LPARAM lp);

  // Layout (DIPs unless noted).
  void Layout();
  void PlaceContent();  // moves the active tab's host (reveal offsets)

  // Compact mode (fix 6): the revealed sidebar floats over the page in a
  // child window stacked above the tab hosts, since a page is a child HWND
  // the main window cannot paint over. It paints with PaintSidebar (on its
  // own device resources) and forwards mouse input to HandleMessage in
  // main-window coordinates, so hit-testing and commands stay shared.
  bool overlay_shown() const;
  HWND SidebarHost() const;  // parent of the address and rename EDITs
  POINT OverlayOriginPx() const;  // the card's resting position (main client px)
  RECT ToHostPx(const D2D1_RECT_F& r) const;  // ToPx relative to SidebarHost()
  void UpdateOverlay();  // after Layout: show/hide, size, stacking, EDIT parents
  void MoveOverlay();    // per animation frame (compact slide)
  void PaintOverlay();
  LRESULT ForwardOverlayMouse(UINT msg, WPARAM wp, LPARAM lp);
  float Width() const;
  float Height() const;
  // Width the sidebar takes from the content (0 in compact mode).
  float SidebarWidth() const;
  // Width of the sidebar as drawn (the revealed panel in compact mode).
  float SidebarPanelWidth() const;
  bool sidebar_visible() const;
  // zen.tabs.vertical.right-side: the sidebar's left edge (0 on the left).
  float SidebarLeft() const;
  bool InSidebar(float x) const;
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
    int64_t split = 0;      // a split view's row: `tab` is its first member
    int depth = 0;          // folder nesting (indentation)
    int64_t linked = 0;     // a linked window space's row (its id); `tab` is -1
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
  void PaintSpaceArea(ID2D1RenderTarget* rt);  // indicator + tab list of workspace_
  void PaintSplitRow(ID2D1RenderTarget* rt, const Row& row, const D2D1_RECT_F& rect,
                     bool focused);
  // Shows the active tab, or every pane of its split, and hides what was
  // shown before (the single place that calls Tab::Show).
  void ShowContent();
  const Workspace::SplitView* ActiveSplit() const;
  int SplitMemberCount(const Row& row) const;
  D2D1_RECT_F SpaceArea() const;
  void PaintGradient(ID2D1RenderTarget* rt, const ui::Backdrop& backdrop, float opacity);
  void PaintThemeLayers(ID2D1RenderTarget* rt, const orbit::ThemePaint& paint, float opacity);
  ID2D1BitmapBrush* GrainBrush(ID2D1RenderTarget* rt);
  Microsoft::WRL::ComPtr<ID2D1BitmapBrush> grain_brush_;
  ID2D1RenderTarget* grain_rt_ = nullptr;
  D2D1_COLOR_F Greyed(const D2D1_COLOR_F& c) const;  // grey-out of inactive windows
  ui::Palette BasePalette() const;  // the app's (or the private window's) palette
  // Snapshots the current space before ShowWorkspace replaces it; `step` is
  // SpaceSwitchStep's direction (+1: the new space comes in from the right).
  void BeginSpaceSwitchAnimation(int step);
  void UpdateAnimationTimer(uint64_t now);
  // Row tween (ZenFolders.animateCollapse/animateExpand, 0.18s ease-in-out):
  // call CaptureRows before a change to the list and AnimateRows after it.
  void CaptureRows();
  void AnimateRows();
  using RowKey = std::pair<bool, intptr_t>;  // {group header, group id or Tab*}
  RowKey KeyOf(const Row& row) const;
  void PaintUrlBar(ID2D1RenderTarget* rt);
  void PaintFooter(ID2D1RenderTarget* rt);
  void PaintMediaCard(ID2D1RenderTarget* rt);
  void UpdateMediaCard();  // picks the tab (called on tab changes and selection)
  void PaintTabRow(ID2D1RenderTarget* rt, int index, const D2D1_RECT_F& row, bool focused,
                   int64_t group_id);
  void PaintGroupRow(ID2D1RenderTarget* rt, const Row& row, const D2D1_RECT_F& rect,
                     bool focused);
  void PaintLinkedRow(ID2D1RenderTarget* rt, const Row& row, const D2D1_RECT_F& rect,
                      bool focused);
  int LinkedIndex(int64_t linked_id) const;  // in LinkedChildren(), or -1
  void ShowLinkedMenu(Workspace* linked, POINT screen);
  void PaintIconButton(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, wchar_t glyph, Hit hit,
                       bool enabled, const D2D1_COLOR_F* color = nullptr);
  void PaintSoftShadow(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, float radius,
                       float spread, float offset_y, float alpha);
  void PaintPopup();
  // |gray|: the black-and-white icon a sleeping tab shows.
  ID2D1Bitmap* FaviconBitmap(Tab* tab, bool gray = false);
  ID2D1Bitmap* PinnedFaviconBitmap(Tab* tab);
  struct CachedFavicon {
    uint32_t generation = 0;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    uint32_t gray_generation = 0;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> gray;  // sleeping tabs: black and white
  };
  ID2D1Bitmap* BitmapFor(const FaviconPixels& px, CachedFavicon& cached, bool gray = false);
  void RefreshBackdrop();  // workspace color or theme strength changed
  D2D1_COLOR_F BackdropAt(float x, float y) const;
  D2D1_COLOR_F UrlFieldColor() const;  // opaque, so the EDIT can match it
  void Invalidate();
  void UpdateSpinnerTimer();
  bool CanCopyUrlFromButton() const;
  void ShareUrl(const std::wstring& url, const std::wstring& title);
  std::wstring last_share_url_;

  // Input.
  void OnMouseMove(float x, float y, WPARAM keys);
  void OnLButtonDown(float x, float y);
  void OnLButtonUp(float x, float y);
  void OnLButtonDoubleClick(float x, float y);
  void OnContextMenu(int screen_x, int screen_y, float x, float y);
  void OnMouseWheel(int delta);
  void OnMouseHWheel(int delta, float x);
  bool OnKeyDown(int vk);
  void Click(const Hit& hit);
  void ShowTabMenu(Tab* tab, POINT screen);
  void ShowWorkspaceMenu(int index, POINT screen);
  void ShowGroupMenu(int64_t group_id, POINT screen);
  void ShowEssentialMenu(Tab* tab, POINT screen);
  // Shows |tab| wherever it lives: its space (switching here, or in the
  // window that shows it) or this window's Essentials.
  void RevealTab(Tab* tab);
  std::vector<D2D1_RECT_F> EssentialRects() const;
  void PaintEssentials(ID2D1RenderTarget* rt);
  void ToggleGroup(int64_t group_id);
  void NewGroupWithTab(Tab* tab);
  void NewSubfolder(int64_t parent_id);  // asks for the name
  // The live folder menu ("Live Folder Options") and its error button.
  void AppendLiveFolderMenu(HMENU menu, int64_t group_id);
  void RunLiveFolderCommand(int64_t group_id, UINT cmd);
  void LiveErrorAction(int64_t group_id);
  D2D1_RECT_F LiveErrorRect(const Row& row) const;  // empty without an error
  void ShowProfileMenu(POINT screen);
  void ShowMainMenu(POINT screen);
  void ShowCreateMenu(POINT screen);
  void UpdateTooltip(const Hit& hit);
  std::wstring TooltipText() const;

  // Commands.
  void NewTab();
  void OpenEmptySpaceTab();
  void DropEmptyPlaceholder(Tab* except);
  void CloseTab(Tab* tab);
  void CloseOtherTabs(Tab* keep);
  void CloseTabsBelow(Tab* tab);

  void DuplicateTab(Tab* tab);
  void TogglePin(Tab* tab);
  // A space of this window's list (any profile, or the private profile's).
  Workspace* FindSpace(int64_t id) const;
  // The tab containers whose tabs this window can switch to (URL bar
  // results, the media card): every normal profile's, or its private one's.
  std::vector<Workspace*> TabContainers() const;
  // A space's name in menus, with its profile when that is not the default.
  std::wstring SpaceLabel(const Workspace* ws) const;
  void OnProfileChanged();  // the current space belongs to another profile now
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
  Tab* TabToSelectOnClose(Tab* closing) const;
  bool PrefEnabled(const wchar_t* key, bool fallback) const;
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
  // |new_tab|: Zen's replace-newtab mode, where Enter opens a new tab and
  // closing the bar creates nothing.
  enum class PopupAnchor {
    kCentered,  // Zen's floating URL bar
    kSidebar,   // #urlbar[breakout-extend]: grown in place from the sidebar field
  };
  void ShowFloatingUrlBar(bool new_tab, bool select_all = true,
                          PopupAnchor anchor = PopupAnchor::kCentered);
  void UpdateUrlResults();
  void MoveUrlResultSelection(int delta);
  void ActivateUrlResult(int index);
  int UrlResultAt(float y) const;      // popup client DIPs -> row index or -1
  float UrlBarInputHeight() const;
  void NavigateTo(const std::wstring& url);  // the address bar's navigation, for a fixed-up URL
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
  HWND rename_edit_ = nullptr;
  HFONT rename_font_ = nullptr;
  HBRUSH rename_brush_ = nullptr;
  D2D1_COLOR_F rename_bg_{};
  Tab* rename_tab_ = nullptr;            // being renamed (or)
  Workspace* rename_workspace_ = nullptr;
  HWND tooltip_ = nullptr;
  std::unique_ptr<ui::Toast> toast_;
  std::unique_ptr<ui::DownloadFlight> download_flight_;
  bool flash_reveal_ = false;  // the sidebar is out for a flash, not a hover
  void FlashSidebarIfNecessary();
  HFONT address_font_ = nullptr;
  HFONT popup_font_ = nullptr;
  HBRUSH address_brush_ = nullptr;
  HBRUSH popup_brush_ = nullptr;
  UINT dpi_ = 96;

  Profile* profile_;
  Workspace* workspace_ = nullptr;
  Workspace* linked_space_ = nullptr;  // see linked_space()
  int drop_linked_ = -1;                // a dragged tab is over this linked row
  D2D1_RECT_F linked_back_{};           // the linked window's back arrow
  // An Essential on screen instead of the space's own active tab, and per
  // space the Essential that was selected when the user left it.
  Tab* active_essential_ = nullptr;
  std::unordered_map<const Workspace*, Tab*> last_essential_;
  mutable std::unordered_map<const Workspace*, bool> pins_collapsed_;  // cache of the stored state
  ui::Palette palette_;
  ui::Backdrop backdrop_{};
  // Settings read by ApplyTheme (refreshed whenever settings change).
  bool newtab_button_ = true;       // zen.tabs.show-newtab-vertical
  bool newtab_button_top_ = true;   // zen.view.show-newtab-button-top
  std::wstring urlbar_behavior_ = L"floating-on-type";  // zen.urlbar.behavior
  bool ctrl_tab_skip_pending_ = false;  // zen.tabs.ctrl-tab.ignore-pending-tabs
  bool grey_out_inactive_ = true;       // zen.view.grey-out-inactive-windows
  bool urlbar_wide_ = false;            // multiple toolbars: the URL field up to 800 (else 560)
  bool right_side_ = false;             // zen.tabs.vertical.right-side
  // Media card state.
  Tab* media_tab_ = nullptr;
  Tab* media_dismissed_ = nullptr;      // closed by the user until it plays again
  ULONGLONG media_dismissed_start_ = 0;
  ULONGLONG media_hidden_until_ = 0;    // 500 ms after leaving the playing tab
  D2D1_RECT_F media_card_{};
  bool window_active_ = true;
  ULONGLONG last_space_scroll_ = 0;     // Ctrl+wheel cooldown (200 ms)
  void SetWindowActive(bool active);
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
  bool resizing_devtools_ = false;
  float devtools_width_ = 440.f;  // DIPs (pref "devtools_width")
  // The page and DevTools rects within |content| (px), when the active tab
  // shows its DevTools; false otherwise.
  bool DevToolsLayout(const RECT& content, RECT* page, RECT* tools) const;
  bool tracking_mouse_ = false;
  bool tracking_nc_mouse_ = false;
  Hit tooltip_hit_;

  // Address bar.
  bool address_editing_ = false;
  bool bookmarked_ = false;  // active tab's URL is bookmarked (cached for painting)
  bool address_programmatic_ = false;
  bool popup_visible_ = false;
  bool new_tab_mode_ = false;
  PopupAnchor popup_anchor_ = PopupAnchor::kCentered;
  std::vector<OmniboxRow> results_;
  int selected_result_ = 0;
  int results_first_ = 0;      // the first row shown (#urlbar-results scrolls)
  bool filling_input_ = false;
  std::wstring typed_text_;    // what the user typed (selection fills the input)
  std::wstring suggest_text_;  // the input the suggestions below are for
  std::vector<std::wstring> suggest_results_;
  std::wstring suggest_pending_;
  void RequestSearchSuggestions(const std::wstring& text);
  void FillInputFromSelection();
  void ScrollUrlResults(int rows);
  int hover_result_ = -1;
  bool new_tab_navigated_ = false;
  // zen.urlbar.wait-to-clear: text typed in a dismissed new-tab bar comes
  // back if the bar reopens within 45 s.
  std::wstring last_new_tab_search_;
  ULONGLONG last_new_tab_search_at_ = 0;
  Tab* focus_guard_tab_ = nullptr;  // see OnContentFocused
  ULONGLONG focus_guard_until_ms_ = 0;

  // Fullscreen.
  bool content_fullscreen_ = false;
  Tab* fullscreen_tab_ = nullptr;  // the page in HTML fullscreen
  bool window_fullscreen_ = false;
  WINDOWPLACEMENT saved_placement_{sizeof(WINDOWPLACEMENT)};
  LONG_PTR saved_style_ = 0;

  // Spinner animation (only runs while a visible tab is loading).
  bool spinner_running_ = false;
  float spinner_phase_ = 0.f;

  // G1 animations (ANIMATION_INVENTORY), timer-driven while one runs; all
  // durations are 0 when Windows animations are off (SPI_GETCLIENTAREAANIMATION).
  bool anim_timer_running_ = false;
  orbit::Transition ws_actions_fade_;  // .zen-workspaces-actions opacity, 0.1s
  orbit::Transition splitter_fade_;    // #zen-sidebar-splitter, 0.2s after 0.2s
  // ZenSpaceManager._animateTabs: the old space slides out as a snapshot,
  // the new one slides in, and the backdrop crossfades (spring, 250ms).
  orbit::Transition space_slide_{1.f};
  Microsoft::WRL::ComPtr<ID2D1Bitmap> space_snapshot_;
  D2D1_RECT_F space_snapshot_rect_{};
  ui::Backdrop space_old_backdrop_{};
  int space_step_ = 0;
  // Split view: tabs whose hosts are on screen, and a divider drag.
  std::vector<Tab*> content_shown_;
  bool painting_split_segment_ = false;
  struct SplitDrag {
    int64_t split_id = 0;
    std::vector<int> path;
    int index = 0;
    bool vertical = true;
    std::vector<float> start;
    float origin = 0.f;  // pointer position along the axis (DIPs)
    float extent = 1.f;  // the node's size along the axis, without gaps (DIPs)
  };
  std::optional<SplitDrag> split_drag_;
  // Glance: the previewed tab and the tab it opened from. The parent fades
  // behind a layered child window (opacity 0.3 over the backdrop, as
  // #animateParentBackground); the buttons are a child window beside it.
  struct GlanceState {
    Tab* parent = nullptr;
    Tab* tab = nullptr;
    // The opening arc: from the link (window DIPs) while anim_start != 0.
    D2D1_RECT_F from{};
    bool has_origin = false;
    uint64_t anim_start = 0;
    uint64_t close_start = 0;  // closing back into the link
  };
  D2D1_RECT_F GlanceRectNow(uint64_t now) const;  // GlanceRect(), or on its way there
  std::optional<GlanceState> glance_;
  HWND glance_dim_ = nullptr;
  HWND glance_bar_ = nullptr;
  int glance_bar_hover_ = -1;
  void EnsureGlanceWindows();
  void PlaceGlanceChrome(bool show);
  void PaintGlanceBar(HDC dc);
  int GlanceButtonAt(int x, int y) const;  // bar client px -> 0 close, 1 expand, 2 split
  // Library panel state.
  HWND library_host_ = nullptr;
  CefRefPtr<CefBrowser> library_browser_;
  CefRefPtr<class LibraryClient> library_client_;
  bool library_creating_ = false;
  bool library_open_ = false;
  orbit::Transition library_slide_;
  HWND site_popup_ = nullptr;
  CefRefPtr<CefBrowser> site_browser_;
  CefRefPtr<class LibraryClient> site_client_;
  bool site_creating_ = false;
  HWND theme_popup_ = nullptr;
  CefRefPtr<CefBrowser> theme_browser_;
  CefRefPtr<class LibraryClient> theme_client_;
  bool theme_creating_ = false;
  int64_t theme_space_id_ = 0;
  HWND boost_popup_ = nullptr;
  CefRefPtr<CefBrowser> boost_browser_;
  CefRefPtr<class LibraryClient> boost_client_;
  bool boost_creating_ = false;
  std::wstring boost_domain_;
  int boost_tab_browser_ = 0;
  std::wstring boost_pick_mode_ = L"off";
  static LRESULT CALLBACK SiteDataProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  float LibraryWidth() const;
  void PlaceLibrary();
  bool DispatchAccelerator(int vk, bool ctrl, bool shift, bool alt);  // default chords
  bool RunShortcutAction(const std::wstring& id);  // actions unbound by default
  // Quick actions in the URL bar (ZenUBGlobalActions): what applies now, and
  // running one. Ids are shortcut ids where they exist.
  std::vector<OmniboxAction> QuickActions() const;
  void RunQuickAction(const std::wstring& id);
  // A menu label with the action's current shortcut: "New tab\tCtrl+T".
  static std::wstring MenuLabel(const wchar_t* label, const wchar_t* action);
  // ZenCompactMode.animateCompactMode: the card slides in/out (0.1s). While
  // it slides out, compact_hiding_ keeps its layout alive.
  orbit::Transition compact_slide_;
  bool compact_hiding_ = false;
  // Rows moved by a fold: old offsets of rows that stay, rows that left
  // (drawn fading out where they were), and the old periphery offset.
  struct RowGhost {
    Row row;
    RowKey key;
    RowKey next;  // the first row after it that stays (its moving top clips the ghost)
  };
  orbit::Transition row_anim_{1.f};
  std::map<RowKey, float> row_from_;
  // --zen-tabbox-element-indent-transition: margin-inline-start 0.1s
  // ease-in-out; the folder depth each moved row had before.
  orbit::Transition indent_anim_{1.f};
  // The swipe: the strip follows the wheel (damped as Zen's), then switches
  // past SUCCESS_THRESHOLD or springs back.
  bool swiping_ = false;
  float swipe_dx_ = 0.f;
  float swipe_last_ = 0.f;
  orbit::Transition swipe_back_{0.f};
  float swipe_handoff_ = 0.f;  // progress the switch animation starts from
  void EndSwipe();
  std::map<RowKey, int> indent_from_;
  // .pinned-tabs-container-separator [hide-separator]: height 0.08s, opacity
  // 0.06s; 1 while the space has pinned tabs.
  orbit::Transition pinned_sep_{0.f};
  const Workspace* pinned_sep_space_ = nullptr;
  float PinnedSeparatorShown() const;  // 0..1 now
  std::vector<RowGhost> row_ghosts_;
  float periphery_from_ = 0.f;
  std::vector<Row> captured_rows_;
  std::vector<RowKey> captured_keys_;  // taken at capture: tabs may close before AnimateRows
  // zen-browser-container.css: the page card moves down for the window
  // controls with `transition: margin 0.15s ease-in-out`.
  orbit::Transition caption_reveal_;
  bool caption_moving_ = false;
  // #zen-workspaces-button toolbarbutton: hover background fades (0.2s).
  std::unordered_map<int, orbit::Transition> footer_fades_;

  // Direct2D resources (recreated on device loss).
  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> rt_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
  // The overlay's own target: Direct2D resources cannot cross HWND render
  // targets, so PaintOverlay swaps these in for rt_/brush_/favicon caches.
  HWND overlay_ = nullptr;
  bool overlay_visible_ = false;
  bool forwarding_mouse_ = false;  // a forwarded overlay message is being handled
  SIZE overlay_size_{};
  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> overlay_rt_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> overlay_brush_;
  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> popup_rt_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> popup_brush_d2d_;
  std::unordered_map<const Tab*, CachedFavicon> favicons_;
  std::unordered_map<const Tab*, CachedFavicon> pinned_favicons_;
  std::unordered_map<const Tab*, CachedFavicon> overlay_favicons_;
  std::unordered_map<const Tab*, CachedFavicon> overlay_pinned_favicons_;
  // Pinned extensions in the top bar (after the extensions button).
  std::vector<D2D1_RECT_F> ext_pin_rects_;
  std::vector<std::string> ext_pin_ids_;
  std::unordered_map<std::string, CachedFavicon> ext_icons_;
  std::unordered_map<std::string, CachedFavicon> overlay_ext_icons_;

  // Cached layout rects (DIPs).
  D2D1_RECT_F address_rect_{}, back_{}, forward_{}, reload_{}, site_info_{}, menu_{}, star_{},
      copy_url_{}, ext_btn_{},
      toggle_{}, caption_min_{}, caption_max_{}, caption_close_{}, ws_indicator_{},
      ws_actions_{}, essentials_rect_{}, list_{}, footer_{}, downloads_{}, create_new_{}, panel_{};
  std::vector<D2D1_RECT_F> workspace_dots_;
  int first_dot_workspace_ = 0;  // workspace index shown by workspace_dots_[0]
  // nsZenWorkspaceIcons.initDragAndDrop: pressing a space icon and moving
  // 5 px reorders the spaces live; letting go keeps the order (no switch).
  bool reordering_spaces_ = false;
  bool multi_toolbar_ = false;
  bool compact_hide_sidebar_ = true;
  bool compact_hide_toolbar_ = false;
  D2D1_RECT_F toolbar_{};
  void PaintToolbar(ID2D1RenderTarget* rt);
  HWND AddressHost() const;  // the address field stays in the toolbar over the compact card
};

}  // namespace orbit
