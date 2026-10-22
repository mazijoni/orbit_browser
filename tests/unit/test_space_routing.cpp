#include "core/space_routing.h"
#include "test.h"

using orbit::ParseRoutes;
using orbit::RouteMatches;
using orbit::RouteUrl;
using orbit::SerializeRoutes;
using orbit::SpaceRoute;

TEST(Routing_ContainsAndEqualToFollowZen) {
  const SpaceRoute yt{SpaceRoute::Match::kContains, L"YouTube.com", 7};
  EXPECT_TRUE(RouteMatches(yt, L"https://www.youtube.com/watch?v=1"));
  EXPECT_TRUE(!RouteMatches(yt, L"https://vimeo.com/"));
  const SpaceRoute eq{SpaceRoute::Match::kEqualTo, L"https://www.example.com/", 3};
  EXPECT_TRUE(RouteMatches(eq, L"http://example.com"));
  EXPECT_TRUE(!RouteMatches(eq, L"https://example.com/page"));
  EXPECT_TRUE(!RouteMatches({SpaceRoute::Match::kContains, L"   ", 1}, L"https://a/"));
}

TEST(Routing_FirstMatchThenExternalDefault) {
  const std::vector<SpaceRoute> routes = {{SpaceRoute::Match::kContains, L"github", 5},
                                          {SpaceRoute::Match::kContains, L"git", 6}};
  EXPECT_TRUE(RouteUrl(routes, L"https://github.com/", false, 9) == 5);
  EXPECT_TRUE(!RouteUrl(routes, L"https://news.ycombinator.com/", false, 9));
  EXPECT_TRUE(RouteUrl(routes, L"https://news.ycombinator.com/", true, 9) == 9);
  EXPECT_TRUE(!RouteUrl(routes, L"https://news.ycombinator.com/", true, 0));
}

TEST(Routing_SerializeRoundTripsAndSkipsGarbage) {
  const std::vector<SpaceRoute> routes = {{SpaceRoute::Match::kContains, L"you\ttube", 7},
                                          {SpaceRoute::Match::kEqualTo, L"example.com", 0}};
  const auto back = ParseRoutes(SerializeRoutes(routes));
  EXPECT_TRUE(back.size() == 2 && back[0].reference == L"you tube" && back[0].space_id == 7 &&
              back[1].match == SpaceRoute::Match::kEqualTo);
  EXPECT_TRUE(ParseRoutes(L"regex\t1\t.*\nnope\ncontains\tx\ty\ncontains\t2\t \n").empty());
}
