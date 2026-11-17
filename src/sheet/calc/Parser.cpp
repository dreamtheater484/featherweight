// The formula parser: Excel's formula syntax (A1 or R1C1) to bytecode (Bytecode.h).
//
// Operator precedence, from loosest: comparisons, &, + -, * /, ^, %, prefix - + @, then the
// reference operators: union (",", only inside parentheses), intersection (a space), range (":").
// Excel's negation binds tighter than ^: -2^2 is 4.

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "sheet/calc/Bytecode.h"
#include "sheet/calc/Formula.h"
#include "sheet/calc/Functions.h"
#include "sheet/calc/Value.h"

namespace fw::sheet::calc {
namespace {

using namespace bc;

constexpr int kMaxDepth = 100;  // nesting; Excel allows 64 levels of functions

bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsLetter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool IsSpace(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }
bool IsNameStart(char c) { return IsLetter(c) || c == '_' || c == '\\' || static_cast<unsigned char>(c) >= 0x80; }
bool IsNameChar(char c) { return IsNameStart(c) || IsDigit(c) || c == '.' || c == '?'; }
char Upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; }

bool EqualNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (Upper(a[i]) != Upper(b[i])) return false;
  return true;
}
bool StartsNoCase(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && EqualNoCase(s.substr(0, prefix.size()), prefix);
}

// One end of a reference. Absolute parts hold the 0-based row or column, relative parts the
// offset from the formula's cell.
struct Part {
  bool hasRow = false, hasCol = false, rowAbs = false, colAbs = false;
  int32_t row = 0, col = 0;
  bool Cell() const { return hasRow && hasCol; }
  bool RowOnly() const { return hasRow && !hasCol; }
  bool ColOnly() const { return hasCol && !hasRow; }
};

// "$A$1", "A1", "$A", "A", "$1", "1" at `p`: its length, or 0. Relative parts become offsets from
// (row, col).
size_t ScanA1(std::string_view s, size_t p, int32_t row, int32_t col, Part& out) {
  Part t;
  size_t i = p, n = s.size();
  bool abs1 = i < n && s[i] == '$';
  if (abs1) ++i;
  size_t ls = i;
  int64_t c = 0;
  while (i < n && IsLetter(s[i]) && i - ls < 4) c = c * 26 + (Upper(s[i++]) - 'A' + 1);
  size_t letters = i - ls;
  if (letters > 3 || (letters && c > kMaxCols)) return 0;
  bool abs2 = abs1;
  if (letters) {
    t.hasCol = true;
    t.colAbs = abs1;
    t.col = static_cast<int32_t>(c - 1);
    abs2 = i < n && s[i] == '$';
    if (abs2) ++i;
  }
  size_t ds = i;
  int64_t r = 0;
  while (i < n && IsDigit(s[i]) && i - ds < 8) r = r * 10 + (s[i++] - '0');
  size_t digits = i - ds;
  if (digits) {
    if (digits > 7 || r < 1 || r > kMaxRows) return 0;
    t.hasRow = true;
    t.rowAbs = abs2;
    t.row = static_cast<int32_t>(r - 1);
  } else if (abs2 && letters) {
    return 0;  // "A$"
  }
  if (!letters && !digits) return 0;
  if (i < n && (IsNameChar(s[i]) || s[i] == '$')) return 0;  // part of a longer name
  if (t.hasRow && !t.rowAbs) t.row -= row;
  if (t.hasCol && !t.colAbs) t.col -= col;
  out = t;
  return i - p;
}

// "R", "R2", "R[-1]", "C", "C3", "C[2]", "RC", "R1C1", "R[1]C[-1]" at `p`: its length, or 0.
size_t ScanR1C1(std::string_view s, size_t p, Part& out) {
  Part t;
  size_t i = p, n = s.size();
  auto number = [&](bool& isAbs, int32_t& v, int32_t max) -> bool {
    isAbs = false;
    v = 0;
    if (i < n && s[i] == '[') {
      ++i;
      bool neg = false;
      if (i < n && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
      size_t ds = i;
      int64_t x = 0;
      while (i < n && IsDigit(s[i]) && i - ds < 8) x = x * 10 + (s[i++] - '0');
      if (i == ds || x >= max || i >= n || s[i] != ']') return false;
      ++i;
      v = static_cast<int32_t>(neg ? -x : x);
      return true;
    }
    size_t ds = i;
    int64_t x = 0;
    while (i < n && IsDigit(s[i]) && i - ds < 8) x = x * 10 + (s[i++] - '0');
    if (i == ds) return true;  // the formula's own row or column
    if (x < 1 || x > max) return false;
    isAbs = true;
    v = static_cast<int32_t>(x - 1);
    return true;
  };
  if (i < n && Upper(s[i]) == 'R') {
    ++i;
    t.hasRow = true;
    if (!number(t.rowAbs, t.row, kMaxRows)) return 0;
  }
  if (i < n && Upper(s[i]) == 'C') {
    ++i;
    t.hasCol = true;
    if (!number(t.colAbs, t.col, kMaxCols)) return 0;
  }
  if (!t.hasRow && !t.hasCol) return 0;
  if (i < n && (IsNameChar(s[i]) || s[i] == '$' || s[i] == '[')) return 0;
  out = t;
  return i - p;
}

class Parser {
 public:
  Parser(std::string_view text, const CompileOptions& at, Names& names, Formula& f)
      : s_(text), at_(at), names_(names), f_(f), code_(f.code), array_(at.array) {}

  bool Run(ParseError* error) {
    SkipSpace();
    if (i_ < s_.size() && s_[i_] == '=') ++i_;
    unionOk_ = true;  // a defined name can be a union: Sheet1!$A$1:$B$2,Sheet1!$D$1
    if (Expr()) {
      SkipSpace();
      if (i_ < s_.size()) Fail("unexpected text");
    }
    if (!ok_) {
      if (error) *error = err_;
      return false;
    }
    if (at_.array) f_.flags |= Formula::kArray;
    return true;
  }

 private:
  // --- emitting ---
  void Op(uint8_t o) { code_.push_back(o); }
  template <class T>
  void Put(T v) {
    size_t k = code_.size();
    code_.resize(k + sizeof v);
    std::memcpy(code_.data() + k, &v, sizeof v);
  }
  void PutStr(std::string_view s) {
    Put<uint32_t>(static_cast<uint32_t>(s.size()));
    code_.insert(code_.end(), s.begin(), s.end());
  }
  uint8_t Mode() const { return array_ ? kArrayMode : 0; }
  void Unary(UnaryOp op) {
    Op(kUnary);
    Op(static_cast<uint8_t>(op | Mode()));
  }
  void Binary(BinaryOp op) {
    Op(kBinary);
    Op(static_cast<uint8_t>(op | Mode()));
  }

  bool Fail(const char* message) {
    if (ok_) {
      ok_ = false;
      err_.message = message;
      err_.pos = i_ < s_.size() ? i_ : s_.size();
    }
    return false;
  }

  // --- reading ---
  void SkipSpace() {
    while (i_ < s_.size() && IsSpace(s_[i_])) ++i_;
  }
  char At(size_t k) const { return k < s_.size() ? s_[k] : '\0'; }

  // --- expressions ---
  bool Expr() { return BinaryExpr(1); }

  static int Prec(BinaryOp op) {
    switch (op) {
      case kEq: case kNe: case kLt: case kLe: case kGt: case kGe: return 1;
      case kConcat: return 2;
      case kAdd: case kSub: return 3;
      case kMul: case kDiv: return 4;
      case kPow: return 5;
      default: return 0;
    }
  }
  bool PeekBinary(BinaryOp& op, size_t& len) const {
    char c = At(i_), d = At(i_ + 1);
    len = 1;
    switch (c) {
      case '=': op = kEq; return true;
      case '<':
        if (d == '>') op = kNe, len = 2;
        else if (d == '=') op = kLe, len = 2;
        else op = kLt;
        return true;
      case '>':
        if (d == '=') op = kGe, len = 2;
        else op = kGt;
        return true;
      case '&': op = kConcat; return true;
      case '+': op = kAdd; return true;
      case '-': op = kSub; return true;
      case '*': op = kMul; return true;
      case '/': op = kDiv; return true;
      case '^': op = kPow; return true;
      default: return false;
    }
  }

  bool BinaryExpr(int minPrec) {
    if (!PercentExpr()) return false;
    for (;;) {
      SkipSpace();
      BinaryOp op;
      size_t len;
      if (!PeekBinary(op, len) || Prec(op) < minPrec) return true;
      i_ += len;
      if (!BinaryExpr(Prec(op) + 1)) return false;  // all left-associative, ^ too
      Binary(op);
    }
  }

  bool PercentExpr() {
    if (!PrefixExpr()) return false;
    for (;;) {
      size_t k = i_;
      while (k < s_.size() && IsSpace(s_[k])) ++k;
      if (At(k) != '%') return true;
      i_ = k + 1;
      Unary(kPercent);
    }
  }

  bool PrefixExpr() {
    SkipSpace();
    char c = At(i_);
    if (c == '-' || c == '+' || c == '@') {
      if (++depth_ > kMaxDepth) return Fail("nested too deeply");
      ++i_;
      bool ok = c == '@' ? RangeExpr() : PrefixExpr();
      --depth_;
      if (!ok) return false;
      Unary(c == '-' ? kNeg : c == '+' ? kPlus : kImplicit);
      return true;
    }
    return UnionExpr();
  }

  bool UnionExpr() {
    if (!IsectExpr()) return false;
    for (;;) {
      if (!unionOk_) return true;
      size_t k = i_;
      while (k < s_.size() && IsSpace(s_[k])) ++k;
      if (At(k) != ',') return true;
      i_ = k + 1;
      if (!IsectExpr()) return false;
      Binary(kUnion);
    }
  }

  static bool StartsOperand(char c) {
    return IsNameStart(c) || IsDigit(c) || c == '$' || c == '\'' || c == '(' || c == '[' || c == '#';
  }

  bool IsectExpr() {
    if (!RangeExpr()) return false;
    for (;;) {
      size_t k = i_;
      while (k < s_.size() && IsSpace(s_[k])) ++k;
      if (k == i_ || !StartsOperand(At(k))) return true;
      i_ = k;
      if (!RangeExpr()) return false;
      Binary(kIsect);
    }
  }

  bool RangeExpr() {
    if (!PostfixExpr()) return false;
    while (At(i_) == ':') {
      ++i_;
      if (!PostfixExpr()) return false;
      Binary(kRange);
    }
    return true;
  }

  bool PostfixExpr() {
    if (!Primary()) return false;
    // A1#: the spill range of A1's dynamic array (an error literal can't follow an operand).
    while (At(i_) == '#' && !(At(i_ + 1) && (IsLetter(At(i_ + 1)) || At(i_ + 1) == '/'))) {
      ++i_;
      Unary(kSpill);
      f_.flags |= Formula::kSpills;
    }
    return true;
  }

  bool Primary() {
    if (++depth_ > kMaxDepth) return Fail("nested too deeply");
    bool ok = PrimaryInner();
    --depth_;
    return ok;
  }

  bool PrimaryInner() {
    SkipSpace();
    if (i_ >= s_.size()) return Fail("operand expected");
    char c = s_[i_];
    if (c == '(') {
      ++i_;
      bool saveUnion = unionOk_;
      unionOk_ = true;
      bool ok = Expr();
      unionOk_ = saveUnion;
      if (!ok) return false;
      SkipSpace();
      if (At(i_) != ')') return Fail("')' expected");
      ++i_;
      Op(kParen);
      return true;
    }
    if (c == '"') {
      std::string text;
      if (!StringLit(text)) return false;
      Op(kStr);
      PutStr(text);
      return true;
    }
    if (c == '{') return ArrayConst();
    if (c == '#') {
      size_t len = 0;
      Err e = ParseErr(s_.substr(i_), &len);
      if (e == Err::None) return Fail("unknown error value");
      i_ += len;
      Op(kErr);
      Op(static_cast<uint8_t>(e));
      return true;
    }
    if (IsDigit(c) || c == '.') {
      // 1:3, whole rows
      Part a, b;
      size_t la = at_.r1c1 ? 0 : ScanA1(s_, i_, at_.row, at_.col, a);
      if (la && a.RowOnly() && At(i_ + la) == ':') {
        size_t lb = ScanA1(s_, i_ + la + 1, at_.row, at_.col, b);
        if (lb && b.RowOnly()) {
          i_ += la + 1 + lb;
          EmitArea(a, b, 0, 0, 0);
          return true;
        }
      }
      return NumberLit();
    }
    if (c == '$' || c == '\'' || c == '[' || IsNameStart(c)) return Word();
    return Fail("unexpected character");
  }

  bool StringLit(std::string& out) {  // at the opening quote
    ++i_;
    for (;;) {
      if (i_ >= s_.size()) return Fail("unterminated text");
      char c = s_[i_++];
      if (c == '"') {
        if (At(i_) == '"') {
          out += '"';
          ++i_;
          continue;
        }
        return true;
      }
      out += c;
    }
  }

  bool ScanNumber(double& v) {
    size_t start = i_, n = s_.size();
    while (i_ < n && IsDigit(s_[i_])) ++i_;
    if (i_ < n && s_[i_] == '.') {
      ++i_;
      while (i_ < n && IsDigit(s_[i_])) ++i_;
    }
    if (i_ == start || (i_ == start + 1 && s_[start] == '.')) return Fail("number expected");
    if (i_ < n && (s_[i_] == 'e' || s_[i_] == 'E')) {
      size_t k = i_ + 1;
      if (k < n && (s_[k] == '+' || s_[k] == '-')) ++k;
      if (k < n && IsDigit(s_[k])) {
        while (k < n && IsDigit(s_[k])) ++k;
        i_ = k;
      }
    }
    if (i_ - start > 400) return Fail("number too long");
    // Excel keeps 15 significant digits of a literal and drops the rest (truncating).
    char buf[408];
    size_t k = 0, sig = 0;
    bool point = false, mantissa = true;
    for (size_t j = start; j < i_; ++j) {
      char c = s_[j];
      if (c == 'e' || c == 'E') mantissa = false;
      if (mantissa && c == '.') point = true;
      if (mantissa && IsDigit(c)) {
        if (sig > 0 || c != '0') ++sig;
        if (sig > 15) {
          if (point) continue;
          c = '0';
        }
      }
      buf[k++] = c;
    }
    buf[k] = '\0';
    v = std::strtod(buf, nullptr);
    if (!(v - v == 0)) return Fail("number out of range");  // inf
    return true;
  }

  bool NumberLit() {
    double v;
    if (!ScanNumber(v)) return false;
    Op(kNum);
    Put(v);
    return true;
  }

  bool ArrayConst() {  // at '{'
    ++i_;
    std::vector<uint8_t> items;
    int rows = 0, cols = 0, col = 0;
    for (;;) {
      SkipSpace();
      char c = At(i_);
      // one element
      if (c == '"') {
        std::string text;
        if (!StringLit(text)) return false;
        items.push_back(kStr);
        uint32_t len = static_cast<uint32_t>(text.size());
        items.insert(items.end(), reinterpret_cast<uint8_t*>(&len), reinterpret_cast<uint8_t*>(&len) + 4);
        items.insert(items.end(), text.begin(), text.end());
      } else if (c == '#') {
        size_t len = 0;
        Err e = ParseErr(s_.substr(i_), &len);
        if (e == Err::None) return Fail("unknown error value");
        i_ += len;
        items.push_back(kErr);
        items.push_back(static_cast<uint8_t>(e));
      } else if (c == '-' || c == '+' || IsDigit(c) || c == '.') {
        bool neg = c == '-';
        if (c == '-' || c == '+') ++i_;
        SkipSpace();
        double v;
        if (!ScanNumber(v)) return false;
        if (neg) v = -v;
        items.push_back(kNum);
        items.insert(items.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 8);
      } else if (IsLetter(c)) {
        size_t e = i_;
        while (e < s_.size() && IsLetter(s_[e])) ++e;
        std::string_view w = s_.substr(i_, e - i_);
        if (!EqualNoCase(w, "TRUE") && !EqualNoCase(w, "FALSE")) return Fail("constant expected in the array");
        items.push_back(kBool);
        items.push_back(EqualNoCase(w, "TRUE") ? 1 : 0);
        i_ = e;
      } else {
        return Fail("constant expected in the array");
      }
      ++col;
      SkipSpace();
      c = At(i_);
      if (c == ',') {
        ++i_;
        continue;
      }
      if (c == ';' || c == '}') {
        if (rows == 0) cols = col;
        else if (col != cols) return Fail("array rows differ in length");
        ++rows;
        col = 0;
        ++i_;
        if (c == '}') break;
        continue;
      }
      return Fail("',', ';' or '}' expected");
    }
    if (rows > 65535 || cols > 65535) return Fail("array too large");
    Op(kArray);
    Put<uint16_t>(static_cast<uint16_t>(rows));
    Put<uint16_t>(static_cast<uint16_t>(cols));
    code_.insert(code_.end(), items.begin(), items.end());
    return true;
  }

  // --- names, references, functions ---

  void EmitSheets(uint8_t flags, int s1, int s2) {
    if (flags & kSheet) Put<uint16_t>(static_cast<uint16_t>(s1));
    if (flags & kSheet3D) Put<uint16_t>(static_cast<uint16_t>(s2));
  }

  void EmitRef(const Part& a, uint8_t sheetFlags, int s1, int s2) {
    uint8_t flags = sheetFlags;
    if (a.rowAbs) flags |= kRowAbs;
    if (a.colAbs) flags |= kColAbs;
    Op(kRef);
    Op(flags);
    EmitSheets(flags, s1, s2);
    Put<int32_t>(a.row);
    Put<int16_t>(static_cast<int16_t>(a.col));
  }

  void EmitArea(const Part& a, const Part& b, uint8_t sheetFlags, int s1, int s2) {
    uint8_t flags = sheetFlags;
    int32_t r1 = a.row, r2 = b.row, c1 = a.col, c2 = b.col;
    if (a.ColOnly()) {  // A:B
      flags |= kWholeCols | kRowAbs | kRow2Abs;
      r1 = 0;
      r2 = kMaxRows - 1;
    } else if (a.RowOnly()) {  // 1:2
      flags |= kWholeRows | kColAbs | kCol2Abs;
      c1 = 0;
      c2 = kMaxCols - 1;
    }
    if (a.hasRow && a.rowAbs) flags |= kRowAbs;
    if (b.hasRow && b.rowAbs) flags |= kRow2Abs;
    if (a.hasCol && a.colAbs) flags |= kColAbs;
    if (b.hasCol && b.colAbs) flags |= kCol2Abs;
    Op(kArea);
    Op(flags);
    EmitSheets(flags, s1, s2);
    Put<int32_t>(r1);
    Put<int16_t>(static_cast<int16_t>(c1));
    Put<int32_t>(r2);
    Put<int16_t>(static_cast<int16_t>(c2));
  }

  size_t ScanPart(size_t p, Part& out) const {
    return at_.r1c1 ? ScanR1C1(s_, p, out) : ScanA1(s_, p, at_.row, at_.col, out);
  }

  // A reference starting with `a` (length `la` at i_), with its second end if ":" follows. False
  // (nothing consumed) when it isn't a reference on its own: in A1, "A" or "1" alone.
  bool TryReference(const Part& a, size_t la, uint8_t sheetFlags, int s1, int s2) {
    if (At(i_ + la) == ':') {
      Part b;
      size_t lb = ScanPart(i_ + la + 1, b);
      if (lb && ((a.Cell() && b.Cell()) || (a.RowOnly() && b.RowOnly()) || (a.ColOnly() && b.ColOnly()))) {
        i_ += la + 1 + lb;
        EmitArea(a, b, sheetFlags, s1, s2);
        return true;
      }
    }
    if (a.Cell()) {
      i_ += la;
      EmitRef(a, sheetFlags, s1, s2);
      return true;
    }
    if (at_.r1c1) {  // R2 is row 2, C[1] the column to the right
      i_ += la;
      EmitArea(a, a, sheetFlags, s1, s2);
      return true;
    }
    return false;
  }

  // After "Sheet!" (i_ past the "!"): the reference or name on that sheet.
  bool SheetTarget(uint8_t sheetFlags, int s1, int s2) {
    char c = At(i_);
    if (c == '#') {
      size_t len = 0;
      if (ParseErr(s_.substr(i_), &len) != Err::Ref) return Fail("reference expected after the sheet");
      i_ += len;
      Op(kRefErr);
      Op(sheetFlags);
      EmitSheets(sheetFlags, s1, s2);
      return true;
    }
    Part a;
    size_t la = ScanPart(i_, a);
    if (la == 0 && IsDigit(c) && !at_.r1c1) return Fail("reference expected after the sheet");
    if (la && TryReference(a, la, sheetFlags, s1, s2)) return true;
    // A name on that sheet.
    size_t e = i_;
    while (e < s_.size() && IsNameChar(s_[e])) ++e;
    if (e == i_ || !IsNameStart(s_[i_])) return Fail("reference expected after the sheet");
    if (sheetFlags & kSheet3D) return Fail("a name can't be on several sheets");
    std::string_view name = s_.substr(i_, e - i_);
    i_ = e;
    Op(kName);
    Put<uint32_t>(names_.NameSlot(name));
    Op(sheetFlags);
    EmitSheets(sheetFlags, s1, s2);
    f_.flags |= Formula::kNames;
    return true;
  }

  // A prefix's sheets: one name or "first:last". Unknown sheets, or another workbook, make the
  // reference kept as text.
  bool SheetPrefix(std::string_view first, std::string_view last, bool external, size_t start) {
    int s1 = 0, s2 = 0;
    bool known = !external;
    if (known) {
      s1 = names_.FindSheet(first);
      s2 = last.empty() ? s1 : names_.FindSheet(last);
      known = s1 >= 0 && s2 >= 0;
      if (s2 < s1) std::swap(s1, s2);
    }
    uint8_t flags = kSheet;
    if (!last.empty()) flags |= kSheet3D;
    size_t mark = code_.size();
    if (!SheetTarget(flags, known ? s1 : 0, known ? s2 : 0)) return false;
    if (!known) {
      code_.resize(mark);
      Op(kExtern);
      PutStr(s_.substr(start, i_ - start));
      f_.flags |= Formula::kUnsupported;
    }
    return true;
  }

  bool Word() {
    size_t start = i_, n = s_.size();
    char c = s_[i_];
    if (c == '\'') {  // 'My sheet'!A1, 'Jan:Mar'!A1, '[1]Sheet1'!A1, 'C:\dir\[Book.xlsx]Sheet1'!A1
      std::string name;
      ++i_;
      for (;;) {
        if (i_ >= n) return Fail("unterminated sheet name");
        char d = s_[i_++];
        if (d == '\'') {
          if (At(i_) == '\'') {
            name += '\'';
            ++i_;
            continue;
          }
          break;
        }
        name += d;
      }
      if (At(i_) != '!') return Fail("'!' expected after the sheet name");
      ++i_;
      bool external = !name.empty() && (name[0] == '[' || name.find(']') != std::string::npos);
      if (external && name.size() > 2 && name[0] == '[' && name[1] == '0' && name[2] == ']') {
        name.erase(0, 3);  // [0] is this workbook
        external = false;
      }
      size_t colon = external ? std::string::npos : name.find(':');
      std::string_view nv = name;
      if (colon != std::string::npos) return SheetPrefix(nv.substr(0, colon), nv.substr(colon + 1), false, start);
      return SheetPrefix(nv, {}, external, start);
    }
    if (c == '[') {
      // [1]Sheet1!A1, [1]!Name, [Book.xlsx]Sheet1!A1: another workbook. Else a structured
      // reference inside a table: [@Col], [Col].
      size_t close = s_.find(']', i_);
      if (close != std::string_view::npos) {
        size_t e = close + 1;
        bool isBook = At(e) == '!';
        if (!isBook) {
          size_t k = e;
          while (k < n && IsNameChar(s_[k])) ++k;
          isBook = k > e && At(k) == '!' && IsNameStart(s_[e]);
          if (isBook) e = k;
        }
        if (isBook) {
          std::string_view book = s_.substr(i_ + 1, close - i_ - 1);
          bool local = book == "0";
          std::string_view sheet = s_.substr(close + 1, e - close - 1);
          i_ = e + 1;
          if (sheet.empty()) {  // [1]!Name: a name in that workbook
            size_t k = i_;
            while (k < n && IsNameChar(s_[k])) ++k;
            if (k == i_) return Fail("name expected");
            std::string_view name = s_.substr(i_, k - i_);
            i_ = k;
            if (local) {
              Op(kName);
              Put<uint32_t>(names_.NameSlot(name));
              Op(0);
              f_.flags |= Formula::kNames;
            } else {
              Op(kExtern);
              PutStr(s_.substr(start, i_ - start));
              f_.flags |= Formula::kUnsupported;
            }
            return true;
          }
          return SheetPrefix(sheet, {}, !local, start);
        }
      }
      return TableRef({}, start);
    }

    // R1C1 references ("R[1]C" contains brackets, so before the word).
    if (at_.r1c1 && (Upper(c) == 'R' || Upper(c) == 'C')) {
      Part a;
      size_t la = ScanR1C1(s_, i_, a);
      if (la && At(i_ + la) != '(' && At(i_ + la) != '!' && TryReference(a, la, 0, 0, 0)) return true;
    }

    size_t e = i_;
    while (e < n && (IsNameChar(s_[e]) || s_[e] == '$')) ++e;
    std::string_view w = s_.substr(i_, e - i_);
    char next = At(e);
    bool dollar = w.find('$') != std::string_view::npos;
    if (next == '(' && !dollar) {
      i_ = e;
      return CallExpr(w);
    }
    if (next == '!' && !dollar) {
      i_ = e + 1;
      return SheetPrefix(w, {}, false, start);
    }
    if (next == ':' && !dollar) {  // Sheet1:Sheet3!A1
      size_t k = e + 1;
      while (k < n && IsNameChar(s_[k])) ++k;
      if (k > e + 1 && At(k) == '!') {
        std::string_view last = s_.substr(e + 1, k - e - 1);
        i_ = k + 1;
        return SheetPrefix(w, last, false, start);
      }
    }
    if (next == '[' && !dollar) {
      i_ = e;
      return TableRef(w, start);
    }
    Part a;
    size_t la = ScanPart(i_, a);
    if (la && TryReference(a, la, 0, 0, 0)) return true;
    if (dollar || !IsNameStart(c)) return Fail("not a valid reference or name");
    i_ = e;
    if (EqualNoCase(w, "TRUE") || EqualNoCase(w, "FALSE")) {
      Op(kBool);
      Op(EqualNoCase(w, "TRUE") ? 1 : 0);
      return true;
    }
    for (size_t k = let_.size(); k-- > 0;) {  // one of LET's names
      if (EqualNoCase(let_[k], w)) {
        Op(kName);
        Put<uint32_t>(names_.NameSlot("_xlpm." + let_[k]));
        Op(0);
        return true;
      }
    }
    Op(kName);
    Put<uint32_t>(names_.NameSlot(w));
    Op(0);
    f_.flags |= Formula::kNames;
    return true;
  }

  // LET's name arguments: a name followed by ','. Each becomes one kName for "_xlpm.name" (as files
  // write them), and is in scope for the arguments after it.
  bool LetName() {
    size_t e = i_;
    if (e >= s_.size() || !IsNameStart(s_[e])) return false;
    while (e < s_.size() && IsNameChar(s_[e])) ++e;
    size_t k = e;
    while (k < s_.size() && s_[k] == ' ') ++k;
    if (At(k) != ',') return false;
    std::string_view w = s_.substr(i_, e - i_);
    if (StartsNoCase(w, "_xlpm.")) w.remove_prefix(6);
    if (w.empty() || !IsNameStart(w[0]) || EqualNoCase(w, "TRUE") || EqualNoCase(w, "FALSE")) return false;
    Part a;
    if (ScanPart(i_, a) == e - i_ && (a.Cell() || at_.r1c1)) return false;  // A1, R1C1: not names
    i_ = e;
    Op(kName);
    Put<uint32_t>(names_.NameSlot("_xlpm." + std::string(w)));
    Op(0);
    let_.emplace_back(w);
    return true;
  }

  // Table1[Col], Table1[[#This Row],[Col]], Table1[[Col1]:[Col2]], Table1[#All], Table1[].
  bool TableRef(std::string_view table, size_t /*start*/) {  // at '['
    uint8_t items = 0;
    std::string cols[2];
    int ncols = 0;
    auto item = [&](std::string_view content) -> bool {
      // content is unescaped already
      if (!content.empty() && content[0] == '#') {
        if (EqualNoCase(content, "#All")) items |= kAll;
        else if (EqualNoCase(content, "#Data")) items |= kData;
        else if (EqualNoCase(content, "#Headers")) items |= kHeaders;
        else if (EqualNoCase(content, "#Totals")) items |= kTotals;
        else if (EqualNoCase(content, "#This Row")) items |= kThisRow;
        else return Fail("unknown table item");
        return true;
      }
      if (!content.empty() && content[0] == '@') {  // [@Col], the formula bar's #This Row
        items |= kThisRow;
        content.remove_prefix(1);
        if (content.empty()) return true;
      }
      if (ncols >= 2) return Fail("too many table columns");
      cols[ncols++] = std::string(content);
      return true;
    };
    ++i_;  // the outer '['
    SkipSpace();
    if (At(i_) == ']') {
      ++i_;
    } else if (At(i_) == '[' || (At(i_) == '@' && At(i_ + 1) == '[')) {
      bool at = At(i_) == '@';
      if (at) {
        items |= kThisRow;
        ++i_;
      }
      bool range = false;
      for (;;) {
        SkipSpace();
        if (At(i_) != '[') return Fail("'[' expected");
        ++i_;
        std::string content;
        if (!readItemInner(content)) return false;
        if (range && (content.empty() || content[0] == '#')) return Fail("column expected");
        if (!item(content)) return false;
        SkipSpace();
        char d = At(i_);
        if (d == ':') {
          if (range) return Fail("']' expected");
          range = true;
          ++i_;
          continue;
        }
        if (d == ',' && !range) {
          ++i_;
          continue;
        }
        if (d == ']') {
          ++i_;
          break;
        }
        return Fail("']' expected");
      }
    } else {
      std::string content;
      if (!readItemInner(content)) return false;
      if (!item(content)) return false;
    }
    Op(kTable);
    Op(items);
    PutStr(table);
    PutStr(cols[0]);
    PutStr(cols[1]);
    f_.flags |= Formula::kTables;
    return true;
  }

  // An item's text up to its closing ']' (i_ just past its '['), with ' escapes undone.
  bool readItemInner(std::string& out) {
    for (;;) {
      if (i_ >= s_.size()) return Fail("']' expected");
      char d = s_[i_++];
      if (d == '\'') {
        if (i_ >= s_.size()) return Fail("']' expected");
        out += s_[i_++];
        continue;
      }
      if (d == ']') return true;
      if (d == '[') return Fail("unexpected '['");
      out += d;
    }
  }

  bool CallExpr(std::string_view written) {  // i_ at '('
    std::string_view name = written;
    for (;;) {
      if (StartsNoCase(name, "_xlfn.")) name.remove_prefix(6);
      else if (StartsNoCase(name, "_xlws.")) name.remove_prefix(6);
      else break;
    }
    if (EqualNoCase(name, "SINGLE") || EqualNoCase(name, "ANCHORARRAY")) {  // @x and x#
      bool spill = EqualNoCase(name, "ANCHORARRAY");
      ++i_;
      bool saveUnion = unionOk_;
      unionOk_ = false;
      bool ok = Expr();
      unionOk_ = saveUnion;
      if (!ok) return false;
      SkipSpace();
      if (At(i_) != ')') return Fail("')' expected");
      ++i_;
      Unary(spill ? kSpill : kImplicit);
      if (spill) f_.flags |= Formula::kSpills;
      return true;
    }
    int id = FindFunction(name);
    const FuncInfo* fn = id >= 0 ? &Function(id) : nullptr;
    bool lazy = fn && (fn->flags & kFnLazy);
    bool let = fn && EqualNoCase(name, "LET");
    size_t letScope = let_.size();
    int letNames = 0;
    size_t argsStart = code_.size();
    std::vector<size_t> starts;
    ++i_;
    int argc = 0;
    SkipSpace();
    if (At(i_) == ')') {
      ++i_;
    } else {
      for (;;) {
        SkipSpace();
        starts.push_back(code_.size());
        char d = At(i_);
        if (let && argc % 2 == 0 && LetName()) {
          ++letNames;
        } else if (d == ',' || d == ')') {
          Op(kMissing);
        } else {
          bool saveArray = array_, saveUnion = unionOk_;
          if (fn && ParamClass(*fn, argc) == 'A') array_ = true;
          unionOk_ = false;
          bool ok = Expr();
          array_ = saveArray;
          unionOk_ = saveUnion;
          if (!ok) return false;
        }
        if (++argc > 255) return Fail("too many arguments");
        SkipSpace();
        d = At(i_);
        if (d == ',') {
          ++i_;
          continue;
        }
        if (d == ')') {
          ++i_;
          break;
        }
        return Fail("',' or ')' expected");
      }
    }
    if (let) {
      let_.resize(letScope);
      if (argc % 2 == 0 || letNames != argc / 2) return Fail("LET wants names with values, then a calculation");
    }
    if (!fn) {
      Op(kUnknown);
      Op(static_cast<uint8_t>(argc));
      Op(array_ ? 1 : 0);
      PutStr(written);
      f_.flags |= Formula::kUnsupported;
      return true;
    }
    if (argc < fn->minArgs || argc > fn->maxArgs) return Fail("wrong number of arguments");
    if (fn->flags & kFnVolatile) f_.flags |= Formula::kVolatile;
    if (EqualNoCase(name, "SUBTOTAL")) f_.flags |= Formula::kSubtotal;
    if (fn->flags & kFnDynRef) f_.flags |= Formula::kDynamicRefs;
    if (!fn->impl && !lazy) f_.flags |= Formula::kUnsupported;
    if (lazy) {
      std::vector<uint8_t> head;
      head.push_back(kLazy);
      head.push_back(static_cast<uint8_t>(id & 0xFF));
      head.push_back(static_cast<uint8_t>(id >> 8));
      head.push_back(static_cast<uint8_t>(argc));
      head.push_back(array_ ? 1 : 0);
      starts.push_back(code_.size());
      for (int k = 0; k < argc; ++k) {
        uint32_t len = static_cast<uint32_t>(starts[k + 1] - starts[k]);
        head.insert(head.end(), reinterpret_cast<uint8_t*>(&len), reinterpret_cast<uint8_t*>(&len) + 4);
      }
      code_.insert(code_.begin() + static_cast<std::ptrdiff_t>(argsStart), head.begin(), head.end());
      return true;
    }
    Op(kCall);
    Put<uint16_t>(static_cast<uint16_t>(id));
    Op(static_cast<uint8_t>(argc));
    Op(array_ ? 1 : 0);
    return true;
  }

  std::string_view s_;
  size_t i_ = 0;
  const CompileOptions& at_;
  Names& names_;
  Formula& f_;
  std::vector<uint8_t>& code_;
  std::vector<std::string> let_;  // LET's names in scope, innermost last
  bool array_ = false;    // evaluating in an array context
  bool unionOk_ = false;  // "," is the union operator here (inside parentheses)
  int depth_ = 0;
  bool ok_ = true;
  ParseError err_;
};

}  // namespace

std::shared_ptr<const Formula> Compile(std::string_view text, const CompileOptions& at, Names& names,
                                       ParseError* error) {
  auto f = std::make_shared<Formula>();
  Parser p(text, at, names, *f);
  if (!p.Run(error)) return nullptr;
  f->code.shrink_to_fit();
  return f;
}

}  // namespace fw::sheet::calc
