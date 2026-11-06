#include "doc/edit/ClipTable.h"

#include <algorithm>
#include <map>
#include <utility>

namespace fw::doc::edit {

namespace cliptable {

namespace {

// The w:tc elements around a paragraph, innermost first.
std::vector<Node> CellsAround(const Document& doc, uint32_t para) {
  std::vector<Node> out;
  for (Node n = doc.ParaNode(para).Parent(); n; n = n.Parent())
    if (n.Is(Name("tc"))) out.push_back(n);
  return out;
}

Node Up(Node n, uint32_t name) {
  for (; n; n = n.Parent())
    if (n.Is(name)) return n;
  return {};
}

// Word's widest table: columns past it are left out.
constexpr int32_t kMaxColumns = 63;

// One table of a clip, cell by cell: [begin, end) of the clip's paragraphs at each row and column.
struct CellText {
  uint32_t row = 0;
  int32_t col = 0;
  size_t begin = 0, end = 0;
};
struct Shape {
  std::vector<CellText> cells;  // by row, then column
  uint32_t rows = 0;
  int32_t cols = 0;
};

Shape ShapeOf(const Clip& clip, size_t b, size_t e) {
  Shape s;
  if (b >= e) return s;
  uint32_t first = UINT32_MAX;
  for (size_t k = b; k < e; ++k) first = std::min(first, clip.paras[k].cell);
  for (size_t k = b; k < e; ++k) {
    const ClipPara& p = clip.paras[k];
    if (k > b && SameCell(clip.paras[k - 1], p)) {
      s.cells.back().end = k + 1;
      continue;
    }
    if (k > b && !SameRow(clip.paras[k - 1], p)) ++s.rows;
    s.cells.push_back({s.rows, static_cast<int32_t>(std::min<uint32_t>(p.cell - first, kMaxColumns)), k, k + 1});
  }
  ++s.rows;
  std::stable_sort(s.cells.begin(), s.cells.end(),
                   [](const CellText& x, const CellText& y) { return x.row != y.row ? x.row < y.row : x.col < y.col; });
  // A place twice (a source that says so) keeps the first; past the widest table, none.
  s.cells.erase(std::unique(s.cells.begin(), s.cells.end(),
                            [](const CellText& x, const CellText& y) { return x.row == y.row && x.col == y.col; }),
                s.cells.end());
  s.cells.erase(std::remove_if(s.cells.begin(), s.cells.end(), [](const CellText& c) { return c.col >= kMaxColumns; }),
                s.cells.end());
  for (const CellText& c : s.cells) s.cols = std::max(s.cols, c.col + 1);
  return s;
}

Result Failed(std::u16string message) {
  Result r;
  r.ok = false;
  r.message = std::move(message);
  return r;
}

// A cell's paragraphs into the cell at [from, to) (all of its text): each with its mark, so each
// gets its properties, then the cell's own paragraph joined to the last.
Result Fill(Editor& ed, Pos from, Pos to, const Clip& clip, const CellText& cell, uint64_t target) {
  Clip part;
  part.source = clip.source;
  for (size_t k = cell.begin; k < cell.end; ++k) {
    ClipPara p = clip.paras[k];
    p.table = p.row = p.cell = 0;
    part.paras.push_back(std::move(p));
  }
  Result r;
  r.caret = from;
  if (part.paras.size() == 1 && part.paras[0].runs.empty() && part.paras[0].label.empty()) {
    // An empty cell: the cell's own paragraph, emptied.
    return from == to ? r : ed.Delete(from, to);
  }
  part.paras.emplace_back();
  r = PasteClip(ed, from, to, part, target);
  if (!r) return r;
  if (r.caret.para == 0 || r.caret.offset != 0) return Failed(u"The pasted cell didn't land where it should.");
  return ed.JoinParagraphs(r.caret.para - 1);
}

// In body text: a new table at `caret` (between the halves of its paragraph), then the caret at the
// start of the paragraph after it.
Result NewTable(Editor& ed, Pos caret, const Clip& clip, size_t b, size_t e, uint64_t target) {
  const Shape s = ShapeOf(clip, b, e);
  if (s.cells.empty()) {
    Result r;
    r.caret = caret;
    return r;
  }
  if (caret.offset != 0) {
    Result r = ed.SplitParagraph(caret);
    if (!r) return r;
    caret = r.caret;
  }
  const Document& doc = ed.Doc();
  const size_t before = doc.Paragraphs().size();
  const auto cols = static_cast<uint32_t>(s.cols);
  Result r = ed.InsertTable(caret, s.rows, cols);
  if (!r) return r;
  // The table went in before the caret's paragraph, an empty paragraph a cell.
  const uint32_t first = r.caret.para;
  if (first != caret.para || doc.Paragraphs().size() != before + static_cast<size_t>(s.rows) * cols)
    return Failed(u"The pasted table didn't land where it should.");
  // From the last cell back, so that the cells before keep their places.
  for (auto it = s.cells.rbegin(); it != s.cells.rend(); ++it) {
    const Pos at{first + it->row * cols + static_cast<uint32_t>(it->col), 0};
    if (Result f = Fill(ed, at, at, clip, *it, target); !f) return f;
  }
  Result ok;
  ok.caret = Pos{caret.para + static_cast<uint32_t>(doc.Paragraphs().size() - before), 0};
  return ok;
}

// In a cell: the clip's cells over the table's from there, rows added below as needed.
Result Overwrite(Editor& ed, Pos caret, const Clip& clip, size_t b, size_t e, uint64_t target) {
  const Shape s = ShapeOf(clip, b, e);
  auto start = ed.CellAt(caret);
  if (!start) return Failed(u"Put the cursor in a cell first.");
  const Pos home = start->start;
  std::vector<Editor::CellInfo> cells = ed.TableCells(home);
  if (cells.empty()) return Failed(u"This table can't take the pasted cells.");
  // Rows: the table's last ones copied below it (as many as missing, or all of it), until enough.
  for (uint32_t have = start->tableRows, need = start->row + s.rows; have < need;) {
    const uint32_t k = std::min(need - have, have);
    auto from = std::find_if(cells.begin(), cells.end(), [&](const Editor::CellInfo& c) { return c.row >= have - k; });
    if (from == cells.end()) return Failed(u"This table can't take the pasted rows.");
    Result r = ed.InsertRows(from->start, cells.back().start, true);
    if (!r) return r;
    cells = ed.TableCells(home);
    if (cells.empty() || cells.front().tableRows <= have) return Failed(u"This table can't take the pasted rows.");
    have = cells.front().tableRows;
  }
  // Each of the clip's cells over the table's cell at its place; a merged cell takes the first.
  std::map<std::pair<uint32_t, int32_t>, size_t> at;  // (row, column) -> the cell there
  for (size_t i = 0; i < cells.size(); ++i)
    for (uint32_t r = 0; r < cells[i].rows; ++r)
      for (int32_t c = 0; c < cells[i].span; ++c) at.emplace(std::make_pair(cells[i].row + r, cells[i].col + c), i);
  std::vector<std::pair<size_t, const CellText*>> fills;  // the table's cell, the clip's
  std::vector<bool> taken(cells.size());
  for (const CellText& c : s.cells) {
    auto it = at.find({start->row + c.row, start->col + c.col});
    if (it == at.end() || taken[it->second]) continue;  // past the table's edge, or merged
    taken[it->second] = true;
    fills.push_back({it->second, &c});
  }
  // From the last cell in the document back, so that the cells before keep their places.
  std::sort(fills.begin(), fills.end(), [&](const auto& x, const auto& y) {
    return cells[x.first].start.para > cells[y.first].start.para;
  });
  for (const auto& [i, c] : fills)
    if (Result f = Fill(ed, cells[i].start, cells[i].end, clip, *c, target); !f) return f;
  Result ok;
  ok.caret = home;
  return ok;
}

// The clip with its tables as text: a row a paragraph, its cells separated by tabs (a cell's
// paragraphs by spaces).
Clip Flatten(const Clip& clip) {
  Clip out;
  out.source = clip.source;
  for (size_t i = 0; i < clip.paras.size(); ++i) {
    const ClipPara& p = clip.paras[i];
    if (!p.table || i == 0 || !SameRow(clip.paras[i - 1], p)) {
      ClipPara q = p;
      q.table = q.row = q.cell = 0;
      out.paras.push_back(std::move(q));
      continue;
    }
    ClipRun gap = p.runs.empty() ? ClipRun{} : p.runs.front();
    gap.text = SameCell(clip.paras[i - 1], p) ? u" " : u"\t";
    gap.link.clear();
    ClipPara& row = out.paras.back();
    row.runs.push_back(std::move(gap));
    row.runs.insert(row.runs.end(), p.runs.begin(), p.runs.end());
  }
  return out;
}

}  // namespace

CellMarker::CellMarker(const Document& doc, uint32_t first, uint32_t last) : doc_(doc) {
  const std::vector<Node> a = CellsAround(doc, first), b = CellsAround(doc, last);
  for (const Node& c : a)  // innermost first: the first both have is the innermost they share
    if (std::any_of(b.begin(), b.end(), [&](const Node& d) { return d.Id() == c.Id(); })) {
      base_ = c.Id();
      break;
    }
}

void CellMarker::Mark(uint32_t para, ClipPara& cp) {
  Node tc;  // the outermost cell around the paragraph below the base
  for (const Node& c : CellsAround(doc_, para)) {
    if (c.Id() == base_) break;
    tc = c;
  }
  const Node tr = tc ? Up(tc.Parent(), Name("tr")) : Node();
  const Node tbl = tr ? Up(tr.Parent(), Name("tbl")) : Node();
  if (!tbl) {
    table_ = kNoElem;
    return;
  }
  if (tbl.Id() != table_) {
    table_ = tbl.Id();
    ++tables_;
    rows_.clear();
    cells_.clear();
    uint32_t r = 0;
    for (Node row : doc_.TableRows(tbl)) {
      rows_[row.Id()] = r++;
      uint32_t i = 0;
      for (Node cell : doc_.RowCells(row)) {
        // Its column in the table's grid, so that rows line up past merged cells.
        const int32_t col = doc_.CellGridColumn(cell);
        cells_[cell.Id()] = col >= 0 ? static_cast<uint32_t>(col) : i;
        ++i;
      }
    }
  }
  cp.table = tables_;
  auto row = rows_.find(tr.Id());
  auto cell = cells_.find(tc.Id());
  cp.row = row == rows_.end() ? 0 : row->second;
  cp.cell = cell == cells_.end() ? 0 : cell->second;
}

Result PasteTables(Editor& ed, Pos from, Pos to, const Clip& clip, uint64_t target) {
  if (ed.Compare(from, to) > 0) std::swap(from, to);
  const bool inCell = ed.CellAt(from).has_value();
  size_t b = SIZE_MAX, e = 0;  // in a cell: the one table, with nothing around it but empty paragraphs
  if (inCell) {
    bool one = true;
    for (size_t i = 0; i < clip.paras.size(); ++i) {
      const ClipPara& p = clip.paras[i];
      if (p.table) {
        if (b != SIZE_MAX && p.table != clip.paras[b].table) one = false;
        if (b == SIZE_MAX) b = i;
        e = i + 1;
      } else if (!p.runs.empty() || !p.label.empty()) {
        one = false;
      }
    }
    if (!one || b == SIZE_MAX) return PasteClip(ed, from, to, Flatten(clip), target);
  }
  ed.BeginGroup(u"Paste");
  auto fail = [&](Result r) {
    ed.CancelGroup();
    r.ok = false;
    return r;
  };
  Pos caret = from;
  if (!(from == to)) {
    Result r = ed.Delete(from, to);
    if (!r) return fail(r);
    caret = r.caret;
  }
  if (inCell) {
    Result r = Overwrite(ed, caret, clip, b, e, target);
    if (!r) return fail(r);
    ed.EndGroup();
    return r;
  }
  // In body text: the text between tables as it comes, each table a new one.
  for (size_t i = 0, n = clip.paras.size(); i < n;) {
    size_t j = i;
    Result r;
    if (!clip.paras[i].table) {
      while (j < n && !clip.paras[j].table) ++j;
      Clip part;
      part.source = clip.source;
      part.paras.assign(clip.paras.begin() + static_cast<std::ptrdiff_t>(i), clip.paras.begin() + static_cast<std::ptrdiff_t>(j));
      if (j < n) part.paras.emplace_back();  // the text before a table ends with its paragraph mark
      r = PasteClip(ed, caret, caret, part, target);
    } else {
      while (j < n && clip.paras[j].table == clip.paras[i].table) ++j;
      r = NewTable(ed, caret, clip, i, j, target);
    }
    if (!r) return fail(r);
    caret = r.caret;
    i = j;
  }
  ed.EndGroup();
  Result ok;
  ok.caret = caret;
  return ok;
}

}  // namespace cliptable

std::optional<Clip> CopyCells(const Editor& ed, Pos from, Pos to, uint64_t source) {
  auto a = ed.CellAt(from), b = ed.CellAt(to);
  if (!a || !b || a->tbl.Id() != b->tbl.Id() || a->tc.Id() == b->tc.Id()) return std::nullopt;
  auto block = ed.BlockAt(from, to);
  if (!block || block->cells.empty()) return std::nullopt;
  std::vector<const Editor::CellInfo*> cells;
  for (const Editor::CellInfo& c : block->cells) cells.push_back(&c);
  std::stable_sort(cells.begin(), cells.end(), [](const Editor::CellInfo* x, const Editor::CellInfo* y) {
    return x->row != y->row ? x->row < y->row : x->col < y->col;
  });
  Clip clip;
  clip.source = source;
  for (const Editor::CellInfo* c : cells) {
    Clip part = CopyClip(ed.Doc(), c->start, c->end, source);
    if (part.paras.empty()) part.paras.emplace_back();
    for (ClipPara& p : part.paras) {
      // A table nested in the cell comes as its text.
      p.table = 1;
      p.row = c->row - block->row0;
      p.cell = static_cast<uint32_t>(c->col - block->col0);
      clip.paras.push_back(std::move(p));
    }
  }
  return clip;
}

}  // namespace fw::doc::edit
