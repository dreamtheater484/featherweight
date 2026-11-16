#include "sheet/numfmt/NumLocale.h"

#include <windows.h>

#include <map>
#include <memory>
#include <mutex>

namespace fw::sheet {

namespace {

const wchar_t* const kMonths[12] = {L"January", L"February", L"March",     L"April",   L"May",      L"June",
                                    L"July",    L"August",   L"September", L"October", L"November", L"December"};
const wchar_t* const kDays[7] = {L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday"};

void EnglishNames(WinLocaleInfo& w, bool sept) {
  for (int k = 0; k < 12; ++k) {
    w.month[k] = kMonths[k];
    w.monthAbbr[k] = std::wstring(kMonths[k], 3);
  }
  if (sept) w.monthAbbr[8] = L"Sept";  // Windows' en-GB-style names (en-NL, en-GB), which Excel shows
  for (int k = 0; k < 7; ++k) {
    w.day[k] = kDays[k];
    w.dayAbbr[k] = std::wstring(kDays[k], 3);
  }
}

// Excel's own table for en-US (ECMA-376 Part 1, 18.8.30, as Excel writes the codes).
const wchar_t* const kEnUsBuiltins[50] = {
    L"General", L"0", L"0.00", L"#,##0", L"#,##0.00",
    L"\"$\"#,##0_);\\(\"$\"#,##0\\)", L"\"$\"#,##0_);[Red]\\(\"$\"#,##0\\)",
    L"\"$\"#,##0.00_);\\(\"$\"#,##0.00\\)", L"\"$\"#,##0.00_);[Red]\\(\"$\"#,##0.00\\)",
    L"0%", L"0.00%", L"0.00E+00", L"# ?/?", L"# ??/??", L"m/d/yyyy", L"d-mmm-yy", L"d-mmm", L"mmm-yy",
    L"h:mm AM/PM", L"h:mm:ss AM/PM", L"h:mm", L"h:mm:ss", L"m/d/yyyy h:mm",
    L"General", L"General", L"General", L"General",                                   // 23..26
    L"m/d/yyyy", L"m/d/yyyy", L"m/d/yyyy", L"m/d/yyyy", L"m/d/yyyy",                  // 27..31
    L"h:mm:ss", L"h:mm:ss", L"h:mm:ss", L"h:mm:ss", L"m/d/yyyy",                      // 32..36
    L"#,##0_);(#,##0)", L"#,##0_);[Red](#,##0)", L"#,##0.00_);(#,##0.00)", L"#,##0.00_);[Red](#,##0.00)",
    L"_(* #,##0_);_(* \\(#,##0\\);_(* \"-\"_);_(@_)",
    L"_(\"$\"* #,##0_);_(\"$\"* \\(#,##0\\);_(\"$\"* \"-\"_);_(@_)",
    L"_(* #,##0.00_);_(* \\(#,##0.00\\);_(* \"-\"??_);_(@_)",
    L"_(\"$\"* #,##0.00_);_(\"$\"* \\(#,##0.00\\);_(\"$\"* \"-\"??_);_(@_)",
    L"mm:ss", L"[h]:mm:ss", L"mm:ss.0", L"##0.0E+0", L"@"};

std::wstring Quoted(std::wstring_view s) {
  if (s.empty()) return {};
  std::wstring out = L"\"";
  for (wchar_t c : s) out += c == '"' ? std::wstring(L"\\\"") : std::wstring(1, c);
  return out + L"\"";
}

// A Windows currency pattern ("$ -1": '$' the symbol, '1' the number) as a format section.
std::wstring CurrencySection(std::wstring_view pattern, std::wstring_view symbol, std::wstring_view number) {
  std::wstring out, lit;
  auto flush = [&] {
    out += Quoted(lit);
    lit.clear();
  };
  for (wchar_t c : pattern) {
    if (c == '$') {
      lit += symbol;
    } else if (c == '1') {
      flush();
      out += number;
    } else {
      lit += c;
    }
  }
  flush();
  return out;
}

const wchar_t* const kPositiveCurrency[4] = {L"$1", L"1$", L"$ 1", L"1 $"};
const wchar_t* const kNegativeCurrency[16] = {L"($1)",  L"-$1",  L"$-1",  L"$1-",  L"(1$)",  L"-1$",
                                              L"1-$",   L"1$-",  L"-1 $", L"-$ 1", L"1 $-",  L"$ 1-",
                                              L"$ -1",  L"1- $", L"($ 1)", L"(1 $)"};
const wchar_t* const kNegativeNumber[5] = {L"(1)", L"-1", L"- 1", L"1-", L"1 -"};

// The short date's day, month and year with the separators between them; false for anything else.
bool SplitDate(const std::wstring& code, std::wstring part[3], std::wstring sep[2], char kind[3]) {
  int n = 0;
  size_t i = 0;
  while (i < code.size()) {
    const wchar_t c = code[i];
    if (c == 'd' || c == 'm' || c == 'y') {
      if (n == 3) return false;
      size_t e = i;
      while (e < code.size() && code[e] == c) ++e;
      part[n] = code.substr(i, e - i);
      kind[n++] = static_cast<char>(c);
      i = e;
    } else {
      if (n == 0 || n == 3) return false;
      sep[n - 1] += c;
      ++i;
    }
  }
  return n == 3;
}

void MakeBuiltins(NumLocale& l, const WinLocaleInfo& w) {
  for (int k = 0; k < 50; ++k) l.builtin[k] = kEnUsBuiltins[k];
  if (w.name == L"en-US") return;
  const std::wstring sym = w.sCurrency.empty() ? L"$" : w.sCurrency;
  const std::wstring pos = kPositiveCurrency[w.iCurrency & 3];
  const std::wstring neg = kNegativeCurrency[w.iNegCurr >= 0 && w.iNegCurr < 16 ? w.iNegCurr : 0];
  for (int dec = 0; dec < 2; ++dec) {
    const std::wstring number = dec ? L"#,##0.00" : L"#,##0";
    const std::wstring p = CurrencySection(pos, sym, number), n = CurrencySection(neg, sym, number);
    l.builtin[5 + 2 * dec] = p + L";" + n;
    l.builtin[6 + 2 * dec] = p + L";[Red]" + n;
    const std::wstring nn = CurrencySection(kNegativeNumber[w.iNegNumber >= 0 && w.iNegNumber < 5 ? w.iNegNumber : 1],
                                            L"", number);
    l.builtin[37 + 2 * dec] = number + L";" + nn;
    l.builtin[38 + 2 * dec] = number + L";[Red]" + nn;
    // Accounting, as Excel's region tables write it (en-NL measured): "_ € * #,##0_ ".
    const bool before = (w.iCurrency & 1) == 0;
    const std::wstring dash = dec ? L"\"-\"??" : L"\"-\"";
    for (int cur = 0; cur < 2; ++cur) {
      const std::wstring s = cur ? Quoted(sym) : L"";
      std::wstring head = L"_ " + (cur && before ? s + L" " : L"") + L"* ";
      std::wstring tail = (cur && !before ? L" " + s : L"") + L"_ ";
      l.builtin[41 + cur + 2 * dec] = head + number + tail + L";" + head + L"-" + number + tail + L";" + head + dash +
                                      tail + L";_ @_ ";
    }
  }
  const std::wstring date = ExcelCodeFromPattern(w.sShortDate);
  if (!date.empty()) {
    l.builtin[14] = date;
    std::wstring part[3], sep[2];
    char kind[3];
    if (SplitDate(date, part, sep, kind)) {
      std::wstring p15[3];
      for (int k = 0; k < 3; ++k) p15[k] = kind[k] == 'm' ? L"mmm" : kind[k] == 'y' ? L"yy" : part[k];
      l.builtin[15] = p15[0] + sep[0] + p15[1] + sep[1] + p15[2];
      auto without = [&](char drop) {  // the other two parts, with the separator between them
        const int j = kind[0] == drop ? 0 : kind[1] == drop ? 1 : 2;
        return j == 0 ? p15[1] + sep[1] + p15[2] : j == 1 ? p15[0] + sep[0] + p15[2] : p15[0] + sep[0] + p15[1];
      };
      l.builtin[16] = without('y');
      l.builtin[17] = without('d');
    }
  }
  auto padded = [](const std::wstring& pattern) {
    return pattern.find(L"HH") != std::wstring::npos || pattern.find(L"hh") != std::wstring::npos;
  };
  l.builtin[20] = padded(w.sShortTime.empty() ? w.sTimeFormat : w.sShortTime) ? L"hh:mm" : L"h:mm";
  l.builtin[21] = padded(w.sTimeFormat) ? L"hh:mm:ss" : L"h:mm:ss";
  l.builtin[22] = l.builtin[14] + L" " + l.builtin[20];
  for (int k : {27, 28, 29, 30, 31, 36}) l.builtin[k] = l.builtin[14];
  for (int k : {32, 33, 34, 35}) l.builtin[k] = l.builtin[21];
}

std::wstring Info(const wchar_t* name, LCTYPE type) {
  wchar_t buf[256];
  const int n = GetLocaleInfoEx(name, type, buf, 256);
  return n > 1 ? std::wstring(buf, static_cast<size_t>(n - 1)) : std::wstring();
}

int InfoNumber(const wchar_t* name, LCTYPE type) {
  DWORD v = 0;
  GetLocaleInfoEx(name, type | LOCALE_RETURN_NUMBER, reinterpret_cast<wchar_t*>(&v), sizeof v / sizeof(wchar_t));
  return static_cast<int>(v);
}

WinLocaleInfo ReadInfo(const wchar_t* name, LCTYPE flags) {
  WinLocaleInfo w;
  w.name = Info(name, LOCALE_SNAME | flags);
  w.sDecimal = Info(name, LOCALE_SDECIMAL | flags);
  w.sThousand = Info(name, LOCALE_STHOUSAND | flags);
  w.sDate = Info(name, LOCALE_SDATE | flags);
  w.sTime = Info(name, LOCALE_STIME | flags);
  w.sList = Info(name, LOCALE_SLIST | flags);
  w.sShortDate = Info(name, LOCALE_SSHORTDATE | flags);
  w.sLongDate = Info(name, LOCALE_SLONGDATE | flags);
  w.sTimeFormat = Info(name, LOCALE_STIMEFORMAT | flags);
  w.sShortTime = Info(name, LOCALE_SSHORTTIME | flags);
  w.s1159 = Info(name, LOCALE_S1159 | flags);
  w.s2359 = Info(name, LOCALE_S2359 | flags);
  w.sCurrency = Info(name, LOCALE_SCURRENCY | flags);
  w.iCurrency = InfoNumber(name, LOCALE_ICURRENCY | flags);
  w.iNegCurr = InfoNumber(name, LOCALE_INEGCURR | flags);
  w.iNegNumber = InfoNumber(name, LOCALE_INEGNUMBER | flags);
  for (int k = 0; k < 12; ++k) {
    w.month[k] = Info(name, (LOCALE_SMONTHNAME1 + k) | flags);
    w.monthAbbr[k] = Info(name, (LOCALE_SABBREVMONTHNAME1 + k) | flags);
  }
  for (int k = 0; k < 7; ++k) {  // Windows starts on Monday
    w.day[(k + 1) % 7] = Info(name, (LOCALE_SDAYNAME1 + k) | flags);
    w.dayAbbr[(k + 1) % 7] = Info(name, (LOCALE_SABBREVDAYNAME1 + k) | flags);
  }
  return w;
}

std::once_flag g_userOnce;
NumLocale g_user;

}  // namespace

std::wstring ExcelCodeFromPattern(std::wstring_view p) {
  std::wstring out;
  for (size_t i = 0; i < p.size();) {
    const wchar_t c = p[i];
    if (c == '\'') {  // quoted text; '' is a quote
      std::wstring lit;
      size_t k = i + 1;
      for (; k < p.size(); ++k) {
        if (p[k] == '\'') {
          if (k + 1 < p.size() && p[k + 1] == '\'') {
            lit += L'\'';
            ++k;
            continue;
          }
          break;
        }
        lit += p[k];
      }
      out += Quoted(lit);
      i = k + 1;
      continue;
    }
    size_t e = i;
    while (e < p.size() && p[e] == c) ++e;
    const size_t n = e - i;
    switch (c) {
      case 'd': out += n >= 4 ? L"dddd" : std::wstring(n, L'd'); break;
      case 'M': out += n >= 4 ? L"mmmm" : std::wstring(n, L'm'); break;
      case 'y': out += n >= 3 ? L"yyyy" : L"yy"; break;
      case 'g': break;
      case 'h':
      case 'H': out += std::wstring(n >= 2 ? 2 : 1, L'h'); break;
      case 'm': out += std::wstring(n >= 2 ? 2 : 1, L'm'); break;
      case 's': out += std::wstring(n >= 2 ? 2 : 1, L's'); break;
      case 't': out += n >= 2 ? L"AM/PM" : L"A/P"; break;
      default:
        for (size_t k = i; k < e; ++k) {
          const wchar_t x = p[k];
          if ((x >= 'a' && x <= 'z') || (x >= 'A' && x <= 'Z') || wcschr(L"0#?@%_*\\\"[];E", x))
            out += L'\\';
          out += x;
        }
        break;
    }
    i = e;
  }
  return out;
}

NumLocale MakeLocale(const WinLocaleInfo& w) {
  NumLocale l;
  auto set = [](std::wstring& field, const std::wstring& v) {
    if (!v.empty()) field = v;
  };
  set(l.decimal, w.sDecimal);
  l.group = w.sThousand;  // may be empty: no grouping character
  set(l.dateSep, w.sDate);
  set(l.timeSep, w.sTime);
  set(l.list, w.sList);
  if (l.list == l.decimal) l.list = L";";  // Excel's rule
  l.am = w.s1159.empty() ? L"AM" : w.s1159;
  l.pm = w.s2359.empty() ? L"PM" : w.s2359;
  set(l.currency, w.sCurrency);
  l.month = w.month;
  l.monthAbbr = w.monthAbbr;
  l.day = w.day;
  l.dayAbbr = w.dayAbbr;
  for (int k = 0; k < 12; ++k) {
    if (l.month[k].empty()) l.month[k] = kMonths[k];
    if (l.monthAbbr[k].empty()) l.monthAbbr[k] = std::wstring(kMonths[k], 3);
  }
  for (int k = 0; k < 7; ++k) {
    if (l.day[k].empty()) l.day[k] = kDays[k];
    if (l.dayAbbr[k].empty()) l.dayAbbr[k] = std::wstring(kDays[k], 3);
  }
  for (wchar_t c : w.sShortDate) {
    if (c == 'd' || c == 'M' || c == 'y') {
      l.dateOrder = c == 'd' ? 'D' : c == 'M' ? 'M' : 'Y';
      break;
    }
  }
  MakeBuiltins(l, w);
  l.longDate = ExcelCodeFromPattern(w.sLongDate.empty() ? L"dddd, MMMM d, yyyy" : w.sLongDate);
  l.time = ExcelCodeFromPattern(w.sTimeFormat.empty() ? L"h:mm:ss tt" : w.sTimeFormat);
  const wchar_t* name = w.name.c_str();
  l.lcid = w.name.empty() ? 0x409 : LocaleNameToLCID(name, 0);
  return l;
}

WinLocaleInfo EnUsInfo() {
  WinLocaleInfo w;
  w.name = L"en-US";
  w.sDecimal = L".", w.sThousand = L",", w.sDate = L"/", w.sTime = L":", w.sList = L",";
  w.sShortDate = L"M/d/yyyy", w.sLongDate = L"dddd, MMMM d, yyyy";
  w.sTimeFormat = L"h:mm:ss tt", w.sShortTime = L"h:mm tt";
  w.s1159 = L"AM", w.s2359 = L"PM", w.sCurrency = L"$";
  w.iCurrency = 0, w.iNegCurr = 0, w.iNegNumber = 1;
  EnglishNames(w, false);
  return w;
}

WinLocaleInfo EnNlInfo() {
  WinLocaleInfo w;
  w.name = L"en-NL";
  w.sDecimal = L",", w.sThousand = L".", w.sDate = L"/", w.sTime = L":", w.sList = L",";
  w.sShortDate = L"dd/MM/yyyy", w.sLongDate = L"dddd, d MMMM yyyy";
  w.sTimeFormat = L"HH:mm:ss", w.sShortTime = L"HH:mm";
  w.s1159 = L"am", w.s2359 = L"pm", w.sCurrency = L"€";
  w.iCurrency = 2, w.iNegCurr = 12, w.iNegNumber = 1;
  EnglishNames(w, true);
  return w;
}

const NumLocale& EnUsLocale() {
  static const NumLocale l = [] {
    NumLocale x = MakeLocale(EnUsInfo());
    x.language = WindowsLanguage;
    return x;
  }();
  return l;
}

const NumLocale& EnNlLocale() {
  static const NumLocale l = [] {
    NumLocale x = MakeLocale(EnNlInfo());
    x.language = WindowsLanguage;
    return x;
  }();
  return l;
}

WinLocaleInfo ReadWinLocale(std::wstring_view name) {
  if (name.empty()) return ReadInfo(LOCALE_NAME_USER_DEFAULT, 0);
  const std::wstring n(name);
  return ReadInfo(n.c_str(), LOCALE_NOUSEROVERRIDE);
}

void RefreshUserLocale() {
  NumLocale l = MakeLocale(ReadWinLocale());
  l.language = WindowsLanguage;
  g_user = std::move(l);
}

const NumLocale& UserLocale() {
  std::call_once(g_userOnce, [] { RefreshUserLocale(); });
  return g_user;
}

const NumLocale* WindowsLanguage(uint32_t lcid) {
  static std::mutex lock;
  static std::map<uint32_t, std::unique_ptr<NumLocale>> cache;
  std::lock_guard<std::mutex> guard(lock);
  auto it = cache.find(lcid);
  if (it != cache.end()) return it->second.get();
  wchar_t name[LOCALE_NAME_MAX_LENGTH];
  std::unique_ptr<NumLocale> l;
  if (lcid && LCIDToLocaleName(lcid, name, LOCALE_NAME_MAX_LENGTH, 0) > 0)
    l = std::make_unique<NumLocale>(MakeLocale(ReadInfo(name, LOCALE_NOUSEROVERRIDE)));
  const NumLocale* result = l.get();
  cache.emplace(lcid, std::move(l));
  return result;
}

}  // namespace fw::sheet
