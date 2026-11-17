#pragma once

// The engine's side of defined names (plan §7.2 "Advanced", S3): what the Name Manager lists, Excel's
// rules for a name, and a typed "Refers to" read in the region's form. Names are added, changed
// and deleted through s2-model's Editor::SetName / DeleteName (its kNames change makes WorkbookCalc
// read them again).

#include <string>
#include <string_view>
#include <vector>

#include "sheet/calc/Entry.h"
#include "sheet/model/Types.h"

namespace fw::sheet {
class Workbook;
}

namespace fw::sheet::calc {

class WorkbookCalc;

struct NameRow {
  std::string name;
  int scope = -1;        // the sheet index, or -1 for the workbook
  std::string refersTo;  // "=Sheet1!$A$1:$B$3", in the region's form
  std::string value;     // what it evaluates to now: "12", "{1;2;3}", "#REF!" ...
  bool hidden = false;
  bool builtIn = false;  // _xlnm. (Print_Area, Print_Titles, _FilterDatabase): shown as Excel names them
};
// The workbook's names, A to Z (then by scope: the workbook's first), as the Name Manager lists them.
std::vector<NameRow> ListNames(const Workbook& book, WorkbookCalc& calc, const FormulaLocale& region);

// Excel's rules for a new name (or `oldName` renamed) in `scope`: empty when it may be used, else
// the message to show.
std::u16string CheckName(const Workbook& book, std::string_view name, int scope, std::string_view oldName = {});

// A typed "Refers to" ("=Sheet1!$A$1;Sheet1!$B$2" in en-NL): the invariant formula without "=",
// for DefinedName::formula. Relative references are relative to `at` (the active cell), as Excel.
// False with `message` when it isn't a formula Excel takes.
bool ReadRefersTo(std::string_view typed, const FormulaLocale& region, const CompileOptions& at, Names& names,
                  std::string& formula, std::string& message);

// A name's formula for cells of `sheet`: "Sheet1!$A$1:$B$3", "'My Sheet'!$C$2" (one cell).
std::string RangeFormula(const Workbook& book, int sheet, const Range& cells);

// The name Excel's Create from Selection makes of a label (measured): ends trimmed of what can't
// be in a name, what's left of that inside becoming "_", "_" before a digit, and "_" after a name
// that reads as a cell (A1_, R_) or before one that reads as R1C1 with numbers (_R1C1). Empty
// when nothing's left.
std::string NameFromLabel(std::string_view label);

struct LabelName {
  std::string name, formula;
};
// Create from Selection (Ctrl+Shift+F3) on `cells` of `sheet`: names from the text labels in its
// top row, left column, bottom row and right column, each for the cells beside it without the
// other label lines; where two label lines meet, the corner's label names all the cells between.
// Two labels making the same name: the later one, as Excel.
std::vector<LabelName> NamesFromLabels(const Workbook& book, int sheet, const Range& cells, bool top, bool left,
                                       bool bottom, bool right);
// The label lines the dialog proposes: the top row when it has text right of its first cell, the
// left column when it has text below it.
void GuessLabels(const Workbook& book, int sheet, const Range& cells, bool& top, bool& left);

}  // namespace fw::sheet::calc
