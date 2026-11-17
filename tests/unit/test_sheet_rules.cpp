// Conditional formats and data validation as the sheet's XML (s3-data): sheet/data/Rules read and
// build them, Editor::SetSheetRules and AddDxf put them in the workbook, saving writes them.
#include "TestHarness.h"

#include <cstdio>
#include <string>
#include <vector>

#include "SheetTestUtil.h"
#include "pkg/Check.h"
#include "sheet/calc/Entry.h"
#include "sheet/condfmt/CondFormat.h"
#include "sheet/data/Rules.h"
#include "sheet/edit/Edit.h"
#include "sheet/edit/Journal.h"
#include "sheet/model/Workbook.h"
#include "sheet/xlsx/Xlsx.h"

using namespace fw::sheet;
using edit::Editor;
using namespace sheet_test;

namespace {

std::vector<uint8_t> Save(Workbook& book, Editor& ed) {
  ed.BeginSave();
  xlsx::SavePlan plan;
  fw::pkg::Status st = xlsx::PrepareSave(book, ed, plan, book.Format());
  CHECK(st.Ok());
  fw::pkg::MemorySink sink;
  if (st.Ok()) st = xlsx::WriteSave(book, plan, sink);
  CHECK(st.Ok());
  if (st.Ok()) xlsx::CommitSave(book, ed, plan);
  else ed.EndSave();
  wchar_t dir[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"FW_SAVE_OUT", dir, MAX_PATH);
  if (n && n < MAX_PATH) CHECK(test::WriteFileBytes(std::wstring(dir) + L"\\rules.xlsx", sink.bytes));
  return sink.bytes;
}

std::string Part(const std::vector<uint8_t>& zip, const std::string& part) {
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(zip, st);
  std::vector<uint8_t> out;
  if (!pkg || !pkg->ReadPart(part, out).Ok()) return {};
  return test::Text(out);
}

void Clean(const std::vector<uint8_t>& zip) {
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(zip, st);
  CHECK(pkg != nullptr);
  if (!pkg) return;
  fw::pkg::CheckReport report = fw::pkg::CheckPackage(*pkg);
  for (const std::string& e : report.errors) std::printf("  check: %s\n", e.c_str());
  CHECK(report.Ok());
}

const char* kSheet =
    "<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c><c r=\"B1\"><v>5</v></c></row>"
    "<row r=\"2\"><c r=\"A2\"><v>2</v></c></row><row r=\"3\"><c r=\"A3\"><v>3</v></c></row></sheetData>"
    "<conditionalFormatting sqref=\"B1:B3\"><cfRule type=\"cellIs\" priority=\"1\" operator=\"greaterThan\">"
    "<formula>4</formula></cfRule></conditionalFormatting>"
    "<pageMargins left=\"0.7\" right=\"0.7\" top=\"0.75\" bottom=\"0.75\" header=\"0.3\" footer=\"0.3\"/>";

}  // namespace

TEST(SheetRules_Areas) {
  using data::CutAreas;
  using data::JoinAreas;
  using data::ParseAreas;
  CHECK(JoinAreas(ParseAreas("A1:B5  D2")) == "A1:B5 D2");
  CHECK(JoinAreas(CutAreas(ParseAreas("A1:C3"), Range{1, 1, 1, 1})) == "A1:C1 A2 C2 A3:C3");
  CHECK(JoinAreas(CutAreas(ParseAreas("A1:A3 C1"), Range{0, 0, 2, 0})) == "C1");
  CHECK(data::AreasContain(ParseAreas("A1:B5 D2"), 1, 3));
  CHECK(!data::AreasContain(ParseAreas("A1:B5 D2"), 1, 2));
}

TEST(SheetRules_ConditionalFormats) {
  auto book = OpenParts({kSheet, "<sheetData/>"});
  if (!book) return;
  Editor ed(*book);
  std::vector<data::CfRule> rules = data::ReadCfRules(book->GetSheet(0).Text());
  CHECK(rules.size() == 1 && rules[0].type == "cellIs" && rules[0].sqref == "B1:B3");
  const uint32_t dxf = ed.AddDxf(data::CfPreset(0));
  CHECK(ed.AddDxf(data::CfPreset(0)) == dxf);  // the same format once
  // New rules go first, as in Excel.
  rules.insert(rules.begin(), data::DataBarRule("A1:A3", 0, "{00000000-0000-4000-8000-000000000001}"));
  rules.insert(rules.begin(), data::CellIsRule("A1:A3", data::CfOp::Between, "2", "3", dxf));
  rules.push_back(data::IconSetRule("A1:A3", 15, "{00000000-0000-4000-8000-000000000002}"));  // 3Stars: x14 only
  rules.push_back(data::TextRule("C1:C3", data::CfText::Contains, "a\"b", dxf));
  rules[2].stopIfTrue = true;
  std::string xml, ext;
  data::WriteCfRules(rules, book->GetSheet(0).Text(), xml, ext);
  CHECK(ed.SetSheetRules(0, Editor::RuleKind::ConditionalFormats, xml, ext, u"New Rule"));
  std::vector<data::CfRule> again = data::ReadCfRules(book->GetSheet(0).Text());
  CHECK(again.size() == 5);
  if (again.size() == 5) {
    CHECK(again[0].type == "cellIs" && again[1].type == "dataBar" && !again[1].ext.empty());
    CHECK(again[2].type == "cellIs" && again[2].stopIfTrue);
    CHECK(again[3].type == "iconSet" && again[3].xml.empty() && !again[3].ext.empty());
    CHECK(again[4].type == "containsText");
  }
  const calc::FormulaLocale& inv = calc::InvariantFormulaLocale();
  CHECK(data::DescribeCf(again[0], inv) == L"Cell Value between 2 and 3");
  CHECK(data::DescribeCf(again[4], inv) == L"Cell Value contains \"a\"b\"");
  // The view's rules read them.
  {
    condfmt::SheetRules r(*book, 0);
    CHECK(r.RuleCount() == 5);
    condfmt::Style st;
    CHECK(r.At(1, 0, st) && (st.set & condfmt::Style::kFill) && (st.set & condfmt::Style::kBar));
  }
  ed.Undo();
  CHECK(data::ReadCfRules(book->GetSheet(0).Text()).size() == 1);
  ed.Redo();
  std::vector<uint8_t> saved = Save(*book, ed);
  Clean(saved);
  const std::string sheet = Part(saved, "/xl/worksheets/sheet1.xml");
  CHECK(sheet.find("<conditionalFormatting sqref=\"A1:A3\"><cfRule type=\"cellIs\" dxfId=\"0\" priority=\"1\"") != std::string::npos);
  CHECK(sheet.find("<extLst><ext uri=\"{78C0D931-6437-407d-A8EE-F0AAD7539E65}\"") != std::string::npos);
  CHECK(sheet.find("</conditionalFormatting><pageMargins") != std::string::npos);
  CHECK(sheet.find("NOT(ISERROR(SEARCH(&quot;a&quot;&quot;b&quot;,C1)))") != std::string::npos ||
        sheet.find("NOT(ISERROR(SEARCH(\"a\"\"b\",C1)))") != std::string::npos);
  CHECK(Part(saved, "/xl/styles.xml").find("<dxfs count=\"1\"><dxf><font><color rgb=\"FF9C0006\"/></font><fill><patternFill>"
                                           "<bgColor rgb=\"FFFFC7CE\"/></patternFill></fill></dxf></dxfs>") != std::string::npos);
  auto reopened = OpenZip(saved);
  if (!reopened) return;
  CHECK(data::ReadCfRules(reopened->GetSheet(0).Text()).size() == 5);
  CHECK(reopened->GetStyles().Dxfs().size() == 1);
  // Clear Rules from a column; then from the sheet (nothing left: the extension goes too).
  std::vector<data::CfRule> left = data::ReadCfRules(reopened->GetSheet(0).Text());
  data::ClearCfRules(left, Range{0, 0, kMaxRows - 1, 0});
  CHECK(left.size() == 2);
  Editor ed2(*reopened);
  data::WriteCfRules({}, reopened->GetSheet(0).Text(), xml, ext);
  CHECK(xml.empty() && ext.empty());
  CHECK(ed2.SetSheetRules(0, Editor::RuleKind::ConditionalFormats, xml, ext, u"Clear Rules"));
  CHECK(reopened->GetSheet(0).Text().afterData.find("extLst") == std::string::npos);
  CHECK(reopened->GetSheet(0).Text().afterData.find("pageMargins") != std::string::npos);
}

TEST(SheetRules_Validations) {
  auto book = OpenParts({kSheet, "<sheetData/>"});
  if (!book) return;
  Editor ed(*book);
  CHECK(data::ReadValidations(book->GetSheet(0).Text()).empty());
  data::Validation list;
  list.type = data::Validation::Type::List;
  list.f1 = "\"red,green,\"\"blue\"\"\"";
  list.sqref = "A1:A3";
  list.prompt = "Pick one";
  data::Validation other;
  other.type = data::Validation::Type::List;
  other.f1 = "Sheet2!$A$1:$A$3";
  other.sqref = "B1";
  data::Validation whole;
  whole.type = data::Validation::Type::Whole;
  whole.op = data::CfOp::GreaterEqual;
  whole.f1 = "10";
  whole.alert = data::Validation::Alert::Warning;
  whole.sqref = "C1:C5";
  std::string xml, ext;
  data::WriteValidations({list, other, whole}, book->GetSheet(0).Text(), xml, ext);
  CHECK(ed.SetSheetRules(0, Editor::RuleKind::Validations, xml, ext, u"Data Validation"));
  std::vector<data::Validation> read = data::ReadValidations(book->GetSheet(0).Text());
  CHECK(read.size() == 3);
  if (read.size() != 3) return;
  CHECK(!read[0].x14 && read[1].type == data::Validation::Type::Whole && read[2].x14);
  CHECK(read[2].f1 == "Sheet2!$A$1:$A$3" && read[2].sqref == "B1");
  CHECK(read[1].op == data::CfOp::GreaterEqual && read[1].alert == data::Validation::Alert::Warning);
  const std::vector<std::string> items = data::ListItems(read[0]);
  CHECK(items.size() == 3 && items[2] == "\"blue\"");
  CHECK(data::ValidationAt(read, 2, 0) == &read[0] && data::ValidationAt(read, 0, 1) == &read[2]);
  CHECK(!data::ValidationAt(read, 5, 0));
  std::vector<uint8_t> saved = Save(*book, ed);
  Clean(saved);
  const std::string sheet = Part(saved, "/xl/worksheets/sheet1.xml");
  CHECK(sheet.find("<dataValidations count=\"2\"><dataValidation type=\"list\" allowBlank=\"1\" showInputMessage=\"1\" "
                   "showErrorMessage=\"1\" prompt=\"Pick one\" sqref=\"A1:A3\">") != std::string::npos);
  CHECK(sheet.find("<x14:dataValidation type=\"list\" allowBlank=\"1\" showInputMessage=\"1\" showErrorMessage=\"1\">"
                   "<x14:formula1><xm:f>Sheet2!$A$1:$A$3</xm:f></x14:formula1><xm:sqref>B1</xm:sqref>") != std::string::npos);
  // Untouched rules keep their XML; Clear All from a range splits the others' cells.
  auto again = OpenZip(saved);
  if (!again) return;
  std::vector<data::Validation> v = data::ReadValidations(again->GetSheet(0).Text());
  data::ClearValidations(v, Range{1, 0, 1, 0});
  CHECK(v.size() == 3 && v[0].sqref == "A1 A3");
  data::WriteValidations(v, again->GetSheet(0).Text(), xml, ext);
  CHECK(xml.find("sqref=\"A1 A3\"") != std::string::npos);
  CHECK(xml.find("<formula1>10</formula1>") != std::string::npos);
}

// Measured on Excel 2021 (Validation.Value): a typed list "Red; Green; Blue;10".
TEST(SheetRules_ListMatch) {
  data::Validation v;
  v.type = data::Validation::Type::List;
  v.f1 = "\"Red,Green, Blue,10\"";
  const std::vector<std::string> items = data::ListItems(v);
  CHECK(data::InList(items, "Red") && data::InList(items, "Blue") && data::InList(items, " Blue"));
  CHECK(data::InList(items, "10"));
  CHECK(!data::InList(items, "red") && !data::InList(items, "RED") && !data::InList(items, "Gre"));
  CHECK(!data::InList(items, "1e1"));
}

TEST(SheetRules_Journal) {
  auto book = OpenParts({kSheet, "<sheetData/>"});
  if (!book) return;
  Editor ed(*book);
  edit::Journal journal;
  ed.SetJournal(&journal);
  const uint32_t dxf = ed.AddDxf(data::CfPreset(2));
  std::string xml, ext;
  data::WriteCfRules({data::DuplicateRule("A1:A3", false, dxf), data::DataBarRule("B1:B3", 7)}, book->GetSheet(0).Text(),
                     xml, ext);
  CHECK(ed.SetSheetRules(0, Editor::RuleKind::ConditionalFormats, xml, ext, u"Duplicate Values"));
  ed.SetJournal(nullptr);
  auto fresh = OpenParts({kSheet, "<sheetData/>"});
  if (!fresh) return;
  Editor replay(*fresh);
  CHECK(replay.Replay(journal.All()));
  CHECK(fresh->GetSheet(0).Text().afterData == book->GetSheet(0).Text().afterData);
  CHECK(fresh->GetStyles().Dxfs().size() == book->GetStyles().Dxfs().size());
}
