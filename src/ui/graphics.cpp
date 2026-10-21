#include "ui/graphics.h"

#include "core/log.h"
#include "ui/theme.h"

namespace orbit::ui {
namespace {

bool HasFontFamily(IDWriteFactory* factory, const wchar_t* family) {
  ComPtr<IDWriteFontCollection> fonts;
  if (FAILED(factory->GetSystemFontCollection(&fonts))) return false;
  UINT32 index = 0;
  BOOL exists = FALSE;
  return SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists;
}

}  // namespace

Graphics& Graphics::Get() {
  static Graphics instance;
  return instance;
}

Graphics::Graphics() {
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()))) {
    ORBIT_LOG_ERROR("D2D1CreateFactory failed");
  }
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())))) {
    ORBIT_LOG_ERROR("DWriteCreateFactory failed");
    return;
  }
  // Windows 11 ships Segoe UI Variable and Segoe Fluent Icons; fall back for
  // older builds so the UI stays legible.
  ui_family_ = HasFontFamily(dwrite_.Get(), L"Segoe UI Variable Text") ? L"Segoe UI Variable Text"
                                                                         : L"Segoe UI";
  const wchar_t* icon_family = HasFontFamily(dwrite_.Get(), L"Segoe Fluent Icons")
                                   ? L"Segoe Fluent Icons"
                                   : L"Segoe MDL2 Assets";
  text_ = MakeFormat(ui_family_, metrics::kFontSize, DWRITE_FONT_WEIGHT_NORMAL, false);
  text_bold_ = MakeFormat(ui_family_, metrics::kFontSize, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
  text_medium_ = MakeFormat(ui_family_, metrics::kFontSize, DWRITE_FONT_WEIGHT_MEDIUM, false);
  text_small_ = MakeFormat(ui_family_, metrics::kFontSizeSmall, DWRITE_FONT_WEIGHT_NORMAL, false);
  text_center_ = MakeFormat(ui_family_, metrics::kFontSize, DWRITE_FONT_WEIGHT_SEMI_BOLD, true);
  icons_ = MakeFormat(icon_family, metrics::kIconSize, DWRITE_FONT_WEIGHT_NORMAL, true);
  icons_small_ = MakeFormat(icon_family, 10.f, DWRITE_FONT_WEIGHT_NORMAL, true);
  icons_caption_ = MakeFormat(icon_family, 10.f, DWRITE_FONT_WEIGHT_NORMAL, true);
}

ComPtr<IDWriteTextFormat> Graphics::MakeFormat(const wchar_t* family, float size,
                                               DWRITE_FONT_WEIGHT weight, bool centered) {
  ComPtr<IDWriteTextFormat> format;
  if (FAILED(dwrite_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, size, L"",
                                       format.GetAddressOf()))) {
    return nullptr;
  }
  format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  format->SetTextAlignment(centered ? DWRITE_TEXT_ALIGNMENT_CENTER
                                    : DWRITE_TEXT_ALIGNMENT_LEADING);
  format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  if (!centered) {
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(format.Get(), &ellipsis))) {
      format->SetTrimming(&trimming, ellipsis.Get());
    }
  }
  return format;
}

void DrawGlyph(ID2D1RenderTarget* rt, IDWriteTextFormat* format, wchar_t glyph,
               const D2D1_RECT_F& rect, ID2D1Brush* brush) {
  if (!format) return;
  rt->DrawText(&glyph, 1, format, rect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void DrawTextIn(ID2D1RenderTarget* rt, IDWriteTextFormat* format, std::wstring_view text,
                const D2D1_RECT_F& rect, ID2D1Brush* brush) {
  if (!format || text.empty()) return;
  rt->DrawText(text.data(), static_cast<UINT32>(text.size()), format, rect, brush,
               D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

}  // namespace orbit::ui
