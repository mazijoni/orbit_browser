#include "ui/download_flight.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "ui/graphics.h"
#include "ui/theme.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace orbit::ui {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kFlightClass[] = L"OrbitDownloadFlight";
constexpr UINT_PTR kFrameTimer = 1;
constexpr UINT kFrameMs = 15;
constexpr float kArcWindow = 96.f;   // DIPs: the circle at 1.8x plus its shadow
constexpr float kCircle = 34.f;      // 30px + 2px padding each side
constexpr float kBox = 42.f;         // .zen-download-box-animation
constexpr float kBoxMargin = 10.f;   // room for its shadow
constexpr float kBoxBottom = 24.f;
constexpr float kBoxEnterMs = 350.f, kBoxSettleMs = 200.f;
constexpr float kBoxShrinkMs = 150.f, kBoxExitMs = 300.f;

uint64_t Now() { return ::GetTickCount64(); }

float EaseOut(float u) { return 1.f - (1.f - u) * (1.f - u); }
float EaseIn(float u) { return u * u * u; }  // cubic-bezier(0.5, 0, 0.75, 0), roughly
float EaseInOutSine(float u) { return 0.5f - std::cos(3.14159265f * u) / 2.f; }  // (.37,0,.63,1)

void SoftShadow(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, D2D1_POINT_2F c, float radius,
                float spread, float alpha) {
  constexpr int kSteps = 6;
  for (int i = kSteps; i >= 1; --i) {
    const float grow = spread * static_cast<float>(i) / kSteps;
    brush->SetColor(D2D1::ColorF(0, 0, 0, alpha / kSteps));
    rt->FillEllipse(D2D1::Ellipse(c, radius + grow, radius + grow), brush);
  }
}

}  // namespace

DownloadFlight::DownloadFlight(HWND owner) : owner_(owner) {
  static bool registered = false;
  HINSTANCE instance = reinterpret_cast<HINSTANCE>(&__ImageBase);
  if (!registered) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &DownloadFlight::WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kFlightClass;
    registered = ::RegisterClassExW(&wc) != 0;
  }
  // Click-through (WS_EX_TRANSPARENT) and never activated: pointer-events: none.
  constexpr DWORD kExStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
  arc_hwnd_ = ::CreateWindowExW(kExStyle, kFlightClass, L"", WS_POPUP, 0, 0, 1, 1, owner_, nullptr,
                                instance, this);
  box_hwnd_ = ::CreateWindowExW(kExStyle, kFlightClass, L"", WS_POPUP, 0, 0, 1, 1, owner_, nullptr,
                                instance, this);
  if (!arc_hwnd_ || !box_hwnd_) ORBIT_LOG_ERROR("download animation windows failed (%lu)", ::GetLastError());
}

DownloadFlight::~DownloadFlight() {
  for (HWND h : {arc_hwnd_, box_hwnd_}) {
    if (!h) continue;
    ::SetWindowLongPtrW(h, GWLP_USERDATA, 0);
    ::DestroyWindow(h);
  }
}

float DownloadFlight::Scale() const { return static_cast<float>(::GetDpiForWindow(owner_)) / 96.f; }

POINT DownloadFlight::BoxTarget(HWND owner, bool right) {
  // #determineEndPosition without a button: 42px in from the side, 40px up.
  RECT client;
  ::GetClientRect(owner, &client);
  const float scale = static_cast<float>(::GetDpiForWindow(owner)) / 96.f;
  POINT p{right ? client.right - static_cast<LONG>(42.f * scale) : static_cast<LONG>(42.f * scale),
          client.bottom - static_cast<LONG>(40.f * scale)};
  ::ClientToScreen(owner, &p);
  return p;
}

void DownloadFlight::Fly(POINT from, POINT to, bool with_box, bool box_right, const Style& style,
                         uint32_t duration_ms) {
  if (!arc_hwnd_ || !box_hwnd_) return;
  style_ = style;
  duration_ms_ = std::max<uint32_t>(duration_ms, 100);
  const float scale = Scale();
  RECT client;
  ::GetClientRect(owner_, &client);
  POINT origin{0, 0};
  ::ClientToScreen(owner_, &origin);
  // In DIPs relative to the window, as Zen's client coordinates.
  arc_ = PlanDownloadArc(static_cast<float>(from.x - origin.x) / scale,
                         static_cast<float>(from.y - origin.y) / scale,
                         static_cast<float>(to.x - origin.x) / scale,
                         static_cast<float>(to.y - origin.y) / scale,
                         static_cast<float>(client.bottom) / scale);
  from_ = from;
  arc_start_ = Now();
  if (with_box) {
    // #startBoxAnimation: a box already up just stays longer.
    if (box_phase_ == BoxPhase::kHolding) {
      box_phase_start_ = Now();
    } else if (box_phase_ == BoxPhase::kHidden || box_phase_ == BoxPhase::kLeaving) {
      box_phase_ = BoxPhase::kEntering;
      box_phase_start_ = Now();
      box_right_ = box_right;
    }
  }
  Tick();
  ::SetTimer(arc_hwnd_, kFrameTimer, kFrameMs, nullptr);
}

void DownloadFlight::Tick() {
  const uint64_t now = Now();
  RenderBox(now);
  RenderArc(now);
  if (!flying() && !box_visible()) ::KillTimer(arc_hwnd_, kFrameTimer);
}

template <typename Paint>
void DownloadFlight::Present(HWND hwnd, POINT origin, int size, float opacity, Paint&& paint) {
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = size;
  bi.bmiHeader.biHeight = -size;
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
  RECT bind{0, 0, size, size};
  if (rt_ && SUCCEEDED(rt_->BindDC(mem, &bind))) {
    const float dpi = Scale() * 96.f;
    rt_->SetDpi(dpi, dpi);
    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(0, 0.f));
    ComPtr<ID2D1SolidColorBrush> brush;
    rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0.f), &brush);
    if (brush) paint(rt_.Get(), brush.Get());
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) rt_.Reset();
  }
  POINT src{0, 0};
  SIZE sz{size, size};
  BLENDFUNCTION blend{AC_SRC_OVER, 0,
                      static_cast<BYTE>(std::lround(std::clamp(opacity, 0.f, 1.f) * 255.f)), AC_SRC_ALPHA};
  ::UpdateLayeredWindow(hwnd, screen, &origin, &sz, mem, &src, 0, &blend, ULW_ALPHA);
  ::SelectObject(mem, old);
  ::DeleteObject(dib);
  ::DeleteDC(mem);
  ::ReleaseDC(nullptr, screen);
  if (!::IsWindowVisible(hwnd)) ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
}

void DownloadFlight::RenderArc(uint64_t now) {
  if (!arc_start_) return;
  const float t = static_cast<float>(now - arc_start_) / static_cast<float>(duration_ms_);
  if (t >= 1.f) {
    arc_start_ = 0;  // #cleanArcAnimation
    ::ShowWindow(arc_hwnd_, SW_HIDE);
    return;
  }
  const DownloadArcFrame f = DownloadArcAt(arc_, EaseInOutSine(t));
  const float scale = Scale();
  const int size = static_cast<int>(std::ceil(kArcWindow * scale));
  const POINT origin{from_.x + static_cast<LONG>(std::lround(f.x * scale)) - size / 2,
                     from_.y + static_cast<LONG>(std::lround(f.y * scale)) - size / 2};
  Present(arc_hwnd_, origin, size, f.opacity, [&](ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush) {
    const D2D1_POINT_2F c{kArcWindow / 2.f, kArcWindow / 2.f};
    const float outer = kCircle / 2.f * f.scale;
    SoftShadow(rt, brush, c, outer, 8.f * f.scale, style_.dark ? 0.5f : 0.25f);  // --zen-big-shadow
    brush->SetColor(style_.surface);
    rt->FillEllipse(D2D1::Ellipse(c, outer, outer), brush);
    const float inner = (kCircle / 2.f - 2.f) * f.scale;
    brush->SetColor(style_.ink);
    rt->FillEllipse(D2D1::Ellipse(c, inner, inner), brush);
    // The download arrow (mask-size: 70%) in the space's color.
    const float glyph = 30.f * 0.7f * f.scale / metrics::kIconSize;
    D2D1_MATRIX_3X2_F old;
    rt->GetTransform(&old);
    rt->SetTransform(D2D1::Matrix3x2F::Scale(glyph, glyph, c) * old);
    brush->SetColor(style_.accent);
    DrawGlyph(rt, Graphics::Get().icons(), glyph::kDownload,
              {c.x - 10.f, c.y - 10.f, c.x + 10.f, c.y + 10.f}, brush);
    rt->SetTransform(old);
  });
}

void DownloadFlight::RenderBox(uint64_t now) {
  if (box_phase_ == BoxPhase::kHidden) return;
  float elapsed = static_cast<float>(now - box_phase_start_);
  float side = 24.f, box_scale = 1.f, opacity = 1.f;
  if (box_phase_ == BoxPhase::kEntering) {
    if (elapsed < kBoxEnterMs) {
      const float u = EaseOut(elapsed / kBoxEnterMs);
      side = -50.f + 84.f * u;
      box_scale = 0.8f + 0.3f * u;
    } else if (elapsed < kBoxEnterMs + kBoxSettleMs) {
      const float u = EaseInOut((elapsed - kBoxEnterMs) / kBoxSettleMs);
      side = 34.f - 10.f * u;
      box_scale = 1.1f - 0.1f * u;
    } else {
      box_phase_ = BoxPhase::kHolding;
      box_phase_start_ = now;
      elapsed = 0.f;
    }
  }
  if (box_phase_ == BoxPhase::kHolding && elapsed >= static_cast<float>(duration_ms_ + 200)) {
    box_phase_ = BoxPhase::kLeaving;  // #finishBoxAnimation
    box_phase_start_ = now;
    elapsed = 0.f;
  }
  if (box_phase_ == BoxPhase::kLeaving) {
    if (elapsed < kBoxShrinkMs) {
      box_scale = 1.f - 0.1f * EaseIn(elapsed / kBoxShrinkMs);
    } else if (elapsed < kBoxShrinkMs + kBoxExitMs) {
      const float u = EaseIn((elapsed - kBoxShrinkMs) / kBoxExitMs);
      side = 24.f - 74.f * u;
      box_scale = 0.9f - 0.1f * u;
      opacity = 1.f - u;
    } else {
      box_phase_ = BoxPhase::kHidden;
      ::ShowWindow(box_hwnd_, SW_HIDE);
      return;
    }
  }
  const float scale = Scale();
  RECT client;
  ::GetClientRect(owner_, &client);
  POINT corner{0, client.bottom};
  ::ClientToScreen(owner_, &corner);
  const float window = kBox + 2.f * kBoxMargin;
  const int size = static_cast<int>(std::ceil(window * scale));
  const float left_dip = box_right_ ? static_cast<float>(client.right) / scale - side - kBox : side;
  const POINT origin{corner.x + static_cast<LONG>(std::lround((left_dip - kBoxMargin) * scale)),
                     corner.y - static_cast<LONG>(std::lround((kBoxBottom + kBox + kBoxMargin) * scale))};
  Present(box_hwnd_, origin, size, opacity, [&](ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush) {
    const D2D1_POINT_2F c{window / 2.f, window / 2.f};
    rt->SetTransform(D2D1::Matrix3x2F::Scale(box_scale, box_scale, c));
    const D2D1_RECT_F body{kBoxMargin, kBoxMargin, kBoxMargin + kBox, kBoxMargin + kBox};
    for (int i = 4; i >= 1; --i) {  // box-shadow: 0 0 6px 1px rgba(0,0,0,.1)
      const float grow = 1.f + 1.5f * static_cast<float>(i);
      brush->SetColor(D2D1::ColorF(0, 0, 0, 0.1f / 4.f));
      rt->FillRoundedRectangle(
          D2D1::RoundedRect({body.left - grow, body.top - grow, body.right + grow, body.bottom + grow},
                            12.f + grow, 12.f + grow),
          brush);
    }
    brush->SetColor(style_.accent);
    rt->FillRoundedRectangle(D2D1::RoundedRect(body, 12.f, 12.f), brush);
    const float glyph = 24.f * 1.2f / metrics::kIconSize;
    rt->SetTransform(D2D1::Matrix3x2F::Scale(glyph * box_scale, glyph * box_scale, c));
    brush->SetColor(style_.ink);
    DrawGlyph(rt, Graphics::Get().icons(), glyph::kLibrary, {c.x - 10.f, c.y - 10.f, c.x + 10.f, c.y + 10.f},
              brush);
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
  });
}

LRESULT CALLBACK DownloadFlight::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wp, lp);
  }
  auto* self = reinterpret_cast<DownloadFlight*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self && msg == WM_TIMER && wp == kFrameTimer) {
    self->Tick();
    return 0;
  }
  if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  if (msg == WM_NCHITTEST) return HTTRANSPARENT;
  return ::DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace orbit::ui
