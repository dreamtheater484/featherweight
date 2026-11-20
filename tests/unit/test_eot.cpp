// Embedded OpenType and MicroType Express (src/draw/Eot.cpp, P1): Windows' own t2embed embeds an
// installed font twice, compressed and not; both must decode to the same glyphs, instructions and
// tables, and DirectWrite must accept the result. (No font files are committed: their licences.)
#include <windows.h>
#include <dwrite_3.h>
#include <t2embapi.h>
#include <wrl/client.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "draw/Sfnt.h"

namespace {

std::vector<uint8_t> Embed(const wchar_t* face, ULONG flags, const std::wstring& chars) {
  HDC dc = CreateCompatibleDC(nullptr);
  HFONT font = CreateFontW(-2048, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, face);
  HGDIOBJ old = SelectObject(dc, font);
  std::vector<uint8_t> out;
  WRITEEMBEDPROC write = [](void* stream, const void* buffer, const unsigned long count) -> unsigned long {
    auto* o = static_cast<std::vector<uint8_t>*>(stream);
    auto* b = static_cast<const uint8_t*>(buffer);
    o->insert(o->end(), b, b + count);
    return count;
  };
  ULONG priv = 0, status = 0;
  std::vector<USHORT> set(chars.begin(), chars.end());
  LONG r = TTEmbedFont(dc, flags, CHARSET_UNICODE, &priv, &status, write, &out, set.empty() ? nullptr : set.data(),
                       static_cast<USHORT>(set.size()), 0, nullptr);
  SelectObject(dc, old);
  DeleteObject(font);
  DeleteDC(dc);
  if (r != E_NONE) out.clear();
  return out;
}

uint16_t Be16(const std::vector<uint8_t>& d, size_t at) { return static_cast<uint16_t>(d[at] << 8 | d[at + 1]); }
uint32_t Be32(const std::vector<uint8_t>& d, size_t at) { return uint32_t{Be16(d, at)} << 16 | Be16(d, at + 2); }

std::map<uint32_t, std::vector<uint8_t>> TablesOf(const std::vector<uint8_t>& font) {
  std::map<uint32_t, std::vector<uint8_t>> out;
  if (font.size() < 12) return out;
  for (uint16_t i = 0; i < Be16(font, 4); ++i) {
    size_t rec = 12 + 16 * size_t{i};
    uint32_t off = Be32(font, rec + 8), len = Be32(font, rec + 12);
    if (off + size_t{len} <= font.size()) out[Be32(font, rec)].assign(font.begin() + off, font.begin() + off + len);
  }
  return out;
}

// A glyph as the rasterizer sees it: outline points, components, and the values its leading
// pushes push followed by the rest of its instructions.
struct GlyphData {
  int contours = 0;
  std::vector<int> numbers;  // bbox, end points, then (on, x, y) per point
  std::vector<uint8_t> components;
  std::vector<int> pushed;
  std::vector<uint8_t> code;
  bool operator==(const GlyphData&) const = default;
};

void SplitInstructions(const uint8_t* p, size_t n, GlyphData& g) {
  size_t i = 0;
  while (i < n) {
    uint8_t op = p[i];
    size_t count = 0;
    bool words = false;
    size_t data = 0;
    if (op == 0x40 || op == 0x41) {
      if (i + 1 >= n) break;
      count = p[i + 1];
      words = op == 0x41;
      data = i + 2;
    } else if (op >= 0xB0 && op <= 0xBF) {
      count = (op & 7) + 1;
      words = op >= 0xB8;
      data = i + 1;
    } else {
      break;
    }
    size_t end = data + count * (words ? 2 : 1);
    if (end > n) break;
    for (size_t k = 0; k < count; ++k) {
      g.pushed.push_back(words ? static_cast<int16_t>(p[data + 2 * k] << 8 | p[data + 2 * k + 1]) : p[data + k]);
    }
    i = end;
  }
  g.code.assign(p + i, p + n);
}

std::vector<GlyphData> Glyphs(const std::vector<uint8_t>& font) {
  auto t = TablesOf(font);
  const auto& head = t[draw::sfnt::Tag("head")];
  const auto& maxp = t[draw::sfnt::Tag("maxp")];
  const auto& loca = t[draw::sfnt::Tag("loca")];
  const auto& glyf = t[draw::sfnt::Tag("glyf")];
  std::vector<GlyphData> out;
  if (head.size() < 54 || maxp.size() < 6) return out;
  const bool longLoca = Be16(head, 50) == 1;
  const uint16_t n = Be16(maxp, 4);
  for (uint32_t gid = 0; gid < n; ++gid) {
    size_t a = longLoca ? Be32(loca, 4 * gid) : size_t{Be16(loca, 2 * gid)} * 2;
    size_t b = longLoca ? Be32(loca, 4 * gid + 4) : size_t{Be16(loca, 2 * gid + 2)} * 2;
    GlyphData g;
    if (b > a && b <= glyf.size()) {
      std::vector<uint8_t> d(glyf.begin() + a, glyf.begin() + b);
      g.contours = static_cast<int16_t>(Be16(d, 0));
      for (int k = 0; k < 4; ++k) g.numbers.push_back(static_cast<int16_t>(Be16(d, 2 + 2 * k)));
      if (g.contours < 0) {
        size_t p = 10;
        uint16_t flags = 0;
        do {
          flags = Be16(d, p);
          size_t len = 4 + (flags & 1 ? 4 : 2) + (flags & 8 ? 2 : flags & 0x40 ? 4 : flags & 0x80 ? 8 : 0);
          g.components.insert(g.components.end(), d.begin() + p, d.begin() + p + len);
          p += len;
        } while (flags & 0x20);
        if (flags & 0x100) SplitInstructions(d.data() + p + 2, Be16(d, p), g);
      } else {
        size_t p = 10;
        int points = 0;
        for (int c = 0; c < g.contours; ++c, p += 2) {
          g.numbers.push_back(Be16(d, p));
          points = Be16(d, p) + 1;
        }
        uint16_t ilen = Be16(d, p);
        SplitInstructions(d.data() + p + 2, ilen, g);
        p += 2 + ilen;
        std::vector<uint8_t> flags;
        while (static_cast<int>(flags.size()) < points) {
          uint8_t f = d[p++];
          flags.push_back(f);
          if (f & 8)
            for (int r = d[p++]; r > 0; --r) flags.push_back(f);
        }
        std::vector<int> xs, ys;
        int x = 0, y = 0;
        for (uint8_t f : flags) {
          if (f & 2) {
            x += f & 16 ? d[p] : -d[p];
            ++p;
          } else if (!(f & 16)) {
            x += static_cast<int16_t>(Be16(d, p));
            p += 2;
          }
          xs.push_back(x);
        }
        for (uint8_t f : flags) {
          if (f & 4) {
            y += f & 32 ? d[p] : -d[p];
            ++p;
          } else if (!(f & 32)) {
            y += static_cast<int16_t>(Be16(d, p));
            p += 2;
          }
          ys.push_back(y);
        }
        for (size_t k = 0; k < flags.size(); ++k) {
          g.numbers.push_back(flags[k] & 1);
          g.numbers.push_back(xs[k]);
          g.numbers.push_back(ys[k]);
        }
      }
    }
    out.push_back(std::move(g));
  }
  return out;
}

uint32_t DirectWriteGlyphCount(const std::vector<uint8_t>& font) {
  Microsoft::WRL::ComPtr<IDWriteFactory5> factory;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory5),
                                 reinterpret_cast<IUnknown**>(factory.GetAddressOf())))) {
    return 0;
  }
  Microsoft::WRL::ComPtr<IDWriteInMemoryFontFileLoader> loader;
  if (FAILED(factory->CreateInMemoryFontFileLoader(&loader)) || FAILED(factory->RegisterFontFileLoader(loader.Get()))) {
    return 0;
  }
  uint32_t glyphs = 0;
  Microsoft::WRL::ComPtr<IDWriteFontFile> file;
  if (SUCCEEDED(loader->CreateInMemoryFontFileReference(factory.Get(), font.data(), static_cast<UINT32>(font.size()),
                                                        nullptr, &file))) {
    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE fileType;
    DWRITE_FONT_FACE_TYPE faceType;
    UINT32 faces = 0;
    Microsoft::WRL::ComPtr<IDWriteFontFace> face;
    if (SUCCEEDED(file->Analyze(&supported, &fileType, &faceType, &faces)) && supported &&
        SUCCEEDED(factory->CreateFontFace(faceType, 1, file.GetAddressOf(), 0, DWRITE_FONT_SIMULATIONS_NONE, &face))) {
      glyphs = face->GetGlyphCount();
    }
  }
  factory->UnregisterFontFileLoader(loader.Get());
  return glyphs;
}

void CheckFace(const wchar_t* face, bool subset) {
  const std::wstring chars = subset ? L"Hello, Wörld 123 éè" : L"";
  const ULONG base = subset ? TTEMBED_SUBSET : TTEMBED_RAW;
  std::vector<uint8_t> packed = Embed(face, base | TTEMBED_TTCOMPRESSED, chars);
  std::vector<uint8_t> plain = Embed(face, base, chars);
  if (packed.empty() || plain.empty()) {
    std::printf("  (t2embed couldn't embed %ls; skipped)\n", face);
    return;
  }
  draw::sfnt::EotInfo info;
  std::vector<uint8_t> a = draw::sfnt::DecodeEot(packed, &info);
  std::vector<uint8_t> b = draw::sfnt::DecodeEot(plain);
  CHECK(!a.empty() && !b.empty());
  CHECK(info.flags & 0x4);
  CHECK(info.family == face);
  std::vector<GlyphData> ga = Glyphs(a), gb = Glyphs(b);
  CHECK(!ga.empty() && ga.size() == gb.size());
  size_t differ = 0;
  for (size_t i = 0; i < ga.size() && i < gb.size(); ++i) differ += !(ga[i] == gb[i]);
  CHECK_EQ(differ, size_t{0});
  // Every other table as it was (hdmx, VDMX and DSIG are left out; head differs in two fields).
  auto ta = TablesOf(a), tb = TablesOf(b);
  for (auto& [tag, bytes] : tb) {
    if (tag == draw::sfnt::Tag("glyf") || tag == draw::sfnt::Tag("loca") || tag == draw::sfnt::Tag("hdmx") ||
        tag == draw::sfnt::Tag("VDMX") || tag == draw::sfnt::Tag("DSIG") || tag == draw::sfnt::Tag("head")) {
      continue;
    }
    CHECK(ta.count(tag) && ta[tag] == bytes);
  }
  CHECK_EQ(DirectWriteGlyphCount(a), static_cast<uint32_t>(ga.size()));
}

}  // namespace

TEST(EotMicroTypeExpressMatchesUncompressed) {
  CheckFace(L"Georgia", true);
  CheckFace(L"Georgia", false);
  CheckFace(L"Arial", true);
  CheckFace(L"Segoe UI", true);
}

TEST(EotDamagedDataGivesNothing) {
  std::vector<uint8_t> packed = Embed(L"Georgia", TTEMBED_SUBSET | TTEMBED_TTCOMPRESSED, L"Abc");
  if (packed.empty()) return;
  CHECK(!draw::sfnt::DecodeEot(packed).empty());
  // Cut short anywhere, or with bytes flipped: nothing, or a font, never a crash.
  for (size_t cut : {size_t{0}, size_t{10}, size_t{100}, packed.size() / 2, packed.size() - 1}) {
    std::vector<uint8_t> d(packed.begin(), packed.begin() + cut);
    CHECK(draw::sfnt::DecodeEot(d).empty());
  }
  for (size_t at = 200; at < packed.size(); at += packed.size() / 37 + 1) {
    std::vector<uint8_t> d = packed;
    d[at] ^= 0x5A;
    draw::sfnt::DecodeEot(d);
  }
  // Not Embedded OpenType at all.
  std::vector<uint8_t> junk(300, 0x41);
  CHECK(draw::sfnt::DecodeEot(junk).empty());
  // A plain font comes back as it is.
  std::vector<uint8_t> ttf = {0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK(draw::sfnt::DecodeEot(ttf) == ttf);
}
