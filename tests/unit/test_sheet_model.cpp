// The workbook model and its .xlsx reader and writer (S1, src/sheet/model and src/sheet/xlsx).
#include "TestHarness.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "PkgTestUtil.h"
#include "pkg/Canonical.h"
#include "sheet/model/TableStyles.h"
#include "sheet/model/Workbook.h"
#include "sheet/xlsx/Internal.h"
#include "sheet/xlsx/Xlsx.h"

using namespace fw::sheet;

namespace {

constexpr const char* kMain = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";

std::string Ns() {
  return std::string(" xmlns=\"") + kMain +
         "\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
         " xmlns:x14ac=\"http://schemas.microsoft.com/office/spreadsheetml/2009/9/ac\"";
}

// A workbook package from its sheets' XML (sheetData and around it), with styles and strings.
std::vector<std::pair<std::string, std::string>> Package(const std::vector<std::string>& sheets, const std::string& strings,
                                                         const std::string& workbookExtra = {}) {
  std::string types =
      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
      "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
      "<Override PartName=\"/xl/workbook.xml\" "
      "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
      "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
      "<Override PartName=\"/xl/sharedStrings.xml\" "
      "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml\"/>";
  std::string rels = "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                     "<Relationship Id=\"rS\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
                     "<Relationship Id=\"rT\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings\" Target=\"sharedStrings.xml\"/>";
  std::string list;
  std::vector<std::pair<std::string, std::string>> parts;
  for (size_t i = 0; i < sheets.size(); ++i) {
    std::string n = std::to_string(i + 1);
    types += "<Override PartName=\"/xl/worksheets/sheet" + n +
             ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
    rels += "<Relationship Id=\"rId" + n +
            "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet" + n +
            ".xml\"/>";
    list += "<sheet name=\"Sheet " + n + "\" sheetId=\"" + n + "\" r:id=\"rId" + n + "\"/>";
    parts.push_back({"xl/worksheets/sheet" + n + ".xml", sheets[i]});
  }
  types += "</Types>";
  rels += "</Relationships>";
  parts.insert(parts.begin(),
               {{"[Content_Types].xml", types},
                {"_rels/.rels",
                 "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                 "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
                 "Target=\"xl/workbook.xml\"/></Relationships>"},
                {"xl/workbook.xml", "<workbook" + Ns() + ">" + workbookExtra + "<sheets>" + list + "</sheets>"
                 "<definedNames><definedName name=\"Total\">'Sheet 1'!$A$1</definedName>"
                 "<definedName name=\"_xlnm.Print_Area\" localSheetId=\"0\">'Sheet 1'!$A$1:$C$3</definedName></definedNames>"
                 "<calcPr calcId=\"191029\" fullCalcOnLoad=\"1\"/><extLst><ext uri=\"{B58B0392-4F1F-4190-BB64-5DF3571DCE5F}\" "
                 "xmlns:xcalcf=\"http://schemas.microsoft.com/office/spreadsheetml/2018/calcfeatures\"><xcalcf:calcFeatures>"
                 "<xcalcf:feature name=\"microsoft.com:RD\"/><xcalcf:feature name=\"microsoft.com:Single\"/>"
                 "</xcalcf:calcFeatures></ext></extLst></workbook>"},
                {"xl/_rels/workbook.xml.rels", rels},
                {"xl/styles.xml",
                 "<styleSheet xmlns=\"" + std::string(kMain) + "\">"
                 "<numFmts count=\"1\"><numFmt numFmtId=\"164\" formatCode=\"0.000\"/></numFmts>"
                 "<fonts count=\"2\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font>"
                 "<font><b/><i val=\"0\"/><u/><sz val=\"14\"/><color theme=\"4\" tint=\"-0.5\"/><name val=\"Arial\"/></font></fonts>"
                 "<fills count=\"3\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill>"
                 "<fill><patternFill patternType=\"solid\"><fgColor indexed=\"10\"/></patternFill></fill></fills>"
                 "<borders count=\"2\"><border><left/><right/><top/><bottom/><diagonal/></border>"
                 "<border><left style=\"thin\"><color rgb=\"FF00FF00\"/></left><bottom style=\"double\"/></border></borders>"
                 "<cellStyleXfs count=\"1\"><xf/></cellStyleXfs>"
                 "<cellXfs count=\"3\"><xf/><xf numFmtId=\"164\" fontId=\"1\" fillId=\"2\" borderId=\"1\" applyNumberFormat=\"1\">"
                 "<alignment horizontal=\"center\" vertical=\"top\" wrapText=\"1\" textRotation=\"45\" indent=\"2\"/></xf>"
                 "<xf numFmtId=\"14\"/></cellXfs>"
                 "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
                 "<dxfs count=\"1\"><dxf><font><b/></font><fill><patternFill><bgColor rgb=\"FFFFC7CE\"/></patternFill></fill></dxf></dxfs>"
                 "</styleSheet>"},
                {"xl/sharedStrings.xml", strings}});
  return parts;
}

std::unique_ptr<Workbook> Open(const std::vector<std::pair<std::string, std::string>>& parts, bool loadAll = true) {
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(test::MakeZip(parts), st);
  CHECK(pkg != nullptr);
  if (!pkg) return nullptr;
  xlsx::ReadOptions options;
  options.loadAll = loadAll;
  auto book = xlsx::Read(std::move(pkg), st, options);
  if (!book) std::printf("    %s\n", st.message.c_str());
  CHECK(book != nullptr);
  return book;
}

// Each part the model writes equals the original after canonicalization.
void CheckRoundTrip(const Workbook& book, const std::vector<std::pair<std::string, std::string>>& parts) {
  for (const std::string& name : xlsx::ModelParts(book)) {
    std::string original;
    for (const auto& [n, xml] : parts)
      if ("/" + n == name) original = xml;
    std::string written, a, b;
    CHECK(xlsx::WritePart(book, name, written).Ok());
    CHECK(fw::pkg::CanonicalizeXml(original, a).Ok());
    CHECK(fw::pkg::CanonicalizeXml(written, b).Ok());
    if (a != b) std::printf("    %s differs:\n    %s\n    %s\n", name.c_str(), a.c_str(), b.c_str());
    CHECK(a == b);
  }
}

const std::string kStrings =
    "<sst" + Ns() + " count=\"4\" uniqueCount=\"4\"><si><t>Hello</t></si><si><t xml:space=\"preserve\"> two  spaces </t></si>"
    "<si><r><t>Bold</t></r><r><rPr><b/><sz val=\"11\"/><color rgb=\"FFFF0000\"/><rFont val=\"Calibri\"/></rPr><t xml:space=\"preserve\"> red</t></r></si>"
    "<si><t>Name</t><rPh sb=\"0\" eb=\"1\"><t>NAME</t></rPh><phoneticPr fontId=\"1\"/></si></sst>";

const std::string kSheet1 =
    "<worksheet" + Ns() + "><sheetPr><tabColor rgb=\"FF00B050\"/><outlinePr summaryBelow=\"0\"/></sheetPr>"
    "<dimension ref=\"A1:D6\"/>"
    "<sheetViews><sheetView tabSelected=\"1\" zoomScale=\"85\" workbookViewId=\"0\"><pane xSplit=\"1\" ySplit=\"2\" topLeftCell=\"B3\" "
    "activePane=\"bottomRight\" state=\"frozen\"/><selection pane=\"bottomRight\" activeCell=\"C4\" sqref=\"C4 D5:D6\"/></sheetView></sheetViews>"
    "<sheetFormatPr defaultRowHeight=\"15\" x14ac:dyDescent=\"0.25\"/>"
    "<cols><col min=\"1\" max=\"1\" width=\"20.7109375\" customWidth=\"1\"/><col min=\"3\" max=\"4\" width=\"5\" hidden=\"1\" style=\"2\"/></cols>"
    "<sheetData>"
    "<row r=\"1\" spans=\"1:4\" x14ac:dyDescent=\"0.25\"><c r=\"A1\" s=\"1\"><v>1.5</v></c><c r=\"B1\" t=\"s\"><v>0</v></c>"
    "<c r=\"C1\" t=\"b\"><v>1</v></c><c r=\"D1\" t=\"e\"><v>#DIV/0!</v></c></row>"
    "<row r=\"2\" spans=\"1:4\" ht=\"30\" customHeight=\"1\" x14ac:dyDescent=\"0.25\"><c r=\"A2\" t=\"n\"><v>0.1</v></c>"
    "<c r=\"B2\" t=\"inlineStr\"><is><t xml:space=\"preserve\">inline </t></is></c><c r=\"C2\" s=\"0\"/>"
    "<c r=\"D2\"><v>4.4999999999999998E-2</v></c></row>"
    "<row r=\"3\" hidden=\"1\" outlineLevel=\"1\"><c r=\"A3\"><f>A1*2</f><v>3</v></c><c r=\"B3\" t=\"str\"><f>B1&amp;\"!\"</f><v>Hello!</v></c></row>"
    "<!-- a comment between rows -->"
    "<row r=\"4\"><c r=\"A4\"><f t=\"shared\" ref=\"A4:A6\" si=\"0\">A1+1</f><v>2.5</v></c>"
    "<c r=\"B4\"><f t=\"array\" ref=\"B4:B5\">A1:A2*2</f><v>3</v></c></row>"
    "<row r=\"5\"><c r=\"A5\"><f t=\"shared\" si=\"0\"/><v>3</v></c><c r=\"B5\"><v>0.2</v></c></row>"
    "<row r=\"6\"><c r=\"A6\"><f t=\"shared\" si=\"0\"/></c><c r=\"C6\" t=\"s\"><v>3</v></c><c r=\"D6\" t=\"s\"><v>1</v></c></row>"
    "</sheetData>"
    "<sheetCalcPr fullCalcOnLoad=\"1\"/>"
    "<mergeCells count=\"1\"><mergeCell ref=\"C3:D4\"/></mergeCells>"
    "<conditionalFormatting sqref=\"A1:A6\"><cfRule type=\"cellIs\" dxfId=\"0\" priority=\"1\" operator=\"greaterThan\"><formula>2</formula></cfRule></conditionalFormatting>"
    "<hyperlinks><hyperlink ref=\"B1\" location=\"'Sheet 2'!A1\" display=\"Go\"/></hyperlinks>"
    "<printOptions gridLines=\"1\"/><pageMargins left=\"0.5\" right=\"0.5\" top=\"1\" bottom=\"1\" header=\"0.3\" footer=\"0.3\"/>"
    "<pageSetup paperSize=\"9\" orientation=\"landscape\" fitToHeight=\"0\"/>"
    "<headerFooter><oddHeader>&amp;CPage &amp;P</oddHeader></headerFooter>"
    "<rowBreaks count=\"1\" manualBreakCount=\"1\"><brk id=\"3\" max=\"16383\" man=\"1\"/></rowBreaks>"
    "</worksheet>";

const std::string kSheet2 = "<worksheet" + Ns() + "><sheetData><row r=\"2\"><c r=\"B2\"><v>42</v></c></row></sheetData></worksheet>";

}  // namespace

TEST(sheet_a1_names) {
  CellAddr a;
  CHECK(ParseA1("A1", a) && a.row == 0 && a.col == 0);
  CHECK(ParseA1("$XFD$1048576", a) && a.row == 1048575 && a.col == 16383);
  CHECK(!ParseA1("XFE1", a));
  CHECK(!ParseA1("A0", a));
  CHECK(!ParseA1("A1048577", a));
  CHECK(!ParseA1("1A", a));
  Range r;
  CHECK(ParseRange("B3:A1", r) && r == (Range{0, 0, 2, 1}));
  CHECK(ParseRange("C:D", r) && r.firstCol == 2 && r.lastCol == 3 && r.firstRow == 0 && r.lastRow == kMaxRows - 1);
  CHECK(ParseRange("2:3", r) && r.firstRow == 1 && r.lastRow == 2 && r.lastCol == kMaxCols - 1);
  CHECK(!ParseRange("A:1", r));
  CHECK_EQ(CellName(0, 0), "A1");
  CHECK_EQ(CellName(1048575, 16383), "XFD1048576");
  CHECK_EQ(ColumnName(25), "Z");
  CHECK_EQ(ColumnName(26), "AA");
  CHECK_EQ(RangeName(Range{1, 1, 2, 2}), "B2:C3");
  ErrorCode e;
  CHECK(ParseError("#N/A", e) && e == ErrorCode::NA);
  CHECK(!ParseError("#nope", e));
}

TEST(sheet_read_values) {
  auto parts = Package({kSheet1, kSheet2}, kStrings);
  auto book = Open(parts);
  if (!book) return;
  CHECK_EQ(book->SheetCount(), 2u);
  const Sheet& s = book->GetSheet(0);
  CHECK_EQ(s.Name(), "Sheet 1");
  CHECK(s.Loaded());
  CHECK_EQ(s.GetValue(0, 0).number, 1.5);
  CHECK(s.GetValue(0, 1).text == "Hello");
  CHECK(s.GetValue(0, 2).type == ValueType::Bool && s.GetValue(0, 2).boolean);
  CHECK(s.GetValue(0, 3).type == ValueType::Error && s.GetValue(0, 3).error == ErrorCode::Div0);
  CHECK_EQ(s.GetValue(1, 0).number, 0.1);
  CHECK(s.GetValue(1, 1).text == "inline ");
  CHECK(s.GetValue(1, 2).type == ValueType::Empty);
  CHECK_EQ(s.GetValue(1, 3).number, 0.045);
  CHECK(s.GetValue(2, 1).text == "Hello!");
  CHECK(s.GetValue(5, 2).text == "Name");
  CHECK(s.GetValue(5, 3).text == " two  spaces ");
  CHECK(s.GetValue(9, 9).type == ValueType::Empty);
  CellInfo c;
  CHECK(s.GetCell(1, 2, c) && c.style == 0 && c.value.type == ValueType::Empty);
  CHECK(!s.GetCell(1, 4, c));
  CHECK_EQ(s.CellCount(), 17u);
  CHECK(s.UsedRange() == (Range{0, 0, 5, 3}));
  CHECK(s.Dimension() == (Range{0, 0, 5, 3}));

  // Rich text.
  CHECK(s.GetCell(5, 3, c) && c.sharedString == 1);
  CHECK(book->Strings().Runs(1).empty());
  auto runs = book->Strings().Runs(2);
  CHECK(book->Strings().Text(2) == "Bold red");
  CHECK_EQ(runs.size(), 2u);
  if (runs.size() == 2) {
    CHECK(runs[0].start == 0 && runs[0].font == kNone);
    CHECK(runs[1].start == 4 && runs[1].font != kNone);
    CHECK(book->Strings().Fonts()[runs[1].font].bold);
  }

  // Cells in a range, in order.
  std::vector<std::string> seen;
  s.ForEachCell(Range{1, 1, 4, 2}, [&](const CellInfo& ci) {
    seen.push_back(CellName(ci.row, ci.col));
    return true;
  });
  CHECK((seen == std::vector<std::string>{"B2", "C2", "B3", "B4", "B5"}));
}

TEST(sheet_read_formulas) {
  auto book = Open(Package({kSheet1, kSheet2}, kStrings));
  if (!book) return;
  const Sheet& s = book->GetSheet(0);
  CHECK_EQ(s.FormulaCount(), 6u);
  FormulaInfo f;
  CHECK(s.GetFormula(2, 0, f) && f.text == "A1*2" && f.kind == FormulaKind::Normal);
  CHECK(s.GetFormula(2, 1, f) && f.text == "B1&\"!\"");
  CHECK(s.GetFormula(3, 0, f) && f.kind == FormulaKind::Shared && f.master && f.text == "A1+1" &&
        f.ref == (Range{3, 0, 5, 0}) && f.sharedIndex == 0);
  // Cells sharing it: the master's text, for the master's cell.
  CHECK(s.GetFormula(4, 0, f) && f.kind == FormulaKind::Shared && !f.master && f.text == "A1+1" &&
        f.origin == (CellAddr{3, 0}));
  CHECK(s.GetFormula(5, 0, f) && f.text == "A1+1");
  CellInfo c;
  CHECK(s.GetCell(5, 0, c) && c.hasFormula && c.value.type == ValueType::Empty);  // no cached value
  CHECK(s.GetFormula(3, 1, f) && f.kind == FormulaKind::Array && f.ref == (Range{3, 1, 4, 1}) && f.master);
  CHECK(!s.GetFormula(4, 1, f));  // inside the array, but no formula of its own
  CHECK(!s.GetFormula(0, 0, f));
  FormulaInfo g;
  CHECK(s.GetFormula(2, 0, g) && s.FormulaAttribute(g, "ref").empty());
  CHECK(s.GetFormula(3, 0, f) && s.FormulaAttribute(f, "ref") == "A4:A6");
}

TEST(sheet_read_layout) {
  auto book = Open(Package({kSheet1, kSheet2}, kStrings));
  if (!book) return;
  const Sheet& s = book->GetSheet(0);
  RowInfo r = s.Row(1);
  CHECK(r.stored && r.height == 30 && r.customHeight);
  r = s.Row(2);
  CHECK(r.hidden && r.outlineLevel == 1);
  CHECK(!s.Row(10).stored);
  CHECK_EQ(s.Format().defaultRowHeight, 15.0);
  CHECK_EQ(s.Column(0).width, 20.7109375);
  CHECK(s.Column(0).customWidth);
  CHECK_EQ(s.Column(1).width, 0.0);
  CHECK(s.Column(2).hidden && s.Column(3).hidden && s.Column(3).style == 2);
  CHECK_EQ(s.StyleAt(9, 2), 2u);  // the column's format
  CHECK_EQ(s.StyleAt(0, 0), 1u);
  CHECK(s.MergeAt(3, 3) != nullptr && *s.MergeAt(3, 3) == (Range{2, 2, 3, 3}));
  CHECK(s.MergeAt(4, 3) == nullptr);
  CHECK_EQ(s.Views().size(), 1u);
  if (!s.Views().empty()) {
    const SheetView& v = s.Views()[0];
    CHECK(v.paneState == PaneState::Frozen && v.xSplit == 1 && v.ySplit == 2);
    CHECK(v.paneTopLeft == (CellAddr{2, 1}));
    CHECK(v.zoomScale == 85 && v.tabSelected);
    CHECK(v.selections.size() == 1 && v.selections[0].ranges.size() == 2 && v.selections[0].active == (CellAddr{3, 2}));
  }
  CHECK(!s.Props().summaryBelow);
  CHECK(s.Props().tabColor.kind == Color::Kind::Rgb && s.Props().tabColor.value == 0xFF00B050);
  CHECK(s.Props().fullCalcOnLoad);
  CHECK(s.KeptXml().find("<sheetCalcPr fullCalcOnLoad=\"1\"/>") != std::string::npos);  // written as it was
  CHECK(s.Hyperlinks().size() == 1 && s.Hyperlinks()[0].location == "'Sheet 2'!A1");
  CHECK(s.Page().paperSize == 9 && s.Page().orientation == Orientation::Landscape && s.Page().fitToHeight == 0);
  CHECK(s.Page().gridLines && s.Page().margins.top == 1);
  CHECK(s.HeadersFooters().oddHeader == "&CPage &P");
  CHECK(s.RowBreaks().size() == 1 && s.RowBreaks()[0].id == 3 && s.RowBreaks()[0].manual);
  CHECK(s.KeptXml().find("conditionalFormatting") != std::string::npos);
  CHECK(s.KeptXml().find("sheetData") == std::string::npos);
}

TEST(sheet_read_workbook) {
  auto book = Open(Package({kSheet1, kSheet2}, kStrings, "<workbookPr date1904=\"1\"/><bookViews><workbookView activeTab=\"1\"/></bookViews>"),
                   false);
  if (!book) return;
  CHECK(book->Date1904());
  CHECK(book->Calc().fullCalcOnLoad && book->Calc().calcId == 191029);
  CHECK(book->Calc().calcFeatures == (std::vector<std::string>{"microsoft.com:RD", "microsoft.com:Single"}));
  CHECK(!book->GetSheet(1).Props().fullCalcOnLoad);
  CHECK_EQ(book->ActiveSheet(), 1u);
  // Only the active sheet is read at first; the others when asked.
  CHECK(!book->GetSheet(0).Loaded());
  CHECK(book->GetSheet(1).Loaded());
  CHECK_EQ(book->GetSheet(1).GetValue(1, 1).number, 42.0);
  CHECK(book->LoadSheet(0).Ok());
  CHECK(book->GetSheet(0).Loaded() && book->GetSheet(0).CellCount() == 17);
  CHECK_EQ(book->FindSheet("sheet 2"), 1);
  CHECK_EQ(book->FindSheet("Sheet 3"), -1);
  CHECK(book->FindName("TOTAL") && book->FindName("TOTAL")->formula == "'Sheet 1'!$A$1");
  CHECK(book->FindName("_xlnm.Print_Area", 0) != nullptr);
  CHECK(book->FindName("_xlnm.Print_Area", 1) == nullptr);
}

TEST(sheet_read_styles) {
  auto book = Open(Package({kSheet1}, kStrings));
  if (!book) return;
  const Styles& st = book->GetStyles();
  CHECK_EQ(st.CellXfs().size(), 3u);
  const Xf& xf = st.CellXf(1);
  CHECK(xf.numFmt == 164 && st.FormatCode(164) == "0.000" && st.FormatCode(14).empty());
  CHECK(xf.alignment.horizontal == HAlign::Center && xf.alignment.vertical == VAlign::Top && xf.alignment.wrap);
  CHECK(xf.alignment.rotation == 45 && xf.alignment.indent == 2);
  const Font& f = st.FontOf(xf);
  CHECK(f.bold && !f.italic && f.underline == Underline::Single && f.size == 14 && f.name == "Arial");
  CHECK(f.color.kind == Color::Kind::Theme && f.color.value == 4);
  CHECK_EQ(st.Resolve(f.color, 0), ApplyTint(0xFF4472C4, -0.5));
  const Fill& fill = st.FillOf(xf);
  CHECK(fill.pattern == PatternType::Solid && st.Resolve(fill.fg, 0) == 0xFFFF0000);
  const Border& b = st.BorderOf(xf);
  CHECK(b.left.style == BorderStyle::Thin && st.Resolve(b.left.color, 0) == 0xFF00FF00 && b.bottom.style == BorderStyle::Double);
  CHECK(st.CellXf(99).numFmt == 0);  // out of range: the first
  CHECK(st.CellStyles().size() == 1 && st.CellStyles()[0].name == "Normal");
  CHECK(st.Dxfs().size() == 1 && st.Dxfs()[0].fill && st.Dxfs()[0].fill->pattern == PatternType::Solid && st.Dxfs()[0].fontBold);
  CHECK_EQ(ApplyTint(0xFF808080, 0), 0xFF808080u);
  CHECK_EQ(ApplyTint(0xFF000000, 0.5), 0xFF808080u);
  CHECK_EQ(ApplyTint(0xFFFFFFFF, -0.5), 0xFF808080u);
  CHECK_EQ(st.Resolve(Color{Color::Kind::Indexed, 64, 0}, 0x12345678), 0x12345678u);
}

TEST(sheet_roundtrip_exact) {
  auto parts = Package({kSheet1, kSheet2}, kStrings);
  auto book = Open(parts);
  if (!book) return;
  CHECK_EQ(xlsx::ModelParts(*book).size(), 3u);
  CheckRoundTrip(*book, parts);
}

TEST(sheet_roundtrip_odd_content) {
  // What the model doesn't write itself is kept as written: unknown attributes and elements,
  // comments, values the model would write otherwise, cells without r, cells out of order.
  std::string sheet =
      "<?xml version=\"1.0\"?>\n<!-- before --><worksheet" + Ns() + " xmlns:u=\"urn:u\"><sheetData u:a=\"1\">\n  "
      "<row><c><v>1</v></c><c u:b=\"2\" cm=\"1\"><v>2</v></c><c r=\"e1\"><v>01</v></c><c r=\"F1\"><v>1E-3</v></c>"
      "<c r=\"G1\" t=\"b\"><v>true</v></c><c r=\"H1\" t=\"e\"><v>#WHAT?</v></c><c r=\"I1\" t=\"x\"><v>7</v></c>"
      "<c r=\"J1\" s=\"01\"/><c r=\"K1\"><v>1</v><f>1</f></c><c r=\"L1\"><v>1<!-- inside --></v></c>"
      "<c r=\"M1\" t=\"inlineStr\"><is><r><t>a</t></r><r><rPr><b/></rPr><t>b</t></r></is></c>"
      "<c r=\"N1\"> </c><c r=\"O1\" t=\"s\"><v>99</v></c><c r=\"P1\" t=\"d\"><v>2024-02-29T12:00:00</v></c>"
      "<c r=\"B1\"><v>5</v></c><u:x/>text</row>"
      "<row r=\"3\"> </row><row r=\"2\"><c r=\"A2\"><v>3</v></c></row><row r=\"07\" u:c=\"x\"/>"
      "<u:unknown><u:child/></u:unknown>"
      "</sheetData><extLst><ext uri=\"{X}\"><u:y/></ext></extLst></worksheet><!-- after -->";
  std::string strings = "<sst" + Ns() + "><!-- c --><si><t>a</t></si><u:z xmlns:u=\"urn:u\"/><si u:q=\"1\" xmlns:u=\"urn:u\"><t>b</t></si>"
                        "<si><t>c</t><t>d</t></si><si/></sst>";
  auto parts = Package({sheet}, strings);
  auto book = Open(parts);
  if (!book) return;
  const Sheet& s = book->GetSheet(0);
  CHECK_EQ(s.GetValue(0, 0).number, 1.0);
  CHECK_EQ(s.GetValue(0, 1).number, 2.0);
  CHECK_EQ(s.GetValue(0, 4).number, 1.0);
  CHECK_EQ(s.GetValue(0, 5).number, 0.001);
  CHECK(s.GetValue(0, 6).boolean);
  CHECK(s.GetValue(0, 7).type == ValueType::Error && s.GetValue(0, 7).error == ErrorCode::Unknown);
  CHECK_EQ(s.GetValue(0, 8).number, 7.0);
  CHECK(s.GetValue(0, 12).text == "ab");
  CHECK(s.GetValue(0, 14).type == ValueType::Empty);  // no such shared string
  CHECK_EQ(s.GetValue(0, 15).number, 45351.5);
  CHECK(s.GetValue(0, 1).number == 2.0 && s.GetValue(0, 1).type == ValueType::Number);
  CHECK(s.GetValue(1, 0).type == ValueType::Empty);  // row 2 came after row 3: kept, not read
  CHECK(s.Row(6).stored);
  CHECK(book->Strings().Text(2) == "cd");
  CHECK_EQ(book->Strings().Size(), 4u);
  CheckRoundTrip(*book, parts);
}

TEST(sheet_build_and_copy_on_write) {
  Workbook book;
  Sheet& s = book.AddSheet("Data");
  {
    SheetBuilder b(s);
    for (uint32_t r = 0; r < 100; ++r) {
      CHECK(b.StartRow(r * 2));
      CHECK(b.AddNumber(0, r));
      CHECK(b.AddString(3, "row " + std::to_string(r)));
      CHECK(!b.AddNumber(2, 1));  // out of order
    }
    CHECK(!b.StartRow(10));
  }
  CHECK_EQ(s.CellCount(), 200u);
  CHECK_EQ(s.GetValue(198, 0).number, 99.0);
  CHECK(s.GetValue(198, 3).text == "row 99");
  CHECK(s.Blocks().size() > 1);  // blocks of rows
  // A copy of the blocks (a snapshot) doesn't see later changes.
  auto snapshot = s.Blocks();
  s.SetValue(4, 0, Value::Number(7));
  s.SetValue(5, 1, Value::Text("new"));  // a new row and cell
  s.SetValue(0, 2, Value::Bool(true));   // a new cell inside a row
  CHECK_EQ(s.GetValue(4, 0).number, 7.0);
  CHECK(s.GetValue(5, 1).text == "new");
  CHECK(s.GetValue(0, 2).boolean);
  CHECK_EQ(s.GetValue(0, 3).text, std::string_view("row 0"));
  CHECK_EQ(s.CellCount(), 202u);
  CHECK_EQ(snapshot[0]->cells[4].v.number, 2.0);  // row 5, column A, as it was
  CHECK(snapshot[0].get() != s.Blocks()[0].get());
  CHECK(snapshot.back().get() == s.Blocks().back().get());  // untouched blocks are shared
  CellInfo c;
  CHECK(s.GetCell(4, 0, c) && c.computed);
}

namespace {

// tests/fuzz/seeds/xlsx/model-features-xlsx.txt: parts as text ("@name" lines), as fuzz_xlsx reads them.
std::vector<std::pair<std::string, std::string>> SeedParts() {
  const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "fuzz" / "seeds" / "xlsx" / "model-features-xlsx.txt";
  std::ifstream in(path, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<std::pair<std::string, std::string>> parts;
  size_t pos = 0;
  while (pos < text.size() && text[pos] == '@') {
    size_t eol = text.find('\n', pos);
    size_t next = text.find("\n@", eol);
    std::string name = text.substr(pos + 1, eol - pos - 1);
    if (!name.empty() && name.back() == '\r') name.pop_back();
    parts.push_back({name, text.substr(eol + 1, (next == std::string::npos ? text.size() : next) - eol - 1)});
    pos = next == std::string::npos ? text.size() : next + 1;
  }
  return parts;
}

}  // namespace

TEST(sheet_fuzz_seed) {
  auto parts = SeedParts();
  CHECK(parts.size() >= 8);
  auto book = Open(parts);
  if (!book) return;
  CHECK_EQ(book->SheetCount(), 2u);
  CHECK(book->GetSheet(1).State() == SheetState::Hidden);
  CHECK(book->Calc().mode == CalcMode::Manual && book->Calc().iterate && book->Calc().iterateCount == 10);
  const Sheet& s = book->GetSheet(0);
  FormulaInfo f;
  CHECK(s.GetFormula(3, 2, f) && f.kind == FormulaKind::DataTable && s.FormulaAttribute(f, "r1") == "A1");
  CHECK(s.Hyperlinks().size() == 2 && s.Hyperlinks()[1].target == "https://example.com/");
  CHECK(s.Columns().size() == 3 && s.Column(2).width == 9 && s.Column(3).width == 5);  // the later overlap gives way
  CHECK(s.ColBreaks().size() == 1 && s.Page().pageOrder == PageOrder::OverThenDown && s.HeadersFooters().differentFirst);
  CHECK(s.DrawingRelId() == "rId2" && s.LegacyDrawingRelId() == "rId3" && s.TableRelIds().size() == 1);
  CHECK(s.Views().size() == 1 && s.Views()[0].rightToLeft && !s.Views()[0].showGridLines);
  CHECK_EQ(s.GetValue(4, 3).number, 45351.5 - 1462);  // t="d" in the 1904 date system
  CHECK(book->GetSheet(1).GetValue(1, 2).text == "x");  // x: prefixed SpreadsheetML
  const Styles& st = book->GetStyles();
  CHECK(st.Palette().size() == 2);
  CHECK(st.CellXf(1).alignment.rotation == 255 && !st.CellXf(1).protection.locked && st.CellXf(1).quotePrefix);
  CHECK(st.FillOf(st.CellXf(2)).gradient && st.FillOf(st.CellXf(2)).stops.size() == 2);
  CHECK(st.BorderOf(st.CellXf(1)).left.style == BorderStyle::Thin && st.BorderOf(st.CellXf(1)).diagonalUp);
  CheckRoundTrip(*book, parts);
}

TEST(sheet_number_styles) {
  // How Excel 2021 wrote these (probed 2026-11-16), and how Python's repr and C's printf do: each
  // is written back exactly from its number and a style, without keeping its text.
  const char* texts[] = {"0.1", "4.4000000000000004", "8.1999999999999993", "0.33333333333333331", "0.5", "1.0000000000000001E-5",
                         "1.4999999999999999E-7", "1E+20", "1.23456789012345E+19", "123456789012345", "100000", "1000000000000000",
                         "1E+16", "-0.1", "0.30000000000000004", "3.1415899999999999", "2.5E-300", "1.7976931348623157E+308",
                         "45351.5", "12345.678", "1.2300000000000001E-4", "1E-3", "1.23E-2", "9.9900000000000006E-3", "0.01",
                         "1500000000000000", "1.1499999999999999", "100.1", "1e-05", "1e+16", "1.5e+20", "0.0001", "-2.5e-07",
                         "1.18059162071741E+21"};
  for (const char* t : texts) {
    std::string_view s(t);
    double v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    uint8_t style = 0xFF;
    bool found = xlsx::NumberStyle(s, v, style);
    if (!found) std::printf("    no style writes %s\n", t);
    CHECK(found);
    char buf[48];
    CHECK(std::string_view(buf, xlsx::FormatNumber(v, style, buf)) == s);
  }
  uint8_t style = 0;
  CHECK(!xlsx::NumberStyle("1.0", 1.0, style));
  CHECK(!xlsx::NumberStyle("01", 1.0, style));
  CHECK(!xlsx::NumberStyle("1E-03", 0.001, style));
  char buf[48];
  CHECK(std::string_view(buf, xlsx::FormatNumber(0, 0, buf)) == "0");
  CHECK(std::string_view(buf, xlsx::FormatNumber(-1234.5, 0, buf)) == "-1234.5");
  CHECK(std::string_view(buf, xlsx::FormatNumber(1e-300, 0, buf)) == "1e-300");
}

TEST(sheet_tables) {
  auto parts = Package({kSheet1, kSheet2}, kStrings);
  for (auto& [name, xml] : parts) {
    if (name == "[Content_Types].xml")
      xml.insert(xml.find("</Types>"), "<Override PartName=\"/xl/tables/table1.xml\" "
                                       "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.table+xml\"/>");
  }
  parts.push_back({"xl/worksheets/_rels/sheet2.xml.rels",
                   "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" "
                   "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/table\" Target=\"../tables/table1.xml\"/>"
                   "</Relationships>"});
  parts.push_back({"xl/tables/table1.xml",
                   "<table xmlns=\"" + std::string(kMain) + "\" id=\"3\" name=\"Table3\" displayName=\"Sales\" ref=\"B2:D7\" "
                   "totalsRowCount=\"1\"><autoFilter ref=\"B2:D6\"/><tableColumns count=\"3\"><tableColumn id=\"1\" name=\"Region\" "
                   "totalsRowLabel=\"Total\"/><tableColumn id=\"2\" name=\"Amount\" totalsRowFunction=\"sum\" dataDxfId=\"0\"/>"
                   "<tableColumn id=\"3\" name=\"Tax_x000a_rate\"><calculatedColumnFormula>Sales[[#This Row],[Amount]]*0.21</calculatedColumnFormula>"
                   "</tableColumn></tableColumns><tableStyleInfo name=\"TableStyleMedium2\" showFirstColumn=\"0\" showLastColumn=\"0\" "
                   "showRowStripes=\"1\" showColumnStripes=\"0\"/></table>"});
  auto book = Open(parts, false);
  if (!book) return;
  size_t sheet = 9;
  const Table* t = book->FindTable("SALES", &sheet);
  CHECK(t != nullptr && sheet == 1);
  if (!t) return;
  CHECK(t->id == 3 && t->name == "Table3" && t->ref == (Range{1, 1, 6, 3}) && t->headerRows == 1 && t->totalsRows == 1);
  CHECK(t->Data() == (Range{2, 1, 5, 3}));
  CHECK(t->autoFilter && t->columns.size() == 3 && t->columns[1].totalsFunction == "sum" && t->columns[1].dataDxf == 0);
  CHECK(t->columns[0].totalsLabel == "Total" && t->columns[2].calculatedFormula == "Sales[[#This Row],[Amount]]*0.21");
  CHECK(t->style.name == "TableStyleMedium2" && t->style.rowStripes && !t->style.columnStripes);
  CHECK(book->GetSheet(1).TableAt(6, 3) == t && book->GetSheet(1).TableAt(7, 3) == nullptr);
  CHECK(book->FindTable("Table3") == nullptr);
  CHECK(t->columns[2].name == "Tax\nrate");  // _x000a_: a line break in the header
  CHECK(t->FindColumn("TAX\nrate") == 2 && t->FindColumn("Amount") == 1 && t->FindColumn("Amount ") == -1);
  CHECK(t->All() == t->ref && t->Headers() == (Range{1, 1, 1, 3}) && t->Totals() == (Range{6, 1, 6, 3}));
}

namespace {

// What Excel shows in the cells of tests/sheet/table-styles.xlsx (tests/sheet/excel_table_styles.py).
struct TableCellCase {
  int sheet, row, col, fill, font, bold, side[8];  // side: style and color of left, top, bottom, right
};
constexpr TableCellCase kTableCells[] = {
#include "sheet_table_styles_table.inc"
};

// Excel's tints and ours may round apart by a step.
bool NearRgb(uint32_t argb, int rgb) {
  for (int shift = 0; shift < 24; shift += 8)
    if (std::abs(static_cast<int>((argb >> shift) & 0xFF) - ((rgb >> shift) & 0xFF)) > 2) return false;
  return true;
}

}  // namespace

TEST(sheet_table_styles) {
  const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "sheet" / "table-styles.xlsx";
  std::ifstream in(path, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(std::move(bytes), st);
  CHECK(pkg != nullptr);
  if (!pkg) return;
  auto book = xlsx::Read(std::move(pkg), st, xlsx::ReadOptions{true});
  CHECK(book != nullptr);
  if (!book) return;
  const Styles& styles = book->GetStyles();
  CHECK(styles.TableStyles().size() >= 3 && styles.DefaultTableStyle() == "TableStyleMedium2");
  CHECK(BuiltInTableStyle("TableStyleDark11") != nullptr && BuiltInTableStyle("TableStyleDark12") == nullptr);
  CHECK(BuiltInTableStyle("TableStyleLight01") == nullptr && BuiltInTableStyle("TableStyleMedium28") != nullptr);
  int failures = 0;
  for (const TableCellCase& c : kTableCells) {
    const TableCellFormat f = TableFormatAt(styles, book->GetSheet(c.sheet), c.row, c.col);
    const bool hasFill = f.fill.pattern == PatternType::Solid;
    bool ok = c.fill < 0 ? !hasFill : hasFill && NearRgb(styles.Resolve(f.fill.fg, 0), c.fill);
    ok = ok && NearRgb(f.fontColor.IsSet() ? styles.Resolve(f.fontColor, 0) : 0xFF000000u, c.font);
    ok = ok && f.bold == (c.bold != 0);
    const BorderSide* sides[] = {&f.left, &f.top, &f.bottom, &f.right};
    for (int i = 0; i < 4; ++i)
      ok = ok && static_cast<int>(sides[i]->style) == c.side[2 * i] &&
           (c.side[2 * i] == 0 || NearRgb(styles.Resolve(sides[i]->color, 0), c.side[2 * i + 1]));
    if (!ok && ++failures <= 20) {
      std::printf("    sheet %d %s: fill %06X font %06X bold %d L%d %06X T%d %06X B%d %06X R%d %06X\n", c.sheet,
                  CellName(c.row, c.col).c_str(), hasFill ? styles.Resolve(f.fill.fg, 0) & 0xFFFFFF : 0xFFFFFFFF,
                  f.fontColor.IsSet() ? styles.Resolve(f.fontColor, 0) & 0xFFFFFF : 0, f.bold ? 1 : 0,
                  static_cast<int>(f.left.style), styles.Resolve(f.left.color, 0) & 0xFFFFFF, static_cast<int>(f.top.style),
                  styles.Resolve(f.top.color, 0) & 0xFFFFFF, static_cast<int>(f.bottom.style),
                  styles.Resolve(f.bottom.color, 0) & 0xFFFFFF, static_cast<int>(f.right.style),
                  styles.Resolve(f.right.color, 0) & 0xFFFFFF);
    }
  }
  if (failures) std::printf("    %d of %zu cells differ from Excel\n", failures, std::size(kTableCells));
  CHECK_EQ(failures, 0);
}

// Excel writes #SPILL! and #CALC! as <v>#VALUE!</v> with vm=, its rich value saying which
// (tests/office/excel/formulas.xlsx, saved by Excel 2021); the text stays as written.
TEST(sheet_rich_value_errors) {
  const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "office" / "excel" / "formulas.xlsx";
  std::ifstream in(path, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(std::move(bytes), st);
  CHECK(pkg != nullptr);
  if (!pkg) return;
  auto book = xlsx::Read(std::move(pkg), st, xlsx::ReadOptions{true});
  CHECK(book != nullptr);
  if (!book) return;
  CHECK(book->ValueError(1) == ErrorCode::Spill && book->ValueError(2) == ErrorCode::Calc && !book->ValueError(3));
  const Sheet& s = book->GetSheet(6);
  const Value spill = s.GetValue(34, 1), calc = s.GetValue(50, 1);
  CHECK(spill.type == ValueType::Error && spill.error == ErrorCode::Spill);
  CHECK(calc.type == ValueType::Error && calc.error == ErrorCode::Calc);
  std::string out;
  CHECK(xlsx::WritePart(*book, s.PartName(), out).Ok());
  CHECK(out.find("<c r=\"B35\" t=\"e\" cm=\"1\" vm=\"1\">") != std::string::npos);
  CHECK(out.find("<v>#VALUE!</v>") != std::string::npos && out.find("#SPILL!") == std::string::npos);

  // Dynamic arrays: cm points at cell metadata of type XLDAPR with fDynamic="1"; an array formula
  // without it is Ctrl+Shift+Enter (sheet 4).
  CHECK(book->DynamicArray(1) && !book->DynamicArray(2) && !book->DynamicArray(0));
  FormulaInfo f;
  CHECK(s.GetFormula(1, 1, f) && f.kind == FormulaKind::Array && f.master && f.dynamic);
  CHECK(s.GetFormula(34, 1, f) && f.dynamic);
  CHECK(book->GetSheet(3).GetFormula(1, 1, f) && f.kind == FormulaKind::Array && !f.dynamic);
}
