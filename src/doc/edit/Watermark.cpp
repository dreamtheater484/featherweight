#include "doc/edit/Watermark.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>

#include "doc/docx/Docx.h"
#include "doc/edit/DrawingEdit.h"
#include "doc/edit/HeaderFooter.h"
#include "draw/Images.h"
#include "pkg/XmlDom.h"

namespace fw::doc::edit {

using namespace dx;

namespace {

constexpr std::string_view kTextId = "PowerPlusWaterMarkObject";
constexpr std::string_view kPictureId = "WordPictureWatermark";
constexpr const char* kVmlNs = "xmlns:v=\"urn:schemas-microsoft-com:vml\" xmlns:o=\"urn:schemas-microsoft-com:office:office\"";

// Word's shape types for WordArt text (136) and pictures (75), as it writes them.
constexpr const char* kTextType =
    "<v:shapetype id=\"_x0000_t136\" coordsize=\"21600,21600\" o:spt=\"136\" adj=\"10800\" "
    "path=\"m@7,l@8,m@5,21600l@6,21600e\"><v:formulas><v:f eqn=\"sum #0 0 10800\"/><v:f eqn=\"prod #0 2 1\"/>"
    "<v:f eqn=\"sum 21600 0 @1\"/><v:f eqn=\"sum 0 0 @2\"/><v:f eqn=\"sum 21600 0 @3\"/><v:f eqn=\"if @0 @3 0\"/>"
    "<v:f eqn=\"if @0 21600 @1\"/><v:f eqn=\"if @0 0 @2\"/><v:f eqn=\"if @0 @4 21600\"/><v:f eqn=\"mid @5 @6\"/>"
    "<v:f eqn=\"mid @8 @5\"/><v:f eqn=\"mid @7 @8\"/><v:f eqn=\"mid @6 @7\"/><v:f eqn=\"sum @6 0 @5\"/></v:formulas>"
    "<v:path textpathok=\"t\" o:connecttype=\"custom\" o:connectlocs=\"@9,0;@10,10800;@11,21600;@12,10800\" "
    "o:connectangles=\"270,180,90,0\"/><v:textpath on=\"t\" fitshape=\"t\"/><v:handles><v:h position=\"#0,bottomRight\" "
    "xrange=\"6629,14971\"/></v:handles><o:lock v:ext=\"edit\" text=\"t\" shapetype=\"t\"/></v:shapetype>";
constexpr const char* kPictureType =
    "<v:shapetype id=\"_x0000_t75\" coordsize=\"21600,21600\" o:spt=\"75\" o:preferrelative=\"t\" "
    "path=\"m@4@5l@4@11@9@11@9@5xe\" filled=\"f\" stroked=\"f\"><v:stroke joinstyle=\"miter\"/><v:formulas>"
    "<v:f eqn=\"if lineDrawn pixelLineWidth 0\"/><v:f eqn=\"sum @0 1 0\"/><v:f eqn=\"sum 0 0 @1\"/><v:f eqn=\"prod @2 1 2\"/>"
    "<v:f eqn=\"prod @3 21600 pixelWidth\"/><v:f eqn=\"prod @3 21600 pixelHeight\"/><v:f eqn=\"sum @0 0 1\"/>"
    "<v:f eqn=\"prod @6 1 2\"/><v:f eqn=\"prod @7 21600 pixelWidth\"/><v:f eqn=\"sum @8 21600 0\"/>"
    "<v:f eqn=\"prod @7 21600 pixelHeight\"/><v:f eqn=\"sum @10 21600 0\"/></v:formulas><v:path o:extrusionok=\"f\" "
    "gradientshapeok=\"t\" o:connecttype=\"rect\"/><o:lock v:ext=\"edit\" aspectratio=\"t\"/></v:shapetype>";
// Centered on the margins, behind the text, as Word places a watermark.
constexpr const char* kPlace =
    "mso-position-horizontal:center;mso-position-horizontal-relative:margin;mso-position-vertical:center;"
    "mso-position-vertical-relative:margin";

// A shape type declaring the VML namespaces it uses (it's kept as a fragment of its own).
std::string WithNs(const char* type) {
  std::string s(type);
  s.insert(std::string_view("<v:shapetype").size(), std::string(" ") + kVmlNs);
  return s;
}

bool IsWatermark(const Document& doc, const Drawing& d) {
  if (d.pos.para >= doc.Paragraphs().size() || d.kind != DrawingKind::Vml) return false;
  return d.name.rfind(kTextId, 0) == 0 || d.name.rfind(kPictureId, 0) == 0;
}

uint32_t FirstWatermark(const Document& doc) {
  for (uint32_t i = 0; i < doc.Drawings().size(); ++i)
    if (IsWatermark(doc, doc.Drawings()[i])) return i;
  return kNone;
}

std::string Pt(double pt) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.2f", pt);
  std::string s = buf;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s + "pt";
}

// Section `s`'s text area, in points.
std::pair<double, double> TextArea(const Document& doc, uint32_t s) {
  const SectionProps& p = doc.Sections()[std::min<size_t>(s, doc.Sections().size() - 1)].props;
  const double w = (p.pageWidth.Value() - p.marginStart.Value() - p.marginEnd.Value() - p.gutter.Value()) / 20.0;
  const double h = (p.pageHeight.Value() - p.marginTop.Value() - p.marginBottom.Value()) / 20.0;
  return {std::max(w, 36.0), std::max(h, 36.0)};
}

// The header stories each section shows on each kind of its pages, made where there are none, with
// a section of each.
bool Headers(Editor& editor, std::vector<std::pair<uint32_t, uint32_t>>& out, std::u16string& why) {
  for (uint32_t s = 0; s < editor.Doc().Sections().size(); ++s) {
    std::vector<HdrFtrType> types{HdrFtrType::Default};
    if (DifferentFirstPage(editor.Doc(), s)) types.push_back(HdrFtrType::First);
    if (editor.Doc().Settings().evenAndOddHeaders) types.push_back(HdrFtrType::Even);
    for (HdrFtrType t : types) {
      const uint32_t story = EnsureHeaderFooter(editor, s, false, t, why);
      if (story == kNone) return false;
      if (std::none_of(out.begin(), out.end(), [&](const auto& h) { return h.first == story; })) out.emplace_back(story, s);
    }
  }
  if (out.empty()) why = u"This document has no pages to put a watermark on.";
  return !out.empty();
}

// Its handle, as the reader makes one for this VML.
bool HandleOf(const std::string& xml, const Document& doc, Drawing& info) {
  pkg::XmlDom dom;
  if (!dom.Parse("<fw>" + xml + "</fw>", doc.Package().GetLimits()).Ok()) return false;
  info.kind = DrawingKind::Vml;
  docx::ReadVmlFrame(dom.Root(), info);
  return true;
}

Result RemoveAll(Editor& editor) {
  Result r;
  for (int guard = 0; guard < 4096; ++guard) {
    const uint32_t d = FirstWatermark(editor.Doc());
    if (d == kNone) break;
    const Pos at = editor.Doc().Drawings()[d].pos;
    r = editor.Delete(at, Pos{at.para, at.offset + 1});
    if (!r) return r;
  }
  return r;
}

// Puts `make(story, section, n)`'s VML at the start of every header, in place of any watermark:
// one undo step, all of it or none.
Result Place(Editor& editor, const char16_t* name,
             const std::function<Result(uint32_t story, uint32_t section, uint32_t n)>& make) {
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  editor.BeginGroup(name);
  Result r = RemoveAll(editor);
  std::vector<std::pair<uint32_t, uint32_t>> headers;
  if (r && !Headers(editor, headers, why)) r = Fail(why);
  uint32_t n = 0;
  for (const auto& [story, section] : headers) {
    if (!r) break;
    r = make(story, section, n++);
  }
  if (r) editor.EndGroup();
  else editor.CancelGroup();  // all of it or none
  return r;
}

uint32_t ColorOf(std::string_view v) {
  if (!v.empty() && v[0] == '#') {
    char* end = nullptr;
    const std::string hex(v.substr(1));
    const unsigned long c = std::strtoul(hex.c_str(), &end, 16);
    if (end && *end == 0 && hex.size() == 6) return static_cast<uint32_t>(c);
    if (end && *end == 0 && hex.size() == 3)
      return static_cast<uint32_t>(((c >> 8) & 0xF) * 0x110000 + ((c >> 4) & 0xF) * 0x1100 + (c & 0xF) * 0x11);
  }
  if (v == "gray" || v == "grey") return 0x808080;
  if (v == "black") return 0x000000;
  if (v == "red") return 0xFF0000;
  if (v == "blue") return 0x0000FF;
  return 0xC0C0C0;  // Word's "silver"
}

}  // namespace

WatermarkInfo FindWatermark(const Document& doc) {
  WatermarkInfo info;
  const uint32_t i = FirstWatermark(doc);
  if (i == kNone) return info;
  const Drawing& d = doc.Drawings()[i];
  info.kind = d.name.rfind(kTextId, 0) == 0 ? WatermarkInfo::Kind::Text : WatermarkInfo::Kind::Picture;
  pkg::XmlDom dom;
  if (!dom.Parse("<fw>" + doc.DrawingXml(d) + "</fw>", doc.Package().GetLimits()).Ok()) return info;
  pugi::xml_node shape;
  for (pugi::xml_node c = dom.Root().first_child(); c && !shape; c = c.next_sibling())
    if (pkg::NamespaceOf(c) == kV && pkg::LocalNameOf(c) != "shapetype") shape = c;
  const std::string_view style = shape.attribute("style").value();
  info.diagonal = style.find("rotation:") != std::string_view::npos && style.find("rotation:0;") == std::string_view::npos;
  info.color = ColorOf(shape.attribute("fillcolor").value());
  for (pugi::xml_node c = shape.first_child(); c; c = c.next_sibling()) {
    if (pkg::IsElement(c, kV, "fill")) {
      const std::string_view o = c.attribute("opacity").value();
      info.semitransparent = !o.empty() && o != "1" && o != "1.0";
    } else if (pkg::IsElement(c, kV, "textpath")) {
      info.text = Utf16(c.attribute("string").value());
      const std::string_view ts = c.attribute("style").value();
      if (const size_t at = ts.find("font-family:"); at != std::string_view::npos) {
        std::string_view f = ts.substr(at + 12);
        f = f.substr(0, f.find(';'));
        while (!f.empty() && (f.front() == '"' || f.front() == '\'' || f.front() == ' ')) f.remove_prefix(1);
        while (!f.empty() && (f.back() == '"' || f.back() == '\'' || f.back() == ' ')) f.remove_suffix(1);
        info.font = std::string(f);
      }
    } else if (pkg::IsElement(c, kV, "imagedata")) {
      info.washout = !std::string_view(c.attribute("gain").value()).empty();
    }
  }
  return info;
}

Result SetTextWatermark(Editor& editor, const TextWatermark& mark) {
  std::u16string text = mark.text;
  while (!text.empty() && (text.back() == u' ' || text.back() == u'\t')) text.pop_back();
  while (!text.empty() && (text.front() == u' ' || text.front() == u'\t')) text.erase(0, 1);
  if (text.empty()) return Fail(u"Type the watermark's text.");
  const std::string utf8 = Utf8(text);
  std::string font = mark.font.empty() ? std::string("Calibri") : mark.font;
  if (!XmlSafe(utf8) || !XmlSafe(font) || font.size() > 256 || utf8.size() > 4096) return Fail(u"That text can't be a watermark.");
  // The shape the text takes, as wide as it is long: across the page's diagonal, or its width.
  const double aspect = mark.aspect > 0.1 && std::isfinite(mark.aspect) ? std::min(mark.aspect, 200.0)
                                                                        : std::max(1.0, 0.6 * static_cast<double>(text.size()));
  return Place(editor, u"Watermark", [&](uint32_t story, uint32_t section, uint32_t n) {
    const Document& doc = editor.Doc();
    const std::vector<uint32_t> paras = doc.StoryParagraphs(story);
    if (paras.empty()) return Fail(u"A header has no paragraph for the watermark.");
    const auto [aw, ah] = TextArea(doc, section);
    double w, h;
    if (mark.diagonal) {
      w = std::min(aw, ah) * std::sqrt(2.0) / (1 + 1 / aspect);
      h = w / aspect;
    } else {
      w = aw;
      h = std::min(ah, w / aspect);
      w = h * aspect;
    }
    const std::string style = "position:absolute;margin-left:0;margin-top:0;width:" + Pt(w) + ";height:" + Pt(h) +
                              (mark.diagonal ? ";rotation:315" : "") + ";z-index:-251651072;" + kPlace;
    char color[8];
    std::snprintf(color, sizeof color, "%06X", mark.color & 0xFFFFFF);
    const std::string xml =
        WithNs(kTextType) + "<v:shape " + kVmlNs +
        " xmlns:w10=\"urn:schemas-microsoft-com:office:word\" id=\"" + std::string(kTextId) + std::to_string(100000 + n) +
        "\" o:spid=\"_x0000_s" + std::to_string(2049 + n) + "\" type=\"#_x0000_t136\" style=\"" + style +
        "\" o:allowincell=\"f\" fillcolor=\"#" + color + "\" stroked=\"f\">" +
        (mark.semitransparent ? "<v:fill opacity=\".5\"/>" : "") + "<v:textpath style=\"" +
        Escape("font-family:\"" + font + "\";font-size:1pt") + "\" string=\"" + Escape(utf8) +
        "\"/><w10:wrap anchorx=\"margin\" anchory=\"margin\"/></v:shape>";
    Drawing info;
    if (!HandleOf(xml, doc, info)) return Fail(u"That text can't be a watermark.");
    return editor.InsertDrawing(Pos{paras[0], 0}, xml, info, true);
  });
}

Result SetPictureWatermark(Editor& editor, const std::vector<uint8_t>& bytes, bool washout) {
  Document& doc = const_cast<Document&>(editor.Doc());
  std::string error;
  std::shared_ptr<draw::Image> image = draw::Image::Open(bytes, draw::ImageLimits::Default(), &error);
  if (!image) return Fail(error.empty() ? u"This picture can't be read." : Utf16(error));
  Media media;
  switch (image->Format()) {
    case draw::ImageFormat::Png: media = {bytes, "png", "image/png"}; break;
    case draw::ImageFormat::Jpeg: media = {bytes, "jpeg", "image/jpeg"}; break;
    case draw::ImageFormat::Gif: media = {bytes, "gif", "image/gif"}; break;
    case draw::ImageFormat::Tiff: media = {bytes, "tiff", "image/tiff"}; break;
    case draw::ImageFormat::Bmp: {
      std::shared_ptr<const draw::Bitmap> px = image->Pixels();
      std::vector<uint8_t> png = px ? EncodePng(*px) : std::vector<uint8_t>{};
      if (png.empty()) return Fail(u"This picture can't be read.");
      media = {std::move(png), "png", "image/png"};
      break;
    }
    default: return Fail(u"A watermark picture is a PNG, JPEG, GIF, BMP or TIFF file.");
  }
  const draw::Size natural = image->NaturalSize();
  if (!(natural.width > 0 && natural.height > 0 && std::isfinite(natural.width) && std::isfinite(natural.height)))
    return Fail(u"This picture has no size.");
  return Place(editor, u"Watermark", [&](uint32_t story, uint32_t section, uint32_t n) {
    const std::vector<uint32_t> paras = doc.StoryParagraphs(story);
    const Part* part = PartOf(doc, doc.Stories()[story].part);
    if (paras.empty() || !part) return Fail(u"A header has no paragraph for the watermark.");
    if (Strict(*part)) return Fail(u"Pictures can't be added to documents saved as Strict Open XML yet.");
    // Its part and the header's relationship to it: one package step for each header.
    auto step = std::make_shared<PackageStep>();
    step->doc = &doc;
    const std::string rid = PlaceMedia(doc, *part, media, *step);
    const auto [aw, ah] = TextArea(doc, section);
    const double k = std::min(aw / natural.width, ah / natural.height);
    const std::string style = "position:absolute;margin-left:0;margin-top:0;width:" + Pt(natural.width * k) + ";height:" +
                              Pt(natural.height * k) + ";z-index:-251650048;" + kPlace;
    const std::string xml = WithNs(kPictureType) + "<v:shape " + kVmlNs +
                            " xmlns:r=\"" + std::string(kR) + "\" id=\"" + std::string(kPictureId) +
                            std::to_string(100000 + n) + "\" o:spid=\"_x0000_s" + std::to_string(2049 + n) +
                            "\" type=\"#_x0000_t75\" style=\"" + style + "\" o:allowincell=\"f\"><v:imagedata r:id=\"" + rid +
                            "\" o:title=\"\"" + (washout ? " gain=\"19661f\" blacklevel=\"22938f\"" : "") + "/></v:shape>";
    Drawing info;
    if (!HandleOf(xml, doc, info)) return Fail(u"This picture can't be a watermark.");
    const Pos at{paras[0], 0};
    Result r = editor.InsertDrawing(at, xml, info, true);
    if (r && step->Any()) {
      step->parts = {doc.Stories()[story].part};
      step->first = at.para;
      step->count = 1;
      step->Redo();
      editor.RecordStep(u"Watermark", step, at);
    }
    return r;
  });
}

Result RemoveWatermark(Editor& editor) {
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  if (FirstWatermark(editor.Doc()) == kNone) return Fail(u"This document has no watermark.");
  editor.BeginGroup(u"Remove watermark");
  Result r = RemoveAll(editor);
  if (r) editor.EndGroup();
  else editor.CancelGroup();
  return r;
}

}  // namespace fw::doc::edit
