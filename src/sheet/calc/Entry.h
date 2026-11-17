#pragma once

// Formula entry (plan §7.2, decision 17): formulas are typed and shown with English function names
// and Windows' separators, as Excel does (=SUM(A1;2,5) with a decimal comma), and stored in the
// invariant form files use (=SUM(A1,2.5)). Measured against Excel 2021 on an en-NL PC
// (tests/unit/make_formula_entry_cases.py).

#include <memory>
#include <string>
#include <string_view>

#include "sheet/calc/Formula.h"

namespace fw::sheet {
struct NumLocale;
}

namespace fw::sheet::calc {

// The separators a region types formulas with. One character each (ASCII).
struct FormulaLocale {
  char decimal = '.';
  char list = ',';      // between arguments, and the union operator
  char arrayCol = ',';  // between an array constant's columns
  char arrayRow = ';';  // between its rows
};
// As files store formulas (en-US).
const FormulaLocale& InvariantFormulaLocale();
// The region's: its decimal and list separators; an array constant's columns are separated by `\`
// when the decimal separator is a comma (Excel's English UI on en-NL), its rows by `;`. A region
// whose separators can't be told apart in a formula falls back to the invariant form.
FormulaLocale MakeFormulaLocale(const NumLocale& region);

// Typed text into the invariant form, character for character (a position in one is the same
// position in the other). False, with the position, where the text uses a separator the region
// doesn't (=1.5 or =SUM(A1,B1) with a decimal comma). `closeCalls` is set to the number of function
// calls left open at the end, which Excel closes (=SUM(1;2 is taken as =SUM(1;2)); -1 when an
// open bracket isn't a call (=(1+2 is refused).
bool ToInvariant(std::string_view typed, const FormulaLocale& region, std::string& out, size_t* errorPos = nullptr,
                 int* closeCalls = nullptr);
// The invariant form shown in the region's.
std::string ToLocal(std::string_view invariant, const FormulaLocale& region);

// A formula typed in a cell or the formula bar, compiled.
struct EnteredFormula {
  std::shared_ptr<const Formula> formula;  // null when Excel refuses it
  std::string text;                        // as files store it, with "=" (_xlfn. prefixes)
  std::string shown;                       // as the formula bar shows it back, in the region's form
  std::string message;                     // why it's refused, in Excel's words
  size_t pos = 0;                          // where, in the typed text
};
EnteredFormula EnterFormula(std::string_view typed, const FormulaLocale& region, const CompileOptions& at,
                            Names& names);

// A stored formula as the formula bar shows it in the region, with "=".
std::string ShowFormula(const Formula& f, const CompileOptions& at, const Names& names, const FormulaLocale& region);

}  // namespace fw::sheet::calc
