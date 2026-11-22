// The presentation tab's slides (P2, p2-slides; plan §8.2 "Slides"): the thumbnails' selection and
// dragging them to move slides, new slide with a layout (Ctrl+M) from the layout gallery, duplicate
// (Ctrl+D), delete, hide, change layout, and moving slides with the keyboard. Every change goes
// through the edit layer (plan §4.3 rule 4) as one undo step.

#include <windows.h>

#include <algorithm>
#include <cmath>

#include "Commands.h"
#include "slide/Presentation.h"
#include "slide/view/PresentationView.h"
#include "slide/view/SlideGallery.h"
#include "ui/Anim.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include "ui/Toolbar.h"

using Microsoft::WRL::ComPtr;
using menu::AppendItem;
using menu::AppendSeparator;
using namespace gfx;

namespace fw::slide::view {

namespace {

constexpr float kDragStart = 5;      // DIPs the pointer moves before a press on a thumbnail drags
constexpr float kScrollEdge = 28;    // DIPs: dragging this near the pane's top or bottom scrolls it
constexpr double kPictureBudget = 30;  // ms of layout pictures drawn per frame while a gallery opens

struct SlideCommandName {
  int cmd;
  const wchar_t* label;
  const wchar_t* keys;
};

constexpr SlideCommandName kSlideNames[] = {
    {CmdSlideNew, L"New slide", L"Ctrl+M"},
    {CmdSlideNewMenu, L"New slide with a layout", L""},
    {CmdSlideDuplicate, L"Duplicate slide", L"Ctrl+D"},
    {CmdSlideDelete, L"Delete slide", L"Delete"},
    {CmdSlideHide, L"Hide slide", L""},
    {CmdSlideLayoutMenu, L"Slide layout", L""},
    {CmdSlideMoveUp, L"Move slide up", L"Ctrl+Up"},
    {CmdSlideMoveDown, L"Move slide down", L"Ctrl+Down"},
    {CmdSlideMoveFirst, L"Move slide to the beginning", L"Ctrl+Shift+Up"},
    {CmdSlideMoveLast, L"Move slide to the end", L"Ctrl+Shift+Down"},
};

}  // namespace

// ---------------------------------------------------------------------------
// Selection and dragging in the thumbnails

std::vector<size_t> PresentationView::SelectedSlides() const {
  std::vector<size_t> s;
  if (selection_.Count() == SlideCount()) s = selection_.Indices();
  if (s.empty() && SlideCount()) s.push_back(current_);
  return s;
}

void PresentationView::ThumbsMouseDown(int t, UINT mods, int /*x*/, int y) {
  if (t < 0 || static_cast<size_t>(t) >= SlideCount()) return;
  const size_t i = static_cast<size_t>(t);
  const bool ctrl = (mods & kModCtrl) != 0, shift = (mods & kModShift) != 0;
  if (selection_.Count() != SlideCount()) selection_.Reset(SlideCount(), current_);
  pendingCollapse_ = -1;
  if (!ctrl && !shift && selection_.Selected(i) && selection_.Many()) {
    // The selection stays, so it can be dragged; a click that doesn't drag selects this slide alone.
    pendingCollapse_ = t;
    selection_.Set(selection_.Indices(), i);
  } else {
    selection_.Click(i, ctrl, shift);
  }
  GoToSlide(selection_.Current());
  thumbPress_ = selection_.Selected(i);
  dragY_ = static_cast<float>(y);
  dropGap_ = SIZE_MAX;
  host_.ViewInvalidate();
}

void PresentationView::ThumbsDragMove(int /*x*/, int y) {
  const float fy = static_cast<float>(y);
  if (drag_ != Drag::Slides) {
    if (!thumbPress_ || std::abs(fy - dragY_) < kDragStart * ui_) return;
    if (!CanEditSlides()) {
      thumbPress_ = false;
      return;
    }
    drag_ = Drag::Slides;
    pendingCollapse_ = -1;
  }
  // Near the pane's top or bottom the list scrolls, faster the closer the pointer is.
  const float edge = kScrollEdge * ui_;
  if (fy < thumbs_.top + edge) thumbScroll_ -= std::min(edge, thumbs_.top + edge - fy) * 0.5f;
  else if (fy > thumbs_.bottom - edge) thumbScroll_ += std::min(edge, fy - (thumbs_.bottom - edge)) * 0.5f;
  ClampScroll();
  const size_t gap = DropGap(Rows(), fy);
  dropGap_ = MoveChanges(SlideCount(), SelectedSlides(), gap) ? gap : SIZE_MAX;
  host_.ViewInvalidate();
}

void PresentationView::ThumbsDragEnd(bool drop) {
  const size_t gap = dropGap_;
  thumbPress_ = false;
  dropGap_ = SIZE_MAX;
  if (drop && gap != SIZE_MAX) {
    MoveSlides(SelectedSlides(), gap);
  } else if (!drop && pendingCollapse_ >= 0 && static_cast<size_t>(pendingCollapse_) < SlideCount()) {
    selection_.Click(static_cast<size_t>(pendingCollapse_), false, false);
    GoToSlide(selection_.Current());
  }
  pendingCollapse_ = -1;
  host_.ViewInvalidate();
}

bool PresentationView::ThumbsKeyDown(UINT vk, UINT mods) {
  const bool ctrl = (mods & kModCtrl) != 0, shift = (mods & kModShift) != 0, alt = (mods & kModAlt) != 0;
  if (alt || SlideCount() == 0) return false;
  if (selection_.Count() != SlideCount()) selection_.Reset(SlideCount(), current_);
  if (shift && !ctrl && (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT)) {
    // Shift+arrows grow or shrink the selection from its anchor.
    const bool back = vk == VK_UP || vk == VK_LEFT;
    if ((back && current_ == 0) || (!back && current_ + 1 >= SlideCount())) return true;
    selection_.Click(back ? current_ - 1 : current_ + 1, false, true);
    GoToSlide(selection_.Current());
    return true;
  }
  if (ctrl && !shift && vk == 'A') {
    selection_.SelectAll();
    host_.ViewInvalidate();
    return true;
  }
  if (!ctrl && !shift && vk == VK_RETURN) {
    // Enter in the thumbnails adds a slide after the current one, as in PowerPoint.
    Execute(CmdSlideNew);
    return true;
  }
  if (!ctrl && !shift && (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT)) {
    const bool back = vk == VK_UP || vk == VK_LEFT;
    if (back && current_ > 0) GoToSlide(current_ - 1);
    else if (!back && current_ + 1 < SlideCount()) GoToSlide(current_ + 1);
    return true;
  }
  return false;
}

void PresentationView::DrawThumbSelection(Canvas& c, const std::vector<ThumbRow>& rows, size_t i) {
  if (i >= rows.size() || i == current_ || !selection_.Selected(i) || selection_.Count() != SlideCount()) return;
  const ThumbRow& r = rows[i];
  c.Fill({r.row.left, r.row.top, r.row.right, r.row.bottom}, Alpha(theme::P().selection, 0.6f));
}

void PresentationView::DrawDropMarker(Canvas& c) {
  if (drag_ != Drag::Slides || dropGap_ == SIZE_MAX) return;
  const std::vector<ThumbRow> rows = Rows();
  if (rows.empty()) return;
  draw::Rect m = DropMarker(rows, dropGap_, 3 * ui_);
  // From the number's left to the picture's right, as PowerPoint draws it.
  m.left = rows[0].slide.left - 6 * ui_;
  m.right = rows[0].slide.right + 6 * ui_;
  const D2D1_RECT_F rc{m.left, m.top, m.right, m.bottom};
  const Palette& p = theme::P();
  c.FillRound(rc, 1.5f * ui_, p.accent);
  c.FillEllipse(rc.left, (rc.top + rc.bottom) / 2, 3.5f * ui_, 3.5f * ui_, p.accent);
}

// ---------------------------------------------------------------------------
// Commands

int PresentationView::SlideShortcut(UINT vk, UINT mods) const {
  const bool ctrl = (mods & kModCtrl) != 0, shift = (mods & kModShift) != 0, alt = (mods & kModAlt) != 0;
  if (alt) return 0;
  if (ctrl && !shift && vk == 'M') return CmdSlideNew;
  if (ctrl && !shift && vk == 'D') return CmdSlideDuplicate;
  if (!keysInThumbs_) return 0;
  if (ctrl) {
    if (vk == VK_UP) return shift ? CmdSlideMoveFirst : CmdSlideMoveUp;
    if (vk == VK_DOWN) return shift ? CmdSlideMoveLast : CmdSlideMoveDown;
    return 0;
  }
  if (!shift && (vk == VK_DELETE || vk == VK_BACK)) return CmdSlideDelete;
  return 0;
}

bool PresentationView::SlideCommandState(int cmd, CommandState& s) {
  const bool ready = Ready();
  const size_t n = SlideCount();
  const bool any = ready && n > 0;
  const std::vector<size_t> sel = any ? SelectedSlides() : std::vector<size_t>();
  switch (cmd) {
    case CmdSlideNew:
    case CmdSlideNewMenu: s.enabled = ready; break;
    case CmdSlideDuplicate: s.enabled = any; break;
    case CmdSlideDelete: s.enabled = any; break;
    case CmdSlideHide:
      s.enabled = any;
      s.checked = any && deck_->Hidden(current_);
      break;
    case CmdSlideLayoutMenu: s.enabled = any && !deck_->LayoutsFor(current_).empty(); break;
    case CmdSlideMoveUp:
    case CmdSlideMoveFirst: s.enabled = any && !sel.empty() && MoveChanges(n, sel, cmd == CmdSlideMoveFirst ? 0 : sel.front() - (sel.front() > 0)); break;
    case CmdSlideMoveDown:
    case CmdSlideMoveLast:
      s.enabled = any && !sel.empty() && MoveChanges(n, sel, cmd == CmdSlideMoveLast ? n : std::min(n, sel.back() + 2));
      break;
    default:
      if (cmd >= CmdSlideLayoutPick && cmd < CmdSlideLayoutPick + 20) {
        const std::vector<Deck::LayoutChoice> layouts = any ? deck_->LayoutsFor(current_) : std::vector<Deck::LayoutChoice>();
        const size_t k = static_cast<size_t>(cmd - CmdSlideLayoutPick);
        s.enabled = k < layouts.size();
        if (s.enabled) {
          s.label = layouts[k].name;
          s.checked = deck_->LayoutOf(current_) == static_cast<int>(layouts[k].index);
        }
        return true;
      }
      return false;
  }
  for (const SlideCommandName& x : kSlideNames)
    if (x.cmd == cmd) {
      s.label = x.label;
      s.shortcut = x.keys;
    }
  if (cmd == CmdSlideDuplicate && sel.size() > 1) s.label = L"Duplicate slides";
  if (cmd == CmdSlideDelete && sel.size() > 1) s.label = L"Delete slides";
  return true;
}

bool PresentationView::SlideCommand(int cmd) {
  const size_t n = SlideCount();
  switch (cmd) {
    case CmdSlideNew: {
      // After the current slide, on the layout PowerPoint picks: the current slide's, except after
      // a title slide, where it's the master's next layout ("Title and Content").
      int layout = -1;
      if (n) {
        layout = deck_->LayoutOf(current_);
        const std::vector<Deck::LayoutChoice> all = deck_->LayoutsFor(current_);
        if (all.size() > 1 && static_cast<int>(all[0].index) == layout) layout = static_cast<int>(all[1].index);
      }
      InsertSlide(n ? current_ + 1 : 0, layout);
      return true;
    }
    case CmdSlideNewMenu:
      if (newSlideGallery_) newSlideGallery_->SetOpen(true);
      return true;
    case CmdSlideDuplicate: DuplicateSlides(SelectedSlides()); return true;
    case CmdSlideDelete: DeleteSlides(SelectedSlides()); return true;
    case CmdSlideHide:
      if (n) HideSlides(SelectedSlides(), !deck_->Hidden(current_));
      return true;
    case CmdSlideLayoutMenu:
      if (layoutGallery_) layoutGallery_->SetOpen(true);
      return true;
    case CmdSlideMoveUp:
    case CmdSlideMoveDown:
    case CmdSlideMoveFirst:
    case CmdSlideMoveLast: {
      const std::vector<size_t> sel = SelectedSlides();
      if (sel.empty()) return true;
      size_t gap = 0;
      if (cmd == CmdSlideMoveUp) gap = sel.front() > 0 ? sel.front() - 1 : 0;
      else if (cmd == CmdSlideMoveDown) gap = std::min(n, sel.back() + 2);
      else if (cmd == CmdSlideMoveLast) gap = n;
      if (MoveChanges(n, sel, gap)) MoveSlides(sel, gap);
      return true;
    }
    default:
      if (cmd >= CmdSlideLayoutPick && cmd < CmdSlideLayoutPick + 20 && n) {
        const std::vector<Deck::LayoutChoice> layouts = deck_->LayoutsFor(current_);
        const size_t k = static_cast<size_t>(cmd - CmdSlideLayoutPick);
        if (k < layouts.size()) SetSlidesLayout(SelectedSlides(), static_cast<int>(layouts[k].index));
        return true;
      }
      return false;
  }
}

void PresentationView::SlideContextMenu(HMENU m, int thumb) {
  const bool any = SlideCount() > 0;
  // A right-click on a thumbnail outside the selection selects it alone.
  if (thumb >= 0 && !selection_.Selected(static_cast<size_t>(thumb))) {
    selection_.Click(static_cast<size_t>(thumb), false, false);
    GoToSlide(selection_.Current());
  }
  const size_t count = SelectedSlides().size();
  const bool many = count > 1;
  AppendItem(m, CmdSlideNew, L"New slide\tCtrl+M");
  AppendItem(m, CmdSlideDuplicate, many ? L"Duplicate slides\tCtrl+D" : L"Duplicate slide\tCtrl+D", any);
  AppendItem(m, CmdSlideDelete, many ? L"Delete slides\tDelete" : L"Delete slide\tDelete", any);
  const std::vector<Deck::LayoutChoice> layouts = any ? deck_->LayoutsFor(current_) : std::vector<Deck::LayoutChoice>();
  if (!layouts.empty()) {
    HMENU sub = CreatePopupMenu();
    const int now = deck_->LayoutOf(current_);
    for (size_t k = 0; k < layouts.size() && k < 20; ++k)
      AppendItem(sub, CmdSlideLayoutPick + static_cast<int>(k), layouts[k].name, true,
                 static_cast<int>(layouts[k].index) == now, true);
    menu::AppendSubmenu(m, sub, L"Layout");
  }
  AppendItem(m, CmdSlideHide, many ? L"Hide slides" : L"Hide slide", any, any && deck_->Hidden(current_));
  AppendSeparator(m);
}

// ---------------------------------------------------------------------------
// The toolbar: ＋ Slide ▾ and Layout ▾ with the layout gallery

void PresentationView::PrepareGalleries() {
  if (!newSlideGallery_) {
    newSlideGallery_ = std::make_unique<SlideGallery>();
    newSlideGallery_->glyph = icon::Add;
    newSlideGallery_->text = L"Slide";
    newSlideGallery_->split = true;
    newSlideGallery_->tooltip = L"New slide (Ctrl+M); the arrow picks its layout";
    newSlideGallery_->label = L"New slide";
    newSlideGallery_->onInvoke = [this] { Execute(CmdSlideNew); };
    newSlideGallery_->onPick = [this](int i) {
      if (i >= 0 && static_cast<size_t>(i) < galleryLayouts_.size())
        InsertSlide(SlideCount() ? current_ + 1 : 0, static_cast<int>(galleryLayouts_[static_cast<size_t>(i)].index));
    };
  }
  if (!layoutGallery_) {
    layoutGallery_ = std::make_unique<SlideGallery>();
    layoutGallery_->text = L"Layout";
    layoutGallery_->tooltip = L"The selected slides' layout";
    layoutGallery_->label = L"Slide layout";
    layoutGallery_->onPick = [this](int i) {
      if (i >= 0 && static_cast<size_t>(i) < galleryLayouts_.size())
        SetSlidesLayout(SelectedSlides(), static_cast<int>(galleryLayouts_[static_cast<size_t>(i)].index));
    };
  }
  galleryLayouts_ = deck_ ? deck_->LayoutsFor(current_) : std::vector<Deck::LayoutChoice>();
  std::vector<std::wstring> names;
  for (const Deck::LayoutChoice& l : galleryLayouts_) names.push_back(l.name);
  const draw::Size size = deck_ ? deck_->SlideSize() : draw::Size{960, 540};
  const int now = deck_ && SlideCount() ? deck_->LayoutOf(current_) : -1;
  int currentItem = -1;
  for (size_t k = 0; k < galleryLayouts_.size(); ++k)
    if (static_cast<int>(galleryLayouts_[k].index) == now) currentItem = static_cast<int>(k);
  for (SlideGallery* g : {newSlideGallery_.get(), layoutGallery_.get()}) {
    if (!g->IsOpen()) g->items = names;  // an open gallery keeps what it shows
    g->aspect = size.height > 0 ? size.width / size.height : 16.0f / 9.0f;
    g->paint = [this](Canvas& c, int i, const D2D1_RECT_F& r) {
      if (i >= 0 && static_cast<size_t>(i) < galleryLayouts_.size()) PaintLayoutPicture(c, galleryLayouts_[static_cast<size_t>(i)].index, r);
    };
  }
  layoutGallery_->current = currentItem;
  newSlideGallery_->current = -1;
}

void PresentationView::PaintLayoutPicture(Canvas& c, size_t layout, const D2D1_RECT_F& r) {
  const UINT w = static_cast<UINT>(std::lround(Width(r))), h = static_cast<UINT>(std::lround(Height(r)));
  if (w != layoutPictureW_) {
    layoutPictures_.clear();
    layoutPictureW_ = w;
  }
  if (layout >= layoutPictures_.size()) layoutPictures_.resize(layout + 1);
  ComPtr<ID2D1Bitmap1>& pic = layoutPictures_[layout];
  // A few pictures a frame, so the gallery opens at once and fills in.
  static double frameStart = 0;
  const double now = anim::Now();
  if (now - frameStart > 100) frameStart = now;
  if (!pic && now - frameStart < kPictureBudget)
    pic = RenderBitmapWith(c.DC(), w, h, [&](draw::Painter& p) { deck_->PaintLayout(layout, p); });
  if (pic) c.DC()->DrawBitmap(pic.Get(), r, 1, D2D1_INTERPOLATION_MODE_LINEAR);
  else host_.ViewInvalidate();
}

void PresentationView::AddSlideTools(ToolbarModel& m) {
  PrepareGalleries();
  const bool ready = Ready();
  ToolItem add;
  add.kind = ToolKind::Custom;
  add.control = newSlideGallery_.get();
  add.cmd = CmdSlideNewMenu;
  add.width = 84;
  add.priority = 8;
  add.tooltip = newSlideGallery_->tooltip;
  add.menuLabel = L"New slide";
  add.enabled = ready;
  newSlideGallery_->enabled = ready;
  ToolItem layout;
  layout.kind = ToolKind::Custom;
  layout.control = layoutGallery_.get();
  layout.cmd = CmdSlideLayoutMenu;
  layout.width = 84;
  layout.priority = 6;
  layout.tooltip = layoutGallery_->tooltip;
  layout.menuLabel = L"Slide layout";
  layout.enabled = ready && SlideCount() > 0 && !galleryLayouts_.empty();
  layoutGallery_->enabled = layout.enabled;
  m.left.insert(m.left.begin(), {add, layout});
  m.left[2].sepBefore = true;  // P1's print items after a separator
}

// ---------------------------------------------------------------------------
// Edits (through the edit layer)

std::vector<uint32_t> PresentationView::SlideIds() const {
  std::vector<uint32_t> ids;
  if (Presentation* model = deck_ ? deck_->Model() : nullptr)
    for (size_t i = 0; i < model->SlideCount(); ++i) ids.push_back(model->SlideId(i));
  return ids;
}

void PresentationView::SlidesChanged(const std::vector<uint32_t>& idsBefore, const std::vector<size_t>& touched, bool all) {
  const std::vector<int> old = OldIndexes(idsBefore, SlideIds());
  FollowSlides(thumbCache_, old);
  for (size_t i : touched)
    if (i < thumbCache_.size()) thumbCache_[i].bitmap.Reset();
  if (all)
    for (Thumb& th : thumbCache_) th.bitmap.Reset();
  slideBitmap_.Reset();
  slideBitmapOf_ = SIZE_MAX;
  linksOf_ = SIZE_MAX;
  layoutPictures_.clear();
  matches_.clear();
  match_ = -1;
  if (current_ >= SlideCount()) current_ = SlideCount() ? SlideCount() - 1 : 0;
  ClampScroll();
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

bool PresentationView::CanEditSlides() const {
  // The edit layer (p2-model) isn't in this build yet: the tab stays read-only.
  host_.ViewMessage(BannerKind::Info, L"Editing slides comes with the edit layer; this build shows and presents decks.");
  return false;
}

bool PresentationView::InsertSlide(size_t /*at*/, int /*layout*/) { return CanEditSlides(); }
bool PresentationView::DuplicateSlides(const std::vector<size_t>& /*slides*/) { return CanEditSlides(); }
bool PresentationView::DeleteSlides(const std::vector<size_t>& /*slides*/) { return CanEditSlides(); }
bool PresentationView::MoveSlides(const std::vector<size_t>& /*slides*/, size_t /*gap*/) { return CanEditSlides(); }
bool PresentationView::HideSlides(const std::vector<size_t>& /*slides*/, bool /*hide*/) { return CanEditSlides(); }
bool PresentationView::SetSlidesLayout(const std::vector<size_t>& /*slides*/, int /*layout*/) { return CanEditSlides(); }

}  // namespace fw::slide::view
