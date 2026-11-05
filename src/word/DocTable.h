#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "TabContent.h"
#include "doc/edit/Editor.h"
#include "ui/SidePanel.h"
#include "ui/Toolbar.h"

// Tables in the Word tab (plan §6.2 "Tables", milestone D3).
//  - The toolbar's table button: outside a table, a grid to insert one; in a table, its menu.
//    Command search and the context menu offer the same commands, with Word's names.
//  - Tab and Shift+Tab go from cell to cell, selecting the cell's text; Tab in the last cell
//    adds a row. A selection from one cell into another covers whole cells: Delete empties them,
//    typing replaces them.
//  - The Table panel (the side panel) sets column widths and row heights, AutoFit, cell
//    alignment, margins, borders and shading, the table's style and alignment, the header row,
//    and splits cells; outside a table it inserts a table of any size.
// Each change is one undo step of the tab's editor.
class DocTable {
 public:
  struct Host {
    // The selection: where it started and where the caret is.
    std::function<std::pair<fw::doc::Pos, fw::doc::Pos>()> selection;
    // After an edit: the caret where it says, or the refusal shown. False for a refusal.
    std::function<bool(const fw::doc::edit::Result&)> apply;
    // Selects from `anchor` to `caret`.
    std::function<void(fw::doc::Pos anchor, fw::doc::Pos caret)> select;
    // Opens a side panel (null closes it).
    std::function<void(PanelContent*)> showPanel;
    // The keyboard back to the page (from the panel, after Insert table).
    std::function<void()> focusPage;
  };

  DocTable(fw::doc::edit::Editor& editor, Host host);
  ~DocTable();
  DocTable(const DocTable&) = delete;
  DocTable& operator=(const DocTable&) = delete;

  static bool Owns(int cmd);
  static std::vector<int> CommandIds();
  CommandState Query(int cmd);
  void Execute(int cmd);
  // The table button, after the formatting groups.
  void AddToolbar(ToolbarModel& m);
  // In a table: Insert, Delete, Merge, Split and Table properties.
  void FillContextMenu(HMENU m);

  // Tab or Shift+Tab: the next or previous cell's text selected (a new row after the last cell).
  // False outside tables.
  bool Tab(bool back);
  // The cells the selection covers when it goes from one cell into another, else nullopt.
  std::optional<fw::doc::edit::Editor::CellBlock> SelectedCells() const;
  // Typing or Delete with cells selected: they're emptied, and `text` goes into the first.
  // One undo step. False when no cells are selected.
  bool ReplaceCells(std::u16string_view text);

 private:
  class GridPicker;
  class Panel;
  friend class Panel;

  const fw::doc::Document& Doc() const { return editor_.Doc(); }
  std::optional<fw::doc::edit::Editor::CellInfo> Cell() const;
  bool Run(const fw::doc::edit::Result& r) { return host_.apply(r); }
  // A change of properties: the paragraphs stay where they are, and so does the selection.
  bool Keep(const fw::doc::edit::Result& r) {
    auto [a, f] = host_.selection();
    if (!host_.apply(r)) return false;
    if (!(a == f)) host_.select(a, f);
    return true;
  }
  void Insert(uint32_t rows, uint32_t cols);
  void Select(fw::doc::edit::Editor::Extent extent);
  // A table style by name: the document's, or one of Word's basics added to it. One undo step.
  void ApplyStyle(const std::string& name);
  // Grid column widths for AutoFit to contents, measured roughly from the cells' text.
  std::vector<int32_t> ContentWidths(const fw::doc::edit::Editor::CellInfo& at) const;

  fw::doc::edit::Editor& editor_;
  Host host_;
  std::unique_ptr<GridPicker> grid_;
  std::unique_ptr<Panel> panel_;
};
