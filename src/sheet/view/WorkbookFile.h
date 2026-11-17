#pragma once

#include <functional>
#include <memory>
#include <string>

#include "FileSession.h"
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

  // The format a path's extension names (.xlsx, .xlsm, .xltx, .xltm); `current` for any other.
  static fw::sheet::FileFormat FormatOf(const std::wstring& path, fw::sheet::FileFormat current);
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

 private:
  struct Running;
  std::unique_ptr<Running> run_;
};
