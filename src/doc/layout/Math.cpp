#include "doc/layout/Math.h"

#include <wrl/client.h>

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>

#include "draw/Painter.h"
#include "text/Fonts.h"

namespace fw::doc::layout::math {

namespace {

using Microsoft::WRL::ComPtr;

// ---- The MATH table (OpenType): constants, and glyph variants for stretching.

// MathConstants' MathValueRecords from MathLeading on, in table order.
enum Const {
  MathLeading, AxisHeight, AccentBaseHeight, FlattenedAccentBaseHeight, SubscriptShiftDown, SubscriptTopMax,
  SubscriptBaselineDropMin, SuperscriptShiftUp, SuperscriptShiftUpCramped, SuperscriptBottomMin,
  SuperscriptBaselineDropMax, SubSuperscriptGapMin, SuperscriptBottomMaxWithSubscript, SpaceAfterScript,
  UpperLimitGapMin, UpperLimitBaselineRiseMin, LowerLimitGapMin, LowerLimitBaselineDropMin, StackTopShiftUp,
  StackTopDisplayStyleShiftUp, StackBottomShiftDown, StackBottomDisplayStyleShiftDown, StackGapMin,
  StackDisplayStyleGapMin, StretchStackTopShiftUp, StretchStackBottomShiftDown, StretchStackGapAboveMin,
  StretchStackGapBelowMin, FractionNumeratorShiftUp, FractionNumeratorDisplayStyleShiftUp,
  FractionDenominatorShiftDown, FractionDenominatorDisplayStyleShiftDown, FractionNumeratorGapMin,
  FractionNumDisplayStyleGapMin, FractionRuleThickness, FractionDenominatorGapMin, FractionDenomDisplayStyleGapMin,
  SkewedFractionHorizontalGap, SkewedFractionVerticalGap, OverbarVerticalGap, OverbarRuleThickness,
  OverbarExtraAscender, UnderbarVerticalGap, UnderbarRuleThickness, UnderbarExtraDescender, RadicalVerticalGap,
  RadicalDisplayStyleVerticalGap, RadicalRuleThickness, RadicalExtraAscender, RadicalKernBeforeDegree,
  RadicalKernAfterDegree, kConstCount
};

// Without a MATH table: Cambria Math's values, in 1/1000 em.
constexpr std::array<int16_t, kConstCount> kDefaults = {
    150, 250, 450, 650, 150, 400, 160, 360, 290, 110, 250, 200, 400, 40,  100, 110, 170, 600,
    440, 690, 350, 690, 300, 640, 0,   0,   0,   0,   390, 690, 340, 690, 30,  90,  65,  30,
    90,  350, 100, 150, 65,  65,  150, 65,  65,  60,  150, 65,  65,  280, -550};

struct MathFont {
  IDWriteFontFace* face = nullptr;
  float upem = 1000;
  int scriptPercent = 73, scriptScriptPercent = 60, degreeRaise = 60;
  float displayOperatorMin = 0;
  std::array<float, kConstCount> c{};
  std::vector<uint8_t> table;
  size_t variants = 0;  // MathVariants' offset, 0 if none
  float minOverlap = 0;
};

uint16_t U16(const std::vector<uint8_t>& t, size_t at) {
  return at + 2 <= t.size() ? static_cast<uint16_t>(t[at] << 8 | t[at + 1]) : 0;
}

MathFont Load(IDWriteFontFace* face) {
  MathFont f;
  f.face = face;
  DWRITE_FONT_METRICS fm{};
  if (face) face->GetMetrics(&fm);
  if (fm.designUnitsPerEm) f.upem = fm.designUnitsPerEm;
  for (int i = 0; i < kConstCount; ++i) f.c[i] = kDefaults[i] * f.upem / 1000;
  f.displayOperatorMin = 1.3f * f.upem;
  const void* data = nullptr;
  UINT32 size = 0;
  void* context = nullptr;
  BOOL exists = FALSE;
  if (!face || FAILED(face->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('M', 'A', 'T', 'H'), &data, &size, &context, &exists)) || !exists)
    return f;
  f.table.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
  face->ReleaseFontTable(context);
  const auto& t = f.table;
  if (const size_t c = U16(t, 4); c && c + 214 <= t.size()) {
    f.scriptPercent = static_cast<int16_t>(U16(t, c));
    f.scriptScriptPercent = static_cast<int16_t>(U16(t, c + 2));
    f.displayOperatorMin = U16(t, c + 6);
    for (int i = 0; i < kConstCount; ++i) f.c[i] = static_cast<int16_t>(U16(t, c + 8 + 4 * i));
    f.degreeRaise = static_cast<int16_t>(U16(t, c + 212));
  }
  if (f.scriptPercent <= 0 || f.scriptPercent > 100) f.scriptPercent = 73;
  if (f.scriptScriptPercent <= 0 || f.scriptScriptPercent > 100) f.scriptScriptPercent = 60;
  f.variants = U16(t, 8);
  if (f.variants) f.minOverlap = U16(t, f.variants);
  return f;
}

int CoverageIndex(const std::vector<uint8_t>& t, size_t at, uint16_t glyph) {
  const uint16_t format = U16(t, at), n = U16(t, at + 2);
  for (uint16_t i = 0; i < n; ++i) {
    if (format == 1 && U16(t, at + 4 + 2 * i) == glyph) return i;
    if (format == 2) {
      const size_t r = at + 4 + 6 * i;
      if (glyph >= U16(t, r) && glyph <= U16(t, r + 2)) return U16(t, r + 4) + glyph - U16(t, r);
    }
  }
  return -1;
}

struct Variant {
  uint16_t glyph;
  float size;
};
struct Part {
  uint16_t glyph;
  float start, end, full;
  bool extender;
};

// The variants of `glyph` that are bigger along one direction, and the parts to build it from.
void Construction(const MathFont& f, uint16_t glyph, bool vertical, std::vector<Variant>& variants, std::vector<Part>& parts) {
  const auto& t = f.table;
  const size_t v = f.variants;
  if (!v) return;
  const uint16_t vertCount = U16(t, v + 6), horizCount = U16(t, v + 8);
  const uint16_t coverage = U16(t, v + (vertical ? 2 : 4));
  const int index = coverage ? CoverageIndex(t, v + coverage, glyph) : -1;
  if (index < 0 || index >= (vertical ? vertCount : horizCount)) return;
  const uint16_t offset = U16(t, v + 10 + 2 * (vertical ? index : vertCount + index));
  if (!offset) return;
  const size_t c = v + offset;
  const uint16_t assembly = U16(t, c), count = U16(t, c + 2);
  for (uint16_t i = 0; i < count; ++i) variants.push_back({U16(t, c + 4 + 4 * i), static_cast<float>(U16(t, c + 6 + 4 * i))});
  if (!assembly) return;
  const size_t a = c + assembly;
  const uint16_t n = std::min<uint16_t>(U16(t, a + 4), 16);
  for (uint16_t i = 0; i < n; ++i) {
    const size_t p = a + 6 + 10 * i;
    parts.push_back({U16(t, p), static_cast<float>(U16(t, p + 2)), static_cast<float>(U16(t, p + 4)),
                     static_cast<float>(U16(t, p + 6)), (U16(t, p + 8) & 1) != 0});
  }
}

// ---- Boxes

// TeX's atom classes, which decide the space between neighbors.
enum Cls : uint8_t { Ord, Op, Bin, Rel, Open, Close, Punct, Inner, None };

// TeX's spaces between classes, in 1/18 em; negative: only at text size, not in scripts.
constexpr int8_t kSpace[8][8] = {
    {0, 3, -4, -5, 0, 0, 0, -3},       {3, 3, 0, -5, 0, 0, 0, -3},   {-4, -4, 0, 0, -4, 0, 0, -4},
    {-5, -5, 0, 0, -5, 0, 0, -5},      {0, 0, 0, 0, 0, 0, 0, 0},     {0, 3, -4, -5, 0, 0, 0, -3},
    {-3, -3, 0, -3, -3, -3, -3, -3},   {-3, 3, -4, -5, -3, 0, -3, -3},
};

struct Box {
  float w = 0, a = 0, d = 0;
  float italic = 0;          // italic correction: how far the last glyph leans past its advance
  float inkL = 0, inkR = 0;  // a single glyph's ink, across
  bool glyph = false;        // one glyph (scripts sit closer to a box than to a glyph)
  Cls first = None, last = None;
  float mark = -1;           // eqArr: where the first '&' is
  std::vector<Equation::Glyphs> g;
  std::vector<Equation::Rule> r;

  void Put(Box&& b, float x, float y) {
    for (auto& gl : b.g) {
      gl.x += x;
      gl.y += y;
      g.push_back(std::move(gl));
    }
    for (auto ru : b.r) {
      ru.left += x, ru.right += x, ru.top += y, ru.bottom += y;
      r.push_back(ru);
    }
    if (b.mark >= 0 && mark < 0) mark = b.mark + x;
    a = std::max(a, b.a - y);
    d = std::max(d, b.d + y);
    glyph = false;
  }
  void Rule(float left, float top, float right, float bottom, uint32_t color) {
    r.push_back({left, top, right, bottom, color});
    a = std::max(a, -top);
    d = std::max(d, bottom);
  }
};

struct St {
  int level = 0;  // 0 text, 1 script, 2 script of script
  bool display = false, cramped = false;
};
St Script(St s, bool cramped) { return {std::min(s.level + 1, 2), false, cramped || s.cramped}; }

std::string_view Local(pugi::xml_node n, char prefix = 'm') {
  const char* name = n.name();
  return name[0] == prefix && name[1] == ':' ? std::string_view(name + 2) : std::string_view();
}
pugi::xml_node Child(pugi::xml_node n, std::string_view local) {
  for (pugi::xml_node c : n.children())
    if (Local(c) == local) return c;
  return {};
}
// The m:val of a property: <m:{pr}><m:{name} m:val="..."/></m:{pr}>. Null when it's absent.
const char* Prop(pugi::xml_node n, std::string_view pr, std::string_view name) {
  pugi::xml_node p = Child(Child(n, pr), name);
  if (!p) return nullptr;
  for (pugi::xml_attribute a : p.attributes())
    if (std::string_view(a.name()).ends_with("val")) return a.value();
  return "";
}
bool On(pugi::xml_node n, std::string_view pr, std::string_view name, bool absent = false) {
  const char* v = Prop(n, pr, name);
  if (!v) return absent;
  const std::string_view s = v;
  return !(s == "0" || s == "off" || s == "false");
}

char32_t Next(std::string_view s, size_t& i) {
  const auto b = [&](size_t k) { return k < s.size() ? static_cast<unsigned char>(s[k]) : 0u; };
  const unsigned c = b(i);
  if (c < 0x80) return ++i, c;
  const int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
  char32_t cp = c & (0x3F >> n);
  for (int k = 1; k <= n; ++k) cp = cp << 6 | (b(i + k) & 0x3F);
  i += n + 1;
  return cp;
}
char32_t Chr(pugi::xml_node n, std::string_view pr, char32_t absent, std::string_view name = "chr") {
  const char* v = Prop(n, pr, name);
  if (!v) return absent;
  size_t i = 0;
  return *v ? Next(v, i) : 0;
}
void Append(std::u16string& s, char32_t c) {
  if (c >= 0x10000) {
    s += static_cast<char16_t>(0xD800 + ((c - 0x10000) >> 10));
    s += static_cast<char16_t>(0xDC00 + ((c - 0x10000) & 0x3FF));
  } else {
    s += static_cast<char16_t>(c);
  }
}

Cls ClassOf(char32_t c) {
  static constexpr std::u32string_view rel = U"=<>:≤≥≠≈≡≢∼≃≅∝≪≫≺≻⊂⊃⊆⊇∈∉∋⊥∥→←↔⇒⇐⇔↦⟶⟹≔≜≐∣";
  static constexpr std::u32string_view bin = U"+-*−±∓×÷·⋅∗∘∙∩∪⊕⊖⊗⊙∧∨∖⊎⋆";
  if (rel.find(c) != rel.npos) return Rel;
  if (bin.find(c) != bin.npos) return Bin;
  if (std::u32string_view(U"([{⟨⌊⌈⟦").find(c) != std::u32string_view::npos) return Open;
  if (std::u32string_view(U")]}⟩⌋⌉⟧!?").find(c) != std::u32string_view::npos) return Close;
  if (c == ',' || c == ';') return Punct;
  return Ord;
}

// The letter shown for `c` in a math run: italic by default, or bold, or a math alphabet (m:scr).
char32_t Styled(char32_t c, std::string_view sty, std::string_view scr) {
  const bool upper = c >= 'A' && c <= 'Z', lower = c >= 'a' && c <= 'z';
  const int i = upper ? c - 'A' : c - 'a';
  if (!scr.empty() && scr != "roman") {
    if (!upper && !lower) return c;
    struct Alphabet {
      std::string_view name;
      char32_t upper, lower;
      std::string_view letters;  // letters encoded before the block: holes in it
      std::u32string_view holes;
    };
    static constexpr Alphabet kAlphabets[] = {
        {"double-struck", 0x1D538, 0x1D552, "CHNPQRZ", U"ℂℍℕℙℚℝℤ"},
        {"script", 0x1D49C, 0x1D4B6, "BEFHILMRego", U"ℬℰℱℋℐℒℳℛℯℊℴ"},
        {"fraktur", 0x1D504, 0x1D51E, "CHIRZ", U"ℭℌℑℜℨ"},
        {"sans-serif", 0x1D5A0, 0x1D5BA, "", U""},
        {"monospace", 0x1D670, 0x1D68A, "", U""},
    };
    for (const Alphabet& a : kAlphabets) {
      if (a.name != scr) continue;
      if (const size_t k = a.letters.find(static_cast<char>(c)); k != std::string_view::npos) return a.holes[k];
      return (upper ? a.upper : a.lower) + i;
    }
    return c;
  }
  if (upper || lower) {
    if (sty == "p") return c;
    if (sty == "b") return (upper ? 0x1D400 : 0x1D41A) + i;
    if (sty == "bi") return (upper ? 0x1D468 : 0x1D482) + i;
    return c == 'h' ? 0x210E : (upper ? 0x1D434 : 0x1D44E) + i;
  }
  if (c >= 0x3B1 && c <= 0x3C9 && sty != "p" && sty != "b") return (sty == "bi" ? 0x1D736 : 0x1D6FC) + (c - 0x3B1);
  if (c >= '0' && c <= '9' && (sty == "b" || sty == "bi")) return 0x1D7CE + (c - '0');
  if (c == '-') return 0x2212;
  if (c == '\'') return 0x2032;
  return c;
}

class Typesetter {
 public:
  Typesetter(text::Fonts& fonts, const MathFont& mf, float size) : fonts_(fonts), mf_(mf), size_(size) {
    for (const wchar_t* family : {L"Cambria", L"Segoe UI Symbol", L"Segoe UI"})
      if (fonts.Installed(family))
        if (const text::Face* face = fonts.Get(fonts.Resolve(family), DWRITE_FONT_WEIGHT_NORMAL, false)) fallbacks_.push_back(face->Get());
  }

  Box Row(pugi::xml_node parent, St st, bool eqArr = false) {
    Box row;
    if (!parent || ++depth_ > 48) {
      --depth_;
      return row;
    }
    std::vector<Box> atoms;
    for (pugi::xml_node c : parent.children()) Atoms(c, st, eqArr, atoms);
    Cls prev = None;
    float x = 0;
    for (size_t i = 0; i < atoms.size(); ++i) {
      Box& b = atoms[i];
      if (b.first == Bin && (prev == None || prev == Op || prev == Bin || prev == Rel || prev == Open || prev == Punct))
        b.first = b.last = Ord;
      if (b.first == Bin && i + 1 < atoms.size() &&
          (atoms[i + 1].first == Rel || atoms[i + 1].first == Close || atoms[i + 1].first == Punct))
        b.first = b.last = Ord;
      x += Space(prev, b.first, st);
      if (row.first == None) row.first = b.first;
      if (b.last != None) prev = b.last;
      const float w = b.w, italic = b.italic;
      const bool single = atoms.size() == 1 && b.glyph;
      const float inkL = b.inkL, inkR = b.inkR;
      row.Put(std::move(b), x, 0);
      x += w;
      row.italic = italic;
      if (single) row.glyph = true, row.inkL = inkL, row.inkR = inkR;
    }
    row.w = x;
    row.last = prev;
    --depth_;
    return row;
  }

  Box Lines(pugi::xml_node para, St st) {
    std::vector<Box> lines;
    for (pugi::xml_node c : para.children())
      if (Local(c) == "oMath") lines.push_back(Row(c, st));
    if (lines.size() == 1) return std::move(lines[0]);
    return Stack(lines, st, 0.5f, false);
  }

 private:
  float Size(St st) const {
    return st.level == 0 ? size_ : size_ * (st.level == 1 ? mf_.scriptPercent : mf_.scriptScriptPercent) / 100;
  }
  float C(Const k, St st) const { return mf_.c[k] * Size(st) / mf_.upem; }
  float Space(Cls l, Cls r, St st) const {
    if (l >= None || r >= None) return 0;
    int v = kSpace[l][r];
    if (v < 0) {
      if (st.level > 0) return 0;
      v = -v;
    }
    return v * Size(st) / 18;
  }

  uint16_t GlyphOf(IDWriteFontFace* face, char32_t c) const {
    UINT16 g = 0;
    const UINT32 cp = c;
    if (face) face->GetGlyphIndices(&cp, 1, &g);
    return g;
  }

  // One glyph of the math font, as a box.
  Box GlyphBox(uint16_t glyph, St st, std::u16string text, IDWriteFontFace* face = nullptr, float upem = 0) {
    if (!face) face = mf_.face, upem = mf_.upem;
    const float s = Size(st), k = s / upem;
    DWRITE_GLYPH_METRICS m{};
    face->GetDesignGlyphMetrics(&glyph, 1, &m, FALSE);
    Box b;
    b.w = m.advanceWidth * k;
    b.a = (m.verticalOriginY - m.topSideBearing) * k;
    b.d = (static_cast<float>(m.advanceHeight) - m.verticalOriginY - m.bottomSideBearing) * k;
    b.inkL = m.leftSideBearing * k;
    b.inkR = (static_cast<float>(m.advanceWidth) - m.rightSideBearing) * k;
    b.italic = std::max(0.f, -m.rightSideBearing * k);
    b.glyph = true;
    b.first = b.last = Ord;
    b.g.push_back({face, s, 0, 0, color_, {glyph}, {b.w}, std::move(text), {}});
    return b;
  }

  // Text in one font: the glyphs of `shown`, which shows `source`.
  Box TextBox(std::u32string_view shown, std::u32string_view source, St st) {
    Box b;
    for (size_t i = 0; i < shown.size(); ++i) {
      IDWriteFontFace* face = mf_.face;
      char32_t c = shown[i];
      uint16_t g = GlyphOf(face, c);
      if (!g && c != source[i]) c = source[i], g = GlyphOf(face, c);
      for (size_t f = 0; !g && f < fallbacks_.size(); ++f) face = fallbacks_[f], g = GlyphOf(face, c);
      if (!g) face = mf_.face;
      DWRITE_FONT_METRICS fm{};
      face->GetMetrics(&fm);
      std::u16string text;
      Append(text, source[i]);
      Box one = GlyphBox(g, st, std::move(text), face, fm.designUnitsPerEm ? fm.designUnitsPerEm : 1000.f);
      // Joins the run before it when the font is the same.
      if (!b.g.empty() && b.g.back().face == face && one.g[0].y == 0) {
        auto& run = b.g.back();
        run.ids.push_back(one.g[0].ids[0]);
        run.advances.push_back(one.w);
        run.text += one.g[0].text;
        b.a = std::max(b.a, one.a);
        b.d = std::max(b.d, one.d);
      } else {
        const bool first = b.g.empty();
        const float inkL = one.inkL;
        b.Put(std::move(one), b.w, 0);
        if (first) b.inkL = inkL;
      }
      b.w += b.g.back().advances.back();
      b.italic = one.italic;
      b.inkR = b.w - b.g.back().advances.back() + one.inkR;
    }
    b.glyph = shown.size() == 1;
    return b;
  }

  // A glyph made at least `target` tall (or wide): a bigger variant, or built from parts.
  Box Stretch(char32_t c, float target, St st, bool vertical) {
    std::u16string text;
    Append(text, c);
    const uint16_t g = GlyphOf(mf_.face, c);
    if (!g) return TextBox(std::u32string(1, c), std::u32string(1, c), st);
    std::vector<Variant> variants;
    std::vector<Part> parts;
    Construction(mf_, g, vertical, variants, parts);
    const float k = Size(st) / mf_.upem;
    for (const Variant& v : variants)
      if (v.size * k >= target) return GlyphBox(v.glyph, st, text);
    if (!parts.empty()) return Assembly(parts, target / k, st, vertical, std::move(text));
    return GlyphBox(variants.empty() ? g : variants.back().glyph, st, text);
  }

  Box Assembly(const std::vector<Part>& parts, float target, St st, bool vertical, std::u16string text) {
    const auto length = [&](int reps, int& count) {
      float sum = 0;
      count = 0;
      for (const Part& p : parts) {
        const int n = p.extender ? reps : 1;
        sum += n * p.full;
        count += n;
      }
      return sum - mf_.minOverlap * std::max(0, count - 1);
    };
    int reps = 0, count = 0;
    float len = length(0, count);
    while (len < target && reps < 64) {
      int c2 = 0;
      const float more = length(reps + 1, c2);
      if (more <= len) break;
      ++reps, len = more, count = c2;
    }
    std::vector<const Part*> seq;
    for (const Part& p : parts)
      for (int n = p.extender ? reps : 1; n > 0; --n) seq.push_back(&p);
    const float spare = std::max(0.f, len - target) / std::max<size_t>(1, seq.size() - 1);
    const float k = Size(st) / mf_.upem;
    Box b;
    b.a = b.d = -1e9f;
    // One run with offsets, so the shape is one character's: PDF export gives it as that text.
    Equation::Glyphs run{mf_.face, Size(st), 0, 0, color_, {}, {}, std::move(text), {}};
    float pos = 0;
    for (size_t i = 0; i < seq.size(); ++i) {
      const Part& p = *seq[i];
      DWRITE_GLYPH_METRICS m{};
      mf_.face->GetDesignGlyphMetrics(&p.glyph, 1, &m, FALSE);
      const float top = static_cast<float>(m.verticalOriginY) - m.topSideBearing;
      const float bottom = static_cast<float>(m.verticalOriginY) - m.advanceHeight + m.bottomSideBearing;
      const float x = vertical ? 0 : pos, y = vertical ? pos - bottom : 0;  // design units, y up
      run.ids.push_back(p.glyph);
      run.advances.push_back(0);
      run.offsets.push_back({x * k, y * k});
      b.a = std::max(b.a, (y + top) * k);
      b.d = std::max(b.d, -(y + bottom) * k);
      b.w = std::max(b.w, (x + m.advanceWidth) * k);
      if (i + 1 < seq.size()) {
        const float most = std::max(mf_.minOverlap, std::min(p.end, seq[i + 1]->start));
        pos += p.full - std::min(most, mf_.minOverlap + spare);
      } else {
        pos += p.full;
      }
    }
    if (!vertical) b.w = pos * k, b.inkL = 0, b.inkR = b.w;
    if (seq.empty()) b.a = b.d = 0;
    b.g.push_back(std::move(run));
    b.first = b.last = Ord;
    return b;
  }

  // Centers a box on the math axis.
  Box OnAxis(Box b, St st) {
    Box out;
    const float dy = (b.a - b.d) / 2 - C(AxisHeight, st);
    out.w = b.w;
    out.first = b.first, out.last = b.last, out.italic = b.italic;
    out.Put(std::move(b), 0, dy);
    return out;
  }

  // Lines stacked and centered on the axis (matrix rows, eqArr, an m:oMathPara's equations).
  Box Stack(std::vector<Box>& lines, St st, float gapEm, bool alignMarks) {
    Box b;
    if (lines.empty()) return b;
    float width = 0, mark = 0;
    bool marks = false;
    for (const Box& l : lines) {
      width = std::max(width, l.w);
      if (alignMarks && l.mark >= 0) marks = true, mark = std::max(mark, l.mark);
    }
    if (marks) {
      width = 0;
      for (const Box& l : lines) width = std::max(width, mark - std::max(0.f, l.mark) + l.w);
    }
    const float gap = gapEm * Size(st) * 0.5f;
    float y = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
      Box& l = lines[i];
      if (i) y += lines[i - 1].d + gap + l.a;
      const float x = marks ? mark - std::max(0.f, l.mark) : (width - l.w) / 2;
      b.Put(std::move(l), x, y);
    }
    b.w = width;
    b.first = b.last = Ord;
    b.mark = -1;
    return OnAxis(std::move(b), st);
  }

  void Atoms(pugi::xml_node n, St st, bool eqArr, std::vector<Box>& out) {
    const std::string_view name = Local(n);
    if (name.empty()) {
      // w:r (a run of document text inside the equation) and wrappers such as w:ins.
      const std::string_view w = Local(n, 'w');
      if (w == "r") Run(n, st, eqArr, out, true);
      else if (w == "ins" || w == "moveTo" || w == "smartTag" || w == "customXml" || w == "sdt" || w == "sdtContent" || w == "hyperlink")
        for (pugi::xml_node c : n.children()) Atoms(c, st, eqArr, out);
      return;
    }
    if (name.ends_with("Pr")) return;
    if (name == "r") return Run(n, st, eqArr, out, false);
    if (name == "nary") {
      out.push_back(Nary(n, st));
      Box e = Row(Child(n, "e"), st);
      if (e.w > 0) out.push_back(std::move(e));
      return;
    }
    if (name == "func") {
      Box f = Row(Child(n, "fName"), st);
      f.first = f.last = Op;
      out.push_back(std::move(f));
      Box e = Row(Child(n, "e"), st);
      if (e.first == Open) e.first = Ord;  // sin(x): Word keeps the thin space
      out.push_back(std::move(e));
      return;
    }
    Box b = Element(n, name, st);
    if (b.first == None) b.first = b.last = Ord;
    out.push_back(std::move(b));
  }

  void Run(pugi::xml_node r, St st, bool eqArr, std::vector<Box>& out, bool wordRun) {
    std::string text;
    for (pugi::xml_node c : r.children())
      if (Local(c) == "t" || Local(c, 'w') == "t") text += c.text().get();
    const char* sty = wordRun ? "p" : Prop(r, "rPr", "sty");
    const char* scr = wordRun ? nullptr : Prop(r, "rPr", "scr");
    const bool normal = wordRun || On(r, "rPr", "nor");
    color_ = 0;
    for (pugi::xml_node rpr : r.children())
      if (Local(rpr, 'w') == "rPr")
        for (pugi::xml_node c : rpr.children())
          if (Local(c, 'w') == "color")
            if (const char* v = c.attribute("w:val").value(); std::strlen(v) == 6) color_ = std::strtoul(v, nullptr, 16);
    std::u32string shown, source;
    Cls cls = None;
    const auto flush = [&] {
      if (source.empty()) return;
      Box b = TextBox(shown, source, st);
      b.first = b.last = normal ? Ord : cls;
      out.push_back(std::move(b));
      shown.clear(), source.clear();
    };
    for (size_t i = 0; i < text.size();) {
      const char32_t c = Next(text, i);
      if (eqArr && c == '&') {
        flush();
        Box mark;
        mark.mark = 0;
        out.push_back(std::move(mark));
        continue;
      }
      const Cls k = normal ? Ord : ClassOf(c);
      if (k != Ord || cls != Ord) flush();
      cls = k;
      source += c;
      shown += normal ? c : Styled(c, sty ? sty : "", scr ? scr : "");
    }
    flush();
  }

  Box Element(pugi::xml_node n, std::string_view name, St st) {
    if (name == "f") return Fraction(n, st);
    if (name == "sSup" || name == "sSub" || name == "sSubSup" || name == "sPre") return Scripts(n, name, st);
    if (name == "rad") return Radical(n, st);
    if (name == "d") return Delimiters(n, st);
    if (name == "m") return Matrix(n, st);
    if (name == "eqArr") {
      std::vector<Box> lines;
      for (pugi::xml_node c : n.children())
        if (Local(c) == "e") lines.push_back(Row(c, st, true));
      return Stack(lines, st, 0.5f, true);
    }
    if (name == "acc") return Accent(n, st);
    if (name == "bar") return Bar(n, st);
    if (name == "limLow" || name == "limUpp") return Limit(n, name == "limUpp", st);
    if (name == "groupChr") return GroupChar(n, st);
    if (name == "borderBox") return BorderBox(n, st);
    if (name == "phant") return Phantom(n, st);
    return Row(name == "box" ? Child(n, "e") : n, st);  // m:box, containers, and what isn't known: its content
  }

  Box Fraction(pugi::xml_node n, St st) {
    const char* type = Prop(n, "fPr", "type");
    const std::string_view kind = type ? type : "bar";
    if (kind == "lin" || kind == "skw") {
      const bool skw = kind == "skw";
      Box num = Row(Child(n, "num"), skw ? Script(st, false) : st);
      Box den = Row(Child(n, "den"), skw ? Script(st, true) : st);
      Box slash = TextBox(skw ? U"⁄" : U"/", U"/", st);
      Box b;
      const float raise = skw ? C(AxisHeight, st) : 0;
      float x = num.w;
      b.Put(std::move(num), 0, -raise);
      const float sw = slash.w;
      b.Put(std::move(slash), x, 0);
      x += sw;
      x += den.w;
      b.Put(std::move(den), x - den.w, skw ? raise * 0.5f : 0);
      b.w = x;
      b.first = b.last = Ord;
      return b;
    }
    const St ns = st.display ? St{st.level, false, st.cramped} : Script(st, false);
    const St ds = {ns.level, false, true};
    Box num = Row(Child(n, "num"), ns), den = Row(Child(n, "den"), ds);
    const float axis = C(AxisHeight, st);
    float u, v;
    const bool bar = kind != "noBar";
    float t = 0;
    if (bar) {
      t = C(FractionRuleThickness, st);
      u = std::max(C(st.display ? FractionNumeratorDisplayStyleShiftUp : FractionNumeratorShiftUp, st),
                   axis + t / 2 + C(st.display ? FractionNumDisplayStyleGapMin : FractionNumeratorGapMin, st) + num.d);
      v = std::max(C(st.display ? FractionDenominatorDisplayStyleShiftDown : FractionDenominatorShiftDown, st),
                   den.a + C(st.display ? FractionDenomDisplayStyleGapMin : FractionDenominatorGapMin, st) - axis + t / 2);
    } else {
      u = C(st.display ? StackTopDisplayStyleShiftUp : StackTopShiftUp, st);
      v = C(st.display ? StackBottomDisplayStyleShiftDown : StackBottomShiftDown, st);
      const float gap = (u - num.d) - (den.a - v), need = C(st.display ? StackDisplayStyleGapMin : StackGapMin, st);
      if (gap < need) u += (need - gap) / 2, v += (need - gap) / 2;
    }
    const float pad = Size(st) * 0.08f, inner = std::max(num.w, den.w);
    Box b;
    const float nw = num.w, dw = den.w;
    b.Put(std::move(num), pad + (inner - nw) / 2, -u);
    b.Put(std::move(den), pad + (inner - dw) / 2, v);
    if (bar) b.Rule(pad * 0.5f, -(axis + t / 2), pad * 1.5f + inner, -(axis - t / 2), color_);
    b.w = inner + 2 * pad;
    b.first = b.last = Inner;
    return b;
  }

  // Sub- and superscripts on `base`: placed by the MATH constants, as TeX places them.
  Box Attach(Box base, Box* sub, Box* sup, St st, bool before) {
    float u = 0, v = 0;
    if (sup) u = std::max({C(st.cramped ? SuperscriptShiftUpCramped : SuperscriptShiftUp, st),
                           base.glyph ? 0.f : base.a - C(SuperscriptBaselineDropMax, st), sup->d + C(SuperscriptBottomMin, st)});
    if (sub) v = std::max({C(SubscriptShiftDown, st), base.glyph ? 0.f : base.d + C(SubscriptBaselineDropMin, st),
                           sub->a - C(SubscriptTopMax, st)});
    if (sub && sup) {
      float gap = (u - sup->d) - (sub->a - v);
      const float need = C(SubSuperscriptGapMin, st);
      if (gap < need) {
        const float up = std::max(0.f, std::min(need - gap, C(SuperscriptBottomMaxWithSubscript, st) - (u - sup->d)));
        u += up, gap += up;
        if (gap < need) v += need - gap;
      }
    }
    const float italic = before ? 0 : base.italic;
    const float sw = std::max(sup ? sup->w + italic : 0.f, sub ? sub->w : 0.f);
    const float space = C(SpaceAfterScript, st);
    Box b;
    b.first = base.first, b.last = base.last;
    const float bw = base.w;
    const float x = before ? sw + space : 0;
    b.Put(std::move(base), x, 0);
    const float sx = before ? 0 : bw;
    if (sup) b.Put(std::move(*sup), before ? sw - sup->w : sx + italic, -u);
    if (sub) b.Put(std::move(*sub), before ? sw - sub->w : sx, v);
    b.w = before ? x + bw : bw + sw + space;
    return b;
  }

  Box Scripts(pugi::xml_node n, std::string_view name, St st) {
    Box base = Row(Child(n, "e"), st);
    std::optional<Box> sub, sup;
    if (name != "sSup") sub = Row(Child(n, "sub"), Script(st, true));
    if (name != "sSub") sup = Row(Child(n, "sup"), Script(st, false));
    return Attach(std::move(base), sub ? &*sub : nullptr, sup ? &*sup : nullptr, st, name == "sPre");
  }

  Box Radical(pugi::xml_node n, St st) {
    Box e = Row(Child(n, "e"), {st.level, st.display, true});
    const float t = C(RadicalRuleThickness, st);
    float gap = C(st.display ? RadicalDisplayStyleVerticalGap : RadicalVerticalGap, st);
    const float need = e.a + e.d + gap + t;
    Box sign = Stretch(U'√', need, st, true);
    const float h = sign.a + sign.d;
    gap += std::max(0.f, (h - need) / 2);
    const float top = e.a + gap + t;
    Box b;
    float x = 0;
    pugi::xml_node degree = Child(n, "deg");
    if (!On(n, "radPr", "degHide") && degree && degree.first_child()) {
      Box deg = Row(degree, {2, false, false});
      const float before = C(RadicalKernBeforeDegree, st), after = C(RadicalKernAfterDegree, st);
      const float dw = deg.w;
      b.Put(std::move(deg), before, -(top - h + h * mf_.degreeRaise / 100.f));
      x = std::max(0.f, before + dw + after);
    }
    const float sw = sign.w, dy = sign.a - top;
    b.Put(std::move(sign), x, dy);
    const float xr = x + sw, ew = e.w;
    b.Rule(xr, -top, xr + ew, -(top - t), color_);
    b.Put(std::move(e), xr, 0);
    b.a = std::max(b.a, top + C(RadicalExtraAscender, st));
    b.w = xr + ew;
    b.first = b.last = Ord;
    return b;
  }

  Box Nary(pugi::xml_node n, St st) {
    const char32_t chr = Chr(n, "naryPr", U'∫');
    const bool integral = chr >= 0x222B && chr <= 0x2233;
    const char* loc = Prop(n, "naryPr", "limLoc");
    // Limits over and under in display equations; inline, at the side, as Word and TeX place them.
    const bool limits = st.display && (loc ? std::string_view(loc) == "undOvr" : !integral);
    Box op;
    std::u16string text;
    Append(text, chr);
    if (const uint16_t g = chr ? GlyphOf(mf_.face, chr) : 0) {
      uint16_t shown = g;
      if (st.display) {
        std::vector<Variant> variants;
        std::vector<Part> parts;
        Construction(mf_, g, true, variants, parts);
        for (const Variant& v : variants) {
          shown = v.glyph;
          if (v.size >= mf_.displayOperatorMin) break;
        }
      }
      op = OnAxis(GlyphBox(shown, st, text), st);  // a box now: scripts go by its height
    }
    std::optional<Box> sub, sup;
    if (!On(n, "naryPr", "subHide")) sub = Row(Child(n, "sub"), Script(st, true));
    if (!On(n, "naryPr", "supHide")) sup = Row(Child(n, "sup"), Script(st, false));
    if (sub && sub->w == 0 && sub->g.empty()) sub.reset();
    if (sup && sup->w == 0 && sup->g.empty()) sup.reset();
    Box b = limits ? Limits(std::move(op), sub ? &*sub : nullptr, sup ? &*sup : nullptr, st)
                   : Attach(std::move(op), sub ? &*sub : nullptr, sup ? &*sup : nullptr, st, false);
    b.first = b.last = Op;
    return b;
  }

  // Limits centered under and over `base` (an n-ary operator, lim).
  Box Limits(Box base, Box* lower, Box* upper, St st) {
    const float w = std::max({base.w, lower ? lower->w : 0.f, upper ? upper->w : 0.f});
    Box b;
    b.first = base.first, b.last = base.last;
    const float a = base.a, d = base.d, bw = base.w;
    b.Put(std::move(base), (w - bw) / 2, 0);
    if (upper) {
      const float rise = std::max(C(UpperLimitBaselineRiseMin, st), C(UpperLimitGapMin, st) + upper->d);
      b.Put(std::move(*upper), (w - upper->w) / 2, -(a + rise));
    }
    if (lower) {
      const float drop = std::max(C(LowerLimitBaselineDropMin, st), C(LowerLimitGapMin, st) + lower->a);
      b.Put(std::move(*lower), (w - lower->w) / 2, d + drop);
    }
    b.w = w;
    return b;
  }

  Box Limit(pugi::xml_node n, bool upper, St st) {
    Box base = Row(Child(n, "e"), st);
    Box lim = Row(Child(n, "lim"), Script(st, !upper));
    return Limits(std::move(base), upper ? nullptr : &lim, upper ? &lim : nullptr, st);
  }

  Box Delimiters(pugi::xml_node n, St st) {
    const char32_t begin = Chr(n, "dPr", '(', "begChr"), end = Chr(n, "dPr", ')', "endChr"), sep = Chr(n, "dPr", '|', "sepChr");
    const bool grow = On(n, "dPr", "grow", true);
    std::vector<Box> args;
    float a = 0, d = 0;
    for (pugi::xml_node c : n.children())
      if (Local(c) == "e") {
        args.push_back(Row(c, st));
        a = std::max(a, args.back().a), d = std::max(d, args.back().d);
      }
    const float axis = C(AxisHeight, st), target = 2 * std::max(a - axis, d + axis);
    Box b;
    float x = 0;
    const auto put = [&](Box&& part) {
      const float w = part.w;
      b.Put(std::move(part), x, 0);
      x += w;
    };
    const auto delimiter = [&](char32_t c) {
      put(OnAxis(grow ? Stretch(c, target, st, true) : TextBox(std::u32string(1, c), std::u32string(1, c), st), st));
    };
    if (begin) delimiter(begin);
    for (size_t i = 0; i < args.size(); ++i) {
      if (i && sep) delimiter(sep);
      put(std::move(args[i]));
    }
    if (end) delimiter(end);
    b.w = x;
    b.first = Open, b.last = Close;
    return b;
  }

  Box Matrix(pugi::xml_node n, St st) {
    std::vector<std::vector<Box>> rows;
    size_t cols = 0;
    for (pugi::xml_node r : n.children()) {
      if (Local(r) != "mr") continue;
      rows.emplace_back();
      for (pugi::xml_node c : r.children())
        if (Local(c) == "e") rows.back().push_back(Row(c, st));
      cols = std::max(cols, rows.back().size());
    }
    std::vector<float> widths(cols, 0);
    for (const auto& r : rows)
      for (size_t j = 0; j < r.size(); ++j) widths[j] = std::max(widths[j], r[j].w);
    const float gap = Size(st);
    std::vector<Box> lines;
    for (auto& r : rows) {
      Box line;
      float x = 0;
      for (size_t j = 0; j < cols; ++j) {
        if (j < r.size()) {
          const float w = r[j].w;
          line.Put(std::move(r[j]), x + (widths[j] - w) / 2, 0);
        }
        x += widths[j] + (j + 1 < cols ? gap : 0);
      }
      line.w = x;
      lines.push_back(std::move(line));
    }
    return Stack(lines, st, 0.6f, false);
  }

  Box Accent(pugi::xml_node n, St st) {
    Box e = Row(Child(n, "e"), {st.level, st.display, true});
    const char32_t chr = Chr(n, "accPr", U'̂');
    if (!chr) return e;
    Box acc = Stretch(chr, e.w, st, false);
    const float center = e.glyph ? (e.inkL + e.inkR) / 2 + e.italic / 2 : e.w / 2;
    const float dx = center - (acc.inkL + acc.inkR) / 2;
    const float dy = -std::max(0.f, e.a - C(AccentBaseHeight, st));
    Box b;
    b.first = e.first, b.last = e.last;
    const float w = e.w;
    b.Put(std::move(e), 0, 0);
    b.Put(std::move(acc), dx, dy);
    b.w = w;
    return b;
  }

  Box Bar(pugi::xml_node n, St st) {
    Box e = Row(Child(n, "e"), st);
    const char* pos = Prop(n, "barPr", "pos");
    const bool top = pos && std::string_view(pos) == "top";
    Box b;
    b.first = e.first, b.last = e.last;
    const float a = e.a, d = e.d, w = e.w;
    b.Put(std::move(e), 0, 0);
    if (top) {
      const float gap = C(OverbarVerticalGap, st), t = C(OverbarRuleThickness, st);
      b.Rule(0, -(a + gap + t), w, -(a + gap), color_);
      b.a = std::max(b.a, a + gap + t + C(OverbarExtraAscender, st));
    } else {
      const float gap = C(UnderbarVerticalGap, st), t = C(UnderbarRuleThickness, st);
      b.Rule(0, d + gap, w, d + gap + t, color_);
      b.d = std::max(b.d, d + gap + t + C(UnderbarExtraDescender, st));
    }
    b.w = w;
    return b;
  }

  Box GroupChar(pugi::xml_node n, St st) {
    Box e = Row(Child(n, "e"), st);
    const char32_t chr = Chr(n, "groupChrPr", U'⏟');
    const char* pos = Prop(n, "groupChrPr", "pos");
    const bool top = pos ? std::string_view(pos) == "top" : false;
    if (!chr) return e;
    Box c = Stretch(chr, e.w, st, false);
    const float gap = Size(st) * 0.1f, w = std::max(e.w, c.w), a = e.a, d = e.d, ew = e.w, cw = c.w;
    Box b;
    b.first = b.last = Ord;
    b.Put(std::move(e), (w - ew) / 2, 0);
    if (top) b.Put(std::move(c), (w - cw) / 2, -(a + gap + c.d));
    else b.Put(std::move(c), (w - cw) / 2, d + gap + c.a);
    b.w = w;
    return b;
  }

  Box BorderBox(pugi::xml_node n, St st) {
    Box e = Row(Child(n, "e"), st);
    const float pad = Size(st) * 0.12f, t = C(FractionRuleThickness, st);
    const float w = e.w + 2 * pad, top = -(e.a + pad), bottom = e.d + pad;
    Box b;
    b.first = e.first, b.last = e.last;
    b.Put(std::move(e), pad, 0);
    if (!On(n, "borderBoxPr", "hideTop")) b.Rule(0, top - t, w, top, color_);
    if (!On(n, "borderBoxPr", "hideBot")) b.Rule(0, bottom, w, bottom + t, color_);
    if (!On(n, "borderBoxPr", "hideLeft")) b.Rule(0, top - t, t, bottom + t, color_);
    if (!On(n, "borderBoxPr", "hideRight")) b.Rule(w - t, top - t, w, bottom + t, color_);
    b.w = w;
    return b;
  }

  Box Phantom(pugi::xml_node n, St st) {
    Box e = Row(Child(n, "e"), st);
    if (!On(n, "phantPr", "show", true)) e.g.clear(), e.r.clear();
    if (On(n, "phantPr", "zeroWid")) e.w = 0;
    if (On(n, "phantPr", "zeroAsc")) e.a = 0;
    if (On(n, "phantPr", "zeroDesc")) e.d = 0;
    return e;
  }

  text::Fonts& fonts_;
  const MathFont& mf_;
  float size_;
  int depth_ = 0;
  uint32_t color_ = 0;
  std::vector<IDWriteFontFace*> fallbacks_;
};

const MathFont& MathFontOf(text::Fonts& fonts) {
  // One per font collection: the MATH table read once.
  static std::map<IDWriteFontFace*, std::pair<ComPtr<IDWriteFontFace>, MathFont>> cache;
  IDWriteFontFace* face = nullptr;
  for (const wchar_t* family : {L"Cambria Math", L"Cambria", L"Segoe UI"}) {
    if (!fonts.Installed(family)) continue;
    if (const text::Face* f = fonts.Get(fonts.Resolve(family), DWRITE_FONT_WEIGHT_NORMAL, false)) {
      face = f->Get();
      break;
    }
  }
  auto it = cache.find(face);
  if (it == cache.end()) it = cache.emplace(face, std::make_pair(ComPtr<IDWriteFontFace>(face), Load(face))).first;
  return it->second.second;
}

}  // namespace

std::shared_ptr<const Equation> LayOut(text::Fonts& fonts, std::string_view xml, float size, bool display) {
  auto eq = std::make_shared<Equation>();
  const MathFont& mf = MathFontOf(fonts);
  if (!mf.face || size <= 0) return eq;
  std::string wrapped =
      "<m:oMath xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\" "
      "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">";
  wrapped += xml;
  wrapped += "</m:oMath>";
  pugi::xml_document doc;
  if (!doc.load_buffer(wrapped.data(), wrapped.size(), pugi::parse_default | pugi::parse_ws_pcdata, pugi::encoding_utf8)) return eq;
  pugi::xml_node root = doc.first_child();
  Typesetter ts(fonts, mf, size);
  Box b = display ? ts.Lines(root, {0, true, false}) : ts.Row(root, {0, false, false});
  if (display) {
    const char* jc = Prop(root, "oMathParaPr", "jc");
    const std::string_view j = jc ? jc : "";
    eq->justify = j == "left" ? Equation::Justify::Left : j == "right" ? Equation::Justify::Right : Equation::Justify::Center;
  }
  eq->width = b.w;
  eq->ascent = std::max(b.a, 0.f);
  eq->descent = std::max(b.d, 0.f);
  eq->glyphs = std::move(b.g);
  eq->rules = std::move(b.r);
  return eq;
}

void Paint(draw::Painter& painter, const Equation& eq, float x, float baseline) {
  const auto color = [](uint32_t rgb) {
    return draw::Color{((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, 1};
  };
  for (const Equation::Glyphs& g : eq.glyphs) {
    if (g.ids.empty()) continue;
    draw::GlyphRun run;
    run.face = g.face;
    run.emSize = g.size;
    run.count = static_cast<uint32_t>(g.ids.size());
    run.indices = g.ids.data();
    run.advances = g.advances.data();
    run.text = g.text.data();
    run.textLength = static_cast<uint32_t>(g.text.size());
    if (!g.offsets.empty()) run.offsets = g.offsets.data();
    painter.DrawGlyphs({x + g.x, baseline + g.y}, run, draw::Brush::Solid(color(g.color)));
  }
  for (const Equation::Rule& r : eq.rules)
    painter.FillRect({x + r.left, baseline + r.top, x + r.right, baseline + r.bottom}, draw::Brush::Solid(color(r.color)));
}

}  // namespace fw::doc::layout::math
