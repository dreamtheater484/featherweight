#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pkg/Status.h"

// Excel 97-2003 workbooks (.xls, BIFF8: [MS-XLS]), imported (plan §7.1, S4).
namespace fw::sheet::xls {

// A stream of the compound file by name ("Workbook"), or nullopt when it has none.
// fw::pkg::CompoundFile gives them; tests and the fuzzer give them from memory.
using StreamFn = std::function<std::optional<std::vector<uint8_t>>(std::string_view name)>;

// Converts an Excel 97-2003 workbook into an .xlsx package (zip bytes) with the same content, for
// the xlsx reader, as doc/msdoc does for .doc: sheets, cells and their cached values, formulas (from
// BIFF8's parsed tokens into Excel's invariant text, so they recalculate), shared strings with rich
// text, fonts, fills, borders, alignment, number formats, the 1904 date system, defined names, the
// palette, merged cells, row heights and column widths, hidden rows and columns, outline levels,
// panes and zoom, tab colors, hyperlinks, notes, sheet protection and page setup. What it doesn't
// understand is left out (a formula it can't write keeps its value); damaged input never crashes or
// hangs. The workbook is then edited and saved as an .xlsx; the .xls itself is never written.
//
// An encrypted workbook (RC4 or RC4 CryptoAPI) is decrypted with `password`, or with Excel's default
// one ("VelvetSweatshop", which Excel uses when only the workbook's structure is protected) when it
// is empty. The .xlsx made isn't encrypted.
//
// Fails with a plain message for Excel 5.0/95 and older files and for encryption it can't read
// (Status::Unsupported), for a missing or wrong password (Status::Rejected: NeedsPassword says
// which files ask for one), and for files that aren't Excel workbooks or are damaged (Damaged).
std::vector<uint8_t> ToXlsx(const StreamFn& streams, pkg::Status& status, std::wstring_view password = {});

// Whether the workbook is encrypted with a password of its own (not Excel's default one), which
// the user must give.
bool NeedsPassword(const StreamFn& streams);

// Whether a compound file's streams look like an Excel workbook: it has a "Workbook" (BIFF8) or
// "Book" (BIFF5, refused by ToXlsx with a message) stream.
bool IsWorkbook(const StreamFn& streams);

}  // namespace fw::sheet::xls
