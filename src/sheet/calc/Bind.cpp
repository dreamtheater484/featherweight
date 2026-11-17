#include "sheet/calc/Bind.h"

#include <map>

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
  book_.GetSheet(static_cast<size_t>(sheet)).SetValue(static_cast<uint32_t>(row), static_cast<uint32_t>(col), m);
  auto& e = extents_[static_cast<size_t>(sheet)];
  e.first = std::max(e.first, row + 1), e.second = std::max(e.second, col + 1);
}

void WorkbookCalc::Extent(int sheet, int32_t& rows, int32_t& cols) {
  rows = cols = 0;
  if (sheet < 0 || static_cast<size_t>(sheet) >= extents_.size()) return;
  rows = extents_[static_cast<size_t>(sheet)].first;
  cols = extents_[static_cast<size_t>(sheet)].second;
}

bool WorkbookCalc::Date1904() const { return book_.Date1904(); }

bool WorkbookCalc::RowHidden(int sheet, int32_t row) {
  if (sheet < 0 || sheet >= SheetCount()) return false;
  return book_.GetSheet(static_cast<size_t>(sheet)).Row(static_cast<uint32_t>(row)).hidden;
}

int WorkbookCalc::FormatText(double value, std::string_view format, std::string& out) {
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
  it->second->Format(value, ctx, f);
  if (f.overflow) return -1;
  out = Narrow(f.text);
  return 1;
}

CalcSettings WorkbookCalc::Settings() const {
  const CalcProps& p = book_.Calc();
  CalcSettings s;
  s.manual = p.mode == CalcMode::Manual;
  s.fullCalcOnLoad = p.fullCalcOnLoad;
  s.iterate = p.iterate;
  s.iterateCount = static_cast<int>(p.iterateCount);
  s.iterateDelta = p.iterateDelta;
  return s;
}

WorkbookCalc::LoadStats WorkbookCalc::Load() {
  LoadStats stats;
  book_.LoadAll();
  std::vector<std::string> names;
  extents_.assign(book_.SheetCount(), {0, 0});
  for (size_t i = 0; i < book_.SheetCount(); ++i) {
    const Sheet& s = book_.GetSheet(i);
    names.push_back(s.Name());
    if (auto used = s.UsedRange()) extents_[i] = {static_cast<int32_t>(used->lastRow) + 1, static_cast<int32_t>(used->lastCol) + 1};
  }
  engine_->SetSheets(std::move(names));
  for (const DefinedName& n : book_.Names()) engine_->DefineName(n.name, n.sheet, n.formula);
  for (size_t i = 0; i < book_.SheetCount(); ++i) {
    const Sheet& s = book_.GetSheet(i);
    // A shared formula compiles once, at its master: relative references are offsets, so its
    // followers use the same bytecode.
    std::map<std::pair<uint32_t, uint32_t>, std::shared_ptr<const Formula>> shared;
    s.ForEachCell(Range::All(), [&](const CellInfo& c) {
      if (!c.hasFormula) return true;
      FormulaInfo fi;
      if (!s.GetFormula(c.row, c.col, fi) || fi.text.empty()) return true;
      if (fi.kind == FormulaKind::DataTable) {
        ++stats.dataTables;
        return true;
      }
      calc::Cell at{static_cast<int>(i), static_cast<int32_t>(c.row), static_cast<int32_t>(c.col)};
      calc::Cell origin{static_cast<int>(i), static_cast<int32_t>(fi.origin.row), static_cast<int32_t>(fi.origin.col)};
      bool array = fi.kind == FormulaKind::Array;
      std::shared_ptr<const Formula> f;
      if (fi.kind == FormulaKind::Shared) {
        auto key = std::make_pair(fi.origin.row, fi.origin.col);
        auto it = shared.find(key);
        if (it == shared.end()) it = shared.emplace(key, engine_->CompileAt(fi.text, origin)).first;
        f = it->second;
      } else {
        f = engine_->CompileAt(fi.text, at, array);
      }
      if (!f) {
        ++stats.parseErrors;
        return true;
      }
      bool cached = c.value.type != ValueType::Empty;
      if (array) {
        Area range;
        range.r1 = static_cast<int32_t>(fi.ref.firstRow), range.r2 = static_cast<int32_t>(fi.ref.lastRow);
        range.c1 = static_cast<int16_t>(fi.ref.firstCol), range.c2 = static_cast<int16_t>(fi.ref.lastCol);
        if (range.r2 < range.r1 || range.c2 < range.c1) range.r1 = range.r2 = at.row, range.c1 = range.c2 = static_cast<int16_t>(at.col);
        engine_->SetFormula(at, std::move(f), cached, &range);
      } else {
        engine_->SetFormula(at, std::move(f), cached);
      }
      ++stats.formulas;
      return true;
    });
  }
  return stats;
}

}  // namespace fw::sheet::calc
