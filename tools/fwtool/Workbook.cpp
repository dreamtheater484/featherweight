// fwtool for workbooks (S1): dump --model and check --model read each workbook into the model
// (src/sheet); roundtrip --model writes back the parts the model owns (Roundtrip.cpp).
#include <chrono>
#include <cstdio>
#include <string>

#include "Common.h"
#include "core/Strings.h"
#include "pkg/Check.h"
#include "pkg/Snapshot.h"
#include "sheet/ods/Ods.h"
#include "sheet/xlsx/Xlsx.h"

namespace fwtool {
namespace {

using fw::sheet::CellInfo;
using fw::sheet::FormulaInfo;
using fw::sheet::Sheet;
using fw::sheet::ValueType;
using fw::sheet::Workbook;

std::string ValueText(const fw::sheet::Value& v) {
  switch (v.type) {
    case ValueType::Empty: return "(empty)";
    case ValueType::Number: {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%.17g", v.number);
      return buf;
    }
    case ValueType::String: return "\"" + std::string(v.text.substr(0, 60)) + (v.text.size() > 60 ? "...\"" : "\"");
    case ValueType::Bool: return v.boolean ? "TRUE" : "FALSE";
    case ValueType::Error: return std::string(fw::sheet::ErrorText(v.error));
  }
  return {};
}

const char* KindName(fw::sheet::SheetKind k) {
  switch (k) {
    case fw::sheet::SheetKind::Worksheet: return "worksheet";
    case fw::sheet::SheetKind::Chartsheet: return "chart sheet";
    case fw::sheet::SheetKind::Dialogsheet: return "dialog sheet";
    case fw::sheet::SheetKind::Macrosheet: return "macro sheet";
  }
  return "";
}

}  // namespace

bool IsWorkbookFile(const std::filesystem::path& file) {
  std::wstring ext = fw::ToLower(file.extension().wstring());
  return ext == L".xlsx" || ext == L".xlsm" || ext == L".xltx" || ext == L".xltm" || ext == L".ods" || ext == L".ots";
}

std::unique_ptr<Workbook> ReadWorkbook(const std::filesystem::path& file, fw::pkg::Status& status, bool loadAll) {
  std::unique_ptr<fw::pkg::Package> pkg;
  const std::wstring ext = fw::ToLower(file.extension().wstring());
  if (ext == L".ods" || ext == L".ots") {  // S4: converted as the app converts it
    auto source = fw::pkg::Snapshot::Open(file.wstring(), {}, status);
    std::vector<uint8_t> xlsx;
    if (!source || !(status = fw::sheet::ods::OdsToXlsx(source->Bytes(), xlsx)).Ok()) return nullptr;
    pkg = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(std::move(xlsx)), fw::pkg::Limits::Default(), status);
  } else {
    pkg = OpenPackage(file, status);
  }
  if (!pkg) return nullptr;
  fw::sheet::xlsx::ReadOptions options;
  options.loadAll = loadAll;
  return fw::sheet::xlsx::Read(std::move(pkg), status, options);
}

std::vector<std::string> CheckWorkbookModel(const Workbook& book) {
  std::vector<std::string> errors;
  const auto& styles = book.GetStyles();
  for (size_t i = 0; i < book.SheetCount(); ++i) {
    const Sheet& s = book.GetSheet(i);
    if (!s.Loaded()) {
      errors.push_back(s.Name() + ": not loaded");
      continue;
    }
    size_t badStyles = 0, badStrings = 0, orphans = 0, cells = 0;
    s.ForEachCell(fw::sheet::Range::All(), [&](const CellInfo& c) {
      ++cells;
      if (c.style >= styles.CellXfs().size() && !(c.style == 0 && styles.CellXfs().empty())) ++badStyles;
      if (c.value.type == ValueType::String && c.sharedString != fw::sheet::kNone && c.sharedString >= book.Strings().Size())
        ++badStrings;
      FormulaInfo f;
      if (c.hasFormula && s.GetFormula(c.row, c.col, f) && f.kind == fw::sheet::FormulaKind::Shared && f.text.empty())
        ++orphans;
      return true;
    });
    if (cells != s.CellCount()) errors.push_back(s.Name() + ": " + std::to_string(cells) + " cells walked, " +
                                                 std::to_string(s.CellCount()) + " stored");
    if (badStyles) errors.push_back(s.Name() + ": " + std::to_string(badStyles) + " cells with a format the styles don't have");
    if (badStrings) errors.push_back(s.Name() + ": " + std::to_string(badStrings) + " cells with a missing shared string");
    if (orphans) errors.push_back(s.Name() + ": " + std::to_string(orphans) + " cells share a formula with no master");
    const auto& merges = s.Merges();
    for (size_t m = 1; m < merges.size(); ++m)
      if (merges[m].firstRow < merges[m - 1].firstRow) errors.push_back(s.Name() + ": merges out of order");
  }
  return errors;
}

void DumpWorkbook(const std::filesystem::path& file) {
  using Clock = std::chrono::steady_clock;
  auto t0 = Clock::now();
  fw::pkg::Status st;
  auto book = ReadWorkbook(file, st, false);
  auto t1 = Clock::now();
  std::printf("%s\n", Display(file).c_str());
  if (!book) {
    std::printf("  refused: %s\n\n", st.message.c_str());
    return;
  }
  st = book->LoadAll();
  auto t2 = Clock::now();
  auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
  std::printf("  opened in %.1f ms (active sheet), all sheets in %.1f ms more%s%s\n", ms(t0, t1), ms(t1, t2),
              st.Ok() ? "" : "; ", st.message.c_str());
  std::printf("  %zu sheets, active %u, date system %s, calculation %s%s; %u shared strings, %zu cell formats, "
              "%zu fonts, %zu fills, %zu borders, %zu custom number formats; model %zu KB\n",
              book->SheetCount(), book->ActiveSheet(), book->Date1904() ? "1904" : "1900",
              book->Calc().mode == fw::sheet::CalcMode::Manual ? "manual" : "automatic",
              book->Calc().fullCalcOnLoad ? " (full on load)" : "", book->Strings().Size(),
              book->GetStyles().CellXfs().size(), book->GetStyles().Fonts().size(), book->GetStyles().Fills().size(),
              book->GetStyles().Borders().size(), book->GetStyles().CustomFormats().size(), book->ModelBytes() / 1024);
  for (const auto& n : book->Names())
    std::printf("  name %s%s%s = %s\n", n.name.c_str(), n.sheet >= 0 ? (" (sheet " + std::to_string(n.sheet) + ")").c_str() : "",
                n.hidden ? " (hidden)" : "", n.formula.c_str());
  for (size_t i = 0; i < book->SheetCount(); ++i) {
    const Sheet& s = book->GetSheet(i);
    auto used = s.UsedRange();
    std::printf("\n  sheet %zu \"%s\" (%s%s) %s: %zu cells, %zu formulas, used %s, %zu merges, %zu column ranges, %zu links\n", i,
                s.Name().c_str(), KindName(s.Kind()),
                s.State() == fw::sheet::SheetState::Visible ? "" : s.State() == fw::sheet::SheetState::Hidden ? ", hidden" : ", very hidden",
                s.PartName().c_str(), s.CellCount(), s.FormulaCount(), used ? fw::sheet::RangeName(*used).c_str() : "-",
                s.Merges().size(), s.Columns().size(), s.Hyperlinks().size());
    for (const auto& v : s.Views())
      if (v.paneState != fw::sheet::PaneState::None)
        std::printf("    pane: %s x %g y %g, top left %s\n", v.paneState == fw::sheet::PaneState::Frozen ? "frozen" : "split",
                    v.xSplit, v.ySplit, fw::sheet::CellName(v.paneTopLeft.row, v.paneTopLeft.col).c_str());
    for (const auto& m : s.Merges()) std::printf("    merge %s\n", fw::sheet::RangeName(m).c_str());
    int shown = 0;
    s.ForEachCell(fw::sheet::Range::All(), [&](const CellInfo& c) {
      if (++shown > 40) return false;
      std::printf("    %-8s s%-4u %s", fw::sheet::CellName(c.row, c.col).c_str(), c.style, ValueText(c.value).c_str());
      FormulaInfo f;
      if (c.hasFormula && s.GetFormula(c.row, c.col, f))
        std::printf("  = %.*s%s", static_cast<int>(std::min<size_t>(f.text.size(), 80)), f.text.data(),
                    f.kind == fw::sheet::FormulaKind::Shared && !f.master ? " (shared)" : "");
      std::printf("\n");
      return true;
    });
  }
  std::printf("\n");
}

}  // namespace fwtool
