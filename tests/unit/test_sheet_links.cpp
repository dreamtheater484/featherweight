// Links (Insert > Link, Ctrl+K; S3, s3-review): Editor::SetHyperlink, and saving a link to a web
// address, file or e-mail address as Excel writes it (an external relationship).
#include "TestHarness.h"

#include <string>
#include <vector>

#include "SheetTestUtil.h"
#include "sheet/edit/Edit.h"
#include "sheet/edit/Journal.h"
#include "sheet/model/Workbook.h"
#include "sheet/xlsx/Xlsx.h"

using namespace fw::sheet;
using edit::CellInput;
using edit::Editor;
using namespace sheet_test;

namespace {

std::vector<uint8_t> SaveLinks(Workbook& book, Editor& ed) {
  ed.BeginSave();
  xlsx::SavePlan plan;
  fw::pkg::Status st = xlsx::PrepareSave(book, ed, plan, book.Format());
  CHECK(st.Ok());
  fw::pkg::MemorySink sink;
  if (st.Ok()) st = xlsx::WriteSave(book, plan, sink);
  CHECK(st.Ok());
  if (st.Ok()) xlsx::CommitSave(book, ed, plan);
  else ed.EndSave();
  return sink.bytes;
}

std::string Part(const std::vector<uint8_t>& zip, const std::string& part) {
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(zip, st);
  std::vector<uint8_t> out;
  if (!pkg || !pkg->ReadPart(part, out).Ok()) return {};
  return test::Text(out);
}

Hyperlink Web(const std::string& url, const std::string& display) {
  Hyperlink h;
  h.target = url;
  h.display = display;
  return h;
}

}  // namespace

TEST(sheet_links_set_save_reopen) {
  auto parts = WorkbookParts({"<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row></sheetData>", "<sheetData/>"});
  for (auto& [name, xml] : parts)
    if (name.find("worksheets/sheet") != std::string::npos)
      xml.replace(xml.find("<worksheet "), 11,
                  "<worksheet xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" ");
  const std::vector<uint8_t> original = test::MakeZip(parts);
  auto book = OpenZip(original);
  if (!book) return;
  Editor ed(*book);
  edit::Journal journal;
  ed.SetJournal(&journal);
  Hyperlink place;
  place.location = "Sheet2!B3";
  place.display = "Sheet2!B3";
  CHECK(ed.SetHyperlink(0, Range{0, 0, 0, 0}, place));
  CHECK(ed.SetHyperlink(0, Range{1, 0, 1, 0}, Web("https://example.com/?a=1&b=2", "Example")));
  CHECK(ed.SetHyperlink(0, Range{2, 0, 2, 0}, Web("mailto:someone@example.com", "Mail")));
  CHECK(book->GetSheet(0).Hyperlinks().size() == 3 && ed.UndoName() == u"Insert Hyperlink");
  // One over A1 again replaces it.
  place.location = "Sheet2!C4";
  CHECK(ed.SetHyperlink(0, Range{0, 0, 0, 0}, place));
  CHECK(book->GetSheet(0).Hyperlinks().size() == 3);
  CHECK(!ed.SetHyperlink(0, Range{3, 0, 3, 0}, Hyperlink{}));  // neither an address nor a place

  const std::vector<uint8_t> zip = SaveLinks(*book, ed);
  const std::string sheet = Part(zip, "/xl/worksheets/sheet1.xml"), rels = Part(zip, "/xl/worksheets/_rels/sheet1.xml.rels");
  CHECK(sheet.find("<hyperlink ref=\"A2\" r:id=\"rId1\" display=\"Example\"/>") != std::string::npos);
  CHECK(sheet.find("<hyperlink ref=\"A3\" r:id=\"rId2\" display=\"Mail\"/>") != std::string::npos);
  CHECK(sheet.find("<hyperlink ref=\"A1\" location=\"Sheet2!C4\" display=\"Sheet2!B3\"/>") != std::string::npos);
  CHECK(rels.find("Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink\" "
                  "Target=\"https://example.com/?a=1&amp;b=2\" TargetMode=\"External\"") != std::string::npos);
  CHECK(rels.find("Target=\"mailto:someone@example.com\" TargetMode=\"External\"") != std::string::npos);

  // Saved again (the package keeps the file's rels): the same ids.
  CHECK(ed.SetCell(0, 5, 5, CellInput::Number(1)));
  const std::vector<uint8_t> zip2 = SaveLinks(*book, ed);
  CHECK(Part(zip2, "/xl/worksheets/_rels/sheet1.xml.rels") == rels);
  CHECK(Part(zip2, "/xl/worksheets/sheet1.xml").find("r:id=\"rId2\" display=\"Mail\"") != std::string::npos);

  auto again = OpenZip(zip);
  if (!again) return;
  const auto& links = again->GetSheet(0).Hyperlinks();
  CHECK(links.size() == 3);
  bool web = false, mail = false;
  for (const Hyperlink& h : links) {
    web = web || (h.target == "https://example.com/?a=1&b=2" && h.relId == "rId1");
    mail = mail || h.target == "mailto:someone@example.com";
  }
  CHECK(web && mail);
  // Removing and undoing.
  Editor ed2(*again);
  CHECK(ed2.SetHyperlink(0, Range{0, 0, 2, 0}, std::nullopt) && again->GetSheet(0).Hyperlinks().empty());
  CHECK(ed2.Undo() && again->GetSheet(0).Hyperlinks().size() == 3);

  // The journal brings them back.
  auto replayed = OpenZip(original);
  if (!replayed) return;
  Editor ed3(*replayed);
  CHECK(ed3.Replay(journal.All()));
  CHECK(replayed->GetSheet(0).Hyperlinks().size() == 3);

  // On a protected sheet: only with Insert links allowed, and into unlocked cells (Excel).
  CHECK(ed.SetSheetProtection(0, edit::SheetProtection{}));
  CHECK(!ed.SetHyperlink(0, Range{4, 0, 4, 0}, Web("https://example.com/", "x")));
}

// Notes (Shift+F2) as Excel writes them: comments part, VML drawing, the sheet's legacyDrawing.
TEST(sheet_notes_set_save_delete) {
  auto parts = WorkbookParts({"<sheetData><row r=\"2\"><c r=\"B2\"><v>1</v></c></row></sheetData>"});
  for (auto& [name, xml] : parts)
    if (name.find("worksheets/sheet") != std::string::npos)
      xml.replace(xml.find("<worksheet "), 11,
                  "<worksheet xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" ");
  auto book = OpenZip(test::MakeZip(parts));
  if (!book) return;
  Editor ed(*book);
  CHECK(ed.SetNote(0, CellAddr{1, 1}, Editor::Note{"Roy", "first note", false}));
  CHECK(ed.SetNote(0, CellAddr{4, 3}, Editor::Note{"Roy", "line one\nline two", true}));
  CHECK(ed.UndoName() == u"Edit Note");
  std::vector<uint8_t> zip = SaveLinks(*book, ed);
  const std::string comments = Part(zip, "/xl/comments1.xml"), vml = Part(zip, "/xl/drawings/vmlDrawing1.vml");
  const std::string sheet = Part(zip, "/xl/worksheets/sheet1.xml"), types = Part(zip, "/[Content_Types].xml");
  CHECK(comments.find("<authors><author>Roy</author></authors><commentList><comment ref=\"B2\" authorId=\"0\" shapeId=\"0\">") !=
        std::string::npos);
  CHECK(comments.find("<t>Roy:</t>") != std::string::npos && comments.find("<comment ref=\"D5\"") > comments.find("ref=\"B2\""));
  CHECK(vml.find("<x:Anchor>2, 10, 0, 12, 4, 20, 4, 17</x:Anchor>") != std::string::npos);
  CHECK(vml.find("<x:Row>4</x:Row><x:Column>3</x:Column><x:Visible/>") != std::string::npos);
  CHECK(sheet.find("<legacyDrawing r:id=\"rId") != std::string::npos);
  CHECK(types.find("spreadsheetml.comments+xml") != std::string::npos && types.find("vmlDrawing") != std::string::npos);
  // Edited in place, then both deleted: the parts and the legacyDrawing go.
  CHECK(ed.SetNote(0, CellAddr{1, 1}, Editor::Note{"Ann", "changed", false}));
  CHECK(ed.PartBytes("/xl/comments1.xml").find("<author>Ann</author>") != std::string::npos);
  CHECK(ed.SetNote(0, CellAddr{1, 1}, std::nullopt) && ed.SetNote(0, CellAddr{4, 3}, std::nullopt));
  CHECK(ed.PartBytes("/xl/comments1.xml").empty() && ed.PartBytes("/xl/drawings/vmlDrawing1.vml").empty());
  zip = SaveLinks(*book, ed);
  CHECK(Part(zip, "/xl/worksheets/sheet1.xml").find("legacyDrawing") == std::string::npos);
  CHECK(Part(zip, "/xl/comments1.xml").empty());
  CHECK(ed.Undo(3) && ed.PartBytes("/xl/comments1.xml").find("<author>Roy</author>") != std::string::npos);
  // On a protected sheet: only with Edit objects allowed (Excel).
  CHECK(ed.SetSheetProtection(0, edit::SheetProtection{}) && !ed.SetNote(0, CellAddr{0, 0}, Editor::Note{"Roy", "x"}));
}

TEST(sheet_threads_set_reply_resolve_delete) {
  auto parts = WorkbookParts({"<sheetData><row r=\"2\"><c r=\"B2\"><v>1</v></c></row></sheetData>"});
  for (auto& [name, xml] : parts)
    if (name.find("worksheets/sheet") != std::string::npos)
      xml.replace(xml.find("<worksheet "), 11,
                  "<worksheet xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" ");
  auto book = OpenZip(test::MakeZip(parts));
  if (!book) return;
  Editor ed(*book);
  Editor::Thread t;
  t.comments.push_back({"Roy", "first comment", {}, {}});
  CHECK(ed.SetThread(0, CellAddr{1, 1}, t));
  CHECK(ed.UndoName() == u"New Comment");
  std::optional<Editor::Thread> got = ed.ThreadAt(0, CellAddr{1, 1});
  CHECK(got && got->comments.size() == 1 && got->comments[0].author == "Roy" && got->comments[0].id.size() == 38);
  if (!got) return;
  // A reply, then resolved: the first comment keeps its id and time.
  got->comments.push_back({"Ann", "a reply", {}, {}});
  got->resolved = true;
  CHECK(ed.SetThread(0, CellAddr{1, 1}, *got));
  CHECK(ed.UndoName() == u"Edit Comment");
  // A note can't go on it, nor a thread on a note's cell.
  CHECK(!ed.SetNote(0, CellAddr{1, 1}, Editor::Note{"Roy", "x"}));
  CHECK(ed.SetNote(0, CellAddr{4, 3}, Editor::Note{"Roy", "a note"}) && !ed.SetThread(0, CellAddr{4, 3}, t));
  std::vector<uint8_t> zip = SaveLinks(*book, ed);
  const std::string threads = Part(zip, "/xl/threadedComments/threadedComment1.xml"), persons = Part(zip, "/xl/persons/person.xml");
  const std::string comments = Part(zip, "/xl/comments1.xml"), types = Part(zip, "/[Content_Types].xml");
  const std::string first = got->comments[0].id;
  CHECK(threads.find("<threadedComment ref=\"B2\" dT=\"" + got->comments[0].date + "\" personId=\"{") != std::string::npos);
  CHECK(threads.find("id=\"" + first + "\" done=\"1\"><text>first comment</text>") != std::string::npos);
  CHECK(threads.find("parentId=\"" + first + "\"><text>a reply</text>") != std::string::npos);
  CHECK(persons.find("<person displayName=\"Roy\" id=\"{") != std::string::npos &&
        persons.find("userId=\"Ann\" providerId=\"None\"/>") != std::string::npos);
  CHECK(comments.find("<author>tc=" + first + "</author>") != std::string::npos);
  CHECK(comments.find("xr:uid=\"" + first + "\"><text><t>[Threaded comment]") != std::string::npos);
  CHECK(comments.find("Comment:\n    first comment\nReply:\n    a reply</t>") != std::string::npos);
  CHECK(comments.find("mc:Ignorable=\"xr\"") != std::string::npos && comments.find("<comment ref=\"D5\"") > comments.find("ref=\"B2\""));
  CHECK(types.find("application/vnd.ms-excel.threadedcomments+xml") != std::string::npos &&
        types.find("application/vnd.ms-excel.person+xml") != std::string::npos);
  CHECK(Part(zip, "/xl/_rels/workbook.xml.rels").find("relationships/person") != std::string::npos);
  CHECK(Part(zip, "/xl/worksheets/_rels/sheet1.xml.rels").find("relationships/threadedComment") != std::string::npos);
  // Reopened: the same thread, resolved.
  auto again = OpenZip(zip);
  if (again) {
    Editor ed2(*again);
    std::optional<Editor::Thread> back = ed2.ThreadAt(0, CellAddr{1, 1});
    CHECK(back && back->resolved && back->comments.size() == 2 && back->comments[1].author == "Ann" &&
          back->comments[1].text == "a reply" && back->comments[0].id == first);
  }
  // Deleted: the thread and its note go; the other note and the persons stay.
  CHECK(ed.SetThread(0, CellAddr{1, 1}, std::nullopt) && ed.UndoName() == u"Delete Comment");
  CHECK(ed.PartBytes("/xl/threadedComments/threadedComment1.xml").empty());
  CHECK(ed.PartBytes("/xl/comments1.xml").find("tc=") == std::string::npos &&
        ed.PartBytes("/xl/comments1.xml").find("<comment ref=\"D5\" authorId=\"0\"") != std::string::npos);
  CHECK(!ed.PartBytes("/xl/persons/person.xml").empty());
  CHECK(ed.Undo() && ed.ThreadAt(0, CellAddr{1, 1}).has_value());
}