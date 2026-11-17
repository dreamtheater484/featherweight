#pragma once

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "PagedView.h"
#include "sheet/calc/Bind.h"
#include "sheet/condfmt/CondFormat.h"
#include "sheet/model/Workbook.h"
#include "sheet/view/CellPaint.h"
#include "sheet/view/CellText.h"
#include "sheet/view/CellEditor.h"
#include "sheet/view/Grid.h"
#include "sheet/view/PrintLayout.h"
#include "text/Fonts.h"

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

  // ---- TabContent ----
  TabKind Kind() const override { return TabKind::Workbook; }
  OpenResult Load(const std::wstring& password, std::wstring& error) override;
  bool Ready() const override { return book_ != nullptr; }
  const std::wstring& Path() const override { return path_; }
  std::wstring Title() const override;
  bool Modified() const override { return false; }
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

  bool Save(const std::wstring& path, std::wstring& error) override;
  SaveFormat SaveAsFormat() const override;
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
  enum class Mouse { None, Cells, RowHeaders, ColHeaders, ThumbV, ThumbH };
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
  void CommitEdit(bool fill);
  void MoveAfterEntry(int dRow, int dCol);
  void PointMove(int dRow, int dCol, bool extend);
  struct EditBox {
    D2D1_RECT_F rc{};
    float size = 0, line = 0, pad = 0;  // font size, line height, inner padding (device pixels)
  };
  bool EditGeometry(EditBox& box);
  void DrawEditor(gfx::Canvas& c);
  CellEditor editor_;
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
