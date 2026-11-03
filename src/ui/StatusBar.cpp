#include "ui/StatusBar.h"

#include <algorithm>
#include <cmath>

#include "core/Strings.h"
#include "ui/Controls.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

using namespace gfx;

namespace {
constexpr float kButtonW = 28;  // DIPs
constexpr float kSliderW = 104;
constexpr float kPercentW = 48;
constexpr float kThumbR = 7;
}  // namespace

StatusBar::StatusBar() {
  zoomOut_.id = 1;
  zoomIn_.id = 2;
  zoomOut_.glyph = icon::Remove;
  zoomOut_.tooltip = L"Zoom out";
  zoomIn_.glyph = icon::Add;
  zoomIn_.tooltip = L"Zoom in";
  zoomOut_.glyphSize = zoomIn_.glyphSize = 12;
  set_.Add(&zoomOut_);
  set_.Add(&zoomIn_);
}

double StatusBar::PositionOf(double zoom) {
  zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
  if (zoom <= 1) return 0.5 * (zoom - kMinZoom) / (1 - kMinZoom);
  return 0.5 + 0.5 * (zoom - 1) / (kMaxZoom - 1);
}

double StatusBar::ZoomAt(double position) {
  position = std::clamp(position, 0.0, 1.0);
  if (position <= 0.5) return kMinZoom + (1 - kMinZoom) * position / 0.5;
  return 1 + (kMaxZoom - 1) * (position - 0.5) / 0.5;
}

void StatusBar::Layout(const RECT& rc, float ui, StatusBarModel m) {
  rc_ = rc;
  ui_ = ui;
  m_ = std::move(m);
  const float u = ui;
  const float cy = std::round((rc.top + rc.bottom) / 2.0f);
  const float h = static_cast<float>(rc.bottom - rc.top);

  // The zoom control, from the right.
  float x = rc.right - 12 * u;
  zoomOut_.visible = zoomIn_.visible = m_.zoom;
  if (m_.zoom) {
    percent_ = RectF(x - kPercentW * u, static_cast<float>(rc.top), x, static_cast<float>(rc.bottom));
    x = percent_.left - 4 * u;
    zoomIn_.rc = RectF(std::round(x - kButtonW * u), cy - 11 * u, std::round(x), cy + 11 * u);
    x = zoomIn_.rc.left - 4 * u;
    slider_ = RectF(std::round(x - kSliderW * u), cy - h / 2 + 3 * u, std::round(x), cy + h / 2 - 3 * u);
    x = slider_.left - 4 * u;
    zoomOut_.rc = RectF(std::round(x - kButtonW * u), cy - 11 * u, std::round(x), cy + 11 * u);
    zoomOut_.enabled = m_.zoomValue > kMinZoom + 1e-6;
    zoomIn_.enabled = m_.zoomValue < kMaxZoom - 1e-6;
  } else {
    percent_ = slider_ = {};
    focused_ = false;
  }

  // The segments, from the left; the dots between them are drawn in the gaps.
  segmentRc_.clear();
  Canvas measure;
  float sx = rc.left + 16 * u;
  for (const std::wstring& s : m_.segments) {
    float w = std::ceil(measure.MeasureText(s, 12 * u));
    segmentRc_.push_back(RectF(sx, static_cast<float>(rc.top), sx + w, static_cast<float>(rc.bottom)));
    sx += w + 24 * u;
  }
}

D2D1_RECT_F StatusBar::Track() const {
  const float u = ui_;
  const float cy = std::round((slider_.top + slider_.bottom) / 2);
  return RectF(slider_.left + kThumbR * u, cy - 1 * u, slider_.right - kThumbR * u, cy + 1 * u);
}

D2D1_RECT_F StatusBar::Thumb() const {
  const D2D1_RECT_F t = Track();
  const float x = t.left + static_cast<float>(PositionOf(m_.zoomValue)) * Width(t);
  const float cy = (t.top + t.bottom) / 2, r = kThumbR * ui_;
  return RectF(x - r, cy - r, x + r, cy + r);
}

void StatusBar::Draw(Canvas& c) {
  const Palette& p = theme::P();
  const float u = ui_;
  const D2D1_RECT_F r = RectF(rc_);
  c.Fill(r, p.chrome);
  c.Fill(RectF(r.left, r.top, r.right, r.top + 1), p.chromeBorder);
  for (size_t i = 0; i < segmentRc_.size(); ++i) {
    c.Text(m_.segments[i], segmentRc_[i], p.textSecondary, 12 * u);
    if (i + 1 < segmentRc_.size()) {
      const float dx = segmentRc_[i].right + 12 * u, dy = (r.top + r.bottom) / 2;
      c.FillEllipse(dx, dy, 1.5f * u, 1.5f * u, Alpha(p.textSecondary, 0.7f));
    }
  }
  if (!m_.zoom) return;

  DrawButton(c, zoomOut_, u);
  DrawButton(c, zoomIn_, u);
  const D2D1_RECT_F track = Track();
  c.FillRound(track, 1 * u, Alpha(p.textSecondary, 0.55f));
  // A tick at 100 %.
  const float mid = std::round((track.left + track.right) / 2);
  c.Fill(RectF(mid - 0.5f * u, track.top - 4 * u, mid + 0.5f * u, track.bottom + 4 * u), Alpha(p.textSecondary, 0.8f));
  const D2D1_RECT_F thumb = Thumb();
  const float cx = (thumb.left + thumb.right) / 2, cy = (thumb.top + thumb.bottom) / 2;
  const float hv = std::max(thumbHover_.V(), dragging_ ? 1.f : 0.f);
  c.FillEllipse(cx, cy, kThumbR * u, kThumbR * u, p.popupBg);
  c.DC()->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), kThumbR * u - 0.5f, kThumbR * u - 0.5f),
                      c.Brush(p.inputBorder), 1);
  const float inner = (4 + 1.2f * hv) * u;
  c.FillEllipse(cx, cy, inner, inner, p.accent);
  if (focused_ && focusVisible_) ctl::DrawFocusRing(c, Inflate(slider_, -1 * u, -1 * u), u);
  c.Text(fw::FormatPercent(m_.zoomValue), percent_, p.textSecondary, 12 * u, DWRITE_FONT_WEIGHT_REGULAR,
         Align::Trailing);
}

bool StatusBar::Hit(int x, int y) const { return Contains(RectF(rc_), (float)x, (float)y); }

void StatusBar::Invalidate() const {
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void StatusBar::SetZoom(double zoom) {
  zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
  if (std::abs(zoom - m_.zoomValue) < 1e-9) return;
  m_.zoomValue = zoom;
  if (m_.onZoom) m_.onZoom(zoom);
  Invalidate();
}

double StatusBar::Step(int direction) const {
  const int pct = static_cast<int>(std::lround(m_.zoomValue * 100));
  const int to = direction > 0 ? (pct / 10 + 1) * 10 : ((pct + 9) / 10 - 1) * 10;
  return to / 100.0;
}

void StatusBar::DragTo(float x) {
  const D2D1_RECT_F t = Track();
  double pos = (x - t.left) / std::max(1.f, Width(t));
  if (std::abs(pos - 0.5) * Width(t) < 4 * ui_) pos = 0.5;  // 100 % holds on to the thumb
  double zoom = ZoomAt(pos);
  SetZoom(std::round(zoom * 100) / 100);
}

bool StatusBar::OnMouseMove(int x, int y) {
  if (dragging_) {
    DragTo((float)x);
    return true;
  }
  if (!m_.zoom) return false;
  bool changed = set_.OnMouseMove((float)x, (float)y);
  const bool over = Contains(Inflate(Thumb(), 3 * ui_, 3 * ui_), (float)x, (float)y);
  if (over != (thumbHover_.Target() > 0.5f)) {
    thumbHover_.To(over ? 1.f : 0.f, 100);
    changed = true;
  }
  return changed;
}

void StatusBar::OnMouseLeave() {
  set_.OnMouseLeave();
  thumbHover_.To(0, 150);
}

void StatusBar::OnMouseDown(int x, int y) {
  if (!m_.zoom) return;
  if (set_.OnMouseDown((float)x, (float)y)) return;
  if (Contains(slider_, (float)x, (float)y)) {
    focusVisible_ = false;
    dragging_ = true;
    DragTo((float)x);
  }
}

void StatusBar::OnMouseUp(int x, int y) {
  if (dragging_) {
    dragging_ = false;
    Invalidate();
    return;
  }
  switch (set_.OnMouseUp((float)x, (float)y)) {
    case 1: SetZoom(Step(-1)); break;
    case 2: SetZoom(Step(1)); break;
    default: break;
  }
}

bool StatusBar::OnWheel(int delta, int x, int y) {
  if (!m_.zoom || !Contains(slider_, (float)x, (float)y)) return false;
  SetZoom(Step(delta > 0 ? 1 : -1));
  return true;
}

std::wstring StatusBar::TooltipAt(int x, int y) const {
  if (!m_.zoom) return {};
  if (zoomOut_.visible && Contains(zoomOut_.rc, (float)x, (float)y)) return zoomOut_.tooltip;
  if (zoomIn_.visible && Contains(zoomIn_.rc, (float)x, (float)y)) return zoomIn_.tooltip;
  if (Contains(slider_, (float)x, (float)y)) return L"Zoom";
  return {};
}

bool StatusBar::FocusIn() {
  if (!m_.zoom) return false;
  focused_ = focusVisible_ = true;
  Invalidate();
  return true;
}

void StatusBar::Blur() {
  if (!focused_) return;
  focused_ = false;
  Invalidate();
}

bool StatusBar::OnKeyDown(UINT vk, bool ctrl, bool) {
  if (!focused_ || ctrl) return false;
  focusVisible_ = true;
  switch (vk) {
    case VK_LEFT:
    case VK_DOWN: SetZoom(Step(-1)); return true;
    case VK_RIGHT:
    case VK_UP: SetZoom(Step(1)); return true;
    case VK_PRIOR: SetZoom(ZoomAt(PositionOf(m_.zoomValue) + 0.1)); return true;
    case VK_NEXT: SetZoom(ZoomAt(PositionOf(m_.zoomValue) - 0.1)); return true;
    case VK_HOME: SetZoom(kMinZoom); return true;
    case VK_END: SetZoom(kMaxZoom); return true;
    default: Invalidate(); return false;
  }
}

void StatusBar::Describe(uia::Tree& t, int parent, const std::function<void()>& focus) {
  int bar = t.Add(parent, 9, UIA_StatusBarControlTypeId, L"Status bar", RectF(rc_));
  for (size_t i = 0; i < segmentRc_.size(); ++i)
    t.Add(bar, 10 + static_cast<int64_t>(i), UIA_TextControlTypeId, m_.segments[i], segmentRc_[i]);
  if (!m_.zoom) return;
  uia::AddButton(t, bar, 1, zoomOut_, [this] { SetZoom(Step(-1)); });
  int s = t.Add(bar, 2, UIA_SliderControlTypeId, L"Zoom", slider_);
  {
    uia::Node& n = t[s];
    n.orientation = OrientationType_Horizontal;
    n.focusable = true;
    n.focused = focused_;
    n.focus = focus;
    n.hasRange = true;
    n.rangeMin = kMinZoom * 100;
    n.rangeMax = kMaxZoom * 100;
    n.rangeValue = std::round(m_.zoomValue * 1000) / 10;
    n.rangeSmall = 10;
    n.rangeLarge = 50;
    n.status = fw::FormatPercent(m_.zoomValue);
    n.setRange = [this](double v) { SetZoom(v / 100); };
  }
  uia::AddButton(t, bar, 3, zoomIn_, [this] { SetZoom(Step(1)); });
  t.Add(bar, 4, UIA_TextControlTypeId, fw::FormatPercent(m_.zoomValue), percent_);
}
