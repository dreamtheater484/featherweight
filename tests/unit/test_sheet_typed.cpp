#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>

#include "TestHarness.h"
#include "sheet/numfmt/NumFormat.h"
#include "sheet/numfmt/NumLocale.h"
#include "sheet/numfmt/Parse.h"

using namespace fw::sheet;

namespace {

struct TypedCase {
  const wchar_t* typed;
  TypedValue::Kind kind;
  double number;
  const wchar_t* text;  // Text: the cell's text; Error: its name
  bool quoted;
  int format;
};

#include "sheet_typed_table.inc"

std::string Narrow(std::wstring_view s) {
  std::string out;
  for (wchar_t c : s) {
    if (c >= 32 && c < 127) {
      out += static_cast<char>(c);
    } else {
      char b[12];
      std::snprintf(b, sizeof b, "\\u%04X", static_cast<unsigned>(c));
      out += b;
    }
  }
  return out;
}

bool Same(const TypedValue& got, const TypedCase& c) {
  if (got.kind != c.kind || got.format != c.format) return false;
  if (c.kind == TypedValue::Text) return got.text == (c.text ? c.text : L"") && got.quoted == c.quoted;
  if (c.kind == TypedValue::Error) return got.text == c.text;
  return std::fabs(got.number - c.number) <= 1e-9 * std::max(1.0, std::fabs(c.number));
}

}  // namespace

// Every string of tests/sheet/typed_cases.json as Excel converted it when typed into a General cell.
TEST(SheetTyped_ExcelTable) {
  int failures = 0;
  for (const TypedCase& c : kTypedCases) {
    TypedValue got;
    ParseTyped(c.typed, EnNlLocale(), false, kTypedYear, got);
    if (Same(got, c)) continue;
    if (++failures <= 60)
      std::printf("  '%s': want kind %d %.17g '%s' format %d, got kind %d %.17g '%s' format %d\n", Narrow(c.typed).c_str(),
                  c.kind, c.number, Narrow(c.text ? c.text : L"").c_str(), c.format, got.kind, got.number,
                  Narrow(got.text).c_str(), got.format);
  }
  std::printf("  %d of %zu typed values differ from Excel\n", failures, std::size(kTypedCases));
  CHECK_EQ(failures, 0);
}

TEST(SheetTyped_Basics) {
  const NumLocale& us = EnUsLocale();
  TypedValue v;
  ParseTyped(L"1,234.5", us, false, 2026, v);
  CHECK(v.kind == TypedValue::Number && v.number == 1234.5 && v.format == 4);
  ParseTyped(L"3/4", us, false, 2026, v);  // m/d in en-US: 4 March this year
  CHECK(v.kind == TypedValue::Number && v.format == 16);
  ParseTyped(L"1/2/2024", EnUsLocale(), true, 2026, v);  // 1904: four years fewer
  CHECK(v.kind == TypedValue::Number && v.number == 45293 - 1462 && v.format == 14);
  ParseTyped(L"'00123", us, false, 2026, v);
  CHECK(v.kind == TypedValue::Text && v.quoted && v.text == L"00123");
  ParseTyped(L"", us, false, 2026, v);
  CHECK(v.kind == TypedValue::Text && v.text.empty() && v.format == 0);
}

// Typing into a cell that has a format: the format the cell ends with, as Excel 2021 in en-NL gave
// it (Range.FormulaLocal into cells preset with each built-in kind and custom twins).
TEST(SheetTyped_IntoFormattedCells) {
  struct Case {
    uint32_t cell;
    const wchar_t* typed;
    uint32_t want;
  };
  static const Case kCases[] = {
      {0, L"50%", 9}, {0, L"5", 0}, {0, L"1.234", 3},
      {2, L"1/2/2024", 2}, {2, L"50%", 2}, {2, L"€ 5", 2}, {2, L"12:30", 2}, {4, L"1e3", 4}, {3, L"0 1/2", 3},
      {37, L"50%", 37}, {40, L"1/2/2024", 40},
      {9, L"5", 9}, {9, L"1.234", 9}, {9, L"0,5%", 9}, {9, L"€ 5", 6}, {9, L"1e3", 11}, {9, L"0 1/2", 12},
      {9, L"1/2/2024", 14}, {9, L"1-feb", 16}, {9, L"12:30", 20}, {9, L"1:30 PM", 18}, {9, L"1/2/2024 12:30", 22},
      {10, L"50%", 10}, {11, L"50%", 9}, {11, L"1e3", 11}, {48, L"1e3", 48}, {48, L"12:30", 20},
      {12, L"0 1/2", 12}, {12, L"€ 5", 6}, {13, L"0 1/2", 13}, {13, L"50%", 9},
      {14, L"1/2/2024", 14}, {14, L"1-feb", 14}, {14, L"1/2/2024 12:30", 14}, {14, L"12:30", 20}, {14, L"50%", 9},
      {16, L"1/2/2024", 16}, {16, L"12:30", 20}, {22, L"1/2/2024", 22}, {22, L"12:30", 20},
      {18, L"12:30", 18}, {18, L"1/2/2024", 14}, {18, L"1/2/2024 12:30", 22}, {20, L"1/2/2024 12:30", 22},
      {20, L"1:30 PM", 20}, {46, L"12:30", 46}, {46, L"1-feb", 16}, {47, L"12:30", 47}, {47, L"50%", 9},
      {5, L"€ 5", 5}, {5, L"50%", 9}, {5, L"5", 5}, {42, L"€ 5", 6}, {42, L"5", 42}, {42, L"1/2/2024", 14},
      {44, L"€ 5", 6}, {49, L"50%", 49}, {164, L"50%", 164}, {164, L"1/2/2024", 164}, {9, L"abc", 9}, {14, L"TRUE", 14},
  };
  int failures = 0;
  for (const Case& c : kCases) {
    TypedValue v;
    ParseTyped(c.typed, EnNlLocale(), false, 2026, v);
    const uint32_t got = TypedFormat(c.cell, v.format);
    if (got != c.want && ++failures <= 10)
      std::printf("  cell %u typed %ls (format %d): got %u, want %u\n", c.cell, c.typed, v.format, got, c.want);
  }
  CHECK_EQ(failures, 0);
}

// Excel's automatic percent entry applies to formats with a % placeholder in any section.
TEST(SheetTyped_PercentEntry) {
  CHECK(NumberFormat(L"0%").HasPercent());
  CHECK(NumberFormat(L"0.000%").HasPercent());
  CHECK(NumberFormat(L"[Red]0%").HasPercent());
  CHECK(NumberFormat(L"0;0%").HasPercent());
  CHECK(!NumberFormat(L"0.00\" %\"").HasPercent());
  CHECK(!NumberFormat(L"#,##0.0\\%").HasPercent());
  CHECK(!NumberFormat(L"General").HasPercent());
  CHECK(!NumberFormat(L"m/d/yyyy").HasPercent());
}
