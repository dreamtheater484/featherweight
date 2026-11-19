#include "sheet/objects/ChartData.h"

#include <algorithm>
#include <limits>

#include "core/Strings.h"
#include "sheet/model/Workbook.h"

namespace fw::sheet::objects {

namespace {

constexpr size_t kMaxPoints = 4096;  // fwchart's own cap per series

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '=')) s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  return s;
}

}  // namespace

bool WorkbookData::Cells(std::string_view ref, std::vector<std::string>& texts, std::vector<double>& numbers,
                         std::string* format) {
  texts.clear();
  numbers.clear();
  if (format) format->clear();
  return Collect(ref, 0, texts, numbers, format);
}

bool WorkbookData::Numbers(std::string_view ref, std::vector<double>& numbers) {
  std::vector<std::string> texts;
  numbers.clear();
  numbersOnly_ = true;
  const bool ok = Collect(ref, 0, texts, numbers, nullptr);
  numbersOnly_ = false;
  return ok;
}

bool WorkbookData::Collect(std::string_view ref, int depth, std::vector<std::string>& texts,
                           std::vector<double>& numbers, std::string* format) {
  ref = Trim(ref);
  while (ref.size() >= 2 && ref.front() == '(' && ref.back() == ')') ref = Trim(ref.substr(1, ref.size() - 2));
  if (ref.empty()) return false;
  // Areas, split at the commas outside quoted sheet names.
  bool quoted = false;
  size_t start = 0;
  for (size_t i = 0; i <= ref.size(); ++i) {
    if (i < ref.size() && ref[i] == '\'') quoted = !quoted;
    if (i == ref.size() || (ref[i] == ',' && !quoted)) {
      if (!Area(Trim(ref.substr(start, i - start)), depth, texts, numbers, format)) return false;
      start = i + 1;
    }
  }
  return true;
}

bool WorkbookData::Area(std::string_view area, int depth, std::vector<std::string>& texts,
                        std::vector<double>& numbers, std::string* format) {
  int sheet = sheet_;
  std::string_view cells = area;
  const size_t bang = area.rfind('!');
  if (bang != std::string_view::npos) {
    std::string_view quoted = area.substr(0, bang);
    std::string name;
    if (quoted.size() >= 2 && quoted.front() == '\'' && quoted.back() == '\'') {
      for (size_t i = 1; i + 1 < quoted.size(); ++i) {
        name += quoted[i];
        if (quoted[i] == '\'' && quoted[i + 1] == '\'') ++i;
      }
    } else {
      name = quoted;
    }
    if (name.starts_with('[')) return false;  // another workbook
    sheet = book_.FindSheet(name);
    cells = area.substr(bang + 1);
  }
  if (sheet < 0) return false;
  Range r;
  if (!ParseRange(cells, r)) {
    // A defined name standing for the cells.
    const DefinedName* n = depth < 3 ? book_.FindName(cells, sheet) : nullptr;
    return n && Collect(n->formula, depth + 1, texts, numbers, format);
  }
  if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return false;
  const Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
  // Whole columns and rows: only the part in use.
  if (r.lastRow == kMaxRows - 1 || r.lastCol == kMaxCols - 1) {
    const std::optional<Range> used = s.UsedRange();
    if (!used) return true;
    if (r.lastRow == kMaxRows - 1) r.lastRow = std::min(r.lastRow, used->lastRow);
    if (r.lastCol == kMaxCols - 1) r.lastCol = std::min(r.lastCol, used->lastCol);
  }
  const Styles& styles = book_.GetStyles();
  FormatContext context;
  context.locale = &UserLocale();
  context.date1904 = book_.Date1904();
  Formatted shown;
  for (uint32_t row = r.firstRow; row <= r.lastRow; ++row) {
    for (uint32_t col = r.firstCol; col <= r.lastCol; ++col) {
      if (texts.size() >= kMaxPoints) return true;
      const Value v = s.GetValue(row, col);
      const uint32_t numFmt = styles.CellXf(s.StyleAt(row, col)).numFmt;
      if (format && texts.empty()) {
        // The first cell's format code, in the invariant (en-US) form fwchart reads.
        std::string_view code = styles.FormatCode(numFmt);
        *format = !code.empty() ? std::string(code) : numFmt == 0 ? "General" : WideToUtf8(BuiltinCode(numFmt, EnUsLocale()));
      }
      double number = std::numeric_limits<double>::quiet_NaN();
      std::string text;
      switch (v.type) {
        case ValueType::Number:
          number = v.number;
          if (numbersOnly_) break;
          shown.Clear();
          FormatOf(numFmt).Format(v.number, context, shown);
          text = WideToUtf8(shown.text);
          break;
        case ValueType::String: text = v.text; break;
        case ValueType::Bool: text = v.boolean ? "TRUE" : "FALSE"; break;
        case ValueType::Error: text = ErrorText(v.error); break;
        default: break;
      }
      texts.push_back(std::move(text));
      numbers.push_back(number);
    }
  }
  return true;
}

const NumberFormat& WorkbookData::FormatOf(uint32_t numFmt) {
  auto it = formats_.find(numFmt);
  if (it != formats_.end()) return it->second;
  std::string_view code = book_.GetStyles().FormatCode(numFmt);
  NumberFormat f = code.empty() ? NumberFormat::Builtin(static_cast<int>(numFmt), UserLocale()) : NumberFormat(Utf8ToWide(code));
  return formats_.emplace(numFmt, std::move(f)).first->second;
}

}  // namespace fw::sheet::objects
