#include "ui/SidePanel.h"

#include <algorithm>
#include <cmath>

#include "ui/Icons.h"
#include "ui/Theme.h"

using namespace gfx;

namespace {
constexpr float kTitleH = 52;  // DIPs: the title row
constexpr float kCloseW = 32;
}  // namespace

SidePanel::SidePanel() {
  close_.id = 1;
  close_.glyph = icon::Cancel;
  close_.glyphSize = 12;
  close_.tooltip = L"Close (Esc)";
  set_.Add(&close_);
}

void SidePanel::Show(PanelContent* content, bool animate) {
  if (content == content_) {
    if (animate) return;
    slide_.Snap(content ? 1.f : 0.f);
    if (!content) {  // done sliding out at once: the old content may be about to go
      shown_ = nullptr;
      group_.Clear();
    }
    return;
  }
  Blur();
  set_.Reset();
  pressed_ = false;
  if (content_) content_->OnMouseLeave();
  content_ = content;
  last_ = nullptr;
  if (content) {
    shown_ = content;
    group_.Clear();
    if (animate) slide_.To(1, 200);
    else slide_.Snap(1);
  } else if (animate) {
    slide_.To(0, 160);  // the old content stays drawn while it slides out
  } else {
    slide_.Snap(0);
    shown_ = nullptr;
    group_.Clear();
  }
  group_.Invalidate();
}

int SidePanel::Width(float ui) const { return static_cast<int>(std::lround(kWidth * ui * slide_.V())); }

D2D1_RECT_F SidePanel::Body() const { return RectF(rc_.left, titleRc_.bottom, rc_.right, rc_.bottom); }

void SidePanel::Layout(const RECT& area, float ui) {
  area_ = area;
  ui_ = ui;
  const float u = ui;
  if (!content_ && slide_.V() <= 0.001f && shown_) {
    shown_ = nullptr;
    group_.Clear();
  }
  const float w = std::round(kWidth * u);
  const float left = static_cast<float>(area.right - Width(ui));
  rc_ = RectF(left, static_cast<float>(area.top), left + w, static_cast<float>(area.bottom));
  titleRc_ = RectF(rc_.left + 20 * u, rc_.top, rc_.right - (12 + kCloseW + 8) * u, rc_.top + kTitleH * u);
  const float cy = std::round(rc_.top + kTitleH * u / 2);
  const float cx = rc_.right - (12 + kCloseW) * u;
  close_.rc = RectF(std::round(cx), cy - kCloseW * u / 2, std::round(cx + kCloseW * u), cy + kCloseW * u / 2);
  close_.visible = content_ != nullptr;
  group_.ui = ui;
  group_.bounds = rc_;
  if (shown_) {
    std::vector<Control*> controls = shown_->Layout(Body(), ui);
    if (content_) group_.Assign(controls);
  }
}

void SidePanel::Draw(Canvas& c, bool windowActive) {
  if (!Visible() || !shown_) return;
  const Palette& p = theme::P();
  c.Fill(rc_, p.chrome);
  c.Fill(RectF(rc_.left, rc_.top, rc_.left + 1, rc_.bottom), p.chromeBorder);
  c.PushClip(rc_);
  c.Text(shown_->Title(), titleRc_, p.text, 16 * ui_, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  DrawButton(c, close_, ui_, windowActive);
  shown_->Draw(c);
  group_.Draw(c);
  c.PopClip();
}

bool SidePanel::Hit(int x, int y) const {
  return content_ && (Contains(rc_, (float)x, (float)y) || group_.HitPopup((float)x, (float)y));
}

bool SidePanel::OnMouseMove(int x, int y) {
  if (!content_) return false;
  const float fx = (float)x, fy = (float)y;
  if (pressed_) return content_->OnMouseMove(fx, fy);
  bool changed = group_.OnMouseMove(fx, fy);
  if (group_.Dragging() || group_.Hit(fx, fy)) {
    set_.OnMouseLeave();
    content_->OnMouseLeave();
    return true;
  }
  changed = set_.OnMouseMove(fx, fy) || changed;
  if (Contains(Body(), fx, fy)) changed = content_->OnMouseMove(fx, fy) || changed;
  else content_->OnMouseLeave();
  return changed;
}

void SidePanel::OnMouseLeave() {
  group_.OnMouseLeave();
  set_.OnMouseLeave();
  if (content_) content_->OnMouseLeave();
}

void SidePanel::OnMouseDown(int x, int y, bool dbl, bool shift) {
  if (!content_) return;
  const float fx = (float)x, fy = (float)y;
  if (group_.OnMouseDown(fx, fy, dbl, shift)) {
    if (contentFocus_) content_->SetFocus(false, false);
    contentFocus_ = false;
    return;
  }
  if (set_.OnMouseDown(fx, fy)) return;
  if (Contains(Body(), fx, fy) && content_->OnMouseDown(fx, fy, dbl)) {
    pressed_ = true;
    if (content_->TakesFocus() && !contentFocus_) {
      contentFocus_ = true;
      content_->SetFocus(true, false);
    }
  }
}

bool SidePanel::OnMouseUp(int x, int y) {
  if (!content_) return false;
  const float fx = (float)x, fy = (float)y;
  group_.OnMouseUp(fx, fy);
  if (pressed_) {
    pressed_ = false;
    content_->OnMouseUp(fx, fy);
  }
  return set_.OnMouseUp(fx, fy) != 0;
}

bool SidePanel::OnWheel(int delta, int x, int y) {
  if (!content_) return false;
  if (group_.OnWheel(delta, (float)x, (float)y)) return true;
  return content_->OnWheel(delta, (float)x, (float)y);
}

std::wstring SidePanel::TooltipAt(int x, int y) const {
  if (!content_) return {};
  const float fx = (float)x, fy = (float)y;
  if (group_.Hit(fx, fy)) return group_.TooltipAt(fx, fy);
  if (close_.visible && Contains(close_.rc, fx, fy)) return close_.tooltip;
  return content_->TooltipAt(fx, fy);
}

bool SidePanel::OverText(int x, int y) const { return content_ && group_.OverText((float)x, (float)y); }

bool SidePanel::FocusIn(bool forward, bool resume) {
  if (!content_) return false;
  const auto& controls = group_.Controls();
  if (resume && last_ && last_->visible && last_->enabled &&
      std::find(controls.begin(), controls.end(), last_) != controls.end()) {
    group_.Focus(last_, true);
    return true;
  }
  if (group_.MoveFocus(forward)) return true;
  if (!content_->TakesFocus()) return false;
  contentFocus_ = true;
  content_->SetFocus(true, true);
  group_.Invalidate();
  return true;
}

void SidePanel::Blur() {
  if (group_.Focused()) last_ = group_.Focused();
  group_.Blur();
  if (contentFocus_ && content_) content_->SetFocus(false, false);
  contentFocus_ = false;
}

bool SidePanel::OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt) {
  if (!content_) return false;
  if (group_.Focused()) return group_.OnKeyDown(vk, ctrl, shift, alt);
  if (contentFocus_ && !alt) return content_->OnKeyDown(vk, ctrl, shift);
  return false;
}

bool SidePanel::OnChar(wchar_t ch) { return content_ && group_.OnChar(ch); }

void SidePanel::Describe(uia::Tree& t, int parent, const std::function<void()>& close) {
  if (!content_) return;
  int pane = t.Add(parent, 8, UIA_PaneControlTypeId, content_->Title(), rc_);
  uia::AddButton(t, pane, 1, close_, close);
  content_->Describe(t, pane, [this] {
    if (!content_ || !content_->TakesFocus()) return;
    group_.Blur();
    if (!contentFocus_) content_->SetFocus(true, true);
    contentFocus_ = true;
  });
  group_.Describe(t, pane);
}
