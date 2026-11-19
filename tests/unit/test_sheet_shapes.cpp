// Pictures and shapes (s3-data): Excel's anchors for new ones, and their formatting read and
// written (sheet/data/Shapes.h).

#include "TestHarness.h"

#include <string>

#include <pugixml.hpp>

#include "sheet/data/Shapes.h"

using namespace fw::sheet;
using data::ObjectFormat;
using data::ShapeKind;

namespace {
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
