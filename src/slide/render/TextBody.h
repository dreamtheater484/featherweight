#pragma once

#include <pugixml.hpp>

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "draw/Drawing.h"
#include "draw/Painter.h"
#include "draw/Theme.h"
#include "slide/render/SlideRender.h"
#include "text/LineLayout.h"

namespace fw::text {
class Fonts;
}

// DrawingML text bodies (a:txBody, p:txBody) as PowerPoint lays them out: the list-style chain
// (presentation, master text styles, the master's, the layout's and the shape's own a:lstStyle),
// bullets and automatic numbering, spacing, insets, anchoring, columns, vertical text and the
// stored autofit scale, on fwtext's line breaker. Internal to the slide renderer.
namespace fw::slide::render {

// What a text body inherits.
struct TextContext {
  // List-style containers, lowest priority first: p:defaultTextStyle, the master's p:titleStyle,
  // p:bodyStyle or p:otherStyle, then the master's and layout's placeholder a:lstStyle. The body's
  // own a:lstStyle comes last, from the body itself.
  std::vector<pugi::xml_node> lists;
  draw::ColorContext colors;
  std::optional<draw::Color> fontColor;  // p:style/a:fontRef's color
  std::string fontFace;                  // p:style/a:fontRef: "+mj-lt" or "+mn-lt"; empty: none
  int slideNumber = 1;
  draw::DrawingResources* resources = nullptr;  // picture bullets
  // Overrides for table cells: the table style's text (bold, italic, color, font).
  std::optional<bool> bold, italic;
  std::optional<draw::Color> color;
  std::string face;
};

class TextBlock {
 public:
  TextBlock();
  ~TextBlock();
  TextBlock(TextBlock&&) noexcept;
  TextBlock& operator=(TextBlock&&) noexcept;

  // Lays out the text box's body (box.content, box.properties, box.rect).
  void Build(text::Fonts& fonts, const draw::TextBox& box, const TextContext& ctx);
  // Draws it with the painter in the box's coordinates (its shape's own). Paragraphs the filter
  // turns down are left out.
  void Draw(draw::Painter& painter, DrawFilter* filter, uint32_t shape) const;

  // The height the text needs (insets not included), for table rows.
  float ContentHeight() const;
  bool Empty() const;
  // Each paragraph's box and each link's, in the box's coordinates (before the shape's
  // transform), and the plain text.
  std::vector<draw::Rect> ParagraphBounds() const;
  struct Link {
    draw::Rect bounds;
    std::string relId, action;
  };
  std::vector<Link> Links() const;
  std::wstring PlainText() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// "+mj-lt", "+mn-ea"… through the theme; other names as they are.
std::wstring ThemeFace(std::string_view typeface, const draw::Theme* theme);

}  // namespace fw::slide::render
