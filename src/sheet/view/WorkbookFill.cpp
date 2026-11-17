// The fill handle, Ctrl+D and Ctrl+R (s2-plan "Fill handle"): dragging the selection's corner
// copies it or extends its series, as Excel's AutoFill does: numbers by their trend (one number is
// copied, one date counts on by days), weekday and month names, text ending in a number, and
// formulas with their relative references moved. A double-click fills down as far as the column
// beside it goes. Every fill is one step through the edit API.
#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>

#include "core/Strings.h"
#include "sheet/calc/Typing.h"
#include "sheet/view/WorkbookView.h"
#include "ui/Theme.h"

using namespace gfx;

namespace fw::sheet::view {

namespace {

std::wstring LowerText(std::wstring s) {
  for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
  return s;
}

// Text ending (or else starting) with a number: "Item 7", "Q3", "3rd".
struct Numbered {
  std::wstring before, after;
  long long number = 0;
  size_t digits = 0;  // as typed ("007" keeps three)
};
bool SplitNumber(const std::wstring& s, Numbered& out) {
  size_t end = s.size();
  while (end > 0 && !std::iswdigit(s[end - 1])) --end;
  if (end == 0) return false;
  // The last run of digits if the text ends with it, else the first run at the start.
  size_t from = end;
  while (from > 0 && std::iswdigit(s[from - 1])) --from;
  if (end != s.size() && from != 0) return false;
  if (end - from > 15) return false;
  out.before = s.substr(0, from);
  out.after = s.substr(end);
  out.digits = end - from;
  out.number = std::stoll(s.substr(from, end - from));
  return !(out.before.empty() && out.after.empty());  // a plain number isn't text
}

std::wstring Join(const Numbered& n, long long v) {
  std::wstring d = std::to_wstring(std::llabs(v));
  if (d.size() < n.digits) d.insert(0, n.digits - d.size(), L'0');
  return n.before + d + n.after;
}

}  // namespace

// The cells a fill writes, `target` from `source` along `dir`: 0 down, 1 up, 2 right, 3 left.
// `series`: AutoFill's series; else every cell copies its source (Ctrl+D, Ctrl+R).
bool WorkbookView::FillCells(const Range& source, const Range& target, int dir, bool series, std::u16string name) {
  if (editor_.Active() && !CommitEdit(false)) return false;
  const int sheet = static_cast<int>(sheet_);
  const Sheet& s = Current();
  const Styles& styles = book_->GetStyles();
  const NumLocale& loc = text_->Locale();
  const bool vertical = dir <= 1, backwards = dir == 1 || dir == 3;
  const uint32_t lines0 = vertical ? source.firstCol : source.firstRow, lines1 = vertical ? source.lastCol : source.lastRow;
  const uint32_t n = vertical ? source.lastRow - source.firstRow + 1 : source.lastCol - source.firstCol + 1;
  const uint32_t m = vertical ? target.lastRow - target.firstRow + 1 : target.lastCol - target.firstCol + 1;
  if (uint64_t{m} * (lines1 - lines0 + 1) > 1000000) {
    host_.ViewMessage(BannerKind::Warning, L"That's more cells than Featherweight fills at once.");
    return false;
  }
  // Weekday and month names, as the region writes them: the lists AutoFill continues.
  const std::array<std::wstring, 7>* dayLists[] = {&loc.day, &loc.dayAbbr};
  const std::array<std::wstring, 12>* monthLists[] = {&loc.month, &loc.monthAbbr};
  auto listIndex = [&](const std::wstring& t, int& list, int& index) {
    const std::wstring low = LowerText(t);
    for (int l = 0; l < 2; ++l) {
      for (size_t i = 0; i < 7; ++i)
        if (LowerText((*dayLists[l])[i]) == low) return list = l, index = static_cast<int>(i), true;
      for (size_t i = 0; i < 12; ++i)
        if (LowerText((*monthLists[l])[i]) == low) return list = 2 + l, index = static_cast<int>(i), true;
    }
    return false;
  };
  calc::Engine& names = Calc().engine();
  std::vector<std::pair<CellAddr, edit::CellInput>> cells;
  for (uint32_t line = lines0; line <= lines1; ++line) {
    // The line's source cells, first to last along the fill.
    std::vector<CellInfo> src(n);
    std::vector<bool> stored(n, false);
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t r = vertical ? source.firstRow + i : line, c = vertical ? line : source.firstCol + i;
      stored[i] = s.GetCell(r, c, src[i]);
      if (!stored[i]) src[i] = CellInfo{r, c};
      src[i].row = r;
      src[i].col = c;
    }
    // What kind of series the line is.
    bool numbers = series, lists = series, numbered = series;
    bool date = false;
    int list = -1;
    std::vector<int> index(n, 0);
    std::vector<Numbered> parts(n);
    for (uint32_t i = 0; i < n; ++i) {
      const CellInfo& c = src[i];
      const bool number = !c.hasFormula && c.value.type == ValueType::Number;
      const bool text = !c.hasFormula && c.value.type == ValueType::String;
      numbers &= number;
      if (number && text_->FormatOf(styles.CellXf(c.style).numFmt).IsDateTime()) date = true;
      int l = -1;
      lists &= text && listIndex(Utf8ToWide(c.value.text), l, index[i]) && (list < 0 || list == l);
      if (lists) list = l;
      numbered &= text && SplitNumber(Utf8ToWide(c.value.text), parts[i]) && parts[i].before == parts[0].before &&
                  parts[i].after == parts[0].after;
    }
    // A trend for numbers: the least-squares line through them (one number is copied; one date
    // counts on by a day).
    double a = 0, b = 0;
    if (numbers) {
      if (n == 1) {
        a = src[0].value.number;
        b = date ? 1 : 0;
      } else {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (uint32_t i = 0; i < n; ++i) {
          const double x = i, y = src[i].value.number;
          sx += x, sy += y, sxx += x * x, sxy += x * y;
        }
        b = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        a = (sy - b * sx) / n;
      }
    }
    const int listStep = lists && n >= 2 ? index[1] - index[0] : 1;
    const long long numStep = numbered && n >= 2 ? parts[n - 1].number - parts[n - 2].number : 1;
    for (uint32_t k = 1; k <= m; ++k) {
      // The position along the line (0..n-1 are the sources) and the source this cell copies.
      const int64_t x = backwards ? -static_cast<int64_t>(k) : static_cast<int64_t>(n - 1 + k);
      const uint32_t from = backwards ? n - 1 - (k - 1) % n : (k - 1) % n;
      const CellInfo& c = src[from];
      const uint32_t tr = vertical ? (backwards ? source.firstRow - k : source.lastRow + k) : line;
      const uint32_t tc = vertical ? line : (backwards ? source.firstCol - k : source.lastCol + k);
      edit::CellInput in;
      if (numbers && (n > 1 || date)) {
        in = edit::CellInput::Number(a + b * static_cast<double>(x));
      } else if (lists) {
        const int len = list < 2 ? 7 : 12;
        const int edge = backwards ? index[0] : index[n - 1];
        const int64_t steps = backwards ? -static_cast<int64_t>(k) : static_cast<int64_t>(k);
        const int i = static_cast<int>(((edge + steps * listStep) % len + len) % len);
        const std::wstring& item = list < 2 ? (*dayLists[list])[static_cast<size_t>(i)] : (*monthLists[list - 2])[static_cast<size_t>(i)];
        in = edit::CellInput::Text(WideToUtf8(item));
      } else if (numbered) {
        const long long v = backwards ? parts[0].number - numStep * static_cast<long long>(k)
                                      : parts[n - 1].number + numStep * static_cast<long long>(k);
        in = edit::CellInput::Text(WideToUtf8(Join(parts[0], std::max(0LL, v))));
      } else if (c.hasFormula) {
        // The formula as it reads in the new cell (shared formulas from their first cell).
        FormulaInfo f;
        if (!s.GetFormula(c.row, c.col, f)) continue;
        calc::CompileOptions at, to;
        at.sheet = to.sheet = sheet;
        at.row = static_cast<int32_t>(f.origin.row);
        at.col = static_cast<int32_t>(f.origin.col);
        to.row = static_cast<int32_t>(tr);
        to.col = static_cast<int32_t>(tc);
        in = edit::CellInput::Formula(calc::CopyFormula("=" + std::string(f.text), at, to, names).substr(1));
      } else {
        switch (c.value.type) {
          case ValueType::Empty: break;
          case ValueType::Number: in = edit::CellInput::Number(c.value.number); break;
          case ValueType::String: in = edit::CellInput::Text(std::string(c.value.text)); break;
          case ValueType::Bool: in = edit::CellInput::Bool(c.value.boolean); break;
          case ValueType::Error: in = edit::CellInput::Error(c.value.error); break;
        }
      }
      in.WithStyle(c.style);  // the format comes along, as in Excel
      cells.emplace_back(CellAddr{tr, tc}, std::move(in));
    }
  }
  if (cells.empty()) return true;
  edit::EditGroup group(Ed(), std::move(name));
  if (EditRefused(Ed().SetCells(sheet, cells))) return false;
  group.done = true;
  return true;
}

void WorkbookView::FillDown(bool right) {
  // Ctrl+D: the selection's first row copied down it (one row: the row above into it); Ctrl+R
  // likewise from the left.
  Range sel = Selected();
  Range source = sel, target = sel;
  if (!right) {
    if (sel.firstRow == sel.lastRow) {
      if (sel.firstRow == 0) return;
      source.firstRow = source.lastRow = sel.firstRow - 1;
    } else {
      source.lastRow = sel.firstRow;
      target.firstRow = sel.firstRow + 1;
    }
  } else {
    if (sel.firstCol == sel.lastCol) {
      if (sel.firstCol == 0) return;
      source.firstCol = source.lastCol = sel.firstCol - 1;
    } else {
      source.lastCol = sel.firstCol;
      target.firstCol = sel.firstCol + 1;
    }
  }
  // Whole columns or rows: up to the used cells.
  if (const std::optional<Range> used = Current().UsedRange()) {
    if (target.lastRow == kMaxRows - 1) target.lastRow = std::max(target.firstRow, used->lastRow);
    if (target.lastCol == kMaxCols - 1) target.lastCol = std::max(target.firstCol, used->lastCol);
    if (source.lastRow == kMaxRows - 1) source.lastRow = target.lastRow;
    if (source.lastCol == kMaxCols - 1) source.lastCol = target.lastCol;
  }
  FillCells(source, target, right ? 2 : 0, false, right ? u"Fill Right" : u"Fill Down");
}

// ---------------------------------------------------------------------------
// The handle

bool WorkbookView::FillHandleAt(float x, float y, D2D1_RECT_F* rc) {
  if (!Ready() || preview_ || ChartSheet() || editor_.Active()) return false;
  const Range sel = Selected();
  if (sel.lastRow == kMaxRows - 1 && sel.lastCol == kMaxCols - 1) return false;
  const float h = std::round(3.5f * ui_) + 1;
  for (const Pane& p : Panes()) {
    if (sel.lastRow < p.row0 || sel.lastRow >= p.row1 || sel.lastCol < p.col0 || sel.lastCol >= p.col1) continue;
    const D2D1_RECT_F f = CellRect(p, sel.firstRow, sel.firstCol, sel.lastRow, sel.lastCol);
    const float cx = rtl_ ? f.left : f.right - 1, cy = f.bottom - 1;
    const D2D1_RECT_F box = {cx - h, cy - h, cx + h, cy + h};
    if (rc) *rc = box;
    if (!rc) return gfx::Contains(Inflate(box, 2, 2), x, y);
    return true;
  }
  return false;
}

void WorkbookView::DrawFillHandle(Canvas& c) {
  D2D1_RECT_F box;
  if (!FillHandleAt(0, 0, &box)) return;
  c.PushClip(cells_);
  c.Fill(Inflate(box, 1, 1), Rgb(0xFFFFFF));
  c.Fill(box, theme::P().accent);
  // While dragging, the cells the fill will take.
  if (mouse_ == Mouse::Fill && !(fillTo_ == Selected())) {
    for (const Pane& p : Panes()) {
      c.PushClip(RectF(p.rc));
      const D2D1_RECT_F f = CellRect(p, fillTo_.firstRow, fillTo_.firstCol, fillTo_.lastRow, fillTo_.lastCol);
      c.Stroke(Inflate(f, -0.5f, -0.5f), Rgb(0x707070), std::max(1.f, std::round(ui_)));
      c.PopClip();
    }
  }
  c.PopClip();
}

void WorkbookView::FillDrag(int x, int y) {
  const int px = static_cast<int>(std::clamp(static_cast<float>(x), cells_.left, cells_.right - 1));
  const int py = static_cast<int>(std::clamp(static_cast<float>(y), cells_.top, cells_.bottom - 1));
  auto cell = CellAt(px, py);
  if (!cell) return;
  // Along one direction only: the one the mouse went furthest in, in cells.
  const Range sel = Selected();
  Range to = sel;
  const int64_t dr = cell->row > sel.lastRow ? int64_t{cell->row} - sel.lastRow
                     : cell->row < sel.firstRow ? int64_t{cell->row} - sel.firstRow : 0;
  const int64_t dc = cell->col > sel.lastCol ? int64_t{cell->col} - sel.lastCol
                     : cell->col < sel.firstCol ? int64_t{cell->col} - sel.firstCol : 0;
  if (std::llabs(dr) >= std::llabs(dc) && dr != 0) (dr > 0 ? to.lastRow : to.firstRow) = cell->row;
  else if (dc != 0) (dc > 0 ? to.lastCol : to.firstCol) = cell->col;
  if (!(to == fillTo_)) {
    fillTo_ = to;
    ScrollIntoView(*cell);
    host_.ViewInvalidate();
  }
}

void WorkbookView::FinishFill() {
  const Range sel = Selected(), to = fillTo_;
  if (to == sel) return;
  int dir = 0;
  Range target = to;
  if (to.lastRow > sel.lastRow) dir = 0, target.firstRow = sel.lastRow + 1;
  else if (to.firstRow < sel.firstRow) dir = 1, target.lastRow = sel.firstRow - 1;
  else if (to.lastCol > sel.lastCol) dir = 2, target.firstCol = sel.lastCol + 1;
  else dir = 3, target.lastCol = sel.firstCol - 1;
  if (FillCells(sel, target, dir, true, u"AutoFill")) Select({to.firstRow, to.firstCol}, {to.lastRow, to.lastCol}, false);
}

void WorkbookView::FillToNeighbour() {
  // Double-click: down as far as the data in the column beside it (left, else right) goes.
  const Range sel = Selected();
  const Sheet& s = Current();
  auto extent = [&](int64_t col) -> uint32_t {
    if (col < 0 || col >= kMaxCols) return 0;
    uint32_t last = sel.lastRow;
    CellInfo c;
    while (last + 1 < kMaxRows && s.GetCell(last + 1, static_cast<uint32_t>(col), c) &&
           (c.value.type != ValueType::Empty || c.hasFormula))
      ++last;
    return last;
  };
  uint32_t last = extent(int64_t{sel.firstCol} - 1);
  if (last <= sel.lastRow) last = extent(int64_t{sel.lastCol} + 1);
  if (last <= sel.lastRow) return;
  Range target = sel;
  target.firstRow = sel.lastRow + 1;
  target.lastRow = last;
  if (FillCells(sel, target, 0, true, u"AutoFill")) Select({sel.firstRow, sel.firstCol}, {last, sel.lastCol}, false);
}

}  // namespace fw::sheet::view
