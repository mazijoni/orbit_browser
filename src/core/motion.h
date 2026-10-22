// Timing helpers for chrome animations (no UI dependency, unit-testable).
//
// Zen animates with motion.dev springs ({type: "spring", bounce: 0,
// duration}); SpringNoBounce approximates that curve. Every function takes
// the current time explicitly so the tests can drive it.
#pragma once

#include <cstdint>

namespace orbit {

// Critically damped spring, 0 → ~1 over u in [0, 1] (exactly 1 at u >= 1).
float SpringNoBounce(float u);
float EaseInOut(float u);  // CSS ease-in-out (cubic approximation)

// A value moving from one number to another over a fixed duration, after an
// optional delay. Retargeting mid-flight starts from the current value.
class Transition {
 public:
  enum class Curve { kSpring, kEaseInOut, kLinear };

  explicit Transition(float value = 0.f) : from_(value), to_(value) {}

  // Moves to `target`; a zero duration (reduced motion) jumps there.
  void Start(float target, uint64_t now_ms, uint32_t duration_ms, uint32_t delay_ms = 0,
             Curve curve = Curve::kEaseInOut);
  void Jump(float value);

  float Value(uint64_t now_ms) const;
  float target() const { return to_; }
  bool Running(uint64_t now_ms) const;

 private:
  float from_;
  float to_;
  uint64_t start_ms_ = 0;
  uint32_t duration_ms_ = 0;
  Curve curve_ = Curve::kEaseInOut;
};

// ZenSpaceManager._animateTabs: the signed step from one space to another,
// wrapped so that the shorter way round is taken (|result| <= count / 2).
int SpaceSwitchStep(int from_index, int to_index, int count);

// ZenDownloadAnimation: a download's icon flies from the click to the
// library button on an arc. Distances in DIPs, relative to the start.
struct DownloadArc {
  float dx = 0.f, dy = 0.f;     // end - start
  float height = 0.f;           // #calculateOptimalArc
  bool downward = false;        // arcs into the larger free space
};
// |view_h|: the window height; the arc stays within 80% of the free space
// above (or below) both ends, and at most 2x the distance or 1200.
DownloadArc PlanDownloadArc(float start_x, float start_y, float end_x, float end_y, float view_h);
struct DownloadArcFrame {
  float x = 0.f, y = 0.f;  // offset from the start
  float scale = 1.f;
  float opacity = 1.f;
};
// #createArcAnimationSequence at |progress| in [0, 1] (easeInOutQuad along
// the path; 0.5 -> 1.8 -> 0.45 scale; fade in, hold, fade out at the end).
DownloadArcFrame DownloadArcAt(const DownloadArc& arc, float progress);

// ZenGlanceManager.#createGlanceArcSequence: the glance grows from the
// clicked link (|from|) to its place (|to|) on a slight arc (at most 30,
// 0.4x the distance, 60% of the free space) with easeOutBack; closing runs
// back with an ease-out (1 - (1 - t)^6). Centre and size, in DIPs.
struct ArcBox {
  float cx = 0.f, cy = 0.f, w = 0.f, h = 0.f;
};
ArcBox GlanceArcAt(const ArcBox& from, const ArcBox& to, float view_h, float progress, bool opening);

}  // namespace orbit
