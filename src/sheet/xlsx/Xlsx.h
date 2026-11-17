#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pkg/Package.h"
#include "pkg/Status.h"
#include "sheet/model/Workbook.h"

namespace fw::sheet::edit {
class Editor;
}

// The .xlsx reader, the writer the round-trip tests use, and saving an edited workbook (plan §7.3).
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

// ---- Saving an edited workbook (S2)

// What a save writes differently from the file the workbook was read from: the parts the edits
// changed, written from the model, the parts left out (deleted sheets, the calculation chain), and
// the parts added (new sheets, a shared strings part). Package::Save copies every other part raw.
struct SavePlan {
  pkg::Package::SaveChanges changes;
  struct NewSheet {
    const Sheet* sheet = nullptr;
    std::string part, relId;
  };
  std::vector<NewSheet> newSheets;
  std::string newStringsPart;  // when the workbook had none
};

// Works out a save of `book` as `editor` changed it. Reads the package, changes nothing: a worker
// thread can then write the package with plan.changes while the workbook stays as it is.
pkg::Status PrepareSave(const Workbook& book, const edit::Editor& editor, SavePlan& plan);

}  // namespace fw::sheet::xlsx
