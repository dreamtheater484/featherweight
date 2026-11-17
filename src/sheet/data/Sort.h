#pragma once

// Sort (plan §7.2 "Data"): the order Excel sorts rows in, for the edit layer's SortRows (and
// columns, for a left-to-right sort, SortColumns), and the header guess.

#include <cstdint>
#include <vector>

#include "sheet/model/Workbook.h"

namespace fw::sheet::data {

struct SortKey {
  uint32_t index = 0;       // the column (rows sorted) or row (columns sorted) the key reads
  bool descending = false;
};

// The order of the rows of `range` (columns with `byColumns`) sorted by `keys`, as Excel sorts
// (measured): ascending numbers, text, FALSE, TRUE, errors; descending the reverse; blanks last
// both ways; text without case in Windows' word order (hyphens and apostrophes ignored, ß as ss);
// ties keep their order. order[i] is the row (column) that goes to position i of the range.
std::vector<uint32_t> SortOrder(const Sheet& sheet, const Range& range, const std::vector<SortKey>& keys,
                                bool byColumns = false);

// Whether the range's first row is a header, as Excel's Sort guesses: a column whose first cell is
// text over a number or date below, or whose first cell is bold and the next isn't.
bool GuessHeader(const Workbook& book, const Sheet& sheet, const Range& range);

}  // namespace fw::sheet::data
