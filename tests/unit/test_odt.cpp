// OpenDocument text (.odt) import (D5): the converter's .docx, read by the .docx reader.
#include "TestHarness.h"

#include <string>
#include <vector>

#include "DocTestUtil.h"
#include "doc/docx/Docx.h"
#include "doc/odf/Odt.h"
#include "pkg/Check.h"
#include "pkg/Zip.h"

using namespace fw::doc;
using namespace doc_test;

namespace {

constexpr const char* kOfficeNs =
    "xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
    "xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" "
    "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" "
    "xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" "
    "xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" "
    "xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" "
    "xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\" "
    "xmlns:xlink=\"http://www.w3.org/1999/xlink\"";

// A 1x1 PNG.
const unsigned char kPng[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44,
                              0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F,
                              0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8,
                              0xCF, 0xC0, 0x00, 0x00, 0x03, 0x01, 0x01, 0x00, 0xC9, 0xFE, 0x92, 0xEF, 0x00, 0x00, 0x00,
                              0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

// `layouts` and `masters`: more page layouts and master pages for styles.xml.
std::vector<uint8_t> Odt(const std::string& body, const std::string& autos = "", const std::string& prefix = "",
                         const std::string& layouts = "", const std::string& masters = "") {
  const std::string styles =
      std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?><office:document-styles ") + kOfficeNs + ">"
      "<office:font-face-decls><style:font-face style:name=\"Liberation Sans\" svg:font-family=\"'Liberation Sans'\"/>"
      "</office:font-face-decls><office:styles>"
      "<style:default-style style:family=\"paragraph\"><style:text-properties style:font-name=\"Liberation Sans\" fo:font-size=\"11pt\"/></style:default-style>"
      "<style:style style:name=\"Standard\" style:family=\"paragraph\"/>"
      "<style:style style:name=\"Heading_20_1\" style:display-name=\"Heading 1\" style:family=\"paragraph\" style:parent-style-name=\"Standard\" style:default-outline-level=\"1\">"
      "<style:text-properties fo:font-size=\"16pt\" fo:font-weight=\"bold\"/></style:style>"
      "<style:style style:name=\"Strong_20_Emphasis\" style:display-name=\"Strong Emphasis\" style:family=\"text\"><style:text-properties fo:font-weight=\"bold\"/></style:style>"
      "<text:list-style style:name=\"Numbering_20_123\"><text:list-level-style-number text:level=\"1\" style:num-suffix=\".\" style:num-format=\"1\"/>"
      "<text:list-level-style-number text:level=\"2\" style:num-suffix=\".\" style:num-format=\"a\"/></text:list-style>"
      "</office:styles><office:automatic-styles><style:page-layout style:name=\"pm1\"><style:page-layout-properties fo:page-width=\"21cm\" "
      "fo:page-height=\"29.7cm\" fo:margin-top=\"2cm\" fo:margin-bottom=\"2cm\" fo:margin-left=\"2cm\" fo:margin-right=\"2cm\"/></style:page-layout>" +
      layouts + "</office:automatic-styles><office:master-styles><style:master-page style:name=\"Standard\" style:page-layout-name=\"pm1\">"
      "<style:header><text:p>Header text <text:page-number>1</text:page-number></text:p></style:header></style:master-page>" + masters +
      "</office:master-styles>"
      "</office:document-styles>";
  const std::string content = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?><office:document-content ") + kOfficeNs + ">"
                              "<office:automatic-styles>" + autos + "</office:automatic-styles><office:body><office:text>" + body +
                              "</office:text></office:body></office:document-content>";
  fw::pkg::MemorySink sink;
  fw::pkg::ZipWriter zip(sink);
  const std::string mime = "application/vnd.oasis.opendocument.text";
  auto add = [&](const std::string& name, const std::string& text, bool store = false) {
    zip.Add(name, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()), nullptr,
            store ? fw::pkg::ZipWriter::Compression::Store : fw::pkg::ZipWriter::Compression::Deflate);
  };
  add("mimetype", mime, true);
  add("content.xml", prefix.empty() ? content : prefix);
  add("styles.xml", styles);
  zip.Add("Pictures/dot.png", std::span(kPng, sizeof(kPng)));
  zip.Finish();
  return std::move(sink.bytes);
}

std::unique_ptr<Document> Converted(const std::vector<uint8_t>& odt, std::string* documentXml = nullptr) {
  std::vector<uint8_t> docx;
  fw::pkg::Status st = odf::OdtToDocx(odt, docx);
  if (!st.Ok()) {
    std::printf("  OdtToDocx: %s\n", st.message.c_str());
    return nullptr;
  }
  auto pkg = test::OpenBytes(std::move(docx), st);
  if (!pkg) return nullptr;
  auto report = fw::pkg::CheckPackage(*pkg);
  for (const auto& e : report.errors) std::printf("  check: %s\n", e.c_str());
  if (!report.Ok()) return nullptr;
  if (documentXml) {
    std::vector<uint8_t> bytes;
    pkg->ReadPart("/word/document.xml", bytes);
    *documentXml = std::string(bytes.begin(), bytes.end());
  }
  return docx::Read(std::move(pkg), st);
}

std::vector<uint32_t> Main(const Document& doc) { return doc.StoryParagraphs(doc.MainStory()); }

std::u16string StoryText(const Document& doc, uint32_t story) {
  std::u16string out;
  for (uint32_t p : doc.StoryParagraphs(story)) out += doc.Para(p).text + u"|";
  return out;
}

}  // namespace

TEST(Odt_TextStylesListsTablesNotes) {
  const std::string autos =
      "<style:style style:name=\"P1\" style:family=\"paragraph\" style:parent-style-name=\"Standard\">"
      "<style:paragraph-properties fo:text-align=\"center\"/><style:text-properties fo:font-style=\"italic\"/></style:style>"
      "<style:style style:name=\"T1\" style:family=\"text\"><style:text-properties fo:color=\"#ff0000\"/></style:style>";
  const std::string body =
      "<text:h text:style-name=\"Heading_20_1\" text:outline-level=\"1\">A <text:bookmark text:name=\"mark\"/>title</text:h>"
      "<text:p text:style-name=\"P1\">Centred   <text:span text:style-name=\"T1\">red</text:span><text:s text:c=\"2\"/>and"
      "<text:tab/><text:span text:style-name=\"Strong_20_Emphasis\">strong</text:span><text:note text:note-class=\"footnote\">"
      "<text:note-citation>1</text:note-citation><text:note-body><text:p>Note text</text:p></text:note-body></text:note></text:p>"
      "<text:list text:style-name=\"Numbering_20_123\"><text:list-item><text:p>One</text:p>"
      "<text:list><text:list-item><text:p>Sub</text:p></text:list-item></text:list></text:list-item>"
      "<text:list-item><text:p>Two</text:p></text:list-item></text:list>"
      "<table:table><table:table-column table:number-columns-repeated=\"2\"/>"
      "<table:table-row><table:table-cell table:number-columns-spanned=\"2\"><text:p>Wide</text:p></table:table-cell>"
      "<table:covered-table-cell/></table:table-row>"
      "<table:table-row><table:table-cell><text:p>C</text:p></table:table-cell><table:table-cell><text:p>D</text:p></table:table-cell></table:table-row>"
      "</table:table>"
      "<text:p>See <text:a xlink:href=\"https://example.com/a b\">the site</text:a> and "
      "<draw:frame svg:width=\"2cm\" svg:height=\"1cm\" text:anchor-type=\"as-char\"><draw:image xlink:href=\"Pictures/dot.png\"/></draw:frame></text:p>";
  std::string xml;
  auto doc = Converted(Odt(body, autos), &xml);
  CHECK(doc);
  if (!doc) return;
  const std::u16string text = StoryText(*doc, doc->MainStory());
  CHECK(text.starts_with(u"A title|Centred red  and\tstrong"));
  CHECK(text.find(u"|One|Sub|Two|Wide|C|D|See the site and ") != std::u16string::npos);
  CHECK(xml.find("<w:pStyle w:val=\"Heading1\"/>") != std::string::npos && xml.find("w:name=\"mark\"") != std::string::npos);
  CHECK(xml.find("<w:jc w:val=\"center\"/>") != std::string::npos && xml.find("<w:color w:val=\"FF0000\"/>") != std::string::npos);
  CHECK(xml.find("<w:rStyle w:val=\"StrongEmphasis\"/>") != std::string::npos);
  CHECK(xml.find("<w:gridSpan w:val=\"2\"/>") != std::string::npos && xml.find("<w:hyperlink r:id=") != std::string::npos);
  // The list's items are numbered, the second level below the first.
  const std::vector<uint32_t> main = Main(*doc);
  int numbered = 0;
  for (uint32_t p : main)
    if (!doc->ResolvedPara(p).numId.empty() && doc->ResolvedPara(p).numId != "0") ++numbered;
  CHECK(numbered == 3);
  // The note, the header with its page number, the picture.
  const uint32_t note = doc->NoteStory(StoryKind::Footnote, "1");
  CHECK(note != kNone && StoryText(*doc, note).find(u"Note text") != std::u16string::npos);
  bool header = false;
  for (uint32_t s = 0; s < doc->Stories().size(); ++s)
    if (doc->Stories()[s].kind == StoryKind::Header && StoryText(*doc, s).starts_with(u"Header text")) header = true;
  CHECK(header);
  CHECK(doc->Drawings().size() == 1);
}

TEST(Odt_NoteInTextWithALanguage) {
  // The note's reference is raised among the run's properties in the schema's order: w:vertAlign
  // before w:lang (Word-written .odt files, found by the Open XML SDK validator).
  const std::string autos =
      "<style:style style:name=\"T1\" style:family=\"text\"><style:text-properties fo:language=\"fr\" fo:country=\"FR\"/></style:style>";
  const std::string body =
      "<text:p><text:span text:style-name=\"T1\">mot<text:note text:note-class=\"footnote\"><text:note-citation>1</text:note-citation>"
      "<text:note-body><text:p>Note</text:p></text:note-body></text:note></text:span></text:p>";
  std::string xml;
  auto doc = Converted(Odt(body, autos), &xml);
  CHECK(doc);
  const size_t ref = xml.find("<w:footnoteReference");
  const size_t rpr = xml.rfind("<w:rPr>", ref);
  const size_t vert = xml.find("<w:vertAlign w:val=\"superscript\"/>", rpr), lang = xml.find("<w:lang ", rpr);
  CHECK(ref != std::string::npos && vert < ref && lang < ref && vert < lang);
}

TEST(Odt_BulletInItsFont) {
  // Word's own .odt files have Symbol bullets (U+F0B7): the level keeps the bullet's font.
  const std::string autos =
      "<text:list-style style:name=\"L1\"><text:list-level-style-bullet text:level=\"1\" text:bullet-char=\"\xEF\x82\xB7\">"
      "<style:text-properties style:font-name=\"Symbol\"/></text:list-level-style-bullet></text:list-style>";
  const std::string body = "<text:list text:style-name=\"L1\"><text:list-item><text:p>Item</text:p></text:list-item></text:list>";
  std::vector<uint8_t> docx;
  CHECK(odf::OdtToDocx(Odt(body, autos), docx).Ok());
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(std::move(docx), st);
  CHECK(pkg != nullptr);
  if (!pkg) return;
  std::vector<uint8_t> bytes;
  CHECK(pkg->ReadPart("/word/numbering.xml", bytes).Ok());
  const std::string numbering(bytes.begin(), bytes.end());
  CHECK(numbering.find("<w:lvlText w:val=\"\xEF\x82\xB7\"/>") != std::string::npos);
  CHECK(numbering.find("</w:pPr><w:rPr><w:rFonts w:ascii=\"Symbol\"") != std::string::npos);
}

TEST(Odt_OtherPrefixesAndBadInput) {
  // Prefixes other than ODF's usual ones are read by their namespaces.
  const std::string content =
      "<?xml version=\"1.0\"?><o:document-content xmlns:o=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
      "xmlns:t=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\"><o:body><o:text><t:p>Hello</t:p></o:text></o:body>"
      "</o:document-content>";
  auto doc = Converted(Odt("", "", content));
  CHECK(doc && StoryText(*doc, doc->MainStory()) == u"Hello|");
  // Not an ODF file, or no content: refused plainly.
  std::vector<uint8_t> docx;
  const std::string junk = "not a zip";
  CHECK(!odf::OdtToDocx(std::span(reinterpret_cast<const uint8_t*>(junk.data()), junk.size()), docx).Ok());
  CHECK(odf::OdtToDocx(Odt("<text:p>x</text:p>"), docx).Ok() && !docx.empty());
}

TEST(Odt_PageStylesAreSectionsAndAnchoredPicturesFloat) {
  // The second paragraph starts the landscape page style "Wide", numbered from 5: the first section ends with
  // the first paragraph. Its picture is anchored to the paragraph, placed on the page, behind the text.
  const std::string autos =
      "<style:style style:name=\"P1\" style:family=\"paragraph\" style:master-page-name=\"Wide\">"
      "<style:paragraph-properties style:page-number=\"5\"/></style:style>"
      "<style:style style:name=\"fr1\" style:family=\"graphic\"><style:graphic-properties style:wrap=\"run-through\" "
      "style:run-through=\"background\" style:horizontal-rel=\"page\" style:vertical-rel=\"page\" "
      "style:horizontal-pos=\"from-left\" style:vertical-pos=\"from-top\"/></style:style>";
  const std::string body =
      "<text:p>One<draw:frame draw:style-name=\"fr1\" text:anchor-type=\"paragraph\" svg:x=\"1in\" svg:y=\"-0.5in\" "
      "svg:width=\"1in\" svg:height=\"1in\"><draw:image xlink:href=\"Pictures/dot.png\"/></draw:frame></text:p>"
      "<text:p text:style-name=\"P1\">Two</text:p>";
  const std::string layouts =
      "<style:page-layout style:name=\"pm2\"><style:page-layout-properties fo:page-width=\"29.7cm\" fo:page-height=\"21cm\" "
      "style:print-orientation=\"landscape\" fo:margin-top=\"1cm\" fo:margin-bottom=\"1cm\" fo:margin-left=\"1cm\" "
      "fo:margin-right=\"1cm\"/></style:page-layout>";
  const std::string masters = "<style:master-page style:name=\"Wide\" style:page-layout-name=\"pm2\"/>";
  std::string xml;
  auto doc = Converted(Odt(body, autos, "", layouts, masters), &xml);
  CHECK(doc);
  if (!doc) return;
  const std::u16string text = StoryText(*doc, doc->MainStory());  // the picture is an object in the first
  CHECK(text.starts_with(u"One") && text.ends_with(u"|Two|") && Main(*doc).size() == 2u);
  // Two sections: the first in the first paragraph, the landscape one at the end, numbered from 5.
  const size_t first = xml.find("<w:sectPr>"), second = xml.find("<w:sectPr>", first + 1);
  CHECK(first != std::string::npos && second != std::string::npos);
  if (second == std::string::npos) return;
  CHECK(xml.find("<w:sectPr>", second + 1) == std::string::npos);
  CHECK(first < xml.find("Two") && second > xml.find("Two"));
  CHECK(xml.find("w:orient=\"landscape\"", second) != std::string::npos && xml.find("w:orient=\"landscape\"") > second);
  CHECK(xml.find("<w:pgNumType w:start=\"5\"/>", second) != std::string::npos);
  CHECK(xml.find("<w:pageBreakBefore/>") == std::string::npos);
  // The picture floats: from the page's edges, behind the text, the negative offset kept.
  CHECK(xml.find("<wp:anchor ") != std::string::npos && xml.find("<wp:inline") == std::string::npos);
  CHECK(xml.find("behindDoc=\"1\"") != std::string::npos && xml.find("<wp:wrapNone/>") != std::string::npos);
  CHECK(xml.find("<wp:positionH relativeFrom=\"page\"><wp:posOffset>914400</wp:posOffset>") != std::string::npos);
  CHECK(xml.find("<wp:positionV relativeFrom=\"page\"><wp:posOffset>-457200</wp:posOffset>") != std::string::npos);
}

TEST(Odt_LetterSpacingAndScale) {
  // fo:letter-spacing and style:text-scale: w:spacing in twips and w:w, in rPr's order (after w:color).
  const std::string autos =
      "<style:style style:name=\"T1\" style:family=\"text\"><style:text-properties fo:color=\"#ff0000\" "
      "fo:letter-spacing=\"0.0027in\" style:text-scale=\"103%\" fo:font-size=\"10pt\"/></style:style>";
  std::string xml;
  auto doc = Converted(Odt("<text:p><text:span text:style-name=\"T1\">Wide</text:span></text:p>", autos), &xml);
  CHECK(doc);
  CHECK(xml.find("<w:color w:val=\"FF0000\"/><w:spacing w:val=\"4\"/><w:w w:val=\"103\"/><w:sz w:val=\"20\"/>") != std::string::npos);
}

TEST(Odt_FontTableFromFontFaces) {
  // The font-face declarations are the font table: family, pitch and PANOSE (all zeros says nothing), so a font
  // that isn't installed is substituted as Word does.
  const std::string content =
      std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?><office:document-content ") + kOfficeNs + ">"
      "<office:font-face-decls><style:font-face style:name=\"PN\" svg:font-family=\"'Proxima Nova Rg'\" "
      "style:font-family-generic=\"modern\" style:font-pitch=\"variable\" svg:panose-1=\"0 0 0 0 0 0 0 0 0 0\"/>"
      "<style:font-face style:name=\"Calibri\" svg:font-family=\"Calibri\" style:font-family-generic=\"swiss\" "
      "style:font-pitch=\"variable\" svg:panose-1=\"2 15 5 2 2 2 4 3 2 4\"/></office:font-face-decls>"
      "<office:body><office:text><text:p>x</text:p></office:text></office:body></office:document-content>";
  auto doc = Converted(Odt("", "", content));
  CHECK(doc);
  if (!doc) return;
  const FontInfo* pn = nullptr;
  const FontInfo* calibri = nullptr;
  for (const FontInfo& f : doc->Fonts()) {
    if (f.name == "Proxima Nova Rg") pn = &f;
    if (f.name == "Calibri") calibri = &f;
  }
  CHECK(pn && pn->family == "modern" && pn->pitch == "variable" && pn->panose.empty());
  CHECK(calibri && calibri->family == "swiss" && calibri->panose == "020F0502020204030204");
}

TEST(Odt_TextReferencesAndParagraphBorders) {
  // A reference to a bookmark's text shows the text (a REF field), with the references in it shown as they
  // show; one to its number keeps what the file has. A border's padding is its w:space.
  const std::string autos =
      "<style:style style:name=\"P1\" style:family=\"paragraph\"><style:paragraph-properties "
      "fo:border=\"0.5pt solid #000000\" fo:padding=\"0.1in\" fo:border-left=\"none\"/></style:style>";
  const std::string body =
      "<text:p><text:bookmark-start text:name=\"inner\"/>Inner<text:bookmark-end text:name=\"inner\"/></text:p>"
      "<text:p><text:bookmark-start text:name=\"_Ref1\"/>Target  text, see "
      "<text:bookmark-ref text:ref-name=\"inner\" text:reference-format=\"text\">1</text:bookmark-ref>"
      "<text:bookmark-end text:name=\"_Ref1\"/></text:p>"
      "<text:p text:style-name=\"P1\">Clause <text:bookmark-ref text:ref-name=\"_Ref1\" text:reference-format=\"text\">7.4</text:bookmark-ref>"
      " and <text:bookmark-ref text:ref-name=\"_Ref1\" text:reference-format=\"number\">7.5</text:bookmark-ref></text:p>";
  std::string xml;
  auto doc = Converted(Odt(body, autos), &xml);
  CHECK(doc);
  if (!doc) return;
  CHECK(xml.find("<w:fldSimple w:instr=\" REF _Ref1 \\h \"><w:r><w:t xml:space=\"preserve\">Target text, see Inner</w:t>") !=
        std::string::npos);
  CHECK(xml.find(">7.5<") != std::string::npos && xml.find(">7.4<") == std::string::npos);
  CHECK(xml.find("<w:pBdr><w:top w:val=\"single\" w:sz=\"4\" w:space=\"7\" w:color=\"000000\"/><w:left w:val=\"nil\"/>") !=
        std::string::npos);
}

TEST(Odt_TableWidthPaddingRowsAndPageNumberFormat) {
  // A table half the text's width (17 cm): its two 1-inch columns fitted to it. Its cells' padding is their
  // margins; its row stays on one page, at least half an inch high. The second page style numbers in roman.
  const std::string autos =
      "<style:style style:name=\"Tb\" style:family=\"table\"><style:table-properties style:width=\"2in\" style:rel-width=\"50%\" "
      "fo:margin-left=\"0.25in\"/></style:style>"
      "<style:style style:name=\"Col\" style:family=\"table-column\"><style:table-column-properties style:column-width=\"1in\"/></style:style>"
      "<style:style style:name=\"Row\" style:family=\"table-row\"><style:table-row-properties fo:keep-together=\"always\" "
      "style:min-row-height=\"0.5in\"/></style:style>"
      "<style:style style:name=\"Cell\" style:family=\"table-cell\"><style:table-cell-properties fo:padding=\"0in\" "
      "fo:padding-left=\"0.1in\"/></style:style>"
      "<style:style style:name=\"P1\" style:family=\"paragraph\" style:master-page-name=\"Roman\"/>";
  const std::string body =
      "<table:table table:style-name=\"Tb\"><table:table-column table:style-name=\"Col\" table:number-columns-repeated=\"2\"/>"
      "<table:table-row table:style-name=\"Row\"><table:table-cell table:style-name=\"Cell\"><text:p>A</text:p></table:table-cell>"
      "<table:table-cell><text:p>B</text:p></table:table-cell></table:table-row></table:table>"
      "<text:p text:style-name=\"P1\">Next</text:p>";
  const std::string layouts =
      "<style:page-layout style:name=\"pm3\"><style:page-layout-properties fo:page-width=\"21cm\" fo:page-height=\"29.7cm\" "
      "style:num-format=\"i\" fo:margin-left=\"2cm\" fo:margin-right=\"2cm\"/></style:page-layout>";
  const std::string masters = "<style:master-page style:name=\"Roman\" style:page-layout-name=\"pm3\"/>";
  std::string xml;
  auto doc = Converted(Odt(body, autos, "", layouts, masters), &xml);
  CHECK(doc);
  if (!doc) return;
  CHECK(xml.find("<w:tblW w:w=\"2500\" w:type=\"pct\"/><w:tblInd w:w=\"360\" w:type=\"dxa\"/>") != std::string::npos);
  CHECK(xml.find("<w:gridCol w:w=\"2409\"/><w:gridCol w:w=\"2409\"/>") != std::string::npos);  // 9638 / 2 / 2
  CHECK(xml.find("<w:trPr><w:cantSplit/><w:trHeight w:val=\"720\" w:hRule=\"atLeast\"/></w:trPr>") != std::string::npos);
  CHECK(xml.find("<w:tcMar><w:top w:w=\"0\" w:type=\"dxa\"/><w:left w:w=\"144\" w:type=\"dxa\"/>") != std::string::npos);
  CHECK(xml.find("<w:pgNumType w:fmt=\"lowerRoman\"/>") != std::string::npos);
}
