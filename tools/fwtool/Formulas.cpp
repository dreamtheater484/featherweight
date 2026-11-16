// fwtool formulas: recalculates every formula of each workbook and compares the results with the
// values Excel cached in the file (docs/s1-plan.md, the formulas exit criterion).
//
// Counted: formulas the engine calculates (only implemented functions) that don't depend on a
// volatile one (NOW, TODAY, RAND, OFFSET, INDIRECT...) and have a cached value. Numbers match to
// 15 significant digits, as Excel shows and compares them; text, booleans and errors exactly.
// --report <file.md> lists every mismatch and the unimplemented functions by use.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "Common.h"
#include "core/Strings.h"
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
};

std::string Show(ValueType t, double n, bool b, ErrorCode e, std::string_view text) {
  switch (t) {
    case ValueType::Number: {
      char buf[40];
      std::snprintf(buf, sizeof buf, "%.15g", n);
      return buf;
    }
    case ValueType::String: return "\"" + std::string(text.substr(0, 80)) + "\"";
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
          name == "SINGLE" || name == "ANCHORARRAY")
        continue;
      ++out[name];
    }
  }
}

}  // namespace

int RunFormulas(const Options& options) {
  std::vector<std::filesystem::path> files;
  for (const auto& f : CollectFiles(options.inputs))
    if (IsWorkbookFile(f)) files.push_back(f);
  size_t formulas = 0, counted = 0, matched = 0, volatileOnes = 0, unsupported = 0, uncached = 0, failed = 0;
  std::map<std::string, size_t> missing;
  std::vector<std::string> mismatches;
  for (const auto& file : files) {
    fw::pkg::Status status;
    auto book = ReadWorkbook(file, status, true);
    if (!book) {
      std::printf("%ls: not read (%s)\n", file.filename().c_str(), status.message.c_str());
      ++failed;
      continue;
    }
    std::vector<Saved> saved;
    for (size_t i = 0; i < book->SheetCount(); ++i) {
      const Sheet& s = book->GetSheet(i);
      s.ForEachCell(Range::All(), [&](const CellInfo& c) {
        if (!c.hasFormula) return true;
        FormulaInfo fi;
        s.GetFormula(c.row, c.col, fi);
        saved.push_back({static_cast<int>(i), c.row, c.col, c.value.type, c.value.number, c.value.boolean,
                         c.value.error, std::string(c.value.text), std::string(fi.text)});
        return true;
      });
    }
    calc::WorkbookCalc calc(*book, &EnUsLocale());
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
    size_t fileCounted = 0, fileMatched = 0;
    for (size_t k = 0; k < saved.size(); ++k) {
      const Saved& s = saved[k];
      ++formulas;
      int32_t r = static_cast<int32_t>(s.row), c = static_cast<int32_t>(s.col);
      if (s.type == ValueType::Empty) {
        ++uncached;
        continue;
      }
      if (!e.HasFormula(s.sheet, r, c) || e.Stale(s.sheet, r, c)) {
        ++unsupported;
        Unimplemented(s.formula, missing);
        continue;
      }
      if (excluded[k]) {
        ++volatileOnes;
        continue;
      }
      ++counted, ++fileCounted;
      Value now = book->GetSheet(static_cast<size_t>(s.sheet)).GetValue(s.row, s.col);
      if (Same(s, now)) {
        ++matched, ++fileMatched;
        continue;
      }
      std::string line = fw::WideToUtf8(file.filename().wstring()) + " | " + book->GetSheet(static_cast<size_t>(s.sheet)).Name() +
                         "!" + CellName(s.row, s.col) + " | `=" + s.formula.substr(0, 120) + "` | " +
                         Show(s.type, s.number, s.boolean, s.error, s.text) + " | " +
                         Show(now.type, now.number, now.boolean, now.error, now.text);
      mismatches.push_back(line);
    }
    std::printf("%ls: %zu formulas, %zu of %zu compared match%s\n", file.filename().c_str(), saved.size(), fileMatched,
                fileCounted, e.Circular() ? " (circular references)" : "");
  }
  double pct = counted ? 100.0 * static_cast<double>(matched) / static_cast<double>(counted) : 100.0;
  std::printf("\n%zu workbooks (%zu not read), %zu formulas: %zu compared, %zu match (%.2f %%); not compared: %zu "
              "unsupported, %zu volatile or depending on one, %zu without a cached value\n",
              files.size(), failed, formulas, counted, matched, pct, unsupported, volatileOnes, uncached);
  std::vector<std::pair<size_t, std::string>> byUse;
  for (const auto& [name, n] : missing) byUse.emplace_back(n, name);
  std::sort(byUse.rbegin(), byUse.rend());
  if (!byUse.empty()) {
    std::printf("Not implemented, by formulas using it:");
    for (size_t i = 0; i < byUse.size() && i < 25; ++i) std::printf(" %s %zu%s", byUse[i].second.c_str(), byUse[i].first, i + 1 < byUse.size() && i < 24 ? "," : "");
    std::printf("\n");
  }
  for (size_t i = 0; i < mismatches.size() && i < 20; ++i) std::printf("  %s\n", mismatches[i].c_str());
  if (!options.reportOut.empty()) {
    std::ofstream out(options.reportOut, std::ios::binary);
    out << "# fwtool formulas\n\n" << files.size() << " workbooks, " << formulas << " formulas: " << counted
        << " compared, " << matched << " match (" << pct << " %).\n\n## Mismatches\n\nFile | Cell | Formula | Excel | Featherweight\n---|---|---|---|---\n";
    for (const auto& m : mismatches) out << m << "\n";
    out << "\n## Not implemented\n\nFunction | Formulas\n---|---\n";
    for (const auto& [n, name] : byUse) out << name << " | " << n << "\n";
  }
  return failed ? 1 : 0;
}

}  // namespace fwtool
