#pragma once

#include <cstdint>
#include <string_view>

#include "sheet/model/Styles.h"

// How a table's style formats its cells, as Excel draws it: Excel's 60 built-in table styles
// (TableStyleLight1-21, Medium1-28, Dark1-11, read from Excel 2021) and a workbook's own.
namespace fw::sheet {

struct Table;
class Sheet;

// What a table style gives one cell. The grid paints it under the cell's own format: the cell's
// fill, where it has one, covers the table's; the table's font color shows where the cell's font
// has the default font's color; bold and italic add to the cell's; a border the cell has on a side
// wins over the table's. Each side is the line the cell shares with its neighbor, so two cells
// side by side report the same line.
struct TableCellFormat {
  Fill fill;        // pattern None and no gradient: none
  Color fontColor;  // Kind::None: none
  bool bold = false, italic = false;
  BorderSide left, top, bottom, right;
  bool Empty() const;
};

// Excel's built-in table style of that name; null for any other name.
const TableStyle* BuiltInTableStyle(std::string_view name);
// A table style by name: the workbook's own, else a built-in one; null when there's none.
const TableStyle* FindTableStyle(const Styles& styles, std::string_view name);

// The format the table's style and options give a cell of the table (empty outside it).
TableCellFormat TableFormat(const TableStyle& style, const Table& table, uint32_t row, uint32_t col);
TableCellFormat TableFormat(const Styles& styles, const Table& table, uint32_t row, uint32_t col);
// The same for any cell of a sheet: empty where no table covers it.
TableCellFormat TableFormatAt(const Styles& styles, const Sheet& sheet, uint32_t row, uint32_t col);

// A table style element from a dxf (the reader, for a workbook's own table styles).
TableStyleElement TableElementFromDxf(const Dxf& dxf, uint32_t stripeSize);

}  // namespace fw::sheet
