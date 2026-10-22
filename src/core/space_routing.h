// Space routing (ZenSpaceRoutingManager): rules that open matching URLs in a
// chosen space. Platform independent, unit-tested.
//
// Zen also offers "RegEx" rules; Orbit is built without C++ exceptions and
// std::regex reports bad patterns by throwing, so only "contains" and
// "equal-to" are supported.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace orbit {

struct SpaceRoute {
  enum class Match { kContains, kEqualTo };
  Match match = Match::kContains;
  std::wstring reference;  // what the URL contains / equals (case-insensitive)
  int64_t space_id = 0;    // 0 = the most recent space
};

// isRouteMatching: "contains" is a substring test; "equal-to" compares the
// URLs without scheme, "www." and a trailing slash.
bool RouteMatches(const SpaceRoute& route, const std::wstring& url);

// routeUri: the first matching rule's space (0 = most recent space), or for
// an external link with no match, |external_default|. nullopt: no rule.
std::optional<int64_t> RouteUrl(const std::vector<SpaceRoute>& routes, const std::wstring& url,
                                bool external, int64_t external_default);

// Storage: one rule per line, "contains|equal-to <TAB> space id <TAB> reference".
std::wstring SerializeRoutes(const std::vector<SpaceRoute>& routes);
std::vector<SpaceRoute> ParseRoutes(const std::wstring& text);  // skips bad lines

}  // namespace orbit
