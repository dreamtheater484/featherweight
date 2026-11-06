// fwtool dump and fwtool check: what a package contains, and whether its references hold.
#include <cstdio>
#include <fstream>
#include <iterator>

#include "Common.h"
#include "LayoutHook.h"
#include "doc/docx/Blank.h"
#include "doc/docx/Docx.h"
#include "doc/odf/Odt.h"
#include "pkg/Check.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
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
  for (const auto& file : CollectFiles(options.inputs)) {
    if (options.layout) DumpLayout(file, options.outDir);
    else if (options.model) DumpModel(file);
    else DumpFile(file);
  }
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

// OpenDocument text files and Word templates as the app opens them (D5): converted to .docx (a
// template to a new document made from it), checked, and read into the model. With --out, the
// converted documents are kept there (for the Word open check).
int RunOdt(const Options& options) {
  std::vector<std::filesystem::path> files;
  auto take = [&](const std::filesystem::path& f) {
    const std::wstring ext = f.extension().wstring();
    for (const wchar_t* e : {L".odt", L".ott", L".dotx", L".dotm"})
      if (_wcsicmp(ext.c_str(), e) == 0) files.push_back(f);
  };
  for (const auto& in : options.inputs) {
    std::error_code ec;
    if (std::filesystem::is_directory(in, ec)) {
      for (const auto& e : std::filesystem::recursive_directory_iterator(in, ec))
        if (e.is_regular_file()) take(e.path());
    } else {
      take(in);
    }
  }
  if (!options.outDir.empty()) std::filesystem::create_directories(options.outDir);
  int failed = 0;
  for (const auto& file : files) {
    std::ifstream f(file, std::ios::binary);
    std::vector<uint8_t> odt((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<uint8_t> docx;
    const bool word = _wcsnicmp(file.extension().wstring().c_str(), L".do", 3) == 0;
    fw::pkg::Status st = word ? fw::doc::docx::DocumentFromTemplate(odt, docx) : fw::doc::odf::OdtToDocx(odt, docx);
    std::string problem;
    size_t paragraphs = 0;
    if (!st.Ok()) {
      problem = "not converted: " + st.message;
    } else {
      auto pkg = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(docx), fw::pkg::Limits::Default(), st);
      if (!pkg) {
        problem = "the .docx doesn't open: " + st.message;
      } else {
        fw::pkg::CheckReport rep = fw::pkg::CheckPackage(*pkg);
        for (const auto& e : rep.errors) problem += (problem.empty() ? "check: " : "; ") + e;
        auto doc = problem.empty() ? fw::doc::docx::Read(std::move(pkg), st) : nullptr;
        if (problem.empty() && !doc) problem = "not read: " + st.message;
        if (doc) paragraphs = doc->Paragraphs().size();
      }
    }
    if (!options.outDir.empty() && !docx.empty()) {
      std::ofstream out(options.outDir / (file.stem().wstring() + L".docx"), std::ios::binary);
      out.write(reinterpret_cast<const char*>(docx.data()), static_cast<std::streamsize>(docx.size()));
    }
    std::printf("%s %s: %zu paragraphs, %zu KB%s%s\n", problem.empty() ? "[ ok ]" : "[FAIL]", Display(file).c_str(), paragraphs,
                docx.size() / 1024, problem.empty() ? "" : "\n    ", problem.c_str());
    if (!problem.empty()) ++failed;
  }
  std::printf("\n%zu files, %d failed\n", files.size(), failed);
  return failed == 0 && !files.empty() ? 0 : 1;
}

}  // namespace fwtool
