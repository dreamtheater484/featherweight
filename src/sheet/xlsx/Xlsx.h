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
// changed, written from the model, the parts left out (deleted sheets and what only they used, the
// calculation chain, macros in a macro-free format), and the parts added (new sheets, a shared
// strings part). Package::Save copies every other part raw.
struct SavePlan {
  pkg::Package::SaveChanges changes;
  // Sheets written from the model, with the text around their cells; `added` since opening.
  struct SheetJob {
    const Sheet* sheet = nullptr;
    std::string part, relId;
    detail::PartText text;
    bool added = false;
  };
  std::vector<SheetJob> sheets;
  bool strings = false;        // the shared strings are written from the model
  std::string stringsPart;     // ... into this part
  bool newStringsPart = false;  // which the workbook didn't have
  bool strict = false;          // the workbook is Strict Open XML: new parts are written in its namespaces
  bool sheetsRemoved = false;
  FileFormat format = FileFormat::Xlsx;
  std::vector<std::string> edited;  // parts the package holds as edits changed them (written as they are)
};

// A save, in three steps so that the writing runs on a worker thread (plan §4.3):
//  1. on the UI thread: editor.BeginSave(), then PrepareSave works out what changes and writes the
//     small parts (the workbook, styles, relationships, content types);
//  2. on any thread: WriteSave writes the sheets and the shared strings from the model and the
//     whole package into `sink` (the workbook is read-only meanwhile: Editor::Saving);
//  3. on the UI thread, once the bytes are safely in the file: CommitSave makes the workbook
//     stand for the file written; on failure, editor.EndSave() instead.
// `format` is what the file becomes: .xlsx and .xltx leave out macros (refused when the workbook
// has macro sheets), .xlsm and .xltm keep them.
pkg::Status PrepareSave(const Workbook& book, const edit::Editor& editor, SavePlan& plan, FileFormat format);
inline pkg::Status PrepareSave(const Workbook& book, const edit::Editor& editor, SavePlan& plan) {
  return PrepareSave(book, editor, plan, book.Format());
}
pkg::Status WriteSave(const Workbook& book, SavePlan& plan, pkg::ByteSink& sink);
void CommitSave(Workbook& book, edit::Editor& editor, SavePlan& plan);
// Every part a plan writes or leaves out (before CommitSave, which empties the plan).
std::vector<std::string> WrittenParts(const SavePlan& plan);

// Whether a workbook has macros a macro-free format (.xlsx, .xltx) would leave out: a VBA
// project or macro sheets. Save As asks first.
bool HasMacros(const Workbook& book);

// A new, empty workbook as Excel starts one: one sheet, Sheet1, in Calibri 11. It has a package of
// its own (the smallest that Excel opens), so it is edited and saved like any other.
std::unique_ptr<Workbook> NewWorkbook(pkg::Status& status);

// A workbook with no package (one opened from a CSV file) gets the smallest one, with no sheets
// and empty style lists, so it is saved as an Excel workbook like any other: PrepareSave writes
// every sheet, the styles and the strings from the model while its format is still CSV. A book
// that has a package keeps it.
pkg::Status GiveBasePackage(Workbook& book);

}  // namespace fw::sheet::xlsx
