// Reading c:chartSpace (fwchart): the plot area's chart groups, their series with the values Office
// cached with them (or, in a workbook, its cells'), the axes, the title and the legend. Untrusted input: counts are capped, and a
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

// A reference's (c:numRef, c:strRef...) cells, from the workbook when there's a data source.
bool FromCells(pugi::xml_node ref, DataSource* source, std::vector<std::string>& texts, std::vector<double>& numbers,
               std::string* format = nullptr) {
  pugi::xml_node f = C(ref, "f");
  if (!source || !f) return false;
  texts.clear();
  numbers.clear();
  if (!source->Cells(f.text().get(), texts, numbers, format)) return false;
  if (texts.size() > kMaxPoints) texts.resize(kMaxPoints);
  numbers.resize(texts.size(), kNaN);
  return true;
}

std::vector<double> Numbers(pugi::xml_node data, DataSource* source) {
  std::vector<double> out;
  if (pugi::xml_node f = C(C(data, "numRef"), "f"); source && f && source->Numbers(f.text().get(), out)) {
    if (out.size() > kMaxPoints) out.resize(kMaxPoints);
    return out;
  }
  out.clear();
  for (const std::string& t : CachedText(CacheOf(data, "numRef", "numCache", "numLit")))
    out.push_back(t.empty() ? kNaN : Number(t, kNaN));
  return out;
}

// c:tx: rich text (paragraphs joined by spaces), a cached string (or the cells'), or a literal.
std::wstring TextOf(pugi::xml_node tx, DataSource* source = nullptr) {
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
    std::vector<std::string> pts;
    std::vector<double> numbers;
    if (!FromCells(ref, source, pts, numbers)) pts = CachedText(C(ref, "strCache"));
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

// A preset dash (a:prstDash) as dashes and gaps in multiples of the line's width; empty: solid.
std::vector<float> PresetDashes(std::string_view v) {
  if (v == "dot" || v == "sysDot") return {1, 1};
  if (v == "dash") return {4, 3};
  if (v == "lgDash") return {8, 3};
  if (v == "dashDot") return {4, 3, 1, 3};
  if (v == "lgDashDot") return {8, 3, 1, 3};
  if (v == "lgDashDotDot") return {8, 3, 1, 3, 1, 3};
  if (v == "sysDash") return {3, 1};
  if (v == "sysDashDot") return {3, 1, 1, 1};
  if (v == "sysDashDotDot") return {3, 1, 1, 1, 1, 1};
  return {};
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
    if (pugi::xml_node d = A(ln, "prstDash")) l.dashes = PresetDashes(Val(d));
    else if (pugi::xml_node c = A(ln, "custDash"))
      ForEachChild(c, [&](pugi::xml_node ds) {
        if (LocalNameOf(ds) != "ds") return true;
        l.dashes.push_back(static_cast<float>(std::clamp(Number(ds, "d", 100000) / 100000, 0.01, 1000.0)));
        l.dashes.push_back(static_cast<float>(std::clamp(Number(ds, "sp", 100000) / 100000, 0.01, 1000.0)));
        return l.dashes.size() < 64;
      });
  }
  return l;
}

// What a c:dLbls shows; what it doesn't say stays nullopt.
struct LabelParts {
  std::optional<bool> val, pct, cat, ser, key;
  std::optional<std::wstring> separator;
};

// A data label's own number format (c:numFmt with sourceLinked off); empty when it follows the data.
std::string LabelFormat(pugi::xml_node dLbls) {
  pugi::xml_node f = C(dLbls, "numFmt");
  return f && !Bool(f, "sourceLinked", false) ? f.attribute("formatCode").value() : "";
}

LabelParts LabelsOf(pugi::xml_node dLbls) {
  LabelParts l;
  if (!dLbls) return l;
  if (On(dLbls, "delete", false)) {
    l.val = l.pct = l.cat = l.ser = l.key = false;
    return l;
  }
  const auto flag = [&](const char* name, std::optional<bool>& out) {
    if (C(dLbls, name)) out = On(dLbls, name, false);
  };
  flag("showVal", l.val);
  flag("showPercent", l.pct);
  flag("showCatName", l.cat);
  flag("showSerName", l.ser);
  flag("showLegendKey", l.key);
  if (pugi::xml_node s = C(dLbls, "separator")) l.separator = Wide(s.text().get());
  return l;
}

Series SeriesOf(pugi::xml_node ser, Type type, const draw::ColorContext& colors, size_t& budget, DataSource* source) {
  Series s;
  if (pugi::xml_node tx = C(ser, "tx")) s.name = TextOf(tx, source);
  s.look = LookOf(C(ser, "spPr"), colors);
  ForEachChild(ser, [&](pugi::xml_node dPt) {
    if (LocalNameOf(dPt) == "dPt" && NamespaceOf(dPt) == kChart && s.points.size() < kMaxPoints) {
      const double idx = Number(C(dPt, "idx"), "val", -1);
      if (idx >= 0 && idx < double(kMaxPoints)) {
        s.points.push_back({static_cast<uint32_t>(idx), LookOf(C(dPt, "spPr"), colors)});
        if (pugi::xml_node e = C(dPt, "explosion"))
          s.explosions.push_back({static_cast<uint32_t>(idx), std::clamp(Number(e, "val", 0), 0.0, 400.0)});
      }
    }
    return true;
  });
  const bool scatter = type == Type::Scatter || type == Type::Bubble;
  pugi::xml_node cat = C(ser, scatter ? "xVal" : "cat");
  pugi::xml_node catRef = C(cat, "strRef");
  if (!catRef) catRef = C(cat, "numRef");
  if (!catRef) catRef = C(cat, "multiLvlStrRef");
  std::vector<std::string> texts;
  std::vector<double> numbers;
  if (scatter && (C(cat, "numRef") || C(cat, "numLit"))) {
    s.xs = Numbers(cat, source);
  } else if (FromCells(catRef, source, texts, numbers)) {
    for (const std::string& t : texts) s.categories.push_back(Wide(t));
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
  s.values = Numbers(C(ser, scatter ? "yVal" : "val"), source);
  // The chart's points are capped as a whole: a series past the budget is cut short.
  const size_t n = std::min(s.values.size(), budget);
  s.values.resize(n);
  if (s.xs.size() > n) s.xs.resize(n);
  if (type == Type::Bubble) s.sizes = Numbers(C(ser, "bubbleSize"), source), s.sizes.resize(n, kNaN);
  budget -= n;
  if (pugi::xml_node m = C(ser, "marker")) {
    s.symbol = Val(C(m, "symbol"));
    if (s.symbol.size() > 16) s.symbol.clear();
    if (pugi::xml_node c = C(m, "symbol")) s.marker = s.symbol != "none";
    s.markerSize = static_cast<float>(std::clamp(Number(C(m, "size"), "val", 7), 2.0, 72.0));
    s.markerLook = LookOf(C(m, "spPr"), colors);
  }
  s.smooth = (type == Type::Line || scatter) && On(ser, "smooth", true);
  const LabelParts l = LabelsOf(C(ser, "dLbls"));
  s.labels = l.val, s.percent = l.pct, s.categoryLabels = l.cat, s.seriesNames = l.ser, s.keys = l.key;
  s.separator = l.separator;
  s.labelFormat = LabelFormat(C(ser, "dLbls"));
  s.labelPos = Val(C(C(ser, "dLbls"), "dLblPos"));
  ForEachChild(C(ser, "dLbls"), [&](pugi::xml_node d) {
    if (LocalNameOf(d) != "dLbl" || s.pointLabels.size() >= 256) return true;
    Series::PointLabel pl;
    pl.index = static_cast<uint32_t>(std::clamp(Number(C(d, "idx"), "val", 0.0), 0.0, 1e6));
    pl.deleted = On(d, "delete", false);
    const LabelParts own = LabelsOf(d);
    pl.value = own.val, pl.seriesName = own.ser, pl.category = own.cat, pl.percent = own.pct;
    pl.separator = own.separator;
    if (pugi::xml_node m = C(C(d, "layout"), "manualLayout")) {
      pl.dx = static_cast<float>(std::clamp(Number(C(m, "x"), "val", 0.0), -2.0, 2.0));
      pl.dy = static_cast<float>(std::clamp(Number(C(m, "y"), "val", 0.0), -2.0, 2.0));
    }
    const Look box = LookOf(C(d, "spPr"), colors);
    if (!box.noFill) pl.fill = box.fill;
    pl.format = LabelFormat(d);
    if (pl.format.empty()) pl.format = C(CacheOf(C(ser, "val"), "numRef", "numCache", "numLit"), "formatCode").text().get();
    // Office 2013's own box size (c15:layout in the label's extensions).
    for (pugi::xml_node e = C(C(d, "extLst"), "ext"); e; e = e.next_sibling())
      for (pugi::xml_node k = e.first_child(); k; k = k.next_sibling())
        if (LocalNameOf(k) == "layout")
          if (pugi::xml_node m = C(k, "manualLayout")) {
            pl.w = static_cast<float>(std::clamp(Number(C(m, "w"), "val", 0.0), 0.0, 1.0));
            pl.h = static_cast<float>(std::clamp(Number(C(m, "h"), "val", 0.0), 0.0, 1.0));
          }
    // Its text's insets: its own a:bodyPr's, else its series' labels'.
    const auto insets = [&](pugi::xml_node body) {
      static const char* const kNames[] = {"lIns", "tIns", "rIns", "bIns"};
      for (int k = 0; k < 4; ++k)
        if (pl.insets[k] < 0 && body.attribute(kNames[k])) pl.insets[k] = std::clamp(EmuToPt(Number(body, kNames[k], 0)), 0.0f, 100.0f);
    };
    insets(A(C(C(d, "tx"), "rich"), "bodyPr"));
    insets(A(C(d, "txPr"), "bodyPr"));
    insets(A(C(C(ser, "dLbls"), "txPr"), "bodyPr"));
    pl.pos = Val(C(d, "dLblPos"));
    if (pl.pos.size() > 8) pl.pos.clear();
    TextLook(C(d, "txPr"), colors, &pl.size, &pl.color);
    if (pugi::xml_node rich = C(C(d, "tx"), "rich")) {
      TextLook(rich, colors, &pl.size, &pl.color);
      std::wstring text;
      ForEachChild(rich, [&](pugi::xml_node p) {
        if (LocalNameOf(p) != "p") return true;
        std::wstring line;
        ForEachChild(p, [&](pugi::xml_node r) {
          if (LocalNameOf(r) == "fld" && std::string_view(r.attribute("type").value()) == "SERIESNAME") line += s.name;
          else if (LocalNameOf(r) == "r" || LocalNameOf(r) == "fld") line += Wide(A(r, "t").text().get());
          return line.size() < 1000;
        });
        text += (text.empty() ? L"" : L"\n") + line;
        return text.size() < 1000;
      });
      pl.text = text;
    }
    s.pointLabels.push_back(std::move(pl));
    return true;
  });
  if (pugi::xml_node e = C(ser, "explosion")) s.explosion = std::clamp(Number(e, "val", 0), 0.0, 400.0);
  return s;
}

// The format code of a group's first series' numbers under `tag` (c:val, c:xVal, c:yVal): their
// first cell's, or the cache's.
std::string SourceFormat(pugi::xml_node group, const char* tag, DataSource* source) {
  std::vector<std::string> texts;
  std::vector<double> numbers;
  std::string format;
  if (FromCells(C(C(C(group, "ser"), tag), "numRef"), source, texts, numbers, &format)) return format;
  return C(CacheOf(C(C(group, "ser"), tag), "numRef", "numCache", "numLit"), "formatCode").text().get();
}

// An element's c:layout/c:manualLayout in edge mode, as fractions of the chart (factor mode, an
// offset from where the chart would put it, isn't followed); `inner`: layoutTarget inner.
std::optional<draw::Rect> ManualLayout(pugi::xml_node owner, bool* inner = nullptr) {
  pugi::xml_node m = C(C(owner, "layout"), "manualLayout");
  if (!m || Val(C(m, "xMode")) != "edge" || Val(C(m, "yMode")) != "edge") return std::nullopt;
  const double x = Number(C(m, "x"), "val", kNaN), y = Number(C(m, "y"), "val", kNaN);
  const double w = Number(C(m, "w"), "val", kNaN), h = Number(C(m, "h"), "val", kNaN);
  if (!(x >= -0.5 && x <= 1.5 && y >= -0.5 && y <= 1.5 && w > 0 && w <= 2 && h > 0 && h <= 2)) return std::nullopt;
  if (inner) *inner = Val(C(m, "layoutTarget")) == "inner";
  return draw::Rect{static_cast<float>(x), static_cast<float>(y), static_cast<float>(x + w), static_cast<float>(y + h)};
}

// An axis; `source` is the format its numbers take when they're linked to the data's.
Axis AxisOf(pugi::xml_node ax, const std::string& source, const draw::ColorContext& colors) {
  Axis a;
  a.shown = !On(ax, "delete", false) && Val(C(ax, "tickLblPos")) != "none";
  a.gridlines = static_cast<bool>(C(ax, "majorGridlines"));
  a.grid = LookOf(C(C(ax, "majorGridlines"), "spPr"), colors);
  a.line = LookOf(C(ax, "spPr"), colors);
  if (pugi::xml_node t = C(ax, "majorTickMark")) {
    const std::string_view v = Val(t);
    a.ticks = v == "none" ? Axis::Tick::None : v == "out" ? Axis::Tick::Out : v == "in" ? Axis::Tick::In : Axis::Tick::Cross;
  }
  if (pugi::xml_node f = C(ax, "numFmt")) a.format = Bool(f, "sourceLinked", false) ? source : f.attribute("formatCode").value();
  pugi::xml_node scaling = C(ax, "scaling");
  if (pugi::xml_node m = C(scaling, "min")) a.min = Number(m, "val", kNaN);
  if (pugi::xml_node m = C(scaling, "max")) a.max = Number(m, "val", kNaN);
  if (a.min && !(*a.min == *a.min)) a.min.reset();
  if (a.max && !(*a.max == *a.max)) a.max.reset();
  a.reversed = Val(C(scaling, "orientation")) == "maxMin";
  if (pugi::xml_node b = C(scaling, "logBase")) {
    const double base = Number(b, "val", 0.0);
    if (base >= 2 && base <= 1000) a.logBase = base;
  }
  if (pugi::xml_node cb = C(ax, "crossBetween")) a.between = Val(cb) != "midCat";
  a.labelsHigh = Val(C(ax, "tickLblPos")) == "high";
  if (pugi::xml_node s = C(ax, "tickLblSkip")) a.labelSkip = static_cast<int>(std::clamp(Number(s, "val", 0.0), 0.0, 1e6));
  if (pugi::xml_node t = C(ax, "title")) {
    a.title = TextOf(C(t, "tx"));
    a.titleOverlay = On(t, "overlay", false);
  }
  return a;
}

// Office 2010 and later write c14:style in an mc:AlternateContent (c:style its fallback); a chart
// without it keeps Office 2007's look.
bool HasC14Style(pugi::xml_node chartSpace) {
  constexpr std::string_view kC14 = "http://schemas.microsoft.com/office/drawing/2007/8/2/chart";
  for (pugi::xml_node ac = chartSpace.first_child(); ac; ac = ac.next_sibling()) {
    if (!IsElement(ac, fw::pkg::mce::kNamespace, "AlternateContent")) continue;
    for (pugi::xml_node choice = ac.first_child(); choice; choice = choice.next_sibling())
      for (pugi::xml_node s = choice.first_child(); s; s = s.next_sibling())
        if (s.type() == pugi::node_element && LocalNameOf(s) == "style" && NamespaceOf(s) == kC14) return true;
  }
  return false;
}

}  // namespace

bool Read(pugi::xml_node chartSpace, const draw::ColorContext& colors, Chart& out, DataSource* data, bool presentation) {
  out = Chart{};
  if (!chartSpace || LocalNameOf(chartSpace) != "chartSpace" || NamespaceOf(chartSpace) != kChart) return false;
  pugi::xml_node chart = C(chartSpace, "chart");
  pugi::xml_node plot = C(chart, "plotArea");
  if (!plot) return false;
  out.plotLayout = ManualLayout(plot, &out.plotInner);
  out.area = LookOf(C(chartSpace, "spPr"), colors);
  // In a workbook (where `data` comes from), Excel rounds the corners when the chart doesn't say,
  // and keeps Office 2007's look for a chart without c14:style; Word does neither (their PDFs).
  out.roundedCorners = On(chartSpace, "roundedCorners", data != nullptr);
  out.styled = HasC14Style(chartSpace);
  out.presentation = presentation;
  out.legacy = (data || presentation) && !out.styled;
  out.workbook = data != nullptr;
  out.plot = LookOf(C(plot, "spPr"), colors);
  float size = -1;  // stated by the chart, or else (below) by its axes or legend
  TextLook(C(chartSpace, "txPr"), colors, &size, &out.textColor);
  const float spaceSize = size;
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
    else if (k == "radarChart" && out.groups.empty()) g.type = Type::Radar;
    else if (k == "bubbleChart" && out.groups.empty()) g.type = Type::Bubble;
    else {
      unknown = true;  // stock, surface…
      return false;
    }
    if (out.groups.size() >= 16) return true;
    const std::string_view grouping = Val(C(c, "grouping"));
    g.grouping = grouping == "stacked" ? Grouping::Stacked : grouping == "percentStacked" ? Grouping::Percent : Grouping::Clustered;
    // Unstated, Office varies a lone series' bars and slices (PowerPoint's PDFs).
    g.varyColors = On(c, "varyColors", g.type == Type::Column || g.type == Type::Bar || g.type == Type::Pie ||
                                           g.type == Type::Doughnut);
    g.gap = std::clamp(Number(C(c, "gapWidth"), "val", 150), 0.0, 500.0);
    g.overlap = std::clamp(Number(C(c, "overlap"), "val", g.grouping == Grouping::Clustered ? 0 : 100), -100.0, 100.0);
    g.hole = std::clamp(Number(C(c, "holeSize"), "val", 50), 1.0, 90.0);
    g.firstAngle = std::clamp(Number(C(c, "firstSliceAng"), "val", 0), 0.0, 360.0);
    g.marker = On(c, "marker", false);
    g.bubbleScale = std::clamp(Number(C(c, "bubbleScale"), "val", 100), 0.0, 300.0);
    g.bubbleWidth = Val(C(c, "sizeRepresents")) == "w";
    if (g.type == Type::Radar) {
      const std::string_view radar = Val(C(c, "radarStyle"));
      g.marker = radar == "marker";
      g.filled = radar == "filled";
    }
    const std::string_view style = Val(C(c, "scatterStyle"));
    g.lines = style != "marker" && style != "none";
    const LabelParts l = LabelsOf(C(c, "dLbls"));
    g.labelFormat = LabelFormat(C(c, "dLbls"));
    g.labelPos = Val(C(C(c, "dLbls"), "dLblPos"));
    g.labels = l.val.value_or(false);
    g.percent = l.pct.value_or(false);
    g.categoryLabels = l.cat.value_or(false);
    g.seriesNames = l.ser.value_or(false);
    g.keys = l.key.value_or(false);
    g.separator = l.separator;
    std::vector<std::string> ids;
    ForEachChild(c, [&](pugi::xml_node s) {
      if (NamespaceOf(s) != kChart) return true;
      if (LocalNameOf(s) == "axId") ids.emplace_back(Val(s));
      else if (LocalNameOf(s) == "ser" && seriesCount < 255) {
        g.series.push_back(SeriesOf(s, g.type, colors, budget, data));
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
    const bool scatter = out.groups.front().type == Type::Scatter || out.groups.front().type == Type::Bubble;
    if (scatter) {
      const std::string_view pos = Val(C(a, "axPos"));
      if (pos == "l" || pos == "r") std::swap(a, b);
    }
    // Linked to the data, a percent-stacked group's axis shows percentages.
    const auto valueFormat = [&](size_t i) {
      return out.groups[i].grouping == Grouping::Percent ? std::string("0%")
                                                          : SourceFormat(groups[i].first, scatter ? "yVal" : "val", data);
    };
    if (a) out.category = AxisOf(a, scatter ? SourceFormat(groups.front().first, "xVal", data) : std::string(), colors);
    if (b) out.value = AxisOf(b, valueFormat(0), colors);
    for (size_t i = 1; i < out.groups.size(); ++i) {
      const std::vector<std::string>& ids = groups[i].second;
      if (ids.size() >= 2 && ids[1] != first[1] && axis(ids[1])) {
        out.groups[i].secondary = true;
        out.secondary = AxisOf(axis(ids[1]), valueFormat(i), colors);
      }
    }
    // Text that doesn't say its color everywhere takes the axes' or legend's (Office 2013 and later
    // give each its own).
    for (pugi::xml_node n : {b, a, C(chart, "legend")}) {
      if (!out.textColor) TextLook(C(n, "txPr"), colors, nullptr, &out.textColor);
      if (size < 0) TextLook(C(n, "txPr"), colors, &size, nullptr);
    }
  }
  // Or the legend's, or the data labels' (pie charts have no axes). Text no part states the size
  // of is Office 2007's 10 points (Office 2013 and later state 9).
  if (size < 0) TextLook(C(C(chart, "legend"), "txPr"), colors, &size, nullptr);
  for (size_t i = 0; size < 0 && i < groups.size(); ++i) {
    TextLook(C(C(groups[i].first, "dLbls"), "txPr"), colors, &size, nullptr);
    if (size < 0) TextLook(C(C(C(groups[i].first, "ser"), "dLbls"), "txPr"), colors, &size, nullptr);
  }
  if (size > 0) out.textSize = size;

  // The title: its own text, or for a title without any, the series' name or "Chart Title".
  if (pugi::xml_node t = C(chart, "title")) {
    pugi::xml_node tx = C(t, "tx");
    if (tx) out.title = TextOf(tx, data);
    else if (out.groups.size() == 1 && out.groups.front().series.size() == 1) out.title = out.groups.front().series.front().name;
    if (out.title.empty() && !tx) out.title = L"Chart Title";
    // Bold and in the chart's text color unless its own text (or else its c:txPr) says otherwise.
    // Unstated, Office 2007's look (Excel's PDFs) makes it the chart's text size, or 1.2 times
    // what the chart's c:txPr states; Office 2010's, 18 points; Office 2013 and later state theirs.
    // On a slide, 1.2 times the chart's stated text size whatever its look (poi line chart: 21.6 at 18).
    if (out.legacy || (presentation && spaceSize > 0)) out.titleSize = spaceSize > 0 ? spaceSize * 1.2f : 10;
    else out.titleSize = size > 0 ? size * 14 / 9 : 18;
    out.titleColor = spaceColor;
    out.titleOverlay = On(t, "overlay", false);
    if (pugi::xml_node rich = C(tx, "rich")) {
      // Text with no properties at all (no a:pPr, a:rPr) is DrawingML's own default in Office
      // 2007's look: 18-point Arial, not bold (Excel's PDFs; PowerPoint's, in Office 2010's look
      // too).
      pugi::xml_node p = A(rich, "p");
      if ((out.legacy || presentation) && !A(p, "pPr") && !A(A(p, "r"), "rPr")) {
        out.titleSize = 18;
        out.titleBold = false;
        out.titleFont = L"Arial";
      }
      TextLook(rich, colors, &out.titleSize, &out.titleColor, &out.titleBold);
    } else {
      TextLook(C(t, "txPr"), colors, &out.titleSize, &out.titleColor, &out.titleBold);
    }
  } else if (C(chart, "autoTitleDeleted") && !On(chart, "autoTitleDeleted", true) && out.groups.size() == 1 &&
             out.groups.front().series.size() == 1) {
    // No title, but the automatic one not deleted: a single series' name, bold, at the size of a
    // title without text (PowerPoint's PDFs).
    out.title = out.groups.front().series.front().name;
    out.titleSize = out.legacy || (presentation && spaceSize > 0) ? (spaceSize > 0 ? spaceSize * 1.2f : 10) : size > 0 ? size * 14 / 9 : 18;
    out.titleColor = spaceColor;
  }
  if (pugi::xml_node legend = C(chart, "legend")) {
    const std::string_view pos = Val(C(legend, "legendPos"));
    out.legend = pos == "b" ? LegendPos::Bottom : pos == "t" ? LegendPos::Top : pos == "l" ? LegendPos::Left : LegendPos::Right;
    out.legendOverlay = On(legend, "overlay", false);
    out.legendLayout = ManualLayout(legend);
    out.legendLook = LookOf(C(legend, "spPr"), colors);
    for (pugi::xml_node e = legend.first_child(); e; e = e.next_sibling())
      if (LocalNameOf(e) == "legendEntry" && On(e, "delete", false))
        out.hiddenEntries.push_back(static_cast<uint32_t>(std::max(0.0, Number(C(e, "idx"), "val", 0.0))));
  }
  return true;
}

}  // namespace fw::chart
