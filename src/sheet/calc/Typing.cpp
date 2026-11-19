// What the grid needs while a formula is typed: references, point mode, F4, argument tips, copies.

#include "sheet/calc/Typing.h"

#include "sheet/edit/Edit.h"

#include <algorithm>
#include <cwctype>

namespace fw::sheet::calc {

namespace {

constexpr int32_t kRows = 1048576, kCols = 16384;

bool Digit(wchar_t c) { return c >= L'0' && c <= L'9'; }
bool Letter(wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'); }
bool NameChar(wchar_t c) {
  return c >= 0x80 || Letter(c) || Digit(c) || c == L'_' || c == L'.' || c == L'\\' || c == L'?';
}

// Whether the text is a formula: its first character (after spaces) is =, + or -.
bool IsFormula(std::wstring_view s) {
  size_t i = 0;
  while (i < s.size() && s[i] == L' ') ++i;
  return i < s.size() && (s[i] == L'=' || s[i] == L'+' || s[i] == L'-');
}

// One past a quoted run's closing quote (`q` doubled inside stays in it), or the end.
size_t QuotedEnd(std::wstring_view s, size_t i, wchar_t q) {
  for (size_t j = i + 1; j < s.size(); ++j) {
    if (s[j] != q) continue;
    if (j + 1 < s.size() && s[j + 1] == q) {
      ++j;
      continue;
    }
    return j + 1;
  }
  return s.size();
}

// One past the bracket group starting at s[i] ([Book.xlsx], Table1[[#All],[a]]), or the end.
size_t BracketEnd(std::wstring_view s, size_t i) {
  int depth = 0;
  for (size_t j = i; j < s.size(); ++j) {
    if (s[j] == L'\'' && j + 1 < s.size()) {
      ++j;
      continue;
    }
    if (s[j] == L'[') ++depth;
    else if (s[j] == L']' && --depth == 0) return j + 1;
  }
  return s.size();
}

// One end of an A1 reference, as written.
struct End {
  bool hasRow = false, hasCol = false, rowAbs = false, colAbs = false;
  int32_t row = 0, col = 0;
  size_t begin = 0, end = 0;  // its text
};

// "$A$1", "A1", "$A", "A", "$1", "1" at `p`: its length, or 0.
size_t ScanEnd(std::wstring_view s, size_t p, End& out) {
  End t;
  size_t i = p, n = s.size();
  bool abs1 = i < n && s[i] == L'$';
  if (abs1) ++i;
  size_t ls = i;
  int64_t c = 0;
  while (i < n && Letter(s[i]) && i - ls < 4) c = c * 26 + (std::towupper(s[i++]) - L'A' + 1);
  size_t letters = i - ls;
  if (letters > 3 || (letters && c > kCols)) return 0;
  bool abs2 = abs1;
  if (letters) {
    t.hasCol = true, t.colAbs = abs1, t.col = static_cast<int32_t>(c - 1);
    abs2 = i < n && s[i] == L'$';
    if (abs2) ++i;
  }
  size_t ds = i;
  int64_t r = 0;
  while (i < n && Digit(s[i]) && i - ds < 8) r = r * 10 + (s[i++] - L'0');
  size_t digits = i - ds;
  if (digits) {
    if (digits > 7 || r < 1 || r > kRows) return 0;
    t.hasRow = true, t.rowAbs = abs2, t.row = static_cast<int32_t>(r - 1);
  } else if (abs2 && letters) {
    return 0;
  }
  if (!letters && !digits) return 0;
  if (i < n && (NameChar(s[i]) || s[i] == L'$')) return 0;  // part of a longer name
  t.begin = p, t.end = i;
  out = t;
  return i - p;
}

struct Ref {
  RefSpan span;
  End a, b;
  bool area = false;
};

// A reference (cell, cell:cell, col:col, row:row) at `p`, not followed by what would make it a name
// or a call. Its end, or 0.
size_t ScanRef(std::wstring_view s, size_t p, Ref& out) {
  End a, b;
  size_t la = ScanEnd(s, p, a);
  if (!la) return 0;
  size_t e = p + la;
  bool area = false;
  if (e < s.size() && s[e] == L':') {
    size_t lb = ScanEnd(s, e + 1, b);
    if (lb && ((a.hasRow && a.hasCol && b.hasRow && b.hasCol) || (!a.hasCol && !b.hasCol) || (!a.hasRow && !b.hasRow))) {
      area = true;
      e += 1 + lb;
    }
  }
  if (!area && !(a.hasRow && a.hasCol)) return 0;
  if (e < s.size() && (s[e] == L'(' || s[e] == L'!' || s[e] == L'[')) return 0;
  if (!area) b = a;
  out.a = a, out.b = b, out.area = area;
  RefSpan& r = out.span;
  r.r1 = a.hasRow ? std::min(a.row, b.row) : 0;
  r.r2 = a.hasRow ? std::max(a.row, b.row) : kRows - 1;
  r.c1 = a.hasCol ? std::min(a.col, b.col) : 0;
  r.c2 = a.hasCol ? std::max(a.col, b.col) : kCols - 1;
  return e;
}

// Every reference in a formula's text, with each end's place (for F4).
std::vector<Ref> Scan(std::wstring_view s, const FormulaLocale& region) {
  std::vector<Ref> refs;
  if (!IsFormula(s)) return refs;
  const size_t n = s.size();
  bool afterName = false;  // the last character belongs to a name, number or reference
  for (size_t i = 0; i < n;) {
    const wchar_t c = s[i];
    if (c == L'"') {
      i = QuotedEnd(s, i, c);
      afterName = false;
      continue;
    }
    if (c == L'{') {  // an array constant: no references
      while (i < n && s[i] != L'}') i = s[i] == L'"' ? QuotedEnd(s, i, L'"') : i + 1;
      ++i;
      continue;
    }
    if (c == L'[') {  // another workbook ([Book.xlsx]Sheet1!A1) or a table: skipped with what follows
      i = BracketEnd(s, i);
      while (i < n && (NameChar(s[i]) || s[i] == L'$' || s[i] == L'!' || s[i] == L':' || s[i] == L'[')) {
        if (s[i] == L'[') i = BracketEnd(s, i);
        else ++i;
      }
      afterName = true;
      continue;
    }
    Ref r;
    if (c == L'\'') {  // 'My sheet'!A1
      size_t q = QuotedEnd(s, i, c);
      if (q < n && s[q] == L'!' && q - i >= 2) {
        std::wstring sheet;
        for (size_t k = i + 1; k + 1 < q; ++k) {
          sheet += s[k];
          if (s[k] == L'\'') ++k;
        }
        size_t e = ScanRef(s, q + 1, r);
        if (e && sheet.find(L':') == std::wstring::npos && sheet.find(L'[') == std::wstring::npos) {
          r.span.begin = i, r.span.end = e, r.span.sheet = sheet;
          refs.push_back(r);
          i = e;
          afterName = true;
          continue;
        }
      }
      i = q;
      afterName = true;
      continue;
    }
    if (!afterName && (NameChar(c) || c == L'$')) {
      // A word: a sheet's name before "!", a function's before "(", a name, a reference or a number.
      size_t e = i;
      while (e < n && (NameChar(s[e]) || s[e] == L'$')) ++e;
      if (e < n && s[e] == L'!' && Letter(c)) {
        size_t end = ScanRef(s, e + 1, r);
        if (end) {
          r.span.begin = i, r.span.end = end, r.span.sheet.assign(s.substr(i, e - i));
          refs.push_back(r);
          i = end;
          afterName = true;
          continue;
        }
      }
      size_t end = ScanRef(s, i, r);
      if (end) {
        r.span.begin = i, r.span.end = end;
        refs.push_back(r);
        i = end;
        afterName = true;
        continue;
      }
      if (Digit(c) || c == region.decimal) {  // a number: digits, the decimal separator, an exponent
        size_t k = i;
        while (k < n && (Digit(s[k]) || s[k] == static_cast<wchar_t>(region.decimal))) ++k;
        if (k < n && (s[k] == L'E' || s[k] == L'e')) {
          ++k;
          if (k < n && (s[k] == L'+' || s[k] == L'-')) ++k;
          while (k < n && Digit(s[k])) ++k;
        }
        i = std::max(k, i + 1);
      } else {
        i = e;
        // Sheet1:Sheet3!A1 (several sheets): not colored.
        if (i < n && s[i] == L':') {
          size_t k = i + 1;
          while (k < n && NameChar(s[k])) ++k;
          if (k < n && s[k] == L'!') {
            Ref skip;
            size_t end2 = ScanRef(s, k + 1, skip);
            i = end2 ? end2 : k + 1;
          }
        }
      }
      afterName = true;
      continue;
    }
    afterName = NameChar(c) || c == L'$' || (afterName && (c == L'!' || c == L']'));
    ++i;
  }
  // Colors: in order, the same range on the same sheet keeping its color.
  std::vector<const RefSpan*> seen;
  for (Ref& r : refs) {
    int color = static_cast<int>(seen.size());
    for (size_t k = 0; k < seen.size(); ++k) {
      const RefSpan& o = *seen[k];
      if (o.r1 == r.span.r1 && o.r2 == r.span.r2 && o.c1 == r.span.c1 && o.c2 == r.span.c2 &&
          _wcsicmp(o.sheet.c_str(), r.span.sheet.c_str()) == 0) {
        color = o.color;
        break;
      }
    }
    r.span.color = color;
    if (color == static_cast<int>(seen.size())) seen.push_back(&r.span);
  }
  return refs;
}

std::wstring EndText(const End& e) {
  std::wstring t;
  if (e.hasCol) {
    if (e.colAbs) t += L'$';
    std::wstring letters;
    for (int32_t c = e.col + 1; c > 0; c = (c - 1) / 26) letters.insert(letters.begin(), static_cast<wchar_t>(L'A' + (c - 1) % 26));
    t += letters;
  }
  if (e.hasRow) {
    if (e.rowAbs) t += L'$';
    t += std::to_wstring(e.row + 1);
  }
  return t;
}

}  // namespace

std::vector<RefSpan> ReferenceSpans(std::wstring_view typed, const FormulaLocale& region) {
  std::vector<RefSpan> out;
  for (Ref& r : Scan(typed, region)) out.push_back(std::move(r.span));
  return out;
}

bool CanPointAt(std::wstring_view s, size_t caret, const FormulaLocale& region) {
  if (!IsFormula(s) || caret > s.size()) return false;
  // Outside text, arrays and brackets.
  for (size_t i = 0; i < caret;) {
    const wchar_t c = s[i];
    size_t e = i + 1;
    if (c == L'"' || c == L'\'') {
      e = QuotedEnd(s, i, c);
      if (e <= i + 1 || s[e - 1] != c) return false;  // not closed: the caret is in it
    } else if (c == L'[') {
      e = BracketEnd(s, i);
    }
    else if (c == L'{') {
      e = i;
      while (e < s.size() && s[e] != L'}') e = s[e] == L'"' ? QuotedEnd(s, e, L'"') : e + 1;
      e = e < s.size() ? e + 1 : e;
    }
    if (e > caret) return false;
    i = e;
  }
  size_t k = caret;
  while (k > 0 && s[k - 1] == L' ') --k;
  if (k == 0) return false;
  const wchar_t prev = s[k - 1];
  const bool operand = prev == L'=' || prev == L'+' || prev == L'-' || prev == L'*' || prev == L'/' || prev == L'^' ||
                       prev == L'&' || prev == L'<' || prev == L'>' || prev == L'(' || prev == L':' ||
                       prev == static_cast<wchar_t>(region.list);
  if (!operand) return false;
  if (caret < s.size()) {
    const wchar_t next = s[caret];
    if (NameChar(next) || next == L'$' || next == L'\'' || next == L'"' || next == L'(' || next == L'{') return false;
  }
  return true;
}

bool CycleReference(std::wstring& text, size_t& selBegin, size_t& selEnd, const FormulaLocale& region) {
  std::vector<Ref> refs = Scan(text, region);
  std::vector<Ref*> hit;
  for (Ref& r : refs) {
    if (selBegin == selEnd ? (r.span.begin <= selBegin && selBegin <= r.span.end)
                           : (r.span.begin < selEnd && r.span.end > selBegin))
      hit.push_back(&r);
    if (selBegin == selEnd && !hit.empty()) break;
  }
  if (hit.empty()) return false;
  // The next form after the first one's: A1, $A$1, A$1, $A1, A1.
  const End& first = hit.front()->a;
  bool colAbs, rowAbs;
  if (!first.hasRow) colAbs = !first.colAbs, rowAbs = false;
  else if (!first.hasCol) rowAbs = !first.rowAbs, colAbs = false;
  else if (!first.colAbs && !first.rowAbs) colAbs = rowAbs = true;
  else if (first.colAbs && first.rowAbs) colAbs = false, rowAbs = true;
  else if (!first.colAbs && first.rowAbs) colAbs = true, rowAbs = false;
  else colAbs = rowAbs = false;
  ptrdiff_t shift = 0;
  size_t newBegin = 0, newEnd = 0;
  for (size_t k = 0; k < hit.size(); ++k) {
    Ref& r = *hit[k];
    End a = r.a, b = r.b;
    for (End* e : {&a, &b}) {
      e->colAbs = e->hasCol && colAbs;
      e->rowAbs = e->hasRow && rowAbs;
    }
    const size_t from = r.a.begin + shift, to = (r.area ? r.b.end : r.a.end) + shift;
    std::wstring repl = EndText(a);
    if (r.area) repl += L':' + EndText(b);
    text.replace(from, to - from, repl);
    const ptrdiff_t delta = static_cast<ptrdiff_t>(repl.size()) - static_cast<ptrdiff_t>(to - from);
    if (k == 0) newBegin = r.span.begin + shift;
    shift += delta;
    newEnd = r.span.end + shift;
  }
  if (selBegin == selEnd) selBegin = selEnd = newEnd;
  else selBegin = newBegin, selEnd = newEnd;
  return true;
}

bool CallAt(std::wstring_view s, size_t caret, const FormulaLocale& region, std::wstring& function, int& argument) {
  if (!IsFormula(s)) return false;
  struct Open {
    std::wstring name;  // empty: brackets that aren't a call
    int arg = 0;
  };
  std::vector<Open> open;
  const size_t end = std::min(caret, s.size());
  for (size_t i = 0; i < end;) {
    const wchar_t c = s[i];
    if (c == L'"' || c == L'\'') {
      i = QuotedEnd(s, i, c);
      continue;
    }
    if (c == L'[') {
      i = BracketEnd(s, i);
      continue;
    }
    if (c == L'{') {
      while (i < end && s[i] != L'}') i = s[i] == L'"' ? QuotedEnd(s, i, L'"') : i + 1;
      ++i;
      continue;
    }
    if (c == L'(') {
      size_t k = i;
      while (k > 0 && (NameChar(s[k - 1]))) --k;
      std::wstring name(s.substr(k, i - k));
      if (!name.empty() && (Digit(name[0]) || name[0] == L'.')) name.clear();
      open.push_back({name, 0});
    } else if (c == L')') {
      if (!open.empty()) open.pop_back();
    } else if (c == static_cast<wchar_t>(region.list) && !open.empty()) {
      ++open.back().arg;
    }
    ++i;
  }
  for (size_t k = open.size(); k-- > 0;) {
    if (open[k].name.empty()) continue;
    function = open[k].name;
    argument = open[k].arg;
    return true;
  }
  return false;
}

std::string CopyFormula(std::string_view formula, const CompileOptions& from, const CompileOptions& to, Names& names) {
  auto f = Compile(formula, from, names);
  if (!f) return std::string(formula);
  PrintOptions how;
  how.offSheetRef = true;
  return "=" + Print(*f, to, names, how);
}

}  // namespace fw::sheet::calc

namespace fw::sheet::calc {

namespace {

std::string A1(int32_t r, int32_t c) { return ColumnName(c) + std::to_string(r + 1); }

// From (row, col) toward the sheet's edge (dr, dc): blank cells, then the run of numbers. The run's
// far end, or false when there are no numbers before text, a boolean, an error or the edge.
bool Run(int32_t row, int32_t col, int dr, int dc, const std::function<SumCell(int32_t, int32_t)>& at, int32_t& r,
         int32_t& c) {
  r = row + dr, c = col + dc;
  while (r >= 0 && c >= 0 && at(r, c) == SumCell::kEmpty) r += dr, c += dc;
  if (r < 0 || c < 0) return false;
  SumCell k = at(r, c);
  if (k != SumCell::kNumber && k != SumCell::kTotal) return false;
  while (r + dr >= 0 && c + dc >= 0) {
    SumCell n = at(r + dr, c + dc);
    if (n != SumCell::kNumber && n != SumCell::kTotal) break;
    r += dr, c += dc;
  }
  return true;
}

}  // namespace

AutoSumProposal ProposeAutoSum(int32_t row, int32_t col, const std::function<SumCell(int32_t, int32_t)>& at) {
  AutoSumProposal p;
  auto number = [&](int32_t r, int32_t c) {
    if (r < 0 || c < 0) return false;
    SumCell k = at(r, c);
    return k == SumCell::kNumber || k == SumCell::kTotal;
  };
  // The column when there are numbers right above (or, past blanks, above and none right left).
  int dr = 0, dc = 0;
  int32_t r = 0, c = 0;
  if (number(row - 1, col)) dr = -1;
  else if (number(row, col - 1)) dc = -1;
  else if (Run(row, col, -1, 0, at, r, c)) dr = -1;
  else if (Run(row, col, 0, -1, at, r, c)) dc = -1;
  if (!dr && !dc) {
    p.formula = "=SUM()";
    return p;
  }
  Run(row, col, dr, dc, at, r, c);
  p.found = true;
  p.r1 = std::min(r, row + dr), p.r2 = std::max(r, row + dr);
  p.c1 = std::min(c, col + dc), p.c2 = std::max(c, col + dc);
  // Totals in the run: sum those only, nearest first.
  for (int32_t k = dr ? row - 1 : col - 1; dr ? k >= p.r1 : k >= p.c1; --k) {
    int32_t rr = dr ? k : row, cc = dr ? col : k;
    if (at(rr, cc) == SumCell::kTotal) p.cells.emplace_back(rr, cc);
  }
  if (!p.cells.empty()) {
    std::string list;
    for (auto [rr, cc] : p.cells) list += (list.empty() ? "" : ",") + A1(rr, cc);
    p.formula = "=SUM(" + list + ")";
    return p;
  }
  const bool one = p.r1 == p.r2 && p.c1 == p.c2;
  p.formula = "=SUM(" + A1(p.r1, p.c1) + (one ? "" : ":" + A1(p.r2, p.c2)) + ")";
  return p;
}

std::vector<AutoSumCell> ProposeAutoSums(int32_t r1, int32_t c1, int32_t r2, int32_t c2, int32_t lastRow,
                                         int32_t lastCol, const std::function<SumCell(int32_t, int32_t)>& at) {
  std::vector<AutoSumCell> out;
  if (r1 > r2 || c1 > c2 || r1 < 0 || c1 < 0) return out;
  auto empty = [&](int32_t ra, int32_t ca, int32_t rb, int32_t cb) {
    for (int32_t r = ra; r <= rb; ++r)
      for (int32_t c = ca; c <= cb; ++c)
        if (at(r, c) != SumCell::kEmpty) return false;
    return true;
  };
  if (empty(r1, c1, r2, c2)) {
    for (int32_t r = r1; r <= r2; ++r)
      for (int32_t c = c1; c <= c2; ++c)
        if (AutoSumProposal p = ProposeAutoSum(r, c, at); p.found) out.push_back({r, c, std::move(p.formula)});
    return out;
  }
  // One line's total (column c rows ra-rb, or row r columns ca-cb): its totals only when it holds
  // some, nearest the total first; else the whole line.
  struct Line {
    bool number = false;
    std::string formula;
  };
  auto line = [&](int32_t ra, int32_t ca, int32_t rb, int32_t cb) {
    Line l;
    std::string totals;
    for (int32_t r = rb, c = cb; r >= ra && c >= ca; ra == rb ? --c : --r) {
      const SumCell k = at(r, c);
      l.number |= k == SumCell::kNumber || k == SumCell::kTotal;
      if (k == SumCell::kTotal) totals += (totals.empty() ? "" : ",") + A1(r, c);
    }
    const bool one = ra == rb && ca == cb;
    l.formula = "=SUM(" + (!totals.empty() ? totals : A1(ra, ca) + (one ? "" : ":" + A1(rb, cb))) + ")";
    return l;
  };
  // The columns' totals in row `to` (data rows ra-rb, columns ca-cb), under those holding a number.
  auto columns = [&](int32_t ra, int32_t rb, int32_t ca, int32_t cb, std::vector<AutoSumCell>& cells) {
    for (int32_t c = ca; c <= cb; ++c)
      if (Line l = line(ra, c, rb, c); l.number) cells.push_back({rb + 1, c, std::move(l.formula)});
    if (cells.empty())
      for (int32_t c = ca; c <= cb; ++c) cells.push_back({rb + 1, c, line(ra, c, rb, c).formula});
  };
  auto rows = [&](int32_t ra, int32_t rb, int32_t ca, int32_t cb, std::vector<AutoSumCell>& cells) {
    for (int32_t r = ra; r <= rb; ++r)
      if (Line l = line(r, ca, r, cb); l.number) cells.push_back({r, cb + 1, std::move(l.formula)});
    if (cells.empty())
      for (int32_t r = ra; r <= rb; ++r) cells.push_back({r, cb + 1, line(r, ca, r, cb).formula});
  };
  const bool lastRowEmpty = r2 > r1 && empty(r2, c1, r2, c2);
  const bool lastColEmpty = c2 > c1 && empty(r1, c2, r2, c2);
  if (lastRowEmpty || lastColEmpty) {
    const int32_t dr = lastRowEmpty ? r2 - 1 : r2, dc = lastColEmpty ? c2 - 1 : c2;
    if (lastRowEmpty) columns(r1, dr, c1, dc, out);
    std::vector<AutoSumCell> side;
    if (lastColEmpty) rows(r1, dr, c1, dc, side);
    out.insert(out.end(), side.begin(), side.end());
    if (lastRowEmpty && lastColEmpty)
      out.push_back({r2, c2, "=SUM(" + A1(r1, c2) + (dr > r1 ? ":" + A1(dr, c2) : "") + ")"});
    return out;
  }
  if (r1 == r2) {
    // One row: right of it, in the first empty cell.
    for (int32_t c = c2 + 1; c <= lastCol; ++c)
      if (at(r1, c) == SumCell::kEmpty) {
        out.push_back({r1, c, line(r1, c1, r1, c2).formula});
        return out;
      }
    return out;
  }
  // Under each column, in the first row below whose cells (under the columns totalled) are empty.
  std::vector<AutoSumCell> cells;
  columns(r1, r2, c1, c2, cells);
  for (int32_t r = r2 + 1; r <= lastRow; ++r) {
    bool free = true;
    for (const AutoSumCell& t : cells) free = free && at(r, t.col) == SumCell::kEmpty;
    if (!free) continue;
    for (AutoSumCell& t : cells) t.row = r;
    return cells;
  }
  return out;
}

}  // namespace fw::sheet::calc

namespace fw::sheet::calc {

namespace {

class ShiftMap : public RefMap {
 public:
  ShiftMap(const StructuralEdit& e, const CompileOptions& from, const CompileOptions& to) : e_(e), from_(from), to_(to) {}

  // One line (row or column) number after the edit, or false when it's deleted.
  bool Along(int64_t& a1, int64_t& a2) const {
    const int64_t max = e_.rows ? 1048576 : 16384;
    if (e_.count > 0) {
      if (a1 >= e_.at) a1 += e_.count, a2 += e_.count;
      else if (a2 >= e_.at) a2 += e_.count;
      return a2 < max;
    }
    const int64_t d = -static_cast<int64_t>(e_.count), d1 = e_.at, d2 = e_.at + d - 1;
    if (a1 >= d1 && a2 <= d2) return false;
    int64_t n1 = a1 < d1 ? a1 : (a1 > d2 ? a1 - d : d1);
    int64_t n2 = a2 < d1 ? a2 : (a2 > d2 ? a2 - d : d1 - 1);
    a1 = n1, a2 = n2;
    return true;
  }
  bool Moves(int64_t across1, int64_t across2) const {
    return e_.band1 < 0 || (across1 >= e_.band1 && across2 <= e_.band2);
  }

  bool Map(int sheet, int32_t& r1, int32_t& c1, int32_t& r2, int32_t& c2, uint8_t abs, bool wholeRows,
           bool wholeCols) const override {
    // A reference that stays still reads from the formula's cell, which may have moved: its
    // relative parts become offsets from the new cell (Sheet3!A5 in a cell moved down stays A5).
    auto stay = [&]() {
      const int32_t dr = from_.row - to_.row, dc = from_.col - to_.col;
      if (!wholeCols) {
        if (!(abs & kRow1)) r1 += dr;
        if (!(abs & kRow2)) r2 += dr;
      }
      if (!wholeRows) {
        if (!(abs & kCol1)) c1 += dc;
        if (!(abs & kCol2)) c2 += dc;
      }
      return true;
    };
    if (sheet != e_.sheet) return stay();
    if (e_.rows ? wholeCols : wholeRows) return stay();                       // spans every line edited
    if (e_.band1 >= 0 && (e_.rows ? wholeRows : wholeCols)) return stay();    // cells: whole lines stay
    int64_t R1 = (abs & kRow1) ? r1 : int64_t{from_.row} + r1, R2 = (abs & kRow2) ? r2 : int64_t{from_.row} + r2;
    int64_t C1 = (abs & kCol1) ? c1 : int64_t{from_.col} + c1, C2 = (abs & kCol2) ? c2 : int64_t{from_.col} + c2;
    if (e_.rows ? !Moves(C1, C2) : !Moves(R1, R2)) {
      // Unchanged, but relative parts are now offsets from the formula's new cell. Except a
      // reference lying in deleted cells' lines with one end in the band: what's left of it.
      int64_t& a1 = e_.rows ? R1 : C1;
      int64_t& a2 = e_.rows ? R2 : C2;
      int64_t& x1 = e_.rows ? C1 : R1;
      int64_t& x2 = e_.rows ? C2 : R2;
      const bool inDeleted = e_.count < 0 && a1 >= e_.at && a2 <= e_.at - static_cast<int64_t>(e_.count) - 1;
      const bool overlaps = x2 >= e_.band1 && x1 <= e_.band2;
      if (inDeleted && overlaps) {
        if (x1 >= e_.band1) x1 = e_.band2 + 1;
        else if (x2 <= e_.band2) x2 = e_.band1 - 1;
      }
    } else if (!(e_.rows ? Along(R1, R2) : Along(C1, C2))) {
      return false;
    }
    r1 = static_cast<int32_t>((abs & kRow1) ? R1 : R1 - to_.row);
    r2 = static_cast<int32_t>((abs & kRow2) ? R2 : R2 - to_.row);
    c1 = static_cast<int32_t>((abs & kCol1) ? C1 : C1 - to_.col);
    c2 = static_cast<int32_t>((abs & kCol2) ? C2 : C2 - to_.col);
    return true;
  }

 private:
  const StructuralEdit& e_;
  const CompileOptions &from_, &to_;
};

// Another map, noting whether it changed what any reference names (printed for `to` instead of
// `from`): when nothing did, the formula keeps its text as written.
class Watch : public RefMap {
 public:
  Watch(const RefMap& map, const CompileOptions& from, const CompileOptions& to) : map_(map), from_(from), to_(to) {}
  mutable bool changed = false;

  bool Map(int sheet, int32_t& r1, int32_t& c1, int32_t& r2, int32_t& c2, uint8_t abs, bool wholeRows,
           bool wholeCols) const override {
    const int32_t o[4] = {r1, c1, r2, c2};
    if (!map_.Map(sheet, r1, c1, r2, c2, abs, wholeRows, wholeCols)) return changed = true, false;
    auto line = [](int32_t v, bool fixed, int32_t at) { return fixed ? int64_t{v} : int64_t{at} + v; };
    if (!wholeCols &&
        (line(o[0], abs & kRow1, from_.row) != line(r1, abs & kRow1, to_.row) ||
         line(o[2], abs & kRow2, from_.row) != line(r2, abs & kRow2, to_.row)))
      changed = true;
    if (!wholeRows &&
        (line(o[1], abs & kCol1, from_.col) != line(c1, abs & kCol1, to_.col) ||
         line(o[3], abs & kCol2, from_.col) != line(c2, abs & kCol2, to_.col)))
      changed = true;
    return true;
  }
  bool MapSheets(int& s1, int& s2) const override {
    const int o1 = s1, o2 = s2;
    const bool ok = map_.MapSheets(s1, s2);
    if (!ok || s1 != o1 || s2 != o2) changed = true;
    return ok;
  }

 private:
  const RefMap& map_;
  const CompileOptions &from_, &to_;
};

}  // namespace

std::string ShiftReferences(std::string_view formula, const CompileOptions& at, const StructuralEdit& edit, Names& names,
                            CompileOptions* moved) {
  CompileOptions to = at;
  if (at.sheet == edit.sheet) {
    // The formula's own cell moves like a cell reference to it.
    ShiftMap own(edit, at, at);
    int32_t r1 = at.row, c1 = at.col, r2 = at.row, c2 = at.col;
    if (own.Map(at.sheet, r1, c1, r2, c2, RefMap::kRow1 | RefMap::kCol1 | RefMap::kRow2 | RefMap::kCol2, false, false))
      to.row = r1, to.col = c1;
  }
  if (moved) *moved = to;
  ShiftMap map(edit, at, to);
  // Most formulas: only the references that move are rewritten, in the text as written.
  std::string out;
  auto refErrors = [](std::string_view s) {
    size_t count = 0;
    for (size_t p = s.find("#REF!"); p != std::string_view::npos; p = s.find("#REF!", p + 5)) ++count;
    return count;
  };
  std::string text;  // the rewrite in the text, when a reference in it became #REF!
  switch (MapReferenceText(formula, at, to, names, map, out)) {
    case 1:
      if (refErrors(out) == refErrors(formula)) return out;
      text = std::move(out);
      break;
    case 0: return std::string(formula);
    default: break;
  }
  auto f = Compile(formula, at, names);
  if (!f) return text.empty() ? std::string(formula) : text;
  Watch watch(map, at, to);
  PrintOptions how;
  how.map = &watch;
  out = Print(*f, to, names, how);
  if (!text.empty()) {
    // A #REF! can change how the text around it reads (after "#REF!:" a cell-like word such as
    // SUM01 reads as a name): the text as written only when it still means what the printer's does.
    auto a = Compile(text, to, names), b = Compile(out, to, names);
    return a && b && Print(*a, to, names) == Print(*b, to, names) ? text : out;
  }
  return watch.changed ? out : std::string(formula);
}

std::string ShiftReferences(std::string_view formula, const CompileOptions& at, int editedSheet, const edit::Shift& shift,
                            Names& names, CompileOptions* moved) {
  StructuralEdit e;
  e.sheet = editedSheet;
  e.rows = shift.axis == edit::Shift::Axis::Rows;
  e.at = static_cast<int32_t>(shift.at);
  e.count = shift.count;
  const uint32_t b1 = e.rows ? shift.band.firstCol : shift.band.firstRow;
  const uint32_t b2 = e.rows ? shift.band.lastCol : shift.band.lastRow;
  if (b1 != 0 || b2 < (e.rows ? fw::sheet::kMaxCols : fw::sheet::kMaxRows) - 1)
    e.band1 = static_cast<int32_t>(b1), e.band2 = static_cast<int32_t>(b2);
  return ShiftReferences(formula, at, e, names, moved);
}

namespace {

// The sheets before the edit, and defined names by text (they only need to print back).
class SheetList : public Names {
 public:
  explicit SheetList(const std::vector<std::string>& sheets) : sheets_(sheets) {}
  int FindSheet(std::string_view name) const override {
    for (size_t i = 0; i < sheets_.size(); ++i)
      if (NameEqualsNoCase(sheets_[i], name)) return static_cast<int>(i);
    return -1;
  }
  std::string SheetName(int sheet) const override {
    return sheet >= 0 && static_cast<size_t>(sheet) < sheets_.size() ? sheets_[static_cast<size_t>(sheet)] : std::string();
  }
  uint32_t NameSlot(std::string_view name) override {
    for (size_t i = 0; i < names_.size(); ++i)
      if (NameEqualsNoCase(names_[i], name)) return static_cast<uint32_t>(i);
    names_.emplace_back(name);
    return static_cast<uint32_t>(names_.size() - 1);
  }
  std::string SlotName(uint32_t slot) const override { return slot < names_.size() ? names_[slot] : std::string(); }

 private:
  const std::vector<std::string>& sheets_;
  std::vector<std::string> names_;
};

// Sheet indexes stay those before the edit (the text has names): only which sheets a prefix names.
class SheetMap : public RefMap {
 public:
  SheetMap(int sheet, int to) : sheet_(sheet), to_(to) {}
  bool Map(int, int32_t&, int32_t&, int32_t&, int32_t&, uint8_t, bool, bool) const override { return true; }
  bool MapSheets(int& s1, int& s2) const override {
    if (to_ < 0) {  // deleted
      if (s1 == sheet_ && s2 == sheet_) return false;
      if (s1 == sheet_) ++s1;
      else if (s2 == sheet_) --s2;
      return true;
    }
    if (s1 != s2 && Position(s1) > Position(s2)) {  // an end moved past the other
      if (s1 == sheet_) ++s1;
      else if (s2 == sheet_) --s2;
    }
    return true;
  }

 private:
  int Position(int i) const {
    if (i == sheet_) return to_;
    int j = i > sheet_ ? i - 1 : i;
    return j >= to_ ? j + 1 : j;
  }
  int sheet_, to_;
};

}  // namespace

std::string SheetsChanged(std::string_view formula, const CompileOptions& at, const std::vector<std::string>& sheets,
                          int sheet, int to) {
  if (sheet < 0 || static_cast<size_t>(sheet) >= sheets.size() || to == sheet) return std::string(formula);
  SheetList names(sheets);
  auto f = Compile(formula, at, names);
  if (!f) return std::string(formula);
  SheetMap map(sheet, to);
  PrintOptions how;
  how.map = &map;
  return Print(*f, at, names, how);
}

}  // namespace fw::sheet::calc
