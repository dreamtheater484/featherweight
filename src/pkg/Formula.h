#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace fw::pkg {

// Just enough of Excel's A1 formula syntax for the reference registry: the sheets a formula
// names, and its cell references. The spreadsheet engine's formula parser (S1) is separate.

// A sheet prefix in a formula: Sheet1!, 'My sheet'!, or a 3D one, Sheet1:Sheet3!.
struct SheetToken {
  size_t begin = 0, end = 0;  // the prefix in the formula, quotes included, without the "!"
  std::string first, last;    // the names, unquoted; `last` is empty unless it is 3D
};

// Calls `fn` for each sheet prefix in `formula`, in order. Prefixes inside string literals,
// error values (#REF!) and references to other workbooks ([1]Sheet1!A1) are skipped.
void ForEachSheetToken(std::string_view formula, const std::function<void(const SheetToken&)>& fn);

// Renames the sheet `from` to `to` (compared without ASCII case, as Excel does) in every prefix,
// quoting the result as needed. Sets `changed` when it did.
std::string RenameSheetInFormula(std::string_view formula, std::string_view from, std::string_view to,
                                 bool* changed = nullptr);

// A sheet name as a formula writes it: Sales, or 'Q1 sales' when it needs quotes.
std::string QuoteSheetName(std::string_view name);

// Calls `fn` for each cell reference in `formula` outside sheet prefixes, strings and function
// names: "A1", "$B$2". Ranges come as their two ends.
void ForEachCellToken(std::string_view formula, const std::function<void(std::string_view)>& fn);

inline constexpr uint32_t kMaxColumn = 16384;   // XFD
inline constexpr uint32_t kMaxRow = 1048576;

// "A1", "$B$2": a column (1-based) and a row (1-based), within the sheet's bounds.
bool ParseCellRef(std::string_view s, uint32_t& col, uint32_t& row);
// A cell or a range: A1, A1:B2, A:B, 1:2, with or without $, within the sheet's bounds.
bool IsCellRange(std::string_view s);
// "B3" for column 2, row 3.
std::string CellName(uint32_t col, uint32_t row);

}  // namespace fw::pkg
