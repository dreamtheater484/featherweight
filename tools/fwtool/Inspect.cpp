// fwtool dump and fwtool check: what a package contains, and whether its references hold.
#include <cstdio>
#include <set>

#include "Common.h"
#include "pkg/References.h"
#include "pkg/XmlDom.h"

namespace fwtool {
namespace {

using fw::pkg::Package;
using fw::pkg::Status;

const char* MethodName(uint16_t method) { return method == 0 ? "stored" : "deflated"; }

void DumpFile(const std::filesystem::path& file) {
  Status st;
  auto pkg = OpenPackage(file, st);
  std::printf("%s\n", Display(file).c_str());
  if (!pkg) {
    std::printf("  refused: %s\n\n", st.message.c_str());
    return;
  }
  const auto& types = pkg->Types();
  std::printf("\nContent types\n");
  for (const auto& [ext, type] : types.defaults) std::printf("  default   %-28s %s\n", ext.c_str(), type.c_str());
  for (const auto& [name, type] : types.overrides) std::printf("  override  %-28s %s\n", name.c_str(), type.c_str());

  std::printf("\nParts\n");
  for (const auto& part : pkg->Parts()) {
    const auto& e = pkg->Zip().Entries()[part.entry];
    std::string_view type = pkg->ContentTypeOf(part.name);
    std::printf("  %-40s %-8s %10llu -> %10llu  %s\n", part.name.c_str(), MethodName(e.method),
                static_cast<unsigned long long>(e.uncompressedSize), static_cast<unsigned long long>(e.compressedSize),
                type.empty() ? "(no content type)" : std::string(type).c_str());
  }

  std::printf("\nRelationships\n");
  for (const auto& set : pkg->Relationships()) {
    std::printf("  %s  (%s)\n", set.source.c_str(), set.relsPart.c_str());
    for (const auto& r : set.items) {
      std::string_view type = r.type;
      type = type.substr(type.rfind('/') + 1);
      if (r.external)
        std::printf("    %-8s %-24s %s (external)\n", r.id.c_str(), std::string(type).c_str(), r.target.c_str());
      else
        std::printf("    %-8s %-24s %s -> %s%s\n", r.id.c_str(), std::string(type).c_str(), r.target.c_str(),
                    r.targetPart.c_str(), pkg->FindPart(r.targetPart) ? "" : "  (missing)");
    }
  }
  std::printf("\n");
}

struct Report {
  std::vector<std::string> errors, warnings;
  size_t relationships = 0, references = 0;
};

Report CheckFile(const std::filesystem::path& file) {
  Report rep;
  Status st;
  auto pkg = OpenPackage(file, st);
  if (pkg) st = pkg->Verify();
  if (!st.Ok()) {
    rep.errors.push_back(st.message);
    return rep;
  }

  // Relationships: unique, non-empty ids; internal targets that exist.
  if (!pkg->RelationshipsOf("/")) rep.errors.push_back("the package has no relationships (_rels/.rels)");
  for (const auto& set : pkg->Relationships()) {
    std::string where = set.relsPart.substr(1);
    if (set.source != "/" && !pkg->FindPart(set.source))
      rep.warnings.push_back(where + ": relationships for " + set.source + ", which doesn't exist");
    std::set<std::string> ids;
    for (const auto& r : set.items) {
      ++rep.relationships;
      if (r.id.empty()) rep.errors.push_back(where + ": a relationship has no id");
      else if (!ids.insert(r.id).second) rep.errors.push_back(where + ": id " + r.id + " is used twice");
      if (r.external) continue;
      if (r.targetPart.empty())
        rep.errors.push_back(where + ": " + r.id + " has no target");
      else if (!pkg->FindPart(r.targetPart))
        rep.errors.push_back(where + ": " + r.id + " points to " + r.targetPart + ", which doesn't exist");
    }
  }

  // Content types: every part has one; overrides name parts that exist.
  for (const auto& part : pkg->Parts()) {
    if (part.name == fw::pkg::kContentTypesPart) continue;
    if (pkg->ContentTypeOf(part.name).empty()) rep.errors.push_back(part.name.substr(1) + " has no content type");
  }
  for (const auto& [name, type] : pkg->Types().overrides)
    if (!pkg->FindPart(name)) rep.warnings.push_back("a content type is given for " + name + ", which doesn't exist");

  // References from the registry (for now: relationship ids) resolve in the part's own relationships.
  auto registry = fw::pkg::ReferenceRegistry::Default();
  std::vector<uint8_t> bytes;
  for (const auto& part : pkg->Parts()) {
    if (part.name == fw::pkg::kContentTypesPart || !fw::pkg::SourceOfRelsPart(part.name).empty() ||
        !pkg->IsXmlPart(part.name))
      continue;
    fw::pkg::XmlDom dom;
    if (!(st = pkg->ReadPart(part.name, bytes)).Ok() ||
        !(st = dom.Parse(AsText(bytes), pkg->GetLimits(), part.name.substr(1))).Ok()) {
      rep.errors.push_back(st.message);
      continue;
    }
    const auto* rels = pkg->RelationshipsOf(part.name);
    for (const auto& ref : registry.Find(dom.Root(), fw::pkg::RefKind::RelationshipId)) {
      ++rep.references;
      if (!rels || !rels->Find(ref.attr.value()))
        rep.errors.push_back(part.name.substr(1) + ": " + ref.attr.name() + "=\"" + ref.attr.value() +
                             "\" doesn't match any of its relationships");
    }
  }
  return rep;
}

}  // namespace

int RunDump(const Options& options) {
  for (const auto& file : CollectFiles(options.inputs)) DumpFile(file);
  return 0;
}

int RunCheck(const Options& options) {
  auto files = CollectFiles(options.inputs);
  if (files.empty()) {
    std::fprintf(stderr, "No Office files found.\n");
    return 1;
  }
  int failed = 0;
  for (const auto& file : files) {
    Report rep = CheckFile(file);
    bool ok = rep.errors.empty();
    std::printf("%s %s: %zu relationships, %zu references\n", ok ? "[ ok ]" : "[FAIL]", Display(file).c_str(),
                rep.relationships, rep.references);
    for (const auto& e : rep.errors) std::printf("    error: %s\n", e.c_str());
    for (const auto& w : rep.warnings) std::printf("    warning: %s\n", w.c_str());
    if (!ok) ++failed;
  }
  std::printf("\n%zu files, %d failed\n", files.size(), failed);
  return failed == 0 ? 0 : 1;
}

}  // namespace fwtool
