#include "Widgets.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

#include "Icons.h"
#include "Theme.h"
#include "Util.h"

using namespace gfx;

namespace {

// Page-layout icons that the icon font doesn't have.
void DrawCustomIcon(Canvas& c, wchar_t glyph, const D2D1_RECT_F& r, D2D1_COLOR_F color, float size) {
  const float cx = std::round((r.left + r.right) / 2), cy = std::round((r.top + r.bottom) / 2);
  const float s = size;
  const float stroke = std::max(1.0f, std::round(size / 12.0f));
  const float rad = s * 0.08f;
  auto page = [&](float l, float t, float w, float h) {
    c.StrokeRound(RectF(std::round(l) + 0.5f, std::round(t) + 0.5f, std::round(l + w) - 0.5f, std::round(t + h) - 0.5f),
                  rad, color, stroke);
  };
  switch (glyph) {
    case icon::ViewSingle: page(cx - s * 0.30f, cy - s * 0.40f, s * 0.60f, s * 0.80f); break;
    case icon::ViewContinuous:
      page(cx - s * 0.30f, cy - s * 0.50f, s * 0.60f, s * 0.44f);
      page(cx - s * 0.30f, cy + s * 0.06f, s * 0.60f, s * 0.44f);
      break;
    case icon::ViewFacing:
      page(cx - s * 0.48f, cy - s * 0.36f, s * 0.44f, s * 0.72f);
      page(cx + s * 0.04f, cy - s * 0.36f, s * 0.44f, s * 0.72f);
      break;
  }
}

}  // namespace

void DrawButton(Canvas& c, const Button& b, float ui, bool windowActive) {
  if (!b.visible) return;
  const Palette& p = theme::P();
  const float hv = b.enabled ? b.hover.V() : 0.f;
  const float pv = b.enabled ? b.press.V() : 0.f;
  D2D1_COLOR_F fg = b.enabled ? p.text : p.textDisabled;
  const float radius = 4 * ui;

  switch (b.style) {
    case ButtonStyle::Subtle: {
      if (b.checked) c.FillRound(b.rc, radius, Alpha(p.accent, p.dark ? 0.22f : 0.14f));
      c.FillRound(b.rc, radius, Alpha(p.hover, hv));
      c.FillRound(b.rc, radius, Alpha(p.pressed, pv));
      if (b.checked && b.enabled) fg = p.accentText;
      break;
    }
    case ButtonStyle::Primary: {
      D2D1_COLOR_F bg = b.enabled ? Mix(Mix(p.accent, p.accentHover, hv), p.accentPressed, pv) : p.textDisabled;
      c.FillRound(b.rc, radius, bg);
      fg = b.enabled ? p.onAccent : p.chrome;
      break;
    }
    case ButtonStyle::Standard: {
      c.FillRound(b.rc, radius, p.dark ? Rgb(0xFFFFFF, 0.06f) : Rgb(0xFFFFFF, 0.9f));
      c.StrokeRound(b.rc, radius, p.inputBorder, 1);
      c.FillRound(b.rc, radius, Alpha(p.hover, hv));
      c.FillRound(b.rc, radius, Alpha(p.pressed, pv));
      break;
    }
    case ButtonStyle::Caption: {
      c.Fill(b.rc, Alpha(p.hover, hv));
      c.Fill(b.rc, Alpha(p.pressed, pv));
      if (!windowActive) fg = p.textDisabled;
      break;
    }
    case ButtonStyle::CaptionClose: {
      c.Fill(b.rc, Alpha(p.closeHover, hv));
      c.Fill(b.rc, Alpha(Rgb(0x000000), pv * 0.15f));
      if (hv > 0.01f) fg = Mix(fg, Rgb(0xFFFFFF), hv);
      else if (!windowActive) fg = p.textDisabled;
      break;
    }
  }

  const float glyphPx = b.glyphSize * ui;
  const float textPx = 13 * ui;
  const auto weight = b.style == ButtonStyle::Primary ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_REGULAR;
  float textW = b.text.empty() ? 0 : c.MeasureText(b.text, textPx, weight);
  float chevW = b.dropdown ? 14 * ui : 0;
  float gap = (b.glyph && !b.text.empty()) ? 8 * ui : 0;
  float glyphW = b.glyph ? glyphPx : 0;
  float total = glyphW + gap + textW + chevW;
  float x = std::round((b.rc.left + b.rc.right - total) / 2);
  if (b.glyph >= 0xF8F0 && b.glyph <= 0xF8FF) {
    DrawCustomIcon(c, b.glyph, RectF(x, b.rc.top, x + glyphW, b.rc.bottom), fg, glyphPx);
    x += glyphW + gap;
  } else if (b.glyph) {
    c.Icon(b.glyph, RectF(x, b.rc.top, x + glyphW, b.rc.bottom), fg, glyphPx);
    x += glyphW + gap;
  }
  if (!b.text.empty()) {
    c.Text(b.text, RectF(x, b.rc.top, x + textW + 2, b.rc.bottom), fg, textPx, weight);
    x += textW;
  }
  if (b.dropdown) {
    c.Icon(icon::ChevronDown, RectF(x + 2 * ui, b.rc.top, x + chevW, b.rc.bottom), Alpha(fg, 0.8f), 9 * ui);
  }
}

Button* ButtonSet::HitTest(float x, float y) const {
  for (Button* b : buttons_)
    if (b->Hit(x, y)) return b;
  return nullptr;
}

bool ButtonSet::OnMouseMove(float x, float y) {
  Button* h = HitTest(x, y);
  if (h == hovered_) return false;
  if (hovered_) hovered_->hover.To(0, 150);
  hovered_ = h;
  if (hovered_) hovered_->hover.To(1, 80);
  return true;
}

void ButtonSet::OnMouseLeave() {
  if (hovered_) hovered_->hover.To(0, 150);
  hovered_ = nullptr;
  if (pressed_) pressed_->press.To(0, 100);
  pressed_ = nullptr;
}

bool ButtonSet::OnMouseDown(float x, float y) {
  pressed_ = HitTest(x, y);
  if (pressed_) pressed_->press.To(1, 40);
  return pressed_ != nullptr;
}

int ButtonSet::OnMouseUp(float x, float y) {
  Button* p = pressed_;
  pressed_ = nullptr;
  if (!p) return 0;
  p->press.To(0, 120);
  return p->Hit(x, y) ? p->id : 0;
}

void ButtonSet::Reset() {
  for (Button* b : buttons_) {
    b->hover.Snap(0);
    b->press.Snap(0);
  }
  hovered_ = pressed_ = nullptr;
}

// ---------------------------------------------------------------------------
// TextBox

namespace {

ComPtr<IDWriteTextLayout> MakeLayout(const std::wstring& s, float sizePx) {
  ComPtr<IDWriteTextLayout> layout;
  IDWriteTextFormat* fmt = Canvas::Format(sizePx, DWRITE_FONT_WEIGHT_REGULAR, false);
  fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  DWrite()->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt, 100000.f, sizePx * 2, &layout);
  return layout;
}

bool IsWordChar(wchar_t c) { return iswalnum(c) || c == L'_'; }

}  // namespace

std::wstring TextBox::Display() const {
  return password ? std::wstring(text_.size(), L'●') : text_;
}

void TextBox::SetText(const std::wstring& t, bool selectAll) {
  text_ = t.substr(0, maxLength);
  caret_ = static_cast<int>(text_.size());
  anchor_ = selectAll ? 0 : caret_;
  scroll_ = 0;
}

void TextBox::SelectAll() {
  anchor_ = 0;
  caret_ = static_cast<int>(text_.size());
}

void TextBox::Focus(bool f) {
  focused = f;
  if (!f) anchor_ = caret_;
}

float TextBox::XOf(int index, float ui) {
  std::wstring d = Display();
  if (d.empty() || index <= 0) return 0;
  auto layout = MakeLayout(d, fontSize * ui);
  if (!layout) return 0;
  FLOAT x = 0, y = 0;
  DWRITE_HIT_TEST_METRICS m{};
  layout->HitTestTextPosition(static_cast<UINT32>(std::min<size_t>(index, d.size())), FALSE, &x, &y, &m);
  return x;
}

int TextBox::IndexAt(float x, float ui) {
  std::wstring d = Display();
  if (d.empty()) return 0;
  auto layout = MakeLayout(d, fontSize * ui);
  if (!layout) return 0;
  BOOL trailing = FALSE, inside = FALSE;
  DWRITE_HIT_TEST_METRICS m{};
  layout->HitTestPoint(x, fontSize * ui, &trailing, &inside, &m);
  int idx = static_cast<int>(m.textPosition) + (trailing ? static_cast<int>(m.length) : 0);
  return std::clamp(idx, 0, static_cast<int>(d.size()));
}

void TextBox::Draw(Canvas& c, float ui, bool caretVisible) {
  const Palette& p = theme::P();
  const float radius = 4 * ui;
  D2D1_RECT_F r = rc;
  float shake = ShakeOffset() * ui;
  r.left += shake;
  r.right += shake;

  c.FillRound(r, radius, focused ? (p.dark ? Rgb(0x1F1F1F) : Rgb(0xFFFFFF)) : p.inputBg);
  c.StrokeRound(r, radius, p.inputBorder, 1);
  // Bottom accent line, like Windows 11 text boxes.
  D2D1_COLOR_F lineColor = error ? p.error : focused ? p.accent : p.inputBorderBottom;
  float lineH = (focused || error) ? 2 * ui : 1;
  c.PushClip(RectF(r.left, r.bottom - lineH, r.right, r.bottom));
  c.FillRound(r, radius, lineColor);
  c.PopClip();

  const float pad = 8 * ui;
  D2D1_RECT_F inner = RectF(r.left + pad, r.top, r.right - pad, r.bottom - 1);
  const float sizePx = fontSize * ui;
  std::wstring d = Display();
  const float innerW = Width(inner);

  float textW = d.empty() ? 0 : c.MeasureText(d, sizePx);
  float caretX = XOf(caret_, ui);
  if (align == Align::Center && textW < innerW) {
    scroll_ = -(innerW - textW) / 2;
  } else {
    if (scroll_ < 0) scroll_ = 0;
    if (caretX - scroll_ > innerW - 1) scroll_ = caretX - innerW + 1;
    if (caretX - scroll_ < 0) scroll_ = caretX;
    if (scroll_ > 0 && textW - scroll_ < innerW) scroll_ = std::max(0.f, textW - innerW + 1);
  }

  c.PushClip(inner);
  if (d.empty()) {
    if (!placeholder.empty() && !focused) {
      c.Text(placeholder, inner, p.textSecondary, sizePx, DWRITE_FONT_WEIGHT_REGULAR, align);
    } else if (!placeholder.empty() && focused) {
      c.Text(placeholder, inner, Alpha(p.textSecondary, 0.7f), sizePx, DWRITE_FONT_WEIGHT_REGULAR, align);
    }
  } else {
    if (focused && HasSelection()) {
      float x1 = XOf(std::min(caret_, anchor_), ui) - scroll_;
      float x2 = XOf(std::max(caret_, anchor_), ui) - scroll_;
      float h = sizePx * 1.35f;
      float cy = (inner.top + inner.bottom) / 2;
      c.Fill(RectF(inner.left + x1, cy - h / 2, inner.left + x2, cy + h / 2), p.selection);
    }
    D2D1_RECT_F tr = RectF(inner.left - scroll_, inner.top, inner.left - scroll_ + textW + sizePx, inner.bottom);
    c.Text(d, tr, p.text, sizePx);
  }
  if (focused && caretVisible) {
    float x = std::round(inner.left + caretX - scroll_);
    float h = sizePx * 1.3f;
    float cy = (inner.top + inner.bottom) / 2;
    c.Line(x + 0.5f, cy - h / 2, x + 0.5f, cy + h / 2, p.text, std::max(1.f, std::floor(ui)));
  }
  c.PopClip();
}

D2D1_POINT_2F TextBox::CaretPos(float ui) {
  float x = rc.left + 8 * ui + XOf(caret_, ui) - std::max(0.f, scroll_);
  return {x, rc.top};
}

void TextBox::SaveUndo() {
  undo_ = text_;
  undoCaret_ = caret_;
}

void TextBox::Changed() {
  if (onChange) onChange();
}

void TextBox::DeleteSelection() {
  if (!HasSelection()) return;
  int a = std::min(caret_, anchor_), b = std::max(caret_, anchor_);
  text_.erase(a, b - a);
  caret_ = anchor_ = a;
}

void TextBox::Insert(const std::wstring& s) {
  std::wstring clean;
  for (wchar_t ch : s) {
    if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
    if (ch < 32) continue;
    if (digitsOnly && (ch < L'0' || ch > L'9')) continue;
    clean += ch;
  }
  if (clean.empty() && !s.empty()) return;
  SaveUndo();
  DeleteSelection();
  size_t room = maxLength > text_.size() ? maxLength - text_.size() : 0;
  if (clean.size() > room) clean.resize(room);
  text_.insert(caret_, clean);
  caret_ += static_cast<int>(clean.size());
  anchor_ = caret_;
  Changed();
}

bool TextBox::OnChar(wchar_t ch) {
  if (ch < 32 || ch == 127) return ch == 8 || ch == 13 || ch == 27 || ch == 1 || ch == 3 || ch == 22 || ch == 24 || ch == 26;
  Insert(std::wstring(1, ch));
  return true;
}

bool TextBox::OnKeyDown(UINT vk, bool ctrl, bool shift) {
  if (onKey && onKey(vk, shift)) return true;
  const int len = static_cast<int>(text_.size());
  auto move = [&](int pos) {
    caret_ = std::clamp(pos, 0, len);
    if (!shift) anchor_ = caret_;
  };
  auto wordLeft = [&](int pos) {
    while (pos > 0 && !IsWordChar(text_[pos - 1])) --pos;
    while (pos > 0 && IsWordChar(text_[pos - 1])) --pos;
    return pos;
  };
  auto wordRight = [&](int pos) {
    while (pos < len && IsWordChar(text_[pos])) ++pos;
    while (pos < len && !IsWordChar(text_[pos])) ++pos;
    return pos;
  };
  switch (vk) {
    case VK_LEFT:
      if (!shift && HasSelection() && !ctrl) move(std::min(caret_, anchor_));
      else move(ctrl ? wordLeft(caret_) : caret_ - 1);
      return true;
    case VK_RIGHT:
      if (!shift && HasSelection() && !ctrl) move(std::max(caret_, anchor_));
      else move(ctrl ? wordRight(caret_) : caret_ + 1);
      return true;
    case VK_HOME: move(0); return true;
    case VK_END: move(len); return true;
    case VK_BACK:
      if (HasSelection()) {
        SaveUndo();
        DeleteSelection();
      } else if (caret_ > 0) {
        SaveUndo();
        int from = ctrl ? wordLeft(caret_) : caret_ - 1;
        text_.erase(from, caret_ - from);
        caret_ = anchor_ = from;
      } else {
        return true;
      }
      Changed();
      return true;
    case VK_DELETE:
      if (HasSelection()) {
        SaveUndo();
        DeleteSelection();
      } else if (caret_ < len) {
        SaveUndo();
        int to = ctrl ? wordRight(caret_) : caret_ + 1;
        text_.erase(caret_, to - caret_);
      } else {
        return true;
      }
      Changed();
      return true;
    case VK_RETURN:
      if (onEnter) onEnter(shift);
      return true;
    case VK_ESCAPE:
      if (onEscape) onEscape();
      return true;
  }
  if (ctrl) {
    switch (vk) {
      case 'A': SelectAll(); return true;
      case 'C':
      case 'X':
        if (HasSelection() && !password) {
          int a = std::min(caret_, anchor_), b = std::max(caret_, anchor_);
          util::SetClipboardText(text_.substr(a, b - a));
          if (vk == 'X') {
            SaveUndo();
            DeleteSelection();
            Changed();
          }
        }
        return true;
      case 'V': Insert(util::GetClipboardText()); return true;
      case 'Z': {
        std::wstring cur = text_;
        int curCaret = caret_;
        text_ = undo_;
        caret_ = anchor_ = std::min(undoCaret_, static_cast<int>(text_.size()));
        undo_ = cur;
        undoCaret_ = curCaret;
        Changed();
        return true;
      }
    }
  }
  return false;
}

void TextBox::OnMouseDown(float x, float y, bool shift, float ui) {
  (void)y;
  float innerLeft = rc.left + 8 * ui;
  caret_ = IndexAt(x - innerLeft + scroll_, ui);
  if (!shift) anchor_ = caret_;
}

void TextBox::OnMouseDrag(float x, float y, float ui) {
  (void)y;
  float innerLeft = rc.left + 8 * ui;
  caret_ = IndexAt(x - innerLeft + scroll_, ui);
}

void TextBox::OnDoubleClick() {
  if (password || text_.empty()) {
    SelectAll();
    return;
  }
  int len = static_cast<int>(text_.size());
  int a = std::min(caret_, len), b = a;
  while (a > 0 && IsWordChar(text_[a - 1])) --a;
  while (b < len && IsWordChar(text_[b])) ++b;
  if (a == b) {
    SelectAll();
    return;
  }
  anchor_ = a;
  caret_ = b;
}

void TextBox::ShakeError() {
  error = true;
  shakeStart_ = anim::Now();
  anim::KeepAlive(shakeStart_ + 450);
}

float TextBox::ShakeOffset() const {
  if (!theme::AnimationsEnabled()) return 0;
  double t = (anim::Now() - shakeStart_) / 400.0;
  if (t < 0 || t >= 1) return 0;
  return static_cast<float>(std::sin(t * 6.2831853 * 4) * 7.0 * (1 - t));
}
