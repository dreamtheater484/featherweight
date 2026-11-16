// Fuzz target: the formula scanner the reference registry uses (sheet names and cell references
// in formulas, sqref lists, field instructions), and the formula engine (src/sheet/calc): the
// parser, printing back and parsing again, and evaluating, as a cell and as an array formula.
// Seeds: tests/fuzz/seeds/formula.
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "pkg/Formula.h"
#include "pkg/References.h"
#include "sheet/calc/Engine.h"

using namespace fw::pkg;
namespace calc = fw::sheet::calc;

namespace {

// A few cells on two sheets: numbers, text, a boolean and an error.
class FuzzHost : public calc::Host {
 public:
  int SheetCount() const override { return 2; }
  calc::CellView Cell(int sheet, int32_t row, int32_t col) override {
    calc::CellView v;
    if (row > 3 || col > 3) return v;
    switch ((sheet + row * 4 + col) % 5) {
      case 0: v.type = calc::Type::Number, v.num = row * 10 + col - 3.5; break;
      case 1: v.type = calc::Type::Text, v.text = row % 2 ? "abc" : "12"; break;
      case 2: v.type = calc::Type::Bool, v.num = 1; break;
      case 3: v.type = calc::Type::Error, v.err = calc::Err::NA; break;
      default: break;
    }
    return v;
  }
  void ForEachCell(int sheet, int32_t r1, int32_t c1, int32_t r2, int32_t c2, calc::CellFn fn, void* ctx) override {
    for (int32_t r = r1; r <= r2 && r <= 3; ++r)
      for (int32_t c = c1; c <= c2 && c <= 3; ++c) {
        calc::CellView v = Cell(sheet, r, c);
        if (v.type != calc::Type::Empty && !fn(ctx, r, c, v)) return;
      }
  }
  void Extent(int, int32_t& rows, int32_t& cols) override { rows = cols = 4; }
  double Now() override { return 45293.5; }
  double Random() override { return 0.5; }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view formula(reinterpret_cast<const char*>(data), size);
  std::string firstSheet;
  ForEachSheetToken(formula, [&](const SheetToken& t) {
    if (t.begin > t.end || t.end > formula.size()) std::abort();
    if (firstSheet.empty()) firstSheet = t.first;
    QuoteSheetName(t.first);
  });
  ForEachCellToken(formula, [&](std::string_view cell) {
    uint32_t col = 0, row = 0;
    if (!ParseCellRef(cell, col, row) || col == 0 || col > kMaxColumn || row == 0 || row > kMaxRow) std::abort();
    if (CellName(col, row).empty()) std::abort();
  });
  IsCellRange(formula);
  bool changed = false;
  RenameSheetInFormula(formula, firstSheet, "Renamed sheet", &changed);

  // The engine: parse, print and parse again, evaluate.
  static FuzzHost host;
  static calc::Engine* engine = [] {
    auto* e = new calc::Engine(host);
    e->SetSheets({"Sheet1", "Sheet 2"});
    e->DefineName("Rate", -1, "Sheet1!$B$2");
    e->DefineName("Loop", -1, "Loop+1");
    return e;
  }();
  if (formula.size() <= 8192) {
    for (bool array : {false, true}) {
      calc::Cell at{0, 2, 2};
      auto f = engine->CompileAt(formula, at, array);
      if (!f) continue;
      calc::CompileOptions where;
      where.row = 2, where.col = 2, where.array = array;
      std::string printed = calc::Print(*f, where, *engine);
      engine->CompileAt(printed, at, array);
      calc::Evaluator ev(host, engine);
      calc::Value v;
      ev.Evaluate(*f, at, v);
    }
  }

  // The other syntaxes a reference can have.
  static const ReferenceRegistry registry = ReferenceRegistry::Default();
  for (const auto& rule : registry.Rules())
    for (const auto& site : rule.sites)
      ForEachRefValue(site, rule.kind, formula, [&](std::string_view v) {
        if (v.size() > formula.size()) std::abort();
      });
  return 0;
}
