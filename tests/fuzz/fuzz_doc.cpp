// Fuzz target: the Word 97-2003 reader (src/doc/msdoc). Whatever .docx it makes must be one the
// docx reader takes and fwtool check finds nothing wrong with: the reader writes only what it has
// made sure of.
// Seeds: tests/fuzz/seeds/doc (Word's .doc copies of the corpus).
//
// An input is a compound file, or its streams one after another, each a byte naming it (0
// WordDocument, 1 1Table, 2 0Table, 3 Data), a 32-bit length and the bytes, so that mutations
// reach the FIB and the tables directly.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "doc/docx/Docx.h"
#include "doc/msdoc/MsDoc.h"
#include "pkg/Check.h"
#include "pkg/CompoundFile.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"

using namespace fw;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::span<const uint8_t> input(data, size);
  std::unique_ptr<pkg::CompoundFile> cf;
  std::map<std::string, std::vector<uint8_t>> streams;
  if (pkg::CompoundFile::HasSignature(input)) {
    cf = pkg::CompoundFile::Open(input);
    if (!cf) return 0;
  } else {
    static const char* const kNames[] = {"WordDocument", "1Table", "0Table", "Data"};
    for (size_t o = 0; o + 5 <= size;) {
      const uint8_t which = data[o];
      const size_t n = std::min<size_t>(data[o + 1] | data[o + 2] << 8 | data[o + 3] << 16 | size_t{data[o + 4]} << 24,
                                        size - o - 5);
      if (which < 4) streams[kNames[which]].assign(data + o + 5, data + o + 5 + n);
      o += 5 + n;
    }
  }
  const doc::msdoc::StreamFn get = [&](std::string_view name) -> std::optional<std::vector<uint8_t>> {
    if (cf) return cf->Stream(name);
    auto it = streams.find(std::string(name));
    if (it == streams.end()) return std::nullopt;
    return it->second;
  };
  pkg::Status st;
  std::vector<uint8_t> docx = doc::msdoc::ToDocx(get, st);
  if (docx.empty()) return 0;
  auto package = pkg::Package::Open(pkg::Snapshot::FromBytes(std::move(docx)), pkg::Limits::Default(), st);
  if (!package) {
    std::fprintf(stderr, "the .docx made isn't a package: %s\n", st.message.c_str());
    std::abort();
  }
  const pkg::CheckReport report = pkg::CheckPackage(*package);
  if (!report.Ok()) {
    for (const std::string& e : report.errors) std::fprintf(stderr, "check: %s\n", e.c_str());
    std::abort();
  }
  if (!doc::docx::Read(std::move(package), st)) {
    std::fprintf(stderr, "the .docx made doesn't read: %s\n", st.message.c_str());
    std::abort();
  }
  return 0;
}
