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

}  // namespace fw::sheet
