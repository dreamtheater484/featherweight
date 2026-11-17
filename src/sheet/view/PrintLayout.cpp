#include "sheet/view/PrintLayout.h"

#include <algorithm>
#include <cmath>

#include "core/Strings.h"
#include "text/Fonts.h"
#include "text/LineLayout.h"

namespace fw::sheet::view {

namespace {

// Printed columns and rows for the objects layer: points at 100 % from A1.
class PrintSheetLayout : public SheetLayout {
 public:
  PrintSheetLayout(const std::vector<double>& colX, const Grid& grid) : colX_(colX), grid_(grid) {}
  double ColumnLeft(uint32_t col) const override { return colX_[std::min<uint32_t>(col, kMaxCols)]; }
  double RowTop(uint32_t row) const override { return grid_.PrintY(0, std::min<uint32_t>(row, kMaxRows)); }

 private:
  const std::vector<double>& colX_;
  const Grid& grid_;
};

// Cells placed on a page: sheet points from the block's top-left at (x0, y0).
class PageGeometry : public Geometry {
 public:
  PageGeometry(const Grid& grid, const std::vector<double>& colX, uint32_t row0, uint32_t col0, float x0, float y0,
               float widthScale_)
      : grid_(grid), colX_(colX), row0_(row0), col0_(col0), x0_(x0), y0_(y0) {
    pt = 1;
    px = 0.75f;
    line = 0.75f;
    widthScale = widthScale_;
    rowPx = static_cast<float>(Grid::kPrintRowPt);
    centered = true;
    padStart = 2.4f;
    padEnd = 0.75f;  // after the gridline's 0.75
    descentPx = 0.797f;
  }
  float Left(uint32_t c) const override { return x0_ + X(c); }
  float Right(uint32_t c) const override { return x0_ + X(c + 1); }
  float Top(uint32_t r) const override { return y0_ + grid_.PrintY(row0_, r); }
  float Bottom(uint32_t r) const override { return y0_ + grid_.PrintY(row0_, r + 1); }

 private:
  float X(uint32_t c) const {
    return static_cast<float>(colX_[std::min<uint32_t>(c, kMaxCols)] - colX_[std::min<uint32_t>(col0_, kMaxCols)]);
  }
  const Grid& grid_;
  const std::vector<double>& colX_;
  uint32_t row0_, col0_;
  float x0_, y0_;
};

bool PaperFromText(const std::string& s, float& pts) {
  if (s.empty()) return false;
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (v <= 0) return false;
  const std::string unit = end ? std::string(end) : std::string();
  if (unit == "mm") pts = static_cast<float>(v / 25.4 * 72);
  else if (unit == "cm") pts = static_cast<float>(v / 2.54 * 72);
  else if (unit == "in") pts = static_cast<float>(v * 72);
  else pts = static_cast<float>(v);
  return true;
}

// One part of a header or footer (left, center or right), as runs of text in their fonts.
struct HfRun {
  std::wstring text;
  text::TextStyle style;
  uint32_t color = 0xFF000000;
  bool underline = false, strike = false;
};

}  // namespace

void PrintLayout::PaperSize(uint32_t code, float& w, float& h) {
  struct Paper {
    uint32_t code;
    float w, h;  // millimeters, or inches when below 30
  };
  static const Paper kPapers[] = {
      {1, 8.5f, 11},     {2, 8.5f, 11},   {3, 11, 17},       {4, 17, 11},       {5, 8.5f, 14},     {6, 5.5f, 8.5f},
      {7, 7.25f, 10.5f}, {8, 297, 420},   {9, 210, 297},     {10, 210, 297},    {11, 148, 210},    {12, 250, 353},
      {13, 176, 250},    {14, 8.5f, 13},  {15, 215, 275},    {16, 10, 14},      {17, 11, 17},      {18, 8.5f, 11},
      {19, 3.875f, 8.875f}, {20, 4.125f, 9.5f}, {21, 4.5f, 10.375f}, {22, 4.75f, 11}, {23, 5, 11.5f},
      {24, 17, 22},      {25, 22, 34},    {26, 34, 44},      {27, 110, 220},    {28, 162, 229},    {29, 324, 458},
      {30, 229, 324},    {31, 114, 162},  {32, 114, 229},    {33, 250, 353},    {34, 176, 250},    {35, 176, 125},
      {36, 110, 230},    {37, 3.875f, 7.5f}, {38, 3.625f, 6.5f}, {39, 14.875f, 11}, {40, 8.5f, 12}, {41, 8.5f, 13},
      {66, 420, 594},    {70, 105, 148},
  };
  for (const Paper& p : kPapers) {
    if (p.code != code) continue;
    const float k = p.w < 30 ? 72.f : 72.f / 25.4f;
    w = p.w * k;
    h = p.h * k;
    return;
  }
  w = 612;
  h = 792;
}

std::vector<Range> PrintLayout::ParseAreas(std::string_view formula) {
  std::vector<Range> out;
  size_t i = 0;
  while (i < formula.size()) {
    // One area: up to the next comma outside quotes.
    bool quoted = false;
    size_t j = i;
    for (; j < formula.size(); ++j) {
      if (formula[j] == '\'') quoted = !quoted;
      else if (formula[j] == ',' && !quoted) break;
    }
    std::string_view part = formula.substr(i, j - i);
    const size_t bang = part.rfind('!');
    if (bang != std::string_view::npos) part = part.substr(bang + 1);
    Range r;
    if (ParseRange(part, r)) out.push_back(r);
    i = j + 1;
  }
  return out;
}

PrintLayout::PrintLayout(const Workbook& book, const Sheet& sheet, size_t sheetIndex, const Grid& grid,
                         CellText& text, PrintInfo info)
    : book_(book), sheet_(sheet), index_(sheetIndex), grid_(grid), text_(text), info_(std::move(info)) {
  Paginate();
}

float PrintLayout::ColsWidth(uint32_t first, uint32_t last) const {
  return last < first || colX_.empty() ? 0 : static_cast<float>(colX_[last + 1] - colX_[first]);
}

float PrintLayout::ColPt(uint32_t col, float scale) {
  const int px = grid_.ColWidth(col);
  if (px <= 0) return 0;
  const int pct = static_cast<int>(std::lround(scale * 100));
  auto it = digits_.find(pct);
  if (it == digits_.end()) {
    const Font& base = book_.GetStyles().DefaultFont();
    it = digits_.emplace(pct, text_.Measure().DigitAt(base, base, Grid::kPrintDpi * pct / 100)).first;
  }
  const double digit = it->second, mdw = grid_.MaxDigit(), pad = Grid::kPrintPad * pct / 100;
  const double dots = px < mdw + 5 ? px * (digit + pad) / (mdw + 5) : (px - 5.0) / mdw * digit + pad;
  return static_cast<float>(dots * Grid::kPrintDotPt * 100 / pct);
}

float PrintLayout::ColsAt(uint32_t first, uint32_t last, float scale) {
  float w = 0;
  for (uint32_t c = first; c <= last && c < kMaxCols; ++c) w += ColPt(c, scale);
  return w;
}

float PrintLayout::RowsHeight(uint32_t first, uint32_t last) const {
  return last < first ? 0 : grid_.PrintY(first, last + 1);
}

uint32_t PrintLayout::Reach(const CellInfo& c, uint32_t past) {
  if (c.value.type != ValueType::String || sheet_.MergeAt(c.row, c.col)) return c.col;
  const Styles& styles = book_.GetStyles();
  const Font& base = styles.DefaultFont();
  const Xf& xf = styles.CellXf(c.style);
  const Alignment& al = xf.alignment;
  if (al.wrap || al.rotation != 0 || al.shrinkToFit) return c.col;
  // No wider than its characters at the font's size, at most: most text can't get past `past`.
  if (past >= c.col && grid_.PrintX(c.col, past + 1) >= c.value.text.size() * FontMeasure::Size(styles.FontOf(xf), base))
    return c.col;
  const int widthPx = static_cast<int>(std::lround(grid_.ColWidth(c.col) * grid_.PrintColPt() / kPtPerPx));
  if (widthPx <= 0) return c.col;
  const CellText::Layout& l = text_.Get(sheet_, c, widthPx);
  if (l.lines.empty() || l.wrapped || l.stacked || l.shown.number || l.shown.fill) return c.col;
  if (l.align != HAlign::Left && l.align != HAlign::Center && l.align != HAlign::CenterContinuous) return c.col;
  // How far its ink runs on paper: the text in the font's own widths (the printer's layout, which
  // is narrower than the screen's hinted one, bold most), from the cell's margin, without a
  // trailing pad (`_)`). Bold "Column 16" fits its 8.43 characters, "(Text)" fits 5.
  FontMeasure& measure = text_.Measure();
  const Font& font = styles.FontOf(xf);
  const float pt = static_cast<float>(kPtPerPx);
  size_t ink = l.shown.text.size();
  while (ink > 0 && ink <= l.shown.hidden.size() && l.shown.hidden[ink - 1]) --ink;
  const float pad = measure.Width(std::wstring_view(l.shown.text).substr(ink), font, base) * pt;
  const float indent = al.indent ? al.indent * 3 * measure.Of(font, base).space * pt : 0;
  const float need = l.width - pad + 3 * pt + indent;
  const float own = grid_.PrintX(c.col, c.col + 1);
  const float reach = l.align == HAlign::Left ? need : (own + need) / 2;
  uint32_t col = c.col;
  for (float x = own; x < reach && col + 1 < kMaxCols; x += grid_.PrintX(col, col + 1)) ++col;
  return col;
}

// Each row's last cell, in `r` (row by row).
static void ForEachLast(const Sheet& sheet, const Range& r, const std::function<void(const CellInfo&)>& fn) {
  CellInfo last;
  bool have = false;
  sheet.ForEachCell(r, [&](const CellInfo& c) {
    if (c.value.type == ValueType::Empty) return true;
    if (have && c.row != last.row) fn(last);
    last = c;
    have = true;
    return true;
  });
  if (have) fn(last);
}

uint32_t PrintLayout::SpillEnd(const Range& used) {
  // Only a row's last cell can run past the range.
  uint32_t end = used.lastCol;
  ForEachLast(sheet_, used, [&](const CellInfo& c) { end = std::max(end, Reach(c, used.lastCol)); });
  return end;
}

draw::Rect PrintLayout::SheetRect(const Range& r) const {
  return {static_cast<float>(colX_[std::min<uint32_t>(r.firstCol, kMaxCols)]), grid_.PrintY(0, r.firstRow),
          static_cast<float>(colX_[std::min<uint32_t>(r.lastCol + 1, kMaxCols)]),
          grid_.PrintY(0, std::min<uint32_t>(r.lastRow + 1, kMaxRows))};
}

void PrintLayout::PlaceObjects() {
  if (info_.objects) info_.objects->Place(PrintSheetLayout(colX_, grid_));
}

std::optional<std::pair<uint32_t, uint32_t>> PrintLayout::ObjectsEnd() const {
  if (!info_.objects) return std::nullopt;
  double right = -1, bottom = -1;
  for (const SheetObjects::Object& o : info_.objects->Objects()) {
    right = std::max(right, static_cast<double>(o.rect.right));
    bottom = std::max(bottom, static_cast<double>(o.rect.bottom));
  }
  if (right <= 0 || bottom <= 0) return std::nullopt;
  // The column and row holding the far edge (a picture ending on a column's left edge isn't in it).
  const auto c = std::lower_bound(colX_.begin(), colX_.end(), right) - colX_.begin();
  const uint32_t col = static_cast<uint32_t>(std::clamp<ptrdiff_t>(c - 1, 0, kMaxCols - 1));
  const uint32_t row = std::min(kMaxRows - 1, grid_.RowAt(static_cast<int64_t>(std::ceil(bottom / Grid::kPrintRowPt)) - 1));
  return std::make_pair(row, col);
}

bool PrintLayout::Blank(const Range& body) {
  const Styles& styles = book_.GetStyles();
  if (info_.objects) {
    const draw::Rect page = SheetRect(body);
    for (const SheetObjects::Object& o : info_.objects->Objects())
      if (o.rect.left < page.right && o.rect.right > page.left && o.rect.top < page.bottom && o.rect.bottom > page.top)
        return false;
  }
  for (const Range& r : sparklines_)
    if (r.firstRow <= body.lastRow && r.lastRow >= body.firstRow && r.firstCol <= body.lastCol && r.lastCol >= body.firstCol)
      return false;
  bool ink = false;
  sheet_.ForEachCell(body, [&](const CellInfo& c) {
    const Xf& xf = styles.CellXf(c.style);
    const Border& b = styles.BorderOf(xf);
    ink = c.value.type != ValueType::Empty || styles.FillOf(xf).pattern != PatternType::None || styles.FillOf(xf).gradient ||
          b.left.style != BorderStyle::None || b.right.style != BorderStyle::None || b.top.style != BorderStyle::None ||
          b.bottom.style != BorderStyle::None;
    return !ink;
  });
  // Or text from the left running into it.
  if (!ink && body.firstCol > 0)
    ForEachLast(sheet_, Range{body.firstRow, 0, body.lastRow, body.firstCol - 1}, [&](const CellInfo& c) {
      if (!ink && Reach(c, body.firstCol - 1) >= body.firstCol) ink = true;
    });
  return !ink;
}

void PrintLayout::Paginate() {
  const PageSetup& ps = sheet_.Page();
  PaperSize(info_.jobPaper ? info_.jobPaper : ps.paperSize, paperW_, paperH_);
  float w, h;
  if (!info_.jobPaper && PaperFromText(ps.paperWidth, w) && PaperFromText(ps.paperHeight, h)) paperW_ = w, paperH_ = h;
  if (ps.orientation == Orientation::Landscape) std::swap(paperW_, paperH_);

  // A chart sheet prints its chart on one page.
  if (info_.objects && info_.objects->IsChartSheet()) {
    // Landscape, unless the sheet says otherwise.
    if (paperW_ < paperH_ && sheet_.KeptXml().find("orientation=\"portrait\"") == std::string::npos)
      std::swap(paperW_, paperH_);
    PrintPage p;
    p.width = paperW_;
    p.height = paperH_;
    p.chart = true;
    p.number = ps.useFirstPageNumber ? ps.firstPageNumber : 1;
    pages_.push_back(p);
    return;
  }

  // The objects on the columns as they print at 100 %, to see how far they reach.
  colX_.assign(kMaxCols + 1, 0);
  for (uint32_t c = 0; c < kMaxCols; ++c) colX_[c + 1] = colX_[c] + ColPt(c, 1);
  PlaceObjects();

  // Sparklines (an extension of the sheet's) are drawn in their cells, which print with the rest.
  sparklines_.clear();
  {
    const std::string kept = sheet_.KeptXml();
    const size_t from = kept.find("sparklineGroups"), to = kept.rfind("sparklineGroups>");
    for (size_t at = kept.find(":sqref>", from); from != std::string::npos && at != std::string::npos && at < to;
         at = kept.find(":sqref>", at + 7)) {
      const size_t end = kept.find('<', at + 7);
      if (end == std::string::npos) break;
      const std::string_view list = std::string_view(kept).substr(at + 7, end - at - 7);
      for (size_t i = 0; i < list.size();) {
        size_t j = list.find(' ', i);
        if (j == std::string_view::npos) j = list.size();
        Range r;
        if (j > i && ParseRange(list.substr(i, j - i), r)) sparklines_.push_back(r);
        i = j + 1;
      }
    }
  }

  // What prints: the print area, else the cells in use.
  if (const DefinedName* n = book_.FindName("_xlnm.Print_Area", static_cast<int>(index_)); n && n->sheet == static_cast<int>(index_))
    areas_ = ParseAreas(n->formula);
  if (areas_.empty()) {
    std::optional<Range> used = sheet_.UsedRange();
    if (used) used->lastCol = SpillEnd(Range{0, 0, used->lastRow, used->lastCol});
    for (const Range& r : sparklines_) {
      if (!used) used = r;
      used->lastRow = std::max(used->lastRow, r.lastRow);
      used->lastCol = std::max(used->lastCol, r.lastCol);
    }
    // Pictures and shapes past the cells print too.
    if (auto drawn = ObjectsEnd()) {
      if (!used) used = Range{0, 0, drawn->first, drawn->second};
      used->lastRow = std::max(used->lastRow, drawn->first);
      used->lastCol = std::max(used->lastCol, drawn->second);
    }
    if (used) areas_.push_back(Range{0, 0, used->lastRow, used->lastCol});  // from A1, as Excel prints
  }
  if (const DefinedName* n = book_.FindName("_xlnm.Print_Titles", static_cast<int>(index_)); n && n->sheet == static_cast<int>(index_)) {
    for (const Range& r : ParseAreas(n->formula)) {
      if (r.firstCol == 0 && r.lastCol == kMaxCols - 1) titleRows_ = std::make_pair(r.firstRow, r.lastRow);
      else if (r.firstRow == 0 && r.lastRow == kMaxRows - 1) titleCols_ = std::make_pair(r.firstCol, r.lastCol);
    }
  }
  if (areas_.empty()) return;

  const PageMargins& m = ps.margins;
  const float availW = std::max(36.f, paperW_ - static_cast<float>((m.left + m.right) * 72));
  const float availH = std::max(36.f, paperH_ - static_cast<float>((m.top + m.bottom) * 72));
  const float titleH = titleRows_ ? RowsHeight(titleRows_->first, titleRows_->second) : 0;
  const bool fit = sheet_.Props().fitToPage;

  // Bands of columns (or rows) that fit across a page at a scale; manual breaks start new ones.
  auto bands = [&](uint32_t first, uint32_t last, bool cols, float scale, bool manual) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    const std::vector<PageBreak>& breaks = cols ? sheet_.ColBreaks() : sheet_.RowBreaks();
    auto size = [&](uint32_t i) {
      return cols ? ColPt(i, scale) : grid_.PrintY(i, i + 1);
    };
    auto manualAt = [&](uint32_t i) {
      if (!manual) return false;
      for (const PageBreak& b : breaks)
        if (b.id == i) return true;
      return false;
    };
    uint32_t start = first;
    while (start <= last) {
      // Hidden rows and columns don't start a page.
      while (start <= last && size(start) == 0) ++start;
      if (start > last) break;
      const auto& titles = cols ? titleCols_ : titleRows_;
      const float titleW = cols && titles ? ColsAt(titles->first, titles->second, scale) : 0;
      const float room = (cols ? availW : availH) / scale - (titles && start > titles->second ? (cols ? titleW : titleH) : 0);
      float used = 0;
      uint32_t end = start;
      while (end <= last) {
        if (end > start && manualAt(end)) break;
        const float s = size(end);
        if (end > start && used + s > room + 0.01f) break;
        used += s;
        ++end;
      }
      out.push_back({start, end - 1});
      start = end;
    }
    return out;
  };

  // The scale: as set, or the largest whole percent that fits the pages asked for.
  scale_ = std::clamp(ps.scale, 10u, 400u) / 100.f;
  if (fit) {
    float s = 1;
    for (const Range& a : areas_) {
      const float wNeed = ColsAt(a.firstCol, a.lastCol, 1), hNeed = RowsHeight(a.firstRow, a.lastRow);
      if (ps.fitToWidth > 0 && wNeed > 0) s = std::min(s, ps.fitToWidth * availW / wNeed);
      if (ps.fitToHeight > 0 && hNeed > 0) s = std::min(s, ps.fitToHeight * availH / hNeed);
    }
    // Columns in the scaled font's digits may fit a few percent more; whole rows and columns may
    // need a little less.
    int pct = std::clamp(static_cast<int>(std::floor(s * 100 + 1e-4)) + 3, 10, 100);
    for (; pct > 10; --pct) {
      bool ok = true;
      for (const Range& a : areas_) {
        if (ps.fitToWidth > 0 && bands(a.firstCol, a.lastCol, true, pct / 100.f, false).size() > ps.fitToWidth) ok = false;
        if (ps.fitToHeight > 0 && bands(a.firstRow, a.lastRow, false, pct / 100.f, false).size() > ps.fitToHeight) ok = false;
      }
      if (ok) break;
    }
    scale_ = pct / 100.f;
  }
  colX_.assign(kMaxCols + 1, 0);
  for (uint32_t c = 0; c < kMaxCols; ++c) colX_[c + 1] = colX_[c] + ColPt(c, scale_);
  if (scale_ != 1) PlaceObjects();

  int number = ps.useFirstPageNumber ? ps.firstPageNumber : 1;
  for (const Range& a : areas_) {
    const auto colBands = bands(a.firstCol, a.lastCol, true, scale_, !fit);
    const auto rowBands = bands(a.firstRow, a.lastRow, false, scale_, !fit);
    auto add = [&](const std::pair<uint32_t, uint32_t>& rb, const std::pair<uint32_t, uint32_t>& cb) {
      PrintPage p;
      p.width = paperW_;
      p.height = paperH_;
      p.body = Range{rb.first, cb.first, rb.second, cb.second};
      p.titleRows = titleRows_ && rb.first > titleRows_->second;
      p.titleCols = titleCols_ && cb.first > titleCols_->second;
      p.number = number++;
      pages_.push_back(p);
    };
    if (ps.pageOrder == PageOrder::OverThenDown) {
      for (const auto& rb : rowBands)
        for (const auto& cb : colBands) add(rb, cb);
    } else {
      for (const auto& cb : colBands)
        for (const auto& rb : rowBands) add(rb, cb);
    }
  }
  // Excel leaves out the pages at the end with nothing on them.
  while (pages_.size() > 1 && Blank(pages_.back().body)) pages_.pop_back();
}

void PrintLayout::Paint(draw::Painter& p, size_t index) {
  if (index >= pages_.size()) return;
  const PrintPage& page = pages_[index];
  const PageSetup& ps = sheet_.Page();
  const PageMargins& m = ps.margins;
  const float left = static_cast<float>(m.left * 72), top = static_cast<float>(m.top * 72);
  const float availW = paperW_ - static_cast<float>((m.left + m.right) * 72);
  const float availH = paperH_ - static_cast<float>((m.top + m.bottom) * 72);
  if (page.chart) {
    if (info_.objects) info_.objects->DrawChartSheet(p, {left, top, left + availW, top + availH});
    PaintHeaderFooter(p, page, true);
    PaintHeaderFooter(p, page, false);
    return;
  }

  // Headings: row numbers and column letters, in the default font.
  const Font& base = book_.GetStyles().DefaultFont();
  const float headH = ps.headings ? static_cast<float>(grid_.DefaultRowPx() * Grid::kPrintRowPt) : 0;
  const int digits = static_cast<int>(std::to_string(page.body.lastRow + 1).size());
  const float headW = ps.headings ? (std::max(digits, 1) * grid_.MaxDigit() + 8) * 0.75f : 0;

  const float tW = page.titleCols ? ColsWidth(titleCols_->first, titleCols_->second) : 0;
  const float tH = page.titleRows ? RowsHeight(titleRows_->first, titleRows_->second) : 0;
  const float bodyW = ColsWidth(page.body.firstCol, page.body.lastCol);
  const float bodyH = RowsHeight(page.body.firstRow, page.body.lastRow);
  const float totalW = (headW + tW + bodyW) * scale_, totalH = (headH + tH + bodyH) * scale_;
  float ox = left, oy = top;
  if (ps.horizontalCentered) ox += std::max(0.f, (availW - totalW) / 2);
  if (ps.verticalCentered) oy += std::max(0.f, (availH - totalH) / 2);

  PaintOptions o;
  o.gridlines = ps.gridLines;
  o.gridColor = 0xFF000000;
  o.errors = ps.errors;

  // Text wraps and shows "####" in the printed columns: wider than the screen's by the digit.
  const float widthScale = static_cast<float>(digits_[static_cast<int>(std::lround(scale_ * 100))] * Grid::kPrintDotPt /
                                              scale_ / grid_.MaxDigit() / kPtPerPx);
  p.Save();
  p.Transform(draw::Matrix::Scale(scale_, scale_) * draw::Matrix::Translate(ox, oy));
  auto block = [&](uint32_t r0, uint32_t r1, uint32_t c0, uint32_t c1, float x, float y, float w, float h) {
    if (w <= 0 || h <= 0) return;
    PageGeometry g(grid_, colX_, r0, c0, x, y, widthScale);
    p.Save();
    // A little room around it for the borders on its edges.
    p.ClipRect({x - 2, y - 2, x + w + 2, y + h + 2});
    const Range area{r0, c0, r1, c1};
    PaintCells(p, sheet_, text_, grid_, g, area, o);
    if (info_.objects) {
      // Pictures, shapes and charts over the cells, in the block's place.
      const draw::Rect cells = SheetRect(area);
      p.Transform(draw::Matrix::Translate(x - cells.left, y - cells.top));
      SheetObjects::DrawOptions d;
      d.printing = true;
      d.indicators = false;
      info_.objects->Draw(p, cells, d);
    }
    p.Restore();
  };
  const float x1 = headW + tW, y1 = headH + tH;
  if (page.titleRows && page.titleCols)
    block(titleRows_->first, titleRows_->second, titleCols_->first, titleCols_->second, headW, headH, tW, tH);
  if (page.titleRows)
    block(titleRows_->first, titleRows_->second, page.body.firstCol, page.body.lastCol, x1, headH, bodyW, tH);
  if (page.titleCols)
    block(page.body.firstRow, page.body.lastRow, titleCols_->first, titleCols_->second, headW, y1, tW, bodyH);
  block(page.body.firstRow, page.body.lastRow, page.body.firstCol, page.body.lastCol, x1, y1, bodyW, bodyH);

  if (ps.headings) {
    // Boxes with the column letters across the top and the row numbers down the side.
    const draw::Brush ink = draw::Brush::Solid({0, 0, 0, 1});
    const draw::Brush shade = draw::Brush::Solid({0.85f, 0.85f, 0.85f, 1});
    text::Fonts& fonts = text_.Fonts();
    text::TextStyle style;
    style.family = FontMeasure::Family(base, base);
    style.size = static_cast<float>(FontMeasure::Size(base, base));
    auto label = [&](const std::wstring& s, float l, float t, float r, float b) {
      p.FillRect({l, t, r, b}, shade);
      p.FillRect({l, b - 0.5f, r, b}, ink);
      p.FillRect({r - 0.5f, t, r, b}, ink);
      text::Paragraph para;
      para.text = s;
      para.runs.push_back({static_cast<uint32_t>(s.size()), &style});
      para.markStyle = &style;
      auto lines = text::Layout(fonts, para, 1e6f);
      if (lines.empty()) return;
      const text::Line& ln = lines.front();
      const float w = ln.contentRight - ln.contentLeft;
      const float baseline = (t + b) / 2 + (ln.ascent - ln.descent) / 2;
      for (const text::GlyphPiece& g : ln.pieces)
        p.DrawGlyphs({(l + r - w) / 2 + g.OriginX() - ln.contentLeft, baseline - g.rise}, g.Run(), ink);
    };
    auto cols = [&](uint32_t c0, uint32_t c1, float x) {
      for (uint32_t c = c0; c <= c1; ++c) {
        if (grid_.ColHidden(c)) continue;
        const float l = x + ColsWidth(c0, c - 1);
        label(Utf8ToWide(ColumnName(c)), l, 0, l + ColsWidth(c, c), headH);
      }
    };
    auto rows = [&](uint32_t r0, uint32_t r1, float y) {
      for (uint32_t r = r0; r <= r1; ++r) {
        if (grid_.RowHidden(r)) continue;
        const float t = y + grid_.PrintY(r0, r);
        label(std::to_wstring(r + 1), 0, t, headW, t + grid_.PrintY(r, r + 1));
      }
    };
    label(L"", 0, 0, headW, headH);
    if (page.titleCols) cols(titleCols_->first, titleCols_->second, headW);
    cols(page.body.firstCol, page.body.lastCol, x1);
    if (page.titleRows) rows(titleRows_->first, titleRows_->second, headH);
    rows(page.body.firstRow, page.body.lastRow, y1);
  }
  p.Restore();

  PaintHeaderFooter(p, page, true);
  PaintHeaderFooter(p, page, false);
}

void PrintLayout::PaintHeaderFooter(draw::Painter& p, const PrintPage& page, bool header) {
  const HeaderFooter& hf = sheet_.HeadersFooters();
  const bool first = hf.differentFirst && &page == &pages_.front() && page.number == 1;
  const bool even = hf.differentOddEven && page.number % 2 == 0;
  const std::string& code = first ? (header ? hf.firstHeader : hf.firstFooter)
                            : even ? (header ? hf.evenHeader : hf.evenFooter)
                                   : (header ? hf.oddHeader : hf.oddFooter);
  if (code.empty()) return;
  const std::wstring s = Utf8ToWide(code);
  const Font& base = book_.GetStyles().DefaultFont();
  const float docScale = hf.scaleWithDoc ? scale_ : 1;

  // Parse Excel's codes into three sections of runs.
  std::vector<HfRun> sections[3];
  int section = 1;  // center unless a section code comes first
  text::TextStyle style;
  style.family = FontMeasure::Family(base, base);
  style.size = static_cast<float>(FontMeasure::Size(base, base));
  const text::TextStyle fresh = style;
  uint32_t color = 0xFF000000;
  bool underline = false, strike = false;
  auto put = [&](const std::wstring& t) {
    if (t.empty()) return;
    auto& runs = sections[section];
    if (!runs.empty() && runs.back().style.family == style.family && runs.back().style.size == style.size &&
        runs.back().style.bold == style.bold && runs.back().style.italic == style.italic &&
        runs.back().style.verticalAlign == style.verticalAlign && runs.back().color == color &&
        runs.back().underline == underline && runs.back().strike == strike)
      runs.back().text += t;
    else
      runs.push_back({t, style, color, underline, strike});
  };
  const int pages = static_cast<int>(pages_.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != L'&' || i + 1 >= s.size()) {
      put(std::wstring(1, s[i]));
      continue;
    }
    const wchar_t c = s[++i];
    switch (c) {
      // Each section starts in the default font, black.
      case L'L':
      case L'C':
      case L'R':
        section = c == L'L' ? 0 : c == L'C' ? 1 : 2;
        style = fresh;
        color = 0xFF000000;
        underline = strike = false;
        break;
      case L'P': put(std::to_wstring(page.number)); break;
      case L'N':
        put(std::to_wstring(info_.jobPages ? info_.jobPages
                                           : pages + (sheet_.Page().useFirstPageNumber ? sheet_.Page().firstPageNumber - 1 : 0)));
        break;
      case L'D': put(info_.date); break;
      case L'T': put(info_.time); break;
      case L'F': put(info_.fileName); break;
      case L'Z': put(info_.path); break;
      case L'A': put(info_.sheetName); break;
      case L'&': put(L"&"); break;
      case L'B': style.bold = !style.bold; break;
      case L'I': style.italic = !style.italic; break;
      case L'U':
      case L'E': underline = !underline; break;
      case L'S': strike = !strike; break;
      case L'X':
        style.verticalAlign = style.verticalAlign == text::VerticalAlign::Superscript ? text::VerticalAlign::Baseline
                                                                                       : text::VerticalAlign::Superscript;
        break;
      case L'Y':
        style.verticalAlign = style.verticalAlign == text::VerticalAlign::Subscript ? text::VerticalAlign::Baseline
                                                                                     : text::VerticalAlign::Subscript;
        break;
      case L'K':
        if (i + 6 < s.size() && iswxdigit(s[i + 1])) {
          const std::wstring hex = s.substr(i + 1, 6);
          color = 0xFF000000u | static_cast<uint32_t>(wcstoul(hex.c_str(), nullptr, 16));
          i += 6;
        }
        break;
      case L'"': {
        const size_t end = s.find(L'"', i + 1);
        if (end == std::wstring::npos) break;
        const std::wstring spec = s.substr(i + 1, end - i - 1);
        i = end;
        const size_t comma = spec.find(L',');
        const std::wstring name = spec.substr(0, comma);
        const std::wstring kind = comma == std::wstring::npos ? L"" : ToLower(spec.substr(comma + 1));
        if (name != L"-" && !name.empty()) style.family = name;
        if (!kind.empty()) {
          style.bold = kind.find(L"bold") != std::wstring::npos;
          style.italic = kind.find(L"italic") != std::wstring::npos || kind.find(L"oblique") != std::wstring::npos;
        }
        break;
      }
      default:
        if (iswdigit(c)) {
          size_t j = i;
          while (j < s.size() && iswdigit(s[j])) ++j;
          style.size = static_cast<float>(_wtoi(s.substr(i, j - i).c_str()));
          i = j - 1;
        }
        break;
    }
  }

  const PageMargins& m = sheet_.Page().margins;
  const float l = hf.alignWithMargins ? static_cast<float>(m.left * 72) : 0.5f * 72;
  const float r = page.width - (hf.alignWithMargins ? static_cast<float>(m.right * 72) : 0.5f * 72);
  text::Fonts& fonts = text_.Fonts();
  for (int sec = 0; sec < 3; ++sec) {
    if (sections[sec].empty()) continue;
    std::wstring all;
    std::vector<text::TextStyle> styles;
    for (HfRun& run : sections[sec]) {
      run.style.size *= docScale;
      styles.push_back(run.style);
      all += run.text;
    }
    for (wchar_t& ch : all)
      if (ch == L'\r') ch = 0x200B;
    text::Paragraph para;
    para.text = all;
    for (size_t i = 0; i < sections[sec].size(); ++i)
      para.runs.push_back({static_cast<uint32_t>(sections[sec][i].text.size()), &styles[i]});
    para.markStyle = &styles.front();
    para.format.alignment = sec == 0 ? text::Alignment::Start : sec == 1 ? text::Alignment::Center : text::Alignment::End;
    const float width = r - l;
    const std::vector<text::Line> lines = text::Layout(fonts, para, std::max(width, 10.f));
    float height = 0;
    for (const text::Line& ln : lines) height += ln.ascent + ln.descent + ln.lineGap;
    float y = header ? static_cast<float>(m.header * 72) : page.height - static_cast<float>(m.footer * 72) - height;
    for (const text::Line& ln : lines) {
      const float baseline = y + ln.ascent;
      for (const text::GlyphPiece& g : ln.pieces) {
        const HfRun& run = sections[sec][std::min<size_t>(g.run, sections[sec].size() - 1)];
        const draw::Brush brush = draw::Brush::Solid(ToColor(run.color));
        p.DrawGlyphs({l + g.OriginX(), baseline - g.rise}, g.Run(), brush);
        if (run.underline) p.FillRect({l + g.x, baseline + 1.5f, l + g.x + g.width, baseline + 2.1f}, brush);
        if (run.strike) p.FillRect({l + g.x, baseline - g.emSize * 0.3f, l + g.x + g.width, baseline - g.emSize * 0.3f + 0.6f}, brush);
      }
      y += ln.ascent + ln.descent + ln.lineGap;
    }
  }
}

}  // namespace fw::sheet::view
