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
  for (uint32_t i : parts) {
    if (i >= doc.Parts().size()) continue;
    const Part& part = doc.Parts()[i];
    if (!part.writable || !part.Editable())
      return Status::Fail(Status::Code::Unsupported, "Part of this document can't be saved by Featherweight.");
    std::string xml;
    Status st = WritePart(doc, i, xml);
    if (!st.Ok()) return st;
    package.SetPart(part.name, std::vector<uint8_t>(xml.begin(), xml.end()));
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

  pkg::MemorySink sink;
  Status st = package.Save(sink);
  if (!st.Ok()) return st;

  // Open what was written and check every part that was rewritten; the others are copies.
  auto again = pkg::Package::Open(pkg::Snapshot::FromBytes(sink.bytes), package.GetLimits(), st);
  if (!again) return Status::Fail(Status::Code::Damaged, "The saved document didn't open again: " + st.message);
  std::vector<uint8_t> bytes;
  for (const auto& p : package.Parts()) {
    if (!p.modified || p.deleted) continue;
    if (!(st = again->ReadPart(p.name, bytes)).Ok() || bytes != p.data)
      return Status::Fail(Status::Code::Damaged, "The saved document didn't read back correctly.");
    if (package.IsXmlPart(p.name) && !(st = pkg::XmlPull::Check(AsText(bytes), package.GetLimits(), p.name)).Ok())
      return st;
  }
  out = std::move(sink.bytes);
  return Status{};
}

}  // namespace fw::doc::docx
