#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "doc/edit/Clip.h"

// Tables in the clipboard (D3), for Clip.cpp: which cell each copied paragraph is in, and pasting
// a clip with tables. Not for use elsewhere.
namespace fw::doc::edit::cliptable {

inline bool SameRow(const ClipPara& a, const ClipPara& b) { return a.table && a.table == b.table && a.row == b.row; }
inline bool SameCell(const ClipPara& a, const ClipPara& b) { return SameRow(a, b) && a.cell == b.cell; }

// Marks copied paragraphs with their table, row and cell: the cells below the innermost one that
// holds both ends of the copy (so a copy inside one cell is plain text, and a table nested in a
// copied cell comes as that cell's text).
class CellMarker {
 public:
  CellMarker(const Document& doc, uint32_t first, uint32_t last);
  void Mark(uint32_t para, ClipPara& cp);

 private:
  const Document& doc_;
  ElemId base_ = kNoElem;  // the w:tc both ends are in, or none
  uint32_t tables_ = 0;
  ElemId table_ = kNoElem;  // the table the last marked paragraph was in
  std::unordered_map<ElemId, uint32_t> rows_, cells_;  // of table_: each w:tr's and w:tc's index
};

// PasteClip for a clip with tables.
Result PasteTables(Editor& ed, Pos from, Pos to, const Clip& clip, uint64_t target);

}  // namespace fw::doc::edit::cliptable
