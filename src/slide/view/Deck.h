#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "draw/Painter.h"

namespace fw::slide {
class Presentation;
}

// What the presentation tab reads from a deck (P1, plan §8): the slides' sizes, text, notes and
// links, and a painter call for each slide. It's p1-model's presentation and p1-render's renderer
// behind one small interface, so the tab, printing and Export PDF stay the same whichever draws
// the slides, and tests can pass a deck of their own.
namespace fw::slide::view {

// A link on a slide (a hyperlink on a shape or its text): a web address, or a place in the deck.
struct SlideLink {
  draw::Rect rect;    // where it can be clicked, slide points
  std::wstring url;   // http, https, mailto...: opened through the app's filter (util::OpenExternalLink)
  int slide = -1;     // a slide of the deck (0-based) when the link goes there instead
  std::wstring tooltip;
};

// One shape's text, for find and copy: its paragraphs separated by '\n'.
struct TextBlock {
  std::wstring text;
  draw::Rect bounds;  // the shape, slide points: what a find match highlights
};

class Deck {
 public:
  virtual ~Deck() = default;

  virtual size_t SlideCount() const = 0;
  virtual draw::Size SlideSize() const = 0;   // points
  // The notes page (portrait, 7.5 x 10 inches unless the file says otherwise), points.
  virtual draw::Size NotesSize() const { return {540, 720}; }
  virtual bool Hidden(size_t slide) const = 0;
  // The slide's shapes' text in reading order (empty shapes left out).
  virtual std::vector<TextBlock> Text(size_t slide) const = 0;
  virtual std::wstring Notes(size_t slide) const = 0;
  virtual std::vector<SlideLink> Links(size_t slide) const = 0;
  // A slide show file (.ppsx): it opens straight into the show.
  virtual bool StartsAsShow() const { return false; }
  // The deck carries macros (.pptm, .potm): kept, never run.
  virtual bool HasMacros() const { return false; }
  // The deck's title (docProps' title), or empty: the PDF's title.
  virtual std::wstring Title() const { return {}; }

  // Draws the slide in slide points, its top left at the origin (whatever transform the painter
  // has: the screen, a thumbnail, a printed page, a PDF).
  virtual void Paint(size_t slide, draw::Painter& p) = 0;
  // Draws the slide's notes page (notes size, its top left at the origin) as the deck's notes
  // master lays it out, without the slide's picture, and says where the picture goes (notes page
  // points). False when the deck can't (the tab then lays the page out itself).
  virtual bool PaintNotesPage(size_t /*slide*/, draw::Painter& /*p*/, draw::Rect& /*slideBox*/) { return false; }
  // The rectangles of characters [from, from + length) of text block `block`, slide points, for
  // highlighting a find match; empty when the renderer can't tell (the block's bounds are used).
  virtual std::vector<draw::Rect> TextRects(size_t /*slide*/, size_t /*block*/, size_t /*from*/,
                                            size_t /*length*/) {
    return {};
  }
  // p1-model's presentation behind the deck, for the slide show; null for a deck without one (tests).
  virtual Presentation* Model() { return nullptr; }
  // Bytes the deck keeps (model, caches), for the memory budget.
  virtual size_t MemoryUse() const { return 0; }
};

}  // namespace fw::slide::view
