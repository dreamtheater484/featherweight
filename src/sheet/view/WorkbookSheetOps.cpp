// The workbook tab's commands on rows, columns, cells and sheets (s2-plan "Rows, columns and
// cells", "Sheets"), its context menus, and keeping the view in step when the edit API changes
// the sheets or their layout. Every change goes through the edit API.
#include <algorithm>
#include <cmath>
#include <iterator>

#include "Commands.h"
#include "core/Strings.h"
#include "sheet/view/WorkbookPanels.h"
#include "sheet/view/WorkbookView.h"
#include "ui/Menu.h"
#include "ui/Theme.h"

using namespace gfx;

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
  if (ReviewCommandState(cmd, s)) return true;
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
    case CmdFillDown: s.label = L"Fill down"; s.shortcut = L"Ctrl+D"; s.enabled = cells; return true;
    case CmdFillRight: s.label = L"Fill right"; s.shortcut = L"Ctrl+R"; s.enabled = cells; return true;
    case CmdCut: s.label = L"Cut"; s.shortcut = L"Ctrl+X"; s.enabled = cells; return true;
    case CmdPaste: s.label = L"Paste"; s.shortcut = L"Ctrl+V"; s.enabled = cells; return true;
    case CmdPasteValues: s.label = L"Paste values"; s.shortcut = L"Ctrl+Shift+V"; s.enabled = cells; return true;
    case CmdPasteFormulas: s.label = L"Paste formulas"; s.enabled = cells; return true;
    case CmdPasteFormats: s.label = L"Paste formats"; s.enabled = cells; return true;
    case CmdPasteTranspose: s.label = L"Paste transposed"; s.enabled = cells; return true;
    case CmdAutoSum: s.label = L"AutoSum"; s.shortcut = L"Alt+="; s.enabled = cells; return true;
    case CmdAutoFitRowHeight: s.label = L"AutoFit row height"; s.enabled = cells; return true;
    case CmdInsertShiftRight: s.label = L"Insert cells, shift right"; s.enabled = cells; return true;
    case CmdInsertShiftDown: s.label = L"Insert cells, shift down"; s.enabled = cells; return true;
    case CmdInsertRows: s.label = L"Insert entire rows"; s.enabled = cells; return true;
    case CmdInsertColumns: s.label = L"Insert entire columns"; s.enabled = cells; return true;
    case CmdDeleteShiftLeft: s.label = L"Delete cells, shift left"; s.enabled = cells; return true;
    case CmdDeleteShiftUp: s.label = L"Delete cells, shift up"; s.enabled = cells; return true;
    case CmdDeleteRows: s.label = L"Delete entire rows"; s.enabled = cells; return true;
    case CmdDeleteColumns: s.label = L"Delete entire columns"; s.enabled = cells; return true;
    case CmdMergeCenter: {
      const Range sel = Selected();
      s.label = L"Merge & Center";
      s.checked = cells && MergedIn(Range{active_.row, active_.col, active_.row, active_.col});
      s.enabled = cells && (s.checked || sel.firstRow != sel.lastRow || sel.firstCol != sel.lastCol);
      return true;
    }
    case CmdMergeAcross:
    case CmdMergeCells: {
      const Range sel = Selected();
      s.label = cmd == CmdMergeAcross ? L"Merge across" : L"Merge cells";
      s.enabled = cells && (sel.firstCol != sel.lastCol || (cmd == CmdMergeCells && sel.firstRow != sel.lastRow));
      return true;
    }
    case CmdUnmerge: s.label = L"Unmerge cells"; s.enabled = cells && MergedIn(Selected()); return true;
    case CmdFreezePanes: s.label = Frozen() ? L"Unfreeze panes" : L"Freeze panes"; s.enabled = cells; return true;
    case CmdFreezeTopRow: s.label = L"Freeze top row"; s.enabled = cells; return true;
    case CmdFreezeFirstColumn: s.label = L"Freeze first column"; s.enabled = cells; return true;
    case CmdGroup: s.label = L"Group"; s.shortcut = L"Alt+Shift+Right"; s.enabled = cells; return true;
    case CmdUngroup: s.label = L"Ungroup"; s.shortcut = L"Alt+Shift+Left"; s.enabled = cells; return true;
    case CmdAutoFitColumnWidth: s.label = L"AutoFit column width"; s.enabled = cells; return true;
    case CmdCalculateNow: s.label = L"Calculate now"; s.shortcut = L"F9"; s.enabled = Ready(); return true;
    case CmdCalculateSheet: s.label = L"Calculate sheet"; s.shortcut = L"Shift+F9"; s.enabled = Ready() && !ChartSheet(); return true;
    case CmdCalculateFull: s.label = L"Calculate everything"; s.shortcut = L"Ctrl+Alt+F9"; s.enabled = Ready(); return true;
    default: return false;
  }
}

bool WorkbookView::GridCommand(int cmd) {
  CommandState state;
  if (!GridCommandState(cmd, state)) return false;
  if (!state.enabled) return true;
  // A command ends the entry being typed first (unless it can't be written).
  if (editor_.Active() && !CommitEdit(false)) return true;
  if (ReviewCommand(cmd)) return true;
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
    case CmdInsertShiftRight:
    case CmdInsertShiftDown:
    case CmdInsertRows:
    case CmdInsertColumns: {
      // Ctrl++: whole rows and columns selected insert rows and columns; other cells shift down,
      // the default of Excel's Insert choices (the cell menu has the others).
      const bool r = cmd == CmdInsertRows || (cmd == CmdInsertCells && rows);
      const bool c = cmd == CmdInsertColumns || (cmd == CmdInsertCells && cols);
      EditRefused(r   ? Ed().InsertRows(sheet, sel.firstRow, sel.lastRow - sel.firstRow + 1)
                  : c ? Ed().InsertColumns(sheet, sel.firstCol, sel.lastCol - sel.firstCol + 1)
                      : Ed().InsertCells(sheet, sel, cmd == CmdInsertShiftRight ? edit::Editor::Direction::RightOrLeft
                                                                                 : edit::Editor::Direction::DownOrUp));
      return true;
    }
    case CmdDeleteCells:
    case CmdDeleteShiftLeft:
    case CmdDeleteShiftUp:
    case CmdDeleteRows:
    case CmdDeleteColumns: {
      const bool r = cmd == CmdDeleteRows || (cmd == CmdDeleteCells && rows);
      const bool c = cmd == CmdDeleteColumns || (cmd == CmdDeleteCells && cols);
      EditRefused(r   ? Ed().DeleteRows(sheet, sel.firstRow, sel.lastRow - sel.firstRow + 1)
                  : c ? Ed().DeleteColumns(sheet, sel.firstCol, sel.lastCol - sel.firstCol + 1)
                      : Ed().DeleteCells(sheet, sel, cmd == CmdDeleteShiftLeft ? edit::Editor::Direction::RightOrLeft
                                                                                : edit::Editor::Direction::DownOrUp));
      return true;
    }
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
    case CmdRenameSheet:
      host_.ViewFocusPage();  // the name is typed in the tab, from a menu or a panel too
      BeginRename(sheet_);
      return true;
    case CmdHideSheet: EditRefused(Ed().SetSheetState(sheet, SheetState::Hidden)); return true;
    case CmdMoveSheetLeft:
    case CmdMoveSheetRight: {
      const int to = ShownNeighbour(cmd == CmdMoveSheetLeft ? -1 : 1);
      if (to >= 0) EditRefused(Ed().MoveSheet(sheet, to));
      return true;
    }
    case CmdGoTo: ShowGoTo(); return true;
    case CmdFillDown:
    case CmdFillRight: FillDown(cmd == CmdFillRight); return true;
    case CmdCut: Cut(); return true;
    case CmdPaste: Paste(PasteMode::All); return true;
    case CmdPasteValues: Paste(PasteMode::Values); return true;
    case CmdPasteFormulas: Paste(PasteMode::Formulas); return true;
    case CmdPasteFormats: Paste(PasteMode::Formats); return true;
    case CmdPasteTranspose: Paste(PasteMode::All, true); return true;
    case CmdAutoSum: AutoSum(); return true;
    case CmdMergeCenter:
    case CmdMergeAcross:
    case CmdMergeCells:
    case CmdUnmerge: MergeSelection(cmd); return true;
    case CmdFreezePanes:
    case CmdFreezeTopRow:
    case CmdFreezeFirstColumn: Freeze(cmd); return true;
    case CmdGroup:
    case CmdUngroup: GroupSelection(cmd == CmdGroup); return true;
    case CmdAutoFitRowHeight:
    case CmdAutoFitColumnWidth: {
      // Fitted to the selected cells, as Excel's; whole rows or columns to all their cells.
      if (cmd == CmdAutoFitRowHeight) AutoFit(true, sel.firstRow, sel.lastRow, WholeRows() ? nullptr : &sel);
      else AutoFit(false, sel.firstCol, sel.lastCol, WholeColumns() ? nullptr : &sel);
      return true;
    }
    case CmdCalculateNow:
    case CmdCalculateFull:
      // Then at idle until done, in manual mode too.
      cmd == CmdCalculateNow ? Calc().CalculateNow() : Calc().CalculateFull();
      recalc_ = calcNow_ = true;
      host_.ViewStateChanged();
      return true;
    case CmdCalculateSheet:
      Calc().CalculateSheet(sheet);  // at once
      text_->Clear();
      totals_ = Totals{};
      for (auto& rules : rules_)
        if (rules) rules->Changed();
      CheckCircular();
      host_.ViewStateChanged();
      host_.ViewInvalidate();
      return true;
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
    AddReviewTabItems(m);
    AppendSeparator(m);
    for (size_t i = 0; i < book_->SheetCount() && CmdSheetPick + static_cast<int>(i) <= CmdSheetPickLast; ++i)
      if (SheetShown(i))
        AppendItem(m, CmdSheetPick + static_cast<UINT>(i), Utf8ToWide(book_->GetSheet(i).Name()), true, i == sheet_, true);
    return;
  }
  if (!preview_ && !ChartSheet()) {
    const bool rows = WholeRows(), cols = WholeColumns();
    item(m, CmdCut, L"Cut\tCtrl+X");
    AppendItem(m, CmdCopy, L"Copy\tCtrl+C");
    item(m, CmdPaste, L"Paste\tCtrl+V");
    HMENU special = CreatePopupMenu();
    item(special, CmdPasteValues, L"Values\tCtrl+Shift+V");
    item(special, CmdPasteFormulas, L"Formulas");
    item(special, CmdPasteFormats, L"Formats");
    item(special, CmdPasteTranspose, L"Transposed");
    AppendSubmenu(m, special, L"Paste special");
    AddReviewCellItems(m);
    AppendSeparator(m);
    if (rows || cols) {
      item(m, CmdInsertCells, rows ? L"Insert rows\tCtrl++" : L"Insert columns\tCtrl++");
      item(m, CmdDeleteCells, rows ? L"Delete rows\tCtrl+-" : L"Delete columns\tCtrl+-");
    } else {
      // Excel's Insert and Delete choices, as submenus.
      HMENU ins = CreatePopupMenu();
      item(ins, CmdInsertShiftDown, L"Shift cells down\tCtrl++");
      item(ins, CmdInsertShiftRight, L"Shift cells right");
      item(ins, CmdInsertRows, L"Entire row");
      item(ins, CmdInsertColumns, L"Entire column");
      AppendSubmenu(m, ins, L"Insert");
      HMENU del = CreatePopupMenu();
      item(del, CmdDeleteShiftUp, L"Shift cells up\tCtrl+-");
      item(del, CmdDeleteShiftLeft, L"Shift cells left");
      item(del, CmdDeleteRows, L"Entire row");
      item(del, CmdDeleteColumns, L"Entire column");
      AppendSubmenu(m, del, L"Delete");
    }
    item(m, CmdClearContents, L"Clear contents\tDelete");
    const Range sel = Selected();
    if (MergedIn(sel)) item(m, CmdUnmerge, L"Unmerge cells");
    else if (sel.firstRow != sel.lastRow || sel.firstCol != sel.lastCol) item(m, CmdMergeCenter, L"Merge & Center");
    item(m, CmdFormatCells, L"Format cells…\tCtrl+1");
    if (rows || cols) {
      AppendSeparator(m);
      item(m, rows ? CmdRowHeight : CmdColumnWidth, rows ? L"Row height…" : L"Column width…");
      item(m, CmdHide, L"Hide");
      item(m, CmdUnhide, L"Unhide");
      item(m, CmdGroup, L"Group\tAlt+Shift+Right");
      item(m, CmdUngroup, L"Ungroup\tAlt+Shift+Left");
    }
    item(m, CmdFreezePanes, Frozen() ? L"Unfreeze panes" : L"Freeze panes");
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
  // The engine read every formula again on the change (SyncEngine: StructureEdited).
  loadNext_ = 0;
  if (shown == SIZE_MAX) {  // the shown sheet was deleted: the next one, as in Excel
    shown = std::min(sheet_, n - 1);
    if (!SheetShown(shown)) shown = NextShownSheet(shown, 1);
  }
  shown_ = false;  // the state kept for the old index isn't this sheet's
  ShowSheet(shown);
}

// ---------------------------------------------------------------------------
// Merge, freeze panes, group

bool WorkbookView::MergedIn(const Range& r) const {
  for (const Range& m : Current().Merges())
    if (m.lastRow >= r.firstRow && m.firstRow <= r.lastRow && m.lastCol >= r.firstCol && m.firstCol <= r.lastCol)
      return true;
  return false;
}

void WorkbookView::MergeSelection(int cmd) {
  const int sheet = static_cast<int>(sheet_);
  const Range sel = Selected();
  // Merge & Center on a merged cell takes it apart again, as Excel's button does.
  if (cmd == CmdUnmerge || (cmd == CmdMergeCenter && MergedIn(Range{active_.row, active_.col, active_.row, active_.col}))) {
    EditRefused(Ed().Unmerge(sheet, Expand(sel)));
    return;
  }
  std::vector<Range> areas;
  if (cmd == CmdMergeAcross && sel.firstCol != sel.lastCol) {
    for (uint32_t r = sel.firstRow; r <= sel.lastRow && r - sel.firstRow < 100000; ++r)
      areas.push_back(Range{r, sel.firstCol, r, sel.lastCol});
  } else if (sel.firstRow != sel.lastRow || sel.firstCol != sel.lastCol) {
    areas.push_back(sel);
  }
  if (areas.empty()) return;
  // Only each area's upper-left value stays: Excel asks first when others would go.
  const Sheet& s = Current();
  bool loses = false;
  for (const Range& a : areas) {
    s.ForEachCell(a, [&](const CellInfo& c) {
      if ((c.row != a.firstRow || c.col != a.firstCol) && (c.value.type != ValueType::Empty || c.hasFormula)) loses = true;
      return !loses;
    });
    if (loses) break;
  }
  if (loses && !host_.ViewConfirm(L"Merging cells only keeps the upper-left value and discards other values.", L"Merge"))
    return;
  edit::EditGroup group(Ed(), cmd == CmdMergeCenter ? u"Merge & Center" : u"Merge Cells");
  edit::Result r;
  for (const Range& a : areas)
    if (r) r = Ed().Merge(sheet, a);
  if (r && cmd == CmdMergeCenter) {
    // Centered across, as the button's name says.
    const Styles& styles = book_->GetStyles();
    std::vector<std::pair<uint32_t, uint32_t>> made;
    r = Ed().MapStyles(sheet, sel, [&](uint32_t style) {
      for (const auto& [from, to] : made)
        if (from == style) return to;
      Xf xf = styles.CellXf(style);
      xf.alignment.horizontal = HAlign::Center;
      const uint32_t to = Ed().AddXf(xf);
      made.emplace_back(style, to);
      return to;
    });
  }
  if (EditRefused(r)) return;
  group.done = true;
}

void WorkbookView::Freeze(int cmd) {
  const int sheet = static_cast<int>(sheet_);
  if (cmd == CmdFreezePanes && Frozen()) {
    EditRefused(Ed().FreezePanes(sheet, 0, 0));
    return;
  }
  // From the top-left cell on screen: the rows above the active cell and the columns left of it,
  // or the first row or column shown.
  const CellAddr top{grid_.RowAt(static_cast<int64_t>(gridY_)), grid_.ColAt(static_cast<int64_t>(gridX_))};
  uint32_t rows = 0, cols = 0;
  if (cmd == CmdFreezeTopRow) {
    rows = 1;
  } else if (cmd == CmdFreezeFirstColumn) {
    cols = 1;
  } else {
    rows = active_.row > top.row ? active_.row - top.row : 0;
    cols = active_.col > top.col ? active_.col - top.col : 0;
    if (rows == 0 && cols == 0) {
      // At the top-left cell, Excel freezes at the middle of the window.
      const double k = K();
      rows = std::max<uint32_t>(1, grid_.RowAt(static_cast<int64_t>(gridY_ + (cells_.bottom - cells_.top) / k / 2)) - top.row);
      cols = std::max<uint32_t>(1, grid_.ColAt(static_cast<int64_t>(gridX_ + (cells_.right - cells_.left) / k / 2)) - top.col);
    }
  }
  if (Frozen()) EditRefused(Ed().FreezePanes(sheet, 0, 0));
  Ed().SetTopLeft(sheet, Frozen() ? CellAddr{frozenRow0_, frozenCol0_} : top);
  EditRefused(Ed().FreezePanes(sheet, rows, cols));
}

void WorkbookView::PanesChanged() {
  // As the file's view says now; the selection and zoom stay.
  const CellAddr active = active_, end = end_;
  const double zoom = zoom_;
  states_[sheet_].sx = states_[sheet_].sy = -1;
  LoadView();
  active_ = active;
  end_ = end;
  zoom_ = zoom;
  LayoutParts();
  ClampScroll();
  ScrollIntoView(active_);
}

void WorkbookView::GroupSelection(bool in) {
  // Rows unless whole columns are selected (Excel asks; rows is its answer by default).
  const int sheet = static_cast<int>(sheet_);
  const Range sel = Selected();
  const bool cols = WholeColumns();
  const Sheet& s = Current();
  const uint32_t first = cols ? sel.firstCol : sel.firstRow, last = cols ? sel.lastCol : sel.lastRow;
  auto level = [&](uint32_t i) { return cols ? s.Column(i).outlineLevel : s.Row(i).outlineLevel; };
  edit::EditGroup group(Ed(), in ? u"Group" : u"Ungroup");
  edit::Result r;
  bool changed = false;
  // A run of rows (or columns) at one level goes one level in or out at a time.
  for (uint32_t i = first; i <= last && r;) {
    const uint8_t at = level(i);
    uint32_t j = i;
    while (j < last && level(j + 1) == at) ++j;
    if (in ? at < 7 : at > 0) {
      const uint8_t to = static_cast<uint8_t>(in ? at + 1 : at - 1);
      r = cols ? Ed().SetColumnOutline(sheet, i, j, to) : Ed().SetRowOutline(sheet, i, j, to);
      changed = true;
    }
    if (j == last) break;
    i = j + 1;
  }
  if (EditRefused(r)) return;
  if (!changed && !in) host_.ViewMessage(BannerKind::Info, cols ? L"These columns aren't grouped." : L"These rows aren't grouped.");
  group.done = changed;
}

// ---------------------------------------------------------------------------
// Moving a sheet by dragging its tab

void WorkbookView::TabDragMove(float x) {
  if (!tabDrag_) return;
  TabDrag& d = *tabDrag_;
  if (!d.moving && std::abs(x - d.x0) < 6 * ui_) return;
  d.moving = true;
  // Before the first tab whose middle is past the mouse; the end after the last.
  d.before = book_->SheetCount();
  for (const TabRect& t : TabRects()) {
    const float mid = (t.rc.left + t.rc.right) / 2;
    if (rtl_ ? x > mid : x < mid) {
      d.before = t.sheet;
      break;
    }
  }
  host_.ViewInvalidate();
}

void WorkbookView::FinishTabDrag() {
  if (!tabDrag_) return;
  const TabDrag d = *tabDrag_;
  tabDrag_.reset();
  if (!d.moving) return;
  const size_t n = book_->SheetCount();
  const size_t to = d.before >= n ? n - 1 : d.before > d.sheet ? d.before - 1 : d.before;
  if (to != d.sheet) EditRefused(Ed().MoveSheet(static_cast<int>(d.sheet), static_cast<int>(to)));
}

void WorkbookView::DrawTabDrop(Canvas& c) {
  if (mouse_ != Mouse::TabDrag || !tabDrag_ || !tabDrag_->moving) return;
  const std::vector<TabRect> tabs = TabRects();
  if (tabs.empty()) return;
  float x = rtl_ ? tabs.back().rc.left : tabs.back().rc.right;
  for (const TabRect& t : tabs)
    if (t.sheet == tabDrag_->before) {
      x = rtl_ ? t.rc.right : t.rc.left;
      break;
    }
  // A small arrow over the gap, as Excel's.
  const Palette& p = theme::P();
  const float u = ui_, top = tabs_.top + 2 * u;
  c.Fill(D2D1_RECT_F{x - 1 * u, top, x + 1 * u, tabs_.bottom - 4 * u}, p.accent);
  c.Fill(D2D1_RECT_F{x - 4 * u, top, x + 4 * u, top + 2 * u}, p.accent);
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
