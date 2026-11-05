#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fw::doc {

// Dates as Word writes and shows them: DATE and TIME fields, and date content controls.

struct DateTime {
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  bool operator==(const DateTime&) const = default;
};

// `t` by a Word date-time picture (a DATE field's \@ switch, a date control's w:dateFormat): M MM
// MMM MMMM, d dd ddd dddd, yy yyyy, h hh H HH, m mm, s ss, AM/PM am/pm A/P a/p, 'literal text';
// month and day names in `lang` (BCP 47; the user's language when empty or unknown). With no
// picture, the language's short date, or with `time` its time without seconds.
std::u16string FormatDateTime(const DateTime& t, std::u16string_view picture, bool time, std::string_view lang);

// A FILETIME (100-nanosecond ticks since 1601, UTC) in local time; and the local time now.
DateTime LocalDateTime(int64_t fileTime);
DateTime Now();

// A date content control's w:fullDate ("2026-11-05T00:00:00Z"), and back.
std::string FullDate(const DateTime& t);
std::optional<DateTime> ParseFullDate(std::string_view s);

}  // namespace fw::doc
