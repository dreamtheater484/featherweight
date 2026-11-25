// The .xlsx reader: the workbook part, styles, theme and shared strings (plan §7.3). Sheets are
// read by SheetReader.cpp.
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>

#include "core/Lang.h"
#include "draw/Theme.h"
#include "pkg/Check.h"
#include "pkg/References.h"
#include "pkg/XmlDom.h"
#include "pkg/XmlWriter.h"
#include "sheet/model/TableStyles.h"
#include "sheet/xlsx/Internal.h"
#include "sheet/xlsx/Xlsx.h"

namespace fw::sheet::xlsx {

using pkg::Status;
using pkg::XmlPull;

// ---- Helpers

size_t DeclarationEnd(std::string_view src) {
  if (src.size() < 6 || src.substr(0, 5) != "<?xml") return 0;
  char c = src[5];
  if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return 0;
  size_t end = src.find("?>");
  return end == std::string_view::npos ? 0 : end + 2;
}

size_t TagBegin(std::string_view src, size_t end) {
  size_t p = end ? src.rfind('<', end - 1) : std::string_view::npos;
  return p == std::string_view::npos ? 0 : p;
}

bool SkipElement(XmlPull& pull) {
  if (pull.IsEmptyElement()) return pull.Next() == XmlPull::Event::EndElement;
  int depth = pull.Depth();
  for (;;) {
    XmlPull::Event e = pull.Next();
    if (e == XmlPull::Event::Error || e == XmlPull::Event::EndDocument) return false;
    if (e == XmlPull::Event::EndElement && pull.Depth() == depth) return true;
  }
}

std::string_view Attr(pugi::xml_node n, const char* name) { return n.attribute(name).value(); }

// A numFmt's code. Excel writes one that older versions can't read twice: cut down in formatCode
// ("[$]ggge") and whole in x16r2:formatCode16 ("[$-ja-JP-x-gannen]ggge"), which it reads first.
std::string_view FormatCode(pugi::xml_node n) {
  for (pugi::xml_attribute a = n.first_attribute(); a; a = a.next_attribute()) {
    const std::string_view name = a.name();
    if (name.size() > 13 && name.substr(name.size() - 13) == ":formatCode16") return a.value();
  }
  return Attr(n, "formatCode");
}

bool ParseBool(std::string_view s, bool def) {
  if (s == "1" || s == "true") return true;
  if (s == "0" || s == "false") return false;
  return def;
}

bool ParseUInt(std::string_view s, uint32_t& out) {
  if (s.empty()) return false;
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc() && p == s.data() + s.size();
}

bool ParseDouble(std::string_view s, double& out) {
  if (s.empty()) return false;
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc() && p == s.data() + s.size() && std::isfinite(out);
}

bool BoolAttr(pugi::xml_node n, const char* name, bool def) { return ParseBool(Attr(n, name), def); }

uint32_t UIntAttr(pugi::xml_node n, const char* name, uint32_t def) {
  uint32_t v = 0;
  return ParseUInt(Attr(n, name), v) ? v : def;
}

int IntAttr(pugi::xml_node n, const char* name, int def) {
  std::string_view s = Attr(n, name);
  int v = 0;
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  return !s.empty() && ec == std::errc() && p == s.data() + s.size() ? v : def;
}

double NumAttr(pugi::xml_node n, const char* name, double def) {
  double v = 0;
  return ParseDouble(Attr(n, name), v) ? v : def;
}

std::string_view RelIdAttr(pugi::xml_node n) {
  for (pugi::xml_attribute a : n.attributes()) {
    std::string_view name = a.name();
    size_t colon = name.find(':');
    if (colon != std::string_view::npos && name.substr(colon + 1) == "id" && name.substr(0, colon) != "xmlns")
      return a.value();
  }
  return {};
}

std::string_view Local(pugi::xml_node n) {
  std::string_view name = n.name();
  size_t colon = name.find(':');
  return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

pugi::xml_node Child(pugi::xml_node n, std::string_view local) {
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && Local(c) == local) return c;
  return {};
}

bool ParseSlice(pugi::xml_document& doc, std::string_view xml) {
  return doc.load_buffer(xml.data(), xml.size(), pugi::parse_default | pugi::parse_ws_pcdata, pugi::encoding_utf8);
}

std::string TextOf(pugi::xml_node n) {
  std::string s;
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata) s += c.value();
  return s;
}

std::string EmptyElementXml(pugi::xml_node n) {
  std::string s = "<";
  s += n.name();
  for (pugi::xml_attribute a : n.attributes()) {
    s += ' ';
    s += a.name();
    s += "=\"";
    pkg::AppendEscapedAttribute(s, a.value());
    s += '"';
  }
  return s + "/>";
}

Color ReadColor(pugi::xml_node n) {
  Color c;
  if (!n) return c;
  uint32_t v = 0;
  std::string_view rgb = Attr(n, "rgb");
  if (BoolAttr(n, "auto", false)) {
    c.kind = Color::Kind::Auto;
  } else if (!rgb.empty()) {
    auto [p, ec] = std::from_chars(rgb.data(), rgb.data() + rgb.size(), v, 16);
    if (ec == std::errc() && p == rgb.data() + rgb.size()) {
      c.kind = Color::Kind::Rgb;
      c.value = rgb.size() <= 6 ? (0xFF000000u | v) : v;
    }
  } else if (ParseUInt(Attr(n, "theme"), v)) {
    c.kind = Color::Kind::Theme;
    c.value = v;
  } else if (ParseUInt(Attr(n, "indexed"), v)) {
    c.kind = Color::Kind::Indexed;
    c.value = v;
  }
  c.tint = std::clamp(NumAttr(n, "tint", 0), -1.0, 1.0);
  return c;
}

Font ReadFont(pugi::xml_node n) {
  Font f;
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    std::string_view k = Local(c);
    bool on = BoolAttr(c, "val", true);
    if (k == "b") f.bold = on;
    else if (k == "i") f.italic = on;
    else if (k == "strike") f.strike = on;
    else if (k == "outline") f.outline = on;
    else if (k == "shadow") f.shadow = on;
    else if (k == "condense") f.condense = on;
    else if (k == "extend") f.extend = on;
    else if (k == "sz") f.size = std::clamp(NumAttr(c, "val", 0), 0.0, 409.0);
    else if (k == "name" || k == "rFont") f.name = Attr(c, "val");
    else if (k == "family") f.family = IntAttr(c, "val", 0);
    else if (k == "charset") f.charset = IntAttr(c, "val", -1);
    else if (k == "color") f.color = ReadColor(c);
    else if (k == "u") {
      std::string_view u = Attr(c, "val");
      f.underline = u.empty() || u == "single"   ? Underline::Single
                    : u == "double"              ? Underline::Double
                    : u == "singleAccounting"    ? Underline::SingleAccounting
                    : u == "doubleAccounting"    ? Underline::DoubleAccounting
                                                 : Underline::None;
    } else if (k == "vertAlign") {
      std::string_view v = Attr(c, "val");
      f.vertAlign = v == "superscript" ? VertAlign::Superscript : v == "subscript" ? VertAlign::Subscript : VertAlign::Baseline;
    } else if (k == "scheme") {
      std::string_view v = Attr(c, "val");
      f.scheme = v == "major" ? FontScheme::Major : v == "minor" ? FontScheme::Minor : FontScheme::None;
    }
  }
  return f;
}

namespace {

PatternType ParsePattern(std::string_view s) {
  static constexpr std::string_view names[] = {
      "none",          "solid",           "mediumGray",    "darkGray",     "lightGray",    "darkHorizontal", "darkVertical",
      "darkDown",      "darkUp",          "darkGrid",      "darkTrellis",  "lightHorizontal", "lightVertical", "lightDown",
      "lightUp",       "lightGrid",       "lightTrellis",  "gray125",      "gray0625"};
  for (size_t i = 0; i < std::size(names); ++i)
    if (names[i] == s) return static_cast<PatternType>(i);
  return PatternType::None;
}

BorderStyle ParseBorderStyle(std::string_view s) {
  static constexpr std::string_view names[] = {
      "none",   "thin",         "medium",     "dashed",        "dotted",     "thick",           "double",
      "hair",   "mediumDashed", "dashDot",    "mediumDashDot", "dashDotDot", "mediumDashDotDot", "slantDashDot"};
  for (size_t i = 0; i < std::size(names); ++i)
    if (names[i] == s) return static_cast<BorderStyle>(i);
  return BorderStyle::None;
}

}  // namespace

Fill ReadFill(pugi::xml_node n, bool differential) {
  Fill f;
  if (pugi::xml_node p = Child(n, "patternFill")) {
    std::string_view type = Attr(p, "patternType");
    f.fg = ReadColor(Child(p, "fgColor"));
    f.bg = ReadColor(Child(p, "bgColor"));
    // In a differential format, a pattern fill without a type is solid in its background color.
    if (type.empty()) f.pattern = differential && (f.fg.IsSet() || f.bg.IsSet()) ? PatternType::Solid : PatternType::None;
    else f.pattern = ParsePattern(type);
  } else if (pugi::xml_node g = Child(n, "gradientFill")) {
    f.gradient = true;
    f.path = Attr(g, "type") == "path";
    f.degree = NumAttr(g, "degree", 0);
    f.left = NumAttr(g, "left", 0);
    f.right = NumAttr(g, "right", 0);
    f.top = NumAttr(g, "top", 0);
    f.bottom = NumAttr(g, "bottom", 0);
    for (pugi::xml_node s = g.first_child(); s; s = s.next_sibling())
      if (s.type() == pugi::node_element && Local(s) == "stop" && f.stops.size() < 256)
        f.stops.push_back({std::clamp(NumAttr(s, "position", 0), 0.0, 1.0), ReadColor(Child(s, "color"))});
  }
  return f;
}

Border ReadBorder(pugi::xml_node n) {
  Border b;
  b.diagonalUp = BoolAttr(n, "diagonalUp", false);
  b.diagonalDown = BoolAttr(n, "diagonalDown", false);
  b.outline = BoolAttr(n, "outline", true);
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    std::string_view k = Local(c);
    BorderSide side{ParseBorderStyle(Attr(c, "style")), ReadColor(Child(c, "color"))};
    if (k == "left" || k == "start") b.left = side;
    else if (k == "right" || k == "end") b.right = side;
    else if (k == "top") b.top = side;
    else if (k == "bottom") b.bottom = side;
    else if (k == "diagonal") b.diagonal = side;
    else if (k == "vertical") b.vertical = side;
    else if (k == "horizontal") b.horizontal = side;
  }
  return b;
}

Alignment ReadAlignment(pugi::xml_node n) {
  Alignment a;
  std::string_view h = Attr(n, "horizontal"), v = Attr(n, "vertical");
  static constexpr std::string_view hs[] = {"general", "left", "center", "right", "fill", "justify", "centerContinuous", "distributed"};
  for (size_t i = 0; i < std::size(hs); ++i)
    if (hs[i] == h) a.horizontal = static_cast<HAlign>(i);
  static constexpr std::string_view vs[] = {"bottom", "top", "center", "justify", "distributed"};
  for (size_t i = 0; i < std::size(vs); ++i)
    if (vs[i] == v) a.vertical = static_cast<VAlign>(i);
  uint32_t rot = UIntAttr(n, "textRotation", 0);
  a.rotation = static_cast<uint16_t>(rot <= 180 || rot == 255 ? rot : 0);
  a.wrap = BoolAttr(n, "wrapText", false);
  a.shrinkToFit = BoolAttr(n, "shrinkToFit", false);
  a.justifyLastLine = BoolAttr(n, "justifyLastLine", false);
  a.indent = static_cast<uint16_t>(std::min<uint32_t>(UIntAttr(n, "indent", 0), 250));
  a.relativeIndent = IntAttr(n, "relativeIndent", 0);
  a.readingOrder = static_cast<uint8_t>(std::min<uint32_t>(UIntAttr(n, "readingOrder", 0), 2));
  return a;
}

// ---- Styles

namespace {

// A cell format (cellXfs), or a cell style's (cellStyleXfs, `style`): there a missing apply flag
// means the style includes that part (Excel writes applyFont="0" for one it leaves out).
Xf ReadXf(pugi::xml_node n, bool style = false) {
  Xf x;
  x.numFmt = UIntAttr(n, "numFmtId", 0);
  x.font = UIntAttr(n, "fontId", 0);
  x.fill = UIntAttr(n, "fillId", 0);
  x.border = UIntAttr(n, "borderId", 0);
  x.parent = UIntAttr(n, "xfId", 0);
  x.applyNumFmt = BoolAttr(n, "applyNumberFormat", style);
  x.applyFont = BoolAttr(n, "applyFont", style);
  x.applyFill = BoolAttr(n, "applyFill", style);
  x.applyBorder = BoolAttr(n, "applyBorder", style);
  x.applyAlignment = BoolAttr(n, "applyAlignment", style);
  x.applyProtection = BoolAttr(n, "applyProtection", style);
  x.quotePrefix = BoolAttr(n, "quotePrefix", false);
  x.pivotButton = BoolAttr(n, "pivotButton", false);
  if (pugi::xml_node a = Child(n, "alignment")) x.alignment = ReadAlignment(a);
  if (pugi::xml_node p = Child(n, "protection")) {
    x.protection.locked = BoolAttr(p, "locked", true);
    x.protection.hidden = BoolAttr(p, "hidden", false);
  }
  return x;
}

template <typename F>
void ForEachChild(pugi::xml_node n, std::string_view local, F&& fn) {
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && Local(c) == local) fn(c);
}

// The workbook's own table styles, from their dxfs. Excel writes table="0" for a style it doesn't
// offer in its gallery; tables still use it, so every style is read.
void ReadTableStyles(pugi::xml_node sec, StylesBuilder& b) {
  static constexpr std::string_view kTypes[] = {
      "wholeTable", "firstColumnStripe", "secondColumnStripe", "firstRowStripe", "secondRowStripe", "lastColumn",
      "firstColumn", "headerRow", "totalRow", "firstHeaderCell", "lastHeaderCell", "firstTotalCell", "lastTotalCell"};
  constexpr size_t kMaxStyles = 1024;
  b.DefaultTableStyle() = Attr(sec, "defaultTableStyle");
  ForEachChild(sec, "tableStyle", [&](pugi::xml_node n) {
    if (b.TableStyles().size() >= kMaxStyles) return;
    TableStyle s;
    s.name = Attr(n, "name");
    ForEachChild(n, "tableStyleElement", [&](pugi::xml_node e) {
      const std::string_view type = Attr(e, "type");
      const auto it = std::find(std::begin(kTypes), std::end(kTypes), type);
      if (it == std::end(kTypes)) return;  // the pivot table elements
      const uint32_t dxf = UIntAttr(e, "dxfId", UINT32_MAX);
      s.elements[it - std::begin(kTypes)] =
          TableElementFromDxf(dxf < b.Dxfs().size() ? b.Dxfs()[dxf] : Dxf{}, UIntAttr(e, "size", 1));
    });
    b.TableStyles().push_back(std::move(s));
  });
}

}  // namespace

Status ReadStyles(std::string_view xml, const pkg::Limits& limits, std::string_view label, Styles& styles) {
  pkg::XmlDom dom;
  Status st = dom.Parse(xml, limits, label);
  if (!st.Ok()) return st;
  StylesBuilder b(styles);
  pugi::xml_node root = dom.Root();
  pugi::xml_node tableStyles;  // read after the dxfs it refers to
  for (pugi::xml_node sec = root.first_child(); sec; sec = sec.next_sibling()) {
    if (sec.type() != pugi::node_element) continue;
    std::string_view k = Local(sec);
    if (k == "numFmts") {
      ForEachChild(sec, "numFmt", [&](pugi::xml_node n) {
        uint32_t id = 0;
        if (ParseUInt(Attr(n, "numFmtId"), id)) b.NumFmts()[id] = FormatCode(n);
      });
    } else if (k == "fonts") {
      ForEachChild(sec, "font", [&](pugi::xml_node n) { b.Fonts().push_back(ReadFont(n)); });
    } else if (k == "fills") {
      ForEachChild(sec, "fill", [&](pugi::xml_node n) { b.Fills().push_back(ReadFill(n, false)); });
    } else if (k == "borders") {
      ForEachChild(sec, "border", [&](pugi::xml_node n) { b.Borders().push_back(ReadBorder(n)); });
    } else if (k == "cellStyleXfs") {
      ForEachChild(sec, "xf", [&](pugi::xml_node n) { b.CellStyleXfs().push_back(ReadXf(n, true)); });
    } else if (k == "cellXfs") {
      ForEachChild(sec, "xf", [&](pugi::xml_node n) { b.CellXfs().push_back(ReadXf(n)); });
    } else if (k == "cellStyles") {
      ForEachChild(sec, "cellStyle", [&](pugi::xml_node n) {
        CellStyle s;
        s.name = Attr(n, "name");
        s.xf = UIntAttr(n, "xfId", 0);
        s.builtinId = IntAttr(n, "builtinId", -1);
        s.outlineLevel = IntAttr(n, "iLevel", -1);
        s.hidden = BoolAttr(n, "hidden", false);
        s.customBuiltin = BoolAttr(n, "customBuiltin", false);
        b.CellStyles().push_back(std::move(s));
      });
    } else if (k == "dxfs") {
      ForEachChild(sec, "dxf", [&](pugi::xml_node n) {
        Dxf d;
        if (pugi::xml_node f = Child(n, "font")) {
          d.font = ReadFont(f);
          d.fontBold = static_cast<bool>(Child(f, "b"));
          d.fontItalic = static_cast<bool>(Child(f, "i"));
          d.fontStrike = static_cast<bool>(Child(f, "strike"));
          d.fontUnderline = static_cast<bool>(Child(f, "u"));
        }
        if (pugi::xml_node f = Child(n, "numFmt")) {
          d.numFmtId = UIntAttr(f, "numFmtId", 0);
          d.numFmtCode = FormatCode(f);
        }
        if (pugi::xml_node f = Child(n, "fill")) d.fill = ReadFill(f, true);
        if (pugi::xml_node f = Child(n, "border")) d.border = ReadBorder(f);
        if (pugi::xml_node f = Child(n, "alignment")) d.alignment = ReadAlignment(f);
        if (pugi::xml_node f = Child(n, "protection"))
          d.protection = Protection{BoolAttr(f, "locked", true), BoolAttr(f, "hidden", false)};
        b.Dxfs().push_back(std::move(d));
      });
    } else if (k == "tableStyles") {
      tableStyles = sec;
    } else if (k == "colors") {
      if (pugi::xml_node ix = Child(sec, "indexedColors")) {
        ForEachChild(ix, "rgbColor", [&](pugi::xml_node n) {
          Color c = ReadColor(n);
          b.Palette().push_back(c.kind == Color::Kind::Rgb ? (c.value | 0xFF000000u) : 0xFF000000u);
        });
      }
    }
  }
  if (tableStyles) ReadTableStyles(tableStyles, b);
  return {};
}

// ---- Shared strings

namespace {

bool AllSpace(std::string_view s) {
  return std::all_of(s.begin(), s.end(), [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
}

// A rich or otherwise unusual string: its text and runs, from the si element as written.
void ReadRichString(std::string_view raw, StringTable& table, std::string& text, std::vector<RichRun>& runs) {
  text.clear();
  runs.clear();
  pugi::xml_document doc;
  if (!ParseSlice(doc, raw)) return;
  pugi::xml_node si = doc.document_element();
  for (pugi::xml_node c = si.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    std::string_view k = Local(c);
    if (k == "t") {
      if (!runs.empty() && runs.back().font != kNone) runs.push_back({static_cast<uint32_t>(text.size()), kNone});
      text += DecodeXstring(TextOf(c));  // Excel's _x000D_ for a CR
    } else if (k == "r") {
      pugi::xml_node rPr = Child(c, "rPr");
      uint32_t font = rPr ? table.AddFont(ReadFont(rPr)) : kNone;
      if (runs.empty() ? (font != kNone || !text.empty()) : runs.back().font != font) {
        if (runs.empty() && !text.empty()) runs.push_back({0, kNone});
        runs.push_back({static_cast<uint32_t>(text.size()), font});
      }
      for (pugi::xml_node t = c.first_child(); t; t = t.next_sibling())
        if (t.type() == pugi::node_element && Local(t) == "t") text += DecodeXstring(TextOf(t));
    }
  }
  // A single run in the cell's font is plain text.
  if (runs.size() == 1 && runs[0].font == kNone) runs.clear();
}

}  // namespace

Status ReadStrings(std::string_view xml, const pkg::Limits& limits, std::string_view label, StringTable& table) {
  XmlPull pull(xml, limits, label);
  std::string_view src = pull.Source();
  auto& part = table.Part();
  size_t declEnd = DeclarationEnd(src);
  XmlPull::Event e;
  do {
    e = pull.Next();
  } while (e == XmlPull::Event::Comment || e == XmlPull::Event::ProcessingInstruction || e == XmlPull::Event::Text);
  if (e != XmlPull::Event::StartElement) return pull.GetStatus().Ok() ? Status::Fail(Status::Code::Damaged, T("The shared strings are damaged.")) : pull.GetStatus();
  size_t rootBegin = TagBegin(src, pull.Offset());
  part.prolog = std::string(src.substr(declEnd, rootBegin > declEnd ? rootBegin - declEnd : 0));
  part.rootStart = std::string(src.substr(rootBegin, pull.Offset() - rootBegin));
  bool rootEmpty = pull.IsEmptyElement();
  std::string text, pendingText;
  std::vector<RichRun> runs;
  bool hasMarkup = false, pendingSpaceOnly = true;
  auto flushText = [&](bool atEnd) {
    if (pendingText.empty()) return;
    if (!pendingSpaceOnly || (atEnd && !hasMarkup)) part.kept.emplace_back(table.Size(), pendingText);
    pendingText.clear();
    pendingSpaceOnly = true;
  };
  for (;;) {
    size_t before = pull.Offset();
    e = pull.Next();
    if (e == XmlPull::Event::Error) return pull.GetStatus();
    if (e == XmlPull::Event::EndDocument) return Status::Fail(Status::Code::Damaged, T("The shared strings are damaged."));
    if (e == XmlPull::Event::EndElement && pull.Depth() == 1) {
      flushText(true);
      size_t endBegin = rootEmpty ? pull.Offset() : TagBegin(src, pull.Offset());
      part.rootEnd = std::string(src.substr(endBegin, pull.Offset() - endBegin));
      break;
    }
    if (e == XmlPull::Event::Text || e == XmlPull::Event::CData) {
      pendingText += src.substr(before, pull.Offset() - before);
      pendingSpaceOnly = pendingSpaceOnly && AllSpace(pull.Text());
      continue;
    }
    flushText(false);
    hasMarkup = true;
    if (e != XmlPull::Event::StartElement) {  // comments, processing instructions
      part.kept.emplace_back(table.Size(), std::string(src.substr(before, pull.Offset() - before)));
      continue;
    }
    if (pull.LocalName() != "si" || !IsMainNs(pull.NamespaceUri())) {
      if (!SkipElement(pull)) return pull.GetStatus();
      part.kept.emplace_back(table.Size(), std::string(src.substr(before, pull.Offset() - before)));
      continue;
    }
    // <si><t>text</t></si>, perhaps with xml:space="preserve", is the usual form; anything else
    // is kept as written, and its text and runs read from that.
    bool plain = pull.Attributes().empty() && !pull.IsEmptyElement();
    bool preserve = false, seenT = false, siMarkup = false, siSpaceOnlyText = false;
    text.clear();
    if (pull.IsEmptyElement()) {
      pull.Next();
    } else {
      for (;;) {
        XmlPull::Event x = pull.Next();
        if (x == XmlPull::Event::Error || x == XmlPull::Event::EndDocument) return pull.GetStatus();
        if (x == XmlPull::Event::EndElement && pull.Depth() == 2) break;
        if (x == XmlPull::Event::Text || x == XmlPull::Event::CData) {
          if (AllSpace(pull.Text())) siSpaceOnlyText = true;
          else plain = false;
          continue;
        }
        siMarkup = true;
        if (x != XmlPull::Event::StartElement || seenT || pull.LocalName() != "t" || !IsMainNs(pull.NamespaceUri())) {
          plain = false;
          if (x == XmlPull::Event::StartElement && !SkipElement(pull)) return pull.GetStatus();
          continue;
        }
        seenT = true;
        for (const auto& a : pull.Attributes()) {
          if (a.nsUri == pkg::kXmlNamespace && a.local == "space" && a.value == "preserve") preserve = true;
          else plain = false;
        }
        if (pull.IsEmptyElement()) {
          pull.Next();
          continue;
        }
        for (;;) {
          XmlPull::Event y = pull.Next();
          if (y == XmlPull::Event::Error || y == XmlPull::Event::EndDocument) return pull.GetStatus();
          if (y == XmlPull::Event::EndElement && pull.Depth() == 3) break;
          if (y == XmlPull::Event::Text || y == XmlPull::Event::CData) {
            text += pull.Text();
          } else {
            plain = false;
            if (y == XmlPull::Event::StartElement && !SkipElement(pull)) return pull.GetStatus();
          }
        }
      }
    }
    if (!seenT || (siSpaceOnlyText && !siMarkup)) plain = false;
    std::string_view raw = src.substr(before, pull.Offset() - before);
    if (!table.HasRoom(raw.size() * 2)) return Status::Fail(Status::Code::TooLarge, T("The shared strings are too large."));
    if (plain) {
      DecodeXstringIn(text);  // Excel's _x000D_ for a CR, _x005F_x0041_ for the text _x0041_
      table.AddRead(text, preserve ? StringTable::kPreserve : 0, {}, {});
    } else {
      ReadRichString(raw, table, text, runs);
      table.AddRead(text, StringTable::kRaw, raw, runs);
    }
  }
  // After the root: comments, processing instructions.
  size_t epilogStart = pull.Offset();
  for (;;) {
    e = pull.Next();
    if (e == XmlPull::Event::Error) return pull.GetStatus();
    if (e == XmlPull::Event::EndDocument) break;
  }
  part.epilog = std::string(src.substr(epilogStart));
  table.Shrink();
  return {};
}

// ---- ST_Xstring

std::string DecodeXstring(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned v = 0;
    if (s[i] == '_' && i + 6 < s.size() && s[i + 1] == 'x' && s[i + 6] == '_' &&
        std::from_chars(s.data() + i + 2, s.data() + i + 6, v, 16).ptr == s.data() + i + 6 && !(v >= 0xD800 && v < 0xE000)) {
      if (v < 0x80) {
        out += static_cast<char>(v);
      } else if (v < 0x800) {
        out += static_cast<char>(0xC0 | (v >> 6)), out += static_cast<char>(0x80 | (v & 0x3F));
      } else {
        out += static_cast<char>(0xE0 | (v >> 12)), out += static_cast<char>(0x80 | ((v >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (v & 0x3F));
      }
      i += 6;
    } else {
      out += s[i];
    }
  }
  return out;
}

std::string EncodeXstring(std::string_view s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  auto escape = [&](std::string& out, unsigned v) {
    out += "_x";
    for (int shift = 12; shift >= 0; shift -= 4) out += kHex[(v >> shift) & 15];
    out += '_';
  };
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '_' && i + 6 < s.size() && s[i + 1] == 'x' && s[i + 6] == '_' &&
        std::all_of(s.begin() + i + 2, s.begin() + i + 6, [](char h) { return std::isxdigit(static_cast<unsigned char>(h)); })) {
      escape(out, '_');  // "_x0041_" as text: its "_" escaped, so it doesn't read back as "A"
    } else if (c < 0x20 && c != '\t' && c != '\n') {
      escape(out, c);  // XML can't hold it, or reads a CR back as a line feed
    } else if (c == 0xEF && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xBF &&
               (static_cast<unsigned char>(s[i + 2]) & 0xFE) == 0xBE) {
      escape(out, 0xFFFE + (static_cast<unsigned char>(s[i + 2]) - 0xBE));  // U+FFFE, U+FFFF: not XML characters
      i += 2;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

void AppendXstringText(std::string& out, std::string_view s) {
  const bool plain = s.find("_x") == std::string_view::npos && s.find("\xEF\xBF") == std::string_view::npos &&
                     std::none_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n'; });
  if (plain) pkg::AppendEscapedText(out, s);
  else pkg::AppendEscapedText(out, EncodeXstring(s));
}

// ---- Tables

Status ReadTable(std::string_view xml, const pkg::Limits& limits, const std::string& part, Table& t) {
  pkg::XmlDom dom;
  Status st = dom.Parse(xml, limits, part.substr(1));
  if (!st.Ok()) return st;
  pugi::xml_node n = dom.Root();
  if (Local(n) != "table") return {};
  t.part = part;
  t.id = UIntAttr(n, "id", 0);
  t.name = Attr(n, "name");
  t.displayName = Attr(n, "displayName");
  if (t.displayName.empty()) t.displayName = t.name;
  if (!ParseRange(Attr(n, "ref"), t.ref)) return {};
  t.headerRows = std::min(UIntAttr(n, "headerRowCount", 1), 1u);
  t.totalsRows = std::min(UIntAttr(n, "totalsRowCount", 0), 1u);
  t.totalsRowShown = BoolAttr(n, "totalsRowShown", true);
  t.headerDxf = IntAttr(n, "headerRowDxfId", -1);
  t.dataDxf = IntAttr(n, "dataDxfId", -1);
  t.totalsDxf = IntAttr(n, "totalsRowDxfId", -1);
  pugi::xml_node filter = Child(n, "autoFilter");
  t.autoFilter = static_cast<bool>(filter);
  t.filtered = static_cast<bool>(Child(filter, "filterColumn"));
  if (SheetFilter f; filter && ReadAutoFilter(filter, f)) t.criteria = std::move(f.columns);
  if (pugi::xml_node cols = Child(n, "tableColumns"))
    for (pugi::xml_node c = cols.first_child(); c; c = c.next_sibling()) {
      if (c.type() != pugi::node_element || Local(c) != "tableColumn") continue;
      TableColumn col;
      col.id = UIntAttr(c, "id", 0);
      col.name = DecodeXstring(Attr(c, "name"));
      col.totalsFunction = Attr(c, "totalsRowFunction");
      if (col.totalsFunction == "none") col.totalsFunction.clear();
      col.totalsLabel = Attr(c, "totalsRowLabel");
      col.calculatedFormula = TextOf(Child(c, "calculatedColumnFormula"));
      col.totalsFormula = TextOf(Child(c, "totalsRowFormula"));
      col.dataDxf = IntAttr(c, "dataDxfId", -1);
      col.headerDxf = IntAttr(c, "headerRowDxfId", -1);
      col.totalsDxf = IntAttr(c, "totalsRowDxfId", -1);
      t.columns.push_back(std::move(col));
      if (t.columns.size() >= kMaxCols) break;
    }
  if (pugi::xml_node s = Child(n, "tableStyleInfo")) {
    t.style.name = Attr(s, "name");
    t.style.firstColumn = BoolAttr(s, "showFirstColumn", false);
    t.style.lastColumn = BoolAttr(s, "showLastColumn", false);
    t.style.rowStripes = BoolAttr(s, "showRowStripes", false);
    t.style.columnStripes = BoolAttr(s, "showColumnStripes", false);
  }
  return {};
}

// ---- The workbook

namespace {

bool TypeEndsWith(std::string_view type, std::string_view name) {
  return type.size() > name.size() && type.ends_with(name) && type[type.size() - name.size() - 1] == '/';
}

// xl/metadata.xml. What each value metadata block (a cell's vm) stands for when it's an error:
// valueMetadata's <rc t v> picks an XLRICHVALUE futureMetadata block, whose xlrd:rvb names a rich
// value; a rich value of an "_error" structure holds the error's type (MS-XLSX 2.3.6.1.4). And
// which cell metadata blocks (a cell's cm) make a dynamic array: cellMetadata's <rc t v> picks an
// XLDAPR block with xda:dynamicArrayProperties fDynamic="1".
void ReadMetadata(const pkg::Package& package, const std::string& metadata, const std::string& values,
                  const std::string& structures, std::vector<uint8_t>& out, std::vector<uint8_t>& dynamic) {
  // errorType -> ErrorCode
  static constexpr ErrorCode kTypes[] = {ErrorCode::Null,    ErrorCode::Div0,    ErrorCode::Value,   ErrorCode::Ref,
                                         ErrorCode::Name,    ErrorCode::Num,     ErrorCode::NA,      ErrorCode::GettingData,
                                         ErrorCode::Spill,   ErrorCode::Connect, ErrorCode::Blocked, ErrorCode::Unknown,
                                         ErrorCode::Field,   ErrorCode::Calc};
  constexpr size_t kMaxBlocks = 1u << 20;
  std::vector<uint8_t> bytes;
  auto parse = [&](const std::string& part, pkg::XmlDom& dom) {
    if (part.empty() || !package.ReadPart(part, bytes).Ok()) return false;
    return dom.Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), package.GetLimits(),
                     part).Ok();
  };
  pkg::XmlDom meta, rich, shapes;
  if (!parse(metadata, meta)) return;
  const bool hasRich = parse(values, rich) && parse(structures, shapes);  // only errors need them
  // The structures: for an "_error" one, which of its values is the error type.
  std::vector<int> errorKey;
  for (pugi::xml_node s = hasRich ? shapes.Root().first_child() : pugi::xml_node(); s && errorKey.size() < kMaxBlocks;
       s = s.next_sibling()) {
    if (Local(s) != "s") continue;
    int key = -1, k = 0;
    if (Attr(s, "t") == "_error")
      for (pugi::xml_node c = s.first_child(); c; c = c.next_sibling())
        if (Local(c) == "k") {
          if (Attr(c, "n") == "errorType") key = k;
          ++k;
        }
    errorKey.push_back(key);
  }
  // The rich values: each one's error, if it is one.
  std::vector<uint8_t> richError;
  for (pugi::xml_node rv = hasRich ? rich.Root().first_child() : pugi::xml_node(); rv && richError.size() < kMaxBlocks;
       rv = rv.next_sibling()) {
    if (Local(rv) != "rv") continue;
    uint8_t e = 0xFF;
    const uint32_t s = UIntAttr(rv, "s", UINT32_MAX);
    if (s < errorKey.size() && errorKey[s] >= 0) {
      int k = 0;
      for (pugi::xml_node v = rv.first_child(); v; v = v.next_sibling())
        if (Local(v) == "v" && k++ == errorKey[s]) {
          uint32_t type = 0;
          if (ParseUInt(TextOf(v), type) && type < std::size(kTypes)) e = static_cast<uint8_t>(kTypes[type]);
          break;
        }
    }
    richError.push_back(e);
  }
  // The metadata: types by index (from 1), the rich value blocks, then the value blocks.
  std::vector<std::string_view> types;
  std::vector<uint32_t> richBlocks;
  std::vector<uint8_t> arrayBlocks;  // XLDAPR blocks: 1 for fDynamic
  pugi::xml_node valueMetadata, cellMetadata;
  for (pugi::xml_node sec = meta.Root().first_child(); sec; sec = sec.next_sibling()) {
    const std::string_view k = Local(sec);
    if (k == "metadataTypes") {
      for (pugi::xml_node t = sec.first_child(); t && types.size() < kMaxBlocks; t = t.next_sibling())
        if (Local(t) == "metadataType") types.push_back(Attr(t, "name"));
    } else if (k == "futureMetadata" && Attr(sec, "name") == "XLRICHVALUE") {
      for (pugi::xml_node bk = sec.first_child(); bk && richBlocks.size() < kMaxBlocks; bk = bk.next_sibling()) {
        if (Local(bk) != "bk") continue;
        uint32_t index = UINT32_MAX;
        for (pugi::xml_node ext = Child(Child(bk, "extLst"), "ext"); ext; ext = ext.next_sibling())
          if (pugi::xml_node rvb = Child(ext, "rvb")) index = UIntAttr(rvb, "i", UINT32_MAX);
        richBlocks.push_back(index);
      }
    } else if (k == "futureMetadata" && Attr(sec, "name") == "XLDAPR") {
      for (pugi::xml_node bk = sec.first_child(); bk && arrayBlocks.size() < kMaxBlocks; bk = bk.next_sibling()) {
        if (Local(bk) != "bk") continue;
        bool on = false;
        for (pugi::xml_node ext = Child(Child(bk, "extLst"), "ext"); ext; ext = ext.next_sibling())
          if (pugi::xml_node p = Child(ext, "dynamicArrayProperties")) on = BoolAttr(p, "fDynamic", false);
        arrayBlocks.push_back(on ? 1 : 0);
      }
    } else if (k == "valueMetadata") {
      valueMetadata = sec;
    } else if (k == "cellMetadata") {
      cellMetadata = sec;
    }
  }
  for (pugi::xml_node bk = cellMetadata.first_child(); bk && dynamic.size() < kMaxBlocks; bk = bk.next_sibling()) {
    if (Local(bk) != "bk") continue;
    uint8_t on = 0;
    for (pugi::xml_node rc = bk.first_child(); rc; rc = rc.next_sibling()) {
      if (Local(rc) != "rc") continue;
      const uint32_t t = UIntAttr(rc, "t", 0), v = UIntAttr(rc, "v", UINT32_MAX);
      if (t >= 1 && t <= types.size() && types[t - 1] == "XLDAPR" && v < arrayBlocks.size()) on |= arrayBlocks[v];
    }
    dynamic.push_back(on);
  }
  for (pugi::xml_node bk = valueMetadata.first_child(); bk && out.size() < kMaxBlocks; bk = bk.next_sibling()) {
    if (Local(bk) != "bk") continue;
    uint8_t e = 0xFF;
    for (pugi::xml_node rc = bk.first_child(); rc; rc = rc.next_sibling()) {
      if (Local(rc) != "rc") continue;
      const uint32_t t = UIntAttr(rc, "t", 0), v = UIntAttr(rc, "v", UINT32_MAX);
      if (t >= 1 && t <= types.size() && types[t - 1] == "XLRICHVALUE" && v < richBlocks.size() &&
          richBlocks[v] < richError.size())
        e = richError[richBlocks[v]];
    }
    out.push_back(e);
  }
}

std::shared_ptr<const draw::Theme> ReadThemePart(const pkg::Package& pkg, const std::string& part) {
  std::vector<uint8_t> bytes;
  if (part.empty() || !pkg.ReadPart(part, bytes).Ok()) return nullptr;
  std::string_view xml(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (!XmlPull::Check(xml, pkg.GetLimits(), part.substr(1)).Ok()) return nullptr;
  return draw::Theme::Parse(xml);
}

}  // namespace

std::unique_ptr<Workbook> Read(std::unique_ptr<pkg::Package> package, Status& status, const ReadOptions& options) {
  status = {};
  if (!package) {
    status = Status::Fail(Status::Code::NotFound, T("There is no workbook to read."));
    return nullptr;
  }
  // The main part.
  std::string main;
  if (const auto* rels = package->RelationshipsOf("/"))
    for (const auto& r : rels->items)
      if (!r.external && TypeEndsWith(r.type, "officeDocument") && main.empty()) main = r.targetPart;
  std::string type(package->ContentTypeOf(main));
  FileFormat format = FileFormat::Xlsx;
  if (type.find("spreadsheetml.sheet.main") != std::string::npos) format = FileFormat::Xlsx;
  else if (type.find("spreadsheetml.template.main") != std::string::npos) format = FileFormat::Xltx;
  else if (type.find("ms-excel.sheet.macroEnabled.main") != std::string::npos) format = FileFormat::Xlsm;
  else if (type.find("ms-excel.template.macroEnabled.main") != std::string::npos) format = FileFormat::Xltm;
  else {
    status = Status::Fail(Status::Code::Unsupported, T("This file isn't an Excel workbook."));
    return nullptr;
  }
  std::vector<uint8_t> bytes;
  if (!(status = package->ReadPart(main, bytes)).Ok()) return nullptr;
  pkg::XmlDom dom;
  std::string_view xml(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (!(status = dom.Parse(xml, package->GetLimits(), main.substr(1))).Ok()) return nullptr;
  pugi::xml_node root = dom.Root();
  if (Local(root) != "workbook" || !IsMainNs(pkg::NamespaceOf(root))) {
    status = Status::Fail(Status::Code::Damaged, T("The workbook part is damaged: it isn't a SpreadsheetML workbook."));
    return nullptr;
  }

  auto book = std::make_unique<Workbook>();
  book->SetFormat(format);
  WorkbookLoader loader(*book);
  const pkg::RelationshipSet* rels = package->RelationshipsOf(main);
  auto relOf = [&](std::string_view id) -> const pkg::Relationship* { return rels ? rels->Find(id) : nullptr; };
  std::string styles, strings, theme, metadata, richValues, richStructures;
  if (rels)
    for (const auto& r : rels->items) {
      if (r.external) continue;
      if (TypeEndsWith(r.type, "sheetMetadata") && metadata.empty()) metadata = r.targetPart;
      else if (TypeEndsWith(r.type, "rdRichValue") && richValues.empty()) richValues = r.targetPart;
      else if (TypeEndsWith(r.type, "rdRichValueStructure") && richStructures.empty()) richStructures = r.targetPart;
      else if (TypeEndsWith(r.type, "styles") && styles.empty()) styles = r.targetPart;
      else if (TypeEndsWith(r.type, "sharedStrings") && strings.empty()) strings = r.targetPart;
      else if (TypeEndsWith(r.type, "theme") && theme.empty()) theme = r.targetPart;
    }
  if (!package->FindPart(styles)) styles.clear();
  if (!package->FindPart(strings)) strings.clear();
  if (!package->FindPart(theme)) theme.clear();

  uint32_t activeTab = 0, firstTab = 0;
  for (pugi::xml_node sec = root.first_child(); sec; sec = sec.next_sibling()) {
    if (sec.type() != pugi::node_element) continue;
    std::string_view k = Local(sec);
    if (k == "workbookPr") {
      book->SetDate1904(BoolAttr(sec, "date1904", false));
    } else if (k == "workbookProtection") {
      loader.ProtectionXml() = EmptyElementXml(sec);
    } else if (k == "bookViews") {
      if (pugi::xml_node v = Child(sec, "workbookView")) {
        activeTab = UIntAttr(v, "activeTab", 0);
        firstTab = UIntAttr(v, "firstSheet", 0);
      }
    } else if (k == "sheets") {
      for (pugi::xml_node s = sec.first_child(); s; s = s.next_sibling()) {
        if (s.type() != pugi::node_element || Local(s) != "sheet") continue;
        Sheet& sheet = book->AddSheet(std::string(Attr(s, "name")));
        std::string_view state = Attr(s, "state");
        std::string relId;
        for (pugi::xml_attribute a : s.attributes())
          if (pkg::LocalNameOf(a) == "id" && (pkg::NamespaceOf(s, a) == pkg::kOfficeRelationshipsNs ||
                                              pkg::NamespaceOf(s, a) == pkg::kStrictRelationshipsNs))
            relId = a.value();
        const pkg::Relationship* r = relOf(relId);
        SheetKind kind = SheetKind::Worksheet;
        std::string part;
        if (r && !r->external && package->FindPart(r->targetPart)) {
          part = r->targetPart;
          if (TypeEndsWith(r->type, "chartsheet")) kind = SheetKind::Chartsheet;
          else if (TypeEndsWith(r->type, "dialogsheet")) kind = SheetKind::Dialogsheet;
          else if (TypeEndsWith(r->type, "xlMacrosheet") || TypeEndsWith(r->type, "xlIntlMacrosheet")) kind = SheetKind::Macrosheet;
        }
        WorkbookLoader::SetSheetInfo(sheet, kind,
                                     state == "hidden"       ? SheetState::Hidden
                                     : state == "veryHidden" ? SheetState::VeryHidden
                                                             : SheetState::Visible,
                                     UIntAttr(s, "sheetId", 0), part, relId);
        WorkbookLoader::MarkLoaded(sheet, part.empty());
      }
    } else if (k == "definedNames") {
      for (pugi::xml_node n = sec.first_child(); n; n = n.next_sibling()) {
        if (n.type() != pugi::node_element || Local(n) != "definedName") continue;
        DefinedName d;
        d.name = Attr(n, "name");
        d.sheet = IntAttr(n, "localSheetId", -1);
        d.hidden = BoolAttr(n, "hidden", false);
        d.formula = DecodeXstring(TextOf(n));  // Excel: "_x005F_x0041_" for the text _x0041_
        loader.Names().push_back(std::move(d));
      }
    } else if (k == "calcPr") {
      CalcProps& c = loader.Calc();
      c.calcId = UIntAttr(sec, "calcId", 0);
      std::string_view mode = Attr(sec, "calcMode");
      c.mode = mode == "manual" ? CalcMode::Manual : mode == "autoNoTable" ? CalcMode::AutoNoTable : CalcMode::Auto;
      c.fullCalcOnLoad = BoolAttr(sec, "fullCalcOnLoad", false);
      c.forceFullCalc = BoolAttr(sec, "forceFullCalc", false);
      c.r1c1 = Attr(sec, "refMode") == "R1C1";
      c.iterate = BoolAttr(sec, "iterate", false);
      c.iterateCount = UIntAttr(sec, "iterateCount", 100);
      c.iterateDelta = NumAttr(sec, "iterateDelta", 0.001);
      c.fullPrecision = BoolAttr(sec, "fullPrecision", true);
      c.calcOnSave = BoolAttr(sec, "calcOnSave", true);
      c.concurrentCalc = BoolAttr(sec, "concurrentCalc", true);
      c.concurrentManualCount = UIntAttr(sec, "concurrentManualCount", 0);
    } else if (k == "extLst") {
      // xcalcf:calcFeatures: with RD, Single, FV and CNMTM listed and calcId 191028 or later, Excel
      // doesn't recalculate the workbook on opening (s1-calc's probes, 2026-11-17).
      for (pugi::xml_node ext = sec.first_child(); ext; ext = ext.next_sibling())
        for (pugi::xml_node f = Child(ext, "calcFeatures").first_child(); f; f = f.next_sibling())
          if (f.type() == pugi::node_element && Local(f) == "feature")
            loader.Calc().calcFeatures.emplace_back(Attr(f, "name"));
    }
  }
  // The errors Excel writes as #VALUE! with a rich value saying which, and which array formulas
  // spill, before any sheet is read.
  ReadMetadata(*package, metadata, richValues, richStructures, loader.ValueErrors(), loader.DynamicArrays());

  // Tables, for every sheet: small, and structured references reach across sheets.
  for (size_t i = 0; i < book->SheetCount(); ++i) {
    Sheet& sheet = book->GetSheet(i);
    const pkg::RelationshipSet* srels = sheet.PartName().empty() ? nullptr : package->RelationshipsOf(sheet.PartName());
    for (size_t r = 0; srels && r < srels->items.size(); ++r) {
      const pkg::Relationship& rel = srels->items[r];
      if (rel.external || !TypeEndsWith(rel.type, "table") || !package->FindPart(rel.targetPart)) continue;
      if (!(status = package->ReadPart(rel.targetPart, bytes)).Ok()) return nullptr;
      xml = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      Table table;
      if (!(status = ReadTable(xml, package->GetLimits(), rel.targetPart, table)).Ok()) return nullptr;
      WorkbookLoader::Tables(sheet).push_back(std::move(table));
    }
  }

  size_t count = book->SheetCount();
  loader.SetActive(count && activeTab < count ? activeTab : 0, count && firstTab < count ? firstTab : 0);

  // Theme, styles and shared strings, which every sheet needs.
  if (auto t = ReadThemePart(*package, theme)) {
    std::array<uint32_t, 12> rgb{};
    static constexpr draw::ThemeColor order[12] = {
        draw::ThemeColor::Light1,  draw::ThemeColor::Dark1,   draw::ThemeColor::Light2,    draw::ThemeColor::Dark2,
        draw::ThemeColor::Accent1, draw::ThemeColor::Accent2, draw::ThemeColor::Accent3,   draw::ThemeColor::Accent4,
        draw::ThemeColor::Accent5, draw::ThemeColor::Accent6, draw::ThemeColor::Hyperlink, draw::ThemeColor::FollowedHyperlink};
    auto byte = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255)); };
    for (int i = 0; i < 12; ++i) {
      draw::Color c = t->GetColor(order[i]);
      rgb[i] = 0xFF000000u | (byte(c.r) << 16) | (byte(c.g) << 8) | byte(c.b);
    }
    book->GetStyles().SetThemeColors(rgb);
    loader.SetTheme(std::move(t));
  }
  if (!styles.empty()) {
    if (!(status = package->ReadPart(styles, bytes)).Ok()) return nullptr;
    xml = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!(status = ReadStyles(xml, package->GetLimits(), styles.substr(1), book->GetStyles())).Ok()) return nullptr;
  }
  if (!strings.empty()) {
    if (!(status = package->ReadPart(strings, bytes)).Ok()) return nullptr;
    xml = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!(status = ReadStrings(xml, package->GetLimits(), strings.substr(1), book->Strings())).Ok()) return nullptr;
  }
  std::vector<uint8_t>().swap(bytes);

  loader.SetParts(main, styles, strings, theme);
  loader.SetPackage(std::move(package));
  loader.SetLoader([](Workbook& b, size_t i) { return LoadSheetPart(b, i); });
  // The active sheet now; the others when asked.
  if (count) {
    bool cut = false;
    if (options.firstBytes && !(status = LoadSheetStart(*book, book->ActiveSheet(), options.firstBytes, cut)).Ok())
      return nullptr;
    if (!cut && !(status = book->LoadSheet(book->ActiveSheet())).Ok()) return nullptr;
    if (options.partial) *options.partial = cut;
  }
  if (options.loadAll && !(status = book->LoadAll()).Ok()) return nullptr;
  return book;
}

std::vector<std::string> ModelParts(const Workbook& book) {
  std::vector<std::string> parts;
  if (!book.StringsPart().empty()) parts.push_back(book.StringsPart());
  for (size_t i = 0; i < book.SheetCount(); ++i) {
    const Sheet& s = book.GetSheet(i);
    if (s.Loaded() && !s.PartName().empty() && !s.Text().rootStart.empty()) parts.push_back(s.PartName());
  }
  return parts;
}

}  // namespace fw::sheet::xlsx
