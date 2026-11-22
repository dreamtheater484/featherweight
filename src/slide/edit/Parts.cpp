#include "slide/edit/Parts.h"

#include <algorithm>

#include "slide/Presentation.h"

namespace fw::slide::edit {

std::shared_ptr<pkg::XmlDom> CloneDom(const pkg::XmlDom& dom) {
  auto copy = std::make_shared<pkg::XmlDom>();
  copy->Doc().reset(dom.Doc());
  return copy;
}

std::shared_ptr<pkg::XmlDom> ParseDom(std::string_view xml, const pkg::Limits& limits, std::string_view label) {
  auto dom = std::make_shared<pkg::XmlDom>();
  if (!dom->Parse(xml, limits, label).Ok()) return nullptr;
  return dom;
}

PartStore::PartStore(const pkg::Package& package) : package_(package) {}
PartStore::~PartStore() = default;

const PartState* PartStore::Find(std::string_view part) const {
  auto it = parts_.find(pkg::PartKey(part));
  return it == parts_.end() ? nullptr : &it->second;
}

bool PartStore::Has(std::string_view part) const {
  if (const PartState* s = Find(part)) return !s->Removed();
  const pkg::Part* p = package_.FindPart(part);
  return p && !p->deleted;
}

DomPtr PartStore::Dom(std::string_view part) const {
  if (const PartState* s = Find(part)) {
    if (s->dom) return s->dom;
    if (!s->bytes) return nullptr;
    return ParseDom(std::string_view(reinterpret_cast<const char*>(s->bytes->data()), s->bytes->size()),
                    package_.GetLimits(), part);
  }
  std::vector<uint8_t> bytes;
  if (!package_.ReadPart(part, bytes).Ok()) return nullptr;
  return ParseDom(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), package_.GetLimits(),
                  part);
}

bool PartStore::ReadPart(std::string_view part, std::vector<uint8_t>& out) const {
  out.clear();
  if (const PartState* s = Find(part)) {
    if (s->bytes) {
      out = *s->bytes;
      return true;
    }
    if (s->dom) {
      std::string xml = s->dom->Serialize();
      out.assign(xml.begin(), xml.end());
      return true;
    }
    return false;
  }
  return package_.ReadPart(part, out).Ok();
}

const pkg::RelationshipSet* PartStore::Relationships(std::string_view source) const {
  auto it = rels_.find(pkg::PartKey(source));
  if (it != rels_.end()) return it->second.get();
  // A part the edits removed or replaced keeps no relationships of the package's.
  if (const PartState* s = Find(source); s && source != "/") {
    if (s->Removed()) return nullptr;
  }
  return package_.RelationshipsOf(source);
}

RelsPtr PartStore::RelationshipsPtr(std::string_view source) const {
  auto it = rels_.find(pkg::PartKey(source));
  return it == rels_.end() ? nullptr : it->second;
}

std::string PartStore::ContentTypeOf(std::string_view part) const {
  if (!types_) return std::string(package_.ContentTypeOf(part));
  return std::string(types_->Lookup(part));
}

std::vector<std::string> PartStore::PartNames() const {
  std::vector<std::string> names;
  std::set<std::string> seen;
  for (const pkg::Part& p : package_.Parts()) {
    if (p.deleted || p.name == pkg::kContentTypesPart) continue;
    std::string key = pkg::PartKey(p.name);
    if (!seen.insert(key).second) continue;
    if (const PartState* s = Find(p.name); s && s->Removed()) continue;
    names.push_back(p.name);
  }
  for (const auto& [key, s] : parts_) {
    if (s.Removed() || seen.count(key)) continue;
    names.push_back(s.name);
  }
  return names;
}

std::optional<PartState> PartStore::Get(std::string_view part) const {
  if (const PartState* s = Find(part)) return *s;
  return std::nullopt;
}

void PartStore::Set(std::string_view part, std::optional<PartState> state) {
  std::string key = pkg::PartKey(part);
  if (state) {
    if (state->name.empty()) state->name.assign(part);
    parts_[key] = std::move(*state);
  } else {
    parts_.erase(key);
  }
}

std::optional<RelsPtr> PartStore::GetRels(std::string_view source) const {
  auto it = rels_.find(pkg::PartKey(source));
  if (it == rels_.end()) return std::nullopt;
  return it->second;
}

void PartStore::SetRels(std::string_view source, std::optional<RelsPtr> rels) {
  std::string key = pkg::PartKey(source);
  if (rels) {
    rels_[key] = std::move(*rels);
  } else {
    rels_.erase(key);
  }
}

std::shared_ptr<const draw::Image> PartStore::Image(std::string_view part, draw::ImageCache& packageImages) const {
  const PartState* s = Find(part);
  if (!s) return packageImages.Get(package_, part);
  if (!s->bytes) return nullptr;
  std::lock_guard lock(imagesMutex_);
  std::string key = pkg::PartKey(part);
  // Keyed by the bytes too: a part name an undone edit used may come back with other bytes.
  key += '#';
  key += std::to_string(reinterpret_cast<uintptr_t>(s->bytes.get()));
  auto it = images_.find(key);
  if (it != images_.end()) return it->second;
  BytesPtr bytes = s->bytes;
  std::shared_ptr<const draw::Image> image = draw::Image::OpenFrom([bytes](std::vector<uint8_t>& out) {
    out = *bytes;
    return true;
  });
  images_[key] = image;
  return image;
}

void PartStore::Bury() {
  older_ = std::move(retired_);
  retired_ = Retired{};
}

// ---- EditedResources

EditedResources::EditedResources(const PartStore& store, std::string part, std::shared_ptr<const draw::Theme> theme,
                                 draw::ColorMap colorMap, std::shared_ptr<draw::ImageCache> images)
    : store_(store),
      part_(std::move(part)),
      theme_(std::move(theme)),
      colorMap_(colorMap),
      images_(images ? std::move(images) : std::make_shared<draw::ImageCache>()) {}

std::string EditedResources::Target(std::string_view relId) const {
  const pkg::RelationshipSet* rels = store_.Relationships(part_);
  const pkg::Relationship* rel = rels ? rels->Find(relId) : nullptr;
  if (!rel || rel->external) return {};
  return rel->targetPart;
}

std::shared_ptr<const draw::Image> EditedResources::GetImage(std::string_view relId) {
  std::string target = Target(relId);
  return target.empty() ? nullptr : store_.Image(target, *images_);
}

pugi::xml_node EditedResources::GetPartXml(std::string_view relId) {
  std::string target = Target(relId);
  if (target.empty()) return {};
  std::string key = pkg::PartKey(target);
  auto it = xml_.find(key);
  if (it == xml_.end()) it = xml_.emplace(key, store_.Dom(target)).first;
  return it->second ? it->second->Root() : pugi::xml_node();
}

}  // namespace fw::slide::edit
