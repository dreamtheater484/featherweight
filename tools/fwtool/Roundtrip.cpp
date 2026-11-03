// fwtool roundtrip: the "open and save, no edits" and "re-serialize all" never-damage tests
// (plan §10), at the package level.
#include <cstdio>
#include <fstream>
#include <map>

#include "Common.h"
#include "pkg/Canonical.h"
#include "pkg/XmlDom.h"

namespace fwtool {
namespace {

using fw::pkg::Package;
using fw::pkg::Status;
using fw::pkg::ZipEntry;

bool SameCentralRecord(const ZipEntry& a, const ZipEntry& b) {
  return a.name == b.name && a.versionMadeBy == b.versionMadeBy && a.versionNeeded == b.versionNeeded &&
         a.flags == b.flags && a.method == b.method && a.modTime == b.modTime && a.modDate == b.modDate &&
         a.crc32 == b.crc32 && a.compressedSize == b.compressedSize && a.uncompressedSize == b.uncompressedSize &&
         a.internalAttrs == b.internalAttrs && a.externalAttrs == b.externalAttrs && a.extra == b.extra &&
         a.comment == b.comment;
}

struct Result {
  bool ok = true;
  std::string detail;
  Result& Fail(std::string why) {
    if (ok) detail = std::move(why);
    ok = false;
    return *this;
  }
};

Result RoundtripFile(const std::filesystem::path& file, const Options& options) {
  Result r;
  Status st;
  auto pkg = OpenPackage(file, st);
  if (pkg) st = pkg->Verify();
  if (options.expectReject) {
    if (st.Ok()) r.Fail("opened, but it should have been refused");
    else r.detail = "refused: " + st.message;
    return r;
  }
  if (!st.Ok()) {
    r.Fail(st.message);
    return r;
  }

  // Re-serialize all: every XML part through the DOM, compared after canonicalization.
  std::map<std::string, std::string> canonical;  // part name -> canonical original
  std::vector<uint8_t> bytes;
  for (size_t i = 0; options.reserializeAll && i < pkg->Parts().size(); ++i) {
    std::string name = pkg->Parts()[i].name;
    if (!pkg->IsXmlPart(name)) continue;
    std::string label = name.substr(1);
    if (!(st = pkg->ReadPart(name, bytes)).Ok()) return r.Fail(st.message);
    fw::pkg::XmlDom dom;
    if (!(st = dom.Parse(AsText(bytes), pkg->GetLimits(), label)).Ok()) return r.Fail(st.message);
    if (!(st = fw::pkg::CanonicalizeXml(AsText(bytes), canonical[name], pkg->GetLimits(), label)).Ok())
      return r.Fail(st.message);
    std::string out = dom.Serialize();
    pkg->SetPart(name, std::vector<uint8_t>(out.begin(), out.end()));
  }

  fw::pkg::MemorySink sink;
  if (!(st = pkg->Save(sink)).Ok()) return r.Fail("saving failed: " + st.message);
  if (!options.outDir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(options.outDir, ec);
    std::ofstream(options.outDir / file.filename(), std::ios::binary)
        .write(reinterpret_cast<const char*>(sink.bytes.data()), static_cast<std::streamsize>(sink.bytes.size()));
  }
  auto again = Package::Open(fw::pkg::Snapshot::FromBytes(std::move(sink.bytes)), pkg->GetLimits(), st);
  if (again) st = again->Verify();
  if (!st.Ok()) return r.Fail("the saved copy doesn't open: " + st.message);

  // Same entries in the same order; untouched ones identical byte for byte.
  const auto& before = pkg->Zip().Entries();
  const auto& after = again->Zip().Entries();
  if (before.size() != after.size()) return r.Fail("the saved copy has a different number of parts");
  if (pkg->Zip().Comment() != again->Zip().Comment()) r.Fail("the zip comment changed");
  size_t copied = 0;
  std::vector<uint8_t> a, b;
  for (size_t i = 0; i < before.size(); ++i) {
    std::string name = "/" + before[i].name;
    if (before[i].name != after[i].name) {
      r.Fail("part " + before[i].name + " moved or was renamed");
      continue;
    }
    if (auto it = canonical.find(name); it != canonical.end()) {
      std::string canon;
      if (!(st = again->ReadPart(name, bytes)).Ok() ||
          !(st = fw::pkg::CanonicalizeXml(AsText(bytes), canon, pkg->GetLimits(), before[i].name)).Ok())
        r.Fail(st.message);
      else if (canon != it->second)
        r.Fail("part " + before[i].name + " differs from the original after canonicalization");
      continue;
    }
    if (!SameCentralRecord(before[i], after[i])) r.Fail("part " + before[i].name + " has a different zip record");
    if (!pkg->Zip().ReadRecord(before[i], a).Ok() || !again->Zip().ReadRecord(after[i], b).Ok() || a != b)
      r.Fail("part " + before[i].name + " isn't byte-identical");
    else
      ++copied;
  }
  if (r.ok) {
    r.detail = std::to_string(copied) + (copied == 1 ? " part" : " parts") + " byte-identical";
    if (options.reserializeAll)
      r.detail += ", " + std::to_string(canonical.size()) + " XML rewritten and equal after canonicalization";
  }
  return r;
}

}  // namespace

int RunRoundtrip(const Options& options) {
  auto files = CollectFiles(options.inputs);
  if (files.empty()) {
    std::fprintf(stderr, "No Office files found.\n");
    return 1;
  }
  int failed = 0;
  for (const auto& file : files) {
    Result r = RoundtripFile(file, options);
    std::printf("%s %s: %s\n", r.ok ? "[ ok ]" : "[FAIL]", Display(file).c_str(), r.detail.c_str());
    if (!r.ok) ++failed;
  }
  std::printf("\n%zu files, %d failed\n", files.size(), failed);
  return failed == 0 ? 0 : 1;
}

}  // namespace fwtool
