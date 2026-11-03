#pragma once

#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <string_view>

using Microsoft::WRL::ComPtr;

namespace gfx {

bool InitFactories();
ID2D1Factory1* D2D();
IDWriteFactory* DWrite();
IWICImagingFactory* WIC();

// "Segoe UI Variable Text" on Windows 11, "Segoe UI" otherwise.
const std::wstring& UiFont();
// "Segoe Fluent Icons" on Windows 11, "Segoe MDL2 Assets" otherwise.
const std::wstring& IconFont();

inline D2D1_COLOR_F Rgb(uint32_t rgb, float a = 1.f) {
  return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, a);
}
inline D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) {
  c.a *= a;
  return c;
}
D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t);

inline D2D1_RECT_F RectF(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
inline D2D1_RECT_F RectF(const RECT& rc) {
  return D2D1::RectF((float)rc.left, (float)rc.top, (float)rc.right, (float)rc.bottom);
}
inline D2D1_RECT_F Inflate(D2D1_RECT_F r, float dx, float dy) { return {r.left - dx, r.top - dy, r.right + dx, r.bottom + dy}; }
inline bool Contains(const D2D1_RECT_F& r, float x, float y) {
  return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}
inline float Width(const D2D1_RECT_F& r) { return r.right - r.left; }
inline float Height(const D2D1_RECT_F& r) { return r.bottom - r.top; }

enum class Align { Leading, Center, Trailing };

// Thin drawing helper over a Direct2D device context. Coordinates are device pixels.
class Canvas {
 public:
  void Begin(ID2D1DeviceContext* dc);
  void End();
  ID2D1DeviceContext* DC() const { return dc_; }

  ID2D1SolidColorBrush* Brush(D2D1_COLOR_F c);
  void Fill(const D2D1_RECT_F& r, D2D1_COLOR_F c);
  void FillRound(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c);
  void StrokeRound(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c, float width);
  void Stroke(const D2D1_RECT_F& r, D2D1_COLOR_F c, float width);
  void Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float width);
  void FillEllipse(float cx, float cy, float rx, float ry, D2D1_COLOR_F c);

  // A soft shadow made of stacked translucent rounded rectangles.
  void SoftShadow(const D2D1_RECT_F& r, float radius, float spread, D2D1_COLOR_F c, float offsetY = 0);

  void Text(std::wstring_view text, const D2D1_RECT_F& r, D2D1_COLOR_F c, float sizePx,
            DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_REGULAR, Align align = Align::Leading);
  void Icon(wchar_t glyph, const D2D1_RECT_F& r, D2D1_COLOR_F c, float sizePx);
  float MeasureText(std::wstring_view text, float sizePx, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_REGULAR);
  // Multi-line text that wraps at the rectangle's width and is cut off after `maxLines`.
  // Returns the height used.
  float TextWrapped(std::wstring_view text, const D2D1_RECT_F& r, D2D1_COLOR_F c, float sizePx, int maxLines,
                    bool draw = true);

  void PushClip(const D2D1_RECT_F& r);
  void PopClip();

  static IDWriteTextFormat* Format(float sizePx, DWRITE_FONT_WEIGHT weight, bool icon);

 private:
  ID2D1DeviceContext* dc_ = nullptr;
  ComPtr<ID2D1SolidColorBrush> brush_;
};

}  // namespace gfx
