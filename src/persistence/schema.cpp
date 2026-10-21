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

constexpr const char* kMigrations[] = {kMigrationV1, kMigrationV2};
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
