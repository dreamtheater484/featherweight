// fwtool bench --sheet-edits: S2's budgets (docs/s2-plan.md, plan §11) for editing workbooks,
// headless: recalculation after one edit in a 100,000-formula workbook (< 100 ms typical), inserting
// a row in a million-cell workbook and undoing it (< 100 ms, the model's part of "to screen"), and
// saving the million-cell workbook (what the UI thread does, and the writing a worker does).
// Keystroke to screen needs the app: bench-app. The workbooks are made from a small template
// through the edit API and the writer, into --out.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Common.h"
#include "core/Strings.h"
#include "pkg/Zip.h"
#include "sheet/calc/Bind.h"
#include "sheet/edit/Edit.h"
#include "sheet/model/Sheet.h"
#include "sheet/xlsx/Xlsx.h"

namespace fwtool {
namespace {

namespace fs = std::filesystem;
namespace sh = fw::sheet;
using Clock = std::chrono::steady_clock;

double Since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

double Quantile(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<size_t>(q * (v.size() - 1) + 0.5))];
}

struct Timed {
  double ui = 0, worker = 0;
};

// Saves through the writer as the app does; `t`: the UI thread's part (prepare, commit) and the
// worker's (writing the bytes).
fw::pkg::Status SaveBook(sh::Workbook& book, sh::edit::Editor& ed, std::vector<uint8_t>& out, Timed* t = nullptr) {
  ed.BeginSave();
  auto t0 = Clock::now();
  sh::xlsx::SavePlan plan;
  fw::pkg::Status st = sh::xlsx::PrepareSave(book, ed, plan);
  const double prepare = Since(t0);
  fw::pkg::MemorySink sink;
  t0 = Clock::now();
  if (st.Ok()) st = sh::xlsx::WriteSave(book, plan, sink);
  const double write = Since(t0);
  if (!st.Ok()) {
    ed.EndSave();
    return st;
  }
  t0 = Clock::now();
  sh::xlsx::CommitSave(book, ed, plan);
  if (t) t->ui = prepare + Since(t0), t->worker = write;
  out = std::move(sink.bytes);
  return {};
}

// A workbook made from `tmpl` through the edit API: `rows` rows of `cols` numbers, and with
// `formulas`, column B = A*2+1 on every row and C1 = SUM(B). Written to `path`.
bool MakeWorkbook(const fs::path& tmpl, const fs::path& path, uint32_t rows, uint32_t cols, bool formulas) {
  fw::pkg::Status st;
  auto book = ReadWorkbook(tmpl, st, true);
  if (!book) {
    std::fprintf(stderr, "Can't read the template %s: %s\n", Display(tmpl).c_str(), st.message.c_str());
    return false;
  }
  sh::edit::Editor ed(*book);
  for (uint32_t first = 0; first < rows; first += 10000) {
    std::vector<std::pair<sh::CellAddr, sh::edit::CellInput>> cells;
    for (uint32_t r = first; r < std::min(rows, first + 10000); ++r)
      for (uint32_t c = 0; c < cols; ++c) {
        if (formulas && c == 1) cells.push_back({{r, c}, sh::edit::CellInput::Formula("A" + std::to_string(r + 1) + "*2+1")});
        else cells.push_back({{r, c}, sh::edit::CellInput::Number(r * 0.5 + c)});
      }
    if (!ed.SetCells(0, cells)) return false;
  }
  if (formulas && !ed.SetCell(0, 0, 2, sh::edit::CellInput::Formula("SUM(B1:B" + std::to_string(rows) + ")"))) return false;
  std::vector<uint8_t> bytes;
  if (!(st = SaveBook(*book, ed, bytes)).Ok()) {
    std::fprintf(stderr, "Saving %s failed: %s\n", Display(path).c_str(), st.message.c_str());
    return false;
  }
  std::ofstream(path, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

void Line(std::string& json, const char* name, const char* what, const std::vector<double>& ms, double budget, int& over) {
  const double typical = Quantile(ms, 0.5), worst = ms.empty() ? 0 : *std::max_element(ms.begin(), ms.end());
  const bool missed = budget > 0 && typical >= budget;
  if (missed) ++over;
  std::printf("  %-44s %9.2f %9.2f  %s\n", what, typical, worst,
              budget > 0 ? (std::string(missed ? "MISSED" : "met") + " (< " + std::to_string(static_cast<int>(budget)) + " ms typical)").c_str() : "");
  json += std::string(json.empty() ? "" : ",\n") + "    \"" + name + "\": {\"typical\": " + std::to_string(typical) +
          ", \"worst\": " + std::to_string(worst) + ", \"budget\": " + (budget > 0 ? std::to_string(budget) : "null") + "}";
}

}  // namespace

int RunSheetEditBench(const Options& options) {
  const fs::path out = options.outDir.empty() ? fs::path("build") / "bench-sheet" : options.outDir;
  const fs::path tmpl = options.inputs.empty() ? fs::path("tests/office/generated/simple.xlsx") : options.inputs.front();
  std::error_code ec;
  fs::create_directories(out, ec);
  const fs::path formulas = out / "formulas-100k.xlsx", million = out / "cells-1m.xlsx";
  auto t0 = Clock::now();
  if (!fs::exists(formulas) && !MakeWorkbook(tmpl, formulas, 100000, 2, true)) return 1;
  if (!fs::exists(million) && !MakeWorkbook(tmpl, million, 100000, 10, false)) return 1;
  std::printf("Workbooks in %s (made in %.0f ms when new). The budgets (plan §11) apply on the typical consumer PC.\n\n",
              Display(out).c_str(), Since(t0));
  const int runs = std::max(3, options.runs);
  std::string json;
  int over = 0;
  std::printf("  %-44s %9s %9s\n", "", "typical", "worst");

  {  // Recalculation after one edit: the edit, the engine told, the dirty formulas calculated.
    fw::pkg::Status st;
    auto book = ReadWorkbook(formulas, st, true);
    if (!book) return std::fprintf(stderr, "%s: %s\n", Display(formulas).c_str(), st.message.c_str()), 1;
    sh::calc::WorkbookCalc calc(*book);
    calc.Load();
    calc.engine().Opened(calc.Settings());
    calc.engine().Recalculate();
    sh::edit::Editor ed(*book);
    std::vector<double> edit, recalc;
    for (int i = 0; i < runs * 4; ++i) {
      const uint32_t row = static_cast<uint32_t>((i * 7919) % 100000);
      auto t = Clock::now();
      ed.SetCell(0, row, 0, sh::edit::CellInput::Number(1000.0 + i));
      edit.push_back(Since(t));
      t = Clock::now();
      calc.CellsEdited(0, {row, 0, row, 0});
      while (!calc.CalcStep(1e9)) {}
      recalc.push_back(Since(t));
    }
    Line(json, "edit", "typing a value (the edit API)", edit, 0, over);
    Line(json, "recalc", "recalculation after it, 100,000 formulas", recalc, 100, over);
  }
  {  // Inserting a row in the million-cell workbook, and undoing it; then saving it.
    fw::pkg::Status st;
    auto book = ReadWorkbook(million, st, true);
    if (!book) return std::fprintf(stderr, "%s: %s\n", Display(million).c_str(), st.message.c_str()), 1;
    sh::edit::Editor ed(*book);
    std::vector<double> insert, undo;
    std::string refused;
    for (int i = 0; i < runs && refused.empty(); ++i) {
      auto t = Clock::now();
      sh::edit::Result r = ed.InsertRows(0, 50000, 1);
      if (!r.ok) {
        refused = fw::WideToUtf8(std::wstring(r.message.begin(), r.message.end()));
        break;
      }
      insert.push_back(Since(t));
      t = Clock::now();
      ed.Undo(1);
      undo.push_back(Since(t));
    }
    if (refused.empty()) {
      Line(json, "insertRow", "inserting a row, 1,000,000 cells", insert, 100, over);
      Line(json, "undoInsertRow", "undoing it", undo, 100, over);
    } else {
      std::printf("  %-44s not measured: %s\n", "inserting a row, 1,000,000 cells", refused.c_str());
    }
    std::vector<double> ui, worker;
    for (int i = 0; i < runs; ++i) {
      ed.SetCell(0, static_cast<uint32_t>(i), 0, sh::edit::CellInput::Number(7.0 + i));
      std::vector<uint8_t> bytes;
      Timed t;
      if (!(st = SaveBook(*book, ed, bytes, &t)).Ok()) return std::fprintf(stderr, "saving: %s\n", st.message.c_str()), 1;
      ui.push_back(t.ui), worker.push_back(t.worker);
    }
    Line(json, "saveUi", "saving it: the UI thread (prepare, commit)", ui, 100, over);
    Line(json, "saveWorker", "saving it: the worker (writing the file)", worker, 0, over);
  }
  std::printf("\nKeystroke to screen in a cell: bench-app (the app draws it). %d over budget.\n", over);
  if (!options.jsonOut.empty()) std::ofstream(options.jsonOut, std::ios::trunc) << "{\n" << json << "\n}\n";
  return options.budgets && over ? 1 : 0;
}

}  // namespace fwtool
