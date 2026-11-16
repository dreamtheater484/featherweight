// Fuzz target: conditional formats (src/sheet/condfmt, S1). The input is a worksheet part; it's put
// in a workbook with a second sheet and differential formats, and every cell of its first rows and
// columns is evaluated with the formula engine and without, then again after an edit. Bars stay
// inside the cell, icons inside their set, and the same cell gives the same answer twice.
// Seeds: tests/fuzz/seeds/condfmt (tests/fuzz/make_condfmt_seeds.py).
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/Zip.h"
#include "sheet/calc/Bind.h"
#include "sheet/condfmt/CondFormat.h"
#include "sheet/numfmt/NumFormat.h"
#include "sheet/numfmt/NumLocale.h"
#include "sheet/xlsx/Xlsx.h"

using namespace fw::sheet;

namespace {

constexpr char kMain[] = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
constexpr char kRel[] = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";

std::vector<uint8_t> Package(std::string_view sheet) {
  const std::string types = std::string(
      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
      "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
      "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
      "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
      "<Override PartName=\"/xl/worksheets/sheet2.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
      "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/></Types>");
  const std::string rels = std::string(
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
      "<Relationship Id=\"rId1\" Type=\"") + kRel + "/officeDocument\" Target=\"xl/workbook.xml\"/></Relationships>";
  const std::string workbook = std::string("<workbook xmlns=\"") + kMain + "\" xmlns:r=\"" + kRel +
                               "\"><sheets><sheet name=\"Cases\" sheetId=\"1\" r:id=\"rId1\"/>"
                               "<sheet name=\"Other\" sheetId=\"2\" r:id=\"rId3\"/></sheets></workbook>";
  const std::string wbRels = std::string(
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
      "<Relationship Id=\"rId1\" Type=\"") + kRel + "/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
      "<Relationship Id=\"rId2\" Type=\"" + kRel + "/styles\" Target=\"styles.xml\"/>"
      "<Relationship Id=\"rId3\" Type=\"" + kRel + "/worksheet\" Target=\"worksheets/sheet2.xml\"/></Relationships>";
  const std::string styles = std::string("<styleSheet xmlns=\"") + kMain + "\"><dxfs count=\"4\">"
      "<dxf><font><color rgb=\"FF9C0006\"/></font><fill><patternFill><bgColor rgb=\"FFFFC7CE\"/></patternFill></fill></dxf>"
      "<dxf><font><b/><i/><u/><strike/><color theme=\"5\" tint=\"-0.25\"/></font><numFmt numFmtId=\"164\" formatCode=\"0.0%\"/></dxf>"
      "<dxf><fill><patternFill patternType=\"lightGrid\"><fgColor indexed=\"10\"/><bgColor rgb=\"FF00FF00\"/></patternFill></fill>"
      "<border><left style=\"thin\"/><top style=\"double\"><color auto=\"1\"/></top></border><numFmt numFmtId=\"14\"/></dxf>"
      "<dxf><fill><gradientFill degree=\"90\"><stop position=\"0\"><color rgb=\"FFFF0000\"/></stop>"
      "<stop position=\"1\"><color rgb=\"FF0000FF\"/></stop></gradientFill></fill></dxf></dxfs></styleSheet>";
  const std::string other = std::string("<worksheet xmlns=\"") + kMain + "\"><sheetData><row r=\"1\"><c r=\"A1\"><v>3</v></c>"
      "</row><row r=\"2\"><c r=\"A2\"><v>5</v></c></row></sheetData></worksheet>";
  fw::pkg::MemorySink sink;
  fw::pkg::ZipWriter w(sink);
  auto add = [&](const char* name, std::string_view data) {
    w.Add(name, std::vector<uint8_t>(data.begin(), data.end()), nullptr, fw::pkg::ZipWriter::Compression::Store);
  };
  add("[Content_Types].xml", types);
  add("_rels/.rels", rels);
  add("xl/workbook.xml", workbook);
  add("xl/_rels/workbook.xml.rels", wbRels);
  add("xl/styles.xml", styles);
  add("xl/worksheets/sheet1.xml", sheet);
  add("xl/worksheets/sheet2.xml", other);
  w.Finish({});
  return sink.bytes;
}

void Check(const condfmt::Style& s) {
  if (s.set & condfmt::Style::kBar) {
    const condfmt::DataBar& b = s.bar;
    if (!(b.from >= 0 && b.from <= b.to && b.to <= 1)) std::abort();
    if (b.axis != -1 && !(b.axis >= 0 && b.axis <= 1)) std::abort();
  }
  if ((s.set & condfmt::Style::kIcon) && (s.iconSet == condfmt::IconSet::None || s.icon >= condfmt::IconCount(s.iconSet)))
    std::abort();
  if ((s.set & condfmt::Style::kNumFmt) && !s.numFmt) std::abort();
  if ((s.set & condfmt::Style::kFill) && s.pattern == PatternType::None && !s.gradient) std::abort();
}

void Run(Workbook& book, calc::WorkbookCalc* calc, const NumLocale& locale) {
  condfmt::Options o;
  o.locale = &locale;
  o.calc = calc;
  o.today = 46342;
  condfmt::SheetRules rules(book, 0, o);
  // The first rows and columns, and the cells the sheet stores (the rules' ranges are around them).
  std::vector<CellAddr> cells;
  for (uint32_t r = 0; r < 16; ++r)
    for (uint32_t c = 0; c < 8; ++c) cells.push_back({r, c});
  book.GetSheet(0).ForEachCell(Range::All(), [&](const CellInfo& ci) {
    cells.push_back({ci.row, ci.col});
    return cells.size() < 1024;
  });
  condfmt::Style a, b;
  for (int pass = 0; pass < 2; ++pass) {
    for (const CellAddr& at : cells) {
      const bool any = rules.At(at.row, at.col, a);
      Check(a);
      if (any != (a.set != 0)) std::abort();
      // The same cell twice gives the same answer.
      rules.At(at.row, at.col, b);
      if (a.set != b.set || a.fillColor != b.fillColor || a.icon != b.icon || a.bar.to != b.bar.to) std::abort();
      if (a.set & condfmt::Style::kNumFmt) {
        FormatContext ctx;
        ctx.locale = &locale;
        Formatted out;
        a.numFmt->Format(1234.5, ctx, out);
      }
    }
    rules.At(kMaxRows - 1, kMaxCols - 1, a);
    Check(a);
    // An edit, then the statistics again.
    book.GetSheet(0).SetValue(2, 1, Value::Number(pass ? -7.5 : 1e300));
    rules.Changed();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > 65536) return 0;
  fw::pkg::Status st;
  fw::pkg::Limits limits;
  auto pkg = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(Package(std::string_view(reinterpret_cast<const char*>(data), size))),
                                    limits, st);
  if (!pkg) return 0;
  xlsx::ReadOptions ro;
  ro.loadAll = true;
  auto book = xlsx::Read(std::move(pkg), st, ro);
  if (!book || book->SheetCount() < 1) return 0;
  Run(*book, nullptr, EnUsLocale());
  calc::WorkbookCalc calc(*book, &EnNlLocale());
  calc.Load();
  Run(*book, &calc, EnNlLocale());
  return 0;
}
