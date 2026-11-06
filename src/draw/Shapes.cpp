#include "draw/Shapes.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace draw {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRad = kPi / 180;

// The shape's own coordinates → its parent's, for the shape placed at `bounds`.
Matrix Place(const Shape& s, const Rect& bounds) {
  const float w = bounds.Width(), h = bounds.Height();
  return Matrix::Translate(-w / 2, -h / 2) * Matrix::Scale(s.flipH ? -1.f : 1.f, s.flipV ? -1.f : 1.f) *
         Matrix::Rotate(s.rotation) * Matrix::Translate(bounds.left + w / 2, bounds.top + h / 2);
}

Rect Inset(const Rect& r, const Rect& f) {
  const float w = r.Width(), h = r.Height();
  return {r.left + f.left * w, r.top + f.top * h, r.right - f.right * w, r.bottom - f.bottom * h};
}

Point AlignPoint(const Rect& r, uint8_t align) {
  const float xs[] = {r.left, (r.left + r.right) / 2, r.right};
  const float ys[] = {r.top, (r.top + r.bottom) / 2, r.bottom};
  return {xs[std::min<int>(align, 8) % 3], ys[std::min<int>(align, 8) / 3]};
}

// The lighter and darker fills some preset paths ask for (the cube's sides, the can's top).
Color Shade(Color c, Geometry::PathFill mode) {
  switch (mode) {
    case Geometry::PathFill::Darken: return {c.r * 0.6f, c.g * 0.6f, c.b * 0.6f, c.a};
    case Geometry::PathFill::DarkenLess: return {c.r * 0.8f, c.g * 0.8f, c.b * 0.8f, c.a};
    case Geometry::PathFill::Lighten:
      return {c.r + (1 - c.r) * 0.4f, c.g + (1 - c.g) * 0.4f, c.b + (1 - c.b) * 0.4f, c.a};
    case Geometry::PathFill::LightenLess:
      return {c.r + (1 - c.r) * 0.2f, c.g + (1 - c.g) * 0.2f, c.b + (1 - c.b) * 0.2f, c.a};
    default: return c;
  }
}

// A gradient's stops as Word draws them (measured in its PDFs): two stops blend through linear
// light, eased in and out along a sin² curve, so they become sixteen here; more stops are plain
// sRGB ramps.
std::vector<GradientStop> GradientStops(const Fill& f, Geometry::PathFill mode) {
  std::vector<GradientStop> out = f.stops;
  for (GradientStop& s : out) s.color = Shade(s.color, mode);
  if (out.size() != 2) return out;
  const GradientStop a = out[0], b = out[1];
  auto linear = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
  auto srgb = [](float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1 / 2.4f) - 0.055f; };
  constexpr int kSteps = 16;
  out.clear();
  for (int i = 0; i <= kSteps; ++i) {
    const float u = static_cast<float>(i) / kSteps, e = std::sin(kPi / 2 * u), t = e * e;
    auto mix = [&](float x, float y) { return srgb(linear(x) + (linear(y) - linear(x)) * t); };
    out.push_back({a.offset + (b.offset - a.offset) * u,
                   {mix(a.color.r, b.color.r), mix(a.color.g, b.color.g), mix(a.color.b, b.color.b),
                    a.color.a + (b.color.a - a.color.a) * t}});
  }
  return out;
}

// A linear gradient across `box` (DrawingML's a:lin): with `scaled`, the angle is for a square
// stretched to the box, so 45° runs corner to corner.
Brush LinearBrush(const Fill& f, const Rect& box, float shapeRotation, std::vector<GradientStop> stops) {
  float angle = f.angle;
  if (!f.rotateWithShape) angle -= shapeRotation;
  const float a = angle * kRad, c = std::cos(a), s = std::sin(a);
  const float w = std::max(box.Width(), 1e-3f), h = std::max(box.Height(), 1e-3f);
  const Point center{(box.left + box.right) / 2, (box.top + box.bottom) / 2};
  Point d;
  if (f.scaled) {
    // The gradient's slope in box coordinates, from the square's.
    const float l = (std::abs(c) + std::abs(s)) / 2;
    const float gx = c / w / (2 * l), gy = s / h / (2 * l), g2 = gx * gx + gy * gy;
    d = {gx / g2 / 2, gy / g2 / 2};
  } else {
    const float l = (w * std::abs(c) + h * std::abs(s)) / 2;
    d = {c * l, s * l};
  }
  return Brush::Linear({center.x - d.x, center.y - d.y}, {center.x + d.x, center.y + d.y}, std::move(stops));
}

// Device pixels per unit of the current coordinates.
float DeviceScale(const Painter& p) {
  const Matrix m = p.CurrentTransform();
  return std::max(1e-3f, std::max(m.ScaleX(), m.ScaleY()));
}

// A rectangular path gradient: the first stop at a focus point, the last at the tile's edges
// (normally the box's), so lines of equal color are rectangles. Word reduces a:fillToRect to a
// point, its insets in proportion: l / (l + r) across (measured in its PDFs).
void FillRectGradient(Painter& p, const Rect& box, const Fill& f, const std::vector<GradientStop>& stops) {
  const Rect B = Inset(box, f.tile);
  auto share = [](float a, float b) { return a + b > 1e-6f ? std::clamp(a / (a + b), 0.f, 1.f) : 0.5f; };
  const Point c{B.left + B.Width() * share(f.focus.left, f.focus.right),
                B.top + B.Height() * share(f.focus.top, f.focus.bottom)};
  if (f.tile.left > 0 || f.tile.top > 0 || f.tile.right > 0 || f.tile.bottom > 0) {
    p.FillRect(box, Brush::Solid(stops.back().color));  // past a smaller tile
  }
  // Four pieces, each a linear gradient from the focus to one side. The left and right ones
  // reach a device pixel past the diagonals, where the colors agree, so antialiasing leaves no
  // seams.
  const float m = 1 / DeviceScale(p);
  auto piece = [&](std::initializer_list<Point> points, Point to) {
    if (std::abs(c.x - to.x) < 1e-4f && std::abs(c.y - to.y) < 1e-4f) return;
    Path q;
    for (const Point& pt : points) {
      if (q.Empty()) q.MoveTo(pt);
      else q.LineTo(pt);
    }
    q.Close();
    p.FillPath(q, Brush::Linear(c, to, stops));
  };
  piece({{B.left, B.top}, {B.right, B.top}, c}, {c.x, B.top});
  piece({{B.left, B.bottom}, {B.right, B.bottom}, c}, {c.x, B.bottom});
  piece({{B.left, B.top - m}, {c.x + m, c.y - m}, {c.x + m, c.y + m}, {B.left, B.bottom + m}}, {B.left, c.y});
  piece({{B.right, B.top - m}, {c.x - m, c.y - m}, {c.x - m, c.y + m}, {B.right, B.bottom + m}}, {B.right, c.y});
}

// A picture into `area` (the shape's box): inset by the stretch rectangle, cropped by srcRect.
void DrawPicture(Painter& p, const PictureFill& pic, const Rect& area) {
  if (!pic.image) return;
  const Rect inner = Inset(area, pic.inset);
  const float fw = 1 - pic.crop.left - pic.crop.right, fh = 1 - pic.crop.top - pic.crop.bottom;
  if (fw <= 1e-4f || fh <= 1e-4f || inner.Width() == 0 || inner.Height() == 0) return;
  // Where the whole, uncropped picture lands.
  const float W = inner.Width() / fw, H = inner.Height() / fh;
  const Rect full{inner.left - pic.crop.left * W, inner.top - pic.crop.top * H, 0, 0};
  const Rect whole{full.left, full.top, full.left + W, full.top + H};
  const std::shared_ptr<const VectorImage>& vector = pic.image->Vector();
  if (vector && !pic.Recolors()) {
    p.Save();
    p.ClipRect(inner);
    p.DrawVectorImage(*vector, whole, pic.alpha);
    p.Restore();
    return;
  }
  const Matrix m = p.CurrentTransform();
  const int pw = static_cast<int>(std::ceil(std::abs(W) * m.ScaleX())), ph = static_cast<int>(std::ceil(std::abs(H) * m.ScaleY()));
  std::shared_ptr<const Bitmap> px = pic.Pixels(std::clamp(pw, 1, 16384), std::clamp(ph, 1, 16384));
  if (!px || px->Width() == 0 || px->Height() == 0) return;
  // Only the visible part of the bitmap; negative crops (padding) leave the rest empty.
  const float l = std::max(0.f, pic.crop.left), t = std::max(0.f, pic.crop.top);
  const float r = 1 - std::max(0.f, pic.crop.right), b = 1 - std::max(0.f, pic.crop.bottom);
  if (r <= l || b <= t) return;
  const Rect src{l * px->Width(), t * px->Height(), r * px->Width(), b * px->Height()};
  const Rect dest{whole.left + l * W, whole.top + t * H, whole.left + r * W, whole.top + b * H};
  p.DrawBitmap(*px, dest, &src, pic.alpha);
}

// A tiled picture as a brush: repeated at its natural size times the tile scale, from the
// alignment point plus the offset.
bool TileBrush(Painter& p, const PictureFill& pic, const Rect& box, Brush& out) {
  if (!pic.image) return false;
  const Size n = pic.image->NaturalSize();
  const float tw = n.width * pic.tileScaleX, th = n.height * pic.tileScaleY;
  if (tw <= 0.01f || th <= 0.01f) return false;
  const Matrix m = p.CurrentTransform();
  std::shared_ptr<const Bitmap> px =
      pic.Pixels(std::clamp(static_cast<int>(std::ceil(tw * m.ScaleX())), 1, 4096),
                 std::clamp(static_cast<int>(std::ceil(th * m.ScaleY())), 1, 4096));
  if (!px) return false;
  const Point origin = AlignPoint(box, pic.tileAlign);
  out = Brush::FromBitmap(px,
                          Matrix::Scale(tw / px->Width(), th / px->Height()) *
                              Matrix::Translate(origin.x + pic.tileX, origin.y + pic.tileY),
                          Brush::Extend::Repeat);
  if (pic.mirrorX) out.extendX = Brush::Extend::Mirror;
  if (pic.mirrorY) out.extendY = Brush::Extend::Mirror;
  return true;
}

Brush PatternBrush(const Fill& f, Geometry::PathFill mode) {
  if (!f.patternBitmap) f.patternBitmap = PatternBitmap(f.pattern, f.color, f.background);
  std::shared_ptr<const Bitmap> bmp = f.patternBitmap;
  if (mode != Geometry::PathFill::Normal) bmp = PatternBitmap(f.pattern, Shade(f.color, mode), Shade(f.background, mode));
  // 8 x 8 cells of 0.75 pt: one pixel each at 96 dpi and 100 %.
  Brush b = Brush::FromBitmap(bmp, Matrix::Scale(0.75f, 0.75f), Brush::Extend::Repeat);
  b.smooth = false;
  return b;
}

void FillArea(Painter& p, const Path& path, const Fill& f, const Rect& box, float rotation, Geometry::PathFill mode) {
  switch (f.kind) {
    case Fill::Kind::None:
      return;
    case Fill::Kind::Solid:
      p.FillPath(path, Brush::Solid(Shade(f.color, mode)));
      return;
    case Fill::Kind::Pattern:
      p.FillPath(path, PatternBrush(f, mode));
      return;
    case Fill::Kind::Gradient: {
      if (f.stops.empty()) return;
      std::vector<GradientStop> stops = GradientStops(f, mode);
      if (f.shape == Fill::GradientShape::Linear) {
        p.FillPath(path, LinearBrush(f, box, rotation, std::move(stops)));
      } else if (f.shape == Fill::GradientShape::Circle) {
        // Word centers a circular gradient on the tile whatever a:fillToRect says (PowerPoint may
        // not) and reaches the last stop at the tile's corners.
        const Rect T = Inset(box, f.tile);
        const Point c{(T.left + T.right) / 2, (T.top + T.bottom) / 2};
        const float r = std::max(std::hypot(T.Width(), T.Height()) / 2, 1e-3f);
        p.FillPath(path, Brush::Radial(c, r, r, std::move(stops)));
      } else {
        p.Save();
        p.ClipPath(path);
        FillRectGradient(p, box, f, stops);
        p.Restore();
      }
      return;
    }
    case Fill::Kind::Picture: {
      if (!f.picture) return;
      if (f.picture->tile) {
        Brush b;
        if (TileBrush(p, *f.picture, box, b)) p.FillPath(path, b);
        return;
      }
      p.Save();
      p.ClipPath(path);
      DrawPicture(p, *f.picture, box);
      p.Restore();
      return;
    }
  }
}

// A brush for a line: lines are nearly always solid; other fills use their main color.
Brush LineBrush(Painter& p, const Line& line, const Rect& box, float rotation) {
  const Fill& f = line.fill;
  switch (f.kind) {
    case Fill::Kind::Gradient:
      if (!f.stops.empty()) {
        if (f.shape == Fill::GradientShape::Linear) {
          return LinearBrush(f, box, rotation, GradientStops(f, Geometry::PathFill::Normal));
        }
        return Brush::Solid(f.stops.back().color);
      }
      break;
    case Fill::Kind::Pattern:
      return PatternBrush(f, Geometry::PathFill::Normal);
    case Fill::Kind::Picture: {
      Brush b;
      if (f.picture && TileBrush(p, *f.picture, box, b)) return b;
      return Brush::Solid({0.5f, 0.5f, 0.5f, 1});
    }
    default:
      break;
  }
  return Brush::Solid(f.color);
}

Pen MakePen(Painter& p, const Line& line, const Rect& box, float rotation) {
  Pen pen;
  pen.brush = LineBrush(p, line, box, rotation);
  // Width 0 is the thinnest line the device shows: one pixel.
  pen.width = line.width > 0 ? line.width : 1 / DeviceScale(p);
  pen.cap = line.cap;
  pen.join = line.join;
  pen.miterLimit = line.miterLimit;
  pen.dashes = line.dashes;
  return pen;
}

// An arrowhead with its tip at `tip`, pointing along `dir` (a unit vector).
void DrawArrowHead(Painter& p, const ArrowHead& a, const Line& line, const Pen& pen, Point tip, Point dir) {
  // Sizes in multiples of the line width (at least about 2 pt), as Office draws them.
  static constexpr float kFactor[] = {2, 3, 5};
  const float base = std::max(line.width, 2.f);
  const float w = base * kFactor[std::min<int>(a.width, 2)], len = base * kFactor[std::min<int>(a.length, 2)];
  p.Save();
  p.Transform(Matrix{dir.x, dir.y, -dir.y, dir.x, tip.x, tip.y});
  Path head;
  switch (a.type) {
    case ArrowHead::Type::Triangle:
      head.MoveTo({0, 0});
      head.LineTo({-len, -w / 2});
      head.LineTo({-len, w / 2});
      head.Close();
      p.FillPath(head, pen.brush);
      break;
    case ArrowHead::Type::Stealth:
      head.MoveTo({0, 0});
      head.LineTo({-len, -w / 2});
      head.LineTo({-len * 0.6f, 0});
      head.LineTo({-len, w / 2});
      head.Close();
      p.FillPath(head, pen.brush);
      break;
    case ArrowHead::Type::Diamond:
      head.MoveTo({len / 2, 0});
      head.LineTo({0, -w / 2});
      head.LineTo({-len / 2, 0});
      head.LineTo({0, w / 2});
      head.Close();
      p.FillPath(head, pen.brush);
      break;
    case ArrowHead::Type::Oval:
      p.FillPath(Path::Ellipse({-len / 2, -w / 2, len / 2, w / 2}), pen.brush);
      break;
    case ArrowHead::Type::Arrow: {
      head.MoveTo({-len, -w / 2});
      head.LineTo({0, 0});
      head.LineTo({-len, w / 2});
      Pen open = pen;
      open.dashes.clear();
      open.join = LineJoin::Miter;
      p.StrokePath(head, open);
      break;
    }
    default:
      break;
  }
  p.Restore();
}

// The tip and outward direction at one end of an open figure.
bool EndOf(const Path& path, bool atStart, Point& tip, Point& dir) {
  std::vector<Path::Polygon> polys = path.Flatten(0.05f);
  if (polys.empty() || polys.front().closed) return false;
  const std::vector<Point>& pts = (atStart ? polys.front() : polys.back()).points;
  if (pts.size() < 2) return false;
  const int n = static_cast<int>(pts.size());
  tip = atStart ? pts[0] : pts[n - 1];
  for (int i = 1; i < n; ++i) {
    const Point& q = atStart ? pts[i] : pts[n - 1 - i];
    const float dx = tip.x - q.x, dy = tip.y - q.y, len = std::hypot(dx, dy);
    if (len > 1e-4f) {
      dir = {dx / len, dy / len};
      return true;
    }
  }
  return false;
}

void StrokeFigures(Painter& p, const Shape& s, const std::vector<Geometry::Figure>& figures, const Rect& box) {
  const Line& line = s.line;
  if (line.fill.kind == Fill::Kind::None) return;
  const Pen pen = MakePen(p, line, box, s.rotation);
  const Path* first = nullptr;
  const Path* last = nullptr;
  for (const Geometry::Figure& fig : figures) {
    if (!fig.stroke || fig.path.Empty()) continue;
    if (!first) first = &fig.path;
    last = &fig.path;
    if (line.inset) {
      // Inside the outline: twice as wide, clipped to the shape.
      Pen inner = pen;
      inner.width *= 2;
      p.Save();
      p.ClipPath(fig.path);
      p.StrokePath(fig.path, inner);
      p.Restore();
    } else {
      p.StrokePath(fig.path, pen);
    }
  }
  Point tip, dir;
  if (line.head.type != ArrowHead::Type::None && first && EndOf(*first, true, tip, dir)) {
    DrawArrowHead(p, line.head, line, pen, tip, dir);
  }
  if (line.tail.type != ArrowHead::Type::None && last && EndOf(*last, false, tip, dir)) {
    DrawArrowHead(p, line.tail, line, pen, tip, dir);
  }
}

Geometry::Outline OutlineOf(const Shape& s, Size size) {
  if (s.geometry) return s.geometry->Evaluate(size, s.adjust);
  Geometry::Outline o;
  o.figures.push_back({Path::FromRect({0, 0, size.width, size.height})});
  o.textRect = {0, 0, size.width, size.height};
  return o;
}

void DrawTextPath(Painter& p, const Shape& s, const Rect& box) {
  const TextPath& tp = *s.textPath;
  Path glyphs = TextOutline(tp.text, tp.font, tp.bold, tp.italic, 36);
  const Rect b = glyphs.Bounds();
  if (glyphs.Empty() || b.Width() <= 0 || b.Height() <= 0) return;
  // Stretched to fill the shape, as Office's WordArt (fitshape) does.
  p.Save();
  p.Transform(Matrix::Translate(-b.left, -b.top) * Matrix::Scale(box.Width() / b.Width(), box.Height() / b.Height()) *
              Matrix::Translate(box.left, box.top));
  FillArea(p, glyphs, s.fill, b, s.rotation, Geometry::PathFill::Normal);
  if (s.line.fill.kind != Fill::Kind::None) {
    Pen pen = MakePen(p, s.line, b, s.rotation);
    pen.width /= std::max(1e-3f, std::min(box.Width() / b.Width(), box.Height() / b.Height()));
    p.StrokePath(glyphs, pen);
  }
  p.Restore();
}

void DrawPlaceholder(Painter& p, const Shape& s, const Rect& box) {
  p.FillRect(box, Brush::Solid({0.95f, 0.95f, 0.95f, 1}));
  Pen pen;
  pen.brush = Brush::Solid({0.75f, 0.75f, 0.75f, 1});
  pen.width = 0.75f;
  p.StrokePath(Path::FromRect({box.left + 0.375f, box.top + 0.375f, box.right - 0.375f, box.bottom - 0.375f}), pen);
  if (s.name.empty()) return;
  std::wstring name(s.name.begin(), s.name.end());
  Path label = TextOutline(name, L"Segoe UI", false, false, 9);
  const Rect b = label.Bounds();
  if (label.Empty() || b.Width() > box.Width() - 8 || b.Height() > box.Height() - 4) return;
  p.Save();
  p.Transform(Matrix::Translate((box.left + box.right - b.Width()) / 2 - b.left,
                                (box.top + box.bottom - b.Height()) / 2 - b.top));
  p.FillPath(label, Brush::Solid({0.45f, 0.45f, 0.45f, 1}));
  p.Restore();
}

Shadow ShadowFor(const OuterShadow& sh, const Rect& bounds) {
  Shadow out;
  out.color = sh.color;
  out.blur = sh.blur;
  const Point a = AlignPoint(bounds, sh.align);
  const float dx = sh.distance * std::cos(sh.direction * kRad), dy = sh.distance * std::sin(sh.direction * kRad);
  const Matrix skew{1, std::tan(sh.skewY * kRad), std::tan(sh.skewX * kRad), 1, 0, 0};
  out.transform = Matrix::Translate(-a.x, -a.y) * Matrix::Scale(sh.scaleX, sh.scaleY) * skew *
                  Matrix::Translate(a.x + dx, a.y + dy);
  return out;
}

void DrawShapeAt(Painter& p, const Shape& s, const Rect& bounds);

void DrawContent(Painter& p, const Shape& s, Size size) {
  const Rect box{0, 0, size.width, size.height};
  switch (s.kind) {
    case Shape::Kind::Group:
      for (const Shape& c : s.children) DrawShapeAt(p, c, ChildToGroup(s, c.bounds));
      return;
    case Shape::Kind::Placeholder:
      DrawPlaceholder(p, s, box);
      return;
    case Shape::Kind::Picture: {
      Geometry::Outline o = OutlineOf(s, size);
      for (const Geometry::Figure& f : o.figures) FillArea(p, f.path, s.fill, box, s.rotation, f.fill);
      p.Save();
      if (s.geometry && o.figures.size() == 1) p.ClipPath(o.figures.front().path);
      if (s.picture && s.picture->image) DrawPicture(p, *s.picture, box);
      else if (!s.picture || !s.picture->image) DrawPlaceholder(p, s, box);
      p.Restore();
      StrokeFigures(p, s, o.figures, box);
      return;
    }
    case Shape::Kind::Shape: {
      if (s.textPath) {
        DrawTextPath(p, s, box);
        return;
      }
      Geometry::Outline o = OutlineOf(s, size);
      for (const Geometry::Figure& f : o.figures) {
        if (f.fill != Geometry::PathFill::None && !f.path.Empty()) FillArea(p, f.path, s.fill, box, s.rotation, f.fill);
      }
      StrokeFigures(p, s, o.figures, box);
      return;
    }
  }
}

void DrawShapeAt(Painter& p, const Shape& s, const Rect& bounds) {
  if (s.hidden) return;
  if (s.shadow.on) p.BeginShadow(ShadowFor(s.shadow, bounds));
  else p.Save();
  p.Transform(Place(s, bounds));
  DrawContent(p, s, {bounds.Width(), bounds.Height()});
  p.Restore();
}

void Expand(Rect& r, bool& any, Point q) {
  if (!any) r = {q.x, q.y, q.x, q.y};
  r = {std::min(r.left, q.x), std::min(r.top, q.y), std::max(r.right, q.x), std::max(r.bottom, q.y)};
  any = true;
}

void PaintedBoundsAt(const Shape& s, const Rect& bounds, const Matrix& toParent, Rect& r, bool& any) {
  if (s.hidden) return;
  const Matrix m = Place(s, bounds) * toParent;
  if (s.kind == Shape::Kind::Group) {
    for (const Shape& c : s.children) PaintedBoundsAt(c, ChildToGroup(s, c.bounds), m, r, any);
    return;
  }
  // The box's corners, grown by the line and its arrowheads, then the shadow.
  float grow = s.line.fill.kind != Fill::Kind::None ? std::max(s.line.width, 0.f) / 2 : 0;
  if (s.line.head.type != ArrowHead::Type::None || s.line.tail.type != ArrowHead::Type::None) {
    grow = std::max(grow, std::max(s.line.width, 2.f) * 2.5f);
  }
  Rect box{-grow, -grow, bounds.Width() + grow, bounds.Height() + grow};
  if (s.geometry) {
    // Callouts and some arrows reach outside their box.
    for (const Geometry::Figure& f : s.geometry->Evaluate({bounds.Width(), bounds.Height()}, s.adjust).figures) {
      const Rect fb = f.path.Bounds();
      box = {std::min(box.left, fb.left - grow), std::min(box.top, fb.top - grow), std::max(box.right, fb.right + grow),
             std::max(box.bottom, fb.bottom + grow)};
    }
  }
  const Point corners[] = {{box.left, box.top}, {box.right, box.top}, {box.right, box.bottom}, {box.left, box.bottom}};
  Rect own{};
  bool ownAny = false;
  for (const Point& c : corners) Expand(own, ownAny, m.Apply(c));
  Expand(r, any, {own.left, own.top});
  Expand(r, any, {own.right, own.bottom});
  if (s.shadow.on) {
    const Shadow sh = ShadowFor(s.shadow, bounds);
    const Matrix sm = Place(s, bounds) * sh.transform * toParent;
    for (const Point& c : corners) {
      const Point q = sm.Apply(c);
      Expand(r, any, {q.x - sh.blur, q.y - sh.blur});
      Expand(r, any, {q.x + sh.blur, q.y + sh.blur});
    }
  }
}

void OutlinesAt(const Shape& s, const Rect& bounds, const Matrix& toDrawing, std::vector<Path::Polygon>& out) {
  if (s.hidden) return;
  const Matrix m = Place(s, bounds) * toDrawing;
  if (s.kind == Shape::Kind::Group) {
    for (const Shape& c : s.children) OutlinesAt(c, ChildToGroup(s, c.bounds), m, out);
    return;
  }
  for (const Geometry::Figure& f : OutlineOf(s, {bounds.Width(), bounds.Height()}).figures) {
    for (Path::Polygon& poly : f.path.Flatten(0.5f)) {
      for (Point& q : poly.points) q = m.Apply(q);
      out.push_back(std::move(poly));
    }
  }
}

void TextBoxesAt(const Shape& s, const Rect& bounds, const Matrix& toDrawing, std::vector<TextBox>& out) {
  if (s.hidden) return;
  const Matrix m = Place(s, bounds) * toDrawing;
  if (s.kind == Shape::Kind::Group) {
    for (const Shape& c : s.children) TextBoxesAt(c, ChildToGroup(s, c.bounds), m, out);
    return;
  }
  if (!s.text) return;
  TextBox box = s.text->box;
  box.content = s.text->content;
  box.properties = s.text->properties;
  box.insets = s.text->insets;
  const Rect r = OutlineOf(s, {bounds.Width(), bounds.Height()}).textRect;
  box.rect = {r.left + box.insets.left, r.top + box.insets.top, r.right - box.insets.right, r.bottom - box.insets.bottom};
  if (box.rect.right < box.rect.left) box.rect.left = box.rect.right = (box.rect.left + box.rect.right) / 2;
  if (box.rect.bottom < box.rect.top) box.rect.top = box.rect.bottom = (box.rect.top + box.rect.bottom) / 2;
  box.transform = m;
  out.push_back(box);
}

}  // namespace

Matrix ShapeTransform(const Shape& s) { return Place(s, s.bounds); }

Rect ChildToGroup(const Shape& g, const Rect& c) {
  const Rect& ch = g.childBounds;
  const float sx = std::abs(ch.Width()) > 1e-6f ? g.bounds.Width() / ch.Width() : 1;
  const float sy = std::abs(ch.Height()) > 1e-6f ? g.bounds.Height() / ch.Height() : 1;
  return {(c.left - ch.left) * sx, (c.top - ch.top) * sy, (c.right - ch.left) * sx, (c.bottom - ch.top) * sy};
}

void DrawShape(Painter& p, const Shape& s) { DrawShapeAt(p, s, s.bounds); }

Rect PaintedBounds(const Shape& s) {
  Rect r{};
  bool any = false;
  PaintedBoundsAt(s, s.bounds, Matrix{}, r, any);
  return r;
}

void CollectOutlines(const Shape& s, const Matrix& toDrawing, std::vector<Path::Polygon>& out) {
  OutlinesAt(s, s.bounds, toDrawing, out);
}

void CollectTextBoxes(const Shape& s, const Matrix& toDrawing, std::vector<TextBox>& out) {
  TextBoxesAt(s, s.bounds, toDrawing, out);
}

std::shared_ptr<const Bitmap> PictureFill::Pixels(int width, int height) const {
  if (!image) return nullptr;
  std::shared_ptr<const Bitmap> src = image->Pixels(width, height);
  if (!src || !Recolors()) return src;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recolored_ && source_.lock() == src) {
      std::shared_ptr<const Bitmap> kept = recolored_;
      UsedPixels(this);
      return kept;
    }
  }
  std::vector<uint8_t> px(src->Pixels(), src->Pixels() + static_cast<size_t>(src->Stride()) * src->Height());
  // Premultiplied BGRA: unpremultiply, change the color, premultiply again.
  for (size_t i = 0; i + 3 < px.size(); i += 4) {
    const float a = px[i + 3] / 255.f;
    if (a <= 0) continue;
    float b = px[i] / 255.f / a, g = px[i + 1] / 255.f / a, r = px[i + 2] / 255.f / a, opacity = a;
    if (colorChange) {
      auto same = [](float x, float y) { return std::abs(x - y) < 2.f / 255; };
      if (same(r, changeFrom.r) && same(g, changeFrom.g) && same(b, changeFrom.b)) {
        r = changeTo.r;
        g = changeTo.g;
        b = changeTo.b;
        opacity *= changeTo.a;
      }
    }
    if (brightness != 0 || contrast != 0) {
      // a:lum as Word draws it (measured on gray ramps): contrast scales around the middle gray,
      // with half the brightness added before and half after.
      const float k = contrast >= 0 ? 1 / std::max(1e-3f, 1 - contrast) : 1 + contrast;
      for (float* c : {&r, &g, &b}) *c = std::clamp((*c + brightness / 2 - 0.5f) * k + 0.5f + brightness / 2, 0.f, 1.f);
    }
    if (gain != 1 || blackLevel != 0) {
      // Word applies these as a:lum, with blacklevel as half the brightness: added before and after.
      for (float* c : {&r, &g, &b}) *c = std::clamp((*c + blackLevel - 0.5f) * gain + 0.5f + blackLevel, 0.f, 1.f);
    }
    const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;  // Rec. 709 weights, as Word's grayscale
    if (grayscale) r = g = b = y;
    if (biLevel >= 0) r = g = b = y >= biLevel ? 1.f : 0.f;
    if (duotone) {
      r = duotoneDark.r + (duotoneLight.r - duotoneDark.r) * y;
      g = duotoneDark.g + (duotoneLight.g - duotoneDark.g) * y;
      b = duotoneDark.b + (duotoneLight.b - duotoneDark.b) * y;
    }
    px[i] = static_cast<uint8_t>(std::lround(std::clamp(b, 0.f, 1.f) * opacity * 255));
    px[i + 1] = static_cast<uint8_t>(std::lround(std::clamp(g, 0.f, 1.f) * opacity * 255));
    px[i + 2] = static_cast<uint8_t>(std::lround(std::clamp(r, 0.f, 1.f) * opacity * 255));
    px[i + 3] = static_cast<uint8_t>(std::lround(opacity * 255));
  }
  auto made = std::make_shared<const Bitmap>(src->Width(), src->Height(), std::move(px));
  {
    std::lock_guard<std::mutex> lock(mutex_);
    source_ = src;
    recolored_ = made;
  }
  KeepPixels(shared_from_this(), static_cast<uint64_t>(made->Stride()) * made->Height());
  return made;
}

void PictureFill::DropPixels() const {
  std::lock_guard<std::mutex> lock(mutex_);
  recolored_.reset();
  source_.reset();
}

namespace {

constexpr std::string_view kPatternNames[] = {
    "pct5",      "pct10",     "pct20",      "pct25",      "pct30",      "pct40",      "pct50",    "pct60",
    "pct70",     "pct75",     "pct80",      "pct90",      "horz",       "vert",       "ltHorz",   "ltVert",
    "dkHorz",    "dkVert",    "narHorz",    "narVert",    "dashHorz",   "dashVert",   "cross",    "dnDiag",
    "upDiag",    "ltDnDiag",  "ltUpDiag",   "dkDnDiag",   "dkUpDiag",   "wdDnDiag",   "wdUpDiag", "dashDnDiag",
    "dashUpDiag", "diagCross", "smCheck",   "lgCheck",    "smGrid",     "lgGrid",     "dotGrid",  "smConfetti",
    "lgConfetti", "horzBrick", "diagBrick", "solidDmnd",  "openDmnd",   "dotDmnd",    "plaid",    "sphere",
    "weave",     "divot",     "shingle",    "wave",       "trellis",    "zigZag"};
constexpr int kPercent[] = {5, 10, 20, 25, 30, 40, 50, 60, 70, 75, 80, 90};
// The other patterns' 8 x 8 cells, a row per byte, the leftmost pixel in the top bit.
constexpr uint8_t kCells[][8] = {
    {0xFF, 0, 0, 0, 0, 0, 0, 0},                               // horz
    {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},          // vert
    {0xFF, 0, 0, 0, 0xFF, 0, 0, 0},                            // ltHorz
    {0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88},          // ltVert
    {0xFF, 0xFF, 0, 0, 0xFF, 0xFF, 0, 0},                      // dkHorz
    {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC},          // dkVert
    {0xFF, 0, 0xFF, 0, 0xFF, 0, 0xFF, 0},                      // narHorz
    {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA},          // narVert
    {0xF0, 0, 0, 0, 0x0F, 0, 0, 0},                            // dashHorz
    {0x80, 0x80, 0x80, 0x80, 0x08, 0x08, 0x08, 0x08},          // dashVert
    {0xFF, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},          // cross
    {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01},          // dnDiag
    {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80},          // upDiag
    {0x88, 0x44, 0x22, 0x11, 0x88, 0x44, 0x22, 0x11},          // ltDnDiag
    {0x11, 0x22, 0x44, 0x88, 0x11, 0x22, 0x44, 0x88},          // ltUpDiag
    {0xCC, 0x66, 0x33, 0x99, 0xCC, 0x66, 0x33, 0x99},          // dkDnDiag
    {0x33, 0x66, 0xCC, 0x99, 0x33, 0x66, 0xCC, 0x99},          // dkUpDiag
    {0xC1, 0xE0, 0x70, 0x38, 0x1C, 0x0E, 0x07, 0x83},          // wdDnDiag
    {0x83, 0x07, 0x0E, 0x1C, 0x38, 0x70, 0xE0, 0xC1},          // wdUpDiag
    {0x88, 0x44, 0x22, 0x11, 0, 0, 0, 0},                      // dashDnDiag
    {0x11, 0x22, 0x44, 0x88, 0, 0, 0, 0},                      // dashUpDiag
    {0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81},          // diagCross
    {0xCC, 0xCC, 0x33, 0x33, 0xCC, 0xCC, 0x33, 0x33},          // smCheck
    {0xF0, 0xF0, 0xF0, 0xF0, 0x0F, 0x0F, 0x0F, 0x0F},          // lgCheck
    {0xFF, 0x88, 0x88, 0x88, 0xFF, 0x88, 0x88, 0x88},          // smGrid
    {0xFF, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},          // lgGrid
    {0xAA, 0, 0x80, 0, 0x80, 0, 0x80, 0},                      // dotGrid
    {0x80, 0x08, 0x40, 0x02, 0x20, 0x01, 0x10, 0x04},          // smConfetti
    {0xB1, 0x30, 0x03, 0x1B, 0xD8, 0xC0, 0x0C, 0x8D},          // lgConfetti
    {0xFF, 0x80, 0x80, 0x80, 0xFF, 0x08, 0x08, 0x08},          // horzBrick
    {0x01, 0x02, 0x04, 0x08, 0x18, 0x24, 0x42, 0x81},          // diagBrick
    {0x10, 0x38, 0x7C, 0xFE, 0x7C, 0x38, 0x10, 0x00},          // solidDmnd
    {0x80, 0x41, 0x22, 0x14, 0x08, 0x14, 0x22, 0x41},          // openDmnd
    {0x80, 0x00, 0x22, 0x00, 0x08, 0x00, 0x22, 0x00},          // dotDmnd
    {0xAA, 0x55, 0xAA, 0x55, 0xF0, 0xF0, 0xF0, 0xF0},          // plaid
    {0x77, 0x89, 0x8F, 0x8F, 0x77, 0x98, 0xF8, 0xF8},          // sphere
    {0x88, 0x54, 0x22, 0x45, 0x88, 0x14, 0x22, 0x51},          // weave
    {0x00, 0x10, 0x08, 0x10, 0x00, 0x01, 0x80, 0x01},          // divot
    {0x03, 0x84, 0x48, 0x30, 0x0C, 0x02, 0x01, 0x01},          // shingle
    {0x00, 0x18, 0xA4, 0x03, 0x00, 0x18, 0xA4, 0x03},          // wave
    {0xFF, 0x66, 0xFF, 0x99, 0xFF, 0x66, 0xFF, 0x99},          // trellis
    {0x81, 0x42, 0x24, 0x18, 0x81, 0x42, 0x24, 0x18}};         // zigZag
// The percentage patterns are ordered dithers: a pixel is set where this matrix is below the share.
constexpr uint8_t kBayer[8][8] = {{0, 32, 8, 40, 2, 34, 10, 42},  {48, 16, 56, 24, 50, 18, 58, 26},
                                  {12, 44, 4, 36, 14, 46, 6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
                                  {3, 35, 11, 43, 1, 33, 9, 41},  {51, 19, 59, 27, 49, 17, 57, 25},
                                  {15, 47, 7, 39, 13, 45, 5, 37}, {63, 31, 55, 23, 61, 29, 53, 21}};

uint32_t Premultiplied(Color c) {
  const float a = std::clamp(c.a, 0.f, 1.f);
  auto ch = [&](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.f, 1.f) * a * 255)); };
  return ch(c.b) | (ch(c.g) << 8) | (ch(c.r) << 16) | (static_cast<uint32_t>(std::lround(a * 255)) << 24);
}

}  // namespace

uint8_t PatternIndex(std::string_view name) {
  for (size_t i = 0; i < std::size(kPatternNames); ++i)
    if (kPatternNames[i] == name) return static_cast<uint8_t>(i);
  return 6;  // pct50
}

std::shared_ptr<const Bitmap> PatternBitmap(uint8_t pattern, Color fg, Color bg) {
  const uint32_t on = Premultiplied(fg), off = Premultiplied(bg);
  std::vector<uint8_t> px(8 * 8 * 4);
  const int percentCount = static_cast<int>(std::size(kPercent));
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      bool set;
      if (pattern < percentCount) set = kBayer[y][x] < (kPercent[pattern] * 64 + 50) / 100;
      else set = (kCells[std::min<size_t>(pattern - percentCount, std::size(kCells) - 1)][y] >> (7 - x)) & 1;
      const uint32_t v = set ? on : off;
      std::memcpy(&px[(y * 8 + x) * 4], &v, 4);
    }
  }
  return std::make_shared<Bitmap>(8, 8, std::move(px));
}

namespace {

// Glyph outlines from DirectWrite, into a Path.
class PathSink : public IDWriteGeometrySink {
 public:
  explicit PathSink(Path& path) : path_(path) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteGeometrySink)) {
      *out = static_cast<IDWriteGeometrySink*>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }  // lives on the stack
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE mode) override {
    path_.fillRule = mode == D2D1_FILL_MODE_ALTERNATE ? Path::FillRule::EvenOdd : Path::FillRule::Winding;
  }
  void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT) override {}
  void STDMETHODCALLTYPE BeginFigure(D2D1_POINT_2F p, D2D1_FIGURE_BEGIN) override { path_.MoveTo({p.x, p.y}); }
  void STDMETHODCALLTYPE AddLines(const D2D1_POINT_2F* pts, UINT32 n) override {
    for (UINT32 i = 0; i < n; ++i) path_.LineTo({pts[i].x, pts[i].y});
  }
  void STDMETHODCALLTYPE AddBeziers(const D2D1_BEZIER_SEGMENT* b, UINT32 n) override {
    for (UINT32 i = 0; i < n; ++i) {
      path_.CubicTo({b[i].point1.x, b[i].point1.y}, {b[i].point2.x, b[i].point2.y}, {b[i].point3.x, b[i].point3.y});
    }
  }
  void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END end) override {
    if (end == D2D1_FIGURE_END_CLOSED) path_.Close();
  }
  HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }

 private:
  Path& path_;
};

ComPtr<IDWriteFontFace> FindFace(IDWriteFactory* dw, const std::wstring& family, bool bold, bool italic) {
  ComPtr<IDWriteFontCollection> fonts;
  if (FAILED(dw->GetSystemFontCollection(&fonts))) return nullptr;
  for (const std::wstring& name : {family, std::wstring(L"Arial")}) {
    UINT32 index = 0;
    BOOL exists = FALSE;
    ComPtr<IDWriteFontFamily> fam;
    ComPtr<IDWriteFont> font;
    ComPtr<IDWriteFontFace> face;
    if (SUCCEEDED(fonts->FindFamilyName(name.c_str(), &index, &exists)) && exists &&
        SUCCEEDED(fonts->GetFontFamily(index, &fam)) &&
        SUCCEEDED(fam->GetFirstMatchingFont(bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL,
                                            italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, &font)) &&
        SUCCEEDED(font->CreateFontFace(&face))) {
      return face;
    }
  }
  return nullptr;
}

}  // namespace

Path TextOutline(const std::wstring& text, const std::wstring& font, bool bold, bool italic, float size) {
  Path path;
  static std::once_flag once;
  static ComPtr<IDWriteFactory> dw;
  std::call_once(once, [] {
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()));
  });
  if (!dw || text.empty() || text.size() > 4096) return path;
  ComPtr<IDWriteFontFace> face = FindFace(dw.Get(), font, bold, italic);
  if (!face) return path;
  std::vector<UINT32> cps;
  for (size_t i = 0; i < text.size(); ++i) {
    UINT32 c = text[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] < 0xE000) {
      c = 0x10000 + ((c - 0xD800) << 10) + (text[++i] - 0xDC00);
    }
    cps.push_back(c);
  }
  std::vector<UINT16> glyphs(cps.size());
  std::vector<DWRITE_GLYPH_METRICS> metrics(cps.size());
  DWRITE_FONT_METRICS fm{};
  face->GetMetrics(&fm);
  if (FAILED(face->GetGlyphIndices(cps.data(), static_cast<UINT32>(cps.size()), glyphs.data())) ||
      FAILED(face->GetDesignGlyphMetrics(glyphs.data(), static_cast<UINT32>(glyphs.size()), metrics.data(), FALSE)) ||
      fm.designUnitsPerEm == 0) {
    return path;
  }
  std::vector<float> advances;
  for (const auto& m : metrics) advances.push_back(m.advanceWidth * size / fm.designUnitsPerEm);
  PathSink sink(path);
  face->GetGlyphRunOutline(size, glyphs.data(), advances.data(), nullptr, static_cast<UINT32>(glyphs.size()), FALSE,
                           FALSE, &sink);
  return path;
}

}  // namespace draw
