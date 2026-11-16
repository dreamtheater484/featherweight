#include <cstdio>
#include <string>

#include "TestHarness.h"
#include "sheet/numfmt/NumFormat.h"

using namespace fw::sheet;

namespace {

struct Case {
  const wchar_t* code;
  int builtin;  // -1: the code
  bool date1904;
  double number;
  const wchar_t* text;  // nullptr: the number
  const wchar_t* nl;    // Excel's Range.Text, en-NL
  const wchar_t* us;    // Excel's TEXT() in English (en-US); nullptr: not asked
};

const Case kCases[] = {
#include "sheet_numfmt_table.inc"
};

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

Formatted Show(const Case& c, const NumLocale& locale) {
  const NumberFormat f = c.builtin >= 0 ? NumberFormat::Builtin(c.builtin, locale) : NumberFormat(c.code);
  FormatContext ctx;
  ctx.locale = &locale;
  ctx.date1904 = c.date1904;
  Formatted out;
  if (c.text) f.Format(c.text, ctx, out);
  else f.Format(c.number, ctx, out);
  return out;
}

// Range.Text: the fill repeats to the column's width, "####" fills it.
bool MatchesCell(const Formatted& got, std::wstring_view want) {
  if (got.overflow) return !want.empty() && want.find_first_not_of(L'#') == std::wstring_view::npos;
  if (got.fill < 0) return got.text == want;
  const size_t at = static_cast<size_t>(got.fill);
  if (want.size() < got.text.size()) return false;
  const size_t k = want.size() - got.text.size();
  if (want.substr(0, at) != std::wstring_view(got.text).substr(0, at)) return false;
  if (want.substr(at + k) != std::wstring_view(got.text).substr(at)) return false;
  return want.substr(at, k).find_first_not_of(got.fillChar) == std::wstring_view::npos;
}

bool MatchesText(const Formatted& got, std::wstring_view want) {
  if (want == L"!error") return got.overflow;
  return !got.overflow && got.text == want;
}

void Report(const Case& c, const char* which, const Formatted& got, const wchar_t* want, int& shown) {
  if (++shown > 60) return;
  std::string value = c.text ? "\"" + Narrow(c.text) + "\"" : std::to_string(c.number);
  if (!c.text) {
    char b[40];
    std::snprintf(b, sizeof b, "%.17g", c.number);
    value = b;
  }
  std::printf("  %s %s%s [%s] %s: want '%s' got '%s'%s\n", which, c.builtin >= 0 ? "builtin " : "",
              c.builtin >= 0 ? std::to_string(c.builtin).c_str() : Narrow(c.code).c_str(), value.c_str(),
              c.date1904 ? "1904" : "", Narrow(want).c_str(), Narrow(got.text).c_str(), got.overflow ? " (####)" : "");
}

}  // namespace

// Every case of tests/sheet/numfmt_cases.txt against what Excel showed, in both locales.
TEST(SheetNumFmt_ExcelTable) {
  int failures = 0, shown = 0, checked = 0;
  for (const Case& c : kCases) {
    const Formatted nl = Show(c, EnNlLocale());
    ++checked;
    if (!MatchesCell(nl, c.nl)) {
      ++failures;
      Report(c, "nl", nl, c.nl, shown);
    }
    if (c.us) {
      const Formatted us = Show(c, EnUsLocale());
      ++checked;
      if (!MatchesText(us, c.us)) {
        ++failures;
        Report(c, "us", us, c.us, shown);
      }
    }
  }
  std::printf("  %d of %d Excel cases differ\n", failures, checked);
  CHECK_EQ(failures, 0);
}

TEST(SheetNumFmt_GeneralWidth) {
  const NumLocale& us = EnUsLocale();
  CHECK(FormatGeneral(1.0 / 3, 11, us) == L"0.333333333");
  CHECK(FormatGeneral(1.0 / 3, 5, us) == L"0.333");
  CHECK(FormatGeneral(123456789, 8, us) == L"1.23E+08");
  CHECK(FormatGeneral(123456789, 5, us) == L"1E+08");
  CHECK(FormatGeneral(123456789, 4, us).empty());
  CHECK(FormatGeneral(-1234.5, 11, us) == L"-1234.5");
  CHECK(FormatGeneral(1234.5, 4, us) == L"1235");
  CHECK(FormatGeneral(0, 1, us) == L"0");
  CHECK(FormatGeneral(1.5, 11, EnNlLocale()) == L"1,5");
}

namespace {
struct Width {
  size_t chars;
};
bool FitsChars(void* context, const Formatted& f) { return f.text.size() <= static_cast<Width*>(context)->chars; }
}  // namespace

TEST(SheetNumFmt_FitCallback) {
  Width w{6};
  FormatContext ctx;
  ctx.fits = FitsChars;
  ctx.fitsContext = &w;
  Formatted out;
  NumberFormat(L"0.00").Format(1234.5, ctx, out);
  CHECK(out.overflow && out.fill == 0 && out.fillChar == L'#' && out.text.empty());
  NumberFormat(L"0.00").Format(123.5, ctx, out);
  CHECK(!out.overflow && out.text == L"123.50");
  NumberFormat().Format(1234.5678, ctx, out);  // General gives up digits first
  CHECK(out.text == L"1234.6");
  NumberFormat().Format(12345678, ctx, out);
  CHECK(out.text == L"1E+07");
  NumberFormat(L"@").Format(L"a long text that overflows", ctx, out);  // text never shows ####
  CHECK(!out.overflow);
}

TEST(SheetNumFmt_PadsFillsColors) {
  FormatContext ctx;
  Formatted out;
  NumberFormat(L"_(\"$\"* #,##0.00_);[Red]_(\"$\"* \\(#,##0.00\\)").Format(-1234.5, ctx, out);
  CHECK(out.text == L" $(1,234.50)");
  CHECK(out.color == 3);
  CHECK(out.fill == 2 && out.fillChar == L' ');
  CHECK(out.pads.size() == 1 && out.pads[0].at == 0 && out.pads[0].as == L'(');
  NumberFormat(L"[Color10]0").Format(5, ctx, out);
  CHECK(out.color == 10);
  CHECK(PaletteColor(3) == 0xFF0000 && PaletteColor(10) == 0x008000);
  NumberFormat(L"[Blue]General").Format(L"x", ctx, out);  // text without a text section: no color
  CHECK(out.color == 0 && out.text == L"x");
}

TEST(SheetNumFmt_Kinds) {
  CHECK(NumberFormat().IsGeneral());
  CHECK(NumberFormat(L"").IsGeneral());
  CHECK(!NumberFormat(L"[Red]General").IsGeneral());
  CHECK(NumberFormat(L"dd/mm/yyyy").IsDateTime());
  CHECK(NumberFormat(L"[h]:mm").IsDateTime());
  CHECK(NumberFormat::Builtin(14, EnNlLocale()).IsDateTime());
  CHECK(!NumberFormat(L"0.00E+00").IsDateTime());
  CHECK(!NumberFormat(L"\"d\"0").IsDateTime());
  CHECK(NumberFormat(L"@").IsText());
  CHECK(BuiltinCode(14, EnUsLocale()) == L"m/d/yyyy");
  CHECK(BuiltinCode(14, EnNlLocale()) == L"dd/mm/yyyy");
  CHECK(BuiltinCode(15, EnNlLocale()) == L"dd/mmm/yy");
  CHECK(BuiltinCode(22, EnNlLocale()) == L"dd/mm/yyyy hh:mm");
  CHECK(BuiltinCode(0, EnNlLocale()).empty());
  CHECK(BuiltinCode(200, EnNlLocale()).empty());
}

TEST(SheetNumFmt_Serials) {
  int y, m, d;
  CHECK(SerialToDate(0, false, y, m, d) && y == 1900 && m == 1 && d == 0);
  CHECK(SerialToDate(60, false, y, m, d) && y == 1900 && m == 2 && d == 29);
  CHECK(SerialToDate(61, false, y, m, d) && y == 1900 && m == 3 && d == 1);
  CHECK(SerialToDate(45000.75, false, y, m, d) && y == 2023 && m == 3 && d == 15);
  CHECK(SerialToDate(0, true, y, m, d) && y == 1904 && m == 1 && d == 1);
  CHECK(!SerialToDate(-1, false, y, m, d));
  CHECK(!SerialToDate(2958466, false, y, m, d));
  double s = 0;
  CHECK(DateToSerial(2023, 3, 15, false, s) && s == 45000);
  CHECK(DateToSerial(1900, 3, 1, false, s) && s == 61);
  CHECK(DateToSerial(1900, 2, 28, false, s) && s == 59);
  CHECK(DateToSerial(2023, 15, 1, false, s) && s == 45352);
  CHECK(DateToSerial(2024, 3, 1, false, s) && s == 45352);
  CHECK(DateToSerial(1904, 1, 1, true, s) && s == 0);
  CHECK(DateToSerial(9999, 12, 31, false, s) && s == 2958465);
  CHECK(!DateToSerial(10000, 1, 1, false, s));
}

TEST(SheetNumFmt_Locale) {
  CHECK(ExcelCodeFromPattern(L"dddd, d MMMM yyyy") == L"dddd, d mmmm yyyy");
  CHECK(ExcelCodeFromPattern(L"h:mm:ss tt") == L"h:mm:ss AM/PM");
  CHECK(ExcelCodeFromPattern(L"d. MMMM yyyy 'r.'") == L"d. mmmm yyyy \"r.\"");
  const NumLocale& nl = EnNlLocale();
  CHECK(nl.decimal == L"," && nl.group == L"." && nl.list == L";" && nl.dateOrder == 'D');
  CHECK(EnUsLocale().list == L"," && EnUsLocale().dateOrder == 'M');
  const NumLocale* de = WindowsLanguage(0x407);
  CHECK(de && de->month[2] == L"März");
  CHECK(WindowsLanguage(0) == nullptr);
  const NumLocale& user = UserLocale();  // whatever this PC is set to: it has names and separators
  CHECK(!user.decimal.empty() && !user.month[0].empty() && !user.builtin[14].empty());
}

TEST(SheetNumFmt_Robust) {
  // Codes Excel would refuse are read as literally as possible, without crashing.
  const wchar_t* codes[] = {L"[", L"\"", L"\\", L"_", L"*", L"[$", L"[>", L"[<=abc]0", L"0.0.0", L"E+", L"0E+",
                            L"?/", L"/?", L"# ?/", L"0;0;0;0;0", L"[h", L"[hhh]:mm", L"yyyyyyyy", L"[$-FFFFFFFFF]0",
                            L"#,###,###,###,###,###,###,###,###,###,###,###,###,###,###", L"0.000000000000000000000000000000000000",
                            L"[Color99]0", L"[Color]0", L"#/#########", L"# ?/1000000000000", L"[ss].0000", L"A/", L"AM/P"};
  FormatContext ctx;
  Formatted out;
  for (const wchar_t* code : codes) {
    const NumberFormat f(code);
    for (double v : {0.0, 1.0, -1.0, 0.5, 1e300, -1e-300, 45000.5, 1e15})
      f.Format(v, ctx, out);
    f.Format(L"text", ctx, out);
  }
  CHECK(true);
}

#include <cmath>

#include "sheet/numfmt/Parse.h"

namespace {
bool IsNumber(const wchar_t* s, const NumLocale& l, double v, const wchar_t* code) {
  ParsedValue p;
  return ParseExact(s, l, false, p) && p.kind == ParsedValue::Number && std::fabs(p.number - v) < 1e-9 &&
         p.code == code;
}
}  // namespace

TEST(SheetNumFmt_ParseNumbers) {
  const NumLocale& us = EnUsLocale();
  const NumLocale& nl = EnNlLocale();
  CHECK(IsNumber(L"123", us, 123, L""));
  CHECK(IsNumber(L"-12.5", us, -12.5, L""));
  CHECK(IsNumber(L"0.5", us, 0.5, L""));
  CHECK(IsNumber(L"12.50", us, 12.5, L"0.00"));
  CHECK(IsNumber(L"1,234.50", us, 1234.5, L"#,##0.00"));
  CHECK(IsNumber(L"1.234,50", nl, 1234.5, L"#,##0.00"));
  CHECK(IsNumber(L"0,5", nl, 0.5, L""));
  CHECK(IsNumber(L"123456789012", us, 123456789012, L"0"));
  CHECK(IsNumber(L"3.14159265358979", us, 3.14159265358979, L"0.00000000000000"));
  CHECK(IsNumber(L"50%", us, 0.5, L"0%"));
  CHECK(IsNumber(L"33.3%", us, 0.333, L"0.0%"));
  CHECK(IsNumber(L"$1,234.50", us, 1234.5, L"\"$\"#,##0.00"));
  CHECK(IsNumber(L"-$5", us, -5, L"\"$\"0"));
  CHECK(IsNumber(L"(1,234)", us, -1234, L"#,##0;(#,##0)"));
  CHECK(IsNumber(L"\u20AC 12,50", nl, 12.5, L"\"\u20AC \"0.00"));
  CHECK(IsNumber(L"12,50 \u20AC", nl, 12.5, L"0.00\" \u20AC\""));
  CHECK(IsNumber(L"1.5E+10", us, 1.5e10, L"0.0E+00"));
  CHECK(IsNumber(L".5", us, 0.5, L".0"));
  ParsedValue p;
  CHECK(ParseExact(L"TRUE", us, false, p) && p.kind == ParsedValue::Bool && p.number == 1);
  CHECK(ParseValue(L"true", us, false, p) && p.kind == ParsedValue::Bool);  // typed: yes; CSV: no
  CHECK(ParseValue(L"007", us, false, p) && p.number == 7 && p.plain);
  // What CSV keeps as text: anything Excel would show differently.
  for (const wchar_t* s : {L"007", L"1234567890123456", L"12345678901234567890", L"1,23", L"1,2345", L"abc", L"",
                           L" 5", L"5 ", L"+5", L"true", L"1e5x", L"5%%", L"$", L"-", L"()", L"1.2.3",
                           L"0x1F", L"1_000", L"\u20AC5", L"5-", L"--5", L"(-5)"})
    CHECK(!ParseExact(s, us, false, p) || p.kind != ParsedValue::Number || (std::wcscmp(s, L"\u20AC5") == 0));
  CHECK(!ParseExact(L"1.5", nl, false, p));
  CHECK(!ParseExact(L"1,234", nl, false, p) || p.number == 1.234);
}

TEST(SheetNumFmt_ParseDates) {
  const NumLocale& us = EnUsLocale();
  const NumLocale& nl = EnNlLocale();
  ParsedValue p;
  CHECK(ParseExact(L"3/15/2023", us, false, p) && p.date && p.number == 45000 && p.code == L"m/d/yyyy");
  CHECK(ParseExact(L"03/15/2023", us, false, p) && p.number == 45000 && p.code == L"mm/dd/yyyy");
  CHECK(ParseExact(L"15/03/2023", nl, false, p) && p.number == 45000 && p.code == L"dd/mm/yyyy");
  CHECK(!ParseExact(L"3/15/2023", nl, false, p));
  CHECK(ParseExact(L"2023-03-15", nl, false, p) && p.number == 45000 && p.code == L"yyyy-mm-dd");
  CHECK(ParseExact(L"15.03.2023", nl, false, p) && p.number == 45000);
  CHECK(ParseExact(L"3/15/23", us, false, p) && p.number == 45000);
  CHECK(ParseExact(L"2023-03-15 13:45:00", us, false, p) && std::fabs(p.number - (45000 + 825 / 1440.0)) < 1e-9 &&
        p.code == L"yyyy-mm-dd h:mm:ss");
  CHECK(ParseExact(L"15-Mar-2023", us, false, p) && p.number == 45000 && p.code == L"d-mmm-yyyy");
  CHECK(ParseExact(L"15 March 2023", us, false, p) && p.number == 45000);
  CHECK(ParseExact(L"March 15, 2023", us, false, p) && p.number == 45000);
  CHECK(ParseExact(L"Mar 5, 2023", us, false, p) && p.number == 44990);
  CHECK(ParseExact(L"13:45", us, false, p) && p.date && p.code == L"h:mm" && std::fabs(p.number - 825 / 1440.0) < 1e-12);
  CHECK(ParseExact(L"09:05:30", us, false, p) && p.code == L"hh:mm:ss");
  CHECK(ParseExact(L"1:45 PM", us, false, p) && std::fabs(p.number - 825 / 1440.0) < 1e-12);
  CHECK(ParseExact(L"12:00:00.500", us, false, p) && std::fabs(p.number - 43200.5 / 86400) < 1e-12);
  CHECK(ParseExact(L"2/29/1900", us, false, p) && p.number == 60);
  CHECK(ParseExact(L"1/1/1904", us, true, p) && p.number == 0);
  // Codes that look like dates stay text: no year, no two-digit minutes, or impossible.
  for (const wchar_t* s : {L"1-2", L"MAR1", L"SEPT2", L"12-31", L"2:1", L"25:00", L"2/30/2023", L"13/1/2023",
                           L"1/1/10000", L"3/15/2023 ", L"3/15/2023 25:00", L"1:45 pm", L"2023-3-15T10:00",
                           L"1/2/3", L"Mar 2023", L"15 Mrch 2023"})
    CHECK(!ParseExact(s, us, false, p));
  CHECK(ParseValue(L"1:45 pm", us, false, p) && std::fabs(p.number - 825 / 1440.0) < 1e-12);  // typed: yes
}
