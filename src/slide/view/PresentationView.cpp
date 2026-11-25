// The presentation tab: its parts, drawing, thumbnails at idle, and the mouse and keys.

#include "slide/view/PresentationView.h"
#include "slide/view/ObjectTool.h"

#include <dwrite.h>

#include <algorithm>
#include <cmath>

#include "Commands.h"
#include "Util.h"
#include "core/Lang.h"
#include "core/Strings.h"
#include "draw/D2DPainter.h"
#include "slide/show/SlideRenderer.h"
#include "slide/edit/Edit.h"
#include "slide/show/SlideShow.h"
#include "slide/view/ModelDeck.h"
#include "slide/view/PresentationText.h"
#include "slide/view/NewDeck.h"
#include "slide/view/PresentationFile.h"
#include "PasswordCard.h"
#include "pkg/OfficeCrypto.h"
#include "slide/view/PresentationComments.h"
#include "slide/view/PresentationPanels.h"
#include "slide/view/PresentationAnim.h"  // p3-anim
#include "text/Fonts.h"
#include "ui/Anim.h"
#include "ui/StatusBar.h"
#include "ui/Theme.h"

using Microsoft::WRL::ComPtr;
using namespace gfx;

namespace fw::slide::view {

namespace {

constexpr float kMargin = 24;         // DIPs around the slide in the view
constexpr float kSplitter = 6;        // DIPs: the bar above the notes, dragged to size them
constexpr float kMinNotes = 40;       // DIPs
constexpr float kNarrow = 560;        // DIPs: below this the thumbnails pane hides
constexpr UINT kMaxBitmap = 4096;     // the largest slide bitmap kept; bigger zooms draw directly
constexpr float kPtToDip = 96.0f / 72.0f;

// A context for drawing off-screen on the same device as `dc`, so its bitmaps draw on `dc`.
ID2D1DeviceContext* OffscreenContext(ID2D1DeviceContext* dc) {
  static ComPtr<ID2D1DeviceContext> ctx;
  static ID2D1Device* of = nullptr;
  ComPtr<ID2D1Device> device;
  dc->GetDevice(&device);
  if (!device) return nullptr;
  if (!ctx || of != device.Get()) {
    ctx.Reset();
    of = nullptr;
    if (FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &ctx))) return nullptr;
    of = device.Get();
  }
  return ctx.Get();
}

}  // namespace

PresentationView::PresentationView(ViewHost& host, std::wstring path)
    : host_(host), path_(std::move(path)), text_(std::make_unique<TextEditing>(host)) {}

PresentationView::PresentationView(ViewHost& host, std::wstring path, std::unique_ptr<Deck> deck)
    : host_(host), path_(std::move(path)), deck_(std::move(deck)), text_(std::make_unique<TextEditing>(host)) {
  if (deck_) thumbCache_.resize(deck_->SlideCount());
  text_->Attach(deck_.get(), deck_ ? deck_->Editor() : nullptr);  // p2-text
  selection_.Reset(SlideCount(), 0);
}

PresentationView::~PresentationView() {
  // The show draws this tab's deck: it ends before the deck goes.
  if (showing_ && fw::slide::show::Running()) fw::slide::show::Stop();
}

std::unique_ptr<PresentationView> PresentationView::NewPresentation(ViewHost& host, int number) {
  auto view = std::make_unique<PresentationView>(host, std::wstring());
  view->untitled_ = T(L"Presentation") + std::to_wstring(number);
  view->newDeck_ = true;
  view->File().New(number);
  return view;
}

OpenResult PresentationView::Load(const std::wstring& password, std::wstring& error) {
  if (deck_) return OpenResult::Ok;
  std::wstring why;
  pkg::crypto::Key key;
  // A PowerPoint 97-2003 presentation (P3, p3-ppt) is converted, and the tab is a copy that Save As
  // saves as a .pptx beside it: the .ppt itself is never written. An encrypted one asks for its
  // password on the password card, and the .pptx it's saved as keeps the password.
  std::vector<uint8_t> converted;
  bool locked = false;
  bool odf = false;
  if (!newDeck_ && !fromBytes_ && protected_.empty() && ReadLegacyPresentation(path_, password, converted, why, locked)) {
    if (converted.empty()) {
      if (locked && why.empty()) {
        needPassword_ = true;
        return password.empty() ? OpenResult::NeedPassword : OpenResult::WrongPassword;
      }
      error = Title() + L": " + why;
      return OpenResult::Failed;
    }
    needPassword_ = false;
    if (locked) key = pkg::crypto::NewKey(password);
    imported_ = Title();
    deck_ = OpenDeckFromBytes(std::move(converted), why);
  } else if (!protected_.empty() || TakeProtected()) {
    // p3-review: a password-protected presentation (or a recovered one) is a compound file, read whole
    // and opened decrypted; until its password is given the tab shows the password card.
    std::vector<uint8_t> plain;
    if (OpenResult r = Unlock(password, plain, key, error); r != OpenResult::Ok) return r;
    deck_ = OpenDeckFromBytes(std::move(plain), why);
  } else {
    odf = !newDeck_ && !fromBytes_ && IsOpenDocument(path_);
    deck_ = newDeck_     ? OpenDeckFromBytes(BlankPresentation(), why)
            : fromBytes_ ? OpenDeckFromBytes(std::move(bytes_), why)
            : odf        ? OpenOpenDocument(path_, why)
                         : OpenDeck(path_, why);
    if (odf) imported_ = Title();  // P3 (p3-files): an OpenDocument presentation is converted too
  }
  if (!deck_) {
    error = Title() + L": " + why;
    return OpenResult::Failed;
  }
  if (newNumber_) {  // a template: the tab is a new presentation from it, saved with Save As
    untitled_ = T(L"Presentation") + std::to_wstring(newNumber_);
    path_.clear();
  }
  FileOpened();
  if (!key.Empty()) {
    File().Protect(std::move(key));  // saved, backed up and autosaved encrypted with it
    protected_.clear();
    protected_.shrink_to_fit();
  }
  thumbCache_.assign(deck_->SlideCount(), {});
  selection_.Reset(SlideCount(), 0);
  // A slide show file starts in the show; Esc comes back here (plan §8.1). Scripted runs
  // (--snapshot, which keep a clipboard of their own) stay in the tab, so their steps run on it.
  showPending_ = (deck_->StartsAsShow() || EndsWithNoCase(path_, L".ppsx") || EndsWithNoCase(path_, L".pps")) &&
                 !util::UsingPrivateClipboard();
  if (!imported_.empty() && !newNumber_) {  // a .pot or .otp is a template: a new presentation, as above
    const size_t dot = path_.find_last_of(L'.');
    const std::wstring stem = FileNameOf(dot == std::wstring::npos ? path_ : path_.substr(0, dot));
    File().Copy(imported_, (dot == std::wstring::npos ? path_ : path_.substr(0, dot)) + L".pptx", false);
    host_.ViewMessage(
        BannerKind::Info,
        Fmt(odf ? T(L"{0} is an OpenDocument presentation. Saving makes a new {1}.pptx and leaves it as it is.")
                : T(L"{0} is a PowerPoint 97–2003 presentation. Saving makes a new {1}.pptx and leaves it as it is."),
            {imported_, stem}));
    untitled_ = imported_;
    path_.clear();  // Save is Save As
  }
  SlideEditor();  // P2: the deck's Editor made and listened to before anything can edit it (OnEditorMade)
  text_->Attach(deck_.get(), SlideEditor());  // p2-text: after the tab's listener; read-only without an editor
  LayoutParts();
  return OpenResult::Ok;
}

bool PresentationView::IsTemplate(const std::wstring& path) {
  return EndsWithNoCase(path, L".potx") || EndsWithNoCase(path, L".potm") || EndsWithNoCase(path, L".pot") ||
         EndsWithNoCase(path, L".otp");  // .pot: p3-ppt, .otp: p3-files
}

std::wstring PresentationView::Title() const { return path_.empty() && !untitled_.empty() ? untitled_ : FileNameOf(path_); }

// ---------------------------------------------------------------------------
// Layout

void PresentationView::SetViewport(const RECT& rc, float ui) {
  const bool resized = rc.right - rc.left != vp_.right - vp_.left || rc.bottom - rc.top != vp_.bottom - vp_.top || ui != ui_;
  vp_ = rc;
  ui_ = ui;
  LayoutParts();
  if (resized) slideBitmap_.Reset();
}

void PresentationView::LayoutParts() {
  const float l = static_cast<float>(vp_.left), t = static_cast<float>(vp_.top);
  const float r = static_cast<float>(vp_.right), b = static_cast<float>(vp_.bottom);
  const bool narrow = r - l < kNarrow * ui_;
  const float tw = narrow ? 0 : std::round(thumbWidth_ * ui_);
  thumbs_ = {l, t, l + tw, b};
  const float x0 = l + tw + (tw > 0 ? std::max(1.0f, ui_) : 0);
  if (notesShown_) {
    const float maxNotes = std::max(kMinNotes * ui_, (b - t) * 0.6f);
    const float nh = std::clamp(std::round(notesHeight_ * ui_), kMinNotes * ui_, maxNotes);
    notes_ = {x0, b - nh, r, b};
    splitter_ = {x0, notes_.top - kSplitter * ui_, r, notes_.top};
    view_ = {x0, t, r, splitter_.top};
  } else {
    notes_ = splitter_ = {x0, b, r, b};
    view_ = {x0, t, r, b};
  }
  // Thumbnail bitmaps follow the pane's width.
  if (deck_) {
    const std::vector<ThumbRow> rows = ThumbRows(1, deck_->SlideSize(), {Width(thumbs_), ui_});
    const UINT w = rows.empty() ? 0 : static_cast<UINT>(std::lround(rows[0].slide.Width()));
    const UINT h = rows.empty() ? 0 : static_cast<UINT>(std::lround(rows[0].slide.Height()));
    if (w != thumbW_ || h != thumbH_) {
      thumbW_ = w;
      thumbH_ = h;
      for (Thumb& th : thumbCache_) th.bitmap.Reset();
    }
  }
  ClampScroll();
}

float PresentationView::FitScale() const {
  if (!deck_) return 1;
  const draw::Size s = deck_->SlideSize();
  const float m = kMargin * ui_;
  const float w = Width(view_) - 2 * m, h = Height(view_) - 2 * m;
  if (s.width <= 0 || s.height <= 0 || w <= 0 || h <= 0) return 0.01f;
  return std::max(0.01f, std::min(w / s.width, h / s.height));
}

float PresentationView::Scale() const {
  return zoom_ > 0 ? static_cast<float>(zoom_) * kPtToDip * ui_ : FitScale();
}

D2D1_RECT_F PresentationView::SlideRect() const {
  if (!deck_) return {};
  const draw::Size s = deck_->SlideSize();
  const float k = Scale(), m = kMargin * ui_;
  const float w = std::round(s.width * k), h = std::round(s.height * k);
  float x = Width(view_) - 2 * m >= w ? view_.left + std::round((Width(view_) - w) / 2) : view_.left + m - sx_;
  float y = Height(view_) - 2 * m >= h ? view_.top + std::round((Height(view_) - h) / 2) : view_.top + m - sy_;
  return {x, y, x + w, y + h};
}

void PresentationView::ClampScroll() {
  if (!deck_) return;
  const draw::Size s = deck_->SlideSize();
  const float k = Scale(), m = kMargin * ui_;
  sx_ = std::clamp(sx_, 0.0f, std::max(0.0f, s.width * k + 2 * m - Width(view_)));
  sy_ = std::clamp(sy_, 0.0f, std::max(0.0f, s.height * k + 2 * m - Height(view_)));
  const std::vector<ThumbRow> rows = ThumbRows(SlideCount(), deck_->SlideSize(), {Width(thumbs_), ui_});
  thumbScroll_ = std::clamp(thumbScroll_, 0.0f, std::max(0.0f, ThumbListHeight(rows, ui_) - Height(thumbs_)));
}

void PresentationView::SetZoom(double zoom) {
  const float before = Scale();
  zoom_ = zoom <= 0 ? 0 : std::clamp(zoom, StatusBar::kMinZoom, StatusBar::kMaxZoom);
  // Zoom about the view's center.
  const float after = Scale();
  if (before > 0) {
    sx_ = (sx_ + Width(view_) / 2) * after / before - Width(view_) / 2;
    sy_ = (sy_ + Height(view_) / 2) * after / before - Height(view_) / 2;
  }
  ClampScroll();
  slideBitmap_.Reset();
  host_.ViewStateChanged();
}

std::vector<ThumbRow> PresentationView::Rows() const {
  std::vector<ThumbRow> rows =
      deck_ ? ThumbRows(SlideCount(), deck_->SlideSize(), {Width(thumbs_), ui_}) : std::vector<ThumbRow>();
  const float dx = thumbs_.left, dy = thumbs_.top - thumbScroll_;
  for (ThumbRow& r : rows)
    for (draw::Rect* rc : {&r.number, &r.slide, &r.row}) *rc = {rc->left + dx, rc->top + dy, rc->right + dx, rc->bottom + dy};
  return rows;
}

void PresentationView::ScrollThumbIntoView(size_t slide) {
  if (!deck_ || Width(thumbs_) <= 0) return;
  const std::vector<ThumbRow> rows = ThumbRows(SlideCount(), deck_->SlideSize(), {Width(thumbs_), ui_});
  if (slide >= rows.size()) return;
  const float top = rows[slide].row.top, bottom = rows[slide].row.bottom;
  if (top < thumbScroll_) thumbScroll_ = top;
  else if (bottom > thumbScroll_ + Height(thumbs_)) thumbScroll_ = bottom - Height(thumbs_);
  ClampScroll();
}

int PresentationView::ThumbAt(int x, int y) const {
  if (!Contains(thumbs_, static_cast<float>(x), static_cast<float>(y))) return -1;
  const std::vector<ThumbRow> rows = Rows();
  for (size_t i = 0; i < rows.size(); ++i)
    if (y >= rows[i].row.top && y < rows[i].row.bottom) return static_cast<int>(i);
  return -1;
}

void PresentationView::GoToSlide(size_t slide) {
  if (!deck_ || SlideCount() == 0) return;
  slide = std::min(slide, SlideCount() - 1);
  if (slide != current_) {
    text_->End();
    EndNotesEdit();  // P2: the notes typed go to the slide they were typed on
    current_ = slide;
    sx_ = sy_ = 0;
    notesScroll_ = 0;
    slideBitmap_.Reset();
  }
  // Going to a slide outside the selection selects it alone (P2).
  if (selection_.Count() != SlideCount() || !selection_.Selected(current_) || selection_.Current() != current_) {
    if (selection_.Count() == SlideCount() && selection_.Selected(current_)) selection_.Set(selection_.Indices(), current_);
    else selection_.Reset(SlideCount(), current_);
  }
  ScrollThumbIntoView(current_);
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

// ---------------------------------------------------------------------------
// Drawing

ComPtr<ID2D1Bitmap1> PresentationView::RenderBitmap(ID2D1DeviceContext* dc, size_t slide, UINT w, UINT h, bool editing) {
  return RenderBitmapWith(dc, w, h, [&](draw::Painter& p) {
    if (editing) {
      deck_->PaintFiltered(slide, p, text_->Filter(slide));
      deck_->PaintPrompts(slide, p, text_->EditingShape(slide));
    } else {
      deck_->Paint(slide, p);
    }
  });
}

ComPtr<ID2D1Bitmap1> PresentationView::RenderBitmapWith(ID2D1DeviceContext* dc, UINT w, UINT h,
                                                        const std::function<void(draw::Painter&)>& paint) {
  ID2D1DeviceContext* ctx = OffscreenContext(dc);
  if (!ctx || !deck_ || w == 0 || h == 0) return nullptr;
  ComPtr<ID2D1Bitmap1> bmp;
  const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
  if (FAILED(ctx->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, &bmp))) return nullptr;
  ctx->SetTarget(bmp.Get());
  const HRESULT hr = draw::DrawFrame(ctx, [&](bool fallback) {
    ctx->Clear(D2D1::ColorF(1, 1, 1, 1));
    {
      const draw::Size s = deck_->SlideSize();
      ctx->SetTransform(D2D1::Matrix3x2F::Scale(w / s.width, h / s.height));
      draw::D2DPainter painter(ctx);
      painter.UsePdfLikeDrawing();
      painter.SetNoShadows(fallback);
      paint(painter);
    }
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
  });
  ctx->SetTarget(nullptr);
  return SUCCEEDED(hr) ? bmp : nullptr;
}

ID2D1Bitmap* PresentationView::ThumbBitmap(ID2D1DeviceContext* dc, size_t slide, bool make) {
  if (slide >= thumbCache_.size()) return nullptr;
  Thumb& th = thumbCache_[slide];
  if (!th.bitmap && make) th.bitmap = RenderBitmap(dc, slide, thumbW_, thumbH_);
  return th.bitmap.Get();
}

void PresentationView::Draw(Canvas& c, ID2D1DeviceContext* dc) {
  const Palette& p = theme::P();
  c.Fill(RectF(vp_), p.docBg);
  if (needPassword_) {  // p3-review: the password card until the file opens
    Card().Draw(c, vp_, ui_, Title());
    return;
  }
  if (!deck_) return;
  if (!bannerShown_) {
    bannerShown_ = true;
    ReadOnlyBanner();
  }
  DrawThumbnails(c, dc);
  DrawSlide(c, dc);
  c.PushClip(view_);
  Objects().Draw(c, dc);  // p2-objects: frames, handles, guides, a drag's layers
  c.PopClip();
  DrawNotes(c);
  if (showPending_) {
    // After the first frame, so the tab is there to come back to.
    showPending_ = false;
    PostMessageW(host_.ViewHwnd(), WM_COMMAND, CmdSlideShowFromStart, 0);
  }
}

void PresentationView::DrawThumbnails(Canvas& c, ID2D1DeviceContext* dc) {
  if (Width(thumbs_) <= 0) return;
  const Palette& p = theme::P();
  c.Fill(thumbs_, p.chrome);
  c.Line(thumbs_.right, thumbs_.top, thumbs_.right, thumbs_.bottom, p.chromeBorder, std::max(1.0f, ui_));
  c.PushClip(thumbs_);
  const std::vector<ThumbRow> rows = Rows();
  for (size_t i = 0; i < rows.size(); ++i) {
    const ThumbRow& r = rows[i];
    if (r.row.bottom < thumbs_.top || r.row.top > thumbs_.bottom) continue;
    const D2D1_RECT_F row{r.row.left, r.row.top, r.row.right, r.row.bottom};
    const D2D1_RECT_F pic{r.slide.left, r.slide.top, r.slide.right, r.slide.bottom};
    if (static_cast<int>(i) == hoverThumb_ && i != current_) c.Fill(row, p.hover);
    DrawThumbSelection(c, rows, i);
    const bool hidden = deck_->Hidden(i);
    // The number; a hidden slide's is crossed out, as PowerPoint marks it.
    const D2D1_RECT_F num{r.number.left, r.number.top, r.number.right, r.number.bottom};
    const std::wstring label = std::to_wstring(i + 1);
    c.Text(label, num, i == current_ ? p.accentText : p.textSecondary, 12 * ui_,
           i == current_ ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_REGULAR, Align::Trailing);
    if (hidden) {
      const float tw = c.MeasureText(label, 12 * ui_), cy = (num.top + num.bottom) / 2;
      const D2D1_RECT_F box{num.right - tw - 3 * ui_, cy - 8 * ui_, num.right + 3 * ui_, cy + 8 * ui_};
      c.Stroke(box, p.textSecondary, std::max(1.0f, ui_));
      c.Line(box.left, box.bottom, box.right, box.top, p.textSecondary, std::max(1.0f, ui_));
    }
    c.SoftShadow(pic, 1.0f * ui_, 3.0f * ui_, Alpha(p.pageShadow, 0.25f));
    if (ID2D1Bitmap* bmp = ThumbBitmap(dc, i, false)) {
      dc->DrawBitmap(bmp, pic, hidden ? 0.45f : 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
    } else {
      c.Fill(pic, Alpha(Rgb(0xFFFFFF), hidden ? 0.45f : 1.0f));
    }
    if (i == current_) {
      c.StrokeRound(Inflate(pic, 2.5f * ui_, 2.5f * ui_), 3 * ui_, p.accent, 2 * ui_);
    } else if (selection_.Count() == rows.size() && selection_.Selected(i)) {  // P2: selected with it
      c.StrokeRound(Inflate(pic, 2.0f * ui_, 2.0f * ui_), 3 * ui_, Alpha(p.accent, 0.75f), 1.5f * ui_);
    } else {
      c.Stroke(pic, Alpha(p.chromeBorder, 0.8f), std::max(1.0f, ui_ * 0.75f));
    }
  }
  DrawDropMarker(c);
  c.PopClip();
}

void PresentationView::DrawSlide(Canvas& c, ID2D1DeviceContext* dc) {
  const Palette& p = theme::P();
  if (SlideCount() == 0) {
    c.Text(T(L"This presentation has no slides."), view_, p.textSecondary, 14 * ui_, DWRITE_FONT_WEIGHT_REGULAR,
           Align::Center);
    return;
  }
  const D2D1_RECT_F rc = SlideRect();
  c.PushClip(view_);
  c.SoftShadow(rc, 1.0f * ui_, 5.0f * ui_, Alpha(p.pageShadow, 0.35f), 1.0f * ui_);
  const UINT w = static_cast<UINT>(std::lround(Width(rc))), h = static_cast<UINT>(std::lround(Height(rc)));
  if (text_->TakeSlideChanged()) slideBitmap_.Reset();
  if (w <= kMaxBitmap && h <= kMaxBitmap) {
    if (!slideBitmap_ || slideBitmapOf_ != current_ || slideBitmapW_ != w || slideBitmapH_ != h) {
      slideBitmap_ = RenderBitmap(dc, current_, w, h, true);
      slideBitmapOf_ = current_;
      slideBitmapW_ = w;
      slideBitmapH_ = h;
    }
    if (slideBitmap_) dc->DrawBitmap(slideBitmap_.Get(), rc, 1, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
  } else {
    // Zoomed in far: only what's in view, drawn directly.
    c.Fill(rc, Rgb(0xFFFFFF));
    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);
    const float k = Width(rc) / deck_->SlideSize().width;
    dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(rc.left, rc.top) * old);
    {
      draw::D2DPainter painter(dc);
      painter.UsePdfLikeDrawing();
      deck_->PaintFiltered(current_, painter, text_->Filter(current_));
      deck_->PaintPrompts(current_, painter, text_->EditingShape(current_));
    }
    dc->SetTransform(old);
  }
  // p3-anim: a transition or the animations playing over the slide (Preview).
  if (preview_ && DrawPreview(c, dc)) {
    c.PopClip();
    return;
  }
  // The text being edited, live over the slide.
  text_->Draw(c, dc, current_, rc);
  text_->DrawSpelling(c, current_, rc, host_.ViewSpelling());
  // Find: the matches on this slide, the current one stronger.
  const float k = Width(rc) / std::max(1.0f, deck_->SlideSize().width);
  for (size_t i = 0; i < matches_.size(); ++i) {
    const Match& m = matches_[i];
    if (m.slide != current_ || m.notes) continue;
    std::vector<draw::Rect> rects = deck_->TextRects(m.slide, m.block, m.at, m.length);
    // Without the text's own rectangles, the shape holding the match is outlined instead.
    const bool outline = rects.empty();
    if (outline) {
      const std::vector<TextBlock> blocks = deck_->Text(m.slide);
      if (m.block < blocks.size()) rects.push_back(blocks[m.block].bounds);
    }
    const bool cur = static_cast<int>(i) == match_;
    for (const draw::Rect& r : rects) {
      const D2D1_RECT_F s{rc.left + r.left * k, rc.top + r.top * k, rc.left + r.right * k, rc.top + r.bottom * k};
      if (outline) {
        c.StrokeRound(Inflate(s, 2 * ui_, 2 * ui_), 3 * ui_, cur ? p.searchCurrent : p.searchHit, (cur ? 2.5f : 1.5f) * ui_);
        continue;
      }
      c.Fill(s, Alpha(cur ? p.searchCurrent : p.searchHit, 0.35f));
      if (cur) c.Stroke(s, p.searchCurrent, std::max(1.0f, ui_));
    }
  }
  Comments().DrawMarkers(c, rc);  // p3-review: the comments' markers, over everything
  c.PopClip();
}

void PresentationView::DrawNotes(Canvas& c) {
  if (!notesShown_ || Height(notes_) <= 0) return;
  const Palette& p = theme::P();
  c.Fill(splitter_, p.docBg);
  c.Line(splitter_.left, splitter_.bottom, splitter_.right, splitter_.bottom, p.chromeBorder, std::max(1.0f, ui_));
  c.Fill(notes_, p.chrome);
  const float pad = 12 * ui_, size = 13 * ui_;
  const D2D1_RECT_F box{notes_.left + pad, notes_.top + pad * 0.75f, notes_.right - pad, notes_.bottom - pad * 0.5f};
  if (notesEdit_ && notesEdit_->IsOpen()) {  // P2: being typed in (a window of its own over the pane)
    PlaceNotesEdit(box);
    return;
  }
  const std::wstring notes = SlideCount() ? deck_->Notes(current_) : std::wstring();
  if (notes.empty()) {
    // P2: an editable deck invites notes, as PowerPoint does.
    edit::Editor* ed = SlideCount() ? SlideEditor() : nullptr;
    c.Text(ed && ed->CanEdit() ? T(L"Click to add notes") : T(L"This slide has no speaker notes."),
           {box.left, box.top, box.right, box.top + 20 * ui_}, p.textDisabled, size);
    return;
  }
  ComPtr<IDWriteTextLayout> layout;
  if (FAILED(gfx::DWrite()->CreateTextLayout(notes.c_str(), static_cast<UINT32>(notes.size()),
                                             Canvas::Format(size, DWRITE_FONT_WEIGHT_REGULAR, false),
                                             std::max(1.0f, Width(box)), 1e6f, &layout)))
    return;
  layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
  layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  DWRITE_TEXT_METRICS tm{};
  layout->GetMetrics(&tm);
  notesScroll_ = std::clamp(notesScroll_, 0.0f, std::max(0.0f, tm.height - Height(box)));
  const float ox = box.left, oy = box.top - notesScroll_;
  c.PushClip(box);
  for (size_t i = 0; i < matches_.size(); ++i) {
    const Match& m = matches_[i];
    if (m.slide != current_ || !m.notes) continue;
    DWRITE_HIT_TEST_METRICS hits[16];
    UINT32 n = 0;
    if (FAILED(layout->HitTestTextRange(static_cast<UINT32>(m.at), static_cast<UINT32>(m.length), ox, oy, hits, 16, &n)))
      continue;
    for (UINT32 k = 0; k < n && k < 16; ++k)
      c.Fill({hits[k].left, hits[k].top, hits[k].left + hits[k].width, hits[k].top + hits[k].height},
             static_cast<int>(i) == match_ ? p.searchCurrent : p.searchHit);
  }
  c.DC()->DrawTextLayout(D2D1::Point2F(ox, oy), layout.Get(), c.Brush(p.text));
  c.PopClip();
}

// ---------------------------------------------------------------------------
// Idle work: the thumbnails near what's shown, the visible ones first.

bool PresentationView::HasIdleWork() const {
  // Nothing competes with the show's frames.
  if (fw::slide::show::Running()) return false;
  if (!deck_ || thumbW_ == 0 || Width(thumbs_) <= 0) return false;
  const std::vector<ThumbRow> rows = Rows();
  const float ahead = Height(thumbs_);
  for (size_t i = 0; i < rows.size(); ++i)
    if (rows[i].row.bottom >= thumbs_.top - ahead && rows[i].row.top <= thumbs_.bottom + ahead && !thumbCache_[i].bitmap)
      return true;
  return false;
}

bool PresentationView::IdleWork(ID2D1DeviceContext* dc, double deadlineMs) {
  if (!HasIdleWork()) return false;
  const std::vector<ThumbRow> rows = Rows();
  const float ahead = Height(thumbs_);
  bool drew = false;
  for (int pass = 0; pass < 2; ++pass)
    for (size_t i = 0; i < rows.size(); ++i) {
      const bool visible = rows[i].row.bottom >= thumbs_.top && rows[i].row.top <= thumbs_.bottom;
      const bool nearby = rows[i].row.bottom >= thumbs_.top - ahead && rows[i].row.top <= thumbs_.bottom + ahead;
      if ((pass == 0 ? !visible : !nearby) || thumbCache_[i].bitmap) continue;
      if (ThumbBitmap(dc, i, true)) drew = true;
      else thumbCache_[i].bitmap.Reset();
      if (anim::Now() >= deadlineMs) return drew;
    }
  return drew;
}

void PresentationView::ReleaseDeviceResources() {
  for (Thumb& th : thumbCache_) th.bitmap.Reset();
  slideBitmap_.Reset();
  if (objects_) objects_->ReleaseDeviceResources();
}

void PresentationView::OnDeactivated() {
  EndNotesEdit();  // P2: the notes typed go to the slide
  // Quick to rebuild: the slide's bitmap. The thumbnails stay, so coming back is instant.
  slideBitmap_.Reset();
}

// ---------------------------------------------------------------------------
// Mouse

void PresentationView::OnMouseDown(int x, int y, int button, UINT mods, bool doubleClick) {
  if (needPassword_) {  // p3-review
    Card().OnMouseDown(x, y, mods, doubleClick, ui_);
    return;
  }
  if (!deck_ || button != 0) return;
  const float fx = static_cast<float>(x), fy = static_cast<float>(y);
  downX_ = x;
  downY_ = y;
  if (Contains(Inflate(splitter_, 0, 2 * ui_), fx, fy) && notesShown_) {
    if (doubleClick) {
      Execute(CmdNotesPane);
      return;
    }
    drag_ = Drag::Splitter;
    downValue_ = notesHeight_;
    return;
  }
  if (const int t = ThumbAt(x, y); t >= 0) {
    keysInThumbs_ = true;
    text_->End();  // p2-text
    ThumbsMouseDown(t, mods, x, y);  // P2: selection, and a drag to move slides
    return;
  }
  if (Contains(thumbs_, fx, fy)) {
    keysInThumbs_ = true;
    text_->End();  // p2-text
    return;
  }
  keysInThumbs_ = false;
  if (notesShown_ && Contains(notes_, fx, fy) && NotesMouseDown()) return;  // P2: typing notes
  if (Contains(view_, fx, fy)) {
    if (Comments().MouseDown(fx, fy)) {  // p3-review: a comment's marker (picked, maybe dragged)
      text_->End();
      return;
    }
    if (const SlideLink* link = LinkAt(x, y); link && (mods & kModCtrl)) {
      FollowLink(*link);
      return;
    }
    // A handle of the selection, a selected shape's frame edge or an object without text is the
    // objects'; then text (a click in a text body edits it); then the objects' selection.
    if (Objects().WantsPress(fx, fy) && Objects().MouseDown(fx, fy, mods, doubleClick)) {  // p2-objects
      text_->End();
      Objects().SetTextEditing(false);
      return;
    }
    if (text_->MouseDown(current_, SlidePoint(x, y), doubleClick, mods)) {  // p2-text
      Objects().SelectForText(text_->Session()->Target().shape);
      Objects().SetTextEditing(true);
      drag_ = Drag::Text;
      return;
    }
    Objects().SetTextEditing(false);
    if (Objects().MouseDown(fx, fy, mods, doubleClick)) return;  // p2-objects
    const D2D1_RECT_F rc = SlideRect();
    if (Width(rc) > Width(view_) || Height(rc) > Height(view_)) {
      drag_ = Drag::Pan;
      downValue_ = sx_;
      downValue2_ = sy_;
    }
  }
}

void PresentationView::OnMouseMove(int x, int y, UINT mods) {
  if (needPassword_) {  // p3-review
    Card().OnMouseMove(x, y, ui_);
    return;
  }
  if (!deck_) return;
  if (commentsPanel_ && commentsPanel_->MouseMove(static_cast<float>(x), static_cast<float>(y))) return;  // p3-review
  if (objects_ && objects_->Dragging()) {  // p2-objects
    objects_->MouseMove(static_cast<float>(x), static_cast<float>(y), mods);
    return;
  }
  switch (drag_) {
    case Drag::Splitter:
      notesHeight_ = std::max(kMinNotes, downValue_ - static_cast<float>(y - downY_) / ui_);
      LayoutParts();
      slideBitmap_.Reset();
      host_.ViewInvalidate();
      return;
    case Drag::Text:
      text_->MouseMove(SlidePoint(x, y));
      return;
    case Drag::Pan:
      sx_ = downValue_ - static_cast<float>(x - downX_);
      sy_ = downValue2_ - static_cast<float>(y - downY_);
      ClampScroll();
      host_.ViewInvalidate();
      return;
    case Drag::Slides: ThumbsDragMove(x, y); return;
    default:
      if (thumbPress_) {
        ThumbsDragMove(x, y);
        if (drag_ == Drag::Slides) return;
      }
      break;
  }
  const int hover = ThumbAt(x, y);
  if (hover != hoverThumb_) {
    hoverThumb_ = hover;
    host_.ViewInvalidate();
  }
}

void PresentationView::OnMouseUp(int x, int y, int /*button*/, UINT mods) {
  if (needPassword_) {  // p3-review
    Card().OnMouseUp(x, y);
    return;
  }
  if (commentsPanel_ && commentsPanel_->MouseUp(static_cast<float>(x), static_cast<float>(y))) return;  // p3-review
  if (objects_ && objects_->Dragging()) {  // p2-objects
    objects_->MouseUp(static_cast<float>(x), static_cast<float>(y), mods);
    return;
  }
  if (drag_ == Drag::Splitter) {
    // Dragged nearly shut: the notes close, as PowerPoint's do.
    const float maxNotes = std::max(kMinNotes, (vp_.bottom - vp_.top) / ui_ * 0.6f);
    notesHeight_ = std::min(notesHeight_, maxNotes);
  }
  if (drag_ == Drag::Slides || thumbPress_) ThumbsDragEnd(drag_ == Drag::Slides);
  if (drag_ == Drag::Text) text_->MouseUp();  // r1-slide: the format painter
  drag_ = Drag::None;
}

void PresentationView::OnMouseLeave() {
  if (needPassword_) Card().OnMouseLeave();  // p3-review
  if (hoverThumb_ >= 0) {
    hoverThumb_ = -1;
    host_.ViewInvalidate();
  }
}

void PresentationView::OnWheel(int delta, bool horizontal, UINT mods, int x, int y) {
  if (!deck_) return;
  const float fx = static_cast<float>(x), fy = static_cast<float>(y);
  const float step = static_cast<float>(delta) / WHEEL_DELTA * 48 * ui_;
  if (Contains(thumbs_, fx, fy)) {
    thumbScroll_ -= step * 2;
    ClampScroll();
    host_.ViewInvalidate();
    return;
  }
  if (Contains(notes_, fx, fy)) {
    notesScroll_ -= step;
    host_.ViewInvalidate();
    return;
  }
  if (mods & kModCtrl) {
    const double now = zoom_ > 0 ? zoom_ : Scale() / (kPtToDip * ui_);
    SetZoom(now * (delta > 0 ? 1.1 : 1 / 1.1));
    host_.ViewInvalidate();
    return;
  }
  const D2D1_RECT_F rc = SlideRect();
  const bool scrolls = horizontal ? Width(rc) > Width(view_) : Height(rc) > Height(view_);
  if (scrolls) {
    (horizontal ? sx_ : sy_) -= step * (horizontal ? -1 : 1);
    ClampScroll();
    host_.ViewInvalidate();
    return;
  }
  // Fitted: the wheel goes through the slides, a notch at a time, as in PowerPoint.
  if (horizontal) return;
  if (delta < 0 && current_ + 1 < SlideCount()) GoToSlide(current_ + 1);
  else if (delta > 0 && current_ > 0) GoToSlide(current_ - 1);
}

bool PresentationView::OnKeyDown(UINT vk, UINT mods) {
  if (!deck_) return false;
  if (keysInThumbs_ && ThumbsKeyDown(vk, mods)) return true;  // P2: the thumbnails' keys
  if (!keysInThumbs_ && text_->KeyDown(vk, mods)) {  // p2-text: the text being edited
    if (!text_->Editing()) {
      Objects().SetTextEditing(false);
      if (const uint32_t left = text_->TakeLeft()) Objects().Select({left});  // Esc: the shape stays selected
    }
    return true;
  }
  if (!keysInThumbs_ && Objects().KeyDown(vk, mods)) return true;  // p2-objects
  if (mods & (kModCtrl | kModAlt)) return false;
  switch (vk) {
    case VK_UP:
    case VK_LEFT:
      if (current_ > 0) GoToSlide(current_ - 1);
      return true;
    case VK_DOWN:
    case VK_RIGHT:
      if (current_ + 1 < SlideCount()) GoToSlide(current_ + 1);
      return true;
    default: return false;
  }
}

bool PresentationView::OnChar(wchar_t ch, UINT mods) {
  if (!deck_) return false;
  // Typing with one shape selected types into it, replacing its text, as PowerPoint (p2-objects, p2-text).
  if (!text_->Editing() && !keysInThumbs_ && ch >= 0x20 && !(mods & (kModCtrl | kModAlt)) && Ready())
    if (const uint32_t id = Objects().SelectedShape(); id && text_->TypeInto(current_, id)) {
      Objects().SelectForText(id);
      Objects().SetTextEditing(true);
    }
  return text_->Char(ch, mods);
}

bool PresentationView::HasKeyFocus() const { return text_ && text_->Editing(); }

std::optional<RECT> PresentationView::CaretRect() { return text_->CaretRect(SlideRect()); }

draw::Point PresentationView::SlidePoint(int x, int y) const {
  const D2D1_RECT_F rc = SlideRect();
  const float k = deck_ && deck_->SlideSize().width > 0 ? Width(rc) / deck_->SlideSize().width : 1;
  return {(static_cast<float>(x) - rc.left) / k, (static_cast<float>(y) - rc.top) / k};
}

HCURSOR PresentationView::CursorAt(int x, int y) {
  if (needPassword_) return Card().CursorAt(x, y);  // p3-review
  static HCURSOR arrow = LoadCursor(nullptr, IDC_ARROW), ns = LoadCursor(nullptr, IDC_SIZENS),
                 hand = LoadCursor(nullptr, IDC_HAND), all = LoadCursor(nullptr, IDC_SIZEALL);
  if (drag_ == Drag::Splitter || (notesShown_ && Contains(Inflate(splitter_, 0, 2 * ui_), (float)x, (float)y))) return ns;
  if (drag_ == Drag::Pan) return all;
  static HCURSOR beam = LoadCursor(nullptr, IDC_IBEAM);
  if (drag_ == Drag::Text) return beam;
  if (deck_ && (Contains(view_, (float)x, (float)y) || Objects().Dragging())) {  // p2-objects, p2-text
    const bool objectsFirst = Objects().Dragging() || Objects().WantsPress(static_cast<float>(x), static_cast<float>(y));
    if (!objectsFirst && text_->OverText(current_, SlidePoint(x, y))) return beam;
    if (HCURSOR h = Objects().Cursor(static_cast<float>(x), static_cast<float>(y))) return h;
  }
  if (LinkAt(x, y)) return hand;
  return arrow;
}

std::wstring PresentationView::TooltipAt(int x, int y) {
  if (commentsPanel_ && Contains(view_, static_cast<float>(x), static_cast<float>(y)))  // p3-review
    if (std::wstring tip = commentsPanel_->Tooltip(static_cast<float>(x), static_cast<float>(y)); !tip.empty()) return tip;
  if (const SlideLink* link = LinkAt(x, y)) {
    std::wstring tip = link->tooltip.empty() ? link->url : link->tooltip;
    return tip + (tip.empty() ? L"" : L"\n") + T(L"Ctrl+click to follow the link");
  }
  if (const int t = ThumbAt(x, y); t >= 0 && deck_->Hidden(static_cast<size_t>(t)))
    return Fmt(T(L"Slide {0} is hidden: the slide show skips it"), {std::to_wstring(t + 1)});
  return {};
}

}  // namespace fw::slide::view
