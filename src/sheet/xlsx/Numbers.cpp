// How numbers are written in <v>: the few styles writers use, so the model can write a number back
// exactly as it was read without keeping its text (4 bits in the cell's record).
//  - Style 0, the default: the shortest digits that read back to the same number, in fixed
//    notation unless the exponent is below -4 or above 15, then 1e-05 / 1e+16 (Python's repr;
//    what openpyxl and many other writers produce; integers as plain digits).
//  - Styles 1-6: the shortest or 17 significant digits (Excel writes 4.4 as 4.4000000000000004
//    but 0.7 as 0.7), each in fixed notation, Excel's (1E-3, 1.5E+20) or C's (1e-03, 1.5e+20).
// Digits come from the C runtime's printf and strtod, which the app has anyway: std::to_chars for
// doubles would bring over 100 KB of tables into the exe.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sheet/xlsx/Internal.h"

namespace fw::sheet::xlsx {
namespace {

// |v| correctly rounded to p significant digits (1-17): the digits without trailing zeros, and the
// decimal exponent (v = d.ddd * 10^exp).
bool DigitsAt(double a, int p, char* digits, size_t& count, int& exp) {
  char buf[48];
  int n = std::snprintf(buf, sizeof buf, "%.*e", p - 1, a);
  if (n <= 0 || n >= static_cast<int>(sizeof buf)) return false;
  const char* e = std::strchr(buf, 'e');
  if (!e) return false;
  count = 0;
  for (const char* q = buf; q < e; ++q)
    if (*q >= '0' && *q <= '9') digits[count++] = *q;
  while (count > 1 && digits[count - 1] == '0') --count;
  exp = std::atoi(e + 1);
  return count > 0;
}

bool RoundTrips(double a, int p) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.*e", p - 1, a);
  return std::strtod(buf, nullptr) == a;
}

// The fewest significant digits that read back as `a` (more digits always do once fewer do).
int ShortestPrecision(double a) {
  int lo = 1, hi = 17;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (RoundTrips(a, mid)) hi = mid;
    else lo = mid + 1;
  }
  return lo;
}

enum Notation { kRepr, kFixed, kExcelSci, kCSci };

size_t Render(bool negative, const char* digits, size_t count, int exp, Notation n, char* out) {
  size_t len = 0;
  if (negative) out[len++] = '-';
  if (n == kRepr) n = exp < -4 || exp > 15 ? kCSci : kFixed;
  if (n == kFixed) {
    if (exp < 0) {
      if (exp < -330) return 0;
      out[len++] = '0';
      out[len++] = '.';
      for (int i = -1; i > exp; --i) out[len++] = '0';
      std::memcpy(out + len, digits, count);
      return len + count;
    }
    if (exp > 330) return 0;
    for (size_t i = 0; i < count || static_cast<int>(i) <= exp; ++i) {
      if (static_cast<int>(i) == exp + 1) out[len++] = '.';
      out[len++] = i < count ? digits[i] : '0';
    }
    return len;
  }
  out[len++] = digits[0];
  if (count > 1) {
    out[len++] = '.';
    std::memcpy(out + len, digits + 1, count - 1);
    len += count - 1;
  }
  out[len++] = n == kExcelSci ? 'E' : 'e';
  out[len++] = exp < 0 ? '-' : '+';
  int a = exp < 0 ? -exp : exp;
  if (n == kCSci && a < 10) out[len++] = '0';
  len += static_cast<size_t>(std::snprintf(out + len, 8, "%d", a));
  return len;
}

Notation NotationOf(uint8_t style) { return style == 0 ? kRepr : static_cast<Notation>(1 + (style - 1) % 3); }

}  // namespace

size_t FormatNumber(double v, uint8_t style, char* out) {
  if (!std::isfinite(v)) v = 0;
  if (v == 0) {
    size_t len = 0;
    if (std::signbit(v)) out[len++] = '-';
    out[len++] = '0';
    return len;
  }
  if (style > 6) style = 0;
  double a = std::fabs(v);
  char digits[24];
  size_t count = 0;
  int exp = 0;
  if (!DigitsAt(a, style >= 4 ? 17 : ShortestPrecision(a), digits, count, exp)) return 0;
  return Render(std::signbit(v), digits, count, exp, NotationOf(style), out);
}

bool NumberStyle(std::string_view text, double v, uint8_t& style) {
  if (!std::isfinite(v) || text.size() > 400) return false;
  char buf[400];
  auto same = [&](uint8_t s, const char* digits, size_t count, int exp) {
    size_t n = Render(std::signbit(v), digits, count, exp, NotationOf(s), buf);
    return n == text.size() && std::memcmp(buf, text.data(), n) == 0;
  };
  if (v == 0) {
    size_t n = FormatNumber(v, 0, buf);
    if (n == text.size() && std::memcmp(buf, text.data(), n) == 0) return style = 0, true;
    return false;
  }
  // The text's significant digits, and its notation.
  char textDigits[400];
  size_t k = 0;
  bool leading = true;
  for (char c : text) {
    if (c == 'e' || c == 'E') break;
    if (c < '0' || c > '9') continue;
    if (leading && c == '0') continue;
    leading = false;
    textDigits[k++] = c;
  }
  while (k > 1 && textDigits[k - 1] == '0') --k;
  if (k == 0 || k > 17) return false;
  Notation n = text.find('E') != std::string_view::npos   ? kExcelSci
               : text.find('e') != std::string_view::npos ? kCSci
                                                          : kFixed;
  double a = std::fabs(v);
  char digits[24];
  size_t count = 0;
  int exp = 0;
  // 17 digits, trailing zeros dropped (Excel's usual): the text's digits are those.
  auto seventeen = [&] {
    if (!DigitsAt(a, 17, digits, count, exp) || count != k || std::memcmp(digits, textDigits, k) != 0) return false;
    uint8_t s = static_cast<uint8_t>(4 + n - 1);
    return same(s, digits, count, exp) && (style = s, true);
  };
  // The shortest: the text's digits are the number correctly rounded, and one digit fewer doesn't
  // read back.
  auto shortest = [&] {
    if (!DigitsAt(a, static_cast<int>(k), digits, count, exp) || count != k || std::memcmp(digits, textDigits, k) != 0 ||
        (k > 1 && RoundTrips(a, static_cast<int>(k) - 1)))
      return false;
    if (same(0, digits, count, exp)) return style = 0, true;
    uint8_t s = static_cast<uint8_t>(1 + n - 1);
    return same(s, digits, count, exp) && (style = s, true);
  };
  return k == 17 ? seventeen() || shortest() : shortest() || seventeen();
}

}  // namespace fw::sheet::xlsx
