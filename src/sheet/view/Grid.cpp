#include "sheet/view/Grid.h"

#include <dwrite.h>

#include <algorithm>
#include <cmath>

#include "core/Strings.h"
#include "text/Fonts.h"

namespace fw::sheet::view {

// ---------------------------------------------------------------------------
// FontMeasure

std::wstring FontMeasure::Family(const Font& f, const Font& base) {
  const std::string& name = !f.name.empty() ? f.name : !base.name.empty() ? base.name : std::string("Calibri");
  return Utf8ToWide(name);
}

FontMeasure::Face& FontMeasure::Get(const Font& f, const Font& base) {
  const std::wstring family = Family(f, base);
  // GDI sizes a font in whole pixels: 11 pt at 96 dpi is 15 px.
  const int ppem = std::max(1, static_cast<int>(std::lround(Size(f, base) * 96.0 / 72.0)));
  auto key = std::make_tuple(ToLower(family), ppem, f.bold, f.italic);
  auto it = faces_.find(key);
  if (it != faces_.end()) return it->second;
  Face& out = faces_[key];
  out.ppem = ppem;
  const text::Face* face = fonts_.FaceFor(family, f.bold, f.italic);
  if (!face) return out;
  IDWriteFontFace* dw = face->Get();
  out.face = dw;
  DWRITE_FONT_METRICS fm{};
  dw->GetGdiCompatibleMetrics(static_cast<float>(ppem), 1, nullptr, &fm);
  out.upem = fm.designUnitsPerEm ? static_cast<float>(fm.designUnitsPerEm) : 2048.f;
  // GDI's ascent and descent: usWinAscent and usWinDescent, each rounded to pixels.
  const text::FontMetrics m = face->Metrics(ppem * 0.75f);
  out.info.ascent = static_cast<int>(std::lround(m.ascent / 0.75f));
  out.info.descent = static_cast<int>(std::lround(m.descent / 0.75f));
  // Excel's row for a font: its line and a pixel above and below (Calibri 11: 20 px, Arial 10: 17).
  out.info.rowHeight = out.info.ascent + out.info.descent + 2;
  UINT32 cps[95];
  UINT16 glyphs[95];
  DWRITE_GLYPH_METRICS gm[95];
  for (int i = 0; i < 95; ++i) cps[i] = 32 + i;
  out.ascii.assign(95, ppem / 2);
  if (SUCCEEDED(dw->GetGlyphIndices(cps, 95, glyphs)) &&
      SUCCEEDED(dw->GetGdiCompatibleGlyphMetrics(static_cast<float>(ppem), 1, nullptr, FALSE, glyphs, 95, gm, FALSE))) {
    for (int i = 0; i < 95; ++i)
      if (glyphs[i]) out.ascii[i] = static_cast<int>(std::lround(gm[i].advanceWidth * ppem / out.upem));
  }
  int digit = 0;
  for (wchar_t d = L'0'; d <= L'9'; ++d) digit = std::max(digit, out.ascii[d - 32]);
  out.info.digit = std::max(1, digit);
  out.info.space = out.ascii[0];
  return out;
}

const FontMeasure::Info& FontMeasure::Of(const Font& f, const Font& base) { return Get(f, base).info; }

double FontMeasure::IdealDigit(const Font& f, const Font& base) {
  const text::Face* face = fonts_.FaceFor(Family(f, base), f.bold, f.italic);
  const float size = static_cast<float>(Size(f, base));
  double widest = 0;
  for (wchar_t d = L'0'; d <= L'9'; ++d) widest = std::max(widest, static_cast<double>(face ? face->Advance(d, size) : 0));
  return widest > 0 ? widest : Of(f, base).digit * 0.75;
}

int FontMeasure::Width(std::wstring_view text, const Font& f, const Font& base) {
  Face& face = Get(f, base);
  int w = 0;
  auto* dw = static_cast<IDWriteFontFace*>(face.face);
  for (size_t i = 0; i < text.size(); ++i) {
    const wchar_t c = text[i];
    if (c >= 32 && c < 127) {
      w += face.ascii[c - 32];
      continue;
    }
    UINT32 cp = c;
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < text.size()) {
      cp = 0x10000 + ((c - 0xD800) << 10) + (text[i + 1] - 0xDC00);
      ++i;
    }
    UINT16 g = 0;
    DWRITE_GLYPH_METRICS gm{};
    if (dw && SUCCEEDED(dw->GetGlyphIndices(&cp, 1, &g)) && g &&
        SUCCEEDED(dw->GetGdiCompatibleGlyphMetrics(static_cast<float>(face.ppem), 1, nullptr, FALSE, &g, 1, &gm, FALSE)))
      w += static_cast<int>(std::lround(gm.advanceWidth * face.ppem / face.upem));
    else
      w += face.ppem;  // another font draws it (East Asian text): about a square
  }
  return w;
}

// ---------------------------------------------------------------------------
// Grid

namespace {

// Lines a text wraps into in `width` pixels: at spaces, and inside words longer than the line.
int WrappedLines(FontMeasure& m, std::wstring_view text, const Font& f, const Font& base, int width) {
  if (width <= 0) return 1;
  int lines = 0;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(L'\n', start);
    if (end == std::wstring_view::npos) end = text.size();
    std::wstring_view para = text.substr(start, end - start);
    int line = 0;
    ++lines;
    size_t i = 0;
    while (i < para.size()) {
      size_t j = para.find(L' ', i);
      if (j == std::wstring_view::npos) j = para.size();
      const int word = m.Width(para.substr(i, j - i), f, base);
      const int space = j < para.size() ? m.Of(f, base).space : 0;
      if (line > 0 && line + word > width) {
        ++lines;
        line = 0;
      }
      if (word > width) lines += word / width, line = word % width;
      else line += word;
      line += space;
      i = j + 1;
    }
    start = end + 1;
  }
  return std::max(1, lines);
}

}  // namespace

int Grid::CharsToPx(double chars) const {
  // ECMA-376 §18.3.1.13: Truncate(((256 * width + Truncate(128 / mdw)) / 256) * mdw).
  const double w = (256.0 * chars + std::trunc(128.0 / mdw_)) / 256.0;
  return std::max(0, static_cast<int>(std::trunc(w * mdw_)));
}

void Grid::Build(const Sheet& sheet, const Styles& styles, FontMeasure& measure) {
  const Font& base = styles.DefaultFont();
  const FontMeasure::Info& def = measure.Of(base, base);
  mdw_ = def.digit;
  printColPt_ = measure.IdealDigit(base, base) / mdw_;
  const SheetFormat& fmt = sheet.Format();

  // Columns.
  if (fmt.defaultColWidth > 0) {
    defaultCol_ = CharsToPx(fmt.defaultColWidth);
  } else {
    // The base width in digits, 2 px of margin each side and the gridline, up to a multiple of 8.
    const int px = static_cast<int>(std::trunc(fmt.baseColWidth * mdw_ + 5));
    defaultCol_ = (px + 7) / 8 * 8;
  }
  colStored_.assign(kMaxCols, defaultCol_);
  colPx_.assign(kMaxCols, defaultCol_);
  for (const ColumnInfo& c : sheet.Columns()) {
    const int px = c.width > 0 ? CharsToPx(c.width) : defaultCol_;
    for (uint32_t i = c.first; i <= c.last && i < kMaxCols; ++i) {
      colStored_[i] = px;
      colPx_[i] = c.hidden ? 0 : px;
    }
  }
  colLeft_.assign(kMaxCols + 1, 0);
  for (uint32_t i = 0; i < kMaxCols; ++i) colLeft_[i + 1] = colLeft_[i] + colPx_[i];

  // Rows: the sheet's default height, unless it is custom then Excel's for the default font.
  defaultRow_ = fmt.customHeight ? static_cast<int>(std::lround(fmt.defaultRowHeight * 96.0 / 72.0)) : def.rowHeight;
  const bool zero = fmt.zeroHeight;

  // Rows the file stores without a height are as tall as their content: the tallest font among
  // their cells with values, and wrapped text.
  std::vector<int> styleRow;  // pixels each cell format needs, -1 unknown
  styleRow.assign(styles.CellXfs().size() + 1, -1);
  auto styleHeight = [&](uint32_t style) {
    if (style >= styleRow.size()) style = 0;
    if (styleRow[style] < 0) {
      const Xf& xf = styles.CellXf(style);
      styleRow[style] = measure.Of(styles.FontOf(xf), base).rowHeight;
    }
    return styleRow[style];
  };
  auto fitRow = [&](uint32_t row) {
    int need = defaultRow_;
    sheet.ForEachCell(Range{row, 0, row, kMaxCols - 1}, [&](const CellInfo& c) {
      if (c.value.type == ValueType::Empty) return true;
      if (sheet.MergeAt(c.row, c.col)) return true;  // merged cells don't size their rows
      const Xf& xf = styles.CellXf(c.style);
      int h = styleHeight(c.style);
      const Font& font = styles.FontOf(xf);
      if (c.value.type == ValueType::String && (xf.alignment.wrap || xf.alignment.rotation != 0)) {
        const std::wstring text = Utf8ToWide(c.value.text);
        const FontMeasure::Info& fi = measure.Of(font, base);
        if (xf.alignment.rotation == 255) {
          h = static_cast<int>(text.size()) * (fi.ascent + fi.descent) + 2;
        } else if (xf.alignment.rotation != 0) {
          const double deg = xf.alignment.rotation <= 90 ? xf.alignment.rotation : xf.alignment.rotation - 90.0;
          const double rad = deg * 3.14159265358979 / 180;
          h = static_cast<int>(std::ceil(measure.Width(text, font, base) * std::sin(rad) +
                                         (fi.ascent + fi.descent) * std::cos(rad))) + 3;
        } else {
          h = WrappedLines(measure, text, font, base, ColWidth(c.col) - 5) * fi.rowHeight;
        }
      }
      need = std::max(need, h);
      return true;
    });
    return need;
  };

  rowsStored_.clear();
  rows_.clear();
  sheet.ForEachRow(0, kMaxRows - 1, [&](uint32_t row, const RowInfo& info) {
    int px = info.height > 0 ? static_cast<int>(std::lround(info.height * 96.0 / 72.0))
             : zero          ? 0
                             : fitRow(row);
    if (px != defaultRow_) rowsStored_.push_back({row, px});
    const int shown = info.hidden ? 0 : px;
    if (shown != (zero ? 0 : defaultRow_)) rows_.push_back({row, shown});
    return true;
  });
  // With zeroHeight every row is hidden but those the file shows: the default is no height at all.
  naturalRow_ = defaultRow_;
  if (zero) defaultRow_ = 0;
  Prefix();
}

void Grid::Prefix() {
  before_.assign(rows_.size() + 1, 0);
  for (size_t i = 0; i < rows_.size(); ++i) before_[i + 1] = before_[i] + rows_[i].second - defaultRow_;
}

uint32_t Grid::ColAt(int64_t x) const {
  if (x < 0) return 0;
  auto it = std::upper_bound(colLeft_.begin(), colLeft_.end(), x);
  uint32_t c = static_cast<uint32_t>(it - colLeft_.begin());
  c = c == 0 ? 0 : c - 1;
  // Past hidden columns to the shown one at x.
  while (c + 1 < kMaxCols && colLeft_[c + 1] <= x) ++c;
  return std::min(c, kMaxCols - 1);
}

int64_t Grid::RowTop(uint32_t row) const {
  row = std::min(row, kMaxRows);
  auto it = std::lower_bound(rows_.begin(), rows_.end(), row, [](const auto& a, uint32_t r) { return a.first < r; });
  return static_cast<int64_t>(row) * defaultRow_ + before_[it - rows_.begin()];
}

int Grid::RowHeight(uint32_t row) const {
  auto it = std::lower_bound(rows_.begin(), rows_.end(), row, [](const auto& a, uint32_t r) { return a.first < r; });
  return it != rows_.end() && it->first == row ? it->second : defaultRow_;
}

int Grid::StoredRowPx(uint32_t row) const {
  auto it = std::lower_bound(rowsStored_.begin(), rowsStored_.end(), row,
                             [](const auto& a, uint32_t r) { return a.first < r; });
  if (it != rowsStored_.end() && it->first == row) return it->second;
  return naturalRow_;
}

uint32_t Grid::RowAt(int64_t y) const {
  if (y < 0) return 0;
  // The last row stored apart whose top is at or above y.
  size_t lo = 0, hi = rows_.size();
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    const int64_t top = static_cast<int64_t>(rows_[mid].first) * defaultRow_ + before_[mid];
    if (top <= y) lo = mid + 1;
    else hi = mid;
  }
  int64_t row;
  if (lo == 0) {
    row = defaultRow_ > 0 ? y / defaultRow_ : (rows_.empty() ? 0 : rows_[0].first);
  } else {
    const size_t i = lo - 1;
    const int64_t top = static_cast<int64_t>(rows_[i].first) * defaultRow_ + before_[i];
    if (y < top + rows_[i].second) {
      row = rows_[i].first;
    } else if (defaultRow_ > 0) {
      row = rows_[i].first + 1 + (y - top - rows_[i].second) / defaultRow_;
    } else {
      row = i + 1 < rows_.size() ? rows_[i + 1].first : kMaxRows - 1;
    }
  }
  return static_cast<uint32_t>(std::clamp<int64_t>(row, 0, kMaxRows - 1));
}

void Grid::SetRowsHidden(uint32_t first, uint32_t last, bool hidden) {
  if (first > last) return;
  last = std::min(last, kMaxRows - 1);
  auto cmp = [](const auto& a, uint32_t r) { return a.first < r; };
  auto a = std::lower_bound(rows_.begin(), rows_.end(), first, cmp);
  auto b = std::lower_bound(rows_.begin(), rows_.end(), last + 1, cmp);
  std::vector<std::pair<uint32_t, int>> mid;
  if (hidden) {
    if (defaultRow_ != 0)
      for (uint32_t r = first; r <= last; ++r) mid.push_back({r, 0});
  } else {
    auto s = std::lower_bound(rowsStored_.begin(), rowsStored_.end(), first, cmp);
    for (uint32_t r = first; r <= last; ++r) {
      while (s != rowsStored_.end() && s->first < r) ++s;
      const int px = s != rowsStored_.end() && s->first == r ? s->second : naturalRow_;
      if (px != defaultRow_) mid.push_back({r, px});
    }
  }
  std::vector<std::pair<uint32_t, int>> out;
  out.reserve(rows_.size() - (b - a) + mid.size());
  out.insert(out.end(), rows_.begin(), a);
  out.insert(out.end(), mid.begin(), mid.end());
  out.insert(out.end(), b, rows_.end());
  rows_ = std::move(out);
  Prefix();
}

void Grid::SetColsHidden(uint32_t first, uint32_t last, bool hidden) {
  last = std::min(last, kMaxCols - 1);
  for (uint32_t c = first; c <= last; ++c) colPx_[c] = hidden ? 0 : colStored_[c];
  for (uint32_t i = first; i < kMaxCols; ++i) colLeft_[i + 1] = colLeft_[i] + colPx_[i];
}

}  // namespace fw::sheet::view
