#include "slide/render/Table.h"

#include <algorithm>

#include "text/Fonts.h"

namespace fw::slide::render {

namespace {

std::vector<float> Dashes(std::string_view v) {
  if (v == "dash" || v == "sysDash") return {3, 1};
  if (v == "dot" || v == "sysDot") return {1, 1};
  if (v == "lgDash") return {8, 3};
  if (v == "dashDot" || v == "sysDashDot") return {4, 3, 1, 3};
  if (v == "lgDashDot") return {8, 3, 1, 3};
  if (v == "lgDashDotDot" || v == "sysDashDotDot") return {8, 3, 1, 3, 1, 3};
  return {};
}

draw::TextBox BoxFor(const fw::slide::TableCell& c, const draw::Rect& r) {
  draw::TextBox box;
  box.insets = {c.marginLeft, c.marginTop, c.marginRight, c.marginBottom};
  box.rect = {r.left + c.marginLeft, r.top + c.marginTop, std::max(r.left + c.marginLeft, r.right - c.marginRight),
              std::max(r.top + c.marginTop, r.bottom - c.marginBottom)};
  using A = fw::slide::BodyProps::Anchor;
  box.anchor = c.anchor == A::Middle ? draw::TextBox::Anchor::Middle : c.anchor == A::Bottom ? draw::TextBox::Anchor::Bottom
                                                                                             : draw::TextBox::Anchor::Top;
  using V = fw::slide::BodyProps::Vertical;
  box.flow = c.vertical == V::Vertical || c.vertical == V::EastAsian ? draw::TextBox::Flow::Vertical
             : c.vertical == V::Vertical270                         ? draw::TextBox::Flow::Vertical270
                                                                    : draw::TextBox::Flow::Horizontal;
  return box;
}

}  // namespace

TableRender::TableRender(const fw::slide::Table& t, text::Fonts& fonts, const TextExtras& extras) : table_(t) {
  const int cols = static_cast<int>(t.columns.size()), rows = static_cast<int>(t.rows.size());
  colX_.assign(cols + 1, 0);
  for (int c = 0; c < cols; ++c) colX_[c + 1] = colX_[c] + std::max(0.f, t.columns[c]);
  std::vector<float> heights(t.rows.begin(), t.rows.end());
  // A column's x, right to left when the table says so.
  auto span = [&](int c, int n) {
    const float l = colX_[c], r = colX_[std::min(cols, c + n)];
    return t.rtl ? std::pair{colX_.back() - r, colX_.back() - l} : std::pair{l, r};
  };
  // Rows grow to their text, laid out at the cells' widths.
  for (const fw::slide::TableCell& c : t.cells) {
    if (c.covered || c.row >= rows || c.column >= cols) continue;
    Cell& cell = cells_.emplace_back();
    cell.cell = &c;
    if (!c.text || c.rowSpan != 1 || c.vertical != fw::slide::BodyProps::Vertical::Horizontal) continue;
    const auto [l, r] = span(c.column, c.columnSpan);
    cell.text.Build(fonts, BoxFor(c, {l, 0, r, 1}), *c.text, extras);
    heights[c.row] = std::max(heights[c.row], cell.text.ContentHeight() + c.marginTop + c.marginBottom);
  }
  rowY_.assign(rows + 1, 0);
  for (int r = 0; r < rows; ++r) rowY_[r + 1] = rowY_[r] + std::max(0.f, heights[r]);
  for (Cell& cell : cells_) {
    const fw::slide::TableCell& c = *cell.cell;
    const auto [l, r] = span(c.column, c.columnSpan);
    cell.rect = {l, rowY_[c.row], r, rowY_[std::min(rows, c.row + c.rowSpan)]};
    if (c.text) cell.text.Build(fonts, BoxFor(c, cell.rect), *c.text, extras);
  }
}

void TableRender::Draw(draw::Painter& p, DrawFilter* filter, uint32_t shape) const {
  const bool body = !filter || filter->ShapeBody(shape);
  auto fill = [&](const fw::slide::CellFill& f, const draw::Rect& r) {
    if (f.kind != fw::slide::CellFill::Kind::None) p.FillRect(r, draw::Brush::Solid(f.color));
  };
  if (body) {
    fill(table_.background, {0, 0, colX_.back(), rowY_.back()});
    for (const Cell& c : cells_) fill(c.cell->fill, c.rect);
  }
  // Paragraphs are counted across the cells, in reading order.
  struct Counting : DrawFilter {
    DrawFilter* inner = nullptr;
    int base = 0;
    bool Paragraph(uint32_t id, int index) override { return !inner || inner->Paragraph(id, base + index); }
  } counting;
  counting.inner = filter;
  for (const Cell& c : cells_) {
    c.text.Draw(p, &counting, shape);
    counting.base += static_cast<int>(c.text.ParagraphBounds().size());
  }
  if (!body) return;
  for (const Cell& c : cells_) {
    const draw::Rect& r = c.rect;
    const draw::Point ends[6][2] = {{{r.left, r.top}, {r.left, r.bottom}},  {{r.right, r.top}, {r.right, r.bottom}},
                                    {{r.left, r.top}, {r.right, r.top}},    {{r.left, r.bottom}, {r.right, r.bottom}},
                                    {{r.left, r.top}, {r.right, r.bottom}}, {{r.left, r.bottom}, {r.right, r.top}}};
    for (size_t e = 0; e < 6; ++e) {
      const fw::slide::CellBorder& b = c.cell->borders[e];
      if (!b.visible || b.width <= 0) continue;
      draw::Path path;
      path.MoveTo(ends[e][0]);
      path.LineTo(ends[e][1]);
      draw::Pen pen;
      pen.brush = draw::Brush::Solid(b.color);
      pen.width = b.width;
      pen.dashes = Dashes(b.dash);
      p.StrokePath(path, pen);
    }
  }
}

std::vector<draw::Rect> TableRender::ParagraphBounds() const {
  std::vector<draw::Rect> out;
  for (const Cell& c : cells_)
    for (const draw::Rect& r : c.text.ParagraphBounds()) out.push_back(r);
  return out;
}

std::vector<TextBlock::Link> TableRender::Links() const {
  std::vector<TextBlock::Link> out;
  for (const Cell& c : cells_)
    for (TextBlock::Link& l : c.text.Links()) out.push_back(std::move(l));
  return out;
}

std::wstring TableRender::PlainText() const {
  std::wstring out;
  int row = -1;
  for (const Cell& c : cells_) {
    if (row >= 0) out += c.cell->row != row ? L'\n' : L'\t';
    row = c.cell->row;
    out += c.text.PlainText();
  }
  return out;
}

}  // namespace fw::slide::render
