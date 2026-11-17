// Saving an edited workbook (S2, src/sheet/xlsx/Save.cpp): edit, save, read the saved file back and
// check it.
#include "TestHarness.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "SheetTestUtil.h"
#include "pkg/Check.h"
#include "sheet/edit/Edit.h"
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

std::vector<uint8_t> SaveBytes(const Workbook& book, const Editor& ed, const std::string& name = {}) {
  xlsx::SavePlan plan;
  fw::pkg::Status st = xlsx::PrepareSave(book, ed, plan);
  if (!st.Ok()) std::printf("  PrepareSave: %s\n", st.message.c_str());
  CHECK(st.Ok());
  fw::pkg::MemorySink sink;
  CHECK(book.GetPackage()->Save(sink, plan.changes).Ok());
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
  // Sheet3 alone uses only3.xml; it shares shared.xml with Sheet1.
  AddPart(parts, "xl/extra/only3.xml", "<only/>", "", "xl/worksheets/sheet3.xml", "rX", "urn:test", "../extra/only3.xml");
  AddPart(parts, "xl/extra/shared.xml", "<shared/>", "", "xl/worksheets/sheet3.xml", "rY", "urn:test",
          "../extra/shared.xml");
  AddRel(parts, "xl/worksheets/sheet1.xml", "rY", "urn:test", "../extra/shared.xml");
  auto book = OpenZip(test::MakeZip(parts));
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
  CHECK(PartText(saved, "/xl/extra/only3.xml").empty());
  CHECK(!PartText(saved, "/xl/extra/shared.xml").empty());
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
