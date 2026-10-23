# Orbit — Architecture Decision Record

Status: accepted (2026-10-20). Revisit when the pinned CEF version changes major.

## 1. Context

Goal: a Chromium-based desktop browser for Windows 11 x64 with a vertical tab
sidebar, isolated profiles, workspaces, low overhead, and Chromium's security
model left intact.

Development machine (verified 2026-10-20): Windows 11 10.0.26200 x64, Ryzen 5
7600 (12 threads), 15 GB RAM, ~425 GB free. VS 2022 Build Tools 17.14 with MSVC
14.44.35207, Windows SDK 10.0.26100.0, the bundled CMake 3.31.6 and Ninja 1.12.1.
No Qt, no Rust toolchain.

## 2. Decision summary

| Area | Decision |
|---|---|
| Engine | CEF **154.0.34+g14c5a08+chromium-154.0.8037.98**, the minimal Windows x64 distribution built with `proprietary_codecs=true ffmpeg_branding=Chrome` (H.264/AAC) by github.com/psmux/godwinmix-cef, pinned by SHA-256 (the official Spotify CDN 152.0.14 build has no proprietary codecs) |
| Browser style | **Chrome-style** windowed browsers hosted as child HWNDs (`CefWindowInfo::SetAsChild`, `runtime_style = CEF_RUNTIME_STYLE_CHROME`), which gives Chromium's own extension system. Panels (library, site data, theme, boost) stay Alloy style. Since 2026-10-23; see the notes below |
| Process model | Chromium multi-process. Sandbox **on**, using CEF's `bootstrap.exe` (renamed `orbit.exe`), which loads `orbit.dll`, our code, and calls its exported `RunWinMain` |
| UI | Native **Win32 + Direct2D/DirectWrite**, drawn by our own code, laid out like Zen Browser (§13). The address bar is a native `EDIT` control, so IME, clipboard and accessibility work |
| Threading | One UI thread. Our windows live on the CEF browser-process UI thread, driven by `CefRunMessageLoop()` |
| Persistence | SQLite 3.54.1 (amalgamation, pinned by SHA3-256) for app metadata. Chromium owns cookies, storage and cache |
| Build | CMake + Ninja + MSVC, `/std:c++20`, static CRT (CEF default), `/W4 /WX` |
| Languages | C++ only. No Rust: no component justifies an FFI boundary yet |

## 3. Alternatives considered

**UI toolkit**

* **Qt 6 / Qt Quick** (the plan's first candidate). Rejected for now:
  * It is not installed and adds a large runtime (Qt Core/Gui/Quick/Qml DLLs, plus LGPL deployment duties).
  * Qt Quick runs its own scene graph and a second D3D device.
  * Integrating with the CEF loop forces either `multi_threaded_message_loop` (cross-thread parent/child HWNDs, every callback marshalled) or pumping `CefDoMessageLoopWork()`, which CEF discourages.
  * Focus and keyboard routing between a QWindow and a foreign Chromium child HWND are a well-known source of bugs.
  * Qt's benefit is rapid styled-widget development. For a sidebar, a toolbar and a few panels that benefit doesn't outweigh the cost.
* **CEF Views** (Chromium's own toolkit through CEF). It has native controls and a shared thread, but the widget set is limited: no custom-drawn virtualized list and no drag-reorder primitives. Chrome-style Views windows also pull in Chrome's own UI.
* **HTML/WebUI chrome** (the sidebar as another CEF browser). Rejected: it needs an extra renderer process per window, it increases what web-like content can reach, and the plan forbids "a website wrapped in a window".
* **WinUI 3 / XAML Islands.** These need the Windows App SDK runtime, have airspace problems with HWND children, and cost more memory than plain Win32.

**Browser style**

* **Chrome style** gives Chrome's own prompts, extension support and download UI. In a client-provided parent HWND, though, we get less control over popups and dialogs, and several Alloy-only callbacks are unavailable.
* We first chose **Alloy style** for full control, at the cost of Chrome extensions. On 2026-10-23 tabs moved to **Chrome style** because extensions were asked for and CEF 154 no longer has the Alloy extension API (`LoadExtension`, `CefExtensionHandler` are gone). In a client-provided parent HWND, Chrome style shows no Chrome toolbar and keeps the callbacks Orbit uses (popups, context menu, keys, downloads, permissions, JS dialogs, DevTools): all self-test suites pass on it.

**What Chrome style changed in the code** (all learned the hard way):

* Closing a browser: Chrome style does not call `DoClose` for a child browser and tears its widget down after `OnBeforeClose`. Destroying the parent host window earlier crashes in libcef (an access violation). `Tab::DestroyHostWhenGone` waits until the host is empty; windows wait for `Tab::PendingHostDestroys()` before they close. `Tab::RequestClose` detaches a live tab from its space itself (what `DoClose` used to do), and `BrowserClient::OnBeforeClose` runs that detach for any tab that closes without it.
* The host window keeps its `Tab*` in a window property, not `GWLP_USERDATA` (Chromium reads the parent's user data).
* Pages that paint no background get `background_color = white`; Chrome style would otherwise give them the browser theme's dark base.
* Orbit's `orbit://` API gate accepts a stale internal frame URL (Chrome style updates it later than Alloy did); the `X-Orbit-Request` header remains the boundary.

**Extensions (Chrome's system, Orbit's UI).** CEF offers no extension API, so Orbit goes through the one privileged door: the `chrome://extensions` page's `chrome.developerPrivate` / `chrome.management`. `browser/extension_bridge.*` keeps one hidden Chrome-style browser on that page per non-private profile (started when the profile already has an extension, when its Extensions page or the Web Store is opened), sends it commands as JavaScript and reads answers from console messages. On top: `orbit://extensions/` (list, on/off, details, remove, load unpacked, developer mode), an extensions button in the address bar with Orbit's own menu, action popups (`browser/extension_popup.*`) in a small window under the button, and install from the Chrome Web Store itself (the store pages get the client-hint brand `Google Chrome`, `orbit_app.cpp`). Chrome draws only its own install and remove confirmations. Each Orbit profile is a separate Chrome profile, so extensions are per profile; private windows run none. Known limits: an action popup's `chrome.tabs` query returns the popup's own window, not the page; actions without a popup cannot be triggered; Chrome's confirmations are Chrome-styled; the Web Store is Google's to allow.

## 4. Object model and ownership

```
App (process singleton, UI thread)
 ├─ Database (SQLite, one connection, UI thread only)
 ├─ ProfileManager ── Profile ──► CefRequestContext (1 per profile, lazily created)
 └─ WindowManager ── BrowserWindow (HWND, owns UI + Tabs)
                          ├─ profile (non-owning ref; a window shows exactly one profile)
                          ├─ workspaces (filter over the profile's tabs)
                          └─ Tab (unique_ptr) ──► CefBrowser (0..1; 0 = discarded/unloaded)
```

* There is **one** `CefClient` (`BrowserClient`) for every browser. It routes each callback to its `Tab` by browser id. A browser's client is fixed at creation, so a per-window client would break moving tabs between windows.
* Each tab owns a small host `HWND`, which is the parent of its Chromium window. `OnAfterCreated` maps a browser to its tab through `GetParent()`. This also covers popups, which Chromium creates itself.
* Raw pointers are non-owning only:
  * `Profile` owns its `Workspace`s, which own their `Tab`s.
  * `Application` owns the windows.
  * A tab whose close is in flight is owned by `Application` until `OnBeforeClose`.
* Chromium objects are held in `CefRefPtr` and released in `OnBeforeClose`.
* Every engine callback we handle runs on the UI thread, so state needs no locks.
* Callbacks on the IO thread (`OnProtocolExecution`, `orbit://` handlers) only post to the UI thread with `CefPostTask(TID_UI, …)`.
* Browser host windows are destroyed in a posted task, not inside `DoClose`. Destroying them there makes Chromium run `OnBeforeClose` re-entrantly. The self-tests caught this as a shutdown hang and a crash.

## 5. Profile isolation

* Each profile has a directory `%LOCALAPPDATA%\Orbit\User Data\Profile-<uuid>`.
* `CefRequestContext::CreateContext` uses `cache_path` = that directory.
* **Verified constraint:** the directory must be a *direct child* of `root_cache_path` (CEF `chrome_browser_context.cc`). Any other path is rejected with only a log line, and CEF silently uses a non-persistent profile instead. The first implementation (`User Data\Profiles\<uuid>`) hit this. Orbit now compares `GetCachePath()` with the expected path, and the `isolation` self-test asserts it.
* Chromium separates cookies, localStorage, IndexedDB, cache, permissions and service workers.
* A private window uses a context with an empty `cache_path`, i.e. in-memory "incognito". It is never written to history.
* **Multiple windows of one profile** share the profile's single `CefRequestContext` instance.
* **Concurrent access / locking:** one browser process serves all profiles. Chromium's process singleton locks `root_cache_path`. A second launch is forwarded through `CefBrowserProcessHandler::OnAlreadyRunningAppRelaunch`, and we open a window in the running instance.
* **Crash:** Chromium's stores are crash-safe on their own. Our metadata uses SQLite WAL with transactions (§7).
* **Deletion:** windows on one of the profile's spaces switch to a space of another profile (its linked windows close), then its DB rows are deleted (cascading to workspaces, tabs, history, bookmarks and downloads) and its context released. The directory is queued and removed at the next start, before `CefInitialize`, when nothing can hold it open. Only UUID-shaped names are ever deleted.
* **Spaces ↔ profiles (2026-10-22, Zen's container per space):** a profile owns its spaces (`workspaces.profile_id`), but the spaces of every normal profile share one order (`workspaces.position`, global since schema v12) and one window switches between all of them. A window's profile is always its current space's: `BrowserWindow::ShowWorkspace` swaps it, and the library, site data, theme and boost panels close because their pages live in the previous profile's context. A space can move to another profile ("Profile ▸" in the space menu): its row, its Essentials and its linked spaces change `profile_id`, and every one of its tabs is unloaded. Tabs move between profiles too ("Move to space/window"). In both cases the tab's Chromium browser is closed, because a browser's request context cannot change; it is created again in the new profile's context (signed out) when it is on screen. Private profiles keep their own windows and spaces: no tab or space crosses into or out of them. Profile directories remain direct children of the user-data root.
* **Spaces ↔ windows:** a space is shown in at most one window. Selecting a space that is open elsewhere focuses that window.
* **Essentials (schema v12):** each space owns its grid (`workspaces.owner_id`, deleted with the space). A linked window space shows its parent's grid; an Essential is one tab, so the window that selects it takes its page (`Tab::Reparent`, `BrowserWindow::OnTabMovedAway`).
* **Linked window spaces (instead of Zen's window sync):** tearing a tab off ("Move to → New window", or a drag released outside every Orbit window) creates a space of kind 2 whose `parent_id` is the current space, in the same profile (same history, passwords, cookies), and opens a window on it; the tab's live browser is reparented, so its page keeps its state. The window owns that space even while it shows another one (`windows.linked_id`). The parent space shows it as a collapsed row (window icon, name, tab count: click focuses the window; Bring Tabs Back / Close Window; drop a tab on it to move it there). Closing the linked window while other windows stay open returns its tabs to the parent in a folder named after it and deletes the linked space; the back arrow and "Bring Tabs Back" return them without a folder. At application exit the linked window is kept and restored like any window; a linked space whose window did not come back is returned at start-up. When the parent space is deleted, its linked spaces become ordinary spaces. Linked spaces are not listed in the space switcher (except in their own window), routing or Manage Spaces.
* Popups opened by a page inherit the opener's request context. CEF requires this, and it keeps them in the same profile.

## 6. Tab lifecycle strategy

CEF 152 has **no public tab-discard API** (verified: `cef_browser.h` has no discard/freeze method). It also exposes **no "audible" state**. `CefAudioHandler` diverts audio output away from the speakers, so it can't be used for detection.

* **Background tabs, a verified limitation:** a background tab's host HWND is hidden (`SW_HIDE`). The original assumption was that Chromium would then mark the WebContents hidden and throttle it. **Measurement disproved this.** The `memory` self-test reads `document.visibilityState` from a background page, and in CEF 152 Alloy windowed mode it stays `visible`. Four approaches were tried, and none changes it:
  * hiding the host
  * hiding CEF's browser HWND
  * hiding Chromium's inner `Chrome_WidgetWin_1` widget window
  * the same with native occlusion tracking disabled

  `CefBrowserHost::WasHidden()` returns early for windowed browsers (CEF source: `alloy_browser_host_impl.cc`). Consequences:
  * Background tabs do not get Chromium's hidden-page throttling.
  * Static pages are unaffected: 25 idle tabs measured 0.3% CPU in total.
  * Pages that run timers or animations keep running at foreground rate.

  Mitigations:
  * The sleep policy, with media protection, frees the tab entirely.
  * Lazy restore, so restored tabs cost nothing until selected.

  A complete fix would be windowless (off-screen) rendering, where `WasHidden` works. That is a large change, with its own CPU and GPU-copy costs, and is deferred. The self-test logs the observed state, so a CEF update that changes this will be noticed.
* **Discard** = close the `CefBrowser` and keep the Tab model (URL, title, favicon). Selecting a discarded tab creates a new browser and loads the saved URL. Limitations, stated in the UI and docs: back/forward history, form input, scroll position and JS state are lost.
* **Policy:** off by default. Pinned and user-protected tabs are never discarded. A tab using camera or microphone (`OnMediaAccessChange`) is never discarded. Audio playback is detected by a query from the browser process to the renderer.
* Restored sessions load **lazily**: only the active tab of each window gets a browser at startup.

## 7. Persistence

* `orbit.db` lives in `User Data`. It uses WAL and `foreign_keys=ON`, with schema migrations recorded in `PRAGMA user_version`. Each migration runs in one transaction.
* The database holds app metadata only: profiles, workspaces, tabs and their order, windows, preferences, bookmarks, history and the downloads list.
* History is our own table (Alloy style has no Chrome history service). It is per-profile and never written for private windows.
* Writes are small transactions on the UI thread. Hot paths are coalesced: tab state changes are flushed after a short delay, not on every callback.

## 8. UI ↔ browser communication and security boundaries

* The UI and the browser layer talk through in-process C++ calls on the UI thread. Web content is never given a native bridge: no `CefMessageRouter`, no V8 extensions in web pages.
* Internal pages (new tab, history, settings) use the custom scheme `orbit://`, registered standard + secure + CORS-enabled. Any privileged action they need must go through a browser-side handler that checks the frame origin is `orbit://`.
* Navigation: `OnBeforeBrowse` blocks `file://` navigations from web origins. `OnProtocolExecution` shows a confirmation prompt for external protocols. The address bar runs typed text through a URL fixer before loading.
* Popups: `OnBeforePopup` turns them into tabs in the same window. They inherit the request context.
* Certificate errors: we keep CEF's default, which cancels the request and shows an error page. There is no bypass UI.
* Permissions: `OnShowPermissionPrompt` / `OnRequestMediaAccessPermission` show our own prompt. The default is deny.
* Sandbox and site isolation stay at Chromium defaults. No `--no-sandbox`, `--disable-site-isolation-trials` or `--single-process` anywhere.

## 9. Known CEF limitations that affect features

| Feature | Limitation | Plan |
|---|---|---|
| Extensions | CEF has no extension API. Chrome style has the extension system | `ExtensionBridge` over `chrome.developerPrivate` (§3). Limits listed there |
| Tab discard | No API | Close-and-reload (§6) |
| Audio indicator | No audible-state API | Renderer query |
| Sync, Safe Browsing UI, password manager | Not provided by Alloy style | Out of scope. Safe Browsing isn't available to CEF builds; documented in security.md |
| Session history restore | No API to inject navigation entries | Restore URL only |
| Background throttling | Hidden windowed (Alloy) browsers stay "visible" to Chromium (verified, §6) | Sleep policy and lazy restore. OSR is future work |
| Notifications | No notification display in Alloy style | Always denied, without prompting |

## 10. Memory expectations

We set no numeric targets until we have a baseline. Structurally:

* The UI uses one Direct2D render target per window and no extra renderer process for the chrome.
* Chromium's own processes dominate memory (browser, GPU, network, renderers).
* Site isolation adds roughly one renderer per site, by design, and we don't trade it away.
* Measurement methodology: `docs/performance.md`.

## 11. Build and distribution complexity

* About 1 GB of CEF binaries is fetched by `scripts/fetch_deps.ps1`, checksum-verified, and kept out of version control.
* The release layout is `orbit.exe` (bootstrap), `orbit.dll`, `libcef.dll`, resources and locales. It runs from any directory.
* The installer comes in Milestone 8, per-user, under `%LOCALAPPDATA%\Programs`.
* `bootstrap.exe` is customized with our icon and version info after copying. All binaries must be code-signed for distribution.

## 12. Update strategy

* The CEF pin lives in one place (`cmake/deps.cmake` + `scripts/fetch_deps.ps1`) and is bumped deliberately.
* Track CEF stable releases for security fixes. Chromium security patches reach us only through CEF.
* Application updates in v1 work by re-running a signed installer. Later: a signed update manifest checked at most daily, with no background service.

## 13. Zen-style interface (2026-10-21)

The chrome reproduces Zen Browser's single-toolbar layout. Metrics and colors come from Zen's stylesheets (MPL-2.0) and live in `ui/theme.h`. Zen's chrome is XUL/CSS running in Firefox, so none of its code can run here. Each behavior was reimplemented natively:

| Zen behavior | Native implementation |
|---|---|
| No title bar; window controls hidden until the pointer reaches the top (`zen.view.hide-window-controls`) | `WM_NCCALCSIZE` removes the caption but keeps the resize borders and the DWM shadow. `WM_NCHITTEST` returns `HTTOP` for the top edge and `HTMIN/MAX/CLOSE` for the drawn buttons (so Windows 11 snap layouts still work), and `HTCAPTION` for empty chrome, which drags the window |
| Content in a rounded card with `--zen-big-shadow` | The tab host gets a rounded window region, which also clips Chromium's child window. The shadow is drawn by Direct2D behind it. The region's corners are aliased: per-pixel alpha would need windowless rendering |
| Workspace theme gradient | `ui::MakeBackdrop` follows `ZenGradientGenerator`: the space color is blended into the toolbar base, plus a hue-shifted second stop. Settings → "Space theme color" controls the strength |
| Reveals (window controls, compact-mode sidebar) | The content card is *moved*, not resized, so the page does not re-layout while the pointer passes. The part pushed past the window edge is clipped until the reveal ends |
| Floating URL bar | An owned `WS_POPUP` (DWM rounded corners, drop shadow). The single address `EDIT` is reparented into it and back. A new browser window takes focus once when it is created; a short guard (`BrowserWindow::OnContentFocused`) returns focus to the bar, and `OnSetFocus` refuses navigation focus while the bar is being typed in |

Not reproduced yet: the essentials grid, split view, glance, URL suggestions, Zen's styled popup menus (Win32 menus are used), and animations.
