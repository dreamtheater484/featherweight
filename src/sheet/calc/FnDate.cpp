// Date and time functions, on serial numbers in the workbook's date system (1900, with its
// 29 February 1900, or 1904).

#include <algorithm>
#include <cmath>
#include <vector>

#include "sheet/calc/Fn.h"

namespace fw::sheet::calc::fn {
namespace {

bool IsLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

// Day of the week, 0 = Sunday.
int Dow(double serial, bool date1904) {
  int64_t n = static_cast<int64_t>(std::floor(serial));
  return static_cast<int>(((n + (date1904 ? 5 : 6)) % 7 + 7) % 7);
}

void TimeParts(double serial, int& h, int& m, int& s) {
  double frac = serial - std::floor(serial);
  int64_t secs = static_cast<int64_t>(std::floor(frac * 86400 + 0.5));
  if (secs >= 86400) secs -= 86400;
  h = static_cast<int>(secs / 3600);
  m = static_cast<int>(secs / 60 % 60);
  s = static_cast<int>(secs % 60);
}

// The weekend mask for NETWORKDAYS.INTL and WORKDAY.INTL: bit d set when day d (0 = Sunday) is off.
bool WeekendMask(Call& c, int i, int& mask) {
  mask = 1 << 0 | 1 << 6;  // Saturday and Sunday
  if (!c.Has(i)) return true;
  const Value& v = c.arg(i);
  if (v.IsText()) {
    std::string_view s = v.text();  // "0000011": Monday first, 1 = weekend
    if (s.size() != 7 || s == "1111111") return c.Fail(Err::Value), false;
    mask = 0;
    for (int k = 0; k < 7; ++k) {
      if (s[k] != '0' && s[k] != '1') return c.Fail(Err::Value), false;
      if (s[k] == '1') mask |= 1 << ((k + 1) % 7);
    }
    return true;
  }
  double n;
  if (!c.Number(i, n)) return false;
  int k = static_cast<int>(n);
  if (k >= 1 && k <= 7) {  // two days: 1 Sat+Sun, 2 Sun+Mon, ... 7 Fri+Sat
    int a = (k + 5) % 7;   // 1 -> 6 (Saturday)
    mask = 1 << a | 1 << ((a + 1) % 7);
  } else if (k >= 11 && k <= 17) {  // one day: 11 Sunday, 12 Monday, ... 17 Saturday
    mask = 1 << (k - 11);
  } else {
    c.Fail(Err::Num);
    return false;
  }
  return true;
}

bool Holidays(Call& c, int i, std::vector<int64_t>& out) {
  if (!c.Has(i)) return true;
  bool ok = EachItem(c.m, c.arg(i), [&](const CellView& v, From) {
    if (v.type == Type::Error) {
      c.Fail(v.err);
      return false;
    }
    double d;
    Err e = Err::None;
    if (v.type == Type::Empty) return true;
    if (!ToNumber(c.m, v, d, e)) {
      c.Fail(e);
      return false;
    }
    out.push_back(static_cast<int64_t>(std::floor(d)));
    return true;
  });
  if (!ok && !c.out.IsError()) c.Fail(Err::Value);
  std::sort(out.begin(), out.end());
  return ok;
}

bool Workday(int64_t day, int mask, const std::vector<int64_t>& holidays, bool date1904) {
  if (mask & (1 << Dow(static_cast<double>(day), date1904))) return false;
  return !std::binary_search(holidays.begin(), holidays.end(), day);
}

void NetDays(Call& c, int maskArg, int holidayArg) {
  double a, b;
  int mask;
  std::vector<int64_t> hol;
  if (!SerialArg(c, 0, a) || !SerialArg(c, 1, b) || !WeekendMask(c, maskArg, mask) || !Holidays(c, holidayArg, hol)) return;
  int64_t s = static_cast<int64_t>(std::floor(a)), e = static_cast<int64_t>(std::floor(b));
  int sign = 1;
  if (s > e) std::swap(s, e), sign = -1;
  int64_t n = 0;
  // Whole weeks at once, then the rest day by day.
  int perWeek = 0;
  for (int d = 0; d < 7; ++d) perWeek += (mask & (1 << d)) ? 0 : 1;
  int64_t weeks = (e - s + 1) / 7;
  n = weeks * perWeek;
  for (int64_t d = s + weeks * 7; d <= e; ++d) n += (mask & (1 << Dow(static_cast<double>(d), c.m.date1904))) ? 0 : 1;
  for (int64_t h : hol)
    if (h >= s && h <= e && !(mask & (1 << Dow(static_cast<double>(h), c.m.date1904)))) --n;
  c.SetNumber(static_cast<double>(sign * n));
}

void AddWorkdays(Call& c, int maskArg, int holidayArg) {
  double a, days;
  int mask;
  std::vector<int64_t> hol;
  if (!SerialArg(c, 0, a) || !c.Number(1, days) || !WeekendMask(c, maskArg, mask) || !Holidays(c, holidayArg, hol)) return;
  int64_t d = static_cast<int64_t>(std::floor(a));
  int64_t left = static_cast<int64_t>(std::trunc(days));
  int step = left < 0 ? -1 : 1;
  if (std::llabs(left) > 1000000) return c.Fail(Err::Num);
  while (left != 0) {
    d += step;
    if (Workday(d, mask, hol, c.m.date1904)) left -= step;
  }
  int y, m, dd;
  if (!SerialDate(static_cast<double>(d), c.m.date1904, y, m, dd)) return c.Fail(Err::Num);
  c.SetNumber(static_cast<double>(d));
}

}  // namespace

void Date(Call& c) {
  double y, m, d;
  if (!c.Integer(0, y) || !c.Integer(1, m) || !c.Integer(2, d)) return;
  if (y < 0 || y >= 10000) return c.Fail(Err::Num);
  if (y < 1900) y += 1900;
  if (std::fabs(m) > 1e6 || std::fabs(d) > 1e8) return c.Fail(Err::Num);
  double first;
  if (!DateSerial(static_cast<int>(y), static_cast<int>(m), 1, c.m.date1904, first)) return c.Fail(Err::Num);
  double s = first + d - 1;
  int yy, mm, dd;
  if (!SerialDate(s, c.m.date1904, yy, mm, dd)) return c.Fail(Err::Num);
  c.SetNumber(s);
}

static void DatePart(Call& c, int part) {
  double s;
  if (!SerialArg(c, 0, s)) return;
  int y, m, d;
  SerialDate(s, c.m.date1904, y, m, d);
  c.SetNumber(part == 0 ? y : part == 1 ? m : d);
}
void Year(Call& c) { DatePart(c, 0); }
void Month(Call& c) { DatePart(c, 1); }
void Day(Call& c) { DatePart(c, 2); }

static void TimePart(Call& c, int part) {
  double s;
  if (!c.Number(0, s)) return;
  if (s < 0) return c.Fail(Err::Num);
  int h, m, sec;
  TimeParts(s, h, m, sec);
  c.SetNumber(part == 0 ? h : part == 1 ? m : sec);
}
void Hour(Call& c) { TimePart(c, 0); }
void Minute(Call& c) { TimePart(c, 1); }
void Second(Call& c) { TimePart(c, 2); }

void Time(Call& c) {
  double h, m, s;
  if (!c.Integer(0, h) || !c.Integer(1, m) || !c.Integer(2, s)) return;
  double t = h * 3600 + m * 60 + s;
  if (t < 0 || h > 32767 || m > 32767 || s > 32767) return c.Fail(Err::Num);
  c.SetNumber(std::fmod(t, 86400) / 86400);
}

void Today(Call& c) { c.SetNumber(std::floor(c.m.host.Now())); }
void Now(Call& c) { c.SetNumber(c.m.host.Now()); }

static void MonthsLater(Call& c, bool end) {
  double s, n;
  if (!SerialArg(c, 0, s) || !c.Integer(1, n)) return;
  int y, m, d;
  SerialDate(s, c.m.date1904, y, m, d);
  if (std::fabs(n) > 120000) return c.Fail(Err::Num);
  int64_t months = static_cast<int64_t>(y) * 12 + (m - 1) + static_cast<int64_t>(n);
  int ny = static_cast<int>(months >= 0 ? months / 12 : -((11 - months) / 12));
  int nm = static_cast<int>(months - static_cast<int64_t>(ny) * 12) + 1;
  int dim = DaysInMonth(ny, nm);
  int nd = end ? dim : std::min(d, dim);
  double out;
  if (!DateSerial(ny, nm, nd, c.m.date1904, out)) return c.Fail(Err::Num);
  c.SetNumber(out);
}
void EDate(Call& c) { MonthsLater(c, false); }
void EOMonth(Call& c) { MonthsLater(c, true); }

void Weekday(Call& c) {
  double s, type;
  if (!SerialArg(c, 0, s) || !c.Integer(1, type, 1)) return;
  int d = Dow(s, c.m.date1904);  // 0 Sunday
  int t = static_cast<int>(type);
  if (t == 1) return c.SetNumber(d + 1);
  if (t == 2) return c.SetNumber((d + 6) % 7 + 1);
  if (t == 3) return c.SetNumber((d + 6) % 7);
  if (t >= 11 && t <= 17) {
    int first = (t - 10) % 7;  // 11 Monday (1), ... 17 Sunday (0)
    return c.SetNumber((d - first + 7) % 7 + 1);
  }
  c.Fail(Err::Num);
}

static int IsoWeek(int y, int m, int d, bool date1904) {
  double s;
  DateSerial(y, m, d, date1904, s);
  int dow = (Dow(s, date1904) + 6) % 7;  // 0 Monday
  double thursday = s - dow + 3;
  int ty, tm, td;
  SerialDate(thursday, date1904, ty, tm, td);
  double jan1;
  DateSerial(ty, 1, 1, date1904, jan1);
  return static_cast<int>((thursday - jan1) / 7) + 1;
}

void WeekNum(Call& c) {
  double s, type;
  if (!SerialArg(c, 0, s) || !c.Integer(1, type, 1)) return;
  int t = static_cast<int>(type);
  int y, m, d;
  SerialDate(s, c.m.date1904, y, m, d);
  if (t == 21) return c.SetNumber(IsoWeek(y, m, d, c.m.date1904));
  int start;  // first day of the week, 0 Sunday
  if (t == 1 || t == 17) start = 0;
  else if (t == 2 || t == 11) start = 1;
  else if (t >= 12 && t <= 16) start = t - 10;
  else return c.Fail(Err::Num);
  double jan1;
  DateSerial(y, 1, 1, c.m.date1904, jan1);
  int offset = (Dow(jan1, c.m.date1904) - start + 7) % 7;
  c.SetNumber(std::floor((std::floor(s) - jan1 + offset) / 7) + 1);
}

void IsoWeekNum(Call& c) {
  double s;
  if (!SerialArg(c, 0, s)) return;
  int y, m, d;
  SerialDate(s, c.m.date1904, y, m, d);
  c.SetNumber(IsoWeek(y, m, d, c.m.date1904));
}

void NetworkDays(Call& c) { NetDays(c, -1, 2); }
void NetworkDaysIntl(Call& c) { NetDays(c, 2, 3); }
void Workday(Call& c) { AddWorkdays(c, -1, 2); }
void WorkdayIntl(Call& c) { AddWorkdays(c, 2, 3); }

void Days(Call& c) {
  double e, s;
  if (!SerialArg(c, 0, e) || !SerialArg(c, 1, s)) return;
  c.SetNumber(std::floor(e) - std::floor(s));
}

void DateDif(Call& c) {
  double a, b;
  std::string unit;
  if (!SerialArg(c, 0, a) || !SerialArg(c, 1, b) || !c.Text(2, unit)) return;
  a = std::floor(a), b = std::floor(b);
  if (a > b) return c.Fail(Err::Num);
  int y1, m1, d1, y2, m2, d2;
  SerialDate(a, c.m.date1904, y1, m1, d1);
  SerialDate(b, c.m.date1904, y2, m2, d2);
  int months = (y2 - y1) * 12 + (m2 - m1) - (d2 < d1 ? 1 : 0);
  if (EqualText(unit, "D")) return c.SetNumber(b - a);
  if (EqualText(unit, "M")) return c.SetNumber(months);
  if (EqualText(unit, "Y")) return c.SetNumber(months / 12);
  if (EqualText(unit, "YM")) return c.SetNumber(months % 12);
  if (EqualText(unit, "MD")) {
    if (d2 >= d1) return c.SetNumber(d2 - d1);
    int pm = m2 == 1 ? 12 : m2 - 1, py = m2 == 1 ? y2 - 1 : y2;
    return c.SetNumber(DaysInMonth(py, pm) - d1 + d2);
  }
  if (EqualText(unit, "YD")) {
    double anniversary;
    int y = (m2 > m1 || (m2 == m1 && d2 >= d1)) ? y2 : y2 - 1;
    int dd = std::min(d1, DaysInMonth(y, m1));
    if (!DateSerial(y, m1, dd, c.m.date1904, anniversary)) return c.Fail(Err::Num);
    return c.SetNumber(b - anniversary);
  }
  c.Fail(Err::Num);
}

void DateValue(Call& c) {
  const Value& v = c.arg(0);
  if (v.IsError()) return c.Fail(v.error());
  if (!v.IsText()) return c.Fail(Err::Value);
  double d;
  if (!c.m.host.ParseNumber(v.text(), d) || d < 0) return c.Fail(Err::Value);
  // A number written as text isn't a date.
  bool letters = false, seps = false;
  for (char ch : v.text()) {
    letters = letters || (ch >= 'A' && ch <= 'z');
    seps = seps || ch == '/' || ch == '-' || ch == ':';
  }
  if (!letters && !seps) return c.Fail(Err::Value);
  c.SetNumber(std::floor(d));
}

void TimeValue(Call& c) {
  const Value& v = c.arg(0);
  if (v.IsError()) return c.Fail(v.error());
  if (!v.IsText()) return c.Fail(Err::Value);
  double d;
  if (!c.m.host.ParseNumber(v.text(), d) || d < 0) return c.Fail(Err::Value);
  c.SetNumber(d - std::floor(d));
}

bool SerialArg(Call& c, int i, double& s) {
  if (!c.Number(i, s)) return false;
  int y, m, d;
  if (!SerialDate(s, c.m.date1904, y, m, d)) {
    c.Fail(Err::Num);
    return false;
  }
  return true;
}

double YearFracOf(double a, double b, int basis, bool date1904) {
  a = std::floor(a), b = std::floor(b);
  if (a > b) std::swap(a, b);
  int y1, m1, d1, y2, m2, d2;
  SerialDate(a, date1904, y1, m1, d1);
  SerialDate(b, date1904, y2, m2, d2);
  switch (basis) {
    case 0: {  // US 30/360
      bool lastFeb1 = m1 == 2 && d1 == DaysInMonth(y1, 2), lastFeb2 = m2 == 2 && d2 == DaysInMonth(y2, 2);
      if (lastFeb1 && lastFeb2) d2 = 30;
      if (lastFeb1) d1 = 30;
      if (d2 == 31 && d1 >= 30) d2 = 30;
      if (d1 == 31) d1 = 30;
      return ((y2 - y1) * 360.0 + (m2 - m1) * 30.0 + (d2 - d1)) / 360;
    }
    case 1: {  // actual/actual
      double days = b - a;
      if (y1 == y2) return days / (IsLeap(y1) ? 366 : 365);
      double oneYear;
      bool within = DateSerial(y1 + 1, m1, d1, date1904, oneYear) && b <= oneYear;
      if (within) {
        bool leap = (IsLeap(y1) && (m1 < 3)) || (IsLeap(y2) && (m2 > 2 || (m2 == 2 && d2 == 29)));
        return days / (leap ? 366 : 365);
      }
      double total = 0;
      for (int y = y1; y <= y2; ++y) total += IsLeap(y) ? 366 : 365;
      return days / (total / (y2 - y1 + 1));
    }
    case 2: return (b - a) / 360;
    case 3: return (b - a) / 365;
    default: {  // European 30/360
      if (d1 == 31) d1 = 30;
      if (d2 == 31) d2 = 30;
      return ((y2 - y1) * 360.0 + (m2 - m1) * 30.0 + (d2 - d1)) / 360;
    }
  }
}

void YearFrac(Call& c) {
  double a, b, basis;
  if (!SerialArg(c, 0, a) || !SerialArg(c, 1, b) || !c.Integer(2, basis)) return;
  if (basis < 0 || basis > 4) return c.Fail(Err::Num);
  c.SetNumber(YearFracOf(a, b, static_cast<int>(basis), c.m.date1904));
}

// DAYS360(start_date, end_date, [method]): FALSE the US (NASD) method, TRUE the European one.
void Days360(Call& c) {
  double a, b;
  bool europe;
  if (!SerialArg(c, 0, a) || !SerialArg(c, 1, b) || !c.Boolean(2, europe)) return;
  int y1, m1, d1, y2, m2, d2;
  SerialDate(std::floor(a), c.m.date1904, y1, m1, d1);
  SerialDate(std::floor(b), c.m.date1904, y2, m2, d2);
  if (europe) {
    if (d1 == 31) d1 = 30;
    if (d2 == 31) d2 = 30;
  } else {
    if (d1 == 31 || (m1 == 2 && d1 == DaysInMonth(y1, 2))) d1 = 30;
    if (d2 == 31) {
      if (d1 < 30) d2 = 1, m2 += 1;
      else d2 = 30;
    }
  }
  c.SetNumber((y2 - y1) * 360.0 + (m2 - m1) * 30.0 + (d2 - d1));
}

}  // namespace fw::sheet::calc::fn
