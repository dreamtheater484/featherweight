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
  CHECK(!ed.DeleteSheet(1));  // Sheet1's formula uses it
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
