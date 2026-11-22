#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "draw/Drawing.h"
#include "draw/Painter.h"
#include "draw/Theme.h"
#include "slide/Text.h"
#include "slide/render/SlideRender.h"

namespace fw::text {
class Fonts;
}

// A resolved text body (fwslide's TextBody) laid out as PowerPoint lays it out: bullets and
// automatic numbers, indents, tabs, line and paragraph spacing, anchoring, columns, vertical
// text and the stored autofit scale, on fwtext's line breaker. Internal to the slide renderer.
namespace fw::slide::render {

struct TextExtras {
  draw::ColorContext colors;  // the slide's: hyperlinks take its hlink color
  // A picture bullet: the image of relationship `rel` of part `part`; null when there's none.
  std::function<std::shared_ptr<const draw::Image>(const std::string& part, const std::string& rel)> picture;
  // Editing: this empty paragraph shows its bullet (the caret is in it), as PowerPoint's editor does.
  int bulletEmptyParagraph = -1;
};

class TextBlock {
 public:
  TextBlock();
  ~TextBlock();
  TextBlock(TextBlock&&) noexcept;
  TextBlock& operator=(TextBlock&&) noexcept;

  // Lays `body` out in the text box (its rect, flow, anchor, columns and autofit).
  void Build(text::Fonts& fonts, const draw::TextBox& box, const fw::slide::TextBody& body, const TextExtras& extras);
  // Draws it with the painter in the box's coordinates (its shape's own). Paragraphs the filter
  // turns down are left out.
  void Draw(draw::Painter& painter, DrawFilter* filter, uint32_t shape) const;

  // The height the text needs (insets not included), for table rows.
  float ContentHeight() const;
  // Each paragraph's box, in the box's coordinates.
  std::vector<draw::Rect> ParagraphBounds() const;
  struct Link {
    draw::Rect bounds;  // the box's coordinates
    fw::slide::Hyperlink link;
  };
  std::vector<Link> Links() const;
  // The text without bullets: paragraphs and line breaks as '\n'.
  std::wstring PlainText() const;

  // ---- Editing (P2, p2-text) ----
  // Positions are (paragraph, offset): the offset counts the paragraph's runs as the model resolved
  // them (a run's text, 1 for a line break, a field's text as shown), without the bullet. Points are
  // in "text coordinates" unless said: ToSlide maps them where the block is drawn.
  struct Caret {
    float x = 0, top = 0, bottom = 0;  // text coordinates
  };
  // The text coordinates -> the slide's, for a box drawn with `boxToSlide` (what the painter's
  // transform is when Draw runs: the box's transform and its place). Mirrored shapes keep their text
  // readable, as Draw does.
  draw::Matrix ToSlide(const draw::Matrix& boxToSlide) const;
  size_t ParagraphCount() const;
  uint32_t ParagraphLength(size_t para) const;  // offsets laid out (a very long body may be cut)
  // The caret before `offset`. At a wrapped line's end, `lineEnd` puts it at the end of the line
  // above rather than the start of the next.
  Caret CaretAt(size_t para, uint32_t offset, bool lineEnd = false) const;
  // The position nearest a point (text coordinates); `lineEnd` says it's at the end of a wrapped line.
  struct Hit {
    size_t para = 0;
    uint32_t offset = 0;
    bool lineEnd = false;
  };
  Hit HitTest(draw::Point text) const;
  // The line holding a position: its start and end offsets (the end before a line break's
  // character, else where the next line starts).
  std::pair<uint32_t, uint32_t> LineRange(size_t para, uint32_t offset, bool lineEnd = false) const;
  // The position on the line above (direction < 0) or below nearest `x` (text coordinates); false
  // at the first or last line.
  bool Vertical(size_t para, uint32_t offset, bool lineEnd, int direction, float x, Hit& out) const;
  // Whether a caret may stop at the offset (not inside a surrogate pair or a grapheme).
  bool IsStop(size_t para, uint32_t offset) const;
  // The rectangles [from, to) covers, text coordinates, line by line. A selection going past a
  // paragraph's end shows a little of its mark.
  std::vector<draw::Rect> SelectionRects(size_t fromPara, uint32_t fromOffset, size_t toPara, uint32_t toOffset) const;
  // Where the text starts and ends vertically (text coordinates), for autofit.
  float TextHeight() const { return ContentHeight(); }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fw::slide::render
