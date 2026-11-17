// Rows, columns and cells inserted and deleted through the reference registry (S2 step 4):
// formulas, names, merges, links, rules and the rest follow, as Excel does to the same file
// (measured with scratchpad probes; Refs.h has the facts), and undo puts everything back.
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "SheetTestUtil.h"
#include "TestHarness.h"
#include "sheet/edit/Edit.h"
#include "sheet/edit/Journal.h"
#include "sheet/edit/Refs.h"

using namespace fw::sheet;
using namespace sheet_test;
using edit::CellInput;
using edit::Editor;

namespace {

std::string Row(int r, const std::string& cells) { return "<row r=\"" + std::to_string(r) + "\">" + cells + "</row>"; }
std::string Num(const char* ref, int v) { return std::string("<c r=\"") + ref + "\"><v>" + std::to_string(v) + "</v></c>"; }
std::string F(const char* ref, const char* f) { return std::string("<c r=\"") + ref + "\"><f>" + f + "</f></c>"; }

// Sheet1: numbers in A1:A10; J1 =A5, J2 =SUM(A2:A10), J3 =$A$5, J4 =Sheet2!A1; a shared formula
// K1:K3 =A1*2. Sheet2: B1 =Sheet1!A5, B2 =SUM(Sheet1!A5:A6).
std::unique_ptr<Workbook> Book(const std::string& afterData = {}) {
  std::string data = "<sheetData>";
  for (int r = 1; r <= 10; ++r) {
    std::string a = "A" + std::to_string(r);
    std::string cells = Num(a.c_str(), r * 10);
    if (r == 1) cells += F("J1", "A5") + "<c r=\"K1\"><f t=\"shared\" ref=\"K1:K3\" si=\"0\">A1*2</f></c>";
    if (r == 2) cells += F("J2", "SUM(A2:A10)") + "<c r=\"K2\"><f t=\"shared\" si=\"0\"/></c>";
    if (r == 3) cells += F("J3", "$A$5") + "<c r=\"K3\"><f t=\"shared\" si=\"0\"/></c>";
    if (r == 4) cells += F("J4", "Sheet2!A1");
    data += Row(r, cells);
  }
  data += "</sheetData>";
  return OpenParts({data + afterData,
                    "<sheetData>" + Row(1, F("B1", "Sheet1!A5")) + Row(2, F("B2", "SUM(Sheet1!A5:A6)")) + "</sheetData>"});
}

}  // namespace

TEST(sheet_shift_rows_formulas_names_undo) {
  auto book = Book();
  Editor ed(*book);
  const Sheet& s1 = book->GetSheet(0);
  const Sheet& s2 = book->GetSheet(1);
  CHECK(ed.InsertRows(0, 3, 2));  // two rows before row 4
  CHECK_EQ(Text(s1, 5, 0), std::string("40"));  // A4 is A6 now
  CHECK_EQ(Text(s1, 3, 0), std::string(""));
  CHECK_EQ(FormulaText(s1, 0, 9), std::string("A7"));
  CHECK_EQ(FormulaText(s1, 1, 9), std::string("SUM(A2:A12)"));
  CHECK_EQ(FormulaText(s1, 2, 9), std::string("$A$7"));
  CHECK_EQ(FormulaText(s1, 5, 9), std::string("Sheet2!A1"));  // its cell moved; the other sheet's cell didn't
  CHECK_EQ(FormulaText(s2, 0, 1), std::string("Sheet1!A7"));
  CHECK_EQ(FormulaText(s2, 1, 1), std::string("SUM(Sheet1!A7:A8)"));
  CHECK_EQ(book->Names()[0].formula, std::string("Sheet1!$A$1"));
  // The shared formula above the edit is still shared (a follower shows its master's text).
  CHECK_EQ(FormulaText(s1, 2, 10), std::string("A1*2"));
  CHECK(ed.Undo());
  CHECK_EQ(Text(s1, 3, 0), std::string("40"));
  CHECK_EQ(FormulaText(s1, 0, 9), std::string("A5"));
  CHECK_EQ(FormulaText(s1, 3, 9), std::string("Sheet2!A1"));
  CHECK_EQ(FormulaText(s2, 1, 1), std::string("SUM(Sheet1!A5:A6)"));
  CHECK(ed.Redo());
  CHECK_EQ(FormulaText(s1, 0, 9), std::string("A7"));
}

TEST(sheet_shift_delete_rows_ref_errors) {
  auto book = Book();
  Editor ed(*book);
  const Sheet& s1 = book->GetSheet(0);
  CHECK(ed.DeleteRows(0, 4, 2));  // rows 5 and 6
  CHECK_EQ(FormulaText(s1, 0, 9), std::string("#REF!"));
  CHECK_EQ(FormulaText(s1, 1, 9), std::string("SUM(A2:A8)"));
  CHECK_EQ(FormulaText(book->GetSheet(1), 1, 1), std::string("SUM(Sheet1!#REF!)"));
  CHECK_EQ(Text(s1, 4, 0), std::string("70"));
}

TEST(sheet_shift_shared_formula_split) {
  auto book = Book();
  Editor ed(*book);
  const Sheet& s1 = book->GetSheet(0);
  // Cells K2:K10 moved right of a band that leaves column A: K2 now reads A2 from two columns further.
  CHECK(ed.InsertCells(0, Range{1, 10, 1, 11}, Editor::Direction::RightOrLeft));
  CHECK_EQ(FormulaText(s1, 1, 12), std::string("A2*2"));  // a formula of its own now
  CHECK_EQ(FormulaText(s1, 2, 10), std::string("A1*2"));  // still shared
  CHECK(ed.Undo());
  CHECK_EQ(FormulaText(s1, 1, 10), std::string("A1*2"));
}

TEST(sheet_shift_merges_links_rules) {
  auto book = Book(
      "<mergeCells count=\"1\"><mergeCell ref=\"C5:D6\"/></mergeCells>"
      "<conditionalFormatting sqref=\"B1:B10\"><cfRule type=\"expression\" priority=\"1\"><formula>B1&gt;A1</formula></cfRule>"
      "</conditionalFormatting>"
      "<dataValidations count=\"1\"><dataValidation type=\"list\" sqref=\"E1:E3\"><formula1>$H$1:$H$5</formula1>"
      "</dataValidation></dataValidations>"
      "<hyperlinks><hyperlink ref=\"G5\" location=\"Sheet2!A1\" display=\"x\"/></hyperlinks>");
  Editor ed(*book);
  const Sheet& s1 = book->GetSheet(0);
  CHECK(ed.InsertRows(0, 3, 1));  // before row 4
  CHECK(s1.Merges().size() == 1 && s1.Merges()[0] == (Range{5, 2, 6, 3}));
  CHECK(s1.Hyperlinks().size() == 1 && s1.Hyperlinks()[0].ref == Range::Cell(5, 6));
  CHECK_EQ(s1.Hyperlinks()[0].location, std::string("Sheet2!A1"));  // Excel leaves the text
  const std::string& kept = s1.Text().afterData;
  CHECK(kept.find("sqref=\"B1:B11\"") != std::string::npos);
  CHECK(kept.find("sqref=\"E1:E4\"") != std::string::npos);  // took in the row inserted under it
  CHECK(kept.find("$H$1:$H$6") != std::string::npos);
  CHECK(ed.Undo());
  CHECK(s1.Merges()[0] == (Range{4, 2, 5, 3}));
  CHECK(s1.Text().afterData.find("sqref=\"B1:B10\"") != std::string::npos);
  // Cells shifted down under B: the rule's moved cells now read A two rows up: Excel splits it.
  CHECK(ed.InsertCells(0, Range{4, 1, 5, 1}, Editor::Direction::DownOrUp));
  const std::string& split = s1.Text().afterData;
  CHECK(split.find("sqref=\"B1:B6\"") != std::string::npos);
  CHECK(split.find("sqref=\"B7:B12\"") != std::string::npos);
  CHECK(split.find("B7&gt;A5") != std::string::npos);
}

TEST(sheet_shift_formats_follow_inserted_rows_and_columns) {
  auto book = OpenParts({"<cols><col min=\"2\" max=\"2\" width=\"20\" customWidth=\"1\"/></cols><sheetData>"
                         "<row r=\"2\" ht=\"30\" customHeight=\"1\"><c r=\"A2\" s=\"1\"><v>1</v></c></row>"
                         "<row r=\"3\"><c r=\"A3\"><v>2</v></c></row></sheetData>"});
  Editor ed(*book);
  const Sheet& s = book->GetSheet(0);
  CHECK(ed.InsertRows(0, 2, 1));  // under row 2: Excel's "Format Same As Above"
  CHECK(s.Row(2).customHeight && s.Row(2).height == 30);
  CHECK_EQ(s.StyleAt(2, 0), 1u);
  CHECK_EQ(Text(s, 3, 0), std::string("2"));
  CHECK(ed.InsertColumns(0, 2, 2));  // right of B: its width
  CHECK(s.Column(2).width == 20 && s.Column(3).width == 20 && s.Column(4).width == 0);
  CHECK(ed.DeleteColumns(0, 0, 1));
  CHECK(s.Column(0).width == 20 && s.Column(3).width == 0);
}

TEST(sheet_shift_refusals) {
  auto book = OpenParts({"<sheetData><row r=\"1\"><c r=\"A1\"><f t=\"array\" ref=\"A1:A3\">B1:B3*2</f></c></row>"
                         "<row r=\"2\"><c r=\"A2\"/></row><row r=\"3\"><c r=\"A3\"/></row></sheetData>"
                         "<mergeCells count=\"1\"><mergeCell ref=\"C5:D6\"/></mergeCells>"});
  Editor ed(*book);
  CHECK(!ed.InsertRows(0, 1, 1));  // through the array
  CHECK(!ed.DeleteRows(0, 2, 1));
  CHECK(!ed.InsertCells(0, Range{3, 2, 3, 2}, Editor::Direction::DownOrUp));  // part of the merge
  CHECK(ed.InsertRows(0, 3, 1));  // under the array, through the merge: it grows
  CHECK(book->GetSheet(0).Merges()[0] == (Range{5, 2, 6, 3}));
}

TEST(sheet_shift_table_columns) {
  // DropTableColumns: structured references to a deleted column become #REF!, others stay.
  CHECK_EQ(edit::refs::DropTableColumns("Table1[[#This Row],[n]]+Table1[[#This Row],[o]]", "Table1", {"n"}),
           std::string("#REF!+Table1[[#This Row],[o]]"));
  CHECK_EQ(edit::refs::DropTableColumns("SUM(table1[N])+\"Table1[n]\"", "Table1", {"n"}),
           std::string("SUM(#REF!)+\"Table1[n]\""));
  CHECK_EQ(edit::refs::DropTableColumns("Table12[n]", "Table1", {"n"}), std::string("Table12[n]"));
}

TEST(sheet_shift_saves_clean) {
  auto book = Book("<conditionalFormatting sqref=\"A1:A10\"><cfRule type=\"cellIs\" priority=\"1\" operator=\"greaterThan\">"
                   "<formula>5</formula></cfRule></conditionalFormatting>");
  Editor ed(*book);
  CHECK(ed.InsertRows(0, 0, 3));
  CHECK(ed.DeleteColumns(0, 1, 2));
  CHECK(ed.InsertCells(0, Range{2, 0, 3, 0}, Editor::Direction::DownOrUp));
  ed.BeginSave();
  xlsx::SavePlan plan;
  fw::pkg::Status st = xlsx::PrepareSave(*book, ed, plan);
  CHECK(st.Ok());
  fw::pkg::MemorySink sink;
  if (st.Ok()) st = xlsx::WriteSave(*book, plan, sink);
  CHECK(st.Ok());
  if (st.Ok()) xlsx::CommitSave(*book, ed, plan);
  else ed.EndSave();
  auto again = OpenZip(sink.bytes);
  CHECK(again != nullptr);
  if (!again) return;
  // J1 is H4 after three rows and two columns; its A5 became A8, then A10 under the cells shifted down in A.
  CHECK_EQ(FormulaText(again->GetSheet(0), 3, 7), FormulaText(book->GetSheet(0), 3, 7));
  CHECK_EQ(FormulaText(again->GetSheet(0), 3, 7), std::string("A10"));
}

// ---- Sheet operations (measured against Excel with scratchpad probe run3)

namespace {

// Sheet1: A1 =Sheet2!A1+1, A2 =SUM(Sheet1:Sheet3!A1), A3 =SUM(Sheet2:Sheet3!A1); a link to Sheet2!A1
// in B1; a sparkline on Sheet2!A1:A3. Sheet2 and Sheet3: numbers in A1.
std::unique_ptr<Workbook> ThreeSheets() {
  std::string sparkline =
      "<extLst><ext uri=\"{05C60535-1F16-4fd2-B633-F4F36F0B64E0}\" "
      "xmlns:x14=\"http://schemas.microsoft.com/office/spreadsheetml/2009/9/main\"><x14:sparklineGroups "
      "xmlns:xm=\"http://schemas.microsoft.com/office/excel/2006/main\"><x14:sparklineGroup><x14:sparklines><x14:sparkline>"
      "<xm:f>Sheet2!A1:A3</xm:f><xm:sqref>K25</xm:sqref></x14:sparkline></x14:sparklines></x14:sparklineGroup>"
      "</x14:sparklineGroups></ext></extLst>";
  return OpenParts({"<sheetData>" + Row(1, F("A1", "Sheet2!A1+1")) + Row(2, F("A2", "SUM(Sheet1:Sheet3!A1)")) +
                        Row(3, F("A3", "SUM(Sheet2:Sheet3!A1)")) + "</sheetData><hyperlinks><hyperlink ref=\"B1\" "
                        "location=\"Sheet2!A1\" display=\"to two\"/></hyperlinks>" + sparkline,
                    "<sheetData>" + Row(1, Num("A1", 2)) + "</sheetData>", "<sheetData>" + Row(1, Num("A1", 3)) + "</sheetData>"});
}

}  // namespace

TEST(sheet_ops_delete_sheet_refs) {
  auto book = ThreeSheets();
  if (!book) return;
  Editor ed(*book);
  CHECK(ed.SetName(DefinedName{"OnTwo", -1, "Sheet2!$A$1:$A$3", false}));
  CHECK(ed.DeleteSheet(1));
  const Sheet& s1 = book->GetSheet(0);
  CHECK_EQ(FormulaText(s1, 0, 0), std::string("#REF!+1"));
  CHECK_EQ(FormulaText(s1, 1, 0), std::string("SUM(Sheet1:Sheet3!A1)"));  // a 3-D end that goes gives way
  CHECK_EQ(FormulaText(s1, 2, 0), std::string("SUM(Sheet3!A1)"));
  CHECK(book->FindName("OnTwo") && book->FindName("OnTwo")->formula == "#REF!");
  CHECK(!book->FindName("Local", 1) && !book->FindName("Local", 0));  // its own names go with it
  CHECK(book->FindName("Total") && book->FindName("Total")->formula == "Sheet1!$A$1");
  CHECK(s1.Hyperlinks().size() == 1 && s1.Hyperlinks()[0].location == "Sheet2!A1");  // Excel leaves it
  CHECK(s1.Text().afterData.find("<xm:sqref>K25</xm:sqref>") != std::string::npos);
  CHECK(s1.Text().afterData.find("<xm:f>") == std::string::npos);  // the sparkline's data is gone
  CHECK(ed.Undo());
  CHECK(book->SheetCount() == 3 && book->GetSheet(1).Name() == "Sheet2");
  CHECK_EQ(FormulaText(book->GetSheet(0), 0, 0), std::string("Sheet2!A1+1"));
  CHECK_EQ(FormulaText(book->GetSheet(0), 2, 0), std::string("SUM(Sheet2:Sheet3!A1)"));
  CHECK(book->FindName("OnTwo")->formula == "Sheet2!$A$1:$A$3" && book->FindName("Local", 1));
  CHECK(book->GetSheet(0).Text().afterData.find("<xm:f>Sheet2!A1:A3</xm:f>") != std::string::npos);
  CHECK(ed.Redo());
  CHECK_EQ(FormulaText(book->GetSheet(0), 0, 0), std::string("#REF!+1"));
}

TEST(sheet_ops_move_sheet_3d) {
  auto book = ThreeSheets();
  if (!book) return;
  Editor ed(*book);
  CHECK(ed.MoveSheet(0, 2));  // Sheet1 last: Sheet2, Sheet3, Sheet1
  const Sheet& s1 = book->GetSheet(2);
  CHECK(s1.Name() == "Sheet1");
  CHECK_EQ(FormulaText(s1, 0, 0), std::string("Sheet2!A1+1"));
  CHECK_EQ(FormulaText(s1, 1, 0), std::string("SUM(Sheet2:Sheet3!A1)"));  // its end moved out of the span
  CHECK_EQ(FormulaText(s1, 2, 0), std::string("SUM(Sheet2:Sheet3!A1)"));
  CHECK(book->FindName("Local", 0) != nullptr);  // Sheet2's own name follows it
  CHECK(ed.Undo());
  CHECK_EQ(FormulaText(book->GetSheet(0), 1, 0), std::string("SUM(Sheet1:Sheet3!A1)"));
  CHECK(ed.MoveSheet(2, 0));  // Sheet3 first; an end moved out of the span shrinks it
  CHECK_EQ(FormulaText(book->GetSheet(1), 1, 0), std::string("SUM(Sheet1:Sheet2!A1)"));
  CHECK_EQ(FormulaText(book->GetSheet(1), 2, 0), std::string("SUM(Sheet2!A1)"));
}

TEST(sheet_ops_rename_everywhere_but_links) {
  auto book = ThreeSheets();
  if (!book) return;
  Editor ed(*book);
  CHECK(ed.RenameSheet(1, "Data & More"));
  const Sheet& s1 = book->GetSheet(0);
  CHECK_EQ(FormulaText(s1, 0, 0), std::string("'Data & More'!A1+1"));
  CHECK_EQ(FormulaText(s1, 2, 0), std::string("SUM('Data & More:Sheet3'!A1)"));
  CHECK(s1.Text().afterData.find("<xm:f>'Data &amp; More'!A1:A3</xm:f>") != std::string::npos);
  CHECK(s1.Hyperlinks()[0].location == "Sheet2!A1");
  CHECK(book->FindName("Local", 1)->formula == "'Data & More'!$B$2");
  CHECK(ed.Undo());
  CHECK(book->GetSheet(0).Text().afterData.find("<xm:f>Sheet2!A1:A3</xm:f>") != std::string::npos);
}

// With FW_SHIFT_BENCH set to an .xlsx (build\budgets\million-cells.xlsx), prints how long the
// structural edits take on its first sheet (plan section 11: 100 ms for inserting a row).
TEST(sheet_shift_timing) {
  wchar_t path[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"FW_SHIFT_BENCH", path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return;
  std::ifstream in(path, std::ios::binary);
  auto book = OpenZip(std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
  if (!book) return;
  Editor ed(*book);
  auto time = [&](const char* what, const std::function<bool()>& fn) {
    auto t0 = std::chrono::steady_clock::now();
    bool ok = fn();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  %-28s %8.1f ms%s\n", what, ms, ok ? "" : " (refused)");
  };
  std::printf("  %zu cells on the first sheet\n", book->GetSheet(0).CellCount());
  for (int round = 0; round < 2; ++round) {
    time("insert a row at 100", [&] { return static_cast<bool>(ed.InsertRows(0, 99, 1)); });
    time("undo", [&] { return static_cast<bool>(ed.Undo()); });
    time("delete a row at 100", [&] { return static_cast<bool>(ed.DeleteRows(0, 99, 1)); });
    time("undo", [&] { return static_cast<bool>(ed.Undo()); });
    time("insert a column at C", [&] { return static_cast<bool>(ed.InsertColumns(0, 2, 1)); });
    time("undo", [&] { return static_cast<bool>(ed.Undo()); });
    time("shift cells down at B100:C100", [&] {
      return static_cast<bool>(ed.InsertCells(0, Range{99, 1, 99, 2}, Editor::Direction::DownOrUp));
    });
    time("undo", [&] { return static_cast<bool>(ed.Undo()); });
  }
}

// ---- Sorting (measured against Excel with scratchpad probes sort1 and sort2)

namespace {

// Rows 1-4: A ids 1..4, B 30 10 40 20, C =B*2, D =B of the next row, E =Sheet1!B (a sheet name),
// F2 =A1, G shared =A+1; H1 =B2 outside; a link on B2; A2 formatted.
std::unique_ptr<Workbook> SortBook(const std::string& extra = {}) {
  const int b[] = {30, 10, 40, 20};
  std::string data = "<sheetData>";
  for (int r = 1; r <= 4; ++r) {
    std::string n = std::to_string(r), next = std::to_string(r + 1);
    std::string cells = "<c r=\"A" + n + "\"" + (r == 2 ? " s=\"1\"" : "") + "><v>" + n + "</v></c>" +
                        "<c r=\"B" + n + "\"><v>" + std::to_string(b[r - 1]) + "</v></c>" + F(("C" + n).c_str(), ("B" + n + "*2").c_str()) +
                        F(("D" + n).c_str(), ("B" + next).c_str()) + F(("E" + n).c_str(), ("Sheet1!B" + n).c_str());
    if (r == 2) cells += F("F2", "A1");
    cells += r == 1 ? std::string("<c r=\"G1\"><f t=\"shared\" ref=\"G1:G4\" si=\"0\">A1+1</f></c>")
                    : "<c r=\"G" + n + "\"><f t=\"shared\" si=\"0\"/></c>";
    if (r == 1) cells += F("H1", "B2");
    data += Row(r, cells);
  }
  data += "</sheetData>" + extra + "<hyperlinks><hyperlink ref=\"B2\" location=\"Sheet2!A1\" display=\"x\"/></hyperlinks>";
  return OpenParts({data, "<sheetData/>"});
}

}  // namespace

TEST(sheet_sort_rows_as_excel) {
  auto book = SortBook();
  if (!book) return;
  Editor ed(*book);
  const Sheet& s = book->GetSheet(0);
  CHECK(ed.SortRows(0, Range{0, 0, 3, 6}, {1, 3, 0, 2}));  // by B, ascending
  CHECK(Text(s, 0, 0) == "2" && Text(s, 1, 0) == "4" && Text(s, 2, 0) == "1" && Text(s, 3, 0) == "3");
  CHECK_EQ(Text(s, 0, 1), std::string("10"));
  CHECK_EQ(FormulaText(s, 0, 2), std::string("B1*2"));    // copied to its new row
  CHECK_EQ(FormulaText(s, 1, 3), std::string("B3"));      // was B5 on row 4
  CHECK_EQ(FormulaText(s, 2, 3), std::string("B4"));      // was B2 on row 1
  CHECK_EQ(FormulaText(s, 0, 4), std::string("Sheet1!B2"));  // a sheet name: it stays
  CHECK_EQ(FormulaText(s, 0, 5), std::string("#REF!"));     // A1 from row 2 to row 1: off the sheet
  CHECK_EQ(FormulaText(s, 0, 6), std::string("A1+1"));
  FormulaInfo info;
  CHECK(s.GetFormula(2, 6, info) && info.kind == FormulaKind::Normal);  // no longer shared
  CHECK_EQ(FormulaText(s, 0, 7), std::string("B2"));  // references from outside don't follow
  CHECK(s.StyleAt(0, 0) == 1 && s.StyleAt(1, 0) == 0);  // the format went with A2
  CHECK(s.Hyperlinks().size() == 1 && s.Hyperlinks()[0].ref == Range::Cell(0, 1));
  CHECK(ed.Undo());
  CHECK(Text(s, 0, 0) == "1" && Text(s, 0, 1) == "30");
  CHECK_EQ(FormulaText(s, 0, 3), std::string("B2"));
  CHECK_EQ(FormulaText(s, 1, 5), std::string("A1"));
  CHECK(s.GetFormula(2, 6, info) && info.kind == FormulaKind::Shared);
  CHECK(s.Hyperlinks()[0].ref == Range::Cell(1, 1) && s.StyleAt(1, 0) == 1);
  CHECK(ed.Redo());
  CHECK_EQ(FormulaText(s, 1, 3), std::string("B3"));
}

TEST(sheet_sort_columns_and_keys) {
  auto book = OpenParts({"<sheetData>" + Row(1, Num("A1", 3) + Num("B1", 1) + Num("C1", 2)) +
                         Row(2, F("A2", "A1*10") + F("B2", "B1*10") + F("C2", "C1*10")) + "</sheetData>",
                         "<sheetData/>"});  // two sheets: the fixture's name Local is on the second
  if (!book) return;
  Editor ed(*book);
  const Sheet& s = book->GetSheet(0);
  CHECK(ed.SortColumns(0, Range{0, 0, 1, 2}, {1, 2, 0}, {{0, false}}));
  CHECK(Text(s, 0, 0) == "1" && Text(s, 0, 1) == "2" && Text(s, 0, 2) == "3");
  CHECK_EQ(FormulaText(s, 1, 0), std::string("A1*10"));
  CHECK_EQ(FormulaText(s, 1, 2), std::string("C1*10"));
  CHECK(s.Text().afterData.find("<sortState columnSort=\"1\" ref=\"A1:C2\"><sortCondition ref=\"A1:C1\"/></sortState>") !=
        std::string::npos);
  CHECK(ed.Undo());
  CHECK(s.Text().afterData.find("sortState") == std::string::npos && Text(s, 0, 0) == "3");
  CHECK(ed.SortRows(0, Range{0, 0, 1, 2}, {0, 1}, {{1, true}}));  // the order as it is: only the record
  CHECK(s.Text().afterData.find("<sortCondition descending=\"1\" ref=\"B1:B2\"/>") != std::string::npos);
  // Saved and read again, the record is there (FW_SAVE_OUT=<folder> keeps the file, for Excel).
  ed.BeginSave();
  xlsx::SavePlan plan;
  fw::pkg::Status st = xlsx::PrepareSave(*book, ed, plan);
  fw::pkg::MemorySink sink;
  if (st.Ok()) st = xlsx::WriteSave(*book, plan, sink);
  CHECK(st.Ok());
  if (st.Ok()) xlsx::CommitSave(*book, ed, plan);
  else ed.EndSave();
  auto again = OpenZip(sink.bytes);
  CHECK(again && again->GetSheet(0).Text().afterData.find("<sortState ref=\"A1:C2\">") != std::string::npos);
  wchar_t dir[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"FW_SAVE_OUT", dir, MAX_PATH);
  if (n > 0 && n < MAX_PATH) CHECK(test::WriteFileBytes(std::wstring(dir) + L"\\sort-keys.xlsx", sink.bytes));
}

TEST(sheet_sort_refusals) {
  auto merged = SortBook("<mergeCells count=\"1\"><mergeCell ref=\"C3:D3\"/></mergeCells>");
  if (!merged) return;
  Editor ed(*merged);
  edit::Result r = ed.SortRows(0, Range{0, 0, 3, 6}, {3, 2, 1, 0});
  CHECK(!r && r.message == u"To do this, all the merged cells need to be the same size.");
  CHECK(!ed.SortRows(0, Range{0, 0, 3, 6}, {0, 1, 2}));     // too few
  CHECK(!ed.SortRows(0, Range{0, 0, 3, 6}, {0, 1, 1, 2}));  // not a permutation
  CHECK(!ed.SortRows(0, Range{0, 0, 3, 6}, {3, 2, 1, 0}, {{9, false}}));  // the key outside
  CHECK(ed.SortRows(0, Range{0, 0, 3, 1}, {3, 2, 1, 0}));  // beside the merge
  auto arrays = OpenParts({"<sheetData>" + Row(1, Num("A1", 1) + "<c r=\"B1\"><f t=\"array\" ref=\"B1:B2\">A1:A2*2</f><v>2</v></c>") +
                           Row(2, Num("A2", 2) + "<c r=\"B2\"><v>4</v></c>") + "</sheetData>"});
  if (!arrays) return;
  Editor ea(*arrays);
  r = ea.SortRows(0, Range{0, 0, 1, 1}, {1, 0});
  CHECK(!r && r.message == u"You can't change part of an array.");
  CHECK(ea.SortRows(0, Range{0, 0, 1, 0}, {1, 0}));
}

TEST(sheet_edit_formula_naming_missing_sheet) {
  auto book = OpenParts({"<sheetData/>", "<sheetData/>"});
  if (!book) return;
  Editor ed(*book);
  CellInput in;
  in.formula = "Nowhere!A1+1";  // Excel would look for a workbook called Nowhere
  edit::Result r = ed.SetCell(0, 0, 0, in);
  CHECK(!r && r.message == u"The formula names a sheet this workbook doesn't have.");
  in.formula = "SUM(Sheet1:Nowhere!A1)";
  CHECK(!ed.SetCell(0, 0, 0, in));
  in.formula = "'sheet2'!A1+1";
  CHECK(ed.SetCell(0, 0, 0, in));
  in.formula = "[1]Sheet1!A1";  // another workbook's: not this check's business
  CHECK(ed.SetCell(0, 0, 1, in));
}

// ---- Tables' headers typed over, and spills blocked (measured: scratchpad probes header1, spill1)

namespace {

std::string Str(const char* ref, const char* text) {
  return std::string("<c r=\"") + ref + "\" t=\"inlineStr\"><is><t>" + text + "</t></is></c>";
}

// Table T over A1:C4 (a, b, c; c is calculated from b); E2 =SUM(T[b]). With `spill`, H2 spills
// =TRANSPOSE(A2:A5) down to H5 (a dynamic array: metadata.xml marks cm 1).
std::unique_ptr<Workbook> TableBook(bool spill = false) {
  std::string data = "<sheetData>" + Row(1, Str("A1", "a") + Str("B1", "b") + Str("C1", "c"));
  for (int r = 2; r <= 5; ++r) {
    std::string n = std::to_string(r);
    std::string cells = r <= 4 ? Num(("A" + n).c_str(), r) + Num(("B" + n).c_str(), r * 10) +
                                     F(("C" + n).c_str(), "T[[#This Row],[b]]*2")
                               : Num(("A" + n).c_str(), r);
    if (r == 2) cells += F("E2", "SUM(T[b])");
    if (spill) cells += r == 2 ? std::string("<c r=\"H2\" cm=\"1\"><f t=\"array\" ref=\"H2:H5\">TRANSPOSE(A2:A5)</f><v>2</v></c>")
                               : "<c r=\"H" + n + "\"><v>" + n + "</v></c>";
    data += Row(r, cells);
  }
  data += "</sheetData><tableParts count=\"1\"><tablePart r:id=\"rT\"/></tableParts>";
  auto parts = WorkbookParts({data, "<sheetData/>"});
  for (auto& [name, xml] : parts) {
    if (name == "[Content_Types].xml")
      xml.insert(xml.find("</Types>"),
                 "<Override PartName=\"/xl/tables/table1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.table+xml\"/>"
                 "<Override PartName=\"/xl/metadata.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheetMetadata+xml\"/>");
    if (name == "xl/_rels/workbook.xml.rels")
      xml.insert(xml.find("</Relationships>"),
                 "<Relationship Id=\"rM\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sheetMetadata\" Target=\"metadata.xml\"/>");
    if (name == "xl/worksheets/sheet1.xml")
      xml.replace(0, xml.find('>'), "<worksheet xmlns=\"" + std::string(kMain) +
                                        "\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"");
  }
  parts.push_back({"xl/worksheets/_rels/sheet1.xml.rels",
                   "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rT\" "
                   "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/table\" Target=\"../tables/table1.xml\"/>"
                   "</Relationships>"});
  parts.push_back({"xl/tables/table1.xml",
                   "<table xmlns=\"" + std::string(kMain) + "\" id=\"1\" name=\"T\" displayName=\"T\" ref=\"A1:C4\"><autoFilter ref=\"A1:C4\"/>"
                   "<tableColumns count=\"3\"><tableColumn id=\"1\" name=\"a\"/><tableColumn id=\"2\" name=\"b\"/><tableColumn id=\"3\" name=\"c\">"
                   "<calculatedColumnFormula>T[[#This Row],[b]]*2</calculatedColumnFormula></tableColumn></tableColumns></table>"});
  parts.push_back({"xl/metadata.xml",
                   "<metadata xmlns=\"" + std::string(kMain) + "\" xmlns:xda=\"http://schemas.microsoft.com/office/spreadsheetml/2017/dynamicarray\">"
                   "<metadataTypes count=\"1\"><metadataType name=\"XLDAPR\" minSupportedVersion=\"120000\" copy=\"1\" pasteAll=\"1\" "
                   "pasteValues=\"1\" merge=\"1\" splitFirst=\"1\" rowColShift=\"1\" clearFormats=\"1\" clearComments=\"1\" assign=\"1\" "
                   "coerce=\"1\" cellMeta=\"1\"/></metadataTypes><futureMetadata name=\"XLDAPR\" count=\"1\"><bk><extLst><ext "
                   "uri=\"{bdbb8cdc-fa1e-496e-a857-3c3f30c029c3}\"><xda:dynamicArrayProperties fDynamic=\"1\" fCollapsed=\"0\"/></ext>"
                   "</extLst></bk></futureMetadata><cellMetadata count=\"1\"><bk><rc t=\"1\" v=\"0\"/></bk></cellMetadata></metadata>"});
  return OpenZip(test::MakeZip(parts));
}

std::string TablePart(Workbook& book) {
  std::vector<uint8_t> bytes;
  if (!book.GetPackage() || !book.GetPackage()->ReadPart("/xl/tables/table1.xml", bytes).Ok()) return {};
  return std::string(bytes.begin(), bytes.end());
}

}  // namespace

TEST(sheet_table_header_typed_over) {
  auto book = TableBook();
  if (!book) return;
  Editor ed(*book);
  const Sheet& s = book->GetSheet(0);
  CHECK(s.Tables().size() == 1);
  if (s.Tables().empty()) return;
  CHECK(ed.SetCell(0, 0, 1, CellInput::Text("new")));
  CHECK(s.Tables()[0].columns[1].name == "new");
  CHECK_EQ(FormulaText(s, 1, 4), std::string("SUM(T[new])"));
  CHECK_EQ(FormulaText(s, 1, 2), std::string("T[[#This Row],[new]]*2"));
  CHECK(s.Tables()[0].columns[2].calculatedFormula == "T[[#This Row],[new]]*2");
  CHECK(TablePart(*book).find("name=\"new\"") != std::string::npos);
  CHECK(ed.Undo());
  CHECK(s.Tables()[0].columns[1].name == "b" && Text(s, 0, 1) == "b");
  CHECK_EQ(FormulaText(s, 1, 4), std::string("SUM(T[b])"));
  CHECK(TablePart(*book).find("name=\"b\"") != std::string::npos);
  // A number shows as text; a taken name gets 2 (ignoring case); cleared, it's ColumnN.
  CHECK(ed.SetCell(0, 0, 1, CellInput::Number(2024)));
  CHECK(Text(s, 0, 1) == "2024" && s.GetValue(0, 1).type == ValueType::String && s.Tables()[0].columns[1].name == "2024");
  CHECK(ed.SetCell(0, 0, 1, CellInput::Text("A")));
  CHECK(Text(s, 0, 1) == "A2" && s.Tables()[0].columns[1].name == "A2");
  CHECK(ed.ClearContents(0, Range{0, 1, 0, 1}));
  CHECK(Text(s, 0, 1) == "Column1" && s.Tables()[0].columns[1].name == "Column1");
  CHECK(ed.SetCell(0, 0, 1, CellInput::Text("x[1]#'y")));
  CHECK_EQ(FormulaText(s, 1, 4), std::string("SUM(T[x'[1']'#''y])"));
  CellInput f;
  f.formula = "1+1";
  CHECK(!ed.SetCell(0, 0, 1, f));  // Excel would keep its value as text
}

TEST(sheet_spill_blocked_by_typing) {
  auto book = TableBook(true);
  if (!book) return;
  Editor ed(*book);
  const Sheet& s = book->GetSheet(0);
  FormulaInfo info;
  CHECK(s.GetFormula(1, 7, info) && info.dynamic && info.ref == (Range{1, 7, 4, 7}));
  CHECK(ed.SetCell(0, 4, 7, CellInput::Number(9)));  // into H5, inside the spill
  CHECK(s.GetFormula(1, 7, info) && info.ref == (Range{1, 7, 1, 7}));
  CHECK(s.GetValue(1, 7).type == ValueType::Error && s.GetValue(1, 7).error == ErrorCode::Spill);
  CHECK(Text(s, 2, 7) == "" && Text(s, 3, 7) == "" && Text(s, 4, 7) == "9");
  CHECK(ed.Undo());
  CHECK(s.GetFormula(1, 7, info) && info.ref == (Range{1, 7, 4, 7}) && Text(s, 2, 7) == "3" && Text(s, 4, 7) == "5");
}

// Cut and paste on one sheet, as Excel rewrites references (measured: cutprobe.py, Range.Cut over
// the same cells): A1:A3 moved to C1:C3.
TEST(sheet_move_cells_references) {
  std::string data = "<sheetData>";
  data += Row(1, Num("A1", 1) + Num("B1", 10) + Num("C1", 100) + F("D1", "A2") + F("E1", "A1") +
                     "<c r=\"G1\"><f t=\"shared\" ref=\"G1:G4\" si=\"0\">A1*2</f></c>");
  data += Row(2, Num("A2", 2) + Num("B2", 20) + Num("C2", 200) + F("E2", "SUM(A1:A3)") + "<c r=\"G2\"><f t=\"shared\" si=\"0\"/></c>");
  data += Row(3, F("A3", "A1+B1") + Num("B3", 30) + F("E3", "SUM(A1:A5)") + "<c r=\"G3\"><f t=\"shared\" si=\"0\"/></c>");
  data += Row(4, F("E4", "A2*2") + "<c r=\"G4\"><f t=\"shared\" si=\"0\"/></c>");
  const char* more[][2] = {{"E5", "C1"},          {"E6", "SUM(C1:C3)"},  {"E7", "SUM(B1:D1)"}, {"E8", "$A$1"},
                           {"E9", "Sheet1!A1"},   {"E10", "SUM(A2:A3)"}, {"E11", "SUM(A3:A6)"}, {"E12", "SUM(C2:C5)"},
                           {"E13", "SUM(A:A)"},   {"E14", "SUM(1:1)"},   {"E15", "SUM(A1:B3)"}, {"E16", "SUM(A1,A2)"},
                           {"E18", "C3"},         {"E19", "SUM(A1:A2)"}, {"E20", "$A1"}};
  for (const auto& m : more) data += Row(std::stoi(std::string(m[0] + 1)), F(m[0], m[1]));
  data += "</sheetData>";
  auto book = OpenParts({data, "<sheetData>" + Row(1, F("A1", "Sheet1!A1")) + Row(2, F("A2", "SUM(Sheet1!A1:A3)")) +
                                   Row(3, F("A3", "Sheet1!C2")) + Row(4, F("A4", "SUM(Sheet1:Sheet2!A1)")) + "</sheetData>"});
  if (!book) return;
  Editor ed(*book);
  edit::Journal journal;
  ed.SetJournal(&journal);
  const Sheet& s1 = book->GetSheet(0);
  const Sheet& s2 = book->GetSheet(1);
  CHECK(ed.MoveCells(0, Range{0, 0, 2, 0}, CellAddr{0, 2}));
  CHECK(Text(s1, 0, 2) == "1" && Text(s1, 1, 2) == "2" && Text(s1, 0, 0) == "" && Text(s1, 1, 0) == "");
  CHECK_EQ(FormulaText(s1, 2, 2), std::string("C1+B1"));  // the moved formula
  CHECK_EQ(FormulaText(s1, 2, 0), std::string("-"));
  const char* want[][2] = {{"D1", "C2"},          {"E1", "C1"},           {"E2", "SUM(C1:C3)"},  {"E3", "SUM(A1:A5)"},
                           {"E4", "C2*2"},        {"E5", "#REF!"},        {"E6", "SUM(#REF!)"},  {"E7", "SUM(B1:D1)"},
                           {"E8", "$C$1"},        {"E9", "Sheet1!C1"},    {"E10", "SUM(C2:C3)"}, {"E11", "SUM(A3:A6)"},
                           {"E12", "SUM(C4:C5)"}, {"E13", "SUM(A:A)"},    {"E14", "SUM(1:1)"},   {"E15", "SUM(A1:B3)"},
                           {"E16", "SUM(C1,C2)"}, {"E18", "#REF!"},       {"E19", "SUM(C1:C2)"}, {"E20", "$C1"},
                           {"G1", "C1*2"},        {"G2", "C2*2"},         {"G3", "C3*2"},        {"G4", "A4*2"}};
  for (const auto& w : want) {
    CellAddr a;
    CHECK(ParseA1(w[0], a));
    if (FormulaText(s1, a.row, a.col) != w[1]) std::printf("  %s: %s\n", w[0], FormulaText(s1, a.row, a.col).c_str());
    CHECK_EQ(FormulaText(s1, a.row, a.col), std::string(w[1]));
  }
  CHECK_EQ(FormulaText(s2, 0, 0), std::string("Sheet1!C1"));
  CHECK_EQ(FormulaText(s2, 1, 0), std::string("SUM(Sheet1!C1:C3)"));
  CHECK_EQ(FormulaText(s2, 2, 0), std::string("Sheet1!#REF!"));
  CHECK_EQ(FormulaText(s2, 3, 0), std::string("SUM(Sheet1:Sheet2!A1)"));
  CHECK(book->Names()[0].name == "Total" && book->Names()[0].formula == "Sheet1!$C$1");

  // One step back.
  CHECK(ed.Undo());
  CHECK(Text(s1, 0, 0) == "1" && Text(s1, 0, 2) == "100" && FormulaText(s1, 2, 0) == "A1+B1");
  CHECK(FormulaText(s1, 0, 4) == "A1" && FormulaText(s1, 4, 4) == "C1" && FormulaText(s2, 2, 0) == "Sheet1!C2");
  CHECK(book->Names()[0].formula == "Sheet1!$A$1");

  // The journal makes it again.
  auto again = OpenParts({data, "<sheetData/>"});
  if (!again) return;
  Editor replayed(*again);
  CHECK(replayed.Replay(journal.All()).ok);
  CHECK(Text(again->GetSheet(0), 0, 0) == "1" && FormulaText(again->GetSheet(0), 0, 4) == "A1");  // undone there too
}

// Moved over part of themselves (A1:A3 to A2:A4), and what's refused.
TEST(sheet_move_cells_overlapping) {
  std::string data = "<sheetData>" + Row(1, Num("A1", 1) + F("E1", "A1")) + Row(2, Num("A2", 2) + F("E2", "SUM(A1:A3)")) +
                     Row(3, Num("A3", 3) + F("E3", "A4")) + Row(4, Num("A4", 4) + F("E4", "SUM(A2:A4)")) +
                     Row(5, F("E5", "A3")) + Row(6, F("E6", "SUM(A1:A4)")) + "</sheetData>" +
                     "<mergeCells count=\"1\"><mergeCell ref=\"B1:C1\"/></mergeCells>";
  auto book = OpenParts({data, "<sheetData/>"});
  if (!book) return;
  Editor ed(*book);
  const Sheet& s = book->GetSheet(0);
  CHECK(ed.MoveCells(0, Range{0, 0, 2, 0}, CellAddr{1, 0}));
  CHECK(Text(s, 0, 0) == "" && Text(s, 1, 0) == "1" && Text(s, 2, 0) == "2" && Text(s, 3, 0) == "3");
  const char* want[] = {"A2", "SUM(A2:A4)", "#REF!", "SUM(A3:A4)", "A4", "SUM(A2:A4)"};
  for (uint32_t r = 0; r < 6; ++r) CHECK_EQ(FormulaText(s, r, 4), std::string(want[r]));
  // A merge moves with its cells; one cut in part is refused.
  CHECK(ed.MoveCells(0, Range{0, 1, 0, 2}, CellAddr{9, 1}));
  CHECK(s.Merges().size() == 1 && s.Merges()[0] == (Range{9, 1, 9, 2}));
  CHECK(!ed.MoveCells(0, Range{9, 1, 9, 1}, CellAddr{12, 1}));
  CHECK(ed.MoveCells(0, Range{0, 0, 0, 0}, CellAddr{kMaxRows - 1, 0}));
  CHECK(!ed.MoveCells(0, Range{0, 0, 1, 0}, CellAddr{kMaxRows - 1, 0}));
}

// Cut on Sheet1 and pasted on Sheet2 (measured: cutprobe.py's third case, A1:A3 to Sheet2!C1).
TEST(sheet_move_cells_to_another_sheet) {
  const std::string one = "<sheetData>" + Row(1, Num("A1", 1) + Num("B1", 10) + F("E1", "A1")) +
                          Row(2, Num("A2", 2) + F("E2", "SUM(A1:A3)")) + Row(3, F("A3", "A1+B1")) + "</sheetData>";
  const std::string two = "<sheetData>" + Row(1, F("A1", "Sheet1!A1")) + Row(2, F("A2", "C1") + Num("C2", 5)) + "</sheetData>";
  auto book = OpenParts({one, two});
  if (!book) return;
  Editor ed(*book);
  edit::Journal journal;
  ed.SetJournal(&journal);
  const Sheet& s1 = book->GetSheet(0);
  const Sheet& s2 = book->GetSheet(1);
  CHECK(ed.MoveCells(0, Range{0, 0, 2, 0}, 1, CellAddr{0, 2}));
  CHECK(Text(s1, 0, 0) == "" && Text(s1, 1, 0) == "" && FormulaText(s1, 2, 0) == "-");
  CHECK(Text(s2, 0, 2) == "1" && Text(s2, 1, 2) == "2");
  CHECK_EQ(FormulaText(s2, 2, 2), std::string("C1+Sheet1!B1"));
  CHECK_EQ(FormulaText(s1, 0, 4), std::string("Sheet2!C1"));
  CHECK_EQ(FormulaText(s1, 1, 4), std::string("SUM(Sheet2!C1:C3)"));
  CHECK_EQ(FormulaText(s2, 0, 0), std::string("Sheet2!C1"));
  CHECK_EQ(FormulaText(s2, 1, 0), std::string("#REF!"));
  CHECK(book->Names()[0].formula == "Sheet2!$C$1");  // Total: Sheet1!$A$1
  CHECK(ed.Undo());
  CHECK(Text(s1, 0, 0) == "1" && Text(s2, 1, 2) == "5" && FormulaText(s1, 0, 4) == "A1" && FormulaText(s2, 1, 0) == "C1");

  auto again = OpenParts({one, two});
  if (!again) return;
  Editor replayed(*again);
  const std::string records(journal.All());
  CHECK(replayed.Replay(records).ok);
  CHECK(Text(again->GetSheet(0), 0, 0) == "1");  // undone there too
  CHECK(ed.Redo());
  CHECK_EQ(FormulaText(s2, 2, 2), std::string("C1+Sheet1!B1"));
}

// A filter's columns follow columns inserted and deleted, in the model as in the file (s2-corpus:
// filter-*.txt).
TEST(sheet_shift_filter_columns) {
  auto book = OpenParts({"<sheetData/>", "<sheetData/>"});
  if (!book) return;
  Editor ed(*book);
  SheetFilter f;
  f.ref = Range{1, 1, 4, 4};  // B2:E5
  FilterColumn c;
  c.colId = 1;  // C
  c.values = {"x"};
  FilterColumn other;
  other.colId = 3;  // E
  other.kind = FilterColumn::Kind::Other;
  other.xml = "<filterColumn colId=\"3\"><colorFilter dxfId=\"0\"/></filterColumn>";
  f.columns = {c, other};
  CHECK(ed.SetFilter(0, f));
  const Sheet& s = book->GetSheet(0);
  CHECK(ed.InsertColumns(0, 2, 1));  // before C
  CHECK(s.Filter()->ref == (Range{1, 1, 4, 5}) && s.Filter()->columns.size() == 2);
  CHECK(s.Filter()->columns[0].colId == 2 && s.Filter()->columns[1].colId == 4);
  CHECK(s.Filter()->columns[1].xml == "<filterColumn colId=\"4\"><colorFilter dxfId=\"0\"/></filterColumn>");
  CHECK(ed.DeleteColumns(0, 3, 1));  // D, the filtered one
  CHECK(s.Filter()->ref == (Range{1, 1, 4, 4}) && s.Filter()->columns.size() == 1 && s.Filter()->columns[0].colId == 3);
  CHECK(s.Filter()->filtered);
  CHECK(ed.Undo(2));
  CHECK(s.Filter()->ref == f.ref && s.Filter()->columns.size() == 2 && s.Filter()->columns[0].colId == 1);
}
