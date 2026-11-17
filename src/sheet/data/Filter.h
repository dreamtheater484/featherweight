#pragma once

// Filter (plan §7.2 "Data"): which rows of a filtered range its criteria keep, and the values a
// column offers, compared as Excel's AutoFilter compares them: a list of values by the text each
// cell shows (without case), custom conditions by value for numbers and with * and ? for text.
// Reads the model only; the edit layer hides the rows.

#include <cstdint>
#include <string>
#include <vector>

#include "sheet/model/Workbook.h"
#include "sheet/numfmt/NumLocale.h"

namespace fw::sheet::data {

struct FilterCondition {
  enum class Op : uint8_t { Equal, NotEqual, Greater, GreaterEqual, Less, LessEqual };
  Op op = Op::Equal;
  std::wstring value;  // a number in the invariant form ("2.5"), or text with * and ? (~ escapes them)
};

// One column's criteria (an autoFilter's filterColumn).
struct FilterCriteria {
  uint32_t column = 0;               // within the range (colId)
  bool listed = false;               // a list of values applies:
  std::vector<std::wstring> values;  //   the shown texts kept
  bool blanks = false;               //   and empty cells
  std::vector<FilterCondition> conditions;  // else up to two custom conditions
  bool both = false;                 // both conditions must hold (and), else either (or)
  // Else one of Excel's other number filters, over the column's numbers only (text and blanks are
  // left out; measured): Top 10 (top10: the `count` largest, the smallest with `bottom`, ties
  // included; with `percent` floor(numbers * count / 100) of them, at least one) or above or below
  // the numbers' average (dynamicFilter), strictly.
  enum class Special : uint8_t { None, Top10, AboveAverage, BelowAverage };
  Special special = Special::None;
  double count = 10;
  bool bottom = false, percent = false;
};

// The text a cell shows, without a width limit, as filters compare it.
std::wstring ShownText(const Workbook& book, const Sheet& sheet, uint32_t row, uint32_t col, const NumLocale& locale);

// The distinct texts of column `column` of `ref` below its header row, in Excel's order (numbers by
// value, then text, FALSE, TRUE, errors), and whether the column has empty cells.
std::vector<std::wstring> ColumnValues(const Workbook& book, const Sheet& sheet, const Range& ref, uint32_t column,
                                       const NumLocale& locale, bool& blanks);

// For each row of `ref` below the header: whether it passes every column's criteria.
std::vector<bool> FilterRows(const Workbook& book, const Sheet& sheet, const Range& ref,
                             const std::vector<FilterCriteria>& criteria, const NumLocale& locale);

// Whether a cell passes one condition.
bool Passes(const Value& value, const std::wstring& shown, const FilterCondition& c);

// Remove duplicates: for each row of `range` (below its first row when `header`), whether it repeats
// an earlier row in `columns` (within the range), compared as Excel compares them: the texts shown,
// without case.
std::vector<bool> DuplicateRows(const Workbook& book, const Sheet& sheet, const Range& range,
                                const std::vector<uint32_t>& columns, bool header, const NumLocale& locale);

// The block of filled cells around `at` that Ctrl+Shift+L filters (Excel's current region): grown
// while a filled cell touches it, diagonals included.
Range CurrentRegion(const Sheet& sheet, CellAddr at);

}  // namespace fw::sheet::data
