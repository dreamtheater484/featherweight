// Slide operations through the reference registry (plan §8.3; P2, p2-model): new, duplicate, delete,
// move, hide, change layout, notes. The slide list, sections, custom shows, links between slides,
// notes and relationships follow.
#include <algorithm>
#include <random>

#include "slide/edit/EditInternal.h"
#include "slide/edit/SlideCopy.h"

namespace fw::slide::edit {

namespace {

constexpr std::string_view kSectionUri = "{521415D9-36F7-43E2-AB2F-B90AF26B5E84}";

uint32_t U32(pugi::xml_node n, const char* attr) {
  double d = Number(n, attr, 0);
  return d >= 0 && d <= 4294967295.0 ? static_cast<uint32_t>(d) : 0;
}

// p14:sectionLst in the presentation part's extension list; null when there's none.
pugi::xml_node SectionList(pugi::xml_node root) {
  for (pugi::xml_node ext = P(root, "extLst").first_child(); ext; ext = ext.next_sibling()) {
    if (std::string_view(ext.attribute("uri").value()) != kSectionUri) continue;
    for (pugi::xml_node c = ext.first_child(); c; c = c.next_sibling()) {
      if (LocalNameOf(c) == "sectionLst") return c;
    }
  }
  return {};
}

// The section element (p14:section) whose list holds slide `id`; null when none does.
pugi::xml_node SectionOf(pugi::xml_node list, uint32_t id) {
  for (pugi::xml_node sec = list.first_child(); sec; sec = sec.next_sibling()) {
    if (sec.type() != pugi::node_element) continue;
    for (pugi::xml_node s = Child(sec, kP14, "sldIdLst").first_child(); s; s = s.next_sibling()) {
      if (s.type() == pugi::node_element && U32(s, "id") == id) return sec;
    }
  }
  return {};
}

// Puts slide `id` into `section`'s list after slide `after` (0: first).
void AddToSection(pugi::xml_node section, uint32_t id, uint32_t after) {
  pugi::xml_node list = Child(section, kP14, "sldIdLst");
  if (!list) {
    std::string prefix = PrefixFor(section, kP14, "p14");
    list = section.prepend_child((prefix + ":sldIdLst").c_str());
  }
  std::string prefix = PrefixFor(list, kP14, "p14");
  pugi::xml_node before;
  if (after) {
    for (pugi::xml_node s = list.first_child(); s; s = s.next_sibling()) {
      if (s.type() == pugi::node_element && U32(s, "id") == after) {
        before = s.next_sibling();
        while (before && before.type() != pugi::node_element) before = before.next_sibling();
        break;
      }
    }
  } else {
    before = list.first_child();
  }
  pugi::xml_node n = before ? list.insert_child_before((prefix + ":sldId").c_str(), before)
                            : list.append_child((prefix + ":sldId").c_str());
  n.append_attribute("id") = id;
}

// Every reference to slides `ids` outside the slide list removed, through the registry (sections).
void RemoveSlideRefs(const pkg::ReferenceRegistry& registry, pugi::xml_node root, const std::set<uint32_t>& ids) {
  std::vector<pugi::xml_node> gone;
  for (const pkg::FoundRef& f : registry.Find(root, pkg::RefKind::SlideId)) {
    if (f.site->role & pkg::kDefines) continue;
    uint32_t v = static_cast<uint32_t>(Number(f.Value(), 0));
    if (ids.count(v)) gone.push_back(f.element);
  }
  for (pugi::xml_node n : gone) n.parent().remove_child(n);
}

// A new p:sldId id: one past the highest (PowerPoint's way), from 256.
uint32_t NewSlideId(pugi::xml_node root) {
  uint32_t max = 255;
  for (pugi::xml_node id = P(root, "sldIdLst").first_child(); id; id = id.next_sibling()) {
    if (id.type() == pugi::node_element) max = std::max(max, U32(id, "id"));
  }
  if (max >= 2147483647u) {
    // Full at the top: the lowest free one.
    std::set<uint32_t> used;
    for (pugi::xml_node id = P(root, "sldIdLst").first_child(); id; id = id.next_sibling()) used.insert(U32(id, "id"));
    for (uint32_t v = 256; v < 2147483647u; ++v) {
      if (!used.count(v)) return v;
    }
  }
  return max + 1;
}

// The presentation part's p:sldIdLst, made (in schema order) when missing.
pugi::xml_node SlideIdList(pugi::xml_node root) {
  pugi::xml_node list = DirectChild(root, kP, "sldIdLst");
  if (list) return list;
  std::string p = PrefixFor(root, kP, "p");
  return AddChildOrdered(root, p + ":sldIdLst",
                         {"sldSz", "notesSz", "smartTags", "embeddedFontLst", "custShowLst", "photoAlbum", "custDataLst",
                          "kinsoku", "defaultTextStyle", "modifyVerifier", "extLst"});
}

// Inserts a p:sldId for (id, rel) at `index` among the list's entries that name a part.
pugi::xml_node InsertSlideId(pugi::xml_node root, const pkg::RelationshipSet* rels, size_t index, uint32_t id,
                             std::string_view rel) {
  pugi::xml_node list = SlideIdList(root);
  pugi::xml_node before;
  size_t i = 0;
  for (pugi::xml_node n = list.first_child(); n; n = n.next_sibling()) {
    if (!Is(n, kP, "sldId") || TargetOf(rels, RelId(n, "id")).empty()) continue;
    if (i++ == index) {
      before = n;
      break;
    }
  }
  std::string p = PrefixFor(list, kP, "p"), r = PrefixFor(list, kRelNs, "r");
  pugi::xml_node n = before ? list.insert_child_before((p + ":sldId").c_str(), before) : list.append_child((p + ":sldId").c_str());
  n.append_attribute("id") = id;
  n.append_attribute((r + ":id").c_str()) = std::string(rel).c_str();
  return n;
}

// The p:sldId of relationship `rel`.
pugi::xml_node SlideIdNode(pugi::xml_node root, std::string_view rel) {
  for (pugi::xml_node n = P(root, "sldIdLst").first_child(); n; n = n.next_sibling()) {
    if (Is(n, kP, "sldId") && RelId(n, "id") == rel) return n;
  }
  return {};
}

std::string PlaceholderName(std::string_view type) {
  if (type == "title" || type == "ctrTitle") return "Title";
  if (type == "subTitle") return "Subtitle";
  if (type == "body") return "Text Placeholder";
  if (type == "pic") return "Picture Placeholder";
  if (type == "chart") return "Chart Placeholder";
  if (type == "tbl") return "Table Placeholder";
  if (type == "media") return "Media Placeholder";
  if (type == "clipArt") return "Clip Art Placeholder";
  if (type == "dgm") return "SmartArt Placeholder";
  if (type == "dt") return "Date Placeholder";
  if (type == "ftr") return "Footer Placeholder";
  if (type == "sldNum") return "Slide Number Placeholder";
  if (type == "hdr") return "Header Placeholder";
  return "Content Placeholder";
}

}  // namespace

// ---- Shared with the other operations (SlideCopy.h)

pugi::xml_node PlaceholderOf(pugi::xml_node shape) {
  pugi::xml_node nv;
  for (pugi::xml_node n = shape.first_child(); n && !nv; n = n.next_sibling()) {
    std::string_view k = LocalNameOf(n);
    if (k.size() > 4 && k.starts_with("nv") && k.ends_with("Pr")) nv = n;
  }
  return P(P(nv, "nvPr"), "ph");
}

std::string EmptyPlaceholderXml(pugi::xml_node layoutPh, uint32_t id) {
  std::string type = layoutPh.attribute("type").value();
  std::string ph = "<p:ph";
  if (!type.empty()) ph += " type=\"" + type + "\"";
  for (const char* a : {"orient", "sz", "idx"}) {
    if (pugi::xml_attribute v = layoutPh.attribute(a)) ph += std::string(" ") + a + "=\"" + v.value() + "\"";
  }
  ph += "/>";
  std::string name = PlaceholderName(type) + " " + std::to_string(id > 1 ? id - 1 : id);
  std::string xml = "<p:sp xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" "
                    "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><p:nvSpPr><p:cNvPr id=\"" +
                    std::to_string(id) + "\" name=\"" + name +
                    "\"/><p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr><p:nvPr>" + ph +
                    "</p:nvPr></p:nvSpPr><p:spPr/>";
  if (type != "pic" && type != "chart" && type != "tbl" && type != "media" && type != "clipArt" && type != "dgm") {
    xml += "<p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:endParaRPr lang=\"en-US\"/></a:p></p:txBody>";
  }
  xml += "</p:sp>";
  return xml;
}

bool PlaceholderIsEmpty(pugi::xml_node shape) {
  if (LocalNameOf(shape) != "sp") return false;  // a picture, table or chart in a placeholder is content
  pugi::xml_node body = P(shape, "txBody");
  for (pugi::xml_node p = body.first_child(); p; p = p.next_sibling()) {
    if (!Is(p, kA, "p")) continue;
    for (pugi::xml_node r = p.first_child(); r; r = r.next_sibling()) {
      std::string_view k = LocalNameOf(r);
      if ((k == "r" || k == "fld") && *A(r, "t").text().get()) return false;
    }
  }
  return true;
}

namespace {

// Types that can stand for each other when placeholders are matched.
std::string_view FamilyOf(std::string_view type) {
  if (type == "title" || type == "ctrTitle") return "title";
  if (type == "dt" || type == "ftr" || type == "sldNum" || type == "hdr" || type == "sldImg") return type;
  return "body";
}

}  // namespace

void MapPlaceholders(pugi::xml_node slideRoot, pugi::xml_node layoutRoot, std::string_view slidePart,
                     Editor::Impl& m, const std::map<uint32_t, draw::Rect>& bounds) {
  pugi::xml_node tree = P(P(slideRoot, "cSld"), "spTree");
  pugi::xml_node layoutTree = P(P(layoutRoot, "cSld"), "spTree");
  struct LayoutPh {
    pugi::xml_node ph;
    bool taken = false;
  };
  std::vector<LayoutPh> layout;
  ForEachChosenChild(layoutTree, [&](pugi::xml_node c) {
    if (pugi::xml_node ph = PlaceholderOf(c)) layout.push_back({ph});
    return true;
  });
  std::vector<std::pair<pugi::xml_node, pugi::xml_node>> slidePhs;  // shape, ph
  for (pugi::xml_node c = tree.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    if (pugi::xml_node ph = PlaceholderOf(c)) slidePhs.emplace_back(c, ph);
  }
  std::vector<int> match(slidePhs.size(), -1);
  auto type = [](pugi::xml_node ph) { return std::string_view(ph.attribute("type").value()); };
  auto idx = [](pugi::xml_node ph) { return std::string_view(ph.attribute("idx").value()); };
  // Pass 1: the same family and index; pass 2: the same family, in order.
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t i = 0; i < slidePhs.size(); ++i) {
      if (match[i] >= 0) continue;
      pugi::xml_node ph = slidePhs[i].second;
      for (size_t j = 0; j < layout.size(); ++j) {
        if (layout[j].taken || FamilyOf(type(layout[j].ph)) != FamilyOf(type(ph))) continue;
        if (pass == 0 && idx(layout[j].ph) != idx(ph)) continue;
        match[i] = static_cast<int>(j);
        layout[j].taken = true;
        break;
      }
    }
  }
  for (size_t i = 0; i < slidePhs.size(); ++i) {
    auto [shape, ph] = slidePhs[i];
    if (match[i] >= 0) {
      pugi::xml_node lph = layout[static_cast<size_t>(match[i])].ph;
      for (const char* a : {"type", "idx"}) {
        if (pugi::xml_attribute v = lph.attribute(a)) {
          (ph.attribute(a) ? ph.attribute(a) : ph.append_attribute(a)).set_value(v.value());
        } else {
          ph.remove_attribute(a);
        }
      }
      continue;
    }
    std::string_view t = type(ph);
    if (t == "dt" || t == "ftr" || t == "sldNum" || t == "hdr" || PlaceholderIsEmpty(shape)) {
      tree.remove_child(shape);
      continue;
    }
    // Content with nowhere to go stays where it is: its place written down.
    pugi::xml_node cNvPr;
    for (pugi::xml_node n = shape.first_child(); n && !cNvPr; n = n.next_sibling()) cNvPr = P(n, "cNvPr");
    auto b = bounds.find(U32(cNvPr, "id"));
    pugi::xml_node spPr = P(shape, "spPr");
    if (b != bounds.end() && spPr && !A(spPr, "xfrm")) {
      std::string a = PrefixFor(spPr, kA, "a");
      pugi::xml_node xfrm = spPr.prepend_child((a + ":xfrm").c_str());
      pugi::xml_node off = xfrm.append_child((a + ":off").c_str());
      off.append_attribute("x") = static_cast<long long>(b->second.left * 12700);
      off.append_attribute("y") = static_cast<long long>(b->second.top * 12700);
      pugi::xml_node ext = xfrm.append_child((a + ":ext").c_str());
      ext.append_attribute("cx") = static_cast<long long>(b->second.Width() * 12700);
      ext.append_attribute("cy") = static_cast<long long>(b->second.Height() * 12700);
    }
  }
  // The layout's placeholders nothing matched, added empty (not the footer's, date's and number's).
  for (LayoutPh& l : layout) {
    std::string_view t = type(l.ph);
    if (l.taken || t == "dt" || t == "ftr" || t == "sldNum" || t == "hdr") continue;
    pugi::xml_document doc;
    if (!ParseFragment(EmptyPlaceholderXml(l.ph, Editor::Impl::NextShapeId(slideRoot)), doc)) continue;
    pugi::xml_node added = InsertCopy(tree, doc.document_element());
    m.Touched(slidePart, U32(P(P(added, "nvSpPr"), "cNvPr"), "id"));
  }
}

// ---- Slides

Result Editor::NewSlide(size_t at, size_t layout, uint32_t* id) {
  Call call(*this, u"New Slide");
  Impl& m = *impl_;
  std::vector<Impl::SlideEntry> list = m.SlideList();
  if (at > list.size()) return call.End(Result::Fail(u"That isn't a place in the presentation."));
  if (layout >= deck_.Layouts().size()) return call.End(Result::Fail(u"That layout isn't in the presentation."));
  const Layout& l = *deck_.Layouts()[layout];
  pugi::xml_node layoutRoot = m.Root(l.part);
  if (!layoutRoot) return call.End(Result::Fail(u"That layout can't be read."));
  std::string part = m.NewName("/ppt/slides/slide", ".xml");
  std::string xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
      "<p:sld xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
      "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
      "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"><p:cSld><p:spTree><p:nvGrpSpPr>"
      "<p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/>"
      "<a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>"
      "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>";
  std::shared_ptr<pkg::XmlDom> dom = ParseDom(xml, deck_.GetPackage().GetLimits(), part);
  pugi::xml_node tree = P(P(dom->Root(), "cSld"), "spTree");
  uint32_t next = 2;
  ForEachChosenChild(P(P(layoutRoot, "cSld"), "spTree"), [&](pugi::xml_node c) {
    pugi::xml_node ph = PlaceholderOf(c);
    std::string_view t = ph.attribute("type").value();
    if (!ph || t == "dt" || t == "ftr" || t == "sldNum" || t == "hdr") return true;
    pugi::xml_document doc;
    if (ParseFragment(EmptyPlaceholderXml(ph, next), doc)) {
      InsertCopy(tree, doc.document_element());
      ++next;
    }
    return true;
  });
  m.PutXml(part, dom, kTypeSlide);
  m.AddRel(part, kRelLayout, l.part);
  std::string pres = m.PresentationPart();
  std::string rel = m.AddRel(pres, kRelSlide, part);
  pugi::xml_node root = m.Edit(pres);
  uint32_t newId = NewSlideId(root);
  InsertSlideId(root, m.Rels(pres), at, newId, rel);
  // Sections: it joins the section of the slide before it (the first section at the start).
  if (pugi::xml_node sections = SectionList(root)) {
    uint32_t after = at > 0 ? list[at - 1].id : 0;
    pugi::xml_node sec = after ? SectionOf(sections, after) : pugi::xml_node();
    if (!sec) {
      for (sec = sections.first_child(); sec && sec.type() != pugi::node_element;) sec = sec.next_sibling();
      after = 0;
    }
    if (sec) AddToSection(sec, newId, after);
  }
  if (id) *id = newId;
  return call.End({});
}

Result Editor::DuplicateSlides(const std::vector<size_t>& slides, std::vector<uint32_t>* ids) {
  Call call(*this, slides.size() > 1 ? u"Duplicate Slides" : u"Duplicate Slide");
  Impl& m = *impl_;
  std::vector<Impl::SlideEntry> list = m.SlideList();
  if (Result r = m.CheckSlides(slides, list.size()); !r) return call.End(r);
  std::vector<size_t> order = slides;
  std::sort(order.begin(), order.end());
  order.erase(std::unique(order.begin(), order.end()), order.end());
  std::string pres = m.PresentationPart();
  if (ids) ids->clear();
  // Each copy goes right after its original; later originals shift by the copies before them.
  size_t shift = 0;
  for (size_t index : order) {
    const Impl::SlideEntry& e = list[index];
    SlideCopier copier(m, nullptr);
    std::string part = copier.CopySlide(e.part);
    if (part.empty()) return call.End(Result::Fail(u"The slide can't be copied, because a part of it can't be read."));
    std::string rel = m.AddRel(pres, kRelSlide, part);
    pugi::xml_node root = m.Edit(pres);
    uint32_t newId = NewSlideId(root);
    InsertSlideId(root, m.Rels(pres), index + shift + 1, newId, rel);
    if (pugi::xml_node sections = SectionList(root)) {
      if (pugi::xml_node sec = SectionOf(sections, e.id)) AddToSection(sec, newId, e.id);
    }
    if (ids) ids->push_back(newId);
    ++shift;
  }
  return call.End({});
}

Result Editor::DeleteSlides(const std::vector<size_t>& slides) {
  Call call(*this, slides.size() > 1 ? u"Delete Slides" : u"Delete Slide");
  Impl& m = *impl_;
  std::vector<Impl::SlideEntry> list = m.SlideList();
  if (Result r = m.CheckSlides(slides, list.size()); !r) return call.End(r);
  std::string pres = m.PresentationPart();
  std::set<uint32_t> ids;
  std::set<std::string> rels, parts;
  for (size_t i : slides) {
    ids.insert(list[i].id);
    rels.insert(list[i].rel);
    parts.insert(pkg::PartKey(list[i].part));
  }
  // An extension Featherweight doesn't know might name the slides: refused, as plan §4.3 rule 2 says.
  {
    std::vector<pkg::EditImpact> impact(2);
    impact[0].kind = pkg::RefKind::SlideId;
    for (uint32_t id : ids) impact[0].values.push_back(std::to_string(id));
    impact[1].kind = pkg::RefKind::RelationshipId;
    impact[1].values.assign(rels.begin(), rels.end());
    if (!m.registry.WouldStrand(m.Root(pres), impact).empty()) {
      return call.End(Result::Fail(
          u"The slide can't be deleted, because the presentation has content from a newer program that may refer to it."));
    }
  }
  pugi::xml_node root = m.Edit(pres);
  for (const std::string& rel : rels) {
    if (pugi::xml_node n = SlideIdNode(root, rel)) n.parent().remove_child(n);
  }
  RemoveSlideRefs(m.registry, root, ids);
  // Custom shows list slides by relationship id.
  std::vector<pugi::xml_node> gone;
  for (const pkg::FoundRef& f : m.registry.Find(root, pkg::RefKind::RelationshipId)) {
    if (!rels.count(std::string(f.Value()))) continue;
    if (LocalNameOf(f.element) == "sld") gone.push_back(f.element);
    else if (f.attr) pugi::xml_attribute(f.attr).set_value("");
  }
  for (pugi::xml_node n : gone) n.parent().remove_child(n);
  pkg::RelationshipSet& presRels = m.EditRels(pres);
  std::erase_if(presRels.items, [&](const pkg::Relationship& r) { return rels.count(r.id) > 0; });
  // Links to these slides from what stays (other slides, notes pages, layouts, masters) go: an
  // a:hlinkClick, a:hlinkHover or a:hlinkMouseOver that jumps to one is dropped, its text or shape kept
  // without the link, and the relationship goes. (PowerPoint repairs a slide jump left without r:id.)
  std::set<std::string> going = parts;  // the deleted slides and their notes pages
  for (size_t i : slides) {
    if (const pkg::RelationshipSet* sr = m.Rels(list[i].part)) {
      for (const pkg::Relationship& r : sr->items) {
        if (!r.external && r.type.ends_with("/notesSlide")) going.insert(pkg::PartKey(r.targetPart));
      }
    }
  }
  for (const std::string& name : m.store.PartNames()) {
    if (going.count(pkg::PartKey(name))) continue;
    const pkg::RelationshipSet* sr = m.Rels(name);
    if (!sr) continue;
    std::set<std::string> dead;
    for (const pkg::Relationship& r : sr->items) {
      if (!r.external && parts.count(pkg::PartKey(r.targetPart)) && r.type.ends_with("/slide")) dead.insert(r.id);
    }
    if (dead.empty()) continue;
    auto uses = [&](pugi::xml_node root) {
      std::vector<pkg::FoundRef> found;
      for (const pkg::FoundRef& f : m.registry.Find(root, pkg::RefKind::RelationshipId)) {
        if (f.attr && dead.count(std::string(f.Value()))) found.push_back(f);
      }
      return found;
    };
    if (!uses(m.Root(name)).empty()) {  // the part copied only when it names them
      std::vector<pugi::xml_node> links;
      for (const pkg::FoundRef& f : uses(m.Edit(name))) {
        const std::string_view k = LocalNameOf(f.element);
        if (k == "hlinkClick" || k == "hlinkHover" || k == "hlinkMouseOver") links.push_back(f.element);
        else pugi::xml_attribute(f.attr).set_value("");  // not a link: nothing known names a slide otherwise
      }
      for (pugi::xml_node n : links) n.parent().remove_child(n);
    }
    pkg::RelationshipSet& edited = m.EditRels(name);
    std::erase_if(edited.items, [&](const pkg::Relationship& r) { return dead.count(r.id) > 0; });
  }
  // The show's slide range stays within the deck.
  std::string props = TargetOfType(m.Rels(pres), "presProps");
  if (pugi::xml_node range = P(P(m.Root(props), "showPr"), "sldRg")) {
    (void)range;
    pugi::xml_node edited = P(P(m.Edit(props), "showPr"), "sldRg");
    int count = static_cast<int>(list.size() - ids.size());
    int st = static_cast<int>(Number(edited, "st", 1)), end = static_cast<int>(Number(edited, "end", count));
    if (count <= 0) {
      edited.parent().remove_child(edited);
    } else {
      st = std::clamp(st, 1, count);
      end = std::clamp(end, st, count);
      edited.attribute("st").set_value(st);
      edited.attribute("end").set_value(end);
    }
  }
  return call.End({});
}

Result Editor::MoveSlides(const std::vector<size_t>& slides, size_t to) {
  Call call(*this, slides.size() > 1 ? u"Move Slides" : u"Move Slide");
  Impl& m = *impl_;
  std::vector<Impl::SlideEntry> list = m.SlideList();
  if (Result r = m.CheckSlides(slides, list.size()); !r) return call.End(r);
  std::vector<size_t> order = slides;
  std::sort(order.begin(), order.end());
  order.erase(std::unique(order.begin(), order.end()), order.end());
  if (to > list.size() - order.size()) return call.End(Result::Fail(u"That isn't a place in the presentation."));
  std::vector<size_t> rest;
  for (size_t i = 0; i < list.size(); ++i) {
    if (!std::binary_search(order.begin(), order.end(), i)) rest.push_back(i);
  }
  // Nothing moves when they're already there.
  std::vector<size_t> result(rest.begin(), rest.begin() + static_cast<ptrdiff_t>(to));
  result.insert(result.end(), order.begin(), order.end());
  result.insert(result.end(), rest.begin() + static_cast<ptrdiff_t>(to), rest.end());
  bool same = true;
  for (size_t i = 0; i < result.size(); ++i) same = same && result[i] == i;
  if (same) return call.End({});
  std::string pres = m.PresentationPart();
  pugi::xml_node root = m.Edit(pres);
  pugi::xml_node idList = SlideIdList(root);
  pugi::xml_node anchor = to < rest.size() ? SlideIdNode(root, list[rest[to]].rel) : pugi::xml_node();
  for (size_t i : order) {
    pugi::xml_node n = SlideIdNode(root, list[i].rel);
    if (!n) continue;
    anchor ? idList.insert_move_before(n, anchor) : idList.append_move(n);
  }
  // Sections: the moved slides join the section they land in, after the slide before them.
  if (pugi::xml_node sections = SectionList(root)) {
    uint32_t after = to > 0 ? list[rest[to - 1]].id : 0;
    pugi::xml_node target = after ? SectionOf(sections, after) : pugi::xml_node();
    if (!target) {
      // At the start: the first section.
      for (target = sections.first_child(); target && target.type() != pugi::node_element;) target = target.next_sibling();
      after = 0;
    }
    std::set<uint32_t> moved;
    for (size_t i : order) moved.insert(list[i].id);
    RemoveSlideRefs(m.registry, root, moved);
    if (target) {
      for (size_t i : order) {
        AddToSection(target, list[i].id, after);
        after = list[i].id;
      }
    }
  }
  return call.End({});
}

Result Editor::HideSlides(const std::vector<size_t>& slides, bool hidden) {
  Call call(*this, hidden ? u"Hide Slide" : u"Show Slide");
  Impl& m = *impl_;
  std::vector<Impl::SlideEntry> list = m.SlideList();
  if (Result r = m.CheckSlides(slides, list.size()); !r) return call.End(r);
  for (size_t i : slides) {
    pugi::xml_node root = m.Root(list[i].part);
    if (!root) continue;
    bool now = !Bool(root, "show", true);
    if (now == hidden) continue;
    root = m.Edit(list[i].part);
    if (hidden) {
      (root.attribute("show") ? root.attribute("show") : root.append_attribute("show")).set_value("0");
    } else {
      root.remove_attribute("show");
    }
  }
  return call.End({});
}

Result Editor::SetLayout(const std::vector<size_t>& slides, size_t layout) {
  Call call(*this, u"Layout");
  Impl& m = *impl_;
  std::vector<Impl::SlideEntry> list = m.SlideList();
  if (Result r = m.CheckSlides(slides, list.size()); !r) return call.End(r);
  if (layout >= deck_.Layouts().size()) return call.End(Result::Fail(u"That layout isn't in the presentation."));
  const Layout& l = *deck_.Layouts()[layout];
  pugi::xml_node layoutRoot = m.Root(l.part);
  if (!layoutRoot) return call.End(Result::Fail(u"That layout can't be read."));
  for (size_t i : slides) {
    const std::string& part = list[i].part;
    // The shapes' places now, for content whose placeholder the new layout lacks.
    std::map<uint32_t, draw::Rect> bounds;
    if (const Slide* s = deck_.GetSlide(i)) {
      for (const Shape& sh : s->shapes) bounds[sh.id] = sh.bounds;
    }
    pugi::xml_node root = m.Edit(part);
    if (!root) continue;
    pkg::RelationshipSet& rels = m.EditRels(part);
    bool found = false;
    for (pkg::Relationship& r : rels.items) {
      if (r.external || !r.type.ends_with("/slideLayout")) continue;
      if (found) continue;
      found = true;
      r.target = Impl::RelativeTarget(part, l.part);
      r.targetPart = l.part;
    }
    if (!found) {
      pkg::Relationship r;
      r.id = Impl::NewRelId(rels);
      r.type.assign(kRelLayout);
      r.target = Impl::RelativeTarget(part, l.part);
      r.targetPart = l.part;
      rels.items.push_back(r);
    }
    MapPlaceholders(root, layoutRoot, part, m, bounds);
  }
  return call.End({});
}

// ---- Notes

namespace {

// The notes slide part of a slide part, or empty.
std::string NotesPartOf(const Editor::Impl& m, std::string_view slidePart) {
  return TargetOfType(m.Rels(slidePart), "notesSlide");
}

// The body placeholder (ph type="body") of a notes part's shape tree.
pugi::xml_node NotesBody(pugi::xml_node root) {
  pugi::xml_node found;
  ForEachChosenChild(P(P(root, "cSld"), "spTree"), [&](pugi::xml_node c) {
    if (std::string_view(PlaceholderOf(c).attribute("type").value()) == "body") found = c;
    return !found;
  });
  return found;
}

}  // namespace

void SetPlainText(pugi::xml_node txBody, std::wstring_view text) {
  std::string a = PrefixFor(txBody, kA, "a");
  // The first paragraph's properties and its first run's carry over.
  pugi::xml_node firstP = A(txBody, "p");
  pugi::xml_document keep;
  pugi::xml_node pPr = keep.append_copy(A(firstP, "pPr"));
  pugi::xml_node rPr;
  for (pugi::xml_node r = firstP.first_child(); r && !rPr; r = r.next_sibling()) {
    if (LocalNameOf(r) == "r") rPr = A(r, "rPr");
  }
  if (!rPr) rPr = A(firstP, "endParaRPr");
  pugi::xml_node runProps = keep.append_copy(rPr);
  for (pugi::xml_node c = txBody.first_child(); c;) {
    pugi::xml_node next = c.next_sibling();
    if (Is(c, kA, "p")) txBody.remove_child(c);
    c = next;
  }
  size_t start = 0;
  do {
    size_t end = text.find(L'\n', start);
    std::wstring_view line = text.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start);
    if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
    pugi::xml_node p = txBody.append_child((a + ":p").c_str());
    if (pPr) p.append_copy(pPr);
    size_t i = 0;
    while (i <= line.size()) {
      size_t br = line.find(L'\v', i);
      std::wstring_view piece = line.substr(i, br == std::wstring_view::npos ? std::wstring_view::npos : br - i);
      if (!piece.empty()) {
        pugi::xml_node r = p.append_child((a + ":r").c_str());
        if (runProps) {
          pugi::xml_node rp = r.append_copy(runProps);
          rp.set_name((a + ":rPr").c_str());
        }
        pugi::xml_node t = r.append_child((a + ":t").c_str());
        std::string utf8 = Utf8(piece);
        t.text().set(utf8.c_str());
        if (!utf8.empty() && (utf8.front() == ' ' || utf8.back() == ' ')) {
          // pugixml keeps whitespace as written; nothing more is needed (a:t has no xml:space).
        }
      }
      if (br == std::wstring_view::npos) break;
      pugi::xml_node b = p.append_child((a + ":br").c_str());
      if (runProps) {
        pugi::xml_node rp = b.append_copy(runProps);
        rp.set_name((a + ":rPr").c_str());
      }
      i = br + 1;
    }
    pugi::xml_node endProps = p.append_child((a + ":endParaRPr").c_str());
    if (runProps) {
      for (pugi::xml_attribute at = runProps.first_attribute(); at; at = at.next_attribute()) {
        endProps.append_attribute(at.name()) = at.value();
      }
    } else {
      endProps.append_attribute("lang") = "en-US";
    }
    if (end == std::wstring_view::npos) break;
    start = end + 1;
  } while (true);
}

std::string EnsureNotesMaster(Editor::Impl& m) {
  std::string pres = m.PresentationPart();
  std::string master = TargetOfType(m.Rels(pres), "notesMaster");
  if (!master.empty() && m.Exists(master)) return master;
  // PowerPoint's default notes master, on a copy of the first slide master's theme.
  std::string slideMaster = TargetOfType(m.Rels(pres), "slideMaster");
  std::string slideTheme = slideMaster.empty() ? std::string() : TargetOfType(m.Rels(slideMaster), "theme");
  std::vector<uint8_t> theme;
  if (slideTheme.empty() || !m.Read(slideTheme, theme)) return {};
  std::shared_ptr<pkg::XmlDom> themeDom =
      ParseDom(std::string_view(reinterpret_cast<const char*>(theme.data()), theme.size()), m.deck.GetPackage().GetLimits(), "theme");
  if (!themeDom) return {};
  master = m.NewName("/ppt/notesMasters/notesMaster", ".xml");
  std::string themePart = m.NewName("/ppt/theme/theme", ".xml");
  std::string xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
      "<p:notesMaster xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
      "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
      "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"><p:cSld><p:bg><p:bgRef idx=\"1001\">"
      "<a:schemeClr val=\"bg1\"/></p:bgRef></p:bg><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/>"
      "<p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" "
      "y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr><p:sp><p:nvSpPr><p:cNvPr id=\"2\" "
      "name=\"Slide Image Placeholder 1\"/><p:cNvSpPr><a:spLocks noGrp=\"1\" noRot=\"1\" noChangeAspect=\"1\"/>"
      "</p:cNvSpPr><p:nvPr><p:ph type=\"sldImg\" idx=\"2\"/></p:nvPr></p:nvSpPr><p:spPr><a:xfrm><a:off x=\"685800\" "
      "y=\"1143000\"/><a:ext cx=\"5486400\" cy=\"3086100\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>"
      "<a:noFill/><a:ln w=\"12700\"><a:solidFill><a:prstClr val=\"black\"/></a:solidFill></a:ln></p:spPr></p:sp><p:sp>"
      "<p:nvSpPr><p:cNvPr id=\"3\" name=\"Notes Placeholder 2\"/><p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr><p:nvPr>"
      "<p:ph type=\"body\" sz=\"quarter\" idx=\"3\"/></p:nvPr></p:nvSpPr><p:spPr><a:xfrm><a:off x=\"685800\" "
      "y=\"4400550\"/><a:ext cx=\"5486400\" cy=\"3600450\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/>"
      "</a:prstGeom></p:spPr><p:txBody><a:bodyPr vert=\"horz\" lIns=\"91440\" tIns=\"45720\" rIns=\"91440\" "
      "bIns=\"45720\" rtlCol=\"0\"/><a:lstStyle/><a:p><a:pPr lvl=\"0\"/><a:r><a:rPr lang=\"en-US\"/><a:t>Click to edit "
      "Master text styles</a:t></a:r></a:p></p:txBody></p:sp></p:spTree></p:cSld><p:clrMap bg1=\"lt1\" tx1=\"dk1\" "
      "bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" accent4=\"accent4\" "
      "accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/><p:notesStyle><a:lvl1pPr marL=\"0\" "
      "algn=\"l\" defTabSz=\"914400\" rtl=\"0\" eaLnBrk=\"1\" latinLnBrk=\"0\" hangingPunct=\"1\"><a:defRPr sz=\"1200\" "
      "kern=\"1200\"><a:solidFill><a:schemeClr val=\"tx1\"/></a:solidFill><a:latin typeface=\"+mn-lt\"/><a:ea "
      "typeface=\"+mn-ea\"/><a:cs typeface=\"+mn-cs\"/></a:defRPr></a:lvl1pPr></p:notesStyle></p:notesMaster>";
  std::shared_ptr<pkg::XmlDom> dom = ParseDom(xml, m.deck.GetPackage().GetLimits(), master);
  if (!dom) return {};
  m.PutXml(themePart, themeDom, kTypeTheme);
  // Its relationships come along (a picture fill names one): the same parts, shared within the deck.
  if (const pkg::RelationshipSet* themeRels = m.Rels(slideTheme); themeRels && !themeRels->items.empty()) {
    std::vector<pkg::Relationship> items = themeRels->items;
    for (pkg::Relationship& r : items) {
      if (!r.external) r.target = Editor::Impl::RelativeTarget(themePart, r.targetPart);
    }
    m.EditRels(themePart).items = std::move(items);
  }
  m.PutXml(master, dom, kTypeNotesMaster);
  m.AddRel(master, kRelTheme, themePart);
  std::string rel = m.AddRel(pres, kRelNotesMaster, master);
  pugi::xml_node root = m.Edit(pres);
  pugi::xml_node list = DirectChild(root, kP, "notesMasterIdLst");
  if (list) {
    for (pugi::xml_node c = list.first_child(); c;) {
      pugi::xml_node next = c.next_sibling();
      list.remove_child(c);
      c = next;
    }
  } else {
    std::string p = PrefixFor(root, kP, "p");
    list = AddChildOrdered(root, p + ":notesMasterIdLst",
                           {"handoutMasterIdLst", "sldIdLst", "sldSz", "notesSz", "smartTags", "embeddedFontLst",
                            "custShowLst", "photoAlbum", "custDataLst", "kinsoku", "defaultTextStyle", "modifyVerifier",
                            "extLst"});
  }
  std::string p = PrefixFor(list, kP, "p"), r = PrefixFor(list, kRelNs, "r");
  list.append_child((p + ":notesMasterId").c_str()).append_attribute((r + ":id").c_str()) = rel.c_str();
  m.Full();
  return master;
}

std::string MakeNotesSlide(Editor::Impl& m, std::string_view slidePart) {
  std::string master = EnsureNotesMaster(m);
  if (master.empty()) return {};
  std::string part = m.NewName("/ppt/notesSlides/notesSlide", ".xml");
  std::string xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
      "<p:notes xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
      "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
      "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"><p:cSld><p:spTree><p:nvGrpSpPr>"
      "<p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/>"
      "<a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>"
      "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Slide Image Placeholder 1\"/><p:cNvSpPr><a:spLocks noGrp=\"1\" "
      "noRot=\"1\" noChangeAspect=\"1\"/></p:cNvSpPr><p:nvPr><p:ph type=\"sldImg\"/></p:nvPr></p:nvSpPr><p:spPr/></p:sp>"
      "<p:sp><p:nvSpPr><p:cNvPr id=\"3\" name=\"Notes Placeholder 2\"/><p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr>"
      "<p:nvPr><p:ph type=\"body\" idx=\"1\"/></p:nvPr></p:nvSpPr><p:spPr/><p:txBody><a:bodyPr/><a:lstStyle/><a:p>"
      "<a:endParaRPr lang=\"en-US\"/></a:p></p:txBody></p:sp></p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/>"
      "</p:clrMapOvr></p:notes>";
  std::shared_ptr<pkg::XmlDom> dom = ParseDom(xml, m.deck.GetPackage().GetLimits(), part);
  m.PutXml(part, dom, kTypeNotes);
  m.AddRel(part, kRelNotesMaster, master);
  m.AddRel(part, kRelSlide, slidePart);
  m.AddRel(slidePart, kRelNotes, part);
  return part;
}

Result Editor::SetNotesText(size_t slide, std::wstring_view text) {
  const std::wstring clean = XmlText(text, true);  // what XML can't hold dropped
  text = clean;
  Call call(*this, u"Edit Notes");
  Impl& m = *impl_;
  std::string part = m.SlidePart(slide);
  if (part.empty()) return call.End(Result::Fail(u"That slide isn't in the presentation."));
  std::string notes = NotesPartOf(m, part);
  if (notes.empty() || !m.Exists(notes)) {
    if (text.empty()) return call.End({});
    notes = MakeNotesSlide(m, part);
    if (notes.empty()) {
      return call.End(Result::Fail(u"Notes can't be added to this presentation, because its theme can't be read."));
    }
  }
  pugi::xml_node root = m.Edit(notes);
  pugi::xml_node body = NotesBody(root);
  if (!body) return call.End(Result::Fail(u"This slide's notes page has no place for notes."));
  body = Impl::Unwrap(body);
  pugi::xml_node txBody = P(body, "txBody");
  if (!txBody) {
    std::string p = PrefixFor(body, kP, "p"), a = PrefixFor(body, kA, "a");
    txBody = AddChildOrdered(body, p + ":txBody", {"extLst"});
    txBody.append_child((a + ":bodyPr").c_str());
    txBody.append_child((a + ":lstStyle").c_str());
  }
  if (HoldsMath(txBody)) return call.End(Result::Fail(u"These notes hold an equation, which Featherweight can't edit yet."));
  SetPlainText(txBody, text);
  return call.End({});
}

uint32_t Editor::NotesShape(size_t slide) {
  Impl& m = *impl_;
  std::string part = m.SlidePart(slide);
  std::string notes = part.empty() ? std::string() : NotesPartOf(m, part);
  pugi::xml_node body = NotesBody(m.Root(notes));
  pugi::xml_node cNvPr;
  for (pugi::xml_node n = body.first_child(); n && !cNvPr; n = n.next_sibling()) cNvPr = P(n, "cNvPr");
  return U32(cNvPr, "id");
}

}  // namespace fw::slide::edit
