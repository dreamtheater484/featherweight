#include "sheet/model/Styles.h"

#include <algorithm>
#include <cmath>

namespace fw::sheet {
namespace {

// Excel's default indexed colors (ECMA-376 §18.8.27), 0-63.
const std::vector<uint32_t>& DefaultPalette() {
  static const std::vector<uint32_t> palette = [] {
    const uint32_t rgb[64] = {
        0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF, 0x000000, 0xFFFFFF, 0xFF0000,
        0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF, 0x800000, 0x008000, 0x000080, 0x808000, 0x800080, 0x008080,
        0xC0C0C0, 0x808080, 0x9999FF, 0x993366, 0xFFFFCC, 0xCCFFFF, 0x660066, 0xFF8080, 0x0066CC, 0xCCCCFF, 0x000080,
        0xFF00FF, 0xFFFF00, 0x00FFFF, 0x800080, 0x800000, 0x008080, 0x0000FF, 0x00CCFF, 0xCCFFFF, 0xCCFFCC, 0xFFFF99,
        0x99CCFF, 0xFF99CC, 0xCC99FF, 0xFFCC99, 0x3366FF, 0x33CCCC, 0x99CC00, 0xFFCC00, 0xFF9900, 0xFF6600, 0x666699,
        0x969696, 0x003366, 0x339966, 0x003300, 0x333300, 0x993300, 0x993366, 0x333399, 0x333333};
    std::vector<uint32_t> v;
    for (uint32_t c : rgb) v.push_back(0xFF000000u | c);
    return v;
  }();
  return palette;
}

const Xf& DefaultXf() {
  static const Xf xf;
  return xf;
}

}  // namespace

Styles::Styles() {
  // Office's theme (2013 and later), in SpreadsheetML's order: lt1, dk1, lt2, dk2, accents, links.
  theme_ = {0xFFFFFFFF, 0xFF000000, 0xFFE7E6E6, 0xFF44546A, 0xFF4472C4, 0xFFED7D31,
            0xFFA5A5A5, 0xFFFFC000, 0xFF5B9BD5, 0xFF70AD47, 0xFF0563C1, 0xFF954F72};
}

const Xf& Styles::CellXf(uint32_t style) const {
  if (style < cellXfs_.size()) return cellXfs_[style];
  return cellXfs_.empty() ? DefaultXf() : cellXfs_[0];
}

const Font& Styles::DefaultFont() const {
  static const Font calibri = [] {
    Font f;
    f.name = "Calibri";
    f.size = 11;
    f.family = 2;
    f.scheme = FontScheme::Minor;
    f.color.kind = Color::Kind::Theme;
    f.color.value = 1;
    return f;
  }();
  return fonts_.empty() ? calibri : fonts_[0];
}

const Font& Styles::FontOf(const Xf& xf) const { return xf.font < fonts_.size() ? fonts_[xf.font] : DefaultFont(); }

const Fill& Styles::FillOf(const Xf& xf) const {
  static const Fill none;
  return xf.fill < fills_.size() ? fills_[xf.fill] : none;
}

const Border& Styles::BorderOf(const Xf& xf) const {
  static const Border none;
  return xf.border < borders_.size() ? borders_[xf.border] : none;
}

std::string_view Styles::FormatCode(uint32_t numFmtId) const {
  auto it = numFmts_.find(numFmtId);
  return it == numFmts_.end() ? std::string_view() : std::string_view(it->second);
}

const std::vector<uint32_t>& Styles::Palette() const { return palette_.empty() ? DefaultPalette() : palette_; }

uint32_t Styles::Resolve(const Color& c, uint32_t fallback) const {
  uint32_t argb = fallback;
  switch (c.kind) {
    case Color::Kind::None:
    case Color::Kind::Auto:
      return fallback;
    case Color::Kind::Rgb:
      argb = c.value | 0xFF000000u;  // Excel ignores the alpha of cell colors
      break;
    case Color::Kind::Indexed: {
      const auto& p = Palette();
      if (c.value >= p.size()) return fallback;  // 64 and 65: the system's text and background
      argb = p[c.value] | 0xFF000000u;
      break;
    }
    case Color::Kind::Theme:
      if (c.value >= theme_.size()) return fallback;
      argb = theme_[c.value];
      break;
  }
  return c.tint != 0 ? ApplyTint(argb, c.tint) : argb;
}

uint32_t ApplyTint(uint32_t argb, double tint) {
  if (!(tint >= -1 && tint <= 1)) return argb;
  double r = ((argb >> 16) & 0xFF) / 255.0, g = ((argb >> 8) & 0xFF) / 255.0, b = (argb & 0xFF) / 255.0;
  // RGB to HLS.
  double mx = std::max({r, g, b}), mn = std::min({r, g, b});
  double l = (mx + mn) / 2, h = 0, s = 0;
  if (mx != mn) {
    double d = mx - mn;
    s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx == r) h = (g - b) / d + (g < b ? 6 : 0);
    else if (mx == g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h /= 6;
  }
  l = tint < 0 ? l * (1 + tint) : l * (1 - tint) + tint;
  // HLS to RGB.
  auto hue = [](double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
  };
  if (s == 0) {
    r = g = b = l;
  } else {
    double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    r = hue(p, q, h + 1.0 / 3);
    g = hue(p, q, h);
    b = hue(p, q, h - 1.0 / 3);
  }
  auto byte = [](double v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
  return (argb & 0xFF000000u) | (byte(r) << 16) | (byte(g) << 8) | byte(b);
}

}  // namespace fw::sheet
