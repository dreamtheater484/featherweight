// Shape trees: placeholders matched along slide → layout → master, what they inherit merged into a
// shape fwdraw can draw (plan §8.3), backgrounds, and hyperlinks.
#include <algorithm>
#include <cstring>

#include "slide/Internal.h"

namespace fw::slide {

using namespace detail;

namespace {

constexpr int kMaxShapes = 20000;  // per part, as fwdraw's ReadContext
constexpr int kMaxDepth = 32;

PlaceholderType TypeOf(std::string_view t) {
  if (t.empty() || t == "obj") return PlaceholderType::Object;
  if (t == "title") return PlaceholderType::Title;
  if (t == "ctrTitle") return PlaceholderType::CenteredTitle;
  if (t == "subTitle") return PlaceholderType::Subtitle;
  if (t == "body") return PlaceholderType::Body;
  if (t == "chart") return PlaceholderType::Chart;
  if (t == "tbl") return PlaceholderType::Table;
  if (t == "clipArt") return PlaceholderType::ClipArt;
  if (t == "dgm") return PlaceholderType::Diagram;
  if (t == "media") return PlaceholderType::Media;
  if (t == "pic") return PlaceholderType::Picture;
  if (t == "sldImg") return PlaceholderType::SlideImage;
  if (t == "dt") return PlaceholderType::Date;
  if (t == "ftr") return PlaceholderType::Footer;
  if (t == "hdr") return PlaceholderType::Header;
  if (t == "sldNum") return PlaceholderType::SlideNumber;
  return PlaceholderType::Object;
}

// Types that can stand for each other when placeholders are matched.
PlaceholderType Family(PlaceholderType t) {
  switch (t) {
    case PlaceholderType::Title:
    case PlaceholderType::CenteredTitle:
      return PlaceholderType::Title;
    case PlaceholderType::Body:
    case PlaceholderType::Subtitle:
    case PlaceholderType::Object:
    case PlaceholderType::Chart:
    case PlaceholderType::Table:
    case PlaceholderType::ClipArt:
    case PlaceholderType::Diagram:
    case PlaceholderType::Media:
    case PlaceholderType::Picture:
      return PlaceholderType::Body;
    default:
      return t;
  }
}

// The non-visual properties element of a shape element (p:nvSpPr, p:nvPicPr…).
pugi::xml_node NonVisual(pugi::xml_node el) {
  pugi::xml_node found;
  ForEachChild(el, [&](pugi::xml_node c) {
    std::string_view k = LocalNameOf(c);
    if (k.size() > 4 && k.starts_with("nv") && k.ends_with("Pr")) found = c;
    return !found;
  });
  return found;
}

}  // namespace

namespace detail {

// Namespace declarations in scope at `src` that `copy` (a copy of it elsewhere) lacks, added to it,
// so the copy's prefixes mean what they meant in its own part.
void DeclareScope(pugi::xml_node copy, pugi::xml_node src) {
  for (pugi::xml_node n = src.parent(); n && n.type() == pugi::node_element; n = n.parent()) {
    for (pugi::xml_attribute a = n.first_attribute(); a; a = a.next_attribute()) {
      std::string_view name = a.name();
      if (name != "xmlns" && !name.starts_with("xmlns:")) continue;
      if (!copy.attribute(a.name())) copy.append_attribute(a.name()) = a.value();
    }
  }
}

pugi::xml_node CopyInto(pugi::xml_node parent, pugi::xml_node src) {
  pugi::xml_node c = parent.append_copy(src);
  DeclareScope(c, src);
  return c;
}

}  // namespace detail

namespace {

bool IsFill(std::string_view k) {
  return k == "noFill" || k == "solidFill" || k == "gradFill" || k == "blipFill" || k == "pattFill" || k == "grpFill";
}

// spPr's child groups in schema order; each group is inherited whole.
int SpPrGroup(std::string_view k) {
  if (k == "xfrm") return 0;
  if (k == "custGeom" || k == "prstGeom") return 1;
  if (IsFill(k)) return 2;
  if (k == "ln") return 3;
  if (k == "effectLst" || k == "effectDag") return 4;
  if (k == "scene3d") return 5;
  if (k == "sp3d") return 6;
  return -1;
}

int BodyPrGroup(std::string_view k) {
  if (k == "prstTxWarp") return 0;
  if (k == "noAutofit" || k == "normAutofit" || k == "spAutoFit") return 1;
  if (k == "scene3d") return 2;
  if (k == "sp3d" || k == "flatTx") return 3;
  return -1;
}

// A placeholder's drawNode: `el` with what `base` (its parent placeholder's drawNode) gives it.
pugi::xml_node MergePlaceholder(pugi::xml_document& scratch, pugi::xml_node el, pugi::xml_node base) {
  pugi::xml_node out = scratch.append_child(el.name());
  DeclareScope(out, el.first_child() ? el.first_child() : el);
  for (pugi::xml_attribute a = el.first_attribute(); a; a = a.next_attribute()) {
    if (!out.attribute(a.name())) out.append_attribute(a.name()) = a.value();
  }
  pugi::xml_node spPr = P(el, "spPr"), baseSpPr = P(base, "spPr");
  pugi::xml_node style = P(el, "style"), baseStyle = P(base, "style");
  pugi::xml_node txBody = P(el, "txBody"), baseBody = P(base, "txBody");
  bool wroteSpPr = false, wroteStyle = false, wroteBody = false;
  // The shape's own p:style gives its fill, line and effects ahead of what the placeholder above
  // it says (PowerPoint: a slide title's fillRef over the master's noFill).
  bool styled[7] = {};
  styled[2] = static_cast<bool>(A(style, "fillRef"));
  styled[3] = static_cast<bool>(A(style, "lnRef"));
  styled[4] = static_cast<bool>(A(style, "effectRef"));
  auto writeSpPr = [&] {
    wroteSpPr = true;
    pugi::xml_node dst = spPr ? CopyInto(out, spPr) : CopyInto(out, baseSpPr);
    if (!spPr || !baseSpPr) {
      if (!spPr) {
        // Inherited whole; a picture fill would point at the other part's relationships.
        for (pugi::xml_node c = dst.first_child(); c;) {
          pugi::xml_node next = c.next_sibling();
          const int g = SpPrGroup(LocalNameOf(c));
          if (LocalNameOf(c) == "blipFill" || (g >= 0 && styled[g])) dst.remove_child(c);
          c = next;
        }
      }
      return;
    }
    // Groups the shape lacks come from the base, in schema order.
    bool have[7] = {};
    for (int g = 0; g < 7; ++g) have[g] = styled[g];
    for (pugi::xml_node c = spPr.first_child(); c; c = c.next_sibling()) {
      int g = SpPrGroup(LocalNameOf(c));
      if (g >= 0) have[g] = true;
    }
    for (pugi::xml_node c = baseSpPr.first_child(); c; c = c.next_sibling()) {
      std::string_view k = LocalNameOf(c);
      int g = SpPrGroup(k);
      if (g < 0 || have[g] || k == "blipFill") continue;
      // Insert before the first child of a later group (or extLst), else at the end.
      pugi::xml_node before;
      for (pugi::xml_node d = dst.first_child(); d && !before; d = d.next_sibling()) {
        if (d.type() != pugi::node_element) continue;
        int dg = SpPrGroup(LocalNameOf(d));
        if (dg > g || dg < 0) before = d;
      }
      pugi::xml_node copy = before ? dst.insert_copy_before(c, before) : dst.append_copy(c);
      DeclareScope(copy, c);
      have[g] = true;
    }
  };
  auto writeBody = [&] {
    wroteBody = true;
    pugi::xml_node body = out.append_child(txBody ? txBody.name() : baseBody.name());
    DeclareScope(body, txBody ? txBody : baseBody);
    pugi::xml_node bodyPr = A(txBody, "bodyPr"), basePr = A(baseBody, "bodyPr");
    pugi::xml_node src = bodyPr ? bodyPr : basePr;
    if (!src) return;
    pugi::xml_node dst = body.append_child(src.name());
    // Attributes: the base's, each overridden by the shape's.
    for (pugi::xml_attribute a = basePr.first_attribute(); a; a = a.next_attribute()) {
      dst.append_attribute(a.name()) = a.value();
    }
    for (pugi::xml_attribute a = bodyPr.first_attribute(); a; a = a.next_attribute()) {
      if (pugi::xml_attribute have = dst.attribute(a.name())) {
        have.set_value(a.value());
      } else {
        dst.append_attribute(a.name()) = a.value();
      }
    }
    bool have[4] = {};
    for (pugi::xml_node c = bodyPr.first_child(); c; c = c.next_sibling()) {
      int g = BodyPrGroup(LocalNameOf(c));
      if (g >= 0) have[g] = true;
    }
    for (int g = 0; g < 4; ++g) {
      pugi::xml_node from = have[g] ? bodyPr : basePr;
      for (pugi::xml_node c = from.first_child(); c; c = c.next_sibling()) {
        if (BodyPrGroup(LocalNameOf(c)) == g) DeclareScope(dst.append_copy(c), c);
      }
    }
  };
  for (pugi::xml_node c = el.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    std::string_view k = LocalNameOf(c);
    // Schema order: non-visual properties (and a picture's blipFill), spPr, style, txBody, extLst.
    bool late = k == "style" || k == "txBody" || k == "extLst";
    if (late && !wroteSpPr && baseSpPr) writeSpPr();
    if ((k == "txBody" || k == "extLst") && !wroteStyle && baseStyle) {
      wroteStyle = true;
      CopyInto(out, baseStyle);
    }
    if (k == "spPr") {
      writeSpPr();
    } else if (k == "style") {
      wroteStyle = true;
      CopyInto(out, c);
    } else if (k == "txBody") {
      writeBody();
    } else {
      CopyInto(out, c);
    }
  }
  if (!wroteSpPr && baseSpPr) writeSpPr();
  if (!wroteStyle && baseStyle) CopyInto(out, baseStyle);
  if (!wroteBody && txBody) writeBody();
  return out;
}

class TreeReader {
 public:
  TreeReader(SlideBase& part, Presentation& deck) : part_(part), deck_(deck) {}

  void Read(pugi::xml_node spTree, std::vector<Shape>& out, int depth) {
    ForEachChild(spTree, [&](pugi::xml_node c) {
      std::string_view k = LocalNameOf(c);
      if (k != "sp" && k != "pic" && k != "cxnSp" && k != "grpSp" && k != "graphicFrame" && k != "contentPart") {
        return true;
      }
      if (++count_ > kMaxShapes) return false;
      Shape s;
      ReadOne(c, s, depth);
      out.push_back(std::move(s));
      return true;
    });
  }

 private:
  void ReadOne(pugi::xml_node el, Shape& s, int depth) {
    std::string_view k = LocalNameOf(el);
    s.node = el;
    s.drawNode = el;
    s.kind = k == "pic"            ? Shape::Kind::Picture
             : k == "cxnSp"        ? Shape::Kind::Connector
             : k == "grpSp"        ? Shape::Kind::Group
             : k == "contentPart"  ? Shape::Kind::ContentPart
             : k == "graphicFrame" ? Shape::Kind::Graphic
                                   : Shape::Kind::Shape;
    pugi::xml_node nv = NonVisual(el);
    pugi::xml_node cNvPr = P(nv, "cNvPr");
    double id = Number(cNvPr, "id", 0);
    s.id = id >= 0 && id <= 4294967295.0 ? static_cast<uint32_t>(id) : 0;
    s.name = cNvPr.attribute("name").value();
    s.description = cNvPr.attribute("descr").value();
    s.hidden = Bool(cNvPr, "hidden", false);
    s.click = ReadHyperlink(A(cNvPr, "hlinkClick"), part_, deck_);
    s.hover = ReadHyperlink(A(cNvPr, "hlinkHover"), part_, deck_);
    s.textBox = Bool(P(nv, "cNvSpPr"), "txBox", false);
    pugi::xml_node nvPr = P(nv, "nvPr");
    s.placeholder = ReadPlaceholder(el);
    ForEachChild(nvPr, [&](pugi::xml_node m) {
      std::string_view mk = LocalNameOf(m);
      if (mk == "videoFile" || mk == "audioFile" || mk == "quickTimeFile" || mk == "wavAudioFile" || mk == "audioCd") {
        s.media = true;
      }
      if (mk == "extLst") {
        ForEachChild(m, [&](pugi::xml_node ext) {
          if (Child(ext, kP14, "media")) s.media = true;
          return !s.media;
        });
      }
      return !s.media;
    });

    if (s.kind == Shape::Kind::Graphic) {
      pugi::xml_node data = A(A(el, "graphic"), "graphicData");
      std::string_view uri = data.attribute("uri").value();
      if (uri == kTableUri) {
        s.kind = Shape::Kind::Table;
      } else if (uri == kChartUri) {
        s.kind = Shape::Kind::Chart;
      } else if (uri == kDiagramUri) {
        s.kind = Shape::Kind::Diagram;
      } else if (uri == kOleUri) {
        s.kind = Shape::Kind::Ole;
      }
    }

    // What a placeholder inherits.
    if (s.placeholder && part_.parent) {
      s.inherits = part_.parent->FindPlaceholder(s.placeholder);
      if (!s.inherits && part_.parent->parent) s.inherits = part_.parent->parent->FindPlaceholder(s.placeholder);
      if (s.inherits && (k == "sp" || k == "pic" || k == "cxnSp")) {
        s.drawNode = MergePlaceholder(part_.scratch, el, s.inherits->drawNode);
      }
    }
    if (k == "sp") {
      pugi::xml_node own = P(el, "txBody");
      if (own) s.textNode = s.drawNode == el ? own : P(s.drawNode, "txBody");
    }

    pugi::xml_node xfrm = k == "graphicFrame" ? P(el, "xfrm")
                          : k == "grpSp"      ? A(P(el, "grpSpPr"), "xfrm")
                                              : A(P(s.drawNode, "spPr"), "xfrm");
    if (k == "contentPart") xfrm = A(Child(P(el, "xfrm").parent(), kP14, "xfrm"), "xfrm");
    ReadXfrm(xfrm, s);

    if (s.kind == Shape::Kind::Group && depth < kMaxDepth) Read(el, s.children, depth + 1);
  }

  static void ReadXfrm(pugi::xml_node xfrm, Shape& s) {
    if (!xfrm) return;
    pugi::xml_node off = A(xfrm, "off"), ext = A(xfrm, "ext");
    float x = Emu(off, "x"), y = Emu(off, "y");
    float w = std::max(0.f, Emu(ext, "cx")), h = std::max(0.f, Emu(ext, "cy"));
    s.bounds = {x, y, x + w, y + h};
    s.rotation = static_cast<float>(Number(xfrm, "rot", 0) / 60000.0);
    s.flipH = Bool(xfrm, "flipH", false);
    s.flipV = Bool(xfrm, "flipV", false);
  }

  SlideBase& part_;
  Presentation& deck_;
  int count_ = 0;
};

const Shape* FindIn(const std::vector<Shape>& shapes, const Placeholder& ph, int pass) {
  for (const Shape& s : shapes) {
    if (s.placeholder) {
      const Placeholder& o = s.placeholder;
      bool match = false;
      switch (pass) {
        case 0:  // the same index, of a type that can stand for it
          match = ph.hasIndex && o.hasIndex && o.index == ph.index && Family(o.type) == Family(ph.type);
          break;
        case 1:  // the same type
          match = o.type == ph.type;
          break;
        default:  // a type that can stand for it
          match = Family(o.type) == Family(ph.type);
          break;
      }
      if (match) return &s;
    }
    if (!s.children.empty()) {
      if (const Shape* f = FindIn(s.children, ph, pass)) return f;
    }
  }
  return nullptr;
}

const Shape* FindType(const std::vector<Shape>& shapes, PlaceholderType type) {
  for (const Shape& s : shapes) {
    if (s.placeholder.type == type) return &s;
    if (const Shape* f = FindType(s.children, type)) return f;
  }
  return nullptr;
}

}  // namespace

const Shape* SlideBase::FindPlaceholder(const Placeholder& ph) const {
  if (!ph) return nullptr;
  for (int pass = 0; pass < 3; ++pass) {
    if (const Shape* s = FindIn(shapes, ph, pass)) return s;
  }
  return nullptr;
}

const Shape* SlideBase::FindPlaceholder(PlaceholderType type) const { return FindType(shapes, type); }

namespace detail {

Placeholder ReadPlaceholder(pugi::xml_node shape) {
  Placeholder ph;
  pugi::xml_node el = P(P(NonVisual(shape), "nvPr"), "ph");
  if (!el) return ph;
  ph.type = TypeOf(el.attribute("type").value());
  if (pugi::xml_attribute idx = el.attribute("idx")) {
    double d = Number(idx.value(), -1);
    if (d >= 0 && d <= 4294967295.0) {
      ph.index = static_cast<uint32_t>(d);
      ph.hasIndex = true;
    }
  }
  std::string_view sz = el.attribute("sz").value();
  ph.size = sz == "half" ? Placeholder::Size::Half : sz == "quarter" ? Placeholder::Size::Quarter : Placeholder::Size::Full;
  ph.vertical = std::string_view(el.attribute("orient").value()) == "vert";
  ph.customPrompt = Bool(el, "hasCustomPrompt", false);
  return ph;
}

void ReadShapeTree(SlideBase& part, pugi::xml_node spTree, Presentation& deck) {
  if (!spTree) return;
  TreeReader(part, deck).Read(spTree, part.shapes, 0);
}

void ResolveBackground(SlideBase& part, draw::Size size) {
  const SlideBase* owner = nullptr;
  pugi::xml_node bg;
  for (const SlideBase* p = &part; p && !bg; p = p->parent) {
    bg = P(P(p->root, "cSld"), "bg");
    if (bg) owner = p;
  }
  // No background anywhere in the chain: PowerPoint fills the slide with bg1, through the color
  // map of the slide shown (a layout's override can make it dark).
  pugi::xml_node bgPr = P(bg, "bgPr"), bgRef = P(bg, "bgRef");
  pugi::xml_document defaultFill;
  if (!bgPr && !bgRef) {
    std::string xml = std::string("<p:bgPr xmlns:p=\"") + std::string(kP) + "\" xmlns:a=\"" + std::string(kA) +
                      "\"><a:solidFill><a:schemeClr val=\"bg1\"/></a:solidFill></p:bgPr>";
    defaultFill.load_buffer(xml.data(), xml.size());
    bgPr = defaultFill.document_element();
    owner = &part;
  }
  pugi::xml_document& doc = part.scratch;
  pugi::xml_node sp = doc.append_child("p:sp");
  sp.append_attribute("xmlns:p") = std::string(kP).c_str();
  sp.append_attribute("xmlns:a") = std::string(kA).c_str();
  sp.append_attribute("xmlns:r") = std::string(kR).c_str();
  pugi::xml_node nv = sp.append_child("p:nvSpPr");
  pugi::xml_node c = nv.append_child("p:cNvPr");
  c.append_attribute("id") = "0";
  c.append_attribute("name") = "Background";
  nv.append_child("p:cNvSpPr");
  nv.append_child("p:nvPr");
  pugi::xml_node spPr = sp.append_child("p:spPr");
  pugi::xml_node xfrm = spPr.append_child("a:xfrm");
  pugi::xml_node off = xfrm.append_child("a:off");
  off.append_attribute("x") = "0";
  off.append_attribute("y") = "0";
  pugi::xml_node ext = xfrm.append_child("a:ext");
  ext.append_attribute("cx") = static_cast<long long>(size.width * 12700.0 + 0.5);
  ext.append_attribute("cy") = static_cast<long long>(size.height * 12700.0 + 0.5);
  pugi::xml_node geom = spPr.append_child("a:prstGeom");
  geom.append_attribute("prst") = "rect";
  geom.append_child("a:avLst");
  if (bgPr) {
    bool filled = false;
    ForEachChild(bgPr, [&](pugi::xml_node f) {
      if (IsFill(LocalNameOf(f)) && !filled) {
        CopyInto(spPr, f);
        filled = true;
      }
      return !filled;
    });
  }
  pugi::xml_node ln = spPr.append_child("a:ln");
  ln.append_child("a:noFill");
  if (!bgPr && bgRef) {
    pugi::xml_node style = sp.append_child("p:style");
    pugi::xml_node lnRef = style.append_child("a:lnRef");
    lnRef.append_attribute("idx") = "0";
    pugi::xml_node fillRef = CopyInto(style, bgRef);
    fillRef.set_name("a:fillRef");
    pugi::xml_node effectRef = style.append_child("a:effectRef");
    effectRef.append_attribute("idx") = "0";
    pugi::xml_node fontRef = style.append_child("a:fontRef");
    fontRef.append_attribute("idx") = "none";
  }
  part.background = sp;
  part.backgroundOwner = owner;
}

Hyperlink ReadHyperlink(pugi::xml_node link, const SlideBase& owner, const Presentation& deck) {
  Hyperlink h;
  if (!link) return h;
  h.actionText = link.attribute("action").value();
  h.tooltip = link.attribute("tooltip").value();
  h.highlightClick = Bool(link, "highlightClick", false);
  h.endSound = Bool(link, "endSnd", false);
  h.soundRel = std::string(RelId(A(link, "snd"), "embed"));
  std::string_view action = h.actionText;
  const pkg::Relationship* rel = owner.relationships ? owner.relationships->Find(RelId(link, "id")) : nullptr;
  auto param = [&](std::string_view name) -> std::string_view {
    size_t q = action.find('?');
    while (q != std::string_view::npos && q + 1 < action.size()) {
      std::string_view rest = action.substr(q + 1);
      size_t amp = rest.find('&');
      std::string_view pair = rest.substr(0, amp);
      if (pair.starts_with(name) && pair.size() > name.size() && pair[name.size()] == '=') {
        return pair.substr(name.size() + 1);
      }
      q = amp == std::string_view::npos ? amp : q + 1 + amp;
    }
    return {};
  };
  if (action.starts_with("ppaction://hlinkshowjump")) {
    std::string_view jump = param("jump");
    h.action = jump == "firstslide"         ? Hyperlink::Action::FirstSlide
               : jump == "lastslide"        ? Hyperlink::Action::LastSlide
               : jump == "nextslide"        ? Hyperlink::Action::NextSlide
               : jump == "previousslide"    ? Hyperlink::Action::PreviousSlide
               : jump == "lastslideviewed"  ? Hyperlink::Action::LastViewed
               : jump == "endshow"          ? Hyperlink::Action::EndShow
                                            : Hyperlink::Action::Other;
  } else if (action.starts_with("ppaction://hlinksldjump")) {
    h.action = Hyperlink::Action::Slide;
    if (rel && !rel->external) h.slide = deck.SlideIndexOfPart(rel->targetPart);
  } else if (action.starts_with("ppaction://customshow")) {
    h.action = Hyperlink::Action::CustomShow;
    double id = Number(param("id"), 0);
    h.customShow = id >= 0 && id <= 4294967295.0 ? static_cast<uint32_t>(id) : 0;
    h.returns = param("return") == "true";
  } else if (action.starts_with("ppaction://program")) {
    h.action = Hyperlink::Action::Program;
  } else if (action.starts_with("ppaction://macro")) {
    h.action = Hyperlink::Action::Macro;
  } else if (action.starts_with("ppaction://ole")) {
    h.action = Hyperlink::Action::Ole;
  } else if (action.starts_with("ppaction://noaction")) {
    h.action = Hyperlink::Action::None;
  } else if (action.starts_with("ppaction://hlinkfile") || action.starts_with("ppaction://hlinkpres")) {
    h.action = Hyperlink::Action::File;
    if (rel) h.url = rel->target;
  } else if (!action.empty()) {
    h.action = Hyperlink::Action::Other;
  } else if (rel && rel->external) {
    h.url = rel->target;
    std::string lower = h.url.substr(0, 8);
    for (char& ch : lower) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
    h.action = lower.starts_with("file:") ? Hyperlink::Action::File : Hyperlink::Action::Url;
  } else if (rel) {
    // An internal target without an action: a slide when it is one.
    h.slide = deck.SlideIndexOfPart(rel->targetPart);
    h.action = h.slide >= 0 ? Hyperlink::Action::Slide : Hyperlink::Action::Other;
  }
  return h;
}

}  // namespace detail

}  // namespace fw::slide
