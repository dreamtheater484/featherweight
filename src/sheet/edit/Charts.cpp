#include "sheet/edit/Charts.h"

#include <optional>
#include <string_view>
#include <vector>

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
// The n-th series' (or pie slice's) color: accent1 to 6, then Excel's darker and lighter rounds.
std::string Fill(size_t n) {
  static const char* const kRounds[] = {"", "<a:lumMod val=\"60000\"/>", "<a:lumMod val=\"80000\"/><a:lumOff val=\"20000\"/>",
                                        "<a:lumMod val=\"80000\"/>", "<a:lumMod val=\"60000\"/><a:lumOff val=\"40000\"/>",
                                        "<a:lumMod val=\"50000\"/>"};
  const std::string accent = "<a:schemeClr val=\"accent" + std::to_string(n % 6 + 1) + "\"";
  const std::string mods = kRounds[(n / 6) % 6];
  return "<a:solidFill>" + (mods.empty() ? accent + "/>" : accent + ">" + mods + "</a:schemeClr>") + "</a:solidFill>";
}

std::string ChartXml(const Sheet& s, const Plan& plan, ChartKind kind, const std::string& title) {
  using K = ChartKind;
  const std::string name = s.Name();
  const bool round = kind == K::Pie || kind == K::Doughnut;
  const bool scatter = kind == K::Scatter;
  std::string sers;
  for (size_t i = 0; i < plan.series.size(); ++i) {
    const Plan::One& one = plan.series[i];
    sers += "<c:ser><c:idx val=\"" + std::to_string(i) + "\"/><c:order val=\"" + std::to_string(i) + "\"/>";
    if (one.name)
      sers += "<c:tx><c:strRef><c:f>" + Esc(AbsRef(name, *one.name)) + "</c:f>" + Cache(s, *one.name, false) + "</c:strRef></c:tx>";
    switch (kind) {
      case K::Column:
      case K::Bar: sers += "<c:spPr>" + Fill(i) + "<a:ln><a:noFill/></a:ln><a:effectLst/></c:spPr><c:invertIfNegative val=\"0\"/>"; break;
      case K::Area: sers += "<c:spPr>" + Fill(i) + "<a:ln><a:noFill/></a:ln><a:effectLst/></c:spPr>"; break;
      case K::Line:
        sers += "<c:spPr><a:ln w=\"28575\" cap=\"rnd\">" + Fill(i) + "<a:round/></a:ln><a:effectLst/></c:spPr><c:marker><c:symbol val=\"none\"/></c:marker>";
        break;
      case K::Scatter:
        sers += "<c:spPr><a:ln w=\"25400\" cap=\"rnd\"><a:noFill/><a:round/></a:ln><a:effectLst/></c:spPr><c:marker><c:symbol val=\"circle\"/>"
                "<c:size val=\"5\"/><c:spPr>" + Fill(i) + "<a:ln w=\"9525\">" + Fill(i) + "</a:ln><a:effectLst/></c:spPr></c:marker>";
        break;
      case K::Pie:
      case K::Doughnut: {
        // A slice per value, each in its color with a white edge.
        const uint64_t n = static_cast<uint64_t>(one.values.lastRow - one.values.firstRow + 1) * (one.values.lastCol - one.values.firstCol + 1);
        for (uint64_t k = 0; k < n && k < 255; ++k)
          sers += "<c:dPt><c:idx val=\"" + std::to_string(k) + "\"/><c:bubble3D val=\"0\"/><c:spPr>" + Fill(k) +
                  "<a:ln w=\"19050\"><a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill></a:ln><a:effectLst/></c:spPr></c:dPt>";
        break;
      }
    }
    if (plan.categories) {
      const bool nums = AllNumbers(s, *plan.categories);
      const std::string ref = nums ? "numRef" : "strRef";
      sers += std::string(scatter ? "<c:xVal>" : "<c:cat>") + "<c:" + ref + "><c:f>" + Esc(AbsRef(name, *plan.categories)) + "</c:f>" +
              Cache(s, *plan.categories, nums) + "</c:" + ref + ">" + (scatter ? "</c:xVal>" : "</c:cat>");
    }
    sers += std::string(scatter ? "<c:yVal>" : "<c:val>") + "<c:numRef><c:f>" + Esc(AbsRef(name, one.values)) + "</c:f>" +
            Cache(s, one.values, true) + "</c:numRef>" + (scatter ? "</c:yVal>" : "</c:val>");
    if (kind == K::Line || scatter) sers += "<c:smooth val=\"0\"/>";
    sers += "</c:ser>";
  }
  const std::string labels = std::string("<c:dLbls><c:showLegendKey val=\"0\"/><c:showVal val=\"0\"/><c:showCatName val=\"0\"/>"
                                         "<c:showSerName val=\"0\"/><c:showPercent val=\"0\"/><c:showBubbleSize val=\"0\"/>") +
                             (round ? "<c:showLeaderLines val=\"1\"/>" : "") + "</c:dLbls>";
  const std::string axIds = "<c:axId val=\"500000001\"/><c:axId val=\"500000002\"/>";
  std::string group;
  switch (kind) {
    case K::Column:
    case K::Bar:
      group = std::string("<c:barChart><c:barDir val=\"") + (kind == K::Bar ? "bar" : "col") +
              "\"/><c:grouping val=\"clustered\"/><c:varyColors val=\"0\"/>" + sers + labels +
              "<c:gapWidth val=\"219\"/><c:overlap val=\"-27\"/>" + axIds + "</c:barChart>";
      break;
    case K::Line:
      group = "<c:lineChart><c:grouping val=\"standard\"/><c:varyColors val=\"0\"/>" + sers + labels + "<c:marker val=\"1\"/>" + axIds + "</c:lineChart>";
      break;
    case K::Area:
      group = "<c:areaChart><c:grouping val=\"standard\"/><c:varyColors val=\"0\"/>" + sers + labels + axIds + "</c:areaChart>";
      break;
    case K::Pie:
      group = "<c:pieChart><c:varyColors val=\"1\"/>" + sers + labels + "<c:firstSliceAng val=\"0\"/></c:pieChart>";
      break;
    case K::Doughnut:
      group = "<c:doughnutChart><c:varyColors val=\"1\"/>" + sers + labels + "<c:firstSliceAng val=\"0\"/><c:holeSize val=\"75\"/></c:doughnutChart>";
      break;
    case K::Scatter:
      group = "<c:scatterChart><c:scatterStyle val=\"lineMarker\"/><c:varyColors val=\"0\"/>" + sers + labels + axIds + "</c:scatterChart>";
      break;
  }
  // An axis: its gridlines and its line in the light gray, its labels in 9 points.
  auto axis = [](const char* el, const char* id, const char* cross, const char* pos, bool grid, bool line, const char* tail) {
    return std::string("<c:") + el + "><c:axId val=\"" + id + "\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling>"
           "<c:delete val=\"0\"/><c:axPos val=\"" + pos + "\"/>" +
           (grid ? "<c:majorGridlines><c:spPr>" + Line15() + "<a:effectLst/></c:spPr></c:majorGridlines>" : std::string()) +
           "<c:numFmt formatCode=\"General\" sourceLinked=\"1\"/><c:majorTickMark val=\"none\"/><c:minorTickMark val=\"none\"/>"
           "<c:tickLblPos val=\"nextTo\"/>" + (line ? "<c:spPr><a:noFill/>" + Line15() + "<a:effectLst/></c:spPr>" : std::string(kNoShape)) +
           TxPr(900, "-60000000") + "<c:crossAx val=\"" + cross + "\"/><c:crosses val=\"autoZero\"/>" + tail + "</c:" + el + ">";
  };
  std::string axes;
  if (scatter) {
    axes = axis("valAx", "500000001", "500000002", "b", true, false, "<c:crossBetween val=\"midCat\"/>") +
           axis("valAx", "500000002", "500000001", "l", true, false, "<c:crossBetween val=\"midCat\"/>");
  } else if (!round) {
    const bool bar = kind == K::Bar;
    axes = axis("catAx", "500000001", "500000002", bar ? "l" : "b", false, true,
                "<c:auto val=\"1\"/><c:lblAlgn val=\"ctr\"/><c:lblOffset val=\"100\"/><c:noMultiLvlLbl val=\"0\"/>") +
           axis("valAx", "500000002", "500000001", bar ? "b" : "l", true, false, "<c:crossBetween val=\"between\"/>");
  }
  // The title: the text given, or Excel's own (the series' name for one, else "Chart Title").
  const std::string tx = title.empty() ? std::string()
                                       : "<c:tx><c:rich>" + BodyPr("0") + "<a:p><a:pPr>" + DefRPr(1400) +
                                             "</a:pPr><a:r><a:rPr lang=\"en-US\"/><a:t>" + Esc(title) + "</a:t></a:r></a:p></c:rich></c:tx>";
  const std::string head = "<c:title>" + tx + "<c:overlay val=\"0\"/>" + std::string(kNoShape) + TxPr(1400, "0") +
                           "</c:title><c:autoTitleDeleted val=\"0\"/>";
  const std::string legend = plan.series.size() > 1 || round ? "<c:legend><c:legendPos val=\"b\"/><c:overlay val=\"0\"/>" +
                                                                   std::string(kNoShape) + TxPr(900, "0") + "</c:legend>"
                                                             : std::string();
  return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<c:chartSpace xmlns:c=\"" + std::string(kChartNs) +
         "\" xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" xmlns:r=\"" + std::string(kRelNs) +
         "\"><c:date1904 val=\"0\"/><c:roundedCorners val=\"0\"/><c:chart>" + head + "<c:plotArea><c:layout/>" + group + axes +
         std::string(kNoShape) + "</c:plotArea>" + legend + "<c:plotVisOnly val=\"1\"/><c:dispBlanksAs val=\"gap\"/></c:chart>" +
         "<c:spPr><a:solidFill><a:schemeClr val=\"bg1\"/></a:solidFill>" + Line15() + "<a:effectLst/></c:spPr>"
         "<c:txPr><a:bodyPr/><a:lstStyle/><a:p><a:pPr><a:defRPr/></a:pPr><a:endParaRPr lang=\"en-US\"/></a:p></c:txPr></c:chartSpace>";
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
