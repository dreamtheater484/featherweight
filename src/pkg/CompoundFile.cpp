#include "pkg/CompoundFile.h"

#include <algorithm>
#include <cstring>

#include "core/Strings.h"

namespace fw::pkg {
namespace {

// Sector numbers with a meaning of their own.
constexpr uint32_t kFree = 0xFFFFFFFF;
constexpr uint32_t kEnd = 0xFFFFFFFE;  // the end of a chain
constexpr uint32_t kFatSector = 0xFFFFFFFD;
constexpr uint32_t kDifatSector = 0xFFFFFFFC;
constexpr uint32_t kMaxSector = 0xFFFFFFFA;
constexpr uint32_t kNone = 0xFFFFFFFF;  // no sibling or child in the directory
constexpr uint32_t kMiniCutoff = 4096;  // smaller streams live in the mini stream
constexpr uint32_t kMiniSize = 64;
constexpr uint8_t kSignature[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};

uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t U32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t U64(const uint8_t* p) { return U32(p) | uint64_t(U32(p + 4)) << 32; }

// Follows a chain through `table` from `start`: exactly `count` links, or with no count to its
// end. False when a link leaves the table, the chain ends early or it loops.
bool Follow(const std::vector<uint32_t>& table, uint32_t start, std::optional<uint64_t> count,
            std::vector<uint32_t>& out) {
  out.clear();
  if (count && *count > table.size()) return false;
  std::vector<bool> seen(table.size());
  for (uint32_t at = start; count ? out.size() < *count : at != kEnd; at = table[at]) {
    if (at >= table.size() || seen[at]) return false;
    seen[at] = true;
    out.push_back(at);
  }
  return true;
}

char16_t Fold(char16_t c) { return c >= u'a' && c <= u'z' ? char16_t(c - u'a' + u'A') : c; }

// Paths compare ignoring ASCII case. Folding UTF-8 bytes this way folds the same letters as
// folding the UTF-16 names would.
bool SamePath(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (Fold(char16_t(uint8_t(a[i]))) != Fold(char16_t(uint8_t(b[i])))) return false;
  return true;
}

std::string Utf8(const std::u16string& s) {
  return fw::WideToUtf8(std::wstring_view(reinterpret_cast<const wchar_t*>(s.data()), s.size()));
}

}  // namespace

bool CompoundFile::HasSignature(std::span<const uint8_t> bytes) {
  return bytes.size() >= sizeof(kSignature) && std::memcmp(bytes.data(), kSignature, sizeof(kSignature)) == 0;
}

bool CompoundFile::Read(uint32_t sector, uint32_t at, uint32_t length, uint8_t* out) const {
  if (sector >= sectors_) return false;
  // The last sector may be cut short; what's missing reads as zeros.
  const uint64_t from = ((uint64_t(sector) + 1) << shift_) + at;
  const uint64_t have = from < bytes_.size() ? std::min<uint64_t>(length, bytes_.size() - from) : 0;
  if (have) std::memcpy(out, bytes_.data() + from, size_t(have));
  std::memset(out + have, 0, size_t(length - have));
  return true;
}

std::unique_ptr<CompoundFile> CompoundFile::Open(std::span<const uint8_t> bytes) {
  if (bytes.size() < 512 || !HasSignature(bytes)) return nullptr;
  const uint8_t* h = bytes.data();
  const uint16_t major = U16(h + 0x1A), shift = U16(h + 0x1E);
  if (U16(h + 0x1C) != 0xFFFE || U16(h + 0x20) != 6 || U32(h + 0x38) != kMiniCutoff) return nullptr;
  if (!((major == 3 && shift == 9) || (major == 4 && shift == 12))) return nullptr;
  const uint32_t size = 1u << shift, per = size / 4;
  if (bytes.size() <= size) return nullptr;  // the header takes the first sector's space

  std::unique_ptr<CompoundFile> cf(new CompoundFile);
  cf->bytes_ = bytes;
  cf->shift_ = shift;
  cf->v3_ = major == 3;
  cf->sectors_ = uint32_t(std::min<uint64_t>((bytes.size() - 1) / size, uint64_t(kMaxSector) + 1));
  std::vector<uint8_t> buf(size);

  // The FAT's sectors: 109 in the header, the rest in the DIFAT chain.
  const uint32_t fatCount = U32(h + 0x2C);
  if (fatCount == 0 || fatCount > cf->sectors_) return nullptr;
  std::vector<uint32_t> fatSectors;
  for (uint32_t i = 0; i < 109 && fatSectors.size() < fatCount; ++i) fatSectors.push_back(U32(h + 0x4C + 4 * i));
  std::vector<bool> seen(cf->sectors_);
  for (uint32_t difat = U32(h + 0x44); fatSectors.size() < fatCount; difat = U32(&buf[size - 4])) {
    if (difat >= cf->sectors_ || seen[difat]) return nullptr;
    seen[difat] = true;
    cf->Read(difat, 0, size, buf.data());
    for (uint32_t i = 0; i + 1 < per && fatSectors.size() < fatCount; ++i) fatSectors.push_back(U32(&buf[4 * i]));
  }
  // Only sectors in the file can be in a chain.
  cf->fat_.assign(size_t(std::min<uint64_t>(uint64_t(fatCount) * per, cf->sectors_)), kFree);
  for (uint32_t i = 0; uint64_t(i) * per < cf->fat_.size(); ++i) {
    if (!cf->Read(fatSectors[i], 0, size, buf.data())) return nullptr;
    const size_t first = size_t(i) * per;
    for (size_t k = 0; k < per && first + k < cf->fat_.size(); ++k) cf->fat_[first + k] = U32(&buf[4 * k]);
  }

  // The directory.
  std::vector<uint32_t> chain;
  if (!Follow(cf->fat_, U32(h + 0x30), std::nullopt, chain) || chain.empty()) return nullptr;
  for (uint32_t s : chain) {
    cf->Read(s, 0, size, buf.data());
    for (uint32_t at = 0; at < size; at += 128) {
      const uint8_t* e = &buf[at];
      Entry d;
      const size_t chars = std::min<size_t>(U16(e + 0x40) / 2, 32);
      for (size_t i = 0; i < chars && U16(e + 2 * i); ++i) d.name.push_back(char16_t(U16(e + 2 * i)));
      d.type = e[0x42];
      d.left = U32(e + 0x44);
      d.right = U32(e + 0x48);
      d.child = U32(e + 0x4C);
      d.start = U32(e + 0x74);
      d.size = cf->v3_ ? U32(e + 0x78) : U64(e + 0x78);  // version 3 ignores the high half
      cf->dir_.push_back(std::move(d));
    }
  }
  if (cf->dir_[0].type != 5) return nullptr;

  // The mini FAT, and the mini stream (the root's) that it divides.
  const uint32_t miniFatStart = U32(h + 0x3C);
  if (miniFatStart != kEnd && miniFatStart != kFree) {
    if (!Follow(cf->fat_, miniFatStart, std::nullopt, chain)) return nullptr;
    cf->miniFat_.reserve(chain.size() * per);
    for (uint32_t s : chain) {
      cf->Read(s, 0, size, buf.data());
      for (uint32_t k = 0; k < per; ++k) cf->miniFat_.push_back(U32(&buf[4 * k]));
    }
  }
  const Entry& root = cf->dir_[0];
  if (root.size > bytes.size()) return nullptr;
  if (root.size && !Follow(cf->fat_, root.start, (root.size + size - 1) >> shift, cf->miniSectors_)) return nullptr;

  // Each storage's children are a binary tree (left, right); a link that leaves the directory or
  // comes back to an entry ends that branch.
  std::vector<bool> visited(cf->dir_.size());
  visited[0] = true;
  std::vector<std::pair<uint32_t, std::string>> storages{{root.child, std::string()}};
  std::vector<uint32_t> stack;
  for (size_t k = 0; k < storages.size(); ++k) {
    const std::string prefix = storages[k].second;
    uint32_t at = storages[k].first;
    for (;;) {
      while (at < cf->dir_.size() && !visited[at]) {
        visited[at] = true;
        stack.push_back(at);
        at = cf->dir_[at].left;
      }
      if (stack.empty()) break;
      const Entry& e = cf->dir_[stack.back()];
      const uint32_t index = stack.back();
      stack.pop_back();
      const std::string name = Utf8(e.name);
      if (!name.empty() && name.find('/') == std::string::npos) {
        if (e.type == 2) cf->streams_.emplace_back(prefix + name, index);
        if (e.type == 1) storages.emplace_back(e.child, prefix + name + "/");
      }
      at = e.right;
    }
  }
  return cf;
}

std::optional<std::vector<uint8_t>> CompoundFile::Stream(std::string_view path) const {
  const auto it = std::find_if(streams_.begin(), streams_.end(), [&](const auto& s) { return SamePath(s.first, path); });
  if (it == streams_.end()) return std::nullopt;
  const Entry& e = dir_[it->second];
  std::vector<uint8_t> out;
  if (e.size == 0) return out;
  if (e.size > bytes_.size()) return std::nullopt;
  std::vector<uint32_t> chain;
  const uint32_t size = 1u << shift_;
  if (e.size < kMiniCutoff) {
    if (!Follow(miniFat_, e.start, (e.size + kMiniSize - 1) / kMiniSize, chain)) return std::nullopt;
    out.resize(size_t(e.size));
    size_t done = 0;
    for (uint32_t m : chain) {
      const uint64_t at = uint64_t(m) * kMiniSize;
      const uint32_t n = uint32_t(std::min<uint64_t>(kMiniSize, e.size - done));
      if ((at >> shift_) >= miniSectors_.size() ||
          !Read(miniSectors_[size_t(at >> shift_)], uint32_t(at & (size - 1)), n, out.data() + done))
        return std::nullopt;
      done += n;
    }
    return out;
  }
  if (!Follow(fat_, e.start, (e.size + size - 1) >> shift_, chain)) return std::nullopt;
  out.resize(size_t(e.size));
  size_t done = 0;
  for (uint32_t s : chain) {
    const uint32_t n = uint32_t(std::min<uint64_t>(size, e.size - done));
    Read(s, 0, n, out.data() + done);
    done += n;
  }
  return out;
}

std::vector<std::string> CompoundFile::Streams() const {
  std::vector<std::string> out;
  for (const auto& s : streams_) out.push_back(s.first);
  return out;
}

// ---------------------------------------------------------------------------
// Writing

namespace {

struct Node {
  std::u16string name;
  uint8_t type = 1;
  std::vector<uint32_t> kids;
  std::span<const uint8_t> data;
  uint32_t left = kNone, right = kNone, child = kNone, start = kEnd;
  bool red = false;
};

// CFB's order: shorter names first, then by letters ignoring case.
bool Before(const std::u16string& a, const std::u16string& b) {
  if (a.size() != b.size()) return a.size() < b.size();
  for (size_t i = 0; i < a.size(); ++i)
    if (Fold(a[i]) != Fold(b[i])) return Fold(a[i]) < Fold(b[i]);
  return false;
}

// A balanced tree of kids[lo, hi). Built from the middle, every empty link is at depth `red` or
// one deeper, so making the nodes at depth `red` red gives each path as many black nodes: a
// red-black tree, as CFB asks.
uint32_t Tree(std::vector<Node>& nodes, const std::vector<uint32_t>& kids, size_t lo, size_t hi, int depth, int red) {
  if (lo >= hi) return kNone;
  const size_t mid = lo + (hi - lo) / 2;
  Node& n = nodes[kids[mid]];
  n.red = depth == red;
  n.left = Tree(nodes, kids, lo, mid, depth + 1, red);
  n.right = Tree(nodes, kids, mid + 1, hi, depth + 1, red);
  return kids[mid];
}

}  // namespace

std::vector<uint8_t> WriteCompoundFile(const std::vector<CompoundStream>& streams) {
  std::vector<Node> nodes(1);
  nodes[0].name = u"Root Entry";
  nodes[0].type = 5;
  for (const auto& s : streams) {
    uint32_t parent = 0;
    for (size_t from = 0;;) {
      const size_t slash = s.path.find('/', from);
      const std::wstring w = fw::Utf8ToWide(std::string_view(s.path).substr(from, slash - from));
      const std::u16string name(w.begin(), w.begin() + std::min<size_t>(w.size(), 31));
      auto it = std::find_if(nodes[parent].kids.begin(), nodes[parent].kids.end(),
                             [&](uint32_t k) { return nodes[k].name == name; });
      uint32_t at = it != nodes[parent].kids.end() ? *it : uint32_t(nodes.size());
      if (at == nodes.size()) {
        nodes.emplace_back().name = name;
        nodes[parent].kids.push_back(at);
      }
      if (slash == std::string::npos) {
        nodes[at].type = 2;
        nodes[at].data = s.data;
        break;
      }
      parent = at;
      from = slash + 1;
    }
  }
  for (size_t i = 0; i < nodes.size(); ++i) {
    std::vector<uint32_t> kids = nodes[i].kids;
    std::sort(kids.begin(), kids.end(), [&](uint32_t a, uint32_t b) { return Before(nodes[a].name, nodes[b].name); });
    int red = 0;
    while ((size_t(2) << red) <= kids.size() + 1) ++red;
    nodes[i].child = Tree(nodes, kids, 0, kids.size(), 0, red);
  }

  // Small streams go in the mini stream, 64 bytes a sector.
  std::vector<uint8_t> mini;
  std::vector<uint32_t> miniFat;
  uint64_t bigSectors = 0;
  for (Node& n : nodes) {
    if (n.type != 2 || n.data.empty()) continue;
    if (n.data.size() >= kMiniCutoff) {
      bigSectors += (n.data.size() + 511) / 512;
      continue;
    }
    n.start = uint32_t(miniFat.size());
    const uint32_t count = uint32_t((n.data.size() + kMiniSize - 1) / kMiniSize);
    for (uint32_t k = 0; k < count; ++k) miniFat.push_back(k + 1 < count ? n.start + k + 1 : kEnd);
    mini.insert(mini.end(), n.data.begin(), n.data.end());
    mini.resize(miniFat.size() * kMiniSize);
  }

  // Sectors: the FAT, the DIFAT, the directory, the mini FAT, the mini stream, then the big
  // streams, each in one run. The FAT has to cover itself and the DIFAT too.
  const uint32_t dirSectors = uint32_t((nodes.size() + 3) / 4);
  const uint32_t miniFatSectors = uint32_t((miniFat.size() + 127) / 128);
  const uint32_t miniSectors = uint32_t((mini.size() + 511) / 512);
  const uint64_t data = dirSectors + miniFatSectors + miniSectors + bigSectors;
  uint32_t fat = 1, difat = 0;
  for (;;) {
    difat = fat > 109 ? (fat - 109 + 126) / 127 : 0;
    const uint32_t need = uint32_t((data + fat + difat + 127) / 128);
    if (need <= fat) break;
    fat = need;
  }
  std::vector<uint32_t> table(size_t(fat) * 128, kFree);
  uint32_t next = 0;
  auto mark = [&](uint32_t count, uint32_t value) {
    const uint32_t first = next;
    for (uint32_t i = 0; i < count; ++i) table[next++] = value;
    return first;
  };
  auto run = [&](uint64_t count) {
    const uint32_t first = next;
    for (uint64_t i = 0; i < count; ++i, ++next) table[next] = i + 1 < count ? next + 1 : kEnd;
    return count ? first : kEnd;
  };
  const uint32_t fatStart = mark(fat, kFatSector);
  const uint32_t difatStart = mark(difat, kDifatSector);
  const uint32_t dirStart = run(dirSectors);
  const uint32_t miniFatStart = run(miniFatSectors);
  const uint32_t miniStart = run(miniSectors);
  for (Node& n : nodes)
    if (n.type == 2 && n.data.size() >= kMiniCutoff) n.start = run((n.data.size() + 511) / 512);

  std::vector<uint8_t> out((size_t(next) + 1) * 512);
  auto put16 = [&](size_t at, uint16_t v) { out[at] = uint8_t(v), out[at + 1] = uint8_t(v >> 8); };
  auto put32 = [&](size_t at, uint32_t v) { put16(at, uint16_t(v)), put16(at + 2, uint16_t(v >> 16)); };
  auto sector = [](uint32_t s) { return (size_t(s) + 1) * 512; };

  std::memcpy(out.data(), kSignature, sizeof(kSignature));
  put16(0x18, 0x3E);
  put16(0x1A, 3);
  put16(0x1C, 0xFFFE);
  put16(0x1E, 9);
  put16(0x20, 6);
  put32(0x2C, fat);
  put32(0x30, dirStart);
  put32(0x38, kMiniCutoff);
  put32(0x3C, miniFatStart);
  put32(0x40, miniFatSectors);
  put32(0x44, difat ? difatStart : kEnd);
  put32(0x48, difat);
  for (uint32_t i = 0; i < 109; ++i) put32(0x4C + 4 * i, i < fat ? fatStart + i : kFree);
  for (uint32_t d = 0; d < difat; ++d) {
    for (uint32_t k = 0; k < 127; ++k) {
      const uint32_t i = 109 + d * 127 + k;
      put32(sector(difatStart + d) + 4 * k, i < fat ? fatStart + i : kFree);
    }
    put32(sector(difatStart + d) + 508, d + 1 < difat ? difatStart + d + 1 : kEnd);
  }
  for (size_t i = 0; i < table.size(); ++i) put32(sector(fatStart) + 4 * i, table[i]);

  for (size_t i = 0; i < size_t(dirSectors) * 4; ++i) {
    const size_t at = sector(dirStart) + 128 * i;
    if (i >= nodes.size()) {  // unused
      put32(at + 0x44, kNone), put32(at + 0x48, kNone), put32(at + 0x4C, kNone);
      continue;
    }
    const Node& n = nodes[i];
    for (size_t c = 0; c < n.name.size(); ++c) put16(at + 2 * c, n.name[c]);
    put16(at + 0x40, uint16_t((n.name.size() + 1) * 2));
    out[at + 0x42] = n.type;
    out[at + 0x43] = n.red ? 0 : 1;
    put32(at + 0x44, n.left);
    put32(at + 0x48, n.right);
    put32(at + 0x4C, n.child);
    const uint64_t length = n.type == 5 ? mini.size() : n.type == 2 ? n.data.size() : 0;
    put32(at + 0x74, n.type == 5 ? miniStart : n.type == 2 ? n.start : 0);
    put32(at + 0x78, uint32_t(length));
    put32(at + 0x7C, uint32_t(length >> 32));
  }
  for (size_t i = 0; i < size_t(miniFatSectors) * 128; ++i)
    put32(sector(miniFatStart) + 4 * i, i < miniFat.size() ? miniFat[i] : kFree);
  if (!mini.empty()) std::memcpy(&out[sector(miniStart)], mini.data(), mini.size());
  for (const Node& n : nodes)
    if (n.type == 2 && n.data.size() >= kMiniCutoff) std::memcpy(&out[sector(n.start)], n.data.data(), n.data.size());
  return out;
}

}  // namespace fw::pkg
