#pragma once
// Equations (OMML, m:oMath and m:oMathPara) laid out for reading (plan milestone D5): the boxes of
// TeX's algorithm, with the constants and glyph variants of Cambria Math's MATH table. The XML
// stays in the model untouched, so saving keeps it as it was.
#include <dwrite.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace draw {
class Painter;
}
namespace fw::text {
class Fonts;
}

namespace fw::doc::layout::math {

// An equation laid out: glyphs and rules placed from its baseline at its left edge (y down).
struct Equation {
  struct Glyphs {
    IDWriteFontFace* face = nullptr;  // owned by text::Fonts
    float size = 0, x = 0, y = 0;
    uint32_t color = 0;               // 0xRRGGBB
    std::vector<uint16_t> ids;
    std::vector<float> advances;
    std::u16string text;              // what the glyphs show, for PDF export
    std::vector<DWRITE_GLYPH_OFFSET> offsets;  // empty, or one per glyph (shapes built from parts)
  };
  struct Rule {
    float left = 0, top = 0, right = 0, bottom = 0;
    uint32_t color = 0;
  };
  enum class Justify : uint8_t { Center, Left, Right };
  float width = 0, ascent = 0, descent = 0;
  Justify justify = Justify::Center;  // a display equation's m:jc
  std::vector<Glyphs> glyphs;
  std::vector<Rule> rules;
};

// `xml`: the content of an m:oMath (display false) or an m:oMathPara (display true), as the
// document keeps it. `size`: the font size in points. Never null.
std::shared_ptr<const Equation> LayOut(text::Fonts& fonts, std::string_view xml, float size, bool display);
void Paint(draw::Painter& painter, const Equation& eq, float x, float baseline);

}  // namespace fw::doc::layout::math
