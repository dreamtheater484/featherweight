#include "DocumentView.h"

#include <d2d1effects.h>
#include <fpdf_formfill.h>
#include <fpdf_fwlevent.h>
#include <fpdf_text.h>

#include <algorithm>
#include <cmath>

#include "Anim.h"
#include "Icons.h"
#include "Theme.h"
#include "Util.h"
#include "core/Strings.h"

using namespace gfx;

namespace {

constexpr double kWheelNotchDip = 100.0;
constexpr double kArrowStepDip = 48.0;
constexpr float kScrollTauMs = 70.0f;

D2D1_RECT_F Intersect(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
  return {std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}
bool Empty(const D2D1_RECT_F& r) { return r.right <= r.left || r.bottom <= r.top; }

// Inverts brightness but keeps hues (invert + 180° hue rotation), mapped into a comfortable range.
D2D1_MATRIX_5X4_F InvertMatrix() {
  const float lo = 0x24 / 255.f, hi = 0xE6 / 255.f, k = hi - lo;
  const float H[3][3] = {{-0.574f, 1.430f, 0.144f}, {0.426f, 0.430f, 0.144f}, {0.426f, 1.430f, -0.856f}};
  D2D1_MATRIX_5X4_F m{};
  m._11 = -k * H[0][0]; m._21 = -k * H[0][1]; m._31 = -k * H[0][2]; m._51 = hi;
  m._12 = -k * H[1][0]; m._22 = -k * H[1][1]; m._32 = -k * H[1][2]; m._52 = hi;
  m._13 = -k * H[2][0]; m._23 = -k * H[2][1]; m._33 = -k * H[2][2]; m._53 = hi;
  m._44 = 1.f;
  return m;
}

}  // namespace

DocumentView::DocumentView(ViewHost& host, std::wstring path) : host_(host), path_(std::move(path)) {
  pwBox_.password = true;
  pwBox_.placeholder = L"Password";
  pwBox_.onEnter = [this](bool) { SubmitPassword(); };
  pwOpen_.id = 1;
  pwOpen_.text = L"Open";
  pwOpen_.style = ButtonStyle::Primary;
  pwButtons_.Add(&pwOpen_);
}

DocumentView::~DocumentView() {
  cache_.reset();
  doc_.reset();
}

const std::wstring& DocumentView::Path() const { return doc_ ? doc_->Path() : path_; }

std::wstring DocumentView::Title() const { return fw::FileNameOf(Path()); }

OpenResult DocumentView::Load(const std::wstring& password, std::wstring& error) {
  std::unique_ptr<Document> doc;
  OpenResult r = Document::Open(path_, password, doc, error);
  if (r == OpenResult::NeedPassword || r == OpenResult::WrongPassword) {
    state_ = LoadState::NeedPassword;
    return r;
  }
  if (r != OpenResult::Ok) return r;
  doc_ = std::move(doc);
  cache_ = std::make_unique<RenderCache>(*doc_);
  doc_->AttachHost(this, host_.ViewHighlightFields());
  state_ = LoadState::Ready;
  Relayout(nullptr);
  return r;
}

void DocumentView::SubmitPassword() {
  std::wstring err;
  OpenResult r = Load(pwBox_.Text(), err);
  if (r == OpenResult::Ok) {
    pwBox_.SetText(L"");
    host_.ViewPasswordAccepted(this);
  } else if (r == OpenResult::WrongPassword || r == OpenResult::NeedPassword) {
    pwError_ = L"That password isn’t right. Try again.";
    pwBox_.SelectAll();
    pwBox_.ShakeError();
  } else {
    pwError_ = err;
  }
  host_.ViewInvalidate();
}

// ---------------------------------------------------------------------------
// Layout and coordinates

void DocumentView::SetViewport(const RECT& rc, float ui) {
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

void DocumentView::RecomputeScale() {
  if (!doc_ || zoomMode_ == fw::ZoomMode::Custom) return;
  fw::FitInput f;
  f.pages = &doc_->PageSizes();
  f.rotation = rotation_;
  f.mode = viewMode_;
  f.coverAlone = host_.ViewCoverAlone();
  f.refPage = currentPage_;
  f.viewportW = VpW();
  f.viewportH = VpH();
  f.margin = margin_;
  f.gap = gap_;
  f.dpi = dpi_;
  double s = fw::ComputeFitScale(zoomMode_, f);
  if (s > 0) scale_ = s;
}

void DocumentView::Relayout(const Anchor* anchor) {
  if (!doc_) return;
  fw::LayoutInput in;
  in.pages = doc_->PageSizes();
  in.rotation = rotation_;
  in.mode = viewMode_;
  in.coverAlone = host_.ViewCoverAlone();
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

int DocumentView::DocHeight() const {
  if (viewMode_ == fw::ViewMode::Single) return layout_.RowDocHeight(singleRow_, margin_);
  return layout_.height;
}

int DocumentView::CenterOffsetY() const { return std::max(0, (VpH() - DocHeight()) / 2); }

void DocumentView::ClampScroll() {
  double maxX = std::max(0, layout_.width - VpW());
  double maxY = std::max(0, DocHeight() - VpH());
  sx_ = std::clamp(sx_, 0.0, maxX);
  sy_ = std::clamp(sy_, 0.0, maxY);
  tsx_ = std::clamp(tsx_, 0.0, maxX);
  tsy_ = std::clamp(tsy_, 0.0, maxY);
}

DocumentView::Anchor DocumentView::CaptureAnchor(double vx, double vy) const {
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

void DocumentView::RestoreAnchor(const Anchor& a) {
  if (a.page < 0 || a.page >= static_cast<int>(layout_.boxes.size())) return;
  if (viewMode_ == fw::ViewMode::Single) singleRow_ = layout_.rowOfPage[a.page];
  const auto& b = layout_.boxes[a.page];
  sx_ = b.x + a.fx * b.w - a.vx;
  sy_ = b.y + a.fy * b.h - a.vy + CenterOffsetY();
  ClampScroll();
}

void DocumentView::VisiblePages(int& first, int& last) const {
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

RECT DocumentView::PageRect(int page) const {
  const auto& b = layout_.boxes[page];
  int ox = vp_.left - static_cast<int>(std::lround(sx_));
  int oy = vp_.top - static_cast<int>(std::lround(sy_)) + CenterOffsetY();
  return RECT{ox + b.x, oy + b.y, ox + b.Right(), oy + b.Bottom()};
}

int DocumentView::PageAtPoint(int x, int y) const {
  int first, last;
  VisiblePages(first, last);
  for (int i = first; i <= last; ++i) {
    RECT r = PageRect(i);
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
  }
  return -1;
}

int DocumentView::NearestPage(int x, int y) const {
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

bool DocumentView::ToPagePoint(int page, int x, int y, double& px, double& py) {
  FPDF_PAGE p = doc_->Page(page);
  if (!p) return false;
  RECT r = PageRect(page);
  return FPDF_DeviceToPage(p, r.left, r.top, r.right - r.left, r.bottom - r.top, rotation_, x, y, &px, &py) != 0;
}

D2D1_RECT_F DocumentView::PageRectToScreen(int page, const FS_RECTF& rc) {
  FPDF_PAGE p = doc_->Page(page);
  if (!p) return {};
  RECT r = PageRect(page);
  int x1, y1, x2, y2;
  FPDF_PageToDevice(p, r.left, r.top, r.right - r.left, r.bottom - r.top, rotation_, rc.left, rc.top, &x1, &y1);
  FPDF_PageToDevice(p, r.left, r.top, r.right - r.left, r.bottom - r.top, rotation_, rc.right, rc.bottom, &x2, &y2);
  return RectF(static_cast<float>(std::min(x1, x2)), static_cast<float>(std::min(y1, y2)),
               static_cast<float>(std::max(x1, x2)), static_cast<float>(std::max(y1, y2)));
}

void DocumentView::UpdateCurrentPage() {
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

void DocumentView::ScrollToDocY(double y, bool smooth) {
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

void DocumentView::StepScroll() {
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

bool DocumentView::Animating() const { return sy_ != tsy_ || sx_ != tsx_; }

void DocumentView::ScrollBy(double dx, double dy, bool smooth) {
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

void DocumentView::GoToPage(int page) {
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

void DocumentView::NextPage() {
  if (!Ready() || layout_.rows.empty()) return;
  int row = layout_.rowOfPage[currentPage_];
  if (row + 1 < static_cast<int>(layout_.rows.size())) GoToPage(layout_.rows[row + 1].first);
}

void DocumentView::PrevPage() {
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

void DocumentView::GoToPosition(int page, bool hasPos, float x, float y) {
  if (!Ready() || page < 0 || page >= PageCount()) return;
  GoToPage(page);
  if (!hasPos) return;
  FPDF_PAGE p = doc_->Page(page);
  if (!p) return;
  const auto& b = layout_.boxes[page];
  int dx, dy;
  FPDF_PageToDevice(p, b.x, b.y, b.w, b.h, rotation_, x, y, &dx, &dy);
  if (viewMode_ == fw::ViewMode::Single) {
    sy_ = tsy_ = dy - 24 * ui_;
  } else {
    sy_ = tsy_ = dy - 24 * ui_ - CenterOffsetY();
  }
  ClampScroll();
  UpdateCurrentPage();
  host_.ViewInvalidate();
}

void DocumentView::ScrollPageRectIntoView(int page, const FS_RECTF& rc, bool center) {
  if (!Ready() || page < 0 || page >= PageCount()) return;
  if (viewMode_ == fw::ViewMode::Single && layout_.rowOfPage[page] != singleRow_) {
    singleRow_ = layout_.rowOfPage[page];
    sy_ = tsy_ = 0;
  }
  D2D1_RECT_F r = PageRectToScreen(page, rc);
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
// Zoom, view mode, rotation

double DocumentView::Zoom() const { return fw::ScaleToZoom(scale_, dpi_); }

void DocumentView::SetZoomMode(fw::ZoomMode mode) {
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

void DocumentView::SetZoom(double zoom, int anchorX, int anchorY) {
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

void DocumentView::ZoomStep(bool in, int anchorX, int anchorY) {
  SetZoom(fw::NextZoomStep(Zoom(), in), anchorX, anchorY);
}

void DocumentView::SetViewMode(fw::ViewMode mode) {
  if (mode == viewMode_) return;
  int page = currentPage_;
  viewMode_ = mode;
  if (!Ready()) return;
  RecomputeScale();
  Relayout(nullptr);
  GoToPage(page);
  host_.ViewStateChanged();
}

void DocumentView::Rotate(bool clockwise) {
  if (!Ready()) return;
  int page = currentPage_;
  rotation_ = (rotation_ + (clockwise ? 1 : 3)) % 4;
  RecomputeScale();
  Relayout(nullptr);
  GoToPage(page);
  host_.ViewStateChanged();
}

void DocumentView::SettingsChanged() {
  if (!Ready()) return;
  doc_->SetHighlightFields(host_.ViewHighlightFields());
  cache_->InvalidateAll();
  int page = currentPage_;
  RecomputeScale();
  Relayout(nullptr);
  GoToPage(page);
}

void DocumentView::SetPresentation(bool on) {
  if (on == presentation_ || !Ready()) return;
  int page = currentPage_;
  presentation_ = on;
  if (on) {
    beforePresentation_ = {viewMode_, zoomMode_, Zoom()};
    viewMode_ = fw::ViewMode::Single;
    zoomMode_ = fw::ZoomMode::FitPage;
    margin_ = gap_ = 0;
    ClearSearch();
    selection_.Clear();
  } else {
    viewMode_ = beforePresentation_.viewMode;
    zoomMode_ = beforePresentation_.zoomMode;
    margin_ = static_cast<int>(std::lround(16 * ui_));
    gap_ = static_cast<int>(std::lround(12 * ui_));
    if (zoomMode_ == fw::ZoomMode::Custom) scale_ = fw::ZoomToScale(beforePresentation_.zoom, dpi_);
  }
  RecomputeScale();
  Relayout(nullptr);
  GoToPage(page);
}

// ---------------------------------------------------------------------------
// Painting

void DocumentView::DrawBitmap(ID2D1DeviceContext* dc, ID2D1Bitmap* bmp, const D2D1_RECT_F& dest,
                              const D2D1_RECT_F& src, bool inverted, bool exact) {
  if (!inverted) {
    dc->DrawBitmap(bmp, dest, 1.0f,
                   exact ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                   &src);
    return;
  }
  if (!invertFx_ || fxDc_ != dc) {
    invertFx_.Reset();
    if (FAILED(dc->CreateEffect(CLSID_D2D1ColorMatrix, &invertFx_))) return;
    invertFx_->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, InvertMatrix());
    invertFx_->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE);
    fxDc_ = dc;
  }
  invertFx_->SetInput(0, bmp);
  D2D1_MATRIX_3X2_F old;
  dc->GetTransform(&old);
  float sx = Width(dest) / Width(src), sy = Height(dest) / Height(src);
  dc->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Translation(dest.left, dest.top) * old);
  D2D1_RECT_F srcLocal = src;
  dc->DrawImage(invertFx_.Get(), D2D1::Point2F(0, 0), srcLocal,
                exact ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_INTERPOLATION_MODE_LINEAR);
  dc->SetTransform(old);
}

void DocumentView::Draw(Canvas& c, ID2D1DeviceContext* dc) {
  const Palette& p = theme::P();
  const bool inverted = host_.ViewInverted();
  D2D1_RECT_F vpr = RectF(vp_);
  c.PushClip(vpr);
  D2D1_COLOR_F bg = presentation_ ? Rgb(0x000000) : inverted ? p.docBgInverted : p.docBg;
  c.Fill(vpr, bg);

  if (state_ == LoadState::NeedPassword) {
    DrawPasswordCard(c);
    c.PopClip();
    return;
  }
  if (!Ready()) {
    c.PopClip();
    return;
  }

  StepScroll();
  cache_->RenderDirtyNow(dc);
  cache_->BeginFrame();

  int first, last;
  VisiblePages(first, last);
  for (int i = first; i <= last; ++i) DrawPage(c, dc, i, inverted);

  // Warm up the next pages so scrolling down feels instant.
  if (last >= 0) {
    for (int i = last + 1; i <= std::min(PageCount() - 1, last + 2); ++i) {
      const auto& b = layout_.boxes[i];
      cache_->WantPreview(i, rotation_, {b.w, b.h}, 5000 + i - last);
    }
  }

  DrawOverlays(c, first, last);
  if (!presentation_) DrawScrollbars(c);
  c.PopClip();
}

void DocumentView::DrawPage(Canvas& c, ID2D1DeviceContext* dc, int page, bool inverted) {
  const Palette& p = theme::P();
  RECT r = PageRect(page);
  D2D1_RECT_F rf = RectF(r);
  D2D1_RECT_F vis = Intersect(rf, RectF(vp_));
  if (Empty(vis)) return;

  if (!presentation_) c.SoftShadow(rf, 1.0f * ui_, 5.0f * ui_, Alpha(p.pageShadow, 0.35f), 1.0f * ui_);
  c.Fill(rf, inverted ? p.paperInverted : p.paper);

  const int T = RenderCache::kTile;
  const auto& b = layout_.boxes[page];
  RenderCache::PageGeom geom{b.w, b.h};
  const int scaleKey = RenderCache::ScaleKey(scale_);
  const float cx = (vp_.left + vp_.right) / 2.0f, cy = (vp_.top + vp_.bottom) / 2.0f;

  int tx0 = static_cast<int>(vis.left - rf.left) / T;
  int tx1 = static_cast<int>(vis.right - 1 - rf.left) / T;
  int ty0 = static_cast<int>(vis.top - rf.top) / T;
  int ty1 = static_cast<int>(vis.bottom - 1 - rf.top) / T;

  // Are all visible tiles ready? If not, draw placeholders underneath.
  bool complete = true;
  for (int ty = ty0; ty <= ty1 && complete; ++ty) {
    for (int tx = tx0; tx <= tx1; ++tx) {
      RenderCache::Key k{page, rotation_, scaleKey, ty, tx};
      bool stale = false;
      if (!cache_->Find(k, &stale)) {
        complete = false;
        break;
      }
    }
  }

  if (!complete) {
    int previewKey = 0;
    if (ID2D1Bitmap* pv = cache_->Preview(page, rotation_, &previewKey)) {
      D2D1_SIZE_F s = pv->GetSize();
      DrawBitmap(dc, pv, rf, RectF(0, 0, s.width, s.height), inverted, false);
    } else {
      cache_->WantPreview(page, rotation_, geom, 0);
    }
    c.PushClip(vis);
    cache_->ForEachOtherScale(page, rotation_, scaleKey, [&](const RenderCache::Key& k, ID2D1Bitmap* bmp) {
      double f = scale_ / RenderCache::ScaleOf(k.scaleKey);
      D2D1_SIZE_F s = bmp->GetSize();
      D2D1_RECT_F d = RectF(static_cast<float>(rf.left + k.tx * T * f), static_cast<float>(rf.top + k.ty * T * f),
                            static_cast<float>(rf.left + (k.tx * T + s.width) * f),
                            static_cast<float>(rf.top + (k.ty * T + s.height) * f));
      if (!Empty(Intersect(d, vis))) DrawBitmap(dc, bmp, d, RectF(0, 0, s.width, s.height), inverted, false);
    });
    c.PopClip();
  } else {
    cache_->WantPreview(page, rotation_, geom, 100000);  // cheap, keeps zoom-outs smooth
  }

  for (int ty = ty0; ty <= ty1; ++ty) {
    for (int tx = tx0; tx <= tx1; ++tx) {
      RenderCache::Key k{page, rotation_, scaleKey, ty, tx};
      bool stale = false;
      ID2D1Bitmap* bmp = cache_->Find(k, &stale);
      float x = rf.left + tx * T, y = rf.top + ty * T;
      if (bmp) {
        D2D1_SIZE_F s = bmp->GetSize();
        DrawBitmap(dc, bmp, RectF(x, y, x + s.width, y + s.height), RectF(0, 0, s.width, s.height), inverted, true);
      }
      if (!bmp || stale) {
        float dx = x + T / 2.0f - cx, dy = y + T / 2.0f - cy;
        int prio = 10 + static_cast<int>(std::sqrt(dx * dx + dy * dy) / 64);
        cache_->Want(k, geom, prio);
      }
    }
  }
}

void DocumentView::DrawOverlays(Canvas& c, int first, int last) {
  const Palette& p = theme::P();
  // Search matches.
  if (search_.Active()) {
    auto& matches = search_.Matches();
    auto lo = std::lower_bound(matches.begin(), matches.end(), first,
                               [](const Search::Match& m, int pg) { return m.page < pg; });
    for (auto it = lo; it != matches.end() && it->page <= last; ++it) {
      bool current = static_cast<int>(it - matches.begin()) == search_.Current();
      for (const auto& rc : search_.RectsFor(*doc_, *it)) {
        D2D1_RECT_F r = Inflate(PageRectToScreen(it->page, rc), 1.5f * ui_, 1.0f * ui_);
        c.FillRound(r, 2 * ui_, current ? p.searchCurrent : p.searchHit);
      }
    }
  }
  // Text selection.
  if (!selection_.Empty()) {
    if (selRectsVersion_ != selection_.version) {
      selRects_.clear();
      selRectsVersion_ = selection_.version;
    }
    for (int i = std::max(first, selection_.Start().page); i <= std::min(last, selection_.End().page); ++i) {
      auto it = selRects_.find(i);
      if (it == selRects_.end()) it = selRects_.emplace(i, textsel::Rects(*doc_, selection_, i)).first;
      for (const auto& rc : it->second) c.Fill(PageRectToScreen(i, rc), p.selection);
    }
  }
  // XFA text caret (AcroForm carets are drawn by PDFium itself).
  if (caret_.visible && caret_.page >= first && caret_.page <= last) {
    D2D1_RECT_F r = PageRectToScreen(caret_.page, FS_RECTF{static_cast<float>(caret_.l), static_cast<float>(caret_.t),
                                                           static_cast<float>(caret_.r), static_cast<float>(caret_.b)});
    c.Fill(RectF(r.left, r.top, r.left + std::max(1.0f, ui_), r.bottom), Rgb(0x000000));
  }
}

bool DocumentView::ScrollbarRects(D2D1_RECT_F& vTrack, D2D1_RECT_F& vThumb, D2D1_RECT_F& hTrack,
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

void DocumentView::DrawScrollbars(Canvas& c) {
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

void DocumentView::DrawPasswordCard(Canvas& c) {
  const Palette& p = theme::P();
  const float u = ui_;
  const float w = 360 * u, h = (pwError_.empty() ? 196 : 222) * u;
  float x = std::round((vp_.left + vp_.right) / 2.0f - w / 2), y = std::round((vp_.top + vp_.bottom) / 2.0f - h / 2 - 20 * u);
  D2D1_RECT_F card = RectF(x, y, x + w, y + h);
  c.SoftShadow(card, 8 * u, 16 * u, p.popupShadow, 4 * u);
  c.FillRound(card, 8 * u, p.popupBg);
  c.StrokeRound(card, 8 * u, p.popupBorder, 1);

  float cy = y + 24 * u;
  c.FillEllipse(x + w / 2, cy + 18 * u, 22 * u, 22 * u, Alpha(p.accent, 0.14f));
  c.Icon(icon::Lock, RectF(x, cy, x + w, cy + 36 * u), p.accentText, 18 * u);
  cy += 50 * u;
  std::wstring title = L"“" + Title() + L"” is protected";
  c.Text(title, RectF(x + 20 * u, cy, x + w - 20 * u, cy + 22 * u), p.text, 15 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD,
         Align::Center);
  cy += 24 * u;
  c.Text(L"Enter the password to open it.", RectF(x + 20 * u, cy, x + w - 20 * u, cy + 20 * u), p.textSecondary,
         13 * u, DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
  cy += 32 * u;
  pwBox_.rc = RectF(x + 24 * u, cy, x + w - 24 * u - 88 * u, cy + 32 * u);
  pwOpen_.rc = RectF(pwBox_.rc.right + 8 * u, cy, x + w - 24 * u, cy + 32 * u);
  pwBox_.Draw(c, u, pwBox_.focused && static_cast<int>(anim::Now() / 530) % 2 == 0);
  DrawButton(c, pwOpen_, u);
  if (!pwError_.empty()) {
    cy += 40 * u;
    c.Text(pwError_, RectF(x + 24 * u, cy, x + w - 24 * u, cy + 20 * u), p.error, 12.5f * u);
  }
}

// ---------------------------------------------------------------------------
// Background work

bool DocumentView::HasIdleWork() const {
  if (!Ready()) return false;
  return cache_->HasWork() || (search_.Active() && !search_.Done());
}

bool DocumentView::IdleWork(ID2D1DeviceContext* dc, double deadlineMs) {
  if (!Ready()) return false;
  bool repaint = false;
  if (search_.Active() && !search_.Done()) {
    double searchDeadline = std::min(deadlineMs, anim::Now() + 5);
    if (search_.Work(*doc_, searchDeadline)) repaint = true;
    if (search_.Current() != lastSearchCurrent_) {
      lastSearchCurrent_ = search_.Current();
      if (lastSearchCurrent_ >= 0) ScrollToMatch(lastSearchCurrent_);
    }
    host_.ViewStateChanged();
  }
  if (cache_->HasWork() && cache_->Work(dc, deadlineMs)) repaint = true;

  // Unload pages far away from the viewport.
  int first, last;
  VisiblePages(first, last);
  int keepFirst = std::max(0, first - 3), keepLast = last + 3;
  int active = cache_->ActivePage();
  if (active >= 0 && (active < keepFirst || active > keepLast)) cache_->CancelPage(active);
  int focusPage = -1;
  doc_->FocusedField(focusPage);
  if (search_.Active() && !search_.Done()) keepLast = std::max(keepLast, last + 3);
  doc_->TrimPages(keepFirst, keepLast, focusPage);
  return repaint;
}

void DocumentView::OnDeactivated() {
  if (cache_) cache_->DropTiles();
  drag_ = Drag::None;
}

void DocumentView::ReleaseDeviceResources() {
  invertFx_.Reset();
  fxDc_ = nullptr;
  if (cache_) cache_->ReleaseDeviceResources();
}

// ---------------------------------------------------------------------------
// Input

void DocumentView::OnMouseDown(int x, int y, int button, UINT mods, bool doubleClick) {
  mouseX_ = x;
  mouseY_ = y;
  if (state_ == LoadState::NeedPassword) {
    if (gfx::Contains(pwBox_.rc, (float)x, (float)y)) {
      host_.ViewFocusTextBox(&pwBox_);
      if (doubleClick) pwBox_.OnDoubleClick();
      else pwBox_.OnMouseDown((float)x, (float)y, (mods & kModShift) != 0, ui_);
    } else {
      pwButtons_.OnMouseDown((float)x, (float)y);
    }
    host_.ViewInvalidate();
    return;
  }
  if (!Ready()) return;
  downX_ = x;
  downY_ = y;
  // A click stops any smooth scrolling right where it is.
  tsx_ = sx_;
  tsy_ = sy_;
  downSx_ = sx_;
  downSy_ = sy_;

  if (button == 2) {  // middle button pans
    drag_ = Drag::Pan;
    return;
  }

  if (!presentation_) {
    D2D1_RECT_F vTrack, vThumb, hTrack, hThumb;
    ScrollbarRects(vTrack, vThumb, hTrack, hThumb);
    if (button == 0 && gfx::Contains(vTrack, (float)x, (float)y)) {
      if (gfx::Contains(vThumb, (float)x, (float)y)) {
        drag_ = Drag::ThumbV;
        thumbGrab_ = y - vThumb.top;
      } else {
        ScrollBy(0, (y < vThumb.top ? -1 : 1) * VpH() * 0.9, true);
      }
      return;
    }
    if (button == 0 && gfx::Contains(hTrack, (float)x, (float)y)) {
      if (gfx::Contains(hThumb, (float)x, (float)y)) {
        drag_ = Drag::ThumbH;
        thumbGrab_ = x - hThumb.left;
      } else {
        ScrollBy((x < hThumb.left ? -1 : 1) * VpW() * 0.9, 0, true);
      }
      return;
    }
  }

  int page = PageAtPoint(x, y);
  double px = 0, py = 0;
  bool onPage = page >= 0 && ToPagePoint(page, x, y, px, py);
  FPDF_FORMHANDLE form = doc_->Form();

  if (button == 1) {  // right click: keep the selection for the context menu
    return;
  }

  // Form fields get first pick.
  if (onPage && form && FPDFPage_HasFormFieldAtPoint(form, doc_->Page(page), px, py) >= 0) {
    selection_.Clear();
    drag_ = Drag::Form;
    formPage_ = page;
    if (doubleClick) FORM_OnLButtonDoubleClick(form, doc_->Page(page), mods, px, py);
    else FORM_OnLButtonDown(form, doc_->Page(page), mods, px, py);
    AfterFormEvent();
    return;
  }
  int fp;
  if (form && doc_->FocusedField(fp)) {
    FORM_ForceToKillFocus(form);
    AfterFormEvent();
  }

  if (onPage) {
    LinkTarget link = doc_->LinkAt(page, px, py);
    if (link.kind != LinkTarget::Kind::None) {
      pendingLink_ = link;
      drag_ = Drag::Link;
      return;
    }
  }

  if (presentation_) {
    if (x < (vp_.left + vp_.right) / 2 - VpW() / 4) PrevPage();
    else NextPage();
    return;
  }

  if (onPage) {
    if (FPDF_TEXTPAGE tp = doc_->TextPage(page)) {
      double tol = 3.0 * 72.0 / dpi_ * ui_ * 2;
      int idx = FPDFText_GetCharIndexAtPos(tp, px, py, tol, tol);
      if (idx >= 0) {
        if (doubleClick) {
          int a, b;
          textsel::WordAt(*doc_, page, idx, a, b);
          selection_.Set({page, a}, {page, b});
          drag_ = Drag::None;
        } else if ((mods & kModShift) && !selection_.Empty()) {
          selection_.Set(selection_.anchor, {page, idx});
          drag_ = Drag::Select;
        } else {
          selection_.Set({page, idx}, {page, idx});
          drag_ = Drag::Select;
        }
        host_.ViewInvalidate();
        return;
      }
    }
  }
  if (!selection_.Empty()) {
    selection_.Clear();
    host_.ViewInvalidate();
  }
  drag_ = Drag::Pan;
}

void DocumentView::OnMouseMove(int x, int y, UINT mods) {
  mouseX_ = x;
  mouseY_ = y;
  if (state_ == LoadState::NeedPassword) {
    if (GetKeyState(VK_LBUTTON) < 0 && pwBox_.focused && gfx::Contains(Inflate(pwBox_.rc, 200, 50), (float)x, (float)y))
      pwBox_.OnMouseDrag((float)x, (float)y, ui_);
    if (pwButtons_.OnMouseMove((float)x, (float)y)) host_.ViewInvalidate();
    return;
  }
  if (!Ready()) return;

  switch (drag_) {
    case Drag::Pan:
      sx_ = tsx_ = downSx_ - (x - downX_);
      sy_ = tsy_ = downSy_ - (y - downY_);
      ClampScroll();
      UpdateCurrentPage();
      host_.ViewInvalidate();
      return;
    case Drag::ThumbV: {
      D2D1_RECT_F vTrack, vThumb, hTrack, hThumb;
      ScrollbarRects(vTrack, vThumb, hTrack, hThumb);
      double range = Height(vTrack) - Height(vThumb);
      if (range > 0) sy_ = tsy_ = (y - thumbGrab_ - vTrack.top) / range * (DocHeight() - VpH());
      ClampScroll();
      UpdateCurrentPage();
      host_.ViewInvalidate();
      return;
    }
    case Drag::ThumbH: {
      D2D1_RECT_F vTrack, vThumb, hTrack, hThumb;
      ScrollbarRects(vTrack, vThumb, hTrack, hThumb);
      double range = Width(hTrack) - Width(hThumb);
      if (range > 0) sx_ = tsx_ = (x - thumbGrab_ - hTrack.left) / range * (layout_.width - VpW());
      ClampScroll();
      host_.ViewInvalidate();
      return;
    }
    case Drag::Form: {
      double px, py;
      if (formPage_ >= 0 && ToPagePoint(formPage_, x, y, px, py)) {
        FORM_OnMouseMove(doc_->Form(), doc_->Page(formPage_), mods | FWL_EVENTFLAG_LeftButtonDown, px, py);
        AfterFormEvent();
      }
      return;
    }
    case Drag::Link:
      if (std::abs(x - downX_) + std::abs(y - downY_) > 6 * ui_) drag_ = Drag::Pan;
      return;
    case Drag::Select: {
      // Auto-scroll while selecting beyond the edges.
      if (y < vp_.top) ScrollBy(0, (y - vp_.top) * 0.5, false);
      else if (y > vp_.bottom) ScrollBy(0, (y - vp_.bottom) * 0.5, false);
      int page = PageAtPoint(x, y);
      if (page < 0) page = NearestPage(x, y);
      double px, py;
      if (page >= 0 && ToPagePoint(page, x, y, px, py)) {
        if (FPDF_TEXTPAGE tp = doc_->TextPage(page)) {
          int idx = FPDFText_GetCharIndexAtPos(tp, px, py, 40, 40);
          if (idx >= 0 && !(selection_.focus == TextPos{page, idx})) {
            selection_.Set(selection_.anchor, {page, idx});
            host_.ViewInvalidate();
          }
        }
      }
      return;
    }
    case Drag::None:
      break;
  }

  // Hover feedback.
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
  if (FPDF_FORMHANDLE form = doc_->Form()) {
    int page = PageAtPoint(x, y);
    double px, py;
    if (page >= 0 && ToPagePoint(page, x, y, px, py)) {
      FORM_OnMouseMove(form, doc_->Page(page), mods, px, py);
      AfterFormEvent();
    }
  }
}

void DocumentView::OnMouseUp(int x, int y, int button, UINT mods) {
  if (state_ == LoadState::NeedPassword) {
    if (pwButtons_.OnMouseUp((float)x, (float)y) == 1) SubmitPassword();
    host_.ViewInvalidate();
    return;
  }
  if (!Ready()) return;
  Drag d = drag_;
  drag_ = Drag::None;
  (void)button;
  if (d == Drag::Form && formPage_ >= 0) {
    double px, py;
    if (ToPagePoint(formPage_, x, y, px, py)) FORM_OnLButtonUp(doc_->Form(), doc_->Page(formPage_), mods, px, py);
    AfterFormEvent();
  } else if (d == Drag::Link) {
    FollowLink(pendingLink_);
  } else if (d == Drag::Select) {
    if (selection_.anchor == selection_.focus && std::abs(x - downX_) + std::abs(y - downY_) < 3) {
      selection_.Clear();  // a plain click on text just clears the selection
      host_.ViewInvalidate();
    }
  }
}

void DocumentView::OnMouseLeave() {
  mouseX_ = mouseY_ = -1;
  vScrollHover_.To(0, 200);
  hScrollHover_.To(0, 200);
  if (state_ == LoadState::NeedPassword) pwButtons_.OnMouseLeave();
  host_.ViewInvalidate();
}

void DocumentView::OnWheel(int delta, bool horizontal, UINT mods, int x, int y) {
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

HCURSOR DocumentView::CursorAt(int x, int y) {
  static HCURSOR arrow = LoadCursor(nullptr, IDC_ARROW), ibeam = LoadCursor(nullptr, IDC_IBEAM),
                 hand = LoadCursor(nullptr, IDC_HAND), move = LoadCursor(nullptr, IDC_SIZEALL);
  if (state_ == LoadState::NeedPassword) return gfx::Contains(pwBox_.rc, (float)x, (float)y) ? ibeam : arrow;
  if (!Ready()) return arrow;
  if (drag_ == Drag::Pan) return move;
  if (drag_ == Drag::Select) return ibeam;
  D2D1_RECT_F vTrack, vThumb, hTrack, hThumb;
  ScrollbarRects(vTrack, vThumb, hTrack, hThumb);
  if (gfx::Contains(vTrack, (float)x, (float)y) || gfx::Contains(hTrack, (float)x, (float)y)) return arrow;
  int page = PageAtPoint(x, y);
  double px, py;
  if (page < 0 || !ToPagePoint(page, x, y, px, py)) return arrow;
  if (FPDF_FORMHANDLE form = doc_->Form()) {
    if (FPDFPage_HasFormFieldAtPoint(form, doc_->Page(page), px, py) >= 0) {
      return formCursor_ == FXCT_VBEAM ? ibeam : formCursor_ == FXCT_HAND ? hand : arrow;
    }
  }
  if (doc_->LinkAt(page, px, py).kind != LinkTarget::Kind::None) return hand;
  if (presentation_) return arrow;
  if (FPDF_TEXTPAGE tp = doc_->TextPage(page)) {
    double tol = 2.0 * 72.0 / dpi_ * ui_;
    if (FPDFText_GetCharIndexAtPos(tp, px, py, tol, tol) >= 0) return ibeam;
  }
  return arrow;
}

std::wstring DocumentView::TooltipAt(int x, int y) {
  if (!Ready() || drag_ != Drag::None) return {};
  int page = PageAtPoint(x, y);
  double px, py;
  if (page < 0 || !ToPagePoint(page, x, y, px, py)) return {};
  LinkTarget t = doc_->LinkAt(page, px, py);
  switch (t.kind) {
    case LinkTarget::Kind::Uri: return t.uri;
    case LinkTarget::Kind::Page: return L"Go to page " + std::to_wstring(t.page + 1);
    case LinkTarget::Kind::Blocked: return L"Links to files and programs are blocked";
    default: return {};
  }
}

bool DocumentView::HasFieldFocus() const {
  int page;
  return Ready() && doc_->FocusedField(page);
}

void DocumentView::AfterFormEvent() {
  bool m = doc_->Modified();
  if (m != lastModified_) {
    lastModified_ = m;
    host_.ViewStateChanged();
  }
  host_.ViewInvalidate();
}

bool DocumentView::FocusFieldOnNextPage(int fromPage, bool backwards) {
  FPDF_FORMHANDLE form = doc_->Form();
  int n = PageCount();
  for (int step = 1; step < n; ++step) {
    int p = backwards ? (fromPage - step + n) % n : (fromPage + step) % n;
    FORM_ForceToKillFocus(form);
    if (FORM_OnKeyDown(form, doc_->Page(p), VK_TAB, backwards ? kModShift : 0)) {
      int fp;
      if (doc_->FocusedField(fp)) return true;
    }
  }
  return false;
}

bool DocumentView::FieldKey(UINT vk, UINT mods) {
  int fp;
  if (!doc_->FocusedField(fp)) return false;
  FPDF_FORMHANDLE form = doc_->Form();
  FPDF_PAGE page = doc_->Page(fp);
  if (!page) return false;
  if (mods & kModCtrl) {
    switch (vk) {
      case 'C':
      case 'X': {
        unsigned long bytes = FORM_GetSelectedText(form, page, nullptr, 0);
        if (bytes > 2) {
          std::wstring text(bytes / 2, L'\0');
          FORM_GetSelectedText(form, page, text.data(), bytes);
          text.resize(wcsnlen(text.c_str(), text.size()));
          util::SetClipboardText(text);
          if (vk == 'X') FORM_ReplaceSelection(form, page, reinterpret_cast<FPDF_WIDESTRING>(L""));
        }
        AfterFormEvent();
        return true;
      }
      case 'V': {
        std::wstring text = util::GetClipboardText();
        FORM_ReplaceSelection(form, page, reinterpret_cast<FPDF_WIDESTRING>(text.c_str()));
        AfterFormEvent();
        return true;
      }
      case 'A': FORM_SelectAllText(form, page); AfterFormEvent(); return true;
      case 'Z': FORM_Undo(form, page); AfterFormEvent(); return true;
      case 'Y': FORM_Redo(form, page); AfterFormEvent(); return true;
      case VK_LEFT: case VK_RIGHT: case VK_HOME: case VK_END: case VK_BACK: case VK_DELETE:
        break;  // word-wise editing inside the field
      default: return false;  // app shortcuts (Ctrl+S, Ctrl+P, ...)
    }
  }
  if (vk == VK_ESCAPE) {
    FORM_ForceToKillFocus(form);
    AfterFormEvent();
    return true;
  }
  if (vk == VK_TAB) {
    if (!FORM_OnKeyDown(form, page, VK_TAB, mods)) FocusFieldOnNextPage(fp, (mods & kModShift) != 0);
    AfterFormEvent();
    return true;
  }
  FORM_OnKeyDown(form, page, static_cast<int>(vk), mods);
  AfterFormEvent();
  return true;
}

bool DocumentView::OnKeyDown(UINT vk, UINT mods) {
  if (!Ready() || layout_.rows.empty()) return false;
  if (FieldKey(vk, mods)) return true;
  const bool ctrl = (mods & kModCtrl) != 0, shift = (mods & kModShift) != 0;
  const bool fitsH = layout_.width <= VpW();
  switch (vk) {
    case VK_TAB:
      if (doc_->HasForm() && !ctrl) {
        // Start keyboard form filling on the current page.
        FPDF_FORMHANDLE form = doc_->Form();
        if (!FORM_OnKeyDown(form, doc_->Page(currentPage_), VK_TAB, mods)) FocusFieldOnNextPage(currentPage_, shift);
        AfterFormEvent();
        return true;
      }
      return false;
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
    case VK_ESCAPE:
      if (!selection_.Empty()) {
        selection_.Clear();
        host_.ViewInvalidate();
        return true;
      }
      return false;
  }
  return false;
}

bool DocumentView::OnChar(wchar_t ch, UINT mods) {
  if (!Ready()) return false;
  int fp;
  if (!doc_->FocusedField(fp)) return false;
  if (ch == 9 || ch == 27) return true;            // handled in OnKeyDown
  if (ch < 32 && ch != 8 && ch != 13) return true;  // Ctrl+letter combinations
  FORM_OnChar(doc_->Form(), doc_->Page(fp), ch, mods);
  AfterFormEvent();
  return true;
}

void DocumentView::FollowLink(const LinkTarget& link) {
  switch (link.kind) {
    case LinkTarget::Kind::Page: GoToPosition(link.page, link.hasPos, link.x, link.y); break;
    case LinkTarget::Kind::Uri:
      if (!util::OpenExternalLink(link.uri))
        host_.ViewMessage(BannerKind::Warning, L"This link was blocked because it doesn’t point to a website.");
      break;
    case LinkTarget::Kind::Blocked:
      host_.ViewMessage(BannerKind::Warning,
                        L"For your safety, Featherweight doesn’t open files or programs from links in PDFs.");
      break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// Text

void DocumentView::Copy() {
  if (!Ready()) return;
  if (FieldKey('C', kModCtrl)) return;
  if (selection_.Empty()) return;
  std::wstring text = textsel::Text(*doc_, selection_);
  if (!text.empty()) {
    util::SetClipboardText(text);
    host_.ViewMessage(BannerKind::Success, L"Copied");
  }
}

std::wstring DocumentView::SelectedText() {
  if (!Ready() || selection_.Empty()) return {};
  return textsel::Text(*doc_, selection_, 1000);
}

void DocumentView::SelectAll() {
  if (!Ready() || PageCount() == 0) return;
  if (FieldKey('A', kModCtrl)) return;
  int last = PageCount() - 1;
  int count = textsel::CharCount(*doc_, last);
  selection_.Set({0, 0}, {last, std::max(0, count - 1)});
  host_.ViewInvalidate();
}

void DocumentView::SelectAllOnPage() {
  if (!Ready()) return;
  int count = textsel::CharCount(*doc_, currentPage_);
  if (count > 0) selection_.Set({currentPage_, 0}, {currentPage_, count - 1});
  host_.ViewInvalidate();
}

// ---------------------------------------------------------------------------
// Search

void DocumentView::StartSearch(const std::wstring& query, bool matchCase) {
  if (!Ready()) return;
  if (query.empty()) {
    ClearSearch();
    return;
  }
  search_.Start(query, matchCase, currentPage_, PageCount());
  lastSearchCurrent_ = -1;
  host_.ViewInvalidate();
  host_.ViewStateChanged();
}

void DocumentView::ClearSearch() {
  search_.Clear();
  lastSearchCurrent_ = -1;
  host_.ViewInvalidate();
}

void DocumentView::SearchStep(bool forward) {
  if (!search_.Active()) return;
  int i = search_.Step(forward);
  lastSearchCurrent_ = i;
  if (i >= 0) ScrollToMatch(i);
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

void DocumentView::ScrollToMatch(int index) {
  auto& matches = search_.Matches();
  if (index < 0 || index >= static_cast<int>(matches.size())) return;
  auto& m = matches[index];
  const auto& rects = search_.RectsFor(*doc_, m);
  if (rects.empty()) {
    GoToPage(m.page);
    return;
  }
  if (viewMode_ == fw::ViewMode::Single && layout_.rowOfPage[m.page] != singleRow_) {
    singleRow_ = layout_.rowOfPage[m.page];
    sy_ = tsy_ = 0;
  }
  ScrollPageRectIntoView(m.page, rects.front(), true);
}

int DocumentView::SearchCount() const { return static_cast<int>(const_cast<Search&>(search_).Matches().size()); }
int DocumentView::SearchCurrent() const { return search_.Current(); }
bool DocumentView::SearchDone() const { return search_.Done(); }

// ---------------------------------------------------------------------------
// File and state

bool DocumentView::Save(const std::wstring& path, std::wstring& error) {
  if (!Ready()) return false;
  bool ok = doc_->Save(path, error);
  if (ok) path_ = doc_->Path();
  lastModified_ = doc_->Modified();
  host_.ViewStateChanged();
  host_.ViewInvalidate();
  return ok;
}

FileState DocumentView::State() const {
  FileState s;
  s.page = currentPage_;
  s.zoomMode = presentation_ ? beforePresentation_.zoomMode : zoomMode_;
  s.zoom = presentation_ ? beforePresentation_.zoom : Zoom();
  s.viewMode = presentation_ ? beforePresentation_.viewMode : viewMode_;
  s.rotation = rotation_;
  return s;
}

void DocumentView::ApplyState(const FileState& s) {
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

// ---------------------------------------------------------------------------
// PDFium callbacks

void DocumentView::OnFormInvalidate(int page, double l, double t, double r, double b) {
  if (cache_) cache_->MarkDirty(page, l, t, r, b);
  host_.ViewInvalidate();
}

void DocumentView::OnFormCursor(int type) { formCursor_ = type; }

void DocumentView::OnFormChanged() {
  if (doc_ && doc_->Modified() != lastModified_) {
    lastModified_ = doc_->Modified();
    host_.ViewStateChanged();
  }
}

void DocumentView::OnFormGoToPage(int page, bool hasPos, float x, float y) { GoToPosition(page, hasPos, x, y); }

void DocumentView::OnFormOpenUri(const std::wstring& uri) {
  LinkTarget t;
  t.kind = LinkTarget::Kind::Uri;
  t.uri = uri;
  FollowLink(t);
}

void DocumentView::OnFormNamedAction(const std::string& name) {
  if (name == "NextPage") NextPage();
  else if (name == "PrevPage") PrevPage();
  else if (name == "FirstPage") GoToPage(0);
  else if (name == "LastPage") GoToPage(PageCount() - 1);
  else if (name == "Print") host_.ViewRequestPrint();
}

int DocumentView::OnFormAlert(const std::wstring& message, const std::wstring& title, int type, int iconType) {
  UINT flags = MB_OK;
  switch (type) {
    case 1: flags = MB_OKCANCEL; break;
    case 2: flags = MB_YESNO; break;
    case 3: flags = MB_YESNOCANCEL; break;
  }
  switch (iconType) {
    case 0: flags |= MB_ICONERROR; break;
    case 1: flags |= MB_ICONWARNING; break;
    case 2: flags |= MB_ICONQUESTION; break;
    default: flags |= MB_ICONINFORMATION; break;
  }
  int r = MessageBoxW(host_.ViewHwnd(), message.c_str(), title.empty() ? Title().c_str() : title.c_str(), flags);
  switch (r) {
    case IDCANCEL: return 2;
    case IDNO: return 3;
    case IDYES: return 4;
    default: return 1;
  }
}

void DocumentView::OnFormPrint() { host_.ViewRequestPrint(); }

void DocumentView::OnFormPageCountChanged() {
  if (!doc_) return;
  doc_->RefreshPageSizes();
  if (cache_) cache_->Clear();
  RecomputeScale();
  Relayout(nullptr);
  host_.ViewStateChanged();
}

void DocumentView::OnFormCaret(int page, bool visible, double l, double t, double r, double b) {
  caret_.page = page;
  caret_.visible = visible;
  caret_.l = l;
  caret_.t = t;
  caret_.r = r;
  caret_.b = b;
  host_.ViewInvalidate();
}

void DocumentView::OnFormFocusChanged(int page, bool hasRect, const FS_RECTF& rect) {
  if (hasRect && page >= 0) ScrollPageRectIntoView(page, rect, false);
}
