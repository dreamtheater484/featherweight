#pragma once

#include <pugixml.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "draw/Drawing.h"
#include "draw/Painter.h"
#include "draw/Theme.h"

namespace fw::text {
class Fonts;
}

// Slide rendering (plan §8.3, P1): a slide drawn as PowerPoint draws it, at any size, through a
// draw::Painter, so the screen (Direct2D), printing and Export PDF show the same slide.
//
//   - Inheritance: slide → layout → master → theme. Placeholders take their place, geometry, fill,
//     line and text body settings from the layout's and master's placeholder of the same index or
//     type, and their text formatting from the list styles of the shape, the layout, the master
//     and the presentation. The master's and layout's own shapes are drawn under the slide's
//     (showMasterSp). Placeholders' prompt text ("Click to add title") is never drawn, as in
//     PowerPoint's slide show and PDF.
//   - Shapes, pictures, groups, charts and SmartArt's stored drawing through fwdraw (DrawingML,
//     the preset geometry, pictures and effects), text through fwtext's line layout, tables here.
//   - Autofit uses the scale PowerPoint stored (normAutofit fontScale and lnSpcReduction).
//
// A SlideRender is prepared once (shapes read, text laid out) and then drawn as often as needed:
// thumbnails, the slide view at any zoom, the slide show, printing and PDF. It keeps pointers into
// the source's XML, so the model must outlive it.
namespace fw::slide::render {

// What a slide (or a notes page) is drawn from. The presentation model fills it in; PackageDeck
// below reads it straight from a package (fwtool, tests).
struct SlideSource {
  // The parts' root elements: p:sld (or p:notes), its p:sldLayout (null for notes pages) and its
  // p:sldMaster (or p:notesMaster).
  pugi::xml_node slide, layout, master;
  // Pictures, charts and SmartArt through each part's own relationships. The renderer uses only
  // GetImage and GetPartXml: colors come from `theme` and `colorMap` below.
  draw::DrawingResources* slideResources = nullptr;
  draw::DrawingResources* layoutResources = nullptr;
  draw::DrawingResources* masterResources = nullptr;
  std::shared_ptr<const draw::Theme> theme;  // null: Office's default theme
  // The slide's color map: its override (p:clrMapOvr), else its layout's, else the master's
  // p:clrMap. Everything drawn on the slide uses it, the master's shapes too.
  draw::ColorMap colorMap;
  pugi::xml_node defaultTextStyle;  // p:presentation/p:defaultTextStyle
  pugi::xml_node tableStyles;       // ppt/tableStyles.xml's a:tblStyleLst
  draw::Size size{720, 540};        // p:sldSz (notes pages: p:notesSz), points
  int number = 1;                   // what a slide-number field shows (firstSlideNum applied)
};

// For drawing a slide piece by piece (the slide show animates shapes and paragraphs as sprites
// over the rest). Shapes are named by their id (cNvPr id); groups count, and skipping a group
// skips its children. Every function says whether to draw that piece.
class DrawFilter {
 public:
  virtual ~DrawFilter() = default;
  // The shape and everything in it.
  virtual bool Shape(uint32_t id) {
    (void)id;
    return true;
  }
  // The shape's own fill, outline, picture and shadow, apart from its text.
  virtual bool ShapeBody(uint32_t id) {
    (void)id;
    return true;
  }
  // Paragraph `index` (from 0, in the shape's text body or a table's cells in reading order).
  virtual bool Paragraph(uint32_t id, int index) {
    (void)id;
    (void)index;
    return true;
  }
  // The slide's background (the master's and layout's shapes count as shapes: id 0 for all).
  virtual bool Background() { return true; }
};

// One shape on the slide, as drawn: for hit testing, hyperlinks and animation extents.
struct ShapeInfo {
  uint32_t id = 0;  // cNvPr id; 0 for the master's and layout's shapes
  std::string name;
  draw::Size size;            // its own size, points
  draw::Matrix transform;     // its own coordinates → the slide's (points), groups and rotation applied
  draw::Rect bounds;          // the transformed box, slide points
  bool group = false;
  bool placeholder = false;
  bool fromMaster = false;    // drawn from the layout or master
  pugi::xml_node node;        // the shape's element in the slide (p:sp, p:pic…); null for inherited shapes
};

// A paragraph's place, for animations by paragraph.
struct ParagraphInfo {
  uint32_t shape = 0;
  int index = 0;
  draw::Rect bounds;  // slide points (the box around its lines after the shape's transform)
};

// A run with a hyperlink (a:hlinkClick on a:rPr), where its text is drawn.
struct LinkArea {
  uint32_t shape = 0;
  draw::Rect bounds;      // slide points
  std::string relId;      // r:id: the slide's relationship (empty for actions without a target)
  std::string action;     // the action attribute (ppaction://hlinksldjump…), may be empty
};

class SlideRender {
 public:
  ~SlideRender();
  SlideRender(const SlideRender&) = delete;
  SlideRender& operator=(const SlideRender&) = delete;

  // Reads the slide's shapes and lays its text out. `fonts` must outlive the call only.
  static std::unique_ptr<SlideRender> Make(const SlideSource& source, text::Fonts& fonts);

  draw::Size Size() const { return size_; }
  // Draws the slide into `frame` (its size: the slide's, in the painter's coordinates; a different
  // size scales it). Without a filter, everything.
  void Draw(draw::Painter& painter, const draw::Rect& frame, DrawFilter* filter = nullptr) const;

  const std::vector<ShapeInfo>& Shapes() const { return shapes_; }
  const std::vector<ParagraphInfo>& Paragraphs() const { return paragraphs_; }
  const std::vector<LinkArea>& Links() const { return links_; }
  // Fonts the slide asked for that aren't on this PC, and what was drawn instead.
  const std::vector<std::wstring>& MissingFonts() const { return missingFonts_; }
  // The slide's plain text, shape by shape and paragraph by paragraph (find, copy, notes).
  const std::wstring& Text() const { return text_; }

 private:
  SlideRender();
  struct Impl;
  std::unique_ptr<Impl> impl_;
  draw::Size size_;
  std::vector<ShapeInfo> shapes_;
  std::vector<ParagraphInfo> paragraphs_;
  std::vector<LinkArea> links_;
  std::vector<std::wstring> missingFonts_;
  std::wstring text_;
  friend struct Builder;
  friend class Hooks;
};

// Draws a prepared slide into a new bitmap `width` x `height` pixels (premultiplied BGRA), on
// Direct2D's software renderer: thumbnails and tests, no device needed. COM must be initialized
// on the thread. Null when it fails.
std::shared_ptr<const draw::Bitmap> RenderBitmap(const SlideRender& slide, int width, int height);

// Thumbnails, rendered at idle and kept as bitmaps (plan §8.3). The view calls RenderNext from its
// idle handler while it returns true; after the last one nothing runs until something changes.
class Thumbnails {
 public:
  // Prepares slide `index` for drawing (null: draw it blank).
  using MakeSlide = std::function<std::unique_ptr<SlideRender>(size_t index)>;
  explicit Thumbnails(MakeSlide make);
  // Every thumbnail again, `width` x `height` pixels (a new deck, size or DPI).
  void Reset(size_t count, int width, int height);
  // One slide changed.
  void Invalidate(size_t index);
  // These slides first (the visible ones).
  void Prefer(size_t first, size_t last);
  // Renders the next thumbnail due. False when none is left.
  bool RenderNext();
  bool Done() const;
  // Null until it has been rendered.
  std::shared_ptr<const draw::Bitmap> Get(size_t index) const;

 private:
  MakeSlide make_;
  std::vector<std::shared_ptr<const draw::Bitmap>> bitmaps_;
  std::vector<bool> due_;
  size_t first_ = 0, last_ = 0;
  int width_ = 0, height_ = 0;
};

}  // namespace fw::slide::render
