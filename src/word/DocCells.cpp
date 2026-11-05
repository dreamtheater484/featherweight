// Table cells on the page (plan §6.2 "Tables", milestone D3), from the layout's cell boxes
// (Layout::Cells), as in Word: a column border dragged moves its grid line (Editor::MoveColumnBorder)
// and a row's bottom border sets the row's height, at least (Editor::SetRowHeight), one undo step
// each, with a line across the table showing where while dragging; a click just above a column
// selects it, and a drag along the tops more columns. Selected cells are filled whole.

#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <iterator>

#include "ui/Theme.h"
#include "word/DocView.h"

using namespace gfx;
namespace layout = fw::doc::layout;
using fw::doc::ElemId;
using fw::doc::kNoElem;
using fw::doc::kNone;

namespace {

constexpr float kMinCell = 7.2f;  // a tenth of an inch: what the cells on either side keep (as the editor)
constexpr float kMinRow = 4;      // a row dragged up is this high at least

int32_t ToTwips(float pt) { return static_cast<int32_t>(std::lround(pt * 20.f)); }

// Word's black arrow pointing down, with a white edge: what a click just above a column does.
HCURSOR DownArrow() {
  constexpr int kSide = 32;
  BYTE andMask[kSide * kSide / 8], xorMask[kSide * kSide / 8];
  std::fill(std::begin(andMask), std::end(andMask), BYTE{0xFF});
  std::fill(std::begin(xorMask), std::end(xorMask), BYTE{0});
  auto in = [](int x, int y) {  // a shaft, then the head down to its tip at (8, 16)
    if (y >= 1 && y <= 8) return x >= 7 && x <= 9;
    return y >= 9 && y <= 16 && std::abs(x - 8) <= 16 - y;
  };
  for (int y = 0; y <= 17; ++y)
    for (int x = 0; x <= 16; ++x) {
      const bool body = in(x, y), edge = !body && (in(x - 1, y) || in(x + 1, y) || in(x, y - 1) || in(x, y + 1));
      if (!body && !edge) continue;
      const int bit = y * kSide + x;
      const BYTE m = static_cast<BYTE>(0x80 >> (bit % 8));
      andMask[bit / 8] = static_cast<BYTE>(andMask[bit / 8] & ~m);
      if (edge) xorMask[bit / 8] = static_cast<BYTE>(xorMask[bit / 8] | m);
    }
  return CreateCursor(GetModuleHandleW(nullptr), 8, 16, kSide, kSide, andMask, xorMask);
}

}  // namespace

std::optional<DocView::TextPos> DocView::CellStart(ElemId tc) const {
  // Its first paragraph: through content controls, not into a table in it.
  static const uint32_t kP = fw::doc::Name("p"), kTbl = fw::doc::Name("tbl");
  auto first = [&](auto& self, fw::doc::Node n) -> uint32_t {
    for (fw::doc::Node k = n.FirstChild(); k; k = k.Next()) {
      if (k.Is(kP)) return k.Raw().start;
      if (k.Is(kTbl)) continue;
      if (const uint32_t p = self(self, k); p != kNone) return p;
    }
    return kNone;
  };
  const uint32_t para = first(first, fw::doc::Node(&doc_->GetTree(), tc));
  if (para >= doc_->Paragraphs().size() || StoryOf({para, 0}) != StoryOf(focus_)) return std::nullopt;
  return TextPos{para, 0};
}

std::optional<DocView::CellEdge> DocView::CellEdgeAt(int x, int y) {
  if (!Editing() || !editor_->CanEdit() || !table_ || placeholder_) return std::nullopt;
  const int page = PageAtPoint(x, y);
  float px, py;
  if (page < 0 || page >= static_cast<int>(pagination_->PageCount()) || !ToPagePoint(page, x, y, px, py))
    return std::nullopt;
  const RECT pr = PageRect(page);
  const float scale = static_cast<float>((pr.right - pr.left) / std::max(1.0, sizes_[page].w));
  const float reach = 3 * ui_ / scale;  // page points
  const std::vector<layout::CellRect> cells = pagination_->Cells(page);
  auto continued = [&](const layout::CellRect& k) {  // a merged cell's piece below the first
    return doc_->ResolvedCell(fw::doc::Node(&doc_->GetTree(), k.tc)).vMerge == fw::doc::VMerge::Continue;
  };
  auto mergedBelow = [&](const layout::CellRect& c) {  // its cell goes on in the next row: no border
    for (const layout::CellRect& k : cells)
      if (k.tbl == c.tbl && k.row == c.row + 1 && k.col == c.col && continued(k)) return true;
    return false;
  };
  for (size_t i = cells.size(); i-- > 0;) {  // a nested table's cells come after the cell they're in
    const layout::CellRect& c = cells[i];
    const layout::Rect& b = c.box;
    if (px < b.left - reach || px > b.right + reach || py < b.top - reach || py > b.bottom + reach) continue;
    const bool alongY = py >= b.top && py <= b.bottom, alongX = px >= b.left && px <= b.right;
    CellEdge e;
    e.page = page;
    e.tbl = c.tbl;
    if (alongY && std::abs(px - b.right) <= reach) {
      e.line = c.col + c.span;
      e.pos = b.right;
    } else if (alongY && std::abs(px - b.left) <= reach) {
      e.line = c.col;
      e.pos = b.left;
    } else if (alongX && std::abs(py - b.bottom) <= reach && !mergedBelow(c)) {
      e.column = false;
      e.pos = b.bottom;
      e.top = b.top;
    } else {
      continue;
    }
    // Across the table on this page; a column border goes as far as the next ones, less what the
    // cells keep (the table's outer edges, to the page's).
    const layout::Page& pg = pagination_->GetPage(page);
    e.from = FLT_MAX;
    e.to = -FLT_MAX;
    e.lo = 0;
    e.hi = e.column ? pg.width : pg.height;
    for (const layout::CellRect& k : cells) {
      if (k.tbl != c.tbl) continue;
      e.from = std::min(e.from, e.column ? k.box.top : k.box.left);
      e.to = std::max(e.to, e.column ? k.box.bottom : k.box.right);
      if (!e.column) continue;
      for (const auto& [line, at] : {std::pair{k.col, k.box.left}, std::pair{k.col + k.span, k.box.right}}) {
        if (line < e.line) e.lo = std::max(e.lo, at + kMinCell);
        if (line > e.line) e.hi = std::min(e.hi, at - kMinCell);
      }
    }
    if (!e.column) e.lo = b.top + kMinRow;
    // A place in the table: for a row, a cell of that row alone (a merged one would take its rows).
    for (const layout::CellRect& k : cells) {
      if (k.tbl != c.tbl || (!e.column && (k.row != c.row || continued(k))) || (e.column && &k != &c)) continue;
      auto at = CellStart(k.tc);
      if (!at) continue;
      auto info = editor_->CellAt({at->para, at->offset});
      if (!info || info->tbl.Id() != c.tbl || (!e.column && (info->row != c.row || info->rows != 1))) continue;
      e.at = *at;
      return e;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<DocView::TextPos> DocView::ColumnTopAt(int x, int y, ElemId tbl) {
  if (!Editing() || !table_ || placeholder_) return std::nullopt;
  const int page = PageAtPoint(x, y);
  float px, py;
  if (page < 0 || page >= static_cast<int>(pagination_->PageCount()) || !ToPagePoint(page, x, y, px, py))
    return std::nullopt;
  const RECT pr = PageRect(page);
  const float scale = static_cast<float>((pr.right - pr.left) / std::max(1.0, sizes_[page].w));
  const float above = 8 * ui_ / scale, edge = 3 * ui_ / scale;
  const std::vector<layout::CellRect> cells = pagination_->Cells(page);
  for (size_t i = cells.size(); i-- > 0;) {
    const layout::CellRect& c = cells[i];
    if ((tbl != kNoElem && c.tbl != tbl) || px < c.box.left || px > c.box.right) continue;
    float top = c.box.top;  // the table's top on this page: only its first row here has the tops
    for (const layout::CellRect& k : cells)
      if (k.tbl == c.tbl) top = std::min(top, k.box.top);
    if (c.box.top > top + 0.5f) continue;
    if (tbl == kNoElem && (py < top - above || py > top + edge * 0.5f || px < c.box.left + edge || px > c.box.right - edge))
      continue;
    return CellStart(c.tc);
  }
  return std::nullopt;
}

bool DocView::CellMouseDown(int x, int y, UINT mods) {
  if (auto e = CellEdgeAt(x, y)) {
    e->now = e->pos;
    cellDrag_ = e;
    editor_->BreakTyping();
    host_.ViewInvalidate();
    return true;
  }
  if (mods & kModShift) return false;
  auto cell = ColumnTopAt(x, y);
  auto info = cell ? editor_->CellAt({cell->para, cell->offset}) : std::nullopt;
  if (!info) return false;
  lineDrag_ = LineDrag::Columns;
  if (!SelectLines(*cell, *cell)) {
    lineDrag_ = LineDrag::None;
    return false;
  }
  lineAnchor_ = *cell;
  lineTbl_ = info->tbl.Id();
  drag_ = Drag::Select;
  goalX_ = -1;
  editor_->BreakTyping();
  CaretMoved(false);
  host_.ViewInvalidate();
  return true;
}

bool DocView::CellMouseMove(int x, int y) {
  if (!cellDrag_) return false;
  CellEdge& e = *cellDrag_;
  float px, py;
  if (ToPagePoint(e.page, x, y, px, py)) {
    e.now = std::clamp(e.column ? px : py, e.lo, std::max(e.lo, e.hi));
    host_.ViewInvalidate();
  }
  return true;
}

bool DocView::CellMouseUp() {
  if (!cellDrag_) return false;
  const CellEdge e = *cellDrag_;
  cellDrag_.reset();
  host_.ViewInvalidate();
  if (std::abs(e.now - e.pos) < 0.05f) return true;  // a click
  const fw::doc::Pos at{e.at.para, e.at.offset};
  fw::doc::edit::Result r;
  if (e.column) {
    r = editor_->MoveColumnBorder(at, e.line, ToTwips(e.now - e.pos));
  } else {
    // An exact height stays exact; any other becomes "at least" this.
    auto info = editor_->CellAt(at);
    const bool exact = info && doc_->ResolvedRow(info->tr).heightRule == "exact";
    r = editor_->SetRowHeight(at, at, ToTwips(e.now - e.top), exact ? "exact" : "atLeast");
  }
  if (!r) host_.ViewMessage(BannerKind::Warning, std::wstring(r.message.begin(), r.message.end()));
  else AfterFormat({});
  return true;
}

HCURSOR DocView::CellCursor(int x, int y) {
  static HCURSOR we = LoadCursor(nullptr, IDC_SIZEWE), ns = LoadCursor(nullptr, IDC_SIZENS), down = DownArrow();
  if (cellDrag_) return cellDrag_->column ? we : ns;
  if (lineDrag_ == LineDrag::Columns) return down;
  if (drag_ != Drag::None || !Ready()) return nullptr;
  if (auto e = CellEdgeAt(x, y)) return e->column ? we : ns;
  if (ColumnTopAt(x, y)) return down;
  return nullptr;
}

void DocView::DrawCellGuide(Canvas& c, int first, int last) {
  if (!cellDrag_ || cellDrag_->page < first || cellDrag_->page > last) return;
  const CellEdge& e = *cellDrag_;
  const draw::Matrix m = PageToScreen(e.page);
  const draw::Point a = m.Apply(e.column ? draw::Point{e.now, e.from} : draw::Point{e.from, e.now});
  const draw::Point b = m.Apply(e.column ? draw::Point{e.now, e.to} : draw::Point{e.to, e.now});
  // On a pixel's middle, so it's sharp.
  const float sx = e.column ? std::floor(a.x) + 0.5f : a.x, sy = e.column ? a.y : std::floor(a.y) + 0.5f;
  const float ex = e.column ? sx : b.x, ey = e.column ? b.y : sy;
  c.Line(sx, sy, ex, ey, theme::P().accent, 1 * ui_);
}

void DocView::MeasureSelectedCells(int first, int last) {
  const TextPos a = std::min(anchor_, focus_), b = std::max(anchor_, focus_);
  if (cellFrom_ == a && cellTo_ == b && cellRevision_ == pagination_->Revision() && cellFirst_ == first &&
      cellLast_ == last)
    return;
  cellRects_.clear();
  cellFrom_ = a;
  cellTo_ = b;
  cellRevision_ = pagination_->Revision();
  cellFirst_ = first;
  cellLast_ = last;
  const auto block = table_->SelectedCells();
  if (!block) return;
  // Each piece of a selected cell on the pages in view: a merged cell has one in each row, a cell
  // split across pages one on each.
  const ElemId tbl = block->tbl.Id();
  const int pages = static_cast<int>(pagination_->PageCount());
  for (int pg = std::max(0, first); pg <= last && pg < pages; ++pg)
    for (const layout::CellRect& c : pagination_->GetPage(static_cast<size_t>(pg)).cells)
      if (c.tbl == tbl && c.row >= block->row0 && c.row <= block->row1 && c.col >= block->col0 &&
          c.col + c.span - 1 <= block->col1)
        cellRects_.push_back({static_cast<size_t>(pg), c.box});
}
