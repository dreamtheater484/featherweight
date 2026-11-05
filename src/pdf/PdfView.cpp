#include "pdf/PdfView.h"

#include <d2d1effects.h>
#include <fpdf_formfill.h>
#include <fpdf_fwlevent.h>
#include <fpdf_text.h>

#include <algorithm>
#include <cmath>

#include "Commands.h"
#include "Util.h"
#include "core/Strings.h"
#include "pdf/Print.h"
#include "ui/Anim.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include "ui/Uia.h"
#include "ui/Toolbar.h"

using namespace gfx;

namespace {

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

PdfView::PdfView(PdfHost& host, std::wstring path) : PagedView(host), pdfHost_(host), path_(std::move(path)) {
  pwBox_.password = true;
  pwBox_.placeholder = L"Password";
  pwBox_.onEnter = [this](bool) { SubmitPassword(); };
  pwOpen_.id = 1;
  pwOpen_.text = L"Open";
  pwOpen_.style = ButtonStyle::Primary;
  pwButtons_.Add(&pwOpen_);
  selHighlight_.id = 1;
  selHighlight_.glyph = icon::Highlight;
  selHighlight_.text = L"Highlight";
  selHighlight_.glyphSize = 14;
  selHighlight_.tooltip = L"Highlight the selected text";
  selCopy_.id = 2;
  selCopy_.glyph = icon::Copy;
  selCopy_.text = L"Copy";
  selCopy_.glyphSize = 14;
  selCopy_.tooltip = L"Copy (Ctrl+C)";
  selButtons_.Add(&selHighlight_);
  selButtons_.Add(&selCopy_);
}

PdfView::~PdfView() {
  cache_.reset();
  doc_.reset();
}

const std::wstring& PdfView::Path() const { return doc_ ? doc_->Path() : path_; }

std::wstring PdfView::Title() const { return fw::FileNameOf(Path()); }

OpenResult PdfView::Load(const std::wstring& password, std::wstring& error) {
  std::unique_ptr<Document> doc;
  OpenResult r = Document::Open(path_, password, doc, error);
  if (r == OpenResult::NeedPassword || r == OpenResult::WrongPassword) {
    state_ = LoadState::NeedPassword;
    return r;
  }
  if (r != OpenResult::Ok) return r;
  doc_ = std::move(doc);
  cache_ = std::make_unique<RenderCache>(*doc_);
  doc_->AttachHost(this, pdfHost_.ViewHighlightFields());
  state_ = LoadState::Ready;
  notesScanned_.assign(doc_->PageCount(), false);
  notes_.clear();
  noteScanNext_ = 0;
  Relayout(nullptr);
  return r;
}

void PdfView::SubmitPassword() {
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

bool PdfView::ToPagePoint(int page, int x, int y, double& px, double& py) {
  FPDF_PAGE p = doc_->Page(page);
  if (!p) return false;
  RECT r = PageRect(page);
  return FPDF_DeviceToPage(p, r.left, r.top, r.right - r.left, r.bottom - r.top, rotation_, x, y, &px, &py) != 0;
}

D2D1_RECT_F PdfView::PageRectToScreen(int page, const FS_RECTF& rc) {
  FPDF_PAGE p = doc_->Page(page);
  if (!p) return {};
  RECT r = PageRect(page);
  int x1, y1, x2, y2;
  FPDF_PageToDevice(p, r.left, r.top, r.right - r.left, r.bottom - r.top, rotation_, rc.left, rc.top, &x1, &y1);
  FPDF_PageToDevice(p, r.left, r.top, r.right - r.left, r.bottom - r.top, rotation_, rc.right, rc.bottom, &x2, &y2);
  return RectF(static_cast<float>(std::min(x1, x2)), static_cast<float>(std::min(y1, y2)),
               static_cast<float>(std::max(x1, x2)), static_cast<float>(std::max(y1, y2)));
}

void PdfView::GoToPosition(int page, bool hasPos, float x, float y) {
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

void PdfView::ScrollPageRectIntoView(int page, const FS_RECTF& rc, bool center) {
  if (!Ready() || page < 0 || page >= PageCount()) return;
  if (viewMode_ == fw::ViewMode::Single && layout_.rowOfPage[page] != singleRow_) {
    singleRow_ = layout_.rowOfPage[page];
    sy_ = tsy_ = 0;
  }
  ScrollScreenRectIntoView(PageRectToScreen(page, rc), center);
}

// ---------------------------------------------------------------------------
// View settings and presenting

void PdfView::SettingsChanged() {
  if (!Ready()) return;
  doc_->SetHighlightFields(pdfHost_.ViewHighlightFields());
  cache_->InvalidateAll();
  int page = currentPage_;
  RecomputeScale();
  Relayout(nullptr);
  GoToPage(page);
}

void PdfView::OnPresentationChanged(bool on) {
  if (!on) return;
  ClearSearch();
  selection_.Clear();
}

const std::vector<fw::PageSizePt>* PdfView::Pages() const { return doc_ ? &doc_->PageSizes() : nullptr; }

bool PdfView::CoverAlone() const { return pdfHost_.ViewCoverAlone(); }

// ---------------------------------------------------------------------------
// Painting

void PdfView::DrawBitmap(ID2D1DeviceContext* dc, ID2D1Bitmap* bmp, const D2D1_RECT_F& dest,
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

void PdfView::Draw(Canvas& c, ID2D1DeviceContext* dc) {
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
  if (inverted) DrawNoteIconsUninverted(c, first, last);
  if (!presentation_) DrawNoteMarkers(c);
  if (!presentation_) DrawScrollbars(c);
  DrawNoteHover(c);
  if (selBar_ && !selection_.Empty() && drag_ != Drag::Select) DrawSelectionBar(c);
  c.PopClip();
}

void PdfView::DrawPage(Canvas& c, ID2D1DeviceContext* dc, int page, bool inverted) {
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

void PdfView::DrawOverlays(Canvas& c, int first, int last) {
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

void PdfView::DrawPasswordCard(Canvas& c) {
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

bool PdfView::HasIdleWork() const {
  if (!Ready()) return false;
  return cache_->HasWork() || (search_.Active() && !search_.Done()) || noteScanNext_ < PageCount();
}

bool PdfView::IdleWork(ID2D1DeviceContext* dc, double deadlineMs) {
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
  if (anim::Now() < deadlineMs && ScanNotes(deadlineMs)) repaint = true;

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

void PdfView::OnDeactivated() {
  if (cache_) cache_->DropTiles();
  drag_ = Drag::None;
}

void PdfView::ReleaseDeviceResources() {
  invertFx_.Reset();
  fxDc_ = nullptr;
  if (cache_) cache_->ReleaseDeviceResources();
}

// ---------------------------------------------------------------------------
// Input

void PdfView::OnMouseDown(int x, int y, int button, UINT mods, bool doubleClick) {
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
  if (selBar_ && button == 0) {
    D2D1_RECT_F bar;
    if (SelectionBarRect(bar) && gfx::Contains(bar, (float)x, (float)y)) {
      selButtons_.OnMouseDown((float)x, (float)y);
      drag_ = Drag::None;
      barPressed_ = true;
      host_.ViewInvalidate();
      return;
    }
  }
  selBar_ = false;
  BeginPress(x, y);

  if (button == 2) {  // middle button pans
    drag_ = Drag::Pan;
    return;
  }

  if (!presentation_ && button == 0) {
    int marker = MarkerAt(x, y);
    if (marker >= 0) {
      GoToNote(notes_[marker]);
      return;
    }
  }
  hoverNote_ = HoverNote{};

  if (ScrollbarMouseDown(x, y, button)) return;

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

  if (onPage && button == 0) {
    AnnotHit hit = doc_->AnnotAt(page, px, py);
    if (hit.kind == AnnotHit::Kind::Note) {
      pendingNote_ = NoteTarget{page, hit.index, hit.rect.left, hit.rect.top};
      drag_ = Drag::Note;
      return;
    }
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
          selBar_ = true;
          selButtons_.Reset();
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

void PdfView::OnMouseMove(int x, int y, UINT mods) {
  mouseX_ = x;
  mouseY_ = y;
  if (state_ == LoadState::NeedPassword) {
    if (GetKeyState(VK_LBUTTON) < 0 && pwBox_.focused && gfx::Contains(Inflate(pwBox_.rc, 200, 50), (float)x, (float)y))
      pwBox_.OnMouseDrag((float)x, (float)y, ui_);
    if (pwButtons_.OnMouseMove((float)x, (float)y)) host_.ViewInvalidate();
    return;
  }
  if (!Ready()) return;

  if (ScrollDragMove(x, y)) return;
  switch (drag_) {
    case Drag::Form: {
      double px, py;
      if (formPage_ >= 0 && ToPagePoint(formPage_, x, y, px, py)) {
        FORM_OnMouseMove(doc_->Form(), doc_->Page(formPage_), mods | FWL_EVENTFLAG_LeftButtonDown, px, py);
        AfterFormEvent();
      }
      return;
    }
    case Drag::Link:
    case Drag::Note:
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
    default:
      break;
  }

  // Hover feedback.
  if (selBar_ && selButtons_.OnMouseMove((float)x, (float)y)) host_.ViewInvalidate();
  UpdateScrollbarHover(x, y);
  if (FPDF_FORMHANDLE form = doc_->Form()) {
    // Only form fields get mouse moves. PDFium would otherwise open its own old-style popups for notes.
    int page = PageAtPoint(x, y);
    double px = 0, py = 0;
    bool overField = page >= 0 && ToPagePoint(page, x, y, px, py) &&
                     FPDFPage_HasFormFieldAtPoint(form, doc_->Page(page), px, py) >= 0;
    if (overField) {
      FORM_OnMouseMove(form, doc_->Page(page), mods, px, py);
      formHoverPage_ = page;
      AfterFormEvent();
    } else if (formHoverPage_ >= 0) {
      // Tell the field the mouse left, at a point where nothing can be.
      if (FPDF_PAGE p = doc_->PageIfLoaded(formHoverPage_)) FORM_OnMouseMove(form, p, mods, -100000.0, -100000.0);
      formHoverPage_ = -1;
      AfterFormEvent();
    }
  }
  UpdateHoverNote(x, y);
  int marker = MarkerAt(x, y);
  if (marker != markerHover_) {
    markerHover_ = marker;
    host_.ViewInvalidate();
  }
}

void PdfView::OnMouseUp(int x, int y, int button, UINT mods) {
  if (state_ == LoadState::NeedPassword) {
    if (pwButtons_.OnMouseUp((float)x, (float)y) == 1) SubmitPassword();
    host_.ViewInvalidate();
    return;
  }
  if (!Ready()) return;
  if (barPressed_) {
    barPressed_ = false;
    switch (selButtons_.OnMouseUp((float)x, (float)y)) {
      case 1: HighlightSelection(); break;
      case 2:
        Copy();
        selBar_ = false;
        break;
    }
    host_.ViewInvalidate();
    return;
  }
  Drag d = drag_;
  drag_ = Drag::None;
  (void)button;
  if (d == Drag::Form && formPage_ >= 0) {
    double px, py;
    if (ToPagePoint(formPage_, x, y, px, py)) FORM_OnLButtonUp(doc_->Form(), doc_->Page(formPage_), mods, px, py);
    AfterFormEvent();
  } else if (d == Drag::Link) {
    FollowLink(pendingLink_);
  } else if (d == Drag::Note) {
    pdfHost_.ViewOpenNote(this, pendingNote_);
  } else if (d == Drag::Select) {
    if (selection_.anchor == selection_.focus && std::abs(x - downX_) + std::abs(y - downY_) < 3) {
      selection_.Clear();  // a plain click on text just clears the selection
    } else {
      selBar_ = true;
      selButtons_.Reset();
    }
    host_.ViewInvalidate();
  }
}

void PdfView::OnMouseLeave() {
  mouseX_ = mouseY_ = -1;
  hoverNote_ = HoverNote{};
  markerHover_ = -1;
  selButtons_.OnMouseLeave();
  ScrollbarMouseLeave();
  if (state_ == LoadState::NeedPassword) pwButtons_.OnMouseLeave();
  host_.ViewInvalidate();
}

HCURSOR PdfView::CursorAt(int x, int y) {
  static HCURSOR arrow = LoadCursor(nullptr, IDC_ARROW), ibeam = LoadCursor(nullptr, IDC_IBEAM),
                 hand = LoadCursor(nullptr, IDC_HAND), move = LoadCursor(nullptr, IDC_SIZEALL);
  if (state_ == LoadState::NeedPassword) return gfx::Contains(pwBox_.rc, (float)x, (float)y) ? ibeam : arrow;
  if (!Ready()) return arrow;
  if (drag_ == Drag::Pan) return move;
  if (drag_ == Drag::Select) return ibeam;
  if (OverScrollbar(x, y)) return arrow;
  if (selBar_) {
    D2D1_RECT_F bar;
    if (SelectionBarRect(bar) && gfx::Contains(bar, (float)x, (float)y)) return hand;
  }
  if (MarkerAt(x, y) >= 0) return hand;
  int page = PageAtPoint(x, y);
  double px, py;
  if (page < 0 || !ToPagePoint(page, x, y, px, py)) return arrow;
  if (doc_->AnnotAt(page, px, py).kind == AnnotHit::Kind::Note) return hand;
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

std::wstring PdfView::TooltipAt(int x, int y) {
  if (!Ready() || drag_ != Drag::None) return {};
  if (int m = MarkerAt(x, y); m >= 0) return L"Note on page " + std::to_wstring(notes_[m].page + 1);
  if (selBar_) {
    D2D1_RECT_F bar;
    if (SelectionBarRect(bar) && gfx::Contains(bar, (float)x, (float)y))
      return gfx::Contains(selHighlight_.rc, (float)x, (float)y) ? selHighlight_.tooltip : selCopy_.tooltip;
  }
  int page = PageAtPoint(x, y);
  double px, py;
  if (page < 0 || !ToPagePoint(page, x, y, px, py)) return {};
  if (doc_->AnnotAt(page, px, py).kind == AnnotHit::Kind::Note) return {};  // shown in the preview card
  LinkTarget t = doc_->LinkAt(page, px, py);
  switch (t.kind) {
    case LinkTarget::Kind::Uri: return t.uri;
    case LinkTarget::Kind::Page: return L"Go to page " + std::to_wstring(t.page + 1);
    case LinkTarget::Kind::Blocked: return L"Links to files and programs are blocked";
    default: return {};
  }
}

bool PdfView::HasKeyFocus() const {
  int page;
  return Ready() && doc_->FocusedField(page);
}

void PdfView::AfterFormEvent() {
  bool m = doc_->Modified();
  if (m != lastModified_) {
    lastModified_ = m;
    host_.ViewStateChanged();
  }
  host_.ViewInvalidate();
}

bool PdfView::FocusFieldOnNextPage(int fromPage, bool backwards) {
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

bool PdfView::FieldKey(UINT vk, UINT mods) {
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

bool PdfView::OnKeyDown(UINT vk, UINT mods) {
  if (!Ready() || layout_.rows.empty()) return false;
  if (FieldKey(vk, mods)) return true;
  const bool ctrl = (mods & kModCtrl) != 0, shift = (mods & kModShift) != 0;
  if (vk == VK_TAB) {
    if (doc_->HasForm() && !ctrl) {
      // Start keyboard form filling on the current page.
      FPDF_FORMHANDLE form = doc_->Form();
      if (!FORM_OnKeyDown(form, doc_->Page(currentPage_), VK_TAB, mods)) FocusFieldOnNextPage(currentPage_, shift);
      AfterFormEvent();
      return true;
    }
    return false;
  }
  if (vk == VK_ESCAPE) {
    if (!selection_.Empty()) {
      selection_.Clear();
      host_.ViewInvalidate();
      return true;
    }
    return false;
  }
  return NavigationKey(vk, mods);
}

bool PdfView::OnChar(wchar_t ch, UINT mods) {
  if (!Ready()) return false;
  int fp;
  if (!doc_->FocusedField(fp)) return false;
  if (ch == 9 || ch == 27) return true;            // handled in OnKeyDown
  if (ch < 32 && ch != 8 && ch != 13) return true;  // Ctrl+letter combinations
  FORM_OnChar(doc_->Form(), doc_->Page(fp), ch, mods);
  AfterFormEvent();
  return true;
}

void PdfView::FollowLink(const LinkTarget& link) {
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

void PdfView::Copy() {
  if (!Ready()) return;
  if (FieldKey('C', kModCtrl)) return;
  if (selection_.Empty()) return;
  std::wstring text = textsel::Text(*doc_, selection_);
  if (!text.empty()) {
    util::SetClipboardText(text);
    host_.ViewMessage(BannerKind::Success, L"Copied");
  }
}

std::wstring PdfView::SelectedText() {
  if (!Ready() || selection_.Empty()) return {};
  return textsel::Text(*doc_, selection_, 1000);
}

void PdfView::SelectAll() {
  if (!Ready() || PageCount() == 0) return;
  if (FieldKey('A', kModCtrl)) return;
  int last = PageCount() - 1;
  int count = textsel::CharCount(*doc_, last);
  selection_.Set({0, 0}, {last, std::max(0, count - 1)});
  host_.ViewInvalidate();
}

void PdfView::SelectAllOnPage() {
  if (!Ready()) return;
  int count = textsel::CharCount(*doc_, currentPage_);
  if (count > 0) selection_.Set({currentPage_, 0}, {currentPage_, count - 1});
  host_.ViewInvalidate();
}

// ---------------------------------------------------------------------------
// Search

void PdfView::StartSearch(const std::wstring& query, bool matchCase, bool wholeWords) {
  if (!Ready()) return;
  if (query.empty()) {
    ClearSearch();
    return;
  }
  search_.Start(query, matchCase, wholeWords, currentPage_, PageCount());
  lastSearchCurrent_ = -1;
  host_.ViewInvalidate();
  host_.ViewStateChanged();
}

void PdfView::ClearSearch() {
  search_.Clear();
  lastSearchCurrent_ = -1;
  host_.ViewInvalidate();
}

void PdfView::SearchStep(bool forward) {
  if (!search_.Active()) return;
  int i = search_.Step(forward);
  lastSearchCurrent_ = i;
  if (i >= 0) ScrollToMatch(i);
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

void PdfView::ScrollToMatch(int index) {
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

int PdfView::SearchCount() const { return static_cast<int>(const_cast<Search&>(search_).Matches().size()); }
int PdfView::SearchCurrent() const { return search_.Current(); }
bool PdfView::SearchDone() const { return search_.Done(); }

// ---------------------------------------------------------------------------
// File and state

bool PdfView::Save(const std::wstring& path, std::wstring& error) {
  if (!Ready()) return false;
  bool ok = doc_->Save(path, error);
  if (ok) path_ = doc_->Path();
  lastModified_ = doc_->Modified();
  host_.ViewStateChanged();
  host_.ViewInvalidate();
  return ok;
}

bool PdfView::ExportPdf(const std::wstring& path, std::wstring& message) {
  if (!Ready() || !CanExportPdf()) {
    message = L"This PDF can't be exported.";
    return false;
  }
  if (!doc_->ExportFlattened(path, message)) return false;
  const int count = doc_->PageCount();
  message = count == 1 ? L"Exported 1 page to " + fw::FileNameOf(path)
                       : L"Exported " + std::to_wstring(count) + L" pages to " + fw::FileNameOf(path);
  return true;
}

// ---------------------------------------------------------------------------
// PDFium callbacks

void PdfView::OnFormInvalidate(int page, double l, double t, double r, double b) {
  if (cache_) cache_->MarkDirty(page, l, t, r, b);
  host_.ViewInvalidate();
}

void PdfView::OnFormCursor(int type) { formCursor_ = type; }

void PdfView::OnFormChanged() {
  if (doc_ && doc_->Modified() != lastModified_) {
    lastModified_ = doc_->Modified();
    host_.ViewStateChanged();
  }
}

void PdfView::OnFormGoToPage(int page, bool hasPos, float x, float y) { GoToPosition(page, hasPos, x, y); }

void PdfView::OnFormOpenUri(const std::wstring& uri) {
  LinkTarget t;
  t.kind = LinkTarget::Kind::Uri;
  t.uri = uri;
  FollowLink(t);
}

void PdfView::OnFormNamedAction(const std::string& name) {
  if (name == "NextPage") NextPage();
  else if (name == "PrevPage") PrevPage();
  else if (name == "FirstPage") GoToPage(0);
  else if (name == "LastPage") GoToPage(PageCount() - 1);
  else if (name == "Print") host_.ViewRequestPrint();
}

int PdfView::OnFormAlert(const std::wstring& message, const std::wstring& title, int type, int iconType) {
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

void PdfView::OnFormPrint() { host_.ViewRequestPrint(); }

void PdfView::OnFormPageCountChanged() {
  if (!doc_) return;
  doc_->RefreshPageSizes();
  if (cache_) cache_->Clear();
  notesScanned_.assign(doc_->PageCount(), false);
  notes_.clear();
  noteScanNext_ = 0;
  RecomputeScale();
  Relayout(nullptr);
  host_.ViewStateChanged();
}

void PdfView::OnFormCaret(int page, bool visible, double l, double t, double r, double b) {
  caret_.page = page;
  caret_.visible = visible;
  caret_.l = l;
  caret_.t = t;
  caret_.r = r;
  caret_.b = b;
  host_.ViewInvalidate();
}

void PdfView::OnFormFocusChanged(int page, bool hasRect, const FS_RECTF& rect) {
  if (hasRect && page >= 0) ScrollPageRectIntoView(page, rect, false);
}

// ---------------------------------------------------------------------------
// Highlights and notes

void PdfView::AnnotationsChanged(int page) {
  cache_->InvalidatePage(page);
  RescanNotes(page);
  hoverNote_ = HoverNote{};
  lastModified_ = doc_->Modified();
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

bool PdfView::HighlightSelection() {
  if (!Ready() || selection_.Empty()) return false;
  TextPos a = selection_.Start(), b = selection_.End();
  bool any = false;
  for (int p = a.page; p <= b.page; ++p) {
    std::vector<FS_RECTF> rects = textsel::Rects(*doc_, selection_, p);
    if (!rects.empty() && doc_->AddHighlight(p, rects)) {
      AnnotationsChanged(p);
      any = true;
    }
  }
  selection_.Clear();
  selBar_ = false;
  host_.ViewInvalidate();
  return any;
}

AnnotHit PdfView::AnnotAtPoint(int x, int y, int& page) {
  page = -1;
  if (!Ready()) return {};
  int p = PageAtPoint(x, y);
  double px, py;
  if (p < 0 || !ToPagePoint(p, x, y, px, py)) return {};
  page = p;
  return doc_->AnnotAt(p, px, py);
}

void PdfView::RemoveAnnot(int page, int index) {
  if (Ready() && doc_->RemoveAnnot(page, index)) AnnotationsChanged(page);
}

bool PdfView::NewNoteAt(int x, int y, NoteTarget& note) {
  int p = PageAtPoint(x, y);
  double px, py;
  if (!Ready() || p < 0 || !ToPagePoint(p, x, y, px, py)) return false;
  note = NoteTarget{p, -1, static_cast<float>(px), static_cast<float>(py)};
  return true;
}

std::wstring PdfView::NoteText(const NoteTarget& note) {
  if (!Ready() || note.index < 0) return {};
  return doc_->NoteText(note.page, note.index);
}

void PdfView::SaveNote(const NoteTarget& note, const std::wstring& text) {
  if (!Ready() || note.page < 0) return;
  if (note.index < 0) {
    if (!text.empty() && doc_->AddNote(note.page, note.x, note.y, text) >= 0) AnnotationsChanged(note.page);
    return;
  }
  if (text.empty()) {
    RemoveAnnot(note.page, note.index);
  } else if (text != doc_->NoteText(note.page, note.index) && doc_->SetNoteText(note.page, note.index, text)) {
    AnnotationsChanged(note.page);
  }
}

D2D1_POINT_2F PdfView::NoteScreenPos(const NoteTarget& note) {
  if (!Ready() || note.page < 0) return {};
  FS_RECTF r{note.x, note.y, note.x, note.y};
  D2D1_RECT_F s = PageRectToScreen(note.page, r);
  return {s.left, s.top};
}

bool PdfView::SelectionBarRect(D2D1_RECT_F& rect) {
  if (selection_.Empty()) return false;
  const TextPos end = selection_.End();
  int first, last;
  VisiblePages(first, last);
  if (end.page < first || end.page > last) return false;
  if (selRectsVersion_ != selection_.version) {
    selRects_.clear();
    selRectsVersion_ = selection_.version;
  }
  auto it = selRects_.find(end.page);
  if (it == selRects_.end()) it = selRects_.emplace(end.page, textsel::Rects(*doc_, selection_, end.page)).first;
  if (it->second.empty()) return false;
  D2D1_RECT_F lastLine = PageRectToScreen(end.page, it->second.back());
  const float u = ui_;
  IDWriteTextFormat* fmt = gfx::Canvas::Format(13 * u, DWRITE_FONT_WEIGHT_REGULAR, false);
  auto measure = [&](const std::wstring& s) {
    ComPtr<IDWriteTextLayout> layout;
    gfx::DWrite()->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt, 1000, 100, &layout);
    DWRITE_TEXT_METRICS m{};
    if (layout) layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
  };
  float w1 = std::ceil(measure(selHighlight_.text) + 14 * u + 8 * u + 24 * u);
  float w2 = std::ceil(measure(selCopy_.text) + 14 * u + 8 * u + 24 * u);
  float w = w1 + w2 + 4 * u * 3, h = 38 * u;
  float x = std::round(std::min(lastLine.right - w / 2, vp_.right - w - 24 * u));
  x = std::max(x, vp_.left + 8 * u);
  float y = std::round(lastLine.bottom + 8 * u);
  if (y + h > vp_.bottom - 8 * u) y = std::round(lastLine.top - h - 8 * u);
  rect = gfx::RectF(x, y, x + w, y + h);
  selHighlight_.rc = gfx::RectF(x + 4 * u, y + 4 * u, x + 4 * u + w1, y + h - 4 * u);
  selCopy_.rc = gfx::RectF(selHighlight_.rc.right + 4 * u, y + 4 * u, selHighlight_.rc.right + 4 * u + w2, y + h - 4 * u);
  return true;
}

void PdfView::DrawSelectionBar(Canvas& c) {
  D2D1_RECT_F bar;
  if (!SelectionBarRect(bar)) return;
  const Palette& p = theme::P();
  c.SoftShadow(bar, 8 * ui_, 10 * ui_, p.popupShadow, 3 * ui_);
  c.FillRound(bar, 8 * ui_, p.popupBg);
  c.StrokeRound(bar, 8 * ui_, p.popupBorder, 1);
  DrawButton(c, selHighlight_, ui_);
  DrawButton(c, selCopy_, ui_);
}

// ---------------------------------------------------------------------------
// Note markers and hover preview

bool PdfView::ScanNotes(double deadlineMs) {
  bool found = false;
  const int n = PageCount();
  while (noteScanNext_ < n && anim::Now() < deadlineMs) {
    int p = noteScanNext_++;
    if (p < static_cast<int>(notesScanned_.size()) && notesScanned_[p]) continue;
    for (const NoteInfo& info : doc_->NotesOnPage(p)) {
      notes_.push_back(info);
      found = true;
    }
    if (p < static_cast<int>(notesScanned_.size())) notesScanned_[p] = true;
  }
  if (found)
    std::sort(notes_.begin(), notes_.end(),
              [](const NoteInfo& a, const NoteInfo& b) { return a.page != b.page ? a.page < b.page : a.fy < b.fy; });
  return found;
}

void PdfView::RescanNotes(int page) {
  notes_.erase(std::remove_if(notes_.begin(), notes_.end(), [&](const NoteInfo& n) { return n.page == page; }),
               notes_.end());
  for (const NoteInfo& info : doc_->NotesOnPage(page)) notes_.push_back(info);
  std::sort(notes_.begin(), notes_.end(),
            [](const NoteInfo& a, const NoteInfo& b) { return a.page != b.page ? a.page < b.page : a.fy < b.fy; });
  if (page < static_cast<int>(notesScanned_.size())) notesScanned_[page] = true;
}

D2D1_RECT_F PdfView::MarkerRect(const NoteInfo& n) const {
  if (n.page < 0 || n.page >= static_cast<int>(layout_.boxes.size())) return {};
  // Where the note sits vertically, as a fraction of the whole document (view rotation applied).
  float fx = n.fx, fy = n.fy;
  switch (rotation_) {
    case 1: fy = fx; break;
    case 2: fy = 1 - fy; break;
    case 3: fy = 1 - fx; break;
  }
  double fraction;
  if (viewMode_ == fw::ViewMode::Single) {
    fraction = (layout_.rowOfPage[n.page] + fy) / std::max<size_t>(1, layout_.rows.size());
  } else {
    const auto& b = layout_.boxes[n.page];
    fraction = (b.y + fy * b.h) / std::max(1, layout_.height);
  }
  const float u = ui_;
  float top = vp_.top + 4 * u, bottom = vp_.bottom - 4 * u;
  float y = static_cast<float>(top + fraction * (bottom - top));
  float right = vp_.right - 9 * u;
  return gfx::RectF(right - 9 * u, std::round(y - 2 * u), right, std::round(y + 2 * u));
}

int PdfView::MarkerAt(int x, int y) const {
  if (!Ready() || presentation_ || notes_.empty()) return -1;
  if (x < vp_.right - 20 * ui_ || x >= vp_.right) return -1;
  int best = -1;
  float bestD = 7 * ui_;
  for (int i = 0; i < static_cast<int>(notes_.size()); ++i) {
    D2D1_RECT_F r = MarkerRect(notes_[i]);
    float d = std::abs((r.top + r.bottom) / 2 - y);
    if (d < bestD) {
      bestD = d;
      best = i;
    }
  }
  return best;
}

void PdfView::DrawNoteMarkers(Canvas& c) {
  if (notes_.empty()) return;
  const Palette& p = theme::P();
  const D2D1_COLOR_F amber = gfx::Rgb(0xFFB900);
  for (int i = 0; i < static_cast<int>(notes_.size()); ++i) {
    D2D1_RECT_F r = MarkerRect(notes_[i]);
    if (gfx::Width(r) <= 0) continue;
    if (i == markerHover_) r = gfx::Inflate(r, 2 * ui_, 1 * ui_);
    c.FillRound(gfx::Inflate(r, 1, 1), 2.5f * ui_, gfx::Alpha(p.docBg, 0.8f));
    c.FillRound(r, 2 * ui_, i == markerHover_ ? amber : gfx::Alpha(amber, 0.85f));
  }
}

void PdfView::GoToNote(const NoteInfo& n) {
  ScrollPageRectIntoView(n.page, n.rect, true);
  hoverNote_ = HoverNote{n.page, n.index, n.rect, doc_->NoteText(n.page, n.index), anim::Now() - 1000};
  host_.ViewInvalidate();
}

void PdfView::UpdateHoverNote(int x, int y) {
  int page = PageAtPoint(x, y);
  double px, py;
  AnnotHit hit;
  if (page >= 0 && ToPagePoint(page, x, y, px, py)) hit = doc_->AnnotAt(page, px, py);
  if (hit.kind != AnnotHit::Kind::Note) {
    if (hoverNote_.page >= 0) {
      hoverNote_ = HoverNote{};
      host_.ViewInvalidate();
    }
    return;
  }
  if (hoverNote_.page == page && hoverNote_.index == hit.index) return;
  hoverNote_ = HoverNote{page, hit.index, hit.rect, doc_->NoteText(page, hit.index), anim::Now()};
  anim::KeepAlive(hoverNote_.since + 420);  // the card fades in after a short pause
  host_.ViewInvalidate();
}

void PdfView::DrawNoteHover(Canvas& c) {
  if (hoverNote_.page < 0 || drag_ != Drag::None) return;
  const Palette& p = theme::P();
  const float u = ui_;
  D2D1_RECT_F iconRect = PageRectToScreen(hoverNote_.page, hoverNote_.rect);
  if (gfx::Width(iconRect) <= 0) return;
  // Soft ring around the icon right away.
  c.StrokeRound(gfx::Inflate(iconRect, 3 * u, 3 * u), 6 * u, gfx::Alpha(p.accent, 0.9f), 2 * u);

  double age = anim::Now() - hoverNote_.since;
  if (age < 250) return;
  float alpha = theme::AnimationsEnabled() ? static_cast<float>(std::min(1.0, (age - 250) / 150)) : 1.0f;

  const std::wstring& text = hoverNote_.text.empty() ? std::wstring(L"Empty note") : hoverNote_.text;
  const float w = 300 * u, pad = 14 * u, textSize = 13.5f * u;
  D2D1_RECT_F measure = gfx::RectF(0, 0, w - 2 * pad, 1000);
  float textH = c.TextWrapped(text, measure, p.text, textSize, 10, false);
  float h = std::ceil(pad + 20 * u + 8 * u + textH + 10 * u + 16 * u + pad);
  float x = iconRect.right + 10 * u, y = iconRect.top - 4 * u;
  if (x + w > vp_.right - 16 * u) x = iconRect.left - 10 * u - w;
  x = std::max(x, vp_.left + 8 * u);
  y = std::clamp(y, vp_.top + 8 * u, std::max(vp_.top + 8 * u, vp_.bottom - h - 8 * u));
  D2D1_RECT_F card = gfx::RectF(std::round(x), std::round(y), std::round(x + w), std::round(y + h));

  ID2D1DeviceContext* dc = c.DC();
  dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                      D2D1::IdentityMatrix(), alpha),
                nullptr);
  c.SoftShadow(card, 10 * u, 16 * u, p.popupShadow, 4 * u);
  c.FillRound(card, 10 * u, p.popupBg);
  c.StrokeRound(card, 10 * u, p.popupBorder, 1);
  // Header: amber badge with the note glyph, then "Note".
  float hy = card.top + pad;
  D2D1_RECT_F badge = gfx::RectF(card.left + pad, hy, card.left + pad + 20 * u, hy + 20 * u);
  c.FillRound(badge, 5 * u, gfx::Rgb(0xFFB900));
  c.Icon(icon::Comment, badge, gfx::Rgb(0x1B1B1B), 11 * u);
  c.Text(L"Note", gfx::RectF(badge.right + 8 * u, hy, card.right - pad, hy + 20 * u), p.textSecondary, 12 * u,
         DWRITE_FONT_WEIGHT_SEMI_BOLD);
  float ty = hy + 20 * u + 8 * u;
  c.TextWrapped(text, gfx::RectF(card.left + pad, ty, card.right - pad, ty + textH + 2),
                hoverNote_.text.empty() ? p.textSecondary : p.text, textSize, 10);
  c.Text(L"Click to edit", gfx::RectF(card.left + pad, card.bottom - pad - 16 * u, card.right - pad, card.bottom - pad),
         p.textSecondary, 11.5f * u);
  dc->PopLayer();
  if (alpha < 1) anim::KeepAlive(anim::Now() + 20);
}

// Night reading inverts the pages, which would turn the amber note icons brown. Redraw them on top in
// their real colors, using the same shape as the icon stored in the PDF (see NoteAppearance).
void PdfView::DrawNoteIconsUninverted(Canvas& c, int first, int last) {
  ID2D1DeviceContext* dc = c.DC();
  for (const NoteInfo& n : notes_) {
    if (n.page < first || n.page > last) continue;
    D2D1_RECT_F r = PageRectToScreen(n.page, n.rect);
    float s = gfx::Width(r) / 20.0f;
    if (s <= 0) continue;
    const float l = r.left + 1 * s, t = r.top + 1 * s, rr = r.left + 19 * s, b = r.top + 15 * s;
    ComPtr<ID2D1PathGeometry> geo;
    gfx::D2D()->CreatePathGeometry(&geo);
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    const float rad = 4 * s;
    sink->BeginFigure({l + rad, t}, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine({rr - rad, t});
    sink->AddArc(D2D1::ArcSegment({rr, t + rad}, {rad, rad}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddLine({rr, b - rad});
    sink->AddArc(D2D1::ArcSegment({rr - rad, b}, {rad, rad}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddLine({l + 8 * s, b});
    sink->AddLine({l + 3.5f * s, b + 4 * s});  // the speech-bubble tail
    sink->AddLine({l + 3 * s, b});
    sink->AddLine({l + rad, b});
    sink->AddArc(D2D1::ArcSegment({l, b - rad}, {rad, rad}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddLine({l, t + rad});
    sink->AddArc(D2D1::ArcSegment({l + rad, t}, {rad, rad}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    // Same shape as the inverted icon underneath, so it covers it exactly (the outline hides edge halos).
    dc->FillGeometry(geo.Get(), c.Brush(gfx::Rgb(0xFFB900)));
    dc->DrawGeometry(geo.Get(), c.Brush(gfx::Rgb(0xFFB900)), 1.0f);
    const float lx = l + 3.5f * s;
    const float widths[3] = {11, 11, 7};
    for (int i = 0; i < 3; ++i) {
      float ly = t + (3.4f + 3.4f * i) * s;
      c.Fill(gfx::RectF(lx, ly, lx + widths[i] * s, ly + 1.6f * s), gfx::Rgb(0xFFFFFF));
    }
  }
}

// ---------------------------------------------------------------------------
// Keys, commands, toolbar and menus

namespace {

// The reader's commands as command search and the toolbar name them, with the reader's keys.
struct CommandName {
  int cmd;
  const wchar_t* label;
  const wchar_t* keys;
};

constexpr CommandName kCommandNames[] = {
    {CmdGoToPage, L"Go to page", L"Ctrl+G"},
    {CmdNextPage, L"Next page", L"Page Down"},
    {CmdPrevPage, L"Previous page", L"Page Up"},
    {CmdFirstPage, L"First page", L"Home"},
    {CmdLastPage, L"Last page", L"End"},
    {CmdZoomIn, L"Zoom in", L"Ctrl+="},
    {CmdZoomOut, L"Zoom out", L"Ctrl+−"},
    {CmdZoomAuto, L"Zoom automatically", L"Ctrl+0"},
    {CmdZoomActual, L"Actual size", L"Ctrl+1"},
    {CmdZoomFitPage, L"Fit page", L"Ctrl+2"},
    {CmdZoomFitWidth, L"Fit width", L"Ctrl+3"},
    {CmdViewSingle, L"Single page", L"Ctrl+6"},
    {CmdViewContinuous, L"Continuous pages", L"Ctrl+7"},
    {CmdViewFacing, L"Two pages", L"Ctrl+8"},
    {CmdToggleCover, L"Show cover page on its own", L""},
    {CmdRotateRight, L"Rotate clockwise", L"Ctrl+Shift+="},
    {CmdRotateLeft, L"Rotate counterclockwise", L"Ctrl+Shift+−"},
    {CmdPresentation, L"Present", L"F5"},
    {CmdToggleInvert, L"Night reading: dark pages", L"Ctrl+I"},
    {CmdCopy, L"Copy", L"Ctrl+C"},
    {CmdSelectAll, L"Select all", L"Ctrl+A"},
    {CmdSelectAllOnPage, L"Select all text on this page", L""},
    {CmdHighlight, L"Highlight selected text", L""},
};

}  // namespace

int PdfView::Shortcut(UINT vk, UINT mods) {
  const bool ctrl = (mods & kModCtrl) != 0, shift = (mods & kModShift) != 0;
  if ((mods & kModAlt) && !ctrl && (vk == VK_LEFT || vk == VK_RIGHT)) return vk == VK_LEFT ? CmdPrevPage : CmdNextPage;
  if (ctrl) {
    switch (vk) {
      case 'G': return CmdGoToPage;
      case 'I': return CmdToggleInvert;
      case VK_OEM_PLUS:
      case VK_ADD: return shift ? CmdRotateRight : CmdZoomIn;
      case VK_OEM_MINUS:
      case VK_SUBTRACT: return shift ? CmdRotateLeft : CmdZoomOut;
      case '0':
      case VK_NUMPAD0: return CmdZoomAuto;
      case '1': return CmdZoomActual;
      case '2': return CmdZoomFitPage;
      case '3': return CmdZoomFitWidth;
      case '6': return CmdViewSingle;
      case '7': return CmdViewContinuous;
      case '8': return CmdViewFacing;
    }
  }
  if (vk == VK_F5) return CmdPresentation;
  return 0;
}

CommandState PdfView::QueryCommand(int cmd) {
  CommandState s;
  switch (cmd) {
    case CmdSave: s = {Modified(), false}; break;
    case CmdPrint: s = {Ready() && CanPrint(), false}; break;
    case CmdCopy:
    case CmdHighlight:
      s = PagedView::QueryCommand(cmd);
      s.enabled = s.enabled && (HasSelection() || (cmd == CmdCopy && HasKeyFocus()));
      break;
    default: s = PagedView::QueryCommand(cmd); break;
  }
  for (const CommandName& n : kCommandNames) {
    if (n.cmd == cmd) {
      s.label = n.label;
      s.shortcut = n.keys;
    }
  }
  return s;
}

std::vector<int> PdfView::Commands() {
  std::vector<int> ids;
  for (const CommandName& n : kCommandNames) ids.push_back(n.cmd);
  return ids;
}

bool PdfView::Execute(int cmd) {
  switch (cmd) {
    case CmdCopy: Copy(); return true;
    case CmdCut: FieldKey('X', kModCtrl); return true;
    case CmdPaste: FieldKey('V', kModCtrl); return true;
    case CmdSelectAll: SelectAll(); return true;
    case CmdSelectAllOnPage: SelectAllOnPage(); return true;
    case CmdHighlight: HighlightSelection(); return true;
    case CmdAddNote: {
      NoteTarget note;
      if (NewNoteAt(contextPt_.x, contextPt_.y, note)) pdfHost_.ViewOpenNote(this, note);
      return true;
    }
    case CmdOpenNote:
      if (contextAnnot_.kind == AnnotHit::Kind::Note)
        pdfHost_.ViewOpenNote(this, NoteTarget{contextPage_, contextAnnot_.index, contextAnnot_.rect.left,
                                               contextAnnot_.rect.top});
      return true;
    case CmdRemoveAnnot:
      if (contextAnnot_.kind != AnnotHit::Kind::None) RemoveAnnot(contextPage_, contextAnnot_.index);
      return true;
  }
  return PagedView::Execute(cmd);
}

void PdfView::BuildToolbar(ToolbarModel& m, const ToolbarContext& ctx) {
  // Items without a tooltip are named after their command, with its keys ("Save (Ctrl+S)").
  auto item = [](int cmd, wchar_t glyph, const wchar_t* tooltip = L"") {
    ToolItem t;
    t.cmd = cmd;
    t.glyph = glyph;
    t.tooltip = tooltip;
    return t;
  };
  ToolItem save = item(CmdSave, icon::Save);
  save.enabled = Modified();
  save.joinNext = true;
  ToolItem saveAs = item(CmdSaveMenu, icon::ChevronDown, L"Save as\u2026 (Ctrl+Shift+S)");
  saveAs.width = 18;
  saveAs.glyphSize = 9;
  ToolItem print = item(CmdPrint, icon::Print, CanPrint() ? L"" : L"This PDF doesn’t allow printing");
  print.enabled = CanPrint();
  m.left = {save, saveAs, print};

  m.pager = true;
  m.page = currentPage_;
  m.pageCount = PageCount();
  m.zoomText = fw::FormatPercent(Zoom());

  wchar_t viewGlyph = viewMode_ == fw::ViewMode::Single   ? icon::ViewSingle
                      : viewMode_ == fw::ViewMode::Facing ? icon::ViewFacing
                                                          : icon::ViewContinuous;
  ToolItem view = item(CmdViewMenu, viewGlyph, L"Page layout");
  view.width = 52;
  view.dropdown = true;
  view.priority = 1;
  ToolItem rotate = item(CmdRotateRight, icon::Rotate, L"Rotate (Ctrl+Shift+=)");
  rotate.priority = 0;
  ToolItem find = item(CmdFind, icon::Search);
  find.checked = ctx.searchOpen;
  find.toggle = true;
  find.priority = 5;
  find.sepBefore = true;
  ToolItem night = item(CmdToggleInvert, icon::QuietHours);
  night.checked = ctx.inverted;
  night.toggle = true;
  night.priority = 3;
  ToolItem full = item(CmdFullScreen, ctx.fullscreen ? icon::BackToWindow : icon::FullScreen);
  full.priority = 2;
  m.right = {view, rotate, find, night, full};
}

void PdfView::DescribeUia(uia::Tree& t, int parent) {
  if (state_ == LoadState::NeedPassword) {
    uia::AddTextBox(t, parent, 1, pwBox_, L"Password", [this] { host_.ViewFocusTextBox(&pwBox_); });
    uia::AddButton(t, parent, 2, pwOpen_, [this] { SubmitPassword(); });
    return;
  }
  if (selBar_ && !selection_.Empty() && drag_ != Drag::Select) {
    uia::AddButton(t, parent, 3, selHighlight_, [this] {
      HighlightSelection();
      host_.ViewInvalidate();
    });
    uia::AddButton(t, parent, 4, selCopy_, [this] { Copy(); });
  }
}

void PdfView::FillContextMenu(HMENU m, int x, int y) {
  using menu::AppendItem;
  using menu::AppendSeparator;
  if (HasKeyFocus()) {
    AppendItem(m, CmdCut, L"Cut\tCtrl+X");
    AppendItem(m, CmdCopy, L"Copy\tCtrl+C");
    AppendItem(m, CmdPaste, L"Paste\tCtrl+V");
    AppendSeparator(m);
    AppendItem(m, CmdSelectAll, L"Select all\tCtrl+A");
    return;
  }
  contextPt_ = POINT{x, y};
  contextAnnot_ = AnnotAtPoint(x, y, contextPage_);
  if (HasSelection()) {
    AppendItem(m, CmdHighlight, L"Highlight");
    AppendItem(m, CmdCopy, L"Copy\tCtrl+C");
  }
  if (contextAnnot_.kind == AnnotHit::Kind::Highlight) {
    AppendItem(m, CmdRemoveAnnot, L"Remove highlight");
  } else if (contextAnnot_.kind == AnnotHit::Kind::Note) {
    AppendItem(m, CmdOpenNote, L"Open note");
    AppendItem(m, CmdRemoveAnnot, L"Delete note");
  } else if (contextPage_ >= 0) {
    AppendItem(m, CmdAddNote, L"Add note here");
  }
  AppendSeparator(m);
  AppendItem(m, CmdSelectAllOnPage, L"Select all text on this page");
  AppendSeparator(m);
  AppendItem(m, CmdZoomIn, L"Zoom in\tCtrl+=");
  AppendItem(m, CmdZoomOut, L"Zoom out\tCtrl+−");
  AppendItem(m, CmdZoomFitWidth, L"Fit width\tCtrl+3");
  AppendItem(m, CmdZoomFitPage, L"Fit page\tCtrl+2");
  AppendSeparator(m);
  AppendItem(m, CmdRotateRight, L"Rotate clockwise\tCtrl+Shift+=");
  AppendItem(m, CmdRotateLeft, L"Rotate counterclockwise\tCtrl+Shift+−");
  AppendSeparator(m);
  AppendItem(m, CmdFind, L"Find…\tCtrl+F");
  AppendItem(m, CmdPrint, L"Print…\tCtrl+P", CanPrint());
}

PrintOutcome PdfView::Print(HWND owner, const std::function<void(int, int)>& progress, std::wstring& message) {
  return print::PrintDocument(owner, *doc_, CurrentPage(), Title(), progress, message);
}
