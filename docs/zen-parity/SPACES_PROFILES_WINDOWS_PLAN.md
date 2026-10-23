# Plan: profiles per space, linked window spaces, per-space Essentials

Written 2026-10-22, at the end of session 5, for a later session.

**Status (2026-10-22, session 6): implemented** with the defaults of the open
questions below (tabs come back in a folder; one window shows every profile's
spaces; the shared Essentials were copied into every space). See
IMPLEMENTATION_LOG.md, "Session 6". Still open: the drag-out check with a real
mouse.

## How to resume

Give Claude this file and say something like:

> Read plan.md, docs/zen-parity/IMPLEMENTATION_LOG.md and
> docs/zen-parity/SPACES_PROFILES_WINDOWS_PLAN.md, then implement the plan in
> the order of the parts below. Inspect the code before you change it. Keep the
> inventories and the log updated. Build and test after every part.

State when this was written: unit tests 91/91, self-tests 9/9 (basic, isolation,
services, memory, security, zen, restore-1/2/3), UIA 4/4. Schema version 11.

## What the user asked for (their words, lightly edited)

1. **Containers become Chromium profiles.** Zen can tie a space to a Firefox
   container. Orbit should use Chromium profiles for this instead.
2. **Window sync becomes linked spaces.** When you drag a tab out of the
   window, or move it to a new window, it turns into a separate space. That
   space stays linked to its original space and uses the same history,
   passwords and profile. "It's basically a temp folder from the space."
3. **Essentials belong to each space.** They are not shared between spaces.

## How Orbit works today (checked in the code)

- `Profile` owns its `Workspace`s (spaces), and each space owns its `Tab`s
  (`src/browser/profile.h`). Each profile has one `CefRequestContext`. That
  gives it its own cookies, storage, cache and permissions. History,
  bookmarks and the downloads list are Orbit's own per-profile tables.
- A window shows exactly one profile (`BrowserWindow::profile_`, about 128
  uses in `src/ui/browser_window.cpp`) and one space at a time. A space is
  shown in at most one window. See `docs/architecture.md` §5.
- A tab can only move between windows of the same profile
  (`BrowserWindow::MoveTabToWindow`). Chromium fixes a browser's request
  context when it is created, so a tab cannot change profile without being
  reloaded.
- Essentials are per profile. They live in one workspace row with
  `kind = kKindEssentials` (schema v6, `Profile::essentials()`,
  `EnsureEssentials()`, `IsEssential()`). They are shown in every space of the
  profile. `BrowserWindow::active_essential_` and `last_essential_`
  (Zen's lastSelectedWorkspaceTabs) track which one is on screen. There are
  about 25 uses in browser_window.cpp, plus profile.cpp, application.cpp,
  browser_window_paint.cpp, internal_api.cpp:1263 and 6 in self_test.cpp.
- "Move to → New window" (`kCmdMoveToNewWindow`) opens a window on a free or
  new space of the same profile (`Application::FreeWorkspaceFor`). Dragging a
  tab out of the window does nothing yet. `OnLButtonUp` only handles drops
  inside the sidebar.
- Zen reference: `zen.workspaces.separate-essentials` (default true) separates
  Essentials per container. Window sync is `src/zen/sync` with
  `zen.window-sync.enabled`.

## Part 1: Essentials per space (do this first; the other parts rely on it)

Goal: each space has its own Essentials grid, with up to `kMaxEssentials`
per space.

- **Schema v12:** add `workspaces.owner_id INTEGER REFERENCES workspaces(id)
  ON DELETE CASCADE`. An Essentials row (`kind = 1`) belongs to the space whose
  id is in `owner_id`.
- **Migration:** copy the profile's existing Essentials into every space of
  that profile, so nothing disappears from any space. Then delete the old
  shared row. Tabs are rows, so copying the rows is enough; copies load
  lazily.
- **Model:** move `essentials()` / `EnsureEssentials()` from `Profile` to
  `Workspace`. `Profile::IsEssential(tab)` becomes
  `tab->workspace()->kind() == kEssentials`, with the owner looked up from the
  tab.
- **Window:** `active_essential_` belongs to the current space. Switching
  spaces hides the old space's Essentials and shows the new one's.
  `last_essential_` can stay; it now only ever points into that space's own
  grid.
- **Other places:** deleting a space deletes its Essentials (the cascade
  does this). "Move space to profile" (Part 2) moves them too. Ctrl+Tab
  (`SelectRelativeTab`) and the paint code use the current space's grid.
- **Tests:**
  - Unit test the migration (an old database with one shared grid ends up
    with a copy in each space).
  - In the `zen` self-test, an Essential added in space A is not shown in
    space B.
  - Update restore-1/2 so the Essential comes back in its own space.
- **Docs:** update the FEATURE_INVENTORY Essentials row. Add a
  SETTINGS_INVENTORY note that `separate-essentials` is always on.

## Part 2: a space can use any profile (Zen's containers)

Goal: one window can switch between spaces that belong to different
profiles. Each space shows its profile, and a space can be moved to another
profile.

- **Model:** keep `Profile` as the owner of spaces; `workspaces.profile_id`
  already exists. Change `BrowserWindow` so its current profile is always
  `workspace_->profile()`. Swap `profile_` in `ShowWorkspace` and check every
  `profile_` use. Things like history, bookmarks, site data, downloads and
  the library must follow the current space's profile.
- **Space switcher:** the footer icons and the space menu list the spaces of
  every normal profile, in one global order. A new `workspaces.position`
  scope would replace the per-profile order. Private windows still show only
  their own space.
- **Badge:** draw a small profile-colour dot on the space icon when the
  space's profile is not the default one. Zen marks container spaces with the
  container colour.
- **Space menu:** add a "Profile ▸" submenu listing the profiles plus
  "New profile…", like Zen's container picker in the space editor. Moving a
  space to another profile:
  - Unload all its tabs. Their cookies don't carry over; the tabs reload in
    the new context when selected.
  - Update `profile_id` on the space, its tabs and its Essentials.
  - Show a toast saying the tabs will reload signed out.
- **Moving tabs:** `MoveTabToWindow` and `MoveTabToWorkspace` across profiles
  become allowed. They unload the tab and recreate it in the target profile.
  Live browsers can't move. Give the user a warning, as above.
- **Routing:** `RouteTarget` already resolves to a space, so it now opens the
  tab in that space's profile.
- **Tests:**
  - Isolation self-test: two spaces in one window, on two profiles, can't
    see each other's localStorage (reuse the `isolation` suite's
    `op=write/read` pages).
  - Moving a space to another profile unloads it, and after a reload it reads
    the new profile's storage.
  - Restore: the space and its profile survive a restart.
- **Keep:** private windows stay as they are, and the sandbox is unchanged.
  Profile directories stay direct children of the user-data root (the
  verified constraint in architecture.md §5).

## Part 3: linked window spaces (instead of Zen's window sync)

Goal: tearing a tab off gives a new window with a temporary space linked
to the original. The linked space uses the same profile, so history,
passwords and cookies are the same.

- **Schema:** use the same v12 migration. Add `workspaces.parent_id INTEGER
  REFERENCES workspaces(id) ON DELETE SET NULL` and a new
  `kind = kKindLinked (2)`.
- **Creating one:**
  - (a) Drag a tab and release it outside every Orbit window. Detect this in
    `OnLButtonUp` while `dragging_tab_`, using `WindowFromPoint`. Open the new
    window at the cursor.
  - (b) "Move to → New window".
  - Both create a linked space with `parent_id` set to the source space. It
    has the same profile and inherits the parent's icon and theme. It is
    named "<parent name> · Window N". The tab moves into it with
    `MoveTabToWindow`, so its live browser is reparented and keeps its page
    state.
  - Releasing a dragged tab over another Orbit window moves it into that
    window's space, as Move to → that window does today.
- **In the parent window:** the parent space shows the linked space as a
  special folder row, the "temp folder". It has a window icon, the name and
  the tab count. It is always collapsed. Clicking it focuses the linked
  window. Its context menu has "Bring tabs back" and "Close window". Dragging
  a tab onto it moves the tab into the linked window.
- **In the linked window:**
  - The sidebar shows a header "Linked to <parent>" with a back arrow that
    brings all tabs back to the parent and closes the window.
  - The space switcher shows the parent's spaces. Switching to another space
    from here is allowed; the linked space stays attached to this window.
  - Essentials: the parent's grid (Part 1). An Essential is a single tab, so
    activating it in either window reparents it, as Essentials do across
    windows today.
- **Closing the linked window:** its tabs return to the parent space, in a
  normal folder named after the linked space so they stay together. The
  user can unpack it. The linked space row is then deleted. An empty linked
  space is just deleted.
- **Restart:** the window and its linked space are restored like any other
  window (`windows` table plus the space row). If the parent was deleted,
  `parent_id` becomes NULL and the linked space turns into a normal space,
  `kind = 0`.
- **What stays out:** linked spaces are not listed as spaces of their own in
  the switcher, routing settings or Manage Spaces.
- **Tests (self-test `zen`, plus a restore phase):**
  - Move to → New window creates a linked space with the same profile and
    the parent's id.
  - The tab keeps its browser and its page state. Check that its browser id
    is unchanged.
  - The parent shows the folder row with a count of 1.
  - Closing the linked window brings the tab back into a folder in the
    parent.
  - The linked space survives a restart.
  - Releasing a drag outside the window opens a new window. Check this by
    hand with a real mouse; the self-tests can't synthesise drags reliably.

## Open questions (defaults chosen; confirm at the start of the session)

1. When a linked window closes, should its tabs come back in a folder
   (default) or be closed? A folder keeps them, which fits "temp folder".
2. Should Part 2 show every profile's spaces in one window (default, closest
   to Zen's containers), or keep one profile per window and only add
   "Move space to profile"?
3. Migrating the shared Essentials: copy them into every space (default) or
   only into the first space?

## Order and checkpoints

1. Part 1, then run the full test set: `pwsh ./scripts/build.ps1`, then
   `pwsh ./scripts/run_selftests.ps1`, then `pwsh tools/uia_check.ps1`.
2. Part 2, then the full test set.
3. Part 3, then the full test set plus the manual drag check.
4. Update FEATURE_INVENTORY:
   - "Spaces vs containers/profiles": IMPLEMENTED/TESTED.
   - "Window sync": replace with "Linked window spaces" and its status.
   - Update architecture.md §5 for the new space↔profile↔window rules.
