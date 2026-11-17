#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "sheet/csv/Csv.h"
#include "sheet/edit/Edit.h"
#include "sheet/numfmt/NumLocale.h"

using namespace fw::sheet;

namespace {

csv::Options Opts(const NumLocale& loc) {
  csv::Options o;
  o.locale = &loc;
  o.threads = 1;
  o.ansiCodePage = 1252;
  return o;
}

std::unique_ptr<Workbook> Open(std::string_view bytes, const csv::Options& o) {
  auto book = std::make_unique<Workbook>();
  csv::Source src;
  csv::Read(bytes, "Sheet1", o, *book, src);
  return book;
}

std::string Hex(std::string_view s) {
  std::string out;
  for (unsigned char c : s) {
    if (c >= 32 && c < 127) {
      out += static_cast<char>(c);
    } else {
      char b[8];
      std::snprintf(b, sizeof b, "\\x%02X", c);
      out += b;
    }
  }
  return out;
}

bool RoundTrips(std::string_view bytes, const NumLocale& loc) {
  const csv::Options o = Opts(loc);
  auto book = Open(bytes, o);
  const std::string out = csv::Write(*book, bytes, o);
  if (out == bytes) return true;
  std::printf("  not the same: '%s'\n  gave         '%s'\n", Hex(bytes).c_str(), Hex(out).c_str());
  return false;
}

std::string Utf16(std::wstring_view w, bool be) {
  std::string out = be ? "\xFE\xFF" : "\xFF\xFE";
  for (wchar_t c : w) {
    const char lo = static_cast<char>(c & 0xFF), hi = static_cast<char>(c >> 8);
    out += be ? hi : lo;
    out += be ? lo : hi;
  }
  return out;
}

}  // namespace

// A file read and saved untouched gives the same bytes, whatever its encoding, quoting and line ends.
TEST(SheetCsvWrite_Untouched) {
  const NumLocale& us = EnUsLocale();
  const NumLocale& nl = EnNlLocale();
  CHECK(RoundTrips("a,b,c\r\n1,2,3\r\n", us));
  CHECK(RoundTrips("a,b\n1,2", us));                       // LF, no final line end
  CHECK(RoundTrips("a,b\r1,2\r\n3,4\n\n", us));            // mixed line ends, a blank line at the end
  CHECK(RoundTrips("\xEF\xBB\xBF\"a\",\"b\"\r\n\"1\",\"\"\r\n", us));  // BOM, every field quoted
  CHECK(RoundTrips("sep=;\r\nx;y\r\n1,5;2\r\n", nl));      // Excel's sep= line
  CHECK(RoundTrips("\"two\r\nlines\",\"say \"\"hi\"\"\"\r\n007,1E3,TRUE,#N/A\r\n", us));
  CHECK(RoundTrips("caf\xE9;na\xEFve\r\n1.234,5;15/03/2023\r\n", nl));  // ANSI (1252)
  CHECK(RoundTrips(Utf16(L"a\tb\r\n\x20AC;1\r\n", false), us));
  CHECK(RoundTrips(Utf16(L"a,b\r\n1,2\r\n", true), us));
  CHECK(RoundTrips("a,,b,\r\n,,\r\n1\r\n", us));            // ragged rows, empty fields
  CHECK(RoundTrips("", us));
  CHECK(RoundTrips("a\r\n\"\",,\"\"\r\n", us));  // a last row of empty fields (fuzz_csv found it)
}

// An edited cell rewrites its row only; the others are copied as they were read.
TEST(SheetCsvWrite_EditedRow) {
  const NumLocale& us = EnUsLocale();
  const csv::Options o = Opts(us);
  const std::string file = "\"id\",\"name\"\n\"1\",\"Ann\"\n\"2\",\"Bob\"\n";
  auto book = Open(file, o);
  book->GetSheet(0).SetValue(2, 1, Value::Text("Bo, Jr."));
  CHECK_EQ(csv::Write(*book, file, o), std::string("\"id\",\"name\"\n\"1\",\"Ann\"\n\"2\",\"Bo, Jr.\"\n"));

  const std::string plain = "a;b\r\n1;2\r\n3;4";  // no final line end
  const csv::Options onl = Opts(EnNlLocale());
  auto b2 = Open(plain, onl);
  b2->GetSheet(0).SetValue(3, 0, Value::Number(1.5));  // a new row after the last
  CHECK_EQ(csv::Write(*b2, plain, onl), std::string("a;b\r\n1;2\r\n3;4\r\n1,5"));
  b2->GetSheet(0).SetValue(0, 1, Value::Text("x;y"));
  CHECK_EQ(csv::Write(*b2, plain, onl), std::string("a;\"x;y\"\r\n1;2\r\n3;4\r\n1,5"));
}

// A new CSV file: UTF-8 with a byte-order mark, the region's list separator, from the used range.
TEST(SheetCsvWrite_New) {
  Workbook book;
  Sheet& s = book.AddSheet("Sheet1");
  s.SetValue(1, 1, Value::Number(1.0 / 3));
  s.SetValue(1, 3, Value::Text("a;b"));
  s.SetValue(2, 2, Value::Bool(true));
  s.SetValue(3, 1, Value::Text("say \"hi\""));
  csv::Options o = Opts(EnNlLocale());
  CHECK_EQ(csv::WriteNew(book, o),
           std::string("\xEF\xBB\xBF" "0,333333333;;\"a;b\"\r\n;TRUE;\r\n\"say \"\"hi\"\"\";;\r\n"));
  o.locale = &EnUsLocale();
  CHECK_EQ(csv::WriteNew(book, o), std::string("\xEF\xBB\xBF" "0.333333333,,a;b\r\n,TRUE,\r\n\"say \"\"hi\"\"\",,\r\n"));
}

// The CSV corpus: every file saved untouched is the same file.
TEST(SheetCsvWrite_Corpus) {
  const auto dir = std::filesystem::path(__FILE__).parent_path().parent_path() / "office" / "csv";
  std::error_code ec;
  int files = 0, same = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    const auto ext = e.path().extension().string();
    if (ext != ".csv" && ext != ".tsv") continue;
    std::ifstream f(e.path(), std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string bytes = ss.str();
    csv::Options o = Opts(EnUsLocale());
    o.tsv = ext == ".tsv";
    auto book = Open(bytes, o);
    ++files;
    if (csv::Write(*book, bytes, o) == bytes) ++same;
    else std::printf("  %s changed\n", e.path().filename().string().c_str());
  }
  std::printf("  %d of %d CSV files saved untouched are the same bytes\n", same, files);
  CHECK_EQ(same, files);
}

// What CSV can't keep: formatting beyond number formats, formulas, another sheet.
TEST(SheetCsvWrite_Loses) {
  auto book = Open("Date;Amount\r\n17/11/2026;2,50\r\n", Opts(EnNlLocale()));
  CHECK(!csv::Loses(*book));  // the dates' and numbers' formats show in the values written
  edit::Editor ed(*book);
  Font bold = book->GetStyles().FontOf(book->GetStyles().CellXf(0));
  bold.bold = true;
  Xf x = book->GetStyles().CellXf(0);
  x.font = ed.AddFont(bold);
  x.applyFont = true;
  const uint32_t xf = ed.AddXf(x);
  CHECK(ed.MapStyles(0, Range{0, 0, 0, 1}, [&](uint32_t) { return xf; }));
  CHECK(csv::Loses(*book));
  CHECK(ed.Undo());
  CHECK(!csv::Loses(*book));  // the bold format is still listed, but no cell has it
  CHECK(ed.SetCell(0, 2, 1, edit::CellInput::Formula("SUM(B2:B2)")));
  CHECK(csv::Loses(*book));
  CHECK(ed.Undo());
  CHECK(!csv::Loses(*book));
  CHECK(ed.SetColumnWidth(0, 0, 0, 20));
  CHECK(csv::Loses(*book));
  CHECK(ed.Undo());
  CHECK(ed.AddSheet(-1));
  CHECK(csv::Loses(*book));
}

// A CSV workbook sorts like any other (the tab's Sort A to Z, with its keys); CSV loses nothing.
TEST(SheetCsvWrite_Sorted) {
  auto book = Open("Item,Amount\r\nTea,5\r\nCoffee,9\r\nMilk,2\r\n", Opts(EnUsLocale()));
  edit::Editor ed(*book);
  const Sheet& s = book->GetSheet(0);
  CHECK(ed.SortRows(0, Range{1, 0, 3, 1}, {1, 0, 2}, {{1, true}}));
  CHECK(s.GetValue(1, 1).number == 9 && s.GetValue(2, 1).number == 5 && s.GetValue(3, 1).number == 2);
  CHECK(!csv::Loses(*book));
  CHECK(ed.Undo());
  CHECK(s.GetValue(1, 1).number == 5);
}
