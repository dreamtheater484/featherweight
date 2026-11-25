#include "slide/objects/ObjectXml.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>

#include "core/Lang.h"
#include "draw/Xml.h"
#include "slide/edit/TableGrid.h"

namespace fw::slide::objects {

namespace {

using namespace draw::xml;

// ---- Names and prefixes

// The prefix bound to `ns` where `scope` is, "" for a default namespace, else `fallback`.
std::string PrefixFor(pugi::xml_node scope, std::string_view ns, const char* fallback) {
  for (pugi::xml_node n = scope; n; n = n.parent()) {
    if (n.type() != pugi::node_element) continue;
    for (pugi::xml_attribute a : n.attributes()) {
      const std::string_view name = a.name();
      if (ns != a.value()) continue;
      if (name == "xmlns") return {};
      if (name.substr(0, 6) == "xmlns:") return std::string(name.substr(6));
    }
  }
  return fallback;
}

std::string NameIn(pugi::xml_node scope, std::string_view ns, std::string_view local) {
  const char* fallback = ns == kP ? "p" : ns == kR ? "r" : ns == kChart ? "c" : "a";
  const std::string prefix = PrefixFor(scope, ns, fallback);
  return prefix.empty() ? std::string(local) : prefix + ":" + std::string(local);
}

// A direct child element (no mc:AlternateContent: edits never reach into a branch).
pugi::xml_node Kid(pugi::xml_node parent, std::string_view ns, std::string_view local) {
  for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && LocalNameOf(c) == local && NamespaceOf(c) == ns) return c;
  return {};
}

std::vector<pugi::xml_node> Kids(pugi::xml_node parent, std::string_view ns, std::string_view local) {
  std::vector<pugi::xml_node> out;
  for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && LocalNameOf(c) == local && NamespaceOf(c) == ns) out.push_back(c);
  return out;
}

// A new child `local` placed by the schema's `order` of local names: before the first existing
// child that comes after it. Children whose names aren't listed don't move anything.
pugi::xml_node InsertOrdered(pugi::xml_node parent, std::string_view ns, std::string_view local,
                             std::initializer_list<std::string_view> order) {
  auto rank = [&](std::string_view name) {
    int i = 0;
    for (std::string_view o : order) {
      if (o == name) return i;
      ++i;
    }
    return -1;
  };
  const int mine = rank(local);
  const std::string name = NameIn(parent, ns, local);
  for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    const int r = rank(LocalNameOf(c));
    if (r > mine) return parent.insert_child_before(name.c_str(), c);
  }
  return parent.append_child(name.c_str());
}

pugi::xml_node Ensure(pugi::xml_node parent, std::string_view ns, std::string_view local,
                      std::initializer_list<std::string_view> order) {
  if (pugi::xml_node k = Kid(parent, ns, local)) return k;
  return InsertOrdered(parent, ns, local, order);
}

void SetInt(pugi::xml_node n, const char* attr, int64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
  (n.attribute(attr) ? n.attribute(attr) : n.append_attribute(attr)).set_value(buf);
}

void SetAttr(pugi::xml_node n, const char* attr, const char* v) {
  (n.attribute(attr) ? n.attribute(attr) : n.append_attribute(attr)).set_value(v);
}

int64_t Int(pugi::xml_node n, const char* attr, int64_t def = 0) {
  const double d = Number(n, attr, static_cast<double>(def));
  return static_cast<int64_t>(std::llround(d));
}

std::string Hex(uint32_t rgb) {
  char buf[8];
  std::snprintf(buf, sizeof buf, "%06X", rgb & 0xFFFFFF);
  return buf;
}

bool ParseHex(std::string_view v, uint32_t& rgb) {
  if (v.size() != 6) return false;
  uint32_t out = 0;
  for (char c : v) {
    out <<= 4;
    if (c >= '0' && c <= '9') out |= c - '0';
    else if (c >= 'a' && c <= 'f') out |= c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') out |= c - 'A' + 10;
    else return false;
  }
  rgb = out;
  return true;
}

// ---- Fragments: subtrees written with the usual prefixes, renamed to the ones in scope.

void Rename(pugi::xml_node n, const std::string& p, const std::string& a, const std::string& r) {
  auto fix = [&](std::string_view name) -> std::string {
    auto swap = [&](std::string_view from, const std::string& to) {
      std::string rest(name.substr(from.size()));
      return to.empty() ? rest : to + ":" + rest;
    };
    if (name.substr(0, 2) == "p:") return swap("p:", p);
    if (name.substr(0, 2) == "a:") return swap("a:", a);
    if (name.substr(0, 2) == "r:") return swap("r:", r);
    return std::string(name);
  };
  for (pugi::xml_node c = n; c;) {
    if (c.type() == pugi::node_element) {
      c.set_name(fix(c.name()).c_str());
      for (pugi::xml_attribute at : c.attributes()) at.set_name(fix(at.name()).c_str());
    }
    if (c.first_child()) {
      c = c.first_child();
      continue;
    }
    while (c && c != n && !c.next_sibling()) c = c.parent();
    if (!c || c == n) break;
    c = c.next_sibling();
  }
}

// Parses `xml` (standard prefixes) and inserts it into `parent` before `before` (or at the end,
// before a trailing p:extLst).
pugi::xml_node InsertFragment(pugi::xml_node parent, const std::string& xml, pugi::xml_node before = {}) {
  pugi::xml_document doc;
  if (!doc.load_buffer(xml.data(), xml.size(), pugi::parse_default, pugi::encoding_utf8)) return {};
  if (!before)
    for (pugi::xml_node c = parent.last_child(); c; c = c.previous_sibling())
      if (c.type() == pugi::node_element) {
        if (LocalNameOf(c) == "extLst") before = c;
        break;
      }
  pugi::xml_node n = before ? parent.insert_copy_before(doc.document_element(), before)
                            : parent.append_copy(doc.document_element());
  const std::string p = PrefixFor(parent, kP, "p"), a = PrefixFor(parent, kA, "a"), r = PrefixFor(parent, kR, "r");
  if (p != "p" || a != "a" || r != "r") Rename(n, p, a, r);
  return n;
}

std::string Escape(std::string_view s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

std::string XfrmXml(const char* element, const Xfrm& x) {
  char buf[320];
  std::string attrs;
  if (x.rot) attrs += " rot=\"" + std::to_string(x.rot) + "\"";
  if (x.flipH) attrs += " flipH=\"1\"";
  if (x.flipV) attrs += " flipV=\"1\"";
  std::snprintf(buf, sizeof buf, "<%s%s><a:off x=\"%lld\" y=\"%lld\"/><a:ext cx=\"%lld\" cy=\"%lld\"/>", element,
                attrs.c_str(), static_cast<long long>(x.x), static_cast<long long>(x.y), static_cast<long long>(x.cx),
                static_cast<long long>(x.cy));
  std::string out = buf;
  if (x.child) {
    std::snprintf(buf, sizeof buf, "<a:chOff x=\"%lld\" y=\"%lld\"/><a:chExt cx=\"%lld\" cy=\"%lld\"/>",
                  static_cast<long long>(x.chX), static_cast<long long>(x.chY), static_cast<long long>(x.chCx),
                  static_cast<long long>(x.chCy));
    out += buf;
  }
  out += std::string("</") + element + ">";
  return out;
}

std::string CNvPr(uint32_t id, std::string_view name, std::string_view descr = {}) {
  std::string out = "<p:cNvPr id=\"" + std::to_string(id) + "\" name=\"" + Escape(name) + "\"";
  if (!descr.empty()) out += " descr=\"" + Escape(descr) + "\"";
  return out + "/>";
}

constexpr std::initializer_list<std::string_view> kSpPrOrder = {
    "xfrm", "custGeom", "prstGeom", "noFill", "solidFill", "gradFill", "blipFill", "pattFill", "grpFill",
    "ln", "effectLst", "effectDag", "scene3d", "sp3d", "extLst"};
constexpr std::initializer_list<std::string_view> kLnOrder = {
    "noFill", "solidFill", "gradFill", "pattFill", "prstDash", "custDash", "round", "bevel", "miter",
    "headEnd", "tailEnd", "extLst"};
constexpr std::initializer_list<std::string_view> kEffectOrder = {
    "blur", "fillOverlay", "glow", "innerShdw", "outerShdw", "prstShdw", "reflection", "softEdge"};
constexpr std::string_view kFills[] = {"noFill", "solidFill", "gradFill", "blipFill", "pattFill", "grpFill"};

pugi::xml_node FillOf(pugi::xml_node parent) {
  for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element || NamespaceOf(c) != kA) continue;
    for (std::string_view f : kFills)
      if (LocalNameOf(c) == f) return c;
  }
  return {};
}

// The element holding the object's properties: p:spPr, p:grpSpPr; null for a graphic frame.
pugi::xml_node PropertiesOf(pugi::xml_node object, bool make) {
  const ObjectKind k = KindOf(object);
  const char* local = k == ObjectKind::Group ? "grpSpPr" : "spPr";
  if (k == ObjectKind::GraphicFrame || k == ObjectKind::Other) return {};
  if (pugi::xml_node n = Kid(object, kP, local)) return n;
  if (!make) return {};
  // After the non-visual properties (and p:blipFill on a picture).
  pugi::xml_node after;
  for (pugi::xml_node c = object.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element) {
      const std::string_view n = LocalNameOf(c);
      if (n.substr(0, 2) == "nv" || n == "blipFill") after = c;
    }
  const std::string name = NameIn(object, kP, local);
  return after ? object.insert_child_after(name.c_str(), after) : object.prepend_child(name.c_str());
}

Paint ReadPaint(pugi::xml_node fill) {
  Paint p;
  if (!fill) return p;
  const std::string_view n = LocalNameOf(fill);
  auto srgb = [](pugi::xml_node color, uint32_t& rgb) {
    // A plain sRGB color, with no transforms (a tint or shade makes it another color).
    return color && LocalNameOf(color) == "srgbClr" && !color.first_child() && ParseHex(color.attribute("val").value(), rgb);
  };
  if (n == "noFill") {
    p.kind = Paint::Kind::None;
  } else if (n == "solidFill") {
    p.kind = srgb(fill.first_child(), p.rgb) ? Paint::Kind::Solid : Paint::Kind::Other;
  } else if (n == "gradFill") {
    p.kind = Paint::Kind::Other;
    const std::vector<pugi::xml_node> stops = Kids(Kid(fill, kA, "gsLst"), kA, "gs");
    pugi::xml_node lin = Kid(fill, kA, "lin");
    if (stops.size() == 2 && lin && srgb(stops[0].first_child(), p.rgb) && srgb(stops[1].first_child(), p.rgb2)) {
      p.kind = Paint::Kind::Gradient;
      p.angle = static_cast<float>(Number(lin, "ang", 0) / 60000.0);
    }
  } else if (n == "blipFill") {
    p.kind = Paint::Kind::Picture;
    p.relId = std::string(RelId(Kid(fill, kA, "blip"), "embed"));
  } else {
    p.kind = Paint::Kind::Other;
  }
  return p;
}

std::string PaintXml(const Paint& p) {
  switch (p.kind) {
    case Paint::Kind::None: return "<a:noFill/>";
    case Paint::Kind::Solid: return "<a:solidFill><a:srgbClr val=\"" + Hex(p.rgb) + "\"/></a:solidFill>";
    case Paint::Kind::Gradient: {
      const long long ang = std::llround(NormalizeDegrees(p.angle) * 60000.0);
      return "<a:gradFill rotWithShape=\"1\"><a:gsLst><a:gs pos=\"0\"><a:srgbClr val=\"" + Hex(p.rgb) +
             "\"/></a:gs><a:gs pos=\"100000\"><a:srgbClr val=\"" + Hex(p.rgb2) + "\"/></a:gs></a:gsLst><a:lin ang=\"" +
             std::to_string(ang) + "\" scaled=\"0\"/></a:gradFill>";
    }
    case Paint::Kind::Picture:
      return "<a:blipFill dpi=\"0\" rotWithShape=\"1\"><a:blip r:embed=\"" + Escape(p.relId) +
             "\"/><a:srcRect/><a:stretch><a:fillRect/></a:stretch></a:blipFill>";
    default: return {};
  }
}

// Replaces the fill element among `parent`'s children (or removes it for Automatic).
void ReplaceFill(pugi::xml_node parent, const Paint& p, std::initializer_list<std::string_view> order) {
  pugi::xml_node old = FillOf(parent);
  if (p.kind == Paint::Kind::Automatic || p.kind == Paint::Kind::Other) {
    if (old && p.kind == Paint::Kind::Automatic) parent.remove_child(old);
    return;
  }
  pugi::xml_node before = old ? old.next_sibling() : pugi::xml_node();
  if (old) {
    parent.remove_child(old);
  } else {
    // Where the schema puts a fill: before the first child ranked after the fills.
    pugi::xml_node probe = InsertOrdered(parent, kA, "solidFill", order);
    before = probe.next_sibling();
    parent.remove_child(probe);
  }
  InsertFragment(parent, PaintXml(p), before);
}

bool Flag(pugi::xml_node n, const char* attr) { return Bool(n, attr, false); }

}  // namespace

// ---- Kinds and transforms

ObjectKind KindOf(pugi::xml_node object) {
  if (NamespaceOf(object) != kP) return ObjectKind::Other;
  const std::string_view n = LocalNameOf(object);
  return n == "sp"             ? ObjectKind::Shape
         : n == "pic"          ? ObjectKind::Picture
         : n == "cxnSp"        ? ObjectKind::Connector
         : n == "graphicFrame" ? ObjectKind::GraphicFrame
         : n == "grpSp"        ? ObjectKind::Group
                               : ObjectKind::Other;
}

Frame ToFrame(const Xfrm& x) {
  Frame f;
  f.rect = {ToPoints(x.x), ToPoints(x.y), ToPoints(x.x + x.cx), ToPoints(x.y + x.cy)};
  f.rotation = NormalizeDegrees(static_cast<float>(x.rot / 60000.0));
  f.flipH = x.flipH;
  f.flipV = x.flipV;
  return f;
}

Xfrm FromFrame(const Frame& f, const Xfrm& like) {
  Xfrm x = like;
  x.x = ToEmu(f.rect.left);
  x.y = ToEmu(f.rect.top);
  x.cx = std::max<int64_t>(0, ToEmu(f.rect.Width()));
  x.cy = std::max<int64_t>(0, ToEmu(f.rect.Height()));
  x.rot = static_cast<int32_t>(std::llround(NormalizeDegrees(f.rotation) * 60000.0) % 21600000);
  x.flipH = f.flipH;
  x.flipV = f.flipV;
  return x;
}

draw::Matrix ChildSpace(const Xfrm& g, const draw::Matrix& groupToSlide) {
  const float sx = g.child && g.chCx > 0 ? static_cast<float>(static_cast<double>(g.cx) / g.chCx) : 1;
  const float sy = g.child && g.chCy > 0 ? static_cast<float>(static_cast<double>(g.cy) / g.chCy) : 1;
  const float ox = g.child ? ToPoints(g.chX) : ToPoints(g.x), oy = g.child ? ToPoints(g.chY) : ToPoints(g.y);
  return draw::Matrix::Translate(-ox, -oy) * draw::Matrix::Scale(sx, sy) * groupToSlide;
}

namespace {

struct Decomposed {
  float sx = 1, sy = 1, angle = 0;
  bool mirrored = false;
};

Decomposed Decompose(const draw::Matrix& m) {
  Decomposed d;
  d.sx = std::hypot(m.m11, m.m12);
  d.sy = std::hypot(m.m21, m.m22);
  d.mirrored = static_cast<double>(m.m11) * m.m22 - static_cast<double>(m.m12) * m.m21 < 0;
  d.angle = static_cast<float>(d.mirrored ? std::atan2(-m.m12, -m.m11) : std::atan2(m.m12, m.m11)) * 180 / 3.14159265f;
  if (d.sx <= 0) d.sx = 1;
  if (d.sy <= 0) d.sy = 1;
  return d;
}

}  // namespace

Frame FrameOnSlide(const Frame& local, const draw::Matrix& p) {
  const Decomposed d = Decompose(p);
  const draw::Point c = p.Apply(local.Centre());
  const float w = local.rect.Width() * d.sx, h = local.rect.Height() * d.sy;
  Frame f = local;
  f.rect = {c.x - w / 2, c.y - h / 2, c.x + w / 2, c.y + h / 2};
  if (d.mirrored) {
    f.flipH = !local.flipH;
    f.rotation = NormalizeDegrees(d.angle - local.rotation);
  } else {
    f.rotation = NormalizeDegrees(local.rotation + d.angle);
  }
  if (std::fabs(f.rotation - std::round(f.rotation)) < 0.001f) f.rotation = NormalizeDegrees(std::round(f.rotation));
  return f;
}

Frame FrameInParent(const Frame& onSlide, const draw::Matrix& p) {
  const Decomposed d = Decompose(p);
  const draw::Point c = p.Inverse().Apply(onSlide.Centre());
  const float w = onSlide.rect.Width() / d.sx, h = onSlide.rect.Height() / d.sy;
  Frame f = onSlide;
  f.rect = {c.x - w / 2, c.y - h / 2, c.x + w / 2, c.y + h / 2};
  if (d.mirrored) {
    f.flipH = !onSlide.flipH;
    f.rotation = NormalizeDegrees(d.angle - onSlide.rotation);
  } else {
    f.rotation = NormalizeDegrees(onSlide.rotation - d.angle);
  }
  if (std::fabs(f.rotation - std::round(f.rotation)) < 0.001f) f.rotation = NormalizeDegrees(std::round(f.rotation));
  return f;
}

draw::Point DeltaInParent(draw::Point delta, const draw::Matrix& p) {
  const draw::Matrix q = p.Inverse();
  return {delta.x * q.m11 + delta.y * q.m21, delta.x * q.m12 + delta.y * q.m22};
}

void MoveBy(pugi::xml_node object, int64_t dx, int64_t dy) {
  Xfrm x;
  if (!ReadXfrm(object, x)) return;
  x.x += dx;
  x.y += dy;
  WriteXfrm(object, x);
}

// ---- Identity

namespace {
pugi::xml_node NonVisual(pugi::xml_node object) {
  for (pugi::xml_node c = object.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && LocalNameOf(c).substr(0, 2) == "nv") return c;
  return {};
}
pugi::xml_node CNvPrOf(pugi::xml_node object) { return Kid(NonVisual(object), kP, "cNvPr"); }
}  // namespace

uint32_t ObjectId(pugi::xml_node object) { return static_cast<uint32_t>(Int(CNvPrOf(object), "id", 0)); }
void SetObjectId(pugi::xml_node object, uint32_t id) {
  if (pugi::xml_node c = CNvPrOf(object)) SetInt(c, "id", id);
}
std::string ObjectName(pugi::xml_node object) { return CNvPrOf(object).attribute("name").value(); }
void SetObjectName(pugi::xml_node object, std::string_view name) {
  if (pugi::xml_node c = CNvPrOf(object)) SetAttr(c, "name", std::string(name).c_str());
}
std::string AltText(pugi::xml_node object) { return CNvPrOf(object).attribute("descr").value(); }
void SetAltText(pugi::xml_node object, std::string_view text) {
  pugi::xml_node c = CNvPrOf(object);
  if (!c) return;
  if (text.empty()) c.remove_attribute("descr");
  else SetAttr(c, "descr", std::string(text).c_str());
}
bool IsPlaceholder(pugi::xml_node object) { return Kid(Kid(NonVisual(object), kP, "nvPr"), kP, "ph"); }

uint32_t MaxObjectId(pugi::xml_node tree) {
  uint32_t best = 0;
  std::vector<pugi::xml_node> stack{tree};
  while (!stack.empty()) {
    pugi::xml_node n = stack.back();
    stack.pop_back();
    for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
      if (c.type() == pugi::node_element) stack.push_back(c);
    if (LocalNameOf(n) == "cNvPr") best = std::max(best, static_cast<uint32_t>(std::clamp<int64_t>(Int(n, "id", 0), 0, 0xFFFFFFFF)));
  }
  return best;
}

std::string NewName(std::string_view base, uint32_t id) { return std::string(base) + " " + std::to_string(id > 0 ? id - 1 : 0); }

// ---- New objects

pugi::xml_node AppendShape(pugi::xml_node parent, uint32_t id, std::string_view preset, const Xfrm& at,
                           std::string_view name) {
  const std::string xml =
      "<p:sp><p:nvSpPr>" + CNvPr(id, name) + "<p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr>" + XfrmXml("a:xfrm", at) +
      "<a:prstGeom prst=\"" + Escape(preset) +
      "\"><a:avLst/></a:prstGeom></p:spPr><p:style><a:lnRef idx=\"2\"><a:schemeClr val=\"accent1\"><a:shade "
      "val=\"50000\"/></a:schemeClr></a:lnRef><a:fillRef idx=\"1\"><a:schemeClr val=\"accent1\"/></a:fillRef><a:effectRef "
      "idx=\"0\"><a:schemeClr val=\"accent1\"/></a:effectRef><a:fontRef idx=\"minor\"><a:schemeClr "
      "val=\"lt1\"/></a:fontRef></p:style><p:txBody><a:bodyPr rtlCol=\"0\" anchor=\"ctr\"/><a:lstStyle/><a:p><a:pPr "
      "algn=\"ctr\"/><a:endParaRPr dirty=\"0\"/></a:p></p:txBody></p:sp>";
  return InsertFragment(parent, xml);
}

pugi::xml_node AppendTextBox(pugi::xml_node parent, uint32_t id, const Xfrm& at) {
  const std::string xml = "<p:sp><p:nvSpPr>" + CNvPr(id, NewName("TextBox", id)) +
                          "<p:cNvSpPr txBox=\"1\"/><p:nvPr/></p:nvSpPr><p:spPr>" + XfrmXml("a:xfrm", at) +
                          "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:noFill/></p:spPr><p:txBody><a:bodyPr "
                          "wrap=\"square\" rtlCol=\"0\"><a:spAutoFit/></a:bodyPr><a:lstStyle/><a:p><a:endParaRPr "
                          "dirty=\"0\"/></a:p></p:txBody></p:sp>";
  return InsertFragment(parent, xml);
}

pugi::xml_node AppendLine(pugi::xml_node parent, uint32_t id, const Xfrm& at, bool arrow) {
  const std::string xml =
      "<p:cxnSp><p:nvCxnSpPr>" + CNvPr(id, NewName(arrow ? "Straight Arrow Connector" : "Straight Connector", id)) +
      "<p:cNvCxnSpPr/><p:nvPr/></p:nvCxnSpPr><p:spPr>" + XfrmXml("a:xfrm", at) + "<a:prstGeom prst=\"" +
      (arrow ? "straightConnector1" : "line") + "\"><a:avLst/></a:prstGeom>" +
      (arrow ? "<a:ln><a:tailEnd type=\"triangle\"/></a:ln>" : "") +
      "</p:spPr><p:style><a:lnRef idx=\"1\"><a:schemeClr val=\"accent1\"/></a:lnRef><a:fillRef idx=\"0\"><a:schemeClr "
      "val=\"accent1\"/></a:fillRef><a:effectRef idx=\"0\"><a:schemeClr val=\"accent1\"/></a:effectRef><a:fontRef "
      "idx=\"minor\"><a:schemeClr val=\"tx1\"/></a:fontRef></p:style></p:cxnSp>";
  return InsertFragment(parent, xml);
}

pugi::xml_node AppendPicture(pugi::xml_node parent, uint32_t id, std::string_view relId, const Xfrm& at,
                             std::string_view name, std::string_view altText) {
  const std::string xml = "<p:pic><p:nvPicPr>" + CNvPr(id, name.empty() ? NewName("Picture", id) : std::string(name), altText) +
                          "<p:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></p:cNvPicPr><p:nvPr/></p:nvPicPr><p:blipFill><a:blip "
                          "r:embed=\"" + Escape(relId) + "\"/><a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr>" +
                          XfrmXml("a:xfrm", at) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>";
  return InsertFragment(parent, xml);
}

pugi::xml_node AppendTable(pugi::xml_node parent, uint32_t id, const Xfrm& at, int rows, int columns,
                           std::string_view styleId) {
  rows = std::clamp(rows, 1, 75);
  columns = std::clamp(columns, 1, 75);
  const int64_t colW = std::max<int64_t>(1, at.cx / columns), rowH = std::max<int64_t>(1, at.cy / rows);
  std::string xml = "<p:graphicFrame><p:nvGraphicFramePr>" + CNvPr(id, NewName("Table", id)) +
                    "<p:cNvGraphicFramePr><a:graphicFrameLocks noGrp=\"1\"/></p:cNvGraphicFramePr><p:nvPr/>"
                    "</p:nvGraphicFramePr>";
  Xfrm x = at;
  x.cx = colW * columns;
  x.cy = rowH * rows;
  x.rot = 0;
  x.flipH = x.flipV = false;
  xml += XfrmXml("p:xfrm", x);
  xml += "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/table\"><a:tbl><a:tblPr "
         "firstRow=\"1\" bandRow=\"1\"><a:tableStyleId>";
  xml += styleId.empty() ? std::string("{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}") : Escape(styleId);
  xml += "</a:tableStyleId></a:tblPr><a:tblGrid>";
  for (int c = 0; c < columns; ++c) xml += "<a:gridCol w=\"" + std::to_string(colW) + "\"/>";
  xml += "</a:tblGrid>";
  for (int r = 0; r < rows; ++r) {
    xml += "<a:tr h=\"" + std::to_string(rowH) + "\">";
    for (int c = 0; c < columns; ++c)
      xml += "<a:tc><a:txBody><a:bodyPr/><a:lstStyle/><a:p><a:endParaRPr dirty=\"0\"/></a:p></a:txBody><a:tcPr/></a:tc>";
    xml += "</a:tr>";
  }
  xml += "</a:tbl></a:graphicData></a:graphic></p:graphicFrame>";
  return InsertFragment(parent, xml);
}

pugi::xml_node AppendChart(pugi::xml_node parent, uint32_t id, std::string_view relId, const Xfrm& at) {
  Xfrm x = at;
  x.rot = 0;
  x.flipH = x.flipV = false;
  const std::string xml = "<p:graphicFrame><p:nvGraphicFramePr>" + CNvPr(id, NewName("Chart", id)) +
                          "<p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr>" + XfrmXml("p:xfrm", x) +
                          "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/chart\"><c:chart "
                          "xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\" "
                          "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" r:id=\"" +
                          Escape(relId) + "\"/></a:graphicData></a:graphic></p:graphicFrame>";
  return InsertFragment(parent, xml);
}

pugi::xml_node AppendGroup(pugi::xml_node parent, uint32_t id, const Xfrm& at) {
  Xfrm x = at;
  x.child = true;
  x.chX = at.x, x.chY = at.y, x.chCx = at.cx, x.chCy = at.cy;
  x.rot = 0;
  x.flipH = x.flipV = false;
  const std::string xml = "<p:grpSp><p:nvGrpSpPr>" + CNvPr(id, NewName("Group", id)) +
                          "<p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr>" + XfrmXml("a:xfrm", x) +
                          "</p:grpSpPr></p:grpSp>";
  return InsertFragment(parent, xml);
}

draw::Rect PlacePicture(draw::Size natural, draw::Size slide) {
  float w = natural.width > 0 ? natural.width : 72, h = natural.height > 0 ? natural.height : 72;
  const float s = std::min({1.f, slide.width / w, slide.height / h});
  w *= s;
  h *= s;
  return {(slide.width - w) / 2, (slide.height - h) / 2, (slide.width + w) / 2, (slide.height + h) / 2};
}

// ---- Format

Format ReadFormat(pugi::xml_node object) {
  Format f;
  const ObjectKind k = KindOf(object);
  f.fillable = k == ObjectKind::Shape || k == ObjectKind::Picture;
  f.lineable = k == ObjectKind::Shape || k == ObjectKind::Picture || k == ObjectKind::Connector;
  f.picture = k == ObjectKind::Picture;
  pugi::xml_node pr = PropertiesOf(object, false);
  if (!pr) return f;
  if (f.fillable) f.fill = ReadPaint(FillOf(pr));
  if (pugi::xml_node ln = Kid(pr, kA, "ln")) {
    f.line = ReadPaint(FillOf(ln));
    f.lineWidth = ToPoints(Int(ln, "w", 0));
    if (pugi::xml_node d = Kid(ln, kA, "prstDash")) f.dash = d.attribute("val").value();
  }
  pugi::xml_node effects = Kid(pr, kA, "effectLst");
  f.shadow = effects && (Kid(effects, kA, "outerShdw") || Kid(effects, kA, "prstShdw"));
  return f;
}

bool SetFill(pugi::xml_node object, const Paint& paint) {
  const ObjectKind k = KindOf(object);
  if (k != ObjectKind::Shape && k != ObjectKind::Picture) return false;
  if (paint.kind == Paint::Kind::Other) return false;
  pugi::xml_node pr = PropertiesOf(object, true);
  if (!pr) return false;
  ReplaceFill(pr, paint, kSpPrOrder);
  return true;
}

bool SetLine(pugi::xml_node object, const Paint& paint) {
  const ObjectKind k = KindOf(object);
  if (k != ObjectKind::Shape && k != ObjectKind::Picture && k != ObjectKind::Connector) return false;
  if (paint.kind == Paint::Kind::Other || paint.kind == Paint::Kind::Picture) return false;
  Paint p = paint;
  if (p.kind == Paint::Kind::Gradient) p.kind = Paint::Kind::Solid;
  pugi::xml_node pr = PropertiesOf(object, true);
  if (!pr) return false;
  pugi::xml_node ln = Kid(pr, kA, "ln");
  if (!ln) {
    if (p.kind == Paint::Kind::Automatic) return true;
    ln = InsertOrdered(pr, kA, "ln", kSpPrOrder);
  }
  ReplaceFill(ln, p, kLnOrder);
  if (p.kind == Paint::Kind::Automatic && !ln.first_child() && !ln.first_attribute()) pr.remove_child(ln);
  return true;
}

bool SetLineWidth(pugi::xml_node object, float points) {
  const ObjectKind k = KindOf(object);
  if (k != ObjectKind::Shape && k != ObjectKind::Picture && k != ObjectKind::Connector) return false;
  pugi::xml_node pr = PropertiesOf(object, true);
  pugi::xml_node ln = Ensure(pr, kA, "ln", kSpPrOrder);
  if (points > 0) SetInt(ln, "w", std::clamp<int64_t>(ToEmu(points), 0, 20116800));
  else ln.remove_attribute("w");
  if (!ln.first_child() && !ln.first_attribute()) pr.remove_child(ln);
  return true;
}

bool SetLineDash(pugi::xml_node object, std::string_view preset) {
  const ObjectKind k = KindOf(object);
  if (k != ObjectKind::Shape && k != ObjectKind::Picture && k != ObjectKind::Connector) return false;
  pugi::xml_node pr = PropertiesOf(object, true);
  pugi::xml_node ln = Ensure(pr, kA, "ln", kSpPrOrder);
  if (pugi::xml_node c = Kid(ln, kA, "custDash")) ln.remove_child(c);
  pugi::xml_node d = Kid(ln, kA, "prstDash");
  if (preset.empty()) {
    if (d) ln.remove_child(d);
  } else {
    if (!d) d = InsertOrdered(ln, kA, "prstDash", kLnOrder);
    SetAttr(d, "val", std::string(preset).c_str());
  }
  if (!ln.first_child() && !ln.first_attribute()) pr.remove_child(ln);
  return true;
}

bool SetShadow(pugi::xml_node object, bool on) {
  const ObjectKind k = KindOf(object);
  if (k != ObjectKind::Shape && k != ObjectKind::Picture && k != ObjectKind::Connector && k != ObjectKind::Group) return false;
  pugi::xml_node pr = PropertiesOf(object, true);
  if (!pr || Kid(pr, kA, "effectDag")) return false;
  pugi::xml_node effects = Ensure(pr, kA, "effectLst",
                                  k == ObjectKind::Group ? std::initializer_list<std::string_view>{"xfrm", "noFill", "solidFill", "gradFill", "blipFill", "pattFill", "grpFill", "effectLst", "effectDag", "scene3d", "extLst"}
                                                         : kSpPrOrder);
  for (const char* n : {"outerShdw", "prstShdw"})
    while (pugi::xml_node s = Kid(effects, kA, n)) effects.remove_child(s);
  if (on) {
    pugi::xml_node s = InsertOrdered(effects, kA, "outerShdw", kEffectOrder);
    SetAttr(s, "blurRad", "50800");
    SetAttr(s, "dist", "38100");
    SetAttr(s, "dir", "2700000");
    SetAttr(s, "algn", "tl");
    SetAttr(s, "rotWithShape", "0");
    pugi::xml_node c = s.append_child(NameIn(s, kA, "prstClr").c_str());
    SetAttr(c, "val", "black");
    SetAttr(c.append_child(NameIn(s, kA, "alpha").c_str()), "val", "40000");
  }
  return true;
}

Crop ReadCrop(pugi::xml_node picture) {
  Crop c;
  pugi::xml_node src = Kid(Kid(picture, kP, "blipFill"), kA, "srcRect");
  if (!src) return c;
  c.left = static_cast<float>(Number(src, "l", 0) / 100000.0);
  c.top = static_cast<float>(Number(src, "t", 0) / 100000.0);
  c.right = static_cast<float>(Number(src, "r", 0) / 100000.0);
  c.bottom = static_cast<float>(Number(src, "b", 0) / 100000.0);
  return c;
}

bool SetCrop(pugi::xml_node picture, const Crop& c) {
  if (KindOf(picture) != ObjectKind::Picture) return false;
  pugi::xml_node fill = Kid(picture, kP, "blipFill");
  if (!fill || !Kid(fill, kA, "blip")) return false;
  pugi::xml_node src = Ensure(fill, kA, "srcRect", {"blip", "srcRect", "tile", "stretch"});
  const std::pair<const char*, float> sides[] = {{"l", c.left}, {"t", c.top}, {"r", c.right}, {"b", c.bottom}};
  for (const auto& [name, v] : sides) {
    const long long n = std::llround(std::clamp(v, -10.f, 1.f) * 100000.0);
    if (n == 0) src.remove_attribute(name);
    else SetInt(src, name, n);
  }
  return true;
}

Crop CropForFrame(const Crop& c, const draw::Rect& before, const draw::Rect& after) {
  const float shownW = 1 - c.left - c.right, shownH = 1 - c.top - c.bottom;
  if (shownW <= 0.0001f || shownH <= 0.0001f || before.Width() <= 0 || before.Height() <= 0) return c;
  const float imgW = before.Width() / shownW, imgH = before.Height() / shownH;
  const float imgL = before.left - c.left * imgW, imgT = before.top - c.top * imgH;
  Crop out;
  out.left = (after.left - imgL) / imgW;
  out.right = (imgL + imgW - after.right) / imgW;
  out.top = (after.top - imgT) / imgH;
  out.bottom = (imgT + imgH - after.bottom) / imgH;
  return out;
}

// ---- Tables

pugi::xml_node TableOf(pugi::xml_node frame) {
  if (KindOf(frame) != ObjectKind::GraphicFrame) return {};
  return Kid(Kid(Kid(frame, kA, "graphic"), kA, "graphicData"), kA, "tbl");
}

int TableRows(pugi::xml_node frame) { return static_cast<int>(Kids(TableOf(frame), kA, "tr").size()); }
int TableColumns(pugi::xml_node frame) {
  return static_cast<int>(Kids(Kid(TableOf(frame), kA, "tblGrid"), kA, "gridCol").size());
}

bool InsertTableRow(pugi::xml_node frame, int at) {
  return KindOf(frame) == ObjectKind::GraphicFrame && fw::slide::edit::table::InsertRow(frame, at);
}

bool InsertTableColumn(pugi::xml_node frame, int at) {
  return KindOf(frame) == ObjectKind::GraphicFrame && fw::slide::edit::table::InsertColumn(frame, at);
}

bool DeleteTableRow(pugi::xml_node frame, int row) {
  return KindOf(frame) == ObjectKind::GraphicFrame && fw::slide::edit::table::DeleteRow(frame, row);
}

bool DeleteTableColumn(pugi::xml_node frame, int column) {
  return KindOf(frame) == ObjectKind::GraphicFrame && fw::slide::edit::table::DeleteColumn(frame, column);
}

void SetTableStyle(pugi::xml_node frame, std::string_view styleId) {
  pugi::xml_node tbl = TableOf(frame);
  if (!tbl) return;
  pugi::xml_node pr = Ensure(tbl, kA, "tblPr", {"tblPr", "tblGrid", "tr", "extLst"});
  pugi::xml_node id = Kid(pr, kA, "tableStyleId");
  if (pugi::xml_node style = Kid(pr, kA, "tableStyle")) pr.remove_child(style);
  if (styleId.empty()) {
    if (id) pr.remove_child(id);
    return;
  }
  if (!id) id = InsertOrdered(pr, kA, "tableStyleId",
                              {"noFill", "solidFill", "gradFill", "blipFill", "pattFill", "grpFill", "effectLst", "effectDag", "tableStyle", "tableStyleId", "extLst"});
  id.text().set(std::string(styleId).c_str());
}

std::string TableStyle(pugi::xml_node frame) {
  return Kid(Kid(TableOf(frame), kA, "tblPr"), kA, "tableStyleId").text().get();
}

bool TableOption(pugi::xml_node frame, const char* option) { return Flag(Kid(TableOf(frame), kA, "tblPr"), option); }

void SetTableOption(pugi::xml_node frame, const char* option, bool on) {
  pugi::xml_node tbl = TableOf(frame);
  if (!tbl) return;
  pugi::xml_node pr = Ensure(tbl, kA, "tblPr", {"tblPr", "tblGrid", "tr", "extLst"});
  if (on) SetAttr(pr, option, "1");
  else pr.remove_attribute(option);
}

std::string ToXml(pugi::xml_node node) {
  std::ostringstream out;
  node.print(out, "", pugi::format_raw);
  return out.str();
}

std::string Standalone(pugi::xml_node object) {
  pugi::xml_document doc;
  pugi::xml_node copy = doc.append_copy(object);
  for (auto [ns, fallback] : {std::pair{kP, "p"}, std::pair{kA, "a"}, std::pair{kR, "r"}}) {
    const std::string prefix = PrefixFor(object, ns, fallback);
    const std::string attr = prefix.empty() ? std::string("xmlns") : "xmlns:" + prefix;
    if (!copy.attribute(attr.c_str())) copy.append_attribute(attr.c_str()).set_value(std::string(ns).c_str());
  }
  return ToXml(copy);
}

std::string NewObjectXml(const std::function<pugi::xml_node(pugi::xml_node tree)>& build) {
  pugi::xml_document doc;
  doc.load_string(
      "<p:spTree xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
      "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
      "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"/>");
  pugi::xml_node made = build(doc.document_element());
  return made ? Standalone(made) : std::string();
}

const std::vector<ShapePreset>& ShapeGallery() {
  static const std::vector<ShapePreset> kGallery = {
      {"line", "Line"},
      {"straightConnector1", "Arrow"},
      {"rect", "Rectangle"},
      {"roundRect", "Rectangle: Rounded Corners"},
      {"snip1Rect", "Rectangle: Single Corner Snipped"},
      {"ellipse", "Oval"},
      {"triangle", "Isosceles Triangle"},
      {"rtTriangle", "Right Triangle"},
      {"parallelogram", "Parallelogram"},
      {"trapezoid", "Trapezoid"},
      {"diamond", "Diamond"},
      {"pentagon", "Regular Pentagon"},
      {"hexagon", "Hexagon"},
      {"octagon", "Octagon"},
      {"plus", "Cross"},
      {"donut", "Circle: Hollow"},
      {"can", "Cylinder"},
      {"cube", "Cube"},
      {"frame", "Frame"},
      {"heart", "Heart"},
      {"lightningBolt", "Lightning Bolt"},
      {"sun", "Sun"},
      {"moon", "Moon"},
      {"smileyFace", "Smiley Face"},
      {"cloud", "Cloud"},
      {"rightArrow", "Arrow: Right"},
      {"leftArrow", "Arrow: Left"},
      {"upArrow", "Arrow: Up"},
      {"downArrow", "Arrow: Down"},
      {"leftRightArrow", "Arrow: Left-Right"},
      {"upDownArrow", "Arrow: Up-Down"},
      {"chevron", "Arrow: Chevron"},
      {"homePlate", "Arrow: Pentagon"},
      {"mathPlus", "Plus Sign"},
      {"mathMinus", "Minus Sign"},
      {"mathMultiply", "Multiplication Sign"},
      {"mathEqual", "Equals"},
      {"flowChartProcess", "Flowchart: Process"},
      {"flowChartDecision", "Flowchart: Decision"},
      {"flowChartInputOutput", "Flowchart: Data"},
      {"flowChartTerminator", "Flowchart: Terminator"},
      {"flowChartDocument", "Flowchart: Document"},
      {"star4", "Star: 4 Points"},
      {"star5", "Star: 5 Points"},
      {"star6", "Star: 6 Points"},
      {"star8", "Star: 8 Points"},
      {"wave", "Wave"},
      {"wedgeRectCallout", "Speech Bubble: Rectangle"},
      {"wedgeRoundRectCallout", T("Speech Bubble: Rectangle with Corners Rounded")},
      {"wedgeEllipseCallout", "Speech Bubble: Oval"},
      {"cloudCallout", "Thought Bubble: Cloud"},
  };
  return kGallery;
}

bool IsLinePreset(std::string_view preset) { return preset == "line" || preset == "straightConnector1"; }

}  // namespace fw::slide::objects
