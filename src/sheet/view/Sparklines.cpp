#include "sheet/view/Sparklines.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

#include "sheet/view/CellPaint.h"

namespace fw::sheet::view {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

std::string_view Local(const char* name) {
  std::string_view n(name);
  const size_t colon = n.find(':');
  return colon == std::string_view::npos ? n : n.substr(colon + 1);
}

pugi::xml_node Child(pugi::xml_node n, std::string_view name) {
  for (pugi::xml_node c : n.children())
    if (Local(c.name()) == name) return c;
  return {};
}

pugi::xml_attribute Attr(pugi::xml_node n, std::string_view name) {
  for (pugi::xml_attribute a : n.attributes())
    if (Local(a.name()) == name) return a;
  return {};
}

bool Flag(pugi::xml_node n, std::string_view name) {
  const pugi::xml_attribute a = Attr(n, name);
  return a && (std::string_view(a.value()) == "1" || std::string_view(a.value()) == "true");
}

// "Data!A1:L1", "'My data'!$A$1:$L$1": the sheet (or `own`) and the range.
bool Reference(const Workbook& book, std::string_view text, int own, int& sheet, Range& range) {
  sheet = own;
  const size_t bang = text.rfind('!');
  if (bang != std::string_view::npos) {
    std::string name(text.substr(0, bang));
    if (name.size() >= 2 && name.front() == '\'' && name.back() == '\'') {
      std::string plain;
      for (size_t i = 1; i + 1 < name.size(); ++i) {
        plain += name[i];
        if (name[i] == '\'' && name[i + 1] == '\'') ++i;
      }
      name = plain;
    }
    sheet = book.FindSheet(name);
    text = text.substr(bang + 1);
  }
  std::string ref;
  for (char c : text)
    if (c != '$') ref += c;
  return sheet >= 0 && ParseRange(ref, range);
}

uint32_t ColorOf(const Workbook& book, pugi::xml_node n, uint32_t fallback) {
  if (!n) return fallback;
  Color c;
  if (pugi::xml_attribute a = Attr(n, "rgb")) {
    c.kind = Color::Kind::Rgb;
    c.value = static_cast<uint32_t>(std::strtoul(a.value(), nullptr, 16));
    if (std::string_view(a.value()).size() <= 6) c.value |= 0xFF000000;
  } else if (pugi::xml_attribute t = Attr(n, "theme")) {
    c.kind = Color::Kind::Theme;
    c.value = static_cast<uint32_t>(t.as_uint());
  } else if (pugi::xml_attribute ix = Attr(n, "indexed")) {
    c.kind = Color::Kind::Indexed;
    c.value = static_cast<uint32_t>(ix.as_uint());
  } else {
    return fallback;
  }
  c.tint = Attr(n, "tint").as_double(0);
  return book.GetStyles().Resolve(c, fallback);
}

uint64_t Key(uint32_t row, uint32_t col) { return (static_cast<uint64_t>(row) << 32) | col; }

// A cell's value as a sparkline reads it: numbers (dates are numbers), else empty.
double NumberAt(const Sheet& s, uint32_t r, uint32_t c) {
  const Value v = s.GetValue(r, c);
  return v.type == ValueType::Number ? v.number : kNaN;
}

}  // namespace

Sparklines::Sparklines(const Workbook& book, size_t index) : book_(book) {
  const std::string kept = book.GetSheet(index).KeptXml();
  const size_t name = kept.find("sparklineGroups");
  if (name == std::string::npos) return;
  const size_t from = kept.rfind('<', name);
  const size_t close = kept.rfind("sparklineGroups>");
  if (from == std::string::npos || close == std::string::npos || close < name) return;
  const std::string xml = kept.substr(from, close + 16 - from);
  pugi::xml_document doc;
  if (!doc.load_buffer(xml.data(), xml.size(), pugi::parse_default, pugi::encoding_utf8)) return;
  const int own = static_cast<int>(index);
  for (pugi::xml_node g : doc.first_child().children()) {
    if (Local(g.name()) != "sparklineGroup") continue;
    SparklineGroup sg;
    const std::string_view type = Attr(g, "type").value();
    sg.type = type == "column" ? SparklineGroup::Type::Column
              : type == "stacked" ? SparklineGroup::Type::WinLoss
                                  : SparklineGroup::Type::Line;
    // The schema's default is zero; Excel writes gap.
    const std::string_view empty = Attr(g, "displayEmptyCellsAs").value();
    sg.empty = empty == "gap" ? SparklineGroup::Empty::Gap
               : empty == "span" ? SparklineGroup::Empty::Span
                                 : SparklineGroup::Empty::Zero;
    auto scale = [](std::string_view s) {
      return s == "group" ? SparklineGroup::Scale::Group
             : s == "custom" ? SparklineGroup::Scale::Custom
                             : SparklineGroup::Scale::Individual;
    };
    sg.minScale = scale(Attr(g, "minAxisType").value());
    sg.maxScale = scale(Attr(g, "maxAxisType").value());
    sg.manualMin = Attr(g, "manualMin").as_double(0);
    sg.manualMax = Attr(g, "manualMax").as_double(0);
    sg.lineWeight = Attr(g, "lineWeight").as_float(0.75f);
    sg.markers = Flag(g, "markers"), sg.high = Flag(g, "high"), sg.low = Flag(g, "low");
    sg.first = Flag(g, "first"), sg.last = Flag(g, "last"), sg.negative = Flag(g, "negative");
    sg.axis = Flag(g, "displayXAxis"), sg.hidden = Flag(g, "displayHidden"), sg.rightToLeft = Flag(g, "rightToLeft");
    sg.series = ColorOf(book, Child(g, "colorSeries"), sg.series);
    sg.negativeColor = ColorOf(book, Child(g, "colorNegative"), sg.negativeColor);
    sg.axisColor = ColorOf(book, Child(g, "colorAxis"), sg.axisColor);
    sg.markerColor = ColorOf(book, Child(g, "colorMarkers"), sg.markerColor);
    sg.firstColor = ColorOf(book, Child(g, "colorFirst"), sg.firstColor);
    sg.lastColor = ColorOf(book, Child(g, "colorLast"), sg.lastColor);
    sg.highColor = ColorOf(book, Child(g, "colorHigh"), sg.highColor);
    sg.lowColor = ColorOf(book, Child(g, "colorLow"), sg.lowColor);
    if (Flag(g, "dateAxis"))
      if (pugi::xml_node f = Child(g, "f")) Reference(book, f.text().get(), own, sg.dateSheet, sg.dates);
    for (pugi::xml_node s : Child(g, "sparklines").children()) {
      if (Local(s.name()) != "sparkline") continue;
      SparklineGroup::Line line;
      Range cell;
      const pugi::xml_node f = Child(s, "f"), sq = Child(s, "sqref");
      if (!f || !sq || !ParseRange(sq.text().get(), cell)) continue;
      if (!Reference(book, f.text().get(), own, line.sheet, line.data)) continue;
      line.row = cell.firstRow, line.col = cell.firstCol;
      at_[Key(line.row, line.col)] = {static_cast<uint32_t>(groups_.size()), static_cast<uint32_t>(sg.lines.size())};
      sg.lines.push_back(line);
    }
    if (!sg.lines.empty()) groups_.push_back(std::move(sg));
  }
}

std::vector<double> Sparklines::Values(const SparklineGroup& group, const SparklineGroup::Line& line) const {
  std::vector<double> out;
  if (line.sheet < 0 || static_cast<size_t>(line.sheet) >= book_.SheetCount()) return out;
  const Sheet& s = book_.GetSheet(static_cast<size_t>(line.sheet));
  const Range& d = line.data;
  const bool across = d.firstRow == d.lastRow && d.lastCol > d.firstCol;
  const uint64_t cells = static_cast<uint64_t>(d.lastRow - d.firstRow + 1) * (d.lastCol - d.firstCol + 1);
  if (cells > 100000) return out;  // a sparkline has a few dozen points; this is not one
  for (uint32_t r = d.firstRow; r <= d.lastRow; ++r) {
    if (!group.hidden && !across && s.Row(r).hidden) continue;
    for (uint32_t c = d.firstCol; c <= d.lastCol; ++c) {
      if (!group.hidden && across && s.Column(c).hidden) continue;
      out.push_back(NumberAt(s, r, c));
    }
  }
  return out;
}

void Sparklines::Draw(draw::Painter& p, const Geometry& g, uint32_t row, uint32_t col, const draw::Rect& cell) const {
  auto it = at_.find(Key(row, col));
  if (it == at_.end()) return;
  const SparklineGroup& sg = groups_[it->second.first];
  const SparklineGroup::Line& line = sg.lines[it->second.second];
  std::vector<double> v = Values(sg, line);
  const size_t n = v.size();
  if (n == 0) return;
  const bool winLoss = sg.type == SparklineGroup::Type::WinLoss;
  if (sg.empty == SparklineGroup::Empty::Zero && !winLoss)
    for (double& x : v)
      if (std::isnan(x)) x = 0;
  // The scale: this line's values, the group's, or the file's.
  auto range = [](const std::vector<double>& xs, double& lo, double& hi) {
    for (double x : xs)
      if (!std::isnan(x)) lo = std::min(lo, x), hi = std::max(hi, x);
  };
  double lo = std::numeric_limits<double>::infinity(), hi = -lo;
  range(v, lo, hi);
  if (!(lo <= hi)) return;  // nothing to draw
  const double ownLo = lo, ownHi = hi;
  if (sg.minScale == SparklineGroup::Scale::Group || sg.maxScale == SparklineGroup::Scale::Group) {
    double glo = lo, ghi = hi;
    for (const SparklineGroup::Line& other : sg.lines) {
      std::vector<double> ov = Values(sg, other);
      if (sg.empty == SparklineGroup::Empty::Zero)
        for (double& x : ov)
          if (std::isnan(x)) x = 0;
      range(ov, glo, ghi);
    }
    if (sg.minScale == SparklineGroup::Scale::Group) lo = glo;
    if (sg.maxScale == SparklineGroup::Scale::Group) hi = ghi;
  }
  if (sg.minScale == SparklineGroup::Scale::Custom) lo = sg.manualMin;
  if (sg.maxScale == SparklineGroup::Scale::Custom) hi = sg.manualMax;
  if (hi <= lo) {  // one value throughout: the middle
    lo -= 1;
    hi += 1;
  }
  // The plot: 2 pixels in from the cell's sides, 4 from its top and bottom (Excel's, measured).
  const draw::Rect plot{cell.left + 2 * g.px, cell.top + 4 * g.rowPx, cell.right - 2 * g.px, cell.bottom - 4 * g.rowPx};
  if (plot.right <= plot.left || plot.bottom <= plot.top) return;
  const float slot = plot.Width() / static_cast<float>(n);
  // x: the middle of each point's slot, or by date on a date axis.
  std::vector<float> xs(n);
  std::vector<double> dates;
  if (sg.dateSheet >= 0 && static_cast<size_t>(sg.dateSheet) < book_.SheetCount()) {
    SparklineGroup::Line dl;
    dl.sheet = sg.dateSheet, dl.data = sg.dates;
    dates = Values(sg, dl);
  }
  double dlo = 0, dhi = 0;
  const bool byDate = dates.size() == n && [&] {
    dlo = std::numeric_limits<double>::infinity(), dhi = -dlo;
    for (double d : dates)
      if (std::isnan(d)) return false;
      else dlo = std::min(dlo, d), dhi = std::max(dhi, d);
    return dhi > dlo;
  }();
  for (size_t i = 0; i < n; ++i) {
    float f = byDate ? static_cast<float>((dates[i] - dlo) / (dhi - dlo)) * (plot.Width() - slot) + slot / 2
                     : (static_cast<float>(i) + 0.5f) * slot;
    xs[i] = sg.rightToLeft ? plot.right - f : plot.left + f;
  }
  // Which point is which.
  size_t firstAt = n, lastAt = n;
  for (size_t i = 0; i < n; ++i)
    if (!std::isnan(v[i])) {
      if (firstAt == n) firstAt = i;
      lastAt = i;
    }
  auto special = [&](size_t i, uint32_t& color) {
    bool any = false;
    auto use = [&](bool on, uint32_t c) {
      if (on) color = c, any = true;
    };
    use(sg.negative && v[i] < 0, sg.negativeColor);
    use(sg.first && i == firstAt, sg.firstColor);
    use(sg.last && i == lastAt, sg.lastColor);
    use(sg.low && v[i] == ownLo, sg.lowColor);
    use(sg.high && v[i] == ownHi, sg.highColor);
    return any;
  };
  const float axisWidth = g.centered ? 0.36f * g.pt : g.line;
  auto axisAt = [&](float y) {
    p.FillRect({plot.left, y - axisWidth / 2, plot.right, y + axisWidth / 2}, draw::Brush::Solid(ToColor(sg.axisColor)));
  };
  p.Save();
  p.ClipRect(cell);
  if (sg.type == SparklineGroup::Type::Line) {
    const float w = sg.lineWeight * g.pt;
    // The points' range: in by half the default line's width, whatever this one's (measured).
    const float top = plot.top + 0.375f * g.pt, bottom = plot.bottom - 0.375f * g.pt;
    auto y = [&](double x) {
      return bottom - static_cast<float>((x - lo) / (hi - lo)) * (bottom - top);
    };
    if (sg.axis && lo < 0 && hi > 0) axisAt(y(0));
    draw::Path path;
    bool open = false;
    size_t inRun = 0;
    for (size_t i = 0; i < n; ++i) {
      if (std::isnan(v[i])) {
        if (sg.empty == SparklineGroup::Empty::Gap) open = false;
        continue;
      }
      const draw::Point pt{xs[i], y(v[i])};
      if (open) {
        path.LineTo(pt);
        ++inRun;
      } else {
        path.MoveTo(pt);
        open = true;
      }
    }
    if (inRun) {
      draw::Pen pen;
      pen.brush = draw::Brush::Solid(ToColor(sg.series));
      pen.width = w;
      pen.join = draw::LineJoin::Round;
      p.StrokePath(path, pen);
    }
    // Markers: diamonds, 2.64 pt corner to corner.
    const float m = 1.32f * g.pt;
    for (size_t i = 0; i < n; ++i) {
      if (std::isnan(v[i])) continue;
      uint32_t color = sg.markerColor;
      if (!special(i, color) && !sg.markers) continue;
      const float cx = xs[i], cy = y(v[i]);
      draw::Path d;
      d.MoveTo({cx, cy - m});
      d.LineTo({cx + m, cy});
      d.LineTo({cx, cy + m});
      d.LineTo({cx - m, cy});
      d.Close();
      p.FillPath(d, draw::Brush::Solid(ToColor(color)));
    }
  } else {
    // Columns and win/loss bars: slot / 1.4 wide, in the middle of their slots.
    const float bw = slot / 1.4f;
    const float least = g.centered ? 0.24f * g.pt : g.line;
    float base;
    auto y = [&](double x) { return plot.bottom - static_cast<float>((x - lo) / (hi - lo)) * plot.Height(); };
    if (winLoss) base = (plot.top + plot.bottom) / 2;
    else base = y(std::clamp(0.0, lo, hi));
    for (size_t i = 0; i < n; ++i) {
      if (std::isnan(v[i])) continue;
      float t, b;
      if (winLoss) {
        if (v[i] == 0) continue;
        t = v[i] > 0 ? plot.top : base;
        b = v[i] > 0 ? base : plot.bottom;
      } else {
        const float yv = y(std::clamp(v[i], lo, hi));
        t = std::min(yv, base), b = std::max(yv, base);
        if (b - t < least) {  // the smallest still shows
          if (yv <= base) t = b - least;
          else b = t + least;
        }
      }
      uint32_t color = sg.series;
      special(i, color);
      const float l = xs[i] - bw / 2;
      p.FillRect({l, t, l + bw, b}, draw::Brush::Solid(ToColor(color)));
    }
    if (sg.axis && (winLoss || (lo < 0 && hi > 0))) axisAt(base);
  }
  p.Restore();
}

}  // namespace fw::sheet::view
