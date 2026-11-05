// fwtool: Featherweight's headless command line for the office engines (plan §10). Not shipped.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "Common.h"
#include "core/Strings.h"

namespace fwtool {
namespace {

constexpr const char* kUsage =
    "Usage:\n"
    "  fwtool roundtrip [--reserialize-all] [--expect-reject] [--out DIR] <file or folder>...\n"
    "      Opens and saves each file without edits. Untouched parts must be byte-identical;\n"
    "      with --reserialize-all every XML part is rewritten through the DOM and must equal\n"
    "      the original after canonicalization. With --expect-reject every file must be\n"
    "      refused with a plain message (malicious files).\n"
    "  fwtool dump <file>...\n"
    "      Lists parts, relationships and content types.\n"
    "  fwtool check [--expect-reject] <file or folder>...\n"
    "      Checks each file against the reference registry: every reference resolves, ids\n"
    "      are unique, pairs are complete, and derived data (the calculation chain, chart\n"
    "      caches) matches its sources. With --expect-reject every file must fail (the\n"
    "      broken files).\n"
    "  fwtool lines [--out DIR] <reference folder or PDF>...\n"
    "      Word's lines in its reference PDFs: page sizes, and each line's story, position and\n"
    "      text. With --out, writes them as layout dumps (docs/layout-dump.md).\n"
    "  fwtool compare [--dumps DIR] [--json FILE] [--report FILE] [--min-pages P] [--min-lines L]\n"
    "                 <reference folder>...\n"
    "      Layout fidelity (plan §10): for each Word document in the reference manifest, does\n"
    "      Featherweight's page count match Word's, and do its lines break where Word's do? The\n"
    "      layout dumps come from --dumps DIR (DIR/<source>.layout.json). Fails below P % of\n"
    "      page counts or L % of lines.\n"
    "  fwtool compare [--all] <Word's PDF> <layout dump or document>\n"
    "      One document: where its line breaks first diverge from Word's, side by side.\n"
    "  fwtool compare --self-test <reference folder>...\n"
    "      Checks the measurement: Word's lines match themselves, one moved line break costs\n"
    "      exactly one line, and an added page changes only the page count.\n"
    "  fwtool bench [--runs N] [--json FILE] [--budgets] <file or folder>...\n"
    "      Times opening each Word document, stage by stage, against the opening budgets\n"
    "      (plan §11). With --budgets, a missed budget fails.\n"
    "  fwtool bench-app [--runs N] [--json FILE] <Featherweight.exe> [<file>...]\n"
    "      Starts a private copy of Featherweight, alone and with each file, and measures launch\n"
    "      to window, to the first frame (the start screen or the first page), memory once\n"
    "      settled, and CPU and wakeups while idle (plan §11). Works with any build, 0.9.4\n"
    "      included, and never touches the user's settings or a running Featherweight.\n"
    "  fwtool wrap [options] (--file <file> | <text>...)\n"
    "      Lays out paragraphs with fwtext and prints their lines, to compare line breaks with\n"
    "      Word's. Run it without text for its options.\n";

std::vector<std::filesystem::path> CollectSorted(const std::filesystem::path& dir) {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::end(it);
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    std::wstring ext = fw::ToLower(it->path().extension().wstring());
    for (const wchar_t* known : {L".docx", L".docm", L".dotx", L".dotm", L".xlsx", L".xlsm", L".xltx", L".xltm",
                                 L".pptx", L".pptm", L".potx", L".potm", L".ppsx", L".ppsm"})
      if (ext == known) files.push_back(it->path());
  }
  std::sort(files.begin(), files.end());
  return files;
}

}  // namespace

std::vector<std::filesystem::path> CollectFiles(const std::vector<std::filesystem::path>& inputs) {
  std::vector<std::filesystem::path> files;
  for (const auto& in : inputs) {
    if (std::filesystem::is_directory(in)) {
      auto found = CollectSorted(in);
      files.insert(files.end(), found.begin(), found.end());
    } else {
      files.push_back(in);
    }
  }
  return files;
}

std::string Display(const std::filesystem::path& file) {
  std::error_code ec;
  std::filesystem::path rel = file.lexically_proximate(std::filesystem::current_path(ec));
  const std::filesystem::path& shown = rel.native().size() < file.native().size() ? rel : file;
  return fw::WideToUtf8(shown.generic_wstring());
}

std::unique_ptr<fw::pkg::Package> OpenPackage(const std::filesystem::path& file, fw::pkg::Status& status) {
  fw::pkg::Snapshot::Options options;
  std::error_code ec;
  options.recoveryDir = (std::filesystem::temp_directory_path(ec) / L"fwtool-recovery").wstring();
  auto snapshot = fw::pkg::Snapshot::Open(file.wstring(), options, status);
  if (!snapshot) return nullptr;
  return fw::pkg::Package::Open(std::move(snapshot), fw::pkg::Limits::Default(), status);
}

std::string_view AsText(const std::vector<uint8_t>& bytes) {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace fwtool

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  if (argc >= 2 && std::wstring(argv[1]) == L"wrap") return fwtool::RunWrap(argc, argv);
  if (argc < 3) {
    std::fputs(fwtool::kUsage, stderr);
    return 2;
  }
  std::wstring command = argv[1];
  fwtool::Options options;
  for (int i = 2; i < argc; ++i) {
    std::wstring arg = argv[i];
    if (arg == L"--reserialize-all") options.reserializeAll = true;
    else if (arg == L"--expect-reject") options.expectReject = true;
    else if (arg == L"--out" && i + 1 < argc) options.outDir = argv[++i];
    else if (arg == L"--printer" && i + 1 < argc) options.printer = argv[++i];
    else if (arg == L"--warp") options.warp = true;
    else if (arg == L"--dumps" && i + 1 < argc) options.dumps = argv[++i];
    else if (arg == L"--json" && i + 1 < argc) options.jsonOut = argv[++i];
    else if (arg == L"--report" && i + 1 < argc) options.reportOut = argv[++i];
    else if (arg == L"--min-pages" && i + 1 < argc) options.minPages = _wtof(argv[++i]);
    else if (arg == L"--min-lines" && i + 1 < argc) options.minLines = _wtof(argv[++i]);
    else if (arg == L"--self-test") options.selfTest = true;
    else if (arg == L"--all") options.all = true;
    else if (arg == L"--runs" && i + 1 < argc) options.runs = _wtoi(argv[++i]);
    else if (arg == L"--budgets") options.budgets = true;
    else if (arg.starts_with(L"--")) {
      std::fprintf(stderr, "Unknown option %s\n\n%s", fw::WideToUtf8(arg).c_str(), fwtool::kUsage);
      return 2;
    } else options.inputs.emplace_back(arg);
  }
  if (options.inputs.empty()) {
    std::fputs(fwtool::kUsage, stderr);
    return 2;
  }
  if (command == L"roundtrip") return fwtool::RunRoundtrip(options);
  if (command == L"dump") return fwtool::RunDump(options);
  if (command == L"check") return fwtool::RunCheck(options);
  if (command == L"print-sample") return fwtool::RunPrintSample(options);
  if (command == L"lines") return fwtool::RunLines(options);
  if (command == L"compare") return fwtool::RunCompare(options);
  if (command == L"bench") return fwtool::RunBench(options);
  if (command == L"bench-app") return fwtool::RunBenchApp(options);
  std::fprintf(stderr, "Unknown command %s\n\n%s", fw::WideToUtf8(command).c_str(), fwtool::kUsage);
  return 2;
}
