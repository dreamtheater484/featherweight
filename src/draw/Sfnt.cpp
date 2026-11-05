#include "draw/Sfnt.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace draw::sfnt {

namespace {

// Big-endian reads, 0 past the end: a short table reads as zeros and fails a later check.
uint16_t U16(std::span<const uint8_t> b, size_t at) {
  return at + 2 <= b.size() ? static_cast<uint16_t>(b[at] << 8 | b[at + 1]) : 0;
}
int16_t S16(std::span<const uint8_t> b, size_t at) { return static_cast<int16_t>(U16(b, at)); }
uint32_t U32(std::span<const uint8_t> b, size_t at) {
  return at + 4 <= b.size() ? uint32_t{b[at]} << 24 | uint32_t{b[at + 1]} << 16 | uint32_t{b[at + 2]} << 8 | b[at + 3] : 0;
}
void Put16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
  b[at] = static_cast<uint8_t>(v >> 8);
  b[at + 1] = static_cast<uint8_t>(v);
}
void Put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}
void Append16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v >> 8));
  b.push_back(static_cast<uint8_t>(v));
}
void Append32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (24 - 8 * i)));
}

uint32_t Checksum(std::span<const uint8_t> b) {
  uint32_t sum = 0;
  for (size_t i = 0; i < b.size(); i += 4) {
    uint32_t word = 0;
    for (size_t k = 0; k < 4; ++k) word = word << 8 | (i + k < b.size() ? b[i + k] : 0);
    sum += word;
  }
  return sum;
}

using Table = std::pair<uint32_t, std::vector<uint8_t>>;

// An sfnt file of these tables: sorted by tag, each 4-byte aligned, with checksums and head's
// checkSumAdjustment.
std::vector<uint8_t> Build(uint32_t version, std::vector<Table> tables) {
  std::sort(tables.begin(), tables.end(), [](const Table& a, const Table& b) { return a.first < b.first; });
  const auto n = static_cast<uint16_t>(tables.size());
  uint16_t power = 1, log = 0;
  while (power * 2 <= n) {
    power *= 2;
    ++log;
  }
  std::vector<uint8_t> out;
  Append32(out, version);
  Append16(out, n);
  Append16(out, static_cast<uint16_t>(power * 16));
  Append16(out, log);
  Append16(out, static_cast<uint16_t>(n * 16 - power * 16));
  size_t offset = 12 + 16 * size_t{n}, head = 0;
  for (Table& t : tables) {
    if (t.first == Tag("head") && t.second.size() >= 12) Put32(t.second, 8, 0);  // checkSumAdjustment
    Append32(out, t.first);
    Append32(out, Checksum(t.second));
    Append32(out, static_cast<uint32_t>(offset));
    Append32(out, static_cast<uint32_t>(t.second.size()));
    offset += (t.second.size() + 3) & ~size_t{3};
  }
  for (const Table& t : tables) {
    if (t.first == Tag("head")) head = out.size();
    out.insert(out.end(), t.second.begin(), t.second.end());
    out.resize((out.size() + 3) & ~size_t{3}, 0);
  }
  if (head) Put32(out, head + 8, 0xB1B0AFBAu - Checksum(out));
  return out;
}

// Whether a CFF font is CID-keyed: its Top DICT has the ROS operator (12 30).
bool CidKeyed(std::span<const uint8_t> cff) {
  if (cff.size() < 4) return false;
  size_t at = cff[2];  // hdrSize
  // An INDEX: its count, offset size, offsets and data. Returns where it ends, and its first
  // object's [from, to).
  auto index = [&](size_t start, size_t& from, size_t& to) -> size_t {
    const uint16_t count = U16(cff, start);
    if (count == 0) {
      from = to = 0;
      return start + 2;
    }
    if (start + 3 > cff.size()) return 0;
    const uint8_t offSize = cff[start + 2];
    if (offSize < 1 || offSize > 4) return 0;
    auto offsetAt = [&](size_t i) {
      uint32_t v = 0;
      for (size_t k = 0; k < offSize; ++k) {
        const size_t p = start + 3 + i * offSize + k;
        v = v << 8 | (p < cff.size() ? cff[p] : 0);
      }
      return v;
    };
    const size_t data = start + 3 + (size_t{count} + 1) * offSize - 1;  // offsets count from 1
    from = data + offsetAt(0);
    to = data + offsetAt(1);
    const size_t end = data + offsetAt(count);
    return end <= cff.size() && from <= to && to <= end ? end : 0;
  };
  size_t from = 0, to = 0;
  at = index(at, from, to);  // the Name INDEX
  if (!at || !index(at, from, to) || from >= to) return false;  // the Top DICT INDEX: its first DICT
  // Operands until the first operator, which in a CID-keyed font is ROS.
  for (size_t p = from; p < to;) {
    const uint8_t b = cff[p];
    if (b == 12) return p + 1 < to && cff[p + 1] == 30;
    if (b <= 21) return false;
    if (b == 28) p += 3;
    else if (b == 29) p += 5;
    else if (b == 30) {  // a real: nibbles until one is 0xf
      for (++p; p < to; ++p)
        if ((cff[p] & 0x0F) == 0x0F || (cff[p] >> 4) == 0x0F) break;
      ++p;
    } else if (b >= 32 && b <= 246) ++p;
    else if (b >= 247 && b <= 254) p += 2;
    else ++p;
  }
  return false;
}

// The PostScript name (name ID 6), printable ASCII without PDF's delimiters.
std::string PostScriptName(std::span<const uint8_t> name) {
  const uint16_t count = U16(name, 2), strings = U16(name, 4);
  std::string best;
  int bestRank = 0;
  for (size_t i = 0; i < count && 6 + i * 12 + 12 <= name.size(); ++i) {
    const size_t r = 6 + i * 12;
    const uint16_t platform = U16(name, r), encoding = U16(name, r + 2), id = U16(name, r + 6);
    const uint16_t length = U16(name, r + 8), offset = U16(name, r + 10);
    if (id != 6 || size_t{strings} + offset + length > name.size()) continue;
    const std::span<const uint8_t> s = name.subspan(size_t{strings} + offset, length);
    std::string text;
    int rank = 0;
    if (platform == 3 && (encoding == 1 || encoding == 0)) {
      for (size_t k = 0; k + 1 < s.size(); k += 2) text += s[k] == 0 ? static_cast<char>(s[k + 1]) : '?';
      rank = 2;
    } else if (platform == 1 && encoding == 0) {
      text.assign(s.begin(), s.end());
      rank = 1;
    } else {
      continue;
    }
    if (rank <= bestRank) continue;
    std::string clean;
    for (char c : text)
      if (c > 32 && c < 127 && std::string_view("[](){}<>/%#").find(c) == std::string_view::npos) clean += c;
    if (!clean.empty()) {
      best = clean.substr(0, 63);
      bestRank = rank;
    }
  }
  return best;
}

}  // namespace

Tables FileTables(std::span<const uint8_t> file) {
  struct Record {
    uint32_t tag, offset, length;
  };
  std::vector<Record> records;
  size_t start = 0;
  if (U32(file, 0) == Tag("ttcf")) start = U32(file, 12);
  const uint16_t n = U16(file, start + 4);
  for (size_t i = 0; i < n; ++i) {
    const size_t r = start + 12 + i * 16;
    if (r + 16 > file.size()) break;
    const uint32_t offset = U32(file, r + 8), length = U32(file, r + 12);
    if (uint64_t{offset} + length <= file.size()) records.push_back({U32(file, r), offset, length});
  }
  return [file, records = std::move(records)](uint32_t tag, std::vector<uint8_t>& data) {
    for (const Record& r : records)
      if (r.tag == tag) {
        data.assign(file.begin() + r.offset, file.begin() + r.offset + r.length);
        return true;
      }
    return false;
  };
}

std::optional<Info> ReadInfo(const Tables& tables) {
  std::vector<uint8_t> head, maxp, hhea, t;
  if (!tables(Tag("head"), head) || head.size() < 54 || U32(head, 12) != 0x5F0F3CF5 || !tables(Tag("maxp"), maxp) ||
      maxp.size() < 6 || !tables(Tag("hhea"), hhea) || hhea.size() < 36) {
    return std::nullopt;
  }
  Info info;
  info.unitsPerEm = U16(head, 18);
  if (info.unitsPerEm < 16 || info.unitsPerEm > 16384) return std::nullopt;
  info.xMin = S16(head, 36);
  info.yMin = S16(head, 38);
  info.xMax = S16(head, 40);
  info.yMax = S16(head, 42);
  info.numGlyphs = U16(maxp, 4);
  info.ascent = S16(hhea, 4);
  info.descent = S16(hhea, 6);
  info.capHeight = info.ascent;
  if (tables(Tag("CFF "), t)) {
    info.cff = true;
    info.cidKeyed = CidKeyed(t);
  } else if (!tables(Tag("glyf"), t) || !tables(Tag("loca"), t)) {
    return std::nullopt;  // no outlines we can embed
  }
  info.variable = tables(Tag("fvar"), t);
  if (tables(Tag("OS/2"), t) && t.size() >= 10) {
    const uint16_t fsType = U16(t, 8);
    info.embeddable = (fsType & 0x000E) != 0x0002 && !(fsType & 0x0200);
    info.subsettable = !(fsType & 0x0100);
    info.weight = U16(t, 4);
    if (U16(t, 0) >= 2 && t.size() >= 90 && S16(t, 88) > 0) info.capHeight = S16(t, 88);
  }
  if (tables(Tag("post"), t) && t.size() >= 8) info.italicAngle = static_cast<int32_t>(U32(t, 4)) / 65536.f;
  if (tables(Tag("name"), t)) info.postScriptName = PostScriptName(t);
  return info;
}

std::vector<uint8_t> Subset(const Tables& tables, const std::vector<uint16_t>& glyphs) {
  std::vector<uint8_t> head, maxp, hhea, hmtx, loca, glyf;
  if (!tables(Tag("head"), head) || head.size() < 54 || !tables(Tag("maxp"), maxp) || maxp.size() < 6 ||
      !tables(Tag("hhea"), hhea) || hhea.size() < 36 || !tables(Tag("hmtx"), hmtx) || !tables(Tag("loca"), loca) ||
      !tables(Tag("glyf"), glyf)) {
    return {};
  }
  const uint16_t numGlyphs = U16(maxp, 4);
  const bool longLoca = S16(head, 50) == 1;
  if (numGlyphs == 0 || loca.size() < (size_t{numGlyphs} + 1) * (longLoca ? 4 : 2)) return {};
  auto range = [&](uint16_t g, uint32_t& from, uint32_t& to) {
    from = longLoca ? U32(loca, size_t{g} * 4) : uint32_t{U16(loca, size_t{g} * 2)} * 2;
    to = longLoca ? U32(loca, size_t{g} * 4 + 4) : uint32_t{U16(loca, size_t{g} * 2 + 2)} * 2;
    return from <= to && to <= glyf.size();
  };

  // The glyphs to keep: those asked for, glyph 0, and the parts of composite glyphs.
  std::vector<uint8_t> keep(numGlyphs, 0);
  std::vector<uint16_t> todo{0};
  for (uint16_t g : glyphs)
    if (g < numGlyphs) todo.push_back(g);
  while (!todo.empty()) {
    const uint16_t g = todo.back();
    todo.pop_back();
    if (keep[g]) continue;
    keep[g] = 1;
    uint32_t from = 0, to = 0;
    if (!range(g, from, to) || to - from < 10 || S16(glyf, from) >= 0) continue;
    // A composite: components until one without MORE_COMPONENTS.
    for (size_t p = from + 10; p + 4 <= to;) {
      const uint16_t flags = U16(glyf, p), part = U16(glyf, p + 2);
      if (part < numGlyphs && !keep[part]) todo.push_back(part);
      p += 4 + ((flags & 0x0001) ? 4 : 2);  // ARG_1_AND_2_ARE_WORDS
      if (flags & 0x0008) p += 2;           // WE_HAVE_A_SCALE
      else if (flags & 0x0040) p += 4;      // WE_HAVE_AN_X_AND_Y_SCALE
      else if (flags & 0x0080) p += 8;      // WE_HAVE_A_TWO_BY_TWO
      if (!(flags & 0x0020)) break;         // MORE_COMPONENTS
    }
  }

  // glyf with only those glyphs; loca (long) for every glyph id; the others' metrics zeroed, so the
  // tables compress to little.
  std::vector<uint8_t> newGlyf, newLoca;
  newLoca.reserve((size_t{numGlyphs} + 1) * 4);
  const uint16_t metrics = std::min<uint16_t>(U16(hhea, 34), numGlyphs);
  for (uint16_t g = 0; g < numGlyphs; ++g) {
    Append32(newLoca, static_cast<uint32_t>(newGlyf.size()));
    uint32_t from = 0, to = 0;
    if (keep[g] && range(g, from, to)) {
      newGlyf.insert(newGlyf.end(), glyf.begin() + from, glyf.begin() + to);
      newGlyf.resize((newGlyf.size() + 3) & ~size_t{3}, 0);
    } else if (!keep[g]) {
      const size_t m = g < metrics ? size_t{g} * 4 : size_t{metrics} * 4 + (size_t{g} - metrics) * 2;
      const size_t len = g < metrics ? 4 : 2;
      if (m + len <= hmtx.size()) std::fill(hmtx.begin() + m, hmtx.begin() + m + len, 0);
    }
  }
  Append32(newLoca, static_cast<uint32_t>(newGlyf.size()));
  Put16(head, 50, 1);  // indexToLocFormat: long

  std::vector<Table> out{{Tag("head"), std::move(head)}, {Tag("hhea"), std::move(hhea)}, {Tag("maxp"), std::move(maxp)},
                         {Tag("hmtx"), std::move(hmtx)}, {Tag("loca"), std::move(newLoca)},
                         {Tag("glyf"), std::move(newGlyf)}};
  // The hinting programs, which glyphs' instructions call.
  for (uint32_t tag : {Tag("cvt "), Tag("fpgm"), Tag("prep")}) {
    std::vector<uint8_t> t;
    if (tables(tag, t)) out.push_back({tag, std::move(t)});
  }
  return Build(0x00010000, std::move(out));
}

std::vector<uint8_t> Whole(const Tables& tables) {
  std::vector<Table> out;
  for (uint32_t tag : {Tag("CFF "), Tag("head"), Tag("hhea"), Tag("hmtx"), Tag("maxp"), Tag("cmap"), Tag("OS/2"),
                       Tag("name"), Tag("post")}) {
    std::vector<uint8_t> t;
    if (!tables(tag, t)) return {};
    out.push_back({tag, std::move(t)});
  }
  return Build(Tag("OTTO"), std::move(out));
}

}  // namespace draw::sfnt
