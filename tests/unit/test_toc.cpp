// Tables of contents (D4): inserted as Word writes one, its fields never split across a link
// (torture finding 6), page numbers or the whole table updated, each one undo step that puts the
// document back exactly, and the saved package passes the checks Office's rules make.
#include "TestHarness.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "DocFixture.h"
#include "doc/docx/Save.h"
#include "doc/edit/Editor.h"
#include "doc/edit/Inline.h"
#include "doc/edit/Toc.h"
#include "doc/model/Utf.h"
#include "pkg/Check.h"

#ifndef REQUIRE
#define REQUIRE(cond)   do { if (!(cond)) { test::Fail(__FILE__, __LINE__, #cond); return; } } while (0)
#endif

using namespace fw;
using namespace fw::doc;
using namespace docfix;
namespace edit = fw::doc::edit;

namespace {

std::vector<uint32_t> MainParas(const Document& doc) { return doc.StoryParagraphs(doc.MainStory()); }

std::string PartXml(const Document& doc, PartKind kind) {
  for (uint32_t i = 0; i < doc.Parts().size(); ++i) {
    if (doc.Parts()[i].kind != kind) continue;
    std::string out;
    if (!docx::WritePart(doc, i, out).Ok()) return "ERROR";
    return out;
  }
  return "";
}

std::string MainXml(const Document& doc) { return PartXml(doc, PartKind::Document); }
bool Has(const std::string& xml, std::string_view what) { return xml.find(what) != std::string::npos; }
size_t Count(const std::string& xml, std::string_view what) {
  size_t n = 0;
  for (size_t at = xml.find(what); at != std::string::npos; at = xml.find(what, at + 1)) ++n;
  return n;
}

// With FW_TEST_OUT set, the saved packages are kept there too, for the Office SDK's validator.
void Keep(const std::vector<uint8_t>& bytes) {
  char* dir = nullptr;
  size_t len = 0;
  if (_dupenv_s(&dir, &len, "FW_TEST_OUT") != 0 || !dir) return;
  static int n = 0;
  const std::string path = std::string(dir) + "/toc-" + std::to_string(++n) + ".docx";
  free(dir);
  FILE* f = nullptr;
  if (fopen_s(&f, path.c_str(), "wb") == 0 && f) {
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
  }
}

bool SavesAndChecks(Document& doc) {
  if (!docx::WriteAllParts(doc).Ok()) return false;
  fw::pkg::MemorySink sink;
  if (!doc.Package().Save(sink).Ok()) return false;
  Keep(sink.bytes);
  Status st;
  auto pkg = test::OpenBytes(std::move(sink.bytes), st);
  if (!pkg) return false;
  fw::pkg::CheckReport report = fw::pkg::CheckPackage(*pkg);
  for (const std::string& e : report.errors) std::printf("  check: %s\n", e.c_str());
  return report.Ok() && docx::Read(std::move(pkg), st) != nullptr;
}

// The link a field character is in (null outside links).
Node LinkOf(Node n) {
  for (; n && !n.Is(Name("p")); n = n.Parent())
    if (n.Is(Name("hyperlink"))) return n;
  return {};
}

// Every field's begin, separator and end are in the same link, or all outside links.
bool NoFieldAcrossALink(const Document& doc) {
  for (const Field& f : doc.Fields(doc.MainStory())) {
    if (f.simple) continue;
    const Node link = LinkOf(f.beginNode);
    if (LinkOf(f.endNode).Id() != link.Id() || (f.separatorNode && LinkOf(f.separatorNode).Id() != link.Id())) {
      std::printf("  field %s crosses a link\n", ToUtf8(f.code).c_str());
      return false;
    }
  }
  return true;
}

// The page numbers the table shows: its PAGEREF fields' results, in order.
std::vector<std::u16string> Pages(const Document& doc) {
  std::vector<std::u16string> out;
  for (const Field& f : doc.Fields(doc.MainStory()))
    if (f.Type() == u"PAGEREF") out.push_back(f.result);
  return out;
}

const char* kStyles =
    "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/></w:style>"
    "<w:style w:type=\"character\" w:default=\"1\" w:styleId=\"DefaultParagraphFont\"><w:name w:val=\"Default Paragraph Font\"/></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/>"
    "<w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr><w:rPr><w:b/></w:rPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading2\"><w:name w:val=\"heading 2\"/><w:basedOn w:val=\"Normal\"/>"
    "<w:pPr><w:outlineLvl w:val=\"1\"/></w:pPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading3\"><w:name w:val=\"heading 3\"/><w:basedOn w:val=\"Normal\"/>"
    "<w:pPr><w:outlineLvl w:val=\"2\"/></w:pPr></w:style>";

std::string H(int level, const std::string& text) {
  return "<w:p><w:pPr><w:pStyle w:val=\"Heading" + std::to_string(level) + "\"/></w:pPr><w:r><w:t xml:space=\"preserve\">" +
         text + "</w:t></w:r></w:p>";
}
std::string P(const std::string& text) { return "<w:p><w:r><w:t xml:space=\"preserve\">" + text + "</w:t></w:r></w:p>"; }

std::string Body() {
  return "<w:p/>" + H(1, "Introduction") + P("Some text.") + H(2, "Background") + H(3, "Detail") +
         "<w:p><w:pPr><w:outlineLvl w:val=\"3\"/></w:pPr><w:r><w:t>Too deep</w:t></w:r></w:p>" + H(2, "") + H(1, "End") +
         P("The end.");
}

// A page for each heading: its paragraph's index, as text.
std::u16string ByIndex(uint32_t para) { return ToUtf16(std::to_string(para)); }

}  // namespace

TEST(Toc_InsertedAsWordWritesIt) {
  Fixture f;
  f.styles = kStyles;
  f.body = Body();
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::string main = MainXml(*doc), styles = PartXml(*doc, PartKind::Styles);
  const size_t paras = doc->Paragraphs().size();
  CHECK(edit::TocHeadings(*doc).size() == 4u);  // not the level 4 one, nor the empty one
  edit::Editor editor(*doc);
  edit::Result r = edit::InsertToc(editor, {MainParas(*doc)[0], 0}, ByIndex);
  if (!r.ok) std::printf("  refused: %s\n", ToUtf8(r.message).c_str());
  REQUIRE(r.ok);
  CHECK(editor.UndoName() == u"Table of Contents");
  // Four entries before the empty paragraph, which holds the field's end.
  CHECK(doc->Paragraphs().size() == paras + 4);
  const std::string after = MainXml(*doc);
  CHECK(Has(after, "<w:instrText xml:space=\"preserve\"> TOC \\o \"1-3\" \\h \\z \\u </w:instrText>"));
  CHECK(Count(after, "<w:pStyle w:val=\"TOC1\"/>") == 2u);
  CHECK(Count(after, "<w:pStyle w:val=\"TOC2\"/>") == 1u);
  CHECK(Count(after, "<w:pStyle w:val=\"TOC3\"/>") == 1u);
  CHECK(Count(after, "w:leader=\"dot\"") == 4u && Count(after, "w:pos=\"9360\"") == 4u);
  CHECK(Count(after, "<w:hyperlink w:anchor=\"_Toc") == 4u);
  CHECK(Count(after, " PAGEREF _Toc") == 4u);
  CHECK(Count(after, "<w:bookmarkStart") == 4u);
  CHECK(Has(after, "<w:rPr><w:noProof/><w:webHidden/></w:rPr><w:fldChar w:fldCharType=\"begin\"/>"));
  CHECK(Has(PartXml(*doc, PartKind::Styles), "w:styleId=\"TOC1\""));
  CHECK(NoFieldAcrossALink(*doc));
  // The TOC field: from the first entry's start to the start of the paragraph after the last.
  const std::vector<edit::TocInfo> tocs = edit::TablesOfContents(*doc);
  REQUIRE(tocs.size() == 1u);
  const std::vector<uint32_t> now = MainParas(*doc);
  CHECK((tocs[0].begin == Pos{now[0], 0}));
  CHECK((tocs[0].end == Pos{now[4], 0}));
  CHECK((r.caret == Pos{now[4], 1}));
  CHECK(edit::TocAt(*doc, {now[2], 3}).has_value());
  CHECK(!edit::TocAt(*doc, {now[6], 0}).has_value());
  // Each entry goes to its heading's bookmark and shows its page as things are now.
  std::vector<std::u16string> want;
  for (const edit::TocHeading& h : edit::TocHeadings(*doc)) want.push_back(ByIndex(h.para));
  CHECK(Pages(*doc) == want);
  CHECK(want.size() == 4u);
  std::optional<edit::BookmarkInfo> b = edit::FindBookmark(*doc, edit::Bookmarks(*doc).front().name);
  REQUIRE(b);
  CHECK(b->from.para == now[5] && b->from.offset == 0u);  // "Introduction"
  CHECK(SavesAndChecks(*doc));
  // One step, undone and redone exactly.
  REQUIRE(editor.Undo().ok);
  CHECK(MainXml(*doc) == main);
  CHECK(PartXml(*doc, PartKind::Styles) == styles);
  CHECK(!editor.CanUndo());
  REQUIRE(editor.Redo().ok);
  CHECK(MainXml(*doc) == after);
}

TEST(Toc_PageNumbersAndTheWholeTableUpdate) {
  Fixture f;
  f.styles = kStyles;
  f.body = Body();
  auto doc = LoadOk(f);
  REQUIRE(doc);
  edit::Editor editor(*doc);
  REQUIRE(edit::InsertToc(editor, {MainParas(*doc)[0], 0}, ByIndex).ok);
  const std::string inserted = MainXml(*doc);
  // Page numbers only: every entry's number changes, nothing else.
  auto seven = [](uint32_t) { return std::u16string(u"7"); };
  edit::Result r = edit::UpdateTocPages(editor, edit::TablesOfContents(*doc).at(0), seven);
  REQUIRE(r.ok);
  CHECK(editor.UndoName() == u"Update Table");
  CHECK((Pages(*doc) == std::vector<std::u16string>(4, u"7")));
  CHECK(NoFieldAcrossALink(*doc));
  CHECK(Count(MainXml(*doc), "<w:hyperlink w:anchor=\"_Toc") == 4u);
  REQUIRE(editor.Undo().ok);
  CHECK(MainXml(*doc) == inserted);
  // Already up to date: no step.
  CHECK(edit::UpdateTocPages(editor, edit::TablesOfContents(*doc).at(0), ByIndex).ok);
  CHECK(MainXml(*doc) == inserted);
  CHECK(editor.UndoName() == u"Table of Contents");
  // A heading more and one renamed, then the whole table again: the headings keep their
  // bookmarks, and the table its place and its code.
  std::vector<uint32_t> paras = MainParas(*doc);
  const uint32_t text = paras[6];  // "Some text."
  edit::PropEdit h2;
  h2.Set("pStyle", {{"val", "Heading2"}});
  REQUIRE(editor.SetParaProps(text, text, h2).ok);
  REQUIRE(editor.InsertText({paras[5], 0}, u"An ").ok);
  const std::string before = MainXml(*doc);
  r = edit::UpdateToc(editor, edit::TablesOfContents(*doc).at(0), ByIndex);
  if (!r.ok) std::printf("  refused: %s\n", ToUtf8(r.message).c_str());
  REQUIRE(r.ok);
  CHECK(editor.UndoName() == u"Update Table");
  const std::string updated = MainXml(*doc);
  CHECK(Count(updated, "<w:hyperlink w:anchor=\"_Toc") == 5u);
  CHECK(Count(updated, "<w:bookmarkStart") == 5u);
  CHECK(Count(updated, " TOC \\o \"1-3\" \\h \\z \\u ") == 1u);
  CHECK(NoFieldAcrossALink(*doc));
  paras = MainParas(*doc);
  CHECK(ToUtf8(doc->Para(paras[0]).text).find("An Introduction") != std::string::npos);
  CHECK(ToUtf8(doc->Para(paras[1]).text).find("Some text.") != std::string::npos);
  CHECK(doc->Para(paras[5]).text.empty() == false);
  std::vector<std::u16string> want;
  for (const edit::TocHeading& h : edit::TocHeadings(*doc)) want.push_back(ByIndex(h.para));
  CHECK(Pages(*doc) == want);
  CHECK(SavesAndChecks(*doc));
  REQUIRE(editor.Undo().ok);
  CHECK(MainXml(*doc) == before);
  REQUIRE(editor.Redo().ok);
  CHECK(MainXml(*doc) == updated);
}

TEST(Toc_WithoutHeadingsSaysSo) {
  Fixture f;
  f.styles = kStyles;
  f.body = P("Only text.");
  auto doc = LoadOk(f);
  REQUIRE(doc);
  edit::Editor editor(*doc);
  edit::Result r = edit::InsertToc(editor, {MainParas(*doc)[0], 4}, ByIndex);
  REQUIRE(r.ok);
  // Split where it went: "Only", the table, then " text.".
  const std::vector<uint32_t> paras = MainParas(*doc);
  REQUIRE(paras.size() == 4u);
  CHECK(ToUtf8(doc->Para(paras[0]).text) == "Only");
  CHECK(ToUtf8(doc->Para(paras[1]).text).find("No table of contents entries found.") != std::string::npos);
  CHECK(ToUtf8(doc->Para(paras[3]).text) == " text.");
  CHECK(edit::TablesOfContents(*doc).size() == 1u);
  CHECK(SavesAndChecks(*doc));
}

TEST(Toc_OnlyInTheText) {
  Fixture f;
  f.styles = kStyles;
  f.header = "<w:p><w:r><w:t>Head</w:t></w:r></w:p>";
  f.body = Body() + "<w:sectPr><w:headerReference w:type=\"default\" r:id=\"rIdHeader\"/></w:sectPr>";
  auto doc = LoadOk(f);
  REQUIRE(doc);
  uint32_t head = kNone;
  for (uint32_t i = 0; i < doc->Paragraphs().size(); ++i)
    if (doc->Stories()[doc->Para(i).story].kind == StoryKind::Header) head = i;
  REQUIRE(head != kNone);
  edit::Editor editor(*doc);
  CHECK(!edit::InsertToc(editor, {head, 1}, ByIndex).ok);
  REQUIRE(edit::InsertToc(editor, {MainParas(*doc)[0], 0}, ByIndex).ok);
  // Not inside the table there is.
  CHECK(!edit::InsertToc(editor, {MainParas(*doc)[1], 2}, ByIndex).ok);
  CHECK(edit::TablesOfContents(*doc).size() == 1u);
}

TEST(Toc_NumberedHeadingsShowTheirNumbers) {
  // Inserted at the start of a numbered heading: the entries show the headings' numbers, and take
  // no number of their own from the paragraph they were made from.
  Fixture f;
  f.styles = kStyles;
  f.numbering =
      "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/>"
      "<w:lvlText w:val=\"%1.\"/></w:lvl></w:abstractNum><w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>";
  auto numbered = [](const std::string& text) {
    return "<w:p><w:pPr><w:pStyle w:val=\"Heading1\"/><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"1\"/></w:numPr></w:pPr>"
           "<w:r><w:t>" + text + "</w:t></w:r></w:p>";
  };
  f.body = numbered("First") + P("Text.") + numbered("Second");
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::vector<edit::TocHeading> headings = edit::TocHeadings(*doc);
  REQUIRE(headings.size() == 2u);
  CHECK(ToUtf8(headings[0].text) == "1.\tFirst");
  CHECK(ToUtf8(headings[1].text) == "2.\tSecond");
  edit::Editor editor(*doc);
  REQUIRE(edit::InsertToc(editor, {MainParas(*doc)[0], 0}, ByIndex).ok);
  const std::string after = MainXml(*doc);
  CHECK(Count(after, "<w:numPr>") == 2u);  // the headings' own
  const std::vector<uint32_t> paras = MainParas(*doc);
  REQUIRE(paras.size() == 6u);  // two entries, the field's end, then the document
  CHECK(!doc->InList(paras[0]) && !doc->InList(paras[1]) && !doc->InList(paras[2]));
  CHECK(doc->InList(paras[3]) && doc->InList(paras[5]));
  CHECK(doc->Label(paras[3]).has_value() && ToUtf8(doc->Label(paras[3])->text) == "1.");
  CHECK(ToUtf8(doc->Para(paras[0]).text).find("1.\tFirst\t") != std::string::npos);
  CHECK(NoFieldAcrossALink(*doc));
  CHECK(SavesAndChecks(*doc));
}
