#pragma once

#include <pugixml.hpp>

#include <cstdint>

// A table's grid (a p:graphicFrame holding a:tbl), changed in place: rows and columns added and
// removed with merged cells kept whole, cells merged and split, column widths and row heights. The
// frame's size follows the grid. One implementation for the edit API's table operations and for
// p2-objects' ObjectXml (whose row and column functions call these). False when it can't be done;
// the table may then be half changed, so the caller drops its copy (the edit layer's calls do).
namespace fw::slide::edit::table {

pugi::xml_node Of(pugi::xml_node frame);  // the frame's a:tbl, or null
int Rows(pugi::xml_node frame);
int Columns(pugi::xml_node frame);
// A new row before row `at` (0 to rows), like the row next to it, with empty cells; merged cells
// spanning the place grow. A new column the same way.
bool InsertRow(pugi::xml_node frame, int at);
bool InsertColumn(pugi::xml_node frame, int at);
// The row or column goes; merged cells over it shrink, and a merge starting in it starts in the
// next one. False for the last row or column (delete the table instead).
bool DeleteRow(pugi::xml_node frame, int row);
bool DeleteColumn(pugi::xml_node frame, int column);
// Cells row0..row1 x col0..col1 (inclusive, either order) become one, as PowerPoint's Merge Cells:
// the top left cell spans them and takes the others' text (their paragraphs after its own). False
// when the range is one cell, leaves the table, or cuts through a merged cell.
bool Merge(pugi::xml_node frame, int row0, int col0, int row1, int col1);
// The merged cell covering (row, col) back to its cells. False when it isn't merged.
bool Split(pugi::xml_node frame, int row, int col);
bool SetColumnWidth(pugi::xml_node frame, int column, int64_t emu);
bool SetRowHeight(pugi::xml_node frame, int row, int64_t emu);

}  // namespace fw::slide::edit::table
