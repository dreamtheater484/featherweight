#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "draw/Painter.h"

namespace fw::sheet {

class Workbook;

// Where the grid puts columns and rows: from the sheet's top left corner (A1's), in points at
// 100 % zoom. Hidden columns and rows have no width. Asked for one past the last column and row
// too.
class SheetLayout {
 public:
  virtual ~SheetLayout() = default;
  virtual double ColumnLeft(uint32_t col) const = 0;
  virtual double RowTop(uint32_t row) const = 0;
};

// A note (the older kind, a yellow box) or a threaded comment on a cell.
struct CellNote {
  uint32_t row = 0, col = 0;
  bool threaded = false;  // a threaded comment: a purple indicator; else a note: a red one
  bool resolved = false;  // a resolved thread
  bool shown = false;     // a note that stays shown (Show Note), drawn by Draw
  std::wstring author;    // a note's author; a thread's first person
  std::wstring text;      // the note's text; for a thread, each comment as "Name: text" on its own line
  // A thread's note for older Excel versions ("[Threaded comment]", a notice, "Comment:" and
  // "Reply:" lines), as Excel 2021 prints the thread at the end of the sheet; empty when there's none.
  std::wstring legacyText;
  float width = 0, height = 0;  // a note's box in points, from its drawing (0: none)
};

// What sits on a sheet above its cells (plan §7.2): pictures, shapes, text boxes, groups and
// charts from its drawing part, anchored to cells (two-cell, one-cell) or absolutely; notes and
// threaded comments with their indicators; a chart sheet's chart. Headless: the grid calls it
// from its painting and printing. Not thread-safe: use it from one thread at a time.
//
//   auto objects = SheetObjects::Load(book, sheet);
//   objects->Place(layout);                // and again when column widths or row heights change
//   objects->Draw(painter, visible, {});   // per pane, after the cells
class SheetObjects {
 public:
  // Reads sheet `index`'s objects. What can't be read is left out, or shown as a placeholder of
  // its size. Charts read their data from the workbook's cells, loading the sheets they refer to.
  static std::unique_ptr<SheetObjects> Load(Workbook& book, size_t index);
  ~SheetObjects();
  SheetObjects(const SheetObjects&) = delete;
  SheetObjects& operator=(const SheetObjects&) = delete;

  // Places the objects for the grid's columns and rows.
  void Place(const SheetLayout& layout);

  struct DrawOptions {
    // Leave out what doesn't print: objects set not to, the indicators, and shown notes unless the
    // sheet's page setup prints notes as displayed (notes printed at the end are the caller's,
    // from Notes()).
    bool printing = false;
    bool indicators = true;  // the notes' and comments' corner marks
    // A right-to-left sheet: where objects, notes and indicators go is mirrored, x becoming
    // mirrorX - x (the sheet's width, or whatever the caller mirrors about); what they show isn't
    // flipped, and indicators sit in the cells' top left corners. 0: left to right.
    float mirrorX = 0;
  };
  // Draws, in their order, the objects that meet `visible` (sheet points), then the shown notes
  // and the indicators. The painter's transform maps sheet points to the device; the caller clips
  // each pane and sets the zoom.
  void Draw(draw::Painter& painter, const draw::Rect& visible, const DrawOptions& options);

  // The placed objects (sheet points) that show, for the scroll extent, the print range and, later,
  // selection. Hidden ones are left out.
  struct Object {
    std::string name;
    draw::Rect rect;
    bool chart = false;
    bool prints = true;  // false: set not to print (xdr:clientData fPrintsWithSheet="0")
  };
  std::vector<Object> Objects() const;

  // ---- Notes and threaded comments
  const std::vector<CellNote>& Notes() const { return notes_; }
  // The note or thread on a cell (any cell of a merged area finds the area's); null when none.
  const CellNote* NoteAt(uint32_t row, uint32_t col) const;
  // A note popping up beside its cell (hover): its size in points, and drawing it into `box`.
  // `note` must be this object's own (from its Notes() or NoteAt()).
  draw::Size NoteSize(const CellNote& note);
  void DrawNote(draw::Painter& painter, const CellNote& note, const draw::Rect& box);

  // ---- A chart sheet
  bool IsChartSheet() const;
  // Cell values changed (an edit, a recalculation): charts read their data again when next drawn.
  void ValuesChanged();
  // Its chart, filling `rect` (the window, or the printed page's area).
  void DrawChartSheet(draw::Painter& painter, const draw::Rect& rect);
  // The chart's size as saved (its absolute anchor), which Excel prints it at; empty when none.
  draw::Size ChartSheetSize() const;

 private:
  struct Impl;
  SheetObjects();
  void ReadNotes();
  void PlaceNotes(const SheetLayout& layout);
  void DrawNotes(draw::Painter& painter, const draw::Rect& visible, const DrawOptions& options);
  std::unique_ptr<Impl> impl_;
  std::vector<CellNote> notes_;
};

}  // namespace fw::sheet
