#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pkg/RoundTrip.h"
#include "sheet/model/TextPool.h"
#include "sheet/model/Types.h"

namespace fw::sheet {

class Workbook;
class SheetBuilder;

enum class SheetKind : uint8_t { Worksheet, Chartsheet, Dialogsheet, Macrosheet };
enum class SheetState : uint8_t { Visible, Hidden, VeryHidden };

// One stored cell, as Sheet::GetCell and ForEachCell give it.
struct CellInfo {
  uint32_t row = 0, col = 0;
  Value value;                     // for a formula: its cached value (Empty when the file has none)
  uint32_t style = 0;              // index into Styles::CellXfs()
  uint32_t sharedString = kNone;   // a shared string's id (StringTable::Runs has its rich text)
  bool hasFormula = false;
  bool computed = false;           // the value was set by recalculation, not read from the file
};

enum class FormulaKind : uint8_t { Normal, Shared, Array, DataTable };

// A cell's formula as the file stores it (invariant form, without the leading "=").
struct FormulaInfo {
  FormulaKind kind = FormulaKind::Normal;
  // The text. A cell that shares a formula has none of its own: this is the master's text, which
  // is written for `origin`, so its relative references move by the distance from there.
  std::string_view text;
  CellAddr origin;          // the cell `text` is written for (this cell, or the shared master)
  Range ref;                // shared, array and data table formulas: the cells it covers
  bool master = false;      // this cell holds the text of a shared or array formula
  uint32_t sharedIndex = kNone;  // si
  bool alwaysCalc = false;  // ca="1"
  pkg::AttrSetId attrs = 0; // all of the f element's attributes (Sheet::Attributes), for data tables (r1, r2...)
};

struct RowInfo {
  double height = 0;  // points; 0: the sheet's default height
  uint32_t style = 0;  // applies when customFormat
  uint8_t outlineLevel = 0;
  bool customHeight = false, hidden = false, collapsed = false, customFormat = false, thickTop = false,
       thickBottom = false;
  bool stored = false;  // the file has a row element for it
};

struct ColumnInfo {
  uint32_t first = 0, last = 0;  // columns, both included
  double width = 0;              // in characters of the default font's widest digit; 0: not given
  uint32_t style = 0;
  uint8_t outlineLevel = 0;
  bool hidden = false, customWidth = false, bestFit = false, collapsed = false, phonetic = false;
};

// sheetFormatPr: the sheet's defaults.
struct SheetFormat {
  double baseColWidth = 8;      // characters, padding excluded
  double defaultColWidth = 0;   // characters, padding included; 0: from baseColWidth
  double defaultRowHeight = 15;  // points
  bool customHeight = false, zeroHeight = false, thickTop = false, thickBottom = false;
  uint8_t outlineLevelRow = 0, outlineLevelCol = 0;
};

enum class PaneState : uint8_t { None, Split, Frozen, FrozenSplit };
enum class PaneId : uint8_t { BottomRight, TopRight, BottomLeft, TopLeft };
enum class ViewMode : uint8_t { Normal, PageBreakPreview, PageLayout };

struct Selection {
  PaneId pane = PaneId::TopLeft;
  CellAddr active;
  uint32_t activeRange = 0;  // activeCellId
  std::vector<Range> ranges;  // sqref
};

struct SheetView {
  bool tabSelected = false, showGridLines = true, showRowColHeaders = true, showZeros = true, rightToLeft = false,
       showFormulas = false, showOutlineSymbols = true, showRuler = true, defaultGridColor = true,
       windowProtection = false, showWhiteSpace = true;
  ViewMode view = ViewMode::Normal;
  uint32_t zoomScale = 100, zoomScaleNormal = 0, zoomScalePageLayoutView = 0, zoomScaleSheetLayoutView = 0;
  CellAddr topLeft;
  uint32_t colorId = 64;  // the gridline color, an indexed color, when !defaultGridColor
  uint32_t workbookViewId = 0;
  // The pane: frozen panes give xSplit columns and ySplit rows; a split gives them in twips.
  PaneState paneState = PaneState::None;
  double xSplit = 0, ySplit = 0;
  CellAddr paneTopLeft;  // the bottom-right pane's first visible cell
  PaneId activePane = PaneId::TopLeft;
  std::vector<Selection> selections;
};

struct Hyperlink {
  Range ref;
  std::string target;    // an external address (through the relationship), else empty
  std::string location;  // a place in the workbook: "Sheet2!A1", a defined name
  std::string display, tooltip;
  std::string relId;
};

struct PageMargins {  // inches
  double left = 0.7, right = 0.7, top = 0.75, bottom = 0.75, header = 0.3, footer = 0.3;
};

enum class Orientation : uint8_t { Default, Portrait, Landscape };
enum class PageOrder : uint8_t { DownThenOver, OverThenDown };
enum class CellComments : uint8_t { None, AsDisplayed, AtEnd };
enum class PrintErrors : uint8_t { Displayed, Blank, Dash, NA };

struct PageSetup {
  uint32_t paperSize = 1;  // 1 Letter, 9 A4 ... (ECMA-376 §18.3.1.63)
  std::string paperWidth, paperHeight;  // "210mm": overrides paperSize when given
  uint32_t scale = 100;  // percent, 10-400
  uint32_t fitToWidth = 1, fitToHeight = 1;  // pages; 0: as many as needed
  int firstPageNumber = 1;
  bool useFirstPageNumber = false;
  Orientation orientation = Orientation::Default;
  PageOrder pageOrder = PageOrder::DownThenOver;
  bool blackAndWhite = false, draft = false, usePrinterDefaults = true;
  CellComments cellComments = CellComments::None;
  PrintErrors errors = PrintErrors::Displayed;
  uint32_t horizontalDpi = 600, verticalDpi = 600, copies = 1;
  // printOptions
  bool horizontalCentered = false, verticalCentered = false, headings = false, gridLines = false,
       gridLinesSet = true;
  PageMargins margins;
};

struct HeaderFooter {
  bool differentOddEven = false, differentFirst = false, scaleWithDoc = true, alignWithMargins = true;
  // Each in Excel's code language (&L, &C, &R, &P, &N, &D, &T, &F, &A, &"font", &12 ...).
  std::string oddHeader, oddFooter, evenHeader, evenFooter, firstHeader, firstFooter;
};

struct PageBreak {
  uint32_t id = 0;  // the row or column the break is before (0-based: the first one on the new page)
  uint32_t min = 0, max = 0;
  bool manual = false, pivot = false;
};

// sheetPr.
struct SheetProps {
  Color tabColor;
  bool summaryBelow = true, summaryRight = true, applyStyles = false, showOutlineSymbols = true;
  bool fitToPage = false, autoPageBreaks = true;
  bool filterMode = false, published = true;
  std::string codeName;
};

namespace detail {

// How the cell's t attribute says its value is stored.
enum class XmlType : uint8_t { N, S, Str, InlineStr, B, E, D };

enum CellFlag : uint8_t {
  kStylePresent = 1,  // s is written (it may say 0)
  kTypePresent = 2,   // t is written (it may say n)
  kHasValue = 4,      // has a value (v, or is for inline strings)
  kHasFormula = 8,    // has an f
  kSide = 16,         // has a CellSide entry: other attributes, the value's text, its content as written
  kPreserve = 32,     // an inline string's t has xml:space="preserve"
  kComputed = 64,     // the value was set by recalculation
};

// 16 bytes per stored cell.
struct CellRec {
  uint16_t col = 0;
  XmlType type = XmlType::N;
  uint8_t flags = 0;
  uint32_t style = 0;
  union {
    double number;
    uint32_t index;  // S: shared string id; Str, InlineStr: the sheet's text id; B: 0 or 1; E: ErrorCode
  } v{0.0};
};

// What a few cells need besides their CellRec.
struct CellSide {
  uint32_t cell = 0;           // index into the block's cells
  pkg::AttrSetId attrs = 0;    // attributes other than r, s and t (cm, vm, ph, unknown ones), and r
                               // itself when it isn't written as the model would write it
  uint32_t valueText = kNone;  // the v text as written, when the model would write the value otherwise
  uint32_t inner = kNone;      // the cell's whole content as written, when it isn't f, v, is in order
  bool noRef = false;          // the cell has no r attribute (or `attrs` carries it)
};

enum RowFlag : uint8_t {
  kRowHidden = 1, kRowCustomHeight = 2, kRowCollapsed = 4, kRowCustomFormat = 8, kRowThickTop = 16,
  kRowThickBottom = 32, kRowNoRef = 64,  // no r attribute (or `attrs` carries it)
};

struct RowRec {
  uint32_t row = 0;
  uint32_t first = 0, count = 0;  // its cells in the block
  pkg::AttrSetId attrs = 0;       // every attribute but r, as written
  float height = 0;
  uint32_t style = 0;
  uint8_t outlineLevel = 0;
  uint8_t flags = 0;
};

// A block of consecutive stored rows: the copy-on-write unit (plan §4.3 rule 5).
struct RowBlock {
  std::vector<RowRec> rows;
  std::vector<CellRec> cells;
  std::vector<std::pair<uint32_t, uint32_t>> formulas;  // cell index -> formula id, by cell
  std::vector<CellSide> sides;                          // by cell
  size_t Bytes() const;
};

struct FormulaRec {
  uint32_t text = kNone;  // in the sheet's text pool; kNone: no text (a cell sharing a formula)
  pkg::AttrSetId attrs = 0;
  FormulaKind kind = FormulaKind::Normal;
  bool alwaysCalc = false;
  uint32_t sharedIndex = kNone;
  Range ref;
  bool hasRef = false;
};

// XML the model keeps as written inside sheetData: comments, unknown elements, significant
// whitespace. `col` says where in the row: kBeforeRow before the row's element, kRowEnd inside it
// after its cells, else before the cell in that column. `row` kNone: at the end of sheetData.
struct Kept {
  static constexpr uint32_t kBeforeRow = 0xFFFFFFFE, kRowEnd = 0xFFFFFFFD;
  uint32_t row = kNone, col = kBeforeRow;
  std::string xml;
};

// The worksheet part around sheetData, as written: the writer puts it back as it was. Elements
// there that the model reads (views, columns, merges, hyperlinks, page setup) are typed too; S2's
// editing writes them from the model.
struct PartText {
  std::string prolog;    // after the XML declaration, before the root element
  std::string rootStart, beforeData, dataStart, afterData, rootEnd, epilog;
  bool hasData = false;  // the part has a sheetData element
  bool emptyData = false;  // written <sheetData/> with nothing inside
};

}  // namespace detail

// A sheet of a workbook. Cells are stored sparsely in copy-on-write blocks of rows, 16 bytes per
// stored cell plus a little per row, with strings in the workbook's shared string table and
// formulas in the sheet's own table, shared ones once per group.
//
// Reading from several threads at once is safe; changes (SetValue, the builder) are not, and no
// one may read a sheet while it loads (Workbook::LoadSheet).
class Sheet {
 public:
  Sheet(Workbook& book, std::string name);
  Sheet(const Sheet&) = delete;
  Sheet& operator=(const Sheet&) = delete;

  const std::string& Name() const { return name_; }
  SheetKind Kind() const { return kind_; }
  SheetState State() const { return state_; }
  bool Loaded() const { return loaded_; }
  const std::string& PartName() const { return part_; }  // "/xl/worksheets/sheet1.xml"; empty for CSV

  // ---- Cells
  // The stored cell at (row, col); false when there is none.
  bool GetCell(uint32_t row, uint32_t col, CellInfo& out) const;
  Value GetValue(uint32_t row, uint32_t col) const;
  // The format that shows at (row, col): the cell's own, else its row's (customFormat), else its
  // column's, else 0.
  uint32_t StyleAt(uint32_t row, uint32_t col) const;
  // Every stored cell inside `r`, row by row, left to right. Return false to stop.
  void ForEachCell(const Range& r, const std::function<bool(const CellInfo&)>& fn) const;
  // Stored cells: with a value, a formula, or only a format.
  size_t CellCount() const { return cellCount_; }
  // The cells stored (values and formats); nullopt for an empty sheet.
  std::optional<Range> UsedRange() const;
  // dimension, as the file states it.
  std::optional<Range> Dimension() const { return dimension_; }

  // ---- Formulas
  bool GetFormula(uint32_t row, uint32_t col, FormulaInfo& out) const;
  size_t FormulaCount() const { return formulaCells_; }
  // An attribute of a formula's f element ("r1", "dt2D"...), as written; empty when absent.
  std::string_view FormulaAttribute(const FormulaInfo& f, std::string_view local) const;

  // ---- Rows and columns
  RowInfo Row(uint32_t row) const;
  // Calls `fn` for each row the file stores (a row element), in order, within [first, last].
  void ForEachRow(uint32_t first, uint32_t last, const std::function<bool(uint32_t row, const RowInfo&)>& fn) const;
  // The column's settings; a ColumnInfo with no width when the file gives none.
  ColumnInfo Column(uint32_t col) const;
  const std::vector<ColumnInfo>& Columns() const { return cols_; }
  const SheetFormat& Format() const { return format_; }

  // ---- Layout
  const std::vector<Range>& Merges() const { return merges_; }
  // The merged range that covers (row, col), or null.
  const Range* MergeAt(uint32_t row, uint32_t col) const;
  const std::vector<SheetView>& Views() const { return views_; }
  const SheetProps& Props() const { return props_; }
  const std::vector<Hyperlink>& Hyperlinks() const { return links_; }
  const PageSetup& Page() const { return page_; }
  const HeaderFooter& HeadersFooters() const { return headerFooter_; }
  const std::vector<PageBreak>& RowBreaks() const { return rowBreaks_; }
  const std::vector<PageBreak>& ColBreaks() const { return colBreaks_; }

  // The worksheet's other content, which later pieces read (conditional formats, data validation,
  // tables, drawings, notes): the part as written, without its cells. Empty for CSV.
  std::string KeptXml() const;
  // The relationship ids of the drawing, legacy drawing (VML notes) and tables, as written.
  const std::string& DrawingRelId() const { return drawingRel_; }
  const std::string& LegacyDrawingRelId() const { return legacyDrawingRel_; }
  const std::vector<std::string>& TableRelIds() const { return tableRels_; }

  // ---- Changes
  // Sets a cell's value, keeping its format and formula: recalculated results (marked computed),
  // CSV values. Strings are copied into the sheet.
  void SetValue(uint32_t row, uint32_t col, const Value& v);

  // Memory the cells use (blocks, formulas, text), for the budgets.
  size_t ModelBytes() const;

  // ---- For the reader and writer (src/sheet/xlsx)
  const std::vector<std::shared_ptr<detail::RowBlock>>& Blocks() const { return blocks_; }
  const std::vector<detail::FormulaRec>& FormulaRecs() const { return formulas_; }
  const TextPool& Texts() const { return texts_; }
  const pkg::RoundTripStore& Store() const { return store_; }
  const std::vector<detail::Kept>& KeptInData() const { return kept_; }
  const detail::PartText& Text() const { return text_; }
  // An interned attribute set's attributes.
  const std::vector<pkg::XmlAttr>& Attributes(pkg::AttrSetId id) const { return store_.Attributes(id); }

 private:
  friend class SheetBuilder;
  friend class SheetLoader;
  friend class Workbook;
  friend class WorkbookLoader;

  struct Found {
    const detail::RowBlock* block = nullptr;
    const detail::RowRec* row = nullptr;
    const detail::CellRec* cell = nullptr;
    uint32_t cellIndex = 0;
  };
  bool Find(uint32_t row, uint32_t col, Found& f) const;
  const detail::RowRec* FindRow(uint32_t row, const detail::RowBlock** block = nullptr) const;
  void FillInfo(const detail::RowBlock& b, const detail::RowRec& r, uint32_t cellIndex, CellInfo& out) const;
  RowInfo RowInfoOf(const detail::RowRec& r) const;
  detail::RowBlock& MutableBlock(size_t index);  // a block of its own (copy on write)

  Workbook& book_;
  std::string name_, part_, relId_;
  SheetKind kind_ = SheetKind::Worksheet;
  SheetState state_ = SheetState::Visible;
  uint32_t sheetId_ = 0;
  bool loaded_ = false;

  std::vector<std::shared_ptr<detail::RowBlock>> blocks_;
  std::vector<uint32_t> blockFirstRow_;  // each block's first row, for the binary search
  size_t cellCount_ = 0, formulaCells_ = 0;
  std::vector<detail::FormulaRec> formulas_;
  std::vector<std::pair<uint32_t, CellAddr>> sharedMasters_;  // si -> the master's cell, by si
  TextPool texts_;
  pkg::RoundTripStore store_;
  std::vector<detail::Kept> kept_;
  detail::PartText text_;

  std::optional<Range> dimension_;
  std::vector<ColumnInfo> cols_;  // by first column, not overlapping
  SheetFormat format_;
  std::vector<Range> merges_;     // by first row
  uint32_t mergeMaxHeight_ = 0;   // the tallest merge, in rows, for MergeAt's search
  std::vector<SheetView> views_;
  SheetProps props_;
  std::vector<Hyperlink> links_;
  PageSetup page_;
  HeaderFooter headerFooter_;
  std::vector<PageBreak> rowBreaks_, colBreaks_;
  std::string drawingRel_, legacyDrawingRel_;
  std::vector<std::string> tableRels_;
};

// Appends rows and cells to a sheet, in order: rows ascending, and cells within a row by column.
// The xlsx reader and the CSV reader fill sheets with it.
class SheetBuilder {
 public:
  explicit SheetBuilder(Sheet& sheet);
  ~SheetBuilder();  // Finish()

  // Starts a row after the last one; false when `row` isn't after it (the row is then ignored).
  bool StartRow(uint32_t row, const RowInfo& info = {});
  // Cells of the current row, by column; false when out of order or outside the sheet.
  bool AddNumber(uint32_t col, double v, uint32_t style = 0);
  // Copies `text` into the workbook's shared string table.
  bool AddString(uint32_t col, std::string_view text, uint32_t style = 0);
  bool AddSharedString(uint32_t col, uint32_t id, uint32_t style = 0);
  bool AddBool(uint32_t col, bool b, uint32_t style = 0);
  bool AddError(uint32_t col, ErrorCode e, uint32_t style = 0);
  bool AddEmpty(uint32_t col, uint32_t style);  // a format only

  // Rows per block, and cells, before a new block starts.
  static constexpr uint32_t kBlockRows = 32, kBlockCells = 4096;

  // ---- For the xlsx reader: the record as the file writes it.
  detail::CellRec* AddCell(uint32_t col);
  detail::RowRec* CurrentRow();
  void SetFormula(uint32_t formulaId);
  detail::CellSide& Side();  // the current cell's side entry
  int64_t LastCol() const { return lastCol_; }  // the current row's last cell, -1 for none
  void Finish();

 private:
  void NewBlock();
  Sheet& s_;
  detail::RowBlock* block_ = nullptr;
  bool rowOpen_ = false;
  bool hasRow_ = false;
  uint32_t lastRow_ = 0;
  int64_t lastCol_ = -1;
};

}  // namespace fw::sheet
