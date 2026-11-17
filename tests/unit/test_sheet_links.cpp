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
