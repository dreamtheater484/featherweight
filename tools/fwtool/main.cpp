// fwtool: Featherweight's headless command line for the office engines (plan §10). Not shipped.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "Common.h"
#include "chart/Chart.h"
#include "draw/FontSubstitutes.h"
#include "core/Strings.h"
#include "doc/msdoc/MsDoc.h"
#include "pkg/CompoundFile.h"
#include "sheet/ods/Ods.h"
#include "sheet/view/Grid.h"
#include "text/Fonts.h"
#include "sheet/xls/Xls.h"
#include "slide/odp/Odp.h"
#include "slide/ppt/Ppt.h"

namespace fwtool {
namespace {

constexpr const char* kUsage =
    "Usage:\n"
    "  fwtool odt [--out DIR] <file or folder>...\n"
    "      Converts OpenDocument text files (.odt, .ott) and Word templates (.dotx, .dotm, to a new\n"
    "      document) as the app opens them, checks the .docx and reads it into the model; with\n"
    "      --out the converted documents are kept there.\n"
    "  fwtool roundtrip [--model] [--workbooks] [--reserialize-all] [--expect-reject] [--out DIR] <file or folder>...\n"
    "      Opens and saves each file without edits. Untouched parts must be byte-identical;\n"
    "      with --reserialize-all every XML part is rewritten through the DOM and must equal\n"
    "      the original after canonicalization. With --expect-reject every file must be\n"
    "      refused with a plain message (malicious files). With --model Word documents are\n"
    "      read into the document model, and workbooks into the workbook model (every sheet), which\n"
    "      write back every part they own; those parts must equal the originals after\n"
    "      canonicalization, and the saved copy must pass check. --workbooks: workbooks only.\n"
    "  fwtool save [--edited] [--out DIR] <file or folder>...\n"
    "      Saves each Word document as Featherweight's Save does (the document model's save, then\n"
    "      safe saving) into DIR (default: a temporary folder, removed afterwards). With no edits\n"
    "      the saved file must be identical to the original; with --edited every part the model\n"
    "      owns is rewritten and the document properties updated, as after an edit. Either way the\n"
    "      saved file must open and have no new fwtool check errors. Prints each save's time.\n"
    "  fwtool dump [--model [--workbooks] | --layout [--out DIR]] <file>...\n"
    "      Lists parts, relationships and content types; with --model, what the document\n"
    "      model reads from a Word document (stories, paragraphs, labels, fields) or the workbook\n"
    "      model from a workbook (sheets, names, the first cells and their formulas); with\n"
    "      --layout, the lines on each page Featherweight lays out, or with --out, its layout\n"
    "      dumps (docs/layout-dump.md) in DIR.\n"
    "  fwtool check [--expect-reject] [--model] [--workbooks] <file or folder>...\n"
    "      Checks each file against the reference registry: every reference resolves, ids\n"
    "      are unique, pairs are complete, and derived data (the calculation chain, chart\n"
    "      caches) matches its sources. With --expect-reject every file must fail (the\n"
    "      broken files). With --model each workbook is also read into the workbook model, every\n"
    "      sheet, and its cells must name formats and strings the workbook has.\n"
    "  fwtool render --out DIR [--scale N] [--pictures] <file or folder>...\n"
    "      Draws the pictures and shapes with fwdraw to PNG files in DIR, N pixels per point\n"
    "      (default 2): each Word drawing at its own size, each slide's shapes on a slide-sized\n"
    "      page, each shape in a sheet's drawings. For comparing with the reference renders.\n"
    "      --pictures also times decoding each picture, whole and as typically shown.\n"
    "  fwtool lines [--out DIR] <reference folder or PDF>...\n"
    "      Word's lines in its reference PDFs: page sizes, and each line's story, position and\n"
    "      text. With --out, writes them as layout dumps (docs/layout-dump.md).\n"
    "  fwtool formulas --threads [--locale en-NL] <file or folder>...\n"
    "      S4: each workbook recalculated in full on one thread and on four (the parallel phase\n"
    "      forced however few formulas it has; NOW and RAND fixed); fails when a value differs.\n"
    "  fwtool formulas [--locale en-NL] [--regions R,...] [--report FILE] [--min-match M]\n"
    "                  <workbooks or folders>...\n"
    "      Workbooks (S1): recalculates every formula and compares it with the value Excel saved\n"
    "      (volatile ones and their dependents aside), in the region Excel calculated in. With\n"
    "      --regions, a workbook with mismatches counts in the listed region where most of its\n"
    "      formulas match (TEXT() codes follow the saving Excel's region); the report says which.\n"
    "      --excel <reference folder>: compares instead with the values this PC's Excel gave each\n"
    "      formula after opening and after a full recalculation (the cells files under it), in\n"
    "      the region it recorded: what the app recalculates on opening, and everything.\n"
    "  fwtool compare --cells [--json FILE] [--min-cells C] [--folders xls,ods] <reference folder>\n"
    "      Workbooks (S1): each visible cell's text as Featherweight formats it against Excel's\n"
    "      (the cells files beside the reference PDFs), in the locale Excel recorded. Differences\n"
    "      that only the column width decides (\"####\", General shortened) are counted apart.\n"
    "      --folders: only the workbooks in those top folders of the manifest (S4: Excel's .xls\n"
    "      and .ods copies of the corpus are xls/ and ods/).\n"
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
    "  fwtool bench --sheet-edits [--runs N] [--out DIR] [--json FILE] [--budgets] <template.xlsx>\n"
    "      S2's editing budgets on workbooks made from the template in DIR: recalculation after\n"
    "      one edit (100,000 formulas), inserting a row and undoing it, and saving (1,000,000 cells).\n"
    "      S3's: a table, a conditional format and a chart from a 1,000-row selection to screen,\n"
    "      and the chart drawn again after an edit to its data.\n"
    "  fwtool bench --decks [--runs N] [--json FILE] [--budgets] <deck>...\n"
    "      P2's editing budgets on each deck (deck-50.pptx), through the edit API, from the edit to the\n"
    "      slide drawn again: a keystroke in a text box (< 16 ms typical, < 50 ms worst), new, duplicate,\n"
    "      delete and move slide (< 50 ms each), undo of each (< 50 ms); saving timed for the record.\n"
    "      With --budgets a missed budget fails; without the edit API in the build it always fails.\n"
    "  fwtool bench --slide-text [--runs N] [--json FILE] [--budgets] <deck>...\n"
    "      Typing in a slide's text as the presentation tab does (p2-text: the working copy, the body laid\n"
    "      out, the edit committed, the text drawn over the slide's bitmap): < 16 ms typical, < 50 ms worst.\n"
    "  fwtool bench [--runs N] [--json FILE] [--budgets] [--typing | --scrolling] <file or folder>...\n"
    "      Times opening each Word document, stage by stage, against the opening budgets\n"
    "      (plan §11). With --budgets, a missed budget fails. With --typing, times keystrokes\n"
    "      at the start, middle and end and in a table instead: from the edit to the page painted\n"
    "      again. Formatting there (bold, centered, Heading 1, bullets, each undone) counts too.\n"
    "      With --scrolling, paints every page in order, as scrolling to the end shows them, and\n"
    "      reports each page's paint time and the memory laid out and scrolled (huge documents).\n"
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
    "  fwtool pdf-valid [--json FILE] [--out DIR] [--samples LIST] [--password P] <pdf or folder>...\n"
    "      Any PDF as a reader meets it (F1, f1-gate: the PDF tools' outputs): its structure without\n"
    "      PDFium (every cross-reference section, tables and streams and the /Prev chain, each object\n"
    "      where its entry says, each stream's /Length, /Root a catalog), then PDFium (opens, every page\n"
    "      loads with a size and text, and renders), the fonts it embeds, and each signature's byte\n"
    "      range and whether it covers the whole file. LIST names more PDFs, one per line. --out\n"
    "      writes each PDF's text, as PDFium reads it, to DIR\\<name>.txt (pages split by form feeds).\n"
    "  fwtool pdf-torture [--seed S] [--edits N] [--rounds R] [--out DIR] [--json FILE] [--password P,Q]\n"
    "                     [--pfx FILE --pfx-password P] [--samples LIST] <pdf or folder>...\n"
    "      The page tools' never-damage test (F1, f1-gate): R rounds of N seeded random operations per PDF\n"
    "      (rotate, delete, move, insert another input, a drawn signature, OCR, extract and split into new\n"
    "      files), through f1-pdf's tools when built with them, saved as the app saves, opened again and\n"
    "      checked page by page (text, size, rotation, annotations) and by pdf-valid's structure check.\n"
    "      PDFs the tools must refuse (XFA, protected): every tool refuses, a save is the file as it was.\n"
    "      --pfx: every third round signed with it (in memory only), saved, changed and saved again; the\n"
    "      signed revision must stay intact (DIR/signed, with signatures.json for pyHanko).\n"
    "      DIR/saved, DIR/extracted, DIR/refused, DIR/signed, DIR/repro.\n"
#ifdef FW_HAVE_PDFTOOLS
    "  fwtool pdf-ocr --out DIR [--password P,Q] <pdf or folder>...\n"
    "      Recognize text on every scanned page, as the PDF tab does (f1-pdf's pdftools::Editor, Windows'\n"
    "      OCR), each PDF saved into DIR under its own name (F1, f1-gate's OCR accuracy run).\n"
#endif
    "  fwtool pdf-compare [--out DIR] [--json FILE] [--min-ssim S] <reference.pdf> <ours.pdf>\n"
    "      Pixels (S3, plan §10): both PDFs rendered at 100 dpi, page origins aligned, each page compared\n"
    "      by SSIM; a page passes at >= 0.95, or at >= S (P2's save without edits: 0.99). --out writes\n"
    "      failing pages side by side with the difference.\n"
    "  fwtool pdf-compare --pngs DIR [--out DIR] [--json FILE] <reference.pdf>\n"
    "      The same against the pages the app drew, DIR\\page-1.png on (P1's slides: the script op\n"
    "      pagepngs:DIR|100 draws every slide, hidden ones too, as the screen does).\n"
    "  fwtool slides --out DIR [--png] [--scale N] <deck or folder>...\n"
    "      Decks (P1): draws every slide with the slide renderer into DIR/<deck>.pdf (for pdf-compare\n"
    "      against PowerPoint's PDF), and with --png each slide as a PNG; times the first slide.\n"
    "  fwtool timing <deck or folder>...\n"
    "      Decks (P3): each slide's transition and main animation sequence as the edit layer reads them\n"
    "      (effect, options, trigger, duration, delay; \"kept\" for what Featherweight can't change).\n"
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
    "  fwtool csv [--values] [--resave] [--locale en-US|en-NL] <file or folder>...\n"
    "      Opens each CSV or TSV file as Featherweight does: what it detected, and how long the\n"
    "      first screen (100 rows) and the whole file took (budget: 1 s and 5 s for 100 MB).\n"
    "      --values: every field must show exactly its text (no value changes), in the locale\n"
    "      (default: this PC's region, as the app reads it).\n"
    "      --resave: saved untouched, each file must be the same bytes.\n"
    "  fwtool cfb [--all] <file>...\n"
    "      Reads each compound file (.doc, protected Office files): every stream must read, and\n"
    "      writing them and reading them back must give the same bytes. --all lists the streams,\n"
    "      with small ones in hex.\n"
    "  fwtool bench-app [--runs N] [--json FILE] [--idle-threads] [--modules] [--languages en,de] <Featherweight.exe> [<file>...]\n"
    "      Starts a private copy of Featherweight, alone and with each file, and measures launch\n"
    "      to window, to the first frame (the start screen or the first page), memory once\n"
    "      settled, and CPU and wakeups while idle (plan §11). Works with any build, 0.9.4\n"
    "      included, and never touches the user's settings or a running Featherweight.\n"
    "      --modules lists the DLLs loaded once launched and settled (F1: nothing before it's used).\n"
    "      --languages times launch to window in each interface language, runs interleaved (F1).\n"
    "  fwtool spelling <file or folder>...\n"
    "      Checks each Word document's spelling with Windows' spell checker and counts its words,\n"
    "      as the app does in idle time, and times both (plan §6.2, §11).\n"
    "  fwtool torture [--seed S] [--edits N] [--rounds R] [--workbooks | --decks] [--out DIR] [--json FILE] <file or folder>...\n"
    "      The open, edit and save test (plan §10, D2): R rounds of N seeded random edits per Word\n"
    "      document through the editor (typing, deletes and replacements across fields, bookmarks,\n"
    "      comments, content controls, tables and tracked changes, splits, joins, run, paragraph and\n"
    "      paragraph-mark formatting, groups of edits as one undo step, undo and redo), then\n"
    "      save: the saved file must open, pass check with no new errors, and read back as the\n"
    "      edited document. A failure is shrunk to the fewest edits that still fail, kept in\n"
    "      DIR/repro (default DIR: build/torture). DIR/saved and DIR/pairs.tsv hold a saved copy of\n"
    "      each document for the validator and Word (scripts/torture.ps1).\n"
    "      Workbooks (S2): seeded edits through the edit API (values and formulas typed in en-NL\n"
    "      and en-US, formats, rows, columns, cells and sheets inserted, deleted, renamed and\n"
    "      moved, merges, sort, filter, paste, undo, redo; S3: names, sheet and workbook\n"
    "      protection, links, notes, comment threads, tables, conditional formats, data\n"
    "      validation, charts, pictures and shapes (inserted, moved, copied, formatted, deleted),\n"
    "      page setup and page breaks), then save: the saved file must open,\n"
    "      pass check and the model's checks with no new errors, read back as the edited workbook,\n"
    "      and keep every part the save didn't write byte for byte. Without the edit API in the\n"
    "      build, every edit is refused and the save writes the model's parts. --workbooks: only\n"
    "      the workbooks among the inputs (scripts/sheet-torture.ps1). --ops value,formula,...:\n"
    "      only those edits (names as reproducers write them). After the edits, undoing them all\n"
    "      must give the workbook as opened and redoing them the workbook as edited.\n"
    "      Decks (P2), with --decks (then only the decks): each deck saved with no edits first (the save writes no part; the copy, in\n"
    "      DIR/unchanged, for PowerPoint and the PDF comparison), then seeded edits through the edit API\n"
    "      (new, duplicated, deleted, moved and hidden slides, layouts, typing, notes, shapes moved,\n"
    "      resized, rotated, deleted, grouped, ungrouped, reordered and formatted, undo, redo), then\n"
    "      save: the same checks as workbooks (scripts/deck-torture.ps1).\n"
    "      Without the edit API in the build, every edit is refused.\n"
    "  fwtool torture --replay <reproducer.txt> [--out DIR]\n"
    "      Applies a reproducer's edits again and reports what happens and which parts the save\n"
    "      wrote. The file it makes goes next to the reproducer as <name>-replay.docx, or into DIR,\n"
    "      with <name>-replay-edited.txt and -readback.txt when the text differs. Its document is\n"
    "      found by its recorded path or, from another checkout, by the tail of that path.\n"
    "  fwtool convert [--password P] <file.doc> <file.docx>\n"
    "      Writes the .docx a Word 97-2003 document is read as (or the .xlsx an Excel 97-2003\n"
    "      workbook, .xls, is read as). Every other command reads .doc and .xls files this way too.\n"
    "      An OpenDocument presentation (.odp, .otp) gives the .pptx the app reads it as.\n"
    "      --password opens an encrypted .ppt (also with other commands that take --password).\n"
    "  fwtool wrap [options] (--file <file> | <text>...)\n"
    "      Lays out paragraphs with fwtext and prints their lines, to compare line breaks with\n"
    "      Word's. Run it without text for its options.\n"
    "Environment:\n"
    "  FW_FILE_LOG_DIR=DIR  every file each fwtool opens or looks for, by any module (fonts too), is\n"
    "      logged once to DIR\\fwtool-<pid>.txt (F1, f1-gate's legal row: no cloud-font file read).\n";

std::vector<std::filesystem::path> CollectSorted(const std::filesystem::path& dir) {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::end(it);
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    std::wstring ext = fw::ToLower(it->path().extension().wstring());
    for (const wchar_t* known : {L".docx", L".docm", L".dotx", L".dotm", L".doc", L".xlsx", L".xlsm", L".xltx", L".xltm", L".xls", L".ods", L".ots",
                                 L".pptx", L".pptm", L".potx", L".potm", L".ppsx", L".ppsm", L".ppt", L".pps", L".pot"})
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

std::vector<std::filesystem::path> CollectFiles(const Options& options) {
  auto files = CollectFiles(options.inputs);
  if (options.workbooks) std::erase_if(files, [](const std::filesystem::path& f) { return !IsWorkbookFile(f); });
  return files;
}

std::string Display(const std::filesystem::path& file) {
  std::error_code ec;
  std::filesystem::path rel = file.lexically_proximate(std::filesystem::current_path(ec));
  const std::filesystem::path& shown = rel.native().size() < file.native().size() ? rel : file;
  return fw::WideToUtf8(shown.generic_wstring());
}

// --password: opens an encrypted PowerPoint 97-2003 presentation.
std::wstring legacyPassword;

// A Word 97-2003 document as the .docx it's read as; empty (with `status`) when it isn't one or
// can't be read.
std::vector<uint8_t> ConvertDoc(const std::filesystem::path& file, fw::pkg::Status& status) {
  // P3: an OpenDocument presentation, as the app converts it (for `convert`).
  if (const std::wstring ext = fw::ToLower(file.extension().wstring()); ext == L".odp" || ext == L".otp") {
    std::vector<uint8_t> pptx;
    auto source = fw::pkg::Snapshot::Open(file.wstring(), {}, status);
    if (!source || !(status = fw::slide::odp::OdpToPptx(source->Bytes(), pptx)).Ok()) pptx.clear();
    return pptx;
  }
  std::vector<uint8_t> bytes;
  FILE* f = nullptr;
  if (_wfopen_s(&f, file.c_str(), L"rb") == 0 && f) {
    uint8_t buf[1 << 16];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
  }
  auto cf = fw::pkg::CompoundFile::Open(bytes);
  // An Excel 97-2003 workbook: the .xlsx it's read as.
  if (cf && fw::sheet::xls::IsWorkbook([&cf](std::string_view name) { return cf->Stream(name); }))
    return fw::sheet::xls::ToXlsx([&cf](std::string_view name) { return cf->Stream(name); }, status);
  // A PowerPoint 97-2003 presentation (P3): the .pptx it's read as.
  if (cf && fw::slide::ppt::IsPresentation([&cf](std::string_view name) { return cf->Stream(name); }))
    return fw::slide::ppt::ToPptx([&cf](std::string_view name) { return cf->Stream(name); }, status, legacyPassword);
  if (!cf || !cf->Stream("WordDocument")) {
    status = fw::pkg::Status::Fail(fw::pkg::Status::Code::Unsupported,
                                   "not a Word 97-2003 document, Excel 97-2003 workbook or PowerPoint 97-2003 presentation");
    return {};
  }
  return fw::doc::msdoc::ToDocx([&cf](std::string_view name) { return cf->Stream(name); }, status);
}

std::unique_ptr<fw::pkg::Package> OpenPackage(const std::filesystem::path& file, fw::pkg::Status& status) {
  if (const std::wstring ext = fw::ToLower(file.extension().wstring());
      ext == L".doc" || ext == L".xls" || ext == L".ppt" || ext == L".pps" || ext == L".pot") {
    std::vector<uint8_t> docx = ConvertDoc(file, status);
    if (docx.empty()) return nullptr;
    return fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(std::move(docx)), fw::pkg::Limits::Default(), status);
  }
  if (const std::wstring ext = fw::ToLower(file.extension().wstring()); ext == L".ods" || ext == L".ots") {
    // S4: an OpenDocument spreadsheet, converted as the app converts it.
    auto source = fw::pkg::Snapshot::Open(file.wstring(), {}, status);
    std::vector<uint8_t> xlsx;
    const fw::sheet::ods::DigitWidth digit = [](const std::string& family, double points) {
    // The default font's widest digit as the grid measures it, so column widths come back as ODF's.
    fw::text::Fonts fonts(fw::sheet::view::ExcelFontSubstitute);  // as the app's grid
    fw::sheet::view::FontMeasure measure(fonts);
    fw::sheet::Font f;
    f.name = family;
    f.size = points;
    return measure.Of(f, f).digit;
  };
    if (!source || !(status = fw::sheet::ods::OdsToXlsx(source->Bytes(), xlsx, fw::pkg::Limits::Default(), digit)).Ok())
      return nullptr;
    return fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(std::move(xlsx)), fw::pkg::Limits::Default(), status);
  }
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
  fwtool::InstallFileLog();  // FW_FILE_LOG_DIR: before anything opens a file
  fw::chart::Install();
  // Substitutes to measure (draw/FontSubstitutes.h): rows of their own, before the table's.
  if (const wchar_t* tuning = _wgetenv(L"FW_SUBSTITUTES")) draw::SetSubstituteTuning(tuning);
  if (argc >= 2 && std::wstring(argv[1]) == L"wrap") return fwtool::RunWrap(argc, argv);
  // The count first: with no arguments argv[1] is null (fwtool alone then prints the usage below).
  if ((argc == 4 || (argc == 6 && std::wstring(argv[2]) == L"--password")) && std::wstring(argv[1]) == L"convert") {
    const int at = argc == 6 ? 4 : 2;
    if (argc == 6) fwtool::legacyPassword = argv[3];
    fw::pkg::Status st;
    const std::vector<uint8_t> docx = fwtool::ConvertDoc(argv[at], st);
    FILE* f = nullptr;
    if (!docx.empty() && _wfopen_s(&f, argv[at + 1], L"wb") != 0) f = nullptr;
    if (!f) {
      std::fprintf(stderr, "%s: %s\n", fw::WideToUtf8(argv[at]).c_str(), docx.empty() ? st.message.c_str() : "can't write");
      return 1;
    }
    std::fwrite(docx.data(), 1, docx.size(), f);
    std::fclose(f);
    return 0;
  }
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
    else if (arg == L"--workbooks") options.workbooks = true;
    else if (arg == L"--decks") options.decks = true;
    else if (arg == L"--layout") options.layout = true;
    else if (arg == L"--expect-reject") options.expectReject = true;
    else if (arg == L"--out" && i + 1 < argc) options.outDir = argv[++i];
    else if (arg == L"--password" && i + 1 < argc) options.password = argv[++i];
    else if (arg == L"--pfx" && i + 1 < argc) options.pfx = argv[++i];
    else if (arg == L"--pfx-password" && i + 1 < argc) options.pfxPassword = argv[++i];
    else if (arg == L"--printer" && i + 1 < argc) options.printer = argv[++i];
    else if (arg == L"--warp") options.warp = true;
    else if (arg == L"--pictures") options.pictures = true;
    else if (arg == L"--png") options.png = true;
    else if (arg == L"--scale" && i + 1 < argc) options.scale = std::clamp(static_cast<float>(_wtof(argv[++i])), 0.1f, 16.f);
    else if (arg == L"--dumps" && i + 1 < argc) options.dumps = argv[++i];
    else if (arg == L"--json" && i + 1 < argc) options.jsonOut = argv[++i];
    else if (arg == L"--report" && i + 1 < argc) options.reportOut = argv[++i];
    else if (arg == L"--locale" && i + 1 < argc) options.locale = argv[++i];
    else if (arg == L"--regions" && i + 1 < argc) {
      std::wstring list = argv[++i];
      for (size_t a = 0; a <= list.size();) {
        size_t b = std::min(list.find(L',', a), list.size());
        if (b > a) options.regions.push_back(list.substr(a, b - a));
        a = b + 1;
      }
    }
    else if (arg == L"--min-match" && i + 1 < argc) options.minMatch = _wtof(argv[++i]);
    else if (arg == L"--threads") options.threads = true;
    else if (arg == L"--idle-threads") options.idleThreads = true;
    else if (arg == L"--modules") options.modules = true;
    else if (arg == L"--languages" && i + 1 < argc) options.languages = fw::WideToUtf8(argv[++i]);
    else if (arg == L"--min-pages" && i + 1 < argc) options.minPages = _wtof(argv[++i]);
    else if (arg == L"--min-lines" && i + 1 < argc) options.minLines = _wtof(argv[++i]);
    else if (arg == L"--self-test") options.selfTest = true;
    else if (arg == L"--cells") options.cells = true;
    else if (arg == L"--excel" && i + 1 < argc) options.excel = argv[++i];
    else if (arg == L"--values") options.values = true;
    else if (arg == L"--resave") options.resave = true;
    else if (arg == L"--min-cells" && i + 1 < argc) options.minCells = _wtof(argv[++i]);
    else if (arg == L"--min-ssim" && i + 1 < argc) options.minSsim = _wtof(argv[++i]);
    else if (arg == L"--folders" && i + 1 < argc) options.folders = fw::WideToUtf8(argv[++i]);
    else if (arg == L"--all") options.all = true;
    else if (arg == L"--runs" && i + 1 < argc) options.runs = _wtoi(argv[++i]);
    else if (arg == L"--budgets") options.budgets = true;
    else if (arg == L"--typing") options.typing = true;
    else if (arg == L"--scrolling") options.scrolling = true;
    else if (arg == L"--sheet-edits") options.sheetEdits = true;
    else if (arg == L"--slide-text") options.slideText = true;
    else if (arg == L"--edits" && i + 1 < argc) options.edits = std::max(1, _wtoi(argv[++i]));
    else if (arg == L"--seed" && i + 1 < argc) options.seed = static_cast<unsigned>(std::wcstoul(argv[++i], nullptr, 10));
    else if (arg == L"--rounds" && i + 1 < argc) options.rounds = static_cast<uint32_t>(std::max(1, _wtoi(argv[++i])));
    else if (arg == L"--replay") options.replay = true;
    else if (arg == L"--ops" && i + 1 < argc) options.ops = fw::WideToUtf8(argv[++i]);
    else if (arg == L"--edited") options.edited = true;
    else if (arg == L"--samples" && i + 1 < argc) options.samples = argv[++i];
    else if (arg == L"--pngs" && i + 1 < argc) options.pngs = argv[++i];
    else if (arg.starts_with(L"--")) {
      std::fprintf(stderr, "Unknown option %s\n\n%s", fw::WideToUtf8(arg).c_str(), fwtool::kUsage);
      return 2;
    } else options.inputs.emplace_back(arg);
  }
  if (options.inputs.empty() && !((command == L"pdf-valid" || command == L"pdf-torture") && !options.samples.empty())) {
    std::fputs(fwtool::kUsage, stderr);
    return 2;
  }
  fwtool::legacyPassword = options.password;
  if (command == L"roundtrip") return fwtool::RunRoundtrip(options);
  if (command == L"dump") return fwtool::RunDump(options);
  if (command == L"formulas") return fwtool::RunFormulas(options);
  if (command == L"save") return fwtool::RunSave(options);
  if (command == L"check") return fwtool::RunCheck(options);
  if (command == L"odt") return fwtool::RunOdt(options);
  if (command == L"print-sample") return fwtool::RunPrintSample(options);
  if (command == L"render") return fwtool::RunRender(options);
  if (command == L"lines") return fwtool::RunLines(options);
  if (command == L"compare") return options.cells ? fwtool::RunCellCompare(options) : fwtool::RunCompare(options);
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
  if (command == L"csv") return fwtool::RunCsv(options);
  if (command == L"pdf-check") return fwtool::RunPdfCheck(options);
  if (command == L"pdf-valid") return fwtool::RunPdfValid(options);
  if (command == L"pdf-torture") return fwtool::RunPdfTorture(options);
  if (command == L"pdf-ocr") return fwtool::RunPdfOcr(options);
  if (command == L"pdf-compare") return fwtool::RunPdfCompare(options);
  if (command == L"slides") return fwtool::RunSlides(options);
  if (command == L"timing") return fwtool::RunTiming(options);
  std::fprintf(stderr, "Unknown command %s\n\n%s", fw::WideToUtf8(command).c_str(), fwtool::kUsage);
  return 2;
}
