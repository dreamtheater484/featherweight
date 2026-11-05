// Footnotes and endnotes (D4): inserting one adds its reference and its note as Word does, in one
// undo step; deleting a reference deletes its note in the same step; undo and redo put every part
// back exactly, and the saved package passes the checks Office's rules make.
#include "TestHarness.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "DocFixture.h"
#include "doc/docx/Save.h"
#include "doc/edit/Editor.h"
#include "doc/edit/Notes.h"
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
std::string NotesXml(const Document& doc) { return PartXml(doc, PartKind::Footnotes); }
bool Has(const std::string& xml, std::string_view what) { return xml.find(what) != std::string::npos; }
std::string Text(const Document& doc, uint32_t para) { return ToUtf8(doc.Para(para).text); }

// The text of note `id`'s first paragraph; "(none)" when there's no such note.
std::string NoteText(const Document& doc, StoryKind kind, std::string_view id) {
  const uint32_t story = doc.NoteStory(kind, id);
  if (story == kNone) return "(none)";
  const std::vector<uint32_t> paras = doc.StoryParagraphs(story);
  return paras.empty() ? "(empty)" : Text(doc, paras.front());
}

// With FW_TEST_OUT set, the saved packages are kept there too, for the Office SDK's validator.
void Keep(const std::vector<uint8_t>& bytes) {
  char* dir = nullptr;
  size_t len = 0;
  if (_dupenv_s(&dir, &len, "FW_TEST_OUT") != 0 || !dir) return;
  static int n = 0;
  const std::string path = std::string(dir) + "/notes-" + std::to_string(++n) + ".docx";
  free(dir);
  FILE* f = nullptr;
  if (fopen_s(&f, path.c_str(), "wb") == 0 && f) {
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
  }
}

bool SavesAndChecks(Document& doc, std::unique_ptr<Document>* reopened = nullptr) {
  if (!docx::WriteAllParts(doc).Ok()) return false;
  fw::pkg::MemorySink sink;
  if (!doc.Package().Save(sink).Ok()) return false;
  Keep(sink.bytes);
  Status st;
  auto pkg = test::OpenBytes(std::move(sink.bytes), st);
  if (!pkg) return false;
  fw::pkg::CheckReport report = fw::pkg::CheckPackage(*pkg);
  for (const std::string& e : report.errors) std::printf("  check: %s\n", e.c_str());
  if (!report.Ok()) return false;
  auto again = docx::Read(std::move(pkg), st);
  if (!again) return false;
  if (reopened) *reopened = std::move(again);
  return true;
}

const char* kStyles =
    "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/></w:style>"
    "<w:style w:type=\"character\" w:default=\"1\" w:styleId=\"DefaultParagraphFont\"><w:name w:val=\"Default Paragraph Font\"/></w:style>";

const std::string kSeparators =
    "<w:footnote w:type=\"separator\" w:id=\"-1\"><w:p><w:r><w:separator/></w:r></w:p></w:footnote>"
    "<w:footnote w:type=\"continuationSeparator\" w:id=\"0\"><w:p><w:r><w:continuationSeparator/></w:r></w:p></w:footnote>";

std::string Note(int id, const std::string& text) {
  return "<w:footnote w:id=\"" + std::to_string(id) + "\"><w:p><w:r><w:footnoteRef/></w:r><w:r><w:t>" + text +
         "</w:t></w:r></w:p></w:footnote>";
}

std::string Ref(int id) { return "<w:r><w:footnoteReference w:id=\"" + std::to_string(id) + "\"/></w:r>"; }
std::string Run(const std::string& text) { return "<w:r><w:t xml:space=\"preserve\">" + text + "</w:t></w:r>"; }

}  // namespace

TEST(Notes_InsertFootnoteAsWordDoes) {
  Fixture f;
  f.styles = kStyles;
  f.body = "<w:p>" + Run("Hello world") + "</w:p>";
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const uint32_t p = MainParas(*doc)[0];
  const std::string main = MainXml(*doc), styles = PartXml(*doc, PartKind::Styles);
  edit::Editor editor(*doc);
  edit::Result r = edit::InsertNote(editor, {p, 5}, false);
  if (!r.ok) std::printf("  refused: %s\n", ToUtf8(r.message).c_str());
  REQUIRE(r.ok);
  CHECK(editor.UndoName() == u"Footnote");
  CHECK(Text(*doc, p) == "Hello\x02 world");
  const std::string mainAfter = MainXml(*doc);
  CHECK(Has(mainAfter, "<w:r><w:rPr><w:rStyle w:val=\"FootnoteReference\"/></w:rPr><w:footnoteReference w:id=\"1\"/></w:r>"));
  // The part Word starts with: its two separators, then the note in Word's styles.
  const std::string notes = NotesXml(*doc);
  CHECK(Has(notes, "<w:footnote w:type=\"separator\" w:id=\"-1\">"));
  CHECK(Has(notes, "<w:footnote w:type=\"continuationSeparator\" w:id=\"0\">"));
  CHECK(Has(notes, "<w:footnote w:id=\"1\"><w:p><w:pPr><w:pStyle w:val=\"FootnoteText\"/></w:pPr><w:r><w:rPr><w:rStyle "
                   "w:val=\"FootnoteReference\"/></w:rPr><w:footnoteRef/></w:r><w:r><w:t xml:space=\"preserve\"> </w:t></w:r>"
                   "</w:p></w:footnote>"));
  const std::string stylesAfter = PartXml(*doc, PartKind::Styles);
  CHECK(Has(stylesAfter, "w:styleId=\"FootnoteText\""));
  CHECK(Has(stylesAfter, "w:styleId=\"FootnoteReference\""));
  // The caret goes into the note, after its number and the space; typing there is the note's text.
  const uint32_t story = doc->NoteStory(StoryKind::Footnote, "1");
  REQUIRE(story != kNone);
  CHECK(r.caret.para == doc->StoryParagraphs(story).front());
  CHECK(r.caret.offset == 2u);
  CHECK(edit::NoteAt(*doc, {p, 5}) == story);
  CHECK(edit::NoteAt(*doc, {p, 6}) == story);
  CHECK(edit::NoteAt(*doc, {p, 4}) == kNone);
  CHECK((edit::NoteReference(*doc, story) == Pos{p, 5}));
  REQUIRE(editor.InsertText(r.caret, u"A note.").ok);
  CHECK(NoteText(*doc, StoryKind::Footnote, "1") == "\x02 A note.");
  const std::string typed = NotesXml(*doc);
  std::unique_ptr<Document> again;
  CHECK(SavesAndChecks(*doc, &again));
  if (again) CHECK(NoteText(*again, StoryKind::Footnote, "1") == "\x02 A note.");
  // Undo: the text, then the note, its reference and its styles, exactly.
  REQUIRE(editor.Undo().ok);
  REQUIRE(editor.Undo().ok);
  CHECK(MainXml(*doc) == main);
  CHECK(PartXml(*doc, PartKind::Styles) == styles);
  CHECK(!Has(NotesXml(*doc), "w:id=\"1\""));
  CHECK(doc->NoteStory(StoryKind::Footnote, "1") == kNone);
  CHECK(Text(*doc, p) == "Hello world");
  CHECK(!editor.CanUndo());
  REQUIRE(editor.Redo().ok);
  CHECK(MainXml(*doc) == mainAfter);
  CHECK(Has(NotesXml(*doc), "<w:footnote w:id=\"1\">"));
  REQUIRE(editor.Redo().ok);
  CHECK(NotesXml(*doc) == typed);
  CHECK(NoteText(*doc, StoryKind::Footnote, "1") == "\x02 A note.");
}

TEST(Notes_NewNotesTakeNewIds) {
  Fixture f;
  f.styles = kStyles;
  f.footnotes = kSeparators + Note(1, "First.");
  f.body = "<w:p>" + Run("One") + Ref(1) + Run(" two") + "</w:p>";
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const uint32_t p = MainParas(*doc)[0];
  edit::Editor editor(*doc);
  // Before the note there is: it numbers first, but its id is new.
  REQUIRE(edit::InsertNote(editor, {p, 1}, false).ok);
  CHECK(Has(MainXml(*doc), "<w:footnoteReference w:id=\"2\"/>"));
  CHECK(NoteText(*doc, StoryKind::Footnote, "2") == "\x02 ");
  CHECK(NoteText(*doc, StoryKind::Footnote, "1") == "\x02" "First.");
  CHECK(SavesAndChecks(*doc));
}

TEST(Notes_DeletingTheReferenceDeletesTheNote) {
  Fixture f;
  f.styles = kStyles;
  f.footnotes = kSeparators + Note(1, "First.") + Note(2, "Second.");
  f.body = "<w:p>" + Run("One") + Ref(1) + Run(" two") + Ref(2) + "</w:p>" + "<w:p>" + Run("After") + "</w:p>";
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const uint32_t p = MainParas(*doc)[0];
  CHECK(Text(*doc, p) == "One\x02 two\x02");
  const std::string main = MainXml(*doc), notes = NotesXml(*doc);
  const size_t paras = doc->Paragraphs().size();
  edit::Editor editor(*doc);
  REQUIRE(editor.Delete({p, 3}, {p, 4}).ok);
  CHECK(Text(*doc, p) == "One two\x02");
  CHECK(editor.UndoName() == u"Delete");
  CHECK(doc->NoteStory(StoryKind::Footnote, "1") == kNone);
  CHECK(NoteText(*doc, StoryKind::Footnote, "2") == "\x02" "Second.");
  CHECK(doc->Paragraphs().size() == paras - 1);
  const std::string mainAfter = MainXml(*doc), notesAfter = NotesXml(*doc);
  CHECK(!Has(notesAfter, "w:id=\"1\""));
  CHECK(!Has(mainAfter, "w:id=\"1\""));
  CHECK(Has(notesAfter, "Second."));
  CHECK(SavesAndChecks(*doc));
  // One step: undo puts the reference and the note back, as they were.
  REQUIRE(editor.Undo().ok);
  CHECK(MainXml(*doc) == main);
  CHECK(NotesXml(*doc) == notes);
  CHECK(NoteText(*doc, StoryKind::Footnote, "1") == "\x02" "First.");
  CHECK(doc->Paragraphs().size() == paras);
  CHECK(!editor.CanUndo());
  REQUIRE(editor.Redo().ok);
  CHECK(MainXml(*doc) == mainAfter);
  CHECK(NotesXml(*doc) == notesAfter);
  // A selection takes every note in it; the notes' text after them stays editable.
  REQUIRE(editor.Delete({p, 0}, {p, static_cast<uint32_t>(doc->Para(p).text.size())}).ok);
  CHECK(doc->NoteStory(StoryKind::Footnote, "2") == kNone);
  CHECK(Text(*doc, MainParas(*doc)[1]) == "After");
  REQUIRE(editor.Undo().ok);
  CHECK(MainXml(*doc) == mainAfter);
  CHECK(NotesXml(*doc) == notesAfter);
  REQUIRE(editor.Undo().ok);
  CHECK(NotesXml(*doc) == notes);
}

TEST(Notes_DeleteNoteTakesItsReference) {
  Fixture f;
  f.styles = kStyles;
  f.footnotes = kSeparators + Note(1, "First.") + Note(2, "Second.");
  f.body = "<w:p>" + Run("One") + Ref(1) + Run(" two") + Ref(2) + "</w:p>";
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const uint32_t p = MainParas(*doc)[0];
  const std::string main = MainXml(*doc), notes = NotesXml(*doc);
  edit::Editor editor(*doc);
  const uint32_t second = doc->NoteStory(StoryKind::Footnote, "2");
  edit::Result r = edit::DeleteNote(editor, second);
  REQUIRE(r.ok);
  CHECK((r.caret == Pos{p, 8}));
  CHECK(Text(*doc, p) == "One\x02 two");
  CHECK(doc->NoteStory(StoryKind::Footnote, "2") == kNone);
  CHECK(edit::NoteReference(*doc, second) == std::nullopt);
  REQUIRE(editor.Undo().ok);
  CHECK(MainXml(*doc) == main);
  CHECK(NotesXml(*doc) == notes);
}

TEST(Notes_AReferenceLeftKeepsItsNote) {
  // A file may refer to a note twice: deleting one reference leaves the note to the other.
  Fixture f;
  f.styles = kStyles;
  f.footnotes = kSeparators + Note(1, "Shared.");
  f.body = "<w:p>" + Run("a") + Ref(1) + Run("b") + Ref(1) + "</w:p>";
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const uint32_t p = MainParas(*doc)[0];
  edit::Editor editor(*doc);
  REQUIRE(editor.Delete({p, 1}, {p, 2}).ok);
  CHECK(NoteText(*doc, StoryKind::Footnote, "1") == "\x02" "Shared.");
  REQUIRE(editor.Delete({p, 1}, {p, 3}).ok);
  CHECK(doc->NoteStory(StoryKind::Footnote, "1") == kNone);
  CHECK(SavesAndChecks(*doc));
}

TEST(Notes_DeletingANotesNumberKeepsTheNote) {
  // As in Word: the note stays, numbered at its reference, without its own number.
  Fixture f;
  f.styles = kStyles;
  f.footnotes = kSeparators + Note(1, "First.");
  f.body = "<w:p>" + Run("One") + Ref(1) + "</w:p>";
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const uint32_t story = doc->NoteStory(StoryKind::Footnote, "1");
  REQUIRE(story != kNone);
  const uint32_t np = doc->StoryParagraphs(story).front();
  const std::string notes = NotesXml(*doc);
  edit::Editor editor(*doc);
  REQUIRE(editor.Delete({np, 0}, {np, 1}).ok);
  CHECK(NoteText(*doc, StoryKind::Footnote, "1") == "First.");
  CHECK(!Has(NotesXml(*doc), "<w:footnoteRef/>"));
  CHECK(SavesAndChecks(*doc));
  REQUIRE(editor.Undo().ok);
  CHECK(NotesXml(*doc) == notes);
}

TEST(Notes_EndnoteMakesItsPart) {
  Fixture f;
  f.styles = kStyles;
  f.body = "<w:p>" + Run("Text") + "</w:p>";
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const uint32_t p = MainParas(*doc)[0];
  edit::Editor editor(*doc);
  edit::Result r = edit::InsertNote(editor, {p, 4}, true);
  REQUIRE(r.ok);
  CHECK(editor.UndoName() == u"Endnote");
  CHECK(Has(MainXml(*doc), "<w:rStyle w:val=\"EndnoteReference\"/></w:rPr><w:endnoteReference w:id=\"1\"/>"));
  const std::string notes = PartXml(*doc, PartKind::Endnotes);
  CHECK(Has(notes, "<w:endnote w:type=\"separator\" w:id=\"-1\">"));
  CHECK(Has(notes, "<w:endnote w:id=\"1\"><w:p><w:pPr><w:pStyle w:val=\"EndnoteText\"/></w:pPr>"));
  REQUIRE(editor.InsertText(r.caret, u"The end.").ok);
  std::unique_ptr<Document> again;
  CHECK(SavesAndChecks(*doc, &again));
  if (again) CHECK(NoteText(*again, StoryKind::Endnote, "1") == "\x02 The end.");
}

TEST(Notes_OnlyInTheTextOutsideFields) {
  Fixture f;
  f.styles = kStyles;
  f.header = "<w:p>" + Run("Head") + "</w:p>";
  f.body = "<w:p><w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText xml:space=\"preserve\"> PAGE </w:instrText></w:r>"
           "<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r><w:r><w:t>1</w:t></w:r><w:r><w:fldChar w:fldCharType=\"end\"/></w:r>"
           "</w:p><w:sectPr><w:headerReference w:type=\"default\" r:id=\"rIdHeader\"/></w:sectPr>";
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const uint32_t p = MainParas(*doc)[0];
  REQUIRE(Text(*doc, p) == "\x13 PAGE \x14" "1\x15");
  uint32_t head = kNone;
  for (uint32_t i = 0; i < doc->Paragraphs().size(); ++i)
    if (doc->Stories()[doc->Para(i).story].kind == StoryKind::Header) head = i;
  REQUIRE(head != kNone);
  edit::Editor editor(*doc);
  CHECK(!edit::InsertNote(editor, {head, 2}, false).ok);
  CHECK(!edit::InsertNote(editor, {p, 3}, false).ok);  // in the code
  CHECK(!edit::InsertNote(editor, {p, 8}, false).ok);  // in the result
  CHECK(doc->FindPart(PartKind::Footnotes) == nullptr);  // nothing added on the way
  CHECK(!editor.CanUndo());
  CHECK(edit::InsertNote(editor, {p, 10}, false).ok);  // after the field
  CHECK(edit::InsertNote(editor, {p, 0}, false).ok);   // before it
  CHECK(SavesAndChecks(*doc));
}

TEST(Notes_DeletedRowsAndTablesTakeTheirNotes) {
  Fixture f;
  f.styles = kStyles;
  f.footnotes = kSeparators + Note(1, "In the table.");
  const std::string cell = "<w:tc><w:tcPr><w:tcW w:w=\"2000\" w:type=\"dxa\"/></w:tcPr>";
  f.body = "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"2000\"/></w:tblGrid>"
           "<w:tr>" + cell + "<w:p>" + Run("Top") + "</w:p></w:tc></w:tr>"
           "<w:tr>" + cell + "<w:p>" + Run("Noted") + Ref(1) + "</w:p></w:tc></w:tr></w:tbl><w:p/>";
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::vector<uint32_t> paras = MainParas(*doc);
  const std::string main = MainXml(*doc), notes = NotesXml(*doc);
  edit::Editor editor(*doc);
  REQUIRE(editor.DeleteRows({paras[1], 0}, {paras[1], 0}).ok);
  CHECK(doc->NoteStory(StoryKind::Footnote, "1") == kNone);
  CHECK(!editor.UndoName().empty());
  CHECK(SavesAndChecks(*doc));
  REQUIRE(editor.Undo().ok);
  CHECK(MainXml(*doc) == main);
  CHECK(NotesXml(*doc) == notes);
  REQUIRE(editor.DeleteTable({paras[0], 0}).ok);
  CHECK(doc->NoteStory(StoryKind::Footnote, "1") == kNone);
  REQUIRE(editor.Undo().ok);
  CHECK(MainXml(*doc) == main);
  CHECK(NotesXml(*doc) == notes);
  CHECK(!editor.CanUndo());
}
