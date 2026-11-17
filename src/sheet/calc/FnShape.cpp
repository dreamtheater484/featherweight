// Excel 365's array shaping functions: VSTACK, HSTACK, TAKE, DROP, CHOOSEROWS, CHOOSECOLS, TOCOL,
// TOROW, WRAPROWS, WRAPCOLS, EXPAND, and RANDARRAY. Excel 2021 lacks them, so they follow
// Microsoft's documentation (tests/unit/test_sheet_functions.cpp has its examples).

#include <algorithm>
#include <cmath>
#include <vector>

#include "sheet/calc/Fn.h"

namespace fw::sheet::calc::fn {
namespace {

constexpr int64_t kMaxCells = 16'000'000;

// A whole-number argument (truncated), or `def` when it's missing.
bool Count(Call& c, int i, double& out, double def) {
  if (!c.Has(i)) {
    out = def;
    return true;
  }
  if (!c.Integer(i, out)) return false;
  return true;
}

Value Made(Array* a) { return Value::Adopt(a); }

// The value pad arguments give: #N/A when missing.
Value Pad(Call& c, int i) {
  if (!c.Has(i)) return Value::Error(Err::NA);
  Value v = c.arg(i);
  if (v.IsArray()) v = Value(v.array().at(0, 0));
  return v;
}

void Stack(Call& c, bool vertical) {
  std::vector<Value> parts(static_cast<size_t>(c.argc));
  int64_t along = 0;
  int32_t across = 0;
  for (int i = 0; i < c.argc; ++i) {
    if (!ArgArray(c, i, parts[static_cast<size_t>(i)])) return;
    const Array& a = parts[static_cast<size_t>(i)].array();
    along += vertical ? a.rows() : a.cols();
    across = std::max(across, vertical ? a.cols() : a.rows());
  }
  if (along * across > kMaxCells || along > (vertical ? 1048576 : 16384)) return c.Fail(Err::Num);
  int32_t n = static_cast<int32_t>(along);
  Array* out = vertical ? Array::Make(n, across) : Array::Make(across, n);
  int32_t at = 0;
  for (const Value& p : parts) {
    const Array& a = p.array();
    int32_t len = vertical ? a.rows() : a.cols();
    for (int32_t i = 0; i < len; ++i)
      for (int32_t j = 0; j < across; ++j) {
        bool inside = j < (vertical ? a.cols() : a.rows());
        Value v = !inside ? Value::Error(Err::NA) : vertical ? a.at(i, j) : a.at(j, i);
        if (vertical) out->at(at + i, j) = std::move(v);
        else out->at(j, at + i) = std::move(v);
      }
    at += len;
  }
  c.out = Made(out);
}

// Rows [r0, r0 + nr) and columns [c0, c0 + nc) of `a`.
Value Slice(const Array& a, int32_t r0, int32_t nr, int32_t c0, int32_t nc) {
  Array* out = Array::Make(nr, nc);
  for (int32_t i = 0; i < nr; ++i)
    for (int32_t j = 0; j < nc; ++j) out->at(i, j) = a.at(r0 + i, c0 + j);
  return Made(out);
}

void TakeDrop(Call& c, bool take) {
  Value arr;
  if (!ArgArray(c, 0, arr)) return;
  const Array& a = arr.array();
  double rows, cols;
  if (!Count(c, 1, rows, take ? a.rows() : 0) || !Count(c, 2, cols, take ? a.cols() : 0)) return;
  auto range = [&](double k, int32_t size, int32_t& first, int32_t& n) {
    double m = std::min(std::fabs(k), static_cast<double>(size));
    int32_t len = static_cast<int32_t>(m);
    if (take) {
      n = len;
      first = k >= 0 ? 0 : size - len;
    } else {
      n = size - len;
      first = k >= 0 ? len : 0;
    }
  };
  int32_t r0, nr, c0, nc;
  range(rows, a.rows(), r0, nr);
  range(cols, a.cols(), c0, nc);
  if (nr == 0 || nc == 0) return c.Fail(Err::Calc);
  c.out = Slice(a, r0, nr, c0, nc);
}

// CHOOSEROWS / CHOOSECOLS: indexes from 1, negative from the end.
void Choose(Call& c, bool rows) {
  Value arr;
  if (!ArgArray(c, 0, arr)) return;
  const Array& a = arr.array();
  int32_t size = rows ? a.rows() : a.cols();
  std::vector<int32_t> pick;
  for (int i = 1; i < c.argc; ++i) {
    Value list;
    if (!ArgArray(c, i, list)) return;
    const Array& l = list.array();
    for (int32_t k = 0; k < l.size(); ++k) {
      double d = 0;
      Err e = Err::None;
      if (!ToNumber(c.m, l.data()[k], d, e)) return c.Fail(e);
      d = std::trunc(d);
      if (d == 0 || std::fabs(d) > size) return c.Fail(Err::Value);
      pick.push_back(static_cast<int32_t>(d > 0 ? d - 1 : size + d));
    }
  }
  int32_t n = static_cast<int32_t>(pick.size());
  Array* out = rows ? Array::Make(n, a.cols()) : Array::Make(a.rows(), n);
  for (int32_t k = 0; k < n; ++k)
    if (rows)
      for (int32_t j = 0; j < a.cols(); ++j) out->at(k, j) = a.at(pick[static_cast<size_t>(k)], j);
    else
      for (int32_t i = 0; i < a.rows(); ++i) out->at(i, k) = a.at(i, pick[static_cast<size_t>(k)]);
  c.out = Made(out);
}

// TOCOL / TOROW(array, [ignore], [scan_by_column]): ignore 1 blanks, 2 errors, 3 both.
void ToLine(Call& c, bool column) {
  Value arr;
  if (!ArgArray(c, 0, arr)) return;
  const Array& a = arr.array();
  double ignore;
  bool byCol;
  if (!Count(c, 1, ignore, 0) || !c.Boolean(2, byCol)) return;
  if (ignore < 0 || ignore > 3) return c.Fail(Err::Value);
  int mode = static_cast<int>(ignore);
  std::vector<const Value*> keep;
  int32_t outer = byCol ? a.cols() : a.rows(), inner = byCol ? a.rows() : a.cols();
  for (int32_t i = 0; i < outer; ++i)
    for (int32_t j = 0; j < inner; ++j) {
      const Value& v = byCol ? a.at(j, i) : a.at(i, j);
      if ((mode & 1) && v.IsEmpty()) continue;
      if ((mode & 2) && v.IsError()) continue;
      keep.push_back(&v);
    }
  if (keep.empty()) return c.Fail(Err::Calc);
  int32_t n = static_cast<int32_t>(keep.size());
  Array* out = column ? Array::Make(n, 1) : Array::Make(1, n);
  for (int32_t k = 0; k < n; ++k) out->data()[k] = *keep[static_cast<size_t>(k)];
  c.out = Made(out);
}

// WRAPROWS / WRAPCOLS(vector, wrap_count, [pad_with]).
void Wrap(Call& c, bool rows) {
  Value arr;
  if (!ArgArray(c, 0, arr)) return;
  const Array& a = arr.array();
  if (a.rows() != 1 && a.cols() != 1) return c.Fail(Err::Value);
  double wrap;
  if (!c.Integer(1, wrap)) return;
  if (wrap < 1) return c.Fail(Err::Num);
  int32_t n = a.size();
  int32_t w = static_cast<int32_t>(std::min(wrap, static_cast<double>(std::max(n, 1))));
  int32_t lines = (n + w - 1) / w;
  Value pad = Pad(c, 2);
  Array* out = rows ? Array::Make(lines, w) : Array::Make(w, lines);
  for (int32_t k = 0; k < lines * w; ++k) {
    Value v = k < n ? a.data()[k] : pad;
    if (rows) out->at(k / w, k % w) = std::move(v);
    else out->at(k % w, k / w) = std::move(v);
  }
  c.out = Made(out);
}

}  // namespace

void VStack(Call& c) { Stack(c, true); }
void HStack(Call& c) { Stack(c, false); }
void Take(Call& c) { TakeDrop(c, true); }
void Drop(Call& c) { TakeDrop(c, false); }
void ChooseRows(Call& c) { Choose(c, true); }
void ChooseCols(Call& c) { Choose(c, false); }
void ToCol(Call& c) { ToLine(c, true); }
void ToRow(Call& c) { ToLine(c, false); }
void WrapRows(Call& c) { Wrap(c, true); }
void WrapCols(Call& c) { Wrap(c, false); }

// EXPAND(array, rows, [columns], [pad_with]): at least the array's size.
void Expand(Call& c) {
  Value arr;
  if (!ArgArray(c, 0, arr)) return;
  const Array& a = arr.array();
  double rows, cols;
  if (!Count(c, 1, rows, a.rows()) || !Count(c, 2, cols, a.cols())) return;
  if (rows < a.rows() || cols < a.cols()) return c.Fail(Err::Value);
  if (rows > 1048576 || cols > 16384 || rows * cols > static_cast<double>(kMaxCells)) return c.Fail(Err::Num);
  Value pad = Pad(c, 3);
  int32_t nr = static_cast<int32_t>(rows), nc = static_cast<int32_t>(cols);
  Array* out = Array::Make(nr, nc);
  for (int32_t i = 0; i < nr; ++i)
    for (int32_t j = 0; j < nc; ++j) out->at(i, j) = i < a.rows() && j < a.cols() ? a.at(i, j) : pad;
  c.out = Made(out);
}

// RANDARRAY([rows], [columns], [min], [max], [whole_number]).
void RandArray(Call& c) {
  double rows, cols, lo, hi;
  bool whole;
  if (!Count(c, 0, rows, 1) || !Count(c, 1, cols, 1) || !c.Number(2, lo, 0) || !c.Number(3, hi, 1) ||
      !c.Boolean(4, whole))
    return;
  if (rows < 0 || cols < 0 || lo > hi) return c.Fail(Err::Value);
  if (rows == 0 || cols == 0) return c.Fail(Err::Calc);
  if (rows > 1048576 || cols > 16384 || rows * cols > static_cast<double>(kMaxCells)) return c.Fail(Err::Num);
  if (whole && (lo != std::floor(lo) || hi != std::floor(hi))) return c.Fail(Err::Value);
  int32_t nr = static_cast<int32_t>(rows), nc = static_cast<int32_t>(cols);
  Array* out = Array::Make(nr, nc);
  for (int32_t k = 0; k < nr * nc; ++k) {
    double r = c.m.host.Random();
    out->data()[k] = Value::Number(whole ? lo + std::floor(r * (hi - lo + 1)) : lo + r * (hi - lo));
  }
  c.out = Made(out);
}

}  // namespace fw::sheet::calc::fn
