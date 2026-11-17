#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "core/Json.h"
#include "core/Strings.h"
#include "sheet/csv/Csv.h"
#include "sheet/numfmt/NumFormat.h"

using namespace fw;
using namespace fw::sheet;

namespace {

std::filesystem::path CsvDir() { return std::filesystem::path(__FILE__).parent_path().parent_path() / "office" / "csv"; }

std::string Load(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}

// A cell as the grid shows it in a wide column.
std::string Shown(const Workbook& book, const Sheet& sheet, uint32_t row, uint32_t col, const NumLocale& loc) {
  CellInfo info;
  if (!sheet.GetCell(row, col, info)) return {};
  switch (info.value.type) {
    case ValueType::String: return std::string(info.value.text);
    case ValueType::Bool: return info.value.boolean ? "TRUE" : "FALSE";
    case ValueType::Error: return std::string(ErrorText(info.value.error));
    case ValueType::Number: {
      const Styles& st = book.GetStyles();
      const uint32_t id = st.CellXf(info.style).numFmt;
      const std::string_view code = st.FormatCode(id);
      const NumberFormat f = code.empty() ? NumberFormat::Builtin(static_cast<int>(id), loc) : NumberFormat(Utf8ToWide(code));
      FormatContext ctx;
      ctx.locale = &loc;
      Formatted out;
      f.Format(info.value.number, ctx, out);
      return WideToUtf8(out.text);
    }
    default: return {};
  }
}

const char* EncodingName(csv::Encoding e) {
  switch (e) {
    case csv::Encoding::Utf8: return "utf-8";
    case csv::Encoding::Utf16LE: return "utf-16-le";
    case csv::Encoding::Utf16BE: return "utf-16-be";
    default: return "cp1252";
  }
}

const char* LineEndName(csv::LineEnd e) { return e == csv::LineEnd::LF ? "LF" : e == csv::LineEnd::CR ? "CR" : "CRLF"; }

}  // namespace

// Every file of tests/office/csv as expected.json describes it: encoding, byte-order mark,
// delimiter, line ends, every field's text, and every value shown exactly as written once read
// into a sheet (in en-US and en-NL, whose decimal separators differ).
TEST(SheetCsv_Corpus) {
  const auto json = ParseJson(Load(CsvDir() / "expected.json"));
  CHECK(json.has_value());
  if (!json) return;
  const JsonValue* files = json->Find("files");
  CHECK(files && files->object.size() >= 25);
  if (!files) return;
  int checked = 0;
  for (const auto& [name, want] : files->object) {
    const std::string bytes = Load(CsvDir() / name);
    csv::Options opts;
    opts.locale = &EnUsLocale();
    opts.tsv = name.size() > 4 && name.substr(name.size() - 4) == ".tsv";
    csv::Reader reader(bytes, opts);
    const csv::Source& src = reader.Info();
    auto fail = [&](const std::string& what) { std::printf("  %s: %s\n", name.c_str(), what.c_str()); CHECK(false); };
    if (want.StringOr("encoding") != EncodingName(src.encoding)) fail("encoding");
    if (const JsonValue* bom = want.Find("bom"); bom && bom->boolean != src.bom) fail("byte-order mark");
    if (want.StringOr("delimiter") != std::string(1, src.delimiter)) fail("delimiter");
    if (want.StringOr("lineEnding") != LineEndName(src.lineEnd)) fail("line ends");
    const JsonValue* rows = want.Find("rows");
    if (!rows) continue;  // the big ones: by their rule, below
    if (reader.RowCount() != rows->array.size()) {
      fail("rows: " + std::to_string(reader.RowCount()) + " not " + std::to_string(rows->array.size()));
      continue;
    }
    std::vector<std::string> fields;
    for (size_t r = 0; r < rows->array.size(); ++r) {
      reader.Fields(r, fields);
      const auto& wantRow = rows->array[r].array;
      bool same = fields.size() == wantRow.size();
      for (size_t c = 0; same && c < fields.size(); ++c) same = fields[c] == wantRow[c].string;
      if (!same) fail("row " + std::to_string(r + 1) + " fields");
    }
    for (const NumLocale* loc : {&EnUsLocale(), &EnNlLocale()}) {
      Workbook book;
      csv::Source source;
      csv::Options o = opts;
      o.locale = loc;
      csv::Read(bytes, "Sheet1", o, book, source);
      const Sheet& sheet = book.GetSheet(0);
      for (size_t r = 0; r < rows->array.size(); ++r)
        for (size_t c = 0; c < rows->array[r].array.size(); ++c) {
          const std::string& text = rows->array[r].array[c].string;
          const std::string shown = Shown(book, sheet, static_cast<uint32_t>(r), static_cast<uint32_t>(c), *loc);
          ++checked;
          if (shown != text) fail("cell " + CellName(static_cast<uint32_t>(r), static_cast<uint32_t>(c)) + " shows '" +
                                  shown + "', not '" + text + "'");
        }
    }
  }
  std::printf("  %d cells checked\n", checked);
}

TEST(SheetCsv_BigOnes) {
  {
    Workbook book;
    csv::Source src;
    csv::Read(Load(CsvDir() / "wide-16500.csv"), "S", {}, book, src);
    const Sheet& s = book.GetSheet(0);
    CHECK(src.tooManyColumns);
    CHECK(Shown(book, s, 0, 16383, EnUsLocale()) == "c16384");
    CHECK(Shown(book, s, 1, 0, EnUsLocale()) == "c1");
  }
  {
    Workbook book;
    csv::Source src;
    csv::Read(Load(CsvDir() / "wide-16384.csv"), "S", {}, book, src);
    CHECK(!src.tooManyColumns);
    CHECK(Shown(book, book.GetSheet(0), 1, 16383, EnUsLocale()) == "c16384");
  }
  {
    Workbook book;
    csv::Source src;
    csv::Read(Load(CsvDir() / "long-field.csv"), "S", {}, book, src);
    CHECK(Shown(book, book.GetSheet(0), 1, 1, EnUsLocale()) == std::string(40000, 'x'));
  }
}

TEST(SheetCsv_Values) {
  const std::string text =
      "id,amount,share,when,flag,err,code,big\r\n"
      "007,\"1,234.50\",12.5%,2023-03-15,TRUE,#N/A,1-2,1234567890123456\r\n"
      "7,-17,50%,3/15/2023 13:45,FALSE,#DIV/0!,MAR1,123456789012\r\n";
  Workbook book;
  csv::Source src;
  csv::Options o;
  o.locale = &EnUsLocale();
  csv::Read(text, "S", o, book, src);
  const Sheet& s = book.GetSheet(0);
  CellInfo c;
  auto type = [&](uint32_t r, uint32_t col) { return s.GetCell(r, col, c) ? c.value.type : ValueType::Empty; };
  CHECK(type(1, 0) == ValueType::String);  // 007
  CHECK(type(2, 0) == ValueType::Number && c.value.number == 7);
  CHECK(type(1, 1) == ValueType::Number && c.value.number == 1234.5);
  CHECK(type(1, 2) == ValueType::Number && c.value.number == 0.125);
  CHECK(type(1, 3) == ValueType::Number && c.value.number == 45000);
  CHECK(type(1, 4) == ValueType::Bool && c.value.boolean);
  CHECK(type(1, 5) == ValueType::Error);
  CHECK(type(1, 6) == ValueType::String);  // 1-2: no year
  CHECK(type(2, 6) == ValueType::String);  // MAR1
  CHECK(type(1, 7) == ValueType::String);  // 16 digits
  CHECK(type(2, 7) == ValueType::Number && Shown(book, s, 2, 7, EnUsLocale()) == "123456789012");
  for (uint32_t r = 1; r < 3; ++r)
    for (uint32_t col = 0; col < 8; ++col) {
      std::vector<std::string> fields;
      csv::Reader(text, o).Fields(r, fields);
      CHECK(Shown(book, s, r, col, EnUsLocale()) == fields[col]);
    }
  // No conversion: everything text.
  Workbook plain;
  o.convert = false;
  csv::Read(text, "S", o, plain, src);
  CHECK(plain.GetSheet(0).GetCell(1, 1, c) && c.value.type == ValueType::String);
}

TEST(SheetCsv_Parallel) {
  // Many rows (more than one batch), parsed by several threads, come out in order and the same as
  // one thread; repeated values share their string.
  std::string text;
  for (int r = 0; r < 70000; ++r) text += std::to_string(r) + ",\"q" + std::to_string(r) + "\",x" + std::to_string(r % 7) + "\n";
  csv::Options o;
  o.locale = &EnUsLocale();
  o.threads = 4;
  Workbook a, b;
  csv::Source sa, sb;
  csv::Read(text, "S", o, a, sa);
  o.threads = 1;
  csv::Read(text, "S", o, b, sb);
  CHECK(sa.rowStart.size() == 70000 && sa.lineEnd == csv::LineEnd::LF);
  for (uint32_t r : {0u, 1u, 4999u, 5000u, 12345u, 19999u, 65535u, 65536u, 69999u}) {
    CHECK(Shown(a, a.GetSheet(0), r, 0, EnUsLocale()) == std::to_string(r));
    CHECK(Shown(a, a.GetSheet(0), r, 1, EnUsLocale()) == "q" + std::to_string(r));
    CHECK(Shown(b, b.GetSheet(0), r, 1, EnUsLocale()) == "q" + std::to_string(r));
    CHECK(Shown(a, a.GetSheet(0), r, 2, EnUsLocale()) == "x" + std::to_string(r % 7));
  }
  CHECK(a.GetSheet(0).CellCount() == 210000);
  CHECK(a.Strings().Size() < 80000);  // 70,000 q's; the x's mostly shared (140,000 unshared)
}
