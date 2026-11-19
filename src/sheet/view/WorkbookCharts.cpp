// Charts and the objects in a sheet's drawing in the grid (S3, s3-charts): Insert Chart (Alt+F1 and
// the toolbar), and a chart, picture or shape selected with a click, moved by dragging it, resized
// by its handles (Shift keeps a corner's aspect ratio), nudged with the arrow keys and deleted.
// The edits are the Editor's (MoveObject, DeleteObjects, InsertChart); the objects are read again
// after each (kObjects).

#include <algorithm>
#include <cmath>

#include "Commands.h"
#include "sheet/data/Filter.h"
#include "sheet/view/WorkbookView.h"
#include "ui/Toolbar.h"

namespace fw::sheet::view {

namespace {

constexpr double kEmuPerPt = 12700;

// The handles' centers: clockwise from the top left (corners even, edges odd).
draw::Point HandlePoint(const draw::Rect& r, int i) {
  const float mx = (r.left + r.right) / 2, my = (r.top + r.bottom) / 2;
  switch (i) {
    case 0: return {r.left, r.top};
    case 1: return {mx, r.top};
    case 2: return {r.right, r.top};
    case 3: return {r.right, my};
    case 4: return {r.right, r.bottom};
    case 5: return {mx, r.bottom};
    case 6: return {r.left, r.bottom};
    default: return {r.left, my};
  }
}

bool Inside(const draw::Rect& r, draw::Point p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

}  // namespace

std::optional<SheetObjects::Object> WorkbookView::SelectedObject() const {
  const SheetObjects* objects = object_ && objectSheet_ == sheet_ && !ChartSheet() ? Objects() : nullptr;
  if (objects)
    for (const SheetObjects::Object& o : objects->Objects())
      if (o.id == object_) return o;
  return std::nullopt;
}

std::optional<draw::Point> WorkbookView::SheetPointAt(float x, float y) const {
  const std::vector<Pane> panes = Panes();
  if (panes.empty()) return std::nullopt;
  const float fx = Mirror(x);
  const Pane* at = &panes.back();  // outside them all (a drag past the edge): the scrolling one's
  for (const Pane& p : panes)
    if (fx >= p.rc.left && fx < p.rc.right && y >= p.rc.top && y < p.rc.bottom) at = &p;
  const double k = K();
  return draw::Point{static_cast<float>((at->sx + (fx - at->rc.left) / k) * kPtPerPx),
                     static_cast<float>((at->sy + (y - at->rc.top) / k) * kPtPerPx)};
}

edit::drawing::Place WorkbookView::PlaceFor(const draw::Rect& r) const {
  auto marker = [&](float xPt, float yPt) {
    const double x = std::max(0.0, xPt / kPtPerPx), y = std::max(0.0, yPt / kPtPerPx);  // Excel pixels
    edit::drawing::Marker m;
    m.col = std::min(grid_.ColAt(static_cast<int64_t>(x)), kMaxCols - 1);
    m.row = std::min(grid_.RowAt(static_cast<int64_t>(y)), kMaxRows - 1);
    m.colOff = std::max<int64_t>(0, std::llround((x - static_cast<double>(grid_.ColLeft(m.col))) * kPtPerPx * kEmuPerPt));
    m.rowOff = std::max<int64_t>(0, std::llround((y - static_cast<double>(grid_.RowTop(m.row))) * kPtPerPx * kEmuPerPt));
    return m;
  };
  edit::drawing::Place p;
  p.from = marker(r.left, r.top);
  p.to = marker(r.right, r.bottom);
  p.x = std::llround(r.left * kEmuPerPt);
  p.y = std::llround(r.top * kEmuPerPt);
  p.cx = std::llround(r.Width() * kEmuPerPt);
  p.cy = std::llround(r.Height() * kEmuPerPt);
  return p;
}

bool WorkbookView::ObjectMouseDown(float x, float y) {
  if (!gfx::Contains(cells_, x, y) || ChartSheet()) return false;
  Bar v, h;
  Bars(v, h);
  if ((v.shown && gfx::Contains(v.track, x, y)) || (h.shown && gfx::Contains(h.track, x, y))) return false;
  SheetObjects* objects = Objects();
  const std::optional<draw::Point> at = SheetPointAt(x, y);
  if (!objects || !at) return false;
  const float reach = static_cast<float>(5 * ui_ / K() * kPtPerPx);  // 5 device pixels, in points
  auto grab = [&](const SheetObjects::Object& o, int handle) {
    object_ = o.id;
    objectSheet_ = sheet_;
    objectDrag_ = ObjectDrag{handle, o.rect, o.rect, *at, false};
    mouse_ = Mouse::Object;
    host_.ViewInvalidate();
    host_.ViewStateChanged();
    return true;
  };
  if (const std::optional<SheetObjects::Object> sel = SelectedObject())
    for (int i = 0; i < 8; ++i) {
      const draw::Point c = HandlePoint(sel->rect, i);
      if (std::abs(c.x - at->x) <= reach && std::abs(c.y - at->y) <= reach) return grab(*sel, i);
    }
  const std::vector<SheetObjects::Object> all = objects->Objects();
  for (auto it = all.rbegin(); it != all.rend(); ++it)  // the top one
    if (it->id && Inside(it->rect, *at)) return grab(*it, -1);
  if (object_) {
    object_ = 0;
    host_.ViewInvalidate();
    host_.ViewStateChanged();
  }
  return false;
}

void WorkbookView::ObjectMouseMove(float x, float y, UINT mods) {
  const std::optional<draw::Point> at = SheetPointAt(x, y);
  if (!at) return;
  ObjectDrag& d = objectDrag_;
  const float dx = at->x - d.from.x, dy = at->y - d.from.y;
  if (!d.moved && std::abs(dx) + std::abs(dy) < static_cast<float>(3 * ui_ / K() * kPtPerPx)) return;
  d.moved = true;
  draw::Rect r = d.start;
  const float least = static_cast<float>(kPtPerPx);
  if (d.handle < 0) {
    const float mx = std::max(dx, -r.left), my = std::max(dy, -r.top);  // not past A1
    r = {r.left + mx, r.top + my, r.right + mx, r.bottom + my};
  } else {
    const int h = d.handle;
    if (h == 0 || h == 6 || h == 7) r.left = std::min(r.left + dx, r.right - least);
    if (h >= 2 && h <= 4) r.right = std::max(r.right + dx, r.left + least);
    if (h <= 2) r.top = std::min(r.top + dy, r.bottom - least);
    if (h >= 4 && h <= 6) r.bottom = std::max(r.bottom + dy, r.top + least);
    // Shift on a corner keeps the shape's proportions, from the opposite corner.
    if ((mods & kModShift) && h % 2 == 0 && d.start.Width() > 0 && d.start.Height() > 0) {
      const float s = std::max(r.Width() / d.start.Width(), r.Height() / d.start.Height());
      const float w = d.start.Width() * s, ht = d.start.Height() * s;
      if (h == 0 || h == 6) r.left = r.right - w;
      else r.right = r.left + w;
      if (h <= 2) r.top = r.bottom - ht;
      else r.bottom = r.top + ht;
    }
    r.left = std::max(r.left, 0.0f);
    r.top = std::max(r.top, 0.0f);
  }
  d.now = r;
  host_.ViewInvalidate();
}

void WorkbookView::ObjectMouseUp() {
  mouse_ = Mouse::None;
  if (objectDrag_.moved && SelectedObject()) EditRefused(Ed().MoveObject(static_cast<int>(sheet_), object_, PlaceFor(objectDrag_.now)));
  objectDrag_ = {};
  host_.ViewInvalidate();
}

bool WorkbookView::ObjectKey(UINT vk, UINT mods) {
  const std::optional<SheetObjects::Object> sel = SelectedObject();
  if (!sel) return false;
  const bool ctrl = (mods & kModCtrl) != 0, alt = (mods & kModAlt) != 0;
  if (ctrl || alt) return false;
  switch (vk) {
    case VK_ESCAPE:
      object_ = 0;
      break;
    case VK_DELETE:
    case VK_BACK:
      if (!EditRefused(Ed().DeleteObjects(static_cast<int>(sheet_), {object_}))) object_ = 0;
      break;
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN: {
      // A nudge of a pixel, as Excel's.
      const float step = static_cast<float>(kPtPerPx);
      const float dx = vk == VK_LEFT ? -step : vk == VK_RIGHT ? step : 0, dy = vk == VK_UP ? -step : vk == VK_DOWN ? step : 0;
      draw::Rect r = sel->rect;
      if (r.left + dx < 0 || r.top + dy < 0) return true;
      r = {r.left + dx, r.top + dy, r.right + dx, r.bottom + dy};
      EditRefused(Ed().MoveObject(static_cast<int>(sheet_), object_, PlaceFor(r)));
      break;
    }
    default: return false;
  }
  host_.ViewInvalidate();
  host_.ViewStateChanged();
  return true;
}

void WorkbookView::DrawObjectSelection(draw::Painter& painter, float scale, float mirrorX) {
  const std::optional<SheetObjects::Object> sel = SelectedObject();
  if (!sel) return;
  draw::Rect r = mouse_ == Mouse::Object && objectDrag_.moved ? objectDrag_.now : sel->rect;
  if (mirrorX != 0) r = {mirrorX - r.right, r.top, mirrorX - r.left, r.bottom};
  const float px = 1 / scale;  // a device pixel, in points
  const draw::Brush gray = draw::Brush::Solid({0.42f, 0.42f, 0.42f, 1});
  draw::Pen pen;
  pen.brush = gray;
  pen.width = px;
  painter.StrokePath(draw::Path::FromRect(r), pen);
  const draw::Brush white = draw::Brush::Solid({1, 1, 1, 1});
  for (int i = 0; i < 8; ++i) {
    const draw::Point c = HandlePoint(r, i);
    const draw::Path dot = draw::Path::Ellipse({c.x - 4 * px, c.y - 4 * px, c.x + 4 * px, c.y + 4 * px});
    painter.FillPath(dot, white);
    painter.StrokePath(dot, pen);
  }
}

// ---- Commands

bool WorkbookView::ChartsCommandState(int cmd, CommandState& s) {
  if (cmd < CmdInsertChart || cmd > CmdDeleteObject) return false;
  static const wchar_t* const kNames[] = {L"Column chart", L"Bar chart",  L"Line chart",   L"Pie chart",
                                          L"Doughnut chart", L"Area chart", L"Scatter chart"};
  s.enabled = Ready() && !preview_ && !ChartSheet() && !Current().PartName().empty();
  if (cmd == CmdInsertChart) {
    s.label = L"Insert chart";
    s.shortcut = L"Alt+F1";
  } else if (cmd == CmdChartBox) {
    s.label = L"Chart";
  } else if (cmd == CmdDeleteObject) {
    s.label = L"Delete object";
    s.enabled = s.enabled && SelectedObject().has_value();
  } else {
    s.label = kNames[cmd - CmdChartColumn];
  }
  return true;
}

bool WorkbookView::ChartsCommand(int cmd) {
  if (cmd < CmdInsertChart || cmd > CmdDeleteObject) return false;
  if (!Ready() || preview_ || ChartSheet()) return true;
  if (cmd == CmdDeleteObject) {
    if (SelectedObject() && !EditRefused(Ed().DeleteObjects(static_cast<int>(sheet_), {object_}))) object_ = 0;
    host_.ViewInvalidate();
    return true;
  }
  if (editor_.Active() && !CommitEdit(false)) return true;
  edit::Editor::NewChart c;
  c.kind = cmd >= CmdChartColumn && cmd <= CmdChartScatter ? static_cast<edit::Editor::ChartKind>(cmd - CmdChartColumn)
                                                           : edit::Editor::ChartKind::Column;
  // The selected cells, or the data around the active cell, as Excel takes them.
  const Range sel = Selected();
  c.data = sel.firstRow == sel.lastRow && sel.firstCol == sel.lastCol ? data::CurrentRegion(Current(), active_) : sel;
  // In the middle of the window, as Excel puts a new chart (Excel's 5 by 3 inches: 480 by 288
  // pixels; written as 8 columns by 15 rows from there).
  const std::vector<Pane> panes = Panes();
  if (!panes.empty()) {
    const Pane& p = panes.back();
    const double k = K();
    const double x = p.sx + (p.rc.right - p.rc.left) / k / 2 - 240, y = p.sy + (p.rc.bottom - p.rc.top) / k / 2 - 144;
    c.at.col = std::max(p.col0, grid_.ColAt(static_cast<int64_t>(std::max(x, p.sx))));
    c.at.row = std::max(p.row0, grid_.RowAt(static_cast<int64_t>(std::max(y, p.sy))));
  }
  if (EditRefused(Ed().InsertChart(static_cast<int>(sheet_), c))) return true;
  // The new chart, selected (it's the drawing's last).
  if (const SheetObjects* objects = Objects()) {
    const std::vector<SheetObjects::Object> all = objects->Objects();
    if (!all.empty()) {
      object_ = all.back().id;
      objectSheet_ = sheet_;
    }
  }
  host_.ViewInvalidate();
  host_.ViewStateChanged();
  return true;
}

void WorkbookView::AddChartTools(ToolbarModel& m) {
  ToolItem chart;
  chart.kind = ToolKind::Dropdown;
  chart.cmd = CmdChartBox;
  chart.width = 64;
  chart.priority = 3;
  chart.text = L"Chart";
  chart.tooltip = L"Insert a chart of the selected cells";
  chart.choices = {L"Column (Alt+F1)", L"Bar", L"Line", L"Pie", L"Doughnut", L"Area", L"Scatter"};
  chart.onChange = [this](const ToolValue& v) {
    if (v.index >= 0 && v.index < 7) ChartsCommand(CmdChartColumn + v.index);
  };
  m.left.push_back(chart);
}

}  // namespace fw::sheet::view
