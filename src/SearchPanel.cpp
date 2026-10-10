#include "SearchPanel.h"

#include <cmath>

#include "Icons.h"
#include "Theme.h"

using namespace gfx;

SearchPanel::SearchPanel() {
  box_.placeholder = L"Find in document";
  box_.maxLength = 256;
  prev_.id = CmdFindPrev;
  prev_.glyph = icon::ChevronUp;
  prev_.tooltip = L"Previous match (Shift+Enter)";
  next_.id = CmdFindNext;
  next_.glyph = icon::ChevronDown;
  next_.tooltip = L"Next match (Enter)";
  case_.id = CmdSearchCase;
  case_.glyph = icon::FontCase;
  case_.tooltip = L"Match case";
  close_.id = CmdCloseFind;
  close_.glyph = icon::Cancel;
  close_.glyphSize = 12;
  close_.tooltip = L"Close (Esc)";
  prev_.glyphSize = next_.glyphSize = 13;
  set_.Add(&prev_);
  set_.Add(&next_);
  set_.Add(&case_);
  set_.Add(&close_);
}

void SearchPanel::Open() {
  open_ = true;
  show_.To(1, 160);
}

void SearchPanel::Close() {
  open_ = false;
  show_.To(0, 120);
  box_.Focus(false);
}

D2D1_RECT_F SearchPanel::Card() const {
  const float u = ui_;
  float w = std::min(420 * u, static_cast<float>(area_.right - area_.left) - 24 * u);
  float h = 48 * u;
  float slide = (1 - show_.V()) * -12 * u;
  float right = area_.right - 20 * u;
  float top = area_.top + 12 * u + slide;
  return RectF(std::round(right - w), std::round(top), std::round(right), std::round(top + h));
}

void SearchPanel::Layout(const RECT& docArea, float ui) {
  area_ = docArea;
  ui_ = ui;
  const float u = ui;
  D2D1_RECT_F c = Card();
  float b = 30 * u;
  float cy = (c.top + c.bottom) / 2;
  float x = c.right - 8 * u - b;
  close_.rc = RectF(x, cy - b / 2, x + b, cy + b / 2);
  x -= b + 6 * u;
  case_.rc = RectF(x, cy - b / 2, x + b, cy + b / 2);
  x -= b + 2 * u;
  next_.rc = RectF(x, cy - b / 2, x + b, cy + b / 2);
  x -= b;
  prev_.rc = RectF(x, cy - b / 2, x + b, cy + b / 2);
  x -= 6 * u;
  status_ = RectF(x - 74 * u, c.top, x, c.bottom);
  box_.rc = RectF(c.left + 8 * u, cy - 15 * u, status_.left - 6 * u, cy + 15 * u);
}

void SearchPanel::Draw(Canvas& c, const std::wstring& status, bool caretVisible, bool noMatches) {
  float v = show_.V();
  if (v <= 0.01f && !open_) return;
  const Palette& p = theme::P();
  D2D1_RECT_F card = Card();
  ComPtr<ID2D1Layer> layer;
  bool fade = v < 0.99f;
  if (fade) {
    c.DC()->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                            D2D1::IdentityMatrix(), v),
                      nullptr);
  }
  c.SoftShadow(card, 8 * ui_, 14 * ui_, p.popupShadow, 4 * ui_);
  c.FillRound(card, 8 * ui_, p.popupBg);
  c.StrokeRound(card, 8 * ui_, p.popupBorder, 1);
  box_.error = noMatches;
  box_.Draw(c, ui_, caretVisible);
  c.Text(status, status_, noMatches ? p.error : p.textSecondary, 12 * ui_, DWRITE_FONT_WEIGHT_REGULAR, Align::Trailing);
  DrawButton(c, prev_, ui_);
  DrawButton(c, next_, ui_);
  DrawButton(c, case_, ui_);
  DrawButton(c, close_, ui_);
  if (fade) c.DC()->PopLayer();
}

bool SearchPanel::Hit(int x, int y) const { return Visible() && Contains(Card(), (float)x, (float)y); }

bool SearchPanel::OnMouseMove(int x, int y) { return set_.OnMouseMove((float)x, (float)y); }
void SearchPanel::OnMouseLeave() { set_.OnMouseLeave(); }

void SearchPanel::OnMouseDown(int x, int y, bool shift, bool dbl) {
  if (Contains(box_.rc, (float)x, (float)y)) {
    if (dbl) box_.OnDoubleClick();
    else box_.OnMouseDown((float)x, (float)y, shift, ui_);
    return;
  }
  set_.OnMouseDown((float)x, (float)y);
}

void SearchPanel::OnMouseDrag(int x, int y) { box_.OnMouseDrag((float)x, (float)y, ui_); }

int SearchPanel::OnMouseUp(int x, int y) { return set_.OnMouseUp((float)x, (float)y); }

std::wstring SearchPanel::TooltipAt(int x, int y) const {
  for (const Button* b : {&prev_, &next_, &case_, &close_})
    if (Contains(b->rc, (float)x, (float)y)) return b->tooltip;
  return {};
}
