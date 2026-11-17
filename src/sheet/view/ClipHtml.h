#pragma once

// Pasting from Excel, Word and the web (s2-plan "Clipboard"): the clipboard's "HTML Format" read
// as rows of cells. Tables give their rows (spans become merged areas); paragraphs, headings and
// list items outside tables are a row of one cell each, as Excel pastes them. Excel's own HTML
// adds the value under the shown text (x:num, x:str, x:bool, x:err) and each cell's format from
// its class (number format, font, fill, alignment); other HTML gives bold, italic, underline,
// strikethrough, text and fill colors and alignment.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sheet/model/Styles.h"
#include "sheet/model/Types.h"

namespace fw::sheet::view {

struct HtmlCell {
  uint32_t row = 0, col = 0;  // from the paste's top-left
  std::wstring text;          // as shown; line breaks inside the cell as \n
  ValueType type = ValueType::Empty;  // Excel's value (x:num and the rest); Empty: read `text` as typed
  double number = 0;
  bool boolean = false;
  std::string error;  // "#DIV/0!"
  // Another Featherweight workbook's copy: the formula (invariant, with "="), written for the cell
  // it was copied from (HtmlGrid::from plus this cell's place).
  std::string formula;
  // The format: what the HTML says; nothing set means the cell's own.
  std::string numFmt;  // Excel's mso-number-format, as a format code
  bool bold = false, italic = false, underline = false, strike = false;
  uint32_t color = 0, fill = 0;  // 0xFFRRGGBB; 0: not given
  double size = 0;               // points (Excel's HTML only)
  std::string font;              // the family (Excel's HTML only)
  HAlign align = HAlign::General;
  bool wrap = false;
  bool Styled() const {
    return !numFmt.empty() || bold || italic || underline || strike || color || fill || size > 0 || !font.empty() ||
           align != HAlign::General || wrap;
  }
};

struct HtmlGrid {
  uint32_t rows = 0, cols = 0;
  std::vector<HtmlCell> cells;  // by row, then column
  std::vector<Range> merges;    // from the paste's top-left
  bool excel = false;           // written by Excel (or Featherweight, as Excel writes it)
  bool hasFrom = false;         // Featherweight's copy: where its top-left cell was
  CellAddr from;
};

// `cfHtml`: the clipboard's bytes (CF_HTML: a header with offsets, then UTF-8 HTML), or plain HTML.
// False when there is nothing to paste (no table and no text), or more than `maxCells` cells.
bool ReadClipHtml(std::string_view cfHtml, HtmlGrid& out, size_t maxCells = 1000000);

}  // namespace fw::sheet::view
