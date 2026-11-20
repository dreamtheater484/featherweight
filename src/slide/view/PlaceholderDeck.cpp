#include "slide/view/PlaceholderDeck.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "core/Strings.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/XmlDom.h"
#include "slide/view/SlidePages.h"
#include "text/Fonts.h"

namespace fw::slide::view {

namespace {

using pugi::xml_node;

constexpr float kEmuPerPt = 12700.0f;
constexpr std::string_view kSlideRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide";
constexpr std::string_view kNotesRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/notesSlide";
constexpr std::string_view kDocRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument";
constexpr std::string_view kShowType = "application/vnd.openxmlformats-officedocument.presentationml.slideshow.main+xml";

std::string_view Local(xml_node n) {
  std::string_view name = n.name();
  const size_t colon = name.find(':');
  return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

xml_node Child(xml_node n, std::string_view local) {
  for (xml_node c = n.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && Local(c) == local) return c;
  return {};
}

const char* Attr(xml_node n, std::string_view local) {
  for (pugi::xml_attribute a = n.first_attribute(); a; a = a.next_attribute()) {
    std::string_view name = a.name();
    const size_t colon = name.find(':');
    if ((colon == std::string_view::npos ? name : name.substr(colon + 1)) == local) return a.value();
  }
  return "";
}

// A relationship id: the r:id attribute (a p:sldId also has a plain id).
const char* RelId(xml_node n) {
  for (pugi::xml_attribute a = n.first_attribute(); a; a = a.next_attribute()) {
    std::string_view name = a.name();
    if (name.size() > 3 && name.substr(name.size() - 3) == ":id") return a.value();
  }
  return "";
}

// Every paragraph of a text body, '\n' between them.
std::wstring BodyText(xml_node body) {
  std::string out;
  bool first = true;
  for (xml_node p = body.first_child(); p; p = p.next_sibling()) {
    if (Local(p) != "p") continue;
    if (!first) out += '\n';
    first = false;
    for (xml_node r = p.first_child(); r; r = r.next_sibling()) {
      if (Local(r) == "br") out += '\v';
      if (Local(r) != "r" && Local(r) != "fld") continue;
      if (xml_node t = Child(r, "t")) out += t.text().get();
    }
  }
  std::wstring w = Utf8ToWide(out);
  std::replace(w.begin(), w.end(), L'\v', L'\n');
  return w;
}

struct Shape {
  TextBlock block;
  bool title = false;
  std::string linkId;  // the shape's own hyperlink (cNvPr/hlinkClick)
};

struct SlideData {
  std::string part;
  bool hidden = false;
  draw::Color background{1, 1, 1, 1};
  std::vector<Shape> shapes;
  std::wstring notes;
  std::vector<SlideLink> links;
};

class PlaceholderDeck : public Deck {
 public:
  bool Open(const std::wstring& path, std::wstring& error);

  size_t SlideCount() const override { return slides_.size(); }
  draw::Size SlideSize() const override { return size_; }
  draw::Size NotesSize() const override { return notes_; }
  bool Hidden(size_t i) const override { return i < slides_.size() && slides_[i].hidden; }
  std::vector<TextBlock> Text(size_t i) const override {
    std::vector<TextBlock> out;
    if (i < slides_.size())
      for (const Shape& s : slides_[i].shapes) out.push_back(s.block);
    return out;
  }
  std::wstring Notes(size_t i) const override { return i < slides_.size() ? slides_[i].notes : std::wstring(); }
  std::vector<SlideLink> Links(size_t i) const override {
    return i < slides_.size() ? slides_[i].links : std::vector<SlideLink>();
  }
  bool StartsAsShow() const override { return show_; }
  void Paint(size_t i, draw::Painter& p) override;
  size_t MemoryUse() const override { return snapshotSize_; }

 private:
  void ReadSlide(SlideData& s);
  std::unique_ptr<pkg::Package> package_;
  std::vector<SlideData> slides_;
  std::vector<std::string> slideParts_;
  draw::Size size_{720, 405}, notes_{540, 720};
  bool show_ = false;
  size_t snapshotSize_ = 0;
  std::unique_ptr<text::Fonts> fonts_;
};

bool ReadXml(const pkg::Package& package, std::string_view part, pkg::XmlDom& dom) {
  std::vector<uint8_t> bytes;
  if (!package.ReadPart(part, bytes).Ok()) return false;
  return dom.Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())).Ok();
}

bool PlaceholderDeck::Open(const std::wstring& path, std::wstring& error) {
  pkg::Status status;
  std::unique_ptr<pkg::Snapshot> snap = pkg::Snapshot::Open(path, {}, status);
  if (!snap) {
    error = L"The file couldn't be read.";
    return false;
  }
  snapshotSize_ = static_cast<size_t>(snap->Size());
  package_ = pkg::Package::Open(std::move(snap), pkg::Limits::Default(), status);
  if (!package_ || !status.Ok()) {
    error = L"This file isn't a presentation Featherweight can open, or it is damaged.";
    return false;
  }
  std::string main;
  if (const pkg::RelationshipSet* rels = package_->RelationshipsOf("/"))
    for (const pkg::Relationship& r : rels->items)
      if (r.type == kDocRel) main = r.targetPart;
  pkg::XmlDom pres;
  if (main.empty() || !ReadXml(*package_, main, pres)) {
    error = L"This file isn't a presentation Featherweight can open, or it is damaged.";
    return false;
  }
  show_ = package_->ContentTypeOf(main) == kShowType;
  xml_node root = pres.Root();
  if (xml_node sz = Child(root, "sldSz"))
    size_ = {std::max(1.0f, std::strtof(Attr(sz, "cx"), nullptr) / kEmuPerPt),
             std::max(1.0f, std::strtof(Attr(sz, "cy"), nullptr) / kEmuPerPt)};
  if (xml_node sz = Child(root, "notesSz"))
    notes_ = {std::max(1.0f, std::strtof(Attr(sz, "cx"), nullptr) / kEmuPerPt),
              std::max(1.0f, std::strtof(Attr(sz, "cy"), nullptr) / kEmuPerPt)};
  const pkg::RelationshipSet* rels = package_->RelationshipsOf(main);
  if (xml_node list = Child(root, "sldIdLst"))
    for (xml_node id = list.first_child(); id; id = id.next_sibling()) {
      const pkg::Relationship* r = rels ? rels->Find(RelId(id)) : nullptr;
      if (!r || r->type != kSlideRel || r->targetPart.empty()) continue;
      SlideData s;
      s.part = r->targetPart;
      slideParts_.push_back(s.part);
      slides_.push_back(std::move(s));
    }
  for (SlideData& s : slides_) ReadSlide(s);
  return true;
}

void PlaceholderDeck::ReadSlide(SlideData& s) {
  pkg::XmlDom dom;
  if (!ReadXml(*package_, s.part, dom)) return;
  xml_node sld = dom.Root();
  s.hidden = std::string_view(Attr(sld, "show")) == "0";
  const pkg::RelationshipSet* rels = package_->RelationshipsOf(s.part);
  auto linkFor = [&](xml_node click, const draw::Rect& rect, const std::wstring& text) {
    SlideLink link;
    link.rect = rect;
    const std::string_view action = Attr(click, "action");
    const pkg::Relationship* r = rels ? rels->Find(RelId(click)) : nullptr;
    if (r && r->external) {
      link.url = Utf8ToWide(r->target);
    } else if (r && r->type == kSlideRel) {
      for (size_t k = 0; k < slideParts_.size(); ++k)
        if (slideParts_[k] == r->targetPart) link.slide = static_cast<int>(k);
    } else if (action.find("jump=nextslide") != std::string_view::npos) {
      link.slide = -2;  // relative: resolved by the tab
    } else {
      return;
    }
    link.tooltip = Utf8ToWide(Attr(click, "tooltip"));
    if (link.tooltip.empty()) link.tooltip = link.url.empty() ? text : link.url;
    s.links.push_back(std::move(link));
  };
  if (xml_node bg = Child(Child(Child(sld, "cSld"), "bg"), "bgPr"))
    if (xml_node fill = Child(bg, "solidFill"))
      if (xml_node rgb = Child(fill, "srgbClr")) {
        const unsigned long v = std::strtoul(Attr(rgb, "val"), nullptr, 16);
        s.background = {((v >> 16) & 255) / 255.0f, ((v >> 8) & 255) / 255.0f, (v & 255) / 255.0f, 1};
      }
  // Shapes, in the order they're drawn (groups flattened; their child offsets are ignored here).
  std::vector<xml_node> stack{Child(Child(sld, "cSld"), "spTree")};
  while (!stack.empty()) {
    xml_node tree = stack.back();
    stack.pop_back();
    std::vector<xml_node> groups;
    for (xml_node sp = tree.first_child(); sp; sp = sp.next_sibling()) {
      if (Local(sp) == "grpSp") {
        groups.push_back(sp);
        continue;
      }
      xml_node nv = sp.first_child();  // nvSpPr, nvPicPr...
      xml_node xfrm = Child(Child(sp, "spPr"), "xfrm");
      if (!xfrm) xfrm = Child(sp, "xfrm");
      draw::Rect rect{};
      if (xml_node off = Child(xfrm, "off"), ext = Child(xfrm, "ext"); off && ext) {
        rect.left = std::strtof(Attr(off, "x"), nullptr) / kEmuPerPt;
        rect.top = std::strtof(Attr(off, "y"), nullptr) / kEmuPerPt;
        rect.right = rect.left + std::strtof(Attr(ext, "cx"), nullptr) / kEmuPerPt;
        rect.bottom = rect.top + std::strtof(Attr(ext, "cy"), nullptr) / kEmuPerPt;
      }
      Shape shape;
      xml_node ph = Child(Child(nv, "nvPr"), "ph");
      const std::string_view type = Attr(ph, "type");
      shape.title = ph && (type == "title" || type == "ctrTitle");
      if (rect.Width() <= 0 && ph) {
        // A placeholder that takes its place from the layout: where PowerPoint's default layouts put it.
        rect = shape.title ? draw::Rect{size_.width * 0.07f, size_.height * 0.06f, size_.width * 0.93f, size_.height * 0.24f}
                           : draw::Rect{size_.width * 0.07f, size_.height * 0.27f, size_.width * 0.93f, size_.height * 0.9f};
      }
      shape.block.bounds = rect;
      if (xml_node body = Child(sp, "txBody")) shape.block.text = BodyText(body);
      if (xml_node click = Child(Child(nv, "cNvPr"), "hlinkClick")) linkFor(click, rect, shape.block.text);
      // Links on the text's runs: the whole shape's box, as the placeholder can't place runs.
      if (xml_node body = Child(sp, "txBody"))
        for (xml_node p = body.first_child(); p; p = p.next_sibling())
          for (xml_node r = p.first_child(); r; r = r.next_sibling())
            if (xml_node click = Child(Child(r, "rPr"), "hlinkClick"))
              linkFor(click, rect, Utf8ToWide(Child(r, "t").text().get()));
      if (!shape.block.text.empty()) s.shapes.push_back(std::move(shape));
    }
    for (auto it = groups.rbegin(); it != groups.rend(); ++it) stack.push_back(*it);
  }
  // Notes: the body placeholder's text on the slide's notes page.
  if (rels)
    for (const pkg::Relationship& r : rels->items) {
      if (r.type != kNotesRel || r.targetPart.empty()) continue;
      pkg::XmlDom notes;
      if (!ReadXml(*package_, r.targetPart, notes)) break;
      for (xml_node sp = Child(Child(notes.Root(), "cSld"), "spTree").first_child(); sp; sp = sp.next_sibling()) {
        xml_node ph = Child(Child(sp.first_child(), "nvPr"), "ph");
        if (ph && std::string_view(Attr(ph, "type")) == "body")
          if (xml_node body = Child(sp, "txBody")) s.notes = BodyText(body);
      }
      break;
    }
}

void PlaceholderDeck::Paint(size_t i, draw::Painter& p) {
  if (i >= slides_.size()) return;
  const SlideData& s = slides_[i];
  p.FillRect({0, 0, size_.width, size_.height}, draw::Brush::Solid(s.background));
  if (!fonts_) fonts_ = std::make_unique<text::Fonts>();
  const float lum = 0.3f * s.background.r + 0.59f * s.background.g + 0.11f * s.background.b;
  const draw::Color ink = lum < 0.45f ? draw::Color{1, 1, 1, 1} : draw::Color{0.1f, 0.1f, 0.1f, 1};
  for (const Shape& sh : s.shapes) {
    const draw::Rect& r = sh.block.bounds;
    p.Save();
    p.ClipRect(r);
    DrawPlainText(*fonts_, p, sh.block.text, {r.left + 7, r.top + 4, r.right - 7, r.bottom - 4}, sh.title ? 36.0f : 20.0f,
                  L"Calibri", ink);
    p.Restore();
  }
}

}  // namespace

std::unique_ptr<Deck> OpenPlaceholderDeck(const std::wstring& path, std::wstring& error) {
  auto deck = std::make_unique<PlaceholderDeck>();
  if (!deck->Open(path, error)) return nullptr;
  return deck;
}

}  // namespace fw::slide::view
