// Shape operations (P2, p2-model; for p2-objects and p2-text): insert, delete, move, resize, rotate,
// z-order, group and ungroup, properties, crop, links.
#include <algorithm>
#include <cmath>
#include <sstream>

#include "core/Strings.h"
#include "pkg/XmlWriter.h"
#include "slide/edit/EditInternal.h"
#include "slide/edit/ShapeXml.h"
#include "slide/edit/SlideCopy.h"

namespace fw::slide::edit {

namespace {

constexpr double kEmu = 12700.0;

long long ToEmu(float pt) { return std::llround(static_cast<double>(pt) * kEmu); }

uint32_t U32(pugi::xml_node n, const char* attr) {
  double d = Number(n, attr, 0);
  return d >= 0 && d <= 4294967295.0 ? static_cast<uint32_t>(d) : 0;
}

bool IsShapeElement(pugi::xml_node c) {
  if (c.type() != pugi::node_element) return false;
  std::string_view k = LocalNameOf(c);
  if (Is(c, pkg::mce::kNamespace, "AlternateContent")) return true;
  return k == "sp" || k == "pic" || k == "cxnSp" || k == "graphicFrame" || k == "grpSp" || k == "contentPart";
}

// What moves when a shape moves in z order or is deleted: the shape, or the mc:AlternateContent
// holding it.
pugi::xml_node UnitOf(pugi::xml_node shape) {
  for (pugi::xml_node n = shape; n.parent(); n = n.parent()) {
    pugi::xml_node p = n.parent();
    if (Is(p, pkg::mce::kNamespace, "Choice") || Is(p, pkg::mce::kNamespace, "Fallback")) return p.parent();
    if (LocalNameOf(p) == "spTree" || LocalNameOf(p) == "grpSp") return n;
  }
  return shape;
}

pugi::xml_node CNvPr(pugi::xml_node shape) {
  for (pugi::xml_node n = shape.first_child(); n; n = n.next_sibling()) {
    std::string_view k = LocalNameOf(n);
    if (k.size() > 4 && k.starts_with("nv") && k.ends_with("Pr")) return P(n, "cNvPr");
  }
  return {};
}

// Every shape id in a subtree (groups' children too).
void IdsIn(pugi::xml_node n, std::set<uint32_t>& ids) {
  std::vector<pugi::xml_node> stack{n};
  while (!stack.empty()) {
    pugi::xml_node c = stack.back();
    stack.pop_back();
    if (LocalNameOf(c) == "cNvPr") ids.insert(U32(c, "id"));
    for (pugi::xml_node k = c.first_child(); k; k = k.next_sibling()) {
      if (k.type() == pugi::node_element) stack.push_back(k);
    }
  }
}

// Relationship ids a subtree names.
void RelsIn(const pkg::ReferenceRegistry& registry, pugi::xml_node n, std::set<std::string>& out) {
  for (const pkg::FoundRef& f : registry.Find(n, pkg::RefKind::RelationshipId)) out.insert(std::string(f.Value()));
}

// The shape tree's place for a new shape at z position `z` (null: at the top, before extLst).
pugi::xml_node InsertPoint(pugi::xml_node tree, int z) {
  int i = 0;
  pugi::xml_node ext;
  for (pugi::xml_node c = tree.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    if (LocalNameOf(c) == "extLst") {
      ext = c;
      continue;
    }
    if (!IsShapeElement(c)) continue;
    if (z >= 0 && i++ == z) return c;
  }
  return ext;
}

std::string Escape(std::string_view s) {
  std::string out;
  pkg::AppendEscapedAttribute(out, s);
  return out;
}

std::string XfrmXml(const draw::Rect& r, std::string_view prefix = "a") {
  std::string p(prefix);
  return "<" + p + ":xfrm><a:off x=\"" + std::to_string(ToEmu(r.left)) + "\" y=\"" + std::to_string(ToEmu(r.top)) +
         "\"/><a:ext cx=\"" + std::to_string(std::max(0LL, ToEmu(r.Width()))) + "\" cy=\"" +
         std::to_string(std::max(0LL, ToEmu(r.Height()))) + "\"/></" + p + ":xfrm>";
}

constexpr std::string_view kNs =
    " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
    " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
    " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"";

std::string ShapeName(std::string_view preset) {
  static const std::pair<std::string_view, std::string_view> kNames[] = {
      {"rect", "Rectangle"}, {"roundRect", "Rectangle: Rounded Corners"}, {"ellipse", "Oval"},
      {"triangle", "Isosceles Triangle"}, {"rtTriangle", "Right Triangle"}, {"diamond", "Diamond"},
      {"pentagon", "Arrow: Pentagon"}, {"hexagon", "Hexagon"}, {"octagon", "Octagon"}, {"star5", "Star: 5 Points"},
      {"rightArrow", "Arrow: Right"}, {"leftArrow", "Arrow: Left"}, {"upArrow", "Arrow: Up"}, {"downArrow", "Arrow: Down"},
      {"line", "Straight Connector"}, {"straightConnector1", "Straight Arrow Connector"}, {"heart", "Heart"},
      {"cloud", "Cloud"}, {"plus", "Plus Sign"}, {"parallelogram", "Parallelogram"}, {"trapezoid", "Trapezoid"},
      {"chevron", "Arrow: Chevron"}, {"wedgeRectCallout", "Speech Bubble: Rectangle"}, {"can", "Cylinder"},
      {"cube", "Cube"}, {"donut", "Circle: Hollow"}, {"smileyFace", "Smiley Face"}, {"frame", "Frame"},
  };
  for (const auto& [k, v] : kNames) {
    if (k == preset) return std::string(v);
  }
  return "Shape";
}

// Picture formats Office stores in a:blip, from their first bytes: (extension, content type).
std::pair<std::string, std::string> PictureType(const std::vector<uint8_t>& b) {
  auto at = [&](size_t i) { return i < b.size() ? b[i] : 0; };
  if (at(0) == 0x89 && at(1) == 'P' && at(2) == 'N' && at(3) == 'G') return {"png", "image/png"};
  if (at(0) == 0xFF && at(1) == 0xD8 && at(2) == 0xFF) return {"jpeg", "image/jpeg"};
  if (at(0) == 'G' && at(1) == 'I' && at(2) == 'F' && at(3) == '8') return {"gif", "image/gif"};
  if (at(0) == 'B' && at(1) == 'M') return {"bmp", "image/bmp"};
  if ((at(0) == 'I' && at(1) == 'I' && at(2) == 42 && at(3) == 0) || (at(0) == 'M' && at(1) == 'M' && at(2) == 0 && at(3) == 42))
    return {"tiff", "image/tiff"};
  if (at(0) == 1 && at(1) == 0 && at(2) == 0 && at(3) == 0 && at(40) == ' ' && at(41) == 'E' && at(42) == 'M' && at(43) == 'F')
    return {"emf", "image/x-emf"};
  if ((at(0) == 0xD7 && at(1) == 0xCD && at(2) == 0xC6 && at(3) == 0x9A) || ((at(0) == 1 || at(0) == 2) && at(1) == 0 && at(2) == 9 && at(3) == 0))
    return {"wmf", "image/x-wmf"};
  return {};
}

// The element whose a:xfrm places a shape: spPr's, a group's grpSpPr's, or a graphic frame's p:xfrm.
pugi::xml_node XfrmOf(pugi::xml_node shape, bool create) {
  std::string_view k = LocalNameOf(shape);
  if (k == "graphicFrame") {
    pugi::xml_node x = P(shape, "xfrm");
    if (!x && create) {
      std::string p = PrefixFor(shape, kP, "p");
      x = AddChildOrdered(shape, p + ":xfrm", {"graphic", "extLst"});
    }
    return x;
  }
  pugi::xml_node props = P(shape, k == "grpSp" ? "grpSpPr" : "spPr");
  if (!props) {
    if (!create) return {};
    std::string p = PrefixFor(shape, kP, "p");
    props = AddChildOrdered(shape, p + (k == "grpSp" ? ":grpSpPr" : ":spPr"), {"style", "txBody", "extLst", "sp", "pic", "grpSp", "graphicFrame", "cxnSp", "contentPart"});
  }
  pugi::xml_node x = A(props, "xfrm");
  if (!x && create) {
    std::string a = PrefixFor(props, kA, "a");
    x = props.prepend_child((a + ":xfrm").c_str());
  }
  return x;
}

struct Box {
  double x = 0, y = 0, w = 0, h = 0, rot = 0;  // EMU, degrees
  bool flipH = false, flipV = false;
  double chX = 0, chY = 0, chW = 0, chH = 0;
  bool hasXfrm = false;
};

Box ReadBox(pugi::xml_node xfrm) {
  Box b;
  if (!xfrm) return b;
  b.hasXfrm = true;
  pugi::xml_node off = A(xfrm, "off"), ext = A(xfrm, "ext");
  b.x = Number(off, "x", 0);
  b.y = Number(off, "y", 0);
  b.w = Number(ext, "cx", 0);
  b.h = Number(ext, "cy", 0);
  b.rot = Number(xfrm, "rot", 0) / 60000.0;
  b.flipH = Bool(xfrm, "flipH", false);
  b.flipV = Bool(xfrm, "flipV", false);
  pugi::xml_node chOff = A(xfrm, "chOff"), chExt = A(xfrm, "chExt");
  b.chX = Number(chOff, "x", b.x);
  b.chY = Number(chOff, "y", b.y);
  b.chW = Number(chExt, "cx", b.w);
  b.chH = Number(chExt, "cy", b.h);
  return b;
}

void SetAttr(pugi::xml_node n, const char* name, long long v) {
  (n.attribute(name) ? n.attribute(name) : n.append_attribute(name)).set_value(v);
}

void WriteBox(pugi::xml_node xfrm, const Box& b, bool group, bool rotate) {
  std::string a = PrefixFor(xfrm, kA, "a");
  auto child = [&](const char* local) {
    pugi::xml_node c = A(xfrm, local);
    if (!c) {
      // Schema order: off, ext, chOff, chExt.
      const char* order[] = {"off", "ext", "chOff", "chExt"};
      pugi::xml_node before;
      bool past = false;
      for (const char* o : order) {
        if (past && !before) before = A(xfrm, o);
        if (std::string_view(o) == local) past = true;
      }
      c = before ? xfrm.insert_child_before((a + ":" + local).c_str(), before) : xfrm.append_child((a + ":" + local).c_str());
    }
    return c;
  };
  pugi::xml_node off = child("off"), ext = child("ext");
  SetAttr(off, "x", std::llround(b.x));
  SetAttr(off, "y", std::llround(b.y));
  SetAttr(ext, "cx", std::max(0LL, std::llround(b.w)));
  SetAttr(ext, "cy", std::max(0LL, std::llround(b.h)));
  if (group) {
    pugi::xml_node chOff = child("chOff"), chExt = child("chExt");
    SetAttr(chOff, "x", std::llround(b.chX));
    SetAttr(chOff, "y", std::llround(b.chY));
    SetAttr(chExt, "cx", std::max(0LL, std::llround(b.chW)));
    SetAttr(chExt, "cy", std::max(0LL, std::llround(b.chH)));
  }
  double rot = rotate ? std::fmod(b.rot, 360.0) : 0;
  if (rot < 0) rot += 360;
  long long r = std::llround(rot * 60000);
  if (r % 21600000 == 0) {
    xfrm.remove_attribute("rot");
  } else {
    SetAttr(xfrm, "rot", r);
  }
  for (auto [name, on] : {std::pair{"flipH", b.flipH}, std::pair{"flipV", b.flipV}}) {
    if (on && rotate) {
      (xfrm.attribute(name) ? xfrm.attribute(name) : xfrm.append_attribute(name)).set_value("1");
    } else {
      xfrm.remove_attribute(name);
    }
  }
}

// A table frame resized: its columns and rows scaled to fill it, as PowerPoint does.
void ScaleTable(pugi::xml_node frame, double sx, double sy) {
  pugi::xml_node tbl = A(A(A(frame, "graphic"), "graphicData"), "tbl");
  if (!tbl) return;
  if (std::abs(sx - 1) > 1e-6) {
    for (pugi::xml_node c = A(tbl, "tblGrid").first_child(); c; c = c.next_sibling()) {
      if (LocalNameOf(c) == "gridCol") SetAttr(c, "w", std::llround(Number(c, "w", 0) * sx));
    }
  }
  if (std::abs(sy - 1) > 1e-6) {
    for (pugi::xml_node r = tbl.first_child(); r; r = r.next_sibling()) {
      if (LocalNameOf(r) == "tr") SetAttr(r, "h", std::llround(Number(r, "h", 0) * sy));
    }
  }
}

// The mc:Choice or mc:Fallback nearest around a shape, below the shape tree; null when none.
pugi::xml_node BranchOf(pugi::xml_node shape) {
  for (pugi::xml_node n = shape; n.parent() && n.parent().type() == pugi::node_element; n = n.parent()) {
    pugi::xml_node p = n.parent();
    if (Is(p, pkg::mce::kNamespace, "Choice") || Is(p, pkg::mce::kNamespace, "Fallback")) return p;
    if (LocalNameOf(p) == "spTree") break;
  }
  return {};
}

// The same shape (by cNvPr id) in the other branches of the mc:AlternateContent around it: what other
// apps show in its place (a math text box's Fallback is usually a picture of it).
std::vector<pugi::xml_node> TwinsOf(pugi::xml_node shape) {
  std::vector<pugi::xml_node> out;
  pugi::xml_node branch = BranchOf(shape), c = CNvPr(shape);
  if (!branch || !c) return out;
  uint32_t id = U32(c, "id");
  for (pugi::xml_node b = branch.parent().first_child(); b; b = b.next_sibling()) {
    if (b == branch || b.type() != pugi::node_element) continue;
    std::vector<pugi::xml_node> stack{b};
    while (!stack.empty()) {
      pugi::xml_node n = stack.back();
      stack.pop_back();
      for (pugi::xml_node k = n.first_child(); k; k = k.next_sibling()) {
        if (k.type() != pugi::node_element) continue;
        if (Is(k, pkg::mce::kNamespace, "AlternateContent") || Is(k, pkg::mce::kNamespace, "Choice") ||
            Is(k, pkg::mce::kNamespace, "Fallback")) {
          stack.push_back(k);
        } else if (IsShapeElement(k)) {
          if (pugi::xml_node kc = CNvPr(k); kc && U32(kc, "id") == id) {
            out.push_back(k);
          } else if (LocalNameOf(k) == "grpSp") {
            stack.push_back(k);
          }
        }
      }
    }
  }
  return out;
}

// What SyncTwins copies.
constexpr unsigned kTwinBox = 1, kTwinGeometry = 2, kTwinFill = 4, kTwinLine = 8, kTwinEffects = 16,
                   kTwinName = 32, kTwinAltText = 64, kTwinHidden = 128;

// `to`'s spPr children of schema group `group` replaced by copies of `from`'s.
void CopySpPrGroup(pugi::xml_node from, pugi::xml_node to, int group) {
  for (pugi::xml_node c = to.first_child(); c;) {
    pugi::xml_node next = c.next_sibling();
    if (c.type() == pugi::node_element && SpPrGroupOf(LocalNameOf(c)) == group) to.remove_child(c);
    c = next;
  }
  pugi::xml_node before;
  for (pugi::xml_node c = to.first_child(); c && !before; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    int g = SpPrGroupOf(LocalNameOf(c));
    if (g > group || g < 0) before = c;
  }
  for (pugi::xml_node c = from.first_child(); c; c = c.next_sibling()) {
    if (c.type() == pugi::node_element && SpPrGroupOf(LocalNameOf(c)) == group) InsertCopy(to, c, before);
  }
}

bool PlainShape(std::string_view k) { return k == "sp" || k == "pic" || k == "cxnSp"; }

// Copies `what` of a shape to its twins (TwinsOf), so the branches of its mc:AlternateContent stay in
// step: the transform for any shape; geometry, fill, outline and effects between plain shapes (sp,
// pic, cxnSp); name, alt text, hidden. The rest of a twin stays as it was.
void SyncTwins(pugi::xml_node shape, unsigned what) {
  if (!what) return;
  std::string_view k = LocalNameOf(shape);
  for (pugi::xml_node twin : TwinsOf(shape)) {
    std::string_view tk = LocalNameOf(twin);
    if (what & kTwinBox) {
      if (pugi::xml_node from = XfrmOf(shape, false)) {
        Box s = ReadBox(from);
        pugi::xml_node to = XfrmOf(twin, true);
        Box t = ReadBox(to);
        double oldW = t.w, oldH = t.h;
        t.x = s.x;
        t.y = s.y;
        t.w = s.w;
        t.h = s.h;
        t.rot = s.rot;
        t.flipH = s.flipH;
        t.flipV = s.flipV;
        WriteBox(to, t, tk == "grpSp", tk != "graphicFrame");
        if (tk == "graphicFrame" && oldW > 0 && oldH > 0) ScaleTable(twin, t.w / oldW, t.h / oldH);
      }
    }
    pugi::xml_node from = P(shape, "spPr"), to = P(twin, "spPr");
    if (PlainShape(k) && PlainShape(tk) && from && to) {
      for (auto [bit, group] : {std::pair{kTwinGeometry, 1}, {kTwinFill, 2}, {kTwinLine, 3}, {kTwinEffects, 4}}) {
        if (what & bit) CopySpPrGroup(from, to, group);
      }
    }
    pugi::xml_node sc = CNvPr(shape), tc = CNvPr(twin);
    if (sc && tc) {
      for (auto [bit, name] : {std::pair{kTwinName, "name"}, {kTwinAltText, "descr"}, {kTwinHidden, "hidden"}}) {
        if (!(what & bit)) continue;
        if (pugi::xml_attribute a = sc.attribute(name)) {
          (tc.attribute(name) ? tc.attribute(name) : tc.append_attribute(name)).set_value(a.value());
        } else {
          tc.remove_attribute(name);
        }
      }
    }
  }
}

// What SyncTwins would copy that differs between two versions of a shape.
unsigned TwinDifferences(pugi::xml_node a, pugi::xml_node b) {
  auto text = [](pugi::xml_node n) {
    std::ostringstream out;
    if (n) n.print(out, "", pugi::format_raw);
    return out.str();
  };
  auto group = [&](pugi::xml_node s, int g) {
    std::string out;
    for (pugi::xml_node c = P(s, "spPr").first_child(); c; c = c.next_sibling()) {
      if (c.type() == pugi::node_element && SpPrGroupOf(LocalNameOf(c)) == g) out += text(c);
    }
    return out;
  };
  unsigned what = 0;
  if (text(XfrmOf(a, false)) != text(XfrmOf(b, false))) what |= kTwinBox;
  for (auto [bit, g] : {std::pair{kTwinGeometry, 1}, {kTwinFill, 2}, {kTwinLine, 3}, {kTwinEffects, 4}}) {
    if (group(a, g) != group(b, g)) what |= bit;
  }
  pugi::xml_node ca = CNvPr(a), cb = CNvPr(b);
  for (auto [bit, name] : {std::pair{kTwinName, "name"}, {kTwinAltText, "descr"}, {kTwinHidden, "hidden"}}) {
    pugi::xml_attribute x = ca.attribute(name), y = cb.attribute(name);
    if (static_cast<bool>(x) != static_cast<bool>(y) || std::string_view(x.value()) != y.value()) what |= bit;
  }
  return what;
}

// The id a shape tree child goes by: its cNvPr id; for mc:AlternateContent, its first branch's shape's.
uint32_t UnitId(pugi::xml_node unit) {
  if (Is(unit, pkg::mce::kNamespace, "AlternateContent")) {
    for (pugi::xml_node b = unit.first_child(); b; b = b.next_sibling()) {
      for (pugi::xml_node k = b.first_child(); k; k = k.next_sibling()) {
        if (pugi::xml_node c = IsShapeElement(k) ? CNvPr(k) : pugi::xml_node()) return U32(c, "id");
      }
    }
    return 0;
  }
  pugi::xml_node c = CNvPr(unit);
  return c ? U32(c, "id") : 0;
}

// A group's twins (TwinsOf) take its z order: the shapes both have, in the places they had.
void SyncOrder(pugi::xml_node group) {
  if (LocalNameOf(group) != "grpSp") return;
  std::vector<uint32_t> order;
  for (pugi::xml_node c = group.first_child(); c; c = c.next_sibling()) {
    if (IsShapeElement(c)) order.push_back(UnitId(c));
  }
  auto rank = [&](pugi::xml_node n) {
    auto it = std::find(order.begin(), order.end(), UnitId(n));
    return it == order.end() ? -1 : static_cast<int>(it - order.begin());
  };
  for (pugi::xml_node twin : TwinsOf(group)) {
    if (LocalNameOf(twin) != "grpSp") continue;
    std::vector<pugi::xml_node> slots;
    for (pugi::xml_node c = twin.first_child(); c; c = c.next_sibling()) {
      if (IsShapeElement(c) && rank(c) >= 0) slots.push_back(c);
    }
    std::vector<pugi::xml_node> sorted = slots;
    std::stable_sort(sorted.begin(), sorted.end(), [&](pugi::xml_node x, pugi::xml_node y) { return rank(x) < rank(y); });
    if (sorted == slots) continue;
    std::vector<pugi::xml_node> marks;
    for (pugi::xml_node s : slots) marks.push_back(twin.insert_child_before(pugi::node_comment, s));
    for (size_t i = 0; i < sorted.size(); ++i) twin.insert_move_before(sorted[i], marks[i]);
    for (pugi::xml_node mk : marks) twin.remove_child(mk);
  }
}

// Animations, builds and connections that name shapes `ids` removed (the shapes are gone).
void RemoveShapeRefs(const pkg::ReferenceRegistry& registry, pugi::xml_node root, const std::set<uint32_t>& ids) {
  std::vector<pugi::xml_node> gone;
  for (const pkg::FoundRef& f : registry.Find(root, pkg::RefKind::DrawingId)) {
    if (f.site->role & pkg::kDefines) continue;
    uint32_t v = static_cast<uint32_t>(Number(f.Value(), 0));
    if (!ids.count(v)) continue;
    std::string_view k = LocalNameOf(f.element);
    if (k == "spTgt") {
      // The effect (or the interactive sequence it triggers) goes.
      pugi::xml_node n = f.element;
      pugi::xml_node unit;
      bool inCondition = false;
      for (pugi::xml_node a = n.parent(); a && !unit; a = a.parent()) {
        std::string_view ak = LocalNameOf(a);
        if (ak == "cond") inCondition = true;
        if (ak == "seq") unit = a;
        if (ak == "par" && !inCondition) {
          pugi::xml_node cTn = P(a, "cTn");
          if (cTn.attribute("presetClass") || cTn.attribute("nodeType")) unit = a;
        }
        if (ak == "timing") break;
      }
      if (unit) gone.push_back(unit);
    } else {
      gone.push_back(f.element);  // bldP, bldGraphic..., a:stCxn, a:endCxn, inkTgt
    }
  }
  std::sort(gone.begin(), gone.end(), [](pugi::xml_node a, pugi::xml_node b) { return a.internal_object() < b.internal_object(); });
  gone.erase(std::unique(gone.begin(), gone.end()), gone.end());
  for (pugi::xml_node n : gone) {
    // Skip one inside another already gone.
    bool inside = false;
    for (pugi::xml_node a = n.parent(); a && !inside; a = a.parent()) {
      inside = std::find(gone.begin(), gone.end(), a) != gone.end();
    }
    if (!inside && n.parent()) n.parent().remove_child(n);
  }
  // Time node lists left empty go with their node, up to the timing itself; an empty build list goes.
  pugi::xml_node timing = P(root, "timing");
  for (bool again = true; again && timing;) {
    again = false;
    std::vector<pugi::xml_node> stack{timing};
    while (!stack.empty() && !again) {
      pugi::xml_node n = stack.back();
      stack.pop_back();
      for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling()) {
        if (c.type() != pugi::node_element) continue;
        std::string_view k = LocalNameOf(c);
        bool empty = true;
        for (pugi::xml_node d = c.first_child(); d && empty; d = d.next_sibling()) empty = d.type() != pugi::node_element;
        if (empty && (k == "childTnLst" || k == "subTnLst" || k == "tnLst" || k == "bldLst")) {
          // The time node holding the list (cTn's parent: p:par, p:seq...) goes; an empty tnLst or
          // build list takes the timing with it when nothing is left.
          pugi::xml_node owner = c.parent();
          if (k == "bldLst" || k == "tnLst") {
            owner.remove_child(c);
          } else if (LocalNameOf(owner) == "cTn") {
            pugi::xml_node node = owner.parent();
            node.parent().remove_child(node);
          } else {
            owner.remove_child(c);
          }
          again = true;
          break;
        }
        stack.push_back(c);
      }
    }
  }
  if (timing && !P(timing, "tnLst")) root.remove_child(timing);
}

// Drops relationships of `part` that `candidates` names and nothing in the part names any more.
void DropUnusedRels(Editor::Impl& m, std::string_view part, pugi::xml_node root, const std::set<std::string>& candidates) {
  if (candidates.empty()) return;
  std::set<std::string> used;
  RelsIn(m.registry, root, used);
  std::set<std::string> drop;
  for (const std::string& id : candidates) {
    if (!id.empty() && !used.count(id)) drop.insert(id);
  }
  if (drop.empty() || !m.Rels(part)) return;
  pkg::RelationshipSet& rels = m.EditRels(part);
  std::erase_if(rels.items, [&](const pkg::Relationship& r) {
    // Only what shapes name: never the layout, notes or comments a slide names implicitly.
    return drop.count(r.id) > 0 && !r.type.ends_with("/slideLayout") && !r.type.ends_with("/notesSlide");
  });
}

}  // namespace

// ---- Shared (ShapeXml.h)

std::string ColorFillXml(const ColorSpec& c) {
  switch (c.kind) {
    case ColorSpec::Kind::None:
      return "<a:noFill xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"/>";
    case ColorSpec::Kind::Rgb: {
      char hex[8];
      snprintf(hex, sizeof hex, "%06X", c.rgb & 0xFFFFFF);
      return std::string("<a:solidFill xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><a:srgbClr val=\"") +
             hex + "\"/></a:solidFill>";
    }
    case ColorSpec::Kind::Scheme: {
      std::string mods;
      if (c.lumMod) mods += "<a:lumMod val=\"" + std::to_string(c.lumMod) + "\"/>";
      if (c.lumOff) mods += "<a:lumOff val=\"" + std::to_string(c.lumOff) + "\"/>";
      return "<a:solidFill xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><a:schemeClr val=\"" +
             Escape(c.scheme) + "\"" + (mods.empty() ? "/>" : ">" + mods + "</a:schemeClr>") + "</a:solidFill>";
    }
    case ColorSpec::Kind::Xml:
      return c.xml;
    case ColorSpec::Kind::Inherit:
      break;
  }
  return {};
}

bool ReplaceGroup(pugi::xml_node parent, std::string_view xml, const std::function<int(std::string_view)>& groupOf,
                  int group) {
  // Out with the group's elements.
  for (pugi::xml_node c = parent.first_child(); c;) {
    pugi::xml_node next = c.next_sibling();
    if (c.type() == pugi::node_element && groupOf(LocalNameOf(c)) == group) parent.remove_child(c);
    c = next;
  }
  if (xml.empty()) return true;
  pugi::xml_document doc;
  if (!ParseFragment(xml, doc)) return false;
  if (groupOf(LocalNameOf(doc.document_element())) != group) return false;
  pugi::xml_node before;
  for (pugi::xml_node c = parent.first_child(); c && !before; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    int g = groupOf(LocalNameOf(c));
    if (g > group || g < 0) before = c;
  }
  InsertCopy(parent, doc.document_element(), before);
  return true;
}

int SpPrGroupOf(std::string_view k) {
  if (k == "xfrm") return 0;
  if (k == "custGeom" || k == "prstGeom") return 1;
  if (k == "noFill" || k == "solidFill" || k == "gradFill" || k == "blipFill" || k == "pattFill" || k == "grpFill") return 2;
  if (k == "ln") return 3;
  if (k == "effectLst" || k == "effectDag") return 4;
  if (k == "scene3d") return 5;
  if (k == "sp3d") return 6;
  return -1;  // extLst and anything else: after
}

std::string LinkAction(const LinkTarget& link) {
  switch (link.kind) {
    case LinkTarget::Kind::Slide: return "ppaction://hlinksldjump";
    case LinkTarget::Kind::FirstSlide: return "ppaction://hlinkshowjump?jump=firstslide";
    case LinkTarget::Kind::LastSlide: return "ppaction://hlinkshowjump?jump=lastslide";
    case LinkTarget::Kind::NextSlide: return "ppaction://hlinkshowjump?jump=nextslide";
    case LinkTarget::Kind::PreviousSlide: return "ppaction://hlinkshowjump?jump=previousslide";
    case LinkTarget::Kind::EndShow: return "ppaction://hlinkshowjump?jump=endshow";
    case LinkTarget::Kind::Url: break;
  }
  return {};
}

bool WriteLink(Editor::Impl& m, std::string_view part, pugi::xml_node link, const LinkTarget& target) {
  std::string rel;
  if (target.kind == LinkTarget::Kind::Url) {
    if (target.url.empty()) return false;
    rel = m.AddRel(part, kRelHyperlink, target.url, true);
  } else if (target.kind == LinkTarget::Kind::Slide) {
    std::string slide = m.SlidePart(target.slide);
    if (slide.empty()) return false;
    rel = m.AddRel(part, kRelSlide, slide);
  }
  std::string r = PrefixFor(link, kRelNs, "r");
  link.append_attribute((r + ":id").c_str()) = rel.c_str();
  if (std::string action = LinkAction(target); !action.empty()) link.append_attribute("action") = action.c_str();
  if (!target.tooltip.empty()) link.append_attribute("tooltip") = target.tooltip.c_str();
  return true;
}

// ---- Editor: shapes

// Puts a parsed shape on a slide: ids renewed against the slide's, at z position `z`.
Result PutShape(Editor::Impl& m, std::string_view part, pugi::xml_node shape, int z, uint32_t* id, uint32_t parentGroup) {
  pugi::xml_node root = m.Edit(part);
  pugi::xml_node tree = P(P(root, "cSld"), "spTree");
  if (!tree) return Result::Fail(u"The slide can't be read.");
  if (parentGroup) {
    tree = Editor::Impl::FindShape(root, parentGroup);
    if (!tree || LocalNameOf(tree) != "grpSp") return Result::Fail(u"That group isn't on the slide.");
    tree = Editor::Impl::Unwrap(tree);
  }
  // Ids the shape defines that the slide uses: new ones above the slide's highest.
  std::set<uint32_t> own;
  IdsIn(shape, own);
  uint32_t next = Editor::Impl::NextShapeId(root);
  std::set<uint32_t> taken;
  IdsIn(root, taken);
  std::map<uint32_t, uint32_t> renamed;
  for (uint32_t v : own) {
    if (v == 0 || taken.count(v)) renamed[v] = next++;
  }
  for (auto [from, to] : renamed) {
    m.registry.RenameInTree(pkg::RefKind::DrawingId, std::to_string(from), std::to_string(to), shape);
  }
  pugi::xml_node added = InsertCopy(tree, shape, InsertPoint(tree, z));
  uint32_t newId = U32(CNvPr(added), "id");
  if (id) *id = newId;
  m.Touched(part, newId);
  return {};
}

Result Editor::MoveShape(size_t slide, uint32_t id, uint32_t parentGroup, int index) {
  Call call(*this, u"Arrange");
  Impl& m = *impl_;
  std::string part = m.SlidePart(slide);
  if (part.empty()) return call.End(Result::Fail(u"That slide isn't in the presentation."));
  pugi::xml_node root = m.Edit(part);
  pugi::xml_node shape = Impl::FindShape(root, id);
  if (!shape) return call.End(Result::Fail(u"That shape isn't on the slide."));
  pugi::xml_node parent = P(P(root, "cSld"), "spTree");
  if (parentGroup) {
    parent = Impl::FindShape(root, parentGroup);
    if (!parent || LocalNameOf(parent) != "grpSp") return call.End(Result::Fail(u"That group isn't on the slide."));
    parent = Impl::Unwrap(parent);
    shape = Impl::FindShape(root, id);
  }
  pugi::xml_node unit = UnitOf(shape);
  for (pugi::xml_node a = parent; a; a = a.parent()) {
    if (a == unit) return call.End(Result::Fail(u"A group can't go into itself."));
  }
  // Into or out of a group inside mc:AlternateContent, the other branches would lack the shape or
  // have it twice.
  if (BranchOf(unit) != (LocalNameOf(parent) == "grpSp" ? BranchOf(parent) : pugi::xml_node())) {
    return call.End(Result::Fail(u"Shapes can't be moved into or out of that group."));
  }
  pugi::xml_node before = InsertPoint(parent, index);
  if (before != unit) before ? parent.insert_move_before(unit, before) : parent.append_move(unit);
  SyncOrder(parent);
  m.Touched(part, id);
  return call.End({});
}

Result Editor::InsertShape(size_t slide, std::string_view xml, uint32_t* id, int z, uint32_t parentGroup) {
  Call call(*this, u"Insert Shape");
  Impl& m = *impl_;
  std::string part = m.SlidePart(slide);
  if (part.empty()) return call.End(Result::Fail(u"That slide isn't in the presentation."));
  pugi::xml_document doc;
  if (!ParseFragment(xml, doc) || !IsShapeElement(doc.document_element()) || !CNvPr(doc.document_element())) {
    return call.End(Result::Fail(u"That isn't a shape Featherweight can insert."));
  }
  // The relationships it names must be the slide's.
  std::set<std::string> rels;
  RelsIn(m.registry, doc.document_element(), rels);
  const pkg::RelationshipSet* have = m.Rels(part);
  for (const std::string& r : rels) {
    if (!r.empty() && (!have || !have->Find(r))) return call.End(Result::Fail(u"The shape names a part the slide doesn't have."));
  }
  return call.End(PutShape(m, part, doc.document_element(), z, id, parentGroup));
}

Result Editor::InsertTextBox(size_t slide, const draw::Rect& r, std::wstring_view text, uint32_t* id) {
  Call call(*this, u"Insert Text Box");
  Impl& m = *impl_;
  std::string part = m.SlidePart(slide);
  if (part.empty()) return call.End(Result::Fail(u"That slide isn't in the presentation."));
  uint32_t next = Impl::NextShapeId(m.Root(part));
  std::string xml = "<p:sp" + std::string(kNs) + "><p:nvSpPr><p:cNvPr id=\"" + std::to_string(next) + "\" name=\"TextBox " +
                    std::to_string(next - 1) + "\"/><p:cNvSpPr txBox=\"1\"/><p:nvPr/></p:nvSpPr><p:spPr>" + XfrmXml(r) +
                    "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:noFill/></p:spPr><p:txBody>"
                    "<a:bodyPr wrap=\"square\" rtlCol=\"0\"><a:spAutoFit/></a:bodyPr><a:lstStyle/><a:p>"
                    "<a:endParaRPr lang=\"en-US\" dirty=\"0\"/></a:p></p:txBody></p:sp>";
  pugi::xml_document doc;
  ParseFragment(xml, doc);
  if (!text.empty()) {
    pugi::xml_node body = P(doc.document_element(), "txBody");
    pugi::xml_node rPr = A(A(body, "p"), "endParaRPr");
    (void)rPr;
    SetPlainText(body, text);
  }
  return call.End(PutShape(m, part, doc.document_element(), -1, id));
}

Result Editor::InsertAutoShape(size_t slide, std::string_view preset, const draw::Rect& r, uint32_t* id) {
  Call call(*this, u"Insert Shape");
  Impl& m = *impl_;
  std::string part = m.SlidePart(slide);
  if (part.empty()) return call.End(Result::Fail(u"That slide isn't in the presentation."));
  if (preset.empty() || preset.find_first_of("<>\"&'") != std::string_view::npos) {
    return call.End(Result::Fail(u"That isn't a shape Featherweight knows."));
  }
  uint32_t next = Impl::NextShapeId(m.Root(part));
  std::string name = ShapeName(preset) + " " + std::to_string(next - 1);
  bool line = preset == "line" || preset.find("Connector") != std::string_view::npos;
  std::string xml;
  if (line) {
    xml = "<p:cxnSp" + std::string(kNs) + "><p:nvCxnSpPr><p:cNvPr id=\"" + std::to_string(next) + "\" name=\"" + Escape(name) +
          "\"/><p:cNvCxnSpPr/><p:nvPr/></p:nvCxnSpPr><p:spPr>" + XfrmXml(r) + "<a:prstGeom prst=\"" + std::string(preset) +
          "\"><a:avLst/></a:prstGeom></p:spPr><p:style><a:lnRef idx=\"1\"><a:schemeClr val=\"accent1\"/></a:lnRef>"
          "<a:fillRef idx=\"0\"><a:schemeClr val=\"accent1\"/></a:fillRef><a:effectRef idx=\"0\"><a:schemeClr val=\"accent1\"/>"
          "</a:effectRef><a:fontRef idx=\"minor\"><a:schemeClr val=\"tx1\"/></a:fontRef></p:style></p:cxnSp>";
  } else {
    xml = "<p:sp" + std::string(kNs) + "><p:nvSpPr><p:cNvPr id=\"" + std::to_string(next) + "\" name=\"" + Escape(name) +
          "\"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr>" + XfrmXml(r) + "<a:prstGeom prst=\"" + std::string(preset) +
          "\"><a:avLst/></a:prstGeom></p:spPr><p:style><a:lnRef idx=\"2\"><a:schemeClr val=\"accent1\"><a:shade val=\"50000\"/>"
          "</a:schemeClr></a:lnRef><a:fillRef idx=\"1\"><a:schemeClr val=\"accent1\"/></a:fillRef><a:effectRef idx=\"0\">"
          "<a:schemeClr val=\"accent1\"/></a:effectRef><a:fontRef idx=\"minor\"><a:schemeClr val=\"lt1\"/></a:fontRef>"
          "</p:style><p:txBody><a:bodyPr rtlCol=\"0\" anchor=\"ctr\"/><a:lstStyle/><a:p><a:pPr algn=\"ctr\"/>"
          "<a:endParaRPr lang=\"en-US\" dirty=\"0\"/></a:p></p:txBody></p:sp>";
  }
  pugi::xml_document doc;
  if (!ParseFragment(xml, doc)) return call.End(Result::Fail(u"That isn't a shape Featherweight knows."));
  return call.End(PutShape(m, part, doc.document_element(), -1, id));
}

Result Editor::InsertPicture(size_t slide, std::vector<uint8_t> bytes, const draw::Rect& r, std::wstring_view altText,
                             uint32_t* id) {
  Call call(*this, u"Insert Picture");
  Impl& m = *impl_;
  std::string part = m.SlidePart(slide);
  if (part.empty()) return call.End(Result::Fail(u"That slide isn't in the presentation."));
  auto [ext, type] = PictureType(bytes);
  if (ext.empty()) return call.End(Result::Fail(u"That picture's format can't be put in a presentation."));
  // Stored once: a media part with the same bytes is used again.
  std::string media;
  for (const std::string& name : m.store.PartNames()) {
    if (!name.starts_with("/ppt/media/")) continue;
    std::vector<uint8_t> have;
    if (m.Read(name, have) && have == bytes) {
      media = name;
      break;
    }
  }
  for (const auto& [key, state] : m.work.parts) {
    if (media.empty() && state.bytes && *state.bytes == bytes) media = state.name;
  }
  if (media.empty()) {
    media = m.NewName("/ppt/media/image", "." + ext);
    m.PutBytes(media, std::move(bytes), type);
  }
  std::string rel = m.AddRel(part, kRelImage, media);
  uint32_t next = Impl::NextShapeId(m.Root(part));
  std::string xml = "<p:pic" + std::string(kNs) + "><p:nvPicPr><p:cNvPr id=\"" + std::to_string(next) + "\" name=\"Picture " +
                    std::to_string(next - 1) + "\" descr=\"" + Escape(Utf8(altText)) +
                    "\"/><p:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></p:cNvPicPr><p:nvPr/></p:nvPicPr><p:blipFill>"
                    "<a:blip r:embed=\"" + rel + "\"/><a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr>" + XfrmXml(r) +
                    "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>";
  pugi::xml_document doc;
  ParseFragment(xml, doc);
  if (altText.empty()) CNvPr(doc.document_element()).remove_attribute("descr");
  return call.End(PutShape(m, part, doc.document_element(), -1, id));
}

Result Editor::InsertTable(size_t slide, int rows, int columns, const draw::Rect& r, uint32_t* id) {
  Call call(*this, u"Insert Table");
  Impl& m = *impl_;
  std::string part = m.SlidePart(slide);
  if (part.empty()) return call.End(Result::Fail(u"That slide isn't in the presentation."));
  if (rows < 1 || columns < 1 || rows > 75 || columns > 75) {
    return call.End(Result::Fail(u"A table has 1 to 75 rows and 1 to 75 columns."));
  }
  long long width = std::max(ToEmu(r.Width()), static_cast<long long>(columns) * 12700);
  long long rowH = std::max(370840LL, ToEmu(r.Height()) / rows);
  uint32_t next = Impl::NextShapeId(m.Root(part));
  std::string xml = "<p:graphicFrame" + std::string(kNs) + "><p:nvGraphicFramePr><p:cNvPr id=\"" + std::to_string(next) +
                    "\" name=\"Table " + std::to_string(next - 1) +
                    "\"/><p:cNvGraphicFramePr><a:graphicFrameLocks noGrp=\"1\"/></p:cNvGraphicFramePr><p:nvPr/>"
                    "</p:nvGraphicFramePr>" +
                    XfrmXml({r.left, r.top, r.left + static_cast<float>(width / kEmu), r.top + static_cast<float>(rowH * rows / kEmu)}, "p") +
                    "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/table\"><a:tbl>"
                    "<a:tblPr firstRow=\"1\" bandRow=\"1\"><a:tableStyleId>{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}"
                    "</a:tableStyleId></a:tblPr><a:tblGrid>";
  long long used = 0;
  for (int c = 0; c < columns; ++c) {
    long long w = c + 1 == columns ? width - used : width / columns;
    used += w;
    xml += "<a:gridCol w=\"" + std::to_string(w) + "\"/>";
  }
  xml += "</a:tblGrid>";
  for (int row = 0; row < rows; ++row) {
    xml += "<a:tr h=\"" + std::to_string(rowH) + "\">";
    for (int c = 0; c < columns; ++c) {
      xml += "<a:tc><a:txBody><a:bodyPr/><a:lstStyle/><a:p><a:endParaRPr lang=\"en-US\" dirty=\"0\"/></a:p></a:txBody><a:tcPr/></a:tc>";
    }
    xml += "</a:tr>";
  }
  xml += "</a:tbl></a:graphicData></a:graphic></p:graphicFrame>";
  pugi::xml_document doc;
  if (!ParseFragment(xml, doc)) return call.End(Result::Fail(u"The table can't be made."));
  return call.End(PutShape(m, part, doc.document_element(), -1, id));
}

Result Editor::DeleteShapes(size_t slide, const std::vector<uint32_t>& ids) {
  Call call(*this, u"Delete");
  Impl& m = *impl_;
  std::string part = m.SlidePart(slide);
  if (part.empty()) return call.End(Result::Fail(u"That slide isn't in the presentation."));
  if (ids.empty()) return call.End({});
  pugi::xml_node root = m.Root(part);
  std::vector<pugi::xml_node> units;
  std::set<uint32_t> gone;
  for (uint32_t id : ids) {
    pugi::xml_node shape = Impl::FindShape(root, id);
    if (!shape) return call.End(Result::Fail(u"That shape isn't on the slide."));
    IdsIn(UnitOf(shape), gone);
    if (UnitOf(shape) == shape) {
      for (pugi::xml_node t : TwinsOf(shape)) IdsIn(t, gone);
    }
  }
  {
    pkg::EditImpact impact;
    impact.kind = pkg::RefKind::DrawingId;
    for (uint32_t g : gone) impact.values.push_back(std::to_string(g));
    if (!m.registry.WouldStrand(root, {impact}).empty()) {
      return call.End(Result::Fail(
          u"The shape can't be deleted, because the slide has content from a newer program that may refer to it."));
    }
  }
  root = m.Edit(part);
  std::set<std::string> rels;
  for (uint32_t id : ids) {
    pugi::xml_node shape = Impl::FindShape(root, id);
    if (!shape) continue;  // inside one already deleted
    pugi::xml_node unit = UnitOf(shape);
    // In a group inside mc:AlternateContent: its twins in the other branches go too (a shape directly
    // in a branch goes with the whole block).
    std::vector<pugi::xml_node> twins;
    if (unit == shape) twins = TwinsOf(shape);
    RelsIn(m.registry, unit, rels);
    unit.parent().remove_child(unit);
    for (pugi::xml_node t : twins) {
      RelsIn(m.registry, t, rels);
      t.parent().remove_child(t);
    }
    m.Touched(part, id);
  }
  RemoveShapeRefs(m.registry, root, gone);
  DropUnusedRels(m, part, root, rels);
  return call.End({});
}

Result Editor::SetTransform(size_t slide, uint32_t id, const Transform& t) { return SetTransforms(slide, {{id, t}}); }

Result Editor::SetTransforms(size_t slide, const std::vector<std::pair<uint32_t, Transform>>& shapes) {
  Call call(*this, u"Move");
  Impl& m = *impl_;
  std::string part = m.SlidePart(slide);
  if (part.empty()) return call.End(Result::Fail(u"That slide isn't in the presentation."));
  pugi::xml_node root = m.Edit(part);
  for (const auto& [id, t] : shapes) {
    pugi::xml_node shape = Impl::FindShape(root, id);
    if (!shape) return call.End(Result::Fail(u"That shape isn't on the slide."));
    shape = Impl::Unwrap(shape);
    std::string_view k = LocalNameOf(shape);
    pugi::xml_node xfrm = XfrmOf(shape, true);
    Box b = ReadBox(xfrm);
    double oldW = b.w, oldH = b.h;
    b.x = t.bounds.left * kEmu;
    b.y = t.bounds.top * kEmu;
    b.w = t.bounds.Width() * kEmu;
    b.h = t.bounds.Height() * kEmu;
    b.rot = t.rotation;
    b.flipH = t.flipH;
    b.flipV = t.flipV;
    bool group = k == "grpSp";
    if (group && !A(xfrm, "chExt")) {
      b.chX = b.x;
      b.chY = b.y;
      b.chW = oldW > 0 ? oldW : b.w;
      b.chH = oldH > 0 ? oldH : b.h;
    }
    WriteBox(xfrm, b, group, k != "graphicFrame");
    if (k == "graphicFrame" && oldW > 0 && oldH > 0) ScaleTable(shape, b.w / oldW, b.h / oldH);
    SyncTwins(shape, kTwinBox);
    m.Touched(part, id);
  }
  return call.End({});
}

Result Editor::Arrange(size_t slide, const std::vector<uint32_t>& ids, ZOrder order) {
  static const char16_t* kNames[] = {u"Bring to Front", u"Bring Forward", u"Send Backward", u"Send to Back"};
  Call call(*this, kNames[static_cast<int>(order)]);
  Impl& m = *impl_;
  std::string part = m.SlidePart(slide);
  if (part.empty()) return call.End(Result::Fail(u"That slide isn't in the presentation."));
  pugi::xml_node root = m.Edit(part);
  std::vector<pugi::xml_node> units;
  pugi::xml_node parent;
  for (uint32_t id : ids) {
    pugi::xml_node shape = Impl::FindShape(root, id);
    if (!shape) return call.End(Result::Fail(u"That shape isn't on the slide."));
    pugi::xml_node unit = UnitOf(shape);
    if (parent && unit.parent() != parent) return call.End(Result::Fail(u"Shapes in different groups can't be arranged together."));
    parent = unit.parent();
    if (std::find(units.begin(), units.end(), unit) == units.end()) units.push_back(unit);
    m.Touched(part, id);
  }
  if (units.empty()) return call.End({});
  // The parent's shapes bottom to top, with what's selected.
  std::vector<pugi::xml_node> all;
  for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling()) {
    if (IsShapeElement(c)) all.push_back(c);
  }
  auto selected = [&](pugi::xml_node n) { return std::find(units.begin(), units.end(), n) != units.end(); };
  std::vector<pugi::xml_node> result;
  switch (order) {
    case ZOrder::BringToFront:
      for (pugi::xml_node n : all) if (!selected(n)) result.push_back(n);
      for (pugi::xml_node n : all) if (selected(n)) result.push_back(n);
      break;
    case ZOrder::SendToBack:
      for (pugi::xml_node n : all) if (selected(n)) result.push_back(n);
      for (pugi::xml_node n : all) if (!selected(n)) result.push_back(n);
      break;
    case ZOrder::BringForward:
      result = all;
      for (size_t i = result.size(); i-- > 0;) {
        if (selected(result[i]) && i + 1 < result.size() && !selected(result[i + 1])) std::swap(result[i], result[i + 1]);
      }
      break;
    case ZOrder::SendBackward:
      result = all;
      for (size_t i = 0; i < result.size(); ++i) {
        if (selected(result[i]) && i > 0 && !selected(result[i - 1])) std::swap(result[i], result[i - 1]);
      }
      break;
  }
  // Put them back in that order where the shapes were (after the group properties, before extLst).
  pugi::xml_node anchor;
  for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling()) {
    if (c.type() == pugi::node_element && LocalNameOf(c) == "extLst") anchor = c;
  }
  for (pugi::xml_node n : result) anchor ? parent.insert_move_before(n, anchor) : parent.append_move(n);
  SyncOrder(parent);
  return call.End({});
}

Result Editor::Group(size_t slide, const std::vector<uint32_t>& ids, uint32_t* groupId) {
  Call call(*this, u"Group");
  Impl& m = *impl_;
  std::string part = m.SlidePart(slide);
  if (part.empty()) return call.End(Result::Fail(u"That slide isn't in the presentation."));
  if (ids.size() < 2) return call.End(Result::Fail(u"Select two or more shapes to group them."));
  pugi::xml_node root = m.Edit(part);
  // What moves into the group: the shapes, or the mc:AlternateContent blocks holding them (every
  // branch goes along).
  std::vector<pugi::xml_node> members, shapes;
  pugi::xml_node parent;
  for (uint32_t id : ids) {
    pugi::xml_node shape = Impl::FindShape(root, id);
    if (!shape) return call.End(Result::Fail(u"That shape isn't on the slide."));
    if (PlaceholderOf(shape)) return call.End(Result::Fail(u"Placeholders can't be grouped."));
    shape = Impl::Unwrap(shape);
    if (!XfrmOf(shape, false)) return call.End(Result::Fail(u"That shape can't be grouped."));
    pugi::xml_node unit = UnitOf(shape);
    if (parent && unit.parent() != parent) return call.End(Result::Fail(u"Shapes in different groups can't be grouped together."));
    parent = unit.parent();
    if (std::find(members.begin(), members.end(), unit) == members.end()) members.push_back(unit);
    if (std::find(shapes.begin(), shapes.end(), shape) == shapes.end()) shapes.push_back(shape);
  }
  // Bottom to top as they are now.
  std::vector<pugi::xml_node> ordered;
  for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling()) {
    if (std::find(members.begin(), members.end(), c) != members.end()) ordered.push_back(c);
  }
  // The box around them (rotated boxes by their corners).
  double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
  for (pugi::xml_node s : shapes) {
    Box b = ReadBox(XfrmOf(s, false));
    double cx = b.x + b.w / 2, cy = b.y + b.h / 2, a = b.rot * 3.14159265358979323846 / 180;
    for (auto [dx, dy] : {std::pair{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}}) {
      double px = dx * b.w / 2, py = dy * b.h / 2;
      double rx = cx + px * std::cos(a) - py * std::sin(a), ry = cy + px * std::sin(a) + py * std::cos(a);
      x0 = std::min(x0, rx);
      y0 = std::min(y0, ry);
      x1 = std::max(x1, rx);
      y1 = std::max(y1, ry);
    }
  }
  uint32_t next = Impl::NextShapeId(root);
  std::string p = PrefixFor(parent, kP, "p"), a = PrefixFor(parent, kA, "a");
  pugi::xml_node group = parent.insert_child_after((p + ":grpSp").c_str(), ordered.back());
  pugi::xml_node nv = group.append_child((p + ":nvGrpSpPr").c_str());
  pugi::xml_node c = nv.append_child((p + ":cNvPr").c_str());
  c.append_attribute("id") = next;
  c.append_attribute("name") = ("Group " + std::to_string(next - 1)).c_str();
  nv.append_child((p + ":cNvGrpSpPr").c_str());
  nv.append_child((p + ":nvPr").c_str());
  pugi::xml_node props = group.append_child((p + ":grpSpPr").c_str());
  pugi::xml_node xfrm = props.append_child((a + ":xfrm").c_str());
  Box gb;
  gb.x = gb.chX = x0;
  gb.y = gb.chY = y0;
  gb.w = gb.chW = x1 - x0;
  gb.h = gb.chH = y1 - y0;
  WriteBox(xfrm, gb, true, true);
  for (pugi::xml_node s : ordered) group.append_move(s);
  if (groupId) *groupId = next;
  m.Touched(part, next);
  for (uint32_t id : ids) m.Touched(part, id);
  return call.End({});
}

Result Editor::Ungroup(size_t slide, uint32_t groupId, std::vector<uint32_t>* ids) {
  Call call(*this, u"Ungroup");
  Impl& m = *impl_;
  std::string part = m.SlidePart(slide);
  if (part.empty()) return call.End(Result::Fail(u"That slide isn't in the presentation."));
  pugi::xml_node root = m.Edit(part);
  pugi::xml_node group = Impl::FindShape(root, groupId);
  if (!group || LocalNameOf(group) != "grpSp") return call.End(Result::Fail(u"That isn't a group."));
  group = Impl::Unwrap(group);
  Box g = ReadBox(XfrmOf(group, false));
  double sx = g.chW > 0 ? g.w / g.chW : 1, sy = g.chH > 0 ? g.h / g.chH : 1;
  double gcx = g.x + g.w / 2, gcy = g.y + g.h / 2, ga = g.rot * 3.14159265358979323846 / 180;
  pugi::xml_node parent = group.parent();
  if (ids) ids->clear();
  std::vector<pugi::xml_node> children;
  for (pugi::xml_node c = group.first_child(); c; c = c.next_sibling()) {
    if (IsShapeElement(c)) children.push_back(c);
  }
  for (pugi::xml_node c : children) {
    pugi::xml_node shape = c;
    if (Is(c, pkg::mce::kNamespace, "AlternateContent")) {
      // Its chosen branch's shape is placed (and its twins with it); the block moves out whole.
      pugi::xml_node chosen;
      ForEachChild(group, [&](pugi::xml_node k) {
        for (pugi::xml_node a = k; a; a = a.parent()) {
          if (a == c) chosen = k;
        }
        return !chosen;
      });
      if (!chosen) continue;
      shape = Impl::Unwrap(chosen);
    }
    pugi::xml_node xfrm = XfrmOf(shape, false);
    if (xfrm) {
      Box b = ReadBox(xfrm);
      bool childGroup = LocalNameOf(shape) == "grpSp";
      // Child space to the group's parent space: scale, then the group's flips and rotation about its center.
      double w = b.w * sx, h = b.h * sy;
      double cx = g.x + (b.x + b.w / 2 - g.chX) * sx, cy = g.y + (b.y + b.h / 2 - g.chY) * sy;
      double rot = b.rot;
      bool flipH = b.flipH, flipV = b.flipV;
      if (g.flipH) {
        cx = 2 * gcx - cx;
        flipH = !flipH;
        rot = -rot;
      }
      if (g.flipV) {
        cy = 2 * gcy - cy;
        flipV = !flipV;
        rot = -rot;
      }
      double dx = cx - gcx, dy = cy - gcy;
      cx = gcx + dx * std::cos(ga) - dy * std::sin(ga);
      cy = gcy + dx * std::sin(ga) + dy * std::cos(ga);
      rot += g.rot;
      Box nb = b;
      nb.x = cx - w / 2;
      nb.y = cy - h / 2;
      nb.w = w;
      nb.h = h;
      nb.rot = rot;
      nb.flipH = flipH;
      nb.flipV = flipV;
      WriteBox(xfrm, nb, childGroup, LocalNameOf(shape) != "graphicFrame");
      if (LocalNameOf(shape) == "graphicFrame" && b.w > 0 && b.h > 0) ScaleTable(shape, sx, sy);
      SyncTwins(shape, kTwinBox);
    }
    pugi::xml_node moved = parent.insert_move_before(shape.parent() == group ? shape : c, group);
    uint32_t id = U32(CNvPr(moved), "id");
    if (ids) ids->push_back(id);
    m.Touched(part, id);
  }
  parent.remove_child(group);
  RemoveShapeRefs(m.registry, root, {groupId});
  m.Touched(part, groupId);
  return call.End({});
}

Result Editor::SetShapeProps(size_t slide, const std::vector<uint32_t>& ids, const ShapePatch& patch) {
  Call call(*this, u"Format Shape");
  Impl& m = *impl_;
  std::string part = m.SlidePart(slide);
  if (part.empty()) return call.End(Result::Fail(u"That slide isn't in the presentation."));
  pugi::xml_node root = m.Edit(part);
  for (uint32_t id : ids) {
    pugi::xml_node shape = Impl::FindShape(root, id);
    if (!shape) return call.End(Result::Fail(u"That shape isn't on the slide."));
    shape = Impl::Unwrap(shape);
    std::string_view k = LocalNameOf(shape);
    if (patch.fill || patch.line || patch.effects || patch.geometry) {
      if (k == "graphicFrame") return call.End(Result::Fail(u"A table's or chart's frame has no fill or outline of its own."));
      // A group's formatting goes to the shapes in it, as PowerPoint's Format Shape does (p:grpSpPr
      // holds no outline or geometry); frames in it are left as they are.
      bool ok = true;
      std::function<void(pugi::xml_node)> format = [&](pugi::xml_node s) {
        std::string_view sk = LocalNameOf(s);
        if (sk == "grpSp") {
          std::vector<pugi::xml_node> kids;
          ForEachChild(s, [&](pugi::xml_node c) {
            std::string_view ck = LocalNameOf(c);
            if (ck == "sp" || ck == "cxnSp" || ck == "pic" || ck == "grpSp") kids.push_back(c);
            return true;
          });
          for (pugi::xml_node c : kids) format(Impl::Unwrap(c));
          return;
        }
        if (sk != "sp" && sk != "cxnSp" && sk != "pic") return;
        pugi::xml_node props = P(s, "spPr");
        if (!props) {
          XfrmOf(s, true);
          props = P(s, "spPr");
        }
        auto apply = [&](const std::optional<std::string>& xml, int group) {
          return !xml || ReplaceGroup(props, *xml, SpPrGroupOf, group);
        };
        if (!apply(patch.geometry, 1) || !apply(patch.fill, 2) || !apply(patch.line, 3) || !apply(patch.effects, 4)) ok = false;
        SyncTwins(s, (patch.geometry ? kTwinGeometry : 0) | (patch.fill ? kTwinFill : 0) | (patch.line ? kTwinLine : 0) |
                         (patch.effects ? kTwinEffects : 0));
      };
      format(shape);
      if (!ok) return call.End(Result::Fail(u"That formatting isn't DrawingML Featherweight can write."));
    }
    pugi::xml_node c = CNvPr(shape);
    if (patch.name) (c.attribute("name") ? c.attribute("name") : c.append_attribute("name")).set_value(Utf8(*patch.name).c_str());
    if (patch.altText) {
      if (patch.altText->empty()) c.remove_attribute("descr");
      else (c.attribute("descr") ? c.attribute("descr") : c.append_attribute("descr")).set_value(Utf8(*patch.altText).c_str());
    }
    if (patch.hidden) {
      if (*patch.hidden) (c.attribute("hidden") ? c.attribute("hidden") : c.append_attribute("hidden")).set_value("1");
      else c.remove_attribute("hidden");
    }
    SyncTwins(shape, (patch.name ? kTwinName : 0) | (patch.altText ? kTwinAltText : 0) | (patch.hidden ? kTwinHidden : 0));
    m.Touched(part, id);
  }
  return call.End({});
}

Result Editor::SetCrop(size_t slide, uint32_t id, const Crop& crop) {
  Call call(*this, u"Crop");
  Impl& m = *impl_;
  std::string part = m.SlidePart(slide);
  if (part.empty()) return call.End(Result::Fail(u"That slide isn't in the presentation."));
  pugi::xml_node root = m.Edit(part);
  pugi::xml_node shape = Impl::FindShape(root, id);
  if (!shape) return call.End(Result::Fail(u"That shape isn't on the slide."));
  shape = Impl::Unwrap(shape);
  pugi::xml_node fill = LocalNameOf(shape) == "pic" ? P(shape, "blipFill") : A(P(shape, "spPr"), "blipFill");
  pugi::xml_node blip = A(fill, "blip");
  if (!blip) return call.End(Result::Fail(u"Only pictures can be cropped."));
  pugi::xml_node rect = A(fill, "srcRect");
  bool none = crop.left == 0 && crop.top == 0 && crop.right == 0 && crop.bottom == 0;
  if (none) {
    if (rect) fill.remove_child(rect);
  } else {
    if (!rect) rect = fill.insert_child_after((PrefixFor(fill, kA, "a") + ":srcRect").c_str(), blip);
    for (auto [name, v] : {std::pair{"l", crop.left}, {"t", crop.top}, {"r", crop.right}, {"b", crop.bottom}}) {
      long long val = std::llround(static_cast<double>(v) * 100000);
      if (val == 0) rect.remove_attribute(name);
      else SetAttr(rect, name, val);
    }
  }
  m.Touched(part, id);
  return call.End({});
}

Result Editor::SetShapeXml(size_t slide, uint32_t id, std::string_view xml, std::u16string stepName) {
  Call call(*this, std::move(stepName));
  Impl& m = *impl_;
  std::string part = m.SlidePart(slide);
  if (part.empty()) return call.End(Result::Fail(u"That slide isn't in the presentation."));
  pugi::xml_document doc;
  if (!ParseFragment(xml, doc) || !IsShapeElement(doc.document_element()) || !CNvPr(doc.document_element())) {
    return call.End(Result::Fail(u"That isn't a shape Featherweight can write."));
  }
  std::set<std::string> rels;
  RelsIn(m.registry, doc.document_element(), rels);
  const pkg::RelationshipSet* have = m.Rels(part);
  for (const std::string& r : rels) {
    if (!r.empty() && (!have || !have->Find(r))) return call.End(Result::Fail(u"The shape names a part the slide doesn't have."));
  }
  CNvPr(doc.document_element()).attribute("id").set_value(id);
  pugi::xml_node root = m.Edit(part);
  pugi::xml_node shape = Impl::FindShape(root, id);
  if (!shape) return call.End(Result::Fail(u"That shape isn't on the slide."));
  // In mc:AlternateContent the Choice's shape is replaced where it is: the block, its other Choices and
  // its Fallback stay, and what changed that applies to the Fallback's shape goes there too.
  shape = Impl::Unwrap(shape);
  std::set<std::string> before;
  RelsIn(m.registry, shape, before);
  pugi::xml_node added = InsertCopy(shape.parent(), doc.document_element(), shape);
  unsigned changed = TwinDifferences(shape, added);
  shape.parent().remove_child(shape);
  SyncTwins(added, changed);
  DropUnusedRels(m, part, root, before);
  m.Touched(part, id);
  return call.End({});
}

std::string Editor::ShapeXml(size_t slide, uint32_t id) const {
  const Impl& m = *impl_;
  std::string part = m.SlidePart(slide);
  pugi::xml_node shape = Impl::FindShape(m.Root(part), id);
  if (!shape) return {};
  pugi::xml_document doc;
  InsertCopy(doc, shape);
  std::ostringstream out;
  doc.document_element().print(out, "", pugi::format_raw);
  return out.str();
}

Result Editor::SetShapeLink(size_t slide, uint32_t id, const std::optional<LinkTarget>& link) {
  Call call(*this, u"Link");
  Impl& m = *impl_;
  std::string part = m.SlidePart(slide);
  if (part.empty()) return call.End(Result::Fail(u"That slide isn't in the presentation."));
  pugi::xml_node root = m.Edit(part);
  pugi::xml_node shape = Impl::FindShape(root, id);
  if (!shape) return call.End(Result::Fail(u"That shape isn't on the slide."));
  shape = Impl::Unwrap(shape);
  pugi::xml_node c = CNvPr(shape);
  std::set<std::string> old;
  if (pugi::xml_node h = A(c, "hlinkClick")) {
    RelsIn(m.registry, h, old);
    c.remove_child(h);
  }
  if (link) {
    pugi::xml_node h = c.prepend_child((PrefixFor(c, kA, "a") + ":hlinkClick").c_str());
    if (!WriteLink(m, part, h, *link)) return call.End(Result::Fail(u"That link can't be made."));
  }
  DropUnusedRels(m, part, root, old);
  m.Touched(part, id);
  return call.End({});
}

}  // namespace fw::slide::edit
