// Live folders (src/zen/live-folders): folders that fill themselves from an
// RSS/Atom feed or from the user's GitHub pull requests / issues. This part
// is platform independent and unit-tested: feed parsing, dates, the stored
// state, GitHub queries and page scraping, and the per-fetch tab diff.
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace orbit {

struct LiveItem {
  std::wstring id;        // stable within the folder (guid / id / repo#number)
  std::wstring title;
  std::wstring url;
  std::wstring subtitle;  // GitHub: the author
  int64_t date_ms = 0;    // 0: missing or unparseable
  bool has_date = false;  // the feed gave a date (even an unparseable one)
};

struct Feed {
  std::wstring title;
  std::wstring link;  // the site the feed belongs to
  std::vector<LiveItem> items;
};

// RssLiveFolder.fetchItems/getMetadata: RSS 2.0, RDF and Atom. nullopt when
// the document is not a feed (an error page must not empty the folder).
std::optional<Feed> ParseFeed(std::string_view utf8);

// RSS: RFC 822 ("Tue, 21 Oct 2026 10:00:00 GMT", numeric or named zones);
// Atom: RFC 3339. Milliseconds since the Unix epoch, 0 if not understood.
int64_t ParseFeedDate(std::wstring_view s);

// The RSS item filter: http(s) links with a date, inside the time range
// (0: all time), at most |max_items|.
std::vector<LiveItem> FilterFeedItems(const std::vector<LiveItem>& items, int64_t now_ms,
                                      int64_t time_range_ms, int max_items);

// A live folder's settings and bookkeeping (nsZenLiveFolderProvider.state).
struct LiveFolderState {
  enum class Kind { kRss, kGithubPulls, kGithubIssues };
  Kind kind = Kind::kRss;
  std::wstring url;  // the feed; GitHub: https://github.com/pulls or /issues/assigned
  int64_t interval_ms = 30 * 60 * 1000;  // DEFAULT_FETCH_INTERVAL
  int max_items = 10;
  int64_t time_range_ms = 0;
  bool author_me = false;
  bool assigned_me = true;
  bool review_requested = false;
  bool include_drafts = true;
  int json_api = -1;  // GitHub pulls: -1 not known yet, 0 HTML, 1 JSON
  std::set<std::wstring> repos;          // seen in the last fetch
  std::set<std::wstring> repo_excludes;  // unchecked in "Repositories"
  int64_t last_fetched = 0;
  std::wstring last_error;  // a zen-live-folder-* id, empty when fine
};

std::wstring SerializeLiveState(const LiveFolderState& state);
std::optional<LiveFolderState> ParseLiveState(const std::wstring& text);
const wchar_t* LiveKindName(LiveFolderState::Kind kind);  // "rss", "github:pull-requests", ...
std::optional<LiveFolderState::Kind> LiveKindFromName(std::wstring_view name);
std::wstring DefaultGithubUrl(LiveFolderState::Kind kind);

// Error ids, as Zen's l10n ids, and their folder labels / tooltips.
inline constexpr wchar_t kLiveFailedFetch[] = L"zen-live-folder-failed-fetch";
inline constexpr wchar_t kLiveGithubNoAuth[] = L"zen-live-folder-github-no-auth";
inline constexpr wchar_t kLiveGithubNoFilter[] = L"zen-live-folder-github-no-filter";
std::wstring LiveErrorLabel(const std::wstring& error_id);
std::wstring LiveErrorTooltip(const std::wstring& error_id);

// #buildSearchOptions: the GitHub search queries one fetch runs ("q=").
// Empty when no filter is on (zen-live-folder-github-no-filter).
std::vector<std::wstring> GithubQueries(const LiveFolderState& state);

struct GithubPage {
  std::vector<LiveItem> items;
  std::set<std::wstring> repos;
};
// parseIssues: a[data-testid="issue-listitem-title-link"] rows.
GithubPage ParseGithubIssuesHtml(std::string_view utf8, const std::wstring& base_url);
// parsePullRequests (classic HTML): a[id^=issue_] title links.
GithubPage ParseGithubPullsHtml(std::string_view utf8, const std::wstring& base_url);

// onLiveFolderFetch: which existing item tabs go (no longer in the feed) and
// which items are new (not present, not dismissed). Composite ids are
// "<folder>:<item>". Dismissals of items gone from a non-empty result are
// forgotten.
struct LiveDiff {
  std::vector<std::wstring> remove;  // composite ids
  std::vector<LiveItem> add;         // in feed order
};
std::wstring LiveCompositeId(int64_t folder_id, const std::wstring& item_id);
LiveDiff DiffLiveItems(int64_t folder_id, const std::vector<std::wstring>& existing,
                       const std::vector<LiveItem>& items, std::set<std::wstring>& dismissed);

// ZenLiveFoldersUI.#timeAgo (Intl.RelativeTimeFormat, numeric: "auto").
std::wstring FormatTimeAgo(int64_t then_ms, int64_t now_ms);

}  // namespace orbit
