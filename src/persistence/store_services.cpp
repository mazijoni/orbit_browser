// Store: history, bookmarks and downloads.

#include "persistence/store.h"

namespace orbit {

// ---------------------------------------------------------------------------
// History

int64_t Store::AddVisit(int64_t profile_id, const std::wstring& url, const std::wstring& title,
                        int64_t visit_time) {
  auto s = db_.Prepare(
      "INSERT INTO history (profile_id, url, title, visit_time) VALUES (?, ?, ?, ?)");
  s.Bind(1, profile_id).Bind(2, url).Bind(3, title).Bind(4, visit_time);
  return s.Run() ? db_.LastInsertId() : 0;
}

bool Store::UpdateLatestVisitTitle(int64_t profile_id, const std::wstring& url,
                                   const std::wstring& title) {
  auto s = db_.Prepare(
      "UPDATE history SET title = ?1 WHERE id = (SELECT id FROM history WHERE profile_id = ?2 "
      "AND url = ?3 ORDER BY visit_time DESC LIMIT 1)");
  s.Bind(1, title).Bind(2, profile_id).Bind(3, url);
  return s.Run();
}

std::vector<HistoryRecord> Store::QueryHistory(int64_t profile_id, const std::wstring& query,
                                               int64_t before, int limit) {
  std::vector<HistoryRecord> out;
  // instr() instead of LIKE: no wildcard escaping needed for user input.
  auto s = db_.Prepare(
      "SELECT id, profile_id, url, title, visit_time FROM history WHERE profile_id = ?1 "
      "AND visit_time < ?2 AND (?3 = '' OR instr(lower(url), lower(?3)) > 0 OR "
      "instr(lower(title), lower(?3)) > 0) ORDER BY visit_time DESC, id DESC LIMIT ?4");
  s.Bind(1, profile_id).Bind(2, before > 0 ? before : INT64_MAX).Bind(3, query).Bind(4, limit);
  while (s.Step()) {
    HistoryRecord h;
    h.id = s.ColumnInt64(0);
    h.profile_id = s.ColumnInt64(1);
    h.url = s.ColumnText(2);
    h.title = s.ColumnText(3);
    h.visit_time = s.ColumnInt64(4);
    out.push_back(std::move(h));
  }
  return out;
}

std::vector<HistoryRecord> Store::QueryHistoryFrecent(int64_t profile_id, const std::wstring& query,
                                                      int64_t now_ms, int limit) {
  std::vector<HistoryRecord> out;
  // SQLite takes the bare columns (title, id) from the row max() picks.
  auto s = db_.Prepare(
      "SELECT id, url, title, MAX(visit_time), "
      "SUM(CASE WHEN ?2 - visit_time < 345600000 THEN 100 WHEN ?2 - visit_time < 1209600000 THEN 70 "
      "WHEN ?2 - visit_time < 2678400000 THEN 50 WHEN ?2 - visit_time < 7776000000 THEN 30 ELSE 10 END) "
      "AS frecency FROM history WHERE profile_id = ?1 AND (?3 = '' OR instr(lower(url), lower(?3)) > 0 OR "
      "instr(lower(title), lower(?3)) > 0) GROUP BY url ORDER BY frecency DESC, MAX(visit_time) DESC "
      "LIMIT ?4");
  s.Bind(1, profile_id).Bind(2, now_ms).Bind(3, query).Bind(4, limit);
  while (s.Step()) {
    HistoryRecord h;
    h.id = s.ColumnInt64(0);
    h.profile_id = profile_id;
    h.url = s.ColumnText(1);
    h.title = s.ColumnText(2);
    h.visit_time = s.ColumnInt64(3);
    out.push_back(std::move(h));
  }
  return out;
}

bool Store::DeleteVisit(int64_t profile_id, int64_t id) {
  auto s = db_.Prepare("DELETE FROM history WHERE profile_id = ? AND id = ?");
  s.Bind(1, profile_id).Bind(2, id);
  return s.Run();
}

int Store::DeleteHistoryRange(int64_t profile_id, int64_t from, int64_t to) {
  auto s = db_.Prepare(
      "DELETE FROM history WHERE profile_id = ? AND visit_time >= ? AND visit_time < ?");
  s.Bind(1, profile_id).Bind(2, from).Bind(3, to > 0 ? to : INT64_MAX);
  return s.Run() ? db_.Changes() : -1;
}

std::vector<Store::TopSite> Store::TopSites(int64_t profile_id, int limit) {
  std::vector<TopSite> out;
  // Recent popularity: visits in the last 30 days, newest title wins.
  auto s = db_.Prepare(
      "SELECT url, (SELECT title FROM history h2 WHERE h2.profile_id = ?1 AND h2.url = h.url "
      "ORDER BY visit_time DESC LIMIT 1), COUNT(*) AS n FROM history h "
      "WHERE profile_id = ?1 AND visit_time > ?2 GROUP BY url ORDER BY n DESC, MAX(visit_time) "
      "DESC LIMIT ?3");
  s.Bind(1, profile_id).Bind(2, NowUnixMillis() - 30LL * 24 * 3600 * 1000).Bind(3, limit);
  while (s.Step()) out.push_back({s.ColumnText(0), s.ColumnText(1), s.ColumnInt(2)});
  return out;
}

// ---------------------------------------------------------------------------
// Bookmarks

std::vector<BookmarkRecord> Store::LoadBookmarks(int64_t profile_id) {
  std::vector<BookmarkRecord> out;
  auto s = db_.Prepare(
      "SELECT id, profile_id, COALESCE(parent_id, 0), is_folder, title, url, position, "
      "created_at FROM bookmarks WHERE profile_id = ? ORDER BY parent_id, position, id");
  s.Bind(1, profile_id);
  while (s.Step()) {
    BookmarkRecord b;
    b.id = s.ColumnInt64(0);
    b.profile_id = s.ColumnInt64(1);
    b.parent_id = s.ColumnInt64(2);
    b.is_folder = s.ColumnBool(3);
    b.title = s.ColumnText(4);
    b.url = s.ColumnText(5);
    b.position = s.ColumnInt(6);
    b.created_at = s.ColumnInt64(7);
    out.push_back(std::move(b));
  }
  return out;
}

int64_t Store::AddBookmark(const BookmarkRecord& b) {
  if (b.parent_id) {
    // The parent must be a folder of the same profile.
    auto check = db_.Prepare("SELECT 1 FROM bookmarks WHERE id = ? AND profile_id = ? AND "
                             "is_folder = 1");
    check.Bind(1, b.parent_id).Bind(2, b.profile_id);
    if (!check.Step()) return 0;
  }
  auto s = db_.Prepare(
      "INSERT INTO bookmarks (profile_id, parent_id, is_folder, title, url, position, "
      "created_at) VALUES (?1, ?2, ?3, ?4, ?5, (SELECT COALESCE(MAX(position) + 1, 0) FROM "
      "bookmarks WHERE profile_id = ?1 AND parent_id IS ?2), ?6)");
  s.Bind(1, b.profile_id);
  if (b.parent_id) {
    s.Bind(2, b.parent_id);
  } else {
    s.BindNull(2);
  }
  s.Bind(3, b.is_folder).Bind(4, b.title).Bind(5, b.url);
  s.Bind(6, b.created_at ? b.created_at : NowUnixMillis());
  return s.Run() ? db_.LastInsertId() : 0;
}

bool Store::UpdateBookmark(int64_t profile_id, int64_t id, const std::wstring& title,
                           const std::wstring& url) {
  auto s = db_.Prepare(
      "UPDATE bookmarks SET title = ?, url = CASE WHEN is_folder THEN '' ELSE ? END "
      "WHERE profile_id = ? AND id = ?");
  s.Bind(1, title).Bind(2, url).Bind(3, profile_id).Bind(4, id);
  return s.Run() && db_.Changes() == 1;
}

bool Store::MoveBookmark(int64_t profile_id, int64_t id, int64_t parent_id) {
  if (parent_id) {
    // Refuse moving a folder into itself or one of its descendants.
    auto cycle = db_.Prepare(
        "WITH RECURSIVE up(id, parent_id) AS (SELECT id, parent_id FROM bookmarks WHERE id = ?1 "
        "UNION ALL SELECT b.id, b.parent_id FROM bookmarks b JOIN up ON b.id = up.parent_id) "
        "SELECT 1 FROM up WHERE id = ?2");
    cycle.Bind(1, parent_id).Bind(2, id);
    if (cycle.Step()) return false;
    auto check = db_.Prepare(
        "SELECT 1 FROM bookmarks WHERE id = ? AND profile_id = ? AND is_folder = 1");
    check.Bind(1, parent_id).Bind(2, profile_id);
    if (!check.Step()) return false;
  }
  auto s = db_.Prepare(
      "UPDATE bookmarks SET parent_id = ?1, position = (SELECT COALESCE(MAX(position) + 1, 0) "
      "FROM bookmarks WHERE profile_id = ?2 AND parent_id IS ?1) WHERE profile_id = ?2 AND "
      "id = ?3");
  if (parent_id) {
    s.Bind(1, parent_id);
  } else {
    s.BindNull(1);
  }
  s.Bind(2, profile_id).Bind(3, id);
  return s.Run() && db_.Changes() == 1;
}

bool Store::DeleteBookmark(int64_t profile_id, int64_t id) {
  // Children go with their folder via ON DELETE CASCADE on parent_id.
  auto s = db_.Prepare("DELETE FROM bookmarks WHERE profile_id = ? AND id = ?");
  s.Bind(1, profile_id).Bind(2, id);
  return s.Run() && db_.Changes() >= 1;
}

std::optional<BookmarkRecord> Store::FindBookmarkByUrl(int64_t profile_id,
                                                       const std::wstring& url) {
  for (auto& b : LoadBookmarks(profile_id)) {
    if (!b.is_folder && b.url == url) return b;
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Downloads

int64_t Store::InsertDownload(const DownloadRecord& d) {
  auto s = db_.Prepare(
      "INSERT INTO downloads (profile_id, url, path, mime_type, total_bytes, received_bytes, "
      "state, started_at, ended_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
  s.Bind(1, d.profile_id).Bind(2, d.url).Bind(3, d.path).Bind(4, d.mime_type);
  s.Bind(5, d.total_bytes).Bind(6, d.received_bytes).Bind(7, static_cast<int>(d.state));
  s.Bind(8, d.started_at).Bind(9, d.ended_at);
  return s.Run() ? db_.LastInsertId() : 0;
}

bool Store::UpdateDownload(const DownloadRecord& d) {
  auto s = db_.Prepare(
      "UPDATE downloads SET path = ?, mime_type = ?, total_bytes = ?, received_bytes = ?, "
      "state = ?, ended_at = ? WHERE id = ? AND profile_id = ?");
  s.Bind(1, d.path).Bind(2, d.mime_type).Bind(3, d.total_bytes).Bind(4, d.received_bytes);
  s.Bind(5, static_cast<int>(d.state)).Bind(6, d.ended_at).Bind(7, d.id).Bind(8, d.profile_id);
  return s.Run();
}

std::vector<DownloadRecord> Store::LoadDownloads(int64_t profile_id, int limit) {
  std::vector<DownloadRecord> out;
  auto s = db_.Prepare(
      "SELECT id, profile_id, url, path, mime_type, total_bytes, received_bytes, state, "
      "started_at, ended_at FROM downloads WHERE profile_id = ? ORDER BY started_at DESC, id "
      "DESC LIMIT ?");
  s.Bind(1, profile_id).Bind(2, limit);
  while (s.Step()) {
    DownloadRecord d;
    d.id = s.ColumnInt64(0);
    d.profile_id = s.ColumnInt64(1);
    d.url = s.ColumnText(2);
    d.path = s.ColumnText(3);
    d.mime_type = s.ColumnText(4);
    d.total_bytes = s.ColumnInt64(5);
    d.received_bytes = s.ColumnInt64(6);
    d.state = static_cast<DownloadState>(s.ColumnInt(7));
    d.started_at = s.ColumnInt64(8);
    d.ended_at = s.ColumnInt64(9);
    out.push_back(std::move(d));
  }
  return out;
}

bool Store::DeleteDownload(int64_t profile_id, int64_t id) {
  auto s = db_.Prepare("DELETE FROM downloads WHERE profile_id = ? AND id = ?");
  s.Bind(1, profile_id).Bind(2, id);
  return s.Run();
}

int Store::MarkInterruptedDownloads() {
  auto s = db_.Prepare("UPDATE downloads SET state = ?, ended_at = ? WHERE state IN (?, ?)");
  s.Bind(1, static_cast<int>(DownloadState::kInterrupted)).Bind(2, NowUnixMillis());
  s.Bind(3, static_cast<int>(DownloadState::kInProgress));
  s.Bind(4, static_cast<int>(DownloadState::kPaused));
  return s.Run() ? db_.Changes() : -1;
}

}  // namespace orbit
