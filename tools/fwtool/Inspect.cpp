// fwtool dump and fwtool check: what a package contains, and whether its references hold.
#include <cstdio>

#include "Common.h"
#include "pkg/Check.h"
#include "pkg/References.h"

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

fw::pkg::CheckReport CheckFile(const std::filesystem::path& file, const fw::pkg::ReferenceRegistry& registry) {
  Status st;
  auto pkg = OpenPackage(file, st);
  if (!pkg) {
    fw::pkg::CheckReport rep;
    rep.errors.push_back(st.message);
    return rep;
  }
  return fw::pkg::CheckPackage(*pkg, registry);
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
  auto registry = fw::pkg::ReferenceRegistry::Default();
  int failed = 0;
  for (const auto& file : files) {
    fw::pkg::CheckReport rep = CheckFile(file, registry);
    // With --expect-reject, each file must have at least one error (the broken files).
    bool ok = options.expectReject ? !rep.Ok() : rep.Ok();
    std::printf("%s %s: %zu relationships, %zu references, %zu ids, %zu pairs, %zu derived values", ok ? "[ ok ]" : "[FAIL]",
                Display(file).c_str(), rep.relationships, rep.references, rep.ids, rep.pairs, rep.derived);
    if (rep.embedded) std::printf(", %zu embedded packages", rep.embedded);
    std::printf("%s\n", options.expectReject && ok ? " (rejected, as expected)" : "");
    for (const auto& e : rep.errors) std::printf("    error: %s\n", e.c_str());
    for (const auto& w : rep.warnings) std::printf("    warning: %s\n", w.c_str());
    for (const auto& x : rep.unknownExtensions) std::printf("    unknown extension (edits near it are blocked): %s\n", x.c_str());
    if (options.expectReject && rep.Ok()) std::printf("    expected an error, but the file passed\n");
    if (!ok) ++failed;
  }
  std::printf("\n%zu files, %d failed\n", files.size(), failed);
  return failed == 0 ? 0 : 1;
}

}  // namespace fwtool
