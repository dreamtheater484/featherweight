// Laying out a chart (fwchart) as vector art: the chart area, title and legend, the axes with their
// labels and gridlines, then each group's marks and data labels. Where the file says nothing, the
// look is Office's: 9 pt black text, light gray gridlines, the theme's accent colors in order. Text
// is drawn as glyph outlines, so it prints and exports like any shape.
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
// kGray: lines a chart doesn't give a color (gridlines, axes, its border), as Office 2007's
// default style draws them; Office 2013 and later state theirs.
const Color kText = draw::Rgb(0x000000), kGray = draw::Rgb(0x868686), kWhite = draw::Rgb(0xFFFFFF);

std::wstring Wide(const std::string& s) { return fw::Utf8ToWide(s); }

wchar_t g_decimal = L'.', g_group = L',';
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
  std::snprintf(buf, sizeof buf, "%.*f", std::clamp(decimals, 0, 10), std::fabs(x));
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

struct Label {
  Path path;
  float width = 0;
};

class Builder {
 public:
  Builder(draw::Size size, std::wstring font) : font_(std::move(font)) { art_.size = size; }

  void Add(Path p, std::optional<Color> fill, std::optional<Color> line, float width, const Matrix& t = {}) {
    if (art_.marks.size() >= kMaxMarks || p.Empty()) return;
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
      m.line.cap = draw::LineCap::Round;
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
  void Line(Point a, Point b, Color c, float width) {
    Path p;
    p.MoveTo(a);
    p.LineTo(b);
    Add(std::move(p), std::nullopt, c, width);
  }
  Label Text(const std::wstring& s, float size, bool bold = false) const {
    Label l;
    if (s.empty() || !(size > 0)) return l;
    l.path = draw::TextOutline(s.substr(0, 255), font_, bold, false, size);
    if (!l.path.Empty()) l.width = std::max(0.0f, l.path.Bounds().right);
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

const Look* PointLook(const Series& s, size_t i) {
  for (const auto& [idx, look] : s.points)
    if (idx == i) return &look;
  return nullptr;
}

// ---- Scales

struct Scale {
  double min = 0, max = 1, step = 0.2;
  double At(double v) const { return (v - min) / (max - min); }
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
// values' range past the extremes, round major units; the axis's own minimum and maximum win.
Scale MakeScale(double lo, double hi, const Axis& axis, bool zero, bool pad, int most = 10) {
  if (!std::isfinite(lo) || !std::isfinite(hi) || lo > hi) lo = 0, hi = 1;
  const double spread = hi - lo;
  if (lo > 0 && (zero || hi - lo >= hi / 6)) lo = 0;
  if (hi < 0 && (zero || hi - lo >= -lo / 6)) hi = 0;
  if (hi == lo) hi = lo + (lo == 0 ? 1 : std::fabs(lo) / 2);
  if (pad) {
    const double room = (spread > 0 ? spread : hi - lo) * 0.05;
    if (hi > 0) hi += room;
    if (lo < 0) lo -= room;
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
  Color color;
  bool line = false;  // a line or scatter series: a stroke for its swatch
};

std::vector<Entry> Entries(const Chart& ch, const draw::ColorContext& colors) {
  std::vector<Entry> out;
  const Group& g0 = ch.groups.front();
  if ((g0.type == Type::Pie || g0.type == Type::Doughnut || (g0.varyColors && g0.series.size() == 1)) && !g0.series.empty()) {
    const Series& s = g0.series.front();
    const size_t n = std::max(s.categories.size(), s.values.size());
    for (size_t i = 0; i < n && i < 64; ++i) {
      const Look* look = PointLook(s, i);
      Color c = look && look->fill ? *look->fill : SeriesColor(i, colors);
      out.push_back({i < s.categories.size() ? s.categories[i] : std::to_wstring(i + 1), c, false});
    }
    return out;
  }
  size_t k = 0;
  for (const Group& g : ch.groups)
    for (const Series& s : g.series) {
      const bool line = g.type == Type::Line || (g.type == Type::Scatter && !s.look.noLine && g.lines);
      Color c = SeriesColor(k, colors);
      if (line && s.look.line) c = *s.look.line;
      else if (!line && s.look.fill) c = *s.look.fill;
      out.push_back({s.name.empty() ? L"Series " + std::to_wstring(k + 1) : s.name, c, line});
      if (++k >= 64) break;
    }
  // Horizontal bars list their series bottom up, as they're stacked in each category; so do
  // stacked columns, lines and areas in a legend at the side, top down as they stack (Word).
  const bool side = ch.legend == LegendPos::Right || ch.legend == LegendPos::Left;
  if (ch.groups.size() == 1 && (g0.type == Type::Bar || (side && g0.grouping != Grouping::Clustered)))
    std::reverse(out.begin(), out.end());
  return out;
}

Rect DrawLegend(Builder& b, LegendPos pos, const std::vector<Entry>& entries, Rect r, float ts, Color text) {
  const float sw = ts * 0.7f, gap = ts * 0.4f, lineH = ts * 1.5f;
  std::vector<Label> labels;
  float widest = 0;
  for (const Entry& e : entries) {
    labels.push_back(b.Text(e.name, ts));
    widest = std::max(widest, labels.back().width);
  }
  const auto swatch = [&](const Entry& e, float x, float cy) {
    if (e.line) b.Line({x, cy}, {x + sw * 1.6f, cy}, e.color, 2);
    else b.Add(Path::FromRect({x + sw * 0.3f, cy - sw / 2, x + sw * 1.3f, cy + sw / 2}), e.color, std::nullopt, 0);
  };
  const float lead = sw * 1.6f + gap;
  if (pos == LegendPos::Right || pos == LegendPos::Left) {
    const float w = std::min(widest + lead + ts, (r.right - r.left) * 0.45f);
    const float x = pos == LegendPos::Right ? r.right - w + ts / 2 : r.left;
    float y = (r.top + r.bottom) / 2 - entries.size() * lineH / 2 + lineH / 2;
    for (size_t i = 0; i < entries.size(); ++i, y += lineH) {
      if (y < r.top || y > r.bottom) continue;
      swatch(entries[i], x, y);
      b.Place(labels[i], x + lead, y + ts * 0.35f, 0, text);
    }
    (pos == LegendPos::Right ? r.right : r.left) += pos == LegendPos::Right ? -w : w;
    return r;
  }
  // Top or bottom: rows of entries, centered.
  std::vector<std::vector<size_t>> rows(1);
  float rowW = 0;
  const float avail = r.right - r.left;
  for (size_t i = 0; i < entries.size(); ++i) {
    const float w = lead + labels[i].width + ts;
    if (!rows.back().empty() && rowW + w > avail) {
      if (rows.size() == 4) break;
      rows.emplace_back();
      rowW = 0;
    }
    rows.back().push_back(i);
    rowW += w;
  }
  const float height = rows.size() * lineH;
  float y = pos == LegendPos::Bottom ? r.bottom - height + lineH / 2 : r.top + lineH / 2;
  for (const auto& row : rows) {
    float total = 0;
    for (size_t i : row) total += lead + labels[i].width + ts;
    float x = (r.left + r.right) / 2 - total / 2;
    for (size_t i : row) {
      swatch(entries[i], x, y);
      b.Place(labels[i], x + lead, y + ts * 0.35f, 0, text);
      x += lead + labels[i].width + ts;
    }
    y += lineH;
  }
  (pos == LegendPos::Bottom ? r.bottom : r.top) += pos == LegendPos::Bottom ? -(height + 4) : height + 4;
  return r;
}

// ---- Data labels, drawn last so marks don't cover them

struct Pending {
  std::wstring text;
  Point at;
  int align;
};

void DrawPending(Builder& b, const std::vector<Pending>& labels, float ts, Color text) {
  for (const Pending& p : labels) b.Place(b.Text(p.text, ts), p.at.x, p.at.y, p.align, text);
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

void DrawPie(Builder& b, const Group& g, Rect r, float ts, Color text, const draw::ColorContext& colors) {
  const Series& s = g.series.front();
  double total = 0;
  for (double v : s.values)
    if (v > 0 && std::isfinite(v)) total += v;
  if (!(total > 0) || !std::isfinite(total)) return;
  const float R = std::min(r.right - r.left, r.bottom - r.top) / 2 * 0.95f;
  if (R < 2) return;
  const Point c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
  const float hole = g.type == Type::Doughnut ? static_cast<float>(R * g.hole / 100) : 0;
  const bool showVal = s.labels.value_or(g.labels), showPct = s.percent.value_or(g.percent),
             showCat = s.categoryLabels.value_or(g.categoryLabels);
  std::vector<Pending> labels;
  double a = g.firstAngle - 90;
  for (size_t i = 0; i < s.values.size(); ++i) {
    const double v = s.values[i];
    if (!(v > 0) || !std::isfinite(v)) continue;
    const double sweep = v / total * 360;
    const Look* look = PointLook(s, i);
    const Color fill = g.varyColors ? SeriesColor(i, colors) : s.look.fill.value_or(SeriesColor(0, colors));
    b.Mark(Slice(c, R, hole, a, a + sweep), look ? *look : s.look, fill, kWhite, 1.5f);
    if (showVal || showPct || showCat) {
      std::wstring t;
      if (showCat && i < s.categories.size()) t = s.categories[i];
      if (showVal) t += (t.empty() ? L"" : L", ") + FormatNumber(v, "General");
      if (showPct) t += (t.empty() ? L"" : L", ") + FormatNumber(v / total, "0%");
      const double mid = (a + sweep / 2) * kPi / 180;
      const float rad = hole > 0 ? (R + hole) / 2 : R * 0.65f;
      labels.push_back({t, {static_cast<float>(c.x + rad * std::cos(mid)), static_cast<float>(c.y + rad * std::sin(mid) + ts * 0.35f)}, 1});
    }
    a += sweep;
  }
  DrawPending(b, labels, ts, text);
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

void DrawGroup(Builder& b, const Group& g, size_t first, const Plot& p, const Cats& c, const Scale& sc,
               const std::string& format, const draw::ColorContext& colors, std::vector<Pending>& labels, float ts) {
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
  if (g.type == Type::Column || g.type == Type::Bar) {
    const double band = c.Band(), gap = g.gap / 100, ov = g.overlap / 100;
    const double w = stacked ? band / (1 + gap) : band / (k - (k - 1) * ov + gap);
    for (size_t i = 0; i < c.n; ++i)
      for (size_t j = 0; j < k; ++j) {
        double base;
        const double top = valueAt(j, i, base);
        if (!std::isfinite(top)) continue;
        const double t0 = stacked ? c.Center(i) - w / 2 : c.Center(i) - (k * w - (k - 1) * ov * w) / 2 + j * w * (1 - ov);
        const Point a = p.At(t0, Clamp01(sc.At(base))), z = p.At(t0 + w, Clamp01(sc.At(top)));
        const Rect rect{std::min(a.x, z.x), std::min(a.y, z.y), std::max(a.x, z.x), std::max(a.y, z.y)};
        const Series& s = g.series[j];
        const Look* look = PointLook(s, i);
        const Color fill = g.varyColors && k == 1 ? SeriesColor(i, colors) : SeriesColor(first + j, colors);
        b.Mark(Path::FromRect(rect), look ? *look : s.look, fill, std::nullopt, 0.75f);
        if (s.labels.value_or(g.labels)) {
          const std::wstring t = FormatNumber(Value(s, i), percent ? std::string() : format);
          if (stacked) labels.push_back({t, {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2 + ts * 0.35f}, 1});
          else if (!p.bar) labels.push_back({t, {(rect.left + rect.right) / 2, (top >= 0 ? rect.top - 3 : rect.bottom + ts + 1)}, 1});
          else labels.push_back({t, {top >= 0 ? rect.right + 3 : rect.left - 3, (rect.top + rect.bottom) / 2 + ts * 0.35f}, top >= 0 ? 0 : 2});
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
      if (s.labels.value_or(g.labels)) labels.push_back({FormatNumber(Value(s, i), percent ? std::string() : format), {pt.x, pt.y - 5}, 1});
    }
    if (!s.look.noLine) b.Add(Polyline(runs, s.smooth), std::nullopt, color, s.look.lineWidth >= 0 ? s.look.lineWidth : 2.25f);
    if (s.marker.value_or(g.marker))
      for (const Point& pt : points) b.Add(Path::Ellipse({pt.x - 2.5f, pt.y - 2.5f, pt.x + 2.5f, pt.y + 2.5f}), s.look.fill.value_or(color), std::nullopt, 0);
  }
}

void DrawScatter(Builder& b, const Group& g, size_t first, const Rect& r, const Scale& xs, const Scale& ys,
                 const draw::ColorContext& colors, std::vector<Pending>& labels, const std::string& format) {
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
      if (s.labels.value_or(g.labels)) labels.push_back({FormatNumber(y, format), {pt.x, pt.y - 5}, 1});
    }
    if (g.lines && !s.look.noLine)
      b.Add(Polyline(runs, s.smooth), std::nullopt, s.look.line.value_or(color), s.look.lineWidth >= 0 ? s.look.lineWidth : 1.5f);
    if (s.marker.value_or(true))
      for (const Point& pt : points) b.Add(Path::Ellipse({pt.x - 2.5f, pt.y - 2.5f, pt.x + 2.5f, pt.y + 2.5f}), s.look.fill.value_or(color), std::nullopt, 0);
  }
}

void DrawAxes(Builder& b, const Chart& ch, Rect r, float ts, Color text, const draw::ColorContext& colors) {
  const Group& g0 = ch.groups.front();
  const bool scatter = g0.type == Type::Scatter, bar = g0.type == Type::Bar;
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
        if (g.type == Type::Scatter) {
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
  // A value axis up the side has room for a label every 1.6 lines or so.
  const int most = bar ? 10 : static_cast<int>((r.bottom - r.top - ts * 3) / (ts * 1.6f));
  const Scale vs = MakeScale(lo[0], hi[0], ch.value, zero[0] || percent, !percent, most);
  const Scale ss = MakeScale(lo[1], hi[1], ch.secondary, zero[1], true, most);
  const Scale xscale = MakeScale(xlo, xhi, ch.category, false, true);
  const std::string vformat = ch.value.format.empty() && percent ? "0%" : ch.value.format;

  // Room for the labels and titles around the plot.
  const std::vector<double> vticks = Ticks(vs), sticks = secondary ? Ticks(ss) : std::vector<double>();
  std::vector<Label> vlabels, slabels, clabels;
  float vw = 0, sw = 0, cw = 0;
  if (ch.value.shown)
    for (double v : vticks) vlabels.push_back(b.Text(FormatNumber(v, vformat), ts)), vw = std::max(vw, vlabels.back().width);
  if (secondary && ch.secondary.shown)
    for (double v : sticks) slabels.push_back(b.Text(FormatNumber(v, ch.secondary.format), ts)), sw = std::max(sw, slabels.back().width);
  const std::vector<double> xticks = scatter ? Ticks(xscale) : std::vector<double>();
  if (ch.category.shown) {
    if (scatter)
      for (double v : xticks) clabels.push_back(b.Text(FormatNumber(v, ch.category.format), ts)), cw = std::max(cw, clabels.back().width);
    else
      for (size_t i = 0; i < cats.n && i < 4096; ++i)
        clabels.push_back(b.Text(i < names.size() ? names[i] : std::to_wstring(i + 1), ts)), cw = std::max(cw, clabels.back().width);
  }
  const float row = ts * 1.5f;
  if (!ch.value.title.empty()) {
    const Label t = b.Text(ch.value.title, ts, true);
    if (bar) b.Place(t, (r.left + r.right) / 2, r.bottom - ts * 0.3f, 1, text), r.bottom -= row;
    else b.Place(t, r.left + ts, (r.top + r.bottom) / 2, 1, text, -90), r.left += row;
  }
  if (!ch.category.title.empty()) {
    const Label t = b.Text(ch.category.title, ts, true);
    if (bar) b.Place(t, r.left + ts, (r.top + r.bottom) / 2, 1, text, -90), r.left += row;
    else b.Place(t, (r.left + r.right) / 2, r.bottom - ts * 0.3f, 1, text), r.bottom -= row;
  }
  Plot p;
  p.bar = bar;
  p.reversed = ch.category.reversed;
  p.r = r;
  const float maxSide = (r.right - r.left) * 0.4f;
  if (bar) {
    p.r.left += std::min(cw, maxSide) + (cw > 0 ? 6 : 0);
    p.r.bottom -= vlabels.empty() ? 0 : row;
  } else {
    p.r.left += std::min(vw, maxSide) + (vw > 0 ? 6 : 0);
    p.r.right -= std::min(sw, maxSide) + (sw > 0 ? 6 : 0);
    p.r.bottom -= clabels.empty() ? 0 : row;
  }
  // Labels centred on the plot's left and right edges (values across the bottom, categories on the
  // tick marks) stay inside the chart.
  const std::vector<Label>& across = bar ? vlabels : clabels;
  if (!across.empty() && (bar || scatter || !cats.between)) {
    p.r.left = std::max(p.r.left, r.left + std::min(across.front().width / 2, maxSide));
    p.r.right = std::min(p.r.right, r.right - std::min(across.back().width / 2, maxSide));
  }
  p.r.top += ts / 2;
  if (p.r.right - p.r.left < 8 || p.r.bottom - p.r.top < 8) return;
  const Rect& P = p.r;
  if (!ch.plot.noFill && ch.plot.fill) b.Mark(Path::FromRect(P), ch.plot, *ch.plot.fill, std::nullopt, 0.75f);

  // Gridlines and the category axis line, as their c:spPr say, or else in Office 2007's gray.
  const auto stroke = [&](Point a, Point z, const Look& look) {
    if (!look.noLine) b.Line(a, z, look.line.value_or(kGray), look.lineWidth >= 0 ? look.lineWidth : 0.75f);
  };
  if (ch.value.gridlines)
    for (double v : vticks) stroke(p.At(0, Clamp01(vs.At(v))), p.At(1, Clamp01(vs.At(v))), ch.value.grid);
  if (scatter && ch.category.gridlines)
    for (double v : xticks) {
      const float x = static_cast<float>(P.left + Clamp01(xscale.At(v)) * (P.right - P.left));
      stroke({x, P.top}, {x, P.bottom}, ch.category.grid);
    }
  // Tick marks across an axis line at `at`, `out` the way out of the plot (4 points long, as Office).
  const auto tick = [&](Point at, Point out, const Axis& ax) {
    if (ax.line.noLine || ax.ticks == Axis::Tick::None) return;
    const float o = ax.ticks == Axis::Tick::In ? 0.0f : 4.0f, i = ax.ticks == Axis::Tick::Out ? 0.0f : 4.0f;
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
  }

  // The groups, back to front.
  std::vector<Pending> labels;
  size_t first = 0;
  for (const Group& g : ch.groups) {
    if (g.type == Type::Scatter) DrawScatter(b, g, first, P, xscale, vs, colors, labels, ch.value.format);
    else if (g.type != Type::Pie && g.type != Type::Doughnut) DrawGroup(b, g, first, p, cats, g.secondary ? ss : vs, g.secondary ? ch.secondary.format : vformat, colors, labels, ts);
    first += g.series.size();
  }
  DrawPending(b, labels, ts, text);

  // Axis labels.
  for (size_t i = 0; i < vlabels.size(); ++i) {
    const Point at = p.At(0, vs.At(vticks[i]));
    if (bar) b.Place(vlabels[i], at.x, P.bottom + ts + 2, 1, text);
    else b.Place(vlabels[i], P.left - 4, at.y + ts * 0.35f, 2, text);
  }
  for (size_t i = 0; i < slabels.size(); ++i) b.Place(slabels[i], P.right + 4, p.At(0, ss.At(sticks[i])).y + ts * 0.35f, 0, text);
  if (scatter) {
    for (size_t i = 0; i < clabels.size(); ++i)
      b.Place(clabels[i], static_cast<float>(P.left + xscale.At(xticks[i]) * (P.right - P.left)), P.bottom + ts + 2, 1, text);
  } else if (!clabels.empty()) {
    // Every label when they fit, else every second, third…
    const double room = cats.Band() * (bar ? P.bottom - P.top : P.right - P.left);
    const double need = bar ? ts * 1.2 : cw + ts * 0.5;
    const size_t every = room > 0 ? std::max<size_t>(1, static_cast<size_t>(std::ceil(need / room))) : 1;
    for (size_t i = 0; i < clabels.size(); i += every) {
      const Point at = p.At(cats.Center(i), 0);
      if (bar) b.Place(clabels[i], P.left - 4, at.y + ts * 0.35f, 2, text);
      else b.Place(clabels[i], at.x, P.bottom + ts + 2, 1, text);
    }
  }
}

bool ReadChart(pugi::xml_node space, draw::Size extent, draw::ReadContext& ctx, draw::Shape& out) {
  if (!(extent.width >= 1 && extent.height >= 1 && extent.width < 20000 && extent.height < 20000)) return false;
  Chart chart;
  if (!Read(space, ctx.colors, chart)) return false;
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
  if (!(W >= 1 && H >= 1 && W < 20000 && H < 20000) || ch.groups.empty()) return b.Take();
  const Color text = ch.textColor.value_or(kText);
  const float ts = std::clamp(ch.textSize, 1.0f, 72.0f), tts = std::clamp(ch.titleSize, 1.0f, 96.0f);
  b.Mark(Path::FromRect({0, 0, W, H}), ch.area, kWhite, kGray, 0.75f);
  Rect r{7, 7, W - 7, H - 7};
  if (!ch.title.empty()) {
    b.Place(b.Text(ch.title, tts, ch.titleBold), W / 2, r.top + tts * 0.95f, 1, ch.titleColor.value_or(kText));
    r.top += tts * 1.5f;
  }
  if (ch.legend != LegendPos::None) {
    const std::vector<Entry> entries = Entries(ch, colors);
    if (!entries.empty()) r = DrawLegend(b, ch.legend, entries, r, ts, text);
  }
  if (r.right - r.left < 10 || r.bottom - r.top < 10) return b.Take();
  const Group& g0 = ch.groups.front();
  if (g0.type == Type::Pie || g0.type == Type::Doughnut) DrawPie(b, g0, r, ts, text, colors);
  else DrawAxes(b, ch, r, ts, text, colors);
  return b.Take();
}

void Install() {
  // Office shows numbers with the separators set in Windows.
  wchar_t decimal[8] = {}, group[8] = {};
  GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, decimal, 8);
  GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, group, 8);
  SetNumberSeparators(decimal[1] ? L'.' : decimal[0], group[1] ? L',' : group[0]);
  wchar_t date[64] = {};
  if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SSHORTDATE, date, 64) > 1) SetShortDate(fw::WideToUtf8(date));
  draw::SetChartReader(&ReadChart);
}

}  // namespace fw::chart
