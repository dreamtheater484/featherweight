#pragma once

#include <memory>
#include <string>
#include <vector>

#include "draw/Painter.h"
#include "slide/Presentation.h"
#include "slide/render/SlideRender.h"
#include "slide/render/TextBody.h"

namespace fw::text {
class Fonts;
}

// A table (fwslide's Table, its style applied) laid out and drawn: cell fills, text, then borders.
// Rows grow to fit their text, as PowerPoint's do. Internal to the slide renderer.
namespace fw::slide::render {

class TableRender {
 public:
  TableRender(const fw::slide::Table& table, text::Fonts& fonts, const TextExtras& extras);
  // Draws it in its graphic frame's own coordinates.
  void Draw(draw::Painter& painter, DrawFilter* filter, uint32_t shape) const;
  draw::Size Extent() const { return {colX_.back(), rowY_.back()}; }
  // The cells' paragraphs in reading order, and links, in the frame's coordinates.
  std::vector<draw::Rect> ParagraphBounds() const;
  std::vector<TextBlock::Link> Links() const;
  std::wstring PlainText() const;
  // Editing (P2): a cell's text box in the frame's coordinates, and its paragraphs' place in the
  // table's count. False when there's no such cell (or it's covered by a merge).
  bool CellFrame(int row, int column, draw::TextBox& box, int& firstParagraph, int& paragraphs, float& contentHeight) const;
  // The cell under a point of the frame's coordinates.
  bool CellAt(draw::Point at, int& row, int& column) const;

 private:
  struct Cell {
    const fw::slide::TableCell* cell = nullptr;
    draw::Rect rect;
    TextBlock text;
  };
  std::vector<float> colX_, rowY_;  // edges
  std::vector<Cell> cells_;
  const fw::slide::Table& table_;
};

}  // namespace fw::slide::render
