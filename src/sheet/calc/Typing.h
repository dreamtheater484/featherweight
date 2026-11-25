#pragma once

// What the grid needs while a formula is typed (plan §7.2 "Formula entry"): the references in the
// text (colored borders, point mode), F4's next form of a reference, the function and argument the
// caret is in (argument tips), and a formula's text when it's copied or filled. Works on the text
// as typed, in the region's form, complete or not; positions are UTF-16 units.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Lang.h"
#include "sheet/calc/Entry.h"

namespace fw::sheet::calc {

// A reference in the typed text.
struct RefSpan {
  size_t begin = 0, end = 0;  // the reference's text, sheet prefix included
  std::wstring sheet;         // the sheet it names, unquoted ("" for the formula's own)
  int32_t r1 = 0, c1 = 0, r2 = 0, c2 = 0;  // 0-based; whole columns span every row, whole rows every column
  int color = 0;              // 0, 1, 2...: the order Excel colors them in; the same range keeps its color
};
// The cell and area references (A1, $B$2:C3, Sheet2!A:A, 'My sheet'!1:2) of a formula being typed;
// not names, tables or other workbooks. Nothing when the text isn't a formula (starts with =, + or -).
std::vector<RefSpan> ReferenceSpans(std::wstring_view typed, const FormulaLocale& region);

// Whether a reference may be inserted at `caret` by pointing (clicking a cell, the arrow keys):
// after "=", an operator, "(", ":" or the list separator, outside text, with nothing of an operand
// right after it. Excel's point mode.
bool CanPointAt(std::wstring_view typed, size_t caret, const FormulaLocale& region);

// F4: the reference at the caret (or every reference in the selection [selBegin, selEnd)) in its
// next form: A1, $A$1, A$1, $A1, then A1 again; both ends of an area together. Updates `text` and
// the selection to cover the changed references. False when there's no reference there.
bool CycleReference(std::wstring& text, size_t& selBegin, size_t& selEnd, const FormulaLocale& region);

// The function call the caret is in, for its argument tip: the function's name as typed and the
// argument the caret is in (0 for the first). False outside any call.
bool CallAt(std::wstring_view typed, size_t caret, const FormulaLocale& region, std::wstring& function, int& argument);

// The functions Featherweight calculates, A to Z, for AutoComplete and the argument tips: the
// arguments as Excel names them ("number1, [number2], ...": optional ones in brackets, "..."
// repeats) and a short description.
struct FunctionHelp {
  const char* name;
  const char* args;
  UiText8 about;  // f1-lang: translated as it's read
};
const FunctionHelp* FunctionHelpList(size_t& count);
const FunctionHelp* FindFunctionHelp(std::string_view name);  // any case, _xlfn. ignored; null if none
// "SUM(number1; [number2]; ...)" in the region's form, with the place of argument `argument`
// (0-based; past the named ones, the repeated one) for bold.
std::wstring ArgumentTip(const FunctionHelp& help, const FormulaLocale& region, int argument, size_t& boldBegin,
                         size_t& boldEnd);

// AutoSum (Alt+=): what it needs to know of a cell.
enum class SumCell : uint8_t {
  kEmpty,
  kNumber,  // a number, typed or calculated
  kTotal,   // a number from a SUM or SUBTOTAL formula
  kOther,   // text, a boolean, an error
};
struct AutoSumProposal {
  std::vector<std::pair<int32_t, int32_t>> cells;  // the totals summed, nearest first; else empty
  int32_t r1 = 0, c1 = 0, r2 = 0, c2 = 0;           // the range summed (when `cells` is empty and `found`)
  bool found = false;                               // false: =SUM() with nothing in it
  std::string formula;                              // invariant, with "=": =SUM(B2:B5), =SUM(B7,B4), =SUM()
};
// The SUM Excel proposes for the empty cell (row, col): the numbers above it, else to its left
// (the column when there are numbers right above, else the row when there are numbers right to
// the left, blank cells between allowed), up to a text or blank cell; when that run holds totals
// (SUM formulas), the totals only. Excel's documented behavior; its AutoSum can't be driven
// without a visible window, so not measured here.
AutoSumProposal ProposeAutoSum(int32_t row, int32_t col, const std::function<SumCell(int32_t row, int32_t col)>& cellAt);

// AutoSum with several cells selected (rows r1-r2, columns c1-c2; the sheet's last row and column
// lastRow, lastCol), as Excel does it: the totals are entered at once, no cell is edited.
// - Every cell empty: each gets its own proposal (ProposeAutoSum), so a row under a block or a
//   column beside it gets its totals.
// - An empty last row in the selection: it gets the totals of the columns above it; an empty last
//   column, those of the rows left of it; both, and the corner gets the total of the row totals.
// - Else one row: its total right of it, in the first empty cell; more rows: each column's total
//   under it, in the first row below whose cells are all empty. Nothing is written over.
// Totals go under the columns (beside the rows) that hold a number, or all of them when none does;
// a line holding totals (SUM or SUBTOTAL formulas) sums those only, as ProposeAutoSum does.
// Empty when there's nothing to enter: empty cells with no numbers near, or no room on the sheet.
struct AutoSumCell {
  int32_t row = 0, col = 0;
  std::string formula;  // invariant, with "="
};
std::vector<AutoSumCell> ProposeAutoSums(int32_t r1, int32_t c1, int32_t r2, int32_t c2, int32_t lastRow,
                                         int32_t lastCol, const std::function<SumCell(int32_t row, int32_t col)>& cellAt);

// The formula of cell `from` as it reads in cell `to` (fill, copy and paste): relative references
// move with it, absolute ones stay; a reference moved off the sheet becomes #REF!, as Excel makes
// it. Invariant text in and out, with "=". Text that doesn't compile comes back unchanged.
std::string CopyFormula(std::string_view formula, const CompileOptions& from, const CompileOptions& to, Names& names);

}  // namespace fw::sheet::calc

namespace fw::sheet::edit {
struct Shift;
}

namespace fw::sheet::calc {

// Rows or columns (or cells, in a band) inserted or deleted on a sheet.
struct StructuralEdit {
  int sheet = 0;
  bool rows = true;    // rows, or cells moving down or up; else columns, or cells moving right or left
  int32_t at = 0;      // the first row or column inserted or deleted, 0-based
  int32_t count = 0;   // > 0 inserted, < 0 deleted
  int32_t band1 = -1, band2 = -1;  // cells only: the columns (rows: the rows) that move; -1 whole lines
};
// A formula of the cell `at` after the edit, as Excel rewrites it (measured): references move,
// grow and shrink; one deleted becomes #REF!; with cells, only references inside the band move.
// Invariant text in and out, without "=". `moved` gets the formula's own cell after the edit.
// Text that doesn't compile, and a formula nothing in it moved for, comes back as written; only the
// references that move are rewritten, the rest keeps its spaces and case.
std::string ShiftReferences(std::string_view formula, const CompileOptions& at, const StructuralEdit& edit,
                            Names& names, CompileOptions* moved = nullptr);
// The same with the editor's edit::Shift on `editedSheet`: along Rows, `band`'s columns are the
// cells that moved (all 16,384: whole rows); along Columns, its rows. 3-D references don't move
// (Excel leaves Sheet1:Sheet3!A2 alone when rows are inserted on Sheet2).
std::string ShiftReferences(std::string_view formula, const CompileOptions& at, int editedSheet, const edit::Shift& shift,
                            Names& names, CompileOptions* moved = nullptr);

// A formula after Editor::MoveSheet(sheet, to) (`to`: its index after the move) or DeleteSheet
// (`to` = -1), as Excel rewrites it (measured): `sheets` are the names before. A reference to a
// deleted sheet becomes #REF!A1. A 3-D range keeps its end sheets wherever they move; an end sheet
// deleted, or moved past the other end, gives way to its neighbour inside the range, and one sheet
// left is a plain reference (Sheet2:Sheet3!A1 → Sheet3!A1). Adding a sheet changes no text.
// Invariant text in and out, without "="; `at` is the formula's cell (its sheet index before).
std::string SheetsChanged(std::string_view formula, const CompileOptions& at, const std::vector<std::string>& sheets,
                          int sheet, int to);

}  // namespace fw::sheet::calc
