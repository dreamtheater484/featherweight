#include "sheet/view/CellPaint.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace fw::sheet::view {

draw::Color ToColor(uint32_t argb) {
  return {((argb >> 16) & 0xFF) / 255.f, ((argb >> 8) & 0xFF) / 255.f, (argb & 0xFF) / 255.f, ((argb >> 24) & 0xFF) / 255.f};
}

namespace {

constexpr uint32_t kOverflowReach = 64;  // columns a text may run into beside the drawn ones

draw::Color Mix(uint32_t a, uint32_t b, float t) {
  const draw::Color x = ToColor(a), y = ToColor(b);
  return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, 1};
}

// How much of a pattern's foreground shows (Excel's 8x8 patterns), for drawing it as a tint.
float PatternShare(PatternType t) {
  switch (t) {
    case PatternType::Solid: return 1;
    case PatternType::DarkGray: return 0.75f;
    case PatternType::MediumGray: return 0.5f;
    case PatternType::LightGray: return 0.25f;
    case PatternType::Gray125: return 0.125f;
    case PatternType::Gray0625: return 0.0625f;
    case PatternType::DarkHorizontal:
    case PatternType::DarkVertical:
    case PatternType::DarkDown:
    case PatternType::DarkUp:
    case PatternType::DarkGrid:
    case PatternType::DarkTrellis: return 0.5f;
    default: return 0.25f;
  }
}

// A border style's lines: widths in Excel pixels and the dash pattern (on, off... in pixels).
struct LineKind {
  int width = 1;
  bool doubled = false;
  std::vector<int> dashes;
  int weight = 0;  // which wins where two meet: the heavier
};

LineKind KindOf(BorderStyle s) {
  switch (s) {
    case BorderStyle::Thin: return {1, false, {}, 1};
    case BorderStyle::Hair: return {1, false, {1, 1}, 0};
    case BorderStyle::Dotted: return {1, false, {1, 1}, 1};
    case BorderStyle::Dashed: return {1, false, {3, 1}, 1};
    case BorderStyle::DashDot: return {1, false, {3, 1, 1, 1}, 1};
    case BorderStyle::DashDotDot: return {1, false, {3, 1, 1, 1, 1, 1}, 1};
    case BorderStyle::Medium: return {2, false, {}, 3};
    case BorderStyle::MediumDashed: return {2, false, {9, 3}, 2};
    case BorderStyle::MediumDashDot: return {2, false, {9, 3, 3, 3}, 2};
    case BorderStyle::MediumDashDotDot: return {2, false, {9, 3, 3, 3, 3, 3}, 2};
    case BorderStyle::SlantDashDot: return {2, false, {9, 1, 3, 1}, 2};
    case BorderStyle::Thick: return {3, false, {}, 5};
    case BorderStyle::Double: return {1, true, {}, 4};
    default: return {0, false, {}, -1};
  }
}

struct Segment {
  float a0, a1;   // along the line
  float b;        // the boundary across it (the gridline's far edge)
  bool vertical;
  BorderStyle style;
  uint32_t color;
};

struct RowData {
  uint32_t row;
  uint32_t style = kNone;  // the row's format (customFormat), else kNone
  std::vector<CellInfo> cells;  // stored cells, by column
  std::vector<std::pair<uint32_t, uint32_t>> spans;  // text running across columns: first, last
  const CellInfo* Find(uint32_t col) const {
    auto it = std::lower_bound(cells.begin(), cells.end(), col, [](const CellInfo& c, uint32_t x) { return c.col < x; });
    return it != cells.end() && it->col == col ? &*it : nullptr;
  }
  bool SpanCrosses(uint32_t a, uint32_t b) const {  // the boundary between columns a and b = a + 1
    for (const auto& s : spans)
      if (s.first <= a && b <= s.second) return true;
    return false;
  }
};

class Painting {
 public:
  Painting(draw::Painter& p, const Sheet& sheet, CellText& text, const Grid& grid, const Geometry& g, const Range& area,
           const PaintOptions& o)
      : p_(p), sheet_(sheet), text_(text), grid_(grid), g_(g), area_(area), o_(o),
        styles_(text.Book().GetStyles()) {}

  void Run() {
    for (uint32_t r = area_.firstRow; r <= area_.lastRow && r < kMaxRows; ++r)
      if (!grid_.RowHidden(r)) rows_.push_back({r});
    for (uint32_t c = area_.firstCol; c <= area_.lastCol && c < kMaxCols; ++c)
      if (!grid_.ColHidden(c)) cols_.push_back(c);
    if (rows_.empty() || cols_.empty()) return;
    for (const Range& m : sheet_.Merges())
      if (m.lastRow >= area_.firstRow && m.firstRow <= area_.lastRow && m.lastCol >= area_.firstCol &&
          m.firstCol <= area_.lastCol)
        merges_.push_back(&m);
    reachFirst_ = area_.firstCol > kOverflowReach ? area_.firstCol - kOverflowReach : 0;
    reachLast_ = std::min(area_.lastCol + kOverflowReach, kMaxCols - 1);
    size_t at = 0;
    sheet_.ForEachCell(Range{rows_.front().row, reachFirst_, rows_.back().row, reachLast_}, [&](const CellInfo& c) {
      while (at < rows_.size() && rows_[at].row < c.row) ++at;
      if (at < rows_.size() && rows_[at].row == c.row) rows_[at].cells.push_back(c);
      return true;
    });
    for (RowData& rd : rows_) {
      const RowInfo info = sheet_.Row(rd.row);
      if (info.customFormat) rd.style = info.style;
    }
    colStyle_.resize(cols_.size());
    for (size_t i = 0; i < cols_.size(); ++i) colStyle_[i] = sheet_.Column(cols_[i]).style;

    PlaceText();
    if (o_.gridlines) Gridlines();
    Fills();
    Borders();
    DrawText();
  }

 private:
  // ---- Helpers
  const Range* MergeAt(uint32_t r, uint32_t c) const {
    for (const Range* m : merges_)
      if (m->Contains(r, c)) return m;
    return nullptr;
  }
  uint32_t StyleAt(const RowData& rd, size_t colIndex) const {
    if (const CellInfo* c = rd.Find(cols_[colIndex])) return c->style;
    return rd.style != kNone ? rd.style : colStyle_[colIndex];
  }
  // A cell text can run into: no value and not merged.
  bool Empty(const RowData& rd, uint32_t c) const {
    if (c < reachFirst_ || c > reachLast_) return false;
    const CellInfo* x = rd.Find(c);
    if (x && x->value.type != ValueType::Empty) return false;
    return !MergeAt(rd.row, c);
  }
  // The cell's inner box: without its gridline (right in a left-to-right sheet, left otherwise).
  draw::Rect Inner(uint32_t r0, uint32_t c0, uint32_t r1, uint32_t c1) const {
    float l = std::min(g_.Left(c0), g_.Left(c1)), rgt = std::max(g_.Right(c0), g_.Right(c1));
    if (g_.rtl) l += g_.line;
    else rgt -= g_.line;
    return {l, g_.Top(r0), rgt, g_.Bottom(r1) - g_.line};
  }
  draw::Rect Outer(uint32_t r0, uint32_t c0, uint32_t r1, uint32_t c1) const {
    return {std::min(g_.Left(c0), g_.Left(c1)), g_.Top(r0), std::max(g_.Right(c0), g_.Right(c1)), g_.Bottom(r1)};
  }
  uint32_t Next(uint32_t c, int visualDir) const {  // the column visually to the right (+1) or left (-1)
    const int step = g_.rtl ? -visualDir : visualDir;
    if (step < 0) return c == 0 ? kNone : c - 1;
    return c + 1 >= kMaxCols ? kNone : c + 1;
  }
  void Rect(float l, float t, float r, float b, const draw::Color& c) {
    if (r > l && b > t) p_.FillRect({l, t, r, b}, draw::Brush::Solid(c));
  }

  // ---- Text: where each cell's text goes, first, because it decides the gridlines it covers.
  struct Item {
    const CellText::Layout* layout;
    draw::Rect box;   // the cell (or merged area) the text is placed in
    draw::Rect clip;
    float x = 0;      // the text's left edge, painter coordinates (wrapped: the wrap box's)
    const Xf* xf;
  };

  void PlaceText() {
    for (RowData& rd : rows_) {
      for (const CellInfo& cell : rd.cells) {
        if (cell.value.type == ValueType::Empty) continue;
        if (!o_.showZeros && cell.value.type == ValueType::Number && cell.value.number == 0) continue;
        if (MergeAt(cell.row, cell.col)) continue;  // drawn with its merged area
        if (grid_.ColHidden(cell.col)) continue;
        PlaceCell(rd, cell);
      }
    }
    // Merged areas: the first cell's text in the whole area.
    for (const Range* m : merges_) {
      CellInfo cell;
      if (!sheet_.GetCell(m->firstRow, m->firstCol, cell) || cell.value.type == ValueType::Empty) continue;
      int widthPx = 0;
      for (uint32_t c = m->firstCol; c <= m->lastCol; ++c) widthPx += grid_.ColWidth(c);
      if (widthPx == 0) continue;
      const CellText::Layout& l = text_.Get(sheet_, cell, widthPx);
      const Xf& xf = styles_.CellXf(cell.style);
      Item it{&l, Inner(m->firstRow, m->firstCol, m->lastRow, m->lastCol), {}, 0, &xf};
      it.clip = it.box;
      it.x = AlignX(l, xf, it.box);
      items_.push_back(it);
    }
  }

  float Indent(const Xf& xf) const {
    if (!xf.alignment.indent) return 0;
    const Font& base = styles_.DefaultFont();
    return xf.alignment.indent * 3 * text_.Measure().Of(styles_.FontOf(xf), base).space * g_.px;
  }

  // The text's left edge in `box` for its alignment (for one line; wrapped lines align inside).
  float AlignX(const CellText::Layout& l, const Xf& xf, const draw::Rect& box) const {
    const float pad = 2 * g_.px, w = l.width * g_.pt, indent = Indent(xf);
    if (l.wrapped) return box.left + pad + (l.align == HAlign::Right ? 0 : indent);
    switch (l.align) {
      case HAlign::Right: return box.right - pad - indent - w;
      case HAlign::Center:
      case HAlign::CenterContinuous: return (box.left + box.right - w) / 2;
      default: return box.left + pad + indent;
    }
  }

  void PlaceCell(RowData& rd, const CellInfo& cell) {
    const int widthPx = grid_.ColWidth(cell.col);
    const CellText::Layout& l = text_.Get(sheet_, cell, widthPx);
    if (l.lines.empty()) return;
    const Xf& xf = styles_.CellXf(cell.style);
    const float pad = 2 * g_.px;
    Item it{&l, Inner(cell.row, cell.col, cell.row, cell.col), {}, 0, &xf};
    it.clip = it.box;
    uint32_t first = cell.col, last = cell.col;
    const float need = l.width * g_.pt + 2 * pad + Indent(xf);
    const bool runs = !l.wrapped && !l.stacked && !l.shown.number && xf.alignment.rotation == 0 &&
                      !xf.alignment.shrinkToFit && l.align != HAlign::Fill && l.align != HAlign::Justify &&
                      l.align != HAlign::Distributed;
    auto grow = [&](int dir, float want) {  // into empty neighbors, visually right (+1) or left (-1)
      float got = 0;
      uint32_t edge = dir > 0 ? (g_.rtl ? first : last) : (g_.rtl ? last : first);
      while (got < want) {
        const uint32_t n = Next(edge, dir);
        if (n == kNone || !Empty(rd, n)) break;
        edge = n;
        got += g_.Right(n) - g_.Left(n);
        first = std::min(first, n);
        last = std::max(last, n);
      }
    };
    const float have = it.box.right - it.box.left;
    if (l.align == HAlign::CenterContinuous) {
      // Across the empty cells after it that center across selection too.
      uint32_t c = cell.col;
      while (true) {
        const uint32_t n = Next(c, g_.rtl ? -1 : 1);
        if (n == kNone || !Empty(rd, n)) break;
        size_t ci = static_cast<size_t>(std::find(cols_.begin(), cols_.end(), n) - cols_.begin());
        const uint32_t style = ci < cols_.size() ? StyleAt(rd, ci) : 0;
        if (styles_.CellXf(style).alignment.horizontal != HAlign::CenterContinuous) break;
        c = n;
        last = std::max(last, n);
        first = std::min(first, n);
      }
    } else if (runs && need > have + g_.line) {
      if (l.align == HAlign::Left) grow(1, need - have);
      else if (l.align == HAlign::Right) grow(-1, need - have);
      else if (l.align == HAlign::Center) {
        const float half = (need - have) / 2;
        grow(1, half);
        grow(-1, half);
      }
    }
    if (first != cell.col || last != cell.col) {
      const draw::Rect span = Inner(cell.row, first, cell.row, last);
      it.clip = span;
      if (l.align == HAlign::CenterContinuous) it.box = span;
      rd.spans.push_back({first, last});
    }
    // Only what reaches the drawn columns.
    if (last < area_.firstCol || first > area_.lastCol) return;
    it.x = AlignX(l, xf, it.box);
    items_.push_back(it);
  }

  void DrawText() {
    for (const Item& it : items_) DrawItem(it);
  }

  void DrawItem(const Item& it) {
    const CellText::Layout& l = *it.layout;
    const Alignment& al = it.xf->alignment;
    const float s = g_.pt;
    // The text block, in points: the lines' pitch (Excel's row height for the font) and the
    // first line's ascent and descent as GDI measures them.
    const size_t n = l.lines.size();
    const float blockH = l.ascent + l.descent + (n > 1 ? (n - 1) * (l.wrapped || l.stacked ? l.pitch : l.pitch) : 0);
    float scale = 1;
    if (al.shrinkToFit && !l.wrapped) {
      const float avail = (it.box.right - it.box.left) - 4 * g_.px;
      if (l.width * s > avail && l.width > 0) scale = std::max(0.1f, avail / (l.width * s));
    }
    const float boxH = it.box.bottom - it.box.top;
    p_.Save();
    p_.ClipRect(it.clip);
    const bool rotated = al.rotation != 0 && al.rotation != 255;
    if (rotated) {
      const double deg = al.rotation <= 90 ? al.rotation : -(al.rotation - 90.0);
      const double rad = deg * 3.14159265358979 / 180;
      const float w = l.width * s, h = blockH * s;
      const float bw = static_cast<float>(w * std::fabs(std::cos(rad)) + h * std::fabs(std::sin(rad)));
      const float bh = static_cast<float>(w * std::fabs(std::sin(rad)) + h * std::fabs(std::cos(rad)));
      const float pad = 2 * g_.px;
      float cx;
      switch (l.align) {
        case HAlign::Right: cx = it.box.right - pad - bw / 2; break;
        case HAlign::Center:
        case HAlign::CenterContinuous: cx = (it.box.left + it.box.right) / 2; break;
        default: cx = it.box.left + pad + bw / 2; break;
      }
      float cy;
      switch (al.vertical) {
        case VAlign::Top: cy = it.box.top + g_.px + bh / 2; break;
        case VAlign::Center: cy = (it.box.top + it.box.bottom) / 2; break;
        default: cy = it.box.bottom - g_.px - bh / 2; break;
      }
      p_.Transform(draw::Matrix::Translate(-l.width / 2, -(blockH / 2 - l.ascent)) * draw::Matrix::Rotate(static_cast<float>(-deg)) *
                   draw::Matrix::Scale(s, s) * draw::Matrix::Translate(cx, cy));
      DrawLines(l, 0, nullptr);
      p_.Restore();
      return;
    }
    // Vertical placement of the block.
    const float h = blockH * s * scale;
    float top;
    switch (al.vertical) {
      case VAlign::Top:
      case VAlign::Justify:
      case VAlign::Distributed: top = it.box.top + g_.px; break;
      case VAlign::Center: top = it.box.top + (boxH - h) / 2; break;
      default: top = it.box.bottom - g_.px - h; break;
    }
    const float baseline = top + l.ascent * s * scale;
    float x = it.x;
    if (scale != 1 && !l.wrapped) {
      const float w = l.width * s * scale;
      switch (l.align) {
        case HAlign::Right: x = it.box.right - 2 * g_.px - w; break;
        case HAlign::Center:
        case HAlign::CenterContinuous: x = (it.box.left + it.box.right - w) / 2; break;
        default: break;
      }
    }
    if (l.stacked) x = (it.box.left + it.box.right) / 2 - (l.lines.empty() ? 0 : (l.lines[0].right * s) / 2);
    if (l.align == HAlign::Fill && !l.wrapped && l.width > 0) {
      // The text repeated across the cell.
      const float w = l.width * s;
      const int times = std::clamp(static_cast<int>((it.box.right - it.box.left - 4 * g_.px) / w), 1, 256);
      for (int i = 0; i < times; ++i) {
        p_.Save();
        p_.Transform(draw::Matrix::Scale(s, s) * draw::Matrix::Translate(it.box.left + 2 * g_.px + i * w, baseline));
        DrawLines(l, 0, nullptr);
        p_.Restore();
      }
      p_.Restore();
      return;
    }
    p_.Transform(draw::Matrix::Scale(s * scale, s * scale) * draw::Matrix::Translate(x, baseline));
    DrawLines(l, l.wrapped || l.stacked ? 0 : 1, nullptr);
    p_.Restore();
  }

  // The lines in text space: x in points from the text's left edge, y from the first baseline.
  void DrawLines(const CellText::Layout& l, int fromContent, void*) {
    float y = 0;
    for (const text::Line& line : l.lines) {
      const float dx = fromContent ? -line.contentLeft : 0;
      for (const text::GlyphPiece& piece : line.pieces) {
        if (piece.glyphs.empty()) continue;
        const CellText::Run* run = piece.run < l.runs.size() ? &l.runs[piece.run] : nullptr;
        if (run && run->hidden) continue;
        const draw::Brush brush = draw::Brush::Solid(ToColor(run ? run->color : 0xFF000000));
        const draw::GlyphRun gr = piece.Run();
        if (piece.scale != 1) {
          p_.Save();
          p_.Transform(draw::Matrix::Translate(-piece.OriginX(), 0) * draw::Matrix::Scale(piece.scale, 1) *
                       draw::Matrix::Translate(piece.OriginX() + dx, y - piece.rise));
          p_.DrawGlyphs({piece.OriginX(), 0}, gr, brush);
          p_.Restore();
        } else {
          p_.DrawGlyphs({piece.OriginX() + dx, y - piece.rise}, gr, brush);
        }
        if (run && run->underline) {
          const float uy = y + run->underlinePos;
          p_.FillRect({piece.x + dx, uy, piece.x + dx + piece.width, uy + run->underlineThick}, brush);
          if (run->doubleUnderline)
            p_.FillRect({piece.x + dx, uy + 2 * run->underlineThick, piece.x + dx + piece.width, uy + 3 * run->underlineThick},
                        brush);
        }
        if (run && run->strike) {
          const float sy = y - run->strikePos;
          p_.FillRect({piece.x + dx, sy, piece.x + dx + piece.width, sy + run->strikeThick}, brush);
        }
      }
      y += l.pitch;
    }
  }

  // ---- Gridlines: under fills, not inside merged areas or text that runs across them.
  void Gridlines() {
    const draw::Color c = ToColor(o_.gridColor);
    const float L = g_.line;
    // Horizontal: each row's bottom.
    for (const RowData& rd : rows_) {
      const float y = g_.Bottom(rd.row) - L;
      float runStart = 0, runEnd = 0;
      bool open = false;
      for (size_t i = 0; i < cols_.size(); ++i) {
        const uint32_t col = cols_[i];
        const Range* m = MergeAt(rd.row, col);
        const bool skip = m && rd.row < m->lastRow;
        const float l = g_.Left(col) - (g_.rtl ? 0 : L), r = g_.Right(col) + (g_.rtl ? L : 0);
        if (skip) {
          if (open) Rect(runStart, y, runEnd, y + L, c);
          open = false;
          continue;
        }
        if (open && (std::fabs(l - runEnd) < 0.5f || std::fabs(r - runStart) < 0.5f)) {
          runStart = std::min(runStart, l);
          runEnd = std::max(runEnd, r);
        } else {
          if (open) Rect(runStart, y, runEnd, y + L, c);
          runStart = l;
          runEnd = r;
          open = true;
        }
      }
      if (open) Rect(runStart, y, runEnd, y + L, c);
    }
    // Vertical: each column's trailing edge.
    for (size_t i = 0; i < cols_.size(); ++i) {
      const uint32_t col = cols_[i];
      const float x = g_.rtl ? g_.Left(col) : g_.Right(col) - L;
      float runTop = 0, runBottom = 0;
      bool open = false;
      for (const RowData& rd : rows_) {
        const Range* m = MergeAt(rd.row, col);
        const bool skip = (m && col < m->lastCol) || rd.SpanCrosses(col, col + 1);
        if (skip) {
          if (open) Rect(x, runTop, x + L, runBottom, c);
          open = false;
          continue;
        }
        const float t = g_.Top(rd.row) - L, b = g_.Bottom(rd.row);
        if (open && std::fabs(t + L - runBottom) < 0.5f) {
          runBottom = b;
        } else {
          if (open) Rect(x, runTop, x + L, runBottom, c);
          runTop = t;
          runBottom = b;
          open = true;
        }
      }
      if (open) Rect(x, runTop, x + L, runBottom, c);
    }
  }

  // ---- Fills
  void FillRect(const draw::Rect& r, const Fill& f) {
    if (f.gradient && !f.stops.empty()) {
      std::vector<draw::GradientStop> stops;
      for (const GradientStop& s : f.stops)
        stops.push_back({static_cast<float>(s.position), ToColor(styles_.Resolve(s.color, 0xFFFFFFFF))});
      if (f.path) {
        const float cx = r.left + (r.right - r.left) * static_cast<float>((f.left + f.right) / 2);
        const float cy = r.top + (r.bottom - r.top) * static_cast<float>((f.top + f.bottom) / 2);
        p_.FillRect(r, draw::Brush::Radial({cx, cy}, (r.right - r.left), (r.bottom - r.top), stops));
        return;
      }
      const double rad = f.degree * 3.14159265358979 / 180;
      const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
      const float hw = (r.right - r.left) / 2, hh = (r.bottom - r.top) / 2;
      const float dx = static_cast<float>(std::cos(rad)), dy = static_cast<float>(std::sin(rad));
      const float reach = std::fabs(hw * dx) + std::fabs(hh * dy);
      p_.FillRect(r, draw::Brush::Linear({cx - dx * reach, cy - dy * reach}, {cx + dx * reach, cy + dy * reach}, stops));
      return;
    }
    if (f.pattern == PatternType::None) return;
    const uint32_t fg = styles_.Resolve(f.fg, 0xFF000000);
    if (f.pattern == PatternType::Solid) {
      p_.FillRect(r, draw::Brush::Solid(ToColor(fg)));
      return;
    }
    const uint32_t bg = styles_.Resolve(f.bg, 0xFFFFFFFF);
    p_.FillRect(r, draw::Brush::Solid(Mix(bg, fg, PatternShare(f.pattern))));
  }

  void Fills() {
    for (const RowData& rd : rows_) {
      for (size_t i = 0; i < cols_.size(); ++i) {
        const uint32_t col = cols_[i];
        if (MergeAt(rd.row, col)) continue;
        const Fill& f = styles_.FillOf(styles_.CellXf(StyleAt(rd, i)));
        if (f.pattern == PatternType::None && !f.gradient) continue;
        FillRect(Outer(rd.row, col, rd.row, col), f);
      }
    }
    for (const Range* m : merges_) {
      const Fill& f = styles_.FillOf(styles_.CellXf(sheet_.StyleAt(m->firstRow, m->firstCol)));
      const draw::Rect r = Outer(m->firstRow, m->firstCol, m->lastRow, m->lastCol);
      if (f.pattern != PatternType::None || f.gradient) FillRect(r, f);
    }
  }

  // ---- Borders
  void AddSide(std::vector<Segment>& out, const BorderSide& side, bool vertical, float a0, float a1, float b) {
    if (side.style == BorderStyle::None) return;
    out.push_back({a0, a1, b, vertical, side.style, styles_.Resolve(side.color, 0xFF000000)});
  }

  void Borders() {
    std::vector<Segment> segs;
    std::vector<std::pair<draw::Rect, std::pair<const Border*, uint32_t>>> diagonals;
    const float L = g_.line;
    for (const RowData& rd : rows_) {
      for (size_t i = 0; i < cols_.size(); ++i) {
        const uint32_t col = cols_[i];
        const Border& b = styles_.BorderOf(styles_.CellXf(StyleAt(rd, i)));
        const Range* m = MergeAt(rd.row, col);
        const bool top = !m || rd.row == m->firstRow, bottom = !m || rd.row == m->lastRow;
        const bool first = !m || col == m->firstCol, last = !m || col == m->lastCol;
        const float l = g_.Left(col), r = g_.Right(col), t = g_.Top(rd.row), btm = g_.Bottom(rd.row);
        // The file's left is the start: the visual right in a right-to-left sheet.
        const BorderSide& visLeft = g_.rtl ? b.right : b.left;
        const BorderSide& visRight = g_.rtl ? b.left : b.right;
        const bool visLeftEdge = g_.rtl ? last : first, visRightEdge = g_.rtl ? first : last;
        if (top) AddSide(segs, b.top, false, l - L, r, t);
        if (bottom) AddSide(segs, b.bottom, false, l - L, r, btm);
        if (visLeftEdge) AddSide(segs, visLeft, true, t - L, btm, l);
        if (visRightEdge) AddSide(segs, visRight, true, t - L, btm, r);
        if ((b.diagonalUp || b.diagonalDown) && b.diagonal.style != BorderStyle::None && !m)
          diagonals.push_back({{l, t, r, btm}, {&b, styles_.Resolve(b.diagonal.color, 0xFF000000)}});
      }
    }
    std::stable_sort(segs.begin(), segs.end(),
                     [](const Segment& a, const Segment& b) { return KindOf(a.style).weight < KindOf(b.style).weight; });
    for (const Segment& s : segs) DrawSegment(s);
    for (const auto& [r, d] : diagonals) {
      const LineKind k = KindOf(d.first->diagonal.style);
      draw::Pen pen;
      pen.brush = draw::Brush::Solid(ToColor(d.second));
      pen.width = std::max(L, k.width * g_.px);
      if (d.first->diagonalDown) {
        draw::Path path;
        path.MoveTo({r.left, r.top});
        path.LineTo({r.right, r.bottom});
        p_.StrokePath(path, pen);
      }
      if (d.first->diagonalUp) {
        draw::Path path;
        path.MoveTo({r.left, r.bottom});
        path.LineTo({r.right, r.top});
        p_.StrokePath(path, pen);
      }
    }
  }

  void DrawSegment(const Segment& s) {
    const LineKind k = KindOf(s.style);
    if (k.width <= 0) return;
    const float L = g_.line;
    const draw::Color c = ToColor(s.color);
    // Across the line: centered on the gridline's pixel, which is the last of the cell before it.
    std::vector<std::pair<float, float>> bands;
    if (k.doubled) {
      bands = {{s.b - 2 * L, s.b - L}, {s.b, s.b + L}};
    } else {
      const float lo = s.b - L * static_cast<float>((k.width + 1) / 2);
      bands = {{lo, lo + L * k.width}};
    }
    auto put = [&](float a0, float a1) {
      for (const auto& [b0, b1] : bands) {
        if (s.vertical) Rect(b0, a0, b1, a1, c);
        else Rect(a0, b0, a1, b1, c);
      }
    };
    if (k.dashes.empty()) {
      put(s.a0, s.a1);
      return;
    }
    const float unit = std::max(L, g_.px);
    float a = s.a0;
    size_t i = 0;
    while (a < s.a1) {
      const float len = k.dashes[i % k.dashes.size()] * unit;
      if (i % 2 == 0) put(a, std::min(a + len, s.a1));
      a += len;
      ++i;
    }
  }

  draw::Painter& p_;
  const Sheet& sheet_;
  CellText& text_;
  const Grid& grid_;
  const Geometry& g_;
  Range area_;
  const PaintOptions& o_;
  const Styles& styles_;
  std::vector<RowData> rows_;
  std::vector<uint32_t> cols_;
  std::vector<uint32_t> colStyle_;
  std::vector<const Range*> merges_;
  std::vector<Item> items_;
  uint32_t reachFirst_ = 0, reachLast_ = 0;
};

}  // namespace

namespace {
OverlayFn& OverlaySlot() {
  static OverlayFn fn;
  return fn;
}
}  // namespace

void SetOverlay(OverlayFn fn) { OverlaySlot() = std::move(fn); }
const OverlayFn& Overlay() { return OverlaySlot(); }

void PaintCells(draw::Painter& p, const Sheet& sheet, CellText& text, const Grid& grid, const Geometry& g,
                const Range& area, const PaintOptions& options) {
  Painting(p, sheet, text, grid, g, area, options).Run();
}

}  // namespace fw::sheet::view
