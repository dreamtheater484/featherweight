// What the grid needs while a formula is typed: references, point mode, F4, argument tips, copies.

#include "sheet/calc/Typing.h"

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
