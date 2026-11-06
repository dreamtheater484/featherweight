// Saving Word documents (plan §6.3 "Saving", D2 "Files"): a new document from the built-in
// template, a save with no edits that is the file as opened, and a save after edits that rewrites
// only the edited parts and the document properties.
#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "PkgTestUtil.h"
#include "TestHarness.h"
#include "doc/docx/Blank.h"
#include "doc/docx/Docx.h"
#include "doc/docx/Save.h"
#include "doc/edit/Forms.h"
#include "pkg/Check.h"
#include "pkg/XmlDom.h"
#include "pkg/Zip.h"

namespace {

int64_t FileTimeOf(WORD y, WORD mo, WORD d, WORD h, WORD mi, WORD s) {
  SYSTEMTIME st{y, mo, 0, d, h, mi, s, 0};
  FILETIME ft{};
  SystemTimeToFileTime(&st, &ft);
  return static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

std::unique_ptr<fw::doc::Document> ReadDoc(std::vector<uint8_t> bytes) {
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(std::move(bytes), st);
  return pkg ? fw::doc::docx::Read(std::move(pkg), st) : nullptr;
}

std::string PartText(const fw::pkg::Package& pkg, std::string_view name) {
  std::vector<uint8_t> bytes;
  return pkg.ReadPart(name, bytes).Ok() ? test::Text(bytes) : std::string();
}

uint32_t MainPart(const fw::doc::Document& doc) {
  return static_cast<uint32_t>(doc.FindPart(fw::doc::PartKind::Document) - doc.Parts().data());
}

}  // namespace

TEST(Blank_IsAWordDocumentWithWordsStyles) {
  const int64_t now = FileTimeOf(2026, 11, 5, 14, 30, 12);
  auto bytes = fw::doc::docx::BlankDocx({"nl-NL", true, "Roy & co", now});
  auto doc = ReadDoc(bytes);
  CHECK(doc);
  if (!doc) return;
  CHECK_EQ(doc->Paragraphs().size(), 1u);
  for (const char* id : {"Normal", "DefaultParagraphFont", "TableNormal", "NoList", "Heading1", "Heading2", "Heading3",
                         "Title", "Subtitle", "ListParagraph", "Hyperlink"})
    CHECK(doc->FindStyle(id) != nullptr);
  const auto& normal = doc->ResolvedRun(0, fw::doc::Node());
  CHECK(normal.fonts.ascii == "Calibri");
  CHECK_EQ(normal.size.Value(), 22);
  const auto& pkg = doc->Package();
  CHECK(fw::pkg::CheckPackage(pkg).Ok());
  const std::string core = PartText(pkg, "/docProps/core.xml");
  CHECK(core.find("<dc:creator>Roy &amp; co</dc:creator>") != std::string::npos);
  CHECK(core.find(">2026-11-05T14:30:12Z</dcterms:created>") != std::string::npos);
  CHECK(PartText(pkg, "/word/styles.xml").find("w:lang w:val=\"nl-NL\"") != std::string::npos);
  CHECK(PartText(pkg, "/word/document.xml").find("w:w=\"11906\"") != std::string::npos);  // A4
  auto letter = ReadDoc(fw::doc::docx::BlankDocx());
  CHECK(letter && PartText(letter->Package(), "/word/document.xml").find("w:w=\"12240\"") != std::string::npos);
}

TEST(Blank_Templates) {
  using fw::doc::docx::Template;
  for (Template kind : {Template::Letter, Template::Cv, Template::Report}) {
    auto bytes = fw::doc::docx::BlankDocx({"en-GB", kind != Template::Cv, "Roy", 0, kind});
    auto doc = ReadDoc(bytes);
    CHECK(doc);
    if (!doc) continue;
    CHECK(fw::pkg::CheckPackage(doc->Package()).Ok());
    const std::vector<uint32_t> main = doc->StoryParagraphs(doc->MainStory());
    CHECK(main.size() > 10);
    CHECK(doc->Para(main.front()).text == (kind == Template::Report ? u"[Report title]" : u"[Your name]"));
    int bullets = 0;
    for (uint32_t p : main) bullets += doc->ResolvedPara(p).numId == "1";
    CHECK_EQ(bullets, kind == Template::Cv ? 5 : kind == Template::Report ? 2 : 0);
    // Each placeholder is a content control showing its placeholder: one click selects it, typing replaces it.
    const auto controls = fw::doc::edit::ContentControls(*doc, doc->MainStory());
    CHECK(controls.size() >= 5);
    for (const auto& c : controls) CHECK(c.placeholder && c.Typable());
    // The model writes it back as it was, and again after the body is rewritten.
    std::vector<uint8_t> same, rewritten;
    CHECK(fw::doc::docx::Serialize(*doc, {}, same).Ok() && same == bytes);
    fw::doc::docx::SaveOptions options;
    options.changedParts = {MainPart(*doc)};
    CHECK(fw::doc::docx::Serialize(*doc, options, rewritten).Ok());
    auto again = ReadDoc(rewritten);
    CHECK(again && fw::pkg::CheckPackage(again->Package()).Ok());
    if (!again) continue;
    const std::vector<uint32_t> main2 = again->StoryParagraphs(again->MainStory());
    CHECK_EQ(main2.size(), main.size());
    for (size_t i = 0; i < std::min(main.size(), main2.size()); ++i) CHECK(doc->Para(main[i]).text == again->Para(main2[i]).text);
  }
  // The report: a table with a repeating heading row, and the page number in the footer.
  auto report = ReadDoc(fw::doc::docx::BlankDocx({"en-US", false, "", 0, Template::Report}));
  CHECK(report);
  if (!report) return;
  CHECK(PartText(report->Package(), "/word/document.xml").find("<w:tblHeader/>") != std::string::npos);
  bool footer = false;
  for (const auto& s : report->Stories()) footer = footer || s.kind == fw::doc::StoryKind::Footer;
  CHECK(footer && PartText(report->Package(), "/word/footer1.xml").find(" PAGE ") != std::string::npos);
}

TEST(Blank_DocumentFromTemplate) {
  // A .dotx (a blank document with a template's type) and a .dotm become documents of their own type.
  std::vector<uint8_t> bytes = fw::doc::docx::BlankDocx();
  auto doc = ReadDoc(bytes);
  CHECK(doc);
  if (!doc) return;
  for (const char* type : {"application/vnd.openxmlformats-officedocument.wordprocessingml.template.main+xml",
                           "application/vnd.ms-word.template.macroEnabledTemplate.main+xml"}) {
    std::string types = PartText(doc->Package(), "/[Content_Types].xml");
    const std::string main = "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml";
    types.replace(types.find(main), main.size(), type);
    fw::pkg::MemorySink sink;
    fw::pkg::ZipWriter zip(sink);
    for (const auto& e : doc->Package().Zip().Entries()) {
      if (e.name == "[Content_Types].xml") zip.Add(e.name, std::span(reinterpret_cast<const uint8_t*>(types.data()), types.size()), &e);
      else zip.CopyRaw(doc->Package().Zip(), e);
    }
    zip.Finish();
    std::vector<uint8_t> docx;
    CHECK(fw::doc::docx::DocumentFromTemplate(sink.bytes, docx).Ok());
    auto made = ReadDoc(docx);
    CHECK(made && fw::pkg::CheckPackage(made->Package()).Ok());
    if (!made) continue;
    const std::string_view now = made->Package().ContentTypeOf("/word/document.xml");
    CHECK(now == (std::string_view(type).find("macro") != std::string_view::npos
                      ? "application/vnd.ms-word.document.macroEnabled.main+xml"
                      : main));
  }
  std::vector<uint8_t> out;
  const std::string junk = "not a zip";
  CHECK(!fw::doc::docx::DocumentFromTemplate(std::span(reinterpret_cast<const uint8_t*>(junk.data()), junk.size()), out).Ok());
}

TEST(Save_NoEditsIsTheFileAsOpened) {
  auto bytes = fw::doc::docx::BlankDocx();
  auto doc = ReadDoc(bytes);
  CHECK(doc);
  if (!doc) return;
  std::vector<uint8_t> out;
  CHECK(fw::doc::docx::Serialize(*doc, {}, out).Ok());
  CHECK(out == bytes);
}

TEST(Save_EditsRewriteOnlyTheirPartsAndTheProperties) {
  auto bytes = fw::doc::docx::BlankDocx({"en-US", false, "First Author", FileTimeOf(2025, 1, 2, 3, 4, 5)});
  auto doc = ReadDoc(bytes);
  CHECK(doc);
  if (!doc) return;
  fw::doc::docx::SaveOptions options;
  options.changedParts = {MainPart(*doc)};
  options.edited = true;
  options.author = "Ann";
  options.now = FileTimeOf(2026, 11, 5, 9, 8, 7);
  std::vector<uint8_t> out;
  CHECK(fw::doc::docx::Serialize(*doc, options, out).Ok());
  CHECK(out != bytes);

  fw::pkg::Status st;
  auto saved = test::OpenBytes(out, st);
  CHECK(saved);
  if (!saved) return;
  const std::string core = PartText(*saved, "/docProps/core.xml");
  CHECK(core.find("<cp:lastModifiedBy>Ann</cp:lastModifiedBy>") != std::string::npos);
  CHECK(core.find("<dc:creator>First Author</dc:creator>") != std::string::npos);
  CHECK(core.find("<dcterms:modified xsi:type=\"dcterms:W3CDTF\">2026-11-05T09:08:07Z</dcterms:modified>") !=
        std::string::npos);
  CHECK(core.find(">2025-01-02T03:04:05Z</dcterms:created>") != std::string::npos);
  // Untouched parts are the same records, byte for byte.
  auto original = test::OpenBytes(bytes, st);
  std::vector<uint8_t> a, b;
  for (const char* name : {"word/styles.xml", "word/settings.xml", "_rels/.rels"}) {
    const auto* ea = original->Zip().Find(name);
    const auto* eb = saved->Zip().Find(name);
    CHECK(ea && eb && original->Zip().ReadRecord(*ea, a).Ok() && saved->Zip().ReadRecord(*eb, b).Ok() && a == b);
  }
  CHECK(fw::pkg::CheckPackage(*saved).Ok());
  auto again = ReadDoc(out);
  CHECK(again && again->Paragraphs().size() == 1);

  // Saved again with nothing new: the edits made before are kept.
  std::vector<uint8_t> second;
  CHECK(fw::doc::docx::Serialize(*doc, {}, second).Ok());
  CHECK(PartText(*test::OpenBytes(second, st), "/docProps/core.xml").find("Ann") != std::string::npos);
}

TEST(Save_CorePropertiesKeepEverythingElse) {
  using fw::doc::docx::UpdateCoreProperties;
  const int64_t now = FileTimeOf(2026, 11, 5, 14, 30, 12);
  std::string out;
  // Other prefixes, a default namespace, unknown content, and both elements missing.
  const std::string odd =
      "<?xml version=\"1.0\"?><coreProperties xmlns=\"http://schemas.openxmlformats.org/package/2006/metadata/"
      "core-properties\" xmlns:t=\"http://purl.org/dc/terms/\"><keywords>a, b</keywords><t:created>2020</t:created>"
      "<x:y xmlns:x=\"urn:x\">kept</x:y></coreProperties>";
  CHECK(UpdateCoreProperties(odd, "Bo", now, out).Ok());
  CHECK(out.find("<keywords>a, b</keywords>") != std::string::npos);
  CHECK(out.find("<x:y xmlns:x=\"urn:x\">kept</x:y>") != std::string::npos);
  CHECK(out.find("<lastModifiedBy>Bo</lastModifiedBy>") != std::string::npos);
  CHECK(out.find("<t:modified xsi:type=\"t:W3CDTF\">2026-11-05T14:30:12Z</t:modified>") != std::string::npos);
  CHECK(out.find("xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"") != std::string::npos);
  CHECK(fw::pkg::XmlDom().Parse(out).Ok());
  // Existing values are replaced; an empty author leaves the old one.
  const std::string usual =
      "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
      "xmlns:dcterms=\"http://purl.org/dc/terms/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
      "<cp:lastModifiedBy>Old</cp:lastModifiedBy><dcterms:modified xsi:type=\"dcterms:W3CDTF\">2001-01-01T00:00:00Z"
      "</dcterms:modified></cp:coreProperties>";
  CHECK(UpdateCoreProperties(usual, "", now, out).Ok());
  CHECK(out.find("<cp:lastModifiedBy>Old</cp:lastModifiedBy>") != std::string::npos);
  CHECK(out.find("2001-01-01") == std::string::npos);
  // Not core properties: refused, so the part stays as it was.
  CHECK(!UpdateCoreProperties("<a/>", "Bo", now, out).Ok());
  CHECK(!UpdateCoreProperties("<a", "Bo", now, out).Ok());
}
