#include <algorithm>
#include <cmath>
#include <set>

#include "core/Strings.h"
#include "draw/Xml.h"
#include "slide/render/SlideRender.h"
#include "slide/render/Table.h"
#include "slide/render/TextBody.h"
#include "text/Fonts.h"

namespace fw::slide::render {

using namespace draw::xml;

namespace {

// A part's pictures and parts, with the slide's theme and color map.
class PartResources : public draw::DrawingResources {
 public:
  PartResources(draw::DrawingResources* inner, std::shared_ptr<const draw::Theme> theme, const draw::ColorMap& map)
      : inner_(inner), theme_(theme ? std::move(theme) : draw::Theme::Default()), map_(map) {}
  std::shared_ptr<const draw::Theme> GetTheme() override { return theme_; }
  const draw::ColorMap& GetColorMap() override { return map_; }
  std::shared_ptr<const draw::Image> GetImage(std::string_view relId) override {
    return inner_ ? inner_->GetImage(relId) : nullptr;
  }
  pugi::xml_node GetPartXml(std::string_view relId) override { return inner_ ? inner_->GetPartXml(relId) : pugi::xml_node(); }

 private:
  draw::DrawingResources* inner_;
  std::shared_ptr<const draw::Theme> theme_;
  draw::ColorMap map_;
};

pugi::xml_node P(pugi::xml_node parent, std::string_view local) { return Child(parent, kP, local); }

// The non-visual properties' p:nvPr of a shape-tree element (p:nvSpPr, p:nvPicPr…).
pugi::xml_node NvPr(pugi::xml_node shape) {
  pugi::xml_node nvPr;
  ForEachChild(shape, [&](pugi::xml_node c) {
    if (LocalNameOf(c).starts_with("nv") && NamespaceOf(c) == kP) nvPr = P(c, "nvPr");
    return !nvPr;
  });
  return nvPr;
}

struct Ph {
  pugi::xml_node node;  // the shape
  std::string type;     // ST_PlaceholderType; "obj" when unstated
  uint32_t idx = 0;
  bool hasIdx = false;
};

bool PhOf(pugi::xml_node shape, Ph& out) {
  pugi::xml_node ph = P(NvPr(shape), "ph");
  if (!ph) return false;
  out.node = shape;
  out.type = ph.attribute("type") ? ph.attribute("type").value() : "obj";
  out.hasIdx = static_cast<bool>(ph.attribute("idx"));
  out.idx = static_cast<uint32_t>(std::clamp(Number(ph, "idx", 0), 0.0, 4294967295.0));
  return true;
}

std::vector<Ph> Placeholders(pugi::xml_node root) {
  std::vector<Ph> out;
  ForEachChild(P(P(root, "cSld"), "spTree"), [&](pugi::xml_node c) {
    Ph ph;
    if (PhOf(c, ph)) out.push_back(ph);
    return out.size() < 1000;
  });
  return out;
}

bool IsTitle(std::string_view t) { return t == "title" || t == "ctrTitle"; }
bool IsFooterKind(std::string_view t) { return t == "dt" || t == "ftr" || t == "sldNum" || t == "hdr"; }

// The layout's placeholder a slide's inherits from: by index, else by type.
pugi::xml_node MatchLayout(const Ph& ph, const std::vector<Ph>& layout) {
  if (ph.hasIdx && ph.idx != 0)
    for (const Ph& l : layout)
      if (l.hasIdx && l.idx == ph.idx) return l.node;
  for (const Ph& l : layout)
    if (l.type == ph.type && (!ph.hasIdx || !l.hasIdx || l.idx == ph.idx || IsTitle(ph.type) || IsFooterKind(ph.type)))
      return l.node;
  if (IsTitle(ph.type))
    for (const Ph& l : layout)
      if (IsTitle(l.type)) return l.node;
  return {};
}

// The master's placeholder of the same kind: title, body, or the footer placeholders by type.
pugi::xml_node MatchMaster(std::string_view type, const std::vector<Ph>& master) {
  const std::string_view want = IsTitle(type) ? "title" : IsFooterKind(type) || type == "sldImg" ? type : "body";
  for (const Ph& m : master)
    if (m.type == want || (want == "title" && IsTitle(m.type))) return m.node;
  return {};
}

// The text style a placeholder's text starts from in the master's p:txStyles.
pugi::xml_node MasterStyle(pugi::xml_node master, std::string_view type, bool placeholder) {
  if (pugi::xml_node notes = P(master, "notesStyle")) return placeholder && type == "body" ? notes : pugi::xml_node();
  pugi::xml_node styles = P(master, "txStyles");
  if (!placeholder || IsFooterKind(type)) return P(styles, "otherStyle");
  return P(styles, IsTitle(type) ? "titleStyle" : "bodyStyle");
}

pugi::xml_node FirstChild(std::initializer_list<pugi::xml_node> parents, std::initializer_list<std::string_view> names,
                          std::string_view ns) {
  for (pugi::xml_node p : parents) {
    pugi::xml_node found;
    ForEachChild(p, [&](pugi::xml_node c) {
      if (NamespaceOf(c) == ns)
        for (std::string_view n : names)
          if (LocalNameOf(c) == n) found = c;
      return !found;
    });
    if (found) return found;
  }
  return {};
}

pugi::xml_node SpPr(pugi::xml_node shape) { return P(shape, "spPr"); }
pugi::xml_node TxBody(pugi::xml_node shape) { return P(shape, "txBody"); }

// A placeholder with what it inherits written into it: the slide's shape, with the layout's and
// master's placement, geometry, fill, line, effects, style and text body settings where it has
// none of its own. Its text is the slide's only: a layout's prompt text is never drawn.
pugi::xml_node MergePlaceholder(pugi::xml_node into, pugi::xml_node s, pugi::xml_node l, pugi::xml_node m) {
  pugi::xml_node out = into.append_child(s.name());
  for (pugi::xml_attribute a : s.attributes()) out.append_attribute(a.name()) = a.value();
  const std::string_view kind = LocalNameOf(s);
  ForEachChild(s, [&](pugi::xml_node c) {
    if (LocalNameOf(c).starts_with("nv")) out.append_copy(c);
    return true;
  });
  // spPr: each property from the nearest part that has it.
  const pugi::xml_node sp[3] = {SpPr(s), SpPr(l), SpPr(m)};
  const std::string prefix = [&] {
    std::string_view n = sp[0] ? sp[0].name() : std::string_view(s.name());
    size_t colon = n.find(':');
    return colon == std::string_view::npos ? std::string() : std::string(n.substr(0, colon + 1));
  }();
  pugi::xml_node spPr = out.append_child((prefix + "spPr").c_str());
  if (sp[0])
    for (pugi::xml_attribute a : sp[0].attributes()) spPr.append_attribute(a.name()) = a.value();
  if (kind == "pic")
    if (pugi::xml_node blip = P(s, "blipFill")) out.insert_copy_before(blip, spPr);
  const std::initializer_list<std::string_view> fills = {"noFill", "solidFill", "gradFill", "blipFill", "pattFill", "grpFill"};
  for (const auto& names : {std::initializer_list<std::string_view>{"xfrm"}, std::initializer_list<std::string_view>{"prstGeom", "custGeom"},
                            fills, std::initializer_list<std::string_view>{"ln"}, std::initializer_list<std::string_view>{"effectLst"}}) {
    if (pugi::xml_node c = FirstChild({sp[0], sp[1], sp[2]}, names, kA)) spPr.append_copy(c);
  }
  if (pugi::xml_node style = FirstChild({s, l, m}, {"style"}, kP)) out.append_copy(style);
  // The text body: bodyPr's attributes from the master up, its autofit from the nearest that says.
  const pugi::xml_node bodies[3] = {TxBody(s), TxBody(l), TxBody(m)};
  if (kind == "sp") {
    pugi::xml_node tx = out.append_child((prefix + "txBody").c_str());
    pugi::xml_node bodyPr = tx.append_child("a:bodyPr");
    for (int i = 2; i >= 0; --i)
      for (pugi::xml_attribute a : A(bodies[i], "bodyPr").attributes()) {
        pugi::xml_attribute have = bodyPr.attribute(a.name());
        (have ? have : bodyPr.append_attribute(a.name())) = a.value();
      }
    if (pugi::xml_node fit = FirstChild({A(bodies[0], "bodyPr"), A(bodies[1], "bodyPr"), A(bodies[2], "bodyPr")},
                                        {"normAutofit", "spAutoFit", "noAutofit"}, kA))
      bodyPr.append_copy(fit);
    if (pugi::xml_node list = A(bodies[0], "lstStyle")) tx.append_copy(list);
    ForEachChild(bodies[0], [&](pugi::xml_node p) {
      if (LocalNameOf(p) == "p") tx.append_copy(p);
      return true;
    });
  }
  // Anything else of the slide's (a graphic frame's graphic, p:xfrm).
  ForEachChild(s, [&](pugi::xml_node c) {
    std::string_view k = LocalNameOf(c);
    if (!k.starts_with("nv") && k != "spPr" && k != "style" && k != "txBody" && k != "blipFill") out.append_copy(c);
    return true;
  });
  return out;
}

bool IsTable(pugi::xml_node shape) {
  if (LocalNameOf(shape) != "graphicFrame") return false;
  return std::string_view(A(A(shape, "graphic"), "graphicData").attribute("uri").value()) ==
         "http://schemas.openxmlformats.org/drawingml/2006/table";
}

draw::Rect Bounds(const draw::Matrix& m, draw::Size size) {
  const draw::Point pts[4] = {m.Apply({0, 0}), m.Apply({size.width, 0}), m.Apply({0, size.height}),
                              m.Apply({size.width, size.height})};
  draw::Rect r{pts[0].x, pts[0].y, pts[0].x, pts[0].y};
  for (const draw::Point& q : pts)
    r = {std::min(r.left, q.x), std::min(r.top, q.y), std::max(r.right, q.x), std::max(r.bottom, q.y)};
  return r;
}

draw::Rect Transformed(const draw::Matrix& m, const draw::Rect& r) {
  draw::Rect b = Bounds(draw::Matrix::Translate(r.left, r.top) * m, {r.Width(), r.Height()});
  return b;
}

}  // namespace

struct SlideRender::Impl {
  pugi::xml_document scratch;  // merged placeholders and backgrounds
  std::unique_ptr<PartResources> res[3];
  struct Item {
    uint32_t id = 0;
    bool inherited = false, background = false;
    std::unique_ptr<draw::Drawing> drawing;
    std::vector<TextBlock> texts;  // one per drawing text box
    std::unique_ptr<Table> table;
    draw::Rect frame;  // tables: the frame, slide points
    float rotation = 0;
  };
  std::vector<Item> items;
};

class Hooks : public draw::DrawHooks {
 public:
  Hooks(const SlideRender::Impl::Item& item, DrawFilter* filter) : item_(item), filter_(filter) {}
  bool DrawShape(uint32_t id) override { return !filter_ || filter_->Shape(item_.inherited ? 0 : id); }
  bool DrawBody(uint32_t id) override { return !filter_ || filter_->ShapeBody(item_.inherited ? 0 : id); }
  void DrawText(draw::Painter& painter, size_t index) override {
    if (index >= item_.texts.size()) return;
    const uint32_t id = item_.inherited ? 0 : item_.drawing->TextBoxes()[index].shape;
    item_.texts[index].Draw(painter, filter_, id);
  }

 private:
  const SlideRender::Impl::Item& item_;
  DrawFilter* filter_;
};

struct Builder {
  const SlideSource& src;
  text::Fonts& fonts;
  SlideRender& out;
  SlideRender::Impl& impl;
  pugi::xml_node scratchRoot;
  std::vector<Ph> layoutPh, masterPh;
  draw::ColorContext colors;

  Builder(const SlideSource& s, text::Fonts& f, SlideRender& o) : src(s), fonts(f), out(o), impl(*o.impl_) {}

  void Run() {
    out.size_ = src.size;
    // The scratch document's root declares the parts' namespaces, so copied elements resolve.
    scratchRoot = impl.scratch.append_child("p:scratch");
    std::set<std::string> declared;
    for (pugi::xml_node root : {src.slide, src.layout, src.master})
      for (pugi::xml_attribute a : root.attributes()) {
        std::string_view n = a.name();
        if ((n == "xmlns" || n.starts_with("xmlns:")) && declared.insert(std::string(n)).second)
          scratchRoot.append_attribute(a.name()) = a.value();
      }
    for (auto [prefix, uri] : {std::pair{"xmlns:a", kA}, std::pair{"xmlns:p", kP}, std::pair{"xmlns:r", kR}})
      if (declared.insert(prefix).second) scratchRoot.append_attribute(prefix) = std::string(uri).c_str();
    draw::DrawingResources* parts[3] = {src.slideResources, src.layoutResources, src.masterResources};
    for (int i = 0; i < 3; ++i) impl.res[i] = std::make_unique<PartResources>(parts[i], src.theme, src.colorMap);
    colors.theme = impl.res[0]->GetTheme().get();
    colors.colorMap = &impl.res[0]->GetColorMap();
    layoutPh = Placeholders(src.layout);
    masterPh = Placeholders(src.master);

    Background();
    const bool slideShowsMaster = Bool(src.slide, "showMasterSp", true);
    if (slideShowsMaster && Bool(src.layout, "showMasterSp", true)) Tree(src.master, 2);
    if (slideShowsMaster) Tree(src.layout, 1);
    Tree(src.slide, 0);
    Finish();
  }

  void Background() {
    pugi::xml_node bg;
    int part = 0;
    for (pugi::xml_node root : {src.slide, src.layout, src.master}) {
      bg = P(P(root, "cSld"), "bg");
      if (bg) break;
      ++part;
    }
    if (!bg) return;
    // A rectangle the size of the slide, filled as the background says.
    pugi::xml_node sp = scratchRoot.append_child("p:sp");
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
    ext.append_attribute("cx") = std::to_string(std::llround(src.size.width * kEmuPerPoint)).c_str();
    ext.append_attribute("cy") = std::to_string(std::llround(src.size.height * kEmuPerPoint)).c_str();
    spPr.append_child("a:prstGeom").append_attribute("prst") = "rect";
    if (pugi::xml_node bgPr = P(bg, "bgPr")) {
      ForEachChild(bgPr, [&](pugi::xml_node f) {
        if (NamespaceOf(f) == kA && LocalNameOf(f) != "extLst") spPr.append_copy(f);
        return true;
      });
    } else if (pugi::xml_node ref = P(bg, "bgRef")) {
      pugi::xml_node style = sp.append_child("p:style");
      pugi::xml_node ln = style.append_child("a:lnRef");
      ln.append_attribute("idx") = "0";
      pugi::xml_node fill = style.append_child("a:fillRef");
      fill.append_attribute("idx") = ref.attribute("idx").value();
      ForEachChild(ref, [&](pugi::xml_node col) {
        fill.append_copy(col);
        return false;
      });
      style.append_child("a:effectRef").append_attribute("idx") = "0";
      style.append_child("a:fontRef").append_attribute("idx") = "none";
    }
    pugi::xml_node ln = spPr.child("a:ln") ? spPr.child("a:ln") : spPr.append_child("a:ln");
    ln.append_child("a:noFill");
    SlideRender::Impl::Item item;
    item.background = item.inherited = true;
    item.drawing = draw::Drawing::FromShapeTree(sp, *impl.res[part]);
    if (item.drawing) impl.items.push_back(std::move(item));
  }

  // The shapes of one part: 0 the slide (all of them), 1 the layout and 2 the master (their own
  // shapes, not their placeholders).
  void Tree(pugi::xml_node root, int part) {
    pugi::xml_node tree = P(P(root, "cSld"), "spTree");
    ForEachChild(tree, [&](pugi::xml_node c) {
      const std::string_view k = LocalNameOf(c);
      if (k != "sp" && k != "pic" && k != "grpSp" && k != "graphicFrame" && k != "cxnSp") return true;
      Ph ph;
      const bool placeholder = PhOf(c, ph);
      if (part != 0 && placeholder) return true;
      Shape(c, part, placeholder ? &ph : nullptr);
      return impl.items.size() < 20000;
    });
  }

  void Shape(pugi::xml_node node, int part, const Ph* ph) {
    pugi::xml_node el = node, layoutNode, masterNode;
    if (ph) {
      layoutNode = MatchLayout(*ph, layoutPh);
      Ph lph;
      const std::string type = layoutNode && PhOf(layoutNode, lph) ? lph.type : ph->type;
      masterNode = MatchMaster(type, masterPh);
      if (type == "sldImg") return;  // the notes page's slide picture: the caller draws the slide
      el = MergePlaceholder(scratchRoot, node, layoutNode, masterNode);
    }
    // What its text inherits.
    TextContext ctx;
    ctx.colors = colors;
    ctx.slideNumber = src.number;
    ctx.resources = impl.res[part].get();
    if (src.defaultTextStyle) ctx.lists.push_back(src.defaultTextStyle);
    if (pugi::xml_node style = MasterStyle(src.master, ph ? ph->type : "", ph != nullptr)) ctx.lists.push_back(style);
    if (masterNode) ctx.lists.push_back(A(TxBody(masterNode), "lstStyle"));
    if (layoutNode) ctx.lists.push_back(A(TxBody(layoutNode), "lstStyle"));

    SlideRender::Impl::Item item;
    item.inherited = part != 0;
    item.id = static_cast<uint32_t>(Number(P(FirstChild({el}, {"nvSpPr", "nvPicPr", "nvGrpSpPr", "nvGraphicFramePr", "nvCxnSpPr"}, kP), "cNvPr"), "id", 0));
    if (IsTable(el)) {
      pugi::xml_node xfrm = P(el, "xfrm");
      const float x = EmuToPt(Number(A(xfrm, "off"), "x", 0)), y = EmuToPt(Number(A(xfrm, "off"), "y", 0));
      const float w = std::max(0.f, EmuToPt(Number(A(xfrm, "ext"), "cx", 0))), h = std::max(0.f, EmuToPt(Number(A(xfrm, "ext"), "cy", 0)));
      if (Bool(P(FirstChild({el}, {"nvGraphicFramePr"}, kP), "cNvPr"), "hidden", false)) return;
      item.table = Table::Read(A(A(A(el, "graphic"), "graphicData"), "tbl"), {w, h}, src.tableStyles, ctx, fonts);
      if (!item.table) return;
      item.frame = {x, y, x + w, y + h};
      item.rotation = static_cast<float>(Number(xfrm, "rot", 0) / 60000.0);
      impl.items.push_back(std::move(item));
      return;
    }
    item.drawing = draw::Drawing::FromShapeTree(el, *impl.res[part]);
    if (!item.drawing || item.drawing->IsHidden()) return;
    for (const draw::TextBox& box : item.drawing->TextBoxes()) {
      TextContext c = ctx;
      // A shape inside a group isn't the placeholder: it inherits like any other shape.
      if (box.shape != item.id && ph) {
        c.lists.clear();
        if (src.defaultTextStyle) c.lists.push_back(src.defaultTextStyle);
        c.lists.push_back(MasterStyle(src.master, "", false));
      }
      c.fontColor = box.fontColor;
      c.fontFace = box.fontFace;
      item.texts.emplace_back().Build(fonts, box, c);
    }
    impl.items.push_back(std::move(item));
  }

  void Finish() {
    for (const SlideRender::Impl::Item& item : impl.items) {
      if (item.background) continue;
      if (item.table) {
        const draw::Size size = item.table->Extent();
        const float cx = (item.frame.left + item.frame.right) / 2, cy = (item.frame.top + item.frame.bottom) / 2;
        const draw::Matrix m = draw::Matrix::Translate(item.frame.left, item.frame.top) *
                               draw::Matrix::Translate(-cx, -cy) * draw::Matrix::Rotate(item.rotation) * draw::Matrix::Translate(cx, cy);
        ShapeInfo info;
        info.id = item.inherited ? 0 : item.id;
        info.size = size;
        info.transform = m;
        info.bounds = Bounds(m, size);
        info.fromMaster = item.inherited;
        out.shapes_.push_back(info);
        int i = 0;
        for (const draw::Rect& r : item.table->ParagraphBounds()) out.paragraphs_.push_back({info.id, i++, Transformed(m, r)});
        for (const TextBlock::Link& l : item.table->Links()) out.links_.push_back({info.id, Transformed(m, l.bounds), l.relId, l.action});
        if (!item.inherited) Append(item.table->PlainText());
        continue;
      }
      const draw::Rect b = item.drawing->Bounds();
      const draw::Matrix place = draw::Matrix::Translate(b.left, b.top);
      std::vector<draw::Drawing::Part> parts = item.drawing->Parts();
      for (const draw::Drawing::Part& p : parts) {
        ShapeInfo info;
        info.id = item.inherited ? 0 : p.id;
        info.size = p.size;
        info.transform = p.transform * place;
        info.bounds = Bounds(info.transform, p.size);
        info.group = p.group;
        info.fromMaster = item.inherited;
        out.shapes_.push_back(info);
      }
      const std::vector<draw::TextBox>& boxes = item.drawing->TextBoxes();
      for (size_t t = 0; t < boxes.size() && t < item.texts.size(); ++t) {
        const draw::Matrix m = boxes[t].transform * place;
        const uint32_t id = item.inherited ? 0 : boxes[t].shape;
        int i = 0;
        for (const draw::Rect& r : item.texts[t].ParagraphBounds()) out.paragraphs_.push_back({id, i++, Transformed(m, r)});
        for (const TextBlock::Link& l : item.texts[t].Links()) out.links_.push_back({id, Transformed(m, l.bounds), l.relId, l.action});
        if (!item.inherited) Append(item.texts[t].PlainText());
      }
    }
    // The slide's own shapes' nodes and names, for the first part of each item.
    ForEachChild(P(P(src.slide, "cSld"), "spTree"), [&](pugi::xml_node c) {
      pugi::xml_node cNvPr = P(FirstChild({c}, {"nvSpPr", "nvPicPr", "nvGrpSpPr", "nvGraphicFramePr", "nvCxnSpPr"}, kP), "cNvPr");
      const uint32_t id = static_cast<uint32_t>(Number(cNvPr, "id", 0));
      for (ShapeInfo& s : out.shapes_)
        if (!s.fromMaster && s.id == id && !s.node) {
          s.node = c;
          s.name = cNvPr.attribute("name").value();
          Ph ph;
          s.placeholder = PhOf(c, ph);
          break;
        }
      return true;
    });
    for (const text::Substitution& s : fonts.Substitutions())
      if (std::find(out.missingFonts_.begin(), out.missingFonts_.end(), s.requested) == out.missingFonts_.end())
        out.missingFonts_.push_back(s.requested);
  }

  void Append(const std::wstring& t) {
    if (t.empty()) return;
    if (!out.text_.empty()) out.text_ += L'\n';
    out.text_ += t;
  }
};

SlideRender::SlideRender() : impl_(std::make_unique<Impl>()) {}
SlideRender::~SlideRender() = default;

std::unique_ptr<SlideRender> SlideRender::Make(const SlideSource& source, text::Fonts& fonts) {
  std::unique_ptr<SlideRender> s(new SlideRender());
  if (!source.slide) return s;
  Builder(source, fonts, *s).Run();
  return s;
}

void SlideRender::Draw(draw::Painter& p, const draw::Rect& frame, DrawFilter* filter) const {
  if (size_.width <= 0 || size_.height <= 0) return;
  p.Save();
  p.Transform(draw::Matrix::Scale(frame.Width() / size_.width, frame.Height() / size_.height) *
              draw::Matrix::Translate(frame.left, frame.top));
  // Nothing shows outside the slide, as in PowerPoint's show and PDF.
  p.ClipRect({0, 0, size_.width, size_.height});
  bool background = false;
  for (const Impl::Item& item : impl_->items) {
    if (item.background) {
      background = true;
      if (!filter || filter->Background()) item.drawing->Draw(p, item.drawing->Bounds());
      continue;
    }
    if (item.table) {
      const uint32_t id = item.inherited ? 0 : item.id;
      if (filter && !filter->Shape(id)) continue;
      p.Save();
      const draw::Rect& f = item.frame;
      const float cx = (f.left + f.right) / 2, cy = (f.top + f.bottom) / 2;
      p.Transform(draw::Matrix::Translate(f.left, f.top) * draw::Matrix::Translate(-cx, -cy) * draw::Matrix::Rotate(item.rotation) *
                  draw::Matrix::Translate(cx, cy));
      item.table->Draw(p, filter, id);
      p.Restore();
      continue;
    }
    Hooks hooks(item, filter);
    item.drawing->Draw(p, item.drawing->Bounds(), hooks);
  }
  (void)background;
  p.Restore();
}

}  // namespace fw::slide::render
