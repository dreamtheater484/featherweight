#include "sheet/numfmt/Parse.h"

#include <charconv>
#include <cmath>
#include <unordered_map>

#include "sheet/numfmt/NumFormat.h"

namespace fw::sheet {

namespace {

bool Digit(wchar_t c) { return c >= '0' && c <= '9'; }
wchar_t Lower(wchar_t c) { return c >= 'A' && c <= 'Z' ? wchar_t(c + 32) : c; }

bool StartsWith(std::wstring_view s, size_t i, std::wstring_view w) {
  return !w.empty() && i + w.size() <= s.size() && s.substr(i, w.size()) == w;
}

bool EqualNoCase(std::wstring_view a, std::wstring_view b) {
  if (a.size() != b.size()) return false;
  for (size_t k = 0; k < a.size(); ++k)
    if (Lower(a[k]) != Lower(b[k])) return false;
  return true;
}

std::wstring Quoted(std::wstring_view s) {
  if (s.empty()) return {};
  std::wstring out = L"\"";
  out += s;
  return out + L"\"";
}

// A literal character in a date or time code.
std::wstring Sep(wchar_t c, const std::wstring& localeSep, wchar_t codeChar) {
  if (localeSep.size() == 1 && c == localeSep[0]) return std::wstring(1, codeChar);
  if (c == '/' || c == ':' || c == ',' || c == '"' || c == '\\') return std::wstring{L'\\', c};
  return std::wstring(1, c);
}

// ---- Numbers ----

bool ParseNumber(std::wstring_view s, const NumLocale& loc, ParsedValue& out) {
  bool negative = false, paren = false, percent = false;
  std::wstring prefix, suffix;
  if (s.size() >= 2 && s.front() == '(' && s.back() == ')') {
    paren = true;
    s = s.substr(1, s.size() - 2);
  }
  if (!s.empty() && s[0] == '-' && !paren) {
    negative = true;
    s.remove_prefix(1);
  }
  if (!s.empty() && s.back() == '%') {
    percent = true;
    s.remove_suffix(1);
  }
  // A currency symbol before or after, with or without a space.
  const std::wstring_view symbols[] = {loc.currency, L"$", L"€", L"£", L"¥"};
  for (std::wstring_view sym : symbols) {
    if (sym.empty() || percent) continue;
    if (StartsWith(s, 0, sym)) {
      prefix = sym;
      s.remove_prefix(sym.size());
      if (!s.empty() && s[0] == ' ') prefix += L' ', s.remove_prefix(1);
      break;
    }
    if (s.size() > sym.size() && s.substr(s.size() - sym.size()) == sym) {
      suffix = sym;
      s.remove_suffix(sym.size());
      if (!s.empty() && s.back() == ' ') suffix.insert(0, L" "), s.remove_suffix(1);
      break;
    }
  }
  char buf[400];
  size_t m = 0;
  size_t i = 0;
  int intDigits = 0, group = 0, decimals = -1;
  bool grouped = false;
  while (i < s.size() && m < 330) {
    if (Digit(s[i])) {
      buf[m++] = static_cast<char>(s[i++]);
      ++intDigits;
      ++group;
    } else if (intDigits && StartsWith(s, i, loc.group) && i + loc.group.size() < s.size() &&
               Digit(s[i + loc.group.size()])) {
      if ((grouped && group != 3) || (!grouped && group > 3)) return false;
      grouped = true;
      group = 0;
      i += loc.group.size();
    } else {
      break;
    }
  }
  if (grouped && group != 3) return false;
  if (StartsWith(s, i, loc.decimal)) {
    i += loc.decimal.size();
    buf[m++] = '.';
    decimals = 0;
    while (i < s.size() && Digit(s[i]) && m < 380) buf[m++] = static_cast<char>(s[i++]), ++decimals;
  }
  if (intDigits == 0 && decimals <= 0) return false;
  int expDigits = 0;
  wchar_t expChar = 0, expSign = 0;
  if (i < s.size() && (s[i] == 'E' || s[i] == 'e')) {
    expChar = s[i++];
    buf[m++] = 'e';
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
      expSign = s[i];
      if (s[i] == '-') buf[m++] = '-';
      ++i;
    }
    while (i < s.size() && Digit(s[i]) && m < 395) buf[m++] = static_cast<char>(s[i++]), ++expDigits;
    if (!expDigits) return false;
  }
  if (i != s.size()) return false;
  if (percent) buf[m++] = 'e', buf[m++] = '-', buf[m++] = '2';  // exact: "33.3%" is 0.333
  double v = 0;
  if (percent && expChar) return false;
  const auto r = std::from_chars(buf, buf + m, v);
  if (r.ec != std::errc() || !std::isfinite(v)) return false;
  out.number = negative || paren ? -v : v;
  std::wstring num = intDigits == 0 ? L"" : grouped ? L"#,##0" : L"0";
  if (decimals >= 0) num += L"." + std::wstring(static_cast<size_t>(decimals), L'0');
  if (expChar) {
    num += expChar;
    num += expSign == '+' ? L'+' : L'-';
    num.append(static_cast<size_t>(expDigits), L'0');
  }
  if (percent) num += L'%';
  std::wstring code = Quoted(prefix) + num + Quoted(suffix);
  if (paren) code += L";(" + code + L")";
  out.code = std::move(code);
  out.plain = !paren && !percent && !grouped && !expChar && prefix.empty() && suffix.empty();
  return true;
}

// ---- Dates and times ----

struct Reader {
  std::wstring_view s;
  size_t i = 0;
  bool End() const { return i >= s.size(); }
  wchar_t Peek() const { return i < s.size() ? s[i] : 0; }
  // Digits: their value and count (up to 4).
  bool Number(int& value, int& digits) {
    value = digits = 0;
    while (i < s.size() && Digit(s[i]) && digits < 5) value = value * 10 + (s[i++] - '0'), ++digits;
    return digits > 0 && digits <= 4;
  }
  std::wstring_view Word() {
    const size_t b = i;
    while (i < s.size() && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || s[i] > 127)) ++i;
    return s.substr(b, i - b);
  }
};

// A month's name in the locale or in English: 1..12, and whether it is the abbreviation.
int MonthName(std::wstring_view w, const NumLocale& loc, bool& abbr) {
  static const NumLocale& en = EnUsLocale();
  for (const NumLocale* l : {&loc, &en})
    for (int k = 0; k < 12; ++k) {
      if (EqualNoCase(w, l->month[static_cast<size_t>(k)])) return abbr = false, k + 1;
      if (EqualNoCase(w, l->monthAbbr[static_cast<size_t>(k)])) return abbr = true, k + 1;
    }
  return 0;
}

int DaysIn(int y, int m) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0 || y == 1900))) return 29;  // 1900: as Excel
  return kDays[m - 1];
}

std::wstring Field(wchar_t letter, bool two) { return std::wstring(two ? 2 : 1, letter); }

// A full date: numbers in the locale's order (or year first with four digits), or a month's name.
bool ParseDate(Reader& rd, const NumLocale& loc, int& y, int& mo, int& d, std::wstring& code) {
  const size_t start = rd.i;
  int a, ad;
  if (rd.Number(a, ad)) {
    const wchar_t s1 = rd.Peek();
    if (s1 == '/' || s1 == '-' || s1 == '.' || (loc.dateSep.size() == 1 && s1 == loc.dateSep[0])) {
      ++rd.i;
      int b, bd, c, cd;
      if (rd.Number(b, bd)) {  // a s b s c
        if (rd.Peek() != s1) return rd.i = start, false;
        ++rd.i;
        if (!rd.Number(c, cd)) return rd.i = start, false;
        const char order = ad == 4 ? 'Y' : loc.dateOrder;
        int yv, yd;
        std::wstring parts[3];
        if (order == 'Y') {
          yv = a, yd = ad, mo = b, d = c;
          parts[0] = yd == 4 ? L"yyyy" : L"yy", parts[1] = Field('m', bd == 2 && cd == 2), parts[2] = Field('d', bd == 2 && cd == 2);
        } else if (order == 'D') {
          d = a, mo = b, yv = c, yd = cd;
          parts[0] = Field('d', ad == 2 && bd == 2), parts[1] = Field('m', ad == 2 && bd == 2), parts[2] = yd == 4 ? L"yyyy" : L"yy";
        } else {
          mo = a, d = b, yv = c, yd = cd;
          parts[0] = Field('m', ad == 2 && bd == 2), parts[1] = Field('d', ad == 2 && bd == 2), parts[2] = yd == 4 ? L"yyyy" : L"yy";
        }
        if (yd != 2 && yd != 4) return rd.i = start, false;
        y = yd == 2 ? (yv < 30 ? 2000 + yv : 1900 + yv) : yv;
        const std::wstring sep = Sep(s1, loc.dateSep, '/');
        code = parts[0] + sep + parts[1] + sep + parts[2];
        return true;
      }
      // d-Mon-yyyy
      bool abbr = false;
      const std::wstring_view w = rd.Word();
      mo = MonthName(w, loc, abbr);
      if (!mo || rd.Peek() != s1) return rd.i = start, false;
      ++rd.i;
      if (!rd.Number(c, cd) || (cd != 2 && cd != 4)) return rd.i = start, false;
      d = a;
      y = cd == 2 ? (c < 30 ? 2000 + c : 1900 + c) : c;
      const std::wstring sep = Sep(s1, loc.dateSep, '/');
      code = Field('d', ad == 2 && a < 10) + sep + (abbr ? L"mmm" : L"mmmm") + sep + (cd == 4 ? L"yyyy" : L"yy");
      return true;
    }
    if (s1 == ' ') {  // d Mon yyyy
      ++rd.i;
      bool abbr = false;
      mo = MonthName(rd.Word(), loc, abbr);
      int c, cd;
      if (!mo || rd.Peek() != ' ') return rd.i = start, false;
      ++rd.i;
      if (!rd.Number(c, cd) || (cd != 2 && cd != 4)) return rd.i = start, false;
      d = a;
      y = cd == 2 ? (c < 30 ? 2000 + c : 1900 + c) : c;
      code = Field('d', ad == 2 && a < 10) + L" " + (abbr ? L"mmm" : L"mmmm") + L" " + (cd == 4 ? L"yyyy" : L"yy");
      return true;
    }
    return rd.i = start, false;
  }
  // Mon d, yyyy
  bool abbr = false;
  mo = MonthName(rd.Word(), loc, abbr);
  if (!mo || rd.Peek() != ' ') return rd.i = start, false;
  ++rd.i;
  int b, bd, c, cd;
  if (!rd.Number(b, bd) || bd > 2) return rd.i = start, false;
  std::wstring between = L" ";
  if (rd.Peek() == ',') ++rd.i, between = L"\\, ";
  if (rd.Peek() != ' ') return rd.i = start, false;
  ++rd.i;
  if (!rd.Number(c, cd) || cd != 4) return rd.i = start, false;
  d = b, y = c;
  code = std::wstring(abbr ? L"mmm" : L"mmmm") + L" " + Field('d', bd == 2 && b < 10) + between + L"yyyy";
  return true;
}

// h:mm[:ss[.000]][ AM/PM]; two-digit minutes and seconds.
bool ParseTime(Reader& rd, const NumLocale& loc, double& fraction, std::wstring& code) {
  const size_t start = rd.i;
  int h, hd, mi, md, sec = 0, sd = 0;
  if (!rd.Number(h, hd) || hd > 2 || rd.Peek() != (loc.timeSep.size() == 1 ? loc.timeSep[0] : L':'))
    return rd.i = start, false;
  ++rd.i;
  if (!rd.Number(mi, md) || md != 2 || mi > 59) return rd.i = start, false;
  code = Field('h', hd == 2 && h < 10) + L":mm";
  double frac = 0;
  if (rd.Peek() == (loc.timeSep.size() == 1 ? loc.timeSep[0] : L':')) {
    ++rd.i;
    if (!rd.Number(sec, sd) || sd != 2 || sec > 59) return rd.i = start, false;
    code += L":ss";
    if (StartsWith(rd.s, rd.i, loc.decimal)) {
      rd.i += loc.decimal.size();
      int fd = 0;
      double scale = 0.1;
      while (rd.i < rd.s.size() && Digit(rd.s[rd.i]) && fd < 3) frac += (rd.s[rd.i++] - '0') * scale, scale /= 10, ++fd;
      if (!fd) return rd.i = start, false;
      code += L"." + std::wstring(static_cast<size_t>(fd), L'0');
    }
  }
  if (rd.Peek() == ' ') {
    const size_t save = rd.i++;
    const std::wstring_view w = rd.Word();
    const bool am = EqualNoCase(w, L"AM") || EqualNoCase(w, L"A"), pm = EqualNoCase(w, L"PM") || EqualNoCase(w, L"P");
    if (!am && !pm) {
      rd.i = save;
    } else {
      if (h < 1 || h > 12) return rd.i = start, false;
      h = h % 12 + (pm ? 12 : 0);
      code += w.size() == 1 ? (w[0] >= 'a' ? L" a/p" : L" A/P") : L" AM/PM";
    }
  }
  if (h > 23) return rd.i = start, false;
  fraction = (h * 3600 + mi * 60 + sec + frac) / 86400.0;
  return true;
}

bool ParseDateTime(std::wstring_view s, const NumLocale& loc, bool date1904, ParsedValue& out) {
  Reader rd{s};
  int y = 0, mo = 0, d = 0;
  std::wstring code;
  double serial = 0, fraction = 0;
  bool date = ParseDate(rd, loc, y, mo, d, code);
  if (date) {
    if (mo < 1 || mo > 12 || d < 1 || d > DaysIn(y, mo)) return false;
    if (y == 1900 && mo == 2 && d == 29) serial = date1904 ? -1 : 60;
    else if (!DateToSerial(y, mo, d, date1904, serial)) return false;
    if (serial < 0) return false;
    if (!rd.End()) {
      if (rd.Peek() != ' ') return false;
      ++rd.i;
      std::wstring timeCode;
      if (!ParseTime(rd, loc, fraction, timeCode)) return false;
      code += L" " + timeCode;
    }
  } else if (!ParseTime(rd, loc, fraction, code)) {
    return false;
  }
  if (!rd.End()) return false;
  out.number = serial + fraction;
  out.code = std::move(code);
  out.date = true;
  return true;
}

}  // namespace

bool ParseValue(std::wstring_view s, const NumLocale& loc, bool date1904, ParsedValue& out) {
  out = ParsedValue{};
  if (s.empty() || s.size() > 400) return false;
  if (EqualNoCase(s, L"TRUE") || EqualNoCase(s, L"FALSE")) {
    out.kind = ParsedValue::Bool;
    out.number = Lower(s[0]) == 't' ? 1 : 0;
    return true;
  }
  const wchar_t c = s[0];
  if (Digit(c) || c == '-' || c == '(' || c == '+' || StartsWith(s, 0, loc.decimal) || !Digit(s.back()) ||
      c > 127 || c == '$') {
    if (ParseNumber(s[0] == '+' ? s.substr(1) : s, loc, out)) {
      if (s[0] == '+') out.plain = false, out.code.clear();  // "+5" types as 5 but doesn't show so
      return true;
    }
  }
  return ParseDateTime(s, loc, date1904, out);
}

bool ParseExact(std::wstring_view s, const NumLocale& loc, bool date1904, ParsedValue& out) {
  if (!ParseValue(s, loc, date1904, out)) return false;
  if (out.kind == ParsedValue::Bool) return s == L"TRUE" || s == L"FALSE";
  if (s[0] == '+') return false;
  // Compiled formats, per thread (CSV parses in parallel).
  thread_local std::unordered_map<std::wstring, NumberFormat> cache;
  thread_local Formatted shown;
  FormatContext ctx;
  ctx.locale = &loc;
  ctx.date1904 = date1904;
  auto showsAs = [&](const std::wstring& code) {
    auto it = cache.find(code);
    if (it == cache.end()) {
      if (cache.size() > 512) cache.clear();
      it = cache.emplace(code, code.empty() ? NumberFormat() : NumberFormat(code)).first;
    }
    it->second.Format(out.number, ctx, shown);
    return !shown.overflow && shown.fill < 0 && shown.text == s;
  };
  if (out.plain && showsAs(std::wstring())) {
    out.code.clear();
    return true;
  }
  return showsAs(out.code);
}

}  // namespace fw::sheet
