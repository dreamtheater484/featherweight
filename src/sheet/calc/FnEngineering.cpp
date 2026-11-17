// Database functions (DSUM ... DVARP: a table with a header row, a field, a criteria range) and
// engineering functions (base conversions, bit operations, DELTA, GESTEP, ERF, ERFC). Checked
// against Excel 2021 (tests/unit/make_sheet_calc_cases.py).

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "sheet/calc/Fn.h"

namespace fw::sheet::calc::fn {
namespace {

bool SameHeader(const CellView& a, const CellView& b) {
  if (a.type == Type::Empty || b.type == Type::Empty) return false;
  return CompareValues(a, b) == 0;
}

// The values of the database's field in the records the criteria select. Field missing: -1 (each
// record counts as itself).
bool Records(Call& c, std::vector<CellView>& out, bool fieldOptional = false) {
  Grid db, cr;
  if (!MakeGrid(c.m, c.arg(0), db) || !db.ref || !MakeGrid(c.m, c.arg(2), cr) || !cr.ref)
    return c.Fail(c.m.stop || !c.arg(0).IsError() ? Err::Value : c.arg(0).error()), false;
  if (db.rows < 1 || cr.rows < 1) return c.Fail(Err::Value), false;
  int32_t field = -1;
  if (c.Has(1)) {
    Value f = c.m.Deref(c.arg(1), false);
    if (f.IsError()) return c.Fail(f.error()), false;
    if (f.IsNumber()) {
      double k = std::trunc(f.number());
      if (k < 1 || k > db.cols) return c.Fail(Err::Value), false;
      field = static_cast<int32_t>(k) - 1;
    } else {
      CellView name = View(f);
      for (int32_t k = 0; k < db.cols && field < 0; ++k)
        if (SameHeader(db.At(0, k), name)) field = k;
      if (field < 0) return c.Fail(Err::Value), false;
    }
  } else if (!fieldOptional) {
    return c.Fail(Err::Value), false;
  }
  // Each criteria row: (database column, criteria) pairs; a row without any matches everything.
  std::vector<std::vector<std::pair<int32_t, Criteria>>> rows;
  for (int32_t r = 1; r < cr.rows; ++r) {
    std::vector<std::pair<int32_t, Criteria>> row;
    for (int32_t k = 0; k < cr.cols; ++k) {
      CellView v = cr.At(r, k);
      if (v.type == Type::Empty) continue;
      int32_t col = -1;
      for (int32_t j = 0; j < db.cols && col < 0; ++j)
        if (SameHeader(db.At(0, j), cr.At(0, k))) col = j;
      if (col < 0) {
        c.m.unsupported = true;  // a computed criterion (a formula under a new label)
        return c.Fail(Err::Value), false;
      }
      Criteria crit;
      Value cv = FromView(v);
      if (!MakeCriteria(c.m, cv, crit)) return false;
      // A bare text criterion matches what begins with it ("Ap" finds "Apple").
      if (cv.IsText() && crit.type == Type::Text && crit.op == Criteria::kEq && !cv.text().empty() &&
          cv.text()[0] != '=') {
        crit.text += "*";
        crit.wildcard = true;
      }
      row.emplace_back(col, std::move(crit));
    }
    rows.push_back(std::move(row));
  }
  for (int32_t r = 1; r < db.rows; ++r) {
    bool any = false;
    for (const auto& row : rows) {
      bool all = true;
      for (const auto& [col, crit] : row) all = all && crit.Match(db.At(r, col));
      any = any || all;
      if (any) break;
    }
    if (!any) continue;
    CellView v = field >= 0 ? db.At(r, field) : CellView{};
    if (field < 0) v.type = Type::Number, v.num = 1;
    out.push_back(v);
  }
  return true;
}

bool RecordNumbers(Call& c, std::vector<double>& v) {
  std::vector<CellView> recs;
  if (!Records(c, recs)) return false;
  for (const CellView& r : recs)
    if (r.type == Type::Number) v.push_back(r.num);
  return true;
}

void DVariance(Call& c, bool sample, bool root) {
  std::vector<double> v;
  if (!RecordNumbers(c, v)) return;
  if (v.size() < (sample ? 2u : 1u)) return c.Fail(Err::Div0);
  double s = SquaredDeviations(v) / static_cast<double>(v.size() - (sample ? 1 : 0));
  c.SetNumber(root ? std::sqrt(s) : s);
}

// --- bases ---

struct BaseInfo {
  int radix;
  double lo, hi;  // the range 10 digits hold in two's complement
};
const BaseInfo kBin{2, -512, 511}, kOct{8, -536870912, 536870911}, kHex{16, -549755813888.0, 549755813887.0};

// A number written in `from` (up to 10 digits; 10 with the top bit set are negative).
bool ParseBase(Call& c, const BaseInfo& from, double& out) {
  std::string t;
  if (!c.Text(0, t)) return false;
  if (t.size() > 10) return c.Fail(Err::Num), false;
  double v = 0;
  for (char ch : t) {
    int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : 99;
    if (d >= from.radix) return c.Fail(Err::Num), false;
    v = v * from.radix + d;
  }
  double full = std::pow(static_cast<double>(from.radix), 10);
  if (t.size() == 10 && v >= full / 2) v -= full;
  out = v;
  return true;
}

void FormatBase(Call& c, double v, const BaseInfo& to, int placesArg) {
  if (v < to.lo || v > to.hi) return c.Fail(Err::Num);
  double places = 0;
  bool hasPlaces = c.Has(placesArg);
  if (hasPlaces) {
    if (!c.Number(placesArg, places)) return;
    places = std::trunc(places);
    if (places < 0) return c.Fail(Err::Num);
    if (places > 10) return c.Fail(Err::Num);
  }
  if (v < 0) v += std::pow(static_cast<double>(to.radix), 10);  // ten digits, places ignored
  std::string s;
  do {
    int d = static_cast<int>(std::fmod(v, to.radix));
    s.insert(s.begin(), static_cast<char>(d < 10 ? '0' + d : 'A' + d - 10));
    v = std::floor(v / to.radix);
  } while (v > 0);
  if (hasPlaces && s.size() < 10) {
    if (s.size() > places) return c.Fail(Err::Num);
    s.insert(0, static_cast<size_t>(places) - s.size(), '0');
  }
  c.out = Value::Text(s);
}

void FromBase(Call& c, const BaseInfo& from, const BaseInfo* to) {
  double v;
  if (!ParseBase(c, from, v)) return;
  if (!to) return c.SetNumber(v);
  FormatBase(c, v, *to, 1);
}

void FromDecimal(Call& c, const BaseInfo& to) {
  double v;
  if (!c.Number(0, v)) return;
  FormatBase(c, std::trunc(v), to, 1);
}

constexpr double kBitLimit = 281474976710656.0;  // 2^48

bool Bits(Call& c, int i, double& v) {
  if (!c.Number(i, v)) return false;
  if (v < 0 || v != std::floor(v) || v >= kBitLimit) return c.Fail(Err::Num), false;
  return true;
}

void BitOp(Call& c, int op) {
  double a, b;
  if (!Bits(c, 0, a) || !Bits(c, 1, b)) return;
  uint64_t x = static_cast<uint64_t>(a), y = static_cast<uint64_t>(b);
  c.SetNumber(static_cast<double>(op == 0 ? x & y : op == 1 ? (x | y) : (x ^ y)));
}

void BitShift(Call& c, bool left) {
  double a, s;
  if (!Bits(c, 0, a) || !c.Number(1, s)) return;
  s = std::trunc(s);
  if (std::fabs(s) > 53) return c.Fail(Err::Num);
  if (!left) s = -s;
  double r = s >= 0 ? a * std::pow(2.0, s) : std::floor(a / std::pow(2.0, -s));
  if (r >= kBitLimit) return c.Fail(Err::Num);
  c.SetNumber(r);
}

}  // namespace

void DSum(Call& c) {
  std::vector<double> v;
  if (!RecordNumbers(c, v)) return;
  double s = 0;
  for (double d : v) s += d;
  c.SetNumber(s);
}
void DAverage(Call& c) {
  std::vector<double> v;
  if (!RecordNumbers(c, v)) return;
  if (v.empty()) return c.Fail(Err::Div0);
  c.SetNumber(Mean(v));
}
void DCount(Call& c) {
  std::vector<CellView> recs;
  if (!Records(c, recs, true)) return;
  double n = 0;
  for (const CellView& r : recs) n += r.type == Type::Number;
  c.SetNumber(n);
}
void DCountA(Call& c) {
  std::vector<CellView> recs;
  if (!Records(c, recs, true)) return;
  double n = 0;
  for (const CellView& r : recs) n += r.type != Type::Empty;
  c.SetNumber(n);
}
void DGet(Call& c) {
  std::vector<CellView> recs;
  if (!Records(c, recs)) return;
  if (recs.empty()) return c.Fail(Err::Value);
  if (recs.size() > 1) return c.Fail(Err::Num);
  c.out = recs[0].type == Type::Empty ? Value::Number(0) : FromView(recs[0]);
}
void DMax(Call& c) {
  std::vector<double> v;
  if (!RecordNumbers(c, v)) return;
  c.SetNumber(v.empty() ? 0 : *std::max_element(v.begin(), v.end()));
}
void DMin(Call& c) {
  std::vector<double> v;
  if (!RecordNumbers(c, v)) return;
  c.SetNumber(v.empty() ? 0 : *std::min_element(v.begin(), v.end()));
}
void DProduct(Call& c) {
  std::vector<double> v;
  if (!RecordNumbers(c, v)) return;
  double p = 1;
  for (double d : v) p *= d;
  c.SetNumber(v.empty() ? 0 : p);
}
void DStDev(Call& c) { DVariance(c, true, true); }
void DStDevP(Call& c) { DVariance(c, false, true); }
void DVar(Call& c) { DVariance(c, true, false); }
void DVarP(Call& c) { DVariance(c, false, false); }

void Bin2Dec(Call& c) { FromBase(c, kBin, nullptr); }
void Bin2Oct(Call& c) { FromBase(c, kBin, &kOct); }
void Bin2Hex(Call& c) { FromBase(c, kBin, &kHex); }
void Oct2Dec(Call& c) { FromBase(c, kOct, nullptr); }
void Oct2Bin(Call& c) { FromBase(c, kOct, &kBin); }
void Oct2Hex(Call& c) { FromBase(c, kOct, &kHex); }
void Hex2Dec(Call& c) { FromBase(c, kHex, nullptr); }
void Hex2Bin(Call& c) { FromBase(c, kHex, &kBin); }
void Hex2Oct(Call& c) { FromBase(c, kHex, &kOct); }
void Dec2Bin(Call& c) { FromDecimal(c, kBin); }
void Dec2Oct(Call& c) { FromDecimal(c, kOct); }
void Dec2Hex(Call& c) { FromDecimal(c, kHex); }

void BitAnd(Call& c) { BitOp(c, 0); }
void BitOr(Call& c) { BitOp(c, 1); }
void BitXor(Call& c) { BitOp(c, 2); }
void BitLShift(Call& c) { BitShift(c, true); }
void BitRShift(Call& c) { BitShift(c, false); }

void Delta(Call& c) {
  double a, b;
  if (c.Number(0, a) && c.Number(1, b, 0)) c.SetNumber(a == b ? 1 : 0);
}
void GeStep(Call& c) {
  double a, b;
  if (c.Number(0, a) && c.Number(1, b, 0)) c.SetNumber(a >= b ? 1 : 0);
}
void Erf(Call& c) {
  double a, b;
  if (!c.Number(0, a)) return;
  if (!c.Has(1)) return c.SetNumber(std::erf(a));
  if (!c.Number(1, b)) return;
  c.SetNumber(std::erf(b) - std::erf(a));
}
void ErfPrecise(Call& c) {
  double a;
  if (c.Number(0, a)) c.SetNumber(std::erf(a));
}
void Erfc(Call& c) {
  double a;
  if (c.Number(0, a)) c.SetNumber(std::erfc(a));
}

}  // namespace fw::sheet::calc::fn
