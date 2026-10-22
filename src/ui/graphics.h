#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string_view>

namespace orbit::ui {

using Microsoft::WRL::ComPtr;

// Process-wide Direct2D / DirectWrite factories and shared text formats.
// Created lazily on the UI thread; DPI-independent (formats are in DIPs).
class Graphics {
 public:
  static Graphics& Get();

  ID2D1Factory* d2d() const { return d2d_.Get(); }
  IDWriteFactory* dwrite() const { return dwrite_.Get(); }

  IDWriteTextFormat* text() const { return text_.Get(); }             // body, left, ellipsis
  IDWriteTextFormat* text_bold() const { return text_bold_.Get(); }
  IDWriteTextFormat* text_medium() const { return text_medium_.Get(); }
  IDWriteTextFormat* text_small() const { return text_small_.Get(); }
  IDWriteTextFormat* text_center() const { return text_center_.Get(); }
  IDWriteTextFormat* icons() const { return icons_.Get(); }            // centered glyphs
  IDWriteTextFormat* icons_small() const { return icons_small_.Get(); }
  IDWriteTextFormat* icons_caption() const { return icons_caption_.Get(); }  // window controls
  const wchar_t* ui_font_family() const { return ui_family_; }

 private:
  Graphics();
  ComPtr<IDWriteTextFormat> MakeFormat(const wchar_t* family, float size,
                                       DWRITE_FONT_WEIGHT weight, bool centered);

  ComPtr<ID2D1Factory> d2d_;
  ComPtr<IDWriteFactory> dwrite_;
  ComPtr<IDWriteTextFormat> text_, text_bold_, text_medium_, text_small_, text_center_, icons_,
      icons_small_, icons_caption_;
  const wchar_t* ui_family_ = L"Segoe UI";
};

// Drawing helpers that operate in DIPs on any render target.
void DrawGlyph(ID2D1RenderTarget* rt, IDWriteTextFormat* format, wchar_t glyph,
               const D2D1_RECT_F& rect, ID2D1Brush* brush);
void DrawTextIn(ID2D1RenderTarget* rt, IDWriteTextFormat* format, std::wstring_view text,
                const D2D1_RECT_F& rect, ID2D1Brush* brush);
inline D2D1_RECT_F Inset(const D2D1_RECT_F& r, float dx, float dy) {
  return {r.left + dx, r.top + dy, r.right - dx, r.bottom - dy};
}
inline bool Contains(const D2D1_RECT_F& r, float x, float y) {
  return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// Windows' "Animation effects" setting (SPI_GETCLIENTAREAANIMATION); when
// it is off, chrome animations jump to their end state.
bool AnimationsEnabled();
inline uint32_t AnimMs(uint32_t ms) { return AnimationsEnabled() ? ms : 0; }

}  // namespace orbit::ui
