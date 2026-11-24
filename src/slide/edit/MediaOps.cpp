// Media operations (P3, p3-present): Insert > Video and Insert > Audio from a file, embedded as
// PowerPoint 2021 inserts them (the shape, relationships and timing as in the corpus decks PowerPoint
// wrote: FeatherweightNotes\p3\p3-present-state.md).
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

#include "pkg/XmlDom.h"
#include "pkg/XmlWriter.h"
#include "slide/edit/EditInternal.h"
#include "slide/edit/ShapeXml.h"

namespace fw::slide::edit {

namespace {

constexpr std::string_view kRelVideo = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/video";
constexpr std::string_view kRelAudio = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/audio";
constexpr std::string_view kRelMedia = "http://schemas.microsoft.com/office/2007/relationships/media";
constexpr std::string_view kNs =
    " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
    " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
    " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"";
constexpr std::string_view kPNs = " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"";

// The files PowerPoint inserts, with the content types it writes for them.
std::string_view MediaType(std::string_view ext, bool video) {
  static const std::pair<std::string_view, std::string_view> kVideo[] = {
      {"mp4", "video/mp4"},       {"m4v", "video/mp4"},       {"mov", "video/quicktime"}, {"wmv", "video/x-ms-wmv"},
      {"avi", "video/avi"},       {"mpg", "video/mpeg"},      {"mpeg", "video/mpeg"},     {"asf", "video/x-ms-asf"}};
  static const std::pair<std::string_view, std::string_view> kAudio[] = {
      {"mp3", "audio/mpeg"},      {"m4a", "audio/mp4"},       {"wav", "audio/wav"},       {"wma", "audio/x-ms-wma"},
      {"mid", "audio/midi"},      {"midi", "audio/midi"},     {"aif", "audio/aiff"},      {"aiff", "audio/aiff"},
      {"au", "audio/basic"}};
  if (video) {
    for (const auto& [e, t] : kVideo)
      if (e == ext) return t;
  } else {
    for (const auto& [e, t] : kAudio)
      if (e == ext) return t;
  }
  return {};
}

std::string Attr(std::string_view s) {
  std::string out;
  pkg::AppendEscapedAttribute(out, s);
  return out;
}

// One p: element as XML, with the p: namespace declared on it.
std::string WithNs(std::string xml) {
  xml.insert(xml.find_first_of(" />", 1), kPNs);
  return xml;
}

// A fragment copied into `parent` before `before` (null: at the end). Its namespace declarations go
// where the part already declares the same.
pugi::xml_node Put(pugi::xml_node parent, const std::string& xml, pugi::xml_node before = {}) {
  pugi::xml_document d;
  if (!parent || !ParseFragment(xml, d)) return {};
  pugi::xml_node c = InsertCopy(parent, d.document_element(), before);
  for (pugi::xml_attribute a = c.first_attribute(), n; a; a = n) {
    n = a.next_attribute();
    std::string_view name = a.name();
    if (!name.starts_with("xmlns:")) continue;
    std::optional<std::string_view> there = pkg::LookupNamespace(parent, name.substr(6));
    if (there && *there == a.value()) c.remove_attribute(a);
  }
  return c;
}

uint32_t MaxTimeNodeId(pugi::xml_node n) {
  uint32_t best = 0;
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    if (LocalNameOf(c) == "cTn") {
      double v = Number(c, "id", 0);
      if (v > best && v < 4294967295.0) best = static_cast<uint32_t>(v);
    }
    best = std::max(best, MaxTimeNodeId(c));
  }
  return best;
}

// What PowerPoint 2021 adds to a slide's timing for Insert > Video / Audio (Start: In Click Sequence):
// a play step at the end of the main sequence, the media node, and a click on the shape toggling
// pause. False when the slide's timing has no place for them.
bool AddMediaTiming(pugi::xml_node sld, uint32_t spid, bool video, double durationMs) {
  if (!sld) return false;
  pugi::xml_node timing = P(sld, "timing");
  if (!timing) timing = Put(sld, WithNs("<p:timing/>"), P(sld, "extLst"));
  pugi::xml_node tnLst = P(timing, "tnLst");
  if (!tnLst) tnLst = Put(timing, WithNs("<p:tnLst/>"), timing.first_child());
  uint32_t next = MaxTimeNodeId(timing) + 1;
  auto id = [&] { return "\"" + std::to_string(next++) + "\""; };
  pugi::xml_node rootPar = P(tnLst, "par");
  if (!rootPar) rootPar = Put(tnLst, WithNs("<p:par><p:cTn id=" + id() + " dur=\"indefinite\" restart=\"never\" nodeType=\"tmRoot\"/></p:par>"));
  pugi::xml_node rootCTn = P(rootPar, "cTn");
  if (!rootCTn) return false;
  pugi::xml_node list = P(rootCTn, "childTnLst");
  if (!list) list = Put(rootCTn, WithNs("<p:childTnLst/>"), P(rootCTn, "subTnLst"));
  if (!list) return false;

  pugi::xml_node mainSeq;
  for (pugi::xml_node c = list.first_child(); c && !mainSeq; c = c.next_sibling()) {
    if (LocalNameOf(c) == "seq" && std::string_view(P(c, "cTn").attribute("nodeType").value()) == "mainSeq") mainSeq = c;
  }
  if (!mainSeq) {
    mainSeq = Put(list,
                  WithNs("<p:seq concurrent=\"1\" nextAc=\"seek\"><p:cTn id=" + id() +
                         " dur=\"indefinite\" nodeType=\"mainSeq\"><p:childTnLst/></p:cTn><p:prevCondLst><p:cond evt=\"onPrev\" "
                         "delay=\"0\"><p:tgtEl><p:sldTgt/></p:tgtEl></p:cond></p:prevCondLst><p:nextCondLst><p:cond evt=\"onNext\" "
                         "delay=\"0\"><p:tgtEl><p:sldTgt/></p:tgtEl></p:cond></p:nextCondLst></p:seq>"),
                  list.first_child());
  }
  pugi::xml_node mainCTn = P(mainSeq, "cTn");
  pugi::xml_node steps = P(mainCTn, "childTnLst");
  if (!steps) steps = Put(mainCTn, WithNs("<p:childTnLst/>"), P(mainCTn, "subTnLst"));
  if (!steps) return false;

  const std::string target = "<p:tgtEl><p:spTgt spid=\"" + std::to_string(spid) + "\"/></p:tgtEl>";
  const std::string dur = durationMs >= 1 ? " dur=\"" + std::to_string(std::llround(durationMs)) + "\"" : std::string();
  // The play step: a click group holding the media call.
  std::string play = "<p:par><p:cTn id=" + id() + " fill=\"hold\"><p:stCondLst><p:cond delay=\"indefinite\"/></p:stCondLst><p:childTnLst>";
  play += "<p:par><p:cTn id=" + id() + " fill=\"hold\"><p:stCondLst><p:cond delay=\"0\"/></p:stCondLst><p:childTnLst>";
  // (One id() per statement: the order of a + b + c's operands isn't fixed, and ids go in document order.)
  play += "<p:par><p:cTn id=" + id();
  play += " presetID=\"1\" presetClass=\"mediacall\" presetSubtype=\"0\" fill=\"hold\" nodeType=\"clickEffect\"><p:stCondLst>"
          "<p:cond delay=\"0\"/></p:stCondLst><p:childTnLst><p:cmd type=\"call\" cmd=\"playFrom(0.0)\"><p:cBhvr><p:cTn id=" + id();
  play += dur + " fill=\"hold\"/>" + target + "</p:cBhvr></p:cmd></p:childTnLst></p:cTn></p:par>";
  play += "</p:childTnLst></p:cTn></p:par></p:childTnLst></p:cTn></p:par>";
  if (!Put(steps, WithNs(play))) return false;

  // The media node, after the last one (else after the main sequence).
  std::string kind = video ? "video" : "audio";
  std::string node = "<p:" + kind + "><p:cMediaNode vol=\"80000\"><p:cTn id=" + id() +
                     " fill=\"hold\" display=\"0\"><p:stCondLst><p:cond delay=\"indefinite\"/></p:stCondLst>";
  if (!video) {
    node += "<p:endCondLst><p:cond evt=\"onStopAudio\" delay=\"0\"><p:tgtEl><p:sldTgt/></p:tgtEl></p:cond></p:endCondLst>";
  }
  node += "</p:cTn>" + target + "</p:cMediaNode></p:" + kind + ">";
  pugi::xml_node after = mainSeq;
  for (pugi::xml_node c = list.first_child(); c; c = c.next_sibling()) {
    std::string_view k = LocalNameOf(c);
    if (k == "video" || k == "audio") after = c;
  }
  if (!Put(list, WithNs(node), after.next_sibling())) return false;

  // A click on it pauses and plays again.
  const std::string click = "<p:cond evt=\"onClick\" delay=\"0\">" + target + "</p:cond>";
  std::string toggle = "<p:seq concurrent=\"1\" nextAc=\"seek\"><p:cTn id=" + id() +
                       " restart=\"whenNotActive\" fill=\"hold\" evtFilter=\"cancelBubble\" nodeType=\"interactiveSeq\"><p:stCondLst>" +
                       click + "</p:stCondLst><p:endSync evt=\"end\" delay=\"0\"><p:rtn val=\"all\"/></p:endSync><p:childTnLst>";
  toggle += "<p:par><p:cTn id=" + id() + " fill=\"hold\"><p:stCondLst><p:cond delay=\"0\"/></p:stCondLst><p:childTnLst>";
  toggle += "<p:par><p:cTn id=" + id() + " fill=\"hold\"><p:stCondLst><p:cond delay=\"0\"/></p:stCondLst><p:childTnLst>";
  toggle += "<p:par><p:cTn id=" + id();
  toggle += " presetID=\"2\" presetClass=\"mediacall\" presetSubtype=\"0\" fill=\"hold\" nodeType=\"clickEffect\"><p:stCondLst>"
            "<p:cond delay=\"0\"/></p:stCondLst><p:childTnLst><p:cmd type=\"call\" cmd=\"togglePause\"><p:cBhvr><p:cTn id=" + id();
  toggle += " dur=\"1\" fill=\"hold\"/>" + target + "</p:cBhvr></p:cmd></p:childTnLst></p:cTn></p:par>";
  toggle += "</p:childTnLst></p:cTn></p:par></p:childTnLst></p:cTn></p:par></p:childTnLst></p:cTn><p:nextCondLst>" + click +
            "</p:nextCondLst></p:seq>";
  return static_cast<bool>(Put(list, WithNs(toggle)));
}

}  // namespace

Result Editor::InsertMedia(size_t slide, MediaInsert media, const draw::Rect& r, uint32_t* id) {
  Call call(*this, media.video ? u"Insert Video" : u"Insert Audio");
  Impl& m = *impl_;
  std::string part = m.SlidePart(slide);
  if (part.empty()) return call.End(Result::Fail(u"That slide isn't in the presentation."));
  std::string ext = media.extension;
  if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
  for (char& c : ext) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  std::string_view type = MediaType(ext, media.video);
  if (type.empty()) {
    return call.End(Result::Fail(media.video ? u"A presentation can't hold a video of that kind."
                                             : u"A presentation can't hold a sound of that kind."));
  }
  if (media.bytes.empty()) return call.End(Result::Fail(u"That file is empty."));
  auto [posterExt, posterType] = PictureType(media.poster);
  if (posterExt != "png" && posterExt != "jpeg") return call.End(Result::Fail(u"The poster frame isn't a PNG or JPEG picture."));

  std::string file = StoreMedia(m, std::move(media.bytes), "/ppt/media/media", ext, type);
  std::string poster = StoreMedia(m, std::move(media.poster), "/ppt/media/image", posterExt, posterType);
  std::string relMedia = m.AddRel(part, kRelMedia, file);
  std::string relFile = m.AddRel(part, media.video ? kRelVideo : kRelAudio, file);
  std::string relPoster = m.AddRel(part, kRelImage, poster);

  uint32_t next = Impl::NextShapeId(m.Root(part));
  std::string name = media.name.empty() ? std::string(media.video ? "Video " : "Audio ") + std::to_string(next - 1)
                                        : Utf8(media.name);
  std::string xml = "<p:pic" + std::string(kNs) + "><p:nvPicPr><p:cNvPr id=\"" + std::to_string(next) + "\" name=\"" +
                    Attr(name) + "\" descr=\"" + Attr(name) +
                    "\"><a:hlinkClick r:id=\"\" action=\"ppaction://media\"/></p:cNvPr><p:cNvPicPr><a:picLocks "
                    "noChangeAspect=\"1\"/></p:cNvPicPr><p:nvPr><a:" +
                    (media.video ? "videoFile" : "audioFile") + " r:link=\"" + relFile +
                    "\"/><p:extLst><p:ext uri=\"{DAA4B4D4-6D71-4841-9C94-3DE7FCFB9230}\"><p14:media "
                    "xmlns:p14=\"http://schemas.microsoft.com/office/powerpoint/2010/main\" r:embed=\"" +
                    relMedia + "\"/></p:ext></p:extLst></p:nvPr></p:nvPicPr><p:blipFill><a:blip r:embed=\"" + relPoster +
                    "\"/><a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr>" + XfrmXml(r) +
                    "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>";
  pugi::xml_document doc;
  if (!ParseFragment(xml, doc)) return call.End(Result::Fail(u"The video's shape can't be made."));
  uint32_t placed = 0;
  Result res = PutShape(m, part, doc.document_element(), -1, &placed);
  if (!res.ok) return call.End(res);
  if (!AddMediaTiming(m.Edit(part), placed, media.video, media.durationMs)) {
    return call.End(Result::Fail(u"The slide's animations can't be read, so the video can't be added to them."));
  }
  if (id) *id = placed;
  return call.End(res);
}

}  // namespace fw::slide::edit
