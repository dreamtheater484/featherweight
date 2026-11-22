#include "slide/objects/Frame.h"

#include <algorithm>
#include <cmath>

namespace fw::slide::objects {

namespace {

constexpr float kPi = 3.14159265358979f;

draw::Point Rotated(draw::Point p, float degrees) {
  const float a = degrees * kPi / 180, c = std::cos(a), s = std::sin(a);
  return {p.x * c - p.y * s, p.x * s + p.y * c};
}

float Distance(draw::Point a, draw::Point b) { return std::hypot(a.x - b.x, a.y - b.y); }

float SegmentDistance(draw::Point p, draw::Point a, draw::Point b) {
  const float dx = b.x - a.x, dy = b.y - a.y, len = dx * dx + dy * dy;
  if (len <= 0) return Distance(p, a);
  const float t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len, 0.f, 1.f);
  return Distance(p, {a.x + t * dx, a.y + t * dy});
}

// The handle's side along each axis: -1 left/top, 0 middle, 1 right/bottom.
void Sides(Handle h, int& sx, int& sy) {
  static constexpr int kX[] = {-1, 0, 1, 1, 1, 0, -1, -1}, kY[] = {-1, -1, -1, 0, 1, 1, 1, 0};
  const int i = static_cast<int>(h);
  sx = IsSizing(h) ? kX[i] : 0;
  sy = IsSizing(h) ? kY[i] : 0;
}

// A point of the unturned box (fractions across it) turned about the centre.
draw::Point BoxPoint(const Frame& f, float fx, float fy) {
  const draw::Point c = f.Centre();
  const draw::Point local{f.rect.left + fx * f.rect.Width() - c.x, f.rect.top + fy * f.rect.Height() - c.y};
  const draw::Point r = Rotated(local, f.rotation);
  return {c.x + r.x, c.y + r.y};
}

}  // namespace

float NormalizeDegrees(float degrees) {
  float d = std::fmod(degrees, 360.f);
  if (d < 0) d += 360;
  if (d >= 360 || d > 359.9995f) d = 0;
  return d;
}

std::array<draw::Point, 4> Frame::Corners() const {
  return {BoxPoint(*this, 0, 0), BoxPoint(*this, 1, 0), BoxPoint(*this, 1, 1), BoxPoint(*this, 0, 1)};
}

draw::Rect Frame::Bounds() const {
  if (rotation == 0) return rect;
  const auto c = Corners();
  draw::Rect b{c[0].x, c[0].y, c[0].x, c[0].y};
  for (const draw::Point& p : c) {
    b.left = std::min(b.left, p.x);
    b.top = std::min(b.top, p.y);
    b.right = std::max(b.right, p.x);
    b.bottom = std::max(b.bottom, p.y);
  }
  return b;
}

draw::Matrix Frame::Transform() const {
  const float w = rect.Width(), h = rect.Height();
  const draw::Point c = Centre();
  return draw::Matrix::Translate(-w / 2, -h / 2) * draw::Matrix::Scale(flipH ? -1.f : 1.f, flipV ? -1.f : 1.f) *
         draw::Matrix::Rotate(rotation) * draw::Matrix::Translate(c.x, c.y);
}

std::array<draw::Point, 2> Frame::LineEnds() const {
  const draw::Matrix m = Transform();
  return {m.Apply({0, 0}), m.Apply({rect.Width(), rect.Height()})};
}

Frame Frame::FromLine(draw::Point from, draw::Point to) {
  Frame f;
  f.rect = Normalized(from, to);
  f.flipH = from.x > to.x;
  f.flipV = from.y > to.y;
  return f;
}

draw::Point HandlePoint(const Frame& f, Handle h, float rotateOffset) {
  switch (h) {
    case Handle::Start: return f.LineEnds()[0];
    case Handle::End: return f.LineEnds()[1];
    case Handle::Rotate: {
      const draw::Point c = f.Centre();
      const draw::Point r = Rotated({0, -f.rect.Height() / 2 - rotateOffset}, f.rotation);
      return {c.x + r.x, c.y + r.y};
    }
    case Handle::Body: return f.Centre();
    default: break;
  }
  if (!IsSizing(h)) return f.Centre();
  int sx = 0, sy = 0;
  Sides(h, sx, sy);
  return BoxPoint(f, (sx + 1) / 2.f, (sy + 1) / 2.f);
}

bool HitFrame(const Frame& f, bool line, draw::Point p, float tolerance) {
  if (line) {
    const auto e = f.LineEnds();
    return SegmentDistance(p, e[0], e[1]) <= tolerance;
  }
  // Into the unturned box's coordinates.
  const draw::Point c = f.Centre();
  const draw::Point q = Rotated({p.x - c.x, p.y - c.y}, -f.rotation);
  const float hw = f.rect.Width() / 2, hh = f.rect.Height() / 2;
  // Thin shapes get the tolerance, so a flat rectangle can still be clicked.
  const float tx = hw * 2 < tolerance * 2 ? tolerance : 0, ty = hh * 2 < tolerance * 2 ? tolerance : 0;
  return std::fabs(q.x) <= hw + tx && std::fabs(q.y) <= hh + ty;
}

Handle HitHandle(const Frame& f, const HandleSet& set, draw::Point p, float tolerance, float rotateOffset) {
  if (set.line) {
    if (Distance(p, HandlePoint(f, Handle::End, 0)) <= tolerance) return Handle::End;
    if (Distance(p, HandlePoint(f, Handle::Start, 0)) <= tolerance) return Handle::Start;
    return HitFrame(f, true, p, tolerance) ? Handle::Body : Handle::None;
  }
  if (set.rotate && Distance(p, HandlePoint(f, Handle::Rotate, rotateOffset)) <= tolerance) return Handle::Rotate;
  // Corners before edges: on a small object they crowd together.
  for (Handle h : {Handle::TopLeft, Handle::TopRight, Handle::BottomRight, Handle::BottomLeft, Handle::Top,
                   Handle::Right, Handle::Bottom, Handle::Left}) {
    if (Distance(p, HandlePoint(f, h, rotateOffset)) <= tolerance) return h;
  }
  return HitFrame(f, false, p, 0) ? Handle::Body : Handle::None;
}

Frame Resize(const Frame& start, Handle h, draw::Point from, draw::Point to, const ResizeOptions& o) {
  if (!IsSizing(h)) return start;
  int sx = 0, sy = 0;
  Sides(h, sx, sy);
  const float w0 = start.rect.Width(), h0 = start.rect.Height();
  float l = -w0 / 2, r = w0 / 2, t = -h0 / 2, b = h0 / 2;
  const draw::Point d = Rotated({to.x - from.x, to.y - from.y}, -start.rotation);
  if (sx > 0) {
    r += d.x;
    if (o.fromCentre) l -= d.x;
  } else if (sx < 0) {
    l += d.x;
    if (o.fromCentre) r -= d.x;
  }
  if (sy > 0) {
    b += d.y;
    if (o.fromCentre) t -= d.y;
  } else if (sy < 0) {
    t += d.y;
    if (o.fromCentre) b -= d.y;
  }
  if (o.keepAspect && w0 > 0 && h0 > 0) {
    float w = r - l, ht = b - t;
    if (sx != 0 && sy != 0) {
      const float s = std::max(std::fabs(w / w0), std::fabs(ht / h0));
      w = (w < 0 ? -1 : 1) * s * w0;
      ht = (ht < 0 ? -1 : 1) * s * h0;
      if (o.fromCentre) {
        l = -w / 2, r = w / 2, t = -ht / 2, b = ht / 2;
      } else {
        if (sx > 0) r = l + w; else l = r - w;
        if (sy > 0) b = t + ht; else t = b - ht;
      }
    } else if (sx != 0) {  // an edge: the other side grows about the middle
      const float nh = std::fabs(w / w0) * h0;
      t = -nh / 2, b = nh / 2;
    } else {
      const float nw = std::fabs(ht / h0) * w0;
      l = -nw / 2, r = nw / 2;
    }
  }
  Frame f = start;
  if (r < l) {
    std::swap(l, r);
    f.flipH = !f.flipH;
  }
  if (b < t) {
    std::swap(t, b);
    f.flipV = !f.flipV;
  }
  if (r - l < o.minSize) {
    if (sx < 0) l = r - o.minSize; else r = l + o.minSize;
  }
  if (b - t < o.minSize) {
    if (sy < 0) t = b - o.minSize; else b = t + o.minSize;
  }
  const draw::Point c0 = start.Centre();
  const draw::Point mid = Rotated({(l + r) / 2, (t + b) / 2}, start.rotation);
  const draw::Point c{c0.x + mid.x, c0.y + mid.y};
  const float w = r - l, ht = b - t;
  f.rect = {c.x - w / 2, c.y - ht / 2, c.x + w / 2, c.y + ht / 2};
  return f;
}

Frame MoveLineEnd(const Frame& start, Handle end, draw::Point to, bool snap45) {
  auto e = start.LineEnds();
  const int moving = end == Handle::Start ? 0 : 1;
  const draw::Point fixed = e[1 - moving];
  if (snap45) {
    const float dx = to.x - fixed.x, dy = to.y - fixed.y, len = std::hypot(dx, dy);
    const float a = std::round(std::atan2(dy, dx) / (kPi / 4)) * (kPi / 4);
    to = {fixed.x + len * std::cos(a), fixed.y + len * std::sin(a)};
    if (std::fabs(to.x - fixed.x) < 0.001f) to.x = fixed.x;
    if (std::fabs(to.y - fixed.y) < 0.001f) to.y = fixed.y;
  }
  e[moving] = to;
  return Frame::FromLine(e[0], e[1]);
}

Frame Rotate(const Frame& start, draw::Point from, draw::Point to, bool snap15) {
  const draw::Point c = start.Centre();
  const float a0 = std::atan2(from.y - c.y, from.x - c.x), a1 = std::atan2(to.y - c.y, to.x - c.x);
  float deg = start.rotation + (a1 - a0) * 180 / kPi;
  if (snap15) deg = std::round(deg / 15) * 15;
  Frame f = start;
  f.rotation = NormalizeDegrees(deg);
  return f;
}

draw::Rect Union(const draw::Rect& a, const draw::Rect& b) {
  return {std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}

draw::Rect Normalized(draw::Point a, draw::Point b) {
  return {std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)};
}

bool Contains(const draw::Rect& outer, const draw::Rect& inner) {
  return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right && inner.bottom <= outer.bottom;
}

bool Intersects(const draw::Rect& a, const draw::Rect& b) {
  return a.left <= b.right && b.left <= a.right && a.top <= b.bottom && b.top <= a.bottom;
}

draw::Rect Offset(const draw::Rect& r, float dx, float dy) { return {r.left + dx, r.top + dy, r.right + dx, r.bottom + dy}; }

}  // namespace fw::slide::objects
