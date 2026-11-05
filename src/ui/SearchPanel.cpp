#include "ui/SearchPanel.h"

#include <cmath>

#include "ui/Icons.h"
#include "ui/Theme.h"

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
  words_.id = CmdSearchWords;
  words_.text = L"ab";  // underlined in Draw, as other programs show whole words
  words_.tooltip = L"Whole words";
  close_.id = CmdCloseFind;
  close_.glyph = icon::Cancel;
  close_.glyphSize = 12;
  close_.tooltip = L"Close (Esc)";
  prev_.glyphSize = next_.glyphSize = 13;
  with_.placeholder = L"Replace with";
  with_.maxLength = 256;
  one_.id = CmdReplaceOne;
  one_.text = L"Replace";
  one_.tooltip = L"Replace this match (Enter)";
  all_.id = CmdReplaceAll;
  all_.text = L"All";
  all_.tooltip = L"Replace all matches";
  set_.Add(&one_);
  set_.Add(&all_);
  set_.Add(&prev_);
  set_.Add(&next_);
  set_.Add(&case_);
  set_.Add(&words_);
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
  with_.Focus(false);
}

D2D1_RECT_F SearchPanel::Card() const {
  const float u = ui_;
  float w = std::min(420 * u, static_cast<float>(area_.right - area_.left) - 24 * u);
  float h = (replace_ ? 86 : 48) * u;
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
  float cy = c.top + 24 * u;
  float x = c.right - 8 * u - b;
  close_.rc = RectF(x, cy - b / 2, x + b, cy + b / 2);
  x -= b + 6 * u;
  words_.rc = RectF(x, cy - b / 2, x + b, cy + b / 2);
  x -= b;
  case_.rc = RectF(x, cy - b / 2, x + b, cy + b / 2);
  x -= b + 2 * u;
  next_.rc = RectF(x, cy - b / 2, x + b, cy + b / 2);
  x -= b;
  prev_.rc = RectF(x, cy - b / 2, x + b, cy + b / 2);
  x -= 6 * u;
  status_ = RectF(x - 74 * u, c.top, x, c.bottom);
  box_.rc = RectF(c.left + 8 * u, cy - 15 * u, status_.left - 6 * u, cy + 15 * u);
  // The replace row.
  one_.visible = all_.visible = replace_;
  const float ry = cy + 38 * u;
  all_.rc = RectF(c.right - 8 * u - 52 * u, ry - b / 2, c.right - 8 * u, ry + b / 2);
  one_.rc = RectF(all_.rc.left - 4 * u - 76 * u, ry - b / 2, all_.rc.left - 4 * u, ry + b / 2);
  with_.rc = RectF(c.left + 8 * u, ry - 15 * u, one_.rc.left - 6 * u, ry + 15 * u);
}

void SearchPanel::Draw(Canvas& c, const std::wstring& status, bool caretVisible, bool noMatches, bool withCaret) {
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
  DrawButton(c, words_, ui_);
  {  // "ab" over a bracket: whole words
    const float u = ui_, mx = (words_.rc.left + words_.rc.right) / 2, y = std::round(words_.rc.bottom - 8 * u) + 0.5f;
    const D2D1_COLOR_F ink = words_.checked ? p.accentText : p.text;
    c.Line(mx - 7 * u, y, mx + 7 * u, y, ink, 1 * u);
    c.Line(mx - 7 * u, y - 3 * u, mx - 7 * u, y, ink, 1 * u);
    c.Line(mx + 7 * u, y - 3 * u, mx + 7 * u, y, ink, 1 * u);
  }
  DrawButton(c, close_, ui_);
  if (replace_) {
    one_.enabled = all_.enabled = !noMatches && !box_.Text().empty();
    with_.Draw(c, ui_, withCaret);
    DrawButton(c, one_, ui_);
    DrawButton(c, all_, ui_);
  }
  if (fade) c.DC()->PopLayer();
}

bool SearchPanel::Hit(int x, int y) const { return Visible() && Contains(Card(), (float)x, (float)y); }

bool SearchPanel::OnMouseMove(int x, int y) { return set_.OnMouseMove((float)x, (float)y); }
void SearchPanel::OnMouseLeave() { set_.OnMouseLeave(); }

void SearchPanel::OnMouseDown(int x, int y, bool shift, bool dbl) {
  for (TextBox* b : {&box_, &with_}) {
    if (b == &with_ && !replace_) continue;
    if (Contains(b->rc, (float)x, (float)y)) {
      if (dbl) b->OnDoubleClick();
      else b->OnMouseDown((float)x, (float)y, shift, ui_);
      return;
    }
  }
  set_.OnMouseDown((float)x, (float)y);
}

void SearchPanel::OnMouseDrag(int x, int y) {
  (with_.focused ? with_ : box_).OnMouseDrag((float)x, (float)y, ui_);
}

int SearchPanel::OnMouseUp(int x, int y) { return set_.OnMouseUp((float)x, (float)y); }

std::wstring SearchPanel::TooltipAt(int x, int y) const {
  for (const Button* b : {&prev_, &next_, &case_, &words_, &close_, &one_, &all_})
    if (b->visible && Contains(b->rc, (float)x, (float)y)) return b->tooltip;
  return {};
}

void SearchPanel::Describe(uia::Tree& t, int parent, const std::wstring& status, const std::function<void(int)>& run,
                           const std::function<void(TextBox*)>& focusBox) {
  if (!open_) return;
  int pane = t.Add(parent, CmdFind, UIA_PaneControlTypeId, L"Find", Card());
  t[pane].layer = 1;
  uia::AddTextBox(t, pane, 1, box_, box_.placeholder + L" (Ctrl+F)", [focusBox, this] { focusBox(&box_); });
  if (!status.empty()) {
    int s = t.Add(pane, 2, UIA_TextControlTypeId, status, status_);
    t[s].live = true;
  }
  for (Button* b : {&prev_, &next_, &close_}) {
    const int cmd = b->id;
    uia::AddButton(t, pane, cmd, *b, [run, cmd] { run(cmd); });
  }
  if (replace_) {
    uia::AddTextBox(t, pane, 3, with_, with_.placeholder + L" (Ctrl+H)", [focusBox, this] { focusBox(&with_); });
    for (Button* b : {&one_, &all_}) {
      const int cmd = b->id;
      uia::AddButton(t, pane, cmd, *b, [run, cmd] { run(cmd); });
    }
  }
  int matchCase = uia::AddButton(t, pane, case_.id, case_, nullptr);
  t[matchCase].toggle = case_.checked ? ToggleState_On : ToggleState_Off;
  t[matchCase].doToggle = [run] { run(CmdSearchCase); };
  int wholeWords = uia::AddButton(t, pane, words_.id, words_, nullptr);
  t[wholeWords].toggle = words_.checked ? ToggleState_On : ToggleState_Off;
  t[wholeWords].doToggle = [run] { run(CmdSearchWords); };
}
