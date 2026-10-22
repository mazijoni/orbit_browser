# Orbit

Orbit is a Chromium-based desktop browser for Windows 11 that looks and works like [Zen Browser](https://github.com/zen-browser/desktop). Zen is built on Firefox; Orbit is built on Chromium.

* Zen's single-toolbar layout: a vertical sidebar holds the navigation buttons, the address bar, the current space's name, pinned tabs, a **+ New Tab** button and the tab list. A footer has downloads, a switcher with one dot per space, and a **+** menu.
* The web page sits in a rounded, shadowed card on a backdrop tinted by the space's color.
* A frameless window. The window controls appear when the pointer reaches the top edge.
* A floating, centered URL bar for new tabs (Ctrl+T), and for Ctrl+L when the sidebar's address bar is hidden.
* **Compact mode**: the sidebar hides and slides in from the left edge. The sidebar can also be **collapsed** to icons.
* Spaces (workspaces), isolated profiles, pinned tabs with Zen's **Clear** button, and folders.

It is built on the Chromium Embedded Framework (CEF 152, Chromium 152) with a native Win32 + Direct2D interface. Chromium's sandbox and site isolation stay at their defaults.

* **Build:** [docs/build.md](docs/build.md)
* **Architecture decisions:** [docs/architecture.md](docs/architecture.md)
* **Status:** [docs/progress.md](docs/progress.md)

```powershell
pwsh ./scripts/fetch_deps.ps1 && pwsh ./scripts/build.ps1 && out/debug/bin/orbit.exe
```

## Layout

```
src/core/         platform-independent helpers (URL fix-up, strings, logging)    [no CEF]
src/persistence/  SQLite wrapper, schema migrations, typed store                 [no CEF]
src/browser/      CEF integration: client, tabs, profiles/workspaces, orbit:// pages
src/ui/           Win32 window, Direct2D drawing, theme tokens
src/app/          entry point, Application controller, self-test runner
tests/unit/       unit tests for core + persistence (ctest)
scripts/          fetch_deps, build, run_selftests
tools/            developer helpers (icon generator, window capture/click/close,
                  UIA accessibility check, resource stamper, benchmark page server)
```

## Keyboard shortcuts

| Keys | Action |
|---|---|
| Ctrl+L, Ctrl+K, Alt+D, F6 | Open the floating URL bar for the current tab (clicking the sidebar field edits it in place; F6 toggles back to the page) |
| Ctrl+T / Ctrl+W | New tab: the floating URL bar opens and Enter creates the tab (press Ctrl+T again or Escape to cancel) / close tab |
| Ctrl+Shift+T | Reopen the last closed tab, or every tab of the last multi-tab close |
| Ctrl+Shift+C | Copy the current URL |
| Ctrl+Shift+K | Close all unpinned tabs in the space (keeps the selected tab and tabs playing media) |
| Ctrl+Shift+D | Pin / unpin the current tab |
| Ctrl+Tab, Ctrl+PgDn / Ctrl+Shift+Tab, Ctrl+PgUp | Next / previous tab |
| Ctrl+1…8, Ctrl+9 | Go to tab *n* / the last tab |
| Ctrl+Alt+→ / Ctrl+Alt+← | Next / previous space, wrapping around (Zen). Scrolling sideways over the sidebar also switches spaces |
| Ctrl+R, F5 / Ctrl+Shift+R, Shift+F5 | Reload / reload without cache |
| Alt+Left / Alt+Right | Back / forward |
| Ctrl+N / Ctrl+Shift+N | New window / new private window |
| Ctrl+S | Compact mode (hide the sidebar; it slides in from the left edge), as in Zen |
| Ctrl+Shift+S | Collapse the sidebar to icons |
| Ctrl+Plus / Ctrl+Minus / Ctrl+0 | Zoom |
| F11 | Full screen |
| F12, Ctrl+Shift+I | Developer tools |
| ↑ ↓ Home End Enter Del, Menu key | Navigate the sidebar when it has focus (← → collapse/expand a folder) |
| Ctrl+D / Ctrl+H / Ctrl+J / Ctrl+Shift+O | Bookmark page / history / downloads / bookmarks |

Ctrl+Alt is AltGr on many keyboard layouts, so Ctrl+Alt combinations are not used, with one exception: Zen's Ctrl+Alt+←/→, because AltGr+arrow types no character. Full shortcut parity status: [docs/zen-parity/KEYBOARD_SHORTCUTS.md](docs/zen-parity/KEYBOARD_SHORTCUTS.md).

## Credits

The interface follows Zen Browser's design. Its layout metrics, colors, the gradient formula and behaviors were taken from Zen's stylesheets and scripts (`src/zen/**`, Mozilla Public License 2.0, https://github.com/zen-browser/desktop) and reimplemented natively. No Zen source files are included.

Zen's icons are Nucleo icons. Their license does not allow redistribution, so Orbit uses the Segoe Fluent Icons that ship with Windows. Orbit does not use Zen's name or logo.
