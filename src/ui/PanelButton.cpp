#include "ui/PanelButton.h"

#include <algorithm>
#include <cmath>

#include "ui/Theme.h"

using namespace gfx;

void PanelButton::Draw(Canvas& c) {
  if (!visible) return;
  const Palette& p = theme::P();
  const float u = Ui();
  D2D1_COLOR_F bg = pressed_ ? p.pressed : hover_ ? p.hover : checked ? p.selection : p.inputBg;
  if (!enabled) bg = Alpha(bg, 0.5f);
  c.FillRound(rc, 4 * u, bg);
  c.StrokeRound(rc, 4 * u, checked ? p.accentText : p.inputBorder, 1);
  const D2D1_COLOR_F fg = !enabled ? p.textDisabled : p.text;
  const float w = c.MeasureText(text, textSize * u);
  const float x = std::max(rc.left + 3 * u, std::round((rc.left + rc.right - w) / 2));
  c.Text(text, RectF(x, rc.top, x + w + 2 * u, rc.bottom), fg, textSize * u);
  if (FocusVisible()) ctl::DrawFocusRing(c, rc, u);
}

bool PanelButton::OnMouseMove(float x, float y) {
  const bool h = enabled && Hit(x, y);
  const bool changed = h != hover_;
  hover_ = h;
  return changed;
}

void PanelButton::OnMouseDown(float x, float y, bool, bool) {
  pressed_ = enabled && Hit(x, y);
  if (pressed_) TakeFocus();
}

void PanelButton::OnMouseUp(float x, float y) {
  const bool fire = pressed_ && Hit(x, y);
  pressed_ = false;
  if (fire) Invoke();
}

bool PanelButton::OnKeyDown(UINT vk, bool, bool, bool) {
  if (!enabled || (vk != VK_RETURN && vk != VK_SPACE)) return false;
  Invoke();
  return true;
}

void PanelButton::Invoke() {
  if (!enabled) return;
  if (toggle) checked = !checked;
  if (onInvoke) onInvoke();
}

void PanelButton::Describe(uia::Tree& t, int parent) {
  if (!visible) return;
  const int i = t.Add(parent, id, UIA_ButtonControlTypeId, text, rc);
  t[i].enabled = enabled;
  t[i].focusable = true;
  t[i].focus = [this] { TakeFocus(); };
  t[i].focused = Focused();
  t[i].invoke = [this] { Invoke(); };
  if (toggle) t[i].toggle = checked ? 1 : 0;
}

