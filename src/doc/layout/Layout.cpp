// Layout (Layout.h): the public face of the pagination engine (Engine.h), and what is done with
// pages once they exist: painting, the layout dump, hit-testing and carets.
#include "doc/layout/Layout.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "core/OpenTimings.h"
#include "doc/layout/Engine.h"
#include "draw/Painter.h"

namespace fw::doc::layout {

struct Layout::Impl {
  Impl(const Document& doc, text::Fonts& fonts, const Options& options, OpenTimings* timings)
      : engine(doc, fonts, options, timings) {}
  Engine engine;
};

Layout::Layout(const Document& doc, text::Fonts& fonts, Options options, OpenTimings* timings)
    : impl_(std::make_unique<Impl>(doc, fonts, options, timings)) {}
Layout::~Layout() = default;

void Layout::LayOutPages(size_t count) {
  Engine& e = impl_->engine;
  while (!e.Done() && e.Pages().size() < count) e.NextPage();
}

bool Layout::Continue(double milliseconds) {
  Engine& e = impl_->engine;
  auto start = std::chrono::steady_clock::now();
  do {
    if (!e.NextPage()) break;
  } while (!e.Done() && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() <
                            milliseconds);
  return !e.Done();
}

bool Layout::Done() const { return impl_->engine.Done(); }
size_t Layout::PageCount() const { return impl_->engine.Pages().size(); }
const Page& Layout::GetPage(size_t index) const { return impl_->engine.Pages()[index]; }
uint32_t Layout::Revision() const { return impl_->engine.Revision(); }
size_t Layout::MemoryBytes() const { return impl_->engine.MemoryBytes(); }

// ---- Selection and find

namespace {

// The model offset of a layout offset on a line, through the paragraph's own text mapping.
uint32_t ToModel(Engine& e, const LineBox& lb, uint32_t layoutOffset) {
  ParaText pt;
  e.BuildText(lb.para, PageFields(), pt);
  return pt.ModelOffset(layoutOffset);
}

}  // namespace

std::optional<TextPos> Layout::HitTest(size_t page, float x, float y) const {
  Engine& e = impl_->engine;
  if (page >= e.Pages().size()) return std::nullopt;
  const Page& p = e.Pages()[page];
  const LineBox* best = nullptr;
  float bestDistance = 1e30f;
  for (const LineBox& lb : p.lines) {
    if (lb.para == kNone) continue;
    float dy = y < lb.top ? lb.top - y : y >= lb.top + lb.height ? y - (lb.top + lb.height) : 0;
    float left = lb.x + lb.line.left, right = lb.x + lb.line.right;
    float dx = x < left ? left - x : x > right ? x - right : 0;
    float d = dy * 4 + dx;  // the line under the point first, then the nearest
    if (d < bestDistance) {
      bestDistance = d;
      best = &lb;
    }
  }
  if (!best) return std::nullopt;
  uint32_t offset = best->line.HitTest(x - best->x);
  return TextPos{best->para, ToModel(e, *best, offset)};
}

std::optional<Layout::Caret> Layout::CaretAt(TextPos pos) {
  Engine& e = impl_->engine;
  for (size_t i = 0;; ++i) {
    while (i >= e.Pages().size() && !e.Done()) e.NextPage();
    if (i >= e.Pages().size()) return std::nullopt;
    const Page& p = e.Pages()[i];
    for (size_t k = 0; k < p.lines.size(); ++k) {
      const LineBox& lb = p.lines[k];
      if (lb.para != pos.para) continue;
      ParaText pt;
      e.BuildText(lb.para, PageFields(), pt);
      uint32_t at = pt.LayoutOffset(pos.offset);
      bool lastOfPara = lb.line.ending == text::LineEnd::ParagraphEnd;
      if (at >= lb.line.start && (at < lb.line.end || (at == lb.line.end && lastOfPara)))
        return Caret{i, lb.x + lb.line.CaretX(at), lb.top, lb.top + lb.height};
    }
  }
}

std::vector<std::pair<size_t, Rect>> Layout::Ranges(TextPos from, TextPos to) {
  std::vector<std::pair<size_t, Rect>> out;
  if (to < from) std::swap(from, to);
  Engine& e = impl_->engine;
  bool started = false, finished = false;
  for (size_t i = 0; !finished; ++i) {
    while (i >= e.Pages().size() && !e.Done()) e.NextPage();
    if (i >= e.Pages().size()) break;
    const Page& p = e.Pages()[i];
    for (const LineBox& lb : p.lines) {
      if (lb.para == kNone || lb.para < from.para || lb.para > to.para) continue;
      ParaText pt;
      e.BuildText(lb.para, PageFields(), pt);
      uint32_t a = lb.para == from.para ? pt.LayoutOffset(from.offset) : 0;
      uint32_t b = lb.para == to.para ? pt.LayoutOffset(to.offset) : UINT32_MAX;
      a = std::max(a, lb.line.start);
      b = std::min(b, lb.line.end);
      if (a >= b) continue;
      started = true;
      for (const auto& [l, r] : lb.line.Ranges(a, b)) out.push_back({i, {lb.x + l, lb.top, lb.x + r, lb.top + lb.height}});
      if (lb.para == to.para && b < lb.line.end) finished = true;
    }
    if (started && e.Pages()[i].lines.empty()) break;
  }
  return out;
}

std::optional<size_t> Layout::PageOf(TextPos pos) {
  if (auto c = CaretAt(pos)) return c->page;
  return std::nullopt;
}

// ---- Painting

namespace {

draw::Color ColorOf(uint32_t rgb) {
  return {static_cast<float>((rgb >> 16) & 255) / 255.0f, static_cast<float>((rgb >> 8) & 255) / 255.0f,
          static_cast<float>(rgb & 255) / 255.0f, 1};
}

void Line(draw::Painter& painter, float x1, float y1, float x2, float y2, float width, uint32_t rgb,
          std::vector<float> dashes = {}) {
  draw::Path path;
  path.MoveTo({x1, y1});
  path.LineTo({x2, y2});
  draw::Pen pen;
  pen.brush = draw::Brush::Solid(ColorOf(rgb));
  pen.width = width;
  pen.dashes = std::move(dashes);
  painter.StrokePath(path, pen);
}

void StrokeEdge(draw::Painter& painter, const Edge& e) {
  const Border& b = e.border;
  float w = std::max(0.25f, Pt(b.width));
  uint32_t rgb = b.color.automatic ? 0 : b.color.rgb;
  std::vector<float> dashes;
  switch (b.style) {
    case BorderStyle::Dotted: dashes = {1, 1}; break;
    case BorderStyle::Dashed: case BorderStyle::DashSmallGap: dashes = {3, 2}; break;
    case BorderStyle::DotDash: dashes = {3, 1, 1, 1}; break;
    case BorderStyle::DotDotDash: dashes = {3, 1, 1, 1, 1, 1}; break;
    default: break;
  }
  if (b.style == BorderStyle::Double) {
    bool horizontal = std::abs(e.y2 - e.y1) < std::abs(e.x2 - e.x1);
    float ox = horizontal ? 0 : w, oy = horizontal ? w : 0;
    Line(painter, e.x1 - ox, e.y1 - oy, e.x2 - ox, e.y2 - oy, w, rgb);
    Line(painter, e.x1 + ox, e.y1 + oy, e.x2 + ox, e.y2 + oy, w, rgb);
    return;
  }
  Line(painter, e.x1, e.y1, e.x2, e.y2, w, rgb, std::move(dashes));
}

void Placeholder(draw::Painter& painter, const Rect& r) {
  draw::Pen pen;
  pen.brush = draw::Brush::Solid({0.75f, 0.75f, 0.75f, 1});
  pen.width = 0.75f;
  painter.StrokePath(draw::Path::FromRect({r.left, r.top, r.right, r.bottom}), pen);
}

}  // namespace

void Layout::Paint(draw::Painter& painter, size_t page, const DrawingPainter& drawing) const {
  const Engine& e = impl_->engine;
  if (page >= e.Pages().size()) return;
  const Page& p = e.Pages()[page];
  auto paintLine = [&](const LineBox& lb) {
    const bool clip = lb.clip.Width() > 0;
    if (clip) {
      painter.Save();
      painter.ClipRect({lb.clip.left, lb.clip.top, lb.clip.right, lb.clip.bottom});
    }
    for (const text::GlyphPiece& g : lb.line.pieces) {
      const size_t at = lb.paint + g.run;
      if (at >= p.paints.size()) continue;
      const RunPaint& rp = p.paints[at];
      if (rp.highlight && !g.leader)
        painter.FillRect({lb.x + g.x, lb.top, lb.x + g.x + g.width, lb.top + lb.height},
                         draw::Brush::Solid(ColorOf(rp.highlightColor)));
    }
    for (const text::GlyphPiece& g : lb.line.pieces) {
      const size_t at = lb.paint + g.run;
      const RunPaint* rp = at < p.paints.size() ? &p.paints[at] : nullptr;
      uint32_t color = rp ? rp->color : 0;
      if (!g.glyphs.empty())
        painter.DrawGlyphs({lb.x + g.OriginX(), lb.baseline - g.rise}, g.Run(), draw::Brush::Solid(ColorOf(color)));
      if (!rp || g.hyphen || g.leader) continue;
      float x1 = lb.x + g.x, x2 = x1 + g.width, base = lb.baseline - g.rise;
      if (rp->underline != Underline::None) {
        float t = std::max(0.5f, rp->underlineThickness), y = base + std::max(1.0f, rp->underlineOffset);
        bool dbl = rp->underline == Underline::Double || rp->underline == Underline::WavyDouble;
        float w = rp->underline == Underline::Thick ? t * 2 : t;
        std::vector<float> dashes;
        if (rp->underline == Underline::Dotted || rp->underline == Underline::DottedHeavy) dashes = {1, 1};
        else if (rp->underline == Underline::Dash || rp->underline == Underline::DashLong) dashes = {3, 2};
        Line(painter, x1, y, x2, y, w, rp->underlineColor, dashes);
        if (dbl) Line(painter, x1, y + t * 2, x2, y + t * 2, w, rp->underlineColor);
      }
      if (rp->strike || rp->doubleStrike) {
        float t = std::max(0.5f, rp->strikeThickness), y = base - std::max(1.0f, rp->strikeOffset);
        Line(painter, x1, y, x2, y, t, color);
        if (rp->doubleStrike) Line(painter, x1, y - t * 2, x2, y - t * 2, t, color);
      }
    }
    if (clip) painter.Restore();
  };
  // A text box's lines go on top of its own shape, which is filled.
  auto drawOne = [&](const DrawingBox& d) {
    if (drawing) drawing(painter, d);
    else Placeholder(painter, d.rect);
    for (const LineBox& lb : p.lines)
      if (lb.drawing == d.drawing) paintLine(lb);
  };
  for (const DrawingBox& d : p.drawings)
    if (d.behindText) drawOne(d);
  for (const Fill& f : p.fills) painter.FillRect({f.rect.left, f.rect.top, f.rect.right, f.rect.bottom}, draw::Brush::Solid(ColorOf(f.color)));
  for (const LineBox& lb : p.lines)
    if (lb.drawing == doc::kNone) paintLine(lb);
  for (const Edge& edge : p.edges) StrokeEdge(painter, edge);
  for (const DrawingBox& d : p.drawings)
    if (!d.behindText) drawOne(d);
}

// ---- The layout dump

LayoutDump Layout::Dump(std::string source) const {
  const Engine& e = impl_->engine;
  LayoutDump dump;
  dump.source = std::move(source);
  dump.producer = "Featherweight";
  for (const Page& p : e.Pages()) {
    LayoutPage page;
    page.width = p.width;
    page.height = p.height;
    for (const LineBox& lb : p.lines) {
      bool any = false;
      for (char16_t c : lb.text)
        if (c != u' ' && c != u'\t') any = true;
      if (!any) continue;
      LayoutLine line;
      line.story = lb.story;
      line.x = lb.x + lb.line.contentLeft;
      line.baseline = lb.baseline;
      line.width = lb.line.contentRight - lb.line.contentLeft;
      float size = 0;
      for (const text::GlyphPiece& g : lb.line.pieces)
        if (!g.leader && !g.hyphen) size = std::max(size, g.emSize);
      line.size = size;
      line.text.assign(lb.text.begin(), lb.text.end());
      page.lines.push_back(std::move(line));
    }
    dump.pages.push_back(std::move(page));
  }
  return dump;
}

}  // namespace fw::doc::layout
