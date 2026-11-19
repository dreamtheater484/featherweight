#pragma once

// Sparklines (S4, plan §7.2): the x14 sparklineGroups a sheet keeps in its extension list, read
// for drawing only (the XML is saved as it is), and drawn in their cells as Excel draws them:
// line, column and win/loss; markers and the high, low, first, last and negative points; the axis;
// colors; empty cells as gaps, zero or connected; individual, group or custom scales; right to left.
// Geometry measured on Excel 2021's printed sparklines (tests/office/make_sparkline_tests.py).

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "draw/Painter.h"
#include "sheet/model/Workbook.h"

namespace fw::sheet::view {

class Geometry;

struct SparklineGroup {
  enum class Type : uint8_t { Line, Column, WinLoss };
  enum class Empty : uint8_t { Gap, Zero, Span };
  enum class Scale : uint8_t { Individual, Group, Custom };
  Type type = Type::Line;
  Empty empty = Empty::Gap;
  Scale minScale = Scale::Individual, maxScale = Scale::Individual;
  double manualMin = 0, manualMax = 0;
  float lineWeight = 0.75f;  // points
  bool markers = false, high = false, low = false, first = false, last = false, negative = false;
  bool axis = false, hidden = false, rightToLeft = false;
  // ARGB.
  uint32_t series = 0xFF376092, negativeColor = 0xFFD00000, axisColor = 0xFF000000, markerColor = 0xFFD00000;
  uint32_t firstColor = 0xFFD00000, lastColor = 0xFFD00000, highColor = 0xFFD00000, lowColor = 0xFFD00000;
  // A date axis: the dates' cells (points then sit by date, not evenly).
  int dateSheet = -1;
  Range dates;
  struct Line {
    uint32_t row = 0, col = 0;  // its cell
    int sheet = -1;             // its data, a row or a column of cells
    Range data;
  };
  std::vector<Line> lines;
};

class Sparklines {
 public:
  // Reads sheet `index`'s sparkline groups (none: Empty()).
  Sparklines(const Workbook& book, size_t index);
  bool Empty() const { return groups_.empty(); }
  const std::vector<SparklineGroup>& Groups() const { return groups_; }

  // Draws the sparkline in (row, col), if there is one, in the cell's box `cell`.
  void Draw(draw::Painter& p, const Geometry& g, uint32_t row, uint32_t col, const draw::Rect& cell) const;

 private:
  // The line's values in order: NaN for an empty cell (and one that isn't a number); hidden rows
  // and columns left out unless the group shows them.
  std::vector<double> Values(const SparklineGroup& group, const SparklineGroup::Line& line) const;

  const Workbook& book_;
  std::vector<SparklineGroup> groups_;
  std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> at_;  // cell -> (group, line)
};

}  // namespace fw::sheet::view
