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
