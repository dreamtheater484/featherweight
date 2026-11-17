#pragma once

// Internal to the formula engine: the machine that runs bytecode, the helpers functions use to
// read their arguments, and the function implementations (Fn*.cpp).

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include "sheet/calc/Bytecode.h"
#include "sheet/calc/Eval.h"
#include "sheet/calc/Functions.h"

namespace fw::sheet::calc {

// One evaluation in progress.
struct Machine {
  Evaluator& ev;
  Host& host;
  Resolver* res;
  Cell at;
  bool date1904 = false;
  char decimal = '.';        // the region's decimal separator, for numbers turned into text
  bool stop = false;         // a run-time reference isn't calculated yet: retry later
  bool unsupported = false;  // something the engine can't do: keep the cached value
  int depth = 0;
  std::vector<Value>& stack;
  // LET's names in scope (name slot, value), innermost last; a defined name's formula sees only
  // those from `localsBase` on (none).
  std::vector<std::pair<uint32_t, Value>> locals;
  size_t localsBase = 0;

  Machine(Evaluator& e, const Cell& c)
      : ev(e), host(e.host()), res(e.resolver()), at(c), date1904(e.host().Date1904()),
        decimal(e.host().Conv().decimal), stack(e.stack_) {}

  // Runs code from p to end, which leaves one more value on the stack. `top`: this is the
  // formula's own code (its last + or - zeroes tiny results, as Excel's does).
  void Run(const uint8_t* p, const uint8_t* end, bool top);
  Value RunOne(const uint8_t* p, const uint8_t* end);  // runs and pops the value
  void Unary(uint8_t code);
  void Binary(uint8_t code, bool final);
  void CallFn(int id, int argc, bool arrayMode);
  const uint8_t* Lazy(const uint8_t* p);  // runs a lazy call; returns the code after it

  // Cells
  Value CellValue(int sheet, int32_t row, int32_t col);
  bool Ready(const Value& ref);  // a run-time reference: its cells calculated? (else sets stop)
  // A reference as one value outside array formulas: the cell it shares the formula's row or
  // column with (implicit intersection), else #VALUE!.
  Value Intersect(const Value& ref);
  // A reference as an array (whole columns and rows cut to the workbook's used extent).
  Value RefArray(const Value& ref);
  // A value for an operator or a 'V' argument: references intersected, or made arrays in array
  // formulas; single cells read.
  Value Deref(const Value& v, bool arrayMode);
  void WorkbookExtent(int32_t& rows, int32_t& cols);
  int32_t extentRows_ = -1, extentCols_ = -1;
};

// The view of a scalar Value as a cell's (text borrowed from the Value).
CellView View(const Value& v);
Value FromView(const CellView& c);

// Excel's conversions of one scalar. On failure, `e` is the error.
bool ToNumber(Machine& m, const Value& v, double& d, Err& e);
bool ToNumber(Machine& m, const CellView& v, double& d, Err& e);
bool ToText(const Value& v, std::string& s, Err& e, char decimal = '.');
bool ToBool(const Value& v, bool& b, Err& e);

// Ordering of two values as comparison operators see them: numbers < text < booleans; text
// case-insensitive; numbers to 15 digits. Empty is 0, "" or FALSE as the other side needs.
int CompareValues(const CellView& a, const CellView& b);
bool EqualNumbers(double a, double b);  // equal to 15 significant digits

struct Call {
  Machine& m;
  Value* args;
  int argc;
  bool arrayMode;
  Value out;

  bool Has(int i) const { return i >= 0 && i < argc && !args[i].IsMissing(); }
  const Value& arg(int i) const { return args[i]; }
  void Fail(Err e) { out = Value::Error(e); }
  // Scalar arguments ('V'). On failure they set `out` to the error and return false. A missing
  // argument gives `def`.
  bool Number(int i, double& v, double def = 0);
  bool Integer(int i, double& v, double def = 0);  // truncated toward zero
  bool Text(int i, std::string& s);
  bool Boolean(int i, bool& b, bool def = false);
  void SetNumber(double v);  // #NUM! for infinities and NaN
};

// A reference (one area on one sheet), an array or a scalar seen as rows x cols. `rows`/`cols` are
// its size; `dataRows`/`dataCols` stop where the sheet's cells do (the rest is empty).
struct Grid {
  Machine* m = nullptr;
  const Value* v = nullptr;
  bool ref = false;
  Area area;
  int32_t rows = 0, cols = 0, dataRows = 0, dataCols = 0;
  CellView At(int32_t r, int32_t c) const;
};
// False for a union of areas or several sheets (#VALUE!), or when the reference isn't ready.
bool MakeGrid(Machine& m, const Value& v, Grid& g);

enum class From { kDirect, kRef, kArray };
// Calls fn(const CellView&, From) for each value of an 'R' or 'A' argument: a value given
// directly, each non-empty cell of a reference, each element of an array. Stops when fn returns
// false; returns false then, or when a reference wasn't ready.
template <class F>
bool EachItem(Machine& m, const Value& v, F&& fn);

// COUNTIF-style criteria: "=x", "<>x", "<5", ">=1/1/2024", "a*", 5, TRUE.
struct Criteria {
  enum Op { kEq, kNe, kLt, kLe, kGt, kGe } op = kEq;
  Type type = Type::Empty;  // what is compared: Number, Text, Bool, Error, or Empty for blanks
  double num = 0;
  std::string text;
  Err err = Err::None;
  bool wildcard = false;
  const ConvLocale* conv = &EnUsConv();
  bool Match(const CellView& c) const;
};
bool MakeCriteria(Machine& m, const Value& v, Criteria& c);

// A text's characters (UTF-16 code units, as Excel counts them).
std::wstring Wide(std::string_view s);
std::string Narrow(std::wstring_view s);

// --- template implementation ---

namespace detail {
template <class F>
struct ItemThunk {
  F* fn;
  bool stopped = false;
  static bool Call(void* ctx, int32_t, int32_t, const CellView& c) {
    auto* t = static_cast<ItemThunk*>(ctx);
    if (!(*t->fn)(c, From::kRef)) {
      t->stopped = true;
      return false;
    }
    return true;
  }
};
}  // namespace detail

template <class F>
bool EachItem(Machine& m, const Value& v, F&& fn) {
  switch (v.type()) {
    case Type::Ref:
    case Type::RefList: {
      if (!m.Ready(v)) return false;
      detail::ItemThunk<std::remove_reference_t<F>> t{&fn};
      auto area = [&](const Area& a) {
        for (int s = a.s1; s <= a.s2 && !t.stopped; ++s)
          m.host.ForEachCell(s, a.r1, a.c1, a.r2, a.c2, &decltype(t)::Call, &t);
      };
      if (v.type() == Type::Ref) {
        area(v.area());
      } else {
        for (const Area& a : v.areas()) {
          if (t.stopped) break;
          area(a);
        }
      }
      return !t.stopped;
    }
    case Type::Array: {
      const Array& a = v.array();
      for (int32_t i = 0, n = a.size(); i < n; ++i)
        if (!fn(View(a.data()[i]), From::kArray)) return false;
      return true;
    }
    case Type::Missing:
      return true;
    default:
      return fn(View(v), From::kDirect);
  }
}

// --- the functions ---

namespace fn {
// An argument as an array: a reference's values (whole columns as far as the workbook's cells
// reach), a single value as 1 x 1. False (with c.out set) when it can't be read.
bool ArgArray(Call& c, int i, Value& out);

// SUM's rules for its arguments [from, to): numbers in references and arrays (text, booleans and
// blanks there are skipped); values given directly count, booleans as 1 and 0, text converted.
// The first error ends it, in c.out.
template <class F>
bool EachNumber(Call& c, int from, int to, F&& fn) {
  for (int i = from; i < to; ++i) {
    if (c.arg(i).IsMissing()) continue;
    bool ok = EachItem(c.m, c.arg(i), [&](const CellView& v, From from) {
      if (v.type == Type::Error) {
        c.Fail(v.err);
        return false;
      }
      if (v.type == Type::Number) {
        fn(v.num);
        return true;
      }
      if (from != From::kDirect) return true;
      double d;
      Err e = Err::None;
      if (!ToNumber(c.m, v, d, e)) {
        c.Fail(e);
        return false;
      }
      fn(d);
      return true;
    });
    if (!ok) {
      if (c.m.stop || !c.out.IsError()) c.Fail(Err::Value);
      return false;
    }
  }
  return true;
}

// The "A" functions' rules: in references, text counts as 0 and booleans as 1 or 0.
template <class F>
bool EachNumberA(Call& c, int from, int to, F&& fn) {
  for (int i = from; i < to; ++i) {
    if (c.arg(i).IsMissing()) continue;
    bool ok = EachItem(c.m, c.arg(i), [&](const CellView& v, From from) {
      if (v.type == Type::Error) {
        c.Fail(v.err);
        return false;
      }
      if (v.type == Type::Number || v.type == Type::Bool) {
        fn(v.num);
        return true;
      }
      if (v.type == Type::Text) {
        double d;
        Err e = Err::None;
        if (from != From::kDirect) {
          fn(0);
          return true;
        }
        if (!ToNumber(c.m, v, d, e)) {
          c.Fail(e);
          return false;
        }
        fn(d);
      }
      return true;
    });
    if (!ok) {
      if (!c.out.IsError()) c.Fail(Err::Value);
      return false;
    }
  }
  return true;
}

inline bool Numbers(Call& c, int from, int to, std::vector<double>& out) {
  return EachNumber(c, from, to, [&](double d) { out.push_back(d); });
}

// Adds as Excel's SUM does: a total that cancels to a rounding error becomes 0.
struct Summer {
  double sum = 0;
  void Add(double d) {
    double r = sum + d;
    if (r != 0 && std::fabs(r) <= std::max(std::fabs(sum), std::fabs(d)) * 8.8817841970012523e-16) r = 0;
    sum = r;
  }
};

inline double Mean(const std::vector<double>& v) {
  Summer s;
  for (double d : v) s.Add(d);
  return s.sum / static_cast<double>(v.size());
}

inline double SquaredDeviations(const std::vector<double>& v) {
  double m = Mean(v), s = 0;
  for (double d : v) s += (d - m) * (d - m);
  return s;
}

// YEARFRAC's year fraction between two serials (any order) by day-count basis 0-4.
double YearFracOf(double a, double b, int basis, bool date1904);
// A serial argument: numbers, or text read as a date; negative or past 9999 is #NUM!.
bool SerialArg(Call& c, int i, double& s);

#define FW_FN(name) void name(Call& c);
FW_FN(Abs) FW_FN(Address) FW_FN(Aggregate) FW_FN(And) FW_FN(Average) FW_FN(AverageA) FW_FN(AverageIf) FW_FN(AverageIfs)
FW_FN(Ceiling) FW_FN(CeilingMath) FW_FN(Char) FW_FN(Clean) FW_FN(Code) FW_FN(Column) FW_FN(Columns)
FW_FN(Concat) FW_FN(Concatenate) FW_FN(Count) FW_FN(CountA) FW_FN(CountBlank) FW_FN(CountIf) FW_FN(CountIfs)
FW_FN(Date) FW_FN(DateDif) FW_FN(DateValue) FW_FN(Day) FW_FN(Days) FW_FN(Dollar) FW_FN(EDate) FW_FN(EOMonth)
FW_FN(ErrorType) FW_FN(Even) FW_FN(Exact) FW_FN(Exp) FW_FN(False) FW_FN(Filter) FW_FN(Find) FW_FN(Fixed) FW_FN(Floor)
FW_FN(FloorMath) FW_FN(Fv) FW_FN(HLookup) FW_FN(Hour) FW_FN(Hyperlink) FW_FN(Index) FW_FN(Indirect) FW_FN(Info) FW_FN(Int)
FW_FN(IsBlank) FW_FN(IsErr) FW_FN(IsError) FW_FN(IsEven) FW_FN(IsLogical) FW_FN(IsNA) FW_FN(IsNonText)
FW_FN(IsNumber) FW_FN(IsOdd) FW_FN(IsoWeekNum) FW_FN(IsRef) FW_FN(IsText) FW_FN(Large) FW_FN(Left) FW_FN(Len)
FW_FN(Ln) FW_FN(Log) FW_FN(Log10) FW_FN(Lookup) FW_FN(Lower) FW_FN(Match) FW_FN(Max) FW_FN(MaxA) FW_FN(MaxIfs)
FW_FN(Median) FW_FN(Mid) FW_FN(Min) FW_FN(MinA) FW_FN(MinIfs) FW_FN(Minute) FW_FN(Mod) FW_FN(Mode) FW_FN(Month)
FW_FN(MRound) FW_FN(N) FW_FN(NA) FW_FN(NetworkDays) FW_FN(NetworkDaysIntl) FW_FN(Not) FW_FN(Now) FW_FN(NPer)
FW_FN(Npv) FW_FN(NumberValue) FW_FN(Odd) FW_FN(Offset) FW_FN(Or) FW_FN(Percentile) FW_FN(PercentileExc) FW_FN(Pi) FW_FN(Pmt)
FW_FN(Power) FW_FN(Product) FW_FN(Proper) FW_FN(Pv) FW_FN(Quartile) FW_FN(QuartileExc) FW_FN(Quotient) FW_FN(Rand)
FW_FN(RandBetween) FW_FN(Rank) FW_FN(RankAvg) FW_FN(Rate) FW_FN(Replace) FW_FN(Rept) FW_FN(Right) FW_FN(Round)
FW_FN(RoundDown) FW_FN(RoundUp) FW_FN(Row) FW_FN(Rows) FW_FN(Search) FW_FN(Second) FW_FN(Sequence) FW_FN(Sign) FW_FN(Small) FW_FN(Sort) FW_FN(SortBy)
FW_FN(LongText) FW_FN(Sqrt) FW_FN(StDev) FW_FN(StDevP) FW_FN(Substitute) FW_FN(Subtotal) FW_FN(Sum) FW_FN(SumIf) FW_FN(SumIfs)
FW_FN(SumProduct) FW_FN(SumSq) FW_FN(T) FW_FN(Text) FW_FN(TextJoin) FW_FN(Time) FW_FN(TimeValue) FW_FN(Today)
FW_FN(Transpose) FW_FN(Trim) FW_FN(True) FW_FN(Trunc) FW_FN(TypeFn) FW_FN(Unique) FW_FN(Upper) FW_FN(ValueFn) FW_FN(Var)
FW_FN(VarP) FW_FN(VLookup) FW_FN(Weekday) FW_FN(WeekNum) FW_FN(Workday) FW_FN(WorkdayIntl) FW_FN(XLookup)
FW_FN(XMatch) FW_FN(Xor) FW_FN(Year) FW_FN(YearFrac)
FW_FN(VStack) FW_FN(HStack) FW_FN(Take) FW_FN(Drop) FW_FN(ChooseRows) FW_FN(ChooseCols) FW_FN(ToCol) FW_FN(ToRow)
FW_FN(WrapRows) FW_FN(WrapCols) FW_FN(Expand) FW_FN(RandArray) FW_FN(TextSplit) FW_FN(TextBefore) FW_FN(TextAfter)
FW_FN(ValueToText) FW_FN(ArrayToText) FW_FN(UniChar) FW_FN(Unicode) FW_FN(IsFormula) FW_FN(FormulaText)
FW_FN(Sheet) FW_FN(Sheets) FW_FN(Areas) FW_FN(CellInfo)
FW_FN(CumIpmt) FW_FN(CumPrinc) FW_FN(Days360) FW_FN(Db) FW_FN(Ddb) FW_FN(Disc) FW_FN(DollarDe)
FW_FN(DollarFr) FW_FN(Effect) FW_FN(FvSchedule) FW_FN(IntRate) FW_FN(Ipmt) FW_FN(Irr) FW_FN(IsPmt)
FW_FN(Mirr) FW_FN(Nominal) FW_FN(PDuration) FW_FN(Ppmt) FW_FN(PriceDisc) FW_FN(Received) FW_FN(Rri)
FW_FN(Sln) FW_FN(Syd) FW_FN(TBillEq) FW_FN(TBillPrice) FW_FN(TBillYield) FW_FN(Vdb) FW_FN(Xirr) FW_FN(Xnpv)
FW_FN(YieldDisc)
FW_FN(Acos) FW_FN(Acosh) FW_FN(Acot) FW_FN(Acoth) FW_FN(Arabic) FW_FN(Asin) FW_FN(Asinh) FW_FN(Atan)
FW_FN(Atan2) FW_FN(Atanh) FW_FN(Base) FW_FN(CombinA) FW_FN(CombinFn) FW_FN(Cos) FW_FN(Cosh) FW_FN(Cot)
FW_FN(Coth) FW_FN(Csc) FW_FN(Csch) FW_FN(Decimal) FW_FN(Degrees) FW_FN(Fact) FW_FN(FactDouble) FW_FN(GcdFn)
FW_FN(Lcm) FW_FN(MDeterm) FW_FN(MInverse) FW_FN(MMult) FW_FN(MUnit) FW_FN(Multinomial) FW_FN(Permut)
FW_FN(PermutationA) FW_FN(Radians) FW_FN(Roman) FW_FN(Sec) FW_FN(Sech) FW_FN(SeriesSum) FW_FN(Sin)
FW_FN(Sinh) FW_FN(SqrtPi) FW_FN(SumX2mY2) FW_FN(SumX2pY2) FW_FN(SumXmY2) FW_FN(Tan) FW_FN(Tanh)
FW_FN(AveDev) FW_FN(Correl) FW_FN(CovarP) FW_FN(CovarS) FW_FN(DevSq) FW_FN(Fisher) FW_FN(FisherInv)
FW_FN(Forecast) FW_FN(Frequency) FW_FN(GeoMean) FW_FN(Growth) FW_FN(HarMean) FW_FN(Intercept) FW_FN(Kurt)
FW_FN(Linest) FW_FN(Logest) FW_FN(ModeMult) FW_FN(PercentRankExc) FW_FN(PercentRankInc) FW_FN(Prob)
FW_FN(Rsq) FW_FN(Skew) FW_FN(SkewP) FW_FN(Slope) FW_FN(StDevA) FW_FN(StDevPA) FW_FN(Standardize) FW_FN(SteYX)
FW_FN(Trend) FW_FN(TrimMean) FW_FN(VarA) FW_FN(VarPA)
#undef FW_FN
}  // namespace fn

}  // namespace fw::sheet::calc
