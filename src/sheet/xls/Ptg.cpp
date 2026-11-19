// BIFF8 parsed formulas ([MS-XLS] 2.5.198 Rgce, 2.5.98 Ptg) into Excel's invariant formula text,
// as an .xlsx file stores it. Formulas are stored in reverse Polish order; this runs them on a stack
// of texts. Anything it doesn't know fails the formula, whose cell then keeps its cached value.
#include <charconv>
#include <cmath>

#include "sheet/calc/Functions.h"
#include "sheet/xls/Internal.h"

namespace fw::sheet::xls::detail {
namespace {

// The built-in functions by their BIFF8 index (iftab, [MS-XLS] 2.5.198.17 Ftab), with the number
// of arguments PtgFunc gives them (-1: they always come with PtgFuncVar). Macro-sheet commands and
// functions are left out (empty): a formula that uses one isn't written.
struct Ftab {
  const char* name;
  int8_t args;
};
constexpr Ftab kFtab[] = {
    {"COUNT", -1}, {"IF", -1}, {"ISNA", 1}, {"ISERROR", 1}, {"SUM", -1}, {"AVERAGE", -1}, {"MIN", -1}, {"MAX", -1},
    {"ROW", -1}, {"COLUMN", -1}, {"NA", 0}, {"NPV", -1}, {"STDEV", -1}, {"DOLLAR", -1}, {"FIXED", -1}, {"SIN", 1},
    {"COS", 1}, {"TAN", 1}, {"ATAN", 1}, {"PI", 0}, {"SQRT", 1}, {"EXP", 1}, {"LN", 1}, {"LOG10", 1},  // 23
    {"ABS", 1}, {"INT", 1}, {"SIGN", 1}, {"ROUND", 2}, {"LOOKUP", -1}, {"INDEX", -1}, {"REPT", 2}, {"MID", 3},  // 31
    {"LEN", 1}, {"VALUE", 1}, {"TRUE", 0}, {"FALSE", 0}, {"AND", -1}, {"OR", -1}, {"NOT", 1}, {"MOD", 2},  // 39
    {"DCOUNT", 3}, {"DSUM", 3}, {"DAVERAGE", 3}, {"DMIN", 3}, {"DMAX", 3}, {"DSTDEV", 3}, {"VAR", -1}, {"DVAR", 3},  // 47
    {"TEXT", 2}, {"LINEST", -1}, {"TREND", -1}, {"LOGEST", -1}, {"GROWTH", -1}, {"", -1}, {"", -1}, {"", -1},  // 55
    {"PV", -1}, {"FV", -1}, {"NPER", -1}, {"PMT", -1}, {"RATE", -1}, {"MIRR", 3}, {"IRR", -1}, {"RAND", 0},  // 63
    {"MATCH", -1}, {"DATE", 3}, {"TIME", 3}, {"DAY", 1}, {"MONTH", 1}, {"YEAR", 1}, {"WEEKDAY", -1}, {"HOUR", 1},  // 71
    {"MINUTE", 1}, {"SECOND", 1}, {"NOW", 0}, {"AREAS", 1}, {"ROWS", 1}, {"COLUMNS", 1}, {"OFFSET", -1}, {"", -1},  // 79
    {"", -1}, {"", -1}, {"SEARCH", -1}, {"TRANSPOSE", 1}, {"", -1}, {"", -1}, {"TYPE", 1}, {"", -1},  // 87
    {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"", -1},  // 95
    {"", -1}, {"ATAN2", 2}, {"ASIN", 1}, {"ACOS", 1}, {"CHOOSE", -1}, {"HLOOKUP", -1}, {"VLOOKUP", -1}, {"", -1},  // 103
    {"", -1}, {"ISREF", 1}, {"", -1}, {"", -1}, {"", -1}, {"LOG", -1}, {"", -1}, {"CHAR", 1},  // 111
    {"LOWER", 1}, {"UPPER", 1}, {"PROPER", 1}, {"LEFT", -1}, {"RIGHT", -1}, {"EXACT", 2}, {"TRIM", 1}, {"REPLACE", 4},  // 119
    {"SUBSTITUTE", -1}, {"CODE", 1}, {"", -1}, {"", -1}, {"FIND", -1}, {"CELL", -1}, {"ISERR", 1}, {"ISTEXT", 1},  // 127
    {"ISNUMBER", 1}, {"ISBLANK", 1}, {"T", 1}, {"N", 1}, {"", -1}, {"", -1}, {"", -1}, {"", -1},  // 135
    {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"DATEVALUE", 1}, {"TIMEVALUE", 1}, {"SLN", 3}, {"SYD", 4},  // 143
    {"DDB", -1}, {"", -1}, {"", -1}, {"", -1}, {"INDIRECT", -1}, {"", -1}, {"", -1}, {"", -1},  // 151
    {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"", -1},  // 159
    {"", -1}, {"", -1}, {"CLEAN", 1}, {"MDETERM", 1}, {"MINVERSE", 1}, {"MMULT", 2}, {"", -1}, {"IPMT", -1},  // 167
    {"PPMT", -1}, {"COUNTA", -1}, {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"", -1},  // 175
    {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"PRODUCT", -1},  // 183
    {"FACT", 1}, {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"DPRODUCT", 3}, {"ISNONTEXT", 1}, {"", -1},  // 191
    {"", -1}, {"STDEVP", -1}, {"VARP", -1}, {"DSTDEVP", 3}, {"DVARP", 3}, {"TRUNC", -1}, {"ISLOGICAL", 1}, {"DCOUNTA", 3},  // 199
    {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"USDOLLAR", -1}, {"FINDB", -1}, {"SEARCHB", -1}, {"REPLACEB", 4},  // 207
    {"LEFTB", -1}, {"RIGHTB", -1}, {"MIDB", 3}, {"LENB", 1}, {"ROUNDUP", 2}, {"ROUNDDOWN", 2}, {"ASC", 1}, {"DBCS", 1},  // 215
    {"RANK", -1}, {"", -1}, {"", -1}, {"ADDRESS", -1}, {"DAYS360", -1}, {"TODAY", 0}, {"VDB", -1}, {"", -1},  // 223
    {"", -1}, {"", -1}, {"", -1}, {"MEDIAN", -1}, {"SUMPRODUCT", -1}, {"SINH", 1}, {"COSH", 1}, {"TANH", 1},  // 231
    {"ASINH", 1}, {"ACOSH", 1}, {"ATANH", 1}, {"DGET", 3}, {"", -1}, {"", -1}, {"", -1}, {"", -1},  // 239
    {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"INFO", 1}, {"", -1}, {"", -1}, {"DB", -1},  // 247
    {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"FREQUENCY", 2}, {"", -1}, {"", -1}, {"", -1},  // 255
    {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"ERROR.TYPE", 1}, {"", -1}, {"", -1},  // 263
    {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"AVEDEV", -1}, {"BETADIST", -1}, {"GAMMALN", 1},  // 271
    {"BETAINV", -1}, {"BINOMDIST", 4}, {"CHIDIST", 2}, {"CHIINV", 2}, {"COMBIN", 2}, {"CONFIDENCE", 3}, {"CRITBINOM", 3},
    {"EVEN", 1},  // 279
    {"EXPONDIST", 3}, {"FDIST", 3}, {"FINV", 3}, {"FISHER", 1}, {"FISHERINV", 1}, {"FLOOR", 2}, {"GAMMADIST", 4},
    {"GAMMAINV", 3},  // 287
    {"CEILING", 2}, {"HYPGEOMDIST", 4}, {"LOGNORMDIST", 3}, {"LOGINV", 3}, {"NEGBINOMDIST", 3}, {"NORMDIST", 4},
    {"NORMSDIST", 1}, {"NORMINV", 3},  // 295
    {"NORMSINV", 1}, {"STANDARDIZE", 3}, {"ODD", 1}, {"PERMUT", 2}, {"POISSON", 3}, {"TDIST", 3}, {"WEIBULL", 4},
    {"SUMXMY2", 2},  // 303
    {"SUMX2MY2", 2}, {"SUMX2PY2", 2}, {"CHITEST", 2}, {"CORREL", 2}, {"COVAR", 2}, {"FORECAST", 3}, {"FTEST", 2},
    {"INTERCEPT", 2},  // 311
    {"PEARSON", 2}, {"RSQ", 2}, {"STEYX", 2}, {"SLOPE", 2}, {"TTEST", 4}, {"PROB", -1}, {"DEVSQ", -1}, {"GEOMEAN", -1},  // 319
    {"HARMEAN", -1}, {"SUMSQ", -1}, {"KURT", -1}, {"SKEW", -1}, {"ZTEST", -1}, {"LARGE", 2}, {"SMALL", 2},
    {"QUARTILE", 2},  // 327
    {"PERCENTILE", 2}, {"PERCENTRANK", -1}, {"MODE", -1}, {"TRIMMEAN", 2}, {"TINV", 2}, {"", -1}, {"", -1}, {"", -1},  // 335
    {"CONCATENATE", -1}, {"POWER", 2}, {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"RADIANS", 1}, {"DEGREES", 1},  // 343
    {"SUBTOTAL", -1}, {"SUMIF", -1}, {"COUNTIF", 2}, {"COUNTBLANK", 1}, {"", -1}, {"", -1}, {"ISPMT", 4},
    {"DATEDIF", 3},  // 351
    {"DATESTRING", 1}, {"NUMBERSTRING", 2}, {"ROMAN", -1}, {"", -1}, {"", -1}, {"", -1}, {"GETPIVOTDATA", -1},
    {"HYPERLINK", -1},  // 359
    {"PHONETIC", 1}, {"AVERAGEA", -1}, {"MAXA", -1}, {"MINA", -1}, {"STDEVPA", -1}, {"VARPA", -1}, {"STDEVA", -1},
    {"VARA", -1},  // 367
    {"BAHTTEXT", 1}, {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"", -1}, {"", -1},  // 375
    {"", -1}, {"", -1}, {"", -1}, {"RTD", -1},  // 379
};
constexpr size_t kFtabSize = sizeof(kFtab) / sizeof(kFtab[0]);
static_assert(kFtabSize == 380);

constexpr size_t kMaxText = 32768;  // a formula's text, and each piece of it
constexpr size_t kMaxStack = 4096;

bool StartsWithNoCase(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    char a = s[i], b = prefix[i];
    if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 32);
    if (b >= 'a' && b <= 'z') b = static_cast<char>(b - 32);
    if (a != b) return false;
  }
  return true;
}

// A function called by name (iftab 255: an add-in's, or one newer than Excel 2003, which files
// write as the name _xlfn.NAME) as an .xlsx file writes it: with _xlfn. where Excel writes it.
std::string FunctionName(std::string_view name) {
  std::string_view bare = name;
  if (StartsWithNoCase(bare, "_xlfn.")) bare.remove_prefix(6);
  if (StartsWithNoCase(bare, "_xlws.")) bare.remove_prefix(6);
  const int id = calc::FindFunction(bare);
  if (id < 0) return std::string(name);
  const calc::FuncInfo& f = calc::Function(id);
  if (f.flags & calc::kFnXlws) return std::string("_xlfn._xlws.") + f.name;
  if (f.flags & calc::kFnXlfn) return std::string("_xlfn.") + f.name;
  return f.name;
}

void AppendCell(std::string& o, uint32_t row, uint32_t col, bool rowRel, bool colRel) {
  if (!colRel) o += '$';
  AppendCol(o, col);
  if (!rowRel) o += '$';
  o += std::to_string(row + 1);
}

struct Loc {
  uint32_t row = 0, col = 0;
  bool rowRel = false, colRel = false;
};

// A row and a column field (ColRelU: 14 bits of column, then colRelative, rowRelative), relative to
// (baseRow, baseCol) when `relative` (PtgRefN, PtgAreaN).
Loc ReadLoc(uint16_t rw, uint16_t colField, bool relative, uint32_t baseRow, uint32_t baseCol) {
  Loc l;
  l.colRel = (colField & 0x4000) != 0;
  l.rowRel = (colField & 0x8000) != 0;
  l.row = rw;
  l.col = colField & 0x3FFF;
  if (relative) {
    if (l.rowRel) l.row = (baseRow + static_cast<uint32_t>(static_cast<int16_t>(rw))) & 0xFFFF;
    if (l.colRel) l.col = (baseCol + static_cast<uint32_t>(static_cast<int8_t>(colField & 0xFF))) & 0xFF;
  }
  return l;
}

void AppendArea(std::string& o, const Loc& a, const Loc& b) {
  if (a.row == 0 && b.row == 0xFFFF) {  // whole columns (BIFF8's 65,536 rows)
    if (!a.colRel) o += '$';
    AppendCol(o, a.col);
    o += ':';
    if (!b.colRel) o += '$';
    AppendCol(o, b.col);
    return;
  }
  if (a.col == 0 && b.col == 0xFF) {  // whole rows (256 columns)
    if (!a.rowRel) o += '$';
    o += std::to_string(a.row + 1);
    o += ':';
    if (!b.rowRel) o += '$';
    o += std::to_string(b.row + 1);
    return;
  }
  AppendCell(o, a.row, a.col, a.rowRel, a.colRel);
  o += ':';
  AppendCell(o, b.row, b.col, b.rowRel, b.colRel);
}

class Decompiler {
 public:
  Decompiler(const FormulaContext& ctx, std::span<const uint8_t> rgce, std::span<const uint8_t> rgcb, uint32_t row,
             uint32_t col)
      : ctx_(ctx), b_(rgce), extra_(rgcb), row_(row), col_(col) {}

  bool Run(std::string& out) {
    while (b_.Left() > 0) {
      if (!Step()) return false;
      if (!b_.Ok() || st_.size() > kMaxStack) return false;
    }
    if (st_.size() != 1 || st_[0].size() > kMaxText) return false;
    out = std::move(st_[0]);
    return true;
  }

 private:
  bool Push(std::string s) {
    if (s.size() > kMaxText) return false;
    st_.push_back(pending_ + s);
    pending_.clear();
    return true;
  }
  bool Pop(std::string& s) {
    if (st_.empty()) return false;
    s = std::move(st_.back());
    st_.pop_back();
    return true;
  }

  // The sheet prefix of a 3-D reference ("Sheet1!", "'Sheet 1:Sheet 3'!"); false when it can't
  // be written (another workbook). `bad`: the sheet is gone (#REF!).
  bool SheetPrefix(uint16_t ixti, std::string& prefix, bool& bad) {
    bad = false;
    if (ixti >= ctx_.xti.size()) return false;
    const FormulaContext::Xti& x = ctx_.xti[ixti];
    if (x.supBook >= ctx_.supBooks.size() || ctx_.supBooks[x.supBook].kind != FormulaContext::SupBook::Self) return false;
    if (x.first < 0 || x.last < 0 || static_cast<size_t>(x.first) >= ctx_.sheets.size() ||
        static_cast<size_t>(x.last) >= ctx_.sheets.size() || ctx_.sheets[x.first].empty() || ctx_.sheets[x.last].empty()) {
      bad = true;
      return true;
    }
    if (x.first == x.last) {
      prefix = QuoteSheet(ctx_.sheets[x.first]) + "!";
    } else {
      const std::string both = ctx_.sheets[x.first] + ":" + ctx_.sheets[x.last];
      if (QuoteSheet(ctx_.sheets[x.first]).front() == '\'' || QuoteSheet(ctx_.sheets[x.last]).front() == '\'') {
        prefix = "'";
        for (char c : both) {
          if (c == '\'') prefix += '\'';
          prefix += c;
        }
        prefix += "'!";
      } else {
        prefix = both + "!";
      }
    }
    return true;
  }

  bool Function(int iftab, int argc) {
    if (argc < 0 || static_cast<size_t>(argc) > st_.size()) return false;
    std::vector<std::string> args(st_.end() - argc, st_.end());
    st_.resize(st_.size() - argc);
    std::string name;
    size_t first = 0;
    if (iftab == 255) {
      if (args.empty()) return false;
      std::string_view n = args[0];
      while (!n.empty() && n.front() == ' ') n.remove_prefix(1);
      if (n.empty() || n.find_first_of("()\"'!") != std::string_view::npos) return false;
      name = FunctionName(n);
      first = 1;
    } else {
      if (iftab < 0 || static_cast<size_t>(iftab) >= kFtabSize || !*kFtab[iftab].name) return false;
      name = kFtab[iftab].name;
    }
    std::string text = name + "(";
    for (size_t i = first; i < args.size(); ++i) {
      if (i > first) text += ',';
      text += args[i];
      if (text.size() > kMaxText) return false;
    }
    text += ')';
    return Push(std::move(text));
  }

  bool Step() {
    const uint8_t ptg = b_.U8();
    const uint8_t base = ptg < 0x20 ? ptg : static_cast<uint8_t>((ptg & 0x1F) | 0x20);
    switch (base) {
      case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C:
      case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: {
        static const char* const kOps[] = {"+", "-", "*", "/", "^", "&", "<", "<=", "=", ">=", ">", "<>", " ", ",", ":"};
        std::string r, l;
        if (!Pop(r) || !Pop(l)) return false;
        std::string op = pending_ + kOps[base - 0x03];
        pending_.clear();
        if (l.size() + r.size() + op.size() > kMaxText) return false;
        st_.push_back(l + op + r);
        return true;
      }
      case 0x12: case 0x13: {
        std::string x;
        if (!Pop(x)) return false;
        return Push((base == 0x12 ? "+" : "-") + x);
      }
      case 0x14: {
        std::string x;
        if (!Pop(x)) return false;
        std::string op = pending_ + "%";
        pending_.clear();
        st_.push_back(x + op);
        return true;
      }
      case 0x15: {
        std::string x;
        if (!Pop(x)) return false;
        std::string open = pending_;
        pending_.clear();
        st_.push_back(open + "(" + x + ")");
        return true;
      }
      case 0x16: return Push("");  // a missing argument
      case 0x17: {
        std::string s;
        if (!ReadShortString(b_, s)) return false;
        std::string q = "\"";
        for (char c : s) q += c == '"' ? std::string("\"\"") : std::string(1, c);
        return Push(q + "\"");
      }
      case 0x19: {  // PtgAttr
        const uint8_t type = b_.U8();
        if (type & 0x04) {  // tAttrChoose: a jump table
          const uint16_t n = b_.U16();
          return b_.Skip((size_t{n} + 1) * 2);
        }
        const uint16_t data = b_.U16();
        if (type & 0x10) {  // tAttrSum: SUM of the one argument
          std::string x;
          if (!Pop(x)) return false;
          return Push("SUM(" + x + ")");
        }
        if ((type & 0x40) && (data & 0xFF) == 0) pending_.append(std::min(data >> 8, 255), ' ');
        return true;
      }
      case 0x1C: {
        const std::string_view e = ErrorName(b_.U8());
        if (e.empty()) return false;
        return Push(std::string(e));
      }
      case 0x1D: return Push(b_.U8() ? "TRUE" : "FALSE");
      case 0x1E: return Push(std::to_string(b_.U16()));
      case 0x1F: {
        const double v = b_.F64();
        if (!std::isfinite(v)) return false;
        std::string s;
        AppendNumber(s, v);
        return Push(s);
      }
      case 0x20: return Array();
      case 0x21: {
        const uint16_t iftab = b_.U16();
        if (iftab >= kFtabSize || kFtab[iftab].args < 0) return false;
        return Function(iftab, kFtab[iftab].args);
      }
      case 0x22: {
        const uint8_t argc = b_.U8() & 0x7F;
        const uint16_t tab = b_.U16();
        if (tab & 0x8000) return false;  // a macro command
        return Function(tab & 0x7FFF, argc);
      }
      case 0x23: {
        const uint32_t i = b_.U32();
        if (i == 0 || i > ctx_.names.size() || ctx_.names[i - 1].empty()) return false;
        return Push(ctx_.names[i - 1]);
      }
      case 0x24: case 0x2C: {  // PtgRef, PtgRefN
        const uint16_t rw = b_.U16(), c = b_.U16();
        const Loc l = ReadLoc(rw, c, base == 0x2C, row_, col_);
        std::string s;
        AppendCell(s, l.row, l.col, l.rowRel, l.colRel);
        return Push(s);
      }
      case 0x25: case 0x2D: {  // PtgArea, PtgAreaN
        const uint16_t r1 = b_.U16(), r2 = b_.U16(), c1 = b_.U16(), c2 = b_.U16();
        std::string s;
        AppendArea(s, ReadLoc(r1, c1, base == 0x2D, row_, col_), ReadLoc(r2, c2, base == 0x2D, row_, col_));
        return Push(s);
      }
      case 0x26: {  // PtgMemArea: its areas are in the extra data
        b_.Skip(6);
        const uint16_t n = extra_.U16();
        extra_.Skip(size_t{n} * 8);
        return extra_.Ok();
      }
      case 0x27: case 0x28: return b_.Skip(6);  // PtgMemErr, PtgMemNoMem
      case 0x29: return b_.Skip(2);             // PtgMemFunc
      case 0x2A: b_.Skip(4); return Push("#REF!");
      case 0x2B: b_.Skip(8); return Push("#REF!");
      case 0x39: {  // PtgNameX
        const uint16_t ixti = b_.U16();
        const uint32_t i = b_.U32();
        if (ixti >= ctx_.xti.size()) return false;
        const uint32_t sb = ctx_.xti[ixti].supBook;
        if (sb >= ctx_.supBooks.size()) return false;
        const FormulaContext::SupBook& book = ctx_.supBooks[sb];
        if (book.kind == FormulaContext::SupBook::Self) {
          if (i == 0 || i > ctx_.names.size() || ctx_.names[i - 1].empty()) return false;
          return Push(ctx_.names[i - 1]);
        }
        if (book.kind != FormulaContext::SupBook::AddIn || i == 0 || i > book.names.size() || book.names[i - 1].empty())
          return false;
        return Push(book.names[i - 1]);
      }
      case 0x3A: case 0x3B: case 0x3C: case 0x3D: {  // PtgRef3d, PtgArea3d, PtgRefErr3d, PtgAreaErr3d
        const uint16_t ixti = b_.U16();
        std::string s;
        bool bad = false;
        if (!SheetPrefix(ixti, s, bad)) return false;
        if (base == 0x3A) {
          const uint16_t rw = b_.U16(), c = b_.U16();
          const Loc l = ReadLoc(rw, c, false, 0, 0);
          AppendCell(s, l.row, l.col, l.rowRel, l.colRel);
        } else if (base == 0x3B) {
          const uint16_t r1 = b_.U16(), r2 = b_.U16(), c1 = b_.U16(), c2 = b_.U16();
          AppendArea(s, ReadLoc(r1, c1, false, 0, 0), ReadLoc(r2, c2, false, 0, 0));
        } else {
          b_.Skip(base == 0x3C ? 4 : 8);
          bad = true;
        }
        return Push(bad ? std::string("#REF!") : s);
      }
      default: return false;  // PtgExp, PtgTbl (handled by the cell), PtgElf..., unknown
    }
  }

  // PtgArray: the values are in the extra data.
  bool Array() {
    b_.Skip(7);
    uint32_t cols = extra_.U8();
    const uint32_t rows = uint32_t{extra_.U16()} + 1;
    cols = cols == 0 ? 256 : cols + 1;
    if (!extra_.Ok() || size_t{rows} * cols > extra_.Left()) return false;
    std::string s = "{";
    for (uint32_t r = 0; r < rows; ++r) {
      if (r) s += ';';
      for (uint32_t c = 0; c < cols; ++c) {
        if (c) s += ',';
        switch (extra_.U8()) {
          case 0x01: {
            const double v = extra_.F64();
            if (!std::isfinite(v)) return false;
            AppendNumber(s, v);
            break;
          }
          case 0x02: {
            std::string t;
            if (!ReadString(extra_, t)) return false;
            s += '"';
            for (char ch : t) s += ch == '"' ? std::string("\"\"") : std::string(1, ch);
            s += '"';
            break;
          }
          case 0x04: s += extra_.U8() ? "TRUE" : "FALSE"; extra_.Skip(7); break;
          case 0x10: {
            const std::string_view e = ErrorName(extra_.U8());
            if (e.empty()) return false;
            s += e;
            extra_.Skip(7);
            break;
          }
          default: return false;  // empty (0x00) can't be written in a constant
        }
        if (!extra_.Ok() || s.size() > kMaxText) return false;
      }
    }
    s += '}';
    return Push(s);
  }

  const FormulaContext& ctx_;
  Bytes b_, extra_;
  uint32_t row_, col_;
  std::vector<std::string> st_;
  std::string pending_;  // spaces before the next token (tAttrSpace)
};

bool IsCellLike(std::string_view s) {
  // "A1".."XFD1048576" or R1C1-like names must be quoted.
  size_t i = 0;
  while (i < s.size() && ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z'))) ++i;
  if (i == 0 || i > 3 || i == s.size()) return false;
  for (size_t k = i; k < s.size(); ++k)
    if (s[k] < '0' || s[k] > '9') return false;
  return true;
}

}  // namespace

void AppendCodeUnit(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | cp >> 6);
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | cp >> 12);
    out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | cp >> 18);
    out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
    out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

void AppendUtf16(std::string& out, const uint16_t* u, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    uint32_t c = u[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00);
      ++i;
    } else if (c >= 0xD800 && c <= 0xDFFF) {
      c = 0xFFFD;
    }
    AppendCodeUnit(out, c);
  }
}

bool ReadChars(Bytes& b, uint32_t cch, std::string& out) {
  const uint8_t flags = b.U8();
  if (!b.Ok()) return false;
  if (flags & 1) {
    if (size_t{cch} * 2 > b.Left()) return false;
    std::vector<uint16_t> u(cch);
    for (uint32_t i = 0; i < cch; ++i) u[i] = b.U16();
    AppendUtf16(out, u.data(), u.size());
  } else {
    if (cch > b.Left()) return false;
    for (uint32_t i = 0; i < cch; ++i) AppendCodeUnit(out, b.U8());
  }
  return b.Ok();
}

bool ReadShortString(Bytes& b, std::string& out) {
  const uint8_t cch = b.U8();
  return b.Ok() && ReadChars(b, cch, out);
}

bool ReadString(Bytes& b, std::string& out) {
  const uint16_t cch = b.U16();
  if (!b.Ok()) return false;
  const uint8_t flags = b.U8();
  uint16_t runs = 0;
  uint32_t ext = 0;
  if (flags & 0x08) runs = b.U16();
  if (flags & 0x04) ext = b.U32();
  if (!b.Ok()) return false;
  const size_t bytes = (flags & 1) ? size_t{cch} * 2 : cch;
  if (bytes > b.Left()) return false;
  if (flags & 1) {
    std::vector<uint16_t> u(cch);
    for (uint32_t i = 0; i < cch; ++i) u[i] = b.U16();
    AppendUtf16(out, u.data(), u.size());
  } else {
    for (uint32_t i = 0; i < cch; ++i) AppendCodeUnit(out, b.U8());
  }
  b.Skip(size_t{runs} * 4);
  b.Skip(ext);
  return b.Ok();
}

void AppendCol(std::string& out, uint32_t col) {
  char buf[8];
  int n = 0;
  uint32_t c = col + 1;
  while (c > 0 && n < 8) {
    buf[n++] = static_cast<char>('A' + (c - 1) % 26);
    c = (c - 1) / 26;
  }
  while (n > 0) out += buf[--n];
}

std::string QuoteSheet(std::string_view name) {
  bool plain = !name.empty() && !IsCellLike(name) && !(name[0] >= '0' && name[0] <= '9') && name[0] != '.';
  for (char c : name) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
    if (!ok) plain = false;
  }
  // R1C1-like names ("R", "C", "R1", "RC2") read as references: quoted too.
  if (plain && (name[0] == 'R' || name[0] == 'r' || name[0] == 'C' || name[0] == 'c')) {
    bool refLike = true;
    for (char c : name)
      if (!(c == 'R' || c == 'r' || c == 'C' || c == 'c' || (c >= '0' && c <= '9'))) refLike = false;
    if (refLike) plain = false;
  }
  if (plain) return std::string(name);
  std::string q = "'";
  for (char c : name) {
    if (c == '\'') q += '\'';
    q += c;
  }
  return q + "'";
}

void AppendNumber(std::string& out, double v) {
  if (v == 0) v = 0;  // no "-0"
  char buf[64];
  auto r = std::to_chars(buf, buf + sizeof buf, v);
  for (char* p = buf; p < r.ptr; ++p) out += *p == 'e' ? 'E' : *p;
}

std::string_view ErrorName(uint8_t code) {
  switch (code) {
    case 0x00: return "#NULL!";
    case 0x07: return "#DIV/0!";
    case 0x0F: return "#VALUE!";
    case 0x17: return "#REF!";
    case 0x1D: return "#NAME?";
    case 0x24: return "#NUM!";
    case 0x2A: return "#N/A";
    case 0x2B: return "#GETTING_DATA";
  }
  return {};
}

bool Decompile(const FormulaContext& ctx, std::span<const uint8_t> rgce, std::span<const uint8_t> rgcb, uint32_t row,
               uint32_t col, std::string& out) {
  if (rgce.empty()) return false;
  Decompiler d(ctx, rgce, rgcb, row, col);
  return d.Run(out);
}

}  // namespace fw::sheet::xls::detail
