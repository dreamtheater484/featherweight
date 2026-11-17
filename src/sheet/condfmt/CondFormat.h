#pragma once

// Conditional formats (s1-plan phase 3): a sheet's rules, from its <conditionalFormatting> elements
// and Excel 2010's extension list, evaluated into what a cell shows: the differential formats of the
// rules that hold (merged by priority), a color scale's fill, a data bar and an icon.
//
//   condfmt::SheetRules rules(book, sheetIndex, {.locale = &UserLocale(), .calc = &calc});
//   condfmt::Style st;
//   if (rules.At(row, col, st)) { /* paint the cell with st over its own format */ }
//   rules.Changed();  // after an edit or a recalculation
//
// Excel's rules, as measured on Excel 2021 (tests/sheet/excel_condfmt.py):
// - Rules apply in priority order (lowest number first), whatever the order written. For each
//   property (font color, bold, fill...) the first rule that sets it wins; a rule with stopIfTrue
//   that holds stops the rules after it, color scales, data bars and icons included. A color
//   scale sets the fill, so a higher-priority rule's fill hides it and the other way round.
// - Relative references in formulas are relative to the top-left of the ranges' bounding box.
// - Cell value rules compare as formulas do: numbers < text < TRUE/FALSE, text without case, an
//   empty cell as 0, "" or FALSE; errors never hold. "between" takes its bounds in either order.
// - Top/bottom, above average, color scales, data bars and icon sets read the range's numbers
//   only (not text, booleans or errors). Top n% takes floor(n% of the count), at least one; ties
//   all hold. Standard deviations are the population's.
// - Duplicates compare values: text without case, text that reads as a number in the region
//   ("07", "50%") as that number, errors by kind; empty cells count as "" but never show.
// - Text rules read numbers as General shows them; "contains" takes * and ? as wildcards (as
//   SEARCH does), "begins with" and "ends with" don't. An error doesn't contain anything, so
//   "doesn't contain" holds for errors and empty cells; "no blanks" holds for errors.
// - A color scale's color moves from the lower color toward the upper one, each channel truncated
//   toward the lower color. A value at or past an end takes that end's color.
// - In a differential format, a solid fill's color is its bgColor.

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "sheet/model/Styles.h"

namespace fw::sheet {
class Workbook;
class NumberFormat;
struct NumLocale;
namespace calc {
class WorkbookCalc;
}
}  // namespace fw::sheet

namespace fw::sheet::condfmt {

// Excel's icon sets, in its order; icons within a set are numbered from the lowest value's (0).
enum class IconSet : uint8_t {
  Arrows3, ArrowsGray3, Flags3, TrafficLights3, TrafficLights3Rimmed, Signs3, Symbols3, Symbols3Plain, Stars3,
  Triangles3, Arrows4, ArrowsGray4, RedToBlack4, Rating4, TrafficLights4, Arrows5, ArrowsGray5, Rating5, Quarters5,
  Boxes5, None
};
// "3Arrows" -> Arrows3 ... "5Boxes", "NoIcons"; TrafficLights3 for a name it doesn't know.
IconSet ParseIconSet(std::string_view name);
int IconCount(IconSet s);  // 3, 4 or 5; 0 for None

// A data bar, in fractions of the cell's width from its left edge (from its right edge when
// rightToLeft): the bar spans from..to, and the axis between negative and positive values is at
// `axis` (-1: none).
struct DataBar {
  float from = 0, to = 0;
  float axis = -1;
  uint32_t fill = 0;    // ARGB
  uint32_t border = 0;  // ARGB; 0: no border
  uint32_t axisColor = 0;
  bool gradient = true;  // fades from `fill` toward white at the bar's far end
  bool negative = false;
  bool rightToLeft = false;
};

// What the rules make of a cell: only the fields named in `set`.
struct Style {
  enum Field : uint16_t {
    kFontColor = 1 << 0, kBold = 1 << 1, kItalic = 1 << 2, kStrike = 1 << 3, kUnderline = 1 << 4, kFill = 1 << 5,
    kBorder = 1 << 6, kNumFmt = 1 << 7, kBar = 1 << 8, kIcon = 1 << 9, kHideValue = 1 << 10,
  };
  struct Side {
    BorderStyle style = BorderStyle::None;
    uint32_t color = 0;  // ARGB
  };
  uint16_t set = 0;
  bool bold = false, italic = false, strike = false;
  Underline underline = Underline::None;
  uint32_t fontColor = 0;  // ARGB
  // kFill: a pattern on a background (Solid: the background only), or a differential format's
  // gradient (its colors as written; Styles::Resolve them).
  PatternType pattern = PatternType::None;
  uint32_t fillColor = 0, patternColor = 0;  // ARGB
  const Fill* gradient = nullptr;
  Side left, right, top, bottom;  // kBorder: the sides whose style isn't None
  const NumberFormat* numFmt = nullptr;  // kNumFmt; owned by the SheetRules
  DataBar bar;                           // kBar
  IconSet iconSet = IconSet::None;       // kIcon
  uint8_t icon = 0;
  void Clear() { *this = Style(); }
};

struct Options {
  // Numbers shown as text (text rules), text read as numbers (duplicates), number formats' built-in
  // ids. Null: en-US.
  const NumLocale* locale = nullptr;
  // The formula engine, loaded (WorkbookCalc::Load): formula rules and thresholds. Without it,
  // only constants (5, "text", TRUE) are read and other formulas never hold.
  calc::WorkbookCalc* calc = nullptr;
  // Today, for time periods: a serial number in the workbook's date system. 0: the clock's.
  double today = 0;
};

class SheetRules {
 public:
  // Reads the sheet's rules (it must be loaded).
  SheetRules(const Workbook& book, size_t sheet, const Options& options = {});
  ~SheetRules();
  SheetRules(const SheetRules&) = delete;
  SheetRules& operator=(const SheetRules&) = delete;

  bool Empty() const;
  size_t RuleCount() const;
  // What the rules make of the cell: false when none applies (`out` is then cleared). Calls in row
  // order (as painting goes) are cheapest. Not thread-safe: the first call for a rule works out its
  // range's thresholds, rankings and duplicates.
  bool At(uint32_t row, uint32_t col, Style& out);
  // The sheet's values changed (an edit, a recalculation): range statistics are worked out again.
  void Changed();

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

}  // namespace fw::sheet::condfmt
