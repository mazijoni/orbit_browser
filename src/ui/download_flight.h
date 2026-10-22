#pragma once

#include <windows.h>

#include <d2d1.h>
#include <wrl/client.h>

#include "core/motion.h"

namespace orbit::ui {

// ZenDownloadAnimation: when a download starts, a small circle with the
// download icon flies on an arc from the click to the library button and
// shrinks into it (1 s, zen.downloads.download-animation-duration). Without
// a visible library button (sidebar hidden), a library "box" slides in at
// the window's bottom corner while the circle flies to it, then slides out.
//
// Two click-through, non-activating layered popups owned by the browser
// window, so the animation can pass over the web content. UI thread only.
class DownloadFlight {
 public:
  struct Style {
    D2D1_COLOR_F surface;  // --zen-colors-hover-bg (the outer ring)
    D2D1_COLOR_F ink;      // --toolbar-color (inner circle, box icon)
    D2D1_COLOR_F accent;   // --zen-primary-color (the arrow), box background
    bool dark = true;
  };

  explicit DownloadFlight(HWND owner);
  ~DownloadFlight();
  DownloadFlight(const DownloadFlight&) = delete;
  DownloadFlight& operator=(const DownloadFlight&) = delete;

  // Screen pixels; |box_right|: the box enters from the right edge.
  void Fly(POINT from, POINT to, bool with_box, bool box_right, const Style& style,
           uint32_t duration_ms);
  bool flying() const { return arc_start_ != 0; }
  bool box_visible() const { return box_phase_ != BoxPhase::kHidden; }
  // Where the circle lands when the box stands in for the library button.
  static POINT BoxTarget(HWND owner, bool right);

 private:
  enum class BoxPhase { kHidden, kEntering, kHolding, kLeaving };
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  void Tick();
  void RenderArc(uint64_t now);
  void RenderBox(uint64_t now);
  // Draws |paint| into |hwnd| at |origin| (screen px) with per-pixel alpha.
  template <typename Paint>
  void Present(HWND hwnd, POINT origin, int size_px, float opacity, Paint&& paint);
  float Scale() const;

  HWND owner_;
  HWND arc_hwnd_ = nullptr;
  HWND box_hwnd_ = nullptr;
  Style style_{};
  POINT from_{};
  DownloadArc arc_{};
  uint64_t arc_start_ = 0;
  uint32_t duration_ms_ = 1000;
  BoxPhase box_phase_ = BoxPhase::kHidden;
  uint64_t box_phase_start_ = 0;
  bool box_right_ = false;
  Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> rt_;
};

}  // namespace orbit::ui
