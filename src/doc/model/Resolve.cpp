// Property resolution (plan §6.3): ECMA-376 Part 1 §17.7.2 as Word implements it. The order is
// document defaults, table style (with conditional formatting), numbering, paragraph styles,
// character styles, direct formatting. Where Word departs from the standard ([MS-OI29500],
// [MS-DOCX]), the code says so with the note's number, and test_doc.cpp tests each departure:
//
//  D1 Toggle properties (b, i, caps, ...): the table style and the paragraph style set the value,
//     only the character style toggles it, direct formatting sets it ([MS-OI29500] 2.1.258,
//     2.1.246; ECMA would toggle at every style level).
//  D2 Numbering after the paragraph style: a level's w:pPr (indents, tabs, jc) overrides the
//     paragraph style when the paragraph names its list directly (2.1.229). When the list comes
//     from the style, the style wins, as in Word and LibreOffice.
//  D3 Table style vs the default paragraph style: unless the document sets the compatibility
//     setting overrideTableStyleFontSizeAndJustification, a default paragraph style's size of 11 or
//     12 pt and its left justification don't override the table style's ([MS-DOCX] 2.3.1).
//  D4 Conditional formats apply in Word's order: row bands, column bands, first and last column,
//     first and last row, then the corners (2.1.252); a wholeTable tblStylePr is ignored, the
//     style's own properties are the whole table's (2.1.557); a corner applies only when both its
//     row and its column formats are on (2.1.246).
//  D5 A missing tblLook means firstRow, firstColumn and noVBand (0x04A0); w:val is read only when
//     no named attribute is present (2.1.160). Band sizes default to 0, no banding (2.1.251/253).
//  D6 Styles NoList, DefaultParagraphFont and TableNormal contribute nothing (2.1.242).
//  D7 Within one w:rFonts the theme font wins over the named one; across levels, whichever of the
//     pair comes later wins (2.1.88).
//  D8 Fonts default to Times New Roman, 10 pt, when nothing names them (2.1.88).
#include <algorithm>
#include <array>

#include "doc/model/Internal.h"

namespace fw::doc {

using namespace names;

// ---------- Parsing helpers ----------

Justification ParseJustification(std::string_view v) {
  if (v == "center") return Justification::Center;
  if (v == "right" || v == "end") return Justification::End;
  if (v == "both" || v == "justify") return Justification::Both;
  if (v == "distribute") return Justification::Distribute;
  if (v == "lowKashida") return Justification::LowKashida;
  if (v == "mediumKashida") return Justification::MediumKashida;
  if (v == "highKashida") return Justification::HighKashida;
  if (v == "thaiDistribute") return Justification::ThaiDistribute;
  if (v == "numTab") return Justification::NumTab;
  return Justification::Start;
}

BorderStyle ParseBorderStyle(std::string_view v) {
  static constexpr std::pair<std::string_view, BorderStyle> kStyles[] = {
      {"single", BorderStyle::Single}, {"thick", BorderStyle::Thick}, {"double", BorderStyle::Double},
      {"dotted", BorderStyle::Dotted}, {"dashed", BorderStyle::Dashed}, {"dotDash", BorderStyle::DotDash},
      {"dotDotDash", BorderStyle::DotDotDash}, {"triple", BorderStyle::Triple},
      {"thinThickSmallGap", BorderStyle::ThinThickSmallGap}, {"thickThinSmallGap", BorderStyle::ThickThinSmallGap},
      {"thinThickThinSmallGap", BorderStyle::ThinThickThinSmallGap},
      {"thinThickMediumGap", BorderStyle::ThinThickMediumGap}, {"thickThinMediumGap", BorderStyle::ThickThinMediumGap},
      {"thinThickThinMediumGap", BorderStyle::ThinThickThinMediumGap},
      {"thinThickLargeGap", BorderStyle::ThinThickLargeGap}, {"thickThinLargeGap", BorderStyle::ThickThinLargeGap},
      {"thinThickThinLargeGap", BorderStyle::ThinThickThinLargeGap}, {"wave", BorderStyle::Wave},
      {"doubleWave", BorderStyle::DoubleWave}, {"dashSmallGap", BorderStyle::DashSmallGap},
      {"dashDotStroked", BorderStyle::DashDotStroked}, {"threeDEmboss", BorderStyle::ThreeDEmboss},
      {"threeDEngrave", BorderStyle::ThreeDEngrave}, {"outset", BorderStyle::Outset}, {"inset", BorderStyle::Inset},
  };
  if (v.empty() || v == "nil" || v == "none") return BorderStyle::None;
  for (auto [name, style] : kStyles)
    if (v == name) return style;
  return BorderStyle::Art;
}

Underline ParseUnderline(std::string_view v) {
  static constexpr std::pair<std::string_view, Underline> kKinds[] = {
      {"single", Underline::Single}, {"words", Underline::Words}, {"double", Underline::Double},
      {"thick", Underline::Thick}, {"dotted", Underline::Dotted}, {"dottedHeavy", Underline::DottedHeavy},
      {"dash", Underline::Dash}, {"dashedHeavy", Underline::DashedHeavy}, {"dashLong", Underline::DashLong},
      {"dashLongHeavy", Underline::DashLongHeavy}, {"dotDash", Underline::DotDash},
      {"dashDotHeavy", Underline::DashDotHeavy}, {"dotDotDash", Underline::DotDotDash},
      {"dashDotDotHeavy", Underline::DashDotDotHeavy}, {"wave", Underline::Wave}, {"wavyHeavy", Underline::WavyHeavy},
      {"wavyDouble", Underline::WavyDouble},
  };
  for (auto [name, kind] : kKinds)
    if (v == name) return kind;
  return Underline::None;
}

std::optional<uint32_t> HighlightRgb(std::string_view v) {
  static constexpr std::pair<std::string_view, uint32_t> kColors[] = {
      {"black", 0x000000}, {"blue", 0x0000FF}, {"cyan", 0x00FFFF}, {"green", 0x00FF00}, {"magenta", 0xFF00FF},
      {"red", 0xFF0000}, {"yellow", 0xFFFF00}, {"white", 0xFFFFFF}, {"darkBlue", 0x000080}, {"darkCyan", 0x008080},
      {"darkGreen", 0x008000}, {"darkMagenta", 0x800080}, {"darkRed", 0x800000}, {"darkYellow", 0x808000},
      {"darkGray", 0x808080}, {"lightGray", 0xC0C0C0},
  };
  for (auto [name, rgb] : kColors)
    if (v == name) return rgb;
  return std::nullopt;
}

TableWidth ParseTableWidth(Node n) {
  TableWidth w;
  if (!n) return w;
  std::string_view type = n.AttrOr(kType, "dxa");
  std::string_view value = n.AttrOr(kW);
  if (type == "auto") w.type = TableWidth::Type::Auto;
  else if (type == "nil") w.type = TableWidth::Type::Nil;
  else if (type == "pct") w.type = TableWidth::Type::Pct;
  else w.type = TableWidth::Type::Dxa;
  if (w.type == TableWidth::Type::Pct && value.ends_with('%')) {
    // Strict and newer files write "50%".
    w.value = static_cast<int32_t>(ParseMeasure(value.substr(0, value.size() - 1), 50).value_or(0));
  } else {
    w.value = static_cast<int32_t>(ParseMeasure(value, 20).value_or(0));
  }
  return w;
}

namespace {

std::optional<uint8_t> HexByte(Node n, uint32_t name) {
  auto v = n.Hex(name);
  if (!v || *v > 0xFF) return std::nullopt;
  return static_cast<uint8_t>(*v);
}

// A color from val/themeColor/themeTint/themeShade (or fill/themeFill/... for shading).
Color ColorOf(const Document& doc, Node n, uint32_t val, uint32_t theme, uint32_t tint, uint32_t shade) {
  if (auto t = n.Attr(theme)) {
    auto ti = HexByte(n, tint), sh = HexByte(n, shade);
    if (auto rgb = doc.ThemeColor(*t, ti ? *ti : -1, sh ? *sh : -1)) return Color::Rgb(*rgb);
  }
  auto v = n.Attr(val);
  if (!v || *v == "auto") return Color{};
  if (auto rgb = ParseHex(*v); rgb && v->size() == 6) return Color::Rgb(*rgb);
  return Color{};
}

Border BorderOf(const Document& doc, Node n) {
  Border b;
  if (!n) return b;
  b.name = n.AttrOr(kVal);
  b.style = ParseBorderStyle(b.name);
  b.width = EighthPoints(static_cast<int32_t>(n.Int(Name("sz")).value_or(0)));
  b.spacePt = static_cast<int32_t>(n.Int(Name("space")).value_or(0));
  b.color = ColorOf(doc, n, Name("color"), Name("themeColor"), Name("themeTint"), Name("themeShade"));
  b.shadow = n.OnOff(Name("shadow")).value_or(false);
  b.frame = n.OnOff(Name("frame")).value_or(false);
  return b;
}

Shading ShadingOf(const Document& doc, Node n) {
  Shading s;
  if (!n) return s;
  s.pattern = n.AttrOr(kVal);
  s.color = ColorOf(doc, n, Name("color"), Name("themeColor"), Name("themeTint"), Name("themeShade"));
  s.fill = ColorOf(doc, n, Name("fill"), Name("themeFill"), Name("themeFillTint"), Name("themeFillShade"));
  return s;
}

bool Ignored(std::string_view styleId) {
  return styleId == "NoList" || styleId == "DefaultParagraphFont" || styleId == "TableNormal";  // D6
}

// A style and the styles it's based on, root first, without loops or ignored styles.
std::vector<const Style*> Chain(const Document& doc, const Style* style) {
  std::vector<const Style*> chain;
  for (int hops = 0; style && hops < 64; ++hops) {
    if (std::find(chain.begin(), chain.end(), style) != chain.end()) break;
    chain.push_back(style);
    style = style->basedOn == kNone ? nullptr : &doc.Styles()[style->basedOn];
  }
  std::reverse(chain.begin(), chain.end());
  chain.erase(std::remove_if(chain.begin(), chain.end(), [](const Style* s) { return Ignored(s->id); }), chain.end());
  return chain;
}

// ---------- Runs ----------

enum Toggle { kB, kBCs, kI, kICs, kCaps, kSmallCaps, kStrike, kEmboss, kImprint, kOutline, kShadow, kVanish, kToggleCount };
constexpr uint32_t kToggleNames[kToggleCount] = {Name("b"), Name("bCs"), Name("i"), Name("iCs"), Name("caps"),
                                                 Name("smallCaps"), Name("strike"), Name("emboss"), Name("imprint"),
                                                 Name("outline"), Name("shadow"), Name("vanish")};
using Toggles = std::array<std::optional<bool>, kToggleCount>;

struct RunWork {
  RunProps p;
  // Theme font references, which win over the named font of the same slot (D7).
  std::string_view theme[4];  // ascii, hAnsi, eastAsia, cs
};

void ApplyRun(const Document& doc, Node rPr, RunWork& w, Toggles& toggles) {
  RunProps& p = w.p;
  for (Node c : rPr.Kids()) {
    uint32_t name = c.NameHash();
    bool toggle = false;
    for (size_t i = 0; i < kToggleCount; ++i)
      if (name == kToggleNames[i]) {
        toggles[i] = c.On();
        toggle = true;
      }
    if (toggle) continue;
    if (name == Name("rFonts")) {
      const uint32_t named[4] = {Name("ascii"), Name("hAnsi"), Name("eastAsia"), Name("cs")};
      const uint32_t themed[4] = {Name("asciiTheme"), Name("hAnsiTheme"), Name("eastAsiaTheme"), Name("cstheme")};
      std::string_view* slots[4] = {&p.fonts.ascii, &p.fonts.hAnsi, &p.fonts.eastAsia, &p.fonts.cs};
      for (size_t i = 0; i < 4; ++i) {
        if (auto t = c.Attr(themed[i])) {
          w.theme[i] = *t;
        } else if (auto n = c.Attr(named[i])) {
          *slots[i] = *n;
          w.theme[i] = {};
        }
      }
      if (auto hint = c.Attr(Name("hint")))
        p.fonts.hint = *hint == "eastAsia" ? FontHint::EastAsia : *hint == "cs" ? FontHint::ComplexScript : FontHint::Default;
    } else if (name == Name("dstrike")) p.doubleStrike = c.On();
    else if (name == Name("noProof")) p.noProof = c.On();
    else if (name == Name("snapToGrid")) p.snapToGrid = c.On();
    else if (name == Name("webHidden")) p.webHidden = c.On();
    else if (name == Name("specVanish")) p.specVanish = c.On();
    else if (name == Name("rtl")) p.rtl = c.On();
    else if (name == Name("cs")) p.complexScript = c.On();
    else if (name == Name("oMath")) p.oMath = c.On();
    else if (name == Name("color")) p.color = ColorOf(doc, c, kVal, Name("themeColor"), Name("themeTint"), Name("themeShade"));
    else if (name == Name("spacing")) p.spacing = Twips(c.Twips(kVal).value_or(0));
    else if (name == Name("w")) {
      std::string_view v = c.AttrOr(kVal);
      if (v.ends_with('%')) v.remove_suffix(1);
      p.scale = static_cast<int32_t>(ParseInt(v).value_or(100));
    } else if (name == Name("kern")) p.kern = HalfPoints(static_cast<int32_t>(ParseMeasure(c.AttrOr(kVal), 2).value_or(0)));
    else if (name == Name("position")) p.position = HalfPoints(static_cast<int32_t>(ParseMeasure(c.AttrOr(kVal), 2).value_or(0)));
    else if (name == Name("sz")) p.size = HalfPoints(static_cast<int32_t>(ParseMeasure(c.AttrOr(kVal), 2).value_or(20)));
    else if (name == Name("szCs")) p.sizeCs = HalfPoints(static_cast<int32_t>(ParseMeasure(c.AttrOr(kVal), 2).value_or(20)));
    else if (name == Name("highlight")) {
      auto rgb = HighlightRgb(c.AttrOr(kVal));
      p.highlight = rgb ? Highlight{true, *rgb} : Highlight{};
    } else if (name == Name("u")) {
      p.underline = ParseUnderline(c.AttrOr(kVal));
      p.underlineColor = ColorOf(doc, c, Name("color"), Name("themeColor"), Name("themeTint"), Name("themeShade"));
    } else if (name == Name("bdr")) p.border = BorderOf(doc, c);
    else if (name == Name("shd")) p.shading = ShadingOf(doc, c);
    else if (name == Name("fitText")) p.fitText = Twips(c.Twips(kVal).value_or(0));
    else if (name == Name("vertAlign")) {
      std::string_view v = c.AttrOr(kVal);
      p.vertAlign = v == "superscript" ? VertAlign::Superscript : v == "subscript" ? VertAlign::Subscript : VertAlign::Baseline;
    } else if (name == Name("em")) {
      std::string_view v = c.AttrOr(kVal);
      p.emphasis = v == "dot" ? Emphasis::Dot : v == "comma" ? Emphasis::Comma : v == "circle" ? Emphasis::Circle
                   : v == "underDot" ? Emphasis::UnderDot : Emphasis::None;
    } else if (name == Name("lang")) {
      if (auto v = c.Attr(kVal)) p.lang = *v;
      if (auto v = c.Attr(Name("eastAsia"))) p.langEastAsia = *v;
      if (auto v = c.Attr(Name("bidi"))) p.langBidi = *v;
    }
  }
}

}  // namespace

// The levels of run resolution: 0 document defaults, 1 table style, 2 paragraph style,
// 3 character style, 4 direct formatting (and for a list label, 5 the level's own w:rPr).
struct RunLevels {
  RunWork work;
  std::array<Toggles, 6> toggles{};
};

namespace {

void FinishRun(const Document& doc, Document::Caches& caches, RunLevels& lv) {
  RunProps& p = lv.work.p;
  bool* fields[kToggleCount] = {&p.bold, &p.boldCs, &p.italic, &p.italicCs, &p.caps, &p.smallCaps,
                                &p.strike, &p.emboss, &p.imprint, &p.outline, &p.shadow, &p.vanish};
  for (size_t t = 0; t < kToggleCount; ++t) {
    // D1: the table and paragraph styles set the value; only the character style toggles it.
    bool v = lv.toggles[0][t].value_or(false);
    if (lv.toggles[1][t]) v = *lv.toggles[1][t];
    if (lv.toggles[2][t]) v = *lv.toggles[2][t];
    if (lv.toggles[3][t].value_or(false)) v = !v;
    if (lv.toggles[4][t]) v = *lv.toggles[4][t];
    if (lv.toggles[5][t]) v = *lv.toggles[5][t];
    *fields[t] = v;
  }
  std::string_view* slots[4] = {&p.fonts.ascii, &p.fonts.hAnsi, &p.fonts.eastAsia, &p.fonts.cs};
  for (size_t i = 0; i < 4; ++i) {
    if (!lv.work.theme[i].empty()) {
      std::string_view face = doc.ThemeFont(lv.work.theme[i]);
      if (!face.empty()) *slots[i] = caches.Own(std::string(face));
    }
    if (slots[i]->empty()) *slots[i] = "Times New Roman";  // D8
  }
}

}  // namespace

// ---------- Tables: the cell a paragraph is in, and its conditional formats ----------

namespace {

Node Up(Node n, uint32_t name) {
  for (Node p = n.Parent(); p; p = p.Parent())
    if (p.Is(name)) return p;
  return {};
}

const Style* TableStyleOf(const Document& doc, Node table) {
  std::string_view id = table.Path({kTblPr, Name("tblStyle")}).AttrOr(kVal);
  const Style* s = id.empty() ? nullptr : doc.FindStyle(id);
  if (s && s->type != StyleType::Table) s = nullptr;
  return s ? s : doc.DefaultStyle(StyleType::Table);
}

TableLook LookOf(Node look) {
  TableLook l;
  if (!look) return l;  // D5: 0x04A0
  const uint32_t named[] = {Name("firstRow"), Name("lastRow"), Name("firstColumn"), Name("lastColumn"),
                            Name("noHBand"), Name("noVBand")};
  bool any = std::any_of(std::begin(named), std::end(named), [&](uint32_t n) { return look.Has(n); });
  if (any) {
    l.firstRow = look.OnOff(named[0]).value_or(false);
    l.lastRow = look.OnOff(named[1]).value_or(false);
    l.firstColumn = look.OnOff(named[2]).value_or(false);
    l.lastColumn = look.OnOff(named[3]).value_or(false);
    l.noHBand = look.OnOff(named[4]).value_or(false);
    l.noVBand = look.OnOff(named[5]).value_or(false);
  } else if (auto bits = look.Hex(kVal)) {
    l.firstRow = *bits & 0x20;
    l.lastRow = *bits & 0x40;
    l.firstColumn = *bits & 0x80;
    l.lastColumn = *bits & 0x100;
    l.noHBand = *bits & 0x200;
    l.noVBand = *bits & 0x400;
  }
  return l;
}

// The tblStylePr types in the order Word applies them (D4), with the condition each needs.
struct CondType {
  std::string_view type;
  uint16_t bit;
};
constexpr CondType kCondOrder[] = {
    {"band1Horz", kCondBand1Horz}, {"band2Horz", kCondBand2Horz}, {"band1Vert", kCondBand1Vert},
    {"band2Vert", kCondBand2Vert}, {"firstCol", kCondFirstCol},   {"lastCol", kCondLastCol},
    {"firstRow", kCondFirstRow},   {"lastRow", kCondLastRow},     {"nwCell", kCondNwCell},
    {"neCell", kCondNeCell},       {"swCell", kCondSwCell},       {"seCell", kCondSeCell},
};

// Calls fn(container) for the table style's whole-table properties and then each conditional
// format that applies, in Word's order; `child` picks pPr, rPr, tblPr, trPr or tcPr.
template <typename Fn>
void ForEachTableStyleLevel(const Document& doc, const Style* style, uint16_t conds, uint32_t child, Fn fn) {
  auto chain = Chain(doc, style);
  for (const Style* s : chain)
    if (Node n = s->node.Child(child)) fn(n);
  for (const CondType& ct : kCondOrder) {
    if (!(conds & ct.bit)) continue;
    for (const Style* s : chain)
      for (Node c : s->node.Kids())
        if (c.Is(Name("tblStylePr")) && c.AttrOr(kType) == ct.type)
          if (Node n = c.Child(child)) fn(n);
  }
}

// The conditional formats that apply to a cell.
uint16_t CellConditions(const Document& doc, Node cell, const TableProps& tp) {
  Node row = Up(cell, kTr), table = Up(cell, kTbl);
  auto rows = doc.TableRows(table);
  auto cells = doc.RowCells(row);
  int32_t r = static_cast<int32_t>(std::find(rows.begin(), rows.end(), row) - rows.begin());
  int32_t c = static_cast<int32_t>(std::find(cells.begin(), cells.end(), cell) - cells.begin());
  int32_t nr = static_cast<int32_t>(rows.size()), nc = static_cast<int32_t>(cells.size());
  const TableLook& look = tp.look;
  uint16_t bits = 0;
  // Header rows: the first row, and the rows marked as repeating headers that follow it.
  int32_t headers = 0;
  if (look.firstRow) {
    headers = 1;
    while (headers < nr && rows[headers].Path({kTrPr, Name("tblHeader")}) &&
           rows[headers].Path({kTrPr, Name("tblHeader")}).On() && rows[headers - 1].Path({kTrPr, Name("tblHeader")}))
      ++headers;
  }
  bool firstRow = look.firstRow && r < headers;
  bool lastRow = look.lastRow && r == nr - 1 && !firstRow;
  bool firstCol = look.firstColumn && c == 0;
  bool lastCol = look.lastColumn && c == nc - 1 && !firstCol;
  if (firstRow) bits |= kCondFirstRow;
  if (lastRow) bits |= kCondLastRow;
  if (firstCol) bits |= kCondFirstCol;
  if (lastCol) bits |= kCondLastCol;
  // Banding skips the header rows and the first column when those formats are on.
  if (!look.noHBand && tp.rowBandSize > 0 && !firstRow && !lastRow) {
    int32_t band = (r - headers) / tp.rowBandSize;
    bits |= band % 2 == 0 ? kCondBand1Horz : kCondBand2Horz;
  }
  if (!look.noVBand && tp.colBandSize > 0 && !firstCol && !lastCol) {
    int32_t band = (c - (look.firstColumn ? 1 : 0)) / tp.colBandSize;
    bits |= band % 2 == 0 ? kCondBand1Vert : kCondBand2Vert;
  }
  // D4: a corner needs both its row and its column format.
  if (firstRow && firstCol) bits |= kCondNwCell;
  if (firstRow && lastCol) bits |= kCondNeCell;
  if (lastRow && firstCol) bits |= kCondSwCell;
  if (lastRow && lastCol) bits |= kCondSeCell;
  return bits;
}

void ApplyTable(const Document& doc, Node pr, TableProps& t, bool direct) {
  for (Node c : pr.Kids()) {
    uint32_t name = c.NameHash();
    if (name == Name("tblStyle")) t.style = c.AttrOr(kVal);
    else if (name == Name("tblpPr")) {
      TableFloat f = t.floating.value_or(TableFloat{});
      f.leftFromText = Twips(c.Twips(Name("leftFromText")).value_or(0));
      f.rightFromText = Twips(c.Twips(Name("rightFromText")).value_or(0));
      f.topFromText = Twips(c.Twips(Name("topFromText")).value_or(0));
      f.bottomFromText = Twips(c.Twips(Name("bottomFromText")).value_or(0));
      f.vertAnchor = c.AttrOr(Name("vertAnchor"));
      f.horzAnchor = c.AttrOr(Name("horzAnchor"));
      f.xSpec = c.AttrOr(Name("tblpXSpec"));
      f.ySpec = c.AttrOr(Name("tblpYSpec"));
      f.x = Twips(c.Twips(Name("tblpX")).value_or(0));
      f.y = Twips(c.Twips(Name("tblpY")).value_or(0));
      t.floating = f;
    } else if (name == Name("tblOverlap")) {
      TableFloat f = t.floating.value_or(TableFloat{});
      f.overlap = c.AttrOr(kVal) != "never";
      if (t.floating) t.floating = f;
    } else if (name == Name("bidiVisual")) t.bidiVisual = c.On();
    else if (name == Name("tblStyleRowBandSize")) t.rowBandSize = static_cast<int32_t>(c.Int(kVal).value_or(0));
    else if (name == Name("tblStyleColBandSize")) t.colBandSize = static_cast<int32_t>(c.Int(kVal).value_or(0));
    else if (name == Name("tblW")) t.width = ParseTableWidth(c);
    else if (name == Name("jc")) t.jc = ParseJustification(c.AttrOr(kVal));
    else if (name == Name("tblCellSpacing")) t.cellSpacing = ParseTableWidth(c);
    else if (name == Name("tblInd")) t.indent = ParseTableWidth(c);
    else if (name == Name("tblBorders")) {
      for (Node b : c.Kids()) {
        uint32_t side = b.NameHash();
        Border v = BorderOf(doc, b);
        if (side == Name("top")) t.borders.top = v;
        else if (side == Name("left") || side == Name("start")) t.borders.start = v;
        else if (side == Name("bottom")) t.borders.bottom = v;
        else if (side == Name("right") || side == Name("end")) t.borders.end = v;
        else if (side == Name("insideH")) t.borders.insideH = v;
        else if (side == Name("insideV")) t.borders.insideV = v;
      }
    } else if (name == Name("shd")) t.shading = ShadingOf(doc, c);
    else if (name == Name("tblLayout")) t.fixedLayout = c.AttrOr(kType) == "fixed";
    else if (name == Name("tblCellMar")) {
      for (Node m : c.Kids()) {
        uint32_t side = m.NameHash();
        TableWidth v = ParseTableWidth(m);
        if (side == Name("top")) t.cellMargins.top = v;
        else if (side == Name("left") || side == Name("start")) t.cellMargins.start = v;
        else if (side == Name("bottom")) t.cellMargins.bottom = v;
        else if (side == Name("right") || side == Name("end")) t.cellMargins.end = v;
      }
    } else if (name == Name("tblLook") && direct) t.look = LookOf(c);
    else if (name == Name("tblCaption")) t.caption = c.AttrOr(kVal);
    else if (name == Name("tblDescription")) t.description = c.AttrOr(kVal);
  }
}

void ApplyRow(Node pr, RowProps& r) {
  for (Node c : pr.Kids()) {
    uint32_t name = c.NameHash();
    if (name == Name("gridBefore")) r.gridBefore = static_cast<int32_t>(c.Int(kVal).value_or(0));
    else if (name == Name("gridAfter")) r.gridAfter = static_cast<int32_t>(c.Int(kVal).value_or(0));
    else if (name == Name("wBefore")) r.widthBefore = ParseTableWidth(c);
    else if (name == Name("wAfter")) r.widthAfter = ParseTableWidth(c);
    else if (name == Name("cantSplit")) r.cantSplit = c.On();
    else if (name == Name("trHeight")) {
      r.height = Twips(c.Twips(kVal).value_or(0));
      r.heightRule = c.AttrOr(Name("hRule"), "atLeast");
    } else if (name == Name("tblHeader")) r.header = c.On();
    else if (name == Name("tblCellSpacing")) r.cellSpacing = ParseTableWidth(c);
    else if (name == Name("jc")) r.jc = ParseJustification(c.AttrOr(kVal));
    else if (name == Name("hidden")) r.hidden = c.On();
  }
}

void ApplyCell(const Document& doc, Node pr, CellProps& p) {
  for (Node c : pr.Kids()) {
    uint32_t name = c.NameHash();
    if (name == Name("tcW")) p.width = ParseTableWidth(c);
    else if (name == Name("gridSpan")) p.gridSpan = static_cast<int32_t>(std::max<int64_t>(1, c.Int(kVal).value_or(1)));
    else if (name == Name("vMerge") || name == Name("hMerge")) {
      VMerge m = c.AttrOr(kVal, "continue") == "restart" ? VMerge::Restart : VMerge::Continue;
      (name == Name("vMerge") ? p.vMerge : p.hMerge) = m;
    } else if (name == Name("tcBorders")) {
      for (Node b : c.Kids()) {
        uint32_t side = b.NameHash();
        Border v = BorderOf(doc, b);
        if (side == Name("top")) p.top = v;
        else if (side == Name("left") || side == Name("start")) p.start = v;
        else if (side == Name("bottom")) p.bottom = v;
        else if (side == Name("right") || side == Name("end")) p.end = v;
        else if (side == Name("insideH")) p.insideH = v;
        else if (side == Name("insideV")) p.insideV = v;
        else if (side == Name("tl2br")) p.tl2br = v;
        else if (side == Name("tr2bl")) p.tr2bl = v;
      }
    } else if (name == Name("shd")) p.shading = ShadingOf(doc, c);
    else if (name == Name("noWrap")) p.noWrap = c.On();
    else if (name == Name("tcMar")) {
      for (Node m : c.Kids()) {
        uint32_t side = m.NameHash();
        TableWidth v = ParseTableWidth(m);
        if (side == Name("top")) p.margins.top = v;
        else if (side == Name("left") || side == Name("start")) p.margins.start = v;
        else if (side == Name("bottom")) p.margins.bottom = v;
        else if (side == Name("right") || side == Name("end")) p.margins.end = v;
      }
    } else if (name == Name("textDirection")) p.textDirection = c.AttrOr(kVal);
    else if (name == Name("tcFitText")) p.fitText = c.On();
    else if (name == Name("vAlign")) p.vAlign = c.AttrOr(kVal);
    else if (name == Name("hideMark")) p.hideMark = c.On();
  }
}

}  // namespace

TableProps Document::ResolvedTable(Node table) const {
  TableProps t;
  t.cellMargins.start = t.cellMargins.end = TableWidth{TableWidth::Type::Dxa, 108};
  t.cellMargins.top = t.cellMargins.bottom = TableWidth{TableWidth::Type::Dxa, 0};
  const Style* style = TableStyleOf(*this, table);
  for (const Style* s : Chain(*this, style))
    if (Node pr = s->node.Child(kTblPr)) ApplyTable(*this, pr, t, false);
  ApplyTable(*this, table.Child(kTblPr), t, true);
  if (!table.Path({kTblPr, Name("tblLook")})) t.look = TableLook{};
  if (style && !Ignored(style->id)) t.style = style->id;
  return t;
}

RowProps Document::ResolvedRow(Node row) const {
  RowProps r;
  Node table = Up(row, kTbl);
  TableProps tp = ResolvedTable(table);
  auto cells = RowCells(row);
  uint16_t conds = cells.empty() ? 0 : CellConditions(*this, cells.front(), tp);
  conds &= kCondFirstRow | kCondLastRow | kCondBand1Horz | kCondBand2Horz;  // row formats only
  ForEachTableStyleLevel(*this, TableStyleOf(*this, table), conds, kTrPr, [&](Node n) { ApplyRow(n, r); });
  ApplyRow(row.Child(kTrPr), r);
  return r;
}

CellProps Document::ResolvedCell(Node cell) const {
  CellProps p;
  Node row = Up(cell, kTr), table = Up(cell, kTbl);
  TableProps tp = ResolvedTable(table);
  // Row exceptions (w:tblPrEx) change the table's borders and margins for one row.
  if (Node ex = row.Child(Name("tblPrEx"))) ApplyTable(*this, ex, tp, false);
  auto rows = TableRows(table);
  auto cells = RowCells(row);
  bool firstRow = !rows.empty() && rows.front() == row, lastRow = !rows.empty() && rows.back() == row;
  bool firstCell = !cells.empty() && cells.front() == cell, lastCell = !cells.empty() && cells.back() == cell;
  // Outer edges take the table's borders, inner edges its inside borders.
  p.top = firstRow ? tp.borders.top : tp.borders.insideH;
  p.bottom = lastRow ? tp.borders.bottom : tp.borders.insideH;
  p.start = firstCell ? tp.borders.start : tp.borders.insideV;
  p.end = lastCell ? tp.borders.end : tp.borders.insideV;
  p.insideH = tp.borders.insideH;
  p.insideV = tp.borders.insideV;
  p.margins = tp.cellMargins;
  p.conditions = CellConditions(*this, cell, tp);
  ForEachTableStyleLevel(*this, TableStyleOf(*this, table), p.conditions, kTcPr, [&](Node n) { ApplyCell(*this, n, p); });
  ApplyCell(*this, cell.Child(kTcPr), p);
  return p;
}

// ---------- Paragraphs ----------

namespace {

void ApplyTabs(Node tabs, std::vector<TabStop>& out) {
  for (Node t : tabs.Kids()) {
    if (!t.Is(Name("tab"))) continue;
    TabStop stop;
    stop.pos = Twips(t.Twips(Name("pos")).value_or(0));
    std::string_view v = t.AttrOr(kVal);
    stop.align = v == "center" ? TabAlign::Center : v == "right" || v == "end" ? TabAlign::End
                 : v == "decimal" ? TabAlign::Decimal : v == "bar" ? TabAlign::Bar : v == "num" ? TabAlign::Num
                 : v == "clear" ? TabAlign::Clear : TabAlign::Start;
    std::string_view l = t.AttrOr(Name("leader"));
    stop.leader = l == "dot" ? TabLeader::Dot : l == "hyphen" ? TabLeader::Hyphen : l == "underscore" ? TabLeader::Underscore
                  : l == "heavy" ? TabLeader::Heavy : l == "middleDot" ? TabLeader::MiddleDot : TabLeader::None;
    out.erase(std::remove_if(out.begin(), out.end(), [&](const TabStop& s) { return s.pos == stop.pos; }), out.end());
    if (stop.align != TabAlign::Clear) out.push_back(stop);
  }
}

void ApplyPara(const Document& doc, Node pPr, ParaProps& p) {
  for (Node c : pPr.Kids()) {
    uint32_t name = c.NameHash();
    if (name == Name("keepNext")) p.keepNext = c.On();
    else if (name == Name("keepLines")) p.keepLines = c.On();
    else if (name == Name("pageBreakBefore")) p.pageBreakBefore = c.On();
    else if (name == Name("widowControl")) p.widowControl = c.On();
    else if (name == Name("suppressLineNumbers")) p.suppressLineNumbers = c.On();
    else if (name == Name("suppressAutoHyphens")) p.suppressAutoHyphens = c.On();
    else if (name == Name("suppressOverlap")) p.suppressOverlap = c.On();
    else if (name == Name("kinsoku")) p.kinsoku = c.On();
    else if (name == Name("wordWrap")) p.wordWrap = c.On();
    else if (name == Name("overflowPunct")) p.overflowPunct = c.On();
    else if (name == Name("topLinePunct")) p.topLinePunct = c.On();
    else if (name == Name("autoSpaceDE")) p.autoSpaceDE = c.On();
    else if (name == Name("autoSpaceDN")) p.autoSpaceDN = c.On();
    else if (name == Name("bidi")) p.bidi = c.On();
    else if (name == Name("adjustRightInd")) p.adjustRightInd = c.On();
    else if (name == Name("snapToGrid")) p.snapToGrid = c.On();
    else if (name == Name("contextualSpacing")) p.contextualSpacing = c.On();
    else if (name == Name("mirrorIndents")) p.mirrorIndents = c.On();
    else if (name == Name("framePr")) {
      FrameProps f;
      f.dropCap = c.AttrOr(Name("dropCap"));
      f.wrap = c.AttrOr(Name("wrap"));
      f.hAnchor = c.AttrOr(Name("hAnchor"));
      f.vAnchor = c.AttrOr(Name("vAnchor"));
      f.xAlign = c.AttrOr(Name("xAlign"));
      f.yAlign = c.AttrOr(Name("yAlign"));
      f.hRule = c.AttrOr(Name("hRule"));
      f.lines = static_cast<int32_t>(c.Int(Name("lines")).value_or(0));
      f.w = Twips(c.Twips(Name("w")).value_or(0));
      f.h = Twips(c.Twips(Name("h")).value_or(0));
      f.x = Twips(c.Twips(Name("x")).value_or(0));
      f.y = Twips(c.Twips(Name("y")).value_or(0));
      f.hSpace = Twips(c.Twips(Name("hSpace")).value_or(0));
      f.vSpace = Twips(c.Twips(Name("vSpace")).value_or(0));
      f.anchorLock = c.OnOff(Name("anchorLock")).value_or(false);
      p.frame = f;
    } else if (name == Name("pBdr")) {
      for (Node b : c.Kids()) {
        uint32_t side = b.NameHash();
        Border v = BorderOf(doc, b);
        if (side == Name("top")) p.borders.top = v;
        else if (side == Name("left")) p.borders.start = v;
        else if (side == Name("bottom")) p.borders.bottom = v;
        else if (side == Name("right")) p.borders.end = v;
        else if (side == Name("between")) p.borders.between = v;
        else if (side == Name("bar")) p.borders.bar = v;
      }
    } else if (name == Name("shd")) p.shading = ShadingOf(doc, c);
    else if (name == Name("tabs")) ApplyTabs(c, p.tabs);
    else if (name == Name("spacing")) {
      if (auto v = c.Twips(Name("before"))) p.spaceBefore = Twips(*v);
      if (auto v = c.Twips(Name("after"))) p.spaceAfter = Twips(*v);
      if (auto v = c.Int(Name("beforeLines"))) p.spaceBeforeLines = static_cast<int32_t>(*v);
      if (auto v = c.Int(Name("afterLines"))) p.spaceAfterLines = static_cast<int32_t>(*v);
      if (auto v = c.OnOff(Name("beforeAutospacing"))) p.spaceBeforeAuto = *v;
      if (auto v = c.OnOff(Name("afterAutospacing"))) p.spaceAfterAuto = *v;
      if (auto v = c.Twips(Name("line"))) p.line = *v;
      if (auto v = c.Attr(Name("lineRule")))
        p.lineRule = *v == "exact" ? LineRule::Exact : *v == "atLeast" ? LineRule::AtLeast : LineRule::Auto;
    } else if (name == Name("ind")) {
      if (auto v = c.Twips(Name("start"))) p.indStart = Twips(*v);
      else if (auto l = c.Twips(Name("left"))) p.indStart = Twips(*l);
      if (auto v = c.Twips(Name("end"))) p.indEnd = Twips(*v);
      else if (auto r = c.Twips(Name("right"))) p.indEnd = Twips(*r);
      // Hanging and first line exclude each other; hanging wins in one element.
      if (auto h = c.Twips(Name("hanging"))) p.firstLine = Twips(-*h);
      else if (auto f = c.Twips(Name("firstLine"))) p.firstLine = Twips(*f);
      if (auto v = c.Int(Name("startChars"))) p.indStartChars = static_cast<int32_t>(*v);
      else if (auto l = c.Int(Name("leftChars"))) p.indStartChars = static_cast<int32_t>(*l);
      if (auto v = c.Int(Name("endChars"))) p.indEndChars = static_cast<int32_t>(*v);
      else if (auto r = c.Int(Name("rightChars"))) p.indEndChars = static_cast<int32_t>(*r);
      if (auto h = c.Int(Name("hangingChars"))) p.firstLineChars = static_cast<int32_t>(-*h);
      else if (auto f = c.Int(Name("firstLineChars"))) p.firstLineChars = static_cast<int32_t>(*f);
    } else if (name == Name("jc")) p.jc = ParseJustification(c.AttrOr(kVal));
    else if (name == Name("textDirection")) p.textDirection = c.AttrOr(kVal);
    else if (name == Name("textAlignment")) p.textAlignment = c.AttrOr(kVal);
    else if (name == Name("outlineLvl")) p.outlineLevel = static_cast<int32_t>(std::clamp<int64_t>(c.Int(kVal).value_or(9), 0, 9));
  }
}

constexpr uint32_t kPPrChange = Name("pPrChange"), kRPrChange = Name("rPrChange");

// A paragraph's own w:pPr; `original`: the one it had before a tracked change, its w:pPrChange's
// (none if that's empty).
Node DirectPPr(Node para, bool original) {
  Node p = para.Child(kPPr);
  if (original)
    if (Node change = p.Child(kPPrChange)) return change.Child(kPPr);
  return p;
}

// Direct run formatting (a run's w:rPr, or a mark's); `original`: before a tracked change.
Node DirectRPr(Node rPr, bool original) {
  if (original)
    if (Node change = rPr.Child(kRPrChange)) return change.Child(kRPr);
  return rPr;
}

const Style* ParaStyleIn(const Document& doc, Node pPr) {
  std::string_view id = pPr.Child(Name("pStyle")).AttrOr(kVal);
  const Style* s = id.empty() ? nullptr : doc.FindStyle(id);
  if (s && s->type != StyleType::Paragraph) s = nullptr;
  return s ? s : doc.DefaultStyle(StyleType::Paragraph);
}

const Style* ParaStyleOf(const Document& doc, Node para) { return ParaStyleIn(doc, para.Child(kPPr)); }

bool OverrideTableStyleFontSize(const Document& doc) {
  for (Node cs : doc.Settings().compat.Kids())
    if (cs.Is(Name("compatSetting")) && cs.AttrOr(Name("name")) == "overrideTableStyleFontSizeAndJustification")
      return ParseOnOff(cs.AttrOr(kVal)).value_or(false);
  return false;
}

// What the table style contributes to a paragraph in a cell: (style, conditions), or none.
std::pair<const Style*, uint16_t> CellStyleOf(const Document& doc, Node para) {
  Node cell = Up(para, kTc);
  if (!cell) return {nullptr, 0};
  Node table = Up(cell, kTbl);
  TableProps tp = doc.ResolvedTable(table);
  return {TableStyleOf(doc, table), CellConditions(doc, cell, tp)};
}

// The list a paragraph is in: numId and ilvl from the closest that names them, direct formatting
// first. Empty numId: none.
struct ParaList {
  std::string_view numId;
  int32_t level = 0;
  Node lvl;
  bool numDirect = false;      // named in the paragraph's own w:pPr
  const Style* style = nullptr;  // otherwise the style that names the w:numId
};
ParaList ListOf(const Document& doc, Node direct, const std::vector<const Style*>& chain) {
  ParaList out;
  std::string_view numId;
  std::optional<int64_t> ilvl;
  bool numDirect = false;
  if (Node np = direct.Child(Name("numPr"))) {
    if (Node n = np.Child(Name("numId"))) {
      numId = n.AttrOr(kVal);
      numDirect = true;
    }
    if (Node l = np.Child(Name("ilvl"))) ilvl = l.Int(kVal);
  }
  // A style can set only the level and take the list from the style it's based on (Heading 2 with
  // w:ilvl 1, based on Heading 1 with the w:numId): the closest w:ilvl on the way to the w:numId.
  const Style* numStyle = nullptr;
  for (size_t i = chain.size(); i-- > 0 && (numId.empty() && !numDirect);) {
    Node np = chain[i]->node.Path({kPPr, Name("numPr")});
    if (!ilvl)
      if (Node l = np.Child(Name("ilvl"))) ilvl = l.Int(kVal);
    if (Node n = np.Child(Name("numId"))) {
      numId = n.AttrOr(kVal);
      numStyle = chain[i];
    }
  }
  if (!numId.empty() && numId != "0") {
    if (!ilvl && numStyle) {
      // A style linked to a list level by the level's w:pStyle.
      for (int32_t k = 0; k < 9 && !ilvl; ++k) {
        Node l = doc.NumberingLevel(numId, k);
        if (l && l.Child(Name("pStyle")).AttrOr(kVal) == numStyle->id) ilvl = k;
      }
    }
    int32_t level = static_cast<int32_t>(std::clamp<int64_t>(ilvl.value_or(0), 0, 8));
    if (Node lvl = doc.NumberingLevel(numId, level)) {
      out.numId = numId;
      out.level = level;
      out.lvl = lvl;
    }
  }
  out.numDirect = numDirect;
  out.style = numStyle;
  return out;
}

// A paragraph's properties; without its own w:pPr when !withDirect (but still in its list, whose
// level gives it indents); `original`: with the w:pPr it had before a tracked change.
ParaProps ResolveParaProps(const Document& doc, uint32_t para, bool withDirect, bool original = false) {
  Node pn = doc.ParaNode(para);
  Node direct = DirectPPr(pn, original);
  ParaProps p;
  const Style* style = ParaStyleIn(doc, direct);
  auto chain = Chain(doc, style);

  const ParaList list = ListOf(doc, direct, chain);
  const Node lvl = list.lvl;
  const bool numDirect = list.numDirect;
  if (lvl) {
    p.numId = list.numId;
    p.ilvl = list.level;
  }

  if (const Part* styles = doc.FindPart(PartKind::Styles))
    ApplyPara(doc, doc.RootOf(*styles).Path({Name("docDefaults"), Name("pPrDefault"), kPPr}), p);
  auto [tableStyle, conds] = CellStyleOf(doc, pn);
  if (tableStyle) ForEachTableStyleLevel(doc, tableStyle, conds, kPPr, [&](Node n) { ApplyPara(doc, n, p); });
  Justification tableJc = p.jc;
  // D2: a list named by a style, just before that style: its level's indents override the styles
  // it's based on (a list style based on List Paragraph) but not its own (measured on the corpus).
  if (lvl && !numDirect && !list.style) ApplyPara(doc, lvl.Child(kPPr), p);
  for (const Style* s : chain) {
    if (lvl && !numDirect && s == list.style) ApplyPara(doc, lvl.Child(kPPr), p);
    ApplyPara(doc, s->node.Child(kPPr), p);
  }
  // D3: the default style's left justification doesn't override the table style's.
  if (tableStyle && style && style->isDefault && !OverrideTableStyleFontSize(doc)) {
    Node jc = style->node.Path({kPPr, Name("jc")});
    if (jc && ParseJustification(jc.AttrOr(kVal)) == Justification::Start) p.jc = tableJc;
  }
  if (lvl && numDirect) ApplyPara(doc, lvl.Child(kPPr), p);  // D2: named directly, after the style
  // A paragraph that takes itself out of its style's list (w:numId 0) loses the style's indents
  // too, as in Word: they belong to the list (measured on the corpus).
  if (!lvl && direct.Path({Name("numPr"), Name("numId")}).AttrOr(kVal) == "0") {
    bool styleList = false;
    for (const Style* s : chain)
      if (Node n = s->node.Path({kPPr, Name("numPr"), Name("numId")}); n && n.AttrOr(kVal) != "0") styleList = true;
    if (styleList) {
      p.indStart = Twips(0);
      p.firstLine = Twips(0);
      p.indStartChars = -1;
      p.firstLineChars = -1;
    }
  }
  if (withDirect) ApplyPara(doc, direct, p);
  std::sort(p.tabs.begin(), p.tabs.end(), [](const TabStop& a, const TabStop& b) { return a.pos < b.pos; });
  if (style) p.style = style->id;
  return p;
}

}  // namespace

const ParaProps& Document::ResolvedPara(uint32_t para) const {
  Caches& c = *caches_;
  if (c.para.size() < paras_.size()) c.para.resize(paras_.size(), kNone);
  if (c.para[para] != kNone) return c.paraPool[c.para[para]];
  c.para[para] = static_cast<uint32_t>(c.paraPool.size());
  c.paraPool.push_back(ResolveParaProps(*this, para, true));
  return c.paraPool.back();
}

const ParaProps& Document::OriginalPara(uint32_t para) const {
  if (!ParaNode(para).Path({kPPr, kPPrChange})) return ResolvedPara(para);
  Caches& c = *caches_;
  if (c.originalPara.size() < paras_.size()) c.originalPara.resize(paras_.size(), kNone);
  if (c.originalPara[para] != kNone) return c.paraPool[c.originalPara[para]];
  c.originalPara[para] = static_cast<uint32_t>(c.paraPool.size());
  c.paraPool.push_back(ResolveParaProps(*this, para, true, true));
  return c.paraPool.back();
}

bool Document::InList(uint32_t para) const {
  const Caches& c = *caches_;
  if (para < c.para.size() && c.para[para] != kNone) return !c.paraPool[c.para[para]].numId.empty();
  const Node pn = ParaNode(para);
  return !ListOf(*this, pn.Child(kPPr), Chain(*this, ParaStyleOf(*this, pn))).numId.empty();
}

ParaProps Document::StylePara(uint32_t para) const { return ResolveParaProps(*this, para, false); }

ParaProps Document::StyleChainPara(std::string_view styleId) const {
  ParaProps p;
  if (const Part* styles = FindPart(PartKind::Styles))
    ApplyPara(*this, RootOf(*styles).Path({Name("docDefaults"), Name("pPrDefault"), kPPr}), p);
  const Style* s = styleId.empty() ? nullptr : FindStyle(styleId);
  if (s && s->type == StyleType::Paragraph) {
    for (const Style* st : Chain(*this, s)) ApplyPara(*this, st->node.Child(kPPr), p);
    p.style = s->id;
  }
  std::sort(p.tabs.begin(), p.tabs.end(), [](const TabStop& a, const TabStop& b) { return a.pos < b.pos; });
  return p;
}

namespace {

// Levels 0-2 of a run in this paragraph: document defaults, table style, paragraph style (the one
// it had before a tracked change when `original`).
void ParagraphLevels(const Document& doc, Node pn, RunLevels& lv, bool original = false) {
  if (const Part* styles = doc.FindPart(PartKind::Styles))
    ApplyRun(doc, doc.RootOf(*styles).Path({Name("docDefaults"), Name("rPrDefault"), kRPr}), lv.work, lv.toggles[0]);
  auto [tableStyle, conds] = CellStyleOf(doc, pn);
  std::optional<HalfPoints> tableSize;
  if (tableStyle) {
    RunProps before = lv.work.p;
    ForEachTableStyleLevel(doc, tableStyle, conds, kRPr, [&](Node n) { ApplyRun(doc, n, lv.work, lv.toggles[1]); });
    if (!(lv.work.p.size == before.size)) tableSize = lv.work.p.size;
  }
  const Style* style = ParaStyleIn(doc, DirectPPr(pn, original));
  for (const Style* s : Chain(doc, style)) ApplyRun(doc, s->node.Child(kRPr), lv.work, lv.toggles[2]);
  // D3: the default style's 11 or 12 pt doesn't override the table style's size.
  if (tableSize && style && style->isDefault && !OverrideTableStyleFontSize(doc)) {
    auto sz = style->node.Path({kRPr, Name("sz")}).Int(kVal);
    if (sz && (*sz == 22 || *sz == 24)) lv.work.p.size = *tableSize;
  }
}

void CharacterStyleLevel(const Document& doc, std::string_view id, RunLevels& lv) {
  const Style* s = id.empty() ? nullptr : doc.FindStyle(id);
  if (!s || s->type != StyleType::Character) return;
  for (const Style* st : Chain(doc, s)) ApplyRun(doc, st->node.Child(kRPr), lv.work, lv.toggles[3]);
  lv.work.p.charStyle = s->id;
}

void CharacterLevel(const Document& doc, Node rPrNode, RunLevels& lv) {
  CharacterStyleLevel(doc, rPrNode.Child(Name("rStyle")).AttrOr(kVal), lv);
}

}  // namespace

RunProps Document::StyleRun(uint32_t para, std::string_view charStyle) const {
  RunLevels lv;
  ParagraphLevels(*this, ParaNode(para), lv);
  CharacterStyleLevel(*this, charStyle, lv);
  FinishRun(*this, *caches_, lv);
  return lv.work.p;
}

RunProps Document::StyleChainRun(std::string_view styleId, std::string_view charStyle) const {
  RunLevels lv;
  if (const Part* styles = FindPart(PartKind::Styles))
    ApplyRun(*this, RootOf(*styles).Path({Name("docDefaults"), Name("rPrDefault"), kRPr}), lv.work, lv.toggles[0]);
  const Style* s = styleId.empty() ? nullptr : FindStyle(styleId);
  if (s && (s->type == StyleType::Paragraph || s->type == StyleType::Character)) {
    size_t level = s->type == StyleType::Character ? 3 : 2;
    for (const Style* st : Chain(*this, s)) ApplyRun(*this, st->node.Child(kRPr), lv.work, lv.toggles[level]);
    if (level == 3) lv.work.p.charStyle = s->id;
  }
  CharacterStyleLevel(*this, charStyle, lv);
  FinishRun(*this, *caches_, lv);
  return lv.work.p;
}

const RunProps& Document::ResolvedRun(uint32_t para, Node run) const {
  Caches& c = *caches_;
  if (auto it = c.run.find(run.Id()); it != c.run.end()) return c.runPool[it->second];
  RunLevels lv;
  ParagraphLevels(*this, ParaNode(para), lv);
  Node direct = run.Child(kRPr);
  CharacterLevel(*this, direct, lv);
  ApplyRun(*this, direct, lv.work, lv.toggles[4]);
  FinishRun(*this, c, lv);
  // Runs with the same result share one entry.
  uint32_t index = kNone;
  for (uint32_t i = c.runPool.size() > 64 ? static_cast<uint32_t>(c.runPool.size() - 64) : 0; i < c.runPool.size(); ++i)
    if (c.runPool[i] == lv.work.p) index = i;
  if (index == kNone) {
    index = static_cast<uint32_t>(c.runPool.size());
    c.runPool.push_back(lv.work.p);
  }
  c.run[run.Id()] = index;
  return c.runPool[index];
}

namespace {

// A run's (or a mark's, `rPr`) properties as they were before tracked changes: its own w:rPrChange,
// and its paragraph's w:pPrChange (for the paragraph style).
RunProps OriginalRunProps(const Document& doc, Document::Caches& c, uint32_t para, Node rPr) {
  RunLevels lv;
  ParagraphLevels(doc, doc.ParaNode(para), lv, true);
  Node direct = DirectRPr(rPr, true);
  CharacterLevel(doc, direct, lv);
  ApplyRun(doc, direct, lv.work, lv.toggles[4]);
  FinishRun(doc, c, lv);
  return lv.work.p;
}

}  // namespace

const RunProps& Document::OriginalRun(uint32_t para, Node run) const {
  Node rPr = run.Child(kRPr);
  if (!rPr.Child(kRPrChange) && !ParaNode(para).Path({kPPr, kPPrChange})) return ResolvedRun(para, run);
  Caches& c = *caches_;
  if (auto it = c.originalRun.find(run.Id()); it != c.originalRun.end()) return c.runPool[it->second];
  const auto index = static_cast<uint32_t>(c.runPool.size());
  c.runPool.push_back(OriginalRunProps(*this, c, para, rPr));
  c.originalRun[run.Id()] = index;
  return c.runPool[index];
}

const RunProps& Document::OriginalMark(uint32_t para) const {
  Node pPr = ParaNode(para).Child(kPPr);
  Node rPr = pPr.Child(kRPr);
  if (!rPr.Child(kRPrChange) && !pPr.Child(kPPrChange)) return ResolvedMark(para);
  Caches& c = *caches_;
  if (c.originalMark.size() < paras_.size()) c.originalMark.resize(paras_.size(), kNone);
  if (c.originalMark[para] != kNone) return c.runPool[c.originalMark[para]];
  c.originalMark[para] = static_cast<uint32_t>(c.runPool.size());
  c.runPool.push_back(OriginalRunProps(*this, c, para, rPr));
  return c.runPool.back();
}

const RunProps& Document::ResolvedMark(uint32_t para) const {
  Caches& c = *caches_;
  if (c.mark.size() < paras_.size()) c.mark.resize(paras_.size(), kNone);
  if (c.mark[para] != kNone) return c.runPool[c.mark[para]];
  RunLevels lv;
  ParagraphLevels(*this, ParaNode(para), lv);
  Node direct = ParaNode(para).Path({kPPr, kRPr});
  CharacterLevel(*this, direct, lv);
  ApplyRun(*this, direct, lv.work, lv.toggles[4]);
  FinishRun(*this, c, lv);
  c.mark[para] = static_cast<uint32_t>(c.runPool.size());
  c.runPool.push_back(lv.work.p);
  return c.runPool.back();
}

RunProps LabelProps(const Document& doc, Document::Caches& caches, uint32_t para, Node lvl) {
  RunLevels lv;
  ParagraphLevels(doc, doc.ParaNode(para), lv);
  Node mark = doc.ParaNode(para).Path({kPPr, kRPr});
  CharacterLevel(doc, mark, lv);
  ApplyRun(doc, mark, lv.work, lv.toggles[4]);
  ApplyRun(doc, lvl.Child(kRPr), lv.work, lv.toggles[5]);
  FinishRun(doc, caches, lv);
  lv.work.p.charStyle = {};
  return lv.work.p;
}

// ---------- Sections ----------

SectionProps ResolveSection(Node sp) {
  SectionProps s;
  for (Node c : sp.Kids()) {
    uint32_t name = c.NameHash();
    if (name == Name("type")) {
      std::string_view v = c.AttrOr(kVal, "nextPage");
      s.start = v == "continuous" ? SectionStart::Continuous : v == "evenPage" ? SectionStart::EvenPage
                : v == "oddPage" ? SectionStart::OddPage : v == "nextColumn" ? SectionStart::NextColumn
                                                         : SectionStart::NextPage;
    } else if (name == Name("pgSz")) {
      if (auto v = c.Twips(kW)) s.pageWidth = Twips(*v);
      if (auto v = c.Twips(Name("h"))) s.pageHeight = Twips(*v);
      s.landscape = c.AttrOr(Name("orient")) == "landscape";
      s.paperCode = static_cast<int32_t>(c.Int(Name("code")).value_or(0));
    } else if (name == Name("pgMar")) {
      if (auto v = c.Twips(Name("top"))) s.marginTop = Twips(*v);
      if (auto v = c.Twips(Name("right"))) s.marginEnd = Twips(*v);
      if (auto v = c.Twips(Name("bottom"))) s.marginBottom = Twips(*v);
      if (auto v = c.Twips(Name("left"))) s.marginStart = Twips(*v);
      if (auto v = c.Twips(Name("header"))) s.headerDistance = Twips(*v);
      if (auto v = c.Twips(Name("footer"))) s.footerDistance = Twips(*v);
      if (auto v = c.Twips(Name("gutter"))) s.gutter = Twips(*v);
    } else if (name == Name("cols")) {
      s.columns.count = static_cast<int32_t>(std::max<int64_t>(1, c.Int(Name("num")).value_or(1)));
      s.columns.space = Twips(c.Twips(Name("space")).value_or(720));
      s.columns.equalWidth = c.OnOff(Name("equalWidth")).value_or(true);
      s.columns.separator = c.OnOff(Name("sep")).value_or(false);
      for (Node col : c.Kids())
        if (col.Is(Name("col")))
          s.columns.cols.push_back({Twips(col.Twips(kW).value_or(0)), Twips(col.Twips(Name("space")).value_or(0))});
      if (!s.columns.cols.empty() && !c.Has(Name("equalWidth"))) s.columns.equalWidth = false;
    } else if (name == Name("titlePg")) s.titlePage = c.On();
    else if (name == Name("vAlign")) s.vAlign = c.AttrOr(kVal);
    else if (name == Name("textDirection")) s.textDirection = c.AttrOr(kVal);
    else if (name == Name("bidi")) s.bidi = c.On();
    else if (name == Name("rtlGutter")) s.rtlGutter = c.On();
    else if (name == Name("formProt")) s.formProt = c.On();
    else if (name == Name("noEndnote")) s.noEndnote = c.On();
    else if (name == Name("pgNumType")) {
      s.pageNumberFormat = c.AttrOr(Name("fmt"));
      if (auto v = c.Int(Name("start"))) s.pageNumberStart = static_cast<int32_t>(*v);
    } else if (name == Name("lnNumType")) {
      s.lineNumberCountBy = static_cast<int32_t>(c.Int(Name("countBy")).value_or(0));
      s.lineNumberStart = static_cast<int32_t>(c.Int(Name("start")).value_or(0)) + 1;  // w:start counts from 0
      s.lineNumberDistance = Twips(c.Twips(Name("distance")).value_or(0));
      s.lineNumberRestart = c.AttrOr(Name("restart"));
    } else if (name == Name("docGrid")) {
      s.docGridType = c.AttrOr(kType);
      s.linePitch = Twips(c.Twips(Name("linePitch")).value_or(0));
      s.charSpace = static_cast<int32_t>(c.Int(Name("charSpace")).value_or(0));
    } else if (name == Name("pgBorders")) {
      s.pageBorderOffsetFrom = c.AttrOr(Name("offsetFrom"));
      s.pageBorderDisplay = c.AttrOr(Name("display"));
      s.pageBorderZOrder = c.AttrOr(Name("zOrder"));
      // Page borders have no theme colors that need the document: their color is val or auto.
      auto border = [](Node b) {
        Border r;
        if (!b) return r;
        r.name = b.AttrOr(kVal);
        r.style = ParseBorderStyle(r.name);
        r.width = EighthPoints(static_cast<int32_t>(b.Int(Name("sz")).value_or(0)));
        r.spacePt = static_cast<int32_t>(b.Int(Name("space")).value_or(0));
        auto rgb = b.Hex(Name("color"));
        if (rgb && b.AttrOr(Name("color")).size() == 6) r.color = Color::Rgb(*rgb);
        r.shadow = b.OnOff(Name("shadow")).value_or(false);
        r.frame = b.OnOff(Name("frame")).value_or(false);
        return r;
      };
      s.pageBorderTop = border(c.Child(Name("top")));
      s.pageBorderStart = border(c.Child(Name("left")));
      s.pageBorderBottom = border(c.Child(Name("bottom")));
      s.pageBorderEnd = border(c.Child(Name("right")));
    } else if (name == Name("footnotePr") || name == Name("endnotePr")) {
      NoteProps& n = name == Name("footnotePr") ? s.footnotes : s.endnotes;
      n.pos = c.Child(Name("pos")).AttrOr(kVal);
      n.numFmt = c.Child(Name("numFmt")).AttrOr(kVal);
      n.start = static_cast<int32_t>(c.Child(Name("numStart")).Int(kVal).value_or(1));
      n.restart = c.Child(Name("numRestart")).AttrOr(kVal);
    }
  }
  return s;
}

}  // namespace fw::doc
