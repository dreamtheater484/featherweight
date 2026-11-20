#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "draw/Painter.h"

// The presentation tab's geometry, headless so tests can check it: the slide fitted into the
// view, the thumbnails list, and the pages that print and export (slides, handouts, notes pages).
namespace fw::slide::view {

// `content` scaled to fit inside `area` (keeping its proportions), centered; at most `maxScale`.
draw::Rect FitRect(draw::Size content, const draw::Rect& area, float maxScale = 1e9f);

// The thumbnails pane: one row per slide, the slide's number left of its picture.
struct ThumbRow {
  draw::Rect number;  // where the number goes
  draw::Rect slide;   // the picture
  draw::Rect row;     // the whole row (hover, selection)
};
struct ThumbMetrics {
  float width = 0;   // the pane, device pixels
  float ui = 1;      // device pixels per DIP
};
// Rows for `count` slides of size `slide` in a pane `m.width` wide, from y = 0 down (unscrolled).
std::vector<ThumbRow> ThumbRows(size_t count, draw::Size slide, const ThumbMetrics& m);
// The list's whole height.
float ThumbListHeight(const std::vector<ThumbRow>& rows, float ui);

// ---- Printing and Export PDF (plan §8.1: slides, handouts, notes pages) ----

enum class PrintWhat { Slides, Handouts, Notes };

struct PrintSettings {
  PrintWhat what = PrintWhat::Slides;
  int perPage = 6;            // handouts: 1, 2, 3, 4, 6 or 9
  bool includeHidden = false; // hidden slides print only when asked, as in PowerPoint
  bool frame = true;          // a thin frame around each slide on handouts and notes pages
};

// The handout counts PowerPoint offers.
constexpr int kHandoutCounts[] = {1, 2, 3, 4, 6, 9};
bool ValidHandoutCount(int n);

// One printed or exported page: its size, and what goes where (page points).
struct PageItem {
  size_t slide = 0;
  draw::Rect rect;     // the slide's picture
  draw::Rect notes;    // notes pages: the notes text below it; handouts of 3: the lines beside it
};
struct PrintPage {
  draw::Size size;
  std::vector<PageItem> items;
  int number = 0;      // 1-based, for the page number in a handout's or notes page's corner
};

// The pages for `slides` (indices into the deck, in order). `slideSize` is the slide's, `paper`
// the page's for handouts and notes pages (the notes page size, or the printer's paper); slides
// print on pages `slidePaper` big (the slide size for PDF, the paper when printing) with the
// slide fitted on them.
std::vector<PrintPage> LayoutPages(const std::vector<size_t>& slides, draw::Size slideSize, draw::Size slidePaper,
                                   draw::Size paper, const PrintSettings& settings);

// The slots on a handout page of `perPage` slides (in reading order: left to right, then down), and
// for 3 per page the ruled area beside each slot.
std::vector<PageItem> HandoutSlots(int perPage, draw::Size page, draw::Size slide);

// A notes page: the slide's picture at the top, the notes below, as PowerPoint's default notes
// master places them.
PageItem NotesSlots(draw::Size page, draw::Size slide);

// ---- Find ----

// Where `query` occurs in `text` (offsets, in order, not overlapping), compared case-insensitively
// unless `matchCase`; with `wholeWords`, only where no letter or digit is just before or after.
std::vector<size_t> FindAll(std::wstring_view text, std::wstring_view query, bool matchCase, bool wholeWords);

}  // namespace fw::slide::view
