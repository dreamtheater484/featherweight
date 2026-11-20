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
                           "formulas.xls", "functions.xls", "merged-frozen.xls", "print-settings.xls", "drawings.xls"}) {
    auto book = ReadSeed(name);
    CHECK(book && book->SheetCount() >= 1);
  }
}

TEST(xls_drawings_from_excel) {
  // Excel 2021's .xls with a picture, a text box, and a rectangle and an oval in a group.
  auto book = ReadSeed("drawings.xls");
  CHECK(book && !book->GetSheet(0).DrawingRelId().empty());
  const std::vector<uint8_t> bytes = SeedBytes("drawings.xls");
  auto cf = pkg::CompoundFile::Open(bytes);
  if (!cf) return;
  pkg::Status st;
  const std::vector<uint8_t> xlsx = xls::ToXlsx([&](std::string_view n) { return cf->Stream(n); }, st);
  const std::string zip(xlsx.begin(), xlsx.end());
  CHECK(zip.find("xl/media/image1.png") != std::string::npos && zip.find("xl/drawings/drawing1.xml") != std::string::npos);
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

namespace {

// A BIFF8 record: type, size, data.
void Rec(std::vector<uint8_t>& out, uint16_t type, std::initializer_list<uint32_t> fields, std::initializer_list<int> sizes) {
  std::vector<uint8_t> data;
  auto size = sizes.begin();
  for (uint32_t v : fields) {
    for (int i = 0; i < *size; ++i) data.push_back(static_cast<uint8_t>(v >> (8 * i)));
    ++size;
  }
  for (uint16_t v : {type, static_cast<uint16_t>(data.size())}) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
  }
  out.insert(out.end(), data.begin(), data.end());
}

}  // namespace

namespace {

// An OfficeArt record: version and instance, type, body.
std::vector<uint8_t> Art(uint16_t ver, uint16_t inst, uint16_t type, const std::vector<uint8_t>& body) {
  const uint16_t vi = static_cast<uint16_t>(ver | inst << 4);
  const uint32_t n = static_cast<uint32_t>(body.size());
  std::vector<uint8_t> out = {static_cast<uint8_t>(vi), static_cast<uint8_t>(vi >> 8), static_cast<uint8_t>(type),
                              static_cast<uint8_t>(type >> 8), static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8),
                              static_cast<uint8_t>(n >> 16), static_cast<uint8_t>(n >> 24)};
  out.insert(out.end(), body.begin(), body.end());
  return out;
}
std::vector<uint8_t> Cat(std::initializer_list<std::vector<uint8_t>> parts) {
  std::vector<uint8_t> out;
  for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}
std::vector<uint8_t> Le(std::initializer_list<uint32_t> v, int size) {
  std::vector<uint8_t> out;
  for (uint32_t x : v)
    for (int i = 0; i < size; ++i) out.push_back(static_cast<uint8_t>(x >> (8 * i)));
  return out;
}
// A shape: FSP (type, flags), FOPT (id, value pairs), an anchor, client data and maybe a text box.
std::vector<uint8_t> ArtShape(uint16_t spt, uint32_t flags, std::vector<std::pair<uint16_t, uint32_t>> props, std::vector<uint8_t> anchor,
                              bool text) {
  std::vector<uint8_t> opt;
  for (auto [id, v] : props) {
    auto b = Cat({Le({id}, 2), Le({v}, 4)});
    opt.insert(opt.end(), b.begin(), b.end());
  }
  return Art(0xF, 0, 0xF004, Cat({Art(2, spt, 0xF00A, Le({1025, flags}, 4)), Art(3, static_cast<uint16_t>(props.size()), 0xF00B, opt),
                                  anchor, Art(0, 0, 0xF011, {}), text ? Art(0, 0, 0xF00D, Le({0}, 4)) : std::vector<uint8_t>()}));
}

}  // namespace

TEST(xls_drawings) {
  // The BLIP store: one PNG (an FBSE with the BLIP inside it).
  const std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 13, 10, 26, 10, 1, 2, 3};
  const std::vector<uint8_t> blip = Art(0, 0x6E0, 0xF01E, Cat({std::vector<uint8_t>(16, 7), {0xFF}, png}));
  std::vector<uint8_t> fbse = Cat({{6, 6}, std::vector<uint8_t>(16, 7), Le({0}, 2), Le({static_cast<uint32_t>(blip.size()), 1, 0}, 4),
                                   {0, 0, 0, 0}, blip});
  const std::vector<uint8_t> dgg = Art(0xF, 0, 0xF000, Cat({Art(0, 0, 0xF006, Le({1026, 2, 1, 1}, 4)), Art(0xF, 1, 0xF001, Art(2, 6, 0xF007, fbse))}));
  std::string ext;
  std::vector<uint8_t> bytes;
  CHECK(xls::detail::BlipPicture(dgg, 1, ext, bytes) && ext == "png" && bytes == png);
  CHECK(!xls::detail::BlipPicture(dgg, 2, ext, bytes));

  // A picture over B2:C4, a text box over E2:F3, and a group (over A6:B8) with a rectangle in it.
  auto anchor = [](uint16_t c1, uint16_t r1, uint16_t c2, uint16_t r2) { return Art(0, 0, 0xF010, Le({0, c1, 0, r1, 0, c2, 0, r2, 0}, 2)); };
  const std::vector<uint8_t> group = Art(0xF, 0, 0xF003, Cat({
      Art(0xF, 0, 0xF004, Cat({Art(1, 0, 0xF009, Le({0, 0, 100, 100}, 4)), Art(2, 0, 0xF00A, Le({1027, 0x201}, 4)), anchor(0, 5, 2, 8),
                                Art(0, 0, 0xF011, {})})),
      Art(0xF, 0, 0xF004, Cat({Art(2, 1, 0xF00A, Le({1028, 0x202}, 4)), Art(3, 1, 0xF00B, Cat({Le({0x0181}, 2), Le({0x0000FF}, 4)})),
                                Art(0, 0, 0xF00F, Le({0, 0, 50, 100}, 4)), Art(0, 0, 0xF011, {})}))}));
  xls::detail::DrawingSheet in;
  in.dg = Art(0xF, 0, 0xF002, Cat({Art(0, 1, 0xF008, Le({4, 1028}, 4)),
                                   Art(0xF, 0, 0xF003, Cat({Art(0xF, 0, 0xF004, Art(2, 0, 0xF00A, Le({1024, 5}, 4))),
                                                            ArtShape(75, 0xA00, {{0x0104, 1}}, anchor(1, 1, 2, 3), false),
                                                            ArtShape(202, 0xA00, {}, anchor(4, 1, 5, 2), true), group}))}));
  in.objs = {8, 6, 0, 2};
  xls::detail::DrawingSheet::Text text;
  text.grbit = 2 << 1 | 2 << 4;  // centered, in the middle
  for (char c : std::string("One\nTwo")) text.chars.push_back(static_cast<uint8_t>(c));
  text.runs = {{0, 0}, {4, 5}};
  in.texts.push_back(text);
  xls::detail::DrawingEnv env;
  env.colPx.assign(256, 64);
  env.rowPt = [](uint32_t) { return 15.0; };
  env.icvRgb = [](uint32_t) { return 0u; };
  env.runProps = [](uint16_t ifnt) { return "<a:rPr sz=\"" + std::to_string(1000 + ifnt) + "\"/>"; };
  env.image = [](uint32_t pib) { return pib == 1 ? std::string("../media/image1.png") : std::string(); };
  std::string rels;
  const std::string xml = xls::detail::DrawingXml(in, env, rels);
  CHECK(rels.find("Target=\"../media/image1.png\"") != std::string::npos);
  CHECK(xml.find("<xdr:pic>") != std::string::npos && xml.find("r:embed=\"rId1\"") != std::string::npos);
  CHECK(xml.find("<xdr:from><xdr:col>1</xdr:col><xdr:colOff>0</xdr:colOff><xdr:row>1</xdr:row>") != std::string::npos);
  CHECK(xml.find("txBox=\"1\"") != std::string::npos && xml.find("anchor=\"ctr\"") != std::string::npos);
  CHECK(xml.find("<a:pPr algn=\"ctr\"/><a:r><a:rPr sz=\"1000\"/><a:t>One</a:t></a:r></a:p>") != std::string::npos);
  CHECK(xml.find("<a:r><a:rPr sz=\"1005\"/><a:t>Two</a:t></a:r>") != std::string::npos);
  // The group's rectangle: its left half, in red (0x0000FF is BGR), from A6 to the middle of A9.
  CHECK(xml.find("<a:srgbClr val=\"FF0000\"/>") != std::string::npos);
  CHECK(xml.find("<xdr:from><xdr:col>0</xdr:col><xdr:colOff>0</xdr:colOff><xdr:row>5</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:from>"
                 "<xdr:to><xdr:col>1</xdr:col><xdr:colOff>0</xdr:colOff><xdr:row>8</xdr:row>") != std::string::npos);
  // Without an OBJ record for each shape, only pictures.
  in.objs.pop_back();
  rels.clear();
  const std::string some = xls::detail::DrawingXml(in, env, rels);
  CHECK(some.find("<xdr:pic>") != std::string::npos && some.find("<xdr:sp ") == std::string::npos);
}

TEST(xls_chart_sheet_kept) {
  // A worksheet and a chart sheet: the chart sheet stays in its place (its chart isn't read; an empty
  // one stands in, which Excel opens), and prints its page.
  std::vector<uint8_t> globals, sheet, chart;
  Rec(globals, 0x0809, {0x0600, 0x0005, 0, 0, 0, 0}, {2, 2, 2, 2, 4, 4});
  Rec(globals, 0x0031, {220, 0, 0x7FFF, 400, 0, 0, 0, 0, 0, 5, 0, 'A', 'r', 'i', 'a', 'l'}, {2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1});
  for (int i = 0; i < 15; ++i) Rec(globals, 0x00E0, {0, 0, 0xFFF5, 0x20, 0, 0, 0, 0x20C0}, {2, 2, 2, 2, 4, 2, 4, 2});
  Rec(globals, 0x00E0, {0, 0, 0x0001, 0x20, 0, 0, 0, 0x20C0}, {2, 2, 2, 2, 4, 2, 4, 2});
  const size_t first = globals.size();
  Rec(globals, 0x0085, {0, 0, 0, 1, 0, 'S'}, {4, 1, 1, 1, 1, 1});
  const size_t second = globals.size();
  Rec(globals, 0x0085, {0, 0, 2, 1, 0, 'C'}, {4, 1, 1, 1, 1, 1});  // dt 2: a chart sheet
  Rec(globals, 0x000A, {}, {});
  Rec(sheet, 0x0809, {0x0600, 0x0010, 0, 0, 0, 0}, {2, 2, 2, 2, 4, 4});
  Rec(sheet, 0x0203, {0, 0, 15, 0, 0x3FF00000}, {2, 2, 2, 4, 4});
  Rec(sheet, 0x000A, {}, {});
  Rec(chart, 0x0809, {0x0600, 0x0020, 0, 0, 0, 0}, {2, 2, 2, 2, 4, 4});  // a chart's substream
  Rec(chart, 0x00A1, {9, 100, 1, 1, 1, 0, 600, 600, 0, 0, 0, 0, 1}, {2, 2, 2, 2, 2, 2, 2, 2, 4, 4, 4, 4, 2});  // A4, landscape
  Rec(chart, 0x000A, {}, {});
  auto put = [&](size_t at, size_t value) {
    for (int i = 0; i < 4; ++i) globals[at + 4 + i] = static_cast<uint8_t>(value >> (8 * i));
  };
  put(first, globals.size());
  put(second, globals.size() + sheet.size());
  std::vector<uint8_t> stream = globals;
  stream.insert(stream.end(), sheet.begin(), sheet.end());
  stream.insert(stream.end(), chart.begin(), chart.end());
  const xls::StreamFn streams = [&](std::string_view n) -> std::optional<std::vector<uint8_t>> {
    if (n == "Workbook") return stream;
    return std::nullopt;
  };
  pkg::Status st;
  std::vector<uint8_t> xlsx = xls::ToXlsx(streams, st);
  CHECK(st.Ok() && !xlsx.empty());
  if (xlsx.empty()) return;
  auto package = pkg::Package::Open(pkg::Snapshot::FromBytes(std::move(xlsx)), pkg::Limits::Default(), st);
  CHECK(package && pkg::CheckPackage(*package).Ok());
  if (!package) return;
  auto book = xlsx::Read(std::move(package), st, {.loadAll = true});
  CHECK(book && book->SheetCount() == 2);
  if (!book || book->SheetCount() != 2) return;
  CHECK(book->GetSheet(0).Kind() == SheetKind::Worksheet && book->GetSheet(1).Kind() == SheetKind::Chartsheet);
  CHECK(book->GetSheet(1).Name() == "C" && book->GetSheet(1).Page().orientation == Orientation::Landscape);
}

TEST(xls_row_formats_print) {
  // Excel's .xls writer keeps a row's format on its ROW record, and Excel prints the .xls down to the
  // last such row within DIMENSIONS: the used range reaches it (a blank cell in the row's format).
  std::vector<uint8_t> globals, sheet;
  Rec(globals, 0x0809, {0x0600, 0x0005, 0, 0, 0, 0}, {2, 2, 2, 2, 4, 4});  // BOF: globals
  Rec(globals, 0x0031, {220, 0, 0x7FFF, 400, 0, 0, 0, 0, 0, 5, 0, 'A', 'r', 'i', 'a', 'l'},
      {2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1});  // FONT Arial 11
  for (int i = 0; i < 15; ++i) Rec(globals, 0x00E0, {0, 0, 0xFFF5, 0x20, 0, 0, 0, 0x20C0}, {2, 2, 2, 2, 4, 2, 4, 2});  // style XFs
  Rec(globals, 0x00E0, {0, 0, 0x0001, 0x20, 0, 0, 0, 0x20C0}, {2, 2, 2, 2, 4, 2, 4, 2});                // XF 15
  Rec(globals, 0x00E0, {0, 0, 0x0001, 0x20, 0x4000, 0, 0x04000000, 0x208D}, {2, 2, 2, 2, 4, 2, 4, 2});  // XF 16: a fill
  const size_t boundSheet = globals.size();
  Rec(globals, 0x0085, {0, 0, 0, 1, 0, 'S'}, {4, 1, 1, 1, 1, 1});  // BOUNDSHEET "S"; its offset below
  Rec(globals, 0x000A, {}, {});
  const uint32_t at = static_cast<uint32_t>(globals.size());
  for (int i = 0; i < 4; ++i) globals[boundSheet + 4 + i] = static_cast<uint8_t>(at >> (8 * i));
  Rec(sheet, 0x0809, {0x0600, 0x0010, 0, 0, 0, 0}, {2, 2, 2, 2, 4, 4});  // BOF: a worksheet
  Rec(sheet, 0x0200, {0, 9, 0, 2, 0}, {4, 4, 2, 2, 2});                  // DIMENSIONS: rows 1-9
  for (uint32_t row : {4u, 5u, 6u, 7u, 8u, 11u})                         // ROW: rows 5-9 and 12 formatted
    Rec(sheet, 0x0208, {row, 0, 0, 300, 0, 0, 0x0180, 16}, {2, 2, 2, 2, 2, 2, 2, 2});
  Rec(sheet, 0x0203, {0, 0, 15, 0, 0x3FF00000}, {2, 2, 2, 4, 4});  // NUMBER: A1 = 1
  Rec(sheet, 0x000A, {}, {});
  std::vector<uint8_t> stream = globals;
  stream.insert(stream.end(), sheet.begin(), sheet.end());

  const xls::StreamFn streams = [&](std::string_view n) -> std::optional<std::vector<uint8_t>> {
    if (n == "Workbook") return stream;
    return std::nullopt;
  };
  pkg::Status st;
  std::vector<uint8_t> xlsx = xls::ToXlsx(streams, st);
  CHECK(st.Ok() && !xlsx.empty());
  if (xlsx.empty()) return;
  auto package = pkg::Package::Open(pkg::Snapshot::FromBytes(std::move(xlsx)), pkg::Limits::Default(), st);
  CHECK(package != nullptr);
  if (!package) return;
  auto book = xlsx::Read(std::move(package), st, {.loadAll = true});
  CHECK(book != nullptr);
  if (!book) return;
  const Sheet& s = book->GetSheet(0);
  CHECK_EQ(s.GetValue(0, 0).number, 1.0);
  const std::optional<Range> used = s.UsedRange();
  CHECK(used && used->lastRow == 8 && used->lastCol == 0);  // row 9; not row 12, past DIMENSIONS
  CellInfo cell;
  CHECK(s.GetCell(8, 0, cell) && cell.style != 0 && cell.style == s.Row(8).style);
  CHECK(s.Row(11).customFormat && !s.GetCell(11, 0, cell));
}
