// Financial functions beyond PMT, PV, FV, NPER, RATE and NPV: loans (IPMT, PPMT, CUMIPMT,
// CUMPRINC, ISPMT), rates (EFFECT, NOMINAL, PDURATION, RRI, FVSCHEDULE), cash flows (IRR, XIRR,
// XNPV, MIRR), depreciation (SLN, SYD, DB, DDB, VDB), dollar fractions (DOLLARDE, DOLLARFR) and
// discounted securities (DISC, INTRATE, RECEIVED, PRICEDISC, YIELDDISC, TBILLEQ, TBILLPRICE,
// TBILLYIELD). Checked against Excel 2021 (tests/unit/make_sheet_calc_cases.py).

#include <algorithm>
#include <cmath>
#include <vector>

#include "sheet/calc/Fn.h"

namespace fw::sheet::calc::fn {
namespace {

double FvOf(double r, double n, double pmt, double pv, bool type) {
  if (r == 0) return -(pv + pmt * n);
  double g = std::pow(1 + r, n);
  return -(pv * g + pmt * (1 + r * (type ? 1 : 0)) * (g - 1) / r);
}

double PmtOf(double r, double n, double pv, double fv, bool type) {
  if (r == 0) return -(pv + fv) / n;
  double ln = n * std::log1p(r);
  return -(pv + fv * std::exp(-ln)) * r / ((1 + r * (type ? 1 : 0)) * -std::expm1(-ln));
}

// The interest in period `per` (LibreOffice's and Excel's way: from the balance before it).
double IpmtOf(double r, double per, double n, double pv, double fv, bool type, double& pmt) {
  pmt = PmtOf(r, n, pv, fv, type);
  double i;
  if (per == 1) i = type ? 0 : -pv;
  else i = type ? FvOf(r, per - 2, pmt, pv, true) - pmt : FvOf(r, per - 1, pmt, pv, false);
  return i * r;
}

bool Loan(Call& c, double& r, double& per, double& n, double& pv, double& fv, bool& type) {
  double t;
  if (!c.Number(0, r) || !c.Number(1, per) || !c.Number(2, n) || !c.Number(3, pv) || !c.Number(4, fv) ||
      !c.Number(5, t))
    return false;
  type = t != 0;
  if (per < 1 || per >= n + 1) return c.Fail(Err::Num), false;
  return true;
}

// The rate at which f is zero, by Newton's method from `guess` (IRR, XIRR).
template <class F>
bool Solve(F&& f, double guess, double& out) {
  double r = guess;
  for (int i = 0; i < 200; ++i) {
    double y = f(r);
    double h = std::max(1e-9, std::fabs(r) * 1e-9);
    double d = (f(r + h) - f(r - h)) / (2 * h);
    if (!std::isfinite(y) || !std::isfinite(d) || d == 0) return false;
    double next = r - y / d;
    if (next <= -1) next = (r - 1) / 2;
    if (std::fabs(next - r) <= 1e-15 * std::max(1.0, std::fabs(r))) return out = next, true;
    r = next;
  }
  return std::fabs(f(r)) < 1e-7 ? (out = r, true) : false;
}

// DDB's depreciation in period `per` (LibreOffice's ScGetDDB, which Excel matches).
double DdbOf(double cost, double salvage, double life, double per, double factor) {
  double rate = factor / life, before;
  if (rate >= 1) {
    rate = 1;
    before = per == 1 ? cost : 0;
  } else {
    before = cost * std::pow(1 - rate, per - 1);
  }
  double after = cost * std::pow(1 - rate, per);
  double d = after < salvage ? before - salvage : before - after;
  return d < 0 ? 0 : d;
}

// VDB's depreciation up to `period` (fractional), switching to straight line when that's more.
double InterVdb(double cost, double salvage, double life, double life1, double period, double factor) {
  double vdb = 0, end = std::ceil(period), sln = 0, left = cost - salvage;
  bool straight = false;
  for (double i = 1; i <= end; ++i) {
    double term;
    if (!straight) {
      double ddb = DdbOf(cost, salvage, life, i, factor);
      sln = left / (life1 - (i - 1));
      if (sln > ddb) {
        term = sln;
        straight = true;
      } else {
        term = ddb;
        left -= ddb;
      }
    } else {
      term = sln;
    }
    if (i == end) term *= period + 1 - end;
    vdb += term;
  }
  return vdb;
}

// Settlement and maturity serials (whole days), and the basis 0-4.
bool Security(Call& c, double& s, double& m, int& basis, int basisArg) {
  double b;
  if (!SerialArg(c, 0, s) || !SerialArg(c, 1, m) || !c.Integer(basisArg, b, 0)) return false;
  s = std::floor(s), m = std::floor(m);
  if (b < 0 || b > 4 || s >= m) return c.Fail(Err::Num), false;
  basis = static_cast<int>(b);
  return true;
}

bool TBill(Call& c, double& s, double& m, double& x) {
  if (!SerialArg(c, 0, s) || !SerialArg(c, 1, m) || !c.Number(2, x)) return false;
  s = std::floor(s), m = std::floor(m);
  double year;
  int y, mo, d;
  SerialDate(s, c.m.date1904, y, mo, d);
  if (!DateSerial(y + 1, mo, d, c.m.date1904, year)) year = s + 366;
  if (s >= m || m > year || x <= 0) return c.Fail(Err::Num), false;
  return true;
}

}  // namespace

void Ipmt(Call& c) {
  double r, per, n, pv, fv, pmt;
  bool type;
  if (Loan(c, r, per, n, pv, fv, type)) c.SetNumber(IpmtOf(r, per, n, pv, fv, type, pmt));
}

void Ppmt(Call& c) {
  double r, per, n, pv, fv, pmt;
  bool type;
  if (!Loan(c, r, per, n, pv, fv, type)) return;
  double i = IpmtOf(r, per, n, pv, fv, type, pmt);
  c.SetNumber(pmt - i);
}

static void Cumulative(Call& c, bool interest) {
  double r, n, pv, s, e, t;
  if (!c.Number(0, r) || !c.Number(1, n) || !c.Number(2, pv) || !c.Number(3, s) || !c.Number(4, e) || !c.Number(5, t))
    return;
  s = std::ceil(s), e = std::floor(e);  // Excel: the periods wholly inside
  if (r <= 0 || n <= 0 || pv <= 0 || s < 1 || e < s || e > n || (t != 0 && t != 1)) return c.Fail(Err::Num);
  bool type = t == 1;
  double pmt = PmtOf(r, n, pv, 0, type), sum = 0;
  if (s == 1) {
    if (interest) sum = type ? 0 : -pv;
    else sum = type ? pmt : pmt + pv * r;
    ++s;
  }
  for (double i = s; i <= e; ++i) {
    double before = type ? FvOf(r, i - 2, pmt, pv, true) - pmt : FvOf(r, i - 1, pmt, pv, false);
    sum += interest ? before : pmt - before * r;
  }
  c.SetNumber(interest ? sum * r : sum);
}
void CumIpmt(Call& c) { Cumulative(c, true); }
void CumPrinc(Call& c) { Cumulative(c, false); }

void IsPmt(Call& c) {
  double r, per, n, pv;
  if (!c.Number(0, r) || !c.Number(1, per) || !c.Number(2, n) || !c.Number(3, pv)) return;
  if (n == 0) return c.Fail(Err::Div0);
  c.SetNumber(pv * r * (per / n - 1));
}

void Effect(Call& c) {
  double rate, n;
  if (!c.Number(0, rate) || !c.Number(1, n)) return;
  n = std::trunc(n);
  if (rate <= 0 || n < 1) return c.Fail(Err::Num);
  c.SetNumber(std::pow(1 + rate / n, n) - 1);
}

void Nominal(Call& c) {
  double rate, n;
  if (!c.Number(0, rate) || !c.Number(1, n)) return;
  n = std::trunc(n);
  if (rate <= 0 || n < 1) return c.Fail(Err::Num);
  c.SetNumber((std::pow(rate + 1, 1 / n) - 1) * n);
}

void PDuration(Call& c) {
  double r, pv, fv;
  if (!c.Number(0, r) || !c.Number(1, pv) || !c.Number(2, fv)) return;
  if (r <= 0 || pv <= 0 || fv <= 0) return c.Fail(Err::Num);
  c.SetNumber((std::log(fv) - std::log(pv)) / std::log1p(r));
}

void Rri(Call& c) {
  double n, pv, fv;
  if (!c.Number(0, n) || !c.Number(1, pv) || !c.Number(2, fv)) return;
  if (n <= 0 || pv == 0) return c.Fail(Err::Num);
  double q = fv / pv;
  if (q < 0) return c.Fail(Err::Num);
  c.SetNumber(std::pow(q, 1 / n) - 1);
}

void FvSchedule(Call& c) {
  double p;
  if (!c.Number(0, p)) return;
  bool ok = EachItem(c.m, c.arg(1), [&](const CellView& v, From) {
    if (v.type == Type::Error) return c.Fail(v.err), false;
    if (v.type == Type::Empty) return true;
    if (v.type != Type::Number) return c.Fail(Err::Value), false;  // text too, as Excel
    p *= 1 + v.num;
    return true;
  });
  if (!ok) {
    if (!c.out.IsError()) c.Fail(Err::Value);
    return;
  }
  c.SetNumber(p);
}

void Irr(Call& c) {
  std::vector<double> v;
  double guess;
  if (!Numbers(c, 0, 1, v) || !c.Number(1, guess, 0.1)) return;
  bool pos = false, neg = false;
  for (double d : v) pos = pos || d > 0, neg = neg || d < 0;
  if (!pos || !neg) return c.Fail(Err::Num);
  auto npv = [&](double r) {
    double s = 0, f = 1;
    for (double d : v) s += d / f, f *= 1 + r;
    return s;
  };
  double r;
  if (!Solve(npv, guess, r)) return c.Fail(Err::Num);
  c.SetNumber(r);
}

static bool CashFlows(Call& c, int vi, int di, std::vector<double>& v, std::vector<double>& d) {
  if (!Numbers(c, vi, vi + 1, v) || !Numbers(c, di, di + 1, d)) return false;
  if (v.size() != d.size() || v.empty()) return c.Fail(Err::Num), false;
  for (double& x : d) {
    x = std::floor(x);
    if (x < 0 || x < std::floor(d[0])) return c.Fail(Err::Num), false;
  }
  return true;
}

void Xnpv(Call& c) {
  double r;
  std::vector<double> v, d;
  if (!c.Number(0, r) || !CashFlows(c, 1, 2, v, d)) return;
  if (r <= -1) return c.Fail(Err::Num);
  double s = 0;
  for (size_t i = 0; i < v.size(); ++i) s += v[i] / std::pow(1 + r, (d[i] - d[0]) / 365);
  c.SetNumber(s);
}

void Xirr(Call& c) {
  std::vector<double> v, d;
  double guess;
  if (!CashFlows(c, 0, 1, v, d) || !c.Number(2, guess, 0.1)) return;
  bool pos = false, neg = false;
  for (double x : v) pos = pos || x > 0, neg = neg || x < 0;
  if (!pos || !neg) return c.Fail(Err::Num);
  auto f = [&](double r) {
    double s = 0;
    for (size_t i = 0; i < v.size(); ++i) s += v[i] / std::pow(1 + r, (d[i] - d[0]) / 365);
    return s;
  };
  double r;
  if (!Solve(f, guess, r)) return c.Fail(Err::Num);
  c.SetNumber(r);
}

void Mirr(Call& c) {
  std::vector<double> v;
  double fr, rr;
  if (!Numbers(c, 0, 1, v) || !c.Number(1, fr) || !c.Number(2, rr)) return;
  double n = static_cast<double>(v.size()), pos = 0, neg = 0, k = 0;
  for (double d : v) {
    if (d > 0) pos += d / std::pow(1 + rr, k);
    else if (d < 0) neg += d / std::pow(1 + fr, k);
    ++k;
  }
  if (pos == 0 || neg == 0 || n < 2) return c.Fail(Err::Div0);
  c.SetNumber(std::pow(-pos * std::pow(1 + rr, n - 1) / neg, 1 / (n - 1)) - 1);
}

void Sln(Call& c) {
  double cost, salvage, life;
  if (!c.Number(0, cost) || !c.Number(1, salvage) || !c.Number(2, life)) return;
  if (life == 0) return c.Fail(Err::Div0);
  c.SetNumber((cost - salvage) / life);
}

void Syd(Call& c) {
  double cost, salvage, life, per;
  if (!c.Number(0, cost) || !c.Number(1, salvage) || !c.Number(2, life) || !c.Number(3, per)) return;
  if (life <= 0 || per <= 0 || per > life) return c.Fail(Err::Num);
  c.SetNumber((cost - salvage) * (life - per + 1) * 2 / (life * (life + 1)));
}

void Db(Call& c) {
  double cost, salvage, life, per, month;
  if (!c.Number(0, cost) || !c.Number(1, salvage) || !c.Number(2, life) || !c.Number(3, per) || !c.Number(4, month, 12))
    return;
  per = std::trunc(per), month = std::trunc(month);
  if (cost < 0 || salvage < 0 || life <= 0 || per <= 0 || month < 1 || month > 12 || per > life + (month < 12 ? 1 : 0))
    return c.Fail(Err::Num);
  if (cost == 0) return c.SetNumber(0);
  double rate = std::round((1 - std::pow(salvage / cost, 1 / life)) * 1000) / 1000;
  double total = cost * rate * month / 12, d = total;
  if (per == 1) return c.SetNumber(d);
  for (double i = 2; i <= per; ++i) {
    d = i == life + 1 ? (cost - total) * rate * (12 - month) / 12 : (cost - total) * rate;
    total += d;
  }
  c.SetNumber(d);
}

void Ddb(Call& c) {
  double cost, salvage, life, per, factor;
  if (!c.Number(0, cost) || !c.Number(1, salvage) || !c.Number(2, life) || !c.Number(3, per) ||
      !c.Number(4, factor, 2))
    return;
  if (cost < 0 || salvage < 0 || life <= 0 || per <= 0 || per > life || factor <= 0) return c.Fail(Err::Num);
  c.SetNumber(DdbOf(cost, salvage, life, per, factor));
}

void Vdb(Call& c) {
  double cost, salvage, life, start, end, factor;
  bool noSwitch;
  if (!c.Number(0, cost) || !c.Number(1, salvage) || !c.Number(2, life) || !c.Number(3, start) || !c.Number(4, end) ||
      !c.Number(5, factor, 2) || !c.Boolean(6, noSwitch))
    return;
  if (start < 0 || end < start || end > life || cost < 0 || salvage > cost || factor <= 0) return c.Fail(Err::Num);
  double first = std::floor(start), last = std::ceil(end), vdb = 0;
  if (noSwitch) {
    for (double i = first + 1; i <= last; ++i) {
      double term = DdbOf(cost, salvage, life, i, factor);
      if (i == first + 1) term *= std::min(end, first + 1) - start;
      else if (i == last) term *= end + 1 - last;
      vdb += term;
    }
    return c.SetNumber(vdb);
  }
  cost -= InterVdb(cost, salvage, life, life, start, factor);
  c.SetNumber(InterVdb(cost, salvage, life, life - start, end - start, factor));
}

static double Fraction(double x, double f, bool toDecimal) {
  double digits = std::pow(10.0, std::ceil(std::log10(f)));
  double whole = std::trunc(x), part = x - whole;
  return toDecimal ? whole + part * digits / f : whole + part * f / digits;
}
static void DollarFraction(Call& c, bool toDecimal) {
  double x, f;
  if (!c.Number(0, x) || !c.Number(1, f)) return;
  f = std::trunc(f);
  if (f < 0) return c.Fail(Err::Num);
  if (f == 0) return c.Fail(Err::Div0);
  c.SetNumber(Fraction(x, f, toDecimal));
}
void DollarDe(Call& c) { DollarFraction(c, true); }
void DollarFr(Call& c) { DollarFraction(c, false); }

void Disc(Call& c) {
  double s, m, pr, redemption;
  int basis;
  if (!Security(c, s, m, basis, 4) || !c.Number(2, pr) || !c.Number(3, redemption)) return;
  if (pr <= 0 || redemption <= 0) return c.Fail(Err::Num);
  c.SetNumber((redemption - pr) / redemption / YearFracOf(s, m, basis, c.m.date1904));
}

void IntRate(Call& c) {
  double s, m, investment, redemption;
  int basis;
  if (!Security(c, s, m, basis, 4) || !c.Number(2, investment) || !c.Number(3, redemption)) return;
  if (investment <= 0 || redemption <= 0) return c.Fail(Err::Num);
  c.SetNumber((redemption - investment) / investment / YearFracOf(s, m, basis, c.m.date1904));
}

void Received(Call& c) {
  double s, m, investment, discount;
  int basis;
  if (!Security(c, s, m, basis, 4) || !c.Number(2, investment) || !c.Number(3, discount)) return;
  if (investment <= 0 || discount <= 0) return c.Fail(Err::Num);
  double k = 1 - discount * YearFracOf(s, m, basis, c.m.date1904);
  if (k <= 0) return c.Fail(Err::Num);
  c.SetNumber(investment / k);
}

void PriceDisc(Call& c) {
  double s, m, discount, redemption;
  int basis;
  if (!Security(c, s, m, basis, 4) || !c.Number(2, discount) || !c.Number(3, redemption)) return;
  if (discount <= 0 || redemption <= 0) return c.Fail(Err::Num);
  c.SetNumber(redemption * (1 - discount * YearFracOf(s, m, basis, c.m.date1904)));
}

void YieldDisc(Call& c) {
  double s, m, pr, redemption;
  int basis;
  if (!Security(c, s, m, basis, 4) || !c.Number(2, pr) || !c.Number(3, redemption)) return;
  if (pr <= 0 || redemption <= 0) return c.Fail(Err::Num);
  c.SetNumber((redemption / pr - 1) / YearFracOf(s, m, basis, c.m.date1904));
}

void TBillPrice(Call& c) {
  double s, m, discount;
  if (!TBill(c, s, m, discount)) return;
  double p = 100 * (1 - discount * (m - s) / 360);
  if (p <= 0) return c.Fail(Err::Num);
  c.SetNumber(p);
}

void TBillYield(Call& c) {
  double s, m, pr;
  if (!TBill(c, s, m, pr)) return;
  c.SetNumber((100 - pr) / pr * 360 / (m - s));
}

void TBillEq(Call& c) {
  double s, m, discount;
  if (!TBill(c, s, m, discount)) return;
  double dsm = m - s;
  if (dsm <= 182) return c.SetNumber(365 * discount / (360 - discount * dsm));
  // Over half a year: the bond-equivalent yield with a coupon at half-year (Excel's formula).
  double price = 100 * (1 - discount * dsm / 360);
  double a = dsm / 365, b = 2 * a - 1;
  double r = (-a + std::sqrt(a * a - b * (1 - 100 / price))) / (a - 0.5);
  (void)b;
  c.SetNumber(r);
}

}  // namespace fw::sheet::calc::fn
