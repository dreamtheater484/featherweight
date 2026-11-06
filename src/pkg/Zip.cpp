#include "pkg/Zip.h"

#include <miniz.h>

#include <algorithm>
#include <cstring>
#include <numeric>
#include <unordered_set>

#include "pkg/Snapshot.h"

namespace fw::pkg {
namespace {

constexpr uint32_t kLocalSig = 0x04034b50;
constexpr uint32_t kCentralSig = 0x02014b50;
constexpr uint32_t kEndSig = 0x06054b50;
constexpr uint32_t kEnd64Sig = 0x06064b50;
constexpr uint32_t kEnd64LocatorSig = 0x07064b50;
constexpr uint32_t kDescriptorSig = 0x08074b50;
constexpr uint16_t kZip64ExtraId = 0x0001;
constexpr uint32_t kMax32 = 0xFFFFFFFF;
constexpr uint16_t kMax16 = 0xFFFF;
constexpr uint16_t kFlagEncrypted = 0x0001;
constexpr uint16_t kFlagDescriptor = 0x0008;
constexpr uint16_t kFlagStrongEncryption = 0x0040;
constexpr uint16_t kFlagUtf8 = 0x0800;
constexpr size_t kChunk = 4u << 20;

uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint64_t U64(const uint8_t* p) { return U32(p) | (static_cast<uint64_t>(U32(p + 4)) << 32); }

void Put16(std::string& s, uint16_t v) {
  s += static_cast<char>(v & 0xFF);
  s += static_cast<char>(v >> 8);
}
void Put32(std::string& s, uint32_t v) {
  Put16(s, static_cast<uint16_t>(v & 0xFFFF));
  Put16(s, static_cast<uint16_t>(v >> 16));
}
void Put64(std::string& s, uint64_t v) {
  Put32(s, static_cast<uint32_t>(v & kMax32));
  Put32(s, static_cast<uint32_t>(v >> 32));
}

Status NotAZip() {
  return Status::Fail(Status::Code::Damaged, "This isn't an Office file, or it's damaged: its zip structure can't be read.");
}

Status DamagedEntry(const std::string& name) {
  return Status::Fail(Status::Code::Damaged, "Part " + name + " is damaged.");
}

bool ReadBytes(const Snapshot& s, uint64_t offset, size_t size, std::vector<uint8_t>& out) {
  out.resize(size);
  return s.ReadAt(offset, out.data(), size);
}

// Splits the zip64 field out of an extra field and applies its values to the entry. Other fields
// stay in `rest`, in order. A malformed extra field is kept whole and not interpreted.
void ParseExtra(std::string_view extra, ZipEntry& e, std::string& rest) {
  rest.clear();
  size_t p = 0;
  while (p + 4 <= extra.size()) {
    auto* b = reinterpret_cast<const uint8_t*>(extra.data() + p);
    uint16_t id = U16(b), size = U16(b + 2);
    if (p + 4 + size > extra.size()) {
      rest.append(extra.substr(p));  // a malformed tail: kept as is
      return;
    }
    if (id == kZip64ExtraId) {
      // Only the fields whose 32-bit value is 0xFFFFFFFF are present, in this order.
      const uint8_t* q = b + 4;
      const uint8_t* end = q + size;
      for (uint64_t* field : {&e.uncompressedSize, &e.compressedSize, &e.localOffset}) {
        if (*field != kMax32 || q + 8 > end) continue;
        *field = U64(q);
        q += 8;
      }
    } else {
      rest.append(extra.substr(p, 4 + size));
    }
    p += 4 + size;
  }
  if (p < extra.size()) rest.append(extra.substr(p));
}

bool HasZip64Field(std::string_view extra) {
  size_t p = 0;
  while (p + 4 <= extra.size()) {
    auto* b = reinterpret_cast<const uint8_t*>(extra.data() + p);
    if (U16(b) == kZip64ExtraId) return true;
    p += 4 + U16(b + 2);
  }
  return false;
}

bool IsAscii(std::string_view s) {
  return std::all_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

mz_bool AppendToVector(const void* buf, int len, void* user) {
  auto* v = static_cast<std::vector<uint8_t>*>(user);
  auto* p = static_cast<const uint8_t*>(buf);
  v->insert(v->end(), p, p + len);
  return MZ_TRUE;
}

}  // namespace

// ---------- Compression ----------

std::vector<uint8_t> Deflate(std::span<const uint8_t> data, int level) {
  std::vector<uint8_t> out;
  out.reserve(data.size() / 4 + 64);
  int flags = tdefl_create_comp_flags_from_zip_params(level, -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY);
  tdefl_compress_mem_to_output(data.data(), data.size(), AppendToVector, &out, flags);
  return out;
}

bool Inflate(std::span<const uint8_t> compressed, uint64_t expectedSize, std::vector<uint8_t>& out) {
  out.resize(static_cast<size_t>(expectedSize));
  // A non-wrapping output buffer of exactly the declared size: data that would inflate to more
  // fails instead of growing, so a lying header can't make a bomb.
  size_t n = tinfl_decompress_mem_to_mem(out.data(), out.size(), compressed.data(), compressed.size(), 0);
  return n != TINFL_DECOMPRESS_MEM_TO_MEM_FAILED && n == out.size();
}

uint32_t Crc32(std::span<const uint8_t> data) {
  return static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, data.data(), data.size()));
}

// ---------- Reader ----------

Status ZipReader::Open(const Snapshot& snapshot, const Limits& limits) {
  snapshot_ = &snapshot;
  entries_.clear();
  comment_.clear();
  const uint64_t fileSize = snapshot.Size();
  if (fileSize < 22) return NotAZip();

  // End of central directory: the last record whose comment fits in the file.
  size_t tailSize = static_cast<size_t>(std::min<uint64_t>(fileSize, 22 + 0xFFFF));
  uint64_t tailStart = fileSize - tailSize;
  std::vector<uint8_t> tail;
  if (!ReadBytes(snapshot, tailStart, tailSize, tail)) return NotAZip();
  size_t eocd = SIZE_MAX;
  for (size_t i = tailSize - 22 + 1; i-- > 0;) {
    if (U32(&tail[i]) == kEndSig && i + 22 + U16(&tail[i + 20]) <= tailSize) {
      eocd = i;
      break;
    }
  }
  if (eocd == SIZE_MAX) return NotAZip();
  const uint8_t* e = &tail[eocd];
  uint64_t count = U16(e + 10);
  uint64_t cdSize = U32(e + 12);
  uint64_t cdOffset = U32(e + 16);
  comment_.assign(reinterpret_cast<const char*>(e + 22), U16(e + 20));
  bool multiDisk = U16(e + 4) != 0 || U16(e + 6) != 0 || U16(e + 8) != U16(e + 10);
  uint64_t cdLimit = tailStart + eocd;  // the central directory ends before this

  // Zip64: a locator right before the end record points at the zip64 end record.
  if (tailStart + eocd >= 20) {
    uint8_t loc[20];
    if (snapshot.ReadAt(tailStart + eocd - 20, loc, sizeof(loc)) && U32(loc) == kEnd64LocatorSig) {
      uint64_t end64 = U64(loc + 8);
      uint8_t z[56];
      if (end64 + sizeof(z) > tailStart + eocd - 20 || !snapshot.ReadAt(end64, z, sizeof(z)) ||
          U32(z) != kEnd64Sig)
        return NotAZip();
      multiDisk = multiDisk || U32(z + 16) != 0 || U32(z + 20) != 0 || U64(z + 24) != U64(z + 32);
      count = U64(z + 32);
      cdSize = U64(z + 40);
      cdOffset = U64(z + 48);
      cdLimit = end64;
    }
  }
  if (multiDisk)
    return Status::Fail(Status::Code::Unsupported, "This file is split across several files, which Office files never are.");
  if (count > limits.maxEntries)
    return Status::Fail(Status::Code::TooLarge, "This file has more than " + std::to_string(limits.maxEntries) +
                                                    " parts, which is more than Featherweight allows.");
  if (cdOffset > cdLimit || cdSize > cdLimit - cdOffset) return NotAZip();

  std::vector<uint8_t> cd;
  if (!ReadBytes(snapshot, cdOffset, static_cast<size_t>(cdSize), cd)) return NotAZip();

  entries_.reserve(static_cast<size_t>(count));
  std::unordered_set<std::string_view> names;
  names.reserve(static_cast<size_t>(count));
  uint64_t total = 0;
  size_t p = 0;
  std::string rest;
  for (uint64_t i = 0; i < count; ++i) {
    if (p + 46 > cd.size() || U32(&cd[p]) != kCentralSig) return NotAZip();
    const uint8_t* h = &cd[p];
    ZipEntry z;
    z.versionMadeBy = U16(h + 4);
    z.versionNeeded = U16(h + 6);
    z.flags = U16(h + 8);
    z.method = U16(h + 10);
    z.modTime = U16(h + 12);
    z.modDate = U16(h + 14);
    z.crc32 = U32(h + 16);
    z.compressedSize = U32(h + 20);
    z.uncompressedSize = U32(h + 24);
    size_t nameLen = U16(h + 28), extraLen = U16(h + 30), commentLen = U16(h + 32);
    uint32_t diskStart = U16(h + 34);
    z.internalAttrs = U16(h + 36);
    z.externalAttrs = U32(h + 38);
    z.localOffset = U32(h + 42);
    if (p + 46 + nameLen + extraLen + commentLen > cd.size()) return NotAZip();
    const char* var = reinterpret_cast<const char*>(h + 46);
    z.name.assign(var, nameLen);
    z.comment.assign(var + nameLen + extraLen, commentLen);
    ParseExtra(std::string_view(var + nameLen, extraLen), z, rest);
    z.extra = rest;
    p += 46 + nameLen + extraLen + commentLen;

    if (diskStart != 0 && diskStart != kMax16)
      return Status::Fail(Status::Code::Unsupported, "This file is split across several files, which Office files never are.");
    if (z.flags & (kFlagEncrypted | kFlagStrongEncryption))
      return Status::Fail(Status::Code::Unsupported,
                          "This file uses zip encryption, which Office files never use. Featherweight can't open it.");
    if (z.method != 0 && z.method != 8)
      return Status::Fail(Status::Code::Unsupported,
                          "Part " + z.name + " uses a kind of compression that Office files don't use.");
    if (z.method == 0 && z.compressedSize != z.uncompressedSize) return DamagedEntry(z.name);
    if (z.uncompressedSize > limits.maxEntrySize)
      return Status::Fail(Status::Code::TooLarge, "Part " + z.name + " unpacks to more than " +
                                                      FormatBytes(limits.maxEntrySize) +
                                                      ", which is more than Featherweight allows.");
    total += z.uncompressedSize;
    if (total > limits.maxTotalSize)
      return Status::Fail(Status::Code::TooLarge, "This file unpacks to more than " + FormatBytes(limits.maxTotalSize) +
                                                      ", which is more than Featherweight allows.");
    if (z.uncompressedSize > limits.ratioGraceSize &&
        z.uncompressedSize / std::max<uint64_t>(z.compressedSize, 1) > limits.maxCompressionRatio)
      return Status::Fail(Status::Code::TooLarge,
                          "This file is packed far more tightly than real documents are, the way harmful files "
                          "(\"zip bombs\") are made. Featherweight won't open it.");
    entries_.push_back(std::move(z));
  }
  for (const auto& z : entries_) {
    if (!names.insert(z.name).second)
      return Status::Fail(Status::Code::Damaged, "This file contains two parts named " + z.name + ".");
  }

  // Local headers: each must agree with its central record, and say where the data starts.
  for (auto& z : entries_) {
    uint8_t lh[30];
    if (z.localOffset > cdOffset || !snapshot.ReadAt(z.localOffset, lh, sizeof(lh)) || U32(lh) != kLocalSig)
      return DamagedEntry(z.name);
    size_t nameLen = U16(lh + 26), extraLen = U16(lh + 28);
    std::vector<uint8_t> var;
    if (!ReadBytes(snapshot, z.localOffset + 30, nameLen + extraLen, var)) return DamagedEntry(z.name);
    if (nameLen != z.name.size() || std::memcmp(var.data(), z.name.data(), nameLen) != 0 || U16(lh + 8) != z.method)
      return DamagedEntry(z.name);
    z.dataOffset = z.localOffset + 30 + nameLen + extraLen;
    if (z.dataOffset > cdOffset || z.compressedSize > cdOffset - z.dataOffset) return DamagedEntry(z.name);
    uint64_t dataEnd = z.dataOffset + z.compressedSize;
    uint64_t descriptor = 0;
    if (U16(lh + 6) & kFlagDescriptor) {
      // [signature] crc, compressed and uncompressed size; the sizes are 8 bytes in zip64 entries.
      bool wide = HasZip64Field(std::string_view(reinterpret_cast<const char*>(var.data()) + nameLen, extraLen));
      uint64_t sizes = wide ? 16 : 8;
      uint8_t d[24] = {};
      size_t avail = static_cast<size_t>(std::min<uint64_t>(sizeof(d), cdOffset - dataEnd));
      if (!snapshot.ReadAt(dataEnd, d, avail)) return DamagedEntry(z.name);
      auto matches = [&](const uint8_t* q, uint64_t room) {
        if (room < 4 + sizes || U32(q) != z.crc32) return false;
        uint64_t c = wide ? U64(q + 4) : U32(q + 4), u = wide ? U64(q + 12) : U32(q + 8);
        return c == z.compressedSize && u == z.uncompressedSize;
      };
      if (avail >= 4 && U32(d) == kDescriptorSig && matches(d + 4, avail - 4)) descriptor = 4 + 4 + sizes;
      else if (matches(d, avail)) descriptor = 4 + sizes;
      else return DamagedEntry(z.name);
    }
    z.recordSize = dataEnd + descriptor - z.localOffset;
  }

  // Entries must not share bytes: overlapping entries are how non-recursive zip bombs work.
  std::vector<size_t> order(entries_.size());
  std::iota(order.begin(), order.end(), size_t{0});
  std::sort(order.begin(), order.end(),
            [&](size_t a, size_t b) { return entries_[a].localOffset < entries_[b].localOffset; });
  for (size_t i = 0; i < order.size(); ++i) {
    const ZipEntry& z = entries_[order[i]];
    uint64_t end = z.localOffset + z.recordSize;
    uint64_t next = i + 1 < order.size() ? entries_[order[i + 1]].localOffset : cdOffset;
    if (end > next)
      return Status::Fail(Status::Code::Rejected,
                          "This file's parts overlap each other, the way harmful files (\"zip bombs\") are made. "
                          "Featherweight won't open it.");
  }
  return Status{};
}

const ZipEntry* ZipReader::Find(std::string_view name) const {
  for (const auto& e : entries_)
    if (e.name == name) return &e;
  return nullptr;
}

Status ZipReader::Read(const ZipEntry& entry, std::vector<uint8_t>& out) const {
  std::span<const uint8_t> all = snapshot_->Bytes();
  std::vector<uint8_t> buffer;
  std::span<const uint8_t> data;
  if (!all.empty()) {
    data = all.subspan(static_cast<size_t>(entry.dataOffset), static_cast<size_t>(entry.compressedSize));
  } else {
    if (!ReadBytes(*snapshot_, entry.dataOffset, static_cast<size_t>(entry.compressedSize), buffer))
      return DamagedEntry(entry.name);
    data = buffer;
  }
  if (entry.method == 0) {
    out.assign(data.begin(), data.end());
  } else if (!Inflate(data, entry.uncompressedSize, out)) {
    out.clear();
    return DamagedEntry(entry.name);
  }
  if (Crc32(out) != entry.crc32) {
    out.clear();
    return DamagedEntry(entry.name);
  }
  return Status{};
}

Status ZipReader::ReadRecord(const ZipEntry& entry, std::vector<uint8_t>& out) const {
  if (!ReadBytes(*snapshot_, entry.localOffset, static_cast<size_t>(entry.recordSize), out))
    return DamagedEntry(entry.name);
  return Status{};
}

// ---------- Writer ----------

bool MemorySink::Write(const void* data, size_t size) {
  auto* p = static_cast<const uint8_t*>(data);
  bytes.insert(bytes.end(), p, p + size);
  return true;
}

bool ZipWriter::Put(const void* data, size_t size) {
  if (!sink_.Write(data, size)) return false;
  offset_ += size;
  return true;
}

static Status WriteFailed() {
  return Status::Fail(Status::Code::IoError, "The file couldn't be written. The disk may be full.");
}

Status ZipWriter::CopyRaw(const ZipReader& reader, const ZipEntry& entry) {
  ZipEntry out = entry;
  out.localOffset = offset_;
  out.dataOffset = offset_ + (entry.dataOffset - entry.localOffset);
  const Snapshot& src = reader.Source();
  std::span<const uint8_t> all = src.Bytes();
  if (!all.empty()) {
    if (!Put(all.data() + entry.localOffset, static_cast<size_t>(entry.recordSize))) return WriteFailed();
  } else {
    std::vector<uint8_t> buffer(static_cast<size_t>(std::min<uint64_t>(entry.recordSize, kChunk)));
    for (uint64_t done = 0; done < entry.recordSize;) {
      size_t n = static_cast<size_t>(std::min<uint64_t>(entry.recordSize - done, buffer.size()));
      if (!src.ReadAt(entry.localOffset + done, buffer.data(), n)) return DamagedEntry(entry.name);
      if (!Put(buffer.data(), n)) return WriteFailed();
      done += n;
    }
  }
  written_.push_back(std::move(out));
  return Status{};
}

Status ZipWriter::Add(std::string_view name, std::span<const uint8_t> data, const ZipEntry* like,
                      Compression compression) {
  ZipEntry e;
  if (like) {
    e.versionMadeBy = like->versionMadeBy;
    e.flags = like->flags & kFlagUtf8;
    e.modTime = like->modTime;
    e.modDate = like->modDate;
    e.internalAttrs = like->internalAttrs;
    e.externalAttrs = like->externalAttrs;
    e.extra = like->extra;
    e.comment = like->comment;
  }
  if (!IsAscii(name)) e.flags |= kFlagUtf8;
  e.name.assign(name);
  e.uncompressedSize = data.size();
  e.crc32 = Crc32(data);
  std::vector<uint8_t> packed;
  std::span<const uint8_t> body = data;
  if (compression == Compression::Deflate) {
    packed = Deflate(data);
    body = packed;
    e.method = 8;
  } else {
    e.method = 0;
  }
  e.compressedSize = body.size();
  const bool zip64 = e.uncompressedSize >= kMax32 || e.compressedSize >= kMax32;
  e.versionNeeded = zip64 ? 45 : 20;

  std::string h;
  Put32(h, kLocalSig);
  Put16(h, e.versionNeeded);
  Put16(h, e.flags);
  Put16(h, e.method);
  Put16(h, e.modTime);
  Put16(h, e.modDate);
  Put32(h, e.crc32);
  Put32(h, zip64 ? kMax32 : static_cast<uint32_t>(e.compressedSize));
  Put32(h, zip64 ? kMax32 : static_cast<uint32_t>(e.uncompressedSize));
  Put16(h, static_cast<uint16_t>(e.name.size()));
  Put16(h, zip64 ? 20 : 0);
  h += e.name;
  if (zip64) {
    Put16(h, kZip64ExtraId);
    Put16(h, 16);
    Put64(h, e.uncompressedSize);
    Put64(h, e.compressedSize);
  }
  e.localOffset = offset_;
  e.dataOffset = offset_ + h.size();
  e.recordSize = h.size() + body.size();
  if (!Put(h.data(), h.size()) || !Put(body.data(), body.size())) return WriteFailed();
  written_.push_back(std::move(e));
  return Status{};
}

Status ZipWriter::Finish(std::string_view comment) {
  if (finished_) return Status{};
  finished_ = true;
  const uint64_t cdOffset = offset_;
  std::string cd;
  for (const auto& e : written_) {
    std::string z64;
    if (e.uncompressedSize >= kMax32) Put64(z64, e.uncompressedSize);
    if (e.compressedSize >= kMax32) Put64(z64, e.compressedSize);
    if (e.localOffset >= kMax32) Put64(z64, e.localOffset);
    std::string extra;
    if (!z64.empty()) {
      Put16(extra, kZip64ExtraId);
      Put16(extra, static_cast<uint16_t>(z64.size()));
      extra += z64;
    }
    extra += e.extra;
    Put32(cd, kCentralSig);
    Put16(cd, e.versionMadeBy);
    Put16(cd, z64.empty() ? e.versionNeeded : std::max<uint16_t>(e.versionNeeded, 45));
    Put16(cd, e.flags);
    Put16(cd, e.method);
    Put16(cd, e.modTime);
    Put16(cd, e.modDate);
    Put32(cd, e.crc32);
    Put32(cd, static_cast<uint32_t>(std::min<uint64_t>(e.compressedSize, kMax32)));
    Put32(cd, static_cast<uint32_t>(std::min<uint64_t>(e.uncompressedSize, kMax32)));
    Put16(cd, static_cast<uint16_t>(e.name.size()));
    Put16(cd, static_cast<uint16_t>(std::min<size_t>(extra.size(), kMax16)));
    Put16(cd, static_cast<uint16_t>(std::min<size_t>(e.comment.size(), kMax16)));
    Put16(cd, 0);  // disk
    Put16(cd, e.internalAttrs);
    Put32(cd, e.externalAttrs);
    Put32(cd, static_cast<uint32_t>(std::min<uint64_t>(e.localOffset, kMax32)));
    cd += e.name;
    cd.append(extra, 0, std::min<size_t>(extra.size(), kMax16));
    cd.append(e.comment, 0, std::min<size_t>(e.comment.size(), kMax16));
  }
  const uint64_t cdSize = cd.size();
  const uint64_t count = written_.size();
  std::string end;
  if (count >= kMax16 || cdOffset >= kMax32 || cdSize >= kMax32) {
    const uint64_t end64 = cdOffset + cdSize;
    Put32(end, kEnd64Sig);
    Put64(end, 44);
    Put16(end, 45);
    Put16(end, 45);
    Put32(end, 0);
    Put32(end, 0);
    Put64(end, count);
    Put64(end, count);
    Put64(end, cdSize);
    Put64(end, cdOffset);
    Put32(end, kEnd64LocatorSig);
    Put32(end, 0);
    Put64(end, end64);
    Put32(end, 1);
  }
  Put32(end, kEndSig);
  Put16(end, 0);
  Put16(end, 0);
  Put16(end, static_cast<uint16_t>(std::min<uint64_t>(count, kMax16)));
  Put16(end, static_cast<uint16_t>(std::min<uint64_t>(count, kMax16)));
  Put32(end, static_cast<uint32_t>(std::min<uint64_t>(cdSize, kMax32)));
  Put32(end, static_cast<uint32_t>(std::min<uint64_t>(cdOffset, kMax32)));
  comment = comment.substr(0, std::min<size_t>(comment.size(), kMax16));
  Put16(end, static_cast<uint16_t>(comment.size()));
  end += comment;
  if (!Put(cd.data(), cd.size()) || !Put(end.data(), end.size())) return WriteFailed();
  return Status{};
}

}  // namespace fw::pkg
