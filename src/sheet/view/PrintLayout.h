#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <string>
#include <vector>

#include "draw/Painter.h"
#include "sheet/model/Workbook.h"
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
};

struct PrintInfo {
  std::wstring fileName;   // &F
  std::wstring path;       // &Z: the folder, with a trailing backslash
  std::wstring sheetName;  // &A
  std::wstring date, time; // &D and &T, as the locale writes them
  // The paper when the file names none, by its paperSize code; 0: the file's default (Letter).
  uint32_t defaultPaper = 0;
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
  float ColsWidth(uint32_t first, uint32_t last) const;   // sheet points
  float RowsHeight(uint32_t first, uint32_t last) const;
  // The last column text in `used` runs into, past its last column too: Excel prints that text.
  uint32_t SpillEnd(const Range& used);
  // The last column a cell's text runs into (its own when it stays in its cell); `past` is a
  // column it can't reach without being long enough to get past it, to skip measuring most cells.
  uint32_t Reach(const CellInfo& c, uint32_t past);
  // Nothing on a page: no values, fills or borders, and no text running into it.
  bool Blank(const Range& body);

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
  std::optional<std::pair<uint32_t, uint32_t>> titleRows_, titleCols_;
};

}  // namespace fw::sheet::view
