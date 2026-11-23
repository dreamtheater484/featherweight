#pragma once

// The ribbon's content, as each editor describes it (plan §5.6, milestone R1).
//
// TabContent::BuildRibbon fills a RibbonModel whenever the ribbon lays out or repaints (as
// BuildToolbar fills a ToolbarModel), and ui::Ribbon shows it: tabs, groups, large, medium and
// small buttons, boxes, galleries and dialog launchers, placed as in Office. The File tab and its
// page are the window's (ui::FilePage); an editor only adds its own entries to it (`file`).
//
// Every item runs an existing command (ToolItem::cmd), so the ribbon is a new front end on the same
// commands as the shortcuts, the right-click menus and command search. A unit test walks each tab's
// model against the command table: every item's command must be one the tab knows.
//
// Placement rules for the module sessions:
//  - Tabs, groups and items in Office's order (Word, Excel and PowerPoint 2021/365, classic ribbon).
//  - Only commands Featherweight has: an Office button with no Featherweight command is left out,
//    never shown greyed out. Set ToolItem::enabled from the selection as the toolbar does today.
//  - Labels in Office's words. An empty label takes the command's own (QueryCommand(cmd).label).

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ui/Toolbar.h"  // ToolItem, ToolKind, ToolValue

// How big an item is drawn.
//   Large:  a 32-DIP icon above its label (one or two lines), the full height of the group.
//   Medium: a 16-DIP icon with its label to the right: one of the group's rows.
//   Small:  the 16-DIP icon alone (the label shows in the tooltip): one of the group's rows.
// Boxes and pickers (ToolKind::ComboBox, Dropdown with no glyph, SpinBox, ColorPicker, a SplitButton
// with colors) are one row high whatever the size, ToolItem::width DIPs wide. A Custom control (the
// tab's own) is one row high too, or, Large, the group's full height: it draws itself in that place
// (Word's table grid as a large "Table" button), at least ToolItem::width wide.
// A Button with `toggle` and no glyph, Medium, is a check box with its label (View › Ruler).
enum class RibbonSize { Large, Medium, Small };

// A gallery: a strip of choices shown in the group itself (Word's Styles, table and shape styles),
// with arrows to scroll it and a ▾ that opens every choice in a grid. The choices are the item's
// ToolItem::choices (their names, for tooltips, KeyTips' popup and command search) and the chosen
// one ToolItem::choice; picking one calls ToolItem::onChange with ToolValue::index.
struct RibbonGallery {
  // Draws choice `index` in `r` (client pixels, at the window's scale `ui`), such as "AaBbCcDd" in
  // the style's font. Without it, the choice's name is drawn.
  std::function<void(gfx::Canvas& c, const D2D1_RECT_F& r, int index, float ui)> drawChoice;
  float choiceWidth = 72;   // DIPs, one choice
  float choiceHeight = 56;  // DIPs; the strip shows one row of choices
  // Choices in the strip at full width. 0: no strip, only a button (the item's size, `label` and
  // glyph) that opens the grid (PowerPoint's Layout, Excel's Cell Styles). A SplitButton gallery's
  // main part runs ToolItem::cmd and its arrow opens the grid (New Slide).
  int columns = 5;
  int minColumns = 2;       // as narrow as the strip gets before it becomes a large button
  int popupColumns = 0;     // columns in the opened grid; 0: as in the strip (4 for a button's)
  // Commands under the grid when it's open ("Create a Style", "Clear Formatting"); 0 a separator.
  std::vector<int> menu;
};

// One item in a group.
struct RibbonItem {
  // The control's kind, command, glyph and state, as on the toolbar: ToolKind::Button (with
  // `dropdown` for a menu), SplitButton, Dropdown, ComboBox, ColorPicker, SpinBox or Custom.
  // `tooltip` may stay empty: the ribbon shows the command's label and shortcut.
  ToolItem tool;
  RibbonSize size = RibbonSize::Small;
  // The smallest the item becomes as the window narrows (Large → Medium → Small), before its group
  // folds into one button. Paste stays Large; Cut, Copy and the format painter go down to Small.
  RibbonSize smallest = RibbonSize::Small;
  // Its name on the button ("Paste", "Format Painter"); empty: the command's label. For a Large
  // item, "\n" sets where its two lines break (else the ribbon breaks it at a space).
  std::wstring label;
  // The letters after Alt and the tab's KeyTip ("V" for Paste, "FF" for the font box). Empty: the
  // ribbon makes one from the label, unique within the tab.
  std::wstring keyTip;
  // A line under the name in the tooltip ("Pick a look for your text.").
  std::wstring description;

  // Where it goes. A Large item, or a gallery, takes a column of its own. Other items stack one per
  // row, `RibbonGroup::rows` to a column, unless:
  bool sameRow = false;    // in the previous item's row, to its right (B I U, or a box and its buttons)
  bool newColumn = false;  // starts a new column even if the current one has room

  // A Button with `tool.dropdown`: the commands its menu lists (0: a separator), with their labels,
  // shortcuts and check marks from QueryCommand. Empty: the tab's FillMenu(tool.cmd) fills it, as
  // the toolbar's menus do now. A SplitButton's arrow opens the same menu, or its `choices`.
  std::vector<int> menu;

  // A gallery in place of a button.
  std::optional<RibbonGallery> gallery;
};

// A group: a labelled block of items, with the ↘ dialog launcher in its bottom right corner.
struct RibbonGroup {
  std::wstring name;     // "Clipboard", "Font", "Paragraph"
  // The ↘ launcher's command (Format Cells, the Paragraph panel); 0 for none.
  int launcher = 0;
  // The icon of the one button the group becomes when the window is too narrow for it (it opens the
  // group in a popup). 0: the first item's glyph.
  wchar_t glyph = 0;
  // That button's KeyTip ("ZC"); empty: the ribbon makes one.
  std::wstring keyTip;
  int rows = 3;          // rows in a column of Medium and Small items: 3, or 2 (Font, Paragraph)
  // Groups with higher values shrink first as the window narrows; equal ones from the right.
  int shrinkFirst = 0;
  std::vector<RibbonItem> items;
};

// A tab of the ribbon: Home, Insert, Layout and so on.
struct RibbonTab {
  std::wstring name;    // "Home"
  std::wstring keyTip;  // "H"; empty: the ribbon makes one
  // A contextual tab (Table Design, Picture Format): the tab adds it only while such an object is
  // selected, after the other tabs. It's drawn in the accent color, at the right of the others.
  bool contextual = false;
  std::vector<RibbonGroup> groups;
};

// Where an editor's own entry goes on the File page.
enum class RibbonFilePlace {
  Info,     // about this file: Restore original, protection, properties
  Export,   // other files made from it, beside Export PDF
  Options,  // settings of this kind of tab (spelling as you type, the reviewer's name)
  Print,    // print settings (PowerPoint's slides, notes pages, handouts): File › Print then shows
            // them under a Print button, as Office does; without any, Print opens the dialog at once
};

struct RibbonFileItem {
  RibbonFilePlace place = RibbonFilePlace::Info;
  int cmd = 0;
  std::wstring label;        // empty: the command's
  std::wstring description;  // a line under it
  wchar_t glyph = 0;
  // A setting: shown with a check box (`setting`) or as one of a set (`choice`, a round button),
  // checked as QueryCommand says; the page stays open when it changes. In Options, an entry without
  // either is a setting unless its label ends in "…" (it opens a box).
  bool setting = false;
  bool choice = false;
};

// What the ribbon shows for the active tab.
struct RibbonModel {
  // After File, in Office's order; contextual tabs last. Leaving it empty keeps the tab's toolbar
  // (the PDF reader, and an editor before its ribbon is done).
  std::vector<RibbonTab> tabs;
  // The editor's own entries on the File page, besides the window's (New, Open, Save, Save As,
  // Print, Export PDF, Restore original and the app's options).
  std::vector<RibbonFileItem> file;
  // A tab to switch to, by name, once each time this changes to a non-empty value: Picture Format
  // after inserting a picture, as Office does. Selecting an object doesn't switch tabs.
  std::wstring show;
  // Quick access in the title bar (Save, Undo, Redo) runs the commands CmdSave, CmdUndo and CmdRedo.
  // An item here with the same command and `choices` adds a ▾ with that list (the actions Undo
  // would take back); picking one calls its onChange with ToolValue::index.
  std::vector<ToolItem> quick;
};
