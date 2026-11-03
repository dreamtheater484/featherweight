#include "PagedView.h"

#include <algorithm>
#include <cmath>

#include "Commands.h"
#include "core/Strings.h"
#include "ui/Menu.h"
#include "ui/Theme.h"

using namespace gfx;

namespace {

constexpr double kWheelNotchDip = 100.0;
constexpr double kArrowStepDip = 48.0;
constexpr float kScrollTauMs = 70.0f;

bool Empty(const D2D1_RECT_F& r) { return r.right <= r.left || r.bottom <= r.top; }

}  // namespace

// ---------------------------------------------------------------------------
// Layout and coordinates

void PagedView::SetViewport(const RECT& rc, float ui) {
  bool sizeChanged = (rc.right - rc.left) != VpW() || (rc.bottom - rc.top) != VpH() || ui != ui_;
  Anchor a{};
  bool haveAnchor = Ready() && VpW() > 0 && VpH() > 0;
  if (haveAnchor) a = CaptureAnchor(0, 0);
  vp_ = rc;
  ui_ = ui;
  dpi_ = 96.0 * ui;
  if (!presentation_) {
    margin_ = static_cast<int>(std::lround(16 * ui));
    gap_ = static_cast<int>(std::lround(12 * ui));
  }
  if (!Ready() || !sizeChanged) return;
  RecomputeScale();
  Relayout(haveAnchor ? &a : nullptr);
}

int PagedView::PageCount() const {
  const auto* pages = Pages();
  return pages ? static_cast<int>(pages->size()) : 0;
}

void PagedView::RecomputeScale() {
  const auto* pages = Pages();
  if (!pages || zoomMode_ == fw::ZoomMode::Custom) return;
  fw::FitInput f;
  f.pages = pages;
  f.rotation = rotation_;
  f.mode = viewMode_;
  f.coverAlone = CoverAlone();
  f.refPage = currentPage_;
  f.viewportW = VpW();
  f.viewportH = VpH();
  f.margin = margin_;
  f.gap = gap_;
  f.dpi = dpi_;
  double s = fw::ComputeFitScale(zoomMode_, f);
  if (s > 0) scale_ = s;
}

void PagedView::Relayout(const Anchor* anchor) {
  const auto* pages = Pages();
  if (!pages) return;
  fw::LayoutInput in;
  in.pages = *pages;
  in.rotation = rotation_;
  in.mode = viewMode_;
  in.coverAlone = CoverAlone();
  in.scale = scale_;
  in.viewportW = VpW();
  in.margin = margin_;
  in.gap = gap_;
  layout_ = fw::ComputeLayout(in);
  if (viewMode_ == fw::ViewMode::Single && !layout_.rows.empty())
    singleRow_ = std::clamp(singleRow_, 0, static_cast<int>(layout_.rows.size()) - 1);
  if (anchor) RestoreAnchor(*anchor);
  ClampScroll();
  tsx_ = sx_;
  tsy_ = sy_;
  UpdateCurrentPage();
  host_.ViewInvalidate();
}

int PagedView::DocHeight() const {
  if (viewMode_ == fw::ViewMode::Single) return layout_.RowDocHeight(singleRow_, margin_);
  return layout_.height;
}

int PagedView::CenterOffsetY() const { return std::max(0, (VpH() - DocHeight()) / 2); }

void PagedView::ClampScroll() {
  double maxX = std::max(0, layout_.width - VpW());
  double maxY = std::max(0, DocHeight() - VpH());
  sx_ = std::clamp(sx_, 0.0, maxX);
  sy_ = std::clamp(sy_, 0.0, maxY);
  tsx_ = std::clamp(tsx_, 0.0, maxX);
  tsy_ = std::clamp(tsy_, 0.0, maxY);
}

PagedView::Anchor PagedView::CaptureAnchor(double vx, double vy) const {
  Anchor a;
  a.vx = vx;
  a.vy = vy;
  if (layout_.boxes.empty()) return a;
  double docX = sx_ + vx, docY = sy_ - CenterOffsetY() + vy;
  int page = 0;
  if (viewMode_ == fw::ViewMode::Single) {
    page = layout_.rows[singleRow_].first;
  } else {
    int first, last;
    layout_.VisibleRows(static_cast<int>(docY), static_cast<int>(docY) + 1, first, last);
    int row = std::clamp(first, 0, static_cast<int>(layout_.rows.size()) - 1);
    page = layout_.rows[row].first;
  }
  const auto& row = layout_.rows[layout_.rowOfPage[page]];
  for (int k = 0; k < row.count; ++k) {
    const auto& b = layout_.boxes[row.first + k];
    if (docX >= b.x && docX < b.Right()) page = row.first + k;
  }
  const auto& b = layout_.boxes[page];
  a.page = page;
  a.fx = (docX - b.x) / std::max(1, b.w);
  a.fy = (docY - b.y) / std::max(1, b.h);
  return a;
}

void PagedView::RestoreAnchor(const Anchor& a) {
  if (a.page < 0 || a.page >= static_cast<int>(layout_.boxes.size())) return;
  if (viewMode_ == fw::ViewMode::Single) singleRow_ = layout_.rowOfPage[a.page];
  const auto& b = layout_.boxes[a.page];
  sx_ = b.x + a.fx * b.w - a.vx;
  sy_ = b.y + a.fy * b.h - a.vy + CenterOffsetY();
  ClampScroll();
}

void PagedView::VisiblePages(int& first, int& last) const {
  first = 0;
  last = -1;
  if (layout_.rows.empty()) return;
  if (viewMode_ == fw::ViewMode::Single) {
    const auto& r = layout_.rows[singleRow_];
    first = r.first;
    last = r.first + r.count - 1;
    return;
  }
  int top = static_cast<int>(std::floor(sy_)) - CenterOffsetY();
  int fr, lr;
  layout_.VisibleRows(top, top + VpH(), fr, lr);
  if (lr < fr) return;
  first = layout_.rows[fr].first;
  last = layout_.rows[lr].first + layout_.rows[lr].count - 1;
}

RECT PagedView::PageRect(int page) const {
  const auto& b = layout_.boxes[page];
  int ox = vp_.left - static_cast<int>(std::lround(sx_));
  int oy = vp_.top - static_cast<int>(std::lround(sy_)) + CenterOffsetY();
  return RECT{ox + b.x, oy + b.y, ox + b.Right(), oy + b.Bottom()};
}

int PagedView::PageAtPoint(int x, int y) const {
  int first, last;
  VisiblePages(first, last);
  for (int i = first; i <= last; ++i) {
    RECT r = PageRect(i);
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
  }
  return -1;
}

int PagedView::NearestPage(int x, int y) const {
  int first, last;
  VisiblePages(first, last);
  int best = -1;
  long long bestD = LLONG_MAX;
  for (int i = first; i <= last; ++i) {
    RECT r = PageRect(i);
    long long dx = x < r.left ? r.left - x : x > r.right ? x - r.right : 0;
    long long dy = y < r.top ? r.top - y : y > r.bottom ? y - r.bottom : 0;
    long long d = dx * dx + dy * dy;
    if (d < bestD) {
      bestD = d;
      best = i;
    }
  }
  return best;
}

void PagedView::UpdateCurrentPage() {
  if (!Ready() || layout_.boxes.empty()) return;
  int first, last;
  VisiblePages(first, last);
  int best = currentPage_;
  long long bestArea = -1;
  for (int i = first; i <= last; ++i) {
    RECT r = PageRect(i);
    long long w = std::max(0L, std::min(r.right, vp_.right) - std::max(r.left, vp_.left));
    long long h = std::max(0L, std::min(r.bottom, vp_.bottom) - std::max(r.top, vp_.top));
    if (w * h > bestArea) {
      bestArea = w * h;
      best = i;
    }
  }
  best = std::clamp(best, 0, std::max(0, PageCount() - 1));
  if (best != currentPage_) {
    currentPage_ = best;
    host_.ViewStateChanged();
  }
}

void PagedView::ScrollToDocY(double y, bool smooth) {
  tsy_ = y;
  ClampScroll();
  if (!smooth || !theme::AnimationsEnabled()) {
    sy_ = tsy_;
  } else {
    lastStep_ = anim::Now();
    anim::KeepAlive(lastStep_ + 50);
  }
  UpdateCurrentPage();
  host_.ViewInvalidate();
}

void PagedView::StepScroll() {
  if (sy_ == tsy_ && sx_ == tsx_) return;
  double now = anim::Now();
  double dt = std::clamp(now - lastStep_, 0.0, 100.0);
  lastStep_ = now;
  double k = 1.0 - std::exp(-dt / kScrollTauMs);
  sy_ += (tsy_ - sy_) * k;
  sx_ += (tsx_ - sx_) * k;
  if (std::abs(tsy_ - sy_) < 0.5) sy_ = tsy_;
  if (std::abs(tsx_ - sx_) < 0.5) sx_ = tsx_;
  if (sy_ != tsy_ || sx_ != tsx_) anim::KeepAlive(now + 40);
  UpdateCurrentPage();
}

bool PagedView::Animating() const { return sy_ != tsy_ || sx_ != tsx_; }

void PagedView::ScrollBy(double dx, double dy, bool smooth) {
  if (!Ready()) return;
  if (viewMode_ == fw::ViewMode::Single && dy != 0) {
    double maxY = std::max(0, DocHeight() - VpH());
    // Scrolling past the edge of a page turns the page.
    if (dy > 0 && tsy_ >= maxY - 0.5 && singleRow_ + 1 < static_cast<int>(layout_.rows.size())) {
      ++singleRow_;
      sy_ = tsy_ = 0;
      UpdateCurrentPage();
      host_.ViewInvalidate();
      return;
    }
    if (dy < 0 && tsy_ <= 0.5 && singleRow_ > 0) {
      --singleRow_;
      sy_ = tsy_ = std::max(0, DocHeight() - VpH());
      UpdateCurrentPage();
      host_.ViewInvalidate();
      return;
    }
  }
  tsx_ += dx;
  tsy_ += dy;
  ClampScroll();
  if (!smooth || !theme::AnimationsEnabled()) {
    sx_ = tsx_;
    sy_ = tsy_;
  } else {
    lastStep_ = anim::Now();
    anim::KeepAlive(lastStep_ + 50);
  }
  UpdateCurrentPage();
  host_.ViewInvalidate();
}

void PagedView::GoToPage(int page) {
  if (!Ready() || PageCount() == 0 || layout_.rows.empty()) return;
  page = std::clamp(page, 0, PageCount() - 1);
  int row = layout_.rowOfPage[page];
  if (viewMode_ == fw::ViewMode::Single) {
    singleRow_ = row;
    sy_ = tsy_ = 0;
  } else {
    double y = layout_.rows[row].y - std::min(margin_, gap_ / 2 + 1) - CenterOffsetY();
    if (row == 0) y = 0;
    sy_ = tsy_ = y;
  }
  ClampScroll();
  currentPage_ = page;
  UpdateCurrentPage();
  currentPage_ = page;  // honour the requested page even if a neighbour shows more area
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

void PagedView::NextPage() {
  if (!Ready() || layout_.rows.empty()) return;
  int row = layout_.rowOfPage[currentPage_];
  if (row + 1 < static_cast<int>(layout_.rows.size())) GoToPage(layout_.rows[row + 1].first);
}

void PagedView::PrevPage() {
  if (!Ready() || layout_.rows.empty()) return;
  int row = layout_.rowOfPage[currentPage_];
  if (viewMode_ != fw::ViewMode::Single) {
    // If the current page's top is above the viewport, go to its top first.
    RECT r = PageRect(layout_.rows[row].first);
    if (r.top < vp_.top - 2) {
      GoToPage(layout_.rows[row].first);
      return;
    }
  }
  if (row > 0) GoToPage(layout_.rows[row - 1].first);
}

void PagedView::ScrollScreenRectIntoView(const D2D1_RECT_F& r, bool center) {
  if (Empty(r)) return;
  const float pad = 48 * ui_;
  double dy = 0, dx = 0;
  if (center && (r.top < vp_.top + pad || r.bottom > vp_.bottom - pad)) {
    dy = (r.top + r.bottom) / 2 - (vp_.top + vp_.bottom) / 2.0;
  } else if (r.top < vp_.top + pad) {
    dy = r.top - (vp_.top + pad);
  } else if (r.bottom > vp_.bottom - pad) {
    dy = r.bottom - (vp_.bottom - pad);
  }
  if (r.left < vp_.left + pad) dx = r.left - (vp_.left + pad);
  else if (r.right > vp_.right - pad) dx = r.right - (vp_.right - pad);
  if (dx != 0 || dy != 0) {
    sx_ = tsx_ = sx_ + dx;
    sy_ = tsy_ = sy_ + dy;
    ClampScroll();
    UpdateCurrentPage();
  }
  host_.ViewInvalidate();
}

// ---------------------------------------------------------------------------
// Zoom, view mode, rotation, presenting

double PagedView::Zoom() const { return fw::ScaleToZoom(scale_, dpi_); }

void PagedView::SetZoomMode(fw::ZoomMode mode) {
  if (!Ready()) {
    zoomMode_ = mode;
    return;
  }
  Anchor a = CaptureAnchor(VpW() / 2.0, 0);
  a.fy = std::max(0.0, a.fy);
  zoomMode_ = mode;
  RecomputeScale();
  if (mode == fw::ZoomMode::FitPage || mode == fw::ZoomMode::Automatic) {
    Relayout(nullptr);
    GoToPage(currentPage_);
  } else {
    Relayout(&a);
  }
  host_.ViewStateChanged();
}

void PagedView::SetZoom(double zoom, int anchorX, int anchorY) {
  if (!Ready()) return;
  zoom = fw::ClampZoom(zoom);
  double vx = anchorX == INT_MIN ? VpW() / 2.0 : anchorX - vp_.left;
  double vy = anchorY == INT_MIN ? VpH() / 2.0 : anchorY - vp_.top;
  Anchor a = CaptureAnchor(vx, vy);
  zoomMode_ = fw::ZoomMode::Custom;
  scale_ = fw::ZoomToScale(zoom, dpi_);
  Relayout(&a);
  host_.ViewStateChanged();
}

void PagedView::ZoomStep(bool in, int anchorX, int anchorY) {
  SetZoom(fw::NextZoomStep(Zoom(), in), anchorX, anchorY);
}

void PagedView::SetViewMode(fw::ViewMode mode) {
  if (mode == viewMode_) return;
  int page = currentPage_;
  viewMode_ = mode;
  if (!Ready()) return;
  RecomputeScale();
  Relayout(nullptr);
  GoToPage(page);
  host_.ViewStateChanged();
}

void PagedView::Rotate(bool clockwise) {
  if (!Ready()) return;
  int page = currentPage_;
  rotation_ = (rotation_ + (clockwise ? 1 : 3)) % 4;
  RecomputeScale();
  Relayout(nullptr);
  GoToPage(page);
  host_.ViewStateChanged();
}

void PagedView::SetPresentation(bool on) {
  if (on == presentation_ || !Ready()) return;
  int page = currentPage_;
  presentation_ = on;
  if (on) {
    beforePresentation_ = {viewMode_, zoomMode_, Zoom()};
    viewMode_ = fw::ViewMode::Single;
    zoomMode_ = fw::ZoomMode::FitPage;
    margin_ = gap_ = 0;
    OnPresentationChanged(true);
  } else {
    viewMode_ = beforePresentation_.viewMode;
    zoomMode_ = beforePresentation_.zoomMode;
    margin_ = static_cast<int>(std::lround(16 * ui_));
    gap_ = static_cast<int>(std::lround(12 * ui_));
    if (zoomMode_ == fw::ZoomMode::Custom) scale_ = fw::ZoomToScale(beforePresentation_.zoom, dpi_);
    OnPresentationChanged(false);
  }
  RecomputeScale();
  Relayout(nullptr);
  GoToPage(page);
}

// ---------------------------------------------------------------------------
// Scrollbars

bool PagedView::ScrollbarRects(D2D1_RECT_F& vTrack, D2D1_RECT_F& vThumb, D2D1_RECT_F& hTrack,
                               D2D1_RECT_F& hThumb) const {
  const float track = 14 * ui_;
  const float minThumb = 32 * ui_;
  const int docH = DocHeight(), docW = layout_.width;
  vTrack = vThumb = hTrack = hThumb = {};
  bool any = false;
  if (docH > VpH()) {
    vTrack = RectF(vp_.right - track, static_cast<float>(vp_.top), static_cast<float>(vp_.right),
                   static_cast<float>(vp_.bottom) - (docW > VpW() ? track : 0));
    float len = std::max(minThumb, Height(vTrack) * VpH() / static_cast<float>(docH));
    float pos = static_cast<float>(sy_ / std::max(1, docH - VpH())) * (Height(vTrack) - len);
    vThumb = RectF(vTrack.left, vTrack.top + pos, vTrack.right, vTrack.top + pos + len);
    any = true;
  }
  if (docW > VpW()) {
    hTrack = RectF(static_cast<float>(vp_.left), vp_.bottom - track,
                   static_cast<float>(vp_.right) - (docH > VpH() ? track : 0), static_cast<float>(vp_.bottom));
    float len = std::max(minThumb, Width(hTrack) * VpW() / static_cast<float>(docW));
    float pos = static_cast<float>(sx_ / std::max(1, docW - VpW())) * (Width(hTrack) - len);
    hThumb = RectF(hTrack.left + pos, hTrack.top, hTrack.left + pos + len, hTrack.bottom);
    any = true;
  }
  return any;
}

void PagedView::DrawScrollbars(Canvas& c) {
  const Palette& p = theme::P();
  D2D1_RECT_F vTrack, vThumb, hTrack, hThumb;
  if (!ScrollbarRects(vTrack, vThumb, hTrack, hThumb)) return;
  auto draw = [&](const D2D1_RECT_F& track, const D2D1_RECT_F& thumb, float hover, bool vertical) {
    if (Empty(track)) return;
    float thick = (3.0f + 5.0f * hover) * ui_;
    float inset = 3 * ui_;
    if (hover > 0.01f) c.FillRound(Inflate(track, -1.5f * ui_, -1.5f * ui_), 6 * ui_, Alpha(p.scrollTrack, hover));
    D2D1_RECT_F r = vertical ? RectF(track.right - inset - thick, thumb.top + 2 * ui_, track.right - inset,
                                     thumb.bottom - 2 * ui_)
                             : RectF(thumb.left + 2 * ui_, track.bottom - inset - thick, thumb.right - 2 * ui_,
                                     track.bottom - inset);
    c.FillRound(r, thick / 2, Mix(p.scrollThumb, p.scrollThumbHover, hover));
  };
  draw(vTrack, vThumb, vScrollHover_.V(), true);
  draw(hTrack, hThumb, hScrollHover_.V(), false);
}

bool PagedView::OverScrollbar(int x, int y) const {
  D2D1_RECT_F vTrack, vThumb, hTrack, hThumb;
  ScrollbarRects(vTrack, vThumb, hTrack, hThumb);
  return gfx::Contains(vTrack, (float)x, (float)y) || gfx::Contains(hTrack, (float)x, (float)y);
}

// ---------------------------------------------------------------------------
// Input

void PagedView::BeginPress(int x, int y) {
  downX_ = x;
  downY_ = y;
  // A click stops any smooth scrolling right where it is.
  tsx_ = sx_;
  tsy_ = sy_;
  downSx_ = sx_;
  downSy_ = sy_;
}

bool PagedView::ScrollbarMouseDown(int x, int y, int button) {
  if (presentation_ || button != 0) return false;
  D2D1_RECT_F vTrack, vThumb, hTrack, hThumb;
  ScrollbarRects(vTrack, vThumb, hTrack, hThumb);
  if (gfx::Contains(vTrack, (float)x, (float)y)) {
    if (gfx::Contains(vThumb, (float)x, (float)y)) {
      drag_ = Drag::ThumbV;
      thumbGrab_ = y - vThumb.top;
    } else {
      ScrollBy(0, (y < vThumb.top ? -1 : 1) * VpH() * 0.9, true);
    }
    return true;
  }
  if (gfx::Contains(hTrack, (float)x, (float)y)) {
    if (gfx::Contains(hThumb, (float)x, (float)y)) {
      drag_ = Drag::ThumbH;
      thumbGrab_ = x - hThumb.left;
    } else {
      ScrollBy((x < hThumb.left ? -1 : 1) * VpW() * 0.9, 0, true);
    }
    return true;
  }
  return false;
}

bool PagedView::ScrollDragMove(int x, int y) {
  switch (drag_) {
    case Drag::Pan:
      sx_ = tsx_ = downSx_ - (x - downX_);
      sy_ = tsy_ = downSy_ - (y - downY_);
      ClampScroll();
      UpdateCurrentPage();
      host_.ViewInvalidate();
      return true;
    case Drag::ThumbV: {
      D2D1_RECT_F vTrack, vThumb, hTrack, hThumb;
      ScrollbarRects(vTrack, vThumb, hTrack, hThumb);
      double range = Height(vTrack) - Height(vThumb);
      if (range > 0) sy_ = tsy_ = (y - thumbGrab_ - vTrack.top) / range * (DocHeight() - VpH());
      ClampScroll();
      UpdateCurrentPage();
      host_.ViewInvalidate();
      return true;
    }
    case Drag::ThumbH: {
      D2D1_RECT_F vTrack, vThumb, hTrack, hThumb;
      ScrollbarRects(vTrack, vThumb, hTrack, hThumb);
      double range = Width(hTrack) - Width(hThumb);
      if (range > 0) sx_ = tsx_ = (x - thumbGrab_ - hTrack.left) / range * (layout_.width - VpW());
      ClampScroll();
      host_.ViewInvalidate();
      return true;
    }
    default:
      return false;
  }
}

void PagedView::UpdateScrollbarHover(int x, int y) {
  D2D1_RECT_F vTrack, vThumb, hTrack, hThumb;
  ScrollbarRects(vTrack, vThumb, hTrack, hThumb);
  float vh = gfx::Contains(vTrack, (float)x, (float)y) ? 1.f : 0.f;
  float hh = gfx::Contains(hTrack, (float)x, (float)y) ? 1.f : 0.f;
  if (vScrollHover_.Target() != vh) {
    vScrollHover_.To(vh, 150);
    host_.ViewInvalidate();
  }
  if (hScrollHover_.Target() != hh) {
    hScrollHover_.To(hh, 150);
    host_.ViewInvalidate();
  }
}

void PagedView::ScrollbarMouseLeave() {
  vScrollHover_.To(0, 200);
  hScrollHover_.To(0, 200);
}

void PagedView::OnWheel(int delta, bool horizontal, UINT mods, int x, int y) {
  if (!Ready()) return;
  if ((mods & kModCtrl) && !horizontal) {
    double factor = std::pow(1.0015, delta);  // one notch (120) is about 20 %
    SetZoom(Zoom() * factor, x, y);
    return;
  }
  bool precise = (delta % WHEEL_DELTA) != 0;  // touchpads send small steps
  double px = -delta / static_cast<double>(WHEEL_DELTA) * kWheelNotchDip * ui_;
  if (horizontal) px = -px;
  if (horizontal || (mods & kModShift)) ScrollBy(px, 0, !precise);
  else ScrollBy(0, px, !precise);
}

bool PagedView::NavigationKey(UINT vk, UINT mods) {
  const bool shift = (mods & kModShift) != 0;
  const bool fitsH = layout_.width <= VpW();
  switch (vk) {
    case VK_DOWN: ScrollBy(0, kArrowStepDip * ui_, true); return true;
    case VK_UP: ScrollBy(0, -kArrowStepDip * ui_, true); return true;
    case VK_RIGHT:
      if (presentation_ || fitsH) NextPage();
      else ScrollBy(kArrowStepDip * ui_, 0, true);
      return true;
    case VK_LEFT:
      if (presentation_ || fitsH) PrevPage();
      else ScrollBy(-kArrowStepDip * ui_, 0, true);
      return true;
    case VK_NEXT:
    case VK_PRIOR: {
      bool down = vk == VK_NEXT;
      if (presentation_) {
        down ? NextPage() : PrevPage();
        return true;
      }
      // Whole pages fit: jump page by page. Otherwise scroll a screen at a time.
      int row = layout_.rowOfPage[currentPage_];
      if (layout_.rows[row].h <= VpH() - 2 * margin_ || viewMode_ == fw::ViewMode::Single) {
        if (viewMode_ == fw::ViewMode::Single) {
          double maxY = std::max(0, DocHeight() - VpH());
          if ((down && tsy_ < maxY - 1) || (!down && tsy_ > 1)) {
            ScrollBy(0, (down ? 1 : -1) * VpH() * 0.9, true);
            return true;
          }
        }
        down ? NextPage() : PrevPage();
      } else {
        ScrollBy(0, (down ? 1 : -1) * VpH() * 0.9, true);
      }
      return true;
    }
    case VK_SPACE:
      if (presentation_) {
        shift ? PrevPage() : NextPage();
      } else {
        ScrollBy(0, (shift ? -1 : 1) * VpH() * 0.9, true);
      }
      return true;
    case VK_BACK:
      if (presentation_) {
        PrevPage();
        return true;
      }
      return false;
    case VK_HOME: GoToPage(0); return true;
    case VK_END: GoToPage(PageCount() - 1); return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Commands

CommandState PagedView::QueryCommand(int cmd) {
  CommandState s;
  s.enabled = Ready();
  switch (cmd) {
    case CmdPrevPage: s.enabled = s.enabled && currentPage_ > 0; break;
    case CmdNextPage: s.enabled = s.enabled && currentPage_ + 1 < PageCount(); break;
    case CmdZoomAuto: s.checked = zoomMode_ == fw::ZoomMode::Automatic; break;
    case CmdZoomFitWidth: s.checked = zoomMode_ == fw::ZoomMode::FitWidth; break;
    case CmdZoomFitPage: s.checked = zoomMode_ == fw::ZoomMode::FitPage; break;
    case CmdZoomActual: s.checked = zoomMode_ == fw::ZoomMode::ActualSize; break;
    case CmdViewSingle: s.checked = viewMode_ == fw::ViewMode::Single; break;
    case CmdViewContinuous: s.checked = viewMode_ == fw::ViewMode::Continuous; break;
    case CmdViewFacing: s.checked = viewMode_ == fw::ViewMode::Facing; break;
    case CmdToggleCover:
      s.enabled = s.enabled && viewMode_ == fw::ViewMode::Facing;
      s.checked = CoverAlone();
      break;
    default:
      if (cmd >= CmdZoomPreset && cmd < CmdZoomPreset + 50) {
        const auto& presets = fw::ZoomPresets();
        int i = cmd - CmdZoomPreset;
        s.checked = i < static_cast<int>(presets.size()) && zoomMode_ == fw::ZoomMode::Custom &&
                    std::abs(Zoom() - presets[i]) < 0.005;
      }
      break;
  }
  return s;
}

bool PagedView::Execute(int cmd) {
  if (cmd >= CmdZoomPreset && cmd < CmdZoomPreset + 50) {
    const auto& presets = fw::ZoomPresets();
    int i = cmd - CmdZoomPreset;
    if (i < static_cast<int>(presets.size())) SetZoom(presets[i]);
    return true;
  }
  switch (cmd) {
    case CmdNextPage: NextPage(); return true;
    case CmdPrevPage: PrevPage(); return true;
    case CmdFirstPage: GoToPage(0); return true;
    case CmdLastPage: GoToPage(PageCount() - 1); return true;
    case CmdZoomIn: ZoomStep(true); return true;
    case CmdZoomOut: ZoomStep(false); return true;
    case CmdZoomAuto: SetZoomMode(fw::ZoomMode::Automatic); return true;
    case CmdZoomActual: SetZoomMode(fw::ZoomMode::ActualSize); return true;
    case CmdZoomFitPage: SetZoomMode(fw::ZoomMode::FitPage); return true;
    case CmdZoomFitWidth: SetZoomMode(fw::ZoomMode::FitWidth); return true;
    case CmdViewSingle: SetViewMode(fw::ViewMode::Single); return true;
    case CmdViewContinuous: SetViewMode(fw::ViewMode::Continuous); return true;
    case CmdViewFacing: SetViewMode(fw::ViewMode::Facing); return true;
    case CmdRotateLeft: Rotate(false); return true;
    case CmdRotateRight: Rotate(true); return true;
  }
  return false;
}

void PagedView::FillMenu(int which, HMENU m) {
  using menu::AppendItem;
  using menu::AppendSeparator;
  auto checked = [&](int cmd) { return QueryCommand(cmd).checked; };
  if (which == CmdZoomMenu) {
    AppendItem(m, CmdZoomAuto, L"Automatic\tCtrl+0", true, checked(CmdZoomAuto), true);
    AppendItem(m, CmdZoomFitWidth, L"Fit width\tCtrl+3", true, checked(CmdZoomFitWidth), true);
    AppendItem(m, CmdZoomFitPage, L"Fit page\tCtrl+2", true, checked(CmdZoomFitPage), true);
    AppendItem(m, CmdZoomActual, L"Actual size\tCtrl+1", true, checked(CmdZoomActual), true);
    AppendSeparator(m);
    const auto& presets = fw::ZoomPresets();
    for (int i = 0; i < static_cast<int>(presets.size()); ++i)
      AppendItem(m, CmdZoomPreset + i, fw::FormatPercent(presets[i]), true, checked(CmdZoomPreset + i), true);
  } else if (which == CmdViewMenu) {
    AppendItem(m, CmdViewSingle, L"Single page\tCtrl+6", true, checked(CmdViewSingle), true);
    AppendItem(m, CmdViewContinuous, L"Continuous\tCtrl+7", true, checked(CmdViewContinuous), true);
    AppendItem(m, CmdViewFacing, L"Two pages\tCtrl+8", true, checked(CmdViewFacing), true);
    AppendSeparator(m);
    CommandState cover = QueryCommand(CmdToggleCover);
    AppendItem(m, CmdToggleCover, L"Show cover page on its own", cover.enabled, cover.checked);
    AppendSeparator(m);
    AppendItem(m, CmdRotateLeft, L"Rotate counterclockwise\tCtrl+Shift+−");
    AppendItem(m, CmdRotateRight, L"Rotate clockwise\tCtrl+Shift+=");
  }
}

// ---------------------------------------------------------------------------
// Saved state

FileState PagedView::State() const {
  FileState s;
  s.page = currentPage_;
  s.zoomMode = presentation_ ? beforePresentation_.zoomMode : zoomMode_;
  s.zoom = presentation_ ? beforePresentation_.zoom : Zoom();
  s.viewMode = presentation_ ? beforePresentation_.viewMode : viewMode_;
  s.rotation = rotation_;
  return s;
}

void PagedView::ApplyState(const FileState& s) {
  viewMode_ = s.viewMode;
  rotation_ = s.rotation;
  zoomMode_ = s.zoomMode;
  currentPage_ = std::clamp(s.page, 0, std::max(0, PageCount() - 1));
  if (zoomMode_ == fw::ZoomMode::Custom) scale_ = fw::ZoomToScale(s.zoom, dpi_);
  if (!Ready()) return;
  RecomputeScale();
  Relayout(nullptr);
  GoToPage(currentPage_);
}
