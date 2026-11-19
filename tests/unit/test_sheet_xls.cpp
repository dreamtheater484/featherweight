// The .xls (BIFF8) reader (src/sheet/xls, S4): Excel 2021's .xls copies of workbooks of
// tests/office/excel (tests/fuzz/seeds/xls) converted into .xlsx packages and read into the model,
// and BIFF8 formula tokens decompiled into formula text.
#include "TestHarness.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "pkg/Check.h"
#include "pkg/CompoundFile.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "sheet/model/Workbook.h"
#include "sheet/xls/Internal.h"
#include "sheet/xls/Xls.h"
#include "sheet/xlsx/Xlsx.h"

using namespace fw;
using namespace fw::sheet;

namespace {

std::vector<uint8_t> SeedBytes(const char* name) {
  const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "fuzz" / "seeds" / "xls" / name;
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The seed converted and read with every sheet; null (with a failed check) when it doesn't.
std::unique_ptr<Workbook> ReadSeed(const char* name, std::wstring_view password = {}, pkg::Status* status = nullptr) {
  const std::vector<uint8_t> bytes = SeedBytes(name);
  auto cf = pkg::CompoundFile::Open(bytes);
  CHECK(cf != nullptr);
  if (!cf) return nullptr;
  const xls::StreamFn streams = [&cf](std::string_view n) { return cf->Stream(n); };
  CHECK(xls::IsWorkbook(streams));
  pkg::Status st;
  std::vector<uint8_t> xlsx = xls::ToXlsx(streams, st, password);
  if (status) *status = st;
  if (xlsx.empty()) return nullptr;
  auto package = pkg::Package::Open(pkg::Snapshot::FromBytes(std::move(xlsx)), pkg::Limits::Default(), st);
  CHECK(package != nullptr);
  if (!package) return nullptr;
  CHECK(pkg::CheckPackage(*package).Ok());
  auto book = xlsx::Read(std::move(package), st, {.loadAll = true});
  CHECK(book != nullptr);
  return book;
}

std::string Formula(const Sheet& s, uint32_t row, uint32_t col) {
  FormulaInfo f;
  return s.GetFormula(row, col, f) ? std::string(f.text) : std::string();
}

std::string Decompile(std::vector<uint8_t> rgce, uint32_t row = 0, uint32_t col = 0) {
  xls::detail::FormulaContext ctx;
  ctx.sheets = {"Data", "My Sheet", ""};
  ctx.supBooks.resize(1);
  ctx.supBooks[0].kind = xls::detail::FormulaContext::SupBook::Self;
  ctx.xti = {{0, 0, 0}, {0, 1, 1}, {0, 2, 2}, {0, 0, 1}};
  ctx.names = {"Total"};
  std::string out;
  return xls::detail::Decompile(ctx, rgce, {}, row, col, out) ? out : std::string("<failed>");
}

}  // namespace

TEST(xls_seeds_read) {
  for (const char* name : {"text.xls", "dates-1904.xls", "borders-fills.xls", "notes.xls", "rows-columns.xls", "sheets.xls",
                           "formulas.xls", "functions.xls", "merged-frozen.xls", "print-settings.xls"}) {
    auto book = ReadSeed(name);
    CHECK(book && book->SheetCount() >= 1);
  }
}

TEST(xls_workbook_and_sheets) {
  auto book = ReadSeed("sheets.xls");
  if (!book) return;
  CHECK(book->SheetCount() >= 5);
  CHECK(book->GetSheet(0).Name() == "First" && book->GetSheet(2).Name() == "Active, zoomed");
  CHECK(book->GetSheet(1).State() == SheetState::Hidden && book->GetSheet(3).State() == SheetState::VeryHidden);
  CHECK(!book->GetSheet(4).Views().empty() && book->GetSheet(4).Views()[0].rightToLeft);
  auto dates = ReadSeed("dates-1904.xls");
  CHECK(dates && dates->Date1904());
}

TEST(xls_formulas_and_notes) {
  auto book = ReadSeed("formulas.xls");
  if (!book) return;
  const Sheet& s = book->GetSheet(0);
  CHECK(s.FormulaCount() > 5);
  CHECK_EQ(Formula(s, 1, 1), std::string("1+2*3"));
  CHECK_EQ(Formula(s, 2, 1), std::string("(1+2)*3"));
  CHECK_EQ(Formula(s, 3, 1), std::string("-2^2"));
  CHECK_EQ(s.GetValue(1, 1).number, 7.0);
  auto notes = ReadSeed("notes.xls");
  CHECK(notes && !notes->GetSheet(0).LegacyDrawingRelId().empty());
  auto merged = ReadSeed("merged-frozen.xls");
  CHECK(merged && merged->GetSheet(0).Merges().size() >= 3 && merged->GetSheet(0).MergeAt(1, 2) != nullptr);
}

TEST(xls_password) {
  const std::vector<uint8_t> bytes = SeedBytes("text-password-feather1.xls");
  auto cf = pkg::CompoundFile::Open(bytes);
  CHECK(cf != nullptr);
  if (!cf) return;
  const xls::StreamFn streams = [&cf](std::string_view n) { return cf->Stream(n); };
  CHECK(xls::NeedsPassword(streams));
  pkg::Status st;
  CHECK(xls::ToXlsx(streams, st).empty() && st.code == pkg::Status::Code::Rejected);
  CHECK(xls::ToXlsx(streams, st, L"wrong").empty() && st.code == pkg::Status::Code::Rejected);
  auto book = ReadSeed("text-password-feather1.xls", L"Feather1", &st);
  CHECK(book && st.Ok());
  auto plain = ReadSeed("text.xls");
  if (book && plain) CHECK(book->GetSheet(0).CellCount() == plain->GetSheet(0).CellCount());
  const std::vector<uint8_t> plainBytes = SeedBytes("text.xls");
  auto plainFile = pkg::CompoundFile::Open(plainBytes);
  CHECK(plainFile && !xls::NeedsPassword([&](std::string_view n) { return plainFile->Stream(n); }));
}

TEST(xls_decompile) {
  // SUM(A1:B2)+1: PtgArea, PtgAttrSum, PtgInt, PtgAdd.
  CHECK_EQ(Decompile({0x25, 0, 0, 1, 0, 0, 0xC0, 1, 0xC0, 0x19, 0x10, 0, 0, 0x1E, 1, 0, 0x03}), std::string("SUM(A1:B2)+1"));
  // Absolute and whole columns: $A:$B (rows 0-65535).
  CHECK_EQ(Decompile({0x25, 0, 0, 0xFF, 0xFF, 0, 0, 1, 0}), std::string("$A:$B"));
  // A 3-D reference to a sheet with a space, and a range of sheets.
  CHECK_EQ(Decompile({0x3A, 1, 0, 4, 0, 2, 0xC0}), std::string("'My Sheet'!C5"));
  CHECK_EQ(Decompile({0x3A, 3, 0, 0, 0, 0, 0}), std::string("'Data:My Sheet'!$A$1"));
  // A sheet left out (a chart sheet) is #REF!.
  CHECK_EQ(Decompile({0x3A, 2, 0, 0, 0, 0, 0}), std::string("#REF!"));
  // Name:Sheet!A1 quotes the sheet.
  CHECK_EQ(Decompile({0x23, 1, 0, 0, 0, 0x3A, 0, 0, 0, 0, 0, 0, 0x11}), std::string("Total:'Data'!$A$1"));
  // PtgRefN in a shared formula: one row up and one column left of C3.
  CHECK_EQ(Decompile({0x2C, 0xFF, 0xFF, 0xFF, 0xC0}, 2, 2), std::string("B2"));
  // IF(A1>0,"yes","no") with a string, tAttrIf and tAttrSkip, PtgFuncVar.
  CHECK_EQ(Decompile({0x24, 0, 0, 0, 0xC0, 0x1E, 0, 0, 0x0D, 0x19, 0x02, 0, 0, 0x17, 3, 0, 'y', 'e', 's', 0x19, 0x08, 0, 0,
                      0x17, 2, 0, 'n', 'o', 0x19, 0x08, 0, 0, 0x42, 3, 1, 0}),
           std::string("IF(A1>0,\"yes\",\"no\")"));
  // An unknown token and a truncated one fail.
  CHECK_EQ(Decompile({0x00}), std::string("<failed>"));
  CHECK_EQ(Decompile({0x24, 0}), std::string("<failed>"));
}
