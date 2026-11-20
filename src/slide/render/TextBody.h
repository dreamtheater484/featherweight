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

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// A text body no model shape owns (SmartArt's stored drawing): its own paragraph and run
// properties over the defaults, and the shape style's font reference.
std::unique_ptr<fw::slide::TextBody> PlainTextBody(pugi::xml_node txBody, const draw::TextBox& box,
                                                   const draw::ColorContext& colors);

}  // namespace fw::slide::render
