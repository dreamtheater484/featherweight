#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "pkg/OwnerFile.h"
#include "pkg/Recovery.h"
#include "pkg/Snapshot.h"

namespace fw::doc {
class Document;
}

// The file behind a Word document tab (plan §6.3 "Saving", §13; milestone D2 "Files"): safe
// saving with a backup of the original, Word's ~$ owner file while there are unsaved edits,
// noticing when another program changes the file, and crash-recovery autosaves. DocView owns
// one and stays the tab; this keeps what happens to the file in one place.
class DocFile {
 public:
  // Where things go and who saves. The window sets this once; an empty folder turns that part off
  // (snapshots and the UI checks never write to the user's folders).
  struct Setup {
    std::wstring backups;
    std::wstring recovery;
    std::wstring user;      // the owner file and the document properties
    std::wstring language;  // new documents: the user's language, "nl-NL"
    bool a4 = false;        // new documents: A4 rather than Letter
  };
  static Setup& Config();
  // This process's place in the recovery folder, started on the first autosave.
  static fw::pkg::recovery::Session& RecoverySession();

  enum class Origin {
    File,  // opened from a file, or saved to one
    New,   // a new document, never saved
    Copy,  // a recovered document or a backup: saved with Save As
  };

  DocFile() = default;
  // A clean close: our owner file and autosave go. (After a crash they stay, and the autosave is
  // offered at the next start.)
  ~DocFile();
  DocFile(const DocFile&) = delete;
  DocFile& operator=(const DocFile&) = delete;

  // What the tab is. Opened: the file was read into a snapshot with this stamp.
  void Opened(const std::wstring& path, const fw::pkg::Snapshot::Stamp& stamp);
  void New(int number);
  // `suggested` is where Save As starts; a recovered document has unsaved changes (`dirty`).
  void Copy(std::wstring title, std::wstring suggested, bool dirty);

  Origin GetOrigin() const { return origin_; }
  const std::wstring& Path() const { return path_; }  // empty unless the tab is a file
  std::wstring Title() const;
  // Where Save As starts: the file, a recovered document's original, or for a new document the
  // first words of its text in the Documents folder.
  std::wstring SuggestedPath(const fw::doc::Document& doc) const;
  bool Dirty() const { return dirty_; }

  // Editing calls this for each change, with the index of the model part it changed. The first
  // change writes our owner file, so Word users see the file is in use.
  void Edited(uint32_t part);
  // The edit session's state after each change, undo or redo (doc::edit::Editor's Modified() and
  // DirtyParts()): undoing back to what was opened or saved makes the tab clean again, and our
  // owner file and autosave go. After a save the editor is told MarkSaved().
  void Changed(bool modified, const std::vector<uint32_t>& parts);

  // Saves to `path` (the file itself, or Save As): refused with a plain message when the file
  // changed on disk or another program has it open, so the window offers Save As instead.
  bool Save(fw::doc::Document& doc, const std::wstring& path, std::wstring& error);

  // Writes unsaved changes to the recovery folder when there are some it doesn't have yet.
  void Autosave(fw::doc::Document& doc);

  // Someone else is editing the file (checked when it was opened): a message for a banner, or empty.
  std::wstring InUseMessage() const;

  // Did the file change on disk since it was opened or saved? Each change is reported once.
  enum class DiskChange { None, Changed, Missing };
  DiskChange CheckDisk();

 private:
  void Clean();  // after a save: no owner file, no autosave

  Origin origin_ = Origin::New;
  std::wstring path_;
  std::wstring title_, suggested_;
  int number_ = 1;
  fw::pkg::Snapshot::Stamp stamp_;  // the file on disk as we last read or wrote it
  fw::pkg::InUse inUse_;
  bool backedUp_ = false;  // the original is in the backups folder (or needs none)
  bool dirty_ = false;
  bool ownerWritten_ = false;
  std::set<uint32_t> changed_;
  uint64_t generation_ = 0, autosaved_ = 0;
  std::wstring autosave_;
  uint64_t notifiedSize_ = 0;  // the last disk change reported
  int64_t notifiedTime_ = 0;
};
