#include "slide/view/SlideGallery.h"

#include <algorithm>
#include <cmath>

#include "ui/Icons.h"
#include "ui/Theme.h"

using namespace gfx;

namespace fw::slide::view {

namespace {
constexpr float kPad = 8;       // DIPs around the pictures
constexpr float kGap = 8;       // DIPs between cells
constexpr float kName = 30;     // DIPs under a picture for its name (two short lines)
constexpr float kHeading = 26;  // DIPs
constexpr float kArrowWidth = 18;    // DIPs: a split button's arrow
}  // namespace

D2D1_RECT_F SlideGallery::ArrowRect() const {
  const float u = Ui();
  return RectF(rc.right - kArrowWidth * u, rc.top, rc.right, rc.bottom);
}

SlideGallery::Part SlideGallery::PartAt(float x, float y) const {
  if (!Hit(x, y)) return kNone;
  if (!split) return kArrow;
  return Contains(ArrowRect(), x, y) ? kArrow : kMain;
}

void SlideGallery::Draw(Canvas& c) {
  if (!visible) return;
  const Palette& p = theme::P();
  const float u = Ui();
  const D2D1_COLOR_F fg = enabled ? p.text : p.textDisabled;
  if (split) {
    const D2D1_RECT_F main = RectF(rc.left, rc.top, rc.right - kArrowWidth * u, rc.bottom), arrow = ArrowRect();
    ctl::DrawSubtle(c, main, u, enabled && hoverPart_ == kMain ? hover_.V() : 0, enabled && pressPart_ == kMain ? press_.V() : 0);
    ctl::DrawSubtle(c, arrow, u, enabled && hoverPart_ == kArrow ? hover_.V() : 0,
                    enabled ? std::max(pressPart_ == kArrow ? press_.V() : 0.f, popup_.open ? 0.7f : 0.f) : 0);
  } else {
    ctl::DrawSubtle(c, rc, u, enabled ? hover_.V() : 0, enabled ? std::max(press_.V(), popup_.open ? 0.7f : 0.f) : 0);
  }
  float x = rc.left + 6 * u;
  if (glyph) {
    c.Icon(glyph, RectF(x, rc.top, x + 20 * u, rc.bottom), fg, 16 * u);
    x += 22 * u;
  }
  if (!text.empty()) c.Text(text, RectF(x, rc.top, rc.right - kArrowWidth * u, rc.bottom), fg, 13 * u);
  const D2D1_RECT_F a = ArrowRect();
  ctl::DrawChevron(c, RectF(a.left, a.top, a.right - 2 * u, a.bottom), enabled ? p.textSecondary : p.textDisabled, u);
  if (FocusVisible()) ctl::DrawFocusRing(c, rc, u);
}

D2D1_SIZE_F SlideGallery::CellSize() const {
  const float u = Ui();
  const float w = pictureWidth * u;
  return {w, std::round(w / std::max(0.2f, aspect)) + kName * u};
}

D2D1_RECT_F SlideGallery::Pictures() const {
  const float u = Ui();
  const D2D1_RECT_F in = popup_.Inner(u);
  return RectF(in.left, in.top + (heading.empty() ? 0 : kHeading * u), in.right, in.bottom);
}

D2D1_RECT_F SlideGallery::CellRect(int i) const {
  const float u = Ui();
  const D2D1_RECT_F area = Pictures();
  const D2D1_SIZE_F cell = CellSize();
  const int col = i % std::max(1, columns), row = i / std::max(1, columns);
  const float x = area.left + kPad * u + col * (cell.width + kGap * u);
  const float y = area.top + kPad * u + row * (cell.height + kGap * u) - scroll_;
  return RectF(std::round(x), std::round(y), std::round(x + cell.width), std::round(y + cell.height));
}

float SlideGallery::MaxScroll() const {
  const float u = Ui();
  const int rows = (static_cast<int>(items.size()) + columns - 1) / std::max(1, columns);
  const float content = 2 * kPad * u + rows * CellSize().height + std::max(0, rows - 1) * kGap * u;
  return std::max(0.f, content - Height(Pictures()));
}

int SlideGallery::ItemAt(float x, float y) const {
  if (!popup_.open || !Contains(Pictures(), x, y)) return -1;
  for (int i = 0; i < static_cast<int>(items.size()); ++i)
    if (Contains(CellRect(i), x, y)) return i;
  return -1;
}

void SlideGallery::EnsureVisible(int i) {
  if (i < 0) return;
  const D2D1_RECT_F r = CellRect(i), area = Pictures();
  const float u = Ui();
  if (r.top < area.top + kPad * u) scroll_ -= area.top + kPad * u - r.top;
  else if (r.bottom > area.bottom - kPad * u) scroll_ += r.bottom - (area.bottom - kPad * u);
  scroll_ = std::clamp(scroll_, 0.f, MaxScroll());
}

void SlideGallery::DrawPopup(Canvas& c) {
  if (!popup_.Visible()) return;
  const Palette& p = theme::P();
  const float u = Ui();
  popup_.Begin(c, u);
  const D2D1_RECT_F in = popup_.Inner(u);
  if (!heading.empty())
    c.Text(heading, RectF(in.left + kPad * u, in.top, in.right - kPad * u, in.top + kHeading * u), p.textSecondary, 12 * u,
           DWRITE_FONT_WEIGHT_SEMI_BOLD);
  const D2D1_RECT_F area = Pictures();
  c.PushClip(area);
  const D2D1_SIZE_F cell = CellSize();
  const float picH = cell.height - kName * u;
  for (int i = 0; i < static_cast<int>(items.size()); ++i) {
    const D2D1_RECT_F r = CellRect(i);
    if (r.bottom < area.top || r.top > area.bottom) continue;
    if (i == hot_) c.FillRound(Inflate(r, 4 * u, 4 * u), 4 * u, p.hover);
    const D2D1_RECT_F pic = RectF(r.left, r.top, r.right, r.top + picH);
    c.Fill(pic, Rgb(0xFFFFFF));
    if (paint) paint(c, i, pic);
    c.Stroke(pic, i == current ? p.accent : Alpha(p.chromeBorder, 0.9f), i == current ? 2 * u : std::max(1.f, u * 0.75f));
    c.TextWrapped(items[static_cast<size_t>(i)], RectF(r.left, pic.bottom + 3 * u, r.right, r.bottom),
                  i == current ? p.accentText : p.text, 11.5f * u, 2);
  }
  c.PopClip();
  popup_.End(c);
}

void SlideGallery::SetOpen(bool open) {
  if (open == popup_.open) return;
  if (!open) {
    popup_.Close();
    hot_ = -1;
    return;
  }
  if (!enabled || items.empty()) return;
  Opened();
  const float u = Ui();
  const int cols = std::max(1, std::min(columns, static_cast<int>(items.size())));
  const int rows = (static_cast<int>(items.size()) + columns - 1) / std::max(1, columns);
  const D2D1_SIZE_F cell = CellSize();
  const float w = 2 * kPad * u + cols * cell.width + (cols - 1) * kGap * u + 8 * u;
  const float h = (heading.empty() ? 0 : kHeading * u) + 2 * kPad * u + rows * cell.height + (rows - 1) * kGap * u + 8 * u;
  popup_.Place(rc, w, h, PopupBounds(), u);
  popup_.Open();
  scroll_ = 0;
  hot_ = current >= 0 ? current : 0;
  EnsureVisible(hot_);
  if (onOpen) onOpen();
}

bool SlideGallery::OnMouseMove(float x, float y) {
  bool changed = false;
  const Part part = PartAt(x, y);
  if (part != hoverPart_) {
    hoverPart_ = part;
    changed = true;
  }
  const bool over = part != kNone;
  if (over != (hover_.Target() > 0.5f)) hover_.To(over ? 1.f : 0.f, 120);
  if (popup_.open) {
    const int i = ItemAt(x, y);
    if (i >= 0 && i != hot_) {
      hot_ = i;
      changed = true;
    }
  }
  return changed || over;
}

void SlideGallery::OnMouseLeave() {
  hover_.To(0, 120);
  hoverPart_ = kNone;
}

void SlideGallery::OnMouseDown(float x, float y, bool, bool) {
  if (popup_.open && popup_.Hit(x, y)) return;  // the click picks on release
  pressPart_ = PartAt(x, y);
  press_.To(1, 40);
  TakeFocus();
  if (pressPart_ == kArrow) SetOpen(!popup_.open);
  else if (popup_.open) SetOpen(false);
}

void SlideGallery::OnMouseUp(float x, float y) {
  press_.To(0, 120);
  const Part pressed = pressPart_;
  pressPart_ = kNone;
  if (popup_.open && popup_.Hit(x, y)) {
    const int i = ItemAt(x, y);
    if (i >= 0) Pick(i);
    return;
  }
  if (pressed == kMain && PartAt(x, y) == kMain && enabled && onInvoke) onInvoke();
}

bool SlideGallery::OnWheel(int delta, float x, float y) {
  if (!popup_.open || !popup_.Hit(x, y)) return false;
  scroll_ = std::clamp(scroll_ - static_cast<float>(delta) / WHEEL_DELTA * 60 * Ui(), 0.f, MaxScroll());
  return true;
}

bool SlideGallery::OnKeyDown(UINT vk, bool, bool, bool alt) {
  if (!enabled) return false;
  const int n = static_cast<int>(items.size());
  if (!popup_.open) {
    if (vk == VK_DOWN || (alt && vk == VK_DOWN) || (!split && (vk == VK_RETURN || vk == VK_SPACE))) {
      SetOpen(true);
      return true;
    }
    if (split && (vk == VK_RETURN || vk == VK_SPACE)) {
      if (onInvoke) onInvoke();
      return false;  // done: a toolbar hands the keyboard back
    }
    return false;
  }
  int next = hot_ < 0 ? 0 : hot_;
  switch (vk) {
    case VK_ESCAPE: SetOpen(false); return true;
    case VK_TAB: SetOpen(false); return false;
    case VK_LEFT: next = std::max(0, next - 1); break;
    case VK_RIGHT: next = std::min(n - 1, next + 1); break;
    case VK_UP: next = next - columns >= 0 ? next - columns : next; break;
    case VK_DOWN: next = next + columns < n ? next + columns : next; break;
    case VK_HOME: next = 0; break;
    case VK_END: next = n - 1; break;
    case VK_RETURN:
    case VK_SPACE:
      if (hot_ >= 0) Pick(hot_);
      return false;
    default: return true;
  }
  hot_ = next;
  EnsureVisible(hot_);
  if (group_) group_->Invalidate();
  return true;
}

void SlideGallery::OnFocus(bool focused) {
  if (!focused) SetOpen(false);
}

std::wstring SlideGallery::TooltipAt(float x, float y) const {
  if (const int i = ItemAt(x, y); i >= 0) return items[static_cast<size_t>(i)];
  return tooltip;
}

void SlideGallery::Pick(int i) {
  SetOpen(false);
  if (i >= 0 && i < static_cast<int>(items.size()) && onPick) onPick(i);
}

void SlideGallery::Describe(uia::Tree& t, int parent) {
  const int i = t.Add(parent, id, UIA_ButtonControlTypeId, Name(), rc);
  uia::Node& n = t[i];
  n.enabled = enabled;
  n.focusable = true;
  n.focus = [this] { TakeFocus(); };
  n.focused = Focused();
  n.expand = popup_.open ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
  n.doExpand = [this](bool e) {
    TakeFocus();
    SetOpen(e);
  };
}

}  // namespace fw::slide::view
