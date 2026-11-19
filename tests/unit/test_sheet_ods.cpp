// .ods import (S4, src/sheet/ods): OpenFormula translated to Excel's formula text, and a small
// spreadsheet converted and read by the workbook model.
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "TestHarness.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/Zip.h"
#include "sheet/ods/Ods.h"
#include "sheet/xlsx/Xlsx.h"

using namespace fw::sheet;

namespace {

std::string Tr(std::string_view f, int rows = 0, int cols = 0) {
  std::string out;
  return ods::TranslateFormula(f, out, rows, cols) ? out : "(refused)";
}

}  // namespace

TEST(sheet_ods_formulas) {
  CHECK_EQ(Tr("of:=SUM([.A1:.B2];[Sheet2.C3])"), std::string("SUM(A1:B2,Sheet2!C3)"));
  CHECK_EQ(Tr("of:=[$'My Sheet'.$A$1]*2"), std::string("'My Sheet'!$A$1*2"));
  CHECK_EQ(Tr("of:=['it''s'.A1]"), std::string("'it''s'!A1"));
  CHECK_EQ(Tr("of:=SUM({1;2|3;4})"), std::string("SUM({1,2;3,4})"));
  CHECK_EQ(Tr("of:=IF([.A1]=\"a;b[.c]\";1;0)"), std::string("IF(A1=\"a;b[.c]\",1,0)"));
  CHECK_EQ(Tr("of:=COM.MICROSOFT.XLOOKUP([.A1];[.B1:.B3];[.C1:.C3])"), std::string("_xlfn.XLOOKUP(A1,B1:B3,C1:C3)"));
  CHECK_EQ(Tr("of:=COM.MICROSOFT.SINGLE($$Global4)"), std::string("_xlfn.SINGLE(Global4)"));
  CHECK_EQ(Tr("of:=LEGACY.NORMSDIST(1)"), std::string("NORMSDIST(1)"));
  CHECK_EQ(Tr("of:=SUM([.A:.A])+SUM([.$2:.$3])"), std::string("SUM(A:A)+SUM($2:$3)"));
  CHECK_EQ(Tr("of:=SUM([Sheet1.A1:Sheet3.A1])"), std::string("SUM(Sheet1:Sheet3!A1)"));
  CHECK_EQ(Tr("of:=[.#REF!]"), std::string("#REF!"));
  CHECK_EQ(Tr("of:=[.A1]/0=#DIV/0!"), std::string("A1/0=#DIV/0!"));
  CHECK_EQ(Tr("msoxl:=SUM(A1:A3)"), std::string("SUM(A1:A3)"));
  CHECK_EQ(Tr("of:=['file:///C:/x.ods'#$Sheet1.A1]"), std::string("#REF!"));  // another file: not followed
  // A formula repeated over cells: its relative references move.
  CHECK_EQ(Tr("of:=[.A1]+[.$B$2]+[.C$3]+[.$D4]", 2, 1), std::string("B3+$B$2+D$3+$D6"));
  CHECK_EQ(Tr("of:=[.A1]", -1, 0), std::string("#REF!"));
  CHECK_EQ(Tr("of:=SUM([.A1"), std::string("(refused)"));
  CHECK_EQ(Tr("of:=\"open"), std::string("(refused)"));
  // Sheets renamed for Excel, and sheets the workbook doesn't have.
  ods::SheetNames names = [](std::string_view n) { return n == "Long" ? std::string("Short") : std::string(); };
  std::string out;
  CHECK(ods::TranslateFormula("of:=[Long.A1]+[Gone.A1]", out, 0, 0, &names) && out == "Short!A1+#REF!");
  CHECK(ods::TranslateAddress("$Sheet1.$A$1:.$B$3", out) && out == "Sheet1!$A$1:$B$3");
  CHECK(!ods::TranslateAddress("not an address", out));
}

TEST(sheet_ods_converts_a_spreadsheet) {
  const std::string content =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
      "xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" "
      "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" "
      "xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" "
      "xmlns:number=\"urn:oasis:names:tc:opendocument:xmlns:datastyle:1.0\" "
      "xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" "
      "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><office:automatic-styles>"
      "<style:style style:name=\"cf1\" style:family=\"table-cell\"><style:text-properties fo:color=\"#FF0000\"/></style:style>"
      "<style:style style:name=\"ce1\" style:family=\"table-cell\"><style:map style:condition=\"of:cell-content()&gt;2\" "
      "style:apply-style-name=\"cf1\" style:base-cell-address=\"Data.B1\"/></style:style>"
      "<number:date-style style:name=\"N1\"><number:era number:calendar=\"gengou\" number:style=\"long\"/>"
      "<number:year number:calendar=\"gengou\"/><number:text>/</number:text><number:month/></number:date-style>"
      "<style:style style:name=\"ce2\" style:family=\"table-cell\" style:data-style-name=\"N1\"/>"
      "<style:style style:name=\"ta1\" style:family=\"table\" style:master-page-name=\"mp1\"/>"
      "<style:page-layout style:name=\"pm1\"><style:page-layout-properties fo:page-width=\"29.7cm\" fo:page-height=\"21cm\" "
      "style:print-orientation=\"landscape\" style:scale-to-X=\"1\" style:table-centering=\"horizontal\"/></style:page-layout>"
      "</office:automatic-styles><office:master-styles><style:master-page style:name=\"mp1\" style:page-layout-name=\"pm1\">"
      "<style:footer><text:p>Page <text:page-number>1</text:page-number> of <text:page-count>2</text:page-count> &amp; more"
      "</text:p></style:footer></style:master-page></office:master-styles><office:body><office:spreadsheet>"
      "<table:table table:name=\"Data\" table:style-name=\"ta1\"><table:table-column table:number-columns-repeated=\"3\"/>"
      "<table:table-row><table:table-cell office:value-type=\"string\"><text:p>Total_x0041_</text:p></table:table-cell>"
      "<table:table-cell table:style-name=\"ce1\" office:value-type=\"float\" office:value=\"2.5\"><text:p>2.5</text:p>"
      "</table:table-cell>"
      "<table:table-cell table:style-name=\"ce2\" office:value-type=\"date\" office:date-value=\"2000-01-01T12:00:00\">"
      "<text:p>x</text:p></table:table-cell>"
      "</table:table-row>"
      "<table:table-row table:number-rows-repeated=\"2\"><table:table-cell office:value-type=\"float\" office:value=\"5\" "
      "table:formula=\"of:=[.B1]*2\"><text:p>5</text:p></table:table-cell>"
      "<table:table-cell table:number-columns-spanned=\"2\" office:value-type=\"boolean\" office:boolean-value=\"true\">"
      "<text:p>TRUE</text:p></table:table-cell><table:covered-table-cell/></table:table-row>"
      "<table:table-row><table:table-cell><office:annotation office:display=\"true\"><dc:creator>Roy</dc:creator>"
      "<text:p>Check</text:p><text:p>this</text:p></office:annotation></table:table-cell>"
      "<table:table-cell office:value-type=\"string\"><text:p><text:ruby><text:ruby-base>Tokyo</text:ruby-base>"
      "<text:ruby-text>kana</text:ruby-text></text:ruby><text:tab/>x</text:p></table:table-cell></table:table-row>"
      "<table:table-row table:number-rows-repeated=\"1048572\"><table:table-cell table:number-columns-repeated=\"16384\"/>"
      "</table:table-row></table:table></office:spreadsheet></office:body></office:document-content>";
  fw::pkg::MemorySink sink;
  fw::pkg::ZipWriter zip(sink);
  const std::string mime = "application/vnd.oasis.opendocument.spreadsheet";
  zip.Add("mimetype", std::span(reinterpret_cast<const uint8_t*>(mime.data()), mime.size()), nullptr,
          fw::pkg::ZipWriter::Compression::Store);
  zip.Add("content.xml", std::span(reinterpret_cast<const uint8_t*>(content.data()), content.size()));
  zip.Finish();

  std::vector<uint8_t> xlsx;
  CHECK(ods::OdsToXlsx(sink.bytes, xlsx).Ok());
  fw::pkg::Status st;
  auto pkg = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(std::move(xlsx)), fw::pkg::Limits::Default(), st);
  CHECK(pkg != nullptr);
  if (!pkg) return;
  auto book = xlsx::Read(std::move(pkg), st, {.loadAll = true});
  CHECK(book != nullptr);
  if (!book) return;
  CHECK(book->SheetCount() == 1 && book->GetSheet(0).Name() == "Data");
  const Sheet& s = book->GetSheet(0);
  CHECK(s.GetValue(0, 0).type == ValueType::String && s.GetValue(0, 0).text == "Total_x0041_");  // not "TotalA" (ST_Xstring)
  CHECK(s.GetValue(0, 1).number == 2.5);
  CHECK_NEAR(s.GetValue(0, 2).number, 36526.5, 1e-9);  // 2000-01-01 noon
  FormulaInfo f;
  CHECK(s.GetFormula(2, 0, f) && f.text == "B2*2");  // the second repeat: B1 moved one row
  CHECK(s.GetValue(1, 1).type == ValueType::Bool && s.GetValue(1, 1).boolean);
  CHECK(s.Merges().size() == 2);
  CHECK(s.GetValue(5, 0).type == ValueType::Empty);
  CHECK(s.GetValue(3, 1).text == "Tokyo x");  // phonetic guide dropped, tab read as a space (as Excel does)
  // The condition on B1's style, and the note on A4.
  auto part = [&](const char* name) {
    std::vector<uint8_t> bytes;
    book->GetPackage()->ReadPart(name, bytes);
    return std::string(bytes.begin(), bytes.end());
  };
  const std::string sheet = part("/xl/worksheets/sheet1.xml"), styles = part("/xl/styles.xml");
  CHECK(sheet.find("<conditionalFormatting sqref=\"B1\"><cfRule type=\"cellIs\" operator=\"greaterThan\" dxfId=\"0\"") !=
        std::string::npos);
  CHECK(sheet.find("<formula>2</formula>") != std::string::npos);
  // The page setup from the table's master page.
  CHECK(sheet.find("<sheetPr><pageSetUpPr fitToPage=\"1\"/></sheetPr>") != std::string::npos);
  CHECK(sheet.find("<printOptions horizontalCentered=\"1\"/>") != std::string::npos);
  CHECK(sheet.find("<pageSetup paperSize=\"9\" fitToWidth=\"1\" fitToHeight=\"0\" orientation=\"landscape\"/>") !=
        std::string::npos);
  CHECK(sheet.find("<oddFooter>&amp;CPage &amp;P of &amp;N &amp;&amp; more</oddFooter>") != std::string::npos);
  CHECK(styles.find("formatCode=\"[$-411]ggge/m\"") != std::string::npos);  // Japan's eras
  CHECK(styles.find("<dxfs count=\"1\"><dxf><font><color rgb=\"FFFF0000\"/></font></dxf></dxfs>") != std::string::npos);
  const std::string comments = part("/xl/comments1.xml");
  CHECK(comments.find("ref=\"A4\"") != std::string::npos && comments.find("Check") != std::string::npos &&
        comments.find("this") != std::string::npos);
  CHECK(sheet.find("<legacyDrawing r:id=\"rIdVml\"/>") != std::string::npos);
  CHECK(!part("/xl/drawings/vmlDrawing1.vml").empty());
}
