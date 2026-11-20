#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "draw/Drawing.h"
#include "draw/Painter.h"
#include "slide/Presentation.h"

namespace fw::text {
class Fonts;
}

// Slide rendering (plan §8.3, P1): a slide drawn as PowerPoint draws it, at any size, through a
// draw::Painter, so the slide view (Direct2D), the slide show, printing and Export PDF show the
// same slide.
//
//   - What to draw comes from the presentation model (fwslide): the background, the master's and
//     layout's own shapes, the slide's shapes with their placeholders' inheritance merged, and
//     every text body's paragraphs and runs resolved.
//   - Shapes, pictures, groups, charts and SmartArt's stored drawing through fwdraw (DrawingML,
//     the preset geometry, pictures and effects); text through fwtext's line layout, with the
//     autofit scale PowerPoint stored (normAutofit fontScale and lnSpcReduction); tables here.
//   - Empty placeholders draw nothing of their text: their prompt ("Click to add title") never
//     shows, as in PowerPoint's slide show and PDF.
//
// A SlideRender is prepared once (shapes read, text laid out) and then drawn as often as needed:
// thumbnails, the slide view at any zoom, the slide show, printing and PDF. It points into the
// presentation, which must outlive it. Drawing is safe from any one thread at a time.
namespace fw::slide::render {

// For drawing a slide piece by piece: the slide show animates shapes and paragraphs as sprites
// over the rest. Shapes are named by their id (cNvPr id); groups count, and skipping a group skips
// its children. The master's and layout's own shapes all have id 0 here.
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
  // Paragraph `index` (from 0: the shape's text body's, or a table's cells' in reading order).
  virtual bool Paragraph(uint32_t id, int index) {
    (void)id;
    (void)index;
    return true;
  }
  virtual bool Background() { return true; }
};

// One shape as drawn: for hit testing, links and animation extents. Slide points.
struct ShapeInfo {
  uint32_t id = 0;               // cNvPr id; 0 for the master's and layout's shapes
  const fw::slide::Shape* shape = nullptr;  // the model's shape: its name, placeholder, links
  draw::Size size;               // its own size
  draw::Matrix transform;        // its own coordinates → the slide's (groups and rotation applied)
  draw::Rect bounds;             // the transformed box
  bool group = false;
  bool fromMaster = false;       // the master's or the layout's own shape
};

// A paragraph's place, for animations by paragraph.
struct ParagraphInfo {
  uint32_t shape = 0;
  int index = 0;
  draw::Rect bounds;  // slide points: the box around its lines
};

// Where a run with a hyperlink (a:hlinkClick on its a:rPr) is drawn.
struct LinkArea {
  uint32_t shape = 0;
  draw::Rect bounds;  // slide points
  fw::slide::Hyperlink link;
};

class SlideRender {
 public:
  ~SlideRender();
  SlideRender(const SlideRender&) = delete;
  SlideRender& operator=(const SlideRender&) = delete;

  // Reads the slide's (or notes page's) shapes from the model and lays its text out. `fonts` is
  // used during the call only (one per thread).
  static std::unique_ptr<SlideRender> Make(fw::slide::Presentation& deck, const fw::slide::SlideBase& slide,
                                           text::Fonts& fonts);

  draw::Size Size() const { return size_; }
  // Draws the slide into `frame` (the slide's size in the painter's coordinates; a different size
  // scales it). Without a filter, everything.
  void Draw(draw::Painter& painter, const draw::Rect& frame, DrawFilter* filter = nullptr) const;

  const std::vector<ShapeInfo>& Shapes() const { return shapes_; }
  const std::vector<ParagraphInfo>& Paragraphs() const { return paragraphs_; }
  const std::vector<LinkArea>& Links() const { return links_; }
  // The slide's own text, shape by shape and paragraph by paragraph, without bullets (find, copy).
  const std::wstring& Text() const { return text_; }

 private:
  SlideRender();
  struct Impl;
  std::unique_ptr<Impl> impl_;
  draw::Size size_;
  std::vector<ShapeInfo> shapes_;
  std::vector<ParagraphInfo> paragraphs_;
  std::vector<LinkArea> links_;
  std::wstring text_;
  friend struct Builder;
  friend class Hooks;
};

// The fonts the deck embeds, for `fonts` to use where this PC lacks them. Before anything is laid
// out with `fonts`.
void UseDeckFonts(text::Fonts& fonts, fw::slide::Presentation& deck);

// Draws a prepared slide into a new bitmap `width` x `height` pixels (premultiplied BGRA) on
// Direct2D's software renderer: thumbnails and tests, no device needed. COM must be initialized
// on the thread. Null when it fails.
std::shared_ptr<const draw::Bitmap> RenderBitmap(const SlideRender& slide, int width, int height, DrawFilter* filter = nullptr);

// Thumbnails, rendered at idle and kept as bitmaps (plan §8.3). The view calls RenderNext from its
// idle handler while it returns true; after the last one nothing runs until something changes.
class Thumbnails {
 public:
  // Prepares slide `index` for drawing (null: leave it blank).
  using MakeSlide = std::function<std::unique_ptr<SlideRender>(size_t index)>;
  explicit Thumbnails(MakeSlide make);
  // Every thumbnail again, `width` x `height` pixels (a new deck, size or DPI).
  void Reset(size_t count, int width, int height);
  // One slide changed.
  void Invalidate(size_t index);
  // These slides first (the visible ones).
  void Prefer(size_t first, size_t last);
  // Renders the next thumbnail due; whether more are due.
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
