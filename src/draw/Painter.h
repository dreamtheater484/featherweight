#pragma once

#include <dwrite.h>

#include <cstdint>
#include <memory>
#include <vector>

// Painter: the one drawing interface the document, spreadsheet and presentation engines draw
// with (plan §4.1, §4.5 and §4.6). Backends draw it to the screen and printers (Direct2D,
// D2DPainter) and, later, to PDF files. Engines never call Direct2D themselves, so whatever they
// draw looks the same on screen, on paper and in an exported PDF.
//
// Coordinates are whatever the current transform makes them; engines work in points with a
// page-to-device transform set up by whoever creates the painter.
namespace draw {

struct Color {
  float r = 0, g = 0, b = 0, a = 1;  // straight (not premultiplied) alpha, sRGB
};

struct Point {
  float x = 0, y = 0;
};

struct Size {
  float width = 0, height = 0;
};

struct Rect {
  float left = 0, top = 0, right = 0, bottom = 0;
  float Width() const { return right - left; }
  float Height() const { return bottom - top; }
};

// An affine transform, laid out as in Direct2D: x' = x*m11 + y*m21 + dx, y' = x*m12 + y*m22 + dy.
// a * b applies a first, then b.
struct Matrix {
  float m11 = 1, m12 = 0, m21 = 0, m22 = 1, dx = 0, dy = 0;

  static Matrix Translate(float x, float y) { return {1, 0, 0, 1, x, y}; }
  static Matrix Scale(float sx, float sy) { return {sx, 0, 0, sy, 0, 0}; }
  // Clockwise in degrees (y points down), around the origin.
  static Matrix Rotate(float degrees);
  Matrix operator*(const Matrix& o) const;
  Point Apply(Point p) const { return {p.x * m11 + p.y * m21 + dx, p.x * m12 + p.y * m22 + dy}; }
  // No rotation or skew, so rectangles stay rectangles.
  bool AxisAligned() const { return m12 == 0 && m21 == 0; }
  // The inverse, or the identity when there is none (a degenerate transform).
  Matrix Inverse() const;
  // How much the transform enlarges lengths along x and y, ignoring rotation.
  float ScaleX() const;
  float ScaleY() const;
};

// An outline made of figures. Each figure starts with MoveTo; a segment without one starts at the
// previous figure's end (or the origin).
class Path {
 public:
  enum class FillRule { EvenOdd, Winding };
  enum class Kind : uint8_t { Move, Line, Quad, Cubic, Arc, Close };
  struct Segment {
    Kind kind;
    Point p[3];                // Move/Line: p[0]. Quad: control p[0], end p[1]. Cubic: p[0], p[1], end p[2]. Arc: end p[0].
    float rx = 0, ry = 0;      // Arc radii
    float rotation = 0;        // Arc: x-axis rotation in degrees
    bool largeArc = false;     // Arc: the longer of the two possible arcs
    bool clockwise = false;    // Arc: sweep direction (y points down)
  };

  void MoveTo(Point p);
  void LineTo(Point p);
  void QuadTo(Point control, Point end);
  void CubicTo(Point c1, Point c2, Point end);
  // An elliptical arc to `end`, as in SVG.
  void ArcTo(Point end, float rx, float ry, float rotation, bool largeArc, bool clockwise);
  void Close();

  static Path FromRect(const Rect& r);
  static Path RoundedRect(const Rect& r, float rx, float ry);
  static Path Ellipse(const Rect& bounds);

  // The outline as straight lines: one polygon per figure, curves and arcs within `tolerance` of
  // the true curve. Wrap outlines and tests use it; drawing doesn't need it.
  struct Polygon {
    std::vector<Point> points;
    bool closed = false;
  };
  std::vector<Polygon> Flatten(float tolerance) const;
  // The bounds of the flattened outline; an empty rectangle at the origin for an empty path.
  Rect Bounds() const;

  FillRule fillRule = FillRule::Winding;
  const std::vector<Segment>& Segments() const { return segments_; }
  bool Empty() const { return segments_.empty(); }

 private:
  std::vector<Segment> segments_;
};

struct GradientStop {
  float offset = 0;  // 0..1 along the gradient
  Color color;
};

class Bitmap;

// How an area is painted. Gradient geometry is in the current coordinate space.
struct Brush {
  enum class Kind { Solid, Linear, Radial, Bitmap };
  // How a bitmap continues past its edges.
  enum class Extend : uint8_t { Clamp, Repeat, Mirror };
  Kind kind = Kind::Solid;
  Color color;                       // Solid
  Point start, end;                  // Linear: the gradient line
  Point center;                      // Radial: the ellipse's center...
  float rx = 0, ry = 0;              // ...and radii
  Point focusOffset;                 // Radial: the focus, relative to the center
  std::vector<GradientStop> stops;   // Linear and Radial, in increasing offset
  // Bitmap: its pixels placed by `bitmapTransform` (from pixels to the current coordinates), for
  // tiled pictures and pattern fills. Without `smooth`, pixels stay sharp squares (patterns).
  std::shared_ptr<const draw::Bitmap> bitmap;
  Matrix bitmapTransform;
  Extend extendX = Extend::Clamp, extendY = Extend::Clamp;
  bool smooth = true;

  static Brush Solid(Color c);
  static Brush Linear(Point start, Point end, std::vector<GradientStop> stops);
  static Brush Radial(Point center, float rx, float ry, std::vector<GradientStop> stops);
  static Brush FromBitmap(std::shared_ptr<const draw::Bitmap> bitmap, const Matrix& transform, Extend extend);
};

enum class LineCap { Flat, Square, Round };
enum class LineJoin { Miter, Bevel, Round };

struct Pen {
  Brush brush;
  float width = 1;
  LineCap cap = LineCap::Flat;
  LineJoin join = LineJoin::Miter;
  float miterLimit = 10;
  // Dash and gap lengths in multiples of the width, as Direct2D and DrawingML count them. Empty
  // means solid.
  std::vector<float> dashes;
  float dashOffset = 0;  // also in multiples of the width
};

// Pixels for an image: 32-bit premultiplied BGRA, top-down rows. Immutable once made, so a backend
// can keep its own copy (a GPU bitmap) under Id().
class Bitmap {
 public:
  Bitmap(int width, int height, std::vector<uint8_t> pixels);
  int Width() const { return width_; }
  int Height() const { return height_; }
  int Stride() const { return width_ * 4; }
  const uint8_t* Pixels() const { return pixels_.data(); }
  uint64_t Id() const { return id_; }

 private:
  int width_, height_;
  std::vector<uint8_t> pixels_;
  uint64_t id_;
};

// A vector picture: an EMF metafile (WMF pictures are converted to EMF when read) or an SVG
// document. Immutable like a Bitmap, so a backend can keep its own copy under Id().
class VectorImage {
 public:
  enum class Kind : uint8_t { Emf, Svg };
  // `size` is its natural size in points: an EMF's frame, an SVG's width and height.
  VectorImage(Kind kind, std::vector<uint8_t> data, Size size);
  Kind GetKind() const { return kind_; }
  const std::vector<uint8_t>& Data() const { return data_; }
  Size NaturalSize() const { return size_; }
  uint64_t Id() const { return id_; }

 private:
  Kind kind_;
  std::vector<uint8_t> data_;
  Size size_;
  uint64_t id_;
};

// A soft shadow behind what is drawn (DrawingML's outer shadow).
struct Shadow {
  Color color;      // its alpha is the shadow's opacity
  float blur = 0;   // blur radius, in the current coordinates
  Matrix transform;  // moves (and scales or skews) the silhouette, in the current coordinates
};

// Shaped glyphs in one font at one size, as text layout produces them. The arrays outlive the
// call; advances and offsets are in the current coordinate space.
struct GlyphRun {
  IDWriteFontFace* face = nullptr;
  float emSize = 0;
  uint32_t count = 0;
  const uint16_t* indices = nullptr;
  const float* advances = nullptr;
  const DWRITE_GLYPH_OFFSET* offsets = nullptr;  // may be null
  bool sideways = false;
  uint32_t bidiLevel = 0;  // odd: right to left
};

class Painter {
 public:
  virtual ~Painter() = default;

  // ---- State ----
  // Save pushes the transform, clip and layers; Restore pops back to the matching Save.
  virtual void Save() = 0;
  virtual void Restore() = 0;
  // Applies `m` before the current transform, so `m` is expressed in the current coordinates.
  virtual void Transform(const Matrix& m) = 0;
  virtual Matrix CurrentTransform() const = 0;
  // Clips to the intersection with the current clip, until the matching Restore.
  virtual void ClipRect(const Rect& r) = 0;
  virtual void ClipPath(const Path& p) = 0;
  // Draws everything until the matching Restore into a group, then blends it at `opacity`.
  // It implies a Save. When printing through Microsoft Print to PDF, the opacity ends up on each
  // shape instead of the whole group, so overlaps inside the group show through (seen in D0; not
  // yet known whether Direct2D's print path or the driver flattens it).
  virtual void BeginGroup(float opacity) = 0;

  // ---- Drawing ----
  virtual void FillPath(const Path& p, const Brush& b) = 0;
  virtual void StrokePath(const Path& p, const Pen& pen) = 0;
  virtual void FillRect(const Rect& r, const Brush& b) { FillPath(Path::FromRect(r), b); }
  // Glyphs on a baseline starting at `origin`.
  virtual void DrawGlyphs(Point origin, const GlyphRun& run, const Brush& b) = 0;
  // The bitmap (or its `src` part, in pixels) stretched to `dest`.
  virtual void DrawBitmap(const Bitmap& bitmap, const Rect& dest, const Rect* src = nullptr, float opacity = 1) = 0;
  // A vector picture stretched to `dest`. By default it is drawn as pixels at the device's
  // resolution (RasterizeVectorImage), which is what a backend that can't play metafiles or SVG
  // gets, such as PDF export (plan §4.6, decision 9).
  virtual void DrawVectorImage(const VectorImage& image, const Rect& dest, float opacity = 1);

  // ---- Effects ----
  // Draws everything until the matching Restore with `shadow` behind it: the silhouette of what
  // was drawn, in the shadow's color, blurred and moved. It implies a Save. By default there is no
  // shadow: backends that can't blur leave it out (best effort, plan §4.5).
  virtual void BeginShadow(const Shadow& shadow);
};

}  // namespace draw
