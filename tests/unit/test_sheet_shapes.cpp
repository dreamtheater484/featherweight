// Pictures and shapes (s3-data): Excel's anchors for new ones, and their formatting read and
// written (sheet/data/Shapes.h).

#include "TestHarness.h"

#include <string>

#include <pugixml.hpp>

#include <cstdio>
#include <vector>

#include "SheetTestUtil.h"
#include "pkg/Check.h"
#include "sheet/data/Shapes.h"
#include "sheet/edit/Edit.h"
#include "sheet/edit/Journal.h"
#include "sheet/model/Workbook.h"
#include "sheet/xlsx/Xlsx.h"

using namespace fw::sheet;
using data::ObjectFormat;
using data::ShapeKind;
using edit::Editor;
using namespace sheet_test;

namespace {
std::vector<uint8_t> SaveBook(Workbook& book, Editor& ed) {
  ed.BeginSave();
  xlsx::SavePlan plan;
  fw::pkg::Status st = xlsx::PrepareSave(book, ed, plan, book.Format());
  CHECK(st.Ok());
  fw::pkg::MemorySink sink;
  if (st.Ok()) st = xlsx::WriteSave(book, plan, sink);
  CHECK(st.Ok());
  if (st.Ok()) xlsx::CommitSave(book, ed, plan);
  else ed.EndSave();
  wchar_t dir[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"FW_SAVE_OUT", dir, MAX_PATH);
  if (n && n < MAX_PATH) CHECK(test::WriteFileBytes(std::wstring(dir) + L"\\shapes.xlsx", sink.bytes));
  return sink.bytes;
}
std::string PartOf(const std::vector<uint8_t>& zip, const std::string& part) {
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(zip, st);
  std::vector<uint8_t> out;
  if (!pkg || !pkg->ReadPart(part, out).Ok()) return {};
  return test::Text(out);
}
void CleanZip(const std::vector<uint8_t>& zip) {
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(zip, st);
  CHECK(pkg != nullptr);
  if (!pkg) return;
  fw::pkg::CheckReport report = fw::pkg::CheckPackage(*pkg);
  for (const std::string& e : report.errors) std::printf("  check: %s\n", e.c_str());
  CHECK(report.Ok());
}
size_t Count(const std::string& s, const std::string& what) {
  size_t n = 0;
  for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
  return n;
}
// A 1 x 1 PNG.
const std::string kPng(
    "\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\0\x01\0\0\0\x01\x08\x06\0\0\0\x1f\x15\xc4\x89\0\0\0\rIDATx\x9c"
    "c\xf8\x0f\x00\x00\x01\x01\x00\x05\x18\xd8N\0\0\0\0IEND\xae\x42\x60\x82",
    67);
bool Parses(const std::string& xml) {
  pugi::xml_document d;
  return d.load_buffer(xml.data(), xml.size());
}
bool Has(const std::string& xml, const std::string& part) { return xml.find(part) != std::string::npos; }
}  // namespace

TEST(SheetShapes_Anchors) {
  const ShapeKind kinds[] = {ShapeKind::Rectangle, ShapeKind::RoundedRectangle, ShapeKind::Oval,
                             ShapeKind::Line,      ShapeKind::Arrow,            ShapeKind::TextBox};
  const char* names[] = {"Rectangle 1", "Rectangle: Rounded Corners 1", "Oval 1", "Straight Connector 1",
                         "Straight Arrow Connector 1", "TextBox 1"};
  for (int i = 0; i < 6; ++i) {
    const std::string xml = data::ShapeAnchor(kinds[i], 2, "Hi & bye\nTwo");
    CHECK(Parses(xml));
    CHECK(xml.rfind("<xdr:twoCellAnchor xmlns:xdr=", 0) == 0 && Has(xml, "<xdr:clientData/></xdr:twoCellAnchor>"));
    const ObjectFormat f = data::ReadObjectFormat(xml);
    CHECK(f.name == names[i]);
    const bool line = kinds[i] == ShapeKind::Line || kinds[i] == ShapeKind::Arrow;
    CHECK(f.kind == (line ? ObjectFormat::Kind::Connector : ObjectFormat::Kind::Shape));
    CHECK(f.hasText == !line);
    if (!line) CHECK(f.text == "Hi & bye\nTwo");
  }
  CHECK(Has(data::ShapeAnchor(ShapeKind::Oval, 2, ""), "<a:prstGeom prst=\"ellipse\">"));
  CHECK(Has(data::ShapeAnchor(ShapeKind::Arrow, 2, ""), "<a:prstGeom prst=\"straightConnector1\"><a:avLst/></a:prstGeom><a:ln><a:tailEnd type=\"triangle\"/></a:ln>"));
  CHECK(Has(data::ShapeAnchor(ShapeKind::TextBox, 2, ""), "<xdr:cNvSpPr txBox=\"1\"/>"));
  // A picture: moves but doesn't size with cells, its aspect locked, the alt text kept.
  const std::string pic = data::PictureAnchor(4, "rId7", "A \"quoted\" <alt>");
  CHECK(Parses(pic));
  CHECK(pic.rfind("<xdr:twoCellAnchor editAs=\"oneCell\"", 0) == 0);
  CHECK(Has(pic, "r:embed=\"rId7\"") && Has(pic, "<a:picLocks noChangeAspect=\"1\"/>"));
  const ObjectFormat p = data::ReadObjectFormat(pic);
  CHECK(p.kind == ObjectFormat::Kind::Picture && p.name == "Picture 3" && p.altText == "A \"quoted\" <alt>" && !p.hasText);
}

TEST(SheetShapes_PictureType) {
  std::string ext, type;
  CHECK(data::PictureType(std::string("\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\0\x01\0\0\0\x01\x08\x06\0\0\0", 29), ext, type) &&
        ext == ".png" && type == "image/png");
  CHECK(data::PictureType(std::string("\xFF\xD8\xFF\xE0\0\x10JFIF\0", 11), ext, type) && ext == ".jpeg" && type == "image/jpeg");
  CHECK(data::PictureType("GIF89a\x01\0\x01\0", ext, type) && type == "image/gif");
  CHECK(data::PictureType(std::string("BM\x3a\0\0\0\0\0\0\0\x36\0\0\0\x28\0\0\0", 18), ext, type) && type == "image/bmp");
  CHECK(!data::PictureType("<html>", ext, type));
}

TEST(SheetShapes_Format) {
  const std::string rect = data::ShapeAnchor(ShapeKind::Rectangle, 2, "");
  ObjectFormat f = data::ReadObjectFormat(rect);
  CHECK(f.fill == ObjectFormat::Paint::Automatic && f.line == ObjectFormat::Paint::Automatic);
  f.fill = ObjectFormat::Paint::Solid;
  f.fillRgb = 0xFF0000;
  f.line = ObjectFormat::Paint::Solid;
  f.lineRgb = 0x00FF00;
  f.lineWidth = 2;
  f.text = "Total\n\nEnd";
  f.altText = "Box";
  std::string out = data::FormatObject(rect, f, data::kFormatFill | data::kFormatLine | data::kFormatText | data::kFormatAltText);
  CHECK(Parses(out));
  CHECK(Has(out, "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:solidFill><a:srgbClr val=\"FF0000\"/></a:solidFill>"
                 "<a:ln w=\"25400\"><a:solidFill><a:srgbClr val=\"00FF00\"/></a:solidFill></a:ln></xdr:spPr>"));
  CHECK(Has(out, "<a:p><a:pPr algn=\"ctr\"/><a:r><a:rPr lang=\"en-US\" sz=\"1100\"/><a:t>Total</a:t></a:r><a:endParaRPr lang=\"en-US\" sz=\"1100\"/></a:p>"
                 "<a:p><a:pPr algn=\"ctr\"/><a:endParaRPr lang=\"en-US\" sz=\"1100\"/></a:p>"));
  CHECK(Has(out, "name=\"Rectangle 1\" descr=\"Box\""));
  ObjectFormat back = data::ReadObjectFormat(out);
  CHECK(back.fill == ObjectFormat::Paint::Solid && back.fillRgb == 0xFF0000 && back.line == ObjectFormat::Paint::Solid &&
        back.lineRgb == 0x00FF00 && back.lineWidth == 2 && back.text == "Total\n\nEnd" && back.altText == "Box");
  // Only what's named changes: no fill, the rest as it was; then automatic again.
  back.fill = ObjectFormat::Paint::None;
  back.lineRgb = 0x123456;  // not asked for
  out = data::FormatObject(out, back, data::kFormatFill);
  CHECK(Has(out, "</a:prstGeom><a:noFill/><a:ln w=\"25400\"><a:solidFill><a:srgbClr val=\"00FF00\"/>"));
  back.fill = ObjectFormat::Paint::Automatic;
  back.line = ObjectFormat::Paint::Automatic;
  back.altText.clear();
  out = data::FormatObject(out, back, data::kFormatFill | data::kFormatLine | data::kFormatAltText);
  CHECK(Has(out, "</a:prstGeom><a:ln w=\"25400\"/></xdr:spPr>") && !Has(out, "descr="));
  // An arrow's outline keeps its arrowhead after the fill, as the schema orders them.
  ObjectFormat arrow = data::ReadObjectFormat(data::ShapeAnchor(ShapeKind::Arrow, 3, ""));
  arrow.line = ObjectFormat::Paint::Solid;
  arrow.lineRgb = 0x0000FF;
  out = data::FormatObject(data::ShapeAnchor(ShapeKind::Arrow, 3, ""), arrow, data::kFormatLine | data::kFormatFill | data::kFormatText);
  CHECK(Has(out, "<a:ln><a:solidFill><a:srgbClr val=\"0000FF\"/></a:solidFill><a:tailEnd type=\"triangle\"/></a:ln>"));
  CHECK(!Has(out, "<a:p>"));  // connectors hold no text
}

// An anchor as Excel wrote it: its prefixes declared on the drawing's root, not on itself.
TEST(SheetShapes_ExcelAnchor) {
  const std::string excel =
      "<xdr:twoCellAnchor><xdr:from><xdr:col>2</xdr:col><xdr:colOff>50800</xdr:colOff><xdr:row>1</xdr:row><xdr:rowOff>63500"
      "</xdr:rowOff></xdr:from><xdr:to><xdr:col>4</xdr:col><xdr:colOff>355600</xdr:colOff><xdr:row>5</xdr:row><xdr:rowOff>63500"
      "</xdr:rowOff></xdr:to><xdr:sp macro=\"\" textlink=\"\"><xdr:nvSpPr><xdr:cNvPr id=\"2\" name=\"Rectangle 1\"><a:extLst><a:ext "
      "uri=\"{FF2B5EF4-FFF2-40B4-BE49-F238E27FC236}\"><a16:creationId xmlns:a16=\"http://schemas.microsoft.com/office/drawing/2014/main\" "
      "id=\"{009EC9CB-304A-62D0-CC4B-827E2DF4120A}\"/></a:ext></a:extLst></xdr:cNvPr><xdr:cNvSpPr/></xdr:nvSpPr><xdr:spPr><a:xfrm>"
      "<a:off x=\"1270000\" y=\"254000\"/><a:ext cx=\"1524000\" cy=\"762000\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/>"
      "</a:prstGeom></xdr:spPr><xdr:txBody><a:bodyPr vertOverflow=\"clip\" horzOverflow=\"clip\" rtlCol=\"0\" anchor=\"t\"/><a:lstStyle/>"
      "<a:p><a:pPr algn=\"l\"/><a:r><a:rPr lang=\"en-GB\" sz=\"1100\"/><a:t>In a box</a:t></a:r><a:endParaRPr lang=\"en-NL\" "
      "sz=\"1100\"/></a:p></xdr:txBody></xdr:sp><xdr:clientData/></xdr:twoCellAnchor>";
  ObjectFormat f = data::ReadObjectFormat(excel);
  CHECK(f.name == "Rectangle 1" && f.text == "In a box" && f.kind == ObjectFormat::Kind::Shape);
  f.text = " spaced ";
  const std::string out = data::FormatObject(excel, f, data::kFormatText);
  CHECK(Has(out, "<a:p><a:pPr algn=\"l\"/><a:r><a:rPr lang=\"en-GB\" sz=\"1100\"/><a:t> spaced </a:t></a:r><a:endParaRPr lang=\"en-NL\" sz=\"1100\"/></a:p>"));
  // The rest byte for byte, the creation id among it.
  CHECK(Has(out, "<a16:creationId xmlns:a16=\"http://schemas.microsoft.com/office/drawing/2014/main\" id=\"{009EC9CB-304A-62D0-CC4B-827E2DF4120A}\"/>"));
  CHECK(out.substr(0, out.find("<xdr:txBody>")) == excel.substr(0, excel.find("<xdr:txBody>")));
  CHECK(data::FormatObject("<not xml", f, data::kFormatText).empty());
}

// Insert > Pictures and Shapes through the Editor: Excel's parts, a picture stored once, formats,
// undo and the journal.
TEST(SheetShapes_Insert) {
  const char* data = "<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row></sheetData>";
  auto book = OpenParts({data, "<sheetData/>"});
  if (!book) return;
  Editor ed(*book);
  edit::Journal journal;
  ed.SetJournal(&journal);
  edit::drawing::Place at;
  at.from = {1, 1, 0, 0};
  at.to = {3, 5, 0, 0};
  at.x = 609600, at.y = 190500, at.cx = 1219200, at.cy = 762000;
  uint32_t shape = 0, pic1 = 0, pic2 = 0;
  CHECK(ed.InsertShape(0, ShapeKind::Rectangle, at, "Hello", &shape));
  CHECK(ed.InsertPicture(0, kPng, at, "A dot", &pic1));
  at.from.row = at.to.row = 9;
  CHECK(ed.InsertPicture(0, kPng, at, "", &pic2));  // the same picture again
  CHECK(shape == 2 && pic1 == 3 && pic2 == 4);
  CHECK(!ed.InsertPicture(0, "not a picture", at, ""));
  // Its format: a red fill.
  const std::string d = ed.SheetDrawing(0, false);
  std::string anchor = ed.AnchorXml(d, shape);
  ObjectFormat f = data::ReadObjectFormat(anchor);
  CHECK(f.name == "Rectangle 1" && f.text == "Hello");
  f.fill = ObjectFormat::Paint::Solid;
  f.fillRgb = 0xC00000;
  CHECK(ed.SetObjectXml(0, shape, data::FormatObject(anchor, f, data::kFormatFill), u"Format Shape"));
  CHECK(!ed.SetObjectXml(0, shape, data::FormatObject(ed.AnchorXml(d, pic1), f, data::kFormatFill), u"Format Shape"));  // another id
  CHECK(!ed.SetObjectXml(0, shape, "<xdr:twoCellAnchor><xdr:sp><xdr:nvSpPr><xdr:cNvPr id=\"2\"/>", u"Format Shape"));
  ed.SetJournal(nullptr);
  CHECK(ed.AnchorXml(d, shape).find("<a:solidFill><a:srgbClr val=\"C00000\"/></a:solidFill>") != std::string::npos);
  ed.Undo();
  CHECK(ed.AnchorXml(d, shape).find("C00000") == std::string::npos);
  ed.Redo();
  std::vector<uint8_t> saved = SaveBook(*book, ed);
  CleanZip(saved);
  const std::string drawingXml = PartOf(saved, d);
  CHECK(Count(drawingXml, "<xdr:twoCellAnchor") == 3 && Count(drawingXml, "editAs=\"oneCell\"") == 2);
  CHECK(PartOf(saved, "/xl/media/image1.png") == kPng && PartOf(saved, "/xl/media/image2.png").empty());
  const std::string rels = PartOf(saved, "/xl/drawings/_rels/drawing1.xml.rels");
  CHECK(Count(rels, "relationships/image\"") == 1);
  CHECK(drawingXml.find("descr=\"A dot\"") != std::string::npos);
  // The journal makes the same drawing on the file as it was opened.
  auto fresh = OpenParts({data, "<sheetData/>"});
  if (fresh) {
    Editor replay(*fresh);
    CHECK(replay.Replay(journal.All()));
    const std::string d2 = replay.SheetDrawing(0, false);
    CHECK(!d2.empty() && replay.AnchorXml(d2, shape).find("C00000") != std::string::npos && !replay.AnchorXml(d2, pic2).empty());
  }
  // Deleting the pictures takes the image with the last one.
  CHECK(ed.DeleteObjects(0, {pic1}));
  CHECK(!ed.PartBytes("/xl/media/image1.png").empty());
  CHECK(ed.DeleteObjects(0, {pic2}));
  CHECK(ed.PartBytes("/xl/media/image1.png").empty());
}
