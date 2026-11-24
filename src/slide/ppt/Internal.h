#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "slide/ppt/Ppt.h"

namespace pugi {
class xml_document;
}

// The .ppt reader's pieces ([MS-PPT]): records, text properties, and what the slide writer needs.
namespace fw::slide::ppt::detail {

// The XML part `name` of a zip package (PowerPoint 2007's round-trip data) into `doc`; false when
// it isn't there or doesn't parse.
bool PackageXml(std::span<const uint8_t> package, std::string_view name, pugi::xml_document& doc);

// Master units: PowerPoint 97-2003 positions are in 1/576 inch.
constexpr double kEmuPerUnit = 914400.0 / 576;

// Little-endian values from a span. Reading past the end gives zeros and makes Ok() false.
class Bytes {
 public:
  Bytes() = default;
  explicit Bytes(std::span<const uint8_t> b) : b_(b) {}
  size_t Pos() const { return pos_; }
  size_t Left() const { return pos_ < b_.size() ? b_.size() - pos_ : 0; }
  bool Ok() const { return ok_; }
  bool Skip(size_t n) {
    if (n > Left()) return Fail(), false;
    pos_ += n;
    return true;
  }
  uint8_t U8() { return Left() < 1 ? Fail() : b_[pos_++]; }
  uint16_t U16() {
    if (Left() < 2) return Fail();
    const uint16_t v = static_cast<uint16_t>(b_[pos_] | b_[pos_ + 1] << 8);
    pos_ += 2;
    return v;
  }
  uint32_t U32() {
    if (Left() < 4) return Fail();
    const uint32_t v = b_[pos_] | b_[pos_ + 1] << 8 | b_[pos_ + 2] << 16 | uint32_t{b_[pos_ + 3]} << 24;
    pos_ += 4;
    return v;
  }
  int16_t I16() { return static_cast<int16_t>(U16()); }
  int32_t I32() { return static_cast<int32_t>(U32()); }
  std::span<const uint8_t> Take(size_t n) {
    if (n > Left()) return Fail(), std::span<const uint8_t>();
    const auto s = b_.subspan(pos_, n);
    pos_ += n;
    return s;
  }

 private:
  uint8_t Fail() {
    ok_ = false;
    pos_ = b_.size();
    return 0;
  }
  std::span<const uint8_t> b_;
  size_t pos_ = 0;
  bool ok_ = true;
};

// A record ([MS-PPT] 2.3.1 RecordHeader): containers (ver 0xF) hold records in their body.
struct Rec {
  uint16_t ver = 0, inst = 0, type = 0;
  std::span<const uint8_t> body;
  bool Container() const { return ver == 0xF; }
};

// The records of `data` one after another; a body longer than what's left is cut to it.
class Records {
 public:
  explicit Records(std::span<const uint8_t> data) : d_(data) {}
  bool Next(Rec& r) {
    if (at_ > d_.size() || d_.size() - at_ < 8) return false;
    const uint8_t* p = d_.data() + at_;
    const uint16_t vi = static_cast<uint16_t>(p[0] | p[1] << 8);
    r.ver = vi & 0xF;
    r.inst = vi >> 4;
    r.type = static_cast<uint16_t>(p[2] | p[3] << 8);
    const uint32_t len = p[4] | p[5] << 8 | p[6] << 16 | uint32_t{p[7]} << 24;
    const size_t body = at_ + 8;
    const size_t n = std::min<size_t>(len, d_.size() - body);
    r.body = d_.subspan(body, n);
    at_ = body + n;
    return true;
  }

 private:
  std::span<const uint8_t> d_;
  size_t at_ = 0;
};

// The first record of `type` (and `inst`, when not negative) among a container's children.
inline std::optional<Rec> Find(std::span<const uint8_t> body, uint16_t type, int inst = -1) {
  Records rs(body);
  for (Rec r; rs.Next(r);)
    if (r.type == type && (inst < 0 || r.inst == inst)) return r;
  return std::nullopt;
}

// Paragraph properties: [MS-PPT] 2.9.18 TextPFException. `mask` says which are given (PFMasks);
// Over() lays another's given ones over these.
struct Pf {
  uint32_t mask = 0;
  uint16_t bulletFlags = 0;  // fHasBullet 1, fBulletHasFont 2, fBulletHasColor 4, fBulletHasSize 8
  uint16_t bulletChar = 0, bulletFont = 0, align = 0, fontAlign = 0, wrapFlags = 0, textDirection = 0;
  int16_t bulletSize = 0, lineSpacing = 0, spaceBefore = 0, spaceAfter = 0, leftMargin = 0, indent = 0, defaultTab = 0;
  uint32_t bulletColor = 0;  // ColorIndexStruct as read: red | green << 8 | blue << 16 | index << 24
  std::vector<std::pair<int16_t, uint16_t>> tabs;  // position, type
  bool Has(int bit) const { return (mask >> bit) & 1; }
  void Over(const Pf& o);
};
enum PfBit {
  kHasBullet = 0, kBulletHasFont = 1, kBulletHasColor = 2, kBulletHasSize = 3, kBulletFont = 4, kBulletColor = 5,
  kBulletSize = 6, kBulletChar = 7, kLeftMargin = 8, kIndent = 10, kAlign = 11, kLineSpacing = 12, kSpaceBefore = 13,
  kSpaceAfter = 14, kDefaultTab = 15, kFontAlign = 16, kCharWrap = 17, kWordWrap = 18, kOverflow = 19, kTabStops = 20,
  kTextDirection = 21
};

// Character properties: [MS-PPT] 2.9.11 TextCFException.
struct Cf {
  uint32_t mask = 0;
  uint16_t style = 0;  // CFStyle: bold 1, italic 2, underline 4, shadow 0x10, emboss 0x200 (where mask has the bit)
  uint16_t font = 0, eaFont = 0, ansiFont = 0, symbolFont = 0;
  int16_t size = 0, position = 0;
  uint32_t color = 0;  // ColorIndexStruct, as Pf::bulletColor
  bool Has(int bit) const { return (mask >> bit) & 1; }
  void Over(const Cf& o);
};
enum CfBit { kBold = 0, kItalic = 1, kUnderline = 2, kShadow = 4, kEmboss = 9, kTypeface = 16, kSize = 17, kColor = 18,
             kPosition = 19, kEaTypeface = 21, kAnsiTypeface = 22, kSymbolTypeface = 23 };

bool ReadPf(Bytes& b, Pf& pf);
bool ReadCf(Bytes& b, Cf& cf);

// A text type's master style ([MS-PPT] 2.9.35 TextMasterStyleAtom): up to five levels.
struct MasterStyle {
  std::array<std::optional<std::pair<Pf, Cf>>, 5> levels;
};
// Read from the atom's body; its instance is the text type (levels carry their number from type 5).
bool ReadMasterStyle(const Rec& r, MasterStyle& out);

// Text types ([MS-PPT] 2.13.33 TextTypeEnum).
enum TextType { kTitle = 0, kBody = 1, kNotes = 2, kOther = 4, kCenterBody = 5, kCenterTitle = 6, kHalfBody = 7, kQuarterBody = 8 };

// A text's margins per level from a TextRulerAtom ([MS-PPT] 2.9.48), in master units.
struct Ruler {
  std::array<std::optional<int16_t>, 5> leftMargin, indent;
  std::optional<int16_t> defaultTab;
  std::vector<std::pair<int16_t, uint16_t>> tabs;
};

// A run of text: the characters a PF or CF run covers ([MS-PPT] 2.9.44 TextPFRun, 2.9.45 TextCFRun).
struct PfRun {
  uint32_t count = 0;
  uint16_t level = 0;
  Pf pf;
};
struct CfRun {
  uint32_t count = 0;
  Cf cf;
};

// A field in the text: a slide number, date or footer stands at `pos` (one character, usually '*').
struct Field {
  uint32_t pos = 0;
  enum Kind { kSlideNumber, kDateTime, kGenericDate, kHeader, kFooter, kRtfDate } kind = kSlideNumber;
  uint32_t format = 0;  // DateTimeMCAtom's index
};

// A hyperlink over [begin, end) of the text, to an ExHyperlink's target.
struct TextLink {
  uint32_t begin = 0, end = 0;
  std::string target;  // a URL, or "#slide:<id>" for a jump to a slide
};

// PowerPoint 2000's additions to a paragraph ([MS-PPT] 2.9.20 TextPFException9), found by the index
// its first character's CF run gives (CFStyle pp9rt): an automatic number for a bulleted paragraph.
struct Pf9 {
  int autoNumber = -1;  // fBulletHasAutoNumber: -1 when not given
  int scheme = -1;      // TextAutoNumberSchemeEnum, -1 when not given
  int start = 1;
};
// A StyleTextProp9Atom's (0x0FAC) entries, from its body.
void ReadStyle9(std::span<const uint8_t> atom, std::vector<Pf9>& out);
// The "___PPT9" tag's records in a ProgTags container's body, or empty.
std::span<const uint8_t> Pp9Tag(std::span<const uint8_t> progTags);

// One text: in a shape's client text box, or the outline's for a placeholder.
struct Text {
  int type = kOther;
  std::u16string chars;
  std::vector<PfRun> pfRuns;
  std::vector<CfRun> cfRuns;
  std::optional<Ruler> ruler;
  std::vector<Field> fields;
  std::vector<TextLink> links;
  bool placeholder = true;  // false: a text box's or shape's own text (other text: the document's style)
  std::vector<Pf9> pf9;     // by pp9rt
  // By paragraph: margin and indent (DrawingML's marL and indent, EMUs) from PowerPoint 2007's copy of the shape.
  std::vector<std::optional<std::pair<double, double>>> indents;
  // By character: letter spacing (hundredths of a point) from that copy; empty when there's none.
  std::vector<int> spacing;
};

// Reads a text's records (TextHeaderAtom, TextCharsAtom or TextBytesAtom, StyleTextPropAtom,
// TextRulerAtom, meta characters) from `records` (a client text box's body, or the outline's
// records of one text) into `out`. `link` resolves an InteractiveInfo's hyperlink id to a target.
void ReadText(std::span<const uint8_t> records, Text& out, const std::function<std::string(uint32_t)>& link);

// Colors: a slide's scheme ([MS-PPT] 2.12.2 ColorSchemeAtom) as 0xRRGGBB.
struct Scheme {
  std::array<uint32_t, 8> rgb = {0xFFFFFF, 0x000000, 0x808080, 0x000000, 0xBBE0E3, 0x333399, 0x009999, 0x99CC00};
};
Scheme ReadScheme(std::span<const uint8_t> body);

// What a slide's text and shapes are written with.
struct Context {
  const std::vector<std::string>* fonts = nullptr;  // the font collection, by index
  Scheme scheme;
  // Text styles by text type (0-8): the master's, with the document's defaults beneath.
  std::array<std::optional<MasterStyle>, 9> styles;
  // The document's style for other text (its Environment's TextMasterStyleAtom), which text that isn't in a
  // placeholder takes as PowerPoint does (a .pptx's defaultTextStyle), rather than the master's.
  std::optional<MasterStyle> docOther;
  Pf basePf;
  Cf baseCf;
  bool notes = false;
  // Kerning (hundredths of a point; 0: none) by title, body and other style and level: the binary
  // format has none, PowerPoint 2007 and later keep it in the master's RoundTripOArtTextStyles12Atom.
  std::array<std::array<int, 5>, 3> kern{};
};

// A master's shape as the shapes on slides that name it (hspMaster) inherit from it: its type and
// properties.
struct MasterShape {
  uint16_t spt = 0;
  std::map<uint16_t, uint32_t> props;
  std::map<uint16_t, std::span<const uint8_t>> complex;
};
using ShapeIndex = std::map<uint32_t, MasterShape>;  // by shape id (unique in the document)
// Adds a drawing's shapes to `index`.
void IndexShapes(std::span<const uint8_t> drawing, ShapeIndex& index);

// What a drawing (a slide's, master's or notes page's OfficeArt shapes) is written with.
struct DrawingEnv {
  Context text;
  // The masters' shapes, for shapes that inherit from one (beneath the shape's own properties).
  const ShapeIndex* masters = nullptr;
  // A BLIP store picture (pib, 1-based) as the relationship id of its media part; empty when none.
  std::function<std::string(uint32_t)> picture;
  // A hyperlink target (URL or ppaction) as a relationship id.
  std::function<std::string(const std::string&)> link;
  // An ExHyperlink's id as its target.
  std::function<std::string(uint32_t)> hyperlink;
  // The outline's text a placeholder refers to (OutlineTextRefAtom's index); null when none.
  std::function<const Text*(int32_t)> outline;
  bool master = false;  // a master's placeholders keep their prompt text
  bool notes = false;   // a notes page: its body placeholder takes the notes text type
};

// A drawing's shapes as an spTree's children, and its background shape's fill (a:solidFill ...,
// for p:bgPr; empty without one).
struct DrawingXmlOut {
  std::string shapes, background;
  int lastId = 1;
};
DrawingXmlOut DrawingXml(std::span<const uint8_t> drawing, const DrawingEnv& env);

// A text body's paragraphs (a:p ...) with their properties resolved from the context's styles,
// the ruler and the runs, written explicitly. Empty text writes one empty paragraph.
std::string ParagraphsXml(const Text& text, const Context& cx, const std::function<std::string(const std::string&)>& linkRel);
// A master's text styles for one text type (a:lvl1pPr ... a:lvl5pPr), resolved as ParagraphsXml.
std::string LevelStylesXml(const Context& cx, int type);
// A color as DrawingML: <a:srgbClr val="RRGGBB"/>.
std::string ColorXml(uint32_t rgb, int alpha = 100000);
std::string Esc(std::u16string_view s);
std::string Esc(std::string_view s);

}  // namespace fw::slide::ppt::detail
