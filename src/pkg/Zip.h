#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pkg/Limits.h"
#include "pkg/Status.h"

namespace fw::pkg {

class Snapshot;

// One entry of a zip archive, as its central directory describes it.
struct ZipEntry {
  std::string name;  // bytes as stored: UTF-8 when flag bit 11 is set
  uint16_t versionMadeBy = 20;
  uint16_t versionNeeded = 20;
  uint16_t flags = 0;
  uint16_t method = 8;      // 0 stored, 8 deflated
  uint16_t modTime = 0;
  uint16_t modDate = 0x21;  // 1980-01-01, as Office writes it
  uint32_t crc32 = 0;
  uint64_t compressedSize = 0;
  uint64_t uncompressedSize = 0;
  uint16_t internalAttrs = 0;
  uint32_t externalAttrs = 0;
  std::string extra;  // central directory extra field without the zip64 field, which is rebuilt
  std::string comment;

  // Where the entry lives in its archive.
  uint64_t localOffset = 0;  // local file header
  uint64_t dataOffset = 0;   // compressed data
  uint64_t recordSize = 0;   // local header + compressed data + data descriptor

  bool IsDirectory() const { return !name.empty() && name.back() == '/'; }
};

// Reads a zip archive from a snapshot. The container is parsed here rather than by miniz, so every
// limit is checked before anything is allocated or decompressed.
class ZipReader {
 public:
  // Reads and checks the central directory and every local header: the limits, overlapping
  // entries (non-recursive zip bombs), duplicate names, encryption and compression methods.
  Status Open(const Snapshot& snapshot, const Limits& limits = Limits::Default());

  const std::vector<ZipEntry>& Entries() const { return entries_; }
  const ZipEntry* Find(std::string_view name) const;
  const std::string& Comment() const { return comment_; }
  const Snapshot& Source() const { return *snapshot_; }

  // Decompresses an entry, checking its size and CRC-32.
  Status Read(const ZipEntry& entry, std::vector<uint8_t>& out) const;
  // The local header, compressed data and data descriptor, exactly as stored.
  Status ReadRecord(const ZipEntry& entry, std::vector<uint8_t>& out) const;

 private:
  const Snapshot* snapshot_ = nullptr;
  std::vector<ZipEntry> entries_;
  std::string comment_;
};

// Where a ZipWriter puts its bytes.
class ByteSink {
 public:
  virtual ~ByteSink() = default;
  virtual bool Write(const void* data, size_t size) = 0;
};

class MemorySink : public ByteSink {
 public:
  bool Write(const void* data, size_t size) override;
  std::vector<uint8_t> bytes;
};

class ZipWriter {
 public:
  enum class Compression { Deflate, Store };

  explicit ZipWriter(ByteSink& sink) : sink_(sink) {}

  // Copies an entry exactly as stored in `reader`'s archive: local header, compressed data and
  // data descriptor, byte for byte. Nothing is decompressed or recompressed.
  Status CopyRaw(const ZipReader& reader, const ZipEntry& entry);
  // Adds an entry with new content. When it replaces an entry, pass that one as `like`: its time,
  // attributes and extra fields are kept, so only the content changes.
  Status Add(std::string_view name, std::span<const uint8_t> data, const ZipEntry* like = nullptr,
             Compression compression = Compression::Deflate);
  // Writes the central directory. Nothing can be added after this.
  Status Finish(std::string_view comment = {});

  const std::vector<ZipEntry>& Written() const { return written_; }

 private:
  bool Put(const void* data, size_t size);

  ByteSink& sink_;
  uint64_t offset_ = 0;
  std::vector<ZipEntry> written_;  // localOffset and dataOffset are positions in the new archive
  bool finished_ = false;
};

// Raw deflate, as zip entries use it.
std::vector<uint8_t> Deflate(std::span<const uint8_t> data, int level = 6);
// Inflates into exactly `expectedSize` bytes. False if the data is damaged or a different size.
bool Inflate(std::span<const uint8_t> compressed, uint64_t expectedSize, std::vector<uint8_t>& out);
uint32_t Crc32(std::span<const uint8_t> data);

}  // namespace fw::pkg
