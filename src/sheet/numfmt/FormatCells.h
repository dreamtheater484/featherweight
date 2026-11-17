#pragma once

// The Number tab of Format Cells (Ctrl+1, plan §7.2) as a model: a category and its options make a
// format code (invariant; NumberFormat::InvariantToLocal shows it as the region types it), and a
// cell's code opens the tab on the category and options that make it, else Custom.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sheet/numfmt/NumLocale.h"

namespace fw::sheet {

enum class FormatCategory : uint8_t {
  General, Number, Currency, Accounting, Date, Time, Percentage, Fraction, Scientific, Text, Custom
};

struct FormatChoice {
  FormatCategory category = FormatCategory::General;
  int decimals = 2;        // Number, Currency, Accounting, Percentage, Scientific (0..30)
  bool thousands = false;  // Number: the 1000 separator
  int negative = 0;        // Number, Currency: -1234.10, red 1234.10, (1234.10), red (1234.10)
  int type = 0;            // Date, Time, Fraction: an index into FormatTypes
  std::wstring code;       // Custom: the code (invariant)
  bool operator==(const FormatChoice&) const = default;
};

// The code a choice makes (invariant); General is "General".
std::wstring FormatCode(const FormatChoice& choice, const NumLocale& locale);
// The choice the tab opens on for a code (invariant): the first category and options that make
// exactly this code, else Custom with the code.
FormatChoice FormatChoiceOf(std::wstring_view code, const NumLocale& locale);
// The listed types of Date, Time and Fraction (invariant codes); empty for the others.
std::vector<std::wstring> FormatTypes(FormatCategory category, const NumLocale& locale);

}  // namespace fw::sheet
