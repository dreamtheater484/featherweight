// Objects on slides (P2, p2-objects): frames and handles, resizing (Shift, Ctrl, flipping past the
// other side, turned objects), lines' ends, turning with Shift, smart guides (slide, objects,
// equal spacing, the grid), align, distribute, order, and selection over a slide's object tree.

#include <pugixml.hpp>

#include <cmath>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "slide/objects/Arrange.h"
#include "slide/objects/Frame.h"
#include "slide/objects/Guides.h"
#include "slide/objects/Selection.h"

using namespace fw::slide::objects;

namespace {

Frame Box(float l, float t, float r, float b, float rot = 0) {
  Frame f;
  f.rect = {l, t, r, b};
  f.rotation = rot;
  return f;
}

bool Near(const draw::Rect& a, const draw::Rect& b, float eps = 0.01f) {
  return std::fabs(a.left - b.left) <= eps && std::fabs(a.top - b.top) <= eps && std::fabs(a.right - b.right) <= eps &&
         std::fabs(a.bottom - b.bottom) <= eps;
}

bool HasGuide(const Snap& s, Guide::Kind k, float at) {
  for (const Guide& g : s.guides)
    if (g.kind == k && std::fabs(g.at - at) < 0.01f) return true;
  return false;
}

}  // namespace

TEST(SlideObjects_HandlesAndHits) {
  const Frame f = Box(100, 100, 200, 150);
  const draw::Point tl = HandlePoint(f, Handle::TopLeft, 20), r = HandlePoint(f, Handle::Right, 20);
  CHECK_NEAR(tl.x, 100, 0.01);
  CHECK_NEAR(tl.y, 100, 0.01);
  CHECK_NEAR(r.x, 200, 0.01);
  CHECK_NEAR(r.y, 125, 0.01);
  const draw::Point rot = HandlePoint(f, Handle::Rotate, 20);
  CHECK_NEAR(rot.x, 150, 0.01);
  CHECK_NEAR(rot.y, 80, 0.01);
  HandleSet set;
  CHECK(HitHandle(f, set, {101, 99}, 4, 20) == Handle::TopLeft);
  CHECK(HitHandle(f, set, {150, 81}, 4, 20) == Handle::Rotate);
  CHECK(HitHandle(f, set, {150, 125}, 4, 20) == Handle::Body);
  CHECK(HitHandle(f, set, {300, 125}, 4, 20) == Handle::None);
  // Turned 90 degrees: the top edge's middle is now on the right.
  const Frame t = Box(100, 100, 200, 150, 90);
  const draw::Point top = HandlePoint(t, Handle::Top, 0);
  CHECK_NEAR(top.x, 175, 0.01);
  CHECK_NEAR(top.y, 125, 0.01);
  CHECK(Near(t.Bounds(), {125, 75, 175, 175}));
  CHECK(HitFrame(t, false, {150, 80}, 0));
  CHECK(!HitFrame(t, false, {110, 125}, 0));
  // A line is hit near its segment only.
  const Frame line = Frame::FromLine({0, 0}, {100, 100});
  CHECK(HitFrame(line, true, {50, 52}, 3));
  CHECK(!HitFrame(line, true, {80, 20}, 3));
  HandleSet ls;
  ls.line = true;
  CHECK(HitHandle(line, ls, {99, 99}, 3, 0) == Handle::End);
  CHECK(HitHandle(line, ls, {1, 1}, 3, 0) == Handle::Start);
}

TEST(SlideObjects_Resize) {
  const Frame f = Box(100, 100, 200, 150);
  ResizeOptions o;
  // The right edge out by 50.
  Frame r = Resize(f, Handle::Right, {200, 125}, {250, 140}, o);
  CHECK(Near(r.rect, {100, 100, 250, 150}));
  // The bottom right corner, Shift: proportions kept (the larger change wins).
  o.keepAspect = true;
  r = Resize(f, Handle::BottomRight, {200, 150}, {300, 160}, o);
  CHECK(Near(r.rect, {100, 100, 300, 200}));
  // Ctrl: from the centre.
  o = {};
  o.fromCentre = true;
  r = Resize(f, Handle::Right, {200, 125}, {210, 125}, o);
  CHECK(Near(r.rect, {90, 100, 210, 150}));
  // Past the left edge: flipped.
  o = {};
  r = Resize(f, Handle::Right, {200, 125}, {50, 125}, o);
  CHECK(Near(r.rect, {50, 100, 100, 150}));
  CHECK(r.flipH);
  CHECK(!r.flipV);
  // A turned object: the drag is along its own axes, and the opposite edge stays put on the slide.
  const Frame t = Box(100, 100, 200, 150, 90);
  const draw::Point leftBefore = HandlePoint(t, Handle::Left, 0);
  r = Resize(t, Handle::Right, HandlePoint(t, Handle::Right, 0), {150, 200}, {});  // down = its +x
  CHECK_NEAR(r.rect.Width(), 125, 0.01);
  const draw::Point leftAfter = HandlePoint(r, Handle::Left, 0);
  CHECK_NEAR(leftAfter.x, leftBefore.x, 0.01);
  CHECK_NEAR(leftAfter.y, leftBefore.y, 0.01);
  // An edge with Shift grows the other side about the middle.
  o = {};
  o.keepAspect = true;
  r = Resize(f, Handle::Right, {200, 125}, {300, 125}, o);
  CHECK(Near(r.rect, {100, 75, 300, 175}));
  // The minimum size holds.
  o = {};
  o.minSize = 5;
  r = Resize(f, Handle::Right, {200, 125}, {101, 125}, o);
  CHECK_NEAR(r.rect.Width(), 5, 0.01);
}

TEST(SlideObjects_LinesAndTurning) {
  const Frame line = Frame::FromLine({100, 200}, {200, 100});
  CHECK(!line.flipH);
  CHECK(line.flipV);
  auto e = line.LineEnds();
  CHECK_NEAR(e[0].x, 100, 0.01);
  CHECK_NEAR(e[0].y, 200, 0.01);
  CHECK_NEAR(e[1].x, 200, 0.01);
  CHECK_NEAR(e[1].y, 100, 0.01);
  // The end dragged with Shift: kept at 45 degrees.
  Frame moved = MoveLineEnd(line, Handle::End, {300, 190}, true);
  e = moved.LineEnds();
  CHECK_NEAR(e[0].x, 100, 0.01);
  CHECK_NEAR(e[0].y, 200, 0.01);
  CHECK_NEAR(e[1].y, 200, 0.01);  // horizontal
  CHECK(e[1].x > 290);
  // Turning: a quarter turn by dragging from above the centre to its right; Shift snaps to 15.
  const Frame f = Box(0, 0, 100, 100);
  Frame t = Rotate(f, {50, -20}, {170, 50}, false);
  CHECK_NEAR(t.rotation, 90, 0.01);
  t = Rotate(f, {50, -20}, {150, 40}, true);
  CHECK_NEAR(std::fmod(t.rotation, 15.f), 0, 0.01);
  CHECK_NEAR(NormalizeDegrees(-90), 270, 0.001);
  CHECK_NEAR(NormalizeDegrees(720), 0, 0.001);
}

TEST(SlideObjects_Guides) {
  const draw::Size slide{960, 540};
  SnapSettings s;
  s.tolerance = 5;
  // Near the slide's centre: snapped to it, a guide down the whole slide.
  Snap snap = SnapMove({427, 100, 527, 150}, {}, slide, s);
  CHECK_NEAR(snap.dx, 3, 0.01);
  CHECK(HasGuide(snap, Guide::Kind::Vertical, 480));
  // Another object's top edge.
  snap = SnapMove({600, 202, 700, 252}, {{100, 200, 200, 300}}, slide, s);
  CHECK_NEAR(snap.dy, -2, 0.01);
  CHECK(HasGuide(snap, Guide::Kind::Horizontal, 200));
  for (const Guide& g : snap.guides)
    if (g.kind == Guide::Kind::Horizontal) CHECK(g.from <= 100 && g.to >= 700);
  // Nothing near: no snap; with a grid, onto it.
  snap = SnapMove({603, 403, 653, 433}, {}, slide, s);
  CHECK_NEAR(snap.dx, 0, 0.001);
  CHECK(snap.guides.empty());
  s.grid = 6;
  snap = SnapMove({602, 403, 652, 433}, {}, slide, s);
  CHECK_NEAR(snap.dx, -2, 0.01);
  CHECK_NEAR(snap.dy, -1, 0.01);
  s.grid = 0;
  // Equal spacing: between two objects in a row.
  std::vector<draw::Rect> row = {{100, 300, 200, 350}, {500, 300, 600, 350}};
  snap = SnapMove({302, 310, 402, 340}, row, slide, s);  // gaps 102 and 98: centred is 300
  CHECK_NEAR(snap.dx, -2, 0.01);
  int gaps = 0;
  for (const Guide& g : snap.guides) gaps += g.kind == Guide::Kind::GapX;
  CHECK_EQ(gaps, 2);
  // Equal spacing: as far from the last as the last two are apart.
  row = {{100, 300, 150, 350}, {200, 300, 250, 350}};
  snap = SnapMove({302, 300, 352, 350}, row, slide, s);
  CHECK_NEAR(snap.dx, -2, 0.01);
  // Resizing snaps only the dragged edge.
  Edges e;
  e.right = true;
  snap = SnapResize({100, 100, 297, 150}, e, {{300, 400, 400, 500}}, slide, s);
  CHECK_NEAR(snap.dx, 3, 0.01);
  CHECK_NEAR(snap.dy, 0, 0.001);
  // Off: nothing.
  SnapSettings off;
  off.slide = off.objects = off.spacing = false;
  snap = SnapMove({427, 100, 527, 150}, {}, slide, off);
  CHECK_NEAR(snap.dx, 0, 0.001);
}

TEST(SlideObjects_AlignDistributeOrder) {
  const draw::Size slide{960, 540};
  const std::vector<draw::Rect> boxes = {{100, 100, 200, 150}, {300, 200, 350, 260}, {500, 50, 520, 80}};
  auto d = Align(boxes, AlignTo::Left, false, slide);
  CHECK_NEAR(d[0].x, 0, 0.001);
  CHECK_NEAR(d[1].x, -200, 0.001);
  CHECK_NEAR(d[2].x, -400, 0.001);
  d = Align(boxes, AlignTo::Bottom, false, slide);
  CHECK_NEAR(d[0].y, 110, 0.001);
  CHECK_NEAR(d[1].y, 0, 0.001);
  d = Align({boxes[0]}, AlignTo::Centre, true, slide);
  CHECK_NEAR(d[0].x, 330, 0.001);
  d = Align(boxes, AlignTo::Middle, false, slide);
  CHECK_NEAR(d[2].y + 65, 155, 0.001);  // all centred on 155 (the span 50-260)
  // Distribute: the first and last stay, the gaps match.
  d = Distribute(boxes, true, false, slide);
  CHECK_NEAR(d[0].x, 0, 0.001);
  CHECK_NEAR(d[2].x, 0, 0.001);
  const float gap1 = boxes[1].left + d[1].x - boxes[0].right, gap2 = boxes[2].left - (boxes[1].right + d[1].x);
  CHECK_NEAR(gap1, gap2, 0.01);
  // Two objects: nothing, unless to the slide.
  d = Distribute({boxes[0], boxes[1]}, true, false, slide);
  CHECK_NEAR(d[1].x, 0, 0.001);
  d = Distribute({boxes[0], boxes[1]}, true, true, slide);
  CHECK_NEAR(boxes[0].left + d[0].x, 0, 0.01);
  CHECK_NEAR(boxes[1].right + d[1].x, 960, 0.01);
  // Order: forward passes one unselected neighbour; selected keep their order.
  auto o = Reorder({true, false, false}, Order::Forward);
  CHECK((o == std::vector<size_t>{1, 0, 2}));
  o = Reorder({true, true, false}, Order::Forward);
  CHECK((o == std::vector<size_t>{2, 0, 1}));
  o = Reorder({false, false, true}, Order::Backward);
  CHECK((o == std::vector<size_t>{0, 2, 1}));
  o = Reorder({true, false, true, false}, Order::ToFront);
  CHECK((o == std::vector<size_t>{1, 3, 0, 2}));
  o = Reorder({false, true, false, true}, Order::ToBack);
  CHECK((o == std::vector<size_t>{1, 3, 0, 2}));
  o = Reorder({false, false, true}, Order::Forward);  // already on top
  CHECK((o == std::vector<size_t>{0, 1, 2}));
}

TEST(SlideObjects_Selection) {
  // A slide: a rectangle, a group of two (its children's space scaled by 2), and a line on top.
  fw::slide::Slide slide;
  pugi::xml_document doc;
  doc.load_string(
      "<p:sld xmlns:p='http://schemas.openxmlformats.org/presentationml/2006/main' "
      "xmlns:a='http://schemas.openxmlformats.org/drawingml/2006/main'><p:cSld><p:spTree>"
      "<p:sp><p:nvSpPr><p:cNvPr id='2' name='A'/></p:nvSpPr><p:spPr/></p:sp>"
      "<p:grpSp><p:nvGrpSpPr><p:cNvPr id='3' name='G'/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x='2540000' y='0'/>"
      "<a:ext cx='2540000' cy='1270000'/><a:chOff x='0' y='0'/><a:chExt cx='1270000' cy='635000'/></a:xfrm></p:grpSpPr>"
      "<p:sp><p:nvSpPr><p:cNvPr id='4' name='B'/></p:nvSpPr><p:spPr/></p:sp>"
      "<p:sp><p:nvSpPr><p:cNvPr id='5' name='C'/></p:nvSpPr><p:spPr/></p:sp></p:grpSp>"
      "<p:cxnSp><p:nvCxnSpPr><p:cNvPr id='6' name='L'/></p:nvCxnSpPr><p:spPr/></p:cxnSp>"
      "</p:spTree></p:cSld></p:sld>");
  pugi::xml_node tree = doc.document_element().first_child().first_child();
  pugi::xml_node a = tree.first_child(), g = a.next_sibling(), l = g.next_sibling();
  using K = fw::slide::Shape::Kind;
  fw::slide::Shape sa;
  sa.kind = K::Shape, sa.id = 2, sa.node = sa.drawNode = a, sa.bounds = {0, 0, 100, 100};
  fw::slide::Shape sg;
  sg.kind = K::Group, sg.id = 3, sg.node = sg.drawNode = g, sg.bounds = {200, 0, 400, 100};
  fw::slide::Shape sb;
  sb.kind = K::Shape, sb.id = 4, sb.node = sb.drawNode = g.child("p:sp"), sb.bounds = {0, 0, 40, 50};
  fw::slide::Shape sc;
  sc.kind = K::Shape, sc.id = 5, sc.node = sc.drawNode = g.last_child(), sc.bounds = {60, 0, 100, 50};
  sg.children = {sb, sc};
  fw::slide::Shape sl;
  sl.kind = K::Connector, sl.id = 6, sl.node = sl.drawNode = l, sl.bounds = {0, 0, 500, 300};
  slide.shapes = {sa, sg, sl};
  ObjectMap map;
  map.Build(slide);
  CHECK_EQ(map.Objects().size(), size_t{5});
  // The group's children on the slide: scaled by 2 and moved by 200.
  const ObjectInfo* b = map.Find(4);
  CHECK(b && b->parent == 3);
  CHECK(Near(b->frame.rect, {200, 0, 280, 100}));
  const ObjectInfo* c = map.Find(5);
  CHECK(Near(c->frame.rect, {320, 0, 400, 100}));
  // Clicking a child selects its group at the slide's level, the child inside the group.
  CHECK_EQ(map.HitTest({250, 50}, 0, 2), 3u);
  CHECK_EQ(map.HitTest({250, 50}, 3, 2), 4u);
  CHECK_EQ(map.HitDeepest({350, 50}, 2), 5u);
  // Between the group's children, nothing of the group is hit; the line is on top of everything.
  CHECK_EQ(map.HitTest({300, 10}, 0, 2), 0u);
  CHECK_EQ(map.HitTest({50, 30}, 0, 2), 6u);
  CHECK_EQ(map.HitTest({90, 10}, 0, 2), 2u);
  // Marquee: wholly inside only.
  auto in = map.InMarquee({-1, -1, 150, 150}, 0);
  CHECK((in == std::vector<uint32_t>{2}));
  in = map.InMarquee({190, -1, 410, 110}, 0);
  CHECK((in == std::vector<uint32_t>{3}));
  in = map.InMarquee({190, -1, 300, 110}, 3);
  CHECK((in == std::vector<uint32_t>{4}));
  // Tab order, wrapping.
  CHECK_EQ(map.Next(0, 0, false), 2u);
  CHECK_EQ(map.Next(2, 0, false), 3u);
  CHECK_EQ(map.Next(6, 0, false), 2u);
  CHECK_EQ(map.Next(2, 0, true), 6u);
  CHECK_EQ(map.Next(4, 3, false), 5u);
  CHECK(map.Within(5, 3));
  CHECK_EQ(map.AtLevel(5, 0), 3u);
  CHECK(Near(SelectionBounds(map, {2, 3}), {0, 0, 400, 100}));
}
