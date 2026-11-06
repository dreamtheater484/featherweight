// Fuzz target: the zip reader and writer, and the package on top of them.
// Seeds: tests/office (every file).
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/Zip.h"

using namespace fw::pkg;

namespace {

// Lower than the defaults, so the fuzzer explores rather than allocating gigabytes.
Limits FuzzLimits() {
  Limits limits;
  limits.maxEntrySize = 16u << 20;
  limits.maxTotalSize = 64u << 20;
  limits.maxEntries = 4096;
  return limits;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const Limits limits = FuzzLimits();
  std::vector<uint8_t> bytes(data, data + size);
  {
    auto snapshot = Snapshot::FromBytes(bytes);
    ZipReader reader;
    if (reader.Open(*snapshot, limits).Ok()) {
      std::vector<uint8_t> out;
      for (const auto& e : reader.Entries()) reader.Read(e, out);
    }
  }
  Status st;
  auto pkg = Package::Open(Snapshot::FromBytes(std::move(bytes)), limits, st);
  if (!pkg) return 0;
  pkg->Verify();
  // Saving without edits must give a package that opens to the same entries, copied raw.
  MemorySink sink;
  if (!pkg->Save(sink).Ok()) std::abort();
  auto again = Package::Open(Snapshot::FromBytes(std::move(sink.bytes)), limits, st);
  if (!again || again->Zip().Entries().size() != pkg->Zip().Entries().size()) std::abort();
  std::vector<uint8_t> a, b;
  for (size_t i = 0; i < pkg->Zip().Entries().size(); ++i) {
    if (!pkg->Zip().ReadRecord(pkg->Zip().Entries()[i], a).Ok() ||
        !again->Zip().ReadRecord(again->Zip().Entries()[i], b).Ok() || a != b)
      std::abort();
  }
  return 0;
}
