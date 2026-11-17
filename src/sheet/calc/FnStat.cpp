// Descriptive statistics and regression: AVEDEV, DEVSQ, GEOMEAN, HARMEAN, KURT, SKEW, SKEW.P,
// STANDARDIZE, CORREL, PEARSON, COVAR, COVARIANCE.P/.S, RSQ, SLOPE, INTERCEPT, STEYX, FORECAST
// (.LINEAR), LINEST, LOGEST, TREND, GROWTH (one x variable), FREQUENCY, PERCENTRANK (.INC/.EXC),
// PROB, TRIMMEAN, MODE.MULT, STDEVA, STDEVPA, VARA, VARPA, FISHER, FISHERINV. Checked against Excel
// 2021 (tests/unit/make_sheet_calc_cases.py).

#include <algorithm>
#include <cmath>
#include <vector>

#include "sheet/calc/Fn.h"

namespace fw::sheet::calc::fn {
namespace {

bool NonEmpty(Call& c, std::vector<double>& v, Err none = Err::Num) {
  if (!Numbers(c, 0, c.argc, v)) return false;
  if (v.empty()) return c.Fail(none), false;
  return true;
}

// Paired numbers of arguments i and j (same size, else #N/A): pairs where both are numbers.
bool Paired(Call& c, int i, int j, std::vector<double>& x, std::vector<double>& y) {
  Value a, b;
  if (!ArgArray(c, i, a) || !ArgArray(c, j, b)) return false;
  const Array &p = a.array(), &q = b.array();
  if (p.size() != q.size()) return c.Fail(Err::NA), false;
  for (int32_t k = 0; k < p.size(); ++k) {
    const Value &u = p.data()[k], &v = q.data()[k];
    if (u.IsError()) return c.Fail(u.error()), false;
    if (v.IsError()) return c.Fail(v.error()), false;
    if (u.IsNumber() && v.IsNumber()) x.push_back(u.number()), y.push_back(v.number());
  }
  return true;
}

struct Fit {
  double n = 0, mx = 0, my = 0, sxx = 0, syy = 0, sxy = 0;
};
Fit Moments(const std::vector<double>& x, const std::vector<double>& y) {
  Fit f;
  f.n = static_cast<double>(x.size());
  f.mx = Mean(x), f.my = Mean(y);
  for (size_t i = 0; i < x.size(); ++i) {
    double dx = x[i] - f.mx, dy = y[i] - f.my;
    f.sxx += dx * dx, f.syy += dy * dy, f.sxy += dx * dy;
  }
  return f;
}

// SLOPE-family arguments: known_y first, known_x second.
bool YX(Call& c, int yi, int xi, Fit& f, size_t minN = 1) {
  std::vector<double> x, y;
  if (!Paired(c, yi, xi, y, x)) return false;
  if (x.size() < minN) return c.Fail(Err::Div0), false;
  f = Moments(x, y);
  return true;
}

void VarianceA(Call& c, bool sample, bool root) {
  std::vector<double> v;
  if (!EachNumberA(c, 0, c.argc, [&](double d) { v.push_back(d); })) return;
  if (v.size() < (sample ? 2u : 1u)) return c.Fail(Err::Div0);
  double s = SquaredDeviations(v) / static_cast<double>(v.size() - (sample ? 1 : 0));
  c.SetNumber(root ? std::sqrt(s) : s);
}

// The known x's for TREND/LINEST: given (one variable, as many as y), or 1, 2, 3...
bool KnownX(Call& c, int i, const Array& y, std::vector<double>& x) {
  if (!c.Has(i)) {
    for (int32_t k = 0; k < y.size(); ++k) x.push_back(k + 1);
    return true;
  }
  Value v;
  if (!ArgArray(c, i, v)) return false;
  const Array& a = v.array();
  if (a.size() != y.size()) {
    c.m.unsupported = true;  // several x variables: kept as Excel calculated it
    return c.Fail(Err::Ref), false;
  }
  for (int32_t k = 0; k < a.size(); ++k) {
    const Value& e = a.data()[k];
    if (e.IsError()) return c.Fail(e.error()), false;
    if (!e.IsNumber()) return c.Fail(Err::Value), false;
    x.push_back(e.number());
  }
  return true;
}

bool KnownY(Call& c, Value& keep, std::vector<double>& y, bool log) {
  if (!ArgArray(c, 0, keep)) return false;
  const Array& a = keep.array();
  for (int32_t k = 0; k < a.size(); ++k) {
    const Value& e = a.data()[k];
    if (e.IsError()) return c.Fail(e.error()), false;
    if (!e.IsNumber()) return c.Fail(Err::Value), false;
    if (log && e.number() <= 0) return c.Fail(Err::Num), false;
    y.push_back(log ? std::log(e.number()) : e.number());
  }
  return true;
}

// Least squares y = m x + b (b = 0 without a constant).
struct Line {
  double m = 0, b = 0, ssreg = 0, ssresid = 0, df = 0, seM = 0, seB = 0, r2 = 0, seY = 0, f = 0;
  bool ok = true;
};
Line Regress(const std::vector<double>& x, const std::vector<double>& y, bool constant) {
  Line l;
  double n = static_cast<double>(x.size());
  if (constant) {
    Fit f = Moments(x, y);
    if (f.sxx == 0) return l.ok = false, l;
    l.m = f.sxy / f.sxx, l.b = f.my - l.m * f.mx;
    l.ssreg = f.sxy * f.sxy / f.sxx;
    l.ssresid = std::max(0.0, f.syy - l.ssreg);
    l.df = n - 2;
    l.r2 = f.syy == 0 ? 1 : l.ssreg / f.syy;
    double s2 = l.df > 0 ? l.ssresid / l.df : 0;
    l.seY = std::sqrt(s2);
    l.seM = std::sqrt(s2 / f.sxx);
    double sx2 = 0;
    for (double d : x) sx2 += d * d;
    l.seB = std::sqrt(s2 * sx2 / (n * f.sxx));
  } else {
    double sxx = 0, sxy = 0, syy = 0;
    for (size_t i = 0; i < x.size(); ++i) sxx += x[i] * x[i], sxy += x[i] * y[i], syy += y[i] * y[i];
    if (sxx == 0) return l.ok = false, l;
    l.m = sxy / sxx;
    l.ssreg = sxy * sxy / sxx;
    l.ssresid = std::max(0.0, syy - l.ssreg);
    l.df = n - 1;
    l.r2 = syy == 0 ? 1 : l.ssreg / syy;
    double s2 = l.df > 0 ? l.ssresid / l.df : 0;
    l.seY = std::sqrt(s2);
    l.seM = std::sqrt(s2 / sxx);
  }
  l.f = l.ssresid == 0 ? 0 : l.ssreg / (l.ssresid / l.df);
  return l;
}

void LineStats(Call& c, bool log) {
  Value ky;
  std::vector<double> y, x;
  bool constant = true, stats = false;
  if (!KnownY(c, ky, y, log) || !KnownX(c, 1, ky.array(), x)) return;
  if (c.Has(2) && !c.Boolean(2, constant, true)) return;
  if (!c.Boolean(3, stats)) return;
  Line l = Regress(x, y, constant);
  if (!l.ok) return c.Fail(Err::Div0);
  auto out_ = [&](double d) { return Value::Number(d); };
  double m = log ? std::exp(l.m) : l.m, b = log ? std::exp(l.b) : (constant ? l.b : 0);
  if (!constant && log) b = 1;
  Array* out = Array::Make(stats ? 5 : 1, 2);
  out->at(0, 0) = out_(m), out->at(0, 1) = out_(b);
  if (stats) {
    out->at(1, 0) = out_(l.seM);
    out->at(1, 1) = constant ? out_(l.seB) : Value::Error(Err::NA);
    out->at(2, 0) = out_(l.r2), out->at(2, 1) = out_(l.seY);
    out->at(3, 0) = l.ssresid == 0 ? Value::Error(Err::Num) : out_(l.f), out->at(3, 1) = out_(l.df);
    out->at(4, 0) = out_(l.ssreg), out->at(4, 1) = out_(l.ssresid);
  }
  c.out = Value::Adopt(out);
}

void Fitted(Call& c, bool log) {
  Value ky;
  std::vector<double> y, x;
  bool constant = true;
  if (!KnownY(c, ky, y, log) || !KnownX(c, 1, ky.array(), x)) return;
  if (c.Has(3) && !c.Boolean(3, constant, true)) return;
  Line l = Regress(x, y, constant);
  if (!l.ok) return c.Fail(Err::Div0);
  Value nx;
  if (c.Has(2)) {
    if (!ArgArray(c, 2, nx)) return;
  } else if (c.Has(1)) {
    if (!ArgArray(c, 1, nx)) return;
  } else {
    nx = ky;  // shape of known_y, values 1, 2, 3...
  }
  const Array& a = nx.array();
  Array* out = Array::Make(a.rows(), a.cols());
  for (int32_t k = 0; k < a.size(); ++k) {
    double xv;
    if (!c.Has(2) && !c.Has(1)) {
      xv = k + 1;
    } else {
      const Value& e = a.data()[k];
      if (e.IsError()) {
        out->data()[k] = e;
        continue;
      }
      if (!e.IsNumber()) {
        out->data()[k] = Value::Error(Err::Value);
        continue;
      }
      xv = e.number();
    }
    double v = l.m * xv + (constant ? l.b : 0);
    out->data()[k] = Value::Number(log ? std::exp(v) : v);
  }
  c.out = Value::Adopt(out);
}

// PERCENTRANK: x's rank in the data (interpolated), truncated to `digits` significant digits.
void PercentRankOf(Call& c, bool exclusive) {
  std::vector<double> v;
  double x, digits;
  if (!Numbers(c, 0, 1, v) || !c.Number(1, x) || !c.Number(2, digits, 3)) return;
  digits = std::trunc(digits);
  if (v.empty() || digits < 1) return c.Fail(Err::Num);
  std::sort(v.begin(), v.end());
  double n = static_cast<double>(v.size());
  if (x < v.front() || x > v.back()) return c.Fail(Err::NA);
  auto rankAt = [&](size_t i) { return exclusive ? (i + 1) / (n + 1) : n == 1 ? 1.0 : i / (n - 1); };
  double r;
  size_t lo = static_cast<size_t>(std::lower_bound(v.begin(), v.end(), x) - v.begin());
  if (v[lo] == x) {
    r = rankAt(lo);
  } else {
    double a = v[lo - 1], b = v[lo];
    r = rankAt(lo - 1) + (x - a) / (b - a) * (rankAt(lo) - rankAt(lo - 1));
  }
  double scale = std::pow(10.0, digits);
  c.SetNumber(std::floor(Round15(r * scale)) / scale);
}

}  // namespace

void AveDev(Call& c) {
  std::vector<double> v;
  if (!NonEmpty(c, v)) return;
  double m = Mean(v), s = 0;
  for (double d : v) s += std::fabs(d - m);
  c.SetNumber(s / static_cast<double>(v.size()));
}
void DevSq(Call& c) {
  std::vector<double> v;
  if (NonEmpty(c, v)) c.SetNumber(SquaredDeviations(v));
}
void GeoMean(Call& c) {
  std::vector<double> v;
  if (!NonEmpty(c, v)) return;
  double s = 0;
  for (double d : v) {
    if (d <= 0) return c.Fail(Err::Num);
    s += std::log(d);
  }
  c.SetNumber(std::exp(s / static_cast<double>(v.size())));
}
void HarMean(Call& c) {
  std::vector<double> v;
  if (!NonEmpty(c, v)) return;
  double s = 0;
  for (double d : v) {
    if (d <= 0) return c.Fail(Err::Num);
    s += 1 / d;
  }
  c.SetNumber(static_cast<double>(v.size()) / s);
}
void Kurt(Call& c) {
  std::vector<double> v;
  if (!NonEmpty(c, v, Err::Div0)) return;
  double n = static_cast<double>(v.size());
  if (n < 4) return c.Fail(Err::Div0);
  double m = Mean(v), sd = std::sqrt(SquaredDeviations(v) / (n - 1));
  if (sd == 0) return c.Fail(Err::Div0);
  double s = 0;
  for (double d : v) s += std::pow((d - m) / sd, 4);
  c.SetNumber(n * (n + 1) / ((n - 1) * (n - 2) * (n - 3)) * s - 3 * (n - 1) * (n - 1) / ((n - 2) * (n - 3)));
}
static void SkewOf(Call& c, bool population) {
  std::vector<double> v;
  if (!NonEmpty(c, v, Err::Div0)) return;
  double n = static_cast<double>(v.size());
  if (!population && n < 3) return c.Fail(Err::Div0);
  double m = Mean(v), sd = std::sqrt(SquaredDeviations(v) / (population ? n : n - 1));
  if (sd == 0) return c.Fail(Err::Div0);
  double s = 0;
  for (double d : v) s += std::pow((d - m) / sd, 3);
  c.SetNumber(population ? s / n : n / ((n - 1) * (n - 2)) * s);
}
void Skew(Call& c) { SkewOf(c, false); }
void SkewP(Call& c) { SkewOf(c, true); }
void Standardize(Call& c) {
  double x, m, sd;
  if (!c.Number(0, x) || !c.Number(1, m) || !c.Number(2, sd)) return;
  if (sd <= 0) return c.Fail(Err::Num);
  c.SetNumber((x - m) / sd);
}

void Correl(Call& c) {
  Fit f;
  if (!YX(c, 0, 1, f)) return;
  if (f.sxx == 0 || f.syy == 0) return c.Fail(Err::Div0);
  c.SetNumber(f.sxy / std::sqrt(f.sxx * f.syy));
}
void Rsq(Call& c) {
  Fit f;
  if (!YX(c, 0, 1, f)) return;
  if (f.sxx == 0 || f.syy == 0) return c.Fail(Err::Div0);
  c.SetNumber(f.sxy * f.sxy / (f.sxx * f.syy));
}
void CovarP(Call& c) {
  Fit f;
  if (YX(c, 0, 1, f)) c.SetNumber(f.sxy / f.n);
}
void CovarS(Call& c) {
  Fit f;
  if (!YX(c, 0, 1, f)) return;
  if (f.n < 2) return c.Fail(Err::Div0);
  c.SetNumber(f.sxy / (f.n - 1));
}
void Slope(Call& c) {
  Fit f;
  if (!YX(c, 0, 1, f)) return;
  if (f.sxx == 0) return c.Fail(Err::Div0);
  c.SetNumber(f.sxy / f.sxx);
}
void Intercept(Call& c) {
  Fit f;
  if (!YX(c, 0, 1, f)) return;
  if (f.sxx == 0) return c.Fail(Err::Div0);
  c.SetNumber(f.my - f.sxy / f.sxx * f.mx);
}
void SteYX(Call& c) {
  Fit f;
  if (!YX(c, 0, 1, f, 3)) return;
  if (f.sxx == 0) return c.Fail(Err::Div0);
  c.SetNumber(std::sqrt(std::max(0.0, f.syy - f.sxy * f.sxy / f.sxx) / (f.n - 2)));
}
void Forecast(Call& c) {
  double x;
  if (!c.Number(0, x)) return;
  Fit f;
  if (!YX(c, 1, 2, f)) return;
  if (f.sxx == 0) return c.Fail(Err::Div0);
  double m = f.sxy / f.sxx;
  c.SetNumber(f.my - m * f.mx + m * x);
}
void Linest(Call& c) { LineStats(c, false); }
void Logest(Call& c) { LineStats(c, true); }
void Trend(Call& c) { Fitted(c, false); }
void Growth(Call& c) { Fitted(c, true); }

// FREQUENCY(data_array, bins_array): a column, one count per bin and one past the last.
void Frequency(Call& c) {
  std::vector<double> data, bins;
  if (!Numbers(c, 0, 1, data) || !Numbers(c, 1, 2, bins)) return;
  std::vector<size_t> order(bins.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return bins[a] < bins[b]; });
  std::vector<double> counts(bins.size() + 1, 0);
  for (double d : data) {
    size_t k = 0;
    while (k < order.size() && d > bins[order[k]]) ++k;
    counts[k < order.size() ? order[k] : bins.size()] += 1;
  }
  Array* out = Array::Make(static_cast<int32_t>(counts.size()), 1);
  for (size_t i = 0; i < counts.size(); ++i) out->at(static_cast<int32_t>(i), 0) = Value::Number(counts[i]);
  c.out = Value::Adopt(out);
}

void PercentRankInc(Call& c) { PercentRankOf(c, false); }
void PercentRankExc(Call& c) { PercentRankOf(c, true); }

void Prob(Call& c) {
  std::vector<double> x, p;
  double lo, hi;
  if (!Paired(c, 0, 1, x, p) || !c.Number(2, lo)) return;
  if (!c.Number(3, hi, lo)) return;
  double total = 0, sum = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    if (p[i] <= 0 || p[i] > 1) return c.Fail(Err::Num);
    total += p[i];
    if (x[i] >= lo && x[i] <= hi) sum += p[i];
  }
  if (std::fabs(total - 1) > 1e-7) return c.Fail(Err::Num);
  c.SetNumber(sum);
}

void TrimMean(Call& c) {
  std::vector<double> v;
  double pct;
  if (!Numbers(c, 0, 1, v) || !c.Number(1, pct)) return;
  if (pct < 0 || pct >= 1 || v.empty()) return c.Fail(Err::Num);
  std::sort(v.begin(), v.end());
  size_t cut = static_cast<size_t>(std::floor(static_cast<double>(v.size()) * pct / 2));
  std::vector<double> mid(v.begin() + static_cast<std::ptrdiff_t>(cut), v.end() - static_cast<std::ptrdiff_t>(cut));
  c.SetNumber(Mean(mid));
}

void ModeMult(Call& c) {
  std::vector<double> v;
  if (!Numbers(c, 0, c.argc, v)) return;
  size_t best = 1;
  std::vector<double> modes;
  for (size_t i = 0; i < v.size(); ++i) {
    size_t n = 0;
    bool seen = false;
    for (size_t j = 0; j < v.size(); ++j) {
      n += v[j] == v[i];
      seen = seen || (j < i && v[j] == v[i]);
    }
    if (seen) continue;
    if (n > best) best = n, modes.assign(1, v[i]);
    else if (n == best && best > 1) modes.push_back(v[i]);
  }
  if (modes.empty()) return c.Fail(Err::NA);
  Array* out = Array::Make(static_cast<int32_t>(modes.size()), 1);
  for (size_t i = 0; i < modes.size(); ++i) out->at(static_cast<int32_t>(i), 0) = Value::Number(modes[i]);
  c.out = Value::Adopt(out);
}

void StDevA(Call& c) { VarianceA(c, true, true); }
void StDevPA(Call& c) { VarianceA(c, false, true); }
void VarA(Call& c) { VarianceA(c, true, false); }
void VarPA(Call& c) { VarianceA(c, false, false); }

void Fisher(Call& c) {
  double x;
  if (!c.Number(0, x)) return;
  if (x <= -1 || x >= 1) return c.Fail(Err::Num);
  c.SetNumber(0.5 * std::log((1 + x) / (1 - x)));
}
void FisherInv(Call& c) {
  double y;
  if (c.Number(0, y)) c.SetNumber(std::tanh(y));
}

}  // namespace fw::sheet::calc::fn
