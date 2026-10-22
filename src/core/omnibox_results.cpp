#include "core/omnibox_results.h"

#include "core/strings.h"

#include <algorithm>
#include <cwctype>

#include "core/url_fixup.h"

namespace orbit {
namespace {

std::wstring Lower(std::wstring_view s) {
  std::wstring out(s);
  for (wchar_t& c : out) c = static_cast<wchar_t>(std::towlower(c));
  return out;
}

std::vector<std::wstring> Words(std::wstring_view input) {
  std::vector<std::wstring> words;
  std::wstring current;
  for (wchar_t c : input) {
    if (std::iswspace(c)) {
      if (!current.empty()) words.push_back(Lower(current));
      current.clear();
    } else {
      current += c;
    }
  }
  if (!current.empty()) words.push_back(Lower(current));
  return words;
}

bool MatchesAll(const OmniboxCandidate& c, const std::vector<std::wstring>& words) {
  const std::wstring haystack = Lower(c.title) + L"\n" + Lower(c.url);
  return std::all_of(words.begin(), words.end(), [&](const std::wstring& w) {
    return haystack.find(w) != std::wstring::npos;
  });
}

std::wstring Host(std::wstring_view url) {
  const size_t start = url.find(L"://");
  if (start == std::wstring_view::npos) return std::wstring(url);
  const size_t host_start = start + 3;
  const size_t end = url.find_first_of(L"/?#:", host_start);
  return std::wstring(url.substr(host_start, end == std::wstring_view::npos ? std::wstring_view::npos
                                                                            : end - host_start));
}

// UrlUtils.REGEXP_LIKE_PROTOCOL: "scheme:" at the start.
bool LooksLikeProtocol(std::wstring_view s) {
  size_t i = 0;
  while (i < s.size() && (std::iswalpha(s[i]) || s[i] == L'+' || s[i] == L'.' || s[i] == L'-')) ++i;
  return i > 0 && i < s.size() && s[i] == L':';
}

}  // namespace

int QuickActionScore(std::wstring_view target, std::wstring_view query) {
  if (target.empty() || query.empty() || query.size() > target.size()) return 0;
  const std::wstring t = Lower(target), q = Lower(query);
  if (t == q) return 1'000'000;
  if (t.rfind(q, 0) == 0) return 100'000 + static_cast<int>(q.size());
  int score = 0;
  size_t qi = 0;
  int last = -1, consecutive = 0;
  for (size_t ti = 0; ti < t.size(); ++ti) {
    if (qi < q.size() && t[ti] == q[qi]) {
      int bonus = 10;
      if (ti == 0 || t[ti - 1] == L' ' || t[ti - 1] == L'-' || t[ti - 1] == L'_') bonus += 15;
      if (last == static_cast<int>(ti) - 1) {
        ++consecutive;
        bonus += 20 * consecutive;
      } else {
        consecutive = 0;
      }
      if (last != -1) bonus -= std::min(static_cast<int>(ti) - last - 1, 10);
      score += bonus;
      last = static_cast<int>(ti);
      ++qi;
    }
  }
  return qi == q.size() ? score : 0;
}

std::wstring SearchSuggestUrl(std::wstring_view search_template, std::wstring_view query) {
  const std::wstring host = Lower(Host(search_template));
  const std::wstring q = EscapeQueryComponent(query);
  if (host.find(L"google.") != std::wstring::npos) return L"https://www.google.com/complete/search?client=firefox&q=" + q;
  if (host.find(L"duckduckgo.") != std::wstring::npos) return L"https://duckduckgo.com/ac/?type=list&q=" + q;
  if (host.find(L"bing.") != std::wstring::npos) return L"https://www.bing.com/osjson.aspx?query=" + q;
  if (host.find(L"search.brave.") != std::wstring::npos) return L"https://search.brave.com/api/suggest?q=" + q;
  if (host.find(L"ecosia.") != std::wstring::npos) return L"https://ac.ecosia.org/autocomplete?type=list&q=" + q;
  return {};
}

std::wstring OmniboxRowInputText(const OmniboxRow& row, size_t index, std::wstring_view typed) {
  if (index == 0 || row.kind == OmniboxRow::Kind::kAction) return std::wstring(typed);
  if (row.kind == OmniboxRow::Kind::kSearch || row.kind == OmniboxRow::Kind::kVisit) return row.title;
  return row.url;
}

std::wstring SearchEngineName(std::wstring_view search_template) {
  struct Known {
    const wchar_t* host_part;
    const wchar_t* name;
  };
  static constexpr Known kKnown[] = {
      {L"duckduckgo.", L"DuckDuckGo"}, {L"google.", L"Google"},   {L"bing.", L"Bing"},
      {L"search.brave.", L"Brave Search"}, {L"startpage.", L"Startpage"},
      {L"ecosia.", L"Ecosia"},
  };
  const std::wstring host = Lower(Host(search_template));
  for (const Known& k : kKnown) {
    if (host.find(k.host_part) != std::wstring::npos) return k.name;
  }
  return host.rfind(L"www.", 0) == 0 ? host.substr(4) : host;
}

std::vector<OmniboxRow> BuildOmniboxRows(std::wstring_view input,
                                         std::wstring_view search_template,
                                         const OmniboxSources& sources, size_t max_rows) {
  std::vector<OmniboxRow> rows;
  const std::vector<std::wstring> words = Words(input);
  if (words.empty() || max_rows == 0) return rows;

  // Heuristic row (UrlbarProviderHeuristicFallback): visit or search.
  const OmniboxResult fixed = FixupOmniboxInput(input, search_template);
  if (fixed.kind == OmniboxResult::Kind::kUrl) {
    rows.push_back({OmniboxRow::Kind::kVisit, fixed.url, std::wstring(input), L"Visit", 0});
  } else if (fixed.kind == OmniboxResult::Kind::kSearch) {
    rows.push_back({OmniboxRow::Kind::kSearch, fixed.url, std::wstring(input),
                    L"Search with " + SearchEngineName(search_template), 0});
  }

  // Search suggestions follow the heuristic row
  // (browser.urlbar.showSearchSuggestionsFirst).
  if (fixed.kind == OmniboxResult::Kind::kSearch) {
    size_t added = 0;
    for (const std::wstring& s : sources.search_suggestions) {
      if (rows.size() >= max_rows || added >= kMaxSearchSuggestions) break;
      if (s.empty() || Lower(s) == Lower(std::wstring(input))) continue;
      std::wstring url(search_template);
      const size_t at = url.find(L"{query}");
      if (at == std::wstring::npos) break;
      url.replace(at, 7, EscapeQueryComponent(s));
      rows.push_back({OmniboxRow::Kind::kSearch, url, s,
                      L"Search with " + SearchEngineName(search_template), 0});
      ++added;
    }
  }

  // Quick actions (zen.urlbar.suggestions.quick-actions): more than two
  // characters that do not start like a URL; best matches first, up to 3.
  if (input.size() > 2 && !LooksLikeProtocol(input) && !sources.actions.empty()) {
    std::wstring query(input);
    while (!query.empty() && std::iswspace(query.front())) query.erase(query.begin());
    while (!query.empty() && std::iswspace(query.back())) query.pop_back();
    std::vector<std::pair<int, const OmniboxAction*>> scored;
    for (const OmniboxAction& a : sources.actions) {
      const int score = QuickActionScore(a.match.empty() ? a.label : a.match, query);
      if (score > kQuickActionMinScore) scored.emplace_back(score, &a);
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& x, const auto& y) { return x.first > y.first; });
    for (size_t i = 0; i < scored.size() && i < 3 && rows.size() < max_rows; ++i) {
      OmniboxRow row{OmniboxRow::Kind::kAction, L"", scored[i].second->label, L"Action", 0};
      row.action_id = scored[i].second->id;
      rows.push_back(std::move(row));
    }
  }
  std::vector<std::wstring> seen;
  if (fixed.kind == OmniboxResult::Kind::kUrl) seen.push_back(fixed.url);
  auto already = [&](const std::wstring& url) {
    return std::any_of(seen.begin(), seen.end(),
                       [&](const std::wstring& s) { return PinnedUrlMatches(url, s); });
  };
  auto add = [&](const std::vector<OmniboxCandidate>& list, OmniboxRow::Kind kind, size_t cap,
                 const wchar_t* action) {
    size_t added = 0;
    for (const OmniboxCandidate& c : list) {
      if (rows.size() >= max_rows || added >= cap) return;
      if (kind == OmniboxRow::Kind::kSwitchTab && c.tab_key == sources.current_tab_key) continue;
      if (c.url.empty() || already(c.url) || !MatchesAll(c, words)) continue;
      seen.push_back(c.url);
      rows.push_back({kind, c.url, c.title.empty() ? c.url : c.title, action, c.tab_key});
      ++added;
    }
  };
  // The current tab's URL is never suggested again as a history or bookmark row.
  for (const OmniboxCandidate& t : sources.open_tabs) {
    if (t.tab_key == sources.current_tab_key) seen.push_back(t.url);
  }
  add(sources.open_tabs, OmniboxRow::Kind::kSwitchTab, 3, L"Switch to Tab");
  add(sources.bookmarks, OmniboxRow::Kind::kBookmark, 3, L"");
  add(sources.history, OmniboxRow::Kind::kHistory, max_rows, L"");
  return rows;
}

}  // namespace orbit
