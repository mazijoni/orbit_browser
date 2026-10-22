# Zen parity: keyboard shortcuts

Reference: `src/zen/kbs/ZenKeyboardShortcuts.mjs`, `zenGetDefaultShortcuts` plus migrations up to version 21, at commit `541ee55`. Firefox's standard keyset is included as well. "Accel" is Ctrl on Windows. In Zen every shortcut is user-configurable through the CKS pane; Orbit's are fixed.

Orbit rule (README): Ctrl+Alt combinations are avoided because Ctrl+Alt is AltGr on many layouts. Zen *does* use Ctrl+Alt for several defaults, and Firefox registers them the same way. This is a conflict to resolve deliberately (see the notes column).

| Zen default (Windows) | Command | Orbit today | Conflict / note | Status |
|---|---|---|---|---|
| Ctrl+S | Toggle compact mode (`cmd_toggleCompactModeIgnoreHover`). Zen moves Save Page to Ctrl+Alt+Shift+S | Ctrl+S toggles compact mode | Orbit has no Save Page, so Ctrl+S is free. Pages that use Ctrl+S (for example web editors) lose it, as in Zen | TESTED (manual, 2026-10-21) |
| Ctrl+Alt+S | Compact mode: show sidebar | Action "Compact mode: show sidebar", unbound by default | AltGr+S types a letter in several layouts (e.g. Polish ś), so Orbit does not bind it by default; assign it in Keyboard Shortcuts | IMPLEMENTED (unbound) |
| Ctrl+Alt+→ / Ctrl+Alt+← | Next / previous space (wrap-around) | Implemented with wrap-around; the invented Ctrl+Shift+PgUp/PgDn bindings were removed | The only Ctrl+Alt exception: AltGr+arrows type no characters | TESTED (manual: Home→Work→Study→Home) |
| (unbound) Switch to space 1–10 | `cmd_zenWorkspaceSwitchN` | Actions space-1…space-10, assignable in Keyboard Shortcuts | — | TESTED (self-test `zen`: bind Ctrl+Alt+2) |
| Ctrl+Alt+G / V / H / U | Split view grid / vertical / horizontal / unsplit | Same (`SplitWithNext`, `UnsplitCurrent`) | AltGr+G/V/H/U type no characters in common layouts (AltGr = Ctrl+Alt) | TESTED (self-test `zen`) |
| Ctrl+Shift+* | New empty split | Same (numpad `*` or Shift+8) | — | IMPLEMENTED |
| Ctrl+Shift+C | Copy current URL | `BrowserWindow::CopyCurrentUrl` + toast "Copied current URL!" (3 s) | Copies the typed text when the URL bar is being edited (as Zen's zenStrippedURI); no tracking-parameter stripping | TESTED (self-test `zen`, manual) |
| Ctrl+Shift+Alt+C | Copy URL as Markdown | Same: `[title](url)`, toast "Copied current URL as Markdown!" | — | TESTED (self-test `zen`) |
| Ctrl+Shift+D | Pin / unpin tab | `TogglePinActiveTab` | — | TESTED (self-test `zen`) |
| Ctrl+O | Expand Glance to full tab | Same; not handled without a glance | — | TESTED (self-test `zen`) |
| Ctrl+Shift+K | Close all unpinned tabs | `ClearUnpinnedTabs` (same as the Clear button) + toast with undo hint | — | TESTED (self-test `zen`, manual) |
| Ctrl+Shift+N | New unsynced (blank) window | New normal window (Orbit windows never share tabs, so all are unsynced) | Was Orbit's private-window key; changed to Zen's mapping with the user's approval | TESTED (self-test `zen`) |
| Ctrl+Shift+P | New private window | New private window; menus show Ctrl+Shift+P | — | TESTED (self-test `zen`) |
| Alt+Shift+L | Toggle Library | Same (also in the panel and in pages) | — | TESTED (self-test `zen`) |
| (unbound) Duplicate tab, Create space, Toggle sidebar, Reset pinned tab | — | Actions duplicate-tab, create-space, show-sidebar, reset-pinned-tab (assignable) | Toggle sidebar = Orbit's Ctrl+Shift+S collapse | IMPLEMENTED |
| Ctrl+T | New tab → floating URL bar (replace-newtab) | Opens the bar in new-tab mode; Enter creates the tab; Escape / Ctrl+T again cancels; typed text kept 45 s | — | TESTED (self-tests `zen`, `basic`; manual) |
| Ctrl+Shift+T | Reopen closed tab(s) | Reopens the whole last batch (schema v3 `closed_tabs.batch_id`) | Restored tabs are appended, not put back at their old index or folder | TESTED (self-test `zen`, unit tests) |
| Ctrl+W on a pinned tab | `onCloseTabShortcut` (close-shortcut behavior) instead of closing | `CloseTabShortcut` → `PinnedCloseShortcut` | — | TESTED (self-test `zen`, default behavior) |
| Ctrl+L / Alt+D / F6 | Open URL bar (floating, `floating-on-type`) | `OpenLocation` | — | TESTED (self-test `zen`) |
| Ctrl+W, Ctrl+Shift+T, Ctrl+Tab, Ctrl+1…9, Ctrl+R, F5, Alt+←/→, Ctrl+N, Ctrl+±/0, F11, F12, Ctrl+D, Ctrl+H, Ctrl+J | Firefox standard set | Implemented | Ctrl+Shift+O (bookmarks) and Ctrl+Shift+I (devtools) are Firefox's too; Firefox's devtools Inspector moved to Ctrl+Shift+L in Zen | TESTED (self-test `basic` for tab keys) |
| Ctrl+Shift+S (Orbit: collapse sidebar) | — | Orbit-only | Not a Zen default; Zen has no default "collapse" key | Orbit extension |
