#pragma once

#include <pugixml.hpp>

#include <string>
#include <vector>

#include "draw/Painter.h"
#include "draw/Theme.h"
#include "text/LineLayout.h"

// Text in a sheet's shapes and notes: paragraphs of styled runs, laid out with fwtext and drawn
// through a Painter. Small on purpose: no bullets, columns, vertical text or autofit.
namespace fw::sheet::objects {

struct RichSpan {
  std::wstring text;
  text::TextStyle style;
  draw::Color color;
  bool underline = false;
};

struct RichParagraph {
  std::vector<RichSpan> spans;
  text::Alignment alignment = text::Alignment::Start;
  text::TextStyle mark;  // an empty paragraph's size
};

// A shape's text body (a:txBody, xdr:txBody): its paragraphs, runs and line breaks, with sizes,
// bold, italic, underline, colors and fonts as the runs say, else `color`, `size` and the theme's
// minor font.
std::vector<RichParagraph> ReadShapeText(pugi::xml_node txBody, const draw::ColorContext& colors, draw::Color color,
                                         float size);

// Lays the paragraphs out `width` wide and returns their height. With a painter, also draws them
// with their top left at `origin`.
float LayoutText(text::Fonts& fonts, const std::vector<RichParagraph>& paragraphs, float width, draw::Painter* painter,
                 draw::Point origin);

}  // namespace fw::sheet::objects
