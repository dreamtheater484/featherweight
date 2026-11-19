// Excel 97-2003 workbooks ([MS-XLS], BIFF8) into an .xlsx package, for the xlsx reader. See Xls.h.
//
// The Workbook stream is a sequence of records (type, size, data; data over 8,224 bytes goes on in
// CONTINUE records). The globals substream comes first (fonts, formats, XFs, the shared strings,
// names, sheets); each sheet's substream follows at the offset its BOUNDSHEET record gives.
#include "sheet/xls/Xls.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <unordered_map>

#include <pugixml.hpp>

#include "pkg/Snapshot.h"
#include "pkg/Zip.h"
#include "sheet/model/Types.h"
#include "sheet/xlsx/Internal.h"
#include "sheet/xls/Internal.h"

namespace fw::sheet::xls {
namespace {

using detail::AppendCol;
using detail::AppendNumber;
using detail::Bytes;

constexpr size_t kMaxCells = 20'000'000;  // more than BIFF8's sheets hold in any real file
constexpr uint16_t kContinue = 0x003C;

// ---------------------------------------------------------------------------
// Records

struct Record {
  uint16_t type = 0;
  std::vector<uint8_t> data;
  std::vector<size_t> breaks;  // where each CONTINUE's data starts in `data`
};

class Records {
 public:
  explicit Records(std::span<const uint8_t> s, size_t at = 0) : s_(s), pos_(at) {}
  // Where the record Next gave last starts in the stream.
  size_t Start() const { return start_; }
  // The next record with its CONTINUE records joined; false at the end or when it's cut short.
  bool Next(Record& r) {
    if (pos_ + 4 > s_.size()) return false;
    start_ = pos_;
    r.type = static_cast<uint16_t>(s_[pos_] | s_[pos_ + 1] << 8);
    size_t len = s_[pos_ + 2] | s_[pos_ + 3] << 8;
    pos_ += 4;
    if (len > s_.size() - pos_) return false;
    r.data.assign(s_.begin() + pos_, s_.begin() + pos_ + len);
    r.breaks.clear();
    pos_ += len;
    while (pos_ + 4 <= s_.size() && (s_[pos_] | s_[pos_ + 1] << 8) == kContinue && r.type != kContinue) {
      len = s_[pos_ + 2] | s_[pos_ + 3] << 8;
      if (len > s_.size() - pos_ - 4) return false;
      r.breaks.push_back(r.data.size());
      r.data.insert(r.data.end(), s_.begin() + pos_ + 4, s_.begin() + pos_ + 4 + len);
      pos_ += 4 + len;
    }
    return true;
  }

 private:
  std::span<const uint8_t> s_;
  size_t pos_, start_ = 0;
};

// Reads strings that may run across CONTINUE records: where character data crosses into the next
// one, that record starts with a fresh flags byte (fHighByte) ([MS-XLS] 2.1.4).
class ContinuedText {
 public:
  ContinuedText(const std::vector<uint8_t>& d, const std::vector<size_t>& breaks) : d_(d), breaks_(breaks) {}
  bool Ok() const { return ok_; }
  size_t Pos() const { return pos_; }
  bool AtEnd() const { return pos_ >= d_.size(); }
  uint8_t U8() {
    if (pos_ >= d_.size()) return ok_ = false, 0;
    return d_[pos_++];
  }
  uint16_t U16() {
    const uint8_t a = U8();
    return static_cast<uint16_t>(a | U8() << 8);
  }
  uint32_t U32() {
    const uint32_t a = U16();
    return a | uint32_t{U16()} << 16;
  }
  void Skip(size_t n) {
    if (n > d_.size() - std::min(pos_, d_.size())) ok_ = false, pos_ = d_.size();
    else pos_ += n;
  }
  // `cch` characters starting with the high-byte flag `high`.
  bool Chars(uint32_t cch, bool high, std::vector<uint16_t>& out) {
    out.clear();
    if (cch > d_.size()) return ok_ = false;
    out.reserve(cch);
    size_t next = NextBreak();
    for (uint32_t i = 0; i < cch; ++i) {
      if (pos_ == next) {  // a CONTINUE starts here: its flags byte
        high = (U8() & 1) != 0;
        next = NextBreak();
      }
      if (high) {
        out.push_back(U16());
      } else {
        out.push_back(U8());
      }
      if (!ok_) return false;
    }
    return true;
  }

 private:
  size_t NextBreak() const {
    auto it = std::upper_bound(breaks_.begin(), breaks_.end(), pos_ == 0 ? 0 : pos_ - 1);
    while (it != breaks_.end() && *it < pos_) ++it;
    return it == breaks_.end() ? SIZE_MAX : *it;
  }
  const std::vector<uint8_t>& d_;
  const std::vector<size_t>& breaks_;
  size_t pos_ = 0;
  bool ok_ = true;
};

// ---------------------------------------------------------------------------
// XML text

// Text for an element or attribute: XML's escapes, and SpreadsheetML's _xHHHH_ for characters XML
// can't hold (and for a literal "_x" that would read as one).
void Esc(std::string& o, std::string_view s) {
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\t': o += "&#9;"; break;
      case '\n': o += "&#10;"; break;
      case '\r': o += "_x000D_"; break;  // as Excel writes it; the reader decodes ST_Xstring
      case '_':
        if (i + 6 < s.size() && s[i + 1] == 'x' && s[i + 6] == '_' &&
            std::all_of(s.begin() + i + 2, s.begin() + i + 6, [](char h) { return std::isxdigit(static_cast<unsigned char>(h)); }))
          o += "_x005F_";
        else
          o += '_';
        break;
      default:
        if (c < 0x20) {
          static const char kHex[] = "0123456789ABCDEF";
          o += "_x00";
          o += kHex[c >> 4];
          o += kHex[c & 15];
          o += '_';
        } else if (c == 0xEF && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xBF &&
                   (static_cast<unsigned char>(s[i + 2]) == 0xBE || static_cast<unsigned char>(s[i + 2]) == 0xBF)) {
          o += static_cast<unsigned char>(s[i + 2]) == 0xBE ? "_xFFFE_" : "_xFFFF_";  // not XML characters
          i += 2;
        } else {
          o += static_cast<char>(c);
        }
    }
  }
}
std::string Esc(std::string_view s) {
  std::string o;
  Esc(o, s);
  return o;
}

// An ST_Xstring element's text (headers and footers), as Excel writes it: xlsx::EncodeXstring's
// _xHHHH_ (a CR as _x000D_, which Excel keeps at the end of a header), then XML's own escapes.
std::string EscXstring(std::string_view s) {
  std::string o;
  for (char c : xlsx::EncodeXstring(s)) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      default: o += c;
    }
  }
  return o;
}

std::string Utf8(const std::vector<uint16_t>& u, size_t from = 0, size_t to = SIZE_MAX) {
  std::string s;
  to = std::min(to, u.size());
  if (from < to) detail::AppendUtf16(s, u.data() + from, to - from);
  return s;
}

void Attr(std::string& o, const char* name, std::string_view v) {
  o += ' ';
  o += name;
  o += "=\"";
  Esc(o, v);
  o += '"';
}
void Attr(std::string& o, const char* name, long long v) {
  o += ' ';
  o += name;
  o += "=\"";
  o += std::to_string(v);
  o += '"';
}
void AttrNum(std::string& o, const char* name, double v) {
  o += ' ';
  o += name;
  o += "=\"";
  AppendNumber(o, v);
  o += '"';
}

// Whether an element has a child whose name (with any prefix) ends with `local`.
bool HasChildEnding(pugi::xml_node n, std::string_view local) {
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling()) {
    const std::string_view name = c.name();
    if (name.size() >= local.size() && name.substr(name.size() - local.size()) == local) return true;
  }
  return false;
}

std::string CellRef(uint32_t row, uint32_t col) {
  std::string s;
  AppendCol(s, col);
  s += std::to_string(row + 1);
  return s;
}
std::string RangeRef(uint32_t r1, uint32_t c1, uint32_t r2, uint32_t c2) {
  if (r1 == r2 && c1 == c2) return CellRef(r1, c1);
  return CellRef(r1, c1) + ":" + CellRef(r2, c2);
}

// ---------------------------------------------------------------------------
// Colors

constexpr uint32_t kDefaultPalette[56] = {
    0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF, 0x800000, 0x008000, 0x000080,
    0x808000, 0x800080, 0x008080, 0xC0C0C0, 0x808080, 0x9999FF, 0x993366, 0xFFFFCC, 0xCCFFFF, 0x660066, 0xFF8080,
    0x0066CC, 0xCCCCFF, 0x000080, 0xFF00FF, 0xFFFF00, 0x00FFFF, 0x800080, 0x800000, 0x008080, 0x0000FF, 0x00CCFF,
    0xCCFFFF, 0xCCFFCC, 0xFFFF99, 0x99CCFF, 0xFF99CC, 0xCC99FF, 0xFFCC99, 0x3366FF, 0x33CCCC, 0x99CC00, 0xFFCC00,
    0xFF9900, 0xFF6600, 0x666699, 0x969696, 0x003366, 0x339966, 0x003300, 0x333300, 0x993300, 0x993366, 0x333399,
    0x333333};

// A color an XFEXT record or SHEETEXT gives (CFColor/FullColorExt): RGB or a theme color with tint.
struct ExtColor {
  enum Kind : uint8_t { None, Rgb, Theme, Indexed } kind = None;
  uint32_t value = 0;
  double tint = 0;
};

struct Palette {
  uint32_t rgb[56];
  bool custom = false;
  Palette() { std::copy(std::begin(kDefaultPalette), std::end(kDefaultPalette), rgb); }
  // An icv as a color element's attributes; empty for automatic (64, 0x7FFF...).
  std::string Attrs(uint32_t icv, const ExtColor* ext = nullptr) const {
    char buf[48];
    if (ext && ext->kind == ExtColor::Rgb) {
      std::snprintf(buf, sizeof buf, " rgb=\"FF%06X\"", ext->value & 0xFFFFFF);
      return buf;
    }
    if (ext && ext->kind == ExtColor::Theme && ext->value < 12) {
      std::string s = " theme=\"" + std::to_string(ext->value) + "\"";
      if (ext->tint != 0 && std::isfinite(ext->tint) && std::abs(ext->tint) <= 1) AttrNum(s, "tint", ext->tint);
      return s;
    }
    if (icv < 64) {
      std::snprintf(buf, sizeof buf, " rgb=\"FF%06X\"", rgb[icv < 8 ? icv : icv - 8] & 0xFFFFFF);
      return buf;
    }
    if (icv == 64 || icv == 65) return " indexed=\"" + std::to_string(icv) + "\"";
    return {};
  }
};

// ---------------------------------------------------------------------------
// The globals

struct FontRec {
  double size = 10;
  bool bold = false, italic = false, strike = false, outline = false, shadow = false;
  uint16_t icv = 0x7FFF;
  uint8_t underline = 0, family = 0, charset = 0;
  uint16_t script = 0;
  std::string name = "Arial";
  ExtColor color;  // from XFEXT? fonts don't have one; cells' text colors come through XFEXT
};

struct XfRec {
  uint16_t font = 0, fmt = 0, parent = 0;
  bool locked = true, hidden = false, style = false;
  uint8_t halign = 0, valign = 2, rot = 0, indent = 0, readingOrder = 0;
  bool wrap = false, shrink = false, justLast = false;
  uint8_t used = 0;
  uint8_t bl = 0, br = 0, bt = 0, bb = 0, bd = 0, diag = 0;
  uint8_t cl = 64, cr = 64, ct = 64, cb = 64, cd = 64;
  uint8_t pattern = 0, fore = 64, back = 65;
  // XFEXT: full colors for the fill, the borders and the text.
  ExtColor extFore, extBack, extTop, extBottom, extLeft, extRight, extDiag, extText;
};

struct SheetEntry {
  std::string name;
  uint32_t pos = 0;
  uint8_t state = 0, type = 0;
  int out = -1;  // its index among the sheets written; -1: left out
};

struct NameEntry {
  std::string name;  // as xlsx writes it (_xlnm. for the built-in ones)
  int itab = 0;      // 1-based sheet; 0: the workbook
  bool hidden = false, func = false, skip = false, builtin = false;
  uint8_t builtinCode = 0xFF;
  std::vector<uint8_t> rgce, rgcb;
};

struct SstString {
  std::vector<uint16_t> text;
  std::vector<std::pair<uint16_t, uint16_t>> runs;  // character, font
};

const char* BuiltinName(uint8_t code) {
  static const char* const kNames[] = {"Consolidate_Area", "Auto_Open", "Auto_Close", "Extract", "Database",
                                       "Criteria", "Print_Area", "Print_Titles", "Recorder", "Data_Form",
                                       "Auto_Activate", "Auto_Deactivate", "Sheet_Title", "_FilterDatabase"};
  return code < 14 ? kNames[code] : nullptr;
}

const char* BuiltinStyleName(uint8_t id) {
  static const char* const kNames[] = {
      "Normal", "RowLevel_", "ColLevel_", "Comma", "Currency", "Percent", "Comma [0]", "Currency [0]", "Hyperlink",
      "Followed Hyperlink", "Note", "Warning Text", "Emphasis 1", "Emphasis 2", "Emphasis 3", "Title", "Heading 1",
      "Heading 2", "Heading 3", "Heading 4", "Input", "Output", "Calculation", "Check Cell", "Linked Cell", "Total",
      "Good", "Bad", "Neutral"};
  return id < sizeof(kNames) / sizeof(kNames[0]) ? kNames[id] : nullptr;
}

std::string StyleName(uint8_t id, uint8_t level) {
  if (id == 1 || id == 2) return std::string(BuiltinStyleName(id)) + std::to_string(level + 1);
  if (const char* n = BuiltinStyleName(id)) return n;
  if (id >= 29 && id <= 52) {
    const int k = (id - 29) / 4 + 1, v = (id - 29) % 4;
    static const char* const kPre[] = {"", "20% - ", "40% - ", "60% - "};
    return std::string(kPre[v]) + "Accent" + std::to_string(k);
  }
  if (id == 53) return "Explanatory Text";
  return {};
}

// ---------------------------------------------------------------------------
// The converter

struct Cell {
  uint32_t row = 0;
  uint16_t col = 0;
  enum Kind : uint8_t { Blank, Number, Sst, Bool, Error, Str } kind = Blank;
  enum Form : uint8_t { None, Normal, SharedMaster, SharedChild, ArrayMaster } form = None;
  uint32_t xf = 0;  // the BIFF XF index
  double num = 0;
  uint32_t index = 0;    // Sst: string; Bool/Error: the code; Str: into `texts`
  uint32_t formula = 0;  // into `texts`: the formula's text (Normal, masters)
  uint32_t si = 0;       // shared formulas
  uint32_t refText = 0;  // into `texts`: the ref of a shared or array formula's master
  uint32_t order = 0;    // the record's place, so the last of two cells at one place wins
};

struct Link {
  uint32_t r1, c1, r2, c2;
  std::string target, location, display, tooltip;
};

struct Note {
  uint32_t row = 0, col = 0;
  bool shown = false;
  std::string author, text;
};

class Converter {
 public:
  std::vector<uint8_t> Convert(const StreamFn& streams, pkg::Status& status);
  std::wstring password;
  bool stopAtFilepass = false;  // NeedsPassword: only check the password
  bool needsPassword = false;

 private:
  bool Globals(pkg::Status& status);
  void Sst(const Record& r);
  void Xf(const Record& r);
  void XfExt(const Record& r);
  void Name(const Record& r);
  void Formulas();
  std::string SheetXml(const SheetEntry& entry, size_t outIndex, std::string& rels, std::vector<Note>& notes);
  std::string StylesXml();
  std::string FontXml(const FontRec& f, bool rich);
  std::string XfXml(const XfRec& x, bool cell);
  uint32_t AddSstText(std::string text);
  std::string WorkbookXml();

  std::vector<uint8_t> stream_;
  Palette palette_;
  std::vector<FontRec> fonts_;
  std::vector<std::pair<uint16_t, std::string>> formats_;
  std::vector<XfRec> xfs_;
  std::vector<uint32_t> cellXf_;   // BIFF XF index -> cellXfs index
  std::vector<uint32_t> styleXf_;  // BIFF XF index -> cellStyleXfs index (kNone: a cell XF)
  std::vector<uint32_t> cellXfOrder_, styleXfOrder_;
  struct StyleRec {
    uint16_t xf;
    bool builtin;
    uint8_t id, level;
    std::string name;
  };
  std::vector<StyleRec> styles_;
  std::vector<SheetEntry> sheets_;
  std::vector<NameEntry> names_;
  std::vector<SstString> sst_;
  std::vector<std::string> extraStrings_;  // strings that only formulas' values have
  std::unordered_map<std::string, uint32_t> extraIndex_;
  detail::FormulaContext ctx_;
  bool date1904_ = false, fullPrecision_ = true, wbProtect_ = false, r1c1_ = false;
  uint16_t wbPassword_ = 0;
  uint16_t activeTab_ = 0, firstTab_ = 0;
  uint32_t recalcId_ = 0;
  int calcMode_ = 1;
  bool iterate_ = false, calcSeen_ = false;
  uint16_t iterateCount_ = 100;
  double delta_ = 0.001;
  std::map<int, std::array<uint32_t, 4>> filterRanges_;  // by 1-based sheet: _FilterDatabase's area
  std::vector<std::string> dxfs_;  // conditional formats' differential formats, for styles.xml
  std::string DxfXml(Bytes& b);
  std::vector<uint8_t> themeZip_;  // THEME: the workbook's theme as a package (.thmx)
  bool theme2007_ = false;
  std::string NotesParts(const std::vector<Note>& notes, size_t sheet, std::string& comments, std::string& vml);
  std::string ThemeXml();
};

uint32_t Converter::AddSstText(std::string text) {
  auto it = extraIndex_.find(text);
  if (it != extraIndex_.end()) return it->second;
  const uint32_t id = static_cast<uint32_t>(sst_.size() + extraStrings_.size());
  extraIndex_.emplace(text, id);
  extraStrings_.push_back(std::move(text));
  return id;
}

void Converter::Sst(const Record& r) {
  ContinuedText t(r.data, r.breaks);
  t.U32();
  const uint32_t unique = t.U32();
  if (!t.Ok()) return;
  sst_.reserve(std::min<size_t>(unique, r.data.size() / 3 + 1));
  for (uint32_t i = 0; i < unique && t.Ok() && !t.AtEnd(); ++i) {
    SstString s;
    const uint16_t cch = t.U16();
    const uint8_t flags = t.U8();
    uint16_t runs = 0;
    uint32_t ext = 0;
    if (flags & 0x08) runs = t.U16();
    if (flags & 0x04) ext = t.U32();
    if (!t.Ok() || !t.Chars(cch, flags & 1, s.text)) break;
    for (uint16_t k = 0; k < runs && t.Ok(); ++k) {
      const uint16_t ich = t.U16(), ifnt = t.U16();
      s.runs.emplace_back(ich, ifnt);
    }
    t.Skip(ext);
    if (!t.Ok()) break;
    sst_.push_back(std::move(s));
  }
}

void Converter::Xf(const Record& r) {
  Bytes b(r.data);
  XfRec x;
  x.font = b.U16();
  x.fmt = b.U16();
  const uint16_t f = b.U16();
  x.locked = f & 1;
  x.hidden = (f & 2) != 0;
  x.style = (f & 4) != 0;
  x.parent = f >> 4;
  uint8_t a = b.U8();
  x.halign = a & 7;
  x.wrap = (a & 8) != 0;
  x.valign = (a >> 4) & 7;
  x.justLast = (a & 0x80) != 0;
  x.rot = b.U8();
  a = b.U8();
  x.indent = a & 15;
  x.shrink = (a & 0x10) != 0;
  x.readingOrder = (a >> 6) & 3;
  x.used = b.U8();
  const uint32_t b1 = b.U32(), b2 = b.U32();
  const uint16_t fill = b.U16();
  if (!b.Ok()) return;
  x.bl = b1 & 15, x.br = (b1 >> 4) & 15, x.bt = (b1 >> 8) & 15, x.bb = (b1 >> 12) & 15;
  x.cl = (b1 >> 16) & 0x7F, x.cr = (b1 >> 23) & 0x7F, x.diag = (b1 >> 30) & 3;
  x.ct = b2 & 0x7F, x.cb = (b2 >> 7) & 0x7F, x.cd = (b2 >> 14) & 0x7F, x.bd = (b2 >> 21) & 15;
  x.pattern = (b2 >> 26) & 0x3F;
  x.fore = fill & 0x7F, x.back = (fill >> 7) & 0x7F;
  xfs_.push_back(x);
}

// XFEXT ([MS-XLS] 2.4.355): the full colors of an XF, which Excel 2007 and later write besides
// the nearest palette color.
void Converter::XfExt(const Record& r) {
  Bytes b(r.data);
  b.Skip(12 + 2);  // FrtHeader, reserved
  const uint16_t ixfe = b.U16();
  b.Skip(2);
  const uint16_t count = b.U16();
  if (!b.Ok() || ixfe >= xfs_.size()) return;
  XfRec& x = xfs_[ixfe];
  for (uint16_t i = 0; i < count && b.Ok(); ++i) {
    const uint16_t type = b.U16(), cb = b.U16();
    if (cb < 4 || cb - 4 > b.Left()) return;
    Bytes p(b.Take(cb - 4));
    ExtColor* target = nullptr;
    switch (type) {
      case 4: target = &x.extFore; break;
      case 5: target = &x.extBack; break;
      case 7: target = &x.extTop; break;
      case 8: target = &x.extBottom; break;
      case 9: target = &x.extLeft; break;
      case 10: target = &x.extRight; break;
      case 11: target = &x.extDiag; break;
      case 13: target = &x.extText; break;
    }
    if (!target) continue;
    // FullColorExt: xclrType (2), nTintShade (2), xclrValue (4).
    const uint16_t kind = p.U16();
    const int16_t tint = static_cast<int16_t>(p.U16());
    const uint32_t value = p.U32();
    if (!p.Ok()) continue;
    ExtColor c;
    c.tint = tint / 32767.0;
    if (kind == 2) {
      c.kind = ExtColor::Rgb;  // LongRGBA: red, green, blue, alpha
      c.value = (value & 0xFF) << 16 | (value & 0xFF00) | (value >> 16 & 0xFF);
    } else if (kind == 3) {
      c.kind = ExtColor::Theme;
      c.value = value;
    } else {
      continue;
    }
    *target = c;
  }
}

void Converter::Name(const Record& r) {
  Bytes b(r.data);
  NameEntry n;
  const uint16_t flags = b.U16();
  b.U8();
  const uint8_t cch = b.U8();
  const uint16_t cce = b.U16();
  b.Skip(2);
  n.itab = b.U16();
  b.Skip(4);
  n.hidden = flags & 1;
  n.func = (flags & 0x0A) != 0;  // fFunc, fProc
  n.builtin = (flags & 0x20) != 0;
  std::string text;
  if (!detail::ReadChars(b, cch, text)) {
    n.skip = true;
    names_.push_back(std::move(n));
    return;
  }
  if (n.builtin && !text.empty()) {
    n.builtinCode = static_cast<uint8_t>(text[0]);
    const char* bn = BuiltinName(n.builtinCode);
    if (!bn) n.skip = true;
    else n.name = std::string("_xlnm.") + bn;
    // The macro ones aren't names an .xlsx keeps.
    if (n.builtinCode == 1 || n.builtinCode == 2 || n.builtinCode == 8 || n.builtinCode == 9 || n.builtinCode == 10 ||
        n.builtinCode == 11)
      n.skip = true;
  } else {
    n.name = std::move(text);
  }
  const auto rgce = b.Take(cce);
  n.rgce.assign(rgce.begin(), rgce.end());
  const auto rest = b.Rest();
  n.rgcb.assign(rest.begin(), rest.end());
  if (n.name.empty()) n.skip = true;
  names_.push_back(std::move(n));
}

bool Converter::Globals(pkg::Status& status) {
  Records rs(stream_);
  Record r;
  if (!rs.Next(r) || r.type != 0x0809 || r.data.size() < 4) {
    status = pkg::Status::Fail(pkg::Status::Code::Damaged, "it isn't an Excel workbook or it is damaged.");
    return false;
  }
  {
    Bytes b(r.data);
    const uint16_t vers = b.U16(), dt = b.U16();
    if (vers != 0x0600 || dt != 0x0005) {
      status = pkg::Status::Fail(pkg::Status::Code::Unsupported,
                                 "it was saved by Excel 5.0/95 or older, which Featherweight can't open. Save it as a newer Excel workbook first.");
      return false;
    }
  }
  bool inSupBook = false, inTheme = false;
  while (rs.Next(r)) {
    Bytes b(r.data);
    switch (r.type) {
      case 0x000A: goto done;  // EOF
      case 0x002F: {  // FILEPASS: the rest of the stream is encrypted
        const std::wstring_view pw = password.empty() ? std::wstring_view(L"VelvetSweatshop") : std::wstring_view(password);
        const detail::CryptResult cr = detail::DecryptWorkbook(stream_, rs.Start(), stopAtFilepass ? std::wstring_view(L"VelvetSweatshop") : pw);
        if (cr == detail::CryptResult::WrongPassword) {
          needsPassword = true;
          status = pkg::Status::Fail(pkg::Status::Code::Rejected, password.empty() ? "it is protected with a password."
                                                                                  : "the password is wrong.");
          return false;
        }
        if (cr != detail::CryptResult::Ok) {
          status = pkg::Status::Fail(cr == detail::CryptResult::Damaged ? pkg::Status::Code::Damaged : pkg::Status::Code::Unsupported,
                                     cr == detail::CryptResult::Damaged ? "it seems to be damaged."
                                                                        : "it is protected in a way Featherweight doesn't support.");
          return false;
        }
        if (stopAtFilepass) return false;
        break;
      }
      case 0x0022: date1904_ = b.U16() == 1; break;
      case 0x000E: fullPrecision_ = b.U16() != 0; break;
      case 0x0012: wbProtect_ = b.U16() != 0; break;
      case 0x0013: wbPassword_ = b.U16(); break;
      case 0x01C1: b.Skip(4); recalcId_ = b.U32(); break;  // RECALCID
      case 0x003D:                                         // WINDOW1
        b.Skip(10);
        activeTab_ = b.U16();
        firstTab_ = b.U16();
        break;
      case 0x0031: {  // FONT
        FontRec f;
        f.size = b.U16() / 20.0;
        const uint16_t grbit = b.U16();
        f.italic = (grbit & 2) != 0;
        f.strike = (grbit & 8) != 0;
        f.outline = (grbit & 0x10) != 0;
        f.shadow = (grbit & 0x20) != 0;
        f.icv = b.U16();
        f.bold = b.U16() >= 600;
        f.script = b.U16();
        f.underline = b.U8();
        f.family = b.U8();
        f.charset = b.U8();
        b.U8();
        std::string name;
        if (detail::ReadShortString(b, name) && !name.empty()) f.name = std::move(name);
        if (!(f.size > 0 && f.size < 410)) f.size = 10;
        fonts_.push_back(std::move(f));
        break;
      }
      case 0x041E: {  // FORMAT
        const uint16_t id = b.U16();
        std::string code;
        if (detail::ReadString(b, code) && !code.empty()) formats_.emplace_back(id, std::move(code));
        break;
      }
      case 0x00E0: Xf(r); break;
      case 0x087D: XfExt(r); break;
      case 0x0293: {  // STYLE
        StyleRec s{};
        const uint16_t ixfe = b.U16();
        s.xf = ixfe & 0xFFF;
        s.builtin = (ixfe & 0x8000) != 0;
        if (s.builtin) {
          s.id = b.U8();
          s.level = b.U8();
          s.name = StyleName(s.id, s.level);
        } else {
          detail::ReadString(b, s.name);
        }
        if (b.Ok() && !s.name.empty()) styles_.push_back(std::move(s));
        break;
      }
      case 0x0092: {  // PALETTE
        const uint16_t n = b.U16();
        for (uint16_t i = 0; i < n && i < 56; ++i) {
          const uint32_t v = b.U32();
          if (!b.Ok()) break;
          palette_.rgb[i] = (v & 0xFF) << 16 | (v & 0xFF00) | (v >> 16 & 0xFF);
          palette_.custom = true;
        }
        break;
      }
      case 0x0085: {  // BOUNDSHEET8
        SheetEntry s;
        s.pos = b.U32();
        s.state = b.U8() & 3;
        s.type = b.U8();
        if (!detail::ReadShortString(b, s.name)) s.name.clear();
        sheets_.push_back(std::move(s));
        break;
      }
      case 0x00FC: Sst(r); break;
      case 0x0018: Name(r); break;
      case 0x01AE: {  // SUPBOOK
        detail::FormulaContext::SupBook sb;
        b.U16();
        const uint16_t cch = b.U16();
        sb.kind = cch == 0x0401 ? sb.Self : cch == 0x3A01 ? sb.AddIn : sb.External;
        ctx_.supBooks.push_back(std::move(sb));
        inSupBook = true;
        break;
      }
      case 0x0023:  // EXTERNNAME, after its SUPBOOK
        if (inSupBook && !ctx_.supBooks.empty()) {
          b.Skip(6);
          std::string n;
          if (!detail::ReadShortString(b, n)) n.clear();
          ctx_.supBooks.back().names.push_back(std::move(n));
        }
        break;
      case 0x0017: {  // EXTERNSHEET
        const uint16_t n = b.U16();
        for (uint16_t i = 0; i < n && b.Left() >= 6; ++i) {
          detail::FormulaContext::Xti x;
          x.supBook = b.U16();
          x.first = static_cast<int16_t>(b.U16());
          x.last = static_cast<int16_t>(b.U16());
          ctx_.xti.push_back(x);
        }
        break;
      }
    }
    // THEME ([MS-XLS] 2.4.326): a custom theme's package, going on in CONTINUEFRT records.
    if (r.type == 0x0896) {
      b.Skip(12);
      const uint32_t version = b.U32();
      themeZip_.clear();
      if (b.Ok() && version == 0) themeZip_.assign(r.data.begin() + 16, r.data.end());
      theme2007_ = b.Ok() && version == 124226;  // Office 2007's default theme, which Excel then uses
      inTheme = !themeZip_.empty();
    } else if (r.type == 0x0812 && inTheme && r.data.size() > 12 && themeZip_.size() < (16u << 20)) {
      themeZip_.insert(themeZip_.end(), r.data.begin() + 12, r.data.end());
    } else {
      inTheme = false;
    }
    if (r.type != 0x0023 && r.type != 0x01AE) inSupBook = false;
  }
done:
  if (sheets_.empty()) {
    status = pkg::Status::Fail(pkg::Status::Code::Damaged, "it has no sheets or it is damaged.");
    return false;
  }
  return true;
}

// The sheets kept, their names made valid and unique, and the names the formulas refer to.
void Converter::Formulas() {
  int out = 0;
  std::vector<std::string> used;
  for (SheetEntry& s : sheets_) {
    if (s.type != 0 && s.type != 1 && s.type != 2 && s.type != 6) s.type = 0;  // treat unknown as a worksheet
    // Worksheets (and dialog sheets) only: chart sheets, macro sheets and VBA modules are left out.
    if (s.type != 0 || s.pos + 4 > stream_.size()) {
      ctx_.sheets.emplace_back();
      continue;
    }
    std::string n;
    for (char c : s.name) n += (c == '[' || c == ']' || c == ':' || c == '*' || c == '?' || c == '/' || c == '\\') ? '_' : c;
    while (!n.empty() && n.front() == '\'') n.erase(n.begin());
    while (!n.empty() && n.back() == '\'') n.pop_back();
    // At most 31 characters (counted as UTF-16, roughly as code points here), whole UTF-8 sequences.
    size_t chars = 0, cut = n.size();
    for (size_t i = 0; i < n.size(); ++i)
      if ((static_cast<unsigned char>(n[i]) & 0xC0) != 0x80 && ++chars > 31) {
        cut = i;
        break;
      }
    n.resize(cut);
    if (n.empty() || NameEqualsNoCase(n, "History")) n = "Sheet" + std::to_string(out + 1);
    std::string base = n;
    for (int k = 2; std::any_of(used.begin(), used.end(), [&](const std::string& u) { return NameEqualsNoCase(u, n); }); ++k) {
      std::string suffix = " (" + std::to_string(k) + ")";
      n = base.substr(0, std::min(base.size(), 31 - suffix.size())) + suffix;
    }
    used.push_back(n);
    s.name = n;
    s.out = out++;
    ctx_.sheets.push_back(n);
  }
  ctx_.names.clear();
  for (const NameEntry& n : names_) {
    if (n.name.empty()) ctx_.names.emplace_back();
    else ctx_.names.push_back(n.name);
  }
}

std::string Converter::FontXml(const FontRec& f, bool rich) {
  std::string o = rich ? "<rPr>" : "<font>";
  if (f.bold) o += "<b/>";
  if (f.italic) o += "<i/>";
  if (f.strike) o += "<strike/>";
  if (f.outline) o += "<outline/>";
  if (f.shadow) o += "<shadow/>";
  switch (f.underline) {
    case 1: o += "<u/>"; break;
    case 2: o += "<u val=\"double\"/>"; break;
    case 0x21: o += "<u val=\"singleAccounting\"/>"; break;
    case 0x22: o += "<u val=\"doubleAccounting\"/>"; break;
  }
  if (f.script == 1) o += "<vertAlign val=\"superscript\"/>";
  if (f.script == 2) o += "<vertAlign val=\"subscript\"/>";
  o += "<sz";
  AttrNum(o, "val", f.size);
  o += "/>";
  if (f.icv < 64) o += "<color" + palette_.Attrs(f.icv) + "/>";
  o += rich ? "<rFont" : "<name";
  Attr(o, "val", f.name.size() > 31 ? f.name.substr(0, 31) : f.name);
  o += "/>";
  if (f.family > 0 && f.family <= 5) o += "<family val=\"" + std::to_string(f.family) + "\"/>";
  if (f.charset) o += "<charset val=\"" + std::to_string(f.charset) + "\"/>";
  o += rich ? "</rPr>" : "</font>";
  return o;
}

std::string Converter::StylesXml() {
  // Fonts: one per FONT record (BIFF's font index 4 doesn't exist: 5 is the fifth record).
  if (fonts_.empty()) fonts_.emplace_back();
  auto fontIndex = [&](uint16_t ifnt) -> uint32_t {
    const uint32_t i = ifnt < 4 ? ifnt : ifnt == 4 ? 0 : ifnt - 1u;
    return i < fonts_.size() ? i : 0;
  };
  // XFs that change a font's color (XFEXT's text color) get a copy of the font.
  std::vector<std::string> fontXml;
  for (const FontRec& f : fonts_) fontXml.push_back(FontXml(f, false));
  std::map<std::string, uint32_t> fontIds;
  for (size_t i = 0; i < fontXml.size(); ++i) fontIds.emplace(fontXml[i], static_cast<uint32_t>(i));
  if (xfs_.empty()) xfs_.push_back(XfRec{});

  std::vector<std::string> fills = {"<fill><patternFill patternType=\"none\"/></fill>",
                                    "<fill><patternFill patternType=\"gray125\"/></fill>"};
  std::vector<std::string> borders = {"<border><left/><right/><top/><bottom/><diagonal/></border>"};
  std::map<std::string, uint32_t> fillIds{{fills[0], 0}, {fills[1], 1}}, borderIds{{borders[0], 0}};
  static const char* const kPatterns[] = {"none", "solid", "mediumGray", "darkGray", "lightGray", "darkHorizontal",
                                          "darkVertical", "darkDown", "darkUp", "darkGrid", "darkTrellis",
                                          "lightHorizontal", "lightVertical", "lightDown", "lightUp", "lightGrid",
                                          "lightTrellis", "gray125", "gray0625"};
  static const char* const kBorders[] = {"", "thin", "medium", "dashed", "dotted", "thick", "double", "hair",
                                         "mediumDashed", "dashDot", "mediumDashDot", "dashDotDot", "mediumDashDotDot",
                                         "slantDashDot"};
  static const char* const kH[] = {"general", "left", "center", "right", "fill", "justify", "centerContinuous", "distributed"};
  static const char* const kV[] = {"top", "center", "bottom", "justify", "distributed"};

  auto xfXml = [&](const XfRec& x, bool cell, uint32_t xfId) {
    // Font, with XFEXT's text color.
    uint32_t font = fontIndex(x.font);
    if (x.extText.kind != ExtColor::None) {
      FontRec f = fonts_[font];
      std::string xml = FontXml(f, false);
      const size_t at = xml.find("<color");
      std::string color = "<color" + palette_.Attrs(0x7FFF, &x.extText) + "/>";
      if (at != std::string::npos) xml.replace(at, xml.find("/>", at) + 2 - at, color);
      else xml.insert(xml.find("<name"), color);
      auto [it, added] = fontIds.emplace(xml, static_cast<uint32_t>(fontXml.size()));
      if (added) fontXml.push_back(xml);
      font = it->second;
    }
    uint32_t fill = 0;
    if (x.pattern > 0 && x.pattern < 19) {
      std::string f = "<fill><patternFill patternType=\"";
      f += kPatterns[x.pattern];
      f += "\">";
      const std::string fg = palette_.Attrs(x.fore, &x.extFore), bg = palette_.Attrs(x.back, &x.extBack);
      if (!fg.empty()) f += "<fgColor" + fg + "/>";
      if (!bg.empty()) f += "<bgColor" + bg + "/>";
      f += "</patternFill></fill>";
      auto [it, added] = fillIds.emplace(f, static_cast<uint32_t>(fills.size()));
      if (added) fills.push_back(f);
      fill = it->second;
    }
    auto side = [&](const char* tag, uint8_t style, uint8_t icv, const ExtColor& ext) {
      std::string s = "<";
      s += tag;
      if (style == 0 || style > 13) return s + "/>";
      s += " style=\"";
      s += kBorders[style];
      s += "\">";
      const std::string c = palette_.Attrs(icv, &ext);
      s += "<color" + (c.empty() ? std::string(" indexed=\"64\"") : c) + "/>";
      s += "</";
      s += tag;
      return s + ">";
    };
    std::string bx = "<border";
    if (x.bd && (x.diag & 1)) bx += " diagonalDown=\"1\"";
    if (x.bd && (x.diag & 2)) bx += " diagonalUp=\"1\"";
    bx += ">" + side("left", x.bl, x.cl, x.extLeft) + side("right", x.br, x.cr, x.extRight) + side("top", x.bt, x.ct, x.extTop) +
          side("bottom", x.bb, x.cb, x.extBottom) + side("diagonal", x.diag ? x.bd : 0, x.cd, x.extDiag) + "</border>";
    auto [bit, badded] = borderIds.emplace(bx, static_cast<uint32_t>(borders.size()));
    if (badded) borders.push_back(bx);
    const uint32_t border = bit->second;

    std::string o = "<xf";
    Attr(o, "numFmtId", x.fmt);
    Attr(o, "fontId", font);
    Attr(o, "fillId", fill);
    Attr(o, "borderId", border);
    if (cell) Attr(o, "xfId", xfId);
    std::string align;
    if (x.halign && x.halign < 8) align += std::string(" horizontal=\"") + kH[x.halign] + "\"";
    if (x.valign != 2 && x.valign < 5) align += std::string(" vertical=\"") + kV[x.valign] + "\"";
    if (x.rot && (x.rot <= 180 || x.rot == 255)) align += " textRotation=\"" + std::to_string(x.rot) + "\"";
    if (x.wrap) align += " wrapText=\"1\"";
    if (x.indent) align += " indent=\"" + std::to_string(x.indent) + "\"";
    if (x.justLast) align += " justifyLastLine=\"1\"";
    if (x.shrink) align += " shrinkToFit=\"1\"";
    if (x.readingOrder == 1 || x.readingOrder == 2) align += " readingOrder=\"" + std::to_string(x.readingOrder) + "\"";
    const bool prot = !x.locked || x.hidden;
    if (cell) {
      if (x.fmt) o += " applyNumberFormat=\"1\"";
      if (font) o += " applyFont=\"1\"";
      if (fill) o += " applyFill=\"1\"";
      if (border) o += " applyBorder=\"1\"";
      if (!align.empty()) o += " applyAlignment=\"1\"";
      if (prot) o += " applyProtection=\"1\"";
    }
    if (align.empty() && !prot) return o + "/>";
    o += ">";
    if (!align.empty()) o += "<alignment" + align + "/>";
    if (prot) o += std::string("<protection") + (x.locked ? "" : " locked=\"0\"") + (x.hidden ? " hidden=\"1\"" : "") + "/>";
    return o + "</xf>";
  };

  // Style XFs first (the Normal style's is XF 0), then cell XFs with the default one (XF 15)
  // first, so that cells without a format are cellXfs 0.
  cellXf_.assign(xfs_.size(), 0);
  styleXf_.assign(xfs_.size(), kNone);
  std::vector<std::string> styleXml, cellXml;
  for (size_t i = 0; i < xfs_.size(); ++i)
    if (xfs_[i].style) styleXf_[i] = static_cast<uint32_t>(styleXml.size()), styleXml.push_back(xfXml(xfs_[i], false, 0));
  if (styleXml.empty()) styleXml.push_back(xfXml(XfRec{}, false, 0));
  std::vector<size_t> cellOrder;
  if (xfs_.size() > 15 && !xfs_[15].style) cellOrder.push_back(15);
  for (size_t i = 0; i < xfs_.size(); ++i)
    if (!xfs_[i].style && i != (cellOrder.empty() ? SIZE_MAX : 15)) cellOrder.push_back(i);
  for (size_t i : cellOrder) {
    const XfRec& x = xfs_[i];
    const uint32_t parent = x.parent < styleXf_.size() && styleXf_[x.parent] != kNone ? styleXf_[x.parent] : 0;
    cellXf_[i] = static_cast<uint32_t>(cellXml.size());
    cellXml.push_back(xfXml(x, true, parent));
  }
  if (cellXml.empty()) cellXml.push_back(xfXml(XfRec{}, true, 0));

  std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                  "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">";
  std::set<uint16_t> seen;
  std::string nf;
  size_t nfCount = 0;
  for (const auto& [id, code] : formats_) {
    if (!seen.insert(id).second) continue;
    nf += "<numFmt";
    Attr(nf, "numFmtId", id);
    Attr(nf, "formatCode", code);
    nf += "/>";
    ++nfCount;
  }
  if (nfCount) o += "<numFmts count=\"" + std::to_string(nfCount) + "\">" + nf + "</numFmts>";
  o += "<fonts count=\"" + std::to_string(fontXml.size()) + "\">";
  for (const auto& f : fontXml) o += f;
  o += "</fonts><fills count=\"" + std::to_string(fills.size()) + "\">";
  for (const auto& f : fills) o += f;
  o += "</fills><borders count=\"" + std::to_string(borders.size()) + "\">";
  for (const auto& b : borders) o += b;
  o += "</borders><cellStyleXfs count=\"" + std::to_string(styleXml.size()) + "\">";
  for (const auto& x : styleXml) o += x;
  o += "</cellStyleXfs><cellXfs count=\"" + std::to_string(cellXml.size()) + "\">";
  for (const auto& x : cellXml) o += x;
  o += "</cellXfs>";
  // Cell styles: unique names, each on a style XF; Normal always.
  std::vector<std::string> styleNames;
  std::string cs;
  bool normal = false;
  for (const StyleRec& s : styles_) {
    if (s.xf >= styleXf_.size() || styleXf_[s.xf] == kNone) continue;
    if (std::any_of(styleNames.begin(), styleNames.end(), [&](const std::string& n) { return NameEqualsNoCase(n, s.name); }))
      continue;
    if (s.builtin && s.id == 0) {
      if (normal) continue;
      normal = true;
    }
    styleNames.push_back(s.name);
    cs += "<cellStyle";
    Attr(cs, "name", s.name);
    Attr(cs, "xfId", styleXf_[s.xf]);
    if (s.builtin && (s.id <= 53)) {
      Attr(cs, "builtinId", s.id);
      if (s.id == 1 || s.id == 2) Attr(cs, "iLevel", s.level);
    }
    cs += "/>";
  }
  if (!normal && !std::any_of(styleNames.begin(), styleNames.end(), [](const std::string& n) { return NameEqualsNoCase(n, "Normal"); })) {
    cs = "<cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/>" + cs;
    styleNames.push_back("Normal");
  }
  o += "<cellStyles count=\"" + std::to_string(styleNames.size()) + "\">" + cs + "</cellStyles>";
  o += "<dxfs count=\"0\"/><tableStyles count=\"0\" defaultTableStyle=\"TableStyleMedium2\" defaultPivotStyle=\"PivotStyleLight16\"/>";
  if (palette_.custom) {
    o += "<colors><indexedColors>";
    char buf[48];
    for (int i = 0; i < 64; ++i) {
      const uint32_t c = i < 8 ? palette_.rgb[i] : i < 64 ? palette_.rgb[i - 8] : 0;
      std::snprintf(buf, sizeof buf, "<rgbColor rgb=\"FF%06X\"/>", c & 0xFFFFFF);
      o += buf;
    }
    o += "</indexedColors></colors>";
  }
  o += "</styleSheet>";
  return o;
}

std::string Converter::WorkbookXml() {
  std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                  "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
                  "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">";
  o += date1904_ ? "<workbookPr date1904=\"1\"/>" : "<workbookPr/>";
  if (wbProtect_ || wbPassword_) {
    o += "<workbookProtection";
    if (wbPassword_) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "%04X", wbPassword_);
      Attr(o, "workbookPassword", buf);
    }
    if (wbProtect_) o += " lockStructure=\"1\"";
    o += "/>";
  }
  auto outIndex = [&](uint16_t tab) {
    // The BIFF tab's sheet, or the next one kept.
    for (size_t i = tab; i < sheets_.size(); ++i)
      if (sheets_[i].out >= 0) return sheets_[i].out;
    return 0;
  };
  o += "<bookViews><workbookView";
  if (const int a = outIndex(activeTab_)) Attr(o, "activeTab", a);
  if (const int f = outIndex(firstTab_)) Attr(o, "firstSheet", f);
  o += "/></bookViews><sheets>";
  for (const SheetEntry& s : sheets_) {
    if (s.out < 0) continue;
    o += "<sheet";
    Attr(o, "name", s.name);
    Attr(o, "sheetId", s.out + 1);
    if (s.state == 1) o += " state=\"hidden\"";
    if (s.state == 2) o += " state=\"veryHidden\"";
    Attr(o, "r:id", "rId" + std::to_string(s.out + 1));
    o += "/>";
  }
  o += "</sheets>";
  // Defined names: unique by name and scope, in order of name (as Excel writes them).
  std::map<std::pair<std::string, int>, std::string> defined;
  for (const NameEntry& n : names_) {
    if (n.skip || n.func || n.name.empty()) continue;
    if (n.name.size() > 255 || n.name.rfind("_xlfn.", 0) == 0) continue;
    int scope = -1;
    if (n.itab > 0) {
      if (static_cast<size_t>(n.itab) > sheets_.size() || sheets_[n.itab - 1].out < 0) continue;
      scope = sheets_[n.itab - 1].out;
    }
    std::string text;
    if (!detail::Decompile(ctx_, n.rgce, n.rgcb, 0, 0, text)) text = "#REF!";
    std::string key;
    for (char c : n.name) key += static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
    if (defined.count({key, scope})) continue;
    std::string e = "<definedName";
    Attr(e, "name", n.name);
    if (scope >= 0) Attr(e, "localSheetId", scope);
    if (n.hidden) e += " hidden=\"1\"";
    e += ">";
    Esc(e, text);
    e += "</definedName>";
    defined.emplace(std::make_pair(key, scope), e);
  }
  if (!defined.empty()) {
    o += "<definedNames>";
    for (const auto& [k, e] : defined) o += e;
    o += "</definedNames>";
  }
  o += "<calcPr";
  if (recalcId_) Attr(o, "calcId", recalcId_);
  if (calcMode_ == 0) o += " calcMode=\"manual\"";
  if (calcMode_ == -1) o += " calcMode=\"autoNoTable\"";
  if (r1c1_) o += " refMode=\"R1C1\"";
  if (iterate_) {
    o += " iterate=\"1\"";
    Attr(o, "iterateCount", iterateCount_);
    if (std::isfinite(delta_) && delta_ >= 0) AttrNum(o, "iterateDelta", delta_);
  }
  if (!fullPrecision_) o += " fullPrecision=\"0\"";
  o += "/></workbook>";
  return o;
}

// ---------------------------------------------------------------------------
// A sheet

struct RowRec {
  double height = 0;
  bool custom = false, hidden = false, collapsed = false, format = false;
  uint8_t level = 0;
  uint16_t xf = 15;
};

struct ColRec {
  uint32_t first, last;
  double width;
  uint16_t xf;
  bool hidden, custom, bestFit, collapsed;
  uint8_t level;
};

// A hyperlink's string (HyperlinkString: a count of UTF-16 characters with the final null).
bool LinkString(Bytes& b, std::string& out) {
  const uint32_t n = b.U32();
  if (!b.Ok() || n > b.Left() / 2) return false;
  std::vector<uint16_t> u(n);
  for (uint32_t i = 0; i < n; ++i) u[i] = b.U16();
  while (!u.empty() && u.back() == 0) u.pop_back();
  out = Utf8(u);
  return b.Ok();
}

// HLINK ([MS-XLS] 2.4.140, [MS-OSHARED] 2.3.7.1 Hyperlink Object).
bool ReadLink(const Record& r, Link& l) {
  Bytes b(r.data);
  l.r1 = b.U16(), l.r2 = b.U16(), l.c1 = b.U16(), l.c2 = b.U16();
  b.Skip(16 + 4);  // hlinkClsid, streamVersion
  const uint32_t flags = b.U32();
  if (!b.Ok()) return false;
  if (flags & 0x14) {  // hlstmfHasDisplayName
    if (!LinkString(b, l.display)) return false;
  }
  if (flags & 0x80) {  // hlstmfHasFrameName
    std::string frame;
    if (!LinkString(b, frame)) return false;
  }
  if (flags & 0x01) {  // hlstmfHasMoniker
    if (flags & 0x100) {  // hlstmfMonikerSavedAsStr
      if (!LinkString(b, l.target)) return false;
    } else {
      const auto clsid = b.Take(16);
      if (!b.Ok()) return false;
      static const uint8_t kUrl[16] = {0xE0, 0xC9, 0xEA, 0x79, 0xF9, 0xBA, 0xCE, 0x11,
                                       0x8C, 0x82, 0x00, 0xAA, 0x00, 0x4B, 0xA9, 0x0B};
      static const uint8_t kFile[16] = {0x03, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                        0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
      if (std::equal(clsid.begin(), clsid.end(), kUrl)) {
        const uint32_t len = b.U32();
        if (!b.Ok() || len > b.Left()) return false;
        Bytes u(b.Take(len));
        std::vector<uint16_t> units;
        while (u.Left() >= 2) {
          const uint16_t c = u.U16();
          if (c == 0) break;
          units.push_back(c);
        }
        l.target = Utf8(units);
      } else if (std::equal(clsid.begin(), clsid.end(), kFile)) {
        const uint16_t up = b.U16();
        const uint32_t ansiLen = b.U32();
        if (!b.Ok() || ansiLen > b.Left()) return false;
        const auto ansi = b.Take(ansiLen);
        b.Skip(2 + 2 + 16 + 4);
        const uint32_t uniSize = b.U32();
        std::string path;
        if (uniSize > 0 && b.Ok()) {
          const uint32_t bytes = b.U32();
          b.U16();
          if (!b.Ok() || bytes > b.Left()) return false;
          std::vector<uint16_t> u(bytes / 2);
          for (auto& c : u) c = b.U16();
          path = Utf8(u);
        } else {
          for (uint8_t c : ansi)
            if (c) detail::AppendCodeUnit(path, c);
        }
        std::string prefix;
        for (uint16_t i = 0; i < up && i < 64; ++i) prefix += "../";
        l.target = prefix + path;
      } else {
        return false;  // another moniker: not written
      }
    }
  }
  if (flags & 0x08) {  // hlstmfHasLocationStr
    if (!LinkString(b, l.location)) return false;
  }
  return b.Ok() && (!l.target.empty() || !l.location.empty());
}

// A target as a relationship's URI: characters a URI can't hold escaped.
std::string UriEscape(std::string_view s) {
  std::string o;
  static const char kHex[] = "0123456789ABCDEF";
  for (unsigned char c : s) {
    if (c <= 0x20 || c >= 0x7F || c == '"' || c == '<' || c == '>' || c == '^' || c == '`' || c == '{' || c == '|' ||
        c == '}') {
      o += '%';
      o += kHex[c >> 4];
      o += kHex[c & 15];
    } else {
      o += static_cast<char>(c);
    }
  }
  return o;
}

std::string Converter::SheetXml(const SheetEntry& entry, size_t outIndex, std::string& rels, std::vector<Note>& notes) {
  std::vector<Cell> cells;
  std::vector<std::string> texts(1);
  std::map<uint32_t, RowRec> rows;
  std::vector<ColRec> cols;
  std::vector<std::array<uint32_t, 4>> merges;
  std::vector<Link> links;
  std::vector<std::pair<uint32_t, std::string>> tips;  // (row << 16 | col) of the link's first cell
  std::vector<uint32_t> rowBreaks, colBreaks;
  std::map<uint16_t, std::string> noteText;  // TXO text by OBJ id
  uint16_t lastObjId = 0;
  bool lastObjNote = false;
  std::string header, footer;
  double margins[4] = {0.75, 0.75, 1, 1};  // left, right, top, bottom (BIFF8's defaults)
  bool marginSet[4] = {};
  double headerMargin = 0.5, footerMargin = 0.5;
  bool hCenter = false, vCenter = false, gridlines = false, headings = false;
  struct Setup {
    bool present = false;
    uint16_t paper = 0, scale = 100, start = 1, fitW = 1, fitH = 1, grbit = 0, hres = 0, vres = 0, copies = 1;
  } setup;
  uint16_t wsBool = 0x04C1;
  bool wsBoolSeen = false;
  double defaultHeight = 12.75;
  uint16_t defaultHeightFlags = 0;
  uint16_t defColWidth = 8;
  uint16_t window2 = 0x06B6, topRow = 0, leftCol = 0, gridColor = 64, scaleSlv = 0, scaleNormal = 0;
  bool window2Seen = false;
  uint16_t zoomNum = 0, zoomDen = 0;
  struct Pane {
    bool present = false;
    uint16_t x = 0, y = 0, top = 0, left = 0;
    uint8_t active = 3;
  } pane;
  struct Sel {
    uint8_t pane = 3;
    uint16_t row = 0, col = 0;
    std::vector<std::array<uint32_t, 4>> refs;
  };
  std::vector<Sel> sels;
  bool protect = false, objects = false, scenarios = false;
  uint16_t sheetPassword = 0;
  ExtColor tab;
  int tabIcv = -1;
  bool autoFilter = false;
  // Conditional formats (CONDFMT and its CF records) and validations (DV), as xlsx elements.
  std::string condFormats, validations;
  std::string condRange;      // the current CONDFMT's sqref
  uint32_t condRow = 0, condCol = 0;  // its first range's top-left: relative references count from it
  int condLeft = 0;           // CF records still to come for it
  std::string condRules;      // its rules written so far
  auto flushCond = [&] {
    if (!condRange.empty() && !condRules.empty())
      condFormats += "<conditionalFormatting sqref=\"" + condRange + "\">" + condRules + "</conditionalFormatting>";
    condRange.clear();
    condRules.clear();
    condLeft = 0;
  };
  int priority = 0;
  size_t validationCount = 0;
  // A SqRefU (a count, then Ref8U ranges) as an sqref; the first range's top-left in (row, col).
  auto readSqref = [](Bytes& b, uint32_t& row, uint32_t& col) {
    const uint16_t n = b.U16();
    std::string sq;
    for (uint16_t i = 0; i < n && b.Left() >= 8; ++i) {
      const uint32_t r1 = b.U16(), r2 = b.U16(), c1 = b.U16(), c2 = b.U16();
      if (r1 > r2 || c1 > c2 || c2 > 255) continue;
      if (sq.empty()) row = r1, col = c1;
      else sq += ' ';
      sq += RangeRef(r1, c1, r2, c2);
    }
    return sq;
  };

  // Formula masters: (row << 8 | col) -> what PtgExp in other cells refers to.
  struct Master {
    enum Kind : uint8_t { Pending, Shared, Array, Table, Failed } kind = Pending;
    uint32_t si = 0;
    std::vector<uint8_t> rgce, rgcb;
    uint32_t r1 = 0, r2 = 0, c1 = 0, c2 = 0;
    bool topLeft = false;
  };
  std::unordered_map<uint32_t, Master> masters;
  uint32_t nextSi = 0;
  int64_t pendingMaster = -1, pendingString = -1;
  uint32_t order = 0;

  auto cellAt = [&](uint32_t row, uint32_t col, uint16_t xf) -> Cell& {
    cells.emplace_back();
    Cell& c = cells.back();
    c.row = row;
    c.col = static_cast<uint16_t>(col);
    c.xf = xf;
    c.order = order++;
    return c;
  };
  auto setValue = [&](Cell& c, const uint8_t* v) {
    // FormulaValue: a double, or (fExprO 0xFFFF) a string, boolean, error or empty string.
    if (v[6] == 0xFF && v[7] == 0xFF) {
      switch (v[0]) {
        case 0: c.kind = Cell::Str; c.index = static_cast<uint32_t>(texts.size()); texts.emplace_back(); break;
        case 1: c.kind = Cell::Bool; c.index = v[2] != 0; break;
        case 2: c.kind = Cell::Error; c.index = v[2]; break;
        case 3: c.kind = Cell::Str; c.index = static_cast<uint32_t>(texts.size()); texts.emplace_back(); break;
        default: c.kind = Cell::Blank;
      }
      return v[0] == 0;
    }
    double d;
    std::memcpy(&d, v, 8);
    if (std::isfinite(d)) c.kind = Cell::Number, c.num = d;
    else c.kind = Cell::Error, c.index = 0x24;
    return false;
  };

  Records rs(stream_, entry.pos);
  Record r;
  int depth = 0;
  while (rs.Next(r)) {
    if (cells.size() > kMaxCells) break;
    Bytes b(r.data);
    if (r.type == 0x0809) {  // BOF: this sheet's, or a chart's inside it
      ++depth;
      continue;
    }
    if (r.type == 0x000A) {
      if (--depth <= 0) break;
      continue;
    }
    if (depth != 1) continue;
    switch (r.type) {
      case 0x0203: {  // NUMBER
        const uint16_t row = b.U16(), col = b.U16(), xf = b.U16();
        const double d = b.F64();
        if (!b.Ok()) break;
        Cell& c = cellAt(row, col, xf);
        if (std::isfinite(d)) c.kind = Cell::Number, c.num = d;
        else c.kind = Cell::Error, c.index = 0x24;
        break;
      }
      case 0x027E: case 0x00BD: {  // RK, MULRK
        const uint16_t row = b.U16();
        uint16_t col = b.U16();
        const size_t count = r.type == 0x027E ? 1 : (b.Left() >= 2 ? (b.Left() - 2) / 6 : 0);
        for (size_t i = 0; i < count && b.Left() >= 6; ++i, ++col) {
          const uint16_t xf = b.U16();
          const uint32_t rk = b.U32();
          double d;
          if (rk & 2) {
            d = static_cast<double>(static_cast<int32_t>(rk) >> 2);
          } else {
            const uint64_t bits = uint64_t{rk & 0xFFFFFFFC} << 32;
            std::memcpy(&d, &bits, 8);
          }
          if (rk & 1) d /= 100;
          Cell& c = cellAt(row, col, xf);
          c.kind = Cell::Number;
          c.num = d;
        }
        break;
      }
      case 0x00FD: {  // LABELSST
        const uint16_t row = b.U16(), col = b.U16(), xf = b.U16();
        const uint32_t isst = b.U32();
        if (!b.Ok()) break;
        Cell& c = cellAt(row, col, xf);
        if (isst < sst_.size()) c.kind = Cell::Sst, c.index = isst;
        break;
      }
      case 0x0204: case 0x00D6: {  // LABEL, RSTRING
        const uint16_t row = b.U16(), col = b.U16(), xf = b.U16();
        std::string s;
        if (!b.Ok() || !detail::ReadString(b, s)) break;
        Cell& c = cellAt(row, col, xf);
        c.kind = Cell::Sst;
        c.index = AddSstText(std::move(s));
        break;
      }
      case 0x0205: {  // BOOLERR
        const uint16_t row = b.U16(), col = b.U16(), xf = b.U16();
        const uint8_t v = b.U8(), isErr = b.U8();
        if (!b.Ok()) break;
        Cell& c = cellAt(row, col, xf);
        c.kind = isErr ? Cell::Error : Cell::Bool;
        c.index = isErr ? v : (v != 0);
        break;
      }
      case 0x0201: {  // BLANK
        const uint16_t row = b.U16(), col = b.U16(), xf = b.U16();
        if (b.Ok()) cellAt(row, col, xf);
        break;
      }
      case 0x00BE: {  // MULBLANK
        const uint16_t row = b.U16();
        uint16_t col = b.U16();
        while (b.Left() >= 4) {
          const uint16_t xf = b.U16();
          cellAt(row, col++, xf);
        }
        break;
      }
      case 0x0006: {  // FORMULA
        const uint16_t row = b.U16(), col = b.U16(), xf = b.U16();
        const auto value = b.Take(8);
        b.U16();  // grbit
        b.Skip(4);
        const uint16_t cce = b.U16();
        if (!b.Ok()) break;
        const auto rgce = b.Take(cce);
        const auto rgcb = b.Rest();
        Cell& c = cellAt(row, col, xf);
        const bool str = setValue(c, value.data());
        pendingString = str ? static_cast<int64_t>(cells.size() - 1) : -1;
        pendingMaster = -1;
        if (rgce.size() == 5 && (rgce[0] == 0x01 || rgce[0] == 0x02)) {  // PtgExp, PtgTbl
          const uint32_t mr = rgce[1] | rgce[2] << 8, mc = rgce[3] | rgce[4] << 8;
          const uint32_t key = mr << 8 | (mc & 0xFF);
          if (rgce[0] == 0x02) break;  // a data table's cell: its value only
          if (mr == row && mc == col) {
            pendingMaster = static_cast<int64_t>(cells.size() - 1);
            masters[key];
            break;
          }
          auto it = masters.find(key);
          if (it == masters.end()) break;
          const Master& m = it->second;
          if (m.kind == Master::Shared) {
            if (m.topLeft && row >= m.r1 && row <= m.r2 && col >= m.c1 && col <= m.c2) {
              c.form = Cell::SharedChild;
              c.si = m.si;
            } else {
              std::string text;
              if (detail::Decompile(ctx_, m.rgce, m.rgcb, row, col, text)) {
                c.form = Cell::Normal;
                c.formula = static_cast<uint32_t>(texts.size());
                texts.push_back(std::move(text));
              }
            }
          }
          break;
        }
        std::string text;
        if (detail::Decompile(ctx_, rgce, rgcb, row, col, text)) {
          c.form = Cell::Normal;
          c.formula = static_cast<uint32_t>(texts.size());
          texts.push_back(std::move(text));
        }
        break;
      }
      case 0x04BC: case 0x0221: case 0x0236: {  // SHRFMLA, ARRAY, TABLE: after their master's FORMULA
        if (pendingMaster < 0) break;
        Cell& c = cells[pendingMaster];
        Master& m = masters[uint32_t{c.row} << 8 | (c.col & 0xFF)];
        pendingMaster = -1;
        m.r1 = b.U16(), m.r2 = b.U16(), m.c1 = b.U8(), m.c2 = b.U8();
        if (r.type == 0x0236) {
          m.kind = Master::Table;
          break;
        }
        if (r.type == 0x04BC) b.Skip(2);  // reserved, cUse
        else b.Skip(6);                    // grbit, chn
        const uint16_t cce = b.U16();
        if (!b.Ok() || m.r1 > m.r2 || m.c1 > m.c2) {
          m.kind = Master::Failed;
          break;
        }
        const auto rgce = b.Take(cce);
        const auto rgcb = b.Rest();
        std::string text;
        if (!detail::Decompile(ctx_, rgce, rgcb, c.row, c.col, text)) {
          m.kind = Master::Failed;
          break;
        }
        c.formula = static_cast<uint32_t>(texts.size());
        texts.push_back(std::move(text));
        c.refText = static_cast<uint32_t>(texts.size());
        texts.push_back(RangeRef(m.r1, m.c1, m.r2, m.c2));
        if (r.type == 0x0221) {
          m.kind = Master::Array;
          c.form = Cell::ArrayMaster;
        } else {
          m.kind = Master::Shared;
          m.rgce.assign(rgce.begin(), rgce.end());
          m.rgcb.assign(rgcb.begin(), rgcb.end());
          m.topLeft = m.r1 == c.row && m.c1 == c.col;
          if (m.topLeft) {
            m.si = nextSi++;
            c.form = Cell::SharedMaster;
            c.si = m.si;
          } else {
            c.form = Cell::Normal;
          }
        }
        break;
      }
      case 0x0207: {  // STRING: a formula's text value
        if (pendingString < 0) break;
        std::string s;
        if (detail::ReadString(b, s)) texts[cells[pendingString].index] = std::move(s);
        pendingString = -1;
        break;
      }
      case 0x0208: {  // ROW
        const uint16_t row = b.U16();
        b.Skip(4);
        const uint16_t height = b.U16();
        b.Skip(4);
        const uint16_t f = b.U16(), f2 = b.U16();
        if (!b.Ok()) break;
        RowRec& rr = rows[row];
        rr.height = (height & 0x7FFF) / 20.0;
        rr.level = f & 7;
        rr.collapsed = (f & 0x10) != 0;
        rr.hidden = (f & 0x20) != 0;
        rr.custom = (f & 0x40) != 0;
        rr.format = (f & 0x80) != 0;
        rr.xf = f2 & 0xFFF;
        break;
      }
      case 0x007D: {  // COLINFO
        ColRec c{};
        c.first = b.U16();
        c.last = b.U16();
        c.width = b.U16() / 256.0;
        c.xf = b.U16();
        const uint16_t f = b.U16();
        if (!b.Ok() || c.first > c.last || c.first > 255) break;
        c.last = std::min<uint32_t>(c.last, 255);
        c.hidden = f & 1;
        c.custom = (f & 2) != 0;
        c.bestFit = (f & 4) != 0;
        c.level = (f >> 8) & 7;
        c.collapsed = (f & 0x1000) != 0;
        cols.push_back(c);
        break;
      }
      case 0x0055: defColWidth = b.U16(); break;
      case 0x0225: {  // DEFAULTROWHEIGHT
        defaultHeightFlags = b.U16();
        const uint16_t h = b.U16();
        if (b.Ok() && h > 0 && h < 8192) defaultHeight = h / 20.0;
        break;
      }
      case 0x0081: wsBool = b.U16(); wsBoolSeen = b.Ok(); break;
      case 0x00E5: {  // MERGECELLS
        const uint16_t n = b.U16();
        for (uint16_t i = 0; i < n && b.Left() >= 8; ++i) {
          const uint32_t r1 = b.U16(), r2 = b.U16(), c1 = b.U16(), c2 = b.U16();
          if (r1 <= r2 && c1 <= c2 && c2 < kMaxCols && (r1 != r2 || c1 != c2)) merges.push_back({r1, c1, r2, c2});
        }
        break;
      }
      case 0x023E: {  // WINDOW2
        window2 = b.U16();
        topRow = b.U16();
        leftCol = b.U16();
        gridColor = b.U16();
        b.Skip(2);
        scaleSlv = b.U16();
        scaleNormal = b.U16();
        window2Seen = true;
        break;
      }
      case 0x00A0: zoomNum = b.U16(); zoomDen = b.U16(); break;
      case 0x0041: {  // PANE
        pane.x = b.U16(), pane.y = b.U16(), pane.top = b.U16(), pane.left = b.U16();
        pane.active = b.U8() & 3;
        pane.present = r.data.size() >= 9;
        break;
      }
      case 0x001D: {  // SELECTION
        Sel s;
        s.pane = b.U8() & 3;
        s.row = b.U16();
        s.col = b.U16();
        b.U16();
        const uint16_t n = b.U16();
        for (uint16_t i = 0; i < n && b.Left() >= 6; ++i) {
          const uint32_t r1 = b.U16(), r2 = b.U16(), c1 = b.U8(), c2 = b.U8();
          if (r1 <= r2 && c1 <= c2) s.refs.push_back({r1, c1, r2, c2});
        }
        if (b.Ok() || !s.refs.empty()) sels.push_back(std::move(s));
        break;
      }
      case 0x01B8: {  // HLINK
        Link l;
        if (ReadLink(r, l) && l.r1 <= l.r2 && l.c1 <= l.c2 && l.c2 < 256) links.push_back(std::move(l));
        break;
      }
      case 0x0800: {  // HLINKTOOLTIP
        b.Skip(2);
        const uint32_t row = b.U16();
        b.Skip(2);
        const uint32_t col = b.U16();
        b.Skip(2);
        std::vector<uint16_t> u;
        while (b.Left() >= 2) {
          const uint16_t c = b.U16();
          if (c == 0) break;
          u.push_back(c);
        }
        if (b.Ok()) tips.emplace_back(row << 16 | col, Utf8(u));
        break;
      }
      case 0x005D: {  // OBJ: its first subrecord, ftCmo, gives the kind and the id
        const uint16_t ft = b.U16(), cb = b.U16(), ot = b.U16(), id = b.U16();
        lastObjNote = b.Ok() && ft == 0x15 && cb >= 4 && ot == 0x19;
        lastObjId = id;
        break;
      }
      case 0x01B6: {  // TXO: a note's text in its CONTINUE records
        if (!lastObjNote) break;
        b.Skip(10);
        const uint16_t cch = b.U16();
        if (!b.Ok() || r.breaks.empty() || cch == 0) break;
        ContinuedText t(r.data, r.breaks);
        t.Skip(r.breaks[0]);
        const bool high = t.U8() & 1;
        std::vector<uint16_t> u;
        if (t.Ok() && t.Chars(cch, high, u)) noteText[lastObjId] = Utf8(u);
        lastObjNote = false;
        break;
      }
      case 0x001C: {  // NOTE
        Note n;
        n.row = b.U16();
        n.col = b.U16();
        const uint16_t f = b.U16();
        const uint16_t id = b.U16();
        n.shown = (f & 2) != 0;
        if (!detail::ReadString(b, n.author)) n.author.clear();
        auto it = noteText.find(id);
        if (b.Ok() || !n.author.empty()) {
          if (it != noteText.end()) n.text = it->second;
          if (n.col < 256 && n.row < 65536) notes.push_back(std::move(n));
        }
        break;
      }
      case 0x0014: header.clear(); if (r.data.size() > 2) detail::ReadString(b, header); break;
      case 0x0015: footer.clear(); if (r.data.size() > 2) detail::ReadString(b, footer); break;
      case 0x0083: hCenter = b.U16() != 0; break;
      case 0x0084: vCenter = b.U16() != 0; break;
      case 0x002B: gridlines = b.U16() != 0; break;
      case 0x002A: headings = b.U16() != 0; break;
      case 0x0026: case 0x0027: case 0x0028: case 0x0029: {
        const double v = b.F64();
        if (b.Ok() && std::isfinite(v) && v >= 0 && v < 50) margins[r.type - 0x26] = v, marginSet[r.type - 0x26] = true;
        break;
      }
      case 0x00A1: {  // SETUP
        setup.paper = b.U16();
        setup.scale = b.U16();
        setup.start = b.U16();
        setup.fitW = b.U16();
        setup.fitH = b.U16();
        setup.grbit = b.U16();
        setup.hres = b.U16();
        setup.vres = b.U16();
        const double h = b.F64(), f = b.F64();
        setup.copies = b.U16();
        if (!b.Ok()) break;
        setup.present = true;
        if (std::isfinite(h) && h >= 0 && h < 50) headerMargin = h;
        if (std::isfinite(f) && f >= 0 && f < 50) footerMargin = f;
        break;
      }
      case 0x001B: case 0x001A: {  // HORIZONTALPAGEBREAKS, VERTICALPAGEBREAKS
        const uint16_t n = b.U16();
        for (uint16_t i = 0; i < n && b.Left() >= 6; ++i) {
          const uint16_t at = b.U16();
          b.Skip(4);
          (r.type == 0x001B ? rowBreaks : colBreaks).push_back(at);
        }
        break;
      }
      case 0x0012: protect = b.U16() != 0; break;
      case 0x0013: sheetPassword = b.U16(); break;
      case 0x0063: objects = b.U16() != 0; break;
      case 0x00DD: scenarios = b.U16() != 0; break;
      case 0x009D: autoFilter = true; break;  // AUTOFILTERINFO
      case 0x01B0: {  // CONDFMT: the ranges of the CF records that follow
        const uint16_t count = b.U16();
        b.Skip(2 + 8);  // nID, refBound
        flushCond();
        condRange = readSqref(b, condRow, condCol);
        condLeft = condRange.empty() ? 0 : count;
        break;
      }
      case 0x01B1: {  // CF: one rule
        if (condLeft <= 0) break;
        --condLeft;
        const uint8_t ct = b.U8(), cp = b.U8();
        const uint16_t cce1 = b.U16(), cce2 = b.U16();
        std::string dxf = DxfXml(b);
        if (!b.Ok() || (ct != 1 && ct != 2) || (ct == 1 && (cp < 1 || cp > 8))) break;
        const auto rgce1 = b.Take(cce1), rgce2 = b.Take(cce2);
        std::string f1, f2;
        if (!b.Ok() || !detail::Decompile(ctx_, rgce1, {}, condRow, condCol, f1)) break;
        if (cce2 && !detail::Decompile(ctx_, rgce2, {}, condRow, condCol, f2)) break;
        static const char* const kOps[] = {"", "between", "notBetween", "equal", "notEqual", "greaterThan", "lessThan",
                                           "greaterThanOrEqual", "lessThanOrEqual"};
        condRules += "<cfRule type=\"" + std::string(ct == 1 ? "cellIs" : "expression") + "\" dxfId=\"" +
                       std::to_string(dxfs_.size()) + "\" priority=\"" + std::to_string(++priority) + "\"";
        if (ct == 1) condRules += std::string(" operator=\"") + kOps[cp] + "\"";
        condRules += "><formula>" + Esc(f1) + "</formula>";
        if (ct == 1 && (cp == 1 || cp == 2) && !f2.empty()) condRules += "<formula>" + Esc(f2) + "</formula>";
        condRules += "</cfRule>";
        dxfs_.push_back(std::move(dxf));
        break;
      }
      case 0x0879: {  // CONDFMT12: the ranges of the CF12 records that follow
        b.Skip(12);     // FrtRefHeaderU
        const uint16_t count = b.U16();
        b.Skip(2 + 8);
        flushCond();
        condRange = readSqref(b, condRow, condCol);
        condLeft = condRange.empty() ? 0 : count;
        break;
      }
      case 0x087A: {  // CF12: color scales, data bars and icon sets (other kinds come as CF records too)
        if (condLeft <= 0) break;
        --condLeft;
        b.Skip(12);  // FrtRefHeader
        const uint8_t ct = b.U8();
        b.U8();
        const uint16_t cce1 = b.U16(), cce2 = b.U16();
        const uint32_t cbDxf = b.U32();
        b.Skip(cbDxf ? cbDxf : 2);
        b.Skip(cce1);
        b.Skip(cce2);
        b.Skip(b.U16());  // fmlaActive
        b.Skip(1 + 2 + 2);  // flags, ipriority, icfTemplate
        b.U8();      // cbTemplateParm
        b.Skip(16);  // rgbTemplateParms, always 16 bytes
        if (!b.Ok() || (ct != 3 && ct != 4 && ct != 6)) break;
        b.Skip(3);  // unused, reserved
        // A CFVO: its type, a formula, and a number when there's neither formula nor min/max.
        auto cfvo = [&](std::string& out) {
          static const char* const kTypes[] = {"", "num", "min", "max", "percent", "percentile", "", "formula"};
          const uint8_t type = b.U8();
          const uint16_t cce = b.U16();
          const auto rgce = b.Take(cce);
          if (type == 0 || type == 6 || type > 7) return false;
          out = std::string("<cfvo type=\"") + kTypes[type] + "\"";
          if (cce) {
            std::string f;
            if (!detail::Decompile(ctx_, rgce, {}, condRow, condCol, f)) return false;
            out += " val=\"" + Esc(f) + "\"";
          } else if (type != 2 && type != 3) {
            const double v = b.F64();
            if (!std::isfinite(v)) return false;
            out += " val=\"";
            AppendNumber(out, v);
            out += "\"";
          }
          out += "/>";
          return b.Ok();
        };
        // A CFColor: kind, value, tint.
        auto cfColor = [&]() {
          const uint32_t kind = b.U32(), value = b.U32();
          const double tint = b.F64();
          ExtColor c;
          if (kind == 2) c.kind = ExtColor::Rgb, c.value = (value & 0xFF) << 16 | (value & 0xFF00) | (value >> 16 & 0xFF);
          if (kind == 3) c.kind = ExtColor::Theme, c.value = value, c.tint = std::isfinite(tint) ? tint : 0;
          std::string a = palette_.Attrs(kind == 1 ? value : 0x7FFF, &c);
          return "<color" + (a.empty() ? std::string(" rgb=\"FF000000\"") : a) + "/>";
        };
        std::string rule;
        if (ct == 3) {  // CFGradient
          const uint8_t interp = b.U8(), curves = b.U8();
          b.U8();
          if (interp < 2 || interp > 3 || curves != interp) break;
          std::string v, colors;
          bool ok = true;
          for (uint8_t i = 0; i < interp && ok; ++i) {
            std::string one;
            ok = cfvo(one);
            b.Skip(8);  // numDomain
            v += one;
          }
          for (uint8_t i = 0; i < curves && ok; ++i) {
            b.Skip(8);  // numGrange
            colors += cfColor();
          }
          if (!ok || !b.Ok()) break;
          rule = "<cfRule type=\"colorScale\" priority=\"" + std::to_string(++priority) + "\"><colorScale>" + v + colors +
                 "</colorScale></cfRule>";
        } else if (ct == 4) {  // CFDatabar
          const uint8_t flags = b.U8(), minLength = b.U8(), maxLength = b.U8();
          const std::string color = cfColor();
          std::string lo, hi;
          if (!cfvo(lo) || !cfvo(hi) || !b.Ok()) break;
          rule = "<cfRule type=\"dataBar\" priority=\"" + std::to_string(++priority) + "\"><dataBar";
          if (flags & 2) rule += " showValue=\"0\"";  // Excel writes 0 for values shown
          if (minLength != 10 && minLength <= 100) rule += " minLength=\"" + std::to_string(minLength) + "\"";
          if (maxLength != 90 && maxLength <= 100) rule += " maxLength=\"" + std::to_string(maxLength) + "\"";
          rule += ">" + lo + hi + color + "</dataBar></cfRule>";
        } else {  // CFMultistate
          static const char* const kSets[] = {"3Arrows", "3ArrowsGray", "3Flags", "3TrafficLights1", "3TrafficLights2",
                                              "3Signs", "3Symbols", "3Symbols2", "4Arrows", "4ArrowsGray", "4RedToBlack",
                                              "4Rating", "4TrafficLights", "5Arrows", "5ArrowsGray", "5Rating", "5Quarters"};
          const uint8_t states = b.U8(), set = b.U8(), flags = b.U8();
          if (set > 16 || states < 3 || states > 5 || states != kSets[set][0] - '0') break;
          std::string v;
          bool ok = true;
          for (uint8_t i = 0; i < states && ok; ++i) {
            std::string one;
            ok = cfvo(one);
            const uint8_t equal = b.U8();
            b.Skip(4);
            if (ok && !equal) one.insert(one.size() - 2, " gte=\"0\"");
            v += one;
          }
          if (!ok || !b.Ok()) break;
          rule = "<cfRule type=\"iconSet\" priority=\"" + std::to_string(++priority) + "\"><iconSet iconSet=\"" + kSets[set] + "\"";
          if (flags & 1) rule += " showValue=\"0\"";
          if (flags & 4) rule += " reverse=\"1\"";
          rule += ">" + v + "</iconSet></cfRule>";
        }
        condRules += rule;
        break;
      }
      case 0x01BE: {  // DV: a validation
        const uint32_t flags = b.U32();
        std::string text[4];  // prompt title, error title, prompt, error
        for (auto& t : text) {
          if (!detail::ReadString(b, t)) break;
          if (t == std::string(1, '\0')) t.clear();
        }
        std::string formula[2];
        std::span<const uint8_t> rgce[2];
        for (auto& g : rgce) {
          const uint16_t cce = b.U16();
          b.Skip(2);
          g = b.Take(cce);
        }
        uint32_t row = 0, col = 0;
        const std::string sq = readSqref(b, row, col);
        if (!b.Ok() || sq.empty() || validationCount >= 65534) break;
        const uint32_t type = flags & 15, op = (flags >> 20) & 15;
        if (type > 7) break;
        bool ok = true;
        for (int i = 0; i < 2 && ok; ++i)
          if (!rgce[i].empty()) ok = detail::Decompile(ctx_, rgce[i], {}, row, col, formula[i]);
        if (!ok) break;
        if (type == 3) std::replace(formula[0].begin(), formula[0].end(), '\0', ',');  // an explicit list: "a\0b"
        static const char* const kTypes[] = {"none", "whole", "decimal", "list", "date", "time", "textLength", "custom"};
        static const char* const kStyles[] = {"stop", "warning", "information"};
        static const char* const kOps[] = {"between", "notBetween", "equal", "notEqual", "greaterThan", "lessThan",
                                           "greaterThanOrEqual", "lessThanOrEqual"};
        std::string v = "<dataValidation";
        if (type) v += std::string(" type=\"") + kTypes[type] + "\"";
        const uint32_t style = (flags >> 4) & 7;
        if (style == 1 || style == 2) v += std::string(" errorStyle=\"") + kStyles[style] + "\"";
        if (op && op < 8 && type != 0 && type != 3 && type != 7) v += std::string(" operator=\"") + kOps[op] + "\"";
        if (flags & 0x100) v += " allowBlank=\"1\"";
        if (flags & 0x200) v += " showDropDown=\"1\"";  // xlsx's name for hiding the arrow
        if (flags & 0x40000) v += " showInputMessage=\"1\"";
        if (flags & 0x80000) v += " showErrorMessage=\"1\"";
        if (!text[1].empty()) Attr(v, "errorTitle", text[1].substr(0, 32));
        if (!text[3].empty()) Attr(v, "error", text[3].substr(0, 255));
        if (!text[0].empty()) Attr(v, "promptTitle", text[0].substr(0, 32));
        if (!text[2].empty()) Attr(v, "prompt", text[2].substr(0, 255));
        v += " sqref=\"" + sq + "\">";
        if (!formula[0].empty()) v += "<formula1>" + Esc(formula[0]) + "</formula1>";
        if (!formula[1].empty()) v += "<formula2>" + Esc(formula[1]) + "</formula2>";
        validations += v + "</dataValidation>";
        ++validationCount;
        break;
      }
      case 0x0862: {                          // SHEETEXT: the tab's color
        b.Skip(12);
        const uint32_t cb = b.U32();
        const uint32_t icv = b.U32() & 0x7F;
        if (!b.Ok()) break;
        if (icv < 64) tabIcv = static_cast<int>(icv);
        if (cb >= 0x28 && b.Left() >= 4 + 16) {
          b.Skip(4);
          const uint32_t kind = b.U32(), value = b.U32();
          const double tint = b.F64();
          if (kind == 2) tab.kind = ExtColor::Rgb, tab.value = (value & 0xFF) << 16 | (value & 0xFF00) | (value >> 16 & 0xFF);
          if (kind == 3) tab.kind = ExtColor::Theme, tab.value = value, tab.tint = std::isfinite(tint) ? tint : 0;
        }
        break;
      }
      case 0x000D:  // CALCMODE, in the first sheet
        if (!calcSeen_) calcMode_ = static_cast<int16_t>(b.U16());
        break;
      case 0x0011: if (!calcSeen_) iterate_ = b.U16() != 0; break;
      case 0x000C: if (!calcSeen_) iterateCount_ = b.U16(); break;
      case 0x0010: if (!calcSeen_) delta_ = b.F64(); break;
      case 0x000F: if (!calcSeen_) r1c1_ = b.U16() == 0; break;
    }
  }
  calcSeen_ = true;

  // Cells in order; of two at one place, the later record.
  std::stable_sort(cells.begin(), cells.end(), [](const Cell& a, const Cell& b) {
    return a.row != b.row ? a.row < b.row : a.col != b.col ? a.col < b.col : a.order < b.order;
  });
  std::vector<Cell> unique;
  unique.reserve(cells.size());
  for (Cell& c : cells) {
    if (c.col >= 256 && c.col >= kMaxCols) continue;
    if (!unique.empty() && unique.back().row == c.row && unique.back().col == c.col) unique.back() = c;
    else unique.push_back(c);
  }
  cells.clear();
  cells.shrink_to_fit();
  // A shared formula whose master didn't come out: its cells keep their values.
  std::set<uint32_t> liveSi;
  for (const Cell& c : unique)
    if (c.form == Cell::SharedMaster) liveSi.insert(c.si);
  for (Cell& c : unique)
    if (c.form == Cell::SharedChild && !liveSi.count(c.si)) c.form = Cell::None;

  auto xfOf = [&](uint32_t xf) { return xf < cellXf_.size() ? cellXf_[xf] : 0u; };

  std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                  "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
                  "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">";
  // sheetPr
  {
    std::string pr;
    if (tab.kind != ExtColor::None || tabIcv >= 0) pr += "<tabColor" + palette_.Attrs(tabIcv >= 0 ? tabIcv : 0x7FFF, &tab) + "/>";
    if (pr == "<tabColor/>") pr.clear();
    if (wsBoolSeen && (!(wsBool & 0x40) || !(wsBool & 0x80)))
      pr += std::string("<outlinePr") + ((wsBool & 0x40) ? "" : " summaryBelow=\"0\"") + ((wsBool & 0x80) ? "" : " summaryRight=\"0\"") + "/>";
    if (wsBoolSeen && (wsBool & 0x100)) pr += "<pageSetUpPr fitToPage=\"1\"/>";
    if (!pr.empty()) o += "<sheetPr>" + pr + "</sheetPr>";
  }
  if (!unique.empty()) {
    uint32_t minC = UINT32_MAX, maxC = 0;
    for (const Cell& c : unique) minC = std::min<uint32_t>(minC, c.col), maxC = std::max<uint32_t>(maxC, c.col);
    o += "<dimension ref=\"" + RangeRef(unique.front().row, minC, unique.back().row, maxC) + "\"/>";
  } else {
    o += "<dimension ref=\"A1\"/>";
  }
  // sheetViews
  {
    o += "<sheetViews><sheetView";
    if (window2Seen) {
      if (window2 & 1) o += " showFormulas=\"1\"";
      if (!(window2 & 2)) o += " showGridLines=\"0\"";
      if (!(window2 & 4)) o += " showRowColHeaders=\"0\"";
      if (!(window2 & 0x10)) o += " showZeros=\"0\"";
      if (window2 & 0x40) o += " rightToLeft=\"1\"";
      if (window2 & 0x200) o += " tabSelected=\"1\"";
      if (!(window2 & 0x80)) o += " showOutlineSymbols=\"0\"";
      if (!(window2 & 0x20) && gridColor < 64) o += " defaultGridColor=\"0\"";
      if (window2 & 0x800) o += " view=\"pageBreakPreview\"";
      if (topRow || leftCol) o += " topLeftCell=\"" + CellRef(std::min<uint32_t>(topRow, 65535), std::min<uint32_t>(leftCol, 255)) + "\"";
      if (!(window2 & 0x20) && gridColor < 64) Attr(o, "colorId", gridColor);
    }
    uint32_t zoom = 0;
    if (zoomNum && zoomDen) zoom = static_cast<uint32_t>(std::lround(100.0 * zoomNum / zoomDen));
    if (zoom >= 10 && zoom <= 400 && zoom != 100) Attr(o, "zoomScale", zoom);
    if (scaleNormal >= 10 && scaleNormal <= 400 && (zoom >= 10 && zoom <= 400 && zoom != 100)) Attr(o, "zoomScaleNormal", scaleNormal);
    if ((window2 & 0x800) && scaleSlv >= 10 && scaleSlv <= 400) Attr(o, "zoomScaleSheetLayoutView", scaleSlv);
    o += " workbookViewId=\"0\">";
    static const char* const kPanes[] = {"bottomRight", "topRight", "bottomLeft", "topLeft"};
    const bool frozen = (window2 & 8) != 0;
    std::string active = "topLeft";
    if (pane.present && (pane.x || pane.y)) {
      o += "<pane";
      if (pane.x) Attr(o, "xSplit", pane.x);
      if (pane.y) Attr(o, "ySplit", pane.y);
      const uint32_t pr = std::min<uint32_t>(pane.top, 65535), pc = std::min<uint32_t>(pane.left, 255);
      o += " topLeftCell=\"" + CellRef(pr, pc) + "\"";
      uint8_t ap = pane.active;
      // The active pane must be one the split makes.
      if (pane.x && pane.y) {
      } else if (pane.y) {
        ap = (ap == 2 || ap == 0) ? 2 : 3;
      } else {
        ap = (ap == 1 || ap == 0) ? 1 : 3;
      }
      active = kPanes[ap];
      Attr(o, "activePane", active);
      if (frozen) o += (window2 & 0x100) ? " state=\"frozen\"" : " state=\"frozenSplit\"";
      o += "/>";
    }
    for (const Sel& s : sels) {
      if (kPanes[s.pane] != active) continue;
      o += "<selection";
      if (active != "topLeft") Attr(o, "pane", active);
      o += " activeCell=\"" + CellRef(s.row, std::min<uint16_t>(s.col, 255)) + "\"";
      std::string sq;
      for (const auto& rr : s.refs) {
        if (!sq.empty()) sq += ' ';
        sq += RangeRef(rr[0], rr[1], rr[2], rr[3]);
      }
      if (!sq.empty()) Attr(o, "sqref", sq);
      o += "/>";
      break;
    }
    o += "</sheetView></sheetViews>";
  }
  // sheetFormatPr
  {
    uint8_t rowLevel = 0, colLevel = 0;
    for (const auto& [i, rr] : rows) rowLevel = std::max(rowLevel, rr.level);
    for (const ColRec& c : cols) colLevel = std::max(colLevel, c.level);
    o += "<sheetFormatPr";
    if (defColWidth != 8 && defColWidth < 256) Attr(o, "baseColWidth", defColWidth);
    AttrNum(o, "defaultRowHeight", defaultHeight);
    if (defaultHeightFlags & 1) o += " customHeight=\"1\"";
    if (defaultHeightFlags & 2) o += " zeroHeight=\"1\"";
    if (rowLevel) Attr(o, "outlineLevelRow", rowLevel);
    if (colLevel) Attr(o, "outlineLevelCol", colLevel);
    o += "/>";
  }
  // cols: sorted, without overlaps
  if (!cols.empty()) {
    std::stable_sort(cols.begin(), cols.end(), [](const ColRec& a, const ColRec& b) { return a.first < b.first; });
    o += "<cols>";
    uint32_t next = 0;
    for (ColRec c : cols) {
      if (c.first < next) c.first = next;
      if (c.first > c.last) continue;
      next = c.last + 1;
      o += "<col";
      Attr(o, "min", c.first + 1);
      Attr(o, "max", c.last + 1);
      AttrNum(o, "width", c.width);
      if (c.xf != 15 && xfOf(c.xf)) Attr(o, "style", xfOf(c.xf));
      if (c.hidden) o += " hidden=\"1\"";
      if (c.bestFit) o += " bestFit=\"1\"";
      if (c.custom || c.width != 0) o += " customWidth=\"1\"";
      if (c.level) Attr(o, "outlineLevel", c.level);
      if (c.collapsed) o += " collapsed=\"1\"";
      o += "/>";
    }
    o += "</cols>";
  }
  // sheetData
  o += "<sheetData>";
  {
    size_t ci = 0;
    auto rowIt = rows.begin();
    auto writeRow = [&](uint32_t row, const RowRec* rr, size_t from, size_t to) {
      o += "<row r=\"" + std::to_string(row + 1) + "\"";
      if (rr) {
        if (rr->format && rr->xf < cellXf_.size()) {
          Attr(o, "s", xfOf(rr->xf));
          o += " customFormat=\"1\"";
        }
        if (rr->custom || rr->height != defaultHeight) AttrNum(o, "ht", rr->height);
        if (rr->hidden) o += " hidden=\"1\"";
        if (rr->custom) o += " customHeight=\"1\"";
        if (rr->level) Attr(o, "outlineLevel", rr->level);
        if (rr->collapsed) o += " collapsed=\"1\"";
      }
      if (from == to) {
        o += "/>";
        return;
      }
      o += ">";
      for (size_t k = from; k < to; ++k) {
        const Cell& c = unique[k];
        o += "<c r=\"" + CellRef(c.row, c.col) + "\"";
        if (const uint32_t s = xfOf(c.xf)) Attr(o, "s", s);
        const bool hasF = c.form != Cell::None;
        switch (c.kind) {
          case Cell::Sst: o += " t=\"s\""; break;
          case Cell::Bool: o += " t=\"b\""; break;
          case Cell::Error: o += " t=\"e\""; break;
          case Cell::Str: o += hasF ? " t=\"str\"" : " t=\"s\""; break;
          default: break;
        }
        o += ">";
        if (c.form == Cell::Normal) {
          o += "<f>";
          Esc(o, texts[c.formula]);
          o += "</f>";
        } else if (c.form == Cell::SharedMaster) {
          o += "<f t=\"shared\" ref=\"" + texts[c.refText] + "\" si=\"" + std::to_string(c.si) + "\">";
          Esc(o, texts[c.formula]);
          o += "</f>";
        } else if (c.form == Cell::SharedChild) {
          o += "<f t=\"shared\" si=\"" + std::to_string(c.si) + "\"/>";
        } else if (c.form == Cell::ArrayMaster) {
          o += "<f t=\"array\" ref=\"" + texts[c.refText] + "\">";
          Esc(o, texts[c.formula]);
          o += "</f>";
        }
        switch (c.kind) {
          case Cell::Blank: break;
          case Cell::Number: o += "<v>"; AppendNumber(o, c.num); o += "</v>"; break;
          case Cell::Sst: o += "<v>" + std::to_string(c.index) + "</v>"; break;
          case Cell::Bool: o += c.index ? "<v>1</v>" : "<v>0</v>"; break;
          case Cell::Error: {
            const std::string_view e = detail::ErrorName(static_cast<uint8_t>(c.index));
            o += "<v>";
            o += e.empty() ? "#N/A" : e;
            o += "</v>";
            break;
          }
          case Cell::Str:
            if (hasF) {
              o += "<v>";
              Esc(o, texts[c.index]);
              o += "</v>";
            } else {
              o += "<v>" + std::to_string(AddSstText(texts[c.index])) + "</v>";
            }
            break;
        }
        o += "</c>";
      }
      o += "</row>";
    };
    while (ci < unique.size() || rowIt != rows.end()) {
      const uint32_t cellRow = ci < unique.size() ? unique[ci].row : UINT32_MAX;
      const uint32_t recRow = rowIt != rows.end() ? rowIt->first : UINT32_MAX;
      if (recRow < cellRow) {
        const RowRec& rr = rowIt->second;
        if (rr.custom || rr.hidden || rr.level || rr.collapsed || rr.format || rr.height != defaultHeight)
          writeRow(recRow, &rr, 0, 0);
        ++rowIt;
        continue;
      }
      size_t end = ci;
      while (end < unique.size() && unique[end].row == cellRow) ++end;
      const RowRec* rr = recRow == cellRow ? &rowIt->second : nullptr;
      writeRow(cellRow, rr, ci, end);
      if (rr) ++rowIt;
      ci = end;
    }
  }
  o += "</sheetData>";
  if (protect) {
    o += "<sheetProtection";
    if (sheetPassword) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "%04X", sheetPassword);
      Attr(o, "password", buf);
    }
    o += " sheet=\"1\"";
    if (objects) o += " objects=\"1\"";
    if (scenarios) o += " scenarios=\"1\"";
    o += "/>";
  }
  if (autoFilter) {
    auto it = filterRanges_.find(static_cast<int>(&entry - sheets_.data()) + 1);
    if (it != filterRanges_.end()) {
      const auto& a = it->second;
      o += "<autoFilter ref=\"" + RangeRef(a[0], a[1], a[2], a[3]) + "\"/>";
    }
  }
  // Merged cells, without overlaps (Excel repairs those).
  if (!merges.empty()) {
    std::sort(merges.begin(), merges.end());
    std::vector<std::array<uint32_t, 4>> kept;
    for (const auto& m : merges) {
      bool overlap = false;
      for (auto it = kept.rbegin(); it != kept.rend(); ++it) {
        const auto& k = *it;
        if (k[2] >= m[0] && k[0] <= m[2] && k[3] >= m[1] && k[1] <= m[3]) {
          overlap = true;
          break;
        }
        if (kept.size() > 4096 && it - kept.rbegin() > 4096) break;  // keeps it linear on huge lists
      }
      if (!overlap) kept.push_back(m);
    }
    o += "<mergeCells count=\"" + std::to_string(kept.size()) + "\">";
    for (const auto& m : kept) o += "<mergeCell ref=\"" + RangeRef(m[0], m[1], m[2], m[3]) + "\"/>";
    o += "</mergeCells>";
  }
  flushCond();
  o += condFormats;
  if (!validations.empty()) o += "<dataValidations count=\"" + std::to_string(validationCount) + "\">" + validations + "</dataValidations>";
  // Hyperlinks
  if (!links.empty()) {
    std::string hl;
    int relId = 0;
    std::set<std::string> refs;
    for (const Link& l : links) {
      const std::string ref = RangeRef(l.r1, l.c1, std::min<uint32_t>(l.r2, 65535), l.c2);
      if (!refs.insert(ref).second) continue;
      hl += "<hyperlink";
      Attr(hl, "ref", ref);
      if (!l.target.empty()) {
        const std::string id = "rId" + std::to_string(++relId);
        Attr(hl, "r:id", id);
        rels += "<Relationship Id=\"" + id +
                "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink\" Target=\"" +
                Esc(UriEscape(l.target)) + "\" TargetMode=\"External\"/>";
      }
      if (!l.location.empty()) Attr(hl, "location", l.location);
      for (const auto& [at, tip] : tips)
        if (at == (l.r1 << 16 | l.c1) && !tip.empty()) {
          Attr(hl, "tooltip", tip.substr(0, 255));
          break;
        }
      if (!l.display.empty()) Attr(hl, "display", l.display);
      hl += "/>";
    }
    o += "<hyperlinks>" + hl + "</hyperlinks>";
  }
  if (hCenter || vCenter || gridlines || headings) {
    o += "<printOptions";
    if (hCenter) o += " horizontalCentered=\"1\"";
    if (vCenter) o += " verticalCentered=\"1\"";
    if (headings) o += " headings=\"1\"";
    if (gridlines) o += " gridLines=\"1\"";
    o += "/>";
  }
  o += "<pageMargins";
  AttrNum(o, "left", margins[0]);
  AttrNum(o, "right", margins[1]);
  AttrNum(o, "top", margins[2]);
  AttrNum(o, "bottom", margins[3]);
  AttrNum(o, "header", headerMargin);
  AttrNum(o, "footer", footerMargin);
  o += "/>";
  if (setup.present) {
    const uint16_t g = setup.grbit;
    o += "<pageSetup";
    if (!(g & 4)) {  // fNoPls: the printer settings aren't there
      if (setup.paper >= 1 && setup.paper <= 118) Attr(o, "paperSize", setup.paper);
      if (setup.scale >= 10 && setup.scale <= 400 && setup.scale != 100) Attr(o, "scale", setup.scale);
      if ((g & 0x80) && static_cast<int16_t>(setup.start) != 1) Attr(o, "firstPageNumber", static_cast<int16_t>(setup.start));
    }
    if (setup.fitW != 1) Attr(o, "fitToWidth", setup.fitW);
    if (setup.fitH != 1) Attr(o, "fitToHeight", setup.fitH);
    if (g & 1) o += " pageOrder=\"overThenDown\"";
    if (!(g & 0x44)) o += (g & 2) ? " orientation=\"portrait\"" : " orientation=\"landscape\"";
    if (!(g & 4)) {
      if (g & 8) o += " blackAndWhite=\"1\"";
      if (g & 0x10) o += " draft=\"1\"";
      if (g & 0x20) o += (g & 0x200) ? " cellComments=\"atEnd\"" : " cellComments=\"asDisplayed\"";
      if (g & 0x80) o += " useFirstPageNumber=\"1\"";
      static const char* const kErrors[] = {"", " errors=\"blank\"", " errors=\"dash\"", " errors=\"NA\""};
      o += kErrors[(g >> 10) & 3];
      if (setup.hres && setup.hres <= 4800) Attr(o, "horizontalDpi", setup.hres);
      if (setup.vres && setup.vres <= 4800) Attr(o, "verticalDpi", setup.vres);
      if (setup.copies > 1 && setup.copies < 1000) Attr(o, "copies", setup.copies);
    }
    o += "/>";
  }
  if (!header.empty() || !footer.empty()) {
    o += "<headerFooter>";
    if (!header.empty()) o += "<oddHeader>" + EscXstring(header) + "</oddHeader>";
    if (!footer.empty()) o += "<oddFooter>" + EscXstring(footer) + "</oddFooter>";
    o += "</headerFooter>";
  }
  auto breaks = [&](const char* tag, std::vector<uint32_t>& v, uint32_t max) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    std::erase_if(v, [](uint32_t x) { return x == 0; });
    if (v.empty()) return;
    o += std::string("<") + tag + " count=\"" + std::to_string(v.size()) + "\" manualBreakCount=\"" + std::to_string(v.size()) + "\">";
    for (uint32_t x : v) o += "<brk id=\"" + std::to_string(x) + "\" max=\"" + std::to_string(max) + "\" man=\"1\"/>";
    o += std::string("</") + tag + ">";
  };
  breaks("rowBreaks", rowBreaks, 16383);
  breaks("colBreaks", colBreaks, 1048575);
  (void)outIndex;
  return o;
}

// A conditional format's differential format (DXFN, [MS-XLS] 2.5.86) as a dxf: its font (bold,
// italic, strikeout, underline, color), borders and fill. Number formats, alignment and protection
// are read past and left out. Reads `b` to the end of the DXFN.
std::string Converter::DxfXml(Bytes& b) {
  const uint32_t f1 = b.U32();
  const uint16_t f2 = b.U16();
  if (!b.Ok()) return {};
  std::string font, border, fill;
  if (f1 & (1u << 25)) {  // number format
    if (f2 & 1) {
      const uint16_t cb = b.U16();
      b.Skip(cb >= 2 ? cb - 2 : 0);
    } else {
      b.Skip(2);
    }
  }
  if (f1 & (1u << 26)) {  // DXFFntD: 118 bytes
    Bytes d(b.Take(118));
    if (d.Size() == 118) {
      d.Seek(68);
      const uint32_t ts = d.U32();
      const uint16_t bls = d.U16();
      d.U16();
      const uint8_t uls = d.U8();
      d.Seek(80);
      const uint32_t icv = d.U32();
      d.U32();
      const uint32_t tsNinch = d.U32(), sssNinch = d.U32(), ulsNinch = d.U32(), blsNinch = d.U32();
      (void)sssNinch;
      if (!blsNinch && (bls == 700 || bls == 400)) font += bls == 700 ? "<b/>" : "<b val=\"0\"/>";
      if (!(tsNinch & 2)) font += (ts & 2) ? "<i/>" : "<i val=\"0\"/>";
      if (!(tsNinch & 0x80)) font += (ts & 0x80) ? "<strike/>" : "<strike val=\"0\"/>";
      if (!ulsNinch) {
        static const std::pair<uint8_t, const char*> kU[] = {{0, "none"}, {1, "single"}, {2, "double"}, {0x21, "singleAccounting"},
                                                             {0x22, "doubleAccounting"}};
        for (const auto& [code, name] : kU)
          if (code == uls) font += std::string("<u val=\"") + name + "\"/>";
      }
      if (icv < 64) font += "<color" + palette_.Attrs(icv) + "/>";
    }
  }
  if (f1 & (1u << 27)) b.Skip(8);  // alignment
  if (f1 & (1u << 28)) {           // DXFBdr
    const uint32_t a = b.U32(), c = b.U32();
    static const char* const kStyles[] = {"", "thin", "medium", "dashed", "dotted", "thick", "double", "hair",
                                          "mediumDashed", "dashDot", "mediumDashDot", "dashDotDot", "mediumDashDotDot",
                                          "slantDashDot"};
    auto side = [&](const char* tag, uint32_t style, uint32_t icv, bool ninch) {
      if (ninch || style > 13) return std::string();
      if (style == 0) return std::string("<") + tag + "/>";
      const std::string col = palette_.Attrs(icv);
      return std::string("<") + tag + " style=\"" + kStyles[style] + "\"><color" + (col.empty() ? " indexed=\"64\"" : col) +
             "/></" + tag + ">";
    };
    border = side("left", a & 15, (a >> 16) & 0x7F, f1 & (1u << 10)) + side("right", (a >> 4) & 15, (a >> 23) & 0x7F, f1 & (1u << 11)) +
             side("top", (a >> 8) & 15, c & 0x7F, f1 & (1u << 12)) + side("bottom", (a >> 12) & 15, (c >> 7) & 0x7F, f1 & (1u << 13));
  }
  if (f1 & (1u << 29)) {  // DXFPat
    const uint16_t p = b.U16(), colors = b.U16();
    const uint32_t fls = p >> 10, fore = colors & 0x7F, back = (colors >> 7) & 0x7F;
    static const char* const kPatterns[] = {"none", "solid", "mediumGray", "darkGray", "lightGray", "darkHorizontal",
                                            "darkVertical", "darkDown", "darkUp", "darkGrid", "darkTrellis",
                                            "lightHorizontal", "lightVertical", "lightDown", "lightUp", "lightGrid",
                                            "lightTrellis", "gray125", "gray0625"};
    std::string pf;
    if (!(f1 & (1u << 16)) && fls < 19 && fls != 1) pf += std::string(" patternType=\"") + kPatterns[fls] + "\"";
    std::string inner;
    if (!(f1 & (1u << 17)) && fls != 1 && fore < 64) inner += "<fgColor" + palette_.Attrs(fore) + "/>";
    if (!(f1 & (1u << 18)) && back < 64) inner += "<bgColor" + palette_.Attrs(back) + "/>";
    if (!pf.empty() || !inner.empty()) fill = "<fill><patternFill" + pf + (inner.empty() ? "/>" : ">" + inner + "</patternFill>") + "</fill>";
  }
  if (f1 & (1u << 30)) b.Skip(2);  // protection
  return "<dxf>" + (font.empty() ? std::string() : "<font>" + font + "</font>") + fill +
         (border.empty() ? std::string() : "<border>" + border + "</border>") + "</dxf>";
}

// A sheet's notes: the comments part, and the VML drawing that places them (Excel's defaults: a
// box to the right of the cell, hidden unless the note is shown).
std::string Converter::NotesParts(const std::vector<Note>& notes, size_t sheet, std::string& comments, std::string& vml) {
  std::vector<std::string> authors;
  std::string list;
  std::set<std::pair<uint32_t, uint32_t>> seen;
  vml = "<xml xmlns:v=\"urn:schemas-microsoft-com:vml\" xmlns:o=\"urn:schemas-microsoft-com:office:office\" "
        "xmlns:x=\"urn:schemas-microsoft-com:office:excel\">"
        "<o:shapelayout v:ext=\"edit\"><o:idmap v:ext=\"edit\" data=\"" + std::to_string(sheet + 1) + "\"/></o:shapelayout>"
        "<v:shapetype id=\"_x0000_t202\" coordsize=\"21600,21600\" o:spt=\"202\" path=\"m,l,21600r21600,l21600,xe\">"
        "<v:stroke joinstyle=\"miter\"/><v:path gradientshapeok=\"t\" o:connecttype=\"rect\"/></v:shapetype>";
  uint32_t id = static_cast<uint32_t>((sheet + 1) * 1024 + 1);
  for (const Note& n : notes) {
    if (!seen.insert({n.row, n.col}).second) continue;
    auto it = std::find(authors.begin(), authors.end(), n.author);
    if (it == authors.end()) it = authors.insert(authors.end(), n.author);
    const bool preserve = !n.text.empty() && (n.text.front() == ' ' || n.text.back() == ' ' || n.text.find('\n') != std::string::npos);
    list += "<comment ref=\"" + CellRef(n.row, n.col) + "\" authorId=\"" + std::to_string(it - authors.begin()) + "\"><text><t" +
            (preserve ? " xml:space=\"preserve\"" : "") + ">" + Esc(n.text) + "</t></text></comment>";
    const uint32_t top = n.row > 0 ? n.row - 1 : 0;
    vml += "<v:shape id=\"_x0000_s" + std::to_string(id++) +
           "\" type=\"#_x0000_t202\" style=\"position:absolute;margin-left:59.25pt;margin-top:1.5pt;width:108pt;height:59.25pt;z-index:" +
           std::to_string(id - (sheet + 1) * 1024 - 1) + (n.shown ? ";visibility:visible" : ";visibility:hidden") +
           "\" fillcolor=\"#ffffe1\" o:insetmode=\"auto\"><v:fill color2=\"#ffffe1\"/>"
           "<v:shadow on=\"t\" color=\"black\" obscured=\"t\"/><v:path o:connecttype=\"none\"/>"
           "<v:textbox style=\"mso-direction-alt:auto\"><div style=\"text-align:left\"></div></v:textbox>"
           "<x:ClientData ObjectType=\"Note\"><x:MoveWithCells/><x:SizeWithCells/><x:Anchor>" +
           std::to_string(n.col + 1) + ", 15, " + std::to_string(top) + ", 10, " + std::to_string(n.col + 3) + ", 15, " +
           std::to_string(top + 4) + ", 4</x:Anchor><x:AutoFill>False</x:AutoFill><x:Row>" + std::to_string(n.row) +
           "</x:Row><x:Column>" + std::to_string(n.col) + "</x:Column>" + (n.shown ? "<x:Visible/>" : "") +
           "</x:ClientData></v:shape>";
  }
  vml += "</xml>";
  comments = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
             "<comments xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><authors>";
  for (const std::string& a : authors) comments += "<author>" + Esc(a) + "</author>";
  comments += "</authors><commentList>" + list + "</commentList></comments>";
  return {};
}

// The theme's part from THEME's package, when it is a well-formed theme; empty otherwise (Office's
// theme then applies).
std::string Converter::ThemeXml() {
  if (themeZip_.empty() && theme2007_) {
    // Office 2007's theme: its colors and fonts, with plain formats for shapes.
    const std::string fills = "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>";
    std::string lines;
    for (const char* w : {"9525", "25400", "38100"}) lines += std::string("<a:ln w=\"") + w + "\">" + fills + "</a:ln>";
    auto color = [](const char* name, const char* rgb) {
      return std::string("<a:") + name + "><a:srgbClr val=\"" + rgb + "\"/></a:" + name + ">";
    };
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
           "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"Office Theme\"><a:themeElements>"
           "<a:clrScheme name=\"Office\"><a:dk1><a:sysClr val=\"windowText\" lastClr=\"000000\"/></a:dk1>"
           "<a:lt1><a:sysClr val=\"window\" lastClr=\"FFFFFF\"/></a:lt1>" +
           color("dk2", "1F497D") + color("lt2", "EEECE1") + color("accent1", "4F81BD") + color("accent2", "C0504D") +
           color("accent3", "9BBB59") + color("accent4", "8064A2") + color("accent5", "4BACC6") + color("accent6", "F79646") +
           color("hlink", "0000FF") + color("folHlink", "800080") +
           "</a:clrScheme><a:fontScheme name=\"Office\"><a:majorFont><a:latin typeface=\"Cambria\"/><a:ea typeface=\"\"/>"
           "<a:cs typeface=\"\"/></a:majorFont><a:minorFont><a:latin typeface=\"Calibri\"/><a:ea typeface=\"\"/>"
           "<a:cs typeface=\"\"/></a:minorFont></a:fontScheme><a:fmtScheme name=\"Office\"><a:fillStyleLst>" +
           fills + fills + fills + "</a:fillStyleLst><a:lnStyleLst>" + lines +
           "</a:lnStyleLst><a:effectStyleLst><a:effectStyle><a:effectLst/></a:effectStyle><a:effectStyle><a:effectLst/>"
           "</a:effectStyle><a:effectStyle><a:effectLst/></a:effectStyle></a:effectStyleLst><a:bgFillStyleLst>" +
           fills + fills + fills + "</a:bgFillStyleLst></a:fmtScheme></a:themeElements><a:objectDefaults/><a:extraClrSchemeLst/></a:theme>";
  }
  if (themeZip_.size() < 22) return {};
  auto snapshot = pkg::Snapshot::FromBytes(themeZip_);
  pkg::ZipReader zip;
  if (!snapshot || !zip.Open(*snapshot).Ok()) return {};
  for (const pkg::ZipEntry& e : zip.Entries()) {
    // theme/theme/theme1.xml (not themeManager.xml beside it).
    constexpr std::string_view kPrefix = "theme/theme/theme";
    if (e.name.rfind(kPrefix, 0) != 0 || e.name.size() <= kPrefix.size() + 4 || !std::isdigit(static_cast<unsigned char>(e.name[kPrefix.size()])) ||
        e.name.substr(e.name.size() - 4) != ".xml")
      continue;
    std::vector<uint8_t> bytes;
    if (!zip.Read(e, bytes).Ok() || bytes.size() > (8u << 20)) continue;
    pugi::xml_document doc;
    if (!doc.load_buffer(bytes.data(), bytes.size(), pugi::parse_default, pugi::encoding_utf8)) continue;
    const std::string_view root = doc.document_element().name();
    if (root.size() < 5 || root.substr(root.size() - 5) != "theme" || !HasChildEnding(doc.document_element(), "themeElements"))
      continue;
    return std::string(bytes.begin(), bytes.end());
  }
  return {};
}

std::vector<uint8_t> Converter::Convert(const StreamFn& streams, pkg::Status& status) {
  auto workbook = streams("Workbook");
  if (!workbook) {
    if (streams("Book"))
      status = pkg::Status::Fail(pkg::Status::Code::Unsupported,
                                 "it was saved by Excel 5.0/95 or older, which Featherweight can't open. Save it as a newer Excel workbook first.");
    else
      status = pkg::Status::Fail(pkg::Status::Code::Damaged, "it isn't an Excel workbook or it is damaged.");
    return {};
  }
  stream_ = std::move(*workbook);
  if (!Globals(status)) return {};
  Formulas();
  // _FilterDatabase's area, for the sheets' filters.
  for (const NameEntry& n : names_) {
    if (n.builtinCode != 0x0D || n.itab <= 0 || n.rgce.size() != 11 || (n.rgce[0] & 0x1F) != 0x1B) continue;
    Bytes b(n.rgce);
    b.Skip(3);
    const uint32_t r1 = b.U16(), r2 = b.U16(), c1 = b.U16() & 0xFF, c2 = b.U16() & 0xFF;
    if (b.Ok() && r1 <= r2 && c1 <= c2) filterRanges_[n.itab] = {r1, c1, r2, c2};
  }
  std::string styles = StylesXml();

  struct Part {
    std::string name, text;
  };
  std::vector<Part> parts;
  std::string types =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
      "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
      "<Default Extension=\"vml\" ContentType=\"application/vnd.openxmlformats-officedocument.vmlDrawing\"/>"
      "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
      "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
      "<Override PartName=\"/xl/sharedStrings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml\"/>";
  std::string wbRels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                       "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
  int kept = 0;
  for (size_t i = 0; i < sheets_.size(); ++i) {
    const SheetEntry& s = sheets_[i];
    if (s.out < 0) continue;
    ++kept;
    std::string rels;
    std::vector<Note> notes;
    std::string xml = SheetXml(s, s.out, rels, notes);
    const std::string n = std::to_string(s.out + 1);
    if (!notes.empty()) {
      std::string comments, vml;
      NotesParts(notes, s.out, comments, vml);
      xml += "<legacyDrawing r:id=\"rIdVml1\"/>";
      rels += "<Relationship Id=\"rIdVml1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/vmlDrawing\" "
              "Target=\"../drawings/vmlDrawing" + n + ".vml\"/>"
              "<Relationship Id=\"rIdNotes1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/comments\" "
              "Target=\"../comments" + n + ".xml\"/>";
      parts.push_back({"xl/comments" + n + ".xml", std::move(comments)});
      parts.push_back({"xl/drawings/vmlDrawing" + n + ".vml", std::move(vml)});
      types += "<Override PartName=\"/xl/comments" + n +
               ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.comments+xml\"/>";
    }
    xml += "</worksheet>";
    parts.push_back({"xl/worksheets/sheet" + n + ".xml", std::move(xml)});
    types += "<Override PartName=\"/xl/worksheets/sheet" + n +
             ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
    wbRels += "<Relationship Id=\"rId" + n +
              "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet" +
              n + ".xml\"/>";
    if (!rels.empty())
      parts.push_back({"xl/worksheets/_rels/sheet" + n + ".xml.rels",
                       "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                       "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">" +
                           rels + "</Relationships>"});
  }
  if (kept == 0) {
    status = pkg::Status::Fail(pkg::Status::Code::Unsupported, "it has no worksheets Featherweight can show.");
    return {};
  }
  const std::string base = std::to_string(kept);
  wbRels += "<Relationship Id=\"rId" + std::to_string(kept + 1) +
            "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
            "<Relationship Id=\"rId" + std::to_string(kept + 2) +
            "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings\" Target=\"sharedStrings.xml\"/>" +
            [&] {
              std::string theme = ThemeXml();
              if (theme.empty()) return std::string();
              parts.push_back({"xl/theme/theme1.xml", std::move(theme)});
              types += "<Override PartName=\"/xl/theme/theme1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>";
              return "<Relationship Id=\"rId" + std::to_string(kept + 3) +
                     "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\" Target=\"theme/theme1.xml\"/>";
            }() +
            "</Relationships>";
  // The shared strings: the SST's, then the texts of LABEL records and formulas' values.
  std::string sst = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                    "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" count=\"" +
                    std::to_string(sst_.size() + extraStrings_.size()) + "\" uniqueCount=\"" +
                    std::to_string(sst_.size() + extraStrings_.size()) + "\">";
  auto textXml = [&](const std::string& t) {
    const bool preserve = !t.empty() && (t.front() == ' ' || t.back() == ' ' || t.find_first_of("\t\n\r") != std::string::npos ||
                                         t.front() == '\n' || t.back() == '\n');
    return std::string(preserve ? "<t xml:space=\"preserve\">" : "<t>") + Esc(t) + "</t>";
  };
  auto fontOf = [&](uint16_t ifnt) -> const FontRec& {
    const uint32_t i = ifnt < 4 ? ifnt : ifnt == 4 ? 0 : ifnt - 1u;
    return fonts_[i < fonts_.size() ? i : 0];
  };
  for (const SstString& s : sst_) {
    sst += "<si>";
    std::vector<std::pair<uint16_t, uint16_t>> runs;
    for (const auto& run : s.runs)
      if (run.first < s.text.size() && (runs.empty() || run.first > runs.back().first)) runs.push_back(run);
    if (runs.empty()) {
      sst += textXml(Utf8(s.text));
    } else {
      if (runs.front().first > 0) sst += "<r>" + textXml(Utf8(s.text, 0, runs.front().first)) + "</r>";
      for (size_t k = 0; k < runs.size(); ++k) {
        const size_t end = k + 1 < runs.size() ? runs[k + 1].first : s.text.size();
        sst += "<r>" + FontXml(fontOf(runs[k].second), true) + textXml(Utf8(s.text, runs[k].first, end)) + "</r>";
      }
    }
    sst += "</si>";
  }
  for (const std::string& t : extraStrings_) sst += "<si>" + textXml(t) + "</si>";
  sst += "</sst>";
  (void)base;

  types += "</Types>";
  parts.insert(parts.begin(), {"[Content_Types].xml", types});
  parts.push_back({"_rels/.rels",
                   "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                   "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                   "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
                   "Target=\"xl/workbook.xml\"/></Relationships>"});
  parts.push_back({"xl/workbook.xml", WorkbookXml()});
  parts.push_back({"xl/_rels/workbook.xml.rels", wbRels});
  if (!dxfs_.empty()) {  // the conditional formats' formats, which the sheets made after the styles
    const std::string none = "<dxfs count=\"0\"/>";
    std::string list = "<dxfs count=\"" + std::to_string(dxfs_.size()) + "\">";
    for (const std::string& d : dxfs_) list += d;
    if (const size_t at = styles.find(none); at != std::string::npos) styles.replace(at, none.size(), list + "</dxfs>");
  }
  parts.push_back({"xl/styles.xml", std::move(styles)});
  parts.push_back({"xl/sharedStrings.xml", std::move(sst)});

  pkg::MemorySink sink;
  pkg::ZipWriter zip(sink);
  for (const Part& p : parts)
    if (!zip.Add(p.name, std::span(reinterpret_cast<const uint8_t*>(p.text.data()), p.text.size())).Ok()) {
      status = pkg::Status::Fail(pkg::Status::Code::TooLarge, "it is too large to open.");
      return {};
    }
  if (!zip.Finish().Ok()) {
    status = pkg::Status::Fail(pkg::Status::Code::TooLarge, "it is too large to open.");
    return {};
  }
  return std::move(sink.bytes);
}

}  // namespace

std::vector<uint8_t> ToXlsx(const StreamFn& streams, pkg::Status& status, std::wstring_view password) {
  status = {};
  Converter c;
  c.password = password;
  return c.Convert(streams, status);
}

bool NeedsPassword(const StreamFn& streams) {
  pkg::Status status;
  Converter c;
  c.stopAtFilepass = true;
  c.Convert(streams, status);
  return c.needsPassword;
}

bool IsWorkbook(const StreamFn& streams) { return streams("Workbook").has_value() || streams("Book").has_value(); }

}  // namespace fw::sheet::xls
