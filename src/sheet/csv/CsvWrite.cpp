// Saving CSV (plan §7.1). A value is written as the cell shows it with no width limit (General
// at most 11 characters, fills left out, pads as spaces, measured on Excel 2021); a field with the
// delimiter, a quote or a line break is quoted.

#include <windows.h>

#include <algorithm>
#include <memory>
#include <unordered_map>

#include "core/Strings.h"
#include "sheet/csv/Csv.h"
#include "sheet/numfmt/NumFormat.h"

namespace fw::sheet::csv {

namespace {

// The texts of the first sheet's cells, as CSV writes them.
class Texts {
 public:
  Texts(const Workbook& book, const NumLocale& locale) : book_(book), locale_(locale) {
    ctx_.locale = &locale_;
    ctx_.date1904 = book.Date1904();
  }

  // Row `row`'s fields from column `first` on, up to the last one that isn't empty.
  void Row(const Sheet& sheet, uint32_t row, uint32_t first, std::vector<std::string>& out) {
    out.clear();
    sheet.ForEachCell(Range{row, first, row, 16383}, [&](const CellInfo& c) {
      std::string t = Text(c);
      if (t.empty()) return true;
      out.resize(c.col - first + 1);
      out[c.col - first] = std::move(t);
      return true;
    });
  }

 private:
  const NumberFormat& FormatOf(uint32_t id) {
    auto& slot = formats_[id];
    if (!slot) {
      const std::string_view code = book_.GetStyles().FormatCode(id);
      slot = code.empty() ? std::make_unique<NumberFormat>(NumberFormat::Builtin(static_cast<int>(id), locale_))
                          : std::make_unique<NumberFormat>(Utf8ToWide(code));
    }
    return *slot;
  }

  std::string Text(const CellInfo& cell) {
    const NumberFormat& nf = FormatOf(book_.GetStyles().CellXf(cell.style).numFmt);
    switch (cell.value.type) {
      case ValueType::Empty: return {};
      case ValueType::Bool: return cell.value.boolean ? "TRUE" : "FALSE";
      case ValueType::Error: return std::string(ErrorText(cell.value.error));
      case ValueType::String:
        if (nf.IsGeneral()) return std::string(cell.value.text);
        nf.Format(Utf8ToWide(cell.value.text), ctx_, shown_);
        break;
      case ValueType::Number: nf.Format(cell.value.number, ctx_, shown_); break;
    }
    if (shown_.overflow) return "#";
    return WideToUtf8(shown_.text);
  }

  const Workbook& book_;
  const NumLocale& locale_;
  FormatContext ctx_;
  Formatted shown_;
  std::unordered_map<uint32_t, std::unique_ptr<NumberFormat>> formats_;
};

const char* LineEndText(LineEnd e) { return e == LineEnd::LF ? "\n" : e == LineEnd::CR ? "\r" : "\r\n"; }

void AppendRow(const std::vector<std::string>& fields, size_t width, char d, bool quoteAll, std::string& out) {
  for (size_t k = 0; k < std::max(width, fields.size()); ++k) {
    if (k) out += d;
    const std::string_view f = k < fields.size() ? std::string_view(fields[k]) : std::string_view();
    const bool quote = quoteAll || f.find_first_of(std::string{d, '"', '\r', '\n'}) != std::string_view::npos;
    if (!quote) {
      out += f;
      continue;
    }
    out += '"';
    for (char c : f) {
      if (c == '"') out += '"';
      out += c;
    }
    out += '"';
  }
}

// UTF-8 text in the file's encoding, with its byte-order mark.
std::string Encode(std::string_view text, const Source& src) {
  std::string out;
  if (src.encoding == Encoding::Utf8) {
    if (src.bom) out = "\xEF\xBB\xBF";
    return out.append(text);
  }
  const std::wstring w = Utf8ToWide(text);
  if (src.encoding == Encoding::Ansi) {
    const int n = WideCharToMultiByte(src.codePage, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    out.resize(static_cast<size_t>(std::max(n, 0)));
    if (n > 0) WideCharToMultiByte(src.codePage, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
  }
  const bool be = src.encoding == Encoding::Utf16BE;
  if (src.bom) out = be ? "\xFE\xFF" : "\xFF\xFE";
  for (wchar_t c : w) {
    const char lo = static_cast<char>(c & 0xFF), hi = static_cast<char>(c >> 8);
    out += be ? hi : lo;
    out += be ? lo : hi;
  }
  return out;
}

std::string NewFile(const Workbook& book, const Options& options) {
  const NumLocale& loc = options.locale ? *options.locale : UserLocale();
  const char d = options.delimiter          ? options.delimiter
                 : options.tsv             ? '\t'
                 : loc.list.size() == 1 && loc.list[0] < 0x80 ? static_cast<char>(loc.list[0])
                                           : ',';
  std::string out = "\xEF\xBB\xBF";
  if (!book.SheetCount()) return out;
  const Sheet& sheet = book.GetSheet(0);
  const std::optional<Range> used = sheet.UsedRange();
  if (!used) return out;
  Texts texts(book, loc);
  std::vector<std::string> fields;
  const size_t width = used->lastCol - used->firstCol + 1;
  for (uint32_t r = used->firstRow; r <= used->lastRow; ++r) {
    texts.Row(sheet, r, used->firstCol, fields);
    AppendRow(fields, width, d, false, out);
    out += "\r\n";
  }
  return out;
}

}  // namespace

std::string WriteNew(const Workbook& book, const Options& options) { return NewFile(book, options); }

bool Loses(const Workbook& book) {
  if (book.SheetCount() != 1) return book.SheetCount() > 1;
  const Sheet& sheet = book.GetSheet(0);
  if (sheet.FormulaCount() || !sheet.Merges().empty() || !sheet.Hyperlinks().empty() || sheet.Filter()) return true;
  for (const ColumnInfo& c : sheet.Columns())
    if (c.customWidth || c.hidden || c.outlineLevel || c.style) return true;
  // Formats that show more than a number format does, by style index (most CSV books have none).
  const Styles& st = book.GetStyles();
  const std::vector<Xf>& xfs = st.CellXfs();
  if (xfs.empty()) return false;
  const Xf& base = xfs[0];
  std::vector<uint8_t> formatted(xfs.size());
  bool any = false;
  for (size_t i = 1; i < xfs.size(); ++i) {
    const Xf& x = xfs[i];
    formatted[i] = x.parent != base.parent || !(st.FontOf(x) == st.FontOf(base)) ||
                   (x.fill != base.fill && x.fill < st.Fills().size() && base.fill < st.Fills().size() &&
                    !(st.Fills()[x.fill] == st.Fills()[base.fill])) ||
                   (x.border != base.border && x.border < st.Borders().size() && base.border < st.Borders().size() &&
                    !(st.Borders()[x.border] == st.Borders()[base.border])) ||
                   !(x.alignment == base.alignment);
    any = any || formatted[i];
  }
  const std::optional<Range> used = sheet.UsedRange();
  bool loses = false;
  if (used) {
    sheet.ForEachRow(0, used->lastRow, [&](uint32_t, const RowInfo& r) {
      loses = r.customHeight || r.hidden || r.outlineLevel || (r.customFormat && r.style);
      return !loses;
    });
    if (!loses && any)
      sheet.ForEachCell(*used, [&](const CellInfo& c) {
        loses = c.style < formatted.size() && formatted[c.style];
        return !loses;
      });
  }
  return loses;
}

std::string Write(const Workbook& book, std::string_view original, const Options& options) {
  const NumLocale& loc = options.locale ? *options.locale : UserLocale();
  Reader reader(original, options);
  const Source& src = reader.Info();
  const std::string_view text = reader.Text();
  const size_t n = std::min<size_t>(reader.RowCount(), 1048576);
  Texts texts(book, loc);
  const Sheet* sheet = book.SheetCount() ? &book.GetSheet(0) : nullptr;
  const std::optional<Range> used = sheet ? sheet->UsedRange() : std::nullopt;
  const int64_t last = used ? static_cast<int64_t>(used->lastRow) : -1;

  // A sheet row equals an original row when it holds the fields the row reads as.
  std::vector<std::string> sheetFields, fileFields;
  auto load = [&](int64_t r) {
    if (sheet && r <= last) texts.Row(*sheet, static_cast<uint32_t>(r), 0, sheetFields);
    else sheetFields.clear();
  };
  auto equal = [&](int64_t r, size_t k) {
    load(r);
    reader.Fields(k, fileFields);
    if (fileFields.size() > 16384) fileFields.resize(16384);  // past XFD: never in the sheet, kept as read
    while (!fileFields.empty() && fileFields.back().empty()) fileFields.pop_back();
    return sheetFields == fileFields;
  };
  auto fieldCount = [&](size_t k) {
    reader.Fields(k, fileFields);
    return fileFields.size();
  };

  struct Out {
    bool copy;
    size_t row;  // the original row (copy) or the sheet row
    size_t width = 0;
  };
  std::vector<Out> rows;
  constexpr size_t kLook = 16;  // rows looked ahead for an insert or a delete
  size_t k = 0;
  for (int64_t r = 0; r <= last; ++r) {
    if (k < n && equal(r, k)) {
      rows.push_back({true, k++});
      continue;
    }
    size_t j = k + 1;
    while (j < std::min(n, k + kLook) && !equal(r, j)) ++j;
    if (j < std::min(n, k + kLook)) {  // rows k..j-1 were deleted
      rows.push_back({true, j});
      k = j + 1;
      continue;
    }
    bool inserted = false;
    for (int64_t q = r + 1; q <= std::min<int64_t>(last, r + kLook) && k < n && !inserted; ++q) inserted = equal(q, k);
    rows.push_back({false, static_cast<size_t>(r), k < n ? fieldCount(k) : 0});
    if (!inserted && k < n) ++k;
  }
  // Original rows past the sheet's last one: rows without values (blank lines, ",,") are kept,
  // rows with values were cleared.
  for (; k < n; ++k)
    if (equal(last + 1, k)) rows.push_back({true, k});
  // Rows past the sheet's 1,048,576 were never in it: kept as read.
  for (k = n; k < reader.RowCount(); ++k) rows.push_back({true, k});

  bool same = rows.size() == reader.RowCount();
  for (size_t i = 0; same && i < rows.size(); ++i) same = rows[i].copy && rows[i].row == i;
  if (same) return std::string(original);

  std::string out = src.sepText;
  const char* const dominant = LineEndText(src.lineEnd);
  for (size_t i = 0; i < rows.size(); ++i) {
    const Out& o = rows[i];
    const bool final = i + 1 == rows.size();
    if (o.copy) {
      out.append(text.substr(src.rowStart[o.row], src.rowEnd[o.row] - src.rowStart[o.row]));
      const size_t after = o.row + 1 < reader.RowCount() ? src.rowStart[o.row + 1] : text.size();
      const std::string_view end = text.substr(src.rowEnd[o.row], after - src.rowEnd[o.row]);
      if (!end.empty()) out.append(end);
      else if (!final) out += dominant;
      continue;
    }
    texts.Row(*sheet, static_cast<uint32_t>(o.row), 0, sheetFields);
    AppendRow(sheetFields, o.width, src.delimiter, src.quotedAll, out);
    if (!final || src.finalLineEnd) out += dominant;
  }
  return Encode(out, src);
}

}  // namespace fw::sheet::csv
