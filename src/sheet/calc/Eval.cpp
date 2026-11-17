// The bytecode machine: operators, lazy functions (IF, IFERROR, IFNA, CHOOSE, IFS, SWITCH), calls
// with Excel's argument classes, and the helpers functions read their arguments with.

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "sheet/calc/Fn.h"

namespace fw::sheet::calc {

using namespace bc;

namespace {

constexpr double kTwoPowMinus50 = 8.8817841970012523e-16;
constexpr size_t kMaxText = 32767;

int32_t Wrap(int64_t v, int32_t max) {
  int64_t r = v % max;
  return static_cast<int32_t>(r < 0 ? r + max : r);
}

Value NumberOrError(double d) {
  if (!std::isfinite(d)) return Value::Error(Err::Num);
  return Value::Number(d);
}

// Element (r, c) of a value broadcast to a larger shape, as Excel's array arithmetic does: a single
// row or column repeats; past the end of a larger one is #N/A.
Value Element(const Value& v, int32_t r, int32_t c) {
  if (!v.IsArray()) return v;
  const Array& a = v.array();
  int32_t rr = a.rows() == 1 ? 0 : r, cc = a.cols() == 1 ? 0 : c;
  if (rr >= a.rows() || cc >= a.cols()) return Value::Error(Err::NA);
  return a.at(rr, cc);
}

void Shape(const Value& v, int32_t& rows, int32_t& cols) {
  if (v.IsArray()) {
    rows = std::max(rows, v.array().rows());
    cols = std::max(cols, v.array().cols());
  }
}

int FnId(const char* name) { return FindFunction(name); }

}  // namespace

// --- Host defaults ---

double Host::Now() {
  SYSTEMTIME st;
  GetLocalTime(&st);
  double serial = 0;
  DateSerial(st.wYear, st.wMonth, st.wDay, Date1904(), serial);
  return serial + (st.wHour * 3600.0 + st.wMinute * 60.0 + st.wSecond + st.wMilliseconds / 1000.0) / 86400.0;
}

double Host::Random() {
  static uint64_t state = 0;
  if (!state) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    state = static_cast<uint64_t>(t.QuadPart) * 0x9E3779B97F4A7C15ull | 1;
  }
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return static_cast<double>(state >> 11) / 9007199254740992.0;
}

// --- conversions ---

CellView View(const Value& v) {
  CellView c;
  switch (v.type()) {
    case Type::Number: c.type = Type::Number, c.num = v.number(); break;
    case Type::Bool: c.type = Type::Bool, c.num = v.boolean() ? 1 : 0; break;
    case Type::Text: c.type = Type::Text, c.text = v.text(); break;
    case Type::Error: c.type = Type::Error, c.err = v.error(); break;
    default: break;
  }
  return c;
}

Value FromView(const CellView& c) {
  switch (c.type) {
    case Type::Number: return Value::Number(c.num);
    case Type::Bool: return Value::Bool(c.num != 0);
    case Type::Text: return Value::Text(c.text);
    case Type::Error: return Value::Error(c.err);
    default: return Value();
  }
}

bool ToNumber(Machine& m, const CellView& v, double& d, Err& e) {
  switch (v.type) {
    case Type::Number: d = v.num; return true;
    case Type::Bool: d = v.num; return true;
    case Type::Text:
      if (m.host.ParseNumber(v.text, d)) return true;
      e = Err::Value;
      return false;
    case Type::Error: e = v.err; return false;
    default: d = 0; return true;
  }
}

bool ToNumber(Machine& m, const Value& v, double& d, Err& e) {
  if (v.IsArray()) return ToNumber(m, View(v.array().at(0, 0)), d, e);
  if (v.IsRef()) {
    e = Err::Value;
    return false;
  }
  return ToNumber(m, View(v), d, e);
}

bool ToText(const Value& v, std::string& s, Err& e, char decimal) {
  switch (v.type()) {
    case Type::Error: e = v.error(); return false;
    case Type::Array: return ToText(v.array().at(0, 0), s, e, decimal);
    case Type::Ref: case Type::RefList: e = Err::Value; return false;
    default: s = ValueText(v, decimal); return true;
  }
}

bool ToBool(const Value& v, bool& b, Err& e) {
  switch (v.type()) {
    case Type::Number: b = v.number() != 0; return true;
    case Type::Bool: b = v.boolean(); return true;
    case Type::Text:
      if (EqualText(v.text(), "TRUE")) b = true;
      else if (EqualText(v.text(), "FALSE")) b = false;
      else {
        e = Err::Value;
        return false;
      }
      return true;
    case Type::Error: e = v.error(); return false;
    case Type::Array: return ToBool(v.array().at(0, 0), b, e);
    case Type::Ref: case Type::RefList: e = Err::Value; return false;
    default: b = false; return true;
  }
}

bool EqualNumbers(double a, double b) {
  if (a == b) return true;
  double d = std::fabs(a - b), m = std::max(std::fabs(a), std::fabs(b));
  if (d > m * 1e-14) return false;
  return Round15(a) == Round15(b);
}

int CompareValues(const CellView& a0, const CellView& b0) {
  CellView a = a0, b = b0;
  auto blank = [](CellView& x, const CellView& other) {
    if (x.type != Type::Empty) return;
    x.type = other.type == Type::Empty ? Type::Number : other.type;
    x.num = 0;
    x.text = {};
  };
  blank(a, b);
  blank(b, a);
  auto rank = [](Type t) { return t == Type::Number ? 0 : t == Type::Text ? 1 : t == Type::Bool ? 2 : 3; };
  int ra = rank(a.type), rb = rank(b.type);
  if (ra != rb) return ra < rb ? -1 : 1;
  switch (a.type) {
    case Type::Number: return EqualNumbers(a.num, b.num) ? 0 : a.num < b.num ? -1 : 1;
    case Type::Text: return CompareText(a.text, b.text);
    case Type::Bool: return a.num == b.num ? 0 : a.num < b.num ? -1 : 1;
    default: return 0;
  }
}

std::wstring Wide(std::string_view s) {
  std::wstring w;
  if (s.empty()) return w;
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  w.resize(static_cast<size_t>(n));
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string Narrow(std::wstring_view w) {
  std::string s;
  if (w.empty()) return s;
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  s.resize(static_cast<size_t>(n));
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

// --- Call helpers ---

bool Call::Number(int i, double& v, double def) {
  if (!Has(i)) {
    v = def;
    return true;
  }
  Err e = Err::None;
  if (ToNumber(m, args[i], v, e)) return true;
  Fail(e);
  return false;
}

bool Call::Integer(int i, double& v, double def) {
  if (!Number(i, v, def)) return false;
  v = std::trunc(v);
  return true;
}

bool Call::Text(int i, std::string& s) {
  if (!Has(i)) {
    s.clear();
    return true;
  }
  Err e = Err::None;
  if (ToText(args[i], s, e, m.decimal)) return true;
  Fail(e);
  return false;
}

bool Call::Boolean(int i, bool& b, bool def) {
  if (!Has(i)) {
    b = def;
    return true;
  }
  Err e = Err::None;
  if (ToBool(args[i], b, e)) return true;
  Fail(e);
  return false;
}

void Call::SetNumber(double v) { out = NumberOrError(v); }

// --- Grid and criteria ---

bool MakeGrid(Machine& m, const Value& v, Grid& g) {
  g = Grid();
  g.m = &m;
  g.v = &v;
  switch (v.type()) {
    case Type::Ref: {
      if (!m.Ready(v)) return false;
      const Area& a = v.area();
      if (a.s1 != a.s2) return false;
      g.ref = true;
      g.area = a;
      g.rows = a.Rows();
      g.cols = a.Cols();
      int32_t er = 0, ec = 0;
      m.host.Extent(a.s1, er, ec);
      g.dataRows = std::clamp(er - a.r1, 0, g.rows);
      g.dataCols = std::clamp(ec - a.c1, 0, g.cols);
      return true;
    }
    case Type::RefList: return false;
    case Type::Array:
      g.rows = g.dataRows = v.array().rows();
      g.cols = g.dataCols = v.array().cols();
      return true;
    default:
      g.rows = g.cols = g.dataRows = g.dataCols = 1;
      return true;
  }
}

CellView Grid::At(int32_t r, int32_t c) const {
  if (ref) {
    if (r >= dataRows || c >= dataCols) return {};
    return m->host.Cell(area.s1, area.r1 + r, area.c1 + c);
  }
  if (v->IsArray()) return View(v->array().at(r, c));
  return View(*v);
}

bool MakeCriteria(Machine& m, const Value& v, Criteria& c) {
  c = Criteria();
  c.conv = &m.host.Conv();
  switch (v.type()) {
    case Type::Number:  // 12 matches the text "12" too
      c.type = Type::Number, c.num = v.number(), c.text = NumberText(v.number());
      return true;
    case Type::Bool: c.type = Type::Bool, c.num = v.boolean() ? 1 : 0; return true;
    case Type::Error: c.type = Type::Error, c.err = v.error(); return true;
    case Type::Text: break;
    case Type::Array: return MakeCriteria(m, v.array().at(0, 0), c);
    default: c.type = Type::Number, c.num = 0; return true;  // a blank criteria cell is 0
  }
  std::string_view s = v.text();
  if (s.size() >= 2 && s[0] == '<' && s[1] == '=') c.op = Criteria::kLe, s.remove_prefix(2);
  else if (s.size() >= 2 && s[0] == '>' && s[1] == '=') c.op = Criteria::kGe, s.remove_prefix(2);
  else if (s.size() >= 2 && s[0] == '<' && s[1] == '>') c.op = Criteria::kNe, s.remove_prefix(2);
  else if (!s.empty() && s[0] == '<') c.op = Criteria::kLt, s.remove_prefix(1);
  else if (!s.empty() && s[0] == '>') c.op = Criteria::kGt, s.remove_prefix(1);
  else if (!s.empty() && s[0] == '=') c.op = Criteria::kEq, s.remove_prefix(1);
  if (s.empty()) {
    c.type = Type::Empty;
    return true;
  }
  double d;
  if (m.host.ParseNumber(s, d)) {
    c.type = Type::Number;
    c.num = d;
    c.text = std::string(s);
    return true;
  }
  if (EqualText(s, "TRUE") || EqualText(s, "FALSE")) {
    c.type = Type::Bool;
    c.num = EqualText(s, "TRUE") ? 1 : 0;
    return true;
  }
  size_t len = 0;
  Err e = ParseErr(s, &len);
  if (e != Err::None && len == s.size()) {
    c.type = Type::Error;
    c.err = e;
    return true;
  }
  c.type = Type::Text;
  c.text = std::string(s);
  c.wildcard = (c.op == Criteria::kEq || c.op == Criteria::kNe) && s.find_first_of("*?~") != std::string_view::npos;
  return true;
}

bool Criteria::Match(const CellView& cell) const {
  auto ordered = [&](int cmp) {
    switch (op) {
      case kEq: return cmp == 0;
      case kNe: return cmp != 0;
      case kLt: return cmp < 0;
      case kLe: return cmp <= 0;
      case kGt: return cmp > 0;
      case kGe: return cmp >= 0;
    }
    return false;
  };
  switch (type) {
    case Type::Empty: {  // "" or "=": blanks; "<>": anything not blank
      bool blank = cell.type == Type::Empty || (cell.type == Type::Text && cell.text.empty());
      if (op == kEq) return cell.type == Type::Empty || (cell.type == Type::Text && cell.text.empty());
      if (op == kNe) return cell.type != Type::Empty;
      return ordered(blank ? 0 : 1) && false;
    }
    case Type::Number:
      if (cell.type == Type::Number) return ordered(EqualNumbers(cell.num, num) ? 0 : cell.num < num ? -1 : 1);
      if (cell.type == Type::Text && (op == kEq || op == kNe)) {
        // 5 and "5" match a cell holding text that reads as 5 (" 5 " too), as in Excel
        double d;
        bool same = TextToNumber(cell.text, d, false, *conv) && EqualNumbers(d, num);
        return op == kEq ? same : !same;
      }
      return op == kNe;
    case Type::Bool:
      if (cell.type == Type::Bool) return ordered(cell.num == num ? 0 : cell.num < num ? -1 : 1);
      return op == kNe;
    case Type::Error:
      if (cell.type == Type::Error) return op == kEq ? cell.err == err : op == kNe ? cell.err != err : false;
      return op == kNe;
    case Type::Text:
      if (cell.type != Type::Text) return op == kNe;
      if (wildcard) {
        bool m = WildcardMatch(text, cell.text);
        return op == kEq ? m : !m;
      }
      if (op == kEq) return EqualText(cell.text, text);
      if (op == kNe) return !EqualText(cell.text, text);
      return ordered(CompareText(cell.text, text));
    default: return false;
  }
}

// --- the machine ---

Value Machine::CellValue(int sheet, int32_t row, int32_t col) { return FromView(host.Cell(sheet, row, col)); }

bool Machine::Ready(const Value& ref) {
  if (!ref.dynamic() || !res) return true;
  if (ref.type() == Type::Ref) {
    if (res->Ready(ref.area())) return true;
  } else {
    bool all = true;
    for (const Area& a : ref.areas()) all = all && res->Ready(a);
    if (all) return true;
  }
  stop = true;
  return false;
}

Value Machine::Intersect(const Value& ref) {
  if (!Ready(ref)) return Value::Error(Err::Value);
  if (ref.type() != Type::Ref) return Value::Error(Err::Value);
  const Area& a = ref.area();
  if (a.s1 != a.s2) return Value::Error(Err::Value);
  int32_t r, c;
  if (a.r1 == a.r2) r = a.r1;
  else if (at.row >= a.r1 && at.row <= a.r2) r = at.row;
  else return Value::Error(Err::Value);
  if (a.c1 == a.c2) c = a.c1;
  else if (at.col >= a.c1 && at.col <= a.c2) c = at.col;
  else return Value::Error(Err::Value);
  return CellValue(a.s1, r, c);
}

void Machine::WorkbookExtent(int32_t& rows, int32_t& cols) {
  if (extentRows_ < 0) {
    extentRows_ = extentCols_ = 0;
    for (int s = 0, n = host.SheetCount(); s < n; ++s) {
      int32_t r = 0, c = 0;
      host.Extent(s, r, c);
      extentRows_ = std::max(extentRows_, r);
      extentCols_ = std::max(extentCols_, c);
    }
  }
  rows = extentRows_;
  cols = extentCols_;
}

Value Machine::RefArray(const Value& ref) {
  if (!Ready(ref)) return Value::Error(Err::Value);
  if (ref.type() != Type::Ref) return Value::Error(Err::Value);
  Area a = ref.area();
  if (a.s1 != a.s2) return Value::Error(Err::Value);
  if (a.r1 == a.r2 && a.c1 == a.c2) return CellValue(a.s1, a.r1, a.c1);
  // Whole columns and rows: only as far as the workbook's cells reach.
  if (a.Rows() > 65536 || a.Cols() > 1024) {
    int32_t er, ec;
    WorkbookExtent(er, ec);
    if (a.Rows() > 65536) a.r2 = std::max(a.r1, std::min(a.r2, er - 1));
    if (a.Cols() > 1024) a.c2 = static_cast<int16_t>(std::max<int32_t>(a.c1, std::min<int32_t>(a.c2, ec - 1)));
  }
  if (static_cast<int64_t>(a.Rows()) * a.Cols() > 16'000'000) return Value::Error(Err::Num);
  Array* arr = Array::Make(a.Rows(), a.Cols());
  struct Fill {
    Array* arr;
    int32_t r1, c1;
    static bool Cell(void* ctx, int32_t r, int32_t c, const CellView& v) {
      auto* f = static_cast<Fill*>(ctx);
      f->arr->at(r - f->r1, c - f->c1) = FromView(v);
      return true;
    }
  } fill{arr, a.r1, a.c1};
  host.ForEachCell(a.s1, a.r1, a.c1, a.r2, a.c2, &Fill::Cell, &fill);
  return Value::Adopt(arr);
}

Value Machine::Deref(const Value& v, bool arrayMode) {
  if (!v.IsRef()) return v;
  return arrayMode ? RefArray(v) : Intersect(v);
}

namespace {

Value Pop(std::vector<Value>& s) {
  if (s.empty()) return Value::Error(Err::Value);
  Value v = std::move(s.back());
  s.pop_back();
  return v;
}

Value UnaryScalar(Machine& m, uint8_t op, const Value& a) {
  if (a.IsError()) return a;
  double d;
  Err e = Err::None;
  if (!ToNumber(m, a, d, e)) return Value::Error(e);
  return NumberOrError(op == kNeg ? -d : d / 100);
}

Value BinaryScalar(Machine& m, uint8_t op, const Value& a, const Value& b, bool final) {
  if (a.IsError()) return a;
  if (b.IsError()) return b;
  if (op == kConcat) {
    std::string s = ValueText(a, m.decimal);
    s += ValueText(b, m.decimal);
    if (s.size() > kMaxText) return Value::Error(Err::Value);
    return Value::Text(s);
  }
  if (op >= kEq && op <= kGe) {
    CellView x = View(a), y = View(b);
    if (op == kEq || op == kNe) {
      bool eq;
      if (x.type == Type::Text && y.type == Type::Text) eq = EqualText(x.text, y.text);
      else eq = CompareValues(x, y) == 0;
      return Value::Bool(op == kEq ? eq : !eq);
    }
    int c = CompareValues(x, y);
    switch (op) {
      case kLt: return Value::Bool(c < 0);
      case kLe: return Value::Bool(c <= 0);
      case kGt: return Value::Bool(c > 0);
      default: return Value::Bool(c >= 0);
    }
  }
  double x, y;
  Err e = Err::None;
  if (!ToNumber(m, a, x, e) || !ToNumber(m, b, y, e)) return Value::Error(e);
  double r;
  switch (op) {
    case kAdd:
    case kSub:
      r = op == kAdd ? x + y : x - y;
      // Excel: a formula ending in + or - whose result is a rounding error away from zero gives 0.
      if (final && r != 0 && std::fabs(r) <= std::max(std::fabs(x), std::fabs(y)) * kTwoPowMinus50) r = 0;
      break;
    case kMul: r = x * y; break;
    case kDiv:
      if (y == 0) return Value::Error(Err::Div0);
      r = x / y;
      break;
    case kPow:
      if (x == 0 && y == 0) return Value::Error(Err::Num);
      if (x == 0 && y < 0) return Value::Error(Err::Div0);
      if (x < 0 && y != std::floor(y)) {
        // Odd roots of negatives: Excel allows 1/n with odd n only when exact; otherwise #NUM!.
        double inv = 1 / y;
        if (std::fabs(inv - std::round(inv)) < 1e-10 && static_cast<int64_t>(std::round(inv)) % 2 != 0) {
          r = -std::pow(-x, y);
          break;
        }
        return Value::Error(Err::Num);
      }
      r = std::pow(x, y);
      break;
    default: return Value::Error(Err::Value);
  }
  return NumberOrError(r);
}

bool SameSheet(const Area& a, const Area& b) { return a.s1 == b.s1 && a.s2 == b.s2; }

}  // namespace

void Machine::Run(const uint8_t* p, const uint8_t* end, bool top) {
  if (++depth > 100) {  // names defined through each other
    stack.push_back(Value::Error(Err::Name));
    --depth;
    return;
  }
  while (p < end && !stop) {
    size_t len = InstrLength(p);
    switch (p[0]) {
      case kNum: stack.push_back(Value::Number(Read<double>(p + 1))); break;
      case kStr: {
        size_t l;
        stack.push_back(Value::Text(ReadStr(p + 1, &l)));
        break;
      }
      case kBool: stack.push_back(Value::Bool(p[1] != 0)); break;
      case kErr: stack.push_back(Value::Error(static_cast<Err>(p[1]))); break;
      case kMissing: stack.push_back(Value::Missing()); break;
      case kParen: break;
      case kArray: {
        int32_t rows = Read<uint16_t>(p + 1), cols = Read<uint16_t>(p + 3);
        Array* a = Array::Make(rows, cols);
        const uint8_t* q = p + 5;
        for (int32_t i = 0; i < rows * cols; ++i) {
          size_t l = 2;
          switch (q[0]) {
            case kNum: a->data()[i] = Value::Number(Read<double>(q + 1)), l = 9; break;
            case kStr: {
              size_t sl;
              a->data()[i] = Value::Text(ReadStr(q + 1, &sl));
              l = 1 + sl;
              break;
            }
            case kBool: a->data()[i] = Value::Bool(q[1] != 0); break;
            default: a->data()[i] = Value::Error(static_cast<Err>(q[1])); break;
          }
          q += l;
        }
        stack.push_back(Value::Adopt(a));
        break;
      }
      case kRef:
      case kArea: {
        uint8_t fl = p[1];
        const uint8_t* q = p + 2;
        int s1 = at.sheet, s2 = at.sheet;
        if (fl & kSheet) {
          s1 = s2 = Read<uint16_t>(q);
          q += 2;
          if (fl & kSheet3D) {
            s2 = Read<uint16_t>(q);
            q += 2;
          }
        }
        if (s2 >= host.SheetCount() || s1 > s2) {
          stack.push_back(Value::Error(Err::Ref));
          break;
        }
        int32_t r1 = Read<int32_t>(q), c1 = Read<int16_t>(q + 4), r2 = r1, c2 = c1;
        bool r2Abs = fl & kRowAbs, c2Abs = fl & kColAbs;
        if (p[0] == kArea) {
          r2 = Read<int32_t>(q + 6);
          c2 = Read<int16_t>(q + 10);
          r2Abs = fl & kRow2Abs;
          c2Abs = fl & kCol2Abs;
        }
        if (!(fl & kRowAbs)) r1 = Wrap(static_cast<int64_t>(at.row) + r1, kMaxRows);
        if (!(fl & kColAbs)) c1 = Wrap(static_cast<int64_t>(at.col) + c1, kMaxCols);
        if (!r2Abs) r2 = Wrap(static_cast<int64_t>(at.row) + r2, kMaxRows);
        if (!c2Abs) c2 = Wrap(static_cast<int64_t>(at.col) + c2, kMaxCols);
        Area a;
        a.r1 = std::min(r1, r2), a.r2 = std::max(r1, r2);
        a.c1 = static_cast<int16_t>(std::min(c1, c2)), a.c2 = static_cast<int16_t>(std::max(c1, c2));
        a.s1 = static_cast<uint16_t>(s1), a.s2 = static_cast<uint16_t>(s2);
        stack.push_back(Value::Ref(a));
        break;
      }
      case kRefErr:
      case kExtern: stack.push_back(Value::Error(Err::Ref)); break;
      case kName: {
        uint32_t slot = Read<uint32_t>(p + 1);
        if (!(p[5] & kSheet)) {
          size_t k = locals.size();
          while (k > localsBase && locals[k - 1].first != slot) --k;
          if (k > localsBase) {
            stack.push_back(locals[k - 1].second);
            break;
          }
        }
        int scope = (p[5] & kSheet) ? Read<uint16_t>(p + 6) : at.sheet;
        const Formula* f = res ? res->Name(slot, scope) : nullptr;
        if (!f) {
          stack.push_back(Value::Error(Err::Name));
          break;
        }
        if (f->flags & Formula::kUnsupported) unsupported = true;
        size_t before = stack.size();
        size_t saveBase = localsBase;
        localsBase = locals.size();
        Run(f->code.data(), f->code.data() + f->code.size(), false);
        localsBase = saveBase;
        if (stack.size() != before + 1 && !stop) {
          stack.resize(before);
          stack.push_back(Value::Error(Err::Value));
        }
        break;
      }
      case kTable: {
        uint8_t items = p[1];
        size_t k = 2, l;
        std::string_view table = ReadStr(p + k, &l);
        k += l;
        std::string_view c1 = ReadStr(p + k, &l);
        k += l;
        std::string_view c2 = ReadStr(p + k, &l);
        Area a;
        if (host.Table(table, at.sheet, at.row, at.col, items, c1, c2, a)) {
          stack.push_back(Value::Ref(a));
        } else {  // tables the host doesn't know (yet): keep the cached value
          unsupported = true;
          stack.push_back(Value::Error(Err::Ref));
        }
        break;
      }
      case kUnary: Unary(p[1]); break;
      case kBinary: Binary(p[1], top && p + len == end); break;
      case kCall: CallFn(Read<uint16_t>(p + 1), p[3], p[4] != 0); break;
      case kLazy: p = Lazy(p); continue;
      case kUnknown: {
        int argc = p[1];
        for (int i = 0; i < argc; ++i) Pop(stack);
        stack.push_back(Value::Error(Err::Name));
        unsupported = true;
        break;
      }
      default:
        stack.push_back(Value::Error(Err::Value));
        p = end;
        continue;
    }
    p += len;
  }
  --depth;
}

Value Machine::RunOne(const uint8_t* p, const uint8_t* end) {
  size_t before = stack.size();
  Run(p, end, false);
  if (stack.size() <= before) return Value::Error(Err::Value);
  Value v = std::move(stack.back());
  stack.resize(before);
  return v;
}

void Machine::Unary(uint8_t code) {
  uint8_t op = code & ~kArrayMode;
  bool arr = code & kArrayMode;
  Value a = Pop(stack);
  switch (op) {
    case kPlus: stack.push_back(std::move(a)); return;
    case kImplicit: {  // @: one cell of a reference, the first element of an array
      if (a.IsRef()) {
        Value v = Intersect(a);
        if (!v.IsError() || a.type() != Type::Ref) {
          const Area& r = a.area();
          if (r.Single()) {
            stack.push_back(std::move(a));
            return;
          }
        }
        stack.push_back(std::move(v));
        return;
      }
      if (a.IsArray()) {
        stack.push_back(a.array().at(0, 0));
        return;
      }
      stack.push_back(std::move(a));
      return;
    }
    case kSpill: {
      Area out;
      if (a.type() == Type::Ref && res && res->Spill(a.area().s1, a.area().r1, a.area().c1, out)) {
        stack.push_back(Value::Ref(out, true));
      } else {
        stack.push_back(a.IsError() ? a : Value::Error(Err::Ref));
      }
      return;
    }
    default: break;
  }
  Value v = Deref(a, arr);
  if (v.IsArray()) {
    const Array& src = v.array();
    Array* r = Array::Make(src.rows(), src.cols());
    for (int32_t i = 0; i < src.size(); ++i) r->data()[i] = UnaryScalar(*this, op, src.data()[i]);
    stack.push_back(Value::Adopt(r));
    return;
  }
  stack.push_back(UnaryScalar(*this, op, v));
}

void Machine::Binary(uint8_t code, bool final) {
  uint8_t op = code & ~kArrayMode;
  bool arr = code & kArrayMode;
  Value b = Pop(stack), a = Pop(stack);
  if (op == kRange || op == kUnion || op == kIsect) {
    if (a.IsError()) return stack.push_back(std::move(a));
    if (b.IsError()) return stack.push_back(std::move(b));
    if (!a.IsRef() || !b.IsRef()) return stack.push_back(Value::Error(Err::Value));
    std::vector<Area> la = a.type() == Type::Ref ? std::vector<Area>{a.area()} : a.areas();
    std::vector<Area> lb = b.type() == Type::Ref ? std::vector<Area>{b.area()} : b.areas();
    bool dyn = a.dynamic() || b.dynamic();
    if (op == kUnion) {
      for (const Area& x : lb) la.push_back(x);
      return stack.push_back(Value::Refs(std::move(la), dyn));
    }
    if (op == kRange) {  // the smallest area holding both
      Area box = la[0];
      auto grow = [&](const Area& x) {
        box.r1 = std::min(box.r1, x.r1), box.r2 = std::max(box.r2, x.r2);
        box.c1 = std::min(box.c1, x.c1), box.c2 = std::max(box.c2, x.c2);
      };
      for (const auto* l : {&la, &lb})
        for (const Area& x : *l) {
          if (!SameSheet(x, box)) return stack.push_back(Value::Error(Err::Value));
          grow(x);
        }
      // Wider than both operands: cells not among the formula's precedents may be read.
      bool wider = !(box == la[0]) && !(box == lb[0]);
      return stack.push_back(Value::Ref(box, dyn || wider));
    }
    std::vector<Area> out;  // intersection
    for (const Area& x : la)
      for (const Area& y : lb) {
        if (!SameSheet(x, y)) continue;
        Area z = x;
        z.r1 = std::max(x.r1, y.r1), z.r2 = std::min(x.r2, y.r2);
        z.c1 = std::max(x.c1, y.c1), z.c2 = std::min(x.c2, y.c2);
        if (z.r1 <= z.r2 && z.c1 <= z.c2) out.push_back(z);
      }
    if (out.empty()) return stack.push_back(Value::Error(Err::Null));
    return stack.push_back(Value::Refs(std::move(out), dyn));
  }
  Value x = Deref(a, arr), y = Deref(b, arr);
  if (x.IsArray() || y.IsArray()) {
    int32_t rows = 1, cols = 1;
    Shape(x, rows, cols);
    Shape(y, rows, cols);
    Array* r = Array::Make(rows, cols);
    for (int32_t i = 0; i < rows; ++i)
      for (int32_t j = 0; j < cols; ++j) r->at(i, j) = BinaryScalar(*this, op, Element(x, i, j), Element(y, i, j), final);
    return stack.push_back(Value::Adopt(r));
  }
  stack.push_back(BinaryScalar(*this, op, x, y, final));
}

void Machine::CallFn(int id, int argc, bool arrayMode) {
  const FuncInfo& f = Function(id);
  if (static_cast<int>(stack.size()) < argc) {
    stack.push_back(Value::Error(Err::Value));
    return;
  }
  Value* args = stack.data() + (stack.size() - static_cast<size_t>(argc));
  if (!f.impl) {
    unsupported = true;
    stack.resize(stack.size() - static_cast<size_t>(argc));
    stack.push_back(Value::Error(Err::Name));
    return;
  }
  bool lift = false;
  for (int i = 0; i < argc; ++i) {
    if (ParamClass(f, i) != 'V') continue;
    if (args[i].IsRef()) args[i] = Deref(args[i], arrayMode);
    if (args[i].IsArray()) lift = true;
  }
  Value result;
  if (stop) {
    result = Value::Error(Err::Value);
  } else if (!lift) {
    Call c{*this, args, argc, arrayMode, Value()};
    f.impl(c);
    result = std::move(c.out);
  } else {
    // A 'V' argument given an array: the function runs for each element.
    int32_t rows = 1, cols = 1;
    for (int i = 0; i < argc; ++i)
      if (ParamClass(f, i) == 'V') Shape(args[i], rows, cols);
    Array* r = Array::Make(rows, cols);
    std::vector<Value> one(args, args + argc);
    for (int32_t i = 0; i < rows && !stop; ++i)
      for (int32_t j = 0; j < cols && !stop; ++j) {
        for (int k = 0; k < argc; ++k)
          if (ParamClass(f, k) == 'V' && args[k].IsArray()) one[static_cast<size_t>(k)] = Element(args[k], i, j);
        Call c{*this, one.data(), argc, arrayMode, Value()};
        f.impl(c);
        Value v = std::move(c.out);
        if (v.IsRef()) v = Intersect(v);
        if (v.IsArray()) v = Value(v.array().at(0, 0));
        r->at(i, j) = std::move(v);
      }
    result = Value::Adopt(r);
  }
  stack.resize(stack.size() - static_cast<size_t>(argc));
  stack.push_back(std::move(result));
}

const uint8_t* Machine::Lazy(const uint8_t* p) {
  static const int kIf = FnId("IF"), kIfError = FnId("IFERROR"), kIfNa = FnId("IFNA"), kChoose = FnId("CHOOSE"),
                   kIfs = FnId("IFS"), kSwitch = FnId("SWITCH"), kLet = FnId("LET");
  int id = Read<uint16_t>(p + 1);
  int argc = p[3];
  bool arr = p[4] != 0;
  const uint8_t* spans[256];
  const uint8_t* q = p + 5 + 4 * static_cast<size_t>(argc);
  for (int i = 0; i < argc; ++i) {
    spans[i] = q;
    q += Read<uint32_t>(p + 5 + 4 * i);
  }
  spans[argc] = q;
  auto arg = [&](int i) { return RunOne(spans[i], spans[i + 1]); };
  auto zeroIfMissing = [](Value v) { return v.IsMissing() ? Value::Number(0) : v; };
  Value result;
  if (id == kIf) {
    Value cond = Deref(arg(0), arr);
    if (stop) return q;
    if (cond.IsArray()) {  // element by element, both branches evaluated
      Value a = argc > 1 ? Deref(zeroIfMissing(arg(1)), true) : Value::Bool(true);
      Value b = argc > 2 ? Deref(zeroIfMissing(arg(2)), true) : Value::Bool(false);
      int32_t rows = 1, cols = 1;
      Shape(cond, rows, cols);
      Shape(a, rows, cols);
      Shape(b, rows, cols);
      Array* r = Array::Make(rows, cols);
      for (int32_t i = 0; i < rows; ++i)
        for (int32_t j = 0; j < cols; ++j) {
          Value c = Element(cond, i, j);
          bool t;
          Err e = Err::None;
          if (c.IsError()) r->at(i, j) = c;
          else if (!ToBool(c, t, e)) r->at(i, j) = Value::Error(e);
          else r->at(i, j) = Element(t ? a : b, i, j);
        }
      result = Value::Adopt(r);
    } else {
      bool t = false;
      Err e = Err::None;
      if (cond.IsError()) result = cond;
      else if (!ToBool(cond, t, e)) result = Value::Error(e);
      else if (t) result = argc > 1 ? zeroIfMissing(arg(1)) : Value::Bool(true);
      else result = argc > 2 ? zeroIfMissing(arg(2)) : Value::Bool(false);
    }
  } else if (id == kIfError || id == kIfNa) {
    auto caught = [&](const Value& v) { return v.IsError() && (id == kIfError || v.error() == Err::NA); };
    Value v = Deref(arg(0), arr);
    if (v.IsArray()) {
      Value alt;
      bool haveAlt = false;
      const Array& src = v.array();
      Array* r = Array::Make(src.rows(), src.cols());
      for (int32_t i = 0; i < src.size() && !stop; ++i) {
        if (caught(src.data()[i])) {
          if (!haveAlt) alt = Deref(zeroIfMissing(arg(1)), true), haveAlt = true;
          int32_t rr = i / src.cols(), cc = i % src.cols();
          r->data()[i] = Element(alt, rr, cc);
        } else {
          r->data()[i] = src.data()[i];
        }
      }
      result = Value::Adopt(r);
    } else if (caught(v)) {
      result = Deref(zeroIfMissing(arg(1)), arr);
    } else {
      result = zeroIfMissing(std::move(v));
    }
  } else if (id == kChoose) {
    Value i = Deref(arg(0), arr);
    double d;
    Err e = Err::None;
    if (i.IsArray()) {
      std::vector<Value> choices;
      for (int k = 1; k < argc; ++k) choices.push_back(Deref(zeroIfMissing(arg(k)), true));
      const Array& src = i.array();
      int32_t rows = src.rows(), cols = src.cols();
      for (const Value& c : choices) Shape(c, rows, cols);
      Array* r = Array::Make(rows, cols);
      for (int32_t y = 0; y < rows; ++y)
        for (int32_t x = 0; x < cols; ++x) {
          Value n = Element(i, y, x);
          if (!ToNumber(*this, n, d, e)) r->at(y, x) = Value::Error(e);
          else if (d < 1 || d >= argc) r->at(y, x) = Value::Error(Err::Value);
          else r->at(y, x) = Element(choices[static_cast<size_t>(d) - 1], y, x);
        }
      result = Value::Adopt(r);
    } else if (!ToNumber(*this, i, d, e)) {
      result = Value::Error(e);
    } else if (d < 1 || std::trunc(d) >= argc) {
      result = Value::Error(Err::Value);
    } else {
      result = zeroIfMissing(arg(static_cast<int>(d)));
    }
  } else if (id == kIfs) {
    result = Value::Error(Err::NA);
    for (int k = 0; k + 1 < argc; k += 2) {
      Value c = Deref(arg(k), arr);
      if (stop) break;
      if (c.IsArray()) {
        unsupported = true;
        break;
      }
      bool t;
      Err e = Err::None;
      if (c.IsError()) {
        result = c;
        break;
      }
      if (!ToBool(c, t, e)) {
        result = Value::Error(e);
        break;
      }
      if (t) {
        result = zeroIfMissing(arg(k + 1));
        break;
      }
    }
  } else if (id == kSwitch) {
    Value e = Deref(arg(0), arr);
    int pairs = (argc - 1) / 2;
    bool hasDefault = (argc - 1) % 2 == 1;
    if (e.IsError()) {
      result = e;
    } else if (e.IsArray()) {
      unsupported = true;
    } else {
      result = Value::Error(Err::NA);
      bool found = false;
      for (int k = 0; k < pairs && !stop; ++k) {
        Value v = Deref(arg(1 + 2 * k), arr);
        if (v.IsError()) {
          result = v;
          found = true;
          break;
        }
        CellView x = View(e), y = View(v);
        bool eq = x.type == y.type && (x.type == Type::Text ? EqualText(x.text, y.text) : CompareValues(x, y) == 0);
        if (eq) {
          result = zeroIfMissing(arg(2 + 2 * k));
          found = true;
          break;
        }
      }
      if (!found && hasDefault) result = zeroIfMissing(arg(argc - 1));
    }
  } else if (id == kLet) {  // LET(name, value, ..., calculation): the parser made each name one kName
    size_t base = locals.size();
    for (int k = 0; k + 1 < argc && !stop; k += 2) {
      uint32_t slot = Read<uint32_t>(spans[k] + 1);
      Value v = arg(k + 1);
      locals.emplace_back(slot, std::move(v));
    }
    if (!stop) result = zeroIfMissing(arg(argc - 1));
    locals.erase(locals.begin() + static_cast<std::ptrdiff_t>(base), locals.end());
  } else {
    unsupported = true;
    result = Value::Error(Err::Name);
  }
  stack.push_back(std::move(result));
  return q;
}

// --- Evaluator ---

EvalResult Evaluator::Evaluate(const Formula& f, const Cell& at, Value& out) {
  if (f.flags & Formula::kUnsupported) return EvalResult::kUnsupported;
  stack_.clear();
  Machine m(*this, at);
  m.Run(f.code.data(), f.code.data() + f.code.size(), true);
  if (m.stop) {
    stack_.clear();
    return EvalResult::kRetry;
  }
  if (m.unsupported) {
    stack_.clear();
    return EvalResult::kUnsupported;
  }
  Value v = stack_.empty() ? Value::Error(Err::Value) : std::move(stack_.back());
  stack_.clear();
  bool array = (f.flags & Formula::kArray) != 0;
  if (v.IsRef()) v = array ? m.RefArray(v) : m.Intersect(v);
  if (m.stop) return EvalResult::kRetry;
  if (!array && v.IsArray()) v = Value(v.array().at(0, 0));
  if (v.IsEmpty() || v.IsMissing()) v = Value::Number(0);
  out = std::move(v);
  return EvalResult::kDone;
}

}  // namespace fw::sheet::calc
