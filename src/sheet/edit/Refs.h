#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "sheet/edit/Edit.h"

// The reference registry's rewriting (plan §7.3, S2 step 4): where rows, columns and cells go when
// some are inserted or deleted (Editor::InsertRows and the rest), and the text of everything in the
// file that refers to them, as Excel rewrites it. Formulas are rewritten by the caller's function
// (sheet/calc's ShiftReferences); everything here is about positions and XML. The edit layer only.
namespace fw::sheet::edit::refs {

// Lines along the shift's axis: rows for Shift::Axis::Rows, else columns.
struct Mover {
  Shift sh;
  uint32_t max = 0;  // lines on the sheet along the axis
  // A sort instead of a shift (Editor::SortRows, SortColumns; sh.count 0): inside the band, line
  // sh.at + i goes to sh.at + dest[i]. Nothing that refers to cells follows a sort (Span, Area and
  // Areas leave them); cells, notes and objects inside one line move with it.
  std::vector<uint32_t> dest;

  explicit Mover(const Shift& s);
  bool Rows() const { return sh.axis == Shift::Axis::Rows; }
  bool Sorting() const { return !dest.empty(); }
  uint32_t Deleted() const { return sh.count < 0 ? static_cast<uint32_t>(-sh.count) : 0; }
  // A line after the edit; kNone when it's deleted or pushed off the sheet.
  uint32_t Line(uint32_t line) const;
  // Lines first..last along the axis after the edit, as Excel moves a reference: they move when
  // the edit is before them, grow when lines are inserted inside (or right after the first), and
  // shrink when some are deleted. False when every one is deleted.
  bool Span(uint32_t& first, uint32_t& last) const;
  // Whether something across first..last on the other axis moves: inside the band (1), beside it (0),
  // or partly (-1).
  int Across(uint32_t first, uint32_t last) const;
  // A cell after the edit (false: deleted, or off the sheet).
  bool Cell(uint32_t& row, uint32_t& col) const;
  // An area after the edit, as a reference moves. Areas partly in the band stay as they are
  // (Excel refuses the edit for merges, tables and arrays; areas in lists are split first).
  bool Area(Range& r) const;
  // An area list (sqref, space-separated) after the edit: areas partly in the band split, the part
  // inside moving, and deleted parts gone. Empty when nothing is left.
  std::string Sqref(std::string_view sqref) const;
  std::vector<Range> Areas(const std::vector<Range>& in) const;
};

// A formula (invariant text, no "=") at a cell of sheet `sheet` (its index) before the edit, after
// it. The edit's own sheet is the caller's.
using FormulaFn = std::function<std::string(std::string_view formula, int sheet, uint32_t row, uint32_t col)>;
// A formula moved from one cell to another of the same sheet, relative references following (CF and
// validation rules whose top-left cell was deleted are read from the next one).
using CopyFn = std::function<std::string(std::string_view formula, int sheet, CellAddr from, CellAddr to)>;

struct Context {
  Mover move;
  int sheet = 0;           // the edited sheet's index
  std::string sheetName;   // its name
  FormulaFn formula;
  CopyFn copy;
};

// Can the sheet's kept XML around its cells (PartText) be moved? Empty when it can, else what
// can't, for the banner: elements and extensions the registry doesn't know may hold references.
std::u16string CheckSheetXml(const detail::PartText& t);

// The kept XML of sheet `index` (the edited sheet or another) after the edit: conditional formats
// and validations (and their x14 forms), sparklines, the autofilter and its sort, page breaks,
// ignored errors, protected ranges, cell watches. On other sheets only formulas change.
void MoveSheetXml(detail::PartText& t, int index, const Context& c);

// Parts of the package, each after the edit; false when the part didn't change.
//  - a drawing of sheet `index`: anchors (when it's the edited sheet) and shapes' cell links
bool MoveDrawing(std::string& xml, int index, const Context& c);
//  - a chart (or chartex) anywhere (`index`: its sheet, or 0): series formulas, and the values cached
//    beside one that changed, read again by `cache` (the cells of the formula's area in order, hidden
//    rows and columns left out when `visibleOnly`; false when it can't).
struct CachePoint {
  enum Kind : uint8_t { Empty, Number, Text } kind = Empty;
  std::string text;  // a number as the cache writes it, or the text
};
using CacheFn = std::function<bool(std::string_view formula, bool visibleOnly, std::vector<CachePoint>& out)>;
bool MoveChart(std::string& xml, int index, const Context& c, const CacheFn& cache);
//  - the edited sheet's notes (comments part), threaded comments, and its VML: notes on deleted
//    cells go (with their shapes), the rest move; form controls' links follow.
bool MoveComments(std::string& xml, const Context& c);
bool MoveVml(std::string& xml, int index, const Context& c);
//  - a table on the edited sheet (its Table already moved): ref, autoFilter, sortState, columns'
//    formulas
bool MoveTable(std::string& xml, const Table& before, const Table& after, int index, const Context& c);
//  - a form control's properties (ctrlProp) of sheet `index`: its cell links
bool MoveControl(std::string& xml, int index, const Context& c);
//  - a pivot cache's worksheet source; a pivot table's location (on the edited sheet)
bool MovePivotCache(std::string& xml, const Context& c);
// A pivot cache whose source is on a renamed sheet: its sheet attributes follow.
bool RenamePivotSource(std::string& xml, std::string_view from, std::string_view to);

// A formula after columns of table `table` were deleted: structured references naming one of
// `columns` (Table1[[#This Row],[n]]) become #REF!, as Excel makes them.
std::string DropTableColumns(std::string_view formula, std::string_view table, const std::vector<std::string>& columns);
bool MovePivotTable(std::string& xml, const Context& c);

}  // namespace fw::sheet::edit::refs
