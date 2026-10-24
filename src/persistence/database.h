#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace orbit {

// Thin RAII wrapper around one SQLite connection. Not thread-safe: the browser
// uses it from the UI thread only. Errors are reported via return values and
// logged through last_error(); the code base is built without exceptions.
class Database {
 public:
  class Statement {
   public:
    Statement() = default;
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement();

    bool valid() const { return stmt_ != nullptr; }

    Statement& Bind(int index, int64_t value);
    Statement& Bind(int index, int value) { return Bind(index, static_cast<int64_t>(value)); }
    Statement& Bind(int index, bool value) { return Bind(index, static_cast<int64_t>(value)); }
    Statement& Bind(int index, double value);
    Statement& Bind(int index, std::wstring_view value);
    Statement& BindNull(int index);
    Statement& BindBlob(int index, const void* data, size_t size);

    // Advances to the next row. Returns false when done or on error.
    bool Step();
    // Executes a statement that returns no rows. Returns false on error.
    bool Run();
    void Reset();

    int64_t ColumnInt64(int col) const;
    int ColumnInt(int col) const { return static_cast<int>(ColumnInt64(col)); }
    bool ColumnBool(int col) const { return ColumnInt64(col) != 0; }
    double ColumnDouble(int col) const;
    std::wstring ColumnText(int col) const;
    bool ColumnIsNull(int col) const;
    std::vector<uint8_t> ColumnBlob(int col) const;

   private:
    friend class Database;
    Statement(Database* db, sqlite3_stmt* stmt) : db_(db), stmt_(stmt) {}
    Database* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
    bool failed_ = false;
  };

  // RAII transaction; rolls back unless Commit() succeeds.
  class Transaction {
   public:
    explicit Transaction(Database& db);
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction();
    bool Commit();
    bool active() const { return active_; }

   private:
    Database& db_;
    bool active_ = false;
  };

  Database() = default;
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  ~Database();

  // Opens (creating if needed) the database at |path|. Pass an empty path
  // for an in-memory database (used by tests).
  bool Open(const std::filesystem::path& path);
  void Close();
  bool is_open() const { return db_ != nullptr; }

  bool Execute(std::string_view sql);
  Statement Prepare(std::string_view sql);
  int64_t LastInsertId() const;
  int Changes() const;

  int UserVersion();
  bool SetUserVersion(int version);

  const std::string& last_error() const { return last_error_; }

 private:
  void RecordError(std::string_view context);

  sqlite3* db_ = nullptr;
  std::string last_error_;
};

}  // namespace orbit
