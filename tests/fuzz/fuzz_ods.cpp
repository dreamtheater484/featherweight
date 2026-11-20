// Fuzz target: OpenDocument spreadsheet import (src/sheet/ods, S4). Whatever the .ods, the converter
// either refuses it or gives a workbook that opens, passes fwtool check, is read by the model with
// every sheet, and whose sheets the model writes back: an imported file is never a damaged one.
// Seeds: tests/fuzz/seeds/ods (a small spreadsheet with every structure the converter reads).
//
// An input is a zip, or parts as text as fuzz_check reads them: each part starts on a line
// "@<part name>", and the rest of the input up to the next such line is its content.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pkg/Check.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/Zip.h"
#include "sheet/ods/Ods.h"
#include "sheet/xlsx/Xlsx.h"

using namespace fw::pkg;
using namespace fw::sheet;

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
  std::vector<uint8_t> ods = input.starts_with("PK\x03\x04") ? std::vector<uint8_t>(data, data + size) : ZipFromText(input);

  // Formulas on their own: any text translates or is refused, never crashes.
  if (size < 4096) {
    std::string out;
    ods::TranslateFormula(input, out, static_cast<int>(size % 7) - 3, static_cast<int>(size % 5) - 2);
    ods::TranslateAddress(input, out);
  }

  std::vector<uint8_t> bytes;
  if (!ods::OdsToXlsx(ods, bytes, limits).Ok()) return 0;
  Status st;
  auto pkg = Package::Open(Snapshot::FromBytes(std::move(bytes)), limits, st);
  Check(pkg != nullptr, "the converted workbook doesn't open:", st.message);
  const CheckReport report = CheckPackage(*pkg);
  for (const auto& e : report.errors) std::fprintf(stderr, "check: %s\n", e.c_str());
  Check(report.Ok(), "the converted workbook fails the check");
  std::unique_ptr<Workbook> book = xlsx::Read(std::move(pkg), st, {.loadAll = true});
  Check(book != nullptr || st.code == Status::Code::TooLarge, "the model refuses the converted workbook:", st.message);
  if (!book) return 0;
  for (size_t i = 0; i < book->SheetCount(); ++i) {
    Sheet& s = book->GetSheet(i);
    size_t cells = 0;
    s.ForEachCell(Range::All(), [&](const CellInfo&) { return ++cells > 0; });
    std::string out;
    if (!s.Text().rootStart.empty()) Check(xlsx::WritePart(*book, s.PartName(), out).Ok(), "a sheet isn't written back");
  }
  return 0;
}
