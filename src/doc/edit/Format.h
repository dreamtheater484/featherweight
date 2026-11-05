#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "doc/edit/TreeEdit.h"
#include "doc/model/Document.h"

namespace fw::doc::edit {

// Character and paragraph formatting (plan §6.2 "Characters", "Paragraphs"), as Word does it.
//
// Formatting writes direct properties (w:rPr in runs, w:pPr in paragraphs) only where they
// differ from what the styles give: a property set back to the style's value is removed, not
// written again. Runs are split at the range's ends. Properties Featherweight doesn't know, and
// known ones a command doesn't change, are kept as they are.

// A range of one story's text: from and to are paragraph positions, from <= to. Paragraphs in
// between are the story's (a story's paragraphs are numbered consecutively); text boxes anchored
// in them are other stories and aren't included.
struct Range {
  Pos from, to;
  bool Empty() const { return from == to; }
};

// Character formatting to apply: each field that is set is applied, the others are left alone.
struct RunFormat {
  std::optional<std::string> font;             // w:rFonts ascii, hAnsi and cs
  std::optional<HalfPoints> size;              // w:sz and w:szCs
  std::optional<bool> bold, italic, strike, doubleStrike, caps, smallCaps;
  std::optional<Underline> underline;          // None removes it
  std::optional<Color> color;                  // automatic: the style's color, or "auto"
  std::optional<std::string> highlight;        // a w:highlight name ("yellow"), "none" for none
  std::optional<VertAlign> vertAlign;          // superscript, subscript or baseline
  std::optional<std::string> charStyle;        // a character style id; empty removes w:rStyle
  bool Any() const;
};

enum class BorderSides : uint8_t { None, Bottom, Top, Left, Right, Outside, All };

// Paragraph formatting to apply, the same way.
struct ParaFormat {
  std::optional<Justification> jc;
  std::optional<Twips> indStart, indEnd;
  std::optional<Twips> firstLine;  // negative for a hanging indent
  std::optional<Twips> spaceBefore, spaceAfter;
  std::optional<LineRule> lineRule;
  std::optional<int32_t> line;     // with lineRule: 240ths of a line (Auto) or twips
  std::optional<bool> keepNext, keepLines, pageBreakBefore, widowControl, contextualSpacing;
  std::optional<BorderSides> borders;  // single 1/2 pt lines, as Word's Borders button makes them
  std::optional<Color> shading;        // automatic: none
  bool Any() const;
};

// What a range looks like, for the toolbar: a value where the whole range agrees, nullopt where it
// is mixed. An empty range is the character before it (or after it, at a paragraph's start).
struct FormatState {
  std::optional<std::string> font;
  std::optional<HalfPoints> size;
  bool bold = false, italic = false, strike = false, superscript = false, subscript = false;
  bool underline = false;
  std::optional<Color> color;
  std::optional<Highlight> highlight;
  std::optional<Justification> jc;
  std::optional<std::string> paraStyle, charStyle;
  std::optional<LineRule> lineRule;
  std::optional<int32_t> line;
  // Lists: every paragraph in one, and of which kind.
  bool bullets = false, numbered = false;
  std::optional<int32_t> level;
  std::string lang;  // BCP 47 ("en-US") of the first character
};

// The range's paragraphs (in the range's story, in order). A range that ends at the start of a
// paragraph doesn't include it, unless that's where it starts too.
std::vector<uint32_t> ParagraphsOf(const Document& doc, Range range);

FormatState StateOf(const Document& doc, Range range);

// Applies character formatting to the range's text, and to the paragraph marks the range covers
// (all but the last paragraph's), so list labels and empty lines follow. With an empty range in an
// empty paragraph, formats its mark.
void FormatRuns(TreeEdit& ed, Range range, const RunFormat& format);
void FormatParagraphs(TreeEdit& ed, Range range, const ParaFormat& format);

// The writers behind FormatRuns and FormatParagraphs, for any w:rPr or w:pPr (a style's too, with
// what its basedOn chain gives): each property set in `format` is removed when it equals
// `inherited`, written otherwise. WriteRunFormat leaves w:rStyle alone.
void WriteRunFormat(TreeEdit& ed, ElemId rPr, const RunFormat& format, const RunProps& inherited);
void WriteParaFormat(TreeEdit& ed, ElemId pPr, const ParaFormat& format, const ParaProps& inherited);

// Ctrl+B, Ctrl+I, Ctrl+U and the like: off when the whole range has it, on otherwise.
enum class Toggle : uint8_t { Bold, Italic, Underline, Strike, Superscript, Subscript };
void ToggleRuns(TreeEdit& ed, Range range, Toggle what);

// Ctrl+Shift+> and Ctrl+Shift+<: each run's size to the next one in Word's list (8, 9, 10, 11,
// 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72, then by 10 to 1638); below 8, by 1 down to 1.
// Ctrl+] and Ctrl+[ (`onePoint`): by one point, from 1 to 1638.
HalfPoints NextFontSize(HalfPoints size, bool grow);
void GrowFont(TreeEdit& ed, Range range, bool grow, bool onePoint = false);

// Ctrl+Space: removes direct character formatting (a character style stays).
void ClearRunFormatting(TreeEdit& ed, Range range);
// Ctrl+Q: removes direct paragraph formatting (the style, the list and section breaks stay).
void ClearParaFormatting(TreeEdit& ed, Range range);
// Clear All Formatting: both, character styles too, and the paragraphs back to the default style.
void ClearAllFormatting(TreeEdit& ed, Range range);

// Increase and decrease indent: list paragraphs change level; others move their left indent to the
// next or previous multiple of half an inch, as Word's buttons do.
void ShiftIndent(TreeEdit& ed, Range range, bool increase);

// The value Word writes for a justification in this document (left/right in transitional parts,
// start/end in strict ones).
std::string_view JcValue(const Document& doc, Justification jc);

}  // namespace fw::doc::edit
