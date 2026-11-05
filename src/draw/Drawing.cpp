#include "draw/Drawing.h"

#include <algorithm>

#include "draw/Shapes.h"
#include "draw/Xml.h"
#include "pkg/Package.h"

namespace draw {

using namespace xml;

// Builds Drawings: the readers fill in the root shape, then Finish works out the rest.
class DrawingBuilder {
 public:
  static std::unique_ptr<Drawing> Make() { return std::unique_ptr<Drawing>(new Drawing()); }
  static Shape& Root(Drawing& d) { return *d.root_; }
  static void Set(Drawing& d, const Rect& bounds, std::string name, std::string description, bool hidden) {
    d.bounds_ = bounds;
    d.name_ = std::move(name);
    d.description_ = std::move(description);
    d.hidden_ = hidden;
  }
  static void SetWrapPolygon(Drawing& d, std::vector<Point> points) {
    d.wrap_.clear();
    if (points.size() >= 3) {
      d.wrap_.push_back({std::move(points), true});
      d.wrapFromFile_ = true;
    }
  }
  static void Finish(Drawing& d) {
    const Matrix toDrawing = Matrix::Translate(-d.bounds_.left, -d.bounds_.top);
    if (!d.wrapFromFile_) CollectOutlines(*d.root_, toDrawing, d.wrap_);
    CollectTextBoxes(*d.root_, toDrawing, d.textBoxes_);
    d.placeholder_ = d.root_->kind == Shape::Kind::Placeholder;
  }
};

Drawing::Drawing() : root_(std::make_unique<Shape>()) {}
Drawing::~Drawing() = default;

Rect Drawing::InkBounds() const {
  const Rect r = PaintedBounds(*root_);
  return {r.left - bounds_.left, r.top - bounds_.top, r.right - bounds_.left, r.bottom - bounds_.top};
}

void Drawing::Draw(Painter& painter, const Rect& frame) const {
  if (hidden_) return;
  const float w = bounds_.Width(), h = bounds_.Height();
  painter.Save();
  painter.Transform(Matrix::Translate(-bounds_.left, -bounds_.top) *
                    Matrix::Scale(w > 0 ? frame.Width() / w : 1, h > 0 ? frame.Height() / h : 1) *
                    Matrix::Translate(frame.left, frame.top));
  DrawShape(painter, *root_);
  painter.Restore();
}

std::unique_ptr<Drawing> Drawing::FromWordDrawing(pugi::xml_node node, DrawingResources& resources) {
  if (IsElement(node, kW, "drawing")) {
    pugi::xml_node inner;
    ForEachChild(node, [&](pugi::xml_node c) {
      if (Is(c, kWp, "inline") || Is(c, kWp, "anchor")) inner = c;
      return !inner;
    });
    node = inner;
  }
  if (!Is(node, kWp, "inline") && !Is(node, kWp, "anchor")) return nullptr;
  pugi::xml_node extent = Child(node, kWp, "extent"), docPr = Child(node, kWp, "docPr");
  const Size size{std::clamp(EmuToPt(Number(extent, "cx", 0)), 0.f, 1e6f),
                  std::clamp(EmuToPt(Number(extent, "cy", 0)), 0.f, 1e6f)};
  std::unique_ptr<Drawing> d = DrawingBuilder::Make();
  ReadContext ctx(resources);
  Shape& root = DrawingBuilder::Root(*d);
  if (!ReadGraphicData(A(A(node, "graphic"), "graphicData"), size, ctx, root)) {
    root.kind = Shape::Kind::Placeholder;  // keeps its size
    root.bounds = {0, 0, size.width, size.height};
  }
  std::string name = docPr.attribute("name").value();
  if (root.kind == Shape::Kind::Placeholder && !name.empty()) root.name = name;
  DrawingBuilder::Set(*d, {0, 0, size.width, size.height}, name,
                      docPr.attribute("descr").value()[0] ? docPr.attribute("descr").value()
                                                          : docPr.attribute("title").value(),
                      Bool(docPr, "hidden", false));
  // Tight and through wrapping: Word's polygon, in 1/21600 of the extent.
  for (std::string_view wrap : {"wrapTight", "wrapThrough"}) {
    pugi::xml_node poly = Child(Child(node, kWp, wrap), kWp, "wrapPolygon");
    if (!poly) continue;
    std::vector<Point> pts;
    ForEachChild(poly, [&](pugi::xml_node pt) {
      pts.push_back({static_cast<float>(Number(pt, "x", 0) * size.width / 21600),
                     static_cast<float>(Number(pt, "y", 0) * size.height / 21600)});
      return pts.size() < 10000;
    });
    DrawingBuilder::SetWrapPolygon(*d, std::move(pts));
  }
  DrawingBuilder::Finish(*d);
  return d;
}

std::unique_ptr<Drawing> Drawing::FromShapeTree(pugi::xml_node node, DrawingResources& resources) {
  std::unique_ptr<Drawing> d = DrawingBuilder::Make();
  ReadContext ctx(resources);
  Shape& root = DrawingBuilder::Root(*d);
  if (!ReadDrawingMLShape(node, ctx, root)) return nullptr;
  // A sheet's shape may leave its own xfrm empty (openpyxl does) and take its size from a
  // one-cell or absolute anchor. A two-cell anchor's size depends on the columns and rows.
  pugi::xml_node anchor = node.parent();
  if (root.bounds.Width() <= 0 && root.bounds.Height() <= 0 && NamespaceOf(anchor) == kXdr) {
    pugi::xml_node ext = Child(anchor, kXdr, "ext"), pos = Child(anchor, kXdr, "pos");
    const float x = EmuToPt(Number(pos, "x", 0)), y = EmuToPt(Number(pos, "y", 0));
    root.bounds = {x, y, x + std::clamp(EmuToPt(Number(ext, "cx", 0)), 0.f, 1e6f),
                   y + std::clamp(EmuToPt(Number(ext, "cy", 0)), 0.f, 1e6f)};
  }
  DrawingBuilder::Set(*d, root.bounds, root.name, root.description, root.hidden);
  root.hidden = false;  // Drawing::IsHidden says it instead
  DrawingBuilder::Finish(*d);
  return d;
}

std::unique_ptr<Drawing> Drawing::FromVml(pugi::xml_node node, DrawingResources& resources) {
  // w:pict and w:object hold the shape (and a v:shapetype it may refer to).
  if (NamespaceOf(node) == kW) {
    pugi::xml_node shape;
    ForEachChild(node, [&](pugi::xml_node c) {
      if (NamespaceOf(c) == kV && LocalNameOf(c) != "shapetype") shape = c;
      return !shape;
    });
    node = shape;
  }
  if (NamespaceOf(node) != kV) return nullptr;
  std::unique_ptr<Drawing> d = DrawingBuilder::Make();
  ReadContext ctx(resources);
  Shape& root = DrawingBuilder::Root(*d);
  if (!ReadVmlShape(node, ctx, root)) return nullptr;
  DrawingBuilder::Set(*d, root.bounds, root.name, root.description, root.hidden);
  root.hidden = false;
  // o:wrapcoords: a polygon in 1/21600 of the shape, as in DrawingML.
  std::string_view coords = FindAttribute(node, kO, "wrapcoords").value();
  if (!coords.empty()) {
    std::vector<Point> pts;
    std::vector<double> nums;
    size_t i = 0;
    while (i < coords.size() && nums.size() < 20000) {
      while (i < coords.size() && (coords[i] == ' ' || coords[i] == ',')) ++i;
      size_t j = i;
      while (j < coords.size() && coords[j] != ' ' && coords[j] != ',') ++j;
      if (j > i) nums.push_back(Number(coords.substr(i, j - i), 0));
      i = j;
    }
    for (size_t k = 0; k + 1 < nums.size(); k += 2) {
      pts.push_back({static_cast<float>(nums[k] * root.bounds.Width() / 21600),
                     static_cast<float>(nums[k + 1] * root.bounds.Height() / 21600)});
    }
    DrawingBuilder::SetWrapPolygon(*d, std::move(pts));
  }
  DrawingBuilder::Finish(*d);
  return d;
}

// ---- Package glue ----

std::shared_ptr<const Image> ImageCache::Get(const fw::pkg::Package& package, std::string_view partName) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string key = fw::pkg::PartKey(partName);
  auto it = images_.find(key);
  if (it != images_.end()) return it->second;
  std::vector<uint8_t> bytes;
  std::shared_ptr<const Image> image;
  if (package.ReadPart(partName, bytes).Ok()) image = Image::Open(std::move(bytes));
  images_[key] = image;  // null too: a damaged picture isn't read again
  return image;
}

PackageResources::PackageResources(const fw::pkg::Package& package, std::string partName,
                                   std::shared_ptr<const Theme> theme, ColorMap colorMap,
                                   std::shared_ptr<ImageCache> images)
    : package_(package),
      part_(std::move(partName)),
      theme_(std::move(theme)),
      colorMap_(colorMap),
      images_(images ? std::move(images) : std::make_shared<ImageCache>()) {}

PackageResources::~PackageResources() = default;

std::shared_ptr<const Theme> PackageResources::GetTheme() { return theme_ ? theme_ : Theme::Default(); }

namespace {

// An internal relationship's target part, or empty.
std::string Target(const fw::pkg::Package& package, std::string_view source, std::string_view relId) {
  const fw::pkg::RelationshipSet* rels = package.RelationshipsOf(source);
  const fw::pkg::Relationship* rel = rels ? rels->Find(relId) : nullptr;
  if (!rel || rel->external) return {};
  return rel->targetPart;
}

// The first internal relationship of `source` whose type ends with "/<kind>".
std::string TargetOfType(const fw::pkg::Package& package, std::string_view source, std::string_view kind) {
  const fw::pkg::RelationshipSet* rels = package.RelationshipsOf(source);
  if (!rels) return {};
  for (const fw::pkg::Relationship& r : rels->items) {
    std::string_view type = r.type;
    if (!r.external && type.size() > kind.size() && type.ends_with(kind) && type[type.size() - kind.size() - 1] == '/') {
      return r.targetPart;
    }
  }
  return {};
}

std::unique_ptr<fw::pkg::XmlDom> ParsePart(const fw::pkg::Package& package, std::string_view part) {
  std::vector<uint8_t> bytes;
  if (part.empty() || !package.ReadPart(part, bytes).Ok()) return nullptr;
  auto dom = std::make_unique<fw::pkg::XmlDom>();
  if (!dom->Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), package.GetLimits(), part)
           .Ok()) {
    return nullptr;
  }
  return dom;
}

// A slide's chain: the slide, its layout, its master (and notes and their master).
std::vector<std::string> Chain(const fw::pkg::Package& package, std::string_view part) {
  std::vector<std::string> chain{std::string(part)};
  for (int i = 0; i < 3; ++i) {
    std::string next;
    for (std::string_view kind : {"slideLayout", "slideMaster", "notesMaster", "handoutMaster"}) {
      next = TargetOfType(package, chain.back(), kind);
      if (!next.empty()) break;
    }
    if (next.empty() || std::find(chain.begin(), chain.end(), next) != chain.end()) break;
    chain.push_back(next);
  }
  return chain;
}

}  // namespace

std::shared_ptr<const Image> PackageResources::GetImage(std::string_view relId) {
  std::string target = Target(package_, part_, relId);
  return target.empty() ? nullptr : images_->Get(package_, target);
}

pugi::xml_node PackageResources::GetPartXml(std::string_view relId) {
  std::string target = Target(package_, part_, relId);
  if (target.empty()) return {};
  std::string key = fw::pkg::PartKey(target);
  auto it = parts_.find(key);
  if (it == parts_.end()) it = parts_.emplace(key, ParsePart(package_, target)).first;
  return it->second ? it->second->Root() : pugi::xml_node();
}

std::shared_ptr<const Theme> PackageResources::FindTheme(const fw::pkg::Package& package, std::string_view partName) {
  std::vector<std::string> parts = Chain(package, partName);
  // Headers, footers and the like use the main document's theme.
  parts.push_back(TargetOfType(package, "/", "officeDocument"));
  for (const std::string& part : parts) {
    std::string theme = part.empty() ? std::string() : TargetOfType(package, part, "theme");
    if (theme.empty()) continue;
    std::vector<uint8_t> bytes;
    if (!package.ReadPart(theme, bytes).Ok()) continue;
    return Theme::Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  }
  return nullptr;
}

ColorMap PackageResources::FindColorMap(const fw::pkg::Package& package, std::string_view partName) {
  // Slides: an override on the slide or its layout, else the master's p:clrMap.
  for (const std::string& part : Chain(package, partName)) {
    std::unique_ptr<fw::pkg::XmlDom> dom = ParsePart(package, part);
    if (!dom) continue;
    pugi::xml_node root = dom->Root();
    if (pugi::xml_node map = Child(root, kP, "clrMap")) return ColorMap::Read(map);
    if (pugi::xml_node over = A(Child(root, kP, "clrMapOvr"), "overrideClrMapping")) return ColorMap::Read(over);
  }
  // Word: w:clrSchemeMapping in the settings.
  std::string settings = TargetOfType(package, TargetOfType(package, "/", "officeDocument"), "settings");
  if (std::unique_ptr<fw::pkg::XmlDom> dom = ParsePart(package, settings)) {
    if (pugi::xml_node map = Child(dom->Root(), kW, "clrSchemeMapping")) return ColorMap::Read(map);
  }
  return {};
}

}  // namespace draw
