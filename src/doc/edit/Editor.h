#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "doc/model/Document.h"

// Editing a Document (plan §4.3 rule 4, milestone D2). Headless: no window, no layout.
//
// Every change is a transaction that replaces a run of sibling paragraphs (one paragraph when
// typing; two after a split; one after a join). The editor records each transaction's paragraphs
// before and after it, element subtrees and round-trip fragments included, so undo and redo put the
// document back exactly. Formatting commands, paste and replace are transactions too, grouped with
// BeginGroup/EndGroup into one undo step.
//
// Never damage a file (plan §3): an edit either keeps the document valid (fields, bookmarks,
// comments, content controls, tracked changes and kept unknown content split and join correctly)
// or is refused with a plain message and changes nothing.
//
// Positions are doc::Pos {para, offset}: a paragraph index (Document::Paragraphs()) and a UTF-16
// offset into its text, the same as layout::TextPos. Paragraph indices stay in document order: a
// transaction that adds or removes paragraphs renumbers the ones after it, and its Change says so.
namespace fw::doc::edit {

struct EditorState;  // Editor.cpp
struct TableOp;      // Editor.cpp
struct KeptRefs;

// What a transaction, undo or redo changed. Paragraphs [first, first + removed) were replaced by
// [first, first + added); later paragraphs moved by added - removed. Layout lays out again from the
// page `first` is on.
struct Change {
  uint32_t story = kNone;
  uint32_t first = kNone;
  uint32_t removed = 0, added = 0;
  bool structure = false;  // fields, lists or sections may read differently after it
  int32_t Shift() const { return static_cast<int32_t>(added) - static_cast<int32_t>(removed); }
};

// The outcome of an edit. On failure nothing changed and `message` says why, in plain language.
struct Result {
  bool ok = true;
  std::u16string message;
  Pos caret;  // where the caret goes after the edit
  explicit operator bool() const { return ok; }
};

// A selection: the anchor stays where it started, the caret moves. Empty when they are equal.
struct Selection {
  Pos anchor, caret;
  bool Empty() const { return anchor == caret; }
};

// Properties to set or remove in a run's w:rPr or a paragraph's w:pPr, by element name as the
// schema spells it ("b", "sz", "color", "rFonts", "pStyle", "jc", "spacing", "numPr/numId").
// An item with no attributes and remove false sets an on/off property on (<w:b/>). Attributes
// replace the ones named and keep the others.
struct PropEdit {
  struct Item {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;  // {"val", "24"}, {"w14:paraId", ...}
    bool remove = false;
  };
  std::vector<Item> items;
  PropEdit& Set(std::string name, std::vector<std::pair<std::string, std::string>> attrs = {}) {
    items.push_back({std::move(name), std::move(attrs), false});
    return *this;
  }
  PropEdit& Remove(std::string name) {
    items.push_back({std::move(name), {}, true});
    return *this;
  }
};

// A change made to the model directly rather than as a run of paragraphs: formatting, styles and
// lists (TreeEdit.h). It has been applied when it's recorded, and undoes and redoes itself.
struct CustomStep {
  virtual ~CustomStep() = default;
  virtual void Undo() = 0;
  virtual void Redo() = 0;
  std::vector<uint32_t> parts;  // the parts it changes (indexes into Document::Parts())
  // The paragraphs whose layout it changes, all of one story (their neighbours are laid out again
  // too). kNone: anything may read differently, and the whole document is laid out again.
  uint32_t first = kNone, count = 0;
  bool structure = false;  // with first: what follows may read differently too (Change::structure)
  // With first: the paragraphs from here to the range's end didn't change, only list numbers in
  // them may have (TreeEdit::Local(true)). kNone: all of the range changed.
  uint32_t renumberFrom = kNone;
  // Paragraphs the step adds to the model or takes out of it (a note's story, D4), in the order
  // it does so, each as the Change it is then. Listeners hear of them after the range above;
  // undoing reverses them, newest first.
  std::vector<Change> paraChanges;
};

class Editor {
 public:
  explicit Editor(Document& doc);
  ~Editor();
  Editor(const Editor&) = delete;
  Editor& operator=(const Editor&) = delete;

  const Document& Doc() const { return doc_; }
  // False, with the reason, when the document is shown but not edited (Document::Editable(), a part
  // the writer can't reproduce).
  bool CanEdit(std::u16string* why = nullptr) const;

  // ---- Positions
  // -1, 0 or 1 by document order. Positions in different stories compare by story.
  int Compare(Pos a, Pos b) const;
  // The paragraph before or after `para` in its story's reading order (table cells included);
  // kNone at either end.
  uint32_t PrevPara(uint32_t para) const;
  uint32_t NextPara(uint32_t para) const;
  uint32_t ParaLength(uint32_t para) const { return static_cast<uint32_t>(doc_.Para(para).text.size()); }

  // ---- Transactions. Each is one undo step unless inside a group, and changes nothing on failure.
  // Inserts text at `at`. A tab becomes w:tab, and U+000B, U+000C and U+000E a line, page and
  // column break (w:br); paragraph breaks ("\r", "\n", "\r\n") split the paragraph. The text takes
  // the formatting of the text before it.
  Result InsertText(Pos at, std::u16string_view text);
  // Deletes [from, to). Both ends in one story; across paragraphs they join. Section breaks in the
  // marks deleted go: their paragraphs join the section after them, which keeps its headers. A
  // footnote's or endnote's reference deleted takes its note with it, in the same undo step (D4).
  Result Delete(Pos from, Pos to);
  // Typing over a selection: deletes it, then inserts.
  Result Replace(Pos from, Pos to, std::u16string_view text);
  // Splits the paragraph at `at` (Enter). The new paragraph after it gets the same properties.
  Result SplitParagraph(Pos at);
  // Joins `para` with the paragraph after it (Delete at the end, Backspace at the start of the next).
  Result JoinParagraphs(uint32_t para);
  // Sets run properties on the text in [from, to), splitting runs at the ends.
  Result SetRunProps(Pos from, Pos to, const PropEdit& edit);
  // Sets the paragraph mark's run properties (w:pPr/w:rPr) of each paragraph in [first, last].
  Result SetMarkProps(uint32_t first, uint32_t last, const PropEdit& edit);
  // Sets paragraph properties on each paragraph from `first` to `last` in document order.
  Result SetParaProps(uint32_t first, uint32_t last, const PropEdit& edit);
  // Inserts a drawing at `at` (one character, U+FFFC): a w:drawing holding `xml`, a self-contained
  // wp:inline or wp:anchor, in the run of the text before it. `info` is its handle as the reader
  // would make it (ReadDrawingFrame); its node, part and position are filled in here. The handle
  // stays in Document::Drawings() when the insert is undone, at no position. Pictures.h makes the
  // XML and adds the picture's part.
  Result InsertDrawing(Pos at, std::string xml, const Drawing& info);
  // ---- Fields and notes (D4)
  // Puts runs at `at` (w:r elements made from XML in the w: namespace, every element and
  // attribute known where it goes, their w:t and w:instrText text included): between the runs
  // there, cutting one, with the text before them (in a link that ends at `at`, not one that
  // starts there). A note's reference, a field's characters. One undo step named `name`.
  Result InsertRuns(Pos at, std::string_view runsXml, std::u16string name);
  // Makes [from, to) the result of a new field with instruction `code` (" PAGEREF _Toc1 \h "): a
  // w:fldChar begin, the code in w:instrText and a separator at `from`, an end at `to`, each in a
  // run of its own with run properties `rPr` (a w:rPr's XML, or empty). The ends go in the
  // innermost container that holds both: in one paragraph, inside a link around them; across
  // paragraphs, in the paragraphs themselves (Word refuses a field split across a link, torture
  // finding 6). Refused across the edge of another field. One undo step named `name`.
  Result WrapInField(Pos from, Pos to, std::string_view code, std::string_view rPr, std::u16string name);
  // Makes the text in [from, to), in one paragraph, a hyperlink to `target`: a web address (a
  // relationship of the part, kept if the link is undone, which is harmless) or "#bookmark".
  // Refused where the text is already a link, or in a field, a content control or a tracked change.
  Result SetLink(Pos from, Pos to, std::string_view target);

  // ---- Tables (D3, plan §6.2)
  // Cells are named by positions in them: `from` and `to` in two cells of one table name the block
  // of cells between them, grown to hold every merged cell it touches whole, as Word selects
  // cells; in one cell, that cell. Each change is one undo step. The table's grid (w:tblGrid)
  // stays consistent with its cells' spans and merges, every row keeps a cell and every cell ends
  // with a paragraph. A table whose rows or cells are inside content controls, or with tracked cell
  // changes or old-style merges, can't change shape: that's refused, as is deleting cells that
  // hold a note's or comment's anchor or part of a field.
  struct CellInfo {
    Node tbl, tr, tc;
    uint32_t row = 0, rows = 1;  // its row, and the rows its vertical merge covers from there
    int32_t col = 0, span = 1;   // grid columns
    uint32_t tableRows = 0;
    int32_t tableCols = 0;
    Pos start, end;  // its text: the start of its first paragraph, the end of its last
  };
  std::optional<CellInfo> CellAt(Pos at) const;  // nullopt outside tables
  struct CellBlock {
    Node tbl;
    uint32_t row0 = 0, row1 = 0;
    int32_t col0 = 0, col1 = 0;
    std::vector<CellInfo> cells;  // each cell of the block once, in reading order
  };
  // nullopt unless `from` is in a table (`to` in another table: the cell of `from` alone).
  std::optional<CellBlock> BlockAt(Pos from, Pos to) const;
  // The two corners of the rows, columns or table around [from, to], for a selection.
  enum class Extent : uint8_t { Cells, Rows, Columns, Table };
  std::optional<std::pair<Pos, Pos>> Grow(Pos from, Pos to, Extent extent) const;
  // The cell after (or before) the one `at` is in, in reading order; nullopt at either end.
  std::optional<CellInfo> NextCell(Pos at, bool back = false) const;
  // Every cell of the table `at` is in, in reading order (a merged cell once); empty elsewhere.
  std::vector<CellInfo> TableCells(Pos at) const;

  // A new table of rows x cols at `at`: before the paragraph when `at` is at its start, after it
  // at its end, else between the halves of a split. Its columns share the text width; it takes
  // the document's "Table Grid" style, or single borders. The caret goes to the first cell.
  Result InsertTable(Pos at, uint32_t rows, uint32_t cols);
  // Paragraphs [first, last], following each other in one place, as a table: a row each, a cell
  // for each piece between separators (rows padded to the widest).
  Result TextToTable(uint32_t first, uint32_t last, char16_t separator = u'\t');
  // The table around `at` as paragraphs: a row each, its cells joined with the separator.
  Result TableToText(Pos at, char16_t separator = u'\t');
  Result DeleteTable(Pos at);
  // As many rows (columns) as the block covers, above or below (left or right) of it.
  Result InsertRows(Pos from, Pos to, bool below);
  Result InsertColumns(Pos from, Pos to, bool right);
  Result DeleteRows(Pos from, Pos to);
  Result DeleteColumns(Pos from, Pos to);
  // Empties the block's cells, each keeping one paragraph (Delete with cells selected).
  Result ClearCells(Pos from, Pos to);
  // The block as one cell; the cells' content joins in reading order (empty cells add nothing).
  Result MergeCells(Pos from, Pos to);
  // The cell as rows x cols cells; its content stays in the first. A merged cell splits into a
  // number of rows that divides its own.
  Result SplitCell(Pos at, uint32_t rows, uint32_t cols);
  // Widths in twips (1/1440 inch). The block's grid columns each `twips` wide.
  Result SetColumnWidth(Pos from, Pos to, int32_t twips);
  // Dragging a column border: grid line `line` (0 = the table's left edge, tableCols = its right)
  // moves by `delta`; the columns on either side give and take (the right edge: the last column;
  // the left edge: the indent and the first column).
  Result MoveColumnBorder(Pos at, int32_t line, int32_t delta);
  // `rule` "atLeast" or "exact"; twips 0: as high as the text needs.
  Result SetRowHeight(Pos from, Pos to, int32_t twips, std::string_view rule = "atLeast");
  // AutoFit: Window spreads the table over its room; Fixed keeps its column widths; Contents takes
  // `widths` (twips per grid column, measured by layout).
  enum class Fit : uint8_t { Window, Fixed, Contents };
  Result AutoFit(Pos at, Fit fit, const std::vector<int32_t>& widths = {});
  // Properties of the block's cells (w:tcPr: shd, vAlign, tcBorders/top, tcMar/start), its rows
  // (w:trPr: tblHeader, cantSplit) or the table (w:tblPr: tblStyle, tblBorders/insideH, jc, tblLook).
  Result SetCellProps(Pos from, Pos to, const PropEdit& edit);
  Result SetRowProps(Pos from, Pos to, const PropEdit& edit);
  Result SetTableProps(Pos at, const PropEdit& edit);
  // Borders of the block's cells, as Word's border button sets them: every side, none (over the
  // table's own), the block's outside edges, the lines between its cells, or one edge. `eighths`:
  // the width in eighths of a point.
  enum class CellBorders : uint8_t { All, None, Outside, Inside, Top, Bottom, Left, Right };
  Result SetCellBorders(Pos from, Pos to, CellBorders which, std::string_view style = "single", int32_t eighths = 4,
                        std::string_view color = "auto");

  // Groups the transactions until EndGroup into one undo step named `name` ("Bold", "Paste").
  // Groups nest; the outermost names the step.
  void BeginGroup(std::u16string name);
  void EndGroup();
  // Ends the group BeginGroup opened last, taking back what was done since, newest first, and
  // leaving no undo or redo step for it: a change made of several that fails part way.
  void CancelGroup();
  // Typing and deleting a character at a time coalesce into one step while they continue where the
  // last one ended. The view breaks the run when the caret moves on its own or time passes.
  void BreakTyping();

  // ---- Direct changes (TreeEdit.h). RecordStep adds an applied step to the history, as one undo
  // step or as part of the open group, rebuilds the model's indexes and tells the listeners that
  // every paragraph may look different. New elements take their node ids from NewNodeId, the
  // transactions' own counter.
  void RecordStep(std::u16string name, std::shared_ptr<CustomStep> step, Pos caret = {});
  pkg::NodeId NewNodeId();

  // ---- Undo and redo
  bool CanUndo() const;
  bool CanRedo() const;
  std::u16string UndoName() const;  // "Typing", "Bold", ...
  std::u16string RedoName() const;
  Result Undo();  // caret: where the undone change was
  Result Redo();

  // ---- Notifications and state
  using Listener = std::function<void(const Change&)>;
  // Called after every transaction, undo and redo, once per replaced paragraph run.
  void OnChange(Listener listener) { listeners_.push_back(std::move(listener)); }
  // Changed since opening, or since MarkSaved(). Undoing back to the saved state clears it.
  bool Modified() const;
  void MarkSaved();
  // The parts changed since opening (indexes into Document::Parts()); a save writes only these.
  std::vector<uint32_t> DirtyParts() const;

 private:
  void AddReferences(const KeptRefs& kept, Pos at);

  Document& doc_;
  Result EditTable(Pos from, Pos to, std::u16string name, const std::function<bool(TableOp&)>& op);

  std::unique_ptr<EditorState> impl_;
  std::vector<Listener> listeners_;
};

// A group (Editor::BeginGroup) for as long as it lives: ended when `done`, else cancelled, so a
// change made of several that fails part way leaves nothing.
struct EditGroup {
  EditGroup(Editor& editor, std::u16string name) : ed(editor) { ed.BeginGroup(std::move(name)); }
  ~EditGroup() { done ? ed.EndGroup() : ed.CancelGroup(); }
  EditGroup(const EditGroup&) = delete;
  EditGroup& operator=(const EditGroup&) = delete;
  Editor& ed;
  bool done = false;
};

// Deleting the section breaks in the marks of `marks` (body paragraphs): the section after the last
// of them goes on showing the headers and footers it did. Where it had no reference of a kind of
// its own, it showed the one of the section before it (Word's "Link to Previous"), which may be one
// that goes: it gets those references, the latest first. Its w:sectPr (kNoElem when no break goes)
// and the references, as XML for TreeEdit::AddXml.
struct KeptRefs {
  ElemId sectPr = kNoElem;
  std::vector<std::string> xml;
};
KeptRefs KeptReferences(const Document& doc, const std::vector<uint32_t>& marks);

// True when every character of `utf8` can be written in XML 1.0: valid UTF-8, without control
// characters (tab, line feed and carriage return aside), lone surrogates, U+FFFE or U+FFFF. A
// property value or link target with others would make the saved file unreadable.
bool XmlSafe(std::string_view utf8);

}  // namespace fw::doc::edit
