#include "sheet/model/Sheet.h"

#include <algorithm>

#include "sheet/model/Workbook.h"

namespace fw::sheet {

using namespace detail;

size_t RowBlock::Bytes() const {
  return sizeof(RowBlock) + rows.capacity() * sizeof(RowRec) + cells.capacity() * sizeof(CellRec) +
         formulas.capacity() * sizeof(formulas[0]) + sides.capacity() * sizeof(CellSide);
}

Sheet::Sheet(Workbook& book, std::string name) : book_(book), name_(std::move(name)) {}

// ---- Finding cells

const RowRec* Sheet::FindRow(uint32_t row, const RowBlock** block) const {
  auto it = std::upper_bound(blockFirstRow_.begin(), blockFirstRow_.end(), row);
  if (it == blockFirstRow_.begin()) return nullptr;
  const RowBlock& b = *blocks_[static_cast<size_t>(it - blockFirstRow_.begin()) - 1];
  auto r = std::lower_bound(b.rows.begin(), b.rows.end(), row, [](const RowRec& x, uint32_t v) { return x.row < v; });
  if (r == b.rows.end() || r->row != row) return nullptr;
  if (block) *block = &b;
  return &*r;
}

bool Sheet::Find(uint32_t row, uint32_t col, Found& f) const {
  const RowBlock* b = nullptr;
  const RowRec* r = FindRow(row, &b);
  if (!r) return false;
  auto first = b->cells.begin() + r->first, last = first + r->count;
  auto c = std::lower_bound(first, last, col, [](const CellRec& x, uint32_t v) { return x.col < v; });
  if (c == last || c->col != col) return false;
  f.block = b;
  f.row = r;
  f.cell = &*c;
  f.cellIndex = static_cast<uint32_t>(c - b->cells.begin());
  return true;
}

void Sheet::FillInfo(const RowBlock& b, const RowRec& r, uint32_t cellIndex, CellInfo& out) const {
  const CellRec& c = b.cells[cellIndex];
  out = CellInfo{};
  out.row = r.row;
  out.col = c.col;
  out.style = c.style;
  out.hasFormula = (c.flags & kHasFormula) != 0;
  out.computed = (c.flags & kComputed) != 0;
  if (!(c.flags & kHasValue)) return;
  switch (c.type) {
    case XmlType::N:
    case XmlType::D:
      out.value = Value::Number(c.v.number);
      break;
    case XmlType::S:
      if (c.v.index < book_.Strings().Size()) {
        out.value = Value::Text(book_.Strings().Text(c.v.index));
        out.sharedString = c.v.index;
      }
      break;
    case XmlType::Str:
    case XmlType::InlineStr:
      out.value = Value::Text(texts_.Get(c.v.index));
      break;
    case XmlType::B:
      out.value = Value::Bool(c.v.index != 0);
      break;
    case XmlType::E:
      out.value = Value::Error(static_cast<ErrorCode>(c.v.index));
      break;
  }
}

bool Sheet::GetCell(uint32_t row, uint32_t col, CellInfo& out) const {
  Found f;
  if (!Find(row, col, f)) return false;
  FillInfo(*f.block, *f.row, f.cellIndex, out);
  return true;
}

Value Sheet::GetValue(uint32_t row, uint32_t col) const {
  CellInfo c;
  return GetCell(row, col, c) ? c.value : Value{};
}

uint32_t Sheet::StyleAt(uint32_t row, uint32_t col) const {
  const RowBlock* b = nullptr;
  const RowRec* r = FindRow(row, &b);
  if (r) {
    auto first = b->cells.begin() + r->first, last = first + r->count;
    auto c = std::lower_bound(first, last, col, [](const CellRec& x, uint32_t v) { return x.col < v; });
    if (c != last && c->col == col) return c->style;
    if (r->flags & kRowCustomFormat) return r->style;
  }
  return Column(col).style;
}

void Sheet::ForEachCell(const Range& range, const std::function<bool(const CellInfo&)>& fn) const {
  auto it = std::upper_bound(blockFirstRow_.begin(), blockFirstRow_.end(), range.firstRow);
  size_t bi = it == blockFirstRow_.begin() ? 0 : static_cast<size_t>(it - blockFirstRow_.begin()) - 1;
  CellInfo info;
  for (; bi < blocks_.size(); ++bi) {
    const RowBlock& b = *blocks_[bi];
    if (blockFirstRow_[bi] > range.lastRow) return;
    auto r = std::lower_bound(b.rows.begin(), b.rows.end(), range.firstRow,
                              [](const RowRec& x, uint32_t v) { return x.row < v; });
    for (; r != b.rows.end(); ++r) {
      if (r->row > range.lastRow) return;
      auto first = b.cells.begin() + r->first, last = first + r->count;
      auto c = std::lower_bound(first, last, range.firstCol, [](const CellRec& x, uint32_t v) { return x.col < v; });
      for (; c != last && c->col <= range.lastCol; ++c) {
        FillInfo(b, *r, static_cast<uint32_t>(c - b.cells.begin()), info);
        if (!fn(info)) return;
      }
    }
  }
}

std::optional<Range> Sheet::UsedRange() const {
  std::optional<Range> used;
  for (const auto& block : blocks_)
    for (const RowRec& r : block->rows) {
      if (r.count == 0) continue;
      uint32_t c0 = block->cells[r.first].col, c1 = block->cells[r.first + r.count - 1].col;
      if (!used) {
        used = Range{r.row, c0, r.row, c1};
      } else {
        used->firstCol = std::min(used->firstCol, c0);
        used->lastCol = std::max(used->lastCol, c1);
        used->lastRow = r.row;
      }
    }
  return used;
}

// ---- Formulas

bool Sheet::GetFormula(uint32_t row, uint32_t col, FormulaInfo& out) const {
  Found f;
  if (!Find(row, col, f) || !(f.cell->flags & kHasFormula)) return false;
  const auto& list = f.block->formulas;
  auto it = std::lower_bound(list.begin(), list.end(), f.cellIndex,
                             [](const std::pair<uint32_t, uint32_t>& x, uint32_t v) { return x.first < v; });
  if (it == list.end() || it->first != f.cellIndex || it->second >= formulas_.size()) return false;
  const FormulaRec& rec = formulas_[it->second];
  out = FormulaInfo{};
  out.kind = rec.kind;
  out.alwaysCalc = rec.alwaysCalc;
  out.sharedIndex = rec.sharedIndex;
  out.attrs = rec.attrs;
  out.origin = {row, col};
  out.ref = rec.hasRef ? rec.ref : Range::Cell(row, col);
  if (rec.text != kNone) {
    out.text = texts_.Get(rec.text);
    out.master = rec.kind == FormulaKind::Shared || rec.kind == FormulaKind::Array;
    return true;
  }
  if (rec.kind != FormulaKind::Shared) return true;
  // A cell sharing a formula: the master's text, written for the master's cell.
  auto m = std::lower_bound(sharedMasters_.begin(), sharedMasters_.end(), rec.sharedIndex,
                            [](const std::pair<uint32_t, CellAddr>& x, uint32_t v) { return x.first < v; });
  if (m == sharedMasters_.end() || m->first != rec.sharedIndex) return true;
  FormulaInfo master;
  if (!(m->second == CellAddr{row, col}) && GetFormula(m->second.row, m->second.col, master) && master.master) {
    out.text = master.text;
    out.origin = master.origin;
    out.ref = master.ref;
  }
  return true;
}

std::string_view Sheet::FormulaAttribute(const FormulaInfo& f, std::string_view local) const {
  for (const auto& a : store_.Attributes(f.attrs)) {
    std::string_view q = a.qname;
    if (q.find(':') == std::string_view::npos && q == local) return a.value;
  }
  return {};
}

// ---- Rows and columns

RowInfo Sheet::RowInfoOf(const RowRec& r) const {
  RowInfo info;
  info.stored = true;
  info.height = r.height;
  info.style = r.style;
  info.outlineLevel = r.outlineLevel;
  info.hidden = (r.flags & kRowHidden) != 0;
  info.customHeight = (r.flags & kRowCustomHeight) != 0;
  info.collapsed = (r.flags & kRowCollapsed) != 0;
  info.customFormat = (r.flags & kRowCustomFormat) != 0;
  info.thickTop = (r.flags & kRowThickTop) != 0;
  info.thickBottom = (r.flags & kRowThickBottom) != 0;
  return info;
}

RowInfo Sheet::Row(uint32_t row) const {
  const RowRec* r = FindRow(row);
  return r ? RowInfoOf(*r) : RowInfo{};
}

void Sheet::ForEachRow(uint32_t first, uint32_t last, const std::function<bool(uint32_t, const RowInfo&)>& fn) const {
  auto it = std::upper_bound(blockFirstRow_.begin(), blockFirstRow_.end(), first);
  size_t bi = it == blockFirstRow_.begin() ? 0 : static_cast<size_t>(it - blockFirstRow_.begin()) - 1;
  for (; bi < blocks_.size(); ++bi) {
    const RowBlock& b = *blocks_[bi];
    auto r = std::lower_bound(b.rows.begin(), b.rows.end(), first, [](const RowRec& x, uint32_t v) { return x.row < v; });
    for (; r != b.rows.end(); ++r) {
      if (r->row > last) return;
      if (!fn(r->row, RowInfoOf(*r))) return;
    }
  }
}

ColumnInfo Sheet::Column(uint32_t col) const {
  auto it = std::upper_bound(cols_.begin(), cols_.end(), col, [](uint32_t v, const ColumnInfo& c) { return v < c.first; });
  if (it != cols_.begin() && col <= (it - 1)->last) {
    ColumnInfo c = *(it - 1);
    c.first = c.last = col;
    return c;
  }
  ColumnInfo c;
  c.first = c.last = col;
  return c;
}

const Range* Sheet::MergeAt(uint32_t row, uint32_t col) const {
  uint32_t from = row >= mergeMaxHeight_ ? row - mergeMaxHeight_ : 0;
  auto it = std::lower_bound(merges_.begin(), merges_.end(), from, [](const Range& r, uint32_t v) { return r.firstRow < v; });
  for (; it != merges_.end() && it->firstRow <= row; ++it)
    if (it->Contains(row, col)) return &*it;
  return nullptr;
}

int Table::FindColumn(std::string_view columnName) const {
  for (size_t i = 0; i < columns.size(); ++i)
    if (NameEqualsNoCase(columns[i].name, columnName)) return static_cast<int>(i);
  return -1;
}

const Table* Sheet::TableAt(uint32_t row, uint32_t col) const {
  for (const Table& t : tables_)
    if (t.ref.Contains(row, col)) return &t;
  return nullptr;
}

std::string Sheet::KeptXml() const {
  if (text_.rootStart.empty()) return {};
  std::string s = text_.rootStart;
  if (!text_.rootEnd.empty()) {
    s += text_.beforeData;
    s += text_.afterData;
    s += text_.rootEnd;
  }
  return s;
}

// ---- Changes

RowBlock& Sheet::MutableBlock(size_t index) {
  auto& p = blocks_[index];
  if (p.use_count() > 1) p = std::make_shared<RowBlock>(*p);
  return *p;
}

void Sheet::SetValue(uint32_t row, uint32_t col, const Value& v) {
  if (row >= kMaxRows || col >= kMaxCols) return;
  // Find or make the row.
  auto it = std::upper_bound(blockFirstRow_.begin(), blockFirstRow_.end(), row);
  size_t bi;
  if (it == blockFirstRow_.begin()) {
    if (blocks_.empty() || blocks_[0]->rows.size() >= SheetBuilder::kBlockRows) {
      blocks_.insert(blocks_.begin(), std::make_shared<RowBlock>());
      blockFirstRow_.insert(blockFirstRow_.begin(), row);
    }
    bi = 0;
    blockFirstRow_[0] = row;
  } else {
    bi = static_cast<size_t>(it - blockFirstRow_.begin()) - 1;
  }
  RowBlock& b = MutableBlock(bi);
  auto r = std::lower_bound(b.rows.begin(), b.rows.end(), row, [](const RowRec& x, uint32_t val) { return x.row < val; });
  if (r == b.rows.end() || r->row != row) {
    RowRec rec;
    rec.row = row;
    rec.first = r == b.rows.end() ? static_cast<uint32_t>(b.cells.size()) : r->first;
    r = b.rows.insert(r, rec);
  }
  auto first = b.cells.begin() + r->first, last = first + r->count;
  auto c = std::lower_bound(first, last, col, [](const CellRec& x, uint32_t val) { return x.col < val; });
  uint32_t index = static_cast<uint32_t>(c - b.cells.begin());
  if (c == last || c->col != col) {
    if (v.type == ValueType::Empty) return;
    CellRec rec;
    rec.col = static_cast<uint16_t>(col);
    rec.style = StyleAt(row, col);
    if (rec.style) rec.flags |= kStylePresent;
    b.cells.insert(b.cells.begin() + index, rec);
    ++r->count;
    for (auto rr = r + 1; rr != b.rows.end(); ++rr) ++rr->first;
    for (auto& f : b.formulas)
      if (f.first >= index) ++f.first;
    for (auto& s : b.sides)
      if (s.cell >= index) ++s.cell;
    ++cellCount_;
  }
  CellRec& cell = b.cells[index];
  // The value as written is gone: drop its text.
  for (auto& s : b.sides)
    if (s.cell == index) s.valueText = kNone;
  XmlType oldType = cell.type;
  cell.flags = static_cast<uint8_t>((cell.flags & ~(kHasValue | kPreserve)) | kComputed);
  switch (v.type) {
    case ValueType::Empty:
      break;
    case ValueType::Number:
      cell.type = oldType == XmlType::D ? XmlType::D : XmlType::N;
      cell.numStyle = 0;
      cell.v.number = v.number;
      cell.flags |= kHasValue;
      break;
    case ValueType::String:
      cell.type = (cell.flags & kHasFormula) ? XmlType::Str : XmlType::InlineStr;
      cell.v.index = texts_.Add(v.text);
      cell.flags |= kHasValue;
      break;
    case ValueType::Bool:
      cell.type = XmlType::B;
      cell.v.index = v.boolean ? 1 : 0;
      cell.flags |= kHasValue;
      break;
    case ValueType::Error:
      cell.type = XmlType::E;
      cell.v.index = static_cast<uint32_t>(v.error);
      cell.flags |= kHasValue;
      break;
  }
  if (cell.type != oldType) cell.flags &= static_cast<uint8_t>(~kTypePresent);
}

size_t Sheet::ModelBytes() const {
  size_t n = sizeof(Sheet) + blocks_.capacity() * sizeof(blocks_[0]) + blockFirstRow_.capacity() * 4 +
             formulas_.capacity() * sizeof(FormulaRec) + sharedMasters_.capacity() * sizeof(sharedMasters_[0]) +
             texts_.Bytes() + cols_.capacity() * sizeof(ColumnInfo) + merges_.capacity() * sizeof(Range);
  for (const auto& b : blocks_) n += b->Bytes();
  return n;
}

// ---- Building

SheetBuilder::SheetBuilder(Sheet& sheet) : s_(sheet) {
  if (!s_.blocks_.empty()) {
    block_ = &s_.MutableBlock(s_.blocks_.size() - 1);
    if (!block_->rows.empty()) {
      hasRow_ = true;
      lastRow_ = block_->rows.back().row;
    }
  }
}

SheetBuilder::~SheetBuilder() { Finish(); }

void SheetBuilder::NewBlock() {
  if (block_) {
    block_->rows.shrink_to_fit();
    block_->cells.shrink_to_fit();
    block_->formulas.shrink_to_fit();
    block_->sides.shrink_to_fit();
  }
  s_.blocks_.push_back(std::make_shared<RowBlock>());
  block_ = s_.blocks_.back().get();
}

bool SheetBuilder::StartRow(uint32_t row, const RowInfo& info) {
  if ((hasRow_ && row <= lastRow_) || row >= kMaxRows) {
    rowOpen_ = false;
    return false;
  }
  if (!block_ || block_->rows.size() >= kBlockRows || block_->cells.size() >= kBlockCells) {
    NewBlock();
    s_.blockFirstRow_.push_back(row);
  }
  RowRec r;
  r.row = row;
  r.first = static_cast<uint32_t>(block_->cells.size());
  r.height = static_cast<float>(info.height);
  r.style = info.style;
  r.outlineLevel = info.outlineLevel;
  r.flags = static_cast<uint8_t>((info.hidden ? kRowHidden : 0) | (info.customHeight ? kRowCustomHeight : 0) |
                                 (info.collapsed ? kRowCollapsed : 0) | (info.customFormat ? kRowCustomFormat : 0) |
                                 (info.thickTop ? kRowThickTop : 0) | (info.thickBottom ? kRowThickBottom : 0));
  block_->rows.push_back(r);
  hasRow_ = rowOpen_ = true;
  lastRow_ = row;
  lastCol_ = -1;
  return true;
}

RowRec* SheetBuilder::CurrentRow() { return rowOpen_ ? &block_->rows.back() : nullptr; }

CellRec* SheetBuilder::AddCell(uint32_t col) {
  if (!rowOpen_ || static_cast<int64_t>(col) <= lastCol_ || col >= kMaxCols) return nullptr;
  lastCol_ = col;
  block_->cells.emplace_back();
  ++block_->rows.back().count;
  ++s_.cellCount_;
  CellRec* c = &block_->cells.back();
  c->col = static_cast<uint16_t>(col);
  return c;
}

void SheetBuilder::SetFormula(uint32_t formulaId) {
  uint32_t index = static_cast<uint32_t>(block_->cells.size() - 1);
  block_->formulas.emplace_back(index, formulaId);
  block_->cells.back().flags |= kHasFormula;
  ++s_.formulaCells_;
}

CellSide& SheetBuilder::Side() {
  uint32_t index = static_cast<uint32_t>(block_->cells.size() - 1);
  if (block_->sides.empty() || block_->sides.back().cell != index) {
    block_->sides.emplace_back();
    block_->sides.back().cell = index;
    block_->cells.back().flags |= kSide;
  }
  return block_->sides.back();
}

bool SheetBuilder::AddNumber(uint32_t col, double v, uint32_t style) {
  CellRec* c = AddCell(col);
  if (!c) return false;
  c->style = style;
  c->flags = static_cast<uint8_t>(kHasValue | (style ? kStylePresent : 0));
  c->v.number = v;
  return true;
}

bool SheetBuilder::AddSharedString(uint32_t col, uint32_t id, uint32_t style) {
  CellRec* c = AddCell(col);
  if (!c) return false;
  c->style = style;
  c->type = XmlType::S;
  c->flags = static_cast<uint8_t>(kHasValue | (style ? kStylePresent : 0));
  c->v.index = id;
  return true;
}

bool SheetBuilder::AddString(uint32_t col, std::string_view text, uint32_t style) {
  if (!rowOpen_ || static_cast<int64_t>(col) <= lastCol_ || col >= kMaxCols) return false;
  return AddSharedString(col, s_.book_.Strings().Add(text), style);
}

bool SheetBuilder::AddBool(uint32_t col, bool b, uint32_t style) {
  CellRec* c = AddCell(col);
  if (!c) return false;
  c->style = style;
  c->type = XmlType::B;
  c->flags = static_cast<uint8_t>(kHasValue | (style ? kStylePresent : 0));
  c->v.index = b ? 1 : 0;
  return true;
}

bool SheetBuilder::AddError(uint32_t col, ErrorCode e, uint32_t style) {
  CellRec* c = AddCell(col);
  if (!c) return false;
  c->style = style;
  c->type = XmlType::E;
  c->flags = static_cast<uint8_t>(kHasValue | (style ? kStylePresent : 0));
  c->v.index = static_cast<uint32_t>(e);
  return true;
}

bool SheetBuilder::AddEmpty(uint32_t col, uint32_t style) {
  CellRec* c = AddCell(col);
  if (!c) return false;
  c->style = style;
  c->flags = kStylePresent;
  return true;
}

void SheetBuilder::Finish() {
  if (block_) {
    block_->rows.shrink_to_fit();
    block_->cells.shrink_to_fit();
    block_->formulas.shrink_to_fit();
    block_->sides.shrink_to_fit();
  }
  rowOpen_ = false;
}

}  // namespace fw::sheet
