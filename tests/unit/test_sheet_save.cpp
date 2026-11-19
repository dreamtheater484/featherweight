// Saving an edited workbook (S2, src/sheet/xlsx/Save.cpp): edit, save, read the saved file back and
// check it.
#include "TestHarness.h"

#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "SheetTestUtil.h"
#include "pkg/Check.h"
#include "core/Strings.h"
#include "sheet/csv/Csv.h"
#include "sheet/numfmt/NumLocale.h"
#include "sheet/edit/Edit.h"
#include "sheet/edit/Journal.h"
#include "sheet/model/Workbook.h"
#include "sheet/xlsx/Xlsx.h"

using namespace fw::sheet;
using edit::CellInput;
using edit::Editor;
using namespace sheet_test;

namespace {

using Parts = std::vector<std::pair<std::string, std::string>>;

std::string& PartOf(Parts& parts, const std::string& name) {
  for (auto& [n, xml] : parts)
    if (n == name) return xml;
  parts.push_back({name, {}});
  return parts.back().second;
}

// A relationship from `source` ("xl/workbook.xml").
void AddRel(Parts& parts, const std::string& source, const std::string& relId, const std::string& relType,
            const std::string& target) {
  size_t slash = source.rfind('/');
  std::string relsName = source.substr(0, slash + 1) + "_rels/" + source.substr(slash + 1) + ".rels";
  std::string& rels = PartOf(parts, relsName);
  if (rels.empty()) rels = "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"></Relationships>";
  rels.insert(rels.rfind("</Relationships>"),
              "<Relationship Id=\"" + relId + "\" Type=\"" + relType + "\" Target=\"" + target + "\"/>");
}

// A part with a content type (none: the default for .xml), and a relationship to it.
void AddPart(Parts& parts, const std::string& name, const std::string& xml, const std::string& contentType,
             const std::string& source, const std::string& relId, const std::string& relType,
             const std::string& target) {
  parts.push_back({name, xml});
  if (!contentType.empty()) {
    std::string& types = PartOf(parts, "[Content_Types].xml");
    types.insert(types.rfind("</Types>"), "<Override PartName=\"/" + name + "\" ContentType=\"" + contentType + "\"/>");
  }
  AddRel(parts, source, relId, relType, target);
}

// With FW_SAVE_OUT set to a folder, the saved files go there as `name`.xlsx too, for opening in
// Excel (scripts\excel-open-check.ps1).
void Keep(const std::string& name, const std::vector<uint8_t>& bytes) {
  wchar_t dir[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"FW_SAVE_OUT", dir, MAX_PATH);
  if (n == 0 || n >= MAX_PATH || name.empty()) return;
  CHECK(test::WriteFileBytes(std::wstring(dir) + L"\\" + std::wstring(name.begin(), name.end()) + L".xlsx", bytes));
}

// A save as the app makes it (Xlsx.h): BeginSave, PrepareSave, WriteSave, then CommitSave as if
// the bytes reached the file.
std::vector<uint8_t> SaveBytes(Workbook& book, Editor& ed, const std::string& name = {},
                               std::optional<FileFormat> format = std::nullopt) {
  ed.BeginSave();
  xlsx::SavePlan plan;
  fw::pkg::Status st = xlsx::PrepareSave(book, ed, plan, format.value_or(book.Format()));
  if (!st.Ok()) std::printf("  PrepareSave: %s\n", st.message.c_str());
  CHECK(st.Ok());
  fw::pkg::MemorySink sink;
  if (st.Ok()) st = xlsx::WriteSave(book, plan, sink);
  CHECK(st.Ok());
  if (st.Ok()) {
    xlsx::CommitSave(book, ed, plan);
  } else {
    ed.EndSave();
  }
  CHECK(!ed.Saving());
  Keep(name, sink.bytes);
  return sink.bytes;
}

// A part's text in a saved file; empty when it hasn't the part.
std::string PartText(const std::vector<uint8_t>& zip, const std::string& part) {
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(zip, st);
  std::vector<uint8_t> out;
  if (!pkg || !pkg->ReadPart(part, out).Ok()) return {};
  return test::Text(out);
}

// What the checker (plan §10) says of a saved file: no errors.
void CheckClean(const std::vector<uint8_t>& zip) {
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(zip, st);
  CHECK(pkg != nullptr);
  if (!pkg) return;
  fw::pkg::CheckReport report = fw::pkg::CheckPackage(*pkg);
  for (const std::string& e : report.errors) std::printf("  check: %s\n", e.c_str());
  CHECK(report.Ok());
}

bool InOrder(const std::string& xml, const std::vector<std::string>& tags) {
  size_t at = 0;
  for (const std::string& t : tags) {
    size_t found = xml.find(t);
    if (found == std::string::npos || found < at) return false;
    at = found;
  }
  return true;
}

constexpr const char* kRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";

}  // namespace

TEST(sheet_save_cells_and_strings) {
  Parts parts = WorkbookParts({"<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row>"
                               "<row r=\"2\"><c r=\"A2\"><f>A1*2</f><v>2</v></c></row></sheetData>",
                               "<sheetData><row r=\"2\"><c r=\"B2\"><v>5</v></c></row></sheetData>"});
  // A calculation chain, which edits to cells make stale: Excel rebuilds it.
  AddPart(parts, "xl/calcChain.xml", "<calcChain xmlns=\"" + std::string(kMain) + "\"><c r=\"A2\" i=\"1\"/></calcChain>",
          "application/vnd.openxmlformats-officedocument.spreadsheetml.calcChain+xml", "xl/workbook.xml", "rC",
          std::string(kRel) + "calcChain", "calcChain.xml");
  std::vector<uint8_t> original = test::MakeZip(parts);
  auto book = OpenZip(original);
  if (!book) return;
  Editor ed(*book);
  CHECK(ed.SetCell(0, 0, 0, CellInput::Number(7)));
  CHECK(ed.SetCell(0, 0, 1, CellInput::Text("Hello & <you>")));
  CHECK(ed.SetCell(0, 2, 0, CellInput::Formula("A1+1")));
  CHECK(ed.StringsDirty() && !ed.StylesDirty());

  std::vector<uint8_t> saved = SaveBytes(*book, ed, "save-cells");
  CheckClean(saved);
  auto again = OpenZip(saved);
  if (!again) return;
  const Sheet& s = again->GetSheet(0);
  CHECK_EQ(Text(s, 0, 0), std::string("7"));
  CHECK_EQ(Text(s, 0, 1), std::string("Hello & <you>"));
  CHECK_EQ(FormulaText(s, 2, 0), std::string("A1+1"));
  CHECK_EQ(FormulaText(s, 1, 0), std::string("A1*2"));
  CHECK_EQ(Text(again->GetSheet(1), 1, 1), std::string("5"));
  // The workbook had no shared strings: they're added, with their relationship and content type.
  CHECK(again->StringsPart() == "/xl/sharedStrings.xml");
  CHECK(PartText(saved, "/xl/sharedStrings.xml").find("uniqueCount=\"1\"") != std::string::npos);
  CHECK(PartText(saved, "/[Content_Types].xml").find("sharedStrings+xml") != std::string::npos);
  // The calculation chain is gone, and so are its relationship and content type.
  CHECK(PartText(saved, "/xl/calcChain.xml").empty());
  CHECK(PartText(saved, "/xl/_rels/workbook.xml.rels").find("calcChain") == std::string::npos);
  CHECK(PartText(saved, "/[Content_Types].xml").find("calcChain") == std::string::npos);
  // What wasn't edited is as it was; the edited sheet's dimension covers its cells.
  CHECK(PartText(saved, "/xl/worksheets/sheet2.xml") == PartText(original, "/xl/worksheets/sheet2.xml"));
  CHECK(PartText(saved, "/xl/styles.xml") == PartText(original, "/xl/styles.xml"));
  CHECK(PartText(saved, "/xl/worksheets/sheet1.xml").find("<dimension ref=\"A1:B3\"/>") != std::string::npos);

  // Undone back to the file: the cells are as they were (the string stays in the table, unused).
  CHECK(ed.Undo(3));
  saved = SaveBytes(*book, ed, "save-undone");
  CheckClean(saved);
  again = OpenZip(saved);
  if (!again) return;
  CHECK_EQ(Text(again->GetSheet(0), 0, 0), std::string("1"));
  CHECK_EQ(Text(again->GetSheet(0), 0, 1), std::string(""));
  CHECK_EQ(FormulaText(again->GetSheet(0), 2, 0), std::string("-"));
}

TEST(sheet_save_layout) {
  std::string margins = "<pageMargins left=\"0.7\" right=\"0.7\" top=\"0.75\" bottom=\"0.75\" header=\"0.3\" footer=\"0.3\"/>";
  auto book = OpenParts({"<sheetViews><sheetView tabSelected=\"1\" workbookViewId=\"0\"/></sheetViews>"
                         "<sheetFormatPr defaultRowHeight=\"15\"/><sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row>"
                         "</sheetData>" + margins,
                         "<sheetViews><sheetView workbookViewId=\"0\"/></sheetViews><sheetData/>" + margins});
  if (!book) return;
  Editor ed(*book);
  CHECK(ed.SetColumnWidth(0, 1, 2, 20));
  CHECK(ed.Merge(0, Range{0, 0, 1, 1}));
  CHECK(ed.FreezePanes(0, 1, 0));
  CHECK(ed.SetTabColor(0, Color{Color::Kind::Rgb, 0xFFFF0000}));
  CHECK(ed.SetRowOutline(0, 3, 4, 1));
  ed.SetZoom(0, 150);
  ed.SetActiveSheet(1);

  std::vector<uint8_t> saved = SaveBytes(*book, ed, "save-layout");
  CheckClean(saved);
  std::string xml = PartText(saved, "/xl/worksheets/sheet1.xml");
  CHECK(InOrder(xml, {"<sheetPr>", "<dimension", "<sheetViews>", "<sheetFormatPr", "<cols>", "<sheetData>",
                      "<mergeCells", "<pageMargins"}));
  CHECK(xml.find("tabSelected") == std::string::npos);
  CHECK(xml.find("outlineLevelRow=\"1\"") != std::string::npos);
  CHECK(PartText(saved, "/xl/worksheets/sheet2.xml").find("tabSelected=\"1\"") != std::string::npos);
  // The workbook had no bookViews: one is added to say which tab is active.
  CHECK(InOrder(PartText(saved, "/xl/workbook.xml"), {"<bookViews><workbookView activeTab=\"1\"/></bookViews>", "<sheets>"}));

  auto again = OpenZip(saved);
  if (!again) return;
  CHECK(again->ActiveSheet() == 1);
  const Sheet& s = again->GetSheet(0);
  CHECK(s.Column(1).width == 20 && s.Column(2).width == 20 && s.Column(3).width == 0);
  CHECK(s.Merges().size() == 1 && s.Merges()[0] == (Range{0, 0, 1, 1}));
  CHECK(!s.Views().empty() && s.Views()[0].paneState == PaneState::Frozen && s.Views()[0].ySplit == 1);
  CHECK(s.Views()[0].zoomScale == 150);
  CHECK(s.Props().tabColor == (Color{Color::Kind::Rgb, 0xFFFF0000}));
  CHECK(s.Row(3).outlineLevel == 1 && s.Row(4).outlineLevel == 1);
  CHECK(s.Format().outlineLevelRow == 1);
}

TEST(sheet_save_prefixed_part) {
  Parts parts = WorkbookParts({"", "<sheetData/>"});
  PartOf(parts, "xl/worksheets/sheet1.xml") =
      "<x:worksheet xmlns:x=\"" + std::string(kMain) + "\"><x:sheetData><x:row r=\"1\"><x:c r=\"A1\"><x:v>1</x:v>"
      "</x:c></x:row></x:sheetData><x:pageMargins left=\"0.7\" right=\"0.7\" top=\"0.75\" bottom=\"0.75\" "
      "header=\"0.3\" footer=\"0.3\"/></x:worksheet>";
  auto book = OpenZip(test::MakeZip(parts));
  if (!book) return;
  Editor ed(*book);
  CHECK(ed.SetColumnWidth(0, 0, 0, 12.5));
  CHECK(ed.Merge(0, Range{2, 0, 2, 3}));
  std::vector<uint8_t> saved = SaveBytes(*book, ed, "save-prefixed");
  CheckClean(saved);
  std::string xml = PartText(saved, "/xl/worksheets/sheet1.xml");
  CHECK(InOrder(xml, {"<x:cols><x:col ", "<x:sheetData>", "<x:mergeCells count=\"1\"><x:mergeCell ref=\"A3:D3\"/>",
                      "<x:pageMargins"}));
  auto again = OpenZip(saved);
  if (!again) return;
  CHECK(again->GetSheet(0).Column(0).width == 12.5 && again->GetSheet(0).Merges().size() == 1);
}

TEST(sheet_save_sheets_names_styles) {
  Parts parts = WorkbookParts({"<sheetData/>", "<sheetData><row r=\"2\"><c r=\"B2\"><v>5</v></c></row></sheetData>",
                               "<sheetData/>"});
  // Sheet3 alone uses only3.png; it shares shared.png with Sheet1 (pictures nothing shows, which
  // Excel keeps).
  std::string& types = PartOf(parts, "[Content_Types].xml");
  types.insert(types.find("<Default "), "<Default Extension=\"png\" ContentType=\"image/png\"/>");
  const std::string png("\x89PNG\r\n\x1a\n", 8), image = std::string(kRel) + "image";
  AddPart(parts, "xl/media/only3.png", png, "", "xl/worksheets/sheet3.xml", "rX", image, "../media/only3.png");
  AddPart(parts, "xl/media/shared.png", png, "", "xl/worksheets/sheet3.xml", "rY", image, "../media/shared.png");
  AddRel(parts, "xl/worksheets/sheet1.xml", "rY", image, "../media/shared.png");
  std::vector<uint8_t> zip = test::MakeZip(parts);
  Keep("save-sheets-original", zip);
  auto book = OpenZip(zip);
  if (!book) return;
  Editor ed(*book);
  CHECK(ed.AddSheet(1, "New"));
  CHECK(ed.SetCell(1, 0, 0, CellInput::Number(42)));
  CHECK(ed.RenameSheet(0, "First"));
  CHECK(ed.DeleteSheet(3));
  CHECK(ed.SetName(DefinedName{"Rate", -1, "First!$A$1", false}));
  Font bold = book->GetStyles().Fonts()[0];
  bold.bold = true;
  Xf xf;
  xf.font = ed.AddFont(bold);
  xf.numFmt = ed.AddNumFmt("0.000");
  xf.applyFont = xf.applyNumFmt = true;
  uint32_t style = ed.AddXf(xf);
  CHECK(ed.SetCell(0, 0, 0, CellInput::Number(1.5).WithStyle(style)));

  std::vector<uint8_t> saved = SaveBytes(*book, ed, "save-sheets");
  CheckClean(saved);
  auto again = OpenZip(saved);
  if (!again) return;
  CHECK(again->SheetCount() == 3);
  CHECK(again->GetSheet(0).Name() == "First" && again->GetSheet(1).Name() == "New" &&
        again->GetSheet(2).Name() == "Sheet2");
  CHECK_EQ(Text(again->GetSheet(1), 0, 0), std::string("42"));
  CHECK_EQ(Text(again->GetSheet(2), 1, 1), std::string("5"));
  CHECK(again->FindName("Rate") && again->FindName("Rate")->formula == "First!$A$1");
  CHECK(again->FindName("Local", 2) && again->FindName("Total")->formula == "First!$A$1");
  // The new format.
  CellInfo c;
  CHECK(again->GetSheet(0).GetCell(0, 0, c));
  const Xf& got = again->GetStyles().CellXf(c.style);
  CHECK(again->GetStyles().FontOf(got).bold && again->GetStyles().FormatCode(got.numFmt) == "0.000");
  // Sheet3's part went, with what only it used.
  CHECK(PartText(saved, "/xl/worksheets/sheet3.xml").empty());
  CHECK(PartText(saved, "/xl/worksheets/_rels/sheet3.xml.rels").empty());
  CHECK(PartText(saved, "/xl/media/only3.png").empty());
  CHECK(!PartText(saved, "/xl/media/shared.png").empty());
  CHECK(PartText(saved, "/[Content_Types].xml").find("sheet3.xml") == std::string::npos);
}

// Workbooks Excel made (tests/sheet): the edits written, what they don't reach as Excel wrote it,
// and the saved file passes check. A chart sheet deleted takes its drawing and chart with it.
TEST(sheet_save_excel_files) {
  for (std::string name : {"objects", "table-styles", "condfmt-cases"}) {
    std::vector<uint8_t> original = TestFile(("sheet/" + name + ".xlsx").c_str());
    CHECK(!original.empty());
    auto book = OpenZip(original);
    if (!book) continue;
    Editor ed(*book);
    int ws = -1, chart = -1;
    for (size_t i = 0; i < book->SheetCount(); ++i) {
      if (ws < 0 && book->GetSheet(i).Kind() == SheetKind::Worksheet) ws = static_cast<int>(i);
      if (chart < 0 && book->GetSheet(i).Kind() == SheetKind::Chartsheet) chart = static_cast<int>(i);
    }
    CHECK(ws >= 0);
    if (ws < 0) continue;
    const std::string wsName = book->GetSheet(static_cast<size_t>(ws)).Name();
    CHECK(ed.SetCell(ws, 200, 0, CellInput::Text("Saved by Featherweight")));
    CHECK(ed.SetCell(ws, 201, 0, CellInput::Formula("1+1")));
    CHECK(ed.SetColumnWidth(ws, 30, 30, 15));
    CHECK(ed.Merge(ws, Range{203, 0, 203, 2}));
    CHECK(ed.AddSheet(-1, "Added"));
    ed.SetActiveSheet(static_cast<int>(book->SheetCount()) - 1);
    std::string chartName = chart >= 0 ? book->GetSheet(static_cast<size_t>(chart)).Name() : "";
    if (chart >= 0) CHECK(ed.DeleteSheet(chart));

    std::vector<uint8_t> saved = SaveBytes(*book, ed, "excel-" + name);
    CheckClean(saved);
    auto again = OpenZip(saved);
    if (!again) continue;
    int at = again->FindSheet(wsName);
    CHECK(at >= 0 && again->FindSheet("Added") == static_cast<int>(again->SheetCount()) - 1);
    if (at < 0) continue;
    CHECK_EQ(Text(again->GetSheet(static_cast<size_t>(at)), 200, 0), std::string("Saved by Featherweight"));
    CHECK_EQ(FormulaText(again->GetSheet(static_cast<size_t>(at)), 201, 0), std::string("1+1"));
    CHECK(again->ActiveSheet() == again->SheetCount() - 1);
    if (chart >= 0) CHECK(again->FindSheet(chartName) < 0);
  }
}

// Saving again after more edits: the workbook stands for the file it saved, so the second file
// has both rounds of edits, sheets added in each, and nothing the first save wrote is lost.
TEST(sheet_save_again) {
  auto book = OpenParts({"<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row></sheetData>", "<sheetData/>"});
  if (!book) return;
  Editor ed(*book);
  CHECK(ed.SetColumnWidth(0, 1, 1, 20));
  CHECK(ed.SetCell(0, 0, 1, CellInput::Text("first")));
  CHECK(ed.AddSheet(-1, "Added"));
  CHECK(ed.SetCell(2, 0, 0, CellInput::Number(3)));
  SaveBytes(*book, ed, "save-first");
  CHECK(!ed.Modified() && ed.CanUndo() && ed.DirtyPieces(0) == 0 && !ed.WorkbookDirty());
  CHECK(book->GetSheet(2).PartName() == "/xl/worksheets/sheet3.xml" && !book->GetSheet(2).RelId().empty());

  CHECK(ed.SetCell(0, 1, 0, CellInput::Text("second")));
  CHECK(ed.Merge(0, Range{4, 0, 4, 1}));
  CHECK(ed.AddSheet(-1, "Another"));
  std::vector<uint8_t> second = SaveBytes(*book, ed, "save-second");
  CheckClean(second);
  auto again = OpenZip(second);
  if (!again) return;
  CHECK(again->SheetCount() == 4 && again->GetSheet(2).Name() == "Added" && again->GetSheet(3).Name() == "Another");
  CHECK(again->GetSheet(3).PartName() != again->GetSheet(2).PartName());
  CHECK_EQ(Text(again->GetSheet(0), 0, 1), std::string("first"));
  CHECK_EQ(Text(again->GetSheet(0), 1, 0), std::string("second"));
  CHECK_EQ(Text(again->GetSheet(2), 0, 0), std::string("3"));
  CHECK(again->GetSheet(0).Column(1).width == 20 && again->GetSheet(0).Merges().size() == 1);

  // Undone back to the first edit: the added sheets go, and with them the undo history (as
  // deleting a sheet ends it in Excel: their parts aren't in the file any more).
  CHECK(ed.Undo(6));
  std::vector<uint8_t> third = SaveBytes(*book, ed, "save-third");
  CheckClean(third);
  again = OpenZip(third);
  if (!again) return;
  CHECK(again->SheetCount() == 2);
  CHECK_EQ(Text(again->GetSheet(0), 0, 1), std::string(""));
  CHECK_EQ(Text(again->GetSheet(0), 1, 0), std::string(""));
  CHECK(again->GetSheet(0).Column(1).width == 20 && again->GetSheet(0).Merges().empty());
  CHECK(PartText(third, "/xl/worksheets/sheet3.xml").empty() && PartText(third, "/xl/worksheets/sheet4.xml").empty());
  CHECK(!ed.CanUndo() && !ed.CanRedo() && !ed.Modified());
}

// Undo past a save makes the workbook differ from the file again, and redo makes it the same.
TEST(sheet_save_undo_past_a_save) {
  auto book = OpenParts({"<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row></sheetData>", "<sheetData/>"});
  if (!book) return;
  Editor ed(*book);
  CHECK(ed.SetColumnWidth(0, 1, 1, 20));
  CHECK(ed.SetTabColor(0, Color{Color::Kind::Rgb, 0xFF00FF00}));
  SaveBytes(*book, ed);
  CHECK(!ed.Modified() && ed.DirtyPieces(0) == 0);
  CHECK(ed.Undo(2));
  CHECK(ed.Modified() && (ed.DirtyPieces(0) & edit::kPieceCols) && (ed.DirtyPieces(0) & edit::kPieceProps));
  std::vector<uint8_t> saved = SaveBytes(*book, ed, "save-undo-past");
  CheckClean(saved);
  auto again = OpenZip(saved);
  if (!again) return;
  CHECK(again->GetSheet(0).Columns().empty() && !again->GetSheet(0).Props().tabColor.IsSet());
  CHECK(ed.Redo(2) && ed.Modified());
  saved = SaveBytes(*book, ed);
  again = OpenZip(saved);
  if (!again) return;
  CHECK(again->GetSheet(0).Column(1).width == 20 && again->GetSheet(0).Props().tabColor.IsSet());
  CHECK(ed.Undo(1) && ed.Modified() && ed.Redo(1) && !ed.Modified() && ed.DirtyPieces(0) == 0);
}

// While a save writes, the workbook is read-only: edits, undo and redo are refused, the view still
// changes (and stays to save), and a failed save leaves everything as it was.
TEST(sheet_save_read_only_while_saving) {
  auto book = OpenParts({"<sheetData/>", "<sheetData/>"});
  if (!book) return;
  Editor ed(*book);
  CHECK(ed.SetCell(0, 0, 0, CellInput::Number(1)));
  ed.BeginSave();
  CHECK(ed.Saving());
  CHECK(!ed.SetCell(0, 0, 0, CellInput::Number(2)) && !ed.Undo() && !ed.AddSheet(-1));
  xlsx::SavePlan plan;
  CHECK(xlsx::PrepareSave(*book, ed, plan).Ok());
  ed.SetSelection(0, CellAddr{3, 3}, {Range::Cell(3, 3)});
  fw::pkg::MemorySink sink;
  CHECK(xlsx::WriteSave(*book, plan, sink).Ok());
  xlsx::CommitSave(*book, ed, plan);
  CHECK(!ed.Saving() && ed.Modified() && ed.DirtyPieces(0) == edit::kPieceViews);
  CHECK(ed.SetCell(0, 0, 0, CellInput::Number(2)));
  ed.BeginSave();
  ed.EndSave();
  CHECK(!ed.Saving() && ed.Modified() && (ed.DirtyPieces(0) & edit::kPieceCells));
  CHECK(ed.Undo() && ed.DirtyPieces(0) == edit::kPieceViews);
}

// The format the file becomes: .xlsm keeps the VBA project, .xlsx leaves it out, .xltx is a
// template; the workbook part's content type says which (Excel refuses a mismatch).
TEST(sheet_save_formats) {
  Parts parts = WorkbookParts({"<sheetData/>", "<sheetData/>"});
  std::string& types = PartOf(parts, "[Content_Types].xml");
  types.insert(types.find("<Default "), "<Default Extension=\"bin\" ContentType=\"application/vnd.ms-office.vbaProject\"/>");
  const std::string xlsxMain = "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml";
  types.replace(types.find(xlsxMain), xlsxMain.size(), "application/vnd.ms-excel.sheet.macroEnabled.main+xml");
  AddPart(parts, "xl/vbaProject.bin", "VBA", "", "xl/workbook.xml", "rV",
          "http://schemas.microsoft.com/office/2006/relationships/vbaProject", "vbaProject.bin");
  auto book = OpenZip(test::MakeZip(parts));
  if (!book) return;
  CHECK(book->Format() == FileFormat::Xlsm && xlsx::HasMacros(*book));
  Editor ed(*book);
  CHECK(ed.SetCell(0, 0, 0, CellInput::Number(1)));
  std::vector<uint8_t> xlsm = SaveBytes(*book, ed);
  CHECK(PartText(xlsm, "/[Content_Types].xml").find("application/vnd.ms-excel.sheet.macroEnabled.main+xml") !=
        std::string::npos);
  CHECK(PartText(xlsm, "/xl/vbaProject.bin") == "VBA");

  std::vector<uint8_t> xlsx = SaveBytes(*book, ed, "save-as-xlsx", FileFormat::Xlsx);
  CheckClean(xlsx);
  std::string xlsxTypes = PartText(xlsx, "/[Content_Types].xml");
  CHECK(xlsxTypes.find("spreadsheetml.sheet.main+xml") != std::string::npos && xlsxTypes.find("macroEnabled") == std::string::npos);
  CHECK(PartText(xlsx, "/xl/vbaProject.bin").empty() &&
        PartText(xlsx, "/xl/_rels/workbook.xml.rels").find("vbaProject") == std::string::npos);
  CHECK(book->Format() == FileFormat::Xlsx && !xlsx::HasMacros(*book));

  std::vector<uint8_t> xltx = SaveBytes(*book, ed, "save-as-xltx", FileFormat::Xltx);
  CheckClean(xltx);
  auto again = OpenZip(xltx);
  if (!again) return;
  CHECK(again->Format() == FileFormat::Xltx);
  CHECK_EQ(Text(again->GetSheet(0), 0, 0), std::string("1"));
}

// A workbook read from a CSV file has no package: it gets the smallest one (GiveBasePackage) and is
// saved as an Excel workbook with every sheet, its formats and its strings.
TEST(sheet_save_csv_as_xlsx) {
  Workbook book;
  csv::Source source;
  csv::Options o;
  o.locale = &EnUsLocale();
  csv::Read("name,amount,when\nPen,12%,3/1/2024\n\"a, b\",-2,\n", "data", o, book, source);
  CHECK(!book.GetPackage() && book.Format() == FileFormat::Csv);
  Editor ed(book);
  CHECK(ed.SetCell(0, 3, 0, CellInput::Text("Total")));
  CHECK(ed.SetCell(0, 3, 1, CellInput::Formula("SUM(B2:B3)")));
  Font bold = book.GetStyles().FontOf(book.GetStyles().CellXf(0));
  bold.bold = true;
  const uint32_t boldFont = ed.AddFont(bold);
  CHECK(ed.MapStyles(0, Range{0, 0, 0, 2}, [&](uint32_t style) {
    Xf xf = book.GetStyles().CellXf(style);
    xf.font = boldFont;
    xf.applyFont = true;
    return ed.AddXf(xf);
  }));
  CHECK(ed.SortRows(0, Range{1, 0, 2, 2}, {1, 0}, {{1, false}}));  // by B; its state goes with the sheet
  CHECK(ed.AddSheet(-1));
  // Saved as CSV first (by the CSV writer): nothing is unsaved, but the .xlsx save writes it all.
  ed.SavedOutside(FileFormat::Csv);
  CHECK(!ed.Modified() && book.Format() == FileFormat::Csv);

  CHECK(xlsx::GiveBasePackage(book).Ok());
  std::vector<uint8_t> saved = SaveBytes(book, ed, "save-csv-as-xlsx", FileFormat::Xlsx);
  CheckClean(saved);
  CHECK(book.Format() == FileFormat::Xlsx && !ed.Modified());
  CHECK(PartText(saved, "/xl/worksheets/sheet1.xml").find("<sortState ref=\"A2:C3\">") != std::string::npos);
  // What Excel repairs: a sheetId of 0, sheetViews without a view.
  CHECK(PartText(saved, "/xl/workbook.xml").find("sheetId=\"0\"") == std::string::npos);
  CHECK(PartText(saved, "/xl/worksheets/sheet1.xml").find("<sheetView tabSelected=\"1\" workbookViewId=\"0\"/>") !=
        std::string::npos);
  auto again = OpenZip(saved);
  if (!again) return;
  CHECK(again->SheetCount() == 2 && again->GetSheet(0).Name() == "data");
  const Sheet& s = again->GetSheet(0);
  CHECK_EQ(Text(s, 0, 0), std::string("name"));
  CHECK_EQ(Text(s, 1, 0), std::string("a, b"));  // -2 sorts first
  CHECK_EQ(Text(s, 2, 0), std::string("Pen"));
  CHECK_EQ(FormulaText(s, 3, 1), std::string("SUM(B2:B3)"));
  auto formatOf = [](const Workbook& b, const Sheet& sh, uint32_t r, uint32_t c) {
    return std::string(b.GetStyles().FormatCode(b.GetStyles().CellXf(sh.StyleAt(r, c)).numFmt));
  };
  CHECK_EQ(formatOf(*again, s, 2, 1), formatOf(book, book.GetSheet(0), 2, 1));
  CHECK_EQ(formatOf(*again, s, 2, 2), formatOf(book, book.GetSheet(0), 2, 2));
  CHECK(!formatOf(*again, s, 2, 2).empty());
  CHECK(again->GetStyles().FontOf(again->GetStyles().CellXf(s.StyleAt(0, 1))).bold);
  CHECK(!again->GetStyles().FontOf(again->GetStyles().CellXf(s.StyleAt(1, 0))).bold);

  // Then saved like any workbook: what changed since.
  CHECK(ed.SetCell(0, 0, 0, CellInput::Text("item")));
  CHECK(ed.Modified());
  std::vector<uint8_t> next = SaveBytes(book, ed, "", FileFormat::Xlsx);
  CheckClean(next);
  auto third = OpenZip(next);
  if (!third) return;
  CHECK_EQ(Text(third->GetSheet(0), 0, 0), std::string("item"));
  CHECK(third->GetStyles().FontOf(third->GetStyles().CellXf(third->GetSheet(0).StyleAt(0, 0))).bold);
}

// ---- Crash recovery: the edit journal (sheet/edit/Journal.h)

namespace {

std::vector<uint8_t> JournalBook() {
  return test::MakeZip(WorkbookParts(
      {"<sheetData><row r=\"1\"><c r=\"A1\"><v>3</v></c><c r=\"B1\"><v>1</v></c></row>"
       "<row r=\"2\"><c r=\"A2\"><v>1</v></c><c r=\"B2\"><f>A2*2</f><v>2</v></c></row>"
       "<row r=\"3\"><c r=\"A3\"><v>2</v></c><c r=\"B3\"><f>SUM(A1:A3)</f><v>6</v></c></row></sheetData>",
       "<sheetData><row r=\"2\"><c r=\"B2\"><f>Sheet1!A1+1</f><v>4</v></c></row></sheetData>"}));
}

// Every kind of edit the journal records, with undo and redo among them.
void EditEverything(Workbook& book, Editor& ed) {
  CHECK(ed.SetCell(0, 0, 2, CellInput::Text("note & <more>")));
  CHECK(ed.SetCell(0, 3, 0, CellInput::Formula("SUM(A1:A3)")));
  CellInput spill = CellInput::Formula("_xlfn.SEQUENCE(2)");  // a dynamic array: cm, xl/metadata.xml
  spill.type = ValueType::Number, spill.number = 1;
  spill.dynamic = true;
  spill.arrayRange = Range{40, 0, 41, 0};
  CHECK(ed.SetCell(0, 40, 0, spill));
  CHECK(ed.SetCells(0, {{CellAddr{4, 0}, CellInput::Number(1.5)}, {CellAddr{4, 1}, CellInput::Bool(true)},
                        {CellAddr{4, 2}, CellInput::Error(ErrorCode::NA)}}));
  Font bold = book.GetStyles().Fonts()[0];
  bold.bold = true;
  const uint32_t font = ed.AddFont(bold);
  Fill fill;
  fill.pattern = PatternType::Solid;
  fill.fg = Color{Color::Kind::Rgb, 0xFFFFC000, 0};
  const uint32_t fillId = ed.AddFill(fill);
  Border border;
  border.bottom.style = BorderStyle::Thin;
  const uint32_t borderId = ed.AddBorder(border);
  Xf xf;
  xf.numFmt = ed.AddNumFmt("0.000");
  xf.font = font;
  xf.fill = fillId;
  xf.border = borderId;
  xf.applyNumFmt = xf.applyFont = xf.applyFill = xf.applyBorder = true;
  xf.alignment.horizontal = HAlign::Center;
  CHECK(ed.SetStyle(0, Range{4, 0, 4, 0}, ed.AddXf(xf)));
  CHECK(ed.MapStyles(0, Range{0, 0, 1, 1}, [&](uint32_t style) {
    Xf x = book.GetStyles().CellXf(style);
    x.font = font;
    x.applyFont = true;
    return ed.AddXf(x);
  }));
  CHECK(ed.ClearContents(0, Range{4, 1, 4, 1}));
  CHECK(ed.ClearAll(0, Range{4, 2, 4, 2}));
  CHECK(ed.SetRowHeight(0, 1, 2, 24));
  CHECK(ed.HideRows(0, 6, 6, true));
  CHECK(ed.SetRowOutline(0, 7, 8, 1));
  CHECK(ed.SetColumnWidth(0, 3, 3, 15.5));
  CHECK(ed.HideColumns(0, 5, 5, true));
  CHECK(ed.SetColumnOutline(0, 6, 7, 1, true));
  CHECK(ed.InsertRows(0, 1, 2));
  CHECK(ed.DeleteRows(0, 1, 1));
  CHECK(ed.InsertColumns(0, 0, 1));
  CHECK(ed.DeleteColumns(0, 0, 1));
  CHECK(ed.InsertCells(0, Range{9, 1, 9, 2}, Editor::Direction::DownOrUp));
  CHECK(ed.DeleteCells(0, Range{9, 1, 9, 1}, Editor::Direction::RightOrLeft));
  CHECK(ed.SortRows(0, Range{0, 0, 3, 0}, {2, 0, 3, 1}, {{0, false}}));
  CHECK(ed.Merge(0, Range{12, 0, 12, 2}));
  CHECK(ed.Merge(0, Range{13, 0, 13, 2}));
  CHECK(ed.Unmerge(0, Range{13, 0, 13, 2}));
  CHECK(ed.AddSheet(-1));
  CHECK(ed.AddSheet(0, "First"));
  CHECK(ed.RenameSheet(1, "Data & More"));
  CHECK(ed.MoveSheet(0, 3));
  CHECK(ed.SetTabColor(0, Color{Color::Kind::Theme, 4, -0.25}));
  CHECK(ed.SetSheetState(3, SheetState::Hidden));
  CHECK(ed.FreezePanes(0, 1, 1));
  ed.SetActiveSheet(1);
  ed.SetSelection(0, CellAddr{2, 2}, {Range{2, 2, 4, 3}});
  ed.SetTopLeft(0, CellAddr{5, 0});
  ed.SetZoom(0, 120);
  ed.SetZoom(0, 150);
  CHECK(ed.SetName(DefinedName{"Rate", -1, "'Data & More'!$A$1", false}));
  CHECK(ed.SetName(DefinedName{"Gone", 0, "1", false}));
  CHECK(ed.DeleteName("Gone", 0));
  ed.BeginGroup(u"Fill");
  CHECK(ed.SetCell(0, 20, 0, CellInput::Number(1)));
  CHECK(ed.SetCell(0, 21, 0, CellInput::Number(2)));
  ed.EndGroup();
  ed.BeginGroup(u"Cancelled");
  CHECK(ed.SetCell(0, 22, 0, CellInput::Number(3)));
  ed.CancelGroup();
  CHECK(ed.Undo(2));
  CHECK(ed.Redo());
  CHECK(!ed.DeleteSheet(9));  // refused: not recorded
  CHECK(ed.DeleteSheet(2));
}

// The parts of two saved files that differ.
std::vector<std::string> DifferentParts(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  fw::pkg::Status st;
  auto pa = test::OpenBytes(a, st), pb = test::OpenBytes(b, st);
  std::vector<std::string> out;
  if (!pa || !pb) return {"(unreadable)"};
  for (const auto& part : pa->Parts())
    if (PartText(a, part.name) != PartText(b, part.name)) out.push_back(part.name);
  if (pa->Parts().size() != pb->Parts().size()) out.push_back("(the parts listed)");
  return out;
}

}  // namespace

TEST(sheet_journal_replays_every_edit) {
  const std::vector<uint8_t> original = JournalBook();
  auto book = OpenZip(original);
  if (!book) return;
  Editor ed(*book);
  edit::Journal journal;
  ed.SetJournal(&journal);
  CHECK(ed.SetCell(0, 0, 0, CellInput::Number(7)));
  ed.SetZoom(0, 70);
  // Written in pieces, as the tab appends them to its recovery file.
  std::string records(journal.Unwritten());
  journal.Written();
  CHECK(journal.Unwritten().empty());
  CHECK(ed.SetCell(0, 30, 0, CellInput::Number(1)));
  EditEverything(*book, ed);
  ed.SetZoom(0, 90);
  const size_t before = journal.Size();
  ed.SetZoom(0, 80);  // a view change right after another of its kind replaces it
  CHECK(journal.Size() == before);
  records += journal.Unwritten();
  journal.Written();
  CHECK(journal.All() == records);

  auto again = OpenZip(original);
  if (!again) return;
  Editor replayed(*again);
  edit::Journal second;
  replayed.SetJournal(&second);
  size_t applied = 0;
  edit::Result r = replayed.Replay(records, &applied);
  if (!r) std::printf("  replay stopped after %zu: %s\n", applied, fw::WideToUtf8(std::wstring(r.message.begin(), r.message.end())).c_str());
  CHECK(r && applied > 60);
  // Replaying makes the same calls, so it records the same journal.
  CHECK(second.All() == records);
  CHECK(replayed.UndoNames() == ed.UndoNames() && replayed.RedoNames() == ed.RedoNames());
  CHECK(again->SheetCount() == book->SheetCount() && again->GetSheet(0).Name() == "Data & More");

  const std::vector<uint8_t> a = SaveBytes(*book, ed), b = SaveBytes(*again, replayed);
  for (const std::string& part : DifferentParts(a, b)) std::printf("  differs: %s\n", part.c_str());
  CHECK(DifferentParts(a, b).empty());
  CheckClean(b);

  // A view record the recovery file has already isn't replaced: the next one comes after it.
  const size_t inFile = second.Size();
  second.Written();
  replayed.SetZoom(0, 75);
  CHECK(second.Size() > inFile);
}

TEST(sheet_journal_refuses_damage) {
  const std::vector<uint8_t> original = JournalBook();
  auto book = OpenZip(original);
  if (!book) return;
  Editor ed(*book);
  edit::Journal journal;
  ed.SetJournal(&journal);
  CHECK(ed.SetCell(0, 0, 0, CellInput::Number(7)));
  CHECK(ed.SetCell(0, 1, 0, CellInput::Number(8)));
  CHECK(ed.SetCell(0, 2, 0, CellInput::Number(9)));
  const std::string records(journal.All());

  auto replayOn = [&](std::string_view bytes, size_t& applied) {
    auto fresh = OpenZip(original);
    Editor e(*fresh);
    edit::Result r = e.Replay(bytes, &applied);
    return std::make_pair(r.ok, Text(fresh->GetSheet(0), 1, 0));
  };
  size_t applied = 0;
  // Cut short (a crash while appending): the edits before the cut are made.
  auto [ok, a2] = replayOn(std::string_view(records).substr(0, records.size() - 2), applied);
  CHECK(!ok && applied == 3 && a2 == "8");
  // Without its version record, or from a newer version: nothing.
  CHECK(!replayOn(std::string_view(records).substr(3), applied).first && applied == 0);
  std::string newer = records;
  newer[2] = 99;  // the version record: kind 1, length 1, version
  CHECK(!replayOn(newer, applied).first && applied == 0);
  // An unknown kind of record stops there.
  std::string unknown = records + std::string("\x7f\x00", 2);
  CHECK(!replayOn(unknown, applied).first && applied == 4);
}

// A built-in cell style the workbook doesn't have yet (the styles gallery): added once, written
// with its format, and found again by Excel's gallery as "Good".
TEST(sheet_save_cell_style_added) {
  auto book = OpenParts({"<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row></sheetData>", "<sheetData/>"});
  if (!book) return;
  Editor ed(*book);
  edit::Journal journal;
  ed.SetJournal(&journal);
  Font green = book->GetStyles().Fonts()[0];
  green.color = Color{Color::Kind::Rgb, 0xFF006100, 0};
  Fill fill;
  fill.pattern = PatternType::Solid;
  fill.fg = Color{Color::Kind::Rgb, 0xFFC6EFCE, 0};
  Xf styleXf;
  styleXf.font = ed.AddFont(green);
  styleXf.fill = ed.AddFill(fill);
  styleXf.applyFont = styleXf.applyFill = true;
  CellStyle good;
  good.name = "Good";
  good.builtinId = 26;
  const uint32_t parent = ed.AddCellStyle(good, styleXf);
  CHECK(parent == 1 && ed.AddCellStyle(good, styleXf) == 1 && ed.StylesDirty());
  Xf cell = styleXf;
  cell.parent = parent;
  CHECK(ed.SetStyle(0, Range::Cell(0, 0), ed.AddXf(cell)));

  std::vector<uint8_t> saved = SaveBytes(*book, ed, "save-cell-style");
  CheckClean(saved);
  const std::string styles = PartText(saved, "/xl/styles.xml");
  CHECK(styles.find("<cellStyle name=\"Good\" xfId=\"1\" builtinId=\"26\"/>") != std::string::npos);
  CHECK(InOrder(styles, {"<cellStyleXfs count=\"2\">", "<cellXfs", "<cellStyles count=\"1\">"}));
  // As Excel writes a style's parts: "0" for those it leaves out, nothing for those it includes.
  const std::string styleXfs = styles.substr(styles.find("<cellStyleXfs"), styles.find("<cellXfs") - styles.find("<cellStyleXfs"));
  CHECK(styleXfs.find("applyNumberFormat=\"0\" applyBorder=\"0\" applyAlignment=\"0\" applyProtection=\"0\"/>") != std::string::npos);
  CHECK(styleXfs.find("applyFont") == std::string::npos && styleXfs.find("applyFill") == std::string::npos);
  auto again = OpenZip(saved);
  if (!again) return;
  const Styles& s = again->GetStyles();
  CHECK(s.CellStyles().size() == 1 && s.CellStyles()[0].name == "Good" && s.CellStyles()[0].xf == 1);
  const Xf& read = s.CellStyleXfs()[1];
  CHECK(read.applyFont && read.applyFill && !read.applyNumFmt && !read.applyBorder && !read.applyAlignment &&
        !read.applyProtection);
  CHECK(s.CellXf(again->GetSheet(0).StyleAt(0, 0)).parent == 1);
  CHECK(!ed.StylesDirty());

  // The journal adds it again on the file as it was (which had no cell styles listed).
  auto replayedBook = OpenParts({"<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row></sheetData>", "<sheetData/>"});
  if (!replayedBook) return;
  Editor replayed(*replayedBook);
  CHECK(replayed.Replay(journal.All()).ok);
  CHECK(replayedBook->GetStyles().CellStyles().size() == 1 && replayedBook->GetStyles().CellStyleXfs().size() == 2);
}

TEST(sheet_save_filter) {
  const std::string data =
      "<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c><c r=\"B1\"><v>2</v></c><c r=\"C1\"><v>3</v></c></row>"
      "<row r=\"2\"><c r=\"A2\"><v>4</v></c><c r=\"B2\"><v>5</v></c><c r=\"C2\"><v>6</v></c></row>"
      "<row r=\"3\"><c r=\"A3\"><v>7</v></c><c r=\"B3\"><v>8</v></c><c r=\"C3\"><v>9</v></c></row></sheetData>";
  auto book = OpenParts({data, "<sheetData/>"});
  if (!book) return;
  Editor ed(*book);
  edit::Journal journal;
  ed.SetJournal(&journal);
  SheetFilter f;
  f.ref = Range{0, 0, 2, 2};
  FilterColumn values;
  values.values = {"4"};
  values.blank = true;
  FilterColumn custom;
  custom.colId = 1;
  custom.kind = FilterColumn::Kind::Custom;
  custom.conditions = {{"greaterThan", "4"}, {"lessThan", "9"}};
  custom.both = true;
  FilterColumn top;
  top.colId = 2;
  top.kind = FilterColumn::Kind::Top10;
  top.count = 50;
  top.percent = true;
  top.threshold = 6;
  f.columns = {values, custom, top};
  CHECK(ed.SetFilter(0, f));
  CHECK(book->GetSheet(0).Filter() && book->GetSheet(0).Filter()->filtered && book->GetSheet(0).Props().filterMode);
  CHECK(book->Names().size() == 3 && book->Names()[2].name == "_xlnm._FilterDatabase" && book->Names()[2].hidden);
  const std::string journaled(journal.All());

  std::vector<uint8_t> saved = SaveBytes(*book, ed, "save-filter");
  CheckClean(saved);
  const std::string xml = PartText(saved, "/xl/worksheets/sheet1.xml");
  CHECK(xml.find("<sheetPr filterMode=\"1\"/>") != std::string::npos);
  CHECK(xml.find("<autoFilter ref=\"A1:C3\"><filterColumn colId=\"0\"><filters blank=\"1\"><filter val=\"4\"/></filters>"
                 "</filterColumn><filterColumn colId=\"1\"><customFilters and=\"1\"><customFilter operator=\"greaterThan\" "
                 "val=\"4\"/><customFilter operator=\"lessThan\" val=\"9\"/></customFilters></filterColumn>"
                 "<filterColumn colId=\"2\"><top10 percent=\"1\" val=\"50\" filterVal=\"6\"/></filterColumn></autoFilter>") !=
        std::string::npos);
  CHECK(InOrder(xml, {"<sheetPr", "<sheetData", "<autoFilter"}));
  CHECK(PartText(saved, "/xl/workbook.xml")
            .find("<definedName name=\"_xlnm._FilterDatabase\" localSheetId=\"0\" hidden=\"1\">Sheet1!$A$1:$C$3</definedName>") !=
        std::string::npos);

  // Read back as set.
  auto again = OpenZip(saved);
  if (!again) return;
  const std::optional<SheetFilter>& g = again->GetSheet(0).Filter();
  CHECK(g && g->ref == f.ref && g->columns.size() == 3 && again->GetSheet(0).Props().filterMode);
  if (g && g->columns.size() == 3) {
    CHECK(g->columns[0].kind == FilterColumn::Kind::Values && g->columns[0].blank && g->columns[0].values == values.values);
    CHECK(g->columns[1].kind == FilterColumn::Kind::Custom && g->columns[1].both && g->columns[1].conditions.size() == 2 &&
          g->columns[1].conditions[1].op == "lessThan" && g->columns[1].conditions[1].val == "9");
    CHECK(g->columns[2].kind == FilterColumn::Kind::Top10 && g->columns[2].count == 50 && g->columns[2].percent &&
          !g->columns[2].bottom && g->columns[2].threshold == 6);
  }

  // Undone: no filter, name or filterMode.
  CHECK(ed.Undo(1));
  CHECK(!book->GetSheet(0).Filter() && !book->GetSheet(0).Props().filterMode && book->Names().size() == 2);
  std::vector<uint8_t> undone = SaveBytes(*book, ed, "save-filter-undone");
  CHECK(PartText(undone, "/xl/worksheets/sheet1.xml").find("autoFilter") == std::string::npos);
  CHECK(PartText(undone, "/xl/worksheets/sheet1.xml").find("filterMode") == std::string::npos);
  CHECK(PartText(undone, "/xl/workbook.xml").find("_FilterDatabase") == std::string::npos);

  // Removed from a file that has one, keeping a column kind the model doesn't know (colors).
  Editor ed2(*again);
  SheetFilter colors = *g;
  colors.columns.resize(1);
  colors.columns[0].kind = FilterColumn::Kind::Other;
  colors.columns[0].xml = "<filterColumn colId=\"0\"><colorFilter dxfId=\"0\"/></filterColumn>";
  CHECK(ed2.SetFilter(0, colors));
  std::vector<uint8_t> colored = SaveBytes(*again, ed2, "save-filter-other");
  CHECK(PartText(colored, "/xl/worksheets/sheet1.xml").find("<colorFilter dxfId=\"0\"/>") != std::string::npos);
  CHECK(ed2.SetFilter(0, std::nullopt));
  std::vector<uint8_t> removed = SaveBytes(*again, ed2, "save-filter-removed");
  CheckClean(removed);
  CHECK(PartText(removed, "/xl/worksheets/sheet1.xml").find("autoFilter") == std::string::npos);
  CHECK(PartText(removed, "/xl/worksheets/sheet1.xml").find("filterMode") == std::string::npos);
  CHECK(PartText(removed, "/xl/workbook.xml").find("_FilterDatabase") == std::string::npos);

  // The journal sets it again.
  auto replayedBook = OpenParts({data, "<sheetData/>"});
  if (!replayedBook) return;
  Editor replayed(*replayedBook);
  CHECK(replayed.Replay(journaled).ok);
  const std::optional<SheetFilter>& r = replayedBook->GetSheet(0).Filter();
  CHECK(r && r->columns.size() == 3 && r->columns[1].conditions.size() == 2 && r->columns[2].threshold == 6);
  CHECK(replayedBook->Names().size() == 3);
}

TEST(sheet_save_moved_cells) {
  auto book = OpenParts({"<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c><c r=\"B1\"><f t=\"shared\" ref=\"B1:B3\" si=\"0\">A1*2</f>"
                         "</c><c r=\"D1\"><f>SUM(A1:A2)</f></c></row><row r=\"2\"><c r=\"A2\"><v>2</v></c><c r=\"B2\"><f t=\"shared\" "
                         "si=\"0\"/></c></row><row r=\"3\"><c r=\"B3\"><f t=\"shared\" si=\"0\"/></c></row></sheetData>"
                         "<mergeCells count=\"1\"><mergeCell ref=\"A5:B5\"/></mergeCells>",
                         "<sheetData/>"});
  if (!book) return;
  Editor ed(*book);
  CHECK(ed.MoveCells(0, Range{0, 0, 1, 0}, CellAddr{0, 5}));  // A1:A2 to F1:F2
  CHECK(ed.MoveCells(0, Range{4, 0, 4, 1}, CellAddr{7, 2}));  // the merge A5:B5 to C8:D8
  CHECK(ed.MoveCells(0, Range{0, 5, 1, 5}, 1, CellAddr{1, 1}));  // F1:F2 to Sheet2!B2:B3
  std::vector<uint8_t> saved = SaveBytes(*book, ed, "save-move-cells");
  CheckClean(saved);
  auto again = OpenZip(saved);
  if (!again) return;
  const Sheet& s = again->GetSheet(0);
  CHECK(FormulaText(s, 0, 1) == "Sheet2!B2*2" && FormulaText(s, 1, 1) == "Sheet2!B3*2" && FormulaText(s, 2, 1) == "A3*2");
  CHECK(FormulaText(s, 0, 3) == "SUM(Sheet2!B2:B3)" && s.Merges().size() == 1 && s.Merges()[0] == (Range{7, 2, 7, 3}));
  CHECK(Text(again->GetSheet(1), 1, 1) == "1" && Text(again->GetSheet(1), 2, 1) == "2");
}

// s2-corpus's registry findings: a formula in a cell kept as written (its value has xml:space) is
// saved as rewritten; a shared formula whose cells a band of cells moves apart is split; a formula
// nothing moved keeps its spaces.
TEST(sheet_save_registry_findings) {
  auto book = OpenParts({"<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row>"
                         "<row r=\"5\"><c r=\"B5\" t=\"str\"><f>A1&amp;\" x\"</f><v xml:space=\"preserve\">1 x</v></c></row>"
                         "<row r=\"6\"><c r=\"B6\"><f t=\"shared\" ref=\"B6:D6\" si=\"0\">$A$1+1</f><v>2</v></c>"
                         "<c r=\"C6\"><f t=\"shared\" si=\"0\"/><v>2</v></c><c r=\"D6\"><f t=\"shared\" si=\"0\"/><v>2</v></c></row>"
                         "</sheetData>",
                         "<sheetData><row r=\"1\"><c r=\"A1\"><f>IF(B1=\"Sheet1\", B2, B3)</f><v>1</v></c></row></sheetData>"});
  if (!book) return;
  Editor ed(*book);
  CHECK(ed.InsertCells(0, Range{0, 2, 0, 2}, Editor::Direction::DownOrUp));  // C1 down: C6 leaves its group's row
  CHECK(ed.InsertRows(0, 0, 1));
  std::vector<uint8_t> saved = SaveBytes(*book, ed, "save-registry-findings");
  CheckClean(saved);
  const std::string xml = PartText(saved, "/xl/worksheets/sheet1.xml");
  CHECK(xml.find("<f>A2&amp;\" x\"</f><v xml:space=\"preserve\">1 x</v>") != std::string::npos);
  auto again = OpenZip(saved);
  if (!again) return;
  const Sheet& s = again->GetSheet(0);
  CHECK_EQ(FormulaText(s, 5, 1), std::string("A2&\" x\""));
  CHECK_EQ(FormulaText(s, 6, 1), std::string("$A$2+1"));
  CHECK_EQ(FormulaText(s, 7, 2), std::string("$A$2+1"));  // C6, moved down twice, its own now
  CHECK_EQ(FormulaText(s, 6, 3), std::string("$A$2+1"));
  CHECK_EQ(FormulaText(again->GetSheet(1), 0, 0), std::string("IF(B1=\"Sheet1\", B2, B3)"));
}

// Parts an edit adds and removes, with their relationship and content type, undone with its step
// (S3: charts, pictures, notes and tables add parts).
TEST(sheet_edit_add_remove_part) {
  auto book = OpenParts({"<sheetData/>", "<sheetData/>"});
  if (!book) return;
  Editor ed(*book);
  const std::string sheetPart = book->GetSheet(0).PartName();
  const std::string type = "application/vnd.openxmlformats-officedocument.drawing+xml";
  const std::string rel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/drawing";
  const std::string xml = "<xdr:wsDr xmlns:xdr=\"http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing\"/>";
  CHECK(ed.AddPart(sheetPart, rel, "/xl/drawings/drawing1.xml", type, xml).empty());  // no group open
  const std::string name = ed.NewPartName("/xl/drawings/drawing", ".xml");
  CHECK_EQ(name, std::string("/xl/drawings/drawing1.xml"));
  ed.BeginGroup(u"Insert");
  const std::string id = ed.AddPart(sheetPart, rel, name, type, xml);
  ed.EndGroup();
  CHECK(!id.empty());
  const fw::pkg::Package& p = *book->GetPackage();
  auto relTo = [&]() -> const fw::pkg::Relationship* {
    if (const fw::pkg::RelationshipSet* set = p.RelationshipsOf(sheetPart))
      for (const fw::pkg::Relationship& r : set->items)
        if (r.id == id) return &r;
    return nullptr;
  };
  CHECK(ed.PartBytes(name) == xml && p.ContentTypeOf(name) == type);
  CHECK(relTo() && relTo()->target == "../drawings/drawing1.xml" && relTo()->type == rel);
  CHECK(ed.NewPartName("/xl/drawings/drawing", ".xml") == "/xl/drawings/drawing2.xml");
  // A second relationship to the same part, from the other sheet.
  ed.BeginGroup(u"Reuse");
  const std::string sheet2 = book->GetSheet(1).PartName();
  const std::string again = ed.AddRelationship(sheet2, rel, name);
  CHECK(ed.AddRelationship(sheet2, rel, "/xl/drawings/nothing.xml").empty());
  ed.EndGroup();
  CHECK(!again.empty() && p.RelationshipsOf(sheet2) && p.RelationshipsOf(sheet2)->items.back().targetPart == name);
  CHECK(ed.Undo());
  CHECK(!p.RelationshipsOf(sheet2) || p.RelationshipsOf(sheet2)->items.empty() || p.RelationshipsOf(sheet2)->items.back().targetPart != name);

  std::vector<uint8_t> saved = SaveBytes(*book, ed, "save-part-added");
  CheckClean(saved);
  CHECK(PartText(saved, name) == xml);
  CHECK(PartText(saved, "/[Content_Types].xml").find("PartName=\"/xl/drawings/drawing1.xml\"") != std::string::npos);
  CHECK(PartText(saved, "/xl/worksheets/_rels/sheet1.xml.rels").find("Target=\"../drawings/drawing1.xml\"") != std::string::npos);

  CHECK(ed.Undo());
  CHECK(ed.PartBytes(name).empty() && p.ContentTypeOf(name) != type && !relTo());
  CHECK(ed.Redo());
  CHECK(ed.PartBytes(name) == xml && relTo() && p.ContentTypeOf(name) == type);

  ed.BeginGroup(u"Delete");
  CHECK(ed.RemovePart(name));
  ed.EndGroup();
  CHECK(ed.PartBytes(name).empty() && !relTo() && p.ContentTypeOf(name) != type);
  std::vector<uint8_t> removed = SaveBytes(*book, ed, "save-part-removed");
  CheckClean(removed);
  CHECK(PartText(removed, name).empty());
  CHECK(PartText(removed, "/[Content_Types].xml").find("drawing1.xml") == std::string::npos);
  CHECK(ed.Undo());
  CHECK(ed.PartBytes(name) == xml && relTo() && p.ContentTypeOf(name) == type);
}

// Objects copied and pasted (s3-charts): a chart gets a part of its own, a picture is stored once,
// a chart pasted into another workbook keeps its values but not its references; undone with the parts.
TEST(sheet_copy_paste_object) {
  auto book = OpenParts({"<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row><row r=\"2\"><c r=\"A2\"><v>2</v></c></row></sheetData>",
                         "<sheetData/>"});
  if (!book) return;
  Editor ed(*book);
  Editor::NewChart c;
  c.data = Range{0, 0, 1, 0};
  CHECK(ed.InsertChart(0, c));
  // A picture beside it.
  static const unsigned char kPng[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
                                       0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
                                       0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
                                       0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE,
                                       0x42, 0x60, 0x82};
  const std::string drawing = ed.SheetDrawing(0, false);
  ed.BeginGroup(u"Insert Picture");
  const std::string rel = ed.AddPart(drawing, "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image", "/xl/media/image1.png",
                                     "image/png", std::string(reinterpret_cast<const char*>(kPng), sizeof kPng));
  CHECK(ed.AddAnchor(drawing,
                     "<xdr:oneCellAnchor xmlns:xdr=\"http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing\" "
                     "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><xdr:from><xdr:col>9</xdr:col><xdr:colOff>0</xdr:colOff>"
                     "<xdr:row>1</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:from><xdr:ext cx=\"952500\" cy=\"952500\"/><xdr:pic><xdr:nvPicPr>"
                     "<xdr:cNvPr id=\"3\" name=\"Picture 2\"/><xdr:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></xdr:cNvPicPr></xdr:nvPicPr>"
                     "<xdr:blipFill><a:blip xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" r:embed=\"" + rel +
                     "\"/><a:stretch><a:fillRect/></a:stretch></xdr:blipFill><xdr:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"952500\" "
                     "cy=\"952500\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></xdr:spPr></xdr:pic><xdr:clientData/></xdr:oneCellAnchor>"));
  ed.EndGroup();

  const Editor::ObjectClip chart = ed.CopyObject(0, 2), picture = ed.CopyObject(0, 3);
  CHECK(chart.parts.size() == 1 && chart.parts[0].name == "/xl/charts/chart1.xml" && picture.parts.size() == 1);
  CHECK(ed.CopyObject(0, 9).anchor.empty());
  edit::drawing::Place at;
  at.from = {1, 20, 0, 0};
  at.to = {9, 35, 0, 0};
  at.cx = at.cy = 952500;
  CHECK(ed.PasteObject(0, chart, at));
  CHECK(!ed.PartBytes("/xl/charts/chart2.xml").empty() && ed.ChartPart(0, 4) == "/xl/charts/chart2.xml");
  CHECK(ed.PasteObject(1, chart, at));  // another sheet: a drawing of its own
  CHECK(ed.ChartPart(1, 2) == "/xl/charts/chart3.xml" && ed.SheetDrawing(1, false) == "/xl/drawings/drawing2.xml");
  CHECK(ed.PasteObject(1, picture, at));
  CHECK(ed.PartBytes("/xl/media/image2.png").empty());  // stored once
  CHECK(ed.AnchorXml("/xl/drawings/drawing2.xml", 3).find("r:embed=\"rId2\"") != std::string::npos);
  // From another workbook: its values, not its references.
  Editor::ObjectClip other = chart;
  other.book = nullptr;
  CHECK(ed.PasteObject(0, other, at));
  const std::string detached = ed.PartBytes("/xl/charts/chart4.xml");
  CHECK(detached.find("<c:f>") == std::string::npos && detached.find("<c:numLit><c:formatCode>General</c:formatCode><c:ptCount val=\"2\"/>") != std::string::npos);

  std::vector<uint8_t> saved = SaveBytes(*book, ed, "save-objects-pasted");
  CheckClean(saved);
  CHECK(ed.Undo(4));
  CHECK(ed.PartBytes("/xl/charts/chart2.xml").empty() && ed.PartBytes("/xl/drawings/drawing2.xml").empty() && ed.SheetDrawing(1, false).empty());
}

// A chart edited (s3-charts): title, axis titles, legend, data labels, colors, type and Select Data,
// each keeping what it doesn't change; undone to the bytes it had.
TEST(sheet_edit_chart) {
  auto book = OpenParts({"<sheetData><row r=\"1\"><c r=\"B1\" t=\"inlineStr\"><is><t>Sales</t></is></c>"
                         "<c r=\"C1\" t=\"inlineStr\"><is><t>Costs</t></is></c></row>"
                         "<row r=\"2\"><c r=\"A2\" t=\"inlineStr\"><is><t>Jan</t></is></c><c r=\"B2\"><v>10</v></c><c r=\"C2\"><v>7</v></c></row>"
                         "<row r=\"3\"><c r=\"A3\" t=\"inlineStr\"><is><t>Feb</t></is></c><c r=\"B3\"><v>12</v></c><c r=\"C3\"><v>8</v></c></row>"
                         "<row r=\"4\"><c r=\"A4\" t=\"inlineStr\"><is><t>Mar</t></is></c><c r=\"B4\"><v>15</v></c><c r=\"C4\"><v>9.5</v></c></row>"
                         "</sheetData>",
                         "<sheetData/>"});
  if (!book) return;
  Editor ed(*book);
  Editor::NewChart c;
  c.data = Range{0, 0, 3, 2};
  CHECK(ed.InsertChart(0, c));
  const std::string part = ed.ChartPart(0, 2);
  CHECK_EQ(part, std::string("/xl/charts/chart1.xml"));
  CHECK(ed.ChartPart(0, 3).empty() && ed.ChartPart(1, 2).empty());
  const std::string original = ed.PartBytes(part);
  edit::ChartInfo info = edit::charts::Info(original);
  CHECK(info.kind == edit::ChartKind::Column && info.title.shown && info.title.text.empty() && info.legend == 'b' && !info.labels);
  CHECK(!info.xTitle.shown && info.series.size() == 2);
  if (info.series.size() == 2)
    CHECK(info.series[1].name == "Sheet1!$C$1" && info.series[1].values == "Sheet1!$C$2:$C$4" && info.series[1].categories == "Sheet1!$A$2:$A$4");

  edit::ChartEdit e;
  e.title = edit::ChartText{true, "Sales & costs"};
  e.xTitle = edit::ChartText{true, "Month"};
  e.yTitle = edit::ChartText{true, "Euro"};
  e.legend = 'r';
  e.labels = true;
  e.colors = {{1, -1, 0xFF0000}, {0, 2, 0x00B050}};
  CHECK(ed.EditChart(0, 2, e));
  std::string xml = ed.PartBytes(part);
  info = edit::charts::Info(xml);
  CHECK(info.title.text == "Sales & costs" && info.xTitle.text == "Month" && info.yTitle.text == "Euro" && info.legend == 'r' && info.labels);
  CHECK(xml.find("<a:srgbClr val=\"FF0000\"/>") != std::string::npos && xml.find("<c:dPt><c:idx val=\"2\"/>") != std::string::npos);
  CHECK(xml.find("rot=\"-5400000\"") != std::string::npos);  // the vertical axis's title
  CHECK(InOrder(xml, {"Sales &amp; costs</a:t>", "<c:autoTitleDeleted val=\"0\"/>", "<c:plotArea>", "<c:catAx>", "Month</a:t>",
                      "<c:valAx>", "Euro</a:t>", "<c:legend>"}));
  CheckClean(SaveBytes(*book, ed, "save-chart-titles"));

  // A line: the axes, titles and colors stay.
  e = {};
  e.kind = edit::ChartKind::Line;
  CHECK(ed.EditChart(0, 2, e));
  xml = ed.PartBytes(part);
  info = edit::charts::Info(xml);
  CHECK(info.kind == edit::ChartKind::Line && info.xTitle.text == "Month" && info.labels && info.series.size() == 2);
  CHECK(xml.find("<c:barChart>") == std::string::npos && xml.find("<a:ln w=\"28575\" cap=\"rnd\"><a:solidFill><a:srgbClr val=\"FF0000\"/>") != std::string::npos);
  CheckClean(SaveBytes(*book, ed, "save-chart-line"));
  // A pie: no axes.
  e.kind = edit::ChartKind::Pie;
  CHECK(ed.EditChart(0, 2, e));
  xml = ed.PartBytes(part);
  CHECK(xml.find("<c:pieChart>") != std::string::npos && xml.find("Ax>") == std::string::npos);
  CHECK(!edit::charts::Info(xml).xTitle.shown);
  // Select Data: one series, a name typed as text.
  e = {};
  e.series = std::vector<edit::ChartSeries>{{"=\"Costs only\"", "=Sheet1!$C$2:$C$4", "Sheet1!$A$2:$A$4"}};
  CHECK(ed.EditChart(0, 2, e));
  info = edit::charts::Info(ed.PartBytes(part));
  CHECK(info.series.size() == 1 && info.series[0].name == "=\"Costs only\"" && info.series[0].values == "Sheet1!$C$2:$C$4");
  CHECK(ed.PartBytes(part).find("<c:v>9.5</c:v>") != std::string::npos);
  e.series = std::vector<edit::ChartSeries>{{"", "Nowhere!$C$2:$C$4", ""}};
  CHECK(!ed.EditChart(0, 2, e));
  // No title.
  e = {};
  e.title = edit::ChartText{false, ""};
  e.legend = 0;
  CHECK(ed.EditChart(0, 2, e));
  info = edit::charts::Info(ed.PartBytes(part));
  CHECK(!info.title.shown && info.legend == 0 && ed.PartBytes(part).find("<c:autoTitleDeleted val=\"1\"/>") != std::string::npos);

  std::vector<uint8_t> saved = SaveBytes(*book, ed, "save-chart-edited");
  CheckClean(saved);
  CHECK(ed.Undo(5));
  CHECK(ed.PartBytes(part) == original);
}

// Charts' caches follow the cells at a save (s3-charts); a chart whose cells hold what it caches
// is saved byte for byte.
TEST(sheet_chart_caches_follow_cells) {
  auto book = OpenParts({"<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c><c r=\"B1\"><v>5</v></c></row>"
                         "<row r=\"2\"><c r=\"A2\"><v>2</v></c><c r=\"B2\"><v>6</v></c></row></sheetData>",
                         "<sheetData/>"});
  if (!book) return;
  Editor ed(*book);
  Editor::NewChart c;
  c.data = Range{0, 0, 1, 0};  // A1:A2
  CHECK(ed.InsertChart(0, c));
  c.data = Range{0, 1, 1, 1};  // B1:B2
  CHECK(ed.InsertChart(0, c));
  const std::string other = ed.PartBytes("/xl/charts/chart2.xml");
  CHECK(ed.SetCell(0, 1, 0, CellInput::Number(0.25)));
  std::vector<uint8_t> saved = SaveBytes(*book, ed, "save-chart-caches");
  CheckClean(saved);
  const std::string chart = PartText(saved, "/xl/charts/chart1.xml");
  CHECK(chart.find("<c:pt idx=\"1\"><c:v>0.25</c:v></c:pt>") != std::string::npos && chart.find("<c:v>2</c:v>") == std::string::npos);
  CHECK(PartText(saved, "/xl/charts/chart2.xml") == other);
  CHECK(ed.PartBytes("/xl/charts/chart1.xml").find("<c:v>2</c:v>") != std::string::npos);  // the package keeps the file's
}

// A drawing part's objects (s3-charts): found by shape id with their bytes, placed, and the
// relationships they use.
TEST(sheet_drawing_objects) {
  namespace dr = edit::drawing;
  const std::string xml =
      "<?xml version=\"1.0\"?>\r\n<xdr:wsDr xmlns:xdr=\"x\" xmlns:a=\"a\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
      "<!-- <xdr:twoCellAnchor> -->"
      "<xdr:twoCellAnchor editAs=\"oneCell\"><xdr:from><xdr:col>1</xdr:col><xdr:colOff>0</xdr:colOff><xdr:row>2</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:from>"
      "<xdr:to><xdr:col>3</xdr:col><xdr:colOff>0</xdr:colOff><xdr:row>4</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:to>"
      "<xdr:sp macro=\"\" textlink=\"\"><xdr:nvSpPr><xdr:cNvPr id=\"7\" name=\"a &gt; b\"/><xdr:cNvSpPr/></xdr:nvSpPr><xdr:spPr><a:xfrm>"
      "<a:off x=\"1\" y=\"2\"/><a:ext cx=\"3\" cy=\"4\"/></a:xfrm></xdr:spPr></xdr:sp><xdr:clientData/></xdr:twoCellAnchor>"
      "<mc:AlternateContent xmlns:mc=\"m\"><mc:Choice Requires=\"a14\"><xdr:twoCellAnchor><xdr:sp><xdr:nvSpPr><xdr:cNvPr id=\"12\" name=\"x\"/>"
      "</xdr:nvSpPr></xdr:sp></xdr:twoCellAnchor></mc:Choice></mc:AlternateContent>"
      "<xdr:oneCellAnchor><xdr:from><xdr:col>0</xdr:col><xdr:colOff>0</xdr:colOff><xdr:row>0</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:from>"
      "<xdr:ext cx=\"10\" cy=\"20\"/><xdr:pic><xdr:nvPicPr><xdr:cNvPr id=\"9\" name=\"p\"/></xdr:nvPicPr><xdr:blipFill><a:blip r:embed=\"rId4\"/>"
      "</xdr:blipFill></xdr:pic><xdr:clientData/></xdr:oneCellAnchor></xdr:wsDr>";
  const std::vector<dr::Object> objects = dr::Objects(xml);
  CHECK_EQ(objects.size(), size_t(3));
  if (objects.size() != 3) return;
  CHECK(objects[0].id == 7 && objects[1].id == 12 && objects[1].alternate && objects[2].id == 9);
  CHECK(xml.compare(objects[0].begin, 30, "<xdr:twoCellAnchor editAs=\"one") == 0);
  CHECK(xml.compare(objects[2].end - 20, 20, "</xdr:oneCellAnchor>") == 0);
  CHECK_EQ(dr::NextShapeId(xml), 13u);
  CHECK(dr::Find(objects, 9) == &objects[2] && !dr::Find(objects, 8) && !dr::Find(objects, 0));
  CHECK(dr::Objects("<a><b>").empty() && dr::Objects("<a/><b/>").empty());

  dr::Place p;
  p.from = {5, 6, 100, 200};
  p.to = {7, 8, 0, 0};
  p.x = 1000, p.y = 2000, p.cx = 3000, p.cy = 4000;
  const std::string two = dr::Placed(std::string_view(xml).substr(objects[0].begin, objects[0].end - objects[0].begin), p);
  CHECK(two.find("<xdr:from><xdr:col>5</xdr:col><xdr:colOff>100</xdr:colOff><xdr:row>6</xdr:row><xdr:rowOff>200</xdr:rowOff></xdr:from>") != std::string::npos);
  CHECK(two.find("<xdr:to><xdr:col>7</xdr:col>") != std::string::npos && two.find("<a:off x=\"1000\" y=\"2000\"/><a:ext cx=\"3000\" cy=\"4000\"/>") != std::string::npos);
  CHECK(two.find("name=\"a > b\"") != std::string::npos);  // pugixml escapes only & and < there
  const std::string one = dr::Placed(std::string_view(xml).substr(objects[2].begin, objects[2].end - objects[2].begin), p);
  CHECK(one.find("<xdr:ext cx=\"3000\" cy=\"4000\"/>") != std::string::npos && one.find("<xdr:col>5</xdr:col>") != std::string::npos);
  CHECK(dr::Placed(std::string_view(xml).substr(objects[1].begin, objects[1].end - objects[1].begin), p).empty());
  const std::vector<std::string> rels = dr::RelIds(std::string_view(xml).substr(objects[2].begin, objects[2].end - objects[2].begin));
  CHECK(rels.size() == 1 && rels[0] == "rId4");
  CHECK(dr::RelIds(xml).size() == 1);
}

// Insert > Chart (s3-charts): series guessed from the selection as Excel guesses them, written as
// Excel writes a new chart, undone with its parts.
TEST(sheet_insert_chart) {
  auto book = OpenParts({"<sheetData><row r=\"1\"><c r=\"B1\" t=\"inlineStr\"><is><t>Sales</t></is></c>"
                         "<c r=\"C1\" t=\"inlineStr\"><is><t>Costs</t></is></c></row>"
                         "<row r=\"2\"><c r=\"A2\" t=\"inlineStr\"><is><t>Jan</t></is></c><c r=\"B2\"><v>10</v></c><c r=\"C2\"><v>7</v></c></row>"
                         "<row r=\"3\"><c r=\"A3\" t=\"inlineStr\"><is><t>Feb</t></is></c><c r=\"B3\"><v>12</v></c><c r=\"C3\"><v>8</v></c></row>"
                         "<row r=\"4\"><c r=\"A4\" t=\"inlineStr\"><is><t>Mar</t></is></c><c r=\"B4\"><v>15</v></c><c r=\"C4\"><v>9.5</v></c></row>"
                         "</sheetData>",
                         "<sheetData/>"});
  if (!book) return;
  Editor ed(*book);
  Editor::NewChart c;
  c.data = Range{0, 0, 3, 2};  // A1:C4
  c.at = CellAddr{5, 1};
  CHECK(ed.InsertChart(0, c));
  const std::string chart = ed.PartBytes("/xl/charts/chart1.xml");
  CHECK(chart.find("<c:barDir val=\"col\"/>") != std::string::npos);
  CHECK(InOrder(chart, {"<c:f>Sheet1!$B$1</c:f>", "<c:f>Sheet1!$A$2:$A$4</c:f>", "<c:f>Sheet1!$B$2:$B$4</c:f>",
                        "<c:f>Sheet1!$C$1</c:f>", "<c:f>Sheet1!$C$2:$C$4</c:f>", "<c:legendPos val=\"b\"/>"}));
  CHECK(chart.find("<c:pt idx=\"2\"><c:v>9.5</c:v></c:pt>") != std::string::npos && chart.find("<c:v>Mar</c:v>") != std::string::npos);
  CHECK(ed.PartBytes("/xl/drawings/drawing1.xml").find("<xdr:row>5</xdr:row>") != std::string::npos);
  // A second chart joins the same drawing.
  c.kind = Editor::ChartKind::Pie;
  c.data = Range{0, 0, 3, 1};
  c.at = CellAddr{5, 10};
  CHECK(ed.InsertChart(0, c));
  CHECK(ed.PartBytes("/xl/charts/chart2.xml").find("<c:pieChart>") != std::string::npos);
  CHECK(ed.PartBytes("/xl/drawings/drawing2.xml").empty());
  // The drawing API pictures and shapes use (s3-data): a rectangle joins the drawing too.
  const std::string drawing = ed.SheetDrawing(0, false);
  CHECK_EQ(drawing, std::string("/xl/drawings/drawing1.xml"));
  CHECK(ed.SheetDrawing(1, false).empty());
  CHECK_EQ(ed.NextShapeId(drawing), 4u);
  CHECK(!ed.AddAnchor(drawing, "<xdr:absoluteAnchor/>"));  // no group open
  ed.BeginGroup(u"Insert Shape");
  CHECK(ed.AddAnchor(drawing,
                     "<xdr:twoCellAnchor xmlns:xdr=\"http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing\" "
                     "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><xdr:from><xdr:col>1</xdr:col><xdr:colOff>0</xdr:colOff>"
                     "<xdr:row>22</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:from><xdr:to><xdr:col>4</xdr:col><xdr:colOff>0</xdr:colOff>"
                     "<xdr:row>26</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:to><xdr:sp macro=\"\" textlink=\"\"><xdr:nvSpPr>"
                     "<xdr:cNvPr id=\"4\" name=\"Rectangle 3\"/><xdr:cNvSpPr/></xdr:nvSpPr><xdr:spPr><a:prstGeom prst=\"rect\">"
                     "<a:avLst/></a:prstGeom></xdr:spPr></xdr:sp><xdr:clientData/></xdr:twoCellAnchor>"));
  ed.EndGroup();
  CHECK_EQ(ed.NextShapeId(drawing), 5u);

  std::vector<uint8_t> saved = SaveBytes(*book, ed, "save-insert-chart");
  CheckClean(saved);
  CHECK(PartText(saved, "/xl/worksheets/sheet1.xml").find("drawing") != std::string::npos);
  auto again = OpenZip(saved);
  if (!again) return;
  // Moved and resized, then the pie deleted with its part.
  edit::drawing::Place place;
  place.from = {2, 30, 12700, 0};
  place.to = {9, 44, 0, 25400};
  CHECK(ed.MoveObject(0, 2, place));
  CHECK(ed.AnchorXml(drawing, 2).find("<xdr:row>30</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:from>") != std::string::npos);
  CHECK(ed.AnchorXml(drawing, 2).find("<xdr:colOff>12700</xdr:colOff>") != std::string::npos);
  CHECK(!ed.MoveObject(0, 99, place));
  CHECK(ed.DeleteObjects(0, {3}));
  CHECK(ed.PartBytes("/xl/charts/chart2.xml").empty() && ed.AnchorXml(drawing, 3).empty());
  CHECK(!ed.AnchorXml(drawing, 2).empty() && !ed.PartBytes("/xl/charts/chart1.xml").empty());
  CHECK(PartText(SaveBytes(*book, ed, "save-chart-moved"), "/xl/drawings/_rels/drawing1.xml.rels").find("chart2") == std::string::npos);
  // The last ones: the drawing goes too.
  CHECK(ed.DeleteObjects(0, {2, 4}));
  CHECK(ed.PartBytes(drawing).empty() && ed.PartBytes("/xl/charts/chart1.xml").empty() && ed.SheetDrawing(0, false).empty());
  std::vector<uint8_t> none = SaveBytes(*book, ed, "save-chart-deleted");
  CheckClean(none);
  CHECK(PartText(none, "/xl/worksheets/sheet1.xml").find("drawing") == std::string::npos);
  CHECK(PartText(none, "/[Content_Types].xml").find("chart") == std::string::npos);
  CHECK(ed.Undo(2));
  CHECK(ed.AnchorXml(drawing, 3).find("<c:chart") != std::string::npos && !ed.PartBytes("/xl/charts/chart2.xml").empty());

  CHECK(ed.Undo(4));
  CHECK(ed.PartBytes("/xl/charts/chart1.xml").empty() && ed.PartBytes("/xl/drawings/drawing1.xml").empty());
  std::vector<uint8_t> undone = SaveBytes(*book, ed, "save-insert-chart-undone");
  CheckClean(undone);
  CHECK(PartText(undone, "/xl/worksheets/sheet1.xml").find("drawing") == std::string::npos);
}

TEST(sheet_save_typed_dynamic_array) {
  // A typed formula that spills saves as Excel 365 writes a dynamic array: the cell's cm names a
  // cellMetadata block whose <rc> picks an XLDAPR block with fDynamic="1", in xl/metadata.xml
  // (added when the workbook has none, extended when it has one without it). Without cm, Excel
  // opens it as a legacy {=...} array.
  const std::string meta = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/sheetMetadata";
  const std::string metaType = "application/vnd.openxmlformats-officedocument.spreadsheetml.sheetMetadata+xml";
  auto type = [](Editor& ed, uint32_t col) {
    CellInput in = CellInput::Formula("_xlfn.SEQUENCE(3)");
    in.type = ValueType::Number, in.number = 1;
    in.dynamic = true;
    in.arrayRange = Range{0, col, 2, col};
    return ed.SetCell(0, 0, col, in);
  };
  auto dynamicAt = [](Workbook& book, uint32_t col) {
    FormulaInfo f;
    return book.GetSheet(0).GetFormula(0, col, f) && f.kind == FormulaKind::Array && f.dynamic;
  };
  const size_t npos = std::string::npos;
  {
    auto book = OpenParts({"<sheetData/>", "<sheetData/>"});
    if (!book) return;
    Editor ed(*book);
    CHECK(type(ed, 0) && type(ed, 2));  // the second uses the same block
    std::vector<uint8_t> saved = SaveBytes(*book, ed, "save-dynamic-array");
    CheckClean(saved);
    const std::string m = PartText(saved, "/xl/metadata.xml");
    CHECK(m.find("<metadataType name=\"XLDAPR\" minSupportedVersion=\"120000\" copy=\"1\" pasteAll=\"1\" pasteValues=\"1\" "
                 "merge=\"1\" splitFirst=\"1\" rowColShift=\"1\" clearFormats=\"1\" clearComments=\"1\" assign=\"1\" "
                 "coerce=\"1\" cellMeta=\"1\"/>") != npos &&
          m.find("fDynamic=\"1\"") != npos &&
          m.find("<cellMetadata count=\"1\"><bk><rc t=\"1\" v=\"0\"/></bk></cellMetadata>") != npos);
    CHECK(PartText(saved, "/[Content_Types].xml").find(metaType) != npos);
    const std::string rels = PartText(saved, "/xl/_rels/workbook.xml.rels");
    CHECK(rels.find(meta) != npos && rels.find("Target=\"metadata.xml\"") != npos);
    const std::string sheet = PartText(saved, "/xl/worksheets/sheet1.xml");
    CHECK(sheet.find("<c r=\"A1\" cm=\"1\">") != npos && sheet.find("<c r=\"C1\" cm=\"1\">") != npos);
    auto back = OpenZip(saved);
    CHECK(back && dynamicAt(*back, 0) && dynamicAt(*back, 2));
    CHECK(ed.Undo() && ed.Undo());
    CHECK(ed.PartBytes("/xl/metadata.xml").empty());
    std::vector<uint8_t> none = SaveBytes(*book, ed);
    CHECK(PartText(none, "/xl/metadata.xml").empty() && PartText(none, "/xl/worksheets/sheet1.xml").find("cm=") == npos);
  }
  {
    // A metadata part without XLDAPR (one that declares rich values only): the type, the block and
    // the cell metadata are added after what's there.
    Parts parts = WorkbookParts({"<sheetData/>", "<sheetData/>"});
    AddPart(parts, "xl/metadata.xml",
            "<metadata xmlns=\"" + std::string(kMain) +
                "\"><metadataTypes count=\"1\"><metadataType name=\"XLRICHVALUE\" minSupportedVersion=\"120000\" "
                "copy=\"1\" pasteAll=\"1\" pasteValues=\"1\" merge=\"1\" splitFirst=\"1\" rowColShift=\"1\" "
                "clearFormats=\"1\" clearComments=\"1\" assign=\"1\" coerce=\"1\"/></metadataTypes></metadata>",
            metaType, "xl/workbook.xml", "rM", meta, "metadata.xml");
    auto book = OpenZip(test::MakeZip(parts));
    if (!book) return;
    Editor ed(*book);
    CHECK(type(ed, 1));
    std::vector<uint8_t> saved = SaveBytes(*book, ed, "save-dynamic-array-meta");
    CheckClean(saved);
    const std::string m = PartText(saved, "/xl/metadata.xml");
    const size_t rich = m.find("<metadataType name=\"XLRICHVALUE\""), type2 = m.find("<metadataType name=\"XLDAPR\""),
                 dap = m.find("</metadataTypes><futureMetadata name=\"XLDAPR\" count=\"1\"><bk>"),
                 cells = m.find("</futureMetadata><cellMetadata count=\"1\"><bk><rc t=\"2\" v=\"0\"/></bk></cellMetadata></metadata>");
    CHECK(m.find("<metadataTypes count=\"2\">") != npos && rich < type2 && type2 < dap && dap < cells && cells != npos);
    CHECK(PartText(saved, "/xl/worksheets/sheet1.xml").find("<c r=\"B1\" cm=\"1\">") != npos);
    auto back = OpenZip(saved);
    CHECK(back && dynamicAt(*back, 1));
  }
  {
    // Excel's own: its block is used, the part stays as it was.
    const std::string xml =
        "<metadata xmlns=\"" + std::string(kMain) +
        "\" xmlns:xda=\"http://schemas.microsoft.com/office/spreadsheetml/2017/dynamicarray\"><metadataTypes count=\"1\">"
        "<metadataType name=\"XLDAPR\" minSupportedVersion=\"120000\" copy=\"1\" pasteAll=\"1\" pasteValues=\"1\" "
        "merge=\"1\" splitFirst=\"1\" rowColShift=\"1\" clearFormats=\"1\" clearComments=\"1\" assign=\"1\" coerce=\"1\" "
        "cellMeta=\"1\"/></metadataTypes><futureMetadata name=\"XLDAPR\" count=\"1\"><bk><extLst><ext "
        "uri=\"{bdbb8cdc-fa1e-496e-a857-3c3f30c029c3}\"><xda:dynamicArrayProperties fDynamic=\"1\" fCollapsed=\"0\"/>"
        "</ext></extLst></bk></futureMetadata><cellMetadata count=\"1\"><bk><rc t=\"1\" v=\"0\"/></bk></cellMetadata></metadata>";
    Parts parts = WorkbookParts({"<sheetData/>", "<sheetData/>"});
    AddPart(parts, "xl/metadata.xml", xml, metaType, "xl/workbook.xml", "rM", meta, "metadata.xml");
    auto book = OpenZip(test::MakeZip(parts));
    if (!book) return;
    Editor ed(*book);
    CHECK(type(ed, 0));
    CHECK(ed.PartBytes("/xl/metadata.xml") == xml);
    std::vector<uint8_t> saved = SaveBytes(*book, ed);
    CHECK(PartText(saved, "/xl/worksheets/sheet1.xml").find("<c r=\"A1\" cm=\"1\">") != npos);
  }
}
