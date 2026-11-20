#include "slide/render/PackageDeck.h"

#include <algorithm>

#include "draw/Xml.h"
#include "pkg/Package.h"
#include "pkg/XmlDom.h"

namespace fw::slide::render {

using namespace draw::xml;

struct PackageDeck::Part {
  std::unique_ptr<fw::pkg::XmlDom> dom;
  std::unique_ptr<draw::PackageResources> resources;
};

namespace {

std::string TargetOfType(const fw::pkg::Package& package, std::string_view source, std::string_view kind) {
  const fw::pkg::RelationshipSet* rels = package.RelationshipsOf(source);
  if (!rels) return {};
  for (const fw::pkg::Relationship& r : rels->items) {
    std::string_view type = r.type;
    if (!r.external && type.size() > kind.size() && type.ends_with(kind) && type[type.size() - kind.size() - 1] == '/')
      return r.targetPart;
  }
  return {};
}

}  // namespace

PackageDeck::PackageDeck(const fw::pkg::Package& package) : package_(package), images_(std::make_shared<draw::ImageCache>()) {
  presentation_ = TargetOfType(package, "/", "officeDocument");
  Part* pres = Get(presentation_);
  if (!pres) return;
  pugi::xml_node root = pres->dom->Root();
  pugi::xml_node sz = Child(root, kP, "sldSz");
  size_ = {std::clamp(EmuToPt(Number(sz, "cx", 9144000)), 1.f, 100000.f), std::clamp(EmuToPt(Number(sz, "cy", 6858000)), 1.f, 100000.f)};
  firstNumber_ = static_cast<int>(Number(root, "firstSlideNum", 1));
  const fw::pkg::RelationshipSet* rels = package.RelationshipsOf(presentation_);
  ForEachChild(Child(root, kP, "sldIdLst"), [&](pugi::xml_node id) {
    const fw::pkg::Relationship* rel = rels ? rels->Find(RelId(id, "id")) : nullptr;
    if (rel && !rel->external) slides_.push_back(rel->targetPart);
    return slides_.size() < 100000;
  });
}

PackageDeck::~PackageDeck() = default;

PackageDeck::Part* PackageDeck::Get(const std::string& name) {
  if (name.empty()) return nullptr;
  auto it = parts_.find(name);
  if (it != parts_.end()) return it->second.get();
  auto part = std::make_unique<Part>();
  std::vector<uint8_t> bytes;
  if (package_.ReadPart(name, bytes).Ok()) {
    part->dom = std::make_unique<fw::pkg::XmlDom>();
    if (!part->dom->Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), package_.GetLimits(), name)
             .Ok())
      part->dom.reset();
  }
  if (!part->dom) part.reset();
  else part->resources = std::make_unique<draw::PackageResources>(package_, name, nullptr, draw::ColorMap{}, images_);
  return (parts_[name] = std::move(part)).get();
}

std::string PackageDeck::Related(const std::string& part, std::string_view kind) const {
  return TargetOfType(package_, part, kind);
}

SlideSource PackageDeck::Source(size_t index) {
  SlideSource s;
  if (index >= slides_.size()) return s;
  Part* slide = Get(slides_[index]);
  if (!slide) return s;
  const std::string layoutName = Related(slides_[index], "slideLayout");
  Part* layout = Get(layoutName);
  const std::string masterName = Related(layoutName, "slideMaster");
  Part* master = Get(masterName);
  s.slide = slide->dom->Root();
  s.slideResources = slide->resources.get();
  if (layout) {
    s.layout = layout->dom->Root();
    s.layoutResources = layout->resources.get();
  }
  if (master) {
    s.master = master->dom->Root();
    s.masterResources = master->resources.get();
    const std::string themeName = Related(masterName, "theme");
    auto it = themes_.find(themeName);
    if (it == themes_.end()) {
      std::vector<uint8_t> bytes;
      std::shared_ptr<const draw::Theme> theme;
      if (!themeName.empty() && package_.ReadPart(themeName, bytes).Ok())
        theme = draw::Theme::Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
      it = themes_.emplace(themeName, theme).first;
    }
    s.theme = it->second;
  }
  // The color map: the slide's override, else the layout's, else the master's.
  pugi::xml_node map;
  for (pugi::xml_node root : {s.slide, s.layout}) {
    if (!map) map = A(Child(root, kP, "clrMapOvr"), "overrideClrMapping");
  }
  if (!map) map = Child(s.master, kP, "clrMap");
  s.colorMap = draw::ColorMap::Read(map);
  if (Part* pres = Get(presentation_)) s.defaultTextStyle = Child(pres->dom->Root(), kP, "defaultTextStyle");
  if (Part* styles = Get(Related(presentation_, "tableStyles"))) s.tableStyles = styles->dom->Root();
  s.size = size_;
  s.number = firstNumber_ + static_cast<int>(index);
  return s;
}

bool PackageDeck::Hidden(size_t index) {
  Part* slide = index < slides_.size() ? Get(slides_[index]) : nullptr;
  return slide && !Bool(slide->dom->Root(), "show", true);
}

}  // namespace fw::slide::render
