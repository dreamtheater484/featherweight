#pragma once

// Evaluating compiled formulas (plan §7.3): a small stack machine over the bytecode. References
// stay references until a function or operator needs values, so SUM(A:A) reads only the cells the
// sheet has, through the Host, without copying them.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sheet/calc/Formula.h"
#include "sheet/calc/Value.h"

namespace fw::sheet::calc {

// A cell's value as the host keeps it. Text is borrowed: valid until the host's cells change.
struct CellView {
  Type type = Type::Empty;  // Empty, Number, Bool, Text or Error
  Err err = Err::None;
  double num = 0;  // a number, or a boolean as 0 or 1
  std::string_view text;
};

// Called for each non-empty cell; return false to stop.
using CellFn = bool (*)(void* context, int32_t row, int32_t col, const CellView& value);

// The workbook as the engine reads it. The model implements it (s1-model); tests use a map.
class Host {
 public:
  virtual ~Host() = default;
  virtual int SheetCount() const = 0;
  // The cell's value; for a formula cell, its current (cached or calculated) value.
  virtual CellView Cell(int sheet, int32_t row, int32_t col) = 0;
  // Each non-empty cell of the rectangle, row by row, left to right.
  virtual void ForEachCell(int sheet, int32_t r1, int32_t c1, int32_t r2, int32_t c2, CellFn fn, void* context) = 0;
  // A formula's new value, after the engine calculated it (each cell of an array formula's range).
  virtual void Store(int sheet, int32_t row, int32_t col, const Value& v) { (void)sheet, (void)row, (void)col, (void)v; }
  // A dynamic array at (row, col) spills over `out` now (calculated again; the cell alone when
  // blocked), after its cells were stored.
  virtual void SpillMoved(int sheet, int32_t row, int32_t col, const Area& out) { (void)sheet, (void)row, (void)col, (void)out; }
  // How far the sheet's cells reach: one past the last used row and column (0 when empty).
  virtual void Extent(int sheet, int32_t& rows, int32_t& cols) = 0;
  // Whether a merged cell or a table lies in `area` (a dynamic array can't spill there).
  virtual bool SpillBlocked(const Area& area) { (void)area; return false; }
  // The workbook's date system (date1904).
  virtual bool Date1904() const { return false; }
  // A structured reference: table `name` (or, when empty, the table holding the cell), with the
  // items (bc::TableItem) and columns asked for. False with `error` set when there's no such table,
  // column or row (#REF!, #VALUE! for a #This Row outside the data), or left Err::None when the host
  // doesn't know tables (the formula keeps its cached value).
  virtual bool Table(std::string_view name, int sheet, int32_t row, int32_t col, uint8_t items, std::string_view col1,
                     std::string_view col2, Area& out, Err& error) {
    (void)name, (void)sheet, (void)row, (void)col, (void)items, (void)col1, (void)col2, (void)out, (void)error;
    return false;
  }
  // TEXT(): `value` (a number, or a text, which goes through the format's text section) formatted
  // with a number format code as the region types it. 1: done; 0: the host can't (the formula then
  // keeps its cached value); -1: #VALUE! (Excel refuses the code, or the value has no section or is
  // out of a date's range: a cell would show ####).
  virtual int FormatText(const CellView& value, std::string_view format, std::string& out) {
    (void)value, (void)format, (void)out;
    return 0;
  }
  // The region's text and number conventions (Excel follows Windows' settings).
  virtual const ConvLocale& Conv() const { return EnUsConv(); }
  // Text as a number, as typing it would read it (numbers, percentages, dates, times).
  virtual bool ParseNumber(std::string_view text, double& out) { return TextToNumber(text, out, Date1904(), Conv()); }
  // The current date and time as a serial number (NOW). Tests fix it.
  virtual double Now();
  // RAND(): [0, 1).
  virtual double Random();
  // Hidden rows (SUBTOTAL 101-111 and AGGREGATE's odd options leave them out).
  virtual bool RowHidden(int sheet, int32_t row) {
    (void)sheet, (void)row;
    return false;
  }
  // Manual calculation (INFO("recalc")).
  virtual bool Manual() { return false; }
  // The most cells an array a formula builds may have (a range read as one, an operator's or a
  // function's result): a larger one is #NUM!, where Excel runs out of resources. Fuzzing lowers it.
  virtual int64_t MaxArrayCells() const { return 16'000'000; }
  // Rows a filter hides: hidden, in an autoFilter range with criteria (every SUBTOTAL leaves them
  // out; AGGREGATE doesn't tell them from other hidden rows).
  virtual bool RowFiltered(int sheet, int32_t row) {
    (void)sheet, (void)row;
    return false;
  }
};

// What evaluation needs beyond the cells: defined names, spill ranges, and whether the cells of a
// reference made at run time (OFFSET, INDIRECT) are calculated yet.
class Resolver {
 public:
  virtual ~Resolver() = default;
  virtual Names& names() = 0;
  // The definition of name `slot` as seen from `sheet` (sheet-scoped first, then the workbook's).
  virtual const Formula* Name(uint32_t slot, int sheet) = 0;
  // A1#: the range the dynamic array at the cell spills into.
  virtual bool Spill(int sheet, int32_t row, int32_t col, Area& out) = 0;
  // False when a cell in `a` must be calculated first: evaluation stops and is retried later.
  virtual bool Ready(const Area& a) = 0;
  // The formula of a cell ("=A1+1", "{=...}" for a legacy array formula's cells), invariant; false
  // when it holds none (a dynamic array's spilled cells hold none). ISFORMULA, FORMULATEXT.
  virtual bool FormulaText(int sheet, int32_t row, int32_t col, std::string& text) {
    (void)sheet, (void)row, (void)col, (void)text;
    return false;
  }
  // Whether the cell holds a SUBTOTAL formula (SUBTOTAL leaves those out).
  virtual bool Subtotal(int sheet, int32_t row, int32_t col) {
    (void)sheet, (void)row, (void)col;
    return false;
  }
};

struct Cell {
  int sheet = 0;
  int32_t row = 0, col = 0;
};

enum class EvalResult { kDone, kRetry, kUnsupported };

// Evaluates formulas. Keeps its stack between calls; one per engine.
class Evaluator {
 public:
  Evaluator(Host& host, Resolver* resolver) : host_(host), resolver_(resolver) {}
  // Evaluates `f` at `at`. An array formula gives an Array (or a scalar); any other gives one
  // scalar, references implicitly intersected with the cell, as Excel does. kRetry: a dynamic
  // reference needs a cell calculated first (Resolver::Ready said so). kUnsupported: a function
  // the engine lacks.
  EvalResult Evaluate(const Formula& f, const Cell& at, Value& out);

  Host& host() { return host_; }
  Resolver* resolver() { return resolver_; }

 private:
  Host& host_;
  Resolver* resolver_;
  std::vector<Value> stack_;
  friend struct Machine;
};

}  // namespace fw::sheet::calc
