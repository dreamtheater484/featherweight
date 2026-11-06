// Fuzz target: OpenDocument text import (src/doc/odf, D5). Whatever the .odt, the converter either
// refuses it or gives a Word document that opens, passes fwtool check and is read by the model:
// an imported file is never a damaged one.
// Seeds: tests/fuzz/seeds/odt (a small document with every structure the converter reads).
//
// An input is a zip, or parts as text as fuzz_check reads them: each part starts on a line
// "@<part name>", and the rest of the input up to the next such line is its content.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

#include "doc/docx/Docx.h"
#include "doc/odf/Odt.h"
#include "pkg/Check.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/Zip.h"

using namespace fw::doc;
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

// Parts as text, stored in a zip (as in fuzz_check.cpp).
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

void Check(bool ok, const char* what, const std::string& why = {}) {
  if (ok) return;
  std::fprintf(stderr, "%s %s\n", what, why.c_str());
  std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const Limits limits = FuzzLimits();
  std::string_view input(reinterpret_cast<const char*>(data), size);
  std::vector<uint8_t> odt = input.starts_with("PK\x03\x04") ? std::vector<uint8_t>(data, data + size) : ZipFromText(input);

  std::vector<uint8_t> docx;
  if (!odf::OdtToDocx(odt, docx, limits).Ok()) return 0;
  Status st;
  auto pkg = Package::Open(Snapshot::FromBytes(std::move(docx)), limits, st);
  Check(pkg != nullptr, "the converted document doesn't open:", st.message);
  const CheckReport report = CheckPackage(*pkg);
  for (const auto& e : report.errors) std::fprintf(stderr, "check: %s\n", e.c_str());
  Check(report.Ok(), "the converted document fails the check");
  std::unique_ptr<Document> doc = docx::Read(std::move(pkg), st);
  Check(doc != nullptr || st.code == Status::Code::TooLarge, "the model refuses the converted document:", st.message);
  if (!doc) return 0;
  for (uint32_t story = 0; story < doc->Stories().size(); ++story)
    for (uint32_t para : doc->StoryParagraphs(story)) {
      for (const TextRun& r : doc->Runs(para))
        if (r.run) doc->ResolvedRun(para, r.run);
      doc->ResolvedPara(para);
      doc->Label(para);
    }
  return 0;
}
