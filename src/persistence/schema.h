#pragma once

namespace orbit {

class Database;

inline constexpr int kSchemaVersion = 13;

// Brings the database schema up to |target| (kSchemaVersion; tests stop at an
// older version to set up a database to upgrade). Each migration step runs in
// its own transaction and bumps PRAGMA user_version. Refuses to open databases
// created by a newer build (returns false) rather than risk corrupting them.
bool MigrateSchema(Database& db, int target = kSchemaVersion);

}  // namespace orbit
