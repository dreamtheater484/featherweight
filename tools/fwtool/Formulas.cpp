// fwtool formulas: recalculates every formula of each workbook and compares the results with the
// values Excel cached in the file (docs/s1-plan.md, the formulas exit criterion).
//
// Counted: formulas the engine calculates (only implemented functions) that don't depend on a
// volatile one (NOW, TODAY, RAND, OFFSET, INDIRECT...) and have a cached value. Numbers match to
// 15 significant digits, as Excel shows and compares them; text, booleans and errors exactly.
// --locale en-NL: the region the files were calculated in (this PC's Excel; any Windows locale
// name). --regions en-US,de-DE: a workbook with mismatches is also calculated in each of these, and
// counts in the one where most of its formulas match: TEXT() codes and text read as numbers follow
// the region of the Excel that saved the file, which a file doesn't record (the report lists which
// files were read in another region). --report <file.md> lists every mismatch, each workbook's
// application (docProps/app.xml), and what was kept as cached.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Common.h"
#include "core/Strings.h"
#include "pkg/Package.h"
#include "sheet/calc/Bind.h"
#include "sheet/calc/Functions.h"
#include "sheet/model/Workbook.h"
#include "sheet/numfmt/NumLocale.h"

namespace fwtool {
namespace {

using namespace fw::sheet;

struct Saved {
  int sheet;
  uint32_t row, col;
  ValueType type;
  double number;
  bool boolean;
  ErrorCode error;
  std::string text;
  std::string formula;
  CellAddr origin;  // the cell the formula's text is written for (a shared formula's master)
};

// At most `n` bytes of UTF-8 text, cut at a character boundary.
std::string Clip(std::string_view s, size_t n) {
  if (s.size() <= n) return std::string(s);
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  return std::string(s.substr(0, n)) + "...";
}

// Text for a Markdown table cell: no line breaks, `|` escaped.
std::string Cell(std::string_view s) {
  std::string out;
  for (char c : s) {
    if (c == '\r') continue;
    if (c == '\n') out += ' ';
    else if (c == '|') out += "\\|";
    else out += c;
  }
  return out;
}

std::string Show(ValueType t, double n, bool b, ErrorCode e, std::string_view text) {
  switch (t) {
    case ValueType::Number: {
      char buf[40];
      std::snprintf(buf, sizeof buf, "%.15g", n);
      return buf;
    }
    case ValueType::String: return "\"" + Clip(text, 80) + "\"";
    case ValueType::Bool: return b ? "TRUE" : "FALSE";
    case ValueType::Error: return std::string(ErrorText(e));
    default: return "(empty)";
  }
}

bool Same(const Saved& s, const Value& v) {
  if (s.type != v.type) {
    // A formula giving "" can be cached as nothing.
    return (s.type == ValueType::Empty && v.type == ValueType::String && v.text.empty());
  }
  switch (s.type) {
    case ValueType::Number: {
      if (s.number == v.number) return true;
      double d = std::fabs(s.number - v.number), m = std::max(std::fabs(s.number), std::fabs(v.number));
      return d <= m * 5e-15 || calc::Round15(s.number) == calc::Round15(v.number);
    }
    case ValueType::String: return s.text == v.text;
    case ValueType::Bool: return s.boolean == v.boolean;
    case ValueType::Error: return s.error == v.error;
    default: return true;
  }
}

// The functions a formula calls that the engine doesn't implement.
void Unimplemented(std::string_view f, std::map<std::string, size_t>& out) {
  for (size_t i = 0; i < f.size(); ++i) {
    if (f[i] == '"') {
      size_t j = f.find('"', i + 1);
      while (j != std::string_view::npos && j + 1 < f.size() && f[j + 1] == '"') j = f.find('"', j + 2);
      if (j == std::string_view::npos) return;
      i = j;
      continue;
    }
    if (f[i] != '(' || i == 0) continue;
    size_t s = i;
    while (s > 0 && (isalnum(static_cast<unsigned char>(f[s - 1])) || f[s - 1] == '.' || f[s - 1] == '_')) --s;
    std::string name(f.substr(s, i - s));
    for (const char* p : {"_xlfn.", "_xlws.", "_xlfn.", "_xlws."})
      if (name.rfind(p, 0) == 0) name.erase(0, 6);
    if (name.empty()) continue;
    int id = calc::FindFunction(name);
    for (char& c : name) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    if (id < 0 || !calc::Function(id).impl) {
      if (name == "IF" || name == "IFERROR" || name == "IFNA" || name == "CHOOSE" || name == "IFS" || name == "SWITCH" ||
          name == "SINGLE" || name == "ANCHORARRAY" || name == "LET")
        continue;
      ++out[name];
    }
  }
}

// The locale for a region name: the fixed en-US and en-NL ones, else Windows' values for it.
const NumLocale* LocaleFor(const std::wstring& name) {
  if (name.empty() || name == L"en-US") return &EnUsLocale();
  if (name == L"en-NL") return &EnNlLocale();
  static std::map<std::wstring, std::unique_ptr<NumLocale>> made;
  auto& p = made[name];
  if (!p) {
    if (LcidFromName(name) == 0) return nullptr;
    p = std::make_unique<NumLocale>(MakeLocale(ReadWinLocale(name)));
  }
  return p.get();
}

// The application that saved the file, from docProps/app.xml ("Microsoft Excel 16.0300").
std::string SavedBy(const std::filesystem::path& file) {
  fw::pkg::Status st;
  auto pkg = OpenPackage(file, st);
  std::vector<uint8_t> bytes;
  if (!pkg || !pkg->FindPart("/docProps/app.xml") || !pkg->ReadPart("/docProps/app.xml", bytes).Ok()) return "?";
  std::string_view x = AsText(bytes);
  auto element = [&](std::string_view tag) {
    std::string open = "<" + std::string(tag) + ">", close = "</" + std::string(tag) + ">";
    size_t a = x.find(open), b = a == std::string_view::npos ? a : x.find(close, a);
    return b == std::string_view::npos ? std::string() : std::string(x.substr(a + open.size(), b - a - open.size()));
  };
  std::string app = element("Application"), version = element("AppVersion");
  if (app.empty()) return "?";
  return Clip(version.empty() ? app : app + " " + version, 60);
}

// One workbook calculated in one region.
struct Run {
  size_t counted = 0, matched = 0;
  std::vector<std::string> mismatches;  // report rows
};

// The formulas of a workbook as read, with their cached values.
std::vector<Saved> ReadSaved(const Workbook& book) {
  std::vector<Saved> saved;
  for (size_t i = 0; i < book.SheetCount(); ++i) {
    const Sheet& s = book.GetSheet(i);
    s.ForEachCell(Range::All(), [&](const CellInfo& c) {
      if (!c.hasFormula) return true;
      FormulaInfo fi;
      s.GetFormula(c.row, c.col, fi);
      saved.push_back({static_cast<int>(i), c.row, c.col, c.value.type, c.value.number, c.value.boolean, c.value.error,
                       std::string(c.value.text), std::string(fi.text), fi.origin});
      return true;
    });
  }
  return saved;
}

// The formula as the report shows it: a shared formula's text says which cell it is written for.
std::string ShownFormula(const Saved& s) {
  std::string f = "`=" + Cell(Clip(s.formula, 120)) + "`";
  if (s.origin.row != s.row || s.origin.col != s.col) f += " (shared from " + CellName(s.origin.row, s.origin.col) + ")";
  return f;
}

// Recalculates `book` in `locale` and compares with `saved`. On the first run (`totals` set) it
// also counts what isn't compared and lists what is kept as cached.
struct Totals {
  size_t formulas = 0, volatileOnes = 0, unsupported = 0, uncached = 0;
  std::map<std::string, size_t> missing;
  std::vector<std::string> kept;
};
Run Check(Workbook& book, const std::vector<Saved>& saved, const NumLocale* locale, const std::string& fileName,
          Totals* totals, bool& circular) {
  Run run;
  calc::WorkbookCalc calc(book, locale);
  calc.Load();
  calc::Engine& e = calc.engine();
  // What Excel recalculates on open (volatile functions and their dependents, formulas without
  // a cached value): not comparable with the cached values.
  e.Opened({});
  std::vector<char> excluded(saved.size());
  for (size_t k = 0; k < saved.size(); ++k)
    excluded[k] = e.Dirty(saved[k].sheet, static_cast<int32_t>(saved[k].row), static_cast<int32_t>(saved[k].col));
  e.MarkAllDirty();
  e.Recalculate();
  circular = e.Circular();
  for (size_t k = 0; k < saved.size(); ++k) {
    const Saved& s = saved[k];
    if (totals) ++totals->formulas;
    int32_t r = static_cast<int32_t>(s.row), c = static_cast<int32_t>(s.col);
    if (s.type == ValueType::Empty) {
      if (totals) ++totals->uncached;
      continue;
    }
    const std::string where = fileName + " | " + Cell(book.GetSheet(static_cast<size_t>(s.sheet)).Name()) + "!" +
                              CellName(s.row, s.col) + " | " + ShownFormula(s);
    if (!e.HasFormula(s.sheet, r, c) || e.Stale(s.sheet, r, c)) {
      if (totals) {
        ++totals->unsupported;
        Unimplemented(s.formula, totals->missing);
        totals->kept.push_back(where);
      }
      continue;
    }
    if (excluded[k]) {
      if (totals) ++totals->volatileOnes;
      continue;
    }
    ++run.counted;
    Value now = book.GetSheet(static_cast<size_t>(s.sheet)).GetValue(s.row, s.col);
    if (Same(s, now)) {
      ++run.matched;
      continue;
    }
    run.mismatches.push_back(where + " | " + Cell(Show(s.type, s.number, s.boolean, s.error, s.text)) + " | " +
                             Cell(Show(now.type, now.number, now.boolean, now.error, now.text)));
  }
  return run;
}

}  // namespace

int RunFormulas(const Options& options) {
  const std::wstring region = options.locale.empty() ? L"en-US" : options.locale;
  const NumLocale* locale = LocaleFor(region);
  if (!locale) {
    std::printf("formulas: Windows doesn't know the region %ls\n", region.c_str());
    return 2;
  }
  std::vector<std::pair<std::wstring, const NumLocale*>> others;
  for (const auto& r : options.regions) {
    const NumLocale* l = LocaleFor(r);
    if (!l) {
      std::printf("formulas: Windows doesn't know the region %ls\n", r.c_str());
      return 2;
    }
    if (r != region) others.emplace_back(r, l);
  }
  std::vector<std::filesystem::path> files;
  for (const auto& f : CollectFiles(options.inputs))
    if (IsWorkbookFile(f)) files.push_back(f);
  size_t counted = 0, matched = 0, failed = 0;
  Totals totals;
  std::vector<std::string> mismatches, regionRows, fileRows;
  for (const auto& file : files) {
    fw::pkg::Status status;
    auto book = ReadWorkbook(file, status, true);
    if (!book) {
      std::printf("%ls: not read (%s)\n", file.filename().c_str(), status.message.c_str());
      ++failed;
      continue;
    }
    const std::string name = Cell(fw::WideToUtf8(file.filename().wstring()));
    const std::vector<Saved> saved = ReadSaved(*book);
    bool circular = false;
    Run run = Check(*book, saved, locale, name, &totals, circular);
    std::wstring used = region;
    if (!run.mismatches.empty()) {
      // TEXT() codes and text read as numbers follow the saving Excel's region: try the others.
      const size_t first = run.matched;
      for (const auto& [r, l] : others) {
        auto again = ReadWorkbook(file, status, true);
        bool circ = false;
        if (!again) continue;
        Run other = Check(*again, saved, l, name, nullptr, circ);
        if (other.matched > run.matched) run = std::move(other), used = r;
      }
      if (used != region)
        regionRows.push_back(name + " | " + Cell(SavedBy(file)) + " | " + fw::WideToUtf8(used) + " | " +
                             std::to_string(first) + " | " + std::to_string(run.matched) + " of " +
                             std::to_string(run.counted));
    }
    counted += run.counted, matched += run.matched;
    if (!run.mismatches.empty())
      fileRows.push_back(name + " | " + Cell(SavedBy(file)) + " | " + fw::WideToUtf8(used) + " | " +
                         std::to_string(run.mismatches.size()) + " of " + std::to_string(run.counted));
    for (auto& m : run.mismatches) mismatches.push_back(std::move(m));
    std::printf("%ls: %zu formulas, %zu of %zu compared match%s%s%ls\n", file.filename().c_str(), saved.size(),
                run.matched, run.counted, circular ? " (circular references)" : "", used != region ? ", read in " : "",
                used != region ? used.c_str() : L"");
  }
  double pct = counted ? 100.0 * static_cast<double>(matched) / static_cast<double>(counted) : 100.0;
  std::printf("\n%zu workbooks (%zu not read), %zu formulas: %zu compared, %zu match (%.2f %%); not compared: %zu "
              "unsupported, %zu volatile or depending on one, %zu without a cached value\n",
              files.size(), failed, totals.formulas, counted, matched, pct, totals.unsupported, totals.volatileOnes,
              totals.uncached);
  if (!regionRows.empty()) std::printf("%zu workbooks read in another region\n", regionRows.size());
  std::vector<std::pair<size_t, std::string>> byUse;
  for (const auto& [fn, n] : totals.missing) byUse.emplace_back(n, fn);
  std::sort(byUse.rbegin(), byUse.rend());
  if (!byUse.empty()) {
    std::printf("Not implemented, by formulas using it:");
    for (size_t i = 0; i < byUse.size() && i < 25; ++i)
      std::printf(" %s %zu%s", byUse[i].second.c_str(), byUse[i].first, i + 1 < byUse.size() && i < 24 ? "," : "");
    std::printf("\n");
  }
  for (size_t i = 0; i < mismatches.size() && i < 20; ++i) std::printf("  %s\n", mismatches[i].c_str());
  if (!options.reportOut.empty()) {
    std::ofstream out(options.reportOut, std::ios::binary);
    char share[32];
    std::snprintf(share, sizeof share, "%.2f", pct);
    out << "# fwtool formulas\n\n"
        << files.size() << " workbooks, " << totals.formulas << " formulas: " << counted << " compared, " << matched
        << " match (" << share << " %), calculated in " << fw::WideToUtf8(region);
    if (!others.empty()) {
      out << " (or, when more of a workbook's formulas match there, in";
      for (size_t i = 0; i < others.size(); ++i) out << (i ? ", " : " ") << fw::WideToUtf8(others[i].first);
      out << ")";
    }
    out << ". Not compared: " << totals.unsupported << " kept as cached, " << totals.volatileOnes
        << " volatile or depending on one, " << totals.uncached << " without a cached value.\n";
    out << "\n## Read in another region\n\nFile | Saved by | Region | Match in " << fw::WideToUtf8(region)
        << " | Match there\n---|---|---|---|---\n";
    for (const auto& r : regionRows) out << r << "\n";
    out << "\n## Workbooks with mismatches\n\nFile | Saved by | Region | Mismatches\n---|---|---|---\n";
    for (const auto& r : fileRows) out << r << "\n";
    out << "\n## Mismatches\n\nFile | Cell | Formula | Excel | Featherweight\n---|---|---|---|---\n";
    for (const auto& m : mismatches) out << m << "\n";
    out << "\n## Not implemented\n\nFunction | Formulas\n---|---\n";
    for (const auto& [n, fn] : byUse) out << fn << " | " << n << "\n";
    out << "\n## Kept as cached (not recalculated)\n\nFile | Cell | Formula\n---|---|---\n";
    for (const auto& k : totals.kept) out << k << "\n";
  }
  if (options.minMatch >= 0 && pct < options.minMatch) {
    std::printf("FAIL: %.2f %% of compared formulas match, below %.2f %%\n", pct, options.minMatch);
    return 1;
  }
  return failed ? 1 : 0;
}

}  // namespace fwtool
