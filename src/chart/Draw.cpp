// Laying out a chart (fwchart) as vector art: the chart area, title and legend, the axes with their
// labels and gridlines, then each group's marks and data labels, where Office puts them (measured
// on Excel's PDFs). Where the file says nothing, the look is Office's: 10 pt black text, black or
// gray gridlines (Office 2007's look or 2010's), the theme's accent colors in order. Text is drawn
// as glyph outlines, so it prints and exports like any shape.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "chart/Chart.h"
#include "core/Strings.h"
#include "draw/Theme.h"

namespace fw::chart {
namespace {

using draw::Color;
using draw::Matrix;
using draw::Path;
using draw::Point;
using draw::Rect;

constexpr size_t kMaxMarks = 60000;
constexpr double kPi = 3.14159265358979323846;
// kGray: lines a chart doesn't give a color (its border; gridlines and axes in Office 2010's look),
// as Office draws them; Office 2013 and later state theirs.
const Color kText = draw::Rgb(0x000000), kGray = draw::Rgb(0x868686), kWhite = draw::Rgb(0xFFFFFF);

std::wstring Wide(const std::string& s) { return fw::Utf8ToWide(s); }

wchar_t g_decimal = L'.', g_group = L',', g_list = L',';
std::string g_shortDate;  // empty: m/d/yyyy as it is

// Digits with '.' and ',' as the separators in use show them.
std::wstring Localized(const std::string& number) {
  std::wstring w(number.begin(), number.end());
  for (wchar_t& c : w) c = c == L'.' ? g_decimal : c == L',' ? g_group : c;
  return w;
}

// ---- Numbers as format codes show them

// Excel's serial day (the 1900 system, with its 29 February 1900) as a date; the weekday 0 is Sunday.
void SerialDate(double v, int& y, int& m, int& d, int& weekday) {
  long long serial = static_cast<long long>(std::floor(std::clamp(v, 1.0, 2958465.0)));
  long long z = serial - (serial < 61 ? 25568 : 25569);  // days since 1970-01-01
  weekday = static_cast<int>(((z % 7) + 11) % 7);
  z += 719468;
  const long long era = (z >= 0 ? z : z - 146096) / 146097;
  const long long doe = z - era * 146097;
  const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const long long mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(yoe + era * 400 + (m <= 2 ? 1 : 0));
}

std::wstring FormatDate(double v, std::string_view code) {
  static const char* kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                                  "July",    "August",   "September", "October", "November", "December"};
  static const char* kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  int y, m, d, wd;
  SerialDate(v, y, m, d, wd);
  std::string out;
  char buf[16];
  for (size_t i = 0; i < code.size();) {
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(code[i])));
    size_t n = 1;
    while (i + n < code.size() && std::tolower(static_cast<unsigned char>(code[i + n])) == c) ++n;
    if (c == 'y') {
      std::snprintf(buf, sizeof buf, n <= 2 ? "%02d" : "%04d", n <= 2 ? y % 100 : y);
      out += buf;
    } else if (c == 'm') {
      if (n <= 2) std::snprintf(buf, sizeof buf, n == 1 ? "%d" : "%02d", m), out += buf;
      else if (n == 3) out += std::string(kMonths[m - 1]).substr(0, 3);
      else if (n == 5) out += kMonths[m - 1][0];
      else out += kMonths[m - 1];
    } else if (c == 'd') {
      if (n <= 2) std::snprintf(buf, sizeof buf, n == 1 ? "%d" : "%02d", d), out += buf;
      else out += n == 3 ? std::string(kDays[wd]).substr(0, 3) : std::string(kDays[wd]);
    } else if (c == '"') {
      const size_t e = code.find('"', i + 1);
      out += code.substr(i + 1, e == std::string_view::npos ? std::string_view::npos : e - i - 1);
      i = e == std::string_view::npos ? code.size() : e + 1;
      continue;
    } else if (c == '\\' && i + 1 < code.size()) {
      out += code[i + 1];
      i += 2;
      continue;
    } else if (c == '[') {
      const size_t e = code.find(']', i);
      i = e == std::string_view::npos ? code.size() : e + 1;
      continue;
    } else if (c == 'h' || c == 's' || c == '@' || c == '_' || c == '*') {
      // Times aren't shown on chart axes Featherweight draws; padding takes no room.
      i += c == '_' || c == '*' ? 2 : n;
      continue;
    } else {
      out.append(n, code[i]);
    }
    i += n;
  }
  return Wide(out);
}

std::wstring General(double v) {
  char buf[48];
  const double a = std::fabs(v);
  std::snprintf(buf, sizeof buf, v != 0 && (a >= 1e11 || a < 1e-9) ? "%.4E" : "%.10g", v);
  return Localized(buf);
}

}  // namespace

void SetNumberSeparators(wchar_t decimal, wchar_t group) {
  g_decimal = decimal ? decimal : L'.';
  g_group = group ? group : L',';
}

void SetListSeparator(wchar_t separator) { g_list = separator ? separator : L','; }

void SetShortDate(std::string_view picture) {
  // Windows' picture (d, M, y, 'text') as a format code (d, m, y, "text").
  g_shortDate.clear();
  for (char c : picture.substr(0, 64)) g_shortDate += c == '\'' ? '"' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::wstring FormatNumber(double v, std::string_view code) {
  if (!std::isfinite(v)) return {};
  // The first section, outside quotes.
  bool quoted = false;
  size_t end = code.size();
  for (size_t i = 0; i < code.size(); ++i) {
    if (code[i] == '"') quoted = !quoted;
    else if (code[i] == ';' && !quoted) {
      end = i;
      break;
    }
  }
  code = code.substr(0, std::min<size_t>(end, 256));
  std::string lower(code);
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (lower.empty() || lower == "general") return General(v);
  // Excel's built-in short date (format 14) is shown as the user's.
  if ((lower == "m/d/yyyy" || lower == "mm-dd-yy") && !g_shortDate.empty()) return FormatDate(v, g_shortDate);

  std::string before, after;
  bool digits = false, inDecimals = false, group = false, percent = false, date = false;
  int decimals = 0;
  for (size_t i = 0; i < code.size(); ++i) {
    const char c = code[i];
    std::string& lit = digits ? after : before;
    if (c == '"') {
      const size_t e = code.find('"', i + 1);
      lit += code.substr(i + 1, e == std::string_view::npos ? std::string_view::npos : e - i - 1);
      i = e == std::string_view::npos ? code.size() : e;
    } else if (c == '\\' && i + 1 < code.size()) {
      lit += code[++i];
    } else if (c == '[') {
      // [$€-413]: the currency's text; [Red], [>100]: nothing shown.
      const size_t e = code.find(']', i);
      const std::string_view inside = code.substr(i + 1, e == std::string_view::npos ? std::string_view::npos : e - i - 1);
      if (!inside.empty() && inside.front() == '$') lit += inside.substr(1, inside.find('-') - 1);
      i = e == std::string_view::npos ? code.size() : e;
    } else if (c == '_' || c == '*') {
      ++i;
    } else if (c == '0' || c == '#' || c == '?') {
      digits = true;
      if (inDecimals) ++decimals;
    } else if (c == '.' && (digits || (i + 1 < code.size() && (code[i + 1] == '0' || code[i + 1] == '#')))) {
      inDecimals = digits = true;
    } else if (c == ',' && digits && !inDecimals) {
      group = true;
    } else if (c == '%') {
      percent = true;
      lit += '%';
    } else {
      const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (!digits && (l == 'y' || l == 'm' || l == 'd')) date = true;
      lit += c;
    }
  }
  if (date && !digits) return FormatDate(v, code);
  if (!digits) return before.empty() ? General(v) : Wide(before);
  const double x = percent ? v * 100 : v;
  char buf[64];
  // Halves round away from zero, as Excel rounds (9.25 shows as 9.3 at one decimal, where printf
  // gives the even 9.2).
  const int places = std::clamp(decimals, 0, 10);
  const double scale = std::pow(10.0, places);
  const double rounded = std::fabs(x) * scale < 1e15 ? std::floor(std::fabs(x) * scale + 0.5 + 1e-9) / scale : std::fabs(x);
  std::snprintf(buf, sizeof buf, "%.*f", places, rounded);
  std::string number = buf;
  if (number.size() > 40) return General(v);
  if (group) {
    const size_t dot = number.find('.');
    for (long long k = static_cast<long long>(dot == std::string::npos ? number.size() : dot) - 3; k > 0; k -= 3)
      number.insert(static_cast<size_t>(k), ",");
  }
  const bool negative = x < 0 && number.find_first_of("123456789") != std::string::npos;
  return (negative ? L"-" : L"") + Wide(before) + Localized(number) + Wide(after);
}

namespace {

// ---- Building the art

// Office's chart layout, measured on Excel's PDFs of charts with 10- and 12-point Calibri text: the
// chart area's padding in points, the rest mostly as parts of the text size.
constexpr float kPad = 6.5f;
constexpr float kAscent = 0.75f, kDescent = 0.25f, kLine = 1.22f;  // Calibri's, of its size
constexpr float kMargin = 0.46f;     // the plot area inside the room left for it
constexpr float kValueGap = 0.93f;   // labels beside an axis to its line
constexpr float kLabelGap = 0.84f;   // the axis line to the top of the labels below it
constexpr float kMiddle = 0.29f;     // a line of text's baseline below its middle
constexpr float kLabelMiddle = 0.36f;  // the same for data labels
constexpr float kTitleGap = 8.2f;    // points: the title's (or a legend's) line to the room below
constexpr float kAxisTitle = 6.1f, kAxisTitleGap = 5.07f;  // points: an axis title from the room's edge; to its labels
constexpr float kKey = 0.549f;       // a legend key's square
constexpr float kAcross = 4.0f;      // a value axis across the bottom: a label every so many text sizes at most
constexpr double kBubble = 0.25;     // the largest bubble across, of the plot's smaller side
constexpr size_t kOutlinedLabels = 64;  // a category axis whose labels are all outlined at once (more: when drawn)

struct Label {
  Path path;
  float width = 0;
};

class Builder {
 public:
  Builder(draw::Size size, std::wstring font) : font_(std::move(font)) { art_.size = size; }
  // The font's ascent over Calibri's on a slide (1 elsewhere): PowerPoint gives labels, keys and
  // the gaps around them room by the font's height, and the constants here are Calibri's.
  float tall = 1;

  void Add(Path p, std::optional<Color> fill, std::optional<Color> line, float width, const Matrix& t = {}) {
    if (p.Empty()) return;
    // A run of untransformed shapes filled in one opaque color with no outline (a series' bars)
    // is one mark, painted at once (a thousand bars: one shape, not a thousand).
    if (fill && !line && fill->a >= 1 && Identity(t) && !art_.marks.empty()) {
      draw::Art::Mark& last = art_.marks.back();
      const Color& c = last.fill.color;
      if (last.line.fill.kind == draw::Fill::Kind::None && last.fill.kind == draw::Fill::Kind::Solid && Identity(last.transform) &&
          c.r == fill->r && c.g == fill->g && c.b == fill->b && c.a == fill->a && last.path.fillRule == p.fillRule) {
        Append(last.path, p);
        return;
      }
    }
    if (art_.marks.size() >= kMaxMarks) return;
    draw::Art::Mark m;
    m.path = std::move(p);
    m.transform = t;
    if (fill) {
      m.fill.kind = draw::Fill::Kind::Solid;
      m.fill.color = *fill;
    }
    if (line) {
      m.line.fill.kind = draw::Fill::Kind::Solid;
      m.line.fill.color = *line;
      m.line.width = width;
      // Axes, gridlines, ticks and outlines end flat, their corners round (Office's PDFs).
      m.line.cap = draw::LineCap::Flat;
      m.line.join = draw::LineJoin::Round;
    }
    art_.marks.push_back(std::move(m));
  }
  // A mark in the file's look (c:spPr) over the defaults.
  void Mark(Path p, const Look& look, Color fill, std::optional<Color> line, float width) {
    std::optional<Color> f;
    if (!look.noFill) f = look.fill.value_or(fill);
    std::optional<Color> l = look.noLine ? std::nullopt : look.line ? look.line : line;
    Add(std::move(p), f, l, look.lineWidth >= 0 ? look.lineWidth : width);
  }
  // A series' line (and its legend key): round ends too, and `dashes` (multiples of the width)
  // shortened by them and their gaps lengthened, as PowerPoint's PDFs have them (sysDot at 2 points:
  // dots 4 points apart).
  void Stroke(Path p, Color line, float width, const std::vector<float>& dashes = {}) {
    const size_t n = art_.marks.size();
    Add(std::move(p), std::nullopt, line, width);
    if (art_.marks.size() > n) {
      draw::Line& l = art_.marks.back().line;
      l.cap = draw::LineCap::Round;
      for (size_t k = 0; k < dashes.size() && k < 64; ++k) l.dashes.push_back(k % 2 ? dashes[k] + 1 : std::max(0.0f, dashes[k] - 1));
      if (l.dashes.size() % 2) l.dashes.push_back(l.dashes.back());
    }
  }
  void Line(Point a, Point b, Color c, float width) {
    Path p;
    p.MoveTo(a);
    p.LineTo(b);
    Add(std::move(p), std::nullopt, c, width);
  }
  Label Text(const std::wstring& s, float size, bool bold = false, const std::wstring& font = {}) const {
    Label l;
    if (s.empty() || !(size > 0)) return l;
    l.path = draw::TextOutline(s.substr(0, 255), font.empty() ? font_ : font, bold, false, size);
    if (!l.path.Empty()) l.width = std::max(0.0f, l.path.Bounds().right);
    return l;
  }
  // Text no wider than `width`: what fits of it followed by "…" when the whole is wider. Only text
  // far too long for its place is cut (a name of hundreds of characters, fuzz_chart); what fits is
  // as Text makes it.
  Label Fit(const std::wstring& s, float size, float width, bool bold = false, const std::wstring& font = {}) const {
    Label l = Text(s, size, bold, font);
    if (l.width <= width || s.size() <= 1) return l;
    const std::wstring_view cut = std::wstring_view(s).substr(0, 255);  // what Text outlines
    const size_t k = Fitting(cut, l.width, width, L"…", size, bold, font);
    return Text(std::wstring(cut.substr(0, k)) + L"…", size, bold, font);
  }
  // How many of the first characters of `s` (at least one, never half a surrogate pair) fit in
  // `width` with `tail` after them, `whole` being the width of all of `s`: a guess in proportion,
  // then fewer until they fit (text outlines are slow, a search through all of them slower).
  size_t Fitting(std::wstring_view s, float whole, float width, std::wstring_view tail, float size, bool bold,
                 const std::wstring& font) const {
    if (s.size() <= 1) return s.size();
    const float guess = whole > 0 ? s.size() * std::max(0.0f, width) / whole : 1;
    size_t k = static_cast<size_t>(std::clamp(guess, 1.0f, static_cast<float>(s.size() - 1)));
    while (k > 1 && Text(std::wstring(s.substr(0, k)) + std::wstring(tail), size, bold, font).width > width)
      k -= std::max<size_t>(1, k / 8);
    if (s[k] >= 0xDC00 && s[k] < 0xE000) k = k > 1 ? k - 1 : k + 1;
    return k;
  }
  // A label's width only, from its glyphs' advances (no outline: Text makes it when it's drawn).
  Label Measure(const std::wstring& s, float size) const {
    Label l;
    if (!s.empty() && size > 0) l.width = draw::TextAdvance(s.substr(0, 255), font_, false, false, size);
    return l;
  }
  // `align`: 0 starts at x, 1 centers on it, 2 ends at it; turned `degrees` about (x, baseline).
  void Place(const Label& l, float x, float baseline, int align, Color c, float degrees = 0) {
    const float dx = align == 1 ? -l.width / 2 : align == 2 ? -l.width : 0;
    Matrix t = Matrix::Translate(dx, 0);
    if (degrees != 0) t = t * Matrix::Rotate(degrees);
    Add(l.path, c, std::nullopt, 0, t * Matrix::Translate(x, baseline));
  }
  draw::Art Take() { return std::move(art_); }

 private:
  static bool Identity(const Matrix& t) { return t.m11 == 1 && t.m12 == 0 && t.m21 == 0 && t.m22 == 1 && t.dx == 0 && t.dy == 0; }
  static void Append(Path& to, const Path& p) {
    for (const Path::Segment& s : p.Segments()) {
      switch (s.kind) {
        case Path::Kind::Move: to.MoveTo(s.p[0]); break;
        case Path::Kind::Line: to.LineTo(s.p[0]); break;
        case Path::Kind::Quad: to.QuadTo(s.p[0], s.p[1]); break;
        case Path::Kind::Cubic: to.CubicTo(s.p[0], s.p[1], s.p[2]); break;
        case Path::Kind::Arc: to.ArcTo(s.p[0], s.rx, s.ry, s.rotation, s.largeArc, s.clockwise); break;
        case Path::Kind::Close: to.Close(); break;
      }
    }
  }

  draw::Art art_;
  std::wstring font_;
};

// The default color of series (or point) `i`: the accents in order, then darker and lighter.
Color SeriesColor(size_t i, const draw::ColorContext& colors) {
  static constexpr std::string_view kAccents[] = {"accent1", "accent2", "accent3", "accent4", "accent5", "accent6"};
  static constexpr uint32_t kFallback[] = {0x4472C4, 0xED7D31, 0xA5A5A5, 0xFFC000, 0x5B9BD5, 0x70AD47};
  Color c = draw::SchemeColor(kAccents[i % 6], colors).value_or(draw::Rgb(kFallback[i % 6]));
  switch ((i / 6) % 4) {
    case 1: draw::ApplyColorTransform(c, "lumMod", 60000); break;
    case 2:
      draw::ApplyColorTransform(c, "lumMod", 80000);
      draw::ApplyColorTransform(c, "lumOff", 20000);
      break;
    case 3: draw::ApplyColorTransform(c, "lumMod", 80000); break;
  }
  return c;
}

// A pie's or doughnut's slice: Excel and Word shade the accent (76 %, their PDFs); PowerPoint
// doesn't (`plain`).
Color SliceColor(size_t i, const draw::ColorContext& colors, bool plain) {
  Color c = SeriesColor(i, colors);
  if (!plain) draw::ApplyColorTransform(c, "shade", 76000);
  return c;
}

const Look* PointLook(const Series& s, size_t i) {
  for (const auto& [idx, look] : s.points)
    if (idx == i) return &look;
  return nullptr;
}

// ---- Markers

// A series' marker: "none", its own symbol, or Office's automatic one for its place in the chart.
std::string MarkerSymbol(const Series& s, bool on, size_t index) {
  if (!s.marker.value_or(on)) return "none";
  static const char* kAuto[] = {"diamond", "square", "triangle", "x", "star", "circle", "plus", "dash", "dot"};
  if (s.symbol.empty() || s.symbol == "auto") return kAuto[index % 9];
  return s.symbol;
}

// A series' markers' fill and outline: their own c:spPr's, else the series' fill and `line`.
struct MarkerLook {
  std::optional<Color> fill, line;
  float width = 0.75f;
};
MarkerLook MarkerLookOf(const Series& s, Color color, Color line) {
  MarkerLook m;
  if (!s.markerLook.noFill) m.fill = s.markerLook.fill.value_or(s.look.fill.value_or(color));
  if (!s.markerLook.noLine) m.line = s.markerLook.line.value_or(line);
  if (s.markerLook.lineWidth >= 0) m.width = s.markerLook.lineWidth;
  return m;
}

void DrawMarker(Builder& b, const std::string& symbol, Point c, float size, const MarkerLook& look) {
  const std::optional<Color> fill = look.fill, line = look.line;
  if (symbol == "none" || symbol.empty()) return;
  const float h = size / 2;
  Path p;
  if (symbol == "x" || symbol == "star" || symbol == "plus" || symbol == "dash" || symbol == "dot") {
    if (symbol == "x" || symbol == "star") {
      p.MoveTo({c.x - h, c.y - h});
      p.LineTo({c.x + h, c.y + h});
      p.MoveTo({c.x - h, c.y + h});
      p.LineTo({c.x + h, c.y - h});
    }
    if (symbol == "plus" || symbol == "star") {
      p.MoveTo({c.x, c.y - h});
      p.LineTo({c.x, c.y + h});
    }
    if (symbol != "x") {
      const float w = symbol == "dot" ? h / 2 : h;
      p.MoveTo({c.x - w, c.y});
      p.LineTo({c.x + w, c.y});
    }
    b.Add(std::move(p), std::nullopt, line ? line : fill, symbol == "dash" || symbol == "dot" ? std::max(look.width, size / 4) : look.width);
    return;
  }
  if (symbol == "square") {
    p = Path::FromRect({c.x - h, c.y - h, c.x + h, c.y + h});
  } else if (symbol == "diamond") {
    p.MoveTo({c.x, c.y - h});
    p.LineTo({c.x + h, c.y});
    p.LineTo({c.x, c.y + h});
    p.LineTo({c.x - h, c.y});
    p.Close();
  } else if (symbol == "triangle") {
    p.MoveTo({c.x, c.y - h});
    p.LineTo({c.x + h, c.y + h});
    p.LineTo({c.x - h, c.y + h});
    p.Close();
  } else {
    p = Path::Ellipse({c.x - h, c.y - h, c.x + h, c.y + h});  // circle, picture, anything else
  }
  b.Add(std::move(p), fill, line, look.width);
}

// ---- Scales

struct Scale {
  double min = 0, max = 1, step = 0.2;
  double base = 0;  // logarithmic (c:logBase): ticks at powers of it, `step` unused; 0: linear
  double At(double v) const {
    if (base > 0) return v > 0 ? (std::log(v) - std::log(min)) / (std::log(max) - std::log(min)) : 0;
    return (v - min) / (max - min);
  }
};

double Up(double v, double step) { return std::ceil(v / step - 1e-9); }
double Down(double v, double step) { return std::floor(v / step + 1e-9); }

// The smallest major unit of 1, 2 or 5 times a power of ten that needs at most `most` of them (ten,
// fewer where the plot is too short for ten labels).
double NiceStep(double lo, double hi, int most) {
  double mag = std::pow(10.0, std::floor(std::log10((hi - lo) / 10)));
  for (int i = 0; i < 12 && mag > 0 && std::isfinite(mag); ++i, mag *= 10)
    for (double f : {1.0, 2.0, 5.0})
      if (Up(hi, f * mag) - Down(lo, f * mag) <= most) return f * mag;
  return (hi - lo) / 5;
}

// Excel's automatic scale: from zero unless the values sit in a narrow band far from it, 5% of the
// axis's range (from zero, when it starts there) past the extremes that aren't zero, round major
// units; the axis's own minimum and maximum win. (Excel's PDFs: a scatter of 55..86 up to 100, of
// 160..190 from 155; columns of 95..175 up to 200.)
Scale MakeScale(double lo, double hi, const Axis& axis, bool zero, bool pad, int most = 10) {
  if (axis.logBase >= 2) {
    // From the power of the base at or below the smallest value to the one at or above the
    // largest; the axis's own minimum and maximum win (15-213's 0.01 to 10^8 by tens).
    Scale s;
    s.base = axis.logBase;
    const double b = std::log(s.base);
    const double low = std::isfinite(lo) && lo > 0 ? lo : 1, high = std::isfinite(hi) && hi > 0 ? hi : low * s.base;
    s.min = axis.min && *axis.min > 0 ? *axis.min : std::pow(s.base, std::floor(std::log(low) / b + 1e-9));
    s.max = axis.max && *axis.max > s.min ? *axis.max : std::pow(s.base, std::ceil(std::log(std::max(high, s.min)) / b - 1e-9));
    if (!(s.max > s.min)) s.max = s.min * s.base;
    return s;
  }
  if (!std::isfinite(lo) || !std::isfinite(hi) || lo > hi) lo = 0, hi = 1;
  const bool narrow = hi > lo;  // a band of values: its far side keeps its room, a single value doesn't
  if (lo > 0 && (zero || hi - lo >= hi / 6)) lo = 0;
  if (hi < 0 && (zero || hi - lo >= -lo / 6)) hi = 0;
  if (hi == lo) hi = lo + (lo == 0 ? 1 : std::fabs(lo) / 2);
  if (pad) {
    const double room = (hi - lo) * 0.05;
    if (hi > 0 || (hi < 0 && narrow)) hi += room;
    if (lo < 0 || (lo > 0 && narrow)) lo -= room;
  }
  if (axis.min) lo = *axis.min;
  if (axis.max) hi = *axis.max;
  if (!(hi > lo) || !std::isfinite(hi - lo)) hi = lo + 1;
  Scale s;
  s.step = NiceStep(lo, hi, std::clamp(most, 1, 10));
  if (!(s.step > 0) || !std::isfinite(s.step)) s.step = (hi - lo) / 5;
  s.min = axis.min ? lo : Down(lo, s.step) * s.step;
  s.max = axis.max ? hi : Up(hi, s.step) * s.step;
  if (!(s.max > s.min)) s.max = s.min + s.step;
  if ((s.max - s.min) / s.step > 60) s.step = (s.max - s.min) / 10;
  return s;
}

std::vector<double> Ticks(const Scale& s) {
  std::vector<double> out;
  if (s.base > 0) {
    for (double v = s.min; v <= s.max * (1 + 1e-9) && out.size() <= 60; v *= s.base) out.push_back(v);
    return out;
  }
  for (int i = 0; i <= 60; ++i) {
    double v = s.min + i * s.step;
    if (v > s.max + s.step * 1e-6) break;
    if (std::fabs(v) < s.step * 1e-9) v = 0;  // not -1.4e-17
    out.push_back(v);
  }
  return out;
}

// ---- Legend

struct Entry {
  std::wstring name;
  Color color;          // the key's fill, or its line's color
  bool line = false;    // a line or scatter series: a stroke for its key
  bool stroke = true;   // with `line`: the stroke drawn (a markers-only scatter series has just its marker)
  float width = 2.25f;  // the line's
  std::vector<float> dashes;    // and its dashes
  std::string symbol = "none";  // its markers'
  float markerSize = 5;
  MarkerLook marker;  // its markers' fill and outline
  bool circle = false;  // a bubble's key: a circle
};

// A legend entry the chart leaves out (c:legendEntry/c:delete).
bool Hidden(const Chart& ch, size_t index) {
  return std::find(ch.hiddenEntries.begin(), ch.hiddenEntries.end(), index) != ch.hiddenEntries.end();
}

std::vector<Entry> Entries(const Chart& ch, const draw::ColorContext& colors) {
  std::vector<Entry> out;
  const Group& g0 = ch.groups.front();
  const bool pie = g0.type == Type::Pie || g0.type == Type::Doughnut;
  if ((pie || (g0.varyColors && g0.series.size() == 1)) && !g0.series.empty()) {
    const Series& s = g0.series.front();
    const size_t n = std::max(s.categories.size(), s.values.size());
    for (size_t i = 0; i < n && i < 64; ++i) {
      const Look* look = PointLook(s, i);
      Color c = look && look->fill ? *look->fill : pie ? SliceColor(i, colors, ch.presentation) : SeriesColor(i, colors);
      Entry e;
      e.name = i < s.categories.size() ? s.categories[i] : std::to_wstring(i + 1);
      e.color = c;
      if (!Hidden(ch, i)) out.push_back(std::move(e));
    }
    return out;
  }
  size_t k = 0;
  for (const Group& g : ch.groups)
    for (const Series& s : g.series) {
      const bool scatter = g.type == Type::Scatter;
      // A scatter series' key is a line's length, its marker alone when it has no line (PowerPoint's PDFs).
      const bool line = g.type == Type::Line || (g.type == Type::Radar && !g.filled) || scatter;
      Entry e;
      e.name = s.name.empty() ? L"Series " + std::to_wstring(k + 1) : s.name;
      e.color = SeriesColor(k, colors);
      if (line && s.look.line) e.color = *s.look.line;
      else if (!line && s.look.fill) e.color = *s.look.fill;
      e.line = line;
      e.stroke = !scatter || (!s.look.noLine && g.lines);
      e.circle = g.type == Type::Bubble;
      if (line) {
        e.width = s.look.lineWidth >= 0 ? s.look.lineWidth : scatter && !ch.presentation ? 1.5f : 2.25f;
        e.dashes = s.look.dashes;
        e.marker = MarkerLookOf(s, SeriesColor(k, colors), e.color);
        e.symbol = MarkerSymbol(s, scatter || g.marker, k);
        e.markerSize = s.markerSize;
      }
      if (!Hidden(ch, k)) out.push_back(std::move(e));
      if (++k >= 64) break;
    }
  // Horizontal bars list their series bottom up, as they're stacked in each category; so do
  // stacked columns, lines and areas in a legend at the side, top down as they stack (Word).
  const bool side = ch.legend == LegendPos::Right || ch.legend == LegendPos::Left;
  if (ch.groups.size() == 1 && (g0.type == Type::Bar || (side && g0.grouping != Grouping::Clustered)))
    std::reverse(out.begin(), out.end());
  return out;
}

// Text on lines as Excel wraps a title or legend entry wider than `width`: at spaces, and within a
// word too wide for a line of its own; at most `most` lines (the rest isn't shown). Text that fits
// stays on one line as it is.
std::vector<Label> WrapLines(const Builder& b, std::wstring_view s, float size, bool bold, const std::wstring& font,
                             float width, size_t most) {
  s = s.substr(0, 255);
  const auto text = [&](std::wstring_view t) { return b.Text(std::wstring(t), size, bold, font); };
  if (Label all = text(s); all.width <= width) return {std::move(all)};
  std::vector<Label> lines;
  std::wstring line;
  const auto flush = [&] {
    if (!line.empty() && lines.size() < most) lines.push_back(text(line));
    line.clear();
  };
  for (size_t at = 0; at < s.size() && lines.size() < most;) {
    size_t end = s.find(L' ', at);
    if (end == std::wstring_view::npos) end = s.size();
    std::wstring word(s.substr(at, end - at));
    at = end + 1;
    if (word.empty()) continue;
    if (!line.empty()) {
      if (text(line + L' ' + word).width <= width) {
        line += L' ' + word;
        continue;
      }
      flush();
    }
    // Alone on its line: a word too wide even so is broken where it stops fitting (whole
    // characters, at least one a line).
    while (lines.size() < most && word.size() > 1) {
      const float whole = text(word).width;
      if (whole <= width) break;
      const size_t k = b.Fitting(word, whole, width, {}, size, bold, font);
      lines.push_back(text(std::wstring_view(word).substr(0, k)));
      word.erase(0, k);
    }
    line = std::move(word);
  }
  flush();
  return lines;
}

// The legend at its side of `r` (Excel's PDFs): entries a line and 6 points apart down a side, or in
// centered rows, each key before its name. Returns the room left for the plot: all of `r` when the
// legend overlays it.
// `placed`: in a box of its own (c:manualLayout), `r`.
Rect DrawLegend(Builder& b, LegendPos pos, bool overlay, const std::vector<Entry>& entries, Rect r, float ts, Color text,
                bool placed = false) {
  std::vector<Label> labels;
  float widest = 0;
  bool lines = false;
  for (const Entry& e : entries) {
    labels.push_back(b.Text(e.name, ts));
    widest = std::max(widest, labels.back().width);
    lines |= e.line;
  }
  // With a line among them, every key is a line's length: 1.92 text sizes, at most 19.2 points, and
  // the gap after it at most 2 (Excel's PDFs at 10 points, PowerPoint's at 18).
  const float box = kKey * ts * b.tall, keyW = lines ? std::min(1.92f * ts, 19.2f) : box,
              lead = keyW + (lines ? std::min(0.212f * ts, 2.0f) : 0.245f * ts * b.tall);
  const auto key = [&](const Entry& e, float x, float cy) {
    if (e.line) {
      if (e.stroke) {
        Path p;
        p.MoveTo({x, cy});
        p.LineTo({x + keyW, cy});
        b.Stroke(std::move(p), e.color, e.width, e.dashes);
      }
      DrawMarker(b, e.symbol, {x + keyW / 2, cy}, e.markerSize, e.marker);
    } else if (e.circle) {
      b.Add(Path::Ellipse({x, cy - box / 2, x + keyW, cy + box / 2}), e.color, std::nullopt, 0);
    } else {
      b.Add(Path::FromRect({x, cy - box / 2, x + keyW, cy + box / 2}), e.color, std::nullopt, 0);
    }
  };
  constexpr float kInset = 3.55f;  // points: the entries inside the room's edge
  const float beyond = std::max(0.0f, ts - 10);  // the text size past 10 points
  // An entry wider than `width` wraps as Excel wraps it, to at most as many lines as `room` holds;
  // its lines `lp` apart. `names[i]`: entry i's lines, `wide[i]` the widest.
  const float lp = kLine * ts;
  std::vector<std::vector<Label>> names(entries.size());
  std::vector<float> wide(entries.size());
  const auto wrap = [&](float width, float room) {
    const size_t most = room >= lp ? static_cast<size_t>(std::min(room / lp, 1000.0f)) : 1;
    for (size_t i = 0; i < entries.size(); ++i) {
      if (labels[i].width <= width) names[i] = {labels[i]};
      else names[i] = WrapLines(b, entries[i].name, ts, false, {}, std::max(1.0f, width), most);
      if (names[i].empty()) names[i].emplace_back();
      wide[i] = 0;
      for (const Label& l : names[i]) wide[i] = std::max(wide[i], l.width);
    }
  };
  if ((pos == LegendPos::Right || pos == LegendPos::Left) && placed) {
    // In a box of its own: the keys 7.13 points in from its left, the entries spread evenly down it,
    // each centred in its share, as wide as the box allows (PowerPoint's PDFs, 15-213's memory gap:
    // six 12-point entries in a 91-point box). Too many for the box: a line apart, as many as fit.
    const float x = r.left + 7.13f;
    wrap(std::max(1.0f, r.right - kInset - x - lead), r.bottom - r.top);
    size_t total = 0;
    for (const auto& n : names) total += n.size();
    const float share = (r.bottom - r.top) / static_cast<float>(std::max<size_t>(1, total)), step = std::max(share, 0.8f * lp);
    float top = r.top;
    for (size_t i = 0; i < entries.size(); ++i) {
      const float h = names[i].size() * step;
      if (i > 0 && top + h > r.bottom + 0.01f) break;
      const float cy = top + h / 2;
      key(entries[i], x, cy);
      for (size_t k = 0; k < names[i].size(); ++k)
        b.Place(names[i][k], x + lead, cy + (k - (names[i].size() - 1) / 2.0f) * lp + kMiddle * ts * b.tall, 0, text);
      top += h;
    }
    return r;
  }
  if (pos == LegendPos::Right || pos == LegendPos::Left) {
    const float w = std::min(lead + widest, (r.right - r.left) * 0.45f);
    wrap(w - lead, r.bottom - r.top);
    // At the side, a little further in past 10-point text (PowerPoint's PDFs: 5.2 points at 18).
    const float inset = kInset + 0.2f * beyond;
    const float x = pos == LegendPos::Right ? r.right - inset - w : r.left + inset;
    const float pitch = 1.21f * ts + 6, mid = (r.top + r.bottom) / 2;
    // Each line past an entry's first moves the entries after it down a line, all centered.
    float extra = 0, before = 0;
    for (const auto& n : names) extra += (n.size() - 1) * lp;
    for (size_t i = 0; i < entries.size(); ++i) {
      const float own = (names[i].size() - 1) * lp;
      const float cy = mid + (static_cast<float>(i) - (entries.size() - 1) / 2.0f) * pitch - extra / 2 + before + own / 2;
      before += own;
      if (cy < r.top || cy > r.bottom) continue;
      key(entries[i], x, cy);
      for (size_t k = 0; k < names[i].size(); ++k) b.Place(names[i][k], x + lead, cy - own / 2 + k * lp + kMiddle * ts * b.tall, 0, text);
    }
    if (!overlay) (pos == LegendPos::Right ? r.right : r.left) = pos == LegendPos::Right ? x - 11.2f : x + w + 11.2f;
    return r;
  }
  // Top or bottom: rows of entries, centered; an entry at most about 90 % of the chart's width
  // (Excel), a row as tall as its tallest entry.
  // Entries in a row 0.976 text sizes apart (from the ink), less past 10 points (PowerPoint's PDFs:
  // 15.8 points with 18-point text).
  const float gap = (0.976f * std::min(ts, 10.0f) + 0.75f * std::max(0.0f, ts - 10)) * b.tall;
  const float avail = r.right - r.left;
  wrap(0.9f * (avail + 2 * kPad) - lead, (r.bottom - r.top) / 2);
  std::vector<std::vector<size_t>> rows(1);
  std::vector<float> extra(1);
  float rowW = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    const float w = lead + wide[i] + gap;
    if (!rows.back().empty() && rowW + w - gap > avail) {
      if (rows.size() == 4) break;
      rows.emplace_back();
      extra.push_back(0);
      rowW = 0;
    }
    rows.back().push_back(i);
    extra.back() = std::max(extra.back(), (names[i].size() - 1) * lp);
    rowW += w;
  }
  const float pitch = kLine * ts + 2;
  // Rows that don't fit in the room aren't shown (the first always is).
  float extras = 0;
  for (float e : extra) extras += e;
  while (rows.size() > 1 && kInset + (rows.size() - 1) * pitch + extras + ts > r.bottom - r.top) {
    extras -= extra.back();
    rows.pop_back();
    extra.pop_back();
  }
  float baseline = pos == LegendPos::Bottom ? r.bottom - kInset - kDescent * ts - (rows.size() - 1) * pitch - extras
                                            : r.top + kInset + kAscent * ts;
  const float first = baseline;
  for (size_t k = 0; k < rows.size(); ++k) {
    float total = -gap;
    for (size_t i : rows[k]) total += lead + wide[i] + gap;
    // Centred, a little right of the middle past 10-point text (PowerPoint's PDFs: 3 points with 18).
    float x = (r.left + r.right) / 2 - total / 2 + 0.38f * beyond;
    for (size_t i : rows[k]) {
      key(entries[i], x, baseline - kMiddle * ts * b.tall + (names[i].size() - 1) * lp / 2);
      for (size_t j = 0; j < names[i].size(); ++j) b.Place(names[i][j], x + lead, baseline + j * lp, 0, text);
      x += lead + wide[i] + gap;
    }
    baseline += pitch + extra[k];
  }
  if (!overlay) {
    // The gap to the plot's room grows with the text past 12 points (PowerPoint's PDFs: 12.6
    // points with 18-point text).
    const float room = std::max(kTitleGap, 0.7f * ts);
    if (pos == LegendPos::Bottom) r.bottom = first - kAscent * ts - room;
    else r.top = baseline - pitch + kDescent * ts + room;
  }
  return r;
}

// ---- Data labels, drawn last so marks don't cover them

struct Pending {
  std::wstring text;  // lines apart at '\n'
  Point at;           // x as `align` says; y the label's middle
  int align;          // 0 starts at x, 1 centers on it, 2 ends at it
  std::optional<Color> key;  // a legend key before the text
  std::optional<Color> fill, color;  // a point's own label (c:dLbl): its box's fill, its text's color
  float size = -1;                   // and its text size (negative: the chart's)
  // A point's own label is anchored: `at` is its point, `side` where it goes beside it ('r', 'l',
  // 't', 'b' or 'c'), `offset` how far it's moved from there; its box is its text's with `insets`
  // (negative: Office's 3 and 1.5 points), or `boxW` by `boxH` with its text wrapped in it.
  bool anchored = false;
  char side = 'r';
  Point offset{};
  float insets[4] = {-1, -1, -1, -1};
  float boxW = 0, boxH = 0;
};

// What a data label says (Office's order: series, category, value, percentage), each part followed
// by the separator but the last; empty when it shows nothing.
std::vector<std::wstring> LabelParts(const Group& g, const Series& s, size_t i, const std::wstring& value,
                                     const std::wstring& percent) {
  std::vector<std::wstring> parts;
  if (s.seriesNames.value_or(g.seriesNames) && !s.name.empty()) parts.push_back(s.name);
  if (s.categoryLabels.value_or(g.categoryLabels) && i < s.categories.size() && !s.categories[i].empty())
    parts.push_back(s.categories[i]);
  if (s.labels.value_or(g.labels) && !value.empty()) parts.push_back(value);
  if (s.percent.value_or(g.percent) && !percent.empty()) parts.push_back(percent);
  const std::wstring sep = s.separator ? *s.separator : g.separator ? *g.separator : std::wstring(1, g_list) + L" ";
  for (size_t k = 0; k + 1 < parts.size(); ++k) parts[k] += sep;
  return parts;
}

std::wstring Joined(const std::vector<std::wstring>& parts) {
  std::wstring out;
  for (const std::wstring& p : parts) out += p;
  return out;
}

bool ShowsKey(const Group& g, const Series& s) { return s.keys.value_or(g.keys); }

void DrawPending(Builder& b, const std::vector<Pending>& labels, float chartSize, Color chartText, float W, float H) {
  for (const Pending& p : labels) {
    const float ts = p.size > 0 ? p.size : chartSize;
    const Color text = p.color.value_or(chartText);
    const float box = kKey * ts, pitch = kLine * ts;
    // As many lines as the chart is high, each no wider than the chart.
    const size_t most = static_cast<size_t>(std::clamp((H - 5.7f) / pitch, 1.0f, 16.0f));
    std::vector<Label> lines;
    float wide = 0;
    const float room = std::max(1.0f, W - 5.7f - (p.key ? box + 0.506f * ts : 0));
    for (size_t at = 0; at <= p.text.size() && lines.size() < most;) {
      size_t end = p.text.find(L'\n', at);
      if (end == std::wstring::npos) end = p.text.size();
      const float il = p.insets[0] >= 0 ? p.insets[0] : 3, ir = p.insets[2] >= 0 ? p.insets[2] : 3;
      if (p.anchored && p.boxW > 0) {
        for (Label& l : WrapLines(b, std::wstring_view(p.text).substr(at, end - at), ts, false, {}, std::max(1.0f, p.boxW - il - ir), most))
          if (lines.size() < most) lines.push_back(std::move(l));
      } else {
        lines.push_back(b.Fit(p.text.substr(at, end - at), ts, room));
      }
      for (const Label& l : lines) wide = std::max(wide, l.width);
      at = end + 1;
    }
    if (p.anchored) {
      // A point's own label (PowerPoint's PDFs, govuk business bodies' value boxes and panel names):
      // its box first beside the point, 5.5 points off it, but inside the chart, then moved by its
      // offset wherever that takes it; its lines centred in the box.
      const float il = p.insets[0] >= 0 ? p.insets[0] : 3, it = p.insets[1] >= 0 ? p.insets[1] : 1.5f;
      const float ir = p.insets[2] >= 0 ? p.insets[2] : 3, ib = p.insets[3] >= 0 ? p.insets[3] : 1.5f;
      const float w = p.boxW > 0 ? p.boxW : wide + il + ir, h = p.boxH > 0 ? p.boxH : lines.size() * pitch + it + ib;
      constexpr float kOff = 5.5f;
      float left = p.side == 'r' ? p.at.x + kOff : p.side == 'l' ? p.at.x - kOff - w : p.at.x - w / 2;
      float top = p.side == 't' ? p.at.y - kOff - h : p.side == 'b' ? p.at.y + kOff : p.at.y - h / 2;
      left = std::max(0.0f, std::min(left, W - w)) + p.offset.x;
      top = std::max(0.0f, std::min(top, H - h)) + p.offset.y;
      if (p.fill) b.Add(Path::FromRect({left, top, left + w, top + h}), *p.fill, std::nullopt, 0);
      const float x = left + il + (w - il - ir) / 2, firstMiddle = top + it + (h - it - ib) / 2 - (lines.size() - 1) * pitch / 2;
      for (size_t k = 0; k < lines.size(); ++k) b.Place(lines[k], x, firstMiddle + k * pitch + kLabelMiddle * ts, 1, text);
      continue;
    }
    const float lead = p.key ? box + 0.506f * ts : 0, total = lead + wide;
    float left = p.align == 1 ? p.at.x - total / 2 : p.align == 2 ? p.at.x - total : p.at.x;
    // Kept inside the chart, as Office does.
    left = std::max(2.85f, std::min(left, W - 2.85f - total));
    const float half = lines.size() * pitch / 2;
    const float middle = std::max(2.85f + half, std::min(p.at.y, H - 2.85f - half));
    const float firstMiddle = middle - (lines.size() - 1) * pitch / 2;
    // A filled box fits the text, a little room at its sides (govuk business bodies' value boxes).
    if (p.fill) b.Add(Path::FromRect({left - 0.15f * ts, middle - half, left + total + 0.15f * ts, middle + half}), *p.fill, std::nullopt, 0);
    if (p.key) b.Add(Path::FromRect({left, firstMiddle - box / 2, left + box, firstMiddle + box / 2}), *p.key, std::nullopt, 0);
    for (size_t k = 0; k < lines.size(); ++k)
      b.Place(lines[k], left + lead + (wide - lines[k].width) / 2, firstMiddle + k * pitch + kLabelMiddle * ts, 0, text);
  }
}

// ---- Pie and doughnut

Path Slice(Point c, float R, float r, double a0, double a1) {
  const auto at = [&](double deg, float rad) {
    const double t = deg * kPi / 180;
    return Point{static_cast<float>(c.x + rad * std::cos(t)), static_cast<float>(c.y + rad * std::sin(t))};
  };
  Path p;
  if (a1 - a0 >= 359.99) {
    for (float rad : {R, r}) {
      if (rad <= 0) continue;
      p.MoveTo(at(0, rad));
      p.ArcTo(at(180, rad), rad, rad, 0, false, rad == R);
      p.ArcTo(at(360, rad), rad, rad, 0, false, rad == R);
      p.Close();
    }
    p.fillRule = Path::FillRule::EvenOdd;
    return p;
  }
  const bool large = a1 - a0 > 180;
  if (r <= 0) {
    p.MoveTo(c);
    p.LineTo(at(a0, R));
    p.ArcTo(at(a1, R), R, R, 0, large, true);
  } else {
    p.MoveTo(at(a0, R));
    p.ArcTo(at(a1, R), R, R, 0, large, true);
    p.LineTo(at(a1, r));
    p.ArcTo(at(a0, r), r, r, 0, large, false);
  }
  p.Close();
  return p;
}

// Parts on lines no wider than `width` (each line at least one part).
std::wstring Wrapped(Builder& b, const std::vector<std::wstring>& parts, float width, float ts) {
  std::wstring out, line;
  for (const std::wstring& part : parts) {
    if (!line.empty() && b.Text(line + part, ts).width > width) {
      out += line + L"\n";
      line.clear();
    }
    line += part;
  }
  return out + line;
}

// A pie (its first series) or a doughnut (a ring a series, the first innermost), in the room `r`;
// `plain`: slices in the accents themselves (SliceColor).
void DrawPie(Builder& b, const Group& g, Rect r, float ts, Color text, const draw::ColorContext& colors, float W, float H,
             bool plain, std::optional<Rect> placed = std::nullopt) {
  const float m = std::min(kMargin * ts, kMargin * 10);  // as DrawAxes' sides (PowerPoint's PDFs)
  r = {r.left + m, r.top + m, r.right - m, r.bottom - m};
  // A plot area the chart places (c:manualLayout): the pie is the largest circle in its box, centred
  // (PowerPoint's PDFs, govuk business bodies' doughnuts).
  if (placed) r = *placed;
  const bool doughnut = g.type == Type::Doughnut;
  const size_t rings = doughnut ? std::min<size_t>(g.series.size(), 16) : 1;
  bool labelled = g.labels || g.percent || g.categoryLabels || g.seriesNames;
  for (const Series& s : g.series)
    labelled |= s.labels.value_or(false) || s.percent.value_or(false) || s.categoryLabels.value_or(false) || s.seriesNames.value_or(false);
  // Labels round a pie take some of its room; so do slices pulled out of it (c:explosion, a part
  // of the radius), the pie shrunk to keep the farthest inside.
  double pulled = 0;
  if (!doughnut && !g.series.empty()) {
    pulled = g.series.front().explosion;
    for (const auto& [i, e] : g.series.front().explosions) pulled = std::max(pulled, e);
  }
  const float R = static_cast<float>((std::min(r.right - r.left, r.bottom - r.top) / 2 - (labelled && !doughnut && !placed ? 1.32f * ts : 0)) /
                                     (1 + pulled / 100));
  if (R < 2) return;
  const Point center{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
  const float hole = doughnut ? static_cast<float>(R * g.hole / 100) : 0, ring = (R - hole) / rings;
  std::vector<Pending> labels;
  for (size_t j = 0; j < rings; ++j) {
    const Series& s = g.series[j];
    double total = 0;
    for (double v : s.values)
      if (v > 0 && std::isfinite(v)) total += v;
    if (!(total > 0) || !std::isfinite(total)) continue;
    const float inner = doughnut ? hole + j * ring : 0, outer = doughnut ? inner + ring : R;
    const bool key = ShowsKey(g, s);
    const std::string format = !s.labelFormat.empty() ? s.labelFormat : !g.labelFormat.empty() ? g.labelFormat : "General";
    const std::string& pos = !s.labelPos.empty() ? s.labelPos : g.labelPos;
    double a = g.firstAngle - 90;
    for (size_t i = 0; i < s.values.size() && labels.size() < 4096; ++i) {
      const double v = s.values[i];
      if (!(v > 0) || !std::isfinite(v)) continue;
      const double sweep = v / total * 360;
      const Look* look = PointLook(s, i);
      const Color fill = g.varyColors ? SliceColor(i, colors, plain) : s.look.fill.value_or(SliceColor(j, colors, plain));
      const double mid = (a + sweep / 2) * kPi / 180;
      const Point dir{static_cast<float>(std::cos(mid)), static_cast<float>(std::sin(mid))};
      double out = doughnut ? 0 : s.explosion;
      if (!doughnut)
        for (const auto& [k, e] : s.explosions)
          if (k == i) out = e;
      const float shift = static_cast<float>(R * out / 100);
      const Point c{center.x + shift * dir.x, center.y + shift * dir.y};
      b.Mark(Slice(c, outer, inner, a, a + sweep), look ? *look : s.look, fill, std::nullopt, 0.75f);
      std::vector<std::wstring> parts = LabelParts(g, s, i, FormatNumber(v, format), FormatNumber(v / total, "0%"));
      if (!parts.empty()) {
        const std::optional<Color> k = key ? std::optional<Color>((look && look->fill) ? *look->fill : fill) : std::nullopt;
        if (doughnut) {
          const float rad = (inner + outer) / 2;
          labels.push_back({Joined(parts), {c.x + rad * dir.x, c.y + rad * dir.y}, 1, k});
        } else {
          // Best fit (Office's default): inside the slice when it's wide enough, wrapped narrow;
          // else outside it, wider.
          const auto measure = [&](const std::wstring& t, float& w, float& h) {
            size_t n = 1;
            w = 0;
            for (size_t at = 0; at <= t.size();) {
              size_t end = t.find(L'\n', at);
              if (end == std::wstring::npos) end = t.size();
              w = std::max(w, b.Text(t.substr(at, end - at), ts).width);
              n += end < t.size();
              at = end + 1;
            }
            if (k) w += (kKey + 0.506f) * ts;
            h = n * kLine * ts;
          };
          const bool wrap = s.values.size() <= 64;
          std::wstring t = wrap ? Wrapped(b, parts, 0.45f * R, ts) : Joined(parts);
          float w, h;
          measure(t, w, h);
          const float fits = static_cast<float>(2 * 0.65 * R * std::sin(std::min(sweep, 180.0) / 2 * kPi / 180));
          Point at;
          if (pos == "ctr" || pos == "inEnd") {
            // In the middle of the slice, or inside its edge (Office's).
            const float rad = pos == "ctr" ? 0.5f * R : R - 0.2f * ts - std::max(std::fabs(dir.x) * w, std::fabs(dir.y) * h) / 2;
            at = {c.x + rad * dir.x, c.y + rad * dir.y};
          } else if (pos != "outEnd" && w <= fits) {
            at = {c.x + 0.62f * R * dir.x, c.y + 0.62f * R * dir.y};
          } else {
            if (wrap) t = Wrapped(b, parts, 0.8f * R, ts), measure(t, w, h);
            const Point edge{c.x + (R + 0.2f * ts) * dir.x, c.y + (R + 0.2f * ts) * dir.y};
            at = {edge.x + dir.x * w / 2, edge.y + dir.y * h / 2};
          }
          labels.push_back({t, at, 1, k});
        }
      }
      a += sweep;
    }
  }
  DrawPending(b, labels, ts, text, W, H);
}

// ---- Axes and the groups on them

struct Plot {
  Rect r;
  bool bar = false;      // categories up the left side, values across
  bool reversed = false;  // categories in reverse order
  // t along the categories, u along the values, both 0 to 1.
  Point At(double t, double u) const {
    if (reversed) t = 1 - t;
    const double w = r.right - r.left, h = r.bottom - r.top;
    if (!bar) return {static_cast<float>(r.left + t * w), static_cast<float>(r.bottom - u * h)};
    return {static_cast<float>(r.left + u * w), static_cast<float>(r.bottom - t * h)};
  }
};

struct Cats {
  size_t n = 1;
  bool between = true;
  double Center(size_t i) const { return between ? (i + 0.5) / n : n <= 1 ? 0.5 : double(i) / (n - 1); }
  double Band() const { return between ? 1.0 / n : n <= 1 ? 1.0 : 1.0 / (n - 1); }
};

double Clamp01(double u) { return std::isfinite(u) ? std::clamp(u, 0.0, 1.0) : 0; }
double Near01(double u) { return std::isfinite(u) ? std::clamp(u, -0.1, 1.1) : 0; }

// Sums a stacked group's values at category i, up to series `j` (positives and negatives apart).
void Stack(const Group& g, size_t i, size_t j, double& pos, double& neg, double& total) {
  pos = neg = total = 0;
  for (size_t k = 0; k < g.series.size(); ++k) {
    const double v = i < g.series[k].values.size() ? g.series[k].values[i] : 0;
    if (!std::isfinite(v)) continue;
    total += std::fabs(v);
    if (k < j) (v >= 0 ? pos : neg) += v;
  }
}

double Value(const Series& s, size_t i) { return i < s.values.size() ? s.values[i] : std::nan(""); }

// A series' line through each run of points (a missing value breaks it), smoothed as Excel does: a
// Bézier through each pair of points, its handles a sixth of the way along the neighbours' chord.
Path Polyline(const std::vector<std::vector<Point>>& runs, bool smooth) {
  Path path;
  for (const std::vector<Point>& pts : runs) {
    if (pts.empty()) continue;
    path.MoveTo(pts[0]);
    for (size_t i = 1; i < pts.size(); ++i) {
      if (!smooth || pts.size() < 3) {
        path.LineTo(pts[i]);
        continue;
      }
      const Point p0 = pts[i > 1 ? i - 2 : 0], p1 = pts[i - 1], p2 = pts[i], p3 = pts[i + 1 < pts.size() ? i + 1 : i];
      const Point c1{p1.x + (p2.x - p0.x) / 6, p1.y + (p2.y - p0.y) / 6}, c2{p2.x - (p3.x - p1.x) / 6, p2.y - (p3.y - p1.y) / 6};
      constexpr int kSteps = 12;
      for (int k = 1; k <= kSteps; ++k) {
        const float t = float(k) / kSteps, u = 1 - t;
        const float a = u * u * u, bb = 3 * u * u * t, cc = 3 * u * t * t, d = t * t * t;
        path.LineTo({a * p1.x + bb * c1.x + cc * c2.x + d * p2.x, a * p1.y + bb * c1.y + cc * c2.y + d * p2.y});
      }
    }
  }
  return path;
}

bool ShowsLabels(const Group& g, const Series& s) {
  return s.labels.value_or(g.labels) || s.categoryLabels.value_or(g.categoryLabels) || s.seriesNames.value_or(g.seriesNames);
}

void DrawGroup(Builder& b, const Group& g, size_t first, const Plot& p, const Cats& c, const Scale& sc,
               const std::string& format, const draw::ColorContext& colors, std::vector<Pending>& labels, float ts,
               float W, float H) {
  const size_t k = g.series.size();
  const bool stacked = g.grouping != Grouping::Clustered, percent = g.grouping == Grouping::Percent;
  const auto valueAt = [&](size_t j, size_t i, double& base) {
    double v = Value(g.series[j], i);
    base = 0;
    if (!stacked || !std::isfinite(v)) return v;
    double pos, neg, total;
    Stack(g, i, j, pos, neg, total);
    if (percent) {
      if (!(total > 0)) return std::nan("");
      v /= total, pos /= total, neg /= total;
    }
    base = v >= 0 ? pos : neg;
    return base + v;
  };
  const auto text = [&](const Series& s, size_t i) {
    const std::string& own = !s.labelFormat.empty() ? s.labelFormat : g.labelFormat;
    return Joined(LabelParts(g, s, i, FormatNumber(Value(s, i), percent ? std::string() : own.empty() ? format : own), {}));
  };
  if (g.type == Type::Column || g.type == Type::Bar) {
    const double band = c.Band(), gap = g.gap / 100, ov = g.overlap / 100;
    const double w = stacked ? band / (1 + gap) : band / (k - (k - 1) * ov + gap);
    // The bars a series at a time (a later series over an earlier one where they overlap, as in a
    // category at a time; a series' bars in one color become one mark), then the labels a
    // category at a time.
    bool labelled = false;
    for (const Series& s : g.series) labelled = labelled || ShowsLabels(g, s);
    for (int pass = 0; pass < (labelled ? 2 : 1); ++pass)
      for (size_t o = 0; o < (pass ? c.n : k); ++o)
        for (size_t q = 0; q < (pass ? k : c.n); ++q) {
          const size_t i = pass ? o : q, j = pass ? q : o;
          double base;
          const double top = valueAt(j, i, base);
          if (!std::isfinite(top)) continue;
          const double t0 = stacked ? c.Center(i) - w / 2 : c.Center(i) - (k * w - (k - 1) * ov * w) / 2 + j * w * (1 - ov);
          const Point a = p.At(t0, Clamp01(sc.At(base))), z = p.At(t0 + w, Clamp01(sc.At(top)));
          const Rect rect{std::min(a.x, z.x), std::min(a.y, z.y), std::max(a.x, z.x), std::max(a.y, z.y)};
          const Series& s = g.series[j];
          const Look* look = PointLook(s, i);
          const Color fill = g.varyColors && k == 1 ? SeriesColor(i, colors) : SeriesColor(first + j, colors);
          if (pass == 0) {
            b.Mark(Path::FromRect(rect), look ? *look : s.look, fill, std::nullopt, 0.75f);
            continue;
          }
          if (ShowsLabels(g, s)) {
            const std::optional<Color> key = ShowsKey(g, s) ? std::optional<Color>((look ? *look : s.look).fill.value_or(fill)) : std::nullopt;
            const float off = 0.95f * ts;  // outside the end: Excel's PDFs
            const float cy = (rect.top + rect.bottom) / 2, cx = (rect.left + rect.right) / 2;
            // Above a column, the baseline 3 points over its end with 10-point text, further with
            // larger (PowerPoint's PDFs: 8.9 with 18-point).
            const float above = 3 + 0.74f * std::max(0.0f, ts - 10);
            if (stacked) labels.push_back({text(s, i), {cx, cy}, 1, key});
            else if (!p.bar) labels.push_back({text(s, i), {cx, top >= 0 ? rect.top - above - kLabelMiddle * ts : rect.bottom + above + kLabelMiddle * ts}, 1, key});
            else labels.push_back({text(s, i), {top >= 0 ? rect.right + off : rect.left - off, cy}, top >= 0 ? 0 : 2, key});
          }
        }
    return;
  }
  if (g.type == Type::Area) {
    for (size_t j = 0; j < k; ++j) {
      Path path;
      std::vector<Point> bottom;
      for (size_t i = 0; i < c.n; ++i) {
        double base;
        double top = valueAt(j, i, base);
        if (!std::isfinite(top)) top = base;
        const Point pt = p.At(c.Center(i), Near01(sc.At(top)));
        if (i == 0) path.MoveTo(pt);
        else path.LineTo(pt);
        bottom.push_back(p.At(c.Center(i), Clamp01(sc.At(stacked ? base : 0))));
      }
      for (auto it = bottom.rbegin(); it != bottom.rend(); ++it) path.LineTo(*it);
      path.Close();
      b.Mark(std::move(path), g.series[j].look, SeriesColor(first + j, colors), std::nullopt, 0.75f);
    }
    return;
  }
  // Lines.
  for (size_t j = 0; j < k; ++j) {
    const Series& s = g.series[j];
    const Color color = s.look.line.value_or(SeriesColor(first + j, colors));
    std::vector<std::vector<Point>> runs(1);
    std::vector<Point> points;
    for (size_t i = 0; i < c.n; ++i) {
      double base;
      const double v = valueAt(j, i, base);
      if (!std::isfinite(v)) {
        if (!runs.back().empty()) runs.emplace_back();
        continue;
      }
      const Point pt = p.At(c.Center(i), Near01(sc.At(v)));
      runs.back().push_back(pt);
      points.push_back(pt);
      const Series::PointLabel* own = nullptr;
      for (const Series::PointLabel& pl : s.pointLabels)
        if (pl.index == i) own = &pl;
      if (own) {
        // The point's own label (c:dLbl): its text or its parts, moved where the chart says, in its
        // box and colors (govuk business bodies' value boxes and panel names).
        if (own->deleted) continue;
        std::wstring t;
        if (own->text) {
          t = *own->text;
        } else {
          Series parts;
          parts.name = s.name;
          parts.values = s.values;
          if (i < s.categories.size()) parts.categories.assign(i + 1, std::wstring()), parts.categories[i] = s.categories[i];
          parts.labels = own->value.value_or(s.labels.value_or(g.labels));
          parts.seriesNames = own->seriesName.value_or(s.seriesNames.value_or(g.seriesNames));
          parts.categoryLabels = own->category.value_or(s.categoryLabels.value_or(g.categoryLabels));
          parts.percent = false;
          parts.separator = s.separator;
          parts.labelFormat = own->format.empty() ? s.labelFormat : own->format;
          t = text(parts, i);
        }
        if (!t.empty()) {
          // Beside the point as it says, else as its series or group does (a line's: to the right).
          const std::string& pos = !own->pos.empty() ? own->pos : !s.labelPos.empty() ? s.labelPos : g.labelPos;
          Pending l{t, pt, 1, std::nullopt};
          l.anchored = true;
          l.side = pos == "l" ? 'l' : pos == "t" ? 't' : pos == "b" ? 'b' : pos == "ctr" ? 'c' : 'r';
          l.offset = {own->dx * W, own->dy * H};
          std::copy(std::begin(own->insets), std::end(own->insets), std::begin(l.insets));
          l.fill = own->fill, l.color = own->color, l.size = own->size;
          l.boxW = own->w * W, l.boxH = own->h * H;
          labels.push_back(std::move(l));
        }
      } else if (ShowsLabels(g, s)) {
        labels.push_back({text(s, i), {pt.x, pt.y - 5 - kLabelMiddle * ts}, 1, ShowsKey(g, s) ? std::optional<Color>(color) : std::nullopt});
      }
    }
    if (!s.look.noLine) b.Stroke(Polyline(runs, s.smooth), color, s.look.lineWidth >= 0 ? s.look.lineWidth : 2.25f, s.look.dashes);
    const std::string symbol = MarkerSymbol(s, g.marker, first + j);
    for (const Point& pt : points) DrawMarker(b, symbol, pt, s.markerSize, MarkerLookOf(s, color, color));
  }
}

// `width`: a line's unless the series says (Excel's 1.5 points, PowerPoint's 2.25).
void DrawScatter(Builder& b, const Group& g, size_t first, const Rect& r, const Scale& xs, const Scale& ys,
                 const draw::ColorContext& colors, std::vector<Pending>& labels, const std::string& format, float ts,
                 float width) {
  const double w = r.right - r.left, h = r.bottom - r.top;
  for (size_t j = 0; j < g.series.size(); ++j) {
    const Series& s = g.series[j];
    const Color color = SeriesColor(first + j, colors);
    std::vector<std::vector<Point>> runs(1);
    std::vector<Point> points;
    for (size_t i = 0; i < s.values.size(); ++i) {
      const double x = s.xs.size() == s.values.size() ? s.xs[i] : double(i + 1), y = s.values[i];
      if (!std::isfinite(x) || !std::isfinite(y)) {
        if (!runs.back().empty()) runs.emplace_back();
        continue;
      }
      const Point pt{static_cast<float>(r.left + Near01(xs.At(x)) * w), static_cast<float>(r.bottom - Near01(ys.At(y)) * h)};
      runs.back().push_back(pt);
      points.push_back(pt);
      if (ShowsLabels(g, s))
        labels.push_back({Joined(LabelParts(g, s, i, FormatNumber(y, !s.labelFormat.empty() ? s.labelFormat : !g.labelFormat.empty() ? g.labelFormat : format), {})),
                          {pt.x, pt.y - 5 - kLabelMiddle * ts}, 1, std::nullopt});
    }
    const Color line = s.look.line.value_or(color);
    if (g.lines && !s.look.noLine) b.Stroke(Polyline(runs, s.smooth), line, s.look.lineWidth >= 0 ? s.look.lineWidth : width, s.look.dashes);
    const std::string symbol = MarkerSymbol(s, true, first + j);
    for (const Point& pt : points) DrawMarker(b, symbol, pt, s.markerSize, MarkerLookOf(s, color, line));
  }
}

// Bubbles: each point a circle at (x, y), the largest a quarter of the plot's smaller side across
// at a bubble scale of 100 %, the others in proportion by area (or by width); sizes not above zero
// aren't shown.
void DrawBubbles(Builder& b, const Group& g, size_t first, const Rect& r, const Scale& xs, const Scale& ys,
                 const draw::ColorContext& colors, std::vector<Pending>& labels, const std::string& format) {
  double largest = 0;
  for (const Series& s : g.series)
    for (double z : s.sizes)
      if (std::isfinite(z)) largest = std::max(largest, z);
  if (!(largest > 0)) return;
  const double w = r.right - r.left, h = r.bottom - r.top, full = std::min(w, h) * kBubble * g.bubbleScale / 100;
  for (size_t j = 0; j < g.series.size(); ++j) {
    const Series& s = g.series[j];
    const Color color = SeriesColor(first + j, colors);
    for (size_t i = 0; i < s.values.size() && i < s.sizes.size(); ++i) {
      const double x = s.xs.size() == s.values.size() ? s.xs[i] : double(i + 1), y = s.values[i], z = s.sizes[i];
      if (!std::isfinite(x) || !std::isfinite(y) || !(z > 0)) continue;
      const float d = static_cast<float>(full * (g.bubbleWidth ? z / largest : std::sqrt(z / largest)));
      const Point c{static_cast<float>(r.left + Near01(xs.At(x)) * w), static_cast<float>(r.bottom - Near01(ys.At(y)) * h)};
      const Look* look = PointLook(s, i);
      b.Mark(Path::Ellipse({c.x - d / 2, c.y - d / 2, c.x + d / 2, c.y + d / 2}), look ? *look : s.look, color, std::nullopt, 0.75f);
      if (ShowsLabels(g, s))
        labels.push_back({Joined(LabelParts(g, s, i, FormatNumber(y, !s.labelFormat.empty() ? s.labelFormat : !g.labelFormat.empty() ? g.labelFormat : format), {})),
                          c, 1, std::nullopt});
    }
  }
}

void DrawAxes(Builder& b, const Chart& ch, Rect r, float ts, Color text, const draw::ColorContext& colors, float W, float H) {
  const Group& g0 = ch.groups.front();
  const bool scatter = g0.type == Type::Scatter || g0.type == Type::Bubble, bar = g0.type == Type::Bar;
  // The categories, and the values each value axis shows.
  Cats cats;
  cats.between = ch.value.between;
  std::vector<std::wstring> names;
  double lo[2] = {INFINITY, INFINITY}, hi[2] = {-INFINITY, -INFINITY}, xlo = INFINITY, xhi = -INFINITY;
  bool zero[2] = {false, false}, percent = false, secondary = false;
  for (const Group& g : ch.groups) {
    if (g.type == Type::Pie || g.type == Type::Doughnut) continue;  // not drawn with axes
    const int a = g.secondary ? 1 : 0;
    secondary |= g.secondary;
    zero[a] |= g.type == Type::Column || g.type == Type::Bar || g.type == Type::Area;
    percent |= !g.secondary && g.grouping == Grouping::Percent;
    size_t n = 0;
    for (const Series& s : g.series) {
      n = std::max(n, s.values.size());
      if (names.empty()) names = s.categories;
      for (size_t i = 0; i < s.values.size(); ++i) {
        if (g.type == Type::Scatter || g.type == Type::Bubble) {
          const double x = s.xs.size() == s.values.size() ? s.xs[i] : double(i + 1);
          if (std::isfinite(x)) xlo = std::min(xlo, x), xhi = std::max(xhi, x);
        }
        if (g.grouping == Grouping::Clustered && std::isfinite(s.values[i])) lo[a] = std::min(lo[a], s.values[i]), hi[a] = std::max(hi[a], s.values[i]);
      }
    }
    if (g.grouping != Grouping::Clustered)
      for (size_t i = 0; i < n; ++i) {
        double pos, neg, total;
        Stack(g, i, g.series.size(), pos, neg, total);
        if (g.grouping == Grouping::Percent) {
          if (total > 0) pos /= total, neg /= total;
          zero[a] = true;
        }
        lo[a] = std::min(lo[a], neg), hi[a] = std::max(hi[a], pos);
      }
    cats.n = std::max(cats.n, n);
  }
  cats.n = std::max<size_t>(1, std::max(cats.n, names.size()));
  // A value axis up the side has room for a label every 1.6 lines or so in Word, 1.2 in Excel
  // (Excel's PDFs: ten intervals on a 133-point plot of 9-point labels); one across the bottom for
  // a label every kAcross text sizes of the room the labels up the side leave (PowerPoint's PDFs:
  // 0 to 10 by 5 on a 280-point plot of 18-point labels, 0 to 5 by 1 on 396 points; Excel's agree).
  // On a slide a little closer: 0 to 10 by 2 on a 312-point plot of 18-point labels (poi bar chart), by 5 on 280.
  const float acrossPitch = ch.presentation ? 3.3f : kAcross;
  // Labels beside an axis, their baseline below their tick, by the font's height (Builder::tall).
  const float valueGap = kValueGap * ts * b.tall, middle = kMiddle * ts * b.tall;
  const auto acrossMost = [&](float side) { return static_cast<int>((r.right - r.left - side) / (acrossPitch * ts)); };
  float catW = 0;  // bars: the widest category name up the side
  if (bar)
    for (size_t i = 0; i < std::min<size_t>(names.size(), 64); ++i) catW = std::max(catW, b.Measure(names[i], ts).width);
  const int most = bar ? acrossMost(std::min(catW, (r.right - r.left) * 0.4f) + valueGap)
                       : static_cast<int>((r.bottom - r.top - ts * 3) / (ts * (ch.workbook || ch.presentation ? 1.2f : 1.6f)));
  const Scale vs = MakeScale(lo[0], hi[0], ch.value, zero[0] || percent, !percent, most);
  const Scale ss = MakeScale(lo[1], hi[1], ch.secondary, zero[1], true, most);
  const std::string vformat = ch.value.format.empty() && percent ? "0%" : ch.value.format;

  // The labels round the plot.
  const std::vector<double> vticks = Ticks(vs), sticks = secondary ? Ticks(ss) : std::vector<double>();
  std::vector<Label> vlabels, slabels, clabels;
  float vw = 0, sw = 0, cw = 0;
  // Labels up a side no wider than the room the plot leaves them, across the bottom no wider than
  // the chart (more is cut).
  const float sideRoom = (r.right - r.left) * 0.4f, catRoom = bar ? sideRoom : W;
  const auto catName = [&](size_t i) { return i < names.size() ? names[i] : std::to_wstring(i + 1); };
  if (ch.value.shown)
    for (double v : vticks) vlabels.push_back(b.Fit(FormatNumber(v, vformat), ts, bar ? W : sideRoom)), vw = std::max(vw, vlabels.back().width);
  if (secondary && ch.secondary.shown)
    for (double v : sticks) slabels.push_back(b.Fit(FormatNumber(v, ch.secondary.format), ts, sideRoom)), sw = std::max(sw, slabels.back().width);
  const Scale xscale = MakeScale(xlo, xhi, ch.category, false, true, acrossMost(std::min(vw, sideRoom) + valueGap));
  const std::vector<double> xticks = scatter ? Ticks(xscale) : std::vector<double>();
  if (ch.category.shown) {
    if (scatter)
      for (double v : xticks) clabels.push_back(b.Fit(FormatNumber(v, ch.category.format), ts, W)), cw = std::max(cw, clabels.back().width);
    else
      // Hundreds of categories, of which only every so many labels show: the others measured by
      // their advances, their outlines made only when they're drawn (a redraw stays within a
      // keystroke). The first and last, which the plot's edges allow for, are outlined. Each no
      // wider than `catRoom`: an outlined one cut now, a measured one counted as cut and cut
      // when it's drawn.
      for (size_t i = 0, n = std::min<size_t>(cats.n, 4096); i < n; ++i) {
        const bool outline = n <= kOutlinedLabels || i == 0 || i + 1 == n;
        clabels.push_back(outline ? b.Fit(catName(i), ts, catRoom) : b.Measure(catName(i), ts));
        if (!outline) clabels.back().width = std::min(clabels.back().width, catRoom);
        cw = std::max(cw, clabels.back().width);
      }
  }

  // Axis titles, bold, at the room's edges (up the sides, turned): they take their room unless
  // they overlay the plot. Placed once the plot is known.
  struct Title {
    Label label;
    int side;  // 0 left, 1 right, 2 bottom
  };
  std::vector<Title> titles;
  const auto title = [&](const Axis& ax, int side) {
    if (ax.title.empty()) return;
    titles.push_back({b.Fit(ax.title, ts, side == 2 ? W : H, true), side});
    if (ax.titleOverlay) return;
    const float room = kAxisTitle + ts + kAxisTitleGap;
    (side == 0 ? r.left : side == 1 ? r.right : r.bottom) += side == 0 ? room : -room;
  };
  const Rect outer = r;
  title(ch.value, bar ? 2 : 0);
  title(ch.category, bar ? 0 : 2);
  if (secondary) title(ch.secondary, 1);

  // The plot: inside the room by Office's margin, or by the labels and their gap to the axis.
  Plot p;
  p.bar = bar;
  p.reversed = ch.category.reversed;
  // The margin at the sides stops growing with the text past 10 points (PowerPoint's PDFs: 4.6
  // points with 18-point text), on a legend's side too (poi line chart: 15.6 points from the plot
  // to its legend's key).
  const float margin = kMargin * ts, side = std::min(margin, kMargin * 10), maxSide = (r.right - r.left) * 0.4f;
  const float leftSide = side, rightSide = side;
  // Value labels up the right side when they're at the axes' high end (c:tickLblPos high).
  const bool high = !bar && !scatter && ch.value.labelsHigh;
  const float sideW = bar ? cw : high ? 0 : vw;  // labels up the left side
  const float rightW = std::max(sw, high ? vw : 0.0f);  // and up the right
  const bool across = bar ? !vlabels.empty() : !clabels.empty();  // labels across the bottom
  p.r.left = r.left + (sideW > 0 ? std::min(sideW, maxSide) + valueGap : leftSide);
  p.r.right = r.right - (rightW > 0 ? std::min(rightW, maxSide) + valueGap : rightSide);
  // At the top too, unless value labels up a side, centred on the top gridline, need more.
  p.r.top = r.top + ((!vlabels.empty() && !bar) || !slabels.empty() ? margin * b.tall : side);
  p.r.bottom = r.bottom - (across ? (kAscent + kDescent + kLabelGap) * ts * b.tall : margin);
  // Labels centred on the plot's left and right edges (values across the bottom, categories on the
  // tick marks) stay inside the room.
  const std::vector<Label>& bottom = bar ? vlabels : clabels;
  if (!bottom.empty() && (bar || scatter || !cats.between)) {
    p.r.left = std::max(p.r.left, r.left + std::min(bottom.front().width / 2, maxSide));
    if (sw <= 0) p.r.right = std::min(p.r.right, r.right - side - std::min(bottom.back().width / 2, maxSide));
  }
  // A plot area the chart places (c:manualLayout): the plot itself there, or the plot with its
  // labels' room around it as worked out above.
  if (ch.plotLayout) {
    const Rect box{ch.plotLayout->left * W, ch.plotLayout->top * H, ch.plotLayout->right * W, ch.plotLayout->bottom * H};
    p.r = ch.plotInner ? box
                       : Rect{box.left + (p.r.left - r.left), box.top + (p.r.top - r.top), box.right - (r.right - p.r.right),
                              box.bottom - (r.bottom - p.r.bottom)};
    if (ch.plotInner && !bar && !scatter) {
      // The labels round it stay inside the chart (PowerPoint's PDFs, govuk business bodies' panels):
      // value labels 1.45 points from its sides, the first and last category labels 5.65. The plot
      // moves over where there's room for them, else it's narrowed to just fit.
      const float valueRoom = vw > 0 ? std::min(vw, maxSide) + valueGap : 0, secondRoom = sw > 0 ? std::min(sw, maxSide) + valueGap : 0;
      const float leftNeed = 1.45f + (high ? 0 : valueRoom), rightNeed = 1.45f + std::max(secondRoom, high ? valueRoom : 0.0f);
      float lo = leftNeed, hi = W - rightNeed;
      if (!clabels.empty() && cats.n > 0) {
        const size_t skip = ch.category.labelSkip > 0 ? static_cast<size_t>(ch.category.labelSkip) : 1;
        const size_t last = std::min(clabels.size(), cats.n) - 1;
        const float span = p.r.right - p.r.left;
        lo = std::max(lo, 5.65f + clabels.front().width / 2 - static_cast<float>(cats.Center(0)) * span);
        hi = std::min(hi, W - 5.65f - clabels[last / skip * skip].width / 2 + static_cast<float>(1 - cats.Center(last / skip * skip)) * span);
      }
      if (lo < hi) {
        if (p.r.right - p.r.left <= hi - lo) {
          const float shift = p.r.left < lo ? lo - p.r.left : p.r.right > hi ? hi - p.r.right : 0;
          p.r.left += shift, p.r.right += shift;
        } else {
          p.r.left = lo, p.r.right = hi;
        }
      }
    }
  }
  if (p.r.right - p.r.left < 8 || p.r.bottom - p.r.top < 8) return;
  const Rect& P = p.r;
  if (!ch.plot.noFill && ch.plot.fill) b.Mark(Path::FromRect(P), ch.plot, *ch.plot.fill, std::nullopt, 0.75f);

  // Gridlines and axis lines, as their c:spPr say, or else in Office 2007's black or Office 2010's gray.
  const Color unstated = ch.legacy ? kText : kGray;
  const auto stroke = [&](Point a, Point z, const Look& look) {
    if (!look.noLine) b.Line(a, z, look.line.value_or(unstated), look.lineWidth >= 0 ? look.lineWidth : 0.75f);
  };
  if (ch.value.gridlines)
    for (double v : vticks) stroke(p.At(0, Clamp01(vs.At(v))), p.At(1, Clamp01(vs.At(v))), ch.value.grid);
  if (secondary && ch.secondary.gridlines)
    for (double v : sticks) stroke(p.At(0, Clamp01(ss.At(v))), p.At(1, Clamp01(ss.At(v))), ch.secondary.grid);
  if (scatter && ch.category.gridlines)
    for (double v : xticks) {
      const float x = static_cast<float>(P.left + Clamp01(xscale.At(v)) * (P.right - P.left));
      stroke({x, P.top}, {x, P.bottom}, ch.category.grid);
    }
  // Tick marks across an axis line at `at`, `out` the way out of the plot (4 points long, as Office,
  // longer past 10-point text: PowerPoint's PDFs, 5.7 with 18-point).
  const float tickLength = 4 + 0.21f * std::max(0.0f, ts - 10);
  const auto tick = [&](Point at, Point out, const Axis& ax) {
    if (ax.line.noLine || ax.ticks == Axis::Tick::None) return;
    const float o = ax.ticks == Axis::Tick::In ? 0.0f : tickLength, i = ax.ticks == Axis::Tick::Out ? 0.0f : tickLength;
    stroke({at.x - out.x * i, at.y - out.y * i}, {at.x + out.x * o, at.y + out.y * o}, ax.line);
  };
  if (scatter) {
    // Both axes are value axes, crossing at zero.
    const float y0 = static_cast<float>(P.bottom - Clamp01(vs.At(0)) * (P.bottom - P.top));
    const float x0 = static_cast<float>(P.left + Clamp01(xscale.At(0)) * (P.right - P.left));
    if (ch.category.shown) {
      stroke({P.left, y0}, {P.right, y0}, ch.category.line);
      for (double v : xticks) tick({static_cast<float>(P.left + Clamp01(xscale.At(v)) * (P.right - P.left)), y0}, {0, 1}, ch.category);
    }
    if (ch.value.shown) {
      stroke({x0, P.top}, {x0, P.bottom}, ch.value.line);
      for (double v : vticks) tick({x0, static_cast<float>(P.bottom - Clamp01(vs.At(v)) * (P.bottom - P.top))}, {-1, 0}, ch.value);
    }
  } else {
    const Point catOut = p.bar ? Point{-1, 0} : Point{0, 1}, valOut = p.bar ? Point{0, 1} : Point{-1, 0};
    if (ch.category.shown) {
      const double u0 = Clamp01(vs.At(0));
      stroke(p.At(0, u0), p.At(1, u0), ch.category.line);
      const size_t marks = cats.between ? cats.n + 1 : cats.n;
      for (size_t i = 0; i < marks && i < 4096; ++i)
        tick(p.At(cats.between ? double(i) / cats.n : cats.Center(i), u0), catOut, ch.category);
    }
    if (ch.value.shown) {
      stroke(p.At(0, 0), p.At(0, 1), ch.value.line);
      for (double v : vticks) tick(p.At(0, Clamp01(vs.At(v))), valOut, ch.value);
    }
    if (secondary && ch.secondary.shown) {
      stroke({P.right, P.top}, {P.right, P.bottom}, ch.secondary.line);
      for (double v : sticks) tick({P.right, p.At(0, Clamp01(ss.At(v))).y}, {1, 0}, ch.secondary);
    }
  }

  // The groups, back to front.
  std::vector<Pending> labels;
  size_t first = 0;
  for (const Group& g : ch.groups) {
    if (g.type == Type::Scatter) DrawScatter(b, g, first, P, xscale, vs, colors, labels, ch.value.format, ts, ch.presentation ? 2.25f : 1.5f);
    else if (g.type == Type::Bubble) DrawBubbles(b, g, first, P, xscale, vs, colors, labels, ch.value.format);
    else if (g.type != Type::Pie && g.type != Type::Doughnut) DrawGroup(b, g, first, p, cats, g.secondary ? ss : vs, g.secondary ? ch.secondary.format : vformat, colors, labels, ts, W, H);
    first += g.series.size();
  }
  DrawPending(b, labels, ts, text, W, H);

  // Axis labels.
  const float below = P.bottom + (kLabelGap + kAscent) * ts * b.tall;  // the baseline of labels below the plot
  for (size_t i = 0; i < vlabels.size(); ++i) {
    const Point at = p.At(0, vs.At(vticks[i]));
    if (bar) b.Place(vlabels[i], at.x, below, 1, text);
    else if (high) b.Place(vlabels[i], P.right + valueGap, at.y + middle, 0, text);
    else b.Place(vlabels[i], P.left - valueGap, at.y + middle, 2, text);
  }
  for (size_t i = 0; i < slabels.size(); ++i)
    b.Place(slabels[i], P.right + valueGap, p.At(0, ss.At(sticks[i])).y + middle, 0, text);
  if (scatter) {
    for (size_t i = 0; i < clabels.size(); ++i)
      b.Place(clabels[i], static_cast<float>(P.left + xscale.At(xticks[i]) * (P.right - P.left)), below, 1, text);
  } else if (!clabels.empty()) {
    // Every label when they fit, else every second, third…
    const double room = cats.Band() * (bar ? P.bottom - P.top : P.right - P.left);
    const double need = bar ? ts * 1.2 : cw + ts * 0.5;
    // A stated step (c:tickLblSkip) as it is, whether or not the labels fit.
    const size_t every = ch.category.labelSkip > 0 ? static_cast<size_t>(ch.category.labelSkip)
                         : room > 0              ? std::max<size_t>(1, static_cast<size_t>(std::ceil(need / room)))
                                                 : 1;
    for (size_t i = 0; i < clabels.size(); i += every) {
      const Point at = p.At(cats.Center(i), 0);
      const Label l = clabels[i].path.Empty() && clabels[i].width > 0 ? b.Fit(catName(i), ts, catRoom) : clabels[i];
      if (bar) b.Place(l, P.left - valueGap, at.y + middle, 2, text);
      else b.Place(l, at.x, below, 1, text);
    }
  }
  // The axis titles: up the sides centred on the plot, or under it.
  for (const Title& t : titles) {
    if (t.side == 2) b.Place(t.label, (P.left + P.right) / 2, outer.bottom - kAxisTitle - kDescent * ts, 1, text);
    else {
      const float x = t.side == 0 ? outer.left + kAxisTitle + kAscent * ts : outer.right - kAxisTitle - kDescent * ts;
      b.Place(t.label, x, (P.top + P.bottom) / 2, 1, text, -90);
    }
  }
}

// A radar chart (Excel's PDFs): a spoke a category, clockwise from 12 o'clock, its name past the
// spoke's end; the values out along each spoke from zero, labelled up the first, their gridlines
// polygons; each series a closed line (or filled shape) through its values.
void DrawRadar(Builder& b, const Chart& ch, Rect r, float ts, Color text, const draw::ColorContext& colors) {
  const Group& g = ch.groups.front();
  size_t n = 0;
  std::vector<std::wstring> names;
  double lo = INFINITY, hi = -INFINITY;
  for (const Series& s : g.series) {
    n = std::max(n, s.values.size());
    if (names.empty()) names = s.categories;
    for (double v : s.values)
      if (std::isfinite(v)) lo = std::min(lo, v), hi = std::max(hi, v);
  }
  n = std::min<size_t>(std::max(n, names.size()), 360);
  if (n == 0) return;
  std::vector<Label> cats;
  float widest = 0;
  if (ch.category.shown)
    for (size_t i = 0; i < n; ++i)
      cats.push_back(b.Fit(i < names.size() ? names[i] : std::to_wstring(i + 1), ts, r.right - r.left)), widest = std::max(widest, cats.back().width);
  const float gapV = cats.empty() ? 0.0f : 4.7f, gapH = cats.empty() ? 0.0f : 3.0f, lineH = cats.empty() ? 0.0f : ts;
  const float R = std::min(r.bottom - r.top - 2 * (lineH + gapV), r.right - r.left - 2 * (widest + gapH)) / 2;
  if (!(R >= 4)) return;
  // A label up the first spoke every 1.2 lines (Excel's PDFs: 0 to 25 by 5 on an 85-point radius
  // of 10-point labels; PowerPoint's: 0 to 20 by 5 on 143 points of 18-point ones).
  const Scale sc = MakeScale(lo, hi, ch.value, true, false, static_cast<int>(R / (1.2f * ts)));
  const Point c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
  const auto at = [&](size_t i, double u) {
    const double t = (-90 + 360.0 * i / n) * kPi / 180;
    return Point{static_cast<float>(c.x + R * u * std::cos(t)), static_cast<float>(c.y + R * u * std::sin(t))};
  };
  const auto polygon = [&](double u) {
    Path p;
    for (size_t i = 0; i < n; ++i) i ? p.LineTo(at(i, u)) : p.MoveTo(at(i, u));
    p.Close();
    return p;
  };
  if (!ch.plot.noFill && ch.plot.fill) b.Mark(polygon(1), ch.plot, *ch.plot.fill, std::nullopt, 0.75f);
  const Color unstated = ch.legacy ? kText : kGray;
  const std::vector<double> ticks = Ticks(sc);
  if (ch.value.gridlines && !ch.value.grid.noLine)
    for (double v : ticks)
      if (v > sc.min) b.Add(polygon(Clamp01(sc.At(v))), std::nullopt, ch.value.grid.line.value_or(unstated), ch.value.grid.lineWidth >= 0 ? ch.value.grid.lineWidth : 0.75f);
  // Spokes only where the category axis states its line (Excel draws none otherwise).
  if (ch.category.line.line && !ch.category.line.noLine)
    for (size_t i = 0; i < n; ++i) b.Line(c, at(i, 1), *ch.category.line.line, ch.category.line.lineWidth >= 0 ? ch.category.line.lineWidth : 0.75f);
  // The series.
  for (size_t j = 0; j < g.series.size(); ++j) {
    const Series& s = g.series[j];
    const Color color = (g.filled ? s.look.fill : s.look.line).value_or(SeriesColor(j, colors));
    Path p;
    std::vector<Point> points;
    for (size_t i = 0; i < n && i < s.values.size(); ++i) {
      if (!std::isfinite(s.values[i])) continue;
      points.push_back(at(i, Clamp01(sc.At(s.values[i]))));
      points.size() > 1 ? p.LineTo(points.back()) : p.MoveTo(points.back());
    }
    if (points.size() < 2) continue;
    p.Close();
    if (g.filled) b.Mark(std::move(p), s.look, color, std::nullopt, 0.75f);
    else if (!s.look.noLine) b.Stroke(std::move(p), color, s.look.lineWidth >= 0 ? s.look.lineWidth : 2.25f);
    const std::string symbol = g.filled ? std::string("none") : MarkerSymbol(s, g.marker, j);
    for (const Point& pt : points) DrawMarker(b, symbol, pt, s.markerSize, MarkerLookOf(s, color, color));
  }
  // The value labels up the first spoke, the category names past each spoke's end.
  if (ch.value.shown)
    for (double v : ticks)
      b.Place(b.Fit(FormatNumber(v, ch.value.format), ts, r.right - r.left), c.x - ts, at(0, Clamp01(sc.At(v))).y + kMiddle * ts, 2, text);
  for (size_t i = 0; i < cats.size(); ++i) {
    const Point v = at(i, 1);
    const float dx = v.x - c.x;
    if (std::fabs(dx) < R * 0.1f) {
      const bool up = v.y < c.y;
      b.Place(cats[i], v.x, up ? v.y - gapV - kDescent * ts : v.y + gapV + kAscent * ts, 1, text);
    } else {
      b.Place(cats[i], dx > 0 ? v.x + gapH : v.x - gapH, v.y + kMiddle * ts, dx > 0 ? 0 : 2, text);
    }
  }
}

bool ReadChart(pugi::xml_node space, draw::Size extent, draw::ReadContext& ctx, draw::Shape& out) {
  if (!(extent.width >= 1 && extent.height >= 1 && extent.width < 20000 && extent.height < 20000)) return false;
  Chart chart;
  if (!Read(space, ctx.colors, chart, nullptr, ctx.resources.InPresentation())) return false;
  std::wstring font;
  if (ctx.theme) font = fw::Utf8ToWide(ctx.theme->MinorFont().latin);
  if (font.empty()) font = L"Calibri";
  out = draw::Shape{};
  out.name = "Chart";
  out.art = std::make_shared<draw::Art>(Draw(chart, extent, ctx.colors, font));
  return true;
}

}  // namespace

draw::Art Draw(const Chart& ch, draw::Size size, const draw::ColorContext& colors, const std::wstring& font) {
  Builder b(size, font);
  const float W = size.width, H = size.height;
  // On a slide the labels' room follows the font's ascent (PowerPoint's PDFs: Segoe UI's 1.08 em
  // against Calibri's .95 moves the axis labels and the plot's edges by its 13 %); Excel's is Calibri's.
  if (ch.presentation) {
    const float calibri = draw::TextAscent(L"Calibri", false, false), own = draw::TextAscent(font, false, false);
    if (calibri > 0 && own > 0) b.tall = std::clamp(own / calibri, 0.8f, 1.5f);
  }
  if (!(W >= 1 && H >= 1 && W < 20000 && H < 20000) || ch.groups.empty()) return b.Take();
  const Color text = ch.textColor.value_or(kText);
  const float ts = std::clamp(ch.textSize, 1.0f, 72.0f), tts = std::clamp(ch.titleSize, 1.0f, 96.0f);
  const float corner = ch.roundedCorners ? std::min(10.0f, std::min(W, H) / 6) : 0;
  // The chart area: white unless it says (Excel, Word); in a presentation, unfilled unless it says,
  // so the slide shows through (PowerPoint).
  Look area = ch.area;
  if (ch.presentation && !area.fill) area.noFill = true;
  b.Mark(corner > 0 ? Path::RoundedRect({0, 0, W, H}, corner, corner) : Path::FromRect({0, 0, W, H}), area, kWhite,
         ch.presentation ? std::nullopt : std::optional<Color>(kGray), 0.75f);
  // The room inside the chart area's padding; the title and legend take theirs from it unless
  // they overlay the plot.
  Rect r{kPad, kPad, W - kPad, H - kPad};
  const Rect inner = r;
  // A title or legend over the plot is drawn after it, on top.
  const auto title = [&] {
    const float top = kPad + 1 + 0.206f * tts;  // the top of the title's first line (Excel's PDFs)
    // Excel wraps a title wider than about 80 % of the chart, and keeps it to about half its
    // height; each line after the first takes a line's room from the plot.
    const float pitch = kLine * tts;
    const size_t most = std::max<size_t>(1, static_cast<size_t>(H / 2 / pitch));
    const std::vector<Label> lines = WrapLines(b, ch.title, tts, ch.titleBold, ch.titleFont, 0.8f * W, most);
    for (size_t k = 0; k < lines.size(); ++k)
      b.Place(lines[k], W / 2, top + kAscent * tts + k * pitch, 1, ch.titleColor.value_or(kText));
    return top + tts + kTitleGap + (lines.empty() ? 0 : lines.size() - 1) * pitch;
  };
  if (!ch.title.empty() && !ch.titleOverlay) r.top = title();
  const std::vector<Entry> entries = ch.legend != LegendPos::None ? Entries(ch, colors) : std::vector<Entry>();
  // A legend the chart places (c:manualLayout) goes in its box, taking no room from the plot.
  const std::optional<Rect> legendBox =
      ch.legendLayout ? std::optional<Rect>(Rect{ch.legendLayout->left * W, ch.legendLayout->top * H, ch.legendLayout->right * W,
                                                 ch.legendLayout->bottom * H})
                      : std::nullopt;
  if (!entries.empty() && !legendBox && !ch.legendOverlay) r = DrawLegend(b, ch.legend, false, entries, r, ts, text);
  if (r.right - r.left >= 10 && r.bottom - r.top >= 10) {
    const Group& g0 = ch.groups.front();
    if (g0.type == Type::Pie || g0.type == Type::Doughnut)
      DrawPie(b, g0, r, ts, text, colors, W, H, ch.presentation,
              ch.plotLayout
                  ? std::optional<Rect>(Rect{ch.plotLayout->left * W, ch.plotLayout->top * H, ch.plotLayout->right * W, ch.plotLayout->bottom * H})
                  : std::nullopt);
    else if (g0.type == Type::Radar) DrawRadar(b, ch, r, ts, text, colors);
    else DrawAxes(b, ch, r, ts, text, colors, W, H);
  }
  if (!ch.title.empty() && ch.titleOverlay) title();
  if (!entries.empty() && legendBox) {
    // Its box's fill and border, as the chart states them.
    const std::optional<Color> fill = ch.legendLook.noFill ? std::nullopt : ch.legendLook.fill;
    const std::optional<Color> border = ch.legendLook.noLine ? std::nullopt : ch.legendLook.line;
    if (fill || border) b.Add(Path::FromRect(*legendBox), fill, border, ch.legendLook.lineWidth >= 0 ? ch.legendLook.lineWidth : 0.75f);
    DrawLegend(b, ch.legend, true, entries, *legendBox, ts, text, true);
  }
  if (!entries.empty() && ch.legendOverlay && !legendBox) DrawLegend(b, ch.legend, true, entries, ch.titleOverlay ? inner : Rect{inner.left, r.top, inner.right, inner.bottom}, ts, text);
  return b.Take();
}

void Install() {
  // Office shows numbers with the separators set in Windows, and puts its list separator between
  // a data label's parts.
  wchar_t decimal[8] = {}, group[8] = {}, list[8] = {};
  GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, decimal, 8);
  GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, group, 8);
  SetNumberSeparators(decimal[1] ? L'.' : decimal[0], group[1] ? L',' : group[0]);
  GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SLIST, list, 8);
  // A list separator that's also the decimal separator would make "1,5, 2" ambiguous: Excel uses
  // a semicolon then (en-NL's defaults).
  const wchar_t sep = list[1] ? L',' : list[0];
  SetListSeparator(sep == (decimal[1] ? L'.' : decimal[0]) ? L';' : sep);
  wchar_t date[64] = {};
  if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SSHORTDATE, date, 64) > 1) SetShortDate(fw::WideToUtf8(date));
  draw::SetChartReader(&ReadChart);
}

}  // namespace fw::chart
