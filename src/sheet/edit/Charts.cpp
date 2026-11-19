#include "sheet/edit/Charts.h"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <vector>

#include <pugixml.hpp>

#include "pkg/Formula.h"
#include "pkg/XmlWriter.h"
#include "sheet/edit/Refs.h"
#include "sheet/model/Sheet.h"
#include "sheet/model/Workbook.h"
#include "sheet/xlsx/Internal.h"

namespace fw::sheet::edit::charts {

namespace {

constexpr std::string_view kChartNs = "http://schemas.openxmlformats.org/drawingml/2006/chart";
constexpr std::string_view kRelNs = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";

std::string Esc(std::string_view s) {
  std::string out;
  pkg::AppendEscapedText(out, s);
  return out;
}

// "Sheet1!$A$2:$A$5" (one cell: "Sheet1!$A$2").
std::string AbsRef(const std::string& sheet, const Range& r) {
  auto cell = [](uint32_t row, uint32_t col) {
    std::string name = CellName(row, col);
    const size_t digits = name.find_first_of("0123456789");
    return "$" + name.substr(0, digits) + "$" + name.substr(digits);
  };
  std::string out = pkg::QuoteSheetName(sheet) + "!" + cell(r.firstRow, r.firstCol);
  if (r.lastRow != r.firstRow || r.lastCol != r.firstCol) out += ":" + cell(r.lastRow, r.lastCol);
  return out;
}

// A cache of cells' values: numbers (numCache, invariant) or text (strCache, as the cells show).
std::string Cache(const Sheet& s, const Range& r, bool numbers) {
  std::string pts;
  uint32_t idx = 0, count = 0;
  for (uint32_t row = r.firstRow; row <= r.lastRow; ++row)
    for (uint32_t col = r.firstCol; col <= r.lastCol; ++col, ++idx) {
      ++count;
      const Value v = s.GetValue(row, col);
      std::string text;
      if (v.type == ValueType::Number) {
        char buf[48];
        text.assign(buf, xlsx::FormatNumber(v.number, 0, buf));
      } else if (v.type == ValueType::String && !numbers) {
        text = std::string(v.text);
      } else if (v.type == ValueType::Bool && !numbers) {
        text = v.boolean ? "TRUE" : "FALSE";
      } else {
        continue;
      }
      pts += "<c:pt idx=\"" + std::to_string(idx) + "\"><c:v>" + Esc(text) + "</c:v></c:pt>";
    }
  std::string head = numbers ? "<c:numCache><c:formatCode>General</c:formatCode>" : "<c:strCache>";
  return head + "<c:ptCount val=\"" + std::to_string(count) + "\"/>" + pts + (numbers ? "</c:numCache>" : "</c:strCache>");
}

bool AllNumbers(const Sheet& s, const Range& r) {
  for (uint32_t row = r.firstRow; row <= r.lastRow; ++row)
    for (uint32_t col = r.firstCol; col <= r.lastCol; ++col) {
      const ValueType t = s.GetValue(row, col).type;
      if (t != ValueType::Number && t != ValueType::Empty) return false;
    }
  return true;
}

// Excel's guess for a new chart's series. Each: its name's cell (or none), its values; the
// categories shared (or none).
struct Plan {
  struct One {
    std::optional<Range> name;
    Range values;
  };
  std::vector<One> series;
  std::optional<Range> categories;
};

Plan PlanSeries(const Sheet& s, const Range& d) {
  auto text = [&](uint32_t row, uint32_t col) {
    const ValueType t = s.GetValue(row, col).type;
    return t == ValueType::String || t == ValueType::Empty;
  };
  const bool many = d.lastRow > d.firstRow && d.lastCol > d.firstCol;
  bool headerRow = many && text(d.firstRow, d.firstCol), headerCol = headerRow;
  if (many) {
    // A header row: text across the top (past the corner); a header column: text down the left.
    headerRow = true;
    for (uint32_t c = d.firstCol + 1; c <= d.lastCol && headerRow; ++c) headerRow = s.GetValue(d.firstRow, c).type == ValueType::String;
    headerCol = true;
    for (uint32_t r = d.firstRow + 1; r <= d.lastRow && headerCol; ++r) headerCol = text(r, d.firstCol) && s.GetValue(r, d.firstCol).type != ValueType::Empty;
    if (!text(d.firstRow, d.firstCol)) headerRow = headerCol = false;
  }
  const Range body{d.firstRow + (headerRow ? 1u : 0u), d.firstCol + (headerCol ? 1u : 0u), d.lastRow, d.lastCol};
  Plan plan;
  if (body.firstRow > body.lastRow || body.firstCol > body.lastCol) return plan;
  const bool byCols = body.lastRow - body.firstRow >= body.lastCol - body.firstCol;
  if (byCols) {
    if (headerCol) plan.categories = Range{body.firstRow, d.firstCol, body.lastRow, d.firstCol};
    for (uint32_t c = body.firstCol; c <= body.lastCol; ++c)
      plan.series.push_back({headerRow ? std::optional<Range>(Range::Cell(d.firstRow, c)) : std::nullopt,
                             Range{body.firstRow, c, body.lastRow, c}});
  } else {
    if (headerRow) plan.categories = Range{d.firstRow, body.firstCol, d.firstRow, body.lastCol};
    for (uint32_t r = body.firstRow; r <= body.lastRow; ++r)
      plan.series.push_back({headerCol ? std::optional<Range>(Range::Cell(r, d.firstCol)) : std::nullopt,
                             Range{r, body.firstCol, r, body.lastCol}});
  }
  return plan;
}

// ---- Excel 2013 and later's look for a new chart (chart style 201, the Office theme), stated as
// Excel states it: text and lines in the theme's text color at 65 % and 15 %, series in the
// accents.

std::string Tx1(int mod, int off) {
  return "<a:schemeClr val=\"tx1\"><a:lumMod val=\"" + std::to_string(mod) + "000\"/><a:lumOff val=\"" + std::to_string(off) +
         "000\"/></a:schemeClr>";
}
std::string Line15() { return "<a:ln w=\"9525\" cap=\"flat\" cmpd=\"sng\" algn=\"ctr\"><a:solidFill>" + Tx1(15, 85) + "</a:solidFill><a:round/></a:ln>"; }
constexpr std::string_view kNoShape = "<c:spPr><a:noFill/><a:ln><a:noFill/></a:ln><a:effectLst/></c:spPr>";
std::string BodyPr(const char* rot) {
  return std::string("<a:bodyPr rot=\"") + rot +
         "\" spcFirstLastPara=\"1\" vertOverflow=\"ellipsis\" vert=\"horz\" wrap=\"square\" anchor=\"ctr\" anchorCtr=\"1\"/><a:lstStyle/>";
}
std::string DefRPr(int size) {
  return "<a:defRPr sz=\"" + std::to_string(size) +
         "\" b=\"0\" i=\"0\" u=\"none\" strike=\"noStrike\" kern=\"1200\" baseline=\"0\"><a:solidFill>" + Tx1(65, 35) +
         "</a:solidFill><a:latin typeface=\"+mn-lt\"/><a:ea typeface=\"+mn-ea\"/><a:cs typeface=\"+mn-cs\"/></a:defRPr>";
}
std::string TxPr(int size, const char* rot) {
  return "<c:txPr>" + BodyPr(rot) + "<a:p><a:pPr>" + DefRPr(size) + "</a:pPr><a:endParaRPr lang=\"en-US\"/></a:p></c:txPr>";
}
// A color element for the n-th series or slice: accent1 to 6, then Excel's darker and lighter rounds.
std::string Accent(size_t n) {
  static const char* const kRounds[] = {"", "<a:lumMod val=\"60000\"/>", "<a:lumMod val=\"80000\"/><a:lumOff val=\"20000\"/>",
                                        "<a:lumMod val=\"80000\"/>", "<a:lumMod val=\"60000\"/><a:lumOff val=\"40000\"/>",
                                        "<a:lumMod val=\"50000\"/>"};
  const std::string accent = "<a:schemeClr val=\"accent" + std::to_string(n % 6 + 1) + "\"";
  const std::string mods = kRounds[(n / 6) % 6];
  return mods.empty() ? accent + "/>" : accent + ">" + mods + "</a:schemeClr>";
}
std::string Solid(const std::string& color) { return "<a:solidFill>" + color + "</a:solidFill>"; }
std::string Rgb(uint32_t rgb) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string hex(6, '0');
  for (int i = 5; i >= 0; --i, rgb >>= 4) hex[static_cast<size_t>(i)] = kHex[rgb & 15];
  return "<a:srgbClr val=\"" + hex + "\"/>";
}

// A cache's XML from cells' values: numbers (numCache) or text (strCache).
std::string CacheXml(const std::vector<refs::CachePoint>& points, bool numbers) {
  std::string out = numbers ? "<c:numCache><c:formatCode>General</c:formatCode>" : "<c:strCache>";
  out += "<c:ptCount val=\"" + std::to_string(points.size()) + "\"/>";
  for (size_t i = 0; i < points.size(); ++i) {
    const refs::CachePoint& p = points[i];
    if (p.kind == refs::CachePoint::Empty || (numbers && p.kind != refs::CachePoint::Number)) continue;
    out += "<c:pt idx=\"" + std::to_string(i) + "\"><c:v>" + Esc(p.text) + "</c:v></c:pt>";
  }
  return out + (numbers ? "</c:numCache>" : "</c:strCache>");
}

// One series in a kind's look: its <c:tx> (or none), its categories' and values' references
// (c:strRef or c:numRef elements; no categories: empty), its color element (empty: the accent),
// and how many values it has (a pie's slices).
struct SeriesParts {
  std::string tx, cat, val, color;
  size_t points = 0;
};
std::string SeriesXml(ChartKind kind, size_t i, const SeriesParts& p) {
  using K = ChartKind;
  const bool scatter = kind == K::Scatter;
  const std::string fill = Solid(p.color.empty() ? Accent(i) : p.color);
  std::string s = "<c:ser><c:idx val=\"" + std::to_string(i) + "\"/><c:order val=\"" + std::to_string(i) + "\"/>" + p.tx;
  switch (kind) {
    case K::Column:
    case K::Bar: s += "<c:spPr>" + fill + "<a:ln><a:noFill/></a:ln><a:effectLst/></c:spPr><c:invertIfNegative val=\"0\"/>"; break;
    case K::Area: s += "<c:spPr>" + fill + "<a:ln><a:noFill/></a:ln><a:effectLst/></c:spPr>"; break;
    case K::Line:
      s += "<c:spPr><a:ln w=\"28575\" cap=\"rnd\">" + fill + "<a:round/></a:ln><a:effectLst/></c:spPr><c:marker><c:symbol val=\"none\"/></c:marker>";
      break;
    case K::Scatter:
      s += "<c:spPr><a:ln w=\"25400\" cap=\"rnd\"><a:noFill/><a:round/></a:ln><a:effectLst/></c:spPr><c:marker><c:symbol val=\"circle\"/>"
           "<c:size val=\"5\"/><c:spPr>" + fill + "<a:ln w=\"9525\">" + fill + "</a:ln><a:effectLst/></c:spPr></c:marker>";
      break;
    case K::Pie:
    case K::Doughnut:
      // A slice per value, each in its color with a white edge.
      for (size_t k = 0; k < p.points && k < 255; ++k)
        s += "<c:dPt><c:idx val=\"" + std::to_string(k) + "\"/><c:bubble3D val=\"0\"/><c:spPr>" + Solid(Accent(k)) +
             "<a:ln w=\"19050\"><a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill></a:ln><a:effectLst/></c:spPr></c:dPt>";
      break;
  }
  if (!p.cat.empty()) s += (scatter ? "<c:xVal>" : "<c:cat>") + p.cat + (scatter ? "</c:xVal>" : "</c:cat>");
  s += (scatter ? "<c:yVal>" : "<c:val>") + p.val + (scatter ? "</c:yVal>" : "</c:val>");
  if (kind == K::Line || scatter) s += "<c:smooth val=\"0\"/>";
  return s + "</c:ser>";
}

std::string LabelsXml(bool round, bool on) {
  return std::string("<c:dLbls><c:showLegendKey val=\"0\"/><c:showVal val=\"") + (on ? "1" : "0") +
         "\"/><c:showCatName val=\"0\"/><c:showSerName val=\"0\"/><c:showPercent val=\"0\"/><c:showBubbleSize val=\"0\"/>" +
         (round ? "<c:showLeaderLines val=\"1\"/>" : "") + "</c:dLbls>";
}

// A chart group of `kind` holding `sers`, on the axes `x` and `y` (ids).
std::string GroupXml(ChartKind kind, const std::string& sers, bool labels, const std::string& x, const std::string& y) {
  using K = ChartKind;
  const std::string dl = LabelsXml(kind == K::Pie || kind == K::Doughnut, labels);
  const std::string axIds = "<c:axId val=\"" + x + "\"/><c:axId val=\"" + y + "\"/>";
  switch (kind) {
    case K::Column:
    case K::Bar:
      return std::string("<c:barChart><c:barDir val=\"") + (kind == K::Bar ? "bar" : "col") +
             "\"/><c:grouping val=\"clustered\"/><c:varyColors val=\"0\"/>" + sers + dl + "<c:gapWidth val=\"219\"/><c:overlap val=\"-27\"/>" +
             axIds + "</c:barChart>";
    case K::Line:
      return "<c:lineChart><c:grouping val=\"standard\"/><c:varyColors val=\"0\"/>" + sers + dl + "<c:marker val=\"1\"/>" + axIds + "</c:lineChart>";
    case K::Area: return "<c:areaChart><c:grouping val=\"standard\"/><c:varyColors val=\"0\"/>" + sers + dl + axIds + "</c:areaChart>";
    case K::Pie: return "<c:pieChart><c:varyColors val=\"1\"/>" + sers + dl + "<c:firstSliceAng val=\"0\"/></c:pieChart>";
    case K::Doughnut:
      return "<c:doughnutChart><c:varyColors val=\"1\"/>" + sers + dl + "<c:firstSliceAng val=\"0\"/><c:holeSize val=\"75\"/></c:doughnutChart>";
    case K::Scatter:
      return "<c:scatterChart><c:scatterStyle val=\"lineMarker\"/><c:varyColors val=\"0\"/>" + sers + dl + axIds + "</c:scatterChart>";
  }
  return {};
}

// The axes a kind has (none for pies): their gridlines and line in the light gray, labels in 9 points.
std::string AxesXml(ChartKind kind, const std::string& x, const std::string& y) {
  auto axis = [](const char* el, const std::string& id, const std::string& cross, const char* pos, bool grid, bool line, const char* tail) {
    return std::string("<c:") + el + "><c:axId val=\"" + id + "\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling>"
           "<c:delete val=\"0\"/><c:axPos val=\"" + pos + "\"/>" +
           (grid ? "<c:majorGridlines><c:spPr>" + Line15() + "<a:effectLst/></c:spPr></c:majorGridlines>" : std::string()) +
           "<c:numFmt formatCode=\"General\" sourceLinked=\"1\"/><c:majorTickMark val=\"none\"/><c:minorTickMark val=\"none\"/>"
           "<c:tickLblPos val=\"nextTo\"/>" + (line ? "<c:spPr><a:noFill/>" + Line15() + "<a:effectLst/></c:spPr>" : std::string(kNoShape)) +
           TxPr(900, "-60000000") + "<c:crossAx val=\"" + cross + "\"/><c:crosses val=\"autoZero\"/>" + tail + "</c:" + el + ">";
  };
  if (kind == ChartKind::Pie || kind == ChartKind::Doughnut) return {};
  if (kind == ChartKind::Scatter)
    return axis("valAx", x, y, "b", true, false, "<c:crossBetween val=\"midCat\"/>") +
           axis("valAx", y, x, "l", true, false, "<c:crossBetween val=\"midCat\"/>");
  const bool bar = kind == ChartKind::Bar;
  return axis("catAx", x, y, bar ? "l" : "b", false, true, "<c:auto val=\"1\"/><c:lblAlgn val=\"ctr\"/><c:lblOffset val=\"100\"/><c:noMultiLvlLbl val=\"0\"/>") +
         axis("valAx", y, x, bar ? "b" : "l", true, false, "<c:crossBetween val=\"between\"/>");
}

// A title in Excel's look: `size` hundredths of a point, the text turned by `rot` (60000ths of a
// degree); no text: Excel's own.
std::string TitleXml(const std::string& text, int size, const char* rot) {
  const std::string tx = text.empty() ? std::string()
                                      : "<c:tx><c:rich>" + BodyPr(rot) + "<a:p><a:pPr>" + DefRPr(size) +
                                            "</a:pPr><a:r><a:rPr lang=\"en-US\"/><a:t>" + Esc(text) + "</a:t></a:r></a:p></c:rich></c:tx>";
  return "<c:title>" + tx + "<c:overlay val=\"0\"/>" + std::string(kNoShape) + TxPr(size, rot) + "</c:title>";
}

std::string LegendPos(char pos) { return pos == 't' ? "t" : pos == 'l' ? "l" : pos == 'r' ? "r" : "b"; }
std::string LegendXml(char pos) {
  return "<c:legend><c:legendPos val=\"" + LegendPos(pos) + "\"/><c:overlay val=\"0\"/>" + std::string(kNoShape) + TxPr(900, "0") + "</c:legend>";
}

std::string ChartXml(const Sheet& s, const Plan& plan, ChartKind kind, const std::string& title) {
  const std::string name = s.Name();
  const bool round = kind == ChartKind::Pie || kind == ChartKind::Doughnut;
  std::string sers;
  for (size_t i = 0; i < plan.series.size(); ++i) {
    const Plan::One& one = plan.series[i];
    SeriesParts p;
    if (one.name) p.tx = "<c:tx><c:strRef><c:f>" + Esc(AbsRef(name, *one.name)) + "</c:f>" + Cache(s, *one.name, false) + "</c:strRef></c:tx>";
    if (plan.categories) {
      const bool nums = AllNumbers(s, *plan.categories);
      const std::string ref = nums ? "numRef" : "strRef";
      p.cat = "<c:" + ref + "><c:f>" + Esc(AbsRef(name, *plan.categories)) + "</c:f>" + Cache(s, *plan.categories, nums) + "</c:" + ref + ">";
    }
    p.val = "<c:numRef><c:f>" + Esc(AbsRef(name, one.values)) + "</c:f>" + Cache(s, one.values, true) + "</c:numRef>";
    p.points = static_cast<size_t>(one.values.lastRow - one.values.firstRow + 1) * (one.values.lastCol - one.values.firstCol + 1);
    sers += SeriesXml(kind, i, p);
  }
  const std::string legend = plan.series.size() > 1 || round ? LegendXml('b') : std::string();
  return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<c:chartSpace xmlns:c=\"" + std::string(kChartNs) +
         "\" xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" xmlns:r=\"" + std::string(kRelNs) +
         "\"><c:date1904 val=\"0\"/><c:roundedCorners val=\"0\"/><c:chart>" + TitleXml(title, 1400, "0") +
         "<c:autoTitleDeleted val=\"0\"/><c:plotArea><c:layout/>" + GroupXml(kind, sers, false, "500000001", "500000002") +
         AxesXml(kind, "500000001", "500000002") + std::string(kNoShape) + "</c:plotArea>" + legend +
         "<c:plotVisOnly val=\"1\"/><c:dispBlanksAs val=\"gap\"/></c:chart>" +
         "<c:spPr><a:solidFill><a:schemeClr val=\"bg1\"/></a:solidFill>" + Line15() + "<a:effectLst/></c:spPr>"
         "<c:txPr><a:bodyPr/><a:lstStyle/><a:p><a:pPr><a:defRPr/></a:pPr><a:endParaRPr lang=\"en-US\"/></a:p></c:txPr></c:chartSpace>";
}

// ---- Reading and editing a chart part (pugixml; prefixes as the part writes them)

struct Writer : pugi::xml_writer {
  std::string out;
  void write(const void* data, size_t size) override { out.append(static_cast<const char*>(data), size); }
};
std::string Print(pugi::xml_node n) {
  Writer w;
  n.print(w, "", pugi::format_raw);
  return std::move(w.out);
}
pugi::xml_node Kid(pugi::xml_node n, std::string_view local) {
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && xlsx::Local(c) == local) return c;
  return {};
}
bool Is(pugi::xml_node n, std::initializer_list<std::string_view> locals) {
  const std::string_view l = xlsx::Local(n);
  return n.type() == pugi::node_element && std::find(locals.begin(), locals.end(), l) != locals.end();
}
// The first child among `locals`, where a new child that comes before them goes.
pugi::xml_node FirstOf(pugi::xml_node n, std::initializer_list<std::string_view> locals) {
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
    if (Is(c, locals)) return c;
  return {};
}
// `xml` (elements) put into `parent` before `before` (null: at the end); the first one put.
pugi::xml_node Put(pugi::xml_node parent, pugi::xml_node before, const std::string& xml) {
  pugi::xml_document t;
  if (!t.load_buffer(xml.data(), xml.size(), pugi::parse_default | pugi::parse_ws_pcdata_single)) return {};
  pugi::xml_node first;
  for (pugi::xml_node c = t.first_child(); c; c = c.next_sibling()) {
    pugi::xml_node n = before ? parent.insert_copy_before(c, before) : parent.append_copy(c);
    if (!first) first = n;
  }
  return first;
}
void SetVal(pugi::xml_node parent, std::string_view local, const std::string& val, pugi::xml_node before = {}) {
  pugi::xml_node n = Kid(parent, local);
  if (!n) n = Put(parent, before, "<c:" + std::string(local) + " val=\"\"/>");
  if (n) (n.attribute("val") ? n.attribute("val") : n.append_attribute("val")).set_value(val.c_str());
}
bool IsGroup(pugi::xml_node n) {
  const std::string_view l = xlsx::Local(n);
  return n.type() == pugi::node_element && l.size() > 5 && l.substr(l.size() - 5) == "Chart";
}
std::vector<pugi::xml_node> Groups(pugi::xml_node plot) {
  std::vector<pugi::xml_node> out;
  for (pugi::xml_node c = plot.first_child(); c; c = c.next_sibling())
    if (IsGroup(c)) out.push_back(c);
  return out;
}
std::vector<pugi::xml_node> SeriesOf(pugi::xml_node plot) {
  std::vector<pugi::xml_node> out;
  for (pugi::xml_node g : Groups(plot))
    for (pugi::xml_node c = g.first_child(); c; c = c.next_sibling())
      if (Is(c, {"ser"})) out.push_back(c);
  return out;
}
std::optional<ChartKind> KindOf(pugi::xml_node plot) {
  const std::vector<pugi::xml_node> groups = Groups(plot);
  if (groups.size() != 1) return std::nullopt;
  const std::string_view l = xlsx::Local(groups[0]);
  if (l == "barChart") return xlsx::Attr(Kid(groups[0], "barDir"), "val") == "bar" ? ChartKind::Bar : ChartKind::Column;
  if (l == "lineChart") return ChartKind::Line;
  if (l == "pieChart") return ChartKind::Pie;
  if (l == "doughnutChart") return ChartKind::Doughnut;
  if (l == "areaChart") return ChartKind::Area;
  if (l == "scatterChart") return ChartKind::Scatter;
  return std::nullopt;
}
// The first group's axes: the category (X) one and the value (Y) one.
std::pair<pugi::xml_node, pugi::xml_node> AxesOf(pugi::xml_node plot) {
  std::vector<std::string_view> ids;
  const std::vector<pugi::xml_node> groups = Groups(plot);
  if (!groups.empty())
    for (pugi::xml_node c = groups[0].first_child(); c; c = c.next_sibling())
      if (Is(c, {"axId"})) ids.push_back(xlsx::Attr(c, "val"));
  auto find = [&](size_t i) {
    for (pugi::xml_node c = plot.first_child(); i < ids.size() && c; c = c.next_sibling())
      if (Is(c, {"catAx", "valAx", "dateAx", "serAx"}) && xlsx::Attr(Kid(c, "axId"), "val") == ids[i]) return c;
    return pugi::xml_node();
  };
  return {find(0), find(1)};
}
std::string TextIn(pugi::xml_node tx) {
  std::string out;
  if (pugi::xml_node rich = Kid(tx, "rich")) {
    for (pugi::xml_node p = rich.first_child(); p; p = p.next_sibling()) {
      if (!Is(p, {"p"})) continue;
      if (!out.empty()) out += '\n';
      for (pugi::xml_node r = p.first_child(); r; r = r.next_sibling())
        if (Is(r, {"r", "fld"})) out += xlsx::TextOf(Kid(r, "t"));
    }
  }
  return out;
}
ChartText TitleOf(pugi::xml_node title) {
  ChartText t;
  t.shown = static_cast<bool>(title);
  t.text = TextIn(Kid(title, "tx"));
  return t;
}
std::string FormulaIn(pugi::xml_node holder) {
  for (pugi::xml_node c = holder.first_child(); c; c = c.next_sibling())
    if (pugi::xml_node f = Kid(c, "f")) return xlsx::TextOf(f);
  if (pugi::xml_node v = Kid(holder, "v")) return "=\"" + xlsx::TextOf(v) + "\"";  // a name typed as text
  return {};
}

// A title put, changed or taken off (`chart`: the chart's own, else an axis's).
void SetTitle(pugi::xml_node parent, const ChartText& t, bool chart, int size, const char* rot) {
  pugi::xml_node title = Kid(parent, "title");
  if (!t.shown) {
    if (title) parent.remove_child(title);
    if (chart) SetVal(parent, "autoTitleDeleted", "1", parent.first_child());
    return;
  }
  if (!title) {
    pugi::xml_node before = chart ? parent.first_child()
                                  : FirstOf(parent, {"numFmt", "majorTickMark", "minorTickMark", "tickLblPos", "spPr", "txPr", "crossAx"});
    title = Put(parent, before, TitleXml(t.text, size, rot));
  } else if (pugi::xml_node tx = Kid(title, "tx"); t.text.empty()) {
    if (tx) title.remove_child(tx);
  } else {
    // The text's look: the old text's first paragraph and run, or the title's txPr.
    pugi::xml_node rich = Kid(tx, "rich"), txPr = Kid(title, "txPr");
    pugi::xml_node look = rich ? rich : txPr;
    pugi::xml_node p = Kid(look, "p");
    const std::string body = look && Kid(look, "bodyPr") ? Print(Kid(look, "bodyPr")) : BodyPr(rot);
    const std::string list = look && Kid(look, "lstStyle") ? Print(Kid(look, "lstStyle")) : "<a:lstStyle/>";
    const std::string pPr = Kid(p, "pPr") ? Print(Kid(p, "pPr")) : "<a:pPr>" + DefRPr(size) + "</a:pPr>";
    const std::string rPr = Kid(Kid(p, "r"), "rPr") ? Print(Kid(Kid(p, "r"), "rPr")) : "<a:rPr lang=\"en-US\"/>";
    std::string paras;
    size_t at = 0;
    do {
      const size_t nl = t.text.find('\n', at);
      const std::string line = t.text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
      paras += "<a:p>" + pPr + "<a:r>" + rPr + "<a:t>" + Esc(line) + "</a:t></a:r></a:p>";
      at = nl == std::string::npos ? nl : nl + 1;
    } while (at != std::string::npos);
    if (tx) title.remove_child(tx);
    Put(title, title.first_child(), "<c:tx><c:rich>" + body + list + paras + "</c:rich></c:tx>");
  }
  if (chart) SetVal(parent, "autoTitleDeleted", "0", Kid(parent, "title").next_sibling());
}

// A series' (or point's) spPr given `color`: its fill, or its line for lines and scatters' markers.
void SetColor(pugi::xml_node holder, const std::string& color, bool stroke, std::initializer_list<std::string_view> after) {
  pugi::xml_node sp = Kid(holder, "spPr");
  if (!sp) {
    pugi::xml_node before;
    for (pugi::xml_node c = holder.first_child(); c; c = c.next_sibling())
      if (c.type() == pugi::node_element && !Is(c, after)) {
        before = c;
        break;
      }
    sp = Put(holder, before, "<c:spPr/>");
  }
  const char* fills[] = {"noFill", "solidFill", "gradFill", "blipFill", "pattFill", "grpFill"};
  auto replaceFill = [&](pugi::xml_node in, pugi::xml_node before) {
    for (pugi::xml_node c = in.first_child(); c; c = c.next_sibling())
      for (const char* f : fills)
        if (Is(c, {f})) {
          Put(in, c, Solid(color));
          in.remove_child(c);
          return;
        }
    Put(in, before, Solid(color));
  };
  if (!stroke) return replaceFill(sp, FirstOf(sp, {"ln", "effectLst", "effectDag", "scene3d", "sp3d", "extLst"}));
  pugi::xml_node ln = Kid(sp, "ln");
  if (!ln) ln = Put(sp, FirstOf(sp, {"effectLst", "effectDag", "scene3d", "sp3d", "extLst"}), "<a:ln w=\"28575\" cap=\"rnd\"/>");
  replaceFill(ln, ln.first_child());
}

// A series as its parts, for writing it anew: its color the solid fill (or line) it has.
SeriesParts PartsOf(pugi::xml_node ser) {
  SeriesParts p;
  if (pugi::xml_node tx = Kid(ser, "tx")) p.tx = Print(tx);
  for (std::string_view l : {"cat", "xVal"})
    if (pugi::xml_node c = Kid(ser, l); c && c.first_child()) p.cat = Print(c.first_child());
  for (std::string_view l : {"val", "yVal"})
    if (pugi::xml_node c = Kid(ser, l); c && c.first_child()) {
      p.val = Print(c.first_child());
      for (pugi::xml_node x = c.first_child().first_child(); x; x = x.next_sibling())
        if (pugi::xml_node n = Kid(x, "ptCount")) p.points = std::strtoul(std::string(xlsx::Attr(n, "val")).c_str(), nullptr, 10);
    }
  pugi::xml_node sp = Kid(ser, "spPr");
  pugi::xml_node solid = Kid(sp, "solidFill") ? Kid(sp, "solidFill") : Kid(Kid(sp, "ln"), "solidFill");
  if (Kid(Kid(ser, "marker"), "spPr") && !solid) solid = Kid(Kid(Kid(ser, "marker"), "spPr"), "solidFill");
  if (solid && solid.first_child()) p.color = Print(solid.first_child());
  return p;
}

// A reference typed in Select Data, with its cache; false when it isn't one.
bool RefXml(const Workbook& book, const std::string& f, bool numbers, std::string& out, size_t* count) {
  std::vector<refs::CachePoint> points;
  if (!CachePoints(book, f, false, points)) return false;
  if (count) *count = points.size();
  if (!numbers) {
    numbers = true;
    for (const refs::CachePoint& p : points) numbers = numbers && p.kind != refs::CachePoint::Text;
  }
  const std::string ref = numbers ? "numRef" : "strRef";
  out = "<c:" + ref + "><c:f>" + Esc(f) + "</c:f>" + CacheXml(points, numbers) + "</c:" + ref + ">";
  return true;
}

}  // namespace

std::string NewChartXml(const Sheet& s, const Range& data, ChartKind kind, const std::string& title) {
  const Plan plan = PlanSeries(s, data);
  return plan.series.empty() ? std::string() : ChartXml(s, plan, kind, title);
}

bool CachePoints(const Workbook& book, std::string_view f, bool visibleOnly, std::vector<refs::CachePoint>& out) {
  const size_t bang = f.rfind('!');
  if (bang == std::string_view::npos) return false;
  std::string sn(f.substr(0, bang));
  if (sn.size() >= 2 && sn.front() == '\'' && sn.back() == '\'') {
    std::string u;
    for (size_t k = 1; k + 1 < sn.size(); ++k) {
      u += sn[k];
      if (sn[k] == '\'' && k + 2 < sn.size() && sn[k + 1] == '\'') ++k;
    }
    sn = std::move(u);
  }
  Range r;
  const int xi = book.FindSheet(sn);
  if (xi < 0 || !ParseRange(f.substr(bang + 1), r) || (r.firstRow != r.lastRow && r.firstCol != r.lastCol) ||
      static_cast<uint64_t>(r.lastRow - r.firstRow) + (r.lastCol - r.firstCol) > 100000)
    return false;
  const Sheet& x = book.GetSheet(static_cast<size_t>(xi));
  if (!x.Loaded()) return false;
  for (uint32_t row = r.firstRow; row <= r.lastRow; ++row)
    for (uint32_t col = r.firstCol; col <= r.lastCol; ++col) {
      if (visibleOnly && (x.Row(row).hidden || x.Column(col).hidden)) continue;
      const Value v = x.GetValue(row, col);
      refs::CachePoint p;
      if (v.type == ValueType::Number) {
        char buf[48];
        p = {refs::CachePoint::Number, std::string(buf, xlsx::FormatNumber(v.number, 0, buf))};
      } else if (v.type == ValueType::String) {
        p = {refs::CachePoint::Text, std::string(v.text)};
      } else if (v.type == ValueType::Bool) {
        p = {refs::CachePoint::Text, v.boolean ? "TRUE" : "FALSE"};
      } else if (v.type == ValueType::Error) {
        p = {refs::CachePoint::Text, "#N/A"};
      }
      out.push_back(std::move(p));
    }
  return true;
}

bool RefreshCaches(const Workbook& book, std::string& xml) {
  return refs::RefreshChart(xml, [&](std::string_view f, bool visibleOnly, std::vector<refs::CachePoint>& out) {
    return CachePoints(book, f, visibleOnly, out);
  });
}

std::vector<ChartSeries> SeriesFor(const Sheet& s, const Range& data) {
  const Plan plan = PlanSeries(s, data);
  std::vector<ChartSeries> out;
  for (const Plan::One& one : plan.series)
    out.push_back({one.name ? AbsRef(s.Name(), *one.name) : std::string(), AbsRef(s.Name(), one.values),
                   plan.categories ? AbsRef(s.Name(), *plan.categories) : std::string()});
  return out;
}

ChartInfo Info(std::string_view xml) {
  ChartInfo info;
  pugi::xml_document d;
  if (!d.load_buffer(xml.data(), xml.size(), pugi::parse_default | pugi::parse_ws_pcdata_single)) return info;
  pugi::xml_node chart = Kid(d.document_element(), "chart"), plot = Kid(chart, "plotArea");
  info.kind = KindOf(plot);
  info.title = TitleOf(Kid(chart, "title"));
  const auto [x, y] = AxesOf(plot);
  info.xTitle = TitleOf(Kid(x, "title"));
  info.yTitle = TitleOf(Kid(y, "title"));
  if (pugi::xml_node legend = Kid(chart, "legend")) {
    const std::string_view pos = xlsx::Attr(Kid(legend, "legendPos"), "val");
    info.legend = pos.empty() || pos == "tr" ? 'r' : pos[0];
  }
  for (pugi::xml_node g : Groups(plot)) {
    info.labels = info.labels || xlsx::BoolAttr(Kid(Kid(g, "dLbls"), "showVal"), "val", false);
    for (pugi::xml_node s = g.first_child(); s; s = s.next_sibling())
      if (Is(s, {"ser"})) info.labels = info.labels || xlsx::BoolAttr(Kid(Kid(s, "dLbls"), "showVal"), "val", false);
  }
  for (pugi::xml_node s : SeriesOf(plot)) {
    ChartSeries cs;
    cs.name = FormulaIn(Kid(s, "tx"));
    cs.values = FormulaIn(Kid(s, "val") ? Kid(s, "val") : Kid(s, "yVal"));
    cs.categories = FormulaIn(Kid(s, "cat") ? Kid(s, "cat") : Kid(s, "xVal"));
    info.series.push_back(std::move(cs));
    info.points.push_back(static_cast<uint32_t>(PartsOf(s).points));
    pugi::xml_node tx = Kid(s, "tx");
    pugi::xml_node v = Kid(tx, "v") ? Kid(tx, "v") : Kid(Kid(Kid(Kid(tx, "strRef"), "strCache"), "pt"), "v");
    info.names.push_back(xlsx::TextOf(v));
  }
  return info;
}

std::string Edited(const Workbook& book, std::string_view xml, const ChartEdit& e) {
  pugi::xml_document d;
  constexpr unsigned kParse = pugi::parse_default | pugi::parse_ws_pcdata_single | pugi::parse_declaration | pugi::parse_comments;
  if (!d.load_buffer(xml.data(), xml.size(), kParse)) return {};
  pugi::xml_node root = d.document_element();
  pugi::xml_node chart = Kid(root, "chart"), plot = Kid(chart, "plotArea");
  // What's written here uses Excel's prefixes, c: and a:.
  if (!chart || !plot || std::string_view(root.name()) != "c:chartSpace") return {};
  constexpr std::string_view kMain = "http://schemas.openxmlformats.org/drawingml/2006/main";
  if (pugi::xml_attribute a = root.attribute("xmlns:a"); !a) root.append_attribute("xmlns:a").set_value(std::string(kMain).c_str());
  else if (a.value() != kMain) return {};
  auto strip = [](std::string f) {
    while (!f.empty() && (f.front() == '=' || f.front() == ' ')) f.erase(0, 1);
    return f;
  };
  // A new type or series list: the series written anew in the type's look, each keeping its color.
  if (e.kind || e.series) {
    const std::optional<ChartKind> now = KindOf(plot);
    if (!e.kind && !now) return {};
    const ChartKind kind = e.kind ? *e.kind : *now;
    std::vector<SeriesParts> parts;
    for (pugi::xml_node s : SeriesOf(plot)) parts.push_back(PartsOf(s));
    if (e.series) {
      std::vector<SeriesParts> next;
      for (size_t i = 0; i < e.series->size(); ++i) {
        const ChartSeries& cs = (*e.series)[i];
        SeriesParts p;
        if (i < parts.size()) p.color = parts[i].color;
        const std::string name = strip(cs.name);
        if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
          p.tx = "<c:tx><c:v>" + Esc(name.substr(1, name.size() - 2)) + "</c:v></c:tx>";
        } else if (!name.empty()) {
          std::vector<refs::CachePoint> pts;
          if (!CachePoints(book, name, false, pts)) return {};
          p.tx = "<c:tx><c:strRef><c:f>" + Esc(name) + "</c:f>" + CacheXml(pts, false) + "</c:strRef></c:tx>";
        }
        if (!RefXml(book, strip(cs.values), true, p.val, &p.points)) return {};
        if (!strip(cs.categories).empty() && !RefXml(book, strip(cs.categories), false, p.cat, nullptr)) return {};
        next.push_back(std::move(p));
      }
      parts = std::move(next);
    }
    bool labels = false;
    const std::vector<pugi::xml_node> groups = Groups(plot);
    for (pugi::xml_node g : groups) labels = labels || xlsx::BoolAttr(Kid(Kid(g, "dLbls"), "showVal"), "val", false);
    // The axes stay (with their look and titles) when both types have a category and a value axis.
    auto catVal = [](ChartKind k) { return k != ChartKind::Pie && k != ChartKind::Doughnut && k != ChartKind::Scatter; };
    const auto [x, y] = AxesOf(plot);
    const bool keep = now && catVal(*now) && catVal(kind) && x && y;
    std::string xId = "500000001", yId = "500000002";
    if (keep) {
      xId = std::string(xlsx::Attr(Kid(x, "axId"), "val"));
      yId = std::string(xlsx::Attr(Kid(y, "axId"), "val"));
      const bool bar = kind == ChartKind::Bar;
      SetVal(x, "axPos", bar ? "l" : "b");
      SetVal(y, "axPos", bar ? "b" : "l");
    }
    std::string sers;
    for (size_t i = 0; i < parts.size(); ++i) sers += SeriesXml(kind, i, parts[i]);
    pugi::xml_node at = groups.empty() ? FirstOf(plot, {"catAx", "valAx", "dateAx", "serAx", "dTable", "spPr", "extLst"}) : groups[0];
    if (!Put(plot, at, GroupXml(kind, sers, labels, xId, yId))) return {};
    for (pugi::xml_node g : groups) plot.remove_child(g);
    if (!keep) {
      std::vector<pugi::xml_node> axes;
      for (pugi::xml_node c = plot.first_child(); c; c = c.next_sibling())
        if (Is(c, {"catAx", "valAx", "dateAx", "serAx"})) axes.push_back(c);
      for (pugi::xml_node a : axes) plot.remove_child(a);
      if (const std::string ax = AxesXml(kind, xId, yId); !ax.empty()) Put(plot, FirstOf(plot, {"dTable", "spPr", "extLst"}), ax);
    }
  }
  if (e.title) SetTitle(chart, *e.title, true, 1400, "0");
  if (e.xTitle || e.yTitle) {
    const auto [x, y] = AxesOf(plot);
    auto rot = [](pugi::xml_node axis) {
      const std::string_view pos = xlsx::Attr(Kid(axis, "axPos"), "val");
      return pos == "l" || pos == "r" ? "-5400000" : "0";
    };
    if (e.xTitle && x) SetTitle(x, *e.xTitle, false, 1000, rot(x));
    if (e.yTitle && y) SetTitle(y, *e.yTitle, false, 1000, rot(y));
  }
  if (e.legend) {
    pugi::xml_node legend = Kid(chart, "legend");
    if (!*e.legend) {
      if (legend) chart.remove_child(legend);
    } else if (!legend) {
      Put(chart, plot.next_sibling(), LegendXml(*e.legend));
    } else {
      SetVal(legend, "legendPos", LegendPos(*e.legend), legend.first_child());
    }
  }
  if (e.labels) {
    const std::string v = *e.labels ? "1" : "0";
    for (pugi::xml_node g : Groups(plot)) {
      const bool round = Is(g, {"pieChart", "pie3DChart", "doughnutChart", "ofPieChart"});
      pugi::xml_node dl = Kid(g, "dLbls");
      if (dl && Kid(dl, "delete")) {
        Put(g, dl, LabelsXml(round, *e.labels));
        g.remove_child(dl);
      } else if (!dl) {
        pugi::xml_node last;
        for (pugi::xml_node c = g.first_child(); c; c = c.next_sibling())
          if (Is(c, {"ser"})) last = c;
        Put(g, last ? last.next_sibling() : FirstOf(g, {"gapWidth", "overlap", "marker", "axId", "firstSliceAng", "holeSize"}),
            LabelsXml(round, *e.labels));
      } else {
        SetVal(dl, "showVal", v, FirstOf(dl, {"showCatName", "showSerName", "showPercent", "showBubbleSize", "separator", "showLeaderLines", "extLst"}));
      }
      for (pugi::xml_node s = g.first_child(); s; s = s.next_sibling())
        if (pugi::xml_node sdl = Is(s, {"ser"}) ? Kid(s, "dLbls") : pugi::xml_node(); sdl && !Kid(sdl, "delete"))
          SetVal(sdl, "showVal", v, FirstOf(sdl, {"showCatName", "showSerName", "showPercent", "showBubbleSize", "separator", "showLeaderLines", "extLst"}));
    }
  }
  if (!e.colors.empty()) {
    const std::vector<pugi::xml_node> sers = SeriesOf(plot);
    for (const ChartColor& c : e.colors) {
      if (c.series >= sers.size()) return {};
      pugi::xml_node ser = sers[c.series], g = ser.parent();
      const std::string color = Rgb(c.rgb);
      if (c.point < 0) {
        const bool lines = Is(g, {"lineChart", "line3DChart", "radarChart", "scatterChart"});
        SetColor(ser, color, lines, {"idx", "order", "tx"});
        if (pugi::xml_node m = Kid(ser, "marker"); m && lines && xlsx::Attr(Kid(m, "symbol"), "val") != "none") {
          SetColor(m, color, false, {"symbol", "size"});
          SetColor(m, color, true, {"symbol", "size"});
        }
        continue;
      }
      pugi::xml_node pt;
      for (pugi::xml_node x = ser.first_child(); x && !pt; x = x.next_sibling())
        if (Is(x, {"dPt"}) && xlsx::Attr(Kid(x, "idx"), "val") == std::to_string(c.point)) pt = x;
      if (!pt) {
        const bool round = Is(g, {"pieChart", "pie3DChart", "doughnutChart", "ofPieChart"});
        pt = Put(ser, FirstOf(ser, {"dLbls", "trendline", "errBars", "cat", "val", "xVal", "yVal", "smooth", "shape", "bubbleSize", "extLst"}),
                 "<c:dPt><c:idx val=\"" + std::to_string(c.point) + "\"/>" + (round ? "<c:bubble3D val=\"0\"/>" : "") + "</c:dPt>");
      }
      SetColor(pt, color, false, {"idx", "invertIfNegative", "marker", "bubble3D", "explosion"});
    }
  }
  Writer w;
  d.save(w, "", pugi::format_raw | pugi::format_no_declaration);
  return std::move(w.out);
}

std::string ChartAnchor(CellAddr from, CellAddr to, const std::string& rel, uint32_t id) {
  auto corner = [](const char* el, CellAddr at) {
    return std::string("<xdr:") + el + "><xdr:col>" + std::to_string(at.col) + "</xdr:col><xdr:colOff>0</xdr:colOff><xdr:row>" +
           std::to_string(at.row) + "</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:" + el + ">";
  };
  return "<xdr:twoCellAnchor xmlns:xdr=\"http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing\" "
         "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\">" +
         corner("from", from) + corner("to", to) + "<xdr:graphicFrame macro=\"\"><xdr:nvGraphicFramePr><xdr:cNvPr id=\"" +
         std::to_string(id) + "\" name=\"Chart " + std::to_string(id - 1) +
         "\"/><xdr:cNvGraphicFramePr/></xdr:nvGraphicFramePr><xdr:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/></xdr:xfrm>"
         "<a:graphic><a:graphicData uri=\"" + std::string(kChartNs) + "\"><c:chart xmlns:c=\"" + std::string(kChartNs) +
         "\" xmlns:r=\"" + std::string(kRelNs) + "\" r:id=\"" + rel +
         "\"/></a:graphicData></a:graphic></xdr:graphicFrame><xdr:clientData/></xdr:twoCellAnchor>";
}

}  // namespace fw::sheet::edit::charts
