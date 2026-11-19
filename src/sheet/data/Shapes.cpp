// Pictures and shapes (s3-data): Excel's anchors for new ones, and their formatting read and
// written. See Shapes.h.

#include "sheet/data/Shapes.h"

#include <cmath>
#include <cstdio>
#include <vector>

#include <pugixml.hpp>

#include "draw/Images.h"
#include "pkg/XmlWriter.h"
#include "sheet/data/Rules.h"

namespace fw::sheet::data {

namespace {

constexpr std::string_view kNs =
    " xmlns:xdr=\"http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing\""
    " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"";
// Where the caller's placing goes (drawing::Placed rewrites these).
constexpr std::string_view kCorners =
    "<xdr:from><xdr:col>0</xdr:col><xdr:colOff>0</xdr:colOff><xdr:row>0</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:from>"
    "<xdr:to><xdr:col>0</xdr:col><xdr:colOff>0</xdr:colOff><xdr:row>0</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:to>";
constexpr std::string_view kXfrm = "<a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/></a:xfrm>";

std::string Attr(std::string_view value) {
  std::string out;
  pkg::AppendEscapedAttribute(out, XmlChars(value));
  return out;
}

std::string CNvPr(uint32_t id, std::string_view name, std::string_view descr = {}) {
  std::string out = "<xdr:cNvPr id=\"" + std::to_string(id) + "\" name=\"" + Attr(name) + " " + std::to_string(id ? id - 1 : 0) + "\"";
  if (!descr.empty()) out += " descr=\"" + Attr(descr) + "\"";
  return out + "/>";
}

std::vector<std::string> Lines(std::string_view text) {
  std::vector<std::string> out;
  size_t i = 0;
  for (;;) {
    size_t e = text.find('\n', i);
    std::string line(text.substr(i, e == std::string_view::npos ? std::string_view::npos : e - i));
    if (!line.empty() && line.back() == '\r') line.pop_back();
    out.push_back(std::move(line));
    if (e == std::string_view::npos) break;
    i = e + 1;
  }
  return out;
}

// A new shape's paragraphs: `pPr` before the runs (or none), Excel's run properties.
std::string Paragraphs(std::string_view text, std::string_view pPr) {
  std::string out;
  for (const std::string& line : Lines(text)) {
    out += "<a:p>";
    out += pPr;
    if (!line.empty()) {
      out += "<a:r><a:rPr lang=\"en-US\" sz=\"1100\"/><a:t>";
      pkg::AppendEscapedText(out, XmlChars(line));
      out += "</a:t></a:r>";
    }
    out += "<a:endParaRPr lang=\"en-US\" sz=\"1100\"/></a:p>";
  }
  return out;
}

std::string_view Local(const pugi::xml_node& n) {
  std::string_view name = n.name();
  const size_t colon = name.find(':');
  return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

std::string PrefixOf(const pugi::xml_node& n) {
  std::string_view name = n.name();
  const size_t colon = name.find(':');
  return colon == std::string_view::npos ? std::string() : std::string(name.substr(0, colon + 1));
}

pugi::xml_node Child(const pugi::xml_node& n, std::string_view local) {
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && Local(c) == local) return c;
  return {};
}

// The object an anchor places: its sp, cxnSp, pic, graphicFrame or grpSp (the first one, inside
// mc:AlternateContent's Choice too).
pugi::xml_node ObjectOf(const pugi::xml_node& root) {
  for (pugi::xml_node c = root.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    const std::string_view l = Local(c);
    if (l == "sp" || l == "cxnSp" || l == "pic" || l == "graphicFrame" || l == "grpSp") return c;
    if (l == "AlternateContent" || l == "Choice" || l == "twoCellAnchor" || l == "oneCellAnchor" || l == "absoluteAnchor")
      if (pugi::xml_node o = ObjectOf(c)) return o;
  }
  return {};
}

pugi::xml_node CNvPrOf(const pugi::xml_node& object) {
  for (pugi::xml_node c = object.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && Local(c).substr(0, 2) == "nv")
      if (pugi::xml_node p = Child(c, "cNvPr")) return p;
  return {};
}

bool IsFill(std::string_view l) {
  return l == "noFill" || l == "solidFill" || l == "gradFill" || l == "blipFill" || l == "pattFill" || l == "grpFill";
}

uint32_t RgbOf(const pugi::xml_node& solidFill) {
  pugi::xml_node c = Child(solidFill, "srgbClr");
  return c ? static_cast<uint32_t>(std::strtoul(c.attribute("val").value(), nullptr, 16)) & 0xFFFFFF : 0;
}

void ReadPaint(const pugi::xml_node& holder, ObjectFormat::Paint& paint, uint32_t& rgb) {
  paint = ObjectFormat::Paint::Automatic;
  rgb = 0;
  for (pugi::xml_node c = holder.first_child(); c; c = c.next_sibling()) {
    const std::string_view l = Local(c);
    if (l == "noFill") paint = ObjectFormat::Paint::None;
    else if (l == "solidFill") {
      paint = ObjectFormat::Paint::Solid;
      rgb = RgbOf(c);
    }
  }
}

// Replaces `holder`'s fill child (a fill element among its children) with `paint`, put after
// `after` (or first when null); Automatic just takes it out.
void WritePaint(pugi::xml_node holder, ObjectFormat::Paint paint, uint32_t rgb, const std::string& a, pugi::xml_node after) {
  for (pugi::xml_node c = holder.first_child(); c;) {
    pugi::xml_node next = c.next_sibling();
    if (c.type() == pugi::node_element && IsFill(Local(c))) {
      if (c == after) after = c.previous_sibling();
      holder.remove_child(c);
    }
    c = next;
  }
  if (paint == ObjectFormat::Paint::Automatic) return;
  pugi::xml_node f = after ? holder.insert_child_after((a + (paint == ObjectFormat::Paint::None ? "noFill" : "solidFill")).c_str(), after)
                           : holder.prepend_child((a + (paint == ObjectFormat::Paint::None ? "noFill" : "solidFill")).c_str());
  if (paint == ObjectFormat::Paint::Solid) {
    char hex[8];
    std::snprintf(hex, sizeof hex, "%06X", rgb & 0xFFFFFF);
    f.append_child((a + "srgbClr").c_str()).append_attribute("val").set_value(hex);
  }
}

}  // namespace

const char* ShapeName(ShapeKind kind) {
  switch (kind) {
    case ShapeKind::Rectangle: return "Rectangle";
    case ShapeKind::RoundedRectangle: return "Rectangle: Rounded Corners";
    case ShapeKind::Oval: return "Oval";
    case ShapeKind::Line: return "Straight Connector";
    case ShapeKind::Arrow: return "Straight Arrow Connector";
    case ShapeKind::TextBox: return "TextBox";
  }
  return "Shape";
}

std::string ShapeAnchor(ShapeKind kind, uint32_t id, std::string_view text) {
  std::string out = "<xdr:twoCellAnchor" + std::string(kNs) + ">" + std::string(kCorners);
  if (kind == ShapeKind::Line || kind == ShapeKind::Arrow) {
    const bool arrow = kind == ShapeKind::Arrow;
    out += "<xdr:cxnSp macro=\"\"><xdr:nvCxnSpPr>" + CNvPr(id, ShapeName(kind)) + "<xdr:cNvCxnSpPr/></xdr:nvCxnSpPr><xdr:spPr>" +
           std::string(kXfrm) + "<a:prstGeom prst=\"" + (arrow ? "straightConnector1" : "line") + "\"><a:avLst/></a:prstGeom>" +
           (arrow ? "<a:ln><a:tailEnd type=\"triangle\"/></a:ln>" : "") +
           "</xdr:spPr><xdr:style><a:lnRef idx=\"1\"><a:schemeClr val=\"accent1\"/></a:lnRef><a:fillRef idx=\"0\"><a:schemeClr "
           "val=\"accent1\"/></a:fillRef><a:effectRef idx=\"0\"><a:schemeClr val=\"accent1\"/></a:effectRef><a:fontRef "
           "idx=\"minor\"><a:schemeClr val=\"tx1\"/></a:fontRef></xdr:style></xdr:cxnSp>";
  } else if (kind == ShapeKind::TextBox) {
    out += "<xdr:sp macro=\"\" textlink=\"\"><xdr:nvSpPr>" + CNvPr(id, ShapeName(kind)) +
           "<xdr:cNvSpPr txBox=\"1\"/></xdr:nvSpPr><xdr:spPr>" + std::string(kXfrm) +
           "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill><a:ln "
           "w=\"9525\" cmpd=\"sng\"><a:solidFill><a:schemeClr val=\"lt1\"><a:shade val=\"50000\"/></a:schemeClr></a:solidFill>"
           "</a:ln></xdr:spPr><xdr:style><a:lnRef idx=\"0\"><a:scrgbClr r=\"0\" g=\"0\" b=\"0\"/></a:lnRef><a:fillRef "
           "idx=\"0\"><a:scrgbClr r=\"0\" g=\"0\" b=\"0\"/></a:fillRef><a:effectRef idx=\"0\"><a:scrgbClr r=\"0\" g=\"0\" "
           "b=\"0\"/></a:effectRef><a:fontRef idx=\"minor\"><a:schemeClr val=\"dk1\"/></a:fontRef></xdr:style><xdr:txBody>"
           "<a:bodyPr vertOverflow=\"clip\" horzOverflow=\"clip\" wrap=\"square\" rtlCol=\"0\" anchor=\"t\"/><a:lstStyle/>" +
           Paragraphs(text, "") + "</xdr:txBody></xdr:sp>";
  } else {
    const char* prst = kind == ShapeKind::Oval ? "ellipse" : kind == ShapeKind::RoundedRectangle ? "roundRect" : "rect";
    out += "<xdr:sp macro=\"\" textlink=\"\"><xdr:nvSpPr>" + CNvPr(id, ShapeName(kind)) + "<xdr:cNvSpPr/></xdr:nvSpPr><xdr:spPr>" +
           std::string(kXfrm) + "<a:prstGeom prst=\"" + prst +
           "\"><a:avLst/></a:prstGeom></xdr:spPr><xdr:style><a:lnRef idx=\"2\"><a:schemeClr val=\"accent1\"><a:shade "
           "val=\"15000\"/></a:schemeClr></a:lnRef><a:fillRef idx=\"1\"><a:schemeClr val=\"accent1\"/></a:fillRef><a:effectRef "
           "idx=\"0\"><a:schemeClr val=\"accent1\"/></a:effectRef><a:fontRef idx=\"minor\"><a:schemeClr val=\"lt1\"/></a:fontRef>"
           "</xdr:style><xdr:txBody><a:bodyPr vertOverflow=\"clip\" horzOverflow=\"clip\" rtlCol=\"0\" anchor=\"ctr\"/><a:lstStyle/>" +
           Paragraphs(text, "<a:pPr algn=\"ctr\"/>") + "</xdr:txBody></xdr:sp>";
  }
  return out + "<xdr:clientData/></xdr:twoCellAnchor>";
}

std::string PictureAnchor(uint32_t id, std::string_view relId, std::string_view altText) {
  return "<xdr:twoCellAnchor editAs=\"oneCell\"" + std::string(kNs) + ">" + std::string(kCorners) + "<xdr:pic><xdr:nvPicPr>" +
         CNvPr(id, "Picture", altText) +
         "<xdr:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></xdr:cNvPicPr></xdr:nvPicPr><xdr:blipFill><a:blip "
         "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" r:embed=\"" +
         Attr(relId) + "\"/><a:stretch><a:fillRect/></a:stretch></xdr:blipFill><xdr:spPr>" + std::string(kXfrm) +
         "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></xdr:spPr></xdr:pic><xdr:clientData/></xdr:twoCellAnchor>";
}

bool PictureType(std::string_view bytes, std::string& extension, std::string& contentType) {
  switch (draw::SniffImageFormat(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size())) {
    case draw::ImageFormat::Png: extension = ".png", contentType = "image/png"; return true;
    case draw::ImageFormat::Jpeg: extension = ".jpeg", contentType = "image/jpeg"; return true;
    case draw::ImageFormat::Gif: extension = ".gif", contentType = "image/gif"; return true;
    case draw::ImageFormat::Bmp: extension = ".bmp", contentType = "image/bmp"; return true;
    default: return false;
  }
}

ObjectFormat ReadObjectFormat(std::string_view anchor) {
  ObjectFormat f;
  pugi::xml_document doc;
  if (!doc.load_buffer(anchor.data(), anchor.size(), pugi::parse_default | pugi::parse_ws_pcdata)) return f;
  pugi::xml_node o = ObjectOf(doc);
  if (!o) return f;
  const std::string_view l = Local(o);
  f.kind = l == "sp" ? ObjectFormat::Kind::Shape : l == "cxnSp" ? ObjectFormat::Kind::Connector
                                                  : l == "pic" ? ObjectFormat::Kind::Picture : ObjectFormat::Kind::Other;
  if (pugi::xml_node c = CNvPrOf(o)) {
    f.name = c.attribute("name").value();
    f.altText = c.attribute("descr").value();
  }
  if (pugi::xml_node sp = Child(o, "spPr")) {
    ReadPaint(sp, f.fill, f.fillRgb);
    if (pugi::xml_node ln = Child(sp, "ln")) {
      ReadPaint(ln, f.line, f.lineRgb);
      if (pugi::xml_attribute w = ln.attribute("w")) f.lineWidth = w.as_double() / 12700.0;
    }
  }
  if (pugi::xml_node body = Child(o, "txBody")) {
    f.hasText = true;
    bool first = true;
    for (pugi::xml_node p = body.first_child(); p; p = p.next_sibling()) {
      if (Local(p) != "p") continue;
      if (!first) f.text += '\n';
      first = false;
      for (pugi::xml_node r = p.first_child(); r; r = r.next_sibling()) {
        const std::string_view rl = Local(r);
        if (rl == "r" || rl == "fld") f.text += Child(r, "t").text().get();
        else if (rl == "br") f.text += '\n';
      }
    }
  } else {
    f.hasText = f.kind == ObjectFormat::Kind::Shape;
  }
  return f;
}

std::string FormatObject(std::string_view anchor, const ObjectFormat& f, uint32_t what) {
  pugi::xml_document doc;
  if (!doc.load_buffer(anchor.data(), anchor.size(), pugi::parse_default | pugi::parse_ws_pcdata | pugi::parse_comments)) return {};
  pugi::xml_node o = ObjectOf(doc);
  if (!o) return {};
  const std::string x = PrefixOf(o);  // xdr:
  pugi::xml_node sp = Child(o, "spPr");
  // DrawingML's prefix, from what the shape's properties hold (a: as Excel writes it).
  std::string a = "a:";
  if (sp)
    for (pugi::xml_node c = sp.first_child(); c; c = c.next_sibling())
      if (c.type() == pugi::node_element) {
        a = PrefixOf(c);
        break;
      }
  if ((what & kFormatAltText))
    if (pugi::xml_node c = CNvPrOf(o)) {
      const std::string alt = XmlChars(f.altText);
      if (alt.empty()) c.remove_attribute("descr");
      else (c.attribute("descr") ? c.attribute("descr") : c.append_attribute("descr")).set_value(alt.c_str());
    }
  if ((what & (kFormatFill | kFormatLine)) && !sp) sp = o.insert_child_after((x + "spPr").c_str(), Child(o, Local(o) == "pic" ? "blipFill" : "nvSpPr"));
  // The shape properties' order: xfrm, the geometry, the fill, the outline, then effects.
  pugi::xml_node geometry = sp ? Child(sp, "prstGeom") : pugi::xml_node();
  if (sp && !geometry) geometry = Child(sp, "custGeom");
  if (sp && !geometry) geometry = Child(sp, "xfrm");
  if ((what & kFormatFill) && sp && Local(o) != "cxnSp") WritePaint(sp, f.fill, f.fillRgb, a, geometry);
  if ((what & kFormatLine) && sp) {
    pugi::xml_node ln = Child(sp, "ln");
    if (!ln && f.line != ObjectFormat::Paint::Automatic) {
      pugi::xml_node after = geometry;
      for (pugi::xml_node c = sp.first_child(); c; c = c.next_sibling())
        if (c.type() == pugi::node_element && IsFill(Local(c))) after = c;
      ln = after ? sp.insert_child_after((a + "ln").c_str(), after) : sp.prepend_child((a + "ln").c_str());
    }
    if (ln) {
      WritePaint(ln, f.line, f.lineRgb, a, pugi::xml_node());
      if (f.lineWidth > 0) {
        const std::string w = std::to_string(static_cast<int64_t>(std::llround(f.lineWidth * 12700)));
        (ln.attribute("w") ? ln.attribute("w") : ln.prepend_attribute("w")).set_value(w.c_str());
      }
      if (!ln.first_child() && !ln.first_attribute()) sp.remove_child(ln);
    }
  }
  if ((what & kFormatText) && Local(o) == "sp") {
    pugi::xml_node body = Child(o, "txBody");
    if (!body && !f.text.empty()) {
      pugi::xml_node after = Child(o, "style");
      if (!after) after = sp;
      body = o.insert_child_after((x + "txBody").c_str(), after);
      body.append_child((a + "bodyPr").c_str());
      body.append_child((a + "lstStyle").c_str());
    }
    if (body) {
      // The first paragraph's properties and run properties, for every paragraph.
      pugi::xml_document keep;
      pugi::xml_node pPr, rPr, endPr;
      if (pugi::xml_node p = Child(body, "p")) {
        if (pugi::xml_node n = Child(p, "pPr")) pPr = keep.append_copy(n);
        for (pugi::xml_node r = p.first_child(); r && !rPr; r = r.next_sibling())
          if (Local(r) == "r")
            if (pugi::xml_node n = Child(r, "rPr")) rPr = keep.append_copy(n);
        if (pugi::xml_node n = Child(p, "endParaRPr")) endPr = keep.append_copy(n);
      }
      if (!rPr && endPr) {
        rPr = keep.append_copy(endPr);
        rPr.set_name((a + "rPr").c_str());
      }
      for (pugi::xml_node c = body.first_child(); c;) {
        pugi::xml_node next = c.next_sibling();
        if (c.type() == pugi::node_element && Local(c) == "p") body.remove_child(c);
        c = next;
      }
      for (const std::string& line : Lines(XmlChars(f.text))) {
        pugi::xml_node p = body.append_child((a + "p").c_str());
        if (pPr) p.append_copy(pPr);
        if (!line.empty()) {
          pugi::xml_node r = p.append_child((a + "r").c_str());
          if (rPr) r.append_copy(rPr);
          else {
            pugi::xml_node n = r.append_child((a + "rPr").c_str());
            n.append_attribute("lang").set_value("en-US");
            n.append_attribute("sz").set_value("1100");
          }
          r.append_child((a + "t").c_str()).text().set(line.c_str());
        }
        if (endPr) p.append_copy(endPr);
      }
    }
  }
  struct Out : pugi::xml_writer {
    std::string s;
    void write(const void* d, size_t n) override { s.append(static_cast<const char*>(d), n); }
  } out;
  for (pugi::xml_node c = doc.first_child(); c; c = c.next_sibling()) c.print(out, "", pugi::format_raw);
  return out.s;
}

}  // namespace fw::sheet::data
