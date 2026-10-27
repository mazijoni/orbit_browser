# Zen parity: implementation log

Read this first when resuming. Then read plan.md, the four inventories in this folder, and the "Next actions" list at the end.

## How to build and test

```powershell
pwsh ./scripts/build.ps1                 # Debug + unit tests (ctest)
pwsh ./scripts/build.ps1 -Config Release
pwsh ./scripts/run_selftests.ps1         # 7 in-browser suites
pwsh ./tools/uia_check.ps1               # accessibility via Windows UI Automation
```

No version control: the folder is not a git repository. Before each stage, copy the sources (excluding `out`, `third_party` and `dist`) to a backup folder.

## 2026-10-21: Session 1 (before plan.md was rewritten)

Done without the inventories. Recorded here so it can be audited:
- Renamed Strata → Orbit everywhere: binaries, `orbit://` scheme, `%LOCALAPPDATA%\Orbit`, window classes, docs. New icon (`tools/make_icon.py`). User data is **not** migrated from `%LOCALAPPDATA%\Strata`.
- Reworked the chrome to Zen's single-toolbar layout: frameless window, window controls revealed on hover, rounded content card, gradient backdrop, sidebar URL bar, space indicator, pinned separator with Clear, "+ New Tab" row, folders, footer switcher, floating URL bar, compact mode, collapsed sidebar, Zen-colored `orbit://` pages, and the settings "Space theme color" and "Compact mode".
- Files: `src/ui/theme.*`, `src/ui/graphics.*`, `src/ui/browser_window*.{h,cpp}`, `src/browser/tab.*` (rounded region), `src/browser/browser_client.*` (OnSetFocus), `src/browser/internal_page_content.cpp`, `src/browser/internal_pages_ui.cpp`, `src/browser/internal_api.cpp`, `src/app/application.*`.
- Tests: unit tests pass; 7/7 self-test suites pass (Debug and Release); UIA check passes after fixing the address-bar name (z-order after reparenting). Manual on-screen checks: layout, caption reveal, floating bar typing + Enter navigation, compact reveal, collapsed sidebar, dark/light, pinned + folders.
- Bugs found and fixed: the floating bar was closed by a new browser's creation focus (focus guard); a stale tooltip stayed in the caption strip; the address EDIT lost its accessible name after reparenting.

## 2026-10-21: Session 2, Stage A (discovery) per plan.md

Investigated:
- The Zen reference at `541ee55` (`dev`, tag 1.23.4b). Modules sized and surveyed: spaces, common/UI manager, library, boosts, split-view, folders, tabs/pinned manager, sessionstore, urlbar, glance, drag-and-drop, live-folders, sync, compact-mode, kbs, space-routing, welcome, share, media, mods, downloads, window-drag.
- Settings panes (`zen*.inc.xhtml`, `zen-settings.js`), all 180 `zen.*` prefs with defaults (`prefs/zen/*.yaml`), the default shortcuts with migrations to v21, the popups and context menus (`zen-panels/popups.inc`, `ZenPinnedTabManager`), and the English strings (`locales/en-US/.../zen-*.ftl`).
- The target: no TODOs or stubs in `src/`; all tests green (baseline above).

Audit of Session 1 against the reference (deviations to fix):
1. The sidebar URL shows host + path; the reference shows the domain only (`zen.urlbar.show-domain-only-in-sidebar=true`).
2. Compact mode is bound to Ctrl+Shift+C, which is Zen's **Copy URL**. Zen's compact toggle is Ctrl+S.
3. Space navigation lacks wrap-around (`zen.workspaces.wrap-around-navigation=true`) and Zen's Ctrl+Alt+←/→.
4. The compact keep-hover delay is 350ms; the reference is 150ms. The reference hides the top bar after 1000ms.
5. Ctrl+T creates a tab before the floating bar opens. The reference (`replace-newtab`) creates the tab only on Enter.
6. Compact mode pushes the page aside; the reference overlays it.
7. The default sidebar width is 220; the reference is 186 on Windows.
8. The create-new menu order differs: the reference is Create Space, Create Folder, New Split, New Tab.

Deliverables written: FEATURE_INVENTORY.md, SETTINGS_INVENTORY.md, KEYBOARD_SHORTCUTS.md, ANIMATION_INVENTORY.md.

## Roadmap (dependency order)

- **B1 Foundation:** toast surface (owned popup like the floating bar); animation clock (timer-driven, respecting reduced motion); a command table so shortcuts and menus share operations.
- **C1 Audit fixes 1–4 and 8** (small, low risk).
- **C2 Tab behaviors:** select recently used tab on close; close tab on Back with no history; Ctrl+Shift+K / D / C; copy URL with toast.
- **C3 Replace-newtab semantics** (fix 5) plus self-test updates.
- **D1 Pinned tabs:** pinned URL, reset, Replace with Current URL, Edit…, close-shortcut behavior (schema migration v-next).
- **D2 Essentials grid** (profile-level, max 12; schema migration).
- **D3 Space icons** (schema column already exists), wrap-around, unload space/others, rename tab, Change Label.
- **D4 URL-bar suggestions** from history, bookmarks, open tabs and actions.
- **E1 Settings:** Zen Look and Feel and Tab Management panes with real prefs; layout cards.
- **G1 Animations:** compact slide, space switch, pinned separator, hover fades.
- **H:** split view, glance, library panel, keyboard-shortcut editor, compact overlay (fix 6).

## 2026-10-21: Stage C1 (audit fixes 1–4 and 8)

Backup: scratchpad `backup_before_C1`.
- Fix 1: `orbit::SidebarDisplayUrl` (`src/core/url_fixup.*`) ports Zen's `trimURL` plus Firefox's `removeSingleTrailingSlashFromURL` (http/https/ftp only). Used by the sidebar field and by the field's text while the floating bar is open. 2 new unit tests.
- Fix 2: compact mode moved to Ctrl+S; Ctrl+Shift+C freed for Copy URL (C2).
- Fix 3: Ctrl+Alt+←/→ switch spaces with wrap-around. This is the only Ctrl+Alt exception, documented in the README. The invented Ctrl+Shift+PgUp/PgDn were removed.
- Fix 4: separate hide timers: window controls 1000ms (`toolbar-hide-after-hover`), compact sidebar 150ms (`sidebar-keep-hover`).
- Fix 8: "+" menu = Create Space, Create Folder, —, New Tab. Window commands stay in the main menu.
- Files: `src/core/url_fixup.{h,cpp}`, `tests/unit/test_url_fixup.cpp`, `src/ui/browser_window.cpp`, `src/ui/browser_window_paint.cpp`, README.md, docs/zen-parity/*.
- Tests: unit 31/31 OK; self-tests 7/7 OK; UIA check OK. Manual on screen: domain-only display (en.wikipedia.org/wiki/Orbit → en.wikipedia.org), Ctrl+Alt+→ ×3 wraps Home→Work→Study→Home, Ctrl+S toggles compact, "+" menu contents.
- Remaining from the audit: fix 5 (replace-newtab), fix 6 (compact overlay), fix 7 (default width 186).

## 2026-10-21: Session 3, slices 1–4

Backup: scratchpad `backup_before_S3`. New self-test suite `zen` (src/app/self_test.cpp `AddZenSuite`, run by scripts/run_selftests.ps1).

1. **Toasts + Copy URL.** `src/ui/toast.{h,cpp}`: a layered (per-pixel alpha) non-activating popup with Zen's geometry, gradient and timings (`ZenToasts.mjs`, `zen-popup.css`). `BrowserWindow::CopyCurrentUrl` (Ctrl+Shift+C, plus a copy button in the URL bar's page actions for http(s) pages).
   Bug found on screen, missed by the state-only test: the first `Show` sized the window before marking it visible, so it stayed 1×1 px. Fixed, and `Toast::visible_id` now requires real on-screen size so the self-test catches this. The screenshot helper needed `CAPTUREBLT` to include layered windows.
2. **Close all unpinned + batch undo.** Schema **v3** adds `closed_tabs.batch_id`. `Store::PopClosedTabBatch`; `Application::CloseTabsTogether` (used by Clear, Ctrl+Shift+K, close other tabs, close tabs below, close folder); `Application::ReopenClosedTab` restores the whole batch (Firefox `undoCloseTab` semantics). `ClearUnpinnedTabs` follows `#getClosableTabs` and shows the "Tabs Closed! Use Ctrl+Shift+T to undo." toast.
   The unit test `Schema_UpgradesFromV1KeepingData` failed because its v1 simulation now kept the v3 column; it now drops it. New tests `Schema_UpgradesFromV2KeepingClosedTabs` and `Store_ClosedTabBatchReopensTogether`.
3. **Tab owner and selection on close; close on Back; pin toggle.** `Tab::owner` (set for foreground tabs opened from a page or popup, cleared on switching away and when the owner leaves). `BrowserWindow::TabToSelectOnClose`: owner → MRU (awake first) → neighbour. `GoBack` closes owned tabs with no history. Ctrl+Shift+D toggles the pin. Settings → Tab Management: two toggles (`select_recent_on_close`, `close_on_back_no_history`). The tests use an activation order where MRU and the neighbour rule disagree, and also run with the setting off.
4. **replace-newtab and floating-on-type.** Ctrl+T / "+ New Tab" / double-click open the floating bar in new-tab mode (no tab until Enter; Escape or Ctrl+T again cancels; text kept 45 s). Ctrl+L / Alt+D / F6 open it for the current tab. Geometry follows `updateTabsToolbar`. An empty space opens a new-tab page with the bar (`OpenEmptySpaceTab`). The `basic` suite now types the URL into the bar and presses Enter.
   A regression caught by the suite: a fresh profile's window had no tab, because startup used `NewTab`. Fixed with `OpenEmptySpaceTab`.

Tests after slice 4: unit 33/33; self-tests 8/8 (basic, isolation, services, memory, security, zen, restore-1, restore-2); UIA check all OK. Manual on screen: toast (copy, clear with key pill), Clear + Ctrl+Shift+T restore, new-tab mode visuals (centered 750px bar, "+ New Tab" selected, previous tab deselected).

Known gaps from these slices: reopened tabs are appended (not put back at their index or folder); no Markdown copy (Ctrl+Shift+Alt+C collides with AltGr+Shift+C on some layouts); one toast at a time; no URL-bar results list yet.

## 2026-10-21: Session 3, slice 5 (pinned tab manager)

Backup: scratchpad `backup_before_S3_slice5`.
- Schema **v4**: `tabs.pinned_url`. Pre-v4 pinned tabs adopt their current URL on load. `core::PinnedUrlMatches` (fragment-insensitive, root-slash normalized) with a unit test; `Store_PinnedUrlRoundTrips`; both schema-upgrade tests updated.
- `Tab`: `SetPinnedUrl`, `pin_changed`, `pinned_favicon` (captured while the tab is at its pinned page), `ResetToPinned` (closes and recreates the browser through `kRecreateTabMessage`, outside the CEF close callback, so history is reset).
- `BrowserWindow`: `CloseTabShortcut` (Ctrl+W, close/unload button), `PinnedCloseShortcut` (all 6 Zen behaviors, plus the middle-click close-if-unloaded), `ResetPinnedTab` (Ctrl+click separates the current page first), `ReplacePinnedUrlWithCurrent`, `SetPinnedUrlFromText` (searches rejected with Zen's invalid-URL toast); pinned context-menu entries; reset-pin hit zone, hover sublabel, divider, unload glyph.
- Bug fixed before it shipped: resetting before unloading would have recreated the browser that the unload was meant to free; the order is unload, then reset.
- Defect found on screen, which predates this slice: the favicon of the previous page stayed when the new page had no icon. Fixed in `Tab::OnFaviconUrl`.
- Tests: unit 35/35; self-tests 8/8 (zen: pin, change, reset with fresh history, replace, edit validation, Ctrl+W reset-unload-switch, unpin; restore-1/2: pinned URL survives a restart); UIA OK. Manual: divider, hover icon + sublabel, unload button tooltip, reset click.

## 2026-10-21: Session 3, slice 6 (URL-bar results)

Backup: scratchpad `backup_before_S3_slice6`.
- `src/core/omnibox_results.{h,cpp}`: `BuildOmniboxRows` (heuristic, Switch to Tab, bookmarks, history; every word must match; dedupe by fragment-insensitive URL; current tab excluded) and `SearchEngineName`. 8 unit tests (`tests/unit/test_omnibox_results.cpp`).
- `BrowserWindow`: `UpdateUrlResults` (open tabs by MRU, bookmarks, `QueryHistory` with the longest word), `MoveUrlResultSelection` (Up/Down/Tab/Shift+Tab), `ActivateUrlResult` (switch tab across spaces, or navigate via `NavigateTo`, which now holds the shared navigation code), mouse hover/click in the popup; the popup grows to fit up to 5 rows. Typing in the sidebar field after a click opens the anchored breakout bar (`PopupAnchor::kSidebar`).
- Test fix: `tools/uia_check.ps1` read the title through `Process.MainWindowTitle`, which can pick another of the process's top-level windows (floating bar, toast). It failed once in five runs. It now reads the Orbit window's own UIA name: 4/4 OK.
- Lesson for manual checks: the capture helper's `SetForegroundWindow` takes focus from the floating bar, which then closes correctly. Send keys inside the same capture call.
- Tests: unit 43/43; self-tests 8/8 (zen: Switch to Tab via the arrow keys + Enter creates no tab; bookmark row + Enter navigates; breakout on typing; Escape closes); UIA 4/4. Manual: floating results (heuristic, two Switch to Tab rows, selected row), breakout with results.

## 2026-10-21: Session 3, slice 7 (rename, space icons, unload spaces)

Backup: scratchpad `backup_before_S3_slice7`.
- Schema **v5**: `tabs.custom_title` (`Tab::SetCustomTitle`; `DisplayTitle` prefers it). The unit-test schema simulations drop it; `Store_PinnedUrlRoundTrips` also round-trips the label.
- Inline renaming (`ZenTabRenamer` semantics): double-click on a tab label or on the space indicator, "Change Label…" and "Change Name…". The editor blends with the row (an EDIT cannot be transparent, so it is opaque in the row's colour over the backdrop).
- Space icons: "Change Icon…" with grapheme trimming (surrogates, ZWJ sequences, variation selectors); color emoji in the indicator, the footer switcher and the collapsed sidebar.
- Unload Space / Unload All Other Spaces; the space menu reordered to Zen's.
- Tests: unit 43/43; self-tests 8/8 (zen: rename commit / whitespace / toast / stored, Escape, empty resets, space rename, icon trimming + storage, unload space; restore-2: custom label survives restart); UIA 2/2. Manual: emoji icons, inline editor, renamed row + toast.

## 2026-10-21: Session 4, slice 8 (Essentials)

Backup: scratchpad `backup_before_S4_essentials`.
- Model: Essentials are a per-profile container (schema **v6** `workspaces.kind = 1`), loaded out of the space list by `Profile::LoadFromStore`; `Profile::containers()` now feeds every lifetime/save loop (close checks, profile deletion, SaveNow, sleep policy, diagnostics, BeginClose, omnibox). `Application::WindowsShowing` notifies every window of the profile when an Essential closes (an Essential has no single window).
- Window: `active_essential_` sits over the space's own active tab, which the space keeps; `last_essential_` restores a space's Essential on return; `OnTabMovedAway` (called from `Tab::Reparent`) makes a window fall back when another window takes the Essential it showed. Grid layout/painting per Zen; essential context menu; Add/Remove commands; pinned-close shortcut and middle click work on Essentials.
- Bugs found while reviewing the patch, before running it: owner pointers were not cleared when an active Essential closed (early return); `PinnedCloseShortcut` rejected Essentials (not in the space); a restore-test step used the wrong variable.
- Tests: unit 44/44; self-tests 8/8 including the new Essentials steps and the restart check; UIA 2/2; manual: grid with the selected tile.

## 2026-10-21: Session 4, fix 7 and the hidden empty tab

- Fix 7: default sidebar width 186 (ZenCustomizableUI, Windows). The buttons fit (left ends at 64, right starts at 93).
- Found on screen: a fresh window listed a "New Tab" row (the empty-space placeholder) beside the URL it was opened with. Zen's empty tab is hidden and replaced. Implemented `Tab::empty_placeholder` (see FEATURE_INVENTORY); `tools/uia_check.ps1` now expects 2 tab items for a fresh profile with two URLs.
- Tests: self-tests 8/8; UIA 2/2; zen suite standalone 4/4.
- Known test fragility: in one standalone `zen` run 9 focus-dependent checks failed (breakout, rename) while the same binary passed in the next five runs. Those steps call `SetFocus`/`SetForegroundWindow`; Windows refuses foreground changes when another application holds the foreground, so run the suites without using the desktop. Not seen in `run_selftests.ps1` runs.

## 2026-10-21: Session 4, slice 9 (collapsible pinned section)

Backup: scratchpad `backup_before_S4_pins`.
- `BrowserWindow::PinnedSectionCollapsed/TogglePinnedSection` (ZenSpace `collapsiblePins`): a click on the space indicator folds the pinned tabs when the space has any; `Rows()` and `PeripheryTop()` keep the selected pinned tab. Per-space pref `pins_collapsed:<id>`. In the collapsed sidebar, and on the actions button, the indicator still opens the space menu.
- Paint: a chevron (down open, right folded) replaces the space icon on hover and while folded; tooltip and the accessible `expanded` state follow.
- Tests: self-tests 8/8 (zen: fold hides pins, the selected pinned tab stays, expand again); manual on screen: hover chevron + tooltip, folded, chevron kept after leaving, unfold.

## 2026-10-21: Session 4, G1a (animation infrastructure, space switch, hover fades)

Backup: scratchpad `backup_before_G1`.
- `src/core/motion.{h,cpp}`: `SpringNoBounce`, `EaseInOut`, `Transition` (delay, duration, retarget from the current value), `SpaceSwitchStep` (Zen's wrap to the shorter direction). 3 unit tests.
- `ui::AnimationsEnabled/AnimMs` (SPI_GETCLIENTAREAANIMATION). `BrowserWindow::UpdateAnimationTimer` runs a 15ms timer only while a transition is running; fades are declared in paint (target from the hover state), so no hover site needed changes.
- Space switch: `BeginSpaceSwitchAnimation` paints the outgoing indicator + list (`PaintSpaceArea`, split out of `PaintSidebar`) into a compatible bitmap target; `PaintSidebar` slides snapshot and live area; `PaintBackdrop` crossfades the gradients. Snapshot is dropped on device loss.
- Fades: workspace ⋯ button (0.1s), sidebar splitter (0.2s after 0.2s).
- Found on screen: a placeholder saved in an earlier run came back as a visible "New Tab" row. `Profile::LoadFromStore` re-marks it (see FEATURE_INVENTORY, hidden empty tab); restore-2 checks it.
- Tests: unit 47/47; self-tests 8/8 (zen: the slide runs then settles, both directions; restore-2: placeholder stays hidden); UIA 2/2; manual: mid-slide frame (Study entering from the left, gradient mid-fade).
- Not done in G1a: compact sidebar slide (needs fix 6 first, since sliding a pushing sidebar would resize the page every frame), footer icon fades, pinned separator, folder collapse, tab indent, window-controls reveal, toolbar flash popup.

## 2026-10-21: Session 5, window shortcuts and fix 6 (compact overlay)

Backup: scratchpad `backup_before_G1` (no change to persisted data in this session).
- Decision approved by the user: Zen's window keys. Ctrl+Shift+N opens a normal window (Orbit windows never share tabs, so every window is Zen's "unsynced" kind), Ctrl+Shift+P a private window; menu labels updated.
- Fix 6: the compact sidebar is a child window above the tab hosts (a page is a child HWND; the main window cannot paint over it). `PaintOverlay` paints `PaintSidebar` with the overlay's own render target, brush and favicon caches swapped in (Direct2D resources do not cross HWND targets); the space-switch snapshot is taken on the same device. Mouse input is forwarded in main-window coordinates (`ForwardOverlayMouse`); empty chrome still drags/resizes the window through `HitTestNonClient`. The address/rename EDITs are reparented onto the card while it shows. Tab hosts raise themselves on show, so `MoveOverlay` restacks the card after every `Tab::Show`.
- Compact slide: `compact_slide_` (spring, 0.1s) moves the overlay window; `compact_hiding_` keeps the card's layout until it has slid out.
- `ContentRectPx` no longer shifts the page in compact mode.
- Tests: unit 47/47; self-tests 8/8 (zen: Ctrl+Shift+N/P; compact card shown, page rect unchanged, card above the host, address EDIT on the card, back in the window after leaving compact); UIA 2/2; manual: hidden, revealed over the page, a click on the card selects a tab, hides after leaving.

## 2026-10-21: Session 5, G1b (folders, window-controls glide, footer fades)

- Row tween for folder collapse/expand and the pinned fold (0.18s ease-in-out, Zen's `#folderAnimationDuration`): rows that stay glide from their old offset; new rows fade in, clipped by the row below so they grow; rows that left are drawn fading out where they were, validated by `IndexOf` each frame in case a tab closes. The separator and "+ New Tab" move with the pinned fold. Reset on a space switch.
- Window-controls reveal: `caption_reveal_` (0.15s) drives the page offset; each frame moves the host (same size, so no re-layout).
- Footer space buttons: hover background fades (0.2s).
- Tests: unit 47/47; self-tests 8/8 (zen: the fold starts the tween); UIA 2/2; manual: mid-collapse frame of the Research folder.
- Not done: pinned separator height/opacity (0.08s) when the first pin appears, tab indent transition, fading the caption buttons themselves.

## 2026-10-21: Session 5, split view

- Reference: `src/zen/split-view/ZenViewSplitter.mjs` (layout tree, grid types, MAX_TABS 4, min-resize-width 7, splitter drag, context menu labels, shortcuts), `zen-split-group.inc.css` (sidebar row), `zen-split-view.css` (gap, outline).
- `src/core/split_layout.{h,cpp}`: tree, layout with gaps and handles, node rects, Zen's cascading resize, removal with hoisting, text serialization. 6 unit tests.
- Schema **v7** `split_views`; `Store::Load/Insert/Update/DeleteSplitView`; unit test incl. cascade on workspace delete.
- `Workspace` split model (members contiguous, unpinned, one folder; `NormalizeGroups` keeps it), block move for row drags, removal on close/move/pin.
- Window: `ShowContent` (only place showing hosts), panes, outline, dividers with drag, segmented sidebar row, UIA item per member, commands and shortcuts, page context menu (`CefContextMenuHandler`).
- Bug found on screen and fixed: a browser created while its host was being resized (a new pane) kept its creation size. `Tab::OnAfterCreated` now fits the Chromium window to the host; self-test checks it.
- Tests: unit 54/54; self-tests 8/8 (zen: split, order, side by side, second pane loads and fits, divider drag + stored, Ctrl+Alt+H, leave/return, focus selects pane, close dissolves, split link joins, Ctrl+Alt+U; restore: split with sizes in another space); UIA 2/2; manual: split, restore, pane click, page menu item.

## 2026-10-21: Session 5, Glance

- Reference: `src/zen/glance/ZenGlanceManager.mjs`, `actors/ZenGlanceChild.sys.mjs`, `zen-glance.css`, `zen-glance.inc.xhtml`, `prefs/zen/glance.yaml`.
- Renderer: `OrbitApp::OnContextCreated` installs the click listener with the native sender passed through a closure (pages cannot call it); `isTrusted`, Alt only, button 0, http(s), 3px drag threshold.
- Browser: `orbit.glance-open` validated as untrusted input; `OnKeyEvent` closes on an unhandled Escape; page menu "Open Link in Glance".
- Window: `OpenGlance/CloseGlance/ExpandGlance/SplitGlance`; `ShowContent` stacks parent, fade (layered child), glance, buttons; the glance tab has no row of its own; selecting another tab, switching spaces or closing the window ends it.
- Bug found on screen and fixed: button colours were mixed the wrong way round (icon invisible).
- Tests: self-tests 8/8 (zen: synthetic Alt+click ignored, real Alt+click through CEF opens at 80% with the fade, Escape through CEF closes, Ctrl+O/expand, split; restore: a glance open at exit is gone and its parent is active); manual: Alt+click on Hacker News, buttons, click beside closes.

## 2026-10-22: Session 5, library panel

- Reference: `src/zen/library/ZenLibrary.mjs` (render, openProgress: library translate, page `webOffset`, spring), `sections/ZenLibrary{Spaces,Downloads,History}Section.mjs`, `zen-library.css` (width clamp 418px..95vw, side column 110px), `zen-library.ftl`, `prefs/zen/library.yaml`, `ZenCustomizableUI.defaultSidebarIcons` (library replaces downloads), shortcut Alt+Shift+L.
- `src/browser/library_client.{h,cpp}`, `src/browser/library_page.cpp`; `LibraryApi` in `internal_api.cpp` (forwards `history/*` and `downloads/*` to those handlers; `spaces`, `spaces/show`, `spaces/new`, `open`, `close`, `section`); `InternalRequest::browser_id`.
- Window: `OpenLibrary/CloseLibrary/ToggleLibrary`, slide (`library_slide_`), page offset in `ContentRectPx`, panel browser lifetime (released when fully closed, closed with the window; `CheckPendingWindowCloses` waits for it).
- Bugs found and fixed: (1) without `DoClose`, closing the panel's browser made CEF send WM_CLOSE to the main window — the whole window closed and the test run hung at shutdown; (2) the animation timer could stop before a closing panel reached 0, keeping a stale page; (3) a reopened panel now refreshes.
- Tests: self-tests 8/8 (zen: see FEATURE_INVENTORY); UIA 2/2; manual: all three sections, space switch, Alt+Shift+L, Escape.

## 2026-10-22: Session 5, settings

- Reference: `zenLooks.inc.xhtml`, `zenTabsManagement.inc.xhtml`, `zen-settings.js` (pref list), `UrlbarInputBase-mjs.patch` (`zenUrlbarBehavior`: float / floating-on-type via `focusedViaMousedown`), `prefs/zen/*.yaml` (defaults).
- Look and Feel: layout cards, New Tab button shown / position, URL bar behavior, Glance on/off and trigger. Tab Management: Ctrl+Tab skips pending tabs.
- Glance trigger reaches the renderer through `extra_info` at browser creation (`Application::RendererInfo`, `OrbitApp::OnBrowserCreated`); the browser process also refuses glance messages when glance is off.
- `zen.workspaces.separate-essentials` is container-specific Essentials: N/A (Orbit Essentials are per profile).
- Tests: self-tests 8/8 (zen 83 steps / 159 checks: New Tab button below/hidden/back, Ctrl+L floating-on-type vs normal, Ctrl+Tab with a sleeping tab both ways, a real Ctrl+click glance with the Ctrl trigger); manual: settings page.

## 2026-10-22: Session 5, keyboard shortcut editor

- `src/core/shortcuts.{h,cpp}`: action table (defaults as Zen / Chromium equivalents), chord format/parse, assignability, `ShortcutMap` (overrides, conflicts, `Resolve`). 4 unit tests; `Store::DeletePref` + test.
- `BrowserWindow::HandleAccelerator` resolves overrides and calls `DispatchAccelerator` (the former switch, unchanged); menus label keys with `MenuLabel`.
- Settings API `shortcuts`, `shortcut-set` (conflicts unless `replace`), `shortcut-reset`, `shortcut-record` (15 s; while on, the settings tab receives keys instead of shortcuts).
- Tests: unit 59/59; self-tests 8/8 (zen: rebind/block/reset, conflict + replace through the page API, recording lets Ctrl+T reach the page); UIA 2/2; manual: recorded Ctrl+B for "Close all unpinned tabs", reset.

## 2026-10-22: Session 5, audit fixes

- Stale rows corrected (layout presets, workspace indicator, space switch animation).
- Grey-out of inactive windows (setting in Look and Feel), Ctrl+wheel over the sidebar switches spaces, Copy URL as Markdown (Ctrl+Shift+Alt+C, also an editable shortcut), Zen's unbound actions (switch to space 1–10, duplicate tab, create space, show sidebar, reset pinned tab) assignable in Keyboard Shortcuts (`RunShortcutAction`; actions without a default resolve by id).
- Test fix: the Markdown clipboard check polls, since another process can hold the clipboard for a moment (seen once).
- Tests: unit 60/60; self-tests 8/8.

## 2026-10-22: Session 5, nested folders

- Reference: `ZenFolders.mjs` (`createSubfolder`, `#convertFolderToSpace`, `#zenFolderActions` popup, `zen.folders.max-subfolders` = 5), `zen-folders.css`.
- Model: schema v8 (`tab_groups.parent_id`, `icon`); `Workspace::GroupDepth/GroupChain/InGroup/GroupTabs/CreateSubfolder`; `Ungroup` moves contents to the parent; `PruneEmptyGroups` and `NormalizeGroups` work on subtrees; folder placeholders re-marked on load.
- Window: `Row::depth`, nested `Rows()` (headers for every folder entered, collapsed ancestors hide the subtree), per-level indent, `RowOfTab` falls back to the nearest shown header, activation opens every ancestor, dropping on a header lands in that folder's parent, `DropFolderPlaceholder`, Zen-order folder menu with `UnloadFolder`, `NewSubfolder`, `MoveFolderToSpace` (recreates the tree), `FolderToSpace`, `UnpackFolder`, `DeleteFolder`; emoji icon painted via `DrawWorkspaceIcon`. `FirstGrapheme` shared with space icons.
- Build fix on the way: `NormalizeGroups` split pass declared its own `taken`.
- Tests: unit 66/66; self-tests 8/8 (new zen step: 8 checks; restore-2: subfolder + icon); manual: nested rows, empty subfolder, icon, menu, collapse.

## 2026-10-22: Session 5, live folders

- Reference: `src/zen/live-folders/*` (manager, UI, RSS and GitHub providers), `zen-live-folders.ftl`, `popups.inc` (`#zenFolderActions`), `ZenFolders.mjs` ("New Live Folder" menu).
- Core: `core/live_folders` (tolerant XML reader, RSS/RDF/Atom, RFC 822 / RFC 3339 dates, item filter, state text, GitHub queries, issue/pull page scraping, diff with dismissals, relative time). 9 unit tests.
- Storage: schema v9 (`live_folders` table cascading with its folder; `tabs.live_item`, `tabs.sublabel`); `Store_LiveFolderRoundTrips`.
- Browser: `LiveFolders` (owned by `Application`): fetches with `CefURLRequest` in the profile's request context (cookies included, as Zen), max 5 MB, text types only, Latin-1 bodies converted; a 30 s clock fetches what is due; results become unloaded tabs at the end of the folder; closing / moving an item out dismisses it; unpacking turns items into ordinary tabs; deleting forgets the folder.
- Window: Create menu "New Live Folder ▸ Pull Requests / Issues / RSS Feed"; folder menu "Live Folder Options"; header error button (`HitKind::kLiveError`, retry or open github.com/login; icon only when the sidebar is narrow); sublabel line; live folders excluded from "Move to folder" and keep their placeholder.
- Bug found by the new tests: `AnimateRows` looked rows up by tab index captured before tabs were removed (vector index assertion on unpack); `CaptureRows` now records row keys up front. The self-test crash logger also records CRT assertions and abort() stacks now.
- Tests: unit 76/76; self-tests 8/8; UIA 2/2; manual screenshots.

## 2026-10-22: Session 5, remaining animations

- Download arc (`ZenDownloadAnimation.mjs`): `core::PlanDownloadArc` / `DownloadArcAt` (unit test); `ui::DownloadFlight` (two click-through, non-activating layered popups): the circle flies 1 s from the pointer to the library button, or to the corner where the library box slides in (0.35 s + 0.2 s, holds duration + 200 ms, leaves 0.15 s + 0.3 s) when the sidebar is hidden. Triggered from `BrowserClient::OnBeforeDownload` in the active window. Self-test `zen`. A manual screenshot was attempted with a local page, but synthetic input did not reach the window, so it is not visually verified.
- Glance arc (`#createGlanceArcSequence`): the renderer listener also sends the link's rect (main frame only; the browser checks the 4 numbers and applies page zoom); `core::GlanceArcAt` (unit test); the glance view grows along the arc (350 ms, easeOutBack) and, closed with Escape / its close button / a click beside it, shrinks back (ease-out) before closing. The glance buttons wait until it lands. Self-test `zen` (the existing Alt+click and Escape steps now require the arcs).
- Compact flash (`zen.view.compact.toolbar-flash-popup`, off): Look and Feel setting; tab select reveals the hidden sidebar for 800 ms. Self-test `zen`.
- Indent transition (0.1 s) for rows whose folder depth changes, and the pinned separator's collapse/open (0.08 s height, 0.06 s opacity; instant on a space switch). Self-test `zen`.
- Tests: unit 78/78; self-tests 8/8; UIA 2/2.

## 2026-10-22: Session 5, space theme picker and space menu

- Reference: `ZenGradientGenerator.mjs`, `theme-picker.inc`, `zen-gradient-generator.css`, `zen-browser-ui.css` (background layers, grain), `popups.inc #zenWorkspaceMoreActions`, `zen-sets.js` (cmd_zenManageWorkspaces).
- `core/space_theme`: the stored theme, Zen's gradient recipes (1 colour solid, 2 crossed linear gradients, 3 = two radial corners + a linear band), each colour tinting the toolbar base by the opacity; `ThemeColorAt`; `ThemeShouldBeDark` (contrast of white vs black text on the tinted base); `ThemeAccentForUi`. 3 unit tests; schema v10 `workspaces.theme` + store test.
- Window: themed backdrop painted as D2D gradient layers plus a generated grain tile; the palette (text scheme, accent) follows an explicit theme; the URL field colour samples the gradient.
- Picker: orbit://theme/ (Zen's picker logic ported, MPL 2.0 notice in `theme_page.cpp`) in a popup beside the sidebar; `ThemeApi` (state / set / scheme) answers only the picker of a window and edits the space it was opened for; live updates while dragging, stored on release and on close. Space menu in Zen's order with "Edit Theme…", "Manage Spaces…" (library on Spaces, toggles) and "Space Routing Settings".
- Bugs found on the way: the window waited forever to close when the picker was open (its browser was not closed in `BeginClose`), found by trying it on screen and reproduced by leaving the picker open at the end of the zen suite; Alt+F4 (WM_CLOSE) on a panel popup would have destroyed it under its browser. Both fixed and covered by self-tests. The compact-flash test now allows for the real pointer resting on the window.
- Tests: unit 82/82; self-tests 8/8; manual screenshots.

## 2026-10-22: Session 5, copy URL extras and swipe

- Copy URL (zenStrippedURI / QueryStringStripper.stripForCopyOrShare): `StripTrackingParams` (utm_*, click ids, mailing ids, a few site share ids; unit test). The toast gained an optional icon button (Zen's toast `button`); "Copied current URL!" offers Share for web links, which opens the Windows share sheet (`ui::ShareWebLink`: DataTransferManager through IDataTransferManagerInterop with WRL, no exceptions; linked runtimeobject). Self-tests replace the sheet with a recorded call.
- Swipe (ZenSpacesSwipe): horizontal wheel/touchpad deltas drag the space strip with Zen's damping; on release it switches past a quarter of the strip (the switch animation starts at the drag's progress) or springs back.
- Tests: unit 83/83; self-test `zen` (stripped copy, share button, swipe both ways).

## 2026-10-22: Session 5, URL bar results

- Reference: Firefox urlbar (maxRichResults 10, showSearchSuggestionsFirst, frecency buckets), Zen `prefs/firefox/urlbar.yaml` (browser.search.suggest.enabled = false), `zen-omnibox.css` (#urlbar-results max-height 244px, scrolls).
- Core: search-suggestion rows after the heuristic (max 4), `SearchSuggestUrl`, `OmniboxRowInputText`, 10 rows; `Store::QueryHistoryFrecent`. Unit tests (85 total).
- Window: five visible rows with scrolling (keyboard / wheel), the selected row fills the input (Escape restores the typed text first), suggestions requested through the shared `FetchText` (moved out of live folders; suggestions go without cookies) and applied when the input still matches.
- Settings: "Show search suggestions" (off).
- Found on the way: a unit test indexing past a one-row result tripped a debug CRT assertion dialog that hung the test binary; fixed the test, and the test commands now run under a timeout.
- Tests: unit 85/85; self-test `zen`.

## 2026-10-22: Session 5, space icon drag, toolbar layouts, compact variants

- Footer space icons reorder by dragging (nsZenWorkspaceIcons.initDragAndDrop: 5 px, live, stored, no switch). The press position now comes from the mouse message rather than the global cursor.
- Multiple toolbars (zen.view.use-single-toolbar = false) and Zen's Collapsed toolbar (multiple + icon sidebar): a 40 px nav-bar above the page with navigation, the URL field (centred, at most 800), compact/menu buttons and the window controls; three layout cards in Look and Feel.
- Compact mode with multiple toolbars: Hide sidebar / Hide toolbar / Hide both (zen.view.compact.hide-tabbar / hide-toolbar), the hidden toolbar revealed from the top edge.
- Investigated a suspected close hang: the scratch profile had two windows restored, so closing one left the process running; no bug.
- Tests: unit 85/85; self-test `zen` (reorder, layouts, compact variants); manual screenshots.

## 2026-10-22: Session 5, boosts

- Reference: `src/zen/boosts/*` (data model `getEmptyBoostEntry`, `ZenBoostStyles.#generateStyleString`, editor markup, zap overlay, `nsZenBoostsBackend` colour math), `zen-boosts.ftl`.
- `core/boosts`: stored form, CSS (zap, focus, text format, size, invert, colour filter, user CSS), duotone filter (accent from #buildBoostColor's HSL, complementary at the delta, dark→accent / light→complementary crossfade narrowed by the tint strength, lightness kept), and the page script. 5 unit tests; schema v11 + store test.
- Browser: `boost_apply` (load start/end, re-apply to a site's tabs after edits), BoostApi, the editor panel (kBoost; not closed on deactivation so the page can be clicked), site data "Boost" button. Renderer: a picker in each main frame (closure-held sender, trusted clicks only, Escape ends) driven by `orbit.boost-pick-mode`; picks come back as `orbit.boost-pick` and the window accepts them only for its editor's page and mode.
- Fixes on the way: the colour disc was squashed by its flex container; the editor popup was too short.
- Tests: unit 91/91; self-test `zen` (editor, size, real vs synthetic zap, delete); manual screenshots.

## 2026-10-22: Session 5, Ctrl+Tab and Essentials

- tabbox-js.patch (advanceSelectedTab with cycleByAttribute): Ctrl+Tab now includes the Essentials by default (Orbit skipped them before) and, with the new setting, stays within the current tab's group. Glance tabs and empty placeholders are skipped as in Zen. Self-test `zen`.

## 2026-10-22: Session 5, Continue where you left off

- `zen.workspaces.continue-where-left-off` (default off, as in Zen): with it off, startup restores every tab unloaded and opens each window on the empty tab with the URL bar open; on, the last selected tab comes back (Orbit's previous fixed behaviour). New self-test phase `restore-3` (run_selftests.ps1 now runs 9 suites). Behaviour change for existing profiles: they now start on the empty tab unless the setting is turned on.

## 2026-10-22: Session 6, Essentials per space (SPACES_PROFILES_WINDOWS_PLAN.md part 1)

- The folder is a git repository now; the backup-copy step above is replaced by git.
- Schema v12 (one migration for the whole plan): `workspaces.owner_id` (an Essentials row names its space, ON DELETE CASCADE), `workspaces.parent_id` and kind 2 (linked window spaces, part 3), `windows.linked_id`, and one order for the spaces of every profile (part 2). The migration copies each profile's shared grid into every space of that profile, points a window that was on a shared Essential at its copy, drops the shared row, and renumbers space positions (profiles in their order). `MigrateSchema(db, target)` lets tests stop at an older version; the v1/v2 upgrade tests now start from v11 (SQLite cannot drop v12's foreign-key columns).
- Model: `Workspace` owns its grid (`essentials()`, `EnsureEssentials`, `AdoptEssentials`, `owner()`), `essentials_space()` (a linked space shows its parent's), `IsEssentialTab`. `Profile` keeps the spaces and, apart, the linked spaces; `containers()` lists each space with its grid. Loading attaches grids to their owners (a grid without one is deleted) and turns a linked space without a parent into a space.
- Window: `essentials()` / `IsEssential()` are the current space's grid; `RevealTab` shows a tab wherever it lives (media card focus, Switch to Tab). `Application::WindowsShowing` for a grid is the space's window plus its linked windows; `RetireWorkspace` retires the grid's tabs too.
- Tests: unit 94/94 (new: v12 migration copies the shared grid into each space and remaps the window's active tab; Essentials rows belong to their space and cascade; linked space outlives its parent; one order across profiles); self-tests 9/9 (`zen`: an Essential added in Home is not shown or activatable in Second, per-space return; `restore-2`: the Essential comes back in its own space only); UIA check passes (8 checks).

## 2026-10-22: Session 6, a space can use any profile (plan part 2)

- Zen ties a space to a Firefox container; Orbit ties it to a profile (its own Chromium request context: cookies, storage, cache, permissions, plus Orbit's history, bookmarks and downloads tables). Defaults from the plan's open questions: one window shows every normal profile's spaces.
- `Application::Spaces()` (every normal profile's spaces in one stored order, v12 renumbered them; `NormalizeSpaceOrder` at load), `FindSpace`, `MoveSpace` (reorder across profiles; drag in the footer, Move Earlier/Later), `MoveSpaceToProfile` (the space, its Essentials and its linked spaces change `profile_id`; their tabs unload), `AllContainers`. A profile may have no space of its own; there is always at least one space. `CreateProfile(..., with_space)`.
- Window: its profile is always the current space's (`ShowWorkspace` swaps it; `OnProfileChanged` closes the library, site data, theme and boost panels, whose pages belong to the old profile). Switcher, space menu, folder "Change Space", tab "Move to space/window", quick actions, Ctrl+Alt+arrows, swipe, accessibility all use `Spaces()`; URL-bar Switch to Tab and the media card see every normal profile's tabs (`TabContainers`). Private windows keep their own space only.
- Badge: a space of a profile other than the first carries a dot in that profile's colour on its footer icon; menus and tooltips name the profile ("Work (Job)").
- Space menu "Profile ▸": the profiles (checked: the space's) and "New profile…" (a profile without a Home space, then the move); toast "<space> now uses <profile>: its tabs reload signed out".
- Moving tabs: "Move to space" / "Move to window" now cross profiles (never into or out of a private session). A Chromium browser keeps the storage it was created with, so `Tab::UnloadForProfileChange` closes it; a tab on screen loads again at once in the new profile (`kRecreateTabMessage` with wParam 1: only if shown), others when selected. Toast "Moved to <profile>: it reloads signed out".
- Routing: a route names a space of any profile (`RouteTarget` uses `FindSpace`); the routes settings list every profile's spaces.
- Deleting a profile switches its windows to a space of another profile instead of closing them (linked windows close); `BrowserWindow::ForgetProfile` drops pointers to its spaces and tabs.
- Investigated a run where services, memory and the glance step timed out (test-server pages and downloads took ~7 s, every suite ran about twice as slow, CPU idle). The original code, rebuilt and run at that time, passed; after a clean rebuild of this code (a stash round-trip had also left a corrupt `schema.cpp.obj`, LNK1236) every suite passed at normal speed, with the test server's connections logged and nothing held open. Cause not found; treated as environmental. Also: `run_selftests.ps1 -Only a,b` passed through `pwsh` arrives as one string and skips every suite, so run one suite per call.
- Tests: unit 94/94; self-tests 9/9 (`isolation`: one window on spaces of two profiles reads each profile's localStorage/IndexedDB and uses its request context; a space moved A→B unloads, reloads on screen in B's context and reads B's data, stored under B; a tab moved to B's space unloads, keeps its URL, loads in B when selected. `restore-1/2`: a space moved to a new profile comes back in that profile, listed in the window, with its tab); UIA check passes.

## 2026-10-22: Session 6, linked window spaces (plan part 3)

- Instead of Zen's window sync: a tab torn off into its own window gets a temporary space (kind 2, `parent_id`) linked to the space it came from, in the same profile (same history, passwords and cookies), named "<parent> · Window N", with the parent's colour, icon and theme. Defaults from the plan's open questions: a closed linked window's tabs come back in a folder.
- `BrowserWindow::TearOffTab`: "Move to → New window" (greyed in private windows), and a tab drag released outside every Orbit window (`WindowFromPoint` + `GA_ROOT`, popups resolved to their owner) opens the window at the cursor; released over another Orbit window, the tab moves into that window's space. The tab moves before the window opens, so `ShowWorkspace` reparents its live browser (same browser id, history and page state).
- Parent window: a collapsed "temp folder" row per linked space after the tabs (`LinkedChildren`, `Row::linked`, `PaintLinkedRow`: window icon, name, tab count); click focuses the linked window (showing the linked space there); menu Show Window / Bring Tabs Back / Close Window; dropping a dragged tab on it moves the tab into that window. Keyboard, tooltip and UI Automation cover the row.
- Linked window: the indicator reads "← Linked to <parent>" (`kLinkedBack`: the back arrow brings the tabs back loose and closes the window, or turns the window back into the parent's when the parent is not on screen); it shows the parent's Essentials; its switcher lists the linked space after its parent (only there); the window keeps owning it while it shows another space (`linked_space_`, `windows.linked_id`).
- Closing it (`Application::OnWindowCloseRequested`, other windows open): `ReturnLinkedSpace` detaches the window, moves the tabs into the parent (live pages reparented to the parent's window), puts the unpinned ones in a folder named after the linked space, and deletes the linked row. At application exit it is kept and restored like any window; at start-up a linked space no window came back for is returned the same way. A deleted parent turns its linked spaces into ordinary spaces (`RetireWorkspace`). Linked spaces stay out of routing and Manage Spaces.
- Found by the new self-test: `RetireWorkspace` kept a live tab in the closing list without closing it when the caller had not unloaded it first, so shutdown waited forever; it now unloads what it retires. The first copy-URL check now polls the clipboard (it read an empty clipboard once, as the Markdown check had before).
- Tests: unit 94/94; self-tests 9/9 (`zen`: Move to → New window opens a linked space of the parent with the same profile, stored as kind 2; the tab keeps its browser id and history; listed only in its own switcher; the parent shows "Linked window …, 1 tab"; WM_CLOSE returns the tab in a folder with the same live browser and removes the row; the back arrow returns it loose; deleting the parent makes it a space. `restore-1/2`: the linked window comes back on its space, linked to Home, with its tab, and Home shows its row); UIA check passes; manual screenshots of a seeded profile (parent row with count 2, "← Linked to Home" header, the profile dot on another profile's space). Not done: the drag-out with a real mouse (synthetic messages cannot drive it: it reads the real cursor).

## 2026-10-22: Session 6, user feedback round

- Second monitor: `WM_DPICHANGED` now drops the compact-card render target, its favicon bitmaps, the grain brush and the space snapshot, so they are rebuilt at the new DPI (the sidebar had stayed at the old scale).
- Multiple toolbars: back/forward/reload, the URL field (560 wide, or 800 with the new "Wide URL bar" setting `urlbar_wide`), compact and menu now form one centred group with the buttons against the field; the toolbar is 48 px (6 px above and below the field) and spans the whole top edge, and the sidebar (also when revealed in compact mode, and the collapsed column) starts below it instead of covering the buttons.
- Library panel: 440 DIPs wide (was 640), 72 px section rail, smaller headings and rows.
- Menus: `ui::TrackMenu` (src/ui/menu.*) replaces every Win32 popup menu in the window: rounded panel in the chrome's palette, hover rows, check glyph, shortcuts, cascading submenus, keyboard and mnemonics. The page's right-click menu goes through `CefContextMenuHandler::RunContextMenu` into it, with Open Link in New Tab / Glance / Split / Copy Link Address, Open Image in New Tab / Copy Image Address, Search for "<selection>" and Inspect.
- DevTools: docked right of the page (F12, Ctrl+Shift+I, Inspect), in an owned popup that follows the page area; draggable divider, width stored (`devtools_width`); closes with the page, on unload and with the window. Self-test `zen` covers open, Inspect, close.
- Import: `browser/importer.*` detects Chrome, Edge, Brave, Vivaldi, Opera, Chromium and Firefox profiles and reads bookmarks, history and logins (Chromium: Local State key via DPAPI + AES-GCM, or legacy DPAPI values; Firefox: key4.db PBES2 + 3DES). Settings → "Import from another browser". Logins go into schema v13 `passwords`, re-encrypted with DPAPI (never cleartext in the database); Settings lists them (Show / Delete) and autofill (`FillSavedLogin` on main-frame load, per-profile, off in private windows, setting `autofill_passwords`) pushes the matched login to that page's frame only. New core modules `json`, `crypto`; unit tests 102/102. Not run against real browser profiles (that would read your actual passwords), so the Chromium and Firefox readers are verified only by their round-trip tests.
- Popups: `window.open` with popup features (OAuth / sign-in) now opens a native popup window owned by the opener, so `window.close()` works and focus returns to the opener; other new windows stay tabs. Not covered by a self-test (needs a real user gesture).
- Video: this engine reports `h264=no aac=no hls=no`, `vp9`/`av1` yes (self-test `basic` logs it). The pinned CEF is Spotify's standard build without proprietary codecs, so H.264/AAC sites (X/Twitter, much news video) cannot play. Fixing it needs a CEF/Chromium build with `proprietary_codecs=true ffmpeg_branding=Chrome`; not done.
- Extensions: still BLOCKED (Alloy style has no extension system; ADR §9). Not implemented.

- Normal-browser plan, Part 0 (2026-10-23): the Essentials grid now starts at `essentials_rect_.top` (it sat under the toolbar in multiple-toolbar layouts); new `Tab::HostWindow()` (hosting window, else space window, else the owning space's window for an Essential) replaces the private `WindowFor`, so the page menu, Inspect, Glance, fullscreen, popups and downloads work for Essentials. Self-test `zen`: grid inside its rect and below the toolbar, host window of an Essential, DevTools open and close on an Essential. Unit 100/100 in the build script run; self-tests pass.
- Normal-browser plan, Part 1 (video) DONE (2026-10-23, user chose option 1): CEF moved from 152.0.14 (Spotify) to 154.0.34 from psmux/godwinmix-cef (H.264/AAC), pinned by SHA-256 (matches the .sha256 beside the release); it built against the 154 headers with no code changes; all 9 suites + UIA check pass. `canPlayType`: h264/aac probably, hls maybe. Self-test `basic` now fails without H.264/AAC and plays a recorded H.264 clip (served by the test HTTP server, since blob playback does not work from `orbit://`). Old engine kept in `third_party/cef152_backup` (delete when happy). Caveats: unknown third-party maintainer (checksum proves the file, not the builder), no patent licence, Widevine unsupported, hardware decode not checked. (Earlier note, superseded:) stopped before changing anything. Only third-party prebuilt CEF with H.264/AAC found: psmux/godwinmix-cef (cef-154.0.34, Chromium 154; pinned is 152). Needs the user's decision (trust of an unknown maintainer's engine binary, major version bump, patent licensing). Part 2 (extensions) also needs the user's A/B choice.
- (Superseded, see the next entry:) Normal-browser plan, Part 2 first spike (user chose A, Chrome style; 2026-10-23): failed, nothing kept. With `info.runtime_style = CEF_RUNTIME_STYLE_CHROME` in `Tab::EnsureBrowser` (SetAsChild into the tab host, CEF 154.0.34) the browser process dies with an access violation (0xC0000005) inside libcef on the first browser creation, before any window content appears; the same with the default request context instead of the profile one, so the per-profile contexts are not the cause. stderr only shows Chrome's extension registry loader starting. No stack symbols from libcef. Not yet checked: whether it needs other CefSettings/command-line changes, CEF's own cefclient with Chrome style + SetAsChild on this build, or a Chrome-style browser through the Views framework (which would mean giving up Orbit's own window ownership). Stopped as the plan says; option B (Alloy extension host) remains open.

- Normal-browser plan, Part 2 DONE (2026-10-23): extensions through Chrome style. The spike crash was Orbit destroying a tab's host window while the Chrome-style widget inside it was still closing (found by elimination: plain parents worked, then `OnBeforeClose` ordering). Fixes: `Tab::DestroyHostWhenGone` (waits until the host is empty; windows, bridges and quit wait for it), `DoClose` returns false, `RequestClose` detaches a live tab at once, `OnBeforeClose` detaches tabs that closed without `DoClose`, host `Tab*` in a window property, white page background, relaxed (header-gated) frame check for a stale internal frame URL. CEF 154 has no extension API, so `browser/extension_bridge.*` drives `chrome.developerPrivate` / `chrome.management` through a hidden `chrome://extensions` browser (JS in, console messages out; a synthetic click gives the user gesture Chrome demands). Orbit side: `orbit://extensions/` (API `ExtensionsApi`), address-bar puzzle button with `ui::TrackMenu`, action popups (`browser/extension_popup.*`), main-menu item, Ctrl+Shift+A, Web Store brand override for the store's own pages. Chrome Web Store install works inside Orbit (Chrome shows its own confirmation). Self-test suite `extensions` (fixture `tests/fixtures/extension_basic`, loaded with `--orbit-test-extension`), plus all other suites on Chrome style: 10/10 passed, repeatedly. Test fixes needed for Chrome's asynchronous close (tab counts, library/theme popups). Known limits: see FEATURE_INVENTORY "Extensions".

- Normal-browser plan, "other gaps" checked (2026-10-23), state on the Chrome-style build:

  | Item | State |
  |---|---|
  | Print, save as PDF | Works: Chrome's print preview opens (Ctrl+P); "Microsoft Print to PDF" saves a PDF. Chrome-drawn, not Orbit's UI |
  | Built-in PDF viewer | Present (Chromium PDF Viewer extension is installed in each profile); not exercised by a test |
  | Find in page | **Added** (`browser/find_bar.*`, tested) |
  | Password save prompt | **Added** (tested). Never-for-this-site list: not yet |
  | Zoom | Per-tab zoom commands exist; per-site storage not checked or added |
  | Autofill of addresses and cards | Not present (passwords only) |
  | Translate | Not present (Chrome's translate UI needs its toolbar) |
  | Spell check | Not checked (Chrome style may offer it; Orbit shows no language list) |
  | Download progress / list | Exists (library and downloads page) |
  | WebRTC camera / microphone | Devices enumerate (3); Orbit's own permission prompts apply |
  | Notifications | `Notification` exists, permission `default`; Orbit's permission manager still answers prompts (denies) |
  | Service workers | API present (a test-server 404 was the only failure in the probe) |
  | WebAuthn / passkeys | Present: `PublicKeyCredential` and a user-verifying platform authenticator (Windows Hello) are available |
  | WebGPU, USB, HID, Serial, Bluetooth, clipboard, fullscreen, picture-in-picture, share, payments, speech, file system picker | All present in the probe |
  | Drag and drop of links / files into the window | Not checked |
  | Default browser setting | **Added**: Settings card, menu item, hidden when Orbit is the default (0.1.3 / 0.1.4) |
  | Sync | Out of scope |
  | Reader mode, page screenshot | Not present |

- Chrome style follow-ups found on the way: unstyled pages were black-on-dark (fixed: light colour scheme + emulated `prefers-color-scheme`); the Web Store only shows "Add to Chrome" when the client hints name Google Chrome (fixed for the store's own pages); `chrome://` and `chrome-extension://` are allowed typed schemes.

- Follow-ups (2026-10-23): (1) The Web Store says "Add to Orbit": `kStoreRename` (orbit_app.h) rewrites "Add / Remove … Chrome" in the store's own pages and hides its "Switch to Chrome / Install Chrome" banner; self-test `extensions` runs the script on a local page; checked by eye on the live store. (2) An extension's own `chrome.tabs.create` (a welcome page after install) opened a separate Chrome window: those browsers go to CEF's default client, so `browser/default_client.*` (via `OrbitApp::GetDefaultClient`) hides the window, opens the URL as a tab of the profile's window and closes it; self-test `extensions` installs a fixture that does this and checks only Orbit's windows are on screen. (3) Installer: `installer/orbit.iss` (Inno Setup, per user, runs Orbit's own `--install --quiet`), `scripts/make_installer.ps1`, `package.ps1 -SkipSmoke`, and `.github/workflows/installer.yml` (every push builds `Orbit-<version>-setup.exe` as an artifact; a `v*` tag also attaches it to a release). The setup was built and run silently here, then uninstalled; the workflow itself has not run yet.

- Popups and extension windows (2026-10-23): every popup crashed in Chrome style (`Tab::PrepareForPopup` still asked for Alloy style under a Chrome-style opener). Now Chrome style; `window.open` with popup features (sign-in windows) opens as a glance over the opener (`CreatePopupTab(..., as_glance)`) with `window.opener` intact, and `window.close()` closes it (`Tab::NoteClosedByPage`: a close the page started now removes the tab). Windows Chromium opens on its own (extension `chrome.tabs.create` / `chrome.windows.create`) go to `DefaultClient`, which adopts any such page as a tab (profile's window, else any window). The store rewrite no longer hides page parts: it rewords the banner, hides only the "Install Chrome" button and clicks the nag's own "No thanks". Self-tests: popup glance + close (`zen`), extension tab + window (`extensions`). Chrome's install / remove confirmations remain Chrome-drawn dialogs.

- 0.1.3 (2026-10-24): the "extra Chromium" after a Web Store install was traced with a live install (manual suite `store-install`, needs the internet; Chrome's dialog accepted through UI Automation): Chrome opens a New Tab window to show its "added to Chromium" bubble, plus its download bubble. `DefaultClient` now moves such windows off screen at once and closes them when they hold no web / extension page; the download bubble is off per profile (`download_bubble.partial_view_enabled`, set in `OnRequestContextInitialized`); Orbit shows its own "<name> was added to Orbit" toast (the bridge starts on Web Store pages). Extensions button + pinned extensions on the left of the top bar (multiple toolbars: beside it; single toolbar: the button after the compact toggle, pins on a row under the address bar); pin / unpin from the button's menu, stored per profile (`extensions_pinned`). Default browser: Settings card and main-menu item (`installer::IsDefaultBrowser`, `MakeDefaultBrowser` registers and opens Default apps). Version 0.1.3; the setup is `Orbit Browser Setup.exe`.

- 0.1.4 (2026-10-24):
  1. **Sign-in crash.** On the user's machine the process ended with no Windows error report and no "clean shutdown" line. Chromium's Local State says it exited cleanly, so the exact cause is not proven.
     - Chrome's own browser sign-in and password leak check are now off per profile (`signin.allowed`, `profile.password_manager_leak_detection`), next to the password manager prefs.
     - A new `zen` test signs in through a popup glance: real typing and Enter, a POST, a message to the opener, then `window.close()`.
     - Diagnostics now run in every session (`app/crash_log.*`):
       - Fatal exceptions go to orbit.log with the module + offset and the stack.
       - An unhandled exception also writes a minidump to `<User Data>\Crashes` (5 kept).
       - An `ExitProcess` from anywhere logs its caller.
     - Release links with `/DEBUG`, and CI uploads `orbit.pdb` as the "Orbit symbols" artifact so offsets can be read.
     - Popups, page-initiated closes and window close requests are logged (no URLs).
  2. **Default browser.** Windows 11 keeps the choice under `UrlAssociations\<scheme>\UserChoiceLatest\ProgId` and leaves `UserChoice` empty, so Orbit never saw itself as the default.
     - `IsDefaultBrowser` now reads that key, then `UserChoice`, then `AssocQueryString`.
     - The Settings button, the menu item and the welcome step disappear when Orbit is the default (`zen` test).
  3. **Updates** (`app/updater.*`). The latest GitHub release of `mazijoni/orbit_browser` (from the GitHub API) is compared with the running version.
     - Settings → Updates checks, then downloads the release's setup over HTTPS. The SHA-256 is checked against the `digest` GitHub lists for the asset.
     - "Restart to update" closes Orbit cleanly. After `CefShutdown` it starts the setup silently with `/relaunch=1`; orbit.iss then starts Orbit again, and the open tabs are restored once.
     - A daily background check shows a toast (Settings toggle, on by default).
     - Needs a public repository and a published `v*` release.
     - `zen` test: version comparison. The live check is informational.

- 0.1.5 (2026-10-24): sleeping tabs show their site's icon in black and white instead of the blank globe.
  - Favicons are now saved: schema v14 table `favicons` (icon URL → premultiplied BGRA, at most 32x32), never written for private windows. Icons no tab uses and unused for 90 days are pruned at startup.
  - A tab created from its saved record (a restored session, tabs not loaded yet) reads its icon from that table.
  - A sleeping or unloaded tab's icon is drawn in grayscale (Rec. 601 luminance) at 60% opacity, in the sidebar and the Essentials grid (`BitmapFor(..., gray)`).
  - `zen` test: the icon is stored, a tab built from the record has the right pixels before loading, and a discarded tab keeps its icon.

- 0.1.6 (2026-10-25):
  1. **Page fullscreen and links from other apps.** A link from another app (or a tab switch) while a video was fullscreen showed the new page fullscreen, with Escape doing nothing.
     - The window now remembers which page is fullscreen (`fullscreen_tab_`).
     - When another page is shown, that page leaves fullscreen and the window comes back (`ShowContent` → `ExitContentFullscreen`).
     - Escape and F11 act on the fullscreen page.
     - `zen` test: fullscreen via a real click, then a routed external link.
  2. **One DevTools, as docked Chrome's.**
     - Chromium's own "Inspect" item (which opened its DevTools in a separate Chromium window) is removed from the page menu.
     - Orbit's DevTools load with `can_dock=true`, which brings the device toolbar, the dock-side menu and the close button.
     - A bridge script in the front-end (`kDevToolsBridge`) sends `closeWindow`, the dock side (`currentDockState`) and `setInspectedPageBounds` back as console messages.
     - Docked, DevTools cover the page area and lay it out themselves (their divider, left / right / bottom). The page goes into the place they keep for it, and their window has a hole there.
     - Undocked, the host becomes a window of its own. DevTools' device-emulation "toolbox" popup is refused, and links from DevTools open as tabs.
     - DevTools follow Orbit's theme (emulated `prefers-color-scheme` on the DevTools browser).
     - `zen` tests: can_dock + bridge + colour scheme, bottom / left / own window / right through DevTools' DockController, and the close button.

- 0.1.7 (2026-10-27):
  1. **Extensions without a popup** (Picture-in-Picture, ...) now get Chrome's toolbar click.
     - Chrome only clicks an action through its toolbar's extensions container. `Extensions.triggerAction` (CDP) looks it up (`ExtensionsContainer::From(browser)`), and embedded browsers have none, so it crashes; it is also refused for a page's own DevTools session.
     - Orbit holds a private DevTools connection to the browser over an anonymous pipe pair (`browser/cdp_pipe.*`; `--remote-debugging-pipe` with `--remote-debugging-io-pipes`, handles not inheritable, no port).
     - `browser/extension_actions.*`:
       - Finds the extension's service worker in the tab's browser context, starting it with `ServiceWorker.startWorker` if asleep.
       - Attaches and maps the tab to the extension's tab id. Each Orbit tab is its own Chrome window, so `Browser.getWindowForTarget` gives `chrome.tabs.query({windowId})`.
       - Dispatches `chrome.action.onClicked`.
     - A shim in the worker wraps `chrome.scripting.executeScript` / `insertCSS`. They run as usual first; when Chrome refuses them for lack of the toolbar's activeTab grant, Orbit runs the same files / function in the page's main frame, in an isolated world, with a user gesture.
       - That access lasts 5 minutes and only while the tab keeps its origin.
       - http(s) pages only.
     - Popups open on about:blank, get the same shim (plus `tabs.query({active, currentWindow})` = the Orbit tab) through `Page.addScriptToEvaluateOnNewDocument`, then load their page.
     - `extensions` tests: a popup-less fixture (`tests/fixtures/extension_action`) marks the page with a user gesture, twice; the popup sees its tab and runs a script in it.
     - `--orbit-no-devtools-pipe` turns the pipe off.
  2. **Spelling suggestions.**
     - Orbit's copy of the page menu showed Chromium's hidden spelling placeholders as empty rows. (Chromium fills them from the Windows spell checker after the menu is built.)
     - Hidden items are now left out, and Orbit asks the Windows spell checker itself (`browser/spelling.*`, `ISpellChecker`, the profile's first spellcheck language).
     - Suggestions go first in the menu; a click replaces the word (`ReplaceMisspelling`).
     - `zen` test: "helo" suggests "hello".
  3. **Stuck link text.**
     - Chromium's status bubble (the hovered link's address, bottom left) is a popup of each tab's browser, never told when Orbit hides that page, so it stayed on screen.
     - `Tab::Hide` now tells the page the pointer left, so Chromium hides it itself.
     - `browser/status_bubbles.*` also notes the page each bubble appeared over (WinEvent hook) and hides it with that page.
     - `zen` test: hover shows it, a tab switch takes it away, the next hover shows it again.
  4. **Found, not fixed:** right after a web page, the next internal page's first API call can be refused (the frame URL is still the web page's; log line `rejected orbit://... (frame: web, header: 1)`). This makes the Release `services` test fail now and then; reloading the internal page fixes it. The `basic` H.264 recording test is also timing-sensitive in Release.

- 0.1.8 (2026-10-27):
  1. **Cloudflare "verify you are human" failing or looping.**
     - 0.1.7's private DevTools pipe made Chromium mark the browser as automated to every page: `navigator.webdriver = true`, the first thing bot checks look at.
     - `cdp_pipe::Setup` now also passes `--disable-blink-features=AutomationControlled`, so pages see `false` and the pipe keeps working.
     - `basic` test "a page sees a normal browser": `navigator.webdriver` false, `window.chrome` present, PDF plugins listed, no automation globals, notification permission consistent (`default` / `prompt`).
     - Manual suite `turnstile` (internet): a real Turnstile widget (2captcha's demo) gives its token. That demo also passed with the flag on, so it is not strict; Cloudflare's full-page managed challenges are stricter.
  2. **Wrong site icons.**
     - A tab kept the previous site's icon until the new page named one. If it never did (no icon, error page, PDF), the old icon stayed and was saved as the new page's (`favicon_url`), so the favicon cache brought it back after a restart.
     - `Tab::OnAddressChange` now drops the icon and its URL as soon as the host changes (`favicon_host_`).
     - `zen` test: an icon page, then another host in the same tab, has no icon and stores none.
  3. **Space icons in one color.** Space and folder icons are drawn from the emoji's outline in the text color (no `ENABLE_COLOR_FONT`), as Zen does. The Library's Spaces cards use the text presentation (`U+FE0E`, `font-variant-emoji: text`).

## Next actions

1. Check by hand, with a real mouse: drag a tab out of the window onto the desktop (a linked window opens at the cursor), and onto another Orbit window (the tab moves there).
2. Known limitations listed in FEATURE_INVENTORY (RegEx routes, popup routing, previous/next track, PiP, split pane header, UIA hit-testing over the overlay).

Done since: pin/unpin and folder-membership row tween (self-test `zen`); window drag from the page's top strip (manual, real mouse); URL-bar quick actions (unit 62/62, self-test `zen`); site data panel (self-test `zen`, manual); `LibraryClient` generalised to panels (library, site data) with a title hook; onboarding (self-test `zen`, manual first run); space routing (unit 65/65, self-test `zen`); media controls (self-test `zen`, manual). Bug found by the new self-test crash logger (VEH + DbgHelp stack in orbit.log for self-test runs): the media card kept a tab that `OnTabRemoving` re-selected while it was being removed (dangling pointer); fixed by skipping closing tabs and clearing on scope exit. Tabs on the right (self-test `zen`, manual).
3. Small leftovers: pinned separator transition, tab indent transition, the compact flash (off by default in Zen).
Investigated this session: the compact-mode variants exist only in Zen's multi-toolbar layouts (N/A for Orbit's single toolbar).
