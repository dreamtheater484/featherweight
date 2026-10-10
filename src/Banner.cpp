#include "Banner.h"

#include <algorithm>
#include <cmath>

#include "Icons.h"
#include "Theme.h"

using namespace gfx;

void Banner::Show(BannerKind kind, const std::wstring& text, std::vector<Action> actions, int autoHideMs) {
  kind_ = kind;
  text_ = text;
  actions_ = std::move(actions);
  visible_ = true;
  show_.To(1, 180);
  hideAt_ = autoHideMs > 0 ? anim::Now() + autoHideMs : 0;

  buttons_.clear();
  set_ = ButtonSet();
  for (const auto& a : actions_) {
    auto b = std::make_unique<Button>();
    b->id = a.id;
    b->text = a.text;
    b->style = a.primary ? ButtonStyle::Primary : ButtonStyle::Standard;
    set_.Add(b.get());
    buttons_.push_back(std::move(b));
  }
  close_.id = -1;
  close_.glyph = icon::Cancel;
  close_.glyphSize = 11;
  close_.tooltip = L"Dismiss";
  set_.Add(&close_);
}

void Banner::Hide() {
  if (!visible_) return;
  visible_ = false;
  show_.To(0, 160);
}

bool Banner::Tick() {
  if (visible_ && hideAt_ > 0 && !hovered_ && anim::Now() >= hideAt_) {
    Hide();
    return true;
  }
  return false;
}

D2D1_RECT_F Banner::Card() const {
  const float u = ui_;
  float h = 44 * u;
  float slide = (1 - show_.V()) * -14 * u;
  float cx = (area_.left + area_.right) / 2.0f;
  float top = area_.top + 12 * u + slide;
  return RectF(std::round(cx - width_ / 2), std::round(top), std::round(cx + width_ / 2), std::round(top + h));
}

void Banner::Layout(const RECT& docArea, float ui) {
  area_ = docArea;
  ui_ = ui;
  const float u = ui;
  Canvas measure;
  IDWriteTextFormat* fmt = Canvas::Format(13 * u, DWRITE_FONT_WEIGHT_REGULAR, false);
  ComPtr<IDWriteTextLayout> layout;
  DWrite()->CreateTextLayout(text_.c_str(), static_cast<UINT32>(text_.size()), fmt, 10000, 100, &layout);
  DWRITE_TEXT_METRICS m{};
  if (layout) layout->GetMetrics(&m);
  float textW = std::ceil(m.widthIncludingTrailingWhitespace);

  std::vector<float> bw;
  float buttonsW = 0;
  for (auto& b : buttons_) {
    IDWriteTextFormat* bf = Canvas::Format(13 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    ComPtr<IDWriteTextLayout> bl;
    DWrite()->CreateTextLayout(b->text.c_str(), static_cast<UINT32>(b->text.size()), bf, 10000, 100, &bl);
    DWRITE_TEXT_METRICS bm{};
    if (bl) bl->GetMetrics(&bm);
    float w = std::ceil(bm.widthIncludingTrailingWhitespace) + 28 * u;
    bw.push_back(w);
    buttonsW += w + 8 * u;
  }
  float maxW = static_cast<float>(docArea.right - docArea.left) - 32 * u;
  width_ = std::min(maxW, 16 * u + 20 * u + 10 * u + textW + 16 * u + buttonsW + 32 * u + 8 * u);
  width_ = std::max(width_, std::min(maxW, 240 * u));

  D2D1_RECT_F card = Card();
  float cy = (card.top + card.bottom) / 2;
  float x = card.right - 8 * u - 28 * u;
  close_.rc = RectF(x, cy - 14 * u, x + 28 * u, cy + 14 * u);
  x -= 8 * u;
  for (int i = static_cast<int>(buttons_.size()) - 1; i >= 0; --i) {
    x -= bw[i];
    buttons_[i]->rc = RectF(x, cy - 15 * u, x + bw[i], cy + 15 * u);
    x -= 8 * u;
  }
}

void Banner::Draw(Canvas& c) {
  float v = show_.V();
  if (v <= 0.01f && !visible_) return;
  const Palette& p = theme::P();
  const float u = ui_;
  D2D1_RECT_F card = Card();
  bool fade = v < 0.99f;
  if (fade)
    c.DC()->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                            D2D1::IdentityMatrix(), v),
                      nullptr);
  c.SoftShadow(card, 8 * u, 14 * u, p.popupShadow, 4 * u);
  c.FillRound(card, 8 * u, p.popupBg);
  c.StrokeRound(card, 8 * u, p.popupBorder, 1);

  wchar_t glyph = icon::Info;
  D2D1_COLOR_F col = p.info;
  switch (kind_) {
    case BannerKind::Success: glyph = icon::Completed; col = p.success; break;
    case BannerKind::Warning: glyph = icon::Warning; col = p.warning; break;
    case BannerKind::Error: glyph = icon::Error; col = p.error; break;
    default: break;
  }
  float cy = (card.top + card.bottom) / 2;
  c.Icon(glyph, RectF(card.left + 14 * u, cy - 10 * u, card.left + 34 * u, cy + 10 * u), col, 16 * u);
  float textRight = buttons_.empty() ? close_.rc.left - 8 * u : buttons_.front()->rc.left - 12 * u;
  c.Text(text_, RectF(card.left + 44 * u, card.top, textRight, card.bottom), p.text, 13 * u);
  for (auto& b : buttons_) DrawButton(c, *b, u);
  DrawButton(c, close_, u);
  if (fade) c.DC()->PopLayer();
}

double Banner::MsUntilHide() const {
  if (!visible_ || hideAt_ <= 0) return -1;
  return std::max(0.0, hideAt_ - anim::Now());
}

bool Banner::Hit(int x, int y) const { return Visible() && Contains(Card(), (float)x, (float)y); }

bool Banner::OnMouseMove(int x, int y) {
  hovered_ = Hit(x, y);
  return set_.OnMouseMove((float)x, (float)y);
}

void Banner::OnMouseLeave() {
  hovered_ = false;
  set_.OnMouseLeave();
  if (hideAt_ > 0) hideAt_ = std::max(hideAt_, anim::Now() + 1500);
}

void Banner::OnMouseDown(int x, int y) { set_.OnMouseDown((float)x, (float)y); }

int Banner::OnMouseUp(int x, int y) {
  int id = set_.OnMouseUp((float)x, (float)y);
  if (id != 0) Hide();
  return id;
}
