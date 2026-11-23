// Charts and the objects in a sheet's drawing in the grid (S3, s3-charts): Insert Chart (Alt+F1 and
// the toolbar), and a chart, picture or shape selected with a click, moved by dragging it, resized
// by its handles (Shift keeps a corner's aspect ratio), nudged with the arrow keys and deleted.
// The edits are the Editor's (MoveObject, DeleteObjects, InsertChart); the objects are read again
// after each (kObjects).

#include <algorithm>
#include <cmath>

#include "Commands.h"
#include "Util.h"
#include "core/Strings.h"
#include "sheet/calc/Typing.h"
#include "sheet/data/Filter.h"
#include "sheet/view/WorkbookFormat.h"
#include "sheet/view/WorkbookView.h"
#include "ui/Controls.h"
#include "ui/SidePanel.h"
#include "ui/Theme.h"
#include "ui/Toolbar.h"

using namespace gfx;

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

bool WorkbookView::ObjectMouseDown(float x, float y, bool doubleClick) {
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
  for (auto it = all.rbegin(); it != all.rend(); ++it) {  // the top one
    draw::Rect r = it->rect;  // a line without width or height: within reach of it
    if (r.Width() < reach) r.left -= reach, r.right += reach;
    if (r.Height() < reach) r.top -= reach, r.bottom += reach;
    if (it->id && Inside(r, *at)) {
      grab(*it, -1);
      if (doubleClick && it->chart) {
        mouse_ = Mouse::None;
        ShowChartPanel();
      }
      return true;
    }
  }
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
    // Shift on a corner keeps the shape's proportions, from the opposite corner; a picture's corner
    // always does (its aspect ratio locked, as Excel inserts it: s3-data's AspectLocked).
    if (((mods & kModShift) || AspectLocked(object_, d.start)) && h % 2 == 0 && d.start.Width() > 0 && d.start.Height() > 0) {
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
  if (ctrl && !alt && vk == '1' && sel->chart) {
    ShowChartPanel();
    return true;
  }
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

// ---- The clipboard: an object copied (in any tab), pasted while the clipboard still holds it

namespace {

struct ObjectCopy {
  edit::Editor::ObjectClip clip;
  draw::Size size;  // points
  unsigned long seq = 0;
};
std::optional<ObjectCopy>& Copied() {
  static std::optional<ObjectCopy> copied;
  return copied;
}

}  // namespace

bool WorkbookView::CopyObject(bool cut) {
  const std::optional<SheetObjects::Object> sel = SelectedObject();
  if (!sel) return false;
  edit::Editor::ObjectClip clip = Ed().CopyObject(static_cast<int>(sheet_), object_);
  if (clip.anchor.empty()) return true;
  util::SetClipboardText(Utf8ToWide(sel->name));
  Copied() = ObjectCopy{std::move(clip), {sel->rect.Width(), sel->rect.Height()}, util::ClipboardSequence()};
  clip_.reset();  // no cells copied any more
  if (cut && !EditRefused(Ed().DeleteObjects(static_cast<int>(sheet_), {object_}))) object_ = 0;
  host_.ViewInvalidate();
  host_.ViewStateChanged();
  return true;
}

bool WorkbookView::PasteObjectClip() {
  const std::optional<ObjectCopy>& c = Copied();
  if (!c || util::ClipboardSequence() != c->seq) return false;
  // At the active cell, at its size.
  const float x = static_cast<float>(grid_.ColLeft(active_.col) * kPtPerPx), y = static_cast<float>(grid_.RowTop(active_.row) * kPtPerPx);
  if (EditRefused(Ed().PasteObject(static_cast<int>(sheet_), c->clip, PlaceFor({x, y, x + c->size.width, y + c->size.height}))))
    return true;
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

// ---- Commands

bool WorkbookView::ChartsCommandState(int cmd, CommandState& s) {
  if (cmd < CmdInsertChart || cmd > CmdChartCombo) return false;
  static const wchar_t* const kNames[] = {L"Column chart",   L"Bar chart",  L"Line chart",    L"Pie chart",
                                          L"Doughnut chart", L"Area chart", L"Scatter chart", L"Combo chart"};
  s.enabled = Ready() && !preview_ && !ChartSheet() && (Current().Kind() == SheetKind::Worksheet || !Current().PartName().empty());
  if (cmd == CmdMoveChart && ChartSheet()) {  // Move Chart > Object in
    s.label = L"Move chart onto a sheet";
    s.enabled = Ready() && !preview_ && FirstWorksheet() >= 0;
    return true;
  }
  if (cmd == CmdInsertChart) {
    s.label = L"Insert chart";
    s.shortcut = L"Alt+F1";
  } else if (cmd == CmdChartBox) {
    s.label = L"Chart";
  } else if (cmd == CmdDeleteObject) {
    s.label = L"Delete object";
    s.enabled = s.enabled && SelectedObject().has_value();
  } else if (cmd == CmdChartPanel || cmd == CmdMoveChart) {
    s.label = cmd == CmdChartPanel ? L"Format chart" : L"Move chart to a new sheet";
    if (cmd == CmdChartPanel) s.shortcut = L"Ctrl+1";
    const std::optional<SheetObjects::Object> sel = SelectedObject();
    s.enabled = s.enabled && sel && sel->chart;
  } else {
    s.label = kNames[cmd == CmdChartCombo ? 7 : cmd - CmdChartColumn];
  }
  return true;
}

// The first worksheet (a chart sheet's chart goes back onto it); -1 when there's none.
int WorkbookView::FirstWorksheet() const {
  for (size_t i = 0; i < book_->SheetCount(); ++i)
    if (book_->GetSheet(i).Kind() == SheetKind::Worksheet && book_->GetSheet(i).State() == SheetState::Visible)
      return static_cast<int>(i);
  return -1;
}

// Move Chart > New sheet: the chart on a chart sheet before this one, which shows (Excel's).
void WorkbookView::MoveChartToSheet(uint32_t id) {
  const size_t at = sheet_;
  if (EditRefused(Ed().MoveChartToSheet(static_cast<int>(at), id))) return;
  object_ = 0;
  host_.ViewShowPanel(nullptr);
  ShowSheet(at);
}

bool WorkbookView::ChartsCommand(int cmd) {
  if (cmd < CmdInsertChart || cmd > CmdChartCombo) return false;
  if (cmd == CmdMoveChart && ChartSheet() && Ready() && !preview_) {
    // Onto the first worksheet, at B2 (Excel's dialog defaults to the first sheet), which shows.
    const int to = FirstWorksheet();
    if (to < 0) return true;
    const size_t from = sheet_;
    if (EditRefused(Ed().MoveChartToObject(static_cast<int>(from), to, CellAddr{1, 1}))) return true;
    ShowSheet(static_cast<size_t>(to > static_cast<int>(from) ? to - 1 : to));
    return true;
  }
  if (!Ready() || preview_ || ChartSheet()) return true;
  if (cmd == CmdChartPanel) {
    ShowChartPanel();
    return true;
  }
  if (cmd == CmdMoveChart) {
    const std::optional<SheetObjects::Object> sel = SelectedObject();
    if (sel && sel->chart) MoveChartToSheet(sel->id);
    return true;
  }
  if (cmd == CmdDeleteObject) {
    if (SelectedObject() && !EditRefused(Ed().DeleteObjects(static_cast<int>(sheet_), {object_}))) object_ = 0;
    host_.ViewInvalidate();
    return true;
  }
  if (editor_.Active() && !CommitEdit(false)) return true;
  edit::Editor::NewChart c;
  c.kind = cmd >= CmdChartColumn && cmd <= CmdChartScatter ? static_cast<edit::Editor::ChartKind>(cmd - CmdChartColumn)
            : cmd == CmdChartCombo                          ? edit::Editor::ChartKind::Combo
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
      ribbonInserted_ = object_;  // the ribbon shows Chart Design (R1)
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
  chart.text = chart.menuLabel = L"Chart";
  chart.fitText = true;
  chart.tooltip = L"Insert a chart of the selected cells";
  chart.choices = {L"Column (Alt+F1)", L"Bar", L"Line", L"Pie", L"Doughnut", L"Area", L"Scatter", L"Combo"};
  chart.onChange = [this](const ToolValue& v) {
    if (v.index >= 0 && v.index < 7) ChartsCommand(CmdChartColumn + v.index);
    if (v.index == 7) ChartsCommand(CmdChartCombo);
  };
  m.left.push_back(chart);
}

// ---- The chart panel (double-click a chart, Ctrl+1 on it): its type, colors, titles, legend,
// data labels and data (Select Data). Lists and switches apply at once; typed text with Apply.

namespace {

const wchar_t* const kKinds[] = {L"Column", L"Bar", L"Line", L"Pie", L"Doughnut", L"Area", L"Scatter", L"Combo"};
const wchar_t* const kLegends[] = {L"None", L"Bottom", L"Right", L"Top", L"Left"};
constexpr char kLegendPos[] = {0, 'b', 'r', 't', 'l'};
const wchar_t* const kPalettes[] = {L"Colorful", L"Blue", L"Orange", L"Gray", L"Green"};
// Office's theme colors: its accents, then each accent's shades from dark to light.
constexpr uint32_t kPaletteRgb[][6] = {{0x4472C4, 0xED7D31, 0xA5A5A5, 0xFFC000, 0x5B9BD5, 0x70AD47},
                                       {0x203864, 0x2F5597, 0x4472C4, 0x8FAADC, 0xB4C7E7, 0xDAE3F3},
                                       {0x843C0C, 0xC55A11, 0xED7D31, 0xF4B183, 0xF8CBAD, 0xFBE5D6},
                                       {0x3B3838, 0x595959, 0x7F7F7F, 0xA5A5A5, 0xBFBFBF, 0xD9D9D9},
                                       {0x385723, 0x548235, 0x70AD47, 0xA9D18E, 0xC5E0B4, 0xE2F0D9}};

template <size_t N>
std::vector<std::wstring> Names(const wchar_t* const (&names)[N]) {
  return std::vector<std::wstring>(names, names + N);
}

std::wstring AsTyped(const std::string& f) { return f.empty() ? std::wstring() : (f[0] == '=' ? L"" : L"=") + Utf8ToWide(f); }

}  // namespace

class WorkbookView::ChartPanel : public PanelContent {
 public:
  explicit ChartPanel(WorkbookView& view) : v_(view) {
    kind_.items = Names(kKinds);
    kind_.onChange = [this](int i) {
      edit::ChartEdit e;
      e.kind = static_cast<edit::ChartKind>(i);
      Edit(e);
    };
    palette_.items = Names(kPalettes);
    palette_.onChange = [this](int i) { Palette(i); };
    legend_.items = Names(kLegends);
    legend_.onChange = [this](int i) {
      edit::ChartEdit e;
      e.legend = kLegendPos[std::clamp(i, 0, 4)];
      Edit(e);
    };
    series_.onChange = [this](int i) {
      Keep();
      at_ = i;
      Fill();
    };
    color_.onChange = [this](D2D1_COLOR_F c, bool automatic) {
      if (automatic || at_ < 0) return;
      auto b = [](float x) { return static_cast<uint32_t>(std::lround(std::clamp(x, 0.0f, 1.0f) * 255)); };
      edit::ChartEdit e;
      e.colors = {{static_cast<uint32_t>(at_), -1, b(c.r) << 16 | b(c.g) << 8 | b(c.b)}};
      Edit(e);
    };
    title_.text = L"Chart title";
    axes_.text = L"Axis titles";
    labels_.text = L"Data labels";
    for (PanelButton* b : {&title_, &axes_, &labels_}) b->toggle = true;
    title_.onInvoke = [this] {
      edit::ChartEdit e;
      e.title = edit::ChartText{title_.checked, WideToUtf8(titleText_.Text())};
      Edit(e);
    };
    axes_.onInvoke = [this] {
      edit::ChartEdit e;
      e.xTitle = edit::ChartText{axes_.checked, WideToUtf8(xText_.Text())};
      e.yTitle = edit::ChartText{axes_.checked, WideToUtf8(yText_.Text())};
      Edit(e);
    };
    labels_.onInvoke = [this] {
      edit::ChartEdit e;
      e.labels = labels_.checked;
      Edit(e);
    };
    for (ComboBox* b : {&titleText_, &xText_, &yText_, &range_, &name_, &values_, &cats_}) b->autoComplete = false;
    use_.text = L"Use range";
    use_.onInvoke = [this] { UseRange(); };
    add_.text = L"Add";
    add_.onInvoke = [this] {
      Keep();
      list_.push_back({});
      at_ = static_cast<int>(list_.size()) - 1;
      Fill();
    };
    remove_.text = L"Remove";
    remove_.onInvoke = [this] {
      if (at_ < 0 || list_.size() < 2) return;
      list_.erase(list_.begin() + at_);
      at_ = std::min(at_, static_cast<int>(list_.size()) - 1);
      Fill();
    };
    up_.text = L"Up";
    up_.onInvoke = [this] { Swap(-1); };
    down_.text = L"Down";
    down_.onInvoke = [this] { Swap(1); };
    apply_.text = L"Apply";
    apply_.onInvoke = [this] { Apply(); };
    delete_.text = L"Delete chart";
    delete_.onInvoke = [this] {
      if (!v_.EditRefused(v_.Ed().DeleteObjects(static_cast<int>(sheet_), {id_}))) v_.host_.ViewShowPanel(nullptr);
    };
    move_.text = L"Move to a new sheet";
    move_.onInvoke = [this] { v_.MoveChartToSheet(id_); };
    for (PanelButton* b : {&use_, &add_, &remove_, &up_, &down_, &delete_, &move_}) b->textSize = 12;
  }

  std::wstring Title() const override { return L"Chart"; }

  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override {
    body_ = body;
    ui_ = ui;
    Place();
    return {&kind_, &palette_, &title_, &titleText_, &axes_, &xText_, &yText_, &legend_, &labels_, &range_, &use_,
            &series_, &color_, &name_, &values_, &cats_, &add_, &remove_, &up_, &down_, &apply_, &delete_, &move_};
  }

  void Draw(Canvas& c) override {
    for (const auto& [text, rc] : labelsText_) c.Text(text, rc, theme::P().textSecondary, 12 * ui_);
  }

  // The selected chart's settings in the controls.
  bool Open(size_t sheet, uint32_t id) {
    sheet_ = sheet;
    id_ = id;
    const std::string part = v_.Ed().ChartPart(static_cast<int>(sheet), id);
    if (part.empty()) return false;
    info_ = edit::charts::Info(v_.Ed().PartBytes(part));
    kind_.selected = info_.kind ? static_cast<int>(*info_.kind) : -1;
    title_.checked = info_.title.shown;
    titleText_.SetText(Utf8ToWide(info_.title.text));
    axes_.checked = info_.xTitle.shown || info_.yTitle.shown;
    xText_.SetText(Utf8ToWide(info_.xTitle.text));
    yText_.SetText(Utf8ToWide(info_.yTitle.text));
    legend_.selected = static_cast<int>(std::find(std::begin(kLegendPos), std::end(kLegendPos), info_.legend) - std::begin(kLegendPos));
    labels_.checked = info_.labels;
    palette_.selected = -1;
    list_ = info_.series;
    at_ = list_.empty() ? -1 : std::clamp(at_, 0, static_cast<int>(list_.size()) - 1);
    Fill();
    return true;
  }

 private:
  void Place() {
    const float u = ui_, left = body_.left + 16 * u, right = body_.right - 16 * u, gap = 8 * u;
    const float half = (right - left - gap) / 2, quarter = (right - left - 3 * gap) / 4;
    float y = body_.top + 10 * u;
    labelsText_.clear();
    auto label = [&](const wchar_t* text, float x0, float x1) { labelsText_.push_back({text, RectF(x0, y, x1, y + 16 * u)}); };
    label(L"Chart type", left, left + half);
    label(L"Colors", left + half + gap, right);
    y += 20 * u;
    kind_.rc = RectF(left, y, left + half, y + 30 * u);
    palette_.rc = RectF(left + half + gap, y, right, y + 30 * u);
    y += 40 * u;
    title_.rc = RectF(left, y, left + half, y + 26 * u);
    titleText_.rc = RectF(left + half + gap, y - 2 * u, right, y + 28 * u);
    y += 36 * u;
    axes_.rc = RectF(left, y, right, y + 26 * u);
    y += 32 * u;
    label(L"Horizontal", left, left + half);
    label(L"Vertical", left + half + gap, right);
    y += 20 * u;
    xText_.rc = RectF(left, y, left + half, y + 30 * u);
    yText_.rc = RectF(left + half + gap, y, right, y + 30 * u);
    y += 40 * u;
    label(L"Legend", left, left + half);
    y += 20 * u;
    legend_.rc = RectF(left, y, left + half, y + 30 * u);
    labels_.rc = RectF(left + half + gap, y + 2 * u, right, y + 28 * u);
    y += 44 * u;
    label(L"Chart data range", left, right);
    y += 20 * u;
    range_.rc = RectF(left, y, right - 100 * u, y + 30 * u);
    use_.rc = RectF(right - 92 * u, y, right, y + 30 * u);
    y += 40 * u;
    label(L"Series", left, right);
    y += 20 * u;
    series_.rc = RectF(left, y, right - 48 * u, y + 30 * u);
    color_.rc = RectF(right - 40 * u, y, right, y + 30 * u);
    y += 38 * u;
    label(L"Name", left, right);
    y += 20 * u;
    name_.rc = RectF(left, y, right, y + 30 * u);
    y += 36 * u;
    label(L"Values", left, right);
    y += 20 * u;
    values_.rc = RectF(left, y, right, y + 30 * u);
    y += 36 * u;
    label(L"Categories (horizontal axis labels)", left, right);
    y += 20 * u;
    cats_.rc = RectF(left, y, right, y + 30 * u);
    y += 38 * u;
    add_.rc = RectF(left, y, left + quarter, y + 28 * u);
    remove_.rc = RectF(left + quarter + gap, y, left + 2 * quarter + gap, y + 28 * u);
    up_.rc = RectF(left + 2 * (quarter + gap), y, left + 3 * quarter + 2 * gap, y + 28 * u);
    down_.rc = RectF(left + 3 * (quarter + gap), y, right, y + 28 * u);
    y += 40 * u;
    apply_.rc = RectF(left, y, left + half, y + 30 * u);
    delete_.rc = RectF(left + half + gap, y, right, y + 30 * u);
    y += 38 * u;
    move_.rc = RectF(left, y, right, y + 30 * u);
    v_.host_.ViewInvalidate();
  }

  // The series list's names and the selected one's references in the boxes.
  void Fill() {
    std::vector<std::wstring> names;
    for (size_t i = 0; i < list_.size(); ++i) {
      // Its name as the chart shows it, while it's the one the chart has.
      const bool same = i < info_.series.size() && i < info_.names.size() && info_.series[i].name == list_[i].name;
      std::wstring n = Utf8ToWide(same && !info_.names[i].empty() ? info_.names[i] : list_[i].name);
      names.push_back(L"Series " + std::to_wstring(i + 1) + (n.empty() ? L"" : L": " + n));
    }
    series_.items = names;
    series_.selected = at_;
    const bool any = at_ >= 0 && static_cast<size_t>(at_) < list_.size();
    name_.SetText(any ? AsTyped(list_[static_cast<size_t>(at_)].name) : L"");
    values_.SetText(any ? AsTyped(list_[static_cast<size_t>(at_)].values) : L"");
    cats_.SetText(any ? AsTyped(list_[static_cast<size_t>(at_)].categories) : L"");
    v_.host_.ViewInvalidate();
  }

  // The boxes into the selected series.
  void Keep() {
    if (at_ < 0 || static_cast<size_t>(at_) >= list_.size()) return;
    auto text = [](const ComboBox& b) {
      std::wstring t = Trim(b.Text());
      if (!t.empty() && t[0] == L'=' && !(t.size() > 1 && t[1] == L'"')) t.erase(0, 1);  // a name typed as ="text" stays
      return WideToUtf8(t);
    };
    list_[static_cast<size_t>(at_)] = {text(name_), text(values_), text(cats_)};
  }

  void Swap(int by) {
    Keep();
    const int to = at_ + by;
    if (at_ < 0 || to < 0 || static_cast<size_t>(to) >= list_.size()) return;
    std::swap(list_[static_cast<size_t>(at_)], list_[static_cast<size_t>(to)]);
    at_ = to;
    Fill();
  }

  void Edit(const edit::ChartEdit& e) {
    if (!v_.EditRefused(v_.Ed().EditChart(static_cast<int>(sheet_), id_, e))) Open(sheet_, id_);
  }

  void Palette(int i) {
    if (i < 0 || i >= static_cast<int>(std::size(kPaletteRgb))) return;
    edit::ChartEdit e;
    // A pie colors its slices; other charts their series.
    const bool round = info_.kind == edit::ChartKind::Pie || info_.kind == edit::ChartKind::Doughnut;
    const size_t n = round ? (info_.points.empty() ? 0 : info_.points[0]) : info_.series.size();
    for (size_t k = 0; k < n && k < 255; ++k)
      e.colors.push_back({round ? 0u : static_cast<uint32_t>(k), round ? static_cast<int32_t>(k) : -1, kPaletteRgb[i][k % 6]});
    Edit(e);
  }

  void UseRange() {
    std::wstring t = Trim(range_.Text());
    if (!t.empty() && t[0] == L'=') t.erase(0, 1);
    const std::vector<calc::RefSpan> spans = calc::ReferenceSpans(L"=" + t, v_.Region());
    int sheet = static_cast<int>(sheet_);
    if (spans.size() == 1 && !spans[0].sheet.empty()) sheet = v_.book_->FindSheet(WideToUtf8(spans[0].sheet));
    if (spans.size() != 1 || sheet < 0) {
      v_.host_.ViewMessage(BannerKind::Warning, L"Type the chart's cells as a range, as A1:C5 or Sheet1!A1:C5.");
      return;
    }
    const calc::RefSpan& r = spans[0];
    const Range range{static_cast<uint32_t>(std::min(r.r1, r.r2)), static_cast<uint32_t>(std::min(r.c1, r.c2)),
                      static_cast<uint32_t>(std::max(r.r1, r.r2)), static_cast<uint32_t>(std::max(r.c1, r.c2))};
    edit::ChartEdit e;
    e.series = edit::charts::SeriesFor(v_.book_->GetSheet(static_cast<size_t>(sheet)), range);
    if (e.series->empty()) {
      v_.host_.ViewMessage(BannerKind::Warning, L"Those cells have no numbers to chart.");
      return;
    }
    Edit(e);
  }

  void Apply() {
    Keep();
    edit::ChartEdit e;
    e.title = edit::ChartText{title_.checked, WideToUtf8(titleText_.Text())};
    e.xTitle = edit::ChartText{axes_.checked, WideToUtf8(xText_.Text())};
    e.yTitle = edit::ChartText{axes_.checked, WideToUtf8(yText_.Text())};
    std::vector<edit::ChartSeries> kept;
    for (const edit::ChartSeries& s : list_)
      if (!s.values.empty()) kept.push_back(s);
    if (kept.size() != info_.series.size() ||
        !std::equal(kept.begin(), kept.end(), info_.series.begin(), [](const edit::ChartSeries& a, const edit::ChartSeries& b) {
          return a.name == b.name && a.values == b.values && a.categories == b.categories;
        }))
      e.series = std::move(kept);
    Edit(e);
  }

  WorkbookView& v_;
  size_t sheet_ = 0;
  uint32_t id_ = 0;
  edit::ChartInfo info_;
  std::vector<edit::ChartSeries> list_;
  int at_ = 0;
  D2D1_RECT_F body_{};
  float ui_ = 1;
  Dropdown kind_, palette_, legend_, series_;
  ColorPicker color_;
  ComboBox titleText_, xText_, yText_, range_, name_, values_, cats_;
  PanelButton title_, axes_, labels_, use_, add_, remove_, up_, down_, apply_, delete_, move_;
  std::vector<std::pair<const wchar_t*, D2D1_RECT_F>> labelsText_;
};

void WorkbookView::ShowChartPanel() {
  const std::optional<SheetObjects::Object> sel = SelectedObject();
  if (!sel || !sel->chart) return;
  if (!chartPanel_) chartPanel_ = std::make_unique<ChartPanel>(*this);
  auto* panel = static_cast<ChartPanel*>(chartPanel_.get());
  if (!panel->Open(sheet_, object_)) return;
  host_.ViewShowPanel(panel);
}

}  // namespace fw::sheet::view
