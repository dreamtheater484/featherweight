#pragma once

#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/Lang.h"
#include "pkg/OfficeCrypto.h"
#include "pkg/OwnerFile.h"
#include "pkg/Recovery.h"
#include "pkg/SafeFile.h"
#include "pkg/Snapshot.h"

// The file behind a document tab, whatever the document (plan §6.3 "Saving", §13): safe saving
// with a backup of the original, the owner file (~$) while there are unsaved edits, noticing when
// another program changes the file, and crash-recovery autosaves. DocFile (Word documents) and
// WorkbookFile (workbooks) add how their document is written.
class FileSession {
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

  // `newName` titles a new document ("Document " -> "Document 1", "Book" -> "Book1"); `owner` is
  // the owner file's naming (Word's or Excel's).
  explicit FileSession(std::wstring newName = T(L"Document "), fw::pkg::OwnerStyle owner = fw::pkg::OwnerStyle::Word)
      : newName_(std::move(newName)), owner_(owner) {}
  // A clean close: our owner file and autosave go. (After a crash they stay, and the autosave is
  // offered at the next start.)
  ~FileSession();
  FileSession(const FileSession&) = delete;
  FileSession& operator=(const FileSession&) = delete;

  // What the tab is. Opened: the file was read into a snapshot with this stamp.
  void Opened(const std::wstring& path, const fw::pkg::Snapshot::Stamp& stamp);
  void New(int number);
  // `suggested` is where Save As starts; a recovered document has unsaved changes (`dirty`).
  void Copy(std::wstring title, std::wstring suggested, bool dirty);

  Origin GetOrigin() const { return origin_; }
  const std::wstring& Path() const { return path_; }  // empty unless the tab is a file
  std::wstring Title() const;
  bool Dirty() const { return dirty_; }

  // Editing calls this for each change, with the index of the model part it changed. The first
  // change writes our owner file, so Word and Excel users see the file is in use.
  void Edited(uint32_t part);
  // The edit session's state after each change, undo or redo (the editor's Modified() and, for
  // documents, its DirtyParts()): undoing back to what was opened or saved makes the tab clean
  // again, and our owner file and autosave go.
  void Changed(bool modified, const std::vector<uint32_t>& parts);

  // Saving, in three steps so that the writing can run on another thread:
  //  1. BeginSave, on the UI thread: refused with a plain message when the file changed on disk or
  //     another program has it open (the window offers Save As instead);
  //  2. WriteSave, on any thread: encrypts a protected document, keeps the original in the backups
  //     the first time the file is replaced, and writes the bytes safely (pkg::SafeWrite);
  //  3. SaveDone, on the UI thread after a successful write: the tab is the file written.
  struct SaveJob {
    std::wstring target, name;    // the full path, and its file name for messages
    bool keepOriginal = false;    // replacing the file opened: the original goes to the backups
    bool keepExisting = false;    // Save As over another file: that one goes to the backups
    std::wstring backups, opened;  // the backups folder; the file as opened
    const fw::pkg::crypto::Key* key = nullptr;  // a protected document's: saved encrypted with it
  };
  bool BeginSave(const std::wstring& path, SaveJob& job, std::wstring& error);
  // `opened` is the snapshot of the file as it was opened (the backup, unless the file is
  // encrypted: then the file itself is kept).
  static fw::pkg::SaveResult WriteSave(const SaveJob& job, std::vector<uint8_t> bytes,
                                       const fw::pkg::Snapshot* opened);
  void SaveDone(const SaveJob& job, const fw::pkg::SaveResult& saved);

  // Autosaves: one is due when there are unsaved changes the recovery folder doesn't have yet.
  bool AutosaveDue() const;
  // Writes it (encrypted with the document's key when it is protected).
  void WriteAutosave(std::vector<uint8_t> document);
  // A recovered password-protected document still waiting for its password: its file as it is.
  void AutosaveProtected(std::span<const uint8_t> file);
  // The journal kind of autosave (pkg/Recovery.h version 2; workbooks: sheet/edit/Journal.h), when
  // one is due: `opened` is the file as it was opened, `meta` what the tab needs to open it again,
  // `all` every record so far and `unwritten` those the recovery file doesn't have. True when they
  // are in the file now (the tab then marks them written). The file starts again after a save
  // (which removes it) from `opened` and `all`. Not for a protected document: WriteAutosave.
  bool WriteJournal(std::span<const uint8_t> opened, std::string_view meta, std::string_view all,
                    std::string_view unwritten);
  // The recovery file is a journal with the opened bytes in it: WriteJournal won't read `opened`
  // (a tab whose file isn't in memory reads it only when this is false).
  bool JournalStarted() const { return journal_ && !autosave_.empty(); }

  // A password-protected document (D5): saves, backups and autosaves stay encrypted with this key
  // (the password the file was opened with).
  void Protect(fw::pkg::crypto::Key key) { key_ = std::move(key); }
  bool Protected() const { return !key_.Empty(); }

  // Someone else is editing the file (checked when it was opened): a message for a banner, or empty.
  std::wstring InUseMessage() const;

  // Did the file change on disk since it was opened or saved? Each change is reported once.
  enum class DiskChange { None, Changed, Missing };
  DiskChange CheckDisk();

 protected:
  const std::wstring& SuggestedBase() const { return suggested_; }
  const std::set<uint32_t>& ChangedParts() const { return changed_; }

 private:
  void Clean();  // after a save: no owner file, no autosave
  void WriteOwnerFile();
  void WriteRecovery(std::span<const uint8_t> bytes);

  std::wstring newName_;
  fw::pkg::OwnerStyle owner_;
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
  bool journal_ = false;  // autosave_ is a journal with the document as opened in it
  fw::pkg::crypto::Key key_;
  uint64_t notifiedSize_ = 0;  // the last disk change reported
  int64_t notifiedTime_ = 0;
};
