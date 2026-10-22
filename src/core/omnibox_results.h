#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace orbit {

// Address-bar results for Zen's (Firefox's) urlbar view: a heuristic first
// row for what was typed, then open tabs ("Switch to Tab"), bookmarks and
// history that contain every typed word. A URL appears at most once; an open
// tab wins over a bookmark, which wins over history. The current tab is
// never offered as a switch target. Platform independent (unit tested).
struct OmniboxCandidate {
  std::wstring url;
  std::wstring title;
  intptr_t tab_key = 0;  // open tabs: identifies the tab
};

struct OmniboxRow {
  enum class Kind { kVisit, kSearch, kSwitchTab, kBookmark, kHistory, kAction };
  Kind kind = Kind::kVisit;
  std::wstring url;     // where Enter goes (search: the search URL)
  std::wstring title;   // main text
  std::wstring action;  // e.g. "Switch to Tab", "Search with DuckDuckGo", "Visit"
  intptr_t tab_key = 0;
  std::wstring action_id;  // kAction: what to run
};

// A quick action (ZenUBGlobalActions): matched by |match| (the label, or a
// space's name for "Focus on <space>"), shown as |label|.
struct OmniboxAction {
  std::wstring id;
  std::wstring label;
  std::wstring match;
};

struct OmniboxSources {
  std::vector<OmniboxCandidate> open_tabs;   // most recently used first
  std::vector<OmniboxCandidate> bookmarks;
  std::vector<OmniboxCandidate> history;     // best first (frecency)
  // Search suggestions from the engine for this input (browser.search.
  // suggest.enabled; off by default in Zen).
  std::vector<std::wstring> search_suggestions;
  intptr_t current_tab_key = 0;
  std::vector<OmniboxAction> actions;  // the ones available right now
};

// ZenUBActionsProvider.#calculateFuzzyScore (VS Code style): 1'000'000 for an
// exact match, 100'000 + length for a prefix, else per-letter bonuses for
// word starts and runs; 0 when not every query letter appears in order.
int QuickActionScore(std::wstring_view target, std::wstring_view query);
inline constexpr int kQuickActionMinScore = 92;  // MINIMUM_QUERY_SCORE

inline constexpr size_t kMaxOmniboxRows = 10;     // browser.urlbar.maxRichResults
inline constexpr size_t kVisibleOmniboxRows = 5;  // #urlbar-results' 244px; the rest scroll
inline constexpr size_t kMaxSearchSuggestions = 4;

std::vector<OmniboxRow> BuildOmniboxRows(std::wstring_view input,
                                         std::wstring_view search_template,
                                         const OmniboxSources& sources,
                                         size_t max_rows = kMaxOmniboxRows);

// "DuckDuckGo" for https://duckduckgo.com/?q={query}; the host otherwise.
std::wstring SearchEngineName(std::wstring_view search_template);

// The engine's suggestion endpoint (OpenSearch JSON: ["query", ["a", ...]])
// for |query|, or empty for an engine without a known one.
std::wstring SearchSuggestUrl(std::wstring_view search_template, std::wstring_view query);

// The text the input shows while |row| is selected (Firefox fills the input
// with the selected result): the URL for pages and tabs, the suggestion for
// a search suggestion, what was typed for the first row and actions.
std::wstring OmniboxRowInputText(const OmniboxRow& row, size_t index, std::wstring_view typed);

}  // namespace orbit
