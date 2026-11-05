// Reads the VML subset Office still writes (plan §4.5): shapes, text boxes, pictures, WordArt
// watermarks and groups, into the same shape model as DrawingML. Shape types come from the
// document's own v:shapetype (its path and formulas are interpreted), else from the preset with
// the same o:spt number.
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <string>

#include "draw/Shapes.h"
#include "draw/Xml.h"

namespace draw {

std::vector<std::pair<std::string_view, std::string_view>> ParseVmlStyle(std::string_view style) {
  std::vector<std::pair<std::string_view, std::string_view>> out;
  auto trim = [](std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) {
      s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) s.remove_suffix(1);
    return s;
  };
  while (!style.empty() && out.size() < 256) {
    size_t end = style.find(';');
    std::string_view decl = style.substr(0, end);
    style = end == std::string_view::npos ? std::string_view() : style.substr(end + 1);
    size_t colon = decl.find(':');
    if (colon == std::string_view::npos) continue;
    out.emplace_back(trim(decl.substr(0, colon)), trim(decl.substr(colon + 1)));
  }
  return out;
}

float VmlLength(std::string_view v, float unitless) {
  while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
  double n = 0;
  auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
  if (ec != std::errc() || !std::isfinite(n)) return 0;
  std::string_view unit(end, v.data() + v.size() - end);
  while (!unit.empty() && unit.back() == ' ') unit.remove_suffix(1);
  double k = unitless;
  if (unit == "pt") k = 1;
  else if (unit == "px") k = 0.75;
  else if (unit == "in") k = 72;
  else if (unit == "cm") k = 72 / 2.54;
  else if (unit == "mm") k = 72 / 25.4;
  else if (unit == "pc") k = 12;
  else if (unit == "em") k = 12;
  else if (unit == "emu") k = 1 / 12700.0;
  return static_cast<float>(std::clamp(n * k, -1e7, 1e7));
}

namespace {

using namespace xml;

// VML's fractions: "0.5", "50%", "32768f" (1/65536).
double Fraction(std::string_view v, double def) {
  while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
  if (v.empty()) return def;
  double n = Number(v.substr(0, v.find_first_not_of("+-0123456789.")), def);
  if (v.ends_with("f")) return n / 65536;
  if (v.ends_with("%")) return n / 100;
  return n;
}

std::string_view StyleValue(const std::vector<std::pair<std::string_view, std::string_view>>& style,
                            std::string_view name) {
  for (auto it = style.rbegin(); it != style.rend(); ++it)
    if (it->first == name) return it->second;
  return {};
}

std::optional<Color> ParseColor(std::string_view v, std::optional<Color> fill = std::nullopt) {
  while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
  // "black [3213]": the bracket is a palette index Office adds; the color comes first.
  v = v.substr(0, v.find(" ["));
  while (!v.empty() && v.back() == ' ') v.remove_suffix(1);
  if (v.starts_with("fill") && fill) {
    // "fill darken(118)", "fill lighten(200)": the fill color, darker or lighter by n/255.
    Color c = *fill;
    size_t open = v.find('('), close = v.find(')');
    const float n = open != std::string_view::npos && close > open
                        ? static_cast<float>(Number(v.substr(open + 1, close - open - 1), 255)) / 255
                        : 1;
    if (v.find("darken") != std::string_view::npos) c = {c.r * n, c.g * n, c.b * n, c.a};
    else if (v.find("lighten") != std::string_view::npos) c = {1 - (1 - c.r) * n, 1 - (1 - c.g) * n, 1 - (1 - c.b) * n, c.a};
    return c;
  }
  if (v.starts_with("#")) {
    std::string_view hex = v.substr(1);
    uint32_t rgb = 0;
    auto [end, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), rgb, 16);
    if (ec != std::errc()) return std::nullopt;
    if (end - hex.data() == 3) {
      rgb = ((rgb & 0xF00) << 12) | ((rgb & 0xF00) << 8) | ((rgb & 0x0F0) << 8) | ((rgb & 0x0F0) << 4) |
            ((rgb & 0x00F) << 4) | (rgb & 0x00F);
    }
    return Rgb(rgb);
  }
  if (v == "windowText" || v == "buttonText" || v == "infoText" || v == "menuText") return Rgb(0);
  if (v == "window" || v == "buttonFace" || v == "menu") return Rgb(0xFFFFFF);
  return NamedColor(v);
}

// The arrowheads VML names.
ArrowHead ParseArrow(pugi::xml_node stroke, const char* type, const char* width, const char* length) {
  ArrowHead a;
  std::string_view t = stroke.attribute(type).value();
  if (t == "block") a.type = ArrowHead::Type::Triangle;
  else if (t == "classic") a.type = ArrowHead::Type::Stealth;
  else if (t == "oval") a.type = ArrowHead::Type::Oval;
  else if (t == "diamond") a.type = ArrowHead::Type::Diamond;
  else if (t == "open") a.type = ArrowHead::Type::Arrow;
  std::string_view w = stroke.attribute(width).value(), l = stroke.attribute(length).value();
  a.width = w == "narrow" ? 0 : w == "wide" ? 2 : 1;
  a.length = l == "short" ? 0 : l == "long" ? 2 : 1;
  return a;
}

std::vector<float> VmlDash(std::string_view v) {
  if (v.empty() || v == "solid") return {};
  if (v == "shortdash") return {3, 1};
  if (v == "shortdot") return {1, 1};
  if (v == "shortdashdot") return {3, 1, 1, 1};
  if (v == "shortdashdotdot") return {3, 1, 1, 1, 1, 1};
  if (v == "dot") return {1, 3};
  if (v == "dash") return {4, 3};
  if (v == "longdash") return {8, 3};
  if (v == "dashdot") return {4, 3, 1, 3};
  if (v == "longdashdot") return {8, 3, 1, 3};
  if (v == "longdashdotdot") return {8, 3, 1, 3, 1, 3};
  std::vector<float> custom;  // "4 3 1 3": multiples of the width
  size_t i = 0;
  while (i < v.size() && custom.size() < 32) {
    while (i < v.size() && (v[i] == ' ' || v[i] == ',')) ++i;
    size_t j = i;
    while (j < v.size() && v[j] != ' ' && v[j] != ',') ++j;
    if (j > i) custom.push_back(std::clamp(static_cast<float>(Number(v.substr(i, j - i), 1)), 0.01f, 1000.f));
    i = j;
  }
  if (custom.size() % 2) custom.push_back(custom.back());
  return custom;
}

// Office's shape type numbers (o:spt) with a DrawingML preset of the same shape.
std::string_view PresetForSpt(int spt) {
  struct Map {
    int spt;
    std::string_view preset;
  };
  static constexpr Map kMap[] = {
      {1, "rect"}, {2, "roundRect"}, {3, "ellipse"}, {4, "diamond"}, {5, "triangle"}, {6, "rtTriangle"},
      {7, "parallelogram"}, {8, "trapezoid"}, {9, "hexagon"}, {10, "octagon"}, {11, "plus"}, {12, "star5"},
      {13, "rightArrow"}, {15, "homePlate"}, {16, "cube"}, {20, "line"}, {21, "plaque"}, {22, "can"},
      {23, "donut"}, {32, "straightConnector1"}, {33, "bentConnector2"}, {34, "bentConnector3"},
      {35, "bentConnector4"}, {36, "bentConnector5"}, {37, "curvedConnector2"}, {38, "curvedConnector3"},
      {39, "curvedConnector4"}, {40, "curvedConnector5"}, {53, "ribbon"}, {54, "ribbon2"}, {55, "chevron"},
      {56, "pentagon"}, {57, "noSmoking"}, {58, "star8"}, {59, "star16"}, {60, "star32"}, {61, "wedgeRectCallout"},
      {62, "wedgeRoundRectCallout"}, {63, "wedgeEllipseCallout"}, {64, "wave"}, {65, "foldedCorner"},
      {66, "leftArrow"}, {67, "downArrow"}, {68, "upArrow"}, {69, "leftRightArrow"}, {70, "upDownArrow"},
      {71, "irregularSeal1"}, {72, "irregularSeal2"}, {73, "lightningBolt"}, {74, "heart"}, {75, "rect"},
      {76, "quadArrow"}, {84, "bevel"}, {85, "leftBracket"}, {86, "rightBracket"}, {87, "leftBrace"},
      {88, "rightBrace"}, {89, "leftUpArrow"}, {90, "bentUpArrow"}, {91, "bentArrow"}, {92, "star24"},
      {93, "stripedRightArrow"}, {94, "notchedRightArrow"}, {95, "blockArc"}, {96, "smileyFace"},
      {97, "verticalScroll"}, {98, "horizontalScroll"}, {99, "circularArrow"}, {109, "flowChartProcess"},
      {110, "flowChartDecision"}, {111, "flowChartInputOutput"}, {112, "flowChartPredefinedProcess"},
      {113, "flowChartInternalStorage"}, {114, "flowChartDocument"}, {115, "flowChartMultidocument"},
      {116, "flowChartTerminator"}, {117, "flowChartPreparation"}, {118, "flowChartManualInput"},
      {119, "flowChartManualOperation"}, {120, "flowChartConnector"}, {121, "flowChartPunchedCard"},
      {122, "flowChartPunchedTape"}, {123, "flowChartSummingJunction"}, {124, "flowChartOr"},
      {125, "flowChartCollate"}, {126, "flowChartSort"}, {127, "flowChartExtract"}, {128, "flowChartMerge"},
      {129, "flowChartOfflineStorage"}, {130, "flowChartOnlineStorage"}, {131, "flowChartMagneticTape"},
      {132, "flowChartMagneticDisk"}, {133, "flowChartMagneticDrum"}, {134, "flowChartDisplay"},
      {135, "flowChartDelay"}, {183, "sun"}, {184, "moon"}, {185, "bracketPair"}, {186, "bracePair"},
      {187, "star4"}, {188, "doubleWave"}, {202, "rect"}};
  for (const Map& m : kMap)
    if (m.spt == spt) return m.preset;
  return {};
}

std::string Num(double d) {
  char buf[32];
  auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), std::isfinite(d) ? d : 0.0);
  return ec == std::errc() ? std::string(buf, end) : std::string("0");
}

// VML formulas (v:formulas) and paths, evaluated once the shape's size is known.
class VmlGeometry {
 public:
  VmlGeometry(double cw, double ch, double ox, double oy, Size size, std::vector<double> adj, bool stroked, bool filled)
      : cw_(cw), ch_(ch), ox_(ox), oy_(oy), size_(size), adj_(std::move(adj)), stroked_(stroked), filled_(filled) {}

  void Formulas(pugi::xml_node formulas) {
    ForEachChild(formulas, [&](pugi::xml_node f) {
      values_.push_back(Eval(f.attribute("eqn").value()));
      return values_.size() < 512;
    });
  }

  // The path as DrawingML geometry in coordsize units.
  std::shared_ptr<Geometry> Path(std::string_view path, std::string_view textboxrect) {
    auto g = std::make_shared<Geometry>();
    Tokenize(path);
    bool fill = filled_, stroke = stroked_, begun = false;
    double cx = 0, cy = 0;
    auto begin = [&] {
      if (!begun) g->BeginPath(cw_, ch_, fill ? Geometry::PathFill::Normal : Geometry::PathFill::None, stroke);
      begun = true;
    };
    auto pt = [&](double x, double y) { return std::make_pair(Num(x - ox_), Num(y - oy_)); };
    size_t i = 0;
    int guard = 0;
    while (i < tokens_.size() && ++guard < 100000) {
      if (!tokens_[i].command) {
        ++i;  // a stray number
        continue;
      }
      const std::string cmd = tokens_[i].text;
      std::vector<double> a;
      for (++i; i < tokens_.size() && !tokens_[i].command; ++i) a.push_back(tokens_[i].value);
      auto arg = [&](size_t k) { return k < a.size() ? a[k] : 0.0; };
      if (cmd == "m" || cmd == "t") {
        begin();
        cx = cmd == "t" ? cx + arg(0) : arg(0);
        cy = cmd == "t" ? cy + arg(1) : arg(1);
        auto [x, y] = pt(cx, cy);
        g->MoveTo(x, y);
      } else if (cmd == "l" || cmd == "r") {
        begin();
        for (size_t k = 0; k + 1 < std::max<size_t>(a.size(), 2); k += 2) {
          cx = cmd == "r" ? cx + arg(k) : arg(k);
          cy = cmd == "r" ? cy + arg(k + 1) : arg(k + 1);
          auto [x, y] = pt(cx, cy);
          g->LineTo(x, y);
        }
      } else if (cmd == "c" || cmd == "v") {
        begin();
        for (size_t k = 0; k + 5 < std::max<size_t>(a.size(), 6); k += 6) {
          const double bx = cmd == "v" ? cx : 0, by = cmd == "v" ? cy : 0;
          auto [x1, y1] = pt(bx + arg(k), by + arg(k + 1));
          auto [x2, y2] = pt(bx + arg(k + 2), by + arg(k + 3));
          cx = bx + arg(k + 4);
          cy = by + arg(k + 5);
          auto [x3, y3] = pt(cx, cy);
          g->CubicTo(x1, y1, x2, y2, x3, y3);
        }
      } else if (cmd == "x") {
        g->Close();
      } else if (cmd == "e") {
        begun = false;
        fill = filled_;
        stroke = stroked_;
      } else if (cmd == "nf") {
        fill = false;
      } else if (cmd == "ns") {
        stroke = false;
      } else if (cmd == "ae" || cmd == "al") {
        begin();
        for (size_t k = 0; k + 5 < std::max<size_t>(a.size(), 6); k += 6) {
          const double ecx = arg(k), ecy = arg(k + 1), rx = std::abs(arg(k + 2)), ry = std::abs(arg(k + 3));
          const double t0 = arg(k + 4) / 65536 * kRad, sweep = std::clamp(arg(k + 5) / 65536, -360.0, 360.0);
          const double sx = ecx + rx * std::cos(t0), sy = ecy + ry * std::sin(t0);
          auto [x, y] = pt(sx, sy);
          if (cmd == "al" && k == 0) g->MoveTo(x, y);
          else g->LineTo(x, y);
          cx = sx;
          cy = sy;
          Arc(*g, ecx, ecy, rx, ry, std::atan2(sy - ecy, sx - ecx), sweep, cx, cy);
        }
      } else if (cmd == "at" || cmd == "ar" || cmd == "wa" || cmd == "wr") {
        begin();
        for (size_t k = 0; k + 7 < std::max<size_t>(a.size(), 8); k += 8) {
          const double l = arg(k), t = arg(k + 1), r = arg(k + 2), b = arg(k + 3);
          const double ecx = (l + r) / 2, ecy = (t + b) / 2, rx = std::abs(r - l) / 2, ry = std::abs(b - t) / 2;
          const double a1 = std::atan2(arg(k + 5) - ecy, arg(k + 4) - ecx);
          const double a2 = std::atan2(arg(k + 7) - ecy, arg(k + 6) - ecx);
          const bool clockwise = cmd[0] == 'w';
          double sweep = (a2 - a1) / kRad;
          if (clockwise) {
            while (sweep <= 0) sweep += 360;
          } else {
            while (sweep >= 0) sweep -= 360;
          }
          const double tp = std::atan2(rx * std::sin(a1), ry * std::cos(a1));
          const double sx = ecx + rx * std::cos(tp), sy = ecy + ry * std::sin(tp);
          auto [x, y] = pt(sx, sy);
          if (cmd[1] == 'r') g->MoveTo(x, y);
          else g->LineTo(x, y);
          cx = sx;
          cy = sy;
          Arc(*g, ecx, ecy, rx, ry, a1, sweep, cx, cy);
        }
      } else if (cmd == "qx" || cmd == "qy") {
        begin();
        bool xFirst = cmd == "qx";
        for (size_t k = 0; k + 1 < std::max<size_t>(a.size(), 2); k += 2, xFirst = !xFirst) {
          const double x1 = arg(k), y1 = arg(k + 1);
          const double ecx = xFirst ? cx : x1, ecy = xFirst ? y1 : cy;
          const double rx = std::abs(x1 - cx), ry = std::abs(y1 - cy);
          const double cross = (cx - ecx) * (y1 - ecy) - (cy - ecy) * (x1 - ecx);
          auto [x, y] = pt(x1, y1);
          if (rx < 1e-9 || ry < 1e-9) {
            g->LineTo(x, y);
          } else {
            g->ArcTo(Num(rx), Num(ry), Num(std::atan2(cy - ecy, cx - ecx) / kRad * 60000), Num(cross > 0 ? 5400000 : -5400000));
          }
          cx = x1;
          cy = y1;
        }
      } else if (cmd == "qb") {
        begin();
        // Quadratic B-spline: control points, then the end point; midpoints are implied.
        for (size_t k = 0; k + 1 < a.size(); k += 2) {
          const bool last = k + 3 >= a.size();
          const double px = arg(k), py = arg(k + 1);
          const double ex = last ? px : (px + arg(k + 2)) / 2, ey = last ? py : (py + arg(k + 3)) / 2;
          if (last) {
            auto [x, y] = pt(px, py);
            g->LineTo(x, y);
          } else {
            auto [x1, y1] = pt(px, py);
            auto [x2, y2] = pt(ex, ey);
            g->QuadTo(x1, y1, x2, y2);
          }
          cx = ex;
          cy = ey;
        }
      }
    }
    // The text area, in the same units.
    if (!textboxrect.empty()) {
      Tokenize(textboxrect.substr(0, textboxrect.find(';')));
      double r[4] = {};
      for (size_t k = 0, n = 0; k < tokens_.size() && n < 4; ++k)
        if (!tokens_[k].command) r[n++] = tokens_[k].value;
      const char* names[4] = {"vml_tl", "vml_tt", "vml_tr", "vml_tb"};
      const double sizes[4] = {cw_, ch_, cw_, ch_};
      const double origins[4] = {ox_, oy_, ox_, oy_};
      for (int k = 0; k < 4; ++k) {
        g->AddGuide(names[k], std::string("*/ ") + (k % 2 ? "h " : "w ") + Num(r[k] - origins[k]) + " " +
                                  Num(sizes[k] > 0 ? sizes[k] : 1));
      }
      g->SetTextRect(names[0], names[1], names[2], names[3]);
    }
    return g;
  }

 private:
  static constexpr double kRad = 3.14159265358979323846 / 180;
  struct Token {
    bool command = false;
    std::string text;
    double value = 0;
  };

  // An arc on the ellipse (center, radii) from the visual angle `start` (radians), sweeping
  // `sweep` degrees (positive: clockwise).
  void Arc(Geometry& g, double ecx, double ecy, double rx, double ry, double start, double sweep, double& cx,
           double& cy) {
    if (rx < 1e-9 || ry < 1e-9 || sweep == 0) return;
    g.ArcTo(Num(rx), Num(ry), Num(start / kRad * 60000), Num(sweep * 60000));
    const double end = start + sweep * kRad;
    const double tp = std::atan2(rx * std::sin(end), ry * std::cos(end));
    cx = ecx + rx * std::cos(tp);
    cy = ecy + ry * std::sin(tp);
  }

  double Arg(std::string_view a) {
    if (a.empty()) return 0;
    if (a[0] == '#') {
      const size_t n = static_cast<size_t>(Number(a.substr(1), 0));
      return n < adj_.size() ? adj_[n] : 0;
    }
    if (a[0] == '@') {
      const size_t n = static_cast<size_t>(Number(a.substr(1), 0));
      return n < values_.size() ? values_[n] : 0;
    }
    if (a == "width") return cw_;
    if (a == "height") return ch_;
    if (a == "xcenter") return ox_ + cw_ / 2;
    if (a == "ycenter") return oy_ + ch_ / 2;
    if (a == "xlimo" || a == "ylimo") return 0;
    if (a == "pixelWidth") return size_.width / 0.75;
    if (a == "pixelHeight") return size_.height / 0.75;
    if (a == "pixelLineWidth") return 1;
    if (a == "emuWidth") return size_.width * 12700.0;
    if (a == "emuHeight") return size_.height * 12700.0;
    if (a == "emuWidth2") return size_.width * 6350.0;
    if (a == "emuHeight2") return size_.height * 6350.0;
    if (a == "lineDrawn" || a == "hasstroke") return stroked_ ? 1 : 0;
    if (a == "hasfill") return filled_ ? 1 : 0;
    return Number(a, 0);
  }

  double Eval(std::string_view eqn) {
    std::vector<std::string_view> t;
    size_t i = 0;
    while (i < eqn.size() && t.size() < 4) {
      while (i < eqn.size() && (eqn[i] == ' ' || eqn[i] == ',')) ++i;
      size_t j = i;
      while (j < eqn.size() && eqn[j] != ' ' && eqn[j] != ',') ++j;
      if (j > i) t.push_back(eqn.substr(i, j - i));
      i = j;
    }
    if (t.empty()) return 0;
    const double v = t.size() > 1 ? Arg(t[1]) : 0, p1 = t.size() > 2 ? Arg(t[2]) : 0, p2 = t.size() > 3 ? Arg(t[3]) : 0;
    const std::string_view op = t[0];
    const double fd = kRad / 65536;  // angles are in 1/65536 degree
    double r = 0;
    if (op == "val") r = v;
    else if (op == "sum") r = v + p1 - p2;
    else if (op == "product") r = p2 == 0 ? 0 : v * p1 / p2;
    else if (op == "mid") r = (v + p1) / 2;
    else if (op == "abs") r = std::abs(v);
    else if (op == "min") r = std::min(v, p1);
    else if (op == "max") r = std::max(v, p1);
    else if (op == "if") r = v > 0 ? p1 : p2;
    else if (op == "mod") r = std::sqrt(v * v + p1 * p1 + p2 * p2);
    else if (op == "atan2") r = std::atan2(p1, v) / fd;
    else if (op == "sin") r = v * std::sin(p1 * fd);
    else if (op == "cos") r = v * std::cos(p1 * fd);
    else if (op == "tan") r = v * std::tan(p1 * fd);
    else if (op == "cosatan2") r = v * std::cos(std::atan2(p2, p1));
    else if (op == "sinatan2") r = v * std::sin(std::atan2(p2, p1));
    else if (op == "sqrt") r = std::sqrt(std::max(0.0, v));
    else if (op == "sumangle") r = v + p1 * 65536 - p2 * 65536;
    else if (op == "ellipse") r = p1 == 0 ? 0 : p2 * std::sqrt(std::max(0.0, 1 - (v / p1) * (v / p1)));
    return std::isfinite(r) ? std::clamp(r, -1e12, 1e12) : 0;
  }

  void Tokenize(std::string_view s) {
    tokens_.clear();
    bool afterValue = false;  // for "m,l": an empty value between separators is 0
    size_t i = 0;
    while (i < s.size() && tokens_.size() < 200000) {
      const char c = s[i];
      if (c == ',') {
        if (!afterValue) tokens_.push_back({false, {}, 0});
        afterValue = false;
        ++i;
      } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++i;
      } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        size_t n = (c == 'a' || c == 'w' || c == 'q' || c == 'n' || c == 'h') && i + 1 < s.size() ? 2 : 1;
        tokens_.push_back({true, std::string(s.substr(i, n)), 0});
        afterValue = false;  // "m,l": a separator right after a command stands for a 0
        i += n;
      } else {
        size_t j = i + 1;
        while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.')) ++j;
        tokens_.push_back({false, {}, Arg(s.substr(i, j - i))});
        afterValue = true;
        i = j;
      }
    }
  }

  double cw_, ch_, ox_, oy_;
  Size size_;
  std::vector<double> adj_;
  bool stroked_, filled_;
  std::vector<double> values_;
  std::vector<Token> tokens_;
};

class VmlReader {
 public:
  explicit VmlReader(ReadContext& ctx) : ctx_(ctx) {}

  bool Read(pugi::xml_node el, Shape& s, bool inGroup) {
    if (++ctx_.shapes > ReadContext::kMaxShapes || NamespaceOf(el) != kV) return false;
    const std::string_view kind = LocalNameOf(el);
    if (kind == "shapetype" || kind == "formulas" || kind == "handles") return false;
    auto style = ParseVmlStyle(el.attribute("style").value());
    // Top-level shapes are in points (unitless values are pixels); a group's children are in its
    // coordinate units.
    const float unit = inGroup ? 1.f : 0.75f;
    const bool absolute = inGroup || StyleValue(style, "position") == "absolute";
    float x = 0, y = 0;
    if (absolute) {
      x = VmlLength(StyleValue(style, "left"), unit) + VmlLength(StyleValue(style, "margin-left"), unit);
      y = VmlLength(StyleValue(style, "top"), unit) + VmlLength(StyleValue(style, "margin-top"), unit);
    }
    const float w = std::max(0.f, VmlLength(StyleValue(style, "width"), unit));
    const float h = std::max(0.f, VmlLength(StyleValue(style, "height"), unit));
    s.bounds = {x, y, x + w, y + h};
    s.name = el.attribute("id").value();
    s.description = el.attribute("alt").value();
    s.hidden = StyleValue(style, "visibility") == "hidden";
    std::string_view rot = StyleValue(style, "rotation");
    s.rotation = static_cast<float>(rot.ends_with("fd") ? Number(rot, 0) / 65536 : Number(rot, 0));
    std::string_view flip = StyleValue(style, "flip");
    s.flipH = flip.find('x') != std::string_view::npos;
    s.flipV = flip.find('y') != std::string_view::npos;

    if (kind == "group") return ReadGroup(el, s);

    // The shape type: from the document's v:shapetype, else the attributes alone.
    pugi::xml_node type = FindShapeType(el);
    auto attr = [&](const char* name) {
      pugi::xml_attribute a = el.attribute(name);
      return a ? a : type.attribute(name);
    };
    auto officeAttr = [&](std::string_view name) {
      pugi::xml_attribute a = FindAttribute(el, kO, name);
      return a ? a : FindAttribute(type, kO, name);
    };
    auto child = [&](std::string_view name) {
      pugi::xml_node c = Child(el, kV, name);
      return c ? c : Child(type, kV, name);
    };

    ReadFillAndStroke(el, type, s);
    ReadShadow(child("shadow"), s);

    if (kind == "line") {
      // from/to, in the same units as the style; the bounds run between them, flipped as needed.
      const Point a = PointAttr(el.attribute("from").value(), unit), b = PointAttr(el.attribute("to").value(), unit);
      s.bounds = {std::min(a.x, b.x) + x, std::min(a.y, b.y) + y, std::max(a.x, b.x) + x, std::max(a.y, b.y) + y};
      s.flipH = b.x < a.x;
      s.flipV = b.y < a.y;
      s.geometry = Geometry::Preset("line");
      s.fill = Fill{};
      return true;
    }
    if (kind == "polyline") return ReadPolyline(el, unit, x, y, s);
    if (kind == "rect") s.geometry = Geometry::Preset("rect");
    else if (kind == "oval") s.geometry = Geometry::Preset("ellipse");
    else if (kind == "roundrect") {
      s.geometry = Geometry::Preset("roundRect");
      // arcsize: the corner radius over half the shorter side.
      s.adjust.push_back({"adj", Fraction(el.attribute("arcsize").value(), 0.2) * 50000});
    } else if (kind == "shape" || kind == "image") {
      const int spt = static_cast<int>(Number(officeAttr("spt").value(), 0));
      std::string_view path = attr("path").value();
      pugi::xml_node pathEl = child("path");
      if (!path.empty() && spt != 75 && spt != 202 && spt != 1) {
        s.geometry = CustomGeometry(el, type, path, pathEl, {w, h});
      } else if (std::string_view preset = PresetForSpt(spt); !preset.empty()) {
        s.geometry = Geometry::Preset(preset);
      } else if (kind == "shape" && !child("imagedata")) {
        s.geometry = std::make_shared<Geometry>();  // no path and no known type: Word draws nothing
      }
    }

    // Pictures: v:imagedata (and v:image's own src).
    pugi::xml_node imagedata = child("imagedata");
    if (imagedata || kind == "image") {
      s.kind = Shape::Kind::Picture;
      s.picture = ReadPicture(imagedata ? imagedata : el);
      if (!child("stroke") && !el.attribute("stroked") && !el.attribute("strokecolor")) s.line.fill = Fill{};
      if (!el.attribute("filled") && !el.attribute("fillcolor") && !Child(el, kV, "fill")) s.fill = Fill{};
      if (std::string_view title = FindAttribute(imagedata, kO, "title").value(); s.description.empty()) {
        s.description = title;
      }
    }
    // WordArt (watermarks): text drawn as the shape.
    pugi::xml_node textpath = Child(el, kV, "textpath");
    if (textpath && (Bool(textpath, "on", false) || Bool(Child(type, kV, "textpath"), "on", false))) {
      ReadTextPath(textpath, s);
    }
    if (pugi::xml_node box = Child(el, kV, "textbox")) ReadTextBox(box, style, s);
    return true;
  }

 private:
  bool ReadGroup(pugi::xml_node el, Shape& s) {
    s.kind = Shape::Kind::Group;
    const Point size = PointAttr(el.attribute("coordsize").value(), 1);
    const Point origin = PointAttr(el.attribute("coordorigin").value(), 1);
    const float cw = size.x != 0 ? size.x : 1000, ch = size.y != 0 ? size.y : 1000;
    s.childBounds = {origin.x, origin.y, origin.x + cw, origin.y + ch};
    if (ctx_.depth >= ReadContext::kMaxDepth) return true;
    ++ctx_.depth;
    ForEachChild(el, [&](pugi::xml_node c) {
      Shape child;
      if (Read(c, child, true)) s.children.push_back(std::move(child));
      return ctx_.shapes <= ReadContext::kMaxShapes;
    });
    --ctx_.depth;
    return true;
  }

  // "x,y" (or "x y") in `unit`s.
  static Point PointAttr(std::string_view v, float unit) {
    size_t sep = v.find_first_of(", ");
    return {VmlLength(v.substr(0, sep), unit),
            sep == std::string_view::npos ? 0.f : VmlLength(v.substr(sep + 1), unit)};
  }

  bool ReadPolyline(pugi::xml_node el, float unit, float x, float y, Shape& s) {
    std::string_view v = el.attribute("points").value();
    std::vector<float> nums;
    size_t i = 0;
    while (i < v.size() && nums.size() < 20000) {
      while (i < v.size() && (v[i] == ' ' || v[i] == ',')) ++i;
      size_t j = i;
      while (j < v.size() && v[j] != ' ' && v[j] != ',') ++j;
      if (j > i) nums.push_back(VmlLength(v.substr(i, j - i), unit));
      i = j;
    }
    if (nums.size() < 4) return false;
    float l = nums[0], t = nums[1], r = nums[0], b = nums[1];
    for (size_t k = 0; k + 1 < nums.size(); k += 2) {
      l = std::min(l, nums[k]);
      r = std::max(r, nums[k]);
      t = std::min(t, nums[k + 1]);
      b = std::max(b, nums[k + 1]);
    }
    auto g = std::make_shared<Geometry>();
    g->BeginPath(std::max(r - l, 1e-3f), std::max(b - t, 1e-3f),
                 s.fill.kind != Fill::Kind::None ? Geometry::PathFill::Normal : Geometry::PathFill::None, true);
    for (size_t k = 0; k + 1 < nums.size(); k += 2) {
      if (k == 0) g->MoveTo(Num(nums[k] - l), Num(nums[k + 1] - t));
      else g->LineTo(Num(nums[k] - l), Num(nums[k + 1] - t));
    }
    s.geometry = std::move(g);
    s.bounds = {x + l, y + t, x + r, y + b};
    return true;
  }

  pugi::xml_node FindShapeType(pugi::xml_node el) {
    std::string_view ref = el.attribute("type").value();
    if (!ref.starts_with("#")) return {};
    ref.remove_prefix(1);
    auto cached = types_.find(std::string(ref));
    if (cached != types_.end()) return cached->second;
    // Word writes the shape type next to the first shape that uses it: look in this container
    // first, then the whole part.
    pugi::xml_node found;
    for (pugi::xml_node scope : {el.parent(), el.root()}) {
      for (pugi::xml_node n = scope.first_child(); n && !found;) {
        if (Is(n, kV, "shapetype") && ref == n.attribute("id").value()) found = n;
        if (scope == el.parent()) {
          n = n.next_sibling();
          continue;
        }
        if (n.first_child()) {
          n = n.first_child();
          continue;
        }
        while (n && n != scope && !n.next_sibling()) n = n.parent();
        n = n && n != scope ? n.next_sibling() : pugi::xml_node();
      }
      if (found) break;
    }
    types_[std::string(ref)] = found;
    return found;
  }

  std::shared_ptr<const Geometry> CustomGeometry(pugi::xml_node el, pugi::xml_node type, std::string_view path,
                                                 pugi::xml_node pathEl, Size size) {
    pugi::xml_attribute cs = el.attribute("coordsize") ? el.attribute("coordsize") : type.attribute("coordsize");
    pugi::xml_attribute co = el.attribute("coordorigin") ? el.attribute("coordorigin") : type.attribute("coordorigin");
    const Point csz = PointAttr(cs.value(), 1), org = PointAttr(co.value(), 1);
    // Adjust values: the shape type's defaults, then the shape's own.
    std::vector<double> adj;
    for (pugi::xml_node src : {type, el}) {
      std::string_view list = src.attribute("adj").value();
      size_t i = 0, n = 0;
      while (i <= list.size() && n < 10 && !list.empty()) {
        size_t j = list.find(',', i);
        if (j == std::string_view::npos) j = list.size();
        if (adj.size() <= n) adj.resize(n + 1, 0);
        if (j > i) adj[n] = Number(list.substr(i, j - i), 0);
        ++n;
        i = j + 1;
      }
    }
    const bool stroked = Bool(el, "stroked", Bool(type, "stroked", true));
    const bool filled = Bool(el, "filled", Bool(type, "filled", true));
    VmlGeometry vg(csz.x != 0 ? csz.x : 1000, csz.y != 0 ? csz.y : 1000, org.x, org.y, size, adj, stroked, filled);
    vg.Formulas(Child(type, kV, "formulas"));
    if (pugi::xml_node own = Child(el, kV, "formulas")) vg.Formulas(own);
    std::shared_ptr<Geometry> g = vg.Path(path, pathEl.attribute("textboxrect").value());
    return g->Empty() ? nullptr : g;
  }

  void ReadFillAndStroke(pugi::xml_node el, pugi::xml_node type, Shape& s) {
    auto attr = [&](const char* name) {
      pugi::xml_attribute a = el.attribute(name);
      return a ? a : type.attribute(name);
    };
    // Fill: white by default.
    const std::optional<Color> base = attr("fillcolor") ? ParseColor(attr("fillcolor").value()) : Rgb(0xFFFFFF);
    pugi::xml_node fill = Child(el, kV, "fill");
    if (Bool(attr("filled").value(), true) && Bool(fill.attribute("on").value(), true) && base) {
      Color color = fill.attribute("color") ? ParseColor(fill.attribute("color").value()).value_or(*base) : *base;
      color.a = static_cast<float>(std::clamp(Fraction(fill.attribute("opacity").value(), 1), 0.0, 1.0));
      std::string_view kind = fill.attribute("type").value();
      s.fill.kind = Fill::Kind::Solid;
      s.fill.color = color;
      if (kind == "gradient" || kind == "gradientRadial") {
        Color color2 = ParseColor(fill.attribute("color2").value(), color).value_or(Rgb(0xFFFFFF));
        color2.a = static_cast<float>(
            std::clamp(Fraction(FindAttribute(fill, kO, "opacity2").value(), color.a), 0.0, 1.0));
        ReadGradient(fill, color, color2, kind == "gradientRadial", s.fill);
      } else if (kind == "tile" || kind == "frame" || kind == "pattern") {
        std::string_view id = RelId(fill, "id");
        if (id.empty()) id = FindAttribute(fill, kO, "relid").value();
        if (std::shared_ptr<const Image> img = id.empty() ? nullptr : ctx_.resources.GetImage(id)) {
          s.fill.kind = Fill::Kind::Picture;
          s.fill.picture = std::make_shared<PictureFill>();
          s.fill.picture->image = img;
          s.fill.picture->tile = kind != "frame";
          s.fill.picture->alpha = color.a;
        }
      }
    } else {
      s.fill = Fill{};
    }
    // Stroke: black, 0.75 pt by default.
    pugi::xml_node stroke = Child(el, kV, "stroke");
    if (Bool(attr("stroked").value(), true) && Bool(stroke.attribute("on").value(), true)) {
      Color color = ParseColor(attr("strokecolor") ? attr("strokecolor").value() : "black").value_or(Rgb(0));
      if (stroke.attribute("color")) color = ParseColor(stroke.attribute("color").value()).value_or(color);
      color.a = static_cast<float>(std::clamp(Fraction(stroke.attribute("opacity").value(), 1), 0.0, 1.0));
      s.line.fill.kind = Fill::Kind::Solid;
      s.line.fill.color = color;
      std::string_view weight = stroke.attribute("weight") ? stroke.attribute("weight").value() : attr("strokeweight").value();
      s.line.width = weight.empty() ? 0.75f : std::clamp(VmlLength(weight, 1.f / 12700), 0.f, 1000.f);
      s.line.dashes = VmlDash(stroke.attribute("dashstyle").value());
      std::string_view join = stroke.attribute("joinstyle").value(), cap = stroke.attribute("endcap").value();
      s.line.join = join == "miter" ? LineJoin::Miter : join == "bevel" ? LineJoin::Bevel : LineJoin::Round;
      s.line.cap = cap == "round" ? LineCap::Round : cap == "square" ? LineCap::Square : LineCap::Flat;
      s.line.head = ParseArrow(stroke, "startarrow", "startarrowwidth", "startarrowlength");
      s.line.tail = ParseArrow(stroke, "endarrow", "endarrowwidth", "endarrowlength");
    } else {
      s.line.fill = Fill{};
    }
  }

  // VML gradients as Word draws them (measured): along the gradient (from the focus outward for
  // radial ones), color2 sits at the focus fraction with color at both ends; a negative focus
  // trades the two and counts from the far end. So 0 runs from color2 to color, 100 % (and
  // -100 %) from color to color2, 50 % is color, color2, color, and -50 % color2, color, color2.
  void ReadGradient(pugi::xml_node fill, Color color, Color color2, bool radial, Fill& out) {
    out.kind = Fill::Kind::Gradient;
    const double focus = std::clamp(Fraction(fill.attribute("focus").value(), 0), -1.0, 1.0);
    // The ramp from color (0) to color2 (1), or the colors attribute's: "0 red;.5 #00ff00;1 blue".
    std::vector<GradientStop> ramp;
    std::string_view colors = fill.attribute("colors").value();
    while (!colors.empty() && ramp.size() < 64) {
      size_t end = colors.find(';');
      std::string_view stop = colors.substr(0, end);
      colors = end == std::string_view::npos ? std::string_view() : colors.substr(end + 1);
      while (!stop.empty() && stop.front() == ' ') stop.remove_prefix(1);
      size_t sp = stop.find(' ');
      if (sp == std::string_view::npos) continue;
      if (std::optional<Color> c = ParseColor(stop.substr(sp + 1))) {
        ramp.push_back({static_cast<float>(std::clamp(Fraction(stop.substr(0, sp), 0), 0.0, 1.0)), *c});
      }
    }
    if (ramp.empty()) ramp = {{0, color}, {1, color2}};
    // q runs from the ends' color (0) to the focus color (1).
    const bool swap = focus < 0;
    const float at = static_cast<float>(swap ? 1 + focus : focus);
    out.stops.clear();
    for (const GradientStop& g : ramp) {
      const float q = swap ? 1 - g.offset : g.offset;
      if (at > 0) out.stops.push_back({q * at, g.color});
      if (at < 1 && (q < 1 || at <= 0)) out.stops.push_back({at + (1 - q) * (1 - at), g.color});
    }
    std::stable_sort(out.stops.begin(), out.stops.end(),
                     [](const GradientStop& a, const GradientStop& b) { return a.offset < b.offset; });
    if (radial) {
      out.shape = Fill::GradientShape::Rect;
      const Point pos = PointAttr(fill.attribute("focusposition").value(), 1);
      const Point size = PointAttr(fill.attribute("focussize").value(), 1);
      out.focus = {pos.x, pos.y, 1 - pos.x - size.x, 1 - pos.y - size.y};
    } else {
      // Angle 0 runs top to bottom, 90 left to right, 180 bottom to top; Word draws a negative
      // angle as that plus 180.
      double angle = std::fmod(Number(fill.attribute("angle").value(), 0), 360.0);
      if (angle < 0) angle += 180;
      out.angle = static_cast<float>(90 - angle);
      out.scaled = true;
    }
  }

  void ReadShadow(pugi::xml_node shadow, Shape& s) {
    if (!shadow || !Bool(shadow, "on", false)) return;
    const Point off = PointAttr(shadow.attribute("offset") ? shadow.attribute("offset").value() : "2pt,2pt", 0.75f);
    s.shadow.on = true;
    s.shadow.color = ParseColor(shadow.attribute("color").value()).value_or(Rgb(0x808080));
    s.shadow.color.a = static_cast<float>(std::clamp(Fraction(shadow.attribute("opacity").value(), 1), 0.0, 1.0));
    s.shadow.distance = std::hypot(off.x, off.y);
    s.shadow.direction = std::atan2(off.y, off.x) * 180 / 3.14159265f;
  }

  std::shared_ptr<PictureFill> ReadPicture(pugi::xml_node data) {
    auto pic = std::make_shared<PictureFill>();
    std::string_view id = RelId(data, "id");
    if (id.empty()) id = FindAttribute(data, kO, "relid").value();
    if (id.empty()) id = RelId(data, "pict");
    if (!id.empty()) pic->image = ctx_.resources.GetImage(id);
    pic->crop = {static_cast<float>(Fraction(data.attribute("cropleft").value(), 0)),
                 static_cast<float>(Fraction(data.attribute("croptop").value(), 0)),
                 static_cast<float>(Fraction(data.attribute("cropright").value(), 0)),
                 static_cast<float>(Fraction(data.attribute("cropbottom").value(), 0))};
    pic->grayscale = Bool(data, "grayscale", false);
    if (Bool(data, "bilevel", false)) pic->biLevel = 0.5f;
    pic->gain = static_cast<float>(std::clamp(Fraction(data.attribute("gain").value(), 1), 0.0, 100.0));
    pic->blackLevel = static_cast<float>(std::clamp(Fraction(data.attribute("blacklevel").value(), 0), -1.0, 1.0));
    if (std::optional<Color> key = ParseColor(data.attribute("chromakey").value())) {
      pic->colorChange = true;
      pic->changeFrom = *key;
      pic->changeTo = {key->r, key->g, key->b, 0};
    }
    return pic;
  }

  void ReadTextPath(pugi::xml_node textpath, Shape& s) {
    auto tp = std::make_shared<TextPath>();
    std::string_view text = textpath.attribute("string").value();
    tp->text = Utf8ToWide(text.substr(0, 4096));
    auto style = ParseVmlStyle(textpath.attribute("style").value());
    std::string_view font = StyleValue(style, "font-family");
    while (!font.empty() && (font.front() == '"' || font.front() == '\'')) font.remove_prefix(1);
    while (!font.empty() && (font.back() == '"' || font.back() == '\'')) font.remove_suffix(1);
    if (!font.empty()) tp->font = Utf8ToWide(font.substr(0, 128));
    tp->bold = StyleValue(style, "font-weight") == "bold";
    tp->italic = StyleValue(style, "font-style") == "italic";
    s.textPath = std::move(tp);
  }

  void ReadTextBox(pugi::xml_node box, const std::vector<std::pair<std::string_view, std::string_view>>& shapeStyle,
                   Shape& s) {
    pugi::xml_node content = Child(box, kW, "txbxContent");
    if (!content) return;
    auto t = std::make_shared<TextBody>();
    t->content = content;
    t->properties = box;
    // inset="l,t,r,b"; missing values keep the defaults (0.1 in, 0.05 in).
    std::string_view inset = box.attribute("inset").value();
    float* sides[4] = {&t->insets.left, &t->insets.top, &t->insets.right, &t->insets.bottom};
    for (int k = 0; k < 4 && !inset.empty(); ++k) {
      size_t end = inset.find(',');
      std::string_view v = inset.substr(0, end);
      if (!v.empty()) *sides[k] = VmlLength(v, 0.75f);
      inset = end == std::string_view::npos ? std::string_view() : inset.substr(end + 1);
    }
    auto style = ParseVmlStyle(box.attribute("style").value());
    if (StyleValue(style, "mso-fit-shape-to-text") == "t") t->box.autofit = TextBox::Autofit::ResizeShape;
    std::string_view flow = StyleValue(style, "layout-flow");
    if (flow == "vertical") t->box.flow = TextBox::Flow::Vertical;
    else if (flow == "vertical-ideographic") t->box.flow = TextBox::Flow::EastAsian;
    std::string_view anchor = StyleValue(shapeStyle, "v-text-anchor");
    if (anchor.starts_with("middle")) t->box.anchor = TextBox::Anchor::Middle;
    else if (anchor.starts_with("bottom")) t->box.anchor = TextBox::Anchor::Bottom;
    t->box.anchorCenter = anchor.ends_with("center") || anchor.ends_with("baseline");
    t->box.wrap = StyleValue(shapeStyle, "mso-wrap-style") != "none";
    s.text = std::move(t);
  }

  static std::wstring Utf8ToWide(std::string_view s) {
    std::wstring out;
    for (size_t i = 0; i < s.size();) {
      const unsigned char c = static_cast<unsigned char>(s[i]);
      uint32_t cp = c;
      size_t n = 1;
      if (c >= 0xF0 && i + 3 < s.size()) {
        cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
        n = 4;
      } else if (c >= 0xE0 && i + 2 < s.size()) {
        cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
        n = 3;
      } else if (c >= 0xC0 && i + 1 < s.size()) {
        cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
        n = 2;
      }
      if (cp >= 0x10000) {
        cp -= 0x10000;
        out += static_cast<wchar_t>(0xD800 + (cp >> 10));
        out += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
      } else {
        out += static_cast<wchar_t>(cp);
      }
      i += n;
    }
    return out;
  }

  ReadContext& ctx_;
  std::map<std::string, pugi::xml_node> types_;
};

}  // namespace

bool ReadVmlShape(pugi::xml_node el, ReadContext& ctx, Shape& out) { return VmlReader(ctx).Read(el, out, false); }

}  // namespace draw
