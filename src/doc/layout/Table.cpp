// Tables (Engine.h): the grid Word saved, cell margins, row heights and vertical merges; cell
// shading and borders, with conflicts between neighboring cells resolved the way Word does.
#include <algorithm>
#include <numeric>

#include "doc/layout/Engine.h"

namespace fw::doc::layout {

namespace {

float WidthPt(const TableWidth& w, float fallback, float whole = 0) {
  switch (w.type) {
    case TableWidth::Type::Dxa: return static_cast<float>(w.value) / 20.0f;
    case TableWidth::Type::Pct: return whole > 0 ? whole * static_cast<float>(w.value) / 5000.0f : fallback;
    case TableWidth::Type::Nil: return 0;
    default: return fallback;
  }
}

// Word's weight of a border, for which of two touching borders wins: wider first, then the style
// that looks heavier, then the darker color.
float Weight(const Border& b) {
  if (!b.Present()) return -1;
  float w = Pt(b.width);
  switch (b.style) {
    case BorderStyle::Double: case BorderStyle::Triple: w *= 3; break;
    default: break;
  }
  return w;
}
const Border& Stronger(const Border& a, const Border& b) {
  float wa = Weight(a), wb = Weight(b);
  if (wa != wb) return wa > wb ? a : b;
  if (a.Present() && b.Present()) {
    uint32_t ca = a.color.automatic ? 0 : a.color.rgb, cb = b.color.automatic ? 0 : b.color.rgb;
    auto luma = [](uint32_t c) { return ((c >> 16) & 255) * 299 + ((c >> 8) & 255) * 587 + (c & 255) * 114; };
    return luma(ca) <= luma(cb) ? a : b;
  }
  return a.Present() ? a : b;
}

}  // namespace

Engine::TableBox Engine::MeasureTable(Node tbl, const Frame& frame) {
  TableBox t;
  t.tbl = tbl;
  t.props = doc_.ResolvedTable(tbl);
  std::vector<float> grid;
  for (Twips w : doc_.TableGrid(tbl)) grid.push_back(std::max(0.0f, Pt(w)));
  std::vector<Node> rows = doc_.TableRows(tbl);

  // Without a usable grid, the first row's cell widths make one.
  float gridSum = std::accumulate(grid.begin(), grid.end(), 0.0f);
  if (gridSum <= 0 && !rows.empty()) {
    grid.clear();
    std::vector<Node> cells = doc_.RowCells(rows.front());
    float fallback = cells.empty() ? frame.width : frame.width / static_cast<float>(cells.size());
    for (Node tc : cells) {
      CellProps cp = doc_.ResolvedCell(tc);
      float w = WidthPt(cp.width, fallback, frame.width);
      for (int32_t s = 0; s < std::max(1, cp.gridSpan); ++s) grid.push_back(w / static_cast<float>(std::max(1, cp.gridSpan)));
    }
    gridSum = std::accumulate(grid.begin(), grid.end(), 0.0f);
  }
  std::vector<float> at(grid.size() + 1, 0);  // grid line positions
  for (size_t i = 0; i < grid.size(); ++i) at[i + 1] = at[i] + grid[i];
  auto gridX = [&](int32_t col) {
    if (col <= 0) return 0.0f;
    if (static_cast<size_t>(col) < at.size()) return at[col];
    return at.back();
  };

  const CellMargins& tm = t.props.cellMargins;
  const float defLeft = WidthPt(tm.start, 5.4f), defRight = WidthPt(tm.end, 5.4f);
  const float defTop = WidthPt(tm.top, 0), defBottom = WidthPt(tm.bottom, 0);

  t.width = gridSum;
  float indent = t.props.indent.type == TableWidth::Type::Dxa ? static_cast<float>(t.props.indent.value) / 20.0f : 0;
  switch (t.props.jc) {
    case Justification::Center: t.left = (frame.width - t.width) / 2; break;
    case Justification::End: t.left = frame.width - t.width; break;
    default: t.left = indent; break;
  }

  bool leading = true;
  for (Node tr : rows) {
    RowBox rb;
    rb.tr = tr;
    rb.props = doc_.ResolvedRow(tr);
    rb.header = leading && rb.props.header;
    if (!rb.props.header) leading = false;
    int32_t col = rb.props.gridBefore;
    for (Node tc : doc_.RowCells(tr)) {
      CellBox c;
      c.tc = tc;
      c.props = doc_.ResolvedCell(tc);
      c.col = col;
      c.span = std::max(1, c.props.gridSpan);
      c.x = gridX(col);
      c.width = gridX(col + c.span) - c.x;
      if (c.width <= 0) c.width = WidthPt(c.props.width, 36, t.width);
      c.continued = c.props.vMerge == VMerge::Continue;
      c.rowSpan = c.props.vMerge == VMerge::Restart ? std::max(1, doc_.CellRowSpan(tc)) : 1;
      const CellMargins& m = c.props.margins;
      c.marginLeft = WidthPt(m.start, defLeft);
      c.marginRight = WidthPt(m.end, defRight);
      c.marginTop = WidthPt(m.top, defTop);
      c.marginBottom = WidthPt(m.bottom, defBottom);
      if (!c.continued) {
        Frame cf = frame;
        cf.x = 0;
        cf.y = 0;
        cf.width = std::max(1.0f, c.width - c.marginLeft - c.marginRight);
        if (frame.story == LineStory::Body) cf.story = LineStory::Cell;
        cf.zones = nullptr;
        cf.inCell = true;
        Spacing sp;
        std::vector<Zone> zones;
        cf.zones = &zones;
        StackBlocks(doc_.Blocks(tc), cf, c.content, sp);
        c.content.height += BorderBelow(sp);
      }
      col += c.span;
      if (rb.cells.empty()) {
        std::vector<Block> blocks = doc_.Blocks(tc);
        rb.breakBefore = !blocks.empty() && blocks.front().kind == BlockKind::Paragraph &&
                         doc_.ResolvedPara(blocks.front().para).pageBreakBefore;
      }
      rb.cells.push_back(std::move(c));
    }
    // Horizontal borders set on the cells themselves (w:tcBorders) take room inside them: the
    // text starts below the stronger of a cell's top border and the bottom border of the cell
    // above, and the last row's ends above its bottom border. The table's and its style's
    // borders don't (measured on the corpus: counting those too makes rows of style-bordered
    // tables taller than Word's).
    {
      auto own = [&](Node tc, uint32_t side) -> float {
        Node b = tc.Child(Name("tcPr")).Child(Name("tcBorders")).Child(side);
        if (!b) return 0;
        std::string_view v = b.AttrOr(Name("val"));
        if (v.empty() || v == "nil" || v == "none") return 0;
        return static_cast<float>(ParseInt(b.AttrOr(Name("sz"))).value_or(0)) / 8.0f;
      };
      const RowBox* above = t.rows.empty() ? nullptr : &t.rows.back();
      const bool lastRow = tr == rows.back();
      for (CellBox& c : rb.cells) {
        if (c.continued) continue;
        float top = own(c.tc, Name("top"));
        if (above)
          for (const CellBox& a : above->cells)
            if (c.x >= a.x - 0.5f && c.x < a.x + a.width - 0.5f) top = std::max(top, own(a.tc, Name("bottom")));
        c.marginTop += top;
        if (lastRow) c.marginBottom += own(c.tc, Name("bottom"));
      }
    }
    float h = 0;
    for (const CellBox& c : rb.cells)
      if (!c.continued && c.rowSpan == 1) h = std::max(h, c.content.height + c.marginTop + c.marginBottom);
    float spec = Pt(rb.props.height);
    if (rb.props.heightRule == "exact" && spec > 0) h = spec;
    else if (spec > 0) {
      float mv = 0;
      for (const CellBox& c : rb.cells) mv = std::max(mv, c.marginTop + c.marginBottom);
      h = std::max(h, spec + mv);
    }
    rb.height = rb.props.hidden ? 0 : h;
    t.rows.push_back(std::move(rb));
  }
  // A vertically merged cell taller than the rows it spans makes the last of them taller.
  for (size_t r = 0; r < t.rows.size(); ++r)
    for (const CellBox& c : t.rows[r].cells) {
      if (c.continued || c.rowSpan <= 1) continue;
      size_t last = std::min(t.rows.size() - 1, r + static_cast<size_t>(c.rowSpan) - 1);
      float have = 0;
      for (size_t k = r; k <= last; ++k) have += t.rows[k].height;
      float need = c.content.height + c.marginTop + c.marginBottom;
      if (need > have && t.rows[last].props.heightRule != "exact") {
        t.rows[last].height += need - have;
        t.rows[last].grown += need - have;
      }
    }
  for (const RowBox& r : t.rows)
    if (r.header) ++t.headerRows;
    else break;
  return t;
}

float Engine::VShift(const CellBox& c, float room) {
  if (c.content.height >= room) return 0;
  if (c.props.vAlign == "center") return (room - c.content.height) / 2;
  if (c.props.vAlign == "bottom") return room - c.content.height;
  return 0;
}

void Engine::EmitCell(const CellBox& c, float x, float top, float from, float to, float vshift, const Rect* clip,
                      Stack& out) {
  const Stack& s = c.content;
  uint32_t base = static_cast<uint32_t>(out.paints.size());
  out.paints.insert(out.paints.end(), s.paints.begin(), s.paints.end());
  float dx = x + c.x + c.marginLeft, dy = top + c.marginTop - from + vshift;
  for (const LineBox& l : s.lines) {
    if (l.top < from - 0.01f || l.top >= to - 0.01f) continue;
    LineBox copy = l;
    copy.x += dx;
    copy.top += dy;
    copy.baseline += dy;
    if (copy.clip.Width() > 0) copy.clip = {copy.clip.left + dx, copy.clip.top + dy, copy.clip.right + dx, copy.clip.bottom + dy};
    if (clip) copy.clip = *clip;
    copy.paint += base;
    out.lines.push_back(std::move(copy));
  }
  for (const Fill& f : s.fills)
    if (f.rect.top >= from - 0.01f && f.rect.top < to - 0.01f)
      out.fills.push_back({{f.rect.left + dx, f.rect.top + dy, f.rect.right + dx, f.rect.bottom + dy}, f.color});
  for (const Edge& e : s.edges)
    if (e.y1 >= from - 0.01f && e.y1 < to - 0.01f) out.edges.push_back({e.border, e.x1 + dx, e.y1 + dy, e.x2 + dx, e.y2 + dy});
  for (const DrawingBox& d : s.drawings)
    if (d.rect.top >= from - 0.01f && d.rect.top < to - 0.01f) {
      DrawingBox copy = d;
      copy.rect = {d.rect.left + dx, d.rect.top + dy, d.rect.right + dx, d.rect.bottom + dy};
      out.drawings.push_back(copy);
    }
}

void Engine::EmitRow(const TableBox& t, size_t r, float x, float top, float height, const std::vector<float>* cutFrom,
                     const std::vector<float>* cutTo, Stack& out, bool firstOnPage, bool lastOnPage, bool spans) {
  const RowBox& row = t.rows[r];
  const RowBox* above = r > 0 ? &t.rows[r - 1] : nullptr;
  const RowBox* below = r + 1 < t.rows.size() ? &t.rows[r + 1] : nullptr;
  auto cellAt = [](const RowBox* rb, float cx) -> const CellBox* {
    if (!rb) return nullptr;
    for (const CellBox& c : rb->cells)
      if (cx >= c.x - 0.5f && cx < c.x + c.width - 0.5f) return &c;
    return nullptr;
  };
  for (size_t i = 0; i < row.cells.size(); ++i) {
    const CellBox& c = row.cells[i];
    float left = x + c.x, right = left + c.width, bottom = top + height;
    // A merged cell's box reaches down through the rows it spans (on this page).
    const CellBox* owner = &c;
    if (c.continued) {
      for (size_t k = r; k-- > 0;) {
        const CellBox* up = cellAt(&t.rows[k], c.x + 1);
        if (up && !up->continued) {
          owner = up;
          break;
        }
        if (!up) break;
      }
    }
    if (c.props.shading.Present() && !c.props.shading.fill.automatic)
      out.fills.push_back({{left, top, right, bottom}, c.props.shading.fill.rgb});
    // Content: a restart cell's lines go in its first row (unless the caller places merged cells'
    // text itself); split rows show their part.
    if (!c.continued && (spans || c.rowSpan <= 1)) {
      float from = cutFrom ? (*cutFrom)[i] : 0;
      float to = cutTo ? (*cutTo)[i] : 1e30f;
      float vshift = 0;
      float room = height - c.marginTop - c.marginBottom;
      if (c.rowSpan > 1) {
        room = 0;
        for (size_t k = r; k < std::min(t.rows.size(), r + static_cast<size_t>(c.rowSpan)); ++k) room += t.rows[k].height;
        room -= c.marginTop + c.marginBottom;
      }
      if (!cutFrom && !cutTo) vshift = VShift(c, room);
      const Rect box{left, top, right, bottom};
      EmitCell(c, x, top, from, to, vshift, row.props.heightRule == "exact" ? &box : nullptr, out);
    }
    // Borders: the cell's own, against its neighbor's (the stronger wins; each shared edge once).
    const CellProps& p = owner->props;
    Border topB = p.top, bottomB = p.bottom, leftB = p.start, rightB = p.end;
    if (!firstOnPage && above && !c.continued)
      if (const CellBox* a = cellAt(above, c.x + 1)) topB = Stronger(topB, a->props.bottom);
    if (!lastOnPage && below) {
      const CellBox* b = cellAt(below, c.x + 1);
      if (b && b->continued) bottomB = Border();  // the merged cell goes on
      else if (b) bottomB = Border();             // drawn as the next row's top
    }
    if (c.continued) topB = Border();
    if (i > 0) leftB = Border();  // drawn as the previous cell's right edge
    if (i + 1 < row.cells.size()) rightB = Stronger(rightB, row.cells[i + 1].props.start);
    if (topB.Present()) out.edges.push_back({topB, left, top, right, top});
    if (bottomB.Present()) out.edges.push_back({bottomB, left, bottom, right, bottom});
    if (leftB.Present()) out.edges.push_back({leftB, left, top, left, bottom});
    if (rightB.Present()) out.edges.push_back({rightB, right, top, right, bottom});
  }
}

}  // namespace fw::doc::layout
