// Filter (Ctrl+Shift+L, S2 plan "sort and filter", s2-format): buttons on the filtered range's
// header row open a side panel with the column's values to keep and a custom condition; the rows
// the criteria leave out are hidden through the edit API.

#include <algorithm>

#include "Commands.h"
#include "core/Strings.h"
#include "sheet/view/WorkbookFilter.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

using namespace gfx;

namespace fw::sheet::view {

WorkbookView::ViewFilter* WorkbookView::FilterOf() {
  if (!Ready()) return nullptr;
  const Sheet* s = &Current();
  if (auto it = filters_.find(s); it != filters_.end()) return &it->second;
  if (const std::optional<SheetFilter>& f = s->Filter()) return &(filters_[s] = ViewFilter{f->ref, {}});
  return nullptr;
}

void WorkbookView::ToggleFilter() {
  if (!Ready() || preview_ || ChartSheet()) return;
  if (ViewFilter* f = FilterOf()) {
    // Off: every row it hid shows again.
    const Range ref = f->ref;
    const bool any = !f->criteria.empty();
    filters_.erase(&Current());
    if (any && ref.lastRow > ref.firstRow)
      EditRefused(Ed().HideRows(static_cast<int>(sheet_), ref.firstRow + 1, ref.lastRow, false));
    if (filterPanel_ && host_.ViewShownPanel() == filterPanel_.get()) host_.ViewShowPanel(nullptr);
    host_.ViewInvalidate();
    return;
  }
  const Range sel = Selected();
  Range ref = sel.firstRow == sel.lastRow && sel.firstCol == sel.lastCol ? data::CurrentRegion(Current(), active_) : sel;
  if (ref.firstRow == ref.lastRow && ref.firstCol == ref.lastCol && Current().GetValue(ref.firstRow, ref.firstCol).type == ValueType::Empty) {
    host_.ViewMessage(BannerKind::Info, L"Select a cell in the table to filter, or the cells themselves.");
    return;
  }
  filters_[&Current()] = ViewFilter{ref, {}};
  host_.ViewInvalidate();
}

bool WorkbookView::ApplyFilter() {
  ViewFilter* f = FilterOf();
  if (!f || f->ref.lastRow <= f->ref.firstRow) return false;
  const std::vector<bool> keep = data::FilterRows(*book_, Current(), f->ref, f->criteria, text_->Locale());
  const int sheet = static_cast<int>(sheet_);
  edit::EditGroup group(Ed(), u"Filter");
  edit::Result r;
  // Runs of rows whose hidden state changes, each in one call.
  const uint32_t first = f->ref.firstRow + 1;
  for (size_t i = 0; r && i < keep.size();) {
    const bool hide = !keep[i];
    size_t j = i;
    while (j < keep.size() && keep[j] == keep[i]) ++j;
    bool differs = false;
    for (size_t k = i; k < j && !differs; ++k) differs = Current().Row(first + static_cast<uint32_t>(k)).hidden != hide;
    if (differs) r = Ed().HideRows(sheet, first + static_cast<uint32_t>(i), first + static_cast<uint32_t>(j - 1), hide);
    i = j;
  }
  if (EditRefused(r)) return false;
  group.done = true;
  host_.ViewInvalidate();
  return true;
}

bool WorkbookView::FilterButtonAt(float x, float y, uint32_t* column, D2D1_RECT_F* rc) {
  ViewFilter* f = FilterOf();
  if (!f || preview_) return false;
  for (const Pane& p : Panes()) {
    if (!gfx::Contains(RectF(p.rc), x, y) && rc == nullptr) continue;
    for (uint32_t col = f->ref.firstCol; col <= f->ref.lastCol; ++col) {
      const D2D1_RECT_F cell = CellRect(p, f->ref.firstRow, col, f->ref.firstRow, col);
      const float s = std::min(cell.bottom - cell.top - 2, std::round(16 * ui_));
      if (s < 6) continue;
      const D2D1_RECT_F b = RectF(cell.right - s - 1, cell.bottom - s - 1, cell.right - 1, cell.bottom - 1);
      if (gfx::Contains(b, x, y) && gfx::Contains(RectF(p.rc), x, y)) {
        if (column) *column = col - f->ref.firstCol;
        if (rc) *rc = b;
        return true;
      }
    }
  }
  return false;
}

void WorkbookView::DrawFilterButtons(Canvas& c) {
  ViewFilter* f = FilterOf();
  if (!f || preview_) return;
  const Palette& p = theme::P();
  for (const Pane& pane : Panes()) {
    c.PushClip(RectF(pane.rc));
    for (uint32_t col = f->ref.firstCol; col <= f->ref.lastCol; ++col) {
      const D2D1_RECT_F cell = CellRect(pane, f->ref.firstRow, col, f->ref.firstRow, col);
      const float s = std::min(cell.bottom - cell.top - 2, std::round(16 * ui_));
      if (s < 6 || cell.right < pane.rc.left || cell.left > pane.rc.right) continue;
      const D2D1_RECT_F b = RectF(cell.right - s - 1, cell.bottom - s - 1, cell.right - 1, cell.bottom - 1);
      const bool active = std::any_of(f->criteria.begin(), f->criteria.end(),
                                      [&](const data::FilterCriteria& x) { return x.column == col - f->ref.firstCol; });
      c.Fill(b, Rgb(0xF3F3F3));
      c.Stroke(Inflate(b, -0.5f, -0.5f), Rgb(0xA0A0A0), 1);
      c.Icon(icon::ChevronDown, b, active ? p.accent : Rgb(0x404040), std::round(s * 0.6f));
    }
    c.PopClip();
  }
}

void WorkbookView::ShowFilterPanel(uint32_t column) {
  if (!filterPanel_) filterPanel_ = std::make_unique<FilterPanel>(*this);
  filterPanel_->Open(column);
  host_.ViewShowPanel(filterPanel_.get());
}

bool WorkbookView::DataCommandState(int cmd, CommandState& s) {
  const bool cells = Ready() && !preview_ && !ChartSheet();
  switch (cmd) {
    case CmdFilter:
      s.label = L"Filter";
      s.shortcut = L"Ctrl+Shift+L";
      s.enabled = cells;
      s.checked = cells && FilterOf() != nullptr;
      return true;
    case CmdDataBox: s.label = L"Sort and filter"; s.enabled = cells; return true;
  }
  return false;
}

bool WorkbookView::DataCommand(int cmd) {
  switch (cmd) {
    case CmdFilter: ToggleFilter(); return true;
  }
  return false;
}

}  // namespace fw::sheet::view
