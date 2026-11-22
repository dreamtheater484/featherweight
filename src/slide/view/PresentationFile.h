#pragma once

#include <functional>
#include <memory>
#include <string>

#include "FileSession.h"
#include "slide/Presentation.h"
#include "slide/edit/Edit.h"
#include "slide/edit/Journal.h"

// The file behind a presentation tab (P2, p2-model; plan §4.3, §13): what FileSession does for any
// document (safe saving with a backup of the original on the first save, PowerPoint's ~$ owner file
// while there are unsaved edits, noticing when another program changes the file, recovery journals),
// with the deck written on a worker thread so the tab keeps responding: slide/edit/Save.h's
// PrepareSave, WriteSave and CommitSave around FileSession's BeginSave, WriteSave and SaveDone.
// Editing may go on while the save writes (the save holds a snapshot).
//
// The tab destroys its PresentationFile (or calls FinishSave) before the deck and its editor.
class PresentationFile : public FileSession {
 public:
  PresentationFile();
  ~PresentationFile();  // waits for a save still writing

  // The format a path's extension names (.pptx, .pptm, .ppsx, .ppsm, .potx, .potm); `current` else.
  static fw::slide::FileFormat FormatOf(const std::wstring& path, fw::slide::FileFormat current);
  // Where Save As starts: the file, a recovered deck's original, or "Presentation1.pptx" in Documents.
  std::wstring SuggestedPath(const fw::slide::Presentation& deck) const;

  // Starts saving to `path` in the format its extension names. False with a plain message when it
  // can't start (the file changed on disk, another program has it, the format can't hold the deck),
  // nothing changed. Otherwise `done` runs on the worker thread once the file is written or the write
  // failed: it posts a message to the window, which calls FinishSave.
  bool StartSave(fw::slide::edit::Editor& editor, const std::wstring& path, std::function<void()> done,
                 std::wstring& error);
  bool SaveRunning() const { return run_ != nullptr; }
  // On the UI thread after `done`: true when the file was saved and the deck stands for it (unless it
  // was edited meanwhile); false with a message.
  bool FinishSave(std::wstring& error);
  // StartSave and FinishSave, waiting for the write (closing a tab, scripted commands, tests).
  bool Save(fw::slide::edit::Editor& editor, const std::wstring& path, std::wstring& error);

  // The journal the tab's Editor records into: attach it before the first edit
  // (editor.SetJournal(file.Journal())).
  fw::slide::edit::Journal* Journal() { return &journal_; }
  // The window's autosave timer: the edits not yet in the recovery folder are appended to its journal
  // (the deck as opened, then part states). Nothing while there's nothing new.
  void Autosave(fw::slide::edit::Editor& editor);
  // Tells FileSession the deck's state after each change, undo and redo (the tab's change listener).
  void Changed(const fw::slide::edit::Editor& editor);

 private:
  struct Running;
  std::unique_ptr<Running> run_;
  fw::slide::edit::Journal journal_;
};
