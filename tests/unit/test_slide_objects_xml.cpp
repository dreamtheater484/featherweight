// Objects on slides as PresentationML (P2, p2-objects): transforms read and written in place, new
// objects as PowerPoint inserts them, other prefixes kept, fill/outline/shadow/crop, grouping and
// ungrouping, table rows and columns with merged cells, and charts with their embedded workbook.

#include <pugixml.hpp>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/Status.h"
#include "slide/objects/ChartPart.h"
#include "slide/objects/ObjectXml.h"

using namespace fw::slide::objects;

namespace {

const char* kSlide =
    "<p:sld xmlns:a='http://schemas.openxmlformats.org/drawingml/2006/main' "
    "xmlns:r='http://schemas.openxmlformats.org/officeDocument/2006/relationships' "
    "xmlns:p='http://schemas.openxmlformats.org/presentationml/2006/main'><p:cSld><p:spTree>"
    "<p:nvGrpSpPr><p:cNvPr id='1' name=''/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>"
    "<p:extLst><p:ext uri='{x}'/></p:extLst></p:spTree></p:cSld></p:sld>";

pugi::xml_node Tree(pugi::xml_document& doc, const char* xml = kSlide) {
  doc.load_string(xml);
  return doc.document_element().first_child().first_child();
}

Xfrm At(int64_t x, int64_t y, int64_t cx, int64_t cy) {
  Xfrm v;
  v.x = x, v.y = y, v.cx = cx, v.cy = cy;
  return v;
}

std::string Text(pugi::xml_node n) { return ToXml(n); }
bool Has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

}  // namespace

TEST(SlideObjectsXml_NewObjectsAndTransforms) {
  pugi::xml_document doc;
  pugi::xml_node tree = Tree(doc);
  pugi::xml_node sp = AppendShape(tree, 4, "roundRect", At(12700, 25400, 1270000, 635000), NewName("Rectangle: Rounded Corners", 4));
  CHECK(sp);
  // Before the tree's extLst, which stays last.
  CHECK(std::string(sp.next_sibling().name()) == "p:extLst");
  CHECK_EQ(ObjectId(sp), 4u);
  CHECK_EQ(ObjectName(sp), std::string("Rectangle: Rounded Corners 3"));
  CHECK(KindOf(sp) == ObjectKind::Shape);
  Xfrm x;
  CHECK(ReadXfrm(sp, x));
  CHECK_EQ(x.x, 12700);
  CHECK_EQ(x.cy, 635000);
  const Frame f = ToFrame(x);
  CHECK_NEAR(f.rect.left, 1, 0.0001);
  CHECK_NEAR(f.rect.Width(), 100, 0.0001);
  // Turn and flip: written as PowerPoint writes them; back to 0 removes the attributes.
  x.rot = 5400000;
  x.flipH = true;
  WriteXfrm(sp, x);
  CHECK(Has(Text(XfrmNode(sp)), "rot=\"5400000\""));
  CHECK(Has(Text(XfrmNode(sp)), "flipH=\"1\""));
  x.rot = 0;
  x.flipH = false;
  WriteXfrm(sp, x);
  CHECK(!Has(Text(XfrmNode(sp)), "rot="));
  // A move changes the offset only.
  MoveBy(sp, 100, -200);
  CHECK(ReadXfrm(sp, x));
  CHECK_EQ(x.x, 12800);
  CHECK_EQ(x.y, 25200);
  CHECK_EQ(x.cx, 1270000);
  // The other new objects.
  pugi::xml_node tb = AppendTextBox(tree, 5, At(0, 0, 100, 100));
  CHECK(Has(Text(tb), "txBox=\"1\""));
  CHECK(Has(Text(tb), "<a:spAutoFit/>"));
  pugi::xml_node line = AppendLine(tree, 6, At(0, 0, 100, 0), true);
  CHECK(KindOf(line) == ObjectKind::Connector);
  CHECK(Has(Text(line), "straightConnector1"));
  CHECK(Has(Text(line), "tailEnd type=\"triangle\""));
  pugi::xml_node pic = AppendPicture(tree, 7, "rId9", At(0, 0, 100, 100), {}, "A <cat>");
  CHECK(Has(Text(pic), "r:embed=\"rId9\""));
  CHECK_EQ(AltText(pic), std::string("A <cat>"));
  CHECK(Has(Text(pic), "noChangeAspect=\"1\""));
  pugi::xml_node chart = AppendChart(tree, 8, "rId10", At(0, 0, 100, 100));
  CHECK(Has(Text(chart), "r:id=\"rId10\""));
  CHECK(ReadXfrm(chart, x));
  CHECK_EQ(x.cx, 100);
  CHECK_EQ(MaxObjectId(tree), 8u);
  // A placeholder with no transform of its own gets one in its spPr, first.
  pugi::xml_document ph;
  ph.load_string(
      "<p:sp xmlns:p='http://schemas.openxmlformats.org/presentationml/2006/main' "
      "xmlns:a='http://schemas.openxmlformats.org/drawingml/2006/main'><p:nvSpPr><p:cNvPr id='2' name='Title 1'/>"
      "<p:cNvSpPr/><p:nvPr><p:ph type='title'/></p:nvPr></p:nvSpPr><p:spPr><a:noFill/></p:spPr></p:sp>");
  CHECK(IsPlaceholder(ph.document_element()));
  CHECK(!XfrmNode(ph.document_element()));
  WriteXfrm(ph.document_element(), At(1, 2, 3, 4));
  CHECK(std::string(ph.document_element().child("p:spPr").first_child().name()) == "a:xfrm");
}

TEST(SlideObjectsXml_OtherPrefixes) {
  // A deck written with other prefixes: new objects use them.
  pugi::xml_document doc;
  pugi::xml_node tree = Tree(doc,
                             "<pp:sld xmlns:d='http://schemas.openxmlformats.org/drawingml/2006/main' "
                             "xmlns:rel='http://schemas.openxmlformats.org/officeDocument/2006/relationships' "
                             "xmlns:pp='http://schemas.openxmlformats.org/presentationml/2006/main'><pp:cSld><pp:spTree/>"
                             "</pp:cSld></pp:sld>");
  pugi::xml_node pic = AppendPicture(tree, 2, "rId1", At(0, 0, 10, 10), {}, {});
  const std::string t = Text(pic);
  CHECK(Has(t, "<pp:pic>"));
  CHECK(Has(t, "<d:blip rel:embed=\"rId1\"/>"));
  CHECK(!Has(t, "<p:"));
  Xfrm x;
  CHECK(ReadXfrm(pic, x));
  CHECK_EQ(x.cx, 10);
  CHECK(SetFill(pic, Paint{Paint::Kind::Solid, 0x123456}));
  CHECK(Has(Text(pic), "<d:solidFill><d:srgbClr val=\"123456\"/></d:solidFill>"));
}

TEST(SlideObjectsXml_Format) {
  pugi::xml_document doc;
  pugi::xml_node tree = Tree(doc);
  pugi::xml_node sp = AppendShape(tree, 2, "rect", At(0, 0, 100, 100), "Rectangle 1");
  // Keep something unknown in spPr to see it survive.
  sp.child("p:spPr").append_child("a:extLst");
  Format f = ReadFormat(sp);
  CHECK(f.fillable && f.lineable);
  CHECK(f.fill.kind == Paint::Kind::Automatic);
  CHECK(SetFill(sp, Paint{Paint::Kind::Solid, 0xFF0000}));
  f = ReadFormat(sp);
  CHECK(f.fill.kind == Paint::Kind::Solid);
  CHECK_EQ(f.fill.rgb, 0xFF0000u);
  // The fill goes after the geometry, before the extLst.
  std::string t = Text(sp.child("p:spPr"));
  CHECK(t.find("prstGeom") < t.find("solidFill"));
  CHECK(t.find("solidFill") < t.find("extLst"));
  Paint g{Paint::Kind::Gradient, 0x000000, 0xFFFFFF, 45};
  CHECK(SetFill(sp, g));
  f = ReadFormat(sp);
  CHECK(f.fill.kind == Paint::Kind::Gradient);
  CHECK_NEAR(f.fill.angle, 45, 0.001);
  CHECK(!Has(Text(sp), "solidFill><a:srgbClr val=\"FF0000"));
  Paint pic{Paint::Kind::Picture};
  pic.relId = "rId3";
  CHECK(SetFill(sp, pic));
  CHECK(ReadFormat(sp).fill.relId == "rId3");
  CHECK(SetFill(sp, Paint{Paint::Kind::Automatic}));
  CHECK(ReadFormat(sp).fill.kind == Paint::Kind::Automatic);
  // The outline: color, width, dash; automatic again removes the empty a:ln.
  CHECK(SetLine(sp, Paint{Paint::Kind::Solid, 0x00FF00}));
  CHECK(SetLineWidth(sp, 2.25f));
  CHECK(SetLineDash(sp, "dash"));
  f = ReadFormat(sp);
  CHECK(f.line.kind == Paint::Kind::Solid);
  CHECK_EQ(f.line.rgb, 0x00FF00u);
  CHECK_NEAR(f.lineWidth, 2.25, 0.001);
  CHECK_EQ(f.dash, std::string("dash"));
  t = Text(sp.child("p:spPr").child("a:ln"));
  CHECK(t.find("solidFill") < t.find("prstDash"));
  CHECK(SetLine(sp, Paint{Paint::Kind::None}));
  CHECK(ReadFormat(sp).line.kind == Paint::Kind::None);
  CHECK(SetLineDash(sp, ""));
  CHECK(SetLineWidth(sp, 0));
  CHECK(SetLine(sp, Paint{Paint::Kind::Automatic}));
  CHECK(!sp.child("p:spPr").child("a:ln"));
  // Shadow on and off: an empty effect list stays (no shadow from the style either).
  CHECK(SetShadow(sp, true));
  CHECK(ReadFormat(sp).shadow);
  CHECK(Has(Text(sp), "outerShdw blurRad=\"50800\""));
  CHECK(SetShadow(sp, false));
  CHECK(!ReadFormat(sp).shadow);
  CHECK(sp.child("p:spPr").child("a:effectLst"));
  t = Text(sp.child("p:spPr"));
  CHECK(t.find("effectLst") < t.find("extLst"));
  // A graphic frame has neither fill nor outline.
  pugi::xml_node chart = AppendChart(tree, 3, "rId1", At(0, 0, 1, 1));
  CHECK(!SetFill(chart, Paint{Paint::Kind::Solid, 1}));
  CHECK(!SetLine(chart, Paint{Paint::Kind::Solid, 1}));
  // Crop.
  pugi::xml_node p = AppendPicture(tree, 4, "rId2", At(0, 0, 100, 100), {}, {});
  Crop c;
  c.left = 0.1f, c.bottom = 0.25f;
  CHECK(SetCrop(p, c));
  const Crop back = ReadCrop(p);
  CHECK_NEAR(back.left, 0.1, 0.00001);
  CHECK_NEAR(back.bottom, 0.25, 0.00001);
  t = Text(p.child("p:blipFill"));
  CHECK(t.find("srcRect") < t.find("stretch"));
  // Cropping by dragging: the left edge in by a tenth of the frame keeps the picture in place.
  const Crop next = CropForFrame({}, {0, 0, 100, 100}, {10, 0, 100, 100});
  CHECK_NEAR(next.left, 0.1, 0.0001);
  CHECK_NEAR(next.right, 0, 0.0001);
  const Crop more = CropForFrame(next, {10, 0, 100, 100}, {20, 0, 100, 100});
  CHECK_NEAR(more.left, 0.2, 0.0001);
}

TEST(SlideObjectsXml_GroupAndUngroup) {
  // Two children; the group's box goes around them.
  std::vector<Xfrm> kids = {At(100, 100, 200, 100), At(400, 300, 100, 100)};
  Xfrm g = GroupBox(kids);
  CHECK_EQ(g.x, 100);
  CHECK_EQ(g.y, 100);
  CHECK_EQ(g.cx, 400);
  CHECK_EQ(g.cy, 300);
  CHECK(g.child && g.chX == 100 && g.chCx == 400);
  // Ungrouping a group never changed: the children are where they were.
  Xfrm u = Ungrouped(g, kids[1]);
  CHECK_EQ(u.x, 400);
  CHECK_EQ(u.y, 300);
  CHECK_EQ(u.cx, 100);
  // The group moved and doubled: the children move and double with it.
  Xfrm moved = g;
  moved.x = 1000, moved.y = 0, moved.cx = 800, moved.cy = 600;
  u = Ungrouped(moved, kids[1]);
  CHECK_EQ(u.x, 1000 + 600);
  CHECK_EQ(u.y, 400);
  CHECK_EQ(u.cx, 200);
  // The group turned a quarter: a child at its right end ends up at the bottom, turned too.
  Xfrm turned = g;
  turned.rot = 5400000;
  u = Ungrouped(turned, kids[1]);
  // The group's centre is (300, 250); the child's centre (450, 350) is (150, 100) from it, which
  // turned clockwise is (-100, 150): (200, 400).
  CHECK_EQ(u.x + u.cx / 2, 200);
  CHECK_EQ(u.y + u.cy / 2, 400);
  CHECK_EQ(u.rot, 5400000);
  // Flipped: mirrored about the centre, the child flipped and its turn reversed.
  Xfrm flipped = g;
  flipped.flipH = true;
  Xfrm child = kids[1];
  child.rot = 600000;
  u = Ungrouped(flipped, child);
  CHECK_EQ(u.x + u.cx / 2, 300 - 150);
  CHECK(u.flipH);
  CHECK_EQ(u.rot, 21600000 - 600000);
  // A turned child's bounds count.
  std::vector<Xfrm> turnedKid = {At(0, 0, 200, 100)};
  turnedKid[0].rot = 5400000;
  g = GroupBox(turnedKid);
  CHECK_EQ(g.x, 50);
  CHECK_EQ(g.y, -50);
  CHECK_EQ(g.cx, 100);
  CHECK_EQ(g.cy, 200);
  // The group element.
  pugi::xml_document doc;
  pugi::xml_node grp = AppendGroup(Tree(doc), 9, GroupBox(kids));
  CHECK(KindOf(grp) == ObjectKind::Group);
  Xfrm back;
  CHECK(ReadXfrm(grp, back));
  CHECK(back.child && back.chCx == 400 && back.cx == 400);
}

TEST(SlideObjectsXml_ChildSpace) {
  // A group at (100, 50) points, twice its children's size, children's space from (10, 10).
  Xfrm g = At(ToEmu(100), ToEmu(50), ToEmu(200), ToEmu(100));
  g.child = true;
  g.chX = ToEmu(10), g.chY = ToEmu(10), g.chCx = ToEmu(100), g.chCy = ToEmu(50);
  const draw::Matrix m = ChildSpace(g, ToFrame(g).Transform());
  const draw::Point p = m.Apply({60, 35});
  CHECK_NEAR(p.x, 200, 0.01);
  CHECK_NEAR(p.y, 100, 0.01);
  Frame local;
  local.rect = {10, 10, 20, 20};
  const Frame on = FrameOnSlide(local, m);
  CHECK_NEAR(on.rect.left, 100, 0.01);
  CHECK_NEAR(on.rect.Width(), 20, 0.01);
  const Frame back = FrameInParent(on, m);
  CHECK_NEAR(back.rect.left, 10, 0.01);
  CHECK_NEAR(back.rect.Width(), 10, 0.01);
  const draw::Point d = DeltaInParent({20, 10}, m);
  CHECK_NEAR(d.x, 10, 0.001);
  CHECK_NEAR(d.y, 5, 0.001);
  // A turned group: a child's turn adds to it, and comes back.
  Xfrm t = g;
  t.rot = 30 * 60000;
  const draw::Matrix mt = ChildSpace(t, ToFrame(t).Transform());
  local.rotation = 10;
  const Frame ont = FrameOnSlide(local, mt);
  CHECK_NEAR(ont.rotation, 40, 0.01);
  CHECK_NEAR(FrameInParent(ont, mt).rotation, 10, 0.01);
}

TEST(SlideObjectsXml_TableRowsAndColumns) {
  pugi::xml_document doc;
  pugi::xml_node tree = Tree(doc);
  pugi::xml_node frame = AppendTable(tree, 2, At(0, 0, 3000, 900), 3, 3);
  CHECK_EQ(TableRows(frame), 3);
  CHECK_EQ(TableColumns(frame), 3);
  CHECK_EQ(TableStyle(frame), std::string("{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}"));
  CHECK(TableOption(frame, "firstRow"));
  CHECK(!TableOption(frame, "firstCol"));
  SetTableOption(frame, "firstCol", true);
  CHECK(TableOption(frame, "firstCol"));
  SetTableStyle(frame, "{073A0DAA-6AF3-43AB-8588-CEC1D06C72B9}");
  CHECK_EQ(TableStyle(frame), std::string("{073A0DAA-6AF3-43AB-8588-CEC1D06C72B9}"));
  pugi::xml_node tbl = TableOf(frame);
  // Text in a cell, to see it's not copied into a new row.
  pugi::xml_node firstCell = tbl.child("a:tr").child("a:tc");
  pugi::xml_node para = firstCell.child("a:txBody").child("a:p");
  pugi::xml_node run = para.prepend_child("a:r");
  run.append_child("a:rPr").append_attribute("b") = "1";
  run.append_child("a:t").text() = "Hello";
  CHECK(InsertTableRow(frame, 1));
  CHECK_EQ(TableRows(frame), 4);
  Xfrm x;
  CHECK(ReadXfrm(frame, x));
  CHECK_EQ(x.cy, 1200);
  pugi::xml_node newRow = tbl.child("a:tr").next_sibling("a:tr");
  const std::string rowXml = Text(newRow);
  CHECK(!Has(rowXml, "Hello"));
  CHECK(Has(rowXml, "endParaRPr b=\"1\""));  // the formatting stays
  CHECK(InsertTableColumn(frame, 3));
  CHECK_EQ(TableColumns(frame), 4);
  CHECK(ReadXfrm(frame, x));
  CHECK_EQ(x.cx, 4000);
  for (pugi::xml_node tr : tbl.children("a:tr")) {
    int n = 0;
    for (pugi::xml_node tc : tr.children("a:tc")) n += tc ? 1 : 0;
    CHECK_EQ(n, 4);
  }
  CHECK(DeleteTableRow(frame, 0));
  CHECK(DeleteTableColumn(frame, 0));
  CHECK_EQ(TableRows(frame), 3);
  CHECK_EQ(TableColumns(frame), 3);
  CHECK(ReadXfrm(frame, x));
  CHECK_EQ(x.cx, 3000);
  CHECK_EQ(x.cy, 900);
  // The last row can't go.
  pugi::xml_document one;
  pugi::xml_node single = AppendTable(Tree(one), 2, At(0, 0, 100, 100), 1, 1);
  CHECK(!DeleteTableRow(single, 0));
  CHECK(!DeleteTableColumn(single, 0));
}

TEST(SlideObjectsXml_TableMerges) {
  // Rows 0-1 of column 0 merged (rowSpan 2), and columns 1-2 of row 2 merged (gridSpan 2).
  pugi::xml_document doc;
  doc.load_string(
      "<p:graphicFrame xmlns:p='http://schemas.openxmlformats.org/presentationml/2006/main' "
      "xmlns:a='http://schemas.openxmlformats.org/drawingml/2006/main'><p:nvGraphicFramePr><p:cNvPr id='2' name='T'/>"
      "</p:nvGraphicFramePr><p:xfrm><a:off x='0' y='0'/><a:ext cx='300' cy='300'/></p:xfrm><a:graphic><a:graphicData "
      "uri='http://schemas.openxmlformats.org/drawingml/2006/table'><a:tbl><a:tblPr/><a:tblGrid><a:gridCol w='100'/>"
      "<a:gridCol w='100'/><a:gridCol w='100'/></a:tblGrid>"
      "<a:tr h='100'><a:tc rowSpan='2'><a:txBody><a:p><a:r><a:t>M</a:t></a:r></a:p></a:txBody></a:tc><a:tc/><a:tc/></a:tr>"
      "<a:tr h='100'><a:tc vMerge='1'/><a:tc/><a:tc/></a:tr>"
      "<a:tr h='100'><a:tc/><a:tc gridSpan='2'><a:txBody><a:p><a:r><a:t>H</a:t></a:r></a:p></a:txBody></a:tc><a:tc hMerge='1'/></a:tr>"
      "</a:tbl></a:graphicData></a:graphic></p:graphicFrame>");
  pugi::xml_node frame = doc.document_element();
  pugi::xml_node tbl = TableOf(frame);
  auto cell = [&](int r, int c) {
    pugi::xml_node tr = tbl.child("a:tr");
    for (int i = 0; i < r; ++i) tr = tr.next_sibling("a:tr");
    pugi::xml_node tc = tr.child("a:tc");
    for (int i = 0; i < c; ++i) tc = tc.next_sibling("a:tc");
    return tc;
  };
  // A row inside the vertical merge: the merge grows, the new cell is covered.
  CHECK(InsertTableRow(frame, 1));
  CHECK_EQ(std::string(cell(0, 0).attribute("rowSpan").value()), std::string("3"));
  CHECK(cell(1, 0).attribute("vMerge"));
  CHECK(!cell(1, 1).attribute("vMerge"));
  // A column inside the horizontal merge: it grows too.
  CHECK(InsertTableColumn(frame, 2));
  CHECK_EQ(std::string(cell(3, 1).attribute("gridSpan").value()), std::string("3"));
  CHECK(cell(3, 2).attribute("hMerge"));
  CHECK(!cell(0, 2).attribute("hMerge"));
  // Deleting the merge's first row: the next row takes over its text and the rest of the span.
  CHECK(DeleteTableRow(frame, 0));
  CHECK_EQ(std::string(cell(0, 0).attribute("rowSpan").value()), std::string("2"));
  CHECK(!cell(0, 0).attribute("vMerge"));
  CHECK(Has(Text(cell(0, 0)), "<a:t>M</a:t>"));
  // Deleting a column the merge covers: it shrinks.
  CHECK(DeleteTableColumn(frame, 2));
  CHECK_EQ(std::string(cell(2, 1).attribute("gridSpan").value()), std::string("2"));
  // Deleting the merge's first column: the next takes over.
  CHECK(DeleteTableColumn(frame, 1));
  CHECK(!cell(2, 1).attribute("hMerge"));
  CHECK(!cell(2, 1).attribute("gridSpan"));
  CHECK(Has(Text(cell(2, 1)), "<a:t>H</a:t>"));
}

TEST(SlideObjectsXml_Chart) {
  ChartData d = DefaultChartData();
  for (ChartType t : {ChartType::Column, ChartType::Bar, ChartType::Line, ChartType::Pie}) {
    const std::string xml = ChartXml(t, d, "rId7");
    pugi::xml_document doc;
    CHECK(doc.load_string(xml.c_str()));
    CHECK(Has(xml, "<c:externalData r:id=\"rId7\">"));
    ChartData back;
    CHECK(ReadChartData(doc.document_element(), back));
    CHECK_EQ(back.categories.size(), size_t{4});
    CHECK_EQ(back.series.size(), t == ChartType::Pie ? size_t{1} : size_t{3});
    CHECK_NEAR(back.values[0][3], 4.5, 1e-9);
    CHECK_EQ(back.series[0], std::string("Series 1"));
  }
  CHECK(Has(ChartXml(ChartType::Column, d), "<c:f>Sheet1!$B$2:$B$5</c:f>"));
  CHECK(Has(ChartXml(ChartType::Column, d), "<c:f>Sheet1!$D$1</c:f>"));
  CHECK_EQ(ColumnName(0), std::string("A"));
  CHECK_EQ(ColumnName(25), std::string("Z"));
  CHECK_EQ(ColumnName(26), std::string("AA"));
  // New data: a series more, a category less, everything else kept.
  pugi::xml_document doc;
  doc.load_string(ChartXml(ChartType::Column, d).c_str());
  ChartData nd;
  nd.categories = {"North", "South & East", "West"};
  nd.series = {"2025", "2026", "2027", "2028"};
  nd.values = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {10, 11, 12.5}};
  CHECK(WriteChartData(doc.document_element(), nd));
  ChartData back;
  CHECK(ReadChartData(doc.document_element(), back));
  CHECK(back.categories == nd.categories);
  CHECK(back.series == nd.series);
  CHECK_NEAR(back.values[3][2], 12.5, 1e-9);
  const std::string after = ToXml(doc.document_element());
  CHECK(Has(after, "<c:f>Sheet1!$E$2:$E$4</c:f>"));
  CHECK(Has(after, "<c:gapWidth val=\"219\"/>"));
  CHECK(Has(after, "<c:idx val=\"3\"/>"));
  // The embedded workbook is a package with Sheet1 holding the data.
  const std::vector<uint8_t> xlsx = ChartWorkbook(nd);
  fw::pkg::Status st;
  auto snap = fw::pkg::Snapshot::FromBytes(std::vector<uint8_t>(xlsx));
  auto pk = fw::pkg::Package::Open(std::move(snap), fw::pkg::Limits::Default(), st);
  CHECK(pk != nullptr);
  if (pk) {
    std::vector<uint8_t> sheet;
    CHECK(pk->ReadPart("/xl/worksheets/sheet1.xml", sheet).Ok());
    const std::string s(sheet.begin(), sheet.end());
    CHECK(Has(s, "<c r=\"E4\"><v>12.5</v></c>"));
    CHECK(Has(s, "South &amp; East"));
  }
}
