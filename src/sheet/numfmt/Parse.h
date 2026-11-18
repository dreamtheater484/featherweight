#pragma once

// Reading values as Excel reads typed text (plan §7.1, §7.3; CSV now, typing in S2): numbers with
// the locale's separators, an exponent or parentheses, percentages, currency amounts, dates and times
// in the locale's order, and TRUE/FALSE. Error values are the model's (ParseError).

#include <string>
#include <string_view>

#include "sheet/numfmt/NumLocale.h"

namespace fw::sheet {

struct ParsedValue {
  enum Kind : uint8_t { Number, Bool } kind = Number;
  double number = 0;  // the number, a date's serial, or 1/0
  // A format code that shows the value as it was written ("1,234.50" gives #,##0.00, "15/03/2023"
  // dd/mm/yyyy); empty for General.
  std::wstring code;
  bool date = false;   // a date or time
  bool plain = false;  // digits, a sign and decimals only: Excel types it as General
};

// Reads text the way Excel reads what's typed; false when Excel would keep it as text. Dates need a
// day, month and year ("1-2" and "MAR1" stay text), times need two-digit minutes.
bool ParseValue(std::wstring_view text, const NumLocale& locale, bool date1904, ParsedValue& out);

// ParseValue, kept only when the value shows exactly as `text` in its format (General first for a
// plain number): CSV's rule, so that reading a file never changes a value. Leading zeros, more than 15
// digits and anything Excel would rewrite stay text.
bool ParseExact(std::wstring_view text, const NumLocale& locale, bool date1904, ParsedValue& out);

// What typing into a General cell gives (S2), as Excel converts it (measured on Excel 2021): more
// than CSV takes ("1-2" is 1 February this year, "0 1/2" a half), and the built-in format the cell
// gets.
struct TypedValue {
  enum Kind : uint8_t { Text, Number, Bool, Error } kind = Text;
  double number = 0;    // Number; Bool: 1 or 0
  std::wstring text;    // Text: the cell's text (an apostrophe's without it); Error: its name ("#N/A")
  bool quoted = false;  // typed with a leading apostrophe (the cell's quotePrefix)
  // The built-in format Excel gives the cell (0: General): 3, 4 grouped; 6, 8 currency; 9, 10
  // percent; 11 scientific; 12, 13 fractions; 14..22, 46, 47 dates and times. A number too big for
  // Excel keeps its text but gets 11, as in Excel.
  int format = 0;
};

// `year` is this year, for a date typed without one ("1-2"). Always succeeds: text is a value too.
void ParseTyped(std::wstring_view typed, const NumLocale& locale, bool date1904, int year, TypedValue& out);

// The format (numFmt id) of a cell with format `cellFormat` after typing a value that reads with
// built-in format `typedFormat` (TypedValue::format), as Excel does (measured on Excel 2021):
// General takes the typed format. A built-in currency (5-8), accounting (41-44), percent (9, 10),
// scientific (11, 48), fraction (12, 13), date (14-17, 22) or time (18-21, 45-47) format takes
// it when the value is of another of those kinds ("50%" into a date cell gives 0%; "12:30" into
// h:mm:ss keeps h:mm:ss). Number built-ins (1-4, 37-40), text (49) and custom formats (164 and up)
// keep theirs, and so does every cell for a plain or grouped number, text, TRUE or an error.
uint32_t TypedFormat(uint32_t cellFormat, int typedFormat);

}  // namespace fw::sheet
