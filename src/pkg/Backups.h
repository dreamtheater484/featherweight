#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace fw::pkg::backups {

// Backups of originals (plan §13): the first time Featherweight saves over a file, the file as
// it was opened is kept in the backups folder for 30 days, all of them together capped at 1 GB.
// ⋯ → Restore original opens the newest one; ⋯ → Delete backups removes them all.
//
// A backup is named after its original, when it was made, and a short hash of the original's
// full path: "report 2026-11-05 143012 #1a2b3c4d.docx".

constexpr int kKeepDays = 30;
constexpr uint64_t kMaxBytes = 1ull << 30;

// Keeps `bytes`, the original at `originalPath`, as a new backup in `dir` (created if needed),
// then prunes. `now` is a FILETIME (0 for the clock). Returns the backup's path, or empty when
// it couldn't be written; a save goes ahead either way, since the save itself is safe.
std::wstring Keep(const std::wstring& dir, const std::wstring& originalPath, std::span<const uint8_t> bytes,
                  int64_t now = 0);
// The newest backup of `originalPath`, or empty.
std::wstring Latest(const std::wstring& dir, const std::wstring& originalPath);
// The original's file name a backup was made from ("report.docx"), for its tab.
std::wstring OriginalName(const std::wstring& backupPath);
// Removes backups older than `days`, then the oldest until the rest fit in `maxBytes`.
void Prune(const std::wstring& dir, int64_t now = 0, uint64_t maxBytes = kMaxBytes, int days = kKeepDays);
// Removes every backup. Returns how many there were.
size_t DeleteAll(const std::wstring& dir);
// True when there is at least one backup.
bool Any(const std::wstring& dir);

}  // namespace fw::pkg::backups
