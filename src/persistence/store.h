#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "persistence/database.h"

namespace orbit {

struct ProfileRecord {
  int64_t id = 0;
  std::wstring uuid;
  std::wstring name;
  uint32_t color = 0;
  std::wstring icon;
  int position = 0;
};

struct WorkspaceRecord {
  int64_t id = 0;
  int64_t profile_id = 0;
  std::wstring name;
  uint32_t color = 0;
  std::wstring icon;
  int position = 0;
};

struct TabRecord {
  int64_t id = 0;
  int64_t workspace_id = 0;
  std::wstring url;
  std::wstring title;
  std::wstring favicon_url;
  bool pinned = false;
  bool is_protected = false;
  int64_t group_id = 0;  // 0 = ungrouped
  int position = 0;
  int64_t last_active = 0;
};

struct TabGroupRecord {
  int64_t id = 0;
  int64_t workspace_id = 0;
  std::wstring name;
  uint32_t color = 0;
  bool collapsed = false;
};

struct WindowRecord {
  int64_t id = 0;
  int64_t profile_id = 0;
  int64_t workspace_id = 0;
  int64_t active_tab_id = 0;
  int x = 0, y = 0, w = 0, h = 0;
  bool maximized = false;
};

struct ClosedTabRecord {
  int64_t id = 0;
  int64_t profile_id = 0;
  int64_t workspace_id = 0;
  std::wstring url;
  std::wstring title;
};

struct HistoryRecord {
  int64_t id = 0;
  int64_t profile_id = 0;
  std::wstring url;
  std::wstring title;
  int64_t visit_time = 0;  // unix ms
};

struct BookmarkRecord {
  int64_t id = 0;
  int64_t profile_id = 0;
  int64_t parent_id = 0;  // 0 = root
  bool is_folder = false;
  std::wstring title;
  std::wstring url;
  int position = 0;
  int64_t created_at = 0;
};

enum class DownloadState { kInProgress = 0, kComplete = 1, kCancelled = 2, kInterrupted = 3,
                           kPaused = 4 };

struct DownloadRecord {
  int64_t id = 0;
  int64_t profile_id = 0;
  std::wstring url;
  std::wstring path;
  std::wstring mime_type;
  int64_t total_bytes = -1;
  int64_t received_bytes = 0;
  DownloadState state = DownloadState::kInProgress;
  int64_t started_at = 0;
  int64_t ended_at = 0;
};

// Typed access to browser metadata. All methods run on the UI thread.
class Store {
 public:
  static constexpr int64_t kGlobalScope = 0;
  static constexpr int kMaxClosedTabsPerProfile = 25;

  explicit Store(Database& db) : db_(db) {}

  Database& db() { return db_; }

  // Profiles.
  std::vector<ProfileRecord> LoadProfiles();
  std::optional<ProfileRecord> CreateProfile(const std::wstring& uuid, const std::wstring& name,
                                             uint32_t color, const std::wstring& icon);
  bool UpdateProfile(const ProfileRecord& profile);
  bool DeleteProfile(int64_t id);

  // Workspaces.
  std::vector<WorkspaceRecord> LoadWorkspaces(int64_t profile_id);
  std::optional<WorkspaceRecord> CreateWorkspace(int64_t profile_id, const std::wstring& name,
                                                 uint32_t color, const std::wstring& icon);
  bool UpdateWorkspace(const WorkspaceRecord& workspace);
  bool DeleteWorkspace(int64_t id);
  bool SetWorkspaceOrder(const std::vector<int64_t>& ids);

  // Tabs.
  std::vector<TabRecord> LoadTabs(int64_t workspace_id);
  int64_t InsertTab(const TabRecord& tab);
  bool UpdateTab(const TabRecord& tab);
  bool DeleteTab(int64_t id);
  // Rewrites positions so that |ids| appear in order (all within one transaction).
  bool SetTabOrder(const std::vector<int64_t>& ids);

  // Tab groups (membership lives in tabs.group_id).
  std::vector<TabGroupRecord> LoadTabGroups(int64_t workspace_id);
  int64_t InsertTabGroup(const TabGroupRecord& group);
  bool UpdateTabGroup(const TabGroupRecord& group);
  bool DeleteTabGroup(int64_t id);

  // Recently closed tabs (bounded per profile).
  bool PushClosedTab(const ClosedTabRecord& tab);
  std::optional<ClosedTabRecord> PopClosedTab(int64_t profile_id);

  // Window session state; SaveWindows replaces the stored set atomically.
  std::vector<WindowRecord> LoadWindows();
  bool SaveWindows(const std::vector<WindowRecord>& windows);

  // History (never written for private profiles).
  int64_t AddVisit(int64_t profile_id, const std::wstring& url, const std::wstring& title,
                   int64_t visit_time);
  bool UpdateLatestVisitTitle(int64_t profile_id, const std::wstring& url,
                              const std::wstring& title);
  // Newest first. |query| matches URL or title (case-insensitive substring);
  // |before| (unix ms, 0 = now) pages through results.
  std::vector<HistoryRecord> QueryHistory(int64_t profile_id, const std::wstring& query,
                                          int64_t before, int limit);
  bool DeleteVisit(int64_t profile_id, int64_t id);
  // Deletes visits with from <= visit_time < to (to == 0 means "until now").
  int DeleteHistoryRange(int64_t profile_id, int64_t from, int64_t to);
  struct TopSite {
    std::wstring url;
    std::wstring title;
    int visits = 0;
  };
  std::vector<TopSite> TopSites(int64_t profile_id, int limit);

  // Bookmarks.
  std::vector<BookmarkRecord> LoadBookmarks(int64_t profile_id);
  int64_t AddBookmark(const BookmarkRecord& bookmark);  // appended within its parent
  bool UpdateBookmark(int64_t profile_id, int64_t id, const std::wstring& title,
                      const std::wstring& url);
  // Moves |id| under |parent_id| (0 = root); refuses cycles.
  bool MoveBookmark(int64_t profile_id, int64_t id, int64_t parent_id);
  bool DeleteBookmark(int64_t profile_id, int64_t id);  // folders delete recursively
  std::optional<BookmarkRecord> FindBookmarkByUrl(int64_t profile_id, const std::wstring& url);

  // Downloads.
  int64_t InsertDownload(const DownloadRecord& download);
  bool UpdateDownload(const DownloadRecord& download);
  std::vector<DownloadRecord> LoadDownloads(int64_t profile_id, int limit);
  bool DeleteDownload(int64_t profile_id, int64_t id);
  // Called at startup: anything still "in progress" was cut off by a restart.
  int MarkInterruptedDownloads();

  // Preferences. scope = kGlobalScope or a profile id.
  std::optional<std::wstring> GetPref(int64_t scope, const std::wstring& key);
  bool SetPref(int64_t scope, const std::wstring& key, const std::wstring& value);

 private:
  Database& db_;
};

int64_t NowUnixMillis();

}  // namespace orbit
