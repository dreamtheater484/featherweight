#include "slide/edit/SlideCopy.h"

#include <random>

namespace fw::slide::edit {

namespace {

// "/ppt/charts/chart3.xml" -> ("/ppt/charts/chart", ".xml"); trailing digits of the base dropped.
std::pair<std::string, std::string> StemOf(std::string_view part) {
  size_t slash = part.rfind('/');
  size_t dot = part.rfind('.');
  if (dot == std::string_view::npos || dot < slash) dot = part.size();
  size_t end = dot;
  while (end > slash + 1 && part[end - 1] >= '0' && part[end - 1] <= '9') --end;
  return {std::string(part.substr(0, end)), std::string(part.substr(dot))};
}

bool IsXmlName(std::string_view part) { return part.ends_with(".xml") || part.ends_with(".rels") || part.ends_with(".vml"); }

// A new p14:creationId for a copied slide (PowerPoint gives each slide its own).
void RenewCreationId(pugi::xml_node root) {
  static std::mt19937 rng{std::random_device{}()};
  for (pugi::xml_node ext = P(root, "extLst").first_child(); ext; ext = ext.next_sibling()) {
    for (pugi::xml_node c = ext.first_child(); c; c = c.next_sibling()) {
      if (LocalNameOf(c) == "creationId") c.attribute("val").set_value(static_cast<unsigned>(rng() | 1u));
    }
  }
}

}  // namespace

SlideCopier::SlideCopier(Editor::Impl& m, Presentation* source) : m_(m), source_(source) {}

DomPtr SlideCopier::SrcDom(std::string_view part) const {
  if (!source_) return m_.Current(part);
  return DomOf(*source_, part, nullptr);
}

bool SlideCopier::SrcBytes(std::string_view part, std::vector<uint8_t>& out) const {
  return source_ ? source_->ReadPart(part, out) : m_.Read(part, out);
}

const pkg::RelationshipSet* SlideCopier::SrcRels(std::string_view part) const {
  return source_ ? source_->RelationshipsOf(part) : m_.Rels(part);
}

std::string SlideCopier::SrcType(std::string_view part) const {
  if (!source_) return m_.ContentType(part);
  if (const PartStore* s = source_->Edits()) return s->ContentTypeOf(part);
  return std::string(source_->GetPackage().ContentTypeOf(part));
}

SlideCopier::Policy SlideCopier::PolicyOf(std::string_view type) {
  for (std::string_view shared : {"/slideLayout", "/image", "/media", "/audio", "/video", "/slide", "/notesMaster",
                                  "/slideMaster", "/theme", "/font", "/hyperlink"}) {
    if (type.ends_with(shared)) return Policy::Share;
  }
  if (type.ends_with("/comments")) return Policy::Drop;
  return Policy::Copy;
}

std::string SlideCopier::CopyPart(std::string_view src, std::string_view newSlide, std::string_view srcSlide) {
  std::string key = pkg::PartKey(src);
  if (auto it = copied_.find(key); it != copied_.end()) return it->second;
  auto [stem, ext] = StemOf(src);
  std::string name = m_.NewName(stem, ext);
  std::string type = SrcType(src);
  if (IsXmlName(src)) {
    DomPtr dom = SrcDom(src);
    if (!dom) return {};
    m_.PutXml(name, CloneDom(*dom), type);
  } else {
    std::vector<uint8_t> bytes;
    if (!SrcBytes(src, bytes)) return {};
    m_.PutBytes(name, std::move(bytes), type);
  }
  copied_[key] = name;
  CopyRels(src, name, newSlide, srcSlide);
  return name;
}

void SlideCopier::CopyRels(std::string_view src, std::string_view dst, std::string_view newSlide,
                           std::string_view srcSlide) {
  const pkg::RelationshipSet* rels = SrcRels(src);
  if (!rels || rels->items.empty()) return;
  std::vector<pkg::Relationship> items;
  std::set<std::string> dropped;
  for (const pkg::Relationship& r : rels->items) {
    pkg::Relationship n = r;
    if (r.external) {
      items.push_back(n);
      continue;
    }
    if (skipLayouts && r.type.ends_with("/slideLayout")) continue;
    Policy policy = PolicyOf(r.type);
    std::string target;
    if (pkg::PartKey(r.targetPart) == pkg::PartKey(srcSlide) && !srcSlide.empty()) {
      target.assign(newSlide);  // a notes page's (or a part's) link back to its slide
    } else if (policy == Policy::Drop) {
      dropped.insert(r.id);
      continue;
    } else if (policy == Policy::Share) {
      if (!source_) {
        target = r.targetPart;
      } else if (r.type.ends_with("/slideLayout")) {
        target = layoutPart;
      } else if (keepDesign && r.type.ends_with("/slideMaster") && !masterPart.empty()) {
        target = masterPart;
      } else if (keepDesign && r.type.ends_with("/theme")) {
        target = CopyPart(r.targetPart, newSlide, srcSlide);
      } else if (r.type.ends_with("/image") || r.type.ends_with("/media") || r.type.ends_with("/audio") ||
                 r.type.ends_with("/video") || r.type.ends_with("/font")) {
        target = CopyPart(r.targetPart, newSlide, srcSlide);
      } else {
        dropped.insert(r.id);  // a link to another slide of the other deck, its notes master...
        continue;
      }
      if (target.empty()) {
        dropped.insert(r.id);
        continue;
      }
    } else {
      target = CopyPart(r.targetPart, newSlide, srcSlide);
      if (target.empty()) {
        dropped.insert(r.id);
        continue;
      }
    }
    n.target = Editor::Impl::RelativeTarget(dst, target);
    n.targetPart = target;
    items.push_back(n);
  }
  pkg::RelationshipSet& out = m_.EditRels(dst);
  out.items = std::move(items);
  if (!dropped.empty()) {
    if (pugi::xml_node root = m_.Edit(dst)) {
      for (const pkg::FoundRef& f : m_.registry.Find(root, pkg::RefKind::RelationshipId)) {
        if (f.attr && dropped.count(std::string(f.Value()))) pugi::xml_attribute(f.attr).set_value("");
      }
    }
  }
}

std::string SlideCopier::CopySlide(std::string_view srcSlide) {
  DomPtr dom = SrcDom(srcSlide);
  if (!dom) return {};
  std::string name = m_.NewName("/ppt/slides/slide", ".xml");
  std::shared_ptr<pkg::XmlDom> copy = CloneDom(*dom);
  RenewCreationId(copy->Root());
  m_.PutXml(name, copy, kTypeSlide);
  copied_[pkg::PartKey(srcSlide)] = name;
  CopyRels(srcSlide, name, name, srcSlide);
  return name;
}

}  // namespace fw::slide::edit
