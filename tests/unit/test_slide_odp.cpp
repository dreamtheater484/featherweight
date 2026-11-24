// .odp import (P3, p3-files; src/slide/odp): a small presentation as PowerPoint writes one,
// converted and read by the presentation model: slides, the master page as a layout, placeholders,
// text with its formats, lists, custom shapes, tables, groups, connectors, pictures and fields.
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "TestHarness.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/Zip.h"
#include "slide/Presentation.h"
#include "slide/Text.h"
#include "slide/odp/Odp.h"

using namespace fw;

namespace {

constexpr std::string_view kNs =
    " xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\""
    " xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\""
    " xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\""
    " xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\""
    " xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\""
    " xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\""
    " xmlns:xlink=\"http://www.w3.org/1999/xlink\""
    " xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\""
    " xmlns:presentation=\"urn:oasis:names:tc:opendocument:xmlns:presentation:1.0\"";

const unsigned char kPng[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00,
                              0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00,
                              0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01,
                              0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

const std::string kStyles = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?><office:document-styles") + std::string(kNs) +
    "><office:styles>"
    "<style:default-style style:family=\"graphic\"><style:text-properties fo:font-size=\"18pt\"/></style:default-style>"
    "<draw:marker draw:name=\"arrow\" svg:viewBox=\"0 0 20 30\" svg:d=\"m10 0-10 30h20z\"/>"
    "</office:styles><office:automatic-styles>"
    "<style:page-layout style:name=\"pm1\"><style:page-layout-properties fo:page-width=\"10in\" fo:page-height=\"5.625in\"/></style:page-layout>"
    "<style:style style:family=\"drawing-page\" style:name=\"a1\"><style:drawing-page-properties draw:fill=\"solid\" draw:fill-color=\"#112233\"/></style:style>"
    "<style:style style:family=\"presentation\" style:name=\"a2\"><style:graphic-properties draw:fill=\"none\" draw:stroke=\"none\" "
    "draw:textarea-vertical-align=\"middle\"/></style:style>"
    "<style:style style:family=\"text\" style:name=\"a3\"><style:text-properties fo:font-size=\"44pt\" fo:color=\"#FFFFFF\"/></style:style>"
    "</office:automatic-styles><office:master-styles>"
    "<style:master-page style:name=\"Master1-Layout1-title-Title-Slide\" style:page-layout-name=\"pm1\" draw:style-name=\"a1\">"
    "<draw:frame presentation:style-name=\"a2\" svg:x=\"0.5in\" svg:y=\"0.5in\" svg:width=\"9in\" svg:height=\"1in\" "
    "presentation:class=\"title\" presentation:placeholder=\"false\"><draw:text-box><text:p><text:span text:style-name=\"a3\">"
    "Click to edit Master title style</text:span></text:p></draw:text-box></draw:frame>"
    "</style:master-page></office:master-styles></office:document-styles>";

const std::string kContent = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?><office:document-content") + std::string(kNs) +
    "><office:automatic-styles>"
    "<style:style style:family=\"drawing-page\" style:name=\"a1\"><style:drawing-page-properties draw:fill=\"solid\" "
    "draw:fill-color=\"#FFFFFF\"/></style:style>"
    "<style:style style:family=\"drawing-page\" style:name=\"a2\"><style:drawing-page-properties presentation:visibility=\"hidden\"/></style:style>"
    "<style:style style:family=\"presentation\" style:name=\"a3\"><style:graphic-properties draw:fill=\"none\" draw:stroke=\"none\" "
    "fo:padding-left=\"0.1in\" draw:textarea-vertical-align=\"bottom\"/></style:style>"
    "<style:style style:family=\"text\" style:name=\"a4\"><style:text-properties fo:font-family=\"'Segoe UI'\" fo:font-size=\"0.33333in\" "
    "fo:font-weight=\"bold\" fo:color=\"#0000FF\" fo:language=\"nl\" fo:country=\"NL\"/></style:style>"
    "<style:style style:family=\"text\" style:name=\"a5\"><style:text-properties fo:font-size=\"20pt\" fo:font-style=\"italic\" "
    "style:text-underline-style=\"solid\" style:text-underline-type=\"single\"/></style:style>"
    "<style:style style:family=\"paragraph\" style:name=\"a6\"><style:paragraph-properties fo:margin-left=\"0.5in\" fo:text-indent=\"-0.25in\" "
    "fo:text-align=\"center\" fo:line-height=\"120%\" fo:margin-top=\"6pt\"/></style:style>"
    "<text:list-style style:name=\"a7\"><text:list-level-style-bullet text:level=\"1\" text:bullet-char=\"\xE2\x80\xA2\">"
    "<style:list-level-properties text:space-before=\"0.25in\" text:min-label-width=\"0.25in\"/>"
    "<style:text-properties fo:color=\"#FF0000\" fo:font-size=\"75%\"/></text:list-level-style-bullet>"
    "<text:list-level-style-number text:level=\"2\" style:num-format=\"1\" style:num-suffix=\".\"/></text:list-style>"
    "<style:style style:family=\"graphic\" style:name=\"a8\"><style:graphic-properties draw:fill=\"solid\" draw:fill-color=\"#FF0000\" "
    "draw:opacity=\"50%\" draw:stroke=\"solid\" svg:stroke-width=\"0.02in\" svg:stroke-color=\"#00FF00\"/></style:style>"
    "<style:style style:family=\"graphic\" style:name=\"a9\"><style:graphic-properties draw:stroke=\"solid\" svg:stroke-width=\"1pt\" "
    "svg:stroke-color=\"#000000\" draw:marker-end=\"arrow\" draw:marker-end-width=\"3pt\"/></style:style>"
    "<style:style style:family=\"table-cell\" style:name=\"a10\"><style:table-cell-properties fo:background-color=\"#CCCCCC\" "
    "fo:border=\"1pt solid #000000\" style:vertical-align=\"middle\"/></style:style>"
    "<style:style style:family=\"table-column\" style:name=\"a11\"><style:table-column-properties style:column-width=\"2in\"/></style:style>"
    "</office:automatic-styles><office:body><office:presentation>"
    // Slide 1
    "<draw:page draw:name=\"Slide1\" draw:style-name=\"a1\" draw:master-page-name=\"Master1-Layout1-title-Title-Slide\">"
    "<draw:frame draw:name=\"Title 1\" presentation:style-name=\"a3\" svg:x=\"1in\" svg:y=\"0.5in\" svg:width=\"8in\" svg:height=\"1in\" "
    "presentation:class=\"title\" presentation:placeholder=\"false\"><draw:text-box><text:p>"
    "<text:span text:style-name=\"a4\">Hello  \n world<text:s text:c=\"2\"/></text:span><text:span text:style-name=\"a5\">&lt;&amp;&gt;</text:span>"
    "<text:span text:style-name=\"a5\"/></text:p></draw:text-box></draw:frame>"
    "<draw:custom-shape draw:name=\"Oval 2\" draw:style-name=\"a8\" svg:x=\"1in\" svg:y=\"2in\" svg:width=\"2in\" svg:height=\"1in\">"
    "<text:p>In the oval</text:p>"
    "<draw:enhanced-geometry svg:viewBox=\"0 0 21600 21600\" draw:enhanced-path=\"M 0 ?f0 A 0 ?f0 0 ?f0 0 ?f0 1 ?f1 W 0 0 21600 21600 0 ?f0 1 ?f1 Z N\" "
    "draw:text-areas=\"?f2 ?f2 ?f3 ?f3\"><draw:equation draw:name=\"f0\" draw:formula=\"bottom / 2\"/>"
    "<draw:equation draw:name=\"f1\" draw:formula=\"?f0 + 157\"/><draw:equation draw:name=\"f2\" draw:formula=\"3163\"/>"
    "<draw:equation draw:name=\"f3\" draw:formula=\"right - ?f2\"/></draw:enhanced-geometry></draw:custom-shape>"
    "<draw:frame draw:name=\"List 3\" svg:x=\"4in\" svg:y=\"2in\" svg:width=\"5in\" svg:height=\"2in\"><draw:text-box>"
    "<text:list text:style-name=\"a7\"><text:list-item><text:p text:style-name=\"a6\">First</text:p></text:list-item></text:list>"
    "<text:list text:style-name=\"a7\"><text:list-item><text:list><text:list-item><text:p>Second</text:p></text:list-item></text:list>"
    "</text:list-item></text:list></draw:text-box></draw:frame>"
    "<draw:frame draw:name=\"Table 4\" svg:x=\"1in\" svg:y=\"3.5in\" svg:width=\"4in\" svg:height=\"1in\"><table:table>"
    "<table:table-column table:style-name=\"a11\"/><table:table-column table:style-name=\"a11\"/>"
    "<table:table-row><table:table-cell table:style-name=\"a10\" table:number-columns-spanned=\"2\"><text:p>Both</text:p></table:table-cell>"
    "<table:covered-table-cell/></table:table-row>"
    "<table:table-row><table:table-cell><text:p>A</text:p></table:table-cell><table:table-cell><text:p>B</text:p></table:table-cell>"
    "</table:table-row></table:table></draw:frame>"
    "<draw:g draw:name=\"Group 5\">"
    "<draw:custom-shape draw:style-name=\"a8\" svg:x=\"6in\" svg:y=\"4in\" svg:width=\"1in\" svg:height=\"1in\">"
    "<draw:enhanced-geometry svg:viewBox=\"0 0 21600 21600\" draw:enhanced-path=\"M 0 0 L 21600 0 21600 21600 0 21600 Z N\"/></draw:custom-shape>"
    "<draw:custom-shape draw:style-name=\"a8\" svg:width=\"1in\" svg:height=\"0.5in\" draw:transform=\"rotate (1.5707963) translate (7in 5in)\">"
    "<draw:enhanced-geometry svg:viewBox=\"0 0 21600 21600\" draw:enhanced-path=\"M 0 0 L 21600 0 21600 21600 0 21600 Z N\"/></draw:custom-shape>"
    "</draw:g>"
    "<draw:connector draw:name=\"Arrow 6\" draw:style-name=\"a9\" draw:type=\"line\" svg:x1=\"9in\" svg:y1=\"5in\" svg:x2=\"8in\" svg:y2=\"4.5in\"/>"
    "<draw:frame draw:name=\"Picture 7\" svg:x=\"0in\" svg:y=\"0in\" svg:width=\"1in\" svg:height=\"1in\"><draw:image xlink:href=\"media/image1.png\"/>"
    "<svg:desc>A dot</svg:desc></draw:frame>"
    "</draw:page>"
    // Slide 2: hidden, a slide number field.
    "<draw:page draw:name=\"Slide2\" draw:style-name=\"a2\" draw:master-page-name=\"Master1-Layout1-title-Title-Slide\">"
    "<draw:frame svg:x=\"1in\" svg:y=\"1in\" svg:width=\"2in\" svg:height=\"1in\"><draw:text-box><text:p>Slide "
    "<text:page-number>2</text:page-number></text:p></draw:text-box></draw:frame></draw:page>"
    "</office:presentation></office:body></office:document-content>";

std::vector<uint8_t> MakeOdp(std::string_view mime = "application/vnd.oasis.opendocument.presentation") {
  pkg::MemorySink sink;
  pkg::ZipWriter zip(sink);
  auto put = [&](std::string_view name, std::string_view text) {
    zip.Add(name, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()), nullptr, pkg::ZipWriter::Compression::Store);
  };
  put("mimetype", mime);
  put("content.xml", kContent);
  put("styles.xml", kStyles);
  zip.Add("media/image1.png", std::span<const uint8_t>(kPng, sizeof kPng));
  zip.Finish();
  return std::move(sink.bytes);
}

std::wstring TextOf(const slide::TextBody& t, size_t paragraph) {
  std::wstring s;
  if (paragraph < t.paragraphs.size())
    for (const slide::TextRun& r : t.paragraphs[paragraph].runs) s += r.kind == slide::TextRun::Kind::Break ? L"\n" : r.text;
  return s;
}

}  // namespace

TEST(slide_odp_converts_a_presentation) {
  std::vector<uint8_t> pptx;
  pkg::Status st = slide::odp::OdpToPptx(MakeOdp(), pptx);
  CHECK(st.Ok());
  auto package = pkg::Package::Open(pkg::Snapshot::FromBytes(std::move(pptx)), pkg::Limits::Default(), st);
  CHECK(package != nullptr);
  if (!package) return;
  auto deck = slide::Read(std::move(package), st);
  CHECK(deck != nullptr);
  if (!deck) return;
  CHECK_EQ(deck->SlideCount(), size_t{2});
  CHECK(std::abs(deck->SlideSize().width - 720) < 0.01 && std::abs(deck->SlideSize().height - 405) < 0.01);
  CHECK_EQ(deck->Layouts().size(), size_t{1});
  CHECK(deck->Layouts()[0]->name == "Title Slide");
  CHECK(deck->Layouts()[0]->FindPlaceholder(slide::PlaceholderType::Title) != nullptr);
  CHECK(deck->Layouts()[0]->root.child("p:cSld").child("p:bg"));

  const slide::Slide* s = deck->GetSlide(0);
  CHECK(s && !s->hidden && s->shapes.size() == 7);
  if (!s || s->shapes.size() != 7) return;
  using Kind = slide::Shape::Kind;
  // The title: a placeholder, its text's white space collapsed, its runs' formats.
  const slide::Shape& title = s->shapes[0];
  CHECK(title.placeholder.type == slide::PlaceholderType::Title && title.name == "Title 1");
  const slide::TextBody* t = deck->Text(title, *s);
  CHECK(t && t->paragraphs.size() == 1);
  if (t) {
    CHECK(TextOf(*t, 0) == L"Hello world  <&>");
    const slide::RunProps& r = t->paragraphs[0].runs[0].props;
    CHECK(r.bold && std::abs(r.size - 24) < 0.01 && r.latin == L"Segoe UI" && r.lang == "nl-NL");
    CHECK(std::abs(r.color.b - 1) < 0.01 && r.color.r < 0.01);
    const slide::RunProps& r2 = t->paragraphs[0].runs.back().props;
    CHECK(r2.italic && r2.underline == slide::Underline::Single && std::abs(r2.size - 20) < 0.01);
    CHECK(std::abs(t->paragraphs[0].endProps.size - 20) < 0.01);
    CHECK(t->body.anchor == slide::BodyProps::Anchor::Bottom && std::abs(t->body.left - 7.2f) < 0.01);
  }
  // The oval: its path drawn with arcs, its text in the text area.
  const slide::Shape& oval = s->shapes[1];
  CHECK(oval.kind == Kind::Shape);
  pugi::xml_node geom = oval.node.child("p:spPr").child("a:custGeom");
  CHECK(geom.child("a:pathLst").child("a:path").child("a:cubicBezTo"));
  CHECK(std::string_view(geom.child("a:pathLst").child("a:path").child("a:moveTo").child("a:pt").attribute("y").value()) == "10800");
  CHECK(std::string_view(geom.child("a:rect").attribute("l").value()) == "267801");  // 3163 of 21600 of 2 in
  CHECK(std::string_view(oval.node.child("p:spPr").child("a:solidFill").child("a:srgbClr").child("a:alpha").attribute("val").value()) == "50000");
  // The list: bullets from the list style, the paragraph's own indents, a numbered second level.
  const slide::TextBody* list = deck->Text(s->shapes[2], *s);
  CHECK(list && list->paragraphs.size() == 2);
  if (list && list->paragraphs.size() == 2) {
    const slide::ParagraphProps& p = list->paragraphs[0].props;
    CHECK(p.bullet.kind == slide::Bullet::Kind::Char && p.bullet.character == L"\x2022");
    CHECK(std::abs(p.bullet.sizePercent - 0.75f) < 0.01 && p.bullet.color && p.bullet.color->r > 0.99);
    CHECK(std::abs(p.marginLeft - 36) < 0.01 && std::abs(p.indent + 18) < 0.01 && p.align == slide::TextAlign::Center);
    CHECK(p.lineSpacing.percent && std::abs(p.lineSpacing.value - 1.2f) < 0.01);
    const slide::ParagraphProps& q = list->paragraphs[1].props;
    CHECK(q.level == 1 && q.bullet.kind == slide::Bullet::Kind::AutoNumber && q.bullet.scheme == "arabicPeriod");
  }
  // The table: a merged first row.
  CHECK(s->shapes[3].kind == Kind::Table);
  if (const slide::Table* table = deck->GetTable(s->shapes[3], *s)) {
    CHECK(table->rows.size() == 2 && table->columns.size() == 2);
    CHECK(table->Cell(0, 0) && table->Cell(0, 0)->columnSpan == 2);
  } else {
    CHECK(false);
  }
  // The group: around both shapes, the turned one about its centre.
  const slide::Shape& group = s->shapes[4];
  CHECK(group.kind == Kind::Group && group.children.size() == 2);
  if (group.children.size() == 2) CHECK(std::abs(group.children[1].rotation - 270) < 0.01);
  // The connector, flipped, with its arrowhead; the picture with its alt text.
  CHECK(s->shapes[5].kind == Kind::Connector && s->shapes[5].flipH && s->shapes[5].flipV);
  CHECK(std::string_view(s->shapes[5].node.child("p:spPr").child("a:ln").child("a:tailEnd").attribute("type").value()) == "triangle");
  CHECK(s->shapes[6].kind == Kind::Picture && s->shapes[6].description == "A dot");

  const slide::Slide* hidden = deck->GetSlide(1);
  CHECK(hidden && hidden->hidden);
  if (hidden && !hidden->shapes.empty()) {
    const slide::TextBody* f = deck->Text(hidden->shapes[0], *hidden);
    CHECK(f && f->paragraphs.size() == 1 && f->paragraphs[0].runs.back().fieldType == "slidenum");
  }
}

TEST(slide_odp_refuses_what_isnt_a_presentation) {
  std::vector<uint8_t> pptx;
  CHECK(slide::odp::OdpToPptx(MakeOdp("application/vnd.oasis.opendocument.spreadsheet"), pptx).code == pkg::Status::Code::Unsupported);
  const std::string junk = "not a zip at all";
  CHECK(!slide::odp::OdpToPptx(std::span(reinterpret_cast<const uint8_t*>(junk.data()), junk.size()), pptx).Ok());
  CHECK(pptx.empty());
}
