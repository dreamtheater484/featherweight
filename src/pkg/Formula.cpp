#include "pkg/Formula.h"

namespace fw::pkg {
namespace {

bool IsAsciiLetter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
// Characters of an unquoted name: sheet names, defined names, function names, cell references
// (with their $ signs).
bool IsNameStart(char c) {
  return IsAsciiLetter(c) || c == '_' || c == '\\' || c == '$' || static_cast<unsigned char>(c) >= 0x80;
}
bool IsNameChar(char c) { return IsNameStart(c) || IsDigit(c) || c == '.'; }

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

bool EqualNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (Lower(a[i]) != Lower(b[i])) return false;
  return true;
}

size_t SkipString(std::string_view f, size_t i) {  // i is at the opening quote
  for (++i; i < f.size(); ++i) {
    if (f[i] != '"') continue;
    if (i + 1 < f.size() && f[i + 1] == '"') {
      ++i;
      continue;
    }
    return i + 1;
  }
  return f.size();
}

size_t SkipBrackets(std::string_view f, size_t i) {  // i is at '[': structured references nest
  int depth = 0;
  for (; i < f.size(); ++i) {
    if (f[i] == '[') ++depth;
    else if (f[i] == ']' && --depth == 0) return i + 1;
  }
  return f.size();
}

size_t SkipName(std::string_view f, size_t i) {
  while (i < f.size() && IsNameChar(f[i])) ++i;
  return i;
}

// Walks a formula, telling `onSheet` about sheet prefixes and `onName` about other names
// (function names, defined names, cell references) with whether a "(" follows.
void Scan(std::string_view f, const std::function<void(const SheetToken&)>& onSheet,
          const std::function<void(std::string_view, bool)>& onName) {
  size_t i = 0;
  bool external = false;  // just after [n]: the sheet that follows is in another workbook
  while (i < f.size()) {
    char c = f[i];
    if (c == '"') {
      i = SkipString(f, i);
      external = false;
    } else if (c == '[') {
      i = SkipBrackets(f, i);
      external = true;
      continue;
    } else if (c == '#') {  // an error value: #REF!, #DIV/0!, #NAME?, #N/A
      ++i;
      while (i < f.size() && (IsNameChar(f[i]) || f[i] == '/')) ++i;
      if (i < f.size() && (f[i] == '!' || f[i] == '?')) ++i;
      external = false;
    } else if (c == '\'') {
      SheetToken t;
      t.begin = i;
      std::string name;
      size_t j = i + 1;
      bool closed = false;
      while (j < f.size()) {
        if (f[j] == '\'') {
          if (j + 1 < f.size() && f[j + 1] == '\'') {
            name += '\'';
            j += 2;
            continue;
          }
          closed = true;
          break;
        }
        name += f[j++];
      }
      if (!closed) return;
      t.end = j + 1;
      i = t.end;
      if (i < f.size() && f[i] == '!') {
        ++i;
        // A quoted external reference ('[1]Sheet 1'!A1) or a quoted 3D one ('Jan:Mar'!A1).
        // Sheet names can't contain ':' or '[', so neither is ambiguous.
        if (!external && !name.starts_with('[')) {
          size_t colon = name.find(':');
          t.first = name.substr(0, colon);
          if (colon != std::string::npos) t.last = name.substr(colon + 1);
          if (onSheet) onSheet(t);
        }
      }
      external = false;
    } else if (IsNameStart(c) || IsDigit(c)) {
      size_t end = SkipName(f, i);
      std::string_view first = f.substr(i, end - i);
      if (end < f.size() && f[end] == '!' && !IsDigit(c)) {
        if (!external && onSheet) onSheet({i, end, std::string(first), {}});
        i = end + 1;
      } else if (end < f.size() && f[end] == ':' && end + 1 < f.size() && IsNameStart(f[end + 1]) && !IsDigit(c)) {
        size_t end2 = SkipName(f, end + 1);
        if (end2 < f.size() && f[end2] == '!') {  // Sheet1:Sheet3!A1
          if (!external && onSheet) onSheet({i, end2, std::string(first), std::string(f.substr(end + 1, end2 - end - 1))});
          i = end2 + 1;
        } else {
          if (onName) onName(first, false);
          i = end;
        }
      } else {
        if (onName) onName(first, end < f.size() && f[end] == '(');
        i = end;
      }
      external = false;
    } else {
      ++i;
      external = false;
    }
  }
}

bool LooksLikeCellRef(std::string_view name) {
  // A1-style (up to three letters and digits) or R1C1-style; Excel quotes such sheet names.
  uint32_t col = 0, row = 0;
  if (ParseCellRef(name, col, row)) return true;
  size_t i = 0;
  auto rc = [&](char letter) {
    if (i < name.size() && Lower(name[i]) == letter) {
      ++i;
      while (i < name.size() && IsDigit(name[i])) ++i;
      return true;
    }
    return false;
  };
  bool r = rc('r');
  bool c = rc('c');
  return (r || c) && i == name.size();
}

}  // namespace

void ForEachSheetToken(std::string_view formula, const std::function<void(const SheetToken&)>& fn) {
  Scan(formula, fn, {});
}

void ForEachCellToken(std::string_view formula, const std::function<void(std::string_view)>& fn) {
  Scan(formula, {}, [&](std::string_view name, bool call) {
    uint32_t col = 0, row = 0;
    if (!call && ParseCellRef(name, col, row)) fn(name);
  });
}

std::string QuoteSheetName(std::string_view name) {
  bool quote = name.empty() || IsDigit(name.front()) || LooksLikeCellRef(name);
  for (char c : name)
    if (!(IsAsciiLetter(c) || IsDigit(c) || c == '_' || c == '.')) quote = true;
  if (!quote) return std::string(name);
  std::string out = "'";
  for (char c : name) {
    if (c == '\'') out += '\'';
    out += c;
  }
  return out + "'";
}

std::string RenameSheetInFormula(std::string_view formula, std::string_view from, std::string_view to,
                                 bool* changed) {
  std::string out;
  size_t copied = 0;
  bool any = false;
  ForEachSheetToken(formula, [&](const SheetToken& t) {
    bool a = EqualNoCase(t.first, from), b = !t.last.empty() && EqualNoCase(t.last, from);
    if (!a && !b) return;
    std::string first = a ? std::string(to) : t.first;
    std::string last = b ? std::string(to) : t.last;
    std::string prefix;
    if (last.empty()) {
      prefix = QuoteSheetName(first);
    } else {
      // A 3D prefix is quoted as a whole: 'Jan 2024:Mar 2024'!A1.
      bool quote = QuoteSheetName(first) != first || QuoteSheetName(last) != last;
      prefix = quote ? QuoteSheetName(first + ":" + last) : first + ":" + last;
    }
    out.append(formula.substr(copied, t.begin - copied));
    out += prefix;
    copied = t.end;
    any = true;
  });
  if (changed) *changed = any;
  if (!any) return std::string(formula);
  out.append(formula.substr(copied));
  return out;
}

bool ParseCellRef(std::string_view s, uint32_t& col, uint32_t& row) {
  size_t i = 0;
  if (i < s.size() && s[i] == '$') ++i;
  size_t letters = 0;
  col = 0;
  while (i < s.size() && IsAsciiLetter(s[i]) && letters < 4) {
    col = col * 26 + static_cast<uint32_t>(Lower(s[i]) - 'a' + 1);
    ++i;
    ++letters;
  }
  if (letters == 0 || letters > 3 || col > kMaxColumn) return false;
  if (i < s.size() && s[i] == '$') ++i;
  if (i >= s.size() || !IsDigit(s[i]) || s[i] == '0') return false;
  row = 0;
  for (; i < s.size(); ++i) {
    if (!IsDigit(s[i])) return false;
    row = row * 10 + static_cast<uint32_t>(s[i] - '0');
    if (row > kMaxRow) return false;
  }
  return true;
}

bool IsCellRange(std::string_view s) {
  size_t colon = s.find(':');
  uint32_t col = 0, row = 0;
  if (colon == std::string_view::npos) return ParseCellRef(s, col, row);
  std::string_view a = s.substr(0, colon), b = s.substr(colon + 1);
  if (ParseCellRef(a, col, row) && ParseCellRef(b, col, row)) return true;
  // Whole columns (A:C) or whole rows (1:3).
  auto strip = [](std::string_view v) { return !v.empty() && v.front() == '$' ? v.substr(1) : v; };
  a = strip(a);
  b = strip(b);
  auto columnOnly = [](std::string_view v) {
    if (v.empty() || v.size() > 3) return false;
    uint32_t c = 0;
    for (char ch : v) {
      if (!IsAsciiLetter(ch)) return false;
      c = c * 26 + static_cast<uint32_t>(Lower(ch) - 'a' + 1);
    }
    return c <= kMaxColumn;
  };
  auto rowOnly = [](std::string_view v) {
    if (v.empty() || v.size() > 7 || v.front() == '0') return false;
    uint32_t r = 0;
    for (char ch : v) {
      if (!IsDigit(ch)) return false;
      r = r * 10 + static_cast<uint32_t>(ch - '0');
    }
    return r <= kMaxRow;
  };
  return (columnOnly(a) && columnOnly(b)) || (rowOnly(a) && rowOnly(b));
}

std::string CellName(uint32_t col, uint32_t row) {
  std::string letters;
  for (uint32_t c = col; c > 0; c = (c - 1) / 26) letters.insert(letters.begin(), static_cast<char>('A' + (c - 1) % 26));
  return letters + std::to_string(row);
}

}  // namespace fw::pkg
