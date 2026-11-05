// Fuzz target: the font subsetter Export PDF embeds fonts with (src/draw/Sfnt.cpp, milestone D4).
// The input is a font file: TrueType, OpenType with CFF, or a collection (the first font). It is
// read as Export PDF reads it, subset to glyphs a hash of the input picks (with glyph 0, the last
// glyph and ids past the end), and embedded whole as a CFF font would be.
// Seeds: tests/fuzz/seeds/font (make_seeds.py), and locally small fonts from C:\Windows\Fonts
// (run-fuzzers.ps1 copies them; they can't be committed).
//
// It aborts when:
// - a subset isn't a font: it doesn't read back, or has another number of glyphs or units per em;
// - a subset's checksums are wrong: the file's doesn't sum to 0xB1B0AFBA;
// - subsetting a subset with the same glyphs doesn't give the same bytes;
// - a whole CFF font's tables don't read back as they went in.
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "draw/Sfnt.h"

using namespace draw::sfnt;

namespace {

void Check(bool ok) {
  if (!ok) std::abort();
}

uint32_t Sum(const std::vector<uint8_t>& b) {
  uint32_t sum = 0;
  for (size_t i = 0; i < b.size(); i += 4) {
    uint32_t word = 0;
    for (size_t k = 0; k < 4; ++k) word = word << 8 | (i + k < b.size() ? b[i + k] : 0);
    sum += word;
  }
  return sum;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::span<const uint8_t> file(data, size);
  const Tables tables = FileTables(file);
  const std::optional<Info> info = ReadInfo(tables);
  if (!info) return 0;

  // Glyphs to keep: 0, a few near the end and past it, and some the input's last bytes name.
  std::vector<uint16_t> glyphs{0, 1, static_cast<uint16_t>(info->numGlyphs - 1), info->numGlyphs, 0xFFFF};
  for (size_t i = 0; i + 1 < size && i < 32; i += 2)
    glyphs.push_back(static_cast<uint16_t>(data[size - 1 - i] << 8 | data[size - 2 - i]));

  if (!info->cff) {
    const std::vector<uint8_t> subset = Subset(tables, glyphs);
    if (!subset.empty()) {
      const Tables again = FileTables(subset);
      const std::optional<Info> read = ReadInfo(again);
      Check(read && read->numGlyphs == info->numGlyphs && read->unitsPerEm == info->unitsPerEm && !read->cff);
      Check(Sum(subset) == 0xB1B0AFBAu);
      Check(Subset(again, glyphs) == subset);
    }
  }

  const std::vector<uint8_t> whole = Whole(tables);
  if (!whole.empty()) {
    const Tables again = FileTables(whole);
    for (const char* tag : {"CFF ", "hhea", "hmtx", "maxp", "cmap", "OS/2", "name", "post"}) {
      const uint32_t t = static_cast<uint32_t>(tag[0]) << 24 | static_cast<uint32_t>(tag[1]) << 16 |
                         static_cast<uint32_t>(tag[2]) << 8 | static_cast<uint8_t>(tag[3]);
      std::vector<uint8_t> in, out;
      Check(tables(t, in) && again(t, out) && in == out);
    }
    Check(Sum(whole) == 0xB1B0AFBAu);
  }
  return 0;
}
