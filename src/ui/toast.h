#pragma once

#include <windows.h>

#include <d2d1.h>
#include <wrl/client.h>

#include <functional>
#include <string>

namespace orbit::ui {

// Zen's toast (ZenToasts.mjs, zen-popup.css #zen-toast-container): a short
// confirmation shown in the window's top-right corner, 8 DIPs from the edges.
// It pops in with a spring scale (0.5 s, bounce 0.2), stays for its timeout
// (2 s by default; hovering pauses it), then fades and shrinks out (0.2 s).
// Showing the same message again while it is up restarts it from scale 0.2.
//
// Drawn into a layered, non-activating popup owned by the browser window, so
// it can float over the web content with per-pixel alpha (rounded corners
// and a soft shadow) without taking focus. UI thread only.
class Toast {
 public:
  struct Content {
    std::wstring id;      // identifies the message (re-show detection, tests)
    std::wstring text;    // main label
    std::wstring pill;    // optional shortcut, drawn as a key cap after |text|
    std::wstring suffix;  // optional text after the pill
    int timeout_ms = 2000;
    // An optional icon button at the end (Zen's toast `button`, e.g. share).
    wchar_t button_glyph = 0;
    std::wstring button_label;
    std::function<void()> on_button;
  };
  struct Style {
    D2D1_COLOR_F primary;  // --zen-primary-color (the space's color)
    bool dark = true;
  };

  explicit Toast(HWND owner);
  ~Toast();
  Toast(const Toast&) = delete;
  Toast& operator=(const Toast&) = delete;

  void Show(const Content& content, const Style& style);
  void Hide();             // immediately, without animation
  void Reposition();       // owner moved or resized
  bool visible() const { return phase_ != Phase::kHidden; }
  // The visible message's id, or empty. Used by the self-tests.
  const std::wstring& visible_id() const;
  bool has_button() const { return visible() && content_.button_glyph != 0; }
  HWND hwnd() const { return hwnd_; }
  // The button's centre in screen pixels (tests click it), or {-1, -1}.
  POINT button_center() const;

 private:
  enum class Phase { kHidden, kShowing, kShown, kHiding };

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
  void Measure();
  void Render();
  void Tick();
  void ScheduleHide();
  // The pointer is over the toast's body (not its shadow) right now.
  bool PointerOverBody() const;
  float Dpi() const;

  HWND owner_;
  HWND hwnd_ = nullptr;
  Content content_;
  Style style_{};
  Phase phase_ = Phase::kHidden;
  ULONGLONG phase_start_ = 0;
  float start_scale_ = 0.f;
  bool hovered_ = false;  // kept up while the pointer rests on it
  POINT shown_at_pointer_{};  // where the pointer was when it appeared
  // Measured toast body in DIPs (without the shadow margin).
  float body_w_ = 0.f;
  float text_w_ = 0.f;
  float pill_w_ = 0.f;
  float suffix_w_ = 0.f;
  D2D1_RECT_F button_{};  // DIPs, in the window
  bool button_hot_ = false;
  Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> rt_;
};

}  // namespace orbit::ui
