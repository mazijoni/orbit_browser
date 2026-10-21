# Progress

Legend: ✅ implemented and verified · 🟡 partial · ⬜ not started

| Milestone | State | Notes |
|---|---|---|
| 1 Environment and architecture | ✅ | CEF 152.0.14 / SQLite 3.54.1 pinned; ADR in `architecture.md` |
| 2 Minimal browser | ✅ | Real sites render; address bar, navigation, progress, site identity; clean shutdown |
| 3 Vertical tabs | ✅ | Sidebar, compact mode, drag reorder, context menus, keyboard navigation, tooltips |
| 4 Profiles and workspaces | ✅ | Isolation, restore, workspaces, profiles; tab groups (collapse, color, rename, drag in/out, keyboard, persisted; `basic` + `restore` self-tests) |
| 5 Browser services | ✅ | History, bookmarks (Netscape import/export), downloads (MOTW, pause/resume), settings, new-tab page, recently closed |
| 6 Memory | 🟡 | Sleep policy with audio and camera/mic protection, diagnostics page, benchmark script. **Background-tab throttling unavailable** (CEF limitation, architecture.md §6). Results in `docs/benchmarks/` |
| 7 Security and polish | 🟡 | Permission prompts, cert-error test, orbit:// API hardening (header gate; race found and fixed by a looped test), `security.md`. Accessibility: UI Automation provider for the custom-drawn chrome (names, states, invoke/select, focus events) and a named address bar, verified with the native UIA client (`tools/uia_check.ps1`). Not done: dark-mode Win32 context menus (only possible through undocumented APIs) |
| 8 Distribution | 🟡 | `package.ps1` (zip, import check, relocated smoke test), `--install/--uninstall` verified round trip, icon/version stamping. **Unsigned**; no auto-update; not tested on a separate clean machine |
| 9 Zen-style interface | ✅ | Renamed to Orbit. Frameless window with window controls revealed on hover; Zen single-toolbar sidebar (address bar, space indicator, pinned separator with Clear, New Tab, folders, footer space switcher); themed gradient backdrop; rounded content card; floating URL bar; compact mode and collapsed sidebar. Not done: essentials grid, split view, glance, URL suggestions, Zen-styled popup menus, animations |

## Tests
* `pwsh scripts/build.ps1`: 29 unit tests
* `pwsh scripts/run_selftests.ps1`: 7 in-browser suites (basic, isolation, services, memory, security, restore-1/2), all passing on Debug and Release
* `pwsh tools/uia_check.ps1`: accessibility through Windows UI Automation

## Next steps
1. Rerun the benchmark when enough memory is free (the corrected run was stopped by the OS low-memory guard), analyse it (`docs/benchmarks/`) and complete the results section of `performance.md`. Explain the renderer-process count gap: Orbit uses 26 processes at 50 same-site tabs, Edge 56.
2. Code signing (needs a certificate); signed update-manifest check (needs a hosting endpoint).
3. A manual Narrator pass (the automated check covers names, roles, selection and invoke).
