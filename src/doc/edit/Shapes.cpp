#include "doc/edit/Shapes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>

#include "doc/docx/Docx.h"
#include "doc/edit/DrawingEdit.h"
#include "pkg/Mce.h"
#include "pkg/RoundTrip.h"
#include "pkg/XmlDom.h"

namespace fw::doc::edit {

using namespace dx;

namespace {

constexpr std::string_view kMc = pkg::mce::kNamespace;
constexpr std::string_view kW = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
constexpr const char* kWpsUri = "http://schemas.microsoft.com/office/word/2010/wordprocessingShape";

// A shape's XML parsed for a change: its wp:anchor (or wp:inline) and wps:wsp, in the fragments
// of `node`. A shape kept as an mc:AlternateContent block (`alternate`): the block is the fragment,
// the DrawingML its choice, and the VML copy its fallback.
struct ShapeFrame : Frame {
  pkg::NodeId node = pkg::kNoNode;
  bool alternate = false;
};

// ---------- Finding a shape's XML ----------

// The store fragment with this tag among paragraph `para`'s elements: its owner, the owner's
// fragments and its place among them.
bool FindTagged(const Document& doc, uint32_t para, uint32_t tag, ShapeFrame& f) {
  std::vector<Node> work{doc.ParaNode(para)};
  while (!work.empty()) {
    const Node n = work.back();
    work.pop_back();
    if (const pkg::NodeId id = n.Raw().node; id != pkg::kNoNode) {
      std::vector<pkg::Fragment> list = doc.Store().FragmentsCopy(id);
      for (size_t k = 0; k < list.size(); ++k)
        if (list[k].tag == tag) {
          f.node = id;
          f.fragments = std::move(list);
          f.index = k;
          return true;
        }
    }
    for (Node c : n.Kids()) work.push_back(c);
  }
  return false;
}

// The wp:anchor or wp:inline in a block's choice (a w:drawing) or at a fragment's root.
pugi::xml_node FrameIn(pugi::xml_node root, bool alternate) {
  auto frame = [](pugi::xml_node n) { return pkg::IsElement(n, kWp, "inline") || pkg::IsElement(n, kWp, "anchor"); };
  if (!alternate) return frame(root) ? root : pugi::xml_node();
  for (pugi::xml_node b = root.first_child(); b; b = b.next_sibling()) {
    if (!pkg::IsElement(b, kMc, "Choice")) continue;
    for (pugi::xml_node d = b.first_child(); d; d = d.next_sibling())
      if (d.type() == pugi::node_element && pkg::LocalNameOf(d) == "drawing" && NsFromUri(pkg::NamespaceOf(d)) == Ns::W)
        for (pugi::xml_node c = d.first_child(); c; c = c.next_sibling())
          if (frame(c)) return c;
  }
  return {};
}

// A block shows only its drawing: each branch is one element.
bool OneDrawing(pugi::xml_node block) {
  size_t branches = 0;
  for (pugi::xml_node b = block.first_child(); b; b = b.next_sibling()) {
    if (b.type() != pugi::node_element) continue;
    if (!pkg::IsElement(b, kMc, "Choice") && !pkg::IsElement(b, kMc, "Fallback")) return false;
    size_t elements = 0;
    for (pugi::xml_node c = b.first_child(); c; c = c.next_sibling()) elements += c.type() == pugi::node_element;
    if (elements != 1) return false;
    ++branches;
  }
  return branches > 0;
}

bool Locate(ShapeFrame& f, std::u16string& why) {
  f.root = FrameIn(f.dom.Root(), f.alternate);
  pugi::xml_node data = Child(Child(f.root, kA, "graphic"), kA, "graphicData");
  f.shape = Child(data, kWps, "wsp");
  if (!f.root || !f.shape) {
    why = u"Only shapes and text boxes can be changed here. This is a picture, a group, a chart or another kind of drawing.";
    return false;
  }
  f.wp = PrefixOf(f.root);
  f.a = PrefixOf(Child(f.root, kA, "graphic"));
  return true;
}

// Parses drawing `i` for a change; false with the reason when it isn't a shape this changes.
bool LoadShape(const Editor& editor, uint32_t i, ShapeFrame& f, std::u16string& why) {
  const Document& doc = editor.Doc();
  if (!editor.CanEdit(&why)) return false;
  if (i >= doc.Drawings().size() || doc.Drawings()[i].pos.para == kNone || !doc.Drawings()[i].node) {
    why = u"That shape isn't in the document any more.";
    return false;
  }
  const Drawing& d = doc.Drawings()[i];
  if (d.kind != DrawingKind::DrawingML) {
    why = d.kind == DrawingKind::Vml ? u"This shape is in an older format, so it can't be changed. The text in it can be edited."
                                      : u"Only shapes and text boxes can be changed here.";
    return false;
  }
  const Part* part = PartOf(doc, d.part);
  if (!part || !part->Editable() || !part->writable) {
    why = u"This part of the document uses features Featherweight doesn't understand, so it can't be edited.";
    return false;
  }
  if (doc.Stories()[doc.Para(d.pos.para).story].drawing != kNone) {
    why = u"Shapes in a text box can't be changed yet.";
    return false;
  }
  const std::u16string kept = u"This shape is kept with other content of the file, so it can't be changed.";
  if (d.displayOnly) {
    if (d.kept == 0 || !FindTagged(doc, d.pos.para, d.kept, f) ||
        !f.dom.Parse(f.fragments[f.index].xml, doc.Package().GetLimits()).Ok() ||
        !pkg::IsElement(f.dom.Root(), kMc, "AlternateContent") || !OneDrawing(f.dom.Root())) {
      why = kept;
      return false;
    }
    f.alternate = true;
  } else {
    if (!d.keptXml.empty()) {
      why = kept;
      return false;
    }
    f.node = d.node.Raw().node;
    f.fragments = doc.Store().FragmentsCopy(f.node);
    bool found = false;
    for (size_t k = 0; k < f.fragments.size() && !found; ++k) {
      const pkg::Fragment& frag = f.fragments[k];
      if (frag.slot != pkg::Slot::Inside || frag.xml.find('<') == std::string::npos) continue;
      if (!f.dom.Parse(frag.xml, doc.Package().GetLimits()).Ok()) continue;
      if (FrameIn(f.dom.Root(), false)) {
        f.index = k;
        found = true;
      }
    }
    if (!found) {
      why = u"Only shapes and text boxes can be changed here.";
      return false;
    }
  }
  return Locate(f, why);
}

// ---------- Reading and writing its look ----------

std::string Hex(uint32_t rgb) {
  char buf[8];
  std::snprintf(buf, sizeof buf, "%06X", rgb & 0xFFFFFF);
  return buf;
}

// A color element's sRGB: a:srgbClr, or a:schemeClr from the document's theme.
std::optional<uint32_t> ColorOf(pugi::xml_node fill, const Document& doc) {
  if (pugi::xml_node c = Child(fill, kA, "srgbClr")) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(c.attribute("val").value(), &end, 16);
    if (end && end != c.attribute("val").value() && *end == 0) return static_cast<uint32_t>(v & 0xFFFFFF);
  }
  if (pugi::xml_node c = Child(fill, kA, "schemeClr")) {
    std::string_view name = c.attribute("val").value();
    if (name == "tx1") name = "text1";
    else if (name == "bg1") name = "background1";
    else if (name == "tx2") name = "text2";
    else if (name == "bg2") name = "background2";
    if (auto rgb = doc.ThemeColor(name)) return *rgb;
  }
  return std::nullopt;
}

pugi::xml_node SpPr(ShapeFrame& f) {
  if (!Child(f.shape, kWps, "spPr")) Xfrm(f, true);
  return Child(f.shape, kWps, "spPr");
}

constexpr const char* kFills[] = {"noFill", "solidFill", "gradFill", "blipFill", "pattFill", "grpFill"};

void SetFillXml(ShapeFrame& f, std::optional<uint32_t> rgb) {
  pugi::xml_node spPr = SpPr(f);
  for (const char* k : kFills)
    while (pugi::xml_node n = Child(spPr, kA, k)) spPr.remove_child(n);
  pugi::xml_node fill = InsertAfterFirst(spPr, f.A(rgb ? "solidFill" : "noFill"), {{kA, "xfrm"}, {kA, "custGeom"}, {kA, "prstGeom"}});
  if (rgb) fill.append_child(f.A("srgbClr").c_str()).append_attribute("val") = Hex(*rgb).c_str();
}

void SetOutlineXml(ShapeFrame& f, std::optional<uint32_t> rgb, double points) {
  pugi::xml_node spPr = SpPr(f);
  pugi::xml_node ln = Child(spPr, kA, "ln");
  if (!ln)
    ln = InsertAfterFirst(spPr, f.A("ln"), {{kA, "xfrm"}, {kA, "custGeom"}, {kA, "prstGeom"}, {kA, "noFill"}, {kA, "solidFill"},
                                            {kA, "gradFill"}, {kA, "blipFill"}, {kA, "pattFill"}, {kA, "grpFill"}});
  for (const char* k : {"noFill", "solidFill", "gradFill", "pattFill"})
    while (pugi::xml_node n = Child(ln, kA, k)) ln.remove_child(n);
  pugi::xml_node fill = ln.prepend_child(f.A(rgb ? "solidFill" : "noFill").c_str());
  if (rgb) fill.append_child(f.A("srgbClr").c_str()).append_attribute("val") = Hex(*rgb).c_str();
  if (points > 0 && std::isfinite(points)) SetInt(ln, "w", std::clamp<int64_t>(std::llround(points * kEmuPerPoint), 0, 20116800));
}

bool IsLine(pugi::xml_node shape) {
  if (Child(shape, kWps, "cNvCnPr")) return true;
  const std::string_view prst = Child(Child(shape, kWps, "spPr"), kA, "prstGeom").attribute("prst").value();
  return prst == "line" || prst.find("Connector") != std::string_view::npos;
}

// ---------- The VML copy ----------

std::string Pt(int64_t emu) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(emu) / kEmuPerPoint);
  std::string s = buf;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s + "pt";
}

std::string_view VmlRelative(std::string_view from, bool horizontal) {
  if (from == "page" || from == "margin" || from == "line") return from;
  if (from == "column" || from == "paragraph") return "text";
  if (from == "character") return "char";
  if (from == "leftMargin") return "left-margin-area";
  if (from == "rightMargin") return "right-margin-area";
  if (from == "topMargin") return "top-margin-area";
  if (from == "bottomMargin") return "bottom-margin-area";
  if (from == "insideMargin") return horizontal ? "inner-margin-area" : "inner-margin-area";
  if (from == "outsideMargin") return "outer-margin-area";
  return horizontal ? "text" : "text";
}

std::string_view Arrow(std::string_view type) {
  if (type == "triangle") return "block";
  if (type == "stealth") return "classic";
  if (type == "arrow") return "open";
  if (type == "oval") return "oval";
  if (type == "diamond") return "diamond";
  return {};
}

// The shape's VML, for Word before 2010: a w:pict holding a rectangle, rounded rectangle, oval or
// line (a text box is a rectangle), with an empty v:textbox where its text goes. Empty for a shape
// Featherweight doesn't write in VML.
std::string VmlOf(ShapeFrame& f, const Document& doc) {
  pugi::xml_node spPr = Child(f.shape, kWps, "spPr");
  if (Child(spPr, kA, "custGeom")) return {};
  const std::string prst = Child(spPr, kA, "prstGeom").attribute("prst").value();
  const bool line = IsLine(f.shape);
  std::string element;
  if (line && (prst == "line" || prst == "straightConnector1")) element = "v:line";
  else if (prst == "rect") element = "v:rect";
  else if (prst == "roundRect") element = "v:roundrect";
  else if (prst == "ellipse") element = "v:oval";
  else return {};
  const bool anchored = pkg::LocalNameOf(f.root) == "anchor";
  pugi::xml_node extent = Child(f.root, kWp, "extent");
  const int64_t cx = AttrInt(extent, "cx"), cy = AttrInt(extent, "cy");
  pugi::xml_node xfrm = Child(spPr, kA, "xfrm");
  const auto on = [](pugi::xml_node n, const char* a) {
    const std::string_view v = n.attribute(a).value();
    return v == "1" || v == "true";
  };
  // Where it is.
  std::string style;
  int64_t x = 0, y = 0;
  bool xSet = false, ySet = false;
  if (anchored) {
    style = "position:absolute;";
    for (const char* axis : {"positionH", "positionV"}) {
      const bool h = axis[8] == 'H';
      pugi::xml_node pos = Child(f.root, kWp, axis);
      const std::string_view from = pos.attribute("relativeFrom").value();
      if (pugi::xml_node off = Child(pos, kWp, "posOffset")) {
        const int64_t v = std::strtoll(off.text().get(), nullptr, 10);
        (h ? x : y) = v;
        (h ? xSet : ySet) = true;
        if (!line) style += std::string(h ? "margin-left:" : "margin-top:") + Pt(v) + ";";
      } else if (pugi::xml_node al = Child(pos, kWp, "align")) {
        style += std::string(h ? "mso-position-horizontal:" : "mso-position-vertical:") + al.text().get() + ";";
      }
      style += std::string(h ? "mso-position-horizontal-relative:" : "mso-position-vertical-relative:") +
               std::string(VmlRelative(from, h)) + ";";
    }
  }
  if (!line) style += "width:" + Pt(cx) + ";height:" + Pt(cy) + ";";
  if (anchored) {
    int64_t z = AttrInt(f.root, "relativeHeight");
    if (on(f.root, "behindDoc")) z = -z;
    style += "z-index:" + std::to_string(z) + ";";
  }
  if (const int64_t rot = AttrInt(xfrm, "rot"); rot != 0) style += "rotation:" + std::to_string(rot / 60000) + ";";
  if (on(xfrm, "flipH") || on(xfrm, "flipV"))
    style += std::string("flip:") + (on(xfrm, "flipH") ? "x" : "") + (on(xfrm, "flipH") && on(xfrm, "flipV") ? " " : "") +
             (on(xfrm, "flipV") ? "y" : "") + ";";
  if (!style.empty() && style.back() == ';') style.pop_back();

  pugi::xml_node docPr = Child(f.root, kWp, "docPr");
  std::string xml = "<w:pict><" + element + " id=\"" + Escape(docPr.attribute("name").value()) + "\" o:spid=\"_x0000_s" +
                    std::to_string(4096 + AttrInt(docPr, "id")) + "\" style=\"" + style + "\"";
  if (prst == "roundRect") xml += " arcsize=\"10923f\"";
  if (line) {
    // From one end to the other, on the page; a flipped line runs the other way.
    const bool fv = on(xfrm, "flipV"), fh = on(xfrm, "flipH");
    const int64_t x1 = fh ? x + cx : x, x2 = fh ? x : x + cx, y1 = fv ? y + cy : y, y2 = fv ? y : y + cy;
    xml += " from=\"" + Pt(xSet ? x1 : 0) + "," + Pt(ySet ? y1 : 0) + "\" to=\"" + Pt(xSet ? x2 : cx) + "," + Pt(ySet ? y2 : cy) + "\"";
  }
  // Its fill and outline, where they're plain colors.
  if (!line) {
    if (Child(spPr, kA, "noFill")) xml += " filled=\"f\"";
    else if (auto c = ColorOf(Child(spPr, kA, "solidFill"), doc)) xml += " fillcolor=\"#" + Hex(*c) + "\"";
  }
  pugi::xml_node ln = Child(spPr, kA, "ln");
  if (Child(ln, kA, "noFill")) {
    xml += " stroked=\"f\"";
  } else {
    if (auto c = ColorOf(Child(ln, kA, "solidFill"), doc)) xml += " strokecolor=\"#" + Hex(*c) + "\"";
    if (const int64_t w = AttrInt(ln, "w"); w > 0) xml += " strokeweight=\"" + Pt(w) + "\"";
  }
  xml += ">";
  const std::string_view start = Arrow(Child(ln, kA, "headEnd").attribute("type").value());
  const std::string_view end = Arrow(Child(ln, kA, "tailEnd").attribute("type").value());
  if (!start.empty() || !end.empty()) {
    xml += "<v:stroke";
    if (!start.empty()) xml += " startarrow=\"" + std::string(start) + "\"";
    if (!end.empty()) xml += " endarrow=\"" + std::string(end) + "\"";
    xml += "/>";
  }
  if (Child(f.shape, kWps, "txbx")) {
    pugi::xml_node body = Child(f.shape, kWps, "bodyPr");
    auto inset = [&](const char* a, int64_t fallback) { return Pt(AttrInt(body, a, fallback)); };
    xml += "<v:textbox inset=\"" + inset("lIns", 91440) + "," + inset("tIns", 45720) + "," + inset("rIns", 91440) + "," +
           inset("bIns", 45720) + "\"";
    if (Child(body, kA, "spAutoFit")) xml += " style=\"mso-fit-shape-to-text:t\"";
    xml += "/>";
  }
  if (anchored) {
    const char* wrap = Child(f.root, kWp, "wrapSquare") ? "square" : Child(f.root, kWp, "wrapTight") ? "tight"
                       : Child(f.root, kWp, "wrapThrough") ? "through" : Child(f.root, kWp, "wrapTopAndBottom") ? "topAndBottom"
                                                                                                     : nullptr;
    if (wrap) xml += std::string("<w10:wrap type=\"") + wrap + "\"/>";
  }
  xml += "</" + element + "></w:pict>";
  return xml;
}

std::string_view InScope(pugi::xml_node node, std::string_view prefix) {
  const std::string decl = prefix.empty() ? std::string("xmlns") : "xmlns:" + std::string(prefix);
  for (pugi::xml_node n = node; n; n = n.parent())
    if (pugi::xml_attribute a = n.attribute(decl.c_str())) return a.value();
  return {};
}

// A copy of `src` (from anywhere) appended to `dest`, declaring on the copy each prefix it uses that
// isn't bound the same where it goes.
pugi::xml_node AppendInScope(pugi::xml_node dest, pugi::xml_node src) {
  pugi::xml_node put = dest.append_copy(src);
  std::vector<std::string> prefixes;
  auto use = [&](std::string_view qname, bool element) {
    const size_t colon = qname.find(':');
    const std::string_view p = colon == std::string_view::npos ? std::string_view() : qname.substr(0, colon);
    if (p == "xml" || p == "xmlns" || qname == "xmlns" || (!element && p.empty())) return;
    if (std::find(prefixes.begin(), prefixes.end(), p) == prefixes.end()) prefixes.emplace_back(p);
  };
  std::vector<pugi::xml_node> work{src};
  while (!work.empty()) {
    pugi::xml_node n = work.back();
    work.pop_back();
    if (n.type() != pugi::node_element) continue;
    use(n.name(), true);
    for (pugi::xml_attribute a : n.attributes()) use(a.name(), false);
    for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling()) work.push_back(c);
  }
  for (const std::string& p : prefixes) {
    const std::string_view want = InScope(src, p);
    if (want.empty() || InScope(put, p) == want) continue;
    put.prepend_attribute((p.empty() ? std::string("xmlns") : "xmlns:" + p).c_str()) = std::string(want).c_str();
  }
  return put;
}

// A block's VML copy, written again from its DrawingML: its mc:Fallback replaced by a new one, or
// none for a shape Featherweight doesn't write in VML. The text goes in as a copy of the choice's.
void RewriteFallback(ShapeFrame& f, const Document& doc) {
  if (!f.alternate) return;
  pugi::xml_node block = f.dom.Root();
  for (pugi::xml_node b = block.first_child(); b;) {
    pugi::xml_node next = b.next_sibling();
    if (pkg::IsElement(b, kMc, "Fallback")) block.remove_child(b);
    b = next;
  }
  const std::string vml = VmlOf(f, doc);
  if (vml.empty()) return;
  pkg::XmlDom piece;
  const std::string wrapped = "<fw xmlns:w=\"" + std::string(kW) + "\" xmlns:v=\"" + std::string(kV) + "\" xmlns:o=\"" +
                              std::string(kO) + "\" xmlns:w10=\"" + std::string(kW10) + "\">" + vml + "</fw>";
  if (!piece.Parse(wrapped, doc.Package().GetLimits()).Ok()) return;
  pugi::xml_node fallback = block.append_child((PrefixOf(block) + "Fallback").c_str());
  pugi::xml_node pict = AppendInScope(fallback, piece.Root().first_child());
  // The text: a copy of the choice's, in the copy's v:textbox.
  pugi::xml_node textbox;
  for (pugi::xml_node shape = pict.first_child(); shape && !textbox; shape = shape.next_sibling())
    for (pugi::xml_node c = shape.first_child(); c && !textbox; c = c.next_sibling())
      if (pkg::IsElement(c, kV, "textbox")) textbox = c;
  const std::vector<pugi::xml_node> boxes = docx::TextBoxesIn(f.shape);
  if (textbox && !boxes.empty()) AppendInScope(textbox, boxes.front());
}

// Whether each copy of the shape's text (each branch of its block, or its own XML) has `boxes`
// text boxes: its stories can be written back.
bool BoxesLineUp(ShapeFrame& f, size_t boxes) {
  if (boxes == 0) return false;
  if (!f.alternate) return docx::TextBoxesIn(f.dom.Doc()).size() == boxes && docx::TextBoxesIn(f.dom.Doc(), true).size() == boxes;
  size_t branches = 0;
  for (pugi::xml_node b = f.dom.Root().first_child(); b; b = b.next_sibling()) {
    if (b.type() != pugi::node_element) continue;
    ++branches;
    if (docx::TextBoxesIn(b).size() != boxes || docx::TextBoxesIn(b, true).size() != boxes) return false;
  }
  return branches > 0;
}

// ---------- Making a change ----------

// Loads shape `i`, lets `edit` change its DrawingML, writes its VML copy again, and records the
// change as one undo step. `tweak` adjusts the drawing's new handle (its text boxes).
Result ChangeShape(Editor& editor, uint32_t i, const char* name, const std::function<bool(ShapeFrame&, std::u16string&)>& edit,
                   const std::function<bool(Drawing&)>& tweak = {}) {
  ShapeFrame f;
  std::u16string why;
  if (!LoadShape(editor, i, f, why)) return Fail(why);
  Document& doc = const_cast<Document&>(editor.Doc());
  const Drawing was = doc.Drawings()[i];
  if (!edit(f, why)) return Fail(why);
  RewriteFallback(f, doc);
  std::string xml = pkg::RoundTripStore::SerializeFragment(f.dom.Root());
  if (!XmlSafe(xml)) return Fail(u"That can't be written in the document.");
  if (xml == f.fragments[f.index].xml) {
    Result r;
    r.caret = was.pos;
    return r;  // nothing changed
  }
  Drawing now = HandleFor(was, f.root);
  now.displayOnly = was.displayOnly;
  now.kept = was.kept;
  now.keptXml = was.displayOnly ? pkg::RoundTripStore::SerializeFragment(f.root) : std::string();
  now.textBoxes = was.textBoxes;
  if (tweak && !tweak(now)) return Fail(u"That can't be done to this shape.");
  now.boxesWritable = BoxesLineUp(f, now.textBoxes.size());
  auto step = std::make_shared<DrawingStep>();
  step->doc = &doc;
  step->drawing = i;
  step->node = f.node;
  step->whole = true;
  step->before = f.fragments;
  step->after = f.fragments;
  step->after[f.index].xml = std::move(xml);
  step->handleBefore = was;
  step->handleAfter = std::move(now);
  step->parts = {was.part};
  step->first = was.pos.para;
  step->count = 1;
  const Pos at = was.pos;
  step->Redo();
  editor.RecordStep(Msg(name), step, at);
  Result r;
  r.caret = at;
  return r;
}

// A new text box story for drawing `drawing` in part `part`: one empty paragraph. Made outside
// undo; the drawing that shows it comes with the change after it.
uint32_t NewTextStory(Editor& editor, Document& doc, uint32_t part, uint32_t drawing) {
  const Schema& s = Schema::Get();
  const ElemDefId rootDef = s.Child(s.RootType(), Name("txbxContent"));
  if (rootDef == kNoDef) return kNone;
  const ElemDefId pDef = s.Child(s.Elem(rootDef).type, Name("p"));
  if (pDef == kNoDef) return kNone;
  Tree& t = PictureAccess::Elems(doc);
  std::vector<Paragraph>& paras = PictureAccess::Paras(doc);
  std::vector<Story>& stories = PictureAccess::Stories(doc);
  const ElemId root = t.Add(rootDef, kNoElem);
  t.elems[root].flags |= kDetached;
  t.elems[root].node = editor.NewNodeId();
  const ElemId p = t.Add(pDef, root);
  t.elems[p].node = editor.NewNodeId();
  t.elems[root].first = p;
  Paragraph para;
  para.elem = p;
  para.story = static_cast<uint32_t>(stories.size());
  t.elems[p].start = static_cast<uint32_t>(paras.size());
  paras.push_back(std::move(para));
  Story st;
  st.kind = StoryKind::TextBox;
  st.part = part;
  st.root = root;
  st.drawing = drawing;
  stories.push_back(std::move(st));
  PictureAccess::Refresh(doc);
  return static_cast<uint32_t>(stories.size() - 1);
}

const char* KindName(ShapeKind kind) {
  switch (kind) {
    case ShapeKind::TextBox: return "Text Box";
    case ShapeKind::Rectangle: return "Rectangle";
    case ShapeKind::RoundedRectangle: return "Rectangle: Rounded Corners";
    case ShapeKind::Oval: return "Oval";
    case ShapeKind::Line: return "Straight Connector";
    case ShapeKind::Arrow: return "Straight Arrow Connector";
    case ShapeKind::Callout: return "Speech Bubble: Rectangle";
  }
  return "Shape";
}

// A new shape's wp:anchor: floating in front of the text, placed on the page, Word's look for its
// kind (blue shapes with a darker outline; text boxes and callouts white, with a thin black one).
std::string AnchorXml(ShapeKind kind, uint32_t id, const std::string& name, int64_t x, int64_t y, int64_t cx, int64_t cy,
                      int64_t z) {
  const bool line = kind == ShapeKind::Line || kind == ShapeKind::Arrow;
  const bool text = kind == ShapeKind::TextBox || kind == ShapeKind::Callout;
  const char* prst = kind == ShapeKind::RoundedRectangle ? "roundRect" : kind == ShapeKind::Oval ? "ellipse"
                     : line ? "line" : kind == ShapeKind::Callout ? "wedgeRectCallout" : "rect";
  auto solid = [](const char* rgb) { return std::string("<a:solidFill><a:srgbClr val=\"") + rgb + "\"/></a:solidFill>"; };
  std::string look;
  if (line) look = "<a:ln w=\"12700\">" + solid("4472C4") + (kind == ShapeKind::Arrow ? "<a:tailEnd type=\"triangle\"/>" : "") + "</a:ln>";
  else if (text) look = solid("FFFFFF") + "<a:ln w=\"6350\">" + solid("000000") + "</a:ln>";
  else look = solid("4472C4") + "<a:ln w=\"12700\">" + solid("2F528F") + "</a:ln>";
  const std::string nv = line ? "<wps:cNvCnPr/>" : kind == ShapeKind::TextBox ? "<wps:cNvSpPr txBox=\"1\"/>" : "<wps:cNvSpPr/>";
  const std::string body = text ? "<wps:txbx><w:txbxContent><w:p/></w:txbxContent></wps:txbx><wps:bodyPr rot=\"0\" vert=\"horz\" "
                                  "wrap=\"square\" lIns=\"91440\" tIns=\"45720\" rIns=\"91440\" bIns=\"45720\" anchor=\"t\" "
                                  "anchorCtr=\"0\"><a:spAutoFit/></wps:bodyPr>"
                                : "<wps:bodyPr/>";
  const std::string n = std::to_string(id);
  return "<wp:anchor distT=\"0\" distB=\"0\" distL=\"114300\" distR=\"114300\" simplePos=\"0\" relativeHeight=\"" +
         std::to_string(z) + "\" behindDoc=\"0\" locked=\"0\" layoutInCell=\"1\" allowOverlap=\"1\"><wp:simplePos x=\"0\" y=\"0\"/>" +
         "<wp:positionH relativeFrom=\"page\"><wp:posOffset>" + std::to_string(x) + "</wp:posOffset></wp:positionH>" +
         "<wp:positionV relativeFrom=\"page\"><wp:posOffset>" + std::to_string(y) + "</wp:posOffset></wp:positionV>" +
         "<wp:extent cx=\"" + std::to_string(cx) + "\" cy=\"" + std::to_string(cy) + "\"/>" +
         "<wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/><wp:wrapNone/><wp:docPr id=\"" + n + "\" name=\"" + Escape(name) +
         "\"/><wp:cNvGraphicFramePr/><a:graphic><a:graphicData uri=\"" + kWpsUri + "\"><wps:wsp>" + nv +
         "<wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"" + std::to_string(cx) + "\" cy=\"" + std::to_string(cy) +
         "\"/></a:xfrm><a:prstGeom prst=\"" + prst + "\"><a:avLst/></a:prstGeom>" + look + "</wps:spPr>" + body +
         "</wps:wsp></a:graphicData></a:graphic></wp:anchor>";
}

int64_t Side(int64_t v, bool line) { return line ? std::clamp<int64_t>(v, 0, kMaxEmu) : ClampEmu(v); }

}  // namespace

// ---------- Shapes ----------

Shapes::Shapes(Editor& editor) : editor_(editor) {}

Result Shapes::Insert(uint32_t para, ShapeKind kind, Emu x, Emu y, Emu cx, Emu cy) {
  Document& doc = const_cast<Document&>(editor_.Doc());
  std::u16string why;
  if (!editor_.CanEdit(&why)) return Fail(why);
  if (para >= doc.Paragraphs().size()) return Fail(u"That place isn't in the document.");
  const Story& story = doc.Stories()[doc.Para(para).story];
  const Part* part = PartOf(doc, story.part);
  if (!part || story.drawing != kNone || doc.ParaNode(para).DisplayOnly()) return Fail(u"A shape can't go here.");
  // Word doesn't open a file with one there (D5 torture).
  if (story.kind == StoryKind::Footnote || story.kind == StoryKind::Endnote || story.kind == StoryKind::Comment)
    return Fail(u"Shapes can't go in a footnote, an endnote or a comment.");
  if (Strict(*part)) return Fail(u"Shapes can't be added to documents saved as Strict Open XML yet.");
  const bool line = kind == ShapeKind::Line || kind == ShapeKind::Arrow;
  const bool text = kind == ShapeKind::TextBox || kind == ShapeKind::Callout;
  int64_t w = Side(cx.Value(), line), h = Side(cy.Value(), line);
  if (line && w == 0 && h == 0) w = 914400;
  const int64_t px = std::clamp<int64_t>(x.Value(), -kMaxEmu, kMaxEmu), py = std::clamp<int64_t>(y.Value(), -kMaxEmu, kMaxEmu);
  // Its docPr id and its block's tag: ones no drawing has.
  const uint32_t id = FreeDrawingId(doc);
  uint32_t tag = 1;
  for (const Drawing& d : doc.Drawings()) tag = std::max(tag, d.kept + 1);
  const std::string name = std::string(KindName(kind)) + " " + std::to_string(id);
  ShapeFrame f;
  f.alternate = true;
  const std::string xml = "<mc:AlternateContent xmlns:mc=\"" + std::string(kMc) + "\" xmlns:w=\"" + std::string(kW) +
                          "\" xmlns:wp=\"" + std::string(kWp) + "\" xmlns:a=\"" + std::string(kA) + "\" xmlns:wps=\"" +
                          std::string(kWps) + "\"><mc:Choice Requires=\"wps\"><w:drawing>" +
                          AnchorXml(kind, id, name, px, py, w, h, TopZ(doc)) + "</w:drawing></mc:Choice></mc:AlternateContent>";
  if (!f.dom.Parse(xml, doc.Package().GetLimits()).Ok() || !Locate(f, why)) return Fail(u"This shape can't be added.");
  RewriteFallback(f, doc);
  std::string block = pkg::RoundTripStore::SerializeFragment(f.dom.Root());
  Drawing info;
  info.kind = DrawingKind::DrawingML;
  docx::ReadDrawingFrame(f.root, info);
  info.kept = tag;
  info.keptXml = pkg::RoundTripStore::SerializeFragment(f.root);
  if (text) {
    // Its text, a story made outside undo, for the drawing about to be added.
    const uint32_t s = NewTextStory(editor_, doc, story.part, static_cast<uint32_t>(doc.Drawings().size()));
    if (s == kNone) return Fail(u"This shape can't be added.");
    info.textBoxes = {s};
    info.boxesWritable = true;
  }
  Result r = editor_.InsertKeptDrawing(para, std::move(block), info);
  if (r) r.caret = Pos{para, 0};  // the shape's own character: the view selects it
  return r;
}

ShapeInfo Shapes::Info(uint32_t drawing) const {
  ShapeInfo info;
  ShapeFrame f;
  if (!LoadShape(editor_, drawing, f, info.why)) return info;
  const Document& doc = editor_.Doc();
  const Drawing& d = doc.Drawings()[drawing];
  info.shape = true;
  pugi::xml_node spPr = Child(f.shape, kWps, "spPr");
  info.preset = Child(spPr, kA, "prstGeom").attribute("prst").value();
  info.line = IsLine(f.shape);
  info.text = !d.textBoxes.empty();
  info.wrap = WrapOf(d);
  info.cx = d.cx;
  info.cy = d.cy;
  const double rot = AttrInt(Child(spPr, kA, "xfrm"), "rot") / 60000.0;
  info.rotation = std::fmod(std::fmod(rot, 360.0) + 360.0, 360.0);
  info.noFill = static_cast<bool>(Child(spPr, kA, "noFill"));
  info.fill = ColorOf(Child(spPr, kA, "solidFill"), doc);
  pugi::xml_node ln = Child(spPr, kA, "ln");
  info.noOutline = static_cast<bool>(Child(ln, kA, "noFill"));
  info.outline = ColorOf(Child(ln, kA, "solidFill"), doc);
  info.outlineWidth = static_cast<double>(AttrInt(ln, "w")) / kEmuPerPoint;
  return info;
}

Result Shapes::Resize(uint32_t drawing, Emu cx, Emu cy) {
  return ChangeShape(editor_, drawing, "Resize shape", [&](ShapeFrame& f, std::u16string&) {
    const bool line = IsLine(f.shape);
    SetGeometry(f, Side(cx.Value(), line), Side(cy.Value(), line), RotationOf(f));
    return true;
  });
}

Result Shapes::Rotate(uint32_t drawing, double degrees) {
  if (!std::isfinite(degrees)) return Fail(u"That isn't an angle.");
  const double d = std::fmod(std::fmod(degrees, 360.0) + 360.0, 360.0);
  const int64_t rot = std::llround(d * 60000) % 21600000;
  return ChangeShape(editor_, drawing, "Rotate shape", [&](ShapeFrame& f, std::u16string&) {
    pugi::xml_node extent = Child(f.root, kWp, "extent");
    const bool line = IsLine(f.shape);
    SetGeometry(f, Side(AttrInt(extent, "cx", kMinEmu), line), Side(AttrInt(extent, "cy", kMinEmu), line), rot);
    return true;
  });
}

Result Shapes::Move(uint32_t drawing, Emu x, Emu y) {
  return ChangeShape(editor_, drawing, "Move shape", [&](ShapeFrame& f, std::u16string& why) {
    if (pkg::LocalNameOf(f.root) != "anchor") {
      why = u"A shape in line with the text moves with it.";
      return false;
    }
    SetPosition(f, std::clamp<int64_t>(x.Value(), -kMaxEmu, kMaxEmu), std::clamp<int64_t>(y.Value(), -kMaxEmu, kMaxEmu));
    return true;
  });
}

Result Shapes::SetWrap(uint32_t drawing, Wrap wrap, Emu x, Emu y) {
  const Document& doc = editor_.Doc();
  return ChangeShape(editor_, drawing, wrap == Wrap::Inline ? "In line with text" : "Wrap text", [&](ShapeFrame& f, std::u16string& why) {
    const bool inlineNow = pkg::LocalNameOf(f.root) == "inline";
    if (wrap == Wrap::Inline) return inlineNow || ToInline(f, why);
    if (inlineNow) ToAnchor(f, doc, wrap, x.Value(), y.Value());
    else SetWrapXml(f, wrap);
    return true;
  });
}

Result Shapes::SetFill(uint32_t drawing, std::optional<uint32_t> rgb) {
  return ChangeShape(editor_, drawing, "Shape fill", [&](ShapeFrame& f, std::u16string& why) {
    if (IsLine(f.shape)) {
      why = u"A line has no fill. Change its outline.";
      return false;
    }
    SetFillXml(f, rgb);
    return true;
  });
}

Result Shapes::SetOutline(uint32_t drawing, std::optional<uint32_t> rgb, double points) {
  return ChangeShape(editor_, drawing, "Shape outline", [&](ShapeFrame& f, std::u16string&) {
    SetOutlineXml(f, rgb, points);
    return true;
  });
}

Result Shapes::AddText(uint32_t drawing) {
  Document& doc = const_cast<Document&>(editor_.Doc());
  if (drawing < doc.Drawings().size() && !doc.Drawings()[drawing].textBoxes.empty()) {
    Result r;
    r.caret = doc.Drawings()[drawing].pos;
    return r;  // it has text already
  }
  return ChangeShape(
      editor_, drawing, "Add text",
      [&](ShapeFrame& f, std::u16string& why) {
        if (IsLine(f.shape)) {
          why = u"A line can't hold text.";
          return false;
        }
        if (Child(f.shape, kWps, "txbx") || Child(f.shape, kWps, "linkedTxbx")) {
          why = u"This shape's text can't be edited here.";
          return false;
        }
        const std::string wps = PrefixOf(f.shape);
        const std::string w = PrefixFor(f.shape, f.dom.Root(), kW, "w");
        pugi::xml_node body = Child(f.shape, kWps, "bodyPr");
        if (!body) body = f.shape.append_child((wps + "bodyPr").c_str());
        pugi::xml_node txbx = f.shape.insert_child_before((wps + "txbx").c_str(), body);
        txbx.append_child((w + "txbxContent").c_str()).append_child((w + "p").c_str());
        if (!body.attribute("anchor")) body.append_attribute("anchor") = "ctr";
        return true;
      },
      [&](Drawing& now) {
        const uint32_t s = NewTextStory(editor_, doc, now.part, drawing);
        if (s == kNone) return false;
        now.textBoxes = {s};
        return true;
      });
}

uint32_t Shapes::TextStory(uint32_t drawing) const {
  const Document& doc = editor_.Doc();
  if (drawing >= doc.Drawings().size() || doc.Drawings()[drawing].textBoxes.empty()) return kNone;
  return doc.Drawings()[drawing].textBoxes.front();
}

}  // namespace fw::doc::edit
