// Reads DrawingML shapes (ECMA-376 Part 1, §20.1, §20.2, §20.4, §19.3) into the shape model.
#include <algorithm>
#include <cmath>
#include <string>

#include "draw/Shapes.h"
#include "draw/Xml.h"

namespace draw {
namespace {

using namespace xml;

constexpr std::string_view kChartUri = "http://schemas.openxmlformats.org/drawingml/2006/chart";
ChartReader g_chartReader = nullptr;  // SetChartReader
constexpr std::string_view kDiagramUri = "http://schemas.openxmlformats.org/drawingml/2006/diagram";

float Percent(pugi::xml_node n, const char* attr, double def) {
  return static_cast<float>(Number(n, attr, def) / 100000.0);
}
float Angle(pugi::xml_node n, const char* attr) { return static_cast<float>(Number(n, attr, 0) / 60000.0); }
float Emu(pugi::xml_node n, const char* attr, double def = 0) { return EmuToPt(Number(n, attr, def)); }

// A child in any of the shape-tree namespaces (p:, xdr:, dsp:, wps:, wpg:, pic:), by local name.
pugi::xml_node Part(pugi::xml_node parent, std::string_view local) {
  pugi::xml_node found;
  ForEachChild(parent, [&](pugi::xml_node c) {
    if (LocalNameOf(c) == local) found = c;
    return !found;
  });
  return found;
}

ArrowHead ReadArrow(pugi::xml_node n) {
  ArrowHead a;
  if (!n) return a;
  std::string_view type = n.attribute("type").value();
  if (type == "triangle") a.type = ArrowHead::Type::Triangle;
  else if (type == "stealth") a.type = ArrowHead::Type::Stealth;
  else if (type == "diamond") a.type = ArrowHead::Type::Diamond;
  else if (type == "oval") a.type = ArrowHead::Type::Oval;
  else if (type == "arrow") a.type = ArrowHead::Type::Arrow;
  auto size = [](std::string_view v) -> uint8_t { return v == "sm" ? 0 : v == "lg" ? 2 : 1; };
  a.width = size(n.attribute("w").value());
  a.length = size(n.attribute("len").value());
  return a;
}

std::vector<float> PresetDash(std::string_view v) {
  if (v == "dot" || v == "sysDot") return {1, 1};
  if (v == "dash") return {4, 3};
  if (v == "lgDash") return {8, 3};
  if (v == "dashDot") return {4, 3, 1, 3};
  if (v == "lgDashDot") return {8, 3, 1, 3};
  if (v == "lgDashDotDot") return {8, 3, 1, 3, 1, 3};
  if (v == "sysDash") return {3, 1};
  if (v == "sysDashDot") return {3, 1, 1, 1};
  if (v == "sysDashDotDot") return {3, 1, 1, 1, 1, 1};
  return {};
}

class Reader {
 public:
  explicit Reader(ReadContext& ctx) : ctx_(ctx) {}

  // A fill element (noFill, solidFill, gradFill, blipFill, pattFill, grpFill). False when `n`
  // isn't one.
  bool ReadFill(pugi::xml_node n, const ColorContext& colors, const Fill* groupFill, Fill& out) {
    if (NamespaceOf(n) != kA) return false;
    std::string_view kind = LocalNameOf(n);
    if (kind == "noFill") {
      out = Fill{};
    } else if (kind == "solidFill") {
      out = Fill{};
      std::optional<Color> c = ReadColor(n, colors);
      out.kind = c ? Fill::Kind::Solid : Fill::Kind::None;
      if (c) out.color = *c;
    } else if (kind == "gradFill") {
      out = Fill{};
      out.kind = Fill::Kind::Gradient;
      out.rotateWithShape = Bool(n, "rotWithShape", true);
      int count = 0;
      ForEachChild(A(n, "gsLst"), [&](pugi::xml_node gs) {
        if (std::optional<Color> c = ReadColor(gs, colors)) {
          out.stops.push_back({std::clamp(Percent(gs, "pos", 0), 0.f, 1.f), *c});
        }
        return ++count < 256;
      });
      std::stable_sort(out.stops.begin(), out.stops.end(),
                       [](const GradientStop& a, const GradientStop& b) { return a.offset < b.offset; });
      if (out.stops.empty()) out.kind = Fill::Kind::None;
      if (pugi::xml_node lin = A(n, "lin")) {
        out.angle = Angle(lin, "ang");
        out.scaled = Bool(lin, "scaled", false);
      } else if (pugi::xml_node path = A(n, "path")) {
        std::string_view p = path.attribute("path").value();
        out.shape = p == "circle" ? Fill::GradientShape::Circle
                    : p == "rect" ? Fill::GradientShape::Rect
                                  : Fill::GradientShape::Shape;
        pugi::xml_node to = A(path, "fillToRect");
        out.focus = {Percent(to, "l", 0), Percent(to, "t", 0), Percent(to, "r", 0), Percent(to, "b", 0)};
        pugi::xml_node tile = A(n, "tileRect");
        out.tile = {Percent(tile, "l", 0), Percent(tile, "t", 0), Percent(tile, "r", 0), Percent(tile, "b", 0)};
        for (float* f : {&out.tile.left, &out.tile.top, &out.tile.right, &out.tile.bottom}) *f = std::clamp(*f, -10.f, 1.f);
      } else {
        out.angle = 90;  // no shade given: top to bottom
      }
    } else if (kind == "blipFill") {
      out = Fill{};
      out.picture = ReadBlipFill(n, colors);
      out.kind = Fill::Kind::Picture;
    } else if (kind == "pattFill") {
      out = Fill{};
      out.kind = Fill::Kind::Pattern;
      out.pattern = PatternIndex(n.attribute("prst").value());
      out.color = ReadColor(A(n, "fgClr"), colors).value_or(Color{0, 0, 0, 1});
      out.background = ReadColor(A(n, "bgClr"), colors).value_or(Color{1, 1, 1, 1});
    } else if (kind == "grpFill") {
      out = groupFill ? *groupFill : Fill{};
    } else {
      return false;
    }
    return true;
  }

  // a:blipFill or pic:blipFill: the picture, its crop, placement and recoloring.
  std::shared_ptr<PictureFill> ReadBlipFill(pugi::xml_node n, const ColorContext& colors) {
    auto pic = std::make_shared<PictureFill>();
    pugi::xml_node blip = A(n, "blip");
    std::shared_ptr<const Image> raster;
    if (std::string_view id = RelId(blip, "embed"); !id.empty()) raster = ctx_.resources.GetImage(id);
    pic->image = raster;
    // Office stores SVG pictures in an extension, with a PNG copy as the blip itself.
    ForEachChild(A(blip, "extLst"), [&](pugi::xml_node ext) {
      if (pugi::xml_node svg = Child(ext, kAsvg, "svgBlip")) {
        std::string_view id = RelId(svg, "embed");
        std::shared_ptr<const Image> vector = id.empty() ? nullptr : ctx_.resources.GetImage(id);
        if (vector && (vector->Faithful() || !raster)) pic->image = vector;
        return false;
      }
      return true;
    });
    ForEachChild(blip, [&](pugi::xml_node e) {
      if (NamespaceOf(e) != kA) return true;
      std::string_view k = LocalNameOf(e);
      if (k == "alphaModFix") pic->alpha = std::clamp(Percent(e, "amt", 100000), 0.f, 1.f);
      else if (k == "grayscl") pic->grayscale = true;
      else if (k == "biLevel") pic->biLevel = std::clamp(Percent(e, "thresh", 50000), 0.f, 1.f);
      else if (k == "lum") {
        pic->brightness = std::clamp(Percent(e, "bright", 0), -1.f, 1.f);
        pic->contrast = std::clamp(Percent(e, "contrast", 0), -1.f, 1.f);
      } else if (k == "duotone") {
        std::vector<Color> two;
        ForEachChild(e, [&](pugi::xml_node c) {
          if (std::optional<Color> col = ReadColorElement(c, colors)) two.push_back(*col);
          return two.size() < 2;
        });
        if (two.size() == 2) {
          pic->duotone = true;
          pic->duotoneDark = two[0];
          pic->duotoneLight = two[1];
        }
      } else if (k == "clrChange") {
        std::optional<Color> from = ReadColor(A(e, "clrFrom"), colors), to = ReadColor(A(e, "clrTo"), colors);
        if (from && to) {
          pic->colorChange = true;
          pic->changeFrom = *from;
          pic->changeTo = *to;
        }
      }
      return true;
    });
    if (pugi::xml_node src = A(n, "srcRect")) {
      pic->crop = {Percent(src, "l", 0), Percent(src, "t", 0), Percent(src, "r", 0), Percent(src, "b", 0)};
    }
    if (pugi::xml_node tile = A(n, "tile")) {
      pic->tile = true;
      pic->tileX = Emu(tile, "tx");
      pic->tileY = Emu(tile, "ty");
      pic->tileScaleX = Percent(tile, "sx", 100000);
      pic->tileScaleY = Percent(tile, "sy", 100000);
      std::string_view flip = tile.attribute("flip").value();
      pic->mirrorX = flip == "x" || flip == "xy";
      pic->mirrorY = flip == "y" || flip == "xy";
      static constexpr std::string_view kAlign[] = {"tl", "t", "tr", "l", "ctr", "r", "bl", "b", "br"};
      std::string_view algn = tile.attribute("algn").value();
      for (uint8_t i = 0; i < 9; ++i)
        if (kAlign[i] == algn) pic->tileAlign = i;
    } else if (pugi::xml_node rect = A(A(n, "stretch"), "fillRect")) {
      pic->inset = {Percent(rect, "l", 0), Percent(rect, "t", 0), Percent(rect, "r", 0), Percent(rect, "b", 0)};
    }
    return pic;
  }

  // a:ln, on top of what `out` already has (from the style's line reference).
  void ReadLine(pugi::xml_node ln, const ColorContext& colors, const Fill* groupFill, Line& out) {
    if (!ln) return;
    if (ln.attribute("w")) out.width = std::clamp(Emu(ln, "w"), 0.f, 1000.f);
    std::string_view cap = ln.attribute("cap").value();
    if (cap == "rnd") out.cap = LineCap::Round;
    else if (cap == "sq") out.cap = LineCap::Square;
    else if (cap == "flat") out.cap = LineCap::Flat;
    out.inset = std::string_view(ln.attribute("algn").value()) == "in" ? true : ln.attribute("algn") ? false : out.inset;
    ForEachChild(ln, [&](pugi::xml_node c) {
      if (NamespaceOf(c) != kA) return true;
      std::string_view k = LocalNameOf(c);
      Fill f;
      if (ReadFill(c, colors, groupFill, f)) {
        out.fill = f;
      } else if (k == "prstDash") {
        out.dashes = PresetDash(c.attribute("val").value());
      } else if (k == "custDash") {
        out.dashes.clear();
        ForEachChild(c, [&](pugi::xml_node ds) {
          out.dashes.push_back(std::clamp(Percent(ds, "d", 100000), 0.01f, 1000.f));
          out.dashes.push_back(std::clamp(Percent(ds, "sp", 100000), 0.01f, 1000.f));
          return out.dashes.size() < 64;
        });
      } else if (k == "round") {
        out.join = LineJoin::Round;
      } else if (k == "bevel") {
        out.join = LineJoin::Bevel;
      } else if (k == "miter") {
        out.join = LineJoin::Miter;
        out.miterLimit = std::clamp(Percent(c, "lim", 800000), 1.f, 100.f);
      } else if (k == "headEnd") {
        out.head = ReadArrow(c);
      } else if (k == "tailEnd") {
        out.tail = ReadArrow(c);
      }
      return true;
    });
  }

  void ReadEffects(pugi::xml_node effectLst, const ColorContext& colors, OuterShadow& out) {
    out = OuterShadow{};
    pugi::xml_node s = A(effectLst, "outerShdw");
    if (!s) return;
    out.on = true;
    out.color = ReadColor(s, colors).value_or(Color{0, 0, 0, 0.5f});
    out.blur = std::clamp(Emu(s, "blurRad"), 0.f, 500.f);
    out.distance = std::clamp(Emu(s, "dist"), 0.f, 2000.f);
    out.direction = Angle(s, "dir");
    out.scaleX = std::clamp(Percent(s, "sx", 100000), -10.f, 10.f);
    out.scaleY = std::clamp(Percent(s, "sy", 100000), -10.f, 10.f);
    out.skewX = std::clamp(Angle(s, "kx"), -89.f, 89.f);
    out.skewY = std::clamp(Angle(s, "ky"), -89.f, 89.f);
    static constexpr std::string_view kAlign[] = {"tl", "t", "tr", "l", "ctr", "r", "bl", "b", "br"};
    std::string_view algn = s.attribute("algn").value();
    out.align = 6;
    for (uint8_t i = 0; i < 9; ++i)
      if (kAlign[i] == algn) out.align = i;
  }

  // a:xfrm (or p:xfrm on graphic frames): bounds, rotation, flips, and a group's child space.
  void ReadXfrm(pugi::xml_node xfrm, Shape& s) {
    if (!xfrm) return;
    pugi::xml_node off = A(xfrm, "off"), ext = A(xfrm, "ext");
    const float x = Emu(off, "x"), y = Emu(off, "y");
    s.bounds = {x, y, x + std::max(0.f, Emu(ext, "cx")), y + std::max(0.f, Emu(ext, "cy"))};
    s.rotation = Angle(xfrm, "rot");
    s.flipH = Bool(xfrm, "flipH", false);
    s.flipV = Bool(xfrm, "flipV", false);
    pugi::xml_node chOff = A(xfrm, "chOff"), chExt = A(xfrm, "chExt");
    if (chOff && chExt) {
      const float cx = Emu(chOff, "x"), cy = Emu(chOff, "y");
      s.childBounds = {cx, cy, cx + Emu(chExt, "cx"), cy + Emu(chExt, "cy")};
    } else {
      s.childBounds = s.bounds;
    }
  }

  void ReadGeometry(pugi::xml_node spPr, Shape& s) {
    if (pugi::xml_node prst = A(spPr, "prstGeom")) {
      s.geometry = Geometry::Preset(prst.attribute("prst").value());
      ForEachChild(A(prst, "avLst"), [&](pugi::xml_node gd) {
        std::string_view f = gd.attribute("fmla").value();
        if (f.starts_with("val ")) s.adjust.push_back({gd.attribute("name").value(), Number(f.substr(4), 0)});
        return s.adjust.size() < 32;
      });
    } else if (pugi::xml_node cust = A(spPr, "custGeom")) {
      auto g = std::make_shared<Geometry>();
      auto guides = [&](std::string_view list, bool adjust) {
        ForEachChild(A(cust, list), [&](pugi::xml_node gd) {
          if (adjust) g->AddAdjust(gd.attribute("name").value(), gd.attribute("fmla").value());
          else g->AddGuide(gd.attribute("name").value(), gd.attribute("fmla").value());
          return true;
        });
      };
      guides("avLst", true);
      guides("gdLst", false);
      ForEachChild(A(cust, "pathLst"), [&](pugi::xml_node path) {
        std::string_view fill = path.attribute("fill").value();
        const Geometry::PathFill pf = fill == "none"          ? Geometry::PathFill::None
                                      : fill == "lighten"     ? Geometry::PathFill::Lighten
                                      : fill == "lightenLess" ? Geometry::PathFill::LightenLess
                                      : fill == "darken"      ? Geometry::PathFill::Darken
                                      : fill == "darkenLess"  ? Geometry::PathFill::DarkenLess
                                                              : Geometry::PathFill::Normal;
        g->BeginPath(Number(path, "w", 0), Number(path, "h", 0), pf, Bool(path, "stroke", true));
        ForEachChild(path, [&](pugi::xml_node c) {
          std::string_view k = LocalNameOf(c);
          std::vector<pugi::xml_node> pts;
          ForEachChild(c, [&](pugi::xml_node pt) {
            pts.push_back(pt);
            return pts.size() < 3;
          });
          auto x = [&](size_t i) { return i < pts.size() ? pts[i].attribute("x").value() : ""; };
          auto y = [&](size_t i) { return i < pts.size() ? pts[i].attribute("y").value() : ""; };
          if (k == "moveTo") g->MoveTo(x(0), y(0));
          else if (k == "lnTo") g->LineTo(x(0), y(0));
          else if (k == "arcTo") {
            g->ArcTo(c.attribute("wR").value(), c.attribute("hR").value(), c.attribute("stAng").value(),
                     c.attribute("swAng").value());
          } else if (k == "quadBezTo") g->QuadTo(x(0), y(0), x(1), y(1));
          else if (k == "cubicBezTo") g->CubicTo(x(0), y(0), x(1), y(1), x(2), y(2));
          else if (k == "close") g->Close();
          return true;
        });
        return true;
      });
      if (pugi::xml_node r = A(cust, "rect")) {
        g->SetTextRect(r.attribute("l").value(), r.attribute("t").value(), r.attribute("r").value(),
                       r.attribute("b").value());
      }
      if (!g->Empty()) s.geometry = std::move(g);
    }
  }

  // The style matrix (p:style and its kin): the theme's line, fill and effect, in the reference's
  // color. spPr then overrides it.
  void ReadStyle(pugi::xml_node style, Shape& s) {
    if (!style || !ctx_.theme) return;
    auto refColor = [&](pugi::xml_node ref) {
      ColorContext c = ctx_.colors;
      c.placeholder = ReadColor(ref, ctx_.colors);
      return c;
    };
    if (pugi::xml_node ln = A(style, "lnRef")) {
      ColorContext c = refColor(ln);
      if (pugi::xml_node themed = ctx_.theme->Line(static_cast<int>(Number(ln, "idx", 0)))) {
        s.line.fill = Fill{};
        ReadLine(themed, c, nullptr, s.line);
      }
    }
    if (pugi::xml_node fill = A(style, "fillRef")) {
      ColorContext c = refColor(fill);
      pugi::xml_node themed = ctx_.theme->Fill(static_cast<int>(Number(fill, "idx", 0)));
      if (!themed || !ReadFill(themed, c, nullptr, s.fill)) s.fill = Fill{};
    }
    if (pugi::xml_node eff = A(style, "effectRef")) {
      ColorContext c = refColor(eff);
      if (pugi::xml_node themed = ctx_.theme->Effect(static_cast<int>(Number(eff, "idx", 0)))) {
        ReadEffects(A(themed, "effectLst"), c, s.shadow);
      }
    }
  }

  // spPr (and grpSpPr): placement, geometry, fill, line, effects.
  void ReadShapeProperties(pugi::xml_node spPr, Shape& s, const Fill* groupFill) {
    ReadXfrm(A(spPr, "xfrm"), s);
    ReadGeometry(spPr, s);
    ForEachChild(spPr, [&](pugi::xml_node c) {
      Fill f;
      if (ReadFill(c, ctx_.colors, groupFill, f)) s.fill = f;
      return true;
    });
    // A line element with no fill of its own keeps the style's.
    if (pugi::xml_node ln = A(spPr, "ln")) ReadLine(ln, ctx_.colors, groupFill, s.line);
    if (pugi::xml_node eff = A(spPr, "effectLst")) ReadEffects(eff, ctx_.colors, s.shadow);
  }

  void ReadNames(pugi::xml_node nv, Shape& s) {
    pugi::xml_node c = Part(nv, "cNvPr");
    s.name = c.attribute("name").value();
    s.description = c.attribute("descr").value();
    s.hidden = Bool(c, "hidden", false);
  }

  void ReadTextBody(pugi::xml_node el, Shape& s) {
    pugi::xml_node content, props;
    if (pugi::xml_node txbx = Child(el, kWps, "txbx")) {
      content = Child(txbx, kW, "txbxContent");
      props = Child(el, kWps, "bodyPr");
    } else if (pugi::xml_node linked = Child(el, kWps, "linkedTxbx")) {
      content = linked;  // the rest of a linked text box chain: its text flows in from the first box
      props = Child(el, kWps, "bodyPr");
    } else if (pugi::xml_node body = Part(el, "txBody")) {
      content = body;
      props = A(body, "bodyPr");
    }
    if (!content) return;
    auto t = std::make_shared<TextBody>();
    t->content = content;
    t->properties = props;
    t->insets = {Emu(props, "lIns", 91440), Emu(props, "tIns", 45720), Emu(props, "rIns", 91440),
                 Emu(props, "bIns", 45720)};
    TextBox& b = t->box;
    b.rotation = Angle(props, "rot");
    std::string_view vert = props.attribute("vert").value();
    b.flow = vert == "vert"                                      ? TextBox::Flow::Vertical
             : vert == "vert270"                                 ? TextBox::Flow::Vertical270
             : vert == "wordArtVert" || vert == "wordArtVertRtl" ? TextBox::Flow::Stacked
             : vert == "eaVert"                                  ? TextBox::Flow::EastAsian
             : vert == "mongolianVert"                           ? TextBox::Flow::Mongolian
                                                                 : TextBox::Flow::Horizontal;
    std::string_view anchor = props.attribute("anchor").value();
    b.anchor = anchor == "ctr"    ? TextBox::Anchor::Middle
               : anchor == "b"    ? TextBox::Anchor::Bottom
               : anchor == "just" ? TextBox::Anchor::Justified
               : anchor == "dist" ? TextBox::Anchor::Distributed
                                  : TextBox::Anchor::Top;
    b.anchorCenter = Bool(props, "anchorCtr", false);
    b.wrap = std::string_view(props.attribute("wrap").value()) != "none";
    b.upright = Bool(props, "upright", false);
    b.columns = std::clamp(static_cast<int>(Number(props, "numCol", 1)), 1, 16);
    b.columnSpacing = Emu(props, "spcCol");
    if (pugi::xml_node fit = A(props, "normAutofit")) {
      b.autofit = TextBox::Autofit::ShrinkText;
      b.fontScale = std::clamp(Percent(fit, "fontScale", 100000), 0.01f, 1.f);
      b.lineSpacingReduction = std::clamp(Percent(fit, "lnSpcReduction", 0), 0.f, 1.f);
    } else if (A(props, "spAutoFit")) {
      b.autofit = TextBox::Autofit::ResizeShape;
    }
    s.text = std::move(t);
  }

  bool ReadShape(pugi::xml_node el, Shape& s, const Fill* groupFill) {
    if (++ctx_.shapes > ReadContext::kMaxShapes) return false;
    std::string_view kind = LocalNameOf(el);
    if (kind == "sp" || kind == "wsp" || kind == "cxnSp") {
      s.kind = Shape::Kind::Shape;
      ReadNames(Part(el, kind == "cxnSp" ? "nvCxnSpPr" : "nvSpPr"), s);
      if (kind == "wsp") s.name.clear();  // Word names the drawing in wp:docPr
      ReadStyle(Part(el, "style"), s);
      ReadShapeProperties(Part(el, "spPr"), s, groupFill);
      ReadTextBody(el, s);
      return true;
    }
    if (kind == "pic") {
      s.kind = Shape::Kind::Picture;
      ReadNames(Part(el, "nvPicPr"), s);
      ReadStyle(Part(el, "style"), s);
      ReadShapeProperties(Part(el, "spPr"), s, groupFill);
      if (pugi::xml_node blipFill = Part(el, "blipFill")) s.picture = ReadBlipFill(blipFill, ctx_.colors);
      return true;
    }
    if (kind == "grpSp" || kind == "wgp" || kind == "wpc") {
      s.kind = Shape::Kind::Group;
      ReadNames(Part(el, "nvGrpSpPr"), s);
      if (kind == "wpc") {
        // A drawing canvas: its children are placed in its own coordinates.
        if (pugi::xml_node bg = Child(el, kWpc, "bg")) {
          ForEachChild(bg, [&](pugi::xml_node c) {
            ReadFill(c, ctx_.colors, nullptr, s.fill);
            return true;
          });
        }
      } else {
        ReadShapeProperties(Part(el, "grpSpPr"), s, groupFill);
      }
      if (ctx_.depth >= ReadContext::kMaxDepth) return true;
      ++ctx_.depth;
      const Fill fill = s.fill;
      ForEachChild(el, [&](pugi::xml_node c) {
        std::string_view k = LocalNameOf(c);
        if (k != "sp" && k != "wsp" && k != "cxnSp" && k != "pic" && k != "grpSp" && k != "graphicFrame") return true;
        Shape child;
        if (ReadShape(c, child, &fill)) s.children.push_back(std::move(child));
        return ctx_.shapes <= ReadContext::kMaxShapes;
      });
      --ctx_.depth;
      // A group's own fill is only for a:grpFill in its children.
      s.fill = Fill{};
      s.line = Line{};
      if (kind == "wpc") s.fill = fill;
      return true;
    }
    if (kind == "graphicFrame") {
      ReadNames(Part(el, "nvGraphicFramePr"), s);
      ReadXfrm(Part(el, "xfrm"), s);
      Shape inner;
      const Size size{s.bounds.Width(), s.bounds.Height()};
      if (ReadGraphicData(A(A(el, "graphic"), "graphicData"), size, ctx_, inner)) {
        inner.bounds = s.bounds;
        inner.rotation = s.rotation;
        inner.flipH = s.flipH;
        inner.flipV = s.flipV;
        if (inner.name.empty()) inner.name = s.name;
        if (inner.description.empty()) inner.description = s.description;
        s = std::move(inner);
      } else {
        s.kind = Shape::Kind::Placeholder;
      }
      return true;
    }
    return false;
  }

  bool ReadGraphic(pugi::xml_node data, Size extent, Shape& s) {
    std::string_view uri = data.attribute("uri").value();
    const Rect frame{0, 0, extent.width, extent.height};
    bool found = false;
    ForEachChild(data, [&](pugi::xml_node c) {
      std::string_view k = LocalNameOf(c);
      if (k == "pic" || k == "wsp" || k == "wgp" || k == "wpc" || k == "sp" || k == "grpSp" || k == "cxnSp") {
        found = ReadShape(c, s, nullptr);
      } else if (k == "oleObj") {
        // PowerPoint's embedded objects carry their preview as a picture.
        if (pugi::xml_node pic = Part(c, "pic")) found = ReadShape(pic, s, nullptr);
      } else if (k == "relIds" && uri == kDiagramUri) {
        found = ReadSmartArt(c, extent, s);
      } else if (k == "chart" && uri == kChartUri && g_chartReader) {
        if (pugi::xml_node space = ctx_.resources.GetPartXml(RelId(c, "id")))
          if (g_chartReader(space, extent, ctx_, s)) {
            s.kind = Shape::Kind::Shape;
            found = true;
          }
      }
      return !found;
    });
    if (!found) {
      // Charts (D5), tables in slides and other graphics: a placeholder of the same size.
      s = Shape{};
      s.kind = Shape::Kind::Placeholder;
      s.name = uri == kChartUri ? "Chart" : "";
    }
    // The frame's own size wins over what the graphic says (Word scales pictures this way).
    s.bounds = frame;
    if (s.kind == Shape::Kind::Group && s.childBounds.Width() == 0 && s.childBounds.Height() == 0) s.childBounds = frame;
    return true;
  }

  // SmartArt shows the drawing Office saved with it (dsp:drawing), found through the data part.
  bool ReadSmartArt(pugi::xml_node relIds, Size extent, Shape& s) {
    pugi::xml_node data = ctx_.resources.GetPartXml(RelId(relIds, "dm"));
    if (!data) return false;
    std::string_view drawingRel;
    ForEachChild(A(data, "extLst"), [&](pugi::xml_node ext) {
      if (pugi::xml_node m = Child(ext, kDsp, "dataModelExt")) drawingRel = m.attribute("relId").value();
      return drawingRel.empty();
    });
    pugi::xml_node drawing = drawingRel.empty() ? pugi::xml_node() : ctx_.resources.GetPartXml(drawingRel);
    pugi::xml_node tree = Child(drawing, kDsp, "spTree");
    if (!tree) return false;
    s.kind = Shape::Kind::Group;
    s.name = "SmartArt";
    s.childBounds = {0, 0, extent.width, extent.height};
    ForEachChild(tree, [&](pugi::xml_node c) {
      Shape child;
      std::string_view k = LocalNameOf(c);
      if ((k == "sp" || k == "grpSp" || k == "pic" || k == "cxnSp") && ReadShape(c, child, nullptr)) {
        s.children.push_back(std::move(child));
      }
      return ctx_.shapes <= ReadContext::kMaxShapes;
    });
    return true;
  }

 private:
  ReadContext& ctx_;
};

}  // namespace

void SetChartReader(ChartReader reader) { g_chartReader = reader; }

ReadContext::ReadContext(DrawingResources& r) : resources(r), theme(r.GetTheme()) {
  if (!theme) theme = Theme::Default();
  colors.theme = theme.get();
  colors.colorMap = &r.GetColorMap();
}

bool ReadDrawingMLShape(pugi::xml_node el, ReadContext& ctx, Shape& out, const Fill* groupFill) {
  return Reader(ctx).ReadShape(el, out, groupFill);
}

bool ReadGraphicData(pugi::xml_node graphicData, Size extent, ReadContext& ctx, Shape& out) {
  if (!graphicData) return false;
  return Reader(ctx).ReadGraphic(graphicData, extent, out);
}

}  // namespace draw
