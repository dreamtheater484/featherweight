// fwtool bench: how long opening a Word document takes, stage by stage, against the opening
// budgets (plan §11); with --typing, what a keystroke costs. Until D1's layout exists it times what does: reading the file, opening the
// package and parsing Word's XML parts; the layout hook (LayoutHook.h) adds the rest.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>

#include "Common.h"
#include "LayoutHook.h"
#include "Reference.h"
#include "core/OpenTimings.h"
#include "core/Strings.h"
#include "pkg/XmlPull.h"

namespace fwtool {
namespace {

using fw::OpenTimings;

constexpr int kStages = OpenTimings::kStages + 1;  // OpenTimings' stages, then "xml"
constexpr int kXml = OpenTimings::kStages;

// How fwtool reads plan §11's budgets: a document's first page within 400 ms up to 20 pages
// ("a typical 20-page .docx") and 700 ms beyond; fully paginated within 3 s up to 300 pages
// ("a 300-page .docx") and 10 ms a page beyond. Short documents have no separate pagination
// budget: their first page is most of them.
double FirstPageBudget(int pages) { return pages <= 20 ? 400 : 700; }
double PaginatedBudget(int pages) { return pages <= 20 ? -1 : std::max(3000.0, 10.0 * pages); }

// Word's own page count from docProps/app.xml, as last saved: the budget class until the layout
// counts the pages itself.
int SavedPageCount(const fw::pkg::Package& pkg) {
  std::vector<uint8_t> bytes;
  if (!pkg.FindPart("/docProps/app.xml") || !pkg.ReadPart("/docProps/app.xml", bytes).Ok()) return 0;
  fw::pkg::XmlPull xml(AsText(bytes));
  for (auto e = xml.Next(); e != fw::pkg::XmlPull::Event::EndDocument && e != fw::pkg::XmlPull::Event::Error;
       e = xml.Next()) {
    if (e == fw::pkg::XmlPull::Event::StartElement && xml.LocalName() == "Pages" && xml.Depth() == 2) {
      xml.Next();
      int pages = 0;
      return fw::ParseInt(fw::Utf8ToWide(xml.Text()), pages) ? pages : 0;
    }
  }
  return 0;
}

struct Run {
  double ms[kStages];
  int pages = 0;
  std::string error;
};

Run OpenOnce(const std::filesystem::path& file) {
  Run run;
  std::fill(std::begin(run.ms), std::end(run.ms), -1.0);
  OpenTimings t;
  t.Start();
  if (HaveLayout()) {
    fw::LayoutDump dump;
    if (!LayOutDocument(file, &dump, &t, run.error)) return run;
    run.pages = static_cast<int>(dump.pages.size());
  } else {
    // What the app does before the model exists: the snapshot, the package, and Word's XML
    // parts read through once (the model's parse will cost about that, plus building).
    fw::pkg::Status st;
    std::error_code ec;
    fw::pkg::Snapshot::Options options;
    options.recoveryDir = (std::filesystem::temp_directory_path(ec) / L"fwtool-recovery").wstring();
    auto snapshot = fw::pkg::Snapshot::Open(file.wstring(), options, st);
    if (!snapshot) {
      run.error = st.message;
      return run;
    }
    t.Mark(OpenTimings::Read);
    auto pkg = fw::pkg::Package::Open(std::move(snapshot), fw::pkg::Limits::Default(), st);
    if (!pkg) {
      run.error = st.message;
      return run;
    }
    t.Mark(OpenTimings::Package);
    auto start = std::chrono::steady_clock::now();
    std::vector<uint8_t> bytes;
    for (const auto& part : pkg->Parts()) {
      if (!part.name.starts_with("/word/") || !pkg->IsXmlPart(part.name)) continue;
      if (!pkg->ReadPart(part.name, bytes).Ok() || !fw::pkg::XmlPull::Check(AsText(bytes)).Ok()) {
        run.error = part.name + " doesn't parse";
        return run;
      }
    }
    run.ms[kXml] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    run.pages = SavedPageCount(*pkg);
  }
  for (int s = 0; s < OpenTimings::kStages; ++s) run.ms[s] = t.Ms(static_cast<OpenTimings::Stage>(s));
  return run;
}

std::string Ms(double ms) {
  if (ms < 0) return "—";
  char buf[32];
  std::snprintf(buf, sizeof(buf), ms < 10 ? "%.2f" : "%.0f", ms);
  return buf;
}

std::string Pad(const std::string& s, size_t width, bool right = true) {
  size_t chars = fw::Utf8ToWide(s).size();
  std::string pad(width > chars ? width - chars : 0, ' ');
  return right ? pad + s : s + pad;
}


// fwtool bench --typing: keystroke to screen, plan §11: under 16 ms typical and 50 ms at worst in
// a 300-page document. Typing is the median of letters and Backspaces; the worst includes Enter.
constexpr double kTypingBudget = 16, kTypingWorstBudget = 50;

double Quantile(std::vector<double> v, double q) {
  if (v.empty()) return -1;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size() - 1) + 0.5))];
}

int RunTypingBench(const std::vector<std::filesystem::path>& files, const Options& options) {
  const int runs = std::max(1, options.runs);
  std::printf("Typing in Word documents, %d run%s: each keystroke from the edit to the page on screen painted again,\n"
              "in ms (plan §11: under %.0f typical and %.0f at worst in a 300-page document).\n\n",
              runs, runs == 1 ? "" : "s", kTypingBudget, kTypingWorstBudget);
  size_t width = 8;
  for (const auto& f : files) width = std::max(width, fw::Utf8ToWide(Display(f)).size());
  std::printf("%s %5s  %-6s %5s %7s %7s %7s %7s\n", Pad("Document", width, false).c_str(), "pages", "where", "keys",
              "median", "p95", "worst", "Enter");
  std::string json = "{\n  \"format\": \"featherweight-typing-bench\",\n  \"version\": 1,\n  \"runs\": " +
                     std::to_string(runs) + ",\n  \"files\": [";
  int failed = 0, over = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    std::vector<TypingPlace> all;
    size_t pages = 0;
    std::string error;
    for (int r = 0; r < runs && error.empty(); ++r) {
      std::vector<TypingPlace> places;
      if (!BenchTyping(files[i], places, pages, error)) break;
      for (TypingPlace& p : places) {
        auto it = std::find_if(all.begin(), all.end(), [&](const TypingPlace& a) { return a.where == p.where; });
        if (it == all.end()) {
          all.push_back(std::move(p));
          continue;
        }
        it->typing.insert(it->typing.end(), p.typing.begin(), p.typing.end());
        it->backspace.insert(it->backspace.end(), p.backspace.begin(), p.backspace.end());
        it->enter.insert(it->enter.end(), p.enter.begin(), p.enter.end());
      }
    }
    const std::string name = Display(files[i]);
    if (!error.empty()) {
      std::printf("%s  failed: %s\n", Pad(name, width, false).c_str(), error.c_str());
      ++failed;
      continue;
    }
    std::vector<double> keys, everything;
    json += std::string(i ? ",\n" : "\n") + "    {\"document\": \"" + fw::WideToUtf8(files[i].generic_wstring()) +
            "\", \"pages\": " + std::to_string(pages) + ", \"places\": [";
    for (size_t k = 0; k < all.size(); ++k) {
      const TypingPlace& p = all[k];
      std::vector<double> v = p.typing;
      v.insert(v.end(), p.backspace.begin(), p.backspace.end());
      keys.insert(keys.end(), v.begin(), v.end());
      everything.insert(everything.end(), v.begin(), v.end());
      everything.insert(everything.end(), p.enter.begin(), p.enter.end());
      const double worstKey = Quantile(v, 1), enter = Quantile(p.enter, 1);
      std::printf("%s %5s  %-6s %5zu %7s %7s %7s %7s\n", Pad(k ? "" : name, width, false).c_str(),
                  k ? "" : std::to_string(pages).c_str(), p.where.c_str(), v.size(), Ms(Quantile(v, 0.5)).c_str(),
                  Ms(Quantile(v, 0.95)).c_str(), Ms(worstKey).c_str(), Ms(enter).c_str());
      json += std::string(k ? ", " : "") + "{\"where\": \"" + p.where + "\", \"keys\": " + std::to_string(v.size()) +
              ", \"medianMs\": " + Ms(Quantile(v, 0.5)) + ", \"p95Ms\": " + Ms(Quantile(v, 0.95)) +
              ", \"worstMs\": " + Ms(worstKey) + ", \"enterMs\": " + Ms(enter) + "}";
    }
    const double typical = Quantile(keys, 0.5), worst = Quantile(everything, 1);
    const bool missed = typical >= kTypingBudget || worst >= kTypingWorstBudget;
    if (missed) ++over;
    std::printf("%s %5s  typical %s, worst %s: %s\n", Pad("", width, false).c_str(), "", Ms(typical).c_str(),
                Ms(worst).c_str(), missed ? "MISSED" : "met");
    json += "], \"typicalMs\": " + Ms(typical) + ", \"worstMs\": " + Ms(worst) + "}";
  }
  json += "\n  ]\n}\n";
  if (!options.jsonOut.empty()) {
    std::ofstream out(options.jsonOut, std::ios::binary);
    out << json;
    if (!out) std::fprintf(stderr, "Couldn't write %s\n", Display(options.jsonOut).c_str());
  }
  std::printf("\n%zu documents, %d failed, %d over budget\n", files.size(), failed, over);
  if (failed) return 1;
  return options.budgets && over ? 1 : 0;
}

}  // namespace

int RunEditCheck(const Options& options) {
  std::vector<std::filesystem::path> files;
  for (const auto& f : CollectFiles(options.inputs))
    if (IsWordDocument(fw::WideToUtf8(f.filename().wstring()))) files.push_back(f);
  if (files.empty()) {
    std::fprintf(stderr, "No Word documents found.\n");
    return 1;
  }
  std::printf("Laying out again after edits (plan §4): %d random edits per document, the layout compared with one\n"
              "made from scratch every ten.\n\n", options.edits);
  int failed = 0, differ = 0, skipped = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    EditCheck check;
    std::string error;
    const std::string name = Display(files[i]);
    const unsigned seed = options.seed ? static_cast<unsigned>(options.seed) : static_cast<unsigned>(i + 1);
    if (!CheckEdits(files[i], options.edits, seed, check, error)) {
      std::printf("%s: failed: %s\n", name.c_str(), error.c_str());
      ++failed;
    } else if (check.skipped) {
      std::printf("%s: not editable, skipped\n", name.c_str());
      ++skipped;
    } else if (!check.mismatch.empty()) {
      std::printf("%s (seed %u): DIFFERS %s\n", name.c_str(), seed, check.mismatch.c_str());
      ++differ;
    } else {
      std::printf("%s: %d edits, %d checks, the same\n", name.c_str(), check.edits, check.checks);
    }
    std::fflush(stdout);
  }
  std::printf("\n%zu documents: %d differ, %d failed, %d not editable\n", files.size(), differ, failed, skipped);
  return differ || failed ? 1 : 0;
}

int RunBench(const Options& options) {
  std::vector<std::filesystem::path> files;
  for (const auto& f : CollectFiles(options.inputs))
    if (IsWordDocument(fw::WideToUtf8(f.filename().wstring()))) files.push_back(f);
  if (files.empty()) {
    std::fprintf(stderr, "No Word documents found.\n");
    return 1;
  }
  if (options.typing) return RunTypingBench(files, options);
  const int runs = std::max(1, options.runs);
  std::printf("Opening Word documents: the median of %d runs after a warm-up, in ms from the open request.\n", runs);
  if (!HaveLayout())
    std::printf("Featherweight can't lay out Word documents yet (milestone D1): \"xml\" reads Word's parts through "
                "once,\nand the model, first page and pagination wait for the layout hook (tools/fwtool/LayoutHook.cpp).\n");
  std::printf("The budgets (plan §11) apply on the typical consumer PC, not on a fast development PC.\n\n");
  size_t width = 8;
  for (const auto& f : files) width = std::max(width, fw::Utf8ToWide(Display(f)).size());
  std::printf("%s %5s %8s %8s %8s %8s %11s %10s  %s\n", Pad("Document", width, false).c_str(), "pages", "read",
              "package", "xml", "model", "first page", "paginated", "budget (ms)");

  std::string json = "{\n  \"format\": \"featherweight-bench\",\n  \"version\": 1,\n  \"runs\": " +
                     std::to_string(runs) + ",\n  \"layout\": " + (HaveLayout() ? "true" : "false") + ",\n  \"files\": [";
  int failed = 0, over = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    const auto& file = files[i];
    OpenOnce(file);  // warm-up: the file in the cache, the code paged in
    std::vector<Run> results;
    for (int r = 0; r < runs; ++r) results.push_back(OpenOnce(file));
    Run median = results.front();
    for (int s = 0; s < kStages; ++s) {
      std::vector<double> v;
      for (const Run& r : results) v.push_back(r.ms[s]);
      std::sort(v.begin(), v.end());
      median.ms[s] = v[v.size() / 2];
    }
    std::string name = Display(file);
    if (!median.error.empty()) {
      std::printf("%s  failed: %s\n", Pad(name, width, false).c_str(), median.error.c_str());
      ++failed;
      continue;
    }
    double first = FirstPageBudget(median.pages), all = PaginatedBudget(median.pages);
    std::string budget = "first page < " + Ms(first) + (all > 0 ? ", paginated < " + Ms(all) : std::string());
    bool missed = (median.ms[OpenTimings::FirstPage] > first) || (all > 0 && median.ms[OpenTimings::Paginated] > all);
    if (HaveLayout()) budget += missed ? "  MISSED" : "  met";
    if (missed) ++over;
    std::printf("%s %5s %8s %8s %8s %8s %11s %10s  %s\n", Pad(name, width, false).c_str(),
                median.pages ? std::to_string(median.pages).c_str() : "?",
                Ms(median.ms[OpenTimings::Read]).c_str(), Ms(median.ms[OpenTimings::Package]).c_str(),
                Ms(median.ms[kXml]).c_str(), Ms(median.ms[OpenTimings::Model]).c_str(),
                Ms(median.ms[OpenTimings::FirstPage]).c_str(), Ms(median.ms[OpenTimings::Paginated]).c_str(),
                budget.c_str());

    json += i ? ",\n    {\"document\": \"" : "\n    {\"document\": \"";
    json += fw::WideToUtf8(file.generic_wstring()) + "\", \"pages\": " + std::to_string(median.pages) + ", \"ms\": {";
    for (int s = 0; s < kStages; ++s) {
      const char* names[] = {"read", "package", "model", "firstPage", "shown", "paginated", "xml"};
      json += std::string(s ? ", \"" : "\"") + names[s] + "\": " + (median.ms[s] < 0 ? "null" : Ms(median.ms[s]));
    }
    json += "}, \"budget\": {\"firstPage\": " + Ms(first) + ", \"paginated\": " + (all > 0 ? Ms(all) : "null") + "}}";
  }
  json += "\n  ]\n}\n";
  if (!options.jsonOut.empty()) {
    std::ofstream out(options.jsonOut, std::ios::binary);
    out << json;
    if (!out) std::fprintf(stderr, "Couldn't write %s\n", Display(options.jsonOut).c_str());
  }
  std::printf("\n%zu documents, %d failed%s\n", files.size(), failed,
              HaveLayout() ? (", " + std::to_string(over) + " over budget").c_str() : "");
  if (failed) return 1;
  return options.budgets && over ? 1 : 0;
}

}  // namespace fwtool
