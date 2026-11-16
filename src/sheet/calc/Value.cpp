#include "sheet/calc/Value.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace fw::sheet::calc {
namespace {

constexpr std::string_view kErrNames[] = {
    "",          "#NULL!",    "#DIV/0!",  "#VALUE!",   "#REF!",     "#NAME?",
    "#NUM!",     "#N/A",      "#GETTING_DATA", "#SPILL!", "#CALC!", "#FIELD!",
    "#BLOCKED!", "#CONNECT!", "#BUSY!",   "#UNKNOWN!", "#PYTHON!",
};

char Upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

std::string_view ErrText(Err e) {
  auto i = static_cast<size_t>(e);
  return i < std::size(kErrNames) ? kErrNames[i] : std::string_view("#VALUE!");
}

Err ParseErr(std::string_view s, size_t* len) {
  Err best = Err::None;
  size_t bestLen = 0;
  for (size_t i = 1; i < std::size(kErrNames); ++i) {
    std::string_view e = kErrNames[i];
    if (s.size() < e.size() || e.size() <= bestLen) continue;
    bool same = true;
    for (size_t k = 0; k < e.size() && same; ++k) same = Upper(s[k]) == e[k];
    if (same) {
      best = static_cast<Err>(i);
      bestLen = e.size();
    }
  }
  if (len) *len = bestLen;
  return best;
}

// --- Array ---

Array* Array::Make(int32_t rows, int32_t cols) {
  size_t n = static_cast<size_t>(rows) * static_cast<size_t>(cols);
  void* mem = ::operator new(sizeof(Array) + n * sizeof(Value));
  auto* a = new (mem) Array();
  a->rows_ = rows;
  a->cols_ = cols;
  Value* v = a->data();
  for (size_t i = 0; i < n; ++i) new (v + i) Value();
  return a;
}

void Array::Release(Array* a) {
  if (--a->refs_) return;
  Value* v = a->data();
  for (int32_t i = 0, n = a->size(); i < n; ++i) v[i].~Value();
  a->~Array();
  ::operator delete(a);
}

// --- Value ---

Value Value::Text(std::string_view s) {
  void* mem = ::operator new(sizeof(Str) + s.size() + 1);
  auto* str = static_cast<Str*>(mem);
  str->refs = 1;
  str->size = static_cast<uint32_t>(s.size());
  char* d = reinterpret_cast<char*>(str + 1);
  if (!s.empty()) std::memcpy(d, s.data(), s.size());
  d[s.size()] = '\0';
  Value v;
  v.t_ = Type::Text;
  v.s_ = str;
  return v;
}

Value Value::Refs(std::vector<Area> areas, bool dynamic) {
  if (areas.size() == 1) return Ref(areas[0], dynamic);
  Value v;
  v.t_ = Type::RefList;
  v.l_ = new RefList{1, std::move(areas)};
  v.dyn_ = dynamic;
  return v;
}

std::string_view Value::text() const {
  if (t_ != Type::Text) return {};
  return {s_->data(), s_->size};
}

void Value::Release() {
  switch (t_) {
    case Type::Text:
      if (--s_->refs == 0) ::operator delete(s_);
      break;
    case Type::Array:
      Array::Release(a_);
      break;
    case Type::RefList:
      if (--l_->refs == 0) delete l_;
      break;
    default:
      break;
  }
  t_ = Type::Empty;
}

void Value::CopyFrom(const Value& o) {
  t_ = o.t_;
  e_ = o.e_;
  dyn_ = o.dyn_;
  area_ = o.area_;
  switch (t_) {
    case Type::Text: ++s_->refs; break;
    case Type::Array: ++a_->refs_; break;
    case Type::RefList: ++l_->refs; break;
    default: break;
  }
}

// --- Numbers and text ---

double Round15(double d) {
  if (d == 0 || !std::isfinite(d)) return d;
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.14e", d);
  return std::strtod(buf, nullptr);
}

std::string NumberText(double d) {
  if (d == 0) return "0";
  if (!std::isfinite(d)) return "#NUM!";
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.14E", d);  // d.ddddddddddddddE+xxx: 15 significant digits
  const char* e = std::strchr(buf, 'E');
  int exp = std::atoi(e + 1);
  bool neg = buf[0] == '-';
  std::string digits;
  for (const char* p = buf + (neg ? 1 : 0); p < e; ++p)
    if (IsDigit(*p)) digits += *p;
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
  size_t n = digits.size();
  // Plain digits while they take at most 20 characters (the sign aside), else E notation:
  // Excel's General without a column width.
  std::string fixed;
  if (exp >= 0 && exp < 20) {
    size_t whole = static_cast<size_t>(exp) + 1;
    if (n <= whole) {
      fixed = digits + std::string(whole - n, '0');
    } else {
      fixed = digits.substr(0, whole) + "." + digits.substr(whole);
    }
  } else if (exp < 0 && exp > -20) {
    fixed = "0." + std::string(static_cast<size_t>(-exp - 1), '0') + digits;
  }
  std::string out = neg ? "-" : "";
  if (!fixed.empty() && fixed.size() <= 20) return out + fixed;
  out += digits[0];
  if (n > 1) {
    out += '.';
    out += digits.substr(1);
  }
  char eb[16];
  std::snprintf(eb, sizeof eb, "E%c%02d", exp < 0 ? '-' : '+', exp < 0 ? -exp : exp);
  return out + eb;
}

namespace {

int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {  // days since 1970-01-01
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void CivilFromDays(int64_t z, int& y, int& m, int& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(yy + (m <= 2));
}

const int64_t kEpoch1900 = DaysFromCivil(1899, 12, 31);  // serial 0 ("1900-01-00")
const int64_t kEpoch1900b = DaysFromCivil(1899, 12, 30); // from 1900-03-01 on, past the leap bug
const int64_t kEpoch1904 = DaysFromCivil(1904, 1, 1);
constexpr double kMaxSerial1900 = 2958465;  // 9999-12-31
constexpr double kMaxSerial1904 = 2957003;

}  // namespace

int DaysInMonth(int year, int month) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
  return kDays[(month - 1) % 12];
}

bool DateSerial(int year, int month, int day, bool date1904, double& serial) {
  // Months past 12 or below 1 roll the year, as DATE does.
  int64_t y = year, m = month - 1;
  y += m >= 0 ? m / 12 : -((11 - m) / 12);
  m = ((m % 12) + 12) % 12 + 1;
  if (y < 0 || y > 9999) return false;
  int64_t first;  // serial of the 1st of the month
  if (date1904) {
    first = DaysFromCivil(y, static_cast<unsigned>(m), 1) - kEpoch1904;
  } else if (y == 1900 && m <= 2) {
    first = DaysFromCivil(y, static_cast<unsigned>(m), 1) - kEpoch1900;
  } else {
    first = DaysFromCivil(y, static_cast<unsigned>(m), 1) - kEpoch1900b;
  }
  double s = static_cast<double>(first) + day - 1;
  if (s < 0 || s > (date1904 ? kMaxSerial1904 : kMaxSerial1900)) return false;
  serial = s;
  return true;
}

bool SerialDate(double serial, bool date1904, int& year, int& month, int& day) {
  if (!(serial >= 0) || serial >= (date1904 ? kMaxSerial1904 : kMaxSerial1900) + 1) return false;
  int64_t n = static_cast<int64_t>(std::floor(serial));
  if (date1904) {
    CivilFromDays(kEpoch1904 + n, year, month, day);
    return true;
  }
  if (n == 0) {
    year = 1900, month = 1, day = 0;
  } else if (n == 60) {
    year = 1900, month = 2, day = 29;
  } else if (n < 60) {
    CivilFromDays(kEpoch1900 + n, year, month, day);
  } else {
    CivilFromDays(kEpoch1900b + n, year, month, day);
  }
  return true;
}

namespace {

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

bool EqualNoCaseAscii(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (Upper(a[i]) != Upper(b[i])) return false;
  return true;
}

int MonthName(std::string_view w) {  // 1..12 for "jan" .. "december", else 0
  static const char* kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                                  "July",    "August",   "September", "October", "November", "December"};
  if (w.size() < 3) return 0;
  for (int i = 0; i < 12; ++i) {
    std::string_view full = kMonths[i];
    if (EqualNoCaseAscii(w, full) || EqualNoCaseAscii(w, full.substr(0, 3))) return i + 1;
    if (i == 8 && EqualNoCaseAscii(w, "Sept")) return 9;
  }
  return 0;
}

// A plain number: digits with optional thousands separators, decimals and exponent.
bool PlainNumber(std::string_view s, double& out) {
  if (s.empty() || s.size() > 300) return false;
  std::string buf;
  size_t i = 0, n = s.size();
  bool digits = false, point = false;
  size_t groupDigits = 0;
  bool grouped = false;
  for (; i < n; ++i) {
    char c = s[i];
    if (IsDigit(c)) {
      buf += c;
      digits = true;
      ++groupDigits;
    } else if (c == ',' && !point && digits) {
      if (grouped && groupDigits != 3) return false;
      grouped = true;
      groupDigits = 0;
    } else if (c == '.' && !point) {
      if (grouped && groupDigits != 3) return false;
      grouped = false;
      point = true;
      buf += c;
    } else {
      break;
    }
  }
  if (grouped && groupDigits != 3) return false;
  if (!digits) return false;
  if (i < n && (s[i] == 'e' || s[i] == 'E')) {
    buf += 'e';
    ++i;
    if (i < n && (s[i] == '+' || s[i] == '-')) buf += s[i++];
    size_t start = i;
    while (i < n && IsDigit(s[i])) buf += s[i++];
    if (i == start) return false;
  }
  if (i != n) return false;
  out = std::strtod(buf.c_str(), nullptr);
  return std::isfinite(out);
}

// "12:30", "12:30:45", "12:30:45.5", "1:30 PM", "1 PM" -> a fraction of a day. Hours run to 9999,
// minutes and seconds to 99 (they roll over: "1:99" is 2:39), as Excel reads them.
bool TimeOfDay(std::string_view s, double& out) {
  s = Trim(s);
  int ampm = 0;  // 1 AM, 2 PM
  size_t sp = s.rfind(' ');
  if (sp != std::string_view::npos) {
    std::string_view tail = s.substr(sp + 1);
    if (EqualNoCaseAscii(tail, "AM") || EqualNoCaseAscii(tail, "A")) ampm = 1;
    else if (EqualNoCaseAscii(tail, "PM") || EqualNoCaseAscii(tail, "P")) ampm = 2;
    if (ampm) s = Trim(s.substr(0, sp));
  }
  int parts[3] = {0, 0, 0};
  double frac = 0;
  int np = 0;
  size_t i = 0;
  while (i < s.size() && np < 3) {
    size_t start = i;
    int v = 0;
    while (i < s.size() && IsDigit(s[i]) && i - start < 5) v = v * 10 + (s[i++] - '0');
    if (i == start) return false;
    parts[np++] = v;
    if (i < s.size() && s[i] == ':') {
      ++i;
      continue;
    }
    if (i < s.size() && s[i] == '.' && np >= 2) {
      std::string f = "0.";
      ++i;
      size_t fs = i;
      while (i < s.size() && IsDigit(s[i])) f += s[i++];
      if (i == fs) return false;
      frac = std::strtod(f.c_str(), nullptr);
    }
    break;
  }
  if (i != s.size() || np == 0) return false;
  if (np == 1 && !ampm) return false;  // "12" alone is a number
  int h = parts[0], m = parts[1], sec = parts[2];
  if (m > 99 || sec > 99 || h > 9999) return false;
  if (ampm) {
    if (h > 12) return false;
    h %= 12;
    if (ampm == 2) h += 12;
  }
  out = (h * 3600.0 + m * 60.0 + sec + frac) / 86400.0;
  return true;
}

int CurrentYear() {
  SYSTEMTIME st;
  GetLocalTime(&st);
  return st.wYear;
}

// en-US dates: 1/2/2024, 1/2/24, 1/2, 2024-01-02, 2024/1/2, 2-Jan-2024, 2-Jan, Jan-24,
// Jan 2, 2024, January 2 2024, 2 January 2024.
bool DateOnly(std::string_view s, bool date1904, double& out) {
  s = Trim(s);
  // tokens: numbers and words
  struct Tok {
    bool num;
    int value;
    size_t digits;
    std::string_view text;
  };
  Tok toks[4];
  int nt = 0;
  char sep = 0;
  size_t i = 0;
  while (i < s.size()) {
    if (nt == 4) return false;
    char c = s[i];
    if (IsDigit(c)) {
      size_t start = i;
      int v = 0;
      while (i < s.size() && IsDigit(s[i]) && i - start < 5) v = v * 10 + (s[i++] - '0');
      if (i < s.size() && IsDigit(s[i])) return false;
      toks[nt++] = {true, v, i - start, s.substr(start, i - start)};
    } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
      size_t start = i;
      while (i < s.size() && ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z'))) ++i;
      if (i < s.size() && s[i] == '.') ++i;  // "Jan."
      std::string_view w = s.substr(start, i - start);
      if (!w.empty() && w.back() == '.') w.remove_suffix(1);
      int m = MonthName(w);
      if (!m) return false;
      toks[nt++] = {false, m, 0, w};
    } else {
      return false;
    }
    // separator
    size_t k = i;
    while (k < s.size() && s[k] == ' ') ++k;
    if (k < s.size() && (s[k] == '/' || s[k] == '-' || s[k] == ',')) {
      char d = s[k] == ',' ? ' ' : s[k];
      if (s[k] != ',') {
        if (sep && sep != d) return false;
        sep = d;
      }
      ++k;
      while (k < s.size() && s[k] == ' ') ++k;
    } else if (k == i && k < s.size()) {
      return false;  // "Jan2"
    }
    i = k;
  }
  auto year = [](const Tok& t) {
    if (t.digits <= 2) return t.value < 30 ? 2000 + t.value : 1900 + t.value;
    return t.value;
  };
  int y = 0, m = 0, d = 0;
  int words = 0;
  for (int k = 0; k < nt; ++k) words += toks[k].num ? 0 : 1;
  if (words > 1) return false;
  if (words == 0) {
    if (sep == 0) return false;
    if (nt == 3 && toks[0].digits == 4) {  // 2024-01-02, 2024/1/2
      y = toks[0].value, m = toks[1].value, d = toks[2].value;
    } else if (nt == 3) {  // 1/2/2024
      m = toks[0].value, d = toks[1].value, y = year(toks[2]);
    } else if (nt == 2) {  // 1/2 (this year), or 1/2024 (month/year)
      if (toks[1].digits >= 3 || toks[1].value > 31) {
        m = toks[0].value, d = 1, y = year(toks[1]);
      } else {
        m = toks[0].value, d = toks[1].value, y = CurrentYear();
      }
    } else {
      return false;
    }
  } else {
    int wi = 0;
    while (toks[wi].num) ++wi;
    m = toks[wi].value;
    if (nt == 3 && wi == 0) {  // Jan 2, 2024
      d = toks[1].value, y = year(toks[2]);
    } else if (nt == 3 && wi == 1) {  // 2-Jan-2024
      d = toks[0].value, y = year(toks[2]);
    } else if (nt == 2 && wi == 1) {  // 2-Jan
      d = toks[0].value, y = CurrentYear();
    } else if (nt == 2 && wi == 0) {  // Jan-24 (month and year), Jan 2 (this year)
      if (toks[1].digits >= 3 || toks[1].value > 31 || sep == '-') {
        d = 1, y = year(toks[1]);
      } else {
        d = toks[1].value, y = CurrentYear();
      }
    } else {
      return false;
    }
  }
  if (m < 1 || m > 12 || d < 1 || y < 1900 || y > 9999) return false;
  if (d > DaysInMonth(y, m) && !(y == 1900 && m == 2 && d == 29 && !date1904)) return false;
  return DateSerial(y, m, d, date1904, out);
}

}  // namespace

bool TextToNumber(std::string_view s, double& out, bool date1904) {
  s = Trim(s);
  if (s.empty()) return false;
  bool neg = false, pct = false, paren = false;
  std::string_view t = s;
  if (t.front() == '(' && t.back() == ')') {
    paren = true;
    t = Trim(t.substr(1, t.size() - 2));
  }
  if (!t.empty() && (t.front() == '-' || t.front() == '+')) {
    neg = t.front() == '-';
    t = Trim(t.substr(1));
  }
  if (!t.empty() && t.front() == '$') t = Trim(t.substr(1));
  if (!t.empty() && (t.front() == '-' || t.front() == '+') && !neg) {
    neg = t.front() == '-';
    t = Trim(t.substr(1));
  }
  if (!t.empty() && t.back() == '%') {
    pct = true;
    t = Trim(t.substr(0, t.size() - 1));
  } else if (!t.empty() && t.front() == '%') {  // "%5" is 5 % too
    pct = true;
    t = Trim(t.substr(1));
  }
  double v;
  if (PlainNumber(t, v)) {
    if (neg && paren) return false;
    if (pct) v /= 100;
    if (neg || paren) v = -v;
    out = v;
    return true;
  }
  if (neg || pct || paren || s.front() == '$') return false;
  // "1 1/2", "3/4": fractions (only when they aren't dates: a numerator and denominator)
  {
    size_t sp = s.find(' ');
    std::string_view whole, frac = s;
    if (sp != std::string_view::npos) {
      whole = s.substr(0, sp);
      frac = Trim(s.substr(sp + 1));
    }
    size_t slash = frac.find('/');
    double w = 0, a, b;
    if (!whole.empty() && slash != std::string_view::npos && PlainNumber(whole, w) &&
        PlainNumber(frac.substr(0, slash), a) && PlainNumber(frac.substr(slash + 1), b) && b != 0 &&
        whole.find_first_not_of("0123456789") == std::string_view::npos) {
      out = w + a / b;
      return true;
    }
  }
  // Dates, times, and a date with a time.
  if (TimeOfDay(s, v)) {
    out = v;
    return true;
  }
  if (DateOnly(s, date1904, v)) {
    out = v;
    return true;
  }
  // date then time: split at the last space before a digit that starts a time
  for (size_t k = s.size(); k-- > 1;) {
    if (s[k] != ' ') continue;
    double d, tm;
    if (DateOnly(s.substr(0, k), date1904, d) && TimeOfDay(s.substr(k + 1), tm)) {
      out = d + tm;
      return true;
    }
  }
  return false;
}

std::string ValueText(const Value& v) {
  switch (v.type()) {
    case Type::Number: return NumberText(v.number());
    case Type::Bool: return v.boolean() ? "TRUE" : "FALSE";
    case Type::Text: return std::string(v.text());
    case Type::Error: return std::string(ErrText(v.error()));
    default: return {};
  }
}

namespace {

// UTF-8 to UTF-16 into `buf` (resized).
void Widen(std::string_view s, std::wstring& buf) {
  buf.resize(s.size());
  bool ascii = true;
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0x80) {
      ascii = false;
      break;
    }
    buf[i] = c;
  }
  if (ascii) return;
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  buf.resize(static_cast<size_t>(n));
  if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), buf.data(), n);
}

}  // namespace

int CompareText(std::string_view a, std::string_view b) {
  thread_local std::wstring wa, wb;
  Widen(a, wa);
  Widen(b, wb);
  int r = CompareStringEx(L"en-US", NORM_IGNORECASE, wa.data(), static_cast<int>(wa.size()), wb.data(),
                          static_cast<int>(wb.size()), nullptr, nullptr, 0);
  if (r == 0) {  // shouldn't happen; fall back to ordinal
    r = CompareStringOrdinal(wa.data(), static_cast<int>(wa.size()), wb.data(), static_cast<int>(wb.size()), TRUE);
  }
  return r == CSTR_LESS_THAN ? -1 : r == CSTR_GREATER_THAN ? 1 : 0;
}

bool EqualText(std::string_view a, std::string_view b) {
  // Plain ASCII without control characters: case-insensitive equality is linguistic equality.
  bool plain = true;
  for (char c : a) plain = plain && static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F;
  for (char c : b) plain = plain && static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F;
  if (plain) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
      if (Upper(a[i]) != Upper(b[i])) return false;
    return true;
  }
  return CompareText(a, b) == 0;
}

namespace {

wchar_t UpperW(wchar_t c) {
  if (c < 0x80) return c >= 'a' && c <= 'z' ? static_cast<wchar_t>(c - 32) : c;
  wchar_t out = c;
  LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, &c, 1, &out, 1, nullptr, nullptr, 0);
  return out;
}

bool Match(const wchar_t* p, size_t pn, const wchar_t* t, size_t tn) {
  // Iterative with backtracking to the last '*'.
  size_t pi = 0, ti = 0, starP = SIZE_MAX, starT = 0;
  while (ti < tn) {
    if (pi < pn) {
      wchar_t pc = p[pi];
      if (pc == L'*') {
        starP = ++pi;
        starT = ti;
        continue;
      }
      bool lit = false;
      if (pc == L'~' && pi + 1 < pn && (p[pi + 1] == L'*' || p[pi + 1] == L'?' || p[pi + 1] == L'~')) {
        pc = p[pi + 1];
        lit = true;
      }
      if ((!lit && pc == L'?') || UpperW(pc) == UpperW(t[ti])) {
        pi += lit ? 2 : 1;
        ++ti;
        continue;
      }
    }
    if (starP == SIZE_MAX) return false;
    pi = starP;
    ti = ++starT;
  }
  while (pi < pn && p[pi] == L'*') ++pi;
  return pi == pn;
}

}  // namespace

bool WildcardMatch(std::string_view pattern, std::string_view text) {
  thread_local std::wstring wp, wt;
  Widen(pattern, wp);
  Widen(text, wt);
  return Match(wp.data(), wp.size(), wt.data(), wt.size());
}

}  // namespace fw::sheet::calc
