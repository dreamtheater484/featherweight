#include "doc/model/DateFormat.h"

#include <windows.h>

#include <cstdio>
#include <cwctype>

namespace fw::doc {
namespace {

SYSTEMTIME System(const DateTime& t) {
  SYSTEMTIME s{};
  s.wYear = static_cast<WORD>(t.year);
  s.wMonth = static_cast<WORD>(t.month);
  s.wDay = static_cast<WORD>(t.day);
  s.wHour = static_cast<WORD>(t.hour);
  s.wMinute = static_cast<WORD>(t.minute);
  s.wSecond = static_cast<WORD>(t.second);
  // The day of the week, for names: through a FILETIME and back.
  FILETIME ft;
  SYSTEMTIME full;
  if (SystemTimeToFileTime(&s, &ft) && FileTimeToSystemTime(&ft, &full)) s.wDayOfWeek = full.wDayOfWeek;
  return s;
}

DateTime FromSystem(const SYSTEMTIME& s) { return {s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute, s.wSecond}; }

}  // namespace

std::u16string FormatDateTime(const DateTime& when, std::u16string_view picture, bool time, std::string_view lang) {
  const SYSTEMTIME t = System(when);
  std::wstring name(lang.begin(), lang.end());
  const wchar_t* locale = !name.empty() && IsValidLocaleName(name.c_str()) ? name.c_str() : LOCALE_NAME_USER_DEFAULT;
  std::wstring out;
  auto names = [&](const wchar_t* code) {  // month and day names from Windows, in the language
    wchar_t b[128];
    const int n = GetDateFormatEx(locale, 0, &t, code, b, 128, nullptr);
    if (n > 1) out.append(b, static_cast<size_t>(n - 1));
  };
  auto num = [&](int v, bool two) {
    wchar_t b[16];
    swprintf_s(b, two ? L"%02d" : L"%d", v);
    out += b;
  };
  if (picture.empty()) {
    wchar_t b[128];
    const int n = time ? GetTimeFormatEx(locale, TIME_NOSECONDS, &t, nullptr, b, 128)
                       : GetDateFormatEx(locale, DATE_SHORTDATE, &t, nullptr, b, 128, nullptr);
    if (n > 1) out.assign(b, static_cast<size_t>(n - 1));
    return std::u16string(out.begin(), out.end());
  }
  auto designator = [&](bool pm) {
    wchar_t b[32] = {};
    std::wstring d = GetLocaleInfoEx(locale, pm ? LOCALE_S2359 : LOCALE_S1159, b, 32) > 1 ? std::wstring(b) : std::wstring();
    return d.empty() ? std::wstring(pm ? L"PM" : L"AM") : d;
  };
  const std::wstring p(picture.begin(), picture.end());
  for (size_t i = 0; i < p.size();) {
    const wchar_t c = p[i];
    if (c == L'\'') {  // text as it is
      const size_t end = p.find(L'\'', i + 1);
      out += p.substr(i + 1, (end == std::wstring::npos ? p.size() : end) - i - 1);
      i = end == std::wstring::npos ? p.size() : end + 1;
      continue;
    }
    auto at = [&](std::wstring_view w) { return p.size() - i >= w.size() && _wcsnicmp(p.c_str() + i, w.data(), w.size()) == 0; };
    if (at(L"AM/PM") || at(L"A/P")) {
      const bool shortForm = at(L"A/P");
      std::wstring d = designator(t.wHour >= 12);
      if (shortForm) d = d.substr(0, 1);
      for (wchar_t& ch : d) ch = c == L'a' ? static_cast<wchar_t>(std::towlower(ch)) : static_cast<wchar_t>(std::towupper(ch));
      out += d;
      i += shortForm ? 3 : 5;
      continue;
    }
    size_t n = 1;
    while (i + n < p.size() && p[i + n] == c) ++n;
    switch (c) {
      case L'M':
        if (n <= 2) num(t.wMonth, n == 2);
        else names(n == 3 ? L"MMM" : L"MMMM");
        break;
      case L'd':
      case L'D':
        if (n <= 2) num(t.wDay, n == 2);
        else names(n == 3 ? L"ddd" : L"dddd");
        break;
      case L'y':
      case L'Y':
        if (n <= 2) num(t.wYear % 100, true);
        else num(t.wYear, false);
        break;
      case L'h': num(t.wHour % 12 == 0 ? 12 : t.wHour % 12, n >= 2); break;
      case L'H': num(t.wHour, n >= 2); break;
      case L'm': num(t.wMinute, n >= 2); break;
      case L's':
      case L'S': num(t.wSecond, n >= 2); break;
      default: out.append(n, c); break;
    }
    i += n;
  }
  return std::u16string(out.begin(), out.end());
}

DateTime LocalDateTime(int64_t fileTime) {
  FILETIME ft;
  ft.dwLowDateTime = static_cast<DWORD>(static_cast<uint64_t>(fileTime) & 0xFFFFFFFFu);
  ft.dwHighDateTime = static_cast<DWORD>(static_cast<uint64_t>(fileTime) >> 32);
  SYSTEMTIME utc, local;
  if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return {};
  return FromSystem(local);
}

DateTime Now() {
  SYSTEMTIME local;
  GetLocalTime(&local);
  return FromSystem(local);
}

std::string FullDate(const DateTime& t) {
  char b[40];
  snprintf(b, sizeof b, "%04d-%02d-%02dT00:00:00Z", t.year, t.month, t.day);
  return b;
}

std::optional<DateTime> ParseFullDate(std::string_view s) {
  DateTime t;
  if (s.size() < 10 || s[4] != '-' || s[7] != '-') return std::nullopt;
  auto num = [&](size_t at, size_t len, int& out) {
    out = 0;
    for (size_t i = at; i < at + len; ++i) {
      if (s[i] < '0' || s[i] > '9') return false;
      out = out * 10 + (s[i] - '0');
    }
    return true;
  };
  if (!num(0, 4, t.year) || !num(5, 2, t.month) || !num(8, 2, t.day)) return std::nullopt;
  if (t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31 || t.year < 1601) return std::nullopt;
  return t;
}

}  // namespace fw::doc
