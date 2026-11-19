// Text functions. Lengths and positions count UTF-16 code units, as Excel does.

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "sheet/calc/Fn.h"

namespace fw::sheet::calc::fn {
namespace {

constexpr size_t kMaxText = 32767;

void SetText(Call& c, std::string s) {
  if (s.size() > kMaxText * 3 || Wide(s).size() > kMaxText) return c.Fail(Err::Value);
  c.out = Value::Text(s);
}
void SetWide(Call& c, const std::wstring& w) {
  if (w.size() > kMaxText) return c.Fail(Err::Value);
  c.out = Value::Text(Narrow(w));
}

std::wstring MapCase(const std::wstring& w, DWORD flag) {
  if (w.empty()) return w;
  std::wstring out(w.size(), L'\0');
  int n = LCMapStringEx(LOCALE_NAME_INVARIANT, flag, w.data(), static_cast<int>(w.size()), out.data(),
                        static_cast<int>(out.size()), nullptr, nullptr, 0);
  out.resize(static_cast<size_t>(n > 0 ? n : 0));
  return out;
}

// Digits with thousands separators and `decimals` decimals (negative: rounded to tens, hundreds...).
std::string Grouped(double x, int decimals, bool commas) {
  double scale = std::pow(10.0, decimals);
  double r = std::floor(Round15(std::fabs(x) * scale) + 0.5) / scale;
  char buf[400];
  std::snprintf(buf, sizeof buf, "%.*f", decimals > 0 ? decimals : 0, r);
  std::string s = buf;
  size_t point = s.find('.');
  std::string whole = s.substr(0, point), frac = point == std::string::npos ? "" : s.substr(point);
  if (commas)
    for (int i = static_cast<int>(whole.size()) - 3; i > 0; i -= 3) whole.insert(static_cast<size_t>(i), ",");
  return whole + frac;
}

}  // namespace

void Len(Call& c) {
  std::string s;
  if (c.Text(0, s)) c.SetNumber(static_cast<double>(Wide(s).size()));
}

static void Side(Call& c, bool left) {
  std::string s;
  double n;
  if (!c.Text(0, s) || !c.Number(1, n, 1)) return;
  if (n < 0) return c.Fail(Err::Value);
  std::wstring w = Wide(s);
  size_t k = static_cast<size_t>(std::min<double>(std::trunc(n), static_cast<double>(w.size())));
  // Excel keeps a character outside the BMP whole.
  if (left && k > 0 && k < w.size() && IS_HIGH_SURROGATE(w[k - 1])) ++k;
  if (!left && k > 0 && k < w.size() && IS_LOW_SURROGATE(w[w.size() - k])) ++k;
  SetWide(c, left ? w.substr(0, k) : w.substr(w.size() - k));
}
void Left(Call& c) { Side(c, true); }
void Right(Call& c) { Side(c, false); }

void Mid(Call& c) {
  std::string s;
  double start, n;
  if (!c.Text(0, s) || !c.Number(1, start) || !c.Number(2, n)) return;
  start = std::trunc(start), n = std::trunc(n);
  if (start < 1 || n < 0) return c.Fail(Err::Value);
  std::wstring w = Wide(s);
  if (start > static_cast<double>(w.size())) return SetWide(c, L"");
  SetWide(c, w.substr(static_cast<size_t>(start) - 1, static_cast<size_t>(std::min(n, 1e9))));
}

void Trim(Call& c) {
  std::string s;
  if (!c.Text(0, s)) return;
  std::string out;
  bool space = false;
  for (char ch : s) {
    if (ch == ' ') {
      space = !out.empty();
      continue;
    }
    if (space) out += ' ';
    space = false;
    out += ch;
  }
  SetText(c, out);
}

void Upper(Call& c) {
  std::string s;
  if (c.Text(0, s)) SetWide(c, MapCase(Wide(s), LCMAP_UPPERCASE));
}
void Lower(Call& c) {
  std::string s;
  if (c.Text(0, s)) SetWide(c, MapCase(Wide(s), LCMAP_LOWERCASE));
}
void Proper(Call& c) {
  std::string s;
  if (!c.Text(0, s)) return;
  std::wstring w = MapCase(Wide(s), LCMAP_LOWERCASE);
  bool prevLetter = false;
  for (wchar_t& ch : w) {
    bool letter = IsCharAlphaW(ch) != 0;
    if (letter && !prevLetter) ch = MapCase(std::wstring(1, ch), LCMAP_UPPERCASE)[0];
    prevLetter = letter;
  }
  SetWide(c, w);
}

void Substitute(Call& c) {
  std::string text, from, to;
  double inst = 0;
  if (!c.Text(0, text) || !c.Text(1, from) || !c.Text(2, to)) return;
  if (c.Has(3)) {
    if (!c.Number(3, inst)) return;
    inst = std::trunc(inst);
    if (inst < 1) return c.Fail(Err::Value);
  }
  if (from.empty()) return SetText(c, text);
  std::string out;
  size_t pos = 0, k = 0;
  for (;;) {
    size_t f = text.find(from, pos);
    if (f == std::string::npos) break;
    ++k;
    if (inst == 0 || k == static_cast<size_t>(inst)) {
      out.append(text, pos, f - pos);
      out += to;
    } else {
      out.append(text, pos, f - pos + from.size());
    }
    pos = f + from.size();
    if (inst != 0 && k == static_cast<size_t>(inst)) break;
  }
  out.append(text, pos, std::string::npos);
  SetText(c, out);
}

void Replace(Call& c) {
  std::string text, with;
  double start, n;
  if (!c.Text(0, text) || !c.Number(1, start) || !c.Number(2, n) || !c.Text(3, with)) return;
  start = std::trunc(start), n = std::trunc(n);
  if (start < 1 || n < 0) return c.Fail(Err::Value);
  std::wstring w = Wide(text);
  size_t s = std::min(static_cast<size_t>(start) - 1, w.size());
  size_t len = std::min(static_cast<size_t>(std::min(n, 1e9)), w.size() - s);
  w.replace(s, len, Wide(with));
  SetWide(c, w);
}

static void Locate(Call& c, bool search) {
  std::string what, within;
  double start;
  if (!c.Text(0, what) || !c.Text(1, within) || !c.Number(2, start, 1)) return;
  start = std::trunc(start);
  std::wstring w = Wide(within), f = Wide(what);
  if (start < 1 || start > static_cast<double>(w.size()) + (f.empty() ? 1 : 0)) return c.Fail(Err::Value);
  size_t s = static_cast<size_t>(start) - 1;
  if (f.empty()) return c.SetNumber(start);
  if (!search) {
    size_t pos = w.find(f, s);
    if (pos == std::wstring::npos) return c.Fail(Err::Value);
    return c.SetNumber(static_cast<double>(pos) + 1);
  }
  std::string pattern = Narrow(f) + "*";
  for (size_t pos = s; pos < w.size(); ++pos)
    if (WildcardMatch(pattern, Narrow(w.substr(pos)))) return c.SetNumber(static_cast<double>(pos) + 1);
  c.Fail(Err::Value);
}
void Find(Call& c) { Locate(c, false); }
void Search(Call& c) { Locate(c, true); }

void ValueFn(Call& c) {
  const Value& v = c.arg(0);
  if (v.IsError()) return c.Fail(v.error());
  if (v.IsNumber()) return c.SetNumber(v.number());
  if (v.IsEmpty()) return c.SetNumber(0);
  if (v.IsBool()) return c.Fail(Err::Value);
  double d;
  if (!c.m.host.ParseNumber(v.text(), d)) return c.Fail(Err::Value);
  c.SetNumber(d);
}

void Text(Call& c) {
  std::string format;
  if (!c.Text(1, format)) return;
  const Value& v = c.arg(0);
  if (v.IsError()) return c.Fail(v.error());
  CellView x;
  x.type = Type::Number;
  if (v.IsText()) {
    if (!c.m.host.ParseNumber(v.text(), x.num)) {  // text goes through the format's text section
      x.type = Type::Text;
      x.text = v.text();
    }
  } else if (v.IsBool()) {
    return SetText(c, v.boolean() ? "TRUE" : "FALSE");
  } else {
    x.num = v.IsNumber() ? v.number() : 0;
  }
  if (format.empty()) {  // one empty section: text as it is, a negative number its minus sign
    if (x.type == Type::Text) return SetText(c, std::string(v.text()));
    return SetText(c, x.num < 0 ? "-" : "");
  }
  std::string out;
  const int done = c.m.host.FormatText(x, format, out);
  if (done == 0) {
    // No number-format engine: text without a text section stays as it is; else keep the cached value.
    if (x.type == Type::Text && format.find('@') == std::string::npos && format.find(';') == std::string::npos)
      return SetText(c, std::string(v.text()));
    c.m.unsupported = true;
  }
  if (done <= 0) return c.Fail(Err::Value);
  SetText(c, out);
}

void Concatenate(Call& c) {
  std::string out, s;
  for (int i = 0; i < c.argc; ++i) {
    if (!c.Text(i, s)) return;
    out += s;
    if (out.size() > kMaxText * 3) return c.Fail(Err::Value);
  }
  SetText(c, out);
}

static bool Pieces(Call& c, int i, bool skipEmpty, std::vector<std::string>& out) {
  const Value& v = c.arg(i);
  if (v.IsMissing()) {
    if (!skipEmpty) out.emplace_back();
    return true;
  }
  if (v.type() == Type::Ref) {
    Grid g;
    if (!MakeGrid(c.m, v, g)) {
      c.Fail(Err::Value);
      return false;
    }
    for (int32_t r = 0; r < g.dataRows; ++r)
      for (int32_t k = 0; k < g.dataCols; ++k) {
        CellView cv = g.At(r, k);
        if (cv.type == Type::Error) {
          c.Fail(cv.err);
          return false;
        }
        std::string s = ValueText(FromView(cv), c.m.decimal);
        if (s.empty() && skipEmpty) continue;
        out.push_back(std::move(s));
        if (out.size() > kMaxText) {
          c.Fail(Err::Value);
          return false;
        }
      }
    return true;
  }
  bool ok = EachItem(c.m, v, [&](const CellView& cv, From) {
    if (cv.type == Type::Error) {
      c.Fail(cv.err);
      return false;
    }
    std::string s = ValueText(FromView(cv), c.m.decimal);
    if (!(s.empty() && skipEmpty)) out.push_back(std::move(s));
    return out.size() <= kMaxText;
  });
  if (!ok && !c.out.IsError()) c.Fail(Err::Value);
  return ok;
}

void Concat(Call& c) {
  std::vector<std::string> parts;
  for (int i = 0; i < c.argc; ++i)
    if (!Pieces(c, i, true, parts)) return;
  std::string out;
  for (const auto& p : parts) out += p;
  SetText(c, out);
}

void TextJoin(Call& c) {
  std::vector<std::string> delims;
  if (!Pieces(c, 0, false, delims)) return;
  bool skip;
  if (!c.Boolean(1, skip)) return;
  std::vector<std::string> parts;
  for (int i = 2; i < c.argc; ++i)
    if (!Pieces(c, i, skip, parts)) return;
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i && !delims.empty()) out += delims[(i - 1) % delims.size()];
    out += parts[i];
    if (out.size() > kMaxText * 3) return c.Fail(Err::Value);
  }
  SetText(c, out);
}

void Rept(Call& c) {
  std::string s;
  double n;
  if (!c.Text(0, s) || !c.Number(1, n)) return;
  n = std::trunc(n);
  if (n < 0) return c.Fail(Err::Value);
  std::wstring w = Wide(s);
  if (w.size() * n > kMaxText) return c.Fail(Err::Value);
  std::string out;
  for (int i = 0; i < static_cast<int>(n); ++i) out += s;
  SetText(c, out);
}

void Exact(Call& c) {
  std::string a, b;
  if (c.Text(0, a) && c.Text(1, b)) c.out = Value::Bool(a == b);
}

void Char(Call& c) {
  double n;
  if (!c.Number(0, n)) return;
  n = std::trunc(n);
  if (n < 1 || n > 255) return c.Fail(Err::Value);
  char ch = static_cast<char>(static_cast<unsigned char>(n));
  wchar_t w = 0;
  MultiByteToWideChar(1252, 0, &ch, 1, &w, 1);
  SetWide(c, std::wstring(1, w));
}

void Code(Call& c) {
  std::string s;
  if (!c.Text(0, s)) return;
  std::wstring w = Wide(s);
  if (w.empty()) return c.Fail(Err::Value);
  char ch = '?';
  BOOL lossy = FALSE;
  WideCharToMultiByte(1252, 0, w.data(), 1, &ch, 1, nullptr, &lossy);
  c.SetNumber(static_cast<unsigned char>(ch));
}

void Clean(Call& c) {
  std::string s;
  if (!c.Text(0, s)) return;
  std::string out;
  for (char ch : s)
    if (static_cast<unsigned char>(ch) >= 32) out += ch;
  SetText(c, out);
}

void T(Call& c) {
  const Value& v = c.arg(0);
  if (v.IsError()) return c.Fail(v.error());
  c.out = v.IsText() ? v : Value::Text("");
}

// _xlfn._LONGTEXT("...", "..."): files write a text constant over 255 characters in pieces.
void LongText(Call& c) {
  std::string out, s;
  for (int i = 0; i < c.argc; ++i) {
    if (!c.Text(i, s)) return;
    out += s;
  }
  SetText(c, std::move(out));
}

// INFO(type): what Excel 2021 answers on Windows (measured); "origin" is the top-left cell as the
// sheet opens, "directory" the current folder.
void Info(Call& c) {
  std::string s;
  if (!c.Text(0, s)) return;
  std::wstring w = MapCase(Wide(s), LCMAP_LOWERCASE);
  if (w == L"system") return SetText(c, "pcdos");
  if (w == L"release") return SetText(c, "16.0");
  if (w == L"osversion") return SetText(c, "Windows (64-bit) NT 10.00");
  if (w == L"recalc") return SetText(c, c.m.host.Manual() ? "Manual" : "Automatic");
  if (w == L"numfile") return c.SetNumber(c.m.host.SheetCount());
  if (w == L"origin") return SetText(c, "$A:$A$1");
  if (w == L"directory") {
    wchar_t dir[MAX_PATH + 1];
    DWORD n = GetCurrentDirectoryW(MAX_PATH, dir);
    std::wstring d(dir, n > 0 && n < MAX_PATH ? n : 0);
    if (d.empty() || d.back() != L'\\') d += L'\\';
    return SetWide(c, d);
  }
  if (w == L"memavail" || w == L"memused" || w == L"totmem") return c.Fail(Err::NA);
  c.Fail(Err::Value);
}

void Fixed(Call& c) {
  double x, d;
  bool noCommas;
  if (!c.Number(0, x) || !c.Number(1, d, 2) || !c.Boolean(2, noCommas)) return;
  d = std::trunc(d);
  if (d > 127) return c.Fail(Err::Value);
  std::string s = Grouped(x, static_cast<int>(d), !noCommas);
  bool zero = s.find_first_not_of("0.,") == std::string::npos;
  SetText(c, (x < 0 && !zero ? "-" : "") + s);
}

void Dollar(Call& c) {
  double x, d;
  if (!c.Number(0, x) || !c.Number(1, d, 2)) return;
  d = std::trunc(d);
  if (d > 127) return c.Fail(Err::Value);
  std::string s = "$" + Grouped(x, static_cast<int>(d), true);
  SetText(c, x < 0 ? "(" + s + ")" : s);
}

void NumberValue(Call& c) {
  std::string s, dec = ".", group = ",";
  if (!c.Text(0, s)) return;
  if (c.Has(1) && !c.Text(1, dec)) return;
  if (c.Has(2) && !c.Text(2, group)) return;
  if (dec.empty()) return c.Fail(Err::Value);
  std::string t;
  bool point = false;
  double pct = 1;
  for (char ch : s) {
    if (ch == ' ') continue;
    if (!group.empty() && ch == group[0] && !point) continue;
    if (ch == dec[0]) {
      if (point) return c.Fail(Err::Value);
      point = true;
      t += '.';
      continue;
    }
    if (ch == '%') {
      pct /= 100;
      continue;
    }
    t += ch;
  }
  if (t.empty()) return c.SetNumber(0);
  double d;
  if (!TextToNumber(t, d)) return c.Fail(Err::Value);
  c.SetNumber(d * pct);
}

void Hyperlink(Call& c) {
  int i = c.Has(1) ? 1 : 0;
  const Value& v = c.arg(i);
  if (v.IsMissing() || v.IsEmpty()) return c.SetNumber(0);
  c.out = v;
}


// --- Excel 365's text functions: TEXTSPLIT, TEXTBEFORE, TEXTAFTER (from Microsoft's documentation;
// Excel 2021 lacks them), VALUETOTEXT, ARRAYTOTEXT, UNICHAR, UNICODE (measured).

namespace {

std::wstring Folded(const std::wstring& w) {
  std::wstring f = w;
  if (!f.empty()) CharLowerBuffW(f.data(), static_cast<DWORD>(f.size()));
  return f;
}

// The delimiters an argument lists (one or an array of them).
bool Delimiters(Call& c, int i, bool fold, std::vector<std::wstring>& out) {
  if (!c.Has(i)) return true;
  Value v;
  if (!ArgArray(c, i, v)) return false;
  const Array& a = v.array();
  for (int32_t k = 0; k < a.size(); ++k) {
    const Value& e = a.data()[k];
    if (e.IsError()) return c.Fail(e.error()), false;
    std::wstring w = Wide(ValueText(e, c.m.decimal));
    out.push_back(fold ? Folded(w) : w);
  }
  return true;
}

// The first delimiter at or after `from` (the longest when several start there): its position and
// length, or false. Empty delimiters are skipped unless `empty` (TEXTBEFORE's "").
bool NextDelimiter(const std::wstring& s, size_t from, const std::vector<std::wstring>& ds, bool empty, size_t& at,
                   size_t& len) {
  for (size_t p = from; p <= s.size(); ++p) {
    bool found = false;
    for (const std::wstring& d : ds) {
      if (d.empty() && !empty) continue;
      if (p + d.size() <= s.size() && s.compare(p, d.size(), d) == 0 && (!found || d.size() > len))
        found = true, len = d.size();
    }
    if (found) return at = p, true;
  }
  return false;
}

// The last delimiter ending at or before `to`.
bool PrevDelimiter(const std::wstring& s, size_t to, const std::vector<std::wstring>& ds, size_t& at, size_t& len) {
  for (size_t e = to + 1; e-- > 0;) {
    bool found = false;
    for (const std::wstring& d : ds)
      if (d.size() <= e && s.compare(e - d.size(), d.size(), d) == 0 && (!found || d.size() > len))
        found = true, len = d.size();
    if (found) return at = e - len, true;
  }
  return false;
}

// `s` split where `key` (s, or s in lower case) has a delimiter.
std::vector<std::wstring> SplitBy(const std::wstring& s, const std::wstring& key, const std::vector<std::wstring>& ds,
                                  bool ignoreEmpty) {
  std::vector<std::wstring> out;
  size_t from = 0, at = 0, len = 0;
  while (NextDelimiter(key, from, ds, false, at, len)) {
    if (!(ignoreEmpty && at == from)) out.push_back(s.substr(from, at - from));
    from = at + len;
  }
  if (!(ignoreEmpty && from == s.size())) out.push_back(s.substr(from));
  return out;
}

bool AllEmpty(const std::vector<std::wstring>& ds) {
  for (const auto& d : ds)
    if (!d.empty()) return false;
  return true;
}

// A value as ARRAYTOTEXT and VALUETOTEXT show it; strict: text in quotes.
std::string Shown(const Value& v, bool strict, char decimal) {
  if (v.IsText() && strict) {
    std::string q = "\"";
    for (char ch : v.text()) q += ch == '"' ? std::string("\"\"") : std::string(1, ch);
    return q + "\"";
  }
  if (v.IsError()) return std::string(ErrText(v.error()));
  return ValueText(v, decimal);
}

bool StrictFormat(Call& c, int i, bool& strict) {
  double f;
  if (!c.Number(i, f, 0)) return false;
  f = std::trunc(f);
  if (f != 0 && f != 1) return c.Fail(Err::Value), false;
  strict = f == 1;
  return true;
}

}  // namespace

// TEXTSPLIT(text, col_delimiter, [row_delimiter], [ignore_empty], [match_mode], [pad_with]).
void TextSplit(Call& c) {
  std::string text;
  bool ignoreEmpty;
  double mode;
  if (!c.Text(0, text) || !c.Boolean(3, ignoreEmpty) || !c.Number(4, mode, 0)) return;
  bool fold = std::trunc(mode) == 1;
  std::vector<std::wstring> cols, rows;
  if (!Delimiters(c, 1, fold, cols) || !Delimiters(c, 2, fold, rows)) return;
  if (AllEmpty(cols) && AllEmpty(rows)) return c.Fail(Err::Value);
  std::wstring s = Wide(text), key = fold ? Folded(s) : s;
  std::vector<std::wstring> lines = SplitBy(s, key, rows, ignoreEmpty);
  std::vector<std::wstring> lineKeys = SplitBy(key, key, rows, ignoreEmpty);
  std::vector<std::vector<std::wstring>> grid;
  size_t width = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    grid.push_back(SplitBy(lines[i], lineKeys[i], cols, ignoreEmpty));
    width = std::max(width, grid.back().size());
  }
  if (grid.empty() || width == 0) return c.Fail(Err::Calc);
  if (!c.m.Fits(static_cast<double>(grid.size()), static_cast<double>(width))) return c.Fail(Err::Num);
  Value pad = c.Has(5) ? c.arg(5) : Value::Error(Err::NA);
  Array* out = Array::Make(static_cast<int32_t>(grid.size()), static_cast<int32_t>(width));
  for (size_t i = 0; i < grid.size(); ++i)
    for (size_t j = 0; j < width; ++j)
      out->at(static_cast<int32_t>(i), static_cast<int32_t>(j)) =
          j < grid[i].size() ? Value::Text(Narrow(grid[i][j])) : pad;
  c.out = Value::Adopt(out);
}

// TEXTBEFORE / TEXTAFTER(text, delimiter, [instance_num], [match_mode], [match_end], [if_not_found]).
static void TextAround(Call& c, bool before) {
  std::string text;
  double n, mode, end;
  if (!c.Text(0, text) || !c.Number(2, n, 1) || !c.Number(3, mode, 0) || !c.Number(4, end, 0)) return;
  n = std::trunc(n);
  bool fold = std::trunc(mode) == 1, matchEnd = end != 0;
  std::vector<std::wstring> ds;
  if (!Delimiters(c, 1, fold, ds)) return;
  if (!c.Has(1)) ds.emplace_back();
  std::wstring s = Wide(text), key = fold ? Folded(s) : s;
  if (n == 0 || std::fabs(n) > static_cast<double>(std::max<size_t>(s.size(), 1))) return c.Fail(Err::Value);
  size_t want = static_cast<size_t>(std::fabs(n)), count = 0, at = 0, len = 0;
  bool found = false;
  if (n > 0) {
    size_t from = 0;
    while (count < want && from <= s.size() && NextDelimiter(key, from, ds, true, at, len)) {
      if (++count == want) found = true;
      else from = at + std::max<size_t>(len, 1);
    }
    if (!found && matchEnd && count + 1 == want) found = true, at = s.size(), len = 0;
  } else {
    size_t to = s.size();
    while (count < want && PrevDelimiter(key, to, ds, at, len)) {
      if (++count == want) found = true;
      else if (at == 0) break;
      else to = len ? at : at - 1;
    }
    if (!found && matchEnd && count + 1 == want) found = true, at = 0, len = 0;
  }
  if (!found) {
    c.out = c.Has(5) ? c.arg(5) : Value::Error(Err::NA);
    if (c.out.IsArray()) c.out = Value(c.out.array().at(0, 0));
    return;
  }
  SetWide(c, before ? s.substr(0, at) : s.substr(at + len));
}
void TextBefore(Call& c) { TextAround(c, true); }
void TextAfter(Call& c) { TextAround(c, false); }

// VALUETOTEXT(value, [format]): errors are shown, not passed on.
void ValueToText(Call& c) {
  bool strict;
  if (!StrictFormat(c, 1, strict)) return;
  SetText(c, Shown(c.arg(0), strict, c.m.decimal));
}

// ARRAYTOTEXT(array, [format]): concise "1; a" with the region's list separator, strict with its
// array separators ({1\"a";TRUE\2,5} on this PC).
void ArrayToText(Call& c) {
  bool strict;
  if (!StrictFormat(c, 1, strict)) return;
  Value arr;
  if (!ArgArray(c, 0, arr)) return;
  const Array& a = arr.array();
  const bool comma = c.m.decimal == ',';
  const std::string list = comma ? "; " : ", ";
  const std::string col = comma ? "\\" : ",";
  std::string out = strict ? "{" : "";
  for (int32_t i = 0; i < a.rows(); ++i)
    for (int32_t j = 0; j < a.cols(); ++j) {
      if (i || j) out += !strict ? list : j ? col : std::string(";");
      out += Shown(a.at(i, j), strict, c.m.decimal);
      if (out.size() > kMaxText * 3) return c.Fail(Err::Value);
    }
  if (strict) out += "}";
  SetText(c, out);
}

void UniChar(Call& c) {
  double n;
  if (!c.Number(0, n)) return;
  n = std::trunc(n);
  if (n < 1 || n > 0x10FFFF) return c.Fail(Err::Value);
  uint32_t u = static_cast<uint32_t>(n);
  if ((u >= 0xD800 && u <= 0xDFFF) || (u & 0xFFFE) == 0xFFFE) return c.Fail(Err::NA);
  std::wstring w;
  if (u < 0x10000) {
    w.push_back(static_cast<wchar_t>(u));
  } else {
    u -= 0x10000;
    w.push_back(static_cast<wchar_t>(0xD800 + (u >> 10)));
    w.push_back(static_cast<wchar_t>(0xDC00 + (u & 0x3FF)));
  }
  SetWide(c, w);
}

void Unicode(Call& c) {
  std::string text;
  if (!c.Text(0, text)) return;
  std::wstring w = Wide(text);
  if (w.empty()) return c.Fail(Err::Value);
  uint32_t u = w[0];
  if (u >= 0xD800 && u <= 0xDBFF && w.size() > 1 && w[1] >= 0xDC00 && w[1] <= 0xDFFF)
    u = 0x10000 + ((u - 0xD800) << 10) + (w[1] - 0xDC00);
  c.SetNumber(u);
}

}  // namespace fw::sheet::calc::fn
