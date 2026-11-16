#include "sheet/calc/Functions.h"

#include <algorithm>
#include <vector>


namespace fw::sheet::calc {
namespace {

constexpr uint8_t V = kFnVolatile, L = kFnLazy, D = kFnDynRef, X = kFnXlfn, W = kFnXlfn | kFnXlws;

// Argument classes: see FuncInfo::params. "+" starts a group that repeats (SUMIFS: "R+RV").
const FuncInfo kFunctions[] = {
    {"ABS", 1, 1, 0, "V", nullptr},
    {"ADDRESS", 2, 5, 0, "V", nullptr},
    {"AND", 1, 255, 0, "R", nullptr},
    {"AVERAGE", 1, 255, 0, "R", nullptr},
    {"AVERAGEA", 1, 255, 0, "R", nullptr},
    {"AVERAGEIF", 2, 3, 0, "RVR", nullptr},
    {"AVERAGEIFS", 3, 255, 0, "R+RV", nullptr},
    {"CEILING", 2, 2, 0, "V", nullptr},
    {"CEILING.MATH", 1, 3, X, "V", nullptr},
    {"CHAR", 1, 1, 0, "V", nullptr},
    {"CHOOSE", 2, 255, L, "VR", nullptr},
    {"CLEAN", 1, 1, 0, "V", nullptr},
    {"CODE", 1, 1, 0, "V", nullptr},
    {"COLUMN", 0, 1, 0, "R", nullptr},
    {"COLUMNS", 1, 1, 0, "R", nullptr},
    {"CONCAT", 1, 255, X, "R", nullptr},
    {"CONCATENATE", 1, 255, 0, "V", nullptr},
    {"COUNT", 1, 255, 0, "R", nullptr},
    {"COUNTA", 1, 255, 0, "R", nullptr},
    {"COUNTBLANK", 1, 1, 0, "R", nullptr},
    {"COUNTIF", 2, 2, 0, "RV", nullptr},
    {"COUNTIFS", 2, 254, 0, "+RV", nullptr},
    {"DATE", 3, 3, 0, "V", nullptr},
    {"DATEDIF", 3, 3, 0, "V", nullptr},
    {"DATEVALUE", 1, 1, 0, "V", nullptr},
    {"DAY", 1, 1, 0, "V", nullptr},
    {"DAYS", 2, 2, X, "V", nullptr},
    {"DOLLAR", 1, 2, 0, "V", nullptr},
    {"EDATE", 2, 2, 0, "V", nullptr},
    {"EOMONTH", 2, 2, 0, "V", nullptr},
    {"ERROR.TYPE", 1, 1, 0, "V", nullptr},
    {"EVEN", 1, 1, 0, "V", nullptr},
    {"EXACT", 2, 2, 0, "V", nullptr},
    {"EXP", 1, 1, 0, "V", nullptr},
    {"FALSE", 0, 0, 0, "V", nullptr},
    {"FILTER", 2, 3, W, "AAV", nullptr},
    {"FIND", 2, 3, 0, "V", nullptr},
    {"FIXED", 1, 3, 0, "V", nullptr},
    {"FLOOR", 2, 2, 0, "V", nullptr},
    {"FLOOR.MATH", 1, 3, X, "V", nullptr},
    {"FV", 3, 5, 0, "V", nullptr},
    {"HLOOKUP", 3, 4, 0, "VRVV", nullptr},
    {"HOUR", 1, 1, 0, "V", nullptr},
    {"HYPERLINK", 1, 2, 0, "V", nullptr},
    {"IF", 2, 3, L, "VRR", nullptr},
    {"IFERROR", 2, 2, L, "VV", nullptr},
    {"IFNA", 2, 2, L | X, "VV", nullptr},
    {"IFS", 2, 254, L | X, "+VR", nullptr},
    {"INDEX", 2, 4, 0, "RVVV", nullptr},
    {"INDIRECT", 1, 2, V | D, "V", nullptr},
    {"INT", 1, 1, 0, "V", nullptr},
    {"ISBLANK", 1, 1, 0, "V", nullptr},
    {"ISERR", 1, 1, 0, "V", nullptr},
    {"ISERROR", 1, 1, 0, "V", nullptr},
    {"ISEVEN", 1, 1, 0, "V", nullptr},
    {"ISFORMULA", 1, 1, X, "R", nullptr},
    {"ISLOGICAL", 1, 1, 0, "V", nullptr},
    {"ISNA", 1, 1, 0, "V", nullptr},
    {"ISNONTEXT", 1, 1, 0, "V", nullptr},
    {"ISNUMBER", 1, 1, 0, "V", nullptr},
    {"ISODD", 1, 1, 0, "V", nullptr},
    {"ISOWEEKNUM", 1, 1, X, "V", nullptr},
    {"ISREF", 1, 1, 0, "R", nullptr},
    {"ISTEXT", 1, 1, 0, "V", nullptr},
    {"LARGE", 2, 2, 0, "RV", nullptr},
    {"LEFT", 1, 2, 0, "V", nullptr},
    {"LEN", 1, 1, 0, "V", nullptr},
    {"LET", 3, 255, X, "V", nullptr},
    {"LN", 1, 1, 0, "V", nullptr},
    {"LOG", 1, 2, 0, "V", nullptr},
    {"LOG10", 1, 1, 0, "V", nullptr},
    {"LOOKUP", 2, 3, 0, "VRR", nullptr},
    {"LOWER", 1, 1, 0, "V", nullptr},
    {"MATCH", 2, 3, 0, "VRV", nullptr},
    {"MAX", 1, 255, 0, "R", nullptr},
    {"MAXA", 1, 255, 0, "R", nullptr},
    {"MAXIFS", 3, 255, X, "R+RV", nullptr},
    {"MEDIAN", 1, 255, 0, "R", nullptr},
    {"MID", 3, 3, 0, "V", nullptr},
    {"MIN", 1, 255, 0, "R", nullptr},
    {"MINA", 1, 255, 0, "R", nullptr},
    {"MINIFS", 3, 255, X, "R+RV", nullptr},
    {"MINUTE", 1, 1, 0, "V", nullptr},
    {"MOD", 2, 2, 0, "V", nullptr},
    {"MODE", 1, 255, 0, "R", nullptr},
    {"MODE.SNGL", 1, 255, X, "R", nullptr},
    {"MONTH", 1, 1, 0, "V", nullptr},
    {"MROUND", 2, 2, 0, "V", nullptr},
    {"N", 1, 1, 0, "V", nullptr},
    {"NA", 0, 0, 0, "V", nullptr},
    {"NETWORKDAYS", 2, 3, 0, "VVR", nullptr},
    {"NETWORKDAYS.INTL", 2, 4, X, "VVVR", nullptr},
    {"NOT", 1, 1, 0, "V", nullptr},
    {"NOW", 0, 0, V, "V", nullptr},
    {"NPER", 3, 5, 0, "V", nullptr},
    {"NPV", 2, 255, 0, "VR", nullptr},
    {"NUMBERVALUE", 1, 3, X, "V", nullptr},
    {"ODD", 1, 1, 0, "V", nullptr},
    {"OFFSET", 3, 5, V | D, "RV", nullptr},
    {"OR", 1, 255, 0, "R", nullptr},
    {"PERCENTILE", 2, 2, 0, "RV", nullptr},
    {"PERCENTILE.INC", 2, 2, X, "RV", nullptr},
    {"PI", 0, 0, 0, "V", nullptr},
    {"PMT", 3, 5, 0, "V", nullptr},
    {"POWER", 2, 2, 0, "V", nullptr},
    {"PRODUCT", 1, 255, 0, "R", nullptr},
    {"PROPER", 1, 1, 0, "V", nullptr},
    {"PV", 3, 5, 0, "V", nullptr},
    {"QUARTILE", 2, 2, 0, "RV", nullptr},
    {"QUARTILE.INC", 2, 2, X, "RV", nullptr},
    {"QUOTIENT", 2, 2, 0, "V", nullptr},
    {"RAND", 0, 0, V, "V", nullptr},
    {"RANDBETWEEN", 2, 2, V, "V", nullptr},
    {"RANK", 2, 3, 0, "VRV", nullptr},
    {"RANK.AVG", 2, 3, X, "VRV", nullptr},
    {"RANK.EQ", 2, 3, X, "VRV", nullptr},
    {"RATE", 3, 6, 0, "V", nullptr},
    {"REPLACE", 4, 4, 0, "V", nullptr},
    {"REPT", 2, 2, 0, "V", nullptr},
    {"RIGHT", 1, 2, 0, "V", nullptr},
    {"ROUND", 2, 2, 0, "V", nullptr},
    {"ROUNDDOWN", 2, 2, 0, "V", nullptr},
    {"ROUNDUP", 2, 2, 0, "V", nullptr},
    {"ROW", 0, 1, 0, "R", nullptr},
    {"ROWS", 1, 1, 0, "R", nullptr},
    {"SEARCH", 2, 3, 0, "V", nullptr},
    {"SECOND", 1, 1, 0, "V", nullptr},
    {"SEQUENCE", 1, 4, X, "V", nullptr},
    {"SIGN", 1, 1, 0, "V", nullptr},
    {"SMALL", 2, 2, 0, "RV", nullptr},
    {"SORT", 1, 4, W, "AV", nullptr},
    {"SQRT", 1, 1, 0, "V", nullptr},
    {"STDEV", 1, 255, 0, "R", nullptr},
    {"STDEV.P", 1, 255, X, "R", nullptr},
    {"STDEV.S", 1, 255, X, "R", nullptr},
    {"STDEVP", 1, 255, 0, "R", nullptr},
    {"SUBSTITUTE", 3, 4, 0, "V", nullptr},
    {"SUBTOTAL", 2, 255, 0, "VR", nullptr},
    {"SUM", 1, 255, 0, "R", nullptr},
    {"SUMIF", 2, 3, 0, "RVR", nullptr},
    {"SUMIFS", 3, 255, 0, "R+RV", nullptr},
    {"SUMPRODUCT", 1, 255, 0, "A", nullptr},
    {"SUMSQ", 1, 255, 0, "R", nullptr},
    {"SWITCH", 3, 254, L | X, "V+VR", nullptr},
    {"T", 1, 1, 0, "V", nullptr},
    {"TEXT", 2, 2, 0, "V", nullptr},
    {"TEXTJOIN", 3, 255, X, "VVR", nullptr},
    {"TIME", 3, 3, 0, "V", nullptr},
    {"TIMEVALUE", 1, 1, 0, "V", nullptr},
    {"TODAY", 0, 0, V, "V", nullptr},
    {"TRANSPOSE", 1, 1, 0, "A", nullptr},
    {"TRIM", 1, 1, 0, "V", nullptr},
    {"TRUE", 0, 0, 0, "V", nullptr},
    {"TRUNC", 1, 2, 0, "V", nullptr},
    {"TYPE", 1, 1, 0, "R", nullptr},
    {"UNIQUE", 1, 3, X, "AV", nullptr},
    {"UPPER", 1, 1, 0, "V", nullptr},
    {"VALUE", 1, 1, 0, "V", nullptr},
    {"VAR", 1, 255, 0, "R", nullptr},
    {"VAR.P", 1, 255, X, "R", nullptr},
    {"VAR.S", 1, 255, X, "R", nullptr},
    {"VARP", 1, 255, 0, "R", nullptr},
    {"VLOOKUP", 3, 4, 0, "VRVV", nullptr},
    {"WEEKDAY", 1, 2, 0, "V", nullptr},
    {"WEEKNUM", 1, 2, 0, "V", nullptr},
    {"WORKDAY", 2, 3, 0, "VVR", nullptr},
    {"WORKDAY.INTL", 2, 4, X, "VVVR", nullptr},
    {"XLOOKUP", 3, 6, X, "VRRVVV", nullptr},
    {"XMATCH", 2, 4, X, "VRVV", nullptr},
    {"XOR", 1, 255, X, "R", nullptr},
    {"YEAR", 1, 1, 0, "V", nullptr},
    {"YEARFRAC", 2, 3, 0, "V", nullptr},
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
