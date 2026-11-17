#include "sheet/calc/Bind.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <map>

#include "sheet/calc/Bytecode.h"
#include "sheet/calc/Fn.h"
#include "sheet/model/Workbook.h"
#include "sheet/numfmt/NumFormat.h"
#include "sheet/numfmt/NumLocale.h"

namespace fw::sheet::calc {
namespace {

// The model's errors start at #NULL!; the engine's at None.
Err FromModel(ErrorCode e) { return static_cast<Err>(static_cast<uint8_t>(e) + 1); }
ErrorCode ToModel(Err e) {
  if (e == Err::None || e == Err::Python) return ErrorCode::Unknown;
  return static_cast<ErrorCode>(static_cast<uint8_t>(e) - 1);
}

CellView ViewOf(const sheet::Value& v) {
  CellView c;
  switch (v.type) {
    case ValueType::Number: c.type = Type::Number, c.num = v.number; break;
    case ValueType::String: c.type = Type::Text, c.text = v.text; break;
    case ValueType::Bool: c.type = Type::Bool, c.num = v.boolean ? 1 : 0; break;
    case ValueType::Error: c.type = Type::Error, c.err = FromModel(v.error); break;
    default: break;
  }
  return c;
}

}  // namespace

WorkbookCalc::WorkbookCalc(Workbook& book, const NumLocale* locale)
    : book_(book), locale_(locale), engine_(std::make_unique<Engine>(*this)) {
  if (locale) {
    if (!locale->decimal.empty() && locale->decimal[0] < 0x80) conv_.decimal = static_cast<char>(locale->decimal[0]);
    if (!locale->group.empty() && locale->group[0] < 0x80) conv_.group = static_cast<char>(locale->group[0]);
    conv_.dateOrder = locale->dateOrder;
    conv_.currency = Narrow(locale->currency);
  }
  // The sheets' and defined names now (cheap), so formulas compile at once (conditional formats).
  std::vector<std::string> names;
  for (size_t i = 0; i < book_.SheetCount(); ++i) names.push_back(book_.GetSheet(i).Name());
  engine_->SetSheets(std::move(names));
  for (const DefinedName& n : book_.Names()) engine_->DefineName(n.name, n.sheet, n.formula);
}
WorkbookCalc::~WorkbookCalc() = default;

int WorkbookCalc::SheetCount() const { return static_cast<int>(book_.SheetCount()); }

CellView WorkbookCalc::Cell(int sheet, int32_t row, int32_t col) {
  if (sheet < 0 || sheet >= SheetCount() || row < 0 || col < 0) return {};
  return ViewOf(book_.GetSheet(static_cast<size_t>(sheet)).GetValue(static_cast<uint32_t>(row), static_cast<uint32_t>(col)));
}

void WorkbookCalc::ForEachCell(int sheet, int32_t r1, int32_t c1, int32_t r2, int32_t c2, CellFn fn, void* context) {
  if (sheet < 0 || sheet >= SheetCount()) return;
  Range r{static_cast<uint32_t>(r1), static_cast<uint32_t>(c1), static_cast<uint32_t>(r2), static_cast<uint32_t>(c2)};
  book_.GetSheet(static_cast<size_t>(sheet)).ForEachCell(r, [&](const CellInfo& c) {
    if (c.value.type == ValueType::Empty) return true;
    return fn(context, static_cast<int32_t>(c.row), static_cast<int32_t>(c.col), ViewOf(c.value));
  });
}

void WorkbookCalc::Store(int sheet, int32_t row, int32_t col, const Value& v) {
  if (sheet < 0 || sheet >= SheetCount()) return;
  sheet::Value m;
  switch (v.type()) {
    case Type::Number: m = sheet::Value::Number(v.number()); break;
    case Type::Text: m = sheet::Value::Text(v.text()); break;
    case Type::Bool: m = sheet::Value::Bool(v.boolean()); break;
    case Type::Error: m = sheet::Value::Error(ToModel(v.error())); break;
    default: break;
  }
  Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
  const sheet::Value old = s.GetValue(static_cast<uint32_t>(row), static_cast<uint32_t>(col));
  if (old.type != m.type || old.number != m.number || old.boolean != m.boolean || old.error != m.error ||
      (m.type == ValueType::String && old.text != m.text))
    ++changes_;
  s.SetValue(static_cast<uint32_t>(row), static_cast<uint32_t>(col), m);
  auto& e = extents_[static_cast<size_t>(sheet)];
  e.first = std::max(e.first, row + 1), e.second = std::max(e.second, col + 1);
}

void WorkbookCalc::Extent(int sheet, int32_t& rows, int32_t& cols) {
  rows = cols = 0;
  if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return;
  if (static_cast<size_t>(sheet) >= extents_.size()) {  // still opening: as far as the sheet is read
    if (auto used = book_.GetSheet(static_cast<size_t>(sheet)).UsedRange())
      rows = static_cast<int32_t>(used->lastRow) + 1, cols = static_cast<int32_t>(used->lastCol) + 1;
    return;
  }
  rows = extents_[static_cast<size_t>(sheet)].first;
  cols = extents_[static_cast<size_t>(sheet)].second;
}

bool WorkbookCalc::Date1904() const { return book_.Date1904(); }

bool WorkbookCalc::Table(std::string_view name, int sheet, int32_t row, int32_t col, uint8_t items,
                         std::string_view col1, std::string_view col2, Area& out, Err& error) {
  error = Err::Ref;
  const sheet::Table* t = nullptr;
  size_t ts = static_cast<size_t>(sheet);
  if (!name.empty()) {
    t = book_.FindTable(name, &ts);
  } else if (sheet >= 0 && sheet < SheetCount()) {
    t = book_.GetSheet(ts).TableAt(static_cast<uint32_t>(row), static_cast<uint32_t>(col));
  }
  if (!t) return false;
  // Columns: all, one, or a span.
  int32_t c1 = static_cast<int32_t>(t->ref.firstCol), c2 = static_cast<int32_t>(t->ref.lastCol);
  if (!col1.empty()) {
    int a = t->FindColumn(col1), b = col2.empty() ? a : t->FindColumn(col2);
    if (a < 0 || b < 0) return false;
    c1 = static_cast<int32_t>(t->ref.firstCol) + std::min(a, b);
    c2 = static_cast<int32_t>(t->ref.firstCol) + std::max(a, b);
  }
  // Rows: the parts asked for (the data when none is), as one span.
  const int32_t first = static_cast<int32_t>(t->ref.firstRow), last = static_cast<int32_t>(t->ref.lastRow);
  const int32_t headers = static_cast<int32_t>(t->headerRows), totals = static_cast<int32_t>(t->totalsRows);
  const int32_t d1 = first + headers, d2 = last - totals;
  int32_t r1 = INT32_MAX, r2 = -1;
  auto add = [&](int32_t a, int32_t b) {
    if (a > b) return;
    r1 = std::min(r1, a), r2 = std::max(r2, b);
  };
  if (items & bc::kThisRow) {
    if (row < d1 || row > d2) return error = Err::Value, false;
    add(row, row);
  } else if (items & bc::kAll) {
    add(first, last);
  } else {
    uint8_t parts = items ? items : static_cast<uint8_t>(bc::kData);
    if (parts & bc::kHeaders) add(first, d1 - 1);
    if (parts & bc::kData) add(d1, d2);
    if (parts & bc::kTotals) add(d2 + 1, last);
  }
  if (r2 < 0) return false;
  out.r1 = r1, out.r2 = r2;
  out.c1 = static_cast<int16_t>(c1), out.c2 = static_cast<int16_t>(c2);
  out.s1 = out.s2 = static_cast<uint16_t>(ts);
  error = Err::None;
  return true;
}

bool WorkbookCalc::RowHidden(int sheet, int32_t row) {
  if (sheet < 0 || sheet >= SheetCount()) return false;
  return book_.GetSheet(static_cast<size_t>(sheet)).Row(static_cast<uint32_t>(row)).hidden;
}

// Hidden in the data rows of an autoFilter with criteria: the sheet's own, or a table's.
bool WorkbookCalc::RowFiltered(int sheet, int32_t row) {
  if (!RowHidden(sheet, row)) return false;
  const Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
  const uint32_t r = static_cast<uint32_t>(row);
  if (const auto& f = s.Filter(); f && f->filtered && r > f->ref.firstRow && r <= f->ref.lastRow) return true;
  for (const fw::sheet::Table& t : s.Tables()) {
    if (!t.filtered) continue;
    fw::sheet::Range d = t.Data();
    if (d.firstRow <= d.lastRow && r >= d.firstRow && r <= d.lastRow) return true;
  }
  return false;
}

void WorkbookCalc::CellsEdited(int sheet, const Range& cells) {
  while (Opening()) OpenStep(1e9);  // an edit while opening: finish that first
  if (sheet < 0 || sheet >= SheetCount()) return;
  Area a;
  a.s1 = a.s2 = static_cast<uint16_t>(sheet);
  a.r1 = static_cast<int32_t>(std::min<uint32_t>(cells.firstRow, 1048575));
  a.r2 = static_cast<int32_t>(std::min<uint32_t>(cells.lastRow, 1048575));
  a.c1 = static_cast<int16_t>(std::min<uint32_t>(cells.firstCol, 16383));
  a.c2 = static_cast<int16_t>(std::min<uint32_t>(cells.lastCol, 16383));
  engine_->ClearFormulas(sheet, a);
  const Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
  std::vector<calc::Cell> entered;
  shared_.clear();
  // Ranges are read as far as the sheet's extent: cells typed beyond it widen it (else =SUM(A2:A5)
  // wouldn't see a value just typed in A5).
  std::pair<int32_t, int32_t>* extent = static_cast<size_t>(sheet) < extents_.size() ? &extents_[static_cast<size_t>(sheet)] : nullptr;
  s.ForEachCell(cells, [&](const CellInfo& c) {
    if (extent && (c.hasFormula || c.value.type != ValueType::Empty)) {
      extent->first = std::max(extent->first, static_cast<int32_t>(c.row) + 1);
      extent->second = std::max(extent->second, static_cast<int32_t>(c.col) + 1);
    }
    if (!c.hasFormula) return true;
    AddFormula(s, static_cast<size_t>(sheet), c);
    calc::Cell at{sheet, static_cast<int32_t>(c.row), static_cast<int32_t>(c.col)};
    engine_->Touch(at);
    entered.push_back(at);
    return true;
  });
  shared_.clear();
  engine_->Changed(sheet, a);
  if (ManualCalc())
    for (const calc::Cell& at : entered) engine_->CalculateCell(at);
}

bool WorkbookCalc::SpillSize(std::string_view formula, int sheet, uint32_t row, uint32_t col, Range& spill) {
  while (Opening()) OpenStep(1e9);
  const calc::Cell at{sheet, static_cast<int32_t>(row), static_cast<int32_t>(col)};
  auto f = engine_->CompileAt(formula, at, true);
  if (!f) return false;
  Evaluator ev(*this, engine_.get());
  Value v;
  ev.Evaluate(*f, at, v);
  if (!v.IsArray() || (v.array().rows() <= 1 && v.array().cols() <= 1)) return false;
  const uint64_t lastRow = uint64_t{row} + static_cast<uint64_t>(v.array().rows()) - 1;
  const uint64_t lastCol = uint64_t{col} + static_cast<uint64_t>(v.array().cols()) - 1;
  spill = Range::Cell(row, col);
  if (lastRow > 1048575 || lastCol > 16383) return true;  // off the sheet
  const Range want{row, col, static_cast<uint32_t>(lastRow), static_cast<uint32_t>(lastCol)};
  // Blocked by something in the way: a value or formula, a merged cell, a table.
  const Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
  bool blocked = false;
  s.ForEachCell(want, [&](const CellInfo& c) {
    blocked = !(c.row == row && c.col == col) && (c.hasFormula || c.value.type != ValueType::Empty);
    return !blocked;
  });
  auto overlaps = [&](const Range& r) {
    return r.firstRow <= want.lastRow && r.lastRow >= want.firstRow && r.firstCol <= want.lastCol && r.lastCol >= want.firstCol;
  };
  for (const Range& m : s.Merges()) blocked = blocked || overlaps(m);
  for (const auto& t : s.Tables()) blocked = blocked || overlaps(t.ref);
  if (!blocked) spill = want;
  return true;
}

void WorkbookCalc::StructureEdited() {
  while (Opening()) OpenStep(1e9);
  engine_ = std::make_unique<Engine>(*this);
  extents_.clear();
  shared_.clear();
  sheet_ = 0, row_ = col_ = 0, formulasLeft_ = 0;
  phase_ = Phase::kSheets;
  // Every formula read again (the model's values are current), then all of them recalculated.
  std::vector<std::string> names;
  for (size_t i = 0; i < book_.SheetCount(); ++i) names.push_back(book_.GetSheet(i).Name());
  engine_->SetSheets(std::move(names));
  for (const DefinedName& n : book_.Names()) engine_->DefineName(n.name, n.sheet, n.formula);
  LoadSome(1e300);
  phase_ = Phase::kDone;
  CalcSettings settings = Settings();
  settings.fullCalcOnLoad = false;
  settings.fullCalcSheets.clear();
  engine_->Opened(settings);  // what's volatile
  if (!settings.manual) engine_->MarkAllDirty();
}

bool WorkbookCalc::CalcStep(double budgetMs) {
  if (Held()) return false;
  if (Opening()) return OpenStep(budgetMs);
  return engine_->Recalculate(budgetMs);
}

bool WorkbookCalc::ManualCalc() const { return book_.Calc().mode == CalcMode::Manual; }

void WorkbookCalc::CalculateNow() {
  if (Held()) return;
  while (Opening()) OpenStep(1e9);
  CalcSettings auto_ = Settings();
  auto_.manual = false;
  auto_.fullCalcOnLoad = false;
  auto_.fullCalcSheets.clear();
  engine_->Opened(auto_);  // volatile formulas and what depends on them, and those without a value
}

void WorkbookCalc::CalculateFull() {
  if (Held()) return;
  while (Opening()) OpenStep(1e9);
  engine_->MarkAllDirty();
}

void WorkbookCalc::CalculateSheet(int sheet) {
  if (Held()) return;
  while (Opening()) OpenStep(1e9);
  if (sheet >= 0 && sheet < SheetCount()) engine_->CalculateSheet(sheet);
}

bool WorkbookCalc::CircularCell(int& sheet, int32_t& row, int32_t& col) const {
  calc::Cell at;
  if (Settings().iterate || !engine_->Circular() || !engine_->FirstCircular(at)) return false;
  sheet = at.sheet, row = at.row, col = at.col;
  return true;
}

bool WorkbookCalc::Manual() { return book_.Calc().mode == CalcMode::Manual; }

int WorkbookCalc::FormatText(const CellView& value, std::string_view format, std::string& out) {
  auto it = formats_.find(std::string(format));
  if (it == formats_.end()) {
    if (formats_.size() > 4096) formats_.clear();
    // TEXT() reads the code as the region types it (on an en-NL PC: "jjjj", "#.##0,00").
    auto f = std::make_unique<NumberFormat>();
    if (!NumberFormat::FromLocal(Wide(format), locale_ ? *locale_ : EnUsLocale(), *f)) f.reset();
    it = formats_.emplace(std::string(format), std::move(f)).first;
  }
  if (!it->second) return -1;
  FormatContext ctx;
  ctx.locale = locale_;
  ctx.date1904 = book_.Date1904();
  Formatted f;
  if (value.type == Type::Text) it->second->Format(Wide(value.text), ctx, f);
  else it->second->Format(value.num, ctx, f);
  if (f.overflow) return -1;
  out = Narrow(f.text);
  return 1;
}

CalcSettings WorkbookCalc::Settings() const {
  const CalcProps& p = book_.Calc();
  CalcSettings s;
  s.manual = p.mode == CalcMode::Manual;
  // Excel (2021 here) keeps a file's cached values only from a calculation engine it trusts: calcId
  // 191028 or later with the calc features RD, Single, FV and CNMTM listed. Anything else it
  // recalculates in full on opening (probed with edited files, docs/s1-formulas.md).
  bool trusted = p.calcId >= 191028;
  for (const char* need : {"microsoft.com:RD", "microsoft.com:Single", "microsoft.com:FV", "microsoft.com:CNMTM"})
    trusted = trusted && std::find(p.calcFeatures.begin(), p.calcFeatures.end(), need) != p.calcFeatures.end();
  s.fullCalcOnLoad = p.fullCalcOnLoad || !trusted;
  for (size_t i = 0; i < book_.SheetCount(); ++i)
    if (book_.GetSheet(i).Props().fullCalcOnLoad) s.fullCalcSheets.push_back(static_cast<int>(i));
  s.iterate = p.iterate;
  s.iterateCount = static_cast<int>(p.iterateCount);
  s.iterateDelta = p.iterateDelta;
  return s;
}

namespace {
double Clock() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}  // namespace

bool WorkbookCalc::LoadSome(double deadline) {
  if (phase_ == Phase::kSheets) {
    // Every sheet: formulas may read any. One at a time, as the view reads them at idle.
    for (; sheet_ < book_.SheetCount(); ++sheet_) {
      if (book_.GetSheet(sheet_).Loaded()) continue;
      if (Clock() > deadline) return false;
      book_.LoadSheet(sheet_);
    }
    extents_.assign(book_.SheetCount(), {0, 0});
    for (size_t i = 0; i < book_.SheetCount(); ++i)
      if (auto used = book_.GetSheet(i).UsedRange())
        extents_[i] = {static_cast<int32_t>(used->lastRow) + 1, static_cast<int32_t>(used->lastCol) + 1};
    phase_ = Phase::kFormulas, sheet_ = 0, row_ = col_ = 0;
    shared_.clear();
  }
  for (; sheet_ < book_.SheetCount(); ++sheet_, row_ = col_ = 0, shared_.clear()) {
    const Sheet& s = book_.GetSheet(sheet_);
    const size_t i = sheet_;
    // Only as far as the sheet's last formula: none to find in a sheet of values.
    if (row_ == 0 && col_ == 0) formulasLeft_ = s.FormulaCount();
    if (formulasLeft_ == 0) continue;
    bool stopped = false;
    uint32_t seen = 0;
    Range from = Range::All();
    from.firstRow = row_;
    s.ForEachCell(from, [&](const CellInfo& c) {
      if (c.row == row_ && c.col < col_) return true;  // done in the last slice
      if ((++seen & 255) == 0 && Clock() > deadline) {
        row_ = c.row, col_ = c.col, stopped = true;
        return false;
      }
      if (!c.hasFormula) return true;
      AddFormula(s, i, c);
      return --formulasLeft_ > 0;
    });
    if (stopped) return false;
  }
  phase_ = Phase::kGraph;
  return true;
}

void WorkbookCalc::AddFormula(const Sheet& s, size_t i, const CellInfo& c) {
  FormulaInfo fi;
  if (!s.GetFormula(c.row, c.col, fi) || fi.text.empty()) return;
  if (fi.kind == FormulaKind::DataTable) {
    ++stats_.dataTables;
    return;
  }
  calc::Cell at{static_cast<int>(i), static_cast<int32_t>(c.row), static_cast<int32_t>(c.col)};
  calc::Cell origin{static_cast<int>(i), static_cast<int32_t>(fi.origin.row), static_cast<int32_t>(fi.origin.col)};
  bool array = fi.kind == FormulaKind::Array;
  std::shared_ptr<const Formula> f;
  if (fi.kind == FormulaKind::Shared) {
    // A shared formula compiles once, at its master: relative references are offsets, so its
    // followers use the same bytecode.
    auto key = std::make_pair(fi.origin.row, fi.origin.col);
    auto it = shared_.find(key);
    if (it == shared_.end()) it = shared_.emplace(key, engine_->CompileAt(fi.text, origin)).first;
    f = it->second;
  } else {
    f = engine_->CompileAt(fi.text, at, array);
  }
  if (!f) {
    ++stats_.parseErrors;
    return;
  }
  bool cached = c.value.type != ValueType::Empty;
  if (array) {
    Area range;
    range.r1 = static_cast<int32_t>(fi.ref.firstRow), range.r2 = static_cast<int32_t>(fi.ref.lastRow);
    range.c1 = static_cast<int16_t>(fi.ref.firstCol), range.c2 = static_cast<int16_t>(fi.ref.lastCol);
    if (range.r2 < range.r1 || range.c2 < range.c1) range.r1 = range.r2 = at.row, range.c1 = range.c2 = static_cast<int16_t>(at.col);
    engine_->SetFormula(at, std::move(f), cached, &range, fi.dynamic);
  } else {
    engine_->SetFormula(at, std::move(f), cached);
  }
  ++stats_.formulas;
}

WorkbookCalc::LoadStats WorkbookCalc::Load() {
  if (phase_ < Phase::kGraph) LoadSome(1e300);
  return stats_;
}

bool WorkbookCalc::OpenStep(double budgetMs) {
  const double deadline = Clock() + budgetMs / 1000;
  if (phase_ < Phase::kGraph && !LoadSome(deadline)) return false;
  if (phase_ == Phase::kGraph) {
    if (!engine_->BuildGraph((deadline - Clock()) * 1000)) return false;
    engine_->Opened(Settings());
    phase_ = Phase::kRecalc;
  }
  if (phase_ == Phase::kRecalc) {
    if (!engine_->Recalculate(std::max(0.0, (deadline - Clock()) * 1000))) return false;
    phase_ = Phase::kDone;
  }
  return true;
}

}  // namespace fw::sheet::calc
