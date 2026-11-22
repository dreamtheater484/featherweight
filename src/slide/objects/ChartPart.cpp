#include "slide/objects/ChartPart.h"

#include <algorithm>
#include <charconv>
#include <span>

#include "draw/Xml.h"
#include "pkg/Zip.h"

namespace fw::slide::objects {

namespace {

using namespace draw::xml;

std::string Escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

std::string Num(double v) {
  char buf[40];
  auto [end, ec] = std::to_chars(buf, buf + sizeof buf, v);
  return ec == std::errc() ? std::string(buf, end) : "0";
}

double ValueAt(const ChartData& d, size_t s, size_t c) {
  return s < d.values.size() && c < d.values[s].size() ? d.values[s][c] : 0;
}

std::string Ref(int column, int row) { return "$" + ColumnName(column) + "$" + std::to_string(row); }

std::string SeriesName(const ChartData& d, size_t s) {
  return s < d.series.size() ? d.series[s] : "Series " + std::to_string(s + 1);
}

std::string StrRef(const std::string& formula, const std::vector<std::string>& values) {
  std::string x = "<c:strRef><c:f>" + Escape(formula) + "</c:f><c:strCache><c:ptCount val=\"" + std::to_string(values.size()) + "\"/>";
  for (size_t i = 0; i < values.size(); ++i)
    x += "<c:pt idx=\"" + std::to_string(i) + "\"><c:v>" + Escape(values[i]) + "</c:v></c:pt>";
  return x + "</c:strCache></c:strRef>";
}

std::string NumRef(const std::string& formula, const ChartData& d, size_t s) {
  const size_t n = d.categories.size();
  std::string x = "<c:numRef><c:f>" + Escape(formula) + "</c:f><c:numCache><c:formatCode>General</c:formatCode><c:ptCount val=\"" +
                  std::to_string(n) + "\"/>";
  for (size_t i = 0; i < n; ++i) x += "<c:pt idx=\"" + std::to_string(i) + "\"><c:v>" + Num(ValueAt(d, s, i)) + "</c:v></c:pt>";
  return x + "</c:numCache></c:numRef>";
}

std::string TxXml(const ChartData& d, size_t s) {
  return "<c:tx>" + StrRef("Sheet1!" + Ref(static_cast<int>(s) + 1, 1), {SeriesName(d, s)}) + "</c:tx>";
}
std::string CatXml(const ChartData& d) {
  const int last = static_cast<int>(d.categories.size()) + 1;
  return "<c:cat>" + StrRef("Sheet1!" + Ref(0, 2) + ":" + Ref(0, std::max(2, last)), d.categories) + "</c:cat>";
}
std::string ValXml(const ChartData& d, size_t s) {
  const int last = static_cast<int>(d.categories.size()) + 1, col = static_cast<int>(s) + 1;
  return "<c:val>" + NumRef("Sheet1!" + Ref(col, 2) + ":" + Ref(col, std::max(2, last)), d, s) + "</c:val>";
}

std::string Accent(size_t s) { return "accent" + std::to_string(s % 6 + 1); }

pugi::xml_node Kid(pugi::xml_node parent, std::string_view local) {
  for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && LocalNameOf(c) == local && NamespaceOf(c) == kChart) return c;
  return {};
}

std::vector<pugi::xml_node> KidsNamed(pugi::xml_node parent, std::string_view local) {
  std::vector<pugi::xml_node> out;
  for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && LocalNameOf(c) == local && NamespaceOf(c) == kChart) out.push_back(c);
  return out;
}

// The first plot with categories and values (bar, line, pie, area, doughnut, radar).
pugi::xml_node FirstPlot(pugi::xml_node chartSpace) {
  pugi::xml_node plotArea = Kid(Kid(chartSpace, "chart"), "plotArea");
  for (pugi::xml_node c = plotArea.first_child(); c; c = c.next_sibling()) {
    const std::string_view n = LocalNameOf(c);
    for (std::string_view k : {"barChart", "bar3DChart", "lineChart", "line3DChart", "pieChart", "pie3DChart",
                               "doughnutChart", "areaChart", "area3DChart", "radarChart"})
      if (n == k && NamespaceOf(c) == kChart) return c;
  }
  return {};
}

std::vector<std::string> CacheStrings(pugi::xml_node ref) {
  std::vector<std::string> out;
  pugi::xml_node cache = Kid(ref, "strCache");
  if (!cache) cache = Kid(ref, "numCache");
  if (!cache) {  // a literal
    cache = Kid(ref.parent(), "strLit");
    if (!cache) cache = Kid(ref.parent(), "numLit");
  }
  const size_t n = std::min<size_t>(static_cast<size_t>(Number(Kid(cache, "ptCount"), "val", 0)), 4096);
  out.resize(n);
  for (pugi::xml_node pt : KidsNamed(cache, "pt")) {
    const size_t i = static_cast<size_t>(Number(pt, "idx", -1));
    if (i < n) out[i] = Kid(pt, "v").text().get();
  }
  return out;
}

// Replaces `parent`'s child `local` with the fragment, or puts it before the first of `before`.
void Put(pugi::xml_node parent, std::string_view local, const std::string& xml, std::initializer_list<std::string_view> before) {
  const std::string doc = "<c:root xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\">" + xml + "</c:root>";
  pugi::xml_document d;
  if (!d.load_buffer(doc.data(), doc.size())) return;
  pugi::xml_node fresh = d.document_element().first_child();
  // The chart's own prefix for its namespace, when it isn't c:.
  auto chartPrefix = [&]() -> std::string {
    for (pugi::xml_node n = parent; n; n = n.parent())
      for (pugi::xml_attribute a : n.attributes()) {
        const std::string_view name = a.name();
        if (kChart == a.value() && name.substr(0, 5) == "xmlns") return name.size() > 6 ? std::string(name.substr(6)) : std::string();
      }
    return "c";
  };
  const std::string prefix = chartPrefix();
  if (prefix != "c") {
    std::vector<pugi::xml_node> stack{fresh};
    while (!stack.empty()) {
      pugi::xml_node n = stack.back();
      stack.pop_back();
      const std::string_view name = n.name();
      if (name.substr(0, 2) == "c:") n.set_name((prefix.empty() ? std::string(name.substr(2)) : prefix + ":" + std::string(name.substr(2))).c_str());
      for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
        if (c.type() == pugi::node_element) stack.push_back(c);
    }
  }
  if (pugi::xml_node old = Kid(parent, local)) {
    parent.insert_copy_before(fresh, old);
    parent.remove_child(old);
    return;
  }
  for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling())
    for (std::string_view b : before)
      if (c.type() == pugi::node_element && LocalNameOf(c) == b) {
        parent.insert_copy_before(fresh, c);
        return;
      }
  parent.append_copy(fresh);
}

}  // namespace

std::string ColumnName(int column) {
  std::string out;
  for (int c = column + 1; c > 0; c = (c - 1) / 26) out.insert(out.begin(), static_cast<char>('A' + (c - 1) % 26));
  return out;
}

ChartData DefaultChartData() {
  ChartData d;
  d.categories = {"Category 1", "Category 2", "Category 3", "Category 4"};
  d.series = {"Series 1", "Series 2", "Series 3"};
  d.values = {{4.3, 2.5, 3.5, 4.5}, {2.4, 4.4, 1.8, 2.8}, {2, 2, 3, 5}};
  return d;
}

std::string ChartXml(ChartType type, const ChartData& d, const std::string& workbookRelId) {
  const bool pie = type == ChartType::Pie;
  const size_t seriesCount = pie ? std::min<size_t>(1, d.series.size()) : d.series.size();
  std::string x =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<c:chartSpace "
      "xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\" "
      "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
      "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><c:date1904 val=\"0\"/><c:lang "
      "val=\"en-US\"/><c:roundedCorners val=\"0\"/><c:chart><c:autoTitleDeleted val=\"" +
      std::string(pie ? "0" : "1") + "\"/><c:plotArea><c:layout/>";
  switch (type) {
    case ChartType::Column:
    case ChartType::Bar:
      x += std::string("<c:barChart><c:barDir val=\"") + (type == ChartType::Bar ? "bar" : "col") +
           "\"/><c:grouping val=\"clustered\"/><c:varyColors val=\"0\"/>";
      break;
    case ChartType::Line: x += "<c:lineChart><c:grouping val=\"standard\"/><c:varyColors val=\"0\"/>"; break;
    case ChartType::Pie: x += "<c:pieChart><c:varyColors val=\"1\"/>"; break;
  }
  for (size_t s = 0; s < seriesCount; ++s) {
    x += "<c:ser><c:idx val=\"" + std::to_string(s) + "\"/><c:order val=\"" + std::to_string(s) + "\"/>" + TxXml(d, s);
    switch (type) {
      case ChartType::Column:
      case ChartType::Bar:
        x += "<c:spPr><a:solidFill><a:schemeClr val=\"" + Accent(s) +
             "\"/></a:solidFill><a:ln><a:noFill/></a:ln></c:spPr><c:invertIfNegative val=\"0\"/>" + CatXml(d) + ValXml(d, s);
        break;
      case ChartType::Line:
        x += "<c:spPr><a:ln w=\"28575\" cap=\"rnd\"><a:solidFill><a:schemeClr val=\"" + Accent(s) +
             "\"/></a:solidFill><a:round/></a:ln></c:spPr><c:marker><c:symbol val=\"none\"/></c:marker>" + CatXml(d) +
             ValXml(d, s) + "<c:smooth val=\"0\"/>";
        break;
      case ChartType::Pie:
        for (size_t c = 0; c < d.categories.size(); ++c)
          x += "<c:dPt><c:idx val=\"" + std::to_string(c) + "\"/><c:bubble3D val=\"0\"/><c:spPr><a:solidFill><a:schemeClr val=\"" +
               Accent(c) + "\"/></a:solidFill><a:ln w=\"19050\"><a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill></a:ln></c:spPr></c:dPt>";
        x += CatXml(d) + ValXml(d, s);
        break;
    }
    x += "</c:ser>";
  }
  switch (type) {
    case ChartType::Column:
    case ChartType::Bar:
      x += "<c:gapWidth val=\"219\"/><c:overlap val=\"-27\"/><c:axId val=\"500000001\"/><c:axId val=\"500000002\"/></c:barChart>";
      break;
    case ChartType::Line:
      x += "<c:marker val=\"1\"/><c:axId val=\"500000001\"/><c:axId val=\"500000002\"/></c:lineChart>";
      break;
    case ChartType::Pie: x += "<c:firstSliceAng val=\"0\"/></c:pieChart>"; break;
  }
  if (!pie) {
    const bool bar = type == ChartType::Bar;
    x += std::string("<c:catAx><c:axId val=\"500000001\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling><c:delete "
                     "val=\"0\"/><c:axPos val=\"") + (bar ? "l" : "b") +
         "\"/><c:numFmt formatCode=\"General\" sourceLinked=\"1\"/><c:majorTickMark val=\"none\"/><c:minorTickMark "
         "val=\"none\"/><c:tickLblPos val=\"nextTo\"/><c:crossAx val=\"500000002\"/><c:crosses val=\"autoZero\"/><c:auto "
         "val=\"1\"/><c:lblAlgn val=\"ctr\"/><c:lblOffset val=\"100\"/><c:noMultiLvlLbl val=\"0\"/></c:catAx>";
    x += std::string("<c:valAx><c:axId val=\"500000002\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling><c:delete "
                     "val=\"0\"/><c:axPos val=\"") + (bar ? "b" : "l") +
         "\"/><c:majorGridlines><c:spPr><a:ln w=\"9525\"><a:solidFill><a:schemeClr val=\"tx1\"><a:lumMod val=\"15000\"/><a:lumOff "
         "val=\"85000\"/></a:schemeClr></a:solidFill></a:ln></c:spPr></c:majorGridlines><c:numFmt formatCode=\"General\" "
         "sourceLinked=\"1\"/><c:majorTickMark val=\"none\"/><c:minorTickMark val=\"none\"/><c:tickLblPos "
         "val=\"nextTo\"/><c:spPr><a:ln><a:noFill/></a:ln></c:spPr><c:crossAx val=\"500000001\"/><c:crosses "
         "val=\"autoZero\"/><c:crossBetween val=\"between\"/></c:valAx>";
  }
  // The plot area and the chart area without fill or line, as PowerPoint 2021 inserts a chart: the
  // slide shows through (readers fill a chart area without c:spPr white).
  constexpr const char* kClear = "<c:spPr><a:noFill/><a:ln><a:noFill/></a:ln><a:effectLst/></c:spPr>";
  x += std::string(kClear) +
       "</c:plotArea><c:legend><c:legendPos val=\"b\"/><c:overlay val=\"0\"/></c:legend><c:plotVisOnly val=\"1\"/><c:dispBlanksAs "
       "val=\"gap\"/></c:chart>" + kClear + "<c:txPr><a:bodyPr/><a:lstStyle/><a:p><a:pPr><a:defRPr sz=\"1800\"/></a:pPr><a:endParaRPr "
       "lang=\"en-US\"/></a:p></c:txPr><c:externalData r:id=\"" +
       Escape(workbookRelId) + "\"><c:autoUpdate val=\"0\"/></c:externalData></c:chartSpace>";
  return x;
}

std::vector<uint8_t> ChartWorkbook(const ChartData& d) {
  const std::string types =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<Types "
      "xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Default Extension=\"rels\" "
      "ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" "
      "ContentType=\"application/xml\"/><Override PartName=\"/xl/workbook.xml\" "
      "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/><Override "
      "PartName=\"/xl/worksheets/sheet1.xml\" "
      "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/></Types>";
  const std::string rels =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<Relationships "
      "xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" "
      "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
      "Target=\"xl/workbook.xml\"/></Relationships>";
  const std::string workbook =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<workbook "
      "xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
      "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets><sheet name=\"Sheet1\" "
      "sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>";
  const std::string workbookRels =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<Relationships "
      "xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" "
      "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" "
      "Target=\"worksheets/sheet1.xml\"/></Relationships>";
  std::string sheet =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<worksheet "
      "xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>";
  auto text = [](int col, int row, const std::string& v) {
    return "<c r=\"" + ColumnName(col) + std::to_string(row) + "\" t=\"inlineStr\"><is><t>" + Escape(v) + "</t></is></c>";
  };
  sheet += "<row r=\"1\">";
  for (size_t s = 0; s < d.series.size(); ++s) sheet += text(static_cast<int>(s) + 1, 1, d.series[s]);
  sheet += "</row>";
  for (size_t c = 0; c < d.categories.size(); ++c) {
    const int row = static_cast<int>(c) + 2;
    sheet += "<row r=\"" + std::to_string(row) + "\">" + text(0, row, d.categories[c]);
    for (size_t s = 0; s < d.series.size(); ++s)
      sheet += "<c r=\"" + ColumnName(static_cast<int>(s) + 1) + std::to_string(row) + "\"><v>" + Num(ValueAt(d, s, c)) + "</v></c>";
    sheet += "</row>";
  }
  sheet += "</sheetData></worksheet>";
  pkg::MemorySink sink;
  pkg::ZipWriter zip(sink);
  auto add = [&](const char* name, const std::string& s) {
    zip.Add(name, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
  };
  add("[Content_Types].xml", types);
  add("_rels/.rels", rels);
  add("xl/workbook.xml", workbook);
  add("xl/_rels/workbook.xml.rels", workbookRels);
  add("xl/worksheets/sheet1.xml", sheet);
  zip.Finish();
  return std::move(sink.bytes);
}

bool ReadChartData(pugi::xml_node chartSpace, ChartData& out) {
  out = ChartData();
  pugi::xml_node plot = FirstPlot(chartSpace);
  const std::vector<pugi::xml_node> sers = KidsNamed(plot, "ser");
  if (sers.empty()) return false;
  for (pugi::xml_node ser : sers) {
    pugi::xml_node tx = Kid(ser, "tx");
    std::string name;
    if (pugi::xml_node ref = Kid(tx, "strRef")) {
      const auto v = CacheStrings(ref);
      if (!v.empty()) name = v[0];
    } else if (pugi::xml_node v = Kid(tx, "v")) {
      name = v.text().get();
    }
    out.series.push_back(name);
    if (out.categories.empty()) {
      pugi::xml_node cat = Kid(ser, "cat");
      pugi::xml_node ref = Kid(cat, "strRef");
      if (!ref) ref = Kid(cat, "numRef");
      out.categories = CacheStrings(ref ? ref : Kid(cat, "strLit"));
    }
    pugi::xml_node val = Kid(ser, "val");
    pugi::xml_node ref = Kid(val, "numRef");
    std::vector<std::string> v = CacheStrings(ref ? ref : Kid(val, "numLit"));
    std::vector<double> nums;
    for (const std::string& s : v) nums.push_back(Number(s, 0));
    out.values.push_back(std::move(nums));
  }
  size_t n = out.categories.size();
  for (const auto& v : out.values) n = std::max(n, v.size());
  for (size_t i = out.categories.size(); i < n; ++i) out.categories.push_back(std::to_string(i + 1));
  for (auto& v : out.values) v.resize(n);
  return true;
}

bool WriteChartData(pugi::xml_node chartSpace, const ChartData& d) {
  pugi::xml_node plot = FirstPlot(chartSpace);
  std::vector<pugi::xml_node> sers = KidsNamed(plot, "ser");
  if (!plot || sers.empty() || d.series.empty()) return false;
  const bool single = LocalNameOf(plot).substr(0, 3) == "pie";
  const size_t want = single ? 1 : d.series.size();
  int next = 0;
  for (pugi::xml_node s : sers) next = std::max(next, static_cast<int>(Number(Kid(s, "idx"), "val", 0)) + 1);
  while (sers.size() < want) {
    pugi::xml_node copy = plot.insert_copy_after(sers.back(), sers.back());
    for (const char* k : {"idx", "order"})
      if (pugi::xml_node e = Kid(copy, k)) e.attribute("val").set_value(next);
    ++next;
    sers.push_back(copy);
  }
  while (sers.size() > want) {
    plot.remove_child(sers.back());
    sers.pop_back();
  }
  for (size_t s = 0; s < sers.size(); ++s) {
    Put(sers[s], "tx", TxXml(d, s),
        {"spPr", "invertIfNegative", "pictureOptions", "marker", "explosion", "dPt", "dLbls", "trendline", "errBars",
         "cat", "val", "smooth", "shape", "extLst"});
    Put(sers[s], "cat", CatXml(d), {"val", "smooth", "shape", "extLst"});
    Put(sers[s], "val", ValXml(d, s), {"smooth", "shape", "extLst"});
  }
  return true;
}

}  // namespace fw::slide::objects
