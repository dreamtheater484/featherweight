#pragma once

#include <functional>
#include <memory>
#include <string>

#include "FileSession.h"
#include "sheet/csv/Csv.h"
#include "sheet/model/Workbook.h"

namespace fw::sheet::edit {
class Editor;
}

// The file behind a workbook tab (S2 "Saving"): what FileSession does for any document (safe
// saving with a backup of the original, Excel's ~$ owner file while there are unsaved edits,
// noticing when another program changes the file, recovery autosaves), with the workbook written
// on a worker thread so the tab keeps responding (plan §4.3): sheet/xlsx/Xlsx.h's PrepareSave,
// WriteSave and CommitSave around FileSession's BeginSave, WriteSave and SaveDone.
//
// The tab destroys its WorkbookFile (or calls FinishSave) before the workbook and its editor.
class WorkbookFile : public FileSession {
 public:
  WorkbookFile();
  ~WorkbookFile();  // waits for a save still writing

  // A template (.xltx, .xltm) opens as a new workbook, as in Excel: titled after it with a number
  // ("Budget1"), and Save goes to Save As, suggesting a workbook (.xlsx, or .xlsm from .xltm)
  // beside the template.
  static bool IsTemplate(const std::wstring& path);
  void OpenedTemplate(const std::wstring& path, int number);

  // The format a path's extension names (.xlsx, .xlsm, .xltx, .xltm, .csv, .tsv); `current` for any
  // other.
  static fw::sheet::FileFormat FormatOf(const std::wstring& path, fw::sheet::FileFormat current);

  // A workbook opened from a CSV or TSV file (plan §7.1): the options it was read with. Saving it
  // to a .csv or .tsv writes the first sheet's values in that file's own way (csv::Write: its
  // encoding, byte-order mark, delimiter, quoting and line ends; rows whose values read as they did
  // are written as they were read), or as a new CSV file when there's no file to follow.
  void SetCsvOptions(const fw::sheet::csv::Options& options) { csv_ = options; }
  // The first sheet's values as a new CSV file (UTF-8 with a byte-order mark and the region's list
  // separator; a tab for .tsv), for "Save a copy as CSV": the tab stays on its own file, which the
  // caller makes sure `path` isn't.
  static bool SaveCsvCopy(const fw::sheet::Workbook& book, const std::wstring& path,
                          const fw::sheet::csv::Options& options, std::wstring& error);
  // Where Save As starts: the file, a recovered workbook's original, or "Book1.xlsx" in the
  // Documents folder.
  std::wstring SuggestedPath(const fw::sheet::Workbook& book) const;

  // Starts saving `book` to `path`, in the format its extension names. False with a plain message
  // when it can't start (the file changed on disk, another program has it, the workbook can't be
  // written in that format), and nothing changed. Otherwise the workbook is read-only
  // (Editor::Saving) until FinishSave, and `done` runs on the worker thread once the file is
  // written or the write failed: it posts a message to the window, which calls FinishSave.
  bool StartSave(fw::sheet::Workbook& book, fw::sheet::edit::Editor& editor, const std::wstring& path,
                 std::function<void()> done, std::wstring& error);
  bool SaveRunning() const { return run_ != nullptr; }
  // On the UI thread after `done` (it waits if the write is still running): true when the file
  // was saved and the workbook stands for it; false with a message, and the workbook as it was.
  bool FinishSave(std::wstring& error);
  // StartSave and FinishSave, waiting for the write (closing a tab, scripted commands, tests).
  bool Save(fw::sheet::Workbook& book, fw::sheet::edit::Editor& editor, const std::wstring& path, std::wstring& error);

  // The window's autosave timer: unsaved changes go to the recovery folder, the workbook written as
  // a save would write it (a CSV file's values as CSV), on the UI thread; the workbook stays as it
  // is. Nothing while a save runs or when the recovery folder is up to date.
  void Autosave(fw::sheet::Workbook& book, fw::sheet::edit::Editor& editor);

 private:
  struct Running;
  bool StartCsv(std::unique_ptr<Running> r, fw::sheet::Workbook& book, fw::sheet::edit::Editor& editor,
                std::function<void()> done, std::wstring& error);
  std::unique_ptr<Running> run_;
  fw::sheet::csv::Options csv_;
};
