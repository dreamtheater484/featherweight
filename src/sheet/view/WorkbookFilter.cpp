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

bool WorkbookView::ReadFilter(int table, ViewFilter& out) {
  if (!Ready()) return false;
  const Sheet& s = Current();
  const std::vector<FilterColumn>* columns = nullptr;
  if (table < 0) {
    if (!s.Filter()) return false;
    out = ViewFilter{s.Filter()->ref, {}, {}};
    columns = &s.Filter()->columns;
  } else {
    if (static_cast<size_t>(table) >= s.Tables().size()) return false;
    const Table& t = s.Tables()[static_cast<size_t>(table)];
    if (!t.autoFilter || !t.headerRows) return false;
    Range ref = t.ref;
    if (t.totalsRows && ref.lastRow > ref.firstRow) --ref.lastRow;
    out = ViewFilter{ref, {}, {}};
    columns = &t.criteria;
  }
  for (const FilterColumn& col : *columns) {
    data::FilterCriteria c;
    if (data::FromFilterColumn(col, c)) out.criteria.push_back(std::move(c));
    else out.other.push_back(col);
  }
  return true;
}

std::vector<int> WorkbookView::FilterAreas() {
  std::vector<int> out;
  if (!Ready()) return out;
  if (Current().Filter()) out.push_back(-1);
  const std::vector<Table>& tables = Current().Tables();
  for (size_t i = 0; i < tables.size(); ++i)
    if (tables[i].autoFilter && tables[i].headerRows) out.push_back(static_cast<int>(i));
  return out;
}

WorkbookView::ViewFilter* WorkbookView::FilterOf() {
  return ReadFilter(filterTable_, filter_) ? &filter_ : nullptr;
}

bool WorkbookView::SetTableButtons(size_t index, bool on) {
  if (!Ready() || index >= Current().Tables().size()) return false;
  const int sheet = static_cast<int>(sheet_);
  Table t = Current().Tables()[index];
  if (t.autoFilter == on) return true;
  edit::EditGroup group(Ed(), u"Filter");
  const std::vector<HeldSize> held = HeldSizes();  // pictures keep their size
  edit::Result r;
  if (!on && !t.criteria.empty()) {
    r = Ed().SetTableFilter(sheet, index, SheetFilter{});
    const Range data = t.Data();
    if (r && data.firstRow <= data.lastRow) r = Ed().HideRows(sheet, data.firstRow, data.lastRow, false);
    if (r) t = Current().Tables()[index];
  }
  t.autoFilter = on;
  if (r) r = Ed().SetTable(sheet, index, t);
  if (r) r = KeepSizes(held);
  if (EditRefused(r)) return false;
  group.done = true;
  if (!on && filterTable_ == static_cast<int>(index) && filterPanel_ && host_.ViewShownPanel() == filterPanel_.get())
    host_.ViewShowPanel(nullptr);
  host_.ViewInvalidate();
  return true;
}

void WorkbookView::ToggleFilter() {
  if (!Ready() || preview_ || ChartSheet()) return;
  const int sheet = static_cast<int>(sheet_);
  // In a table: its own buttons, as Excel does.
  if (const int i = TableIndexAt(active_); i >= 0) {
    SetTableButtons(static_cast<size_t>(i), !Current().Tables()[static_cast<size_t>(i)].autoFilter);
    return;
  }
  filterTable_ = -1;
  if (ViewFilter* f = FilterOf()) {
    // Off: every row it hid shows again.
    const Range ref = f->ref;
    const bool any = !f->criteria.empty() || !f->other.empty();
    edit::EditGroup group(Ed(), u"Remove Filter");
    const std::vector<HeldSize> held = HeldSizes();  // s3-data: pictures keep their size
    edit::Result r = Ed().SetFilter(sheet, std::nullopt);
    if (r && any && ref.lastRow > ref.firstRow) r = Ed().HideRows(sheet, ref.firstRow + 1, ref.lastRow, false);
    if (r) r = KeepSizes(held);
    if (EditRefused(r)) return;
    group.done = true;
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
  SheetFilter on;
  on.ref = ref;
  if (EditRefused(Ed().SetFilter(sheet, on))) return;  // a table's range has its own filter
  host_.ViewInvalidate();
}

bool WorkbookView::ApplyFilter() {
  // filter_ as the panel left it (FilterOf would read the model's again).
  if (!Ready()) return false;
  const Sheet& s = Current();
  const int table = filterTable_;
  const bool ofTable = table >= 0 && static_cast<size_t>(table) < s.Tables().size();
  if (table >= 0 && !ofTable) return false;
  if (!ofTable && !s.Filter()) return false;
  const std::vector<FilterColumn>& had = ofTable ? s.Tables()[static_cast<size_t>(table)].criteria : s.Filter()->columns;
  ViewFilter* f = &filter_;
  if (f->ref.lastRow <= f->ref.firstRow) return false;
  std::vector<bool> keep = data::FilterRows(*book_, s, f->ref, f->criteria, text_->Locale());
  if (!f->other.empty()) {
    // Rows a kept column from the file hides (a color, a date filter) stay hidden: those the
    // model's criteria let through and that are hidden all the same.
    std::vector<data::FilterCriteria> before;
    for (const FilterColumn& col : had) {
      data::FilterCriteria c;
      if (data::FromFilterColumn(col, c)) before.push_back(std::move(c));
    }
    const std::vector<bool> passed = data::FilterRows(*book_, s, f->ref, before, text_->Locale());
    for (size_t i = 0; i < keep.size(); ++i)
      if (passed[i] && s.Row(f->ref.firstRow + 1 + static_cast<uint32_t>(i)).hidden) keep[i] = false;
  }
  SheetFilter model;
  model.ref = f->ref;
  for (const data::FilterCriteria& c : f->criteria) model.columns.push_back(data::ToFilterColumn(s, f->ref, c));
  model.columns.insert(model.columns.end(), f->other.begin(), f->other.end());
  std::sort(model.columns.begin(), model.columns.end(),
            [](const FilterColumn& a, const FilterColumn& b) { return a.colId < b.colId; });
  const int sheet = static_cast<int>(sheet_);
  edit::EditGroup group(Ed(), u"Filter");
  const std::vector<HeldSize> held = HeldSizes();  // s3-data: pictures keep their size
  edit::Result r = ofTable ? Ed().SetTableFilter(sheet, static_cast<size_t>(table), model) : Ed().SetFilter(sheet, model);
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
  if (r) r = KeepSizes(held);
  if (EditRefused(r)) return false;
  group.done = true;
  host_.ViewInvalidate();
  return true;
}

bool WorkbookView::FilterButtonAt(float x, float y, uint32_t* column, D2D1_RECT_F* rc) {
  if (preview_) return false;
  ViewFilter area;
  for (const int which : FilterAreas()) {
    if (!ReadFilter(which, area)) continue;
    const ViewFilter* f = &area;
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
          filterTable_ = which;  // the panel's filter
          return true;
        }
      }
    }
  }
  return false;
}

void WorkbookView::DrawFilterButtons(Canvas& c) {
  if (preview_) return;
  const Palette& p = theme::P();
  ViewFilter area;
  for (const int which : FilterAreas()) {
  if (!ReadFilter(which, area)) continue;
  const ViewFilter* f = &area;
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
}

void WorkbookView::ShowFilterPanel(uint32_t column) {
  if (!filterPanel_) filterPanel_ = std::make_unique<FilterPanel>(*this);
  filterPanel_->Open(column);
  host_.ViewShowPanel(filterPanel_.get());
}

// ---------------------------------------------------------------------------
// Sort and remove duplicates

Range WorkbookView::DataRange() {
  const Range sel = Selected();
  if (sel.firstRow != sel.lastRow || sel.firstCol != sel.lastCol) {
    // Whole columns or rows: as far as the sheet is filled.
    Range r = sel;
    if (const std::optional<Range> used = Current().UsedRange()) {
      r.lastRow = std::min(r.lastRow, std::max(used->lastRow, r.firstRow));
      r.lastCol = std::min(r.lastCol, std::max(used->lastCol, r.firstCol));
    }
    return r;
  }
  return data::CurrentRegion(Current(), active_);
}

bool WorkbookView::SortRange(const Range& range, bool header, const std::vector<data::SortKey>& keys) {
  Range rows = range;
  if (header) ++rows.firstRow;
  if (!Ready() || rows.firstRow >= rows.lastRow || keys.empty()) return false;
  const Sheet& s = Current();
  const std::vector<uint32_t> sorted = data::SortOrder(s, rows, keys);
  // SortRows' order: the row at rows.firstRow + order[i] goes to rows.firstRow + i. Under a filter,
  // the rows it hides stay where they are and the shown rows are sorted among themselves (Excel).
  std::vector<uint32_t> order(sorted.size());
  // Under a filter (the sheet's or a table's) whose criteria hide rows in the range.
  bool filtered = false;
  ViewFilter area;
  for (const int which : FilterAreas())
    filtered |= ReadFilter(which, area) && !area.criteria.empty() && area.ref.firstRow < rows.lastRow &&
                area.ref.lastRow >= rows.firstRow;
  if (filtered) {
    std::vector<uint32_t> shown;
    for (const uint32_t row : sorted)
      if (!s.Row(row).hidden) shown.push_back(row);
    size_t k = 0;
    for (size_t i = 0; i < order.size(); ++i) {
      const uint32_t at = rows.firstRow + static_cast<uint32_t>(i);
      order[i] = (s.Row(at).hidden ? at : shown[k++]) - rows.firstRow;
    }
  } else {
    for (size_t i = 0; i < order.size(); ++i) order[i] = sorted[i] - rows.firstRow;
  }
  // The keys are kept as Excel's sortState (Data > Reapply); a CSV file leaves it out.
  std::vector<edit::Editor::SortKey> sortKeys;
  for (const data::SortKey& k : keys) sortKeys.push_back({k.index, k.descending});
  if (EditRefused(Ed().SortRows(static_cast<int>(sheet_), rows, order, sortKeys))) return false;
  host_.ViewInvalidate();
  return true;
}

bool WorkbookView::RemoveDuplicates(const Range& range, bool header, const std::vector<uint32_t>& columns) {
  if (!Ready()) return false;
  const std::vector<bool> dup = data::DuplicateRows(*book_, Current(), range, columns, header, text_->Locale());
  const uint32_t first = range.firstRow + (header ? 1 : 0);
  // The unique rows move up as a sort moves them (measured: formulas copied, relative references
  // following, absolute ones and references from elsewhere staying), and the rows left over clear.
  std::vector<uint32_t> order;
  for (size_t i = 0; i < dup.size(); ++i)
    if (!dup[i]) order.push_back(static_cast<uint32_t>(i));
  const size_t unique = order.size(), removed = dup.size() - unique;
  if (removed) {
    for (size_t i = 0; i < dup.size(); ++i)
      if (dup[i]) order.push_back(static_cast<uint32_t>(i));
    const int sheet = static_cast<int>(sheet_);
    edit::EditGroup group(Ed(), u"Remove Duplicates");
    edit::Result r = Ed().SortRows(sheet, Range{first, range.firstCol, range.lastRow, range.lastCol}, order);
    if (r) r = Ed().ClearAll(sheet, Range{first + static_cast<uint32_t>(unique), range.firstCol, range.lastRow, range.lastCol});
    if (EditRefused(r)) return false;
    group.done = true;
    host_.ViewInvalidate();
  }
  // Excel's report, in its words.
  host_.ViewMessage(BannerKind::Info, removed ? std::to_wstring(removed) + L" duplicate values found and removed; " +
                                                    std::to_wstring(unique) + L" unique values remain."
                                              : L"No duplicate values found.");
  return true;
}

void WorkbookView::ShowDataPanel(bool sort) {
  if (!Ready() || preview_ || ChartSheet()) return;
  if (!dataPanel_) dataPanel_ = std::make_unique<DataPanel>(*this);
  dataPanel_->Open(sort);
  host_.ViewShowPanel(dataPanel_.get());
}

bool WorkbookView::DataCommandState(int cmd, CommandState& s) {
  const bool cells = Ready() && !preview_ && !ChartSheet();
  switch (cmd) {
    case CmdFilter:
      s.label = L"Filter";
      s.shortcut = L"Ctrl+Shift+L";
      s.enabled = cells;
      if (const int i = cells ? TableIndexAt(active_) : -1; i >= 0) s.checked = Current().Tables()[static_cast<size_t>(i)].autoFilter;
      else s.checked = cells && Current().Filter().has_value();
      return true;
    case CmdDataBox: s.label = L"Sort and filter"; s.enabled = cells; return true;
    case CmdSortAscending: s.label = L"Sort A to Z"; s.enabled = cells; return true;
    case CmdSortDescending: s.label = L"Sort Z to A"; s.enabled = cells; return true;
    case CmdRemoveDuplicates: s.label = L"Remove duplicates"; s.enabled = cells; return true;
  }
  return false;
}

bool WorkbookView::DataCommand(int cmd) {
  CommandState s;
  if (!DataCommandState(cmd, s)) return false;
  if (!s.enabled) return true;
  switch (cmd) {
    case CmdFilter: ToggleFilter(); break;
    case CmdSortAscending:
    case CmdSortDescending: {
      // By the active cell's column, with Excel's header guess.
      const Range r = DataRange();
      SortRange(r, data::GuessHeader(*book_, Current(), r),
                {{std::clamp(active_.col, r.firstCol, r.lastCol), cmd == CmdSortDescending}});
      break;
    }
    case CmdRemoveDuplicates: ShowDataPanel(false); break;
    case CmdDataBox: ShowDataPanel(true); break;
  }
  return true;
}

}  // namespace fw::sheet::view
