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
| Engine | CEF **152.0.14+g8a0b5b9+chromium-152.0.7977.167**, the minimal Windows x64 distribution, pinned by SHA-1 |
| Browser style | **Alloy-style** windowed browsers hosted as child HWNDs (`CefWindowInfo::SetAsChild`, `runtime_style = CEF_RUNTIME_STYLE_ALLOY`) |
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
* We chose **Alloy style** for full control and the most-used embedding path. The cost is that Chrome extensions are unavailable (see §9).

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
* **Deletion:** close the profile's windows, then delete its DB rows (cascading to workspaces, tabs, history, bookmarks and downloads) and release its context. The directory is queued and removed at the next start, before `CefInitialize`, when nothing can hold it open. Only UUID-shaped names are ever deleted.
* **Workspaces ↔ windows:** a workspace is shown in at most one window. Selecting a workspace that is open elsewhere focuses that window. Tabs can move only between windows of the same profile, because a browser's request context cannot change.
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
| Extensions | Alloy style does not support Chrome extensions. Chrome style supports them only with Chrome UI | Not supported in v1. Documented |
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
