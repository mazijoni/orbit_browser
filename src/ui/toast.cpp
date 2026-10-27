#include "ui/toast.h"

#include <dwrite.h>

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "ui/graphics.h"
#include "ui/theme.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace orbit::ui {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kToastClass[] = L"OrbitToast";
constexpr UINT_PTR kAnimationTimer = 1;
constexpr UINT_PTR kHideTimer = 2;
constexpr UINT_PTR kHoverTimer = 3;  // while hovered: has the pointer left?
constexpr UINT kHoverCheckMs = 400;
constexpr UINT kFrameMs = 15;
constexpr float kShowMs = 500.f;   // spring, bounce 0.2
constexpr float kHideMs = 200.f;   // opacity 1 -> 0, scale 1 -> 0.5
constexpr float kHeight = 42.f;    // --zen-toast-max-height
constexpr float kRadius = 14.f;
constexpr float kPadding = 6.f;    // --zen-toast-padding
constexpr float kLabelMargin = 4.f;
constexpr float kSpacing = 8.f;    // --zen-toast-spacing (from the window edges)
constexpr float kMargin = 24.f;    // room for the shadow around the body
constexpr float kPillPadX = 7.f;
constexpr float kPillHeight = 24.f;
constexpr float kButton = 28.f;

IDWriteTextFormat* Format(float size) {
  static ComPtr<IDWriteTextFormat> label, pill;
  ComPtr<IDWriteTextFormat>& slot = size > 13.f ? label : pill;
  if (!slot) {
    Graphics::Get().dwrite()->CreateTextFormat(
        Graphics::Get().ui_font_family(), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &slot);
    if (slot) {
      slot->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      slot->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
  }
  return slot.Get();
}

float TextWidth(const std::wstring& text, IDWriteTextFormat* format) {
  if (text.empty() || !format) return 0.f;
  ComPtr<IDWriteTextLayout> layout;
  if (FAILED(Graphics::Get().dwrite()->CreateTextLayout(
          text.c_str(), static_cast<UINT32>(text.size()), format, 2000.f, 100.f, &layout))) {
    return 0.f;
  }
  DWRITE_TEXT_METRICS m{};
  layout->GetMetrics(&m);
  return std::ceil(m.widthIncludingTrailingWhitespace);
}

// Underdamped spring from 0 to 1 over u in [0, 1] with a small overshoot,
// approximating motion's {type: "spring", bounce: 0.2}.
float Spring(float u) { return 1.f - std::exp(-6.f * u) * std::cos(9.f * u); }

float EaseOut(float u) { return 1.f - (1.f - u) * (1.f - u) * (1.f - u); }

}  // namespace

Toast::Toast(HWND owner) : owner_(owner) {
  static bool registered = false;
  HINSTANCE instance = reinterpret_cast<HINSTANCE>(&__ImageBase);
  if (!registered) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &Toast::WndProc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kToastClass;
    registered = ::RegisterClassExW(&wc) != 0;
  }
  hwnd_ = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kToastClass,
                            L"", WS_POPUP, 0, 0, 1, 1, owner_, nullptr, instance, this);
  if (!hwnd_) ORBIT_LOG_ERROR("toast window creation failed (%lu)", ::GetLastError());
}

Toast::~Toast() {
  if (hwnd_) {
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    ::DestroyWindow(hwnd_);
  }
}

POINT Toast::button_center() const {
  if (!has_button() || button_.right <= button_.left) return {-1, -1};
  RECT wr;
  ::GetWindowRect(hwnd_, &wr);
  const float scale = Dpi() / 96.f;
  return {wr.left + static_cast<LONG>(std::lround((button_.left + button_.right) / 2.f * scale)),
          wr.top + static_cast<LONG>(std::lround((button_.top + button_.bottom) / 2.f * scale))};
}

const std::wstring& Toast::visible_id() const {
  static const std::wstring kEmpty;
  if (!visible() || phase_ == Phase::kHiding || !::IsWindowVisible(hwnd_)) return kEmpty;
  // On screen means laid out for its message, not just flagged as shown.
  RECT r;
  ::GetWindowRect(hwnd_, &r);
  const float min_width = (body_w_ + 2 * kMargin) * Dpi() / 96.f - 2.f;
  return static_cast<float>(r.right - r.left) >= min_width && body_w_ > 0.f ? content_.id : kEmpty;
}

float Toast::Dpi() const { return static_cast<float>(::GetDpiForWindow(owner_)); }

void Toast::Show(const Content& content, const Style& style) {
  if (!hwnd_) return;
  const bool reused = visible() && phase_ != Phase::kHiding && content.id == content_.id;
  content_ = content;
  style_ = style;
  ::KillTimer(hwnd_, kHideTimer);
  ::KillTimer(hwnd_, kHoverTimer);
  hovered_ = false;  // a new message: the pointer must rest on it again
  ::GetCursorPos(&shown_at_pointer_);
  Measure();
  start_scale_ = reused ? 0.2f : 0.f;
  phase_ = Phase::kShowing;
  phase_start_ = ::GetTickCount64();
  Reposition();  // sizes the window for the new message and renders it
  // The window's title is the message, for assistive technology.
  std::wstring spoken = content_.text;
  if (!content_.pill.empty()) spoken += L" " + content_.pill;
  if (!content_.suffix.empty()) spoken += L" " + content_.suffix;
  if (!content_.button_label.empty()) spoken += L" (" + content_.button_label + L")";
  button_hot_ = false;
  ::SetWindowTextW(hwnd_, spoken.c_str());
  ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  ::NotifyWinEvent(EVENT_OBJECT_SHOW, hwnd_, OBJID_WINDOW, CHILDID_SELF);
  ::SetTimer(hwnd_, kAnimationTimer, kFrameMs, nullptr);
}

void Toast::Hide() {
  if (!hwnd_) return;
  ::KillTimer(hwnd_, kAnimationTimer);
  ::KillTimer(hwnd_, kHideTimer);
  ::KillTimer(hwnd_, kHoverTimer);
  hovered_ = false;
  phase_ = Phase::kHidden;
  ::ShowWindow(hwnd_, SW_HIDE);
}

void Toast::ScheduleHide() {
  if (phase_ != Phase::kShown) return;
  // Hovered only while the pointer is really there: Windows does not always
  // report leaving (the toast replaced or hidden under it), and a stale flag
  // kept every later toast up for good.
  if (hovered_ && !PointerOverBody()) hovered_ = false;
  if (hovered_) {
    ::SetTimer(hwnd_, kHoverTimer, kHoverCheckMs, nullptr);
  } else {
    ::SetTimer(hwnd_, kHideTimer, static_cast<UINT>(content_.timeout_ms), nullptr);
  }
}

bool Toast::PointerOverBody() const {
  if (!hwnd_ || !::IsWindowVisible(hwnd_)) return false;
  POINT pt;
  ::GetCursorPos(&pt);
  RECT r;
  ::GetWindowRect(hwnd_, &r);
  const int margin = static_cast<int>(std::lround(kMargin * Dpi() / 96.f));
  ::InflateRect(&r, -margin, -margin);
  return ::PtInRect(&r, pt) != FALSE;
}

void Toast::Measure() {
  text_w_ = TextWidth(content_.text, Format(14.f));
  pill_w_ = content_.pill.empty() ? 0.f : TextWidth(content_.pill, Format(12.f)) + 2 * kPillPadX;
  suffix_w_ = TextWidth(content_.suffix, Format(14.f));
  float w = kPadding + kLabelMargin + text_w_;
  if (pill_w_ > 0.f) w += 6.f + pill_w_;
  if (suffix_w_ > 0.f) w += 6.f + suffix_w_;
  if (content_.button_glyph) w += 6.f + kButton;
  body_w_ = w + kLabelMargin + kPadding + 4.f;
}

void Toast::Reposition() {
  if (!hwnd_ || !visible()) return;
  RECT client;
  ::GetClientRect(owner_, &client);
  POINT origin{client.right, client.top};
  ::ClientToScreen(owner_, &origin);
  const float scale = Dpi() / 96.f;
  const int w = static_cast<int>(std::ceil((body_w_ + 2 * kMargin) * scale));
  const int h = static_cast<int>(std::ceil((kHeight + 2 * kMargin) * scale));
  const int x = origin.x - static_cast<int>(std::lround((kSpacing + body_w_ + kMargin) * scale));
  const int y = origin.y + static_cast<int>(std::lround((kSpacing - kMargin) * scale));
  ::SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOACTIVATE | SWP_NOZORDER);
  if (phase_ != Phase::kHidden) Render();
}

void Toast::Tick() {
  const float elapsed = static_cast<float>(::GetTickCount64() - phase_start_);
  if (phase_ == Phase::kShowing && elapsed >= kShowMs) {
    phase_ = Phase::kShown;
    ::KillTimer(hwnd_, kAnimationTimer);
    ScheduleHide();
  } else if (phase_ == Phase::kHiding && elapsed >= kHideMs) {
    Hide();
    return;
  }
  Render();
}

void Toast::Render() {
  if (!hwnd_) return;
  RECT wr;
  ::GetWindowRect(hwnd_, &wr);
  const int pw = wr.right - wr.left;
  const int ph = wr.bottom - wr.top;
  if (pw <= 0 || ph <= 0) return;

  // Animation state.
  const float elapsed = static_cast<float>(::GetTickCount64() - phase_start_);
  float scale = 1.f;
  float opacity = 1.f;
  if (phase_ == Phase::kShowing) {
    const float u = std::clamp(elapsed / kShowMs, 0.f, 1.f);
    scale = start_scale_ + (1.f - start_scale_) * Spring(u);
  } else if (phase_ == Phase::kHiding) {
    const float u = EaseOut(std::clamp(elapsed / kHideMs, 0.f, 1.f));
    scale = 1.f - 0.5f * u;
    opacity = 1.f - u;
  }

  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = pw;
  bi.bmiHeader.biHeight = -ph;  // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = ::GetDC(nullptr);
  HDC mem = ::CreateCompatibleDC(screen);
  HBITMAP dib = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib) {
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);
    return;
  }
  HGDIOBJ old = ::SelectObject(mem, dib);

  if (!rt_) {
    const auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    Graphics::Get().d2d()->CreateDCRenderTarget(&props, &rt_);
  }
  RECT bind{0, 0, pw, ph};
  if (rt_ && SUCCEEDED(rt_->BindDC(mem, &bind))) {
    rt_->SetDpi(Dpi(), Dpi());
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(0, 0.f));
    const D2D1_RECT_F body{kMargin, kMargin, kMargin + body_w_, kMargin + kHeight};
    // transform-origin: top center.
    rt_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale,
                                              {kMargin + body_w_ / 2.f, kMargin}));
    ComPtr<ID2D1SolidColorBrush> brush;
    rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0.f), &brush);
    // box-shadow: 0 0 22px 2px rgba(0,0,0,.4) (dark) / .1 (light).
    const float shadow_alpha = style_.dark ? 0.4f : 0.1f;
    constexpr int kSteps = 7;
    for (int i = kSteps; i >= 1; --i) {
      const float grow = 2.f + 14.f * static_cast<float>(i) / kSteps;
      brush->SetColor(D2D1::ColorF(0, 0, 0, shadow_alpha / kSteps));
      rt_->FillRoundedRectangle(
          D2D1::RoundedRect(Inset(body, -grow, -grow), kRadius + grow, kRadius + grow),
          brush.Get());
    }
    // background: linear-gradient(to bottom, primary -40%,
    //                             color-mix(primary, #0f0f0f 20%))
    const D2D1_COLOR_F bottom = Mix(style_.primary, ColorFromRgb(0x0F0F0F), 0.8f);
    const D2D1_GRADIENT_STOP stops[] = {{0.f, Mix(bottom, style_.primary, 0.4f / 1.4f)},
                                        {1.f, bottom}};
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> gradient;
    if (SUCCEEDED(rt_->CreateGradientStopCollection(stops, 2, &collection)) &&
        SUCCEEDED(rt_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties({0, body.top}, {0, body.bottom}),
            collection.Get(), &gradient))) {
      rt_->FillRoundedRectangle(D2D1::RoundedRect(body, kRadius, kRadius), gradient.Get());
    }
    const D2D1_COLOR_F fg = style_.dark ? ColorFromRgb(0xFFFFFF, 0.92f)
                                        : ColorFromRgb(0x000000, 0.85f);
    float x = body.left + kPadding + kLabelMargin;
    brush->SetColor(fg);
    DrawTextIn(rt_.Get(), Format(14.f), content_.text, {x, body.top, x + text_w_ + 2, body.bottom},
               brush.Get());
    x += text_w_;
    if (pill_w_ > 0.f) {
      x += 6.f;
      const float cy = (body.top + body.bottom) / 2.f;
      const D2D1_RECT_F pill{x, cy - kPillHeight / 2.f, x + pill_w_, cy + kPillHeight / 2.f};
      brush->SetColor(style_.dark ? ColorFromRgb(0xFFFFFF, 0.1f) : ColorFromRgb(0xFFFFFF, 0.4f));
      rt_->FillRoundedRectangle(D2D1::RoundedRect(pill, 4.f, 4.f), brush.Get());
      brush->SetColor(fg);
      DrawTextIn(rt_.Get(), Format(12.f), content_.pill,
                 {pill.left + kPillPadX, pill.top, pill.right, pill.bottom}, brush.Get());
      x += pill_w_;
    }
    if (suffix_w_ > 0.f) {
      x += 6.f;
      DrawTextIn(rt_.Get(), Format(14.f), content_.suffix,
                 {x, body.top, x + suffix_w_ + 2, body.bottom}, brush.Get());
      x += suffix_w_;
    }
    button_ = {};
    if (content_.button_glyph) {
      x += 6.f;
      const float cy = (body.top + body.bottom) / 2.f;
      button_ = {x, cy - kButton / 2.f, x + kButton, cy + kButton / 2.f};
      if (button_hot_) {
        brush->SetColor(style_.dark ? ColorFromRgb(0xFFFFFF, 0.12f) : ColorFromRgb(0x000000, 0.08f));
        rt_->FillRoundedRectangle(D2D1::RoundedRect(button_, 8.f, 8.f), brush.Get());
      }
      brush->SetColor(fg);
      DrawGlyph(rt_.Get(), Graphics::Get().icons(), content_.button_glyph, button_, brush.Get());
    }
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) rt_.Reset();
  }

  POINT dst{wr.left, wr.top};
  SIZE size{pw, ph};
  POINT src{0, 0};
  BLENDFUNCTION blend{AC_SRC_OVER, 0,
                      static_cast<BYTE>(std::lround(std::clamp(opacity, 0.f, 1.f) * 255.f)),
                      AC_SRC_ALPHA};
  ::UpdateLayeredWindow(hwnd_, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);
  ::SelectObject(mem, old);
  ::DeleteObject(dib);
  ::DeleteDC(mem);
  ::ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK Toast::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wp, lp);
  }
  auto* self = reinterpret_cast<Toast*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self) return ::DefWindowProcW(hwnd, msg, wp, lp);
  return self->HandleMessage(msg, wp, lp);
}

LRESULT Toast::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_TIMER:
      if (wp == kAnimationTimer) {
        Tick();
      } else if (wp == kHoverTimer) {
        if (!PointerOverBody()) {
          ::KillTimer(hwnd_, kHoverTimer);
          hovered_ = false;
          ScheduleHide();
        }
      } else if (wp == kHideTimer) {
        ::KillTimer(hwnd_, kHideTimer);
        phase_ = Phase::kHiding;
        phase_start_ = ::GetTickCount64();
        ::SetTimer(hwnd_, kAnimationTimer, kFrameMs, nullptr);
      }
      return 0;
    case WM_MOUSEMOVE: {
      if (button_.right > button_.left) {
        const float scale = Dpi() / 96.f;
        const float x = static_cast<float>(static_cast<short>(LOWORD(lp))) / scale;
        const float y = static_cast<float>(static_cast<short>(HIWORD(lp))) / scale;
        const bool hot = x >= button_.left && x < button_.right && y >= button_.top && y < button_.bottom;
        if (hot != button_hot_) {
          button_hot_ = hot;
          Render();
        }
      }
      // Hovering keeps the toast up (mouseover clears the hide timer). Not
      // when it appeared under a pointer that has not moved since (it shows
      // over the copy-URL button of a top-right address bar): that kept it
      // up until the user happened to move away.
      POINT now;
      ::GetCursorPos(&now);
      if (!hovered_ && (now.x != shown_at_pointer_.x || now.y != shown_at_pointer_.y)) {
        hovered_ = true;
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        ::TrackMouseEvent(&tme);
        ::KillTimer(hwnd_, kHideTimer);
        if (phase_ == Phase::kShown) ::SetTimer(hwnd_, kHoverTimer, kHoverCheckMs, nullptr);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      hovered_ = false;
      ::KillTimer(hwnd_, kHoverTimer);
      if (button_hot_) {
        button_hot_ = false;
        Render();
      }
      ScheduleHide();
      return 0;
    case WM_SETCURSOR:
      ::SetCursor(::LoadCursorW(nullptr, button_hot_ ? IDC_HAND : IDC_ARROW));
      return TRUE;
    case WM_LBUTTONUP:
      if (content_.on_button && button_.right > button_.left) {
        const float scale = Dpi() / 96.f;
        const float x = static_cast<float>(static_cast<short>(LOWORD(lp))) / scale;
        const float y = static_cast<float>(static_cast<short>(HIWORD(lp))) / scale;
        if (x >= button_.left && x < button_.right && y >= button_.top && y < button_.bottom) {
          auto action = content_.on_button;  // may show another toast
          action();
        }
      }
      return 0;
  }
  return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace orbit::ui
