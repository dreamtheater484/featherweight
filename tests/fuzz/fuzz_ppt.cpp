// Fuzz target: the PowerPoint 97-2003 reader (src/slide/ppt, P3). Whatever .pptx it makes must be
// one the package reader opens, fwtool check finds nothing wrong with, and the slide model reads
// with every slide, its text resolved: the converter writes only what it has made sure of.
// Seeds: tests/fuzz/seeds/ppt (a presentation built record by record: FW_PPT_SEED=<file> fw_tests
// writes it; PowerPoint's own .ppt copies when p3-corpus has them).
//
// An input is a compound file, or the "PowerPoint Document" stream itself (anything that isn't a
// compound file, read without a "Current User" stream), so that mutations reach the records directly.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pkg/Check.h"
#include "pkg/CompoundFile.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "slide/Presentation.h"
#include "slide/ppt/Ppt.h"

using namespace fw;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::span<const uint8_t> input(data, size);
  std::unique_ptr<pkg::CompoundFile> cf;
  if (pkg::CompoundFile::HasSignature(input)) {
    cf = pkg::CompoundFile::Open(input);
    if (!cf) return 0;
  }
  const slide::ppt::StreamFn get = [&](std::string_view name) -> std::optional<std::vector<uint8_t>> {
    if (cf) return cf->Stream(name);
    if (name != "PowerPoint Document") return std::nullopt;
    return std::vector<uint8_t>(data, data + size);
  };
  pkg::Status st;
  slide::ppt::NeedsPassword(get);
  std::vector<uint8_t> pptx = slide::ppt::ToPptx(get, st);
  if (pptx.empty()) return 0;
  auto package = pkg::Package::Open(pkg::Snapshot::FromBytes(std::move(pptx)), pkg::Limits::Default(), st);
  if (!package) {
    std::fprintf(stderr, "the .pptx made isn't a package: %s\n", st.message.c_str());
    std::abort();
  }
  const pkg::CheckReport report = pkg::CheckPackage(*package);
  if (!report.Ok()) {
    for (const std::string& e : report.errors) std::fprintf(stderr, "check: %s\n", e.c_str());
    std::abort();
  }
  auto deck = slide::Read(std::move(package), st);
  if (!deck) {
    std::fprintf(stderr, "the .pptx made doesn't read: %s\n", st.message.c_str());
    std::abort();
  }
  deck->LoadAll();
  for (size_t i = 0; i < deck->SlideCount(); ++i)
    if (const slide::Slide* s = deck->GetSlide(i))
      for (const slide::Shape& shape : s->shapes) deck->Text(shape, *s);
  return 0;
}
