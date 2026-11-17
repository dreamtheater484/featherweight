#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <string>
#include <vector>

#include "draw/Painter.h"
#include "sheet/model/Workbook.h"
#include "sheet/objects/SheetObjects.h"
#include "sheet/view/CellPaint.h"
#include "sheet/view/CellText.h"
#include "sheet/view/Grid.h"

// A sheet on printed pages, as Excel paginates it (plan §7.2 "Printing"): the print area (or the
// used range), its manual page breaks and automatic ones, print titles repeated on every page,
// the scale or fit to pages, orientation, paper and margins, centering, headers and footers with
// their fields, gridlines and headings. Headless: pages are drawn through draw::Painter in points.
namespace fw::sheet::view {

struct PrintPage {
  float width = 612, height = 792;  // points, as the paper is turned
  Range body;                       // the cells on the page
  bool titleRows = false, titleCols = false;  // the print titles repeat on it
  int number = 1;                   // as printed (&P)
  bool chart = false;               // a chart sheet's page: its chart fills it
};

struct PrintInfo {
  std::wstring fileName;   // &F
  std::wstring path;       // &Z: the folder, with a trailing backslash
  std::wstring sheetName;  // &A
  std::wstring date, time; // &D and &T, as the locale writes them
  // Printing a whole workbook is one job: every sheet on the first one's paper (by its paperSize
  // code; each keeps its orientation), and &N counts all its pages. 0: the sheet's own.
  uint32_t jobPaper = 0;
  int jobPages = 0;
  // What sits on the sheet (pictures, shapes, charts, a chart sheet's chart): an instance of the
  // caller's own for printing, which the layout places on its printed columns and rows. May be null.
  SheetObjects* objects = nullptr;
};

class PrintLayout {
 public:
  // `sheetIndex`: the sheet's place in the workbook, for its print area and titles.
  PrintLayout(const Workbook& book, const Sheet& sheet, size_t sheetIndex, const Grid& grid, CellText& text,
              PrintInfo info);

  size_t PageCount() const { return pages_.size(); }
  const PrintPage& Page(size_t i) const { return pages_[i]; }
  float Scale() const { return scale_; }
  // Draws a page in points from the paper's top-left corner (white paper is the caller's).
  void Paint(draw::Painter& p, size_t page);

  // Paper sizes in points by SpreadsheetML's paperSize code (§18.3.1.63); Letter for unknown ones.
  static void PaperSize(uint32_t code, float& w, float& h);
  // The areas a defined name's formula covers on its sheet: "Sheet1!$A$1:$C$9,Sheet1!$E$1:$F$9".
  static std::vector<Range> ParseAreas(std::string_view formula);

 private:
  void Paginate();
  void PaintHeaderFooter(draw::Painter& p, const PrintPage& page, bool header);
  float ColsWidth(uint32_t first, uint32_t last) const;   // sheet points, at the page's scale
  // A column on paper at a scale, in points at 100 % (the page's scale brings it to size). Excel
  // lays a scaled page out in the font at its scaled size: a column is as many of that font's
  // digits wide as on screen (Calibri 11 at 84 % is 77 dots, with digits of 39).
  float ColPt(uint32_t col, float scale);
  float ColsAt(uint32_t first, uint32_t last, float scale);
  float RowsHeight(uint32_t first, uint32_t last) const;
  // The last column text in `used` runs into, past its last column too: Excel prints that text.
  uint32_t SpillEnd(const Range& used);
  // The last column a cell's text runs into (its own when it stays in its cell); `past` is a
  // column it can't reach without being long enough to get past it, to skip measuring most cells.
  uint32_t Reach(const CellInfo& c, uint32_t past);
  // Nothing on a page: no values, fills or borders, no pictures, and no text running into it.
  bool Blank(const Range& body);
  // The objects placed on the printed columns (colX_) and rows; the last row and column they reach.
  void PlaceObjects();
  std::optional<std::pair<uint32_t, uint32_t>> ObjectsEnd() const;
  draw::Rect SheetRect(const Range& r) const;  // cells on paper, in points at 100 % from A1

  const Workbook& book_;
  const Sheet& sheet_;
  size_t index_;
  const Grid& grid_;
  CellText& text_;
  PrintInfo info_;
  std::vector<PrintPage> pages_;
  float scale_ = 1;
  float paperW_ = 612, paperH_ = 792;
  std::vector<Range> areas_;
  std::vector<double> colX_;   // column lefts at the page's scale, points at 100 %
  std::vector<Range> sparklines_;  // the cells the sheet's sparklines are in: they print
  std::map<int, int> digits_;  // the printer's digit in dots, by scale percent
  std::optional<std::pair<uint32_t, uint32_t>> titleRows_, titleCols_;
};

}  // namespace fw::sheet::view
