// Protect Sheet and Protect Workbook (S3, s3-review: src/sheet/edit/Protection.h): Excel's
// password hash, the elements as Excel writes them, Excel's refusals on a protected sheet and
// workbook (measured in Excel 2021 through automation), saving and the recovery journal.
#include "TestHarness.h"

#include <cstdio>
#include <string>
#include <vector>

#include "SheetTestUtil.h"
#include "pkg/OfficeCrypto.h"
#include "sheet/edit/Edit.h"
#include "sheet/edit/Journal.h"
#include "sheet/edit/Protection.h"
#include "sheet/model/Workbook.h"
#include "sheet/xlsx/Xlsx.h"

using namespace fw::sheet;
using edit::CellInput;
using edit::Editor;
using edit::SheetProtection;
using edit::WorkbookProtection;
using namespace sheet_test;

namespace {

std::vector<uint8_t> SaveAll(Workbook& book, Editor& ed) {
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

std::string PartOf(const std::vector<uint8_t>& zip, const std::string& part) {
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(zip, st);
  std::vector<uint8_t> out;
  if (!pkg || !pkg->ReadPart(part, out).Ok()) return {};
  return test::Text(out);
}

// Sheet1: A1 "a" (locked, the default), B2 2 in an unlocked format (cellXfs 2); Sheet2 empty.
std::unique_ptr<Workbook> Book() {
  auto parts = WorkbookParts({"<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row><row r=\"2\"><c r=\"B2\" s=\"2\"><v>2</v></c>"
                              "<c r=\"C2\" s=\"2\"><v>3</v></c></row></sheetData>",
                              "<sheetData/>"});
  for (auto& [name, xml] : parts)
    if (name == "xl/styles.xml") {
      const std::string from = "<cellXfs count=\"2\"><xf/><xf numFmtId=\"14\" applyNumberFormat=\"1\"/></cellXfs>";
      xml.replace(xml.find(from), from.size(),
                  "<cellXfs count=\"3\"><xf/><xf numFmtId=\"14\" applyNumberFormat=\"1\"/>"
                  "<xf applyProtection=\"1\"><protection locked=\"0\"/></xf></cellXfs>");
    }
  return OpenZip(test::MakeZip(parts));
}

std::u16string Why(const edit::Result& r) { return r.ok ? std::u16string() : r.message; }

const std::u16string kLocked = edit::LockedCellMessage();

}  // namespace

// Excel 2021's own hashes (Protect with these passwords, saved, read from the file).
TEST(sheet_protection_hash_as_excel) {
  namespace c = fw::pkg::crypto;
  CHECK(c::ProtectionHash("SHA-512", "jQa1nQXinTBmdIHht9YOqQ==", L"abc", 100000) ==
        "oODGAdW5c2Xi6b4oSFkPwcudOUCal5lcyFzcWt+uQVoaTEICmfvEWYm7127dMJVMv/VuoOEKwkrIRA8sRm+PSQ==");
  CHECK(c::ProtectionHash("SHA-512", "FsNG/YA6Kb3gNG2kVI1AuA==", L"abc", 100000) ==
        "ErdUg+ZJwCG1cPIxQOiArFCoGunaGQjS9OmZeNeD2hjAlj+2fYbiyz5q5CHSaO0B4QfZPOPCwIPZC6aZe47qsQ==");
  CHECK(c::ProtectionHash("SHA-512", "1uGX5PnarggQlpmyJjSn9Q==", L"Pässwörd €", 100000) ==
        "BdYgtmkMXiABPAPqNlBSg3DE42rBF4vfSL4ugbei1xyhLmJdVmUTjjfUmQMTo2KkOpvNdZU3mGO4RRxuAgtHvw==");
  CHECK(c::ProtectionHash("MD4", "jQa1nQXinTBmdIHht9YOqQ==", L"abc", 1).empty());
  CHECK(c::ProtectionHash("SHA-512", "not base64!", L"abc", 1).empty());

  // Excel's element: the password matches, a wrong one doesn't.
  auto p = edit::ReadSheetProtection(
      "<sheetProtection algorithmName=\"SHA-512\" hashValue=\"oODGAdW5c2Xi6b4oSFkPwcudOUCal5lcyFzcWt+uQVoaTEICmfvEWYm7127dMJVMv/"
      "VuoOEKwkrIRA8sRm+PSQ==\" saltValue=\"jQa1nQXinTBmdIHht9YOqQ==\" spinCount=\"100000\" sheet=\"1\" objects=\"1\" scenarios=\"1\"/>");
  CHECK(p && p->sheet && p->password.Set());
  CHECK(p && p->password.Matches(L"abc") && !p->password.Matches(L"ABC") && !p->password.Matches(L""));
  // A new one: salted, 100,000 spins, and it matches.
  const edit::ProtectionPassword made = edit::ProtectionPassword::Make(L"s3cret");
  CHECK(made.algorithm == "SHA-512" && made.spinCount == 100000 && made.salt.size() == 24 && made.hash.size() == 88);
  CHECK(made.Matches(L"s3cret") && !made.Matches(L"secret"));
  CHECK(!edit::ProtectionPassword::Make(L"").Set() && edit::ProtectionPassword{}.Matches(L"anything"));
  // Excel 2010's 16-bit hash.
  edit::ProtectionPassword legacy;
  legacy.legacy = "CC1A";
  CHECK(legacy.Matches(L"abc") && !legacy.Matches(L"abd"));
}

// The elements as Excel 2021 writes them (Protect with its dialog's defaults, and with every
// permission given), and read back.
TEST(sheet_protection_elements) {
  CHECK_EQ(edit::SheetProtectionXml(SheetProtection{}), std::string("<sheetProtection sheet=\"1\" objects=\"1\" scenarios=\"1\"/>"));
  SheetProtection all;
  all.objects = all.scenarios = false;
  all.formatCells = all.formatColumns = all.formatRows = all.insertColumns = all.insertRows = all.insertHyperlinks =
      all.deleteColumns = all.deleteRows = all.sort = all.autoFilter = all.pivotTables = false;
  CHECK_EQ(edit::SheetProtectionXml(all, "x:"),
           std::string("<x:sheetProtection sheet=\"1\" formatCells=\"0\" formatColumns=\"0\" formatRows=\"0\" insertColumns=\"0\" "
                       "insertRows=\"0\" insertHyperlinks=\"0\" deleteColumns=\"0\" deleteRows=\"0\" sort=\"0\" autoFilter=\"0\" "
                       "pivotTables=\"0\"/>"));
  CHECK(edit::ReadSheetProtection(edit::SheetProtectionXml(all, "x:")) == all);
  // Left out: the schema's defaults (objects and scenarios not protected; the rest protected).
  auto bare = edit::ReadSheetProtection("<sheetProtection sheet=\"1\"/>");
  CHECK(bare && bare->sheet && !bare->objects && !bare->scenarios && bare->formatCells && bare->deleteRows &&
        !bare->selectLockedCells);
  // Unprotected, Excel keeps the selection flags: not a protected sheet.
  auto off = edit::ReadSheetProtection("<sheetProtection selectLockedCells=\"1\" selectUnlockedCells=\"1\"/>");
  CHECK(off && !off->sheet && off->selectLockedCells && off->selectUnlockedCells);
  CHECK(!edit::ReadSheetProtection("<sheetPr/>") && !edit::ReadSheetProtection(""));

  WorkbookProtection w;
  w.password = edit::ProtectionPassword::Make(L"abc");
  const std::string xml = edit::WorkbookProtectionXml(w);
  CHECK(xml.find("<workbookProtection workbookAlgorithmName=\"SHA-512\" workbookHashValue=\"") == 0 &&
        xml.find("workbookSpinCount=\"100000\" lockStructure=\"1\"/>") != std::string::npos);
  auto back = edit::ReadWorkbookProtection(xml);
  CHECK(back && *back == w && back->password.Matches(L"abc"));
  auto revisions = edit::ReadWorkbookProtection("<workbookProtection revisionsPassword=\"CC1A\" lockStructure=\"1\"/>");
  CHECK(revisions && revisions->lockStructure && revisions->other == " revisionsPassword=\"CC1A\"");
  CHECK(edit::WorkbookProtectionXml(*revisions) == "<workbookProtection revisionsPassword=\"CC1A\" lockStructure=\"1\"/>");
}

// What Excel refuses on a protected sheet with its dialog's defaults, and what each permission
// lets through (Excel 2021, through automation: tests in the order measured).
TEST(sheet_protection_refusals) {
  auto book = Book();
  if (!book) return;
  Editor ed(*book);
  const Sheet& s = book->GetSheet(0);
  CHECK(!edit::SheetProtected(s) && edit::CellLocked(s, book->GetStyles(), 0, 0) && !edit::CellLocked(s, book->GetStyles(), 1, 1));
  CHECK(ed.SetSheetProtection(0, SheetProtection{}));
  CHECK(edit::SheetProtected(s) && ed.UndoName() == u"Protect Sheet");
  CHECK(!edit::SheetProtected(book->GetSheet(1)));

  CHECK(Why(ed.SetCell(0, 0, 0, CellInput::Number(1))) == kLocked);  // locked A1
  CHECK(ed.SetCell(0, 1, 1, CellInput::Number(5)));                     // unlocked B2
  CHECK(Why(ed.SetCells(0, {{CellAddr{1, 1}, CellInput::Number(6)}, {CellAddr{2, 1}, CellInput::Number(7)}})) == kLocked);
  CHECK(ed.ClearContents(0, Range{1, 1, 1, 2}));                        // B2:C2, unlocked
  CHECK(Why(ed.ClearContents(0, Range{0, 0, 1, 1})) == kLocked);       // A1:B2, mixed
  CHECK(Why(ed.SetStyle(0, Range{1, 1, 1, 1}, 1)) == kLocked);         // formatting: not allowed
  CHECK(!ed.SetColumnWidth(0, 2, 2, 20) && !ed.SetRowHeight(0, 4, 4, 30) && !ed.HideRows(0, 5, 5, true));
  CHECK(!ed.InsertRows(0, 11, 1) && !ed.InsertColumns(0, 7, 1) && !ed.DeleteRows(0, 19, 1));
  const std::u16string command = Why(ed.Merge(0, Range{5, 1, 6, 1}));
  CHECK(command.find(u"You cannot use this command on a protected sheet.") == 0);
  CHECK(!ed.InsertCells(0, Range{9, 1, 9, 1}, Editor::Direction::DownOrUp));
  CHECK(!ed.SetRowOutline(0, 13, 14, 1));
  CHECK(!ed.SortRows(0, Range{1, 1, 2, 1}, {1, 0}));
  CHECK(!ed.SetFilter(0, SheetFilter{Range{0, 3, 4, 3}}));
  CHECK(ed.MoveCells(0, Range{1, 1, 1, 1}, CellAddr{1, 2}));           // cutting unlocked cells
  CHECK(Why(ed.MoveCells(0, Range{1, 2, 1, 2}, CellAddr{0, 0})) == kLocked);  // onto a locked one
  CHECK(ed.ClearFormats(0, Range{1, 2, 1, 2}));                        // unlocked: allowed, and now locked
  CHECK(edit::CellLocked(s, book->GetStyles(), 1, 2));
  CHECK(ed.FreezePanes(0, 1, 0));                                       // the view: allowed
  CHECK(ed.AddSheet(2) && ed.RenameSheet(2, "New"));                    // the workbook isn't protected

  // Every permission given (Excel's Protect with all the Allow arguments).
  CHECK(ed.Undo(5));  // back to before the cut
  SheetProtection all;
  all.objects = false;
  all.formatCells = all.formatColumns = all.formatRows = all.insertColumns = all.insertRows = all.insertHyperlinks =
      all.deleteColumns = all.deleteRows = all.sort = all.autoFilter = all.pivotTables = false;
  CHECK(ed.SetSheetProtection(0, all));
  CHECK(Why(ed.SetCell(0, 0, 0, CellInput::Number(1))) == kLocked);   // still: locked cells
  CHECK(ed.SetStyle(0, Range{0, 0, 0, 0}, 1));                          // formatting locked cells: allowed
  CHECK(ed.SetColumnWidth(0, 2, 2, 20) && ed.SetRowHeight(0, 4, 4, 30) && ed.HideRows(0, 5, 5, true));
  CHECK(ed.InsertRows(0, 11, 1) && ed.InsertColumns(0, 7, 1));
  CHECK(Why(ed.DeleteRows(0, 19, 1)) == kLocked);                      // its cells are locked
  CHECK(Why(ed.DeleteColumns(0, 6, 1)) == kLocked);
  CHECK(!ed.Merge(0, Range{5, 1, 6, 1}) && !ed.InsertCells(0, Range{9, 1, 9, 1}, Editor::Direction::DownOrUp));
  CHECK(Why(ed.SortRows(0, Range{0, 0, 1, 1}, {1, 0})) == kLocked);   // A1:B2 holds locked cells
  CHECK(ed.SortRows(0, Range{1, 1, 1, 2}, {0}));                        // B2:C2, unlocked
  CHECK(!ed.SetFilter(0, SheetFilter{Range{0, 3, 4, 3}}));             // turning a filter on: never
  // Locked and Hidden don't change on a protected sheet, even with formatting allowed (Excel).
  Xf open;
  open.protection.locked = false;
  open.applyProtection = true;
  const uint32_t unlockedXf = ed.AddXf(open);
  CHECK(Why(ed.SetStyle(0, Range{0, 0, 0, 0}, unlockedXf)) == kLocked);

  // Unprotected: everything again.
  CHECK(ed.SetSheetProtection(0, std::nullopt));
  CHECK(!edit::SheetProtected(s) && ed.UndoName() == u"Unprotect Sheet");
  CHECK(ed.SetCell(0, 0, 0, CellInput::Number(1)) && ed.Merge(0, Range{5, 1, 6, 1}));
  // Undo goes back through the protection, as any edit.
  CHECK(ed.Undo(3) && edit::SheetProtected(s));
}

// A whole column unlocked (its column format), and a row's own format.
TEST(sheet_protection_unlocked_columns_and_rows) {
  auto parts = WorkbookParts({"<cols><col min=\"4\" max=\"4\" width=\"9\" style=\"2\" customWidth=\"1\"/></cols>"
                              "<sheetData><row r=\"3\" s=\"2\" customFormat=\"1\"><c r=\"A3\" s=\"0\"><v>1</v></c></row></sheetData>"});
  for (auto& [name, xml] : parts)
    if (name == "xl/styles.xml") {
      const std::string from = "</cellXfs>";
      xml.replace(xml.find(from), from.size(), "<xf applyProtection=\"1\"><protection locked=\"0\"/></xf></cellXfs>");
      xml.replace(xml.find("cellXfs count=\"2\""), 17, "cellXfs count=\"3\"");
    }
  auto book = OpenZip(test::MakeZip(parts));
  if (!book) return;
  const Sheet& s = book->GetSheet(0);
  const Styles& st = book->GetStyles();
  CHECK(edit::AllUnlocked(s, st, Range{0, 3, kMaxRows - 1, 3}));       // column D
  CHECK(!edit::AllUnlocked(s, st, Range{0, 3, 0, 4}));                 // D1:E1
  CHECK(edit::AllUnlocked(s, st, Range{2, 1, 2, 10}));                 // B3:K3: row 3's format
  CHECK(!edit::AllUnlocked(s, st, Range{2, 0, 2, 10}));                // A3 has its own, locked
  CHECK(!edit::AllUnlocked(s, st, Range{2, 1, 3, 1}));                 // B3:B4
  Editor ed(*book);
  CHECK(ed.SetSheetProtection(0, SheetProtection{}));
  CHECK(ed.SetCell(0, 99999, 3, CellInput::Text("ok")) && ed.SetCell(0, 2, 7, CellInput::Text("ok")));
  CHECK(!ed.SetCell(0, 3, 7, CellInput::Text("no")));
}

TEST(sheet_protection_structure) {
  auto book = Book();
  if (!book) return;
  Editor ed(*book);
  CHECK(!edit::StructureProtected(*book));
  CHECK(ed.SetWorkbookProtection(WorkbookProtection{}));
  CHECK(edit::StructureProtected(*book) && ed.UndoName() == u"Protect Workbook");
  const std::u16string why = Why(ed.AddSheet(2));
  CHECK(why.find(u"The workbook's structure is protected") == 0);
  CHECK(!ed.RenameSheet(0, "X") && !ed.MoveSheet(0, 1) && !ed.DeleteSheet(1) && !ed.SetTabColor(0, Color{Color::Kind::Rgb, 0xFFFF0000}) &&
        !ed.SetSheetState(1, SheetState::Hidden));
  CHECK(ed.SetCell(0, 0, 0, CellInput::Number(9)));  // cells: as the sheet allows
  CHECK(ed.Undo(2) && !edit::StructureProtected(*book) && book->ProtectionXml().empty());
  CHECK(ed.Redo() && edit::StructureProtected(*book));
  CHECK(ed.SetWorkbookProtection(std::nullopt) && ed.AddSheet(2));
}

// Saved where the schema puts them, read back the same; an unchanged workbookProtection from the
// file stays as it was.
TEST(sheet_protection_saved) {
  auto book = Book();
  if (!book) return;
  Editor ed(*book);
  SheetProtection p;
  p.formatCells = false;
  p.password = edit::ProtectionPassword::Make(L"abc");
  WorkbookProtection w;
  w.password = edit::ProtectionPassword::Make(L"xyz");
  CHECK(ed.SetSheetProtection(0, p) && ed.SetWorkbookProtection(w));
  const std::vector<uint8_t> zip = SaveAll(*book, ed);
  const std::string sheet = PartOf(zip, "/xl/worksheets/sheet1.xml"), wb = PartOf(zip, "/xl/workbook.xml");
  CHECK(sheet.find("</sheetData><sheetProtection algorithmName=\"SHA-512\"") != std::string::npos);
  CHECK(wb.find("<workbookProtection workbookAlgorithmName=\"SHA-512\"") < wb.find("<sheets>"));
  auto again = OpenZip(zip);
  if (!again) return;
  auto readP = edit::SheetProtectionOf(again->GetSheet(0));
  auto readW = edit::WorkbookProtectionOf(*again);
  CHECK(readP && *readP == p && readP->password.Matches(L"abc"));
  CHECK(readW && *readW == w && readW->password.Matches(L"xyz") && !readW->password.Matches(L"abc"));

  // Edited elsewhere and saved again: the workbook's element is the file's, byte for byte.
  Editor ed2(*again);
  CHECK(ed2.SetSheetState(1, SheetState::Hidden).ok == false);  // the structure is protected
  CHECK(ed2.SetWorkbookProtection(std::nullopt) && ed2.SetSheetState(1, SheetState::Hidden));
  CHECK(ed2.Undo() && ed2.Undo() && ed2.SetCell(0, 1, 1, CellInput::Number(4)));
  const std::vector<uint8_t> zip2 = SaveAll(*again, ed2);
  CHECK(PartOf(zip2, "/xl/workbook.xml") == wb);
  // Removed: gone from the file.
  CHECK(ed2.SetWorkbookProtection(std::nullopt) && ed2.SetSheetProtection(0, std::nullopt));
  const std::vector<uint8_t> zip3 = SaveAll(*again, ed2);
  CHECK(PartOf(zip3, "/xl/workbook.xml").find("workbookProtection") == std::string::npos);
  CHECK(PartOf(zip3, "/xl/worksheets/sheet1.xml").find("sheetProtection") == std::string::npos);
}

TEST(sheet_protection_journal) {
  auto parts = WorkbookParts({"<sheetData/>", "<sheetData/>"});
  const std::vector<uint8_t> zip = test::MakeZip(parts);
  auto book = OpenZip(zip);
  if (!book) return;
  Editor ed(*book);
  edit::Journal journal;
  ed.SetJournal(&journal);
  SheetProtection p;
  p.insertRows = false;
  p.password = edit::ProtectionPassword::Make(L"pw");
  CHECK(ed.SetSheetProtection(1, p) && ed.SetWorkbookProtection(WorkbookProtection{}));
  CHECK(!ed.AddSheet(0));  // refused: not recorded
  auto again = OpenZip(zip);
  if (!again) return;
  Editor replayed(*again);
  size_t applied = 0;
  CHECK(replayed.Replay(journal.All(), &applied) && applied == 3);
  auto readP = edit::SheetProtectionOf(again->GetSheet(1));
  CHECK(readP && *readP == p && edit::StructureProtected(*again));
}

// Every permission, as Protect Sheet sets it, reads back from the XML the journal records
// (fuzz_xlsx: a replay found the sheet protected otherwise).
TEST(sheet_protection_xml_round_trip) {
  for (uint32_t bits = 0; bits < (1u << 17); bits += 1 + (bits % 7)) {
    SheetProtection p;
    bool* flags[] = {&p.sheet, &p.objects, &p.scenarios, &p.formatCells, &p.formatColumns, &p.formatRows, &p.insertColumns,
                     &p.insertRows, &p.insertHyperlinks, &p.deleteColumns, &p.deleteRows, &p.selectLockedCells, &p.sort,
                     &p.autoFilter, &p.pivotTables, &p.selectUnlockedCells};
    for (size_t i = 0; i < std::size(flags); ++i) *flags[i] = (bits >> i) & 1;
    if (bits & (1u << 16)) p.password.legacy = "CC1A";
    const std::optional<SheetProtection> back = edit::ReadSheetProtection(edit::SheetProtectionXml(p));
    if (!back || !(*back == p)) {
      std::fprintf(stderr, "bits %u: %s\n", bits, edit::SheetProtectionXml(p).c_str());
      CHECK(false);
      return;
    }
  }
}
