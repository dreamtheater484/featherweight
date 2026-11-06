#pragma once

#include <pugixml.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "draw/Painter.h"
#include "pkg/XmlDom.h"

// Office themes (ECMA-376 Part 1, §20.1.6): the colors, fonts and formats documents, sheets and
// slides refer to by name, and DrawingML's colors with their transforms (§20.1.2.3).
namespace draw {

// The twelve colors of a theme's color scheme, in the order the scheme lists them.
enum class ThemeColor : uint8_t {
  Dark1, Light1, Dark2, Light2, Accent1, Accent2, Accent3, Accent4, Accent5, Accent6, Hyperlink, FollowedHyperlink
};
inline constexpr int kThemeColorCount = 12;

// Maps the names text and shapes use for scheme colors (bg1, tx1, bg2, tx2, accent1…folHlink, in
// that order) to theme colors: a slide master's p:clrMap or Word's w:clrSchemeMapping.
struct ColorMap {
  std::array<ThemeColor, kThemeColorCount> map = {
      ThemeColor::Light1,  ThemeColor::Dark1,   ThemeColor::Light2,  ThemeColor::Dark2,
      ThemeColor::Accent1, ThemeColor::Accent2, ThemeColor::Accent3, ThemeColor::Accent4,
      ThemeColor::Accent5, ThemeColor::Accent6, ThemeColor::Hyperlink, ThemeColor::FollowedHyperlink};

  // p:clrMap (bg1="lt1" …) or w:clrSchemeMapping (w:bg1="light1" …); missing names keep the default.
  static ColorMap Read(pugi::xml_node node);
};

struct ThemeFont {
  std::string latin, eastAsian, complexScript;
  std::vector<std::pair<std::string, std::string>> scripts;  // script code ("Jpan") → typeface
};

class Theme {
 public:
  // A theme part (word/theme/theme1.xml, ppt/theme/themeN.xml, xl/theme/theme1.xml). Null when it
  // isn't one.
  static std::shared_ptr<const Theme> Parse(std::string_view xml);
  // For files without a theme: Office's (2013 and later) colors, fonts and formats.
  static const std::shared_ptr<const Theme>& Default();

  const std::string& Name() const { return name_; }
  Color GetColor(ThemeColor c) const { return colors_[static_cast<int>(c)]; }
  const ThemeFont& MajorFont() const { return major_; }
  const ThemeFont& MinorFont() const { return minor_; }

  // The format scheme's style matrix as a:fillRef, a:lnRef and a:effectRef count it: fills 1-3
  // (and background fills from 1001), lines 1-3, effects 1-3. The XML is kept, because its colors
  // say phClr and are resolved against each reference's color. Null when out of range.
  pugi::xml_node Fill(int idx) const;
  pugi::xml_node Line(int idx) const;
  pugi::xml_node Effect(int idx) const;

 private:
  Theme() = default;
  bool Read();

  fw::pkg::XmlDom dom_;
  std::string name_;
  std::array<Color, kThemeColorCount> colors_{};
  ThemeFont major_, minor_;
  std::vector<pugi::xml_node> fills_, backgroundFills_, lines_, effects_;
};

// Where colors are resolved: the theme, the color map, and phClr, the color a style reference
// gives the theme formats it picks.
struct ColorContext {
  const Theme* theme = nullptr;
  const ColorMap* colorMap = nullptr;
  std::optional<Color> placeholder;
};

// The color element among `parent`'s children (srgbClr, schemeClr, sysClr, prstClr, hslClr,
// scrgbClr), with its transforms applied in order. Nothing when there is none.
std::optional<Color> ReadColor(pugi::xml_node parent, const ColorContext& ctx);
// A color element itself.
std::optional<Color> ReadColorElement(pugi::xml_node color, const ColorContext& ctx);
// A scheme color name (bg1, tx1, accent1, dk1, phClr…) through the color map.
std::optional<Color> SchemeColor(std::string_view name, const ColorContext& ctx);
// One color transform (tint, shade, lumMod, alpha…) with its value as written (100000 = 100 %).
void ApplyColorTransform(Color& c, std::string_view transform, double value);
// DrawingML's preset colors ("dkBlue", "ltGray") and the CSS names VML uses, ignoring case.
std::optional<Color> NamedColor(std::string_view name);
// sRGB from 0xRRGGBB.
Color Rgb(uint32_t rgb, float alpha = 1);

}  // namespace draw
