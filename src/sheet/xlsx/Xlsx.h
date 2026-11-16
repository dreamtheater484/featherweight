#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pkg/Package.h"
#include "pkg/Status.h"
#include "sheet/model/Workbook.h"

// The .xlsx reader and, for the round-trip tests, its writer (plan §7.3). Saving comes in S2.
namespace fw::sheet::xlsx {

struct ReadOptions {
  // Read every sheet now instead of the active one only (tools, tests, fuzzing).
  bool loadAll = false;
};

// Reads a workbook (.xlsx, .xlsm, .xltx, .xltm) from an opened package: the workbook part, the
// styles, the theme, the shared strings and the active sheet; the workbook reads the others with
// Workbook::LoadSheet. The workbook keeps the package. Fails with a plain message when the package
// isn't a workbook, or when a part it reads isn't well-formed or exceeds the package's limits.
std::unique_ptr<Workbook> Read(std::unique_ptr<pkg::Package> package, pkg::Status& status,
                               const ReadOptions& options = {});

// The parts the model writes back: the shared strings and every loaded sheet. The workbook part
// and the styles are read into the model and kept as they were until S2 writes them.
std::vector<std::string> ModelParts(const Workbook& book);

// Writes one of those parts from the model and what it keeps as written (plan §4.3 rule 1).
pkg::Status WritePart(const Workbook& book, std::string_view partName, std::string& out);

}  // namespace fw::sheet::xlsx
