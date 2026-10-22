#pragma once

namespace orbit {

class Database;

inline constexpr int kSchemaVersion = 11;

// Brings the database schema up to kSchemaVersion. Each migration step runs in
// its own transaction and bumps PRAGMA user_version. Refuses to open databases
// created by a newer build (returns false) rather than risk corrupting them.
bool MigrateSchema(Database& db);

}  // namespace orbit
