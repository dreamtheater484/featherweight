// OpenDocument presentations as .pptx packages (P3, p3-files). The converter reads content.xml and
// styles.xml once and writes a presentation the .pptx reader opens: one slide master (the default
// theme, a white background, no shapes), one slide layout for each master page the slides use (its
// background, its shapes, its presentation objects as placeholders), and the slides. Every
// property is written on the shape, paragraph and run it applies to (DrawingML's inheritance is
// left unused), since ODF's automatic styles, which PowerPoint writes resolved, say it all there.

#include "slide/odp/Odp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "draw/Images.h"
#include "draw/Theme.h"
#include "pkg/Odf.h"
#include "pkg/Snapshot.h"
#include "pkg/XmlDom.h"
#include "pkg/Zip.h"
#include "slide/odp/OdpGeometry.h"

namespace fw::slide::odp {
namespace {

using pugi::xml_node;
using Chain = std::vector<xml_node>;  // a style, its parents, ..., the family's default: most specific first
namespace odf = pkg::odf;

constexpr std::string_view kDecl = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";
constexpr std::string_view kNs =
    " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
    " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
    " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"";
constexpr std::string_view kRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
constexpr std::string_view kCt = "application/vnd.openxmlformats-officedocument.presentationml.";
constexpr std::string_view kRelsOpen = "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
constexpr const char* kText = "style:text-properties";
constexpr const char* kPara = "style:paragraph-properties";
constexpr const char* kGraphic = "style:graphic-properties";
constexpr const char* kPage = "style:drawing-page-properties";
constexpr int kMaxDepth = 24;         // groups and lists within one another
constexpr size_t kMaxSlides = 5000;
constexpr int64_t kMaxCoord = 200000000;  // EMU, about 5.5 m

std::string_view Attr(xml_node n, const char* name) { return n.attribute(name).value(); }
std::string Esc(std::string_view s) { return odf::Escape(s); }
std::string N(int64_t v) { return std::to_string(v); }

int64_t Emu(std::string_view v) {
  const auto p = odf::Points(v);
  return p ? std::clamp<int64_t>(std::llround(*p * 12700), -kMaxCoord, kMaxCoord) : 0;
}
// "50%" as 50; -1 when it isn't a percentage.
double Percent(std::string_view v) {
  if (v.empty() || v.back() != '%') return -1;
  try {
    const double d = std::stod(std::string(v.substr(0, v.size() - 1)));
    return std::isfinite(d) ? std::clamp(d, -1e6, 1e6) : -1;
  } catch (...) {
    return -1;
  }
}
double Opacity(std::string_view v) {
  const double p = Percent(v);
  return p < 0 ? 1 : std::clamp(p / 100, 0.0, 1.0);
}

std::string Srgb(const std::string& hex, double opacity = 1) {
  std::string s = "<a:srgbClr val=\"" + hex + "\"";
  if (opacity >= 0.9995) return s + "/>";
  return s + "><a:alpha val=\"" + N(std::llround(opacity * 100000)) + "\"/></a:srgbClr>";
}
std::string Solid(const std::string& hex, double opacity = 1) { return "<a:solidFill>" + Srgb(hex, opacity) + "</a:solidFill>"; }

// A color scaled by an intensity (ODF gradients' start and end intensity).
std::string Scaled(std::string hex, double intensity) {
  if (intensity >= 0.9995) return hex;
  static const char* digits = "0123456789ABCDEF";
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    const int v = std::stoi(hex.substr(i, 2), nullptr, 16);
    const int s = std::clamp(static_cast<int>(std::lround(v * intensity)), 0, 255);
    hex[i] = digits[s >> 4], hex[i + 1] = digits[s & 15];
  }
  return hex;
}

// The first family of a font list, without quotes ("'Segoe UI', sans-serif" -> "Segoe UI").
std::string FirstFamily(std::string_view v) {
  v = v.substr(0, v.find(','));
  while (!v.empty() && (v.front() == ' ' || v.front() == '\'' || v.front() == '"')) v.remove_prefix(1);
  while (!v.empty() && (v.back() == ' ' || v.back() == '\'' || v.back() == '"')) v.remove_suffix(1);
  return std::string(v);
}

// A style property: the first of the chain's styles that sets it.
std::string_view Prop(const Chain& chain, const char* element, const char* attr) {
  for (xml_node s : chain)
    if (pugi::xml_attribute a = s.child(element).attribute(attr)) return a.value();
  return {};
}

// The styles of both parts. Automatic styles are the part's own: a slide (content.xml) and a
// master page (styles.xml) can each have an "a1".
class Styles {
 public:
  void Add(xml_node container, char scope) {
    for (xml_node n : container.children()) {
      const std::string_view k = n.name();
      if (k == "style:style") map_.emplace(Key(scope, 's', Attr(n, "style:family"), Attr(n, "style:name")), n);
      else if (k == "style:default-style") map_.emplace(Key('c', 'D', Attr(n, "style:family"), {}), n);
      else if (k == "text:list-style") map_.emplace(Key(scope, 'l', {}, Attr(n, "style:name")), n);
      else if (k == "draw:gradient") map_.emplace(Key('c', 'g', {}, Attr(n, "draw:name")), n);
      else if (k == "draw:fill-image") map_.emplace(Key('c', 'i', {}, Attr(n, "draw:name")), n);
      else if (k == "draw:stroke-dash") map_.emplace(Key('c', 'd', {}, Attr(n, "draw:name")), n);
      else if (k == "draw:marker") map_.emplace(Key('c', 'k', {}, Attr(n, "draw:name")), n);
      else if (k == "style:page-layout") map_.emplace(Key('c', 'p', {}, Attr(n, "style:name")), n);
      else if (k == "style:font-face") map_.emplace(Key('c', 'f', {}, Attr(n, "style:name")), n);
      else if (k == "style:master-page") map_.emplace(Key('c', 'M', {}, Attr(n, "style:name")), n);
    }
  }
  // A style of the part (`master`: styles.xml's automatic styles, else content.xml's), or a common one.
  xml_node Find(char kind, std::string_view family, std::string_view name, bool master) const {
    if (name.empty()) return {};
    if (kind == 's' || kind == 'l') {
      if (auto it = map_.find(Key(master ? 'm' : 'a', kind, family, name)); it != map_.end()) return it->second;
    }
    auto it = map_.find(Key('c', kind, family, name));
    return it == map_.end() ? xml_node() : it->second;
  }
  xml_node Named(char kind, std::string_view name) const { return Find(kind, {}, name, false); }
  // `name` and its parents, most specific first.
  void Add(Chain& out, std::string_view family, std::string_view name, bool master) const {
    xml_node s = Find('s', family, name, master);
    for (int guard = 0; s && guard < 16; ++guard) {
      out.push_back(s);
      s = Find('s', family, Attr(s, "style:parent-style-name"), master);
    }
  }
  void AddDefault(Chain& out, std::string_view family) const {
    if (auto it = map_.find(Key('c', 'D', family, {})); it != map_.end()) out.push_back(it->second);
  }
  std::string Font(std::string_view name) const {
    xml_node f = Named('f', name);
    return f ? FirstFamily(Attr(f, "svg:font-family")) : FirstFamily(name);
  }

 private:
  static std::string Key(char scope, char kind, std::string_view family, std::string_view name) {
    std::string k{scope, kind};
    k += family;
    k += '\n';
    k += name;
    return k;
  }
  std::unordered_map<std::string, xml_node> map_;
};

// One part being written: its relationships and its shapes' ids.
struct Part {
  std::string rels;
  int nextRel = 1;
  uint32_t nextId = 2;
  bool master = false;  // a master page: styles.xml's automatic styles
  int bodies = 0;       // placeholders numbered so far (idx)
  std::map<std::string, std::string> ids;

  std::string Rel(std::string_view type, std::string_view target, bool external = false) {
    std::string key = std::string(type) + '\n' + std::string(target);
    if (auto it = ids.find(key); it != ids.end()) return it->second;
    std::string id = "rId" + N(nextRel++);
    rels += "<Relationship Id=\"" + id + "\" Type=\"" + std::string(kRel) + std::string(type) + "\" Target=\"" +
            Esc(target) + "\"" + (external ? " TargetMode=\"External\"" : "") + "/>";
    ids.emplace(std::move(key), id);
    return id;
  }
};

struct Box {
  int64_t x = 0, y = 0, w = 0, h = 0;
  double rot = 0;
  bool flipH = false, flipV = false;
};

struct Bounds {
  int64_t l = 0, t = 0, r = 0, b = 0;
  bool any = false;
  void Add(const Box& box) {
    if (!any) l = box.x, t = box.y, r = box.x + box.w, b = box.y + box.h, any = true;
    l = std::min(l, box.x), t = std::min(t, box.y), r = std::max(r, box.x + box.w), b = std::max(b, box.y + box.h);
  }
};

// a:xfrm; a group's also maps its children's coordinates (`children`: as they are).
std::string Xfrm(const Box& b, const Bounds* children = nullptr) {
  std::string s = "<a:xfrm";
  if (const int64_t rot = std::llround(b.rot * 60000) % 21600000; rot > 0) s += " rot=\"" + N(rot) + "\"";
  if (b.flipH) s += " flipH=\"1\"";
  if (b.flipV) s += " flipV=\"1\"";
  s += "><a:off x=\"" + N(b.x) + "\" y=\"" + N(b.y) + "\"/><a:ext cx=\"" + N(b.w) + "\" cy=\"" + N(b.h) + "\"/>";
  if (children)
    s += "<a:chOff x=\"" + N(children->l) + "\" y=\"" + N(children->t) + "\"/><a:chExt cx=\"" + N(children->r - children->l) +
         "\" cy=\"" + N(children->b - children->t) + "\"/>";
  return s + "</a:xfrm>";
}

// Where a shape is: svg:x/y/width/height, then draw:transform (which moves the box's centre and
// turns it). DrawingML turns a shape about its centre.
Box ReadBox(xml_node el) {
  Box b;
  b.w = std::max<int64_t>(0, Emu(Attr(el, "svg:width")));
  b.h = std::max<int64_t>(0, Emu(Attr(el, "svg:height")));
  double x = static_cast<double>(Emu(Attr(el, "svg:x"))), y = static_cast<double>(Emu(Attr(el, "svg:y")));
  Transform t;
  if (t.Read(Attr(el, "draw:transform"))) {
    const double cx = x + b.w / 2.0, cy = y + b.h / 2.0;
    x = t.a * cx + t.b * cy + t.e - b.w / 2.0;
    y = t.c * cx + t.d * cy + t.f - b.h / 2.0;
    b.rot = t.Degrees();
  }
  b.x = std::clamp<int64_t>(std::llround(x), -kMaxCoord, kMaxCoord);
  b.y = std::clamp<int64_t>(std::llround(y), -kMaxCoord, kMaxCoord);
  return b;
}

// LibreOffice's named custom shapes (no path written) as DrawingML presets.
std::string PresetOf(std::string_view type) {
  if (type.starts_with("ooxml-")) return std::string(type.substr(6));
  static constexpr std::pair<std::string_view, std::string_view> kNames[] = {
      {"rectangle", "rect"}, {"round-rectangle", "roundRect"}, {"ellipse", "ellipse"}, {"circle", "ellipse"},
      {"isosceles-triangle", "triangle"}, {"right-triangle", "rtTriangle"}, {"diamond", "diamond"},
      {"parallelogram", "parallelogram"}, {"trapezoid", "trapezoid"}, {"hexagon", "hexagon"}, {"octagon", "octagon"},
      {"pentagon", "pentagon"}, {"star5", "star5"}, {"star4", "star4"}, {"star8", "star8"}, {"right-arrow", "rightArrow"},
      {"left-arrow", "leftArrow"}, {"up-arrow", "upArrow"}, {"down-arrow", "downArrow"}, {"heart", "heart"},
      {"smiley", "smileyFace"}, {"sun", "sun"}, {"moon", "moon"}, {"can", "can"}, {"cube", "cube"}, {"ring", "donut"},
      {"block-arc", "blockArc"}, {"cloud", "cloud"}, {"lightning", "lightningBolt"}, {"cross", "plus"},
      {"flowchart-process", "flowChartProcess"}, {"flowchart-decision", "flowChartDecision"},
      {"flowchart-terminator", "flowChartTerminator"}, {"chevron", "chevron"}, {"pentagon-right", "homePlate"}};
  for (const auto& [odf, prst] : kNames)
    if (odf == type) return std::string(prst);
  return {};
}

std::string Preset(std::string_view prst) { return "<a:prstGeom prst=\"" + std::string(prst) + "\"><a:avLst/></a:prstGeom>"; }

class Converter {
 public:
  Converter(const pkg::ZipReader& zip, const pkg::Limits& limits) : zip_(zip), limits_(limits) {}
  pkg::Status Run(std::vector<uint8_t>& pptx);

 private:
  // Shapes
  void Shapes(xml_node parent, Part& part, std::string& out, int depth, Bounds* bounds);
  void Frame(xml_node el, Part& part, std::string& out, Bounds* bounds);
  void Sp(xml_node el, xml_node text, const std::string& geometry, const Box& box, const Chain& chain, Part& part,
          std::string& out, bool textBox);
  void Picture(xml_node el, xml_node image, const Box& box, const Chain& chain, Part& part, std::string& out);
  void Table(xml_node el, xml_node table, const Box& box, Part& part, std::string& out);
  void Connector(xml_node el, Part& part, std::string& out, Bounds* bounds);
  std::string Names(xml_node el, Part& part, std::string_view fallback);
  Chain ShapeChain(xml_node el, bool master) const;

  // Fills and lines
  std::string Fill(const Chain& chain, const char* element, Part& part);
  std::string Gradient(xml_node g, double opacity) const;
  std::string Line(const Chain& chain, bool connector) const;
  std::string Effects(const Chain& chain) const;
  std::string Image(std::string_view href);  // the part name in ppt/media, or empty
  draw::Size NaturalSize(const std::string& media) const;  // points, as the picture records it; 0 when unknown

  // Text
  struct Runs;
  std::string BodyPr(const Chain& chain) const;
  void Paragraphs(xml_node parent, const Chain& shape, Part& part, xml_node list, int level, bool header, int depth,
                  std::string& out);
  void Paragraph(xml_node p, const Chain& shape, Part& part, xml_node list, int level, bool header, std::string& out);
  void Inline(xml_node node, const Chain& chain, const std::string& props, Part& part, Runs& runs, int depth);
  std::string RunProps(const Chain& chain, const char* tag, std::string_view link = {}) const;
  std::string Bullet(xml_node level, Part& part);
  xml_node ListLevel(xml_node list, int level) const;
  std::string TextBody(xml_node container, const Chain& chain, Part& part, const char* tag);

  // Pages
  std::string Background(xml_node page, bool master, Part& part);
  std::string Layout(xml_node masterPage, Part& part);
  int LayoutOf(std::string_view masterName);

  const pkg::ZipReader& zip_;
  const pkg::Limits& limits_;
  pkg::XmlDom content_, styles_;
  Styles st_;
  std::vector<std::pair<std::string, std::vector<uint8_t>>> media_;  // ppt/media name, bytes
  std::map<std::string, std::string, std::less<>> mediaOf_;          // ODF href -> media name ("" if unreadable)
  uint64_t mediaBytes_ = 0;
  std::vector<std::string> layoutMasters_;                            // master page names, by layout
  std::vector<std::pair<std::string, std::string>> layouts_;          // xml, rels
  std::vector<std::tuple<std::string, std::string, std::string>> notes_;  // xml, its slide's number, rels
  int fields_ = 0;
  uint64_t written_ = 0;  // shape XML written, against the limits
};

// ---------------------------------------------------------------------------------------------
// Fills, lines and effects

std::string Converter::Gradient(xml_node g, double opacity) const {
  const std::string style(Attr(g, "draw:style"));
  std::string start = odf::Color(Attr(g, "draw:start-color")), end = odf::Color(Attr(g, "draw:end-color"));
  if (start.empty()) start = "000000";
  if (end.empty()) end = "FFFFFF";
  start = Scaled(start, Opacity(Attr(g, "draw:start-intensity")));
  end = Scaled(end, Opacity(Attr(g, "draw:end-intensity")));
  const double border = std::clamp(Percent(Attr(g, "draw:border")), 0.0, 100.0);
  auto stop = [&](double pos, const std::string& c) {
    return "<a:gs pos=\"" + N(std::llround(std::clamp(pos, 0.0, 100.0) * 1000)) + "\">" + Srgb(c, opacity) + "</a:gs>";
  };
  std::string stops;
  std::string shape;
  if (style == "linear" || style == "axial" || style.empty()) {
    // ODF turns counter-clockwise from "start at the top"; DrawingML clockwise from "start at the left".
    std::string_view a = Attr(g, "draw:angle");
    double deg = 0;
    try {
      deg = a.empty() ? 0 : std::stod(std::string(a));
    } catch (...) {
    }
    if (a.ends_with("rad")) deg = deg * 180 / 3.14159265358979;
    else if (a.ends_with("grad")) deg = deg * 0.9;
    else if (!a.ends_with("deg")) deg /= 10;
    if (!std::isfinite(deg)) deg = 0;
    double ang = std::fmod(90 - deg, 360.0);
    if (ang < 0) ang += 360;
    if (style == "axial") stops = stop(0, start) + stop(50, end) + stop(100, start);
    else stops = stop(0, start) + (border > 0 ? stop(border, start) : "") + stop(100, end);
    shape = "<a:lin ang=\"" + N(std::llround(ang * 60000) % 21600000) + "\" scaled=\"0\"/>";
  } else {
    // Radial and the rest: the end color at the centre (draw:cx, draw:cy), the start color outside.
    double cx = Percent(Attr(g, "draw:cx")), cy = Percent(Attr(g, "draw:cy"));
    if (cx < 0) cx = 50;
    if (cy < 0) cy = 50;
    stops = stop(0, end) + (border > 0 ? stop(100 - border, start) : "") + stop(100, start);
    const bool round = style == "radial" || style == "ellipsoid";
    auto pct = [](double v) { return N(std::llround(std::clamp(v, -1000.0, 1000.0) * 1000)); };
    shape = std::string("<a:path path=\"") + (round ? "circle" : "rect") + "\"><a:fillToRect l=\"" + pct(cx) + "\" t=\"" + pct(cy) +
            "\" r=\"" + pct(100 - cx) + "\" b=\"" + pct(100 - cy) + "\"/></a:path>";
  }
  return "<a:gradFill rotWithShape=\"1\"><a:gsLst>" + stops + "</a:gsLst>" + shape + "</a:gradFill>";
}

std::string Converter::Fill(const Chain& chain, const char* element, Part& part) {
  const std::string_view kind = Prop(chain, element, "draw:fill");
  if (kind.empty()) return {};
  double opacity = Opacity(Prop(chain, element, "draw:opacity"));
  if (const double t = Percent(Prop(chain, element, "draw:transparency")); t >= 0) opacity = std::clamp(1 - t / 100, 0.0, 1.0);
  if (kind == "solid") {
    std::string c = odf::Color(Prop(chain, element, "draw:fill-color"));
    return Solid(c.empty() ? "729FCF" : c, opacity);
  }
  if (kind == "gradient") {
    if (xml_node g = st_.Named('g', Prop(chain, element, "draw:fill-gradient-name"))) return Gradient(g, opacity);
    return {};
  }
  if (kind == "bitmap") {
    xml_node i = st_.Named('i', Prop(chain, element, "draw:fill-image-name"));
    const std::string media = i ? Image(Attr(i, "xlink:href")) : std::string();
    if (media.empty()) return {};
    std::string s = "<a:blipFill dpi=\"0\" rotWithShape=\"1\"><a:blip r:embed=\"" + part.Rel("image", "../media/" + media) + "\"";
    s += opacity < 0.9995 ? "><a:alphaModFix amt=\"" + N(std::llround(opacity * 100000)) + "\"/></a:blip>" : "/>";
    if (Prop(chain, element, "style:repeat") == "repeat") s += "<a:tile tx=\"0\" ty=\"0\" sx=\"100000\" sy=\"100000\" flip=\"none\" algn=\"tl\"/>";
    else s += "<a:stretch><a:fillRect/></a:stretch>";
    return s + "</a:blipFill>";
  }
  return "<a:noFill/>";  // none, and hatches
}

std::string Converter::Line(const Chain& chain, bool connector) const {
  const std::string_view stroke = Prop(chain, kGraphic, "draw:stroke");
  if (stroke == "none") return "<a:ln><a:noFill/></a:ln>";
  if (stroke.empty() && !connector) return {};
  const int64_t w = std::max<int64_t>(0, Emu(Prop(chain, kGraphic, "svg:stroke-width")));
  std::string color = odf::Color(Prop(chain, kGraphic, "svg:stroke-color"));
  if (color.empty()) color = "000000";
  std::string s = "<a:ln w=\"" + N(stroke.empty() ? 9525 : w) + "\"";
  const std::string_view cap = Prop(chain, kGraphic, "svg:stroke-linecap");
  s += cap == "round" ? " cap=\"rnd\"" : cap == "square" ? " cap=\"sq\"" : " cap=\"flat\"";
  s += ">" + Solid(color, Opacity(Prop(chain, kGraphic, "svg:stroke-opacity")));
  if (stroke == "dash") {
    if (xml_node d = st_.Named('d', Prop(chain, kGraphic, "draw:stroke-dash"))) {
      // Lengths as percentages of the line's width, or absolute.
      const double width = w > 0 ? static_cast<double>(w) : 12700;
      auto rel = [&](std::string_view v) {
        const double p = Percent(v);
        const double len = p >= 0 ? p / 100 : static_cast<double>(Emu(v)) / width;
        return N(std::llround(std::clamp(len <= 0 ? 1.0 : len, 0.01, 1000.0) * 100000));
      };
      const std::string space = rel(Attr(d, "draw:distance"));
      std::string ds;
      const int dots1 = std::clamp(std::atoi(Attr(d, "draw:dots1").data()), 0, 10);
      const int dots2 = std::clamp(std::atoi(Attr(d, "draw:dots2").data()), 0, 10);
      for (int i = 0; i < dots1; ++i) ds += "<a:ds d=\"" + rel(Attr(d, "draw:dots1-length")) + "\" sp=\"" + space + "\"/>";
      for (int i = 0; i < dots2; ++i) ds += "<a:ds d=\"" + rel(Attr(d, "draw:dots2-length")) + "\" sp=\"" + space + "\"/>";
      s += ds.empty() ? "<a:prstDash val=\"dash\"/>" : "<a:custDash>" + ds + "</a:custDash>";
    } else {
      s += "<a:prstDash val=\"dash\"/>";
    }
  }
  const std::string_view join = Prop(chain, kGraphic, "draw:stroke-linejoin");
  if (join == "round") s += "<a:round/>";
  else if (join == "bevel") s += "<a:bevel/>";
  else if (join == "miter") s += "<a:miter lim=\"800000\"/>";
  // Arrowheads: the marker's outline names the kind, its width against the line's the size.
  auto end = [&](const char* tag, const char* name, const char* width) -> std::string {
    xml_node m = st_.Named('k', Prop(chain, kGraphic, name));
    if (!m) return {};
    const std::string_view d = Attr(m, "svg:d");
    const char* type = "triangle";
    if (d.find_first_of("cCaAqQ") != std::string_view::npos) type = "oval";
    else if (std::count_if(d.begin(), d.end(), [](char c) { return c == 'l' || c == 'L' || c == 'h' || c == 'v' || c == 'H' || c == 'V'; }) >= 3 ||
             std::count(d.begin(), d.end(), ' ') >= 7)
      type = "diamond";
    const double ratio = static_cast<double>(Emu(Prop(chain, kGraphic, width))) / static_cast<double>(std::max<int64_t>(w, 9525));
    const char* size = ratio <= 2.5 ? "sm" : ratio <= 4.5 ? "med" : "lg";
    return std::string("<a:") + tag + " type=\"" + type + "\" w=\"" + size + "\" len=\"" + size + "\"/>";
  };
  s += end("headEnd", "draw:marker-start", "draw:marker-start-width");
  s += end("tailEnd", "draw:marker-end", "draw:marker-end-width");
  return s + "</a:ln>";
}

std::string Converter::Effects(const Chain& chain) const {
  if (Prop(chain, kGraphic, "draw:shadow") != "visible") return {};
  const double dx = static_cast<double>(Emu(Prop(chain, kGraphic, "draw:shadow-offset-x")));
  const double dy = static_cast<double>(Emu(Prop(chain, kGraphic, "draw:shadow-offset-y")));
  std::string color = odf::Color(Prop(chain, kGraphic, "draw:shadow-color"));
  if (color.empty()) color = "808080";
  int64_t dir = std::llround(std::atan2(dy, dx) * 180 / 3.14159265358979 * 60000);
  if (dir < 0) dir += 21600000;
  const int64_t blur = std::max<int64_t>(0, Emu(Prop(chain, kGraphic, "draw:shadow-blur")));
  return "<a:effectLst><a:outerShdw blurRad=\"" + N(blur) + "\" dist=\"" + N(std::llround(std::hypot(dx, dy))) + "\" dir=\"" +
         N(dir % 21600000) + "\" algn=\"ctr\" rotWithShape=\"0\">" + Srgb(color, Opacity(Prop(chain, kGraphic, "draw:shadow-opacity"))) +
         "</a:outerShdw></a:effectLst>";
}

std::string Converter::Image(std::string_view href) {
  while (href.starts_with("./")) href.remove_prefix(2);
  if (href.empty() || href.find("://") != std::string_view::npos || href.starts_with("/") || href.starts_with("..")) return {};
  if (auto it = mediaOf_.find(href); it != mediaOf_.end()) return it->second;
  std::string& name = mediaOf_[std::string(href)];
  const pkg::ZipEntry* e = zip_.Find(href);
  std::vector<uint8_t> bytes;
  if (!e || mediaBytes_ + e->uncompressedSize > limits_.maxTotalSize || !zip_.Read(*e, bytes).Ok() || bytes.size() < 8) return {};
  // The kind by its signature (an extension can say anything).
  const uint8_t* b = bytes.data();
  const char* ext = nullptr;
  if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') ext = "png";
  else if (b[0] == 0xFF && b[1] == 0xD8) ext = "jpeg";
  else if (b[0] == 'G' && b[1] == 'I' && b[2] == 'F') ext = "gif";
  else if (b[0] == 'B' && b[1] == 'M') ext = "bmp";
  else if ((b[0] == 'I' && b[1] == 'I' && b[2] == 42) || (b[0] == 'M' && b[1] == 'M' && b[3] == 42)) ext = "tiff";
  else if (b[0] == 0xD7 && b[1] == 0xCD && b[2] == 0xC6 && b[3] == 0x9A) ext = "wmf";
  else if (bytes.size() > 44 && b[0] == 1 && b[40] == ' ' && b[41] == 'E' && b[42] == 'M' && b[43] == 'F') ext = "emf";
  if (!ext) return {};
  mediaBytes_ += bytes.size();
  name = "image" + N(static_cast<int64_t>(media_.size()) + 1) + "." + ext;
  media_.emplace_back(name, std::move(bytes));
  return name;
}

draw::Size Converter::NaturalSize(const std::string& media) const {
  for (const auto& [name, bytes] : media_)
    if (name == media) {
      // Only the header is read.
      if (auto image = draw::Image::Open(bytes)) return image->NaturalSize();
      break;
    }
  return {};
}

// ---------------------------------------------------------------------------------------------
// Text

struct Converter::Runs {
  std::string out;    // a:r, a:br and a:fld
  std::string props;  // the a:rPr of the run being built
  std::string text;
  std::string end;    // the paragraph's end properties (its last span's)
  bool space = false; // white space read, not written yet (ODF collapses it)
  bool start = true;  // nothing written yet
  std::string_view link;

  void Flush() {
    if (!text.empty()) out += "<a:r>" + props + "<a:t>" + Esc(text) + "</a:t></a:r>";
    text.clear();
  }
  void Append(const std::string& rp, std::string_view s) {
    if (rp != props) {
      Flush();
      props = rp;
    }
    text += s;
  }
  // Character data, its white space collapsed.
  void Characters(const std::string& rp, std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
      const char c = s[i];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        space = !start;
        ++i;
        continue;
      }
      size_t j = i;
      while (j < s.size() && s[j] != ' ' && s[j] != '\t' && s[j] != '\n' && s[j] != '\r') ++j;
      if (space) Append(rp, " ");
      Append(rp, s.substr(i, j - i));
      space = start = false;
      i = j;
    }
  }
};

xml_node Converter::ListLevel(xml_node list, int level) const {
  xml_node best;
  for (xml_node l : list.children()) {
    const int n = std::atoi(Attr(l, "text:level").data());
    if (n == level + 1) return l;
    if (n >= 1 && n <= level + 1) best = l;
  }
  return best;
}

std::string Converter::Bullet(xml_node lvl, Part& part) {
  if (!lvl) return "<a:buNone/>";
  const std::string_view kind = lvl.name();
  xml_node tp = lvl.child(kText);
  std::string s;
  if (std::string c = odf::Color(Attr(tp, "fo:color")); !c.empty()) s += "<a:buClr>" + Srgb(c) + "</a:buClr>";
  std::string_view size = Attr(tp, "fo:font-size");
  if (size.empty()) size = Attr(lvl, "text:bullet-relative-size");
  if (const double p = Percent(size); p > 0) s += "<a:buSzPct val=\"" + N(std::llround(std::clamp(p, 25.0, 400.0) * 1000)) + "\"/>";
  else if (const auto pt = odf::Points(size)) s += "<a:buSzPts val=\"" + N(std::llround(std::clamp(*pt, 1.0, 4000.0) * 100)) + "\"/>";
  std::string font = FirstFamily(Attr(tp, "fo:font-family"));
  if (font.empty() && !Attr(tp, "style:font-name").empty()) font = st_.Font(Attr(tp, "style:font-name"));
  if (!font.empty()) s += "<a:buFont typeface=\"" + Esc(font) + "\"/>";
  if (kind == "text:list-level-style-bullet") {
    const std::string_view c = Attr(lvl, "text:bullet-char");
    if (c.empty()) return "<a:buNone/>";
    return s + "<a:buChar char=\"" + Esc(c) + "\"/>";
  }
  if (kind == "text:list-level-style-number") {
    const std::string_view f = Attr(lvl, "style:num-format"), pre = Attr(lvl, "style:num-prefix"), suf = Attr(lvl, "style:num-suffix");
    const char* scheme = f == "1" ? "arabic" : f == "a" ? "alphaLc" : f == "A" ? "alphaUc" : f == "i" ? "romanLc" : f == "I" ? "romanUc" : nullptr;
    if (!scheme) return "<a:buNone/>";
    std::string type = scheme;
    if (pre == "(" && suf == ")") type += "ParenBoth";
    else if (suf == ")") type += "ParenR";
    else if (suf == "." || std::string_view(scheme) != "arabic") type += "Period";
    else type += suf.empty() ? "Plain" : "Period";
    s += "<a:buAutoNum type=\"" + type + "\"";
    if (const int start = std::atoi(Attr(lvl, "text:start-value").data()); start > 1 && start < 32768) s += " startAt=\"" + N(start) + "\"";
    return s + "/>";
  }
  if (kind == "text:list-level-style-image") {
    const std::string media = Image(Attr(lvl, "xlink:href"));
    if (!media.empty()) return s + "<a:buBlip><a:blip r:embed=\"" + part.Rel("image", "../media/" + media) + "\"/></a:buBlip>";
    return s + "<a:buChar char=\"\xE2\x80\xA2\"/>";
  }
  return "<a:buNone/>";
}

std::string Converter::RunProps(const Chain& c, const char* tag, std::string_view link) const {
  std::string a = std::string("<") + tag;
  const std::string_view lang = Prop(c, kText, "fo:language"), country = Prop(c, kText, "fo:country");
  if (!lang.empty() && lang != "zxx" && lang != "none")
    a += " lang=\"" + Esc(lang) + (country.empty() || country == "none" ? "" : "-" + Esc(country)) + "\"";
  // The size: the first absolute one, scaled by the percentages before it.
  double factor = 1, size = 18;
  for (xml_node s : c) {
    const std::string_view v = s.child(kText).attribute("fo:font-size").value();
    if (v.empty()) continue;
    if (const double p = Percent(v); p >= 0) {
      factor *= p / 100;
      continue;
    }
    if (const auto pt = odf::Points(v)) {
      size = *pt;
      break;
    }
  }
  a += " sz=\"" + N(std::llround(std::clamp(size * factor, 1.0, 4000.0) * 100)) + "\"";
  if (const std::string_view w = Prop(c, kText, "fo:font-weight"); !w.empty())
    a += (w == "bold" || std::atoi(w.data()) >= 600) ? " b=\"1\"" : " b=\"0\"";
  if (const std::string_view i = Prop(c, kText, "fo:font-style"); !i.empty()) a += (i == "italic" || i == "oblique") ? " i=\"1\"" : " i=\"0\"";
  const std::string_view uStyle = Prop(c, kText, "style:text-underline-style");
  if (!uStyle.empty()) {
    const bool dbl = Prop(c, kText, "style:text-underline-type") == "double";
    const bool bold = Prop(c, kText, "style:text-underline-width") == "bold";
    std::string u = uStyle == "none" ? "none" : uStyle == "dotted" ? "dotted" : uStyle == "dash" ? "dash"
                  : uStyle == "long-dash" ? "dashLong" : uStyle == "dot-dash" ? "dotDash" : uStyle == "dot-dot-dash" ? "dotDotDash"
                  : uStyle == "wave" ? (dbl ? "wavyDbl" : "wavy") : dbl ? "dbl" : "sng";
    if (bold && u != "none" && u != "dbl" && u != "wavyDbl") u = u == "sng" ? "heavy" : u == "wavy" ? "wavyHeavy" : u + "Heavy";
    a += " u=\"" + u + "\"";
  }
  const std::string_view strike = Prop(c, kText, "style:text-line-through-style");
  if (!strike.empty())
    a += strike == "none" ? " strike=\"noStrike\"" : Prop(c, kText, "style:text-line-through-type") == "double" ? " strike=\"dblStrike\"" : " strike=\"sngStrike\"";
  if (const std::string_view k = Prop(c, kText, "style:letter-kerning"); !k.empty()) a += k == "true" ? " kern=\"1200\"" : " kern=\"0\"";
  if (Prop(c, kText, "fo:text-transform") == "uppercase") a += " cap=\"all\"";
  else if (Prop(c, kText, "fo:font-variant") == "small-caps") a += " cap=\"small\"";
  if (const std::string_view sp = Prop(c, kText, "fo:letter-spacing"); !sp.empty() && sp != "normal")
    if (const auto pt = odf::Points(sp); pt && *pt != 0) a += " spc=\"" + N(std::llround(std::clamp(*pt, -4000.0, 4000.0) * 100)) + "\"";
  if (const std::string_view pos = Prop(c, kText, "style:text-position"); !pos.empty()) {
    const std::string_view first = pos.substr(0, pos.find(' '));
    const double p = first == "super" ? 33 : first == "sub" ? -33 : Percent(first);
    if (p != 0 && p > -1000 && p < 1000) a += " baseline=\"" + N(std::llround(p * 1000)) + "\"";
  }
  std::string kids;
  if (Prop(c, kText, "style:use-window-font-color") != "true")
    if (std::string col = odf::Color(Prop(c, kText, "fo:color")); !col.empty()) kids += Solid(col);
  if (std::string hl = odf::Color(Prop(c, kText, "fo:background-color")); !hl.empty()) kids += "<a:highlight>" + Srgb(hl) + "</a:highlight>";
  auto face = [&](const char* tag2, const char* family, const char* name) {
    std::string f = FirstFamily(Prop(c, kText, family));
    if (f.empty()) {
      const std::string_view n = Prop(c, kText, name);
      if (!n.empty()) f = st_.Font(n);
    }
    if (!f.empty()) kids += std::string("<a:") + tag2 + " typeface=\"" + Esc(f) + "\"/>";
  };
  face("latin", "fo:font-family", "style:font-name");
  face("ea", "style:font-family-asian", "style:font-name-asian");
  face("cs", "style:font-family-complex", "style:font-name-complex");
  if (!link.empty()) kids += "<a:hlinkClick r:id=\"" + std::string(link) + "\"/>";
  if (kids.empty()) return a + "/>";
  return a + ">" + kids + "</" + tag + ">";
}

void Converter::Inline(xml_node node, const Chain& chain, const std::string& props, Part& part, Runs& runs, int depth) {
  if (depth > kMaxDepth) return;
  for (xml_node n : node.children()) {
    if (written_ + runs.out.size() + runs.text.size() > limits_.maxTotalSize) return;
    if (n.type() == pugi::node_pcdata || n.type() == pugi::node_cdata) {
      runs.Characters(props, n.value());
      continue;
    }
    if (n.type() != pugi::node_element) continue;
    const std::string_view k = n.name();
    if (k == "text:span") {
      Chain c;
      st_.Add(c, "text", Attr(n, "text:style-name"), part.master);
      c.insert(c.end(), chain.begin(), chain.end());
      const std::string rp = RunProps(c, "a:rPr", runs.link);
      runs.end = RunProps(c, "a:endParaRPr");
      Inline(n, c, rp, part, runs, depth + 1);
    } else if (k == "text:s") {
      const int count = std::clamp(n.attribute("text:c").as_int(1), 1, 1000);
      runs.Append(props, std::string(static_cast<size_t>(count), ' '));
      runs.space = runs.start = false;
    } else if (k == "text:tab") {
      runs.Append(props, "\t");
      runs.space = runs.start = false;
    } else if (k == "text:line-break") {
      runs.Flush();
      runs.out += "<a:br>" + props + "</a:br>";
      runs.space = false;
      runs.start = true;
    } else if (k == "text:a") {
      const std::string_view href = Attr(n, "xlink:href");
      const std::string_view before = runs.link;
      std::string id;
      if (!href.empty() && href[0] != '#' && href.size() < 2048) id = part.Rel("hyperlink", href, true);
      runs.link = id;
      Inline(n, chain, RunProps(chain, "a:rPr", runs.link), part, runs, depth + 1);
      runs.link = before;
    } else if (k == "text:page-number" || ((k == "text:date" || k == "text:time") && Attr(n, "text:fixed") != "true")) {
      runs.Flush();
      char id[48];
      std::snprintf(id, sizeof id, "{5C7A0B6E-0D9A-4F3B-9E1C-%012X}", ++fields_);
      const char* type = k == "text:page-number" ? "slidenum" : k == "text:date" ? "datetime1" : "datetime10";
      runs.out += std::string("<a:fld id=\"") + id + "\" type=\"" + type + "\">" + props + "<a:t>" + Esc(n.text().get()) + "</a:t></a:fld>";
      runs.space = runs.start = false;
    } else if (k == "text:soft-page-break" || k == "text:bookmark" || k == "text:bookmark-start" || k == "text:bookmark-end" ||
               k == "office:annotation" || k == "office:annotation-end" || k == "text:note") {
      continue;
    } else {
      Inline(n, chain, props, part, runs, depth + 1);  // fields shown as their text, and the like
    }
  }
}

void Converter::Paragraph(xml_node p, const Chain& shape, Part& part, xml_node list, int level, bool header, std::string& out) {
  Chain pc;
  st_.Add(pc, "paragraph", Attr(p, "text:style-name"), part.master);
  pc.insert(pc.end(), shape.begin(), shape.end());
  st_.AddDefault(pc, "paragraph");
  xml_node lvl = list ? ListLevel(list, level) : xml_node();

  std::string pPr = "<a:pPr";
  if (list && level > 0) pPr += " lvl=\"" + N(std::min(level, 8)) + "\"";
  std::string_view ml = Prop(pc, kPara, "fo:margin-left"), ti = Prop(pc, kPara, "fo:text-indent");
  int64_t marL = Emu(ml), indent = Emu(ti);
  if (lvl && ml.empty()) {
    xml_node props = lvl.child("style:list-level-properties");
    if (xml_node align = props.child("style:list-level-label-alignment")) {
      marL = Emu(Attr(align, "fo:margin-left")), indent = Emu(Attr(align, "fo:text-indent"));
    } else {
      const int64_t label = Emu(Attr(props, "text:min-label-width"));
      marL = Emu(Attr(props, "text:space-before")) + label, indent = -label;
    }
  }
  marL = std::clamp<int64_t>(marL, 0, 51206400), indent = std::clamp<int64_t>(indent, -51206400, 51206400);
  if (marL) pPr += " marL=\"" + N(marL) + "\"";
  if (indent) pPr += " indent=\"" + N(indent) + "\"";
  const std::string_view align = Prop(pc, kPara, "fo:text-align");
  if (align == "center") pPr += " algn=\"ctr\"";
  else if (align == "end" || align == "right") pPr += " algn=\"r\"";
  else if (align == "justify") pPr += Prop(pc, kPara, "fo:text-align-last") == "justify" ? " algn=\"dist\"" : " algn=\"just\"";
  else pPr += " algn=\"l\"";
  if (const int64_t tab = Emu(Prop(pc, kPara, "style:tab-stop-distance")); tab > 0) pPr += " defTabSz=\"" + N(tab) + "\"";
  if (Prop(pc, kPara, "style:writing-mode") == "rl-tb") pPr += " rtl=\"1\"";
  std::string kids;
  const std::string_view lh = Prop(pc, kPara, "fo:line-height");
  if (const double pct = Percent(lh); pct > 0) kids += "<a:lnSpc><a:spcPct val=\"" + N(std::llround(std::min(pct, 13200.0) * 1000)) + "\"/></a:lnSpc>";
  else if (const auto pt = odf::Points(lh.empty() ? Prop(pc, kPara, "style:line-height-at-least") : lh); pt && *pt > 0)
    kids += "<a:lnSpc><a:spcPts val=\"" + N(std::llround(std::min(*pt, 1584.0) * 100)) + "\"/></a:lnSpc>";
  auto spacing = [&](const char* tag, const char* attr) {
    if (const auto pt = odf::Points(Prop(pc, kPara, attr)); pt && *pt > 0)
      kids += std::string("<a:") + tag + "><a:spcPts val=\"" + N(std::llround(std::min(*pt, 1584.0) * 100)) + "\"/></a:" + tag + ">";
  };
  spacing("spcBef", "fo:margin-top");
  spacing("spcAft", "fo:margin-bottom");
  kids += list && !header ? Bullet(lvl, part) : "<a:buNone/>";
  // Tab stops, from the paragraph's indent in ODF, from the text's left edge in DrawingML.
  std::string tabs;
  int count = 0;
  for (xml_node s : pc) {
    xml_node stops = s.child(kPara).child("style:tab-stops");
    if (!stops) continue;
    for (xml_node t : stops.children("style:tab-stop")) {
      if (++count > 32) break;
      const std::string_view type = Attr(t, "style:type");
      const int64_t pos = std::clamp<int64_t>(Emu(Attr(t, "style:position")) + marL, 0, 51206400);
      tabs += "<a:tab pos=\"" + N(pos) + "\" algn=\"" + (type == "center" ? "ctr" : type == "right" ? "r" : type == "char" ? "dec" : "l") + "\"/>";
    }
    break;
  }
  if (!tabs.empty()) kids += "<a:tabLst>" + tabs + "</a:tabLst>";
  out += "<a:p>" + pPr + ">" + kids + "</a:pPr>";

  Runs runs;
  const std::string props = RunProps(pc, "a:rPr");
  runs.end = RunProps(pc, "a:endParaRPr");
  Inline(p, pc, props, part, runs, 0);
  runs.Flush();
  out += runs.out + runs.end + "</a:p>";
}

void Converter::Paragraphs(xml_node parent, const Chain& shape, Part& part, xml_node list, int level, bool header, int depth,
                           std::string& out) {
  if (depth > kMaxDepth) return;
  for (xml_node n : parent.children()) {
    if (written_ + out.size() > limits_.maxTotalSize) return;
    const std::string_view k = n.name();
    if (k == "text:p" || k == "text:h") {
      Paragraph(n, shape, part, list, level, header, out);
    } else if (k == "text:list") {
      // A list: its own style, else the list it's in, else the shape's (LibreOffice's outlines).
      xml_node style = st_.Find('l', {}, Attr(n, "text:style-name"), part.master);
      if (!style) style = list;
      if (!style)
        for (xml_node s : shape)
          if ((style = s.child(kGraphic).child("text:list-style"))) break;
      for (xml_node item : n.children()) {
        const std::string_view ik = item.name();
        if (ik == "text:list-item" || ik == "text:list-header")
          Paragraphs(item, shape, part, style, level + 1, ik == "text:list-header", depth + 1, out);
      }
    } else if (k == "text:section" || k == "text:list-item") {
      Paragraphs(n, shape, part, list, level, header, depth + 1, out);
    }
  }
}

std::string Converter::BodyPr(const Chain& c) const {
  std::string s = "<a:bodyPr";
  const bool noWrap = Prop(c, kGraphic, "fo:wrap-option") == "no-wrap";
  s += noWrap ? " wrap=\"none\"" : " wrap=\"square\"";
  auto inset = [&](const char* attr, const char* side) {
    std::string_view v = Prop(c, kGraphic, attr);
    if (v.empty()) v = Prop(c, kGraphic, "fo:padding");
    if (!v.empty()) s += std::string(" ") + side + "=\"" + N(std::max<int64_t>(0, Emu(v))) + "\"";
  };
  inset("fo:padding-left", "lIns");
  inset("fo:padding-top", "tIns");
  inset("fo:padding-right", "rIns");
  inset("fo:padding-bottom", "bIns");
  const std::string_view v = Prop(c, kGraphic, "draw:textarea-vertical-align");
  s += v == "middle" ? " anchor=\"ctr\"" : v == "bottom" ? " anchor=\"b\"" : v == "justify" ? " anchor=\"just\"" : " anchor=\"t\"";
  if (noWrap && Prop(c, kGraphic, "draw:textarea-horizontal-align") == "center") s += " anchorCtr=\"1\"";
  if (Prop(c, kPara, "style:writing-mode") == "tb-rl") s += " vert=\"vert\"";
  s += " rtlCol=\"0\">";
  if (Prop(c, kGraphic, "style:shrink-to-fit") == "true" || Prop(c, kGraphic, "draw:fit-to-size") == "shrink-to-fit")
    s += "<a:normAutofit/>";
  else if (Prop(c, kGraphic, "draw:auto-grow-height") == "true")
    s += "<a:spAutoFit/>";
  return s + "</a:bodyPr>";
}

std::string Converter::TextBody(xml_node container, const Chain& chain, Part& part, const char* tag) {
  std::string paras;
  Paragraphs(container, chain, part, {}, -1, false, 0, paras);
  if (paras.empty()) {
    Chain c = chain;
    st_.AddDefault(c, "paragraph");
    paras = "<a:p>" + RunProps(c, "a:endParaRPr") + "</a:p>";
  }
  return std::string("<") + tag + ">" + BodyPr(chain) + "<a:lstStyle/>" + paras + "</" + tag + ">";
}

// ---------------------------------------------------------------------------------------------
// Shapes

Chain Converter::ShapeChain(xml_node el, bool master) const {
  Chain c;
  st_.Add(c, "presentation", Attr(el, "presentation:style-name"), master);
  st_.Add(c, "graphic", Attr(el, "draw:style-name"), master);
  st_.AddDefault(c, "graphic");
  return c;
}

std::string Converter::Names(xml_node el, Part& part, std::string_view fallback) {
  const uint32_t id = part.nextId++;
  std::string_view name = Attr(el, "draw:name");
  std::string s = "<p:cNvPr id=\"" + N(id) + "\" name=\"" + (name.empty() ? std::string(fallback) + " " + N(id) : Esc(name)) + "\"";
  const std::string_view desc = el.child("svg:desc").text().get();
  if (!desc.empty()) s += " descr=\"" + Esc(desc) + "\"";
  return s + "/>";
}

void Converter::Sp(xml_node el, xml_node text, const std::string& geometry, const Box& box, const Chain& chain, Part& part,
                   std::string& out, bool textBox) {
  // Presentation objects are placeholders, numbered as their layout's (title, then the others).
  const std::string_view cls = Attr(el, "presentation:class");
  std::string ph;
  if (cls == "title") ph = "<p:ph type=\"title\"/>";
  else if (cls == "subtitle") ph = "<p:ph type=\"subTitle\" idx=\"" + N(++part.bodies) + "\"/>";
  else if (cls == "outline" || cls == "text" || cls == "object") ph = "<p:ph idx=\"" + N(++part.bodies) + "\"/>";
  else if (cls == "graphic") ph = "<p:ph type=\"pic\" idx=\"" + N(++part.bodies) + "\"/>";
  else if (cls == "chart") ph = "<p:ph type=\"chart\" idx=\"" + N(++part.bodies) + "\"/>";
  else if (cls == "table") ph = "<p:ph type=\"tbl\" idx=\"" + N(++part.bodies) + "\"/>";
  else if (cls == "orgchart") ph = "<p:ph type=\"dgm\" idx=\"" + N(++part.bodies) + "\"/>";
  else if (cls == "header" || cls == "notes" || cls == "handout") return;  // slides have none
  else if (cls == "date-time") ph = "<p:ph type=\"dt\" sz=\"half\" idx=\"10\"/>";
  else if (cls == "footer") ph = "<p:ph type=\"ftr\" sz=\"quarter\" idx=\"11\"/>";
  else if (cls == "page-number") ph = "<p:ph type=\"sldNum\" sz=\"quarter\" idx=\"12\"/>";
  out += "<p:sp><p:nvSpPr>" + Names(el, part, ph.empty() ? "Shape" : "Placeholder");
  out += ph.empty() ? (textBox ? "<p:cNvSpPr txBox=\"1\"/>" : "<p:cNvSpPr/>") : "<p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr>";
  out += "<p:nvPr>" + ph + "</p:nvPr></p:nvSpPr><p:spPr>" + Xfrm(box) + geometry + Fill(chain, kGraphic, part) + Line(chain, false) +
         Effects(chain) + "</p:spPr>";
  if (text) out += TextBody(text, chain, part, "p:txBody");
  out += "</p:sp>";
}

void Converter::Picture(xml_node el, xml_node image, const Box& box, const Chain& chain, Part& part, std::string& out) {
  const std::string media = Image(Attr(image, "xlink:href"));
  if (media.empty()) return;
  out += "<p:pic><p:nvPicPr>" + Names(el, part, "Picture") +
         "<p:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></p:cNvPicPr><p:nvPr/></p:nvPicPr><p:blipFill><a:blip r:embed=\"" +
         part.Rel("image", "../media/" + media) + "\"";
  const double opacity = Opacity(Prop(chain, kGraphic, "draw:image-opacity"));
  out += opacity < 0.9995 ? "><a:alphaModFix amt=\"" + N(std::llround(opacity * 100000)) + "\"/></a:blip>" : "/>";
  // A crop: fo:clip's "rect(top, right, bottom, left)", lengths of the picture at its own size.
  if (std::string_view clip = Prop(chain, kGraphic, "fo:clip"); clip.starts_with("rect(")) {
    clip.remove_prefix(5);
    double side[4] = {0, 0, 0, 0};
    for (double& s : side) {
      while (!clip.empty() && (clip.front() == ' ' || clip.front() == ',')) clip.remove_prefix(1);
      const size_t end = clip.find_first_of(" ,)");
      const auto pt = odf::Points(clip.substr(0, end));
      s = pt ? *pt : 0;  // "auto": none
      clip.remove_prefix(end == std::string_view::npos ? clip.size() : end);
    }
    const draw::Size natural = NaturalSize(media);
    if (natural.width > 0 && natural.height > 0 && (side[0] || side[1] || side[2] || side[3])) {
      auto pct = [](double v, double of) { return N(std::llround(std::clamp(v / of, -10.0, 10.0) * 100000)); };
      out += "<a:srcRect l=\"" + pct(side[3], natural.width) + "\" t=\"" + pct(side[0], natural.height) + "\" r=\"" +
             pct(side[1], natural.width) + "\" b=\"" + pct(side[2], natural.height) + "\"/>";
    }
  }
  out += "<a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr>" + Xfrm(box) + Preset("rect") + Fill(chain, kGraphic, part) +
         Line(chain, false) + Effects(chain) + "</p:spPr></p:pic>";
}

void Converter::Table(xml_node el, xml_node table, const Box& box, Part& part, std::string& out) {
  // Columns and rows, repeated as the table says (within reason).
  std::vector<int64_t> cols;
  for (xml_node c : table.children("table:table-column")) {
    Chain cc;
    st_.Add(cc, "table-column", Attr(c, "table:style-name"), part.master);
    const int64_t w = std::max<int64_t>(0, Emu(Prop(cc, "style:table-column-properties", "style:column-width")));
    const int repeat = std::clamp(c.attribute("table:number-columns-repeated").as_int(1), 1, 64);
    for (int i = 0; i < repeat && cols.size() < 64; ++i) cols.push_back(w);
  }
  std::vector<xml_node> rows;
  for (xml_node r : table.children()) {
    const std::string_view k = r.name();
    if (k == "table:table-row") rows.push_back(r);
    else if (k == "table:table-header-rows" || k == "table:table-rows")
      for (xml_node rr : r.children("table:table-row")) rows.push_back(rr);
    if (rows.size() >= 256) break;
  }
  size_t width = cols.size();
  for (xml_node r : rows) {
    size_t n = 0;
    for (xml_node c : r.children())
      if (std::string_view(c.name()).starts_with("table:")) n += std::clamp(c.attribute("table:number-columns-repeated").as_int(1), 1, 64);
    width = std::max(width, std::min<size_t>(n, 64));
  }
  if (width == 0 || rows.empty()) return;
  const int64_t known = [&] {
    int64_t sum = 0;
    for (int64_t w : cols) sum += w;
    return sum;
  }();
  while (cols.size() < width) cols.push_back(0);
  for (int64_t& w : cols)
    if (w == 0) w = std::max<int64_t>(1, (box.w - known) / static_cast<int64_t>(width));
  // Which cells a merged one covers: 1 to the right (hMerge), 2 below (vMerge), 3 both.
  std::vector<uint8_t> covered(rows.size() * width, 0);
  std::string grid;
  for (int64_t w : cols) grid += "<a:gridCol w=\"" + N(w) + "\"/>";
  std::string body;
  for (size_t r = 0; r < rows.size() && written_ + body.size() <= limits_.maxTotalSize; ++r) {
    Chain rc;
    st_.Add(rc, "table-row", Attr(rows[r], "table:style-name"), part.master);
    std::string_view hv = Prop(rc, "style:table-row-properties", "style:row-height");
    if (hv.empty()) hv = Prop(rc, "style:table-row-properties", "style:min-row-height");
    body += "<a:tr h=\"" + N(std::max<int64_t>(0, Emu(hv))) + "\">";
    size_t col = 0;
    for (xml_node cell : rows[r].children()) {
      const std::string_view k = cell.name();
      if (k != "table:table-cell" && k != "table:covered-table-cell") continue;
      const int repeat = std::clamp(cell.attribute("table:number-columns-repeated").as_int(1), 1, 64);
      for (int rep = 0; rep < repeat && col < width; ++rep, ++col) {
        const uint8_t cov = covered[r * width + col];
        std::string tc = "<a:tc";
        if (cov & 1) tc += " hMerge=\"1\"";
        if (cov & 2) tc += " vMerge=\"1\"";
        const int cs = std::clamp(cell.attribute("table:number-columns-spanned").as_int(1), 1, static_cast<int>(width - col));
        const int rs = std::clamp(cell.attribute("table:number-rows-spanned").as_int(1), 1, static_cast<int>(rows.size() - r));
        if (k == "table:table-cell" && !cov && (cs > 1 || rs > 1)) {
          if (cs > 1) tc += " gridSpan=\"" + N(cs) + "\"";
          if (rs > 1) tc += " rowSpan=\"" + N(rs) + "\"";
          for (int dr = 0; dr < rs; ++dr)
            for (int dc = 0; dc < cs; ++dc)
              if (dr || dc) covered[(r + dr) * width + col + dc] |= static_cast<uint8_t>((dc ? 1 : 0) | (dr ? 2 : 0));
        }
        Chain cc;
        st_.Add(cc, "table-cell", Attr(cell, "table:style-name"), part.master);
        // The cell's text: its paragraphs, or one empty one.
        std::string paras;
        if (k == "table:table-cell") Paragraphs(cell, cc, part, {}, -1, false, 0, paras);
        if (paras.empty()) {
          Chain e = cc;
          st_.AddDefault(e, "paragraph");
          paras = "<a:p>" + RunProps(e, "a:endParaRPr") + "</a:p>";
        }
        tc += "><a:txBody><a:bodyPr/><a:lstStyle/>" + paras + "</a:txBody><a:tcPr";
        const char* cp = "style:table-cell-properties";
        auto margin = [&](const char* attr, const char* name) {
          std::string_view v = Prop(cc, cp, attr);
          if (v.empty()) v = Prop(cc, cp, "fo:padding");
          if (!v.empty()) tc += std::string(" ") + name + "=\"" + N(std::max<int64_t>(0, Emu(v))) + "\"";
        };
        margin("fo:padding-left", "marL");
        margin("fo:padding-right", "marR");
        margin("fo:padding-top", "marT");
        margin("fo:padding-bottom", "marB");
        const std::string_view va = Prop(cc, cp, "style:vertical-align");
        if (va == "middle") tc += " anchor=\"ctr\"";
        else if (va == "bottom") tc += " anchor=\"b\"";
        tc += ">";
        for (const auto& [attr, tag] : {std::pair{"fo:border-left", "lnL"}, {"fo:border-right", "lnR"}, {"fo:border-top", "lnT"},
                                        {"fo:border-bottom", "lnB"}}) {
          std::string_view v = Prop(cc, cp, attr);
          if (v.empty()) v = Prop(cc, cp, "fo:border");
          const auto b = odf::ParseBorder(v);
          if (!b) continue;
          if (b->none) {
            tc += std::string("<a:") + tag + " w=\"0\"><a:noFill/></a:" + tag + ">";
            continue;
          }
          tc += std::string("<a:") + tag + " w=\"" + N(std::llround(b->width * 12700)) + "\">" + Solid(b->color.empty() ? "000000" : b->color) +
                (b->style == "dashed" ? "<a:prstDash val=\"dash\"/>" : b->style == "dotted" ? "<a:prstDash val=\"sysDot\"/>" : "") +
                "</a:" + tag + ">";
        }
        if (std::string bg = odf::Color(Prop(cc, cp, "fo:background-color")); !bg.empty()) tc += Solid(bg);
        else if (Prop(cc, cp, "fo:background-color") == "transparent") tc += "<a:noFill/>";
        body += tc + "</a:tcPr></a:tc>";
      }
    }
    for (; col < width; ++col) body += "<a:tc><a:txBody><a:bodyPr/><a:lstStyle/><a:p><a:endParaRPr lang=\"en-US\"/></a:p></a:txBody><a:tcPr/></a:tc>";
    body += "</a:tr>";
  }
  out += "<p:graphicFrame><p:nvGraphicFramePr>" + Names(el, part, "Table") +
         "<p:cNvGraphicFramePr><a:graphicFrameLocks noGrp=\"1\"/></p:cNvGraphicFramePr><p:nvPr/></p:nvGraphicFramePr>"
         "<p:xfrm><a:off x=\"" + N(box.x) + "\" y=\"" + N(box.y) + "\"/><a:ext cx=\"" + N(box.w) + "\" cy=\"" + N(box.h) +
         "\"/></p:xfrm><a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/table\"><a:tbl><a:tblPr/>"
         "<a:tblGrid>" + grid + "</a:tblGrid>" + body + "</a:tbl></a:graphicData></a:graphic></p:graphicFrame>";
}

void Converter::Frame(xml_node el, Part& part, std::string& out, Bounds* bounds) {
  const Box box = ReadBox(el);
  const Chain chain = ShapeChain(el, part.master);
  // What the frame holds: the first thing it can show (a picture before an object's replacement).
  xml_node text, image, table;
  for (xml_node c : el.children()) {
    const std::string_view k = c.name();
    if (k == "draw:text-box" && !text) text = c;
    else if (k == "table:table" && !table) table = c;
    else if (k == "draw:image" && !image) {
      // Of several (an SVG and its PNG), one the slide can draw.
      const std::string_view href = Attr(c, "xlink:href");
      if (!href.ends_with(".svg") || !c.next_sibling("draw:image")) image = c;
    } else if ((k == "draw:object" || k == "draw:object-ole") && !image) {
      image = c.next_sibling("draw:image");
    }
  }
  if (table) Table(el, table, box, part, out);
  else if (image) Picture(el, image, box, chain, part, out);
  else if (text) Sp(el, text, Preset("rect"), box, chain, part, out, Attr(el, "presentation:class").empty());
  else if (!Attr(el, "presentation:class").empty()) Sp(el, {}, Preset("rect"), box, chain, part, out, false);  // an empty one: its fill and line show
  else return;
  if (bounds) bounds->Add(box);
}

void Converter::Connector(xml_node el, Part& part, std::string& out, Bounds* bounds) {
  const double x1 = static_cast<double>(Emu(Attr(el, "svg:x1"))), y1 = static_cast<double>(Emu(Attr(el, "svg:y1")));
  const double x2 = static_cast<double>(Emu(Attr(el, "svg:x2"))), y2 = static_cast<double>(Emu(Attr(el, "svg:y2")));
  Box b;
  b.w = std::llround(std::abs(x2 - x1)), b.h = std::llround(std::abs(y2 - y1));
  b.flipH = x2 < x1, b.flipV = y2 < y1;
  double x = std::min(x1, x2), y = std::min(y1, y2);
  Transform t;
  if (t.Read(Attr(el, "draw:transform"))) {
    const double cx = x + b.w / 2.0, cy = y + b.h / 2.0;
    x = t.a * cx + t.b * cy + t.e - b.w / 2.0;
    y = t.c * cx + t.d * cy + t.f - b.h / 2.0;
    b.rot = t.Degrees();
  }
  b.x = std::clamp<int64_t>(std::llround(x), -kMaxCoord, kMaxCoord);
  b.y = std::clamp<int64_t>(std::llround(y), -kMaxCoord, kMaxCoord);
  const std::string_view type = Attr(el, "draw:type");
  const char* prst = std::string_view(el.name()) == "draw:line" || type == "line" || type == "lines" ? "line"
                     : type == "curve" ? "curvedConnector3" : "bentConnector3";
  const Chain chain = ShapeChain(el, part.master);
  out += "<p:cxnSp><p:nvCxnSpPr>" + Names(el, part, "Connector") + "<p:cNvCxnSpPr/><p:nvPr/></p:nvCxnSpPr><p:spPr>" + Xfrm(b) +
         Preset(prst) + Line(chain, true) + Effects(chain) + "</p:spPr></p:cxnSp>";
  if (bounds) bounds->Add(b);
}

void Converter::Shapes(xml_node parent, Part& part, std::string& out, int depth, Bounds* bounds) {
  if (depth > kMaxDepth) return;
  for (xml_node el : parent.children()) {
    if (written_ + out.size() > limits_.maxTotalSize) return;
    const std::string_view k = el.name();
    if (k == "draw:frame") {
      Frame(el, part, out, bounds);
    } else if (k == "draw:custom-shape" || k == "draw:rect" || k == "draw:ellipse" || k == "draw:circle" || k == "draw:polygon" ||
               k == "draw:polyline") {
      Box box = ReadBox(el);
      std::string geometry;
      if (k == "draw:custom-shape") {
        xml_node g = el.child("draw:enhanced-geometry");
        box.flipH = Attr(g, "draw:mirror-horizontal") == "true";
        box.flipV = Attr(g, "draw:mirror-vertical") == "true";
        geometry = CustomGeometry(g, box.w, box.h);
        if (geometry.empty())
          if (const std::string prst = PresetOf(Attr(g, "draw:type")); !prst.empty()) geometry = Preset(prst);
      } else if (k == "draw:polygon" || k == "draw:polyline") {
        geometry = PolygonGeometry(Attr(el, "draw:points"), Attr(el, "svg:viewBox"), k == "draw:polygon");
      } else if (k == "draw:rect" && !Attr(el, "draw:corner-radius").empty()) {
        const double r = static_cast<double>(Emu(Attr(el, "draw:corner-radius")));
        const double side = static_cast<double>(std::max<int64_t>(1, std::min(box.w, box.h)));
        geometry = "<a:prstGeom prst=\"roundRect\"><a:avLst><a:gd name=\"adj\" fmla=\"val " +
                   N(std::llround(std::clamp(r / side, 0.0, 0.5) * 100000)) + "\"/></a:avLst></a:prstGeom>";
      } else {
        geometry = Preset(k == "draw:rect" ? "rect" : "ellipse");
      }
      if (geometry.empty()) geometry = Preset("rect");
      const bool hasText = el.child("text:p") || el.child("text:list") || el.child("text:h");
      Sp(el, hasText ? el : xml_node(), geometry, box, ShapeChain(el, part.master), part, out, false);
      if (bounds) bounds->Add(box);
    } else if (k == "draw:connector" || k == "draw:line") {
      Connector(el, part, out, bounds);
    } else if (k == "draw:g") {
      // A group's box is its shapes' (their coordinates stay the slide's).
      std::string children;
      Bounds inner;
      const std::string names = Names(el, part, "Group");
      Shapes(el, part, children, depth + 1, &inner);
      if (!inner.any) continue;
      Box b;
      b.x = inner.l, b.y = inner.t, b.w = inner.r - inner.l, b.h = inner.b - inner.t;
      out += "<p:grpSp><p:nvGrpSpPr>" + names + "<p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr>" + Xfrm(b, &inner) +
             "</p:grpSpPr>" + children + "</p:grpSp>";
      if (bounds) bounds->Add(b);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Pages

std::string Converter::Background(xml_node page, bool master, Part& part) {
  Chain c;
  st_.Add(c, "drawing-page", Attr(page, "draw:style-name"), master);
  const std::string fill = Fill(c, kPage, part);
  if (fill.empty()) return {};
  return "<p:bg><p:bgPr>" + fill + "<a:effectLst/></p:bgPr></p:bg>";
}

int Converter::LayoutOf(std::string_view masterName) {
  for (size_t i = 0; i < layoutMasters_.size(); ++i)
    if (layoutMasters_[i] == masterName) return static_cast<int>(i);
  xml_node mp = st_.Named('M', masterName);
  if (!mp && !layoutMasters_.empty()) return 0;
  Part part;
  part.master = true;
  part.Rel("slideMaster", "../slideMasters/slideMaster1.xml");
  std::string xml = Layout(mp, part);
  layoutMasters_.emplace_back(masterName);
  layouts_.emplace_back(std::move(xml), std::move(part.rels));
  return static_cast<int>(layouts_.size() - 1);
}

std::string Converter::Layout(xml_node mp, Part& part) {
  // Its name: PowerPoint's ("Master1-Layout2-obj-Title-and-Content"), or the master page's.
  std::string name(Attr(mp, "style:display-name"));
  if (name.empty()) {
    std::string_view n = Attr(mp, "style:name");
    if (n.starts_with("Master")) {
      size_t dashes = 0, at = 0;
      while (dashes < 3 && (at = n.find('-', at)) != std::string_view::npos) ++dashes, ++at;
      if (dashes == 3) n.remove_prefix(at);
    }
    name = odf::DisplayName(n);
    std::replace(name.begin(), name.end(), '-', ' ');
  }
  if (name.empty()) name = "Layout";
  std::string shapes;
  Shapes(mp, part, shapes, 0, nullptr);
  written_ += shapes.size();
  return std::string(kDecl) + "<p:sldLayout" + std::string(kNs) + " preserve=\"1\" userDrawn=\"1\"><p:cSld name=\"" + Esc(name) + "\">" +
         Background(mp, true, part) +
         "<p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>" + shapes +
         "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>";
}

pkg::Status Converter::Run(std::vector<uint8_t>& pptx) {
  const std::string mime = odf::MimeType(zip_);
  if (!mime.empty() && !mime.starts_with("application/vnd.oasis.opendocument.presentation"))
    return pkg::Status::Fail(pkg::Status::Code::Unsupported, "This OpenDocument file isn't a presentation.");
  constexpr std::string_view kNot = "This isn't an OpenDocument presentation.";
  if (pkg::Status s = odf::LoadPart(zip_, "content.xml", content_, limits_, true, kNot); !s.Ok()) return s;
  if (pkg::Status s = odf::LoadPart(zip_, "styles.xml", styles_, limits_, false, kNot); !s.Ok()) return s;
  xml_node pres = content_.Root().child("office:body").child("office:presentation");
  if (!pres) return pkg::Status::Fail(pkg::Status::Code::Unsupported, "This OpenDocument file isn't a presentation.");
  xml_node sr = styles_.Root();
  st_.Add(sr.child("office:font-face-decls"), 'c');
  st_.Add(content_.Root().child("office:font-face-decls"), 'c');
  st_.Add(sr.child("office:styles"), 'c');
  st_.Add(sr.child("office:automatic-styles"), 'm');
  st_.Add(sr.child("office:master-styles"), 'c');
  st_.Add(content_.Root().child("office:automatic-styles"), 'a');

  // The slides. Each names its master page, which becomes its layout.
  std::vector<std::pair<std::string, std::string>> slides;  // xml, rels
  for (xml_node page : pres.children("draw:page")) {
    if (slides.size() >= kMaxSlides) break;
    const int layout = LayoutOf(Attr(page, "draw:master-page-name"));
    Part part;
    part.Rel("slideLayout", "../slideLayouts/slideLayout" + N(layout + 1) + ".xml");
    std::string shapes;
    Shapes(page, part, shapes, 0, nullptr);
    written_ += shapes.size();
    if (written_ > limits_.maxTotalSize) return pkg::Status::Fail(pkg::Status::Code::TooLarge, "The presentation is too large to open.");
    Chain pc;
    st_.Add(pc, "drawing-page", Attr(page, "draw:style-name"), false);
    std::string sld = std::string(kDecl) + "<p:sld" + std::string(kNs);
    if (Prop(pc, kPage, "presentation:background-objects-visible") == "false") sld += " showMasterSp=\"0\"";
    if (Prop(pc, kPage, "presentation:visibility") == "hidden") sld += " show=\"0\"";
    sld += "><p:cSld>" + Background(page, false, part) +
           "<p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>" + shapes +
           "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>";
    // The speaker notes: the notes frame's text, when there is some.
    const std::string n = N(static_cast<int64_t>(slides.size()) + 1);
    xml_node notes = page.child("presentation:notes");
    xml_node body;
    for (xml_node f : notes.children("draw:frame")) {
      xml_node box = f.child("draw:text-box");
      const bool text = box.find_node([](xml_node x) {
        return x.type() == pugi::node_pcdata && std::string_view(x.value()).find_first_not_of(" \t\r\n") != std::string_view::npos;
      });
      if (Attr(f, "presentation:class") == "notes" && text) {
        body = box;
        break;
      }
    }
    if (body) {
      Part np;
      np.Rel("notesMaster", "../notesMasters/notesMaster1.xml");
      np.Rel("slide", "../slides/slide" + n + ".xml");
      const Box box = ReadBox(body.parent());
      std::string text = TextBody(body, ShapeChain(body.parent(), false), np, "p:txBody");
      notes_.emplace_back(std::string(kDecl) + "<p:notes" + std::string(kNs) +
                              "><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
                              "<p:grpSpPr/><p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Slide Image Placeholder 1\"/><p:cNvSpPr><a:spLocks "
                              "noGrp=\"1\" noRot=\"1\" noChangeAspect=\"1\"/></p:cNvSpPr><p:nvPr><p:ph type=\"sldImg\"/></p:nvPr></p:nvSpPr>"
                              "<p:spPr/></p:sp><p:sp><p:nvSpPr><p:cNvPr id=\"3\" name=\"Notes Placeholder 2\"/><p:cNvSpPr><a:spLocks "
                              "noGrp=\"1\"/></p:cNvSpPr><p:nvPr><p:ph type=\"body\" idx=\"1\"/></p:nvPr></p:nvSpPr><p:spPr>" +
                              (box.w > 0 && box.h > 0 ? Xfrm(box) : std::string()) + "</p:spPr>" + text +
                              "</p:sp></p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:notes>",
                          n, std::move(np.rels));
      part.Rel("notesSlide", "../notesSlides/notesSlide" + n + ".xml");
    }
    slides.emplace_back(std::move(sld), std::move(part.rels));
  }
  if (layouts_.empty()) LayoutOf({});  // no slides: a blank layout

  // The slide size: the first master page's page layout.
  int64_t cx = 9144000, cy = 6858000;
  {
    xml_node mp = st_.Named('M', layoutMasters_.front());
    xml_node pl = st_.Named('p', Attr(mp, "style:page-layout-name")).child("style:page-layout-properties");
    if (const int64_t w = Emu(Attr(pl, "fo:page-width")), h = Emu(Attr(pl, "fo:page-height")); w > 0 && h > 0)
      cx = std::clamp<int64_t>(w, 914400, 51206400), cy = std::clamp<int64_t>(h, 914400, 51206400);
  }

  // The package.
  pkg::MemorySink sink;
  pkg::ZipWriter zip(sink);
  auto put = [&](const std::string& name, std::string_view text) {
    return zip.Add(name, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
  };
  std::string types = std::string(kDecl) +
                      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                      "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                      "<Default Extension=\"png\" ContentType=\"image/png\"/><Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>"
                      "<Default Extension=\"gif\" ContentType=\"image/gif\"/><Default Extension=\"bmp\" ContentType=\"image/bmp\"/>"
                      "<Default Extension=\"tiff\" ContentType=\"image/tiff\"/><Default Extension=\"emf\" ContentType=\"image/x-emf\"/>"
                      "<Default Extension=\"wmf\" ContentType=\"image/x-wmf\"/>"
                      "<Override PartName=\"/ppt/presentation.xml\" ContentType=\"" + std::string(kCt) + "presentation.main+xml\"/>"
                      "<Override PartName=\"/ppt/slideMasters/slideMaster1.xml\" ContentType=\"" + std::string(kCt) + "slideMaster+xml\"/>"
                      "<Override PartName=\"/ppt/theme/theme1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>"
                      "<Override PartName=\"/ppt/presProps.xml\" ContentType=\"" + std::string(kCt) + "presProps+xml\"/>"
                      "<Override PartName=\"/ppt/tableStyles.xml\" ContentType=\"" + std::string(kCt) + "tableStyles+xml\"/>";
  std::string presRels = std::string(kDecl) + std::string(kRelsOpen) +
                         "<Relationship Id=\"rId1\" Type=\"" + std::string(kRel) + "slideMaster\" Target=\"slideMasters/slideMaster1.xml\"/>";
  std::string slideIds;
  for (size_t i = 0; i < slides.size(); ++i) {
    const std::string n = N(static_cast<int64_t>(i) + 1);
    if (pkg::Status s = put("ppt/slides/slide" + n + ".xml", slides[i].first); !s.Ok()) return s;
    put("ppt/slides/_rels/slide" + n + ".xml.rels", std::string(kDecl) + std::string(kRelsOpen) + slides[i].second + "</Relationships>");
    types += "<Override PartName=\"/ppt/slides/slide" + n + ".xml\" ContentType=\"" + std::string(kCt) + "slide+xml\"/>";
    presRels += "<Relationship Id=\"rId" + N(static_cast<int64_t>(i) + 2) + "\" Type=\"" + std::string(kRel) + "slide\" Target=\"slides/slide" + n + ".xml\"/>";
    slideIds += "<p:sldId id=\"" + N(static_cast<int64_t>(i) + 256) + "\" r:id=\"rId" + N(static_cast<int64_t>(i) + 2) + "\"/>";
  }
  int64_t rid = static_cast<int64_t>(slides.size()) + 2;
  slides.clear();
  for (const char* type : {"theme", "presProps", "tableStyles"}) {
    const std::string target = std::string_view(type) == "theme" ? "theme/theme1.xml" : std::string(type) + ".xml";
    presRels += "<Relationship Id=\"rId" + N(rid++) + "\" Type=\"" + std::string(kRel) + type + "\" Target=\"" + target + "\"/>";
  }
  // The notes: a notes master (its own theme), and each slide's notes slide.
  std::string notesMasterId;
  if (!notes_.empty()) {
    const std::string ph = "<p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr><p:nvPr><p:ph ";
    put("ppt/notesMasters/notesMaster1.xml",
        std::string(kDecl) + "<p:notesMaster" + std::string(kNs) +
            "><p:cSld><p:bg><p:bgRef idx=\"1001\"><a:schemeClr val=\"bg1\"/></p:bgRef></p:bg><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" "
            "name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/><p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Slide Image "
            "Placeholder 1\"/>" + ph + "type=\"sldImg\" idx=\"2\"/></p:nvPr></p:nvSpPr><p:spPr><a:xfrm><a:off x=\"1143000\" y=\"685800\"/>"
            "<a:ext cx=\"4572000\" cy=\"3429000\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:noFill/><a:ln w=\"12700\">"
            "<a:solidFill><a:srgbClr val=\"000000\"/></a:solidFill></a:ln></p:spPr></p:sp><p:sp><p:nvSpPr><p:cNvPr id=\"3\" "
            "name=\"Notes Placeholder 2\"/>" + ph + "type=\"body\" sz=\"quarter\" idx=\"3\"/></p:nvPr></p:nvSpPr><p:spPr><a:xfrm>"
            "<a:off x=\"685800\" y=\"4343400\"/><a:ext cx=\"5486400\" cy=\"4114800\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/>"
            "</a:prstGeom></p:spPr><p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:rPr lang=\"en-US\"/><a:t>Click to edit Master text "
            "styles</a:t></a:r></a:p></p:txBody></p:sp></p:spTree></p:cSld><p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" "
            "accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" "
            "hlink=\"hlink\" folHlink=\"folHlink\"/><p:notesStyle><a:lvl1pPr><a:defRPr sz=\"1200\"/></a:lvl1pPr></p:notesStyle>"
            "</p:notesMaster>");
    put("ppt/notesMasters/_rels/notesMaster1.xml.rels", std::string(kDecl) + std::string(kRelsOpen) + "<Relationship Id=\"rId1\" Type=\"" +
                                                            std::string(kRel) + "theme\" Target=\"../theme/theme2.xml\"/></Relationships>");
    put("ppt/theme/theme2.xml", std::string(kDecl) + std::string(draw::Theme::DefaultXml()));
    types += "<Override PartName=\"/ppt/notesMasters/notesMaster1.xml\" ContentType=\"" + std::string(kCt) + "notesMaster+xml\"/>"
             "<Override PartName=\"/ppt/theme/theme2.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>";
    notesMasterId = "rId" + N(rid++);
    presRels += "<Relationship Id=\"" + notesMasterId + "\" Type=\"" + std::string(kRel) + "notesMaster\" Target=\"notesMasters/notesMaster1.xml\"/>";
    for (auto& [xml, n, rels] : notes_) {
      if (pkg::Status s = put("ppt/notesSlides/notesSlide" + n + ".xml", xml); !s.Ok()) return s;
      put("ppt/notesSlides/_rels/notesSlide" + n + ".xml.rels", std::string(kDecl) + std::string(kRelsOpen) + rels + "</Relationships>");
      types += "<Override PartName=\"/ppt/notesSlides/notesSlide" + n + ".xml\" ContentType=\"" + std::string(kCt) + "notesSlide+xml\"/>";
    }
    notes_.clear();
  }
  std::string layoutIds, masterRels = std::string(kDecl) + std::string(kRelsOpen);
  for (size_t i = 0; i < layouts_.size(); ++i) {
    const std::string n = N(static_cast<int64_t>(i) + 1);
    if (pkg::Status s = put("ppt/slideLayouts/slideLayout" + n + ".xml", layouts_[i].first); !s.Ok()) return s;
    put("ppt/slideLayouts/_rels/slideLayout" + n + ".xml.rels", std::string(kDecl) + std::string(kRelsOpen) + layouts_[i].second + "</Relationships>");
    types += "<Override PartName=\"/ppt/slideLayouts/slideLayout" + n + ".xml\" ContentType=\"" + std::string(kCt) + "slideLayout+xml\"/>";
    masterRels += "<Relationship Id=\"rId" + n + "\" Type=\"" + std::string(kRel) + "slideLayout\" Target=\"../slideLayouts/slideLayout" + n + ".xml\"/>";
    layoutIds += "<p:sldLayoutId id=\"" + N(2147483649LL + static_cast<int64_t>(i)) + "\" r:id=\"rId" + n + "\"/>";
  }
  masterRels += "<Relationship Id=\"rId" + N(static_cast<int64_t>(layouts_.size()) + 1) + "\" Type=\"" + std::string(kRel) +
                "theme\" Target=\"../theme/theme1.xml\"/></Relationships>";
  layouts_.clear();
  // The master: white, empty, the default text styles (every paragraph and run says its own).
  put("ppt/slideMasters/slideMaster1.xml",
      std::string(kDecl) + "<p:sldMaster" + std::string(kNs) +
          "><p:cSld><p:bg><p:bgPr><a:solidFill><a:srgbClr val=\"FFFFFF\"/></a:solidFill><a:effectLst/></p:bgPr></p:bg>"
          "<p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/></p:spTree></p:cSld>"
          "<p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" "
          "accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/><p:sldLayoutIdLst>" +
          layoutIds +
          "</p:sldLayoutIdLst><p:txStyles><p:titleStyle><a:lvl1pPr><a:defRPr sz=\"4400\"/></a:lvl1pPr></p:titleStyle>"
          "<p:bodyStyle><a:lvl1pPr><a:defRPr sz=\"2800\"/></a:lvl1pPr></p:bodyStyle><p:otherStyle><a:lvl1pPr><a:defRPr sz=\"1800\"/>"
          "</a:lvl1pPr></p:otherStyle></p:txStyles></p:sldMaster>");
  put("ppt/slideMasters/_rels/slideMaster1.xml.rels", masterRels);
  put("ppt/theme/theme1.xml", std::string(kDecl) + std::string(draw::Theme::DefaultXml()));
  put("ppt/presProps.xml", std::string(kDecl) + "<p:presentationPr" + std::string(kNs) + "/>");
  put("ppt/tableStyles.xml", std::string(kDecl) + "<a:tblStyleLst xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
                                                  "def=\"{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}\"/>");
  put("ppt/presentation.xml", std::string(kDecl) + "<p:presentation" + std::string(kNs) +
                                  " saveSubsetFonts=\"1\"><p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" r:id=\"rId1\"/>"
                                  "</p:sldMasterIdLst>" +
                                  (notesMasterId.empty() ? "" : "<p:notesMasterIdLst><p:notesMasterId r:id=\"" + notesMasterId + "\"/></p:notesMasterIdLst>") +
                                  (slideIds.empty() ? "" : "<p:sldIdLst>" + slideIds + "</p:sldIdLst>") +
                                  "<p:sldSz cx=\"" + N(cx) + "\" cy=\"" + N(cy) + "\"/><p:notesSz cx=\"6858000\" cy=\"9144000\"/></p:presentation>");
  put("ppt/_rels/presentation.xml.rels", presRels + "</Relationships>");
  for (auto& [name, bytes] : media_) {
    const bool packed = name.ends_with(".png") || name.ends_with(".jpeg") || name.ends_with(".gif");
    if (pkg::Status s = zip.Add("ppt/media/" + name, bytes, nullptr, packed ? pkg::ZipWriter::Compression::Store : pkg::ZipWriter::Compression::Deflate);
        !s.Ok())
      return s;
    bytes.clear();
    bytes.shrink_to_fit();
  }
  put("[Content_Types].xml", types + "</Types>");
  put("_rels/.rels", std::string(kDecl) + std::string(kRelsOpen) + "<Relationship Id=\"rId1\" Type=\"" + std::string(kRel) +
                         "officeDocument\" Target=\"ppt/presentation.xml\"/></Relationships>");
  if (pkg::Status s = zip.Finish(); !s.Ok()) return s;
  pptx = std::move(sink.bytes);
  return pkg::Status();
}

}  // namespace

pkg::Status OdpToPptx(std::span<const uint8_t> odp, std::vector<uint8_t>& pptx, const pkg::Limits& limits) {
  pptx.clear();
  auto snapshot = pkg::Snapshot::FromBytes(std::vector<uint8_t>(odp.begin(), odp.end()));
  pkg::ZipReader zip;
  if (pkg::Status st = zip.Open(*snapshot, limits); !st.Ok()) return st;
  Converter c(zip, limits);
  return c.Run(pptx);
}

}  // namespace fw::slide::odp
