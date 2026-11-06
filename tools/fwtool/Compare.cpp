// fwtool lines and fwtool compare: Word's lines from its reference PDFs, and how closely
// Featherweight's layout follows them, page count and line breaks (plan §10, milestone D1).
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <set>

#include "Common.h"
#include "Fidelity.h"
#include "LayoutHook.h"
#include "Reference.h"
#include "core/Strings.h"

namespace fwtool {
namespace {

using fidelity::Area;
using fidelity::LineResult;
using fidelity::Score;
using fidelity::Text;
using fw::LayoutDump;

bool WriteFile(const std::filesystem::path& path, std::string_view text) {
  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  return static_cast<bool>(out);
}

std::optional<LayoutDump> LoadDump(const std::filesystem::path& path, std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "no layout dump at " + Display(path);
    return std::nullopt;
  }
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::string why;
  auto dump = fw::ReadLayoutDump(text, &why);
  if (!dump) error = Display(path) + ": " + why;
  return dump;
}

std::string Percent(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f %%", v);
  return buf;
}

double Share(int part, int whole) { return whole ? 100.0 * part / whole : 100.0; }

std::string Plural(int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

// ---------- Describing lines ----------

const std::wstring& TextOf(const LayoutDump& dump, const fidelity::Line& line) {
  return dump.pages[line.page].lines[line.index].text;
}

// "p2 l7": the page, and the line's number among that page's lines of its area.
std::string LineLabel(const Text& t, int line) {
  const fidelity::Line& l = t.lines[line];
  int n = 1;
  for (int i = line - 1; i >= 0 && t.lines[i].area == l.area && t.lines[i].page == l.page; --i) ++n;
  return "p" + std::to_string(l.page + 1) + " l" + std::to_string(n);
}

std::string Where(const Text& t, int line) {
  const fidelity::Line& l = t.lines[line];
  std::string label = LineLabel(t, line);
  return "page " + std::to_string(l.page + 1) + ", line " + label.substr(label.find('l') + 1) + " of the " +
         fidelity::AreaName(l.area);
}

// The last word of a line as drawn.
std::string LastWord(const std::wstring& text) {
  std::wstring t = fw::Trim(text);
  size_t space = t.find_last_of(L" \t\u00A0");
  return fw::WideToUtf8(space == std::wstring::npos ? t : t.substr(space + 1));
}

std::string Quote(const std::string& s) { return "\u201C" + s + "\u201D"; }

std::string Difference(const Score& s, const LineResult& r, const LayoutDump& reference, const LayoutDump& ours) {
  const fidelity::Line& line = s.reference.lines[r.line];
  std::string word = Quote(LastWord(TextOf(reference, line)));
  if (r.ours < 0 || s.wordMap[line.end.token] < 0)
    return "Word ends the line at " + word + ", which isn't in Featherweight's text there";
  std::string theirs = Quote(LastWord(TextOf(ours, s.ours.lines[r.ours])));
  if (r.wordsLater == 0) return "Word ends the line at " + word + ", Featherweight breaks that word elsewhere: " + theirs;
  return "Word ends the line at " + word + ", Featherweight " +
         Plural(std::abs(r.wordsLater), "word", "words") + (r.wordsLater > 0 ? " later" : " earlier") + ", at " + theirs;
}

// ---------- One document: where the line breaks diverge ----------

int ConsoleWidth() {
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
    return std::max(100, static_cast<int>(info.srWindow.Right - info.srWindow.Left + 1));
  return 160;
}

// Pads or shortens text to `width` characters. Lines lose their start, not their end: the end is
// where they break.
std::string Fit(const std::wstring& text, size_t width, bool keepEnd = false) {
  std::wstring t = fw::Trim(text);
  if (t.size() > width && width)
    t = keepEnd ? L"\u2026" + t.substr(t.size() - width + 1) : t.substr(0, width - 1) + L"\u2026";
  std::string s = fw::WideToUtf8(t);
  return s + std::string(width - std::min(width, t.size()), ' ');
}

std::string Cell(const Text& t, const LayoutDump& dump, int line, size_t width) {
  if (line < 0) return std::string(width, ' ');
  const fw::LayoutLine& l = dump.pages[t.lines[line].page].lines[t.lines[line].index];
  char head[48];
  int n = std::snprintf(head, sizeof(head), "%-8s y %6.1f  ", LineLabel(t, line).c_str(), l.baseline);
  return head + Fit(l.text, width > static_cast<size_t>(n) ? width - n : 0, true);
}

void PrintDivergence(const Score& s, const LineResult& first, const LayoutDump& reference, const LayoutDump& ours) {
  const Text& R = s.reference;
  const Text& O = s.ours;
  int area = static_cast<int>(R.lines[first.line].area);
  int r0 = std::max(R.lineStart[area], first.line - 3);
  int r1 = std::min(R.lineStart[area + 1], first.line + 5);
  // Our lines, row by row, from the one where the first of Word's lines shown starts.
  bool anchored = false;
  int o0 = 0, oArea = area;
  for (int r = r0; r < r1 && !anchored; ++r) {
    if (R.lines[r].firstWord < 0 || s.wordMap[R.lines[r].firstWord] < 0) continue;
    int o = O.words[s.wordMap[R.lines[r].firstWord]].line;
    anchored = true;
    o0 = o - (r - r0);
    oArea = static_cast<int>(O.lines[o].area);
  }
  size_t half = static_cast<size_t>(ConsoleWidth() - 5) / 2;
  std::printf("\n  %s \u2502 %s\n", Fit(L"Word", half).c_str(), Fit(L"Featherweight", half).c_str());
  for (int r = r0; r < r1; ++r) {
    int o = anchored ? o0 + (r - r0) : -1;
    if (o < O.lineStart[oArea] || o >= O.lineStart[oArea + 1]) o = -1;
    std::printf("%c %s \u2502 %s\n", r == first.line ? '>' : ' ', Cell(R, reference, r, half).c_str(),
                Cell(O, ours, o, half).c_str());
  }
}

int CompareOne(const Options& options) {
  const auto& refPath = options.inputs[0];
  const auto& ourPath = options.inputs[1];
  std::string error;
  std::optional<LayoutDump> reference = fw::ToLower(refPath.extension().wstring()) == L".pdf"
                                            ? ReadPdfLines(refPath, error)
                                            : LoadDump(refPath, error);
  if (!reference) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  std::optional<LayoutDump> ours;
  if (fw::ToLower(ourPath.extension().wstring()) == L".json") {
    ours = LoadDump(ourPath, error);
  } else {
    LayoutDump laidOut;
    if (LayOutDocument(ourPath, &laidOut, nullptr, error)) ours = std::move(laidOut);
  }
  if (!ours) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  Score s = fidelity::Compare(*reference, *ours);
  std::printf("%s: %s in Word, %d in Featherweight; %d of %s break where Word breaks them (%s).\n",
              Display(ourPath).c_str(), Plural(s.referencePages, "page", "pages").c_str(), s.pages, s.matched,
              Plural(s.lines, "line", "lines").c_str(), Percent(s.LinePercent()).c_str());
  const LineResult* first = s.FirstDifference();
  if (!first) {
    std::printf("Every line breaks where Word breaks it.\n");
    return s.PagesMatch() ? 0 : 1;
  }
  std::printf("\nFirst difference: %s\n  %s.\n", Where(s.reference, first->line).c_str(),
              Difference(s, *first, *reference, *ours).c_str());
  PrintDivergence(s, *first, *reference, *ours);
  int shown = 0, total = s.lines - s.matched;
  std::printf("\n%s:\n", total == 1 ? "The difference" : ("All " + std::to_string(total) + " differences").c_str());
  for (const LineResult& r : s.results) {
    if (r.matched) continue;
    if (!options.all && shown == 20) {
      std::printf("  \u2026 and %d more (--all lists them all)\n", total - shown);
      break;
    }
    std::printf("  %s: %s\n", Where(s.reference, r.line).c_str(), Difference(s, r, *reference, *ours).c_str());
    ++shown;
  }
  return 1;
}

// ---------- A corpus ----------

struct DocResult {
  std::string source;
  std::string error;  // why it couldn't be measured
  int referencePages = 0, pages = 0, lines = 0, matched = 0;
  std::string first;  // the first difference
  bool copy = false;  // another format of a document the corpus has as .docx (see CopyKey)
  bool Measured() const { return error.empty(); }
};

// The corpus keeps each document as .docx under docx/<category>/ and may add copies of it in other
// formats or folders: dotx/<category>/<name>.dotx, doc/..., odt/..., protected/<category>/<name>.docx.
// A copy is measured against Word's render of the copy but reported apart, so one document doesn't
// count twice toward the targets. The key is "<category>/<name>" without the format; `docx` says
// whether the source is the .docx original itself.
std::string CopyKey(const std::string& source, bool* docx) {
  const size_t slash = source.find('/');
  const size_t dot = source.rfind('.');
  const std::string folder = slash == std::string::npos ? "" : source.substr(0, slash);
  std::string ext = dot == std::string::npos ? "" : source.substr(dot);
  for (char& c : ext)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  *docx = folder == "docx" && ext == ".docx";
  const size_t from = slash == std::string::npos ? 0 : slash + 1;
  return source.substr(from, (dot == std::string::npos || dot < from ? source.size() : dot) - from);
}

void AppendJsonString(std::string& out, std::string_view s) {
  out += '"';
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if (static_cast<unsigned char>(c) < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out += c;
    }
  }
  out += '"';
}

std::string Today() {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &now);
  char buf[16];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
  return buf;
}

struct Totals {
  int documents = 0, pagesMatch = 0, lines = 0, matched = 0;
  double PagePercent() const { return Share(pagesMatch, documents); }
  double LinePercent() const { return Share(matched, lines); }
};

// The totals are the targets'; "copies" has the format copies' (see CopyKey).
std::string JsonReport(const std::vector<DocResult>& docs, const Totals& t, const Totals& copies,
                       const std::string& references) {
  std::string out = "{\n  \"format\": \"featherweight-fidelity\",\n  \"version\": 2,\n  \"made\": \"" + Today() +
                    "\",\n  \"references\": ";
  AppendJsonString(out, references);
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                ",\n  \"documents\": %d,\n  \"pages\": {\"match\": %d, \"documents\": %d, \"percent\": %.2f},\n"
                "  \"lines\": {\"match\": %d, \"lines\": %d, \"percent\": %.2f},\n"
                "  \"copies\": {\"documents\": %d, \"pages\": {\"match\": %d, \"percent\": %.2f}, "
                "\"lines\": {\"match\": %d, \"lines\": %d, \"percent\": %.2f}},\n  \"files\": [",
                t.documents, t.pagesMatch, t.documents, t.PagePercent(), t.matched, t.lines, t.LinePercent(),
                copies.documents, copies.pagesMatch, copies.PagePercent(), copies.matched, copies.lines,
                copies.LinePercent());
  out += buf;
  for (size_t i = 0; i < docs.size(); ++i) {
    const DocResult& d = docs[i];
    out += i ? ",\n    {\"source\": " : "\n    {\"source\": ";
    AppendJsonString(out, d.source);
    if (!d.Measured()) {
      out += ", \"error\": ";
      AppendJsonString(out, d.error);
    }
    std::snprintf(buf, sizeof(buf), ", \"wordPages\": %d, \"pages\": %d, \"lines\": %d, \"match\": %d, \"percent\": %.2f",
                  d.referencePages, d.pages, d.lines, d.matched, Share(d.matched, d.lines));
    out += buf;
    if (d.copy) out += ", \"copy\": true";
    if (!d.first.empty()) {
      out += ", \"firstDifference\": ";
      AppendJsonString(out, d.first);
    }
    out += '}';
  }
  out += "\n  ]\n}\n";
  return out;
}

std::string MarkdownCell(std::string s) {
  for (size_t i = 0; (i = s.find('|', i)) != std::string::npos; i += 2) s.replace(i, 1, "\\|");
  return s;
}

std::string MarkdownReport(const std::vector<DocResult>& docs, const Totals& t, const Totals& copies,
                           const std::string& references, const Options& options) {
  std::string out = "# Layout fidelity\n\nFeatherweight's layout measured against Word's reference renders in `" +
                    references + "` (plan \u00A710), " + Today() + ".\n\n";
  bool targets = options.minPages >= 0 || options.minLines >= 0;
  out += targets ? "| | Match | Of | Share | Target |\n|---|---:|---:|---:|---:|\n" : "| | Match | Of | Share |\n|---|---:|---:|---:|\n";
  auto row = [&](const char* name, int match, int of, double pct, double target) {
    out += std::string("| ") + name + " | " + std::to_string(match) + " | " + std::to_string(of) + " | " + Percent(pct);
    if (targets) out += target >= 0 ? " | \u2265 " + Percent(target) : std::string(" | ");
    out += " |\n";
  };
  row("Documents whose page count matches Word's", t.pagesMatch, t.documents, t.PagePercent(), options.minPages);
  row("Lines that break where Word breaks them", t.matched, t.lines, t.LinePercent(), options.minLines);
  if (copies.documents) {
    out += "\nFormat copies (.dotx, .doc, .odt and password-protected copies of the documents above), measured against "
           "Word's render of each copy and not counted in the targets:\n\n| | Match | Of | Share |\n|---|---:|---:|---:|\n";
    auto plain = [&](const char* name, int match, int of, double pct) {
      out += std::string("| ") + name + " | " + std::to_string(match) + " | " + std::to_string(of) + " | " + Percent(pct) + " |\n";
    };
    plain("Copies whose page count matches Word's", copies.pagesMatch, copies.documents, copies.PagePercent());
    plain("Lines that break where Word breaks them", copies.matched, copies.lines, copies.LinePercent());
  }

  std::vector<const DocResult*> measured;
  for (const DocResult& d : docs)
    if (d.Measured() && !d.copy) measured.push_back(&d);
  std::stable_sort(measured.begin(), measured.end(), [](const DocResult* a, const DocResult* b) {
    return Share(a->matched, a->lines) < Share(b->matched, b->lines);
  });
  out += "\n## The 20 documents with the most line breaks that differ\n\n"
         "| Document | Pages (Word / Featherweight) | Lines that match | First difference |\n|---|---|---:|---|\n";
  for (size_t i = 0; i < measured.size() && i < 20; ++i) {
    const DocResult& d = *measured[i];
    out += "| " + MarkdownCell(d.source) + " | " + std::to_string(d.referencePages) + " / " + std::to_string(d.pages) +
           (d.referencePages == d.pages ? "" : " \u26A0") + " | " + std::to_string(d.matched) + " of " +
           std::to_string(d.lines) + " (" + Percent(Share(d.matched, d.lines)) + ") | " + MarkdownCell(d.first) + " |\n";
  }
  std::string pages;
  for (const DocResult* d : measured)
    if (d->referencePages != d->pages)
      pages += "- " + d->source + ": " + std::to_string(d->referencePages) + " in Word, " + std::to_string(d->pages) +
               " in Featherweight\n";
  if (!pages.empty()) out += "\n## Page count differs\n\n" + pages;
  std::string copyPages;
  for (const DocResult& d : docs)
    if (d.copy && d.Measured() && d.referencePages != d.pages)
      copyPages += "- " + d.source + ": " + std::to_string(d.referencePages) + " in Word, " + std::to_string(d.pages) +
                   " in Featherweight\n";
  if (!copyPages.empty()) out += "\n## Copies whose page count differs\n\n" + copyPages;
  std::string missing;
  for (const DocResult& d : docs)
    if (!d.Measured()) missing += "- " + d.source + (d.copy ? " (a copy)" : "") + ": " + d.error + "\n";
  if (!missing.empty()) out += "\n## Not measured\n\nThese count as wrong in the totals.\n\n" + missing;
  return out;
}

int CompareCorpus(const Options& options) {
  if (options.dumps.empty() && !HaveLayout()) {
    std::fprintf(stderr,
                 "Featherweight can't lay out Word documents yet (milestone D1). Measure layout dumps with\n"
                 "--dumps DIR (docs/layout-dump.md), or check the measurement itself with --self-test.\n");
    return 2;
  }
  std::vector<DocResult> docs;
  Totals totals, copies;  // the targets, and the format copies (see CopyKey)
  std::string references;
  int unreadable = 0;  // reference PDFs PDFium can't read: not measured, but the run fails
  for (const auto& input : options.inputs) {
    std::string error;
    auto set = ReadReferences(input, error);
    if (!set) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    references += (references.empty() ? "" : ", ") + Display(set->folder);
    std::set<std::string> originals;
    for (const ReferenceEntry& e : set->entries) {
      bool docx = false;
      std::string key = CopyKey(e.source, &docx);
      if (docx) originals.insert(std::move(key));
    }
    for (const ReferenceEntry& e : set->entries) {
      error.clear();  // one document's failure says nothing about the next
      DocResult d;
      d.source = e.source;
      bool docx = false;
      const std::string key = CopyKey(e.source, &docx);
      d.copy = !e.copyOf.empty() || (!docx && originals.count(key) > 0);  // the manifest's copyOf, else by name
      if (e.pdf.empty()) {
        std::printf("[skip] %s: no reference PDF (%s)\n", e.source.c_str(), e.error.c_str());
        continue;
      }
      if (!e.exclude.empty()) {  // the manifest's "exclude": Word's own render can't be the reference
        std::printf("[skip] %s: left out (%s)\n", e.source.c_str(), e.exclude.c_str());
        continue;
      }
      error.clear();  // one unreadable PDF mustn't fail the documents after it
      auto reference = ReadPdfLines(e.pdf, error);
      if (!reference) {
        std::printf("[FAIL] %s: Word's PDF can't be read: %s\n", e.source.c_str(), error.c_str());
        ++unreadable;
        continue;
      }
      std::optional<LayoutDump> ours;
      if (!options.dumps.empty()) {
        ours = LoadDump(DumpPathFor(options.dumps, e.source), error);
      } else {
        LayoutDump laidOut;
        if (LayOutDocument(e.document, &laidOut, nullptr, error, fw::Utf8ToWide(e.password))) ours = std::move(laidOut);
      }
      if (!ours) d.error = error;
      Score s = fidelity::Compare(*reference, ours ? *ours : LayoutDump{});
      d.referencePages = s.referencePages;
      d.pages = s.pages;
      d.lines = s.lines;
      d.matched = s.matched;
      if (const LineResult* first = s.FirstDifference(); first && ours)
        d.first = Where(s.reference, first->line) + ": " + Difference(s, *first, *reference, *ours);
      Totals& sum = d.copy ? copies : totals;
      ++sum.documents;
      if (s.PagesMatch() && ours) ++sum.pagesMatch;
      sum.lines += d.lines;
      sum.matched += d.matched;
      bool perfect = ours && s.PagesMatch() && s.matched == s.lines;
      std::printf("%s %s: %s in Word, %d here; %d of %d lines match (%s)%s%s\n", perfect ? "[ ok ]" : "[diff]",
                  d.source.c_str(), Plural(d.referencePages, "page", "pages").c_str(), d.pages, d.matched, d.lines,
                  Percent(Share(d.matched, d.lines)).c_str(), d.Measured() ? "" : "; ", d.error.c_str());
      if (!d.first.empty()) std::printf("    first difference: %s\n", d.first.c_str());
      docs.push_back(std::move(d));
    }
  }
  std::printf("\n%d Word documents: the page count matches for %d (%s); %d of %d lines break where Word breaks "
              "them (%s).\n",
              totals.documents, totals.pagesMatch, Percent(totals.PagePercent()).c_str(), totals.matched, totals.lines,
              Percent(totals.LinePercent()).c_str());
  if (copies.documents)
    std::printf("Copies in other formats, apart from those: the page count matches for %d of %d (%s); %d of %d lines "
                "(%s).\n",
                copies.pagesMatch, copies.documents, Percent(copies.PagePercent()).c_str(), copies.matched, copies.lines,
                Percent(copies.LinePercent()).c_str());
  if (!options.jsonOut.empty() && !WriteFile(options.jsonOut, JsonReport(docs, totals, copies, references)))
    std::fprintf(stderr, "Couldn't write %s\n", Display(options.jsonOut).c_str());
  if (!options.reportOut.empty() &&
      !WriteFile(options.reportOut, MarkdownReport(docs, totals, copies, references, options)))
    std::fprintf(stderr, "Couldn't write %s\n", Display(options.reportOut).c_str());

  bool failed = unreadable > 0;
  if (unreadable) std::printf("FAILED: %s can't be read.\n", Plural(unreadable, "reference PDF", "reference PDFs").c_str());
  if (options.minPages >= 0 && totals.PagePercent() < options.minPages) {
    std::printf("FAILED: the page count matches for %s of documents; the target is %s.\n",
                Percent(totals.PagePercent()).c_str(), Percent(options.minPages).c_str());
    failed = true;
  }
  if (options.minLines >= 0 && totals.LinePercent() < options.minLines) {
    std::printf("FAILED: %s of lines break where Word breaks them; the target is %s.\n",
                Percent(totals.LinePercent()).c_str(), Percent(options.minLines).c_str());
    failed = true;
  }
  return failed ? 1 : 0;
}

// ---------- The self-test: Word against itself, and against known changes ----------

int SelfTest(const Options& options) {
  int documents = 0, failed = 0, noted = 0;
  for (const auto& input : options.inputs) {
    std::string error;
    auto set = ReadReferences(input, error);
    if (!set) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    for (const ReferenceEntry& e : set->entries) {
      if (e.pdf.empty()) continue;
      ++documents;
      error.clear();
      auto reference = ReadPdfLines(e.pdf, error);
      if (!reference) {
        std::printf("[FAIL] %s: %s\n", e.source.c_str(), error.c_str());
        ++failed;
        continue;
      }
      reference->source = e.source;
      std::vector<std::string> problems, notes;
      // Word counts pages before it exports, from a layout that can lag on long documents (it may
      // still be the count the file was saved with). The PDF is what's measured against.
      if (e.pages && e.pages != static_cast<int>(reference->pages.size()))
        notes.push_back("note: Word counted " + std::to_string(e.pages) + " pages before exporting, its PDF has " +
                        std::to_string(reference->pages.size()) + "; the PDF is the reference");

      // Word against itself, through the dump format: every line matches.
      auto copy = fw::ReadLayoutDump(fw::WriteLayoutDump(*reference), &error);
      if (!copy) {
        problems.push_back("the dump doesn't read back: " + error);
        copy = *reference;
      }
      Score self = fidelity::Compare(*reference, *copy);
      if (!self.PagesMatch() || self.matched != self.lines)
        problems.push_back("against itself: " + std::to_string(self.matched) + " of " + std::to_string(self.lines) +
                           " lines, " + std::to_string(self.pages) + " pages");
      std::string detail = Plural(self.referencePages, "page", "pages") + ", " + Plural(self.lines, "line", "lines") +
                           ": itself " + Percent(self.LinePercent());

      // One line break moved: exactly that line differs.
      LayoutDump moved = *reference;
      if (int line = fidelity::MoveBreak(moved); line >= 0) {
        Score s = fidelity::Compare(*reference, moved);
        const LineResult* first = s.FirstDifference();
        if (!s.PagesMatch() || s.matched != s.lines - 1 || !first || first->line != line)
          problems.push_back("with a break moved at " + Where(self.reference, line) + ": " +
                             std::to_string(s.matched) + " of " + std::to_string(s.lines) + " lines" +
                             (first ? ", first difference at " + Where(s.reference, first->line) : ""));
        detail += ", a moved break " + std::to_string(s.matched) + "/" + std::to_string(s.lines);
      } else if (self.lines > 1) {
        detail += ", no line to move a break in";
      }

      // A page added: the page count differs, the lines don't.
      LayoutDump added = *reference;
      if (fidelity::AddPage(added)) {
        Score s = fidelity::Compare(*reference, added);
        if (s.pages != s.referencePages + 1 || s.matched != s.lines)
          problems.push_back("with a page added: " + std::to_string(s.pages) + " pages, " +
                             std::to_string(s.matched) + " of " + std::to_string(s.lines) + " lines");
        detail += ", an added page " + std::to_string(s.pages) + " pages";
      }

      std::printf("%s %s: %s\n", problems.empty() ? "[ ok ]" : "[FAIL]", e.source.c_str(), detail.c_str());
      for (const std::string& p : problems) std::printf("    %s\n", p.c_str());
      for (const std::string& n : notes) std::printf("    %s\n", n.c_str());
      if (!problems.empty()) ++failed;
      if (!notes.empty()) ++noted;
    }
  }
  std::printf("\n%d documents, %d failed", documents, failed);
  if (noted) std::printf("; %d with Word's page count differing from its PDF", noted);
  std::printf("\n");
  return failed == 0 && documents > 0 ? 0 : 1;
}

void PrintLines(const std::string& name, const LayoutDump& dump) {
  std::printf("%s: %s\n", name.c_str(), Plural(static_cast<int>(dump.pages.size()), "page", "pages").c_str());
  for (size_t p = 0; p < dump.pages.size(); ++p) {
    const fw::LayoutPage& page = dump.pages[p];
    std::printf("\nPage %zu  %.1f \u00D7 %.1f pt\n", p + 1, page.width, page.height);
    for (const fw::LayoutLine& l : page.lines)
      std::printf("  %-9s x %6.1f  y %6.1f  w %6.1f  %5.1f pt  %s\n", std::string(fw::StoryName(l.story)).c_str(), l.x,
                  l.baseline, l.width, l.size, fw::WideToUtf8(l.text).c_str());
  }
  std::printf("\n");
}

}  // namespace

int RunLines(const Options& options) {
  int failed = 0;
  auto handle = [&](const std::filesystem::path& pdf, const std::string& source, const std::filesystem::path& out) {
    std::string error;
    auto dump = ReadPdfLines(pdf, error);
    if (!dump) {
      std::printf("[FAIL] %s: %s\n", Display(pdf).c_str(), error.c_str());
      ++failed;
      return;
    }
    dump->source = source;
    if (options.outDir.empty()) {
      PrintLines(Display(pdf), *dump);
    } else if (WriteFile(out, fw::WriteLayoutDump(*dump))) {
      size_t lines = 0;
      for (const auto& page : dump->pages) lines += page.lines.size();
      std::printf("[ ok ] %s: %zu pages, %zu lines -> %s\n", Display(pdf).c_str(), dump->pages.size(), lines,
                  Display(out).c_str());
    } else {
      std::printf("[FAIL] couldn't write %s\n", Display(out).c_str());
      ++failed;
    }
  };
  for (const auto& input : options.inputs) {
    if (std::filesystem::is_directory(input) || input.filename() == L"manifest.json") {
      std::string error;
      auto set = ReadReferences(input, error);
      if (!set) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
      }
      for (const ReferenceEntry& e : set->entries)
        if (!e.pdf.empty()) handle(e.pdf, e.source, DumpPathFor(options.outDir, e.source));
    } else {
      std::wstring name = input.filename().wstring();
      if (fw::EndsWithNoCase(name, L".pdf")) name.resize(name.size() - 4);
      handle(input, fw::WideToUtf8(name), options.outDir / (name + L".layout.json"));
    }
  }
  return failed == 0 ? 0 : 1;
}

int RunCompare(const Options& options) {
  if (options.selfTest) return SelfTest(options);
  bool oneDocument = options.inputs.size() == 2 && !std::filesystem::is_directory(options.inputs[0]) &&
                     options.inputs[0].filename() != L"manifest.json";
  return oneDocument ? CompareOne(options) : CompareCorpus(options);
}

}  // namespace fwtool
