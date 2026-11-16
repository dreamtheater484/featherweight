#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sheet/model/Types.h"

// A workbook's formatting (xl/styles.xml, ECMA-376 Part 1 §18.8), as cells refer to it: a cell's
// style index picks one of CellXfs(), which names a number format, a font, a fill and a border,
// and carries the alignment and protection.
namespace fw::sheet {

enum class Underline : uint8_t { None, Single, Double, SingleAccounting, DoubleAccounting };
enum class VertAlign : uint8_t { Baseline, Superscript, Subscript };
enum class FontScheme : uint8_t { None, Major, Minor };

struct Font {
  std::string name;  // empty: not given (the default font's)
  double size = 0;   // points; 0: not given
  bool bold = false, italic = false, strike = false, outline = false, shadow = false, condense = false,
       extend = false;
  Underline underline = Underline::None;
  VertAlign vertAlign = VertAlign::Baseline;
  Color color;
  int family = 0, charset = -1;
  FontScheme scheme = FontScheme::None;
  bool operator==(const Font&) const = default;
};

enum class PatternType : uint8_t {
  None, Solid, MediumGray, DarkGray, LightGray, DarkHorizontal, DarkVertical, DarkDown, DarkUp, DarkGrid,
  DarkTrellis, LightHorizontal, LightVertical, LightDown, LightUp, LightGrid, LightTrellis, Gray125, Gray0625
};

struct GradientStop {
  double position = 0;  // 0..1
  Color color;
  bool operator==(const GradientStop&) const = default;
};

struct Fill {
  PatternType pattern = PatternType::None;
  Color fg, bg;  // a solid fill's color is fg
  // A gradient fill instead of a pattern.
  bool gradient = false;
  bool path = false;  // a path gradient (left/right/top/bottom), else linear at `degree`
  double degree = 0, left = 0, right = 0, top = 0, bottom = 0;
  std::vector<GradientStop> stops;
  bool operator==(const Fill&) const = default;
};

enum class BorderStyle : uint8_t {
  None, Thin, Medium, Dashed, Dotted, Thick, Double, Hair, MediumDashed, DashDot, MediumDashDot, DashDotDot,
  MediumDashDotDot, SlantDashDot
};

struct BorderSide {
  BorderStyle style = BorderStyle::None;
  Color color;
  bool operator==(const BorderSide&) const = default;
};

struct Border {
  BorderSide left, right, top, bottom, diagonal, vertical, horizontal;  // start/end are read as left/right
  bool diagonalUp = false, diagonalDown = false, outline = true;
  bool operator==(const Border&) const = default;
};

enum class HAlign : uint8_t { General, Left, Center, Right, Fill, Justify, CenterContinuous, Distributed };
enum class VAlign : uint8_t { Bottom, Top, Center, Justify, Distributed };

struct Alignment {
  HAlign horizontal = HAlign::General;
  VAlign vertical = VAlign::Bottom;
  uint16_t rotation = 0;  // textRotation: 0-90 counterclockwise, 91-180 clockwise (degrees - 90), 255 stacked
  bool wrap = false, shrinkToFit = false, justifyLastLine = false;
  uint16_t indent = 0;
  int relativeIndent = 0;
  uint8_t readingOrder = 0;  // 0 context, 1 left to right, 2 right to left
  bool operator==(const Alignment&) const = default;
};

struct Protection {
  bool locked = true, hidden = false;
  bool operator==(const Protection&) const = default;
};

// A cell format (an xf in cellXfs or cellStyleXfs).
struct Xf {
  uint32_t numFmt = 0, font = 0, fill = 0, border = 0;
  uint32_t parent = 0;  // cellXfs: the cell style's xf (cellStyleXfs index)
  Alignment alignment;
  Protection protection;
  bool applyNumFmt = false, applyFont = false, applyFill = false, applyBorder = false, applyAlignment = false,
       applyProtection = false;
  bool quotePrefix = false, pivotButton = false;
};

struct CellStyle {
  std::string name;
  uint32_t xf = 0;          // cellStyleXfs index
  int builtinId = -1;       // -1: not built in
  int outlineLevel = -1;    // iLevel, for RowLevel_n and ColLevel_n
  bool hidden = false, customBuiltin = false;
};

// A differential format (conditional formats, tables): only what it sets.
struct Dxf {
  std::optional<Font> font;  // the font's given fields; Font::size 0 and an empty name mean not given
  bool fontBold = false, fontItalic = false, fontStrike = false, fontUnderline = false;  // which flags it sets
  std::optional<uint32_t> numFmtId;
  std::string numFmtCode;
  std::optional<Fill> fill;
  std::optional<Border> border;
  std::optional<Alignment> alignment;
  std::optional<Protection> protection;
};

class Styles {
 public:
  Styles();

  const std::vector<Xf>& CellXfs() const { return cellXfs_; }
  const std::vector<Xf>& CellStyleXfs() const { return cellStyleXfs_; }
  const std::vector<Font>& Fonts() const { return fonts_; }
  const std::vector<Fill>& Fills() const { return fills_; }
  const std::vector<Border>& Borders() const { return borders_; }
  const std::vector<CellStyle>& CellStyles() const { return cellStyles_; }
  const std::vector<Dxf>& Dxfs() const { return dxfs_; }

  // The cell format at a style index; the first one (or a default) when the index is out of range.
  const Xf& CellXf(uint32_t style) const;
  // Its font, fill and border; defaults when an index is out of range.
  const Font& FontOf(const Xf& xf) const;
  const Fill& FillOf(const Xf& xf) const;
  const Border& BorderOf(const Xf& xf) const;
  // The workbook's default font: the first font (Calibri 11 when there is none).
  const Font& DefaultFont() const;

  // The format code numFmts defines for an id; empty for the built-in ids it doesn't redefine
  // (0-163: the number-format engine knows those).
  std::string_view FormatCode(uint32_t numFmtId) const;
  const std::unordered_map<uint32_t, std::string>& CustomFormats() const { return numFmts_; }

  // A color as 0xAARRGGBB: theme colors through the workbook's theme with their tint, indexed
  // colors through the palette (the file's own, else Excel's default 64). `fallback` for None and
  // Auto (Auto means the system's window text or background, which the caller knows).
  uint32_t Resolve(const Color& c, uint32_t fallback) const;
  // The palette indexed colors use: the file's, else Excel's default.
  const std::vector<uint32_t>& Palette() const;
  // The theme's colors in SpreadsheetML's order (theme="0" is lt1, 1 dk1, 2 lt2, 3 dk2, 4-9 the
  // accents, 10 hlink, 11 folHlink). Set by the reader from the theme part, else Office's.
  void SetThemeColors(const std::array<uint32_t, 12>& rgb) { theme_ = rgb; }
  const std::array<uint32_t, 12>& ThemeColors() const { return theme_; }

 private:
  friend class StylesBuilder;
  std::vector<Xf> cellXfs_, cellStyleXfs_;
  std::vector<Font> fonts_;
  std::vector<Fill> fills_;
  std::vector<Border> borders_;
  std::vector<CellStyle> cellStyles_;
  std::vector<Dxf> dxfs_;
  std::unordered_map<uint32_t, std::string> numFmts_;
  std::vector<uint32_t> palette_;  // empty: the default
  std::array<uint32_t, 12> theme_{};
};

// Fills a Styles object; the xlsx reader uses it, and so can CSV.
class StylesBuilder {
 public:
  explicit StylesBuilder(Styles& s) : s_(s) {}
  std::vector<Xf>& CellXfs() { return s_.cellXfs_; }
  std::vector<Xf>& CellStyleXfs() { return s_.cellStyleXfs_; }
  std::vector<Font>& Fonts() { return s_.fonts_; }
  std::vector<Fill>& Fills() { return s_.fills_; }
  std::vector<Border>& Borders() { return s_.borders_; }
  std::vector<CellStyle>& CellStyles() { return s_.cellStyles_; }
  std::vector<Dxf>& Dxfs() { return s_.dxfs_; }
  std::unordered_map<uint32_t, std::string>& NumFmts() { return s_.numFmts_; }
  std::vector<uint32_t>& Palette() { return s_.palette_; }

 private:
  Styles& s_;
};

// Excel's tint (ECMA-376 §18.3.1.15): lighter towards white for tint > 0, darker for tint < 0,
// in HLS luminance. `argb` keeps its alpha.
uint32_t ApplyTint(uint32_t argb, double tint);

}  // namespace fw::sheet
