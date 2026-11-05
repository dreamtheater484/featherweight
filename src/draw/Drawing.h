#pragma once

#include <pugixml.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "draw/Images.h"
#include "draw/Painter.h"
#include "draw/Theme.h"

namespace fw::pkg {
class Package;
}

// Drawing objects in documents, sheets and slides (plan §4.5): pictures, shapes, groups,
// connectors and text boxes, from DrawingML or from VML, read into one shape model and drawn with a
// Painter. Layout decides where a drawing goes and how text wraps around it; a Drawing says how
// big it is, what outline text wraps against, where its text goes, and draws itself.
//
//   MyResources res(...);  // relationship targets and the theme
//   auto d = draw::Drawing::FromWordDrawing(inlineOrAnchor, res);
//   Size size = d->Extent();                       // points
//   d->Draw(painter, {x, y, x + size.width, y + size.height});
//   for (const TextBox& box : d->TextBoxes()) ...  // the text is laid out by the caller
namespace draw {

struct Shape;

// What a drawing needs from the package it lives in. The layout phase implements it, or uses
// PackageResources below.
class DrawingResources {
 public:
  virtual ~DrawingResources() = default;
  // The theme in effect; Theme::Default() when the file has none.
  virtual std::shared_ptr<const Theme> GetTheme() { return Theme::Default(); }
  // bg1, tx1… for this part: a slide's color map, Word's w:clrSchemeMapping.
  virtual const ColorMap& GetColorMap() {
    static const ColorMap identity;
    return identity;
  }
  // The picture an embedded relationship (r:embed, r:id) of the drawing's part points to. Null when
  // the target is missing or not a picture Featherweight reads. Linked pictures (r:link,
  // TargetMode="External") are never fetched (plan §4.7): don't resolve them.
  virtual std::shared_ptr<const Image> GetImage(std::string_view relId) = 0;
  // The root element of the XML part a relationship points to (SmartArt's drawing), or null.
  virtual pugi::xml_node GetPartXml(std::string_view relId) {
    (void)relId;
    return {};
  }
};

// Where a shape's text goes. The text itself is laid out and drawn by the caller (fwtext); fwdraw
// only reads the box.
struct TextBox {
  enum class Flow : uint8_t { Horizontal, Vertical, Vertical270, Stacked, EastAsian, Mongolian };
  enum class Anchor : uint8_t { Top, Middle, Bottom, Justified, Distributed };
  enum class Autofit : uint8_t { None, ShrinkText, ResizeShape };

  pugi::xml_node content;     // the text: w:txbxContent (Word), a:txBody/p:txBody/xdr:txBody (DrawingML)
  pugi::xml_node properties;  // a:bodyPr / wps:bodyPr, or the v:textbox element (VML)
  Rect rect;                  // the text area in the shape's coordinates, insets taken off
  Rect insets;                // left, top, right, bottom, in points
  Matrix transform;           // the shape's coordinates → the drawing's (its group's rotation and flips)
  float rotation = 0;         // extra rotation of the text (bodyPr rot), degrees
  Flow flow = Flow::Horizontal;
  Anchor anchor = Anchor::Top;
  bool anchorCenter = false;  // center the text block horizontally
  bool wrap = true;           // false: lines break only at paragraph ends
  bool upright = false;       // keep the text upright when the shape rotates
  Autofit autofit = Autofit::None;
  float fontScale = 1, lineSpacingReduction = 0;  // ShrinkText: what Office last applied
  int columns = 1;
  float columnSpacing = 0;    // points
};

class Drawing {
 public:
  ~Drawing();
  Drawing(const Drawing&) = delete;
  Drawing& operator=(const Drawing&) = delete;

  // Word: a w:drawing, wp:inline or wp:anchor (pictures, wps shapes and text boxes, wpg groups,
  // wpc canvases; charts and other graphics show as placeholders). Null when it isn't a drawing.
  static std::unique_ptr<Drawing> FromWordDrawing(pugi::xml_node node, DrawingResources& resources);
  // VML: a v:shape, v:rect, v:roundrect, v:oval, v:line, v:polyline, v:image or v:group
  // (watermarks, older text boxes, OLE previews), or a w:pict or w:object for its first shape.
  // Word writes one shape to a w:pict, but one holding more shows them all: pass each.
  static std::unique_ptr<Drawing> FromVml(pugi::xml_node node, DrawingResources& resources);
  // One shape of a DrawingML shape tree: p:sp, p:pic, p:cxnSp, p:grpSp, p:graphicFrame, and their
  // xdr: (sheets) and dsp: (SmartArt drawings) equivalents. Bounds() is where the file puts it:
  // the xfrm, or for a sheet's shape without one, its one-cell or absolute anchor's ext.
  static std::unique_ptr<Drawing> FromShapeTree(pugi::xml_node node, DrawingResources& resources);

  // Where the drawing sits in its container, in points: for Word drawings (0, 0) and the extent,
  // for shapes in a tree their xfrm, for VML its style's left, top, width and height.
  Rect Bounds() const { return bounds_; }
  Size Extent() const { return {bounds_.Width(), bounds_.Height()}; }
  // What drawing paints, line widths and shadows included, relative to the extent's top left.
  Rect InkBounds() const;
  // Draws the drawing into `frame` (normally the extent at its place on the page; a different
  // size scales it).
  void Draw(Painter& painter, const Rect& frame) const;
  // The outline for tight and through wrapping, relative to the extent's top left: the file's
  // wrap polygon (wp:wrapPolygon, o:wrapcoords) when it has one, else the shapes' own outlines.
  const std::vector<Path::Polygon>& WrapOutline() const { return wrap_; }
  bool HasWrapPolygon() const { return wrapFromFile_; }
  // The text boxes, relative to the extent's top left, in drawing order.
  const std::vector<TextBox>& TextBoxes() const { return textBoxes_; }
  const std::string& Name() const { return name_; }
  const std::string& Description() const { return description_; }  // alt text
  // Something Featherweight can't draw yet (a chart, an embedded object without a preview): it
  // keeps its size and shows a neat placeholder with its name (plan §3).
  bool IsPlaceholder() const { return placeholder_; }
  // Hidden (wp:docPr hidden, VML visibility:hidden): takes no space on the page, draws nothing.
  bool IsHidden() const { return hidden_; }

 private:
  Drawing();

  std::unique_ptr<Shape> root_;
  Rect bounds_;
  std::vector<Path::Polygon> wrap_;
  bool wrapFromFile_ = false;
  std::vector<TextBox> textBoxes_;
  std::string name_, description_;
  bool placeholder_ = false;
  bool hidden_ = false;

  friend class DrawingBuilder;
};

// VML's CSS-like style attribute ("position:absolute;margin-left:12pt;…") as name and value pairs
// in order, for reading a VML shape's anchoring, and its lengths in points. `unitless` is what a
// number without a unit counts: pixels (0.75 pt) at the top level, a group's own units inside it.
std::vector<std::pair<std::string_view, std::string_view>> ParseVmlStyle(std::string_view style);
float VmlLength(std::string_view value, float unitless = 0.75f);

// Pictures opened once per package part and shared by every drawing that shows them, for one
// package. A raster picture reads its part again whenever it decodes rather than keeping a copy of
// its bytes, so the package must outlive the pictures.
class ImageCache {
 public:
  // Null when the part is missing or isn't a picture Featherweight reads.
  std::shared_ptr<const Image> Get(const fw::pkg::Package& package, std::string_view partName);

 private:
  std::mutex mutex_;
  std::map<std::string, std::shared_ptr<const Image>> images_;
};

// DrawingResources for one part of an OPC package: pictures through the part's relationships.
class PackageResources : public DrawingResources {
 public:
  // `theme` may be null (the default theme; FindTheme finds the usual one). Without `images`,
  // pictures are cached for this object only.
  PackageResources(const fw::pkg::Package& package, std::string partName, std::shared_ptr<const Theme> theme,
                   ColorMap colorMap = {}, std::shared_ptr<ImageCache> images = nullptr);
  ~PackageResources() override;

  std::shared_ptr<const Theme> GetTheme() override;
  const ColorMap& GetColorMap() override { return colorMap_; }
  std::shared_ptr<const Image> GetImage(std::string_view relId) override;
  pugi::xml_node GetPartXml(std::string_view relId) override;

  // The theme for a part: the document's, the workbook's, or a slide's through its layout and
  // master. Null when there is none.
  static std::shared_ptr<const Theme> FindTheme(const fw::pkg::Package& package, std::string_view partName);
  // The color map for a part: a slide's (its override, its layout's, its master's p:clrMap), or
  // Word's w:clrSchemeMapping. The default mapping when there is none.
  static ColorMap FindColorMap(const fw::pkg::Package& package, std::string_view partName);

 private:
  const fw::pkg::Package& package_;
  std::string part_;
  std::shared_ptr<const Theme> theme_;
  ColorMap colorMap_;
  std::shared_ptr<ImageCache> images_;
  std::map<std::string, std::unique_ptr<fw::pkg::XmlDom>> parts_;
};

}  // namespace draw
