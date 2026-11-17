#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fw::pkg::recovery {

// Crash recovery (plan §4.3 rule 6, §13). A document with unsaved changes is written to the
// recovery folder every so often: whole (the file as it would be saved), or as a journal of its
// edits (version 2, below), encrypted for this Windows user with DPAPI, so a password-protected
// document's content is never stored in clear. A clean save or close removes it. After a crash, the next
// start offers what was left behind.
//
// Each running Featherweight holds a lock file in the folder; autosaves carry its name. An
// autosave whose lock file nobody holds belongs to a process that ended without cleaning up.

constexpr uint32_t kVersion = 2;  // each version reads the ones before it

struct Entry {
  std::wstring file;          // the autosave in the recovery folder
  std::wstring originalPath;  // where the document lives; empty for a new one never saved
  std::wstring title;         // its tab's title
  int64_t savedAt = 0;        // FILETIME
  // A journal (version 2): the document is as it was opened, and the edits since come apart
  // (ReadJournal), with what its tab needs to open the document again (`meta`, its own format).
  bool journal = false;
  std::string meta;
};

// This process's place in the recovery folder.
class Session {
 public:
  Session() = default;
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // Creates the folder and takes this session's lock. False when the folder can't be used.
  bool Start(const std::wstring& dir);
  bool Started() const { return lock_ != nullptr; }
  const std::wstring& Dir() const { return dir_; }
  // A new autosave file name for one document.
  std::wstring NewFile();
  // Releases the lock and removes this session's autosaves (a clean exit).
  void End();

 private:
  std::wstring dir_;
  std::wstring prefix_;  // "session-<pid>-<ticks>"
  void* lock_ = nullptr;
  uint32_t next_ = 0;
};

// Writes an autosave atomically (a temporary file, then a rename).
bool Write(const std::wstring& file, const std::wstring& originalPath, const std::wstring& title,
           std::span<const uint8_t> document, int64_t now = 0);
// Reads an autosave back. False when it isn't one, is damaged, was made by another Windows user
// or by a newer version, or is a journal (which needs ReadJournal: its document lacks the edits).
bool Read(const std::wstring& file, Entry& entry, std::vector<uint8_t>& document);

// Plan §4.3 rule 6, version 2: a journal instead of whole autosaves. StartJournal writes the
// document as it was opened (the file's bytes, at the first edit); AppendJournal adds the edits
// recorded since (sheet/edit/Journal.h: Journal::Take), every few seconds, each piece encrypted on
// its own. A piece cut short by a crash is left out when read; the pieces before it count.
bool StartJournal(const std::wstring& file, const std::wstring& originalPath, const std::wstring& title,
                  std::span<const uint8_t> document, std::string_view meta = {}, int64_t now = 0);
bool AppendJournal(const std::wstring& file, std::span<const uint8_t> records, int64_t now = 0);
// Reads a journal or an autosave: the document, and for a journal the edits' records (all pieces,
// in order; empty for an autosave). `entry.savedAt` is when the last piece was written.
bool ReadJournal(const std::wstring& file, Entry& entry, std::vector<uint8_t>& document, std::vector<uint8_t>& records);
// Autosaves left behind by sessions that ended without cleaning up, oldest first. Their lock
// files are removed.
std::vector<Entry> FindOrphans(const std::wstring& dir);
void Remove(const std::wstring& file);

}  // namespace fw::pkg::recovery
