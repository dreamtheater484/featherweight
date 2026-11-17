#pragma once

// The formula engine bound to a workbook model (src/sheet/model): the engine reads cells through
// the model and writes recalculated values back with Sheet::SetValue.
//
//   WorkbookCalc calc(book);
//   while (!calc.OpenStep(8)) { /* the next idle slice */ }  // the app, after opening
//
//   calc.Load();                     // tools: every formula and the defined names at once
//   calc.engine().Opened(calc.Settings());
//   calc.engine().Recalculate();

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sheet/calc/Engine.h"

namespace fw::sheet {
class Workbook;
class Sheet;
struct CellInfo;
struct Range;
class NumberFormat;
struct NumLocale;
}  // namespace fw::sheet

namespace fw::sheet::calc {

class WorkbookCalc : public Host {
 public:
  // `locale`: TEXT()'s separators and month names (null: en-US).
  explicit WorkbookCalc(Workbook& book, const NumLocale* locale = nullptr);
  ~WorkbookCalc() override;

  struct LoadStats {
    size_t formulas = 0;      // given to the engine
    size_t parseErrors = 0;   // text the parser refused (kept as cached)
    size_t dataTables = 0;    // TABLE() what-if tables (kept as cached)
  };
  // Loads every sheet (formulas may read any), then gives the engine the formulas and names.
  LoadStats Load();
  CalcSettings Settings() const;
  Engine& engine() { return *engine_; }

  // After opening (the app, at idle, once the shown sheet is on screen): what Load() does, then the
  // dependency graph, then the recalculation Excel does when it opens a file (volatile functions
  // and what depends on them, formulas without a cached value, everything with fullCalcOnLoad;
  // nothing in manual mode), about `budgetMs` at a time. True once done; nothing runs after that
  // until an edit. A formula with an unsupported function keeps its cached value: engine().Stale().
  bool OpenStep(double budgetMs);
  bool Opening() const { return phase_ != Phase::kDone; }
  // Counts the recalculated values that differ from what their cells showed (repaint when it moves).
  uint64_t Changes() const { return changes_; }

  // After an edit (s2-model's change notifications, the model already changed):
  // values or formulas in `cells` of `sheet` (kValues, kFormulas): its formulas are read again and
  // what depends on the cells becomes dirty. Manual mode: a formula entered is calculated, the rest
  // waits for F9.
  void CellsEdited(int sheet, const Range& cells);
  // Sheets, names, rows or columns inserted or deleted (kSheets, kNames, kShift): every formula is
  // read again and recalculated.
  void StructureEdited();
  // Recalculates what edits left dirty, about `budgetMs` at a time (at idle): true when nothing is
  // left, and nothing runs after that. Not called in manual mode until F9.
  bool CalcStep(double budgetMs);
  bool Calculating() const { return engine_->Pending(); }  // "Calculating…" while a step leaves work
  bool ManualCalc() const;                                  // calcPr calcMode="manual"
  void CalculateNow();   // F9: then CalcStep until done (dirty and volatile formulas)
  void CalculateFull();  // Ctrl+Alt+F9: every formula
  void CalculateSheet(int sheet);  // Shift+F9: the sheet's dirty and volatile formulas, at once
  // A cell in a circular reference, for the banner and "Circular References: B2"; none while
  // iterative calculation is on (Excel then warns of nothing).
  bool CircularCell(int& sheet, int32_t& row, int32_t& col) const;

  // Host
  int SheetCount() const override;
  CellView Cell(int sheet, int32_t row, int32_t col) override;
  void ForEachCell(int sheet, int32_t r1, int32_t c1, int32_t r2, int32_t c2, CellFn fn, void* context) override;
  void Store(int sheet, int32_t row, int32_t col, const Value& v) override;
  void Extent(int sheet, int32_t& rows, int32_t& cols) override;
  bool Date1904() const override;
  int FormatText(const CellView& value, std::string_view format, std::string& out) override;
  bool Table(std::string_view name, int sheet, int32_t row, int32_t col, uint8_t items, std::string_view col1,
             std::string_view col2, Area& out, Err& error) override;
  bool RowHidden(int sheet, int32_t row) override;
  bool RowFiltered(int sheet, int32_t row) override;
  bool Manual() override;
  const ConvLocale& Conv() const override { return conv_; }

 private:
  enum class Phase : uint8_t { kSheets, kFormulas, kGraph, kRecalc, kDone };
  // Load()'s work until `deadline` (Seconds-like, steady clock): true once the formulas are in.
  bool LoadSome(double deadline);
  void AddFormula(const Sheet& s, size_t sheet, const CellInfo& c);  // the cell's formula, to the engine

  Workbook& book_;
  Phase phase_ = Phase::kSheets;
  size_t sheet_ = 0;                // kSheets, kFormulas: the sheet being read
  uint32_t row_ = 0, col_ = 0;      // kFormulas: the next cell
  size_t formulasLeft_ = 0;         // kFormulas: in this sheet
  std::map<std::pair<uint32_t, uint32_t>, std::shared_ptr<const Formula>> shared_;  // this sheet's shared formulas
  LoadStats stats_;
  uint64_t changes_ = 0;
  const NumLocale* locale_;
  ConvLocale conv_;
  std::unique_ptr<Engine> engine_;
  std::vector<std::pair<int32_t, int32_t>> extents_;
  std::unordered_map<std::string, std::unique_ptr<NumberFormat>> formats_;
};

}  // namespace fw::sheet::calc
