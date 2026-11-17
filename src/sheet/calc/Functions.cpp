#include "sheet/calc/Functions.h"

#include <algorithm>
#include <vector>

#include "sheet/calc/Fn.h"

namespace fw::sheet::calc {
namespace {

constexpr uint8_t V = kFnVolatile, L = kFnLazy, D = kFnDynRef, X = kFnXlfn, W = kFnXlfn | kFnXlws;

// Argument classes: see FuncInfo::params. "+" starts a group that repeats (SUMIFS: "R+RV").
const FuncInfo kFunctions[] = {
    {"ABS", 1, 1, 0, "V", fn::Abs},
    {"ADDRESS", 2, 5, 0, "V", fn::Address},
    {"AND", 1, 255, 0, "R", fn::And},
    {"AVERAGE", 1, 255, 0, "R", fn::Average},
    {"AVERAGEA", 1, 255, 0, "R", fn::AverageA},
    {"AVERAGEIF", 2, 3, 0, "RVR", fn::AverageIf},
    {"AVERAGEIFS", 3, 255, 0, "R+RV", fn::AverageIfs},
    {"CEILING", 2, 2, 0, "V", fn::Ceiling},
    {"CEILING.MATH", 1, 3, X, "V", fn::CeilingMath},
    {"CHAR", 1, 1, 0, "V", fn::Char},
    {"CHOOSE", 2, 255, L, "VR", nullptr},
    {"CLEAN", 1, 1, 0, "V", fn::Clean},
    {"CODE", 1, 1, 0, "V", fn::Code},
    {"COLUMN", 0, 1, 0, "R", fn::Column},
    {"COLUMNS", 1, 1, 0, "R", fn::Columns},
    {"CONCAT", 1, 255, X, "R", fn::Concat},
    {"CONCATENATE", 1, 255, 0, "V", fn::Concatenate},
    {"COUNT", 1, 255, 0, "R", fn::Count},
    {"COUNTA", 1, 255, 0, "R", fn::CountA},
    {"COUNTBLANK", 1, 1, 0, "R", fn::CountBlank},
    {"COUNTIF", 2, 2, 0, "RV", fn::CountIf},
    {"COUNTIFS", 2, 254, 0, "+RV", fn::CountIfs},
    {"DATE", 3, 3, 0, "V", fn::Date},
    {"DATEDIF", 3, 3, 0, "V", fn::DateDif},
    {"DATEVALUE", 1, 1, 0, "V", fn::DateValue},
    {"DAY", 1, 1, 0, "V", fn::Day},
    {"DAYS", 2, 2, X, "V", fn::Days},
    {"DOLLAR", 1, 2, 0, "V", fn::Dollar},
    {"EDATE", 2, 2, 0, "V", fn::EDate},
    {"EOMONTH", 2, 2, 0, "V", fn::EOMonth},
    {"ERROR.TYPE", 1, 1, 0, "V", fn::ErrorType},
    {"EVEN", 1, 1, 0, "V", fn::Even},
    {"EXACT", 2, 2, 0, "V", fn::Exact},
    {"EXP", 1, 1, 0, "V", fn::Exp},
    {"FALSE", 0, 0, 0, "V", fn::False},
    {"FILTER", 2, 3, W, "A", fn::Filter},
    {"FIND", 2, 3, 0, "V", fn::Find},
    {"FIXED", 1, 3, 0, "V", fn::Fixed},
    {"FLOOR", 2, 2, 0, "V", fn::Floor},
    {"FLOOR.MATH", 1, 3, X, "V", fn::FloorMath},
    {"FV", 3, 5, 0, "V", fn::Fv},
    {"HLOOKUP", 3, 4, 0, "VRVV", fn::HLookup},
    {"HOUR", 1, 1, 0, "V", fn::Hour},
    {"HYPERLINK", 1, 2, 0, "V", fn::Hyperlink},
    {"IF", 2, 3, L, "VRR", nullptr},
    {"IFERROR", 2, 2, L, "VV", nullptr},
    {"IFNA", 2, 2, L | X, "VV", nullptr},
    {"IFS", 2, 254, L | X, "+VR", nullptr},
    {"INDEX", 2, 4, 0, "RVVV", fn::Index},
    {"INDIRECT", 1, 2, V | D, "V", fn::Indirect},
    {"INT", 1, 1, 0, "V", fn::Int},
    {"ISBLANK", 1, 1, 0, "V", fn::IsBlank},
    {"ISERR", 1, 1, 0, "V", fn::IsErr},
    {"ISERROR", 1, 1, 0, "V", fn::IsError},
    {"ISEVEN", 1, 1, 0, "V", fn::IsEven},
    {"ISFORMULA", 1, 1, X, "R", nullptr},
    {"ISLOGICAL", 1, 1, 0, "V", fn::IsLogical},
    {"ISNA", 1, 1, 0, "V", fn::IsNA},
    {"ISNONTEXT", 1, 1, 0, "V", fn::IsNonText},
    {"ISNUMBER", 1, 1, 0, "V", fn::IsNumber},
    {"ISODD", 1, 1, 0, "V", fn::IsOdd},
    {"ISOWEEKNUM", 1, 1, X, "V", fn::IsoWeekNum},
    {"ISREF", 1, 1, 0, "R", fn::IsRef},
    {"ISTEXT", 1, 1, 0, "V", fn::IsText},
    {"LARGE", 2, 2, 0, "RV", fn::Large},
    {"LEFT", 1, 2, 0, "V", fn::Left},
    {"LEN", 1, 1, 0, "V", fn::Len},
    {"LET", 3, 253, L | X, "V", nullptr},
    {"LN", 1, 1, 0, "V", fn::Ln},
    {"LOG", 1, 2, 0, "V", fn::Log},
    {"LOG10", 1, 1, 0, "V", fn::Log10},
    {"LOOKUP", 2, 3, 0, "VRR", fn::Lookup},
    {"LOWER", 1, 1, 0, "V", fn::Lower},
    {"MATCH", 2, 3, 0, "VRV", fn::Match},
    {"MAX", 1, 255, 0, "R", fn::Max},
    {"MAXA", 1, 255, 0, "R", fn::MaxA},
    {"MAXIFS", 3, 255, X, "R+RV", fn::MaxIfs},
    {"MEDIAN", 1, 255, 0, "R", fn::Median},
    {"MID", 3, 3, 0, "V", fn::Mid},
    {"MIN", 1, 255, 0, "R", fn::Min},
    {"MINA", 1, 255, 0, "R", fn::MinA},
    {"MINIFS", 3, 255, X, "R+RV", fn::MinIfs},
    {"MINUTE", 1, 1, 0, "V", fn::Minute},
    {"MOD", 2, 2, 0, "V", fn::Mod},
    {"MODE", 1, 255, 0, "R", fn::Mode},
    {"MODE.SNGL", 1, 255, X, "R", fn::Mode},
    {"MONTH", 1, 1, 0, "V", fn::Month},
    {"MROUND", 2, 2, 0, "V", fn::MRound},
    {"N", 1, 1, 0, "V", fn::N},
    {"NA", 0, 0, 0, "V", fn::NA},
    {"NETWORKDAYS", 2, 3, 0, "VVR", fn::NetworkDays},
    {"NETWORKDAYS.INTL", 2, 4, X, "VVVR", fn::NetworkDaysIntl},
    {"NOT", 1, 1, 0, "V", fn::Not},
    {"NOW", 0, 0, V, "V", fn::Now},
    {"NPER", 3, 5, 0, "V", fn::NPer},
    {"NPV", 2, 255, 0, "VR", fn::Npv},
    {"NUMBERVALUE", 1, 3, X, "V", fn::NumberValue},
    {"ODD", 1, 1, 0, "V", fn::Odd},
    {"OFFSET", 3, 5, V | D, "RV", fn::Offset},
    {"OR", 1, 255, 0, "R", fn::Or},
    {"PERCENTILE", 2, 2, 0, "RV", fn::Percentile},
    {"PERCENTILE.INC", 2, 2, X, "RV", fn::Percentile},
    {"PI", 0, 0, 0, "V", fn::Pi},
    {"PMT", 3, 5, 0, "V", fn::Pmt},
    {"POWER", 2, 2, 0, "V", fn::Power},
    {"PRODUCT", 1, 255, 0, "R", fn::Product},
    {"PROPER", 1, 1, 0, "V", fn::Proper},
    {"PV", 3, 5, 0, "V", fn::Pv},
    {"QUARTILE", 2, 2, 0, "RV", fn::Quartile},
    {"QUARTILE.INC", 2, 2, X, "RV", fn::Quartile},
    {"QUOTIENT", 2, 2, 0, "V", fn::Quotient},
    {"RAND", 0, 0, V, "V", fn::Rand},
    {"RANDBETWEEN", 2, 2, V, "V", fn::RandBetween},
    {"RANK", 2, 3, 0, "VRV", fn::Rank},
    {"RANK.AVG", 2, 3, X, "VRV", fn::RankAvg},
    {"RANK.EQ", 2, 3, X, "VRV", fn::Rank},
    {"RATE", 3, 6, 0, "V", fn::Rate},
    {"REPLACE", 4, 4, 0, "V", fn::Replace},
    {"REPT", 2, 2, 0, "V", fn::Rept},
    {"RIGHT", 1, 2, 0, "V", fn::Right},
    {"ROUND", 2, 2, 0, "V", fn::Round},
    {"ROUNDDOWN", 2, 2, 0, "V", fn::RoundDown},
    {"ROUNDUP", 2, 2, 0, "V", fn::RoundUp},
    {"ROW", 0, 1, 0, "R", fn::Row},
    {"ROWS", 1, 1, 0, "R", fn::Rows},
    {"SEARCH", 2, 3, 0, "V", fn::Search},
    {"SECOND", 1, 1, 0, "V", fn::Second},
    {"SEQUENCE", 1, 4, X, "V", fn::Sequence},
    {"SIGN", 1, 1, 0, "V", fn::Sign},
    {"SMALL", 2, 2, 0, "RV", fn::Small},
    {"SORT", 1, 4, W, "AAAV", fn::Sort},
    {"SQRT", 1, 1, 0, "V", fn::Sqrt},
    {"STDEV", 1, 255, 0, "R", fn::StDev},
    {"STDEV.P", 1, 255, X, "R", fn::StDevP},
    {"STDEV.S", 1, 255, X, "R", fn::StDev},
    {"STDEVP", 1, 255, 0, "R", fn::StDevP},
    {"SUBSTITUTE", 3, 4, 0, "V", fn::Substitute},
    {"SUBTOTAL", 2, 255, 0, "VR", fn::Subtotal},
    {"SUM", 1, 255, 0, "R", fn::Sum},
    {"SUMIF", 2, 3, 0, "RVR", fn::SumIf},
    {"SUMIFS", 3, 255, 0, "R+RV", fn::SumIfs},
    {"SUMPRODUCT", 1, 255, 0, "A", fn::SumProduct},
    {"SUMSQ", 1, 255, 0, "R", fn::SumSq},
    {"SWITCH", 3, 254, L | X, "V+VR", nullptr},
    {"T", 1, 1, 0, "V", fn::T},
    {"TEXT", 2, 2, 0, "V", fn::Text},
    {"TEXTJOIN", 3, 255, X, "RVR", fn::TextJoin},
    {"TIME", 3, 3, 0, "V", fn::Time},
    {"TIMEVALUE", 1, 1, 0, "V", fn::TimeValue},
    {"TODAY", 0, 0, V, "V", fn::Today},
    {"TRANSPOSE", 1, 1, 0, "A", fn::Transpose},
    {"TRIM", 1, 1, 0, "V", fn::Trim},
    {"TRUE", 0, 0, 0, "V", fn::True},
    {"TRUNC", 1, 2, 0, "V", fn::Trunc},
    {"TYPE", 1, 1, 0, "R", fn::TypeFn},
    {"UNIQUE", 1, 3, X, "AV", fn::Unique},
    {"UPPER", 1, 1, 0, "V", fn::Upper},
    {"VALUE", 1, 1, 0, "V", fn::ValueFn},
    {"VAR", 1, 255, 0, "R", fn::Var},
    {"VAR.P", 1, 255, X, "R", fn::VarP},
    {"VAR.S", 1, 255, X, "R", fn::Var},
    {"VARP", 1, 255, 0, "R", fn::VarP},
    {"VLOOKUP", 3, 4, 0, "VRVV", fn::VLookup},
    {"WEEKDAY", 1, 2, 0, "V", fn::Weekday},
    {"WEEKNUM", 1, 2, 0, "V", fn::WeekNum},
    {"WORKDAY", 2, 3, 0, "VVR", fn::Workday},
    {"WORKDAY.INTL", 2, 4, X, "VVVR", fn::WorkdayIntl},
    {"XLOOKUP", 3, 6, X, "VRRVVV", fn::XLookup},
    {"XMATCH", 2, 4, X, "VRVV", fn::XMatch},
    {"XOR", 1, 255, X, "R", fn::Xor},
    {"YEAR", 1, 1, 0, "V", fn::Year},
    {"YEARFRAC", 2, 3, 0, "V", fn::YearFrac},
};

char Upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; }

int CompareName(std::string_view a, std::string_view b) {  // a in any case, b upper case
  size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    char x = Upper(a[i]), y = b[i];
    if (x != y) return static_cast<unsigned char>(x) < static_cast<unsigned char>(y) ? -1 : 1;
  }
  return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
}

}  // namespace

int FindFunction(std::string_view name) {
  // The table is in alphabetical order (a unit test checks).
  int lo = 0, hi = static_cast<int>(std::size(kFunctions)) - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    int c = CompareName(name, kFunctions[mid].name);
    if (c == 0) return mid;
    if (c < 0) hi = mid - 1;
    else lo = mid + 1;
  }
  return -1;
}

const FuncInfo& Function(int id) { return kFunctions[id]; }
int FunctionCount() { return static_cast<int>(std::size(kFunctions)); }

char ParamClass(const FuncInfo& f, int i) {
  std::string_view p = f.params;
  size_t plus = p.find('+');
  if (plus == std::string_view::npos) {
    if (p.empty()) return 'V';
    return p[std::min(static_cast<size_t>(i), p.size() - 1)];
  }
  if (static_cast<size_t>(i) < plus) return p[static_cast<size_t>(i)];
  std::string_view group = p.substr(plus + 1);
  if (group.empty()) return 'V';
  return group[(static_cast<size_t>(i) - plus) % group.size()];
}

}  // namespace fw::sheet::calc
