// Trigonometry, combinatorics, number bases and matrices: SIN ... SECH, DEGREES, RADIANS, FACT,
// FACTDOUBLE, COMBIN, COMBINA, PERMUT, PERMUTATIONA, GCD, LCM, MULTINOMIAL, SQRTPI, SERIESSUM,
// ROMAN, ARABIC, BASE, DECIMAL, SUMX2MY2, SUMX2PY2, SUMXMY2, MDETERM, MINVERSE, MMULT, MUNIT.
// Checked against Excel 2021 (tests/unit/make_sheet_calc_cases.py).

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "sheet/calc/Fn.h"

namespace fw::sheet::calc::fn {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTrigLimit = 134217728;  // 2^27: Excel's SIN, COS, TAN refuse larger arguments
constexpr double kMaxExact = 9007199254740992;  // 2^53

template <class F>
void Unary(Call& c, F&& f) {
  double x;
  if (c.Number(0, x)) f(x);
}

// Numbers of an 'R' argument list, truncated, for GCD, LCM and MULTINOMIAL; negatives are #NUM!.
bool Whole(Call& c, std::vector<double>& out) {
  if (!Numbers(c, 0, c.argc, out)) return false;
  for (double& d : out) {
    d = std::trunc(d);
    if (d < 0 || d >= kMaxExact) return c.Fail(Err::Num), false;
  }
  return true;
}

double Gcd(double a, double b) {
  while (b != 0) {
    double t = std::fmod(a, b);
    a = b, b = t;
  }
  return a;
}

// C(n, k) by products, rounded (exact while it fits).
double Combin(double n, double k) {
  k = std::min(k, n - k);
  double r = 1;
  for (double i = 1; i <= k; ++i) r = r * (n - k + i) / i;
  return std::round(r);
}

// A numeric matrix from argument i: every element a number (else #VALUE!).
bool Matrix(Call& c, int i, Value& keep, int32_t& rows, int32_t& cols, std::vector<double>& m) {
  if (!ArgArray(c, i, keep)) return false;
  const Array& a = keep.array();
  rows = a.rows(), cols = a.cols();
  m.resize(static_cast<size_t>(rows) * cols);
  for (int32_t k = 0; k < a.size(); ++k) {
    const Value& v = a.data()[k];
    if (v.IsError()) return c.Fail(v.error()), false;
    if (!v.IsNumber()) return c.Fail(Err::Value), false;
    m[static_cast<size_t>(k)] = v.number();
  }
  return true;
}

// LU decomposition with partial pivoting in place; false when singular. `sign` gets the
// permutation's sign, `perm` the row order.
bool Lu(std::vector<double>& a, int n, std::vector<int>& perm, double& sign) {
  perm.resize(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) perm[static_cast<size_t>(i)] = i;
  sign = 1;
  auto at = [&](int r, int c) -> double& { return a[static_cast<size_t>(r) * n + c]; };
  for (int k = 0; k < n; ++k) {
    int p = k;
    for (int i = k + 1; i < n; ++i)
      if (std::fabs(at(i, k)) > std::fabs(at(p, k))) p = i;
    if (at(p, k) == 0) return false;
    if (p != k) {
      for (int j = 0; j < n; ++j) std::swap(at(p, j), at(k, j));
      std::swap(perm[static_cast<size_t>(p)], perm[static_cast<size_t>(k)]);
      sign = -sign;
    }
    for (int i = k + 1; i < n; ++i) {
      at(i, k) /= at(k, k);
      for (int j = k + 1; j < n; ++j) at(i, j) -= at(i, k) * at(k, j);
    }
  }
  return true;
}

// Pairs of numbers from two same-size arrays (SUMX2MY2 and friends): positions where either isn't
// a number are skipped; different sizes are #N/A.
bool Pairs(Call& c, std::vector<double>& x, std::vector<double>& y) {
  Value a, b;
  if (!ArgArray(c, 0, a) || !ArgArray(c, 1, b)) return false;
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

}  // namespace

void Sin(Call& c) {
  Unary(c, [&](double x) { std::fabs(x) >= kTrigLimit ? c.Fail(Err::Num) : c.SetNumber(std::sin(x)); });
}
void Cos(Call& c) {
  Unary(c, [&](double x) { std::fabs(x) >= kTrigLimit ? c.Fail(Err::Num) : c.SetNumber(std::cos(x)); });
}
void Tan(Call& c) {
  Unary(c, [&](double x) { std::fabs(x) >= kTrigLimit ? c.Fail(Err::Num) : c.SetNumber(std::tan(x)); });
}
void Asin(Call& c) {
  Unary(c, [&](double x) { std::fabs(x) > 1 ? c.Fail(Err::Num) : c.SetNumber(std::asin(x)); });
}
void Acos(Call& c) {
  Unary(c, [&](double x) { std::fabs(x) > 1 ? c.Fail(Err::Num) : c.SetNumber(std::acos(x)); });
}
void Atan(Call& c) {
  Unary(c, [&](double x) { c.SetNumber(std::atan(x)); });
}
void Atan2(Call& c) {
  double x, y;
  if (!c.Number(0, x) || !c.Number(1, y)) return;
  if (x == 0 && y == 0) return c.Fail(Err::Div0);
  c.SetNumber(std::atan2(y, x));
}
void Sinh(Call& c) {
  Unary(c, [&](double x) { c.SetNumber(std::sinh(x)); });
}
void Cosh(Call& c) {
  Unary(c, [&](double x) { c.SetNumber(std::cosh(x)); });
}
void Tanh(Call& c) {
  Unary(c, [&](double x) { c.SetNumber(std::tanh(x)); });
}
void Asinh(Call& c) {
  Unary(c, [&](double x) { c.SetNumber(std::asinh(x)); });
}
void Acosh(Call& c) {
  Unary(c, [&](double x) { x < 1 ? c.Fail(Err::Num) : c.SetNumber(std::acosh(x)); });
}
void Atanh(Call& c) {
  Unary(c, [&](double x) { std::fabs(x) >= 1 ? c.Fail(Err::Num) : c.SetNumber(std::atanh(x)); });
}
void Cot(Call& c) {
  Unary(c, [&](double x) {
    if (std::fabs(x) >= kTrigLimit) return c.Fail(Err::Num);
    if (x == 0) return c.Fail(Err::Div0);
    c.SetNumber(1 / std::tan(x));
  });
}
void Coth(Call& c) {
  Unary(c, [&](double x) { x == 0 ? c.Fail(Err::Div0) : c.SetNumber(1 / std::tanh(x)); });
}
void Acot(Call& c) {
  Unary(c, [&](double x) { c.SetNumber(kPi / 2 - std::atan(x)); });
}
void Acoth(Call& c) {
  Unary(c, [&](double x) { std::fabs(x) <= 1 ? c.Fail(Err::Num) : c.SetNumber(0.5 * std::log((x + 1) / (x - 1))); });
}
void Csc(Call& c) {
  Unary(c, [&](double x) {
    if (std::fabs(x) >= kTrigLimit) return c.Fail(Err::Num);
    if (x == 0) return c.Fail(Err::Div0);
    c.SetNumber(1 / std::sin(x));
  });
}
void Csch(Call& c) {
  Unary(c, [&](double x) { x == 0 ? c.Fail(Err::Div0) : c.SetNumber(1 / std::sinh(x)); });
}
void Sec(Call& c) {
  Unary(c, [&](double x) { std::fabs(x) >= kTrigLimit ? c.Fail(Err::Num) : c.SetNumber(1 / std::cos(x)); });
}
void Sech(Call& c) {
  Unary(c, [&](double x) { c.SetNumber(1 / std::cosh(x)); });
}
void Degrees(Call& c) {
  Unary(c, [&](double x) { c.SetNumber(x * 180 / kPi); });
}
void Radians(Call& c) {
  Unary(c, [&](double x) { c.SetNumber(x * kPi / 180); });
}
void SqrtPi(Call& c) {
  Unary(c, [&](double x) { x < 0 ? c.Fail(Err::Num) : c.SetNumber(std::sqrt(x * kPi)); });
}

void Fact(Call& c) {
  Unary(c, [&](double x) {
    x = std::trunc(x);
    if (x < 0 || x > 170) return c.Fail(Err::Num);
    double r = 1;
    for (double i = 2; i <= x; ++i) r *= i;
    c.SetNumber(r);
  });
}
void FactDouble(Call& c) {
  Unary(c, [&](double x) {
    x = std::trunc(x);
    if (x < -1 || x > 300) return c.Fail(Err::Num);
    double r = 1;
    for (double i = x; i > 1; i -= 2) r *= i;
    c.SetNumber(r);
  });
}

static bool TwoWhole(Call& c, double& n, double& k) {
  if (!c.Number(0, n) || !c.Number(1, k)) return false;
  n = std::trunc(n), k = std::trunc(k);
  if (n < 0 || k < 0) return c.Fail(Err::Num), false;
  return true;
}
void CombinFn(Call& c) {
  double n, k;
  if (!TwoWhole(c, n, k)) return;
  if (n < k) return c.Fail(Err::Num);
  c.SetNumber(Combin(n, k));
}
void CombinA(Call& c) {
  double n, k;
  if (!TwoWhole(c, n, k)) return;
  if (n == 0 && k == 0) return c.SetNumber(1);
  if (n == 0) return c.Fail(Err::Num);
  c.SetNumber(Combin(n + k - 1, k));
}
void Permut(Call& c) {
  double n, k;
  if (!TwoWhole(c, n, k)) return;
  if (n < k) return c.Fail(Err::Num);
  double r = 1;
  for (double i = n - k + 1; i <= n; ++i) r *= i;
  c.SetNumber(r);
}
void PermutationA(Call& c) {
  double n, k;
  if (!TwoWhole(c, n, k)) return;
  c.SetNumber(std::pow(n, k));
}

void GcdFn(Call& c) {
  std::vector<double> v;
  if (!Whole(c, v)) return;
  double g = 0;
  for (double d : v) g = Gcd(g, d);
  c.SetNumber(g);
}
void Lcm(Call& c) {
  std::vector<double> v;
  if (!Whole(c, v)) return;
  double l = 1;
  for (double d : v) {
    if (d == 0) return c.SetNumber(0);
    l = l / Gcd(l, d) * d;
  }
  c.SetNumber(l);
}
void Multinomial(Call& c) {
  std::vector<double> v;
  if (!Whole(c, v)) return;
  double s = 0, r = 1;
  for (double d : v) {
    s += d;
    r *= Combin(s, d);
  }
  c.SetNumber(r);
}

void SeriesSum(Call& c) {
  double x, n, m;
  if (!c.Number(0, x) || !c.Number(1, n) || !c.Number(2, m)) return;
  Value coef;
  if (!ArgArray(c, 3, coef)) return;
  const Array& a = coef.array();
  double s = 0;
  for (int32_t k = 0; k < a.size(); ++k) {
    const Value& v = a.data()[k];
    if (v.IsError()) return c.Fail(v.error());
    if (!v.IsNumber()) return c.Fail(Err::Value);
    s += v.number() * std::pow(x, n + k * m);
  }
  c.SetNumber(s);
}

// ROMAN(number, [form]): form 0 classic to 4 simplified (LibreOffice's algorithm, as Excel).
void Roman(Call& c) {
  double x, form = 0;
  if (!c.Number(0, x)) return;
  if (c.Has(1)) {
    if (c.arg(1).IsBool()) form = c.arg(1).boolean() ? 0 : 4;
    else if (!c.Number(1, form)) return;
  }
  x = std::trunc(x), form = std::trunc(form);
  if (x < 0 || x > 3999 || form < 0 || form > 4) return c.Fail(Err::Value);
  static const char kChars[] = {'M', 'D', 'C', 'L', 'X', 'V', 'I'};
  static const int kValues[] = {1000, 500, 100, 50, 10, 5, 1};
  const int maxIndex = 6, mode = static_cast<int>(form);
  int val = static_cast<int>(x);
  std::string out;
  for (int i = 0; i <= maxIndex / 2; ++i) {
    int index = 2 * i, digit = val / kValues[index];
    if (digit % 5 == 4) {
      int index2 = digit == 4 ? index - 1 : index - 2, steps = 0;
      while (steps < mode && index < maxIndex) {
        ++steps;
        if (kValues[index2] - kValues[index + 1] <= val) ++index;
        else steps = mode;
      }
      out += kChars[index];
      out += kChars[index2];
      val = val + kValues[index] - kValues[index2];
    } else {
      if (digit > 4) out += kChars[index - 1];
      out.append(static_cast<size_t>(digit % 5), kChars[index]);
      val %= kValues[index];
    }
  }
  c.out = Value::Text(out);
}

void Arabic(Call& c) {
  std::string t;
  if (!c.Text(0, t)) return;
  size_t a = t.find_first_not_of(' '), b = t.find_last_not_of(' ');
  t = a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
  if (t.size() > 255) return c.Fail(Err::Value);
  bool neg = !t.empty() && t[0] == '-';
  if (neg) t.erase(0, 1);
  auto value = [](char ch) {
    switch (ch >= 'a' ? ch - 32 : ch) {
      case 'I': return 1;
      case 'V': return 5;
      case 'X': return 10;
      case 'L': return 50;
      case 'C': return 100;
      case 'D': return 500;
      case 'M': return 1000;
      default: return 0;
    }
  };
  double sum = 0;
  for (size_t i = 0; i < t.size(); ++i) {
    int v = value(t[i]);
    if (!v) return c.Fail(Err::Value);
    int next = i + 1 < t.size() ? value(t[i + 1]) : 0;
    sum += next > v ? -v : v;
  }
  c.SetNumber(neg ? -sum : sum);
}

void Base(Call& c) {
  double x, radix, len;
  if (!c.Number(0, x) || !c.Number(1, radix) || !c.Number(2, len, 0)) return;
  x = std::trunc(x), radix = std::trunc(radix), len = std::trunc(len);
  if (x < 0 || x >= kMaxExact || radix < 2 || radix > 36 || len < 0 || len > 255) return c.Fail(Err::Num);
  std::string out;
  int r = static_cast<int>(radix);
  do {
    int d = static_cast<int>(std::fmod(x, r));
    out.insert(out.begin(), static_cast<char>(d < 10 ? '0' + d : 'A' + d - 10));
    x = std::floor(x / r);
  } while (x > 0);
  if (out.size() < len) out.insert(0, static_cast<size_t>(len) - out.size(), '0');
  c.out = Value::Text(out);
}

void Decimal(Call& c) {
  std::string t;
  double radix;
  if (!c.Text(0, t) || !c.Number(1, radix)) return;
  radix = std::trunc(radix);
  if (radix < 2 || radix > 36 || t.size() > 255) return c.Fail(Err::Num);
  double v = 0;
  for (char ch : t) {
    int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'z' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'Z' ? ch - 'A' + 10 : 99;
    if (d >= radix) return c.Fail(Err::Num);
    v = v * radix + d;
  }
  if (v >= kMaxExact) return c.Fail(Err::Num);
  c.SetNumber(v);
}

static void SumPairs(Call& c, int kind) {
  std::vector<double> x, y;
  if (!Pairs(c, x, y)) return;
  double s = 0;
  for (size_t i = 0; i < x.size(); ++i)
    s += kind == 0 ? x[i] * x[i] - y[i] * y[i] : kind == 1 ? x[i] * x[i] + y[i] * y[i] : (x[i] - y[i]) * (x[i] - y[i]);
  c.SetNumber(s);
}
void SumX2mY2(Call& c) { SumPairs(c, 0); }
void SumX2pY2(Call& c) { SumPairs(c, 1); }
void SumXmY2(Call& c) { SumPairs(c, 2); }

void MDeterm(Call& c) {
  Value keep;
  int32_t rows, cols;
  std::vector<double> m;
  if (!Matrix(c, 0, keep, rows, cols, m)) return;
  if (rows != cols) return c.Fail(Err::Value);
  std::vector<int> perm;
  double sign;
  if (!Lu(m, rows, perm, sign)) return c.SetNumber(0);
  double d = sign;
  for (int i = 0; i < rows; ++i) d *= m[static_cast<size_t>(i) * rows + i];
  c.SetNumber(d);
}

void MInverse(Call& c) {
  Value keep;
  int32_t rows, cols;
  std::vector<double> m;
  if (!Matrix(c, 0, keep, rows, cols, m)) return;
  if (rows != cols) return c.Fail(Err::Value);
  int n = rows;
  std::vector<int> perm;
  double sign;
  if (!Lu(m, n, perm, sign)) return c.Fail(Err::Num);
  Array* out = Array::Make(n, n);
  std::vector<double> col(static_cast<size_t>(n));
  for (int j = 0; j < n; ++j) {
    for (int i = 0; i < n; ++i) col[static_cast<size_t>(i)] = perm[static_cast<size_t>(i)] == j ? 1 : 0;
    for (int i = 0; i < n; ++i)  // forward: L y = P e
      for (int k = 0; k < i; ++k) col[static_cast<size_t>(i)] -= m[static_cast<size_t>(i) * n + k] * col[static_cast<size_t>(k)];
    for (int i = n - 1; i >= 0; --i) {  // back: U x = y
      for (int k = i + 1; k < n; ++k) col[static_cast<size_t>(i)] -= m[static_cast<size_t>(i) * n + k] * col[static_cast<size_t>(k)];
      col[static_cast<size_t>(i)] /= m[static_cast<size_t>(i) * n + i];
    }
    for (int i = 0; i < n; ++i) out->at(i, j) = Value::Number(col[static_cast<size_t>(i)]);
  }
  c.out = Value::Adopt(out);
}

void MMult(Call& c) {
  Value ka, kb;
  int32_t ar, ac, br, bc;
  std::vector<double> a, b;
  if (!Matrix(c, 0, ka, ar, ac, a) || !Matrix(c, 1, kb, br, bc, b)) return;
  if (ac != br) return c.Fail(Err::Value);
  Array* out = Array::Make(ar, bc);
  for (int32_t i = 0; i < ar; ++i)
    for (int32_t j = 0; j < bc; ++j) {
      double s = 0;
      for (int32_t k = 0; k < ac; ++k) s += a[static_cast<size_t>(i) * ac + k] * b[static_cast<size_t>(k) * bc + j];
      out->at(i, j) = Value::Number(s);
    }
  c.out = Value::Adopt(out);
}

void MUnit(Call& c) {
  double n;
  if (!c.Integer(0, n)) return;
  if (n < 1 || n > 16384) return c.Fail(Err::Value);
  int32_t k = static_cast<int32_t>(n);
  Array* out = Array::Make(k, k);
  for (int32_t i = 0; i < k; ++i)
    for (int32_t j = 0; j < k; ++j) out->at(i, j) = Value::Number(i == j ? 1 : 0);
  c.out = Value::Adopt(out);
}

}  // namespace fw::sheet::calc::fn
