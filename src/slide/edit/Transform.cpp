// A shape's transform read and written, and grouping math (P2; from p2-objects' ObjectXml, which now
// calls these: one copy).
#include "slide/edit/Transform.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "slide/edit/EditInternal.h"

namespace fw::slide::edit::xfrm {

namespace {

int64_t Int(pugi::xml_node n, const char* attr) { return n.attribute(attr).as_llong(0); }

void SetInt(pugi::xml_node n, const char* attr, int64_t v) {
  (n.attribute(attr) ? n.attribute(attr) : n.append_attribute(attr)).set_value(static_cast<long long>(v));
}

bool Flag(pugi::xml_node n, const char* attr) {
  const std::string_view v = n.attribute(attr).value();
  return v == "1" || v == "true" || v == "on";
}

std::string Name(pugi::xml_node scope, std::string_view ns, std::string_view fallback, std::string_view local) {
  const std::string p = PrefixFor(scope, ns, fallback);
  return p.empty() ? std::string(local) : p + ":" + std::string(local);
}

// The child `local` of `parent` (namespace a:), made before the first of the later ones when missing.
pugi::xml_node Ensure(pugi::xml_node parent, std::string_view local, std::initializer_list<std::string_view> order) {
  if (pugi::xml_node c = DirectChild(parent, kA, local)) return c;
  bool later = false;
  pugi::xml_node before;
  for (std::string_view o : order) {
    if (later && !before) before = DirectChild(parent, kA, o);
    if (o == local) later = true;
  }
  const std::string name = Name(parent, kA, "a", local);
  return before ? parent.insert_child_before(name.c_str(), before) : parent.append_child(name.c_str());
}

constexpr double kPi = 3.14159265358979323846;

}  // namespace

pugi::xml_node XfrmOf(pugi::xml_node object, bool create) {
  const std::string_view k = LocalNameOf(object);
  if (k == "graphicFrame") {
    pugi::xml_node x = DirectChild(object, kP, "xfrm");
    if (!x && create) {
      const std::string name = Name(object, kP, "p", "xfrm");
      pugi::xml_node after = DirectChild(object, kP, "nvGraphicFramePr");
      x = after ? object.insert_child_after(name.c_str(), after) : object.prepend_child(name.c_str());
    }
    return x;
  }
  if (k != "sp" && k != "pic" && k != "cxnSp" && k != "grpSp" && k != "contentPart") return {};
  const char* propsName = k == "grpSp" ? "grpSpPr" : "spPr";
  pugi::xml_node props = DirectChild(object, kP, propsName);
  if (!props) {
    if (!create) return {};
    props = AddChildOrdered(object, Name(object, kP, "p", propsName),
                            {"style", "txBody", "extLst", "sp", "pic", "grpSp", "graphicFrame", "cxnSp", "contentPart"});
  }
  pugi::xml_node x = DirectChild(props, kA, "xfrm");
  if (!x && create) x = props.prepend_child(Name(props, kA, "a", "xfrm").c_str());
  return x;
}

bool Read(pugi::xml_node object, Box& out) {
  pugi::xml_node x = XfrmOf(object, false);
  if (!x) return false;
  out = Box();
  pugi::xml_node off = DirectChild(x, kA, "off"), ext = DirectChild(x, kA, "ext");
  out.x = Int(off, "x");
  out.y = Int(off, "y");
  out.cx = std::max<int64_t>(0, Int(ext, "cx"));
  out.cy = std::max<int64_t>(0, Int(ext, "cy"));
  out.rot = static_cast<int32_t>(Int(x, "rot") % 21600000);
  out.flipH = Flag(x, "flipH");
  out.flipV = Flag(x, "flipV");
  pugi::xml_node chOff = DirectChild(x, kA, "chOff"), chExt = DirectChild(x, kA, "chExt");
  if (chOff && chExt) {
    out.child = true;
    out.chX = Int(chOff, "x");
    out.chY = Int(chOff, "y");
    out.chCx = Int(chExt, "cx");
    out.chCy = Int(chExt, "cy");
  }
  return true;
}

void Write(pugi::xml_node object, const Box& v) {
  pugi::xml_node x = XfrmOf(object, true);
  if (!x) return;
  if (v.rot) SetInt(x, "rot", v.rot);
  else x.remove_attribute("rot");
  for (auto [name, on] : {std::pair{"flipH", v.flipH}, std::pair{"flipV", v.flipV}}) {
    if (on) (x.attribute(name) ? x.attribute(name) : x.append_attribute(name)).set_value("1");
    else x.remove_attribute(name);
  }
  // static: a function-local constexpr initializer_list's array has automatic storage, which
  // newer MSVC (CI's) rejects as not a constant expression.
  static constexpr std::initializer_list<std::string_view> order = {"off", "ext", "chOff", "chExt"};
  pugi::xml_node off = Ensure(x, "off", order), ext = Ensure(x, "ext", order);
  SetInt(off, "x", v.x);
  SetInt(off, "y", v.y);
  SetInt(ext, "cx", v.cx);
  SetInt(ext, "cy", v.cy);
  if (v.child) {
    pugi::xml_node chOff = Ensure(x, "chOff", order), chExt = Ensure(x, "chExt", order);
    SetInt(chOff, "x", v.chX);
    SetInt(chOff, "y", v.chY);
    SetInt(chExt, "cx", v.chCx);
    SetInt(chExt, "cy", v.chCy);
  }
}

Box GroupBox(const std::vector<Box>& children) {
  Box g;
  if (children.empty()) return g;
  double l = 1e300, t = 1e300, r = -1e300, b = -1e300;
  for (const Box& c : children) {
    if (c.rot == 0) {
      l = std::min(l, double(c.x)), t = std::min(t, double(c.y));
      r = std::max(r, double(c.x + c.cx)), b = std::max(b, double(c.y + c.cy));
      continue;
    }
    // A rotated child by its turned corners.
    const double mx = c.x + c.cx / 2.0, my = c.y + c.cy / 2.0, a = c.rot / 60000.0 * kPi / 180.0;
    for (auto [dx, dy] : {std::pair{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}}) {
      const double px = dx * c.cx / 2.0, py = dy * c.cy / 2.0;
      const double qx = mx + px * std::cos(a) - py * std::sin(a), qy = my + px * std::sin(a) + py * std::cos(a);
      l = std::min(l, qx), t = std::min(t, qy), r = std::max(r, qx), b = std::max(b, qy);
    }
  }
  g.x = std::llround(l), g.y = std::llround(t);
  g.cx = std::llround(r) - g.x, g.cy = std::llround(b) - g.y;
  g.child = true;
  g.chX = g.x, g.chY = g.y, g.chCx = g.cx, g.chCy = g.cy;
  return g;
}

Box Ungrouped(const Box& g, const Box& c) {
  const double sx = g.child && g.chCx > 0 ? double(g.cx) / g.chCx : 1;
  const double sy = g.child && g.chCy > 0 ? double(g.cy) / g.chCy : 1;
  const double ox = static_cast<double>(g.child ? g.chX : g.x), oy = static_cast<double>(g.child ? g.chY : g.y);
  // The child's centre in the group's box, from the box's centre.
  double px = (c.x + c.cx / 2.0 - ox) * sx - g.cx / 2.0;
  double py = (c.y + c.cy / 2.0 - oy) * sy - g.cy / 2.0;
  if (g.flipH) px = -px;
  if (g.flipV) py = -py;
  const double a = g.rot / 60000.0 * kPi / 180.0;
  const double rx = px * std::cos(a) - py * std::sin(a), ry = px * std::sin(a) + py * std::cos(a);
  const double cx = g.x + g.cx / 2.0 + rx, cy = g.y + g.cy / 2.0 + ry;
  Box out = c;
  out.cx = std::llround(c.cx * sx);
  out.cy = std::llround(c.cy * sy);
  out.x = std::llround(cx - out.cx / 2.0);
  out.y = std::llround(cy - out.cy / 2.0);
  int64_t rot = c.rot;
  if (g.flipH != g.flipV) rot = -rot;
  rot += g.rot;
  rot %= 21600000;
  if (rot < 0) rot += 21600000;
  out.rot = static_cast<int32_t>(rot);
  out.flipH = c.flipH != g.flipH;
  out.flipV = c.flipV != g.flipV;
  return out;
}

}  // namespace fw::slide::edit::xfrm
