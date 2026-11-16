#include "sheet/view/PrintLayout.h"

#include <algorithm>
#include <cmath>

#include "core/Strings.h"
#include "text/Fonts.h"
#include "text/LineLayout.h"

namespace fw::sheet::view {

namespace {

// Cells placed on a page: sheet points from the block's top-left at (x0, y0).
class PageGeometry : public Geometry {
 public:
  PageGeometry(const Grid& grid, uint32_t row0, uint32_t col0, float x0, float y0)
      : grid_(grid), row0_(row0), col0_(col0), x0_(x0), y0_(y0) {
    pt = 1;
    px = 0.75f;
    line = 0.75f;
  }
  float Left(uint32_t c) const override { return x0_ + (grid_.ColLeft(c) - grid_.ColLeft(col0_)) * 0.75f; }
  float Right(uint32_t c) const override { return x0_ + (grid_.ColLeft(c + 1) - grid_.ColLeft(col0_)) * 0.75f; }
  float Top(uint32_t r) const override { return y0_ + (grid_.RowTop(r) - grid_.RowTop(row0_)) * 0.75f; }
  float Bottom(uint32_t r) const override { return y0_ + (grid_.RowTop(r + 1) - grid_.RowTop(row0_)) * 0.75f; }

 private:
  const Grid& grid_;
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
  return last < first ? 0 : (grid_.ColLeft(last + 1) - grid_.ColLeft(first)) * 0.75f;
}

float PrintLayout::RowsHeight(uint32_t first, uint32_t last) const {
  return last < first ? 0 : (grid_.RowTop(last + 1) - grid_.RowTop(first)) * 0.75f;
}

void PrintLayout::Paginate() {
  const PageSetup& ps = sheet_.Page();
  uint32_t paper = ps.paperSize;
  if (sheet_.KeptXml().find("paperSize=") == std::string::npos) paper = info_.defaultPaper;
  PaperSize(paper, paperW_, paperH_);
  float w, h;
  if (PaperFromText(ps.paperWidth, w) && PaperFromText(ps.paperHeight, h)) paperW_ = w, paperH_ = h;
  if (ps.orientation == Orientation::Landscape) std::swap(paperW_, paperH_);

  // What prints: the print area, else the cells in use.
  if (const DefinedName* n = book_.FindName("_xlnm.Print_Area", static_cast<int>(index_)); n && n->sheet == static_cast<int>(index_))
    areas_ = ParseAreas(n->formula);
  if (areas_.empty())
    if (auto used = sheet_.UsedRange()) areas_.push_back(*used);
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
  const float titleW = titleCols_ ? ColsWidth(titleCols_->first, titleCols_->second) : 0;
  const float titleH = titleRows_ ? RowsHeight(titleRows_->first, titleRows_->second) : 0;
  const bool fit = sheet_.Props().fitToPage;

  // Bands of columns (or rows) that fit across a page at a scale; manual breaks start new ones.
  auto bands = [&](uint32_t first, uint32_t last, bool cols, float scale, bool manual) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    const std::vector<PageBreak>& breaks = cols ? sheet_.ColBreaks() : sheet_.RowBreaks();
    auto size = [&](uint32_t i) {
      return cols ? grid_.ColWidth(i) * 0.75f : grid_.RowHeight(i) * 0.75f;
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
      const float wNeed = ColsWidth(a.firstCol, a.lastCol), hNeed = RowsHeight(a.firstRow, a.lastRow);
      if (ps.fitToWidth > 0 && wNeed > 0) s = std::min(s, ps.fitToWidth * availW / wNeed);
      if (ps.fitToHeight > 0 && hNeed > 0) s = std::min(s, ps.fitToHeight * availH / hNeed);
    }
    int pct = std::clamp(static_cast<int>(std::floor(s * 100 + 1e-4)), 10, 100);
    // Whole rows and columns may need a little less.
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
}

void PrintLayout::Paint(draw::Painter& p, size_t index) {
  if (index >= pages_.size()) return;
  const PrintPage& page = pages_[index];
  const PageSetup& ps = sheet_.Page();
  const PageMargins& m = ps.margins;
  const float left = static_cast<float>(m.left * 72), top = static_cast<float>(m.top * 72);
  const float availW = paperW_ - static_cast<float>((m.left + m.right) * 72);
  const float availH = paperH_ - static_cast<float>((m.top + m.bottom) * 72);

  // Headings: row numbers and column letters, in the default font.
  const Font& base = book_.GetStyles().DefaultFont();
  const float headH = ps.headings ? grid_.DefaultRowPx() * 0.75f : 0;
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

  p.Save();
  p.Transform(draw::Matrix::Scale(scale_, scale_) * draw::Matrix::Translate(ox, oy));
  auto block = [&](uint32_t r0, uint32_t r1, uint32_t c0, uint32_t c1, float x, float y, float w, float h) {
    if (w <= 0 || h <= 0) return;
    PageGeometry g(grid_, r0, c0, x, y);
    p.Save();
    p.ClipRect({x, y, x + w, y + h});
    const Range area{r0, c0, r1, c1};
    PaintCells(p, sheet_, text_, grid_, g, area, o);
    if (const OverlayFn& overlay = Overlay()) overlay(p, sheet_, g, area);
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
        const float l = x + (grid_.ColLeft(c) - grid_.ColLeft(c0)) * 0.75f;
        label(Utf8ToWide(ColumnName(c)), l, 0, l + grid_.ColWidth(c) * 0.75f, headH);
      }
    };
    auto rows = [&](uint32_t r0, uint32_t r1, float y) {
      for (uint32_t r = r0; r <= r1; ++r) {
        if (grid_.RowHidden(r)) continue;
        const float t = y + (grid_.RowTop(r) - grid_.RowTop(r0)) * 0.75f;
        label(std::to_wstring(r + 1), 0, t, headW, t + grid_.RowHeight(r) * 0.75f);
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
  const bool first = hf.differentFirst && &page == &pages_.front();
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
      case L'L': section = 0; break;
      case L'C': section = 1; break;
      case L'R': section = 2; break;
      case L'P': put(std::to_wstring(page.number)); break;
      case L'N': put(std::to_wstring(pages + (sheet_.Page().useFirstPageNumber ? sheet_.Page().firstPageNumber - 1 : 0))); break;
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
