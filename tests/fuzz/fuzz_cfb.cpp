// Fuzz target: the compound file reader (.doc files, password-protected Office files), and writing
// back what it read.
// Seeds: tests/fuzz/seeds/cfb.
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "pkg/CompoundFile.h"

using namespace fw::pkg;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto cf = CompoundFile::Open(std::span(data, size));
  if (!cf) return 0;
  std::vector<std::vector<uint8_t>> kept;
  std::vector<CompoundStream> streams;
  for (const auto& path : cf->Streams()) {
    auto s = cf->Stream(path);
    if (!s) continue;
    kept.push_back(std::move(*s));
    streams.push_back({path, {}});
  }
  for (size_t i = 0; i < kept.size(); ++i) streams[i].data = kept[i];
  // What it read, written and read again, gives the same streams (names that differ only in case
  // are one stream to CFB: Stream() gives the first of those, and that's the one written).
  const auto file = WriteCompoundFile(streams);
  auto again = CompoundFile::Open(file);
  if (!again) std::abort();
  for (const auto& s : streams) {
    auto back = again->Stream(s.path);
    auto first = cf->Stream(s.path);
    if (!back || !first || *back != *first) std::abort();
  }
  return 0;
}
