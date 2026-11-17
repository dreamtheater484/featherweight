#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "pkg/Package.h"

namespace fwtool {

struct Options {
  bool reserializeAll = false;  // roundtrip: write every XML part through the DOM
  bool model = false;           // roundtrip: read Word documents and workbooks into their models and write their parts
  bool workbooks = false;       // roundtrip, check, dump: workbooks only
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
  bool cells = false;               // compare: workbooks' displayed cell text against Excel's cells files
  bool values = false;              // csv: every field must show its text unchanged
  bool resave = false;              // csv: saved untouched, the same bytes
  double minCells = -1;             // compare --cells: fail below this share of cells showing Excel's text (%)
  double minLines = -1;             // compare: fail below this share of matching line breaks (%)
  bool selfTest = false;            // compare: Word's lines against themselves and known changes
  bool all = false;                 // compare (one document): list every difference
  int runs = 5;                     // bench: runs per document
  bool budgets = false;             // bench: fail when a budget is missed
  bool typing = false;              // bench: time keystrokes instead of opening
  bool scrolling = false;           // bench: time painting every page in order, and memory
  int edits = 40;                   // edit-check, torture: random edits per document (torture: per round)
  unsigned seed = 0;                // edit-check, torture: the random edits' seed (0: edit-check takes the
                                    // document's number, torture 1; torture derives one per file and round)
  bool layout = false;              // dump: the pages Featherweight lays out
  bool edited = false;              // save: as after an edit in every part the model owns
  uint32_t rounds = 3;              // torture: rounds per document, each from the original
  bool replay = false;              // torture: the input is a reproducer to replay
  std::string ops;                  // torture (workbooks): only these edits, comma-separated ("value,formula,undo")
  std::filesystem::path samples;    // paste-check: clipboard samples (.rtf, .html) to paste
  std::filesystem::path pngs;       // pdf-check: the pages the app drew (page-1.png, ...)
  std::wstring locale;              // formulas: the region the files were calculated in (en-US, en-NL)
  std::vector<std::wstring> regions;  // formulas: other regions to try a workbook with mismatches in
  double minMatch = -1;             // formulas: fail below this share of compared formulas matching (%)
  std::filesystem::path excel;      // formulas: compare with Excel's values in the cells files under it
  std::wstring password;            // encrypt, decrypt
  std::vector<std::filesystem::path> inputs;
};

// Office files under the inputs (folders are searched recursively), in a stable order.
std::vector<std::filesystem::path> CollectFiles(const std::vector<std::filesystem::path>& inputs);
// How a file is named in output: relative to the current folder when that is shorter.
std::string Display(const std::filesystem::path& file);
// Takes a snapshot and opens the package, as the app does.
std::unique_ptr<fw::pkg::Package> OpenPackage(const std::filesystem::path& file, fw::pkg::Status& status);
std::string_view AsText(const std::vector<uint8_t>& bytes);

// Workbooks (Workbook.cpp).
}  // namespace fwtool
namespace fw::sheet {
class Workbook;
struct CellInfo;
struct NumLocale;
}
namespace fwtool {
// What a cell shows with no column width: its value through its number format, in `locale`, as
// Range.Text gives it (CellCompare.cpp). Keeps the parsed formats between calls.
class CellText {
 public:
  CellText(const fw::sheet::Workbook& book, const fw::sheet::NumLocale& locale);
  ~CellText();
  std::string operator()(const fw::sheet::CellInfo& cell);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
bool IsWorkbookFile(const std::filesystem::path& file);
std::unique_ptr<fw::sheet::Workbook> ReadWorkbook(const std::filesystem::path& file, fw::pkg::Status& status, bool loadAll);
// What the model finds wrong with a workbook it read (cells naming formats or strings that don't exist).
std::vector<std::string> CheckWorkbookModel(const fw::sheet::Workbook& book);
void DumpWorkbook(const std::filesystem::path& file);
// The inputs' files, only workbooks with --workbooks.
std::vector<std::filesystem::path> CollectFiles(const Options& options);

int RunRoundtrip(const Options& options);
int RunSave(const Options& options);
void DumpModel(const std::filesystem::path& file);
int RunDump(const Options& options);
int RunFormulas(const Options& options);  // formulas: recalculate and compare with the cached values
int RunCheck(const Options& options);
int RunOdt(const Options& options);
int RunPrintSample(const Options& options);
int RunRender(const Options& options);
int RunLines(const Options& options);
int RunCompare(const Options& options);
int RunCellCompare(const Options& options);
int RunBench(const Options& options);
int RunEditCheck(const Options& options);
int RunPasteCheck(const Options& options);
int RunExport(const Options& options);
int RunCrypt(const Options& options, bool encrypt);
int RunCfb(const Options& options);
int RunCsv(const Options& options);
int RunPdfCheck(const Options& options);
int RunBenchApp(const Options& options);
int RunSpelling(const Options& options);
int RunTorture(const Options& options);
// Parses its own options, from argv[2] on.
int RunWrap(int argc, wchar_t** argv);

}  // namespace fwtool
