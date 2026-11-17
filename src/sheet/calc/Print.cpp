// Bytecode back to formula text, A1 or R1C1, as files store it or as the formula bar shows it.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "pkg/Formula.h"
#include "sheet/calc/Bytecode.h"
#include "sheet/calc/Formula.h"
#include "sheet/calc/Functions.h"
#include "sheet/calc/Value.h"

namespace fw::sheet::calc {

namespace bc {

size_t InstrLength(const uint8_t* p) {
  size_t len;
  switch (p[0]) {
    case kNum: return 9;
    case kStr: return 1 + 4 + Read<uint32_t>(p + 1);
    case kBool: case kErr: return 2;
    case kMissing: case kParen: case kEnd: return 1;
    case kArray: {
      uint32_t n = static_cast<uint32_t>(Read<uint16_t>(p + 1)) * Read<uint16_t>(p + 3);
      size_t k = 5;
      for (uint32_t i = 0; i < n; ++i) {
        switch (p[k]) {
          case kNum: k += 9; break;
          case kStr: k += 5 + Read<uint32_t>(p + k + 1); break;
          default: k += 2; break;
        }
      }
      return k;
    }
    case kRef: return 2 + SheetBytes(p[1]) + 6;
    case kArea: return 2 + SheetBytes(p[1]) + 12;
    case kRefErr: return 2 + SheetBytes(p[1]);
    case kName: return 6 + ((p[5] & kSheet) ? 2 : 0);
    case kExtern: return 1 + 4 + Read<uint32_t>(p + 1);
    case kTable: {
      size_t k = 2;
      for (int i = 0; i < 3; ++i) {
        ReadStr(p + k, &len);
        k += len;
      }
      return k;
    }
    case kUnary: case kBinary: return 2;
    case kCall: return 5;
    case kLazy: return 5 + 4 * static_cast<size_t>(p[3]);
    case kUnknown: return 3 + 4 + Read<uint32_t>(p + 3);
    default: return 1;
  }
}

}  // namespace bc

std::string ColumnName(int32_t col) {
  std::string s;
  int32_t c = col + 1;
  while (c > 0) {
    int32_t r = (c - 1) % 26;
    s.insert(s.begin(), static_cast<char>('A' + r));
    c = (c - 1) / 26;
  }
  return s;
}

namespace {

using namespace bc;

std::string NumberLiteral(double v) {
  // Literals have at most 15 significant digits (the parser keeps no more), written as Excel
  // writes numbers; a computed value with more keeps them all.
  std::string t = NumberText(v);
  if (std::strtod(t.c_str(), nullptr) == v) return t;
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  std::string s = buf;
  // Excel writes 1E+20, not 1e+20.
  for (char& c : s)
    if (c == 'e') c = 'E';
  return s;
}

std::string StringLiteral(std::string_view t) {
  std::string s = "\"";
  for (char c : t) {
    if (c == '"') s += '"';
    s += c;
  }
  return s + "\"";
}

int32_t Wrap(int64_t v, int32_t max) {
  int64_t r = v % max;
  return static_cast<int32_t>(r < 0 ? r + max : r);
}

class Printer {
 public:
  Printer(const CompileOptions& at, const Names& names, const PrintOptions& how)
      : at_(at), names_(names), how_(how) {}

  std::string Run(const Formula& f) {
    const uint8_t* code = f.code.data();
    size_t n = f.code.size(), i = 0;
    while (i < n) {
      const uint8_t* p = code + i;
      size_t len = InstrLength(p);
      if (i + len > n) break;
      if (p[0] == kLazy) {  // its arguments follow; the call completes after the last one
        size_t total = 0;
        for (int k = 0; k < p[3]; ++k) total += Read<uint32_t>(p + 5 + 4 * k);
        lazy_.push_back({i + len + total, Read<uint16_t>(p + 1), p[3]});
      } else {
        Instr(p);
      }
      i += len;
      while (!lazy_.empty() && lazy_.back().end == i) {
        Pending c = lazy_.back();
        lazy_.pop_back();
        PushCall(Function(c.fn).name, Function(c.fn).flags, c.argc);
      }
    }
    return stack_.empty() ? std::string() : stack_.back();
  }

 private:
  struct Pending {
    size_t end;
    uint16_t fn;
    int argc;
  };

  std::string Pop() {
    if (stack_.empty()) return {};
    std::string s = std::move(stack_.back());
    stack_.pop_back();
    return s;
  }

  std::string Sheets(const uint8_t* p, uint8_t flags) const {
    if (!(flags & kSheet)) return {};
    int s1 = Read<uint16_t>(p);
    int s2 = (flags & kSheet3D) ? Read<uint16_t>(p + 2) : s1;
    if (how_.map) {
      if (!how_.map->MapSheets(s1, s2)) return "#REF!";
      if (s1 == s2) flags = static_cast<uint8_t>(flags & ~kSheet3D);
      else flags |= kSheet3D;
    }
    std::string a = names_.SheetName(s1);
    if (flags & kSheet3D) {
      std::string b = names_.SheetName(s2);
      bool quote = pkg::QuoteSheetName(a) != a || pkg::QuoteSheetName(b) != b;
      return (quote ? pkg::QuoteSheetName(a + ":" + b) : a + ":" + b) + "!";
    }
    return pkg::QuoteSheetName(a) + "!";
  }

  // The sheet a reference is on: its own, or the formula's. -1 for several sheets.
  int RefSheet(const uint8_t* p, uint8_t flags) const {
    if (!(flags & kSheet)) return at_.sheet;
    if ((flags & kSheet3D) && Read<uint16_t>(p) != Read<uint16_t>(p + 2)) return -1;
    return Read<uint16_t>(p);
  }

  std::string RowText(int32_t v, bool abs) const {
    if (how_.r1c1) {
      if (abs) return "R" + std::to_string(v + 1);
      return v == 0 ? "R" : "R[" + std::to_string(v) + "]";
    }
    int32_t r = abs ? v : Wrap(static_cast<int64_t>(at_.row) + v, kMaxRows);
    return (abs ? "$" : "") + std::to_string(r + 1);
  }
  std::string ColText(int32_t v, bool abs) const {
    if (how_.r1c1) {
      if (abs) return "C" + std::to_string(v + 1);
      return v == 0 ? "C" : "C[" + std::to_string(v) + "]";
    }
    int32_t c = abs ? v : Wrap(static_cast<int64_t>(at_.col) + v, kMaxCols);
    return (abs ? "$" : "") + ColumnName(c);
  }
  // A relative part off the sheet at the cell printed for, when that prints #REF!.
  bool Off(int32_t v, bool abs, int32_t at, int32_t max) const {
    if (!how_.offSheetRef || how_.r1c1 || abs) return false;
    int64_t x = static_cast<int64_t>(at) + v;
    return x < 0 || x >= max;
  }

  std::string CellText(int32_t r, bool rAbs, int32_t c, bool cAbs) const {
    if (how_.r1c1) return RowText(r, rAbs) + ColText(c, cAbs);
    return ColText(c, cAbs) + RowText(r, rAbs);
  }

  void PushCall(std::string_view name, uint8_t fnFlags, int argc) {
    std::vector<std::string> args(static_cast<size_t>(argc));
    for (int k = argc; k-- > 0;) args[static_cast<size_t>(k)] = Pop();
    std::string s;
    if (how_.file && (fnFlags & kFnXlws)) s = "_xlfn._xlws.";
    else if (how_.file && (fnFlags & kFnXlfn)) s = "_xlfn.";
    s += name;
    s += '(';
    for (int k = 0; k < argc; ++k) {
      if (k) s += ',';
      s += args[static_cast<size_t>(k)];
    }
    s += ')';
    stack_.push_back(std::move(s));
  }

  std::string ArrayItem(const uint8_t* p, size_t& len) const {
    switch (p[0]) {
      case kNum: len = 9; return NumberLiteral(Read<double>(p + 1));
      case kStr: {
        size_t l;
        std::string_view t = ReadStr(p + 1, &l);
        len = 1 + l;
        return StringLiteral(t);
      }
      case kBool: len = 2; return p[1] ? "TRUE" : "FALSE";
      default: len = 2; return std::string(ErrText(static_cast<Err>(p[1])));
    }
  }

  static std::string TableEscape(std::string_view t) {
    std::string s;
    for (char c : t) {
      if (c == '[' || c == ']' || c == '#' || c == '\'') s += '\'';
      s += c;
    }
    return s;
  }

  void Instr(const uint8_t* p) {
    size_t len;
    switch (p[0]) {
      case kNum: stack_.push_back(NumberLiteral(Read<double>(p + 1))); break;
      case kStr: stack_.push_back(StringLiteral(ReadStr(p + 1, &len))); break;
      case kBool: stack_.push_back(p[1] ? "TRUE" : "FALSE"); break;
      case kErr: stack_.push_back(std::string(ErrText(static_cast<Err>(p[1])))); break;
      case kMissing: stack_.push_back({}); break;
      case kParen: stack_.push_back("(" + Pop() + ")"); break;
      case kArray: {
        int rows = Read<uint16_t>(p + 1), cols = Read<uint16_t>(p + 3);
        std::string s = "{";
        size_t k = 5;
        for (int r = 0; r < rows; ++r) {
          if (r) s += ';';
          for (int c = 0; c < cols; ++c) {
            if (c) s += ',';
            s += ArrayItem(p + k, len);
            k += len;
          }
        }
        stack_.push_back(s + "}");
        break;
      }
      case kRef: {
        uint8_t fl = p[1];
        const uint8_t* q = p + 2 + SheetBytes(fl);
        int32_t r = Read<int32_t>(q), c = Read<int16_t>(q + 4);
        if (how_.map) {
          int32_t r2 = r, c2 = c;
          uint8_t abs = static_cast<uint8_t>(((fl & kRowAbs) ? RefMap::kRow1 | RefMap::kRow2 : 0) |
                                             ((fl & kColAbs) ? RefMap::kCol1 | RefMap::kCol2 : 0));
          if (!how_.map->Map(RefSheet(p + 2, fl), r, c, r2, c2, abs, false, false)) {
            stack_.push_back(Sheets(p + 2, fl) + "#REF!");
            break;
          }
        }
        if (Off(r, fl & kRowAbs, at_.row, kMaxRows) || Off(c, fl & kColAbs, at_.col, kMaxCols)) {
          stack_.push_back(Sheets(p + 2, fl) + "#REF!");
          break;
        }
        stack_.push_back(Sheets(p + 2, fl) + CellText(r, fl & kRowAbs, c, fl & kColAbs));
        break;
      }
      case kArea: {
        uint8_t fl = p[1];
        const uint8_t* q = p + 2 + SheetBytes(fl);
        int32_t r1 = Read<int32_t>(q), r2 = Read<int32_t>(q + 6);
        int32_t c1 = Read<int16_t>(q + 4), c2 = Read<int16_t>(q + 10);
        if (how_.map) {
          uint8_t abs = static_cast<uint8_t>(((fl & kRowAbs) ? RefMap::kRow1 : 0) | ((fl & kColAbs) ? RefMap::kCol1 : 0) |
                                             ((fl & kRow2Abs) ? RefMap::kRow2 : 0) | ((fl & kCol2Abs) ? RefMap::kCol2 : 0));
          if (!how_.map->Map(RefSheet(p + 2, fl), r1, c1, r2, c2, abs, fl & kWholeRows, fl & kWholeCols)) {
            stack_.push_back(Sheets(p + 2, fl) + "#REF!");
            break;
          }
        }
        const bool rows = !(fl & kWholeCols), cols = !(fl & kWholeRows);
        if ((rows && (Off(r1, fl & kRowAbs, at_.row, kMaxRows) || Off(r2, fl & kRow2Abs, at_.row, kMaxRows))) ||
            (cols && (Off(c1, fl & kColAbs, at_.col, kMaxCols) || Off(c2, fl & kCol2Abs, at_.col, kMaxCols)))) {
          stack_.push_back(Sheets(p + 2, fl) + "#REF!");
          break;
        }
        std::string a, b;
        if (fl & kWholeCols) {
          a = ColText(c1, fl & kColAbs);
          b = ColText(c2, fl & kCol2Abs);
        } else if (fl & kWholeRows) {
          a = RowText(r1, fl & kRowAbs);
          b = RowText(r2, fl & kRow2Abs);
        } else {
          a = CellText(r1, fl & kRowAbs, c1, fl & kColAbs);
          b = CellText(r2, fl & kRow2Abs, c2, fl & kCol2Abs);
        }
        // R1C1 writes a single whole row or column once: R2, C[1].
        bool once = how_.r1c1 && (fl & (kWholeCols | kWholeRows)) && a == b;
        stack_.push_back(Sheets(p + 2, fl) + (once ? a : a + ":" + b));
        break;
      }
      case kRefErr: stack_.push_back(Sheets(p + 2, p[1]) + "#REF!"); break;
      case kName: {
        uint8_t fl = p[5];
        std::string name = names_.SlotName(Read<uint32_t>(p + 1));
        if (!how_.file && name.size() > 6 && _strnicmp(name.c_str(), "_xlpm.", 6) == 0) name.erase(0, 6);  // LET's names
        stack_.push_back(Sheets(p + 6, fl) + name);
        break;
      }
      case kExtern: stack_.push_back(std::string(ReadStr(p + 1, &len))); break;
      case kTable: {
        uint8_t items = p[1];
        size_t k = 2;
        std::string_view table = ReadStr(p + k, &len);
        k += len;
        std::string_view c1 = ReadStr(p + k, &len);
        k += len;
        std::string_view c2 = ReadStr(p + k, &len);
        std::vector<std::string> parts;
        if (items & kAll) parts.push_back("#All");
        if (items & kHeaders) parts.push_back("#Headers");
        if (items & kData) parts.push_back("#Data");
        if (items & kTotals) parts.push_back("#Totals");
        if (items & kThisRow) parts.push_back("#This Row");
        std::string s(table);
        s += '[';
        if (!how_.file && items == kThisRow && !c1.empty() && c2.empty()) {
          s += "@" + TableEscape(c1);  // Table1[@Col]
        } else if (parts.empty() && c2.empty()) {
          s += TableEscape(c1);  // Table1[Col], Table1[]
        } else if (parts.size() == 1 && c1.empty()) {
          s += parts[0];  // Table1[#All]
        } else {
          bool first = true;
          for (const auto& part : parts) {
            s += first ? "[" : ",[";
            s += part + "]";
            first = false;
          }
          if (!c1.empty()) {
            s += first ? "[" : ",[";
            s += TableEscape(c1) + "]";
            if (!c2.empty()) s += ":[" + TableEscape(c2) + "]";
          }
        }
        stack_.push_back(s + "]");
        break;
      }
      case kUnary: {
        std::string a = Pop();
        switch (p[1] & ~kArrayMode) {
          case kNeg: stack_.push_back("-" + a); break;
          case kPlus: stack_.push_back("+" + a); break;
          case kPercent: stack_.push_back(a + "%"); break;
          case kImplicit: stack_.push_back(how_.file ? "_xlfn.SINGLE(" + a + ")" : "@" + a); break;
          case kSpill: stack_.push_back(how_.file ? "_xlfn.ANCHORARRAY(" + a + ")" : a + "#"); break;
          default: stack_.push_back(a); break;
        }
        break;
      }
      case kBinary: {
        static const char* kOps[] = {"+", "-", "*", "/", "^", "&", "=", "<>", "<", "<=", ">", ">=", ":", ",", " "};
        std::string b = Pop(), a = Pop();
        uint8_t op = p[1] & ~kArrayMode;
        stack_.push_back(a + (op < std::size(kOps) ? kOps[op] : "?") + b);
        break;
      }
      case kCall: {
        const FuncInfo& fn = Function(Read<uint16_t>(p + 1));
        PushCall(fn.name, fn.flags, p[3]);
        break;
      }
      case kUnknown: {
        int argc = p[1];
        std::string_view name = ReadStr(p + 3, &len);
        std::vector<std::string> args(static_cast<size_t>(argc));
        for (int k = argc; k-- > 0;) args[static_cast<size_t>(k)] = Pop();
        std::string s(name);
        s += '(';
        for (int k = 0; k < argc; ++k) {
          if (k) s += ',';
          s += args[static_cast<size_t>(k)];
        }
        stack_.push_back(s + ")");
        break;
      }
      default: break;
    }
  }

 private:
  const CompileOptions& at_;
  const Names& names_;
  const PrintOptions& how_;
  std::vector<std::string> stack_;
  std::vector<Pending> lazy_;
};

}  // namespace

std::string Print(const Formula& f, const CompileOptions& at, const Names& names, const PrintOptions& how) {
  return Printer(at, names, how).Run(f);
}

}  // namespace fw::sheet::calc
