#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "sheet/model/Sheet.h"

// A sheet's page setup as Excel 2021 writes it (S3, s3-print; measured: a sheet set up in Excel and
// saved): printOptions, pageMargins, pageSetup (attributes in Excel's order, defaults left out),
// headerFooter, rowBreaks and colBreaks, and sheetPr's pageSetUpPr fitToPage. Each takes the element
// as the file has it (`existing`, empty for none) and keeps what it doesn't set (pageSetup's r:id of
// the printer settings, its dpi); `p` is the prefix the sheet's elements are written with. Empty
// output: the element goes.
namespace fw::sheet::edit::pagexml {

std::string PrintOptions(const PageSetup& page, std::string_view existing, const std::string& p);
std::string Margins(const PageMargins& m, std::string_view existing, const std::string& p);
std::string Setup(const PageSetup& page, std::string_view existing, const std::string& p);
std::string HeadersFooters(const HeaderFooter& hf, std::string_view existing, const std::string& p);
// rowBreaks (`rows`) or colBreaks.
std::string Breaks(const std::vector<PageBreak>& breaks, bool rows, const std::string& p);
// sheetPr with pageSetUpPr's fitToPage set or cleared (Excel's "Fit to" scaling).
std::string SheetPrFit(std::string_view existing, bool fitToPage, const std::string& p);

}  // namespace fw::sheet::edit::pagexml
