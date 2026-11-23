// The workbook tab's ribbon (R1, r1-sheet; plan §5.6): Excel 365's tabs, groups and buttons, each on
// one of the tab's commands. The tables follow Excel's own ribbon as UI Automation reads it (build
// 20326, classic ribbon at full width: names, order, sizes and KeyTips); Excel's buttons that
// Featherweight has no command for are left out. The layout and the reasons are in
// FeatherweightNotes\r1\r1-sheet-state.md.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>

#include "Commands.h"
#include "core/Strings.h"
#include "sheet/view/WorkbookView.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/RibbonModel.h"

namespace fw::sheet::view {

namespace {

// Segoe Fluent Icons (Segoe MDL2 Assets on Windows 10) code points the toolbar didn't name; the
// rest are ui/Icons.h's, its drawn ones (icon::AlignTop, icon::MergeCenter...) where the fonts have
// nothing like Excel's picture.
namespace g {
constexpr wchar_t Calculator = 0xE8EF, Check = 0xE930, Cut = 0xE8C6, Down = 0xE74B, Eraser = 0xE75C,
                  Filter = 0xE71C, FontDown = 0xE8E7, FontUp = 0xE8E8, Key = 0xE8D7, Next = 0xE76C,
                  Note = 0xE70B, Paste = 0xE77F, Pencil = 0xE70F, Prev = 0xE76B, Select = 0xE8B3,
                  Sort = 0xE8CB, Sync = 0xE895, Tag = 0xE8EC, Window = 0xE8A7, Zoom = 0xE71E;
}

// Item sizes and flags, as the tables below write them.
constexpr uint8_t L = 0, M = 1, S = 2;  // RibbonSize: Large, Medium, Small
enum : uint8_t {
  kToggle = 1,      // stays on while checked
  kMenu = 2,        // a button that opens a menu (FillRibbonMenu)
  kSplit = 4,       // a split button: the command, and its menu
  kSameRow = 8,     // in the previous item's row
  kNewColumn = 16,  // starts a column
  kTool = 32,       // a box, color button or dropdown of its own (RibbonTool)
  kKeepLarge = 64,  // a large item that stays large as the window narrows (Paste)
  kNoLabel = 128,   // a box Excel draws without "Label:" in front (its tooltip names it)
};

// One line of a tab's table: a group (cmd 0: its ↘ launcher, collapsed glyph, rows in `size`, how
// early it shrinks in `flags`, KeyTip and name), or an item of the group above it. Kept small:
// about 160 of them.
struct Spec {
  uint16_t cmd;
  uint16_t launcher;
  wchar_t glyph;
  uint8_t size;
  uint8_t flags;
  char key[4];        // Excel's KeyTip
  const char* label;  // UTF-8
};

constexpr Spec kHome[] = {
    {0, 0, g::Paste, 3, 0, "ZL", "Clipboard"},
    {CmdPaste, 0, g::Paste, L, kSplit | kKeepLarge, "V", "Paste"},
    {CmdCut, 0, g::Cut, M, 0, "X", "Cut"},
    {CmdCopy, 0, icon::Copy, M, 0, "C", "Copy"},
    {CmdFormatPainter, 0, icon::Brush, M, kToggle, "FP", "Format Painter"},
    {0, CmdFormatCellsFont, icon::FontColor, 2, 0, "ZF", "Font"},
    {CmdFontBox, 0, 0, M, kTool | kNoLabel, "FF", "Font"},
    {CmdSizeBox, 0, 0, M, kTool | kSameRow | kNoLabel, "FS", "Font Size"},
    {CmdGrowFont, 0, g::FontUp, S, kSameRow, "FG", "Increase Font Size"},
    {CmdShrinkFont, 0, g::FontDown, S, kSameRow, "FK", "Decrease Font Size"},
    {CmdBold, 0, icon::Bold, S, kToggle, "1", "Bold"},
    {CmdItalic, 0, icon::Italic, S, kToggle | kSameRow, "2", "Italic"},
    {CmdUnderline, 0, icon::Underline, S, kToggle | kSameRow, "3", "Underline"},
    {CmdBordersBox, 0, icon::BordersBottom, S, kTool | kSameRow, "B", "Borders"},
    {CmdFillColor, 0, icon::FillBucket, S, kTool | kSameRow, "H", "Fill Color"},
    {CmdFontColor, 0, icon::FontColor, S, kTool | kSameRow, "FC", "Font Color"},
    {0, CmdFormatCellsAlignment, icon::AlignLeft, 2, 0, "ZA", "Alignment"},
    {CmdAlignTop, 0, icon::AlignTop, S, kToggle, "AT", "Top Align"},
    {CmdAlignMiddle, 0, icon::AlignMiddle, S, kToggle | kSameRow, "AM", "Middle Align"},
    {CmdAlignBottom, 0, icon::AlignBottom, S, kToggle | kSameRow, "AB", "Bottom Align"},
    {CmdAlignLeft, 0, icon::AlignLeft, S, kToggle, "AL", "Align Left"},
    {CmdAlignCenter, 0, icon::AlignCenter, S, kToggle | kSameRow, "AC", "Center"},
    {CmdAlignRight, 0, icon::AlignRight, S, kToggle | kSameRow, "AR", "Align Right"},
    {CmdOutdent, 0, icon::DecreaseIndent, S, kSameRow, "5", "Decrease Indent"},
    {CmdIndent, 0, icon::IncreaseIndent, S, kSameRow, "6", "Increase Indent"},
    {CmdWrapText, 0, icon::WrapCell, M, kToggle | kNewColumn, "W", "Wrap Text"},
    {CmdMergeCenter, 0, icon::MergeCenter, M, kSplit, "M", "Merge & Center"},
    {0, CmdNumberFormatBox, icon::Percent, 2, 0, "ZN", "Number"},
    {CmdNumberFormatBox, 0, 0, M, kTool | kNoLabel, "N", "Number Format"},
    {CmdNumAccounting, 0, icon::Accounting, S, 0, "AN", "Accounting Number Format"},
    {CmdNumPercent, 0, icon::Percent, S, kSameRow, "P", "Percent Style"},
    {CmdCommaStyle, 0, icon::Comma, S, kSameRow, "K", "Comma Style"},
    {CmdIncreaseDecimal, 0, icon::IncreaseDecimal, S, kSameRow, "0", "Increase Decimal"},
    {CmdDecreaseDecimal, 0, icon::DecreaseDecimal, S, kSameRow, "9", "Decrease Decimal"},
    {0, 0, icon::Color, 3, 0, "ZS", "Styles"},
    {CmdCondFormatBox, 0, icon::ConditionalFormat, L, kMenu, "L", "Conditional Formatting"},
    {CmdCreateTable, 0, icon::Table, L, 0, "T", "Format as Table"},
    {CmdCellStyles, 0, icon::CellStyles, L, kTool, "J", "Cell Styles"},
    {0, 0, icon::Add, 3, 0, "ZC", "Cells"},
    {CmdInsertCells, 0, icon::InsertCells, L, kSplit, "I", "Insert"},
    {CmdDeleteCells, 0, icon::Delete, L, kSplit, "D", "Delete"},
    {CmdCellsFormatMenu, 0, icon::Settings, L, kMenu, "O", "Format"},
    {0, 0, icon::Search, 3, 0, "ZE", "Editing"},
    {CmdAutoSum, 0, icon::AutoSum, M, 0, "U", "AutoSum"},
    {CmdFillMenu, 0, g::Down, M, kMenu, "FI", "Fill"},
    {CmdClearMenu, 0, g::Eraser, M, kMenu, "E", "Clear"},
    {CmdSortFilterMenu, 0, g::Sort, L, kMenu, "S", "Sort & Filter"},
    {CmdFindSelectMenu, 0, icon::Search, L, kMenu, "FD", "Find & Select"},
};

constexpr Spec kInsert[] = {
    {0, 0, icon::Table, 3, 0, "ZB", "Tables"},
    {CmdPivotTable, 0, icon::PivotTable, L, 0, "V", "PivotTable"},
    {CmdCreateTable, 0, icon::Table, L, 0, "T", "Table"},
    {0, 0, icon::Picture, 3, 0, "ZI", "Illustrations"},
    {CmdObjectPicture, 0, icon::Picture, L, 0, "P", "Pictures"},
    {CmdShapesMenu, 0, icon::Shapes, L, kMenu, "SH", "Shapes"},
    {0, CmdInsertChart, icon::Report, 3, 0, "ZC", "Charts"},
    {CmdColumnChartMenu, 0, icon::ChartColumn, S, kMenu, "C", "Insert Column or Bar Chart"},
    {CmdLineChartMenu, 0, icon::ChartLine, S, kMenu, "N1", "Insert Line or Area Chart"},
    {CmdPieChartMenu, 0, icon::ChartPie, S, kMenu, "Q", "Insert Pie or Doughnut Chart"},
    {CmdChartScatter, 0, icon::ChartScatter, S, kNewColumn, "D", "Insert Scatter (X, Y) Chart"},
    {CmdChartCombo, 0, icon::ChartCombo, S, 0, "SD", "Insert Combo Chart"},
    {0, 0, icon::Link, 3, 0, "ZL", "Links"},
    {CmdInsertLink, 0, icon::Link, L, 0, "I2", "Link"},
    {0, 0, icon::TextBox, 3, 0, "ZT", "Text"},
    {CmdObjectTextBox, 0, icon::TextBox, L, 0, "X", "Text Box"},
};

constexpr Spec kPageLayout[] = {
    {0, CmdPageSetup, icon::Page, 3, 0, "ZS", "Page Setup"},
    {CmdMarginsMenu, 0, icon::Margins, L, kMenu, "M", "Margins"},
    {CmdOrientationMenu, 0, icon::Orientation, L, kMenu, "O", "Orientation"},
    {CmdPrintAreaMenu, 0, icon::PrintArea, L, kMenu, "R", "Print Area"},
    {CmdBreaksMenu, 0, icon::PageBreak, L, kMenu, "B", "Breaks"},
    {CmdPrintTitles, 0, icon::PrintTitles, L, 0, "I", "Print Titles"},
    {0, CmdPageSetup, icon::FullScreen, 3, 0, "ZC", "Scale to Fit"},
    {CmdScaleWidth, 0, 0, M, kTool, "W", "Width"},
    {CmdScaleHeight, 0, 0, M, kTool, "H", "Height"},
};

constexpr Spec kFormulas[] = {
    {0, 0, icon::AutoSum, 3, 0, "ZL", "Function Library"},
    {CmdAutoSum, 0, icon::AutoSum, L, 0, "U", "AutoSum"},
    {0, 0, g::Tag, 3, 0, "ZN", "Defined Names"},
    {CmdNameManager, 0, g::Tag, L, 0, "N", "Name Manager"},
    {CmdDefineName, 0, g::Pencil, M, 0, "M", "Define Name"},
    {CmdPasteNames, 0, g::Tag, M, 0, "S", "Use in Formula"},
    {CmdCreateNames, 0, g::Select, M, 0, "C", "Create from Selection"},
    {0, 0, g::Calculator, 2, 0, "ZC", "Calculation"},
    {CmdCalculateNow, 0, g::Calculator, M, 0, "B", "Calculate Now"},
    {CmdCalculateSheet, 0, g::Sync, M, 0, "J", "Calculate Sheet"},
};

constexpr Spec kData[] = {
    {0, 0, g::Sync, 3, 0, "ZC", "Queries & Connections"},
    {CmdPivotRefreshAll, 0, g::Sync, L, kSplit, "R", "Refresh All"},
    {0, 0, g::Sort, 2, 0, "ZS", "Sort & Filter"},
    {CmdSortAscending, 0, icon::SortAscending, S, 0, "SA", "Sort A to Z"},
    {CmdSortDescending, 0, icon::SortDescending, S, 0, "SD", "Sort Z to A"},
    {CmdDataBox, 0, g::Sort, L, 0, "SS", "Sort"},
    {CmdFilter, 0, g::Filter, L, kToggle, "T", "Filter"},
    {0, 0, icon::RemoveDuplicates, 3, 0, "ZD", "Data Tools"},
    {CmdRemoveDuplicates, 0, icon::RemoveDuplicates, L, 0, "M", "Remove Duplicates"},
    {CmdDataValidation, 0, icon::DropDownList, L, kSplit, "V", "Data Validation"},
    {0, 0, icon::Add, 3, 0, "ZO", "Outline"},
    {CmdGroup, 0, icon::GroupRows, L, 0, "G", "Group"},
    {CmdUngroup, 0, icon::UngroupRows, L, 0, "U", "Ungroup"},
};

constexpr Spec kReview[] = {
    {0, 0, icon::Comment, 3, 0, "ZC", "Comments"},
    {CmdNewComment, 0, icon::Comment, L, 0, "C", "New Comment"},
    {CmdDeleteThread, 0, icon::Delete, L, 0, "D", "Delete"},
    {0, 0, g::Note, 3, 0, "ZN", "Notes"},
    {CmdNotesMenu, 0, g::Note, L, kMenu, "T", "Notes"},
    {0, 0, icon::Lock, 3, 0, "ZH", "Protect"},
    {CmdProtectSheet, 0, icon::Lock, L, 0, "PS", "Protect Sheet"},
    {CmdProtectWorkbook, 0, g::Key, L, kToggle, "PW", "Protect Workbook"},
};

constexpr Spec kView[] = {
    {0, 0, icon::ViewNormal, 3, 0, "ZV", "Workbook Views"},
    {CmdViewNormal, 0, icon::ViewNormal, L, kToggle, "L", "Normal"},
    {CmdPageBreakPreview, 0, icon::PageBreakPreview, L, kToggle, "I", "Page Break Preview"},
    {0, 0, g::Zoom, 3, 0, "ZZ", "Zoom"},
    {CmdZoomMenu, 0, g::Zoom, L, kMenu, "Q", "Zoom"},
    {CmdZoomActual, 0, icon::Zoom100, L, 0, "J", "100%"},
    {0, 0, icon::FreezePanes, 3, 0, "ZW", "Window"},
    {CmdFreezeMenu, 0, icon::FreezePanes, L, kMenu, "F", "Freeze Panes"},
};

// Contextual tabs, while what they work on is selected.
constexpr Spec kTableDesign[] = {
    {0, 0, icon::Table, 3, 0, "ZT", "Tools"},
    {CmdPivotTable, 0, icon::PivotTable, M, 0, "V", "Summarize with PivotTable"},
    {CmdRemoveDuplicates, 0, icon::RemoveDuplicates, M, 0, "M", "Remove Duplicates"},
    {CmdConvertToRange, 0, icon::Table, M, 0, "G", "Convert to Range"},
    {0, 0, g::Check, 3, 0, "ZL", "Table Style Options"},
    {CmdTableTotalRow, 0, 0, M, kToggle, "T", "Total Row"},  // no glyph: a check box, as Excel's
    {CmdFilter, 0, 0, M, kToggle, "B", "Filter Button"},
    {0, 0, icon::Color, 3, 0, "ZS", "Table Styles"},
    {CmdTableDesign, 0, icon::Color, L, 0, "S", "Quick Styles"},
};

constexpr Spec kPivotAnalyze[] = {
    {0, 0, g::Sync, 3, 0, "ZD", "Data"},
    {CmdPivotRefresh, 0, g::Sync, L, kSplit, "F", "Refresh"},
};

constexpr Spec kChartDesign[] = {
    {0, 0, icon::Add, 3, 0, "ZL", "Chart Layouts"},
    {CmdChartPanel, 0, icon::Add, L, 0, "A", "Add Chart Element"},
    {0, 0, icon::Color, 3, 0, "ZS", "Chart Styles"},
    {CmdChartPanel, 0, icon::Color, L, 0, "H", "Change Colors"},
    {0, 0, icon::Table, 3, 0, "ZD", "Data"},
    {CmdChartPanel, 0, icon::Table, L, 0, "E", "Select Data"},
    {0, 0, icon::Report, 3, 0, "ZT", "Type"},
    {CmdChartPanel, 0, icon::Report, L, 0, "C", "Change Chart Type"},
    {0, 0, g::Window, 3, 0, "ZO", "Location"},
    {CmdMoveChart, 0, g::Window, L, 0, "V", "Move Chart"},
};

constexpr Spec kPictureFormat[] = {
    {0, CmdFormatObject, icon::Picture, 3, 0, "ZS", "Picture Styles"},
    {CmdObjectOutline, 0, g::Pencil, M, kTool, "SO", "Picture Border"},
    {0, 0, icon::AltText, 3, 0, "Z1", "Accessibility"},
    {CmdFormatObject, 0, icon::AltText, L, 0, "AT", "Alt Text"},
};

constexpr Spec kShapeFormat[] = {
    {0, 0, icon::Shapes, 3, 0, "ZH", "Insert Shapes"},
    {CmdShapesMenu, 0, icon::Shapes, L, kMenu, "SH", "Shapes"},
    {CmdObjectTextBox, 0, icon::TextBox, M, 0, "X", "Text Box"},
    {0, CmdFormatObject, icon::Color, 3, 0, "ZY", "Shape Styles"},
    {CmdObjectFill, 0, icon::Color, M, kTool, "SF", "Shape Fill"},
    {CmdObjectOutline, 0, g::Pencil, M, kTool, "SO", "Shape Outline"},
    {0, 0, icon::AltText, 3, 0, "Z1", "Accessibility"},
    {CmdFormatObject, 0, icon::AltText, L, 0, "AT", "Alt Text"},
};

// Print preview, as Excel 2007's Print Preview tab (Featherweight's preview is a mode of the tab).
constexpr Spec kPrintPreview[] = {
    {0, 0, icon::Print, 3, 0, "ZP", "Print"},
    {CmdPrint, 0, icon::Print, L, 0, "P", "Print"},
    {CmdPageSetup, 0, icon::Settings, L, 0, "S", "Page Setup"},
    {0, 0, icon::Page, 3, 0, "ZS", "Settings"},
    {CmdPrintWhatMenu, 0, icon::Page, M, kMenu, "A", "Print What"},
    {CmdScalingMenu, 0, icon::FullScreen, M, kMenu, "F", "Scaling"},
    {0, 0, g::Zoom, 3, 0, "ZZ", "Zoom"},
    {CmdZoomMenu, 0, g::Zoom, L, kMenu, "Q", "Zoom"},
    {0, 0, g::Next, 3, 0, "ZV", "Preview"},
    {CmdNextPage, 0, g::Next, M, 0, "N", "Next Page"},
    {CmdPrevPage, 0, g::Prev, M, 0, "V", "Previous Page"},
    {CmdPrintPreview, 0, icon::Cancel, L, 0, "C", "Close Print Preview"},
};

// A menu's lines: cmd 0 is a separator; `radio` marks a choice among several.
struct Line {
  uint16_t cmd;
  const char* label;  // UTF-8, with the menu's & mnemonic
  bool radio = false;
};

struct Menu {
  int which;
  std::span<const Line> lines;
};

constexpr Line kPasteMenu[] = {{CmdPaste, "&Paste\tCtrl+V"},  {CmdPasteFormulas, "&Formulas"},
                               {CmdPasteTranspose, "&Transpose"}, {0, nullptr},
                               {CmdPasteValues, "&Values\tCtrl+Shift+V"}, {CmdPasteFormats, "Fo&rmatting"}};
constexpr Line kMergeMenu[] = {{CmdMergeCenter, "Merge && &Center"}, {CmdMergeAcross, "Merge &Across"},
                               {CmdMergeCells, "&Merge Cells"}, {CmdUnmerge, "&Unmerge Cells"}};
constexpr Line kInsertMenu[] = {{CmdInsertCells, "&Insert Cells…\tCtrl++"}, {CmdInsertRows, "Insert Sheet &Rows"},
                                {CmdInsertColumns, "Insert Sheet &Columns"}, {CmdInsertSheet, "Insert &Sheet\tShift+F11"}};
constexpr Line kDeleteMenu[] = {{CmdDeleteCells, "&Delete Cells…\tCtrl+-"}, {CmdDeleteRows, "Delete Sheet &Rows"},
                                {CmdDeleteColumns, "Delete Sheet &Columns"}, {CmdDeleteSheet, "Delete &Sheet"}};
constexpr Line kFillMenu[] = {{CmdFillDown, "&Down\tCtrl+D"}, {CmdFillRight, "&Right\tCtrl+R"}};
constexpr Line kClearMenu[] = {{CmdClearContents, "Clear &Contents\tDelete"}, {CmdRemoveLink, "&Remove Hyperlinks"}};
constexpr Line kSortMenu[] = {{CmdSortAscending, "&Sort A to Z"}, {CmdSortDescending, "S&ort Z to A"},
                              {CmdDataBox, "C&ustom Sort…"}, {0, nullptr}, {CmdFilter, "&Filter\tCtrl+Shift+L"}};
constexpr Line kFindMenu[] = {{CmdFind, "&Find…\tCtrl+F"}, {CmdReplace, "&Replace…\tCtrl+H"}, {CmdGoTo, "&Go To…\tCtrl+G"}};
constexpr Line kShapesMenu[] = {{CmdObjectRectangle, "&Rectangle"}, {CmdObjectRoundRect, "R&ounded Rectangle"},
                                {CmdObjectOval, "O&val"}, {CmdObjectLine, "&Line"}, {CmdObjectArrow, "&Arrow"}};
constexpr Line kColumnMenu[] = {{CmdChartColumn, "Clustered &Column\tAlt+F1"}, {CmdChartBar, "Clustered &Bar"}};
constexpr Line kLineMenu[] = {{CmdChartLine, "&Line"}, {CmdChartArea, "&Area"}};
constexpr Line kPieMenu[] = {{CmdChartPie, "&Pie"}, {CmdChartDoughnut, "&Doughnut"}};
constexpr Line kMarginsMenu[] = {{CmdMarginsNormal, "&Normal", true}, {CmdMarginsWide, "&Wide", true},
                                 {CmdMarginsNarrow, "N&arrow", true}, {0, nullptr}, {CmdPageSetup, "Custom M&argins…"}};
constexpr Line kOrientationMenu[] = {{CmdPortrait, "&Portrait", true}, {CmdLandscape, "&Landscape", true}};
constexpr Line kPrintAreaMenu[] = {{CmdSetPrintArea, "&Set Print Area"}, {CmdAddToPrintArea, "&Add to Print Area"},
                                   {CmdClearPrintArea, "&Clear Print Area"}};
constexpr Line kBreaksMenu[] = {{CmdInsertPageBreak, "&Insert Page Break"}, {CmdRemovePageBreak, "&Remove Page Break"},
                                {CmdResetPageBreaks, "Re&set All Page Breaks"}};
constexpr Line kRefreshMenu[] = {{CmdPivotRefreshAll, "Refresh &All\tCtrl+Alt+F5"}, {CmdPivotRefresh, "&Refresh\tAlt+F5"}};
constexpr Line kValidationMenu[] = {{CmdDataValidation, "Data &Validation…"}, {CmdCircleInvalid, "C&ircle Invalid Data"},
                                    {CmdClearCircles, "Clear Validation Ci&rcles"}};
constexpr Line kNotesMenu[] = {{CmdNewNote, "&New Note\tShift+F2"}, {CmdShowNote, "&Show/Hide Note"},
                               {CmdShowAllNotes, "Show &All Notes"}, {CmdDeleteNote, "&Delete Note"}};
constexpr Line kFreezeMenu[] = {{CmdFreezePanes, "&Freeze Panes"}, {CmdFreezeTopRow, "Freeze Top &Row"},
                                {CmdFreezeFirstColumn, "Freeze First &Column"}};
constexpr Line kPrintWhatMenu[] = {{CmdPrintActiveSheet, "Print &Active Sheets", true},
                                   {CmdPrintWorkbook, "Print &Entire Workbook", true},
                                   {CmdPrintSelection, "Print &Selection", true}};
constexpr Line kScalingMenu[] = {{CmdNoScaling, "&No Scaling", true},
                                 {CmdFitSheetOnePage, "&Fit Sheet on One Page", true},
                                 {CmdFitColumnsOnePage, "Fit All &Columns on One Page", true},
                                 {CmdFitRowsOnePage, "Fit All &Rows on One Page", true}};
constexpr Line kCondMenu[] = {{CmdCondFormat, "&New Rule…"}, {0, nullptr}, {CmdCfClearSelection, "Clear Rules from &Selected Cells"},
                              {CmdCfClearSheet, "Clear Rules from &Entire Sheet"}, {0, nullptr}, {CmdCfManage, "Manage &Rules…"}};

constexpr Menu kMenus[] = {
    {CmdPaste, kPasteMenu},           {CmdMergeCenter, kMergeMenu},       {CmdInsertCells, kInsertMenu},
    {CmdDeleteCells, kDeleteMenu},    {CmdFillMenu, kFillMenu},           {CmdClearMenu, kClearMenu},
    {CmdSortFilterMenu, kSortMenu},   {CmdFindSelectMenu, kFindMenu},     {CmdShapesMenu, kShapesMenu},
    {CmdColumnChartMenu, kColumnMenu}, {CmdLineChartMenu, kLineMenu},     {CmdPieChartMenu, kPieMenu},
    {CmdMarginsMenu, kMarginsMenu},   {CmdOrientationMenu, kOrientationMenu}, {CmdPrintAreaMenu, kPrintAreaMenu},
    {CmdBreaksMenu, kBreaksMenu},     {CmdPivotRefreshAll, kRefreshMenu}, {CmdPivotRefresh, kRefreshMenu},
    {CmdDataValidation, kValidationMenu}, {CmdNotesMenu, kNotesMenu},     {CmdFreezeMenu, kFreezeMenu},
    {CmdPrintWhatMenu, kPrintWhatMenu}, {CmdScalingMenu, kScalingMenu},   {CmdCondFormatBox, kCondMenu},
};

// Labels of the commands whose meaning turns around (Freeze or Unfreeze).
const char* Turned(int cmd, const CommandState& q) {
  if (cmd == CmdFreezePanes && q.label.rfind(L"Unfreeze", 0) == 0) return "Un&freeze Panes";
  if (cmd == CmdNewNote && q.label.rfind(L"Edit", 0) == 0) return "&Edit Note\tShift+F2";
  return nullptr;
}

#if FW_TEST_SCREENS
// ribbon:dump's lines for a menu and its submenus.
void DumpMenu(HMENU menu, int depth) {
  for (int i = 0; i < GetMenuItemCount(menu); ++i) {
    wchar_t text[128] = {};
    MENUITEMINFOW mi{sizeof mi, MIIM_ID | MIIM_STRING | MIIM_SUBMENU | MIIM_STATE | MIIM_FTYPE};
    mi.dwTypeData = text;
    mi.cch = 127;
    GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &mi);
    if (mi.fType & MFT_SEPARATOR) continue;
    fwprintf(stderr, L"ribbon: %*smenu %u %s%s%s\n", depth * 2, L"", mi.hSubMenu ? 0 : mi.wID, text,
             mi.fState & MFS_DISABLED ? L" (off)" : L"", mi.fState & MFS_CHECKED ? L" (on)" : L"");
    if (mi.hSubMenu) DumpMenu(mi.hSubMenu, depth + 1);
  }
}
#endif

std::wstring Hex(const D2D1_COLOR_F& c) {
  auto b = [](float x) { return static_cast<unsigned>(std::lround(std::clamp(x, 0.0f, 1.0f) * 255)); };
  wchar_t buf[8];
  std::swprintf(buf, 8, L"%02X%02X%02X", b(c.r), b(c.g), b(c.b));
  return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// The tabs

void WorkbookView::BuildRibbon(RibbonModel& m, const ToolbarContext&) {
  if (!Ready()) return;
  auto add = [&](const wchar_t* name, const wchar_t* key, std::span<const Spec> specs, bool contextual = false) {
    RibbonTab& tab = m.tabs.emplace_back();
    tab.name = name;
    tab.keyTip = key;
    tab.contextual = contextual;
    for (const Spec& s : specs) {
      if (s.cmd == 0) {
        RibbonGroup& group = tab.groups.emplace_back();
        group.name = Utf8ToWide(s.label);
        group.keyTip = Utf8ToWide(s.key);
        group.glyph = s.glyph;
        group.rows = s.size;
        group.launcher = s.launcher;
        group.shrinkFirst = s.flags;  // a group's flags: how early it shrinks
        continue;
      }
      RibbonItem& it = tab.groups.back().items.emplace_back();
      ToolItem& t = it.tool;
      if (s.flags & kTool) {
        t = RibbonTool(s.cmd);
      } else {
        const CommandState q = QueryCommand(s.cmd);
        t.cmd = s.cmd;
        t.enabled = q.enabled;
        // Pressed only for toggles and Merge & Center (Close Print Preview's command is "checked" in preview).
        t.checked = q.checked && (s.flags & (kToggle | kSplit));
        t.toggle = (s.flags & kToggle) != 0;
        t.dropdown = (s.flags & kMenu) != 0;
        if (s.flags & kSplit) t.kind = ToolKind::SplitButton;
        // Labels that turn around with the selection (Unprotect Sheet).
        if (s.cmd == CmdProtectSheet && q.label.rfind(L"Unprotect", 0) == 0) it.label = L"Unprotect Sheet";
      }
      if (s.glyph) t.glyph = s.glyph;
      it.size = static_cast<RibbonSize>(s.size);
      // Large items go to medium before their group folds, as Excel's Cells and Styles; others to small.
      it.smallest = s.flags & kKeepLarge ? RibbonSize::Large : s.size == L ? RibbonSize::Medium : RibbonSize::Small;
      if (it.label.empty() && !(s.flags & kNoLabel)) it.label = Utf8ToWide(s.label);
      it.keyTip = Utf8ToWide(s.key);
      it.sameRow = (s.flags & kSameRow) != 0;
      it.newColumn = (s.flags & kNewColumn) != 0;
    }
  };
  add(L"Home", L"H", kHome);
  add(L"Insert", L"N", kInsert);
  add(L"Page Layout", L"P", kPageLayout);
  add(L"Formulas", L"M", kFormulas);
  add(L"Data", L"A", kData);
  add(L"Review", L"R", kReview);
  add(L"View", L"W", kView);
  // Quick access: Undo and Redo with their lists; picking a step takes it back (or does it again)
  // with every newer one, as Excel's.
  if (edit_ && !preview_)
    for (const int cmd : {CmdUndo, CmdRedo}) {
      ToolItem& t = m.quick.emplace_back();
      t.cmd = cmd;
      for (const std::u16string& name : cmd == CmdUndo ? edit_->UndoNames() : edit_->RedoNames()) {
        if (t.choices.size() >= 30) break;
        t.choices.emplace_back(name.begin(), name.end());
      }
      t.onChange = [this, cmd](const ToolValue& v) {
        if (v.index < 0 || !edit_ || editor_.Active()) return;
        const size_t steps = static_cast<size_t>(v.index) + 1;
        EditRefused(cmd == CmdUndo ? edit_->Undo(steps) : edit_->Redo(steps));
      };
    }
  // Contextual tabs: Excel's, for what's selected.
  if (preview_) {
    add(L"Print Preview", L"JV", kPrintPreview, true);
    m.show = L"Print Preview";
    return;
  }
  const std::optional<SheetObjects::Object> object = SelectedObject();
  const bool table = !object && !ChartSheet() && TableIndexAt(active_) >= 0;
  // An object or table just inserted shows its tab, as in Office; selecting one later doesn't.
  if (object ? object->id != ribbonInserted_ : !table || ribbonInserted_ != kRibbonTable) ribbonInserted_ = 0;
  if (ChartSheet() || (object && object->chart)) {
    add(L"Chart Design", L"JC", kChartDesign, true);
  } else if (object) {
    add(object->picture ? L"Picture Format" : L"Shape Format", object->picture ? L"JP" : L"JD",
        object->picture ? std::span<const Spec>(kPictureFormat) : std::span<const Spec>(kShapeFormat), true);
  } else if (table) {
    add(L"Table Design", L"JT", kTableDesign, true);
  } else if (const auto* looks = PivotLooksFor(sheet_)) {
    for (const pivot::Look& l : *looks)
      if (l.area.Contains(active_.row, active_.col)) {
        add(L"PivotTable Analyze", L"JT", kPivotAnalyze, true);
        break;
      }
  }
  if (ribbonInserted_ && m.tabs.back().contextual) m.show = m.tabs.back().name;
}

// The boxes, color buttons and dropdowns that set something of their own.
ToolItem WorkbookView::RibbonTool(int cmd) {
  ToolItem t;
  switch (cmd) {
    case CmdScaleWidth:
    case CmdScaleHeight: {
      // Excel's Width and Height boxes, with the two values the scaling commands set: Automatic or
      // 1 page; together they pick No scaling, Fit sheet, Fit all columns or Fit all rows.
      t.cmd = cmd;
      t.kind = ToolKind::Dropdown;
      t.width = 104;  // "Automatic" in full
      t.enabled = QueryCommand(cmd).enabled;
      t.choices = {L"Automatic", L"1 page"};
      const int now = ScalingNow();
      const bool wide = now == 1 || now == 2, tall = now == 1 || now == 3;
      t.choice = (cmd == CmdScaleWidth ? wide : tall) ? 1 : 0;
      t.text = t.choices[static_cast<size_t>(t.choice)];
      t.onChange = [this, cmd, wide, tall](const ToolValue& v) {
        if (v.index < 0) return;
        const bool w = cmd == CmdScaleWidth ? v.index == 1 : wide, h = cmd == CmdScaleHeight ? v.index == 1 : tall;
        Execute(w && h ? CmdFitSheetOnePage : w ? CmdFitColumnsOnePage : h ? CmdFitRowsOnePage : CmdNoScaling);
      };
      return t;
    }
    case CmdObjectFill:
    case CmdObjectOutline: {
      // The selected picture's or shape's fill and outline, as its Format panel sets them.
      t.cmd = cmd;
      t.kind = ToolKind::ColorPicker;
      t.width = 52;
      t.enabled = QueryCommand(cmd).enabled;
      t.automaticLabel = cmd == CmdObjectFill ? L"No Fill" : L"No Outline";
      t.automaticColor = D2D1_COLOR_F{0, 0, 0, 0};
      t.onChange = [this, cmd](const ToolValue& v) {
        ShapesScriptOp((cmd == CmdObjectFill ? L"fill," : L"outline,") + (v.automatic ? std::wstring(L"none") : Hex(v.color)));
        host_.ViewStateChanged();
      };
      return t;
    }
    case CmdCellStyles:
      t = FormatTool(cmd);
      t.text.clear();  // a Large button with its glyph and label, the styles in its list
      return t;
    default: return FormatTool(cmd);
  }
}

// ---------------------------------------------------------------------------
// The menus

bool WorkbookView::FillRibbonMenu(int which, HMENU menu) {
  if (which == CmdCellsFormatMenu) {
    // Excel's Format ▾: cell size, visibility, organize sheets, protection.
    auto item = [&](HMENU into, int cmd, const wchar_t* label) {
      const CommandState q = QueryCommand(cmd);
      menu::AppendItem(into, static_cast<UINT>(cmd), label, q.enabled, q.checked);
    };
    item(menu, CmdRowHeight, L"Row &Height…");
    item(menu, CmdAutoFitRowHeight, L"&AutoFit Row Height");
    item(menu, CmdColumnWidth, L"Column &Width…");
    item(menu, CmdAutoFitColumnWidth, L"AutoF&it Column Width");
    menu::AppendSeparator(menu);
    HMENU hide = CreatePopupMenu();
    item(hide, CmdHide, L"&Hide Rows or Columns");
    item(hide, CmdUnhide, L"&Unhide Rows or Columns");
    item(hide, CmdHideSheet, L"Hide &Sheet");
    AppendUnhideSheetMenu(hide, L"Unhide S&heet");
    menu::AppendSubmenu(menu, hide, L"Hide && &Unhide");
    menu::AppendSeparator(menu);
    item(menu, CmdRenameSheet, L"&Rename Sheet");
    item(menu, CmdMoveSheetLeft, L"Move Sheet &Left");
    item(menu, CmdMoveSheetRight, L"Move Sheet Ri&ght");
    AppendTabColorMenu(menu, L"&Tab Color");
    menu::AppendSeparator(menu);
    const CommandState protect = QueryCommand(CmdProtectSheet);
    menu::AppendItem(menu, CmdProtectSheet, protect.label.rfind(L"Unprotect", 0) == 0 ? L"Un&protect Sheet…" : L"&Protect Sheet…",
                     protect.enabled);
    item(menu, CmdFormatCells, L"Format C&ells…\tCtrl+1");
    return true;
  }
  const Menu* found = nullptr;
  for (const Menu& m : kMenus)
    if (m.which == which) found = &m;
  if (!found) return false;
  const int scaling = which == CmdScalingMenu ? ScalingNow() : -1;
  const int margins = which == CmdMarginsMenu ? MarginsNow() : -1;
  for (const Line& l : found->lines) {
    if (l.cmd == 0) {
      menu::AppendSeparator(menu);
      continue;
    }
    const CommandState q = QueryCommand(l.cmd);
    bool enabled = q.enabled, checked = q.checked;
    if (l.cmd == CmdFind || l.cmd == CmdReplace) enabled = l.cmd == CmdFind || CanReplace();  // the window's
    if (which == CmdScalingMenu) checked = scaling == l.cmd - CmdNoScaling;
    if (which == CmdMarginsMenu && l.radio) checked = margins == (l.cmd == CmdMarginsNormal ? 0 : l.cmd == CmdMarginsWide ? 1 : 2);
    const char* turned = Turned(l.cmd, q);
    menu::AppendItem(menu, static_cast<UINT>(l.cmd), Utf8ToWide(turned ? turned : l.label), enabled, checked, l.radio);
  }
  return true;
}

// ribbon:dump: the model as built now, each item with its command, state and KeyTip, and the lines
// of each menu it opens (FillMenu), for checks (stderr, "ribbon: ..."). Test builds only (FW_TEST_SCREENS).
bool WorkbookView::RibbonScriptOp(const std::wstring& arg) {
#if !FW_TEST_SCREENS
  (void)arg;
  return false;
#else
  if (arg != L"dump") return false;
  RibbonModel m;
  BuildRibbon(m, ToolbarContext{});
  for (const RibbonTab& t : m.tabs) {
    fwprintf(stderr, L"ribbon: tab %s [%s]%s\n", t.name.c_str(), t.keyTip.c_str(), t.contextual ? L" contextual" : L"");
    for (const RibbonGroup& g : t.groups) {
      fwprintf(stderr, L"ribbon:  group %s [%s] launcher %d\n", g.name.c_str(), g.keyTip.c_str(), g.launcher);
      for (const RibbonItem& it : g.items) {
        fwprintf(stderr, L"ribbon:   item %d %s [%s] size %d%s%s%s\n", it.tool.cmd, it.label.c_str(), it.keyTip.c_str(),
                 static_cast<int>(it.size), it.tool.enabled ? L"" : L" (off)", it.tool.checked ? L" (on)" : L"",
                 it.tool.text.empty() ? L"" : (L" \"" + it.tool.text + L"\"").c_str());
        if (it.tool.dropdown || (it.tool.kind == ToolKind::SplitButton && it.tool.choices.empty())) {
          HMENU menu = CreatePopupMenu();
          FillMenu(it.tool.cmd, menu);
          DumpMenu(menu, 2);
          DestroyMenu(menu);
        }
      }
    }
  }
  if (!m.show.empty()) fwprintf(stderr, L"ribbon: show %s\n", m.show.c_str());
  return true;
#endif
}

// ---------------------------------------------------------------------------
// The ribbon's own commands: its menus, and what Excel's buttons do that only a panel did

bool WorkbookView::RibbonCommandState(int cmd, CommandState& s) {
  if (cmd < CmdFormatCellsFont && cmd != CmdZoomMenu && cmd != CmdMarginsNormal && cmd != CmdMarginsWide &&
      cmd != CmdMarginsNarrow && cmd != CmdPortrait && cmd != CmdLandscape && cmd != CmdObjectFill && cmd != CmdObjectOutline)
    return false;  // most commands: not the ribbon's (QueryCommand asks here first)
  const bool grid = Ready() && !preview_, cells = grid && !ChartSheet();
  auto set = [&](const wchar_t* label, bool enabled, bool checked = false) {
    s.label = label;
    s.enabled = enabled;
    s.checked = enabled && checked;
    return true;
  };
  switch (cmd) {
    case CmdCellsFormatMenu: return set(L"Format", grid);
    case CmdFillMenu: return set(L"Fill", cells);
    case CmdClearMenu: return set(L"Clear", cells);
    case CmdSortFilterMenu: return set(L"Sort & Filter", cells);
    case CmdFindSelectMenu: return set(L"Find & Select", grid);
    case CmdShapesMenu: return set(L"Shapes", QueryCommand(CmdObjectRectangle).enabled);
    case CmdColumnChartMenu: return set(L"Insert column or bar chart", QueryCommand(CmdChartColumn).enabled);
    case CmdLineChartMenu: return set(L"Insert line or area chart", QueryCommand(CmdChartLine).enabled);
    case CmdPieChartMenu: return set(L"Insert pie or doughnut chart", QueryCommand(CmdChartPie).enabled);
    case CmdMarginsMenu: return set(L"Margins", Ready());
    case CmdOrientationMenu: return set(L"Orientation", Ready());
    case CmdPrintAreaMenu: return set(L"Print area", cells);
    case CmdBreaksMenu: return set(L"Breaks", cells);
    case CmdScaleWidth: return set(L"Scale to fit: width", Ready() && !ChartSheet());
    case CmdScaleHeight: return set(L"Scale to fit: height", Ready() && !ChartSheet());
    case CmdNotesMenu: return set(L"Notes", cells);
    case CmdFreezeMenu: return set(L"Freeze panes", cells);
    case CmdPrintWhatMenu: return set(L"Print what", Ready());
    case CmdScalingMenu: return set(L"Scaling", Ready() && !ChartSheet());
    case CmdZoomMenu: return set(L"Zoom", Ready());
    case CmdViewNormal: return set(L"Normal view", Ready(), !preview_ && !BreakPreviewOn());
    case CmdMarginsNormal: return set(L"Normal margins", Ready(), MarginsNow() == 0);
    case CmdMarginsWide: return set(L"Wide margins", Ready(), MarginsNow() == 1);
    case CmdMarginsNarrow: return set(L"Narrow margins", Ready(), MarginsNow() == 2);
    case CmdPortrait:
    case CmdLandscape: {
      const bool landscape = Ready() && Current().Page().orientation == Orientation::Landscape;
      return set(cmd == CmdPortrait ? L"Portrait" : L"Landscape", Ready(), landscape == (cmd == CmdLandscape));
    }
    case CmdObjectFill:
    case CmdObjectOutline: {
      const std::optional<SheetObjects::Object> object = grid ? SelectedObject() : std::nullopt;
      const bool shape = object && !object->chart;
      return set(cmd == CmdObjectFill ? L"Shape fill" : object && object->picture ? L"Picture border" : L"Shape outline", shape);
    }
    default: return false;
  }
}

bool WorkbookView::RibbonCommand(int cmd) {
  CommandState s;
  if (!RibbonCommandState(cmd, s)) return false;
  if (!s.enabled) return true;
  switch (cmd) {
    case CmdViewNormal:
      if (preview_) TogglePreview();
      if (BreakPreviewOn()) TogglePageBreakPreview();
      break;
    case CmdMarginsNormal: PageScriptOp(L"margins=normal"); break;
    case CmdMarginsWide: PageScriptOp(L"margins=wide"); break;
    case CmdMarginsNarrow: PageScriptOp(L"margins=narrow"); break;
    case CmdPortrait: PageScriptOp(L"orientation=portrait"); break;
    case CmdLandscape: PageScriptOp(L"orientation=landscape"); break;
    case CmdObjectFill:
    case CmdObjectOutline: ShowShapePanel(); break;
    case CmdZoomMenu: return false;  // the window's (its menu: FillMenu)
    default: break;                  // a menu: the ribbon opens it
  }
  host_.ViewStateChanged();
  return true;
}

}  // namespace fw::sheet::view
