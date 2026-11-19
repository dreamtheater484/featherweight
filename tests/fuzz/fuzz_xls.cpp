// Fuzz target: the Excel 97-2003 reader (src/sheet/xls, S4). Whatever .xlsx it makes must be one
// the package reader opens, fwtool check finds nothing wrong with, and the workbook model reads with
// every sheet: the converter writes only what it has made sure of.
// Seeds: tests/fuzz/seeds/xls (Excel's .xls copies of small public workbooks).
//
// An input is a compound file, or the Workbook stream itself (anything that isn't a compound file),
// so that mutations reach the records directly.
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
#include "sheet/xls/Xls.h"
#include "sheet/xlsx/Xlsx.h"

using namespace fw;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::span<const uint8_t> input(data, size);
  std::unique_ptr<pkg::CompoundFile> cf;
  if (pkg::CompoundFile::HasSignature(input)) {
    cf = pkg::CompoundFile::Open(input);
    if (!cf) return 0;
  }
  const sheet::xls::StreamFn get = [&](std::string_view name) -> std::optional<std::vector<uint8_t>> {
    if (cf) return cf->Stream(name);
    if (name != "Workbook") return std::nullopt;
    return std::vector<uint8_t>(data, data + size);
  };
  pkg::Status st;
  sheet::xls::NeedsPassword(get);
  std::vector<uint8_t> xlsx = sheet::xls::ToXlsx(get, st);
  if (xlsx.empty()) return 0;
  auto package = pkg::Package::Open(pkg::Snapshot::FromBytes(std::move(xlsx)), pkg::Limits::Default(), st);
  if (!package) {
    std::fprintf(stderr, "the .xlsx made isn't a package: %s\n", st.message.c_str());
    std::abort();
  }
  const pkg::CheckReport report = pkg::CheckPackage(*package);
  if (!report.Ok()) {
    for (const std::string& e : report.errors) std::fprintf(stderr, "check: %s\n", e.c_str());
    std::abort();
  }
  auto book = sheet::xlsx::Read(std::move(package), st, {.loadAll = true});
  if (!book) {
    std::fprintf(stderr, "the .xlsx made doesn't read: %s\n", st.message.c_str());
    std::abort();
  }
  return 0;
}
