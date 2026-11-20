// The edit layer (S2, src/sheet/edit): operations, groups, undo and redo on the workbook model.
#include "TestHarness.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "SheetTestUtil.h"
#include "sheet/edit/Edit.h"
#include "sheet/model/Workbook.h"
#include "sheet/xlsx/Xlsx.h"

using namespace fw::sheet;
using edit::CellInput;
using edit::Editor;

using namespace sheet_test;

TEST(sheet_edit_cells_undo_redo) {
  // Row 1 with two cells, then rows 41-80 (a second block of rows).
  std::string data = "<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c><c r=\"B1\"><v>2</v></c></row>";
  for (int r = 41; r <= 80; ++r) data += "<row r=\"" + std::to_string(r) + "\"><c r=\"A" + std::to_string(r) + "\"><v>1</v></c></row>";
  auto book = OpenParts({data + "</sheetData>"});
  if (!book) return;
  Sheet& s = book->GetSheet(0);
  Editor ed(*book);
  std::vector<edit::Change> heard;
  ed.OnChange([&](const std::vector<edit::Change>& c) { heard = c; });
  CHECK(s.Blocks().size() >= 2);
  auto untouched = s.Blocks().back();  // blocks an edit doesn't touch aren't copied
  CHECK(ed.SetCell(0, 0, 0, CellInput::Number(5)));
  CHECK(ed.SetCell(0, 2, 3, CellInput::Text("hello")));
  CHECK(ed.SetCell(0, 0, 2, CellInput::Formula("A1+B1")));
  CHECK_EQ(Text(s, 0, 0), std::string("5"));
  CHECK_EQ(Text(s, 2, 3), std::string("hello"));
  CHECK_EQ(FormulaText(s, 0, 2), std::string("A1+B1"));
  CHECK(heard.size() == 1 && heard[0].sheet == 0 && (heard[0].what & edit::kFormulas));
  CHECK(s.CellCount() == 44 && s.FormulaCount() == 1);
  CHECK(s.Blocks().back() == untouched);
  CHECK(ed.Modified() && (ed.DirtyPieces(0) & edit::kPieceCells));
  // Text typed again is the same shared string.
  CHECK(ed.SetCell(0, 3, 3, CellInput::Text("hello")));
  CellInfo a, b;
  CHECK(s.GetCell(2, 3, a) && s.GetCell(3, 3, b) && a.sharedString == b.sharedString && a.sharedString != kNone);

  CHECK(ed.Undo(2));
  CHECK_EQ(FormulaText(s, 0, 2), std::string("-"));
  CHECK_EQ(Text(s, 0, 2), std::string(""));
  CHECK(s.CellCount() == 43 && s.FormulaCount() == 0);
  CHECK(ed.Undo(2));
  CHECK_EQ(Text(s, 0, 0), std::string("1"));
  CHECK_EQ(Text(s, 2, 3), std::string(""));
  CHECK(s.CellCount() == 42 && !ed.CanUndo() && !ed.Modified() && ed.DirtyPieces(0) == 0);
  CHECK(ed.RedoNames().size() == 4 && ed.RedoName() == u"Typing");
  CHECK(ed.Redo(3));
  CHECK_EQ(Text(s, 0, 0), std::string("5"));
  CHECK_EQ(FormulaText(s, 0, 2), std::string("A1+B1"));
  // A new edit drops what could be redone.
  CHECK(ed.ClearContents(0, Range{0, 0, 0, 2}));
  CHECK(!ed.CanRedo() && ed.UndoName() == u"Clear");
  CHECK_EQ(Text(s, 0, 1), std::string(""));
  CHECK_EQ(FormulaText(s, 0, 2), std::string("-"));
  CHECK(ed.Undo());
  CHECK_EQ(Text(s, 0, 1), std::string("2"));
  CHECK_EQ(FormulaText(s, 0, 2), std::string("A1+B1"));
}

TEST(sheet_edit_groups) {
  auto book = OpenParts({"<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row></sheetData>"});
  if (!book) return;
  Sheet& s = book->GetSheet(0);
  Editor ed(*book);
  int notified = 0;
  ed.OnChange([&](const std::vector<edit::Change>&) { ++notified; });
  ed.BeginGroup(u"Paste");
  CHECK(ed.SetCell(0, 0, 0, CellInput::Number(7)));
  ed.BeginGroup(u"inner");
  CHECK(ed.SetCell(0, 1, 0, CellInput::Number(8)));
  CHECK(ed.SetCell(0, 0, 0, CellInput::Number(9)));
  ed.EndGroup();
  ed.EndGroup();
  CHECK(notified == 1 && ed.UndoNames().size() == 1 && ed.UndoName() == u"Paste");
  CHECK_EQ(Text(s, 0, 0), std::string("9"));
  CHECK(ed.Undo());
  CHECK_EQ(Text(s, 0, 0), std::string("1"));
  CHECK_EQ(Text(s, 1, 0), std::string(""));
  // A cancelled group leaves nothing.
  {
    edit::EditGroup g(ed, u"Fill");
    CHECK(ed.SetCell(0, 5, 5, CellInput::Number(3)));
  }
  CHECK_EQ(Text(s, 5, 5), std::string(""));
  CHECK(ed.CanRedo() && !ed.CanUndo());
  // An edit that fails changes nothing.
  CHECK(!ed.SetCell(0, 0, 0, CellInput::Text(std::string("bad\x01"))));
  CHECK_EQ(Text(s, 0, 0), std::string("1"));
}

TEST(sheet_edit_shared_and_array_formulas) {
  auto book = OpenParts({"<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c><c r=\"B1\"><f t=\"shared\" ref=\"B1:B3\" si=\"0\">A1*2</f><v>2</v></c>"
                         "<c r=\"D1\"><f t=\"array\" ref=\"D1:D2\">A1:A2*3</f><v>3</v></c></row>"
                         "<row r=\"2\"><c r=\"A2\"><v>2</v></c><c r=\"B2\"><f t=\"shared\" si=\"0\"/><v>4</v></c><c r=\"D2\"><v>6</v></c></row>"
                         "<row r=\"3\"><c r=\"A3\"><v>3</v></c><c r=\"B3\"><f t=\"shared\" si=\"0\"/><v>6</v></c></row></sheetData>"});
  if (!book) return;
  Sheet& s = book->GetSheet(0);
  Editor ed(*book);
  // Changing the master: the cells sharing its formula keep it, written for themselves.
  CHECK(ed.SetCell(0, 0, 1, CellInput::Number(10)));
  FormulaInfo f;
  CHECK(s.GetFormula(1, 1, f) && f.text == "A2*2" && f.kind == FormulaKind::Normal);
  CHECK(s.GetFormula(2, 1, f) && f.text == "A3*2");
  CHECK(ed.Undo());
  CHECK(s.GetFormula(1, 1, f) && f.kind == FormulaKind::Shared && f.text == "A1*2" && f.origin == (CellAddr{0, 1}));
  // Part of an array can't change; all of it can.
  CHECK(!ed.SetCell(0, 1, 3, CellInput::Number(1)));
  CHECK(!ed.ClearContents(0, Range{1, 3, 1, 3}));
  CHECK(!ed.Merge(0, Range{0, 2, 1, 3}));
  CHECK(ed.ClearContents(0, Range{0, 3, 1, 3}));
  CHECK_EQ(FormulaText(s, 0, 3), std::string("-"));
  CHECK(ed.Undo());
  CellInput arr = CellInput::Formula("A1:A3*4");
  arr.array = true;
  arr.arrayRange = Range{0, 4, 2, 4};
  CHECK(ed.SetCell(0, 0, 4, arr));
  CHECK(s.GetFormula(0, 4, f) && f.kind == FormulaKind::Array && f.ref == (Range{0, 4, 2, 4}));
  CHECK(!ed.SetCell(0, 2, 4, CellInput::Number(1)));
}

TEST(sheet_edit_rows_columns_merges) {
  auto book = OpenParts({"<cols><col min=\"1\" max=\"4\" width=\"10\" customWidth=\"1\"/></cols><sheetData>"
                         "<row r=\"1\" spans=\"1:2\"><c r=\"A1\"><v>1</v></c><c r=\"B1\"><v>2</v></c></row>"
                         "<row r=\"2\"><c r=\"A2\"><v>3</v></c></row></sheetData>"});
  if (!book) return;
  Sheet& s = book->GetSheet(0);
  Editor ed(*book);
  CHECK(ed.SetColumnWidth(0, 1, 2, 20));
  CHECK(s.Columns().size() == 3 && s.Column(0).width == 10 && s.Column(1).width == 20 && s.Column(2).width == 20 &&
        s.Column(3).width == 10);
  CHECK(ed.SetColumnWidth(0, 1, 2, 10));
  CHECK(s.Columns().size() == 1);  // joined again
  CHECK(ed.Undo(2));
  CHECK(s.Columns().size() == 1 && s.Column(3).width == 10);
  CHECK(ed.HideColumns(0, 5, 5, true) && s.Column(5).hidden && s.Column(5).width == 0);

  CHECK(ed.SetRowHeight(0, 0, 2, 30));
  CHECK(s.Row(0).height == 30 && s.Row(0).customHeight && s.Row(2).height == 30 && s.Row(2).stored);
  CHECK(ed.HideRows(0, 1, 1, true) && s.Row(1).hidden);
  CHECK(ed.Undo(2));
  CHECK(s.Row(0).height == 0 && !s.Row(1).hidden && !s.Row(2).stored);
  CHECK(ed.SetRowOutline(0, 0, 1, 1) && s.Row(1).outlineLevel == 1);
  // The writer writes what the rows say: their attributes follow the typed fields.
  CHECK(ed.SetRowHeight(0, 0, 0, 30) && ed.SetCell(0, 0, 2, CellInput::Text("x")));
  std::string xml;
  CHECK(xlsx::WritePart(*book, s.PartName(), xml).Ok());
  size_t row1 = xml.find("<row r=\"1\" spans=\"1:2\"");
  std::string tag = row1 == std::string::npos ? std::string() : xml.substr(row1, xml.find('>', row1) - row1);
  if (tag.find(" ht=\"30\"") == std::string::npos) std::printf("    %s\n", tag.c_str());
  CHECK(tag.find(" ht=\"30\"") != std::string::npos && tag.find(" customHeight=\"1\"") != std::string::npos &&
        tag.find(" outlineLevel=\"1\"") != std::string::npos);
  CHECK(xml.find("<c r=\"C1\" t=\"s\"><v>0</v></c>") != std::string::npos);
  CHECK(ed.Undo(2));

  CHECK(ed.Merge(0, Range{0, 0, 1, 1}));
  CHECK(s.Merges().size() == 1 && s.MergeAt(1, 1) != nullptr);
  CHECK_EQ(Text(s, 0, 0), std::string("1"));
  CHECK_EQ(Text(s, 0, 1), std::string(""));  // the others' contents go
  CHECK_EQ(Text(s, 1, 0), std::string(""));
  CHECK(ed.Unmerge(0, Range{1, 1, 1, 1}) && s.Merges().empty());
  CHECK(ed.Undo(2));
  CHECK(s.Merges().empty());
  CHECK_EQ(Text(s, 1, 0), std::string("3"));
  CHECK(ed.DirtyPieces(0) & edit::kPieceCols);
}

TEST(sheet_edit_styles) {
  auto book = OpenParts({"<sheetData><row r=\"1\"><c r=\"A1\" s=\"1\"><v>45000</v></c><c r=\"B1\"><v>2</v></c></row></sheetData>"});
  if (!book) return;
  Sheet& s = book->GetSheet(0);
  Editor ed(*book);
  Font bold = book->GetStyles().Fonts()[0];
  bold.bold = true;
  uint32_t font = ed.AddFont(bold);
  CHECK(font == 1 && ed.AddFont(bold) == 1 && ed.StylesDirty());
  // Bold on: each cell keeps its other formatting.
  auto boldOf = [&](uint32_t xf) {
    Xf x = book->GetStyles().CellXf(xf);
    x.font = font;
    x.applyFont = true;
    return ed.AddXf(x);
  };
  CHECK(ed.MapStyles(0, Range{0, 0, 1, 1}, boldOf));
  CHECK(book->GetStyles().CellXf(s.StyleAt(0, 0)).numFmt == 14 && book->GetStyles().CellXf(s.StyleAt(0, 0)).font == 1);
  CHECK(book->GetStyles().CellXf(s.StyleAt(1, 1)).font == 1);  // an empty cell got the format
  CHECK(ed.Undo());
  CHECK(s.StyleAt(0, 0) == 1 && s.StyleAt(1, 1) == 0 && s.CellCount() == 2);
  // Whole columns set the column's format rather than every cell's.
  CHECK(ed.SetStyle(0, Range{0, 2, kMaxRows - 1, 2}, 1));
  CHECK(s.Column(2).style == 1 && s.CellCount() == 2);
  CHECK(ed.ClearFormats(0, Range{0, 0, 0, 0}) && s.StyleAt(0, 0) == 0);
}

TEST(sheet_edit_sheets_and_names) {
  auto book = OpenParts({"<sheetData><row r=\"1\"><c r=\"A1\"><f>Sheet2!B2+1</f><v>1</v></c></row></sheetData>",
                         "<sheetData><row r=\"2\"><c r=\"B2\"><v>5</v></c></row></sheetData>"});
  if (!book) return;
  Editor ed(*book);
  CHECK(ed.CheckSheetName("a/b") != u"" && ed.CheckSheetName("sheet2") != u"" && ed.CheckSheetName("Fine") == u"");
  CHECK(ed.RenameSheet(1, "Data"));
  CHECK_EQ(FormulaText(book->GetSheet(0), 0, 0), std::string("Data!B2+1"));
  CHECK(book->FindName("Local", 1) && book->FindName("Local", 1)->formula == "Data!$B$2");
  CHECK(ed.Undo());
  CHECK(book->GetSheet(1).Name() == "Sheet2");
  CHECK_EQ(FormulaText(book->GetSheet(0), 0, 0), std::string("Sheet2!B2+1"));

  CHECK(ed.AddSheet(0));
  CHECK(book->SheetCount() == 3 && book->GetSheet(0).Name() == "Sheet3");
  CHECK(book->FindName("Local", 2) != nullptr);  // local names follow their sheet
  CHECK(ed.MoveSheet(0, 2));
  CHECK(book->GetSheet(2).Name() == "Sheet3" && book->FindName("Local", 1) != nullptr);
  CHECK(ed.DeleteSheet(2));
  CHECK(book->SheetCount() == 2);
  CHECK(ed.Undo(3));
  CHECK(book->SheetCount() == 2 && book->GetSheet(0).Name() == "Sheet1" && book->FindName("Local", 1) != nullptr);
  CHECK(ed.Redo(3));
  CHECK(book->SheetCount() == 2 && book->GetSheet(1).Name() == "Sheet2");

  CHECK(ed.SetTabColor(0, Color{Color::Kind::Rgb, 0xFFFF0000}) && book->GetSheet(0).Props().tabColor.value == 0xFFFF0000);
  CHECK(ed.SetSheetState(1, SheetState::Hidden) && !ed.SetSheetState(0, SheetState::Hidden));
  CHECK(book->ActiveSheet() == 0 && ed.Undo() && book->GetSheet(1).State() == SheetState::Visible);
  CHECK(ed.FreezePanes(0, 1, 2));
  CHECK(book->GetSheet(0).Views()[0].paneState == PaneState::Frozen && book->GetSheet(0).Views()[0].ySplit == 1);
  CHECK(ed.SetName(DefinedName{"Rate", -1, "Sheet1!$C$1", false}) && book->FindName("rate"));
  CHECK(!ed.SetName(DefinedName{"A1", -1, "1", false}));
  CHECK(ed.DeleteName("Rate") && !book->FindName("Rate"));
  CHECK(ed.Undo() && book->FindName("Rate"));
  CHECK(ed.WorkbookDirty());
}

// A reference that lost its cells ('Mental Health'!#REF!, its rows deleted) whose sheet is deleted
// too is #REF! once, as Excel writes it: #REF!#REF! isn't a formula, and Excel removes it as a
// damaged record (s4 final gate). The same in a name, a 3-D range and a conditional format; a
// rename keeps the sheet's name in front of it.
TEST(sheet_edit_lost_reference_on_a_deleted_sheet) {
  auto book = OpenParts({"<sheetData><row r=\"24\"><c r=\"D24\"><v>5</v></c></row></sheetData>",
                         "<sheetData><row r=\"1\"><c r=\"A1\"><f>Sheet1!D24</f><v>5</v></c></row></sheetData>",
                         "<sheetData><row r=\"1\"><c r=\"A1\"><f>SUM(Sheet1:Sheet2!#REF!)+1</f><v>0</v></c></row></sheetData>"
                         "<conditionalFormatting sqref=\"A1\"><cfRule type=\"expression\" priority=\"1\">"
                         "<formula>Sheet1!D24&gt;0</formula></cfRule></conditionalFormatting>"});
  if (!book) return;
  Editor ed(*book);
  CHECK(ed.RenameSheet(0, "Mental Health"));
  CHECK(ed.SetName(DefinedName{"Fee", -1, "'Mental Health'!$D$24", false}));
  CHECK(ed.DeleteRows(0, 22, 2));  // rows 23:24
  CHECK_EQ(FormulaText(book->GetSheet(1), 0, 0), std::string("'Mental Health'!#REF!"));
  CHECK(book->FindName("Fee") && book->FindName("Fee")->formula == "'Mental Health'!#REF!");
  CHECK(book->GetSheet(2).Text().afterData.find("<formula>'Mental Health'!#REF!&gt;0</formula>") != std::string::npos);
  CHECK(ed.RenameSheet(0, "MH"));
  CHECK_EQ(FormulaText(book->GetSheet(1), 0, 0), std::string("MH!#REF!"));
  CHECK_EQ(FormulaText(book->GetSheet(2), 0, 0), std::string("SUM(MH:Sheet2!#REF!)+1"));
  // Its sheet deleted: #REF! once; the 3-D range gives way to the sheet left in it.
  CHECK(ed.DeleteSheet(0));
  CHECK_EQ(FormulaText(book->GetSheet(0), 0, 0), std::string("#REF!"));
  CHECK(book->FindName("Fee") && book->FindName("Fee")->formula == "#REF!");
  CHECK_EQ(FormulaText(book->GetSheet(1), 0, 0), std::string("SUM(Sheet2!#REF!)+1"));
  CHECK(book->GetSheet(1).Text().afterData.find("<formula>#REF!&gt;0</formula>") != std::string::npos);
  // The 3-D range's last sheet deleted too: #REF! once.
  CHECK(ed.DeleteSheet(0));
  CHECK_EQ(FormulaText(book->GetSheet(0), 0, 0), std::string("SUM(#REF!)+1"));
  CHECK(book->GetSheet(0).Text().afterData.find("#REF!#REF!") == std::string::npos);
  CHECK(ed.Undo(2));
  CHECK_EQ(FormulaText(book->GetSheet(1), 0, 0), std::string("MH!#REF!"));
}

// A 3-D range (Sheet1:Sheet3!A1) as Excel changes it (checked in Excel 2026-11-20), in a cell, a name,
// a conditional format and a validation alike (Excel refuses 3-D ranges in the last two, but a
// file can have them):
// - deleting an end sheet moves that end to the next sheet inward; one sheet left, it's a plain
//   reference; every sheet deleted, #REF!;
// - renaming an end renames it;
// - moving a sheet changes what the range spans; an end moved past the other end gives way to the
//   next sheet inward.
TEST(sheet_edit_3d_ranges_follow_their_sheets) {
  struct Step {
    char op;  // 'd' delete, 'r' rename, 'm' move
    const char* sheet;
    const char* name = "";
    int to = 0;
  };
  auto run = [](const char* range, std::vector<Step> steps, const std::string& want) {
    const std::string sum = std::string("SUM(") + range + "!A1)";
    const std::string five = "<sheetData><row r=\"1\"><c r=\"C1\"><f>" + sum + "</f><v>0</v></c></row></sheetData>"
                             "<conditionalFormatting sqref=\"E1\"><cfRule type=\"expression\" priority=\"1\"><formula>" + sum +
                             "&gt;0</formula></cfRule></conditionalFormatting><dataValidations count=\"1\">"
                             "<dataValidation type=\"custom\" sqref=\"F1\"><formula1>" + sum + "&gt;0</formula1></dataValidation></dataValidations>";
    const std::string data = "<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row></sheetData>";
    auto book = OpenParts({data, data, data, data, five});
    if (!book) return;
    Editor ed(*book);
    CHECK(ed.SetName(DefinedName{"N", -1, std::string(range) + "!$A$1", false}));
    auto index = [&](const char* name) {
      for (size_t i = 0; i < book->SheetCount(); ++i)
        if (book->GetSheet(i).Name() == name) return static_cast<int>(i);
      return -1;
    };
    for (const Step& s : steps) {
      const int i = index(s.sheet);
      CHECK(i >= 0);
      if (s.op == 'd') CHECK(ed.DeleteSheet(i));
      else if (s.op == 'r') CHECK(ed.RenameSheet(i, s.name));
      else CHECK(ed.MoveSheet(i, s.to));
    }
    const Sheet& five2 = book->GetSheet(static_cast<size_t>(index("Sheet5")));
    const std::string ref = want == "#REF!" ? want : want + "!A1", abs = want == "#REF!" ? want : want + "!$A$1";
    const std::string cell = FormulaText(five2, 0, 2), name = book->FindName("N") ? book->FindName("N")->formula : "-";
    const std::string& rules = five2.Text().afterData;
    const bool cf = rules.find("<formula>SUM(" + ref + ")&gt;0</formula>") != std::string::npos;
    const bool dv = rules.find("<formula1>SUM(" + ref + ")&gt;0</formula1>") != std::string::npos;
    if (cell != "SUM(" + ref + ")" || name != abs || !cf || !dv)
      std::printf("    %s, %zu steps: %s | %s | %s\n", range, steps.size(), cell.c_str(), name.c_str(), rules.c_str());
    CHECK_EQ(cell, "SUM(" + ref + ")");
    CHECK_EQ(name, abs);
    CHECK(cf && dv);
    // Undone, as it was.
    CHECK(ed.Undo(static_cast<int>(steps.size())));
    CHECK_EQ(FormulaText(book->GetSheet(4), 0, 2), sum);
    CHECK(book->FindName("N") && book->FindName("N")->formula == std::string(range) + "!$A$1");
  };
  const char* r = "Sheet1:Sheet3";
  // Deleting.
  run(r, {{'d', "Sheet1"}}, "Sheet2:Sheet3");
  run(r, {{'d', "Sheet3"}}, "Sheet1:Sheet2");
  run(r, {{'d', "Sheet2"}}, "Sheet1:Sheet3");
  run(r, {{'d', "Sheet1"}, {'d', "Sheet2"}}, "Sheet3");
  run(r, {{'d', "Sheet3"}, {'d', "Sheet2"}}, "Sheet1");
  run(r, {{'d', "Sheet1"}, {'d', "Sheet3"}}, "Sheet2");
  run(r, {{'d', "Sheet1"}, {'d', "Sheet2"}, {'d', "Sheet3"}}, "#REF!");
  // Renaming.
  run(r, {{'r', "Sheet1", "Jan"}}, "Jan:Sheet3");
  run(r, {{'r', "Sheet3", "Mar 2"}}, "'Sheet1:Mar 2'");
  run(r, {{'r', "Sheet1", "Jan"}, {'r', "Sheet3", "Mar"}}, "Jan:Mar");
  // Moving: what the range spans changes, its text only when an end passes the other.
  run(r, {{'m', "Sheet4", "", 1}}, "Sheet1:Sheet3");  // into it
  run(r, {{'m', "Sheet2", "", 4}}, "Sheet1:Sheet3");  // out of it
  run(r, {{'m', "Sheet1", "", 1}}, "Sheet1:Sheet3");  // an end inward
  run(r, {{'m', "Sheet3", "", 3}}, "Sheet1:Sheet3");  // an end outward
  run(r, {{'m', "Sheet4", "", 0}}, "Sheet1:Sheet3");  // before the first
  run(r, {{'m', "Sheet1", "", 2}}, "Sheet2:Sheet3");  // the first past the last
  run(r, {{'m', "Sheet1", "", 4}}, "Sheet2:Sheet3");
  run(r, {{'m', "Sheet3", "", 0}}, "Sheet1:Sheet2");  // the last before the first
  run(r, {{'m', "Sheet1", "", 2}, {'d', "Sheet2"}}, "Sheet3");
  run("Sheet1:Sheet2", {{'m', "Sheet1", "", 1}}, "Sheet2");
  run("Sheet1:Sheet2", {{'m', "Sheet2", "", 0}}, "Sheet1");
}

// A name, as a cell's formula, can't name a sheet the workbook doesn't have: Excel would look for a
// workbook by that name, and the file would name a sheet it hasn't (fuzz_xlsx, CI 44693360543:
// Sheet1:Sheet2!A1 with no Sheet1).
TEST(sheet_edit_names_name_sheets_the_workbook_has) {
  auto book = OpenParts({"<sheetData/>", "<sheetData/>"});
  if (!book) return;
  Editor ed(*book);
  CHECK(ed.RenameSheet(0, "Sheet 1"));
  for (const char* f : {"Sheet1:Sheet2!A1", "Sheet2:Sheet1!A1", "Sheet9!A1*2", "SUM(Sheet2!A1,Sheet1!B2)"}) {
    CHECK(!ed.SetName(DefinedName{"N", -1, f, false}));
    CHECK(!ed.SetName(DefinedName{"_xlnm.Print_Area", 1, f, false}));
  }
  CHECK(!book->FindName("N") && !book->FindName("_xlnm.Print_Area", 1));
  for (const char* f : {"'Sheet 1:Sheet2'!A1", "sheet2!A1*2", "5", "\"Sheet1!A1\"", "[1]Sheet1!A1", "#REF!"}) {
    CHECK(ed.SetName(DefinedName{"N", -1, f, false}));
    CHECK(book->FindName("N") && book->FindName("N")->formula == f);
  }
}

// So can't a conditional format's or a validation's formula (Excel takes no other workbook in a
// rule); a rule the file had that names one doesn't stop the sheet's other rules from changing.
TEST(sheet_edit_rules_name_sheets_the_workbook_has) {
  const std::string stale = "<conditionalFormatting sqref=\"C1\"><cfRule type=\"expression\" priority=\"1\">"
                            "<formula>Gone!A1&gt;0</formula></cfRule></conditionalFormatting>";
  auto book = OpenParts({"<sheetData/>" + stale, "<sheetData/>"});
  if (!book) return;
  Editor ed(*book);
  CHECK(ed.RenameSheet(0, "Sheet 1"));
  using RK = Editor::RuleKind;
  auto cf = [](const char* f, int priority) {
    return "<conditionalFormatting sqref=\"A1\"><cfRule type=\"expression\" priority=\"" + std::to_string(priority) +
           "\"><formula>" + std::string(f) + "</formula></cfRule></conditionalFormatting>";
  };
  auto dv = [](const char* f) {
    return "<dataValidations count=\"1\"><dataValidation type=\"list\" sqref=\"B1\"><formula1>" + std::string(f) +
           "</formula1></dataValidation></dataValidations>";
  };
  for (const char* f : {"Sheet1:Sheet2!A1&gt;0", "Sheet9!A1&gt;0"})
    CHECK(!ed.SetSheetRules(0, RK::ConditionalFormats, stale + cf(f, 2), "", u"New Rule"));
  CHECK(!ed.SetSheetRules(0, RK::Validations, dv("Sheet1!$A$1:$A$3"), "", u"Data Validation"));
  CHECK(!ed.SetSheetRules(1, RK::Validations, dv("Sheet1:Sheet2!$A$1"), "", u"Data Validation"));
  CHECK(book->GetSheet(0).Text().afterData == stale);
  CHECK(ed.SetSheetRules(0, RK::ConditionalFormats, stale + cf("'Sheet 1:Sheet2'!A1&gt;0", 2), "", u"New Rule"));
  CHECK(ed.SetSheetRules(0, RK::Validations, dv("Sheet2!$A$1:$A$3"), "", u"Data Validation"));
  CHECK(book->GetSheet(0).Text().afterData.find("<formula1>Sheet2!$A$1:$A$3</formula1>") != std::string::npos);
}

// A formula that doesn't compile (a file can hold one) keeps its text, but its sheet prefixes
// follow its sheets as a compiled one's do: it never names a sheet that's gone (fuzz_xlsx).
TEST(sheet_edit_sheet_prefixes_of_formulas_that_dont_compile) {
  const std::string data = "<sheetData/>";
  auto book = OpenParts({data, data, data, data});
  if (!book) return;
  Editor ed(*book);
  CHECK(ed.SetName(DefinedName{"Span", -1, "Sheet1:Sheet3!A1+", false}));
  CHECK(ed.SetName(DefinedName{"One", -1, "Sheet1!A1+Sheet4!A1+", false}));
  CHECK(ed.SetName(DefinedName{"Moved", -1, "SUM('Sheet2:Sheet4'!A1", false}));
  auto name = [&](const char* n) { return book->FindName(n) ? book->FindName(n)->formula : std::string("-"); };
  CHECK(ed.DeleteSheet(0));  // Sheet1
  CHECK_EQ(name("Span"), std::string("Sheet2:Sheet3!A1+"));
  CHECK_EQ(name("One"), std::string("#REF!+Sheet4!A1+"));
  CHECK_EQ(name("Moved"), std::string("SUM('Sheet2:Sheet4'!A1"));
  CHECK(ed.MoveSheet(0, 2));  // Sheet2, a first end, past both last ends: each gives way to Sheet3
  CHECK_EQ(name("Span"), std::string("Sheet3!A1+"));
  CHECK_EQ(name("Moved"), std::string("SUM(Sheet3:Sheet4!A1"));
  CHECK(ed.Undo(2));
  CHECK_EQ(name("Span"), std::string("Sheet1:Sheet3!A1+"));
  CHECK_EQ(name("One"), std::string("Sheet1!A1+Sheet4!A1+"));
}
