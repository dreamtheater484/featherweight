#pragma once

// Excel's number formats (plan §7.3): one engine for every format code. Sections and conditions,
// colors, locale and currency tags, digits, thousands and scaling, percent, scientific, fractions,
// dates and times (elapsed, fractional seconds, 1900 and 1904), text, padding and fills, and
// General as a cell shows it, which depends on the column's width. Headless: the cell's width is a
// callback. Behavior measured against Excel 2021 (tests/unit/test_sheet_numfmt.cpp).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sheet/numfmt/NumLocale.h"

namespace fw::sheet {

// A value as a format shows it.
struct Formatted {
  // The text as Excel's TEXT() gives it: each `_x` pad is one space, and a `*x` fill adds nothing.
  std::wstring text;
  // `_x`: text[at] is a space as wide as `as`.
  struct Pad {
    uint32_t at;
    wchar_t as;
  };
  std::vector<Pad> pads;
  // `*x`: `fillChar` repeats at text index `fill` to fill the cell's width; -1: no fill.
  int fill = -1;
  wchar_t fillChar = 0;
  // A color from the format ([Red], [Color10]): Excel's palette index 1..56 (see PaletteColor);
  // 0: the cell's own color.
  uint8_t color = 0;
  // The value can't be shown ("####": a date out of range, no section for it, or too wide for the
  // cell): text is empty and the fill is '#' at 0, so the cell fills with '#'.
  bool overflow = false;

  void Clear() {
    text.clear();
    pads.clear();
    fill = -1;
    fillChar = 0;
    color = 0;
    overflow = false;
  }
};

// Where a value is shown.
struct FormatContext {
  const NumLocale* locale = nullptr;  // nullptr: en-US
  bool date1904 = false;              // the workbook's date system
  // The cell's width: true when `f` fits (text, pads and at least no fill). nullptr: any width
  // (TEXT(), copying). A number that doesn't fit shows "####"; General shows fewer digits first.
  bool (*fits)(void* context, const Formatted& f) = nullptr;
  void* fitsContext = nullptr;
};

class NumberFormat {
 public:
  NumberFormat();  // General
  // A format code in Excel's invariant form, as files store it. Anything is accepted: what Excel
  // would refuse is read as literally as it can be.
  explicit NumberFormat(std::wstring_view code);
  // A built-in format by id (0..163) as the locale shows it; an id without a format is General.
  static NumberFormat Builtin(int id, const NumLocale& locale);
  // A code as the region types it (TEXT()'s format, the Format Cells box): its letters for year,
  // day and hour ("jjjj", "uu" in the Netherlands), its name for General ("Standaard") and its
  // separators (decimal comma, thousands point there). False when Excel refuses the code (TEXT()
  // gives #VALUE!): a digit placeholder in a date, an English code the region doesn't have ([h],
  // "General"). AM/PM then shows the region's designators, unless the code has a locale tag.
  static bool FromLocal(std::wstring_view code, const NumLocale& locale, NumberFormat& out);
  // The code in Excel's invariant form, as Excel stores what was typed; false when refused.
  static bool LocalToInvariant(std::wstring_view code, const NumLocale& locale, std::wstring& out);

  void Format(double value, const FormatContext& context, Formatted& out) const;
  void Format(std::wstring_view text, const FormatContext& context, Formatted& out) const;

  bool IsGeneral() const { return general_; }
  // The value's first section shows a date or time (dates and times are copied, typed and saved
  // to CSV as dates).
  bool IsDateTime() const;
  // The first section is text (@): typing into such a cell keeps the text.
  bool IsText() const;

  // The compiled code (internal to the engine).
  struct Token {
    enum Kind : uint8_t {
      Literal,   // pool_[a, a + b)
      Digit,     // ch: '0', '#' or '?'
      Point,     // the decimal separator
      Percent,
      Exp,       // ch: 'E' or 'e'; a: '+' or '-'
      Slash,     // a fraction's bar; in a date, the date separator
      Colon,     // in a time, the time separator
      FixedDen,  // a fraction's fixed denominator: a
      Pad,       // `_ch`
      Fill,      // `*ch`
      At,        // the text
      General,
      Year,      // b letters ('e': ch == 'e')
      Month,
      Day,
      Hour,
      Minute,
      Second,
      Elapsed,   // [h], [mm], [ss]: ch 'h', 'm' or 's'; b: width
      AmPm,      // ch 0: AM/PM; otherwise A/P: a, b are the letters' cases ('A' or 'a', 'P' or 'p')
      FracSec,   // b digits of seconds
      Era,       // shown as nothing
      Comma,     // resolved when compiled: grouping, scaling or a literal
    } kind;
    wchar_t ch = 0;
    uint32_t a = 0, b = 0;
  };
  struct Section {
    enum Kind : uint8_t { Number, Fraction, Scientific, DateTime, Text, General } kind = Number;
    std::vector<Token> tokens;
    char op = 0;           // condition: '<', '>', '=', 'l' (<=), 'g' (>=), 'n' (<>); 0: none
    double limit = 0;
    uint8_t color = 0;
    char system = 0;       // 'D': Windows' long date ([$-F800]), 'T': its time ([$-F400])
    uint32_t lcid = 0;     // [$-407]: names in that language
    // Numbers: integer and decimal placeholders (mantissa's for scientific), grouping, and the
    // power of ten the value is multiplied by (2 per %, -3 per scaling comma).
    int intDigits = 0, fracDigits = 0, expDigits = 0, scale = 0;
    bool group = false;
    bool hasPoint = false;
    // Fractions: token ranges [0, intEnd) integer part, [intEnd, numStart) between, numerator up to
    // the slash, denominator up to denEnd; fixedDen 0 when it's placeholders.
    uint32_t intEnd = 0, numStart = 0, slash = 0, denEnd = 0;
    int numDigits = 0, denDigits = 0;
    uint32_t fixedDen = 0;
    // Dates: 12-hour clock, the seconds' decimals, and a single elapsed unit and nothing else
    // ([h] alone: negative values and any size are shown).
    bool ampm = false, elapsedOnly = false;
    int secDigits = 0;
  };

 private:
  std::vector<Section> sections_;
  std::wstring pool_;  // literal text
  bool general_ = true;
  bool localAmPm_ = false;  // AM/PM shows the locale's designators (built-ins 18 and 19)
  friend struct Formatter;
  friend struct Compiler;
  friend struct FormatAccess;
};

// Excel's code for a built-in format id in the locale ("" for General).
std::wstring BuiltinCode(int id, const NumLocale& locale);

// The default palette's color for an index 1..56 (0x00RRGGBB); a workbook's indexedColors replace it.
uint32_t PaletteColor(int index);

// A number as General shows it in at most `width` characters, not counting a minus sign
// (11 in a wide cell); empty when it can't be shown in that width.
std::wstring FormatGeneral(double value, int width, const NumLocale& locale);

// The date of a serial number: false outside 0..9999-12-31. 1900 system: 0 is 1900-01-00 and 60 is
// 1900-02-29, as in Excel.
bool SerialToDate(double serial, bool date1904, int& year, int& month, int& day);
// A date's serial number (day may run over the month, as DATE() allows); false when out of range.
bool DateToSerial(int year, int month, int day, bool date1904, double& serial);

}  // namespace fw::sheet
