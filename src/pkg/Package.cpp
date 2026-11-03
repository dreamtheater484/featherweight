#include "pkg/Package.h"

#include <algorithm>
#include <unordered_map>

#include "pkg/XmlDom.h"
#include "pkg/XmlPull.h"
#include "pkg/XmlWriter.h"

namespace fw::pkg {
namespace {

std::string_view ZipNameOf(std::string_view partName) {
  return partName.starts_with('/') ? partName.substr(1) : partName;
}

std::string LowerAscii(std::string_view s) {
  std::string out(s);
  for (auto& c : out)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  return out;
}

bool EndsWithNoCase(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && LowerAscii(s.substr(s.size() - suffix.size())) == suffix;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::vector<uint8_t> ToBytes(std::string_view s) { return std::vector<uint8_t>(s.begin(), s.end()); }

std::string_view AsText(const std::vector<uint8_t>& bytes) {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace

std::string PartKey(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  for (size_t i = 0; i < name.size(); ++i) {
    if (name[i] == '%' && i + 2 < name.size() && HexValue(name[i + 1]) >= 0 && HexValue(name[i + 2]) >= 0) {
      out += static_cast<char>(HexValue(name[i + 1]) * 16 + HexValue(name[i + 2]));
      i += 2;
    } else {
      out += name[i];
    }
  }
  return LowerAscii(out);
}

std::string RelsPartFor(std::string_view source) {
  if (source == "/" || source.empty()) return "/_rels/.rels";
  size_t slash = source.rfind('/');
  return std::string(source.substr(0, slash + 1)) + "_rels/" + std::string(source.substr(slash + 1)) + ".rels";
}

std::string SourceOfRelsPart(std::string_view name) {
  if (!EndsWithNoCase(name, ".rels")) return {};
  size_t slash = name.rfind('/');
  if (slash == std::string_view::npos || slash < 6 || LowerAscii(name.substr(slash - 6, 7)) != "/_rels/") return {};
  std::string dir(name.substr(0, slash - 5));  // keeps the slash before _rels
  std::string file(name.substr(slash + 1, name.size() - slash - 1 - 5));
  if (dir == "/" && file.empty()) return "/";
  if (file.empty()) return {};
  return dir + file;
}

std::string ResolveTarget(std::string_view source, std::string_view target) {
  target = target.substr(0, std::min(target.find('#'), target.size()));
  if (target.empty()) return {};
  std::string path;
  if (target.front() == '/') {
    path = target;
  } else {
    path = source == "/" ? std::string("/") : std::string(source.substr(0, source.rfind('/') + 1));
    path += target;
  }
  // Remove dot segments; ".." above the root stays at the root, as in RFC 3986.
  std::vector<std::string_view> segments;
  std::string_view rest(path);
  rest.remove_prefix(1);
  for (;;) {
    size_t slash = rest.find('/');
    std::string_view seg = rest.substr(0, slash);
    if (seg == "..") {
      if (!segments.empty()) segments.pop_back();
    } else if (seg != "." && !(seg.empty() && slash != std::string_view::npos)) {
      segments.push_back(seg);
    }
    if (slash == std::string_view::npos) break;
    rest.remove_prefix(slash + 1);
  }
  std::string out;
  for (auto seg : segments) {
    out += '/';
    out += seg;
  }
  return out.empty() ? "/" : out;
}

// ---------- Relationships and content types ----------

const Relationship* RelationshipSet::Find(std::string_view id) const {
  for (const auto& r : items)
    if (r.id == id) return &r;
  return nullptr;
}

std::string RelationshipSet::Serialize() const {
  XmlWriter w;
  w.Declaration();
  w.StartElement("Relationships");
  w.Attribute("xmlns", kRelationshipsNs);
  for (const auto& r : items) {
    w.StartElement("Relationship");
    w.Attribute("Id", r.id);
    w.Attribute("Type", r.type);
    w.Attribute("Target", r.target);
    if (r.external) w.Attribute("TargetMode", "External");
    w.EndElement();
  }
  w.EndElement();
  return w.Take();
}

std::string_view ContentTypes::Lookup(std::string_view partName) const {
  std::string key = PartKey(partName);
  for (const auto& [name, type] : overrides)
    if (PartKey(name) == key) return type;
  return LookupDefault(partName);
}

std::string_view ContentTypes::LookupDefault(std::string_view partName) const {
  size_t slash = partName.rfind('/');
  size_t dot = partName.rfind('.');
  if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
  std::string ext = LowerAscii(partName.substr(dot + 1));
  for (const auto& [extension, type] : defaults)
    if (LowerAscii(extension) == ext) return type;
  return {};
}

std::string ContentTypes::Serialize() const {
  XmlWriter w;
  w.Declaration();
  w.StartElement("Types");
  w.Attribute("xmlns", kContentTypesNs);
  for (const auto& [extension, type] : defaults) {
    w.StartElement("Default");
    w.Attribute("Extension", extension);
    w.Attribute("ContentType", type);
    w.EndElement();
  }
  for (const auto& [name, type] : overrides) {
    w.StartElement("Override");
    w.Attribute("PartName", name);
    w.Attribute("ContentType", type);
    w.EndElement();
  }
  w.EndElement();
  return w.Take();
}

// ---------- Package ----------

std::unique_ptr<Package> Package::Open(std::unique_ptr<Snapshot> snapshot, const Limits& limits, Status& status) {
  std::unique_ptr<Package> pkg(new Package());
  pkg->snapshot_ = std::move(snapshot);
  pkg->limits_ = limits;
  status = pkg->zip_.Open(*pkg->snapshot_, limits);
  if (!status.Ok()) return nullptr;

  const auto& entries = pkg->zip_.Entries();
  pkg->entryToPart_.assign(entries.size(), Part::kNoEntry);
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].IsDirectory()) continue;
    Part part;
    part.name = "/" + entries[i].name;
    part.entry = i;
    if (!pkg->index_.emplace(PartKey(part.name), pkg->parts_.size()).second) {
      status = Status::Fail(Status::Code::Damaged, "This file contains two parts named " + entries[i].name + ".");
      return nullptr;
    }
    pkg->entryToPart_[i] = pkg->parts_.size();
    pkg->parts_.push_back(std::move(part));
  }

  // Content types: without them nothing in the package can be interpreted.
  std::vector<uint8_t> bytes;
  if (!pkg->FindPart(kContentTypesPart)) {
    status = Status::Fail(Status::Code::Damaged,
                          "This file is missing its list of contents ([Content_Types].xml), so it isn't a valid "
                          "Office file.");
    return nullptr;
  }
  status = pkg->ReadPart(kContentTypesPart, bytes);
  if (!status.Ok()) return nullptr;
  status = ParseContentTypes(AsText(bytes), limits, pkg->types_);
  if (!status.Ok()) return nullptr;
  pkg->IndexTypes();

  for (const auto& part : pkg->parts_) {
    if (SourceOfRelsPart(part.name).empty()) continue;
    status = pkg->ReadRelationships(part);
    if (!status.Ok()) return nullptr;
  }
  status = Status{};
  return pkg;
}

Status ParseContentTypes(std::string_view xml, const Limits& limits, ContentTypes& out) {
  XmlDom dom;
  Status s = dom.Parse(xml, limits, ZipNameOf(kContentTypesPart));
  if (!s.Ok()) return s;
  if (!IsElement(dom.Root(), kContentTypesNs, "Types"))
    return Status::Fail(Status::Code::Damaged, "Part [Content_Types].xml is damaged.");
  for (auto child : dom.Root().children()) {
    if (IsElement(child, kContentTypesNs, "Default"))
      out.defaults.emplace_back(child.attribute("Extension").value(), child.attribute("ContentType").value());
    else if (IsElement(child, kContentTypesNs, "Override"))
      out.overrides.emplace_back(child.attribute("PartName").value(), child.attribute("ContentType").value());
  }
  return Status{};
}

Status ParseRelationships(std::string_view xml, std::string_view source, const Limits& limits, std::string_view label,
                          RelationshipSet& out) {
  XmlDom dom;
  Status s = dom.Parse(xml, limits, label);
  if (!s.Ok()) return s;
  if (!IsElement(dom.Root(), kRelationshipsNs, "Relationships"))
    return Status::Fail(Status::Code::Damaged, "Part " + std::string(label) + " is damaged.");
  out.source.assign(source);
  for (auto child : dom.Root().children()) {
    if (!IsElement(child, kRelationshipsNs, "Relationship")) continue;
    Relationship r;
    r.id = child.attribute("Id").value();
    r.type = child.attribute("Type").value();
    r.target = child.attribute("Target").value();
    r.external = std::string_view(child.attribute("TargetMode").value()) == "External";
    if (!r.external) r.targetPart = ResolveTarget(out.source, r.target);
    out.items.push_back(std::move(r));
  }
  return Status{};
}

Status Package::ReadRelationships(const Part& part) {
  std::vector<uint8_t> bytes;
  Status s = ReadPart(part.name, bytes);
  if (!s.Ok()) return s;
  RelationshipSet set;
  s = ParseRelationships(AsText(bytes), SourceOfRelsPart(part.name), limits_, ZipNameOf(part.name), set);
  if (!s.Ok()) return s;
  set.relsPart = part.name;
  relsIndex_.emplace(PartKey(set.source), rels_.size());
  rels_.push_back(std::move(set));
  return Status{};
}

void Package::IndexTypes() {
  overrideIndex_.clear();
  for (size_t i = 0; i < types_.overrides.size(); ++i)
    overrideIndex_.emplace(PartKey(types_.overrides[i].first), i);  // the first of duplicates wins
}

std::string_view Package::ContentTypeOf(std::string_view partName) const {
  auto it = overrideIndex_.find(PartKey(partName));
  if (it != overrideIndex_.end()) return types_.overrides[it->second].second;
  return types_.LookupDefault(partName);
}

const Part* Package::FindPart(std::string_view name) const {
  return const_cast<Package*>(this)->FindPartMutable(name);
}

Part* Package::FindPartMutable(std::string_view name) {
  auto it = index_.find(PartKey(name));
  if (it == index_.end() || parts_[it->second].deleted) return nullptr;
  return &parts_[it->second];
}

Status Package::ReadPart(std::string_view name, std::vector<uint8_t>& out) const {
  const Part* p = FindPart(name);
  if (!p) return Status::Fail(Status::Code::NotFound, "Part " + std::string(ZipNameOf(name)) + " is missing.");
  if (p->modified || p->entry == Part::kNoEntry) {
    out = p->data;
    return Status{};
  }
  return zip_.Read(zip_.Entries()[p->entry], out);
}

bool Package::IsXmlPart(std::string_view partName) const {
  std::string_view type = ContentTypeOf(partName);
  if (!type.empty()) return type.ends_with("+xml") || type == "application/xml" || type == "text/xml";
  return EndsWithNoCase(partName, ".xml") || EndsWithNoCase(partName, ".rels");
}

const RelationshipSet* Package::RelationshipsOf(std::string_view source) const {
  auto it = relsIndex_.find(PartKey(source));
  return it == relsIndex_.end() ? nullptr : &rels_[it->second];
}

void Package::SetPart(std::string_view name, std::vector<uint8_t> data) {
  Part* p = FindPartMutable(name);
  if (!p) {
    index_[PartKey(name)] = parts_.size();
    parts_.emplace_back();
    p = &parts_.back();
    p->name.assign(name);
  }
  p->modified = true;
  p->data = std::move(data);
}

void Package::DeletePart(std::string_view name) {
  if (Part* p = FindPartMutable(name)) p->deleted = true;
}

void Package::SetContentTypes(ContentTypes types) {
  types_ = std::move(types);
  IndexTypes();
  SetPart(kContentTypesPart, ToBytes(types_.Serialize()));
}

void Package::SetRelationships(RelationshipSet set) {
  if (set.relsPart.empty()) set.relsPart = RelsPartFor(set.source);
  for (auto& r : set.items)
    r.targetPart = r.external ? std::string() : ResolveTarget(set.source, r.target);
  SetPart(set.relsPart, ToBytes(set.Serialize()));
  std::string key = PartKey(set.source);
  if (auto it = relsIndex_.find(key); it != relsIndex_.end()) {
    rels_[it->second] = std::move(set);
    return;
  }
  relsIndex_.emplace(std::move(key), rels_.size());
  rels_.push_back(std::move(set));
}

Status Package::Verify() const {
  std::vector<uint8_t> bytes;
  for (const auto& part : parts_) {
    if (part.deleted) continue;
    Status s = ReadPart(part.name, bytes);
    if (!s.Ok()) return s;
    if (IsXmlPart(part.name)) {
      s = XmlPull::Check(AsText(bytes), limits_, ZipNameOf(part.name));
      if (!s.Ok()) return s;
    }
  }
  return Status{};
}

Status Package::Save(ByteSink& sink) const {
  ZipWriter writer(sink);
  const auto& entries = zip_.Entries();
  for (size_t i = 0; i < entries.size(); ++i) {
    size_t pi = entryToPart_[i];
    Status s;
    if (pi == Part::kNoEntry) {
      s = writer.CopyRaw(zip_, entries[i]);  // a directory entry
    } else {
      const Part& part = parts_[pi];
      if (part.deleted) continue;
      s = part.modified ? writer.Add(ZipNameOf(part.name), part.data, &entries[i]) : writer.CopyRaw(zip_, entries[i]);
    }
    if (!s.Ok()) return s;
  }
  for (const auto& part : parts_) {
    if (part.entry != Part::kNoEntry || part.deleted) continue;
    Status s = writer.Add(ZipNameOf(part.name), part.data);
    if (!s.Ok()) return s;
  }
  return writer.Finish(zip_.Comment());
}

}  // namespace fw::pkg
