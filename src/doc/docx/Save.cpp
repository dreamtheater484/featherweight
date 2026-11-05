#include "doc/docx/Save.h"

#include <windows.h>

#include <algorithm>

#include "doc/docx/Docx.h"
#include "pkg/Package.h"
#include "pkg/XmlDom.h"
#include "pkg/XmlPull.h"
#include "pkg/Zip.h"

namespace fw::doc::docx {
namespace {

using pkg::Status;

constexpr std::string_view kCp = "http://schemas.openxmlformats.org/package/2006/metadata/core-properties";
constexpr std::string_view kDcTerms = "http://purl.org/dc/terms/";
constexpr std::string_view kXsi = "http://www.w3.org/2001/XMLSchema-instance";
constexpr std::string_view kImageRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image";
constexpr std::string_view kHeaderRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/header";
constexpr std::string_view kFooterRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/footer";

// Any mention of a relationship's id in a part keeps it: an attribute, kept XML, a field.
bool Mentions(std::string_view xml, const std::string& id) {
  return xml.find("\"" + id + "\"") != std::string_view::npos || xml.find("'" + id + "'") != std::string_view::npos;
}

// The prefix `uri` has on the root, declaring it as `preferred` (or a free variant) if it has none.
std::string PrefixFor(pugi::xml_node root, std::string_view uri, std::string preferred) {
  for (pugi::xml_attribute a : root.attributes()) {
    std::string_view name = a.name();
    if (a.value() != uri) continue;
    if (name == "xmlns") return {};
    if (name.starts_with("xmlns:")) return std::string(name.substr(6));
  }
  while (root.attribute(("xmlns:" + preferred).c_str())) preferred += "1";
  root.append_attribute(("xmlns:" + preferred).c_str()).set_value(std::string(uri).c_str());
  return preferred;
}

std::string Qualified(const std::string& prefix, std::string_view local) {
  return prefix.empty() ? std::string(local) : prefix + ":" + std::string(local);
}

// The root's child element {uri}local, added at the end when there is none, with `text` as its content.
pugi::xml_node SetText(pugi::xml_node root, std::string_view uri, std::string_view local, const char* preferred,
                       const std::string& text) {
  pugi::xml_node node;
  for (pugi::xml_node c : root.children())
    if (c.type() == pugi::node_element && pkg::IsElement(c, uri, local)) node = c;
  if (!node) node = root.append_child(Qualified(PrefixFor(root, uri, preferred), local).c_str());
  while (node.first_child()) node.remove_child(node.first_child());
  node.append_child(pugi::node_pcdata).set_value(text.c_str());
  return node;
}

// The package-relative name of the core properties part, or empty.
std::string CorePropertiesPart(const pkg::Package& package) {
  if (const pkg::RelationshipSet* rels = package.RelationshipsOf("/"))
    for (const auto& r : rels->items)
      if (!r.external && r.type.ends_with("/metadata/core-properties")) return r.targetPart;
  return {};
}

std::string_view AsText(const std::vector<uint8_t>& bytes) {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace

std::string W3cTime(int64_t filetime) {
  FILETIME ft;
  if (filetime) {
    ft.dwLowDateTime = static_cast<DWORD>(filetime);
    ft.dwHighDateTime = static_cast<DWORD>(static_cast<uint64_t>(filetime) >> 32);
  } else {
    GetSystemTimeAsFileTime(&ft);
  }
  SYSTEMTIME st{};
  FileTimeToSystemTime(&ft, &st);
  char buf[32];
  snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
           st.wSecond);
  return buf;
}

Status UpdateCoreProperties(std::string_view xml, const std::string& author, int64_t now, std::string& out) {
  pkg::XmlDom dom;
  Status st = dom.Parse(xml, pkg::Limits::Default(), "docProps/core.xml");
  if (!st.Ok()) return st;
  pugi::xml_node root = dom.Root();
  if (!pkg::IsElement(root, kCp, "coreProperties"))
    return Status::Fail(Status::Code::Damaged, "The document's properties aren't in the expected form.");
  if (!author.empty()) SetText(root, kCp, "lastModifiedBy", "cp", author);
  pugi::xml_node modified = SetText(root, kDcTerms, "modified", "dcterms", W3cTime(now));
  // W3CDTF must be named, as Word does: xsi:type="dcterms:W3CDTF".
  const std::string xsiType = Qualified(PrefixFor(root, kXsi, "xsi"), "type");
  pugi::xml_attribute type = modified.attribute(xsiType.c_str());
  if (!type) type = modified.append_attribute(xsiType.c_str());
  type.set_value(Qualified(PrefixFor(root, kDcTerms, "dcterms"), "W3CDTF").c_str());
  out = dom.Serialize();
  return Status{};
}

UnusedMedia FindUnusedMedia(const Document& doc, const std::vector<std::pair<uint32_t, const std::string*>>& written) {
  UnusedMedia out;
  const pkg::Package& package = doc.Package();
  auto storyRel = [](const pkg::Relationship& r) { return !r.external && (r.type == kHeaderRel || r.type == kFooterRel); };
  for (const auto& [index, xml] : written) {
    if (index >= doc.Parts().size() || !xml) continue;
    const pkg::RelationshipSet* rels = package.RelationshipsOf(doc.Parts()[index].name);
    if (!rels) continue;
    for (const pkg::Relationship& r : rels->items)
      if (((!r.external && r.type == kImageRel) || storyRel(r)) && !Mentions(*xml, r.id))
        out.relationships.emplace_back(rels->source, r.id);
  }
  // A header or footer made and then undone (d3-torture finding 17) left a relationship in a part
  // whose XML is as it was, so isn't written: its relationships changed since it was opened. Such a
  // part is read as the package has it.
  std::vector<uint8_t> bytes;
  for (uint32_t i = 0; i < doc.Parts().size(); ++i) {
    if (std::any_of(written.begin(), written.end(), [&](const auto& w) { return w.first == i && w.second; })) continue;
    const pkg::RelationshipSet* rels = package.RelationshipsOf(doc.Parts()[i].name);
    const pkg::Part* relsPart = rels ? package.FindPart(rels->relsPart) : nullptr;
    if (!relsPart || !relsPart->modified || std::none_of(rels->items.begin(), rels->items.end(), storyRel)) continue;
    if (!package.ReadPart(doc.Parts()[i].name, bytes).Ok()) continue;
    const std::string_view xml = AsText(bytes);
    for (const pkg::Relationship& r : rels->items)
      if (storyRel(r) && !Mentions(xml, r.id)) out.relationships.emplace_back(rels->source, r.id);
  }
  // A part goes when every relationship to it does.
  for (const auto& [source, id] : out.relationships) {
    const pkg::RelationshipSet* rels = package.RelationshipsOf(source);
    const pkg::Relationship* r = rels ? rels->Find(id) : nullptr;
    if (!r || r->targetPart.empty()) continue;
    const std::string target = pkg::PartKey(r->targetPart);
    if (std::any_of(out.parts.begin(), out.parts.end(), [&](const std::string& p) { return pkg::PartKey(p) == target; }))
      continue;
    bool used = false;
    for (const pkg::RelationshipSet& set : package.Relationships())
      for (const pkg::Relationship& other : set.items) {
        if (other.external || pkg::PartKey(other.targetPart) != target) continue;
        const bool going = std::find(out.relationships.begin(), out.relationships.end(),
                                     std::make_pair(set.source, other.id)) != out.relationships.end();
        if (!going) used = true;
      }
    if (!used) out.parts.push_back(r->targetPart);
  }
  return out;
}

Status Serialize(Document& doc, const SaveOptions& options, std::vector<uint8_t>& out) {
  pkg::Package& package = doc.Package();
  bool changed = options.edited || !options.changedParts.empty();
  for (const auto& p : package.Parts()) changed = changed || p.modified || p.deleted;
  if (!changed) {
    // Nothing edited: the file exactly as it was opened.
    const pkg::Snapshot& snapshot = package.GetSnapshot();
    out.resize(static_cast<size_t>(snapshot.Size()));
    if (!snapshot.ReadAt(0, out.data(), out.size()))
      return Status::Fail(Status::Code::IoError, "The document's private copy couldn't be read.");
    return Status{};
  }

  // The edited parts, and every part written before (an earlier save or autosave): the package
  // holds that version, and an undo since may have changed the model back.
  std::vector<uint32_t> parts = options.changedParts;
  for (uint32_t i = 0; i < doc.Parts().size(); ++i) {
    const pkg::Part* written = package.FindPart(doc.Parts()[i].name);
    if (written && written->modified && std::find(parts.begin(), parts.end(), i) == parts.end()) parts.push_back(i);
  }
  std::vector<std::string> xmls(parts.size());
  std::vector<std::pair<uint32_t, const std::string*>> written;
  for (size_t k = 0; k < parts.size(); ++k) {
    const uint32_t i = parts[k];
    if (i >= doc.Parts().size()) continue;
    const Part& part = doc.Parts()[i];
    if (!part.writable || !part.Editable())
      return Status::Fail(Status::Code::Unsupported, "Part of this document can't be saved by Featherweight.");
    std::string& xml = xmls[k];
    Status st = WritePart(doc, i, xml);
    if (!st.Ok()) return st;
    package.SetPart(part.name, std::vector<uint8_t>(xml.begin(), xml.end()));
    written.emplace_back(i, &xml);
  }
  if (options.edited) {
    if (std::string core = CorePropertiesPart(package); !core.empty() && package.FindPart(core)) {
      std::vector<uint8_t> bytes;
      std::string updated;
      // Properties that can't be read stay as they were: they don't stop a save.
      if (package.ReadPart(core, bytes).Ok() &&
          UpdateCoreProperties(AsText(bytes), options.author, options.now, updated).Ok())
        package.SetPart(core, std::vector<uint8_t>(updated.begin(), updated.end()));
    }
  }

  // Pictures nothing refers to any more stay out of the file: their relationships, their parts and
  // any content type naming those. The package keeps them, for undo.
  pkg::Package::SaveChanges leave;
  const UnusedMedia unused = FindUnusedMedia(doc, written);
  std::vector<pkg::RelationshipSet> sets;
  for (const auto& [source, id] : unused.relationships) {
    auto set = std::find_if(sets.begin(), sets.end(), [&](const pkg::RelationshipSet& s) { return s.source == source; });
    if (set == sets.end()) {
      sets.push_back(*package.RelationshipsOf(source));
      set = sets.end() - 1;
    }
    std::erase_if(set->items, [&](const pkg::Relationship& r) { return r.id == id; });
  }
  for (const pkg::RelationshipSet& set : sets) {
    const std::string xml = set.Serialize();
    leave.replace.emplace_back(set.relsPart, std::vector<uint8_t>(xml.begin(), xml.end()));
  }
  leave.leaveOut = unused.parts;
  for (const std::string& part : unused.parts)  // a header's own relationships go with it
    if (const pkg::RelationshipSet* rels = package.RelationshipsOf(part); rels && package.FindPart(rels->relsPart))
      leave.leaveOut.push_back(rels->relsPart);
  pkg::ContentTypes types = package.Types();
  const size_t overrides = types.overrides.size();
  std::erase_if(types.overrides, [&](const auto& o) {
    return std::any_of(unused.parts.begin(), unused.parts.end(),
                       [&](const std::string& p) { return pkg::PartKey(p) == pkg::PartKey(o.first); });
  });
  if (types.overrides.size() != overrides) {
    const std::string xml = types.Serialize();
    leave.replace.emplace_back(std::string(pkg::kContentTypesPart), std::vector<uint8_t>(xml.begin(), xml.end()));
  }

  pkg::MemorySink sink;
  Status st = package.Save(sink, leave);
  if (!st.Ok()) return st;

  // Open what was written and check every part that was rewritten; the others are copies.
  auto again = pkg::Package::Open(pkg::Snapshot::FromBytes(sink.bytes), package.GetLimits(), st);
  if (!again) return Status::Fail(Status::Code::Damaged, "The saved document didn't open again: " + st.message);
  std::vector<uint8_t> bytes;
  for (const auto& p : package.Parts()) {
    if (p.deleted) continue;
    const auto replaced = std::find_if(leave.replace.begin(), leave.replace.end(),
                                       [&](const auto& r) { return pkg::PartKey(r.first) == pkg::PartKey(p.name); });
    const bool leftOut = std::any_of(leave.leaveOut.begin(), leave.leaveOut.end(),
                                     [&](const std::string& n) { return pkg::PartKey(n) == pkg::PartKey(p.name); });
    if (leftOut) {
      if (again->FindPart(p.name)) return Status::Fail(Status::Code::Damaged, "The saved document kept a part it left out.");
      continue;
    }
    if (!p.modified && replaced == leave.replace.end()) continue;
    const std::vector<uint8_t>& want = replaced != leave.replace.end() ? replaced->second : p.data;
    if (!(st = again->ReadPart(p.name, bytes)).Ok() || bytes != want)
      return Status::Fail(Status::Code::Damaged, "The saved document didn't read back correctly.");
    if (package.IsXmlPart(p.name) && !(st = pkg::XmlPull::Check(AsText(bytes), package.GetLimits(), p.name)).Ok())
      return st;
  }
  out = std::move(sink.bytes);
  return Status{};
}

}  // namespace fw::doc::docx
