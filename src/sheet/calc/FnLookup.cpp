// Lookup and reference functions, and the conditional aggregates (SUMIF and friends).

#include <algorithm>
#include <cmath>
#include <vector>

#include "pkg/Formula.h"
#include "sheet/calc/Entry.h"
#include "sheet/calc/Fn.h"

namespace fw::sheet::calc::fn {
namespace {

bool SameType(const CellView& a, const CellView& b) { return a.type == b.type; }

bool ExactMatch(const CellView& v, const CellView& key, bool wildcards) {
  if (!SameType(v, key)) return false;
  switch (key.type) {
    case Type::Number: return EqualNumbers(v.num, key.num);
    case Type::Bool: return v.num == key.num;
    case Type::Text:
      if (wildcards && key.text.find_first_of("*?~") != std::string_view::npos) return WildcardMatch(key.text, v.text);
      return EqualText(v.text, key.text);
    case Type::Error: return v.err == key.err;
    default: return false;
  }
}

// Ordering for the sorted searches: like comparison operators, but blanks sort last.
int Order(const CellView& v, const CellView& key) {
  if (v.type == Type::Empty) return 1;
  return CompareValues(v, key);
}

// Position of `key` among n cells, or -1. mode 0: exact (with wildcards when `wild`), 1: the
// largest value <= key in ascending data (binary search), -1: the smallest >= key in descending.
template <class Get>
int32_t Search(const CellView& key, int32_t n, Get get, int mode, bool wild) {
  if (mode == 0) {
    for (int32_t i = 0; i < n; ++i)
      if (ExactMatch(get(i), key, wild)) return i;
    return -1;
  }
  int32_t lo = 0, hi = n - 1, found = -1;
  while (lo <= hi) {
    int32_t mid = lo + (hi - lo) / 2;
    CellView v = get(mid);
    int c = Order(v, key);
    bool sameKind = v.type == key.type || (v.type == Type::Empty);
    if (mode == 1) {
      if (c <= 0) {
        if (sameKind && v.type != Type::Empty) found = mid;
        lo = mid + 1;
      } else {
        hi = mid - 1;
      }
    } else {
      if (c >= 0 && v.type != Type::Empty) {
        if (v.type == key.type) found = mid;
        lo = mid + 1;
      } else {
        hi = mid - 1;
      }
    }
  }
  // Excel steps over runs of equal values to the last one.
  return found;
}

bool KeyView(Call& c, int i, CellView& key) {
  const Value& v = c.arg(i);
  if (v.IsError()) {
    c.Fail(v.error());
    return false;
  }
  key = View(v);
  if (key.type == Type::Empty) key.type = Type::Number, key.num = 0;
  return true;
}

// A row or column of a reference or array as a value: a reference stays one (Ref of the slice).
Value Slice(Machine& m, const Value& src, const Grid& g, bool row, int32_t index) {
  if (g.ref) {
    Area a = g.area;
    if (row) a.r1 = a.r2 = a.r1 + index;
    else a.c1 = a.c2 = static_cast<int16_t>(a.c1 + index);
    return Value::Ref(a, src.dynamic());
  }
  (void)m;
  if (!src.IsArray()) return src;
  const Array& arr = src.array();
  int32_t rows = row ? 1 : arr.rows(), cols = row ? arr.cols() : 1;
  if (rows == 1 && cols == 1) return row ? arr.at(index, 0) : arr.at(0, index);
  Array* out = Array::Make(rows, cols);
  for (int32_t r = 0; r < rows; ++r)
    for (int32_t k = 0; k < cols; ++k) out->at(r, k) = row ? arr.at(index, k) : arr.at(r, index);
  return Value::Adopt(out);
}

void TableLookup(Call& c, bool vertical) {
  CellView key;
  if (!KeyView(c, 0, key)) return;
  Grid g;
  if (!MakeGrid(c.m, c.arg(1), g)) return c.Fail(Err::Value);
  double index;
  bool approx;
  if (!c.Integer(2, index) || !c.Boolean(3, approx, true)) return;
  int32_t span = vertical ? g.cols : g.rows;
  if (index < 1) return c.Fail(Err::Value);
  if (index > span) return c.Fail(Err::Ref);
  int32_t n = vertical ? g.dataRows : g.dataCols;
  auto get = [&](int32_t i) { return vertical ? g.At(i, 0) : g.At(0, i); };
  int32_t pos = Search(key, n, get, approx ? 1 : 0, true);
  if (pos < 0) return c.Fail(Err::NA);
  int32_t k = static_cast<int32_t>(index) - 1;
  c.out = FromView(vertical ? g.At(pos, k) : g.At(k, pos));
}

// Criteria pairs from argument `first` on: (range, criteria), (range, criteria)...
struct Conditions {
  std::vector<Value> held;
  std::vector<Grid> grids;
  std::vector<Criteria> crits;
  int32_t rows = 0, cols = 0;
  bool allBlank = true;  // every criterion matches a blank cell
};

bool MakeConditions(Call& c, int first, Conditions& k) {
  int pairs = (c.argc - first) / 2;
  if ((c.argc - first) % 2 != 0 || pairs < 1) {
    c.Fail(Err::Value);
    return false;
  }
  k.held.resize(static_cast<size_t>(pairs));
  k.grids.resize(static_cast<size_t>(pairs));
  k.crits.resize(static_cast<size_t>(pairs));
  for (int i = 0; i < pairs; ++i) {
    k.held[i] = c.arg(first + 2 * i);
    if (k.held[i].IsError()) return c.Fail(k.held[i].error()), false;
    if (!k.held[i].IsRef() && !k.held[i].IsArray()) return c.Fail(Err::Value), false;
    if (!MakeGrid(c.m, k.held[i], k.grids[i])) return c.Fail(Err::Value), false;
    const Value& crit = c.arg(first + 2 * i + 1);
    if (!MakeCriteria(c.m, crit, k.crits[i])) return c.Fail(Err::Value), false;
    if (i == 0) k.rows = k.grids[i].rows, k.cols = k.grids[i].cols;
    else if (k.grids[i].rows != k.rows || k.grids[i].cols != k.cols) return c.Fail(Err::Value), false;
    if (!k.crits[i].Match(CellView{})) k.allBlank = false;
  }
  return true;
}

// The cells to look at: where a criterion that blanks fail has data, else all data.
void Limits(const Conditions& k, const Grid* values, int32_t& rows, int32_t& cols) {
  if (!k.allBlank) {
    rows = k.rows, cols = k.cols;
    for (size_t i = 0; i < k.grids.size(); ++i)
      if (!k.crits[i].Match(CellView{})) rows = std::min(rows, k.grids[i].dataRows), cols = std::min(cols, k.grids[i].dataCols);
    return;
  }
  rows = cols = 0;
  for (const Grid& g : k.grids) rows = std::max(rows, g.dataRows), cols = std::max(cols, g.dataCols);
  if (values) rows = std::max(rows, values->dataRows), cols = std::max(cols, values->dataCols);
  rows = std::min(rows, k.rows), cols = std::min(cols, k.cols);
}

template <class F>
void EachMatch(const Conditions& k, int32_t rows, int32_t cols, F&& fn) {
  for (int32_t r = 0; r < rows; ++r)
    for (int32_t col = 0; col < cols; ++col) {
      bool all = true;
      for (size_t i = 0; i < k.grids.size() && all; ++i) all = k.crits[i].Match(k.grids[i].At(r, col));
      if (all && !fn(r, col)) return;
    }
}

// The cells holding the values: a reference given the criteria range's size from its top-left.
bool ValueGrid(Call& c, const Value& v, int32_t rows, int32_t cols, Value& held, Grid& g) {
  if (v.type() == Type::Ref) {
    Area a = v.area();
    a.r2 = a.r1 + rows - 1;
    a.c2 = static_cast<int16_t>(a.c1 + cols - 1);
    if (a.r2 >= kMaxRows || a.c2 >= kMaxCols) a.r2 = std::min(a.r2, kMaxRows - 1), a.c2 = static_cast<int16_t>(std::min<int32_t>(a.c2, kMaxCols - 1));
    held = Value::Ref(a, v.dynamic());
  } else if (v.IsArray()) {
    if (v.array().rows() != rows || v.array().cols() != cols) return c.Fail(Err::Value), false;
    held = v;
  } else {
    return c.Fail(Err::Value), false;
  }
  if (!MakeGrid(c.m, held, g)) return c.Fail(Err::Value), false;
  return true;
}

enum class Agg { kSum, kAverage, kMax, kMin };

// SUMIFS-style: values in argument `valuesArg`, criteria pairs from `first`.
void Aggregate(Call& c, int valuesArg, Conditions& k, Agg agg) {
  Value held;
  Grid g;
  if (!ValueGrid(c, c.arg(valuesArg), k.rows, k.cols, held, g)) return;
  int32_t rows, cols;
  Limits(k, &g, rows, cols);
  double sum = 0, n = 0, best = 0;
  bool any = false;
  Err err = Err::None;
  EachMatch(k, rows, cols, [&](int32_t r, int32_t col) {
    CellView v = g.At(r, col);
    if (v.type == Type::Error) {
      err = v.err;
      return false;
    }
    if (v.type != Type::Number) return true;
    sum += v.num, ++n;
    if (!any || (agg == Agg::kMax ? v.num > best : v.num < best)) best = v.num;
    any = true;
    return true;
  });
  if (err != Err::None) return c.Fail(err);
  switch (agg) {
    case Agg::kSum: return c.SetNumber(sum);
    case Agg::kAverage: return n ? c.SetNumber(sum / n) : c.Fail(Err::Div0);
    default: return c.SetNumber(any ? best : 0);
  }
}

}  // namespace

void VLookup(Call& c) { TableLookup(c, true); }
void HLookup(Call& c) { TableLookup(c, false); }

void Match(Call& c) {
  CellView key;
  if (!KeyView(c, 0, key)) return;
  Grid g;
  if (!MakeGrid(c.m, c.arg(1), g)) return c.Fail(Err::NA);
  if (g.rows != 1 && g.cols != 1) return c.Fail(Err::NA);
  double type;
  if (!c.Number(2, type, 1)) return;
  int mode = type > 0 ? 1 : type < 0 ? -1 : 0;
  bool vertical = g.cols == 1;
  int32_t n = vertical ? g.dataRows : g.dataCols;
  int32_t pos = Search(key, n, [&](int32_t i) { return vertical ? g.At(i, 0) : g.At(0, i); }, mode, true);
  if (pos < 0) return c.Fail(Err::NA);
  c.SetNumber(pos + 1);
}

void Lookup(Call& c) {
  CellView key;
  if (!KeyView(c, 0, key)) return;
  Grid g;
  if (!MakeGrid(c.m, c.arg(1), g)) return c.Fail(Err::NA);
  bool vertical = g.rows >= g.cols;  // the array form searches along the longer side
  int32_t n = vertical ? g.dataRows : g.dataCols;
  int32_t pos = Search(key, n, [&](int32_t i) { return vertical ? g.At(i, 0) : g.At(0, i); }, 1, false);
  if (pos < 0) return c.Fail(Err::NA);
  if (c.argc < 3) {
    c.out = FromView(vertical ? g.At(pos, g.cols - 1) : g.At(g.rows - 1, pos));
    return;
  }
  Grid r;
  if (!MakeGrid(c.m, c.arg(2), r)) return c.Fail(Err::Value);
  if (r.rows == 1 && r.ref) {
    Area a = r.area;
    if (a.c1 + pos >= kMaxCols) return c.Fail(Err::Ref);
    c.out = c.m.CellValue(a.s1, a.r1, a.c1 + pos);
  } else if (r.ref) {
    if (r.area.r1 + pos >= kMaxRows) return c.Fail(Err::Ref);
    c.out = c.m.CellValue(r.area.s1, r.area.r1 + pos, r.area.c1);
  } else {
    bool rv = r.rows >= r.cols;
    if (pos >= (rv ? r.rows : r.cols)) return c.Fail(Err::NA);
    c.out = FromView(rv ? r.At(pos, 0) : r.At(0, pos));
  }
}

static int32_t XFind(Call& c, const CellView& key, const Grid& g, bool vertical, int matchMode, int searchMode) {
  int32_t n = vertical ? g.rows : g.cols;
  int32_t data = vertical ? g.dataRows : g.dataCols;
  auto get = [&](int32_t i) { return vertical ? g.At(i, 0) : g.At(0, i); };
  if (searchMode == 2 || searchMode == -2) {
    if (matchMode == 2) return c.Fail(Err::Value), -2;
    // binary search, ascending (2) or descending (-2)
    int32_t lo = 0, hi = data - 1, exact = -1, below = -1, above = -1;
    while (lo <= hi) {
      int32_t mid = lo + (hi - lo) / 2;
      int cmp = CompareValues(get(mid), key);
      if (cmp == 0) {
        exact = mid;
        break;
      }
      bool goRight = searchMode == 2 ? cmp < 0 : cmp > 0;
      if (cmp < 0) below = mid;
      else above = mid;
      if (goRight) lo = mid + 1;
      else hi = mid - 1;
    }
    if (exact >= 0) return exact;
    if (matchMode == -1) return below;
    if (matchMode == 1) return above;
    return -1;
  }
  int32_t best = -1;
  CellView bestView;
  bool reverse = searchMode == -1;
  for (int32_t k = 0; k < data; ++k) {
    int32_t i = reverse ? data - 1 - k : k;
    CellView v = get(i);
    if (ExactMatch(v, key, matchMode == 2)) return i;
    if (matchMode == 0 || matchMode == 2 || v.type != key.type) continue;
    int cmp = CompareValues(v, key);
    bool candidate = matchMode == -1 ? cmp < 0 : cmp > 0;
    if (!candidate) continue;
    if (best < 0 || (matchMode == -1 ? CompareValues(v, bestView) > 0 : CompareValues(v, bestView) < 0)) {
      best = i;
      bestView = v;
    }
  }
  (void)n;
  // Blank keys can match the blank cells past the data.
  if (best < 0 && key.type == Type::Empty && data < n) return data;
  return best;
}

void XLookup(Call& c) {
  CellView key;
  if (c.arg(0).IsError()) return c.Fail(c.arg(0).error());
  key = View(c.arg(0));
  Grid g, r;
  if (!MakeGrid(c.m, c.arg(1), g) || !MakeGrid(c.m, c.arg(2), r)) return c.Fail(Err::Value);
  if (g.rows != 1 && g.cols != 1) return c.Fail(Err::Value);
  bool vertical = g.cols == 1 && !(g.rows == 1 && r.rows == 1 && r.cols != 1);
  if (vertical ? r.rows != g.rows : r.cols != g.cols) return c.Fail(Err::Value);
  double mm, sm;
  if (!c.Integer(4, mm) || !c.Integer(5, sm, 1)) return;
  if (mm < -1 || mm > 2 || !(sm == 1 || sm == -1 || sm == 2 || sm == -2)) return c.Fail(Err::Value);
  int32_t pos = XFind(c, key, g, vertical, static_cast<int>(mm), static_cast<int>(sm));
  if (pos == -2) return;
  if (pos < 0) {
    if (c.Has(3)) {
      c.out = c.arg(3);
      return;
    }
    return c.Fail(Err::NA);
  }
  c.out = Slice(c.m, c.arg(2), r, vertical, pos);
}

void XMatch(Call& c) {
  if (c.arg(0).IsError()) return c.Fail(c.arg(0).error());
  CellView key = View(c.arg(0));
  Grid g;
  if (!MakeGrid(c.m, c.arg(1), g)) return c.Fail(Err::Value);
  if (g.rows != 1 && g.cols != 1) return c.Fail(Err::Value);
  double mm, sm;
  if (!c.Integer(2, mm) || !c.Integer(3, sm, 1)) return;
  if (mm < -1 || mm > 2 || !(sm == 1 || sm == -1 || sm == 2 || sm == -2)) return c.Fail(Err::Value);
  int32_t pos = XFind(c, key, g, g.cols == 1, static_cast<int>(mm), static_cast<int>(sm));
  if (pos == -2) return;
  if (pos < 0) return c.Fail(Err::NA);
  c.SetNumber(pos + 1);
}

void Index(Call& c) {
  const Value& src = c.arg(0);
  if (src.IsError()) return c.Fail(src.error());
  double row, col, areaNo;
  if (!c.Integer(1, row) || !c.Integer(2, col) || !c.Integer(3, areaNo, 1)) return;
  if (row < 0 || col < 0 || areaNo < 1) return c.Fail(Err::Value);
  Value one = src;
  if (src.type() == Type::RefList) {
    if (areaNo > static_cast<double>(src.areas().size())) return c.Fail(Err::Ref);
    one = Value::Ref(src.areas()[static_cast<size_t>(areaNo) - 1], src.dynamic());
  } else if (areaNo != 1) {
    return c.Fail(Err::Ref);
  }
  Grid g;
  if (!MakeGrid(c.m, one, g)) return c.Fail(Err::Value);
  // One index into a single row or column picks along it; into a table, a whole row.
  if (c.argc < 3) {
    if (g.rows == 1 && g.cols > 1) col = row, row = 1;
    else if (g.cols == 1) col = 1;
    else col = 0;
  }
  if (row > g.rows || col > g.cols) return c.Fail(Err::Ref);
  if (g.rows == 1 && row == 0) row = 1;
  if (g.cols == 1 && col == 0) col = 1;
  int32_t r = static_cast<int32_t>(row), k = static_cast<int32_t>(col);
  if (g.ref) {
    Area a = g.area;
    if (r > 0) a.r1 = a.r2 = a.r1 + r - 1;
    if (k > 0) a.c1 = a.c2 = static_cast<int16_t>(a.c1 + k - 1);
    c.out = Value::Ref(a, one.dynamic());
    return;
  }
  if (!one.IsArray()) {
    if (r > 1 || k > 1) return c.Fail(Err::Ref);
    c.out = one;
    return;
  }
  const Array& arr = one.array();
  if (r > 0 && k > 0) {
    c.out = arr.at(r - 1, k - 1);
    return;
  }
  int32_t rows = r > 0 ? 1 : arr.rows(), cols = k > 0 ? 1 : arr.cols();
  Array* out = Array::Make(rows, cols);
  for (int32_t y = 0; y < rows; ++y)
    for (int32_t x = 0; x < cols; ++x) out->at(y, x) = arr.at(r > 0 ? r - 1 : y, k > 0 ? k - 1 : x);
  c.out = Value::Adopt(out);
}

static void RowCol(Call& c, bool row) {
  if (c.argc == 0 || c.arg(0).IsMissing()) return c.SetNumber((row ? c.m.at.row : c.m.at.col) + 1);
  const Value& v = c.arg(0);
  if (v.IsError()) return c.Fail(v.error());
  if (v.type() != Type::Ref) return c.Fail(Err::Value);
  const Area& a = v.area();
  int32_t first = row ? a.r1 : a.c1, n = row ? a.Rows() : a.Cols();
  if (n == 1 || !c.arrayMode) return c.SetNumber(first + 1);  // legacy formulas: the first only
  if (n > 1048576) n = 1048576;
  Array* out = row ? Array::Make(n, 1) : Array::Make(1, n);
  for (int32_t i = 0; i < n; ++i) out->data()[i] = Value::Number(first + i + 1);
  c.out = Value::Adopt(out);
}
void Row(Call& c) { RowCol(c, true); }
void Column(Call& c) { RowCol(c, false); }

static void Size(Call& c, bool rows) {
  const Value& v = c.arg(0);
  if (v.IsError()) return c.Fail(v.error());
  if (v.type() == Type::RefList) return c.Fail(Err::Ref);
  if (v.type() == Type::Ref) return c.SetNumber(rows ? v.area().Rows() : v.area().Cols());
  if (v.IsArray()) return c.SetNumber(rows ? v.array().rows() : v.array().cols());
  c.SetNumber(1);
}
void Rows(Call& c) { Size(c, true); }
void Columns(Call& c) { Size(c, false); }

void Offset(Call& c) {
  const Value& v = c.arg(0);
  if (v.IsError()) return c.Fail(v.error());
  if (v.type() != Type::Ref) return c.Fail(Err::Value);
  Area a = v.area();
  double dr, dc, h, w;
  if (!c.Integer(1, dr) || !c.Integer(2, dc) || !c.Integer(3, h, a.Rows()) || !c.Integer(4, w, a.Cols())) return;
  if (h == 0 || w == 0) return c.Fail(Err::Ref);
  double r1 = a.r1 + dr, c1 = a.c1 + dc;
  double r2 = h > 0 ? r1 + h - 1 : r1, c2 = w > 0 ? c1 + w - 1 : c1;
  if (h < 0) r1 = r1 + h + 1;  // a negative height extends upward
  if (w < 0) c1 = c1 + w + 1;
  if (r1 < 0 || c1 < 0 || r2 >= kMaxRows || c2 >= kMaxCols) return c.Fail(Err::Ref);
  a.r1 = static_cast<int32_t>(r1), a.r2 = static_cast<int32_t>(r2);
  a.c1 = static_cast<int16_t>(c1), a.c2 = static_cast<int16_t>(c2);
  c.out = Value::Ref(a, true);
}

void Indirect(Call& c) {
  std::string text;
  bool a1;
  if (!c.Text(0, text) || !c.Boolean(1, a1, true)) return;
  if (!c.m.res || text.empty()) return c.Fail(Err::Ref);
  CompileOptions at;
  at.sheet = c.m.at.sheet, at.row = c.m.at.row, at.col = c.m.at.col, at.r1c1 = !a1;
  auto f = Compile(text, at, c.m.res->names());
  if (!f || f->code.empty() || (f->flags & Formula::kUnsupported)) return c.Fail(Err::Ref);
  uint8_t op = f->code[0];
  if (op != bc::kRef && op != bc::kArea && op != bc::kName && op != bc::kTable) return c.Fail(Err::Ref);
  if (bc::InstrLength(f->code.data()) != f->code.size() && op != bc::kName) return c.Fail(Err::Ref);
  Value v = c.m.RunOne(f->code.data(), f->code.data() + f->code.size());
  if (!v.IsRef()) return c.Fail(Err::Ref);
  if (v.type() == Type::Ref) c.out = Value::Ref(v.area(), true);
  else c.out = Value::Refs(v.areas(), true);
}

void Address(Call& c) {
  double row, col, abs;
  bool a1;
  std::string sheet;
  if (!c.Integer(0, row) || !c.Integer(1, col) || !c.Integer(2, abs, 1) || !c.Boolean(3, a1, true)) return;
  if (c.Has(4) && !c.Text(4, sheet)) return;
  if (row < 1 || col < 1 || row > kMaxRows || col > kMaxCols || abs < 1 || abs > 4) return c.Fail(Err::Value);
  bool rAbs = abs == 1 || abs == 2, cAbs = abs == 1 || abs == 3;
  std::string s;
  if (a1) {
    s = (cAbs ? "$" : "") + ColumnName(static_cast<int32_t>(col) - 1) + (rAbs ? "$" : "") +
        std::to_string(static_cast<int64_t>(row));
  } else {
    auto part = [](char letter, double v, bool isAbs) {
      std::string n = std::to_string(static_cast<int64_t>(v));
      return std::string(1, letter) + (isAbs ? n : "[" + n + "]");
    };
    s = part('R', row, rAbs) + part('C', col, cAbs);
  }
  if (!sheet.empty()) s = pkg::QuoteSheetName(sheet) + "!" + s;
  c.out = Value::Text(s);
}

void Transpose(Call& c) {
  Value v = c.arg(0);
  if (v.IsRef()) v = c.arrayMode ? c.m.RefArray(v) : c.m.Intersect(v);  // as Excel's legacy formulas
  if (!v.IsArray()) {
    c.out = v;
    return;
  }
  const Array& a = v.array();
  Array* out = Array::Make(a.cols(), a.rows());
  for (int32_t r = 0; r < a.rows(); ++r)
    for (int32_t k = 0; k < a.cols(); ++k) out->at(k, r) = a.at(r, k);
  c.out = Value::Adopt(out);
}

// --- conditional aggregates ---

void CountIf(Call& c) {
  Conditions k;
  if (!MakeConditions(c, 0, k)) return;
  int32_t rows, cols;
  Limits(k, nullptr, rows, cols);
  double n = 0;
  EachMatch(k, rows, cols, [&](int32_t, int32_t) { return ++n, true; });
  if (k.allBlank) n += static_cast<double>(k.rows) * k.cols - static_cast<double>(rows) * cols;
  c.SetNumber(n);
}
void CountIfs(Call& c) { CountIf(c); }

void SumIf(Call& c) {
  Conditions k;
  if (c.argc < 3 || c.arg(2).IsMissing()) {
    if (!MakeConditions(c, 0, k)) return;
    Value src = c.arg(0);
    Grid g;
    int32_t rows, cols;
    Limits(k, &k.grids[0], rows, cols);
    double sum = 0;
    Err err = Err::None;
    EachMatch(k, rows, cols, [&](int32_t r, int32_t col) {
      CellView v = k.grids[0].At(r, col);
      if (v.type == Type::Error) return err = v.err, false;
      if (v.type == Type::Number) sum += v.num;
      return true;
    });
    if (err != Err::None) return c.Fail(err);
    return c.SetNumber(sum);
  }
  Value args[2] = {c.arg(0), c.arg(1)};
  Call sub{c.m, args, 2, c.arrayMode, Value()};
  if (!MakeConditions(sub, 0, k)) return c.Fail(sub.out.IsError() ? sub.out.error() : Err::Value);
  Aggregate(c, 2, k, Agg::kSum);
}

void SumIfs(Call& c) {
  Conditions k;
  if (!MakeConditions(c, 1, k)) return;
  Aggregate(c, 0, k, Agg::kSum);
}

void AverageIf(Call& c) {
  Conditions k;
  Value args[2] = {c.arg(0), c.arg(1)};
  Call sub{c.m, args, 2, c.arrayMode, Value()};
  if (!MakeConditions(sub, 0, k)) return c.Fail(sub.out.IsError() ? sub.out.error() : Err::Value);
  Aggregate(c, c.Has(2) ? 2 : 0, k, Agg::kAverage);
}

void AverageIfs(Call& c) {
  Conditions k;
  if (!MakeConditions(c, 1, k)) return;
  Aggregate(c, 0, k, Agg::kAverage);
}

void MaxIfs(Call& c) {
  Conditions k;
  if (!MakeConditions(c, 1, k)) return;
  Aggregate(c, 0, k, Agg::kMax);
}

void MinIfs(Call& c) {
  Conditions k;
  if (!MakeConditions(c, 1, k)) return;
  Aggregate(c, 0, k, Agg::kMin);
}


// --- Information about references (measured in Excel 2021): ISFORMULA, FORMULATEXT, SHEET,
// SHEETS, AREAS, CELL.

namespace {

bool TopLeft(const Value& v, int& sheet, int32_t& row, int32_t& col) {
  if (v.type() == Type::Ref) {
    sheet = v.area().s1, row = v.area().r1, col = v.area().c1;
    return true;
  }
  if (v.type() == Type::RefList && !v.areas().empty()) {
    sheet = v.areas()[0].s1, row = v.areas()[0].r1, col = v.areas()[0].c1;
    return true;
  }
  return false;
}

}  // namespace

void IsFormula(Call& c) {
  int s;
  int32_t r, k;
  if (c.arg(0).IsError()) return c.Fail(c.arg(0).error());
  if (!TopLeft(c.arg(0), s, r, k)) return c.Fail(Err::Value);
  std::string text;
  c.out = Value::Bool(c.m.res && c.m.res->FormulaText(s, r, k, text));
}

// The formula as the formula bar shows it: in the region's form.
void FormulaText(Call& c) {
  int s;
  int32_t r, k;
  if (c.arg(0).IsError()) return c.Fail(c.arg(0).error());
  if (!TopLeft(c.arg(0), s, r, k)) return c.Fail(Err::NA);
  std::string text;
  if (!c.m.res || !c.m.res->FormulaText(s, r, k, text)) return c.Fail(Err::NA);
  FormulaLocale region;
  if (c.m.decimal == ',') region = FormulaLocale{',', ';', '\\', ';'};
  c.out = Value::Text(ToLocal(text, region));
}

void Sheet(Call& c) {
  if (!c.Has(0)) return c.SetNumber(c.m.at.sheet + 1);
  const Value& v = c.arg(0);
  int s;
  int32_t r, k;
  if (TopLeft(v, s, r, k)) return c.SetNumber(s + 1);
  if (v.IsError()) return c.Fail(v.error());
  if (v.IsText() && c.m.res) {
    int found = c.m.res->names().FindSheet(v.text());
    if (found >= 0) return c.SetNumber(found + 1);
  }
  c.Fail(Err::NA);
}

void Sheets(Call& c) {
  if (!c.Has(0)) return c.SetNumber(c.m.host.SheetCount());
  const Value& v = c.arg(0);
  if (v.type() == Type::Ref) return c.SetNumber(v.area().s2 - v.area().s1 + 1);
  if (v.type() == Type::RefList) {
    int lo = 65535, hi = -1;
    for (const Area& a : v.areas()) lo = std::min<int>(lo, a.s1), hi = std::max<int>(hi, a.s2);
    return c.SetNumber(hi >= lo ? hi - lo + 1 : 0);
  }
  if (v.IsError()) return c.Fail(v.error());
  c.Fail(Err::NA);
}

void Areas(Call& c) {
  const Value& v = c.arg(0);
  if (v.type() == Type::Ref) return c.SetNumber(1);
  if (v.type() == Type::RefList) return c.SetNumber(static_cast<double>(v.areas().size()));
  if (v.IsError()) return c.Fail(v.error());
  c.Fail(Err::Value);
}

// CELL(info_type, [reference]): what doesn't need the file's path, formats or the active cell.
void CellInfo(Call& c) {
  std::string what;
  if (!c.Text(0, what)) return;
  for (char& ch : what) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
  static const char* const kKnown[] = {"address", "col", "color", "contents", "filename", "format",
                                       "parentheses", "prefix", "protect", "row", "type", "width"};
  bool known = false;
  for (const char* k : kKnown) known = known || what == k;
  if (!known) return c.Fail(Err::Value);
  int s;
  int32_t r, k;
  if (!c.Has(1) || !TopLeft(c.arg(1), s, r, k) ||
      (what != "address" && what != "col" && what != "row" && what != "contents" && what != "type") ||
      (what == "address" && s != c.m.at.sheet)) {
    c.m.unsupported = true;  // the active cell, the file's name, formats: kept as Excel left them
    return c.Fail(Err::NA);
  }
  if (what == "col") return c.SetNumber(k + 1);
  if (what == "row") return c.SetNumber(r + 1);
  if (what == "address") return void(c.out = Value::Text("$" + ColumnName(k) + "$" + std::to_string(r + 1)));
  Area one;
  one.s1 = one.s2 = static_cast<uint16_t>(s), one.r1 = one.r2 = r, one.c1 = one.c2 = static_cast<int16_t>(k);
  if (!c.m.Ready(Value::Ref(one, c.arg(1).dynamic()))) return c.Fail(Err::Value);
  Value v = c.m.CellValue(s, r, k);
  if (what == "contents") return void(c.out = v.IsEmpty() ? Value::Number(0) : v);
  c.out = Value::Text(v.IsEmpty() ? "b" : v.IsText() ? "l" : "v");
}

}  // namespace fw::sheet::calc::fn
