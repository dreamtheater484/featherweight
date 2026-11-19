// fwtool bench --sheet-edits: S2's budgets (docs/s2-plan.md, plan §11) for editing workbooks,
// headless: recalculation after one edit in a 100,000-formula workbook (< 100 ms typical), inserting
// a row in a million-cell workbook and undoing it (< 100 ms, the model's part of "to screen"), and
// saving the million-cell workbook (what the UI thread does, and the writing a worker does), and
// keystroke to screen in a cell (< 16 ms typical, < 50 ms worst), headless as `bench --typing` is
// for Word: the cell editor takes the key and the cells of a 1400 x 900 window are painted again
// on the graphics adapter; Enter adds the edit and the recalculation. The workbooks are made from
// a small template through the edit API and the writer, into --out.
#include <windows.h>

#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Common.h"
#include "core/Strings.h"
#include "draw/D2DDevice.h"
#include "draw/D2DPainter.h"
#include "pkg/Zip.h"
#include "sheet/calc/Bind.h"
#include "sheet/condfmt/CondFormat.h"
#include "sheet/data/Rules.h"
#include "sheet/edit/Edit.h"
#include "sheet/model/Sheet.h"
#include "sheet/objects/SheetObjects.h"
#include "sheet/numfmt/NumLocale.h"
#include "sheet/view/CellEditor.h"
#include "sheet/view/CellPaint.h"
#include "sheet/view/CellText.h"
#include "sheet/view/Grid.h"
#include "sheet/xlsx/Xlsx.h"
#include "text/Fonts.h"

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

// One timing: typical (the median) and worst, against a budget for the typical and one for the worst
// (0: none).
void Line(std::string& json, const char* name, const char* what, const std::vector<double>& ms, double budget, int& over,
          double worstBudget = 0) {
  const double typical = Quantile(ms, 0.5), worst = ms.empty() ? 0 : *std::max_element(ms.begin(), ms.end());
  const bool missed = (budget > 0 && typical >= budget) || (worstBudget > 0 && worst >= worstBudget);
  if (missed) ++over;
  std::string verdict;
  if (budget > 0 || worstBudget > 0) {
    verdict = std::string(missed ? "MISSED" : "met") + " (<";
    if (budget > 0) verdict += " " + std::to_string(static_cast<int>(budget)) + " ms typical";
    if (worstBudget > 0) verdict += std::string(budget > 0 ? "," : "") + " " + std::to_string(static_cast<int>(worstBudget)) + " ms worst";
    verdict += ")";
  }
  std::printf("  %-52s %9.2f %9.2f  %s\n", what, typical, worst, verdict.c_str());
  json += std::string(json.empty() ? "" : ",\n") + "    \"" + name + "\": {\"typical\": " + std::to_string(typical) +
          ", \"worst\": " + std::to_string(worst) + ", \"budget\": " + (budget > 0 ? std::to_string(budget) : "null") +
          ", \"worstBudget\": " + (worstBudget > 0 ? std::to_string(worstBudget) : "null") + "}";
}

// Cells on a screen at 100 % on a 96 dpi display, as the workbook tab places them: the row headers
// and column headers take `left` and `top`.
class ScreenCells : public fw::sheet::view::Geometry {
 public:
  ScreenCells(const fw::sheet::view::Grid& grid, float left, float top) : grid_(grid), left_(left), top_(top) {
    pt = 4.f / 3;
    px = 1;
    line = 1;
    rowPx = 1;
    padStart = padEnd = 2;
  }
  float Left(uint32_t c) const override { return left_ + static_cast<float>(grid_.ColLeft(c)); }
  float Right(uint32_t c) const override { return Left(c + 1); }
  float Top(uint32_t r) const override { return top_ + static_cast<float>(grid_.RowTop(r)); }
  float Bottom(uint32_t r) const override { return Top(r + 1); }

 private:
  const fw::sheet::view::Grid& grid_;
  float left_, top_;
};

// A window's worth of pixels on the graphics adapter, painted as the tab paints its cells.
class Screen {
 public:
  bool Init(UINT32 w, UINT32 h) {
    D2D1_FACTORY_OPTIONS opts{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                 reinterpret_cast<void**>(factory_.GetAddressOf()))))
      return false;
    if (FAILED(draw::CreateD2DDevice(factory_.Get(), false, d3d_, d2d_)) &&
        FAILED(draw::CreateD2DDevice(factory_.Get(), true, d3d_, d2d_)))
      return false;
    if (FAILED(d2d_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return false;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(dc_->CreateBitmap({w, h}, nullptr, 0, props, &target_))) return false;
    dc_->SetTarget(target_.Get());
    return true;
  }
  template <typename F>
  void Paint(F&& draw) {
    dc_->BeginDraw();
    dc_->Clear(D2D1::ColorF(1, 1, 1, 1));
    {
      draw::D2DPainter painter(dc_.Get());
      draw(painter);
    }
    dc_->EndDraw();
  }

 private:
  Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
  Microsoft::WRL::ComPtr<ID3D11Device> d3d_;
  Microsoft::WRL::ComPtr<ID2D1Device> d2d_;
  Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc_;
  Microsoft::WRL::ComPtr<ID2D1Bitmap1> target_;
};

// Sheet points to the screen's pixels, as the tab places objects (100 %, 96 dpi).
class ScreenLayout : public sh::SheetLayout {
 public:
  explicit ScreenLayout(const sh::view::Grid& grid) : grid_(grid) {}
  double ColumnLeft(uint32_t col) const override { return grid_.ColLeft(col) * 0.75; }
  double RowTop(uint32_t row) const override { return grid_.RowTop(row) * 0.75; }

 private:
  const sh::view::Grid& grid_;
};

// S3's budgets (docs/s3-plan.md): creating a table, a conditional format and a chart from a
// selection (1,000 rows of four columns) to screen, < 100 ms each; and a chart drawn again after an
// edit to its data, within the keystroke budget (< 16 ms typical, < 50 ms worst). Each creation is
// undone before the next. "To screen": the edit, what the tab reads again (the rules, the sheet's
// objects) and the cells of a 1400 x 900 window painted, with the objects over them.
int S3Timings(const fs::path& tmpl, int runs, std::string& json, int& over) {
  fw::pkg::Status st;
  auto book = ReadWorkbook(tmpl, st, true);
  if (!book) return std::fprintf(stderr, "Can't read the template %s: %s\n", Display(tmpl).c_str(), st.message.c_str()), 1;
  sh::edit::Editor ed(*book);
  const uint32_t rows = 1000;
  {
    std::vector<std::pair<sh::CellAddr, sh::edit::CellInput>> cells;
    const char* heads[] = {"Region", "Q1", "Q2", "Q3"};
    for (uint32_t c = 0; c < 4; ++c) cells.push_back({{0, c}, sh::edit::CellInput::Text(heads[c])});
    for (uint32_t r = 1; r <= rows; ++r) {
      cells.push_back({{r, 0}, sh::edit::CellInput::Text("Region " + std::to_string(r % 12))});
      for (uint32_t c = 1; c < 4; ++c) cells.push_back({{r, c}, sh::edit::CellInput::Number(static_cast<double>((r * 37 + c * 101) % 1000))});
    }
    if (!ed.SetCells(0, cells)) return std::fprintf(stderr, "Can't fill the S3 workbook.\n"), 1;
    std::vector<uint8_t> bytes;  // saved once, so the sheet has its part (charts need it)
    if (!(st = SaveBook(*book, ed, bytes)).Ok()) return std::fprintf(stderr, "saving: %s\n", st.message.c_str()), 1;
  }
  sh::calc::WorkbookCalc calc(*book);
  calc.Load();
  calc.engine().Opened(calc.Settings());
  calc.engine().Recalculate();
  Screen screen;
  if (!screen.Init(1400, 900)) return std::fprintf(stderr, "Can't paint: no Direct2D device.\n"), 1;
  static fw::text::Fonts fonts;
  sh::view::FontMeasure measure(fonts);
  sh::view::CellText text(*book, fonts, measure, sh::UserLocale());
  const sh::Sheet& sheet = book->GetSheet(0);
  sh::view::Grid grid;
  grid.Build(sheet, book->GetStyles(), measure);
  const ScreenCells geometry(grid, 31, 141);
  const ScreenLayout layout(grid);
  const sh::Range area{0, 0, 44, 19};
  std::unique_ptr<sh::condfmt::SheetRules> rules;
  std::unique_ptr<sh::SheetObjects> objects;
  auto paint = [&] {
    sh::view::PaintOptions o;
    o.rules = rules.get();
    screen.Paint([&](draw::Painter& p) {
      sh::view::PaintCells(p, sheet, text, grid, geometry, area, o);
      if (objects) {
        p.Save();
        p.Transform(draw::Matrix::Scale(4.f / 3, 4.f / 3) * draw::Matrix::Translate(31, 141));
        objects->Draw(p, {0, 0, 1369 * 0.75f, 759 * 0.75f}, {});
        p.Restore();
      }
    });
  };
  auto readRules = [&] {
    sh::condfmt::Options o;
    o.locale = &text.Locale();
    o.calc = &calc;
    rules = std::make_unique<sh::condfmt::SheetRules>(*book, 0, o);
    if (rules->Empty()) rules.reset();
  };
  auto readObjects = [&] {
    objects = sh::SheetObjects::Load(*book, 0);
    objects->Place(layout);
  };
  paint();
  const sh::Range data{0, 0, rows, 3};
  std::vector<double> table, rule, chart, redraw;
  std::string refused;
  auto failed = [&](const char* what, const sh::edit::Result& r) {
    refused = std::string(what) + ": " + fw::WideToUtf8(std::wstring(r.message.begin(), r.message.end()));
  };
  for (int i = 0; i < runs && refused.empty(); ++i) {
    // Insert > Table over the selection (its first row the headers, text already).
    auto t = Clock::now();
    {
      sh::Table tb;
      tb.ref = data;
      tb.autoFilter = true;
      tb.totalsRowShown = false;
      tb.style = sh::TableStyleInfo{"TableStyleMedium2", false, false, true, false};
      for (uint32_t c = 0; c < 4; ++c) tb.columns.push_back(sh::TableColumn{0, std::string(sheet.GetValue(0, c).text)});
      sh::edit::EditGroup group(ed, u"Create Table");
      if (sh::edit::Result r = ed.AddTable(0, tb); !r.ok) {
        failed("a table", r);
        break;
      }
      group.done = true;
    }
    text.Clear();
    paint();
    table.push_back(Since(t));
    if (sheet.Tables().size() != 1) return std::fprintf(stderr, "The table timed isn't on the sheet.\n"), 1;
    ed.Undo(1);
    text.Clear();

    // A conditional format over the selection: values over 500 in Light Red Fill with Dark Red Text.
    t = Clock::now();
    {
      std::vector<sh::data::CfRule> list = sh::data::ReadCfRules(sheet.Text());
      list.insert(list.begin(), sh::data::CellIsRule(sh::RangeName({1, 1, rows, 3}), sh::data::CfOp::Greater, "500", {},
                                                     ed.AddDxf(sh::data::CfPreset(0))));
      std::string xml, ext;
      sh::data::WriteCfRules(list, sheet.Text(), xml, ext);
      if (sh::edit::Result r = ed.SetSheetRules(0, sh::edit::Editor::RuleKind::ConditionalFormats, xml, ext, u"New Rule"); !r.ok) {
        failed("a rule", r);
        break;
      }
    }
    readRules();
    text.Clear();
    paint();
    rule.push_back(Since(t));
    if (!rules || rules->RuleCount() != 1) return std::fprintf(stderr, "The rule timed isn't on the sheet.\n"), 1;
    ed.Undo(1);
    readRules();
    text.Clear();

    // Insert > Chart (a column chart) of the first 30 rows, beside the data.
    t = Clock::now();
    sh::edit::Editor::NewChart nc;
    nc.data = {0, 0, 30, 2};
    nc.at = {1, 5};
    if (sh::edit::Result r = ed.InsertChart(0, nc); !r.ok) {
      failed("a chart", r);
      break;
    }
    readObjects();
    paint();
    chart.push_back(Since(t));
    if (const auto placed = objects->Objects(); placed.size() != 1 || !placed[0].chart)
      return std::fprintf(stderr, "The chart timed isn't on the sheet.\n"), 1;
    // The chart drawn again after an edit to its data: the tab paints its cells and the objects
    // (the chart reads the cells when it draws).
    for (int k = 0; k < 4; ++k) {
      t = Clock::now();
      ed.SetCell(0, 1 + static_cast<uint32_t>(k), 1, sh::edit::CellInput::Number(900.0 - k * 50 - i));
      calc.CellsEdited(0, {1u + static_cast<uint32_t>(k), 1, 1u + static_cast<uint32_t>(k), 1});
      while (!calc.CalcStep(1e9)) {}
      text.Clear();
      paint();
      redraw.push_back(Since(t));
    }
    ed.Undo(5);
    objects.reset();
    readObjects();
    text.Clear();
  }
  if (!refused.empty()) {
    std::printf("  %-52s not measured: %s\n", "S3: creating a table, rule and chart", refused.c_str());
    return 0;
  }
  Line(json, "createTable", "S3: a table from 1,000 rows, to screen", table, 100, over);
  Line(json, "createRule", "S3: a conditional format over them, to screen", rule, 100, over);
  Line(json, "createChart", "S3: a chart of 30 rows, to screen", chart, 100, over);
  Line(json, "chartRedraw", "S3: the chart drawn again after a data edit", redraw, 16, over, 50);
  return 0;
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
  std::printf("  %-52s %9s %9s\n", "", "typical", "worst");

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

    // Keystroke to screen in a cell of the same workbook: "1234,5" typed into A2, A3, ... (the cell
    // editor), each key followed by the cells of a 1400 x 900 window painted again; then Enter: the
    // edit, B's formula and the SUM over 100,000 recalculated, the cells' text made again (the tab
    // drops it after an edit) and painted. What the tab adds (the editor's box, the formula bar,
    // headers) is a line of text each; reading the typed text is left out (microseconds).
    Screen screen;
    if (!screen.Init(1400, 900)) return std::fprintf(stderr, "Can't paint: no Direct2D device.\n"), 1;
    static fw::text::Fonts fonts;
    sh::view::FontMeasure measure(fonts);
    sh::view::CellText text(*book, fonts, measure, sh::UserLocale());
    const sh::Sheet& sheet = book->GetSheet(0);
    sh::view::Grid grid;
    grid.Build(sheet, book->GetStyles(), measure);
    const ScreenCells geometry(grid, 31, 141);
    const sh::Range area{0, 0, 44, 19};
    const sh::view::PaintOptions paintOptions;
    auto paint = [&] { screen.Paint([&](draw::Painter& p) { sh::view::PaintCells(p, sheet, text, grid, geometry, area, paintOptions); }); };
    paint();  // on screen before the typing starts
    sh::view::CellEditor cell;
    std::vector<double> keys, enters;
    for (int i = 0; i < runs * 4; ++i) {
      const uint32_t row = 1 + static_cast<uint32_t>(i % 40);
      cell.Begin(L"", sh::view::CellEditor::Mode::Enter);
      for (wchar_t ch : std::wstring_view(L"1234,5")) {
        auto t = Clock::now();
        cell.OnChar(ch);
        paint();
        keys.push_back(Since(t));
      }
      auto t = Clock::now();
      cell.End();
      ed.SetCell(0, row, 0, sh::edit::CellInput::Number(1234.5 + i));
      calc.CellsEdited(0, {row, 0, row, 0});
      while (!calc.CalcStep(1e9)) {}
      text.Clear();
      paint();
      enters.push_back(Since(t));
    }
    Line(json, "cellKey", "a key typed in a cell, to screen", keys, 16, over, 50);
    Line(json, "cellEnter", "Enter: the edit, recalculation, to screen", enters, 0, over, 50);
  }
  {  // Inserting a row in the million-cell workbook, and undoing it; then saving it.
    fw::pkg::Status st;
    auto book = ReadWorkbook(million, st, true);
    if (!book) return std::fprintf(stderr, "%s: %s\n", Display(million).c_str(), st.message.c_str()), 1;
    sh::edit::Editor ed(*book);
    std::vector<double> insert, undo;
    std::string refused;
    // Row 50,001's A before, after the insert (moved down a row) and after the undo (back): what
    // was timed must have happened.
    const sh::Sheet& sheet = book->GetSheet(0);
    auto valueAt = [&](uint32_t row) {
      const sh::Value v = sheet.GetValue(row, 0);
      return v.type == sh::ValueType::Number ? v.number : -1.0;
    };
    const double before = valueAt(50000);
    for (int i = 0; i < runs && refused.empty(); ++i) {
      auto t = Clock::now();
      sh::edit::Result r = ed.InsertRows(0, 50000, 1);
      if (!r.ok) {
        refused = fw::WideToUtf8(std::wstring(r.message.begin(), r.message.end()));
        break;
      }
      insert.push_back(Since(t));
      if (valueAt(50000) != -1.0 || valueAt(50001) != before)
        return std::fprintf(stderr, "Inserting a row at 50,001 didn't move the cells below it.\n"), 1;
      t = Clock::now();
      r = ed.Undo(1);
      if (!r.ok || valueAt(50000) != before)
        return std::fprintf(stderr, "Undoing the inserted row didn't bring the cells back.\n"), 1;
      undo.push_back(Since(t));
    }
    if (refused.empty()) {
      Line(json, "insertRow", "inserting a row, 1,000,000 cells", insert, 100, over);
      Line(json, "undoInsertRow", "undoing it", undo, 100, over);
    } else {
      std::printf("  %-52s not measured: %s\n", "inserting a row, 1,000,000 cells", refused.c_str());
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
  if (int r = S3Timings(tmpl, runs, json, over)) return r;
  std::printf("\n%d over budget.\n", over);
  if (!options.jsonOut.empty()) std::ofstream(options.jsonOut, std::ios::trunc) << "{\n" << json << "\n}\n";
  return options.budgets && over ? 1 : 0;
}

}  // namespace fwtool
