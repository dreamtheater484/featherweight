#include "draw/Painter.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>

namespace draw {

Matrix Matrix::Rotate(float degrees) {
  const double rad = degrees * 3.14159265358979323846 / 180.0;
  const float c = static_cast<float>(std::cos(rad)), s = static_cast<float>(std::sin(rad));
  return {c, s, -s, c, 0, 0};
}

Matrix Matrix::operator*(const Matrix& o) const {
  return {m11 * o.m11 + m12 * o.m21,       m11 * o.m12 + m12 * o.m22,       m21 * o.m11 + m22 * o.m21,
          m21 * o.m12 + m22 * o.m22,       dx * o.m11 + dy * o.m21 + o.dx, dx * o.m12 + dy * o.m22 + o.dy};
}

void Path::MoveTo(Point p) { segments_.push_back({Kind::Move, {p}}); }
void Path::LineTo(Point p) { segments_.push_back({Kind::Line, {p}}); }
void Path::QuadTo(Point control, Point end) { segments_.push_back({Kind::Quad, {control, end}}); }
void Path::CubicTo(Point c1, Point c2, Point end) { segments_.push_back({Kind::Cubic, {c1, c2, end}}); }

void Path::ArcTo(Point end, float rx, float ry, float rotation, bool largeArc, bool clockwise) {
  Segment s{Kind::Arc, {end}};
  s.rx = rx;
  s.ry = ry;
  s.rotation = rotation;
  s.largeArc = largeArc;
  s.clockwise = clockwise;
  segments_.push_back(s);
}

void Path::Close() { segments_.push_back({Kind::Close}); }

Path Path::FromRect(const Rect& r) {
  Path p;
  p.MoveTo({r.left, r.top});
  p.LineTo({r.right, r.top});
  p.LineTo({r.right, r.bottom});
  p.LineTo({r.left, r.bottom});
  p.Close();
  return p;
}

Path Path::RoundedRect(const Rect& r, float rx, float ry) {
  rx = std::clamp(rx, 0.f, std::abs(r.Width()) / 2);
  ry = std::clamp(ry, 0.f, std::abs(r.Height()) / 2);
  if (rx == 0 || ry == 0) return FromRect(r);
  Path p;
  p.MoveTo({r.left + rx, r.top});
  p.LineTo({r.right - rx, r.top});
  p.ArcTo({r.right, r.top + ry}, rx, ry, 0, false, true);
  p.LineTo({r.right, r.bottom - ry});
  p.ArcTo({r.right - rx, r.bottom}, rx, ry, 0, false, true);
  p.LineTo({r.left + rx, r.bottom});
  p.ArcTo({r.left, r.bottom - ry}, rx, ry, 0, false, true);
  p.LineTo({r.left, r.top + ry});
  p.ArcTo({r.left + rx, r.top}, rx, ry, 0, false, true);
  p.Close();
  return p;
}

Path Path::Ellipse(const Rect& b) {
  const float rx = b.Width() / 2, ry = b.Height() / 2, cy = b.top + ry;
  Path p;
  p.MoveTo({b.left, cy});
  p.ArcTo({b.right, cy}, rx, ry, 0, false, true);
  p.ArcTo({b.left, cy}, rx, ry, 0, false, true);
  p.Close();
  return p;
}

Brush Brush::Solid(Color c) {
  Brush b;
  b.color = c;
  return b;
}

Brush Brush::Linear(Point start, Point end, std::vector<GradientStop> stops) {
  Brush b;
  b.kind = Kind::Linear;
  b.start = start;
  b.end = end;
  b.stops = std::move(stops);
  return b;
}

Brush Brush::Radial(Point center, float rx, float ry, std::vector<GradientStop> stops) {
  Brush b;
  b.kind = Kind::Radial;
  b.center = center;
  b.rx = rx;
  b.ry = ry;
  b.stops = std::move(stops);
  return b;
}

Bitmap::Bitmap(int width, int height, std::vector<uint8_t> pixels)
    : width_(width), height_(height), pixels_(std::move(pixels)) {
  static std::atomic<uint64_t> next{1};
  id_ = next++;
  pixels_.resize(static_cast<size_t>(std::max(0, width_)) * std::max(0, height_) * 4);
}

}  // namespace draw
