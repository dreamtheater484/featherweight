#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pkg/Limits.h"
#include "pkg/Status.h"

// OpenDocument spreadsheets (.ods, and .ots templates) as Excel workbooks (S4, plan §7.1): the
// sheets, cells and their values, formulas (OpenFormula translated to Excel's), number formats,
// cell styles, merged cells, column widths and row heights, hidden rows and columns, outline
// groups, freeze panes, named ranges, print areas, filters and links, written as .xlsx bytes that
// the .xlsx reader opens (as D5 does for .odt). What Excel has no place for, or Featherweight
// doesn't read yet, is left out, but never a cell's value. The source is never written to: a
// workbook opened from one is saved with Save As, as an .xlsx.
namespace fw::sheet::ods {

pkg::Status OdsToXlsx(std::span<const uint8_t> ods, std::vector<uint8_t>& xlsx,
                      const pkg::Limits& limits = pkg::Limits::Default());

// An OpenFormula expression as ODF stores it ("of:=SUM([.A1:.B2];[Sheet2.C3])") as Excel's formula
// text in a file ("SUM(A1:B2,Sheet2!C3)", with _xlfn. prefixes). Relative references move by
// `rows` and `cols` (a formula repeated over several cells is written once). `sheet` maps an ODF
// sheet name to the name it has in the workbook, or "" when it has none (#REF!); null: unchanged. False when the expression
// can't be read; `out` is then unusable.
using SheetNames = std::function<std::string(std::string_view)>;
bool TranslateFormula(std::string_view formula, std::string& out, int rows = 0, int cols = 0,
                      const SheetNames* sheet = nullptr);
// An ODF cell or range address as table attributes write it ("$Sheet1.$A$1:.$B$3", "Sheet1.A1")
// as an Excel reference with its sheet ("Sheet1!$A$1:$B$3"). False when it isn't one.
bool TranslateAddress(std::string_view address, std::string& out, const SheetNames* sheet = nullptr);

}  // namespace fw::sheet::ods
