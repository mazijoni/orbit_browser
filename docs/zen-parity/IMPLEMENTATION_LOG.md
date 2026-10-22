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

## Next actions

1. Known limitations listed in FEATURE_INVENTORY (RegEx routes, popup routing, previous/next track, PiP, split pane header, UIA hit-testing over the overlay).

Done since: pin/unpin and folder-membership row tween (self-test `zen`); window drag from the page's top strip (manual, real mouse); URL-bar quick actions (unit 62/62, self-test `zen`); site data panel (self-test `zen`, manual); `LibraryClient` generalised to panels (library, site data) with a title hook; onboarding (self-test `zen`, manual first run); space routing (unit 65/65, self-test `zen`); media controls (self-test `zen`, manual). Bug found by the new self-test crash logger (VEH + DbgHelp stack in orbit.log for self-test runs): the media card kept a tab that `OnTabRemoving` re-selected while it was being removed (dangling pointer); fixed by skipping closing tabs and clearing on scope exit. Tabs on the right (self-test `zen`, manual).
2. Small leftovers: pinned separator transition, tab indent transition, the compact flash (off by default in Zen).
Investigated this session: the compact-mode variants exist only in Zen's multi-toolbar layouts (N/A for Orbit's single toolbar).
