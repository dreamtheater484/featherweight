#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "pkg/Package.h"

namespace fwtool {

struct Options {
  bool reserializeAll = false;  // roundtrip: write every XML part through the DOM
  bool model = false;           // roundtrip: read Word documents into the model and write its parts from it
  bool expectReject = false;    // roundtrip: every file must be refused; check: every file must fail
  std::filesystem::path outDir; // roundtrip: keep the saved copies here
  std::wstring printer;         // print-sample: the printer (default: Microsoft Print to PDF)
  bool warp = false;            // print-sample: draw with WARP instead of the graphics adapter
  float scale = 2;              // render: pixels per point
  bool pictures = false;        // render: time decoding each picture
  std::filesystem::path dumps;      // compare: Featherweight's layout dumps, <dumps>/<source>.layout.json
  std::filesystem::path jsonOut;    // compare, bench: the JSON report
  std::filesystem::path reportOut;  // compare: the Markdown report
  double minPages = -1;             // compare: fail below this share of matching page counts (%)
  double minLines = -1;             // compare: fail below this share of matching line breaks (%)
  bool selfTest = false;            // compare: Word's lines against themselves and known changes
  bool all = false;                 // compare (one document): list every difference
  int runs = 5;                     // bench: runs per document
  bool budgets = false;             // bench: fail when a budget is missed
  bool typing = false;              // bench: time keystrokes instead of opening
  int edits = 40;                   // edit-check, torture: random edits per document (torture: per round)
  unsigned seed = 0;                // edit-check, torture: the random edits' seed (0: edit-check takes the
                                    // document's number, torture 1; torture derives one per file and round)
  bool layout = false;              // dump: the pages Featherweight lays out
  bool edited = false;              // save: as after an edit in every part the model owns
  uint32_t rounds = 3;              // torture: rounds per document, each from the original
  bool replay = false;              // torture: the input is a reproducer to replay
  std::filesystem::path samples;    // paste-check: clipboard samples (.rtf, .html) to paste
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
int RunSave(const Options& options);
void DumpModel(const std::filesystem::path& file);
int RunDump(const Options& options);
int RunCheck(const Options& options);
int RunPrintSample(const Options& options);
int RunRender(const Options& options);
int RunLines(const Options& options);
int RunCompare(const Options& options);
int RunBench(const Options& options);
int RunEditCheck(const Options& options);
int RunPasteCheck(const Options& options);
int RunExport(const Options& options);
int RunBenchApp(const Options& options);
int RunSpelling(const Options& options);
int RunTorture(const Options& options);
// Parses its own options, from argv[2] on.
int RunWrap(int argc, wchar_t** argv);

}  // namespace fwtool
