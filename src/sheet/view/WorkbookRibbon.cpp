// The workbook tab's ribbon (R1, r1-sheet; plan §5.6): Excel 365's tabs, groups and buttons, each on
// one of the tab's commands. The tables follow Excel's own ribbon as UI Automation reads it (build
// 20326, classic ribbon at full width: names, order, sizes and KeyTips); Excel's buttons that
// Featherweight has no command for are left out. The layout and the reasons are in
// FeatherweightNotes\r1\r1-sheet-state.md.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <span>

#include "Commands.h"
#include "core/Strings.h"
#include "sheet/view/WorkbookView.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/RibbonModel.h"

namespace fw::sheet::view {

namespace {

// Segoe Fluent Icons (Segoe MDL2 Assets on Windows 10) code points the toolbar didn't use. Where
// the font has nothing like Excel's picture (borders, merge, charts, Σ), the nearest one stands in
// until ui/Icons.h draws one.
namespace g {
constexpr wchar_t Paste = 0xE77F, Cut = 0xE8C6, FontUp = 0xE8E8, FontDown = 0xE8E7, Sort = 0xE8CB, Filter = 0xE71C,
                  Sync = 0xE895, Calculator = 0xE8EF, Zoom = 0xE71E, Tag = 0xE8EC, Key = 0xE8D7, Down = 0xE74B,
                  Up = 0xE74A, Eraser = 0xE75C, Wrap = 0xE751, Percent = 0xE94C, Plus = 0xE948, Minus = 0xE949,
                  Pin = 0xE718, Note = 0xE70B, Pie = 0xEB05, Line = 0xEAFC, Area = 0xE9D2, Grid = 0xF0E2,
                  Pencil = 0xE70F, Check = 0xE930, Select = 0xE8B3, Window = 0xE8A7, Money = 0xE8C7, Case = 0xE8D2,
                  Equal = 0xE94E, Shapes = 0xF156, Next = 0xE76C, Prev = 0xE76B, List = 0xE8FD;
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
};

// One line of a tab's table: a group (cmd 0: its name, KeyTip, rows in `size`, ↘ launcher in
// `launcher`), or an item of the group above it.
struct Spec {
  int cmd;
  wchar_t glyph;
  uint8_t size;
  uint8_t flags;
  const wchar_t* label;
  const wchar_t* key;
  int launcher = 0;
};

constexpr Spec kHome[] = {
    {0, g::Paste, 3, 0, L"Clipboard", L"ZL"},
    {CmdPaste, g::Paste, L, kSplit, L"Paste", L"V"},
    {CmdCut, g::Cut, M, 0, L"Cut", L"X"},
    {CmdCopy, icon::Copy, M, 0, L"Copy", L"C"},
    {CmdFormatPainter, icon::Brush, M, kToggle, L"Format Painter", L"FP"},
    {0, icon::FontColor, 2, 0, L"Font", L"ZF", CmdFormatCellsFont},
    {CmdFontBox, 0, M, kTool, L"Font", L"FF"},
    {CmdSizeBox, 0, M, kTool | kSameRow, L"Font Size", L"FS"},
    {CmdGrowFont, g::FontUp, S, kSameRow, L"Increase Font Size", L"FG"},
    {CmdShrinkFont, g::FontDown, S, kSameRow, L"Decrease Font Size", L"FK"},
    {CmdBold, icon::Bold, S, kToggle, L"Bold", L"1"},
    {CmdItalic, icon::Italic, S, kToggle | kSameRow, L"Italic", L"2"},
    {CmdUnderline, icon::Underline, S, kToggle | kSameRow, L"Underline", L"3"},
    {CmdBordersBox, icon::Table, S, kTool | kSameRow, L"Borders", L"B"},
    {CmdFillColor, icon::Color, S, kTool | kSameRow, L"Fill Color", L"H"},
    {CmdFontColor, icon::FontColor, S, kTool | kSameRow, L"Font Color", L"FC"},
    {0, icon::AlignLeft, 2, 0, L"Alignment", L"ZA", CmdFormatCellsAlignment},
    {CmdAlignTop, g::Up, S, kToggle, L"Top Align", L"AT"},
    {CmdAlignMiddle, g::Equal, S, kToggle | kSameRow, L"Middle Align", L"AM"},
    {CmdAlignBottom, g::Down, S, kToggle | kSameRow, L"Bottom Align", L"AB"},
    {CmdAlignLeft, icon::AlignLeft, S, kToggle, L"Align Left", L"AL"},
    {CmdAlignCenter, icon::AlignCenter, S, kToggle | kSameRow, L"Center", L"AC"},
    {CmdAlignRight, icon::AlignRight, S, kToggle | kSameRow, L"Align Right", L"AR"},
    {CmdOutdent, icon::DecreaseIndent, S, kSameRow, L"Decrease Indent", L"5"},
    {CmdIndent, icon::IncreaseIndent, S, kSameRow, L"Increase Indent", L"6"},
    {CmdWrapText, g::Wrap, M, kToggle | kNewColumn, L"Wrap Text", L"W"},
    {CmdMergeCenter, icon::Table, M, kSplit, L"Merge & Center", L"M"},
    {0, g::Percent, 2, 0, L"Number", L"ZN", CmdNumberFormatBox},
    {CmdNumberFormatBox, 0, M, kTool, L"Number Format", L"N"},
    {CmdNumAccounting, g::Money, S, 0, L"Accounting Number Format", L"AN"},
    {CmdNumPercent, g::Percent, S, kSameRow, L"Percent Style", L"P"},
    {CmdCommaStyle, g::Case, S, kSameRow, L"Comma Style", L"K"},
    {CmdIncreaseDecimal, g::Plus, S, kSameRow, L"Increase Decimal", L"0"},
    {CmdDecreaseDecimal, g::Minus, S, kSameRow, L"Decrease Decimal", L"9"},
    {0, icon::Color, 3, 0, L"Styles", L"ZS"},
    {CmdCondFormatBox, g::Check, L, kMenu, L"Conditional Formatting", L"L"},
    {CmdCreateTable, icon::Table, L, 0, L"Format as Table", L"T"},
    {CmdCellStyles, g::Case, L, kTool, L"Cell Styles", L"J"},
    {0, icon::Add, 3, 0, L"Cells", L"ZC"},
    {CmdInsertCells, icon::Add, L, kSplit, L"Insert", L"I"},
    {CmdDeleteCells, icon::Delete, L, kSplit, L"Delete", L"D"},
    {CmdCellsFormatMenu, icon::Settings, L, kMenu, L"Format", L"O"},
    {0, icon::Search, 3, 0, L"Editing", L"ZE"},
    {CmdAutoSum, g::Calculator, M, 0, L"AutoSum", L"U"},
    {CmdFillMenu, g::Down, M, kMenu, L"Fill", L"FI"},
    {CmdClearMenu, g::Eraser, M, kMenu, L"Clear", L"E"},
    {CmdSortFilterMenu, g::Sort, L, kMenu, L"Sort & Filter", L"S"},
    {CmdFindSelectMenu, icon::Search, L, kMenu, L"Find & Select", L"FD"},
};

constexpr Spec kInsert[] = {
    {0, icon::Table, 3, 0, L"Tables", L"ZB"},
    {CmdPivotTable, g::Grid, L, 0, L"PivotTable", L"V"},
    {CmdCreateTable, icon::Table, L, 0, L"Table", L"T"},
    {0, icon::Picture, 3, 0, L"Illustrations", L"ZI"},
    {CmdObjectPicture, icon::Picture, L, 0, L"Pictures", L"P"},
    {CmdShapesMenu, g::Shapes, L, kMenu, L"Shapes", L"SH"},
    {0, icon::Report, 3, 0, L"Charts", L"ZC", CmdInsertChart},
    {CmdColumnChartMenu, icon::Report, S, kMenu, L"Insert Column or Bar Chart", L"C"},
    {CmdLineChartMenu, g::Line, S, kMenu, L"Insert Line or Area Chart", L"N1"},
    {CmdPieChartMenu, g::Pie, S, kMenu, L"Insert Pie or Doughnut Chart", L"Q"},
    {CmdChartScatter, g::Area, S, kNewColumn, L"Insert Scatter (X, Y) Chart", L"D"},
    {CmdChartCombo, icon::Report, S, 0, L"Insert Combo Chart", L"SD"},
    {0, icon::Link, 3, 0, L"Links", L"ZL"},
    {CmdInsertLink, icon::Link, L, 0, L"Link", L"I2"},
    {0, g::Case, 3, 0, L"Text", L"ZT"},
    {CmdObjectTextBox, g::Case, L, 0, L"Text Box", L"X"},
};

constexpr Spec kPageLayout[] = {
    {0, icon::Page, 3, 0, L"Page Setup", L"ZS", CmdPageSetup},
    {CmdMarginsMenu, icon::Page, L, kMenu, L"Margins", L"M"},
    {CmdOrientationMenu, icon::Rotate, L, kMenu, L"Orientation", L"O"},
    {CmdPrintAreaMenu, icon::Print, L, kMenu, L"Print Area", L"R"},
    {CmdBreaksMenu, icon::TwoPage, L, kMenu, L"Breaks", L"B"},
    {CmdPrintTitles, g::List, L, 0, L"Print Titles", L"I"},
    {0, icon::FullScreen, 3, 0, L"Scale to Fit", L"ZC", CmdPageSetup},
    {CmdScaleWidth, 0, M, kTool, L"Width", L"W"},
    {CmdScaleHeight, 0, M, kTool, L"Height", L"H"},
};

constexpr Spec kFormulas[] = {
    {0, g::Calculator, 3, 0, L"Function Library", L"ZL"},
    {CmdAutoSum, g::Calculator, L, 0, L"AutoSum", L"U"},
    {0, g::Tag, 3, 0, L"Defined Names", L"ZN"},
    {CmdNameManager, g::Tag, L, 0, L"Name Manager", L"N"},
    {CmdDefineName, g::Pencil, M, 0, L"Define Name", L"M"},
    {CmdPasteNames, g::Tag, M, 0, L"Use in Formula", L"S"},
    {CmdCreateNames, g::Select, M, 0, L"Create from Selection", L"C"},
    {0, g::Calculator, 2, 0, L"Calculation", L"ZC"},
    {CmdCalculateNow, g::Calculator, M, 0, L"Calculate Now", L"B"},
    {CmdCalculateSheet, g::Sync, M, 0, L"Calculate Sheet", L"J"},
};

constexpr Spec kData[] = {
    {0, g::Sync, 3, 0, L"Queries & Connections", L"ZC"},
    {CmdPivotRefreshAll, g::Sync, L, kSplit, L"Refresh All", L"R"},
    {0, g::Sort, 2, 0, L"Sort & Filter", L"ZS"},
    {CmdSortAscending, g::Up, S, 0, L"Sort A to Z", L"SA"},
    {CmdSortDescending, g::Down, S, 0, L"Sort Z to A", L"SD"},
    {CmdDataBox, g::Sort, L, 0, L"Sort", L"SS"},
    {CmdFilter, g::Filter, L, kToggle, L"Filter", L"T"},
    {0, g::Check, 3, 0, L"Data Tools", L"ZD"},
    {CmdRemoveDuplicates, icon::Delete, L, 0, L"Remove Duplicates", L"M"},
    {CmdDataValidation, g::Check, L, kSplit, L"Data Validation", L"V"},
    {0, icon::Add, 3, 0, L"Outline", L"ZO"},
    {CmdGroup, icon::Add, L, 0, L"Group", L"G"},
    {CmdUngroup, icon::Remove, L, 0, L"Ungroup", L"U"},
};

constexpr Spec kReview[] = {
    {0, icon::Comment, 3, 0, L"Comments", L"ZC"},
    {CmdNewComment, icon::Comment, L, 0, L"New Comment", L"C"},
    {CmdDeleteThread, icon::Delete, L, 0, L"Delete", L"D"},
    {0, g::Note, 3, 0, L"Notes", L"ZN"},
    {CmdNotesMenu, g::Note, L, kMenu, L"Notes", L"T"},
    {0, icon::Lock, 3, 0, L"Protect", L"ZH"},
    {CmdProtectSheet, icon::Lock, L, 0, L"Protect Sheet", L"PS"},
    {CmdProtectWorkbook, g::Key, L, kToggle, L"Protect Workbook", L"PW"},
};

constexpr Spec kView[] = {
    {0, icon::Table, 3, 0, L"Workbook Views", L"ZV"},
    {CmdViewNormal, icon::Table, L, kToggle, L"Normal", L"L"},
    {CmdPageBreakPreview, icon::Page, L, kToggle, L"Page Break Preview", L"I"},
    {0, g::Zoom, 3, 0, L"Zoom", L"ZZ"},
    {CmdZoomMenu, g::Zoom, L, kMenu, L"Zoom", L"Q"},
    {CmdZoomActual, icon::ZoomIn, L, 0, L"100%", L"J"},
    {0, g::Pin, 3, 0, L"Window", L"ZW"},
    {CmdFreezeMenu, g::Pin, L, kMenu, L"Freeze Panes", L"F"},
};

// Contextual tabs, while what they work on is selected.
constexpr Spec kTableDesign[] = {
    {0, icon::Table, 3, 0, L"Tools", L"ZT"},
    {CmdPivotTable, g::Grid, M, 0, L"Summarize with PivotTable", L"V"},
    {CmdRemoveDuplicates, icon::Delete, M, 0, L"Remove Duplicates", L"M"},
    {CmdConvertToRange, icon::Table, M, 0, L"Convert to Range", L"G"},
    {0, g::Check, 3, 0, L"Table Style Options", L"ZL"},
    {CmdTableTotalRow, g::Check, M, kToggle, L"Total Row", L"T"},
    {CmdFilter, g::Filter, M, kToggle, L"Filter Button", L"B"},
    {0, icon::Color, 3, 0, L"Table Styles", L"ZS"},
    {CmdTableDesign, icon::Color, L, 0, L"Quick Styles", L"S"},
};

constexpr Spec kPivotAnalyze[] = {
    {0, g::Sync, 3, 0, L"Data", L"ZD"},
    {CmdPivotRefresh, g::Sync, L, kSplit, L"Refresh", L"F"},
};

constexpr Spec kChartDesign[] = {
    {0, icon::Add, 3, 0, L"Chart Layouts", L"ZL"},
    {CmdChartPanel, icon::Add, L, 0, L"Add Chart Element", L"A"},
    {0, icon::Color, 3, 0, L"Chart Styles", L"ZS"},
    {CmdChartPanel, icon::Color, L, 0, L"Change Colors", L"H"},
    {0, icon::Table, 3, 0, L"Data", L"ZD"},
    {CmdChartPanel, icon::Table, L, 0, L"Select Data", L"E"},
    {0, icon::Report, 3, 0, L"Type", L"ZT"},
    {CmdChartPanel, icon::Report, L, 0, L"Change Chart Type", L"C"},
    {0, g::Window, 3, 0, L"Location", L"ZO"},
    {CmdMoveChart, g::Window, L, 0, L"Move Chart", L"V"},
};

constexpr Spec kPictureFormat[] = {
    {0, icon::Picture, 3, 0, L"Picture Styles", L"ZS", CmdFormatObject},
    {CmdObjectOutline, g::Pencil, M, kTool, L"Picture Border", L"SO"},
    {0, icon::Info, 3, 0, L"Accessibility", L"Z1"},
    {CmdFormatObject, icon::Info, L, 0, L"Alt Text", L"AT"},
};

constexpr Spec kShapeFormat[] = {
    {0, g::Shapes, 3, 0, L"Insert Shapes", L"ZH"},
    {CmdShapesMenu, g::Shapes, L, kMenu, L"Shapes", L"SH"},
    {CmdObjectTextBox, g::Case, M, 0, L"Text Box", L"X"},
    {0, icon::Color, 3, 0, L"Shape Styles", L"ZY", CmdFormatObject},
    {CmdObjectFill, icon::Color, M, kTool, L"Shape Fill", L"SF"},
    {CmdObjectOutline, g::Pencil, M, kTool, L"Shape Outline", L"SO"},
    {0, icon::Info, 3, 0, L"Accessibility", L"Z1"},
    {CmdFormatObject, icon::Info, L, 0, L"Alt Text", L"AT"},
};

// Print preview, as Excel 2007's Print Preview tab (Featherweight's preview is a mode of the tab).
constexpr Spec kPrintPreview[] = {
    {0, icon::Print, 3, 0, L"Print", L"ZP"},
    {CmdPrint, icon::Print, L, 0, L"Print", L"P"},
    {CmdPageSetup, icon::Settings, L, 0, L"Page Setup", L"S"},
    {0, icon::Page, 3, 0, L"Settings", L"ZS"},
    {CmdPrintWhatMenu, icon::Page, M, kMenu, L"Print What", L"A"},
    {CmdScalingMenu, icon::FullScreen, M, kMenu, L"Scaling", L"F"},
    {0, g::Zoom, 3, 0, L"Zoom", L"ZZ"},
    {CmdZoomMenu, g::Zoom, L, kMenu, L"Zoom", L"Z"},
    {0, g::Next, 3, 0, L"Preview", L"ZV"},
    {CmdNextPage, g::Next, M, 0, L"Next Page", L"N"},
    {CmdPrevPage, g::Prev, M, 0, L"Previous Page", L"V"},
    {CmdPrintPreview, icon::Cancel, L, 0, L"Close Print Preview", L"C"},
};

// A menu's lines: cmd 0 is a separator; `radio` marks a choice among several.
struct Line {
  int cmd;
  const wchar_t* label;
  bool radio = false;
};

struct Menu {
  int which;
  std::span<const Line> lines;
};

constexpr Line kPasteMenu[] = {{CmdPaste, L"&Paste\tCtrl+V"},  {CmdPasteFormulas, L"&Formulas"},
                               {CmdPasteTranspose, L"&Transpose"}, {0, nullptr},
                               {CmdPasteValues, L"&Values\tCtrl+Shift+V"}, {CmdPasteFormats, L"Fo&rmatting"}};
constexpr Line kMergeMenu[] = {{CmdMergeCenter, L"Merge && &Center"}, {CmdMergeAcross, L"Merge &Across"},
                               {CmdMergeCells, L"&Merge Cells"}, {CmdUnmerge, L"&Unmerge Cells"}};
constexpr Line kInsertMenu[] = {{CmdInsertCells, L"&Insert Cells…\tCtrl++"}, {CmdInsertRows, L"Insert Sheet &Rows"},
                                {CmdInsertColumns, L"Insert Sheet &Columns"}, {CmdInsertSheet, L"Insert &Sheet\tShift+F11"}};
constexpr Line kDeleteMenu[] = {{CmdDeleteCells, L"&Delete Cells…\tCtrl+-"}, {CmdDeleteRows, L"Delete Sheet &Rows"},
                                {CmdDeleteColumns, L"Delete Sheet &Columns"}, {CmdDeleteSheet, L"Delete &Sheet"}};
constexpr Line kFillMenu[] = {{CmdFillDown, L"&Down\tCtrl+D"}, {CmdFillRight, L"&Right\tCtrl+R"}};
constexpr Line kClearMenu[] = {{CmdClearContents, L"Clear &Contents\tDelete"}, {CmdRemoveLink, L"&Remove Hyperlinks"}};
constexpr Line kSortMenu[] = {{CmdSortAscending, L"&Sort A to Z"}, {CmdSortDescending, L"S&ort Z to A"},
                              {CmdDataBox, L"C&ustom Sort…"}, {0, nullptr}, {CmdFilter, L"&Filter\tCtrl+Shift+L"}};
constexpr Line kFindMenu[] = {{CmdFind, L"&Find…\tCtrl+F"}, {CmdReplace, L"&Replace…\tCtrl+H"}, {CmdGoTo, L"&Go To…\tCtrl+G"}};
constexpr Line kShapesMenu[] = {{CmdObjectRectangle, L"&Rectangle"}, {CmdObjectRoundRect, L"R&ounded Rectangle"},
                                {CmdObjectOval, L"O&val"}, {CmdObjectLine, L"&Line"}, {CmdObjectArrow, L"&Arrow"}};
constexpr Line kColumnMenu[] = {{CmdChartColumn, L"Clustered &Column\tAlt+F1"}, {CmdChartBar, L"Clustered &Bar"}};
constexpr Line kLineMenu[] = {{CmdChartLine, L"&Line"}, {CmdChartArea, L"&Area"}};
constexpr Line kPieMenu[] = {{CmdChartPie, L"&Pie"}, {CmdChartDoughnut, L"&Doughnut"}};
constexpr Line kMarginsMenu[] = {{CmdMarginsNormal, L"&Normal", true}, {CmdMarginsWide, L"&Wide", true},
                                 {CmdMarginsNarrow, L"N&arrow", true}, {0, nullptr}, {CmdPageSetup, L"Custom M&argins…"}};
constexpr Line kOrientationMenu[] = {{CmdPortrait, L"&Portrait", true}, {CmdLandscape, L"&Landscape", true}};
constexpr Line kPrintAreaMenu[] = {{CmdSetPrintArea, L"&Set Print Area"}, {CmdAddToPrintArea, L"&Add to Print Area"},
                                   {CmdClearPrintArea, L"&Clear Print Area"}};
constexpr Line kBreaksMenu[] = {{CmdInsertPageBreak, L"&Insert Page Break"}, {CmdRemovePageBreak, L"&Remove Page Break"},
                                {CmdResetPageBreaks, L"Re&set All Page Breaks"}};
constexpr Line kRefreshMenu[] = {{CmdPivotRefreshAll, L"Refresh &All\tCtrl+Alt+F5"}, {CmdPivotRefresh, L"&Refresh\tAlt+F5"}};
constexpr Line kValidationMenu[] = {{CmdDataValidation, L"Data &Validation…"}, {CmdCircleInvalid, L"C&ircle Invalid Data"},
                                    {CmdClearCircles, L"Clear Validation Ci&rcles"}};
constexpr Line kNotesMenu[] = {{CmdNewNote, L"&New Note\tShift+F2"}, {CmdShowNote, L"&Show/Hide Note"},
                               {CmdShowAllNotes, L"Show &All Notes"}, {CmdDeleteNote, L"&Delete Note"}};
constexpr Line kFreezeMenu[] = {{CmdFreezePanes, L"&Freeze Panes"}, {CmdFreezeTopRow, L"Freeze Top &Row"},
                                {CmdFreezeFirstColumn, L"Freeze First &Column"}};
constexpr Line kPrintWhatMenu[] = {{CmdPrintActiveSheet, L"Print &Active Sheets", true},
                                   {CmdPrintWorkbook, L"Print &Entire Workbook", true},
                                   {CmdPrintSelection, L"Print &Selection", true}};
constexpr Line kScalingMenu[] = {{CmdNoScaling, L"&No Scaling", true},
                                 {CmdFitSheetOnePage, L"&Fit Sheet on One Page", true},
                                 {CmdFitColumnsOnePage, L"Fit All &Columns on One Page", true},
                                 {CmdFitRowsOnePage, L"Fit All &Rows on One Page", true}};
constexpr Line kCondMenu[] = {{CmdCondFormat, L"&New Rule…"}, {0, nullptr}, {CmdCfClearSelection, L"Clear Rules from &Selected Cells"},
                              {CmdCfClearSheet, L"Clear Rules from &Entire Sheet"}, {0, nullptr}, {CmdCfManage, L"Manage &Rules…"}};

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
const wchar_t* Turned(int cmd, const CommandState& q) {
  if (cmd == CmdFreezePanes && q.label.rfind(L"Unfreeze", 0) == 0) return L"Un&freeze Panes";
  if (cmd == CmdNewNote && q.label.rfind(L"Edit", 0) == 0) return L"&Edit Note\tShift+F2";
  return nullptr;
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
        group.name = s.label;
        group.keyTip = s.key;
        group.glyph = s.glyph;
        group.rows = s.size;
        group.launcher = s.launcher;
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
        t.checked = q.checked;
        t.toggle = (s.flags & kToggle) != 0;
        t.dropdown = (s.flags & kMenu) != 0;
        if (s.flags & kSplit) t.kind = ToolKind::SplitButton;
        // Labels that turn around with the selection (Unprotect Sheet).
        if (s.cmd == CmdProtectSheet && q.label.rfind(L"Unprotect", 0) == 0) it.label = L"Unprotect Sheet";
      }
      if (s.glyph) t.glyph = s.glyph;
      it.size = static_cast<RibbonSize>(s.size);
      it.smallest = s.size == L ? RibbonSize::Large : RibbonSize::Small;
      if (it.label.empty()) it.label = s.label;
      it.keyTip = s.key;
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
  // Contextual tabs: Excel's, for what's selected.
  if (preview_) {
    add(L"Print Preview", L"JV", kPrintPreview, true);
    m.show = L"Print Preview";
    return;
  }
  const std::optional<SheetObjects::Object> object = SelectedObject();
  // An object just inserted shows its tab, as in Office; selecting one later doesn't.
  if (!object || object->id != ribbonInserted_) ribbonInserted_ = 0;
  if (ChartSheet() || (object && object->chart)) {
    add(L"Chart Design", L"JC", kChartDesign, true);
  } else if (object) {
    add(object->picture ? L"Picture Format" : L"Shape Format", object->picture ? L"JP" : L"JD",
        object->picture ? std::span<const Spec>(kPictureFormat) : std::span<const Spec>(kShapeFormat), true);
  }
  if (ribbonInserted_ && m.tabs.back().contextual) m.show = m.tabs.back().name;
  if (object || ChartSheet()) return;
  if (TableIndexAt(active_) >= 0) {
    add(L"Table Design", L"JT", kTableDesign, true);
  } else if (const auto* looks = PivotLooksFor(sheet_)) {
    for (const pivot::Look& l : *looks)
      if (l.area.Contains(active_.row, active_.col)) {
        add(L"PivotTable Analyze", L"JT", kPivotAnalyze, true);
        break;
      }
  }
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
      t.width = 92;
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
    const wchar_t* turned = Turned(l.cmd, q);
    menu::AppendItem(menu, static_cast<UINT>(l.cmd), turned ? turned : l.label, enabled, checked, l.radio);
  }
  return true;
}

// ribbon:dump: the model as built now, each item with its command, state and KeyTip, and the lines
// of each menu it opens (FillMenu), for checks (stderr, "ribbon: ...").
bool WorkbookView::RibbonScriptOp(const std::wstring& arg) {
  if (arg != L"dump") return false;
  RibbonModel m;
  BuildRibbon(m, ToolbarContext{});
  std::function<void(HMENU, int)> lines = [&](HMENU menu, int depth) {
    for (int i = 0; i < GetMenuItemCount(menu); ++i) {
      wchar_t text[128] = {};
      MENUITEMINFOW mi{sizeof mi, MIIM_ID | MIIM_STRING | MIIM_SUBMENU | MIIM_STATE | MIIM_FTYPE};
      mi.dwTypeData = text;
      mi.cch = 127;
      GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &mi);
      if (mi.fType & MFT_SEPARATOR) continue;
      fwprintf(stderr, L"ribbon: %*smenu %u %s%s\n", depth * 2, L"", mi.wID, text, mi.fState & MFS_DISABLED ? L" (off)" : L"");
      if (mi.hSubMenu) lines(mi.hSubMenu, depth + 1);
    }
  };
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
          lines(menu, 2);
          DestroyMenu(menu);
        }
      }
    }
  }
  if (!m.show.empty()) fwprintf(stderr, L"ribbon: show %s\n", m.show.c_str());
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
