#pragma once

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "PagedView.h"
#include "sheet/data/Filter.h"
#include "sheet/data/Sort.h"
#include "sheet/calc/Bind.h"
#include "sheet/calc/Entry.h"
#include "sheet/edit/Edit.h"
#include "sheet/condfmt/CondFormat.h"
#include "sheet/model/Workbook.h"
#include "sheet/view/CellPaint.h"
#include "sheet/view/CellText.h"
#include "sheet/view/CellEditor.h"
#include "sheet/view/Grid.h"
#include "sheet/view/PrintLayout.h"
#include "sheet/view/WorkbookFile.h"
#include "text/Fonts.h"

namespace fw::sheet::calc {
struct FunctionHelp;
}

namespace fw::sheet::view {

// An outline group: rows (or columns) at one level, which collapse into their summary row.
struct Group {
  int level = 1;
  uint32_t first = 0, last = 0;
  bool collapsed = false;
};

// A workbook in a tab (milestone S1, plan §5.3 and §7.3): the grid of the active sheet with its
// headers, gridlines, frozen panes, outline groups and zoom; the formula bar (read-only) above it
// and the sheet tabs below; selection, keyboard navigation, copy, find and the status bar's sum.
// Print preview shows the pages as they print, through PagedView.
//
// Opening reads the workbook and its active sheet; the other sheets are read at idle. Only the
// cells on screen are drawn, with their text laid out once and cached.
class WorkbookView : public PagedView {
 public:
  WorkbookView(ViewHost& host, std::wstring path);
  ~WorkbookView() override;
  // A new workbook, "Book<number>" (File > New), and a workbook that isn't its file: a recovered
  // one (`unsaved` changes) or a copy. Both save with Save As, starting at `suggested`.
  static std::unique_ptr<WorkbookView> NewWorkbook(ViewHost& host, int number);
  static std::unique_ptr<WorkbookView> FromBytes(ViewHost& host, std::vector<uint8_t> bytes, std::wstring title,
                                                 std::wstring suggested, bool unsaved);
  // A template (.xltx, .xltm) opens as a new workbook from it, as in Excel: "Budget1".
  void OpenAsNew(int number) { newNumber_ = number; }

  // ---- TabContent ----
  TabKind Kind() const override { return TabKind::Workbook; }
  OpenResult Load(const std::wstring& password, std::wstring& error) override;
  bool Ready() const override { return book_ != nullptr; }
  const std::wstring& Path() const override { return path_; }
  std::wstring Title() const override;
  bool Modified() const override { return file_ && file_->Dirty(); }
  PagedView* Paged() override { return preview_ ? this : nullptr; }

  void SetViewport(const RECT& rc, float ui) override;
  void Draw(gfx::Canvas& c, ID2D1DeviceContext* dc) override;
  bool HasIdleWork() const override;
  bool IdleWork(ID2D1DeviceContext* dc, double deadlineMs) override;
  void ReleaseDeviceResources() override {}
  void OnDeactivated() override;

  void OnMouseDown(int x, int y, int button, UINT mods, bool doubleClick) override;
  void OnMouseMove(int x, int y, UINT mods) override;
  void OnMouseUp(int x, int y, int button, UINT mods) override;
  void OnMouseLeave() override;
  void OnWheel(int delta, bool horizontal, UINT mods, int x, int y) override;
  bool OnKeyDown(UINT vk, UINT mods) override;
  bool OnChar(wchar_t ch, UINT mods) override;
  HCURSOR CursorAt(int x, int y) override;
  std::wstring TooltipAt(int x, int y) override;
  bool Dragging() const override { return mouse_ != Mouse::None || PagedView::Dragging(); }
  bool HasKeyFocus() const override { return editor_.Active(); }
  bool CanReplace() const override { return Ready() && !preview_ && !ChartSheet(); }
  bool ReplaceCurrent(const std::wstring& with) override;
  int ReplaceAll(const std::wstring& with) override;
  std::optional<RECT> CaretRect() override;

  int ShortcutFor(UINT vk, UINT mods) const override;
  CommandState QueryCommand(int cmd) override;
  std::vector<int> Commands() override;
  bool Execute(int cmd) override;
  void BuildToolbar(ToolbarModel& m, const ToolbarContext& ctx) override;
  void FillMenu(int which, HMENU menu) override;
  void FillContextMenu(HMENU menu, int x, int y) override;
  bool HasStatusBar() const override { return Ready() && !preview_; }
  void BuildStatusBar(StatusBarModel& m) override;

  void StartSearch(const std::wstring& query, bool matchCase, bool wholeWords) override;
  void ClearSearch() override;
  void SearchStep(bool forward) override;
  int SearchCount() const override { return static_cast<int>(matches_.size()); }
  int SearchCurrent() const override { return matchIndex_; }
  bool SearchDone() const override { return searchDone_; }
  std::wstring SelectedText() override;

  // Saving goes through the tab's WorkbookFile: Ctrl+S and Save As write in the background
  // (StartSave; the workbook is read-only until SaveDone), closing and scripts wait (Save).
  bool Save(const std::wstring& path, std::wstring& error) override;
  SaveStart StartSave(const std::wstring& path, std::wstring& error) override;
  void SaveDone() override;
  SaveFormat SaveAsFormat() const override;
  std::vector<SaveFormat> OtherSaveFormats() const override;
  void Autosave() override;
  bool ReplayEdits(std::string_view records, std::wstring& error) override;  // s2-format: WorkbookCommands.cpp
  FileChange CheckFileOnDisk() override;
  // CSV (s2-format): an Excel workbook saves a copy of its first sheet's values as CSV; a CSV file's
  // Save As suggests a workbook; saving a CSV that holds more than values asks first (the banner).
  std::vector<SaveFormat> CopyFormats() const override;
  bool SaveCopy(const std::wstring& path, size_t format, std::wstring& error) override;
  std::wstring SuggestedSavePath() const override;
  std::wstring FormatLoss(const std::wstring& path) const override;
  void KeepFormat() override { keepFormat_ = true; }
  // format:size=14, font=, color=RRGGBB, fill=RRGGBB, numfmt=<code as typed here>, code=<English code>,
  // border=, align=, wrap, bold, italic, locked, hidden, style=: the selection, through the toolbar's
  // and Format Cells' calls (WorkbookFormat.cpp).
  bool ScriptOp(const std::wstring& op, const std::wstring& arg) override;
  bool CanPrint() const override { return Ready(); }
  PrintOutcome Print(HWND owner, const std::function<void(int, int)>& progress, std::wstring& message) override;
  bool CanExportPdf() const override { return Ready(); }
  bool ExportPdf(const std::wstring& path, std::wstring& message) override;
  FileState State() const override;
  void ApplyState(const FileState& s) override;
  void DescribeUia(uia::Tree& t, int parent) override;

  // ---- Tests and tools ----
  // The active sheet's pages to Microsoft Print to PDF, with no dialog (the visual checks).
  PrintOutcome PrintToFile(const std::wstring& path, std::wstring& message);
  // Each printed page as a PNG (page-1.png...) in `dir` at `dpi`, for comparing with Excel's PDFs.
  int RenderPagesForTesting(const std::wstring& dir, float dpi);
  bool ExportWorkbookPdfForTesting(const std::wstring& path);  // every sheet's pages, one PDF
  // The cell shown at a point, for tests; nullopt outside the cells.
  std::optional<CellAddr> CellAtForTesting(int x, int y) const { return CellAt(x, y); }

 protected:
  const std::vector<fw::PageSizePt>* Pages() const override { return preview_ ? &pageSizes_ : nullptr; }

 private:
  // ---- Sheets
  struct PerSheet {
    CellAddr active, end;      // the active cell and the selection's other corner
    double sx = -1, sy = -1;   // the scrolled panes' top-left, Excel pixels (-1: from the file)
    double zoom = 0;           // 0: the file's
    std::vector<Group> rowGroups, colGroups;  // outline groups as expanded and collapsed here
    bool groupsKept = false;
  };
  void ShowSheet(size_t index);
  bool SheetShown(size_t i) const;
  size_t NextShownSheet(size_t from, int dir) const;
  Sheet& Current() const { return book_->GetSheet(sheet_); }
  const fw::sheet::SheetView* ViewOf() const;  // the file's view of the sheet
  void LoadView();  // the sheet's view from the file: panes, zoom, selection
  OpenResult Opened(std::unique_ptr<Workbook> book, std::wstring& error);  // the rest of loading, once the file is read

  // ---- Layout: the formula bar, the grid with its headers, the sheet tabs.
  void LayoutParts();
  double K() const { return zoom_ * ui_; }  // device pixels per Excel pixel
  struct Pane {
    RECT rc{};                 // on screen
    uint32_t row0 = 0, col0 = 0, row1 = 0, col1 = 0;  // the cells it may show (row1/col1 exclusive)
    double sx = 0, sy = 0;     // the sheet position (Excel pixels) at its top-left
    bool scrollsX = false, scrollsY = false;
  };
  std::vector<Pane> Panes() const;
  int FrozenWidthPx() const;   // device pixels
  int FrozenHeightPx() const;
  void ClampScroll();
  void ScrollTo(double sx, double sy);
  void ScrollIntoView(CellAddr cell);
  std::optional<CellAddr> CellAt(int x, int y) const;
  // Screen X of a column's left edge in a pane, left to right (mirrored afterwards for RTL).
  float PaneX(const Pane& p, uint32_t col) const;
  float PaneY(const Pane& p, uint32_t row) const;
  D2D1_RECT_F CellRect(const Pane& p, uint32_t r0, uint32_t c0, uint32_t r1, uint32_t c1) const;
  float Mirror(float x) const { return rtl_ ? cells_.left + cells_.right - x : x; }

  // ---- Drawing (WorkbookView.cpp)
  void DrawGrid(gfx::Canvas& c, ID2D1DeviceContext* dc);
  void DrawSelection(gfx::Canvas& c, const Pane& p);
  void DrawHeaders(gfx::Canvas& c);
  void DrawOutline(gfx::Canvas& c);
  void DrawFormulaBar(gfx::Canvas& c);
  void DrawSheetTabs(gfx::Canvas& c);
  void DrawScrollbars(gfx::Canvas& c);
  // The overlay scrollbars of the scrolled panes, in Excel pixels: the range they cover.
  struct Bar {
    bool shown = false;
    D2D1_RECT_F track{}, thumb{};
    double min = 0, end = 0, view = 0;
  };
  bool Bars(Bar& vertical, Bar& horizontal) const;
  void DrawPreview(gfx::Canvas& c, ID2D1DeviceContext* dc);

  // ---- Selection and navigation (SheetInput.cpp)
  Range Selected() const;
  Range Expand(Range r) const;  // to whole merged areas
  void Select(CellAddr active, CellAddr end, bool scroll = true);
  void MoveActive(int dr, int dc, bool extend, bool jump);
  uint32_t StepRow(uint32_t r, int dir) const;  // the next shown row
  uint32_t StepCol(uint32_t c, int dir) const;
  CellAddr JumpEdge(CellAddr from, int dr, int dc) const;
  bool HasValue(uint32_t r, uint32_t c) const;
  void PageMove(int dir, bool horizontal, bool extend);

  // ---- Outline groups (WorkbookInput.cpp)
  void BuildGroups();
  void ApplyGroups(bool rows);
  std::vector<Group> rowGroups_, colGroups_;
  struct OutlineButton {
    D2D1_RECT_F rc{};
    bool rows = true;
    int level = 0;        // a level button (1..), or 0 for a group's + or −
    uint32_t first = 0, last = 0;  // the group's rows or columns
    bool collapsed = false;
  };
  std::vector<OutlineButton> OutlineButtons() const;
  int OutlineLevels(bool rows) const;
  void ToggleGroup(const OutlineButton& b);
  void ShowLevel(bool rows, int level);

  // ---- Formula bar
  std::wstring FormulaBarText() const;
  std::wstring EditTextAt(const Sheet& s, uint32_t row, uint32_t col) const;
  std::wstring ActiveName() const;

  // ---- Sheet tabs
  struct TabRect {
    size_t sheet;
    D2D1_RECT_F rc;
  };
  std::vector<TabRect> TabRects() const;

  // ---- Commands (SheetCommands.cpp)
  void Copy();
  void UpdateStatus();
  void FollowLink(const Hyperlink& link);
  const Hyperlink* LinkAt(uint32_t r, uint32_t c) const;
  void SetZoom100(double zoom);  // the grid's zoom (1 = 100 %)
  void TogglePreview();

  // ---- Print (SheetPrint.cpp)
  void Paginate();
  PrintInfo InfoFor(const Sheet& sheet) const;
  // Every shown sheet laid out on pages in turn (the active one as shown), as Excel exports a workbook.
  void ForEachSheetLayout(const std::function<void(PrintLayout&)>& fn);
  bool RenderLayoutPages(PrintLayout& layout, const std::wstring& dir, float dpi, int& written);
  PrintOutcome PrintPages(const std::wstring& printer, const DEVMODEW* devmode, const std::vector<int>& pages,
                          IStream* output, const std::function<void(int, int)>& progress, std::wstring& message);
  void PaintPage(draw::Painter& p, int page);

  // ---- Search
  struct Match {
    uint32_t sheet, row, col;
  };
  void SearchIdle(double deadlineMs);
  void ShowMatch();

  std::wstring path_;
  std::unique_ptr<Workbook> book_;
  std::unique_ptr<text::Fonts> fonts_;
  std::unique_ptr<FontMeasure> measure_;
  std::unique_ptr<CellText> text_;
  Grid grid_;
  size_t sheet_ = 0;
  bool shown_ = false;  // a sheet is shown (its state can be kept when another is)
  std::vector<PerSheet> states_;
  size_t loadNext_ = 0;  // the next sheet to read at idle
  bool rtl_ = false;
  bool gridlines_ = true, headers_ = true, showZeros_ = true;
  uint32_t gridColor_ = 0xFFD9D9D9;
  double zoom_ = 1;
  // Frozen panes: rows [frozenRow0, frozenRow1) and columns [frozenCol0, frozenCol1) stay.
  uint32_t frozenRow0_ = 0, frozenRow1_ = 0, frozenCol0_ = 0, frozenCol1_ = 0;
  double gridX_ = 0, gridY_ = 0;  // the scrolled panes' top-left, Excel pixels
  CellAddr active_, end_;

  // Parts, in client pixels.
  RECT full_{};
  D2D1_RECT_F formulaBar_{}, nameBox_{}, grid_rc_{}, cells_{}, colHeader_{}, rowHeader_{}, corner_{}, tabs_{};
  D2D1_RECT_F rowGutter_{}, colGutter_{};
  float tabScroll_ = 0;

  // Mouse.
  enum class Mouse { None, Cells, RowHeaders, ColHeaders, ThumbV, ThumbH, EditText, Point, RefDrag, Fill, Size, TabDrag };
  // A sheet tab dragged to move the sheet: where it would go (before that sheet; SheetCount() for
  // the end), shown once the mouse has moved a few pixels.
  struct TabDrag {
    size_t sheet = 0;
    float x0 = 0;
    bool moving = false;
    size_t before = 0;
  };
  std::optional<TabDrag> tabDrag_;
  void TabDragMove(float x);
  void FinishTabDrag();
  void DrawTabDrop(gfx::Canvas& c);
  // Undo and redo with their lists (split buttons: the arrow lists the steps, newest first).
  void AddUndoToolbar(ToolbarModel& m);
  double calcSince_ = 0;  // anim::Now() when the recalculation under way began; 0: none
  Mouse mouse_ = Mouse::None;
  float thumbGrab_ = 0;
  int hoverTab_ = -1;
  POINT hover_{-1, -1};
  bool pendingLink_ = false;

  // Status bar: sum, average and count of the selection, kept until it changes.
  struct Totals {
    Range range{kNone, kNone, kNone, kNone};
    size_t sheet = kNone;
    double sum = 0;
    size_t numbers = 0, count = 0;
    uint32_t style = 0;
  } totals_;

  // Find.
  std::wstring query_;
  bool matchCase_ = false, wholeWords_ = false;
  std::vector<Match> matches_;
  int matchIndex_ = -1;
  bool searchDone_ = true;
  uint32_t searchRow_ = 0;  // where the scan has got to
  std::optional<Range> searchArea_;

  // Print preview and printing.
  bool preview_ = false;
  std::unique_ptr<PrintLayout> print_;
  std::unique_ptr<SheetObjects> printObjects_;  // print_'s, placed on the printed columns
  std::vector<fw::PageSizePt> pageSizes_;

  // Pictures, shapes, charts and notes (fwsheetobjects): each sheet's, read when first shown and
  // placed on the screen's columns and rows.
  std::vector<std::unique_ptr<SheetObjects>> objects_;
  SheetObjects* Objects() const { return sheet_ < objects_.size() ? objects_[sheet_].get() : nullptr; }
  bool ChartSheet() const { return Objects() && Objects()->IsChartSheet(); }
  void PlaceObjects();
  void DrawNote(ID2D1DeviceContext* dc);
  // Formulas that kept their cached value (a function the engine can't calculate yet): a small
  // gray mark in the cell's top left corner; the status bar says why for the active cell.
  void DrawStale(gfx::Canvas& c);

  // ---- Editing a cell (s2-plan "Editing in the grid"): the text in CellEditor, drawn over the
  // cell and in the formula bar; Enter and Tab move as Excel's do (Enter after Tabs returns to the
  // column the Tabs started from).
  void BeginEdit(CellEditor::Mode mode, const std::wstring& text);
  bool EditKey(UINT vk, UINT mods);
  // Writes the entry through the edit API (with Ctrl+Enter into every selected cell). False when
  // it stays open: a formula Excel refuses, or an edit the model refuses (the banner says why).
  bool CommitEdit(bool fill);
  bool InputFor(const std::wstring& text, int sheet, CellAddr cell, edit::CellInput& in, std::string& formula,
                std::string& message, size_t& pos);
  // Replace (Ctrl+H): every place the search's text is in a match's content, as typed.
  bool ReplacedText(const Match& m, const std::wstring& with, std::wstring& out) const;
  bool editReplaces_ = false;  // typing over the cell: the entry is written even if unchanged
  // The model's edits (s2-model's Editor), made when first needed; Edited hears every change.
  edit::Editor& Ed();
  void Edited(const std::vector<edit::Change>& changes);
  void SyncEngine(const std::vector<edit::Change>& changes);
  // F9 and Ctrl+Alt+F9 recalculate at idle even in manual mode until done; circular references
  // found warn once and show in the status bar ("Circular References: B2"), as Excel's do.
  bool calcNow_ = false;
  std::optional<std::tuple<int, int32_t, int32_t>> circular_;  // sheet, row, col
  void CheckCircular();
  bool EditRefused(const edit::Result& r);  // shows why; true when it failed
  std::unique_ptr<edit::Editor> edit_;
  // The tab's file (saving, the unsaved dot, the owner file, recovery, changes on disk). After the
  // workbook and its editor, so it goes first; there from the start.
  std::unique_ptr<WorkbookFile> file_;
  // Where Load reads from: path_ (a template when newNumber_ is set), a new workbook, or bytes.
  int newNumber_ = 0;
  bool newBook_ = false;
  std::vector<uint8_t> bytes_;
  bool fromBytes_ = false;
  bool ReadyToSave(std::wstring& error);  // commits the cell being edited
  void SavedTo();                         // after a save: the tab is the file written
  bool keepFormat_ = false;  // "Keep CSV" chosen: no banner on the next saves
  // ---- Formats (WorkbookFormat.cpp, s2-format): Format Cells (Ctrl+1) and the quick formats.
  bool FormatCommand(int cmd);
  bool FormatCommandState(int cmd, CommandState& s);
  class FormatPanel;
  std::unique_ptr<FormatPanel> formatPanel_;
  void ShowFormatPanel(int page);
  uint32_t ActiveStyle() const;  // the active cell's format (a CellXfs index)
  // Every selected cell's format through `change`, which sets only what it changes; one undo step.
  bool ChangeFormat(std::u16string name, const std::function<void(Xf&)>& change);
  // The same for the cells of `r`, inside a group (Editor::BeginGroup).
  bool ChangeFormatIn(const Range& r, const std::function<void(Xf&)>& change);
  bool ChangeFont(std::u16string name, const std::function<void(Font&)>& change);
  // A number format's id: a built-in format's (the code as files define it), else a custom one's.
  uint32_t NumFmtId(const std::wstring& code);
  std::wstring CodeOf(uint32_t numFmt) const;  // invariant
  bool ApplyNumberCode(const std::wstring& code);  // invariant
  bool ChangeDecimals(int delta);                  // Increase and Decrease decimal, from the active cell
  // Borders on the selection: 0 none, 1 outline, 2 all, 3 inside, 4..7 top, bottom, left, right.
  bool ApplyBorders(int which, const BorderSide& side);
  bool MergeCells(int how);  // 0 merge and center, 1 merge across, 2 merge, 3 unmerge
  void AddFormatToolbar(ToolbarModel& m);
  D2D1_COLOR_F lastFontColor_{0.75f, 0, 0, 1}, lastFill_{1, 1, 0, 1};  // the color buttons' main parts
  int lastBorders_ = 0;  // the borders button's choice its main part repeats
  bool ApplyCellStyle(int index);  // one of Excel's built-in cell styles (WorkbookFormat.cpp's list)
  // The format painter: the formats copied, painted on the next selection made with the mouse.
  struct Painter {
    const Sheet* sheet = nullptr;
    Range range;
  };
  std::optional<Painter> painter_;
  void PaintFormats();
  // ---- Filter (WorkbookFilter.cpp, s2-format): Ctrl+Shift+L, the header row's buttons, their panel.
  struct ViewFilter {
    Range ref;  // the header row and the rows it filters
    std::vector<data::FilterCriteria> criteria;
  };
  std::unordered_map<const Sheet*, ViewFilter> filters_;
  ViewFilter* FilterOf();  // the shown sheet's filter, or null
  void ToggleFilter();
  bool ApplyFilter();  // hides the rows its criteria leave out, shows the others
  // The header button at (x, y): the column within the filter, and where it is.
  bool FilterButtonAt(float x, float y, uint32_t* column, D2D1_RECT_F* rc = nullptr);
  void DrawFilterButtons(gfx::Canvas& c);
  class FilterPanel;
  std::unique_ptr<FilterPanel> filterPanel_;
  void ShowFilterPanel(uint32_t column);
  bool DataCommand(int cmd);
  bool DataCommandState(int cmd, CommandState& s);
  // Sort and remove duplicates: the range they work on (the selection, or the current region).
  Range DataRange();
  // Through the edit layer's SortRows: formulas move as Excel moves them, one undo step; refused
  // over merged cells or array formulas, with Excel's message.
  bool SortRange(const Range& range, bool header, const std::vector<data::SortKey>& keys);
  bool RemoveDuplicates(const Range& range, bool header, const std::vector<uint32_t>& columns);
  class DataPanel;
  std::unique_ptr<DataPanel> dataPanel_;
  void ShowDataPanel(bool sort);
  // ---- Rows, columns, cells and sheets (WorkbookSheetOps.cpp, WorkbookPanels.cpp).
  bool GridCommand(int cmd);
  bool GridCommandState(int cmd, CommandState& s);
  // ---- Sizes from the headers' edges (WorkbookPanels.cpp): drag to size, double-click to fit, as
  // in Excel; the selected whole rows or columns all take the size when the edge is one of theirs.
  struct SizeEdge {
    bool rows = false;
    uint32_t index = 0;  // the row above the edge, or the column before it
    float start = 0;     // that row's top or column's left on screen (left to right)
    float edge = 0;      // and the edge, where the drag started
  };
  std::optional<SizeEdge> SizeEdgeAt(int x, int y) const;
  std::optional<SizeEdge> sizing_;
  float sizeTo_ = 0;  // the edge while dragging
  void FinishSizing();
  void FitAtEdge(const SizeEdge& e);
  void DrawSizeGuide(gfx::Canvas& c);
  // The rows or columns first..last fitted to their cells (only those in `within`, when given).
  void AutoFit(bool rows, uint32_t first, uint32_t last, const Range* within);
  // Sizes in the file's units (points; characters with padding), 0 hiding, as one undo step.
  void ApplySize(bool rows, uint32_t first, uint32_t last, double value, const char16_t* name);
  // Excel's column width as it shows it (8.43) from pixels, and the file's width for it (9.140625).
  double DisplayWidth(int px) const;
  double FileWidth(double display) const;
  // ---- Merge, freeze panes, group (WorkbookSheetOps.cpp).
  bool MergedIn(const Range& r) const;  // a merged area overlaps r
  void MergeSelection(int cmd);         // CmdMergeCenter, CmdMergeAcross, CmdMergeCells, CmdUnmerge
  bool Frozen() const { return frozenRow1_ > frozenRow0_ || frozenCol1_ > frozenCol0_; }
  void Freeze(int cmd);                 // CmdFreezePanes (or unfreeze), CmdFreezeTopRow, CmdFreezeFirstColumn
  void PanesChanged();                  // the sheet's frozen panes changed (an edit, undo, redo)
  void GroupSelection(bool in);         // Alt+Shift+Right and Left
  bool WholeRows() const;     // whole rows are selected (not everything)
  bool WholeColumns() const;
  int ShownNeighbour(int dir) const;  // the next shown sheet left (-1) or right (1) of this one, or -1
  void SheetsChanged();  // sheets added, deleted or moved: the view's per-sheet state follows them
  void Restructure();    // row heights, hidden rows and columns, merges changed on this sheet
  std::vector<const Sheet*> sheetOrder_;  // the sheets in tab order, as the view last saw them
  class SizePanel;
  class GoToPanel;
  std::unique_ptr<SizePanel> sizePanel_;
  std::unique_ptr<GoToPanel> goToPanel_;
  void ShowSizePanel(bool rows);
  void ShowGoTo();
  double CurrentSize(bool rows) const;  // the active row's height (points) or column's width (characters)
  void SetSizes(bool rows, double value);
  bool GoTo(const std::wstring& text);  // a cell, a range, Sheet2!A1 or a defined name
  // ---- The fill handle, Ctrl+D and Ctrl+R (WorkbookFill.cpp). `dir`: 0 down, 1 up, 2 right, 3 left.
  bool FillCells(const Range& source, const Range& target, int dir, bool series, std::u16string name);
  void FillDown(bool right);
  bool FillHandleAt(float x, float y, D2D1_RECT_F* rc = nullptr);  // rc: where it is (x, y unused)
  void DrawFillHandle(gfx::Canvas& c);
  void FillDrag(int x, int y);
  void FinishFill();
  // Snapshot script ops of the grid's (s2-format's ScriptOp hands them on): fill:<range> drags the
  // selection's fill handle until the selection covers the range ("fill:B3:M3"), as the mouse does.
  bool GridScriptOp(const std::wstring& op, const std::wstring& arg);
  void FillToNeighbour();
  Range fillTo_{};
  // ---- Cut, copy and paste (WorkbookClipboard.cpp).
  enum class PasteMode : uint8_t { All, Values, Formulas, Formats };
  struct Clip {
    const Sheet* sheet = nullptr;  // what Copy put on the clipboard, while it's still there
    Range range;
    bool cut = false;
    DWORD seq = 0;  // the clipboard's sequence number then
  };
  std::optional<Clip> clip_;
  bool pasting_ = false;  // a paste's own changes don't end the copied cells' border
  void Cut();
  void Paste(PasteMode mode, bool transpose = false);
  void DrawClipBorder(gfx::Canvas& c, const Pane& pane);  // while the handle is dragged: the selection with the cells it will fill
  // Renaming a sheet on its tab (double-click it, or Rename): the cell editor's text and keys.
  void BeginRename(size_t sheet);
  int renaming_ = -1;
  void DrawEditBox(gfx::Canvas& c, const D2D1_RECT_F& clip);
  // The region's formula separators (en-NL: "," decimal, ";" between arguments).
  const calc::FormulaLocale& Region() const;
  mutable std::optional<calc::FormulaLocale> region_;
  void MoveAfterEntry(int dRow, int dCol);
  void PointMove(int dRow, int dCol, bool extend);
  struct EditBox {
    D2D1_RECT_F rc{};
    float size = 0, line = 0, pad = 0;  // font size, line height, inner padding (device pixels)
  };
  bool EditGeometry(EditBox& box);
  void DrawEditor(gfx::Canvas& c);
  // The formula bar's text box; editing there (a click in it) draws the caret there.
  D2D1_RECT_F FormulaBox() const;
  float FormulaBarSize() const { return 13 * ui_; }
  void DrawBarText(gfx::Canvas& c);
  // A click or drag in the formula bar's text or the box over the cell: true when it was there.
  bool EditMouseDown(float x, float y, UINT mods, bool doubleClick);
  void EditMouseMove(float x, float y);
  int BarHit(float x);
  int BoxHit(const EditBox& b, float x, float y);
  // The references in the formula being typed, each distinct one with its color (index into the
  // reference colors): outlined on the sheet and colored in the text, as Excel's are.
  struct RefBox {
    int at = 0, length = 0;  // in the editor's text
    int sheet = -1;          // the sheet index; -1 none named (the edited cell's), -2 not found
    uint32_t r0 = 0, c0 = 0, r1 = 0, c1 = 0;
    int color = 0;
    int refAt = 0;  // where the reference starts after its sheet
    int kind = 0;   // 0 cells, 1 whole columns, 2 whole rows
  };
  const std::vector<RefBox>& Refs();
  // Dragging a reference's border moves it; a corner's handle sizes it (from the opposite corner).
  struct RefGrab {
    size_t index = SIZE_MAX;
    bool resize = false, nwse = false;  // nwse: the top-left or bottom-right handle
    CellAddr anchor{};                  // resize: the corner that stays
    uint32_t dRow = 0, dCol = 0;        // move: the grabbed cell from the range's top-left
  };
  bool RefHit(int x, int y, RefGrab& g);
  void RefDrag(int x, int y);
  RefGrab refGrab_;
  void DrawRefBorders(gfx::Canvas& c, const Pane& pane);
  // One line of the editor's text from `lineAt`, references in their colors.
  void DrawEditLine(gfx::Canvas& c, std::wstring_view line, int lineAt, float left, float top, float bottom,
                    float size, D2D1_COLOR_F ink);
  // A click on a cell while a formula waits for a reference inserts it (point mode by mouse).
  bool PointClick(int x, int y);
  void PointDrag(int x, int y);
  std::vector<RefBox> refs_;
  std::wstring refsText_;
  // AutoComplete: typing text at the end proposes the one entry in the column's run of cells
  // (above and below, to a blank) that starts with it, the rest selected.
  void AutoComplete();
  std::vector<std::pair<std::wstring, std::wstring>> autoList_;  // lowercase, as entered
  bool autoLoaded_ = false;
  std::wstring autoText_;  // the entry proposed (its case wins when the entry is committed)
  // Ctrl+; and Ctrl+Shift+;: today's date or the time now, as Windows writes them short.
  void InsertNow(bool time);
  static UINT SemicolonKey();  // the key that types ";" on this keyboard
  // F4: the reference at the caret cycles A1, $A$1, A$1, $A1.
  bool CycleReference();
  // Formula AutoComplete: the functions starting with the name being typed (Tab puts one in with
  // "("), and the argument tip inside a call (WorkbookFunctions.cpp).
  void UpdateFormulaHelp();
  bool FormulaHelpKey(UINT vk, bool ctrl, bool shift, bool alt);
  bool FormulaHelpClick(float x, float y, bool doubleClick);
  void InsertFunction(size_t i);
  void DrawFormulaHelp(gfx::Canvas& c);
  // Alt+=: s2-calc's proposed SUM in the cell, its range pointed at, as Excel's AutoSum.
  void AutoSum();
  std::vector<const calc::FunctionHelp*> fnList_;
  size_t fnSel_ = 0, fnTop_ = 0;
  int fnFrom_ = -1;          // where the name being typed starts
  bool fnClosed_ = false;    // Esc closed the list: closed until the next character
  D2D1_RECT_F fnListRc_{};
  float fnRowH_ = 0;
  CellEditor editor_;
  bool editInBar_ = false;  // the caret is in the formula bar, not over the cell
  float barScroll_ = 0;     // the formula bar's text scrolled left to keep the caret in sight
  CellAddr editCell_{};
  size_t editSheet_ = 0;
  CellAddr point_{}, pointEnd_{};  // point mode's reference
  uint32_t tabStartCol_ = UINT32_MAX;
  bool Stale(uint32_t row, uint32_t col) const;
  std::optional<CellAddr> note_;  // the cell whose note shows while the mouse is over it

  // Conditional formats: each sheet's rules, made when it is first painted if it has any, with the
  // formula engine they need for theirs (loaded for the first such sheet, and only then).
  condfmt::SheetRules* RulesFor(size_t index);
  std::unique_ptr<calc::WorkbookCalc> calc_;  // see Calc()
  bool recalc_ = false;  // a workbook (not CSV): recalculate what Excel would after opening
  calc::WorkbookCalc& Calc();
  std::vector<std::unique_ptr<condfmt::SheetRules>> rules_;
  std::vector<bool> rulesRead_;
};

}  // namespace fw::sheet::view
