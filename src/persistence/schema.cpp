#include "persistence/schema.h"

#include "core/log.h"
#include "persistence/database.h"

namespace orbit {
namespace {

// v1: browser organisation and session state.
constexpr const char* kMigrationV1 = R"sql(
CREATE TABLE profiles (
  id INTEGER PRIMARY KEY,
  uuid TEXT NOT NULL UNIQUE,
  name TEXT NOT NULL,
  color INTEGER NOT NULL,
  icon TEXT NOT NULL DEFAULT '',
  position INTEGER NOT NULL,
  created_at INTEGER NOT NULL
);
CREATE TABLE workspaces (
  id INTEGER PRIMARY KEY,
  profile_id INTEGER NOT NULL REFERENCES profiles(id) ON DELETE CASCADE,
  name TEXT NOT NULL,
  color INTEGER NOT NULL,
  icon TEXT NOT NULL DEFAULT '',
  position INTEGER NOT NULL
);
CREATE INDEX workspaces_profile ON workspaces(profile_id, position);
CREATE TABLE tab_groups (
  id INTEGER PRIMARY KEY,
  workspace_id INTEGER NOT NULL REFERENCES workspaces(id) ON DELETE CASCADE,
  name TEXT NOT NULL,
  color INTEGER NOT NULL,
  collapsed INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE tabs (
  id INTEGER PRIMARY KEY,
  workspace_id INTEGER NOT NULL REFERENCES workspaces(id) ON DELETE CASCADE,
  url TEXT NOT NULL,
  title TEXT NOT NULL DEFAULT '',
  favicon_url TEXT NOT NULL DEFAULT '',
  pinned INTEGER NOT NULL DEFAULT 0,
  protected INTEGER NOT NULL DEFAULT 0,
  group_id INTEGER REFERENCES tab_groups(id) ON DELETE SET NULL,
  position INTEGER NOT NULL,
  last_active INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX tabs_workspace ON tabs(workspace_id, position);
CREATE TABLE windows (
  id INTEGER PRIMARY KEY,
  profile_id INTEGER NOT NULL REFERENCES profiles(id) ON DELETE CASCADE,
  workspace_id INTEGER REFERENCES workspaces(id) ON DELETE SET NULL,
  active_tab_id INTEGER,
  x INTEGER NOT NULL, y INTEGER NOT NULL, w INTEGER NOT NULL, h INTEGER NOT NULL,
  maximized INTEGER NOT NULL DEFAULT 0,
  position INTEGER NOT NULL
);
CREATE TABLE closed_tabs (
  id INTEGER PRIMARY KEY,
  profile_id INTEGER NOT NULL REFERENCES profiles(id) ON DELETE CASCADE,
  workspace_id INTEGER,
  url TEXT NOT NULL,
  title TEXT NOT NULL DEFAULT '',
  closed_at INTEGER NOT NULL
);
CREATE INDEX closed_tabs_profile ON closed_tabs(profile_id, closed_at);
CREATE TABLE prefs (
  scope INTEGER NOT NULL,
  key TEXT NOT NULL,
  value TEXT NOT NULL,
  PRIMARY KEY (scope, key)
) WITHOUT ROWID;
)sql";

// v2: browser services (history, bookmarks, downloads).
constexpr const char* kMigrationV2 = R"sql(
CREATE TABLE history (
  id INTEGER PRIMARY KEY,
  profile_id INTEGER NOT NULL REFERENCES profiles(id) ON DELETE CASCADE,
  url TEXT NOT NULL,
  title TEXT NOT NULL DEFAULT '',
  visit_time INTEGER NOT NULL
);
CREATE INDEX history_profile_time ON history(profile_id, visit_time DESC);
CREATE TABLE bookmarks (
  id INTEGER PRIMARY KEY,
  profile_id INTEGER NOT NULL REFERENCES profiles(id) ON DELETE CASCADE,
  parent_id INTEGER REFERENCES bookmarks(id) ON DELETE CASCADE,
  is_folder INTEGER NOT NULL DEFAULT 0,
  title TEXT NOT NULL DEFAULT '',
  url TEXT NOT NULL DEFAULT '',
  position INTEGER NOT NULL,
  created_at INTEGER NOT NULL
);
CREATE INDEX bookmarks_parent ON bookmarks(profile_id, parent_id, position);
CREATE TABLE downloads (
  id INTEGER PRIMARY KEY,
  profile_id INTEGER NOT NULL REFERENCES profiles(id) ON DELETE CASCADE,
  url TEXT NOT NULL,
  path TEXT NOT NULL DEFAULT '',
  mime_type TEXT NOT NULL DEFAULT '',
  total_bytes INTEGER NOT NULL DEFAULT -1,
  received_bytes INTEGER NOT NULL DEFAULT 0,
  state INTEGER NOT NULL,
  started_at INTEGER NOT NULL,
  ended_at INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX downloads_profile ON downloads(profile_id, started_at DESC);
)sql";

// v3: tabs closed together (close all unpinned, close others, ...) share a
// batch id so that one "reopen closed tab" restores all of them, as in Firefox.
constexpr const char* kMigrationV3 = R"sql(
ALTER TABLE closed_tabs ADD COLUMN batch_id INTEGER NOT NULL DEFAULT 0;
)sql";

// v4: the URL a pinned tab returns to when it is reset (Zen's pinned initial
// state). Existing pinned tabs get their current URL when loaded.
constexpr const char* kMigrationV4 = R"sql(
ALTER TABLE tabs ADD COLUMN pinned_url TEXT NOT NULL DEFAULT '';
)sql";

// v5: a label the user gave a tab (Zen's "Change Label…", zenStaticLabel).
constexpr const char* kMigrationV5 = R"sql(
ALTER TABLE tabs ADD COLUMN custom_title TEXT NOT NULL DEFAULT '';
)sql";

// v6: a profile's Essentials live in a workspace row of kind 1, which is
// never shown as a space (Zen's essentials are shared by all spaces of a
// container; Orbit's profiles play the container's role).
constexpr const char* kMigrationV6 = R"sql(
ALTER TABLE workspaces ADD COLUMN kind INTEGER NOT NULL DEFAULT 0;
)sql";

// v7: split views (Zen's split view groups). `layout` is the layout tree in
// SerializeSplit form, with tab ids as leaves.
constexpr const char* kMigrationV7 = R"sql(
CREATE TABLE split_views (
  id INTEGER PRIMARY KEY,
  workspace_id INTEGER NOT NULL REFERENCES workspaces(id) ON DELETE CASCADE,
  layout TEXT NOT NULL
);
CREATE INDEX split_views_workspace ON split_views(workspace_id);
)sql";

// v8: nested folders (ZenFolders subfolders) and folder icons.
constexpr const char* kMigrationV8 = R"sql(
ALTER TABLE tab_groups ADD COLUMN parent_id INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tab_groups ADD COLUMN icon TEXT NOT NULL DEFAULT '';
)sql";

// v9: live folders (src/zen/live-folders): a folder's provider state and
// dismissed items; tabs remember the item they show and its sublabel.
constexpr const char* kMigrationV9 = R"sql(
ALTER TABLE tabs ADD COLUMN live_item TEXT NOT NULL DEFAULT '';
ALTER TABLE tabs ADD COLUMN sublabel TEXT NOT NULL DEFAULT '';
CREATE TABLE live_folders (
  group_id INTEGER PRIMARY KEY REFERENCES tab_groups(id) ON DELETE CASCADE,
  state TEXT NOT NULL,
  dismissed TEXT NOT NULL DEFAULT ''
);
)sql";

// v10: a space's theme from the gradient picker (SerializeSpaceTheme; empty:
// the plain space colour).
constexpr const char* kMigrationV10 = R"sql(
ALTER TABLE workspaces ADD COLUMN theme TEXT NOT NULL DEFAULT '';
)sql";

// v11: boosts (src/zen/boosts), one per site and profile (SerializeBoost).
constexpr const char* kMigrationV11 = R"sql(
CREATE TABLE boosts (
  profile_id INTEGER NOT NULL REFERENCES profiles(id) ON DELETE CASCADE,
  domain TEXT NOT NULL,
  data TEXT NOT NULL,
  PRIMARY KEY (profile_id, domain)
);
)sql";

constexpr const char* kMigrations[] = {kMigrationV1, kMigrationV2, kMigrationV3, kMigrationV4,
                                       kMigrationV5, kMigrationV6, kMigrationV7, kMigrationV8,
                                       kMigrationV9, kMigrationV10, kMigrationV11};
static_assert(sizeof(kMigrations) / sizeof(kMigrations[0]) == kSchemaVersion);

}  // namespace

bool MigrateSchema(Database& db) {
  const int current = db.UserVersion();
  if (current < 0) return false;
  if (current > kSchemaVersion) {
    ORBIT_LOG_ERROR("database schema v%d is newer than supported v%d", current,
                     kSchemaVersion);
    return false;
  }
  for (int version = current; version < kSchemaVersion; ++version) {
    Database::Transaction txn(db);
    if (!txn.active() || !db.Execute(kMigrations[version]) ||
        !db.SetUserVersion(version + 1) || !txn.Commit()) {
      ORBIT_LOG_ERROR("migration to schema v%d failed", version + 1);
      return false;
    }
    ORBIT_LOG_INFO("migrated database to schema v%d", version + 1);
  }
  return true;
}

}  // namespace orbit
