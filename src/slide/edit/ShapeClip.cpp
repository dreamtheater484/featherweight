// Copying and pasting shapes with the parts they use (P2, p2-model; for p2-objects' clipboard).
#include <algorithm>
#include <sstream>

#include "slide/edit/EditInternal.h"
#include "slide/edit/ShapeXml.h"

namespace fw::slide::edit {

namespace {

bool IsXmlName(std::string_view part) { return part.ends_with(".xml") || part.ends_with(".rels") || part.ends_with(".vml"); }

// Parts shared by name within a deck and stored once by bytes across decks.
bool Shared(std::string_view type) {
  for (std::string_view t : {"/image", "/media", "/audio", "/video"}) {
    if (type.ends_with(t)) return true;
  }
  return false;
}

std::pair<std::string, std::string> StemOf(std::string_view part) {
  size_t slash = part.rfind('/');
  size_t dot = part.rfind('.');
  if (dot == std::string_view::npos || dot < slash) dot = part.size();
  size_t end = dot;
  while (end > slash + 1 && part[end - 1] >= '0' && part[end - 1] <= '9') --end;
  return {std::string(part.substr(0, end)), std::string(part.substr(dot))};
}

// The part and the parts it names (not slides, layouts or masters), into the clip once each.
void Collect(const Editor::Impl& m, const std::string& part, ShapeClip& clip) {
  for (const ShapeClip::Part& p : clip.parts) {
    if (pkg::PartKey(p.name) == pkg::PartKey(part)) return;
  }
  ShapeClip::Part p;
  p.name = part;
  p.contentType = m.ContentType(part);
  if (!m.Read(part, p.bytes)) return;
  clip.parts.push_back(std::move(p));
  size_t index = clip.parts.size() - 1;
  if (const pkg::RelationshipSet* rels = m.Rels(part)) {
    for (const pkg::Relationship& r : rels->items) {
      ShapeClip::Part::Rel rel{r.id, r.type, r.external ? r.target : r.targetPart, r.external};
      if (!r.external) {
        if (r.type.ends_with("/slide") || r.type.ends_with("/slideLayout") || r.type.ends_with("/slideMaster")) continue;
        Collect(m, r.targetPart, clip);
      }
      clip.parts[index].rels.push_back(std::move(rel));
    }
  }
}

}  // namespace

ShapeClip Editor::CopyShapes(size_t slide, const std::vector<uint32_t>& ids) const {
  const Impl& m = *impl_;
  ShapeClip clip;
  clip.deck = &deck_;
  std::string part = m.SlidePart(slide);
  pugi::xml_node root = m.Root(part);
  if (!root) return clip;
  // In z order: the shape tree's order, whatever the order of `ids`.
  std::vector<pugi::xml_node> units;
  for (uint32_t id : ids) {
    pugi::xml_node s = Impl::FindShape(root, id);
    if (s && std::find(units.begin(), units.end(), s) == units.end()) units.push_back(s);
  }
  std::vector<std::pair<size_t, pugi::xml_node>> ordered;
  std::vector<pugi::xml_node> stack{root};
  size_t order = 0;
  while (!stack.empty()) {
    pugi::xml_node n = stack.back();
    stack.pop_back();
    for (pugi::xml_node c = n.last_child(); c; c = c.previous_sibling()) stack.push_back(c);
    if (std::find(units.begin(), units.end(), n) != units.end()) ordered.emplace_back(order, n);
    ++order;
  }
  std::set<std::string> names;
  for (const auto& [o, s] : ordered) {
    pugi::xml_document doc;
    InsertCopy(doc, s);
    std::ostringstream out;
    doc.document_element().print(out, "", pugi::format_raw);
    clip.shapes.push_back(out.str());
    for (const pkg::FoundRef& f : m.registry.Find(s, pkg::RefKind::RelationshipId)) names.insert(std::string(f.Value()));
  }
  if (const pkg::RelationshipSet* rels = m.Rels(part)) {
    for (const pkg::Relationship& r : rels->items) {
      if (!names.count(r.id)) continue;
      clip.rels.push_back({r.id, r.type, r.external ? r.target : r.targetPart, r.external});
      if (!r.external && !r.type.ends_with("/slide")) Collect(m, r.targetPart, clip);
    }
  }
  return clip;
}

Result Editor::PasteShapes(size_t slide, const ShapeClip& clip, std::vector<uint32_t>* ids) {
  Call call(*this, u"Paste");
  Impl& m = *impl_;
  std::string part = m.SlidePart(slide);
  if (part.empty()) return call.End(Result::Fail(u"That slide isn't in the presentation."));
  if (clip.Empty()) return call.End(Result::Fail(u"There's nothing to paste."));
  bool sameDeck = clip.deck == &deck_;
  // The clip's parts here: shared ones found by bytes (or used again within this deck), the rest new.
  std::map<std::string, std::string> placed;  // clip part key -> name here
  std::function<std::string(const std::string&)> place = [&](const std::string& name) -> std::string {
    std::string key = pkg::PartKey(name);
    if (auto it = placed.find(key); it != placed.end()) return it->second;
    auto p = std::find_if(clip.parts.begin(), clip.parts.end(), [&](const ShapeClip::Part& x) { return pkg::PartKey(x.name) == key; });
    if (p == clip.parts.end()) return {};
    bool shared = p->name.starts_with("/ppt/media/");
    std::string here;
    if (shared) {
      if (sameDeck && m.Exists(p->name)) {
        std::vector<uint8_t> have;
        if (m.Read(p->name, have) && have == p->bytes) here = p->name;
      }
      for (const std::string& n : m.store.PartNames()) {
        if (!here.empty() || !n.starts_with("/ppt/media/")) continue;
        std::vector<uint8_t> have;
        if (m.Read(n, have) && have == p->bytes) here = n;
      }
    }
    if (here.empty()) {
      auto [stem, ext] = StemOf(p->name);
      here = m.NewName(stem, ext);
      if (IsXmlName(here)) {
        auto dom = ParseDom(std::string_view(reinterpret_cast<const char*>(p->bytes.data()), p->bytes.size()),
                            deck_.GetPackage().GetLimits(), here);
        if (!dom) return {};
        m.PutXml(here, dom, p->contentType);
      } else {
        m.PutBytes(here, p->bytes, p->contentType);
      }
      placed[key] = here;
      if (!p->rels.empty()) {
        std::vector<pkg::Relationship> items;
        for (const ShapeClip::Part::Rel& r : p->rels) {
          pkg::Relationship rel;
          rel.id = r.id;
          rel.type = r.type;
          rel.external = r.external;
          if (r.external) {
            rel.target = r.target;
          } else {
            std::string t = place(r.target);
            if (t.empty()) continue;
            rel.target = Impl::RelativeTarget(here, t);
            rel.targetPart = t;
          }
          items.push_back(std::move(rel));
        }
        m.EditRels(here).items = std::move(items);
      }
      return here;
    }
    placed[key] = here;
    return here;
  };
  // The slide's new relationships, and what the shapes' r:ids become.
  std::map<std::string, std::string> renamed;
  for (const ShapeClip::Part::Rel& r : clip.rels) {
    std::string id;
    if (r.external) {
      id = m.AddRel(part, r.type, r.target, true);
    } else if (r.type.ends_with("/slide")) {
      if (sameDeck && m.Exists(r.target)) id = m.AddRel(part, r.type, r.target);
    } else if (std::string t = place(r.target); !t.empty()) {
      id = m.AddRel(part, r.type, t);
    }
    renamed[r.id] = id;  // empty: a link that can't come along
  }
  if (ids) ids->clear();
  for (const std::string& xml : clip.shapes) {
    pugi::xml_document doc;
    if (!ParseFragment(xml, doc)) return call.End(Result::Fail(u"The copied shapes can't be read."));
    // Through placeholders first, so a new id that equals an old one isn't renamed twice.
    std::vector<std::pair<pugi::xml_attribute, std::string>> sites;
    for (const pkg::FoundRef& f : m.registry.Find(doc.document_element(), pkg::RefKind::RelationshipId)) {
      auto it = renamed.find(std::string(f.Value()));
      if (it != renamed.end() && f.attr) sites.emplace_back(f.attr, it->second);
    }
    for (auto& [a, v] : sites) pugi::xml_attribute(a).set_value(v.c_str());
    uint32_t id = 0;
    if (Result r = PutShape(m, part, doc.document_element(), -1, &id); !r) return call.End(r);
    if (ids) ids->push_back(id);
  }
  return call.End({});
}

}  // namespace fw::slide::edit
