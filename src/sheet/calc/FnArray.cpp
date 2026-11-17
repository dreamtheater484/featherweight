// The dynamic-array functions: SEQUENCE, FILTER, UNIQUE, SORT, SORTBY. Their results are arrays, which a
// spilling formula shows across its range. Behavior checked against Excel 2021 (see the cases in
// tests/unit/make_sheet_calc_cases.py).

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "sheet/calc/Fn.h"

namespace fw::sheet::calc::fn {
namespace {

constexpr int32_t kMaxRowsArg = 1048576, kMaxColsArg = 16384;
constexpr int64_t kMaxCells = 16'000'000;  // as Machine::RefArray

// An argument as an array: a reference's values (whole columns only as far as the workbook's cells
// reach), a single value as 1 x 1. False (with `c.out` set) when it can't be read.
bool AsArray(Call& c, int i, Value& out) {
  Value v = c.arg(i);
  if (v.type() == Type::RefList) return c.Fail(Err::Value), false;
  if (v.IsRef()) {
    const Area& area = v.area();
    if (area.s1 != area.s2) return c.Fail(Err::Value), false;
    bool single = area.r1 == area.r2 && area.c1 == area.c2;
    v = c.m.RefArray(v);
    if (c.m.stop) return c.Fail(Err::Value), false;
    if (!single && v.IsError()) return c.Fail(v.error()), false;  // too large
  }
  if (v.IsMissing()) v = Value();
  if (!v.IsArray()) {
    Array* a = Array::Make(1, 1);
    a->at(0, 0) = std::move(v);
    v = Value::Adopt(a);
  }
  out = std::move(v);
  return true;
}

// The value shown for an element: blanks in results show as 0 only when spilled (Engine::Store).
const Value& Elem(const Array& a, int32_t r, int32_t c, bool byCol) { return byCol ? a.at(c, r) : a.at(r, c); }

// Numbers from an argument (SORT's indexes and orders): each element converted as a number.
bool NumberList(Call& c, int i, std::vector<double>& out) {
  Value v;
  if (!AsArray(c, i, v)) return false;
  const Array& a = v.array();
  for (int32_t r = 0; r < a.rows(); ++r)
    for (int32_t k = 0; k < a.cols(); ++k) {
      double d = 0;
      Err e = Err::None;
      if (!ToNumber(c.m, a.at(r, k), d, e)) return c.Fail(e), false;
      out.push_back(d);
    }
  return true;
}

// The type order SORT uses, ascending: numbers, text, logicals, errors; blanks always last.
int Rank(Type t) {
  switch (t) {
    case Type::Number: return 0;
    case Type::Text: return 1;
    case Type::Bool: return 2;
    case Type::Error: return 3;
    default: return 4;
  }
}

int CompareForSort(const Value& a, const Value& b, bool descending) {
  int ra = Rank(a.type()), rb = Rank(b.type());
  if (ra == 4 || rb == 4) return ra == rb ? 0 : (ra == 4 ? 1 : -1);  // blanks last either way
  int r = 0;
  if (ra != rb) {
    r = ra < rb ? -1 : 1;
  } else {
    switch (a.type()) {
      case Type::Number: r = a.number() < b.number() ? -1 : a.number() > b.number() ? 1 : 0; break;
      case Type::Text: r = CompareText(a.text(), b.text()); break;
      case Type::Bool: r = (a.boolean() ? 1 : 0) - (b.boolean() ? 1 : 0); break;
      default: r = 0; break;
    }
  }
  return descending ? -r : r;
}

// A key that is equal for values UNIQUE sees as the same: numbers to 15 digits, text without
// case (linguistically, as Excel compares text), the same type otherwise.
void AppendKey(const Value& v, bool linguistic, std::string& key) {
  switch (v.type()) {
    case Type::Number: {
      double d = Round15(v.number());
      if (d == 0) d = 0;  // -0
      key += 'n';
      key.append(reinterpret_cast<const char*>(&d), sizeof d);
      break;
    }
    case Type::Text: {
      std::string k = TextKey(v.text(), linguistic);
      uint32_t n = static_cast<uint32_t>(k.size());
      key += 's';
      key.append(reinterpret_cast<const char*>(&n), sizeof n);
      key += k;
      break;
    }
    case Type::Bool: key += v.boolean() ? 'T' : 'F'; break;
    case Type::Error: key += 'e', key += static_cast<char>(v.error()); break;
    default: key += 'b'; break;
  }
}

bool PlainText(const Array& a) {
  for (int32_t r = 0; r < a.rows(); ++r)
    for (int32_t k = 0; k < a.cols(); ++k) {
      const Value& v = a.at(r, k);
      if (!v.IsText()) continue;
      for (char ch : v.text())
        if (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) >= 0x7F) return false;
    }
  return true;
}

// A result of `lines` rows (or columns when `byCol`) of `width` taken from `src`.
Value Pick(const Array& src, const std::vector<int32_t>& lines, bool byCol) {
  int32_t width = byCol ? src.rows() : src.cols();
  int32_t n = static_cast<int32_t>(lines.size());
  Array* out = byCol ? Array::Make(width, n) : Array::Make(n, width);
  for (int32_t i = 0; i < n; ++i)
    for (int32_t k = 0; k < width; ++k) {
      if (byCol) out->at(k, i) = src.at(k, lines[static_cast<size_t>(i)]);
      else out->at(i, k) = src.at(lines[static_cast<size_t>(i)], k);
    }
  return Value::Adopt(out);
}

}  // namespace

// SEQUENCE(rows, [columns], [start], [step]).
void Sequence(Call& c) {
  double rows, cols, start, step;
  if (!c.Number(0, rows) || !c.Number(1, cols, 1) || !c.Number(2, start, 1) || !c.Number(3, step, 1)) return;
  if (rows < 0 || cols < 0) return c.Fail(Err::Value);
  rows = std::trunc(rows), cols = std::trunc(cols);
  if (rows == 0 || cols == 0) return c.Fail(Err::Calc);
  if (rows > kMaxRowsArg || cols > kMaxColsArg) return c.Fail(Err::Value);
  if (rows * cols > static_cast<double>(kMaxCells)) return c.Fail(Err::Num);
  int32_t nr = static_cast<int32_t>(rows), nc = static_cast<int32_t>(cols);
  Array* a = Array::Make(nr, nc);
  for (int32_t i = 0; i < nr; ++i)
    for (int32_t j = 0; j < nc; ++j) a->at(i, j) = Value::Number(start + step * (static_cast<double>(i) * nc + j));
  c.out = Value::Adopt(a);
}

// FILTER(array, include, [if_empty]): the rows (or columns) whose include value is true. Include
// is one column as tall as the array, or one row as wide; its values are numbers or logicals.
void Filter(Call& c) {
  Value arr, inc;
  if (!AsArray(c, 0, arr) || !AsArray(c, 1, inc)) return;
  const Array& a = arr.array();
  const Array& f = inc.array();
  bool byCol;
  if (f.cols() == 1 && f.rows() == a.rows()) byCol = false;
  else if (f.rows() == 1 && f.cols() == a.cols()) byCol = true;
  else return c.Fail(Err::Value);
  int32_t n = byCol ? f.cols() : f.rows();
  std::vector<int32_t> keep;
  for (int32_t i = 0; i < n; ++i) {
    const Value& v = byCol ? f.at(0, i) : f.at(i, 0);
    switch (v.type()) {
      case Type::Error: return c.Fail(v.error());
      case Type::Text: return c.Fail(Err::Value);
      case Type::Number:
      case Type::Bool:
        if (v.number() != 0) keep.push_back(i);
        break;
      default: break;
    }
  }
  if (keep.empty()) {
    if (c.Has(2)) c.out = c.arg(2);
    else c.Fail(Err::Calc);
    return;
  }
  c.out = Pick(a, keep, byCol);
}

// UNIQUE(array, [by_col], [exactly_once]).
void Unique(Call& c) {
  Value arr;
  bool byCol, once;
  if (!AsArray(c, 0, arr) || !c.Boolean(1, byCol) || !c.Boolean(2, once)) return;
  const Array& a = arr.array();
  int32_t n = byCol ? a.cols() : a.rows(), width = byCol ? a.rows() : a.cols();
  bool linguistic = !PlainText(a);
  struct Seen {
    int32_t first;
    int32_t count;
  };
  std::unordered_map<std::string, Seen> seen;
  std::vector<std::string> keys(static_cast<size_t>(n));
  std::vector<int32_t> order;
  for (int32_t i = 0; i < n; ++i) {
    std::string& key = keys[static_cast<size_t>(i)];
    for (int32_t k = 0; k < width; ++k) AppendKey(Elem(a, i, k, byCol), linguistic, key);
    auto [it, added] = seen.try_emplace(key, Seen{i, 0});
    if (added) order.push_back(i);
    ++it->second.count;
  }
  std::vector<int32_t> keep;
  for (int32_t i : order)
    if (!once || seen[keys[static_cast<size_t>(i)]].count == 1) keep.push_back(i);
  if (keep.empty()) return c.Fail(Err::Calc);
  c.out = Pick(a, keep, byCol);
}

// SORT(array, [sort_index], [sort_order], [by_col]): a stable sort by one or more rows/columns.
void Sort(Call& c) {
  Value arr;
  bool byCol;
  if (!AsArray(c, 0, arr) || !c.Boolean(3, byCol)) return;
  const Array& a = arr.array();
  std::vector<double> index, order;
  if (c.Has(1)) {
    if (!NumberList(c, 1, index)) return;
  } else {
    index.push_back(1);
  }
  if (c.Has(2)) {
    if (!NumberList(c, 2, order)) return;
  } else {
    order.push_back(1);
  }
  if (order.size() != 1 && order.size() != index.size()) return c.Fail(Err::Value);
  int32_t n = byCol ? a.cols() : a.rows(), width = byCol ? a.rows() : a.cols();
  struct Key {
    int32_t line;
    bool descending;
  };
  std::vector<Key> keys;
  for (size_t k = 0; k < index.size(); ++k) {
    double i = std::trunc(index[k]);
    double o = order[order.size() == 1 ? 0 : k];
    if (i < 1 || i > width || (o != 1 && o != -1)) return c.Fail(Err::Value);
    keys.push_back({static_cast<int32_t>(i) - 1, o == -1});
  }
  std::vector<int32_t> lines(static_cast<size_t>(n));
  for (int32_t i = 0; i < n; ++i) lines[static_cast<size_t>(i)] = i;
  std::stable_sort(lines.begin(), lines.end(), [&](int32_t x, int32_t y) {
    for (const Key& k : keys) {
      int r = CompareForSort(Elem(a, x, k.line, byCol), Elem(a, y, k.line, byCol), k.descending);
      if (r) return r < 0;
    }
    return false;
  });
  c.out = Pick(a, lines, byCol);
}

// SORTBY(array, by1, [order1], by2, [order2], ...): the array's rows in the order of the by-arrays,
// each one column as tall as the array (or its columns, by rows as wide as it; all one way).
void SortBy(Call& c) {
  Value arr;
  if (!AsArray(c, 0, arr)) return;
  const Array& a = arr.array();
  struct Key {
    Value by;
    bool descending;
  };
  std::vector<Key> keys;
  int dir = 0;  // 1: by columns (sorts rows), 2: by rows (sorts columns)
  for (int i = 1; i < c.argc; i += 2) {
    if (!c.Has(i)) return c.Fail(Err::Value);
    Key k;
    if (!AsArray(c, i, k.by)) return;
    double o = 1;
    if (i + 1 < c.argc && !c.Number(i + 1, o, 1)) return;
    if (o != 1 && o != -1) return c.Fail(Err::Value);
    k.descending = o == -1;
    const Array& b = k.by.array();
    int d = b.cols() == 1 && b.rows() == a.rows() ? 1 : b.rows() == 1 && b.cols() == a.cols() ? 2 : 0;
    if (d == 1 && dir == 2 && b.rows() == 1 && b.cols() == a.cols()) d = 2;  // 1 x 1 goes either way
    if (d == 0 || (dir && d != dir)) return c.Fail(Err::Value);
    dir = d;
    keys.push_back(std::move(k));
  }
  const bool byCol = dir == 2;
  int32_t n = byCol ? a.cols() : a.rows();
  std::vector<int32_t> lines(static_cast<size_t>(n));
  for (int32_t i = 0; i < n; ++i) lines[static_cast<size_t>(i)] = i;
  std::stable_sort(lines.begin(), lines.end(), [&](int32_t x, int32_t y) {
    for (const Key& k : keys) {
      const Array& b = k.by.array();
      int r = CompareForSort(b.data()[x], b.data()[y], k.descending);
      if (r) return r < 0;
    }
    return false;
  });
  c.out = Pick(a, lines, byCol);
}

}  // namespace fw::sheet::calc::fn
