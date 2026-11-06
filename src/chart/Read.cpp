// Reading c:chartSpace (fwchart): the plot area's chart groups, their series with the values Office
// cached with them, the axes, the title and the legend. Untrusted input: counts are capped, and a
// chart with a type Featherweight doesn't draw isn't read at all (it stays a placeholder rather than
// showing part of itself).
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

#include "chart/Chart.h"
#include "core/Strings.h"
#include "draw/Theme.h"
#include "draw/Xml.h"

namespace fw::chart {
namespace {

using namespace draw::xml;

constexpr size_t kMaxPoints = 4096;     // per series
constexpr size_t kMaxAllPoints = 20000;  // in the chart
constexpr size_t kMaxText = 255;        // characters of a name or label
const double kNaN = std::numeric_limits<double>::quiet_NaN();

pugi::xml_node C(pugi::xml_node parent, std::string_view local) { return Child(parent, kChart, local); }
std::string_view Val(pugi::xml_node n) { return n.attribute("val").value(); }
// <c:name val="1"/>: absent gives `def`, present without val is on.
bool On(pugi::xml_node parent, std::string_view local, bool def) {
  pugi::xml_node n = C(parent, local);
  return n ? Bool(n, "val", true) : def;
}
std::wstring Wide(std::string s) {
  if (s.size() > kMaxText * 4) s.resize(kMaxText * 4);
  std::wstring w = fw::Utf8ToWide(s);
  if (w.size() > kMaxText) w.resize(kMaxText);
  return w;
}

// A cache's points by c:pt idx, ptCount long: the text of each.
std::vector<std::string> CachedText(pugi::xml_node cache) {
  std::vector<std::string> out;
  if (!cache) return out;
  out.resize(static_cast<size_t>(std::clamp(Number(C(cache, "ptCount"), "val", 0.0), 0.0, double(kMaxPoints))));
  ForEachChild(cache, [&](pugi::xml_node pt) {
    if (LocalNameOf(pt) != "pt") return true;
    const double idx = Number(pt, "idx", -1);
    if (!(idx >= 0 && idx < double(kMaxPoints))) return true;
    const size_t i = static_cast<size_t>(idx);
    if (i >= out.size()) out.resize(i + 1);
    out[i] = C(pt, "v").text().get();
    return true;
  });
  return out;
}

pugi::xml_node CacheOf(pugi::xml_node data, std::string_view ref, std::string_view cache, std::string_view literal) {
  if (pugi::xml_node r = C(data, ref)) return C(r, cache);
  return C(data, literal);
}

std::vector<double> Numbers(pugi::xml_node data) {
  std::vector<double> out;
  for (const std::string& t : CachedText(CacheOf(data, "numRef", "numCache", "numLit")))
    out.push_back(t.empty() ? kNaN : Number(t, kNaN));
  return out;
}

// c:tx: rich text (paragraphs joined by spaces), a cached string, or a literal.
std::wstring TextOf(pugi::xml_node tx) {
  std::string out;
  if (pugi::xml_node rich = C(tx, "rich")) {
    ForEachChild(rich, [&](pugi::xml_node p) {
      if (LocalNameOf(p) != "p") return true;
      std::string line;
      ForEachChild(p, [&](pugi::xml_node r) {
        if (LocalNameOf(r) == "r" || LocalNameOf(r) == "fld") line += A(r, "t").text().get();
        return line.size() < kMaxText;
      });
      if (!line.empty()) out += (out.empty() ? "" : " ") + line;
      return out.size() < kMaxText;
    });
  } else if (pugi::xml_node ref = C(tx, "strRef")) {
    std::vector<std::string> pts = CachedText(C(ref, "strCache"));
    for (const std::string& s : pts) out += (out.empty() || s.empty() ? "" : " ") + s;
  } else {
    out = C(tx, "v").text().get();
  }
  return Wide(out);
}

// The size (points), color and weight of text from a c:txPr or a:rich's first paragraph properties.
void TextLook(pugi::xml_node body, const draw::ColorContext& colors, float* size, std::optional<draw::Color>* color,
              bool* bold = nullptr) {
  pugi::xml_node rPr;
  ForEachChild(body, [&](pugi::xml_node p) {
    if (LocalNameOf(p) != "p") return true;
    rPr = A(A(p, "pPr"), "defRPr");
    if (!rPr) rPr = A(A(p, "r"), "rPr");
    return false;
  });
  if (!rPr) return;
  if (size && rPr.attribute("sz")) *size = static_cast<float>(std::clamp(Number(rPr, "sz", 900) / 100, 1.0, 200.0));
  if (bold && rPr.attribute("b")) *bold = Bool(rPr, "b", true);
  if (color)
    if (pugi::xml_node f = A(rPr, "solidFill"))
      if (auto c = draw::ReadColor(f, colors)) *color = c;
}

Look LookOf(pugi::xml_node spPr, const draw::ColorContext& colors) {
  Look l;
  if (!spPr) return l;
  if (A(spPr, "noFill")) l.noFill = true;
  else if (pugi::xml_node f = A(spPr, "solidFill")) l.fill = draw::ReadColor(f, colors);
  else if (pugi::xml_node g = A(spPr, "gradFill")) l.fill = draw::ReadColor(A(A(g, "gsLst"), "gs"), colors);
  else if (pugi::xml_node pf = A(spPr, "pattFill")) l.fill = draw::ReadColor(A(pf, "fgClr"), colors);
  if (pugi::xml_node ln = A(spPr, "ln")) {
    if (A(ln, "noFill")) l.noLine = true;
    else if (pugi::xml_node f = A(ln, "solidFill")) l.line = draw::ReadColor(f, colors);
    if (ln.attribute("w")) l.lineWidth = std::clamp(EmuToPt(Number(ln, "w", 9525)), 0.0f, 50.0f);
  }
  return l;
}

void LabelsOf(pugi::xml_node dLbls, std::optional<bool>& val, std::optional<bool>& pct, std::optional<bool>& cat) {
  if (!dLbls) return;
  if (On(dLbls, "delete", false)) {
    val = pct = cat = false;
    return;
  }
  if (C(dLbls, "showVal")) val = On(dLbls, "showVal", false);
  if (C(dLbls, "showPercent")) pct = On(dLbls, "showPercent", false);
  if (C(dLbls, "showCatName")) cat = On(dLbls, "showCatName", false);
}

Series SeriesOf(pugi::xml_node ser, Type type, const draw::ColorContext& colors, size_t& budget) {
  Series s;
  if (pugi::xml_node tx = C(ser, "tx")) s.name = TextOf(tx);
  s.look = LookOf(C(ser, "spPr"), colors);
  ForEachChild(ser, [&](pugi::xml_node dPt) {
    if (LocalNameOf(dPt) == "dPt" && NamespaceOf(dPt) == kChart && s.points.size() < kMaxPoints) {
      const double idx = Number(C(dPt, "idx"), "val", -1);
      if (idx >= 0 && idx < double(kMaxPoints)) s.points.push_back({static_cast<uint32_t>(idx), LookOf(C(dPt, "spPr"), colors)});
    }
    return true;
  });
  const bool scatter = type == Type::Scatter;
  pugi::xml_node cat = C(ser, scatter ? "xVal" : "cat");
  if (scatter && (C(cat, "numRef") || C(cat, "numLit"))) {
    s.xs = Numbers(cat);
  } else if (cat) {
    pugi::xml_node cache = CacheOf(cat, "strRef", "strCache", "strLit");
    if (!cache) cache = CacheOf(cat, "numRef", "numCache", "numLit");
    if (!cache)
      if (pugi::xml_node multi = C(C(cat, "multiLvlStrRef"), "multiLvlStrCache")) cache = C(multi, "lvl");
    std::string format = C(cache, "formatCode").text().get();
    for (const std::string& t : CachedText(cache)) {
      if (!format.empty() && !t.empty()) {
        if (double v = Number(t, kNaN); v == v) {
          s.categories.push_back(FormatNumber(v, format));
          continue;
        }
      }
      s.categories.push_back(Wide(t));
    }
  }
  s.values = Numbers(C(ser, scatter ? "yVal" : "val"));
  // The chart's points are capped as a whole: a series past the budget is cut short.
  const size_t n = std::min(s.values.size(), budget);
  s.values.resize(n);
  if (s.xs.size() > n) s.xs.resize(n);
  budget -= n;
  if (pugi::xml_node m = C(ser, "marker")) s.marker = Val(C(m, "symbol")) != "none";
  s.smooth = (type == Type::Line || scatter) && On(ser, "smooth", true);
  LabelsOf(C(ser, "dLbls"), s.labels, s.percent, s.categoryLabels);
  return s;
}

// The format code of a group's first series' cached numbers under `tag` (c:val, c:xVal, c:yVal).
std::string SourceFormat(pugi::xml_node group, const char* tag) {
  return C(CacheOf(C(C(group, "ser"), tag), "numRef", "numCache", "numLit"), "formatCode").text().get();
}

// An axis; `source` is the format its numbers take when they're linked to the data's.
Axis AxisOf(pugi::xml_node ax, const std::string& source) {
  Axis a;
  a.shown = !On(ax, "delete", false) && Val(C(ax, "tickLblPos")) != "none";
  a.gridlines = static_cast<bool>(C(ax, "majorGridlines"));
  if (pugi::xml_node f = C(ax, "numFmt")) a.format = Bool(f, "sourceLinked", false) ? source : f.attribute("formatCode").value();
  pugi::xml_node scaling = C(ax, "scaling");
  if (pugi::xml_node m = C(scaling, "min")) a.min = Number(m, "val", kNaN);
  if (pugi::xml_node m = C(scaling, "max")) a.max = Number(m, "val", kNaN);
  if (a.min && !(*a.min == *a.min)) a.min.reset();
  if (a.max && !(*a.max == *a.max)) a.max.reset();
  a.reversed = Val(C(scaling, "orientation")) == "maxMin";
  if (pugi::xml_node cb = C(ax, "crossBetween")) a.between = Val(cb) != "midCat";
  if (pugi::xml_node t = C(ax, "title")) a.title = TextOf(C(t, "tx"));
  return a;
}

}  // namespace

bool Read(pugi::xml_node chartSpace, const draw::ColorContext& colors, Chart& out) {
  out = Chart{};
  if (!chartSpace || LocalNameOf(chartSpace) != "chartSpace" || NamespaceOf(chartSpace) != kChart) return false;
  pugi::xml_node chart = C(chartSpace, "chart");
  pugi::xml_node plot = C(chart, "plotArea");
  if (!plot) return false;
  out.area = LookOf(C(chartSpace, "spPr"), colors);
  out.plot = LookOf(C(plot, "spPr"), colors);
  TextLook(C(chartSpace, "txPr"), colors, &out.textSize, &out.textColor);
  const std::optional<draw::Color> spaceColor = out.textColor;

  // The chart groups, and the axes by id.
  std::map<std::string, pugi::xml_node> axes;
  std::vector<std::pair<pugi::xml_node, std::vector<std::string>>> groups;  // a group and its axis ids
  bool unknown = false;
  size_t budget = kMaxAllPoints, seriesCount = 0;
  ForEachChild(plot, [&](pugi::xml_node c) {
    if (NamespaceOf(c) != kChart) return true;
    const std::string_view k = LocalNameOf(c);
    if (k == "catAx" || k == "valAx" || k == "dateAx" || k == "serAx") {
      axes[std::string(Val(C(c, "axId")))] = c;
      return true;
    }
    if (k == "layout" || k == "spPr" || k == "dTable" || k == "extLst") return true;
    Group g;
    if (k == "barChart" || k == "bar3DChart") g.type = Val(C(c, "barDir")) == "bar" ? Type::Bar : Type::Column;
    else if (k == "lineChart" || k == "line3DChart") g.type = Type::Line;
    else if (k == "pieChart" || k == "pie3DChart" || k == "ofPieChart") g.type = Type::Pie;
    else if (k == "doughnutChart") g.type = Type::Doughnut;
    else if (k == "areaChart" || k == "area3DChart") g.type = Type::Area;
    else if (k == "scatterChart") g.type = Type::Scatter;
    else {
      unknown = true;  // radar, stock, bubble, surface…
      return false;
    }
    if (out.groups.size() >= 16) return true;
    const std::string_view grouping = Val(C(c, "grouping"));
    g.grouping = grouping == "stacked" ? Grouping::Stacked : grouping == "percentStacked" ? Grouping::Percent : Grouping::Clustered;
    g.varyColors = On(c, "varyColors", g.type == Type::Pie || g.type == Type::Doughnut);
    g.gap = std::clamp(Number(C(c, "gapWidth"), "val", 150), 0.0, 500.0);
    g.overlap = std::clamp(Number(C(c, "overlap"), "val", g.grouping == Grouping::Clustered ? 0 : 100), -100.0, 100.0);
    g.hole = std::clamp(Number(C(c, "holeSize"), "val", 50), 1.0, 90.0);
    g.firstAngle = std::clamp(Number(C(c, "firstSliceAng"), "val", 0), 0.0, 360.0);
    g.marker = On(c, "marker", false);
    const std::string_view style = Val(C(c, "scatterStyle"));
    g.lines = style != "marker" && style != "none";
    std::optional<bool> val, pct, cat;
    LabelsOf(C(c, "dLbls"), val, pct, cat);
    g.labels = val.value_or(false);
    g.percent = pct.value_or(false);
    g.categoryLabels = cat.value_or(false);
    std::vector<std::string> ids;
    ForEachChild(c, [&](pugi::xml_node s) {
      if (NamespaceOf(s) != kChart) return true;
      if (LocalNameOf(s) == "axId") ids.emplace_back(Val(s));
      else if (LocalNameOf(s) == "ser" && seriesCount < 255) {
        g.series.push_back(SeriesOf(s, g.type, colors, budget));
        ++seriesCount;
      }
      return true;
    });
    if (!g.series.empty()) {
      groups.push_back({c, std::move(ids)});
      out.groups.push_back(std::move(g));
    }
    return true;
  });
  if (unknown || out.groups.empty()) return false;

  // Axes: the first group's category and value axes; a group on another value axis is secondary.
  const auto axis = [&](const std::string& id) { auto it = axes.find(id); return it == axes.end() ? pugi::xml_node() : it->second; };
  const std::vector<std::string>& first = groups.front().second;
  if (first.size() >= 2) {
    pugi::xml_node a = axis(first[0]), b = axis(first[1]);
    // Scatter has two value axes: x is the one across the bottom (or top).
    const bool scatter = out.groups.front().type == Type::Scatter;
    if (scatter) {
      const std::string_view pos = Val(C(a, "axPos"));
      if (pos == "l" || pos == "r") std::swap(a, b);
    }
    // Linked to the data, a percent-stacked group's axis shows percentages.
    const auto valueFormat = [&](size_t i) {
      return out.groups[i].grouping == Grouping::Percent ? std::string("0%") : SourceFormat(groups[i].first, scatter ? "yVal" : "val");
    };
    if (a) out.category = AxisOf(a, scatter ? SourceFormat(groups.front().first, "xVal") : std::string());
    if (b) out.value = AxisOf(b, valueFormat(0));
    for (size_t i = 1; i < out.groups.size(); ++i) {
      const std::vector<std::string>& ids = groups[i].second;
      if (ids.size() >= 2 && ids[1] != first[1] && axis(ids[1])) {
        out.groups[i].secondary = true;
        out.secondary = AxisOf(axis(ids[1]), valueFormat(i));
      }
    }
    // Text that doesn't say its color everywhere takes the axes' or legend's (Office 2013 and later
    // give each its own).
    for (pugi::xml_node n : {b, a, C(chart, "legend")})
      if (!out.textColor) TextLook(C(n, "txPr"), colors, nullptr, &out.textColor);
  }

  // The title: its own text, or for a title without any, the series' name or "Chart Title".
  if (pugi::xml_node t = C(chart, "title")) {
    pugi::xml_node tx = C(t, "tx");
    if (tx) out.title = TextOf(tx);
    else if (out.groups.size() == 1 && out.groups.front().series.size() == 1) out.title = out.groups.front().series.front().name;
    if (out.title.empty() && !tx) out.title = L"Chart Title";
    // Bold and in the chart's text color unless its own text (or else its c:txPr) says otherwise.
    out.titleSize = out.textSize * 14 / 9;
    out.titleColor = spaceColor;
    if (pugi::xml_node rich = C(tx, "rich")) TextLook(rich, colors, &out.titleSize, &out.titleColor, &out.titleBold);
    else TextLook(C(t, "txPr"), colors, &out.titleSize, &out.titleColor, &out.titleBold);
  }
  if (pugi::xml_node legend = C(chart, "legend")) {
    const std::string_view pos = Val(C(legend, "legendPos"));
    out.legend = pos == "b" ? LegendPos::Bottom : pos == "t" ? LegendPos::Top : pos == "l" ? LegendPos::Left : LegendPos::Right;
  }
  return true;
}

}  // namespace fw::chart
