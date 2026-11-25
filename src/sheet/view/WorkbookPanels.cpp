#include "sheet/view/WorkbookPanels.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

#include "core/Lang.h"
#include "core/Strings.h"
#include "sheet/numfmt/NumFormat.h"
#include "sheet/calc/Typing.h"
#include "ui/Theme.h"

using namespace gfx;

namespace fw::sheet::view {

namespace {

bool SameText(std::wstring_view a, std::wstring_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
           return std::towlower(x) == std::towlower(y);
         });
}

}  // namespace

// ---------------------------------------------------------------------------
// Row height and column width

WorkbookView::SizePanel::SizePanel(WorkbookView& view) : v_(view) {
  size_.label = T(L"Size");
  size_.onChange = [this](double value) { v_.SetSizes(rows_, value); };
  size_.onEnter = [this] { v_.host_.ViewFocusPage(); };  // the panel stays open, as Format Cells
}

std::vector<Control*> WorkbookView::SizePanel::Layout(const D2D1_RECT_F& body, float ui) {
  ui_ = ui;
  const float left = body.left + 16 * ui, right = body.right - 16 * ui;
  labelRc_ = {left, body.top + 12 * ui, right, body.top + 30 * ui};
  size_.rc = {left, body.top + 34 * ui, left + 140 * ui, body.top + 66 * ui};
  noteRc_ = {left, size_.rc.bottom + 12 * ui, right, size_.rc.bottom + 52 * ui};
  return {&size_};
}

void WorkbookView::SizePanel::Draw(Canvas& c) {
  const Palette& p = theme::P();
  c.Text(rows_ ? T(L"Height of the selected rows") : T(L"Width of the selected columns"), labelRc_, p.textSecondary, 12 * ui_);
  c.TextWrapped(rows_ ? T(L"In points. 0 hides the rows.") : T(L"In characters of the default font. 0 hides the columns."),
                noteRc_, p.textSecondary, 12 * ui_, 2);
  if (focusWanted_) {
    size_.TakeFocus();
    focusWanted_ = false;
  }
}

void WorkbookView::SizePanel::Open(bool rows) {
  rows_ = rows;
  size_.unit = rows ? L" pt" : L"";
  size_.decimals = 2;
  size_.minValue = 0;
  size_.maxValue = rows ? 409 : 255;  // Excel's limits
  size_.step = rows ? 0.75 : 1;
  size_.SetValue(v_.CurrentSize(rows));
  focusWanted_ = true;
}

// ---------------------------------------------------------------------------
// Go to

WorkbookView::GoToPanel::GoToPanel(WorkbookView& view) : v_(view) {
  ref_.label = T(L"Reference");
  ref_.tooltip = T(L"A cell (B12), a range (A1:C5), another sheet's (Sheet2!A1) or a name; a new name names the selection");
  ref_.onEnter = [this] {
    if (v_.GoTo(ref_.Text())) {
      note_.clear();
      v_.host_.ViewShowPanel(nullptr);
      v_.host_.ViewFocusPage();
    } else {
      note_ = T(L"There's no cell, range or name like that.");
      v_.host_.ViewInvalidate();
    }
  };
}

std::vector<Control*> WorkbookView::GoToPanel::Layout(const D2D1_RECT_F& body, float ui) {
  ui_ = ui;
  const float left = body.left + 16 * ui, right = body.right - 16 * ui;
  labelRc_ = {left, body.top + 12 * ui, right, body.top + 30 * ui};
  ref_.rc = {left, body.top + 34 * ui, right, body.top + 66 * ui};
  noteRc_ = {left, ref_.rc.bottom + 12 * ui, right, ref_.rc.bottom + 52 * ui};
  return {&ref_};
}

void WorkbookView::GoToPanel::Draw(Canvas& c) {
  const Palette& p = theme::P();
  c.Text(T(L"Reference or name"), labelRc_, p.textSecondary, 12 * ui_);
  c.TextWrapped(note_.empty() ? T(L"Enter goes there.") : note_, noteRc_, note_.empty() ? p.textSecondary : p.text, 12 * ui_, 2);
  if (focusWanted_) {
    ref_.TakeFocus();
    focusWanted_ = false;
  }
}

void WorkbookView::GoToPanel::Open() {
  note_.clear();
  std::vector<std::wstring> names;
  for (const DefinedName& n : v_.book_->Names())
    if (!n.hidden && n.name.rfind("_xlnm.", 0) != 0 && (n.sheet < 0 || n.sheet == static_cast<int>(v_.sheet_)))
      names.push_back(Utf8ToWide(n.name));
  std::sort(names.begin(), names.end());
  ref_.items = std::move(names);
  ref_.SetText(L"");
  focusWanted_ = true;
}

// ---------------------------------------------------------------------------
// The view's side of them

void WorkbookView::ShowSizePanel(bool rows) {
  if (!sizePanel_) sizePanel_ = std::make_unique<SizePanel>(*this);
  sizePanel_->Open(rows);
  host_.ViewShowPanel(sizePanel_.get());
}

void WorkbookView::ShowGoTo() {
  if (!goToPanel_) goToPanel_ = std::make_unique<GoToPanel>(*this);
  goToPanel_->Open();
  host_.ViewShowPanel(goToPanel_.get());
}

// Excel shows a column's width in characters of the default font's widest digit, without the
// 5 pixels of padding the file's width includes (ECMA-376 §18.3.1.13).
constexpr int kColumnPadPx = 5;

double WorkbookView::DisplayWidth(int px) const {
  const int mdw = std::max(1, grid_.MaxDigit());
  if (px <= 0) return 0;
  if (px < mdw + kColumnPadPx) return std::round(px * 100.0 / (mdw + kColumnPadPx)) / 100;
  return std::trunc((px - kColumnPadPx) * 100.0 / mdw + 0.5) / 100;
}

double WorkbookView::FileWidth(double display) const {
  const int mdw = std::max(1, grid_.MaxDigit());
  if (display <= 0) return 0;
  const double px = display < 1 ? display * (mdw + kColumnPadPx) : display * mdw + kColumnPadPx;
  return std::min(255.0, std::trunc(px / mdw * 256) / 256);
}

double WorkbookView::CurrentSize(bool rows) const {
  // As the grid shows it: points, or Excel's characters (8.43 for Calibri 11's default).
  if (rows) return grid_.RowHeight(active_.row) * 0.75;
  return DisplayWidth(grid_.ColWidth(active_.col));
}

void WorkbookView::ApplySize(bool rows, uint32_t first, uint32_t last, double value, const char16_t* name) {
  const int sheet = static_cast<int>(sheet_);
  edit::EditGroup group(Ed(), name);
  const std::vector<HeldSize> held = HeldSizes();  // s3-data: pictures keep their size
  // 0 hides them, as in Excel; another size shows them again.
  edit::Result r = rows ? Ed().HideRows(sheet, first, last, value <= 0) : Ed().HideColumns(sheet, first, last, value <= 0);
  if (r && value > 0)
    r = rows ? Ed().SetRowHeight(sheet, first, last, value) : Ed().SetColumnWidth(sheet, first, last, value);
  if (r) r = KeepSizes(held);
  if (EditRefused(r)) return;
  group.done = true;
}

void WorkbookView::SetSizes(bool rows, double value) {
  const Range sel = Selected();
  if (rows) ApplySize(true, sel.firstRow, sel.lastRow, value, T(u"Row Height"));
  else ApplySize(false, sel.firstCol, sel.lastCol, FileWidth(value), T(u"Column Width"));
}

// ---------------------------------------------------------------------------
// Sizes from the headers' edges

std::optional<WorkbookView::SizeEdge> WorkbookView::SizeEdgeAt(int x, int y) const {
  if (!Ready() || preview_ || ChartSheet() || !headers_) return std::nullopt;
  const float fx = static_cast<float>(x), fy = static_cast<float>(y);
  const bool cols = gfx::Contains(colHeader_, fx, fy), rows = !cols && gfx::Contains(rowHeader_, fx, fy);
  if (!cols && !rows) return std::nullopt;
  const float at = cols ? Mirror(fx) : fy;  // left to right
  const double k = K();
  const double slop = 4.0 * ui_;
  for (const Pane& p : Panes()) {
    const float lo = static_cast<float>(cols ? p.rc.left : p.rc.top), hi = static_cast<float>(cols ? p.rc.right : p.rc.bottom);
    if (at < lo - slop || at >= hi) continue;
    const double g = (cols ? p.sx : p.sy) + (at - lo) / k;  // Excel pixels
    const uint32_t first = cols ? p.col0 : p.row0;
    const uint32_t end = std::min(cols ? p.col1 : p.row1, cols ? kMaxCols : kMaxRows);
    if (end == 0) continue;
    uint32_t i = cols ? grid_.ColAt(static_cast<int64_t>(std::floor(g))) : grid_.RowAt(static_cast<int64_t>(std::floor(g)));
    i = std::clamp(i, first, end - 1);
    auto begin = [&](uint32_t n) { return static_cast<double>(cols ? grid_.ColLeft(n) : grid_.RowTop(n)); };
    auto hidden = [&](uint32_t n) { return cols ? grid_.ColHidden(n) : grid_.RowHidden(n); };
    uint32_t index;
    if ((begin(i + 1) - g) * k <= slop) {
      index = i;  // its own far edge
    } else if ((g - begin(i)) * k <= slop && i > first) {
      index = i - 1;  // the edge before it: the last shown one's
      while (index > first && hidden(index)) --index;
    } else {
      return std::nullopt;
    }
    SizeEdge e;
    e.rows = rows;
    e.index = index;
    e.start = lo + static_cast<float>((begin(index) - (cols ? p.sx : p.sy)) * k);
    e.edge = lo + static_cast<float>((begin(index + 1) - (cols ? p.sx : p.sy)) * k);
    return e;
  }
  return std::nullopt;
}

void WorkbookView::FinishSizing() {
  if (!sizing_) return;
  const SizeEdge e = *sizing_;
  sizing_.reset();
  if (std::abs(sizeTo_ - e.edge) < 1) return;  // a click, or the first of a double-click
  const int px = std::max(0, static_cast<int>(std::lround((sizeTo_ - e.start) / K())));
  // The selected whole rows or columns all take it when the edge is one of theirs.
  const Range sel = Selected();
  uint32_t first = e.index, last = e.index;
  if (e.rows && WholeRows() && e.index >= sel.firstRow && e.index <= sel.lastRow) {
    first = sel.firstRow;
    last = sel.lastRow;
  } else if (!e.rows && WholeColumns() && e.index >= sel.firstCol && e.index <= sel.lastCol) {
    first = sel.firstCol;
    last = sel.lastCol;
  }
  if (e.rows) ApplySize(true, first, last, px * 0.75, T(u"Row Height"));
  else ApplySize(false, first, last, FileWidth(DisplayWidth(px)), T(u"Column Width"));
}

void WorkbookView::FitAtEdge(const SizeEdge& e) {
  const Range sel = Selected();
  if (e.rows && WholeRows() && e.index >= sel.firstRow && e.index <= sel.lastRow)
    AutoFit(true, sel.firstRow, sel.lastRow, nullptr);
  else if (!e.rows && WholeColumns() && e.index >= sel.firstCol && e.index <= sel.lastCol)
    AutoFit(false, sel.firstCol, sel.lastCol, nullptr);
  else
    AutoFit(e.rows, e.index, e.index, nullptr);
}

void WorkbookView::AutoFit(bool rows, uint32_t first, uint32_t last, const Range* within) {
  if (!Ready() || ChartSheet() || first > last) return;
  const Sheet& s = Current();
  const Styles& styles = book_->GetStyles();
  // Each row's tallest cell in points, or each column's widest text in pixels; merged cells
  // don't count, as in Excel.
  std::vector<double> need(static_cast<size_t>(last - first) + 1, -1);
  Range r = rows ? Range{first, 0, last, kMaxCols - 1} : Range{0, first, kMaxRows - 1, last};
  if (within) {
    if (rows) r.firstCol = within->firstCol, r.lastCol = within->lastCol;
    else r.firstRow = within->firstRow, r.lastRow = within->lastRow;
  }
  std::vector<Range> merges;  // the ones in reach
  for (const Range& m : s.Merges())
    if (m.lastRow >= r.firstRow && m.firstRow <= r.lastRow && m.lastCol >= r.firstCol && m.firstCol <= r.lastCol)
      merges.push_back(m);
  auto merged = [&](uint32_t row, uint32_t col) {
    for (const Range& m : merges)
      if (row >= m.firstRow && row <= m.lastRow && col >= m.firstCol && col <= m.lastCol) return true;
    return false;
  };
  s.ForEachCell(r, [&](const CellInfo& cell) {
    if (merged(cell.row, cell.col)) return true;
    const Xf& xf = styles.CellXf(cell.style);
    if (xf.alignment.rotation != 0) return true;
    double& n = need[rows ? cell.row - first : cell.col - first];
    if (rows) {
      // Wrapped text breaks inside its column, as drawn.
      const CellText::Layout& l = text_->Get(s, cell, grid_.ColWidth(cell.col));
      const size_t lines = std::max<size_t>(1, l.lines.size());
      n = std::max(n, l.lines.empty() ? static_cast<double>(l.pitch) : l.pitch * static_cast<double>(lines));
    } else {
      const CellText::Layout& l = text_->Get(s, cell, 1 << 20);  // the whole text, unwrapped
      if (l.lines.empty()) return true;
      const double indent = xf.alignment.indent * 3 * text_->Measure().Of(styles.FontOf(xf), styles.DefaultFont()).space;
      // The text and the 5 pixels of padding Excel counts, with a pixel or two to spare.
      n = std::max(n, std::ceil(l.width * 4.0 / 3 + indent) + kColumnPadPx + 2);
    }
    return true;
  });
  text_->Clear();  // laid out at other widths
  const int sheet = static_cast<int>(sheet_);
  edit::EditGroup group(Ed(), rows ? T(u"AutoFit Row Height") : T(u"AutoFit Column Width"));
  const std::vector<HeldSize> held = HeldSizes();  // s3-data: pictures keep their size
  edit::Result res;
  for (uint32_t i = first; i <= last && res; ++i) {
    const double n = need[i - first];
    if (n < 0) continue;  // nothing to fit: as it is
    if (rows) {
      const int px = static_cast<int>(std::ceil(n * 4.0 / 3 - 0.01));
      // The default height is no height of its own.
      const double points = px == grid_.DefaultRowPx() ? 0 : std::min(409.0, px * 0.75);
      res = Ed().HideRows(sheet, i, i, false);
      if (res) res = Ed().SetRowHeight(sheet, i, i, points);
    } else {
      const int px = static_cast<int>(n);
      double display = DisplayWidth(px);
      while (grid_.CharsToPx(FileWidth(display)) < px && display < 255) display += 0.01;
      res = Ed().HideColumns(sheet, i, i, false);
      if (res) res = Ed().SetColumnWidth(sheet, i, i, FileWidth(display));
    }
  }
  if (res) res = KeepSizes(held);
  if (EditRefused(res)) return;
  group.done = true;
}

void WorkbookView::DrawSizeGuide(gfx::Canvas& c) {
  if (mouse_ != Mouse::Size || !sizing_) return;
  const SizeEdge& e = *sizing_;
  const Palette& p = theme::P();
  const float u = ui_, to = std::max(sizeTo_, e.start);
  const int px = static_cast<int>(std::lround((to - e.start) / K()));
  std::wstring label;
  D2D1_POINT_2F at{};
  if (e.rows) {
    c.Fill({rowHeader_.left, to - 0.5f * u, cells_.right, to + 0.5f * u}, p.text);
    label = Fmt(T(L"Height: {0} ({1} pixels)"), {FormatGeneral(px * 0.75, 15, text_->Locale()), std::to_wstring(px)});
    at = {rowHeader_.right + 4 * u, to - 26 * u};
  } else {
    const float x = Mirror(to);
    c.Fill({x - 0.5f * u, colHeader_.top, x + 0.5f * u, cells_.bottom}, p.text);
    label = Fmt(T(L"Width: {0} ({1} pixels)"),
                {FormatGeneral(DisplayWidth(px), 15, text_->Locale()), std::to_wstring(px)});
    at = {x + 4 * u, colHeader_.top - 24 * u};
  }
  const float size = 12 * u, pad = 6 * u;
  Canvas measure;
  const float w = measure.MeasureText(label, size) + 2 * pad, h = std::round(20 * u);
  if (at.x + w > cells_.right) at.x = std::max(cells_.left, at.x - w - 8 * u);
  at.y = std::max(at.y, 0.f);
  const D2D1_RECT_F box = {at.x, at.y, at.x + w, at.y + h};
  c.FillRound(box, 3 * u, p.popupBg);
  c.StrokeRound(box, 3 * u, p.popupBorder, 1);
  c.Text(label, {box.left + pad, box.top, box.right, box.bottom}, p.text, size);
}

bool WorkbookView::GoTo(const std::wstring& typed) {
  std::wstring text = typed;
  while (!text.empty() && std::iswspace(text.back())) text.pop_back();
  while (!text.empty() && std::iswspace(text.front())) text.erase(text.begin());
  if (text.empty() || !Ready()) return false;
  // A defined name goes where it refers.
  for (const DefinedName& n : book_->Names())
    if ((n.sheet < 0 || n.sheet == static_cast<int>(sheet_)) && SameText(Utf8ToWide(n.name), text)) {
      text = Utf8ToWide(n.formula);
      break;
    }
  const std::wstring f = L"=" + text;
  const std::vector<calc::RefSpan> spans = calc::ReferenceSpans(f, Region());
  if (spans.size() != 1 || spans[0].begin != 1 || spans[0].end != f.size()) return DefineFromNameBox(text);
  const calc::RefSpan& s = spans[0];
  size_t target = sheet_;
  if (!s.sheet.empty()) {
    const int i = book_->FindSheet(WideToUtf8(s.sheet));
    if (i < 0 || !SheetShown(static_cast<size_t>(i))) return false;
    target = static_cast<size_t>(i);
  }
  if (target != sheet_) ShowSheet(target);
  Select({static_cast<uint32_t>(s.r1), static_cast<uint32_t>(s.c1)}, {static_cast<uint32_t>(s.r2), static_cast<uint32_t>(s.c2)});
  return true;
}

}  // namespace fw::sheet::view
