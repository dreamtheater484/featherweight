// Rows, columns and cells inserted and deleted through the reference registry (S2 step 4):
// formulas, names, merges, links, rules and the rest follow, as Excel does to the same file
// (measured with scratchpad probes; Refs.h has the facts), and undo puts everything back.
#include <string>

#include "SheetTestUtil.h"
#include "TestHarness.h"
#include "sheet/edit/Edit.h"
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
