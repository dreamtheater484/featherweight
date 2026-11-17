#pragma once

// The formula engine bound to a workbook model (src/sheet/model): the engine reads cells through
// the model and writes recalculated values back with Sheet::SetValue.
//
//   WorkbookCalc calc(book);
//   calc.Load();                     // every formula, the defined names and calcPr
//   calc.engine().Opened(calc.Settings());
//   while (!calc.engine().Recalculate(8)) { /* the next idle slice */ }

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sheet/calc/Engine.h"

namespace fw::sheet {
class Workbook;
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

  // Host
  int SheetCount() const override;
  CellView Cell(int sheet, int32_t row, int32_t col) override;
  void ForEachCell(int sheet, int32_t r1, int32_t c1, int32_t r2, int32_t c2, CellFn fn, void* context) override;
  void Store(int sheet, int32_t row, int32_t col, const Value& v) override;
  void Extent(int sheet, int32_t& rows, int32_t& cols) override;
  bool Date1904() const override;
  int FormatText(double value, std::string_view format, std::string& out) override;
  bool RowHidden(int sheet, int32_t row) override;
  const ConvLocale& Conv() const override { return conv_; }

 private:
  Workbook& book_;
  const NumLocale* locale_;
  ConvLocale conv_;
  std::unique_ptr<Engine> engine_;
  std::vector<std::pair<int32_t, int32_t>> extents_;
  std::unordered_map<std::string, std::unique_ptr<NumberFormat>> formats_;
};

}  // namespace fw::sheet::calc
