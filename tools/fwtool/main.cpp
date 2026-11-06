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
    "  fwtool roundtrip [--model] [--reserialize-all] [--expect-reject] [--out DIR] <file or folder>...\n"
    "      Opens and saves each file without edits. Untouched parts must be byte-identical;\n"
    "      with --reserialize-all every XML part is rewritten through the DOM and must equal\n"
    "      the original after canonicalization. With --expect-reject every file must be\n"
    "      refused with a plain message (malicious files). With --model Word documents are\n"
    "      read into the document model, which writes back every part it owns; those parts\n"
    "      must equal the originals after canonicalization, and the saved copy must pass check.\n"
    "  fwtool save [--edited] [--out DIR] <file or folder>...\n"
    "      Saves each Word document as Featherweight's Save does (the document model's save, then\n"
    "      safe saving) into DIR (default: a temporary folder, removed afterwards). With no edits\n"
    "      the saved file must be identical to the original; with --edited every part the model\n"
    "      owns is rewritten and the document properties updated, as after an edit. Either way the\n"
    "      saved file must open and have no new fwtool check errors. Prints each save's time.\n"
    "  fwtool dump [--model | --layout [--out DIR]] <file>...\n"
    "      Lists parts, relationships and content types; with --model, what the document\n"
    "      model reads from a Word document (stories, paragraphs, labels, fields); with\n"
    "      --layout, the lines on each page Featherweight lays out, or with --out, its layout\n"
    "      dumps (docs/layout-dump.md) in DIR.\n"
    "  fwtool check [--expect-reject] <file or folder>...\n"
    "      Checks each file against the reference registry: every reference resolves, ids\n"
    "      are unique, pairs are complete, and derived data (the calculation chain, chart\n"
    "      caches) matches its sources. With --expect-reject every file must fail (the\n"
    "      broken files).\n"
    "  fwtool render --out DIR [--scale N] [--pictures] <file or folder>...\n"
    "      Draws the pictures and shapes with fwdraw to PNG files in DIR, N pixels per point\n"
    "      (default 2): each Word drawing at its own size, each slide's shapes on a slide-sized\n"
    "      page, each shape in a sheet's drawings. For comparing with the reference renders.\n"
    "      --pictures also times decoding each picture, whole and as typically shown.\n"
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
    "  fwtool bench [--runs N] [--json FILE] [--budgets] [--typing] <file or folder>...\n"
    "      Times opening each Word document, stage by stage, against the opening budgets\n"
    "      (plan §11). With --budgets, a missed budget fails. With --typing, times keystrokes\n"
    "      at the start, middle and end and in a table instead: from the edit to the page painted\n"
    "      again. Formatting there (bold, centered, Heading 1, bullets, each undone) counts too.\n"
    "  fwtool edit-check [--edits N] [--seed S] <file or folder>...\n"
    "      Makes N random edits (40) in each Word document through the editor, lays it out again\n"
    "      incrementally after each, and checks every ten that it's what a layout from scratch is.\n"
    "  fwtool pdf-check [--scale N] [--out DIR] <file or folder>...\n"
    "      Export PDF (D4): lays out each Word document, draws its pages on the screen's renderer\n"
    "      and into a PDF, then checks the PDF's structure strictly, reads it back with PDFium\n"
    "      (pages, text, lines) and compares PDFium's pages with the screen's. --out keeps the PDFs.\n"
    "  fwtool pdf-check --pngs DIR <exported.pdf>\n"
    "      The same for a PDF the app exported (script ops exportpdf: and pagepngs:), against the\n"
    "      pages in DIR; lists its links, bookmarks, title and author.\n"
    "  fwtool paste-check [--samples DIR] [--out DIR] <file or folder>...\n"
    "      Copies each Word document's main story as RTF and HTML and pastes it back (the HTML\n"
    "      over the story, the RTF at its end), and pastes each clipboard sample in --samples at\n"
    "      its start. Every result must save, pass check (when the original does) and undo; with\n"
    "      --out the saved copies go to DIR/html, DIR/rtf and DIR/samples, for fwvalidate.\n"
    "  fwtool export --out DIR <file or folder>...\n"
    "      Saves each Word document as RTF and as plain text (UTF-8) in DIR, as the Word tab's\n"
    "      Save As does, for Word to read back (scripts\\word-open-check.ps1).\n"
    "  fwtool encrypt --password P --out DIR <file or folder>...\n"
    "      Saves each Office file in DIR protected with password P, as Featherweight saves a\n"
    "      protected document (agile encryption), for Word to open.\n"
    "  fwtool decrypt --password P --out DIR <file or folder>...\n"
    "      Opens each password-protected Office file as Featherweight does and saves its package\n"
    "      in DIR; checks that encrypting it again with the key from opening round-trips.\n"
    "  fwtool cfb [--all] <file>...\n"
    "      Reads each compound file (.doc, protected Office files): every stream must read, and\n"
    "      writing them and reading them back must give the same bytes. --all lists the streams,\n"
    "      with small ones in hex.\n"
    "  fwtool bench-app [--runs N] [--json FILE] <Featherweight.exe> [<file>...]\n"
    "      Starts a private copy of Featherweight, alone and with each file, and measures launch\n"
    "      to window, to the first frame (the start screen or the first page), memory once\n"
    "      settled, and CPU and wakeups while idle (plan §11). Works with any build, 0.9.4\n"
    "      included, and never touches the user's settings or a running Featherweight.\n"
    "  fwtool spelling <file or folder>...\n"
    "      Checks each Word document's spelling with Windows' spell checker and counts its words,\n"
    "      as the app does in idle time, and times both (plan §6.2, §11).\n"
    "  fwtool torture [--seed S] [--edits N] [--rounds R] [--out DIR] [--json FILE] <file or folder>...\n"
    "      The open, edit and save test (plan §10, D2): R rounds of N seeded random edits per Word\n"
    "      document through the editor (typing, deletes and replacements across fields, bookmarks,\n"
    "      comments, content controls, tables and tracked changes, splits, joins, run, paragraph and\n"
    "      paragraph-mark formatting, groups of edits as one undo step, undo and redo), then\n"
    "      save: the saved file must open, pass check with no new errors, and read back as the\n"
    "      edited document. A failure is shrunk to the fewest edits that still fail, kept in\n"
    "      DIR/repro (default DIR: build/torture). DIR/saved and DIR/pairs.tsv hold a saved copy of\n"
    "      each document for the validator and Word (scripts/torture.ps1).\n"
    "  fwtool torture --replay <reproducer.txt> [--out DIR]\n"
    "      Applies a reproducer's edits again and reports what happens and which parts the save\n"
    "      wrote. The file it makes goes next to the reproducer as <name>-replay.docx, or into DIR,\n"
    "      with <name>-replay-edited.txt and -readback.txt when the text differs. Its document is\n"
    "      found by its recorded path or, from another checkout, by the tail of that path.\n"
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
    else if (arg == L"--model") options.model = true;
    else if (arg == L"--layout") options.layout = true;
    else if (arg == L"--expect-reject") options.expectReject = true;
    else if (arg == L"--out" && i + 1 < argc) options.outDir = argv[++i];
    else if (arg == L"--password" && i + 1 < argc) options.password = argv[++i];
    else if (arg == L"--printer" && i + 1 < argc) options.printer = argv[++i];
    else if (arg == L"--warp") options.warp = true;
    else if (arg == L"--pictures") options.pictures = true;
    else if (arg == L"--scale" && i + 1 < argc) options.scale = std::clamp(static_cast<float>(_wtof(argv[++i])), 0.1f, 16.f);
    else if (arg == L"--dumps" && i + 1 < argc) options.dumps = argv[++i];
    else if (arg == L"--json" && i + 1 < argc) options.jsonOut = argv[++i];
    else if (arg == L"--report" && i + 1 < argc) options.reportOut = argv[++i];
    else if (arg == L"--min-pages" && i + 1 < argc) options.minPages = _wtof(argv[++i]);
    else if (arg == L"--min-lines" && i + 1 < argc) options.minLines = _wtof(argv[++i]);
    else if (arg == L"--self-test") options.selfTest = true;
    else if (arg == L"--all") options.all = true;
    else if (arg == L"--runs" && i + 1 < argc) options.runs = _wtoi(argv[++i]);
    else if (arg == L"--budgets") options.budgets = true;
    else if (arg == L"--typing") options.typing = true;
    else if (arg == L"--edits" && i + 1 < argc) options.edits = std::max(1, _wtoi(argv[++i]));
    else if (arg == L"--seed" && i + 1 < argc) options.seed = static_cast<unsigned>(std::wcstoul(argv[++i], nullptr, 10));
    else if (arg == L"--rounds" && i + 1 < argc) options.rounds = static_cast<uint32_t>(std::max(1, _wtoi(argv[++i])));
    else if (arg == L"--replay") options.replay = true;
    else if (arg == L"--edited") options.edited = true;
    else if (arg == L"--samples" && i + 1 < argc) options.samples = argv[++i];
    else if (arg == L"--pngs" && i + 1 < argc) options.pngs = argv[++i];
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
  if (command == L"save") return fwtool::RunSave(options);
  if (command == L"check") return fwtool::RunCheck(options);
  if (command == L"print-sample") return fwtool::RunPrintSample(options);
  if (command == L"render") return fwtool::RunRender(options);
  if (command == L"lines") return fwtool::RunLines(options);
  if (command == L"compare") return fwtool::RunCompare(options);
  if (command == L"bench") return fwtool::RunBench(options);
  if (command == L"bench-app") return fwtool::RunBenchApp(options);
  if (command == L"edit-check") return fwtool::RunEditCheck(options);
  if (command == L"spelling") return fwtool::RunSpelling(options);
  if (command == L"torture") return fwtool::RunTorture(options);
  if (command == L"paste-check") return fwtool::RunPasteCheck(options);
  if (command == L"export") return fwtool::RunExport(options);
  if (command == L"encrypt") return fwtool::RunCrypt(options, true);
  if (command == L"decrypt") return fwtool::RunCrypt(options, false);
  if (command == L"cfb") return fwtool::RunCfb(options);
  if (command == L"pdf-check") return fwtool::RunPdfCheck(options);
  std::fprintf(stderr, "Unknown command %s\n\n%s", fw::WideToUtf8(command).c_str(), fwtool::kUsage);
  return 2;
}
