#include "text/LineLayout.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include "text/Analysis.h"
#include "text/Fonts.h"
#include "text/Hyphenation.h"

using Microsoft::WRL::ComPtr;

namespace fw::text {
namespace {

constexpr float kEpsilon = 0.001f;       // points: rounding noise when comparing widths
constexpr float kSmallCapsScale = 0.8f;  // small capitals are capitals at 80 % of the size

enum Kind : uint8_t {
  kText,   // shaped
  kSpace,  // U+0020: shaped; hangs at a line end; justification stretches it
  kHang,   // the other breaking spaces (en, em, thin...): shaped; hang; not stretched
  kTab,
  kLineBreak,
  kPageBreak,
  kColumnBreak,
  kSoftHyphen,  // zero width; a hyphen when the line breaks after it
  kZeroWidth,   // U+200B, the no-width optional break
  kObject,
};

bool Shaped(uint8_t k) { return k <= kHang; }
bool Hangs(uint8_t k) { return k == kSpace || k == kHang; }
bool Mandatory(uint8_t k) { return k == kLineBreak || k == kPageBreak || k == kColumnBreak; }

Kind KindOf(wchar_t c) {
  switch (c) {
    case 0x0009: return kTab;
    case 0x000A:
    case 0x000B:
    case 0x000D:
    case 0x2028: return kLineBreak;
    case 0x000C: return kPageBreak;
    case 0x000E: return kColumnBreak;
    case 0x0020: return kSpace;
    case 0x00AD: return kSoftHyphen;
    case 0x200B: return kZeroWidth;
    case 0xFFFC: return kObject;
    default: break;
  }
  // Word breaks after en and em spaces but not after thin and hair spaces.
  if ((c >= 0x2000 && c <= 0x2006) || c == 0x2008) return kHang;
  return kText;
}

LineEnd EndingOf(uint8_t k) {
  switch (k) {
    case kPageBreak: return LineEnd::PageBreak;
    case kColumnBreak: return LineEnd::ColumnBreak;
    default: return LineEnd::LineBreak;
  }
}

// Which of a run's fonts a character uses (w:rFonts): see TextStyle.
enum Slot : uint8_t { kLatin, kEastAsia, kComplex };

bool IsComplexScript(uint16_t iso) {
  switch (iso) {
    case 125:  // Hebr
    case 135:  // Syrc
    case 160:  // Arab
    case 165:  // Nkoo
    case 170:  // Thaa
    case 310:  // Guru
    case 315:  // Deva
    case 320:  // Gujr
    case 325:  // Beng
    case 327:  // Orya
    case 330:  // Tibt
    case 340:  // Telu
    case 345:  // Knda
    case 346:  // Taml
    case 347:  // Mlym
    case 348:  // Sinh
    case 350:  // Mymr
    case 352:  // Thai
    case 355:  // Khmr
    case 356:  // Laoo
      return true;
    default:
      return false;
  }
}

bool IsEastAsianScript(uint16_t iso) {
  return iso == 285 || iso == 286 || iso == 287 || iso == 410 || iso == 411 || iso == 412 || iso == 413 ||
         iso == 460 || (iso >= 500 && iso <= 503);
}

bool IsEastAsianChar(wchar_t c) {
  return (c >= 0x1100 && c <= 0x11FF) || (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xA960 && c <= 0xA97F) ||
         (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
         (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0xD840 && c <= 0xD87F);  // the last: planes 2 and 3
}

// Punctuation and symbols both Latin and East Asian fonts have, which w:hint="eastAsia" sends to
// the East Asian font.
bool IsSharedPunctuation(wchar_t c) {
  return (c >= 0x00A1 && c <= 0x00BF) || c == 0x00D7 || c == 0x00F7 || (c >= 0x2010 && c <= 0x2BFF);
}

bool IsLowSurrogate(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

// Scripts whose break opportunities come from the text analyzer (UAX #14 and its dictionaries):
// East Asian text, which breaks between characters, the scripts without spaces between words
// (Thai, Lao, Khmer, Myanmar...), and inline objects. Other text follows Word's own rules.
bool UsesAnalyzerBreaks(wchar_t c) {
  return IsEastAsianChar(c) || (c >= 0x0E00 && c <= 0x0EFF) || (c >= 0x1000 && c <= 0x109F) ||
         (c >= 0x1780 && c <= 0x17FF) || (c >= 0x1950 && c <= 0x19DF) || (c >= 0x1A20 && c <= 0x1AAF) ||
         (c >= 0xAA60 && c <= 0xAADF) || c == 0xFFFC || (c >= 0x3000 && c <= 0x303F);
}

// A line may break after these: hyphen-minus, hyphen, figure dash, en dash, em dash.
bool IsBreakingDash(wchar_t c) { return c == L'-' || c == 0x2010 || c == 0x2012 || c == 0x2013 || c == 0x2014; }

WORD CharType(wchar_t c) {
  WORD type = 0;
  GetStringTypeW(CT_CTYPE1, &c, 1, &type);
  return type;
}

// A letter of a word for hyphenation; apostrophes count inside words.
bool IsLetter(wchar_t c) { return (CharType(c) & C1_ALPHA) != 0; }
bool IsWordChar(wchar_t c) { return IsLetter(c) || c == L'\'' || c == 0x2019; }

const TextStyle& DefaultStyle() {
  static const TextStyle style;
  return style;
}

struct SlotFont {
  const std::wstring* family;
  float size;
  bool bold, italic;
  const wchar_t* locale;
};

SlotFont ForSlot(const TextStyle& s, Slot slot) {
  const std::wstring& lang = s.language;
  switch (slot) {
    case kEastAsia:
      return {s.eastAsiaFamily.empty() ? &s.family : &s.eastAsiaFamily, s.size, s.bold, s.italic,
              (s.eastAsiaLanguage.empty() ? lang : s.eastAsiaLanguage).c_str()};
    case kComplex:
      return {s.complexFamily.empty() ? &s.family : &s.complexFamily, s.complexSize > 0 ? s.complexSize : s.size,
              s.complexBold, s.complexItalic, (s.complexLanguage.empty() ? lang : s.complexLanguage).c_str()};
    default:
      return {&s.family, s.size, s.bold, s.italic, lang.c_str()};
  }
}

// Shaped glyphs for a stretch of visible characters in one font, run, script and bidi level.
struct Segment {
  uint32_t start = 0, end = 0;
  uint32_t run = 0;
  const Face* face = nullptr;
  float emSize = 0, rise = 0, scale = 1;
  uint8_t level = 0;
  float ascent = 0, descent = 0, lineGap = 0;
  std::vector<uint16_t> clusters;  // per character: its cluster's first glyph
  std::vector<uint16_t> glyphs;
  std::vector<float> advances;
  std::vector<DWRITE_GLYPH_OFFSET> offsets;

  uint32_t GlyphAt(uint32_t i) const { return i < end ? clusters[i - start] : static_cast<uint32_t>(glyphs.size()); }
};

// A run's own font (its ascii family) for what isn't shaped: tabs, breaks, hyphens and leaders.
struct RunFont {
  const Face* face = nullptr;
  float size = 0, rise = 0;
  FontMetrics metrics;
  bool made = false;
};

struct TabInfo {
  float stop = 0;
  TabAlign align = TabAlign::Start;
  TabLeader leader = TabLeader::None;
  bool isDefault = true;
};

// The tab whose text segment is being laid out.
struct TabContext {
  bool active = false;
  float origin = 0;  // the line's start, before any tab
  float from = 0;    // where the tab started
  float stop = 0;
  TabAlign align = TabAlign::Start;
  TabLeader leader = TabLeader::None;
};

// Where a tab segment's content starts, given its width and the width before its decimal point.
float SegmentStart(const TabContext& t, float content, float beforeDecimal) {
  if (!t.active) return t.origin;
  switch (t.align) {
    case TabAlign::End: return std::max(t.from, t.stop - content);
    case TabAlign::Center: return std::max(t.from, t.stop - content / 2);
    case TabAlign::Decimal: return std::max(t.from, t.stop - (beforeDecimal >= 0 ? beforeDecimal : content));
    default: return t.stop;
  }
}

struct Fit {
  uint32_t end;
  LineEnd ending;
  bool hyphen;
};

// One thing on a line, in logical order.
struct Item {
  enum Type : uint8_t { Glyphs, Tab, Object, Gap, Hyphen };
  uint32_t from = 0, to = 0;  // visible characters
  int seg = -1;
  Type type = Gap;
  uint8_t level = 0;
  float width = 0;
  float x = 0;  // physical left edge, once placed
  TabLeader leader = TabLeader::None;
  bool trailing = false;  // hanging spaces or the break at the end of the line
};

// Rule L2 of the bidi algorithm: from the highest level down to the lowest odd one, reverse every
// run of items at that level or higher.
void Reorder(std::vector<size_t>& order, const std::vector<Item>& items) {
  uint8_t highest = 0, lowest = 255;
  for (size_t i : order) {
    highest = std::max(highest, items[i].level);
    lowest = std::min(lowest, items[i].level);
  }
  if (highest == 0) return;
  int lowestOdd = lowest | 1;
  for (int level = highest; level >= lowestOdd; --level) {
    for (size_t i = 0; i < order.size();) {
      if (items[order[i]].level < level) {
        ++i;
        continue;
      }
      size_t j = i;
      while (j < order.size() && items[order[j]].level >= level) ++j;
      std::reverse(order.begin() + static_cast<ptrdiff_t>(i), order.begin() + static_cast<ptrdiff_t>(j));
      i = j;
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------------------------

struct LineBreaker::Impl {
  Fonts& fonts;
  const Paragraph& p;
  float width;  // the column's
  bool rtl;

  // Per visible character (hidden text is left out).
  std::vector<uint32_t> orig;  // its offset in p.text
  std::wstring text;           // the characters, for analysis and hyphenation
  std::wstring shape;          // what is shaped: capitals for w:caps, "-" for U+2011, ...
  std::vector<uint8_t> kind, level, smallCaps, clusterStart, caretStop;
  std::vector<uint32_t> runOf;
  std::vector<int32_t> segOf;
  std::vector<float> adv;  // a cluster's advance on its first character, letter spacing included
  std::vector<DWRITE_LINE_BREAKPOINT> breaks;
  std::vector<Segment> segs;
  std::vector<RunFont> runFonts;
  std::vector<const TextStyle*> styles;  // per run
  State state;

  Impl(Fonts& f, const Paragraph& para, float columnWidth);

  uint32_t Size() const { return static_cast<uint32_t>(text.size()); }
  const TextStyle& StyleOf(uint32_t i) const { return *styles[runOf[i]]; }
  bool CanBreakBefore(uint32_t i) const;
  const RunFont& FontOfRun(uint32_t run);
  void Shape(uint32_t start, uint32_t end, uint32_t run, Slot slot, const DWRITE_SCRIPT_ANALYSIS& script,
             bool connected, bool substituted, IDWriteNumberSubstitution* numbers);
  void ShapePiece(Segment& seg, const DWRITE_SCRIPT_ANALYSIS& script, const wchar_t* locale, bool connected,
                  const TextStyle& style, IDWriteNumberSubstitution* numbers);
  // The hyphen drawn after visible character `at`: its face, size and advance.
  void HyphenFont(uint32_t at, const Face*& face, float& emSize, float& rise, uint32_t& run);
  float HyphenAdvance(uint32_t at);
  TabInfo NextTab(float x, bool first) const;
  Fit FindBreak(uint32_t a, float s0, float s1, bool first);
  bool Squeezes(uint32_t segFrom, uint32_t upTo, float over, float limit, float lastEnd) const;
  uint32_t Squeeze(uint32_t segFrom, uint32_t i, uint32_t we, float content, float limit, float lastEnd,
                   const TabContext& tab, float beforeDecimal);
  uint32_t Hyphenate(uint32_t segFrom, uint32_t ws, uint32_t we, bool afterBreak, float contentAtWs, float endAtWs,
                     float limit, const TabContext& tab, float beforeDecimal);
  Line Build(uint32_t a, const Fit& fit, float s0, float s1, bool first);
  void MarkMetrics(Line& line);
};

LineBreaker::Impl::Impl(Fonts& f, const Paragraph& para, float columnWidth)
    : fonts(f), p(para), width(columnWidth), rtl(para.format.rightToLeft) {
  // Runs, and the visible characters.
  uint32_t covered = 0;
  for (const auto& r : p.runs) {
    styles.push_back(r.style ? r.style : &DefaultStyle());
    uint32_t end = std::min<uint32_t>(covered + r.length, static_cast<uint32_t>(p.text.size()));
    for (uint32_t i = covered; i < end; ++i) {
      if (styles.back()->hidden) continue;
      orig.push_back(i);
      runOf.push_back(static_cast<uint32_t>(styles.size() - 1));
    }
    covered = end;
  }
  if (covered < p.text.size()) {  // text the runs don't cover: the default style
    styles.push_back(&DefaultStyle());
    for (uint32_t i = covered; i < p.text.size(); ++i) {
      orig.push_back(i);
      runOf.push_back(static_cast<uint32_t>(styles.size() - 1));
    }
  }
  runFonts.resize(styles.size());
  const uint32_t n = static_cast<uint32_t>(orig.size());
  text.resize(n);
  kind.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    text[i] = p.text[orig[i]];
    kind[i] = KindOf(text[i]);
  }

  // What gets shaped: capitals, and look-alikes the font is more likely to have.
  shape = text;
  smallCaps.assign(n, 0);
  for (uint32_t i = 0; i < n;) {
    uint32_t j = i;
    while (j < n && runOf[j] == runOf[i]) ++j;
    const TextStyle& s = StyleOf(i);
    if (s.caps != Caps::None) {
      std::wstring upper(shape.begin() + i, shape.begin() + j);
      int got = LCMapStringEx(s.language.empty() ? LOCALE_NAME_INVARIANT : s.language.c_str(),
                              LCMAP_UPPERCASE | LCMAP_LINGUISTIC_CASING, shape.data() + i, static_cast<int>(j - i),
                              upper.data(), static_cast<int>(upper.size()), nullptr, nullptr, 0);
      if (got != static_cast<int>(j - i)) {
        upper.assign(shape.begin() + i, shape.begin() + j);
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, shape.data() + i, static_cast<int>(j - i), upper.data(),
                      static_cast<int>(upper.size()), nullptr, nullptr, 0);
      }
      for (uint32_t k = i; k < j; ++k) {
        if (s.caps == Caps::Small && upper[k - i] != shape[k]) smallCaps[k] = 1;
        shape[k] = upper[k - i];
      }
    }
    i = j;
  }
  for (auto& c : shape) {
    if (c == 0x00A0) c = L' ';
    else if (c == 0x2011) c = L'-';
  }

  // Analysis: scripts, bidi levels, break opportunities, digits.
  std::vector<detail::LocaleRange> locales;
  for (uint32_t i = 0; i < n;) {
    uint32_t j = i;
    while (j < n && runOf[j] == runOf[i]) ++j;
    const TextStyle& s = StyleOf(i);
    bool complex = s.rightToLeft || s.complexScript;
    const std::wstring& lang = complex && !s.complexLanguage.empty() ? s.complexLanguage : s.language;
    locales.push_back({i, j, lang.c_str()});
    i = j;
  }
  ComPtr<IDWriteNumberSubstitution> numbers;
  if (p.format.digits != DWRITE_NUMBER_SUBSTITUTION_METHOD_NONE && fonts.Factory()) {
    const TextStyle& s = n ? StyleOf(0) : DefaultStyle();
    const std::wstring& lang = s.complexLanguage.empty() ? s.language : s.complexLanguage;
    fonts.Factory()->CreateNumberSubstitution(p.format.digits, lang.c_str(), TRUE, &numbers);
  }
  detail::Analysis analysis;
  if (fonts.Analyzer()) {
    analysis = detail::Analyze(fonts.Analyzer(), text, rtl, locales, numbers.Get());
  } else {
    analysis.levels.assign(n, rtl ? 1 : 0);
    analysis.breaks.assign(n, {});
    analysis.substituted.assign(n, 0);
  }
  level = std::move(analysis.levels);
  breaks = std::move(analysis.breaks);

  // Segments: shaped characters with the same run, script, level, font slot and digits; font
  // fallback splits them further.
  segOf.assign(n, -1);
  adv.assign(n, 0);
  clusterStart.assign(n, 1);
  if (fonts.Analyzer()) {
    for (const auto& sr : analysis.scripts) {
      DWRITE_SCRIPT_PROPERTIES props{};
      fonts.Analyzer()->GetScriptProperties(sr.script, &props);
      bool connected = props.isConnectedWriting || props.isCursiveWriting;
      auto iso = static_cast<uint16_t>(props.isoScriptNumber);
      auto slotOf = [&](uint32_t i) {
        const TextStyle& s = StyleOf(i);
        // A symbol font's characters (U+F000..U+F0FF, a bullet in Symbol or Wingdings) are in the
        // run's own font, also in right-to-left text, as Word draws them.
        if (text[i] >= 0xF000 && text[i] <= 0xF0FF) return kLatin;
        if (s.rightToLeft || s.complexScript || IsComplexScript(iso)) return kComplex;
        if (IsEastAsianScript(iso) || IsEastAsianChar(text[i])) return kEastAsia;
        if (s.eastAsiaHint && IsSharedPunctuation(text[i])) return kEastAsia;
        return kLatin;
      };
      for (uint32_t i = sr.start; i < sr.end && i < n;) {
        if (!Shaped(kind[i])) {
          ++i;
          continue;
        }
        Slot slot = slotOf(i);
        uint32_t j = i + 1;
        while (j < sr.end && j < n && Shaped(kind[j]) && runOf[j] == runOf[i] && level[j] == level[i] &&
               smallCaps[j] == smallCaps[i] && analysis.substituted[j] == analysis.substituted[i] && slotOf(j) == slot) {
          ++j;
        }
        Shape(i, j, runOf[i], slot, sr.script, connected, analysis.substituted[i] != 0, numbers.Get());
        i = j;
      }
    }
  }
  for (uint32_t i = 0; i < n; ++i) {
    if (kind[i] == kObject) adv[i] = StyleOf(i).objectWidth;
  }

  // Caret stops: grapheme starts (not a low surrogate, not a combining mark).
  caretStop.assign(n, 1);
  std::vector<WORD> types(n);
  if (n) GetStringTypeW(CT_CTYPE3, text.data(), static_cast<int>(n), types.data());
  for (uint32_t i = 0; i < n; ++i) {
    if (IsLowSurrogate(text[i]) || (i > 0 && (types[i] & (C3_NONSPACING | C3_VOWELMARK)) && Shaped(kind[i])))
      caretStop[i] = 0;
  }
  if (n == 0) state.position = 0;
}

void LineBreaker::Impl::Shape(uint32_t start, uint32_t end, uint32_t run, Slot slot,
                              const DWRITE_SCRIPT_ANALYSIS& script, bool connected, bool substituted,
                              IDWriteNumberSubstitution* numbers) {
  const TextStyle& style = *styles[run];
  SlotFont sf = ForSlot(style, slot);
  const std::wstring& family = fonts.Resolve(*sf.family);
  const Face* base = fonts.Get(family, sf.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, sf.italic);
  if (!base) base = fonts.Get(fonts.Resolve(L""), DWRITE_FONT_WEIGHT_NORMAL, false);
  if (!base) return;

  float size = sf.size * (smallCaps[start] ? kSmallCapsScale : 1.0f);
  float rise = style.baselineShift;
  FontMetrics bm = base->Metrics(size);
  if (style.verticalAlign == VerticalAlign::Superscript) {
    rise += bm.superscriptRaise;
    size = bm.superscriptSize;
  } else if (style.verticalAlign == VerticalAlign::Subscript) {
    rise -= bm.subscriptLower;
    size = bm.subscriptSize;
  }

  // Font fallback, for characters the font doesn't have.
  std::vector<detail::LocaleRange> locales{{start, end, sf.locale}};
  detail::TextSource source(shape, rtl, locales, nullptr);
  // The base face as DirectWrite names it: "Calibri" at weight 300 for Word's "Calibri Light".
  for (uint32_t pos = start; pos < end;) {
    UINT32 mapped = 0;
    ComPtr<IDWriteFont> font;
    FLOAT scale = 1;
    const Face* face = base;
    if (FAILED(fonts.Fallback()->MapCharacters(&source, pos, end - pos, fonts.Collection(), base->Family().c_str(),
                                               base->Weight(), base->Style(), DWRITE_FONT_STRETCH_NORMAL, &mapped,
                                               &font, &scale)) ||
        mapped == 0) {
      mapped = end - pos;
      scale = 1;
    } else if (font) {
      const Face* other = fonts.Get(font.Get());
      if (other && _wcsicmp(other->Family().c_str(), base->Family().c_str()) != 0) face = other;
      else scale = 1;
    }
    Segment seg;
    seg.start = pos;
    seg.end = pos + mapped;
    seg.run = run;
    seg.face = face;
    seg.emSize = size * (scale > 0 ? scale : 1);
    seg.rise = rise;
    seg.level = level[pos];
    FontMetrics m = face->Metrics(seg.emSize);
    seg.ascent = m.ascent + rise;
    seg.descent = m.descent - rise;
    seg.lineGap = m.lineGap;
    if (style.verticalAlign != VerticalAlign::Baseline) {
      // Superscript and subscript text sizes its line as the run's text would on the baseline, at
      // its own size: Word doesn't make a line taller for a footnote reference.
      seg.ascent = bm.ascent + style.baselineShift;
      seg.descent = bm.descent - style.baselineShift;
      seg.lineGap = bm.lineGap;
    }
    ShapePiece(seg, script, sf.locale, connected, style, substituted ? numbers : nullptr);
    auto index = static_cast<int32_t>(segs.size());
    for (uint32_t i = seg.start; i < seg.end; ++i) segOf[i] = index;
    segs.push_back(std::move(seg));
    pos += mapped;
  }
}

void LineBreaker::Impl::ShapePiece(Segment& seg, const DWRITE_SCRIPT_ANALYSIS& script, const wchar_t* locale,
                                   bool connected, const TextStyle& style, IDWriteNumberSubstitution* numbers) {
  const UINT32 len = seg.end - seg.start;
  const wchar_t* str = shape.data() + seg.start;
  const BOOL rtlRun = (seg.level & 1) ? TRUE : FALSE;
  IDWriteFontFace* face = seg.face->Get();

  // Word applies ligatures, contextual alternates and kerning only when the run asks for them.
  // Connected scripts (Arabic and others) keep the font's defaults, which they need.
  DWRITE_FONT_FEATURE list[4];
  UINT32 count = 0;
  if (!connected) {
    if (!style.ligatures) {
      list[count++] = {DWRITE_FONT_FEATURE_TAG_STANDARD_LIGATURES, 0};
      list[count++] = {DWRITE_FONT_FEATURE_TAG_CONTEXTUAL_LIGATURES, 0};
    }
    list[count++] = {DWRITE_FONT_FEATURE_TAG_CONTEXTUAL_ALTERNATES, 0};
    bool kern = style.kerningMinSize > 0 && seg.emSize >= style.kerningMinSize;
    list[count++] = {DWRITE_FONT_FEATURE_TAG_KERNING, kern ? 1u : 0u};
  }
  DWRITE_TYPOGRAPHIC_FEATURES features{list, count};
  const DWRITE_TYPOGRAPHIC_FEATURES* featureList[1] = {&features};
  const UINT32 featureLengths[1] = {len};
  const DWRITE_TYPOGRAPHIC_FEATURES** fl = count ? featureList : nullptr;
  const UINT32* flen = count ? featureLengths : nullptr;
  const UINT32 ranges = count ? 1 : 0;

  seg.clusters.resize(len);
  std::vector<DWRITE_SHAPING_TEXT_PROPERTIES> textProps(len);
  std::vector<DWRITE_SHAPING_GLYPH_PROPERTIES> glyphProps;
  UINT32 maxGlyphs = len * 3 / 2 + 16, actual = 0;
  HRESULT hr = E_FAIL;
  for (int attempt = 0; attempt < 4; ++attempt) {
    seg.glyphs.resize(maxGlyphs);
    glyphProps.resize(maxGlyphs);
    hr = fonts.Analyzer()->GetGlyphs(str, len, face, FALSE, rtlRun, &script, locale, numbers, fl, flen, ranges,
                                     maxGlyphs, seg.clusters.data(), textProps.data(), seg.glyphs.data(),
                                     glyphProps.data(), &actual);
    if (hr != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) break;
    maxGlyphs *= 2;
  }
  if (SUCCEEDED(hr)) {
    seg.glyphs.resize(actual);
    seg.advances.resize(actual);
    seg.offsets.resize(actual);
    hr = fonts.Analyzer()->GetGlyphPlacements(str, seg.clusters.data(), textProps.data(), len, seg.glyphs.data(),
                                              glyphProps.data(), actual, face, seg.emSize, FALSE, rtlRun, &script,
                                              locale, fl, flen, ranges, seg.advances.data(), seg.offsets.data());
  }
  if (FAILED(hr)) {
    // One glyph per character, unshaped: better than nothing.
    std::vector<UINT32> cps(str, str + len);
    seg.glyphs.assign(len, 0);
    face->GetGlyphIndices(cps.data(), len, seg.glyphs.data());
    std::vector<DWRITE_GLYPH_METRICS> gm(len);
    face->GetDesignGlyphMetrics(seg.glyphs.data(), len, gm.data(), FALSE);
    DWRITE_FONT_METRICS fm{};
    face->GetMetrics(&fm);
    seg.advances.resize(len);
    seg.offsets.assign(len, DWRITE_GLYPH_OFFSET{});
    for (UINT32 i = 0; i < len; ++i) {
      seg.clusters[i] = static_cast<uint16_t>(i);
      seg.advances[i] = gm[i].advanceWidth * seg.emSize / fm.designUnitsPerEm;
    }
  }

  // Character scaling (w:w) widens or narrows the glyphs and their placement.
  seg.scale = style.scale > 0 ? style.scale : 1;
  if (seg.scale != 1) {
    for (float& a : seg.advances) a *= seg.scale;
    for (DWRITE_GLYPH_OFFSET& o : seg.offsets) o.advanceOffset *= seg.scale;
  }
  // Per character: its cluster's advance, plus the letter spacing, which Word adds after each one.
  const float spacing = style.letterSpacing;
  const auto glyphCount = static_cast<uint32_t>(seg.glyphs.size());
  for (uint32_t k = 0; k < len; ++k) {
    uint32_t i = seg.start + k;
    if (k > 0 && seg.clusters[k] == seg.clusters[k - 1]) {
      clusterStart[i] = 0;
      continue;
    }
    uint32_t next = k + 1;
    while (next < len && seg.clusters[next] == seg.clusters[k]) ++next;
    uint32_t g0 = seg.clusters[k], g1 = next < len ? seg.clusters[next] : glyphCount;
    float sum = 0;
    for (uint32_t g = g0; g < g1; ++g) sum += seg.advances[g];
    if (spacing != 0 && g1 > g0) {
      seg.advances[g1 - 1] += spacing;
      sum += spacing;
    }
    adv[i] = sum;
  }
}

bool LineBreaker::Impl::CanBreakBefore(uint32_t i) const {
  if (i == 0 || i >= Size() || !clusterStart[i]) return false;
  uint8_t prev = kind[i - 1];
  if (prev == kSoftHyphen || prev == kZeroWidth || prev == kTab) return true;
  const wchar_t a = text[i - 1], b = text[i];
  if (!UsesAnalyzerBreaks(a) && !UsesAnalyzerBreaks(b)) {
    // Word breaks far less than UAX #14 (measured against Word, 2026): after a breaking space
    // whatever follows (also before "!", ")", "%"), and after a hyphen or dash ("pre-|1990",
    // "10-|20", "check—|often"), but never before a dash, and not after "/", "|", "!", "?", "…",
    // a thin space, or between a letter and "$", "€", "+", "(".
    if (Hangs(prev)) return true;
    return IsBreakingDash(a) && !Hangs(kind[i]) && b != 0x00A0;
  }
  auto after = static_cast<DWRITE_BREAK_CONDITION>(breaks[i - 1].breakConditionAfter);
  auto before = static_cast<DWRITE_BREAK_CONDITION>(breaks[i].breakConditionBefore);
  if (after == DWRITE_BREAK_CONDITION_MAY_NOT_BREAK || before == DWRITE_BREAK_CONDITION_MAY_NOT_BREAK) return false;
  return after == DWRITE_BREAK_CONDITION_CAN_BREAK || before == DWRITE_BREAK_CONDITION_CAN_BREAK ||
         after == DWRITE_BREAK_CONDITION_MUST_BREAK || before == DWRITE_BREAK_CONDITION_MUST_BREAK;
}

const RunFont& LineBreaker::Impl::FontOfRun(uint32_t run) {
  RunFont& rf = runFonts[run];
  if (rf.made) return rf;
  rf.made = true;
  const TextStyle& s = *styles[run];
  const std::wstring& family = fonts.Resolve(s.family);
  rf.face = fonts.Get(family, s.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, s.italic);
  rf.size = s.size;
  rf.rise = s.baselineShift;
  if (rf.face) {
    FontMetrics m = rf.face->Metrics(s.size);
    if (s.verticalAlign == VerticalAlign::Superscript) {
      rf.rise += m.superscriptRaise;
      rf.size = m.superscriptSize;
    } else if (s.verticalAlign == VerticalAlign::Subscript) {
      rf.rise -= m.subscriptLower;
      rf.size = m.subscriptSize;
    }
    rf.metrics = rf.face->Metrics(rf.size);
  }
  return rf;
}

void LineBreaker::Impl::HyphenFont(uint32_t at, const Face*& face, float& emSize, float& rise, uint32_t& run) {
  uint32_t i = at;
  if (!(segOf[i] >= 0) && i > 0 && segOf[i - 1] >= 0 && runOf[i - 1] == runOf[i]) --i;
  if (segOf[i] >= 0) {
    const Segment& s = segs[static_cast<size_t>(segOf[i])];
    face = s.face;
    emSize = s.emSize;
    rise = s.rise;
    run = s.run;
    return;
  }
  const RunFont& rf = FontOfRun(runOf[at]);
  face = rf.face;
  emSize = rf.size;
  rise = rf.rise;
  run = runOf[at];
}

float LineBreaker::Impl::HyphenAdvance(uint32_t at) {
  const Face* face = nullptr;
  float emSize = 0, rise = 0;
  uint32_t run = 0;
  HyphenFont(at, face, emSize, rise, run);
  return face ? face->Advance(L'-', emSize) + styles[run]->letterSpacing : 0;
}

TabInfo LineBreaker::Impl::NextTab(float x, bool first) const {
  const ParagraphFormat& f = p.format;
  TabInfo best;
  bool found = false;
  float lastCustom = -std::numeric_limits<float>::max();
  for (const auto& t : f.tabs) {
    if (t.align == TabAlign::Bar) continue;
    lastCustom = std::max(lastCustom, t.position);
    if (t.position > x + kEpsilon && (!found || t.position < best.stop)) {
      best = {t.position, t.align, t.leader, false};
      found = true;
    }
  }
  // A hanging indent is a tab stop on the first line, for list numbers.
  if (first && f.firstLineIndent < 0 && f.startIndent > x + kEpsilon && (!found || f.startIndent < best.stop)) {
    best = {f.startIndent, TabAlign::Start, TabLeader::None, false};
    found = true;
  }
  if (found) return best;
  float interval = f.defaultTabStop;
  if (interval <= 0) return {x, TabAlign::Start, TabLeader::None, true};
  float base = std::max(x, lastCustom);
  float stop = (std::floor((base + kEpsilon) / interval) + 1) * interval;
  return {stop, TabAlign::Start, TabLeader::None, true};
}

Fit LineBreaker::Impl::FindBreak(uint32_t a, float s0, float s1, bool first) {
  const uint32_t n = Size();
  TabContext tab;
  tab.origin = s0;
  float content = 0, beforeDecimal = -1, trailing = 0, limit = s1;
  bool placed = false;
  uint32_t lastBreak = 0;  // 0: none yet (a break is always after a)
  bool lastHyphen = false;
  float lastContent = 0, lastEnd = s0;
  uint32_t segFrom = a;  // where the text after the last tab starts
  auto end = [&](float c) { return SegmentStart(tab, c, beforeDecimal) + c; };

  for (uint32_t i = a; i < n; ++i) {
    const uint8_t k = kind[i];
    if (Mandatory(k)) return {i + 1, EndingOf(k), false};
    if (k == kTab) {
      float from = end(content);
      TabInfo t = NextTab(from, first);
      // Past the last stop that fits: the tab fills the line, and the text after it wraps.
      if (t.isDefault && t.stop > s1 + kEpsilon) return {i + 1, LineEnd::Wrap, false};
      tab = {true, s0, from, t.stop, t.align, t.leader};
      segFrom = i + 1;
      content = 0;
      beforeDecimal = -1;
      trailing = 0;
      placed = true;
      // Text aligned on a custom stop past the end of the line may reach that stop.
      limit = t.align != TabAlign::Start && !t.isDefault ? std::max(s1, t.stop) : s1;
      lastBreak = i + 1;
      lastHyphen = false;
      lastContent = 0;
      lastEnd = end(0);
      continue;
    }
    if (Hangs(k)) {
      content += adv[i];
      trailing += adv[i];
      if (CanBreakBefore(i + 1)) {
        lastBreak = i + 1;
        lastHyphen = false;
        lastContent = content;
        lastEnd = end(content - trailing);
      }
      continue;
    }
    if (k == kSoftHyphen) {
      float hyphenEnd = end(content) + HyphenAdvance(i);
      if (placed && i + 1 < n && hyphenEnd <= limit + kEpsilon) {
        lastBreak = i + 1;
        lastHyphen = true;
        lastContent = content;
        lastEnd = hyphenEnd;
      }
      continue;
    }
    const float w = adv[i];
    if (tab.active && tab.align == TabAlign::Decimal && beforeDecimal < 0 && text[i] == p.format.decimalSeparator)
      beforeDecimal = content;
    if (placed && w > 0 && end(content + w) > limit + kEpsilon) {
      // The cluster at i doesn't fit. The word it's in: from the last break to the next one.
      uint32_t ws = lastBreak ? lastBreak : a;
      uint32_t we = i + 1;
      while (we < n && kind[we] == kText && !CanBreakBefore(we)) ++we;
      if (lastBreak) {
        if (uint32_t e = Squeeze(segFrom, i, we, content, limit, lastEnd, tab, beforeDecimal))
          return {e, LineEnd::Wrap, false};
      }
      if (uint32_t h = Hyphenate(segFrom, ws, we, lastBreak != 0, lastBreak ? lastContent : 0, lastBreak ? lastEnd : s0,
                                 limit, tab, beforeDecimal))
        return {h, LineEnd::Wrap, true};
      if (lastBreak) return {lastBreak, LineEnd::Wrap, lastHyphen};
      // No break opportunity on the line: break the word before this cluster.
      return {i, LineEnd::Wrap, false};
    }
    content += w;
    trailing = 0;
    placed = true;
    if (CanBreakBefore(i + 1)) {
      lastBreak = i + 1;
      lastHyphen = false;
      lastContent = content;
      lastEnd = end(content);
    }
  }
  return {n, LineEnd::ParagraphEnd, false};
}

// Word fits a line that overflows by `over` into a justified line by shrinking its spaces (those
// after the last tab, before `upTo`): by at most a quarter of their width, and by at most half as
// much per space as wrapping would stretch them, with `lastEnd` where the line would then end.
// (Measured against Word 16.0, build 20326, in compatibility mode 15: probes in Courier New, in
// 0.5 pt steps, hit both limits exactly.)
bool LineBreaker::Impl::Squeezes(uint32_t segFrom, uint32_t upTo, float over, float limit, float lastEnd) const {
  if (p.format.alignment != Alignment::Justify || !p.format.squeezeSpaces) return false;
  uint32_t spaces = 0;
  float spaceWidth = 0;
  for (uint32_t k = segFrom; k < upTo; ++k) {
    if (kind[k] != kSpace) continue;
    ++spaces;
    spaceWidth += adv[k];
  }
  if (spaces == 0 || over > spaceWidth / 4 + kEpsilon) return false;
  const float gap = limit - lastEnd;  // what wrapping would leave to stretch over spaces - 1 spaces
  return spaces == 1 || 2 * over / static_cast<float>(spaces) <= gap / static_cast<float>(spaces - 1) + kEpsilon;
}

// Keeps the word that overflows a justified line on it when Squeezes allows. Returns where the line
// then ends, or 0.
uint32_t LineBreaker::Impl::Squeeze(uint32_t segFrom, uint32_t i, uint32_t we, float content, float limit,
                                    float lastEnd, const TabContext& tab, float beforeDecimal) {
  const uint32_t n = Size();
  // Only a word that ends at a break ("locale-" before "specific" is squeezed in), and only on a
  // line that will be justified: text must follow.
  if (we >= n || (kind[we] != kSpace && !(kind[we] == kText && CanBreakBefore(we)))) return 0;
  uint32_t e = we;
  while (e < n && Hangs(kind[e])) ++e;
  if (e >= n || Mandatory(kind[e])) return 0;
  float c = content;
  for (uint32_t k = i; k < we; ++k) c += adv[k];
  return Squeezes(segFrom, we, SegmentStart(tab, c, beforeDecimal) + c - limit, limit, lastEnd) ? e : 0;
}

// Hyphenates the word [ws, we) that doesn't fit: at the last point where it fits, squeezed in on a
// justified line as Squeeze would. Each run of letters is hyphenated on its own, so a web address
// hyphenates inside "example" and "policies" as in Word. Returns where the line then ends, or 0.
uint32_t LineBreaker::Impl::Hyphenate(uint32_t segFrom, uint32_t ws, uint32_t we, bool afterBreak, float contentAtWs,
                                      float endAtWs, float limit, const TabContext& tab, float beforeDecimal) {
  const HyphenationSettings& h = p.format.hyphenation;
  if (!h.automatic) return 0;
  if (h.consecutiveLimit > 0 && state.hyphenated >= h.consecutiveLimit) return 0;
  // Before Word 2013: the word moves to the next line instead when the line, with the spaces at its
  // end, would still end within the zone. (Measured in compatibility mode 14.)
  if (afterBreak && h.zone > 0 && limit - (SegmentStart(tab, contentAtWs, beforeDecimal) + contentAtWs) <= h.zone)
    return 0;
  // A word with optional hyphens breaks only there; they also end the part [ws, we).
  if ((ws > 0 && kind[ws - 1] == kSoftHyphen) || (we < Size() && kind[we] == kSoftHyphen)) return 0;
  for (uint32_t k = ws; k < we; ++k)
    if (kind[k] == kSoftHyphen) return 0;
  std::vector<uint32_t> points;
  for (uint32_t rs = ws; rs < we;) {
    uint32_t re = rs;
    while (re < we && kind[re] == kText && IsWordChar(text[re])) ++re;
    uint32_t cs = rs, ce = re;
    while (cs < ce && !IsLetter(text[cs])) ++cs;
    while (ce > cs && !IsLetter(text[ce - 1])) --ce;
    bool lower = false;
    for (uint32_t k = cs; k < ce; ++k)
      if (CharType(text[k]) & C1_LOWER) lower = true;
    const Hyphenator* hyphenator = ce - cs >= 4 && (h.caps || lower) ? Hyphenator::For(StyleOf(cs).language) : nullptr;
    if (hyphenator) {
      for (uint32_t point : hyphenator->Points(std::wstring_view(text).substr(cs, ce - cs)))
        if (clusterStart[cs + point]) points.push_back(cs + point);
    }
    rs = re == rs ? re + 1 : re;
  }
  for (auto it = points.rbegin(); it != points.rend(); ++it) {
    const uint32_t b = *it;
    float prefix = 0;
    for (uint32_t k = ws; k < b; ++k) prefix += adv[k];
    float c = contentAtWs + prefix + HyphenAdvance(b - 1);
    const float over = SegmentStart(tab, c, beforeDecimal) + c - limit;
    if (over <= kEpsilon || (afterBreak && Squeezes(segFrom, b, over, limit, endAtWs))) return b;
  }
  return 0;
}

// Word's line height with several fonts on a line: the tallest ascent with its font's line gap
// above it, and the deepest descent. (Not the tallest ascent plus the largest gap: a Symbol bullet
// beside Times New Roman would be 0.4 pt too tall.)
static void CombineMetrics(Line& line, float ascent, float descent, float gap) {
  const float top = std::max(line.ascent + line.lineGap, ascent + gap);
  line.ascent = std::max(line.ascent, ascent);
  line.descent = std::max(line.descent, descent);
  line.lineGap = top - line.ascent;
}

void LineBreaker::Impl::MarkMetrics(Line& line) {
  const TextStyle* mark = p.markStyle ? p.markStyle : (styles.empty() ? &DefaultStyle() : styles.back());
  const Face* face = fonts.Get(fonts.Resolve(mark->family), mark->bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                               mark->italic);
  if (!face) return;
  FontMetrics m = face->Metrics(mark->size);
  CombineMetrics(line, m.ascent, m.descent, m.lineGap);
}

Line LineBreaker::Impl::Build(uint32_t a, const Fit& fit, float s0, float s1, bool first) {
  const uint32_t n = Size();
  const uint32_t e = fit.end;
  const ParagraphFormat& f = p.format;
  Line line;
  line.ending = fit.ending;
  line.hyphenated = fit.hyphen;
  line.start = a == 0 ? 0 : a < n ? orig[a] : static_cast<uint32_t>(p.text.size());
  line.end = e >= n ? static_cast<uint32_t>(p.text.size()) : orig[e];
  line.left = rtl ? width - s1 : s0;
  line.right = rtl ? width - s0 : s1;
  const uint8_t paraLevel = rtl ? 1 : 0;

  // The content, without the break character and the spaces that hang at the end.
  uint32_t t = e;
  if (t > a && Mandatory(kind[t - 1])) --t;
  const uint32_t withSpaces = t;
  while (t > a && Hangs(kind[t - 1])) --t;
  // Word counts the spaces before a paragraph mark or line break when centering or right-aligning.
  const bool countTrailing = (fit.ending == LineEnd::ParagraphEnd || fit.ending == LineEnd::LineBreak) &&
                             (f.alignment == Alignment::Center || f.alignment == Alignment::End);
  const uint32_t alignEnd = countTrailing ? withSpaces : t;

  // Items, in logical order.
  std::vector<Item> items;
  for (uint32_t i = a; i < e;) {
    const uint8_t k = kind[i];
    Item it;
    it.from = i;
    it.level = level[i];
    if (Shaped(k) && segOf[i] >= 0) {
      uint32_t j = i + 1;
      uint32_t stop = i < alignEnd ? alignEnd : e;
      while (j < stop && Shaped(kind[j]) && segOf[j] == segOf[i]) ++j;
      it.to = j;
      it.seg = segOf[i];
      it.type = Item::Glyphs;
      for (uint32_t c = i; c < j; ++c) it.width += adv[c];
      i = j;
    } else {
      it.to = i + 1;
      it.type = k == kTab ? Item::Tab : k == kObject ? Item::Object : Item::Gap;
      it.width = k == kObject ? adv[i] : 0;
      if (k == kTab) it.level = paraLevel;
      ++i;
    }
    // Rule L1: the spaces and the break at the end of a line take the paragraph's direction.
    if (it.from >= alignEnd) {
      it.trailing = true;
      it.level = paraLevel;
    }
    items.push_back(it);
  }
  if (fit.hyphen && e > a) {
    Item it;
    it.from = it.to = e;
    it.type = Item::Hyphen;
    it.level = level[e - 1];
    it.width = HyphenAdvance(e - 1);
    items.push_back(it);
  }

  // Tab segments: the items between tabs, positioned by the tab before them.
  struct TabSegment {
    size_t first = 0, last = 0;  // items [first, last)
    float start = 0, content = 0, total = 0;
  };
  std::vector<TabSegment> tabSegments;
  TabContext tab;
  tab.origin = s0;
  float prevEnd = s0;
  auto closeSegment = [&](size_t first, size_t last) {
    TabSegment sg{first, last};
    float beforeDecimal = -1;
    for (size_t k = first; k < last; ++k) {
      const Item& it = items[k];
      sg.total += it.width;
      if (it.trailing) continue;
      if (tab.active && tab.align == TabAlign::Decimal && beforeDecimal < 0 && it.type == Item::Glyphs) {
        float part = 0;
        for (uint32_t c = it.from; c < it.to; ++c) {
          if (text[c] == f.decimalSeparator) {
            beforeDecimal = sg.content + part;
            break;
          }
          part += adv[c];
        }
      }
      sg.content += it.width;
    }
    sg.start = SegmentStart(tab, sg.content, beforeDecimal);
    prevEnd = sg.start + sg.content;
    tabSegments.push_back(sg);
  };
  size_t segFirst = 0;
  for (size_t k = 0; k < items.size(); ++k) {
    if (items[k].type != Item::Tab) continue;
    closeSegment(segFirst, k);
    TabInfo ti = NextTab(prevEnd, first);
    // Past the last stop that fits, the tab fills the line (FindBreak ended the line after it).
    float stop = ti.isDefault && ti.stop > s1 + kEpsilon ? std::max(prevEnd, s1) : ti.stop;
    tab = {true, s0, prevEnd, stop, ti.align, ti.leader};
    items[k].x = prevEnd;  // logical, until placed
    items[k].leader = ti.leader;
    segFirst = k + 1;
  }
  closeSegment(segFirst, items.size());

  // Alignment and justification act on the text after the last tab.
  TabSegment& last = tabSegments.back();
  std::vector<float> extra(e - a + 1, 0);  // added after each visible character
  float alignedEnd = last.start + last.content;
  if (countTrailing) {
    for (size_t k = last.first; k < last.last; ++k)
      if (items[k].trailing) alignedEnd += items[k].width;
  }
  const float free = s1 - alignedEnd;
  const uint32_t lastFrom = last.first < items.size() ? items[last.first].from : e;
  bool justify = f.alignment == Alignment::Justify &&
                 (fit.ending == LineEnd::Wrap || (fit.ending == LineEnd::LineBreak && f.expandLineBreaks));
  bool distribute = f.alignment == Alignment::Distribute;
  // A justified line can be wider than its box: FindBreak kept a word by squeezing the spaces.
  if (justify && std::fabs(free) > kEpsilon) {
    uint32_t spaces = 0;
    bool eastAsian = false;
    for (uint32_t c = lastFrom; c < t; ++c) {
      if (kind[c] == kSpace) ++spaces;
      if (IsEastAsianChar(text[c])) eastAsian = true;
    }
    if (spaces) {
      for (uint32_t c = lastFrom; c < t; ++c)
        if (kind[c] == kSpace) extra[c - a] = free / static_cast<float>(spaces);
    } else if (eastAsian && free > 0) {
      distribute = true;  // East Asian text without spaces spreads between characters
    }
  }
  if (distribute && free > kEpsilon) {
    std::vector<uint32_t> gaps;
    for (uint32_t c = lastFrom; c < t; ++c)
      if (clusterStart[c] && Shaped(kind[c])) gaps.push_back(c);
    if (gaps.size() > 1) {
      gaps.pop_back();  // nothing after the last character
      float each = free / static_cast<float>(gaps.size());
      for (uint32_t c : gaps) extra[c - a] = each;
    }
  }
  float added = 0;
  for (size_t k = last.first; k < last.last; ++k) {
    Item& it = items[k];
    if (it.type != Item::Glyphs) continue;
    for (uint32_t c = it.from; c < it.to; ++c) {
      it.width += extra[c - a];
      if (!it.trailing) added += extra[c - a];
    }
  }
  last.content += added;
  last.total += added;
  if (!justify && !distribute && free > 0) {
    if (f.alignment == Alignment::Center) last.start += free / 2;
    else if (f.alignment == Alignment::End) last.start += free;
  }

  // Place each tab segment's items, in visual order. Tabs sit between segments.
  for (size_t s = 0; s < tabSegments.size(); ++s) {
    const TabSegment& sg = tabSegments[s];
    if (s > 0) {
      Item& tabIt = items[sg.first - 1];
      float from = tabIt.x;  // logical
      tabIt.width = std::max(0.0f, sg.start - from);
      tabIt.x = rtl ? width - (from + tabIt.width) : from;
    }
    // Logically the segment is [start, start + total], its hanging items last. Right to left,
    // everything is at an odd level or higher, so rule L2 puts those items at the left.
    std::vector<size_t> order;
    for (size_t k = sg.first; k < sg.last; ++k) order.push_back(k);
    Reorder(order, items);
    float x = rtl ? width - (sg.start + sg.total) : sg.start;
    for (size_t k : order) {
      items[k].x = x;
      x += items[k].width;
    }
  }

  // Output: glyph pieces, objects, carets and metrics.
  const uint32_t span = line.end - line.start;
  line.caretX_.assign(span + 1, std::numeric_limits<float>::quiet_NaN());
  line.advance_.assign(span, 0);
  line.flags_.assign(span, 0);
  auto setCaret = [&](uint32_t c, float lead, float advance, bool rtlChar) {
    uint32_t k = orig[c] - line.start;
    line.caretX_[k] = lead;
    line.advance_[k] = advance;
    line.flags_[k] = static_cast<uint8_t>((rtlChar ? Line::kRtl : 0) | (caretStop[c] ? Line::kStop : 0));
  };
  auto useMetrics = [&](float asc, float desc, float gap) { CombineMetrics(line, asc, desc, gap); };
  float contentLeft = std::numeric_limits<float>::max(), contentRight = -std::numeric_limits<float>::max();
  std::vector<size_t> visual(items.size());
  for (size_t k = 0; k < items.size(); ++k) visual[k] = k;
  std::stable_sort(visual.begin(), visual.end(), [&](size_t x, size_t y) { return items[x].x < items[y].x; });

  for (size_t k : visual) {
    const Item& it = items[k];
    const bool odd = (it.level & 1) != 0;
    if (!it.trailing || it.type == Item::Hyphen) {
      contentLeft = std::min(contentLeft, it.x);
      contentRight = std::max(contentRight, it.x + it.width);
    }
    if (it.type == Item::Glyphs) {
      const Segment& sg = segs[static_cast<size_t>(it.seg)];
      useMetrics(sg.ascent, sg.descent, sg.lineGap);
      GlyphPiece piece;
      piece.face = sg.face->Get();
      piece.emSize = sg.emSize;
      piece.scale = sg.scale;
      piece.x = it.x;
      piece.width = it.width;
      piece.rise = sg.rise;
      piece.textStart = orig[it.from];
      piece.textEnd = orig[it.to - 1] + 1;
      piece.run = sg.run;
      piece.bidiLevel = it.level;
      uint32_t g0 = sg.GlyphAt(it.from), g1 = sg.GlyphAt(it.to);
      piece.glyphs.assign(sg.glyphs.begin() + g0, sg.glyphs.begin() + g1);
      piece.advances.assign(sg.advances.begin() + g0, sg.advances.begin() + g1);
      piece.offsets.assign(sg.offsets.begin() + g0, sg.offsets.begin() + g1);
      // Justification goes after each character's cluster, and carets follow the advances.
      float along = 0;
      for (uint32_t c = it.from; c < it.to;) {
        uint32_t next = c + 1;
        while (next < it.to && !clusterStart[next]) ++next;
        float clusterAdv = adv[c] + extra[c - a];
        if (extra[c - a] != 0) {
          uint32_t lastGlyph = sg.GlyphAt(next) - 1;
          if (lastGlyph >= g0 && lastGlyph < g1) piece.advances[lastGlyph - g0] += extra[c - a];
        }
        // Characters inside a cluster (a ligature) share its width.
        uint32_t parts = next - c;
        for (uint32_t q = c; q < next; ++q) {
          float share = clusterAdv / static_cast<float>(parts);
          float lead = along + share * static_cast<float>(q - c);
          setCaret(q, odd ? it.x + it.width - lead : it.x + lead, q == c ? clusterAdv : 0, odd);
        }
        along += clusterAdv;
        c = next;
      }
      if (!piece.glyphs.empty()) line.pieces.push_back(std::move(piece));
      continue;
    }
    if (it.type == Item::Hyphen) {
      GlyphPiece piece;
      const Face* face = nullptr;
      float rise = 0;
      HyphenFont(e - 1, face, piece.emSize, rise, piece.run);
      if (face) {
        UINT32 cp = L'-';
        UINT16 glyph = 0;
        face->Get()->GetGlyphIndices(&cp, 1, &glyph);
        piece.face = face->Get();
        piece.x = it.x;
        piece.width = it.width;
        piece.rise = rise;
        piece.textStart = piece.textEnd = line.end;
        piece.bidiLevel = it.level;
        piece.hyphen = true;
        piece.glyphs = {glyph};
        piece.advances = {it.width};
        piece.offsets = {DWRITE_GLYPH_OFFSET{}};
        FontMetrics m = face->Metrics(piece.emSize);
        useMetrics(m.ascent + rise, m.descent - rise, m.lineGap);
        line.pieces.push_back(std::move(piece));
      }
      continue;
    }
    const uint32_t c = it.from;
    const RunFont& rf = FontOfRun(runOf[c]);
    if (it.type == Item::Object) {
      const TextStyle& s = StyleOf(c);
      InlineObject obj{orig[c], runOf[c], it.x, it.width, s.objectAscent, s.objectDescent, s.baselineShift};
      useMetrics(s.objectAscent + s.baselineShift, s.objectDescent - s.baselineShift, 0);
      line.objects.push_back(obj);
    } else if (it.type != Item::Tab || it.leader != TabLeader::None) {
      // A tab without a leader draws nothing, and Word doesn't size the line by its font.
      useMetrics(rf.metrics.ascent + rf.rise, rf.metrics.descent - rf.rise, rf.metrics.lineGap);
    }
    setCaret(c, odd ? it.x + it.width : it.x, it.width, odd);
    if (it.type == Item::Tab && it.leader != TabLeader::None && it.width > 0 && rf.face) {
      wchar_t ch = it.leader == TabLeader::Dot ? L'.' : it.leader == TabLeader::Hyphen ? L'-'
                   : it.leader == TabLeader::MiddleDot ? 0x00B7 : L'_';
      float a1 = rf.face->Advance(ch, rf.size);
      if (a1 > 0) {
        auto count = static_cast<size_t>(std::floor(it.width / a1));
        if (count > 0) {
          GlyphPiece piece;
          UINT32 cp = ch;
          UINT16 glyph = 0;
          rf.face->Get()->GetGlyphIndices(&cp, 1, &glyph);
          piece.face = rf.face->Get();
          piece.emSize = rf.size;
          piece.width = static_cast<float>(count) * a1;
          piece.x = it.x + it.width - piece.width;  // leaders end where the text starts
          piece.rise = rf.rise;
          piece.textStart = piece.textEnd = orig[c];
          piece.run = runOf[c];
          piece.leader = true;
          piece.glyphs.assign(count, glyph);
          piece.advances.assign(count, a1);
          piece.offsets.assign(count, DWRITE_GLYPH_OFFSET{});
          line.pieces.push_back(std::move(piece));
        }
      }
    }
  }
  std::stable_sort(line.pieces.begin(), line.pieces.end(),
                   [](const GlyphPiece& x, const GlyphPiece& y) { return x.x < y.x; });
  // The paragraph mark's font sizes only a line with nothing drawn on it (an empty paragraph, or
  // the empty line after a line break): Word sizes a last line with text by its text alone.
  if (line.pieces.empty() && line.objects.empty()) MarkMetrics(line);

  // Where the content is, and the carets of hidden text and the line's end.
  float startX = rtl ? width - last.start : last.start;
  if (contentLeft > contentRight) contentLeft = contentRight = (items.empty() ? (rtl ? width - s0 : s0) : startX);
  line.contentLeft = contentLeft;
  line.contentRight = contentRight;
  float endCaret = rtl ? line.contentLeft : line.contentRight;
  if (e > a) {
    uint32_t c = e - 1;
    uint32_t k = orig[c] - line.start;
    if (!std::isnan(line.caretX_[k])) {
      bool odd = (line.flags_[k] & Line::kRtl) != 0;
      endCaret = odd ? line.caretX_[k] - line.advance_[k] : line.caretX_[k] + line.advance_[k];
    }
  }
  line.caretX_[span] = endCaret;
  for (uint32_t k = span; k-- > 0;) {
    if (std::isnan(line.caretX_[k])) line.caretX_[k] = line.caretX_[k + 1];
  }
  return line;
}

// ---------------------------------------------------------------------------------------------

LineBreaker::LineBreaker(Fonts& fonts, const Paragraph& paragraph, float columnWidth)
    : impl_(std::make_unique<Impl>(fonts, paragraph, columnWidth)) {}

LineBreaker::~LineBreaker() = default;

bool LineBreaker::Done() const { return impl_->state.done; }

Line LineBreaker::Next() { return Next(0, impl_->width); }

Line LineBreaker::Next(float left, float right) {
  Impl& m = *impl_;
  const ParagraphFormat& f = m.p.format;
  const bool first = m.state.line == 0;
  // Logical coordinates: from the column's start edge.
  float dStart = m.rtl ? m.width - right : left;
  float dEnd = m.rtl ? m.width - left : right;
  float s0 = f.startIndent + (first ? f.firstLineIndent : 0);
  float s1 = m.width - f.endIndent;
  // A float narrowing the line pushes the text in; otherwise indents may reach into the margin.
  if (dStart > kEpsilon) s0 = std::max(s0, dStart);
  if (dEnd < m.width - kEpsilon) s1 = std::min(s1, dEnd);

  const uint32_t a = m.state.position;
  Fit fit = a < m.Size() ? m.FindBreak(a, s0, s1, first) : Fit{a, LineEnd::ParagraphEnd, false};
  Line line = m.Build(a, fit, s0, s1, first);
  m.state.position = fit.end;
  ++m.state.line;
  m.state.hyphenated = fit.hyphen ? m.state.hyphenated + 1 : 0;
  if (fit.ending == LineEnd::ParagraphEnd) m.state.done = true;
  return line;
}

LineBreaker::State LineBreaker::Save() const { return impl_->state; }
void LineBreaker::Restore(const State& state) { impl_->state = state; }

std::vector<Line> Layout(Fonts& fonts, const Paragraph& paragraph, float columnWidth, const LineSpaceFn& space,
                         const LineHeightFn& height) {
  auto heightOf = [&](const Line& l) { return height ? height(l) : l.ascent + l.descent + l.lineGap; };
  LineBreaker breaker(fonts, paragraph, columnWidth);
  std::vector<Line> lines;
  float top = 0, guess = 0;
  while (!breaker.Done()) {
    if (!space) {
      Line line = breaker.Next();
      line.top = top;
      top += heightOf(line);
      lines.push_back(std::move(line));
      continue;
    }
    LineBreaker::State before = breaker.Save();
    LineSpace sp = space(top, guess);
    Line line = breaker.Next(sp.left, sp.right);
    float h = heightOf(line);
    if (h > guess + kEpsilon) {
      // Taller than asked: what's free beside floats may be less.
      LineSpace again = space(sp.top, h);
      if (again.top != sp.top || again.left != sp.left || again.right != sp.right) {
        breaker.Restore(before);
        line = breaker.Next(again.left, again.right);
        h = heightOf(line);
        sp = again;
      }
    }
    line.top = sp.top;
    top = sp.top + h;
    guess = h;
    lines.push_back(std::move(line));
  }
  return lines;
}

// ---------------------------------------------------------------------------------------------

draw::GlyphRun GlyphPiece::Run() const {
  draw::GlyphRun r;
  r.face = face.Get();
  r.emSize = emSize;
  r.count = static_cast<uint32_t>(glyphs.size());
  r.indices = glyphs.data();
  r.advances = advances.data();
  r.offsets = offsets.data();
  r.bidiLevel = bidiLevel;
  return r;
}

float Line::CaretX(uint32_t offset) const {
  if (caretX_.empty()) return contentLeft;
  uint32_t k = offset <= start ? 0 : std::min(offset - start, static_cast<uint32_t>(caretX_.size() - 1));
  return caretX_[k];
}

uint32_t Line::HitTest(float x) const {
  uint32_t best = start;
  float bestDistance = std::numeric_limits<float>::max();
  for (uint32_t k = 0; k < caretX_.size(); ++k) {
    if (k < flags_.size() && !(flags_[k] & kStop)) continue;
    float d = std::fabs(caretX_[k] - x);
    if (d < bestDistance) {
      bestDistance = d;
      best = start + k;
    }
  }
  return best;
}

std::vector<std::pair<float, float>> Line::Ranges(uint32_t from, uint32_t to) const {
  std::vector<std::pair<float, float>> boxes;
  from = std::max(from, start);
  to = std::min(to, end);
  for (uint32_t o = from; o < to; ++o) {
    uint32_t k = o - start;
    if (advance_[k] <= 0) continue;
    float x0 = caretX_[k], x1 = (flags_[k] & kRtl) ? x0 - advance_[k] : x0 + advance_[k];
    boxes.emplace_back(std::min(x0, x1), std::max(x0, x1));
  }
  std::sort(boxes.begin(), boxes.end());
  std::vector<std::pair<float, float>> merged;
  for (const auto& b : boxes) {
    if (!merged.empty() && b.first <= merged.back().second + 0.01f) merged.back().second = std::max(merged.back().second, b.second);
    else merged.push_back(b);
  }
  return merged;
}

bool Line::IsCaretStop(uint32_t offset) const {
  if (offset < start || offset > end) return false;
  uint32_t k = offset - start;
  return k >= flags_.size() || (flags_[k] & kStop) != 0;
}

}  // namespace fw::text
