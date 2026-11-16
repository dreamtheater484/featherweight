#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// The workbook model's basic types (plan §7.3). Rows and columns are 0-based everywhere in the
// model: A1 is row 0, column 0; XFD1048576 is row 1048575, column 16383.
namespace fw::sheet {

inline constexpr uint32_t kMaxRows = 1048576;
inline constexpr uint32_t kMaxCols = 16384;
inline constexpr uint32_t kNone = 0xFFFFFFFF;

struct CellAddr {
  uint32_t row = 0, col = 0;
  bool operator==(const CellAddr&) const = default;
};

// A rectangle of cells, both ends included.
struct Range {
  uint32_t firstRow = 0, firstCol = 0, lastRow = 0, lastCol = 0;
  bool operator==(const Range&) const = default;
  bool Contains(uint32_t row, uint32_t col) const {
    return row >= firstRow && row <= lastRow && col >= firstCol && col <= lastCol;
  }
  static Range Cell(uint32_t row, uint32_t col) { return {row, col, row, col}; }
  static Range All() { return {0, 0, kMaxRows - 1, kMaxCols - 1}; }
};

// "B3" -> column 1, row 2; "$B$3" too. False when it isn't a cell inside the sheet's bounds.
bool ParseA1(std::string_view s, CellAddr& out);
// "A1:B2", "A1" (one cell), "A:B" (whole columns), "1:2" (whole rows), with or without $.
bool ParseRange(std::string_view s, Range& out);
// "B3" for row 2, column 1; "B3:C4" for a range ("B3" when it is one cell).
std::string CellName(uint32_t row, uint32_t col);
std::string RangeName(const Range& r);
// "A", "B", ..., "XFD" for columns 0..16383.
std::string ColumnName(uint32_t col);

// What a cell holds: for a formula, the value it last calculated (the cached value).
enum class ValueType : uint8_t { Empty, Number, String, Bool, Error };

enum class ErrorCode : uint8_t {
  Null,         // #NULL!
  Div0,         // #DIV/0!
  Value,        // #VALUE!
  Ref,          // #REF!
  Name,         // #NAME?
  Num,          // #NUM!
  NA,           // #N/A
  GettingData,  // #GETTING_DATA
  Spill,        // #SPILL!
  Calc,         // #CALC!
  Field,        // #FIELD!
  Blocked,      // #BLOCKED!
  Connect,      // #CONNECT!
  Busy,         // #BUSY!
  Unknown,      // #UNKNOWN!, and any other error text
};
// "#DIV/0!" for Div0; false for text that isn't an error Excel knows.
std::string_view ErrorText(ErrorCode e);
bool ParseError(std::string_view text, ErrorCode& out);

struct Value {
  ValueType type = ValueType::Empty;
  bool boolean = false;
  ErrorCode error = ErrorCode::Unknown;
  double number = 0;
  std::string_view text;  // String: UTF-8, valid until the workbook changes

  static Value Number(double v) { Value x; x.type = ValueType::Number; x.number = v; return x; }
  static Value Text(std::string_view t) { Value x; x.type = ValueType::String; x.text = t; return x; }
  static Value Bool(bool b) { Value x; x.type = ValueType::Bool; x.boolean = b; return x; }
  static Value Error(ErrorCode e) { Value x; x.type = ValueType::Error; x.error = e; return x; }
};

// A color as styles write it (SpreadsheetML's CT_Color); Styles::Resolve turns it into ARGB.
struct Color {
  enum class Kind : uint8_t { None, Auto, Rgb, Indexed, Theme };
  Kind kind = Kind::None;
  uint32_t value = 0;  // Rgb: 0xAARRGGBB; Indexed: the palette index; Theme: the theme color index
  double tint = 0;     // -1..1: darker or lighter
  bool operator==(const Color&) const = default;
  bool IsSet() const { return kind != Kind::None; }
};

}  // namespace fw::sheet
