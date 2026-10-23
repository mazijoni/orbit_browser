# Plan: everything a normal browser has, in Orbit's own UI

**Status (2026-10-23): Parts 0, 1 and 2 are done; Part 3 and the "other gaps" were worked through.** See `IMPLEMENTATION_LOG.md` (entries "Normal-browser plan") for what was built, what was found, and the gap table. Part 1 took option 1 (the third-party H.264/AAC CEF build). Part 2 took option A, Chrome style; the spike's crash was Orbit's own host-window teardown, not a CEF limit.

Written 2026-10-23 for the next session. The text below is the original plan.

## How to resume

Give Claude this file and say something like:

> Read docs/zen-parity/NORMAL_BROWSER_PLAN.md, IMPLEMENTATION_LOG.md and
> docs/architecture.md (§2, §3, §9), then implement the plan in the order of
> the parts below. Inspect the code before you change it. Keep the inventories
> and the log updated. Build and test after every part. Ask me before the
> decision in Part 2.

State when this was written: unit tests 102/102, self-tests 9/9, schema v13.
Build: `pwsh ./scripts/build.ps1`, `pwsh ./scripts/run_selftests.ps1` (one
`-Only` suite per call), `pwsh ./tools/uia_check.ps1`.

## What the user asked for

1. All video support (X/Twitter, news sites, HLS, anything a normal browser plays).
2. Extensions.
3. Keep Orbit's own UI style (no Chrome toolbar, menus or dialogs showing).
4. "Inspect" must work on a tab that is an Essential.
5. The top of the Essentials grid is cut off.

## Why things are as they are (verified)

- **Video:** the pinned CEF (`scripts/fetch_deps.ps1`, Spotify CDN "minimal"
  build, 152.0.14) has no proprietary codecs. The self-test `basic` logs
  `h264=no aac=no hls=no vp9=probably av1=probably`. YouTube serves VP9/AV1, so
  it works; X/Twitter serves H.264/AAC (and HLS), so it does not.
- **Extensions:** Orbit runs CEF in *Alloy* style (`runtime_style =
  CEF_RUNTIME_STYLE_ALLOY`), which has no extension system (ADR §2, §3, §9).
  Chrome style has extensions but brings Chrome's UI with it.
- **Inspect on Essentials:** `WindowFor(tab)` in `browser_client.cpp:28` is
  `tab->workspace()->window()`. An Essential's workspace is the space's grid
  (kind 1), whose `window()` is always null (only spaces get a window). So for
  an Essential every handler that uses `WindowFor` does nothing: the custom page
  menu (`RunContextMenu` returns false), Inspect, glance, fullscreen, focus,
  popups and downloads' window. Inspect is just where it shows.
- **Essentials top cut off:** `BrowserWindow::EssentialRects()` places the tiles
  at `address_rect_.bottom + 6` (browser_window.cpp ~5875). With multiple
  toolbars `address_rect_` is in the toolbar (bottom about 42), but since the
  last session the sidebar starts below the 48 px toolbar, so the first row sits
  under the toolbar. It must use `essentials_rect_.top`, which `Layout()`
  already computes correctly.

## Part 0: the two Essentials bugs (small; do first)

1. **Essentials position.** In `EssentialRects()` use `essentials_rect_.top` as
   the grid's top in the expanded sidebar (the collapsed branch already does).
   Check both layouts (single and multiple toolbars), compact mode revealed, and
   tabs on the right. Add a self-test: with an Essential present, every tile's
   rect is inside `essentials_rect_`, below `toolbar_.bottom` when the toolbar is
   shown, and equal to the sidebar's top part when it is not.
2. **Essentials and `WindowFor`.** Make one shared helper
   (`BrowserWindow* WindowOfTab(Tab*)`, e.g. in `Application`) that returns the
   window showing the tab: for a space tab its space's window; for an Essential
   the window of its owner space (`workspace->owner()->window()`), or the window
   whose `essentials()` contains it and that has it active (`Tab::window()` is
   the hosting window and is the best answer when set). Replace the private
   `WindowFor` in `browser_client.cpp` (and the copies in `internal_api.cpp`,
   `tab.cpp` close paths) with it. Then check: custom page menu on an
   Essential, Inspect (docked DevTools on an Essential), Glance from an
   Essential (it currently opens a normal tab), fullscreen, popups, downloads.
   Self-tests: DevTools opens for an Essential and closes with it;
   `RunContextMenu` handles an Essential's browser.

## Part 1: all video (proprietary codecs)

Goal: `canPlayType` says yes for H.264, AAC, HEVC where the OS can decode it,
and HLS plays (X/Twitter, news sites, Vimeo, Twitch, Reddit video).

Options, in the order to try:

1. **Find a CEF build with proprietary codecs for the same major version.**
   Chromium builds with `proprietary_codecs=true ffmpeg_branding=Chrome` are what
   Chrome uses. Check what is available (CEF's own builds do not ship it; some
   third-party distributions do). Verify the checksum, record the source in
   `fetch_deps.ps1` and `docs/build.md`, and check its licence (H.264/AAC carry
   patent licensing that the user, not the code, is responsible for; say so in
   the docs). Run the whole test set against it.
2. **Build CEF/Chromium from source** with those flags (automate with CEF's
   `automate-git.py`). Very large (100+ GB, hours); do it only if option 1
   fails, and put the result in a local cache the fetch script can use.
3. **Not a fix:** do not add a command-line flag or try to sideload ffmpeg;
   there is no flag that enables the demuxer in a build without it.

Also:
- Widevine (DRM: Netflix, Disney+, Spotify web) is separate. The distribution
  has `WidevineCdm` folders in the user-data directory but no licence for
  Orbit to ship it. Check whether the CEF build can load a Widevine CDM from an
  installed Chrome, and implement that only if it is permitted; otherwise
  document DRM video as unsupported.
- HLS on non-Safari browsers needs MSE players (hls.js) which work once H.264
  works; no extra code.
- Extend the self-test's codec probe into a real check (fail if h264 is "no").
  Add a local test page that plays a small H.264/AAC MP4 and an HLS stream
  (served by `test_http_server.cpp`) and checks `currentTime` advances.
- Check hardware decode (`--enable-features` for D3D11 video decode is on by
  default in Chromium; confirm in `chrome://gpu`-equivalent diagnostics).

## Part 2: extensions (decision needed first)

Ask the user to choose; the choice changes the work:

**A. Switch to Chrome style, theme it to look like Orbit.** Chrome style gives
the real extension system (Manifest V3, the Web Store, `chrome.*` APIs,
extension popups and the service worker lifecycle). Cost: Chrome style shows
its own toolbar/UI for browser windows. In CEF's Chrome style with
`CefWindowInfo::SetAsChild` the browser is still embedded in an HWND the app
owns (ADR §3 notes "less control over popups and dialogs and several
Alloy-only callbacks unavailable"). Spike first (one day): create a Chrome-style
browser as a child of a tab host, and check (1) that no Chrome toolbar appears,
(2) the callbacks Orbit uses still fire (`OnBeforePopup`, `RunContextMenu`,
`OnPreKeyEvent`, `CefDownloadHandler`, `CefPermissionHandler`,
`CefJSDialogHandler`), (3) `CefExtensionHandler` works, (4) the profile/request
context story (Part 2 of the last plan) still holds. If the spike fails, stop
and tell the user.

**B. Stay on Alloy and build a small extension host.** CEF's Alloy style has
the legacy extension API (`CefRequestContext::LoadExtension`, 
`CefExtensionHandler`) with partial `chrome.*` support. It loads unpacked MV2
extensions and some MV3 pieces. Cost: patchy API coverage; many popular
extensions will not work. Spike: load a trivial unpacked extension and a content
blocker; see what works.

Whichever is chosen, keep Orbit's own UI:
- **Install/manage UI:** an `orbit://extensions/` page in Orbit's style: list,
  enable/disable, remove, details, "Load unpacked…" (developer mode), install
  from a `.crx`/Web Store link with a permission prompt drawn by Orbit
  (reuse `PermissionManager` patterns), per-profile storage (a table
  `extensions(profile_id, id, path, enabled, version)`, schema v14).
- **Toolbar buttons:** extension action buttons in the toolbar group (next to the
  compact/menu buttons, collapsing into an overflow menu) drawn with Orbit's
  icon style; the action popup opens in a popup like the site-data panel
  (`LibraryClient` panels), sized to the extension's popup, closing on blur.
- **Context menu items:** extensions' menu items appear in Orbit's page menu
  (`ui::TrackMenu`), not a Chrome menu.
- **Private windows:** extensions off unless the user allows per extension.
- **Security:** review `docs/security.md`; extension pages must not reach
  `orbit://` APIs (the `X-Orbit-Request` gate and frame check stay), the
  extension scheme gets the same scrutiny, and permissions are shown before
  install. Add a security self-test (an extension page cannot call
  `orbit://history/api/*`).
- **Tests:** a test extension (content script that changes the title, a
  browser action) loaded in a self-test; it runs, shows its button, its popup
  opens, it is removed cleanly; extension data is per profile; restart restores
  it.

## Part 3: keep the UI Orbit's own (applies to Parts 1–2)

- No Chrome UI may appear: no toolbar, no native Chrome menus, no Chrome
  permission bubbles, no download shelf. Check each new callback and replace
  any Chrome-drawn surface with the Orbit one (`ui::TrackMenu`, toasts,
  `LibraryClient` panels).
- Take screenshots (`tools/capture_window.ps1`, the scratchpad
  `capture_all.ps1` pattern) of each new surface in dark and light, single and
  multiple toolbar layouts, and on a second monitor at a different DPI.

## Other gaps worth checking on the way ("a normal browser has")

Check each against the current build and fix or list it:
- **Print / PDF viewer / save as PDF**, **find in page (Ctrl+F)**, **zoom
  per site stored**, **autofill for addresses and cards** (only passwords now),
  **password save prompt** (import and autofill exist; saving a new login after
  sign-in does not), **translate**, **spell check** language list,
  **download shelf/progress** (exists in the library), **built-in PDF
  viewer**, **WebRTC/camera pickers**, **notifications**, **service workers**,
  **WebAuthn/passkeys** (needs Windows Hello integration; check CEF support),
  **drag and drop of links/files into the window**, **default browser** setting,
  **sync** (out of scope unless asked), **reader mode**, **page screenshot**.
  Produce a short table in the log with each one's state before starting work.

## Order and checkpoints

1. Part 0, then the full test set. (Quick and safe; fixes what the user sees.)
2. Part 1, then the full test set against the new CEF build, plus the video
   self-test. If no suitable CEF build exists, stop and report; do not fake it.
3. Part 2: ask the user A or B, do the spike, report the result before building
   the whole thing, then build and test.
4. Part 3 checks after each part; update `FEATURE_INVENTORY.md` ("Extensions"
   from BLOCKED, a new "Video codecs" row), `docs/architecture.md` (§2, §3, §9),
   `docs/security.md`, `docs/build.md`, and the log.
