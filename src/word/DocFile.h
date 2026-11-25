#pragma once

#include <string>

#include "FileSession.h"
#include "core/Lang.h"

namespace fw::doc {
class Document;
}

// The file behind a Word document tab (plan §6.3 "Saving", §13; milestone D2 "Files"): what
// FileSession does for any document (safe saving with a backup of the original, Word's ~$ owner
// file while there are unsaved edits, noticing when another program changes the file, crash-
// recovery autosaves), with the document written as .docx. DocView owns one and stays the tab.
class DocFile : public FileSession {
 public:
  DocFile() : FileSession(T(L"Document {0}")) {}

  // Where Save As starts: the file, a recovered document's original, or for a new document the
  // first words of its text in the Documents folder.
  std::wstring SuggestedPath(const fw::doc::Document& doc) const;

  // Saves to `path` (the file itself, or Save As): refused with a plain message when the file
  // changed on disk or another program has it open, so the window offers Save As instead. After a
  // save the editor is told MarkSaved().
  bool Save(fw::doc::Document& doc, const std::wstring& path, std::wstring& error);

  // Writes unsaved changes to the recovery folder when there are some it doesn't have yet.
  void Autosave(fw::doc::Document& doc);
  using FileSession::AutosaveProtected;
};
