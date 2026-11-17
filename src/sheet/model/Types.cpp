#include "sheet/model/Types.h"

#include <array>

namespace fw::sheet {
namespace {

// Letters then digits, each with an optional $; either part may be missing when `allowPartial`.
// Sets has* for the parts present.
bool ParseRef(std::string_view s, uint32_t& col, uint32_t& row, bool& hasCol, bool& hasRow) {
  size_t i = 0;
  if (i < s.size() && s[i] == '$') ++i;
  uint32_t c = 0;
  size_t letters = 0;
  while (i < s.size()) {
    char ch = s[i];
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
    if (ch < 'A' || ch > 'Z') break;
    c = c * 26 + static_cast<uint32_t>(ch - 'A' + 1);
    if (++letters > 3) return false;
    ++i;
  }
  hasCol = letters > 0;
  if (hasCol && (c == 0 || c > kMaxCols)) return false;
  if (!hasCol && i > 0) {
    // "$" then digits: a row.
  }
  if (i < s.size() && s[i] == '$') {
    if (!hasCol && i > 0) return false;  // "$$"
    ++i;
  }
  uint32_t r = 0;
  size_t digits = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    r = r * 10 + static_cast<uint32_t>(s[i] - '0');
    if (++digits > 7) return false;
    ++i;
  }
  if (i != s.size()) return false;
  hasRow = digits > 0;
  if (hasRow && (r == 0 || r > kMaxRows)) return false;
  if (!hasCol && !hasRow) return false;
  col = hasCol ? c - 1 : 0;
  row = hasRow ? r - 1 : 0;
  return true;
}

constexpr std::array<std::string_view, 15> kErrors = {"#NULL!",    "#DIV/0!",  "#VALUE!",   "#REF!",     "#NAME?",
                                                      "#NUM!",     "#N/A",     "#GETTING_DATA", "#SPILL!", "#CALC!",
                                                      "#FIELD!",   "#BLOCKED!", "#CONNECT!", "#BUSY!",    "#UNKNOWN!"};

}  // namespace

bool ParseA1(std::string_view s, CellAddr& out) {
  bool hasCol = false, hasRow = false;
  uint32_t c = 0, r = 0;
  if (!ParseRef(s, c, r, hasCol, hasRow) || !hasCol || !hasRow) return false;
  out = {r, c};
  return true;
}

bool ParseRange(std::string_view s, Range& out) {
  size_t colon = s.find(':');
  bool hc1 = false, hr1 = false, hc2 = false, hr2 = false;
  uint32_t c1 = 0, r1 = 0, c2 = 0, r2 = 0;
  if (colon == std::string_view::npos) {
    if (!ParseRef(s, c1, r1, hc1, hr1) || !hc1 || !hr1) return false;
    out = {r1, c1, r1, c1};
    return true;
  }
  if (!ParseRef(s.substr(0, colon), c1, r1, hc1, hr1) || !ParseRef(s.substr(colon + 1), c2, r2, hc2, hr2))
    return false;
  if (hc1 != hc2 || hr1 != hr2) return false;
  if (!hr1) r1 = 0, r2 = kMaxRows - 1;  // A:B
  if (!hc1) c1 = 0, c2 = kMaxCols - 1;  // 1:2
  out = {std::min(r1, r2), std::min(c1, c2), std::max(r1, r2), std::max(c1, c2)};
  return true;
}

std::string ColumnName(uint32_t col) {
  char buf[4];
  size_t n = 0;
  uint32_t c = col + 1;
  while (c > 0 && n < 4) {
    buf[n++] = static_cast<char>('A' + (c - 1) % 26);
    c = (c - 1) / 26;
  }
  std::string s;
  while (n > 0) s += buf[--n];
  return s;
}

std::string CellName(uint32_t row, uint32_t col) {
  std::string s = ColumnName(col);
  s += std::to_string(row + 1);
  return s;
}

std::string RangeName(const Range& r) {
  std::string s = CellName(r.firstRow, r.firstCol);
  if (r.lastRow != r.firstRow || r.lastCol != r.firstCol) {
    s += ':';
    s += CellName(r.lastRow, r.lastCol);
  }
  return s;
}

std::string_view ErrorText(ErrorCode e) {
  size_t i = static_cast<size_t>(e);
  return i < kErrors.size() ? kErrors[i] : kErrors.back();
}

bool ParseError(std::string_view text, ErrorCode& out) {
  for (size_t i = 0; i < kErrors.size(); ++i)
    if (kErrors[i] == text) {
      out = static_cast<ErrorCode>(i);
      return true;
    }
  return false;
}

}  // namespace fw::sheet
