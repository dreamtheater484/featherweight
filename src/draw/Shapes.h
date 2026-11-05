#pragma once

#include <pugixml.hpp>

#include <memory>
#include <string>
#include <vector>

#include "draw/Drawing.h"
#include "draw/Geometry.h"
#include "draw/Images.h"
#include "draw/Painter.h"

// The shape model the DrawingML and VML readers build and the renderer draws. Colors are resolved
// against the theme while reading, so drawing needs nothing but the model. Internal to fwdraw:
// engines use Drawing.
namespace draw {

// A picture filling an area (a:blipFill, v:imagedata, v:fill type="frame"/"tile").
struct PictureFill {
  std::shared_ptr<const Image> image;  // null: missing or unreadable
  Rect crop{};                         // a:srcRect: fractions cut from each side (negative pads)
  Rect inset{};                        // a:stretch/a:fillRect: fractions inset from each side
  bool tile = false;                   // a:tile: repeated at its natural size
  float tileX = 0, tileY = 0;          // tile offset, points
  float tileScaleX = 1, tileScaleY = 1;
  bool mirrorX = false, mirrorY = false;
  uint8_t tileAlign = 0;               // 0-8: tl, t, tr, l, ctr, r, bl, b, br
  // Recoloring: a:blip's effects and VML's picture settings.
  float alpha = 1;
  bool grayscale = false;
  float biLevel = -1;                  // threshold 0-1; negative: off
  bool duotone = false;
  Color duotoneDark, duotoneLight;
  bool colorChange = false;
  Color changeFrom, changeTo;
  float brightness = 0, contrast = 0;  // a:lum, -1 to 1
  float gain = 1, blackLevel = 0;      // VML's contrast factor and brightness offset (washout)

  bool Recolors() const {
    return grayscale || biLevel >= 0 || duotone || colorChange || brightness != 0 || contrast != 0 || gain != 1 ||
           blackLevel != 0;
  }
  // Pixels to draw for about `width` x `height` device pixels: the picture's own, recolored when
  // needed. The recolored copy is kept between draws.
  std::shared_ptr<const Bitmap> Pixels(int width, int height) const;

 private:
  mutable std::shared_ptr<const Bitmap> source_, recolored_;
};

struct Fill {
  enum class Kind : uint8_t { None, Solid, Gradient, Picture, Pattern };
  enum class GradientShape : uint8_t { Linear, Circle, Rect, Shape };
  Kind kind = Kind::None;
  Color color;                       // Solid; Pattern: the foreground
  Color background;                  // Pattern
  uint8_t pattern = 0;               // Pattern: index into the presets (PatternBitmap)
  std::vector<GradientStop> stops;   // Gradient, sorted
  GradientShape shape = GradientShape::Linear;
  float angle = 0;                   // Linear: degrees, clockwise from left-to-right
  bool scaled = false;               // Linear: the angle is for a square, stretched with the shape
  Rect focus{0.5f, 0.5f, 0.5f, 0.5f};  // Circle, Rect, Shape: a:fillToRect, fractions inset from each side
  Rect tile{};                         // a:tileRect: fractions inset from each side, negative beyond the box
  bool rotateWithShape = true;
  std::shared_ptr<PictureFill> picture;
  mutable std::shared_ptr<const Bitmap> patternBitmap;  // made on first use
};

struct ArrowHead {
  enum class Type : uint8_t { None, Triangle, Stealth, Diamond, Oval, Arrow };
  Type type = Type::None;
  uint8_t width = 1, length = 1;  // 0 small, 1 medium, 2 large
};

struct Line {
  Fill fill;  // Kind::None: no line
  float width = 0.75f;  // points; 0 is the thinnest line the device can show
  LineCap cap = LineCap::Flat;
  LineJoin join = LineJoin::Round;
  float miterLimit = 8;
  std::vector<float> dashes;  // multiples of the width; empty: solid
  bool inset = false;         // drawn inside the outline (algn="in")
  ArrowHead head, tail;       // at the start and the end of open paths
};

struct OuterShadow {
  bool on = false;
  Color color{0, 0, 0, 0.5f};
  float blur = 0, distance = 0, direction = 0;  // points, points, degrees clockwise from the right
  float scaleX = 1, scaleY = 1, skewX = 0, skewY = 0;  // skews in degrees
  uint8_t align = 6;  // the point scaling keeps still, as PictureFill::tileAlign (6: bottom left)
};

// WordArt from VML (v:textpath): text drawn as a shape, stretched to fill it (watermarks).
struct TextPath {
  std::wstring text;
  std::wstring font = L"Arial";
  bool bold = false, italic = false;
  bool fitShape = true;
};

// A text body and its properties, before it is placed (Drawing turns it into a TextBox).
struct TextBody {
  pugi::xml_node content, properties;
  Rect insets{7.2f, 3.6f, 7.2f, 3.6f};
  TextBox box;  // the flags; rect and transform are filled in later
};

struct Shape {
  enum class Kind : uint8_t { Shape, Picture, Group, Placeholder };
  Kind kind = Kind::Shape;
  std::string name, description;
  Rect bounds;  // in the parent's coordinates, points
  float rotation = 0;  // degrees clockwise
  bool flipH = false, flipV = false;
  bool hidden = false;
  std::shared_ptr<const Geometry> geometry;  // null: a rectangle
  std::vector<Geometry::Adjust> adjust;
  Fill fill;
  Line line;
  OuterShadow shadow;
  std::shared_ptr<PictureFill> picture;  // Picture: what fills the geometry
  Rect childBounds;                       // Group: chOff and chExt, the children's coordinates
  std::vector<Shape> children;
  std::shared_ptr<TextBody> text;
  std::shared_ptr<TextPath> textPath;
};

// The shape's own coordinates ((0, 0) to its size) → its parent's: flips, then rotation, about
// its center.
Matrix ShapeTransform(const Shape& s);
// A group child's bounds in the group's own coordinates.
Rect ChildToGroup(const Shape& group, const Rect& child);
// Draws a shape in its parent's coordinates.
void DrawShape(Painter& p, const Shape& s);
// The flattened outlines of every visible shape (`toDrawing`: parent coordinates → the drawing's).
void CollectOutlines(const Shape& s, const Matrix& toDrawing, std::vector<Path::Polygon>& out);
// The text boxes, in drawing order.
void CollectTextBoxes(const Shape& s, const Matrix& toDrawing, std::vector<TextBox>& out);
// The bounds of what the shape paints in its parent's coordinates, lines and shadows included.
Rect PaintedBounds(const Shape& s);
// A pattern fill's 8 x 8 cell (ST_PresetPatternVal, in the schema's order).
uint8_t PatternIndex(std::string_view name);
std::shared_ptr<const Bitmap> PatternBitmap(uint8_t pattern, Color foreground, Color background);
// Glyph outlines for `text` in `font`, as one path, with the baseline at y = 0 (DirectWrite).
Path TextOutline(const std::wstring& text, const std::wstring& font, bool bold, bool italic, float size);

// ---- Readers (DrawingML.cpp, Vml.cpp) ----

struct ReadContext {
  DrawingResources& resources;
  std::shared_ptr<const Theme> theme;
  ColorContext colors;
  int shapes = 0;  // read so far, against kMaxShapes
  int depth = 0;   // group nesting, against kMaxDepth
  static constexpr int kMaxShapes = 20000;
  static constexpr int kMaxDepth = 32;

  explicit ReadContext(DrawingResources& r);
};

// One DrawingML shape element (sp, wsp, pic, cxnSp, grpSp, wgp, graphicFrame, wpc) into `out`,
// with bounds from its xfrm. `groupFill` is the enclosing group's fill, for a:grpFill.
bool ReadDrawingMLShape(pugi::xml_node el, ReadContext& ctx, Shape& out, const Fill* groupFill = nullptr);
// What a:graphicData holds, as a shape filling (0, 0) to `extent` (Word's inline and anchored drawings).
bool ReadGraphicData(pugi::xml_node graphicData, Size extent, ReadContext& ctx, Shape& out);
// One VML element (v:shape, v:rect, v:group…) into `out`, with bounds from its style.
bool ReadVmlShape(pugi::xml_node el, ReadContext& ctx, Shape& out);

}  // namespace draw
