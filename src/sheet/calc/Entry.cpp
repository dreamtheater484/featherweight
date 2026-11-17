// Formula entry: the region's separators to the invariant form and back, and Excel's messages.

#include "sheet/calc/Entry.h"

#include <algorithm>
#include <cctype>
#include <vector>

#include "sheet/numfmt/NumLocale.h"

namespace fw::sheet::calc {

namespace {

bool IsDigit(char c) { return c >= '0' && c <= '9'; }
// Characters of names, references and function names (A1, $B$2, Sheet1, STDEV.S, _xlfn.X, \name).
bool IsNameChar(char c) {
  const auto u = static_cast<unsigned char>(c);
  return u >= 0x80 || std::isalnum(u) || c == '_' || c == '.' || c == '\\' || c == '$' || c == '?';
}

// The end of a quoted run starting at s[i] (`q` doubled inside stays in it): one past the closing
// quote, or s.size() when there's none.
size_t QuotedEnd(std::string_view s, size_t i, char q) {
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

// `s` with `from`'s separators made `to`'s, in place in `out` (same length). Stops at a separator
// `to` uses that `from` doesn't, outside text, names and numbers.
bool Convert(std::string_view s, const FormulaLocale& from, const FormulaLocale& to, std::string& out,
             size_t* errorPos, int* closeCalls) {
  out.assign(s);
  std::vector<bool> calls;  // open brackets: a function call's?
  bool array = false, afterName = false;
  auto fail = [&](size_t i) {
    if (errorPos) *errorPos = i;
    return false;
  };
  const size_t n = s.size();
  for (size_t i = 0; i < n;) {
    const char c = s[i];
    if (c == '"' || c == '\'') {
      i = QuotedEnd(s, i, c);
      afterName = c == '\'';  // 'My sheet'!A1
      continue;
    }
    if (c == '[' && !array) {
      // A structured reference or another workbook: copied, except the separators between its
      // parts (Table1[[#This Row],[Amount]]).
      int depth = 0;
      size_t j = i;
      for (; j < n; ++j) {
        if (s[j] == '\'' && j + 1 < n) {
          ++j;  // escapes the next character
          continue;
        }
        if (s[j] == '[') ++depth;
        else if (s[j] == ']' && --depth == 0) break;
        else if (depth == 1 && s[j] == from.list && j > 0 && s[j - 1] == ']') out[j] = to.list;
      }
      i = j < n ? j + 1 : n;
      afterName = true;
      continue;
    }
    if (!afterName && (IsDigit(c) || (c == from.decimal && i + 1 < n && IsDigit(s[i + 1])))) {
      // A number: digits, the decimal separator and more digits, an exponent.
      size_t j = i;
      while (j < n && IsDigit(s[j])) ++j;
      if (j < n && s[j] == from.decimal) {
        out[j++] = to.decimal;
        while (j < n && IsDigit(s[j])) ++j;
      }
      // The other form's decimal separator (=1.5 with a decimal comma), unless it's a separator here.
      const char d = j < n ? s[j] : 0;
      if (d && d == to.decimal && d != from.decimal && d != from.list &&
          !(array && (d == from.arrayCol || d == from.arrayRow)))
        return fail(j);
      if (j < n && (s[j] == 'E' || s[j] == 'e') && j + 1 < n &&
          (IsDigit(s[j + 1]) || ((s[j + 1] == '+' || s[j + 1] == '-') && j + 2 < n && IsDigit(s[j + 2])))) {
        j += 2;
        while (j < n && IsDigit(s[j])) ++j;
      }
      i = j;
      afterName = false;
      continue;
    }
    if (array) {
      if (c == '}') {
        array = false;
      } else if (c == from.arrayCol || c == from.arrayRow) {
        out[i] = c == from.arrayCol ? to.arrayCol : to.arrayRow;
        afterName = false;
        ++i;
        continue;
      } else if (c == to.arrayCol || c == to.arrayRow || c == to.decimal || c == to.list) {
        return fail(i);
      }
    } else if (c == '{') {
      array = true;
    } else if (c == from.list) {
      out[i] = to.list;
    } else if (c == to.list || ((c == from.decimal || c == to.decimal) && !afterName)) {
      return fail(i);
    } else if (c == '(') {
      calls.push_back(afterName);
    } else if (c == ')') {
      if (!calls.empty()) calls.pop_back();
    }
    afterName = IsNameChar(c) || (afterName && c == '!');
    ++i;
  }
  if (closeCalls) {
    *closeCalls = static_cast<int>(calls.size());
    for (bool call : calls)
      if (!call) *closeCalls = -1;
  }
  return true;
}

const char* kProblem = "There's a problem with this formula.";

std::string MessageFor(const std::string& parser) {
  if (parser == "too few arguments") return "You've entered too few arguments for this function.";
  if (parser == "too many arguments") return "You've entered too many arguments for this function.";
  return kProblem;
}

}  // namespace

const FormulaLocale& InvariantFormulaLocale() {
  static const FormulaLocale invariant;
  return invariant;
}

FormulaLocale MakeFormulaLocale(const NumLocale& region) {
  auto one = [](const std::wstring& w) -> char {
    if (w.size() != 1 || w[0] >= 0x80) return 0;
    char c = static_cast<char>(w[0]);
    // Not a character with its own meaning in a formula.
    if (std::isalnum(static_cast<unsigned char>(c)) || std::strchr("\"'()[]{}+-*/^&=<>%:!$#@_ \\", c)) return 0;
    return c;
  };
  FormulaLocale f;
  const char decimal = one(region.decimal), list = one(region.list);
  if (!decimal || !list || decimal == list) return f;
  f.decimal = decimal;
  f.list = list;
  f.arrayCol = decimal == ',' ? '\\' : ',';
  f.arrayRow = ';';
  if (f.arrayCol == list) return InvariantFormulaLocale();
  return f;
}

bool ToInvariant(std::string_view typed, const FormulaLocale& region, std::string& out, size_t* errorPos,
                 int* closeCalls) {
  return Convert(typed, region, InvariantFormulaLocale(), out, errorPos, closeCalls);
}

std::string ToLocal(std::string_view invariant, const FormulaLocale& region) {
  std::string out;
  if (!Convert(invariant, InvariantFormulaLocale(), region, out, nullptr, nullptr)) out.assign(invariant);
  return out;
}

EnteredFormula EnterFormula(std::string_view typed, const FormulaLocale& region, const CompileOptions& at,
                            Names& names) {
  EnteredFormula e;
  auto refuse = [&](size_t pos, std::string message) {
    e.formula.reset();
    e.pos = std::min(pos, typed.size());
    e.message = std::move(message);
    return e;
  };
  std::string text;
  size_t bad = 0;
  int open = 0;
  if (!ToInvariant(typed, region, text, &bad, &open)) return refuse(bad, kProblem);
  // Excel closes the function calls left open at the end; another open bracket is a problem.
  if (open < 0) return refuse(typed.size(), kProblem);
  text.append(static_cast<size_t>(open), ')');
  ParseError error;
  CompileOptions typedAt = at;
  typedAt.typed = true;
  e.formula = Compile(text, typedAt, names, &error);
  if (!e.formula) return refuse(error.pos, MessageFor(error.message));
  PrintOptions file;
  e.text = "=" + Print(*e.formula, at, names, file);
  e.shown = ShowFormula(*e.formula, at, names, region);
  return e;
}

std::string ShowFormula(const Formula& f, const CompileOptions& at, const Names& names, const FormulaLocale& region) {
  PrintOptions bar;
  bar.file = false;
  return "=" + ToLocal(Print(f, at, names, bar), region);
}

}  // namespace fw::sheet::calc
