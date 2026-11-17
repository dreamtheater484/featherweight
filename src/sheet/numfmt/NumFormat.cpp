#include "sheet/numfmt/NumFormat.h"

#include <algorithm>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>

// How Excel shows values, as measured on Excel 2021 (tests/sheet/excel_numfmt.py asks Excel; the
// answers are tests/unit/test_sheet_numfmt.cpp's table). The less obvious rules:
//
// - Numbers are 15 significant digits, rounded half away from zero in decimal (2.675 shows 2.68).
//   Subnormal values show as 0.
// - Sections: one for all; two for >= 0 and < 0; three for > 0, < 0 and 0; a fourth (or a last
//   one with @) for text. The negative section shows no minus sign of its own. Conditions
//   ([>100]) replace the first two sections' tests; the minus sign is then left out when the
//   section's condition can only hold for negative values ([<0], [<=-1], [=-5]); a value no section
//   takes shows "####", or General when the format is one conditional section.
// - An automatic minus goes in front of everything, and only when the shown digits aren't all
//   zero (-0.3 in "0" shows "0"); fractions and elapsed times keep it ("-0    ").
// - Fractions are the last continued-fraction convergent whose denominator fits, computed in
//   doubles (0.3 shows 2/7 with one digit, because 1/0.3 isn't exactly 10/3). The numerator is
//   the placeholders right before the bar, text may stand on both sides of it, and the integer's
//   are all placeholders before the numerator, with the text among them.
// - Times round to the second (or the shown decimals of one), then each field is truncated.
//   Dates outside 0..9999-12-31 show "####", except a lone elapsed unit ([h]), which shows any
//   value, negative ones too. With an elapsed code, the largest time unit is the elapsed one ([h] h
//   shows the count twice, [m] h:mm a plain minute); after another date code, it shows Windows
//   Excel's odd count, the seconds field plus a half read as days (s [h]: 3.14159 shows 1293).
// - General shows at most 11 characters (a minus not counted): fixed-point unless that shows
//   fewer significant digits than scientific notation would (1.23456789E-05).
// - A tag's high byte is a digit system ([$-3000401]: Persian), the first section's for the
//   whole format; quoted digits, text and a blank fraction's 0 stay ASCII. Its next byte is the
//   calendar: 3, like a Japanese tag, shows Japan's eras for g and e.

namespace fw::sheet {

using Token = NumberFormat::Token;
using Section = NumberFormat::Section;

namespace {

constexpr int kMaxDecimals = 340;  // more decimal places than this are shown as zeros
constexpr double kMaxSerial1900 = 2958466;  // 10000-01-01: dates must be below
constexpr double kMaxSerial1904 = 2957004;

wchar_t Lower(wchar_t c) { return c >= 'A' && c <= 'Z' ? wchar_t(c + 32) : c; }

bool StartsNoCase(std::wstring_view s, size_t i, std::wstring_view lowerWord) {
  if (i + lowerWord.size() > s.size()) return false;
  for (size_t k = 0; k < lowerWord.size(); ++k)
    if (Lower(s[i + k]) != lowerWord[k]) return false;
  return true;
}

bool IsDateToken(Token::Kind k) {
  switch (k) {
    case Token::Year:
    case Token::Month:
    case Token::Day:
    case Token::Hour:
    case Token::Minute:
    case Token::Second:
    case Token::Elapsed:
    case Token::AmPm:
    case Token::FracSec:
    case Token::Era:
      return true;
    default:
      return false;
  }
}

// ---- Decimal digits ----

struct Decimal {
  char d[17] = {};
  int n = 0;  // significant digits (no trailing zeros); 0 for zero
  int e = 0;  // |v| = d[0].d[1]d[2]... x 10^e
};

Decimal ToDecimal(double v) {
  Decimal x;
  v = std::fabs(v);
  if (!(v >= DBL_MIN) || !std::isfinite(v)) return x;
  // printf, not std::to_chars: the app has printf anyway, and to_chars brings 100 KB of tables.
  char buf[48];
  const int len = std::snprintf(buf, sizeof buf, "%.14e", v);
  if (len <= 0 || len >= static_cast<int>(sizeof buf)) return x;
  const char* end = buf + len;
  const char* p = buf;
  x.d[0] = *p++;
  int k = 1;
  if (p < end && *p == '.') ++p;
  while (p < end && *p != 'e' && k < 15) x.d[k++] = *p++;
  while (p < end && *p != 'e') ++p;
  x.n = k;
  while (x.n > 1 && x.d[x.n - 1] == '0') --x.n;
  if (p < end) ++p;
  int sign = 1;
  if (p < end && (*p == '-' || *p == '+')) sign = *p++ == '-' ? -1 : 1;
  int e = 0;
  while (p < end) e = e * 10 + (*p++ - '0');
  x.e = sign * e;
  return x;
}

// x rounded to `decimals` places, half away from zero: `out` gets the integer digits (none for
// less than 1) then exactly `decimals` digits. Returns the integer digits' count.
int Fixed(const Decimal& x, int decimals, std::string& out) {
  decimals = std::clamp(decimals, 0, kMaxDecimals);
  int intLen = x.n && x.e >= 0 ? x.e + 1 : 0;
  const int total = intLen + decimals;
  out.assign(static_cast<size_t>(total), '0');
  for (int j = 0; j < total; ++j) {
    const int k = x.e - intLen + 1 + j;
    if (x.n && k >= 0 && k < x.n) out[j] = x.d[k];
  }
  const int next = x.e + 1 + decimals;
  if (x.n && next >= 0 && next < x.n && x.d[next] >= '5') {
    int j = total - 1;
    while (j >= 0 && out[j] == '9') out[j--] = '0';
    if (j >= 0) {
      ++out[j];
    } else {
      out.insert(out.begin(), '1');
      ++intLen;
    }
  }
  return intLen;
}

bool AllZero(const std::string& s) { return s.find_first_not_of('0') == std::string::npos; }

// a * b rounded half up, from the exact product: a * b in doubles can round up onto a half (0.5 days
// and half a second is 43200.4999999999985 seconds, which a double makes 43200.5).
double RoundProduct(double a, double b) {
  const double p = a * b;
  const double err = std::fma(a, b, -p);
  double r = std::floor(p + 0.5);
  if (r - p == 0.5 && err < 0) r -= 1;
  return r;
}

int FloorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

std::string IntDigits(double v) {  // a non-negative whole number's digits ("0" for zero)
  char buf[400];
  const int len = std::snprintf(buf, sizeof buf, "%.0f", std::floor(v));
  return len > 0 && len < static_cast<int>(sizeof buf) ? std::string(buf, static_cast<size_t>(len)) : std::string("0");
}

// The last convergent of x's continued fraction with a denominator up to maxDen, in doubles as
// Excel computes it.
void Convergent(double x, double maxDen, double& P, double& Q) {
  double p2 = 0, p1 = 1, q2 = 1, q1 = 0, B = x;
  P = 0;
  Q = 1;
  for (int guard = 0; guard < 100 && q1 < maxDen; ++guard) {
    const double A = std::floor(B);
    P = A * p1 + p2;
    Q = A * q1 + q2;
    if (B - A < 5e-8) break;
    B = 1 / (B - A);
    p2 = p1;
    p1 = P;
    q2 = q1;
    q1 = Q;
  }
  if (Q > maxDen) {
    if (q1 > maxDen) {
      Q = q2;
      P = p2;
    } else {
      Q = q1;
      P = p1;
    }
  }
}

// ---- Dates ----

void CivilFromDays(int64_t z, int& y, int& m, int& d) {  // days since 1970-01-01
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(static_cast<int64_t>(yoe) + era * 400 + (m <= 2));
}

int64_t DaysFromCivil(int64_t y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

constexpr int64_t kDay1899_12_31 = -25568;  // DaysFromCivil(1899, 12, 31)
constexpr int64_t kDay1904_01_01 = -24107;

void DateOfDay(int64_t serial, bool date1904, int& y, int& m, int& d) {
  if (date1904) return CivilFromDays(kDay1904_01_01 + serial, y, m, d);
  if (serial == 0) {
    y = 1900, m = 1, d = 0;
  } else if (serial == 60) {
    y = 1900, m = 2, d = 29;
  } else {
    CivilFromDays(kDay1899_12_31 + serial - (serial > 60 ? 1 : 0), y, m, d);
  }
}

const wchar_t* const kEnAm = L"AM";
const wchar_t* const kEnPm = L"PM";

bool EastAsian(uint32_t lcid) {
  const uint32_t primary = lcid & 0x3FF;
  return primary == 0x04 || primary == 0x11 || primary == 0x12;  // Chinese, Japanese, Korean
}

// Japan's eras from 1868: the first day (yyyymmdd), the name and its letter.
struct JapaneseEra {
  int start;
  const wchar_t* name;
  wchar_t letter;
};
constexpr JapaneseEra kJapaneseEras[] = {{18680101, L"\u660E\u6CBB", L'M'}, {19120730, L"\u5927\u6B63", L'T'},
                                         {19261225, L"\u662D\u548C", L'S'}, {19890108, L"\u5E73\u6210", L'H'},
                                         {20190501, L"\u4EE4\u548C", L'R'}};

// Digits 0..9 of the digit system a tag's high byte names ([$-3000401]: 03, Persian's), as Excel
// shows them; nullptr: ASCII.
constexpr const wchar_t* kDigitSystems[] = {
    nullptr,  // 00
    nullptr,  // 01
    L"\u0660\u0661\u0662\u0663\u0664\u0665\u0666\u0667\u0668\u0669",  // 02 Arabic-Indic
    L"\u06F0\u06F1\u06F2\u06F3\u06F4\u06F5\u06F6\u06F7\u06F8\u06F9",  // 03 Extended Arabic-Indic
    L"\u0966\u0967\u0968\u0969\u096A\u096B\u096C\u096D\u096E\u096F",  // 04 Devanagari
    L"\u09E6\u09E7\u09E8\u09E9\u09EA\u09EB\u09EC\u09ED\u09EE\u09EF",  // 05 Bengali
    L"\u0A66\u0A67\u0A68\u0A69\u0A6A\u0A6B\u0A6C\u0A6D\u0A6E\u0A6F",  // 06 Gurmukhi
    L"\u0AE6\u0AE7\u0AE8\u0AE9\u0AEA\u0AEB\u0AEC\u0AED\u0AEE\u0AEF",  // 07 Gujarati
    L"\u0B66\u0B67\u0B68\u0B69\u0B6A\u0B6B\u0B6C\u0B6D\u0B6E\u0B6F",  // 08 Oriya
    L"0\u0BE7\u0BE8\u0BE9\u0BEA\u0BEB\u0BEC\u0BED\u0BEE\u0BEF",  // 09 Tamil
    L"\u0C66\u0C67\u0C68\u0C69\u0C6A\u0C6B\u0C6C\u0C6D\u0C6E\u0C6F",  // 0A Telugu
    L"\u0CE6\u0CE7\u0CE8\u0CE9\u0CEA\u0CEB\u0CEC\u0CED\u0CEE\u0CEF",  // 0B Kannada
    L"\u0D66\u0D67\u0D68\u0D69\u0D6A\u0D6B\u0D6C\u0D6D\u0D6E\u0D6F",  // 0C Malayalam
    L"\u0E50\u0E51\u0E52\u0E53\u0E54\u0E55\u0E56\u0E57\u0E58\u0E59",  // 0D Thai
    L"\u0ED0\u0ED1\u0ED2\u0ED3\u0ED4\u0ED5\u0ED6\u0ED7\u0ED8\u0ED9",  // 0E Lao
    L"\u0F20\u0F21\u0F22\u0F23\u0F24\u0F25\u0F26\u0F27\u0F28\u0F29",  // 0F Tibetan
    L"\u1040\u1041\u1042\u1043\u1044\u1045\u1046\u1047\u1048\u1049",  // 10 Myanmar
    L"0\u1369\u136A\u136B\u136C\u136D\u136E\u136F\u1370\u1371",  // 11 Ethiopic
    L"\u17E0\u17E1\u17E2\u17E3\u17E4\u17E5\u17E6\u17E7\u17E8\u17E9",  // 12 Khmer
    L"\u1810\u1811\u1812\u1813\u1814\u1815\u1816\u1817\u1818\u1819",  // 13 Mongolian
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,    // 14..1A
    L"\u3007\u4E00\u4E8C\u4E09\u56DB\u4E94\u516D\u4E03\u516B\u4E5D",  // 1B
    L"\u3007\u58F1\u5F10\u53C2\u56DB\u4F0D\u516D\u4E03\u516B\u4E5D",  // 1C
    L"\uFF10\uFF11\uFF12\uFF13\uFF14\uFF15\uFF16\uFF17\uFF18\uFF19",  // 1D
    L"\u25CB\u4E00\u4E8C\u4E09\u56DB\u4E94\u516D\u4E03\u516B\u4E5D",  // 1E
    L"\u96F6\u58F9\u8D30\u53C1\u8086\u4F0D\u9646\u67D2\u634C\u7396",  // 1F
    L"\uFF10\uFF11\uFF12\uFF13\uFF14\uFF15\uFF16\uFF17\uFF18\uFF19",  // 20
    L"\u25CB\u4E00\u4E8C\u4E09\u56DB\u4E94\u516D\u4E03\u516B\u4E5D",  // 21
    L"\u96F6\u58F9\u8CB3\u53C3\u8086\u4F0D\u9678\u67D2\u634C\u7396",  // 22
    L"\uFF10\uFF11\uFF12\uFF13\uFF14\uFF15\uFF16\uFF17\uFF18\uFF19",  // 23
    L"\uFF10\u4E00\u4E8C\u4E09\u56DB\u4E94\uF9D1\u4E03\u516B\u4E5D",  // 24
    L"\uF9B2\u58F9\u8CB3\uF96B\u56DB\u4F0D\uF9D1\u4E03\u516B\u4E5D",  // 25
    L"\uFF10\uFF11\uFF12\uFF13\uFF14\uFF15\uFF16\uFF17\uFF18\uFF19",  // 26
    L"\uC601\uC77C\uC774\uC0BC\uC0AC\uC624\uC721\uCE60\uD314\uAD6C",  // 27
};

}  // namespace

// ---- Compiling ----

struct Compiler {
  NumberFormat& f;
  std::wstring_view code;

  void Push(Section& s, Token::Kind kind, wchar_t ch, size_t a, size_t b) {
    s.tokens.push_back({kind, ch, static_cast<uint32_t>(a), static_cast<uint32_t>(b)});
  }

  void Run() {
    Section s;
    size_t i = 0;
    for (;;) {
      if (i >= code.size() || code[i] == ';') {
        Finish(s);
        if (f.sections_.size() < 4) f.sections_.push_back(std::move(s));
        s = Section{};
        if (i >= code.size()) break;
        ++i;
        continue;
      }
      i = Next(s, i);
    }
  }

  size_t Next(Section& s, size_t i) {
    const size_t n = code.size();
    const wchar_t c = code[i];
    switch (c) {
      case '"': {
        size_t e = code.find('"', i + 1);
        if (e == std::wstring_view::npos) e = n;
        Push(s, Token::Literal, 0, i + 1, e - i - 1);
        return std::min(e + 1, n);
      }
      case '\\':
        if (i + 1 < n) Push(s, Token::Literal, 0, i + 1, 1);
        return i + 2;
      case '_':
      case '*':
        if (i + 1 < n) Push(s, c == '_' ? Token::Pad : Token::Fill, code[i + 1], i, 2);
        return i + 2;
      case '[':
        return Bracket(s, i);
      case '0':
      case '#':
      case '?':
        Push(s, Token::Digit, c, i, 1);
        return i + 1;
      case '.':
        Push(s, Token::Point, c, i, 1);
        return i + 1;
      case ',':
        Push(s, Token::Comma, c, i, 1);
        return i + 1;
      case '%':
        Push(s, Token::Percent, c, i, 1);
        return i + 1;
      case '@':
        Push(s, Token::At, c, i, 1);
        return i + 1;
      case '/':
        Push(s, Token::Slash, c, i, 1);
        return i + 1;
      case ':':
        Push(s, Token::Colon, c, i, 1);
        return i + 1;
      default:
        break;
    }
    const wchar_t lc = Lower(c);
    auto run = [&] {
      size_t e = i;
      while (e < n && Lower(code[e]) == lc) ++e;
      return e - i;
    };
    if ((lc == 'e') && i + 1 < n && (code[i + 1] == '+' || code[i + 1] == '-')) {
      Push(s, Token::Exp, c, i, 2);
      return i + 2;
    }
    if (lc == 'g' && StartsNoCase(code, i, L"general")) {
      Push(s, Token::General, 0, i, 7);
      return i + 7;
    }
    if (lc == 'a') {
      if (StartsNoCase(code, i, L"am/pm")) {
        Push(s, Token::AmPm, 0, i, 5);
        return i + 5;
      }
      if (i + 2 < n && code[i + 1] == '/' && Lower(code[i + 2]) == 'p') {
        Push(s, Token::AmPm, 'A', i, 3);
        return i + 3;
      }
    }
    if (lc == 'b' && i + 1 < n && (code[i + 1] == '1' || code[i + 1] == '2')) return i + 2;  // calendar
    Token::Kind kind = Token::Literal;
    switch (lc) {
      case 'y': kind = Token::Year; break;
      case 'e': kind = Token::Year; break;
      case 'b': kind = Token::Year; break;  // Buddhist: the year + 543
      case 'm': kind = Token::Month; break;
      case 'd': kind = Token::Day; break;
      case 'h': kind = Token::Hour; break;
      case 's': kind = Token::Second; break;
      case 'g': kind = Token::Era; break;
      default: break;
    }
    if (kind == Token::Literal) {
      Push(s, Token::Literal, 0, i, 1);
      return i + 1;
    }
    const size_t len = run();
    Push(s, kind, lc == 'e' || lc == 'b' ? lc : 0, i, len);
    return i + len;
  }

  size_t Bracket(Section& s, size_t i) {
    const size_t e = code.find(']', i + 1);
    if (e == std::wstring_view::npos) {
      Push(s, Token::Literal, 0, i, 1);
      return i + 1;
    }
    const std::wstring_view in = code.substr(i + 1, e - i - 1);
    if (in.empty()) return e + 1;
    const wchar_t c0 = in[0];
    if (c0 == '$') {
      const size_t dash = in.find('-');
      const size_t symEnd = dash == std::wstring_view::npos ? in.size() : dash;
      if (symEnd > 1) Push(s, Token::Literal, 0, i + 2, symEnd - 1);
      if (dash != std::wstring_view::npos) {
        const std::wstring_view tag = in.substr(dash + 1);
        if (StartsNoCase(tag, 0, L"x-sysdate")) {
          s.system = 'D';
        } else if (StartsNoCase(tag, 0, L"x-systime")) {
          s.system = 'T';
        } else {
          uint32_t v = 0;
          size_t k = 0;
          for (; k < tag.size() && k < 8; ++k) {
            const wchar_t h = Lower(tag[k]);
            const int digit = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : -1;
            if (digit < 0) break;
            v = v * 16 + static_cast<uint32_t>(digit);
          }
          // Hex ("409", "F800", "1010409") or a name ("en-US", "nl-NL"), as some files have.
          // The hex's high byte is the digits, the next the calendar.
          const size_t nameEnd = std::min(tag.find(','), tag.size());
          if (k < nameEnd) {
            std::wstring_view name = tag.substr(0, nameEnd);
            constexpr std::wstring_view kGannen = L"-x-gannen";  // Excel's formatCode16
            if (name.size() > kGannen.size() && StartsNoCase(name, name.size() - kGannen.size(), kGannen)) {
              s.gannen = true;
              name.remove_suffix(kGannen.size());
            }
            v = LcidFromName(name);
          }
          const uint32_t low = v & 0xFFFF;
          if (low == 0xF800) s.system = 'D';
          else if (low == 0xF400) s.system = 'T';
          else if (low) s.lcid = low;
          s.calendar = static_cast<uint8_t>(v >> 16);
          const uint32_t digits = v >> 24;  // the first section's, for the whole format
          if (f.sections_.empty() && digits < std::size(kDigitSystems) && kDigitSystems[digits])
            f.digits_ = static_cast<uint8_t>(digits);
        }
      }
      return e + 1;
    }
    if (c0 == '<' || c0 == '>' || c0 == '=') {
      char op = static_cast<char>(c0);
      size_t k = 1;
      if (in.size() > 1 && c0 == '<' && in[1] == '=') op = 'l', k = 2;
      else if (in.size() > 1 && c0 == '>' && in[1] == '=') op = 'g', k = 2;
      else if (in.size() > 1 && c0 == '<' && in[1] == '>') op = 'n', k = 2;
      char buf[64];
      size_t m = 0;
      for (; k < in.size() && m < sizeof buf - 1; ++k) {
        if (in[k] == ' ') continue;
        if (in[k] > 127) break;
        buf[m++] = static_cast<char>(in[k]);
      }
      double limit = 0;
      const auto r = std::from_chars(buf, buf + m, limit);
      if (r.ec == std::errc() && r.ptr == buf + m && m > 0) {
        s.op = op;
        s.limit = limit;
      }
      return e + 1;
    }
    // [h], [mm], [ss]: elapsed time.
    const wchar_t u = Lower(c0);
    if (u == 'h' || u == 'm' || u == 's') {
      bool same = true;
      for (wchar_t ch : in) same = same && Lower(ch) == u;
      if (same) {
        Push(s, Token::Elapsed, u, i, in.size() + 2);
        return e + 1;
      }
    }
    static const wchar_t* const kColors[] = {L"black", L"white", L"red", L"green", L"blue", L"yellow", L"magenta", L"cyan"};
    for (int k = 0; k < 8; ++k)
      if (in.size() == wcslen(kColors[k]) && StartsNoCase(in, 0, kColors[k])) {
        s.color = static_cast<uint8_t>(k + 1);
        return e + 1;
      }
    if (StartsNoCase(in, 0, L"color")) {
      int v = 0;
      size_t k = 5;
      while (k < in.size() && in[k] == ' ') ++k;
      const size_t start = k;
      for (; k < in.size() && in[k] >= '0' && in[k] <= '9' && v < 1000; ++k) v = v * 10 + (in[k] - '0');
      if (k == in.size() && k > start && v >= 1 && v <= 56) s.color = static_cast<uint8_t>(v);
      return e + 1;
    }
    return e + 1;  // [DBNum1], [NatNum1] and the like: digits stay as they are
  }

  static void ToLiteral(Token& t) {
    if (t.kind == Token::Pad || t.kind == Token::Fill || t.kind == Token::Literal) return;
    t.kind = Token::Literal;
  }
  static void Erase(Token& t) {
    t.kind = Token::Literal;
    t.b = 0;
  }

  // Thousands separators and scaling commas within [begin, end) of integer placeholders and the
  // decimals after `point` (or none).
  static void Commas(Section& s, size_t begin, size_t end, size_t point) {
    auto& t = s.tokens;
    auto digitIn = [&](size_t from, size_t to) {
      for (size_t k = from; k < to; ++k)
        if (t[k].kind == Token::Digit) return true;
      return false;
    };
    for (size_t k = begin; k < end; ++k) {
      if (t[k].kind != Token::Comma) continue;
      const bool inInt = k < point;
      const size_t partBegin = inInt ? begin : point + 1, partEnd = inInt ? std::min(point, end) : end;
      size_t prev = k;
      while (prev > partBegin && (t[prev - 1].kind == Token::Comma || (t[prev - 1].kind == Token::Literal && t[prev - 1].b == 0)))
        --prev;
      const bool afterDigit = prev > partBegin && t[prev - 1].kind == Token::Digit;
      if (inInt && digitIn(partBegin, k) && digitIn(k + 1, partEnd)) {
        s.group = true;
        Erase(t[k]);
      } else if (!inInt && digitIn(k + 1, partEnd)) {  // among the decimals: ignored (#.##0,00 is #.##000)
        Erase(t[k]);
      } else if (afterDigit && !digitIn(k + 1, partEnd)) {
        s.scale -= 3;
        Erase(t[k]);
      } else {
        ToLiteral(t[k]);
      }
    }
  }

  void Finish(Section& s) {
    auto& t = s.tokens;
    bool date = false, eYear = false, digits = false, at = false, general = false, exp = false;
    for (const Token& k : t) {
      switch (k.kind) {
        case Token::Year:
          (k.ch ? eYear : date) = true;  // e and b years and eras make a date when there are no digits
          break;
        case Token::Era:
          eYear = true;
          break;
        case Token::Month:
        case Token::Day:
        case Token::Hour:
        case Token::Second:
        case Token::Elapsed:
        case Token::AmPm:
          date = true;
          break;
        case Token::Digit: digits = true; break;
        case Token::At: at = true; break;
        case Token::General: general = true; break;
        case Token::Exp: exp = true; break;
        default: break;
      }
    }
    if (!date && eYear && !digits) date = true;
    if (s.system) date = true;
    if (t.empty() && (s.color || s.op || s.lcid)) {  // "[Red][<=100]": General (an empty section shows nothing)
      Push(s, Token::General, 0, 0, 0);
      general = true;
    }
    if (general) {
      s.kind = Section::General;
      for (Token& k : t)
        if (k.kind != Token::General && k.kind != Token::Percent) ToLiteral(k);
    } else if (date) {
      FinishDate(s);
    } else if (at) {
      s.kind = Section::Text;
      for (Token& k : t)
        if (k.kind != Token::At) ToLiteral(k);
    } else if (digits && exp) {
      FinishScientific(s);
    } else if (!(digits && FinishFraction(s))) {
      FinishNumber(s);
    }
  }

  void FinishNumber(Section& s) {
    s.kind = Section::Number;
    auto& t = s.tokens;
    size_t point = t.size();
    for (size_t k = 0; k < t.size(); ++k) {
      Token& x = t[k];
      if (x.kind == Token::Point) {
        if (point == t.size()) point = k, s.hasPoint = true;
        else x.ch = 0;  // another point: the separator again, the decimals go on (0.0.0 shows 1234.5.7)
      } else if (x.kind == Token::Digit) {
        (point == t.size() ? s.intDigits : s.fracDigits)++;
      } else if (x.kind == Token::Percent) {
        s.scale += 2;
      } else if (x.kind != Token::Comma) {
        ToLiteral(x);
      }
    }
    Commas(s, 0, t.size(), point);
  }

  void FinishScientific(Section& s) {
    s.kind = Section::Scientific;
    auto& t = s.tokens;
    size_t xi = t.size(), point = t.size(), firstInt = t.size();
    bool comma = false;
    for (size_t k = 0; k < t.size(); ++k) {
      Token& x = t[k];
      if (x.kind == Token::Exp) {
        if (xi == t.size()) xi = k;
        else ToLiteral(x);
      } else if (x.kind == Token::Point && xi == t.size()) {
        if (point == t.size()) point = k, s.hasPoint = true;
        else ToLiteral(x);
      } else if (x.kind == Token::Digit) {
        if (k > xi) {
          s.expDigits++;
        } else if (point == t.size()) {
          if (comma) s.group = true;
          if (!s.intDigits++) firstInt = k;
        } else {
          s.fracDigits++;
        }
      } else if (x.kind == Token::Percent) {
        // Shown, but in scientific notation it doesn't multiply (measured: 0.5 shows 5.0E-1%).
      } else if (x.kind == Token::Comma && k < xi) {
        comma = comma || (s.intDigits && point == t.size());
        Erase(x);
      } else {
        ToLiteral(x);
      }
    }
    // A grouped mantissa has at least four places (measured: 0,0E+00 is #,#00E+00, so 1234.5 shows
    // 1,235E+00 and 7 shows 07E+00).
    if (s.group && s.intDigits < 4) {
      Token pad{};
      pad.kind = Token::Digit;
      pad.ch = '#';
      t.insert(t.begin() + static_cast<std::ptrdiff_t>(firstInt), static_cast<size_t>(4 - s.intDigits), pad);
      s.intDigits = 4;
    }
  }

  bool FinishFraction(Section& s) {
    auto& t = s.tokens;
    auto digitLiteral = [&](const Token& x) {
      return x.kind == Token::Literal && x.b == 1 && code[x.a] >= '1' && code[x.a] <= '9';
    };
    // Text may stand between the numerator and the bar and between the bar and the denominator
    // (measured: # ?_)/? shows " 3 /4", |#\:#=/=#| "|23:3=/=4|").
    auto between = [&](const Token& x) {
      return (x.kind == Token::Literal && !digitLiteral(x)) || x.kind == Token::Pad || x.kind == Token::Fill ||
             x.kind == Token::Percent;
    };
    size_t sl = 0, ne = 0, k = 0;
    for (; sl < t.size(); ++sl) {
      if (t[sl].kind != Token::Slash) continue;
      for (ne = sl; ne > 0 && between(t[ne - 1]);) --ne;
      for (k = sl + 1; k < t.size() && between(t[k]);) ++k;
      if (ne > 0 && t[ne - 1].kind == Token::Digit && k < t.size() && (t[k].kind == Token::Digit || digitLiteral(t[k])))
        break;
    }
    if (sl == t.size()) return false;
    s.kind = Section::Fraction;
    // The numerator is the placeholders right before the bar; any before them are the integer's,
    // with the text among them (|#\:? ?=/=#| shows 23.75 as "|2:3 3=/=4|").
    size_t ns = ne;
    while (ns > 0 && t[ns - 1].kind == Token::Digit) --ns;
    s.numDigits = static_cast<int>(ne - ns);
    size_t ie = 0;
    for (size_t j = 0; j < ns; ++j)
      if (t[j].kind == Token::Digit) ie = j + 1, s.intDigits++;
    if (digitLiteral(t[k])) {
      uint64_t den = 0;
      const size_t first = k;
      while (k < t.size() && (digitLiteral(t[k]) || (t[k].kind == Token::Digit && t[k].ch == '0'))) {
        if (den < 100000000) den = den * 10 + (t[k].kind == Token::Digit ? 0 : code[t[k].a] - '0');
        if (k > first) Erase(t[k]);
        ++k;
      }
      t[first] = {Token::FixedDen, 0, static_cast<uint32_t>(den), 0};
      s.fixedDen = static_cast<uint32_t>(den);
    } else {
      while (k < t.size() && t[k].kind == Token::Digit) ++k, s.denDigits++;
    }
    s.intEnd = static_cast<uint32_t>(ie);
    s.numStart = static_cast<uint32_t>(ns);
    s.slash = static_cast<uint32_t>(sl);
    s.denEnd = static_cast<uint32_t>(k);
    for (size_t j = 0; j < t.size(); ++j) {
      Token& x = t[j];
      if (x.kind == Token::Percent) s.scale += 2;
      else if (x.kind == Token::Comma && j >= ie) ToLiteral(x);
      else if (x.kind == Token::Slash && j != sl) ToLiteral(x);
      else if (x.kind != Token::Digit && x.kind != Token::Comma && x.kind != Token::Slash &&
               x.kind != Token::FixedDen && x.kind != Token::Percent)
        ToLiteral(x);
    }
    Commas(s, 0, ie, ie);
    return true;
  }

  void FinishDate(Section& s) {
    s.kind = Section::DateTime;
    auto& t = s.tokens;
    auto prevDate = [&](size_t k) -> const Token* {
      while (k-- > 0)
        if (IsDateToken(t[k].kind) && t[k].kind != Token::Era) return &t[k];
      return nullptr;
    };
    auto nextDate = [&](size_t k) -> const Token* {
      for (++k; k < t.size(); ++k)
        if (IsDateToken(t[k].kind) && t[k].kind != Token::Era) return &t[k];
      return nullptr;
    };
    // m is minutes after an hour or before a second, and after an earlier second while no minute
    // has come yet (measured: ss:mm, s m and ss d m show minutes; mm:ss m and ss m mm the month).
    bool second = false, minute = false;
    for (size_t k = 0; k < t.size(); ++k) {
      Token& x = t[k];
      if (x.kind == Token::Month && x.b <= 2) {
        const Token* p = prevDate(k);
        const Token* q = nextDate(k);
        if ((p && (p->kind == Token::Hour || (p->kind == Token::Elapsed && p->ch == 'h'))) ||
            (q && (q->kind == Token::Second || (q->kind == Token::Elapsed && q->ch == 's'))) || (second && !minute))
          x.kind = Token::Minute;
      }
      second = second || x.kind == Token::Second || (x.kind == Token::Elapsed && x.ch == 's');
      minute = minute || x.kind == Token::Minute || (x.kind == Token::Elapsed && x.ch == 'm');
    }
    for (size_t k = 0; k < t.size(); ++k) {
      Token& x = t[k];
      if (x.kind == Token::Point) {
        size_t z = k + 1;
        while (z < t.size() && t[z].kind == Token::Digit && t[z].ch == '0') ++z;
        const size_t zeros = z - k - 1;
        if (zeros == 0) {
          ToLiteral(x);
          continue;
        }
        const size_t used = std::min<size_t>(zeros, 3);
        x.kind = Token::FracSec;
        x.b = static_cast<uint32_t>(used);
        for (size_t j = k + 1; j < k + 1 + used; ++j) Erase(t[j]);
        for (size_t j = k + 1 + used; j < z; ++j) ToLiteral(t[j]);
        s.secDigits = std::max(s.secDigits, static_cast<int>(used));
        k = z - 1;
      } else if (x.kind == Token::AmPm) {
        s.ampm = true;
      } else if (!IsDateToken(x.kind) && x.kind != Token::Slash && x.kind != Token::Colon && x.kind != Token::Percent) {
        ToLiteral(x);
      }
    }
    int units = 0, elapsed = 0;
    for (const Token& x : t) {
      if (x.kind == Token::Elapsed) ++elapsed;
      else if (IsDateToken(x.kind) && x.kind != Token::Era) ++units;
    }
    s.elapsedOnly = elapsed == 1 && units == 0 && !s.system;
    if (!elapsed) return;
    // With an elapsed code, the largest time unit is the elapsed one, each of its codes, and a
    // smaller elapsed one is plain (measured: [h] hh shows 75 75 for 3 days 3 hours, [s] ss 271433
    // 271433, [m] h:mm 23 75:23).
    auto rank = [](const Token& x) {
      if (x.kind == Token::Elapsed) return x.ch == 'h' ? 2 : x.ch == 'm' ? 1 : 0;
      return x.kind == Token::Hour ? 2 : x.kind == Token::Minute ? 1 : x.kind == Token::Second ? 0 : -1;
    };
    int largest = -1;
    for (const Token& x : t) largest = std::max(largest, rank(x));
    const wchar_t unit = largest == 2 ? L'h' : largest == 1 ? L'm' : L's';
    for (Token& x : t) {
      if (x.kind != Token::Elapsed && rank(x) == largest) {
        x.kind = Token::Elapsed;
        x.ch = unit;
        x.b = x.b >= 2 ? 4 : 3;
      } else if (x.kind == Token::Elapsed && rank(x) < largest) {
        x.kind = rank(x) == 1 ? Token::Minute : Token::Second;
        x.b = std::min<uint32_t>(x.b - 2, 2);
      }
    }
    // An elapsed code after another date code (not its own unit's) shows Windows Excel's odd count,
    // marked by an upper-case unit (Formatter::DateTime).
    bool other = false;
    for (Token& x : t) {
      if (x.kind == Token::Elapsed && other) x.ch = static_cast<wchar_t>(x.ch - 32);
      other = other || (IsDateToken(x.kind) && x.kind != Token::Era && x.kind != Token::Elapsed);
    }
  }
};

NumberFormat::NumberFormat() {
  sections_.emplace_back();
  sections_[0].kind = Section::General;
  sections_[0].tokens.push_back({Token::General, 0, 0, 0});
}

NumberFormat::NumberFormat(std::wstring_view code) : pool_(code) {
  general_ = false;
  if (code.empty()) {
    *this = NumberFormat();
    return;
  }
  Compiler{*this, pool_}.Run();
  const Section& s = sections_[0];
  general_ = sections_.size() == 1 && s.kind == Section::General && s.tokens.size() == 1 && !s.color && !s.op &&
             !digits_;
}

bool NumberFormat::IsDateTime() const { return !sections_.empty() && sections_[0].kind == Section::DateTime; }

bool NumberFormat::IsText() const { return !sections_.empty() && sections_[0].kind == Section::Text; }

// ---- Formatting ----

struct Formatter {
  const NumberFormat& f;
  const FormatContext& ctx;
  const NumLocale& loc;
  Formatted& out;
  std::string digits;  // scratch
  size_t digitsShown = 0;
  bool ascii = false;  // Digit() writes ASCII whatever the digit system

  std::wstring_view Pool(const Token& t) const {
    return std::wstring_view(f.pool_).substr(std::min<size_t>(t.a, f.pool_.size()), t.b);
  }
  void Ch(wchar_t c) { out.text += c; }
  void Str(std::wstring_view s) { out.text += s; }
  // A digit the number shows, in the format's digit system (a literal "5" stays ASCII).
  void Digit(char d) {
    ++digitsShown;
    const bool mapped = f.digits_ && !ascii && d >= '0' && d <= '9';
    out.text += mapped ? kDigitSystems[f.digits_][d - '0'] : static_cast<wchar_t>(d);
  }

  bool Common(const Token& t) {
    switch (t.kind) {
      case Token::Literal:
        Str(Pool(t));
        return true;
      case Token::Pad:
        out.pads.push_back({static_cast<uint32_t>(out.text.size()), t.ch});
        Ch(L' ');
        return true;
      case Token::Fill:
        if (out.fill < 0) {
          out.fill = static_cast<int>(out.text.size());
          out.fillChar = t.ch;
        }
        return true;
      case Token::Percent:
        Ch(L'%');
        return true;
      case Token::Slash:
        Ch(L'/');
        return true;
      case Token::Colon:
        Ch(L':');
        return true;
      default:
        return false;
    }
  }

  bool Fits() const { return !ctx.fits || ctx.fits(ctx.fitsContext, out); }

  void Overflow() {
    out.text.clear();
    out.pads.clear();
    out.fill = 0;
    out.fillChar = L'#';
    out.overflow = true;
    out.generalAt = out.generalLength = 0;
  }

  // Integer digits right-aligned in `n` placeholders: placeholder p (0 = leftmost) of character
  // `ch`; the first takes any digits beyond n. `I` holds L digits.
  void IntDigit(int p, int n, wchar_t ch, const char* I, int L, bool group) {
    const int r = n - 1 - p;
    auto emit = [&](char d, int pos) {
      Digit(d);
      if (group && pos > 0 && pos % 3 == 0) Str(loc.group);
    };
    if (p == 0)
      for (int pos = L - 1; pos > r; --pos) emit(I[L - 1 - pos], pos);
    if (r < L) {
      emit(I[L - 1 - r], r);
    } else if (ch == '0') {
      emit('0', r);
    } else if (ch == '?') {
      Ch(L' ');
      if (group && r > 0 && r % 3 == 0) Ch(L' ');
    }
  }

  // ---- General ----

  // General in at most w characters (no sign): false when it can't be.
  bool General(double v, int w) {
    Decimal x = ToDecimal(v);
    if (!x.n) {
      Digit('0');
      return true;
    }
    auto emitFixed = [&](int L) {
      size_t end = digits.size();
      while (end > static_cast<size_t>(L) && digits[end - 1] == '0') --end;
      if (L == 0) Digit('0');
      for (int k = 0; k < L; ++k) Digit(digits[k]);
      if (end > static_cast<size_t>(L)) {
        Str(loc.decimal);
        for (size_t k = static_cast<size_t>(L); k < end; ++k) Digit(digits[k]);
      }
    };
    auto expLength = [](int e) { return std::max(2, e >= 100 || e <= -100 ? 3 : 2); };
    if (x.e >= 0) {
      if (x.e + 1 <= w) {
        const int L = Fixed(x, std::max(0, w - (x.e + 1) - 1), digits);
        if (L <= w) {
          emitFixed(L);
          return true;
        }
      }
    } else if (w >= 3) {
      const int dec = w - 2;
      const int L = Fixed(x, dec, digits);
      const bool nonzero = !AllZero(digits);
      const bool exact = x.e + dec + 1 >= x.n;
      const int shown = dec + x.e + 1;
      const int sciSig = std::max(0, w - 4 - expLength(x.e)) + 1;
      if (nonzero && (exact || shown >= sciSig)) {
        emitFixed(L);
        return true;
      }
    } else {
      const int L = Fixed(x, 0, digits);
      Digit(L ? '1' : '0');
      return true;
    }
    for (int pass = 0; pass < 2; ++pass) {
      const int expDigits = expLength(x.e);
      int sd = w - 4 - expDigits;
      if (sd < 0) {
        if (w < 3 + expDigits) return false;
        sd = 0;
      }
      Decimal m = x;
      m.e = 0;
      const int L = Fixed(m, sd, digits);
      if (L > 1) {  // rounding carried (9.999995 became 10.00000): one digit more in the exponent
        const int e = x.e + 1;
        x = Decimal{};
        x.d[0] = '1', x.n = 1, x.e = e;
        continue;
      }
      emitFixed(1);
      Ch(L'E');
      Ch(x.e < 0 ? L'-' : L'+');
      const int ae = std::abs(x.e);
      if (ae < 10) Digit('0');
      for (char c : std::to_string(ae)) Digit(c);
      return true;
    }
    return false;
  }

  // ---- Sections ----

  // The value's section: its index, whether its sign is left out, or -1 with `general` (one
  // conditional section the value fails: General) or without (no section: "####").
  int Choose(double v, bool& absolute, bool& general) const {
    absolute = general = false;
    const auto& secs = f.sections_;
    const int count = static_cast<int>(secs.size());
    const int textIx = count >= 4 ? 3 : secs[count - 1].kind == Section::Text ? count - 1 : -1;
    const int n = textIx >= 0 ? textIx : count;
    if (n == 0) {
      general = true;
      return -1;
    }
    auto match = [&](const Section& s) {
      switch (s.op) {
        case '<': return v < s.limit;
        case '>': return v > s.limit;
        case '=': return v == s.limit;
        case 'l': return v <= s.limit;
        case 'g': return v >= s.limit;
        case 'n': return v != s.limit;
        default: return true;
      }
    };
    // Only negative values can meet it: the section shows them without a sign.
    auto negativeOnly = [](const Section& s) {
      return (s.op == '<' && s.limit <= 0) || ((s.op == 'l' || s.op == '=') && s.limit < 0);
    };
    const bool c1 = secs[0].op != 0, c2 = n >= 2 && secs[1].op != 0;
    if (!c1 && !c2) {
      if (n == 1) return 0;
      if (n == 2) return v >= 0 ? 0 : (absolute = true, 1);
      return v > 0 ? 0 : v < 0 ? (absolute = true, 1) : 2;
    }
    if (c1 && !c2) {
      if (match(secs[0])) return absolute = negativeOnly(secs[0]), 0;
      // What's left goes on without its sign, unless the first section's condition is "=" or a
      // limit above zero (as measured; it isn't symmetric).
      const bool fallbackAbs = secs[0].op != '=' && secs[0].limit <= 0;
      if (n == 1) {
        if (textIx >= 0) return -1;
        general = true;
        absolute = fallbackAbs;
        return -1;
      }
      if (n == 2) return absolute = fallbackAbs, 1;
      return v < 0 ? (absolute = true, 1) : 2;
    }
    if (c1 ? match(secs[0]) : v > 0) return absolute = c1 && negativeOnly(secs[0]), 0;
    if (match(secs[1])) return absolute = negativeOnly(secs[1]), 1;
    if (n >= 3) return 2;
    return -1;
  }

  void Value(double v) {
    if (!std::isfinite(v)) return Overflow();
    if (std::fabs(v) < DBL_MIN) v = 0;
    bool absolute = false, general = false;
    const int ix = f.general_ ? -1 : Choose(v, absolute, general);
    if (f.general_) general = true;
    if (absolute) v = std::fabs(v);
    if (ix < 0) {
      if (!general) return Overflow();
      for (int w = 11; w >= 1; --w) {
        out.text.clear();
        if (v < 0) Ch(L'-');
        const bool made = General(v, w);
        out.generalAt = 0, out.generalLength = static_cast<uint32_t>(out.text.size());
        if (made && Fits()) return;
      }
      return Overflow();
    }
    const Section& s = f.sections_[ix];
    out.color = s.color;
    if (s.kind == Section::General) {
      for (int w = 11; w >= 1; --w) {
        out.text.clear();
        out.pads.clear();
        out.fill = -1;
        out.generalAt = out.generalLength = 0;
        if (GeneralSection(s, v, w) && Fits()) return;
      }
      return Overflow();
    }
    switch (s.kind) {
      case Section::Number: Number(s, v); break;
      case Section::Scientific: Scientific(s, v); break;
      case Section::Fraction: Fraction(s, v); break;
      case Section::DateTime: DateTime(s, v); break;
      case Section::Text: TextAsNumber(s, v); break;
      default: break;
    }
    if (!out.overflow && !Fits()) Overflow();
  }

  bool GeneralSection(const Section& s, double v, int w) {
    if (v < 0) Ch(L'-');
    for (const Token& t : s.tokens)
      if (t.kind == Token::General) {
        // The minus belongs to General's number when nothing comes between them.
        const size_t at = out.text.size() == 1 && v < 0 ? 0 : out.text.size();
        if (!General(v, w)) return false;
        out.generalAt = static_cast<uint32_t>(at), out.generalLength = static_cast<uint32_t>(out.text.size() - at);
      } else {
        Common(t);
      }
    return true;
  }

  void TextAsNumber(const Section& s, double v) {
    for (const Token& t : s.tokens)
      if (t.kind == Token::At) {
        if (v < 0) Ch(L'-');
        General(v, 11);
      } else {
        Common(t);
      }
  }

  void Number(const Section& s, double v) {
    Decimal x = ToDecimal(v);
    if (x.n) x.e += s.scale;
    const int L = Fixed(x, s.fracDigits, digits);
    if (v < 0 && !AllZero(digits)) Ch(L'-');
    const std::string_view I(digits.data(), static_cast<size_t>(L));
    const std::string_view F = std::string_view(digits).substr(static_cast<size_t>(L));
    const int lastNonZero = static_cast<int>(F.find_last_not_of('0'));  // -1: none
    int p = 0, q = 0;
    bool past = false;
    for (const Token& t : s.tokens) {
      if (Common(t)) continue;
      if (t.kind == Token::Digit) {
        if (!past) {
          IntDigit(p++, s.intDigits, t.ch, I.data(), L, s.group);
        } else {
          if (q < static_cast<int>(F.size()) && q <= lastNonZero) Digit(F[q]);
          else if (t.ch == '0') Digit('0');
          else if (t.ch == '?') Ch(L' ');
          ++q;
        }
      } else if (t.kind == Token::Point) {
        if (s.intDigits == 0 && t.ch)
          for (int k = 0; k < L; ++k) Digit(I[k]);
        Str(loc.decimal);
        past = true;
      }
    }
  }

  void Scientific(const Section& s, double v) {
    Decimal x = ToDecimal(v);
    if (x.n) x.e += s.scale;
    const int ni = s.intDigits;
    int ex = 0, L = 0;
    if (!x.n) {
      L = Fixed(x, s.fracDigits, digits);
    } else {
      for (int pass = 0; pass < 2; ++pass) {
        ex = ni > 0 ? FloorDiv(x.e, ni) * ni : x.e + 1;
        Decimal m = x;
        m.e = x.e - ex;
        L = Fixed(m, s.fracDigits, digits);
        if (L <= std::max(m.e + 1, 0)) break;
        const int e = x.e + 1;  // rounding carried: 9.99 became 10.0
        x = Decimal{};
        x.d[0] = '1', x.n = 1, x.e = e;
      }
    }
    if (v < 0 && x.n) Ch(L'-');
    const std::string_view I(digits.data(), static_cast<size_t>(L));
    const std::string_view F = std::string_view(digits).substr(static_cast<size_t>(L));
    const int lastNonZero = static_cast<int>(F.find_last_not_of('0'));
    const std::string E = std::to_string(std::abs(ex));
    int p = 0, q = 0, r = 0, phase = 0;
    for (const Token& t : s.tokens) {
      if (Common(t)) continue;
      if (t.kind == Token::Digit) {
        if (phase == 0) {
          if (!x.n) IntDigit(p, ni, '0', "", 0, s.group);  // zero shows every integer placeholder as 0
          else IntDigit(p, ni, t.ch, I.data(), L, s.group);
          ++p;
        } else if (phase == 1) {
          if (q < static_cast<int>(F.size()) && q <= lastNonZero) Digit(F[q]);
          else if (t.ch == '0') Digit('0');
          else if (t.ch == '?') Ch(L' ');
          ++q;
        } else {
          IntDigit(r++, s.expDigits, t.ch, E.data(), static_cast<int>(E.size()), false);
        }
      } else if (t.kind == Token::Point) {
        Str(loc.decimal);
        phase = 1;
      } else if (t.kind == Token::Exp) {
        Ch(t.ch);
        if (ex < 0) Ch(L'-');
        else if (t.a < f.pool_.size() && f.pool_[t.a + 1] == '+') Ch(L'+');
        phase = 2;
      }
    }
  }

  void Fraction(const Section& s, double v) {
    double a = std::fabs(v);
    if (s.scale) a *= std::pow(10.0, s.scale);
    const bool mixed = s.intDigits > 0;
    double ip = 0, num = 0, den = 1;
    if (s.fixedDen) {
      den = s.fixedDen;
      if (mixed) {
        ip = std::floor(a);
        num = std::floor((a - ip) * den + 0.5);
        if (num >= den) ip += 1, num -= den;
      } else {
        num = std::floor(a * den + 0.5);
        if (num > 32767) return Overflow();  // measured: ?/2 shows 32767/2, and #### from 16384
      }
    } else {
      const double maxDen = std::pow(10.0, std::min(s.denDigits, 9)) - 1;
      double P, Q;
      Convergent(a, std::max(1.0, maxDen), P, Q);
      den = Q;
      if (mixed) {
        ip = std::floor(P / Q);
        num = P - ip * Q;
        if (num < 0) ip -= 1, num += Q;
        if (num >= Q) ip += 1, num -= Q;
      } else {
        num = P;
      }
    }
    if (v < 0) Ch(L'-');
    const auto& t = s.tokens;
    bool numZero = false, anyQ = false;
    int denZeros = 0;  // the denominator's leading 0 placeholders
    bool denLead = true;
    for (uint32_t k = 0; k < s.denEnd && k < t.size(); ++k) {
      if (t[k].kind != Token::Digit) continue;
      if (k >= s.numStart && k < s.slash && t[k].ch == '0') numZero = true;
      if (t[k].ch == '?') anyQ = true;
      if (k > s.slash) {
        denLead = denLead && t[k].ch == '0';
        if (denLead) ++denZeros;
      }
    }
    const wchar_t firstNum = s.numStart < t.size() ? t[s.numStart].ch : L'#';
    const wchar_t lastInt = mixed && s.intEnd > 0 ? t[s.intEnd - 1].ch : L'#';
    const bool blank = mixed && num == 0 && !numZero;
    // A zero integer shows "0" when the fraction is blank, or when the integer ends in ? (measured:
    // ?\:0/? shows 0 as "0:0/1", #\:0/? as "0/1").
    const bool zero = ip == 0 && (blank || (num == 0 && lastInt == '?'));
    const std::string I = zero ? std::string("0") : ip == 0 ? std::string() : IntDigits(ip);
    const std::string N = IntDigits(num), D = IntDigits(den);
    // A short denominator fills its leading 0 placeholders first (measured: ?/00 shows 1/02, ?/?0 3/40).
    const int dpad = std::clamp(s.denDigits - static_cast<int>(D.size()), 0, denZeros);
    int p = 0, np = 0, dp = 0;
    bool intShown = false, intVisible = false;
    for (uint32_t k = 0; k < t.size(); ++k) {
      const Token& x = t[k];
      if (blank && k >= s.intEnd && k < s.denEnd) {
        if (!anyQ) continue;
        if (x.kind == Token::Literal) out.text.append(x.b, L' ');
        else if (x.kind == Token::Digit || x.kind == Token::Slash || x.kind == Token::Pad || x.kind == Token::Percent) Ch(L' ');
        else if (x.kind == Token::FixedDen) out.text.append(D.size(), L' ');
        continue;
      }
      if (mixed && k >= s.intEnd && k < s.numStart) {
        // Text between the integer and the numerator: dropped when the integer shows no digit, or
        // blanked when either side's placeholder is ? (#\:#/# shows 0.75 as "3/4", ?\:#/# "  3/4").
        if (!intVisible) {
          if (firstNum != '?' && lastInt != '?') continue;
          if (x.kind == Token::Literal) out.text.append(x.b, L' ');
          else if (x.kind == Token::Pad || x.kind == Token::Percent) Ch(L' ');
          continue;
        }
        Common(x);
        continue;
      }
      if (x.kind == Token::Digit) {
        if (k < s.intEnd) {
          const size_t before = digitsShown;
          ascii = zero && blank;  // that "0" is ASCII in any digit system ([$-3000401]# ?/? shows 0 as "0")
          IntDigit(p++, s.intDigits, x.ch, I.data(), static_cast<int>(I.size()), s.group);
          ascii = false;
          intShown = intShown || digitsShown > before;
          if (k + 1 == s.intEnd) intVisible = intShown;
        } else if (k < s.slash) {
          IntDigit(np++, s.numDigits, x.ch, N.data(), static_cast<int>(N.size()), false);
        } else if (k >= s.denEnd) {
          // Placeholders after the denominator show as unfilled ones (# ?/? ? shows " 3/4  ").
          if (x.ch == '0') Digit('0');
          else if (x.ch == '?') Ch(L' ');
        } else {
          const int at = dp - dpad;
          if (dp < dpad) {
            Digit('0');
          } else if (at < static_cast<int>(D.size())) {
            if (dp == s.denDigits - 1)
              for (size_t c = static_cast<size_t>(at); c < D.size(); ++c) Digit(D[c]);
            else
              Digit(D[static_cast<size_t>(at)]);
          } else if (x.ch == '0') {
            Digit('0');
          } else if (x.ch == '?') {
            Ch(L' ');
          }
          ++dp;
        }
      } else if (x.kind == Token::FixedDen) {
        for (char c : D) Digit(c);
      } else if (x.kind == Token::Slash && k == s.slash) {
        Ch(L'/');
      } else {
        Common(x);
      }
    }
  }

  void DateTime(const Section& s, double v) {
    if (s.system) {
      NumberFormat sys(s.system == 'D' ? loc.longDate : loc.time);
      sys.localAmPm_ = true;
      if (sys.sections_.empty() || sys.sections_[0].kind != Section::DateTime) return Overflow();
      Section copy = sys.sections_[0];
      copy.system = 0;
      Formatter inner{sys, ctx, loc, out, {}};
      return inner.DateTime(copy, v);
    }
    // Negative dates: "####", except in the 1904 system, which shows "-" and the date of the size.
    const bool negative = v < 0;
    if (negative && !s.elapsedOnly && !ctx.date1904) return Overflow();
    const double a = std::fabs(v);
    const double perSecond = std::pow(10.0, s.secDigits);
    const double total = RoundProduct(a, 86400.0 * perSecond);  // in 10^-secDigits seconds
    if (!std::isfinite(total)) return Overflow();
    const double perDay = 86400.0 * perSecond;
    const double day = std::floor(total / perDay);
    if (!s.elapsedOnly && day >= (ctx.date1904 ? kMaxSerial1904 : kMaxSerial1900)) return Overflow();
    const double inDay = total - day * perDay;
    const double secs = std::floor(inDay / perSecond);
    const int hour = static_cast<int>(secs / 3600), minute = static_cast<int>(std::fmod(secs, 3600) / 60),
              second = static_cast<int>(std::fmod(secs, 60));
    const double frac = inDay - secs * perSecond;
    int y = 0, mo = 0, d = 0, dow = 0;
    const bool needDate = !s.elapsedOnly;
    if (needDate) {
      const int64_t serial = static_cast<int64_t>(day);
      DateOfDay(serial, ctx.date1904, y, mo, d);
      dow = static_cast<int>((serial + (ctx.date1904 ? 5 : 6)) % 7);
    }
    const NumLocale* names = &loc;
    if (s.lcid && loc.language)
      if (const NumLocale* lang = loc.language(s.lcid)) names = lang;
    if (negative) Ch(L'-');
    auto num = [&](double value, int width) {
      const std::string d = IntDigits(value);
      for (int k = static_cast<int>(d.size()); k < width; ++k) Digit('0');
      for (char c : d) Digit(c);
    };
    // Japan's eras (measured): g shows the era (R, 令, 令和 for g, gg, ggg) and e its year (ee: two
    // digits) in Japanese or the Japanese calendar ([$-30409]); the calendar's y is that year in two
    // digits too, and Japanese's y after a g. Elsewhere g shows nothing and e is the year.
    const bool japan = s.calendar == 3 || (s.lcid & 0x3FF) == 0x11;
    const JapaneseEra* era = &kJapaneseEras[0];
    bool kanji = false, afterEra = false;
    if (japan) {
      for (const JapaneseEra& x : kJapaneseEras)
        if (y * 10000 + mo * 100 + d >= x.start) era = &x;
      for (const Token& t : s.tokens) kanji = kanji || (t.kind == Token::Era && t.b >= 2);
    }
    auto eraYear = [&](int width) {
      const int ey = y - era->start / 10000 + 1;
      if (ey == 1 && s.gannen && kanji) Ch(L'\u5143');  // 元年: the first year, with the era in kanji
      else num(ey, width);
    };
    for (const Token& t : s.tokens) {
      switch (t.kind) {
        case Token::Year:
          if (japan && (t.ch == 'e' || (!t.ch && (s.calendar == 3 || afterEra)))) {
            eraYear(t.ch == 'e' && t.b < 2 ? 1 : 2);
          } else if (t.ch == 'b') {
            const int by = s.lcid ? y : y + 543;  // a language tag shows the year itself
            num(t.b > 2 ? by : by % 100, t.b > 2 ? 1 : 2);
          } else if (t.ch == 'e' || t.b > 2) {
            num(y, 1);
          } else {
            num(y % 100, 2);
          }
          break;
        case Token::Month:
          if (t.b <= 2) num(mo, static_cast<int>(t.b));
          else if (mo >= 1 && mo <= 12) {
            const std::wstring& full = names->month[static_cast<size_t>(mo - 1)];
            if (t.b == 3) Str(names->monthAbbr[static_cast<size_t>(mo - 1)]);
            else if (t.b == 5) Str(std::wstring_view(full).substr(0, 1));
            else Str(full);
          }
          break;
        case Token::Day:
          if (t.b <= 2) num(d, static_cast<int>(t.b));
          else Str(t.b == 3 ? names->dayAbbr[static_cast<size_t>(dow)] : names->day[static_cast<size_t>(dow)]);
          break;
        case Token::Hour: {
          int h = hour;
          if (s.ampm) h = h % 12 == 0 ? 12 : h % 12;
          num(h, t.b >= 2 ? 2 : 1);
          break;
        }
        case Token::Minute: num(minute, t.b >= 2 ? 2 : 1); break;
        case Token::Second: num(second, t.b >= 2 ? 2 : 1); break;
        case Token::Elapsed: {
          const wchar_t u = Lower(t.ch);
          const double unit = u == 'h' ? 3600 : u == 'm' ? 60 : 1;
          const int width = static_cast<int>(t.b > 2 ? t.b - 2 : 1);
          if (t.ch == u) {
            num(std::floor(std::floor(total / perSecond) / unit), width);
          } else {
            // After another date code, Windows Excel shows the seconds field before rounding (in the
            // shown decimals' unit) plus a half, read as days (measured: s [h] shows 3.14159 as
            // "53 1293", (53.376 + 0.5) * 24; Mac Excel shows 75).
            const double field = std::round((a - std::floor(a)) * 86400.0 * perSecond * 1e6) / 1e6;
            const double x = std::fmod(field + 0.5, s.secDigits ? perSecond : 60.0);
            num(std::floor(x * 86400.0 / unit + 1e-9), width);
          }
          break;
        }
        case Token::AmPm: {
          const bool pm = hour >= 12;
          if (t.ch) {
            Ch(f.pool_[pm ? t.a + 2 : t.a]);
          } else if (f.localAmPm_) {
            Str(pm ? loc.pm : loc.am);
          } else if (s.lcid && EastAsian(s.lcid) && names != &loc && !names->am.empty()) {
            Str(pm ? names->pm : names->am);
          } else {
            Str(pm ? kEnPm : kEnAm);
          }
          break;
        }
        case Token::FracSec: {
          Str(loc.decimal);
          std::wstring fd = std::to_wstring(static_cast<long long>(frac));
          if (static_cast<int>(fd.size()) < s.secDigits) fd.insert(0, static_cast<size_t>(s.secDigits) - fd.size(), L'0');
          for (wchar_t c : std::wstring_view(fd).substr(0, t.b)) Digit(static_cast<char>(c));
          break;
        }
        case Token::Slash: Str(loc.dateSep); break;
        case Token::Colon: Str(loc.timeSep); break;
        case Token::Era:
          if (japan) {
            if (t.b == 1) Ch(era->letter);
            else if (t.b == 2) Ch(era->name[0]);
            else Str(era->name);
            afterEra = true;
          }
          break;
        default: Common(t); break;
      }
    }
  }

  void Text(std::wstring_view text) {
    const auto& secs = f.sections_;
    const int count = static_cast<int>(secs.size());
    const int ix = count >= 4 ? 3 : secs[count - 1].kind == Section::Text ? count - 1 : -1;
    if (ix < 0) {
      Str(text);
      return;
    }
    const Section& s = secs[ix];
    out.color = s.color;
    for (const Token& t : s.tokens) {
      if (t.kind == Token::At) Str(text);
      else if (!Common(t) && t.b) Str(Pool(t));
    }
  }
};

void NumberFormat::Format(double value, const FormatContext& context, Formatted& out) const {
  out.Clear();
  Formatter fm{*this, context, context.locale ? *context.locale : EnUsLocale(), out, {}};
  fm.Value(value);
}

void NumberFormat::Format(std::wstring_view text, const FormatContext& context, Formatted& out) const {
  out.Clear();
  Formatter fm{*this, context, context.locale ? *context.locale : EnUsLocale(), out, {}};
  fm.Text(text);
}

std::wstring FormatGeneral(double value, int width, const NumLocale& locale) {
  Formatted out;
  FormatContext ctx;
  ctx.locale = &locale;
  NumberFormat general;
  Formatter fm{general, ctx, locale, out, {}};
  if (!std::isfinite(value)) return {};
  if (std::fabs(value) < DBL_MIN) value = 0;
  if (value < 0) fm.Ch(L'-');
  return fm.General(value, width) ? out.text : std::wstring();
}

// ---- Built-in formats ----

namespace {

// Excel's built-in ids beyond the common ones show as one of 0..49 (measured: 27..36 and 50..58 are
// East Asian dates and times, 59..81 Thai digits of the common ones).
int BuiltinBase(int id) {
  if (id >= 0 && id <= 22) return id;
  if (id >= 23 && id <= 26) return 0;
  if ((id >= 27 && id <= 31) || id == 36 || (id >= 50 && id <= 58)) return 14;
  if (id >= 32 && id <= 35) return 21;
  if (id >= 37 && id <= 49) return id;
  static const int kThai[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 14, 15, 16, 17, 20, 21, 22, 45, 46, 47};
  if (id >= 59 && id <= 81) return kThai[id - 59];
  return 0;
}

// --- Codes as the region types them (TEXT(), measured on en-NL) ---

bool Localized(const NumLocale& l) {
  return l.decimal != L"." || l.group != L"," || l.yearCode != L'y' || l.dayCode != L'd' || l.hourCode != L'h' ||
         l.generalName != L"General";
}

bool At(std::wstring_view s, size_t i, std::wstring_view w) { return !w.empty() && s.substr(i, w.size()) == w; }

bool AtNoCase(std::wstring_view s, size_t i, std::wstring_view w) {
  if (w.empty() || i + w.size() > s.size()) return false;
  for (size_t k = 0; k < w.size(); ++k)
    if (Lower(s[i + k]) != Lower(w[k])) return false;
  return true;
}

// The end of a quoted string, an escape, a pad or fill and its character, or a bracket at i; i
// when there is none.
size_t SkipPart(std::wstring_view s, size_t i) {
  const wchar_t c = s[i];
  if (c == '"') {
    const size_t e = s.find('"', i + 1);
    return e == std::wstring_view::npos ? s.size() : e + 1;
  }
  if (c == '\\' || c == '_' || c == '*') return std::min(i + 2, s.size());
  if (c == '[') {
    const size_t e = s.find(']', i + 1);
    return e == std::wstring_view::npos ? s.size() : e + 1;
  }
  return i;
}

// A bracket's letter when it is elapsed time ([uu], [mm], [ss]); 0 otherwise.
wchar_t ElapsedLetter(std::wstring_view in) {
  if (in.empty()) return 0;
  const wchar_t c = Lower(in[0]);
  for (wchar_t x : in)
    if (Lower(x) != c) return 0;
  return c;
}

// A section is a date or time when it has one of the region's codes (a digit placeholder in it
// then makes Excel refuse the code).
bool LocalDateSection(std::wstring_view s, const NumLocale& l, wchar_t year, wchar_t day, wchar_t hour) {
  for (size_t i = 0; i < s.size();) {
    const size_t e = SkipPart(s, i);
    if (e != i) {
      if (s[i] == '[' && e - i >= 3) {
        const wchar_t c = ElapsedLetter(s.substr(i + 1, e - i - 2));
        if (c && (c == hour || c == 'm' || c == 's')) return true;
      }
      i = e;
      continue;
    }
    if (AtNoCase(s, i, l.generalName)) {
      i += l.generalName.size();
      continue;
    }
    if (AtNoCase(s, i, L"am/pm")) return true;
    if (s[i] == 'E' && i + 1 < s.size() && (s[i + 1] == '+' || s[i + 1] == '-')) {
      i += 2;
      continue;
    }
    const wchar_t c = Lower(s[i]);
    if (c == year || c == day || c == hour || c == 'm' || c == 's' || c == 'e' || c == 'b' || c == 'g') return true;
    ++i;
  }
  return false;
}

// A bracket's content: elapsed hours in the region's letter, a condition's number with its decimal
// separator (Excel ignores what follows a thousands separator there), anything else as it is.
bool LocalBracket(std::wstring_view in, const NumLocale& l, wchar_t hour, std::wstring& out) {
  const wchar_t c = ElapsedLetter(in);
  if (c == hour) {
    out.append(in.size(), L'h');
    return true;
  }
  if (c == 'h') return false;  // [h] is no code here
  if (!in.empty() && (in[0] == '<' || in[0] == '>' || in[0] == '=')) {
    for (size_t k = 0; k < in.size();) {
      if (At(in, k, l.decimal)) {
        out += L'.';
        k += l.decimal.size();
      } else if (At(in, k, l.group)) {
        break;
      } else {
        out += in[k++];
      }
    }
    return true;
  }
  out.append(in);
  return true;
}

bool LocalSection(std::wstring_view s, const NumLocale& l, std::wstring& out) {
  const wchar_t year = Lower(l.yearCode), day = Lower(l.dayCode), hour = Lower(l.hourCode);
  const bool date = LocalDateSection(s, l, year, day, hour);
  bool afterCode = false, decimals = false;  // dates: a decimal separator after a code starts seconds' decimals
  bool point = false, fraction = false;      // numbers
  for (size_t i = 0; i < s.size();) {
    const wchar_t c = s[i];
    const size_t e = SkipPart(s, i);
    if (e != i) {
      afterCode = decimals = false;
      if (c == '[' && s[e - 1] == ']') {
        const std::wstring_view in = s.substr(i + 1, e - i - 2);
        out += L'[';
        if (!LocalBracket(in, l, hour, out)) return false;
        out += L']';
        afterCode = ElapsedLetter(in) != 0;
      } else {
        out.append(s.substr(i, e - i));
      }
      i = e;
      continue;
    }
    if (AtNoCase(s, i, l.generalName)) {
      out += L"General";
      i += l.generalName.size();
      continue;
    }
    if (AtNoCase(s, i, L"general")) return false;  // English's name isn't the region's
    const wchar_t lc = Lower(c);
    if (date) {
      const bool wasCode = afterCode;
      afterCode = false;
      if (AtNoCase(s, i, L"am/pm")) {
        out += L"AM/PM";
        i += 5;
        continue;
      }
      if (lc == 'a' && i + 2 < s.size() && s[i + 1] == '/' && Lower(s[i + 2]) == 'p') {  // A/P is no code here
        out += L'\\', out += c, out += L"/\\", out += s[i + 2];
        i += 3;
        continue;
      }
      if (lc == year || lc == day || lc == hour || lc == 'm' || lc == 's' || lc == 'e' || lc == 'b' || lc == 'g') {
        out += lc == year ? L'y' : lc == day ? L'd' : lc == hour ? L'h' : lc;
        afterCode = true;
        decimals = false;
        ++i;
        continue;
      }
      if (c == '0' && decimals) {
        out += c;
        afterCode = true;  // more decimals may follow
        ++i;
        continue;
      }
      if (c == '0' || c == '#' || c == '?') return false;
      if (wasCode && At(s, i, l.decimal) && i + l.decimal.size() < s.size() && s[i + l.decimal.size()] == '0') {
        out += L'.';
        i += l.decimal.size();
        afterCode = decimals = true;
        continue;
      }
      decimals = false;
      if (lc == 'y' || lc == 'd' || lc == 'h' || c == '.' || c == ',') {  // English codes and separators: text
        out += L'\\';
      }
      out += c;
      ++i;
      continue;
    }
    if (At(s, i, l.decimal)) {
      out += L'.';
      i += l.decimal.size();
      point = true;
      continue;
    }
    if (At(s, i, l.group)) {
      out += L',';
      i += l.group.size();
      continue;
    }
    if (c == '/' && i + 1 < s.size() && wcschr(L"0123456789#?", s[i + 1])) fraction = true;
    if (lc == 'y' || lc == 'd' || lc == 'h' || lc == 'm' || lc == 's' || c == '.' || c == ',') out += L'\\';
    out += c;
    ++i;
  }
  return !(point && fraction);  // decimals and a fraction: refused
}

}  // namespace

std::wstring BuiltinCode(int id, const NumLocale& locale) {
  const int base = BuiltinBase(id);
  return base == 0 ? std::wstring() : locale.builtin[static_cast<size_t>(base)];
}

NumberFormat NumberFormat::Builtin(int id, const NumLocale& locale) {
  const int base = BuiltinBase(id);
  if (base == 0) return NumberFormat();
  NumberFormat f(locale.builtin[static_cast<size_t>(base)]);
  f.localAmPm_ = base == 18 || base == 19;
  return f;
}

bool NumberFormat::LocalToInvariant(std::wstring_view code, const NumLocale& locale, std::wstring& out) {
  out.clear();
  if (!Localized(locale)) {
    out.assign(code);
    return true;
  }
  size_t start = 0;
  for (;;) {
    size_t end = start;
    while (end < code.size() && code[end] != ';') {
      const size_t e = SkipPart(code, end);
      end = e == end ? end + 1 : e;
    }
    if (!LocalSection(code.substr(start, end - start), locale, out)) {
      out.clear();
      return false;
    }
    if (end >= code.size()) return true;
    out += L';';
    start = end + 1;
  }
}

bool NumberFormat::FromLocal(std::wstring_view code, const NumLocale& locale, NumberFormat& out) {
  std::wstring invariant;
  if (!LocalToInvariant(code, locale, invariant)) return false;
  out = NumberFormat(invariant);
  // AM/PM typed in the region shows its designators ("pm" on en-NL); under a locale tag, English.
  out.localAmPm_ = std::none_of(out.sections_.begin(), out.sections_.end(), [](const Section& s) { return s.lcid != 0; });
  return true;
}

uint32_t PaletteColor(int index) {
  static const uint32_t kPalette[56] = {
      0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF, 0x800000, 0x008000, 0x000080,
      0x808000, 0x800080, 0x008080, 0xC0C0C0, 0x808080, 0x9999FF, 0x993366, 0xFFFFCC, 0xCCFFFF, 0x660066, 0xFF8080,
      0x0066CC, 0xCCCCFF, 0x000080, 0xFF00FF, 0xFFFF00, 0x00FFFF, 0x800080, 0x800000, 0x008080, 0x0000FF, 0x00CCFF,
      0xCCFFFF, 0xCCFFCC, 0xFFFF99, 0x99CCFF, 0xFF99CC, 0xCC99FF, 0xFFCC99, 0x3366FF, 0x33CCCC, 0x99CC00, 0xFFCC00,
      0xFF9900, 0xFF6600, 0x666699, 0x969696, 0x003366, 0x339966, 0x003300, 0x333300, 0x993300, 0x993366, 0x333399,
      0x333333};
  return index >= 1 && index <= 56 ? kPalette[index - 1] : 0;
}

bool SerialToDate(double serial, bool date1904, int& year, int& month, int& day) {
  if (!(serial >= 0) || serial >= (date1904 ? kMaxSerial1904 : kMaxSerial1900)) return false;
  DateOfDay(static_cast<int64_t>(serial), date1904, year, month, day);
  return true;
}

bool DateToSerial(int year, int month, int day, bool date1904, double& serial) {
  if (year < 0 || year > 9999 || month < -100000 || month > 100000) return false;
  int64_t y = year + FloorDiv(month - 1, 12);
  const int m = month - 1 - FloorDiv(month - 1, 12) * 12 + 1;
  const int64_t days = DaysFromCivil(y, m, 1) + day - 1;
  double s;
  if (date1904) {
    s = static_cast<double>(days - kDay1904_01_01);
  } else {
    s = static_cast<double>(days - kDay1899_12_31);
    if (s >= 60) s += 1;  // 1900-02-29, which Excel counts
  }
  if (s < 0 || s >= (date1904 ? kMaxSerial1904 : kMaxSerial1900)) return false;
  serial = s;
  return true;
}

}  // namespace fw::sheet
