// Embedded OpenType and MicroType Express (W3C member submissions, 2008): the fonts PowerPoint
// embeds in presentations (ppt/fonts/*.fntdata), as Windows' t2embed writes them. Checked against
// t2embed's own output: every glyph, its instructions and the cvt come back as the font had them.
//
// Untrusted input: every read is bounds-checked, every loop is bounded by the input or a cap, and
// anything that doesn't hold together gives an empty result.
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <string>

#include "draw/Sfnt.h"

namespace draw::sfnt {

namespace {

constexpr size_t kMaxOutput = 64u << 20;    // a decoded font
constexpr uint32_t kFlagCompressed = 0x4, kFlagXor = 0x10000000;

uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t U32(const uint8_t* p) { return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3]; }
void Put16(std::vector<uint8_t>& o, uint32_t v) {
  o.push_back(static_cast<uint8_t>(v >> 8));
  o.push_back(static_cast<uint8_t>(v));
}
void Put32(std::vector<uint8_t>& o, uint32_t v) {
  Put16(o, v >> 16);
  Put16(o, v & 0xFFFF);
}

// ---- LZCOMP: LZ77 with three adaptive Huffman coders, over a preloaded window

class BitReader {
 public:
  explicit BitReader(std::span<const uint8_t> d) : d_(d) {}
  int Bit() {
    size_t byte = pos_ >> 3;
    if (byte >= d_.size()) {
      failed_ = true;
      return 0;
    }
    int b = (d_[byte] >> (7 - (pos_ & 7))) & 1;
    ++pos_;
    return b;
  }
  uint32_t Value(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) v = v << 1 | static_cast<uint32_t>(Bit());
    return v;
  }
  bool Failed() const { return failed_; }

 private:
  std::span<const uint8_t> d_;
  size_t pos_ = 0;
  bool failed_ = false;
};

// An adaptive Huffman tree of n symbols: nodes 1..2n-1, the root 1, symbol s at first in leaf n+s;
// a node's weight never falls below a later node's (the sibling property).
class AHuff {
 public:
  AHuff(BitReader& in, int n) : in_(in), n_(n), nodes_(2 * static_cast<size_t>(n)), leaf_(static_cast<size_t>(n)) {
    for (int i = 2; i < 2 * n; ++i) {
      nodes_[i].up = i / 2;
      nodes_[i].weight = 1;
    }
    for (int i = 1; i < n; ++i) {
      nodes_[i].left = 2 * i;
      nodes_[i].right = 2 * i + 1;
    }
    for (int i = 0; i < n; ++i) {
      nodes_[n + i].code = i;
      leaf_[i] = n + i;
    }
    nodes_[0].weight = INT64_MAX;  // stops the scans in Update
    for (int i = n - 1; i >= 1; --i) nodes_[i].weight = nodes_[nodes_[i].left].weight + nodes_[nodes_[i].right].weight;
    if (n > 256 && n < 512) {
      Update(leaf_[256]);
      Update(leaf_[257]);
      for (int i = 0; i < 12; ++i) Update(leaf_[n - 3]);
      for (int i = 0; i < 6; ++i) Update(leaf_[n - 2]);
    } else {
      for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < n; ++i) Update(leaf_[i]);
    }
  }

  int Read() {
    int a = 1;
    // A path is at most n long; a damaged tree can't loop.
    for (int depth = 0; depth < 2 * n_; ++depth) {
      a = in_.Bit() ? nodes_[a].right : nodes_[a].left;
      if (a <= 0 || a >= 2 * n_) return -1;
      if (nodes_[a].code >= 0) {
        int s = nodes_[a].code;
        Update(a);
        return in_.Failed() ? -1 : s;
      }
    }
    return -1;
  }

 private:
  struct Node {
    int up = 0, left = 0, right = 0, code = -1;
    int64_t weight = 0;
  };

  void Swap(int a, int b) {
    int upa = nodes_[a].up, upb = nodes_[b].up;
    std::swap(nodes_[a], nodes_[b]);
    nodes_[a].up = upa;
    nodes_[b].up = upb;
    for (int x : {a, b}) {
      if (nodes_[x].code < 0) {
        nodes_[nodes_[x].left].up = x;
        nodes_[nodes_[x].right].up = x;
      } else {
        leaf_[nodes_[x].code] = x;
      }
    }
  }

  void Update(int a) {
    while (a != 1) {
      int64_t w = nodes_[a].weight;
      int b = a - 1;
      if (nodes_[b].weight == w) {
        while (nodes_[b].weight == w) --b;
        ++b;
        if (b > 1) {
          Swap(a, b);
          a = b;
        }
      }
      nodes_[a].weight = w + 1;
      a = nodes_[a].up;
    }
    ++nodes_[a].weight;
  }

  BitReader& in_;
  int n_;
  std::vector<Node> nodes_;
  std::vector<int> leaf_;
};

constexpr size_t kPreload = 2 * 32 * 96 + 4 * 256;

// One LZCOMP block: its output, or false.
bool Lzcomp(std::span<const uint8_t> data, int version, std::vector<uint8_t>& out) {
  BitReader in(data);
  const bool rle = version != 1 && in.Bit();
  AHuff dist(in, 8), len(in, 8);
  const uint32_t outLen = in.Value(24);
  if (in.Failed()) return false;
  int ranges = 1;
  while ((uint64_t{1} << (3 * ranges)) < outLen) ++ranges;
  const int dup2 = 256 + 8 * ranges;
  AHuff sym(in, dup2 + 3);
  std::vector<uint8_t> buf;
  buf.reserve(kPreload + outLen);
  for (int k = 0; k < 32; ++k) {
    for (int j = 0; j < 96; ++j) {
      buf.push_back(static_cast<uint8_t>(k));
      buf.push_back(static_cast<uint8_t>(j));
    }
  }
  for (int j = 0; j < 256; ++j) buf.insert(buf.end(), 4, static_cast<uint8_t>(j));
  const size_t end = kPreload + outLen;
  while (buf.size() < end) {
    int s = sym.Read();
    if (s < 0) return false;
    if (s < 256) {
      buf.push_back(static_cast<uint8_t>(s));
    } else if (s >= dup2) {
      size_t back = static_cast<size_t>(s - dup2 + 1) * 2;
      buf.push_back(buf[buf.size() - back]);
    } else {
      int t = s - 256, nr = t / 8 + 1, g = t % 8;
      uint64_t n = 0;
      for (int i = 0;; ++i) {
        n = n << 2 | static_cast<uint64_t>(g & 3);
        if (!(g & 4)) break;
        if (i > 16) return false;  // longer than any block
        g = len.Read();
        if (g < 0) return false;
      }
      n += 2;
      uint64_t d = 0;
      for (int i = 0; i < nr; ++i) {
        int v = dist.Read();
        if (v < 0) return false;
        d = d << 3 | static_cast<uint64_t>(v);
      }
      uint64_t distance = d + 1;
      if (distance >= 512) ++n;
      if (distance + n - 1 > buf.size()) return false;
      size_t src = buf.size() - static_cast<size_t>(distance + n - 1);
      n = std::min<uint64_t>(n, end - buf.size());
      for (uint64_t i = 0; i < n; ++i) buf.push_back(buf[src + i]);
    }
  }
  if (!rle) {
    out.assign(buf.begin() + kPreload, buf.end());
    return true;
  }
  // Run-length coding: an escape byte, then escape 0 for itself and escape n b for n b's.
  std::span<const uint8_t> packed(buf.data() + kPreload, outLen);
  out.clear();
  if (packed.empty()) return true;
  const uint8_t esc = packed[0];
  for (size_t i = 1; i < packed.size();) {
    if (packed[i] != esc) {
      out.push_back(packed[i++]);
    } else if (i + 1 < packed.size() && packed[i + 1] == 0) {
      out.push_back(esc);
      i += 2;
    } else if (i + 2 < packed.size()) {
      if (out.size() + packed[i + 1] > kMaxOutput) return false;
      out.insert(out.end(), packed[i + 1], packed[i + 2]);
      i += 3;
    } else {
      return false;
    }
  }
  return true;
}

// ---- The compact table format

class Reader {
 public:
  explicit Reader(std::span<const uint8_t> d) : d_(d) {}
  bool Ok() const { return ok_; }
  size_t Pos() const { return pos_; }
  bool Has(size_t n) {
    if (d_.size() - pos_ < n) ok_ = false;
    return ok_;
  }
  uint8_t U8() { return Has(1) ? d_[pos_++] : 0; }
  uint8_t Peek() { return Has(1) ? d_[pos_] : 0; }
  uint16_t U16() {
    if (!Has(2)) return 0;
    uint16_t v = draw::sfnt::U16(d_.data() + pos_);
    pos_ += 2;
    return v;
  }
  int16_t S16() { return static_cast<int16_t>(U16()); }
  // 255UShort
  uint32_t U255() {
    uint8_t c = U8();
    if (c == 253) return U16();
    if (c == 254) return 506u + U8();
    if (c == 255) return 253u + U8();
    return c;
  }
  // 255Short
  int32_t S255() {
    uint8_t c = U8();
    if (c == 253) return S16();
    int sign = 1;
    if (c == 250) {
      sign = -1;
      c = U8();
    }
    int32_t v = c == 255 ? 250 + U8() : c == 254 ? 500 + U8() : c;
    return sign * v;
  }
  std::span<const uint8_t> Take(size_t n) {
    if (!Has(n)) return {};
    auto s = d_.subspan(pos_, n);
    pos_ += n;
    return s;
  }

 private:
  std::span<const uint8_t> d_;
  size_t pos_ = 0;
  bool ok_ = true;
};

// A point's flag (low 7 bits): data bytes after the flag, bits of x and y, their offsets and signs.
struct Triplet {
  uint8_t bytes, xBits, yBits;
  uint16_t dx, dy;
  int8_t xSign, ySign;
};

constexpr Triplet TripletOf(int i) {
  constexpr int8_t sx[] = {-1, 1, -1, 1}, sy[] = {-1, -1, 1, 1};
  if (i < 10) return {1, 0, 8, 0, static_cast<uint16_t>(256 * (i / 2)), 1, static_cast<int8_t>(i % 2 ? 1 : -1)};
  if (i < 20) {
    int j = i - 10;
    return {1, 8, 0, static_cast<uint16_t>(256 * (j / 2)), 0, static_cast<int8_t>(j % 2 ? 1 : -1), 1};
  }
  if (i < 84) {
    int g = i - 20;
    constexpr uint16_t o[] = {1, 17, 33, 49};
    return {1, 4, 4, o[g / 16], o[(g % 16) / 4], sx[g % 4], sy[g % 4]};
  }
  if (i < 120) {
    int g = i - 84;
    constexpr uint16_t o[] = {1, 257, 513};
    return {2, 8, 8, o[g / 12], o[(g % 12) / 4], sx[g % 4], sy[g % 4]};
  }
  if (i < 124) return {3, 12, 12, 0, 0, sx[(i - 120) % 4], sy[(i - 120) % 4]};
  return {4, 16, 16, 0, 0, sx[(i - 124) % 4], sy[(i - 124) % 4]};
}

// The push data of one glyph (hop codes expanded), as TrueType push instructions.
bool PushInstructions(Reader& push, uint32_t count, std::vector<uint8_t>& out) {
  std::vector<int32_t> values;
  values.reserve(std::min<uint32_t>(count, 65536));
  while (values.size() < count) {
    if (!push.Ok() || values.size() > 65535) return false;
    uint8_t c = push.Peek();
    if (c == 251 || c == 252) {
      push.U8();
      if (values.size() < 2) return false;
      int32_t a = values[values.size() - 2];
      values.push_back(a);
      values.push_back(push.S255());
      values.push_back(a);
      if (c == 252) {
        values.push_back(push.S255());
        values.push_back(a);
      }
    } else {
      values.push_back(push.S255());
    }
  }
  if (!push.Ok() || values.size() != count) return false;
  for (size_t at = 0; at < values.size();) {
    size_t n = std::min<size_t>(255, values.size() - at);
    bool bytes = std::all_of(values.begin() + at, values.begin() + at + n, [](int32_t v) { return v >= 0 && v <= 255; });
    if (n <= 8) {
      out.push_back(static_cast<uint8_t>((bytes ? 0xB0 : 0xB8) + n - 1));
    } else {
      out.push_back(bytes ? 0x40 : 0x41);
      out.push_back(static_cast<uint8_t>(n));
    }
    for (size_t i = at; i < at + n; ++i) {
      if (bytes) {
        out.push_back(static_cast<uint8_t>(values[i]));
      } else {
        Put16(out, static_cast<uint16_t>(static_cast<int16_t>(values[i])));
      }
    }
    at += n;
  }
  return true;
}

// The instructions of one glyph: its pushes, then its code, with their length first.
bool Instructions(Reader& ctf, Reader& push, Reader& code, std::vector<uint8_t>& glyph) {
  uint32_t pushCount = ctf.U255(), codeSize = ctf.U255();
  if (!ctf.Ok()) return false;
  std::vector<uint8_t> instr;
  if (!PushInstructions(push, pushCount, instr)) return false;
  std::span<const uint8_t> rest = code.Take(codeSize);
  if (!code.Ok()) return false;
  instr.insert(instr.end(), rest.begin(), rest.end());
  if (instr.size() > 65535) return false;
  Put16(glyph, static_cast<uint32_t>(instr.size()));
  glyph.insert(glyph.end(), instr.begin(), instr.end());
  return true;
}

// One glyph of the compact glyph table, as a TrueType glyph (empty for an empty glyph).
bool Glyph(Reader& ctf, Reader& push, Reader& code, std::vector<uint8_t>& glyph) {
  int16_t contours = ctf.S16();
  std::array<int16_t, 4> box{};
  bool haveBox = false;
  if (contours < 0) {
    for (int16_t& v : box) v = ctf.S16();
    Put16(glyph, 0xFFFF);
    for (int16_t v : box) Put16(glyph, static_cast<uint16_t>(v));
    uint16_t flags = 0;
    for (int i = 0;; ++i) {
      if (i > 1024 || !ctf.Ok()) return false;
      flags = ctf.U16();
      size_t n = 2 + (flags & 0x1 ? 4 : 2) + (flags & 0x8 ? 2 : flags & 0x40 ? 4 : flags & 0x80 ? 8 : 0);
      std::span<const uint8_t> rest = ctf.Take(n);
      if (!ctf.Ok()) return false;
      Put16(glyph, flags);
      glyph.insert(glyph.end(), rest.begin(), rest.end());
      if (!(flags & 0x20)) break;
    }
    if (flags & 0x100) return Instructions(ctf, push, code, glyph);
    return ctf.Ok();
  }
  if (contours == 0x7FFF) {
    contours = ctf.S16();
    for (int16_t& v : box) v = ctf.S16();
    haveBox = true;
    if (contours < 0) return false;
  }
  if (contours == 0) return ctf.Ok();
  std::vector<uint16_t> ends;
  ends.reserve(static_cast<size_t>(contours));
  uint32_t end = 0;
  for (int i = 0; i < contours; ++i) {
    uint32_t v = ctf.U255();
    end = i == 0 ? v : end + v;
    if (end > 65534 || !ctf.Ok()) return false;
    ends.push_back(static_cast<uint16_t>(end));
  }
  const size_t points = size_t{end} + 1;
  std::span<const uint8_t> flags = ctf.Take(points);
  if (!ctf.Ok()) return false;
  std::vector<int32_t> xs(points), ys(points);
  int32_t x = 0, y = 0;
  for (size_t i = 0; i < points; ++i) {
    Triplet t = TripletOf(flags[i] & 0x7F);
    uint32_t d = 0;
    for (int b = 0; b < t.bytes; ++b) d = d << 8 | ctf.U8();
    int bits = 8 * t.bytes;
    int32_t dx = t.dx, dy = t.dy;
    if (t.xBits) dx += static_cast<int32_t>((d >> (bits - t.xBits)) & ((1u << t.xBits) - 1));
    if (t.yBits) dy += static_cast<int32_t>((d >> (bits - t.xBits - t.yBits)) & ((1u << t.yBits) - 1));
    x += t.xSign * dx;
    y += t.ySign * dy;
    if (x < -32768 || x > 32767 || y < -32768 || y > 32767) return false;
    xs[i] = x;
    ys[i] = y;
  }
  if (!ctf.Ok()) return false;
  if (!haveBox) {
    box = {static_cast<int16_t>(*std::min_element(xs.begin(), xs.end())),
           static_cast<int16_t>(*std::min_element(ys.begin(), ys.end())),
           static_cast<int16_t>(*std::max_element(xs.begin(), xs.end())),
           static_cast<int16_t>(*std::max_element(ys.begin(), ys.end()))};
  }
  Put16(glyph, static_cast<uint16_t>(contours));
  for (int16_t v : box) Put16(glyph, static_cast<uint16_t>(v));
  for (uint16_t e : ends) Put16(glyph, e);
  if (!Instructions(ctf, push, code, glyph)) return false;
  // Flags and coordinates, each delta in one byte where it fits.
  std::vector<uint8_t> xBytes, yBytes;
  int32_t px = 0, py = 0;
  for (size_t i = 0; i < points; ++i) {
    uint8_t f = (flags[i] & 0x80) ? 0 : 1;
    int32_t dx = xs[i] - px, dy = ys[i] - py;
    px = xs[i];
    py = ys[i];
    if (dx == 0) {
      f |= 0x10;
    } else if (dx >= -255 && dx <= 255) {
      f |= 0x02 | (dx > 0 ? 0x10 : 0);
      xBytes.push_back(static_cast<uint8_t>(dx > 0 ? dx : -dx));
    } else {
      Put16(xBytes, static_cast<uint16_t>(static_cast<int16_t>(dx)));
    }
    if (dy == 0) {
      f |= 0x20;
    } else if (dy >= -255 && dy <= 255) {
      f |= 0x04 | (dy > 0 ? 0x20 : 0);
      yBytes.push_back(static_cast<uint8_t>(dy > 0 ? dy : -dy));
    } else {
      Put16(yBytes, static_cast<uint16_t>(static_cast<int16_t>(dy)));
    }
    glyph.push_back(f);
  }
  glyph.insert(glyph.end(), xBytes.begin(), xBytes.end());
  glyph.insert(glyph.end(), yBytes.begin(), yBytes.end());
  return true;
}

bool Cvt(std::span<const uint8_t> data, std::vector<uint8_t>& out) {
  Reader r(data);
  uint16_t n = r.U16();
  int32_t last = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint8_t c = r.U8();
    int32_t delta = c;
    if (c == 238) {
      delta = r.S16();
    } else if (c >= 239 && c <= 247) {
      delta = -(238 * (c - 239) + r.U8());
    } else if (c >= 248) {
      delta = 238 * (c - 247) + r.U8();
    }
    if (!r.Ok()) return false;
    last = static_cast<int16_t>(static_cast<uint16_t>(last + delta));
    Put16(out, static_cast<uint16_t>(last));
  }
  return true;
}

uint32_t Checksum(const std::vector<uint8_t>& d, size_t at, size_t n) {
  uint32_t sum = 0;
  for (size_t i = 0; i < n; i += 4) {
    uint8_t b[4] = {};
    for (size_t k = 0; k < 4 && i + k < n; ++k) b[k] = d[at + i + k];
    sum += U32(b);
  }
  return sum;
}

}  // namespace

std::vector<uint8_t> DecodeMtx(std::span<const uint8_t> data) {
  if (data.size() < 10) return {};
  const int version = data[0];
  const size_t off2 = size_t{data[4]} << 16 | size_t{data[5]} << 8 | data[6];
  const size_t off3 = size_t{data[7]} << 16 | size_t{data[8]} << 8 | data[9];
  if (off2 < 10 || off3 < off2 || off3 > data.size()) return {};
  std::vector<uint8_t> block1, block2, block3;
  if (!Lzcomp(data.subspan(10, off2 - 10), version, block1) || !Lzcomp(data.subspan(off2, off3 - off2), version, block2) ||
      !Lzcomp(data.subspan(off3), version, block3)) {
    return {};
  }

  // Block 1: an sfnt file whose glyf, loca and cvt are in compact form.
  if (block1.size() < 12) return {};
  const uint16_t count = U16(block1.data() + 4);
  if (count == 0 || count > 256 || 12 + size_t{count} * 16 > block1.size()) return {};
  std::map<uint32_t, std::span<const uint8_t>> tables;
  for (uint16_t i = 0; i < count; ++i) {
    const uint8_t* rec = block1.data() + 12 + 16 * i;
    uint32_t tag = U32(rec), offset = U32(rec + 8), length = U32(rec + 12);
    if (offset > block1.size() || length > block1.size() - offset) return {};
    tables[tag] = std::span<const uint8_t>(block1.data() + offset, length);
  }
  auto find = [&](uint32_t tag) {
    auto it = tables.find(tag);
    return it == tables.end() ? std::span<const uint8_t>() : it->second;
  };
  std::span<const uint8_t> maxp = find(Tag("maxp")), head = find(Tag("head")), glyf = find(Tag("glyf"));
  if (maxp.size() < 6 || head.size() < 54) return {};
  const uint16_t numGlyphs = U16(maxp.data() + 4);

  std::vector<uint8_t> newGlyf, newLoca, newCvt;
  if (glyf.data()) {
    Reader ctf(glyf), push(block2), code(block3);
    Put32(newLoca, 0);
    for (uint32_t g = 0; g < numGlyphs; ++g) {
      if (!Glyph(ctf, push, code, newGlyf)) return {};
      while (newGlyf.size() % 4) newGlyf.push_back(0);
      if (newGlyf.size() > kMaxOutput) return {};
      Put32(newLoca, static_cast<uint32_t>(newGlyf.size()));
    }
  }
  if (std::span<const uint8_t> cvt = find(Tag("cvt ")); cvt.data() && !Cvt(cvt, newCvt)) return {};

  // The font, its tables in tag order, 4-byte aligned.
  std::map<uint32_t, std::vector<uint8_t>> out;
  for (const auto& [tag, bytes] : tables) {
    if (tag == Tag("hdmx") || tag == Tag("VDMX") || tag == Tag("DSIG")) continue;
    if (tag == Tag("glyf")) {
      out[tag] = std::move(newGlyf);
    } else if (tag == Tag("loca")) {
      out[tag] = std::move(newLoca);
    } else if (tag == Tag("cvt ")) {
      out[tag] = std::move(newCvt);
    } else {
      out[tag].assign(bytes.begin(), bytes.end());
    }
  }
  if (glyf.data() && !out.count(Tag("loca"))) out[Tag("loca")] = std::move(newLoca);
  std::vector<uint8_t>& newHead = out[Tag("head")];
  newHead[50] = 0;  // indexToLocFormat: long offsets
  newHead[51] = 1;
  std::memset(newHead.data() + 8, 0, 4);  // checkSumAdjustment, set below
  const uint16_t n = static_cast<uint16_t>(out.size());
  uint16_t power = 1, log2 = 0;
  while (power * 2 <= n) {
    power = static_cast<uint16_t>(power * 2);
    ++log2;
  }
  std::vector<uint8_t> font;
  Put32(font, 0x00010000);
  Put16(font, n);
  Put16(font, power * 16u);
  Put16(font, log2);
  Put16(font, n * 16u - power * 16u);
  size_t offset = 12 + size_t{n} * 16;
  size_t headAt = 0;
  for (const auto& [tag, bytes] : out) {
    Put32(font, tag);
    Put32(font, 0);  // checksum, below
    Put32(font, static_cast<uint32_t>(offset));
    Put32(font, static_cast<uint32_t>(bytes.size()));
    offset += (bytes.size() + 3) & ~size_t{3};
  }
  if (offset > kMaxOutput) return {};
  size_t record = 12;
  for (const auto& [tag, bytes] : out) {
    size_t at = font.size();
    if (tag == Tag("head")) headAt = at;
    font.insert(font.end(), bytes.begin(), bytes.end());
    while (font.size() % 4) font.push_back(0);
    uint32_t sum = Checksum(font, at, bytes.size());
    for (int k = 0; k < 4; ++k) font[record + 4 + k] = static_cast<uint8_t>(sum >> (24 - 8 * k));
    record += 16;
  }
  uint32_t adjust = 0xB1B0AFBAu - Checksum(font, 0, font.size());
  for (int k = 0; k < 4; ++k) font[headAt + 8 + k] = static_cast<uint8_t>(adjust >> (24 - 8 * k));
  return font;
}

std::vector<uint8_t> DecodeEot(std::span<const uint8_t> data, EotInfo* info) {
  if (info) *info = {};
  // A plain font file.
  if (data.size() >= 4) {
    uint32_t magic = U32(data.data());
    if (magic == 0x00010000 || magic == Tag("true") || magic == Tag("OTTO") || magic == Tag("ttcf")) {
      return std::vector<uint8_t>(data.begin(), data.end());
    }
  }
  auto le16 = [&](size_t at) { return static_cast<uint32_t>(data[at] | data[at + 1] << 8); };
  auto le32 = [&](size_t at) { return le16(at) | le16(at + 2) << 16; };
  if (data.size() < 84) return {};
  const uint32_t fontSize = le32(4), version = le32(8), flags = le32(12);
  if (le16(34) != 0x504C) return {};
  if (version != 0x00010000 && version != 0x00020001 && version != 0x00020002) return {};
  EotInfo meta;
  meta.flags = flags;
  meta.italic = data[27] == 1;
  meta.weight = le32(28);
  meta.fsType = static_cast<uint16_t>(le16(32));
  // The names, each after two bytes of padding: family, style, version, full name.
  size_t pos = 80;
  auto string = [&](std::wstring* to) {
    if (pos + 4 > data.size()) return false;
    size_t n = le16(pos + 2);
    pos += 4;
    if (n > data.size() - pos) return false;
    if (to) {
      to->clear();
      for (size_t i = 0; i + 1 < n; i += 2) {
        wchar_t c = static_cast<wchar_t>(le16(pos + i));
        if (c) to->push_back(c);
      }
    }
    pos += n;
    return true;
  };
  if (!string(&meta.family) || !string(&meta.style) || !string(nullptr) || !string(&meta.fullName)) return {};
  if (version >= 0x00020001 && !string(nullptr)) return {};  // the root string
  if (version == 0x00020002) {
    pos += 8;  // root string checksum, EUDC code page
    if (!string(nullptr)) return {};  // the signature
    if (pos + 8 > data.size()) return {};
    uint32_t eudc = le32(pos + 4);
    pos += 8;
    if (eudc > data.size() - pos) return {};
    pos += eudc;
  }
  if (fontSize == 0 || fontSize > data.size() - pos) return {};
  std::vector<uint8_t> font(data.begin() + pos, data.begin() + pos + fontSize);
  if (flags & kFlagXor) {
    for (uint8_t& b : font) b ^= 0x50;
  }
  if (flags & kFlagCompressed) font = DecodeMtx(font);
  if (font.size() < 12) return {};
  uint32_t magic = U32(font.data());
  if (magic != 0x00010000 && magic != Tag("true") && magic != Tag("OTTO")) return {};
  if (info) *info = std::move(meta);
  return font;
}

}  // namespace draw::sfnt
