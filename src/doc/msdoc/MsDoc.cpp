// Word 97-2003 binary documents ([MS-DOC]) read into WordprocessingML, which the docx reader then
// makes the model of. The binary structures are read with bounds checks everywhere: the file is
// untrusted, and what doesn't make sense is left out rather than trusted.
//
// The document's character positions (CPs) are mapped to the WordDocument stream through the
// piece table; character and paragraph properties come from the FKPs by file position (FC), as
// lists of sprms that change the style's properties. Each sprm becomes the WordprocessingML
// element that says the same, so the styles' inheritance stays Word's.
#include "doc/msdoc/MsDoc.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

#include "pkg/Zip.h"

namespace fw::doc::msdoc {
namespace {

constexpr size_t kMaxChars = 64u << 20;  // more text than any real document has
constexpr int kMaxDepth = 16;            // nested tables

// ---------------------------------------------------------------------------
// Bytes: little-endian reads that give 0 past the end.

struct Bytes {
  std::span<const uint8_t> d;
  size_t size() const { return d.size(); }
  bool empty() const { return d.empty(); }
  uint8_t U8(size_t o) const { return o < d.size() ? d[o] : 0; }
  uint16_t U16(size_t o) const { return static_cast<uint16_t>(U8(o) | (U8(o + 1) << 8)); }
  uint32_t U32(size_t o) const { return U16(o) | (static_cast<uint32_t>(U16(o + 2)) << 16); }
  int16_t I16(size_t o) const { return static_cast<int16_t>(U16(o)); }
  int32_t I32(size_t o) const { return static_cast<int32_t>(U32(o)); }
  bool Has(size_t o, size_t n) const { return o <= d.size() && n <= d.size() - o; }
  Bytes Sub(size_t o, size_t n = SIZE_MAX) const {
    if (o >= d.size()) return {};
    return {d.subspan(o, std::min(n, d.size() - o))};
  }
};

// ---------------------------------------------------------------------------
// Sprms: a 16-bit operation, whose top three bits give the operand's size.

struct Sprm {
  uint16_t op;
  Bytes arg;  // a variable-length operand starts with its size
};

size_t OperandSize(uint16_t op, const Bytes& rest) {
  switch (op >> 13) {
    case 0: case 1: return 1;
    case 2: case 4: case 5: return 2;
    case 3: return 4;
    case 7: return 3;
    default: break;
  }
  if (op == 0xD608 || op == 0xD606) return size_t{rest.U16(0)} + 1;  // TDefTableOperand.cb: the rest, plus one
  if (op == 0xC615 && rest.U8(0) == 255) {                          // sprmPChgTabs with close tolerances
    const size_t del = rest.U8(1), ins = rest.U8(2 + 4 * del);
    return 3 + 4 * del + 3 * ins;
  }
  return size_t{rest.U8(0)} + 1;
}

template <class F>
void ForEachSprm(const Bytes& g, F&& f) {
  for (size_t i = 0; g.Has(i, 2);) {
    const uint16_t op = g.U16(i);
    const Bytes rest = g.Sub(i + 2);
    const size_t n = OperandSize(op, rest);
    if (!rest.Has(0, n)) break;
    f(Sprm{op, rest.Sub(0, n)});
    i += 2 + n;
  }
}

// ---------------------------------------------------------------------------
// Strings and XML.

// An Xst: a 16-bit count, then that many UTF-16 code units. `o` moves past it.
std::u16string Xst(const Bytes& b, size_t& o) {
  const size_t n = b.U16(o);
  o += 2;
  std::u16string s;
  if (!b.Has(o, 2 * n)) {
    o = b.size();
    return s;
  }
  for (size_t k = 0; k < n; ++k) s.push_back(static_cast<char16_t>(b.U16(o + 2 * k)));
  o += 2 * n;
  return s;
}

// An STTB's strings: extended (fExtend 0xFFFF, UTF-16 with 16-bit counts) or 8-bit.
std::vector<std::u16string> Sttb(const Bytes& b) {
  std::vector<std::u16string> out;
  const bool extended = b.U16(0) == 0xFFFF;
  size_t o = extended ? 2 : 0;
  const size_t count = b.U16(o), extra = b.U16(o + 2);
  o += 4;
  for (size_t i = 0; i < count && o < b.size(); ++i) {
    if (extended) {
      out.push_back(Xst(b, o));
    } else {
      const size_t n = b.U8(o++);
      std::u16string s;
      for (size_t k = 0; k < n; ++k) s.push_back(static_cast<char16_t>(b.U8(o + k)));
      o += n;
      out.push_back(std::move(s));
    }
    o += extra;
  }
  return out;
}

// UTF-16 as escaped UTF-8, without what XML can't hold (control characters, lone surrogates).
void Esc(std::string& out, std::u16string_view s) {
  for (size_t i = 0; i < s.size(); ++i) {
    char32_t c = s[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
      c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      ++i;
    } else if ((c >= 0xD800 && c < 0xE000) || c == 0xFFFE || c == 0xFFFF || (c < 0x20 && c != 9)) {
      continue;
    }
    switch (c) {
      case '&': out += "&amp;"; continue;
      case '<': out += "&lt;"; continue;
      case '>': out += "&gt;"; continue;
      case '"': out += "&quot;"; continue;
      default: break;
    }
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
}

std::string Esc(std::u16string_view s) {
  std::string out;
  Esc(out, s);
  return out;
}

std::string Num(int64_t v) { return std::to_string(v); }

std::string Hex(uint32_t v, int digits) {
  static const char k[] = "0123456789ABCDEF";
  std::string s(static_cast<size_t>(digits), '0');
  for (int i = digits - 1; i >= 0; --i, v >>= 4) s[static_cast<size_t>(i)] = k[v & 15];
  return s;
}

// <w:name w:val="v"/>
std::string El(std::string_view name, std::string_view val) {
  return "<w:" + std::string(name) + " w:val=\"" + std::string(val) + "\"/>";
}
std::string El(std::string_view name, int64_t val) { return El(name, Num(val)); }

// The namespaces of every part's root.
constexpr const char* kNs =
    " xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\""
    " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
    " xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\""
    " xmlns:v=\"urn:schemas-microsoft-com:vml\" xmlns:w10=\"urn:schemas-microsoft-com:office:word\"";

// Word's 16 colors (ico), 0 being automatic.
constexpr uint32_t kIco[17] = {0,        0x000000, 0x0000FF, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0xFFFF00, 0xFFFFFF,
                               0x000080, 0x008080, 0x008000, 0x800080, 0x800000, 0x808000, 0x808080, 0xC0C0C0};
constexpr const char* kHighlight[17] = {"none",      "black",    "blue",        "cyan",    "green",      "magenta",
                                        "red",       "yellow",   "white",       "darkBlue", "darkCyan",  "darkGreen",
                                        "darkMagenta", "darkRed", "darkYellow", "darkGray", "lightGray"};

std::string IcoColor(unsigned ico) { return ico == 0 || ico > 16 ? "auto" : Hex(kIco[ico], 6); }
// A COLORREF (0x00BBGGRR; 0xFF000000 is automatic).
std::string CvColor(uint32_t cv) {
  if ((cv >> 24) == 0xFF) return "auto";
  return Hex(((cv & 0xFF) << 16) | (cv & 0xFF00) | ((cv >> 16) & 0xFF), 6);
}

const char* BorderName(unsigned t) {
  static const char* const k[] = {"nil",          "single",          "thick",           "double",
                                  "single",       "single",          "dotted",          "dashed",
                                  "dotDash",      "dotDotDash",      "triple",          "thinThickSmallGap",
                                  "thickThinSmallGap", "thinThickThinSmallGap", "thinThickMediumGap",
                                  "thickThinMediumGap", "thinThickThinMediumGap", "thinThickLargeGap",
                                  "thickThinLargeGap", "thinThickThinLargeGap", "wave", "doubleWave",
                                  "dashSmallGap", "dashDotStroked", "threeDEmboss",  "threeDEngrave",
                                  "outset",       "inset"};
  return t < std::size(k) ? k[t] : "single";
}

// A Brc80 (4 bytes) or Brc (8 bytes) as the border element `name`; empty for the nil border
// (nothing said).
std::string Border(std::string_view name, const Bytes& b, bool brc80) {
  unsigned width = 0, type = 0, space = 0;
  bool shadow = false;
  std::string color;
  if (brc80) {
    if (b.U32(0) == 0xFFFFFFFF) return {};
    width = b.U8(0), type = b.U8(1), color = IcoColor(b.U8(2)), space = b.U8(3) & 0x1F, shadow = (b.U8(3) & 0x20) != 0;
  } else {
    if (b.U32(0) == 0xFFFFFFFF && b.U32(4) == 0xFFFFFFFF) return {};
    color = CvColor(b.U32(0)), width = b.U8(4), type = b.U8(5), space = b.U8(6) & 0x1F, shadow = (b.U8(6) & 0x20) != 0;
  }
  if (type == 0) return {};  // none
  std::string s = "<w:" + std::string(name) + " w:val=\"" + BorderName(type) + "\"";
  s += " w:sz=\"" + Num(std::clamp(width, 2u, 96u)) + "\" w:space=\"" + Num(space) + "\" w:color=\"" + color + "\"" +
         (shadow ? " w:shadow=\"1\"" : "");
  return s + "/>";
}

// A SHD80 (2 bytes) or SHD (10 bytes) as w:shd; empty for the nil shading.
std::string Shading(const Bytes& b, bool shd80) {
  static const char* const k[] = {"clear", "solid", "pct5", "pct10", "pct20", "pct25", "pct30", "pct40", "pct50",
                                  "pct60", "pct70", "pct75", "pct80", "pct90", "horzStripe", "vertStripe",
                                  "reverseDiagStripe", "diagStripe", "horzCross", "diagCross", "thinHorzStripe",
                                  "thinVertStripe", "thinReverseDiagStripe", "thinDiagStripe", "thinHorzCross",
                                  "thinDiagCross"};
  unsigned pat = 0;
  std::string fore, back;
  if (shd80) {
    const uint16_t v = b.U16(0);
    if (v == 0xFFFF) return {};
    fore = IcoColor(v & 0x1F), back = IcoColor((v >> 5) & 0x1F), pat = v >> 10;
  } else {
    pat = b.U16(8);
    if (pat == 0xFFFF) return {};
    fore = CvColor(b.U32(0)), back = CvColor(b.U32(4));
  }
  return std::string("<w:shd w:val=\"") + (pat < std::size(k) ? k[pat] : "clear") + "\" w:color=\"" + fore +
         "\" w:fill=\"" + back + "\"/>";
}

const char* NumberFormat(unsigned nfc) {
  static const char* const k[] = {"decimal", "upperRoman", "lowerRoman", "upperLetter", "lowerLetter", "ordinal",
                                  "cardinalText", "ordinalText", "hex", "chicago", "ideographDigital",
                                  "japaneseCounting", "aiueo", "iroha", "decimalFullWidth", "decimalHalfWidth",
                                  "japaneseLegal", "japaneseDigitalTenThousand", "decimalEnclosedCircle",
                                  "decimalFullWidth2", "aiueoFullWidth", "irohaFullWidth", "decimalZero", "bullet"};
  if (nfc == 255) return "none";
  return nfc < std::size(k) ? k[nfc] : "decimal";
}

// A DTTM as an xsd:dateTime; empty when it's 0.
std::string Date(uint32_t dttm) {
  if (dttm == 0) return {};
  const unsigned min = dttm & 0x3F, hr = (dttm >> 6) & 0x1F, day = (dttm >> 11) & 0x1F, mon = (dttm >> 16) & 0xF,
                 yr = 1900 + ((dttm >> 20) & 0x1FF);
  if (mon < 1 || mon > 12 || day < 1 || hr > 23 || min > 59) return {};
  auto two = [](unsigned v) { return (v < 10 ? "0" : "") + Num(v); };
  return Num(yr) + "-" + two(mon) + "-" + two(day) + "T" + two(hr) + ":" + two(min) + ":00Z";
}

// A language id (LCID) as a BCP 47 tag, empty for none.
std::string Lang(uint16_t lid) {
  static std::unordered_map<uint16_t, std::string> cache;
  if (lid == 0 || lid == 0x0400 || lid == 0x0800) return {};
  if (auto it = cache.find(lid); it != cache.end()) return it->second;
  wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
  std::string s;
  if (LCIDToLocaleName(lid, name, LOCALE_NAME_MAX_LENGTH, 0) > 0)
    for (const wchar_t* p = name; *p; ++p)
      if (*p < 0x80) s += static_cast<char>(*p);
  return cache[lid] = s;
}

// ---------------------------------------------------------------------------
// Properties, as the WordprocessingML elements they become, in schema order.

enum RSlot {
  rStyle, rFonts, rB, rBCs, rI, rICs, rCaps, rSmallCaps, rStrike, rDStrike, rOutline, rShadow, rEmboss, rImprint,
  rNoProof, rSnap, rVanish, rWebHidden, rColor, rSpacing, rW, rKern, rPosition, rSz, rSzCs, rHighlight, rU,
  rEffect, rBdr, rShd, rFitText, rVertAlign, rRtl, rCs, rEm, rLang, rEastAsian, rSpecVanish, kRSlots
};

struct Rpr {
  std::array<std::string, kRSlots> s;
  std::array<std::string, 4> font;  // ascii, eastAsia, hAnsi, cs
  std::array<std::string, 3> lang;  // val, eastAsia, bidi
  bool spec = false, ole = false, ins = false, del = false;
  int64_t pic = -1;  // sprmCPicLocation: the picture in the Data stream
  uint16_t ibst = 0, ibstDel = 0;
  uint32_t dttm = 0, dttmDel = 0;
  int cstyle = -1;  // sprmCIstd: the character style
  std::string sym;  // w:sym for a symbol character
};

enum PSlot {
  pStyle, pKeepNext, pKeepLines, pPageBreak, pFramePr, pWidow, pNumPr, pNoLnn, pBdr, pShd, pTabs, pNoHyph,
  pKinsoku, pWordWrap, pOverflow, pTopLine, pAutoDE, pAutoDN, pBidi, pAdjustRight, pSnap, pSpacing, pInd,
  pContextual, pMirror, pOverlap, pJc, pTextDir, pTextAlign, pTightWrap, pOutline, kPSlots
};

struct Ppr {
  std::array<std::string, kPSlots> s;
  uint16_t istd = 0;
  int ilvl = -1, ilfo = -1;
  std::optional<int> left, right, first, before, after, line;
  std::optional<bool> beforeAuto, afterAuto;
  const char* lineRule = "auto";
  std::array<std::string, 6> bdr;  // top, left, bottom, right, between, bar
  std::map<int, std::string> tabs;
  // A frame (w:framePr): position codes, position, size, wrap, distance from text; a drop cap.
  std::optional<int> pc, frameX, frameY, frameW, frameH, wrap, hSpace, vSpace, dropCap;
  int itap = 0;
  bool inTable = false, ttp = false, innerCell = false, innerTtp = false;
  std::vector<Bytes> table;  // grpprls whose table sprms describe the row this paragraph ends
};

struct Sep {
  int bkc = 2, xa = 12240, ya = 15840, left = 1800, right = 1800, top = 1440, bottom = 1440, header = 720,
      footer = 720, gutter = 0, cols = 1, colSpace = 720, nfc = 0, start = 1, vjc = 0, lnnMod = 0, lnc = 0,
      dxaLnn = 0, lnnMin = 0, pitch = 0, clm = 0;
  bool titlePg = false, restart = false, lineBetween = false;
};

struct Tc {
  uint16_t flags = 0;  // TCGRF
  std::array<std::string, 4> brc;  // top, left, bottom, right
  std::string shd;
  std::array<int, 4> padding = {-1, -1, -1, -1};  // the cell's own margins: top, left, bottom, right
};

struct Tap {
  std::vector<int> centers;  // cell edges
  std::vector<Tc> tc;
  int gapHalf = 0, height = 0, jc = 0, istd = -1;
  bool header = false, cantSplit = false, autofit = true;
  std::array<std::string, 6> borders;  // top, left, bottom, right, insideH, insideV
  std::array<int, 4> padding = {-1, -1, -1, -1};  // top, left, bottom, right
  // A floating table: position codes (as a frame's), position, and distance from the text
  // (left, top, right, bottom).
  std::optional<int> pc, x, y;
  std::array<int, 4> fromText{};
};

// A toggle property's operand: 0 off, 1 on, 0x80 as the style says, 0x81 the style's opposite. The
// last is kept as "\x81name" until the paragraph's and character's styles are known (RprXml).
void Toggle(std::string& slot, std::string_view name, uint8_t v) {
  if (v == 0x80) slot.clear();
  else if (v == 0) slot = "<w:" + std::string(name) + " w:val=\"0\"/>";
  else if (v == 0x81) slot = "\x81" + std::string(name);
  else slot = "<w:" + std::string(name) + "/>";
}

// The toggle sprms, by their element's name.
constexpr std::pair<std::string_view, uint16_t> kToggleSprms[] = {
    {"b", 0x0835},       {"i", 0x0836},       {"strike", 0x0837},  {"outline", 0x0838}, {"shadow", 0x0839},
    {"smallCaps", 0x083A}, {"caps", 0x083B},  {"vanish", 0x083C},  {"dstrike", 0x2A53}, {"imprint", 0x0854},
    {"emboss", 0x0858},  {"bCs", 0x085C},     {"iCs", 0x085D}};

void OnOff(std::string& slot, std::string_view name, uint8_t v) {
  slot = v ? "<w:" + std::string(name) + "/>" : "<w:" + std::string(name) + " w:val=\"0\"/>";
}

const char* Underline(unsigned kul) {
  switch (kul) {
    case 0: return "none";
    case 2: return "words";
    case 3: return "double";
    case 4: return "dotted";
    case 6: return "thick";
    case 7: return "dash";
    case 9: return "dotDash";
    case 10: return "dotDotDash";
    case 11: return "wave";
    case 20: return "dottedHeavy";
    case 23: return "dashedHeavy";
    case 25: return "dashDotHeavy";
    case 26: return "dashDotDotHeavy";
    case 27: return "wavyHeavy";
    case 39: return "dashLong";
    case 43: return "wavyDouble";
    case 55: return "dashLongHeavy";
    default: return "single";
  }
}

const char* Justification(unsigned jc) {
  switch (jc) {
    case 1: return "center";
    case 2: return "right";
    case 3: return "both";
    case 4: return "distribute";
    case 5: return "mediumKashida";
    case 7: return "highKashida";
    case 8: return "lowKashida";
    case 9: return "thaiDistribute";
    default: return "left";
  }
}

// ---------------------------------------------------------------------------
// The converter.

struct PartOut {
  std::string name;     // "word/header1.xml"
  std::string content;  // headers and footers: their XML
  std::string rels;     // its relationships, without the root element
  int nextRel = 1;
  std::string Add(std::string_view type, std::string_view target) {
    const std::string id = "rId" + Num(nextRel++);
    rels += "<Relationship Id=\"" + id + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/" +
            std::string(type) + "\" Target=\"" + std::string(target) + "\"/>";
    return id;
  }
};

enum class StoryKind { Main, Footnote, Endnote, Comment, Header, TextBox };

class Converter {
 public:
  bool Convert(const StreamFn& streams, pkg::Status& status, std::vector<uint8_t>& out);
  bool decrypted = false;

 private:
  struct Piece {
    uint32_t cp0, cp1, fc;
    bool compressed;
    uint16_t prm;
  };
  struct Run {
    uint32_t fc0, fc1;
    Bytes grpprl;
    uint16_t istd;
  };
  struct Style {
    std::u16string name;
    std::string id;
    int type = 0, base = 0xFFF, next = 0xFFF;
    uint16_t sti = 0, grfstd = 0;
    Bytes papx, chpx;
    bool defined = false;
  };
  struct Para {
    uint32_t cp0, mark;  // its text, and its mark (cp1 when it has none)
    Ppr ppr;
    int depth = 0;
    bool cellEnd = false, rowEnd = false;
  };
  struct Field {
    bool code = true;  // still in the instruction
  };
  struct Story {
    StoryKind kind;
    PartOut* part;
    std::vector<Field> fields;
  };

  // Reading.
  bool Load(const StreamFn& streams, pkg::Status& status);
  Bytes Table(size_t index) const;
  void ReadPieces();
  void ReadFkps(size_t index, bool para, std::vector<Run>& out);
  void ReadStyles();
  void ReadFonts();
  void ReadLists();
  void ReadSections();
  void ReadNotes();
  const Piece* PieceAt(uint32_t cp) const;
  static uint32_t FcOf(const Piece& p, uint32_t cp) { return p.fc + (cp - p.cp0) * (p.compressed ? 1 : 2); }
  static const Run* Find(const std::vector<Run>& runs, uint32_t fc);
  Bytes PrmGrpprl(uint16_t prm) const;

  // Properties.
  void ApplyChp(const Bytes& g, Rpr& r) const;
  void ApplyPap(const Bytes& g, Ppr& p, int depth = 0) const;
  void ApplySep(const Bytes& g, Sep& s) const;
  void ApplyTap(const Bytes& g, Tap& t) const;
  Rpr ChpAt(uint32_t cp) const;
  Ppr PapAt(uint32_t markCp) const;
  std::string RprXml(const Rpr& r, bool mark = false);
  std::string PprXml(const Ppr& p, const std::string& rPr, const std::string& sectPr) const;
  std::string SepXml(size_t section);
  std::string StyleId(size_t istd) const { return istd < styles_.size() ? styles_[istd].id : std::string(); }
  std::string RevisionAttrs(uint16_t ibst, uint32_t dttm);

  // Writing.
  std::string EmitStory(uint32_t cp0, uint32_t cp1, Story& story, bool body = false);
  void EmitBlocks(const std::vector<Para>& paras, size_t i0, size_t i1, int depth, Story& story, std::string& out);
  void EmitTable(const std::vector<Para>& paras, size_t i0, size_t i1, int depth, Story& story, std::string& out);
  void EmitPara(const Para& p, Story& story, std::string& out);
  void EmitRuns(uint32_t cp0, uint32_t cp1, Story& story, std::string& out);
  void EmitMarks(uint32_t cp, std::string& out);
  std::string Picture(int64_t fc, PartOut& part);
  std::string Media(const std::string& ext, std::vector<uint8_t> bytes, PartOut& part);
  void ReadArt();
  std::string Floating(uint32_t cp, Story& story);
  std::string StylesXml();
  std::string NumberingXml();
  std::string FontsXml() const;
  std::string SettingsXml() const;
  std::string NotesXml(bool endnotes);
  std::string CommentsXml();

  std::vector<uint8_t> wordBuf_, tableBuf_, dataBuf_;
  Bytes word_, table_, data_;
  std::array<uint32_t, 8> ccp_{};  // text, footnotes, headers, macros, comments, endnotes, text boxes, header text boxes
  std::vector<std::pair<uint32_t, uint32_t>> fcLcb_;
  std::vector<Piece> pieces_;
  std::vector<Bytes> prcs_;
  std::u16string text_;
  std::vector<Run> chpx_, papx_;
  std::vector<Style> styles_;
  std::array<uint16_t, 4> defaultFonts_{};
  struct Font {
    std::u16string name;
    uint8_t ffid = 0, chs = 0;
    Bytes panose;
  };
  std::vector<Font> fonts_;
  // Lists: each LST's levels (LVL bytes), and each LFO's list and overrides.
  struct Level {
    Bytes lvlf, papx, chpx;
    std::u16string text;
  };
  struct List {
    int32_t lsid = 0, tplc = 0;
    uint8_t flags = 0;
    std::array<uint16_t, 9> styles{};
    std::vector<Level> levels;
  };
  struct Override {
    int32_t lsid = 0;
    std::vector<std::pair<int, int32_t>> starts;  // level, start
  };
  std::vector<List> lists_;
  std::vector<Override> overrides_;
  // The drawing layer.
  struct Shape {
    int64_t pib = 0;  // its picture: 1-based, in the blip store
    bool text = false;
  };
  std::vector<Bytes> bstore_;
  std::unordered_map<uint32_t, Shape> shapes_;           // by spid
  std::map<uint32_t, Bytes> spaMain_, spaHeader_;        // anchor CP -> SPA
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> textBoxes_, headerTextBoxes_;  // spid -> its story
  int z_ = 1;
  int paraStyle_ = -1;  // the paragraph being written's style, for toggles that say "the style's opposite"
  bool StyleToggle(int istd, uint16_t op) const;
  std::vector<std::pair<uint32_t, Sep>> sections_;  // each section's end CP and properties
  std::map<uint32_t, size_t> sectionEnds_;           // the CP of a section's last paragraph mark -> section
  std::vector<uint32_t> hdd_;                        // header story CPs
  std::map<uint32_t, int> footRefs_, endRefs_, commentRefs_;
  std::vector<std::pair<uint32_t, uint32_t>> footTexts_, endTexts_, commentTexts_;
  std::vector<std::pair<std::u16string, std::u16string>> commentBy_;  // author, initials
  std::vector<std::u16string> revisionAuthors_;
  std::multimap<uint32_t, std::pair<bool, int>> bookmarks_;  // CP -> (start?, id)
  std::vector<std::u16string> bookmarkNames_;
  std::set<int> bookmarksOpen_, bookmarksClosed_;
  bool facingPages_ = false;
  int tabStop_ = 720;

  // The package.
  std::vector<PartOut> parts_;  // headers and footers
  std::vector<std::pair<std::string, std::vector<uint8_t>>> media_;
  std::set<std::string> mediaTypes_;
  PartOut document_{"word/document.xml"}, footnotes_{"word/footnotes.xml"}, endnotes_{"word/endnotes.xml"},
      comments_{"word/comments.xml"};
  int docPr_ = 1, revision_ = 1;
};

Bytes Converter::Table(size_t index) const {
  if (index >= fcLcb_.size() || fcLcb_[index].second == 0) return {};
  return table_.Sub(fcLcb_[index].first, fcLcb_[index].second);
}

const Converter::Run* Converter::Find(const std::vector<Run>& runs, uint32_t fc) {
  auto it = std::upper_bound(runs.begin(), runs.end(), fc, [](uint32_t f, const Run& r) { return f < r.fc0; });
  if (it == runs.begin()) return nullptr;
  --it;
  return fc < it->fc1 ? &*it : nullptr;
}

const Converter::Piece* Converter::PieceAt(uint32_t cp) const {
  auto it = std::upper_bound(pieces_.begin(), pieces_.end(), cp, [](uint32_t c, const Piece& p) { return c < p.cp0; });
  if (it == pieces_.begin()) return nullptr;
  --it;
  return cp < it->cp1 ? &*it : nullptr;
}

Bytes Converter::PrmGrpprl(uint16_t prm) const {
  // Prm1: an index into the CLX's grpprls. (Prm0, one compressed sprm of a fast save, isn't read.)
  if ((prm & 1) == 0) return {};
  const size_t i = prm >> 1;
  return i < prcs_.size() ? prcs_[i] : Bytes{};
}

// ---------------------------------------------------------------------------
// Reading the streams.

bool Converter::Load(const StreamFn& streams, pkg::Status& status) {
  using Code = pkg::Status::Code;
  auto fail = [&](Code c, const char* msg) {
    status = pkg::Status::Fail(c, msg);
    return false;
  };
  std::optional<std::vector<uint8_t>> word = streams("WordDocument");
  if (!word) return fail(Code::Damaged, "This file isn't a Word document.");
  wordBuf_ = std::move(*word);
  word_ = Bytes{wordBuf_};
  if (word_.U16(0) != 0xA5EC || word_.size() < 154) return fail(Code::Damaged, "This Word document is damaged.");
  if (word_.U16(2) < 0xC0)
    return fail(Code::Unsupported,
                "This document was made with Word 6 or Word 95, which Featherweight doesn't open. Open it in Word and "
                "save it as a .docx.");
  const uint16_t flags = word_.U16(10);
  if ((flags & 0x0100) && !decrypted) return fail(Code::Unsupported, "This document is protected with a password.");
  std::optional<std::vector<uint8_t>> table = streams((flags & 0x0200) ? "1Table" : "0Table");
  if (!table) return fail(Code::Damaged, "This Word document is damaged: its table stream is missing.");
  tableBuf_ = std::move(*table);
  table_ = Bytes{tableBuf_};
  if (std::optional<std::vector<uint8_t>> data = streams("Data")) dataBuf_ = std::move(*data);
  data_ = Bytes{dataBuf_};

  // The FIB: FibBase, then counted arrays of 16-bit values, 32-bit values and (fc, lcb) pairs.
  size_t o = 32;
  o += 2 + 2 * size_t{word_.U16(o)};
  const size_t cslw = word_.U16(o);
  o += 2;
  for (size_t k = 0; k < ccp_.size() && 3 + k < cslw; ++k) ccp_[k] = word_.U32(o + 4 * (3 + k));
  o += 4 * cslw;
  const size_t pairs = word_.U16(o);
  o += 2;
  for (size_t k = 0; k < pairs && word_.Has(o + 8 * k, 8); ++k)
    fcLcb_.push_back({word_.U32(o + 8 * k), word_.U32(o + 8 * k + 4)});

  ReadPieces();
  if (pieces_.empty()) return fail(Code::Damaged, "This Word document is damaged: its text can't be found.");
  ReadFkps(12, false, chpx_);
  ReadFkps(13, true, papx_);
  ReadStyles();
  ReadFonts();
  ReadLists();
  ReadSections();
  ReadNotes();
  ReadArt();
  const Bytes dop = Table(31);
  facingPages_ = (dop.U8(0) & 1) != 0;
  if (const int tab = dop.U16(10); tab > 0 && tab < 20000) tabStop_ = tab;
  revisionAuthors_ = Sttb(Table(51));
  return true;
}

void Converter::ReadPieces() {
  const Bytes clx = Table(33);
  for (size_t o = 0; o < clx.size();) {
    const uint8_t type = clx.U8(o);
    if (type == 1) {  // Prc: a grpprl the pieces can refer to
      const size_t cb = static_cast<size_t>(std::max<int16_t>(0, clx.I16(o + 1)));
      prcs_.push_back(clx.Sub(o + 3, cb));
      o += 3 + cb;
      continue;
    }
    if (type != 2) break;
    const Bytes plc = clx.Sub(o + 5, clx.U32(o + 1));
    const size_t n = plc.size() >= 4 ? (plc.size() - 4) / 12 : 0;
    size_t total = 0;
    for (size_t k = 0; k < n; ++k) {
      const uint32_t cp0 = plc.U32(4 * k), cp1 = plc.U32(4 * k + 4);
      const size_t pcd = 4 * (n + 1) + 8 * k;
      const uint32_t raw = plc.U32(pcd + 2);
      Piece p{cp0, cp1, raw & 0x3FFFFFFF, (raw & 0x40000000) != 0, plc.U16(pcd + 6)};
      if (p.compressed) p.fc /= 2;
      if (cp1 <= cp0 || cp1 > kMaxChars || (!pieces_.empty() && cp0 < pieces_.back().cp1)) continue;
      // No more text than the stream holds.
      const size_t room = p.fc < word_.size() ? (word_.size() - p.fc) / (p.compressed ? 1 : 2) : 0;
      p.cp1 = static_cast<uint32_t>(std::min<size_t>(cp1, cp0 + room));
      if (p.cp1 <= cp0 || (total += p.cp1 - cp0) > kMaxChars) continue;
      pieces_.push_back(p);
    }
    break;
  }
  if (pieces_.empty()) return;
  static constexpr char16_t kCp1252[32] = {0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                           0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,   0x017D, 0x8F,
                                           0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                           0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};
  text_.assign(pieces_.back().cp1, u'\0');
  for (const Piece& p : pieces_)
    for (uint32_t cp = p.cp0; cp < p.cp1; ++cp) {
      const uint32_t fc = FcOf(p, cp);
      char16_t c = p.compressed ? static_cast<char16_t>(word_.U8(fc)) : static_cast<char16_t>(word_.U16(fc));
      if (p.compressed && c >= 0x80 && c < 0xA0) c = kCp1252[c - 0x80];
      text_[cp] = c;
    }
}

void Converter::ReadFkps(size_t index, bool para, std::vector<Run>& out) {
  const Bytes plc = Table(index);
  const size_t n = plc.size() >= 4 ? (plc.size() - 4) / 8 : 0;
  for (size_t k = 0; k < n; ++k) {
    const size_t pn = plc.U32(4 * (n + 1) + 4 * k) & 0x3FFFFF;
    const Bytes fkp = word_.Sub(pn * 512, 512);
    if (fkp.size() < 512) continue;
    const size_t count = std::min<size_t>(fkp.U8(511), para ? 29 : 101);
    for (size_t i = 0; i < count; ++i) {
      Run r{fkp.U32(4 * i), fkp.U32(4 * i + 4), {}, 0};
      if (r.fc1 <= r.fc0) continue;
      if (!para) {
        const size_t at = size_t{fkp.U8(4 * (count + 1) + i)} * 2;
        if (at) r.grpprl = fkp.Sub(at + 1, fkp.U8(at));
      } else {
        const size_t at = size_t{fkp.U8(4 * (count + 1) + 13 * i)} * 2;
        if (at == 0) continue;
        const size_t cb = fkp.U8(at);
        const Bytes d = cb ? fkp.Sub(at + 1, 2 * cb - 1) : fkp.Sub(at + 2, 2 * size_t{fkp.U8(at + 1)});
        r.istd = d.U16(0);
        r.grpprl = d.Sub(2);
      }
      out.push_back(r);
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const Run& a, const Run& b) { return a.fc0 < b.fc0; });
}

void Converter::ReadStyles() {
  const Bytes st = Table(1);
  const size_t cbStshi = st.U16(0);
  const Bytes stshi = st.Sub(2, cbStshi);
  const size_t count = std::min<size_t>(stshi.U16(0), 4096), cbBase = stshi.U16(2);
  for (size_t k = 0; k < 3; ++k) defaultFonts_[k] = stshi.U16(12 + 2 * k);
  defaultFonts_[3] = cbStshi >= 20 ? stshi.U16(18) : defaultFonts_[0];
  styles_.resize(count);
  size_t o = 2 + cbStshi;
  for (size_t istd = 0; istd < count && o < st.size(); ++istd) {
    const size_t cb = st.U16(o);
    const Bytes d = st.Sub(o + 2, cb);
    o += 2 + cb;
    if (cb < 10) continue;
    Style& s = styles_[istd];
    s.defined = true;
    s.sti = d.U16(0) & 0xFFF;
    s.type = d.U16(2) & 0xF;
    s.base = d.U16(2) >> 4;
    s.next = d.U16(4) >> 4;
    s.grfstd = d.U16(8);
    const size_t cupx = d.U16(4) & 0xF;
    size_t at = cbBase;
    s.name = Xst(d, at);
    at += 2;  // the name's terminating 0
    for (size_t u = 0; u < cupx && at < d.size(); ++u) {
      if (at & 1) ++at;
      const size_t n = d.U16(at);
      const Bytes upx = d.Sub(at + 2, n);
      at += 2 + n;
      // Paragraph styles: papx, chpx. Character styles: chpx. Tables: tapx, papx, chpx. Lists: papx.
      const int kind = s.type == 1 ? (u == 0 ? 1 : 2) : s.type == 2 ? 2 : s.type == 3 ? static_cast<int>(u) : 1;
      if (kind == 1) s.papx = upx.Sub(2);
      else if (kind == 2) s.chpx = upx;
    }
  }
  // Ids from the names, as Word makes them ("heading 1" is Heading1), each once.
  std::set<std::string> used;
  for (size_t istd = 0; istd < styles_.size(); ++istd) {
    Style& s = styles_[istd];
    if (!s.defined) continue;
    std::u16string name = s.name.substr(0, s.name.find(u','));
    std::string id;
    bool upper = true;
    for (char16_t c : name) {
      if ((c >= u'0' && c <= u'9') || (c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z')) {
        id += static_cast<char>(upper && c >= u'a' && c <= u'z' ? c - 32 : c);
        upper = false;
      } else if (c == u' ') {
        upper = true;
      }
    }
    if (id.empty()) id = "Style" + Num(static_cast<int64_t>(istd));
    std::string unique = id;
    for (int n = 1; !used.insert(unique).second; ++n) unique = id + Num(n);
    s.id = unique;
    if (name.empty()) s.name = std::u16string(unique.begin(), unique.end());
  }
}

void Converter::ReadFonts() {
  const Bytes t = Table(15);
  const size_t count = t.U16(0);
  for (size_t i = 0, o = 4; i < count && o < t.size(); ++i) {
    const size_t cb = t.U8(o);
    const Bytes ffn = t.Sub(o + 1, cb);
    o += 1 + cb;
    Font f;
    f.ffid = ffn.U8(0);
    f.chs = ffn.U8(3);
    f.panose = ffn.Sub(5, 10);
    for (size_t k = 39; k + 1 < ffn.size() && ffn.U16(k); k += 2) f.name.push_back(static_cast<char16_t>(ffn.U16(k)));
    fonts_.push_back(std::move(f));
  }
}

void Converter::ReadLists() {
  if (fcLcb_.size() <= 74) return;
  const Bytes lst = Table(73);
  const size_t count = static_cast<size_t>(std::clamp<int>(lst.I16(0), 0, 4096));
  // The levels follow the PlfLst, in the lists' order.
  const Bytes after = table_.Sub(fcLcb_[73].first + 2 + 28 * count);
  size_t o = 0;
  for (size_t i = 0; i < count; ++i) {
    const Bytes f = lst.Sub(2 + 28 * i, 28);
    List l;
    l.lsid = f.I32(0);
    l.tplc = f.I32(4);
    for (size_t k = 0; k < 9; ++k) l.styles[k] = f.U16(8 + 2 * k);
    l.flags = f.U8(26);
    const size_t levels = (l.flags & 1) ? 1 : 9;
    for (size_t k = 0; k < levels && o < after.size(); ++k) {
      Level v;
      v.lvlf = after.Sub(o, 28);
      v.papx = after.Sub(o + 28, v.lvlf.U8(25));
      v.chpx = after.Sub(o + 28 + v.lvlf.U8(25), v.lvlf.U8(24));
      o += 28 + v.lvlf.U8(25) + v.lvlf.U8(24);
      v.text = Xst(after, o);
      l.levels.push_back(std::move(v));
    }
    lists_.push_back(std::move(l));
  }
  const Bytes lfo = Table(74);
  const size_t lfos = static_cast<size_t>(std::clamp(lfo.I32(0), 0, 4096));
  size_t d = 4 + 16 * lfos;  // the LFOData, one per LFO
  for (size_t i = 0; i < lfos; ++i) {
    Override ov;
    ov.lsid = lfo.I32(4 + 16 * i);
    const size_t levels = lfo.U8(4 + 16 * i + 12);
    d += 4;  // cp
    for (size_t k = 0; k < levels && d < lfo.size(); ++k) {
      const int32_t start = lfo.I32(d);
      const uint32_t bits = lfo.U32(d + 4);
      d += 8;
      if (bits & 0x10) ov.starts.push_back({static_cast<int>(bits & 0xF), start});
      if (bits & 0x20) {  // a whole LVL follows
        d += 28 + lfo.U8(d + 25) + lfo.U8(d + 24);
        Xst(lfo, d);
      }
    }
    overrides_.push_back(std::move(ov));
  }
}

void Converter::ReadSections() {
  const Bytes plc = Table(6);
  const size_t n = plc.size() >= 4 ? (plc.size() - 4) / 16 : 0;
  for (size_t k = 0; k < n; ++k) {
    const uint32_t cp = std::min(plc.U32(4 * (k + 1)), ccp_[0]);
    if (!sections_.empty() && cp <= sections_.back().first) continue;
    Sep sep;
    const uint32_t fc = plc.U32(4 * (n + 1) + 12 * k + 2);
    if (fc != 0xFFFFFFFF) ApplySep(word_.Sub(size_t{fc} + 2, word_.U16(fc)), sep);
    sections_.push_back({cp, sep});
  }
  if (sections_.empty() || sections_.back().first < ccp_[0]) sections_.push_back({ccp_[0], Sep()});
  for (size_t k = 0; k + 1 < sections_.size(); ++k)
    if (sections_[k].first > 0) sectionEnds_[sections_[k].first - 1] = k;
  const Bytes hdd = Table(11);
  for (size_t o = 0; o + 4 <= hdd.size(); o += 4) hdd_.push_back(hdd.U32(o));
}

void Converter::ReadNotes() {
  const uint32_t ftnBase = ccp_[0], atnBase = ccp_[0] + ccp_[1] + ccp_[2] + ccp_[3], ednBase = atnBase + ccp_[4];
  // References: (n + 1) CPs and n records; texts: the stories' CPs.
  auto read = [&](size_t refIndex, size_t txtIndex, size_t cbData, uint32_t base, std::map<uint32_t, int>& refs,
                  std::vector<std::pair<uint32_t, uint32_t>>& texts) {
    const Bytes ref = Table(refIndex), txt = Table(txtIndex);
    const size_t n = ref.size() >= 4 ? (ref.size() - 4) / (4 + cbData) : 0;
    for (size_t k = 0; k < n; ++k) {
      refs[ref.U32(4 * k)] = static_cast<int>(k);
      const uint32_t a = txt.U32(4 * k), b = txt.U32(4 * k + 4);
      texts.push_back({base + a, base + std::max(a, b)});
    }
    return ref.Sub(4 * (n + 1));
  };
  read(2, 3, 2, ftnBase, footRefs_, footTexts_);
  read(46, 47, 2, ednBase, endRefs_, endTexts_);
  const Bytes atrd = read(4, 5, 30, atnBase, commentRefs_, commentTexts_);
  std::vector<std::u16string> owners;
  const Bytes grp = Table(36);
  for (size_t o = 0; o < grp.size() && owners.size() < 65536;) owners.push_back(Xst(grp, o));
  for (size_t k = 0; k < commentTexts_.size(); ++k) {
    size_t o = 30 * k;
    std::u16string initials = Xst(atrd, o);
    const size_t who = atrd.U16(30 * k + 20);
    commentBy_.push_back({who < owners.size() ? owners[who] : std::u16string(), initials.substr(0, 9)});
  }
  // Bookmarks: names, starts (with the index of their end) and ends.
  bookmarkNames_ = Sttb(Table(21));
  const Bytes bkf = Table(22), bkl = Table(23);
  const size_t n = bkf.size() >= 4 ? (bkf.size() - 4) / 8 : 0;
  const size_t ends = bkl.size() >= 4 ? bkl.size() / 4 - 1 : 0;
  for (size_t k = 0; k < n && k < bookmarkNames_.size(); ++k) {
    const size_t end = bkf.U16(4 * (n + 1) + 4 * k);
    const uint32_t a = bkf.U32(4 * k), b = end < ends ? bkl.U32(4 * end) : a;
    if (a > ccp_[0] || b > ccp_[0] || b < a || bookmarkNames_[k].empty()) continue;
    bookmarks_.insert({a, {true, static_cast<int>(k)}});
    bookmarks_.insert({b, {false, static_cast<int>(k)}});
  }
}

// ---------------------------------------------------------------------------
// Properties from sprms.

void Converter::ApplyChp(const Bytes& g, Rpr& r) const {
  ForEachSprm(g, [&](const Sprm& s) {
    const Bytes& a = s.arg;
    switch (s.op) {
      case 0x0835: Toggle(r.s[rB], "b", a.U8(0)); break;
      case 0x0836: Toggle(r.s[rI], "i", a.U8(0)); break;
      case 0x0837: Toggle(r.s[rStrike], "strike", a.U8(0)); break;
      case 0x0838: Toggle(r.s[rOutline], "outline", a.U8(0)); break;
      case 0x0839: Toggle(r.s[rShadow], "shadow", a.U8(0)); break;
      case 0x083A: Toggle(r.s[rSmallCaps], "smallCaps", a.U8(0)); break;
      case 0x083B: Toggle(r.s[rCaps], "caps", a.U8(0)); break;
      case 0x083C: Toggle(r.s[rVanish], "vanish", a.U8(0)); break;
      case 0x2A53: Toggle(r.s[rDStrike], "dstrike", a.U8(0)); break;
      case 0x0854: Toggle(r.s[rImprint], "imprint", a.U8(0)); break;
      case 0x0858: Toggle(r.s[rEmboss], "emboss", a.U8(0)); break;
      case 0x085C: Toggle(r.s[rBCs], "bCs", a.U8(0)); break;
      case 0x085D: Toggle(r.s[rICs], "iCs", a.U8(0)); break;
      case 0x0875: OnOff(r.s[rNoProof], "noProof", a.U8(0)); break;
      case 0x0811: OnOff(r.s[rWebHidden], "webHidden", a.U8(0)); break;
      case 0x0818: OnOff(r.s[rSpecVanish], "specVanish", a.U8(0)); break;
      case 0x085A: OnOff(r.s[rRtl], "rtl", a.U8(0)); break;
      case 0x0882: OnOff(r.s[rCs], "cs", a.U8(0)); break;
      case 0x2A3E: r.s[rU] = El("u", Underline(a.U8(0))); break;
      case 0x2A42: r.s[rColor] = El("color", IcoColor(a.U8(0))); break;  // sprmCCv, after it, is exact
      case 0x6870: r.s[rColor] = El("color", CvColor(a.U32(0))); break;
      case 0x4A43: r.s[rSz] = El("sz", a.U16(0)); break;
      case 0x4A61: r.s[rSzCs] = El("szCs", a.U16(0)); break;
      case 0x2A48: {
        const uint8_t v = a.U8(0);
        r.s[rVertAlign] = El("vertAlign", v == 1 ? "superscript" : v == 2 ? "subscript" : "baseline");
        break;
      }
      case 0x8840: r.s[rSpacing] = El("spacing", a.I16(0)); break;
      case 0x4852: r.s[rW] = El("w", a.U16(0)); break;
      case 0x484B: r.s[rKern] = El("kern", a.U16(0)); break;
      case 0x4845: r.s[rPosition] = El("position", a.I16(0)); break;
      case 0x2A0C: r.s[rHighlight] = El("highlight", a.U8(0) <= 16 ? kHighlight[a.U8(0)] : "none"); break;
      case 0x4A30: {
        const uint16_t istd = a.U16(0);
        r.s[rStyle] = istd == 10 || istd >= styles_.size() || !styles_[istd].defined ? "" : El("rStyle", StyleId(istd));
        r.cstyle = r.s[rStyle].empty() ? -1 : istd;
        break;
      }
      case 0x4A4F: case 0x4A50: case 0x4A51: case 0x4A5E: {
        const size_t slot = s.op == 0x4A4F ? 0 : s.op == 0x4A50 ? 1 : s.op == 0x4A51 ? 2 : 3;
        const uint16_t ftc = a.U16(0);
        r.font[slot] = ftc < fonts_.size() ? Esc(fonts_[ftc].name) : std::string();
        break;
      }
      case 0x486D: case 0x4873: r.lang[0] = Lang(a.U16(0)); break;
      case 0x486E: case 0x4874: r.lang[1] = Lang(a.U16(0)); break;
      case 0x485F: r.lang[2] = Lang(a.U16(0)); break;
      case 0x4866: r.s[rShd] = Shading(a, true); break;
      case 0xCA71: r.s[rShd] = Shading(a.Sub(1), false); break;
      case 0x6865: r.s[rBdr] = Border("bdr", a, true); break;
      case 0xCA72: r.s[rBdr] = Border("bdr", a.Sub(1), false); break;
      case 0x6A09: {
        const uint16_t ftc = a.U16(0);
        if (ftc < fonts_.size())
          r.sym = "<w:sym w:font=\"" + Esc(fonts_[ftc].name) + "\" w:char=\"" + Hex(a.U16(2), 4) + "\"/>";
        break;
      }
      case 0x0855: r.spec = a.U8(0) != 0; break;
      case 0x6A03: r.pic = a.U32(0); break;
      case 0x080A: r.ole = a.U8(0) != 0; break;
      case 0x0800: r.del = a.U8(0) != 0; break;
      case 0x0801: r.ins = a.U8(0) != 0; break;
      case 0x4804: r.ibst = a.U16(0); break;
      case 0x6805: r.dttm = a.U32(0); break;
      case 0x4863: r.ibstDel = a.U16(0); break;
      case 0x6864: r.dttmDel = a.U32(0); break;
      case 0x2A33:  // sprmCPlain: back to the paragraph style's
        for (size_t k = 1; k < r.s.size(); ++k) r.s[k].clear();
        r.font = {};
        r.lang = {};
        break;
      default: break;
    }
  });
}

void Converter::ApplyPap(const Bytes& g, Ppr& p, int depth) const {
  auto tabs = [&](const Bytes& a, bool close) {
    size_t o = 1;
    const size_t del = a.U8(o++);
    for (size_t k = 0; k < del; ++k) p.tabs[a.I16(o + 2 * k)] = "<w:tab w:val=\"clear\" w:pos=\"" + Num(a.I16(o + 2 * k)) + "\"/>";
    o += (close ? 4 : 2) * del;
    const size_t add = a.U8(o++);
    for (size_t k = 0; k < add; ++k) {
      static const char* const kJc[] = {"left", "center", "right", "decimal", "bar", "left", "num", "left"};
      static const char* const kLeader[] = {"none", "dot", "hyphen", "underscore", "heavy", "middleDot", "none", "none"};
      const int pos = a.I16(o + 2 * k);
      const uint8_t tbd = a.U8(o + 2 * add + k);
      p.tabs[pos] = std::string("<w:tab w:val=\"") + kJc[tbd & 7] + "\"" +
                    ((tbd >> 3) & 7 ? std::string(" w:leader=\"") + kLeader[(tbd >> 3) & 7] + "\"" : "") + " w:pos=\"" +
                    Num(pos) + "\"/>";
    }
  };
  ForEachSprm(g, [&](const Sprm& s) {
    const Bytes& a = s.arg;
    switch (s.op) {
      case 0x4600: p.istd = a.U16(0); break;
      case 0x2403: case 0x2461: p.s[pJc] = El("jc", Justification(a.U8(0))); break;
      case 0x2405: OnOff(p.s[pKeepLines], "keepLines", a.U8(0)); break;
      case 0x2406: OnOff(p.s[pKeepNext], "keepNext", a.U8(0)); break;
      case 0x2407: OnOff(p.s[pPageBreak], "pageBreakBefore", a.U8(0)); break;
      case 0x240C: OnOff(p.s[pNoLnn], "suppressLineNumbers", a.U8(0)); break;
      case 0x2431: OnOff(p.s[pWidow], "widowControl", a.U8(0)); break;
      case 0x242A: OnOff(p.s[pNoHyph], "suppressAutoHyphens", a.U8(0)); break;
      case 0x2433: OnOff(p.s[pKinsoku], "kinsoku", a.U8(0)); break;
      case 0x2434: OnOff(p.s[pWordWrap], "wordWrap", a.U8(0)); break;
      case 0x2435: OnOff(p.s[pOverflow], "overflowPunct", a.U8(0)); break;
      case 0x2436: OnOff(p.s[pTopLine], "topLinePunct", a.U8(0)); break;
      case 0x2437: OnOff(p.s[pAutoDE], "autoSpaceDE", a.U8(0)); break;
      case 0x2438: OnOff(p.s[pAutoDN], "autoSpaceDN", a.U8(0)); break;
      case 0x2441: OnOff(p.s[pBidi], "bidi", a.U8(0)); break;
      case 0x2448: OnOff(p.s[pAdjustRight], "adjustRightInd", a.U8(0)); break;
      case 0x246D: OnOff(p.s[pContextual], "contextualSpacing", a.U8(0)); break;
      case 0x2640: p.s[pOutline] = a.U8(0) < 9 ? El("outlineLvl", a.U8(0)) : ""; break;
      case 0x260A: p.ilvl = std::min<int>(a.U8(0), 8); break;
      case 0x460B: p.ilfo = a.U16(0); break;
      case 0xC60D: tabs(a, false); break;
      case 0xC615: tabs(a, true); break;
      case 0x840E: case 0x845D: p.right = a.I16(0); break;
      case 0x840F: case 0x845E: p.left = a.I16(0); break;
      case 0x8411: case 0x8460: p.first = a.I16(0); break;
      case 0x6412: {
        const int line = a.I16(0);
        if (a.I16(2)) p.line = line, p.lineRule = "auto";
        else if (line < 0) p.line = -line, p.lineRule = "exact";
        else if (line > 0) p.line = line, p.lineRule = "atLeast";
        else p.line = 240, p.lineRule = "auto";
        break;
      }
      case 0xA413: p.before = a.U16(0); break;
      case 0xA414: p.after = a.U16(0); break;
      case 0x245B: p.beforeAuto = a.U8(0) != 0; break;
      case 0x245C: p.afterAuto = a.U8(0) != 0; break;
      case 0x2416: p.inTable = a.U8(0) != 0; break;
      case 0x2417: p.ttp = a.U8(0) != 0; break;
      case 0x6649: p.itap = std::clamp(a.I32(0), 0, kMaxDepth); break;
      case 0x664A: p.itap = std::clamp(p.itap + a.I32(0), 0, kMaxDepth); break;
      case 0x244B: p.innerCell = a.U8(0) != 0; break;
      case 0x244C: p.innerTtp = a.U8(0) != 0; break;
      case 0x6424: case 0x6425: case 0x6426: case 0x6427: case 0x6428: case 0x6629: {
        static const char* const k[] = {"top", "left", "bottom", "right", "between", "bar"};
        const size_t side = s.op == 0x6629 ? 5 : static_cast<size_t>(s.op - 0x6424);
        p.bdr[side] = Border(k[side], a, true);
        break;
      }
      case 0xC64E: case 0xC64F: case 0xC650: case 0xC651: case 0xC652: case 0xC653: {
        static const char* const k[] = {"top", "left", "bottom", "right", "between", "bar"};
        const size_t side = static_cast<size_t>(s.op - 0xC64E);
        p.bdr[side] = Border(k[side], a.Sub(1), false);
        break;
      }
      case 0x261B: p.pc = a.U8(0); break;
      case 0x8418: p.frameX = a.I16(0); break;
      case 0x8419: p.frameY = a.I16(0); break;
      case 0x841A: p.frameW = a.U16(0); break;
      case 0x442B: p.frameH = a.U16(0); break;
      case 0x2423: p.wrap = a.U8(0); break;
      case 0x842F: p.hSpace = a.I16(0); break;
      case 0x842E: p.vSpace = a.I16(0); break;
      case 0x442C: p.dropCap = a.U16(0); break;
      case 0x442D: p.s[pShd] = Shading(a, true); break;
      case 0xC64D: p.s[pShd] = Shading(a.Sub(1), false); break;
      case 0x6646:  // sprmPHugePapx: the rest of the paragraph's sprms, in the Data stream
        if (depth == 0) {
          const Bytes huge = data_.Sub(a.U32(0));
          const Bytes more = huge.Sub(2, huge.U16(0));
          ApplyPap(more, p, 1);
          p.table.push_back(more);
        }
        break;
      default: break;
    }
  });
  if (depth == 0) p.table.push_back(g);
}

void Converter::ApplySep(const Bytes& g, Sep& s) const {
  ForEachSprm(g, [&](const Sprm& sp) {
    const Bytes& a = sp.arg;
    switch (sp.op) {
      case 0x3009: s.bkc = a.U8(0); break;
      case 0x300A: s.titlePg = a.U8(0) != 0; break;
      case 0x500B: s.cols = std::clamp(a.U16(0) + 1, 1, 45); break;
      case 0x900C: s.colSpace = a.I16(0); break;
      case 0x300E: s.nfc = a.U8(0); break;
      case 0x3011: s.restart = a.U8(0) != 0; break;
      case 0x501C: s.start = a.U16(0); break;
      case 0xB01F: s.xa = a.U16(0); break;
      case 0xB020: s.ya = a.U16(0); break;
      case 0xB021: s.left = a.U16(0); break;
      case 0xB022: s.right = a.U16(0); break;
      case 0x9023: s.top = a.I16(0); break;
      case 0x9024: s.bottom = a.I16(0); break;
      case 0xB025: s.gutter = a.U16(0); break;
      case 0xB017: s.header = a.U16(0); break;
      case 0xB018: s.footer = a.U16(0); break;
      case 0x3019: s.lineBetween = a.U8(0) != 0; break;
      case 0x301A: s.vjc = a.U8(0); break;
      case 0x5015: s.lnnMod = a.U16(0); break;
      case 0x9016: s.dxaLnn = a.I16(0); break;
      case 0x3013: s.lnc = a.U8(0); break;
      case 0x501B: s.lnnMin = a.U16(0); break;
      case 0x9031: s.pitch = a.I16(0); break;
      case 0x5032: s.clm = a.U16(0); break;
      default: break;
    }
  });
}

void Converter::ApplyTap(const Bytes& g, Tap& t) const {
  ForEachSprm(g, [&](const Sprm& s) {
    const Bytes& a = s.arg;
    switch (s.op) {
      case 0xD608: case 0xD606: {
        const size_t n = std::min<size_t>(a.U8(2), 63);
        t.centers.clear();
        t.tc.clear();
        for (size_t k = 0; k <= n; ++k) t.centers.push_back(a.I16(3 + 2 * k));
        const Bytes tcs = a.Sub(3 + 2 * (n + 1));
        for (size_t k = 0; k < n; ++k) {
          Tc tc;
          if (tcs.Has(20 * k, 20)) {
            const Bytes c = tcs.Sub(20 * k, 20);
            tc.flags = c.U16(0);
            static const char* const kSide[] = {"top", "left", "bottom", "right"};
            for (size_t b = 0; b < 4; ++b) tc.brc[b] = Border(kSide[b], c.Sub(4 + 4 * b, 4), true);
          }
          t.tc.push_back(std::move(tc));
        }
        break;
      }
      case 0x9602: t.gapHalf = a.I16(0); break;
      case 0x360D: t.pc = a.U8(0); break;
      case 0x940E: t.x = a.I16(0); break;
      case 0x940F: t.y = a.I16(0); break;
      case 0x9410: t.fromText[0] = a.I16(0); break;
      case 0x9411: t.fromText[1] = a.I16(0); break;
      case 0x941B: t.fromText[2] = a.I16(0); break;
      case 0x941C: t.fromText[3] = a.I16(0); break;
      case 0x9407: t.height = a.I16(0); break;
      case 0x3404: t.header = a.U8(0) != 0; break;
      case 0x3403: case 0x3466: t.cantSplit = a.U8(0) != 0; break;
      case 0x5400: case 0x548A: t.jc = a.U16(0); break;
      case 0x3615: t.autofit = a.U8(0) != 0; break;
      case 0x563A: t.istd = a.U16(0); break;
      case 0xD605: case 0xD613: {
        static const char* const k[] = {"top", "left", "bottom", "right", "insideH", "insideV"};
        const bool old = s.op == 0xD605;
        for (size_t b = 0; b < 6; ++b) t.borders[b] = Border(k[b], a.Sub(1 + (old ? 4 : 8) * b, old ? 4 : 8), old);
        break;
      }
      case 0xD609: case 0xD612: {
        const bool old = s.op == 0xD609;
        const size_t size = old ? 2 : 10, n = (a.size() - 1) / size;
        for (size_t k = 0; k < n && k < t.tc.size(); ++k) t.tc[k].shd = Shading(a.Sub(1 + size * k, size), old);
        break;
      }
      case 0xD634: {
        const int width = a.U16(5);
        for (size_t b = 0; b < 4; ++b)
          if (a.U8(3) & (1 << b)) t.padding[b] = width;
        break;
      }
      case 0xD632: {  // sprmTCellPadding: cells itcFirst to itcLim, the sides in grfbrc, the width (ftsWidth 3: twips; 0: none)
        const int width = a.U8(4) == 3 ? a.U16(5) : 0;
        for (size_t k = a.U8(1); k < a.U8(2) && k < t.tc.size(); ++k)
          for (size_t b = 0; b < 4; ++b)
            if (a.U8(3) & (1 << b)) t.tc[k].padding[b] = width;
        break;
      }
      case 0x7629: {  // sprmTTextFlow: itcFirst, itcLim, then the cells' text flow
        const size_t first = a.U8(0), lim = a.U8(1);
        for (size_t k = first; k < lim && k < t.tc.size(); ++k)
          t.tc[k].flags = static_cast<uint16_t>((t.tc[k].flags & ~0x1C) | ((a.U16(2) & 7) << 2));
        break;
      }
      case 0xD62C: {
        const size_t first = a.U8(1), lim = a.U8(2);
        for (size_t k = first; k < lim && k < t.tc.size(); ++k)
          t.tc[k].flags = static_cast<uint16_t>((t.tc[k].flags & ~0x180) | ((a.U8(3) & 3) << 7));
        break;
      }
      default: break;
    }
  });
}

Rpr Converter::ChpAt(uint32_t cp) const {
  Rpr r;
  if (const Piece* p = PieceAt(cp)) {
    if (const Run* run = Find(chpx_, FcOf(*p, cp))) ApplyChp(run->grpprl, r);
    ApplyChp(PrmGrpprl(p->prm), r);
  }
  return r;
}

Ppr Converter::PapAt(uint32_t markCp) const {
  Ppr pp;
  if (const Piece* p = PieceAt(markCp)) {
    if (const Run* run = Find(papx_, FcOf(*p, markCp))) {
      pp.istd = run->istd;
      ApplyPap(run->grpprl, pp);
    }
    ApplyPap(PrmGrpprl(p->prm), pp);
  }
  return pp;
}

std::string Converter::RevisionAttrs(uint16_t ibst, uint32_t dttm) {
  std::string s = " w:id=\"" + Num(revision_++) + "\" w:author=\"" +
                  (ibst < revisionAuthors_.size() ? Esc(revisionAuthors_[ibst]) : std::string("Unknown")) + "\"";
  if (const std::string d = Date(dttm); !d.empty()) s += " w:date=\"" + d + "\"";
  return s;
}

std::string Converter::RprXml(const Rpr& r, bool mark) {
  std::string out;
  if (mark) {
    if (r.ins) out += "<w:ins" + RevisionAttrs(r.ibst, r.dttm) + "/>";
    if (r.del) out += "<w:del" + RevisionAttrs(r.ibstDel ? r.ibstDel : r.ibst, r.dttmDel ? r.dttmDel : r.dttm) + "/>";
  }
  for (size_t k = 0; k < r.s.size(); ++k) {
    if (k == rFonts && (!r.font[0].empty() || !r.font[1].empty() || !r.font[2].empty() || !r.font[3].empty())) {
      out += "<w:rFonts";
      static const char* const kAttr[] = {" w:ascii=\"", " w:eastAsia=\"", " w:hAnsi=\"", " w:cs=\""};
      for (size_t f = 0; f < 4; ++f)
        if (!r.font[f].empty()) out += kAttr[f] + r.font[f] + "\"";
      out += "/>";
    } else if (k == rLang && (!r.lang[0].empty() || !r.lang[1].empty() || !r.lang[2].empty())) {
      out += "<w:lang";
      static const char* const kAttr[] = {" w:val=\"", " w:eastAsia=\"", " w:bidi=\""};
      for (size_t f = 0; f < 3; ++f)
        if (!r.lang[f].empty()) out += kAttr[f] + r.lang[f] + "\"";
      out += "/>";
    } else if (!r.s[k].empty() && r.s[k][0] == '\x81') {
      // The style's opposite: the paragraph's and the character's styles, toggled as Word does. In a style,
      // where this is the base style's opposite, w:on toggles it already.
      const std::string name = r.s[k].substr(1);
      bool on = true;
      if (paraStyle_ >= 0)
        for (const auto& [n, op] : kToggleSprms)
          if (n == name) on = StyleToggle(paraStyle_, op) == StyleToggle(r.cstyle, op);
      out += on ? "<w:" + name + "/>" : "<w:" + name + " w:val=\"0\"/>";
    } else {
      out += r.s[k];
    }
  }
  return out.empty() ? out : "<w:rPr>" + out + "</w:rPr>";
}

bool Converter::StyleToggle(int istd, uint16_t op) const {
  std::vector<size_t> chain;
  for (size_t i = static_cast<size_t>(istd); istd >= 0 && i < styles_.size() && chain.size() < 16;
       i = static_cast<size_t>(styles_[i].base))
    chain.push_back(i);
  bool on = false;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    ForEachSprm(styles_[*it].chpx, [&](const Sprm& sp) {
      if (sp.op != op) return;
      const uint8_t v = sp.arg.U8(0);
      on = v == 0 ? false : v == 1 ? true : v == 0x81 ? !on : on;
    });
  return on;
}

std::string Converter::PprXml(const Ppr& p, const std::string& rPr, const std::string& sectPr) const {
  std::string out;
  for (size_t k = 0; k < p.s.size(); ++k) {
    switch (k) {
      case pStyle:
        if (p.istd != 0 && p.istd < styles_.size() && styles_[p.istd].defined && styles_[p.istd].type == 1)
          out += El("pStyle", StyleId(p.istd));
        break;
      case pFramePr:
        if (p.pc || p.frameX || p.frameY || p.frameW || p.frameH || p.wrap || p.dropCap) {
          out += "<w:framePr";
          if (p.dropCap && (*p.dropCap & 7)) {
            out += std::string(" w:dropCap=\"") + ((*p.dropCap & 7) == 2 ? "margin" : "drop") + "\" w:lines=\"" +
                   Num(std::max(1, (*p.dropCap >> 3) & 0x1F)) + "\"";
          }
          if (p.frameW && *p.frameW) out += " w:w=\"" + Num(*p.frameW) + "\"";
          if (p.frameH && (*p.frameH & 0x7FFF)) {
            out += " w:h=\"" + Num(*p.frameH & 0x7FFF) + "\" w:hRule=\"" + ((*p.frameH & 0x8000) ? "atLeast" : "exact") + "\"";
          }
          if (p.vSpace) out += " w:vSpace=\"" + Num(*p.vSpace) + "\"";
          if (p.hSpace) out += " w:hSpace=\"" + Num(*p.hSpace) + "\"";
          static const char* const kWrap[] = {"auto", "notBeside", "around", "none", "tight", "through"};
          if (p.wrap && *p.wrap < 6) out += std::string(" w:wrap=\"") + kWrap[*p.wrap] + "\"";
          // Position codes: the vertical anchor in bits 4-5 (margin, page, text), the horizontal in
          // bits 6-7 (column, margin, page).
          const int pc = p.pc.value_or(0x20);
          static const char* const kV[] = {"margin", "page", "text", "text"};
          static const char* const kH[] = {"text", "margin", "page", "page"};
          out += std::string(" w:vAnchor=\"") + kV[(pc >> 4) & 3] + "\" w:hAnchor=\"" + kH[(pc >> 6) & 3] + "\"";
          const int x = p.frameX.value_or(0), y = p.frameY.value_or(0);
          static const char* const kX[] = {"left", "center", "right", "inside", "outside"};
          if (x <= 0 && x >= -16 && x % 4 == 0) out += std::string(" w:xAlign=\"") + kX[-x / 4] + "\"";
          else out += " w:x=\"" + Num(x) + "\"";
          static const char* const kY[] = {"inline", "top", "center", "bottom", "inside", "outside"};
          if (y < 0 && y >= -20 && y % 4 == 0) out += std::string(" w:yAlign=\"") + kY[-y / 4] + "\"";
          else out += " w:y=\"" + Num(y) + "\"";
          out += "/>";
        }
        break;
      case pNumPr:
        if (p.ilfo >= 0 || p.ilvl >= 0) {
          out += "<w:numPr>";
          if (p.ilvl >= 0) out += El("ilvl", p.ilvl);
          if (p.ilfo >= 0) out += El("numId", p.ilfo >= 0xF800 ? 0 : p.ilfo);
          out += "</w:numPr>";
        }
        break;
      case pBdr:
        if (std::any_of(p.bdr.begin(), p.bdr.end(), [](const std::string& b) { return !b.empty(); })) {
          out += "<w:pBdr>";
          for (const std::string& b : p.bdr) out += b;
          out += "</w:pBdr>";
        }
        break;
      case pTabs:
        if (!p.tabs.empty()) {
          out += "<w:tabs>";
          for (const auto& [pos, xml] : p.tabs) out += xml;
          out += "</w:tabs>";
        }
        break;
      case pSpacing:
        if (p.before || p.after || p.line || p.beforeAuto || p.afterAuto) {
          out += "<w:spacing";
          if (p.before) out += " w:before=\"" + Num(*p.before) + "\"";
          if (p.beforeAuto) out += std::string(" w:beforeAutospacing=\"") + (*p.beforeAuto ? "1" : "0") + "\"";
          if (p.after) out += " w:after=\"" + Num(*p.after) + "\"";
          if (p.afterAuto) out += std::string(" w:afterAutospacing=\"") + (*p.afterAuto ? "1" : "0") + "\"";
          if (p.line) out += " w:line=\"" + Num(*p.line) + "\" w:lineRule=\"" + p.lineRule + "\"";
          out += "/>";
        }
        break;
      case pInd:
        if (p.left || p.right || p.first) {
          out += "<w:ind";
          if (p.left) out += " w:left=\"" + Num(*p.left) + "\"";
          if (p.right) out += " w:right=\"" + Num(*p.right) + "\"";
          if (p.first) out += *p.first < 0 ? " w:hanging=\"" + Num(-*p.first) + "\"" : " w:firstLine=\"" + Num(*p.first) + "\"";
          out += "/>";
        }
        break;
      default: out += p.s[k]; break;
    }
  }
  out += rPr + sectPr;
  return out.empty() ? out : "<w:pPr>" + out + "</w:pPr>";
}

std::string Converter::SepXml(size_t section) {
  const Sep& s = sections_[section].second;
  std::string out = "<w:sectPr>";
  // Headers and footers: six stories per section after the six separators; an empty one is the
  // previous section's.
  static const char* const kKind[] = {"header", "header", "footer", "footer", "header", "footer"};
  static const char* const kType[] = {"even", "default", "even", "default", "first", "first"};
  const uint32_t base = ccp_[0] + ccp_[1];
  for (size_t k = 0; k < 6; ++k) {
    const size_t j = 6 + 6 * section + k;
    if (j + 1 >= hdd_.size() || hdd_[j + 1] <= hdd_[j]) continue;
    const uint32_t cp0 = base + hdd_[j];
    uint32_t cp1 = std::min<uint32_t>(base + hdd_[j + 1], base + ccp_[2]);
    if (cp1 <= cp0 || cp1 > text_.size()) continue;
    // Each story ends with a paragraph mark of its own, then one that isn't part of it.
    if (cp1 - cp0 >= 2 && text_[cp1 - 1] == 0x0D && text_[cp1 - 2] == 0x0D) --cp1;
    PartOut part{"word/" + std::string(kKind[k]) + Num(static_cast<int64_t>(parts_.size() + 1)) + ".xml"};
    Story story{StoryKind::Header, &part, {}};
    const std::string root = k == 2 || k == 3 || k == 5 ? "w:ftr" : "w:hdr";
    std::string xml = "<" + root + kNs + ">" + EmitStory(cp0, cp1, story) + "</" + root + ">";
    part.content = std::move(xml);
    const std::string id = document_.Add(kKind[k], part.name.substr(5));
    out += std::string("<w:") + kKind[k] + "Reference w:type=\"" + kType[k] + "\" r:id=\"" + id + "\"/>";
    parts_.push_back(std::move(part));
  }
  static const char* const kBreak[] = {"continuous", "nextColumn", "nextPage", "evenPage", "oddPage"};
  if (s.bkc != 2 && s.bkc < 5) out += El("type", kBreak[s.bkc]);
  out += "<w:pgSz w:w=\"" + Num(s.xa) + "\" w:h=\"" + Num(s.ya) + "\"" + (s.xa > s.ya ? " w:orient=\"landscape\"" : "") + "/>";
  out += "<w:pgMar w:top=\"" + Num(s.top) + "\" w:right=\"" + Num(s.right) + "\" w:bottom=\"" + Num(s.bottom) +
         "\" w:left=\"" + Num(s.left) + "\" w:header=\"" + Num(s.header) + "\" w:footer=\"" + Num(s.footer) +
         "\" w:gutter=\"" + Num(s.gutter) + "\"/>";
  if (s.lnnMod > 0) {
    static const char* const kRestart[] = {"newPage", "newSection", "continuous"};
    out += "<w:lnNumType w:countBy=\"" + Num(s.lnnMod) + "\"" + (s.lnnMin ? " w:start=\"" + Num(s.lnnMin) + "\"" : "") +
           (s.dxaLnn ? " w:distance=\"" + Num(s.dxaLnn) + "\"" : "") + " w:restart=\"" + kRestart[std::min(s.lnc, 2)] + "\"/>";
  }
  if (s.nfc != 0 || s.restart)
    out += std::string("<w:pgNumType") + (s.nfc ? " w:fmt=\"" + std::string(NumberFormat(static_cast<unsigned>(s.nfc))) + "\"" : "") +
           (s.restart ? " w:start=\"" + Num(s.start) + "\"" : "") + "/>";
  out += "<w:cols w:space=\"" + Num(s.colSpace) + "\"" + (s.cols > 1 ? " w:num=\"" + Num(s.cols) + "\"" : "") +
         (s.lineBetween ? " w:sep=\"1\"" : "") + "/>";
  static const char* const kVAlign[] = {"top", "center", "both", "bottom"};
  if (s.vjc > 0 && s.vjc < 4) out += El("vAlign", kVAlign[s.vjc]);
  if (s.titlePg) out += "<w:titlePg/>";
  static const char* const kGrid[] = {"default", "linesAndChars", "lines", "snapToChars"};
  if (s.pitch > 0) out += "<w:docGrid w:type=\"" + std::string(kGrid[std::clamp(s.clm, 0, 3)]) + "\" w:linePitch=\"" + Num(s.pitch) + "\"/>";
  return out + "</w:sectPr>";
}

// ---------------------------------------------------------------------------
// Stories.

std::string Converter::EmitStory(uint32_t cp0, uint32_t cp1, Story& story, bool body) {
  cp1 = std::min<uint32_t>(cp1, static_cast<uint32_t>(text_.size()));
  std::vector<Para> paras;
  uint32_t start = cp0;
  auto add = [&](uint32_t mark) {
    Para p{start, mark, mark < cp1 ? PapAt(mark) : (mark > cp0 ? PapAt(mark - 1) : Ppr())};
    p.depth = std::max(p.ppr.itap, p.ppr.inTable ? 1 : 0);
    const char16_t c = mark < cp1 ? text_[mark] : 0;
    if (p.depth == 1) {
      p.rowEnd = p.ppr.ttp;
      p.cellEnd = c == 0x07 && !p.rowEnd;
    } else if (p.depth > 1) {
      p.rowEnd = p.ppr.innerTtp;
      p.cellEnd = p.ppr.innerCell && !p.rowEnd;
    }
    paras.push_back(std::move(p));
  };
  for (uint32_t cp = cp0; cp < cp1; ++cp) {
    const char16_t c = text_[cp];
    if (c == 0x0D || c == 0x07 || (c == 0x0C && story.kind == StoryKind::Main && sectionEnds_.count(cp))) {
      add(cp);
      start = cp + 1;
    }
  }
  if (start < cp1) add(cp1);
  std::string out;
  EmitBlocks(paras, 0, paras.size(), 0, story, out);
  if (paras.empty()) out += "<w:p/>";
  // What the story left open is closed: fields, and (in the body) bookmarks.
  if (!story.fields.empty()) {
    std::string runs;
    for (; !story.fields.empty(); story.fields.pop_back()) runs += "<w:r><w:fldChar w:fldCharType=\"end\"/></w:r>";
    out += "<w:p>" + runs + "</w:p>";
  }
  if (body) {
    std::string ends;
    for (int id : bookmarksOpen_)
      if (!bookmarksClosed_.count(id)) ends += "<w:bookmarkEnd w:id=\"" + Num(id) + "\"/>";
    if (!ends.empty()) out += "<w:p>" + ends + "</w:p>";
  }
  return out;
}

void Converter::EmitBlocks(const std::vector<Para>& paras, size_t i0, size_t i1, int depth, Story& story, std::string& out) {
  for (size_t i = i0; i < i1;) {
    if (paras[i].depth > depth && depth < kMaxDepth) {
      size_t j = i;
      while (j < i1 && paras[j].depth > depth) ++j;
      EmitTable(paras, i, j, depth + 1, story, out);
      i = j;
    } else {
      EmitPara(paras[i], story, out);
      ++i;
    }
  }
}

void Converter::EmitTable(const std::vector<Para>& paras, size_t i0, size_t i1, int depth, Story& story, std::string& out) {
  struct Row {
    std::vector<std::pair<size_t, size_t>> cells;  // paragraph ranges
    Tap tap;
  };
  std::vector<Row> rows;
  Row row;
  size_t cellStart = i0;
  for (size_t i = i0; i < i1; ++i) {
    const Para& p = paras[i];
    if (p.depth != depth) continue;
    if (p.rowEnd) {
      if (cellStart < i) row.cells.push_back({cellStart, i});
      for (const Bytes& g : p.ppr.table) ApplyTap(g, row.tap);
      rows.push_back(std::move(row));
      row = Row();
      cellStart = i + 1;
    } else if (p.cellEnd) {
      row.cells.push_back({cellStart, i + 1});
      cellStart = i + 1;
    }
  }
  if (cellStart < i1) row.cells.push_back({cellStart, i1});
  if (!row.cells.empty()) rows.push_back(std::move(row));
  if (rows.empty()) return;

  // The grid: every row's cell edges.
  std::set<int> edges;
  for (const Row& r : rows)
    for (int c : r.tap.centers) edges.insert(c);
  std::vector<int> grid(edges.begin(), edges.end());
  const Tap& first = rows.front().tap;
  out += "<w:tbl><w:tblPr>";
  if (first.istd > 0 && static_cast<size_t>(first.istd) < styles_.size() && styles_[static_cast<size_t>(first.istd)].type == 3)
    out += El("tblStyle", StyleId(static_cast<size_t>(first.istd)));
  if (first.pc || first.x || first.y) {
    const int pc = first.pc.value_or(0x20), x = first.x.value_or(0), y = first.y.value_or(0);
    static const char* const kV[] = {"margin", "page", "text", "text"};
    static const char* const kH[] = {"text", "margin", "page", "page"};
    out += "<w:tblpPr w:leftFromText=\"" + Num(first.fromText[0]) + "\" w:rightFromText=\"" + Num(first.fromText[2]) +
           "\" w:topFromText=\"" + Num(first.fromText[1]) + "\" w:bottomFromText=\"" + Num(first.fromText[3]) +
           "\" w:vertAnchor=\"" + kV[(pc >> 4) & 3] + "\" w:horzAnchor=\"" + kH[(pc >> 6) & 3] + "\"";
    static const char* const kX[] = {"left", "center", "right", "inside", "outside"};
    if (x < 0 && x >= -16 && x % 4 == 0) out += std::string(" w:tblpXSpec=\"") + kX[-x / 4] + "\"";
    else out += " w:tblpX=\"" + Num(x) + "\"";
    static const char* const kY[] = {"inline", "top", "center", "bottom", "inside", "outside"};
    if (y < 0 && y >= -20 && y % 4 == 0) out += std::string(" w:tblpYSpec=\"") + kY[-y / 4] + "\"";
    else out += " w:tblpY=\"" + Num(y) + "\"";
    out += "/>";
  }
  out += "<w:tblW w:w=\"0\" w:type=\"auto\"/>";
  if (first.jc == 1 || first.jc == 2) out += El("jc", first.jc == 1 ? "center" : "right");
  out += "<w:tblInd w:w=\"" + Num((grid.empty() ? 0 : grid.front()) + first.gapHalf) + "\" w:type=\"dxa\"/>";
  if (std::any_of(first.borders.begin(), first.borders.end(), [](const std::string& b) { return !b.empty(); })) {
    out += "<w:tblBorders>";
    for (const std::string& b : first.borders) out += b;
    out += "</w:tblBorders>";
  }
  if (!first.autofit) out += "<w:tblLayout w:type=\"fixed\"/>";
  {
    static const char* const kSide[] = {"top", "left", "bottom", "right"};
    out += "<w:tblCellMar>";
    for (size_t b = 0; b < 4; ++b) {
      const int w = first.padding[b] >= 0 ? first.padding[b] : (b == 1 || b == 3) ? first.gapHalf : 0;
      out += std::string("<w:") + kSide[b] + " w:w=\"" + Num(w) + "\" w:type=\"dxa\"/>";
    }
    out += "</w:tblCellMar>";
  }
  out += "<w:tblLook w:val=\"0000\"/></w:tblPr><w:tblGrid>";
  for (size_t g = 0; g + 1 < grid.size(); ++g) out += "<w:gridCol w:w=\"" + Num(grid[g + 1] - grid[g]) + "\"/>";
  if (grid.size() < 2) out += "<w:gridCol w:w=\"2000\"/>";
  out += "</w:tblGrid>";
  for (const Row& r : rows) {
    out += "<w:tr><w:trPr>";
    // A row that starts after the grid's left edge, or ends before its right one, skips those columns.
    if (!r.tap.centers.empty() && grid.size() >= 2) {
      const auto before = std::lower_bound(grid.begin(), grid.end(), r.tap.centers.front()) - grid.begin();
      const auto last = std::lower_bound(grid.begin(), grid.end(), r.tap.centers.back()) - grid.begin();
      const auto after = static_cast<std::ptrdiff_t>(grid.size()) - 1 - last;
      if (before > 0)
        out += El("gridBefore", before) + "<w:wBefore w:w=\"" + Num(r.tap.centers.front() - grid.front()) + "\" w:type=\"dxa\"/>";
      if (after > 0)
        out += El("gridAfter", after) + "<w:wAfter w:w=\"" + Num(grid.back() - r.tap.centers.back()) + "\" w:type=\"dxa\"/>";
    }
    if (r.tap.cantSplit) out += "<w:cantSplit/>";
    if (r.tap.height)
      out += "<w:trHeight w:val=\"" + Num(std::abs(r.tap.height)) + "\" w:hRule=\"" + (r.tap.height < 0 ? "exact" : "atLeast") + "\"/>";
    if (r.tap.header) out += "<w:tblHeader/>";
    out += "</w:trPr>";
    for (size_t c = 0; c < r.cells.size(); ++c) {
      const Tc tc = c < r.tap.tc.size() ? r.tap.tc[c] : Tc();
      const unsigned horz = tc.flags & 3;
      if (horz >= 2) continue;  // merged into the cell before it (the old way)
      size_t lastCell = c;
      while (lastCell + 1 < r.cells.size() && lastCell + 1 < r.tap.tc.size() && (r.tap.tc[lastCell + 1].flags & 3) >= 2) ++lastCell;
      int x0 = 0, x1 = 0, span = 1;
      if (lastCell + 1 < r.tap.centers.size()) {
        x0 = r.tap.centers[c], x1 = r.tap.centers[lastCell + 1];
        span = static_cast<int>(std::count_if(grid.begin(), grid.end(), [&](int e) { return e > x0 && e <= x1; }));
      }
      out += "<w:tc><w:tcPr><w:tcW w:w=\"" + Num(std::max(0, x1 - x0)) + "\" w:type=\"dxa\"/>";
      if (span > 1) out += El("gridSpan", span);
      const unsigned vert = (tc.flags >> 5) & 3;
      if (vert == 3) out += "<w:vMerge w:val=\"restart\"/>";
      else if (vert == 1) out += "<w:vMerge/>";
      if (std::any_of(tc.brc.begin(), tc.brc.end(), [](const std::string& b) { return !b.empty(); })) {
        out += "<w:tcBorders>";
        for (const std::string& b : tc.brc) out += b;
        out += "</w:tcBorders>";
      }
      out += tc.shd;
      if (tc.flags & 0x2000) out += "<w:noWrap/>";
      if (std::any_of(tc.padding.begin(), tc.padding.end(), [](int w) { return w >= 0; })) {
        static const char* const kSide[] = {"top", "left", "bottom", "right"};
        out += "<w:tcMar>";
        for (size_t b = 0; b < 4; ++b)
          if (tc.padding[b] >= 0) out += std::string("<w:") + kSide[b] + " w:w=\"" + Num(tc.padding[b]) + "\" w:type=\"dxa\"/>";
        out += "</w:tcMar>";
      }
      // TCGRF.textFlow: vertical text (a header turned along the page's side, for one).
      switch ((tc.flags >> 2) & 7) {
        case 1: out += El("textDirection", "tbRl"); break;
        case 3: out += El("textDirection", "btLr"); break;
        case 4: out += El("textDirection", "lrTbV"); break;
        case 5: out += El("textDirection", "tbRlV"); break;
        default: break;
      }
      const unsigned va = (tc.flags >> 7) & 3;
      if (va == 1 || va == 2) out += El("vAlign", va == 1 ? "center" : "bottom");
      out += "</w:tcPr>";
      const size_t before = out.size();
      EmitBlocks(paras, r.cells[c].first, r.cells[lastCell].second, depth, story, out);
      // A cell ends with a paragraph (a nested table can't be its last block).
      if (out.size() == before || out.compare(out.size() - 8, 8, "</w:tbl>") == 0) out += "<w:p/>";
      out += "</w:tc>";
    }
    out += "</w:tr>";
  }
  out += "</w:tbl>";
}

void Converter::EmitPara(const Para& p, Story& story, std::string& out) {
  if (p.rowEnd) return;  // the row's end: its properties went into the row
  std::string sectPr;
  if (story.kind == StoryKind::Main && p.depth == 0)
    if (auto it = sectionEnds_.find(p.mark); it != sectionEnds_.end()) sectPr = SepXml(it->second);
  const int outerStyle = paraStyle_;  // a text box's paragraphs are written inside another's runs
  paraStyle_ = p.ppr.istd;
  const std::string mark = p.mark < text_.size() ? RprXml(ChpAt(p.mark), true) : std::string();
  out += "<w:p>" + PprXml(p.ppr, mark, sectPr);
  EmitRuns(p.cp0, p.mark, story, out);
  paraStyle_ = outerStyle;
  if (story.kind == StoryKind::Main) EmitMarks(p.mark, out);
  out += "</w:p>";
}

void Converter::EmitMarks(uint32_t cp, std::string& out) {
  auto [a, b] = bookmarks_.equal_range(cp);
  for (auto it = a; it != b; ++it) {
    const auto [start, id] = it->second;
    if (!start || bookmarksOpen_.count(id)) continue;
    bookmarksOpen_.insert(id);
    out += "<w:bookmarkStart w:id=\"" + Num(id) + "\" w:name=\"" + Esc(bookmarkNames_[static_cast<size_t>(id)]) + "\"/>";
  }
  for (auto it = a; it != b; ++it) {
    const auto [start, id] = it->second;
    if (start || !bookmarksOpen_.count(id) || !bookmarksClosed_.insert(id).second) continue;
    out += "<w:bookmarkEnd w:id=\"" + Num(id) + "\"/>";
  }
}

void Converter::EmitRuns(uint32_t cp0, uint32_t cp1, Story& story, std::string& out) {
  for (uint32_t cp = cp0; cp < cp1;) {
    const Piece* piece = PieceAt(cp);
    if (!piece) break;
    // A stretch with the same properties: within the piece, the CHPX run and up to a bookmark.
    uint32_t end = std::min(cp1, piece->cp1);
    const uint32_t fc = FcOf(*piece, cp);
    if (const Run* run = Find(chpx_, fc)) {
      const uint32_t step = piece->compressed ? 1 : 2;
      end = std::min<uint32_t>(end, cp + (run->fc1 - fc + step - 1) / step);
    }
    if (story.kind == StoryKind::Main) {
      if (auto it = bookmarks_.upper_bound(cp); it != bookmarks_.end()) end = std::min(end, it->first);
      EmitMarks(cp, out);
    }
    end = std::max(end, cp + 1);
    const Rpr r = ChpAt(cp);
    const std::string rPr = RprXml(r);
    const bool deleted = r.del;
    std::string wrapOpen, wrapClose;
    if (r.ins) wrapOpen = "<w:ins" + RevisionAttrs(r.ibst, r.dttm) + ">", wrapClose = "</w:ins>";
    if (r.del) {
      wrapOpen += "<w:del" + RevisionAttrs(r.ibstDel ? r.ibstDel : r.ibst, r.dttmDel ? r.dttmDel : r.dttm) + ">";
      wrapClose = "</w:del>" + wrapClose;
    }
    auto run = [&](const std::string& content) { out += wrapOpen + "<w:r>" + rPr + content + "</w:r>" + wrapClose; };
    std::u16string text;
    auto flush = [&]() {
      if (text.empty()) return;
      const bool code = !story.fields.empty() && story.fields.back().code;
      const char* tag = code ? (deleted ? "w:delInstrText" : "w:instrText") : (deleted ? "w:delText" : "w:t");
      run(std::string("<") + tag + " xml:space=\"preserve\">" + Esc(text) + "</" + tag + ">");
      text.clear();
    };
    for (uint32_t c = cp; c < end; ++c) {
      const char16_t ch = text_[c];
      const bool inCode = !story.fields.empty() && story.fields.back().code;
      switch (ch) {
        case 0x13:
          flush();
          story.fields.push_back(Field());
          run("<w:fldChar w:fldCharType=\"begin\"/>");
          continue;
        case 0x14:
          if (story.fields.empty() || !story.fields.back().code) continue;
          flush();
          story.fields.back().code = false;
          run("<w:fldChar w:fldCharType=\"separate\"/>");
          continue;
        case 0x15:
          if (story.fields.empty()) continue;
          flush();
          story.fields.pop_back();
          run("<w:fldChar w:fldCharType=\"end\"/>");
          continue;
        case 0x09: flush(); run("<w:tab/>"); continue;
        case 0x0B: flush(); run("<w:br/>"); continue;
        case 0x0C: flush(); run("<w:br w:type=\"page\"/>"); continue;
        case 0x0E: flush(); run("<w:br w:type=\"column\"/>"); continue;
        case 0x1E: flush(); run("<w:noBreakHyphen/>"); continue;
        case 0x1F: flush(); run("<w:softHyphen/>"); continue;
        default: break;
      }
      if (r.spec && !inCode) {
        if (ch == 0x01) {
          flush();
          if (r.pic >= 0) {
            if (std::string pic = Picture(r.pic, *story.part); !pic.empty()) run(pic);
          }
          continue;
        }
        if (ch == 0x02) {
          flush();
          if (story.kind == StoryKind::Footnote) run("<w:footnoteRef/>");
          else if (story.kind == StoryKind::Endnote) run("<w:endnoteRef/>");
          else if (story.kind == StoryKind::Main) {
            if (auto f = footRefs_.find(c); f != footRefs_.end()) run("<w:footnoteReference w:id=\"" + Num(f->second + 1) + "\"/>");
            else if (auto e = endRefs_.find(c); e != endRefs_.end()) run("<w:endnoteReference w:id=\"" + Num(e->second + 1) + "\"/>");
          }
          continue;
        }
        if (ch == 0x05) {
          flush();
          if (story.kind == StoryKind::Comment) run("<w:annotationRef/>");
          else if (auto a = commentRefs_.find(c); a != commentRefs_.end() && story.kind == StoryKind::Main)
            run("<w:commentReference w:id=\"" + Num(a->second) + "\"/>");
          continue;
        }
        if (ch == 0x28 && !r.sym.empty()) {
          flush();
          run(r.sym);
          continue;
        }
        if (ch == 0x08 && (story.kind == StoryKind::Main || story.kind == StoryKind::Header)) {
          flush();
          if (std::string shape = Floating(c, story); !shape.empty()) run(shape);
          continue;
        }
        if (ch < 0x20) continue;
      }
      if (ch < 0x20) continue;
      text.push_back(ch);
    }
    flush();
    cp = end;
  }
}

// An OfficeArt blip record as an image file: its extension and bytes. False for kinds that aren't
// read (PICT) and damaged ones.
bool Blip(const Bytes& rec, std::string& ext, std::vector<uint8_t>& bytes) {
  const unsigned inst = rec.U16(0) >> 4, type = rec.U16(2);
  const Bytes body = rec.Sub(8, rec.U32(4));
  const size_t uids = (inst & 1) ? 32 : 16;
  bytes.clear();
  if (type == 0xF01A || type == 0xF01B) {  // EMF, WMF: a header, then usually deflated
    const Bytes h = body.Sub(uids, 34);
    const Bytes data = body.Sub(uids + 34);
    if (h.U8(32) == 0) {
      if (data.size() < 2 || h.U32(0) > (64u << 20) || !pkg::Inflate(data.d.subspan(2), h.U32(0), bytes)) return false;
    } else {
      bytes.assign(data.d.begin(), data.d.end());
    }
    ext = type == 0xF01A ? "emf" : "wmf";
    return !bytes.empty();
  }
  const Bytes data = body.Sub(uids + 1);
  if (type == 0xF01D || type == 0xF02A) ext = "jpeg";
  else if (type == 0xF01E) ext = "png";
  else if (type == 0xF029) ext = "tiff";
  else if (type == 0xF01F) {  // a DIB: made a BMP file
    const uint32_t header = data.U32(0), bits = data.U16(14), used = data.U32(32), compression = data.U32(16);
    const uint32_t palette = 4 * (used ? used : bits <= 8 ? (1u << bits) : 0) + (compression == 3 && header == 40 ? 12 : 0);
    const uint32_t size = static_cast<uint32_t>(14 + data.size()), off = 14 + header + palette;
    const uint8_t h[14] = {'B', 'M', static_cast<uint8_t>(size), static_cast<uint8_t>(size >> 8), static_cast<uint8_t>(size >> 16),
                           static_cast<uint8_t>(size >> 24), 0, 0, 0, 0, static_cast<uint8_t>(off), static_cast<uint8_t>(off >> 8),
                           static_cast<uint8_t>(off >> 16), static_cast<uint8_t>(off >> 24)};
    bytes.assign(h, h + 14);
    ext = "bmp";
  } else {
    return false;
  }
  bytes.insert(bytes.end(), data.d.begin(), data.d.end());
  return data.size() > 0;
}

// A picture's a:graphic.
std::string Graphic(const std::string& rid, const std::string& id, const std::string& ext, const std::string& size) {
  return "<a:graphic xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><a:graphicData "
         "uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><pic:pic "
         "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><pic:nvPicPr><pic:cNvPr id=\"0\" "
         "name=\"image" + id + "." + ext + "\"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill><a:blip r:embed=\"" + rid +
         "\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill><pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext " +
         size + "/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData>"
         "</a:graphic>";
}

// Twips as VML points ("12.05").
std::string Pt(int64_t twips) {
  const int64_t a = twips < 0 ? -twips : twips, hundredths = a % 20 * 5;
  return (twips < 0 ? "-" : "") + Num(a / 20) + "." + (hundredths < 10 ? "0" : "") + Num(hundredths);
}

std::string Converter::Media(const std::string& ext, std::vector<uint8_t> bytes, PartOut& part) {
  const std::string name = "media/image" + Num(static_cast<int64_t>(media_.size() + 1)) + "." + ext;
  media_.push_back({"word/" + name, std::move(bytes)});
  mediaTypes_.insert(ext);
  return part.Add("image", name);
}

// An inline picture: a PICF in the Data stream, then the OfficeArt shape and its blip.
std::string Converter::Picture(int64_t fc, PartOut& part) {
  const Bytes d0 = data_.Sub(static_cast<size_t>(fc));
  const Bytes d = d0.Sub(0, static_cast<size_t>(std::max(0, d0.I32(0))));
  const size_t cbHeader = d.U16(4);
  if (cbHeader < 0x44 || d.size() < cbHeader) return {};
  const int64_t cx = int64_t{d.I16(28)} * d.U16(32) / 1000 * 635, cy = int64_t{d.I16(30)} * d.U16(34) / 1000 * 635;
  if (cx <= 0 || cy <= 0) return {};
  size_t o = cbHeader;
  if (d.I16(6) == 0x66) o += 1 + size_t{d.U8(o)};
  // The blip: inside an FBSE, or on its own, after the shape container.
  std::string ext;
  std::vector<uint8_t> bytes;
  for (int guard = 0; d.Has(o, 8) && guard < 64; ++guard) {
    const uint16_t type = d.U16(o + 2);
    const size_t len = d.U32(o + 4);
    const Bytes rec = d.Sub(o, 8 + len);
    if (type == 0xF007 && Blip(rec.Sub(8 + 36 + rec.U8(8 + 33)), ext, bytes)) break;
    if (type >= 0xF018 && type <= 0xF117 && Blip(rec, ext, bytes)) break;
    o += 8 + len;
  }
  if (bytes.empty()) return {};
  const std::string rid = Media(ext, std::move(bytes), part);
  const std::string id = Num(docPr_++), size = "cx=\"" + Num(cx) + "\" cy=\"" + Num(cy) + "\"";
  return "<w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\"><wp:extent " + size +
         "/><wp:docPr id=\"" + id + "\" name=\"Picture " + id + "\"/>" + Graphic(rid, id, ext, size) +
         "</wp:inline></w:drawing>";
}

// The drawing layer: the blip store, the shapes, where the text anchors them (SPA) and the text
// boxes' stories.
void Converter::ReadArt() {
  const Bytes art = Table(50);
  auto record = [](const Bytes& b, size_t at) { return b.Sub(at, 8 + size_t{b.U32(at + 4)}); };
  size_t o = 0;
  if (art.U16(2) == 0xF000) {  // OfficeArtDggContainer, with the OfficeArtBStoreContainer
    const Bytes dgg = record(art, 0);
    for (size_t k = 8; dgg.Has(k, 8); k += 8 + size_t{dgg.U32(k + 4)})
      if (dgg.U16(k + 2) == 0xF001) {
        const Bytes store = record(dgg, k);
        for (size_t b = 8; store.Has(b, 8) && bstore_.size() < 65536; b += 8 + size_t{store.U32(b + 4)})
          bstore_.push_back(record(store, b));
      }
    o = dgg.size();
  }
  // Each drawing (the body's, the headers'): a byte, then an OfficeArtDgContainer whose group holds
  // the shapes. Groups' own shapes are placed in the group, so groups are left out.
  for (int guard = 0; art.Has(o + 1, 8) && guard < 16; ++guard) {
    const Bytes dg = record(art, o + 1);
    o += 1 + dg.size();
    for (size_t k = 8; dg.Has(k, 8); k += 8 + size_t{dg.U32(k + 4)}) {
      if (dg.U16(k + 2) != 0xF003) continue;
      const Bytes group = record(dg, k);
      for (size_t j = 8; group.Has(j, 8); j += 8 + size_t{group.U32(j + 4)}) {
        if (group.U16(j + 2) != 0xF004) continue;
        const Bytes sp = record(group, j);
        Shape shape;
        uint32_t spid = 0, flags = 0;
        for (size_t r = 8; sp.Has(r, 8); r += 8 + size_t{sp.U32(r + 4)}) {
          const uint16_t type = sp.U16(r + 2);
          const Bytes body = record(sp, r).Sub(8);
          if (type == 0xF00A) {
            spid = body.U32(0);
            flags = body.U32(4);
          } else if (type == 0xF00B || type == 0xF122) {
            for (size_t p = 0, n = sp.U16(r) >> 4; p < n; ++p) {
              const uint16_t id = body.U16(6 * p);
              if ((id & 0x3FFF) == 0x0104 && (id & 0x4000)) shape.pib = body.U32(6 * p + 2);
              if ((id & 0x3FFF) == 0x0080) shape.text = true;
            }
          }
        }
        if (spid && !(flags & 0x5)) shapes_[spid] = shape;  // not a group, not the patriarch
      }
    }
  }
  auto spa = [&](size_t index, std::map<uint32_t, Bytes>& out) {
    const Bytes plc = Table(index);
    const size_t n = plc.size() >= 4 ? (plc.size() - 4) / 30 : 0;
    for (size_t k = 0; k < n; ++k) out[plc.U32(4 * k)] = plc.Sub(4 * (n + 1) + 26 * k, 26);
  };
  spa(40, spaMain_);
  spa(41, spaHeader_);
  const uint32_t txbx = ccp_[0] + ccp_[1] + ccp_[2] + ccp_[3] + ccp_[4] + ccp_[5];
  auto boxes = [&](size_t index, uint32_t base, std::map<uint32_t, std::pair<uint32_t, uint32_t>>& out) {
    const Bytes plc = Table(index);
    const size_t n = plc.size() >= 4 ? (plc.size() - 4) / 26 : 0;
    for (size_t k = 0; k < n; ++k) {
      const uint32_t a = plc.U32(4 * k), b = plc.U32(4 * k + 4);
      if (b > a) out[plc.U32(4 * (n + 1) + 22 * k + 14)] = {base + a, base + b};
    }
  };
  boxes(56, txbx, textBoxes_);
  boxes(58, txbx + ccp_[6], headerTextBoxes_);
}

// A floating picture or text box where the text has its anchor (0x08); empty for other shapes.
std::string Converter::Floating(uint32_t cp, Story& story) {
  const bool header = story.kind == StoryKind::Header;
  const auto& spas = header ? spaHeader_ : spaMain_;
  auto at = spas.find(header ? cp - (ccp_[0] + ccp_[1]) : cp);
  if (at == spas.end()) return {};
  const Bytes& spa = at->second;
  const uint32_t spid = spa.U32(0);
  auto shape = shapes_.find(spid);
  if (shape == shapes_.end()) return {};
  const int64_t x = spa.I32(4), y = spa.I32(8), w = int64_t{spa.I32(12)} - x, h = int64_t{spa.I32(16)} - y;
  if (w <= 0 || h <= 0 || w > 1000000 || h > 1000000) return {};
  const uint16_t flags = spa.U16(20);
  const unsigned bx = (flags >> 1) & 3, by = (flags >> 3) & 3, wr = (flags >> 5) & 0xF, wrk = (flags >> 9) & 0xF;
  const bool below = (flags & 0x4000) != 0;
  const std::string z = Num(z_++);
  static const char* const kSide[] = {"bothSides", "left", "right", "largest"};
  if (shape->second.pib > 0 && static_cast<size_t>(shape->second.pib) <= bstore_.size()) {
    // The blip: in its FBSE, or in the WordDocument stream where the FBSE says (foDelay).
    const Bytes& fbse = bstore_[static_cast<size_t>(shape->second.pib - 1)];
    const size_t inner = 8 + 36 + size_t{fbse.U8(8 + 33)};
    const Bytes blip = fbse.size() > inner + 8 ? fbse.Sub(inner)
                                               : word_.Sub(fbse.U32(8 + 28), 8 + size_t{word_.U32(size_t{fbse.U32(8 + 28)} + 4)});
    std::string ext;
    std::vector<uint8_t> bytes;
    if (!Blip(blip, ext, bytes)) return {};
    const std::string rid = Media(ext, std::move(bytes), *story.part);
    const std::string id = Num(docPr_++), size = "cx=\"" + Num(w * 635) + "\" cy=\"" + Num(h * 635) + "\"";
    static const char* const kH[] = {"margin", "page", "column", "column"};
    static const char* const kV[] = {"margin", "page", "paragraph", "paragraph"};
    const std::string wrap = wr == 1 ? "<wp:wrapTopAndBottom/>"
                             : wr == 3 ? "<wp:wrapNone/>"
                                       : std::string("<wp:wrapSquare wrapText=\"") + kSide[std::min(wrk, 3u)] + "\"/>";
    return "<w:drawing><wp:anchor distT=\"0\" distB=\"0\" distL=\"114300\" distR=\"114300\" simplePos=\"0\" relativeHeight=\"" + z +
           "\" behindDoc=\"" + (below ? "1" : "0") + "\" locked=\"0\" layoutInCell=\"1\" allowOverlap=\"1\"><wp:simplePos x=\"0\" "
           "y=\"0\"/><wp:positionH relativeFrom=\"" + kH[bx] + "\"><wp:posOffset>" + Num(x * 635) +
           "</wp:posOffset></wp:positionH><wp:positionV relativeFrom=\"" + kV[by] + "\"><wp:posOffset>" + Num(y * 635) +
           "</wp:posOffset></wp:positionV><wp:extent " + size + "/><wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/>" + wrap +
           "<wp:docPr id=\"" + id + "\" name=\"Picture " + id + "\"/>" + Graphic(rid, id, ext, size) + "</wp:anchor></w:drawing>";
  }
  const auto& boxes = header ? headerTextBoxes_ : textBoxes_;
  auto box = boxes.find(spid);
  if (box == boxes.end()) return {};
  // A text box, as VML (which Word reads in every version): its story's paragraphs inside.
  static const char* const kRel[] = {"margin", "page", "text", "text"};
  static const char* const kWrap[] = {"square", "topAndBottom", "square", "none", "tight", "through"};
  Story text{StoryKind::TextBox, story.part, {}};
  uint32_t cp1 = box->second.second;
  if (cp1 - box->second.first >= 2 && text_.size() >= cp1 && text_[cp1 - 1] == 0x0D && text_[cp1 - 2] == 0x0D) --cp1;
  const std::string content = EmitStory(box->second.first, cp1, text);
  return "<w:pict><v:shape id=\"_x0000_s" + Num(spid) + "\" style=\"position:absolute;margin-left:" +
         Pt(x) + "pt;margin-top:" + Pt(y) + "pt;width:" + Pt(w) + "pt;height:" + Pt(h) + "pt;z-index:" + (below ? "-" : "") + z +
         ";mso-position-horizontal-relative:" + kRel[bx] + ";mso-position-vertical-relative:" + kRel[by] +
         "\"><v:textbox><w:txbxContent>" + content + "</w:txbxContent></v:textbox><w10:wrap type=\"" +
         kWrap[std::min(wr, 5u)] + "\"/></v:shape></w:pict>";
}

// ---------------------------------------------------------------------------
// The other parts.


std::string Converter::StylesXml() {
  std::string out = std::string("<w:styles") + kNs + "><w:docDefaults><w:rPrDefault><w:rPr>";
  {
    Rpr r;
    for (size_t k = 0; k < 4; ++k)
      if (defaultFonts_[k] < fonts_.size()) r.font[k] = Esc(fonts_[defaultFonts_[k]].name);
    r.s[rSz] = El("sz", 20);
    r.s[rSzCs] = El("szCs", 20);
    const std::string x = RprXml(r);
    out += x.substr(7, x.size() - 15);  // without <w:rPr> and </w:rPr>
  }
  out += "</w:rPr></w:rPrDefault><w:pPrDefault/></w:docDefaults>";
  static const char* const kType[] = {"", "paragraph", "character", "table", "numbering"};
  bool defaults[5] = {};
  for (size_t istd = 0; istd < styles_.size(); ++istd) {
    const Style& s = styles_[istd];
    if (!s.defined || s.type < 1 || s.type > 4) continue;
    // Normal, Default Paragraph Font, Table Normal and No List are the defaults.
    const bool isDefault = !defaults[s.type] && (istd == 0 || istd == 10 || istd == 11 || istd == 12 || s.sti == 0 ||
                                                  s.sti == 65 || s.sti == 105 || s.sti == 107);
    if (isDefault) defaults[s.type] = true;
    out += std::string("<w:style w:type=\"") + kType[s.type] + "\"" + (isDefault ? " w:default=\"1\"" : "") +
           " w:styleId=\"" + Esc(std::u16string(s.id.begin(), s.id.end())) + "\">";
    const size_t comma = s.name.find(u',');
    out += "<w:name w:val=\"" + Esc(s.name.substr(0, comma)) + "\"/>";
    if (comma != std::u16string::npos) out += "<w:aliases w:val=\"" + Esc(s.name.substr(comma + 1)) + "\"/>";
    auto ref = [&](const char* name, int istd2) {
      if (istd2 >= 0 && static_cast<size_t>(istd2) < styles_.size() && styles_[static_cast<size_t>(istd2)].defined &&
          static_cast<size_t>(istd2) != istd && styles_[static_cast<size_t>(istd2)].type == s.type)
        out += El(name, StyleId(static_cast<size_t>(istd2)));
    };
    ref("basedOn", s.base);
    if (s.type == 1) ref("next", s.next);
    if (s.grfstd & 0x0002) out += "<w:hidden/>";
    if (s.grfstd & 0x0100) out += "<w:semiHidden/>";
    if (s.grfstd & 0x0800) out += "<w:unhideWhenUsed/>";
    if (s.grfstd & 0x1000) out += "<w:qFormat/>";
    if (s.type == 1 || s.type == 3 || s.type == 4) {
      Ppr p;
      ApplyPap(s.papx, p);
      p.istd = 0;
      out += PprXml(p, "", "");
    }
    if (s.type <= 3) {
      Rpr r;
      ApplyChp(s.chpx, r);
      r.s[rStyle].clear();
      out += RprXml(r);
    }
    if (s.type == 3)
      out += "<w:tblPr><w:tblInd w:w=\"0\" w:type=\"dxa\"/><w:tblCellMar><w:top w:w=\"0\" w:type=\"dxa\"/><w:left w:w=\"108\" "
             "w:type=\"dxa\"/><w:bottom w:w=\"0\" w:type=\"dxa\"/><w:right w:w=\"108\" w:type=\"dxa\"/></w:tblCellMar></w:tblPr>";
    out += "</w:style>";
  }
  return out + "</w:styles>";
}

std::string Converter::NumberingXml() {
  std::string out = std::string("<w:numbering") + kNs + ">";
  for (size_t i = 0; i < lists_.size(); ++i) {
    const List& l = lists_[i];
    out += "<w:abstractNum w:abstractNumId=\"" + Num(static_cast<int64_t>(i)) + "\"><w:nsid w:val=\"" +
           Hex(static_cast<uint32_t>(l.lsid), 8) + "\"/>" +
           El("multiLevelType", (l.flags & 1) ? "singleLevel" : (l.flags & 0x10) ? "hybridMultilevel" : "multilevel") +
           "<w:tmpl w:val=\"" + Hex(static_cast<uint32_t>(l.tplc), 8) + "\"/>";
    for (size_t k = 0; k < l.levels.size(); ++k) {
      const Level& v = l.levels[k];
      out += "<w:lvl w:ilvl=\"" + Num(static_cast<int64_t>(k)) + "\">" + El("start", v.lvlf.I32(0)) +
             El("numFmt", NumberFormat(v.lvlf.U8(4)));
      if (l.styles[k] < styles_.size() && styles_[l.styles[k]].defined) out += El("pStyle", StyleId(l.styles[k]));
      if (v.lvlf.U8(5) & 4) out += "<w:isLgl/>";
      const uint8_t follow = v.lvlf.U8(15);
      if (follow == 1 || follow == 2) out += El("suff", follow == 1 ? "space" : "nothing");
      std::u16string text;
      for (char16_t c : v.text) {
        if (c < 9) text += u"%" + std::u16string(1, static_cast<char16_t>(u'1' + c));
        else text += c;
      }
      out += "<w:lvlText w:val=\"" + Esc(text) + "\"/>";
      const unsigned jc = v.lvlf.U8(5) & 3;
      out += El("lvlJc", jc == 1 ? "center" : jc == 2 ? "right" : "left");
      Ppr p;
      ApplyPap(v.papx, p);
      p.istd = 0;
      out += PprXml(p, "", "");
      Rpr r;
      ApplyChp(v.chpx, r);
      out += RprXml(r);
      out += "</w:lvl>";
    }
    out += "</w:abstractNum>";
  }
  for (size_t i = 0; i < overrides_.size(); ++i) {
    const Override& ov = overrides_[i];
    size_t list = 0;
    while (list < lists_.size() && lists_[list].lsid != ov.lsid) ++list;
    if (list == lists_.size()) list = 0;
    out += "<w:num w:numId=\"" + Num(static_cast<int64_t>(i + 1)) + "\">" + El("abstractNumId", static_cast<int64_t>(list));
    for (const auto& [level, start] : ov.starts)
      out += "<w:lvlOverride w:ilvl=\"" + Num(level) + "\">" + El("startOverride", start) + "</w:lvlOverride>";
    out += "</w:num>";
  }
  return out + "</w:numbering>";
}

std::string Converter::FontsXml() const {
  std::string out = std::string("<w:fonts") + kNs + ">";
  static const char* const kFamily[] = {"auto", "roman", "swiss", "modern", "script", "decorative", "auto", "auto"};
  static const char* const kPitch[] = {"default", "fixed", "variable", "default"};
  for (const Font& f : fonts_) {
    if (f.name.empty()) continue;
    out += "<w:font w:name=\"" + Esc(f.name) + "\">";
    if (f.panose.size() == 10) {
      std::string hex;
      for (size_t k = 0; k < 10; ++k) hex += Hex(f.panose.U8(k), 2);
      out += El("panose1", hex);
    }
    out += El("charset", Hex(f.chs, 2)) + El("family", kFamily[(f.ffid >> 4) & 7]) + El("pitch", kPitch[f.ffid & 3]) + "</w:font>";
  }
  return out + "</w:fonts>";
}

std::string Converter::SettingsXml() const {
  return std::string("<w:settings") + kNs + ">" + El("defaultTabStop", tabStop_) + (facingPages_ ? "<w:evenAndOddHeaders/>" : "") +
         (footTexts_.empty() ? "" : "<w:footnotePr><w:footnote w:id=\"-1\"/><w:footnote w:id=\"0\"/></w:footnotePr>") +
         (endTexts_.empty() ? "" : "<w:endnotePr><w:endnote w:id=\"-1\"/><w:endnote w:id=\"0\"/></w:endnotePr>") +
         "<w:compat><w:compatSetting w:name=\"compatibilityMode\" w:uri=\"http://schemas.microsoft.com/office/word\" "
         "w:val=\"11\"/></w:compat></w:settings>";
}

std::string Converter::NotesXml(bool endnotes) {
  const char* tag = endnotes ? "endnote" : "footnote";
  std::string out = std::string("<w:") + tag + "s" + kNs + ">";
  out += std::string("<w:") + tag + " w:type=\"separator\" w:id=\"-1\"><w:p><w:pPr><w:spacing w:after=\"0\" w:line=\"240\" "
         "w:lineRule=\"auto\"/></w:pPr><w:r><w:separator/></w:r></w:p></w:" + tag + ">";
  out += std::string("<w:") + tag + " w:type=\"continuationSeparator\" w:id=\"0\"><w:p><w:pPr><w:spacing w:after=\"0\" "
         "w:line=\"240\" w:lineRule=\"auto\"/></w:pPr><w:r><w:continuationSeparator/></w:r></w:p></w:" + tag + ">";
  const auto& texts = endnotes ? endTexts_ : footTexts_;
  PartOut& part = endnotes ? endnotes_ : footnotes_;
  for (size_t k = 0; k < texts.size(); ++k) {
    Story story{endnotes ? StoryKind::Endnote : StoryKind::Footnote, &part, {}};
    out += std::string("<w:") + tag + " w:id=\"" + Num(static_cast<int64_t>(k + 1)) + "\">" +
           EmitStory(texts[k].first, texts[k].second, story) + "</w:" + tag + ">";
  }
  return out + "</w:" + tag + "s>";
}

std::string Converter::CommentsXml() {
  std::string out = std::string("<w:comments") + kNs + ">";
  for (size_t k = 0; k < commentTexts_.size(); ++k) {
    Story story{StoryKind::Comment, &comments_, {}};
    out += "<w:comment w:id=\"" + Num(static_cast<int64_t>(k)) + "\" w:author=\"" + Esc(commentBy_[k].first) +
           "\" w:initials=\"" + Esc(commentBy_[k].second) + "\">" +
           EmitStory(commentTexts_[k].first, commentTexts_[k].second, story) + "</w:comment>";
  }
  return out + "</w:comments>";
}

bool Converter::Convert(const StreamFn& streams, pkg::Status& status, std::vector<uint8_t>& out) {
  if (!Load(streams, status)) return false;
  Story main{StoryKind::Main, &document_, {}};
  std::string body = EmitStory(0, ccp_[0], main, true);
  body += SepXml(sections_.size() - 1);
  const std::string document = std::string("<w:document") + kNs + "><w:body>" + body + "</w:body></w:document>";
  const bool hasNotes = !footTexts_.empty(), hasEndnotes = !endTexts_.empty(), hasComments = !commentTexts_.empty();
  const std::string footnotes = hasNotes ? NotesXml(false) : "", endnotes = hasEndnotes ? NotesXml(true) : "",
                    comments = hasComments ? CommentsXml() : "";
  document_.Add("styles", "styles.xml");
  document_.Add("settings", "settings.xml");
  document_.Add("fontTable", "fontTable.xml");
  if (!lists_.empty()) document_.Add("numbering", "numbering.xml");
  if (hasNotes) document_.Add("footnotes", "footnotes.xml");
  if (hasEndnotes) document_.Add("endnotes", "endnotes.xml");
  if (hasComments) document_.Add("comments", "comments.xml");

  const std::string decl = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";
  const std::string wml = "application/vnd.openxmlformats-officedocument.wordprocessingml.";
  std::string types = "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                      "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>";
  for (const std::string& ext : mediaTypes_)
    types += "<Default Extension=\"" + ext + "\" ContentType=\"" +
             (ext == "emf" ? "image/x-emf" : ext == "wmf" ? "image/x-wmf" : "image/" + ext) + "\"/>";
  auto override = [&](const std::string& name, const std::string& type) {
    types += "<Override PartName=\"/" + name + "\" ContentType=\"" + wml + type + "+xml\"/>";
  };
  std::vector<std::pair<std::string, std::string>> parts;
  auto rels = [&](const PartOut& p) {
    if (p.rels.empty()) return;
    const size_t slash = p.name.rfind('/');
    parts.push_back({p.name.substr(0, slash) + "/_rels/" + p.name.substr(slash + 1) + ".rels",
                     decl + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">" + p.rels +
                         "</Relationships>"});
  };
  override("word/document.xml", "document.main");
  parts.push_back({"word/document.xml", decl + document});
  parts.push_back({"word/styles.xml", decl + StylesXml()});
  override("word/styles.xml", "styles");
  parts.push_back({"word/settings.xml", decl + SettingsXml()});
  override("word/settings.xml", "settings");
  parts.push_back({"word/fontTable.xml", decl + FontsXml()});
  override("word/fontTable.xml", "fontTable");
  if (!lists_.empty()) {
    parts.push_back({"word/numbering.xml", decl + NumberingXml()});
    override("word/numbering.xml", "numbering");
  }
  if (hasNotes) {
    parts.push_back({"word/footnotes.xml", decl + footnotes});
    override("word/footnotes.xml", "footnotes");
    rels(footnotes_);
  }
  if (hasEndnotes) {
    parts.push_back({"word/endnotes.xml", decl + endnotes});
    override("word/endnotes.xml", "endnotes");
    rels(endnotes_);
  }
  if (hasComments) {
    parts.push_back({"word/comments.xml", decl + comments});
    override("word/comments.xml", "comments");
    rels(comments_);
  }
  for (const PartOut& p : parts_) {
    parts.push_back({p.name, decl + p.content});
    override(p.name, p.name.find("/footer") != std::string::npos ? "footer" : "header");
    rels(p);
  }
  rels(document_);
  parts.push_back({"_rels/.rels", decl + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                                         "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/"
                                         "2006/relationships/officeDocument\" Target=\"word/document.xml\"/></Relationships>"});
  parts.insert(parts.begin(), {"[Content_Types].xml", decl + types + "</Types>"});

  pkg::MemorySink sink;
  pkg::ZipWriter zip(sink);
  for (const auto& [name, text] : parts) zip.Add(name, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
  for (const auto& [name, bytes] : media_) zip.Add(name, bytes, nullptr, pkg::ZipWriter::Compression::Store);
  if (!zip.Finish().Ok()) {
    status = pkg::Status::Fail(pkg::Status::Code::TooLarge, "This Word document is too large to open.");
    return false;
  }
  out = std::move(sink.bytes);
  return true;
}

}  // namespace

std::vector<uint8_t> ToDocx(const StreamFn& streams, pkg::Status& status, bool decrypted) {
  status = {};
  std::vector<uint8_t> out;
  Converter c;
  c.decrypted = decrypted;
  if (!c.Convert(streams, status, out)) return {};
  return out;
}

bool IsEncrypted(const std::vector<uint8_t>& wordDocument) {
  const Bytes b{wordDocument};
  return b.U16(0) == 0xA5EC && (b.U16(10) & 0x0100) != 0;
}

}  // namespace fw::doc::msdoc
