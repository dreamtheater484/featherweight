#pragma once

// The calculation engine (plan §7.3): formula cells, the dependency graph and recalculation.
//
// Ranges are tracked as ranges (SUM(A:A) is one dependency). A change marks the formulas that
// depend on it dirty; recalculation then calculates them precedents first, time-sliced. After a
// workbook opens, it calculates only what Excel would (volatile functions and what depends on them,
// formulas without a cached value, everything with fullCalcOnLoad or from an older calculation
// engine, a sheet with its own fullCalcOnLoad); manual mode is respected. A
// formula with something the engine can't calculate keeps its cached value, marked stale.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sheet/calc/Eval.h"
#include "sheet/calc/Formula.h"

namespace fw::sheet::calc {

struct CalcSettings {
  bool manual = false;          // calcPr calcMode="manual"
  // Everything is recalculated on opening: calcPr fullCalcOnLoad="1", or a file Excel doesn't take
  // its cached values from (WorkbookCalc::Settings).
  bool fullCalcOnLoad = false;
  std::vector<int> fullCalcSheets;  // sheets with sheetCalcPr fullCalcOnLoad="1"
  bool iterate = false;         // calcPr iterate="1"
  int iterateCount = 100;
  double iterateDelta = 0.001;
};

class Engine : public Resolver, public Names {
 public:
  explicit Engine(Host& host);
  ~Engine() override;

  // --- the workbook ---
  void SetSheets(std::vector<std::string> names);
  // A defined name: `scope` is its sheet, or -1 for the workbook. Its references are relative to
  // A1, as files store them.
  bool DefineName(std::string_view name, int scope, std::string_view text);
  // A formula cell. `f` is compiled at `at` (or, for a shared formula, at its first cell with the
  // same relative offsets). `cached`: the file has its value. `range`: the cells an array formula
  // fills (its ref); `dynamic`: a dynamic array (it spills) rather than Ctrl+Shift+Enter.
  void SetFormula(const Cell& at, std::shared_ptr<const Formula> f, bool cached = true, const Area* range = nullptr,
                  bool dynamic = false);
  void ClearFormula(const Cell& at);
  // After an edit: every formula of `a` on `sheet` removed (the edited cells are read again).
  void ClearFormulas(int sheet, const Area& a);
  // A formula an edit set: calculated next.
  void Touch(const Cell& at);
  // A formula in a circular reference, for the status bar ("Circular References: B2").
  bool FirstCircular(Cell& out) const;
  // Calculates the formula at `at` now, from the values as they are (manual mode: the formula
  // just entered; what depends on it waits for F9).
  void CalculateCell(const Cell& at);
  // Shift+F9: the sheet's dirty and volatile formulas, from the other sheets' values as they are
  // (those stay dirty for F9).
  void CalculateSheet(int sheet);
  // Compiles `text` at `at` with this engine's sheets and names. Null on a syntax error.
  std::shared_ptr<const Formula> CompileAt(std::string_view text, const Cell& at, bool array = false,
                                           ParseError* error = nullptr);

  // --- recalculation ---
  // After opening: marks what Excel recalculates when it opens the file.
  void Opened(const CalcSettings& settings);
  // A value cell changed (an edit): its dependents become dirty. Volatile formulas too, as Excel.
  void Changed(int sheet, const Area& cells);
  void MarkAllDirty();
  // Calculates dirty formulas for at most `budgetMs` (UI time slices). True when nothing is left.
  bool Recalculate(double budgetMs = 1e300);
  // Builds the dependency graph about `budgetMs` at a time (the app, after opening): true once it
  // is built. Opened, Changed and Recalculate build what is left at once.
  bool BuildGraph(double budgetMs);
  bool Pending() const { return dirtyCount_ > 0; }

  // --- what happened ---
  bool Stale(int sheet, int32_t row, int32_t col) const;  // kept its cached value: unsupported
  bool HasFormula(int sheet, int32_t row, int32_t col) const { return NodeAt(sheet, row, col) != UINT32_MAX; }
  bool Dirty(int sheet, int32_t row, int32_t col) const;  // waiting to be calculated
  size_t StaleCount() const;
  bool Circular() const { return circular_; }
  size_t FormulaCount() const { return nodes_.size() - free_.size(); }
  size_t Evaluations() const { return evaluations_; }  // since the engine was made

  // Resolver
  Names& names() override { return *this; }
  const Formula* Name(uint32_t slot, int sheet) override;
  bool Spill(int sheet, int32_t row, int32_t col, Area& out) override;
  bool Ready(const Area& a) override;
  bool Subtotal(int sheet, int32_t row, int32_t col) override;
  bool FormulaText(int sheet, int32_t row, int32_t col, std::string& text) override;
  // Names
  int FindSheet(std::string_view name) const override;
  std::string SheetName(int sheet) const override;
  uint32_t NameSlot(std::string_view name) override;
  std::string SlotName(uint32_t slot) const override;

 private:
  enum State : uint8_t { kClean, kDirty, kVisiting };
  enum NodeFlag : uint8_t { kCached = 1, kStale = 2, kCircularNode = 4, kDynamicArray = 8, kVolatileNode = 16, kFree = 32 };
  struct Node {
    std::shared_ptr<const Formula> f;
    Cell at;
    Area out;  // the cells it fills
    uint8_t state = kClean;
    uint8_t flags = 0;
  };
  struct NameDef {
    std::string name;
    std::vector<std::pair<int, std::shared_ptr<const Formula>>> scopes;
  };
  struct Frame {
    uint32_t node;
    bool evaluating;
  };
  class Graph;
  class DirtyCells;

  static uint64_t Key(int sheet, int32_t row, int32_t col) {
    return (static_cast<uint64_t>(sheet) << 40) | (static_cast<uint64_t>(row) << 14) | static_cast<uint64_t>(col);
  }
  uint32_t NodeAt(int sheet, int32_t row, int32_t col) const;  // or UINT32_MAX
  void MarkDirty(uint32_t node);
  void Propagate();
  void Clean(uint32_t node);
  void StoreSpill(uint32_t node, Value v);
  // The areas a formula reads, at its cell (names and tables resolved).
  void Precedents(const Node& n, std::vector<Area>& out, bool* volatileName = nullptr);
  void Collect(const uint8_t* code, size_t size, const Cell& at, std::vector<Area>& out, bool* volatileName, int depth);
  void Store(uint32_t node, Value v);
  void EnsureGraph();
  void GraphChanged();  // formulas or names changed: the graph is built again when needed
  bool Iterate();

  Host& host_;
  Evaluator ev_;
  std::vector<std::string> sheets_;
  std::vector<NameDef> names_;
  std::unordered_map<std::string, uint32_t> nameSlots_;
  std::vector<Node> nodes_;
  std::vector<uint32_t> free_;
  std::unordered_map<uint64_t, uint32_t> at_;  // every cell a formula fills -> its node
  std::unique_ptr<Graph> graph_;
  std::unique_ptr<DirtyCells> dirtyCells_;
  bool graphStale_ = true;
  std::vector<uint32_t> pending_;  // dirty nodes to calculate
  std::vector<Area> changed_;      // areas whose dependents must be marked
  std::vector<Frame> stack_;
  size_t dirtyCount_ = 0;
  uint32_t needed_ = UINT32_MAX;  // the dirty cell a run-time reference needs first
  int onlySheet_ = -1;            // CalculateSheet: other sheets' cells are taken as they are
  bool circular_ = false;
  CalcSettings settings_;
  size_t evaluations_ = 0;
  std::vector<uint32_t> circularNodes_;
};

}  // namespace fw::sheet::calc
