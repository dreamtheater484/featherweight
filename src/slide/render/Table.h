#pragma once

#include <pugixml.hpp>

#include <memory>
#include <string>
#include <vector>

#include "draw/Painter.h"
#include "slide/render/SlideRender.h"
#include "slide/render/TextBody.h"

namespace fw::text {
class Fonts;
}

// DrawingML tables (a:tbl in a graphic frame) with their table styles: the deck's
// ppt/tableStyles.xml, else PowerPoint's built-in style of that id. Internal to the slide renderer.
namespace fw::slide::render {

class Table {
 public:
  ~Table();
  // `frame`: the graphic frame's box (its own coordinates start at its top left). `text`: what the
  // cells' text inherits (the list styles and colors).
  static std::unique_ptr<Table> Read(pugi::xml_node tbl, draw::Size frame, pugi::xml_node tableStyles,
                                     const TextContext& text, text::Fonts& fonts);
  // Draws the table in the frame's own coordinates.
  void Draw(draw::Painter& painter, DrawFilter* filter, uint32_t shape) const;
  // The table's size as laid out: rows grow to fit their text.
  draw::Size Extent() const;
  // Each cell's paragraphs in reading order, in the frame's coordinates.
  std::vector<draw::Rect> ParagraphBounds() const;
  std::vector<TextBlock::Link> Links() const;
  std::wstring PlainText() const;

 private:
  Table();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// PowerPoint's built-in table style with this id ({5C22544A-…}), as an a:tblStyle element in a
// document the function keeps; null when the id isn't a built-in one.
pugi::xml_node BuiltInTableStyle(std::string_view id);

}  // namespace fw::slide::render
