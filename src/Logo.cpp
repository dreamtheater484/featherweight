#include "Logo.h"

#include <cmath>
#include <vector>

using namespace gfx;

namespace logo {
namespace {

// Same geometry as tools/make_icon.py, so the app icon and the in-app logo match.
std::vector<D2D1_POINT_2F> FeatherOutline(int steps) {
  std::vector<D2D1_POINT_2F> upper, lower;
  for (int i = 0; i <= steps; ++i) {
    double t = static_cast<double>(i) / steps;
    double wu = 0.185 * std::sin(3.14159265 * std::pow(t, 0.72)) * (1.0 - 0.18 * t);
    double wl = 0.135 * std::sin(3.14159265 * std::pow(t, 0.80)) * (1.0 - 0.10 * t);
    const double cuts[2][3] = {{0.44, 0.075, 0.10}, {0.66, 0.055, 0.08}};
    for (const auto& cut : cuts) {
      double c = cut[0], d = cut[1], span = cut[2];
      if (t >= c - span && t <= c) wu -= d * (t - (c - span)) / span;
      else if (t > c && t < c + 0.012) wu -= d * (1.0 - (t - c) / 0.012);
    }
    upper.push_back({static_cast<float>(t), static_cast<float>(-wu)});
    lower.push_back({static_cast<float>(t), static_cast<float>(wl)});
  }
  for (auto it = lower.rbegin(); it != lower.rend(); ++it) upper.push_back(*it);
  return upper;
}

D2D1_POINT_2F Place(D2D1_POINT_2F p, float cx, float cy, float length) {
  const float a = -0.78539816f;  // -45 degrees
  float x = (p.x - 0.42f) * length, y = p.y * length;
  return {cx + x * std::cos(a) - y * std::sin(a), cy + x * std::sin(a) + y * std::cos(a)};
}

ComPtr<ID2D1PathGeometry> Geometry(const std::vector<D2D1_POINT_2F>& pts, float cx, float cy, float length) {
  ComPtr<ID2D1PathGeometry> geo;
  D2D()->CreatePathGeometry(&geo);
  ComPtr<ID2D1GeometrySink> sink;
  geo->Open(&sink);
  sink->BeginFigure(Place(pts[0], cx, cy, length), D2D1_FIGURE_BEGIN_FILLED);
  for (size_t i = 1; i < pts.size(); ++i) sink->AddLine(Place(pts[i], cx, cy, length));
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);
  sink->Close();
  return geo;
}

}  // namespace

void DrawFeather(Canvas& c, float cx, float cy, float length, D2D1_COLOR_F color) {
  static const auto outline = FeatherOutline(160);
  auto geo = Geometry(outline, cx, cy, length);
  c.DC()->FillGeometry(geo.Get(), c.Brush(color));
  // Stem below the vane.
  D2D1_POINT_2F a = Place({-0.16f, 0}, cx, cy, length), b = Place({0.02f, 0}, cx, cy, length);
  c.DC()->DrawLine(a, b, c.Brush(color), std::max(1.0f, length * 0.035f));
}

void Draw(Canvas& c, float x, float y, float size) {
  ID2D1DeviceContext* dc = c.DC();
  D2D1_RECT_F tile = RectF(x + size * 0.06f, y + size * 0.06f, x + size * 0.94f, y + size * 0.94f);
  ComPtr<ID2D1GradientStopCollection> stops;
  D2D1_GRADIENT_STOP s[2] = {{0.f, Rgb(0x637DFF)}, {1.f, Rgb(0x3E48D6)}};
  dc->CreateGradientStopCollection(s, 2, &stops);
  ComPtr<ID2D1LinearGradientBrush> grad;
  dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({x, tile.top}, {x, tile.bottom}), stops.Get(),
                                &grad);
  float r = size * 0.20f;
  dc->FillRoundedRectangle(D2D1::RoundedRect(tile, r, r), grad.Get());

  static const auto outline = FeatherOutline(160);
  const float cx = x + size / 2, cy = y + size / 2, length = size * 0.80f;
  auto geo = Geometry(outline, cx, cy, length);
  dc->FillGeometry(geo.Get(), c.Brush(Rgb(0xFFFFFF)));
  float w = std::max(1.0f, size * 0.028f);
  dc->DrawLine(Place({-0.16f, 0}, cx, cy, length), Place({0.0f, 0}, cx, cy, length), c.Brush(Rgb(0xFFFFFF)), w);
  dc->DrawLine(Place({0.0f, 0}, cx, cy, length), Place({0.70f, 0}, cx, cy, length), c.Brush(Rgb(0x3E48D6)), w);
}

}  // namespace logo
