#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>

#include "TestHarness.h"
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
