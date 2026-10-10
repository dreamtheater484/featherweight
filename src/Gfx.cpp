#include "Gfx.h"

#include <cmath>
#include <map>
#include <tuple>

namespace gfx {
namespace {

ComPtr<ID2D1Factory1> g_d2d;
ComPtr<IDWriteFactory> g_dwrite;
ComPtr<IWICImagingFactory> g_wic;
std::wstring g_uiFont = L"Segoe UI";
std::wstring g_iconFont = L"Segoe MDL2 Assets";

bool FontExists(const wchar_t* family) {
  ComPtr<IDWriteFontCollection> fonts;
  if (FAILED(g_dwrite->GetSystemFontCollection(&fonts))) return false;
  UINT32 index = 0;
  BOOL exists = FALSE;
  fonts->FindFamilyName(family, &index, &exists);
  return exists != FALSE;
}

}  // namespace

bool InitFactories() {
  D2D1_FACTORY_OPTIONS opts{};
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                               reinterpret_cast<void**>(g_d2d.GetAddressOf())))) {
    return false;
  }
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(g_dwrite.GetAddressOf())))) {
    return false;
  }
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
  if (FontExists(L"Segoe UI Variable Text")) g_uiFont = L"Segoe UI Variable Text";
  if (FontExists(L"Segoe Fluent Icons")) g_iconFont = L"Segoe Fluent Icons";
  return true;
}

ID2D1Factory1* D2D() { return g_d2d.Get(); }
IDWriteFactory* DWrite() { return g_dwrite.Get(); }
IWICImagingFactory* WIC() { return g_wic.Get(); }
const std::wstring& UiFont() { return g_uiFont; }
const std::wstring& IconFont() { return g_iconFont; }

D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

void Canvas::Begin(ID2D1DeviceContext* dc) {
  if (dc_ != dc) brush_.Reset();
  dc_ = dc;
  if (!brush_) dc_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &brush_);
}

void Canvas::End() {}

ID2D1SolidColorBrush* Canvas::Brush(D2D1_COLOR_F c) {
  brush_->SetColor(c);
  return brush_.Get();
}

void Canvas::Fill(const D2D1_RECT_F& r, D2D1_COLOR_F c) {
  if (c.a <= 0) return;
  dc_->FillRectangle(r, Brush(c));
}

void Canvas::FillRound(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c) {
  if (c.a <= 0) return;
  dc_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Brush(c));
}

void Canvas::StrokeRound(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c, float width) {
  if (c.a <= 0) return;
  D2D1_RECT_F inset = Inflate(r, -width / 2, -width / 2);
  dc_->DrawRoundedRectangle(D2D1::RoundedRect(inset, radius, radius), Brush(c), width);
}

void Canvas::Stroke(const D2D1_RECT_F& r, D2D1_COLOR_F c, float width) {
  if (c.a <= 0) return;
  dc_->DrawRectangle(Inflate(r, -width / 2, -width / 2), Brush(c), width);
}

void Canvas::Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float width) {
  if (c.a <= 0) return;
  dc_->DrawLine({x1, y1}, {x2, y2}, Brush(c), width);
}

void Canvas::FillEllipse(float cx, float cy, float rx, float ry, D2D1_COLOR_F c) {
  if (c.a <= 0) return;
  dc_->FillEllipse(D2D1::Ellipse({cx, cy}, rx, ry), Brush(c));
}

void Canvas::SoftShadow(const D2D1_RECT_F& r, float radius, float spread, D2D1_COLOR_F c, float offsetY) {
  if (c.a <= 0 || spread <= 0) return;
  const int layers = 6;
  for (int i = layers; i >= 1; --i) {
    float t = static_cast<float>(i) / layers;
    float grow = spread * t;
    D2D1_RECT_F rr = Inflate(r, grow, grow);
    rr.top += offsetY * t;
    rr.bottom += offsetY * t;
    D2D1_COLOR_F lc = c;
    lc.a = c.a * (1.0f - t) * (1.0f - t) * 0.55f + c.a * 0.04f;
    FillRound(rr, radius + grow, lc);
  }
}

IDWriteTextFormat* Canvas::Format(float sizePx, DWRITE_FONT_WEIGHT weight, bool icon) {
  static std::map<std::tuple<int, int, bool>, ComPtr<IDWriteTextFormat>> cache;
  auto key = std::make_tuple(static_cast<int>(std::lround(sizePx * 10)), static_cast<int>(weight), icon);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second.Get();
  ComPtr<IDWriteTextFormat> fmt;
  g_dwrite->CreateTextFormat(icon ? g_iconFont.c_str() : g_uiFont.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, sizePx, L"en-us", &fmt);
  if (!fmt) return nullptr;
  fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  if (!icon) {
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    ComPtr<IDWriteInlineObject> ellipsis;
    g_dwrite->CreateEllipsisTrimmingSign(fmt.Get(), &ellipsis);
    fmt->SetTrimming(&trimming, ellipsis.Get());
  } else {
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  }
  cache[key] = fmt;
  return fmt.Get();
}

void Canvas::Text(std::wstring_view text, const D2D1_RECT_F& r, D2D1_COLOR_F c, float sizePx,
                  DWRITE_FONT_WEIGHT weight, Align align) {
  if (text.empty() || c.a <= 0) return;
  IDWriteTextFormat* fmt = Format(sizePx, weight, false);
  if (!fmt) return;
  fmt->SetTextAlignment(align == Align::Leading   ? DWRITE_TEXT_ALIGNMENT_LEADING
                        : align == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                 : DWRITE_TEXT_ALIGNMENT_TRAILING);
  dc_->DrawText(text.data(), static_cast<UINT32>(text.size()), fmt, r, Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Canvas::Icon(wchar_t glyph, const D2D1_RECT_F& r, D2D1_COLOR_F c, float sizePx) {
  if (c.a <= 0) return;
  IDWriteTextFormat* fmt = Format(sizePx, DWRITE_FONT_WEIGHT_REGULAR, true);
  if (!fmt) return;
  // Snap the glyph box to whole pixels so icons stay crisp.
  float cx = std::round((r.left + r.right) / 2), cy = std::round((r.top + r.bottom) / 2);
  float half = std::ceil(sizePx);
  D2D1_RECT_F box{cx - half, cy - half, cx + half, cy + half};
  dc_->DrawText(&glyph, 1, fmt, box, Brush(c), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

float Canvas::MeasureText(std::wstring_view text, float sizePx, DWRITE_FONT_WEIGHT weight) {
  if (text.empty()) return 0;
  IDWriteTextFormat* fmt = Format(sizePx, weight, false);
  ComPtr<IDWriteTextLayout> layout;
  if (FAILED(g_dwrite->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt, 100000.f, 1000.f,
                                        &layout))) {
    return 0;
  }
  DWRITE_TEXT_METRICS m{};
  layout->GetMetrics(&m);
  return m.widthIncludingTrailingWhitespace;
}

void Canvas::PushClip(const D2D1_RECT_F& r) { dc_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED); }
void Canvas::PopClip() { dc_->PopAxisAlignedClip(); }

}  // namespace gfx
