#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "draw/Painter.h"
#include "sheet/model/Workbook.h"
#include "sheet/view/CellText.h"
#include "sheet/view/Grid.h"

namespace fw::sheet::pivot {
struct Look;
}

namespace fw::sheet::condfmt {
class SheetRules;
}

// Drawing cells through draw::Painter (plan §4.5): the same code draws the screen, printed pages
// and print preview, so they look alike.
namespace fw::sheet::view {

// Where cells go in the painter's coordinates. The screen gives whole device pixels at the zoom;
// a printed page gives points. In a right-to-left sheet column A is at the right.
class Geometry {
 public:
  virtual ~Geometry() = default;
  virtual float Left(uint32_t col) const = 0;  // the column's visual left edge
  virtual float Right(uint32_t col) const = 0;
  virtual float Top(uint32_t row) const = 0;
  virtual float Bottom(uint32_t row) const = 0;
  bool rtl = false;
  float pt = 1;        // painter units per point: text
  float px = 0.75f;    // painter units per Excel pixel at 100 %: padding
  float line = 0.75f;  // a thin line: one Excel pixel, at least one device pixel
  float rowPx = 0.75f; // painter units per Excel pixel down a row (printing: 0.72 pt)
  // How much wider a column is than on screen, for wrapping and "####": printed columns are wider.
  float widthScale = 1;
  // Paper: lines centred on the cell edges, a border's pixel a point (Excel prints thin borders at
  // 1 pt, thick at 3) and gridlines and hair borders as 0.2 pt hairlines. The screen draws a line in
  // the last device pixels of the cell before the edge.
  bool centered = false;
  // Text in its cell: how far in it starts and how far before the gridline it ends (the screen:
  // 2 px each), and how high a bottom line's baseline sits above the row's edge, per pixel of the
  // font's descent (0: on the screen's pixels). Excel prints text 2.4 pt in and 1.5 pt from the
  // right edge, and Calibri 11's baseline 3.18 pt up (its descent is 4 px).
  float padStart = 1.5f, padEnd = 1.5f;
  float descentPx = 0;
};

struct PaintOptions {
  bool gridlines = true;
  uint32_t gridColor = 0xFFD9D9D9;  // ARGB
  bool showZeros = true;
  PrintErrors errors = PrintErrors::Displayed;  // printing: how error values print
  bool blackAndWhite = false;  // printing (Excel's Black and white): no fills, text and borders black
  // Conditional formats over the cells' own formats (null: none); table styles under them.
  condfmt::SheetRules* rules = nullptr;
  bool tables = true;
  const std::vector<pivot::Look>* pivots = nullptr;  // the sheet's PivotTables' styles, like tables'
};


// Draws the cells of `area` (rows and columns as the geometry places them): fills, gridlines,
// borders and text, with text running into empty neighbors and merged cells drawn as one. Hidden
// rows and columns are skipped. The caller clips.
void PaintCells(draw::Painter& p, const Sheet& sheet, CellText& text, const Grid& grid, const Geometry& g,
                const Range& area, const PaintOptions& options);

draw::Color ToColor(uint32_t argb);

}  // namespace fw::sheet::view
