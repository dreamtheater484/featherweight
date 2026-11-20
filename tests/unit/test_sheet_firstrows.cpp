// A large workbook's first screen (S4): the active sheet read from the start of its part only, cut
// after the last whole row (xlsx::ReadOptions::firstBytes), then the whole sheet read again and
// swapped in (Workbook::SwapSheet) must be exactly what reading it whole gives.
#include "TestHarness.h"

#include <string>

#include "SheetTestUtil.h"
#include "sheet/model/Workbook.h"
#include "sheet/xlsx/Xlsx.h"

using namespace fw;
using namespace fw::sheet;

namespace {

constexpr uint32_t kRows = 3000;

// Rows of a number, a text and a formula; merged cells and a margin after sheetData, which a cut
// leaves out. `p` is the element prefix ("" or "x:").
std::string SheetXml(const std::string& p) {
  std::string x = "<" + p + "dimension ref=\"A1:C" + std::to_string(kRows) + "\"/><" + p + "cols><" + p +
                  "col min=\"2\" max=\"2\" width=\"20\" customWidth=\"1\"/></" + p + "cols><" + p + "sheetData>";
  for (uint32_t r = 1; r <= kRows; ++r) {
    const std::string n = std::to_string(r);
    x += "<" + p + "row r=\"" + n + "\"><" + p + "c r=\"A" + n + "\"><" + p + "v>" + n + "</" + p + "v></" + p + "c><" +
         p + "c r=\"B" + n + "\" t=\"inlineStr\"><" + p + "is><" + p + "t>row " + n + "</" + p + "t></" + p + "is></" + p +
         "c><" + p + "c r=\"C" + n + "\"><" + p + "f>A" + n + "*2</" + p + "f><" + p + "v>" + std::to_string(2 * r) + "</" +
         p + "v></" + p + "c></" + p + "row>";
  }
  x += "</" + p + "sheetData><" + p + "mergeCells count=\"1\"><" + p + "mergeCell ref=\"E1:F2\"/></" + p + "mergeCells><" +
       p + "pageMargins left=\"0.5\" right=\"0.5\" top=\"1\" bottom=\"1\" header=\"0.3\" footer=\"0.3\"/>";
  return x;
}

std::vector<uint8_t> Zip(const std::string& prefix, pkg::ZipWriter::Compression c) {
  auto parts = sheet_test::WorkbookParts({"", "<sheetData/>"});
  for (auto& [name, data] : parts)
    if (name == "xl/worksheets/sheet1.xml")
      data = prefix.empty() ? "<worksheet xmlns=\"" + std::string(sheet_test::kMain) + "\">" + SheetXml("") + "</worksheet>"
                            : "<?xml version=\"1.0\"?><x:worksheet xmlns:x=\"" + std::string(sheet_test::kMain) + "\">" +
                                  SheetXml("x:") + "</x:worksheet>";
  return test::MakeZip(parts, c);
}

std::unique_ptr<Workbook> ReadFirst(const std::vector<uint8_t>& zip, size_t bytes, bool& partial) {
  pkg::Status st;
  auto pkg = test::OpenBytes(zip, st);
  if (!pkg) return nullptr;
  xlsx::ReadOptions o;
  o.firstBytes = bytes;
  o.partial = &partial;
  return xlsx::Read(std::move(pkg), st, o);
}

std::unique_ptr<Workbook> ReadWhole(const std::vector<uint8_t>& zip) {
  pkg::Status st;
  auto pkg = test::OpenBytes(zip, st);
  return pkg ? xlsx::Read(std::move(pkg), st) : nullptr;
}

void Same(const Sheet& a, const Sheet& b) {
  CHECK_EQ(a.CellCount(), b.CellCount());
  CHECK_EQ(a.Merges().size(), b.Merges().size());
  CHECK(a.Dimension().has_value() == b.Dimension().has_value());
  for (uint32_t r = 0; r < kRows; r += 7) {
    CHECK_EQ(sheet_test::Text(a, r, 0), sheet_test::Text(b, r, 0));
    CHECK_EQ(sheet_test::Text(a, r, 1), sheet_test::Text(b, r, 1));
    CHECK_EQ(sheet_test::FormulaText(a, r, 2), sheet_test::FormulaText(b, r, 2));
  }
  CHECK_EQ(sheet_test::Text(a, kRows - 1, 1), "row " + std::to_string(kRows));
}

void Check(const std::string& prefix, pkg::ZipWriter::Compression c) {
  const std::vector<uint8_t> zip = Zip(prefix, c);
  bool partial = false;
  auto first = ReadFirst(zip, 16 * 1024, partial);
  CHECK(first != nullptr);
  if (!first) return;
  CHECK(partial);
  const Sheet& s = first->GetSheet(0);
  // Whole rows only, from the first: some, not all, and nothing after sheetData yet.
  CHECK(s.CellCount() > 30 && s.CellCount() < 3 * kRows);
  CHECK_EQ(s.CellCount() % 3, 0u);
  CHECK_EQ(sheet_test::Text(s, 0, 1), "row 1");
  CHECK_EQ(sheet_test::FormulaText(s, 0, 2), "A1*2");
  CHECK(s.Merges().empty());
  // The whole sheet read again takes its place: the same as reading it whole.
  auto again = ReadWhole(zip);
  auto whole = ReadWhole(zip);
  CHECK(again && whole);
  if (!again || !whole) return;
  first->SwapSheet(0, *again);
  Same(first->GetSheet(0), whole->GetSheet(0));
  CHECK_EQ(first->GetSheet(0).Merges().size(), 1u);
  CHECK_EQ(first->GetSheet(0).CellCount(), static_cast<size_t>(3 * kRows));
  // A sheet that fits in the bytes asked for is read whole, and isn't partial.
  partial = true;
  auto fits = ReadFirst(zip, 64u << 20, partial);
  CHECK(fits != nullptr);
  CHECK(!partial);
  if (fits) Same(fits->GetSheet(0), whole->GetSheet(0));
}

}  // namespace

TEST(sheet_first_rows_deflated) { Check("", pkg::ZipWriter::Compression::Deflate); }

TEST(sheet_first_rows_stored) { Check("", pkg::ZipWriter::Compression::Store); }

TEST(sheet_first_rows_prefixed) { Check("x:", pkg::ZipWriter::Compression::Deflate); }

// A comment in the rows read is never cut into: such a sheet is read whole.
TEST(sheet_first_rows_comment) {
  auto parts = sheet_test::WorkbookParts({"", "<sheetData/>"});
  std::string rows = "<sheetData><!-- </row> -->";
  for (uint32_t r = 1; r <= kRows; ++r)
    rows += "<row r=\"" + std::to_string(r) + "\"><c r=\"A" + std::to_string(r) + "\"><v>" + std::to_string(r) + "</v></c></row>";
  for (auto& [name, data] : parts)
    if (name == "xl/worksheets/sheet1.xml")
      data = "<worksheet xmlns=\"" + std::string(sheet_test::kMain) + "\">" + rows + "</sheetData></worksheet>";
  bool partial = true;
  auto book = ReadFirst(test::MakeZip(parts), 4096, partial);
  CHECK(book != nullptr);
  CHECK(!partial);
  if (book) CHECK_EQ(book->GetSheet(0).CellCount(), static_cast<size_t>(kRows));
}
