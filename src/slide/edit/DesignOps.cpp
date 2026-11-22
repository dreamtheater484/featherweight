// Design operations (P2, p2-model; for p2-slides): slide size, backgrounds, themes, header and
// footer, copying slides between decks.
#include <algorithm>
#include <cmath>

#include "slide/edit/EditInternal.h"
#include "slide/edit/SlideCopy.h"

namespace fw::slide::edit {

namespace {

constexpr double kEmu = 12700.0;

void SetAttr(pugi::xml_node n, const char* name, long long v) {
  (n.attribute(name) ? n.attribute(name) : n.append_attribute(name)).set_value(v);
}

// A part's top-level shapes scaled by `s` about the origin and moved by (dx, dy), EMU.
void ScaleShapes(pugi::xml_node root, double s, double dx, double dy) {
  pugi::xml_node tree = P(P(root, "cSld"), "spTree");
  std::vector<pugi::xml_node> xfrms;
  for (pugi::xml_node c = tree.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    std::vector<pugi::xml_node> shapes;
    if (Is(c, pkg::mce::kNamespace, "AlternateContent")) {
      for (pugi::xml_node b = c.first_child(); b; b = b.next_sibling()) {
        for (pugi::xml_node k = b.first_child(); k; k = k.next_sibling()) shapes.push_back(k);
      }
    } else {
      shapes.push_back(c);
    }
    for (pugi::xml_node shape : shapes) {
      std::string_view k = LocalNameOf(shape);
      pugi::xml_node xfrm;
      if (k == "graphicFrame") xfrm = P(shape, "xfrm");
      else if (k == "grpSp") xfrm = A(P(shape, "grpSpPr"), "xfrm");
      else if (k == "sp" || k == "pic" || k == "cxnSp") xfrm = A(P(shape, "spPr"), "xfrm");
      if (!xfrm) continue;
      pugi::xml_node off = A(xfrm, "off"), ext = A(xfrm, "ext");
      SetAttr(off, "x", std::llround(Number(off, "x", 0) * s + dx));
      SetAttr(off, "y", std::llround(Number(off, "y", 0) * s + dy));
      SetAttr(ext, "cx", std::llround(Number(ext, "cx", 0) * s));
      SetAttr(ext, "cy", std::llround(Number(ext, "cy", 0) * s));
      if (k == "graphicFrame") {
        pugi::xml_node tbl = A(A(A(shape, "graphic"), "graphicData"), "tbl");
        for (pugi::xml_node g = A(tbl, "tblGrid").first_child(); g; g = g.next_sibling()) {
          if (LocalNameOf(g) == "gridCol") SetAttr(g, "w", std::llround(Number(g, "w", 0) * s));
        }
        for (pugi::xml_node r = tbl.first_child(); r; r = r.next_sibling()) {
          if (LocalNameOf(r) == "tr") SetAttr(r, "h", std::llround(Number(r, "h", 0) * s));
        }
      }
    }
  }
}

// Replaces the p:bg of a part's p:cSld with `xml` (empty: none).
bool PutBackground(pugi::xml_node root, std::string_view xml) {
  pugi::xml_node cSld = P(root, "cSld");
  if (!cSld) return false;
  if (pugi::xml_node bg = DirectChild(cSld, kP, "bg")) cSld.remove_child(bg);
  if (xml.empty()) return true;
  pugi::xml_document doc;
  if (!ParseFragment(xml, doc) || !Is(doc.document_element(), kP, "bg")) return false;
  InsertCopy(cSld, doc.document_element(), cSld.first_child());
  return true;
}

}  // namespace

Result Editor::SetSlideSize(draw::Size size, Scale scale) {
  Call call(*this, u"Slide Size");
  Impl& m = *impl_;
  long long cx = std::llround(size.width * kEmu), cy = std::llround(size.height * kEmu);
  if (cx < 914400 || cx > 51206400 || cy < 914400 || cy > 51206400) {
    return call.End(Result::Fail(u"Slides are 1 to 56 inches wide and high."));
  }
  std::string pres = m.PresentationPart();
  pugi::xml_node root = m.Edit(pres);
  pugi::xml_node sz = DirectChild(root, kP, "sldSz");
  if (!sz) {
    std::string p = PrefixFor(root, kP, "p");
    sz = AddChildOrdered(root, p + ":sldSz", {"notesSz", "smartTags", "embeddedFontLst", "custShowLst", "photoAlbum",
                                              "custDataLst", "kinsoku", "defaultTextStyle", "modifyVerifier", "extLst"});
  }
  double oldW = Number(sz, "cx", 9144000), oldH = Number(sz, "cy", 6858000);
  SetAttr(sz, "cx", cx);
  SetAttr(sz, "cy", cy);
  if (cx == 9144000 && cy == 6858000) {
    (sz.attribute("type") ? sz.attribute("type") : sz.append_attribute("type")).set_value("screen4x3");
  } else {
    sz.remove_attribute("type");
  }
  if (scale != Scale::DontScale && oldW > 0 && oldH > 0) {
    double sx = cx / oldW, sy = cy / oldH;
    double s = scale == Scale::EnsureFit ? std::min(sx, sy) : std::max(sx, sy);
    double dx = (cx - oldW * s) / 2, dy = (cy - oldH * s) / 2;
    std::vector<std::string> parts;
    for (const auto& ma : deck_.Masters()) parts.push_back(ma->part);
    for (const auto& l : deck_.Layouts()) parts.push_back(l->part);
    for (const Impl::SlideEntry& e : m.SlideList()) parts.push_back(e.part);
    for (const std::string& part : parts) {
      if (pugi::xml_node r = m.Edit(part)) ScaleShapes(r, s, dx, dy);
    }
  }
  m.Full();
  return call.End({});
}

Result Editor::SetBackground(const std::vector<size_t>& slides, std::string_view bgXml) {
  Call call(*this, u"Format Background");
  Impl& m = *impl_;
  std::vector<Impl::SlideEntry> list = m.SlideList();
  if (Result r = m.CheckSlides(slides, list.size()); !r) return call.End(r);
  for (size_t i : slides) {
    if (!PutBackground(m.Edit(list[i].part), bgXml)) return call.End(Result::Fail(u"That background can't be written."));
  }
  return call.End({});
}

Result Editor::SetMasterBackground(size_t master, std::string_view bgXml) {
  Call call(*this, u"Format Background");
  Impl& m = *impl_;
  if (master >= deck_.Masters().size()) return call.End(Result::Fail(u"That slide master isn't in the presentation."));
  if (!PutBackground(m.Edit(deck_.Masters()[master]->part), bgXml)) {
    return call.End(Result::Fail(u"That background can't be written."));
  }
  m.Full();
  return call.End({});
}

Result Editor::SetTheme(size_t master, std::string_view themeXml) {
  Call call(*this, u"Theme");
  Impl& m = *impl_;
  if (master >= deck_.Masters().size()) return call.End(Result::Fail(u"That slide master isn't in the presentation."));
  const std::string& part = deck_.Masters()[master]->part;
  std::shared_ptr<pkg::XmlDom> dom = ParseDom(themeXml, deck_.GetPackage().GetLimits(), "theme");
  if (!dom || LocalNameOf(dom->Root()) != "theme") return call.End(Result::Fail(u"That isn't a theme."));
  std::string theme = TargetOfType(m.Rels(part), "theme");
  if (theme.empty()) {
    theme = m.NewName("/ppt/theme/theme", ".xml");
    m.AddRel(part, kRelTheme, theme);
  }
  m.PutXml(theme, dom, kTypeTheme);
  m.Full();
  return call.End({});
}

Result Editor::SetHeaderFooter(const std::vector<size_t>& slides, const HeaderFooter& hf) {
  (void)slides;
  (void)hf;
  return Result::Fail(u"Header and footer can't be changed yet.");
}

Result Editor::InsertSlides(Presentation& source, const std::vector<size_t>& slides, size_t at, bool keepSourceDesign,
                            std::vector<uint32_t>* ids) {
  if (keepSourceDesign) return Result::Fail(u"Keeping the source formatting isn't available yet.");
  Call call(*this, u"Reuse Slides");
  Impl& m = *impl_;
  std::vector<Impl::SlideEntry> list = m.SlideList();
  if (at > list.size()) return call.End(Result::Fail(u"That isn't a place in the presentation."));
  if (deck_.Layouts().empty()) return call.End(Result::Fail(u"This presentation has no layouts to put slides on."));
  if (ids) ids->clear();
  std::string pres = m.PresentationPart();
  size_t pos = at;
  for (size_t index : slides) {
    const Slide* s = source.GetSlide(index);
    if (!s || index >= source.SlideCount()) return call.End(Result::Fail(u"That slide isn't in the other presentation."));
    // The layout here with the same type and name, else the same type, else the first.
    const Layout* target = nullptr;
    std::string type = s->layout ? s->layout->type : std::string("obj");
    std::string name = s->layout ? s->layout->name : std::string();
    for (const auto& l : deck_.Layouts()) {
      if (!target && l->type == type && l->name == name) target = l.get();
    }
    for (const auto& l : deck_.Layouts()) {
      if (!target && l->type == type) target = l.get();
    }
    if (!target) target = deck_.Layouts().front().get();
    SlideCopier copier(m, &source);
    copier.layoutPart = target->part;
    std::string part = copier.CopySlide(s->part);
    if (part.empty()) return call.End(Result::Fail(u"The slide can't be copied, because a part of it can't be read."));
    std::map<uint32_t, draw::Rect> bounds;
    for (const Shape& sh : s->shapes) bounds[sh.id] = sh.bounds;
    MapPlaceholders(m.Edit(part), m.Root(target->part), part, m, bounds);
    std::string rel = m.AddRel(pres, kRelSlide, part);
    pugi::xml_node root = m.Edit(pres);
    uint32_t newId = 0;
    {
      uint32_t max = 255;
      for (pugi::xml_node n = P(root, "sldIdLst").first_child(); n; n = n.next_sibling()) {
        if (n.type() == pugi::node_element) max = std::max(max, static_cast<uint32_t>(Number(n, "id", 0)));
      }
      newId = max + 1;
    }
    // Into the slide list at `pos` (sections: the slide before's).
    std::vector<Impl::SlideEntry> now = m.SlideList();
    pugi::xml_node listNode = DirectChild(root, kP, "sldIdLst");
    pugi::xml_node before = pos < now.size() ? pugi::xml_node() : pugi::xml_node();
    size_t i = 0;
    for (pugi::xml_node n = listNode.first_child(); n; n = n.next_sibling()) {
      if (!Is(n, kP, "sldId") || TargetOf(m.Rels(pres), RelId(n, "id")).empty()) continue;
      if (i++ == pos) {
        before = n;
        break;
      }
    }
    std::string p = PrefixFor(listNode, kP, "p"), r = PrefixFor(listNode, kRelNs, "r");
    pugi::xml_node n = before ? listNode.insert_child_before((p + ":sldId").c_str(), before)
                              : listNode.append_child((p + ":sldId").c_str());
    n.append_attribute("id") = newId;
    n.append_attribute((r + ":id").c_str()) = rel.c_str();
    if (ids) ids->push_back(newId);
    ++pos;
  }
  return call.End({});
}

// ---- Tables (p2-objects): to come.

Result Editor::InsertTableRows(size_t, uint32_t, int, int) { return Result::Fail(u"Not available yet."); }
Result Editor::DeleteTableRows(size_t, uint32_t, int, int) { return Result::Fail(u"Not available yet."); }
Result Editor::InsertTableColumns(size_t, uint32_t, int, int) { return Result::Fail(u"Not available yet."); }
Result Editor::DeleteTableColumns(size_t, uint32_t, int, int) { return Result::Fail(u"Not available yet."); }
Result Editor::MergeTableCells(size_t, uint32_t, int, int, int, int) { return Result::Fail(u"Not available yet."); }
Result Editor::SplitTableCell(size_t, uint32_t, int, int) { return Result::Fail(u"Not available yet."); }
Result Editor::SetTableColumnWidth(size_t, uint32_t, int, float) { return Result::Fail(u"Not available yet."); }
Result Editor::SetTableRowHeight(size_t, uint32_t, int, float) { return Result::Fail(u"Not available yet."); }
Result Editor::SetCellProps(size_t, uint32_t, int, int, int, int, const CellPatch&) {
  return Result::Fail(u"Not available yet.");
}

}  // namespace fw::slide::edit
