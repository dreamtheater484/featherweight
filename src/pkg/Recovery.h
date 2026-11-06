#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fw::pkg::recovery {

// Crash recovery (plan §4.3 rule 6, §13). Until the edit log of rule 6 exists, a document with
// unsaved changes is written whole to the recovery folder every so often: the file as it would
// be saved, encrypted for this Windows user with DPAPI, so a password-protected document's
// content is never stored in clear. A clean save or close removes it. After a crash, the next
// start offers what was left behind.
//
// Each running Featherweight holds a lock file in the folder; autosaves carry its name. An
// autosave whose lock file nobody holds belongs to a process that ended without cleaning up.

constexpr uint32_t kVersion = 1;  // each version reads the ones before it

struct Entry {
  std::wstring file;          // the autosave in the recovery folder
  std::wstring originalPath;  // where the document lives; empty for a new one never saved
  std::wstring title;         // its tab's title
  int64_t savedAt = 0;        // FILETIME
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
// or by a newer version.
bool Read(const std::wstring& file, Entry& entry, std::vector<uint8_t>& document);
// Autosaves left behind by sessions that ended without cleaning up, oldest first. Their lock
// files are removed.
std::vector<Entry> FindOrphans(const std::wstring& dir);
void Remove(const std::wstring& file);

}  // namespace fw::pkg::recovery
