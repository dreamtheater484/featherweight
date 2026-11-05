#include "draw/Painter.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>

#include "draw/Images.h"

namespace draw {
namespace {

constexpr double kPi = 3.14159265358979323846;

uint64_t NextId() {
  static std::atomic<uint64_t> next{1};
  return next++;
}

// Points along an elliptical arc from `from`, in SVG's endpoint form (SVG 1.1, F.6.5), excluding
// `from` itself.
void FlattenArc(Point from, const Path::Segment& s, float tolerance, std::vector<Point>& out) {
  const Point to = s.p[0];
  double rx = std::abs(s.rx), ry = std::abs(s.ry);
  if (rx < 1e-6 || ry < 1e-6 || (from.x == to.x && from.y == to.y)) {
    out.push_back(to);
    return;
  }
  const double phi = s.rotation * kPi / 180, c = std::cos(phi), sn = std::sin(phi);
  const double dx2 = (from.x - to.x) / 2.0, dy2 = (from.y - to.y) / 2.0;
  const double x1 = c * dx2 + sn * dy2, y1 = -sn * dx2 + c * dy2;
  const double lambda = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
  if (lambda > 1) {
    rx *= std::sqrt(lambda);
    ry *= std::sqrt(lambda);
  }
  const double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
  const double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
  double coef = den > 0 ? std::sqrt(std::max(0.0, num / den)) : 0;
  if (s.largeArc == s.clockwise) coef = -coef;
  const double cxp = coef * rx * y1 / ry, cyp = -coef * ry * x1 / rx;
  const double cx = c * cxp - sn * cyp + (from.x + to.x) / 2.0, cy = sn * cxp + c * cyp + (from.y + to.y) / 2.0;
  const double t1 = std::atan2((y1 - cyp) / ry, (x1 - cxp) / rx);
  double dt = std::atan2((-y1 - cyp) / ry, (-x1 - cxp) / rx) - t1;
  if (s.clockwise && dt < 0) dt += 2 * kPi;
  if (!s.clockwise && dt > 0) dt -= 2 * kPi;
  const double r = std::max(rx, ry), tol = std::max(1e-3, static_cast<double>(tolerance));
  const double step = tol >= r ? kPi / 2 : 2 * std::acos(1 - tol / r);
  const int n = std::clamp(static_cast<int>(std::ceil(std::abs(dt) / std::max(step, 1e-3))), 1, 256);
  for (int i = 1; i < n; ++i) {
    const double t = t1 + dt * i / n, ex = rx * std::cos(t), ey = ry * std::sin(t);
    out.push_back({static_cast<float>(cx + c * ex - sn * ey), static_cast<float>(cy + sn * ex + c * ey)});
  }
  out.push_back(to);
}

int CurveSteps(std::initializer_list<Point> points, float tolerance) {
  double length = 0;
  const Point* prev = nullptr;
  for (const Point& p : points) {
    if (prev) length += std::hypot(p.x - prev->x, p.y - prev->y);
    prev = &p;
  }
  return std::clamp(static_cast<int>(std::ceil(std::sqrt(length / std::max(1e-3f, tolerance)))), 1, 128);
}

}  // namespace

Matrix Matrix::Rotate(float degrees) {
  const double rad = degrees * 3.14159265358979323846 / 180.0;
  const float c = static_cast<float>(std::cos(rad)), s = static_cast<float>(std::sin(rad));
  return {c, s, -s, c, 0, 0};
}

Matrix Matrix::Inverse() const {
  const double det = static_cast<double>(m11) * m22 - static_cast<double>(m12) * m21;
  if (std::abs(det) < 1e-12) return {};
  const double i = 1 / det;
  return {static_cast<float>(m22 * i), static_cast<float>(-m12 * i), static_cast<float>(-m21 * i),
          static_cast<float>(m11 * i), static_cast<float>((m21 * dy - m22 * dx) * i),
          static_cast<float>((m12 * dx - m11 * dy) * i)};
}

float Matrix::ScaleX() const { return std::hypot(m11, m12); }
float Matrix::ScaleY() const { return std::hypot(m21, m22); }

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

std::vector<Path::Polygon> Path::Flatten(float tolerance) const {
  std::vector<Polygon> out;
  Point last{}, figureStart{};
  bool open = false;
  auto begin = [&](Point at) {
    out.push_back({{at}, false});
    open = true;
    figureStart = last = at;
  };
  for (const auto& s : segments_) {
    if (s.kind == Kind::Move) {
      begin(s.p[0]);
      continue;
    }
    if (s.kind == Kind::Close) {
      if (open) out.back().closed = true;
      open = false;
      last = figureStart;
      continue;
    }
    if (!open) begin(last);
    std::vector<Point>& pts = out.back().points;
    switch (s.kind) {
      case Kind::Line:
        pts.push_back(s.p[0]);
        break;
      case Kind::Quad: {
        const int n = CurveSteps({last, s.p[0], s.p[1]}, tolerance);
        for (int i = 1; i <= n; ++i) {
          const float t = static_cast<float>(i) / n, u = 1 - t;
          pts.push_back({u * u * last.x + 2 * u * t * s.p[0].x + t * t * s.p[1].x,
                         u * u * last.y + 2 * u * t * s.p[0].y + t * t * s.p[1].y});
        }
        break;
      }
      case Kind::Cubic: {
        const int n = CurveSteps({last, s.p[0], s.p[1], s.p[2]}, tolerance);
        for (int i = 1; i <= n; ++i) {
          const float t = static_cast<float>(i) / n, u = 1 - t;
          const float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
          pts.push_back({a * last.x + b * s.p[0].x + c * s.p[1].x + d * s.p[2].x,
                         a * last.y + b * s.p[0].y + c * s.p[1].y + d * s.p[2].y});
        }
        break;
      }
      case Kind::Arc:
        FlattenArc(last, s, tolerance, pts);
        break;
      default:
        break;
    }
    last = pts.back();
  }
  return out;
}

Rect Path::Bounds() const {
  Rect r{};
  bool any = false;
  for (const Polygon& poly : Flatten(0.25f)) {
    for (const Point& p : poly.points) {
      if (!any) r = {p.x, p.y, p.x, p.y};
      r = {std::min(r.left, p.x), std::min(r.top, p.y), std::max(r.right, p.x), std::max(r.bottom, p.y)};
      any = true;
    }
  }
  return r;
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

Brush Brush::FromBitmap(std::shared_ptr<const draw::Bitmap> bitmap, const Matrix& transform, Extend extend) {
  Brush b;
  b.kind = Kind::Bitmap;
  b.bitmap = std::move(bitmap);
  b.bitmapTransform = transform;
  b.extendX = b.extendY = extend;
  return b;
}

Bitmap::Bitmap(int width, int height, std::vector<uint8_t> pixels)
    : width_(width), height_(height), pixels_(std::move(pixels)), id_(NextId()) {
  pixels_.resize(static_cast<size_t>(std::max(0, width_)) * std::max(0, height_) * 4);
}

VectorImage::VectorImage(Kind kind, std::vector<uint8_t> data, Size size)
    : kind_(kind), data_(std::move(data)), size_(size), id_(NextId()) {}

void Painter::DrawVectorImage(const VectorImage& image, const Rect& dest, float opacity) {
  const Matrix m = CurrentTransform();
  const int w = std::clamp(static_cast<int>(std::ceil(std::abs(dest.Width()) * m.ScaleX())), 1, 4096);
  const int h = std::clamp(static_cast<int>(std::ceil(std::abs(dest.Height()) * m.ScaleY())), 1, 4096);
  if (std::shared_ptr<const Bitmap> pixels = RasterizeVectorImage(image, w, h)) DrawBitmap(*pixels, dest, nullptr, opacity);
}

void Painter::BeginShadow(const Shadow&) { Save(); }

}  // namespace draw
