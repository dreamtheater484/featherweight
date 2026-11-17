// The workbook tab's commands on rows, columns, cells and sheets (s2-plan "Rows, columns and
// cells", "Sheets"), its context menus, and keeping the view in step when the edit API changes
// the sheets or their layout. Every change goes through the edit API.
#include <algorithm>
#include <iterator>

#include "Commands.h"
#include "core/Strings.h"
#include "sheet/view/WorkbookPanels.h"
#include "sheet/view/WorkbookView.h"
#include "ui/Menu.h"

namespace fw::sheet::view {

namespace {

// Excel's standard colors, as its Tab Color menu offers them.
struct TabColorName {
  uint32_t rgb;
  const wchar_t* name;
};
constexpr TabColorName kTabColors[] = {
    {0xC00000, L"Dark red"},    {0xFF0000, L"Red"},   {0xFFC000, L"Orange"}, {0xFFFF00, L"Yellow"},
    {0x92D050, L"Light green"}, {0x00B050, L"Green"}, {0x00B0F0, L"Light blue"}, {0x0070C0, L"Blue"},
    {0x002060, L"Dark blue"},   {0x7030A0, L"Purple"},
};

}  // namespace

bool WorkbookView::WholeRows() const {
  const Range sel = Selected();
  return sel.firstCol == 0 && sel.lastCol == kMaxCols - 1 && !(sel.firstRow == 0 && sel.lastRow == kMaxRows - 1);
}

bool WorkbookView::WholeColumns() const {
  const Range sel = Selected();
  return sel.firstRow == 0 && sel.lastRow == kMaxRows - 1 && !(sel.firstCol == 0 && sel.lastCol == kMaxCols - 1);
}

int WorkbookView::ShownNeighbour(int dir) const {
  for (int i = static_cast<int>(sheet_) + dir; i >= 0 && i < static_cast<int>(book_->SheetCount()); i += dir)
    if (SheetShown(static_cast<size_t>(i))) return i;
  return -1;
}

bool WorkbookView::GridCommandState(int cmd, CommandState& s) {
  const bool grid = Ready() && !preview_;
  const bool cells = grid && !ChartSheet();
  size_t shown = 0;
  if (Ready())
    for (size_t i = 0; i < book_->SheetCount(); ++i) shown += SheetShown(i);
  if (cmd >= CmdTabColor && cmd <= CmdTabColor + static_cast<int>(std::size(kTabColors))) {
    s.enabled = grid;
    return true;
  }
  if (cmd >= CmdUnhideSheet && cmd <= CmdUnhideSheetLast) {
    const size_t i = static_cast<size_t>(cmd - CmdUnhideSheet);
    s.enabled = Ready() && i < book_->SheetCount() && book_->GetSheet(i).State() == SheetState::Hidden;
    return true;
  }
  switch (cmd) {
    case CmdInsertCells: s.label = L"Insert cells"; s.shortcut = L"Ctrl++"; s.enabled = cells; return true;
    case CmdDeleteCells: s.label = L"Delete cells"; s.shortcut = L"Ctrl+-"; s.enabled = cells; return true;
    case CmdClearContents: s.label = L"Clear contents"; s.shortcut = L"Delete"; s.enabled = cells; return true;
    case CmdRowHeight: s.label = L"Row height"; s.enabled = cells; return true;
    case CmdColumnWidth: s.label = L"Column width"; s.enabled = cells; return true;
    case CmdHide: s.label = L"Hide rows or columns"; s.enabled = cells && (WholeRows() || WholeColumns()); return true;
    case CmdUnhide: s.label = L"Unhide rows or columns"; s.enabled = cells && (WholeRows() || WholeColumns()); return true;
    case CmdInsertSheet: s.label = L"Insert sheet"; s.shortcut = L"Shift+F11"; s.enabled = grid; return true;
    case CmdDeleteSheet: s.label = L"Delete sheet"; s.enabled = grid && shown > 1; return true;
    case CmdRenameSheet: s.label = L"Rename sheet"; s.enabled = grid; return true;
    case CmdHideSheet: s.label = L"Hide sheet"; s.enabled = grid && shown > 1; return true;
    case CmdMoveSheetLeft: s.label = L"Move sheet left"; s.enabled = grid && ShownNeighbour(-1) >= 0; return true;
    case CmdMoveSheetRight: s.label = L"Move sheet right"; s.enabled = grid && ShownNeighbour(1) >= 0; return true;
    case CmdGoTo: s.label = L"Go to"; s.shortcut = L"Ctrl+G"; s.enabled = cells; return true;
    default: return false;
  }
}

bool WorkbookView::GridCommand(int cmd) {
  CommandState state;
  if (!GridCommandState(cmd, state)) return false;
  if (!state.enabled) return true;
  // A command ends the entry being typed first (unless it can't be written).
  if (editor_.Active() && !CommitEdit(false)) return true;
  const int sheet = static_cast<int>(sheet_);
  const Range sel = Selected();
  const bool rows = WholeRows(), cols = WholeColumns();
  if (cmd >= CmdTabColor && cmd <= CmdTabColor + static_cast<int>(std::size(kTabColors))) {
    Color color;
    if (cmd > CmdTabColor) {
      color.kind = Color::Kind::Rgb;
      color.value = 0xFF000000u | kTabColors[cmd - CmdTabColor - 1].rgb;
    }
    EditRefused(Ed().SetTabColor(sheet, color));
    return true;
  }
  if (cmd >= CmdUnhideSheet && cmd <= CmdUnhideSheetLast) {
    const size_t i = static_cast<size_t>(cmd - CmdUnhideSheet);
    if (!EditRefused(Ed().SetSheetState(static_cast<int>(i), SheetState::Visible))) ShowSheet(i);
    return true;
  }
  switch (cmd) {
    case CmdInsertCells:
      // Excel asks which way the cells go; Featherweight shifts them down (rows and columns
      // selected whole insert rows and columns).
      EditRefused(rows   ? Ed().InsertRows(sheet, sel.firstRow, sel.lastRow - sel.firstRow + 1)
                  : cols ? Ed().InsertColumns(sheet, sel.firstCol, sel.lastCol - sel.firstCol + 1)
                         : Ed().InsertCells(sheet, sel, edit::Editor::Direction::DownOrUp));
      return true;
    case CmdDeleteCells:
      EditRefused(rows   ? Ed().DeleteRows(sheet, sel.firstRow, sel.lastRow - sel.firstRow + 1)
                  : cols ? Ed().DeleteColumns(sheet, sel.firstCol, sel.lastCol - sel.firstCol + 1)
                         : Ed().DeleteCells(sheet, sel, edit::Editor::Direction::DownOrUp));
      return true;
    case CmdClearContents: EditRefused(Ed().ClearContents(sheet, sel)); return true;
    case CmdHide:
    case CmdUnhide:
      EditRefused(rows ? Ed().HideRows(sheet, sel.firstRow, sel.lastRow, cmd == CmdHide)
                       : Ed().HideColumns(sheet, sel.firstCol, sel.lastCol, cmd == CmdHide));
      return true;
    case CmdRowHeight:
    case CmdColumnWidth: ShowSizePanel(cmd == CmdRowHeight); return true;
    case CmdInsertSheet:
      // A new sheet before the active one, as Excel's Shift+F11 adds it, and shown.
      if (!EditRefused(Ed().AddSheet(sheet))) ShowSheet(static_cast<size_t>(sheet));
      return true;
    case CmdDeleteSheet: EditRefused(Ed().DeleteSheet(sheet)); return true;
    case CmdRenameSheet: BeginRename(sheet_); return true;
    case CmdHideSheet: EditRefused(Ed().SetSheetState(sheet, SheetState::Hidden)); return true;
    case CmdMoveSheetLeft:
    case CmdMoveSheetRight: {
      const int to = ShownNeighbour(cmd == CmdMoveSheetLeft ? -1 : 1);
      if (to >= 0) EditRefused(Ed().MoveSheet(sheet, to));
      return true;
    }
    case CmdGoTo: ShowGoTo(); return true;
    default: return false;
  }
}

void WorkbookView::FillContextMenu(HMENU m, int x, int y) {
  using menu::AppendItem;
  using menu::AppendSeparator;
  using menu::AppendSubmenu;
  const float fx = static_cast<float>(x), fy = static_cast<float>(y);
  auto item = [&](HMENU into, int cmd, const std::wstring& text) {
    CommandState s;
    GridCommandState(cmd, s);
    AppendItem(into, static_cast<UINT>(cmd), text, s.enabled);
  };
  if (!preview_ && gfx::Contains(tabs_, fx, fy)) {
    // The sheet's own commands, then every sheet to go to.
    item(m, CmdInsertSheet, L"Insert sheet\tShift+F11");
    item(m, CmdDeleteSheet, L"Delete");
    item(m, CmdRenameSheet, L"Rename");
    item(m, CmdMoveSheetLeft, L"Move left");
    item(m, CmdMoveSheetRight, L"Move right");
    HMENU colors = CreatePopupMenu();
    const Color current = Current().Props().tabColor;
    AppendItem(colors, CmdTabColor, L"No color", true, !current.IsSet(), true);
    for (size_t i = 0; i < std::size(kTabColors); ++i)
      AppendItem(colors, CmdTabColor + 1 + static_cast<UINT>(i), kTabColors[i].name, true,
                 current.kind == Color::Kind::Rgb && (current.value & 0xFFFFFF) == kTabColors[i].rgb, true);
    AppendSubmenu(m, colors, L"Tab color");
    item(m, CmdHideSheet, L"Hide");
    HMENU hidden = CreatePopupMenu();
    bool anyHidden = false;
    for (size_t i = 0; i < book_->SheetCount() && CmdUnhideSheet + static_cast<int>(i) <= CmdUnhideSheetLast; ++i)
      if (book_->GetSheet(i).State() == SheetState::Hidden) {
        AppendItem(hidden, CmdUnhideSheet + static_cast<UINT>(i), Utf8ToWide(book_->GetSheet(i).Name()));
        anyHidden = true;
      }
    AppendSubmenu(m, hidden, L"Unhide", anyHidden);
    AppendSeparator(m);
    for (size_t i = 0; i < book_->SheetCount() && CmdSheetPick + static_cast<int>(i) <= CmdSheetPickLast; ++i)
      if (SheetShown(i))
        AppendItem(m, CmdSheetPick + static_cast<UINT>(i), Utf8ToWide(book_->GetSheet(i).Name()), true, i == sheet_, true);
    return;
  }
  if (!preview_ && !ChartSheet()) {
    const bool rows = WholeRows(), cols = WholeColumns();
    AppendItem(m, CmdCut, L"Cut\tCtrl+X", false);
    AppendItem(m, CmdCopy, L"Copy\tCtrl+C");
    AppendItem(m, CmdPaste, L"Paste\tCtrl+V", false);
    AppendSeparator(m);
    item(m, CmdInsertCells, rows ? L"Insert rows\tCtrl++" : cols ? L"Insert columns\tCtrl++" : L"Insert cells\tCtrl++");
    item(m, CmdDeleteCells, rows ? L"Delete rows\tCtrl+-" : cols ? L"Delete columns\tCtrl+-" : L"Delete cells\tCtrl+-");
    item(m, CmdClearContents, L"Clear contents\tDelete");
    if (rows || cols) {
      AppendSeparator(m);
      item(m, rows ? CmdRowHeight : CmdColumnWidth, rows ? L"Row height…" : L"Column width…");
      item(m, CmdHide, L"Hide");
      item(m, CmdUnhide, L"Unhide");
    }
    AppendSeparator(m);
    AppendItem(m, CmdSelectAll, L"Select all\tCtrl+A");
    AppendSeparator(m);
  }
  AppendItem(m, CmdFind, L"Find…\tCtrl+F");
  AppendItem(m, CmdPrint, L"Print…\tCtrl+P");
  AppendItem(m, CmdPrintPreview, preview_ ? L"Close print preview\tEsc" : L"Print preview\tCtrl+F2");
}

// ---------------------------------------------------------------------------
// The view in step with the model

void WorkbookView::SheetsChanged() {
  // Sheets were added, deleted or moved: what the view keeps for each follows its sheet.
  const size_t n = book_->SheetCount();
  if (shown_ && sheet_ < states_.size()) {  // where the shown sheet is, kept as ShowSheet keeps it
    PerSheet& was = states_[sheet_];
    was.active = active_;
    was.end = end_;
    was.sx = gridX_;
    was.sy = gridY_;
    was.zoom = zoom_;
  }
  const Sheet* shownWas = sheet_ < sheetOrder_.size() ? sheetOrder_[sheet_] : nullptr;
  std::vector<PerSheet> states(n);
  std::vector<const Sheet*> order(n);
  size_t shown = SIZE_MAX;
  for (size_t i = 0; i < n; ++i) {
    order[i] = &book_->GetSheet(i);
    const auto it = std::find(sheetOrder_.begin(), sheetOrder_.end(), order[i]);
    const size_t j = static_cast<size_t>(it - sheetOrder_.begin());
    if (it != sheetOrder_.end() && j < states_.size()) states[i] = std::move(states_[j]);
    if (order[i] == shownWas) shown = i;
  }
  states_ = std::move(states);
  sheetOrder_ = std::move(order);
  // Pictures and charts, conditional formats and the engine know sheets by index: made again.
  objects_.clear();
  objects_.resize(n);
  rules_.clear();
  rules_.resize(n);
  rulesRead_.assign(n, false);
  calc_.reset();
  recalc_ = true;
  loadNext_ = 0;
  if (shown == SIZE_MAX) {  // the shown sheet was deleted: the next one, as in Excel
    shown = std::min(sheet_, n - 1);
    if (!SheetShown(shown)) shown = NextShownSheet(shown, 1);
  }
  shown_ = false;  // the state kept for the old index isn't this sheet's
  ShowSheet(shown);
}

void WorkbookView::Restructure() {
  // Row heights, hidden rows and columns, merges or moved cells on the shown sheet.
  Sheet& sheet = Current();
  text_->Clear();
  grid_.Build(sheet, book_->GetStyles(), *measure_);
  BuildGroups();
  PlaceObjects();
  totals_ = Totals{};
  LayoutParts();
  ClampScroll();
}

}  // namespace fw::sheet::view
