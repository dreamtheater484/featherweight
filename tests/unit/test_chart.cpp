// fwchart (D5): charts read from their cached values, drawn as art, shown in Word drawings, and
// their parts kept as they were when the document is saved.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "PkgTestUtil.h"
#include "TestHarness.h"
#include "chart/Chart.h"
#include "doc/docx/Docx.h"
#include "doc/docx/Save.h"
#include "doc/edit/Editor.h"
#include "draw/Drawing.h"
#include "draw/Theme.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/XmlDom.h"

// Stops the test when a condition it needs doesn't hold.
#define REQUIRE(cond)                                                \
  do {                                                               \
    const bool required_ = static_cast<bool>(cond);                  \
    if (!required_) return test::Fail(__FILE__, __LINE__, #cond);    \
  } while (0)

namespace {

using namespace fw::chart;

const char* kNs =
    " xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\""
    " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
    " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"";

std::string Cache(const char* kind, const std::vector<std::string>& pts) {
  std::string s = std::string("<c:") + kind + "><c:ptCount val=\"" + std::to_string(pts.size()) + "\"/>";
  for (size_t i = 0; i < pts.size(); ++i) s += "<c:pt idx=\"" + std::to_string(i) + "\"><c:v>" + pts[i] + "</c:v></c:pt>";
  return s + "</c:" + kind + ">";
}

std::string Ser(int i, const std::string& name, const std::vector<std::string>& cats, const std::vector<std::string>& vals,
                const char* catTag = "cat", const char* valTag = "val") {
  return "<c:ser><c:idx val=\"" + std::to_string(i) + "\"/><c:order val=\"" + std::to_string(i) +
         "\"/><c:tx><c:strRef><c:f>Sheet1!$B$1</c:f>" + Cache("strCache", {name}) + "</c:strRef></c:tx><c:" + catTag +
         "><c:strRef><c:f>Sheet1!$A$2:$A$4</c:f>" + Cache("strCache", cats) + "</c:strRef></c:" + catTag + "><c:" + valTag +
         "><c:numRef><c:f>Sheet1!$B$2:$B$4</c:f>" + Cache("numCache", vals) + "</c:numRef></c:" + valTag + "></c:ser>";
}

const char* kAxes =
    "<c:axId val=\"1\"/><c:axId val=\"2\"/></c:barChart><c:catAx><c:axId val=\"1\"/><c:axPos val=\"b\"/>"
    "<c:crossAx val=\"2\"/></c:catAx><c:valAx><c:axId val=\"2\"/><c:axPos val=\"l\"/><c:majorGridlines/>"
    "<c:numFmt formatCode=\"General\" sourceLinked=\"1\"/><c:crossAx val=\"1\"/><c:crossBetween val=\"between\"/></c:valAx>";

std::string ColumnChart() {
  return std::string("<c:chartSpace") + kNs +
         "><c:chart><c:title><c:tx><c:rich><a:bodyPr/><a:p><a:r><a:t>Sales</a:t></a:r></a:p></c:rich></c:tx></c:title>"
         "<c:autoTitleDeleted val=\"0\"/><c:plotArea><c:barChart><c:barDir val=\"col\"/><c:grouping val=\"clustered\"/>"
         "<c:varyColors val=\"0\"/>" +
         Ser(0, "North", {"Q1", "Q2", "Q3"}, {"4.3", "2.5", "3.5"}) + Ser(1, "South", {"Q1", "Q2", "Q3"}, {"2.4", "4.4", "1.8"}) +
         "<c:gapWidth val=\"219\"/>" + kAxes + "</c:plotArea><c:legend><c:legendPos val=\"b\"/></c:legend></c:chart></c:chartSpace>";
}

std::string PieChart() {
  return std::string("<c:chartSpace") + kNs + "><c:chart><c:plotArea><c:pieChart><c:varyColors val=\"1\"/>" +
         Ser(0, "Share", {"A", "B", "C", "D"}, {"8.2", "3.2", "1.4", "1.2"}) +
         "<c:dLbls><c:showPercent val=\"1\"/></c:dLbls><c:firstSliceAng val=\"0\"/></c:pieChart></c:plotArea>"
         "<c:legend><c:legendPos val=\"r\"/></c:legend></c:chart></c:chartSpace>";
}

struct Parsed {
  fw::pkg::XmlDom dom;
  explicit Parsed(const std::string& xml) { (void)dom.Parse(xml, fw::pkg::Limits{}); }
  pugi::xml_node Root() { return dom.Root(); }
};

draw::ColorContext Colors() {
  static std::shared_ptr<const draw::Theme> theme = draw::Theme::Default();
  static draw::ColorMap map;
  draw::ColorContext c;
  c.theme = theme.get();
  c.colorMap = &map;
  return c;
}

bool Same(const draw::Color& a, const draw::Color& b) {
  return std::abs(a.r - b.r) < 0.002f && std::abs(a.g - b.g) < 0.002f && std::abs(a.b - b.b) < 0.002f;
}

// The shapes filled in `c`: each mark's figures (a series' bars are one mark, a figure a bar).
size_t Filled(const draw::Art& art, const draw::Color& c) {
  size_t n = 0;
  for (const auto& m : art.marks)
    if (m.fill.kind == draw::Fill::Kind::Solid && Same(m.fill.color, c))
      for (const draw::Path::Segment& s : m.path.Segments()) n += s.kind == draw::Path::Kind::Move;
  return n;
}

}  // namespace

TEST(Chart_ReadsAColumnChartsCachedValues) {
  Parsed p(ColumnChart());
  Chart c;
  REQUIRE(Read(p.Root(), Colors(), c));
  CHECK(c.title == L"Sales" && c.legend == LegendPos::Bottom);
  REQUIRE(c.groups.size() == 1u);
  const Group& g = c.groups[0];
  CHECK(g.type == Type::Column && g.grouping == Grouping::Clustered && g.gap == 219);
  REQUIRE(g.series.size() == 2u);
  CHECK(g.series[0].name == L"North" && g.series[1].name == L"South");
  CHECK(g.series[0].categories == (std::vector<std::wstring>{L"Q1", L"Q2", L"Q3"}));
  CHECK(g.series[1].values == (std::vector<double>{2.4, 4.4, 1.8}));
  CHECK(c.value.gridlines && c.category.shown);
}

TEST(Chart_DrawsABarForEachValueAndALegend) {
  Parsed p(ColumnChart());
  Chart c;
  REQUIRE(Read(p.Root(), Colors(), c));
  const draw::Art art = Draw(c, {360, 216}, Colors(), L"Calibri");
  const draw::Color first = *draw::SchemeColor("accent1", Colors()), second = *draw::SchemeColor("accent2", Colors());
  CHECK_EQ(Filled(art, first), 4u);  // three bars and the legend's swatch
  CHECK_EQ(Filled(art, second), 4u);
  for (const auto& m : art.marks) {
    const draw::Rect b = m.path.Bounds();
    CHECK(std::isfinite(b.left) && std::isfinite(b.bottom));
  }
}

TEST(Chart_PieSlicesAndPercentLabels) {
  Parsed p(PieChart());
  Chart c;
  REQUIRE(Read(p.Root(), Colors(), c));
  REQUIRE(c.groups.size() == 1u);
  CHECK(c.groups[0].type == Type::Pie && c.groups[0].varyColors && c.groups[0].percent);
  const draw::Art art = Draw(c, {300, 200}, Colors(), L"Calibri");
  // Office shades a slice's accent (76 %), with no outline between slices (Excel's PDFs).
  for (const char* accent : {"accent1", "accent2", "accent3", "accent4"}) {
    draw::Color slice = *draw::SchemeColor(accent, Colors());
    draw::ApplyColorTransform(slice, "shade", 76000);
    CHECK_EQ(Filled(art, slice), 2u);  // a slice and its legend swatch
  }
  for (const auto& m : art.marks) CHECK(m.fill.kind != draw::Fill::Kind::Solid || m.line.fill.kind == draw::Fill::Kind::None || m.path.Bounds().right - m.path.Bounds().left > 290);
}

TEST(Chart_WhatAChartDoesntSayIsAsOffice2007DrawsIt) {
  // A chart without c:spPr or c:txPr: 10-point text and a gray border. Office 2010's look
  // (c14:style, which LibreOffice and Office 2010 and later write) draws the gridlines, axis lines
  // and ticks gray too (Word's PDFs of FW_CORPUS features); without c14:style Office keeps 2007's
  // look, where they're black (Excel's PDFs).
  const auto strokes = [](const draw::Art& art, uint32_t rgb) {
    size_t n = 0;
    for (const auto& m : art.marks) n += m.line.fill.kind == draw::Fill::Kind::Solid && Same(m.line.fill.color, draw::Rgb(rgb));
    return n;
  };
  std::string modern = ColumnChart();
  modern.insert(modern.find("<c:chart>"),
                "<mc:AlternateContent xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\"><mc:Choice"
                " Requires=\"c14\" xmlns:c14=\"http://schemas.microsoft.com/office/drawing/2007/8/2/chart\"><c14:style"
                " val=\"102\"/></mc:Choice><mc:Fallback><c:style val=\"2\"/></mc:Fallback></mc:AlternateContent>");
  Parsed p(modern);
  Chart c;
  REQUIRE(Read(p.Root(), Colors(), c));
  CHECK(!c.legacy);
  CHECK_EQ(c.textSize, 10.0f);
  CHECK(c.value.ticks == Axis::Tick::Cross && !c.value.line.line && !c.value.grid.noLine);
  draw::Art art = Draw(c, {360, 216}, Colors(), L"Calibri");
  CHECK(strokes(art, 0x868686) >= 10);  // the border, 6 gridlines, 2 axis lines and their ticks
  // Word draws a chart without c14:style in Office 2010's look too; Excel, in a workbook, in 2007's.
  Parsed old(ColumnChart());
  REQUIRE(Read(old.Root(), Colors(), c));
  CHECK(!c.legacy && !c.roundedCorners && c.titleSize == 18 && c.titleBold);
  struct Workbook : DataSource {
    bool Cells(std::string_view, std::vector<std::string>&, std::vector<double>&, std::string*) override { return false; }
  } book;
  REQUIRE(Read(old.Root(), Colors(), c, &book));
  // Its title's text states nothing at all: DrawingML's own 18-point Arial (Excel's PDFs).
  CHECK(c.legacy && c.roundedCorners && c.titleSize == 18 && !c.titleBold && c.titleFont == L"Arial");
  art = Draw(c, {360, 216}, Colors(), L"Calibri");
  CHECK_EQ(strokes(art, 0x868686), 1u);  // the border
  CHECK(strokes(art, 0x000000) >= 9);
  // What a chart states wins: Office 2013 and later's 9 points on the axes, no value axis line.
  std::string stated = ColumnChart();
  const std::string valAx = "<c:valAx><c:axId val=\"2\"/>";
  stated.replace(stated.find(valAx), valAx.size(),
                 valAx + "<c:majorTickMark val=\"none\"/><c:spPr><a:ln><a:noFill/></a:ln></c:spPr><c:txPr><a:bodyPr/>"
                         "<a:p><a:pPr><a:defRPr sz=\"900\"/></a:pPr></a:p></c:txPr>");
  Parsed q(stated);
  REQUIRE(Read(q.Root(), Colors(), c));
  CHECK_EQ(c.textSize, 9.0f);
  CHECK(c.value.ticks == Axis::Tick::None && c.value.line.noLine);
}

TEST(Chart_StackedSeriesListedAsTheyStack) {
  // A stacked column chart with its legend at the side lists the series top down, as they stack.
  std::string xml = ColumnChart();
  xml.replace(xml.find("clustered"), 9, "stacked");
  xml.replace(xml.find("<c:legendPos val=\"b\"/>"), 22, "<c:legendPos val=\"r\"/>");
  Parsed p(xml);
  Chart c;
  REQUIRE(Read(p.Root(), Colors(), c));
  REQUIRE(c.groups.size() == 1u && c.groups[0].grouping == Grouping::Stacked && c.legend == LegendPos::Right);
  const draw::Art art = Draw(c, {360, 216}, Colors(), L"Calibri");
  // The legend's swatches: the second series' above the first's.
  const draw::Color first = *draw::SchemeColor("accent1", Colors()), second = *draw::SchemeColor("accent2", Colors());
  auto swatchTop = [&](const draw::Color& color) {
    float top = -1;
    for (const auto& m : art.marks) {
      const draw::Rect b = m.path.Bounds();
      if (m.fill.kind == draw::Fill::Kind::Solid && Same(m.fill.color, color) && b.right - b.left < 10 && b.bottom - b.top < 10)
        top = b.top;  // a swatch, not a bar
    }
    return top;
  };
  CHECK(swatchTop(second) >= 0 && swatchTop(first) > swatchTop(second));
}

TEST(Chart_OtherTypesStayPlaceholders) {
  Parsed stock(std::string("<c:chartSpace") + kNs + "><c:chart><c:plotArea><c:stockChart>" + Ser(0, "S", {"a"}, {"1"}) +
               "</c:stockChart></c:plotArea></c:chart></c:chartSpace>");
  Chart c;
  CHECK(!Read(stock.Root(), Colors(), c));
  Parsed empty(std::string("<c:chartSpace") + kNs + "><c:chart><c:plotArea/></c:chart></c:chartSpace>");
  CHECK(!Read(empty.Root(), Colors(), c));
}

TEST(Chart_UnstatedChartAreaFillByProgram) {
  // No chart-area c:spPr: white in a workbook or document, unfilled on a slide (PowerPoint shows
  // the slide through it); a stated fill is drawn on a slide too.
  // Whether something fills the whole chart area.
  const auto filledArea = [](const std::string& xml, bool presentation) {
    Parsed doc(xml);
    Chart c;
    if (!Read(doc.Root(), Colors(), c, nullptr, presentation)) return false;
    const draw::Art art = Draw(c, {400, 300}, Colors(), L"Calibri");
    for (const auto& m : art.marks) {
      const draw::Rect b = m.path.Bounds();
      if (m.fill.kind == draw::Fill::Kind::Solid && b.right - b.left >= 399 && b.bottom - b.top >= 299) return true;
    }
    return false;
  };
  CHECK(filledArea(ColumnChart(), false));
  CHECK(!filledArea(ColumnChart(), true));
  std::string stated = ColumnChart();
  stated.insert(stated.rfind("</c:chartSpace>"), "<c:spPr><a:solidFill><a:srgbClr val=\"FFFF00\"/></a:solidFill></c:spPr>");
  CHECK(filledArea(stated, true));
}

TEST(Chart_SlideTitleIsTheTextSizeTimesOnePointTwo) {
  // A styled chart (c14:style) on a slide with c:txPr at 18 pt and an automatic title: 21.6 pt
  // (poi line chart in PowerPoint's PDF); in a workbook Excel's 14/9 ratio stays.
  std::string xml = ColumnChart();
  const std::string title = "<c:title><c:tx><c:rich><a:bodyPr/><a:p><a:r><a:t>Sales</a:t></a:r></a:p></c:rich></c:tx></c:title>";
  xml.replace(xml.find(title), title.size(), "<c:title><c:overlay val=\"0\"/></c:title>");
  xml.insert(xml.find("<c:chart>"), "<mc:AlternateContent xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\">"
                                    "<mc:Choice Requires=\"c14\" xmlns:c14=\"http://schemas.microsoft.com/office/drawing/2007/8/2/chart\">"
                                    "<c14:style val=\"102\"/></mc:Choice><mc:Fallback><c:style val=\"2\"/></mc:Fallback></mc:AlternateContent>");
  xml.insert(xml.rfind("</c:chartSpace>"), "<c:txPr><a:bodyPr/><a:lstStyle/><a:p><a:pPr><a:defRPr sz=\"1800\"/></a:pPr>"
                                           "<a:endParaRPr lang=\"en-US\"/></a:p></c:txPr>");
  Parsed doc(xml);
  Chart slide, book;
  CHECK(Read(doc.Root(), Colors(), slide, nullptr, true));
  CHECK_NEAR(slide.titleSize, 21.6f, 0.01f);
  CHECK(Read(doc.Root(), Colors(), book, nullptr, false));
  CHECK_NEAR(book.titleSize, 28.f, 0.01f);
}

TEST(Chart_SlideLabelRoomFollowsTheFontsAscent) {
  // Segoe UI's ascent (1.08 em) is 13 % over Calibri's (.95): on a slide the category labels below
  // the plot take that much more room, so the bars end higher (PowerPoint's PDFs); in a workbook
  // the room stays Calibri's.
  CHECK_NEAR(draw::TextAscent(L"Calibri", false, false), 0.952f, 0.01f);
  Parsed doc(ColumnChart());
  const auto plotBottom = [](const draw::Art& art) {
    float bottom = -1e9f;
    for (const auto& m : art.marks) {
      if (m.fill.kind != draw::Fill::Kind::Solid) continue;
      for (const draw::Path::Polygon& f : m.path.Flatten(0.25f)) {
        float l = 1e9f, t = 1e9f, r = -1e9f, b = -1e9f;
        for (const draw::Point& pt : f.points) l = std::min(l, pt.x), t = std::min(t, pt.y), r = std::max(r, pt.x), b = std::max(b, pt.y);
        if (r - l > 10 && b - t > 20 && r - l < 100) bottom = std::max(bottom, b);  // a bar
      }
    }
    return bottom;
  };
  for (const bool slide : {true, false}) {
    Chart c;
    REQUIRE(Read(doc.Root(), Colors(), c, nullptr, slide));
    const float calibri = plotBottom(Draw(c, {360, 216}, Colors(), L"Calibri"));
    const float segoe = plotBottom(Draw(c, {360, 216}, Colors(), L"Segoe UI"));
    CHECK(calibri > 100);
    if (slide) CHECK(segoe < calibri - 1);
    else CHECK_NEAR(segoe, calibri, 0.01f);
  }
}

TEST(Chart_BubblesByArea) {
  // Bubbles at (x, y) sized by area: four times the size, twice as wide; the largest a quarter of
  // the plot's smaller side across (PowerPoint's PDFs). A series each, so each has its own color.
  const auto ser = [](int i, const char* x, const char* y, const char* size) {
    return "<c:ser><c:idx val=\"" + std::to_string(i) + "\"/><c:order val=\"" + std::to_string(i) + "\"/><c:xVal>" +
           Cache("numLit", {x}) + "</c:xVal><c:yVal>" + Cache("numLit", {y}) + "</c:yVal><c:bubbleSize>" +
           Cache("numLit", {size}) + "</c:bubbleSize></c:ser>";
  };
  Parsed bubble(std::string("<c:chartSpace") + kNs + "><c:chart><c:plotArea><c:bubbleChart><c:varyColors val=\"0\"/>" +
                ser(0, "1", "3", "1") + ser(1, "2", "5", "4") +
                "<c:axId val=\"1\"/><c:axId val=\"2\"/></c:bubbleChart>"
                "<c:valAx><c:axId val=\"1\"/><c:axPos val=\"b\"/><c:crossAx val=\"2\"/></c:valAx>"
                "<c:valAx><c:axId val=\"2\"/><c:axPos val=\"l\"/><c:crossAx val=\"1\"/></c:valAx></c:plotArea>"
                "</c:chart></c:chartSpace>");
  Chart c;
  REQUIRE(Read(bubble.Root(), Colors(), c));
  REQUIRE(c.groups.size() == 1 && c.groups[0].type == Type::Bubble && c.groups[0].series.size() == 2);
  const Series& s = c.groups[0].series[1];
  CHECK(s.xs.size() == 1 && s.xs[0] == 2 && s.values[0] == 5 && s.sizes.size() == 1 && s.sizes[0] == 4);
  const draw::Art art = Draw(c, {400, 300}, Colors(), L"Calibri");
  const auto width = [&](const char* accent) {
    const draw::Color fill = *draw::SchemeColor(accent, Colors());
    for (const auto& m : art.marks) {
      const draw::Rect b = m.path.Bounds();
      if (m.fill.kind == draw::Fill::Kind::Solid && Same(m.fill.color, fill)) return b.right - b.left;
    }
    return 0.0f;
  };
  const float narrow = width("accent1"), wide = width("accent2");
  REQUIRE(narrow > 0);
  CHECK(std::fabs(wide / narrow - 2) < 0.01f);
  CHECK(wide > 50 && wide < 75);  // a quarter of a plot a little under 300 points high
}

TEST(Chart_RadarSpokesAndLines) {
  // A spoke a category, the series closed lines through their values (Excel's look).
  Parsed radar(std::string("<c:chartSpace") + kNs + "><c:chart><c:plotArea><c:radarChart><c:radarStyle val=\"marker\"/>" +
               Ser(0, "North", {"a", "b", "c"}, {"10", "20", "15"}) +
               "<c:axId val=\"1\"/><c:axId val=\"2\"/></c:radarChart><c:catAx><c:axId val=\"1\"/><c:crossAx val=\"2\"/></c:catAx>"
               "<c:valAx><c:axId val=\"2\"/><c:majorGridlines/><c:crossAx val=\"1\"/></c:valAx></c:plotArea>"
               "<c:legend><c:legendPos val=\"r\"/></c:legend></c:chart></c:chartSpace>");
  Chart c;
  REQUIRE(Read(radar.Root(), Colors(), c));
  REQUIRE(c.groups.size() == 1u && c.groups[0].type == Type::Radar && c.groups[0].marker);
  const draw::Art art = Draw(c, {360, 216}, Colors(), L"Calibri");
  const draw::Color accent = *draw::SchemeColor("accent1", Colors());
  size_t lines = 0;
  for (const auto& m : art.marks) lines += m.line.fill.kind == draw::Fill::Kind::Solid && Same(m.line.fill.color, accent);
  CHECK(lines >= 5);  // the series' line, its 3 markers' outlines, the legend's key
}

TEST(Chart_LabelPartsKeysAndOverlay) {
  // Data labels with the series and category names (Excel's order and separator, a legend key
  // before them); a title and legend over the plot, which keeps its room.
  std::string xml = ColumnChart();
  xml.replace(xml.find("<c:gapWidth"), 0,
              "<c:dLbls><c:showLegendKey val=\"1\"/><c:showVal val=\"1\"/><c:showCatName val=\"1\"/>"
              "<c:showSerName val=\"1\"/><c:separator>; </c:separator></c:dLbls>");
  Parsed p(xml);
  Chart c;
  REQUIRE(Read(p.Root(), Colors(), c));
  const Group& g = c.groups[0];
  CHECK(g.seriesNames && g.categoryLabels && g.labels && g.keys && g.separator && *g.separator == L"; ");
  CHECK(!c.titleOverlay && !c.legendOverlay);
  const auto plotTop = [](const draw::Art& art) {
    float top = 1e9f;
    for (const auto& m : art.marks) {
      if (m.fill.kind != draw::Fill::Kind::Solid) continue;
      for (const draw::Path::Polygon& f : m.path.Flatten(0.25f)) {  // each bar of a series' mark
        float l = 1e9f, t = 1e9f, r = -1e9f, b = -1e9f;
        for (const draw::Point& pt : f.points) l = std::min(l, pt.x), t = std::min(t, pt.y), r = std::max(r, pt.x), b = std::max(b, pt.y);
        if (r - l > 10 && b - t > 20 && r - l < 100) top = std::min(top, t);  // the tallest bar
      }
    }
    return top;
  };
  const float apart = plotTop(Draw(c, {360, 216}, Colors(), L"Calibri"));
  c.titleOverlay = c.legendOverlay = true;
  const float over = plotTop(Draw(c, {360, 216}, Colors(), L"Calibri"));
  CHECK(over < apart - 10);  // the bars reach higher with the title over them
  // Without c:separator, the user's list separator and a space.
  SetListSeparator(L';');
  std::string plain = ColumnChart();
  plain.replace(plain.find("<c:gapWidth"), 0, "<c:dLbls><c:showVal val=\"1\"/><c:showSerName val=\"1\"/></c:dLbls>");
  Parsed q(plain);
  REQUIRE(Read(q.Root(), Colors(), c));
  CHECK(!c.groups[0].separator && c.groups[0].seriesNames);
  SetListSeparator(L',');
}

TEST(Chart_LongTitleWrapsAsExcelDoes) {
  // Excel (a 355 x 211-point chart, 14-point title): a title wider than about 80 % of the chart
  // wraps at spaces, a word too wide for a line alone is broken, each line after the first takes
  // 1.22 times the size from the plot; a 92-point one keeps to one line. fuzz_chart found a title
  // of 92 points drawn far outside a 60-point chart.
  Parsed p(ColumnChart());
  Chart c;
  REQUIRE(Read(p.Root(), Colors(), c));
  c.titleSize = 14;
  const draw::Color red = draw::Rgb(0xFF0000);
  c.titleColor = red;
  // The title's lines (the marks in its color): left, baseline, right.
  struct Line {
    float left, baseline, right;
  };
  const auto lines = [&](draw::Size size) {
    const draw::Art art = Draw(c, size, Colors(), L"Calibri");
    std::vector<Line> out;
    for (const draw::Art::Mark& m : art.marks) {
      if (m.fill.kind != draw::Fill::Kind::Solid || !Same(m.fill.color, red)) continue;
      const draw::Rect b = m.path.Bounds();
      const draw::Point a = m.transform.Apply({b.left, b.top}), z = m.transform.Apply({b.right, b.bottom});
      out.push_back({a.x, m.transform.Apply({0, 0}).y, z.x});
    }
    return out;
  };
  CHECK_EQ(lines({355, 211}).size(), 1u);  // "Sales"
  c.title = L"Quarterly revenue by region for the fiscal year two thousand twenty six and beyond";
  std::vector<Line> two = lines({355, 211});
  REQUIRE(two.size() == 2u);
  CHECK(std::abs(two[1].baseline - two[0].baseline - 1.22f * 14) < 0.01f);
  for (const Line& l : two) CHECK(l.left >= 0.1f * 355 - 0.5f && l.right <= 0.9f * 355 + 0.5f);
  c.title = L"Supercalifragilisticexpialidocious";
  c.title += c.title + c.title;
  CHECK_EQ(lines({355, 211}).size(), 3u);
  c.title = L"Lines with markers";
  CHECK_EQ(lines({55, 211}).size(), 4u);
  c.titleSize = 92;
  CHECK_EQ(lines({355, 211}).size(), 1u);
  for (const draw::Size s : {draw::Size{60, 40}, draw::Size{1500, 8}}) {
    const std::vector<Line> one = lines(s);
    REQUIRE(one.size() == 1u);
    CHECK(one[0].left > -s.width && one[0].right < 2 * s.width);
  }
}

TEST(Chart_LongLegendEntryWrapsAsExcelDoes) {
  // Excel: a series name of 110 characters in a bottom legend wraps to 3 lines in a 355-point
  // chart, to 5 in a 195-point one; at the side it wraps too. fuzz_chart found such a name drawn
  // far outside a small chart.
  Parsed p(ColumnChart());
  Chart c;
  REQUIRE(Read(p.Root(), Colors(), c));
  c.title.clear();
  c.value.shown = c.category.shown = false;
  const draw::Color red = draw::Rgb(0xFF0000);
  c.textColor = red;  // the legend's text is all the text there is
  c.groups[0].series[1].name = L"Series" + std::wstring(108, L'O') + L" 3";
  const auto lines = [&](draw::Size size) {
    const draw::Art art = Draw(c, size, Colors(), L"Calibri");
    size_t n = 0;
    for (const draw::Art::Mark& m : art.marks) {
      const draw::Rect b = m.path.Bounds();
      for (const draw::Point corner : {draw::Point{b.left, b.top}, draw::Point{b.right, b.bottom}}) {
        const draw::Point at = m.transform.Apply(corner);
        CHECK(at.x >= -0.5f && at.x <= size.width + 0.5f && at.y >= -0.5f && at.y <= size.height + 0.5f);
      }
      n += m.fill.kind == draw::Fill::Kind::Solid && Same(m.fill.color, red);
    }
    return n - 1;  // "North" is one line
  };
  CHECK_EQ(lines({355, 211}), 3u);
  CHECK_EQ(lines({195, 211}), 5u);
  c.legend = LegendPos::Right;
  CHECK(lines({355, 211}) > 3u);
  CHECK(lines({195, 211}) > 5u);
}

TEST(Chart_TextFarTooLongForItsPlaceIsCut) {
  // Category names, axis titles and data labels hundreds of characters long are cut to their
  // room, so nothing lands far outside the chart (fuzz_chart's check); bars stay where they were.
  // With 100 categories most labels are only measured, and cut when they're drawn (a bar chart's
  // labels up the side show every few).
  const std::wstring longName(200, L'W');
  for (const int kind : {0, 1, 2, 3}) {
    const bool bar = kind % 2 == 1;
    Parsed p(ColumnChart());
    Chart c;
    REQUIRE(Read(p.Root(), Colors(), c));
    Group& g = c.groups[0];
    if (bar) g.type = Type::Bar;
    for (Series& s : g.series) {
      if (kind >= 2) s.categories.assign(100, longName), s.values.assign(100, 1.0);
      s.categories[1] = longName, s.name = longName;
    }
    g.seriesNames = true;
    c.value.title = c.category.title = longName;
    for (const draw::Size size : {draw::Size{432, 252}, draw::Size{120, 90}}) {
      const draw::Art art = Draw(c, size, Colors(), L"Calibri");
      if (size.width > 400) CHECK(Filled(art, *draw::SchemeColor("accent1", Colors())) >= 3u);  // the bars are still drawn
      for (const draw::Art::Mark& m : art.marks) {
        const draw::Rect b = m.path.Bounds();
        for (const draw::Point corner : {draw::Point{b.left, b.top}, draw::Point{b.right, b.bottom}}) {
          const draw::Point at = m.transform.Apply(corner);
          CHECK(at.x > -size.width && at.x < 2 * size.width && at.y > -size.height && at.y < 2 * size.height);
        }
      }
    }
  }
}

TEST(Chart_NumbersAsTheirFormatCodesShowThem) {
  SetNumberSeparators(L'.', L',');
  SetShortDate("");
  CHECK(FormatNumber(1234.5, "General") == L"1234.5");
  CHECK(FormatNumber(1234.5, "") == L"1234.5");
  CHECK(FormatNumber(1234567.891, "#,##0.00") == L"1,234,567.89");
  CHECK(FormatNumber(0.256, "0%") == L"26%");
  CHECK(FormatNumber(0.256, "0.0%") == L"25.6%");
  CHECK(FormatNumber(-42, "\"$\"#,##0") == L"-$42");
  CHECK(FormatNumber(1500, "[$€-413] #,##0") == L"€ 1,500");
  CHECK(FormatNumber(45000, "m/d/yyyy") == L"3/15/2023");
  CHECK(FormatNumber(45000, "d-mmm") == L"15-Mar");
  CHECK(FormatNumber(3, "0.00;[Red]-0.00") == L"3.00");
  // With the separators of the Netherlands, as Office shows them there.
  SetNumberSeparators(L',', L'.');
  CHECK(FormatNumber(1234567.891, "#,##0.00") == L"1.234.567,89");
  CHECK(FormatNumber(4.5, "General") == L"4,5");
  CHECK(FormatNumber(0.256, "0.0%") == L"25,6%");
  // Excel's built-in short date takes the user's form; other date codes stay as they are.
  SetShortDate("dd/MM/yyyy");
  CHECK(FormatNumber(45000, "m/d/yyyy") == L"15/03/2023");
  CHECK(FormatNumber(45000, "d-mmm") == L"15-Mar");
  SetShortDate("");
  SetNumberSeparators(L'.', L',');
}

TEST(Chart_ShowsInAWordDrawing) {
  // fwdraw reads the chart through its hook: art in place of the placeholder.
  struct Resources : draw::DrawingResources {
    Parsed chart{ColumnChart()};
    std::shared_ptr<const draw::Image> GetImage(std::string_view) override { return nullptr; }
    pugi::xml_node GetPartXml(std::string_view relId) override { return relId == "rIdChart" ? chart.Root() : pugi::xml_node(); }
  } res;
  const std::string xml =
      "<w:drawing xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\""
      " xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\""
      " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
      " xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\""
      " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><wp:inline><wp:extent cx=\"5486400\""
      " cy=\"3200400\"/><wp:docPr id=\"1\" name=\"Chart 1\"/><a:graphic><a:graphicData"
      " uri=\"http://schemas.openxmlformats.org/drawingml/2006/chart\"><c:chart r:id=\"rIdChart\"/></a:graphicData>"
      "</a:graphic></wp:inline></w:drawing>";
  Parsed d(xml);
  Install();
  auto drawing = draw::Drawing::FromWordDrawing(d.Root(), res);
  REQUIRE(drawing != nullptr);
  CHECK(!drawing->IsPlaceholder());
  CHECK(drawing->Extent().width == 432 && drawing->Extent().height == 252);
  // A chart part that's missing leaves the placeholder.
  struct None : draw::DrawingResources {
    std::shared_ptr<const draw::Image> GetImage(std::string_view) override { return nullptr; }
  } none;
  auto missing = draw::Drawing::FromWordDrawing(d.Root(), none);
  REQUIRE(missing != nullptr);
  CHECK(missing->IsPlaceholder());
}

TEST(Chart_PartIsKeptAsItWasOnSave) {
  const std::string chart = ColumnChart();
  const std::string body =
      "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\""
      " xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\""
      " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
      " xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\""
      " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><w:body><w:p><w:r><w:t>Text</w:t>"
      "</w:r><w:r><w:drawing><wp:inline><wp:extent cx=\"5486400\" cy=\"3200400\"/><wp:docPr id=\"1\" name=\"Chart 1\"/>"
      "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/chart\"><c:chart r:id=\"rId2\"/>"
      "</a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p><w:sectPr/></w:body></w:document>";
  const std::vector<uint8_t> zip = test::MakeZip({
      {"[Content_Types].xml",
       "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Default Extension=\"rels\" "
       "ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" "
       "ContentType=\"application/xml\"/><Override PartName=\"/word/document.xml\" "
       "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/><Override "
       "PartName=\"/word/charts/chart1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.drawingml.chart+xml\"/>"
       "</Types>"},
      {"_rels/.rels",
       "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" "
       "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
       "Target=\"word/document.xml\"/></Relationships>"},
      {"word/_rels/document.xml.rels",
       "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId2\" "
       "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/chart\" "
       "Target=\"charts/chart1.xml\"/></Relationships>"},
      {"word/document.xml", body},
      {"word/charts/chart1.xml", chart},
  });
  fw::pkg::Status st;
  auto pkg = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(zip), fw::pkg::Limits{}, st);
  REQUIRE(pkg != nullptr);
  auto doc = fw::doc::docx::Read(std::move(pkg), st);
  REQUIRE(doc != nullptr);
  fw::doc::edit::Editor ed(*doc);
  REQUIRE(ed.InsertText({0, 0}, u"More ").ok);
  fw::doc::docx::SaveOptions o;
  o.changedParts = ed.DirtyParts();
  o.edited = true;
  o.now = 1;
  std::vector<uint8_t> out;
  REQUIRE(fw::doc::docx::Serialize(*doc, o, out).Ok());
  auto back = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(out), fw::pkg::Limits{}, st);
  REQUIRE(back != nullptr);
  std::vector<uint8_t> part;
  REQUIRE(back->ReadPart("/word/charts/chart1.xml", part).Ok());
  CHECK(std::string(part.begin(), part.end()) == chart);
}
