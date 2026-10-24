# Orbit Security Model

## Boundaries

| Boundary | Mechanism | Verified by |
|---|---|---|
| Web content ↔ OS | Chromium sandbox stays on. `orbit.exe` is CEF's `bootstrap.exe`, which creates the sandbox broker; renderer, GPU and utility processes run restricted (LPAC ACLs are applied to the install directory at build time). No `--no-sandbox`, `--single-process` or `--disable-site-isolation-trials` anywhere | Code review: `OrbitApp::OnBeforeCommandLineProcessing` adds a switch only in self-test mode (autoplay policy) |
| Site ↔ site | Chromium site isolation (default) | Chromium |
| Profile ↔ profile | One `CefRequestContext` per profile, each with its own `cache_path` (direct child of the user-data root). Private windows get an in-memory context each | `isolation` self-test: cookies, localStorage and IndexedDB are invisible across profiles and private windows; the on-disk path is accepted by Chromium |
| Web content ↔ Orbit internals | No JS bindings or message routers are exposed to web pages. `orbit://` pages are served by the browser process. Their `/api/` endpoints require an `X-Orbit-Request` header.
<br>• A cross-origin document (a web page, or another `orbit://` host) cannot attach a custom header without a CORS preflight, and the scheme is not CORS-enabled, so the preflight cannot succeed.
<br>• `no-cors` requests cannot carry custom headers at all.
<br>• When the frame URL is known it must also be the same `orbit://` host. It is not relied on, because the IO thread can see it empty during navigation; that race was found by a looped self-test.
<br>• State changes require POST.
<br>Internal pages send a strict CSP (`default-src 'none'`, `frame-ancestors 'none'`). Page data is rendered with `textContent`, and JSON is written by CEF's JSON writer | `services` self-test: a hostile http page tries to read `orbit://history/api/list`, to POST a history clear with `no-cors`, and to POST it *with* the gate header. All three are blocked and the history is intact |
| Renderer → browser IPC | The only message accepted from renderers (`orbit.media-reply`) is read as a single bool; anything else is ignored. The renderer side only answers queries from `PID_BROWSER` | Code review (`BrowserClient::OnProcessMessageReceived`) |
| Typed input → navigation | The address bar never treats `javascript:` or `vbscript:` as a URL (they become searches). Unknown schemes become searches | Unit tests `Omnibox_*` |
| Page → OS protocol handlers | `OnProtocolExecution` always refuses Chromium's own launch. Orbit asks the user on the UI thread and launches via `ShellExecuteEx` only after validation: no control characters or quotes, a syntactically valid scheme, and a deny-list (`file`, `ms-msdt`, `search-ms`, `ms-appinstaller`, `hcp`, …) | Unit tests `ExternalScheme_*` |
| Popups | Script popups without a user gesture are blocked. Allowed popups become tabs in the opener's profile, keeping `window.opener` (needed by sign-in flows) | `basic` self-test |
| Downloads | Files are written only inside the chosen directory: the suggested name is reduced to its last path component and collisions are renamed. Mark-of-the-Web (`Zone.Identifier`) is verified and added if missing (private downloads record no source URL). Opening an executable type asks for confirmation first, then uses `ShellExecuteEx`, so SmartScreen and attachment policy apply | `services` self-test: MOTW present, `file (1).bin` on collision |
| Permissions | Default deny. Camera, microphone, location, clipboard and similar features need an explicit Allow from a native prompt. Decisions are stored per profile and origin; private-window decisions are kept in memory only. Notifications, VR/AR, protocol-handler registration and web-app install are always denied (unsupported in Alloy style) | `security` self-test |
| Certificates | No `OnCertificateError` override: Chromium's default cancels the request and shows an error page. There is no "proceed anyway". Mixed or broken HTTPS shows a warning indicator | `security` self-test (needs internet: `expired.badssl.com`) |
| JS dialogs | `alert`/`confirm`/`prompt` from background tabs are suppressed, so a hidden tab cannot pop dialogs over the active page | Code review |
| Local data | Orbit stores no credentials. The SQLite metadata DB holds history, bookmarks, tab URLs and preferences, unencrypted, in the user's profile directory (same trust level as Chromium's own history DB) | — |
| Extensions | Chrome's own extension system (MV3, permissions shown by Chrome before install, per-profile storage). Orbit adds: extension pages (`chrome-extension://`) cannot reach `orbit://` APIs (no gate header possible cross-origin, frame check kept); the bridge page is locked to `chrome://extensions` and its console channel is read only from that browser; action popups load only `chrome-extension://<id>/<manifest popup path>` after a path check (no scheme, `..`, backslash); private windows run no extensions; the Web Store brand override applies only to the store's own pages | `extensions` self-test: an extension page and a web page both fail to call `orbit://history/api/list`; a private window's page gets no content script; ids and popup paths are validated |
| Logging | Orbit's log never contains URLs, titles or form data above verbose level | Code review (`core/log.h` rule) |
| Updates | Orbit asks only `https://api.github.com/repos/mazijoni/orbit_browser/releases/latest` (WinHTTP, TLS 1.2+, no cookies, never redirected to HTTP).
<br>• The setup is downloaded only after a click, from the release asset's HTTPS link, and must match the SHA-256 GitHub lists for the asset.
<br>• It runs only after the user picks "Restart to update".
<br>• The asset is unsigned, so the trust is in the GitHub account and the release | `zen` self-test (version comparison; the live check is informational) |
| Crash diagnostics | orbit.log gets the faulting module + offset and the stack frames (no page data). An unhandled exception writes a small minidump (thread stacks, `MiniDumpNormal`) to `<User Data>\Crashes`; five are kept and none is uploaded | Manual: a forced `ExitProcess` logged its caller |

## Limitations inherited from CEF (Alloy style) / this version

* **No Safe Browsing.** Google Safe Browsing is not available to CEF applications. Malicious-site and download-reputation warnings come only from Windows SmartScreen when files are opened.
* **No password manager UI beyond Orbit's own** (saved logins in Settings, autofill, the save-on-sign-in toast). Chrome's own password manager, browser sign-in and leak check are turned off, no address or card autofill. Chrome extensions are supported (architecture.md §3) with the limits listed there.
* **Web Audio** playback is not detected by the sleep policy's media check. A tab playing only through Web Audio can be put to sleep. The policy is off by default.
* **Clearing site data:** cookies and Orbit's own history can be cleared per profile. Cache and other site storage have no CEF API, so they are cleared by deleting the profile.
* **Updates:** Chromium security fixes arrive only through new CEF releases. See the update strategy in architecture.md §12. The CEF pin must be bumped regularly.
* The metadata DB is not encrypted at rest. That is acceptable for its contents, but full-disk encryption (BitLocker) is the recommended protection.

## Review checklist (run before each release)

- [ ] `rg -n "no-sandbox|disable-web-security|single-process|disable-site-isolation|ignore-certificate-errors" src` finds nothing
- [ ] `pwsh scripts/run_selftests.ps1 -Config Release`: all suites pass (`isolation`, `services`, `security` cover the boundaries above)
- [ ] Unit tests pass (`ctest`)
- [ ] Every new `orbit://` endpoint either is read-only or checks `req.trusted_action`, and none returns another profile's data
- [ ] Every new renderer→browser message validates its argument types and count
- [ ] New external inputs (files, URLs, IPC) have a unit test for malformed input
- [ ] The CEF archive comes from a third-party build (psmux/godwinmix-cef, pinned by SHA-256); re-verify the pin and the source on every upgrade, or build your own
- [ ] CEF version is the latest stable (`cef-builds.spotifycdn.com/index.json`); release notes checked for security fixes
- [ ] Release binaries are code-signed; `bootstrap.exe` resources were modified *before* signing
