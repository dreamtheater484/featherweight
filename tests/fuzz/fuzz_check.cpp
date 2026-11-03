// Fuzz target: fwtool check's path over a package (the reference registry, the checker, the id
// renewer and the extension query), on content the models don't understand.
// Seeds: tests/fuzz/seeds/package (corpus files as text, without their largest parts).
//
// An input is either a zip (a corpus file), or parts as text, which lets the fuzzer change the XML
// inside: each part starts on a line "@<part name>", and the rest of the input up to the next such
// line is its content.
//   @[Content_Types].xml
//   <Types ...>...</Types>
//   @word/document.xml
//   <w:document ...>...</w:document>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pkg/Check.h"
#include "pkg/Package.h"
#include "pkg/References.h"
#include "pkg/Snapshot.h"
#include "pkg/XmlDom.h"
#include "pkg/Zip.h"

using namespace fw::pkg;

namespace {

Limits FuzzLimits() {
  Limits limits;
  limits.maxEntrySize = 16u << 20;
  limits.maxTotalSize = 64u << 20;
  limits.maxEntries = 4096;
  limits.maxStringLength = 1u << 20;
  return limits;
}

// Parts as text, stored in a zip.
std::vector<uint8_t> ZipFromText(std::string_view text) {
  MemorySink sink;
  ZipWriter writer(sink);
  size_t pos = text.starts_with('@') ? 0 : text.find("\n@");
  while (pos != std::string_view::npos) {
    if (text[pos] == '\n') ++pos;
    size_t eol = text.find('\n', pos);
    if (eol == std::string_view::npos) break;
    std::string_view name = text.substr(pos + 1, eol - pos - 1);
    if (!name.empty() && name.back() == '\r') name.remove_suffix(1);
    size_t next = text.find("\n@", eol);
    std::string_view body = text.substr(eol + 1, (next == std::string_view::npos ? text.size() : next) - eol - 1);
    auto bytes = reinterpret_cast<const uint8_t*>(body.data());
    writer.Add(name, std::span<const uint8_t>(bytes, body.size()), nullptr, ZipWriter::Compression::Store);
    pos = next;
  }
  writer.Finish();
  return std::move(sink.bytes);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const Limits limits = FuzzLimits();
  std::string_view input(reinterpret_cast<const char*>(data), size);
  std::vector<uint8_t> zip = input.starts_with("PK\x03\x04") ? std::vector<uint8_t>(data, data + size) : ZipFromText(input);

  Status st;
  auto pkg = Package::Open(Snapshot::FromBytes(std::move(zip)), limits, st);
  if (!pkg) return 0;
  static const ReferenceRegistry registry = ReferenceRegistry::Default();
  CheckPackage(*pkg, registry);

  // What the editors do with a part: block edits near unknown extensions, renew ids for pasting,
  // rename references. None of it may break the document.
  std::vector<uint8_t> bytes;
  for (const auto& part : pkg->Parts()) {
    if (part.deleted || !pkg->IsXmlPart(part.name) || !SourceOfRelsPart(part.name).empty()) continue;
    XmlDom dom;
    if (!pkg->ReadPart(part.name, bytes).Ok() ||
        !dom.Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), limits).Ok())
      continue;
    registry.WouldStrand(dom.Root(), {{RefKind::SheetName, {"Sheet1"}}, {RefKind::CellRange, {}}});
    auto refs = registry.Find(dom.Root());
    if (!refs.empty()) {
      std::string from(refs.front().Value());
      registry.RenameInTree(refs.front().kind, from, "Renamed 1", dom.Root());
    }
    IdRenewer renewer(registry);
    renewer.Take(dom.Root());
    renewer.Renew(dom.Root());  // everything collides with itself
    // The result is still a document.
    XmlDom again;
    if (!again.Parse(dom.Serialize(), limits).Ok()) std::abort();
  }
  return 0;
}
