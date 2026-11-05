#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "pkg/Snapshot.h"
#include "pkg/Status.h"

namespace fw::pkg {

// Why a save failed, so the window knows what to offer next.
enum class SaveFailure : uint8_t {
  None,
  Folder,    // nothing can be written in the folder (read-only, no permission): Save As elsewhere
  InUse,     // another program holds the file, so it can't be replaced
  ReadOnly,  // the file itself is marked read-only
  DiskFull,
  Verify,    // what was written didn't read back the same
  Other,
};

struct SaveResult {
  SaveFailure failure = SaveFailure::None;
  std::string message;     // plain language (UTF-8), for a banner
  Snapshot::Stamp stamp;   // the saved file as it is on disk now, for change detection
  bool Ok() const { return failure == SaveFailure::None; }
};

// Failures the tests inject into SafeWrite, one step at a time (plan §10: disk full, a locked
// file, permissions, a crash in the middle of a save).
struct SaveFaults {
  // Crash stops after the temporary file is written, as if the process died. ReplaceHalfway
  // renames the original away and then fails, as ReplaceFileW can (ERROR_UNABLE_TO_MOVE_REPLACEMENT_2).
  enum class Step : uint8_t { None, Create, Write, Flush, Verify, Crash, Replace, ReplaceHalfway };
  Step step = Step::None;
  uint32_t error = 0;                // the Windows error the step fails with
  uint64_t writeLimit = UINT64_MAX;  // Write: bytes that reach the disk before it fails
};

// Saves `bytes` as `path` without ever damaging what is there (plan §3, §6.3 "Saving"):
//   1. writes a hidden temporary file in the same folder, flushes it to the disk and reads it back;
//   2. only then swaps it in with ReplaceFileW, which keeps the original's attributes, ACLs,
//      creation time and streams (a new file is moved into place instead).
// Any failure leaves the original exactly as it was and removes the temporary file. A crash
// leaves at most the temporary file, which the next save in that folder removes.
SaveResult SafeWrite(const std::wstring& path, std::span<const uint8_t> bytes, const SaveFaults* faults = nullptr);

// The temporary file SafeWrite uses for `path` in this process.
std::wstring SafeWriteTempPath(const std::wstring& path);

// A plain description of a Windows error, for messages (UTF-8, no trailing period or newline).
std::string SystemMessage(uint32_t error);

}  // namespace fw::pkg
