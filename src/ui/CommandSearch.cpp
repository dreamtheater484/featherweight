#include "ui/CommandSearch.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

#include "core/Strings.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

using namespace gfx;

namespace {

constexpr float kWidth = 520;  // DIPs
constexpr float kCardH = 40;
constexpr float kRowH = 36;
constexpr int kMaxRows = 8;

// The words of a label or a query, in lower case: "Night reading: dark pages" has four.
std::vector<std::wstring> Words(const std::wstring& s) {
  std::vector<std::wstring> words;
  std::wstring w;
  for (wchar_t ch : s) {
    if (iswalnum(ch)) {
      w += static_cast<wchar_t>(towlower(ch));
    } else if (!w.empty()) {
      words.push_back(w);
      w.clear();
    }
  }
  if (!w.empty()) words.push_back(w);
  return words;
}

}  // namespace

CommandSearch::CommandSearch() {
  box_.placeholder = L"Search for a command";
  box_.maxLength = 128;
  box_.onChange = [this] { Filter(); };
  box_.onEnter = [this](bool) { Run(hot_); };
  box_.onEscape = [this] {
    if (onEscape) onEscape();
  };
  box_.onKey = [this](UINT vk, bool) {
    const int n = static_cast<int>(results_.size());
    const int page = std::max(1, Rows() - 1);
    switch (vk) {
      case VK_UP: SetHot(std::max(0, hot_ - 1), true); return true;
      case VK_DOWN: SetHot(std::min(n - 1, hot_ + 1), true); return true;
      case VK_PRIOR: SetHot(std::max(0, hot_ - page), true); return true;
      case VK_NEXT: SetHot(std::min(n - 1, hot_ + page), true); return true;
      case VK_TAB: return true;  // the keyboard stays in the box
      default: return false;
    }
  };
}

int CommandSearch::Match(const std::wstring& label, const std::wstring& query) {
  const std::vector<std::wstring> want = Words(query), have = Words(label);
  if (want.empty()) return -1;
  std::vector<bool> used(have.size(), false);
  bool firstFirst = false;
  for (size_t q = 0; q < want.size(); ++q) {
    bool found = false;
    for (size_t k = 0; k < have.size() && !found; ++k) {
      if (used[k] || have[k].compare(0, want[q].size(), want[q]) != 0) continue;
      used[k] = found = true;
      if (q == 0 && k == 0) firstFirst = true;
    }
    if (!found) return -1;
  }
  if (ctl::StartsWithNoCase(label, fw::Trim(query))) return 0;
  return firstFirst ? 1 : 2;
}

void CommandSearch::Open(std::vector<CommandEntry> commands) {
  all_ = std::move(commands);
  open_ = true;
  show_.To(1, 120);
  box_.SetText(L"");
  results_.clear();
  query_.clear();
  hot_ = pressed_ = -1;
  scroll_ = 0;
  inList_ = false;
  popup_.open = false;
  popup_.show.Snap(0);
  Invalidate();
}

void CommandSearch::Close() {
  if (!open_) return;
  open_ = false;
  show_.To(0, 120);
  popup_.Close();
  box_.Focus(false);
  inList_ = false;
  pressed_ = -1;
  Invalidate();
}

void CommandSearch::Invalidate() const {
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void CommandSearch::Filter() {
  query_ = box_.Text();
  std::vector<std::pair<int, size_t>> found;  // how well, which
  for (size_t i = 0; i < all_.size(); ++i) {
    int score = Match(all_[i].label, query_);
    if (score >= 0) found.push_back({score, i});
  }
  std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  results_.clear();
  for (const auto& f : found) results_.push_back(all_[f.second]);
  // The first command that can run is the one Enter runs.
  hot_ = results_.empty() ? -1 : 0;
  for (size_t i = 0; i < results_.size(); ++i) {
    if (results_[i].enabled) {
      hot_ = static_cast<int>(i);
      break;
    }
  }
  scroll_ = 0;
  inList_ = false;
  const bool show = !fw::Trim(query_).empty();
  if (show && !popup_.open) popup_.Open();
  else if (!show && popup_.open) popup_.Close();
  EnsureVisible(hot_);
  Invalidate();
}

int CommandSearch::Rows() const {
  return std::max(1, std::min(static_cast<int>(results_.size()), kMaxRows));
}

void CommandSearch::Layout(const RECT& bar, const RECT& client, float ui) {
  ui_ = ui;
  const float u = ui;
  const float w = std::min(kWidth * u, static_cast<float>(client.right - client.left) - 32 * u);
  const float h = kCardH * u;
  const float cx = (client.left + client.right) / 2.0f;
  const float top = bar.top + std::round(((bar.bottom - bar.top) - h) / 2);
  card_ = RectF(std::round(cx - w / 2), top, std::round(cx + w / 2), top + h);
  const float cy = std::round((card_.top + card_.bottom) / 2);
  icon_ = RectF(card_.left + 8 * u, card_.top, card_.left + 34 * u, card_.bottom);
  box_.rc = RectF(card_.left + 36 * u, cy - 15 * u, card_.right - 6 * u, cy + 15 * u);
  bounds_ = RectF(static_cast<float>(client.left), static_cast<float>(bar.top), static_cast<float>(client.right),
                  static_cast<float>(client.bottom));
  popup_.Place(card_, Width(card_), Rows() * kRowH * u + 8 * u, bounds_, u);
  scroll_ = std::clamp(scroll_, 0.f, MaxScroll());
}

D2D1_RECT_F CommandSearch::RowRect(int i) const {
  const D2D1_RECT_F inner = popup_.Inner(ui_);
  const float h = kRowH * ui_;
  const float top = inner.top + i * h - scroll_;
  return RectF(inner.left, top, inner.right, top + h);
}

float CommandSearch::MaxScroll() const {
  return std::max(0.f, results_.size() * kRowH * ui_ - Height(popup_.Inner(ui_)));
}

void CommandSearch::EnsureVisible(int i) {
  if (i < 0) return;
  const float h = kRowH * ui_, view = Height(popup_.Inner(ui_));
  const float top = i * h, bottom = top + h;
  if (top < scroll_) scroll_ = top;
  else if (bottom > scroll_ + view) scroll_ = bottom - view;
  scroll_ = std::clamp(scroll_, 0.f, MaxScroll());
}

int CommandSearch::RowAt(int x, int y) const {
  if (!popup_.open || !Contains(popup_.Inner(ui_), (float)x, (float)y)) return -1;
  const int i = static_cast<int>(std::floor((y - popup_.Inner(ui_).top + scroll_) / (kRowH * ui_)));
  return i >= 0 && i < static_cast<int>(results_.size()) ? i : -1;
}

void CommandSearch::SetHot(int i, bool byKeyboard) {
  if (i < 0 || i >= static_cast<int>(results_.size())) return;
  hot_ = i;
  if (byKeyboard) inList_ = true;
  EnsureVisible(i);
  Invalidate();
}

void CommandSearch::Run(int i) {
  if (i < 0 || i >= static_cast<int>(results_.size()) || !results_[i].enabled) return;
  const int cmd = results_[i].cmd;
  Close();
  if (onRun) onRun(cmd);
}

void CommandSearch::Draw(Canvas& c, bool caretVisible) {
  if (!Visible()) return;
  const Palette& p = theme::P();
  const float u = ui_;
  const float v = show_.V();
  const bool fade = v < 0.99f;
  if (fade) {
    c.DC()->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                            D2D1::IdentityMatrix(), v),
                      nullptr);
  }
  c.SoftShadow(card_, 8 * u, 14 * u, p.popupShadow, 4 * u);
  c.FillRound(card_, 8 * u, p.popupBg);
  c.StrokeRound(card_, 8 * u, p.popupBorder, 1);
  c.Icon(icon::Search, icon_, p.textSecondary, 16 * u);
  box_.Draw(c, u, caretVisible);
  if (fade) c.DC()->PopLayer();

  if (!popup_.Visible()) return;
  popup_.Begin(c, u);
  const D2D1_RECT_F inner = popup_.Inner(u);
  c.PushClip(inner);
  if (results_.empty()) {
    c.Text(L"No commands match “" + fw::Trim(query_) + L"”",
           RectF(inner.left + 12 * u, inner.top, inner.right - 12 * u, inner.top + kRowH * u), p.textSecondary, 13 * u);
  }
  const float h = kRowH * u;
  const int first = std::max(0, static_cast<int>(scroll_ / h));
  const int last = std::min(static_cast<int>(results_.size()) - 1, static_cast<int>((scroll_ + Height(inner)) / h));
  for (int i = first; i <= last; ++i) {
    const CommandEntry& e = results_[i];
    const D2D1_RECT_F r = RowRect(i);
    const D2D1_RECT_F hr = RectF(r.left, r.top + 1 * u, r.right, r.bottom - 1 * u);
    const float cy = std::round((r.top + r.bottom) / 2);
    if (i == hot_) {
      c.FillRound(hr, 4 * u, p.hover);
      c.FillRound(RectF(hr.left, cy - 8 * u, hr.left + 3 * u, cy + 8 * u), 1.5f * u, p.accent);
    }
    if (i == pressed_) c.FillRound(hr, 4 * u, p.pressed);
    const D2D1_COLOR_F fg = e.enabled ? p.text : p.textDisabled;
    if (e.checked) c.Icon(icon::CheckMark, RectF(hr.left + 8 * u, r.top, hr.left + 28 * u, r.bottom), fg, 12 * u);
    float keysW = e.shortcut.empty() ? 0 : std::ceil(c.MeasureText(e.shortcut, 12 * u));
    c.Text(e.label, RectF(hr.left + 32 * u, r.top, hr.right - 24 * u - keysW, r.bottom), fg, 13 * u);
    if (keysW > 0) {
      c.Text(e.shortcut, RectF(hr.right - 12 * u - keysW - 2, r.top, hr.right - 12 * u, r.bottom),
             e.enabled ? p.textSecondary : p.textDisabled, 12 * u, DWRITE_FONT_WEIGHT_REGULAR, Align::Trailing);
    }
  }
  c.PopClip();
  if (const float max = MaxScroll(); max > 0) {
    const float trackTop = inner.top + 2 * u, trackH = Height(inner) - 4 * u;
    const float thumbH = std::max(24 * u, trackH * Height(inner) / (results_.size() * h));
    const float top = trackTop + (trackH - thumbH) * scroll_ / max;
    c.FillRound(RectF(inner.right - 5 * u, top, inner.right - 2 * u, top + thumbH), 1.5f * u, p.scrollThumb);
  }
  popup_.End(c);
}

bool CommandSearch::Hit(int x, int y) const {
  if (!open_) return false;
  return Contains(card_, (float)x, (float)y) || popup_.Hit((float)x, (float)y);
}

bool CommandSearch::HitBox(int x, int y) const { return open_ && Contains(box_.rc, (float)x, (float)y); }

bool CommandSearch::OnMouseMove(int x, int y) {
  const int row = RowAt(x, y);
  if (row < 0 || row == hot_) return false;
  hot_ = row;
  return true;
}

void CommandSearch::OnMouseLeave() {}

void CommandSearch::OnMouseDown(int x, int y, bool shift, bool dbl) {
  if (HitBox(x, y)) {
    inList_ = false;
    if (dbl) box_.OnDoubleClick();
    else box_.OnMouseDown((float)x, (float)y, shift, ui_);
    return;
  }
  pressed_ = RowAt(x, y);
}

void CommandSearch::OnMouseDrag(int x, int y) {
  if (pressed_ < 0) box_.OnMouseDrag((float)x, (float)y, ui_);
}

void CommandSearch::OnMouseUp(int x, int y) {
  const int pressed = pressed_;
  pressed_ = -1;
  if (pressed >= 0 && RowAt(x, y) == pressed) Run(pressed);
}

bool CommandSearch::OnWheel(int delta, int x, int y) {
  if (!Hit(x, y)) return false;
  if (popup_.Hit((float)x, (float)y)) scroll_ = std::clamp(scroll_ - delta / 120.f * 3 * kRowH * ui_, 0.f, MaxScroll());
  return true;
}

void CommandSearch::Describe(uia::Tree& t, int parent, const std::function<void()>& focusBox) {
  if (!open_) return;
  D2D1_RECT_F all = card_;
  if (popup_.open) {
    all = RectF(std::min(all.left, popup_.rc.left), std::min(all.top, popup_.rc.top),
                std::max(all.right, popup_.rc.right), std::max(all.bottom, popup_.rc.bottom));
  }
  const int pane = t.Add(parent, 10, UIA_PaneControlTypeId, L"Command search", all);
  t[pane].layer = 2;
  const bool itemFocus = box_.focused && inList_ && hot_ >= 0;
  const int edit = uia::AddTextBox(t, pane, 1, box_, L"Search for a command (Alt+Q)", focusBox);
  t[edit].focused = box_.focused && !itemFocus;
  t[edit].focus = [this, focusBox] {
    inList_ = false;
    focusBox();
  };
  if (!popup_.open) return;
  if (results_.empty()) {
    t.Add(pane, 3, UIA_TextControlTypeId, L"No commands match", popup_.rc);
    return;
  }
  const int list = t.Add(pane, 2, UIA_ListControlTypeId, L"Commands", popup_.rc);
  const D2D1_RECT_F inner = popup_.Inner(ui_);
  {
    uia::Node& n = t[list];
    n.layer = 2;
    n.orientation = OrientationType_Vertical;
    n.selectionContainer = true;
    n.scrollable = true;
    if (const float max = MaxScroll(); max > 0) {
      n.scrollPercent = scroll_ / max * 100.0;
      n.viewPercent = Height(inner) / (results_.size() * kRowH * ui_) * 100.0;
      n.setScroll = [this](double pct) { scroll_ = static_cast<float>(MaxScroll() * pct / 100.0); };
    }
  }
  const int count = static_cast<int>(results_.size());
  for (int i = 0; i < count; ++i) {
    const CommandEntry& e = results_[i];
    D2D1_RECT_F r = RowRect(i);
    const bool inView = r.bottom > inner.top && r.top < inner.bottom;
    if (inView) r = RectF(r.left, std::max(r.top, inner.top), r.right, std::min(r.bottom, inner.bottom));
    const int item = t.Add(list, e.cmd, UIA_ListItemControlTypeId, e.label, inView ? r : D2D1_RECT_F{});
    uia::Node& n = t[item];
    n.layer = 2;
    n.accelerator = e.shortcut;
    n.enabled = e.enabled;
    if (e.checked) n.status = L"On";
    n.offscreen = !inView;
    n.selectable = true;
    n.selected = i == hot_;
    n.select = [this, i] { SetHot(i, false); };
    n.invoke = [this, i] { Run(i); };
    n.focusable = true;
    n.focused = itemFocus && i == hot_;
    n.focus = [this, i, focusBox] {
      focusBox();
      SetHot(i, true);
    };
    n.positionInSet = i + 1;
    n.sizeOfSet = count;
    n.scrollIntoView = [this, i] { EnsureVisible(i); };
  }
}
