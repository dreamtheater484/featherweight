// A sheet's OfficeArt drawing ([MS-ODRAW], as BIFF8 stores it: [MS-XLS] 2.4.168 MsoDrawing) as a
// DrawingML drawing part: pictures from the workbook's BLIP store, text boxes with their TXO text,
// rectangles, ovals and lines. The k-th shape with client data goes with the k-th OBJ record, and the
// k-th with a client text box with the k-th TXO record, as Excel writes them.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "pkg/Zip.h"
#include "sheet/xls/Internal.h"

namespace fw::sheet::xls::detail {
namespace {

constexpr double kEmuPx = 9525, kEmuPt = 12700;

struct Header {
  uint16_t ver = 0, inst = 0, type = 0;
  size_t body = 0, end = 0;
};

// The record header at `at`, its body cut to `limit`.
bool ReadHeader(std::span<const uint8_t> d, size_t at, size_t limit, Header& h) {
  if (at > limit || limit - at < 8) return false;
  Bytes b(d.subspan(at, 8));
  const uint16_t vi = b.U16();
  h.type = b.U16();
  const uint32_t len = b.U32();
  h.ver = vi & 0xF;
  h.inst = vi >> 4;
  h.body = at + 8;
  h.end = h.body + std::min<size_t>(len, limit - h.body);
  return true;
}

struct Rect {
  double l = 0, t = 0, r = 0, b = 0;
};

struct Shape {
  uint16_t spt = 0;
  uint32_t flags = 0;  // FSP: fGroup 1, fChild 2, fPatriarch 4, fDeleted 8, fFlipH 0x40, fFlipV 0x80
  bool anchored = false, childAnchored = false, group = false;
  uint16_t anchorFlags = 0;
  uint16_t a[8] = {};  // ClientAnchor: col, dx (1/1024 of it), row, dy (1/256 of it); then the end's
  Rect child, space;   // ChildAnchor (in the group's space); FSPGR (a group's space for its shapes)
  std::map<uint16_t, uint32_t> props;
  int obj = -1, text = -1, parent = -1;
  Rect at;  // where it is, in EMUs from the sheet's top-left
};

class Walker {
 public:
  Walker(std::span<const uint8_t> d) : d_(d) {}
  std::vector<Shape> shapes;
  int objs = 0, texts = 0;

  void Walk(size_t at, size_t end, int depth) {
    Header h;
    while (depth < 12 && ReadHeader(d_, at, end, h)) {
      if (h.type == 0xF003) Group(h.body, h.end, -1, depth + 1);
      else if (h.ver == 0xF) Walk(h.body, h.end, depth + 1);
      at = h.end;
      if (shapes.size() > 100000) return;
    }
  }

 private:
  // An SpgrContainer: its first SpContainer is the group's own shape; the rest are in the group.
  void Group(size_t at, size_t end, int parent, int depth) {
    Header h;
    int self = -1;
    bool first = true;
    while (depth < 12 && ReadHeader(d_, at, end, h)) {
      if (h.type == 0xF004) {
        Shape s = Read(h.body, h.end);
        s.parent = first ? parent : self;
        if (first) s.group = true;
        const bool patriarch = first && (s.flags & 4);  // the drawing's own group: its shapes are on the sheet
        shapes.push_back(std::move(s));
        if (first) self = patriarch ? parent : static_cast<int>(shapes.size()) - 1;
        first = false;
      } else if (h.type == 0xF003) {
        Group(h.body, h.end, self, depth + 1);
        first = false;
      }
      at = h.end;
      if (shapes.size() > 100000) return;
    }
  }

  Shape Read(size_t at, size_t end) {
    Shape s;
    Header h;
    while (ReadHeader(d_, at, end, h)) {
      Bytes b(d_.subspan(h.body, h.end - h.body));
      switch (h.type) {
        case 0xF00A:  // FSP
          s.spt = h.inst;
          b.U32();
          s.flags = b.U32();
          break;
        case 0xF009:  // FSPGR
          s.space = {static_cast<double>(static_cast<int32_t>(b.U32())), static_cast<double>(static_cast<int32_t>(b.U32())),
                     static_cast<double>(static_cast<int32_t>(b.U32())), static_cast<double>(static_cast<int32_t>(b.U32()))};
          break;
        case 0xF00B: case 0xF122:  // FOPT, its tertiary: simple properties (complex ones' data follows them)
          for (uint16_t i = 0; i < h.inst && b.Left() >= 6; ++i) {
            const uint16_t id = b.U16();
            const uint32_t v = b.U32();
            if (!(id & 0x8000)) s.props[id & 0x3FFF] = v;
          }
          break;
        case 0xF010:  // ClientAnchor
          if (h.end - h.body < 18) break;
          s.anchorFlags = b.U16();
          for (uint16_t& v : s.a) v = b.U16();
          s.anchored = true;
          break;
        case 0xF00F:  // ChildAnchor
          s.child = {static_cast<double>(static_cast<int32_t>(b.U32())), static_cast<double>(static_cast<int32_t>(b.U32())),
                     static_cast<double>(static_cast<int32_t>(b.U32())), static_cast<double>(static_cast<int32_t>(b.U32()))};
          s.childAnchored = b.Ok();
          break;
        case 0xF011: s.obj = objs++; break;    // ClientData: an OBJ record follows
        case 0xF00D: s.text = texts++; break;  // ClientTextbox: a TXO record follows
      }
      at = h.end;
    }
    return s;
  }

  std::span<const uint8_t> d_;
};

std::string Esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      default:
        if (static_cast<unsigned char>(c) >= 0x20 || c == '\t') o += c;
    }
  }
  return o;
}

std::string Hex(uint32_t rgb) {
  char buf[8];
  std::snprintf(buf, sizeof buf, "%06X", rgb & 0xFFFFFF);
  return buf;
}

std::string Emu(double v) { return std::to_string(static_cast<int64_t>(std::llround(std::clamp(v, 0.0, 2e13)))); }

}  // namespace

std::string DrawingXml(const DrawingSheet& in, const DrawingEnv& env, std::string& rels) {
  Walker w(in.dg);
  w.Walk(0, in.dg.size(), 0);
  if (w.shapes.empty()) return {};
  const bool objsMatch = w.objs == static_cast<int>(in.objs.size());
  const bool textsMatch = w.texts == static_cast<int>(in.texts.size());

  // Columns' and rows' left and top edges in EMUs.
  std::vector<double> colLeft(257, 0);
  for (size_t c = 0; c < 256; ++c) colLeft[c + 1] = colLeft[c] + (c < env.colPx.size() ? env.colPx[c] : 64) * kEmuPx;
  std::vector<double> rowTop(1, 0);
  auto top = [&](uint32_t row) {
    row = std::min<uint32_t>(row, 65536);
    while (rowTop.size() <= row) rowTop.push_back(rowTop.back() + env.rowPt(static_cast<uint32_t>(rowTop.size() - 1)) * kEmuPt);
    return rowTop[row];
  };
  auto x = [&](uint16_t col, uint16_t dx) {
    col = std::min<uint16_t>(col, 255);
    return colLeft[col] + std::min<uint16_t>(dx, 1024) / 1024.0 * (colLeft[col + 1] - colLeft[col]);
  };
  auto y = [&](uint16_t row, uint16_t dy) { return top(row) + std::min<uint16_t>(dy, 256) / 256.0 * (top(row + 1u) - top(row)); };

  // Where each shape is: a group's shapes through the group's space.
  for (Shape& s : w.shapes) {
    if (s.parent < 0 || !s.childAnchored) {
      if (s.anchored) s.at = {x(s.a[0], s.a[1]), y(s.a[2], s.a[3]), x(s.a[4], s.a[5]), y(s.a[6], s.a[7])};
      continue;
    }
    const Shape& g = w.shapes[s.parent];
    const double sw = g.space.r - g.space.l, sh = g.space.b - g.space.t;
    if (sw == 0 || sh == 0) continue;
    const double kx = (g.at.r - g.at.l) / sw, ky = (g.at.b - g.at.t) / sh;
    s.at = {g.at.l + (s.child.l - g.space.l) * kx, g.at.t + (s.child.t - g.space.t) * ky, g.at.l + (s.child.r - g.space.l) * kx,
            g.at.t + (s.child.b - g.space.t) * ky};
  }

  auto prop = [](const Shape& s, uint16_t id, uint32_t def) {
    auto it = s.props.find(id);
    return it == s.props.end() ? def : it->second;
  };
  // A boolean property set's flag: `bit`, when its "use" bit (16 higher) is set.
  auto flag = [&](const Shape& s, uint16_t id, int bit, bool def) {
    auto it = s.props.find(id);
    if (it == s.props.end() || !(it->second & (1u << (bit + 16)))) return def;
    return (it->second & (1u << bit)) != 0;
  };
  // An OfficeArtCOLORREF as RRGGBB.
  auto color = [&](uint32_t v, uint32_t def) -> uint32_t {
    const uint8_t f = v >> 24;
    if (f & 0x10) return def;                 // a system color
    if (f & 0x08) return env.icvRgb(v & 0xFF);  // the palette's
    return (v & 0xFF) << 16 | (v & 0xFF00) | (v >> 16 & 0xFF);
  };
  auto solid = [](uint32_t rgb) { return "<a:solidFill><a:srgbClr val=\"" + Hex(rgb) + "\"/></a:solidFill>"; };

  // Cell anchors: a shape's own where it has one, else from where it is.
  auto marker = [&](const char* name, double ex, double ey) {
    ex = std::max(ex, 0.0), ey = std::max(ey, 0.0);
    const size_t col = std::clamp<ptrdiff_t>(std::upper_bound(colLeft.begin(), colLeft.end(), ex) - colLeft.begin() - 1, 0, 255);
    while (rowTop.back() <= ey && rowTop.size() <= 65536) top(static_cast<uint32_t>(rowTop.size()));
    const size_t row = std::clamp<ptrdiff_t>(std::upper_bound(rowTop.begin(), rowTop.end(), ey) - rowTop.begin() - 1, 0,
                                             std::min<ptrdiff_t>(65535, static_cast<ptrdiff_t>(rowTop.size()) - 1));
    return std::string("<xdr:") + name + "><xdr:col>" + std::to_string(col) + "</xdr:col><xdr:colOff>" + Emu(ex - colLeft[col]) +
           "</xdr:colOff><xdr:row>" + std::to_string(row) + "</xdr:row><xdr:rowOff>" + Emu(ey - rowTop[row]) + "</xdr:rowOff></xdr:" + name + ">";
  };
  auto cellMarker = [&](const char* name, uint16_t col, uint16_t dx, uint16_t row, uint16_t dy) {
    col = std::min<uint16_t>(col, 255);
    return std::string("<xdr:") + name + "><xdr:col>" + std::to_string(col) + "</xdr:col><xdr:colOff>" + Emu(x(col, dx) - colLeft[col]) +
           "</xdr:colOff><xdr:row>" + std::to_string(row) + "</xdr:row><xdr:rowOff>" + Emu(y(row, dy) - top(row)) +
           "</xdr:rowOff></xdr:" + name + ">";
  };

  std::string body;
  std::map<uint32_t, std::string> imageIds;  // BLIP -> relationship id
  int id = 1;
  for (const Shape& s : w.shapes) {
    if (s.group || (s.flags & 0x0D) || (!s.anchored && !s.childAnchored)) continue;  // groups, the patriarch, deleted
    if (s.at.r < s.at.l || s.at.b < s.at.t) continue;
    bool placed = true;
    for (int p = s.parent; p >= 0 && placed; p = w.shapes[p].parent) placed = w.shapes[p].anchored || w.shapes[p].childAnchored;
    if (!placed) continue;
    {
      // Without an OBJ record for each shape, pictures only: a note's box looks like a text box.
      const int ot = objsMatch && s.obj >= 0 ? in.objs[s.obj] : -1;
      const uint32_t pib = prop(s, 0x0104, 0);
      const bool picture = (ot == 8 || (ot < 0 && s.spt == 75)) && pib > 0;
      const bool shape = !picture && objsMatch && (ot == 1 || ot == 2 || ot == 3 || ot == 6 || ot == 30);
      if (!picture && !shape) continue;
      std::string target;
      if (picture && !imageIds.count(pib)) {
        target = env.image(pib);
        if (target.empty()) continue;
        const std::string rid = "rId" + std::to_string(imageIds.size() + 1);
        imageIds[pib] = rid;
        rels += "<Relationship Id=\"" + rid + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"" +
                Esc(target) + "\"/>";
      }
      ++id;
      const bool topLevel = s.parent < 0 && s.anchored;
      std::string a = "<xdr:twoCellAnchor";
      if (topLevel && (s.anchorFlags & 1)) a += " editAs=\"absolute\"";
      else if (topLevel && (s.anchorFlags & 2)) a += " editAs=\"oneCell\"";
      a += ">";
      a += topLevel ? cellMarker("from", s.a[0], s.a[1], s.a[2], s.a[3]) + cellMarker("to", s.a[4], s.a[5], s.a[6], s.a[7])
                    : marker("from", s.at.l, s.at.t) + marker("to", s.at.r, s.at.b);
      const bool hidden = flag(s, 0x03BF, 1, false);
      const std::string name = (picture ? "Picture " : ot == 6 || s.spt == 202 ? "TextBox " : "Shape ") + std::to_string(id - 1);
      const std::string cNvPr = "<xdr:cNvPr id=\"" + std::to_string(id) + "\" name=\"" + name + "\"" + (hidden ? " hidden=\"1\"" : "") + "/>";
      std::string xfrm = "<a:xfrm";
      if (s.flags & 0x40) xfrm += " flipH=\"1\"";
      if (s.flags & 0x80) xfrm += " flipV=\"1\"";
      xfrm += "><a:off x=\"" + Emu(s.at.l) + "\" y=\"" + Emu(s.at.t) + "\"/><a:ext cx=\"" + Emu(s.at.r - s.at.l) + "\" cy=\"" +
              Emu(s.at.b - s.at.t) + "\"/></a:xfrm>";
      const bool lined = flag(s, 0x01FF, 3, !picture);
      std::string line;
      if (lined) {
        line = "<a:ln w=\"" + std::to_string(std::min<uint32_t>(prop(s, 0x01CB, 9525), 20116800)) + "\">" +
               solid(color(prop(s, 0x01C0, 0), 0)) + "</a:ln>";
      } else if (!picture) {
        line = "<a:ln><a:noFill/></a:ln>";
      }
      if (picture) {
        std::string crop;
        const char* sides[] = {"t", "b", "l", "r"};
        for (int k = 0; k < 4; ++k) {
          const int32_t v = static_cast<int32_t>(prop(s, static_cast<uint16_t>(0x0100 + k), 0));
          if (v != 0) crop += std::string(" ") + sides[k] + "=\"" + std::to_string(std::llround(v / 65536.0 * 100000)) + "\"";
        }
        a += "<xdr:pic><xdr:nvPicPr>" + cNvPr + "<xdr:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></xdr:cNvPicPr></xdr:nvPicPr>"
             "<xdr:blipFill><a:blip r:embed=\"" + imageIds[pib] + "\"/>" + (crop.empty() ? "" : "<a:srcRect" + crop + "/>") +
             "<a:stretch><a:fillRect/></a:stretch></xdr:blipFill><xdr:spPr>" + xfrm +
             "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>" + line + "</xdr:spPr></xdr:pic>";
      } else {
        const bool isLine = ot == 1 || s.spt == 20 || s.spt == 32;
        const bool textBox = ot == 6 || s.spt == 202;
        const char* geom = isLine ? "line" : (ot == 3 || s.spt == 3) ? "ellipse" : s.spt == 2 ? "roundRect" : "rect";
        std::string fill;
        if (!isLine) fill = flag(s, 0x01BF, 4, true) ? solid(color(prop(s, 0x0181, 0xFFFFFF), 0xFFFFFF)) : "<a:noFill/>";
        a += std::string("<xdr:sp macro=\"\" textlink=\"\"><xdr:nvSpPr>") + cNvPr + (textBox ? "<xdr:cNvSpPr txBox=\"1\"/>" : "<xdr:cNvSpPr/>") +
             "</xdr:nvSpPr><xdr:spPr>" + xfrm + "<a:prstGeom prst=\"" + geom + "\"><a:avLst/></a:prstGeom>" + fill + line + "</xdr:spPr>";
        const DrawingSheet::Text* t = textsMatch && s.text >= 0 ? &in.texts[s.text] : nullptr;
        if (t && !t->chars.empty()) {
          static const char* const kAlign[] = {"l", "l", "ctr", "r", "just", "l", "l", "dist"};
          static const char* const kAnchor[] = {"t", "t", "ctr", "b", "t", "t", "t", "t"};
          a += "<xdr:txBody><a:bodyPr vertOverflow=\"clip\" wrap=\"" + std::string(prop(s, 0x0085, 0) == 2 ? "none" : "square") +
               "\" lIns=\"" + std::to_string(prop(s, 0x0081, 91440)) + "\" tIns=\"" + std::to_string(prop(s, 0x0082, 45720)) +
               "\" rIns=\"" + std::to_string(prop(s, 0x0083, 91440)) + "\" bIns=\"" + std::to_string(prop(s, 0x0084, 45720)) +
               "\" anchor=\"" + kAnchor[t->grbit >> 4 & 7] + "\"/><a:lstStyle/>";
          const std::string pPr = std::string("<a:pPr algn=\"") + kAlign[t->grbit >> 1 & 7] + "\"/>";
          size_t run = 0;
          uint16_t font = t->runs.empty() ? 0 : t->runs[0].second;
          std::string para = pPr, text;
          bool any = false;
          auto flush = [&] {
            if (!text.empty()) para += "<a:r>" + env.runProps(font) + "<a:t>" + Esc(text) + "</a:t></a:r>", any = true;
            text.clear();
          };
          for (size_t i = 0; i <= t->chars.size(); ++i) {
            while (run < t->runs.size() && t->runs[run].first <= i) {
              if (t->runs[run].first == i && i > 0) flush();
              font = t->runs[run++].second;
            }
            if (i == t->chars.size() || t->chars[i] == '\n') {
              flush();
              a += "<a:p>" + para + (any ? "" : "<a:endParaRPr lang=\"en-US\"/>") + "</a:p>";
              para = pPr;
              any = false;
              continue;
            }
            if (t->chars[i] == '\r') continue;
            const uint16_t c = t->chars[i];
            AppendUtf16(text, &c, 1);
          }
          a += "</xdr:txBody>";
        }
        a += "</xdr:sp>";
      }
      body += a + "<xdr:clientData/></xdr:twoCellAnchor>";
    }
  }
  if (body.empty()) return {};
  return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
         "<xdr:wsDr xmlns:xdr=\"http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing\" "
         "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
         "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">" +
         body + "</xdr:wsDr>";
}

bool BlipPicture(std::span<const uint8_t> dgg, uint32_t pib, std::string& ext, std::vector<uint8_t>& out) {
  // DggContainer > BStoreContainer > the pib-th FBSE, with its BLIP inside it.
  Header h;
  size_t at = 0;
  std::span<const uint8_t> fbse;
  while (fbse.empty() && ReadHeader(dgg, at, dgg.size(), h)) {
    if (h.type == 0xF000) {
      at = h.body;
      continue;
    }
    if (h.type == 0xF001) {
      size_t p = h.body;
      Header e;
      for (uint32_t i = 1; ReadHeader(dgg, p, h.end, e); ++i, p = e.end)
        if (i == pib) {
          if (e.type == 0xF007) fbse = dgg.subspan(e.body, e.end - e.body);
          break;
        }
      break;
    }
    at = h.end;
  }
  if (fbse.size() < 36 + 8) return false;
  const size_t blipAt = 36 + fbse[33];  // after the FBSE's fields and its name
  if (!ReadHeader(fbse, blipAt, fbse.size(), h)) return false;
  Bytes b(fbse.subspan(h.body, h.end - h.body));
  b.Skip(h.inst & 1 ? 32 : 16);  // one or two UIDs
  switch (h.type) {
    case 0xF01D: case 0xF02A: ext = "jpeg"; break;
    case 0xF01E: ext = "png"; break;
    case 0xF01F: ext = "bmp"; break;
    case 0xF029: ext = "tiff"; break;
    case 0xF01A: ext = "emf"; break;
    case 0xF01B: ext = "wmf"; break;
    default: return false;
  }
  if (h.type == 0xF01A || h.type == 0xF01B) {
    // OfficeArtMetafileHeader: sizes, bounds, and the data deflated (zlib) or not.
    const uint32_t size = b.U32();
    int32_t bounds[4];
    for (int32_t& v : bounds) v = static_cast<int32_t>(b.U32());
    const int32_t emuW = static_cast<int32_t>(b.U32());
    b.U32();
    const uint32_t saved = b.U32();
    const uint8_t compression = b.U8();
    b.U8();
    std::span<const uint8_t> data = b.Take(std::min<size_t>(saved, b.Left()));
    if (!b.Ok() || size > (64u << 20)) return false;
    if (compression == 0) {
      if (data.size() > 2 && (data[0] & 0x0F) == 8 && ((data[0] << 8) | data[1]) % 31 == 0) data = data.subspan(2);
      if (!pkg::Inflate(data, size, out)) return false;
    } else {
      out.assign(data.begin(), data.end());
    }
    if (h.type == 0xF01B && !(out.size() >= 4 && out[0] == 0xD7 && out[1] == 0xCD && out[2] == 0xC6 && out[3] == 0x9A)) {
      // A WMF without its placeable header: one from the bounds, at their units per inch.
      const int32_t w = bounds[2] - bounds[0];
      const double inch = emuW > 0 && w > 0 ? w * 914400.0 / emuW : 1440;
      uint16_t hdr[11] = {0xCDD7, 0x9AC6, 0, static_cast<uint16_t>(bounds[0]), static_cast<uint16_t>(bounds[1]),
                          static_cast<uint16_t>(bounds[2]), static_cast<uint16_t>(bounds[3]),
                          static_cast<uint16_t>(std::clamp(std::lround(inch), 1L, 65535L)), 0, 0, 0};
      for (int i = 0; i < 10; ++i) hdr[10] ^= hdr[i];
      std::vector<uint8_t> head(22);
      for (int i = 0; i < 11; ++i) head[2 * i] = static_cast<uint8_t>(hdr[i]), head[2 * i + 1] = static_cast<uint8_t>(hdr[i] >> 8);
      out.insert(out.begin(), head.begin(), head.end());
    }
    return !out.empty();
  }
  b.U8();  // tag
  std::span<const uint8_t> data = b.Rest();
  if (!b.Ok() || data.empty()) return false;
  out.clear();
  if (h.type == 0xF01F) {
    // A DIB: a .bmp needs its file header, with where the pixels start.
    if (data.size() < 40) return false;
    Bytes info(data);
    const uint32_t infoSize = info.U32();
    info.Skip(10);
    const uint16_t bits = info.U16();
    const uint32_t compression = info.U32();
    info.Skip(12);
    const uint32_t used = info.U32();
    if (infoSize < 12 || infoSize > data.size()) return false;
    const uint64_t colors = used ? used : bits <= 8 ? (1u << bits) : 0;
    const uint64_t offset = 14 + infoSize + colors * 4 + (compression == 3 && infoSize == 40 ? 12 : 0);
    const uint64_t total = 14 + data.size();
    if (offset > total) return false;
    const uint8_t head[14] = {'B', 'M', static_cast<uint8_t>(total), static_cast<uint8_t>(total >> 8), static_cast<uint8_t>(total >> 16),
                              static_cast<uint8_t>(total >> 24), 0, 0, 0, 0, static_cast<uint8_t>(offset),
                              static_cast<uint8_t>(offset >> 8), static_cast<uint8_t>(offset >> 16), static_cast<uint8_t>(offset >> 24)};
    out.assign(std::begin(head), std::end(head));
  }
  out.insert(out.end(), data.begin(), data.end());
  return true;
}

}  // namespace fw::sheet::xls::detail
