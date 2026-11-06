#include "PasswordCard.h"

#include <cmath>

#include "ui/Anim.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Uia.h"

using namespace gfx;

PasswordCard::PasswordCard(ViewHost& host, std::function<void(const std::wstring&)> submit)
    : host_(host), submit_(std::move(submit)) {
  box_.password = true;
  box_.placeholder = L"Password";
  box_.onEnter = [this](bool) { Submit(); };
  open_.id = 1;
  open_.text = L"Open";
  open_.style = ButtonStyle::Primary;
  buttons_.Add(&open_);
}

void PasswordCard::Draw(Canvas& c, const RECT& vp, float u, const std::wstring& title) {
  const Palette& p = theme::P();
  const float w = 360 * u, h = (error_.empty() ? 196 : 222) * u;
  float x = std::round((vp.left + vp.right) / 2.0f - w / 2), y = std::round((vp.top + vp.bottom) / 2.0f - h / 2 - 20 * u);
  D2D1_RECT_F card = RectF(x, y, x + w, y + h);
  c.SoftShadow(card, 8 * u, 16 * u, p.popupShadow, 4 * u);
  c.FillRound(card, 8 * u, p.popupBg);
  c.StrokeRound(card, 8 * u, p.popupBorder, 1);

  float cy = y + 24 * u;
  c.FillEllipse(x + w / 2, cy + 18 * u, 22 * u, 22 * u, Alpha(p.accent, 0.14f));
  c.Icon(icon::Lock, RectF(x, cy, x + w, cy + 36 * u), p.accentText, 18 * u);
  cy += 50 * u;
  c.Text(L"“" + title + L"” is protected", RectF(x + 20 * u, cy, x + w - 20 * u, cy + 22 * u), p.text, 15 * u,
         DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center);
  cy += 24 * u;
  c.Text(L"Enter the password to open it.", RectF(x + 20 * u, cy, x + w - 20 * u, cy + 20 * u), p.textSecondary,
         13 * u, DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
  cy += 32 * u;
  box_.rc = RectF(x + 24 * u, cy, x + w - 24 * u - 88 * u, cy + 32 * u);
  open_.rc = RectF(box_.rc.right + 8 * u, cy, x + w - 24 * u, cy + 32 * u);
  box_.Draw(c, u, box_.focused && static_cast<int>(anim::Now() / 530) % 2 == 0);
  DrawButton(c, open_, u);
  if (!error_.empty()) {
    cy += 40 * u;
    c.Text(error_, RectF(x + 24 * u, cy, x + w - 24 * u, cy + 20 * u), p.error, 12.5f * u);
  }
}

void PasswordCard::OnMouseDown(int x, int y, UINT mods, bool doubleClick, float ui) {
  if (Contains(box_.rc, (float)x, (float)y)) {
    host_.ViewFocusTextBox(&box_);
    if (doubleClick) box_.OnDoubleClick();
    else box_.OnMouseDown((float)x, (float)y, (mods & kModShift) != 0, ui);
  } else {
    buttons_.OnMouseDown((float)x, (float)y);
  }
  host_.ViewInvalidate();
}

void PasswordCard::OnMouseMove(int x, int y, float ui) {
  if (GetKeyState(VK_LBUTTON) < 0 && box_.focused && Contains(Inflate(box_.rc, 200, 50), (float)x, (float)y))
    box_.OnMouseDrag((float)x, (float)y, ui);
  if (buttons_.OnMouseMove((float)x, (float)y)) host_.ViewInvalidate();
}

void PasswordCard::OnMouseUp(int x, int y) {
  if (buttons_.OnMouseUp((float)x, (float)y) == 1) Submit();
  host_.ViewInvalidate();
}

HCURSOR PasswordCard::CursorAt(int x, int y) const {
  static HCURSOR arrow = LoadCursor(nullptr, IDC_ARROW), ibeam = LoadCursor(nullptr, IDC_IBEAM);
  return Contains(box_.rc, (float)x, (float)y) ? ibeam : arrow;
}

void PasswordCard::DescribeUia(uia::Tree& t, int parent) {
  uia::AddTextBox(t, parent, 1, box_, L"Password", [this] { host_.ViewFocusTextBox(&box_); });
  uia::AddButton(t, parent, 2, open_, [this] { Submit(); });
}

void PasswordCard::Wrong() {
  error_ = L"That password isn’t right. Try again.";
  box_.SelectAll();
  box_.ShakeError();
}

void PasswordCard::Accepted() {
  box_.SetText(L"");
  error_.clear();
}
