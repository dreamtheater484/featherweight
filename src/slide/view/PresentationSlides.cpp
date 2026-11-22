// The presentation tab's slides (P2, p2-slides; plan §8.2 "Slides"): the thumbnails' selection and
// dragging them to move slides, new slide with a layout (Ctrl+M) from the layout gallery, duplicate
// (Ctrl+D), delete, hide, change layout, and moving slides with the keyboard. Every change goes
// through the edit layer (plan §4.3 rule 4) as one undo step.

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Commands.h"
#include "Util.h"
#include "core/Strings.h"
#include "slide/Presentation.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "slide/edit/Edit.h"
#include "slide/edit/Save.h"
#include "slide/view/NotesEdit.h"
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

// Slides copied in this process, for pasting into a presentation tab: the source deck as it was
// then (saved to memory, so changing or closing the source afterwards doesn't matter) and which
// of its slides.
struct SlideClip {
  std::vector<uint8_t> package;
  std::vector<size_t> slides;
  unsigned long sequence = 0;  // the clipboard's after copying: anything copied later replaces the slides
  bool Current() const { return !package.empty() && sequence == util::ClipboardSequence(); }
};

SlideClip& Clip() {
  static SlideClip clip;
  return clip;
}

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
    case CmdCut:
    case CmdCopy:
      if (!keysInThumbs_) return false;  // the slide's own (P1's picture and text; p2-objects' shapes)
      s.enabled = any;
      s.label = cmd == CmdCut ? L"Cut slides" : L"Copy slides";
      s.shortcut = cmd == CmdCut ? L"Ctrl+X" : L"Ctrl+C";
      return true;
    case CmdPaste:
      if (!Clip().Current()) return false;
      s.enabled = ready;
      s.label = L"Paste slides";
      s.shortcut = L"Ctrl+V";
      return true;
    case CmdPasteSlidesDestination:
    case CmdPasteSlidesSource:
      s.enabled = ready && Clip().Current();
      s.label = cmd == CmdPasteSlidesDestination ? L"Paste slides: use destination theme" : L"Paste slides: keep source formatting";
      return true;
    case CmdSlideMoveUp:
    case CmdSlideMoveFirst: s.enabled = any && !sel.empty() && MoveChanges(n, sel, cmd == CmdSlideMoveFirst ? 0 : sel.front() - (sel.front() > 0)); break;
    case CmdSlideMoveDown:
    case CmdSlideMoveLast:
      s.enabled = any && !sel.empty() && MoveChanges(n, sel, cmd == CmdSlideMoveLast ? n : std::min(n, sel.back() + 2));
      break;
    default:
      if (DesignCommandState(cmd, s)) return true;  // PresentationDesign.cpp
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
      // After the current slide, on the layout PowerPoint picks (NewSlideLayout).
      InsertSlide(n ? current_ + 1 : 0, -1);
      return true;
    }
    case CmdSlideNewMenu:
      if (newSlideGallery_) newSlideGallery_->SetOpen(true);
      return true;
    case CmdSlideDuplicate: DuplicateSlides(SelectedSlides()); return true;
    case CmdCut:
    case CmdCopy:
      if (!keysInThumbs_ || !n) return false;
      CopySlides(cmd == CmdCut);
      return true;
    case CmdPaste:
      if (!Clip().Current()) return false;
      PasteSlides(false);
      return true;
    case CmdPasteSlidesDestination:
    case CmdPasteSlidesSource: PasteSlides(cmd == CmdPasteSlidesSource); return true;
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
      if (DesignCommand(cmd)) return true;  // PresentationDesign.cpp
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
  const bool clip = Clip().Current();
  AppendItem(m, CmdCut, many ? L"Cut slides\tCtrl+X" : L"Cut slide\tCtrl+X", any);
  AppendItem(m, CmdCopy, many ? L"Copy slides\tCtrl+C" : L"Copy slide\tCtrl+C", any);
  AppendItem(m, CmdPasteSlidesDestination, L"Paste: use destination theme\tCtrl+V", clip);
  AppendItem(m, CmdPasteSlidesSource, L"Paste: keep source formatting", clip);
  AppendSeparator(m);
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
  AppendItem(m, CmdFormatBackground, L"Format background…", any);
  AppendItem(m, CmdResetBackground, L"Reset background", any);
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
  ToolItem design;
  design.cmd = CmdDesignPanel;
  design.glyph = icon::Color;
  design.tooltip = L"Design: themes, theme colors, background, slide size";
  design.toggle = true;
  design.checked = QueryCommand(CmdDesignPanel).checked;
  design.enabled = ready;
  design.priority = 5;
  ToolItem hf;
  hf.cmd = CmdHeaderFooter;
  hf.glyph = icon::Page;
  hf.tooltip = L"Header & footer: slide numbers, footer and date";
  hf.menuLabel = L"Header & footer";
  hf.toggle = true;
  hf.checked = QueryCommand(CmdHeaderFooter).checked;
  hf.enabled = ready && SlideCount() > 0;
  hf.priority = 4;
  m.left.insert(m.left.begin(), {add, layout, design, hf});
  m.left[4].sepBefore = true;  // P1's print items after a separator
}

// ---------------------------------------------------------------------------
// Script ops (--script; p2-corpus's build-from-scratch run and the timing runs)

bool PresentationView::SlideScriptOp(const std::wstring& op, const std::wstring& arg) {
  auto refuse = [&](const std::wstring& why) {
    fwprintf(stderr, L"%ls:%ls: %ls\n", op.c_str(), arg.c_str(), why.c_str());
    fflush(stderr);
    return true;
  };
  if (!Ready()) return false;
  if (op == L"select") {
    // select:2,4-6 (1-based): those slides, the first of them shown.
    std::vector<size_t> picked;
    for (const std::wstring& part : Split(arg, L',')) {
      const size_t dash = part.find(L'-');
      int a = 0, b = 0;
      if (!ParseInt(part.substr(0, dash), a)) continue;
      b = a;
      if (dash != std::wstring::npos && !ParseInt(part.substr(dash + 1), b)) continue;
      for (int k = std::max(1, a); k <= b && k <= static_cast<int>(SlideCount()); ++k) picked.push_back(static_cast<size_t>(k - 1));
    }
    if (picked.empty()) return refuse(L"no such slides");
    GoToSlide(picked.front());
    selection_.Set(picked, picked.front());
    keysInThumbs_ = true;
    host_.ViewInvalidate();
    return true;
  }
  if (op == L"layout") {
    if (!SlideCount()) return refuse(L"no slide");
    for (const Deck::LayoutChoice& l : deck_->LayoutsFor(current_))
      if (EqualsNoCase(l.name, arg)) {
        if (!SetSlidesLayout(SelectedSlides(), static_cast<int>(l.index))) refuse(L"refused");
        return true;
      }
    return refuse(L"no layout of that name");
  }
  if (op == L"theme") {
    const std::vector<BuiltInTheme>& themes = BuiltInThemes();
    for (size_t i = 0; i < themes.size(); ++i)
      if (EqualsNoCase(themes[i].name, arg)) {
        if (!ApplyTheme(static_cast<int>(i), {})) refuse(L"refused");
        return true;
      }
    return refuse(L"no built-in theme of that name");
  }
  if (op == L"colors") {
    const std::vector<ColorScheme>& schemes = ColorSchemes();
    for (size_t i = 0; i < schemes.size(); ++i)
      if (EqualsNoCase(schemes[i].name, arg)) {
        if (!ApplyColors(static_cast<int>(i), {})) refuse(L"refused");
        return true;
      }
    return refuse(L"no theme colors of that name");
  }
  if (op == L"notestext") {
    // notestext:<text>, '|' between paragraphs: the current slide's notes as typed.
    std::wstring text = arg;
    std::replace(text.begin(), text.end(), L'|', L'\n');
    if (!SlideCount() || !SetNotes(current_, text)) refuse(L"refused");
    return true;
  }
  if (op == L"notesedit") {
    // notesedit:<text>: through the notes pane's text box (opened as a click opens it, the text
    // typed, then left), '|' between lines.
    std::wstring text = arg;
    std::replace(text.begin(), text.end(), L'|', L'\n');
    if (!notesShown_ || !NotesMouseDown()) return refuse(L"the notes pane can't be typed in");
    notesEdit_->SetText(text);
    EndNotesEdit();
    return true;
  }
  if (op == L"headerfooter") {
    // headerfooter:slidenumber=1|footer=<text>|date=1|datefixed=<text>|dateformat=N|title=0,
    // applied to every slide (title=0: not on title slides).
    HeaderFooterSettings hf;
    for (const std::wstring& part : Split(arg, L'|')) {
      const size_t eq = part.find(L'=');
      const std::wstring key = ToLower(part.substr(0, eq)), value = eq == std::wstring::npos ? L"1" : part.substr(eq + 1);
      int n = 0;
      if (key == L"slidenumber") hf.slideNumber = value != L"0";
      else if (key == L"footer") hf.footer = !value.empty(), hf.footerText = value;
      else if (key == L"date") hf.date = value != L"0";
      else if (key == L"datefixed") hf.date = true, hf.dateAuto = false, hf.dateFixed = value;
      else if (key == L"dateformat" && ParseInt(value, n)) hf.dateFormat = n;
      else if (key == L"title") hf.notOnTitle = value == L"0";
    }
    if (!ApplyHeaderFooter(hf, {})) refuse(L"refused");
    return true;
  }
  if (op == L"slidesize") {
    // slidesize:16:9|4:3, with ,fit or ,max for how the content scales.
    const int choice = arg.rfind(L"4:3", 0) == 0 ? 1 : 0;
    const int scale = arg.find(L"max") != std::wstring::npos ? 0 : 1;
    if (!SetSlideSize(choice, scale)) refuse(L"refused");
    return true;
  }
  return false;
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
  // The current slide and the selection follow their slides; a slide gone leaves the one now in
  // its place current.
  const size_t n = SlideCount();
  std::vector<size_t> selected;
  size_t current = SIZE_MAX;
  for (size_t i = 0; i < old.size(); ++i) {
    if (old[i] < 0) continue;
    const size_t o = static_cast<size_t>(old[i]);
    if (selection_.Selected(o)) selected.push_back(i);
    if (o == current_) current = i;
  }
  if (current == SIZE_MAX) current = n ? std::min(current_, n - 1) : 0;
  if (current != current_) {
    current_ = current;
    sx_ = sy_ = 0;
    notesScroll_ = 0;
  }
  if (std::find(selected.begin(), selected.end(), current_) == selected.end()) selected.assign(n ? 1 : 0, current_);
  selection_.Reset(n, current_);
  if (n) selection_.Set(selected, current_);
  FollowSlides(thumbCache_, old);
  thumbCache_.resize(n);
  for (size_t i : touched)
    if (i < thumbCache_.size()) thumbCache_[i].bitmap.Reset();
  if (all) {
    for (Thumb& th : thumbCache_) th.bitmap.Reset();
    layoutPictures_.clear();
  }
  slideBitmap_.Reset();
  slideBitmapOf_ = SIZE_MAX;
  links_.clear();
  linksOf_ = SIZE_MAX;
  matches_.clear();
  match_ = -1;
  ClampScroll();
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

edit::Editor* PresentationView::SlideEditor() const {
  edit::Editor* ed = deck_ ? deck_->Editor() : nullptr;
  if (ed && !editListening_) {
    editListening_ = true;
    editIds_ = SlideIds();
    PresentationView* self = const_cast<PresentationView*>(this);
    // Every edit, undo and redo, whoever made it (the tab's slide edits, p2-objects, p2-text).
    ed->OnChange([self](const edit::Changes& c) { self->OnSlideEdits(c); });
    self->OnEditorMade(*ed);
  }
  return ed;
}

void PresentationView::OnEditorMade(edit::Editor& ed) {
  AttachFile(ed);  // p2-model (PresentationSave.cpp): the recovery journal, and the file told of each change
}

void PresentationView::OnSlideEdits(const edit::Changes& c) {
  if (!deck_) return;
  const std::vector<uint32_t> before = editIds_;
  editIds_ = SlideIds();
  const bool all = c.allSlides || c.slideSize;
  // What the deck prepared follows the slides, then goes for the ones that changed (their model
  // objects were read again: what was built from them is stale).
  if (c.slideList) deck_->Reorder(OldIndexes(before, editIds_));
  std::vector<size_t> touched;
  for (const edit::SlideChange& sc : c.slides)
    if (sc.index >= 0 && static_cast<size_t>(sc.index) < SlideCount()) {
      touched.push_back(static_cast<size_t>(sc.index));
      if (!all) deck_->Invalidate(static_cast<size_t>(sc.index));
    }
  if (all)
    for (size_t i = 0; i < SlideCount(); ++i) deck_->Invalidate(i);
  SlidesChanged(before, touched, all);
  DesignChanged();
}

bool PresentationView::CopySlides(bool cut) {
  const std::vector<size_t> slides = SelectedSlides();
  edit::Editor* ed = SlideEditor();
  if (slides.empty() || !ed) return false;
  // Other programs get the current slide as a picture and its text (P1); a presentation tab the
  // slides themselves.
  CopySlide(true, true);
  SlideClip& clip = Clip();
  clip.package.clear();
  if (!edit::SaveToBytes(*ed, clip.package).Ok()) {
    clip.package.clear();
    host_.ViewMessage(BannerKind::Warning, L"These slides couldn't be copied.");
    return false;
  }
  clip.slides = slides;
  clip.sequence = util::ClipboardSequence();
  if (slides.size() > 1)
    host_.ViewMessage(BannerKind::Success, (cut ? L"Cut " : L"Copied ") + std::to_wstring(slides.size()) + L" slides");
  return !cut || DeleteSlides(slides);
}

bool PresentationView::PasteSlides(bool keepSource) {
  SlideClip& clip = Clip();
  if (!clip.Current() || !CanEditSlides()) return false;
  pkg::Status status;
  std::unique_ptr<Presentation> source =
      fw::slide::Read(pkg::Package::Open(pkg::Snapshot::FromBytes(clip.package), pkg::Limits::Default(), status), status);
  if (!source) {
    host_.ViewMessage(BannerKind::Warning, L"The copied slides couldn't be read.");
    return false;
  }
  const size_t at = SlideCount() ? current_ + 1 : 0;
  std::vector<uint32_t> ids;
  if (!Done(SlideEditor()->InsertSlides(*source, clip.slides, at, keepSource, &ids))) return false;
  const std::vector<uint32_t> now = SlideIds();
  std::vector<size_t> pasted;
  for (uint32_t id : ids)
    if (auto it = std::find(now.begin(), now.end(), id); it != now.end()) pasted.push_back(static_cast<size_t>(it - now.begin()));
  if (!pasted.empty()) SelectSlides(pasted, pasted.front());
  return true;
}

bool PresentationView::NotesMouseDown() {
  if (!SlideCount()) return false;
  if (notesEdit_ && notesEdit_->IsOpen()) return true;
  edit::Editor* ed = SlideEditor();
  if (!ed || !ed->CanEdit()) return false;  // a read-only deck: the notes are only read
  if (!notesEdit_) {
    notesEdit_ = std::make_unique<NotesEdit>();
    notesEdit_->onCommit = [this](const std::wstring& text) { SetNotes(notesSlide_, text); };
    notesEdit_->onLeave = [this] { EndNotesEdit(); };
  }
  notesSlide_ = current_;
  const float pad = 12 * ui_;
  const D2D1_RECT_F box{notes_.left + pad, notes_.top + pad * 0.75f, notes_.right - pad, notes_.bottom - pad * 0.5f};
  if (!notesEdit_->Open(host_.ViewHwnd(), Inflate(box, 4 * ui_, 4 * ui_), deck_->Notes(current_), ui_)) return false;
  host_.ViewInvalidate();
  return true;
}

void PresentationView::EndNotesEdit() {
  if (!notesEdit_ || !notesEdit_->IsOpen()) return;
  notesEdit_->Commit();
  notesEdit_->Close();
  host_.ViewInvalidate();
}

void PresentationView::PlaceNotesEdit(const D2D1_RECT_F& box) {
  if (notesEdit_) notesEdit_->Place(Inflate(box, 4 * ui_, 4 * ui_), ui_);
}

bool PresentationView::CanEditSlides() const {
  edit::Editor* ed = SlideEditor();
  std::u16string why;
  if (ed && ed->CanEdit(&why)) return true;
  host_.ViewMessage(BannerKind::Warning, why.empty() ? std::wstring(L"This presentation can't be edited.") : std::wstring(why.begin(), why.end()));
  return false;
}

bool PresentationView::Done(const edit::Result& r) const {
  if (!r.ok)
    host_.ViewMessage(BannerKind::Warning,
                      r.message.empty() ? std::wstring(L"That change couldn't be made.") : std::wstring(r.message.begin(), r.message.end()));
  return r.ok;
}

void PresentationView::SelectSlides(const std::vector<size_t>& slides, size_t current) {
  const size_t n = SlideCount();
  if (!n) return;
  current = std::min(current, n - 1);
  if (current != current_) {
    current_ = current;
    sx_ = sy_ = 0;
    notesScroll_ = 0;
    slideBitmap_.Reset();
  }
  selection_.Reset(n, current_);
  std::vector<size_t> valid;
  for (size_t i : slides)
    if (i < n) valid.push_back(i);
  if (!valid.empty()) selection_.Set(valid, current_);
  ScrollThumbIntoView(current_);
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

int PresentationView::NewSlideLayout() const {
  if (!deck_ || SlideCount() == 0) return 0;
  const int l = deck_->LayoutOf(current_);
  if (l < 0) return 0;
  // After a title slide PowerPoint goes on with the master's next layout (Title and Content).
  const Presentation* model = deck_->Model();
  if (model && static_cast<size_t>(l) < model->Layouts().size() && IsTitleLayout(model->Layouts()[static_cast<size_t>(l)].get())) {
    const std::vector<Deck::LayoutChoice> choices = deck_->LayoutsFor(current_);
    for (size_t k = 0; k + 1 < choices.size(); ++k)
      if (choices[k].index == static_cast<size_t>(l)) return static_cast<int>(choices[k + 1].index);
  }
  return l;
}

bool PresentationView::InsertSlide(size_t at, int layout) {
  if (!CanEditSlides()) return false;
  at = std::min(at, SlideCount());
  if (layout < 0) layout = NewSlideLayout();
  if (!Done(SlideEditor()->NewSlide(at, static_cast<size_t>(layout)))) return false;
  SelectSlides({at}, at);
  return true;
}

bool PresentationView::DuplicateSlides(const std::vector<size_t>& slides) {
  if (slides.empty() || !CanEditSlides()) return false;
  std::vector<uint32_t> ids;
  if (!Done(SlideEditor()->DuplicateSlides(slides, &ids))) return false;
  // The copies selected, as PowerPoint leaves them.
  const std::vector<uint32_t> now = SlideIds();
  std::vector<size_t> copies;
  for (uint32_t id : ids)
    if (auto it = std::find(now.begin(), now.end(), id); it != now.end()) copies.push_back(static_cast<size_t>(it - now.begin()));
  if (!copies.empty()) SelectSlides(copies, copies.front());
  return true;
}

bool PresentationView::DeleteSlides(const std::vector<size_t>& slides) {
  if (slides.empty() || !CanEditSlides()) return false;
  return Done(SlideEditor()->DeleteSlides(slides));
}

bool PresentationView::MoveSlides(const std::vector<size_t>& slides, size_t gap) {
  if (slides.empty() || !MoveChanges(SlideCount(), slides, gap) || !CanEditSlides()) return false;
  const size_t to = MovedTo(SlideCount(), slides, gap);
  // The current slide stays current where it went.
  std::vector<size_t> sorted = slides;
  std::sort(sorted.begin(), sorted.end());
  const auto at = std::find(sorted.begin(), sorted.end(), current_);
  const size_t offset = at == sorted.end() ? 0 : static_cast<size_t>(at - sorted.begin());
  if (!Done(SlideEditor()->MoveSlides(sorted, to))) return false;
  std::vector<size_t> moved;
  for (size_t k = 0; k < sorted.size(); ++k) moved.push_back(to + k);
  SelectSlides(moved, to + offset);
  return true;
}

bool PresentationView::HideSlides(const std::vector<size_t>& slides, bool hide) {
  if (slides.empty() || !CanEditSlides()) return false;
  return Done(SlideEditor()->HideSlides(slides, hide));
}

bool PresentationView::SetSlidesLayout(const std::vector<size_t>& slides, int layout) {
  if (slides.empty() || layout < 0 || !CanEditSlides()) return false;
  return Done(SlideEditor()->SetLayout(slides, static_cast<size_t>(layout)));
}

}  // namespace fw::slide::view
