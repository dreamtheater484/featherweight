#pragma once

#include <dwrite_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "draw/Painter.h"

// Line layout: one paragraph into lines, the way Word breaks them (plan §4.4, §6.3 "Layout").
// It's built on DirectWrite's text analyzer, not IDWriteTextLayout, which can't do Word's tab
// stops and leaders, lines of different widths beside floating pictures, or Word's justification.
//
//   - Itemization, bidi levels, line-break opportunities and number substitution come from
//     IDWriteTextAnalyzer; fonts from Fonts (with system fallback); glyphs from GetGlyphs and
//     GetGlyphPlacements, with ligatures and kerning only when the run asks for them, as in Word.
//   - Lines break first fit: as much as fits, then back to the last break opportunity. Spaces at
//     the end of a line hang past it and don't count; a word longer than the line breaks between
//     characters. With hyphenation on, the word that doesn't fit is hyphenated at the last point
//     where it fits (with a zone, only when the line would otherwise end more than the zone before
//     its end).
//   - Justification stretches only spaces (after the line's last tab), and never the last line or
//     a line ending in a page or column break. Like Word it also shrinks them, by up to a quarter,
//     to keep a word or the first part of a hyphenated one on a line. Distributed alignment
//     spreads every character, the last line too.
//
// All lengths are in points. x is measured from the column's left edge ("column coordinates").
// Line heights (auto, at least, exactly, multiple), paragraph spacing and pagination are the
// caller's: each line reports its fonts' ascent, descent and line gap.
//
// Characters with a meaning of their own in Paragraph::text:
//   U+0009 tab                U+000B, U+000A, U+000D, U+2028 line break (w:br)
//   U+000C page break         U+000E column break
//   U+00AD optional hyphen    U+2011 non-breaking hyphen (drawn as "-")
//   U+00A0 no-break space     U+200B no-width optional break
//   U+FFFC an inline object (picture, shape) the size its run's TextStyle gives
namespace fw::text {

class Fonts;

enum class Caps : uint8_t { None, All, Small };
enum class VerticalAlign : uint8_t { Baseline, Superscript, Subscript };

// A run's resolved character properties (the document model resolves styles, plan §6.3). Word
// picks the font per character: East Asian characters use the eastAsia family, complex-script
// characters (Arabic, Hebrew, Thai, Indic scripts...) and runs marked rtl or cs use the complex
// family, size, bold and italic, and everything else the ascii/hAnsi family.
struct TextStyle {
  std::wstring family;          // w:rFonts ascii and hAnsi
  std::wstring eastAsiaFamily;  // w:rFonts eastAsia; empty: family
  std::wstring complexFamily;   // w:rFonts cs; empty: family
  float size = 11;              // w:sz, in points
  float complexSize = 0;        // w:szCs; 0: size
  bool bold = false, italic = false;                // w:b, w:i
  bool complexBold = false, complexItalic = false;  // w:bCs, w:iCs
  float letterSpacing = 0;   // w:spacing: added after every character; may be negative
  float scale = 1;           // w:w: the glyphs' width, 1 for as designed (1.03 for 103 %)
  float baselineShift = 0;   // w:position: raised (positive) or lowered
  float kerningMinSize = 0;  // w:kern: kern pairs from this size up; 0: no kerning
  bool ligatures = false;    // w14:ligatures standard (or more): standard ligatures
  Caps caps = Caps::None;    // w:caps, w:smallCaps
  VerticalAlign verticalAlign = VerticalAlign::Baseline;  // w:vertAlign
  bool hidden = false;         // w:vanish: laid out as if absent
  bool rightToLeft = false;    // w:rtl
  bool complexScript = false;  // w:cs
  bool eastAsiaHint = false;   // w:rFonts/@w:hint="eastAsia": shared punctuation uses eastAsiaFamily
  std::wstring language;          // w:lang/@w:val (BCP 47): hyphenation, shaping, line breaks
  std::wstring eastAsiaLanguage;  // w:lang/@w:eastAsia; empty: language
  std::wstring complexLanguage;   // w:lang/@w:bidi; empty: language
  // An inline object's box for U+FFFC in this run. It stands on the baseline.
  float objectWidth = 0, objectAscent = 0, objectDescent = 0;
};

struct TextRun {
  uint32_t length = 0;               // UTF-16 code units
  const TextStyle* style = nullptr;  // must outlive the layout
};

// Logical, as in w:jc in a Word 2010+ file: Start is left in a left-to-right paragraph.
enum class Alignment : uint8_t { Start, Center, End, Justify, Distribute };
enum class TabAlign : uint8_t { Start, Center, End, Decimal, Bar };
enum class TabLeader : uint8_t { None, Dot, Hyphen, Underscore, Heavy, MiddleDot };

struct TabStop {
  float position = 0;  // from the column's start edge (left, or right in a right-to-left paragraph)
  TabAlign align = TabAlign::Start;
  TabLeader leader = TabLeader::None;
};

struct HyphenationSettings {
  bool automatic = false;    // w:autoHyphenation, and not w:suppressAutoHyphens
  // w:hyphenationZone (Word's default is 0.25 inch, 18 pt), for documents in compatibility mode 14
  // and earlier. Word 2013 and later ignore the zone in their own mode (15): pass 0 for those.
  float zone = 0;
  int consecutiveLimit = 0;  // w:consecutiveHyphenLimit: most lines in a row ending in a hyphen; 0: any
  bool caps = true;          // false: w:doNotHyphenateCaps (words typed in capitals)
};

struct ParagraphFormat {
  Alignment alignment = Alignment::Start;
  bool rightToLeft = false;   // w:bidi
  // From the column's start and end edges; firstLineIndent is negative for a hanging indent,
  // whose position is then also a tab stop on the first line.
  float startIndent = 0, endIndent = 0, firstLineIndent = 0;
  std::vector<TabStop> tabs;  // custom stops, by position; bar tabs are drawn by the caller
  float defaultTabStop = 36;  // w:defaultTabStop; default stops start after the last custom stop
  HyphenationSettings hyphenation;
  bool expandLineBreaks = true;  // justify lines ending in a line break (false: w:doNotExpandShiftReturn)
  // A justified line may shrink its spaces to keep a word that overflows it, as Word 2013 and
  // later do (compatibility mode 15); earlier modes don't.
  bool squeezeSpaces = true;
  wchar_t decimalSeparator = L'.';
  // Digits: none (as typed), or by context or locale for Arabic and other scripts.
  DWRITE_NUMBER_SUBSTITUTION_METHOD digits = DWRITE_NUMBER_SUBSTITUTION_METHOD_NONE;
};

struct Paragraph {
  std::wstring_view text;   // UTF-16, without the paragraph mark
  std::vector<TextRun> runs;  // in order, covering text exactly
  // The paragraph mark's properties: an empty paragraph's height, and the last line's.
  const TextStyle* markStyle = nullptr;
  ParagraphFormat format;
};

enum class LineEnd : uint8_t { Wrap, LineBreak, PageBreak, ColumnBreak, ParagraphEnd };

// Positioned glyphs in one font, from one TextRun (or a hyphen or tab leader).
struct GlyphPiece {
  Microsoft::WRL::ComPtr<IDWriteFontFace> face;
  float emSize = 0;
  float x = 0, width = 0;  // the box, column coordinates
  float rise = 0;          // above the line's baseline: superscript, subscript, w:position
  float scale = 1;         // the run's horizontal scale: advances include it; draw the glyphs
                           // through a horizontal scale about OriginX() with advances / scale
  uint32_t textStart = 0, textEnd = 0;  // in Paragraph::text; empty for a hyphen or leader
  uint32_t run = 0;                     // index into Paragraph::runs: color, underline, ...
  uint8_t bidiLevel = 0;
  bool hyphen = false;  // the hyphen of a hyphenated line end
  bool leader = false;  // a tab leader
  std::vector<uint16_t> glyphs;
  std::vector<float> advances;
  std::vector<DWRITE_GLYPH_OFFSET> offsets;

  // Where DrawGlyphs starts: the left edge, or the right edge for right-to-left glyphs.
  float OriginX() const { return (bidiLevel & 1) ? x + width : x; }
  // For draw::Painter::DrawGlyphs at (OriginX(), baseline - rise). Valid while this piece lives.
  draw::GlyphRun Run() const;
};

// An inline object's place on its line.
struct InlineObject {
  uint32_t offset = 0;  // of its U+FFFC
  uint32_t run = 0;
  float x = 0, width = 0, ascent = 0, descent = 0;
  float rise = 0;
};

struct Line {
  uint32_t start = 0, end = 0;  // in Paragraph::text; the lines partition it, hidden text included
  LineEnd ending = LineEnd::Wrap;
  bool hyphenated = false;  // ends with a hyphen: automatic, or at an optional hyphen
  float top = 0;            // set by Layout() from the LineSpaceFn; 0 from LineBreaker
  float left = 0, right = 0;  // the line's box after indents and floats, column coordinates
  // The content's extent, column coordinates: without trailing spaces, after alignment and
  // justification. A justified line spans [left, right].
  float contentLeft = 0, contentRight = 0;
  float ascent = 0, descent = 0, lineGap = 0;  // the largest of the fonts on the line (points)
  std::vector<GlyphPiece> pieces;  // left to right
  std::vector<InlineObject> objects;

  // Caret positions, for selection and copy. Offsets are in Paragraph::text, from start to end.
  // The x of the caret before `offset` (its character's leading edge; for end, the trailing edge
  // of the last character). Offsets in hidden text take the next visible character's caret.
  float CaretX(uint32_t offset) const;
  // The offset whose caret is nearest to x.
  uint32_t HitTest(float x) const;
  // The x ranges, left to right, that the characters in [from, to) cover: one range unless
  // right-to-left text is mixed in. For selection highlights and underlines.
  std::vector<std::pair<float, float>> Ranges(uint32_t from, uint32_t to) const;
  // False inside a grapheme (a surrogate pair, a base and its accents) and in hidden text.
  bool IsCaretStop(uint32_t offset) const;

  // Per offset in [start, end): the leading edge's x, the advance, and flags. Filled by the layout.
  std::vector<float> caretX_;
  std::vector<float> advance_;
  std::vector<uint8_t> flags_;  // kRtl, kStop
  static constexpr uint8_t kRtl = 1, kStop = 2;
};

// The free part of the column for one line, column coordinates.
struct LineSpace {
  float top = 0;  // where the line goes: the top asked for, or lower to skip past a float
  float left = 0, right = 0;
};

// Breaks a paragraph into lines one at a time, so pagination can place each line (and move to the
// next page) before asking for the next. Analysis and shaping happen once, in the constructor.
class LineBreaker {
 public:
  // `fonts`, the paragraph's text and its styles must outlive the breaker. `columnWidth` is where
  // the end edge is, for indents and right-to-left paragraphs.
  LineBreaker(Fonts& fonts, const Paragraph& paragraph, float columnWidth);
  ~LineBreaker();
  LineBreaker(const LineBreaker&) = delete;
  LineBreaker& operator=(const LineBreaker&) = delete;

  bool Done() const;
  // The next line, in the whole column.
  Line Next();
  // The next line where only [left, right] of the column is free (beside floating objects). The
  // paragraph's indents apply inside it.
  Line Next(float left, float right);

  // Where the breaker is, to lay a line out again (when its height changed what's free beside a
  // float) or to resume later.
  struct State {
    uint32_t position = 0;  // internal: the next visible character
    uint32_t line = 0;      // lines made so far
    int hyphenated = 0;     // lines in a row ending in a hyphen
    bool done = false;
  };
  State Save() const;
  void Restore(const State& state);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Asked for each line: `top` is where the line would start and `height` how tall it is (first an
// estimate, then again with the real height if it's taller).
using LineSpaceFn = std::function<LineSpace(float top, float height)>;
// How far a line moves the next one down: the caller's line-height rule. The default is
// ascent + descent + lineGap, Word's single spacing.
using LineHeightFn = std::function<float(const Line&)>;

// Lays out a whole paragraph in a column `columnWidth` wide, with lines from top 0 down, and sets
// each line's top. Without a LineSpaceFn every line gets the whole column.
std::vector<Line> Layout(Fonts& fonts, const Paragraph& paragraph, float columnWidth,
                         const LineSpaceFn& space = {}, const LineHeightFn& height = {});

}  // namespace fw::text
