#pragma once

#include <cstdint>
#include <functional>

#include "draw/Painter.h"
#include "sheet/model/Workbook.h"
#include "sheet/view/CellText.h"
#include "sheet/view/Grid.h"

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
};

struct PaintOptions {
  bool gridlines = true;
  uint32_t gridColor = 0xFFD9D9D9;  // ARGB
  bool showZeros = true;
  PrintErrors errors = PrintErrors::Displayed;  // printing: how error values print
};

// What draws over the cells: pictures, shapes, charts and notes (s1-model's drawing layer). It gets
// the painter, the sheet, the geometry and the cells drawn (the screen's panes and printed pages
// call it after their cells, clipped to them). Set once at startup; empty until that layer exists.
using OverlayFn = std::function<void(draw::Painter&, const Sheet&, const Geometry&, const Range&)>;
void SetOverlay(OverlayFn fn);
const OverlayFn& Overlay();

// Draws the cells of `area` (rows and columns as the geometry places them): fills, gridlines,
// borders and text, with text running into empty neighbors and merged cells drawn as one. Hidden
// rows and columns are skipped. The caller clips.
void PaintCells(draw::Painter& p, const Sheet& sheet, CellText& text, const Grid& grid, const Geometry& g,
                const Range& area, const PaintOptions& options);

draw::Color ToColor(uint32_t argb);

}  // namespace fw::sheet::view
