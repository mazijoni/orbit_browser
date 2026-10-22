#include "core/space_routing.h"

#include <cwctype>

namespace orbit {
namespace {

std::wstring Lower(std::wstring s) {
  for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
  return s;
}

std::wstring Trim(std::wstring s) {
  while (!s.empty() && std::iswspace(s.front())) s.erase(s.begin());
  while (!s.empty() && std::iswspace(s.back())) s.pop_back();
  return s;
}

// #normalizeURL: no scheme, no "www.", no trailing slash.
std::wstring Normalize(std::wstring url) {
  url = Lower(Trim(std::move(url)));
  for (const wchar_t* scheme : {L"https://", L"http://"}) {
    if (url.rfind(scheme, 0) == 0) {
      url.erase(0, wcslen(scheme));
      break;
    }
  }
  if (url.rfind(L"www.", 0) == 0) url.erase(0, 4);
  if (!url.empty() && url.back() == L'/') url.pop_back();
  return url;
}

}  // namespace

bool RouteMatches(const SpaceRoute& route, const std::wstring& url) {
  const std::wstring reference = Lower(route.reference);
  if (Trim(reference).empty()) return false;
  if (route.match == SpaceRoute::Match::kContains) return Lower(url).find(reference) != std::wstring::npos;
  return Normalize(url) == Normalize(route.reference);
}

std::optional<int64_t> RouteUrl(const std::vector<SpaceRoute>& routes, const std::wstring& url,
                                bool external, int64_t external_default) {
  for (const SpaceRoute& r : routes) {
    if (RouteMatches(r, url)) return r.space_id;
  }
  if (external && external_default) return external_default;
  return std::nullopt;
}

std::wstring SerializeRoutes(const std::vector<SpaceRoute>& routes) {
  std::wstring out;
  for (const SpaceRoute& r : routes) {
    std::wstring reference = r.reference;
    for (wchar_t& c : reference) {
      if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
    }
    out += r.match == SpaceRoute::Match::kContains ? L"contains" : L"equal-to";
    out += L'\t' + std::to_wstring(r.space_id) + L'\t' + reference + L'\n';
  }
  return out;
}

std::vector<SpaceRoute> ParseRoutes(const std::wstring& text) {
  std::vector<SpaceRoute> out;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find(L'\n', start);
    if (end == std::wstring::npos) end = text.size();
    const std::wstring line = text.substr(start, end - start);
    start = end + 1;
    const size_t a = line.find(L'\t');
    const size_t b = a == std::wstring::npos ? a : line.find(L'\t', a + 1);
    if (b == std::wstring::npos) continue;
    SpaceRoute r;
    const std::wstring type = line.substr(0, a);
    if (type == L"contains") {
      r.match = SpaceRoute::Match::kContains;
    } else if (type == L"equal-to") {
      r.match = SpaceRoute::Match::kEqualTo;
    } else {
      continue;
    }
    const std::wstring id = line.substr(a + 1, b - a - 1);
    if (id.empty() || id.find_first_not_of(L"0123456789") != std::wstring::npos) continue;
    r.space_id = _wtoi64(id.c_str());
    r.reference = line.substr(b + 1);
    if (Trim(r.reference).empty()) continue;
    out.push_back(std::move(r));
  }
  return out;
}

}  // namespace orbit
