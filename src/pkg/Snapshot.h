#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "pkg/Status.h"

namespace fw::pkg {

using Sha256 = std::array<uint8_t, 32>;

Sha256 HashBytes(std::span<const uint8_t> bytes);

// A private copy of a file, taken when it is opened (plan §4.3 rule 3). The live file is never
// read again for content and never memory-mapped, so a sync client or another program changing it
// can't crash Featherweight or mix two versions into one file.
//
// Files up to Options::memoryLimit are read into memory. Larger ones are copied into the recovery
// folder and read from that copy, which is deleted when the snapshot goes away.
class Snapshot {
 public:
  struct Options {
    uint64_t memoryLimit = 256ull << 20;
    std::wstring recoveryDir;  // needed only for files over memoryLimit
  };

  // What the file looked like when it was read.
  struct Stamp {
    uint64_t size = 0;
    int64_t lastWrite = 0;  // FILETIME, 100 ns ticks
    Sha256 hash{};
  };

  enum class DiskState { Unchanged, Changed, Missing };

  ~Snapshot();
  Snapshot(const Snapshot&) = delete;
  Snapshot& operator=(const Snapshot&) = delete;

  static std::unique_ptr<Snapshot> Open(const std::wstring& path, const Options& options, Status& status);
  // Bytes that are already in memory (new documents, tests, fuzzers). No file behind them.
  static std::unique_ptr<Snapshot> FromBytes(std::vector<uint8_t> bytes);

  const std::wstring& Path() const { return path_; }
  const Stamp& GetStamp() const { return stamp_; }
  uint64_t Size() const { return stamp_.size; }
  bool InMemory() const { return copy_ == nullptr; }

  // The whole file, when it is in memory; empty for a copy on disk.
  std::span<const uint8_t> Bytes() const { return bytes_; }
  // Reads [offset, offset + size). False past the end or on a read error.
  bool ReadAt(uint64_t offset, void* dst, size_t size) const;

  // Compares the file on disk with the snapshot: size and last-write time, then the hash.
  // Before saving over the file, anything but Unchanged turns Save into Save As.
  DiskState CheckDisk() const;

 private:
  Snapshot() = default;

  std::wstring path_;
  Stamp stamp_;
  std::vector<uint8_t> bytes_;
  void* copy_ = nullptr;  // HANDLE of the recovery copy, opened delete-on-close
};

// Compares the file at `path` with `stamp` (what Snapshot::CheckDisk does), for a file that has
// been saved since it was opened: size and last-write time, then the hash.
Snapshot::DiskState CheckDisk(const std::wstring& path, const Snapshot::Stamp& stamp);
// Size and last-write time only: cheap enough to run each time the window is activated.
bool SameSizeAndTime(const std::wstring& path, const Snapshot::Stamp& stamp);
// The size and last-write time of the file at `path` now, with `hash` as given. False when it
// can't be read.
bool StampOf(const std::wstring& path, const Sha256& hash, Snapshot::Stamp& stamp);

}  // namespace fw::pkg
