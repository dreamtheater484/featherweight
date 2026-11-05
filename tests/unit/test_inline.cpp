#include "TestHarness.h"

#include <string>

#include "DocFixture.h"
#include "doc/edit/Editor.h"
#include "doc/edit/Inline.h"
#include "doc/model/Utf.h"
#include "pkg/Check.h"

using namespace fw;
using namespace fw::doc;
using namespace docfix;
namespace edit = fw::doc::edit;

// Fields, links and bookmarks (D3): each is one undo step that puts the part back exactly, and
// the saved package passes the checks Office's rules make.
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
Pos P(uint32_t para, uint32_t offset) { return Pos{para, offset}; }
std::string Text(const Document& doc, uint32_t para) { return ToUtf8(doc.Para(para).text); }

std::unique_ptr<Document> SaveAndReopen(Document& doc) {
  if (!docx::WriteAllParts(doc).Ok()) return nullptr;
  fw::pkg::MemorySink sink;
  if (!doc.Package().Save(sink).Ok()) return nullptr;
  Status st;
  auto pkg = test::OpenBytes(std::move(sink.bytes), st);
  if (!pkg) return nullptr;
  fw::pkg::CheckReport report = fw::pkg::CheckPackage(*pkg);
  for (const std::string& e : report.errors) std::printf("  check: %s\n", e.c_str());
  if (!report.Ok()) return nullptr;
  return docx::Read(std::move(pkg), st);
}

const char* kStyles =
    "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/></w:style>"
    "<w:style w:type=\"character\" w:default=\"1\" w:styleId=\"DefaultParagraphFont\"><w:name w:val=\"Default Paragraph Font\"/></w:style>";

}  // namespace

TEST(Inline_FieldIsOneStepThatUndoesExactly) {
  Fixture f;
  f.body = "<w:p><w:r><w:rPr><w:b/></w:rPr><w:t xml:space=\"preserve\">Page </w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const uint32_t p = MainParas(*doc)[0];
  const std::string original = MainXml(*doc);
  edit::Editor editor(*doc);
  edit::Result r = edit::InsertField(editor, {p, 5}, " PAGE ", u"1", u"Page Number");
  CHECK(r.ok);
  const std::string after = MainXml(*doc);
  CHECK(Has(after, "<w:fldSimple w:instr=\" PAGE \"><w:r><w:rPr><w:b/></w:rPr><w:t xml:space=\"preserve\">1</w:t></w:r></w:fldSimple>"));
  CHECK(Text(*doc, p) == "Page 1");
  bool inField = false;
  for (const TextRun& run : doc->Runs(p)) inField |= run.start == 5 && run.simpleField;
  CHECK(inField);
  CHECK(editor.UndoName() == u"Page Number");
  CHECK(SaveAndReopen(*doc) != nullptr);
  CHECK(editor.Undo() && MainXml(*doc) == original && !editor.CanUndo());
  CHECK(editor.Redo() && MainXml(*doc) == after);
}

TEST(Inline_FieldIsRefusedInsideAField) {
  Fixture f;
  f.body = "<w:p><w:fldSimple w:instr=\" DATE \"><w:r><w:t>today</w:t></w:r></w:fldSimple></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const uint32_t p = MainParas(*doc)[0];
  const std::string original = MainXml(*doc);
  edit::Editor editor(*doc);
  CHECK(!edit::InsertField(editor, {p, 2}, " PAGE ", u"1").ok);
  CHECK(!edit::InsertField(editor, {p, 0}, "", u"1").ok);
  CHECK(!edit::InsertField(editor, {p, 0}, " PAGE ", u"").ok);
  CHECK(MainXml(*doc) == original && !editor.CanUndo());
}

TEST(Inline_BookmarkInsertGoesAndDeletes) {
  Fixture f;
  f.body = "<w:p><w:r><w:t>Hello world</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const uint32_t p = MainParas(*doc)[0];
  const std::string original = MainXml(*doc);
  edit::Editor editor(*doc);
  CHECK(edit::InsertBookmark(editor, {p, 6}, {p, 11}, "Target").ok);
  const std::string after = MainXml(*doc);
  CHECK(Has(after, "<w:r><w:t xml:space=\"preserve\">Hello </w:t></w:r><w:bookmarkStart w:id=\"0\" w:name=\"Target\"/>"
                   "<w:r><w:t>world</w:t></w:r><w:bookmarkEnd w:id=\"0\"/></w:p>"));
  auto marks = edit::Bookmarks(*doc);
  CHECK(marks.size() == 1 && marks[0].name == "Target" && marks[0].from == P(p, 6) && marks[0].to == P(p, 11) &&
        !marks[0].hidden);
  CHECK(edit::FindBookmark(*doc, "TARGET").has_value());
  CHECK(SaveAndReopen(*doc) != nullptr);
  CHECK(editor.Undo() && MainXml(*doc) == original && edit::Bookmarks(*doc).empty());
  CHECK(editor.Redo() && MainXml(*doc) == after);
  // A taken name (in any case) moves that bookmark, as Word's Add does.
  CHECK(edit::InsertBookmark(editor, {p, 0}, {p, 5}, "target").ok);
  marks = edit::Bookmarks(*doc);
  CHECK(marks.size() == 1 && marks[0].from == P(p, 0) && marks[0].to == P(p, 5) && marks[0].name == "target");
  CHECK(edit::DeleteBookmark(editor, "Target").ok);
  CHECK(edit::Bookmarks(*doc).empty() && Text(*doc, p) == "Hello world");
  CHECK(SaveAndReopen(*doc) != nullptr);
  CHECK(!edit::DeleteBookmark(editor, "Target").ok);
}

TEST(Inline_BookmarkNamesAndIds) {
  CHECK(edit::ValidBookmarkName("Intro") && edit::ValidBookmarkName("Größe_2") && edit::ValidBookmarkName("a"));
  CHECK(!edit::ValidBookmarkName("") && !edit::ValidBookmarkName("1st") && !edit::ValidBookmarkName("_Toc1") &&
        !edit::ValidBookmarkName("two words") && !edit::ValidBookmarkName("a-b") &&
        !edit::ValidBookmarkName(std::string(41, 'a')) && edit::ValidBookmarkName(std::string(40, 'a')));
  Fixture f;
  f.body = "<w:p><w:bookmarkStart w:id=\"7\" w:name=\"Old\"/><w:r><w:t>One</w:t></w:r><w:bookmarkEnd w:id=\"7\"/></w:p>"
           "<w:p><w:r><w:t>Two</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const auto paras = MainParas(*doc);
  edit::Editor editor(*doc);
  CHECK(!edit::InsertBookmark(editor, {paras[0], 0}, {paras[0], 1}, "9lives").ok);
  // Across paragraphs, with an id no other bookmark has.
  CHECK(edit::InsertBookmark(editor, {paras[0], 1}, {paras[1], 2}, "Span").ok);
  const std::string xml = MainXml(*doc);
  CHECK(Has(xml, "<w:bookmarkStart w:id=\"8\" w:name=\"Span\"/>") && Has(xml, "<w:bookmarkEnd w:id=\"8\"/>"));
  auto span = edit::FindBookmark(*doc, "span");
  CHECK(span && span->from == P(paras[0], 1) && span->to == P(paras[1], 2));
  CHECK(SaveAndReopen(*doc) != nullptr);
}

TEST(Inline_LinkInsertEditRemove) {
  Fixture f;
  f.styles = kStyles;
  f.body = "<w:p><w:r><w:t xml:space=\"preserve\">Visit our site today</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const uint32_t p = MainParas(*doc)[0];
  const std::string original = MainXml(*doc), originalStyles = PartXml(*doc, PartKind::Styles);
  edit::Editor editor(*doc);
  CHECK(edit::InsertLink(editor, {p, 10}, {p, 14}, "https://example.com/", u"site").ok);
  CHECK(editor.UndoName() == u"Link");
  auto link = edit::LinkAt(*doc, {p, 12});
  CHECK(link && link->from == P(p, 10) && link->to == P(p, 14) && link->target == "https://example.com/");
  CHECK(edit::LinkAt(*doc, {p, 14}).has_value() && !edit::LinkAt(*doc, {p, 3}).has_value());
  const std::string linked = MainXml(*doc);
  CHECK(Has(linked, "w:history=\"1\"><w:r><w:rPr><w:rStyle w:val=\"Hyperlink\"/></w:rPr><w:t xml:space=\"preserve\">site</w:t></w:r></w:hyperlink>"));
  const std::string styles = PartXml(*doc, PartKind::Styles);
  CHECK(Has(styles, "<w:style w:type=\"character\" w:styleId=\"Hyperlink\"><w:name w:val=\"Hyperlink\"/>"
                    "<w:basedOn w:val=\"DefaultParagraphFont\"/><w:uiPriority w:val=\"99\"/><w:unhideWhenUsed/>"
                    "<w:rPr><w:color w:val=\"0563C1\" w:themeColor=\"hyperlink\"/><w:u w:val=\"single\"/></w:rPr></w:style>"));
  CHECK(SaveAndReopen(*doc) != nullptr);

  // New text and a bookmark for its target.
  CHECK(edit::EditLink(editor, {p, 12}, "#Target", u"page").ok);
  CHECK(Text(*doc, p) == "Visit our page today");
  link = edit::LinkAt(*doc, {p, 11});
  CHECK(link && link->from == P(p, 10) && link->to == P(p, 14) && link->target == "#Target");
  CHECK(Has(MainXml(*doc), "<w:hyperlink w:anchor=\"Target\" w:history=\"1\"><w:r><w:rPr><w:rStyle w:val=\"Hyperlink\"/></w:rPr><w:t xml:space=\"preserve\">page</w:t></w:r></w:hyperlink>"));
  CHECK(editor.UndoName() == u"Edit Link");

  CHECK(edit::RemoveLink(editor, {p, 11}).ok);
  CHECK(!edit::LinkAt(*doc, {p, 11}) && Text(*doc, p) == "Visit our page today");
  CHECK(!Has(MainXml(*doc), "hyperlink") && !Has(MainXml(*doc), "Hyperlink"));
  CHECK(SaveAndReopen(*doc) != nullptr);

  CHECK(editor.Undo() && MainXml(*doc) != original);  // the link is back
  CHECK(editor.Undo() && MainXml(*doc) == linked);
  CHECK(editor.Undo() && MainXml(*doc) == original && PartXml(*doc, PartKind::Styles) == originalStyles);
  CHECK(!editor.CanUndo());
}

TEST(Inline_LinkWithNewTextAndRefusals) {
  Fixture f;
  f.styles = kStyles;
  f.body = "<w:p><w:r><w:t xml:space=\"preserve\">Write to </w:t></w:r></w:p>"
           "<w:p><w:hyperlink w:anchor=\"X\"><w:r><w:t>linked</w:t></w:r></w:hyperlink></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const auto paras = MainParas(*doc);
  const uint32_t p = paras[0];
  edit::Editor editor(*doc);
  CHECK(edit::InsertLink(editor, {p, 9}, {p, 9}, "mailto:someone@example.com", u"me").ok);
  CHECK(Text(*doc, p) == "Write to me");
  auto link = edit::LinkAt(*doc, {p, 10});
  CHECK(link && link->from == P(p, 9) && link->target == "mailto:someone@example.com");
  CHECK(editor.Undo() && Text(*doc, p) == "Write to " && !editor.CanUndo());
  // Never a link in a link, an empty address or one XML can't hold.
  CHECK(!edit::InsertLink(editor, {paras[1], 1}, {paras[1], 3}, "https://x.org/", u"").ok);
  CHECK(!edit::InsertLink(editor, {p, 0}, {p, 5}, "", u"").ok);
  CHECK(!edit::InsertLink(editor, {p, 0}, {p, 5}, std::string("https://x.org/\x01"), u"").ok);
  CHECK(!edit::RemoveLink(editor, {p, 2}).ok);
  CHECK(!editor.CanUndo());
}

TEST(Inline_RemovingALinkKeepsWhatItHolds) {
  Fixture f;
  f.body = "<w:p><w:r><w:t xml:space=\"preserve\">See </w:t></w:r><w:hyperlink w:anchor=\"X\"><x:mark/>"
           "<w:r><w:t>this</w:t></w:r></w:hyperlink><x:after/></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const uint32_t p = MainParas(*doc)[0];
  const std::string original = MainXml(*doc);
  edit::Editor editor(*doc);
  CHECK(edit::RemoveLink(editor, {p, 5}).ok);
  const std::string xml = MainXml(*doc);
  CHECK(!Has(xml, "hyperlink"));
  const size_t mark = xml.find("x:mark"), run = xml.find("<w:t>this"), after = xml.find("x:after");
  CHECK(mark != std::string::npos && run != std::string::npos && after != std::string::npos && mark < run && run < after);
  CHECK(SaveAndReopen(*doc) != nullptr);
  CHECK(editor.Undo() && MainXml(*doc) == original);
}

TEST(Inline_HeadingTargetGetsAHiddenBookmark) {
  Fixture f;
  f.body = "<w:p><w:r><w:t>Intro</w:t></w:r></w:p><w:p><w:bookmarkStart w:id=\"3\" w:name=\"Mine\"/>"
           "<w:r><w:t>Has one</w:t></w:r><w:bookmarkEnd w:id=\"3\"/></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const auto paras = MainParas(*doc);
  edit::Editor editor(*doc);
  CHECK(edit::BookmarkForParagraph(editor, paras[1]) == "Mine");
  CHECK(!editor.CanUndo());
  const std::string name = edit::BookmarkForParagraph(editor, paras[0]);
  CHECK(name == "_Hlk100000001");
  auto b = edit::FindBookmark(*doc, name);
  CHECK(b && b->hidden && b->from == P(paras[0], 0) && b->to == P(paras[0], 5));
  CHECK(Has(MainXml(*doc), "<w:bookmarkStart w:id=\"4\" w:name=\"_Hlk100000001\"/>"));
  CHECK(SaveAndReopen(*doc) != nullptr);
}
