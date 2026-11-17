#include "sheet/data/Filter.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <unordered_set>

#include "core/Strings.h"
#include "sheet/numfmt/NumFormat.h"

namespace fw::sheet::data {

namespace {

std::wstring Lower(std::wstring s) {
  if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
  return s;
}

int CompareText(const std::wstring& a, const std::wstring& b) {
  const int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                static_cast<int>(b.size()), nullptr, nullptr, 0);
  return r == CSTR_LESS_THAN ? -1 : r == CSTR_GREATER_THAN ? 1 : 0;
}

// `text` matches `pattern` without case: * any run, ? one character, ~ takes the next literally.
bool Matches(const std::wstring& text, const std::wstring& pattern) {
  const std::wstring t = Lower(text), p = Lower(pattern);
  size_t ti = 0, pi = 0, star = std::wstring::npos, mark = 0;
  while (ti < t.size()) {
    if (pi < p.size() && p[pi] == L'*') {
      star = pi++;
      mark = ti;
    } else if (pi < p.size() && (p[pi] == L'?' || (p[pi] == L'~' && pi + 1 < p.size() ? p[pi + 1] == t[ti] : p[pi] == t[ti]))) {
      pi += p[pi] == L'~' && pi + 1 < p.size() ? 2 : 1;
      ++ti;
    } else if (star != std::wstring::npos) {
      pi = star + 1;
      ti = ++mark;
    } else {
      return false;
    }
  }
  while (pi < p.size() && p[pi] == L'*') ++pi;
  return pi == p.size();
}

bool ParseNumber(const std::wstring& s, double& out) {
  if (s.empty()) return false;
  wchar_t* end = nullptr;
  out = std::wcstod(s.c_str(), &end);
  return end && *end == 0;
}

// Excel's order of kinds in the value list: numbers, text, FALSE, TRUE, errors.
int Rank(const Value& v) {
  switch (v.type) {
    case ValueType::Number: return 0;
    case ValueType::String: return 1;
    case ValueType::Bool: return v.boolean ? 3 : 2;
    default: return 4;
  }
}

}  // namespace

std::wstring ShownText(const Workbook& book, const Sheet& sheet, uint32_t row, uint32_t col, const NumLocale& locale) {
  const Value v = sheet.GetValue(row, col);
  switch (v.type) {
    case ValueType::Empty: return {};
    case ValueType::String: return Utf8ToWide(v.text);
    case ValueType::Bool: return v.boolean ? L"TRUE" : L"FALSE";
    case ValueType::Error: return Utf8ToWide(std::string(ErrorText(v.error)));
    case ValueType::Number: break;
  }
  const Styles& st = book.GetStyles();
  const uint32_t id = st.CellXf(sheet.StyleAt(row, col)).numFmt;
  const std::string_view code = st.FormatCode(id);
  const NumberFormat nf = code.empty() ? NumberFormat::Builtin(static_cast<int>(id), locale) : NumberFormat(Utf8ToWide(code));
  FormatContext ctx;
  ctx.locale = &locale;
  ctx.date1904 = book.Date1904();
  Formatted f;
  nf.Format(v.number, ctx, f);
  return f.overflow ? std::wstring() : f.text;
}

bool Passes(const Value& value, const std::wstring& shown, const FilterCondition& c) {
  double n = 0;
  if (ParseNumber(c.value, n) && value.type == ValueType::Number) {
    const double x = value.number;
    switch (c.op) {
      case FilterCondition::Op::Equal: return x == n;
      case FilterCondition::Op::NotEqual: return x != n;
      case FilterCondition::Op::Greater: return x > n;
      case FilterCondition::Op::GreaterEqual: return x >= n;
      case FilterCondition::Op::Less: return x < n;
      case FilterCondition::Op::LessEqual: return x <= n;
    }
  }
  switch (c.op) {
    case FilterCondition::Op::Equal: return Matches(shown, c.value);
    case FilterCondition::Op::NotEqual: return !Matches(shown, c.value);
    default: break;
  }
  // Text against text; a number never passes a text comparison, nor text a numeric one.
  if (value.type != ValueType::String || ParseNumber(c.value, n)) return false;
  const int r = CompareText(shown, c.value);
  switch (c.op) {
    case FilterCondition::Op::Greater: return r > 0;
    case FilterCondition::Op::GreaterEqual: return r >= 0;
    case FilterCondition::Op::Less: return r < 0;
    case FilterCondition::Op::LessEqual: return r <= 0;
    default: return false;
  }
}

std::vector<std::wstring> ColumnValues(const Workbook& book, const Sheet& sheet, const Range& ref, uint32_t column,
                                       const NumLocale& locale, bool& blanks) {
  struct Entry {
    Value value;
    std::wstring shown;
  };
  std::vector<Entry> entries;
  std::unordered_set<std::wstring> seen;
  const uint32_t col = ref.firstCol + column;
  size_t filled = 0;
  if (ref.lastRow > ref.firstRow)
    sheet.ForEachCell(Range{ref.firstRow + 1, col, ref.lastRow, col}, [&](const CellInfo& c) {
      if (c.value.type == ValueType::Empty) return true;
      ++filled;
      std::wstring shown = ShownText(book, sheet, c.row, c.col, locale);
      if (seen.insert(Lower(shown)).second) entries.push_back({c.value, std::move(shown)});
      return true;
    });
  blanks = filled < ref.lastRow - ref.firstRow;
  std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    const int ra = Rank(a.value), rb = Rank(b.value);
    if (ra != rb) return ra < rb;
    if (ra == 0) return a.value.number < b.value.number;
    return CompareText(a.shown, b.shown) < 0;
  });
  std::vector<std::wstring> out;
  out.reserve(entries.size());
  for (Entry& e : entries) out.push_back(std::move(e.shown));
  return out;
}

std::vector<bool> FilterRows(const Workbook& book, const Sheet& sheet, const Range& ref,
                             const std::vector<FilterCriteria>& criteria, const NumLocale& locale) {
  const size_t rows = ref.lastRow > ref.firstRow ? ref.lastRow - ref.firstRow : 0;
  std::vector<bool> keep(rows, true);
  for (const FilterCriteria& c : criteria) {
    if (!c.listed && c.conditions.empty()) continue;
    std::unordered_set<std::wstring> values;
    for (const std::wstring& v : c.values) values.insert(Lower(v));
    const uint32_t col = ref.firstCol + c.column;
    for (size_t i = 0; i < rows; ++i) {
      if (!keep[i]) continue;
      const uint32_t row = ref.firstRow + 1 + static_cast<uint32_t>(i);
      const Value v = sheet.GetValue(row, col);
      const std::wstring shown = ShownText(book, sheet, row, col, locale);
      bool pass;
      if (c.listed) {
        pass = v.type == ValueType::Empty ? c.blanks : values.count(Lower(shown)) > 0;
      } else {
        pass = c.both;
        for (const FilterCondition& f : c.conditions) {
          const bool p = Passes(v, shown, f);
          pass = c.both ? pass && p : pass || p;
        }
      }
      keep[i] = pass;
    }
  }
  return keep;
}

std::vector<bool> DuplicateRows(const Workbook& book, const Sheet& sheet, const Range& range,
                                const std::vector<uint32_t>& columns, bool header, const NumLocale& locale) {
  const uint32_t first = range.firstRow + (header ? 1 : 0);
  std::vector<bool> dup(range.lastRow >= first ? range.lastRow - first + 1 : 0, false);
  std::unordered_set<std::wstring> seen;
  std::wstring key;
  for (size_t i = 0; i < dup.size(); ++i) {
    key.clear();
    for (uint32_t c : columns) {
      key += Lower(ShownText(book, sheet, first + static_cast<uint32_t>(i), range.firstCol + c, locale));
      key += L'\x1F';  // between the columns' texts
    }
    dup[i] = !seen.insert(key).second;
  }
  return dup;
}

Range CurrentRegion(const Sheet& sheet, CellAddr at) {
  Range r{at.row, at.col, at.row, at.col};
  auto filled = [&](const Range& line) {
    bool any = false;
    sheet.ForEachCell(line, [&](const CellInfo& c) {
      any = c.value.type != ValueType::Empty;
      return !any;
    });
    return any;
  };
  const std::optional<Range> used = sheet.UsedRange();
  if (!used) return r;
  for (bool grew = true; grew;) {
    grew = false;
    const uint32_t c0 = r.firstCol > 0 ? r.firstCol - 1 : 0, c1 = std::min(r.lastCol + 1, kMaxCols - 1);
    const uint32_t r0 = r.firstRow > 0 ? r.firstRow - 1 : 0, r1 = std::min(r.lastRow + 1, kMaxRows - 1);
    if (r.firstRow > 0 && filled(Range{r.firstRow - 1, c0, r.firstRow - 1, c1})) --r.firstRow, grew = true;
    if (r.firstCol > 0 && filled(Range{r0, r.firstCol - 1, r1, r.firstCol - 1})) --r.firstCol, grew = true;
    if (r.lastCol + 1 < kMaxCols && r.lastCol < used->lastCol && filled(Range{r0, r.lastCol + 1, r1, r.lastCol + 1}))
      ++r.lastCol, grew = true;
    // Downwards in one pass: the rows below while each has a filled cell in the columns' span.
    if (r.lastRow < used->lastRow) {
      uint32_t next = r.lastRow + 1;
      bool stop = false;
      sheet.ForEachCell(Range{r.lastRow + 1, c0, used->lastRow, c1}, [&](const CellInfo& c) {
        if (c.value.type == ValueType::Empty) return true;
        if (c.row > next) {
          stop = true;
          return false;
        }
        if (c.row == next) ++next;
        return true;
      });
      (void)stop;
      if (next > r.lastRow + 1) {
        r.lastRow = next - 1;
        grew = true;
      }
    }
  }
  return r;
}

}  // namespace fw::sheet::data
