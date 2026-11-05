#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

// sfnt fonts (TrueType and OpenType) for PDF export (plan §4.6): what a PDF font needs to know
// about a font, and the font program to embed. A TrueType font is cut down to the glyphs used, and
// a CFF font is embedded whole. Fonts are untrusted input, since documents can embed their own, so
// every read is checked: a font that doesn't hold together gives nothing, never a broken file.
namespace draw::sfnt {

constexpr uint32_t Tag(const char (&s)[5]) {
  return uint32_t{static_cast<uint8_t>(s[0])} << 24 | uint32_t{static_cast<uint8_t>(s[1])} << 16 |
         uint32_t{static_cast<uint8_t>(s[2])} << 8 | uint32_t{static_cast<uint8_t>(s[3])};
}

// A font's tables by tag: false when it has no such table.
using Tables = std::function<bool(uint32_t tag, std::vector<uint8_t>& data)>;

// The tables of an sfnt file in memory (of a TrueType collection, its first font). The bytes must
// outlive the result.
Tables FileTables(std::span<const uint8_t> file);

struct Info {
  bool cff = false;          // CFF outlines, else TrueType ones (glyf)
  bool cidKeyed = false;     // a CID-keyed CFF font: its glyphs are found by CID, not glyph id
  bool variable = false;     // has variations (fvar): the tables hold only the default instance
  bool embeddable = true;    // OS/2 fsType allows embedding outlines
  bool subsettable = true;   // ...and cutting them down
  uint16_t unitsPerEm = 1000;
  uint16_t numGlyphs = 0;
  int16_t xMin = 0, yMin = 0, xMax = 0, yMax = 0;  // head
  int16_t ascent = 0, descent = 0, capHeight = 0;  // hhea, OS/2
  float italicAngle = 0;                           // post
  uint16_t weight = 400;                           // OS/2
  std::string postScriptName;                      // name 6, printable ASCII; may be empty
};
std::optional<Info> ReadInfo(const Tables& tables);

// A TrueType font program holding only `glyphs`, the glyphs their composites are made of, and
// glyph 0. Glyph ids stay as they are, so a PDF can use them as CIDs. Empty for a font without
// TrueType outlines or one that doesn't hold together.
std::vector<uint8_t> Subset(const Tables& tables, const std::vector<uint16_t>& glyphs);

// An OpenType font program with the tables a PDF reader needs, for CFF fonts, which are embedded
// whole. Empty when the font lacks one of them.
std::vector<uint8_t> Whole(const Tables& tables);

}  // namespace draw::sfnt
