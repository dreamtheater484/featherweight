#pragma once

#include <string>
#include <string_view>

#include "draw/Painter.h"
#include "slide/view/Deck.h"
#include "slide/view/SlideLayout.h"

namespace fw::text {
class Fonts;
}

// Drawing the pages that print and export (headless): the same code for the printer, Export PDF
// and the tests' pictures.
namespace fw::slide::view {

// Paints `page` in page points with its top left at the origin.
void PaintPage(Deck& deck, const PrintPage& page, const PrintSettings& settings, text::Fonts& fonts,
               draw::Painter& p);

// Draws `text` (paragraphs separated by '\n') wrapped inside `box`, top down, in `family` at
// `size` points; lines below the box are left out. Returns the height used.
float DrawPlainText(text::Fonts& fonts, draw::Painter& p, std::wstring_view text, const draw::Rect& box, float size,
                    const std::wstring& family, draw::Color color, bool rightAligned = false);

}  // namespace fw::slide::view
