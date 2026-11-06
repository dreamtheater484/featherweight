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

std::vector<uint8_t> Odt(const std::string& body, const std::string& autos = "", const std::string& prefix = "") {
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
      "fo:page-height=\"29.7cm\" fo:margin-top=\"2cm\" fo:margin-bottom=\"2cm\" fo:margin-left=\"2cm\" fo:margin-right=\"2cm\"/></style:page-layout>"
      "</office:automatic-styles><office:master-styles><style:master-page style:name=\"Standard\" style:page-layout-name=\"pm1\">"
      "<style:header><text:p>Header text <text:page-number>1</text:page-number></text:p></style:header></style:master-page></office:master-styles>"
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
