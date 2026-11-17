// The workbook tab's selection, keyboard and mouse, and outline groups.

#include <algorithm>
#include <cmath>

#include "Commands.h"
#include "core/Strings.h"
#include "sheet/view/WorkbookView.h"
#include "ui/Theme.h"

using namespace gfx;

namespace fw::sheet::view {

// ---------------------------------------------------------------------------
// Selection

Range WorkbookView::Expand(Range r) const {
  // Merged areas the range touches join it, until nothing more joins.
  const Sheet& s = Current();
  for (bool grew = true; grew;) {
    grew = false;
    for (const Range& m : s.Merges()) {
      if (m.lastRow < r.firstRow || m.firstRow > r.lastRow || m.lastCol < r.firstCol || m.firstCol > r.lastCol) continue;
      if (m.firstRow < r.firstRow || m.lastRow > r.lastRow || m.firstCol < r.firstCol || m.lastCol > r.lastCol) {
        r.firstRow = std::min(r.firstRow, m.firstRow);
        r.lastRow = std::max(r.lastRow, m.lastRow);
        r.firstCol = std::min(r.firstCol, m.firstCol);
        r.lastCol = std::max(r.lastCol, m.lastCol);
        grew = true;
      }
    }
  }
  return r;
}

Range WorkbookView::Selected() const {
  Range r{std::min(active_.row, end_.row), std::min(active_.col, end_.col), std::max(active_.row, end_.row),
          std::max(active_.col, end_.col)};
  // Whole rows and columns stay whole; merged areas only matter inside the sheet's cells in use.
  if ((r.firstRow == 0 && r.lastRow == kMaxRows - 1) || (r.firstCol == 0 && r.lastCol == kMaxCols - 1)) return r;
  return Expand(r);
}

void WorkbookView::Select(CellAddr active, CellAddr end, bool scroll) {
  active.row = std::min(active.row, kMaxRows - 1);
  active.col = std::min(active.col, kMaxCols - 1);
  end.row = std::min(end.row, kMaxRows - 1);
  end.col = std::min(end.col, kMaxCols - 1);
  active_ = active;
  end_ = end;
  if (scroll) ScrollIntoView(end);
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

bool WorkbookView::HasValue(uint32_t r, uint32_t c) const {
  return Current().GetValue(r, c).type != ValueType::Empty;
}

uint32_t WorkbookView::StepRow(uint32_t r, int dir) const {
  uint32_t x = r;
  do {
    if (dir < 0 && x == 0) return r;
    if (dir > 0 && x + 1 >= kMaxRows) return r;
    x = dir < 0 ? x - 1 : x + 1;
  } while (grid_.RowHidden(x));
  return x;
}

uint32_t WorkbookView::StepCol(uint32_t c, int dir) const {
  uint32_t x = c;
  do {
    if (dir < 0 && x == 0) return c;
    if (dir > 0 && x + 1 >= kMaxCols) return c;
    x = dir < 0 ? x - 1 : x + 1;
  } while (grid_.ColHidden(x));
  return x;
}

CellAddr WorkbookView::JumpEdge(CellAddr from, int dr, int dc) const {
  // Excel's Ctrl+arrow: along a run of values to its last one, else to the next value, else to
  // the sheet's edge.
  const Sheet& s = Current();
  auto step = [&](CellAddr a) { return CellAddr{dr ? StepRow(a.row, dr) : a.row, dc ? StepCol(a.col, dc) : a.col}; };
  CellAddr cur = from, next = step(cur);
  if (next == cur) return cur;
  if (HasValue(cur.row, cur.col) && HasValue(next.row, next.col)) {
    while (true) {
      const CellAddr n = step(next);
      if (n == next || !HasValue(n.row, n.col)) return next;
      next = n;
    }
  }
  // The next cell with a value along the line: the stored cells say where.
  std::optional<CellAddr> found;
  Range line;
  if (dr > 0) line = {next.row, cur.col, kMaxRows - 1, cur.col};
  else if (dr < 0) line = {0, cur.col, next.row, cur.col};
  else if (dc > 0) line = {cur.row, next.col, cur.row, kMaxCols - 1};
  else line = {cur.row, 0, cur.row, next.col};
  s.ForEachCell(line, [&](const CellInfo& c) {
    if (c.value.type == ValueType::Empty || grid_.RowHidden(c.row) || grid_.ColHidden(c.col)) return true;
    found = CellAddr{c.row, c.col};
    return dr < 0 || dc < 0;  // backwards: keep the last one before
  });
  if (found) return *found;
  if (dr > 0) return {StepRow(kMaxRows - 1, 0) == kMaxRows - 1 ? kMaxRows - 1 : kMaxRows - 1, cur.col};
  if (dr < 0) return {0, cur.col};
  if (dc > 0) return {cur.row, kMaxCols - 1};
  return {cur.row, 0};
}

void WorkbookView::MoveActive(int dr, int dc, bool extend, bool jump) {
  const Sheet& s = Current();
  CellAddr from = extend ? end_ : active_;
  // From a merged area, step from its far edge in that direction.
  if (const Range* m = s.MergeAt(from.row, from.col)) {
    if (dr > 0) from.row = m->lastRow;
    if (dr < 0) from.row = m->firstRow;
    if (dc > 0) from.col = m->lastCol;
    if (dc < 0) from.col = m->firstCol;
  }
  CellAddr to = jump ? JumpEdge(from, dr, dc)
                     : CellAddr{dr ? StepRow(from.row, dr) : from.row, dc ? StepCol(from.col, dc) : from.col};
  if (extend) {
    Select(active_, to);
    return;
  }
  // Into a merged area: its first cell is the active one.
  if (const Range* m = s.MergeAt(to.row, to.col)) {
    ScrollIntoView(to);
    to = {m->firstRow, m->firstCol};
  }
  Select(to, to);
}

void WorkbookView::PageMove(int dir, bool horizontal, bool extend) {
  const double k = K();
  if (horizontal) {
    const double w = (cells_.right - cells_.left - FrozenWidthPx()) / k;
    const CellAddr from = extend ? end_ : active_;
    const uint32_t col = grid_.ColAt(static_cast<int64_t>(grid_.ColLeft(from.col) + dir * w));
    gridX_ += dir * w;
    ClampScroll();
    const CellAddr to{from.row, std::max(col, frozenCol1_)};
    extend ? Select(active_, to) : Select(to, to);
    return;
  }
  const double h = (cells_.bottom - cells_.top - FrozenHeightPx()) / k;
  const CellAddr from = extend ? end_ : active_;
  const uint32_t row = grid_.RowAt(static_cast<int64_t>(grid_.RowTop(from.row) + dir * h));
  gridY_ += dir * h;
  ClampScroll();
  const CellAddr to{std::max(row, frozenRow1_), from.col};
  extend ? Select(active_, to) : Select(to, to);
}

bool WorkbookView::OnKeyDown(UINT vk, UINT mods) {
  if (!Ready()) return false;
  if (preview_) {
    if (vk == VK_ESCAPE) {
      TogglePreview();
      return true;
    }
    return NavigationKey(vk, mods);
  }
  if (editor_.Active()) return EditKey(vk, mods);
  const bool ctrl = (mods & kModCtrl) != 0, shift = (mods & kModShift) != 0, alt = (mods & kModAlt) != 0;
  // Enter after Tabs returns to the Tabs' first column; any other key forgets it.
  if (vk != VK_TAB && vk != VK_RETURN && vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU) tabStartCol_ = UINT32_MAX;
  // Ctrl+; and Ctrl+Shift+; start an entry with the date or the time.
  if (ctrl && !alt && vk == SemicolonKey() && !ChartSheet()) {
    InsertNow(shift);
    return true;
  }
  // In a right-to-left sheet column A is at the right: the arrows go where they point.
  const int left = rtl_ ? 1 : -1;
  switch (vk) {
    case VK_LEFT: MoveActive(0, left, shift, ctrl); return true;
    case VK_RIGHT: MoveActive(0, -left, shift, ctrl); return true;
    case VK_UP: MoveActive(-1, 0, shift, ctrl); return true;
    case VK_DOWN: MoveActive(1, 0, shift, ctrl); return true;
    case VK_PRIOR:
    case VK_NEXT: PageMove(vk == VK_NEXT ? 1 : -1, alt, shift); return true;
    case VK_TAB:
      if (ctrl || alt) return false;
      if (!shift && tabStartCol_ == UINT32_MAX) tabStartCol_ = active_.col;
      MoveActive(0, shift ? -1 : 1, false, false);
      return true;
    case VK_RETURN:
      if (ctrl || alt) return false;
      if (shift) tabStartCol_ = UINT32_MAX;
      MoveAfterEntry(shift ? -1 : 1, 0);
      return true;
    case VK_F2:
      if (ctrl || shift || alt) return false;
      BeginEdit(CellEditor::Mode::Edit, FormulaBarText());
      return true;
    case VK_HOME: {
      const CellAddr to = ctrl ? CellAddr{frozenRow1_, frozenCol1_} : CellAddr{(shift ? end_ : active_).row, frozenCol1_};
      shift ? Select(active_, to) : Select(to, to);
      return true;
    }
    case VK_END:
      if (ctrl) {
        const auto used = Current().UsedRange();
        const CellAddr to = used ? CellAddr{used->lastRow, used->lastCol} : CellAddr{0, 0};
        shift ? Select(active_, to) : Select(to, to);
        return true;
      }
      return false;
    case VK_SPACE:
      if (shift && !ctrl) {
        Select({active_.row, 0}, {end_.row, kMaxCols - 1}, false);
        return true;
      }
      if (ctrl && !shift) {
        Select({0, active_.col}, {kMaxRows - 1, end_.col}, false);
        return true;
      }
      if (ctrl && shift) {
        Select({0, 0}, {kMaxRows - 1, kMaxCols - 1}, false);
        return true;
      }
      return false;
    case VK_BACK:
      if (ctrl) {
        ScrollIntoView(active_);
        return true;
      }
      // Backspace empties the cell and starts typing in it, as in Excel.
      if (!shift && !alt && !ChartSheet()) BeginEdit(CellEditor::Mode::Enter, L"");
      return true;
    case VK_DELETE:
      // Delete clears the selection's contents; formats stay.
      if (ctrl || alt || ChartSheet()) return false;
      if (std::u16string why; !Ed().CanEdit(&why)) host_.ViewMessage(BannerKind::Info, std::wstring(why.begin(), why.end()));
      else EditRefused(Ed().ClearContents(static_cast<int>(sheet_), Selected()));
      return true;
    default: return false;
  }
}

// ---------------------------------------------------------------------------
// Mouse

void WorkbookView::OnWheel(int delta, bool horizontal, UINT mods, int x, int y) {
  if (!Ready()) return;
  if (preview_) {
    PagedView::OnWheel(delta, horizontal, mods, x, y);
    return;
  }
  if ((mods & kModCtrl) && !horizontal) {
    SetZoom100(zoom_ * std::pow(1.0015, delta));
    return;
  }
  // A notch is three rows, as in Excel; touchpads scroll in proportion.
  const double notches = delta / static_cast<double>(WHEEL_DELTA);
  if (horizontal || (mods & kModShift)) {
    const double dx = (horizontal ? notches : -notches) * 3 * grid_.ColWidth(std::max(active_.col, frozenCol1_));
    ScrollTo(gridX_ + (rtl_ ? -dx : dx), gridY_);
  } else {
    ScrollTo(gridX_, gridY_ - notches * 3 * std::max(1, grid_.DefaultRowPx()));
  }
}

void WorkbookView::OnMouseDown(int x, int y, int button, UINT mods, bool doubleClick) {
  if (!Ready()) return;
  if (preview_) {
    if (ScrollbarMouseDown(x, y, button)) return;
    BeginPress(x, y);
    drag_ = Drag::Pan;
    return;
  }
  const float fx = static_cast<float>(x), fy = static_cast<float>(y);
  // The formula bar's text and the box over the cell place the caret; any other click ends the
  // entry being typed (in point mode it will insert a reference instead).
  if (button == 0 && !ChartSheet() && EditMouseDown(fx, fy, mods, doubleClick)) return;
  if (RefGrab g; button == 0 && RefHit(x, y, g)) {
    refGrab_ = g;
    mouse_ = Mouse::RefDrag;
    return;
  }
  if (button == 0 && gfx::Contains(cells_, fx, fy) && PointClick(x, y)) return;
  if (editor_.Active() && !CommitEdit(false)) return;
  tabStartCol_ = UINT32_MAX;
  // Double-clicking a cell edits it there (Edit mode).
  if (doubleClick && button == 0 && gfx::Contains(cells_, fx, fy) && !ChartSheet()) {
    if (auto cell = CellAt(x, y)) {
      Select(*cell, *cell);
      BeginEdit(CellEditor::Mode::Edit, FormulaBarText());
      return;
    }
  }
  const bool shift = (mods & kModShift) != 0;
  pendingLink_ = false;
  // Sheet tabs.
  if (gfx::Contains(tabs_, fx, fy)) {
    const float u = ui_;
    if (fx < tabs_.left + 30 * u) {
      Execute(CmdPrevSheet);
    } else if (fx < tabs_.left + 60 * u) {
      Execute(CmdNextSheet);
    } else {
      for (const TabRect& t : TabRects())
        if (gfx::Contains(t.rc, fx, fy)) ShowSheet(t.sheet);
    }
    return;
  }
  if (gfx::Contains(formulaBar_, fx, fy)) return;
  // Outline buttons.
  for (const OutlineButton& b : OutlineButtons()) {
    if (!gfx::Contains(b.rc, fx, fy)) continue;
    if (b.level > 0) ShowLevel(b.rows, b.level);
    else ToggleGroup(b);
    return;
  }
  // Scrollbars.
  Bar v, h;
  Bars(v, h);
  if (v.shown && gfx::Contains(v.track, fx, fy)) {
    if (gfx::Contains(v.thumb, fx, fy)) {
      mouse_ = Mouse::ThumbV;
      thumbGrab_ = fy - v.thumb.top;
    } else {
      PageMove(fy < v.thumb.top ? -1 : 1, false, false);
    }
    return;
  }
  if (h.shown && gfx::Contains(h.track, fx, fy)) {
    if (gfx::Contains(h.thumb, fx, fy)) {
      mouse_ = Mouse::ThumbH;
      thumbGrab_ = fx - h.thumb.left;
    } else {
      PageMove((fx < h.thumb.left) != rtl_ ? -1 : 1, true, false);
    }
    return;
  }
  // Headers: whole columns and rows; the corner, everything.
  if (gfx::Contains(corner_, fx, fy)) {
    Select({0, 0}, {kMaxRows - 1, kMaxCols - 1}, false);
    return;
  }
  auto clamped = [&](float px, float py) {
    px = std::clamp(px, cells_.left, cells_.right - 1);
    py = std::clamp(py, cells_.top, cells_.bottom - 1);
    return CellAt(static_cast<int>(px), static_cast<int>(py));
  };
  if (gfx::Contains(colHeader_, fx, fy)) {
    if (auto cell = clamped(fx, cells_.top)) {
      if (shift) Select({0, active_.col}, {kMaxRows - 1, cell->col}, false);
      else Select({0, cell->col}, {kMaxRows - 1, cell->col}, false);
      active_.row = frozenRow1_ ? std::min(active_.row, kMaxRows - 1) : 0;
      if (button == 0) mouse_ = Mouse::ColHeaders;
    }
    return;
  }
  if (gfx::Contains(rowHeader_, fx, fy)) {
    if (auto cell = clamped(cells_.left, fy)) {
      if (shift) Select({active_.row, 0}, {cell->row, kMaxCols - 1}, false);
      else Select({cell->row, 0}, {cell->row, kMaxCols - 1}, false);
      if (button == 0) mouse_ = Mouse::RowHeaders;
    }
    return;
  }
  if (!gfx::Contains(cells_, fx, fy)) return;
  auto cell = CellAt(x, y);
  if (!cell) return;
  if (button != 0) {
    // A right click outside the selection selects the cell, as in Excel.
    if (!Selected().Contains(cell->row, cell->col)) Select(*cell, *cell, false);
    return;
  }
  if (shift) {
    Select(active_, *cell, false);
  } else {
    Select(*cell, *cell, false);
    pendingLink_ = !doubleClick && LinkAt(cell->row, cell->col) != nullptr;
  }
  mouse_ = Mouse::Cells;
}

void WorkbookView::OnMouseMove(int x, int y, UINT mods) {
  if (!Ready()) return;
  if (preview_) {
    if (!ScrollDragMove(x, y)) UpdateScrollbarHover(x, y);
    return;
  }
  const float fx = static_cast<float>(x), fy = static_cast<float>(y);
  const POINT was = hover_;
  hover_ = {x, y};
  int tab = -1;
  if (gfx::Contains(tabs_, fx, fy))
    for (const TabRect& t : TabRects())
      if (gfx::Contains(t.rc, fx, fy)) tab = static_cast<int>(t.sheet);
  if (tab != hoverTab_) {
    hoverTab_ = tab;
    host_.ViewInvalidate();
  }
  Bar v, h;
  Bars(v, h);
  auto hot = [&](POINT p) {
    return (v.shown && gfx::Contains(v.track, (float)p.x, (float)p.y)) || (h.shown && gfx::Contains(h.track, (float)p.x, (float)p.y));
  };
  if (hot(was) != hot(hover_)) host_.ViewInvalidate();
  // A note shows while the mouse is over its cell.
  if (mouse_ == Mouse::None) {
    std::optional<CellAddr> note;
    if (SheetObjects* o = Objects(); o && gfx::Contains(cells_, fx, fy) && !hot(hover_))
      if (auto cell = CellAt(x, y); cell && o->NoteAt(cell->row, cell->col)) note = cell;
    if (note != note_) {
      note_ = note;
      host_.ViewInvalidate();
    }
  }
  switch (mouse_) {
    case Mouse::EditText: EditMouseMove(fx, fy); return;
    case Mouse::Point: PointDrag(x, y); return;
    case Mouse::RefDrag: RefDrag(x, y); return;
    case Mouse::ThumbV: {
      const double range = Height(v.track) - Height(v.thumb);
      if (range > 0) ScrollTo(gridX_, v.min + (fy - thumbGrab_ - v.track.top) / range * (v.end - v.min - v.view));
      return;
    }
    case Mouse::ThumbH: {
      const double range = Width(h.track) - Width(h.thumb);
      double t = range > 0 ? (fx - thumbGrab_ - h.track.left) / range : 0;
      if (rtl_) t = 1 - t;
      if (range > 0) ScrollTo(h.min + t * (h.end - h.min - h.view), gridY_);
      return;
    }
    case Mouse::Cells:
    case Mouse::RowHeaders:
    case Mouse::ColHeaders: {
      pendingLink_ = false;
      // Past the edge, the sheet scrolls along.
      const double k = K();
      double sx = gridX_, sy = gridY_;
      if (fy > cells_.bottom) sy += (fy - cells_.bottom) / k;
      if (fy < cells_.top + FrozenHeightPx() && mouse_ != Mouse::ColHeaders && gridY_ > grid_.RowTop(frozenRow1_))
        sy -= std::max(1.0, (cells_.top + FrozenHeightPx() - fy) / k);
      const float rightEdge = rtl_ ? cells_.left : cells_.right;
      if ((rtl_ ? fx < rightEdge : fx > rightEdge)) sx += std::fabs(fx - rightEdge) / k;
      if (sx != gridX_ || sy != gridY_) ScrollTo(sx, sy);
      const float px = std::clamp(fx, cells_.left, cells_.right - 1), py = std::clamp(fy, cells_.top, cells_.bottom - 1);
      auto cell = CellAt(static_cast<int>(px), static_cast<int>(py));
      if (!cell) return;
      if (mouse_ == Mouse::Cells) Select(active_, *cell, false);
      else if (mouse_ == Mouse::ColHeaders) Select({0, active_.col}, {kMaxRows - 1, cell->col}, false);
      else Select({active_.row, 0}, {cell->row, kMaxCols - 1}, false);
      return;
    }
    default: break;
  }
  (void)mods;
}

void WorkbookView::OnMouseUp(int x, int y, int button, UINT mods) {
  (void)button;
  (void)mods;
  if (preview_) {
    drag_ = Drag::None;
    return;
  }
  const bool follow = pendingLink_ && mouse_ == Mouse::Cells;
  mouse_ = Mouse::None;
  pendingLink_ = false;
  if (follow) {
    auto cell = CellAt(x, y);
    if (cell && *cell == active_)
      if (const Hyperlink* link = LinkAt(cell->row, cell->col)) FollowLink(*link);
  }
  host_.ViewInvalidate();
}

void WorkbookView::OnMouseLeave() {
  hover_ = {-1, -1};
  note_.reset();
  hoverTab_ = -1;
  if (preview_) ScrollbarMouseLeave();
  host_.ViewInvalidate();
}

HCURSOR WorkbookView::CursorAt(int x, int y) {
  if (Ready() && !preview_ && !ChartSheet()) {
    const float fx = static_cast<float>(x), fy = static_cast<float>(y);
    EditBox b;
    if (gfx::Contains(FormulaBox(), fx, fy) || (EditGeometry(b) && gfx::Contains(b.rc, fx, fy)))
      return LoadCursorW(nullptr, IDC_IBEAM);
    if (RefGrab g; RefHit(x, y, g))
      return LoadCursorW(nullptr, !g.resize ? IDC_SIZEALL : g.nwse ? IDC_SIZENWSE : IDC_SIZENESW);
  }
  if (Ready() && !preview_ && gfx::Contains(cells_, static_cast<float>(x), static_cast<float>(y))) {
    Bar v, h;
    Bars(v, h);
    const float fx = static_cast<float>(x), fy = static_cast<float>(y);
    if (!(v.shown && gfx::Contains(v.track, fx, fy)) && !(h.shown && gfx::Contains(h.track, fx, fy))) {
      if (auto cell = CellAt(x, y); cell && LinkAt(cell->row, cell->col)) return LoadCursorW(nullptr, IDC_HAND);
      return LoadCursorW(nullptr, IDC_CROSS);
    }
  }
  return LoadCursorW(nullptr, IDC_ARROW);
}

std::wstring WorkbookView::TooltipAt(int x, int y) {
  if (!Ready() || preview_ || !gfx::Contains(cells_, static_cast<float>(x), static_cast<float>(y))) return {};
  auto cell = CellAt(x, y);
  if (!cell) return {};
  if (const Hyperlink* link = LinkAt(cell->row, cell->col)) {
    if (!link->tooltip.empty()) return Utf8ToWide(link->tooltip);
    std::string target = link->target;
    if (!link->location.empty()) target += (target.empty() ? "" : "#") + link->location;
    return Utf8ToWide(target) + L"\nClick to follow the link";
  }
  return {};
}

// ---------------------------------------------------------------------------
// Outline groups

void WorkbookView::BuildGroups() {
  rowGroups_.clear();
  colGroups_.clear();
  const Sheet& s = Current();
  // Runs of rows (columns) at each level and deeper are the groups of that level.
  auto runs = [](std::vector<std::pair<uint32_t, int>>& levels, std::vector<Group>& out) {
    int maxLevel = 0;
    for (const auto& [i, l] : levels) maxLevel = std::max(maxLevel, l);
    for (int level = 1; level <= std::min(maxLevel, 7); ++level) {
      bool open = false;
      Group g;
      for (const auto& [i, l] : levels) {
        if (l >= level && open && i == g.last + 1) {
          g.last = i;
        } else if (l >= level) {
          if (open) out.push_back(g);
          g = {level, i, i, false};
          open = true;
        }
      }
      if (open) out.push_back(g);
    }
  };
  std::vector<std::pair<uint32_t, int>> levels;
  s.ForEachRow(0, kMaxRows - 1, [&](uint32_t row, const RowInfo& info) {
    if (info.outlineLevel > 0) levels.push_back({row, info.outlineLevel});
    return true;
  });
  runs(levels, rowGroups_);
  levels.clear();
  for (const ColumnInfo& c : s.Columns())
    if (c.outlineLevel > 0)
      for (uint32_t i = c.first; i <= c.last && i < kMaxCols; ++i) levels.push_back({i, c.outlineLevel});
  runs(levels, colGroups_);
  // Collapsed in the file: every row (column) of it hidden.
  for (Group& g : rowGroups_) {
    g.collapsed = true;
    for (uint32_t r = g.first; r <= g.last && g.collapsed; ++r) g.collapsed = grid_.RowHidden(r);
  }
  for (Group& g : colGroups_) {
    g.collapsed = true;
    for (uint32_t c = g.first; c <= g.last && g.collapsed; ++c) g.collapsed = grid_.ColHidden(c);
  }
}

void WorkbookView::ApplyGroups(bool rows) {
  std::vector<Group>& groups = rows ? rowGroups_ : colGroups_;
  std::vector<Group*> order;
  for (Group& g : groups) order.push_back(&g);
  std::stable_sort(order.begin(), order.end(), [](const Group* a, const Group* b) { return a->level < b->level; });
  for (const Group* g : order)
    if (!g->collapsed) rows ? grid_.SetRowsHidden(g->first, g->last, false) : grid_.SetColsHidden(g->first, g->last, false);
  for (const Group* g : order)
    if (g->collapsed) rows ? grid_.SetRowsHidden(g->first, g->last, true) : grid_.SetColsHidden(g->first, g->last, true);
  if (!rows) text_->Clear();  // widths changed
  PlaceObjects();
  states_[sheet_].rowGroups = rowGroups_;
  states_[sheet_].colGroups = colGroups_;
  states_[sheet_].groupsKept = true;
  ClampScroll();
  host_.ViewInvalidate();
}

int WorkbookView::OutlineLevels(bool rows) const {
  const std::vector<Group>& groups = rows ? rowGroups_ : colGroups_;
  int levels = 0;
  for (const Group& g : groups) levels = std::max(levels, g.level);
  return Current().Views().empty() || Current().Views().front().showOutlineSymbols ? levels : 0;
}

std::vector<WorkbookView::OutlineButton> WorkbookView::OutlineButtons() const {
  std::vector<OutlineButton> out;
  const float u = ui_;
  const float step = 14 * u, box = 11 * u;
  const SheetProps& props = Current().Props();
  // Level buttons: 1, 2, ... in the gutters' corner rows.
  for (int rows = 0; rows < 2; ++rows) {
    const int levels = OutlineLevels(rows != 0);
    for (int level = 1; level <= levels + 1 && levels > 0; ++level) {
      OutlineButton b;
      b.rows = rows != 0;
      b.level = level;
      if (b.rows) {
        const float x = Mirror(rowGutter_.left + 2 * u + (level - 1) * step + (rtl_ ? step : 0));
        const float cy = (corner_.top + corner_.bottom) / 2;
        b.rc = {x + (rtl_ ? -step : 0) + 1.5f * u, cy - box / 2, x + (rtl_ ? 0 : step) - 1.5f * u, cy + box / 2};
      } else {
        const float cx = (corner_.left + corner_.right) / 2;
        const float y = colGutter_.top + 2 * u + (level - 1) * step;
        b.rc = {cx - box / 2, y + 1.5f * u, cx + box / 2, y + step - 1.5f * u};
      }
      out.push_back(b);
    }
  }
  // A + or − beside each group's summary row (column) that is on screen.
  const auto panes = Panes();
  for (const Group& g : rowGroups_) {
    const uint32_t at = props.summaryBelow ? g.last + 1 : (g.first ? g.first - 1 : 0);
    if (at >= kMaxRows || grid_.RowHidden(at)) continue;
    for (const Pane& p : panes) {
      if (p.rc.left != static_cast<LONG>(cells_.left) || at < p.row0 || at >= p.row1) continue;
      const float t = PaneY(p, at), b = PaneY(p, at + 1);
      if (b <= p.rc.top || t >= p.rc.bottom) continue;
      const float x = rowGutter_.left + 2 * u + (g.level - 1) * step;
      const float cy = (t + b) / 2;
      OutlineButton ob;
      ob.rows = true;
      ob.first = g.first;
      ob.last = g.last;
      ob.collapsed = g.collapsed;
      ob.rc = {x + 1.5f * u, cy - box / 2, x + step - 1.5f * u, cy + box / 2};
      ob.level = 0;
      ob.rc.left = Mirror(ob.rc.left);
      ob.rc.right = Mirror(ob.rc.right);
      if (ob.rc.left > ob.rc.right) std::swap(ob.rc.left, ob.rc.right);
      // (stores the group's level in `first`/`last` lookups)
      out.push_back(ob);
    }
  }
  for (const Group& g : colGroups_) {
    const uint32_t at = props.summaryRight ? g.last + 1 : (g.first ? g.first - 1 : 0);
    if (at >= kMaxCols || grid_.ColHidden(at)) continue;
    for (const Pane& p : panes) {
      if (p.rc.top != static_cast<LONG>(cells_.top) || at < p.col0 || at >= p.col1) continue;
      const D2D1_RECT_F r = CellRect(p, 0, at, 0, at);
      if (r.right <= cells_.left || r.left >= cells_.right) continue;
      const float y = colGutter_.top + 2 * u + (g.level - 1) * step;
      const float cx = (r.left + r.right) / 2;
      OutlineButton ob;
      ob.rows = false;
      ob.first = g.first;
      ob.last = g.last;
      ob.collapsed = g.collapsed;
      ob.rc = {cx - box / 2, y + 1.5f * u, cx + box / 2, y + step - 1.5f * u};
      out.push_back(ob);
    }
  }
  return out;
}

void WorkbookView::ToggleGroup(const OutlineButton& b) {
  std::vector<Group>& groups = b.rows ? rowGroups_ : colGroups_;
  for (Group& g : groups)
    if (g.first == b.first && g.last == b.last && g.collapsed == b.collapsed) {
      g.collapsed = !g.collapsed;
      break;
    }
  ApplyGroups(b.rows);
}

void WorkbookView::ShowLevel(bool rows, int level) {
  for (Group& g : rows ? rowGroups_ : colGroups_) g.collapsed = g.level >= level;
  ApplyGroups(rows);
}

void WorkbookView::DrawOutline(Canvas& c) {
  if (!OutlineLevels(true) && !OutlineLevels(false)) return;
  const Palette& p = theme::P();
  const float u = ui_;
  const D2D1_COLOR_F ink = p.dark ? p.textSecondary : Rgb(0x555555);
  if (OutlineLevels(true)) c.Fill(rowGutter_, p.chrome);
  if (OutlineLevels(false)) c.Fill(colGutter_, p.chrome);
  // Brackets along each group on screen.
  const float step = 14 * u;
  const double k = K();
  for (const Pane& pane : Panes()) {
    if (pane.rc.left == static_cast<LONG>(cells_.left)) {
      c.PushClip({rowGutter_.left, static_cast<float>(pane.rc.top), rowGutter_.right, static_cast<float>(pane.rc.bottom)});
      for (const Group& g : rowGroups_) {
        if (g.collapsed || g.last < pane.row0 || g.first >= pane.row1) continue;
        const float t = PaneY(pane, std::max(g.first, pane.row0)), b = PaneY(pane, g.last + 1);
        if (b < pane.rc.top || t > pane.rc.bottom) continue;
        const float x = Mirror(rowGutter_.left + 2 * u + (g.level - 1) * step + step / 2);
        c.Fill({x - 0.5f * u, t + 2 * u, x + 0.5f * u, b - 1}, ink);
        c.Fill({std::min(x, Mirror(x + (rtl_ ? -1 : 1) * 4 * u)), b - 1 - 1 * u, std::max(x, Mirror(x + (rtl_ ? -1 : 1) * 4 * u)), b - 1}, ink);
      }
      c.PopClip();
    }
    if (pane.rc.top == static_cast<LONG>(cells_.top)) {
      for (const Group& g : colGroups_) {
        if (g.collapsed || g.last < pane.col0 || g.first >= pane.col1) continue;
        const D2D1_RECT_F r = CellRect(pane, 0, std::max(g.first, pane.col0), 0, g.last);
        const float y = colGutter_.top + 2 * u + (g.level - 1) * step + step / 2;
        c.PushClip({std::max(r.left, cells_.left), colGutter_.top, std::min(r.right, cells_.right), colGutter_.bottom});
        c.Fill({r.left + 2 * u, y - 0.5f * u, r.right - 1, y + 0.5f * u}, ink);
        c.PopClip();
      }
    }
  }
  (void)k;
  for (const OutlineButton& b : OutlineButtons()) {
    c.Fill(b.rc, p.dark ? p.inputBg : Rgb(0xFFFFFF));
    c.Stroke(b.rc, ink, 1);
    if (b.level > 0) {
      c.Text(std::to_wstring(b.level), b.rc, ink, 9 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center);
    } else {
      const float cx = (b.rc.left + b.rc.right) / 2, cy = (b.rc.top + b.rc.bottom) / 2, r = 3 * u;
      c.Fill({cx - r, cy - 0.5f * u, cx + r, cy + 0.5f * u}, ink);
      if (b.collapsed) c.Fill({cx - 0.5f * u, cy - r, cx + 0.5f * u, cy + r}, ink);
    }
  }
}

}  // namespace fw::sheet::view
