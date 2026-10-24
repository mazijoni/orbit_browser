#include "persistence/database.h"

#include <sqlite3.h>

#include <utility>

#include "core/log.h"
#include "core/strings.h"

namespace orbit {

Database::Statement::Statement(Statement&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)),
      stmt_(std::exchange(other.stmt_, nullptr)),
      failed_(other.failed_) {}

Database::Statement& Database::Statement::operator=(Statement&& other) noexcept {
  if (this != &other) {
    if (stmt_) sqlite3_finalize(stmt_);
    db_ = std::exchange(other.db_, nullptr);
    stmt_ = std::exchange(other.stmt_, nullptr);
    failed_ = other.failed_;
  }
  return *this;
}

Database::Statement::~Statement() {
  if (stmt_) sqlite3_finalize(stmt_);
}

Database::Statement& Database::Statement::Bind(int index, int64_t value) {
  if (stmt_ && sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK) failed_ = true;
  return *this;
}

Database::Statement& Database::Statement::Bind(int index, double value) {
  if (stmt_ && sqlite3_bind_double(stmt_, index, value) != SQLITE_OK) failed_ = true;
  return *this;
}

Database::Statement& Database::Statement::Bind(int index, std::wstring_view value) {
  if (!stmt_) return *this;
  const std::string utf8 = ToUtf8(value);
  if (sqlite3_bind_text(stmt_, index, utf8.data(), static_cast<int>(utf8.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK) {
    failed_ = true;
  }
  return *this;
}

Database::Statement& Database::Statement::BindBlob(int index, const void* data, size_t size) {
  if (stmt_ && sqlite3_bind_blob(stmt_, index, data, static_cast<int>(size), SQLITE_TRANSIENT) != SQLITE_OK) {
    failed_ = true;
  }
  return *this;
}

Database::Statement& Database::Statement::BindNull(int index) {
  if (stmt_ && sqlite3_bind_null(stmt_, index) != SQLITE_OK) failed_ = true;
  return *this;
}

bool Database::Statement::Step() {
  if (!stmt_ || failed_) return false;
  const int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW) return true;
  if (rc != SQLITE_DONE) {
    failed_ = true;
    db_->RecordError("step");
  }
  return false;
}

bool Database::Statement::Run() {
  if (!stmt_ || failed_) {
    if (db_) db_->RecordError("run (invalid statement)");
    return false;
  }
  const int rc = sqlite3_step(stmt_);
  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    failed_ = true;
    db_->RecordError("run");
    return false;
  }
  return true;
}

void Database::Statement::Reset() {
  if (!stmt_) return;
  sqlite3_reset(stmt_);
  sqlite3_clear_bindings(stmt_);
  failed_ = false;
}

int64_t Database::Statement::ColumnInt64(int col) const {
  return stmt_ ? sqlite3_column_int64(stmt_, col) : 0;
}

double Database::Statement::ColumnDouble(int col) const {
  return stmt_ ? sqlite3_column_double(stmt_, col) : 0.0;
}

std::wstring Database::Statement::ColumnText(int col) const {
  if (!stmt_) return {};
  const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, col));
  const int len = sqlite3_column_bytes(stmt_, col);
  if (!text || len <= 0) return {};
  return FromUtf8(std::string_view(text, static_cast<size_t>(len)));
}

std::vector<uint8_t> Database::Statement::ColumnBlob(int col) const {
  if (!stmt_) return {};
  const auto* data = static_cast<const uint8_t*>(sqlite3_column_blob(stmt_, col));
  const int len = sqlite3_column_bytes(stmt_, col);
  if (!data || len <= 0) return {};
  return std::vector<uint8_t>(data, data + len);
}

bool Database::Statement::ColumnIsNull(int col) const {
  return !stmt_ || sqlite3_column_type(stmt_, col) == SQLITE_NULL;
}

Database::Transaction::Transaction(Database& db) : db_(db) {
  active_ = db_.Execute("BEGIN IMMEDIATE");
}

Database::Transaction::~Transaction() {
  if (active_) db_.Execute("ROLLBACK");
}

bool Database::Transaction::Commit() {
  if (!active_) return false;
  active_ = false;
  if (!db_.Execute("COMMIT")) {
    db_.Execute("ROLLBACK");
    return false;
  }
  return true;
}

Database::~Database() { Close(); }

bool Database::Open(const std::filesystem::path& path) {
  Close();
  const std::string utf8 = path.empty() ? std::string(":memory:") : ToUtf8(path.wstring());
  const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
  if (sqlite3_open_v2(utf8.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
    RecordError("open");
    Close();
    return false;
  }
  sqlite3_busy_timeout(db_, 2000);
  if (!path.empty() && !Execute("PRAGMA journal_mode=WAL")) return false;
  return Execute("PRAGMA foreign_keys=ON") && Execute("PRAGMA synchronous=NORMAL");
}

void Database::Close() {
  if (db_) {
    sqlite3_close_v2(db_);
    db_ = nullptr;
  }
}

bool Database::Execute(std::string_view sql) {
  if (!db_) return false;
  char* err = nullptr;
  const std::string owned(sql);
  if (sqlite3_exec(db_, owned.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    last_error_ = err ? err : "unknown error";
    sqlite3_free(err);
    ORBIT_LOG_ERROR("sqlite exec failed: %s", last_error_.c_str());
    return false;
  }
  return true;
}

Database::Statement Database::Prepare(std::string_view sql) {
  if (!db_) return {};
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr) !=
      SQLITE_OK) {
    RecordError("prepare");
    return {};
  }
  return Statement(this, stmt);
}

int64_t Database::LastInsertId() const { return db_ ? sqlite3_last_insert_rowid(db_) : 0; }

int Database::Changes() const { return db_ ? sqlite3_changes(db_) : 0; }

int Database::UserVersion() {
  auto stmt = Prepare("PRAGMA user_version");
  return stmt.Step() ? stmt.ColumnInt(0) : -1;
}

bool Database::SetUserVersion(int version) {
  return Execute("PRAGMA user_version=" + std::to_string(version));
}

void Database::RecordError(std::string_view context) {
  last_error_ = std::string(context) + ": " + (db_ ? sqlite3_errmsg(db_) : "no database");
  ORBIT_LOG_ERROR("sqlite %s", last_error_.c_str());
}

}  // namespace orbit
