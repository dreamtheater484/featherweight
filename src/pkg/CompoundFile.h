#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// [MS-CFB] compound files (OLE structured storage): the container of .doc files and of
// password-protected Office files (milestone D5). Untrusted input: every sector number, chain and
// size is checked, and a damaged file gives nullptr or nullopt, never a crash or a hang.
namespace fw::pkg {

class CompoundFile {
 public:
  // True when `bytes` starts with the compound file signature.
  static bool HasSignature(std::span<const uint8_t> bytes);

  // Reads the header, the FAT, the mini FAT and the directory. Nullptr when `bytes` isn't a
  // compound file or they are damaged. Streams are read from `bytes` later, so it must stay alive
  // and unchanged while the CompoundFile is used.
  static std::unique_ptr<CompoundFile> Open(std::span<const uint8_t> bytes);
  // Not from a temporary, which would be gone before the streams are read.
  static std::unique_ptr<CompoundFile> Open(std::vector<uint8_t>&&) = delete;

  // A stream by its path from the root, with storages separated by '/': "WordDocument",
  // "ObjectPool/_1234/\x01Ole". Names are UTF-8 and compared ignoring ASCII case, as CFB compares
  // them. Nullopt when there's no such stream or its sectors are damaged.
  std::optional<std::vector<uint8_t>> Stream(std::string_view path) const;
  // Every stream's path, in the directory's order (storages first, as they nest).
  std::vector<std::string> Streams() const;

 private:
  struct Entry {
    std::u16string name;
    uint8_t type = 0;  // 1 storage, 2 stream, 5 root
    uint32_t left = 0, right = 0, child = 0, start = 0;
    uint64_t size = 0;
  };

  CompoundFile() = default;
  bool Read(uint32_t sector, uint32_t at, uint32_t length, uint8_t* out) const;

  std::span<const uint8_t> bytes_;
  uint32_t shift_ = 9;  // sector size 512 (version 3) or 4096 (version 4)
  uint32_t sectors_ = 0;
  bool v3_ = true;
  std::vector<uint32_t> fat_, miniFat_, miniSectors_;  // miniSectors_: the root's chain
  std::vector<Entry> dir_;
  std::vector<std::pair<std::string, uint32_t>> streams_;  // path and directory entry
};

// A compound file (version 3, 512-byte sectors) holding these streams. Storages are made from the
// paths ("\x06" "DataSpaces/Version" makes the storage "\x06" "DataSpaces"). Used to save
// password-protected files.
struct CompoundStream {
  std::string path;  // UTF-8, '/' between storages
  std::span<const uint8_t> data;
};
std::vector<uint8_t> WriteCompoundFile(const std::vector<CompoundStream>& streams);

}  // namespace fw::pkg
