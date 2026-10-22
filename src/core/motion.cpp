#include "core/motion.h"

#include <algorithm>
#include <cmath>

namespace orbit {

float SpringNoBounce(float u) {
  if (u <= 0.f) return 0.f;
  if (u >= 1.f) return 1.f;
  // x(t) = 1 - (1 + k t) e^(-k t); k = 6.6 leaves ~1% at the end, which the
  // u >= 1 branch snaps away.
  constexpr float k = 6.6f;
  return 1.f - (1.f + k * u) * std::exp(-k * u);
}

float EaseInOut(float u) {
  u = std::clamp(u, 0.f, 1.f);
  return u < 0.5f ? 4.f * u * u * u : 1.f - std::pow(-2.f * u + 2.f, 3.f) / 2.f;
}

void Transition::Start(float target, uint64_t now_ms, uint32_t duration_ms, uint32_t delay_ms,
                       Curve curve) {
  if (target == to_ && Running(now_ms)) return;
  from_ = Value(now_ms);
  to_ = target;
  start_ms_ = now_ms + delay_ms;
  duration_ms_ = duration_ms;
  curve_ = curve;
  if (duration_ms == 0 && delay_ms == 0) from_ = to_;
}

void Transition::Jump(float value) {
  from_ = to_ = value;
  duration_ms_ = 0;
}

float Transition::Value(uint64_t now_ms) const {
  if (now_ms < start_ms_) return from_;
  if (duration_ms_ == 0) return to_;
  const float u = static_cast<float>(now_ms - start_ms_) / static_cast<float>(duration_ms_);
  float p = 1.f;
  switch (curve_) {
    case Curve::kSpring: p = SpringNoBounce(u); break;
    case Curve::kEaseInOut: p = EaseInOut(u); break;
    case Curve::kLinear: p = std::clamp(u, 0.f, 1.f); break;
  }
  return from_ + (to_ - from_) * p;
}

bool Transition::Running(uint64_t now_ms) const {
  return from_ != to_ && now_ms < start_ms_ + duration_ms_;
}

int SpaceSwitchStep(int from_index, int to_index, int count) {
  if (count <= 0) return 0;
  int diff = to_index - from_index;
  if (diff > count / 2) {
    diff -= count;
  } else if (diff < -(count / 2)) {
    diff += count;
  }
  return diff;
}

DownloadArc PlanDownloadArc(float start_x, float start_y, float end_x, float end_y, float view_h) {
  DownloadArc arc;
  arc.dx = end_x - start_x;
  arc.dy = end_y - start_y;
  const float distance = std::sqrt(arc.dx * arc.dx + arc.dy * arc.dy);
  const float top_space = std::min(start_y, end_y);
  const float bottom_space = view_h - std::max(start_y, end_y);
  arc.downward = bottom_space > top_space;
  const float space = std::max(0.f, arc.downward ? bottom_space : top_space);
  arc.height = std::min({distance * 2.f, 1200.f, space * 0.8f});
  return arc;
}

DownloadArcFrame DownloadArcAt(const DownloadArc& arc, float p) {
  p = std::clamp(p, 0.f, 1.f);
  const float eased = p < 0.5f ? 2.f * p * p : -1.f + (4.f - 2.f * p) * p;
  DownloadArcFrame f;
  if (p < 0.3f) {
    f.opacity = 0.3f + (p / 0.3f) * 0.6f;
  } else if (p < 0.98f) {
    f.opacity = 0.9f + ((p - 0.3f) / 0.6f) * 0.1f;
  } else {
    f.opacity = 1.f - ((p - 0.9f) / 0.1f);
  }
  f.opacity = std::clamp(f.opacity, 0.f, 1.f);
  f.scale = p < 0.5f ? 0.5f + (p / 0.5f) * 1.3f : 1.8f - ((p - 0.5f) / 0.5f) * (1.8f - 0.45f);
  const float bend = 2.f * eased - 1.f;
  f.x = arc.dx * eased;
  f.y = arc.dy * eased + (arc.downward ? 1.f : -1.f) * arc.height * (1.f - bend * bend);
  return f;
}

ArcBox GlanceArcAt(const ArcBox& from, const ArcBox& to, float view_h, float p, bool opening) {
  p = std::clamp(p, 0.f, 1.f);
  float eased;
  if (opening) {
    constexpr float c1 = 0.6f, c3 = c1 + 1.f;  // easeOutBack
    const float q = p - 1.f;
    eased = 1.f + c3 * q * q * q + c1 * q * q;
  } else {
    eased = 1.f - std::pow(1.f - p, 6.f);
  }
  const float dx = to.cx - from.cx, dy = to.cy - from.cy;
  const float distance = std::sqrt(dx * dx + dy * dy);
  const float top_space = std::min(from.cy, to.cy);
  const float bottom_space = view_h - std::max(from.cy, to.cy);
  const bool downward = bottom_space > top_space;
  const float space = std::max(0.f, downward ? bottom_space : top_space);
  const float height = std::min({distance * 0.4f, 30.f, space * 0.6f});
  const float bend = 2.f * eased - 1.f;
  ArcBox box;
  box.w = from.w + (to.w - from.w) * eased;
  box.h = from.h + (to.h - from.h) * eased;
  box.cx = from.cx + dx * eased;
  box.cy = from.cy + dy * eased + (downward ? 1.f : -1.f) * height * (1.f - bend * bend);
  return box;
}

}  // namespace orbit
