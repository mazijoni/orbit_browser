#include "core/motion.h"
#include "test.h"

#include <cmath>

using orbit::SpaceSwitchStep;
using orbit::SpringNoBounce;
using orbit::Transition;
using orbit::DownloadArc;
using orbit::DownloadArcAt;
using orbit::DownloadArcFrame;
using orbit::PlanDownloadArc;
using orbit::ArcBox;
using orbit::GlanceArcAt;

TEST(Motion_SpringEndpointsAndMonotonic) {
  EXPECT_TRUE(SpringNoBounce(0.f) == 0.f);
  EXPECT_TRUE(SpringNoBounce(1.f) == 1.f);
  float last = 0.f;
  bool monotonic = true;
  for (int i = 1; i <= 100; ++i) {
    const float v = SpringNoBounce(static_cast<float>(i) / 100.f);
    if (v < last || v > 1.f) monotonic = false;  // bounce 0: no overshoot
    last = v;
  }
  EXPECT_TRUE(monotonic);
  EXPECT_TRUE(SpringNoBounce(0.5f) > 0.8f);  // springs front-load the motion
}

TEST(Motion_TransitionDelayDurationAndRetarget) {
  Transition t(0.f);
  t.Start(1.f, 1000, 200, 100);
  EXPECT_TRUE(t.Value(1050) == 0.f);  // still in the delay
  EXPECT_TRUE(t.Running(1050));
  const float mid = t.Value(1200);
  EXPECT_TRUE(mid > 0.f && mid < 1.f);
  EXPECT_TRUE(t.Value(1300) == 1.f);
  EXPECT_TRUE(!t.Running(1300));
  // Retargeting halfway starts from the current value, not from the end.
  t.Start(0.f, 1400, 100);
  t.Start(1.f, 1450, 100);
  const float v = t.Value(1450);
  EXPECT_TRUE(v > 0.f && v < 1.f);
  // Reduced motion: zero duration jumps.
  t.Start(0.f, 2000, 0);
  EXPECT_TRUE(t.Value(2000) == 0.f && !t.Running(2000));
}

TEST(Motion_SpaceSwitchTakesTheShorterWay) {
  EXPECT_TRUE(SpaceSwitchStep(0, 1, 3) == 1);
  EXPECT_TRUE(SpaceSwitchStep(1, 0, 3) == -1);
  EXPECT_TRUE(SpaceSwitchStep(2, 0, 3) == 1);   // wrap-around forward
  EXPECT_TRUE(SpaceSwitchStep(0, 2, 3) == -1);  // wrap-around back
  EXPECT_TRUE(SpaceSwitchStep(0, 2, 5) == 2);
  EXPECT_TRUE(SpaceSwitchStep(0, 1, 2) == 1);
  EXPECT_TRUE(SpaceSwitchStep(0, 0, 0) == 0);
}

TEST(Motion_DownloadArc) {
  // Click near the top of a 800-high window, library button at the bottom left.
  const DownloadArc arc = PlanDownloadArc(600.f, 100.f, 20.f, 780.f, 800.f);
  EXPECT_FALSE(arc.downward);  // 100 above vs 20 below: arcs up
  EXPECT_TRUE(arc.height > 79.f && arc.height < 81.f);  // 80% of the 100 free above
  const DownloadArcFrame start = DownloadArcAt(arc, 0.f);
  EXPECT_TRUE(start.x == 0.f && start.y == 0.f && start.scale == 0.5f);
  EXPECT_TRUE(start.opacity > 0.29f && start.opacity < 0.31f);
  const DownloadArcFrame mid = DownloadArcAt(arc, 0.5f);
  EXPECT_TRUE(mid.scale > 1.79f && mid.scale < 1.81f);
  EXPECT_TRUE(mid.y < arc.dy * 0.5f - 79.f);  // lifted by the arc at its middle
  const DownloadArcFrame end = DownloadArcAt(arc, 1.f);
  EXPECT_TRUE(end.x == arc.dx && end.y == arc.dy && end.opacity == 0.f);
  EXPECT_TRUE(end.scale > 0.44f && end.scale < 0.46f);
}

TEST(Motion_GlanceArc) {
  const ArcBox link{200.f, 100.f, 80.f, 20.f};
  const ArcBox page{500.f, 400.f, 800.f, 780.f};
  const ArcBox a = GlanceArcAt(link, page, 800.f, 0.f, true);
  EXPECT_TRUE(a.cx == 200.f && a.cy == 100.f && a.w == 80.f && a.h == 20.f);
  const ArcBox b = GlanceArcAt(link, page, 800.f, 1.f, true);
  EXPECT_TRUE(b.cx == 500.f && b.cy == 400.f && b.w == 800.f && b.h == 780.f);
  // easeOutBack overshoots a little before settling.
  bool overshoot = false;
  for (int i = 1; i < 20; ++i) overshoot |= GlanceArcAt(link, page, 800.f, i / 20.f, true).w > 800.f;
  EXPECT_TRUE(overshoot);
  // The arc bends by at most 30 DIPs off the straight line.
  const ArcBox mid = GlanceArcAt(link, page, 800.f, 0.5f, false);
  const float eased = 1.f - 0.5f * 0.5f * 0.5f * 0.5f * 0.5f * 0.5f;
  const float straight_y = 100.f + 300.f * eased;
  EXPECT_TRUE(std::abs(mid.cy - straight_y) <= 30.01f);
}
