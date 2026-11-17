#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sheet/model/Workbook.h"
#include "sheet/numfmt/NumFormat.h"
#include "sheet/view/Grid.h"
#include "text/LineLayout.h"

namespace fw::text {
class Face;
}

// What a cell shows, and its text laid out (plan §7.3 "Grid view"): the value through its number
// format at the column's width ("####" when a number doesn't fit), the font, the alignment Excel
// resolves General to, and the lines fwtext lays out, cached by cell.
namespace fw::sheet::view {

// The text of a value as the cell shows it, without layout: copying, find and the status bar use it.
// `widthPx` is the column's width in Excel's pixels (0: any width, as TEXT() and copying see it).
struct Shown {
  std::wstring text;    // with fills repeated and pads as the characters whose width they take
  std::vector<uint8_t> hidden;  // per character: 1 for a pad (`_x`), drawn as space
  uint32_t color = 0;   // ARGB from the format ([Red]); 0: the font's
  bool number = false;  // a number (dates too): right-aligned in General, and never overflows
  bool center = false;  // TRUE, FALSE and errors: centered in General
  bool overflow = false;  // "####"
  bool fill = false;      // a repeated character fills the cell (`*x`): never runs into the next
  uint32_t fillAt = 0, fillCount = 0;  // where the fill's characters are in `text`, and how many
};

// What a table's style and conditional formats change in a cell's text, over its own format.
struct TextLook {
  uint32_t fontColor = 0;      // ARGB; 0: the cell's own
  bool colorWins = false;      // over the number format's color ([Red]): a conditional format's
  int8_t bold = -1, italic = -1, strike = -1;  // -1: the cell's own
  int8_t underline = -1;       // an Underline; -1: the cell's own
  const NumberFormat* numFmt = nullptr;  // shown in this format instead
  bool operator==(const TextLook&) const = default;
};

class CellText {
 public:
  CellText(const Workbook& book, text::Fonts& fonts, FontMeasure& measure, const NumLocale& locale);

  // The cell's value as shown in `widthPx` (its column, or merged area), in its own format.
  void Show(const Sheet& sheet, const CellInfo& cell, int widthPx, Shown& out, const NumberFormat* format = nullptr);
  // A value shown with no width limit: copying, find. Empty for an empty cell.
  std::wstring Text(const Sheet& sheet, const CellInfo& cell);
  const NumberFormat& FormatOf(uint32_t numFmtId);
  const NumLocale& Locale() const { return locale_; }
  const Workbook& Book() const { return book_; }
  FontMeasure& Measure() { return measure_; }
  text::Fonts& Fonts() { return fonts_; }

  // A cell laid out: its lines in points from the text's top-left, and how to draw each run.
  struct Run {
    uint32_t color = 0xFF000000;
    bool hidden = false;  // a pad: takes room, isn't drawn
    bool underline = false, doubleUnderline = false, strike = false;
    bool accounting = false;  // an accounting underline: lower, and across the cell
    float underlinePos = 0, underlineThick = 0, strikePos = 0, strikeThick = 0;  // points
  };
  struct Layout {
    Shown shown;
    HAlign align = HAlign::Left;  // resolved: never General
    std::vector<text::Line> lines;
    std::vector<Run> runs;
    float width = 0;       // the widest line, points
    float pitch = 15;      // from one line's baseline to the next, points
    float ascent = 10.5f, descent = 3;  // the first line's, as Excel places it (GDI pixels as points)
    float size = 11;       // the font size, points
    int widthPx = -1;      // the width it was made for
    int textPx = -1;       // and the width its text was decided at
    TextLook look;         // and the look
    bool wrapped = false, stacked = false;
  };
  // The layout for a cell `widthPx` wide (wrapped text breaks inside it), made or from the cache.
  // `textPx`: the screen's width, which decides what the cell shows ("####", how General
  // shortens) when printing lays it out wider; -1: `widthPx`. Excel prints the screen's text.
  const Layout& Get(const Sheet& sheet, const CellInfo& cell, int widthPx, const TextLook& look = {}, int textPx = -1);
  // Forgets the cache: another sheet, recalculated values, other column widths.
  void Clear() { cache_.clear(); }

 private:
  void Make(const Sheet& sheet, const CellInfo& cell, int widthPx, const TextLook& look, int textPx, Layout& out);
  // Most cells' text is one line of Latin characters in one font. Cells ask for no kerning or
  // ligatures, so shaping it is each character's glyph and advance: a table per face has them,
  // where text::Layout's analysis and shaping cost ten times as much. False (nothing done) for
  // other characters and for a glyph the font lacks.
  bool LayOutSimple(std::wstring_view s, const text::TextStyle& style, Layout& out);
  struct Glyphs {  // U+0020 to U+00FF: glyph ids (0: missing) and advances per point
    std::wstring family;
    bool bold = false, italic = false;
    const text::Face* face = nullptr;
    uint16_t id[224] = {};
    float advance[224] = {};
  };
  std::vector<std::unique_ptr<Glyphs>> glyphs_;
  const Workbook& book_;
  text::Fonts& fonts_;
  FontMeasure& measure_;
  const NumLocale& locale_;
  std::unordered_map<uint32_t, std::unique_ptr<NumberFormat>> formats_;
  std::unordered_map<uint64_t, std::unique_ptr<Layout>> cache_;
  const Sheet* cacheSheet_ = nullptr;
};

// A color from the format's palette index (1..56), through the workbook's palette, as ARGB.
uint32_t FormatColor(const Styles& styles, int index);

}  // namespace fw::sheet::view
