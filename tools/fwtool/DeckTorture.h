#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pkg/Status.h"
#include "slide/Presentation.h"

// fwtool torture for decks (P2's "never damage a file" gate, docs/p2-plan.md). The harness
// (DeckTorture.cpp) generates edits from what the presentation model reads, and a DeckTarget applies
// them through p2-model's edit API (src/slide/edit), the way the presentation tab's commands do.
namespace fwtool::decktorture {

enum class Op : uint8_t {
  NewSlide,    // a new slide after `slide` (-1: first) with the deck's layout number `count`
  Duplicate,   // duplicate `slide` (the copy goes after it)
  DeleteSlide,
  MoveSlide,   // `slide` goes to index `to`
  Hide,        // arg: on, off
  Layout,      // `slide` gets the deck's layout number `count`
  Type,        // type `text` into shape `shape` of `slide`; arg: start, end, replace (all its text), para
               // (a new paragraph at the end)
  Notes,       // `slide`'s speaker notes become `text`
  Move,        // shape `shape` moves by `dx`, `dy` points
  Resize,      // shape `shape` gets `dx` x `dy` times its size, from its top-left corner
  Rotate,      // shape `shape` turns to `dx` degrees
  DeleteShape,
  Group,       // the shapes in `shapes` (ids, on `slide`, top level) become one group
  Ungroup,     // group `shape` becomes its shapes
  Order,       // arg: front, back, forward, backward
  Format,      // shape `shape`; arg: fill=RRGGBB|none|gradient, line=RRGGBB|none, width=<points>,
               // dash=<preset>, shadow=on|off (through fwslideobjects, as the Format panel), bold, italic,
               // size=<points>, color=RRGGBB (the text ones over all its text)
  Undo,        // count steps
  Redo,
  Save,        // a save in the middle: checked as the last one is, then the edits go on
  // Design (p2-slides):
  Theme,        // master `slide` (an index into the deck's masters) gets a built-in-like theme; arg: light, dark
  MasterShapes, // arg on, off: the master's and layout's graphics shown on `slide`
  HeaderFooter, // `slide` (-1: every slide) gets slide numbers, a footer and a date as arg says: letters of
                // "n" (number), "f" (footer, `text`), "d" (date), "t" (not on the title slide), or "-"
  // Objects (p2-objects, through fwslideobjects and the editor as the Insert tab and the panels do):
  Insert,       // on `slide`, arg: shape:<preset>, textbox, line, arrow, picture, table:<rows>x<columns>,
                // chart:<0-3> (column, bar, line, pie), at (dx, dy) points, `w` x `h` points
  Crop,         // picture `shape`: dx, dy, w, h are the fractions cut from the left, top, right, bottom
  Table,        // table `shape`; arg: insert-row, delete-row, insert-col, delete-col (nums: at, count),
                // merge (nums: row0 col0 row1 col1), split (nums: row col)
};

// One edit, as reproducers keep it. Slides are indexes at the moment the edit is applied; shapes are
// cNvPr ids on that slide.
struct Edit {
  Op op = Op::Type;
  int slide = 0;
  int to = 0;              // MoveSlide
  uint32_t shape = 0;
  std::vector<uint32_t> shapes;  // Group
  double dx = 0, dy = 0;   // Move (points), Resize (factors), Rotate (dx: degrees), Insert, Crop
  double w = 0, h = 0;     // Insert (points), Crop
  std::vector<int> nums;   // Table
  std::string text;        // UTF-8
  std::string arg;
  uint32_t count = 1;      // Undo, Redo; NewSlide, Layout: the layout's number in the deck
  std::string target;      // what it aims at, for people: "a placeholder", "a group", "a table"
};

// One line per edit, and back:
//   new-slide 3 L2 / duplicate 2 / delete-slide 4 / move-slide 2 5 / hide 1 on / layout 1 L4 /
//   type 2#5 end "text" / notes 2 "text" / move 2#5 12.5 -30 / resize 2#5 1.2 0.8 / rotate 2#5 45 /
//   delete 2#5 / group 2#5,7,9 / ungroup 2#11 / order 2#5 front / format 2#5 fill=C00000 /
//   undo 2 / redo 1 / save / theme 0 dark / master-shapes 2 off / header-footer -1 nf "footer text" /
//   insert 2 shape:rightArrow 100 80 200 120 / crop 2#5 0.1 0 0.2 0 / table 2#5 merge 0 0 1 1,
//   each optionally followed by "# target".
std::string FormatEdit(const Edit& edit);
bool ParseEdit(std::string_view line, Edit& edit);

enum class Applied : uint8_t {
  Done,
  Refused,  // the edit API didn't do it (blocked, or not possible there): fine, and counted
};

// What the torture test edits through. Each target owns its presentation.
class DeckTarget {
 public:
  virtual ~DeckTarget() = default;
  virtual const char* Name() const = 0;
  // The presentation as edited so far (non-const: slides and text are read on first use).
  virtual fw::slide::Presentation& Deck() = 0;
  // Applies one edit, Undo and Redo included. `why` says why an edit was refused.
  virtual Applied Apply(const Edit& edit, std::string& why) = 0;
  // Saves as the app saves: the bytes of the file it would write. Fills `written`.
  virtual fw::pkg::Status Save(std::vector<uint8_t>& out) = 0;
  // Undoes every step it can and says how many (-1: this target has no undo); RedoSteps redoes
  // that many.
  virtual int UndoAll() { return -1; }
  virtual bool RedoSteps(int steps) { return steps == 0; }
  virtual bool CanUndo() const { return false; }
  // The parts the last Save wrote or removed (every other part must be copied byte for byte).
  std::vector<std::string> written;
};

// Opens a read presentation for editing through p2-model's edit API (slide/edit/Edit.h). Built
// without it, every edit is refused and Save writes the package as it was read.
std::unique_ptr<DeckTarget> OpenDeckTarget(std::unique_ptr<fw::slide::Presentation> deck, fw::pkg::Status& status);

}  // namespace fwtool::decktorture

namespace fwtool {
struct Options;
bool IsDeckFile(const std::filesystem::path& file);
// The deck part of `fwtool torture`: the decks among `files`. `appendPairs`: pairs.tsv was begun by
// the documents or workbooks of the same run.
int RunDeckTorture(const Options& options, const std::vector<std::filesystem::path>& files, bool appendPairs);
// `fwtool torture --replay` for a deck reproducer.
int ReplayDeck(const std::filesystem::path& repro, const std::filesystem::path& outDir);
// Whether a reproducer is a deck's (its file: line names a deck).
bool IsDeckReproducer(const std::filesystem::path& repro);
}  // namespace fwtool
