// OpenFormula (ODF 1.2 part 2) as Excel's formula text: references in brackets ("[.A1]",
// "[$Sheet2.$A$1:.$B$3]") become Excel's ("Sheet2!$A$1:$B$3"), ";" separates arguments, inline
// arrays use ";" and "|" where Excel uses "," and ";", and function names lose ODF's namespaces
// (COM.MICROSOFT.XLOOKUP is Excel's _xlfn.XLOOKUP).
#include <cctype>
#include <string>

#include "sheet/calc/Functions.h"
#include "sheet/ods/Ods.h"

namespace fw::sheet::ods {
namespace {

constexpr int kMaxRow = 1048576, kMaxCol = 16384;

bool Alpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80; }
bool Digit(char c) { return c >= '0' && c <= '9'; }

std::string Upper(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

std::string ColName(int col) {  // 0-based
  std::string s;
  for (int c = col + 1; c > 0; c = (c - 1) / 26) s.insert(s.begin(), static_cast<char>('A' + (c - 1) % 26));
  return s;
}

// Whether Excel needs a sheet name in quotes: anything but letters, digits and "_" or a start with
// a digit, or a name that reads as a reference (A1, R1C1).
bool NeedsQuotes(std::string_view s) {
  if (s.empty() || Digit(s[0])) return true;
  for (char c : s)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return true;
  size_t i = 0;
  while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
  if (i > 0 && i <= 3 && i < s.size()) {
    size_t j = i;
    while (j < s.size() && Digit(s[j])) ++j;
    if (j == s.size()) return true;
  }
  const std::string u = Upper(s);
  return u[0] == 'R' || u[0] == 'C' || u == "TRUE" || u == "FALSE";
}

std::string SheetRef(const std::string& name) {
  if (!NeedsQuotes(name)) return name;
  std::string q = "'";
  for (char c : name) q += c == '\'' ? std::string("''") : std::string(1, c);
  return q + "'";
}

// One end of an ODF reference: [$]sheet . [$]col [$]row, any part may be missing.
struct End {
  bool hasSheet = false;
  std::string sheet;
  int col = -1, row = -1;  // 0-based; -1 when not given
  bool absCol = false, absRow = false;
  bool bad = false;  // #REF!
};

// Reads one end from `s` at `i`; stops at ':' or the end.
bool ReadEnd(std::string_view s, size_t& i, End& e) {
  // The sheet: '$'? then quoted or bare up to the last '.' before the cell part.
  size_t start = i;
  if (i < s.size() && s[i] == '$') ++i;
  if (i < s.size() && s[i] == '\'') {
    ++i;
    std::string name;
    while (i < s.size()) {
      if (s[i] == '\'') {
        if (i + 1 < s.size() && s[i + 1] == '\'') {
          name += '\'';
          i += 2;
          continue;
        }
        break;
      }
      name += s[i++];
    }
    if (i >= s.size()) return false;
    ++i;  // the closing quote
    if (i < s.size() && s[i] == '#') return false;  // another document: 'file:///...'#$Sheet1.A1
    if (i >= s.size() || s[i] != '.') return false;
    ++i;
    e.hasSheet = true;
    e.sheet = std::move(name);
  } else {
    // Bare: up to the '.' that starts the cell part (sheet names with dots are quoted).
    size_t dot = s.find('.', i);
    size_t colon = s.find(':', i);
    if (dot != std::string_view::npos && (colon == std::string_view::npos || dot < colon)) {
      if (dot > i) {
        e.hasSheet = true;
        e.sheet = std::string(s.substr(i, dot - i));
        if (e.sheet == "#REF!") e.bad = true;
      } else if (i != start) {
        return false;  // "$." isn't a sheet
      }
      i = dot + 1;
    } else {
      i = start;  // no sheet part at all
    }
  }
  if (s.substr(i).starts_with("#REF!")) {
    e.bad = true;
    i += 5;
    return true;
  }
  if (i < s.size() && s[i] == '$') {
    e.absCol = true;
    ++i;
  }
  int col = 0, letters = 0;
  while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i])) && letters < 4) {
    col = col * 26 + (std::toupper(static_cast<unsigned char>(s[i])) - 'A' + 1);
    ++i;
    ++letters;
  }
  if (letters) e.col = col - 1;
  else if (e.absCol) {
    e.absCol = false;
    e.absRow = true;  // "$1": the '$' was the row's
  }
  if (i < s.size() && s[i] == '$') {
    e.absRow = true;
    ++i;
  }
  int64_t row = 0;
  int digits = 0;
  while (i < s.size() && Digit(s[i]) && digits < 8) {
    row = row * 10 + (s[i] - '0');
    ++i;
    ++digits;
  }
  if (digits) {
    if (row < 1) return false;
    e.row = static_cast<int>(row - 1);
  }
  if (!letters && !digits) return false;
  if (e.col >= kMaxCol || e.row >= kMaxRow) e.bad = true;
  return true;
}

void Shift(End& e, int rows, int cols) {
  if (e.bad) return;
  if (e.row >= 0 && !e.absRow) {
    e.row += rows;
    if (e.row < 0 || e.row >= kMaxRow) e.bad = true;
  }
  if (e.col >= 0 && !e.absCol) {
    e.col += cols;
    if (e.col < 0 || e.col >= kMaxCol) e.bad = true;
  }
}

std::string Cell(const End& e) {
  std::string s;
  if (e.col >= 0) s += (e.absCol ? "$" : "") + ColName(e.col);
  if (e.row >= 0) s += (e.absRow ? "$" : "") + std::to_string(e.row + 1);
  return s;
}

// The text inside one pair of brackets (or an attribute's address) as Excel's reference.
bool Reference(std::string_view s, std::string& out, int rows, int cols, const SheetNames* names) {
  End a, b;
  size_t i = 0;
  if (!ReadEnd(s, i, a)) return false;
  bool range = false;
  if (i < s.size() && s[i] == ':') {
    ++i;
    if (!ReadEnd(s, i, b)) return false;
    range = true;
  }
  if (i != s.size()) return false;
  if (rows || cols) {
    Shift(a, rows, cols);
    if (range) Shift(b, rows, cols);
  }
  if (a.bad || (range && b.bad) || (a.row < -1 || a.col < -1) || (range && (b.row < -1 || b.col < -1))) {
    out += "#REF!";
    return true;
  }
  // A1:B2 needs both ends alike: a cell, whole columns or whole rows.
  if (range && ((a.col < 0) != (b.col < 0) || (a.row < 0) != (b.row < 0))) return false;
  if (!range && (a.col < 0 || a.row < 0)) return false;
  auto mapped = [&](const std::string& n) { return names && *names ? (*names)(n) : n; };
  if (a.hasSheet) {
    std::string sheet = mapped(a.sheet);
    if (sheet.empty() || (range && b.hasSheet && mapped(b.sheet).empty())) {  // a sheet the workbook doesn't have
      out += "#REF!";
      return true;
    }
    if (range && b.hasSheet && b.sheet != a.sheet) {
      const std::string both = sheet + ":" + mapped(b.sheet);
      // Quoted as one: 'Sheet 1:Sheet 3'!A1
      out += NeedsQuotes(sheet) || NeedsQuotes(mapped(b.sheet)) ? "'" + [&] {
        std::string q;
        for (char c : both) q += c == '\'' ? std::string("''") : std::string(1, c);
        return q;
      }() + "'" : both;
    } else {
      out += SheetRef(sheet);
    }
    out += '!';
  }
  out += Cell(a);
  if (range && (a.row < 0 || a.col < 0 || Cell(b) != Cell(a))) out += ":" + Cell(b);  // A1:A1 is A1, A:A stays
  return true;
}

// ODF and Excel names that differ, after COM.MICROSOFT. and the like are taken off.
std::string FunctionName(std::string_view written) {
  std::string name = Upper(written);
  bool microsoft = false;
  for (std::string_view p : {"COM.MICROSOFT.", "_XLFN._XLWS.", "_XLFN.", "_XLWS."}) {
    if (name.starts_with(p)) {
      name.erase(0, p.size());
      microsoft = true;
      break;
    }
  }
  if (name.starts_with("LEGACY.")) name.erase(0, 7);  // LEGACY.NORMSDIST is Excel's NORMSDIST
  if (name == "ORG.OPENOFFICE.ERRORTYPE") name = "ERROR.TYPE";
  const int id = calc::FindFunction(name);
  if (id >= 0) {
    const uint8_t flags = calc::Function(id).flags;
    if (flags & calc::kFnXlws) return "_xlfn._xlws." + name;
    if (flags & calc::kFnXlfn) return "_xlfn." + name;
    return name;
  }
  if (name == "SINGLE" || name == "ANCHORARRAY") return "_xlfn." + name;
  return microsoft ? "_xlfn." + name : name;  // one the engine doesn't have: as Excel names it
}

constexpr std::string_view kErrors[] = {"#NULL!", "#DIV/0!", "#VALUE!", "#REF!", "#NAME?", "#NUM!", "#N/A", "#GETTING_DATA"};

}  // namespace

bool TranslateFormula(std::string_view f, std::string& out, int rows, int cols, const SheetNames* names) {
  out.clear();
  // The namespace: "of:" (OpenFormula), "oooc:" (OpenOffice 1/2, the same references), "msoxl:"
  // (Excel 2007 SP2: Excel's own syntax).
  if (const size_t colon = f.find(':'); colon != std::string_view::npos && colon < 8 && f.find('=') == colon + 1) {
    const std::string_view ns = f.substr(0, colon);
    f.remove_prefix(colon + 1);
    if (ns == "msoxl") {
      if (!f.empty() && f[0] == '=') f.remove_prefix(1);
      if (rows || cols) return false;  // not written repeated
      out = std::string(f);
      return !out.empty();
    }
    if (ns != "of" && ns != "oooc") return false;
  }
  if (!f.empty() && f[0] == '=') f.remove_prefix(1);
  if (!f.empty() && f[0] == '=') f.remove_prefix(1);  // "==" marks a formula LibreOffice reads as text
  int array = 0;
  size_t i = 0;
  while (i < f.size()) {
    const char c = f[i];
    if (c == '"') {  // a string, "" inside
      size_t j = i + 1;
      while (j < f.size()) {
        if (f[j] == '"') {
          if (j + 1 < f.size() && f[j + 1] == '"') {
            j += 2;
            continue;
          }
          break;
        }
        ++j;
      }
      if (j >= f.size()) return false;
      out.append(f.substr(i, j + 1 - i));
      i = j + 1;
    } else if (c == '[') {
      size_t j = i + 1;
      bool quoted = false;
      while (j < f.size() && (quoted || f[j] != ']')) {
        if (f[j] == '\'') quoted = !quoted;
        ++j;
      }
      if (j >= f.size()) return false;
      if (!Reference(f.substr(i + 1, j - i - 1), out, rows, cols, names)) out += "#REF!";
      i = j + 1;
    } else if (c == '{') {
      ++array;
      out += c;
      ++i;
    } else if (c == '}') {
      if (array) --array;
      out += c;
      ++i;
    } else if (c == ';') {
      out += array ? ',' : ',';
      ++i;
    } else if (c == '|') {
      out += array ? ';' : '|';
      ++i;
    } else if (c == '~') {
      out += ',';  // the union operator
      ++i;
    } else if (c == '!') {
      out += ' ';  // the intersection operator
      ++i;
    } else if (c == '#') {
      bool known = false;
      for (std::string_view e : kErrors) {
        if (f.substr(i, e.size()) == e) {
          out.append(e);
          i += e.size();
          known = true;
          break;
        }
      }
      if (!known) return false;
    } else if (c == '$' && i + 1 < f.size() && f[i + 1] == '$') {
      i += 2;  // $$Name: a named expression
    } else if (Alpha(c) || c == '_' || c == '\\') {
      size_t j = i;
      while (j < f.size() && (Alpha(f[j]) || Digit(f[j]) || f[j] == '_' || f[j] == '.' || f[j] == '\\')) ++j;
      size_t k = j;
      while (k < f.size() && f[k] == ' ') ++k;
      const std::string_view word = f.substr(i, j - i);
      if (k < f.size() && f[k] == '(') out += FunctionName(word);
      else out.append(word);
      i = j;
    } else if (Digit(c) || (c == '.' && i + 1 < f.size() && Digit(f[i + 1]))) {
      size_t j = i;
      while (j < f.size() && (Digit(f[j]) || f[j] == '.')) ++j;
      if (j < f.size() && (f[j] == 'e' || f[j] == 'E')) {
        size_t k = j + 1;
        if (k < f.size() && (f[k] == '+' || f[k] == '-')) ++k;
        if (k < f.size() && Digit(f[k])) {
          j = k;
          while (j < f.size() && Digit(f[j])) ++j;
        }
      }
      out.append(f.substr(i, j - i));
      i = j;
    } else if (c == '\r' || c == '\n' || c == '\t') {
      out += ' ';
      ++i;
    } else {
      out += c;
      ++i;
    }
    if (out.size() > 16384) return false;  // Excel's limit is 8,192 characters
  }
  return !out.empty();
}

bool TranslateAddress(std::string_view address, std::string& out, const SheetNames* names) {
  out.clear();
  while (!address.empty() && address.front() == ' ') address.remove_prefix(1);
  while (!address.empty() && address.back() == ' ') address.remove_suffix(1);
  if (address.empty()) return false;
  if (!Reference(address, out, 0, 0, names)) return false;
  return out != "#REF!";
}

}  // namespace fw::sheet::ods
