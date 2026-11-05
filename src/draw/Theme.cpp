#include "draw/Theme.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>

#include "draw/Xml.h"

namespace draw {
namespace {

using namespace xml;

// Office's own theme (2013 and later), for files that have none.
constexpr std::string_view kDefaultTheme =
    R"(<a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" name="Office Theme"><a:themeElements>)"
    R"(<a:clrScheme name="Office"><a:dk1><a:sysClr val="windowText" lastClr="000000"/></a:dk1>)"
    R"(<a:lt1><a:sysClr val="window" lastClr="FFFFFF"/></a:lt1><a:dk2><a:srgbClr val="44546A"/></a:dk2>)"
    R"(<a:lt2><a:srgbClr val="E7E6E6"/></a:lt2><a:accent1><a:srgbClr val="4472C4"/></a:accent1>)"
    R"(<a:accent2><a:srgbClr val="ED7D31"/></a:accent2><a:accent3><a:srgbClr val="A5A5A5"/></a:accent3>)"
    R"(<a:accent4><a:srgbClr val="FFC000"/></a:accent4><a:accent5><a:srgbClr val="5B9BD5"/></a:accent5>)"
    R"(<a:accent6><a:srgbClr val="70AD47"/></a:accent6><a:hlink><a:srgbClr val="0563C1"/></a:hlink>)"
    R"(<a:folHlink><a:srgbClr val="954F72"/></a:folHlink></a:clrScheme>)"
    R"(<a:fontScheme name="Office"><a:majorFont><a:latin typeface="Calibri Light"/><a:ea typeface=""/><a:cs typeface=""/></a:majorFont>)"
    R"(<a:minorFont><a:latin typeface="Calibri"/><a:ea typeface=""/><a:cs typeface=""/></a:minorFont></a:fontScheme>)"
    R"(<a:fmtScheme name="Office"><a:fillStyleLst><a:solidFill><a:schemeClr val="phClr"/></a:solidFill>)"
    R"(<a:gradFill rotWithShape="1"><a:gsLst><a:gs pos="0"><a:schemeClr val="phClr"><a:lumMod val="110000"/><a:satMod val="105000"/><a:tint val="67000"/></a:schemeClr></a:gs>)"
    R"(<a:gs pos="50000"><a:schemeClr val="phClr"><a:lumMod val="105000"/><a:satMod val="103000"/><a:tint val="73000"/></a:schemeClr></a:gs>)"
    R"(<a:gs pos="100000"><a:schemeClr val="phClr"><a:lumMod val="105000"/><a:satMod val="109000"/><a:tint val="81000"/></a:schemeClr></a:gs></a:gsLst><a:lin ang="5400000" scaled="0"/></a:gradFill>)"
    R"(<a:gradFill rotWithShape="1"><a:gsLst><a:gs pos="0"><a:schemeClr val="phClr"><a:satMod val="103000"/><a:lumMod val="102000"/><a:tint val="94000"/></a:schemeClr></a:gs>)"
    R"(<a:gs pos="50000"><a:schemeClr val="phClr"><a:satMod val="110000"/><a:lumMod val="100000"/><a:shade val="100000"/></a:schemeClr></a:gs>)"
    R"(<a:gs pos="100000"><a:schemeClr val="phClr"><a:lumMod val="99000"/><a:satMod val="120000"/><a:shade val="78000"/></a:schemeClr></a:gs></a:gsLst><a:lin ang="5400000" scaled="0"/></a:gradFill></a:fillStyleLst>)"
    R"(<a:lnStyleLst><a:ln w="6350" cap="flat" cmpd="sng" algn="ctr"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill><a:prstDash val="solid"/><a:miter lim="800000"/></a:ln>)"
    R"(<a:ln w="12700" cap="flat" cmpd="sng" algn="ctr"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill><a:prstDash val="solid"/><a:miter lim="800000"/></a:ln>)"
    R"(<a:ln w="19050" cap="flat" cmpd="sng" algn="ctr"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill><a:prstDash val="solid"/><a:miter lim="800000"/></a:ln></a:lnStyleLst>)"
    R"(<a:effectStyleLst><a:effectStyle><a:effectLst/></a:effectStyle><a:effectStyle><a:effectLst/></a:effectStyle>)"
    R"(<a:effectStyle><a:effectLst><a:outerShdw blurRad="57150" dist="19050" dir="5400000" algn="ctr" rotWithShape="0"><a:srgbClr val="000000"><a:alpha val="63000"/></a:srgbClr></a:outerShdw></a:effectLst></a:effectStyle></a:effectStyleLst>)"
    R"(<a:bgFillStyleLst><a:solidFill><a:schemeClr val="phClr"/></a:solidFill><a:solidFill><a:schemeClr val="phClr"><a:tint val="95000"/><a:satMod val="170000"/></a:schemeClr></a:solidFill>)"
    R"(<a:gradFill rotWithShape="1"><a:gsLst><a:gs pos="0"><a:schemeClr val="phClr"><a:tint val="93000"/><a:satMod val="150000"/><a:shade val="98000"/><a:lumMod val="102000"/></a:schemeClr></a:gs>)"
    R"(<a:gs pos="50000"><a:schemeClr val="phClr"><a:tint val="98000"/><a:satMod val="130000"/><a:shade val="90000"/><a:lumMod val="103000"/></a:schemeClr></a:gs>)"
    R"(<a:gs pos="100000"><a:schemeClr val="phClr"><a:shade val="63000"/><a:satMod val="120000"/></a:schemeClr></a:gs></a:gsLst><a:lin ang="5400000" scaled="0"/></a:gradFill></a:bgFillStyleLst>)"
    R"(</a:fmtScheme></a:themeElements></a:theme>)";

constexpr std::string_view kSchemeNames[kThemeColorCount] = {"dk1",     "lt1",     "dk2",     "lt2",
                                                             "accent1", "accent2", "accent3", "accent4",
                                                             "accent5", "accent6", "hlink",   "folHlink"};
// The names a color map maps from, in ColorMap::map's order, and Word's names for both sides.
constexpr std::string_view kMapNames[kThemeColorCount] = {"bg1",     "tx1",     "bg2",     "tx2",
                                                          "accent1", "accent2", "accent3", "accent4",
                                                          "accent5", "accent6", "hlink",   "folHlink"};
constexpr std::string_view kWordMapNames[kThemeColorCount] = {"bg1",     "t1",      "bg2",     "t2",
                                                              "accent1", "accent2", "accent3", "accent4",
                                                              "accent5", "accent6", "hyperlink", "followedHyperlink"};
constexpr std::string_view kWordColorNames[kThemeColorCount] = {"dark1",   "light1",  "dark2",   "light2",
                                                                "accent1", "accent2", "accent3", "accent4",
                                                                "accent5", "accent6", "hyperlink", "followedHyperlink"};

// Office's gamma for the "linear" color transforms (tint, shade, the channel modulations).
constexpr double kGamma = 2.3;

double ToLinear(double c) { return std::pow(std::clamp(c, 0.0, 1.0), kGamma); }
double FromLinear(double c) { return std::pow(std::clamp(c, 0.0, 1.0), 1 / kGamma); }

struct Hsl {
  double h = 0, s = 0, l = 0;  // h in degrees, s and l 0..1
};

Hsl ToHsl(const Color& c) {
  const double r = c.r, g = c.g, b = c.b, mx = std::max({r, g, b}), mn = std::min({r, g, b});
  Hsl o;
  o.l = (mx + mn) / 2;
  const double d = mx - mn;
  if (d <= 1e-9) return o;
  o.s = o.l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
  if (mx == r) o.h = (g - b) / d + (g < b ? 6 : 0);
  else if (mx == g) o.h = (b - r) / d + 2;
  else o.h = (r - g) / d + 4;
  o.h *= 60;
  return o;
}

void FromHsl(const Hsl& in, Color& c) {
  const double s = std::clamp(in.s, 0.0, 1.0), l = std::clamp(in.l, 0.0, 1.0);
  double h = std::fmod(in.h, 360.0);
  if (h < 0) h += 360;
  if (s <= 0) {
    c.r = c.g = c.b = static_cast<float>(l);
    return;
  }
  const double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
  auto hue = [&](double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
  };
  c.r = static_cast<float>(hue(h / 360 + 1.0 / 3));
  c.g = static_cast<float>(hue(h / 360));
  c.b = static_cast<float>(hue(h / 360 - 1.0 / 3));
}

uint32_t Hex(std::string_view v) {
  uint32_t rgb = 0;
  std::from_chars(v.data(), v.data() + v.size(), rgb, 16);
  return rgb;
}

std::optional<Color> SystemColor(pugi::xml_node n) {
  if (pugi::xml_attribute last = n.attribute("lastClr")) return Rgb(Hex(last.value()));
  struct Sys {
    std::string_view name;
    uint32_t rgb;
  };
  // Windows' default colors, so files look the same on every PC.
  static constexpr Sys kSys[] = {{"windowText", 0x000000}, {"window", 0xFFFFFF},      {"btnFace", 0xF0F0F0},
                                 {"btnText", 0x000000},    {"highlight", 0x0078D7},   {"highlightText", 0xFFFFFF},
                                 {"grayText", 0x6D6D6D},   {"windowFrame", 0x646464}, {"3dDkShadow", 0x696969},
                                 {"3dLight", 0xE3E3E3},    {"btnShadow", 0xA0A0A0},   {"btnHighlight", 0xFFFFFF},
                                 {"menu", 0xF0F0F0},       {"menuText", 0x000000},    {"infoBk", 0xFFFFE1},
                                 {"infoText", 0x000000},   {"captionText", 0x000000}, {"activeBorder", 0xB4B4B4}};
  std::string_view val = n.attribute("val").value();
  for (const Sys& s : kSys)
    if (s.name == val) return Rgb(s.rgb);
  return Rgb(0);
}

bool ReadFont(pugi::xml_node font, ThemeFont& out) {
  if (!font) return false;
  out.latin = A(font, "latin").attribute("typeface").value();
  out.eastAsian = A(font, "ea").attribute("typeface").value();
  out.complexScript = A(font, "cs").attribute("typeface").value();
  ForEachChild(font, [&](pugi::xml_node f) {
    if (Is(f, kA, "font")) out.scripts.emplace_back(f.attribute("script").value(), f.attribute("typeface").value());
    return out.scripts.size() < 256;
  });
  return true;
}

}  // namespace

Color Rgb(uint32_t rgb, float alpha) {
  return {((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, alpha};
}

ColorMap ColorMap::Read(pugi::xml_node node) {
  ColorMap m;
  if (!node) return m;
  for (pugi::xml_attribute a : node.attributes()) {
    std::string_view name = LocalNameOf(a), value = a.value();
    for (int i = 0; i < kThemeColorCount; ++i) {
      if (name != kMapNames[i] && name != kWordMapNames[i]) continue;
      for (int j = 0; j < kThemeColorCount; ++j)
        if (value == kSchemeNames[j] || value == kWordColorNames[j]) m.map[i] = static_cast<ThemeColor>(j);
    }
  }
  return m;
}

std::shared_ptr<const Theme> Theme::Parse(std::string_view xml) {
  std::shared_ptr<Theme> t(new Theme());
  if (!t->dom_.Parse(xml).Ok() || !t->Read()) return nullptr;
  return t;
}

const std::shared_ptr<const Theme>& Theme::Default() {
  static const std::shared_ptr<const Theme> theme = Parse(kDefaultTheme);
  return theme;
}

bool Theme::Read() {
  pugi::xml_node root = dom_.Root();
  if (!Is(root, kA, "theme")) return false;
  name_ = root.attribute("name").value();
  pugi::xml_node elements = A(root, "themeElements");
  const ColorContext none;
  pugi::xml_node scheme = A(elements, "clrScheme");
  for (int i = 0; i < kThemeColorCount; ++i) {
    colors_[i] = ReadColor(A(scheme, kSchemeNames[i]), none).value_or(Color{0, 0, 0, 1});
  }
  pugi::xml_node fonts = A(elements, "fontScheme");
  ReadFont(A(fonts, "majorFont"), major_);
  ReadFont(A(fonts, "minorFont"), minor_);
  pugi::xml_node formats = A(elements, "fmtScheme");
  auto collect = [&](std::string_view list, std::vector<pugi::xml_node>& out) {
    ForEachChild(A(formats, list), [&](pugi::xml_node c) {
      out.push_back(c);
      return out.size() < 64;
    });
  };
  collect("fillStyleLst", fills_);
  collect("bgFillStyleLst", backgroundFills_);
  collect("lnStyleLst", lines_);
  collect("effectStyleLst", effects_);
  return true;
}

pugi::xml_node Theme::Fill(int idx) const {
  if (idx >= 1001 && idx - 1001 < static_cast<int>(backgroundFills_.size())) return backgroundFills_[idx - 1001];
  if (idx >= 1 && idx - 1 < static_cast<int>(fills_.size())) return fills_[idx - 1];
  return {};
}

pugi::xml_node Theme::Line(int idx) const {
  return idx >= 1 && idx - 1 < static_cast<int>(lines_.size()) ? lines_[idx - 1] : pugi::xml_node();
}

pugi::xml_node Theme::Effect(int idx) const {
  return idx >= 1 && idx - 1 < static_cast<int>(effects_.size()) ? effects_[idx - 1] : pugi::xml_node();
}

std::optional<Color> SchemeColor(std::string_view name, const ColorContext& ctx) {
  if (name == "phClr") return ctx.placeholder;
  const Theme* theme = ctx.theme ? ctx.theme : Theme::Default().get();
  if (!theme) return std::nullopt;
  for (int i = 0; i < kThemeColorCount; ++i)
    if (name == kSchemeNames[i]) return theme->GetColor(static_cast<ThemeColor>(i));
  static const ColorMap kIdentity;
  const ColorMap& map = ctx.colorMap ? *ctx.colorMap : kIdentity;
  for (int i = 0; i < kThemeColorCount; ++i)
    if (name == kMapNames[i]) return theme->GetColor(map.map[i]);
  return std::nullopt;
}

void ApplyColorTransform(Color& c, std::string_view t, double value) {
  const double v = value / 100000.0;
  auto linear = [&](auto&& f) {
    double ch[3] = {ToLinear(c.r), ToLinear(c.g), ToLinear(c.b)};
    for (double& x : ch) x = f(x);
    c.r = static_cast<float>(FromLinear(ch[0]));
    c.g = static_cast<float>(FromLinear(ch[1]));
    c.b = static_cast<float>(FromLinear(ch[2]));
  };
  auto hsl = [&](auto&& f) {
    Hsl h = ToHsl(c);
    f(h);
    FromHsl(h, c);
  };
  auto channel = [&](float& ch, int how) {  // 0 set, 1 offset, 2 modulate, in linear light
    double x = ToLinear(ch);
    x = how == 0 ? v : how == 1 ? x + v : x * v;
    ch = static_cast<float>(FromLinear(x));
  };
  if (t == "tint") {
    if (v >= 0 && v < 1) linear([&](double x) { return 1 - (1 - x) * v; });
  } else if (t == "shade") {
    if (v >= 0 && v < 1) linear([&](double x) { return x * v; });
  } else if (t == "lumMod") {
    hsl([&](Hsl& h) { h.l *= v; });
  } else if (t == "lumOff") {
    hsl([&](Hsl& h) { h.l += v; });
  } else if (t == "lum") {
    hsl([&](Hsl& h) { h.l = v; });
  } else if (t == "satMod") {
    hsl([&](Hsl& h) { h.s *= v; });
  } else if (t == "satOff") {
    hsl([&](Hsl& h) { h.s += v; });
  } else if (t == "sat") {
    hsl([&](Hsl& h) { h.s = v; });
  } else if (t == "hueMod") {
    hsl([&](Hsl& h) { h.h *= v; });
  } else if (t == "hueOff") {
    hsl([&](Hsl& h) { h.h += value / 60000.0; });
  } else if (t == "hue") {
    hsl([&](Hsl& h) { h.h = value / 60000.0; });
  } else if (t == "comp") {
    hsl([&](Hsl& h) { h.h += 180; });
  } else if (t == "inv") {
    c = {1 - c.r, 1 - c.g, 1 - c.b, c.a};
  } else if (t == "gray") {
    const float y = 0.22f * c.r + 0.72f * c.g + 0.06f * c.b;
    c = {y, y, y, c.a};
  } else if (t == "alpha") {
    c.a = static_cast<float>(v);
  } else if (t == "alphaMod") {
    c.a = static_cast<float>(c.a * v);
  } else if (t == "alphaOff") {
    c.a = static_cast<float>(c.a + v);
  } else if (t == "red") {
    channel(c.r, 0);
  } else if (t == "redOff") {
    channel(c.r, 1);
  } else if (t == "redMod") {
    channel(c.r, 2);
  } else if (t == "green") {
    channel(c.g, 0);
  } else if (t == "greenOff") {
    channel(c.g, 1);
  } else if (t == "greenMod") {
    channel(c.g, 2);
  } else if (t == "blue") {
    channel(c.b, 0);
  } else if (t == "blueOff") {
    channel(c.b, 1);
  } else if (t == "blueMod") {
    channel(c.b, 2);
  } else if (t == "gamma") {
    linear([](double x) { return std::pow(x, 1 / kGamma); });
  } else if (t == "invGamma") {
    linear([](double x) { return std::pow(x, kGamma); });
  }
  c.r = std::clamp(c.r, 0.f, 1.f);
  c.g = std::clamp(c.g, 0.f, 1.f);
  c.b = std::clamp(c.b, 0.f, 1.f);
  c.a = std::clamp(c.a, 0.f, 1.f);
}

std::optional<Color> ReadColorElement(pugi::xml_node n, const ColorContext& ctx) {
  if (NamespaceOf(n) != kA) return std::nullopt;
  std::string_view kind = LocalNameOf(n);
  std::optional<Color> c;
  if (kind == "srgbClr") {
    c = Rgb(Hex(n.attribute("val").value()));
  } else if (kind == "schemeClr") {
    c = SchemeColor(n.attribute("val").value(), ctx);
  } else if (kind == "sysClr") {
    c = SystemColor(n);
  } else if (kind == "prstClr") {
    c = NamedColor(n.attribute("val").value());
  } else if (kind == "hslClr") {
    Color hc{0, 0, 0, 1};
    FromHsl({Number(n, "hue", 0) / 60000, Number(n, "sat", 0) / 100000, Number(n, "lum", 0) / 100000}, hc);
    c = hc;
  } else if (kind == "scrgbClr") {
    c = Color{static_cast<float>(FromLinear(Number(n, "r", 0) / 100000)),
              static_cast<float>(FromLinear(Number(n, "g", 0) / 100000)),
              static_cast<float>(FromLinear(Number(n, "b", 0) / 100000)), 1};
  } else {
    return std::nullopt;
  }
  if (!c) return std::nullopt;
  int count = 0;
  for (pugi::xml_node t = n.first_child(); t && count < 64; t = t.next_sibling(), ++count) {
    if (t.type() == pugi::node_element && NamespaceOf(t) == kA) {
      ApplyColorTransform(*c, LocalNameOf(t), Number(t, "val", 0));
    }
  }
  return c;
}

std::optional<Color> ReadColor(pugi::xml_node parent, const ColorContext& ctx) {
  std::optional<Color> found;
  ForEachChild(parent, [&](pugi::xml_node c) {
    found = ReadColorElement(c, ctx);
    return !found;
  });
  return found;
}

std::optional<Color> NamedColor(std::string_view name) {
  struct Named {
    std::string_view name;
    uint32_t rgb;
  };
  // CSS's named colors, sorted. DrawingML's dk/lt/med prefixes are spelled out below.
  static constexpr Named kNamed[] = {
      {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF}, {"aquamarine", 0x7FFFD4},
      {"azure", 0xF0FFFF}, {"beige", 0xF5F5DC}, {"bisque", 0xFFE4C4}, {"black", 0x000000},
      {"blanchedalmond", 0xFFEBCD}, {"blue", 0x0000FF}, {"blueviolet", 0x8A2BE2}, {"brown", 0xA52A2A},
      {"burlywood", 0xDEB887}, {"cadetblue", 0x5F9EA0}, {"chartreuse", 0x7FFF00}, {"chocolate", 0xD2691E},
      {"coral", 0xFF7F50}, {"cornflowerblue", 0x6495ED}, {"cornsilk", 0xFFF8DC}, {"crimson", 0xDC143C},
      {"cyan", 0x00FFFF}, {"darkblue", 0x00008B}, {"darkcyan", 0x008B8B}, {"darkgoldenrod", 0xB8860B},
      {"darkgray", 0xA9A9A9}, {"darkgreen", 0x006400}, {"darkgrey", 0xA9A9A9}, {"darkkhaki", 0xBDB76B},
      {"darkmagenta", 0x8B008B}, {"darkolivegreen", 0x556B2F}, {"darkorange", 0xFF8C00}, {"darkorchid", 0x9932CC},
      {"darkred", 0x8B0000}, {"darksalmon", 0xE9967A}, {"darkseagreen", 0x8FBC8F}, {"darkslateblue", 0x483D8B},
      {"darkslategray", 0x2F4F4F}, {"darkslategrey", 0x2F4F4F}, {"darkturquoise", 0x00CED1},
      {"darkviolet", 0x9400D3}, {"deeppink", 0xFF1493}, {"deepskyblue", 0x00BFFF}, {"dimgray", 0x696969},
      {"dimgrey", 0x696969}, {"dodgerblue", 0x1E90FF}, {"firebrick", 0xB22222}, {"floralwhite", 0xFFFAF0},
      {"forestgreen", 0x228B22}, {"fuchsia", 0xFF00FF}, {"gainsboro", 0xDCDCDC}, {"ghostwhite", 0xF8F8FF},
      {"gold", 0xFFD700}, {"goldenrod", 0xDAA520}, {"gray", 0x808080}, {"green", 0x008000},
      {"greenyellow", 0xADFF2F}, {"grey", 0x808080}, {"honeydew", 0xF0FFF0}, {"hotpink", 0xFF69B4},
      {"indianred", 0xCD5C5C}, {"indigo", 0x4B0082}, {"ivory", 0xFFFFF0}, {"khaki", 0xF0E68C},
      {"lavender", 0xE6E6FA}, {"lavenderblush", 0xFFF0F5}, {"lawngreen", 0x7CFC00}, {"lemonchiffon", 0xFFFACD},
      {"lightblue", 0xADD8E6}, {"lightcoral", 0xF08080}, {"lightcyan", 0xE0FFFF},
      {"lightgoldenrodyellow", 0xFAFAD2}, {"lightgray", 0xD3D3D3}, {"lightgreen", 0x90EE90},
      {"lightgrey", 0xD3D3D3}, {"lightpink", 0xFFB6C1}, {"lightsalmon", 0xFFA07A}, {"lightseagreen", 0x20B2AA},
      {"lightskyblue", 0x87CEFA}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
      {"lightsteelblue", 0xB0C4DE}, {"lightyellow", 0xFFFFE0}, {"lime", 0x00FF00}, {"limegreen", 0x32CD32},
      {"linen", 0xFAF0E6}, {"magenta", 0xFF00FF}, {"maroon", 0x800000}, {"mediumaquamarine", 0x66CDAA},
      {"mediumblue", 0x0000CD}, {"mediumorchid", 0xBA55D3}, {"mediumpurple", 0x9370DB},
      {"mediumseagreen", 0x3CB371}, {"mediumslateblue", 0x7B68EE}, {"mediumspringgreen", 0x00FA9A},
      {"mediumturquoise", 0x48D1CC}, {"mediumvioletred", 0xC71585}, {"midnightblue", 0x191970},
      {"mintcream", 0xF5FFFA}, {"mistyrose", 0xFFE4E1}, {"moccasin", 0xFFE4B5}, {"navajowhite", 0xFFDEAD},
      {"navy", 0x000080}, {"oldlace", 0xFDF5E6}, {"olive", 0x808000}, {"olivedrab", 0x6B8E23},
      {"orange", 0xFFA500}, {"orangered", 0xFF4500}, {"orchid", 0xDA70D6}, {"palegoldenrod", 0xEEE8AA},
      {"palegreen", 0x98FB98}, {"paleturquoise", 0xAFEEEE}, {"palevioletred", 0xDB7093}, {"papayawhip", 0xFFEFD5},
      {"peachpuff", 0xFFDAB9}, {"peru", 0xCD853F}, {"pink", 0xFFC0CB}, {"plum", 0xDDA0DD},
      {"powderblue", 0xB0E0E6}, {"purple", 0x800080}, {"red", 0xFF0000}, {"rosybrown", 0xBC8F8F},
      {"royalblue", 0x4169E1}, {"saddlebrown", 0x8B4513}, {"salmon", 0xFA8072}, {"sandybrown", 0xF4A460},
      {"seagreen", 0x2E8B57}, {"seashell", 0xFFF5EE}, {"sienna", 0xA0522D}, {"silver", 0xC0C0C0},
      {"skyblue", 0x87CEEB}, {"slateblue", 0x6A5ACD}, {"slategray", 0x708090}, {"slategrey", 0x708090},
      {"snow", 0xFFFAFA}, {"springgreen", 0x00FF7F}, {"steelblue", 0x4682B4}, {"tan", 0xD2B48C},
      {"teal", 0x008080}, {"thistle", 0xD8BFD8}, {"tomato", 0xFF6347}, {"turquoise", 0x40E0D0},
      {"violet", 0xEE82EE}, {"wheat", 0xF5DEB3}, {"white", 0xFFFFFF}, {"whitesmoke", 0xF5F5F5},
      {"yellow", 0xFFFF00}, {"yellowgreen", 0x9ACD32}};
  std::string key;
  for (char ch : name) key += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  if (key.starts_with("dk")) key = "dark" + key.substr(2);
  else if (key.starts_with("lt")) key = "light" + key.substr(2);
  else if (key.starts_with("med") && !key.starts_with("medium")) key = "medium" + key.substr(3);
  auto it = std::lower_bound(std::begin(kNamed), std::end(kNamed), key,
                             [](const Named& n, const std::string& k) { return n.name < k; });
  if (it != std::end(kNamed) && it->name == key) return Rgb(it->rgb);
  return std::nullopt;
}

}  // namespace draw
