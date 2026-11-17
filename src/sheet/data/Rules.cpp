#include "sheet/data/Rules.h"

#include <algorithm>
#include <cstdio>
#include <random>

#include "core/Strings.h"
#include "pkg/Formula.h"
#include "pkg/XmlWriter.h"
#include "sheet/calc/Entry.h"
#include "sheet/model/Sheet.h"
#include "sheet/xlsx/Internal.h"

namespace fw::sheet::data {

namespace {

constexpr std::string_view kXmNs = "http://schemas.microsoft.com/office/excel/2006/main";
constexpr std::string_view kDataBarExtUri = "{B025F937-C7B1-47D3-B67F-A62EFF666E3E}";
constexpr std::string_view kX14Ns = "http://schemas.microsoft.com/office/spreadsheetml/2009/9/main";

std::string_view LocalOf(std::string_view qname) {
  const size_t colon = qname.find(':');
  return colon == std::string_view::npos ? qname : qname.substr(colon + 1);
}

pugi::xml_node ChildLocal(pugi::xml_node n, std::string_view local) {
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && LocalOf(c.name()) == local) return c;
  return {};
}

std::string AttrOf(pugi::xml_node n, std::string_view local) {
  for (pugi::xml_attribute a = n.first_attribute(); a; a = a.next_attribute())
    if (LocalOf(a.name()) == local) return a.value();
  return {};
}

bool BoolOf(pugi::xml_node n, std::string_view local, bool def) {
  const std::string v = AttrOf(n, local);
  if (v.empty()) return def;
  return v == "1" || v == "true";
}

// Where an element's start tag ends: just after its '>'.
size_t StartTagEnd(std::string_view el) {
  char quote = 0;
  for (size_t j = 0; j < el.size(); ++j) {
    const char c = el[j];
    if (quote) {
      if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '>') {
      return j + 1;
    }
  }
  return el.size();
}

// The content of an element given as text (empty for a self-closing one).
std::string_view Inner(std::string_view el) {
  const size_t from = StartTagEnd(el);
  if (from >= el.size() || el[from - 2] == '/') return {};
  const size_t to = el.rfind("</");
  return to == std::string_view::npos || to < from ? std::string_view{} : el.substr(from, to - from);
}

std::vector<std::string_view> Children(std::string_view inner, std::string_view local) {
  std::vector<std::string_view> out;
  for (const xlsx::XmlPiece& p : xlsx::TopLevel(inner))
    if (p.local == local) out.push_back(inner.substr(p.begin, p.end - p.begin));
  return out;
}

std::string XmlChars(std::string_view s);

// Sets (or, with `remove`, takes out) an attribute in an element's start tag.
void SetAttr(std::string& el, std::string_view name, std::string_view value, bool remove = false) {
  const size_t end = StartTagEnd(el);
  const std::string_view tag(el.data(), end);
  for (size_t at = tag.find(name); at != std::string_view::npos; at = tag.find(name, at + 1)) {
    if (at == 0 || !std::isspace(static_cast<unsigned char>(tag[at - 1]))) continue;
    size_t eq = at + name.size();
    while (eq < tag.size() && std::isspace(static_cast<unsigned char>(tag[eq]))) ++eq;
    if (eq >= tag.size() || tag[eq] != '=') continue;
    size_t q = eq + 1;
    while (q < tag.size() && std::isspace(static_cast<unsigned char>(tag[q]))) ++q;
    if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\'')) continue;
    const size_t close = tag.find(tag[q], q + 1);
    if (close == std::string_view::npos) return;
    if (remove) {
      el.erase(at - 1, close + 1 - (at - 1));
    } else {
      std::string v;
      pkg::AppendEscapedAttribute(v, XmlChars(value));
      el.replace(q + 1, close - q - 1, v);
    }
    return;
  }
  if (remove) return;
  size_t insert = end - 1;
  if (insert > 0 && el[insert - 1] == '/') --insert;
  std::string a = " " + std::string(name) + "=\"";
  pkg::AppendEscapedAttribute(a, XmlChars(value));
  el.insert(insert, a + "\"");
}

// New XML written without a prefix takes the sheet's ("x:") on its unprefixed elements.
std::string Prefixed(std::string_view xml, const std::string& p) {
  if (p.empty()) return std::string(xml);
  std::string out;
  out.reserve(xml.size() + 32);
  for (size_t i = 0; i < xml.size(); ++i) {
    out += xml[i];
    if (xml[i] != '<' || i + 1 >= xml.size()) continue;
    size_t k = i + 1;
    if (xml[k] == '/') out += xml[k++];
    if (k >= xml.size() || xml[k] == '!' || xml[k] == '?') {
      i = k - 1;
      continue;
    }
    size_t e = k;
    while (e < xml.size() && !std::strchr(" \t\r\n/>", xml[e])) ++e;
    if (xml.substr(k, e - k).find(':') == std::string_view::npos) out += p;
    i = k - 1;
  }
  return out;
}

// Text without what XML 1.0 can't hold (control characters, broken UTF-8, U+FFFE and U+FFFF), so a
// rule always reads back and Excel never has to repair the part.
std::string XmlChars(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      if (c >= 0x20 || c == '\t' || c == '\n' || c == '\r') out += static_cast<char>(c);
      ++i;
      continue;
    }
    const size_t n = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    bool ok = n > 0 && i + n <= s.size();
    for (size_t k = 1; ok && k < n; ++k) ok = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
    if (ok && n >= 3) {
      const auto c1 = static_cast<unsigned char>(s[i + 1]);
      if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0) || (c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90))
        ok = false;
      if (n == 3 && c == 0xEF && c1 == 0xBF && static_cast<unsigned char>(s[i + 2]) >= 0xBE) ok = false;
    }
    if (ok) out.append(s.substr(i, n));
    i += ok ? n : 1;
  }
  return out;
}

std::string Esc(std::string_view s) {
  std::string out;
  pkg::AppendEscapedText(out, XmlChars(s));
  return out;
}
std::string EscAttr(std::string_view s) {
  std::string out;
  pkg::AppendEscapedAttribute(out, XmlChars(s));
  return out;
}

std::string NewGuid() {
  static std::mt19937_64 rng{std::random_device{}()};
  const uint64_t a = rng(), b = rng();
  char buf[40];
  std::snprintf(buf, sizeof buf, "{%08X-%04X-4%03X-%04X-%012llX}", static_cast<unsigned>(a >> 32),
                static_cast<unsigned>((a >> 16) & 0xFFFF), static_cast<unsigned>(a & 0xFFF),
                static_cast<unsigned>(0x8000 | (b >> 50 & 0x3FFF)), static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFull));
  return buf;
}

std::string TopLeft(const std::string& sqref) {
  const std::vector<Range> a = ParseAreas(sqref);
  return a.empty() ? std::string("A1") : CellName(a[0].firstRow, a[0].firstCol);
}

std::string Quoted(std::string_view text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '"') out += '"';
    out += c;
  }
  return out + "\"";
}

constexpr const char* kOpNames[] = {"between", "notBetween", "equal", "notEqual", "greaterThan", "lessThan",
                                    "greaterThanOrEqual", "lessThanOrEqual"};
CfOp OpOf(std::string_view name) {
  for (size_t i = 0; i < std::size(kOpNames); ++i)
    if (name == kOpNames[i]) return static_cast<CfOp>(i);
  return CfOp::Between;
}

}  // namespace

// ---- Sqrefs

std::vector<Range> ParseAreas(std::string_view sqref) {
  std::vector<Range> out;
  size_t i = 0;
  while (i < sqref.size()) {
    while (i < sqref.size() && sqref[i] == ' ') ++i;
    size_t e = sqref.find(' ', i);
    if (e == std::string_view::npos) e = sqref.size();
    Range r;
    if (e > i && ParseRange(sqref.substr(i, e - i), r)) out.push_back(r);
    i = e;
  }
  return out;
}

std::string JoinAreas(const std::vector<Range>& areas) {
  std::string out;
  for (const Range& r : areas) {
    if (!out.empty()) out += ' ';
    out += RangeName(r);
  }
  return out;
}

bool AreasContain(const std::vector<Range>& areas, uint32_t row, uint32_t col) {
  for (const Range& r : areas)
    if (row >= r.firstRow && row <= r.lastRow && col >= r.firstCol && col <= r.lastCol) return true;
  return false;
}

std::vector<Range> CutAreas(const std::vector<Range>& areas, const Range& cut) {
  std::vector<Range> out;
  for (const Range& r : areas) {
    if (r.lastRow < cut.firstRow || r.firstRow > cut.lastRow || r.lastCol < cut.firstCol || r.firstCol > cut.lastCol) {
      out.push_back(r);
      continue;
    }
    const uint32_t top = std::max(r.firstRow, cut.firstRow), bottom = std::min(r.lastRow, cut.lastRow);
    if (r.firstRow < cut.firstRow) out.push_back(Range{r.firstRow, r.firstCol, cut.firstRow - 1, r.lastCol});
    if (r.firstCol < cut.firstCol) out.push_back(Range{top, r.firstCol, bottom, cut.firstCol - 1});
    if (r.lastCol > cut.lastCol) out.push_back(Range{top, cut.lastCol + 1, bottom, r.lastCol});
    if (r.lastRow > cut.lastRow) out.push_back(Range{cut.lastRow + 1, r.firstCol, r.lastRow, r.lastCol});
  }
  return out;
}

// ---- Conditional formats

std::vector<CfRule> ReadCfRules(const detail::PartText& t) {
  std::string xml, ext;
  xlsx::GetSheetRules(t, false, xml, ext);
  struct Item {
    CfRule rule;
    uint64_t priority = 0;
    size_t order = 0;
    std::string id;  // the x14 id a main rule points to
  };
  std::vector<Item> items;
  for (const xlsx::XmlPiece& p : xlsx::TopLevel(xml)) {
    const std::string_view el = std::string_view(xml).substr(p.begin, p.end - p.begin);
    pugi::xml_document whole;
    if (!xlsx::ParseSlice(whole, el)) continue;
    const std::string sqref = AttrOf(whole.document_element(), "sqref");
    for (std::string_view rule : Children(Inner(el), "cfRule")) {
      pugi::xml_document rd;
      if (!xlsx::ParseSlice(rd, rule)) continue;
      pugi::xml_node r = rd.document_element();
      Item it;
      it.rule.sqref = sqref;
      it.rule.type = AttrOf(r, "type");
      it.rule.stopIfTrue = BoolOf(r, "stopIfTrue", false);
      it.rule.xml = std::string(rule);
      it.priority = r.attribute("priority").as_ullong(0);
      it.order = items.size();
      for (pugi::xml_node e : ChildLocal(r, "extLst").children())
        if (pugi::xml_node id = ChildLocal(e, "id")) it.id = xlsx::TextOf(id);
      items.push_back(std::move(it));
    }
  }
  for (const xlsx::XmlPiece& list : xlsx::TopLevel(ext)) {
    if (list.local != "conditionalFormattings") continue;
    const std::string_view inner = Inner(std::string_view(ext).substr(list.begin, list.end - list.begin));
    for (std::string_view cf : Children(inner, "conditionalFormatting")) {
      pugi::xml_document doc;
      if (!xlsx::ParseSlice(doc, cf)) continue;
      const std::string sqref = xlsx::TextOf(ChildLocal(doc.document_element(), "sqref"));
      for (std::string_view rule : Children(Inner(cf), "cfRule")) {
        pugi::xml_document rd;
        if (!xlsx::ParseSlice(rd, rule)) continue;
        pugi::xml_node r = rd.document_element();
        const std::string id = AttrOf(r, "id");
        auto main = std::find_if(items.begin(), items.end(), [&](const Item& i) { return !id.empty() && i.id == id && i.rule.ext.empty(); });
        if (main != items.end()) {
          main->rule.ext = std::string(rule);
          continue;
        }
        Item it;
        it.rule.sqref = sqref;
        it.rule.type = AttrOf(r, "type");
        it.rule.stopIfTrue = BoolOf(r, "stopIfTrue", false);
        it.rule.ext = std::string(rule);
        it.priority = r.attribute("priority").as_ullong(0);
        it.order = items.size();
        items.push_back(std::move(it));
      }
    }
  }
  std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.priority < b.priority; });
  std::vector<CfRule> out;
  out.reserve(items.size());
  for (Item& i : items) out.push_back(std::move(i.rule));
  return out;
}

void WriteCfRules(const std::vector<CfRule>& rules, const detail::PartText& t, std::string& xml, std::string& ext) {
  const std::string p = xlsx::SheetPrefix(t);
  xml.clear();
  ext.clear();
  std::string x14;
  size_t i = 0;
  while (i < rules.size()) {
    // Rules next to each other with the same cells share one element, as Excel writes them.
    std::string block;
    size_t j = i;
    for (; j < rules.size() && rules[j].sqref == rules[i].sqref; ++j) {
      const CfRule& r = rules[j];
      const std::string priority = std::to_string(j + 1);
      if (!r.xml.empty()) {
        std::string el = Prefixed(r.xml, p);
        SetAttr(el, "priority", priority);
        SetAttr(el, "stopIfTrue", "1", !r.stopIfTrue);
        block += el;
      }
      if (!r.ext.empty()) {
        std::string el = r.ext;
        if (r.xml.empty()) {
          SetAttr(el, "priority", priority);
          SetAttr(el, "stopIfTrue", "1", !r.stopIfTrue);
        }
        x14 += "<x14:conditionalFormatting xmlns:xm=\"" + std::string(kXmNs) + "\">" + el + "<xm:sqref>" + Esc(r.sqref) +
               "</xm:sqref></x14:conditionalFormatting>";
      }
    }
    if (!block.empty())
      xml += "<" + p + "conditionalFormatting sqref=\"" + EscAttr(rules[i].sqref) + "\">" + block + "</" + p +
             "conditionalFormatting>";
    i = j;
  }
  if (!x14.empty()) ext = "<x14:conditionalFormattings>" + x14 + "</x14:conditionalFormattings>";
}

Dxf CfPreset(int preset) {
  auto rgb = [](uint32_t v) { return Color{Color::Kind::Rgb, v, 0}; };
  static constexpr uint32_t kFont[] = {0xFF9C0006, 0xFF9C5700, 0xFF006100, 0, 0xFF9C0006, 0};
  static constexpr uint32_t kFill[] = {0xFFFFC7CE, 0xFFFFEB9C, 0xFFC6EFCE, 0xFFFFC7CE, 0, 0};
  preset = std::clamp(preset, 0, static_cast<int>(std::size(kCfPresets)) - 1);
  Dxf d;
  if (kFont[preset]) {
    d.font = Font{};
    d.font->color = rgb(kFont[preset]);
  }
  if (preset == 5) {  // Bold
    d.font = Font{};
    d.font->bold = true;
    d.fontBold = true;
  }
  if (kFill[preset]) {
    Fill f;
    f.pattern = PatternType::Solid;
    f.bg = rgb(kFill[preset]);
    d.fill = f;
  }
  return d;
}

CfRule CellIsRule(const std::string& sqref, CfOp op, const std::string& f1, const std::string& f2, uint32_t dxf) {
  CfRule r{sqref, "cellIs"};
  r.xml = "<cfRule type=\"cellIs\" dxfId=\"" + std::to_string(dxf) + "\" priority=\"1\" operator=\"" +
          kOpNames[static_cast<size_t>(op)] + "\"><formula>" + Esc(f1) + "</formula>";
  if (op == CfOp::Between || op == CfOp::NotBetween) r.xml += "<formula>" + Esc(f2) + "</formula>";
  r.xml += "</cfRule>";
  return r;
}

CfRule ExpressionRule(const std::string& sqref, const std::string& formula, uint32_t dxf) {
  CfRule r{sqref, "expression"};
  r.xml = "<cfRule type=\"expression\" dxfId=\"" + std::to_string(dxf) + "\" priority=\"1\"><formula>" + Esc(formula) +
          "</formula></cfRule>";
  return r;
}

CfRule TextRule(const std::string& sqref, CfText kind, const std::string& text, uint32_t dxf) {
  static constexpr const char* kTypes[] = {"containsText", "notContainsText", "beginsWith", "endsWith"};
  static constexpr const char* kOps[] = {"containsText", "notContains", "beginsWith", "endsWith"};
  const std::string cell = TopLeft(sqref), q = Quoted(text);
  std::string f;
  switch (kind) {
    case CfText::Contains: f = "NOT(ISERROR(SEARCH(" + q + "," + cell + ")))"; break;
    case CfText::NotContains: f = "ISERROR(SEARCH(" + q + "," + cell + "))"; break;
    case CfText::BeginsWith: f = "LEFT(" + cell + ",LEN(" + q + "))=" + q; break;
    case CfText::EndsWith: f = "RIGHT(" + cell + ",LEN(" + q + "))=" + q; break;
  }
  CfRule r{sqref, kTypes[static_cast<size_t>(kind)]};
  r.xml = "<cfRule type=\"" + r.type + "\" dxfId=\"" + std::to_string(dxf) + "\" priority=\"1\" operator=\"" +
          kOps[static_cast<size_t>(kind)] + "\" text=\"" + EscAttr(text) + "\"><formula>" + Esc(f) + "</formula></cfRule>";
  return r;
}

CfRule DateRule(const std::string& sqref, int period, uint32_t dxf) {
  static constexpr const char* kPeriods[] = {"yesterday", "today",    "tomorrow",  "last7Days", "lastWeek",
                                             "thisWeek",  "nextWeek", "lastMonth", "thisMonth", "nextMonth"};
  period = std::clamp(period, 0, 9);
  const std::string c = TopLeft(sqref);
  const std::string fl = "FLOOR(" + c + ",1)", rd = "TODAY()-ROUNDDOWN(" + c + ",0)", up = "ROUNDDOWN(" + c + ",0)-TODAY()";
  std::string f;
  switch (period) {
    case 0: f = fl + "=TODAY()-1"; break;
    case 1: f = fl + "=TODAY()"; break;
    case 2: f = fl + "=TODAY()+1"; break;
    case 3: f = "AND(TODAY()-" + fl + "<=6," + fl + "<=TODAY())"; break;
    case 4: f = "AND(" + rd + ">=(WEEKDAY(TODAY()))," + rd + "<(WEEKDAY(TODAY())+7))"; break;
    case 5: f = "AND(" + rd + "<=WEEKDAY(TODAY())-1," + up + "<=7-WEEKDAY(TODAY()))"; break;
    case 6: f = "AND(" + up + ">(7-WEEKDAY(TODAY()))," + up + "<(15-WEEKDAY(TODAY())))"; break;
    case 7: f = "AND(MONTH(" + c + ")=MONTH(EDATE(TODAY(),0-1)),YEAR(" + c + ")=YEAR(EDATE(TODAY(),0-1)))"; break;
    case 8: f = "AND(MONTH(" + c + ")=MONTH(TODAY()),YEAR(" + c + ")=YEAR(TODAY()))"; break;
    default: f = "AND(MONTH(" + c + ")=MONTH(EDATE(TODAY(),0+1)),YEAR(" + c + ")=YEAR(EDATE(TODAY(),0+1)))"; break;
  }
  CfRule r{sqref, "timePeriod"};
  r.xml = "<cfRule type=\"timePeriod\" dxfId=\"" + std::to_string(dxf) + "\" priority=\"1\" timePeriod=\"" +
          kPeriods[period] + "\"><formula>" + Esc(f) + "</formula></cfRule>";
  return r;
}

CfRule DuplicateRule(const std::string& sqref, bool unique, uint32_t dxf) {
  CfRule r{sqref, unique ? "uniqueValues" : "duplicateValues"};
  r.xml = "<cfRule type=\"" + r.type + "\" dxfId=\"" + std::to_string(dxf) + "\" priority=\"1\"/>";
  return r;
}

CfRule TopRule(const std::string& sqref, uint32_t rank, bool percent, bool bottom, uint32_t dxf) {
  CfRule r{sqref, "top10"};
  r.xml = "<cfRule type=\"top10\" dxfId=\"" + std::to_string(dxf) + "\" priority=\"1\"" + (percent ? " percent=\"1\"" : "") +
          (bottom ? " bottom=\"1\"" : "") + " rank=\"" + std::to_string(std::clamp<uint32_t>(rank, 1, 1000)) + "\"/>";
  return r;
}

CfRule AverageRule(const std::string& sqref, bool above, uint32_t dxf) {
  CfRule r{sqref, "aboveAverage"};
  r.xml = "<cfRule type=\"aboveAverage\" dxfId=\"" + std::to_string(dxf) + "\" priority=\"1\"" +
          (above ? "" : " aboveAverage=\"0\"") + "/>";
  return r;
}

CfRule ColorScaleRule(const std::string& sqref, int preset) {
  static constexpr uint32_t kScales[][3] = {
      {0xFF63BE7B, 0xFFFFEB84, 0xFFF8696B}, {0xFFF8696B, 0xFFFFEB84, 0xFF63BE7B}, {0xFF63BE7B, 0xFFFCFCFF, 0xFFF8696B},
      {0xFFF8696B, 0xFFFCFCFF, 0xFF63BE7B}, {0xFF5A8AC6, 0xFFFCFCFF, 0xFFF8696B}, {0xFFF8696B, 0xFFFCFCFF, 0xFF5A8AC6},
      {0xFFFCFCFF, 0xFFF8696B, 0},          {0xFFF8696B, 0xFFFCFCFF, 0},          {0xFF63BE7B, 0xFFFCFCFF, 0},
      {0xFFFCFCFF, 0xFF63BE7B, 0},          {0xFF63BE7B, 0xFFFFEF9C, 0},          {0xFFFFEF9C, 0xFF63BE7B, 0}};
  preset = std::clamp(preset, 0, 11);
  const uint32_t* c = kScales[preset];
  char buf[16];
  auto color = [&](uint32_t v) {
    std::snprintf(buf, sizeof buf, "%08X", v);
    return std::string("<color rgb=\"") + buf + "\"/>";
  };
  CfRule r{sqref, "colorScale"};
  r.xml = "<cfRule type=\"colorScale\" priority=\"1\"><colorScale><cfvo type=\"min\"/>";
  if (c[2]) r.xml += "<cfvo type=\"percentile\" val=\"50\"/>";
  r.xml += "<cfvo type=\"max\"/>" + color(c[0]) + color(c[1]) + (c[2] ? color(c[2]) : "") + "</colorScale></cfRule>";
  return r;
}

CfRule DataBarRule(const std::string& sqref, int preset, std::string id) {
  static constexpr uint32_t kColors[] = {0xFF638EC6, 0xFF63C384, 0xFFFF555A, 0xFFFFB628, 0xFF008AEF, 0xFFD6007B};
  preset = std::clamp(preset, 0, 11);
  const bool solid = preset >= 6;
  char rgb[16];
  std::snprintf(rgb, sizeof rgb, "%08X", kColors[preset % 6]);
  if (id.empty()) id = NewGuid();
  CfRule r{sqref, "dataBar"};
  r.xml = "<cfRule type=\"dataBar\" priority=\"1\"><dataBar><cfvo type=\"min\"/><cfvo type=\"max\"/><color rgb=\"" +
          std::string(rgb) + "\"/></dataBar><extLst><ext uri=\"" + std::string(kDataBarExtUri) + "\" xmlns:x14=\"" +
          std::string(kX14Ns) + "\"><x14:id>" + id + "</x14:id></ext></extLst></cfRule>";
  r.ext = "<x14:cfRule type=\"dataBar\" id=\"" + id + "\"><x14:dataBar minLength=\"0\" maxLength=\"100\"" +
          (solid ? std::string(" gradient=\"0\">") : std::string(" border=\"1\" negativeBarBorderColorSameAsPositive=\"0\">")) +
          "<x14:cfvo type=\"autoMin\"/><x14:cfvo type=\"autoMax\"/>" +
          (solid ? std::string() : "<x14:borderColor rgb=\"" + std::string(rgb) + "\"/>") +
          "<x14:negativeFillColor rgb=\"FFFF0000\"/>" + (solid ? "" : "<x14:negativeBorderColor rgb=\"FFFF0000\"/>") +
          "<x14:axisColor rgb=\"FF000000\"/></x14:dataBar></x14:cfRule>";
  return r;
}

CfRule IconSetRule(const std::string& sqref, int preset, std::string id) {
  preset = std::clamp(preset, 0, static_cast<int>(std::size(kIconSets)) - 1);
  const std::string name = kIconSets[preset];
  const int n = name[0] - '0';
  std::vector<int> at;
  for (int k = 0; k < n; ++k) at.push_back(n == 3 ? (k == 0 ? 0 : k == 1 ? 33 : 67) : k * 100 / n);
  CfRule r{sqref, "iconSet"};
  if (name == "3Triangles" || name == "3Stars" || name == "5Boxes") {  // Excel 2010's sets: the extension only
    if (id.empty()) id = NewGuid();
    r.ext = "<x14:cfRule type=\"iconSet\" priority=\"1\" id=\"" + id + "\"><x14:iconSet iconSet=\"" + name + "\">";
    for (int v : at) r.ext += "<x14:cfvo type=\"percent\"><xm:f>" + std::to_string(v) + "</xm:f></x14:cfvo>";
    r.ext += "</x14:iconSet></x14:cfRule>";
    return r;
  }
  r.xml = "<cfRule type=\"iconSet\" priority=\"1\"><iconSet" +
          (name == "3TrafficLights1" ? std::string() : " iconSet=\"" + name + "\"") + ">";
  for (int v : at) r.xml += "<cfvo type=\"percent\" val=\"" + std::to_string(v) + "\"/>";
  r.xml += "</iconSet></cfRule>";
  return r;
}

std::wstring DescribeCf(const CfRule& rule, const calc::FormulaLocale& region) {
  pugi::xml_document d;
  const bool parsed = xlsx::ParseSlice(d, rule.xml.empty() ? rule.ext : rule.xml);
  pugi::xml_node r = d.document_element();
  auto formula = [&](int k) {
    int i = 0;
    for (pugi::xml_node f = r.first_child(); f; f = f.next_sibling())
      if (LocalOf(f.name()) == "formula" && i++ == k) return Utf8ToWide(calc::ToLocal(xlsx::TextOf(f), region));
    return std::wstring();
  };
  const std::string& t = rule.type;
  if (!parsed) return Utf8ToWide(t);
  if (t == "cellIs") {
    static constexpr const wchar_t* kWords[] = {L"between", L"not between", L"=", L"<>", L">", L"<", L">=", L"<="};
    const CfOp op = OpOf(AttrOf(r, "operator"));
    std::wstring s = L"Cell Value " + std::wstring(kWords[static_cast<size_t>(op)]) + L" " + formula(0);
    if (op == CfOp::Between || op == CfOp::NotBetween) s += L" and " + formula(1);
    return s;
  }
  if (t == "expression") return L"Formula: =" + formula(0);
  const std::wstring text = Utf8ToWide(AttrOf(r, "text"));
  if (t == "containsText") return L"Cell Value contains \"" + text + L"\"";
  if (t == "notContainsText") return L"Cell Value does not contain \"" + text + L"\"";
  if (t == "beginsWith") return L"Cell Value begins with \"" + text + L"\"";
  if (t == "endsWith") return L"Cell Value ends with \"" + text + L"\"";
  if (t == "timePeriod") {
    static constexpr const char* kPeriods[] = {"yesterday", "today",    "tomorrow",  "last7Days", "lastWeek",
                                               "thisWeek",  "nextWeek", "lastMonth", "thisMonth", "nextMonth"};
    const std::string p = AttrOf(r, "timePeriod");
    for (size_t i = 0; i < std::size(kPeriods); ++i)
      if (p == kPeriods[i]) return kCfPeriods[i];
    return L"A date occurring";
  }
  if (t == "duplicateValues") return L"Duplicate Values";
  if (t == "uniqueValues") return L"Unique Values";
  if (t == "top10") {
    std::wstring s = BoolOf(r, "bottom", false) ? L"Bottom " : L"Top ";
    s += Utf8ToWide(AttrOf(r, "rank").empty() ? std::string("10") : AttrOf(r, "rank"));
    if (BoolOf(r, "percent", false)) s += L"%";
    return s;
  }
  if (t == "aboveAverage") {
    const bool above = BoolOf(r, "aboveAverage", true), equal = BoolOf(r, "equalAverage", false);
    const std::string sd = AttrOf(r, "stdDev");
    if (!sd.empty()) return Utf8ToWide(sd) + (above ? L" std dev above Average" : L" std dev below Average");
    return std::wstring(above ? L"Above Average" : L"Below Average") + (equal ? L" or equal" : L"");
  }
  if (t == "colorScale") return L"Graded Color Scale";
  if (t == "dataBar") return L"Data Bar";
  if (t == "iconSet") return L"Icon Set";
  if (t == "containsBlanks") return L"Blanks";
  if (t == "notContainsBlanks") return L"No Blanks";
  if (t == "containsErrors") return L"Errors";
  if (t == "notContainsErrors") return L"No Errors";
  return Utf8ToWide(t);
}

void ClearCfRules(std::vector<CfRule>& rules, const Range& cut) {
  for (CfRule& r : rules) {
    std::vector<Range> areas = ParseAreas(r.sqref);
    std::vector<Range> left = CutAreas(areas, cut);
    if (left.size() != areas.size() || !std::equal(left.begin(), left.end(), areas.begin(), [](const Range& a, const Range& b) {
          return a.firstRow == b.firstRow && a.firstCol == b.firstCol && a.lastRow == b.lastRow && a.lastCol == b.lastCol;
        }))
      SetCfSqref(r, JoinAreas(left));
  }
  std::erase_if(rules, [](const CfRule& r) { return r.sqref.empty(); });
}

void SetCfSqref(CfRule& rule, const std::string& sqref) { rule.sqref = sqref; }

namespace {
// The element's formulas (formula, xm:f) through `move`; false when it doesn't parse.
bool MoveFormulas(std::string& el, const std::string& oldId, const std::string& newId, MoveFormula move, void* ctx) {
  if (el.empty()) return true;
  pugi::xml_document d;
  if (!xlsx::ParseSlice(d, el)) return false;
  std::vector<pugi::xml_node> todo{d.document_element()};
  while (!todo.empty()) {
    pugi::xml_node n = todo.back();
    todo.pop_back();
    const std::string_view local = LocalOf(n.name());
    if (local == "formula" || local == "f") {
      const std::string moved = move(ctx, xlsx::TextOf(n));
      while (n.first_child()) n.remove_child(n.first_child());
      n.append_child(pugi::node_pcdata).set_value(moved.c_str());
      continue;
    }
    if ((local == "id" && !oldId.empty() && xlsx::TextOf(n) == oldId)) {
      while (n.first_child()) n.remove_child(n.first_child());
      n.append_child(pugi::node_pcdata).set_value(newId.c_str());
    }
    if (pugi::xml_attribute a = n.attribute("id"); a && !oldId.empty() && oldId == a.value()) a.set_value(newId.c_str());
    for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
      if (c.type() == pugi::node_element) todo.push_back(c);
  }
  struct Out : pugi::xml_writer {
    std::string s;
    void write(const void* data, size_t size) override { s.append(static_cast<const char*>(data), size); }
  } out;
  d.document_element().print(out, "", pugi::format_raw);
  el = std::move(out.s);
  return true;
}
}  // namespace

CfRule CopyCfRule(const CfRule& rule, const std::string& sqref, MoveFormula move, void* ctx) {
  CfRule r = rule;
  r.sqref = sqref;
  std::string oldId;
  if (!r.ext.empty()) {
    pugi::xml_document d;
    if (xlsx::ParseSlice(d, r.ext)) oldId = AttrOf(d.document_element(), "id");
  }
  const std::string newId = oldId.empty() ? std::string() : NewGuid();
  MoveFormulas(r.xml, oldId, newId, move, ctx);
  MoveFormulas(r.ext, oldId, newId, move, ctx);
  return r;
}

Validation CopyValidation(const Validation& v, const std::string& sqref, MoveFormula move, void* ctx) {
  Validation c = v;
  c.sqref = sqref;
  c.xml.clear();  // written again from the fields
  if (!c.f1.empty()) c.f1 = move(ctx, c.f1);
  if (!c.f2.empty()) c.f2 = move(ctx, c.f2);
  return c;
}

// ---- Tables

namespace {

bool IdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '\\' || static_cast<unsigned char>(c) >= 0x80; }

// The end of a bracketed item starting at `at` ('['): one past its matching ']', ' escaping.
size_t BracketEnd(std::string_view f, size_t at) {
  int depth = 0;
  for (size_t i = at; i < f.size(); ++i) {
    if (f[i] == '\'') {
      ++i;
      continue;
    }
    if (f[i] == '[') ++depth;
    else if (f[i] == ']' && --depth == 0) return i + 1;
  }
  return std::string_view::npos;
}

std::string Unescape(std::string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\'' && i + 1 < s.size()) ++i;
    out += s[i];
  }
  return out;
}

// One structured reference's brackets (without the table's name) as cells; empty when it can't.
std::string Cells(std::string_view spec, const Table& t, const std::string& sheet, uint32_t row) {
  std::string_view in = spec.substr(1, spec.size() - 2);  // inside the outer brackets
  bool all = false, data = false, headers = false, totals = false, thisRow = false;
  std::string col1, col2;
  auto item = [&](std::string_view x) {
    x = std::string_view(x.data(), x.size());
    while (!x.empty() && x.front() == ' ') x.remove_prefix(1);
    while (!x.empty() && x.back() == ' ') x.remove_suffix(1);
    if (x.size() >= 2 && x.front() == '[' && x.back() == ']' && BracketEnd(x, 0) == x.size()) x = x.substr(1, x.size() - 2);
    if (!x.empty() && x[0] == '#') {
      std::string k(x);
      for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      all |= k == "#all", data |= k == "#data", headers |= k == "#headers", totals |= k == "#totals";
      thisRow |= k == "#this row";
      return;
    }
    (col1.empty() ? col1 : col2) = Unescape(x);
  };
  if (!in.empty() && in[0] == '@') {
    thisRow = true;
    in.remove_prefix(1);
    if (!in.empty()) item(in);
  } else if (!in.empty() && in[0] == '[') {
    // [[#X],[A]:[B]] or [[A]:[B]]: the top-level items, a colon joining two columns.
    size_t i = 0;
    while (i < in.size()) {
      if (in[i] == '[') {
        const size_t e = BracketEnd(in, i);
        if (e == std::string_view::npos) return {};
        item(in.substr(i, e - i));
        i = e;
      } else {
        ++i;  // ',' ':' and spaces
      }
    }
  } else {
    item(in);
  }
  if (!data && !headers && !totals && !all && !thisRow) data = true;
  // The rows.
  const uint32_t dataFirst = t.ref.firstRow + t.headerRows;
  const uint32_t dataLast = t.ref.lastRow >= t.totalsRows ? t.ref.lastRow - t.totalsRows : t.ref.firstRow;
  uint32_t r1 = 0, r2 = 0;
  if (all) r1 = t.ref.firstRow, r2 = t.ref.lastRow;
  else if (thisRow) r1 = r2 = row;
  else {
    r1 = headers && t.headerRows ? t.ref.firstRow : data ? dataFirst : t.ref.lastRow;
    r2 = totals && t.totalsRows ? t.ref.lastRow : data ? dataLast : t.ref.firstRow;
    if (headers && !t.headerRows && !data) return {};
    if (totals && !t.totalsRows && !data) return {};
  }
  // The columns.
  uint32_t c1 = t.ref.firstCol, c2 = t.ref.lastCol;
  if (!col1.empty()) {
    const int a = t.FindColumn(col1), b = col2.empty() ? a : t.FindColumn(col2);
    if (a < 0 || b < 0) return {};
    c1 = t.ref.firstCol + static_cast<uint32_t>(std::min(a, b));
    c2 = t.ref.firstCol + static_cast<uint32_t>(std::max(a, b));
  }
  auto cell = [&](uint32_t r, uint32_t c, bool absRow) {
    return "$" + ColumnName(c) + (absRow ? "$" : "") + std::to_string(r + 1);
  };
  std::string out = pkg::QuoteSheetName(sheet) + "!" + cell(r1, c1, !thisRow);
  if (r1 != r2 || c1 != c2) out += ":" + cell(r2, c2, !thisRow);
  return out;
}

}  // namespace

std::string Unstructure(std::string_view f, const Table& t, const std::string& sheetName, uint32_t row, bool inside) {
  std::string out;
  bool changed = false;
  for (size_t i = 0; i < f.size();) {
    if (f[i] == '"') {  // a text constant
      size_t e = i + 1;
      while (e < f.size() && !(f[e] == '"' && (e + 1 >= f.size() || f[e + 1] != '"'))) e += f[e] == '"' ? 2 : 1;
      out.append(f.substr(i, e + 1 - i));
      i = e + 1;
      continue;
    }
    const bool start = i == 0 || !IdentChar(f[i - 1]);
    if (start && NameEqualsNoCase(f.substr(i, std::min(t.name.size(), f.size() - i)), t.name) && i + t.name.size() < f.size() &&
        f[i + t.name.size()] == '[') {
      const size_t b = i + t.name.size(), e = BracketEnd(f, b);
      if (e != std::string_view::npos) {
        const std::string cells = Cells(f.substr(b, e - b), t, sheetName, row);
        if (!cells.empty()) {
          out += cells;
          changed = true;
          i = e;
          continue;
        }
      }
    }
    if (inside && f[i] == '[' && (i == 0 || (!IdentChar(f[i - 1]) && f[i - 1] != ']' && f[i - 1] != '!'))) {
      const size_t e = BracketEnd(f, i);
      if (e != std::string_view::npos) {
        const std::string cells = Cells(f.substr(i, e - i), t, sheetName, row);
        if (!cells.empty()) {
          out += cells;
          changed = true;
          i = e;
          continue;
        }
      }
    }
    out += f[i++];
  }
  return changed ? out : std::string(f);
}

// ---- Data validation

namespace {

constexpr const char* kDvTypes[] = {"none", "whole", "decimal", "list", "date", "time", "textLength", "custom"};

Validation FromNode(pugi::xml_node n, bool x14) {
  Validation v;
  const std::string type = AttrOf(n, "type");
  for (size_t i = 0; i < std::size(kDvTypes); ++i)
    if (type == kDvTypes[i]) v.type = static_cast<Validation::Type>(i);
  v.op = OpOf(AttrOf(n, "operator"));
  v.allowBlank = BoolOf(n, "allowBlank", false);
  v.dropDown = !BoolOf(n, "showDropDown", false);
  v.showInput = BoolOf(n, "showInputMessage", false);
  v.showError = BoolOf(n, "showErrorMessage", false);
  const std::string style = AttrOf(n, "errorStyle");
  v.alert = style == "warning" ? Validation::Alert::Warning : style == "information" ? Validation::Alert::Information
                                                                                      : Validation::Alert::Stop;
  v.errorTitle = AttrOf(n, "errorTitle");
  v.error = AttrOf(n, "error");
  v.promptTitle = AttrOf(n, "promptTitle");
  v.prompt = AttrOf(n, "prompt");
  v.x14 = x14;
  if (x14) {
    v.sqref = xlsx::TextOf(ChildLocal(n, "sqref"));
    v.f1 = xlsx::TextOf(ChildLocal(ChildLocal(n, "formula1"), "f"));
    v.f2 = xlsx::TextOf(ChildLocal(ChildLocal(n, "formula2"), "f"));
  } else {
    v.sqref = AttrOf(n, "sqref");
    v.f1 = xlsx::TextOf(ChildLocal(n, "formula1"));
    v.f2 = xlsx::TextOf(ChildLocal(n, "formula2"));
  }
  return v;
}

// A formula that refers to another sheet: Excel 2010's extension holds the rule.
bool OtherSheet(std::string_view f) {
  bool quoted = false;
  for (char c : f) {
    if (c == '"') quoted = !quoted;
    else if (c == '!' && !quoted) return true;
  }
  return false;
}

std::string RuleAttrs(const Validation& v) {
  std::string a;
  if (v.type != Validation::Type::Any) a += " type=\"" + std::string(kDvTypes[static_cast<size_t>(v.type)]) + "\"";
  if (v.alert == Validation::Alert::Warning) a += " errorStyle=\"warning\"";
  if (v.alert == Validation::Alert::Information) a += " errorStyle=\"information\"";
  const bool hasOp = v.type != Validation::Type::Any && v.type != Validation::Type::List && v.type != Validation::Type::Custom;
  if (hasOp && v.op != CfOp::Between) a += " operator=\"" + std::string(kOpNames[static_cast<size_t>(v.op)]) + "\"";
  if (v.allowBlank) a += " allowBlank=\"1\"";
  if (v.type == Validation::Type::List && !v.dropDown) a += " showDropDown=\"1\"";
  if (v.showInput) a += " showInputMessage=\"1\"";
  if (v.showError) a += " showErrorMessage=\"1\"";
  if (!v.errorTitle.empty()) a += " errorTitle=\"" + EscAttr(v.errorTitle) + "\"";
  if (!v.error.empty()) a += " error=\"" + EscAttr(v.error) + "\"";
  if (!v.promptTitle.empty()) a += " promptTitle=\"" + EscAttr(v.promptTitle) + "\"";
  if (!v.prompt.empty()) a += " prompt=\"" + EscAttr(v.prompt) + "\"";
  return a;
}

bool TwoFormulas(const Validation& v) {
  return v.type != Validation::Type::Any && v.type != Validation::Type::List && v.type != Validation::Type::Custom &&
         (v.op == CfOp::Between || v.op == CfOp::NotBetween);
}

}  // namespace

std::vector<Validation> ReadValidations(const detail::PartText& t) {
  std::string xml, ext;
  xlsx::GetSheetRules(t, true, xml, ext);
  std::vector<Validation> out;
  for (const xlsx::XmlPiece& p : xlsx::TopLevel(xml))
    for (std::string_view el : Children(Inner(std::string_view(xml).substr(p.begin, p.end - p.begin)), "dataValidation")) {
      pugi::xml_document d;
      if (!xlsx::ParseSlice(d, el)) continue;
      Validation v = FromNode(d.document_element(), false);
      v.xml = std::string(el);
      out.push_back(std::move(v));
    }
  for (const xlsx::XmlPiece& p : xlsx::TopLevel(ext)) {
    if (p.local != "dataValidations") continue;
    for (std::string_view el : Children(Inner(std::string_view(ext).substr(p.begin, p.end - p.begin)), "dataValidation")) {
      pugi::xml_document d;
      if (!xlsx::ParseSlice(d, el)) continue;
      Validation v = FromNode(d.document_element(), true);
      v.xml = std::string(el);
      out.push_back(std::move(v));
    }
  }
  return out;
}

void WriteValidations(const std::vector<Validation>& rules, const detail::PartText& t, std::string& xml, std::string& ext) {
  const std::string p = xlsx::SheetPrefix(t);
  xml.clear();
  ext.clear();
  std::string main, x14;
  size_t mainCount = 0, x14Count = 0;
  for (const Validation& v : rules) {
    const bool inExt = v.xml.empty() ? OtherSheet(v.f1) || OtherSheet(v.f2) : v.x14;
    if (!v.xml.empty()) {
      std::string el = v.xml;
      if (inExt) {  // the cells are its xm:sqref
        const size_t a = el.find("sqref>"), b = a == std::string::npos ? a : el.find("</", a);
        if (b != std::string::npos) el.replace(a + 6, b - a - 6, Esc(v.sqref));
      } else {
        SetAttr(el, "sqref", v.sqref);
      }
      (inExt ? x14 : main) += inExt ? el : Prefixed(el, p);
    } else if (inExt) {
      x14 += "<x14:dataValidation" + RuleAttrs(v) + ">";
      if (v.type != Validation::Type::Any) x14 += "<x14:formula1><xm:f>" + Esc(v.f1) + "</xm:f></x14:formula1>";
      if (TwoFormulas(v)) x14 += "<x14:formula2><xm:f>" + Esc(v.f2) + "</xm:f></x14:formula2>";
      x14 += "<xm:sqref>" + Esc(v.sqref) + "</xm:sqref></x14:dataValidation>";
    } else {
      std::string el = "<dataValidation" + RuleAttrs(v) + " sqref=\"" + EscAttr(v.sqref) + "\">";
      if (v.type != Validation::Type::Any) el += "<formula1>" + Esc(v.f1) + "</formula1>";
      if (TwoFormulas(v)) el += "<formula2>" + Esc(v.f2) + "</formula2>";
      main += Prefixed(el + "</dataValidation>", p);
    }
    ++(inExt ? x14Count : mainCount);
  }
  if (mainCount) {
    // The list's own attributes as written (disablePrompts, the prompt's place), its count new.
    std::string old, oldExt;
    xlsx::GetSheetRules(t, true, old, oldExt);
    std::string start = "<" + p + "dataValidations>";
    if (!old.empty()) start = old.substr(0, StartTagEnd(old));
    if (start.size() > 1 && start[start.size() - 2] == '/') start.erase(start.size() - 2, 1);
    SetAttr(start, "count", std::to_string(mainCount));
    xml = start + main + "</" + p + "dataValidations>";
  }
  if (x14Count)
    ext = "<x14:dataValidations count=\"" + std::to_string(x14Count) + "\" xmlns:xm=\"" + std::string(kXmNs) + "\">" +
          x14 + "</x14:dataValidations>";
}

const Validation* ValidationAt(const std::vector<Validation>& rules, uint32_t row, uint32_t col) {
  for (const Validation& v : rules)
    if (AreasContain(ParseAreas(v.sqref), row, col)) return &v;
  return nullptr;
}

std::vector<std::string> ListItems(const Validation& v) {
  std::vector<std::string> out;
  const std::string& f = v.f1;
  if (v.type != Validation::Type::List || f.size() < 2 || f.front() != '"' || f.back() != '"') return out;
  std::string item;
  for (size_t i = 1; i + 1 < f.size(); ++i) {
    if (f[i] == '"' && i + 2 < f.size() && f[i + 1] == '"') {
      item += '"';
      ++i;
    } else if (f[i] == ',') {
      out.push_back(item);
      item.clear();
    } else {
      item += f[i];
    }
  }
  out.push_back(item);
  return out;
}

bool InList(const std::vector<std::string>& items, std::string_view value) {
  auto trim = [](std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
  };
  const std::string_view v = trim(value);
  return std::any_of(items.begin(), items.end(), [&](const std::string& i) { return trim(i) == v; });
}

void ClearValidations(std::vector<Validation>& rules, const Range& cut) {
  for (Validation& v : rules) {
    const std::vector<Range> areas = ParseAreas(v.sqref);
    const std::vector<Range> left = CutAreas(areas, cut);
    if (left.size() == areas.size() && std::equal(left.begin(), left.end(), areas.begin(), [](const Range& a, const Range& b) {
          return a.firstRow == b.firstRow && a.firstCol == b.firstCol && a.lastRow == b.lastRow && a.lastCol == b.lastCol;
        }))
      continue;
    v.sqref = JoinAreas(left);
  }
  std::erase_if(rules, [](const Validation& v) { return v.sqref.empty(); });
}

}  // namespace fw::sheet::data
