#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "sheet/model/Workbook.h"

namespace fw::text {
class Fonts;
}

// The grid's geometry (plan §7.3 "Grid view"): column widths and row heights as Excel shows them.
// Excel counts both in pixels at 100 % (96 dpi): a column's width in the file is in characters of
// the default font's widest digit, and becomes whole pixels by Excel's rule; rows are whole pixels
// too. The grid keeps those pixels; points (0.75 pt a pixel) are what printing and text use.
namespace fw::sheet::view {

inline constexpr double kPtPerPx = 0.75;

// Fonts measured as Excel measures them: with GDI's metrics at 96 dpi, in whole pixels at a whole
// pixel size (11 pt is 15 px). The widest digit sets column widths; a line's height sets the rows
// Excel fits to their content; text widths decide "####" and overflow.
class FontMeasure {
 public:
  explicit FontMeasure(text::Fonts& fonts) : fonts_(fonts) {}
  struct Info {
    int digit = 7;    // the widest of 0-9, pixels
    int ascent = 14, descent = 4;  // pixels
    int rowHeight = 20;  // the height Excel gives a row of this font, pixels
    int space = 3;
  };
  // `f`'s fields that aren't given come from `base` (the workbook's default font).
  const Info& Of(const Font& f, const Font& base);
  // The widest digit in whole pixels at `dpi` (GDI's hinted advance): a printer's digit.
  int DigitAt(const Font& f, const Font& base, int dpi);
  // A text's width in pixels, with each character's GDI advance (no kerning, as Excel measures).
  int Width(std::wstring_view text, const Font& f, const Font& base);
  text::Fonts& Fonts() { return fonts_; }
  // The family a cell's font asks for (its own, else the default's), and its size.
  static std::wstring Family(const Font& f, const Font& base);
  static double Size(const Font& f, const Font& base) { return f.size > 0 ? f.size : base.size > 0 ? base.size : 11; }

 private:
  struct Face {
    Info info;
    void* face = nullptr;  // IDWriteFontFace*, kept alive by fonts_
    int ppem = 15;
    float upem = 2048;
    std::vector<int> ascii;  // advances of 32..126, pixels
  };
  Face& Get(const Font& f, const Font& base);
  text::Fonts& fonts_;
  std::map<std::tuple<std::wstring, int, bool, bool>, Face> faces_;
};

// Column and row positions of one sheet in Excel's pixels at 100 %. Hidden rows and columns have no
// size. Rows the file doesn't size are the sheet's default height, or as tall as their content
// needs (Excel fits a row without a stored height to its fonts and wrapped text).
class Grid {
 public:
  void Build(const Sheet& sheet, const Styles& styles, FontMeasure& measure);

  int MaxDigit() const { return mdw_; }
  int DefaultRowPx() const { return defaultRow_; }

  // ---- Columns, in pixels from the sheet's left edge.
  int64_t ColLeft(uint32_t col) const { return colLeft_[std::min<uint32_t>(col, kMaxCols)]; }
  int ColWidth(uint32_t col) const { return static_cast<int>(ColLeft(col + 1) - ColLeft(col)); }
  // The column under x (the last one past the end).
  uint32_t ColAt(int64_t x) const;
  bool ColHidden(uint32_t col) const { return ColWidth(col) == 0; }
  // The width Excel gives a column `chars` wide (file units, padding included).
  int CharsToPx(double chars) const;

  // ---- Rows.
  int64_t RowTop(uint32_t row) const;
  int RowHeight(uint32_t row) const;
  uint32_t RowAt(int64_t y) const;
  bool RowHidden(uint32_t row) const { return RowHeight(row) == 0; }
  int64_t Height() const { return RowTop(kMaxRows); }

  // ---- On paper, in points. Excel lays a page out at the printer's resolution (600 dpi): rows at
  // 6 dots a pixel (a 15 pt row is 120 dots), and columns as many characters wide as on screen in
  // the printer's digit, plus padding (Calibri 11: 47 dots a character and 30.6). The sheet then
  // prints 0.025 % larger than its dots: a 15 pt row is 14.4035 pt and 8.43 characters 51.22 pt,
  // as measured in Excel's PDFs (45 such rows no longer fit the 9 inches Letter leaves).
  static constexpr int kPrintDpi = 600;
  static constexpr double kPrintStretch = 1.00025;
  static constexpr double kPrintPad = 30.58;  // dots
  static constexpr double kPrintDotPt = 72.0 / kPrintDpi * kPrintStretch;
  static constexpr double kPrintRowPt = 6 * kPrintDotPt;
  float PrintX(uint32_t fromCol, uint32_t toCol) const {
    const auto at = [&](uint32_t c) { return colPrint_[std::min<uint32_t>(c, kMaxCols)]; };
    return static_cast<float>((at(toCol) - at(fromCol)) * 72.0 / kPrintDpi * kPrintStretch);
  }
  double PrintColPt() const { return printColPt_; }
  float PrintY(uint32_t fromRow, uint32_t toRow) const {
    return static_cast<float>((RowTop(toRow) - RowTop(fromRow)) * kPrintRowPt);
  }
  int64_t Width() const { return ColLeft(kMaxCols); }

  // ---- Outline groups: rows or columns shown or hidden by expanding and collapsing.
  void SetRowsHidden(uint32_t first, uint32_t last, bool hidden);
  void SetColsHidden(uint32_t first, uint32_t last, bool hidden);
  // Whether the file hides a row or column itself (not through a group the view changed).
  int StoredRowPx(uint32_t row) const;
  int StoredColPx(uint32_t col) const { return colPx_[std::min<uint32_t>(col, kMaxCols - 1)]; }

 private:
  void Prefix();
  int mdw_ = 7;
  double printColPt_ = 0.75;  // points a pixel of column width prints as, about
  int printDigit_ = 47;       // the default font's digit at kPrintDpi
  std::vector<double> colPrint_;  // column lefts at kPrintDpi
  double PrintDots(int px) const;
  void Lefts(uint32_t from);
  int defaultRow_ = 20;   // rows not stored apart (0 when the sheet hides them: zeroHeight)
  int naturalRow_ = 20;   // the default height a row has when shown
  int defaultCol_ = 64;
  std::vector<int> colPx_;       // as shown (0: hidden)
  std::vector<int> colStored_;   // as the file sizes them (hidden ones too)
  std::vector<int64_t> colLeft_;
  // Rows that aren't the default height: (row, pixels), by row, and the sum of the differences
  // from the default before each.
  std::vector<std::pair<uint32_t, int>> rows_;
  std::vector<int64_t> before_;
  std::vector<std::pair<uint32_t, int>> rowsStored_;  // the heights the rows have when shown
};

}  // namespace fw::sheet::view
