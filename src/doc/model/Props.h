#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "core/Units.h"
#include "doc/model/Tree.h"

namespace fw::doc {

// Resolved properties, in typed units (plan §4.3 rule 9, §6.3 "Property resolution").
//
// Document resolves them in Word's order: document defaults, table style (with its conditional
// formatting), numbering, the paragraph style chain, the character style chain, then direct
// formatting (ECMA-376 Part 1 §17.7.2 as amended by [MS-OI29500]; Resolve.cpp lists each
// departure from the standard, and test_doc.cpp tests each one). Toggle properties combine
// across the styles; direct formatting sets them outright.
//
// Strings (font and style names, languages) are views into the document and stay valid as long
// as it does.

// An sRGB color, 0xRRGGBB, or "auto" (the renderer picks black or white against the background).
struct Color {
  uint32_t rgb = 0;
  bool automatic = true;
  bool operator==(const Color&) const = default;
  static Color Rgb(uint32_t v) { return Color{v & 0xFFFFFF, false}; }
};

enum class Justification : uint8_t {
  Start,  // left in left-to-right paragraphs
  Center,
  End,
  Both,  // justified
  Distribute,
  LowKashida,
  MediumKashida,
  HighKashida,
  ThaiDistribute,
  NumTab,
};

enum class BorderStyle : uint8_t {
  None, Single, Thick, Double, Dotted, Dashed, DotDash, DotDotDash, Triple,
  ThinThickSmallGap, ThickThinSmallGap, ThinThickThinSmallGap,
  ThinThickMediumGap, ThickThinMediumGap, ThinThickThinMediumGap,
  ThinThickLargeGap, ThickThinLargeGap, ThinThickThinLargeGap,
  Wave, DoubleWave, DashSmallGap, DashDotStroked, ThreeDEmboss, ThreeDEngrave, Outset, Inset,
  Art,  // one of the picture borders ("apples", "basicBlackDots", ...): `name` says which
};

struct Border {
  BorderStyle style = BorderStyle::None;
  std::string_view name;  // w:val as written
  EighthPoints width{0};  // w:sz
  int32_t spacePt = 0;    // w:space: distance from the text, in points
  Color color;
  bool shadow = false, frame = false;
  bool Present() const { return style != BorderStyle::None; }
  bool operator==(const Border&) const = default;
};

struct Shading {
  std::string_view pattern;  // w:val: "clear", "solid", "pct25", "horzStripe", ...; empty for none
  Color color;               // the pattern's color
  Color fill;                // the background
  bool Present() const { return !pattern.empty() && pattern != "nil"; }
  bool operator==(const Shading&) const = default;
};

enum class Underline : uint8_t {
  None, Single, Words, Double, Thick, Dotted, DottedHeavy, Dash, DashedHeavy, DashLong, DashLongHeavy,
  DotDash, DashDotHeavy, DotDotDash, DashDotDotHeavy, Wave, WavyHeavy, WavyDouble,
};

enum class VertAlign : uint8_t { Baseline, Superscript, Subscript };

enum class Emphasis : uint8_t { None, Dot, Comma, Circle, UnderDot };

// w:highlight: the 16 colors Word offers, resolved to sRGB (None when absent or "none").
struct Highlight {
  bool present = false;
  uint32_t rgb = 0;
  bool operator==(const Highlight&) const = default;
};

enum class FontHint : uint8_t { Default, EastAsia, ComplexScript };

// The four fonts of a run, with theme fonts (+mj/+mn) resolved to names. Which one a character
// uses depends on its script and the hint (ECMA-376 Part 1 §17.3.2.26); fwtext decides.
struct RunFonts {
  std::string_view ascii, hAnsi, eastAsia, cs;
  FontHint hint = FontHint::Default;
  bool operator==(const RunFonts&) const = default;
};

struct RunProps {
  std::string_view charStyle;  // the w:rStyle id, empty for none
  RunFonts fonts;
  bool bold = false, boldCs = false, italic = false, italicCs = false;
  bool caps = false, smallCaps = false, strike = false, doubleStrike = false;
  bool outline = false, shadow = false, emboss = false, imprint = false;
  bool vanish = false, webHidden = false, specVanish = false;
  bool noProof = false, snapToGrid = true, rtl = false, complexScript = false, oMath = false;
  HalfPoints size{20}, sizeCs{20};  // 10 pt unless something says otherwise
  Color color;
  Underline underline = Underline::None;
  Color underlineColor;
  Highlight highlight;
  Shading shading;
  Border border;
  Twips spacing{0};           // extra space after each character (negative condenses)
  int32_t scale = 100;        // horizontal scale, in percent
  HalfPoints kern{0};         // kern at this size and above; 0 for no kerning
  HalfPoints position{0};     // raised (positive) or lowered (negative) from the baseline
  VertAlign vertAlign = VertAlign::Baseline;
  Emphasis emphasis = Emphasis::None;
  std::string_view lang, langEastAsia, langBidi;  // BCP 47 tags as written ("en-US")
  Twips fitText{0};           // fit the run's text into this width; 0 for none
  bool operator==(const RunProps&) const = default;
};

enum class LineRule : uint8_t {
  Auto,     // `line` is in 240ths of a single line (240 = single, 480 = double)
  Exact,    // `line` is the line height, in twips
  AtLeast,  // `line` is the minimum line height, in twips
};

enum class TabAlign : uint8_t { Start, Center, End, Decimal, Bar, Num, Clear };
enum class TabLeader : uint8_t { None, Dot, Hyphen, Underscore, Heavy, MiddleDot };

struct TabStop {
  Twips pos{0};
  TabAlign align = TabAlign::Start;
  TabLeader leader = TabLeader::None;
  bool operator==(const TabStop&) const = default;
};

struct ParaBorders {
  Border top, start, bottom, end, between, bar;  // start and end are w:left and w:right
  bool operator==(const ParaBorders&) const = default;
};

// w:framePr: a frame (text frame or drop cap) positioned like a floating object.
struct FrameProps {
  std::string_view dropCap, wrap, hAnchor, vAnchor, xAlign, yAlign, hRule;
  int32_t lines = 0;
  Twips w{0}, h{0}, x{0}, y{0}, hSpace{0}, vSpace{0};
  bool anchorLock = false;
  bool operator==(const FrameProps&) const = default;
};

struct ParaProps {
  std::string_view style;  // the paragraph style id (the default style when none is named)
  Justification jc = Justification::Start;
  // Indents. firstLine is negative for a hanging indent. The *Chars values, in hundredths of a
  // character, override the twips values when set (-1 when not).
  Twips indStart{0}, indEnd{0}, firstLine{0};
  int32_t indStartChars = -1, indEndChars = -1, firstLineChars = -1;
  bool mirrorIndents = false;
  // Spacing. The *Lines values, in hundredths of a line, override the twips values when set
  // (-1 when not); the auto values mean HTML-style automatic spacing.
  Twips spaceBefore{0}, spaceAfter{0};
  int32_t spaceBeforeLines = -1, spaceAfterLines = -1;
  bool spaceBeforeAuto = false, spaceAfterAuto = false;
  LineRule lineRule = LineRule::Auto;
  int32_t line = 240;
  bool contextualSpacing = false;
  bool keepNext = false, keepLines = false, pageBreakBefore = false;
  bool widowControl = true;
  bool suppressLineNumbers = false, suppressAutoHyphens = false, suppressOverlap = false;
  bool bidi = false, snapToGrid = true;
  bool wordWrap = true, kinsoku = true, overflowPunct = true, topLinePunct = false;
  bool autoSpaceDE = true, autoSpaceDN = true, adjustRightInd = true;
  std::vector<TabStop> tabs;  // sorted by position, cleared stops removed
  ParaBorders borders;
  Shading shading;
  std::optional<FrameProps> frame;
  int32_t outlineLevel = 9;  // 0-8 for headings; 9 is body text
  std::string_view textAlignment;  // vertical alignment of characters in the line: "auto", "baseline", ...
  std::string_view textDirection;
  // Numbering: the w:num id (empty when the paragraph isn't numbered) and the level.
  std::string_view numId;
  int32_t ilvl = 0;
  bool operator==(const ParaProps&) const = default;
};

enum class SectionStart : uint8_t { NextPage, Continuous, EvenPage, OddPage, NextColumn };
enum class HdrFtrType : uint8_t { Default, First, Even };

struct Columns {
  int32_t count = 1;
  Twips space{720};
  bool equalWidth = true;
  bool separator = false;
  struct Col {
    Twips width{0}, space{0};
  };
  std::vector<Col> cols;  // when not equal width
};

// Footnote or endnote numbering, from the section and the settings.
struct NoteProps {
  std::string_view pos;      // "pageBottom", "beneathText", "sectEnd", "docEnd"
  std::string_view numFmt;   // "decimal", "lowerRoman", ...
  int32_t start = 1;
  std::string_view restart;  // "continuous", "eachSect", "eachPage"
};

struct SectionProps {
  SectionStart start = SectionStart::NextPage;
  Twips pageWidth{12240}, pageHeight{15840};  // US Letter when the section doesn't say
  bool landscape = false;
  Twips marginTop{1440}, marginEnd{1440}, marginBottom{1440}, marginStart{1440};  // start = left
  Twips headerDistance{720}, footerDistance{720}, gutter{0};
  int32_t paperCode = 0;
  Columns columns;
  bool titlePage = false;  // a different first-page header and footer
  std::string_view vAlign;  // vertical alignment of text on the page: "top", "center", "both", "bottom"
  std::string_view textDirection;
  bool bidi = false, rtlGutter = false, formProt = false, noEndnote = false;
  // Page numbers.
  std::string_view pageNumberFormat;  // "decimal" when empty
  std::optional<int32_t> pageNumberStart;
  // Line numbers (countBy 0 = off). lineNumberStart is the first number shown (w:start + 1).
  int32_t lineNumberCountBy = 0, lineNumberStart = 1;
  Twips lineNumberDistance{0};
  std::string_view lineNumberRestart;
  // The document grid.
  std::string_view docGridType;
  Twips linePitch{0};
  int32_t charSpace = 0;
  // Page borders.
  Border pageBorderTop, pageBorderStart, pageBorderBottom, pageBorderEnd;
  std::string_view pageBorderOffsetFrom, pageBorderDisplay, pageBorderZOrder;
  NoteProps footnotes, endnotes;  // as this section overrides the document's
};

// A table width (w:tblW, w:tcW, cell margins): an amount and how to read it.
struct TableWidth {
  enum class Type : uint8_t { Auto, Nil, Dxa, Pct } type = Type::Auto;
  int32_t value = 0;  // Dxa: twips; Pct: fiftieths of a percent (5000 = 100 %)
  bool operator==(const TableWidth&) const = default;
};

struct CellMargins {
  TableWidth top, start, bottom, end;
  bool operator==(const CellMargins&) const = default;
};

struct TableBorders {
  Border top, start, bottom, end, insideH, insideV;
  bool operator==(const TableBorders&) const = default;
};

// w:tblLook: which conditional formats of the table style apply.
struct TableLook {
  bool firstRow = true, lastRow = false, firstColumn = true, lastColumn = false;
  bool noHBand = false, noVBand = true;
  bool operator==(const TableLook&) const = default;
};

// A floating table (w:tblpPr).
struct TableFloat {
  Twips leftFromText{0}, rightFromText{0}, topFromText{0}, bottomFromText{0};
  std::string_view vertAnchor, horzAnchor, xSpec, ySpec;
  Twips x{0}, y{0};
  bool overlap = true;
};

struct TableProps {
  std::string_view style;  // the table style id
  TableWidth width;
  Justification jc = Justification::Start;
  TableWidth indent;
  TableWidth cellSpacing;
  bool fixedLayout = false;
  CellMargins cellMargins;  // the table's defaults: 0 top and bottom, 108 twips (0.08") start and end
  TableBorders borders;
  Shading shading;
  TableLook look;
  std::optional<TableFloat> floating;
  bool bidiVisual = false;
  // Rows and columns per band. Word's default is 0, no banding ([MS-OI29500] 2.1.251, 2.1.253);
  // its built-in table styles set 1.
  int32_t rowBandSize = 0, colBandSize = 0;
  std::string_view caption, description;
};

struct RowProps {
  Twips height{0};
  std::string_view heightRule;  // "auto", "atLeast", "exact"
  bool cantSplit = false, header = false, hidden = false;
  int32_t gridBefore = 0, gridAfter = 0;
  TableWidth widthBefore, widthAfter;
  std::optional<Justification> jc;
  std::optional<TableWidth> cellSpacing;
};

enum class VMerge : uint8_t { None, Restart, Continue };

// Conditional formats of a table style that apply to a cell (ECMA-376 Part 1 §17.7.6).
enum CondFormat : uint16_t {
  kCondWholeTable = 0,
  kCondFirstRow = 1 << 0, kCondLastRow = 1 << 1, kCondFirstCol = 1 << 2, kCondLastCol = 1 << 3,
  kCondBand1Vert = 1 << 4, kCondBand2Vert = 1 << 5, kCondBand1Horz = 1 << 6, kCondBand2Horz = 1 << 7,
  kCondNeCell = 1 << 8, kCondNwCell = 1 << 9, kCondSeCell = 1 << 10, kCondSwCell = 1 << 11,
};

struct CellProps {
  TableWidth width;
  int32_t gridSpan = 1;
  VMerge vMerge = VMerge::None;
  VMerge hMerge = VMerge::None;  // legacy horizontal merge
  // Borders of this cell as the table, its style and the cell say (w:tcBorders over
  // w:tblBorders, with insideH and insideV for the inner edges). Resolving conflicts between
  // neighboring cells is layout's work.
  Border top, start, bottom, end, insideH, insideV, tl2br, tr2bl;
  Shading shading;
  CellMargins margins;
  std::string_view vAlign;  // "top", "center", "bottom", "both"
  std::string_view textDirection;
  bool noWrap = false, fitText = false, hideMark = false;
  uint16_t conditions = 0;  // CondFormat bits that applied
};

// Parsing helpers, shared with the resolver and useful to layout.
Justification ParseJustification(std::string_view v);
BorderStyle ParseBorderStyle(std::string_view v);
Underline ParseUnderline(std::string_view v);
std::optional<uint32_t> HighlightRgb(std::string_view v);  // nullopt for "none" and unknown names
TableWidth ParseTableWidth(Node width);
// A section's properties from its w:sectPr (null: every default).
SectionProps ResolveSection(Node sectPr);

}  // namespace fw::doc
