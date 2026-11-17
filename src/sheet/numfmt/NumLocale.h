#pragma once

// What number formats take from the locale (plan §7.3, decision 17): separators, names, and the
// codes of Excel's built-in formats, which follow the region (on an en-NL PC built-in 14 is
// dd/mm/yyyy and 42 has "€"). The app uses Windows' regional settings; tests use fixed locales.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace fw::sheet {

struct NumLocale {
  std::wstring decimal = L".", group = L",";  // number separators
  std::wstring dateSep = L"/", timeSep = L":";
  std::wstring list = L",";                   // CSV and formula argument separator, as Excel uses it
  std::wstring am = L"AM", pm = L"PM";        // the region's designators (built-ins 18 and 19, system time)
  std::wstring currency = L"$";
  std::array<std::wstring, 12> month, monthAbbr;
  std::array<std::wstring, 7> day, dayAbbr;   // Sunday first
  // Built-in formats 0..49 in Excel's invariant syntax (ids 50..81 map onto these; see BuiltinCode).
  std::array<std::wstring, 50> builtin;
  std::wstring longDate, time;                // [$-F800] and [$-F400]: Windows' long date and time
  char dateOrder = 'M';                       // typed dates: 'M' (m/d/y), 'D' (d/m/y) or 'Y' (y/m/d)
  uint32_t lcid = 0x409;
  // Format codes as the region types them (TEXT(), Excel's Format Cells box): the letters for
  // year, day and hour, and General's name. Excel takes them from the region, not the language:
  // on an en-NL PC they are Dutch (jjjj, uu, Standaard; measured).
  wchar_t yearCode = L'y', dayCode = L'd', hourCode = L'h';
  std::wstring generalName = L"General";
  // Names in another language, for a [$-407] tag: nullptr, or a locale whose names (month, day,
  // am, pm) to use. Null by default (the tag's language then shows this locale's names).
  const NumLocale* (*language)(uint32_t lcid) = nullptr;
};

// The Windows values a locale is made from (GetLocaleInfoEx's names), so a locale can be recorded
// with a reference and rebuilt anywhere.
struct WinLocaleInfo {
  std::wstring name;                       // "en-NL"
  std::wstring sDecimal, sThousand, sDate, sTime, sList;
  std::wstring sShortDate, sLongDate, sTimeFormat, sShortTime;
  std::wstring s1159, s2359, sCurrency;
  int iCurrency = 0, iNegCurr = 0, iNegNumber = 1;
  std::array<std::wstring, 12> month, monthAbbr;
  std::array<std::wstring, 7> day, dayAbbr;  // Sunday first
};

// Builds a locale from Windows' values, with Excel's built-in table for that region: en-US's own
// table (ECMA-376's codes) for en-US, otherwise made from the patterns as Excel makes the region's
// (exact for en-NL, measured; close for others).
NumLocale MakeLocale(const WinLocaleInfo& info);

// Fixed locales for tests and tools: en-US (Excel's reference table) and en-NL (the reference PC).
const NumLocale& EnUsLocale();
const NumLocale& EnNlLocale();
WinLocaleInfo EnUsInfo();
WinLocaleInfo EnNlInfo();

// Windows' values for a locale name (LOCALE_NAME_USER_DEFAULT when empty), with the user's
// overrides for the user default.
WinLocaleInfo ReadWinLocale(std::wstring_view name = {});
// The user's regional settings, read once (RefreshUserLocale reads them again, after
// WM_SETTINGCHANGE "intl"). Its `language` reads other languages' names from Windows.
const NumLocale& UserLocale();
void RefreshUserLocale();
// Names for an LCID, from Windows, cached; nullptr for an unknown LCID.
const NumLocale* WindowsLanguage(uint32_t lcid);
// The LCID of a locale name ("en-US", as a [$-en-US] tag has it); 0 when Windows doesn't know it.
uint32_t LcidFromName(std::wstring_view name);

// Converts a Windows date or time pattern ("dddd, d MMMM yyyy", "h:mm:ss tt") to an Excel code.
std::wstring ExcelCodeFromPattern(std::wstring_view pattern);

}  // namespace fw::sheet
