#include "sheet/data/Sort.h"

#include <windows.h>

#include <algorithm>
#include <numeric>
#include <string>

#include "core/Strings.h"

namespace fw::sheet::data {

namespace {

// Excel's order of value kinds, ascending; blanks are placed apart (always last).
int Rank(const Value& v) {
  switch (v.type) {
    case ValueType::Number: return 0;
    case ValueType::String: return 1;
    case ValueType::Bool: return v.boolean ? 3 : 2;
    case ValueType::Error: return 4;
    default: return 5;
  }
}

struct Key {
  Value value;
  std::wstring text;  // String values, wide for CompareStringEx
};

// <0, 0 or >0 as a sorts before, with or after b ascending (blanks aside).
int Compare(const Key& a, const Key& b) {
  const int ra = Rank(a.value), rb = Rank(b.value);
  if (ra != rb) return ra < rb ? -1 : 1;
  if (ra == 0) return a.value.number < b.value.number ? -1 : a.value.number > b.value.number ? 1 : 0;
  if (ra == 1) {
    const int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, a.text.c_str(), static_cast<int>(a.text.size()),
                                  b.text.c_str(), static_cast<int>(b.text.size()), nullptr, nullptr, 0);
    return r == CSTR_LESS_THAN ? -1 : r == CSTR_GREATER_THAN ? 1 : 0;
  }
  return 0;  // booleans of one kind, errors: equal
}

}  // namespace

std::vector<uint32_t> SortOrder(const Sheet& sheet, const Range& range, const std::vector<SortKey>& keys, bool byColumns) {
  const uint32_t first = byColumns ? range.firstCol : range.firstRow, last = byColumns ? range.lastCol : range.lastRow;
  std::vector<uint32_t> order(last - first + 1);
  std::iota(order.begin(), order.end(), first);
  // The keys' values, read once: keys[k] of line i at [i * keys.size() + k].
  std::vector<Key> values(order.size() * keys.size());
  for (size_t i = 0; i < order.size(); ++i)
    for (size_t k = 0; k < keys.size(); ++k) {
      const uint32_t line = first + static_cast<uint32_t>(i);
      Key& key = values[i * keys.size() + k];
      key.value = byColumns ? sheet.GetValue(keys[k].index, line) : sheet.GetValue(line, keys[k].index);
      if (key.value.type == ValueType::String) key.text = Utf8ToWide(key.value.text);
    }
  std::stable_sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) {
    for (size_t k = 0; k < keys.size(); ++k) {
      const Key& a = values[(x - first) * keys.size() + k];
      const Key& b = values[(y - first) * keys.size() + k];
      const bool blankA = a.value.type == ValueType::Empty, blankB = b.value.type == ValueType::Empty;
      if (blankA != blankB) return blankB;  // blanks last, either way
      if (blankA) continue;
      const int c = Compare(a, b);
      if (c) return keys[k].descending ? c > 0 : c < 0;
    }
    return false;
  });
  return order;
}

bool GuessHeader(const Workbook& book, const Sheet& sheet, const Range& range) {
  if (range.lastRow <= range.firstRow) return false;
  const Styles& st = book.GetStyles();
  for (uint32_t c = range.firstCol; c <= range.lastCol; ++c) {
    const Value top = sheet.GetValue(range.firstRow, c), next = sheet.GetValue(range.firstRow + 1, c);
    if (top.type == ValueType::String && (next.type == ValueType::Number || next.type == ValueType::Bool)) return true;
    const bool boldTop = st.FontOf(st.CellXf(sheet.StyleAt(range.firstRow, c))).bold;
    const bool boldNext = st.FontOf(st.CellXf(sheet.StyleAt(range.firstRow + 1, c))).bold;
    if (top.type != ValueType::Empty && boldTop && !boldNext) return true;
  }
  return false;
}

}  // namespace fw::sheet::data
