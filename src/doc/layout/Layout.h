#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/LayoutDump.h"
#include "doc/model/Document.h"
#include "text/LineLayout.h"

namespace draw {
class Painter;
}
namespace fw {
class OpenTimings;
}
namespace fw::text {
class Fonts;
}
namespace fw::doc::edit {
struct Change;
}

// Pagination: a Document's stories laid out on pages, the way Word lays them out (plan §6.3
// "Layout", milestone D1). Lines come from fwtext; this places them in sections, columns, headers
// and footers, footnotes and endnotes, table cells and text boxes, around floating drawings, with
// Word's keep and widow/orphan rules and its page and section breaks.
//
// It's incremental: the document is laid out a page at a time, so the first page can be shown
// before the rest exist. LayOutPages(1) lays out the first page; Continue() lays out more in idle
// time until Done(). Pages already laid out never change while later ones are added, except their
// NUMPAGES and SECTIONPAGES fields, which are filled in again once the page count is known
// (Revision() changes when that happens).
//
// And after an edit (D2, plan §4), Edited() lays out again only what the change moves: from the
// page before the change, until a page ends where it ended before, past the change; the pages
// after that are kept as they are. Until they're laid out again, the old pages are still shown.
//
// Coordinates are page coordinates in points (1/72 inch): from the page's top-left corner, y down.
// Single-threaded: use it, the Document and the Fonts on one thread.
namespace fw::doc::layout {

struct Rect {
  float left = 0, top = 0, right = 0, bottom = 0;
  float Width() const { return right - left; }
  float Height() const { return bottom - top; }
  bool Contains(float x, float y) const { return x >= left && x < right && y >= top && y < bottom; }
};

// A place in the text: a paragraph (an index into Document's paragraphs, as Para(i)) and a UTF-16
// offset into its text (Document::Para(i).text). The story is the paragraph's.
struct TextPos {
  uint32_t para = 0;
  uint32_t offset = 0;
  auto operator<=>(const TextPos&) const = default;
};

// How one fwtext run of a line is painted: what the glyphs themselves don't say. Indexed by
// text::GlyphPiece::run.
struct RunPaint {
  uint32_t color = 0;  // 0xRRGGBB; "auto" is already resolved against the background
  doc::Underline underline = doc::Underline::None;
  uint32_t underlineColor = 0;
  bool strike = false, doubleStrike = false;
  bool highlight = false;  // w:highlight or run shading behind the text
  uint32_t highlightColor = 0;
  // Positions below the baseline (positive down) and thicknesses, in points, from the run's font.
  float underlineOffset = 0, underlineThickness = 0;
  float strikeOffset = 0, strikeThickness = 0;  // strikeOffset is above the baseline (positive up)
  uint32_t modelRun = doc::kNone;  // index into Document::Runs(para): hyperlink, revision, field
};

// What a line is part of, for the dump and for painting. Nesting is resolved as the dump does it:
// header and footer win over notes, notes over text boxes, text boxes over cells (docs/layout-dump.md).
using LineStory = fw::LayoutStory;

// One line on a page.
struct LineBox {
  uint32_t para = doc::kNone;  // whose text
  LineStory story = LineStory::Body;
  // The line itself, from fwtext. Its x coordinates (pieces, objects, carets) are relative to
  // `x`; its text offsets are in the line's own text, which is the paragraph's text after
  // `prefix` characters (the list label and what follows it). Fields whose result layout computes
  // (PAGE, NUMPAGES, SECTIONPAGES) make the rest differ: use Layout's HitTest and CaretAt, which
  // map offsets back to TextPos, rather than the offsets directly.
  text::Line line;
  float x = 0;         // where the line's column coordinate 0 is on the page
  float top = 0;       // the top of the line's box
  float height = 0;    // the line's advance (its line-height rule applied)
  float baseline = 0;  // page coordinates
  uint32_t prefix = 0; // characters of label before the paragraph's own text
  uint32_t paint = 0;  // first of the paragraph's RunPaints in Page::paints (index + piece.run)
  uint32_t drawing = doc::kNone;  // text boxes: the drawing (Document::Drawings()) it's in
  Rect clip;           // text boxes and exact-height rows: where the line may be drawn; empty: anywhere
  // Vertical text (table cells with w:textDirection btLr or tbRl): the line is laid out as if
  // horizontal, in a frame turned `turn` quarter turns clockwise about (turnX, turnY) on the page,
  // and `x`, `top` and `baseline` are in that frame. 0: an ordinary line, in page coordinates.
  // HitTest and Ranges turn with it; a caret across such a line is horizontal, so CaretAt gives
  // where it starts, with top == bottom.
  uint8_t turn = 0;
  float turnX = 0, turnY = 0;
  // What the line shows, in logical order: hidden text left out, a tab as 	, the hyphen of a
  // hyphenated break added. For copying, find and the layout dump.
  std::u16string text;
};

// A drawing on a page: a picture, shape, text box frame, chart or embedded object. fwdraw draws it
// from Document::DrawingXml(drawing) into `rect`; until it does, Paint() draws a placeholder.
struct DrawingBox {
  uint32_t drawing = doc::kNone;  // index into Document::Drawings()
  Rect rect;                      // its extent (wp:extent), page coordinates
  bool floating = false;          // anchored (wp:anchor or absolute VML), not in a line
  bool behindText = false;        // drawn before the text
  int64_t z = 0;                  // floating: relativeHeight, the z-order among floating drawings
};

// A filled rectangle behind text: table cell and paragraph shading, highlighted lines are RunPaint.
struct Fill {
  Rect rect;
  uint32_t color = 0;  // 0xRRGGBB
};

// One border edge, after conflicts between neighboring cells are resolved: a line from (x1, y1)
// to (x2, y2) along the middle of the edge, drawn in the border's style and width.
struct Edge {
  doc::Border border;
  float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

// A table cell's piece on a page, as drawn: a cell split across pages has one on each, and a
// merged cell (vMerge) one in each row it spans.
struct CellRect {
  ElemId tc = kNoElem, tbl = kNoElem;
  uint32_t row = 0;           // the row's index in its table
  int32_t col = 0, span = 1;  // the grid columns it covers
  Rect box;                   // its border box, the row's full height (page points)
};

struct Page {
  float width = 0, height = 0;  // points; landscape sections already swapped
  uint32_t section = 0;         // index into Document::Sections()
  int32_t number = 1;           // the page number fields show
  std::u16string numberText;    // the same, in the section's format ("iv", "4")
  Rect body;                    // the page's text area (margins, header and footer space excluded)
  std::vector<Fill> fills;
  std::vector<Edge> edges;
  std::vector<DrawingBox> drawings;  // behind-text ones first, then by z
  std::vector<LineBox> lines;        // in reading order per story
  std::vector<RunPaint> paints;      // LineBox::paint indexes this
  std::vector<CellRect> cells;       // table cells, a nested table's after the cell it's in
  TextPos first;                     // the first body text on the page
};

struct Options {
  bool showHiddenText = false;  // w:vanish text laid out and drawn (formatting marks shown)
  // Formatting marks drawn over the text (Ctrl+Shift+8): paragraph marks, spaces, tabs and breaks.
  // Hidden text is shown with them, so set showHiddenText too.
  bool formattingMarks = false;
  // The moment DATE and TIME fields show, as Word updates them when it opens a document: a FILETIME
  // (100-nanosecond ticks since 1601, UTC), shown in local time. 0 keeps the results the file holds
  // (the tools' layouts, which must not depend on the day).
  int64_t now = 0;
};

// Which lines Layout::HitTest picks from: all, the body's (with notes, cells and text boxes), or
// the headers' and footers' (for editing them in place).
enum class HitStories : uint8_t { All, Body, HeaderFooter };

// The family Word draws for one a document names but the PC doesn't have, from the document's font
// table: fixed pitch is Courier New; a sans-serif panose number or the "swiss" family is Arial; a
// serif one or "roman" is Times New Roman; "modern" with variable pitch is Tahoma (measured on
// Word's renders of the corpus). Empty when the table says nothing useful, or for families fwtext's
// own table knows (Aptos, Helvetica...). For text::Fonts' SubstituteFn.
std::wstring SubstituteFamily(const Document& doc, std::wstring_view family);

// What a DATE or TIME field (its instruction, `code`) shows at `now` (as Options::now): by its \@
// picture, as Word reads one, in the language `lang` (BCP 47; the user's when empty or unknown).
// Empty for other fields, or when `now` is 0.
std::u16string DateFieldResult(std::u16string_view code, int64_t now, std::string_view lang);

// Draws a drawing into its box with the painter. fwdraw provides it (plan §4.5); without one,
// Paint() draws a light frame where the drawing goes.
using DrawingPainter = std::function<void(draw::Painter&, const DrawingBox&)>;

class Layout {
 public:
  // `doc` and `fonts` must outlive the layout. `timings` (optional) gets the FirstPage mark when
  // the first page is done and Paginated when the last is.
  Layout(const Document& doc, text::Fonts& fonts, Options options = {}, OpenTimings* timings = nullptr);
  ~Layout();
  Layout(const Layout&) = delete;
  Layout& operator=(const Layout&) = delete;

  // ---- Laying out
  // Lays out until there are at least `count` pages, or the document ends.
  void LayOutPages(size_t count);
  // Lays out for about `milliseconds` (at least one page), for idle time. True while more remain.
  bool Continue(double milliseconds);
  // Everything at once.
  void LayOutAll() { LayOutPages(SIZE_MAX); }
  bool Done() const;

  // ---- Editing (D2)
  // The document changed: an edit::Editor change notification (Editor::OnChange). The pages from
  // the one before the change on are laid out again as LayOutPages and Continue ask for them.
  // Until then they're shown as they were (PageCount and GetPage include them), with paragraph
  // indices moved along, so HitTest on them stays valid; Done() is false. For typing: Edited, then
  // LayOutPages(last page on screen + 1) and paint; Continue() lays out the rest in idle time.
  void Edited(const edit::Change& change);

  // ---- Pages
  // Laid out so far (final once Done()), then after an edit the old pages not laid out again yet.
  size_t PageCount() const;
  const Page& GetPage(size_t index) const;
  // Changes whenever pages already returned change: their NUMPAGES fields once the count is known,
  // or an edit.
  uint32_t Revision() const;

  // ---- Selection and find. Positions on pages not laid out yet are laid out first.
  // The text position nearest to (x, y) on a page: the line under or nearest the point, then the
  // nearest caret stop on it. nullopt on a page without text.
  std::optional<TextPos> HitTest(size_t page, float x, float y, HitStories which = HitStories::All) const;
  struct Caret {
    size_t page = 0;
    float x = 0, top = 0, bottom = 0;
  };
  // Where the caret before `pos` is drawn; nullopt for text that isn't shown (hidden, or in a story
  // not on any page, such as an unused header or a comment). A header or footer is on many pages:
  // `page` says which one to show it on (the first it's on, when it isn't there).
  std::optional<Caret> CaretAt(TextPos pos, size_t page = SIZE_MAX);
  // The rectangles that [from, to) covers, page by page, for the selection and find highlights.
  // Both positions are in one story; table cells are covered in reading order. For a header or
  // footer, `page` names the page (else the first it's on).
  std::vector<std::pair<size_t, Rect>> Ranges(TextPos from, TextPos to, size_t page = SIZE_MAX);
  // The page `pos` is on (the first, for a story repeated on many pages, such as a header).
  std::optional<size_t> PageOf(TextPos pos);
  // The table cells on a page (Page::cells), laying it out first when it isn't yet: for dragging
  // column borders and row heights, and selecting rows and columns (D3).
  std::vector<CellRect> Cells(size_t page);
  // Whether CaretAt and Ranges for `pos` answer without laying out more than a page: its main-text
  // paragraph is on a page laid out since the last edit (other stories always are). What's drawn
  // each frame asks first, so a change of the page (D3) lays out in idle time, not in a paint.
  bool LaidOut(TextPos pos) const;
  // The line `pos` is on: its page, its box, and where Home and End take the caret (the end of a
  // wrapped line is before its last character, so the caret stays on the line).
  struct LineSpan {
    size_t page = 0;
    TextPos start, end;
    Rect box;
  };
  std::optional<LineSpan> LineAt(TextPos pos);
  // The place nearest to `x` (page points) on the line before (`step` < 0) or after `pos`'s line
  // in its story's reading order, across pages for the main text; nullopt at either end.
  std::optional<TextPos> LineStep(TextPos pos, int step, float x);

  // ---- Painting
  // Paints a page at its own coordinates (the caller's transform scales it): page color and
  // borders, behind-text drawings, fills, text, borders, then the other drawings.
  void Paint(draw::Painter& painter, size_t page, const DrawingPainter& drawing = {}) const;

  // ---- Measuring
  // The layout dump of the pages so far (docs/layout-dump.md); `source` names the document as the
  // reference manifest does.
  LayoutDump Dump(std::string source) const;
  size_t MemoryBytes() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fw::doc::layout
