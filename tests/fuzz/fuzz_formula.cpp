// Fuzz target: the formula scanner the reference registry uses (sheet names and cell references
// in formulas, sqref lists, field instructions). Seeds: tests/fuzz/seeds/formula.
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "pkg/Formula.h"
#include "pkg/References.h"

using namespace fw::pkg;

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

  // The other syntaxes a reference can have.
  static const ReferenceRegistry registry = ReferenceRegistry::Default();
  for (const auto& rule : registry.Rules())
    for (const auto& site : rule.sites)
      ForEachRefValue(site, rule.kind, formula, [&](std::string_view v) {
        if (v.size() > formula.size()) std::abort();
      });
  return 0;
}
