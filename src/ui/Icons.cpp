// The ribbon's own icons (R1): those the icon fonts lack, each a few drawing steps on a 16 × 16
// grid, drawn at any size in the text color with the accent for what stands out. Featherweight's
// own drawings: nothing here is copied from Office.
//
// Steps: l x1 y1 x2 y2 a line; p x y x y… a polyline; P x y… a filled polygon; r x y w h a frame;
// f x y w h a filled box; b x y w h a box filled with the background and framed (in front of
// another); e cx cy rx ry an ellipse; o … a filled one; c … one filled with the background and
// framed; g cx cy r a0 a1 a filled sector (degrees clockwise from twelve o'clock); D x1 y1 x2 y2 a
// dotted line; t x y w h size 'text'; a the accent from here, n the text color again; w k lines k
// times as thick.

#include <cmath>
#include <cstdlib>
#include <vector>

#include "core/Strings.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

using namespace gfx;

namespace icon {

namespace {

struct Path {
  wchar_t glyph;
  const char* steps;  // UTF-8
};

constexpr Path kPaths[] = {
    // Text
    {Subscript, "t0 1 10 12 12'x' a t9 7 7 9 7'2'"},
    {Superscript, "t0 3 10 12 12'x' a t9 0 7 9 7'2'"},
    {Strikethrough, "t0 1 16 13 10'ab' l1 8.5 15 8.5"},
    {ClearFormatting, "t0 0 11 13 11'A' a l9 9 15 15 l15 9 9 15"},
    {MultilevelList, "o2 3 1 1 l4.5 3 15 3 o5 8 1 1 l7.5 8 15 8 o8 13 1 1 l10.5 13 15 13"},
    {LineSpacing, "l7 3 15 3 l7 8 15 8 l7 13 15 13 a l3 1.5 3 14.5 p1 3.5 3 1.5 5 3.5 p1 12.5 3 14.5 5 12.5"},
    {Hyphenation, "t0 1 16 13 9'bc-'"},
    {IndentLeft, "l1 2 15 2 l7 6 15 6 l7 10 15 10 l1 14 15 14 a P1 5 4.5 8 1 11"},
    {IndentRight, "l1 2 15 2 l1 6 9 6 l1 10 9 10 l1 14 15 14 a P15 5 11.5 8 15 11"},
    {SpaceBefore, "a l3 1 3 6 p1 4 3 6 5 4 n l1 9 15 9 l1 12 15 12 l1 15 11 15"},
    {SpaceAfter, "l1 1 15 1 l1 4 15 4 l1 7 11 7 a l3 10 3 15 p1 13 3 15 5 13"},
    {WordCount, "t0 0 16 8 6.5'ABC' t0 8 16 8 6.5'123'"},
    {Footnote, "t0 2 12 13 9'AB' a t11 0 5 8 6'1'"},
    {Endnote, "r3 1.5 10 13 l5 4.5 11 4.5 l5 7 11 7 l8 11.5 11 11.5 a t4.5 8.5 4 6 5'1'"},
    {Replace, "t0 0 9 8 6.5'ab' a t7 8 9 8 6.5'ac' n p3 9 3 12 6 12 p5 10.5 6.5 12 5 13.5"},
    {Spelling, "t0 0 16 9 6.5'ABC' a p3 11.5 6 14.5 13 8.5"},
    {Zoom100, "t0 0 16 16 7.5'100'"},
    {TextBox, "r1.5 1.5 13 13 a t3 2 10 12 9.5'A'"},
    // Cells and numbers
    {FillBucket, "p2.5 7.5 8 2 13.5 7.5 8 13 2.5 7.5 l2.5 7.5 13.5 7.5 p8 2 6.5 0.5 a o13.8 12.2 1.4 1.6 P13.8 9 15.2 11.6 12.4 11.6"},
    {BordersAll, "r1.5 1.5 13 13 l8 1.5 8 14.5 l1.5 8 14.5 8"},
    {BordersBottom, "D1.5 1.5 14.5 1.5 D1.5 1.5 1.5 14.5 D14.5 1.5 14.5 14.5 D8 1.5 8 14.5 D1.5 8 14.5 8 w2 l1 14.5 15 14.5"},
    {AlignTop, "a l1 1.5 15 1.5 n l4 4.5 12 4.5 l4 7.5 12 7.5"},
    {AlignMiddle, "a D1 1.5 15 1.5 D1 14.5 15 14.5 n l4 6.5 12 6.5 l4 9.5 12 9.5"},
    {AlignBottom, "l4 8.5 12 8.5 l4 11.5 12 11.5 a l1 14.5 15 14.5"},
    {WrapCell, "l1 3 15 3 l1 12.5 4 12.5 a p1 8 13 8 13 12.5 7 12.5 p9 10.5 7 12.5 9 14.5"},
    {MergeCenter, "r1.5 3.5 13 9 t5 4 6 8 6.5'a' a p2.5 8 5 8 p3.8 6.7 2.5 8 3.8 9.3 p13.5 8 11 8 p12.2 6.7 13.5 8 12.2 9.3"},
    {Accounting, "t0 0 16 16 12'$'"},
    {Percent, "t0 0 16 16 12'%'"},
    {Comma, "t0 0 16 16 8'000' a t0 2 7 16 12','"},
    {IncreaseDecimal, "a t0 0 16 8 6.5'←.0' n t0 8 16 8 6.5'.00'"},
    {DecreaseDecimal, "t0 0 16 8 6.5'.00' a t0 8 16 8 6.5'→.0'"},
    {AutoSum, "t0 0 16 16 13'Σ'"},
    {InsertCells, "r1.5 1.5 9 9 l1.5 6 10.5 6 l6 1.5 6 10.5 a w2 l12.5 9 12.5 16 l9 12.5 16 12.5"},
    {ConditionalFormat, "r1.5 1.5 13 13 l1.5 6 14.5 6 l1.5 10 14.5 10 l6 1.5 6 14.5 l10 1.5 10 14.5 a f6.5 6.5 3 3 f10.5 2 3.5 3.5 f2 10.5 3.5 3.5"},
    {CellStyles, "a f2 2 12 4 n r1.5 1.5 13 13 l1.5 10 14.5 10 l8 6 8 14.5"},
    {PivotTable, "a f2 2 12 2.5 f2 2 2.5 12 n r1.5 1.5 13 13 l4.5 1.5 4.5 14.5 l1.5 4.5 14.5 4.5 a p8 12 12 12 12 8 p10.5 9.5 12 8 13.5 9.5"},
    // Pages and slides
    {PageBreak, "p3 6 3 1.5 13 1.5 13 6 p3 10 3 14.5 13 14.5 13 10 a D1 8 15 8"},
    {Header, "a f4 2.5 8 3 n r3 1.5 10 13 l5 9 11 9 l5 11.5 11 11.5"},
    {Footer, "a f4 10.5 8 3 n r3 1.5 10 13 l5 4.5 11 4.5 l5 7 11 7"},
    {PageNumber, "r3 1.5 10 13 a t3 3 10 10 8'#'"},
    {Watermark, "r3 1.5 10 13 a t3 3 10 10 9'A'"},
    {Margins, "r2.5 1.5 11 13 a D4.5 3.5 11.5 3.5 D4.5 12.5 11.5 12.5 D4.5 3.5 4.5 12.5 D11.5 3.5 11.5 12.5"},
    {PaperSize, "r2 2 9 12 a l8 8 15 15 p11 15 15 15 15 11"},
    {Columns, "r1.5 1.5 13 13 l3.5 4 7 4 l3.5 6.5 7 6.5 l3.5 9 7 9 l3.5 11.5 7 11.5 l9 4 12.5 4 l9 6.5 12.5 6.5 l9 9 12.5 9 l9 11.5 12.5 11.5"},
    {Orientation, "r1.5 1.5 7 10 a b6.5 8 8 6.5"},
    {PrintArea, "r1.5 1.5 13 13 a D4 4 12 4 D12 4 12 11 D4 11 12 11 D4 4 4 11"},
    {PrintTitles, "a f2 2 12 2.5 n r1.5 1.5 13 13 l1.5 7.5 14.5 7.5 l1.5 11 14.5 11 l6 1.5 6 14.5"},
    {TableOfContents, "r3 1.5 10 13 l5 4.5 11 4.5 a l7 7 11 7 l7 9.5 11 9.5 n l5 12 11 12"},
    {Bookmark, "p4 1.5 12 1.5 12 14.5 8 11 4 14.5 4 1.5"},
    {NewSlide, "r1 3 11 8 a w2 l13 8.5 13 15.5 l9.5 12 16.5 12"},
    {SlideSize, "r1 3 14 10 a l4 6 12 10 p4 8.5 4 6 6.5 6 p12 7.5 12 10 9.5 10"},
    {FormatBackground, "a f3 5 10 6 n r1 3 14 10"},
    {HideSlide, "r1.5 3.5 13 9 a l1 15 15 1"},
    {FromCurrentSlide, "r1 3 10 8 a P9.5 8.5 15 11.75 9.5 15"},
    {FitToWindow, "r3.5 4 9 8 a p1 4 1 1 4 1 p12 1 15 1 15 4 p15 12 15 15 12 15 p4 15 1 15 1 12"},
    // Objects
    {Shapes, "r1.5 1.5 8 8 a c10.5 10.5 4.5 4.5"},
    {WrapSquare, "a f5 5 6 6 n l1 2 15 2 l1 5 3.5 5 l12.5 5 15 5 l1 8 3.5 8 l12.5 8 15 8 l1 11 3.5 11 l12.5 11 15 11 l1 14 15 14"},
    {Arrange, "r1.5 1.5 8 8 a f6.5 6.5 8 8"},
    {BringForward, "r6.5 6.5 8 8 a f1.5 1.5 8 8"},
    {SendBackward, "a f6.5 6.5 8 8 n b1.5 1.5 8 8"},
    {AlignObjects, "l1.5 1 1.5 15 a f3 3 10 3.5 f3 9.5 6 3.5"},
    {Group, "D1 1 15 1 D15 1 15 15 D1 15 15 15 D1 1 1 15 r3.5 3.5 5 5 a c10.5 10.5 2.5 2.5"},
    {ShapeEffects, "a f4.5 4.5 10 10 n b1.5 1.5 10 10"},
    {AltText, "r1.5 2.5 9 8 p3 9 5.5 6 8 9 a l12 5 15 5 l12 8 15 8 l1.5 13 15 13"},
    {DropDownList, "r1.5 4 13 8 l10.5 4 10.5 12 a P11.8 7 14 7 12.9 9"},
    // Tables
    {TableInsertAbove, "a f1.5 1.5 13 4 n r1.5 5.5 13 9 l1.5 10 14.5 10 l6 5.5 6 14.5 l10 5.5 10 14.5"},
    {TableInsertBelow, "r1.5 1.5 13 9 l1.5 6 14.5 6 l6 1.5 6 10.5 l10 1.5 10 10.5 a f1.5 11 13 4"},
    {TableInsertLeft, "a f1.5 1.5 4 13 n r5.5 1.5 9 13 l5.5 6 14.5 6 l5.5 10 14.5 10 l10 1.5 10 14.5"},
    {TableInsertRight, "r1.5 1.5 9 13 l1.5 6 10.5 6 l1.5 10 10.5 10 l6 1.5 6 14.5 a f11 1.5 4 13"},
    {TableDelete, "r1.5 1.5 9 9 l1.5 6 10.5 6 l6 1.5 6 10.5 a w2 l10.5 10.5 15 15 l15 10.5 10.5 15"},
    {MergeCells, "r1.5 3.5 13 9 a p3.5 8 7 8 p5.5 6 7.5 8 5.5 10 p12.5 8 9 8 p10.5 6 8.5 8 10.5 10"},
    {SplitCells, "r1.5 3.5 13 9 l8 3.5 8 12.5 a p6.5 8 3 8 p4.5 6 2.5 8 4.5 10 p9.5 8 13 8 p11.5 6 13.5 8 11.5 10"},
    {AutoFit, "r1.5 1.5 13 9 l6 1.5 6 10.5 l10 1.5 10 10.5 a l2 13.5 14 13.5 p4 12 2 13.5 4 15 p12 12 14 13.5 12 15"},
    {DistributeColumns, "r1.5 1.5 13 9 l5.8 1.5 5.8 10.5 l10.2 1.5 10.2 10.5 a l2 13.5 5 13.5 l11 13.5 14 13.5 p3.5 12 2 13.5 3.5 15 p12.5 12 14 13.5 12.5 15"},
    {DistributeRows, "r1.5 1.5 9 13 l1.5 5.8 10.5 5.8 l1.5 10.2 10.5 10.2 a l13.5 2 13.5 14 p12 4 13.5 2 15 4 p12 12 13.5 14 15 12"},
    {RepeatHeader, "a f2 2 12 3.5 n r1.5 1.5 13 13 l1.5 9.5 14.5 9.5 l6 1.5 6 14.5 l10 1.5 10 14.5"},
    {ConvertToText, "r1.5 1.5 6 6 l1.5 4.5 7.5 4.5 l4.5 1.5 4.5 7.5 a p9 4.5 12.5 4.5 p11 3 12.5 4.5 11 6 n l2 10.5 14.5 10.5 l2 13.5 11 13.5"},
    // Charts
    {ChartColumn, "a f2 9 3 6 f6.5 4 3 11 f11 6.5 3 8.5 n l1 15.5 15 15.5"},
    {ChartBar, "a f1 2 8 3 f1 6.5 13 3 f1 11 10 3 n l0.5 1 0.5 15"},
    {ChartLine, "l1 1 1 15.5 l1 15.5 15 15.5 a p2.5 12 6 7.5 9.5 10 14.5 3"},
    {ChartArea, "a P2 14.5 2 10 6 6 10 9 14.5 4 14.5 14.5 n l1 15.5 15 15.5"},
    {ChartPie, "e8 8 6.5 6.5 a g8 8 6.5 0 120"},
    {ChartDoughnut, "e8 8 6.5 6.5 a g8 8 6.5 0 120 n c8 8 3 3"},
    {ChartScatter, "l1 1 1 15.5 l1 15.5 15 15.5 a o4.5 11.5 1.3 1.3 o7.5 6.5 1.3 1.3 o10.5 9.5 1.3 1.3 o13 3.5 1.3 1.3"},
    {ChartCombo, "a f2 9 3 6 f7 6 3 9 f12 10 3 5 n p1 8 6 3 11 6 15 2 l1 15.5 15 15.5"},
    // Data and sheet views
    {SortAscending, "t0 0 9 8 7.5'A' t0 8 9 8 7.5'Z' a l12 1.5 12 14 p9.5 11.5 12 14 14.5 11.5"},
    {SortDescending, "t0 0 9 8 7.5'Z' t0 8 9 8 7.5'A' a l12 1.5 12 14 p9.5 11.5 12 14 14.5 11.5"},
    {RemoveDuplicates, "r1.5 1.5 9 13 l1.5 5.5 10.5 5.5 l1.5 9.5 10.5 9.5 a l12 6 15 9 l15 6 12 9"},
    {GroupRows, "l6 2.5 15 2.5 l6 6.5 15 6.5 l6 10.5 15 10.5 l6 14.5 15 14.5 a p3.5 2.5 1.5 2.5 1.5 14.5 3.5 14.5"},
    {UngroupRows, "l6 2.5 15 2.5 l6 6.5 15 6.5 l6 10.5 15 10.5 l6 14.5 15 14.5 a p3.5 2.5 1.5 2.5 1.5 5 p1.5 12 1.5 14.5 3.5 14.5"},
    {ViewNormal, "a f1.5 1.5 13 3 n r1.5 1.5 13 13 l1.5 8 14.5 8 l1.5 11.25 14.5 11.25 l6 1.5 6 14.5 l10.5 1.5 10.5 14.5"},
    {PageBreakPreview, "r1.5 1.5 13 13 l3.5 4.5 6.5 4.5 l3.5 11.5 6.5 11.5 l9.5 4.5 12.5 4.5 l9.5 11.5 12.5 11.5 a w1.5 D8 1.5 8 14.5 D1.5 8 14.5 8"},
    {FreezePanes, "r1.5 1.5 13 13 l1.5 10 14.5 10 l10 1.5 10 14.5 a w2 l1.5 5.5 14.5 5.5 l5.5 1.5 5.5 14.5"},
};

void FillShape(Canvas& c, const std::vector<D2D1_POINT_2F>& pts, D2D1_COLOR_F color, bool sector, float radius) {
  ComPtr<ID2D1PathGeometry> geo;
  if (pts.size() < 3 || FAILED(D2D()->CreatePathGeometry(&geo))) return;
  ComPtr<ID2D1GeometrySink> sink;
  if (FAILED(geo->Open(&sink))) return;
  sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
  if (sector) {  // center, start, end: an arc from start to end
    sink->AddLine(pts[1]);
    const float dx1 = pts[1].x - pts[0].x, dy1 = pts[1].y - pts[0].y, dx2 = pts[2].x - pts[0].x, dy2 = pts[2].y - pts[0].y;
    const bool large = dx1 * dy2 - dy1 * dx2 < 0;  // more than half way round
    sink->AddArc(D2D1::ArcSegment(pts[2], D2D1::SizeF(radius, radius), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
  } else {
    for (size_t i = 1; i < pts.size(); ++i) sink->AddLine(pts[i]);
  }
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);
  sink->Close();
  c.DC()->FillGeometry(geo.Get(), c.Brush(color));
}

}  // namespace

void DrawDrawn(Canvas& c, wchar_t glyph, const D2D1_RECT_F& r, D2D1_COLOR_F color, float size) {
  const char* q = nullptr;
  for (const Path& p : kPaths)
    if (p.glyph == glyph) q = p.steps;
  if (!q) return;
  const Palette& pal = theme::P();
  const float s = size / 16;
  const float ox = std::round((r.left + r.right) / 2 - size / 2), oy = std::round((r.top + r.bottom) / 2 - size / 2);
  const float base = std::max(1.0f, std::round(size / 16));
  float sw = base;
  // The accent only where the icon is in the text color: a disabled or checked one stays one color.
  const bool plain = color.r != pal.text.r || color.g != pal.text.g || color.b != pal.text.b || color.a != pal.text.a;
  const D2D1_COLOR_F accent = plain ? color : pal.accentText;
  D2D1_COLOR_F cur = color;
  auto X = [&](float v) { return ox + v * s; };
  auto Y = [&](float v) { return oy + v * s; };
  auto snap = [&](float v) { return static_cast<int>(sw) % 2 == 1 ? std::floor(v) + 0.5f : std::round(v); };
  auto skip = [&] {
    while (*q == ' ' || *q == ',') ++q;
  };
  auto more = [&] {
    skip();
    return (*q >= '0' && *q <= '9') || *q == '-' || *q == '.';
  };
  auto num = [&]() -> float {
    skip();
    char* end = nullptr;
    const float v = std::strtof(q, &end);
    q = end && end != q ? end : q + (*q ? 1 : 0);
    return v;
  };
  auto line = [&](float x1, float y1, float x2, float y2) {
    float ax = X(x1), ay = Y(y1), bx = X(x2), by = Y(y2);
    if (y1 == y2) ay = by = snap(ay);
    if (x1 == x2) ax = bx = snap(ax);
    c.Line(ax, ay, bx, by, cur, sw);
  };
  auto box = [&](float x, float y, float w, float h) {
    return RectF(snap(X(x)), snap(Y(y)), snap(X(x + w)), snap(Y(y + h)));
  };
  while (*q) {
    const char op = *q++;
    switch (op) {
      case 'a': cur = accent; break;
      case 'n': cur = color; break;
      case 'w': sw = base * num(); break;
      case 'l': {
        const float x1 = num(), y1 = num(), x2 = num(), y2 = num();
        line(x1, y1, x2, y2);
        break;
      }
      case 'p': {
        float px = num(), py = num();
        while (more()) {
          const float nx = num(), ny = num();
          line(px, py, nx, ny);
          px = nx;
          py = ny;
        }
        break;
      }
      case 'P': {
        std::vector<D2D1_POINT_2F> pts;
        while (more()) {
          const float x = num(), y = num();
          pts.push_back({X(x), Y(y)});
        }
        FillShape(c, pts, cur, false, 0);
        break;
      }
      case 'r':
      case 'b': {
        const float x = num(), y = num(), w = num(), h = num();
        const D2D1_RECT_F b = box(x, y, w, h);
        if (op == 'b') c.Fill(b, pal.chrome);
        c.Stroke(b, cur, sw);
        break;
      }
      case 'f': {
        const float x = num(), y = num(), w = num(), h = num();
        c.Fill(RectF(std::round(X(x)), std::round(Y(y)), std::round(X(x + w)), std::round(Y(y + h))), cur);
        break;
      }
      case 'e':
      case 'o':
      case 'c': {
        const float cx = num(), cy = num(), rx = num(), ry = num();
        if (op != 'e') c.FillEllipse(X(cx), Y(cy), rx * s, ry * s, op == 'o' ? cur : pal.chrome);
        if (op != 'o') c.DC()->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(X(cx), Y(cy)), rx * s, ry * s), c.Brush(cur), sw);
        break;
      }
      case 'g': {
        const float cx = num(), cy = num(), rad = num(), a0 = num(), a1 = num();
        auto at = [&](float deg) {
          const float t = deg * 3.14159265f / 180;
          return D2D1::Point2F(X(cx + rad * std::sin(t)), Y(cy - rad * std::cos(t)));
        };
        FillShape(c, {D2D1::Point2F(X(cx), Y(cy)), at(a0), at(a1)}, cur, true, rad * s);
        break;
      }
      case 'D': {
        const float x1 = X(num()), y1 = Y(num()), x2 = X(num()), y2 = Y(num());
        const float len = std::hypot(x2 - x1, y2 - y1);
        const int n = std::max(1, static_cast<int>(len / (2 * sw)));
        for (int i = 0; i <= n; ++i) {
          const float x = std::round(x1 + (x2 - x1) * i / n - sw / 2), y = std::round(y1 + (y2 - y1) * i / n - sw / 2);
          c.Fill(RectF(x, y, x + sw, y + sw), cur);
        }
        break;
      }
      case 't': {
        const float x = num(), y = num(), w = num(), h = num(), sz = num();
        skip();
        if (*q != '\'') break;
        const char* from = ++q;
        while (*q && *q != '\'') ++q;
        c.Text(fw::Utf8ToWide(std::string_view(from, static_cast<size_t>(q - from))), RectF(X(x), Y(y), X(x + w), Y(y + h)), cur, sz * s,
               DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center);
        if (*q) ++q;
        break;
      }
      default: break;  // spaces
    }
  }
}

}  // namespace icon
