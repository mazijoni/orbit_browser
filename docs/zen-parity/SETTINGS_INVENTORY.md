# Zen parity: settings inventory

Reference: `src/browser/components/preferences/zen*.inc.xhtml`, `zen-settings.js` and `prefs/zen/*.yaml` at commit `541ee55`. In Zen, settings open in a tab (`about:preferences`). Zen adds four panes to Firefox's own: **Look and Feel** (`paneZenLooks`), **Tab Management** (`paneZenTabManagement`), **Keyboard Shortcuts** (`paneZenCKS`) and **Mods** (`paneZenMarketplace`).
Orbit: `orbit://settings/` in a tab. Prefs live in the `prefs` table of `orbit.db` with scope 0 (global); the settings API is in `internal_api.cpp`.

Defaults below come from `prefs/zen/*.yaml`, which wins where `zen-settings.js` declares a different fallback.

## Look and Feel

| Setting (Zen pref) | Type / default | Effect | Orbit pref | Persisted | Scope | Status |
|---|---|---|---|---|---|---|
| Layout: single / multiple / collapsed toolbar (`zen.view.use-single-toolbar`, `zen.view.sidebar-expanded`) | cards / single + expanded | Changes the chrome layout live | Layout cards "Single toolbar" / "Collapsed toolbar" (orbit://settings → Look and Feel) → `sidebar_compact` | yes | app | IMPLEMENTED (cards checked on screen); the multiple-toolbar layout does not exist in Orbit (N/A) |
| New tab button in tab list (`zen.tabs.show-newtab-vertical`) | bool / true | Shows "+ New Tab" row | `newtab_button` | yes | app | TESTED (self-test `zen`) |
| New tab button at the top (`zen.view.show-newtab-button-top`) | bool / true | Places the row above the tabs (else after the last tab) | `newtab_button_top` | yes | app | TESTED (self-test `zen`) |
| Theme (Firefox web appearance + `zen.view.window.scheme`) | system=2 | Dark/light/system | `theme` (system/dark/light) | yes | app | TESTED |
| Compact mode: flash toolbar popup (`zen.view.compact.toolbar-flash-popup`) | bool / false | Briefly shows the toolbar on changes | Look and Feel checkbox | `compact_flash` | app | TESTED (self-test `zen`) |
| Drag window from content (`zen.view.drag-window-from-content`) | bool / true | Top 5% of page drags window | `drag_window_from_content` (applies to newly opened pages) | yes | app | IMPLEMENTED (behavior checked manually) |
| Glance enabled (`zen.glance.enabled`) + trigger (`zen.glance.activation-method`) | bool true / "alt" | `glance_enabled`, `glance_trigger` (alt/ctrl/shift; passed to each new browser's renderer, so it applies to newly opened pages) | yes | app | TESTED (self-test `zen`: Ctrl trigger with a real Ctrl+click) — meta is not offered on Windows |
| URL bar behavior (`zen.urlbar.behavior`) | normal / floating-on-type / float; default floating-on-type | When the URL bar floats | `urlbar_behavior`: normal (Ctrl+L / Ctrl+T open it in the sidebar), floating-on-type (keyboard floats, click edits in place), float (a click floats too) | yes | app | TESTED (self-test `zen`: floating-on-type and normal; float checked by code path only) |

## Tab Management

| Setting | Type / default | Orbit | Status |
|---|---|---|---|
| Sync only pinned tabs (`zen.window-sync.sync-only-pinned-tabs`) | bool / false | Window sync is not implemented; Orbit links a torn-off window's space to its parent instead (FEATURE_INVENTORY "Linked window spaces"), with nothing to sync | NOT APPLICABLE |
| Hide default container indicator (`zen.workspaces.hide-default-container-indicator`) | bool / true | Profiles play the containers' role; the default (first) profile's spaces never carry a profile dot, as with this setting on; there is no setting to show it | NOT APPLICABLE (always on) |
| Force container tabs to workspace (`zen.workspaces.force-container-workspace`) | bool / false | In Zen a container tab opened elsewhere jumps to its container's space; in Orbit a tab always belongs to its space's profile, so a tab cannot be in another profile's space | NOT APPLICABLE |
| Continue where left off (`zen.workspaces.continue-where-left-off`) | bool / false | Off: the session's tabs are restored (none loaded) and each window opens on the hidden empty tab with the URL bar open (ZenSpaceManager selects `_emptyTab`); on: the last selected tab. Settings > Startup checkbox (`continue_where_left_off`), read in `Application::Start` | TESTED (`restore-1` turns it on, `restore-2` checks the last tab, `restore-3` checks the empty tab with it off) |
| Close tab on Back with no history (`zen.tabs.close-on-back-with-no-history`) | bool / true | `close_on_back_no_history` (orbit://settings → Tab Management) | IMPLEMENTED (behavior TESTED; toggle wired, persisted in prefs) |
| Ctrl+Tab ignores pending tabs (`zen.tabs.ctrl-tab.ignore-pending-tabs`) | bool / false | `ctrl_tab_skip_pending` (Tab Management) | TESTED (self-test `zen`, both values) |
| Ctrl+Tab ignores essentials (`zen.tabs.ctrl-tab.ignore-essential-tabs`) | bool / false | Off: Ctrl+Tab passes through the Essentials and the space's tabs in strip order; on: it stays within the current tab's group | Tab Management checkbox "Ctrl+Tab cycles within Essential or Workspace tabs only" (`ctrl_tab_ignore_essentials`) | TESTED (self-test `zen`) |
| Select recently used on close (`zen.tabs.select-recently-used-on-close`) | bool / true | `select_recent_on_close` (orbit://settings → Tab Management) | TESTED (self-test `zen` covers both values) |
| Restore pinned tabs to pinned URL (`zen.pinned-tab-manager.restore-pinned-tabs-to-pinned-url`) | bool / false | `restore_pinned_to_pinned_url`, applied in `Profile::LoadFromStore` | TESTED (`restore-1` enables it, `restore-2` checks the pinned tab came back at its pinned page) |
| Container-specific essentials (`zen.workspaces.separate-essentials`, label "container-specific essentials") | bool / true | Always on, with no setting: every space has its own Essentials grid (schema v12 `workspaces.owner_id`), and a linked window space shows its parent's. Spaces can use any profile (Orbit's containers), so per-space covers Zen's per-container grids | NOT APPLICABLE (always on; TESTED by self-test `zen`) |
| Close-shortcut behavior on pinned tabs (`zen.pinned-tab-manager.close-shortcut-behavior`) | reset-unload-switch / unload-switch / reset-switch / switch / reset / close; default reset-unload-switch | `pinned_close_behavior` (select in orbit://settings → Pinned Tabs) | IMPLEMENTED (default behavior TESTED in `zen`; other values untested) |

## Keyboard Shortcuts pane

A per-shortcut editor with conflict detection and reset (`gZenCKSSettings`). Stored in `zen-keyboard-shortcuts.json`, versioned (v21). Orbit: IMPLEMENTED in orbit://settings (see FEATURE_INVENTORY "Custom keyboard shortcuts editor"); overrides in prefs `shortcut:<id>` (a chord or `none`).

## Mods pane

A marketplace of CSS mods (`zen.mods.auto-update`). Orbit's chrome is not CSS, so this is NOT APPLICABLE.

## Orbit-only settings (kept, plan.md Rule 1 "do not overwrite working functionality")

| Setting | Orbit pref | Status |
|---|---|---|
| Accent color | `accent` | TESTED (pre-existing) |
| Space theme color strength (Off / Subtle / Normal / Vivid) | `theme_strength` | IMPLEMENTED. Applies live; not yet restart-tested |
| Compact mode | `sidebar_hidden` | IMPLEMENTED |
| Collapse sidebar | `sidebar_compact` | TESTED (pre-existing) |
| Search engine | `search_template` | TESTED (pre-existing) |
| Tab sleep timeout | `sleep_minutes` | TESTED (pre-existing) |
| Ask where to save downloads | `download_ask` | TESTED (pre-existing) |
| Site permissions, clear data | — | TESTED (pre-existing) |
| Show search suggestions (`browser.search.suggest.enabled`) | bool / false | Engine suggestions in the URL bar | Search section checkbox | `search_suggestions` | app | TESTED (self-test `zen`) |
