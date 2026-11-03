#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "pkg/Package.h"

namespace fwtool {

struct Options {
  bool reserializeAll = false;  // roundtrip: write every XML part through the DOM
  bool expectReject = false;    // roundtrip: every file must be refused with a plain message
  std::filesystem::path outDir; // roundtrip: keep the saved copies here
  std::wstring printer;         // print-sample: the printer (default: Microsoft Print to PDF)
  bool warp = false;            // print-sample: draw with WARP instead of the graphics adapter
  std::vector<std::filesystem::path> inputs;
};

// Office files under the inputs (folders are searched recursively), in a stable order.
std::vector<std::filesystem::path> CollectFiles(const std::vector<std::filesystem::path>& inputs);
// How a file is named in output: relative to the current folder when that is shorter.
std::string Display(const std::filesystem::path& file);
// Takes a snapshot and opens the package, as the app does.
std::unique_ptr<fw::pkg::Package> OpenPackage(const std::filesystem::path& file, fw::pkg::Status& status);
std::string_view AsText(const std::vector<uint8_t>& bytes);

int RunRoundtrip(const Options& options);
int RunDump(const Options& options);
int RunCheck(const Options& options);
int RunPrintSample(const Options& options);

}  // namespace fwtool
