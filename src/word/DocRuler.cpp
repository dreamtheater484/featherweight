// The rulers of a Word document (plan §6.2 "Page layout", milestone D3), shown on demand as Word
// shows them in print layout: along the top, the caret's page with its margins and columns, and the
// caret's paragraph with its indents and tab stops; down the left, the page's top and bottom margins.
// Each mark can be dragged, one undo step when it's let go: indents and tab stops through
// fw::doc::edit's formatting (Format.h), margins and columns through its page setup (Page.h).

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>

#include "core/Strings.h"
#include "doc/edit/Format.h"
#include "doc/edit/Page.h"
#include "ui/Theme.h"
#include "word/DocView.h"

using namespace gfx;
namespace layout = fw::doc::layout;
namespace edit = fw::doc::edit;
using fw::Twips;
using fw::doc::kNone;
using fw::doc::TabAlign;
using fw::doc::TabStop;

namespace {

constexpr float kBand = 24;  // the rulers' breadth, UI pixels

float Pt(Twips t) { return static_cast<float>(t.Value()) / 20.f; }
Twips ToTwips(float pt) { return Twips(static_cast<int32_t>(std::lround(pt * 20.f))); }
// A cell margin as the layout takes it (Table.cpp): twips, none, or the table's default.
float MarginPt(const fw::doc::TableWidth& w, float fallback) {
  using Type = fw::doc::TableWidth::Type;
  return w.type == Type::Dxa ? static_cast<float>(w.value) / 20.f : w.type == Type::Nil ? 0.f : fallback;
}
// Widths that must fit between margins: rounded down, so their sum doesn't grow.
Twips FloorTwips(float pt) { return Twips(static_cast<int32_t>(std::floor(pt * 20.f + 0.01f))); }

// Centimeters where Windows measures in metric, as Word's rulers do; inches elsewhere.
bool Metric() {
  static const bool metric = [] {
    wchar_t v[4] = {};
    return GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IMEASURE, v, 4) > 0 && v[0] == L'0';
  }();
  return metric;
}
float Unit() { return Metric() ? 72.f / 2.54f : 72.f; }  // a centimeter or an inch, in points
// What a drag snaps to (Alt drags freely): an eighth of a centimeter, a sixteenth of an inch.
float SnapStep() { return Unit() / (Metric() ? 8.f : 16.f); }

std::wstring Length(float pt) {
  wchar_t s[32];
  swprintf_s(s, Metric() ? L"%.2f cm" : L"%.2f″", pt / Unit());
  return s;
}

const wchar_t* TabName(TabAlign a) {
  switch (a) {
    case TabAlign::Center: return L"Center tab";
    case TabAlign::End: return L"Right tab";
    case TabAlign::Decimal: return L"Decimal tab";
    case TabAlign::Bar: return L"Bar tab";
    default: return L"Left tab";
  }
}

// A small triangle, its point at (x, tip), pointing down (its base above) or up, filled a row of
// pixels at a time so its edges stay crisp.
void Triangle(Canvas& c, float x, float tip, float h, bool down, D2D1_COLOR_F color) {
  const int rows = std::max(1, static_cast<int>(std::lround(h)));
  for (int i = 0; i < rows; ++i) {
    const float half = (static_cast<float>(i) + 1.f) * h / static_cast<float>(rows);
    const float y = down ? tip - static_cast<float>(i) - 1.f : tip + static_cast<float>(i);
    c.Fill(RectF(x - half, y, x + half, y + 1.f), color);
  }
}

// Word's tab stop marks, standing on `bottom`: an L for a left stop, mirrored for a right one, an
// upside-down T for a centered one (a dot beside it: decimal), a bar for a bar.
void TabMark(Canvas& c, TabAlign a, float x, float bottom, float ui, D2D1_COLOR_F color) {
  const float h = std::round(6 * ui), w = std::round(5 * ui), t = std::max(1.f, std::round(1.5f * ui));
  x = std::round(x);
  auto stem = [&] { c.Fill(RectF(x - t / 2, bottom - h, x + t / 2, bottom), color); };
  auto foot = [&](float from, float to) { c.Fill(RectF(from, bottom - t, to, bottom), color); };
  switch (a) {
    case TabAlign::Center: stem(); foot(x - w, x + w); break;
    case TabAlign::End: stem(); foot(x - w, x + t / 2); break;
    case TabAlign::Decimal:
      stem();
      foot(x - w, x + w);
      c.Fill(RectF(x + t * 1.5f, bottom - h / 2 - t, x + t * 2.5f, bottom - h / 2), color);
      break;
    case TabAlign::Bar: c.Fill(RectF(x - t / 2, bottom - h - 2 * ui, x + t / 2, bottom), color); break;
    default: stem(); foot(x - t / 2, x + w); break;
  }
}

}  // namespace

// What the rulers show now. Page points from `section` on.
struct DocView::RulerInfo {
  D2D1_RECT_F top{}, left{}, corner{};  // the bands, window pixels
  int page = -1;                         // the page they measure; -1: none
  float x0 = 0, y0 = 0, scale = 1;       // the page's corner on screen; pixels per point
  float width = 0, height = 0;           // the page
  uint32_t section = 0;
  float marginL = 0, marginR = 0, marginT = 0, marginB = 0;  // the text area's edges
  float gutterL = 0, gutterR = 0, gutterT = 0;  // the gutter's share of a margin
  bool swapped = false;                  // mirrored margins on an even page: the left is the end margin
  bool topFixed = false, bottomFixed = false;  // a negative w:top or w:bottom (not moved by headers)
  std::vector<std::pair<float, float>> columns;  // the section's columns on the page
  bool equalColumns = true, columnLine = false;
  Twips columnSpace{720};
  int column = -1;  // the caret's column; -1 in a cell, a text box, a header
  // The caret's table row: the grid lines its cells' edges are on, where (page points), and the
  // cell margins on either side.
  struct Edge {
    int32_t line = 0;
    float x = 0, marginBefore = 0, marginAfter = 0;
  };
  std::vector<Edge> cells;
  // The caret's paragraph: kNone when its marks aren't shown.
  uint32_t para = kNone;
  float frameL = 0, frameR = 0;            // its column or cell
  float indent = 0, first = 0, right = 0;  // its left and right indents, where its first line starts
  std::vector<TabStop> tabs;
  float defaultTab = 36;
  bool canEdit = false;   // margins and columns can be dragged
  bool paraEdit = false;  // and the paragraph's marks
  float X(float pt) const { return x0 + pt * scale; }
  float Y(float pt) const { return y0 + pt * scale; }
  float Origin() const { return column >= 0 ? columns[column].first : marginL; }  // the ruler's 0
  float StripTop() const { return top.top + std::round((top.bottom - top.top) * 0.17f); }
  float StripBottom() const { return top.bottom - std::round((top.bottom - top.top) * 0.17f); }
};

// ---------------------------------------------------------------------------
// Geometry

bool DocView::RulerOn() const { return host_.ViewRuler() && !presentation_; }

RECT DocView::RulerInner(const RECT& rc, float ui) const {
  RECT r = rc;
  if (!RulerOn()) return r;
  const LONG band = static_cast<LONG>(std::lround(kBand * ui));
  r.top = std::min(r.bottom, r.top + band);
  r.left = std::min(r.right, r.left + band);
  return r;
}

void DocView::SetViewport(const RECT& rc, float ui) {
  full_ = rc;
  PagedView::SetViewport(RulerInner(rc, ui), ui);
}

bool DocView::OnRuler(int x, int y) const {
  return RulerOn() && x >= full_.left && y >= full_.top && x < full_.right && y < full_.bottom &&
         (x < vp_.left || y < vp_.top);
}

DocView::RulerInfo DocView::MeasureRuler() {
  RulerInfo r;
  const float band = std::round(kBand * ui_);
  const float l = static_cast<float>(full_.left), t = static_cast<float>(full_.top);
  r.corner = RectF(l, t, l + band, t + band);
  r.top = RectF(l + band, t, static_cast<float>(full_.right), t + band);
  r.left = RectF(l, t + band, l + band, static_cast<float>(full_.bottom));
  if (!Ready() || placeholder_ || sizes_.empty()) return r;
  // The caret's page while it's in view; else the page in view.
  int first, last;
  VisiblePages(first, last);
  int page = currentPage_;
  // (Only once its page is laid out: after a change of the page, the rest is laid out in idle time
  // and the rulers don't force it in a paint.)
  std::optional<layout::Layout::Caret> caret;
  if (Editing() && pagination_->LaidOut(focus_)) caret = CaretBox();
  if (caret && static_cast<int>(caret->page) >= first && static_cast<int>(caret->page) <= last)
    page = static_cast<int>(caret->page);
  else
    caret.reset();
  if (page < 0 || page >= static_cast<int>(pagination_->PageCount()) || page >= static_cast<int>(sizes_.size())) return r;
  const layout::Page& pg = pagination_->GetPage(page);
  const RECT pr = PageRect(page);
  r.page = page;
  r.scale = static_cast<float>((pr.right - pr.left) / std::max(1.0, sizes_[page].w));
  r.x0 = static_cast<float>(pr.left);
  r.y0 = static_cast<float>(pr.top);
  r.width = pg.width;
  r.height = pg.height;

  // The margins as the layout places them: swapped on even pages when mirrored, the gutter added.
  static const fw::doc::SectionProps kNoSection;
  const auto& sections = doc_->Sections();
  r.section = pg.section;
  const fw::doc::SectionProps& sp = pg.section < sections.size() ? sections[pg.section].props : kNoSection;
  const fw::doc::DocSettings& set = doc_->Settings();
  r.swapped = set.mirrorMargins && pg.number % 2 == 0;
  const float gutter = Pt(sp.gutter);
  if (set.gutterAtTop) r.gutterT = gutter;
  else if (r.swapped) r.gutterR = gutter;
  else r.gutterL = gutter;
  r.marginL = pg.body.left;
  r.marginR = pg.body.right;
  r.topFixed = sp.marginTop.Value() < 0;
  r.bottomFixed = sp.marginBottom.Value() < 0;
  r.marginT = std::abs(Pt(sp.marginTop)) + r.gutterT;
  r.marginB = pg.height - std::abs(Pt(sp.marginBottom));
  // The columns, as the layout makes them.
  const fw::doc::Columns& cols = sp.columns;
  r.equalColumns = cols.count <= 1 || cols.equalWidth || cols.cols.size() < static_cast<size_t>(cols.count);
  r.columnLine = cols.separator;
  r.columnSpace = cols.space;
  if (cols.count <= 1 || r.marginR <= r.marginL) {
    r.columns = {{r.marginL, r.marginR}};
  } else if (!r.equalColumns) {
    float x = r.marginL;
    for (const fw::doc::Columns::Col& k : cols.cols) {
      r.columns.push_back({x, x + Pt(k.width)});
      x += Pt(k.width) + Pt(k.space);
    }
  } else {
    const float space = Pt(cols.space), n = static_cast<float>(cols.count);
    const float w = (r.marginR - r.marginL - space * (n - 1)) / n;
    for (int32_t i = 0; i < cols.count; ++i) {
      const float x = r.marginL + static_cast<float>(i) * (w + space);
      r.columns.push_back({x, x + w});
    }
  }
  r.canEdit = Editing() && editor_->CanEdit();
  r.defaultTab = std::max(9.f, Pt(set.defaultTabStop));
  if (!caret) return r;

  // The caret's paragraph: its line here says which column or cell it's in.
  const uint32_t para = focus_.para;
  if (para >= doc_->Paragraphs().size()) return r;
  const layout::LineBox* line = nullptr;
  for (const layout::LineBox& lb : pg.lines) {
    if (lb.para != para || lb.turn != 0) continue;
    if (!line) line = &lb;
    if (lb.top <= caret->top + 0.5f && caret->top < lb.top + lb.height) {
      line = &lb;
      break;
    }
  }
  if (!line) return r;
  const fw::doc::ParaProps& pp = doc_->ResolvedPara(para);
  if (pp.bidi) return r;  // right-to-left paragraphs' marks aren't shown yet
  if (line->story == layout::LineStory::Cell || line->story == layout::LineStory::TextBox) {
    r.frameL = line->x;
    r.frameR = line->x + line->line.right + Pt(pp.indEnd);
    if (line->story == layout::LineStory::Cell) MeasureCells(r, pg, para);
  } else {
    r.frameL = r.marginL;
    r.frameR = r.marginR;
    for (size_t i = 0; line->story == layout::LineStory::Body && i < r.columns.size(); ++i) {
      if (std::abs(r.columns[i].first - line->x) < 1.f) {
        r.column = static_cast<int>(i);
        r.frameL = r.columns[i].first;
        r.frameR = r.columns[i].second;
      }
    }
  }
  r.para = para;
  r.indent = r.frameL + Pt(pp.indStart);
  r.first = r.indent + Pt(pp.firstLine);
  r.right = r.frameR - Pt(pp.indEnd);
  for (const TabStop& s : pp.tabs)
    if (s.align != TabAlign::Clear && s.align != TabAlign::Num) r.tabs.push_back(s);
  const fw::doc::Paragraph& p = doc_->Para(para);
  r.paraEdit = r.canEdit && doc_->Stories()[p.story].drawing == kNone &&
               !(doc_->GetTree().elems[p.elem].flags & fw::doc::kDisplayOnly);
  return r;
}

void DocView::MeasureCells(RulerInfo& r, const layout::Page& pg, uint32_t para) {
  // Each cell of the caret's row with text on this page: its edges are its text's, past its margins.
  const fw::doc::Pos at{para, focus_.offset};
  const auto cell = editor_ ? editor_->CellAt(at) : std::nullopt;
  if (!cell) return;
  const fw::doc::TableProps table = doc_->ResolvedTable(cell->tbl);
  const float defLeft = MarginPt(table.cellMargins.start, 5.4f), defRight = MarginPt(table.cellMargins.end, 5.4f);
  auto edge = [&](int32_t line) -> RulerInfo::Edge& {  // a grid line's edge, made when first seen
    for (RulerInfo::Edge& e : r.cells)
      if (e.line == line) return e;
    r.cells.push_back({line});
    return r.cells.back();
  };
  for (const auto& c : editor_->TableCells(at)) {
    if (c.row > cell->row || cell->row >= c.row + c.rows) continue;
    const layout::LineBox* text = nullptr;
    for (const layout::LineBox& lb : pg.lines)
      if (lb.para == c.start.para && lb.turn == 0 && lb.story == layout::LineStory::Cell) {
        text = &lb;
        break;
      }
    if (!text) continue;
    const fw::doc::CellProps props = doc_->ResolvedCell(c.tc);
    const float left = MarginPt(props.margins.start, defLeft), right = MarginPt(props.margins.end, defRight);
    RulerInfo::Edge& before = edge(c.col);
    before.x = text->x - left;
    before.marginAfter = left;
    RulerInfo::Edge& after = edge(c.col + c.span);  // (before may have moved)
    after.x = text->x + text->line.right + Pt(doc_->ResolvedPara(c.start.para).indEnd) + right;
    after.marginBefore = right;
  }
  std::sort(r.cells.begin(), r.cells.end(), [](const auto& a, const auto& b) { return a.line < b.line; });
}

DocView::RulerHit DocView::RulerAt(const RulerInfo& r, int xi, int yi) const {
  const float x = static_cast<float>(xi) + 0.5f, y = static_cast<float>(yi) + 0.5f;
  if (Contains(r.corner, x, y)) return {RulerPart::Corner};
  if (r.page < 0) return {};
  const float reach = 4 * ui_, edge = 3 * ui_;
  if (Contains(r.top, x, y)) {
    const float s0 = r.StripTop(), s1 = r.StripBottom(), mid = (s0 + s1) / 2, tri = std::round(5 * ui_) + 1;
    // The indents' marks are triangles at the strip's edges; between them, the margin can be taken.
    if (r.para != kNone && r.paraEdit) {
      if (y < s0 + tri && std::abs(x - r.X(r.first)) <= reach) return {RulerPart::FirstLine};
      if (y >= s1 && std::abs(x - r.X(r.indent)) <= reach) return {RulerPart::LeftIndent};
      if (y >= s1 - tri && std::abs(x - r.X(r.indent)) <= reach) return {RulerPart::Hanging};
      if (y >= s1 - tri && std::abs(x - r.X(r.right)) <= reach) return {RulerPart::RightIndent};
      for (size_t i = 0; i < r.tabs.size(); ++i)
        if (y >= mid && std::abs(x - r.X(r.frameL + Pt(r.tabs[i].pos))) <= reach) return {RulerPart::Tab, static_cast<int>(i)};
    }
    if (r.canEdit) {
      for (size_t i = 0; i < r.cells.size(); ++i)
        if (std::abs(x - r.X(r.cells[i].x)) <= reach) return {RulerPart::CellEdge, static_cast<int>(i)};
      for (size_t i = 1; i < r.columns.size(); ++i) {
        if (std::abs(x - r.X(r.columns[i - 1].second)) <= edge) return {RulerPart::ColumnEnd, static_cast<int>(i) - 1};
        if (std::abs(x - r.X(r.columns[i].first)) <= edge) return {RulerPart::ColumnStart, static_cast<int>(i)};
      }
      if (std::abs(x - r.X(r.marginL)) <= edge) return {RulerPart::MarginLeft};
      if (std::abs(x - r.X(r.marginR)) <= edge) return {RulerPart::MarginRight};
    }
    if (r.para != kNone && r.paraEdit && y >= s0 && y < s1 && x > r.X(r.frameL) && x < r.X(r.frameR)) return {RulerPart::Strip};
    return {};
  }
  if (Contains(r.left, x, y) && r.canEdit) {
    if (std::abs(y - r.Y(r.marginT)) <= edge) return {RulerPart::MarginTop};
    if (std::abs(y - r.Y(r.marginB)) <= edge) return {RulerPart::MarginBottom};
  }
  return {};
}

// ---------------------------------------------------------------------------
// Painting

void DocView::DrawRulers(Canvas& c) {
  const Palette& p = theme::P();
  RulerInfo r = MeasureRuler();
  const bool dragging = drag_ == Drag::Ruler;
  if (dragging && r.page >= 0) {  // what the drag is moving, where it is now
    const RulerDrag& d = rulerDrag_;
    const float at = d.at;
    switch (d.hit.part) {
      case RulerPart::FirstLine: r.first = at; break;
      case RulerPart::Hanging: r.indent = at; break;
      case RulerPart::LeftIndent:
        r.first += at - r.indent;
        r.indent = at;
        break;
      case RulerPart::RightIndent: r.right = at; break;
      case RulerPart::Tab:
        if (d.hit.index >= 0 && d.hit.index < static_cast<int>(r.tabs.size())) {
          if (d.off) r.tabs.erase(r.tabs.begin() + d.hit.index);
          else r.tabs[d.hit.index].pos = ToTwips(at - r.frameL);
        }
        break;
      case RulerPart::MarginLeft: r.marginL = r.columns.front().first = at; break;
      case RulerPart::MarginRight: r.marginR = r.columns.back().second = at; break;
      case RulerPart::MarginTop: r.marginT = at; break;
      case RulerPart::MarginBottom: r.marginB = at; break;
      case RulerPart::ColumnEnd: r.columns[d.hit.index].second = at; break;
      case RulerPart::ColumnStart: r.columns[d.hit.index].first = at; break;
      case RulerPart::CellEdge: r.cells[d.hit.index].x = at; break;
      default: break;
    }
  }
  const float hair = std::max(1.f, std::floor(ui_));
  c.Fill(r.corner, p.chrome);
  c.Fill(r.top, p.chrome);
  c.Fill(r.left, p.chrome);
  c.Fill(RectF(r.corner.left, r.top.bottom - hair, r.top.right, r.top.bottom), p.chromeBorder);
  c.Fill(RectF(r.left.right - hair, r.left.top, r.left.right, r.left.bottom), p.chromeBorder);
  // The corner: the kind of tab stop a click sets.
  {
    const float cx = (r.corner.left + r.corner.right) / 2, cy = (r.corner.top + r.corner.bottom) / 2, h = 7 * ui_;
    const D2D1_RECT_F box = RectF(cx - h, cy - h, cx + h, cy + h);
    if (rulerHover_.part == RulerPart::Corner) c.FillRound(box, 3 * ui_, p.hover);
    c.StrokeRound(box, 3 * ui_, p.inputBorder, hair);
    TabMark(c, rulerTab_, cx - (rulerTab_ == TabAlign::Start ? 2 * ui_ : rulerTab_ == TabAlign::End ? -2 * ui_ : 0),
            cy + 3 * ui_, ui_, p.text);
  }
  if (r.page < 0) return;

  const D2D1_COLOR_F margin = Alpha(p.text, p.dark ? 0.16f : 0.10f), paper = p.inputBg;
  const D2D1_COLOR_F mark = p.textSecondary, active = p.accent;
  auto colorOf = [&](RulerPart part, int index = -1) {
    const RulerHit h{part, index};
    if (dragging ? rulerDrag_.hit == h : rulerHover_ == h) return active;
    return mark;
  };
  // Ticks and numbers from the ruler's 0, each unit; what fits at this zoom.
  const float unit = Unit(), perUnit = unit * r.scale;
  int sub = Metric() ? 4 : 8;
  while (sub > 1 && perUnit / static_cast<float>(sub) < 5 * ui_) sub /= 2;
  int every = 1;
  while (perUnit * static_cast<float>(every) < 18 * ui_ && every < 64) every *= 2;
  auto ticks = [&](float from, float to, float origin, auto&& draw) {
    const float step = unit / static_cast<float>(sub);
    const int k0 = static_cast<int>(std::ceil((from - origin) / step)), k1 = static_cast<int>(std::floor((to - origin) / step));
    for (int k = k0; k <= k1 && k - k0 < 4000; ++k) {
      if (k == 0) continue;
      const int n = std::abs(k) / sub;
      const bool whole = k % sub == 0, half = sub > 1 && (2 * k) % sub == 0;
      draw(origin + static_cast<float>(k) * step, whole && n % every == 0 ? n : -1, whole || half);
    }
  };
  const float fontPx = 9 * ui_;

  // Along the top.
  {
    c.PushClip(r.top);
    const float s0 = r.StripTop(), s1 = r.StripBottom(), mid = (s0 + s1) / 2;
    c.Fill(RectF(r.X(0), s0, r.X(r.width), s1), margin);
    for (const auto& [a, b] : r.columns) c.Fill(RectF(r.X(a), s0, r.X(b), s1), paper);
    if (!r.cells.empty()) {
      // A table row: its cells' edges, with their margins, as Word's "Move Table Column" marks.
      for (size_t i = 0; i < r.cells.size(); ++i) {
        const RulerInfo::Edge& e = r.cells[i];
        const float x = r.X(e.x), half = 3 * ui_;
        const D2D1_RECT_F box =
            RectF(std::min(r.X(e.x - e.marginBefore), x - half), s0, std::max(r.X(e.x + e.marginAfter), x + half), s1);
        c.Fill(box, margin);
        const D2D1_COLOR_F grip = colorOf(RulerPart::CellEdge, static_cast<int>(i));
        for (float y = s0 + 3 * ui_; y + hair <= s1 - 2 * ui_; y += 3 * ui_)
          c.Fill(RectF(x - 2 * ui_, y, x + 2 * ui_, y + hair), grip);
      }
    } else if (r.para != kNone && r.column < 0) {  // a text box: its edges
      for (float e : {r.frameL, r.frameR}) c.Fill(RectF(r.X(e) - 2 * ui_, s0, r.X(e) + 2 * ui_, s1), margin);
    }
    ticks(0, r.width, r.Origin(), [&](float at, int number, bool longer) {
      const float x = std::round(r.X(at));
      if (number >= 0) {
        c.Text(std::to_wstring(number), RectF(x - 12 * ui_, s0, x + 12 * ui_, s1), p.textSecondary, fontPx,
               DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
      } else {
        const float h = longer ? 3 * ui_ : 1.5f * ui_;
        c.Fill(RectF(x, mid - h, x + hair, mid + h), p.textSecondary);
      }
    });
    if (r.para != kNone) {
      // Word's default stops, after the last stop of the paragraph's own and its indent.
      float from = r.indent;
      if (!r.tabs.empty()) from = std::max(from, r.frameL + Pt(r.tabs.back().pos));
      const D2D1_COLOR_F faint = Alpha(p.textSecondary, 0.6f);
      int n = 0;
      for (float x = r.frameL + r.defaultTab * (std::floor((from - r.frameL) / r.defaultTab) + 1); x < r.right && n < 200;
           x += r.defaultTab, ++n)
        c.Fill(RectF(std::round(r.X(x)), s1, std::round(r.X(x)) + hair, s1 + 2 * ui_), faint);
      for (size_t i = 0; i < r.tabs.size(); ++i) {
        const D2D1_COLOR_F color = r.paraEdit ? colorOf(RulerPart::Tab, static_cast<int>(i)) : p.textDisabled;
        TabMark(c, r.tabs[i].align, r.X(r.frameL + Pt(r.tabs[i].pos)), s1, ui_, color);
      }
      if (dragging && rulerDrag_.hit.part == RulerPart::Strip && !rulerDrag_.off)
        TabMark(c, rulerTab_, r.X(rulerDrag_.at), s1, ui_, active);
      // The indents: first line from the top, hanging indent and the left indent's box at the bottom.
      const float tri = std::round(5 * ui_);
      const auto off = p.textDisabled;
      Triangle(c, r.X(r.first), s0 + tri, tri, true, r.paraEdit ? colorOf(RulerPart::FirstLine) : off);
      Triangle(c, r.X(r.indent), s1 - tri, tri, false, r.paraEdit ? colorOf(RulerPart::Hanging) : off);
      const float bx = std::round(r.X(r.indent));
      c.Fill(RectF(bx - tri, s1, bx + tri, s1 + std::round(3 * ui_)), r.paraEdit ? colorOf(RulerPart::LeftIndent) : off);
      Triangle(c, r.X(r.right), s1 - tri, tri, false, r.paraEdit ? colorOf(RulerPart::RightIndent) : off);
    }
    c.PopClip();
  }
  // Down the left.
  {
    c.PushClip(r.left);
    const float s0 = r.left.left + std::round((r.left.right - r.left.left) * 0.2f);
    const float s1 = r.left.right - std::round((r.left.right - r.left.left) * 0.2f), mid = (s0 + s1) / 2;
    c.Fill(RectF(s0, r.Y(0), s1, r.Y(r.height)), margin);
    c.Fill(RectF(s0, r.Y(r.marginT), s1, r.Y(r.marginB)), paper);
    ticks(0, r.height, r.marginT, [&](float at, int number, bool longer) {
      const float y = std::round(r.Y(at));
      if (number >= 0) {
        c.Text(std::to_wstring(number), RectF(s0 - 4 * ui_, y - 7 * ui_, s1 + 4 * ui_, y + 7 * ui_), p.textSecondary, fontPx,
               DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
      } else {
        const float h = longer ? 3 * ui_ : 1.5f * ui_;
        c.Fill(RectF(mid - h, y, mid + h, y + hair), p.textSecondary);
      }
    });
    c.PopClip();
  }

  // While dragging: a dotted line across the pages where the mark is, and how far it is from 0.
  if (dragging && !rulerDrag_.off) {
    const RulerPart part = rulerDrag_.hit.part;
    const bool vertical = part == RulerPart::MarginTop || part == RulerPart::MarginBottom;
    const float at = vertical ? r.Y(rulerDrag_.at) : r.X(rulerDrag_.at);
    const D2D1_COLOR_F dots = Alpha(p.textSecondary, 0.8f);
    c.PushClip(RectF(vp_));
    if (vertical) {
      for (float x = static_cast<float>(vp_.left); x < vp_.right; x += 4 * ui_)
        c.Fill(RectF(x, at, x + 2 * ui_, at + hair), dots);
    } else {
      for (float y = static_cast<float>(vp_.top); y < vp_.bottom; y += 4 * ui_)
        c.Fill(RectF(at, y, at + hair, y + 2 * ui_), dots);
    }
    const float zero = vertical ? (part == RulerPart::MarginTop ? 0.f : r.height) : r.Origin();
    const std::wstring text = Length(std::abs(rulerDrag_.at - zero));
    const float tw = c.MeasureText(text, 11 * ui_) + 12 * ui_, th = 20 * ui_;
    const float bx = vertical ? static_cast<float>(vp_.left) + 6 * ui_
                              : std::min(at + 6 * ui_, static_cast<float>(vp_.right) - tw - 4 * ui_);
    const float by = vertical ? at + 6 * ui_ : static_cast<float>(vp_.top) + 6 * ui_;
    const D2D1_RECT_F box = RectF(bx, by, bx + tw, by + th);
    c.FillRound(box, 4 * ui_, p.popupBg);
    c.StrokeRound(box, 4 * ui_, p.popupBorder, hair);
    c.Text(text, box, p.text, 11 * ui_, DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
    c.PopClip();
  }
}

// ---------------------------------------------------------------------------
// Mouse

void DocView::RulerMouseDown(int x, int y) {
  const RulerInfo r = MeasureRuler();
  const RulerHit hit = RulerAt(r, x, y);
  if (hit.part == RulerPart::None) return;
  if (hit.part == RulerPart::Corner) {
    static constexpr TabAlign kOrder[] = {TabAlign::Start, TabAlign::Center, TabAlign::End, TabAlign::Decimal, TabAlign::Bar};
    size_t i = 0;
    while (i < std::size(kOrder) && kOrder[i] != rulerTab_) ++i;
    rulerTab_ = kOrder[(i + 1) % std::size(kOrder)];
    host_.ViewInvalidate();
    return;
  }
  const bool vertical = hit.part == RulerPart::MarginTop || hit.part == RulerPart::MarginBottom;
  const float mouse = ((vertical ? static_cast<float>(y) - r.y0 : static_cast<float>(x) - r.x0)) / r.scale;
  float at = mouse;
  switch (hit.part) {
    case RulerPart::FirstLine: at = r.first; break;
    case RulerPart::Hanging:
    case RulerPart::LeftIndent: at = r.indent; break;
    case RulerPart::RightIndent: at = r.right; break;
    case RulerPart::Tab: at = r.frameL + Pt(r.tabs[hit.index].pos); break;
    case RulerPart::MarginLeft: at = r.marginL; break;
    case RulerPart::MarginRight: at = r.marginR; break;
    case RulerPart::MarginTop: at = r.marginT; break;
    case RulerPart::MarginBottom: at = r.marginB; break;
    case RulerPart::ColumnEnd: at = r.columns[hit.index].second; break;
    case RulerPart::ColumnStart: at = r.columns[hit.index].first; break;
    case RulerPart::CellEdge: at = r.cells[hit.index].x; break;
    default: break;  // a new tab stop: where the click is
  }
  rulerDrag_ = {hit, at, at, at, false};
  rulerDrag_.grab = mouse;
  drag_ = Drag::Ruler;
  if (hit.part == RulerPart::Strip) RulerMouseMove(x, y, 0);  // snapped
  host_.ViewInvalidate();
}

void DocView::RulerMouseMove(int x, int y, UINT mods) {
  const RulerInfo r = MeasureRuler();
  if (r.page < 0) return;
  RulerDrag& d = rulerDrag_;
  const RulerPart part = d.hit.part;
  const bool vertical = part == RulerPart::MarginTop || part == RulerPart::MarginBottom;
  const float mouse = (vertical ? static_cast<float>(y) - r.y0 : static_cast<float>(x) - r.x0) / r.scale;
  float at = d.start + (mouse - d.grab);
  const float origin = vertical ? r.marginT : r.Origin(), step = SnapStep();
  if (!(mods & kModAlt)) at = origin + std::round((at - origin) / step) * step;
  // Where each can go: the page's edges, and room for text between the marks.
  const float minText = 9, minColumn = 18;
  float lo = 0, hi = vertical ? r.height : r.width;
  switch (part) {
    case RulerPart::FirstLine:
    case RulerPart::Hanging: hi = r.right - minText; break;
    case RulerPart::LeftIndent:
      lo = std::max(0.f, r.indent - r.first);
      hi = r.right - minText - std::max(0.f, r.first - r.indent);
      break;
    case RulerPart::RightIndent: lo = std::max(r.indent, r.first) + minText; break;
    case RulerPart::Tab:
    case RulerPart::Strip:
      lo = r.frameL;
      hi = r.frameR;
      break;
    case RulerPart::MarginLeft:
      lo = r.gutterL;
      hi = r.columns.front().second - minColumn;
      break;
    case RulerPart::MarginRight:
      lo = r.columns.back().first + minColumn;
      hi = r.width - r.gutterR;
      break;
    case RulerPart::MarginTop:
      lo = r.gutterT;
      hi = r.marginB - minColumn;
      break;
    case RulerPart::MarginBottom: lo = r.marginT + minColumn; break;
    case RulerPart::CellEdge: {  // the cells on either side keep a tenth of an inch (Editor::MoveColumnBorder)
      const int i = d.hit.index;
      if (i > 0) lo = r.cells[i - 1].x + 7.2f;
      if (i + 1 < static_cast<int>(r.cells.size())) hi = r.cells[i + 1].x - 7.2f;
      break;
    }
    case RulerPart::ColumnEnd:
    case RulerPart::ColumnStart: {
      const auto& c = r.columns;
      const int i = d.hit.index;
      const float equal = (r.marginR - r.marginL) / static_cast<float>(c.size());  // equal columns at no spacing
      if (part == RulerPart::ColumnEnd) {
        lo = c[i].first + minColumn;
        hi = r.equalColumns ? c[i].first + equal : c[i + 1].first;
      } else {
        lo = r.equalColumns ? c[i].second - equal : c[i - 1].second;
        hi = c[i].second - minColumn;
      }
      break;
    }
    default: break;
  }
  d.at = std::clamp(at, lo, std::max(lo, hi));
  // A tab stop pulled down off the ruler goes, as in Word.
  if (part == RulerPart::Tab || part == RulerPart::Strip) d.off = static_cast<float>(y) > r.top.bottom + 8 * ui_;
  host_.ViewInvalidate();
}

void DocView::RulerMouseUp() {
  const RulerDrag d = rulerDrag_;
  rulerDrag_ = {};
  host_.ViewInvalidate();
  if (!Editing()) return;
  const RulerInfo r = MeasureRuler();
  if (r.page < 0) return;
  const bool moved = std::abs(d.at - d.start) > 0.01f;
  const edit::Range sel{SelFrom(), SelTo()};
  auto tabs = [&](std::vector<Twips> remove, std::vector<TabStop> add) {
    {
      edit::TreeEdit te(*editor_, SelFrom());
      edit::ChangeTabStops(te, sel, remove, add);
    }
    AfterFormat({});
  };
  auto paragraphs = [&](const edit::ParaFormat& f) {
    {
      edit::TreeEdit te(*editor_, SelFrom());
      edit::FormatParagraphs(te, sel, f);
    }
    AfterFormat({});
  };
  // A change of the page: the first pages now, the rest in idle time (plan §11).
  auto section = [&](const edit::Result& res) { ApplyPageChange(res, false); };
  auto table = [&](const edit::Result& res) {
    if (!res) host_.ViewMessage(BannerKind::Warning, std::wstring(res.message.begin(), res.message.end()));
    else AfterFormat({});
  };
  edit::ParaFormat pf;
  edit::PageSetup ps;
  switch (d.hit.part) {
    case RulerPart::Strip:
      if (!d.off && r.para != kNone) tabs({}, {TabStop{ToTwips(d.at - r.frameL), rulerTab_}});
      return;
    case RulerPart::Tab: {
      if (r.para == kNone || d.hit.index < 0 || d.hit.index >= static_cast<int>(r.tabs.size())) return;
      const TabStop was = r.tabs[d.hit.index];
      TabStop now = was;
      now.pos = ToTwips(d.at - r.frameL);
      if (d.off) tabs({was.pos}, {});
      else if (moved) tabs({was.pos}, {now});
      return;
    }
    default: break;
  }
  if (!moved) return;
  switch (d.hit.part) {
    case RulerPart::FirstLine:
      pf.firstLine = ToTwips(d.at - r.indent);
      return paragraphs(pf);
    case RulerPart::Hanging:  // the first line stays where it is
      pf.indStart = ToTwips(d.at - r.frameL);
      pf.firstLine = ToTwips(r.first - d.at);
      return paragraphs(pf);
    case RulerPart::LeftIndent:  // the first line moves with it
      pf.indStart = ToTwips(d.at - r.frameL);
      return paragraphs(pf);
    case RulerPart::RightIndent:
      pf.indEnd = ToTwips(r.frameR - d.at);
      return paragraphs(pf);
    case RulerPart::MarginLeft:
      (r.swapped ? ps.right : ps.left) = ToTwips(d.at - r.gutterL);
      return section(edit::SetPageSetup(*editor_, r.section, r.section, ps));
    case RulerPart::MarginRight:
      (r.swapped ? ps.left : ps.right) = ToTwips(r.width - d.at - r.gutterR);
      return section(edit::SetPageSetup(*editor_, r.section, r.section, ps));
    case RulerPart::MarginTop: {
      const Twips m = ToTwips(d.at - r.gutterT);
      ps.top = r.topFixed ? Twips(-m.Value()) : m;
      return section(edit::SetPageSetup(*editor_, r.section, r.section, ps));
    }
    case RulerPart::MarginBottom: {
      const Twips m = ToTwips(r.height - d.at);
      ps.bottom = r.bottomFixed ? Twips(-m.Value()) : m;
      return section(edit::SetPageSetup(*editor_, r.section, r.section, ps));
    }
    case RulerPart::CellEdge:
      if (d.hit.index < 0 || d.hit.index >= static_cast<int>(r.cells.size())) return;
      return table(editor_->MoveColumnBorder({focus_.para, focus_.offset}, r.cells[d.hit.index].line,
                                             ToTwips(d.at - d.start).Value()));
    case RulerPart::ColumnEnd:
    case RulerPart::ColumnStart: {
      const int i = d.hit.index;
      const size_t n = r.columns.size();
      edit::ColumnSetup cs;
      cs.count = static_cast<int32_t>(n);
      cs.line = r.columnLine;
      cs.space = r.columnSpace;
      if (r.equalColumns) {
        // Equal columns stay equal: each is as wide as the dragged one, the spacing takes the rest.
        const float w = d.hit.part == RulerPart::ColumnEnd ? d.at - r.columns[i].first : r.columns[i].second - d.at;
        cs.space = FloorTwips((r.marginR - r.marginL - static_cast<float>(n) * w) / static_cast<float>(n - 1));
      } else {
        std::vector<std::pair<float, float>> c = r.columns;
        (d.hit.part == RulerPart::ColumnEnd ? c[i].second : c[i].first) = d.at;
        for (size_t k = 0; k < n; ++k)
          cs.widths.push_back({FloorTwips(c[k].second - c[k].first),
                               k + 1 < n ? FloorTwips(c[k + 1].first - c[k].second) : Twips(0)});
      }
      return section(edit::SetColumns(*editor_, r.section, r.section, cs));
    }
    default: return;
  }
}

void DocView::RulerHover(int x, int y) {
  RulerHit hit;
  if (OnRuler(x, y)) hit = RulerAt(MeasureRuler(), x, y);
  if (hit == rulerHover_) return;
  rulerHover_ = hit;
  host_.ViewInvalidate();
}

std::wstring DocView::RulerTooltip(int x, int y) {
  const RulerInfo r = MeasureRuler();
  const RulerHit hit = RulerAt(r, x, y);
  const float o = r.Origin();
  switch (hit.part) {
    case RulerPart::Corner:
      return std::wstring(TabName(rulerTab_)) + L": a click on the ruler sets one. Click here for another kind.";
    case RulerPart::Strip: return std::wstring(L"Click to set a ") + fw::ToLower(std::wstring(TabName(rulerTab_)));
    case RulerPart::FirstLine: return L"First line indent: " + Length(r.first - o);
    case RulerPart::Hanging: return L"Hanging indent: " + Length(r.indent - o);
    case RulerPart::LeftIndent: return L"Left indent: " + Length(r.indent - o);
    case RulerPart::RightIndent: return L"Right indent: " + Length(r.right - o);
    case RulerPart::Tab:
      return std::wstring(TabName(r.tabs[hit.index].align)) + L": " + Length(r.frameL + Pt(r.tabs[hit.index].pos) - o) +
             L". Drag it off the ruler to remove it.";
    case RulerPart::MarginLeft: return L"Left margin: " + Length(r.marginL);
    case RulerPart::MarginRight: return L"Right margin: " + Length(r.width - r.marginR);
    case RulerPart::MarginTop: return L"Top margin: " + Length(r.marginT);
    case RulerPart::MarginBottom: return L"Bottom margin: " + Length(r.height - r.marginB);
    case RulerPart::ColumnEnd:
    case RulerPart::ColumnStart: return L"Column edge: drag to change the columns' widths and spacing";
    case RulerPart::CellEdge: return L"Move table column";
    default: return {};
  }
}

HCURSOR DocView::RulerCursor(int x, int y) {
  static HCURSOR arrow = LoadCursor(nullptr, IDC_ARROW), we = LoadCursor(nullptr, IDC_SIZEWE),
                 ns = LoadCursor(nullptr, IDC_SIZENS);
  const RulerPart part = drag_ == Drag::Ruler ? rulerDrag_.hit.part : RulerAt(MeasureRuler(), x, y).part;
  switch (part) {
    case RulerPart::MarginTop:
    case RulerPart::MarginBottom: return ns;
    case RulerPart::MarginLeft:
    case RulerPart::MarginRight:
    case RulerPart::ColumnEnd:
    case RulerPart::ColumnStart:
    case RulerPart::CellEdge: return we;
    default: return arrow;
  }
}
