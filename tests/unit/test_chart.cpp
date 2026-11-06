// fwchart (D5): charts read from their cached values, drawn as art, shown in Word drawings, and
// their parts kept as they were when the document is saved.
#include <windows.h>

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

size_t Filled(const draw::Art& art, const draw::Color& c) {
  size_t n = 0;
  for (const auto& m : art.marks) n += m.fill.kind == draw::Fill::Kind::Solid && Same(m.fill.color, c);
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
  for (const char* accent : {"accent1", "accent2", "accent3", "accent4"})
    CHECK_EQ(Filled(art, *draw::SchemeColor(accent, Colors())), 2u);  // a slice and its legend swatch
}

TEST(Chart_OtherTypesStayPlaceholders) {
  Parsed radar(std::string("<c:chartSpace") + kNs + "><c:chart><c:plotArea><c:radarChart>" + Ser(0, "S", {"a"}, {"1"}) +
               "</c:radarChart></c:plotArea></c:chart></c:chartSpace>");
  Chart c;
  CHECK(!Read(radar.Root(), Colors(), c));
  Parsed empty(std::string("<c:chartSpace") + kNs + "><c:chart><c:plotArea/></c:chart></c:chartSpace>");
  CHECK(!Read(empty.Root(), Colors(), c));
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
