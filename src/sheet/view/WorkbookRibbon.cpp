// The workbook tab's ribbon (R1, r1-sheet; plan §5.6): Excel 365's tabs, groups and buttons, each on
// one of the tab's commands. The tables follow Excel's own ribbon as UI Automation reads it (build
// 20326, classic ribbon at full width: names, order, sizes and KeyTips); Excel's buttons that
// Featherweight has no command for are left out. The layout and the reasons are in
// FeatherweightNotes\r1\r1-sheet-state.md.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <utility>

#include "Commands.h"
#include "core/Lang.h"
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
  UiText8 label;  // UTF-8
};

constexpr Spec kHome[] = {
    {0, 0, g::Paste, 3, 0, "ZL", N_("Clipboard")},
    {CmdPaste, 0, g::Paste, L, kSplit | kKeepLarge, "V", N_("Paste")},
    {CmdCut, 0, g::Cut, M, 0, "X", N_("Cut")},
    {CmdCopy, 0, icon::Copy, M, 0, "C", N_("Copy")},
    {CmdFormatPainter, 0, icon::Brush, M, kToggle, "FP", N_("Format Painter")},
    {0, CmdFormatCellsFont, icon::FontColor, 2, 0, "ZF", N_("Font")},
    {CmdFontBox, 0, 0, M, kTool | kNoLabel, "FF", N_("Font")},
    {CmdSizeBox, 0, 0, M, kTool | kSameRow | kNoLabel, "FS", N_("Font Size")},
    {CmdGrowFont, 0, g::FontUp, S, kSameRow, "FG", N_("Increase Font Size")},
    {CmdShrinkFont, 0, g::FontDown, S, kSameRow, "FK", N_("Decrease Font Size")},
    {CmdBold, 0, icon::Bold, S, kToggle, "1", N_("Bold")},
    {CmdItalic, 0, icon::Italic, S, kToggle | kSameRow, "2", N_("Italic")},
    {CmdUnderline, 0, icon::Underline, S, kToggle | kSameRow, "3", N_("Underline")},
    {CmdBordersBox, 0, icon::BordersBottom, S, kTool | kSameRow, "B", N_("Borders")},
    {CmdFillColor, 0, icon::FillBucket, S, kTool | kSameRow, "H", N_("Fill Color")},
    {CmdFontColor, 0, icon::FontColor, S, kTool | kSameRow, "FC", N_("Font Color")},
    {0, CmdFormatCellsAlignment, icon::AlignLeft, 2, 0, "ZA", N_("Alignment")},
    {CmdAlignTop, 0, icon::AlignTop, S, kToggle, "AT", N_("Top Align")},
    {CmdAlignMiddle, 0, icon::AlignMiddle, S, kToggle | kSameRow, "AM", N_("Middle Align")},
    {CmdAlignBottom, 0, icon::AlignBottom, S, kToggle | kSameRow, "AB", N_("Bottom Align")},
    {CmdAlignLeft, 0, icon::AlignLeft, S, kToggle, "AL", N_("Align Left")},
    {CmdAlignCenter, 0, icon::AlignCenter, S, kToggle | kSameRow, "AC", N_("Center")},
    {CmdAlignRight, 0, icon::AlignRight, S, kToggle | kSameRow, "AR", N_("Align Right")},
    {CmdOutdent, 0, icon::DecreaseIndent, S, kSameRow, "5", N_("Decrease Indent")},
    {CmdIndent, 0, icon::IncreaseIndent, S, kSameRow, "6", N_("Increase Indent")},
    {CmdWrapText, 0, icon::WrapCell, M, kToggle | kNewColumn, "W", N_("Wrap Text")},
    {CmdMergeCenter, 0, icon::MergeCenter, M, kSplit, "M", N_("Merge & Center")},
    {0, CmdNumberFormatBox, icon::Percent, 2, 0, "ZN", N_("Number")},
    {CmdNumberFormatBox, 0, 0, M, kTool | kNoLabel, "N", N_("Number Format")},
    {CmdNumAccounting, 0, icon::Accounting, S, 0, "AN", N_("Accounting Number Format")},
    {CmdNumPercent, 0, icon::Percent, S, kSameRow, "P", N_("Percent Style")},
    {CmdCommaStyle, 0, icon::Comma, S, kSameRow, "K", N_("Comma Style")},
    {CmdIncreaseDecimal, 0, icon::IncreaseDecimal, S, kSameRow, "0", N_("Increase Decimal")},
    {CmdDecreaseDecimal, 0, icon::DecreaseDecimal, S, kSameRow, "9", N_("Decrease Decimal")},
    {0, 0, icon::Color, 3, 0, "ZS", N_("Styles")},
    {CmdCondFormatBox, 0, icon::ConditionalFormat, L, kMenu, "L", N_("Conditional Formatting")},
    {CmdCreateTable, 0, icon::Table, L, 0, "T", N_("Format as Table")},
    {CmdCellStyles, 0, icon::CellStyles, L, kTool, "J", N_("Cell Styles")},
    {0, 0, icon::Add, 3, 0, "ZC", N_("Cells")},
    {CmdInsertCells, 0, icon::InsertCells, L, kSplit, "I", N_("Insert")},
    {CmdDeleteCells, 0, icon::Delete, L, kSplit, "D", N_("Delete")},
    {CmdCellsFormatMenu, 0, icon::Settings, L, kMenu, "O", N_("Format")},
    {0, 0, icon::Search, 3, 0, "ZE", N_("Editing")},
    {CmdAutoSum, 0, icon::AutoSum, M, 0, "U", "AutoSum"},
    {CmdFillMenu, 0, g::Down, M, kMenu, "FI", N_("Fill")},
    {CmdClearMenu, 0, g::Eraser, M, kMenu, "E", N_("Clear")},
    {CmdSortFilterMenu, 0, g::Sort, L, kMenu, "S", N_("Sort & Filter")},
    {CmdFindSelectMenu, 0, icon::Search, L, kMenu, "FD", N_("Find & Select")},
};

constexpr Spec kInsert[] = {
    {0, 0, icon::Table, 3, 0, "ZB", N_("Tables")},
    {CmdPivotTable, 0, icon::PivotTable, L, 0, "V", "PivotTable"},
    {CmdCreateTable, 0, icon::Table, L, 0, "T", N_("Table")},
    {0, 0, icon::Picture, 3, 0, "ZI", N_("Illustrations")},
    {CmdObjectPicture, 0, icon::Picture, L, 0, "P", N_("Pictures")},
    {CmdShapesMenu, 0, icon::Shapes, L, kMenu, "SH", N_("Shapes")},
    {0, CmdInsertChart, icon::ChartColumn, 3, 0, "ZC", N_("Charts")},
    {CmdColumnChartMenu, 0, icon::ChartColumn, S, kMenu, "C", N_("Insert Column or Bar Chart")},
    {CmdLineChartMenu, 0, icon::ChartLine, S, kMenu, "N1", N_("Insert Line or Area Chart")},
    {CmdPieChartMenu, 0, icon::ChartPie, S, kMenu, "Q", N_("Insert Pie or Doughnut Chart")},
    {CmdChartScatter, 0, icon::ChartScatter, S, kNewColumn, "D", N_("Insert Scatter (X, Y) Chart")},
    {CmdChartCombo, 0, icon::ChartCombo, S, 0, "SD", N_("Insert Combo Chart")},
    {0, 0, icon::Link, 3, 0, "ZL", N_("Links")},
    {CmdInsertLink, 0, icon::Link, L, 0, "I2", N_("Link")},
    {0, 0, icon::TextBox, 3, 0, "ZT", N_("Text")},
    {CmdObjectTextBox, 0, icon::TextBox, L, 0, "X", N_("Text Box")},
};

constexpr Spec kPageLayout[] = {
    {0, CmdPageSetup, icon::Page, 3, 0, "ZS", N_("Page Setup")},
    {CmdMarginsMenu, 0, icon::Margins, L, kMenu, "M", N_("Margins")},
    {CmdOrientationMenu, 0, icon::Orientation, L, kMenu, "O", N_("Orientation")},
    {CmdPrintAreaMenu, 0, icon::PrintArea, L, kMenu, "R", N_("Print Area")},
    {CmdBreaksMenu, 0, icon::PageBreak, L, kMenu, "B", N_("Breaks")},
    {CmdPrintTitles, 0, icon::PrintTitles, L, 0, "I", N_("Print Titles")},
    {0, CmdPageSetup, icon::FullScreen, 3, 0, "ZC", N_("Scale to Fit")},
    {CmdScaleWidth, 0, 0, M, kTool, "W", N_("Width")},
    {CmdScaleHeight, 0, 0, M, kTool, "H", N_("Height")},
};

constexpr Spec kFormulas[] = {
    {0, 0, icon::AutoSum, 3, 0, "ZL", N_("Function Library")},
    {CmdAutoSum, 0, icon::AutoSum, L, 0, "U", "AutoSum"},
    {0, 0, g::Tag, 3, 0, "ZN", N_("Defined Names")},
    {CmdNameManager, 0, g::Tag, L, 0, "N", N_("Name Manager")},
    {CmdDefineName, 0, g::Pencil, M, 0, "M", N_("Define Name")},
    {CmdPasteNames, 0, g::Tag, M, 0, "S", N_("Use in Formula")},
    {CmdCreateNames, 0, g::Select, M, 0, "C", N_("Create from Selection")},
    {0, 0, g::Calculator, 2, 0, "ZC", N_("Calculation")},
    {CmdCalculateNow, 0, g::Calculator, M, 0, "B", N_("Calculate Now")},
    {CmdCalculateSheet, 0, g::Sync, M, 0, "J", N_("Calculate Sheet")},
};

constexpr Spec kData[] = {
    {0, 0, g::Sync, 3, 0, "ZC", N_("Queries & Connections")},
    {CmdPivotRefreshAll, 0, g::Sync, L, kSplit, "R", N_("Refresh All")},
    {0, 0, g::Sort, 2, 0, "ZS", N_("Sort & Filter")},
    {CmdSortAscending, 0, icon::SortAscending, S, 0, "SA", N_("Sort A to Z")},
    {CmdSortDescending, 0, icon::SortDescending, S, 0, "SD", N_("Sort Z to A")},
    {CmdDataBox, 0, g::Sort, L, 0, "SS", N_("Sort")},
    {CmdFilter, 0, g::Filter, L, kToggle, "T", N_("Filter")},
    {0, 0, icon::RemoveDuplicates, 3, 0, "ZD", N_("Data Tools")},
    {CmdRemoveDuplicates, 0, icon::RemoveDuplicates, L, 0, "M", N_("Remove Duplicates")},
    {CmdDataValidation, 0, icon::DropDownList, L, kSplit, "V", N_("Data Validation")},
    {0, 0, icon::Add, 3, 0, "ZO", N_("Outline")},
    {CmdGroup, 0, icon::GroupRows, L, 0, "G", N_("Group")},
    {CmdUngroup, 0, icon::UngroupRows, L, 0, "U", N_("Ungroup")},
};

constexpr Spec kReview[] = {
    {0, 0, icon::Comment, 3, 0, "ZC", N_("Comments")},
    {CmdNewComment, 0, icon::Comment, L, 0, "C", N_("New Comment")},
    {CmdDeleteThread, 0, icon::Delete, L, 0, "D", N_("Delete")},
    {0, 0, g::Note, 3, 0, "ZN", N_("Notes")},
    {CmdNotesMenu, 0, g::Note, L, kMenu, "T", N_("Notes")},
    {0, 0, icon::Lock, 3, 0, "ZH", N_("Protect")},
    {CmdProtectSheet, 0, icon::Lock, L, 0, "PS", N_("Protect Sheet")},
    {CmdProtectWorkbook, 0, g::Key, L, kToggle, "PW", N_("Protect Workbook")},
};

constexpr Spec kView[] = {
    {0, 0, icon::ViewNormal, 3, 0, "ZV", N_("Workbook Views")},
    {CmdViewNormal, 0, icon::ViewNormal, L, kToggle, "L", N_("Normal")},
    {CmdPageBreakPreview, 0, icon::PageBreakPreview, L, kToggle, "I", N_("Page Break Preview")},
    {0, 0, g::Zoom, 3, 0, "ZZ", N_("Zoom")},
    {CmdZoomMenu, 0, g::Zoom, L, kMenu, "Q", N_("Zoom")},
    {CmdZoomActual, 0, icon::Zoom100, L, 0, "J", "100%"},
    {0, 0, icon::FreezePanes, 3, 0, "ZW", N_("Window")},
    {CmdFreezeMenu, 0, icon::FreezePanes, L, kMenu, "F", N_("Freeze Panes")},
};

// Contextual tabs, while what they work on is selected.
constexpr Spec kTableDesign[] = {
    {0, 0, icon::Table, 3, 0, "ZT", N_("Tools")},
    {CmdPivotTable, 0, icon::PivotTable, M, 0, "V", N_("Summarize with PivotTable")},
    {CmdRemoveDuplicates, 0, icon::RemoveDuplicates, M, 0, "M", N_("Remove Duplicates")},
    {CmdConvertToRange, 0, icon::Table, M, 0, "G", N_("Convert to Range")},
    {0, 0, g::Check, 3, 0, "ZL", N_("Table Style Options")},
    {CmdTableTotalRow, 0, 0, M, kToggle, "T", N_("Total Row")},  // no glyph: a check box, as Excel's
    {CmdFilter, 0, 0, M, kToggle, "B", N_("Filter Button")},
    {0, 0, icon::Color, 3, 0, "ZS", N_("Table Styles")},
    {CmdTableDesign, 0, icon::Color, L, 0, "S", N_("Quick Styles")},
};

constexpr Spec kPivotAnalyze[] = {
    {0, 0, g::Sync, 3, 0, "ZD", N_("Data")},
    {CmdPivotRefresh, 0, g::Sync, L, kSplit, "F", N_("Refresh")},
};

constexpr Spec kChartDesign[] = {
    {0, 0, icon::Add, 3, 0, "ZL", N_("Chart Layouts")},
    {CmdChartPanel, 0, icon::Add, L, 0, "A", N_("Add Chart Element")},
    {0, 0, icon::Color, 3, 0, "ZS", N_("Chart Styles")},
    {CmdChartPanel, 0, icon::Color, L, 0, "H", N_("Change Colors")},
    {0, 0, icon::Table, 3, 0, "ZD", N_("Data")},
    {CmdChartPanel, 0, icon::Table, L, 0, "E", N_("Select Data")},
    {0, 0, icon::Report, 3, 0, "ZT", N_("Type")},
    {CmdChartPanel, 0, icon::Report, L, 0, "C", N_("Change Chart Type")},
    {0, 0, g::Window, 3, 0, "ZO", N_("Location")},
    {CmdMoveChart, 0, g::Window, L, 0, "V", N_("Move Chart")},
};

constexpr Spec kPictureFormat[] = {
    {0, CmdFormatObject, icon::Picture, 3, 0, "ZS", N_("Picture Styles")},
    {CmdObjectOutline, 0, g::Pencil, M, kTool, "SO", N_("Picture Border")},
    {0, 0, icon::AltText, 3, 0, "Z1", N_("Accessibility")},
    {CmdFormatObject, 0, icon::AltText, L, 0, "AT", N_("Alt Text")},
};

constexpr Spec kShapeFormat[] = {
    {0, 0, icon::Shapes, 3, 0, "ZH", N_("Insert Shapes")},
    {CmdShapesMenu, 0, icon::Shapes, L, kMenu, "SH", N_("Shapes")},
    {CmdObjectTextBox, 0, icon::TextBox, M, 0, "X", N_("Text Box")},
    {0, CmdFormatObject, icon::Color, 3, 0, "ZY", N_("Shape Styles")},
    {CmdObjectFill, 0, icon::Color, M, kTool, "SF", N_("Shape Fill")},
    {CmdObjectOutline, 0, g::Pencil, M, kTool, "SO", N_("Shape Outline")},
    {0, 0, icon::AltText, 3, 0, "Z1", N_("Accessibility")},
    {CmdFormatObject, 0, icon::AltText, L, 0, "AT", N_("Alt Text")},
};

// Print preview, as Excel 2007's Print Preview tab (Featherweight's preview is a mode of the tab).
constexpr Spec kPrintPreview[] = {
    {0, 0, icon::Print, 3, 0, "ZP", N_("Print")},
    {CmdPrint, 0, icon::Print, L, 0, "P", N_("Print")},
    {CmdPageSetup, 0, icon::Settings, L, 0, "S", N_("Page Setup")},
    {0, 0, icon::Page, 3, 0, "ZS", N_("Settings")},
    {CmdPrintWhatMenu, 0, icon::Page, M, kMenu, "A", N_("Print What")},
    {CmdScalingMenu, 0, icon::FullScreen, M, kMenu, "F", N_("Scaling")},
    {0, 0, g::Zoom, 3, 0, "ZZ", N_("Zoom")},
    {CmdZoomMenu, 0, g::Zoom, L, kMenu, "Q", N_("Zoom")},
    {0, 0, g::Next, 3, 0, "ZV", N_("Preview")},
    {CmdNextPage, 0, g::Next, M, 0, "N", N_("Next Page")},
    {CmdPrevPage, 0, g::Prev, M, 0, "V", N_("Previous Page")},
    {CmdPrintPreview, 0, icon::Cancel, L, 0, "C", N_("Close Print Preview")},
};

// A menu's lines: cmd 0 is a separator; `radio` marks a choice among several.
struct Line {
  uint16_t cmd;
  UiText8 label;  // UTF-8, with the menu's & mnemonic
  bool radio = false;
};

struct Menu {
  int which;
  std::span<const Line> lines;
};

constexpr Line kPasteMenu[] = {{CmdPaste, "&Paste\tCtrl+V"},  {CmdPasteFormulas, "&Formulas"},
                               {CmdPasteTranspose, "&Transpose"}, {0, nullptr},
                               {CmdPasteValues, "&Values\tCtrl+Shift+V"}, {CmdPasteFormats, "Fo&rmatting"}};
constexpr Line kMergeMenu[] = {{CmdMergeCenter, N_("Merge && &Center")}, {CmdMergeAcross, N_("Merge &Across")},
                               {CmdMergeCells, N_("&Merge Cells")}, {CmdUnmerge, N_("&Unmerge Cells")}};
constexpr Line kInsertMenu[] = {{CmdInsertCells, N_("&Insert Cells…\tCtrl++")}, {CmdInsertRows, N_("Insert Sheet &Rows")},
                                {CmdInsertColumns, N_("Insert Sheet &Columns")}, {CmdInsertSheet, N_("Insert &Sheet\tShift+F11")}};
constexpr Line kDeleteMenu[] = {{CmdDeleteCells, N_("&Delete Cells…\tCtrl+-")}, {CmdDeleteRows, N_("Delete Sheet &Rows")},
                                {CmdDeleteColumns, N_("Delete Sheet &Columns")}, {CmdDeleteSheet, N_("Delete &Sheet")}};
constexpr Line kFillMenu[] = {{CmdFillDown, "&Down\tCtrl+D"}, {CmdFillRight, "&Right\tCtrl+R"}};
constexpr Line kClearMenu[] = {{CmdClearContents, N_("Clear &Contents\tDelete")}, {CmdRemoveLink, N_("&Remove Hyperlinks")}};
constexpr Line kSortMenu[] = {{CmdSortAscending, N_("&Sort A to Z")}, {CmdSortDescending, N_("S&ort Z to A")},
                              {CmdDataBox, N_("C&ustom Sort…")}, {0, nullptr}, {CmdFilter, "&Filter\tCtrl+Shift+L"}};
constexpr Line kFindMenu[] = {{CmdFind, "&Find…\tCtrl+F"}, {CmdReplace, "&Replace…\tCtrl+H"}, {CmdGoTo, N_("&Go To…\tCtrl+G")}};
constexpr Line kShapesMenu[] = {{CmdObjectRectangle, "&Rectangle"}, {CmdObjectRoundRect, N_("R&ounded Rectangle")},
                                {CmdObjectOval, "O&val"}, {CmdObjectLine, "&Line"}, {CmdObjectArrow, "&Arrow"}};
constexpr Line kColumnMenu[] = {{CmdChartColumn, N_("Clustered &Column\tAlt+F1")}, {CmdChartBar, N_("Clustered &Bar")}};
constexpr Line kLineMenu[] = {{CmdChartLine, "&Line"}, {CmdChartArea, "&Area"}};
constexpr Line kPieMenu[] = {{CmdChartPie, "&Pie"}, {CmdChartDoughnut, "&Doughnut"}};
constexpr Line kMarginsMenu[] = {{CmdMarginsNormal, "&Normal", true}, {CmdMarginsWide, "&Wide", true},
                                 {CmdMarginsNarrow, "N&arrow", true}, {0, nullptr}, {CmdPageSetup, N_("Custom M&argins…")}};
constexpr Line kOrientationMenu[] = {{CmdPortrait, "&Portrait", true}, {CmdLandscape, "&Landscape", true}};
constexpr Line kPrintAreaMenu[] = {{CmdSetPrintArea, N_("&Set Print Area")}, {CmdAddToPrintArea, N_("&Add to Print Area")},
                                   {CmdClearPrintArea, N_("&Clear Print Area")}};
constexpr Line kBreaksMenu[] = {{CmdInsertPageBreak, N_("&Insert Page Break")}, {CmdRemovePageBreak, N_("&Remove Page Break")},
                                {CmdResetPageBreaks, N_("Re&set All Page Breaks")}};
constexpr Line kRefreshMenu[] = {{CmdPivotRefreshAll, N_("Refresh &All\tCtrl+Alt+F5")}, {CmdPivotRefresh, "&Refresh\tAlt+F5"}};
constexpr Line kValidationMenu[] = {{CmdDataValidation, N_("Data &Validation…")}, {CmdCircleInvalid, N_("C&ircle Invalid Data")},
                                    {CmdClearCircles, N_("Clear Validation Ci&rcles")}};
constexpr Line kNotesMenu[] = {{CmdNewNote, N_("&New Note\tShift+F2")}, {CmdShowNote, N_("&Show/Hide Note")},
                               {CmdShowAllNotes, N_("Show &All Notes")}, {CmdDeleteNote, N_("&Delete Note")}};
constexpr Line kFreezeMenu[] = {{CmdFreezePanes, N_("&Freeze Panes")}, {CmdFreezeTopRow, N_("Freeze Top &Row")},
                                {CmdFreezeFirstColumn, N_("Freeze First &Column")}};
constexpr Line kPrintWhatMenu[] = {{CmdPrintActiveSheet, N_("Print &Active Sheets"), true},
                                   {CmdPrintWorkbook, N_("Print &Entire Workbook"), true},
                                   {CmdPrintSelection, N_("Print &Selection"), true}};
constexpr Line kScalingMenu[] = {{CmdNoScaling, N_("&No Scaling"), true},
                                 {CmdFitSheetOnePage, N_("&Fit Sheet on One Page"), true},
                                 {CmdFitColumnsOnePage, N_("Fit All &Columns on One Page"), true},
                                 {CmdFitRowsOnePage, N_("Fit All &Rows on One Page"), true}};
constexpr Line kCondMenu[] = {{CmdCondFormat, N_("&New Rule…")}, {0, nullptr}, {CmdCfClearSelection, N_("Clear Rules from &Selected Cells")},
                              {CmdCfClearSheet, N_("Clear Rules from &Entire Sheet")}, {0, nullptr}, {CmdCfManage, N_("Manage &Rules…")}};

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
  if (cmd == CmdFreezePanes && q.label == T(L"Unfreeze panes")) return T("Un&freeze Panes");
  if (cmd == CmdNewNote && q.label == T(L"Edit note")) return T("&Edit Note\tShift+F2");
  return nullptr;
}

// Excel's KeyTips for the ↘ launchers, by group (the ribbon's own lead with the group's letter, and
// Number's clashed with its box, N).
const wchar_t* LauncherKey(const char* group) {
  // The groups by their English names (Spec::label.en): the keys don't change with the language.
  static constexpr std::pair<const char*, const wchar_t*> kKeys[] = {
      {"Font", L"FN"},       {"Alignment", L"FA"},    {"Number", L"FM"},         {"Charts", L"K"},  // lang: no
      {"Page Setup", L"SP"}, {"Scale to Fit", L"SC"}, {"Picture Styles", L"O"}, {"Shape Styles", L"O"}};  // lang: no
  for (const auto& [name, key] : kKeys)
    if (std::strcmp(name, group) == 0) return key;
  return L"";
}

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
        if (s.launcher) group.launcherKeyTip = LauncherKey(s.label.en);
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
        if (s.cmd == CmdProtectSheet && q.label == T(L"Unprotect sheet")) it.label = T(L"Unprotect Sheet");
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
  add(T(L"Home"), L"H", kHome);
  add(T(L"Insert"), L"N", kInsert);
  add(T(L"Page Layout"), L"P", kPageLayout);
  add(T(L"Formulas"), L"M", kFormulas);
  add(T(L"Data"), L"A", kData);
  add(T(L"Review"), L"R", kReview);
  add(T(L"View"), L"W", kView);
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
    add(T(L"Print Preview"), L"JV", kPrintPreview, true);
    m.show = T(L"Print Preview");
    return;
  }
  const std::optional<SheetObjects::Object> object = SelectedObject();
  const bool table = !object && !ChartSheet() && TableIndexAt(active_) >= 0;
  // An object or table just inserted shows its tab, as in Office; selecting one later doesn't.
  if (object ? object->id != ribbonInserted_ : !table || ribbonInserted_ != kRibbonTable) ribbonInserted_ = 0;
  if (ChartSheet() || (object && object->chart)) {
    add(T(L"Chart Design"), L"JC", kChartDesign, true);
  } else if (object) {
    add(object->picture ? T(L"Picture Format") : T(L"Shape Format"), object->picture ? L"JP" : L"JD",
        object->picture ? std::span<const Spec>(kPictureFormat) : std::span<const Spec>(kShapeFormat), true);
  } else if (table) {
    add(T(L"Table Design"), L"JT", kTableDesign, true);
  } else if (const auto* looks = PivotLooksFor(sheet_)) {
    for (const pivot::Look& l : *looks)
      if (l.area.Contains(active_.row, active_.col)) {
        add(T(L"PivotTable Analyze"), L"JT", kPivotAnalyze, true);
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
      t.choices = {T(L"Automatic"), T(L"1 page")};
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
      t.automaticLabel = cmd == CmdObjectFill ? T(L"No Fill") : T(L"No Outline");
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
    item(menu, CmdRowHeight, T(L"Row &Height…"));
    item(menu, CmdAutoFitRowHeight, T(L"&AutoFit Row Height"));
    item(menu, CmdColumnWidth, T(L"Column &Width…"));
    item(menu, CmdAutoFitColumnWidth, T(L"AutoF&it Column Width"));
    menu::AppendSeparator(menu);
    HMENU hide = CreatePopupMenu();
    item(hide, CmdHide, T(L"&Hide Rows or Columns"));
    item(hide, CmdUnhide, T(L"&Unhide Rows or Columns"));
    item(hide, CmdHideSheet, T(L"Hide &Sheet"));
    AppendUnhideSheetMenu(hide, T(L"Unhide S&heet"));
    menu::AppendSubmenu(menu, hide, T(L"Hide && &Unhide"));
    menu::AppendSeparator(menu);
    item(menu, CmdRenameSheet, T(L"&Rename Sheet"));
    item(menu, CmdMoveSheetLeft, T(L"Move Sheet &Left"));
    item(menu, CmdMoveSheetRight, T(L"Move Sheet Ri&ght"));
    AppendTabColorMenu(menu, T(L"&Tab Color"));
    menu::AppendSeparator(menu);
    const CommandState protect = QueryCommand(CmdProtectSheet);
    menu::AppendItem(menu, CmdProtectSheet, protect.label == T(L"Unprotect sheet") ? T(L"Un&protect Sheet…") : T(L"&Protect Sheet…"),
                     protect.enabled);
    item(menu, CmdFormatCells, T(L"Format C&ells…\tCtrl+1"));
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
    menu::AppendItem(menu, static_cast<UINT>(l.cmd), Utf8ToWide(turned ? turned : static_cast<const char*>(l.label)), enabled, checked, l.radio);
  }
  return true;
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
    case CmdCellsFormatMenu: return set(T(L"Format"), grid);
    case CmdFillMenu: return set(T(L"Fill", L"verb"), cells);
    case CmdClearMenu: return set(T(L"Clear"), cells);
    case CmdSortFilterMenu: return set(T(L"Sort & Filter"), cells);
    case CmdFindSelectMenu: return set(T(L"Find & Select"), grid);
    case CmdShapesMenu: return set(T(L"Shapes"), QueryCommand(CmdObjectRectangle).enabled);
    case CmdColumnChartMenu: return set(T(L"Insert column or bar chart"), QueryCommand(CmdChartColumn).enabled);
    case CmdLineChartMenu: return set(T(L"Insert line or area chart"), QueryCommand(CmdChartLine).enabled);
    case CmdPieChartMenu: return set(T(L"Insert pie or doughnut chart"), QueryCommand(CmdChartPie).enabled);
    case CmdMarginsMenu: return set(T(L"Margins"), Ready());
    case CmdOrientationMenu: return set(T(L"Orientation"), Ready());
    case CmdPrintAreaMenu: return set(T(L"Print area"), cells);
    case CmdBreaksMenu: return set(T(L"Breaks"), cells);
    case CmdScaleWidth: return set(T(L"Scale to fit: width"), Ready() && !ChartSheet());
    case CmdScaleHeight: return set(T(L"Scale to fit: height"), Ready() && !ChartSheet());
    case CmdNotesMenu: return set(T(L"Notes"), cells);
    case CmdFreezeMenu: return set(T(L"Freeze panes"), cells);
    case CmdPrintWhatMenu: return set(T(L"Print what"), Ready());
    case CmdScalingMenu: return set(T(L"Scaling"), Ready() && !ChartSheet());
    case CmdZoomMenu: return set(T(L"Zoom"), Ready());
    case CmdViewNormal: return set(T(L"Normal view"), Ready(), !preview_ && !BreakPreviewOn());
    case CmdMarginsNormal: return set(T(L"Normal margins"), Ready(), MarginsNow() == 0);
    case CmdMarginsWide: return set(T(L"Wide margins"), Ready(), MarginsNow() == 1);
    case CmdMarginsNarrow: return set(T(L"Narrow margins"), Ready(), MarginsNow() == 2);
    case CmdPortrait:
    case CmdLandscape: {
      const bool landscape = Ready() && Current().Page().orientation == Orientation::Landscape;
      return set(cmd == CmdPortrait ? T(L"Portrait") : T(L"Landscape"), Ready(), landscape == (cmd == CmdLandscape));
    }
    case CmdObjectFill:
    case CmdObjectOutline: {
      const std::optional<SheetObjects::Object> object = grid ? SelectedObject() : std::nullopt;
      const bool shape = object && !object->chart;
      return set(cmd == CmdObjectFill ? T(L"Shape fill") : object && object->picture ? T(L"Picture border") : T(L"Shape outline"), shape);
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
