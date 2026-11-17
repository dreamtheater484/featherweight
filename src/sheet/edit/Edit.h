#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sheet/model/Workbook.h"

// Editing a workbook (plan §4.3 rules 4 and 5, §7.3; S2). Headless: no window, no recalculation.
//
// Every change goes through an Editor (plan §4.3 rule 4: no other code changes the model). Each
// call is one undo step, unless inside BeginGroup/EndGroup, and changes nothing when it fails
// (Result says why, in plain language). Undo and redo cost about what the edit did: a step keeps
// the row blocks it touched as they were (the model's copy-on-write blocks, rule 5) and copies of
// the small lists it changed (columns, merges, views, names), and undoing swaps them back.
//
// Sheets are given by their index in tab order (Workbook::GetSheet). A sheet that isn't loaded yet
// is loaded before it's edited; edits that reach every sheet (renaming a sheet, structural edits)
// load them all.
//
// Recalculation isn't the editor's: the engine listens (OnChange), recalculates what the change
// dirtied and stores results with Sheet::SetValue, as it does after opening. Undo and redo report
// their changes the same way.
namespace fw::sheet {
struct NumLocale;
}

namespace fw::sheet::edit {

class Journal;

// What a change touched. Several flags may come together.
enum ChangeFlag : uint32_t {
  kValues = 1u << 0,      // values typed, pasted or cleared in `cells`
  kFormulas = 1u << 1,    // formulas set or removed in `cells` (their cached values too)
  kStyles = 1u << 2,      // cell formats in `cells` (and row or column formats in that band)
  kRowProps = 1u << 3,    // row heights, hidden, outline levels: rows cells.firstRow..lastRow
  kColProps = 1u << 4,    // column widths, hidden, outline levels: columns cells.firstCol..lastCol
  kMerges = 1u << 5,      // merged ranges inside `cells`
  kShift = 1u << 6,       // cells moved: rows, columns or cells inserted or deleted (`shift`)
  kSheets = 1u << 7,      // sheets added, deleted, moved or renamed: sheet indexes may differ now
  kSheetProps = 1u << 8,  // tab color, visibility, frozen panes
  kNames = 1u << 9,       // defined names
  kView = 1u << 10,       // selection, active sheet, top-left cell, zoom (not undo steps)
  kObjects = 1u << 11,    // the sheet's notes, links or drawings moved (a sort): load them again
  kRules = 1u << 12,      // the sheet's conditional formats or validations changed: read them again
};

// Cells inserted or deleted (kShift): along `axis`, `count` rows or columns at `at` (> 0 inserted
// before `at`, < 0 deleted from `at` on). `band` is what moved: whole rows or columns, or for
// cells shifted right or down, the rows or columns of the cells that moved.
struct Shift {
  enum class Axis : uint8_t { Rows, Columns };
  Axis axis = Axis::Rows;
  uint32_t at = 0;
  int32_t count = 0;
  Range band;
};

// One thing a transaction, undo or redo changed. `sheet` is the index after the change; -1 for
// workbook-wide changes (names, the list of sheets).
struct Change {
  int sheet = -1;
  uint32_t what = 0;  // ChangeFlag bits
  Range cells;
  Shift shift;        // with kShift
};

// The outcome of an edit. On failure nothing changed.
struct Result {
  bool ok = true;
  std::u16string message;
  explicit operator bool() const { return ok; }
  static Result Fail(std::u16string why) { return Result{false, std::move(why)}; }
};

// A cell's new content: a value, or a formula (the invariant form files store, without "=").
// What's typed in the region's form is converted before (s2-calc: formulas; s2-format: values).
struct CellInput {
  ValueType type = ValueType::Empty;  // the value; for a formula, its result when known
  double number = 0;
  bool boolean = false;
  ErrorCode error = ErrorCode::Unknown;
  std::string text;                   // a String value
  std::string formula;                // empty: no formula
  bool array = false;                 // a Ctrl+Shift+Enter formula over `arrayRange` (anchor: the cell)
  Range arrayRange;
  std::optional<uint32_t> style;      // a new format (a CellXfs() index); nullopt keeps the cell's

  static CellInput Empty() { return {}; }
  static CellInput Number(double v) { CellInput c; c.type = ValueType::Number; c.number = v; return c; }
  static CellInput Text(std::string t) { CellInput c; c.type = ValueType::String; c.text = std::move(t); return c; }
  static CellInput Bool(bool b) { CellInput c; c.type = ValueType::Bool; c.boolean = b; return c; }
  static CellInput Error(ErrorCode e) { CellInput c; c.type = ValueType::Error; c.error = e; return c; }
  static CellInput Formula(std::string f) { CellInput c; c.formula = std::move(f); return c; }
  CellInput& WithStyle(uint32_t s) { style = s; return *this; }
};

// Which parts of a sheet differ from the file (for the writer): bits set while an edit not undone
// changed them.
enum SheetPiece : uint32_t {
  kPieceCells = 1u << 0,    // sheetData: cells and rows
  kPieceCols = 1u << 1,     // cols
  kPieceMerges = 1u << 2,   // mergeCells
  kPieceViews = 1u << 3,    // sheetViews (panes, selection, zoom)
  kPieceProps = 1u << 4,    // sheetPr (tab color), sheetFormatPr
  kPieceLinks = 1u << 5,    // hyperlinks
  kPieceOther = 1u << 6,    // anything else around sheetData (structural edits rewrite references there)
};

class Editor {
 public:
  explicit Editor(Workbook& book);
  ~Editor();
  Editor(const Editor&) = delete;
  Editor& operator=(const Editor&) = delete;

  const Workbook& Book() const { return book_; }
  // False, with the reason, when the workbook is shown but not edited (CSV is always editable).
  bool CanEdit(std::u16string* why = nullptr) const;

  // ---- Cells
  Result SetCell(int sheet, uint32_t row, uint32_t col, const CellInput& in);
  // Several cells as one step (paste, fill, Ctrl+Enter), in any order.
  Result SetCells(int sheet, const std::vector<std::pair<CellAddr, CellInput>>& cells);
  // Delete: values and formulas go, formats stay.
  Result ClearContents(int sheet, const Range& r);
  // Formats go back to the row's or column's (or none); contents stay.
  Result ClearFormats(int sheet, const Range& r);
  // Contents and formats.
  Result ClearAll(int sheet, const Range& r);

  // ---- Formats. A cell's format is a CellXfs() index; Add* find an equal entry or append one
  // (appended entries aren't undone: an unused one is harmless, and the writer leaves it out).
  // The same format for every cell of `r`. Whole rows or columns set the row's or column's format
  // too, as Excel does, rather than storing every cell.
  Result SetStyle(int sheet, const Range& r, uint32_t style);
  // Each cell's format through `map` (bold on: same format, bold font), stored cells and empty
  // ones alike; whole rows or columns as SetStyle.
  Result MapStyles(int sheet, const Range& r, const std::function<uint32_t(uint32_t)>& map);
  uint32_t AddXf(const Xf& xf);
  // A cell style (Good, Heading 1...): the workbook's own with the same built-in id (or, for a
  // custom one, name), else `style` added with `styleXf` as its format. Its cellStyleXfs index,
  // for Xf::parent. Not undone, like the formats.
  uint32_t AddCellStyle(const CellStyle& style, const Xf& styleXf);
  uint32_t AddFont(const Font& font);
  uint32_t AddFill(const Fill& fill);
  uint32_t AddBorder(const Border& border);
  uint32_t AddNumFmt(std::string_view code);  // a custom format's id (164 on), or the built-in id
  uint32_t AddDxf(const Dxf& dxf);  // a differential format (conditional formats): its dxfs index

  // ---- Rows and columns. first..last, both included.
  Result SetRowHeight(int sheet, uint32_t first, uint32_t last, double points);  // 0: the default
  Result SetColumnWidth(int sheet, uint32_t first, uint32_t last, double chars);  // 0: the default
  Result HideRows(int sheet, uint32_t first, uint32_t last, bool hidden);
  Result HideColumns(int sheet, uint32_t first, uint32_t last, bool hidden);
  Result SetRowOutline(int sheet, uint32_t first, uint32_t last, uint8_t level, bool collapsed = false);
  Result SetColumnOutline(int sheet, uint32_t first, uint32_t last, uint8_t level, bool collapsed = false);
  // Structural edits, through the reference registry (§7.3): formulas, names, merges, links,
  // tables, conditional formats and the rest follow; #REF! where Excel puts it. Refused, with a
  // banner's text, when an unknown extension could hold references.
  Result InsertRows(int sheet, uint32_t at, uint32_t count);
  Result DeleteRows(int sheet, uint32_t first, uint32_t count);
  Result InsertColumns(int sheet, uint32_t at, uint32_t count);
  Result DeleteColumns(int sheet, uint32_t first, uint32_t count);
  enum class Direction : uint8_t { RightOrLeft, DownOrUp };
  Result InsertCells(int sheet, const Range& r, Direction shift);  // Right or Down
  // Cut and paste (or drag): cells `from` of `sheet` go to the block whose top-left cell is `to` on
  // sheet `toSheet`, values, formulas, formats and merges; the cells they leave are cleared and
  // those they cover replaced. References everywhere follow as Excel makes them (measured): one
  // wholly inside the cut cells moves with them (named with the other sheet when they go there);
  // covered cells that weren't cut drop out of references as deleted cells do (#REF! when nothing
  // is left); the rest stay. Rich text, notes, hyperlinks, conditional formats and validations
  // stay where they are (not moved yet). Refused over part of a merge or an array, and over
  // tables, PivotTables and data tables.
  Result MoveCells(int sheet, const Range& from, int toSheet, CellAddr to, std::u16string stepName = u"Paste");
  Result MoveCells(int sheet, const Range& from, CellAddr to, std::u16string stepName = u"Paste") {
    return MoveCells(sheet, from, sheet, to, std::move(stepName));
  }
  Result DeleteCells(int sheet, const Range& r, Direction shift);  // Left or Up

  // ---- Sorting, as Excel sorts: the rows of `r` put in `order` (the row at r.firstRow + order[i]
  // goes to r.firstRow + i); SortColumns the same for its columns. Only `r`'s cells move: values,
  // formats, links and notes with them, and drawings inside one row. Moved formulas are copied:
  // references without a sheet name follow (off the sheet: #REF!), ones naming a sheet stay, and
  // references from elsewhere don't follow. Validation, conditional formats and row heights stay.
  // Refused like Excel over merged cells or an array formula. `keys` go into the sheet's
  // sortState, Excel's record of the sort (Data > Reapply); none: it's left as it was.
  struct SortKey {
    uint32_t line = 0;  // the key's column (its row, for SortColumns)
    bool descending = false;
  };
  Result SortRows(int sheet, const Range& r, const std::vector<uint32_t>& order, const std::vector<SortKey>& keys = {});
  Result SortColumns(int sheet, const Range& r, const std::vector<uint32_t>& order,
                     const std::vector<SortKey>& keys = {});

  // ---- Merged cells. Merging keeps the top-left cell's content and clears the others' (Excel
  // asks first; the grid does). Merges that overlap `r` are taken into it.
  Result Merge(int sheet, const Range& r);
  Result Unmerge(int sheet, const Range& r);  // every merge inside `r`

  // ---- Sheets
  // A new worksheet at `at` (tab order); an empty name takes Excel's next "SheetN".
  Result AddSheet(int at, std::string name = {});
  Result RenameSheet(int sheet, std::string name);  // formulas and names that use it follow
  Result MoveSheet(int sheet, int to);
  Result DeleteSheet(int sheet);
  Result SetTabColor(int sheet, const Color& color);  // Color{} removes it
  Result SetSheetState(int sheet, SheetState state);  // the last visible sheet can't be hidden
  // Freezes `rows` rows and `cols` columns from the view's top-left cell; 0, 0 unfreezes.
  Result FreezePanes(int sheet, uint32_t rows, uint32_t cols);
  // The sheet's filter (Ctrl+Shift+L) with its columns' criteria, or none: written as Excel writes
  // it (autoFilter, sheetPr filterMode while a column filters, the hidden name
  // _xlnm._FilterDatabase). The rows it hides are the caller's (HideRows, in the same group).
  Result SetFilter(int sheet, const std::optional<SheetFilter>& filter);
  // The sheet's conditional formats or validations, as XML in Excel's form (sheet/data/Rules builds
  // it): `xml` every top-level conditionalFormatting element (or the dataValidations element), `ext`
  // the content of Excel's x14 extension for them; empty removes them. The rest of the part is kept.
  enum class RuleKind : uint8_t { ConditionalFormats, Validations };
  Result SetSheetRules(int sheet, RuleKind kind, const std::string& xml, const std::string& ext, std::u16string stepName);
  // ---- Tables (Insert > Table, Ctrl+T; s3-data). AddTable: `t` over its ref, its id, name and part
  // chosen when not given (unique in the workbook, Excel's "TableN"), written as Excel writes it and
  // listed in the sheet's tableParts; the header cells are the caller's, in the same group. SetTable:
  // the sheet's table `index` changed to `t` (name, size, header and total rows, filter buttons,
  // style and its options, columns), its part updated with the rest kept. RemoveTable: Convert to
  // Range (the cells stay).
  Result AddTable(int sheet, Table t);
  Result SetTable(int sheet, size_t index, const Table& t);
  Result RemoveTable(int sheet, size_t index);

  // ---- Package parts, for edit operations (charts, pictures, notes, comments, tables...): call
  // them inside the operation's group (Run, or BeginGroup and EndGroup); undo and redo take them
  // back with its step. False or empty when they can't: no group open, a workbook without a
  // package (opened from CSV, never saved as .xlsx), no such part.
  // A new part with its content type, and a relationship of type `relType` to it from `source`
  // ("/" the package, else a part: a sheet's PartName(), a drawing...): the relationship's id.
  std::string AddPart(std::string_view source, std::string_view relType, std::string_view name,
                      std::string_view contentType, std::string bytes);
  // A part that exists written anew.
  bool WritePart(std::string_view name, std::string bytes);
  // A part gone, with every relationship to it, its content type override and its own
  // relationships (what those point to is the caller's to remove).
  bool RemovePart(std::string_view name);
  // The first name "<stem><n><ext>" no part has, n from 1: NewPartName("/xl/charts/chart", ".xml").
  std::string NewPartName(std::string_view stem, std::string_view ext) const;
  std::string PartBytes(std::string_view name) const;  // empty when there's no such part
  // Excel's rules for a sheet name: 1-31 characters, none of : \ / ? * [ ], not starting or
  // ending with ', not "History", and no other sheet's name (without case). Empty when valid.
  std::u16string CheckSheetName(std::string_view name, int except = -1) const;

  // ---- View state: saved with the file, but not undo steps (as in Excel).
  void SetActiveSheet(int sheet);
  void SetSelection(int sheet, CellAddr active, const std::vector<Range>& ranges);
  void SetTopLeft(int sheet, CellAddr topLeft);
  void SetZoom(int sheet, uint32_t percent);

  // ---- Defined names. Adds the name, or replaces the one with the same name and scope.
  Result SetName(const DefinedName& name);
  Result DeleteName(std::string_view name, int sheet = -1);

  // ---- Steps
  // Groups the edits until EndGroup into one undo step named `name`. Groups nest; the outermost
  // names the step.
  void BeginGroup(std::u16string name);
  void EndGroup();
  // Ends the group BeginGroup opened last, undoing what was done in it.
  void CancelGroup();

  bool CanUndo() const;
  bool CanRedo() const;
  std::u16string UndoName() const;  // "Typing", "Format Cells", ...
  std::u16string RedoName() const;
  // The steps undo would take, newest first (the undo list), and redo likewise.
  std::vector<std::u16string> UndoNames() const;
  std::vector<std::u16string> RedoNames() const;
  Result Undo(size_t steps = 1);
  Result Redo(size_t steps = 1);
  static constexpr size_t kMaxSteps = 100;  // as Excel

  // ---- Notifications and state
  using Listener = std::function<void(const std::vector<Change>&)>;
  // Called after every edit, undo and redo, with everything it changed.
  void OnChange(Listener listener) { listeners_.push_back(std::move(listener)); }
  // Changed since opening or the last save; undoing back to the saved state clears it.
  bool Modified() const;
  // For the writer: the pieces of sheet `sheet` that differ from the file (SheetPiece bits), and
  // whether the workbook part (sheets, names, active tab), the styles or the shared strings do.
  uint32_t DirtyPieces(int sheet) const;
  bool WorkbookDirty() const;
  bool StylesDirty() const;
  bool StringsDirty() const;

  // ---- Saving (sheet/xlsx/Xlsx.h: PrepareSave, WriteSave on a worker thread, CommitSave)
  // While a save runs, the workbook is read-only: edits, undo and redo are refused (view state
  // still changes), and the engine must not write values (it checks Saving()).
  void BeginSave();
  bool Saving() const;
  void EndSave();  // the save failed: everything stays as it was before BeginSave
  // The save reached the file: the workbook now stands for it. Each sheet written gets the text
  // the file has around its cells (and a sheet added since opening its part and relationship);
  // a shared strings part the save added is the workbook's from now on.
  struct Written {
    const Sheet* sheet = nullptr;
    std::string part, relId;
    detail::PartText text;
  };
  // A save that removed sheets ends the undo history, as deleting a sheet does in Excel: their parts
  // are no longer in the file to bring back.
  void Saved(std::vector<Written> sheets, bool stringsWritten, std::string newStringsPart, FileFormat format,
             bool sheetsRemoved);
  // Parts an earlier save wrote from the model: the package still holds what the file had when
  // it was opened, so every save writes them from the model.
  bool MustWrite(int sheet) const;
  bool MustWriteStrings() const;
  // The current state counts as saved, without a file (tools and tests; saving uses CommitSave).
  void MarkSaved();
  // Saved by another writer (a CSV file): Modified() is false, the book's format is `format`, and
  // everything edited still counts as unsaved for the next .xlsx save (which writes it all anyway).
  void SavedOutside(FileFormat format);
  // The region numbers typed into tables' headers are shown in, as text (default: en-US).
  void SetLocale(const NumLocale* locale) { locale_ = locale; }

  // ---- Crash recovery (Journal.h)
  // The edits from now on are recorded in `journal` (nullptr: none).
  void SetJournal(Journal* journal);
  // Makes the edits a journal recorded again, in order, on the workbook reopened as it was when the
  // journal started. Stops at the first one that can't be read or made (the edits before it stay);
  // `applied` counts the records applied. Recorded into this editor's own journal, if it has one.
  Result Replay(std::string_view records, size_t* applied = nullptr);

 private:
  struct State;
  struct Rec;  // records an edit in the journal (Edit.cpp)
  void DropHistory();
  // Rows, columns or cells inserted or deleted (the public calls above build `shift`).
  Result ShiftCells(int sheet, const Shift& shift, std::u16string stepName);
  Result SortLines(int sheet, const Range& r, const std::vector<uint32_t>& order, const std::vector<SortKey>& keys, bool rows);
  // A sheet renamed, moved or deleted: every formula and part that names sheets (cells, names,
  // rules, sparklines, charts, drawings, controls, tables, pivot caches) through `rewrite` (a
  // formula's text and its sheet's index); `touches` says cheaply whether a text may need it. Sheet
  // `skip` (one being deleted) is left out; `from` and `to` rename pivot caches' sources.
  void RewriteSheetRefs(int skip, const std::function<std::string(std::string_view, int)>& rewrite,
                        const std::function<bool(std::string_view)>& touches, std::string_view from = {},
                        std::string_view to = {});
  // Header cells of tables among `cells`, after they were written: each holds its text as Excel
  // shows it (a number as formatted; cleared: "ColumnN"), unique in its table (Excel adds 2, 3...),
  // and its column takes that name, structured references everywhere following.
  void FixHeaders(Sheet& s, const std::vector<CellAddr>& cells);
  Workbook& book_;
  std::unique_ptr<State> s_;
  const NumLocale* locale_ = nullptr;
  Journal* journal_ = nullptr;
  int calls_ = 0;  // Editor calls under way: only the outermost records itself
  std::vector<Listener> listeners_;
};

// A group (Editor::BeginGroup) for as long as it lives: ended when `done`, else cancelled, so an
// edit made of several that fails part way leaves nothing.
struct EditGroup {
  EditGroup(Editor& editor, std::u16string name) : ed(editor) { ed.BeginGroup(std::move(name)); }
  ~EditGroup() { done ? ed.EndGroup() : ed.CancelGroup(); }
  EditGroup(const EditGroup&) = delete;
  EditGroup& operator=(const EditGroup&) = delete;
  Editor& ed;
  bool done = false;
};

}  // namespace fw::sheet::edit
