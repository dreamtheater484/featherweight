// A .ppt drawing ([MS-PPT] 2.5.14 DrawingContainer: OfficeArt, [MS-ODRAW]) as PresentationML shapes:
// preset and freeform geometry, fills (solid, gradient, picture, pattern), lines with dashes and
// arrowheads, shadows, pictures with their crop, groups with their own coordinate space,
// placeholders, and text bodies from the client text boxes.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "draw/OfficeArt.h"
#include "slide/ppt/Internal.h"

namespace fw::slide::ppt::detail {
namespace {

std::string Num(long long v) { return std::to_string(v); }
long long Emu(double units) { return std::llround(units * kEmuPerUnit); }

struct Shape {
  uint16_t spt = 0;
  uint32_t id = 0, flags = 0;  // FSP: group 1, child 2, patriarch 4, deleted 8, OLE 0x10, flipH 0x40, flipV 0x80, background 0x400
  std::map<uint16_t, uint32_t> props;
  std::map<uint16_t, std::span<const uint8_t>> complex;
  bool anchored = false, grouped = false;
  double anchor[4] = {};  // left, top, right, bottom: master units, or the parent group's space
  double space[4] = {};   // FSPGR: a group's space for its shapes
  int placeholder = -1, phPosition = -1, phSize = 0;
  std::span<const uint8_t> textbox;
  std::span<const uint8_t> style9;  // the "___PPT9" tag's records: the text's PowerPoint 2000 additions
  std::string clickTarget;
};

// Arrays in complex properties (IMsoArray: count, allocated, element size) can store a length
// without their 6-byte header; those take the header too.
bool IsArray(uint16_t pid) {
  switch (pid) {
    case 0x0145: case 0x0146: case 0x0151: case 0x0152: case 0x0155: case 0x0156: case 0x0157: case 0x0197: case 0x0383:
    case 0x03A1:
      return true;
    default:
      return false;
  }
}

void ReadOpt(const Rec& r, Shape& s) {
  Bytes b(r.body);
  std::vector<std::pair<uint16_t, uint32_t>> complexes;
  for (uint16_t i = 0; i < r.inst && b.Ok(); ++i) {
    const uint16_t opid = b.U16();
    const uint32_t op = b.U32();
    if (!b.Ok()) break;
    const uint16_t pid = opid & 0x3FFF;
    if (opid & 0x8000) complexes.emplace_back(pid, op);
    s.props.emplace(pid, op);
  }
  for (const auto& [pid, len] : complexes) {
    size_t n = std::min<size_t>(len, b.Left());
    if (IsArray(pid) && b.Left() >= 6) {
      const size_t at = b.Pos();
      const auto head = r.body.subspan(at, 6);
      const uint16_t count = static_cast<uint16_t>(head[0] | head[1] << 8);
      uint16_t elem = static_cast<uint16_t>(head[4] | head[5] << 8);
      if (elem == 0xFFF0) elem = 4;
      const size_t whole = 6 + size_t{count} * elem;
      if (len + 6 == whole && whole <= b.Left()) n = whole;
    }
    s.complex.emplace(pid, b.Take(n));
  }
}

// A string property (UTF-16, null-terminated).
std::u16string ComplexString(const Shape& s, uint16_t pid) {
  const auto it = s.complex.find(pid);
  std::u16string out;
  if (it == s.complex.end()) return out;
  for (size_t i = 0; i + 1 < it->second.size() && out.size() < 1024; i += 2) {
    const char16_t c = static_cast<char16_t>(it->second[i] | it->second[i + 1] << 8);
    if (!c) break;
    out += c;
  }
  return out;
}

class Writer {
 public:
  explicit Writer(const DrawingEnv& env) : env_(env) {}

  DrawingXmlOut Write(std::span<const uint8_t> drawing) {
    DrawingXmlOut out;
    const auto dg = Find(drawing, 0xF002);
    if (!dg) return out;
    Records rs(dg->body);
    for (Rec r; rs.Next(r);) {
      if (r.type == 0xF003) {
        out.shapes += Group(r.body, 0, true);
      } else if (r.type == 0xF004) {
        Shape s = Read(r.body);
        if (s.flags & 0x400) {
          bool useBg = false;
          std::string fill = FillXml(s, useBg);
          if (!fill.empty() && fill != "<a:noFill/>") out.background = fill;
        }
      }
    }
    out.lastId = static_cast<int>(std::min<uint32_t>(maxId_, 0x7FFFFFFF));
    return out;
  }

  // Every shape of a drawing (groups' too) into the index.
  void Index(std::span<const uint8_t> drawing, ShapeIndex& index, int depth = 0) {
    if (depth > 34 || index.size() > 200000) return;
    Records rs(depth == 0 ? (Find(drawing, 0xF002) ? Find(drawing, 0xF002)->body : std::span<const uint8_t>()) : drawing);
    for (Rec r; rs.Next(r);) {
      if (r.type == 0xF003) {
        Index(r.body, index, depth + 1);
      } else if (r.type == 0xF004) {
        Shape s = Read(r.body);
        if (s.id) index[s.id] = MasterShape{s.spt, std::move(s.props), std::move(s.complex)};
      }
    }
  }

 private:
  uint32_t Prop(const Shape& s, uint16_t pid, uint32_t def) const {
    const auto it = s.props.find(pid);
    return it != s.props.end() ? it->second : def;
  }
  bool Has(const Shape& s, uint16_t pid) const { return s.props.count(pid) != 0; }
  // A boolean of a boolean property set: `bit`, used when bit + 16 is set.
  bool Bool(const Shape& s, uint16_t pid, int bit, bool def) const {
    if (auto it = s.props.find(pid); it != s.props.end() && (it->second >> (bit + 16) & 1)) return it->second >> bit & 1;
    return def;
  }

  // An OfficeArtCOLORREF as 0xRRGGBB: scheme colors from the slide's scheme, the shape's own colors
  // for the "system" indexes PowerPoint uses (fill, line, shadow), darkened or lightened.
  uint32_t Rgb(const Shape& s, uint32_t c, int depth = 0) const {
    const uint8_t flags = static_cast<uint8_t>(c >> 24);
    if (flags & 0x08) return env_.text.scheme.rgb[c & 7];
    if (flags & 0x10) {
      if (depth > 2) return 0;
      const uint32_t index = c & 0xFF, fn = c >> 8 & 0x0F, param = c >> 16 & 0xFF;
      uint32_t base;
      switch (index) {
        case 0xF0: base = Rgb(s, Prop(s, 0x0181, 0xFFFFFF), depth + 1); break;
        case 0xF1: base = Rgb(s, Prop(s, 0x01C0, 0), depth + 1); break;
        case 0xF2: base = Rgb(s, Prop(s, 0x01C2, 0xFFFFFF), depth + 1); break;
        case 0xF3: base = Rgb(s, Prop(s, 0x0183, 0xFFFFFF), depth + 1); break;
        case 0xF4: base = Rgb(s, Prop(s, 0x0201, 0x808080), depth + 1); break;
        default: base = 0;
      }
      if (fn != 1 && fn != 2) return base;
      uint32_t out = 0;
      for (int shift = 0; shift <= 16; shift += 8) {
        const uint32_t v = base >> shift & 0xFF;
        const uint32_t w = fn == 1 ? v * param / 255 : 255 - (255 - v) * param / 255;
        out |= std::min<uint32_t>(w, 255) << shift;
      }
      return out;
    }
    if (flags & 0x01) return 0;  // a palette index: not used by PowerPoint
    return (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF);
  }
  static int Alpha(uint32_t opacity) { return static_cast<int>(std::clamp<double>(opacity / 65536.0, 0, 1) * 100000); }

  Shape Read(std::span<const uint8_t> body) {
    Shape s;
    Records rs(body);
    for (Rec r; rs.Next(r);) {
      Bytes b(r.body);
      switch (r.type) {
        case 0xF00A:  // FSP
          s.spt = r.inst;
          s.id = b.U32();
          s.flags = b.U32();
          break;
        case 0xF009:  // FSPGR
          for (double& v : s.space) v = b.I32();
          s.grouped = b.Ok();
          break;
        case 0xF00B: case 0xF121: case 0xF122:  // FOPT, secondary and tertiary
          ReadOpt(r, s);
          break;
        case 0xF010:  // ClientAnchor: top, left, right, bottom
          if (r.body.size() >= 16) {
            const double t = b.I32(), l = b.I32(), rt = b.I32(), bt = b.I32();
            s.anchor[0] = l, s.anchor[1] = t, s.anchor[2] = rt, s.anchor[3] = bt;
          } else {
            const double t = b.I16(), l = b.I16(), rt = b.I16(), bt = b.I16();
            s.anchor[0] = l, s.anchor[1] = t, s.anchor[2] = rt, s.anchor[3] = bt;
          }
          s.anchored = b.Ok();
          break;
        case 0xF00F:  // ChildAnchor: left, top, right, bottom
          for (double& v : s.anchor) v = b.I32();
          s.anchored = b.Ok();
          break;
        case 0xF011: {  // ClientData: the placeholder, the click action
          Records cd(r.body);
          for (Rec c; cd.Next(c);) {
            Bytes p(c.body);
            if (c.type == 0x0BC3) {  // OEPlaceholderAtom
              s.phPosition = p.I32();
              s.placeholder = p.U8();
              s.phSize = p.U8();
              if (!p.Ok()) s.placeholder = -1;
            } else if (c.type == 0x1388) {  // ProgTags
              s.style9 = Pp9Tag(c.body);
            } else if (c.type == 0x0FF2 && c.inst == 0) {  // InteractiveInfo: on click
              if (auto atom = Find(c.body, 0x0FF3)) {
                Bytes a(atom->body);
                a.U32();
                const uint32_t link = a.U32();
                const uint8_t action = a.U8();
                a.U8();
                const uint8_t jump = a.U8();
                if (action == 4 && env_.hyperlink) s.clickTarget = env_.hyperlink(link);
                else if (action == 3 && jump >= 1 && jump <= 6) {
                  static const char* const kJump[] = {"", "nextslide", "previousslide", "firstslide", "lastslide", "lastslideviewed", "endshow"};
                  s.clickTarget = std::string("ppaction://hlinkshowjump?jump=") + kJump[jump];
                }
              }
            }
          }
          break;
        }
        case 0xF00D: s.textbox = r.body; break;  // ClientTextbox
        default: break;
      }
    }
    maxId_ = std::max(maxId_, s.id);
    // A shape that names a master's shape (hspMaster) has that one's properties beneath its own, and
    // its type when it has none of its own (FSP fHaveSpt).
    if (env_.masters) {
      const bool ownType = s.flags & 0x800;
      uint32_t ref = s.props.count(0x0301) ? s.props.at(0x0301) : 0;
      for (int depth = 0; ref && ref != s.id && depth < 8; ++depth) {
        const auto it = env_.masters->find(ref);
        if (it == env_.masters->end()) break;
        const MasterShape& m = it->second;
        if (!ownType && depth == 0) s.spt = m.spt;
        for (const auto& [pid, v] : m.props) {
          if (pid == 0x0301) continue;
          auto [own, added] = s.props.emplace(pid, v);
          // Boolean property sets (pid ...3F) merge bit by bit: a bit the shape doesn't set (its "use"
          // bit 16 higher is clear) comes from the master's.
          if (!added && (pid & 0x3F) == 0x3F) {
            const uint32_t take = (v >> 16) & ~(own->second >> 16) & 0xFFFF;
            own->second = (own->second & ~take) | (v & take) | (take << 16);
          }
        }
        for (const auto& [pid, v] : m.complex) s.complex.emplace(pid, v);
        ref = m.props.count(0x0301) ? m.props.at(0x0301) : 0;
      }
    }
    return s;
  }

  // An SpgrContainer: its first SpContainer is the group's own shape; the rest are its children.
  std::string Group(std::span<const uint8_t> body, int depth, bool top) {
    if (depth > 32 || count_ > 50000) return {};
    std::string children;
    Shape self;
    bool first = true;
    Records rs(body);
    for (Rec r; rs.Next(r);) {
      if (r.type == 0xF004) {
        Shape s = Read(r.body);
        if (first) {
          self = std::move(s);
          first = false;
          continue;
        }
        if (s.flags & 0x408) continue;  // deleted, or the background
        ++count_;
        children += ShapeXml(s);
      } else if (r.type == 0xF003) {
        children += Group(r.body, depth + 1, false);
      }
      if (count_ > 50000) break;
    }
    if (top) return children;
    if (children.empty()) return {};
    // A group: its place in its parent and its own space for the children.
    double rect[4];
    Bounds(self, rect);
    const double* sp = self.grouped ? self.space : rect;
    std::string x = "<p:grpSp><p:nvGrpSpPr><p:cNvPr id=\"" + Num(IdOf(self)) + "\" name=\"" + NameOf(self, "Group") + "\"" +
                    (Hidden(self) ? " hidden=\"1\"" : "") + "/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm" +
                    XfrmAttrs(self) + "><a:off x=\"" + Num(Emu(rect[0])) + "\" y=\"" + Num(Emu(rect[1])) + "\"/><a:ext cx=\"" +
                    Num(Emu(std::max(0.0, rect[2] - rect[0]))) + "\" cy=\"" + Num(Emu(std::max(0.0, rect[3] - rect[1]))) +
                    "\"/><a:chOff x=\"" + Num(Emu(sp[0])) + "\" y=\"" + Num(Emu(sp[1])) + "\"/><a:chExt cx=\"" +
                    Num(Emu(std::max(0.0, sp[2] - sp[0]))) + "\" cy=\"" + Num(Emu(std::max(0.0, sp[3] - sp[1]))) +
                    "\"/></a:xfrm></p:grpSpPr>";
    return x + children + "</p:grpSp>";
  }

  uint32_t IdOf(const Shape& s) {
    uint32_t id = s.id;
    if (id < 2 || !ids_.insert(id).second) {
      id = std::max<uint32_t>(maxId_, 1) + 1;
      while (!ids_.insert(id).second) ++id;
      maxId_ = id;
    }
    return id;
  }
  std::string NameOf(const Shape& s, const char* kind) const {
    const std::u16string name = ComplexString(s, 0x0380);
    if (!name.empty()) return Esc(name);
    return std::string(kind) + " " + Num(s.id & 0x3FF);
  }
  bool Hidden(const Shape& s) const { return Bool(s, 0x03BF, 1, false); }

  double Rotation(const Shape& s) const {
    double deg = static_cast<int32_t>(Prop(s, 0x0004, 0)) / 65536.0;
    deg = std::fmod(deg, 360.0);
    if (deg < 0) deg += 360;
    return deg;
  }
  // The shape's unrotated rectangle: OfficeArt stores the bounds of a shape turned by 45-135 or
  // 225-315 degrees as if turned by 90.
  void Bounds(const Shape& s, double out[4]) const {
    std::copy(std::begin(s.anchor), std::end(s.anchor), out);
    const double deg = Rotation(s);
    if ((deg >= 45 && deg < 135) || (deg >= 225 && deg < 315)) {
      const double cx = (out[0] + out[2]) / 2, cy = (out[1] + out[3]) / 2, w = out[2] - out[0], h = out[3] - out[1];
      out[0] = cx - h / 2, out[2] = cx + h / 2, out[1] = cy - w / 2, out[3] = cy + w / 2;
    }
  }
  std::string XfrmAttrs(const Shape& s) const {
    std::string a;
    const double deg = Rotation(s);
    if (deg > 0.001) a += " rot=\"" + Num(std::llround(deg * 60000) % 21600000) + "\"";
    if (s.flags & 0x40) a += " flipH=\"1\"";
    if (s.flags & 0x80) a += " flipV=\"1\"";
    return a;
  }
  std::string Xfrm(const Shape& s) const {
    double r[4];
    Bounds(s, r);
    return "<a:xfrm" + XfrmAttrs(s) + "><a:off x=\"" + Num(Emu(r[0])) + "\" y=\"" + Num(Emu(r[1])) + "\"/><a:ext cx=\"" +
           Num(Emu(std::max(0.0, r[2] - r[0]))) + "\" cy=\"" + Num(Emu(std::max(0.0, r[3] - r[1]))) + "\"/></a:xfrm>";
  }

  std::string Geometry(const Shape& s) const {
    if (s.complex.count(0x0145) && s.complex.count(0x0146) && (s.spt == 0 || s.spt == 100 || PresetForSptOr(s).empty()))
      if (std::string custom = CustomGeometry(s); !custom.empty()) return custom;
    std::string_view preset = PresetForSptOr(s);
    if (preset.empty()) preset = "rect";
    std::string av;
    // Adjust values in OfficeArt's 21600 space for presets whose DrawingML guide is the same share.
    const auto adj = [&](uint16_t pid, const char* name, double scale) {
      if (!s.props.count(pid)) return;
      const double v = static_cast<int32_t>(s.props.at(pid)) * scale;
      av += std::string("<a:gd name=\"") + name + "\" fmla=\"val " + Num(std::llround(v)) + "\"/>";
    };
    if (preset == "roundRect" || preset == "octagon" || preset == "bevel" || preset == "plaque" || preset == "foldedCorner" ||
        preset == "can" || preset == "cube" || preset == "donut" || preset == "hexagon" || preset == "parallelogram" ||
        preset == "trapezoid" || preset == "triangle" || preset == "plus")
      adj(0x0147, "adj", 100000.0 / 21600);
    return "<a:prstGeom prst=\"" + std::string(preset) + "\"><a:avLst>" + av + "</a:avLst></a:prstGeom>";
  }
  static std::string_view PresetForSptOr(const Shape& s) { return draw::PresetForSpt(s.spt); }

  // A freeform: pVertices and pSegmentInfo in the geoLeft..geoBottom space.
  std::string CustomGeometry(const Shape& s) const {
    const auto vs = s.complex.at(0x0145), ss = s.complex.at(0x0146);
    if (vs.size() < 6 || ss.size() < 6) return {};
    const uint16_t nv = static_cast<uint16_t>(vs[0] | vs[1] << 8);
    uint16_t ev = static_cast<uint16_t>(vs[4] | vs[5] << 8);
    if (ev == 0xFFF0) ev = 4;
    if (ev != 4 && ev != 8) return {};
    std::vector<std::pair<long long, long long>> pts;
    for (size_t i = 0; i < nv && 6 + (i + 1) * ev <= vs.size(); ++i) {
      const uint8_t* p = vs.data() + 6 + i * ev;
      if (ev == 4) pts.emplace_back(static_cast<int16_t>(p[0] | p[1] << 8), static_cast<int16_t>(p[2] | p[3] << 8));
      else pts.emplace_back(static_cast<int32_t>(p[0] | p[1] << 8 | p[2] << 16 | uint32_t{p[3]} << 24),
                            static_cast<int32_t>(p[4] | p[5] << 8 | p[6] << 16 | uint32_t{p[7]} << 24));
    }
    const uint16_t ns = static_cast<uint16_t>(ss[0] | ss[1] << 8);
    uint16_t es = static_cast<uint16_t>(ss[4] | ss[5] << 8);
    if (es == 0xFFF0) es = 2;
    if (es != 2 && es != 4) return {};
    const long long l = static_cast<int32_t>(Prop(s, 0x0140, 0)), t = static_cast<int32_t>(Prop(s, 0x0141, 0));
    const long long r = static_cast<int32_t>(Prop(s, 0x0142, 21600)), b = static_cast<int32_t>(Prop(s, 0x0143, 21600));
    const long long w = std::max(1LL, r - l), h = std::max(1LL, b - t);
    std::string path;
    size_t v = 0;
    const auto pt = [&](size_t i) {
      return "<a:pt x=\"" + Num(std::clamp(pts[i].first - l, -w * 64, w * 64)) + "\" y=\"" + Num(std::clamp(pts[i].second - t, -h * 64, h * 64)) + "\"/>";
    };
    bool open = false;
    for (size_t i = 0; i < ns && 6 + (i + 1) * es <= ss.size() && path.size() < (4u << 20); ++i) {
      const uint16_t seg = static_cast<uint16_t>(ss[6 + i * es] | ss[7 + i * es] << 8);
      const int type = seg >> 13;
      const int n = std::max(1, seg & 0x1FFF);
      switch (type) {
        case 0:  // lines
          for (int k = 0; k < n && v < pts.size(); ++k) {
            if (!open) path += "<a:moveTo>" + pt(v++) + "</a:moveTo>", open = true;
            else path += "<a:lnTo>" + pt(v++) + "</a:lnTo>";
          }
          break;
        case 1:  // curves, three points each
          for (int k = 0; k < n && v + 3 <= pts.size(); ++k, v += 3) {
            if (!open) path += "<a:moveTo>" + pt(v - (v > 0 ? 1 : 0)) + "</a:moveTo>", open = true;
            path += "<a:cubicBezTo>" + pt(v) + pt(v + 1) + pt(v + 2) + "</a:cubicBezTo>";
          }
          break;
        case 2:  // move
          if (v < pts.size()) path += "<a:moveTo>" + pt(v++) + "</a:moveTo>", open = true;
          break;
        case 3:  // close
          if (open) path += "<a:close/>";
          break;
        case 4:  // end
          open = false;
          break;
        default:  // escapes: their vertices are skipped
          v += seg & 0xFF;
          break;
      }
    }
    if (path.empty()) return {};
    const bool noFill = !Bool(s, 0x017F, 4, true);  // fFillOK
    return "<a:custGeom><a:avLst/><a:gdLst/><a:ahLst/><a:cxnLst/><a:rect l=\"l\" t=\"t\" r=\"r\" b=\"b\"/><a:pathLst><a:path w=\"" + Num(w) +
           "\" h=\"" + Num(h) + "\"" + (noFill ? " fill=\"none\"" : "") + ">" + path + "</a:path></a:pathLst></a:custGeom>";
  }

  std::string FillXml(const Shape& s, bool& useBg) const {
    useBg = false;
    if (!Bool(s, 0x01BF, 4, true)) return "<a:noFill/>";
    const uint32_t type = Prop(s, 0x0180, 0);
    const uint32_t color = Rgb(s, Prop(s, 0x0181, 0xFFFFFF));
    const int alpha = Alpha(Prop(s, 0x0182, 0x10000));
    const uint32_t back = Rgb(s, Prop(s, 0x0183, 0xFFFFFF));
    const int backAlpha = Alpha(Prop(s, 0x0184, 0x10000));
    switch (type) {
      case 1:  // pattern
        return "<a:pattFill prst=\"pct50\"><a:fgClr>" + ColorXml(color, alpha) + "</a:fgClr><a:bgClr>" + ColorXml(back, backAlpha) +
               "</a:bgClr></a:pattFill>";
      case 2: case 3: {  // texture, picture
        const std::string rel = Has(s, 0x0186) && env_.picture ? env_.picture(Prop(s, 0x0186, 0)) : std::string();
        if (rel.empty()) return "<a:solidFill>" + ColorXml(color, alpha) + "</a:solidFill>";
        return "<a:blipFill dpi=\"0\" rotWithShape=\"1\"><a:blip r:embed=\"" + rel + "\"/>" +
               (type == 2 ? "<a:tile tx=\"0\" ty=\"0\" sx=\"100000\" sy=\"100000\" flip=\"none\" algn=\"tl\"/>"
                          : "<a:srcRect/><a:stretch><a:fillRect/></a:stretch>") +
               "</a:blipFill>";
      }
      case 4: case 5: case 6: case 7: case 8: return Gradient(s, type, color, alpha, back, backAlpha);
      case 9: useBg = true; return {};
      default: return "<a:solidFill>" + ColorXml(color, alpha) + "</a:solidFill>";
    }
  }

  // fillFocus is where the back color sits: 100 gives fill to back along the angle, 0 (and -100) back to
  // fill, 50 back in the middle with fill at both edges (-50 the other way round). The shade colors run
  // from fill (0) to back (1).
  std::string Gradient(const Shape& s, uint32_t type, uint32_t color, int alpha, uint32_t back, int backAlpha) const {
    const int32_t focus = static_cast<int32_t>(Prop(s, 0x018C, 0));
    const bool backFirst = focus == 0 || focus == -100;
    std::vector<std::pair<int, std::string>> stops;  // position (1/1000 %), color
    if (const auto it = s.complex.find(0x0197); it != s.complex.end() && it->second.size() >= 6) {
      const auto d = it->second;
      const uint16_t n = static_cast<uint16_t>(d[0] | d[1] << 8);
      for (size_t i = 0; i < n && 6 + (i + 1) * 8 <= d.size() && stops.size() < 32; ++i) {
        const uint8_t* p = d.data() + 6 + i * 8;
        const uint32_t c = p[0] | p[1] << 8 | p[2] << 16 | uint32_t{p[3]} << 24;
        const int32_t pos = static_cast<int32_t>(p[4] | p[5] << 8 | p[6] << 16 | uint32_t{p[7]} << 24);
        const int at = static_cast<int>(std::clamp(pos / 65536.0, 0.0, 1.0) * 100000);
        stops.emplace_back(backFirst ? 100000 - at : at, ColorXml(Rgb(s, c)));
      }
      if (backFirst) std::reverse(stops.begin(), stops.end());
    }
    if (stops.size() < 2) {
      stops = {{0, ColorXml(color, alpha)}, {100000, ColorXml(back, backAlpha)}};
      if (backFirst) std::swap(stops[0].second, stops[1].second);
      else if (focus == 50 || focus == -50) {
        const std::string edge = focus > 0 ? stops[0].second : stops[1].second, middle = focus > 0 ? stops[1].second : stops[0].second;
        stops = {{0, edge}, {50000, middle}, {100000, edge}};
      }
    }
    std::string g = "<a:gradFill rotWithShape=\"1\"><a:gsLst>";
    for (const auto& [pos, c] : stops) g += "<a:gs pos=\"" + Num(pos) + "\">" + c + "</a:gs>";
    g += "</a:gsLst>";
    if (type == 5 || type == 6) {
      g += std::string("<a:path path=\"") + (type == 5 ? "rect" : "shape") +
           "\"><a:fillToRect l=\"50000\" t=\"50000\" r=\"50000\" b=\"50000\"/></a:path>";
    } else {
      double deg = static_cast<int32_t>(Prop(s, 0x018B, 0)) / 65536.0;
      deg = std::fmod(90 - deg + 720, 360);
      g += "<a:lin ang=\"" + Num(std::llround(deg * 60000) % 21600000) + "\" scaled=\"1\"/>";
    }
    return g + "</a:gradFill>";
  }

  std::string LineXml(const Shape& s) const {
    if (!Bool(s, 0x01FF, 3, true)) return "<a:ln><a:noFill/></a:ln>";
    const uint32_t w = std::min<uint32_t>(Prop(s, 0x01CB, 9525), 20116800);
    std::string a = " w=\"" + Num(w) + "\"";
    switch (Prop(s, 0x01D7, 2)) {
      case 0: a += " cap=\"rnd\""; break;
      case 1: a += " cap=\"sq\""; break;
      default: a += " cap=\"flat\""; break;
    }
    static const char* const kCmpd[] = {"sng", "dbl", "thickThin", "thinThick", "tri"};
    if (const uint32_t st = Prop(s, 0x01CD, 0); st > 0 && st < 5) a += std::string(" cmpd=\"") + kCmpd[st] + "\"";
    std::string x = "<a:ln" + a + "><a:solidFill>" + ColorXml(Rgb(s, Prop(s, 0x01C0, 0)), Alpha(Prop(s, 0x01C1, 0x10000))) + "</a:solidFill>";
    static const char* const kDash[] = {"solid", "sysDash", "sysDot", "sysDashDot", "sysDashDotDot", "dot", "dash", "lgDash", "dashDot", "lgDashDot", "lgDashDotDot"};
    const uint32_t dash = Prop(s, 0x01CE, 0);
    x += std::string("<a:prstDash val=\"") + kDash[dash < 11 ? dash : 0] + "\"/>";
    switch (Prop(s, 0x01D6, 2)) {
      case 0: x += "<a:bevel/>"; break;
      case 1: x += "<a:miter lim=\"800000\"/>"; break;
      default: x += "<a:round/>"; break;
    }
    const auto end = [&](const char* tag, uint16_t typePid, uint16_t widthPid, uint16_t lengthPid) {
      static const char* const kType[] = {"none", "triangle", "stealth", "diamond", "oval", "arrow"};
      static const char* const kSize[] = {"sm", "med", "lg"};
      const uint32_t t = Prop(s, typePid, 0);
      if (t == 0 || t > 5) return;
      x += std::string("<a:") + tag + " type=\"" + kType[t] + "\" w=\"" + kSize[std::min<uint32_t>(Prop(s, widthPid, 1), 2)] + "\" len=\"" +
           kSize[std::min<uint32_t>(Prop(s, lengthPid, 1), 2)] + "\"/>";
    };
    end("headEnd", 0x01D0, 0x01D2, 0x01D3);
    end("tailEnd", 0x01D1, 0x01D4, 0x01D5);
    return x + "</a:ln>";
  }

  std::string EffectXml(const Shape& s) const {
    if (!Bool(s, 0x023F, 1, false)) return {};
    const double dx = static_cast<int32_t>(Prop(s, 0x0205, 25400)), dy = static_cast<int32_t>(Prop(s, 0x0206, 25400));
    const double dist = std::sqrt(dx * dx + dy * dy);
    double dir = std::atan2(dy, dx) * 180 / 3.14159265358979;
    if (dir < 0) dir += 360;
    return "<a:effectLst><a:outerShdw dist=\"" + Num(std::llround(dist)) + "\" dir=\"" + Num(std::llround(dir * 60000) % 21600000) +
           "\" algn=\"tl\" rotWithShape=\"0\">" + ColorXml(Rgb(s, Prop(s, 0x0201, 0x808080)), Alpha(Prop(s, 0x0204, 0x10000))) +
           "</a:outerShdw></a:effectLst>";
  }

  // The placeholder's p:ph, from OEPlaceholderAtom's placement id.
  std::string Ph(const Shape& s) const {
    const char* type = nullptr;
    bool vert = false, index = true;
    switch (s.placeholder) {
      case 0x01: case 0x0D: type = "title", index = false; break;
      case 0x11: type = "title", vert = true, index = false; break;
      case 0x02: case 0x0E: type = "body"; break;
      case 0x12: type = "body", vert = true; break;
      case 0x03: case 0x0F: type = "ctrTitle", index = false; break;
      case 0x04: case 0x10: type = "subTitle"; break;
      case 0x05: case 0x0B: type = "sldImg"; break;
      case 0x06: case 0x0C: type = "body"; break;
      case 0x07: type = "dt"; break;
      case 0x08: type = "sldNum"; break;
      case 0x09: type = "ftr"; break;
      case 0x0A: type = "hdr"; break;
      case 0x13: type = ""; break;
      case 0x1A: type = "", vert = true; break;
      case 0x14: type = "chart"; break;
      case 0x15: type = "tbl"; break;
      case 0x16: type = "clipArt"; break;
      case 0x17: type = "dgm"; break;
      case 0x18: type = "media"; break;
      case 0x19: type = "pic"; break;
      default: return {};
    }
    std::string ph = "<p:ph";
    if (*type) ph += std::string(" type=\"") + type + "\"";
    if (vert) ph += " orient=\"vert\"";
    if (s.phSize == 1) ph += " sz=\"half\"";
    else if (s.phSize == 2) ph += " sz=\"quarter\"";
    if (index) {
      int idx = s.phPosition > 0 ? s.phPosition : 1;
      if (std::string_view(type) == "dt") idx = 10;
      else if (std::string_view(type) == "ftr") idx = 11;
      else if (std::string_view(type) == "sldNum") idx = 12;
      ph += " idx=\"" + Num(idx) + "\"";
    }
    return ph + "/>";
  }
  // The text type a placeholder's text has when its text box doesn't say.
  int PlaceholderTextType(const Shape& s) const {
    switch (s.placeholder) {
      case 0x01: case 0x0D: case 0x11: return kTitle;
      case 0x03: case 0x0F: return kCenterTitle;
      case 0x04: case 0x10: return kCenterBody;
      case 0x06: case 0x0C: return kNotes;
      case 0x02: case 0x0E: case 0x12: case 0x13: case 0x1A: return kBody;
      default: return kOther;
    }
  }

  std::string BodyPr(const Shape& s) const {
    std::string a;
    const uint32_t wrap = Prop(s, 0x0085, 0);
    a += wrap == 2 ? " wrap=\"none\"" : " wrap=\"square\"";
    // Signed: table cells PowerPoint saves often have a slightly negative inset (-635), drawn as none.
    auto inset = [&](uint16_t pid, uint32_t def) {
      return Num(std::clamp<int64_t>(static_cast<int32_t>(Prop(s, pid, def)), 0, 51206400));
    };
    a += " lIns=\"" + inset(0x0081, 91440) + "\" tIns=\"" + inset(0x0082, 45720) + "\" rIns=\"" + inset(0x0083, 91440) +
         "\" bIns=\"" + inset(0x0084, 45720) + "\"";
    static const char* const kAnchor[] = {"t", "ctr", "b", "t", "ctr", "b", "t", "b", "t", "b"};
    const uint32_t an = Prop(s, 0x0087, 0);
    a += std::string(" anchor=\"") + kAnchor[an < 10 ? an : 0] + "\"";
    a += an == 3 || an == 4 || an == 5 || an == 8 || an == 9 ? " anchorCtr=\"1\"" : " anchorCtr=\"0\"";
    switch (Prop(s, 0x0088, 0)) {
      case 1: case 3: a += " vert=\"vert\""; break;
      case 2: a += " vert=\"vert270\""; break;
      case 5: a += " vert=\"eaVert\""; break;
      default: a += " vert=\"horz\""; break;
    }
    a += " rtlCol=\"0\"";
    const bool fit = Bool(s, 0x00BF, 1, false);
    return "<a:bodyPr" + a + ">" + (fit ? "<a:spAutoFit/>" : "<a:noAutofit/>") + "</a:bodyPr>";
  }

  // The shape's text: in its client text box, or the outline's when it refers to it.
  bool TextOf(const Shape& s, Text& t) const {
    if (s.textbox.empty()) return false;
    if (auto ref = Find(s.textbox, 0x0F9E)) {
      Bytes b(ref->body);
      const int32_t index = b.I32();
      const Text* o = env_.outline && b.Ok() ? env_.outline(index) : nullptr;
      if (!o) return false;
      t = *o;
      return true;
    }
    t.type = -1;
    ReadText(s.textbox, t, env_.hyperlink);
    if (auto atom = Find(s.style9, 0x0FAC)) ReadStyle9(atom->body, t.pf9);
    if (t.type < 0 || t.type > 8) t.type = s.placeholder >= 0 ? PlaceholderTextType(s) : kOther;
    if (env_.notes && s.placeholder == 0x0C) t.type = kNotes;
    t.placeholder = s.placeholder >= 0;
    return true;
  }

  std::string ClickXml(const Shape& s) const {
    if (s.clickTarget.empty() || !env_.link) return {};
    const std::string rel = env_.link(s.clickTarget);
    if (rel.empty()) return {};
    if (s.clickTarget.rfind("ppaction:", 0) == 0) return "<a:hlinkClick r:id=\"\" action=\"" + Esc(s.clickTarget) + "\"/>";
    return "<a:hlinkClick r:id=\"" + rel + "\"/>";
  }

  std::string ShapeXml(const Shape& s) {
    if (!s.anchored) return {};
    const uint32_t id = IdOf(s);
    const std::string hidden = Hidden(s) ? " hidden=\"1\"" : "";
    const std::string click = ClickXml(s);
    const std::string cNvPrEnd = click.empty() ? "/>" : ">" + click + "</p:cNvPr>";
    const bool picture = s.spt == 75 || ((s.flags & 0x10) && Has(s, 0x0104));
    if (picture) {
      const std::string rel = Has(s, 0x0104) && env_.picture ? env_.picture(Prop(s, 0x0104, 0)) : std::string();
      if (rel.empty()) return {};
      std::string crop;
      const auto c = [&](uint16_t pid, const char* side) {
        const int32_t v = static_cast<int32_t>(Prop(s, pid, 0));
        if (v) crop += std::string(" ") + side + "=\"" + Num(std::llround(v / 65536.0 * 100000)) + "\"";
      };
      c(0x0102, "l"), c(0x0100, "t"), c(0x0103, "r"), c(0x0101, "b");
      std::string blip = "<a:blip r:embed=\"" + rel + "\">";
      if (Bool(s, 0x013F, 2, false)) blip += "<a:grayscl/>";
      blip += "</a:blip>";
      std::string ph = s.placeholder >= 0 ? Ph(s) : std::string();
      std::string x = "<p:pic><p:nvPicPr><p:cNvPr id=\"" + Num(id) + "\" name=\"" + NameOf(s, "Picture") + "\"" + hidden + cNvPrEnd +
                      "<p:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></p:cNvPicPr><p:nvPr>" + ph + "</p:nvPr></p:nvPicPr><p:blipFill>" +
                      blip + "<a:srcRect" + crop + "/><a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr>" + Xfrm(s) +
                      "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>";
      // A picture's line shows only when the file turns it on.
      if (Bool(s, 0x01FF, 3, false)) x += LineXml(s);
      return x + "</p:spPr></p:pic>";
    }
    bool useBg = false;
    std::string fill = FillXml(s, useBg);
    const std::string_view preset = draw::PresetForSpt(s.spt);
    if (preset == "line" || (s.spt >= 32 && s.spt <= 40)) fill = "<a:noFill/>", useBg = false;
    const bool textBox = s.spt == 202 && s.placeholder < 0;
    std::string x = std::string("<p:sp") + (useBg ? " useBgFill=\"1\"" : "") + "><p:nvSpPr><p:cNvPr id=\"" + Num(id) + "\" name=\"" +
                    NameOf(s, s.placeholder >= 0 ? "Placeholder" : textBox ? "TextBox" : "Shape") + "\"" + hidden + cNvPrEnd +
                    "<p:cNvSpPr" + (textBox ? " txBox=\"1\"" : "") + "/><p:nvPr>" + (s.placeholder >= 0 ? Ph(s) : std::string()) +
                    "</p:nvPr></p:nvSpPr><p:spPr>" + Xfrm(s) + Geometry(s) + fill + LineXml(s) + EffectXml(s) + "</p:spPr>";
    Text t;
    if (TextOf(s, t) && (!t.chars.empty() || s.placeholder >= 0)) {
      x += "<p:txBody>" + BodyPr(s) + "<a:lstStyle/>" + ParagraphsXml(t, env_.text, env_.link) + "</p:txBody>";
    }
    return x + "</p:sp>";
  }

  const DrawingEnv& env_;
  uint32_t maxId_ = 1;
  size_t count_ = 0;
  std::set<uint32_t> ids_;
};

}  // namespace

DrawingXmlOut DrawingXml(std::span<const uint8_t> drawing, const DrawingEnv& env) {
  Writer w(env);
  return w.Write(drawing);
}

void IndexShapes(std::span<const uint8_t> drawing, ShapeIndex& index) {
  const DrawingEnv env;
  Writer w(env);
  w.Index(drawing, index);
}

}  // namespace fw::slide::ppt::detail
