#pragma once

#include <d2d1_1.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "draw/Painter.h"
#include "slide/Text.h"

namespace fw::slide {
class Presentation;
}

// What the slide show draws slides with (plan §8.3): p1-render's slides (slide/render) behind it. The
// show draws a slide whole, or in parts for its animation layers and sprites, so drawing takes a
// filter.
namespace fw::slide::show {

// What a draw leaves out. Items are the slide's shapes bottom to top, by index: item 0 is its master's
// and layout's shapes together (they never animate), then the slide's own top-level shapes.
class DrawFilter {
 public:
  virtual ~DrawFilter() = default;
  virtual bool Background() const { return true; }
  virtual bool Item(size_t /*item*/) const { return true; }  // the whole shape
  // Its geometry, fill and outline; its text goes by paragraph.
  virtual bool Body(size_t /*item*/) const { return true; }
  virtual bool Paragraph(size_t /*item*/, int /*paragraph*/) const { return true; }
};

struct ItemInfo {
  uint32_t spid = 0;     // the cNvPr id of one of the slide's own shapes; 0 for the master's and layout's
  draw::Rect bounds;     // the shape's box, slide points
  draw::Rect ink;        // what it paints (shadows, text past the box), slide points
  int paragraphs = 0;    // in its text
  const Hyperlink* click = nullptr;  // the shape's own link
};

class SlideRenderer {
 public:
  virtual ~SlideRenderer() = default;
  virtual size_t SlideCount() const = 0;
  virtual draw::Size SlideSize() const = 0;  // points
  // The slide's items, bottom to top.
  virtual const std::vector<ItemInfo>& Items(size_t slide) = 0;
  // A paragraph's box, slide points: what by-paragraph animations move and wipe.
  virtual draw::Rect ParagraphBox(size_t slide, size_t item, int paragraph) = 0;
  // Draws the slide; `toTarget` maps slide points to the target's pixels. With a filter, only what it
  // lets through. Between the context's BeginDraw and EndDraw.
  virtual void Draw(ID2D1DeviceContext* dc, size_t slide, const D2D1_MATRIX_3X2_F& toTarget,
                    const DrawFilter* filter) = 0;
  // The link in a shape's text at a point (slide points), or null.
  virtual const Hyperlink* TextLinkAt(size_t /*slide*/, size_t /*item*/, draw::Point /*pt*/) { return nullptr; }
  // The device was lost: drop what was made with it.
  virtual void ReleaseDevice() {}
};

// The slides as p1-render draws them (slide/render/SlideRender.h), a few prepared at a time; the
// master's and layout's shapes are item 0. The presentation must outlive it.
std::unique_ptr<SlideRenderer> MakeRenderer(Presentation& deck);

}  // namespace fw::slide::show
