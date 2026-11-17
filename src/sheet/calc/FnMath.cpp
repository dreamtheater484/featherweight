// Math, statistics, logic, information and financial functions.

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "sheet/calc/Fn.h"

namespace fw::sheet::calc::fn {
namespace {

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

bool Numbers(Call& c, int from, int to, std::vector<double>& out) {
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

double Mean(const std::vector<double>& v) {
  Summer s;
  for (double d : v) s.Add(d);
  return s.sum / static_cast<double>(v.size());
}

double SquaredDeviations(const std::vector<double>& v) {
  double m = Mean(v), s = 0;
  for (double d : v) s += (d - m) * (d - m);
  return s;
}

// Rounds |x| * 10^digits at its 15th significant digit first, so 2.675 rounds to 2.68 as in Excel.
double Scaled(double x, double digits, double& scale) {
  scale = std::pow(10.0, digits);
  return Round15(x * scale);
}

double RoundTo(double x, double digits, int mode) {  // 0 half away from zero, 1 up, 2 down
  digits = std::trunc(digits);
  if (digits > 300) return x;
  double scale;
  double y = Scaled(std::fabs(x), digits, scale);
  double r = mode == 0 ? std::floor(y + 0.5) : mode == 1 ? std::ceil(y) : std::floor(y);
  r /= scale;
  return x < 0 ? -r : r;
}

}  // namespace

// --- aggregates ---

void Sum(Call& c) {
  Summer s;
  if (EachNumber(c, 0, c.argc, [&](double d) { s.Add(d); })) c.SetNumber(s.sum);
}

void SumSq(Call& c) {
  double s = 0;
  if (EachNumber(c, 0, c.argc, [&](double d) { s += d * d; })) c.SetNumber(s);
}

void Product(Call& c) {
  double p = 1;
  bool any = false;
  if (EachNumber(c, 0, c.argc, [&](double d) { p *= d, any = true; })) c.SetNumber(any ? p : 0);
}

void Average(Call& c) {
  Summer s;
  double n = 0;
  if (!EachNumber(c, 0, c.argc, [&](double d) { s.Add(d), ++n; })) return;
  if (n == 0) return c.Fail(Err::Div0);
  c.SetNumber(s.sum / n);
}

void AverageA(Call& c) {
  Summer s;
  double n = 0;
  if (!EachNumberA(c, 0, c.argc, [&](double d) { s.Add(d), ++n; })) return;
  if (n == 0) return c.Fail(Err::Div0);
  c.SetNumber(s.sum / n);
}

void Count(Call& c) {
  double n = 0;
  for (int i = 0; i < c.argc; ++i) {
    if (c.arg(i).IsMissing()) continue;
    EachItem(c.m, c.arg(i), [&](const CellView& v, From from) {
      if (v.type == Type::Number) ++n;
      else if (from == From::kDirect && v.type == Type::Bool) ++n;
      else if (from == From::kDirect && v.type == Type::Text) {
        double d;
        Err e;
        if (ToNumber(c.m, v, d, e)) ++n;
      }
      return true;
    });
  }
  c.SetNumber(n);
}

void CountA(Call& c) {
  double n = 0;
  for (int i = 0; i < c.argc; ++i) {
    if (c.arg(i).IsMissing()) continue;
    EachItem(c.m, c.arg(i), [&](const CellView& v, From) {
      if (v.type != Type::Empty) ++n;
      return true;
    });
  }
  c.SetNumber(n);
}

void CountBlank(Call& c) {
  Grid g;
  if (!MakeGrid(c.m, c.arg(0), g)) return c.Fail(Err::Value);
  double filled = 0;
  for (int32_t r = 0; r < g.dataRows; ++r)
    for (int32_t k = 0; k < g.dataCols; ++k) {
      CellView v = g.At(r, k);
      if (!(v.type == Type::Empty || (v.type == Type::Text && v.text.empty()))) ++filled;
    }
  c.SetNumber(static_cast<double>(g.rows) * g.cols - filled);
}

static void MinMax(Call& c, bool max, bool a) {
  double best = 0;
  bool any = false;
  auto take = [&](double d) {
    if (!any || (max ? d > best : d < best)) best = d;
    any = true;
  };
  bool ok = a ? EachNumberA(c, 0, c.argc, take) : EachNumber(c, 0, c.argc, take);
  if (ok) c.SetNumber(any ? best : 0);
}
void Max(Call& c) { MinMax(c, true, false); }
void Min(Call& c) { MinMax(c, false, false); }
void MaxA(Call& c) { MinMax(c, true, true); }
void MinA(Call& c) { MinMax(c, false, true); }

void Median(Call& c) {
  std::vector<double> v;
  if (!Numbers(c, 0, c.argc, v)) return;
  if (v.empty()) return c.Fail(Err::Num);
  std::sort(v.begin(), v.end());
  size_t n = v.size();
  c.SetNumber(n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2);
}

void Mode(Call& c) {
  std::vector<double> v;
  if (!Numbers(c, 0, c.argc, v)) return;
  double best = 0;
  size_t bestCount = 1;
  for (size_t i = 0; i < v.size(); ++i) {
    size_t n = 0;
    for (size_t j = 0; j < v.size(); ++j) n += v[j] == v[i];
    if (n > bestCount) best = v[i], bestCount = n;
  }
  if (bestCount < 2) return c.Fail(Err::NA);
  c.SetNumber(best);
}

static void Variance(Call& c, bool sample, bool root) {
  std::vector<double> v;
  if (!Numbers(c, 0, c.argc, v)) return;
  size_t need = sample ? 2 : 1;
  if (v.size() < need) return c.Fail(Err::Div0);
  double r = SquaredDeviations(v) / static_cast<double>(v.size() - (sample ? 1 : 0));
  c.SetNumber(root ? std::sqrt(r) : r);
}
void StDev(Call& c) { Variance(c, true, true); }
void StDevP(Call& c) { Variance(c, false, true); }
void Var(Call& c) { Variance(c, true, false); }
void VarP(Call& c) { Variance(c, false, false); }

static void Kth(Call& c, bool large) {
  std::vector<double> v;
  if (!Numbers(c, 0, 1, v)) return;
  double k;
  if (!c.Number(1, k)) return;
  k = std::ceil(k);
  if (v.empty() || k < 1 || k > static_cast<double>(v.size())) return c.Fail(Err::Num);
  std::sort(v.begin(), v.end());
  size_t i = static_cast<size_t>(k) - 1;
  c.SetNumber(large ? v[v.size() - 1 - i] : v[i]);
}
void Large(Call& c) { Kth(c, true); }
void Small(Call& c) { Kth(c, false); }

static void PercentileOf(Call& c, double k) {
  std::vector<double> v;
  if (!Numbers(c, 0, 1, v)) return;
  if (v.empty() || k < 0 || k > 1) return c.Fail(Err::Num);
  std::sort(v.begin(), v.end());
  double pos = k * static_cast<double>(v.size() - 1);
  size_t lo = static_cast<size_t>(std::floor(pos));
  double frac = pos - static_cast<double>(lo);
  c.SetNumber(lo + 1 < v.size() ? v[lo] + frac * (v[lo + 1] - v[lo]) : v[lo]);
}
void Percentile(Call& c) {
  double k;
  if (c.Number(1, k)) PercentileOf(c, k);
}
void Quartile(Call& c) {
  double q;
  if (!c.Number(1, q)) return;
  q = std::trunc(q);
  if (q < 0 || q > 4) return c.Fail(Err::Num);
  PercentileOf(c, q / 4);
}

namespace {
// PERCENTILE.EXC of sorted numbers: rank k * (n + 1), which must fall within 1..n.
Err PercentileExcOf(const std::vector<double>& v, double k, double& out) {
  double pos = k * static_cast<double>(v.size() + 1);
  if (v.empty() || k <= 0 || k >= 1 || pos < 1 || pos > static_cast<double>(v.size())) return Err::Num;
  size_t lo = static_cast<size_t>(std::floor(pos));
  double frac = pos - static_cast<double>(lo);
  out = lo < v.size() ? v[lo - 1] + frac * (v[lo] - v[lo - 1]) : v[lo - 1];
  return Err::None;
}
}  // namespace

static void PercentileExcFn(Call& c, double k) {
  std::vector<double> v;
  if (!Numbers(c, 0, 1, v)) return;
  std::sort(v.begin(), v.end());
  double r;
  Err e = PercentileExcOf(v, k, r);
  if (e != Err::None) return c.Fail(e);
  c.SetNumber(r);
}
void PercentileExc(Call& c) {
  double k;
  if (c.Number(1, k)) PercentileExcFn(c, k);
}
void QuartileExc(Call& c) {
  double q;
  if (!c.Number(1, q)) return;
  q = std::trunc(q);
  if (q <= 0 || q >= 4) return c.Fail(Err::Num);
  PercentileExcFn(c, q / 4);
}

static void RankOf(Call& c, bool average) {
  double x, order;
  if (!c.Number(0, x) || !c.Number(2, order)) return;
  std::vector<double> v;
  if (!Numbers(c, 1, 2, v)) return;
  double before = 0, same = 0;
  for (double d : v) {
    if (EqualNumbers(d, x)) ++same;
    else if (order == 0 ? d > x : d < x) ++before;
  }
  if (same == 0) return c.Fail(Err::NA);
  c.SetNumber(average ? before + (same + 1) / 2 : before + 1);
}
void Rank(Call& c) { RankOf(c, false); }
void RankAvg(Call& c) { RankOf(c, true); }

void SumProduct(Call& c) {
  // Arguments are arrays or references of one size; text and blanks count as 0.
  std::vector<Grid> grids(static_cast<size_t>(c.argc));
  std::vector<Value> held(static_cast<size_t>(c.argc));
  for (int i = 0; i < c.argc; ++i) {
    held[i] = c.arg(i);
    if (held[i].IsMissing()) return c.Fail(Err::Value);
    if (held[i].IsError()) return c.Fail(held[i].error());
    if (!MakeGrid(c.m, held[i], grids[i])) return c.Fail(Err::Value);
    if (grids[i].rows != grids[0].rows || grids[i].cols != grids[0].cols) return c.Fail(Err::Value);
  }
  int32_t rows = grids[0].rows, cols = grids[0].cols, dataRows = rows, dataCols = cols;
  for (const Grid& g : grids) dataRows = std::min(dataRows, g.dataRows), dataCols = std::min(dataCols, g.dataCols);
  Summer s;
  for (int32_t r = 0; r < dataRows; ++r)
    for (int32_t k = 0; k < dataCols; ++k) {
      double p = 1;
      for (const Grid& g : grids) {
        CellView v = g.At(r, k);
        if (v.type == Type::Error) return c.Fail(v.err);
        p *= v.type == Type::Number ? v.num : 0;
      }
      s.Add(p);
    }
  c.SetNumber(s.sum);
}

void Subtotal(Call& c) {
  double f;
  if (!c.Integer(0, f)) return;
  bool skipHidden = f > 100;
  int kind = static_cast<int>(skipHidden ? f - 100 : f);
  if (kind < 1 || kind > 11) return c.Fail(Err::Value);
  std::vector<double> v;
  double counta = 0;
  for (int i = 1; i < c.argc; ++i) {
    const Value& a = c.arg(i);
    if (!a.IsRef()) return c.Fail(Err::Value);
    if (!c.m.Ready(a)) return c.Fail(Err::Value);
    std::vector<Area> areas = a.type() == Type::Ref ? std::vector<Area>{a.area()} : a.areas();
    for (const Area& ar : areas)
      for (int s = ar.s1; s <= ar.s2; ++s) {
        struct Ctx {
          Call* c;
          int sheet;
          bool skipHidden;
          std::vector<double>* v;
          double* counta;
          bool failed = false;
          static bool Cell(void* p, int32_t r, int32_t col, const CellView& cv) {
            auto* x = static_cast<Ctx*>(p);
            // 101-111 leave out hidden rows; every SUBTOTAL leaves out rows a filter hides.
            if (x->skipHidden ? x->c->m.host.RowHidden(x->sheet, r) : x->c->m.host.RowFiltered(x->sheet, r)) return true;
            if (x->c->m.res && x->c->m.res->Subtotal(x->sheet, r, col)) return true;
            if (cv.type == Type::Error) {
              x->c->Fail(cv.err);
              x->failed = true;
              return false;
            }
            ++*x->counta;
            if (cv.type == Type::Number) x->v->push_back(cv.num);
            return true;
          }
        } ctx{&c, s, skipHidden, &v, &counta};
        c.m.host.ForEachCell(s, ar.r1, ar.c1, ar.r2, ar.c2, &Ctx::Cell, &ctx);
        if (ctx.failed) return;
      }
  }
  Summer sum;
  for (double d : v) sum.Add(d);
  double n = static_cast<double>(v.size());
  switch (kind) {
    case 1: return n ? c.SetNumber(sum.sum / n) : c.Fail(Err::Div0);
    case 2: return c.SetNumber(n);
    case 3: return c.SetNumber(counta);
    case 4: return c.SetNumber(v.empty() ? 0 : *std::max_element(v.begin(), v.end()));
    case 5: return c.SetNumber(v.empty() ? 0 : *std::min_element(v.begin(), v.end()));
    case 6: {
      double p = 1;
      for (double d : v) p *= d;
      return c.SetNumber(v.empty() ? 0 : p);
    }
    case 7: case 8: case 10: case 11: {
      bool sample = kind == 7 || kind == 10;
      if (v.size() < (sample ? 2u : 1u)) return c.Fail(Err::Div0);
      double var = SquaredDeviations(v) / (n - (sample ? 1 : 0));
      return c.SetNumber(kind <= 8 ? std::sqrt(var) : var);
    }
    default: return c.SetNumber(sum.sum);
  }
}

namespace {
// AGGREGATE's calculation `kind` (1..19) of the numbers `v` (sorted for 12 and 14..19), `counta` values in
// all, `k` for 14..19.
Err AggregateOf(int kind, const std::vector<double>& v, double counta, double k, double& out) {
  const double n = static_cast<double>(v.size());
  switch (kind) {
    case 1:
      if (v.empty()) return Err::Div0;
      out = Mean(v);
      return Err::None;
    case 2: out = n; return Err::None;
    case 3: out = counta; return Err::None;
    case 4: out = v.empty() ? 0 : *std::max_element(v.begin(), v.end()); return Err::None;
    case 5: out = v.empty() ? 0 : *std::min_element(v.begin(), v.end()); return Err::None;
    case 6: {
      double p = 1;
      for (double d : v) p *= d;
      out = v.empty() ? 0 : p;
      return Err::None;
    }
    case 7: case 8: case 10: case 11: {
      bool sample = kind == 7 || kind == 10;
      if (v.size() < (sample ? 2u : 1u)) return Err::Div0;
      double var = SquaredDeviations(v) / (n - (sample ? 1 : 0));
      out = kind <= 8 ? std::sqrt(var) : var;
      return Err::None;
    }
    case 9: {
      Summer s;
      for (double d : v) s.Add(d);
      out = s.sum;
      return Err::None;
    }
    case 12:
      if (v.empty()) return Err::Num;
      out = v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2;
      return Err::None;
    case 13: {  // the most frequent, the first of them in the values' order
      std::unordered_map<double, size_t> count;
      for (double d : v) ++count[d];
      size_t best = 1;
      for (double d : v)
        if (count[d] > best) best = count[d], out = d;
      return best < 2 ? Err::NA : Err::None;
    }
    case 14: case 15:
      k = std::ceil(k);
      if (v.empty() || k < 1 || k > n) return Err::Num;
      out = kind == 14 ? v[v.size() - static_cast<size_t>(k)] : v[static_cast<size_t>(k) - 1];
      return Err::None;
    case 16: case 17: {
      if (kind == 17) {
        k = std::trunc(k);
        if (k < 0 || k > 4) return Err::Num;
        k /= 4;
      }
      if (v.empty() || k < 0 || k > 1) return Err::Num;
      double pos = k * (n - 1);
      size_t lo = static_cast<size_t>(std::floor(pos));
      out = lo + 1 < v.size() ? v[lo] + (pos - static_cast<double>(lo)) * (v[lo + 1] - v[lo]) : v[lo];
      return Err::None;
    }
    default:  // 18, 19
      if (kind == 19) {
        k = std::trunc(k);
        if (k <= 0 || k >= 4) return Err::Num;
        k /= 4;
      }
      return PercentileExcOf(v, k, out);
  }
}
}  // namespace

// AGGREGATE(function, options, ref1, ...) for 1..13, AGGREGATE(function, options, array, k) for
// 14..19. Options 0..3 leave out cells with SUBTOTAL or AGGREGATE; 1, 3, 5, 7 hidden rows; 2, 3,
// 6, 7 error values in cells and arrays (an error given directly is the result, whatever the
// option). The reference form takes only references (#VALUE! for arrays); COUNT and COUNTA count
// past errors. Checked against Excel 2021.
void Aggregate(Call& c) {
  double f, o;
  if (!c.Integer(0, f) || !c.Integer(1, o)) return;
  if (f < 1 || f > 19 || o < 0 || o > 7) return c.Fail(Err::Value);
  const int kind = static_cast<int>(f), opt = static_cast<int>(o);
  const bool arrayForm = kind >= 14;
  if (arrayForm ? c.argc != 4 : c.argc < 3) return c.Fail(Err::Value);
  struct Ctx {
    Call* c;
    int sheet, kind;
    bool skipHidden, skipNested, skipErrors;
    std::vector<double>* v;
    double* counta;
    bool failed = false;
    // A value of a cell or array element; false (with c->out set) for an error that ends it.
    bool Take(const CellView& cv) {
      if (cv.type == Type::Error) {
        if (kind == 2 || skipErrors) return true;
        if (kind == 3) return ++*counta, true;
        c->Fail(cv.err);
        failed = true;
        return false;
      }
      if (cv.type == Type::Empty) return true;
      ++*counta;
      if (cv.type == Type::Number) v->push_back(cv.num);
      return true;
    }
    static bool Cell(void* p, int32_t r, int32_t col, const CellView& cv) {
      auto* x = static_cast<Ctx*>(p);
      if (x->skipHidden && x->c->m.host.RowHidden(x->sheet, r)) return true;
      if (x->skipNested && x->c->m.res && x->c->m.res->Subtotal(x->sheet, r, col)) return true;
      return x->Take(cv);
    }
  };
  std::vector<double> v;
  double counta = 0;
  Ctx ctx{&c, 0, kind, (opt & 1) != 0, opt <= 3, opt == 2 || opt == 3 || opt >= 6, &v, &counta};
  for (int i = 2; i < (arrayForm ? 3 : c.argc); ++i) {
    const Value& a = c.arg(i);
    if (a.IsMissing()) continue;
    if (a.IsError()) return c.Fail(a.error());
    if (a.IsRef()) {
      if (!c.m.Ready(a)) return c.Fail(Err::Value);
      std::vector<Area> areas = a.type() == Type::Ref ? std::vector<Area>{a.area()} : a.areas();
      for (const Area& ar : areas)
        for (int s = ar.s1; s <= ar.s2; ++s) {
          ctx.sheet = s;
          c.m.host.ForEachCell(s, ar.r1, ar.c1, ar.r2, ar.c2, &Ctx::Cell, &ctx);
          if (ctx.failed) return;
        }
    } else if (arrayForm && a.IsArray()) {
      const Array& arr = a.array();
      for (int32_t k = 0, n = arr.size(); k < n; ++k) {
        CellView e = View(arr.data()[k]);
        if (e.type != Type::Number && e.type != Type::Error) continue;  // text and booleans aren't counted
        if (!ctx.Take(e)) return;
      }
    } else if (arrayForm && a.IsNumber()) {
      v.push_back(a.number());
    } else {
      return c.Fail(Err::Value);
    }
  }
  if (kind == 12 || kind >= 14) std::sort(v.begin(), v.end());
  // k: one number, or an array of them (the result is then an array of the same shape).
  Value kv = arrayForm ? c.arg(3) : Value::Number(0);
  if (kv.IsRef()) kv = c.arrayMode ? c.m.RefArray(kv) : c.m.Intersect(kv);
  auto one = [&](const Value& k, Value& out) {
    double d = 0, r = 0;
    Err e = Err::None;
    if (!ToNumber(c.m, k, d, e) || (e = AggregateOf(kind, v, counta, d, r)) != Err::None) {
      out = Value::Error(e);
    } else {
      out = std::isfinite(r) ? Value::Number(r) : Value::Error(Err::Num);
    }
  };
  if (!kv.IsArray()) {
    if (kv.IsError()) return c.Fail(kv.error());
    one(kv, c.out);
    return;
  }
  const Array& ka = kv.array();
  Array* out = Array::Make(ka.rows(), ka.cols());
  for (int32_t r = 0; r < ka.rows(); ++r)
    for (int32_t k = 0; k < ka.cols(); ++k) one(ka.at(r, k), out->at(r, k));
  c.out = Value::Adopt(out);
}

// --- math ---

void Abs(Call& c) {
  double x;
  if (c.Number(0, x)) c.SetNumber(std::fabs(x));
}
void Int(Call& c) {
  double x;
  if (c.Number(0, x)) c.SetNumber(std::floor(x));
}
void Trunc(Call& c) {
  double x, d;
  if (c.Number(0, x) && c.Number(1, d)) c.SetNumber(RoundTo(x, d, 2));
}
void Sign(Call& c) {
  double x;
  if (c.Number(0, x)) c.SetNumber(x > 0 ? 1 : x < 0 ? -1 : 0);
}
void Sqrt(Call& c) {
  double x;
  if (!c.Number(0, x)) return;
  if (x < 0) return c.Fail(Err::Num);
  c.SetNumber(std::sqrt(x));
}
void Exp(Call& c) {
  double x;
  if (c.Number(0, x)) c.SetNumber(std::exp(x));
}
void Ln(Call& c) {
  double x;
  if (!c.Number(0, x)) return;
  if (x <= 0) return c.Fail(Err::Num);
  c.SetNumber(std::log(x));
}
void Log10(Call& c) {
  double x;
  if (!c.Number(0, x)) return;
  if (x <= 0) return c.Fail(Err::Num);
  c.SetNumber(std::log10(x));
}
void Log(Call& c) {
  double x, b;
  if (!c.Number(0, x) || !c.Number(1, b, 10)) return;
  if (x <= 0 || b <= 0) return c.Fail(Err::Num);
  if (b == 1) return c.Fail(Err::Div0);
  c.SetNumber(std::log(x) / std::log(b));
}
void Pi(Call& c) { c.SetNumber(3.14159265358979); }
void Power(Call& c) {
  double x, y;
  if (!c.Number(0, x) || !c.Number(1, y)) return;
  if (x == 0 && y == 0) return c.Fail(Err::Num);
  if (x == 0 && y < 0) return c.Fail(Err::Div0);
  if (x < 0 && y != std::floor(y)) return c.Fail(Err::Num);
  c.SetNumber(std::pow(x, y));
}
void Mod(Call& c) {
  double n, d;
  if (!c.Number(0, n) || !c.Number(1, d)) return;
  if (d == 0) return c.Fail(Err::Div0);
  c.SetNumber(n - d * std::floor(n / d));
}
void Quotient(Call& c) {
  double n, d;
  if (!c.Number(0, n) || !c.Number(1, d)) return;
  if (d == 0) return c.Fail(Err::Div0);
  c.SetNumber(std::trunc(n / d));
}
void Round(Call& c) {
  double x, d;
  if (c.Number(0, x) && c.Number(1, d)) c.SetNumber(RoundTo(x, d, 0));
}
void RoundUp(Call& c) {
  double x, d;
  if (c.Number(0, x) && c.Number(1, d)) c.SetNumber(RoundTo(x, d, 1));
}
void RoundDown(Call& c) {
  double x, d;
  if (c.Number(0, x) && c.Number(1, d)) c.SetNumber(RoundTo(x, d, 2));
}
void MRound(Call& c) {
  double x, m;
  if (!c.Number(0, x) || !c.Number(1, m)) return;
  if (m == 0) return c.SetNumber(0);
  if ((x > 0 && m < 0) || (x < 0 && m > 0)) return c.Fail(Err::Num);
  c.SetNumber(std::floor(Round15(x / m) + 0.5) * m);
}

static double ToMultiple(double x, double sig, bool up) {  // away from (up) or toward zero, in |x|
  double q = Round15(x / sig);
  return (up ? std::ceil(q) : std::floor(q)) * sig;
}
void Ceiling(Call& c) {
  double x, s;
  if (!c.Number(0, x) || !c.Number(1, s)) return;
  if (s == 0 || x == 0) return c.SetNumber(0);
  if (x > 0 && s < 0) return c.Fail(Err::Num);
  // Negative numbers with a negative significance round away from zero; with a positive one, up.
  if (x < 0 && s < 0) return c.SetNumber(-ToMultiple(-x, -s, true));
  c.SetNumber(ToMultiple(x, s, true));
}
void Floor(Call& c) {
  double x, s;
  if (!c.Number(0, x) || !c.Number(1, s)) return;
  if (x == 0) return c.SetNumber(0);
  if (s == 0) return c.Fail(Err::Div0);
  if (x > 0 && s < 0) return c.Fail(Err::Num);
  if (x < 0 && s < 0) return c.SetNumber(-ToMultiple(-x, -s, false));
  c.SetNumber(ToMultiple(x, s, false));
}
void CeilingMath(Call& c) {
  double x, s, mode;
  if (!c.Number(0, x) || !c.Number(1, s, 1) || !c.Number(2, mode)) return;
  if (s == 0 || x == 0) return c.SetNumber(0);
  s = std::fabs(s);
  if (x < 0 && mode != 0) return c.SetNumber(-ToMultiple(-x, s, true));
  c.SetNumber(ToMultiple(x, s, true));
}
void FloorMath(Call& c) {
  double x, s, mode;
  if (!c.Number(0, x) || !c.Number(1, s, 1) || !c.Number(2, mode)) return;
  if (s == 0 || x == 0) return c.SetNumber(0);
  s = std::fabs(s);
  if (x < 0 && mode != 0) return c.SetNumber(-ToMultiple(-x, s, false));
  c.SetNumber(ToMultiple(x, s, false));
}
void Even(Call& c) {
  double x;
  if (!c.Number(0, x)) return;
  double r = std::ceil(std::fabs(x) / 2 - 1e-12) * 2;
  c.SetNumber(x < 0 ? -r : r);
}
void Odd(Call& c) {
  double x;
  if (!c.Number(0, x)) return;
  double a = std::fabs(x);
  double r = std::ceil((a - 1) / 2 - 1e-12) * 2 + 1;
  if (a == 0) r = 1;
  c.SetNumber(x < 0 ? -r : r);
}
void Rand(Call& c) { c.SetNumber(c.m.host.Random()); }
void RandBetween(Call& c) {
  double lo, hi;
  if (!c.Number(0, lo) || !c.Number(1, hi)) return;
  lo = std::ceil(lo), hi = std::floor(hi);
  if (lo > hi) return c.Fail(Err::Num);
  c.SetNumber(lo + std::floor(c.m.host.Random() * (hi - lo + 1)));
}

// --- logic and information ---

static void Logical(Call& c, int kind) {  // 0 AND, 1 OR, 2 XOR
  bool any = false, acc = kind == 0;
  for (int i = 0; i < c.argc; ++i) {
    if (c.arg(i).IsMissing()) continue;
    bool ok = EachItem(c.m, c.arg(i), [&](const CellView& v, From from) {
      bool b;
      if (v.type == Type::Error) {
        c.Fail(v.err);
        return false;
      }
      if (v.type == Type::Number || v.type == Type::Bool) b = v.num != 0;
      else if (v.type == Type::Text && from == From::kDirect) {
        if (EqualText(v.text, "TRUE")) b = true;
        else if (EqualText(v.text, "FALSE")) b = false;
        else {
          c.Fail(Err::Value);
          return false;
        }
      } else {
        return true;
      }
      any = true;
      if (kind == 0) acc = acc && b;
      else if (kind == 1) acc = acc || b;
      else acc = acc != b;
      return true;
    });
    if (!ok) {
      if (!c.out.IsError()) c.Fail(Err::Value);
      return;
    }
  }
  if (!any) return c.Fail(Err::Value);
  c.out = Value::Bool(acc);
}
void And(Call& c) { Logical(c, 0); }
void Or(Call& c) { Logical(c, 1); }
void Xor(Call& c) { Logical(c, 2); }
void Not(Call& c) {
  bool b;
  if (c.Boolean(0, b)) c.out = Value::Bool(!b);
}
void True(Call& c) { c.out = Value::Bool(true); }
void False(Call& c) { c.out = Value::Bool(false); }
void NA(Call& c) { c.Fail(Err::NA); }

void IsBlank(Call& c) { c.out = Value::Bool(c.arg(0).IsEmpty()); }
void IsErr(Call& c) { c.out = Value::Bool(c.arg(0).IsError() && c.arg(0).error() != Err::NA); }
void IsError(Call& c) { c.out = Value::Bool(c.arg(0).IsError()); }
void IsNA(Call& c) { c.out = Value::Bool(c.arg(0).IsError() && c.arg(0).error() == Err::NA); }
void IsLogical(Call& c) { c.out = Value::Bool(c.arg(0).IsBool()); }
void IsNumber(Call& c) { c.out = Value::Bool(c.arg(0).IsNumber()); }
void IsText(Call& c) { c.out = Value::Bool(c.arg(0).IsText()); }
void IsNonText(Call& c) { c.out = Value::Bool(!c.arg(0).IsText()); }
void IsRef(Call& c) { c.out = Value::Bool(c.arg(0).IsRef()); }
static void Parity(Call& c, bool even) {
  if (c.arg(0).IsEmpty()) return void(c.out = Value::Bool(even));
  double x;
  if (!c.Number(0, x)) return;
  bool isEven = std::fmod(std::trunc(std::fabs(x)), 2) == 0;
  c.out = Value::Bool(even == isEven);
}
void IsEven(Call& c) { Parity(c, true); }
void IsOdd(Call& c) { Parity(c, false); }
void ErrorType(Call& c) {
  if (!c.arg(0).IsError()) return c.Fail(Err::NA);
  c.SetNumber(static_cast<double>(c.arg(0).error()));
}
void N(Call& c) {
  const Value& v = c.arg(0);
  if (v.IsError()) return c.Fail(v.error());
  c.SetNumber(v.IsNumber() ? v.number() : v.IsBool() ? (v.boolean() ? 1 : 0) : 0);
}
void TypeFn(Call& c) {
  const Value& v = c.arg(0);
  Value x = v.IsRef() ? c.m.Intersect(v) : v;
  if (v.IsRef() && !v.area().Single()) return c.SetNumber(16);
  switch (x.type()) {
    case Type::Text: return c.SetNumber(2);
    case Type::Bool: return c.SetNumber(4);
    case Type::Error: return c.SetNumber(16);
    case Type::Array: return c.SetNumber(64);
    default: return c.SetNumber(1);
  }
}

// --- financial ---

namespace {
// Values for the annuity functions: rate, nper, pmt, pv, fv, type.
double FvOf(double r, double n, double pmt, double pv, double type) {
  if (r == 0) return -(pv + pmt * n);
  double g = std::pow(1 + r, n);
  return -(pv * g + pmt * (1 + r * type) * (g - 1) / r);
}
}  // namespace

void Pmt(Call& c) {
  double r, n, pv, fv, type;
  if (!c.Number(0, r) || !c.Number(1, n) || !c.Number(2, pv) || !c.Number(3, fv) || !c.Number(4, type)) return;
  type = type != 0 ? 1 : 0;
  if (n == 0) return c.Fail(Err::Num);
  if (r == 0) return c.SetNumber(-(pv + fv) / n);
  if (r <= -1) return c.Fail(Err::Num);
  // As Excel: with 1 - (1+r)^-n from expm1 (the plain form is off in the 15th digit).
  double ln = n * std::log1p(r);
  c.SetNumber(-(pv + fv * std::exp(-ln)) * r / ((1 + r * type) * -std::expm1(-ln)));
}
void Fv(Call& c) {
  double r, n, pmt, pv, type;
  if (!c.Number(0, r) || !c.Number(1, n) || !c.Number(2, pmt) || !c.Number(3, pv) || !c.Number(4, type)) return;
  c.SetNumber(FvOf(r, n, pmt, pv, type != 0 ? 1 : 0));
}
void Pv(Call& c) {
  double r, n, pmt, fv, type;
  if (!c.Number(0, r) || !c.Number(1, n) || !c.Number(2, pmt) || !c.Number(3, fv) || !c.Number(4, type)) return;
  type = type != 0 ? 1 : 0;
  if (r == 0) return c.SetNumber(-(fv + pmt * n));
  double g = std::pow(1 + r, n);
  c.SetNumber(-(fv + pmt * (1 + r * type) * (g - 1) / r) / g);
}
void NPer(Call& c) {
  double r, pmt, pv, fv, type;
  if (!c.Number(0, r) || !c.Number(1, pmt) || !c.Number(2, pv) || !c.Number(3, fv) || !c.Number(4, type)) return;
  type = type != 0 ? 1 : 0;
  if (r == 0) {
    if (pmt == 0) return c.Fail(Err::Num);
    return c.SetNumber(-(pv + fv) / pmt);
  }
  double a = pmt * (1 + r * type) - fv * r, b = pv * r + pmt * (1 + r * type);
  if (a / b <= 0) return c.Fail(Err::Num);
  c.SetNumber(std::log(a / b) / std::log(1 + r));
}
void Rate(Call& c) {
  double n, pmt, pv, fv, type, guess;
  if (!c.Number(0, n) || !c.Number(1, pmt) || !c.Number(2, pv) || !c.Number(3, fv) || !c.Number(4, type) ||
      !c.Number(5, guess, 0.1))
    return;
  type = type != 0 ? 1 : 0;
  double r = guess;
  for (int i = 0; i < 100; ++i) {  // Newton's method, as Excel (20 iterations there; more is harmless)
    double f = -FvOf(r, n, pmt, pv, type) - fv;  // zero at the rate
    double h = std::max(1e-7, std::fabs(r) * 1e-7);
    double f2 = -FvOf(r + h, n, pmt, pv, type) - fv;
    double d = (f2 - f) / h;
    if (d == 0 || !std::isfinite(d)) break;
    double next = r - f / d;
    if (std::fabs(next - r) < 1e-12) return c.SetNumber(next);
    r = next;
  }
  if (std::fabs(-FvOf(r, n, pmt, pv, type) - fv) < 1e-7) return c.SetNumber(r);
  c.Fail(Err::Num);
}
void Npv(Call& c) {
  double r;
  if (!c.Number(0, r)) return;
  double k = 1, s = 0;
  if (EachNumber(c, 1, c.argc, [&](double d) { s += d / std::pow(1 + r, k++); })) c.SetNumber(s);
}

}  // namespace fw::sheet::calc::fn
