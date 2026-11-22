#include "slide/objects/Selection.h"

#include <algorithm>

#include "draw/Xml.h"

namespace fw::slide::objects {

namespace {
constexpr int kMaxDepth = 32;
}

void ObjectMap::Build(const fw::slide::SlideBase& slide) {
  objects_.clear();
  for (const fw::slide::Shape& s : slide.shapes) Add(s, 0, draw::Matrix(), 0);
}

void ObjectMap::Add(const fw::slide::Shape& s, uint32_t parent, const draw::Matrix& parentToSlide, int depth) {
  using K = fw::slide::Shape::Kind;
  if (s.hidden || s.id == 0 || depth > kMaxDepth) return;
  ObjectInfo o;
  o.id = s.id;
  o.parent = parent;
  o.shape = &s;
  o.kind = KindOf(s.node);
  o.placeholder = static_cast<bool>(s.placeholder);
  o.line = s.kind == K::Connector;
  if (!o.line && s.kind == K::Shape) {
    // A p:sp drawn as a line ("line" preset) has ends, not a box.
    pugi::xml_node geom = draw::xml::A(draw::xml::Child(s.drawNode, draw::xml::kP, "spPr"), "prstGeom");
    const std::string_view prst = geom.attribute("prst").value();
    o.line = prst == "line" || prst == "straightConnector1";
  }
  o.lockAspect = s.kind == K::Picture || s.kind == K::Group;
  o.local.rect = s.bounds;
  o.local.rotation = s.rotation;
  o.local.flipH = s.flipH;
  o.local.flipV = s.flipV;
  o.parentToSlide = parentToSlide;
  o.frame = parent ? FrameOnSlide(o.local, parentToSlide) : o.local;
  objects_.push_back(o);
  if (s.kind == K::Group && !s.children.empty()) {
    Xfrm x;
    if (!ReadXfrm(s.node, x)) x = FromFrame(o.local, Xfrm());
    const draw::Matrix child = ChildSpace(x, o.local.Transform() * parentToSlide);
    for (const fw::slide::Shape& c : s.children) Add(c, s.id, child, depth + 1);
  }
}

const ObjectInfo* ObjectMap::Find(uint32_t id) const {
  for (const ObjectInfo& o : objects_)
    if (o.id == id) return &o;
  return nullptr;
}

std::vector<const ObjectInfo*> ObjectMap::Children(uint32_t parent) const {
  std::vector<const ObjectInfo*> out;
  for (const ObjectInfo& o : objects_)
    if (o.parent == parent) out.push_back(&o);
  return out;
}

bool ObjectMap::Within(uint32_t id, uint32_t ancestor) const {
  for (int guard = 0; id && guard <= kMaxDepth + 1; ++guard) {
    if (id == ancestor) return true;
    const ObjectInfo* o = Find(id);
    id = o ? o->parent : 0;
  }
  return ancestor == 0;
}

uint32_t ObjectMap::AtLevel(uint32_t id, uint32_t level) const {
  for (int guard = 0; id && guard <= kMaxDepth + 1; ++guard) {
    const ObjectInfo* o = Find(id);
    if (!o) return 0;
    if (o->parent == level) return id;
    id = o->parent;
  }
  return 0;
}

uint32_t ObjectMap::HitDeepest(draw::Point p, float tolerance) const {
  // Later objects are drawn above: look from the top. Groups themselves are hit through children.
  for (auto it = objects_.rbegin(); it != objects_.rend(); ++it) {
    if (it->kind == ObjectKind::Group) continue;
    if (HitFrame(it->frame, it->line, p, tolerance)) return it->id;
  }
  return 0;
}

uint32_t ObjectMap::HitTest(draw::Point p, uint32_t level, float tolerance) const {
  for (auto it = objects_.rbegin(); it != objects_.rend(); ++it) {
    if (it->kind == ObjectKind::Group || !Within(it->id, level) || it->id == level) continue;
    if (HitFrame(it->frame, it->line, p, tolerance)) return AtLevel(it->id, level);
  }
  return 0;
}

std::vector<uint32_t> ObjectMap::InMarquee(const draw::Rect& marquee, uint32_t level) const {
  std::vector<uint32_t> out;
  for (const ObjectInfo& o : objects_)
    if (o.parent == level && Contains(marquee, o.frame.Bounds())) out.push_back(o.id);
  return out;
}

uint32_t ObjectMap::Next(uint32_t current, uint32_t level, bool back) const {
  const std::vector<const ObjectInfo*> kids = Children(level);
  if (kids.empty()) return 0;
  const auto it = std::find_if(kids.begin(), kids.end(), [&](const ObjectInfo* o) { return o->id == current; });
  if (it == kids.end()) return back ? kids.back()->id : kids.front()->id;
  const size_t i = static_cast<size_t>(it - kids.begin()), n = kids.size();
  return kids[back ? (i + n - 1) % n : (i + 1) % n]->id;
}

draw::Rect SelectionBounds(const ObjectMap& map, const std::vector<uint32_t>& ids) {
  draw::Rect out{};
  bool first = true;
  for (uint32_t id : ids) {
    const ObjectInfo* o = map.Find(id);
    if (!o) continue;
    const draw::Rect b = o->frame.Bounds();
    out = first ? b : Union(out, b);
    first = false;
  }
  return out;
}

}  // namespace fw::slide::objects
