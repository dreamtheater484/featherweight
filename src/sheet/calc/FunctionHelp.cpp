// The function list for AutoComplete and argument tips: Excel's argument names, short descriptions.

#include <cctype>
#include <cstring>

#include "sheet/calc/Typing.h"

namespace fw::sheet::calc {

namespace {

// A to Z. Arguments as Excel names them; [optional]; "..." repeats the one before it.
const FunctionHelp kHelp[] = {
    {"ABS", "number", "The number without its sign."},
    {"ADDRESS", "row_num, column_num, [abs_num], [a1], [sheet_text]", "A cell's reference as text."},
    {"AGGREGATE", "function_num, options, ref1, [ref2], ...", "A total that can leave out hidden rows and errors."},
    {"AND", "logical1, [logical2], ...", "TRUE when every argument is TRUE."},
    {"AVERAGE", "number1, [number2], ...", "The average of the numbers."},
    {"AVERAGEA", "value1, [value2], ...", "The average, counting text as 0 and TRUE as 1."},
    {"AVERAGEIF", "range, criteria, [average_range]", "The average of the cells that meet a condition."},
    {"AVERAGEIFS", "average_range, criteria_range1, criteria1, ...", "The average of the cells that meet every condition."},
    {"CEILING", "number, significance", "Rounds up to a multiple of significance."},
    {"CEILING.MATH", "number, [significance], [mode]", "Rounds up to an integer or a multiple."},
    {"CHAR", "number", "The character with this code."},
    {"CHOOSE", "index_num, value1, [value2], ...", "The value at the position index_num."},
    {"CLEAN", "text", "The text without characters that can't be printed."},
    {"CODE", "text", "The code of the text's first character."},
    {"COLUMN", "[reference]", "The column number of a reference."},
    {"COLUMNS", "array", "The number of columns."},
    {"CONCAT", "text1, [text2], ...", "Joins texts and ranges."},
    {"CONCATENATE", "text1, [text2], ...", "Joins texts."},
    {"COUNT", "value1, [value2], ...", "Counts the numbers."},
    {"COUNTA", "value1, [value2], ...", "Counts the cells that aren't empty."},
    {"COUNTBLANK", "range", "Counts the empty cells."},
    {"COUNTIF", "range, criteria", "Counts the cells that meet a condition."},
    {"COUNTIFS", "criteria_range1, criteria1, ...", "Counts the cells that meet every condition."},
    {"DATE", "year, month, day", "The date of a year, month and day."},
    {"DATEDIF", "start_date, end_date, unit", "Years, months or days between two dates."},
    {"DATEVALUE", "date_text", "A date typed as text, as a date."},
    {"DAY", "serial_number", "The day of the month, 1 to 31."},
    {"DAYS", "end_date, start_date", "The number of days between two dates."},
    {"DOLLAR", "number, [decimals]", "A number as currency text."},
    {"EDATE", "start_date, months", "The date a number of months later."},
    {"EOMONTH", "start_date, months", "The last day of a month a number of months later."},
    {"ERROR.TYPE", "error_val", "The number of an error value."},
    {"EVEN", "number", "Rounds away from zero to an even integer."},
    {"EXACT", "text1, text2", "TRUE when two texts are the same, case included."},
    {"EXP", "number", "e raised to the power of the number."},
    {"FALSE", "", "The value FALSE."},
    {"FILTER", "array, include, [if_empty]", "The rows or columns that meet a condition."},
    {"FIND", "find_text, within_text, [start_num]", "Where a text starts in another, case sensitive."},
    {"FIXED", "number, [decimals], [no_commas]", "A rounded number as text."},
    {"FLOOR", "number, significance", "Rounds down to a multiple of significance."},
    {"FLOOR.MATH", "number, [significance], [mode]", "Rounds down to an integer or a multiple."},
    {"FV", "rate, nper, pmt, [pv], [type]", "The future value of an investment."},
    {"HLOOKUP", "lookup_value, table_array, row_index_num, [range_lookup]", "Looks up a value in the top row and returns one below it."},
    {"HOUR", "serial_number", "The hour, 0 to 23."},
    {"HYPERLINK", "link_location, [friendly_name]", "A link to open."},
    {"IF", "logical_test, [value_if_true], [value_if_false]", "One value if a condition is TRUE, another if it's FALSE."},
    {"IFERROR", "value, value_if_error", "The value, or another one when it's an error."},
    {"IFNA", "value, value_if_na", "The value, or another one when it's #N/A."},
    {"IFS", "logical_test1, value_if_true1, ...", "The value of the first condition that's TRUE."},
    {"INDEX", "array, row_num, [column_num], [area_num]", "The value at a row and column of a range."},
    {"INDIRECT", "ref_text, [a1]", "The reference a text names."},
    {"INFO", "type_text", "Information about the environment."},
    {"INT", "number", "Rounds down to an integer."},
    {"ISBLANK", "value", "TRUE when the cell is empty."},
    {"ISERR", "value", "TRUE for an error other than #N/A."},
    {"ISERROR", "value", "TRUE for any error."},
    {"ISEVEN", "number", "TRUE for an even number."},
    {"ISFORMULA", "reference", "TRUE when the cell holds a formula."},
    {"ISLOGICAL", "value", "TRUE for TRUE or FALSE."},
    {"ISNA", "value", "TRUE for #N/A."},
    {"ISNONTEXT", "value", "TRUE for anything but text."},
    {"ISNUMBER", "value", "TRUE for a number."},
    {"ISODD", "number", "TRUE for an odd number."},
    {"ISOWEEKNUM", "date", "The ISO week number of a date."},
    {"ISREF", "value", "TRUE for a reference."},
    {"ISTEXT", "value", "TRUE for text."},
    {"LARGE", "array, k", "The k-th largest value."},
    {"LEFT", "text, [num_chars]", "The first characters of a text."},
    {"LEN", "text", "The number of characters."},
    {"LET", "name1, name_value1, calculation_or_name2, ...", "Names values to use in a calculation."},
    {"LN", "number", "The natural logarithm."},
    {"LOG", "number, [base]", "The logarithm to a base (10 when left out)."},
    {"LOG10", "number", "The base-10 logarithm."},
    {"LOOKUP", "lookup_value, lookup_vector, [result_vector]", "Looks up a value in one row or column."},
    {"LOWER", "text", "The text in lowercase."},
    {"MATCH", "lookup_value, lookup_array, [match_type]", "The position of a value in a range."},
    {"MAX", "number1, [number2], ...", "The largest number."},
    {"MAXA", "value1, [value2], ...", "The largest value, counting text as 0."},
    {"MAXIFS", "max_range, criteria_range1, criteria1, ...", "The largest value of the cells that meet every condition."},
    {"MEDIAN", "number1, [number2], ...", "The middle number."},
    {"MID", "text, start_num, num_chars", "Characters from the middle of a text."},
    {"MIN", "number1, [number2], ...", "The smallest number."},
    {"MINA", "value1, [value2], ...", "The smallest value, counting text as 0."},
    {"MINIFS", "min_range, criteria_range1, criteria1, ...", "The smallest value of the cells that meet every condition."},
    {"MINUTE", "serial_number", "The minute, 0 to 59."},
    {"MOD", "number, divisor", "The remainder of a division."},
    {"MODE", "number1, [number2], ...", "The most frequent number."},
    {"MODE.SNGL", "number1, [number2], ...", "The most frequent number."},
    {"MONTH", "serial_number", "The month, 1 to 12."},
    {"MROUND", "number, multiple", "Rounds to the nearest multiple."},
    {"N", "value", "The value as a number."},
    {"NA", "", "The error #N/A."},
    {"NETWORKDAYS", "start_date, end_date, [holidays]", "The number of working days between two dates."},
    {"NETWORKDAYS.INTL", "start_date, end_date, [weekend], [holidays]", "Working days between two dates, with chosen weekend days."},
    {"NOT", "logical", "TRUE for FALSE and FALSE for TRUE."},
    {"NOW", "", "The current date and time."},
    {"NPER", "rate, pmt, pv, [fv], [type]", "The number of payments of a loan or investment."},
    {"NPV", "rate, value1, [value2], ...", "The net present value of cash flows."},
    {"NUMBERVALUE", "text, [decimal_separator], [group_separator]", "A text as a number, with chosen separators."},
    {"ODD", "number", "Rounds away from zero to an odd integer."},
    {"OFFSET", "reference, rows, cols, [height], [width]", "A range moved by rows and columns from a reference."},
    {"OR", "logical1, [logical2], ...", "TRUE when any argument is TRUE."},
    {"PERCENTILE", "array, k", "The k-th percentile."},
    {"PERCENTILE.EXC", "array, k", "The k-th percentile, k between 0 and 1 exclusive."},
    {"PERCENTILE.INC", "array, k", "The k-th percentile, k from 0 to 1."},
    {"PI", "", "The number pi."},
    {"PMT", "rate, nper, pv, [fv], [type]", "The payment of a loan."},
    {"POWER", "number, power", "A number raised to a power."},
    {"PRODUCT", "number1, [number2], ...", "Multiplies the numbers."},
    {"PROPER", "text", "The text with each word capitalized."},
    {"PV", "rate, nper, pmt, [fv], [type]", "The present value of an investment."},
    {"QUARTILE", "array, quart", "A quartile of the values."},
    {"QUARTILE.EXC", "array, quart", "A quartile, from percentiles between 0 and 1 exclusive."},
    {"QUARTILE.INC", "array, quart", "A quartile of the values."},
    {"QUOTIENT", "numerator, denominator", "The integer part of a division."},
    {"RAND", "", "A random number from 0 to 1."},
    {"RANDBETWEEN", "bottom, top", "A random integer between two numbers."},
    {"RANK", "number, ref, [order]", "The rank of a number in a list."},
    {"RANK.AVG", "number, ref, [order]", "The rank of a number; ties get the average rank."},
    {"RANK.EQ", "number, ref, [order]", "The rank of a number; ties get the top rank."},
    {"RATE", "nper, pmt, pv, [fv], [type], [guess]", "The interest rate per period."},
    {"REPLACE", "old_text, start_num, num_chars, new_text", "Replaces characters at a position."},
    {"REPT", "text, number_times", "A text repeated."},
    {"RIGHT", "text, [num_chars]", "The last characters of a text."},
    {"ROUND", "number, num_digits", "Rounds to a number of digits."},
    {"ROUNDDOWN", "number, num_digits", "Rounds toward zero."},
    {"ROUNDUP", "number, num_digits", "Rounds away from zero."},
    {"ROW", "[reference]", "The row number of a reference."},
    {"ROWS", "array", "The number of rows."},
    {"SEARCH", "find_text, within_text, [start_num]", "Where a text starts in another, any case, with wildcards."},
    {"SECOND", "serial_number", "The second, 0 to 59."},
    {"SEQUENCE", "rows, [columns], [start], [step]", "A list of sequential numbers."},
    {"SIGN", "number", "1, 0 or -1 for the number's sign."},
    {"SMALL", "array, k", "The k-th smallest value."},
    {"SORT", "array, [sort_index], [sort_order], [by_col]", "The values sorted."},
    {"SORTBY", "array, by_array1, [sort_order1], ...", "The values sorted by another range."},
    {"SQRT", "number", "The square root."},
    {"STDEV", "number1, [number2], ...", "The standard deviation of a sample."},
    {"STDEV.P", "number1, [number2], ...", "The standard deviation of a whole population."},
    {"STDEV.S", "number1, [number2], ...", "The standard deviation of a sample."},
    {"STDEVP", "number1, [number2], ...", "The standard deviation of a whole population."},
    {"SUBSTITUTE", "text, old_text, new_text, [instance_num]", "Replaces a text with another."},
    {"SUBTOTAL", "function_num, ref1, [ref2], ...", "A total of a list, without other subtotals."},
    {"SUM", "number1, [number2], ...", "Adds the numbers."},
    {"SUMIF", "range, criteria, [sum_range]", "Adds the cells that meet a condition."},
    {"SUMIFS", "sum_range, criteria_range1, criteria1, ...", "Adds the cells that meet every condition."},
    {"SUMPRODUCT", "array1, [array2], ...", "Multiplies ranges item by item and adds the products."},
    {"SUMSQ", "number1, [number2], ...", "Adds the squares of the numbers."},
    {"SWITCH", "expression, value1, result1, ...", "The result for the first value that matches."},
    {"T", "value", "The value if it's text, else an empty text."},
    {"TEXT", "value, format_text", "A value as text in a number format."},
    {"TEXTJOIN", "delimiter, ignore_empty, text1, ...", "Joins texts with a delimiter."},
    {"TIME", "hour, minute, second", "The time of an hour, minute and second."},
    {"TIMEVALUE", "time_text", "A time typed as text, as a time."},
    {"TODAY", "", "Today's date."},
    {"TRANSPOSE", "array", "Rows as columns and columns as rows."},
    {"TRIM", "text", "The text without extra spaces."},
    {"TRUE", "", "The value TRUE."},
    {"TRUNC", "number, [num_digits]", "Cuts off the decimals."},
    {"TYPE", "value", "A number for the kind of value."},
    {"UNIQUE", "array, [by_col], [exactly_once]", "The distinct values."},
    {"UPPER", "text", "The text in capitals."},
    {"VALUE", "text", "A text as a number."},
    {"VAR", "number1, [number2], ...", "The variance of a sample."},
    {"VAR.P", "number1, [number2], ...", "The variance of a whole population."},
    {"VAR.S", "number1, [number2], ...", "The variance of a sample."},
    {"VARP", "number1, [number2], ...", "The variance of a whole population."},
    {"VLOOKUP", "lookup_value, table_array, col_index_num, [range_lookup]", "Looks up a value in the first column and returns one beside it."},
    {"WEEKDAY", "serial_number, [return_type]", "The day of the week as a number."},
    {"WEEKNUM", "serial_number, [return_type]", "The week number in the year."},
    {"WORKDAY", "start_date, days, [holidays]", "The date a number of working days later."},
    {"WORKDAY.INTL", "start_date, days, [weekend], [holidays]", "A working day later, with chosen weekend days."},
    {"XLOOKUP", "lookup_value, lookup_array, return_array, [if_not_found], [match_mode], [search_mode]", "Looks up a value and returns the matching item."},
    {"XMATCH", "lookup_value, lookup_array, [match_mode], [search_mode]", "The position of a value in a range."},
    {"XOR", "logical1, [logical2], ...", "TRUE when an odd number of arguments are TRUE."},
    {"YEAR", "serial_number", "The year."},
    {"YEARFRAC", "start_date, end_date, [basis]", "The fraction of a year between two dates."},
};

bool SameName(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

}  // namespace

const FunctionHelp* FunctionHelpList(size_t& count) {
  count = sizeof kHelp / sizeof kHelp[0];
  return kHelp;
}

const FunctionHelp* FindFunctionHelp(std::string_view name) {
  for (std::string_view prefix : {"_xlfn._xlws.", "_xlfn.", "_xlws."})
    if (name.size() > prefix.size() && SameName(name.substr(0, prefix.size()), prefix)) {
      name.remove_prefix(prefix.size());
      break;
    }
  size_t lo = 0, hi = sizeof kHelp / sizeof kHelp[0];
  while (lo < hi) {  // A to Z
    size_t mid = (lo + hi) / 2;
    std::string upper(name);
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    int cmp = std::strcmp(kHelp[mid].name, upper.c_str());
    if (cmp == 0) return &kHelp[mid];
    if (cmp < 0) lo = mid + 1;
    else hi = mid;
  }
  return nullptr;
}

std::wstring ArgumentTip(const FunctionHelp& help, const FormulaLocale& region, int argument, size_t& boldBegin,
                         size_t& boldEnd) {
  std::wstring tip;
  for (const char* p = help.name; *p; ++p) tip += static_cast<wchar_t>(*p);
  tip += L'(';
  boldBegin = boldEnd = 0;
  // Arguments past the last named one fall on the repeated ones ("number2" for the 5th of SUM).
  int count = 0;
  for (const char* p = help.args; *p; ++p) count += *p == ',';
  const bool repeats = std::strstr(help.args, "...") != nullptr;
  int target = argument;
  if (repeats && target >= count) target = count - 1;
  int k = 0;
  bool started = false;
  for (const char* p = help.args; *p; ++p) {
    if (*p == ',') {
      tip += static_cast<wchar_t>(region.list);
      ++k;
      continue;
    }
    if (k == target && !started && *p != ' ') boldBegin = tip.size(), started = true;
    tip += static_cast<wchar_t>(*p);
    if (k == target && (p[1] == ',' || p[1] == 0)) boldEnd = tip.size();
  }
  tip += L')';
  if (boldEnd < boldBegin) boldEnd = boldBegin;
  return tip;
}

}  // namespace fw::sheet::calc
