#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pkg/Status.h"
#include "sheet/model/Workbook.h"

// fwtool torture for workbooks (S2): what the random edits go through. The harness
// (SheetTorture.cpp) generates edits from what the workbook model reads, and a SheetTarget applies
// them through s2-model's edit API (src/sheet/edit), the way the grid's commands do.
namespace fwtool::sheettorture {

enum class Op : uint8_t {
  Value,        // type a value into a cell, as the region types it (text: what's typed; arg: "nl" or "us")
  Formula,      // type a formula, as the region types it (text, arg as Value)
  Clear,        // Delete: clear the contents of `range`
  Format,       // arg: bold, italic, underline, numfmt=<code>, fill=<RRGGBB>, color=<RRGGBB>, border=<thin|none>,
                // align=<left|center|right>, wrap, font=<name>, size=<points>
  InsertRows,   // range: the rows (columns ignored)
  DeleteRows,
  InsertCols,   // range: the columns (rows ignored)
  DeleteCols,
  InsertCells,  // arg: down or right
  DeleteCells,  // arg: up or left
  AddSheet,     // sheet: where the new tab goes; text: its name
  RenameSheet,  // text: the new name
  MoveSheet,    // toSheet: where it goes
  DeleteSheet,
  Merge,
  Unmerge,
  Sort,         // arg: "<column letter> <asc|desc>"
  Filter,       // turn the filter on `range` on or off; with arg "<column letter>" and text: keep that value
  Paste,        // copy `range` of `sheet`, paste at `to` of `toSheet`; arg: all, values, formats
  Undo,         // count steps
  Redo,
  Save,         // a save in the middle: checked as the last one is, then the edits go on
  // S3's objects
  Name,         // arg: the name; text: what it refers to (invariant, without "="), empty: delete it;
                // sheetScope: it belongs to `sheet`, else to the workbook
  Protect,      // Protect Sheet on `sheet`; arg: on, pw (with a password), lax (formatting and inserting
                // allowed), off
  ProtectBook,  // Protect Workbook (its structure); arg: on, pw, off
  Link,         // a link over `range`; arg: web (text: the address), tip (the same with a ScreenTip),
                // place (text: "Sheet2!A1" or a name), remove
  Note,         // the note on `range`'s first cell; arg: set, shown (text: the note), delete
  Thread,       // the comment thread on `range`'s first cell; arg: comment (text: a new thread, or a
                // reply to the one there), resolve (resolved or reopened), delete
  Table,        // arg: add (a table over `range`, its first row the headers), remove, totals (the
                // total row on or off), style=<name>: the table at `range`'s first cell
  Rule,         // a conditional format over `range`, Excel's newest first; arg: greater, between,
                // expression, contains (text: the value or formula), duplicate, top, above, scale,
                // bar, icons, clear (the rules there go)
  Validation,   // data validation on `range`, replacing what's there; arg: list, list-ref, whole,
                // decimal, length, custom (text: the list or formula), clear
  Chart,        // Insert > Chart of `range` at `to`'s cell; arg: column, bar, line, pie, doughnut, area, scatter, combo
  Object,       // the drawing's object with shape id `count`; arg: delete, move (to `range`'s first cell),
                // copy (pasted at `range`'s first cell), type-line, type-bar, type-combo, title (its chart edited),
                // tosheet (Move Chart > New sheet)
  PageSetup,    // Page Setup on `sheet`, one thing changed; arg: portrait, landscape, a4, letter,
                // scale=<percent>, fit=<wide>,<tall>, nofit, margins=narrow|wide, center, header,
                // gridlines, headings, order
  PageBreak,    // arg: insert (above and left of `range`'s first cell), remove (there), reset (all)
};

// One edit, as reproducers keep it. Sheets are tab indexes at the moment the edit is applied (names
// change with renames); ranges are A1 ranges of that sheet.
struct Edit {
  Op op = Op::Value;
  uint32_t sheet = 0;
  fw::sheet::Range range;
  uint32_t toSheet = 0;
  fw::sheet::Range to;   // Paste: the top-left cell it goes to
  std::string text;      // UTF-8
  std::string arg;
  uint32_t count = 1;    // Undo, Redo
  bool sheetScope = false;  // Name: the name belongs to `sheet`
  std::string target;    // what it aims at, for people: "a formula", "a merged area", "a table"
};

// One line per edit, and back:
//   value 0!B3 "12,5" nl / formula 1!C4 "=SUM(A1;2,5)" nl / clear 0!A1:B2 / format 0!A1:B2 bold /
//   insert-rows 0!5:6 / delete-cols 0!C:D / insert-cells 0!B2:C3 down / add-sheet 2 "Name" /
//   rename-sheet 1 "Name" / move-sheet 1 3 / delete-sheet 2 / merge 0!A1:C1 / unmerge 0!A1:C1 /
//   sort 0!A2:D20 "B desc" / filter 0!A1:D20 / filter 0!A1:D20 "B" "x" / paste 0!A1:B2 1!D5 values /
//   undo 2 / redo 1 / save /
//   name book Rate "Sheet1!$B$2" / name 1 Print_Area "Sheet2!$A$1:$D$9" / name book Rate - /
//   protect 0 on / protect-book off / link 0!B2:C3 web "https://example.com" / link 0!B2 remove /
//   note 0!B2 set "text" / note 0!B2 delete / thread 0!B2 comment "text" / thread 0!B2 resolve /
//   table 0!A1:D9 add / table 0!A1 style=TableStyleLight9 / rule 0!A1:C5 greater "5" /
//   rule 0!A1:C5 clear / validation 0!A1:A5 list "\"a,b,c\"" / chart 0!A1:C9 0!F2 column /
//   object 0!H5 3 move / object 0!A1 3 delete / pagesetup 0 landscape / pagebreak 0!A20 insert,
//   each optionally followed by "# target".
std::string FormatEdit(const Edit& edit);
bool ParseEdit(std::string_view line, Edit& edit);

enum class Applied : uint8_t {
  Done,
  Refused,  // the edit API didn't do it (blocked, or not possible there): fine, and counted
};

// What the torture test edits through. Each target owns its workbook.
class SheetTarget {
 public:
  virtual ~SheetTarget() = default;
  virtual const char* Name() const = 0;
  virtual const fw::sheet::Workbook& Book() const = 0;
  // Applies one edit, Undo and Redo included. `why` says why an edit was refused.
  virtual Applied Apply(const Edit& edit, std::string& why) = 0;
  // Saves as the app saves: the bytes of the file it would write.
  virtual fw::pkg::Status Save(std::vector<uint8_t>& out) = 0;
  // Undoes every step it can and says how many (-1: this target has no undo); RedoSteps redoes
  // that many. The harness checks that undoing everything gives the workbook as opened, and
  // redoing it the workbook as edited.
  virtual int UndoAll() { return -1; }
  virtual bool RedoSteps(int steps) { return steps == 0; }
  // What the edits changed that Save can't write yet ("styles, workbook part"); empty when the
  // save covers everything. The save's checks are skipped while it isn't empty.
  virtual std::string Unsaved() const { return {}; }
  // Whether there is a step to undo (a save that removed sheets ends the undo history).
  virtual bool CanUndo() const { return false; }
  // The parts the last Save wrote or removed (every other part must be copied byte for byte).
  std::vector<std::string> written;
};

// Opens a read workbook for editing through s2-model's edit API (sheet/edit/Edit.h). Built without
// it, every edit is refused. Either way Save writes the parts the model owns (the shared strings and
// the sheets) until the writer for edited workbooks lands.
std::unique_ptr<SheetTarget> OpenSheetTarget(std::unique_ptr<fw::sheet::Workbook> book, fw::pkg::Status& status);

}  // namespace fwtool::sheettorture

namespace fwtool {
struct Options;
// The workbook part of `fwtool torture`: the workbooks among `files`. `appendPairs`: pairs.tsv was
// begun by the Word documents of the same run.
int RunSheetTorture(const Options& options, const std::vector<std::filesystem::path>& files, bool appendPairs);
// `fwtool torture --replay` for a workbook reproducer.
int ReplaySheet(const std::filesystem::path& repro, const std::filesystem::path& outDir);
// Whether a reproducer is a workbook's (its file: line names a workbook).
bool IsSheetReproducer(const std::filesystem::path& repro);
}  // namespace fwtool
