#include "ui/Controls.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cwctype>

#include "ui/Icons.h"
#include "ui/Theme.h"

using namespace gfx;

namespace caret {
namespace {
bool g_steady = false;
}
bool On(double since) { return g_steady || static_cast<int>((anim::Now() - since) / 530) % 2 == 0; }
void SetSteady(bool steady) { g_steady = steady; }
}  // namespace caret

namespace {

constexpr int kMaxPopupRows = 12;

void CALLBACK CaretProc(HWND hwnd, UINT, UINT_PTR, DWORD) { InvalidateRect(hwnd, nullptr, FALSE); }

void HoverTo(Anim& a, bool on) {
  if (on != (a.Target() > 0.5f)) a.To(on ? 1.f : 0.f, on ? 80.f : 150.f);
}

}  // namespace

// ---------------------------------------------------------------------------
// Shared drawing

namespace ctl {

void DrawBox(Canvas& c, const D2D1_RECT_F& r, float ui, float hover, float press) {
  const Palette& p = theme::P();
  const float radius = 4 * ui;
  c.FillRound(r, radius, p.dark ? Rgb(0xFFFFFF, 0.06f) : Rgb(0xFFFFFF, 0.9f));
  c.StrokeRound(r, radius, p.inputBorder, 1);
  c.FillRound(r, radius, Alpha(p.hover, hover));
  c.FillRound(r, radius, Alpha(p.pressed, press));
}

void DrawSubtle(Canvas& c, const D2D1_RECT_F& r, float ui, float hover, float press) {
  const Palette& p = theme::P();
  c.FillRound(r, 4 * ui, Alpha(p.hover, hover));
  c.FillRound(r, 4 * ui, Alpha(p.pressed, press));
}

void DrawFocusRing(Canvas& c, const D2D1_RECT_F& r, float ui) {
  const Palette& p = theme::P();
  float w = std::max(1.f, std::round(2 * ui));
  c.StrokeRound(Inflate(r, 3 * ui, 3 * ui), 6 * ui, p.focusRing, w);
}

void DrawChevron(Canvas& c, const D2D1_RECT_F& r, D2D1_COLOR_F color, float ui, bool up) {
  c.Icon(up ? icon::ChevronUp : icon::ChevronDown, r, color, 10 * ui);
}

void DrawSwatch(Canvas& c, const D2D1_RECT_F& r, D2D1_COLOR_F color, float ui) {
  const Palette& p = theme::P();
  if (color.a <= 0) {
    // No color: white with a red slash.
    c.Fill(r, Rgb(0xFFFFFF));
    c.Line(r.left + 1, r.bottom - 1, r.right - 1, r.top + 1, Rgb(0xC42B1C), std::max(1.f, 1.5f * ui));
  } else {
    c.Fill(r, color);
  }
  c.Stroke(r, Alpha(p.text, p.dark ? 0.35f : 0.22f), 1);
}

bool StartsWithNoCase(const std::wstring& s, const std::wstring& prefix) {
  if (prefix.size() > s.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i)
    if (towlower(s[i]) != towlower(prefix[i])) return false;
  return true;
}

}  // namespace ctl

// ---------------------------------------------------------------------------
// Control

bool Control::Focused() const { return group_ && group_->Focused() == this; }

std::wstring Control::Name() const { return label.empty() ? uia::SplitShortcut(tooltip).first : label; }

float Control::Ui() const { return group_ ? group_->ui : 1.f; }

bool Control::FocusVisible() const { return Focused() && group_->FocusVisible(); }

bool Control::CaretOn() const { return group_ && group_->CaretOn(); }

void Control::TakeFocus() {
  if (group_ && enabled && group_->Focused() != this) group_->Focus(this, false);
}

HWND Control::Window() const { return group_ ? group_->hwnd : nullptr; }

D2D1_RECT_F Control::PopupBounds() const { return group_ ? group_->bounds : D2D1_RECT_F{}; }

void Control::Opened() {
  if (group_) group_->CloseAll(this);
}

// ---------------------------------------------------------------------------
// ControlGroup

ControlGroup::~ControlGroup() { CaretTimer(false); }

void ControlGroup::Add(Control* c) {
  c->SetGroup(this);
  controls_.push_back(c);
}

void ControlGroup::Clear() {
  Blur();
  controls_.clear();
  hover_ = capture_ = nullptr;
}

void ControlGroup::Assign(const std::vector<Control*>& controls) {
  if (controls == controls_) return;
  auto stays = [&](Control* c) { return std::find(controls.begin(), controls.end(), c) != controls.end(); };
  if (focused_ && !stays(focused_)) Focus(nullptr, false);
  for (Control* c : controls_)
    if (!stays(c) && c->IsOpen()) c->SetOpen(false);
  if (hover_ && !stays(hover_)) hover_ = nullptr;
  if (capture_ && !stays(capture_)) capture_ = nullptr;
  controls_ = controls;
  for (Control* c : controls_) c->SetGroup(this);
}

void ControlGroup::Draw(Canvas& c) {
  for (Control* k : controls_)
    if (k->visible) k->Draw(c);
}

void ControlGroup::DrawPopup(Canvas& c) {
  for (Control* k : controls_)
    if (k->visible) k->DrawPopup(c);
}

Control* ControlGroup::At(float x, float y) const {
  for (auto it = controls_.rbegin(); it != controls_.rend(); ++it)
    if ((*it)->Hit(x, y)) return *it;
  return nullptr;
}

bool ControlGroup::Hit(float x, float y) const { return HitPopup(x, y) || At(x, y); }

bool ControlGroup::HitPopup(float x, float y) const {
  Control* open = Open();
  return open && open->HitPopup(x, y);
}

Control* ControlGroup::Open() const {
  for (Control* k : controls_)
    if (k->visible && k->IsOpen()) return k;
  return nullptr;
}

void ControlGroup::CloseAll(Control* except) {
  for (Control* k : controls_)
    if (k != except && k->IsOpen()) k->SetOpen(false);
}

bool ControlGroup::OnMouseMove(float x, float y) {
  if (capture_) return capture_->OnMouseMove(x, y);
  Control* open = Open();
  Control* h = open && open->HitPopup(x, y) ? open : At(x, y);
  bool changed = false;
  if (h != hover_) {
    if (hover_) hover_->OnMouseLeave();
    hover_ = h;
    changed = true;
  }
  if (h && h->OnMouseMove(x, y)) changed = true;
  return changed;
}

void ControlGroup::OnMouseLeave() {
  if (hover_) hover_->OnMouseLeave();
  hover_ = nullptr;
}

bool ControlGroup::OnMouseDown(float x, float y, bool dbl, bool shift) {
  Control* open = Open();
  Control* c = open && open->HitPopup(x, y) ? open : At(x, y);
  if (!c) {
    CloseAll();
    return false;
  }
  if (open && open != c) open->SetOpen(false);
  if (!c->enabled) return true;
  if (focused_ != c) Focus(c, false);
  focusVisible_ = false;
  KeepCaret();
  capture_ = c;
  c->OnMouseDown(x, y, dbl, shift);
  return true;
}

void ControlGroup::OnMouseUp(float x, float y) {
  Control* c = capture_;
  capture_ = nullptr;
  if (c) c->OnMouseUp(x, y);
}

bool ControlGroup::OnWheel(int delta, float x, float y) {
  Control* open = Open();
  if (open && open->HitPopup(x, y)) return open->OnWheel(delta, x, y);
  Control* c = At(x, y);
  return c && c->enabled && c->OnWheel(delta, x, y);
}

bool ControlGroup::OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt) {
  if (!focused_) return false;
  KeepCaret();
  switch (vk) {
    case VK_TAB:
    case VK_UP:
    case VK_DOWN:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_F4: focusVisible_ = true; break;
    default: break;
  }
  if (focused_->OnKeyDown(vk, ctrl, shift, alt)) return true;
  if (vk == VK_TAB && !ctrl && !alt) {
    CloseAll();
    return MoveFocus(!shift);
  }
  return false;
}

bool ControlGroup::OnChar(wchar_t ch) {
  if (!focused_) return false;
  KeepCaret();
  return focused_->OnChar(ch);
}

void ControlGroup::Focus(Control* c, bool byKeyboard) {
  focusVisible_ = byKeyboard;
  if (c == focused_) return;
  Control* old = focused_;
  focused_ = c;
  if (old) old->OnFocus(false);
  if (c) c->OnFocus(true);
  KeepCaret();
  CaretTimer(c && c->UsesCaret());
  Invalidate();
}

void ControlGroup::Blur() {
  CloseAll();
  Focus(nullptr, false);
}

bool ControlGroup::MoveFocus(bool forward) {
  const int n = static_cast<int>(controls_.size());
  if (n == 0) return false;
  int start = -1;
  for (int i = 0; i < n; ++i)
    if (controls_[i] == focused_) start = i;
  if (start < 0) start = forward ? -1 : n;
  for (int k = 1; k <= n; ++k) {
    int i = forward ? start + k : start - k;
    if (!wrap && (i < 0 || i >= n)) return false;
    i = ((i % n) + n) % n;
    Control* c = controls_[i];
    if (c->visible && c->enabled) {
      Focus(c, true);
      return true;
    }
  }
  return false;
}

void ControlGroup::KeepCaret() { caretStart_ = anim::Now(); }

void ControlGroup::CaretTimer(bool on) {
  if (!hwnd) return;
  if (on) SetTimer(hwnd, reinterpret_cast<UINT_PTR>(this), 265, CaretProc);
  else KillTimer(hwnd, reinterpret_cast<UINT_PTR>(this));
}

std::wstring ControlGroup::TooltipAt(float x, float y) const {
  Control* open = Open();
  if (open && open->HitPopup(x, y)) return open->TooltipAt(x, y);
  Control* c = At(x, y);
  return c ? c->TooltipAt(x, y) : L"";
}

bool ControlGroup::OverText(float x, float y) const {
  if (HitPopup(x, y)) return false;
  Control* c = At(x, y);
  return c && c->enabled && c->OverText(x, y);
}

void ControlGroup::Describe(uia::Tree& t, int parent) {
  for (Control* c : controls_)
    if (c->visible) c->Describe(t, parent);
}

void ControlGroup::Invalidate() const {
  if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Popup

void Popup::Place(const D2D1_RECT_F& anchor, float w, float h, const D2D1_RECT_F& bounds, float ui) {
  const float gap = 2 * ui, margin = 4 * ui;
  float below = bounds.bottom - margin - (anchor.bottom + gap);
  float room = anchor.top - gap - (bounds.top + margin);
  above = h > below && room > below;
  h = std::max(0.f, std::min(h, above ? room : below));
  w = std::min(w, Width(bounds) - 2 * margin);
  float x = std::clamp(anchor.left, bounds.left + margin, std::max(bounds.left + margin, bounds.right - margin - w));
  float top = above ? anchor.top - gap - h : anchor.bottom + gap;
  rc = RectF(std::round(x), std::round(top), std::round(x + w), std::round(top + h));
}

void Popup::Open() {
  open = true;
  show.To(1, 120);
}

void Popup::Close() {
  open = false;
  show.To(0, 90);
}

void Popup::Begin(Canvas& c, float ui) {
  const Palette& p = theme::P();
  float v = show.V();
  layered_ = v < 0.99f;
  c.DC()->GetTransform(&saved_);
  if (layered_) {
    float dy = (1 - v) * (above ? 6 : -6) * ui;
    c.DC()->SetTransform(D2D1::Matrix3x2F::Translation(0, dy) * saved_);
    c.DC()->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                            D2D1::IdentityMatrix(), v),
                      nullptr);
  }
  c.SoftShadow(rc, 8 * ui, 12 * ui, p.popupShadow, 4 * ui);
  c.FillRound(rc, 8 * ui, p.popupBg);
  c.StrokeRound(rc, 8 * ui, p.popupBorder, 1);
}

void Popup::End(Canvas& c) {
  if (layered_) c.DC()->PopLayer();
  c.DC()->SetTransform(saved_);
  layered_ = false;
}

D2D1_RECT_F Popup::Inner(float ui) const { return Inflate(rc, -4 * ui, -4 * ui); }

// ---------------------------------------------------------------------------
// ListBox

ListBox::ListBox() = default;

D2D1_RECT_F ListBox::Viewport() const {
  if (inPopup) return rc;
  const float u = Ui();
  return Inflate(rc, -3 * u, -3 * u);
}

float ListBox::ContentHeight() const { return static_cast<float>(items.size()) * rowHeight * Ui(); }

float ListBox::MaxScroll() const { return std::max(0.f, ContentHeight() - Height(Viewport())); }

int ListBox::RowsVisible() const { return std::max(1, static_cast<int>(Height(Viewport()) / (rowHeight * Ui()))); }

float ListBox::PreferredWidth() const {
  const float u = Ui();
  Canvas measure;
  float w = 0;
  for (const auto& s : items) w = std::max(w, measure.MeasureText(s, 13 * u));
  return std::ceil(w + 12 * u + 24 * u);
}

D2D1_RECT_F ListBox::ItemRect(int i) const {
  D2D1_RECT_F vp = Viewport();
  float h = rowHeight * Ui();
  float top = vp.top + i * h - scroll_;
  return RectF(vp.left, top, vp.right, top + h);
}

int ListBox::ItemAt(float x, float y) const {
  D2D1_RECT_F vp = Viewport();
  if (!Contains(vp, x, y)) return -1;
  int i = static_cast<int>(std::floor((y - vp.top + scroll_) / (rowHeight * Ui())));
  return i >= 0 && i < static_cast<int>(items.size()) ? i : -1;
}

D2D1_RECT_F ListBox::Thumb() const {
  float max = MaxScroll();
  if (max <= 0) return {};
  const float u = Ui();
  D2D1_RECT_F vp = Viewport();
  float trackTop = vp.top + 2 * u, trackH = Height(vp) - 4 * u;
  float thumbH = std::max(24 * u, trackH * Height(vp) / ContentHeight());
  float top = trackTop + (trackH - thumbH) * scroll_ / max;
  float w = (overBar_ || dragThumb_) ? 6 * u : 3 * u;
  float right = vp.right - 2 * u;
  return RectF(right - w, top, right, top + thumbH);
}

void ListBox::ScrollTo(float px) { scroll_ = std::clamp(px, 0.f, MaxScroll()); }

void ListBox::SetSelected(int i) { selected_ = i >= 0 && i < static_cast<int>(items.size()) ? i : -1; }

void ListBox::SetHighlight(int i) { hot_ = i >= 0 && i < static_cast<int>(items.size()) ? i : -1; }

void ListBox::EnsureVisible(int i) {
  if (i < 0) return;
  float h = rowHeight * Ui();
  float top = i * h, bottom = top + h, view = Height(Viewport());
  if (top < scroll_) scroll_ = top;
  else if (bottom > scroll_ + view) scroll_ = bottom - view;
  ScrollTo(scroll_);
}

void ListBox::Move(int delta) {
  const int n = static_cast<int>(items.size());
  if (n == 0) return;
  int cur = Current();
  int to;
  if (delta == INT_MIN) to = 0;
  else if (delta == INT_MAX) to = n - 1;
  else if (cur < 0) to = delta > 0 ? 0 : n - 1;
  else to = std::clamp(cur + delta, 0, n - 1);
  if (inPopup) {
    SetHighlight(to);
  } else if (to != selected_) {
    SetSelected(to);
    if (onSelect) onSelect(to);
  }
  EnsureVisible(to);
}

int ListBox::TypeAhead(wchar_t ch) {
  double now = anim::Now();
  if (now - typedAt_ > 1000) typed_.clear();
  typedAt_ = now;
  typed_ += static_cast<wchar_t>(towlower(ch));
  const int n = static_cast<int>(items.size());
  if (n == 0) return -1;
  // The same letter again steps through the items that start with it.
  bool same = std::all_of(typed_.begin(), typed_.end(), [&](wchar_t c) { return c == typed_[0]; });
  std::wstring prefix = same ? typed_.substr(0, 1) : typed_;
  int cur = Current();
  int from = (same || cur < 0) ? cur + 1 : cur;
  for (int k = 0; k < n; ++k) {
    int i = ((from + k) % n + n) % n;
    if (ctl::StartsWithNoCase(items[i], prefix)) return i;
  }
  return -1;
}

void ListBox::Draw(Canvas& c) {
  if (!visible) return;
  const Palette& p = theme::P();
  const float u = Ui();
  if (!inPopup) {
    c.FillRound(rc, 4 * u, p.inputBg);
    c.StrokeRound(rc, 4 * u, p.inputBorder, 1);
    if (FocusVisible()) ctl::DrawFocusRing(c, rc, u);
  }
  D2D1_RECT_F vp = Viewport();
  const float h = rowHeight * u;
  const int n = static_cast<int>(items.size());
  c.PushClip(vp);
  int first = std::max(0, static_cast<int>(scroll_ / h));
  int last = std::min(n - 1, static_cast<int>((scroll_ + Height(vp)) / h));
  for (int i = first; i <= last; ++i) {
    D2D1_RECT_F r = ItemRect(i);
    D2D1_RECT_F hr = RectF(r.left + 2 * u, r.top + 1 * u, r.right - 2 * u, r.bottom - 1 * u);
    const bool sel = i == selected_;
    const bool hot = inPopup ? i == hot_ : i == hover_;
    if (sel && !inPopup) c.FillRound(hr, 4 * u, Alpha(p.hover, 1.3f));
    if (hot && enabled) c.FillRound(hr, 4 * u, p.hover);
    if (i == pressed_ && enabled) c.FillRound(hr, 4 * u, p.pressed);
    float cy = std::round((r.top + r.bottom) / 2);
    if (sel) c.FillRound(RectF(hr.left, cy - 8 * u, hr.left + 3 * u, cy + 8 * u), 1.5f * u, p.accent);
    c.Text(items[i], RectF(hr.left + 12 * u, r.top, hr.right - 8 * u, r.bottom), enabled ? p.text : p.textDisabled,
           13 * u);
  }
  c.PopClip();
  if (MaxScroll() > 0) {
    D2D1_RECT_F t = Thumb();
    c.FillRound(t, Width(t) / 2, (overBar_ || dragThumb_) ? p.scrollThumbHover : p.scrollThumb);
  }
}

bool ListBox::OnMouseMove(float x, float y) {
  const float u = Ui();
  if (dragThumb_) {
    D2D1_RECT_F vp = Viewport(), t = Thumb();
    float travel = Height(vp) - 4 * u - Height(t);
    if (travel > 0) ScrollTo(dragScroll_ + (y - dragFrom_) * MaxScroll() / travel);
    return true;
  }
  bool changed = false;
  D2D1_RECT_F vp = Viewport();
  bool bar = MaxScroll() > 0 && Contains(vp, x, y) && x >= vp.right - 12 * u;
  if (bar != overBar_) {
    overBar_ = bar;
    changed = true;
  }
  int row = bar ? -1 : ItemAt(x, y);
  if (row != hover_) {
    hover_ = row;
    changed = true;
  }
  if (inPopup && row >= 0 && row != hot_) {
    hot_ = row;
    changed = true;
  }
  return changed;
}

void ListBox::OnMouseLeave() {
  hover_ = -1;
  overBar_ = false;
}

void ListBox::OnMouseDown(float x, float y, bool dbl, bool) {
  const float u = Ui();
  D2D1_RECT_F vp = Viewport();
  if (MaxScroll() > 0 && Contains(vp, x, y) && x >= vp.right - 12 * u) {
    D2D1_RECT_F t = Thumb();
    if (y >= t.top && y < t.bottom) {
      dragThumb_ = true;
      dragFrom_ = y;
      dragScroll_ = scroll_;
    } else {
      ScrollTo(scroll_ + (y < t.top ? -1 : 1) * Height(vp));  // page towards the click
    }
    return;
  }
  int row = ItemAt(x, y);
  pressed_ = row;
  if (row < 0) return;
  if (!inPopup) {
    if (row != selected_) {
      SetSelected(row);
      if (onSelect) onSelect(row);
    }
    EnsureVisible(row);
    if (dbl && onActivate) onActivate(row);
  }
}

void ListBox::OnMouseUp(float x, float y) {
  dragThumb_ = false;
  int pressed = pressed_;
  pressed_ = -1;
  if (inPopup && pressed >= 0 && ItemAt(x, y) == pressed && onActivate) onActivate(pressed);
}

bool ListBox::OnWheel(int delta, float, float) {
  if (MaxScroll() <= 0) return false;
  ScrollTo(scroll_ - delta / 120.f * 3 * rowHeight * Ui());
  return true;
}

bool ListBox::OnKeyDown(UINT vk, bool, bool, bool alt) {
  if (alt || !enabled) return false;
  const int page = std::max(1, RowsVisible() - 1);
  switch (vk) {
    case VK_UP: Move(-1); return true;
    case VK_DOWN: Move(1); return true;
    case VK_PRIOR: Move(-page); return true;
    case VK_NEXT: Move(page); return true;
    case VK_HOME: Move(INT_MIN); return true;
    case VK_END: Move(INT_MAX); return true;
    case VK_RETURN:
      if (Current() >= 0 && onActivate) onActivate(Current());
      return true;
    default: return false;
  }
}

bool ListBox::OnChar(wchar_t ch) {
  if (!enabled || ch < 32 || ch == 127) return false;
  // A space only counts as part of a name that is being typed.
  if (ch == L' ' && (typed_.empty() || anim::Now() - typedAt_ > 1000)) return false;
  int i = TypeAhead(ch);
  if (i < 0) return true;
  if (inPopup) {
    SetHighlight(i);
  } else if (i != selected_) {
    SetSelected(i);
    if (onSelect) onSelect(i);
  }
  EnsureVisible(i);
  return true;
}

void ListBox::Describe(uia::Tree& t, int parent) {
  int list = DescribeAsPart(t, parent, id, true, Focused(), [this](int k) {
    TakeFocus();
    SetSelected(k);
    EnsureVisible(k);
    if (onSelect) onSelect(k);
  });
  uia::Node& n = t[list];
  n.accelerator = uia::SplitShortcut(tooltip).second;
  n.focusable = true;
  n.focus = [this] { TakeFocus(); };
  n.focused = Focused() && selected_ < 0;
}

int ListBox::DescribeAsPart(uia::Tree& t, int parent, int64_t partId, bool open, bool focusItems,
                            const std::function<void(int)>& pick) {
  const int list = t.Add(parent, partId, UIA_ListControlTypeId, Name(), open ? rc : D2D1_RECT_F{});
  const int n = static_cast<int>(items.size());
  {
    uia::Node& node = t[list];
    if (inPopup) node.layer = 2;
    node.enabled = enabled;
    node.offscreen = !open;
    node.orientation = OrientationType_Vertical;
    node.selectionContainer = true;
    if (open) {
      node.scrollable = true;
      float max = MaxScroll();
      if (max > 0) {
        node.scrollPercent = scroll_ / max * 100.0;
        node.viewPercent = Height(Viewport()) / ContentHeight() * 100.0;
        node.setScroll = [this](double pct) { ScrollTo(static_cast<float>(MaxScroll() * pct / 100.0)); };
      }
    }
  }
  const D2D1_RECT_F vp = Viewport();
  for (int k = 0; k < n; ++k) {
    D2D1_RECT_F r = ItemRect(k);
    const bool inView = open && r.bottom > vp.top && r.top < vp.bottom;
    if (inView) r = RectF(r.left, std::max(r.top, vp.top), r.right, std::min(r.bottom, vp.bottom));
    int item = t.Add(list, k, UIA_ListItemControlTypeId, items[k], open ? r : D2D1_RECT_F{});
    uia::Node& node = t[item];
    node.enabled = enabled;
    node.offscreen = !inView;
    node.selectable = true;
    node.selected = k == selected_;
    node.select = [pick, k] { pick(k); };
    node.focusable = true;
    node.focused = focusItems && k == Current();
    node.positionInSet = k + 1;
    node.sizeOfSet = n;
    if (open) node.scrollIntoView = [this, k] { EnsureVisible(k); };
    node.focus = [this, k] {
      if (inPopup) {
        SetHighlight(k);
      } else {
        TakeFocus();
        if (k != selected_) {
          SetSelected(k);
          if (onSelect) onSelect(k);
        }
      }
      EnsureVisible(k);
    };
  }
  return list;
}

// ---------------------------------------------------------------------------
// Dropdown

Dropdown::Dropdown() {
  list_.inPopup = true;
  list_.rowHeight = 30;
  list_.onActivate = [this](int i) { Pick(i); };
}

void Dropdown::SetGroup(ControlGroup* g) {
  Control::SetGroup(g);
  list_.SetGroup(g);
}

void Dropdown::Pick(int i) {
  SetOpen(false);
  if (i < 0 || i >= static_cast<int>(items.size()) || i == selected) return;
  selected = i;
  if (onChange) onChange(i);
}

void Dropdown::SetOpen(bool open) {
  if (open == popup_.open) return;
  if (!open) {
    popup_.Close();
    inList_ = false;
    return;
  }
  if (!enabled || items.empty()) return;
  Opened();
  const float u = Ui();
  list_.items = items;
  list_.label = Name();
  list_.SetSelected(selected);
  list_.SetHighlight(selected);
  float w = std::max(Width(rc), list_.PreferredWidth() + 8 * u);
  float rows = static_cast<float>(std::min<size_t>(items.size(), kMaxPopupRows));
  popup_.Place(rc, w, rows * list_.rowHeight * u + 8 * u, PopupBounds(), u);
  list_.rc = popup_.Inner(u);
  list_.ResetScroll();
  list_.EnsureVisible(selected);
  popup_.Open();
}

bool Dropdown::HitPopup(float x, float y) const { return popup_.Hit(x, y); }

void Dropdown::Draw(Canvas& c) {
  const Palette& p = theme::P();
  const float u = Ui();
  const float hv = enabled ? hover_.V() : 0, pv = enabled ? std::max(press_.V(), popup_.open ? 0.7f : 0.f) : 0;
  ctl::DrawBox(c, rc, u, hv, pv);
  D2D1_COLOR_F fg = enabled ? p.text : p.textDisabled;
  float x = rc.left + 11 * u;
  if (glyph) {
    DrawGlyph(c, glyph, RectF(x, rc.top, x + 16 * u, rc.bottom), fg, 14 * u);
    x += 16 * u + 8 * u;
  }
  const bool chosen = selected >= 0 && selected < static_cast<int>(items.size());
  c.Text(chosen ? items[selected] : placeholder, RectF(x, rc.top, rc.right - 28 * u, rc.bottom),
         chosen ? fg : (enabled ? p.textSecondary : p.textDisabled), 13 * u);
  ctl::DrawChevron(c, RectF(rc.right - 28 * u, rc.top, rc.right - 6 * u, rc.bottom),
                   enabled ? p.textSecondary : p.textDisabled, u);
  if (FocusVisible()) ctl::DrawFocusRing(c, rc, u);
}

void Dropdown::DrawPopup(Canvas& c) {
  if (!popup_.Visible()) return;
  popup_.Begin(c, Ui());
  list_.Draw(c);
  popup_.End(c);
}

bool Dropdown::OnMouseMove(float x, float y) {
  if (inList_ || HitPopup(x, y)) return list_.OnMouseMove(x, y);
  bool over = Hit(x, y);
  bool changed = over != (hover_.Target() > 0.5f);
  HoverTo(hover_, over);
  return changed;
}

void Dropdown::OnMouseLeave() {
  HoverTo(hover_, false);
  list_.OnMouseLeave();
}

void Dropdown::OnMouseDown(float x, float y, bool dbl, bool shift) {
  if (HitPopup(x, y)) {
    inList_ = true;
    list_.OnMouseDown(x, y, dbl, shift);
    return;
  }
  press_.To(1, 40);
  SetOpen(!popup_.open);
}

void Dropdown::OnMouseUp(float x, float y) {
  press_.To(0, 120);
  if (inList_) {
    inList_ = false;
    list_.OnMouseUp(x, y);
  }
}

bool Dropdown::OnWheel(int delta, float x, float y) { return HitPopup(x, y) && list_.OnWheel(delta, x, y); }

bool Dropdown::OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt) {
  if (!enabled) return false;
  const int n = static_cast<int>(items.size());
  if (popup_.open) {
    switch (vk) {
      case VK_ESCAPE: SetOpen(false); return true;
      case VK_RETURN:  // done: false lets a toolbar hand the keyboard back
        Pick(list_.Highlight());
        return false;
      case VK_SPACE:
      case VK_F4: Pick(list_.Highlight()); return true;
      case VK_UP:
      case VK_DOWN:
        if (alt) {
          Pick(list_.Highlight());
          return true;
        }
        break;
      case VK_TAB:
        Pick(list_.Highlight());
        return false;
      default: break;
    }
    return list_.OnKeyDown(vk, ctrl, shift, alt);
  }
  int to = -1;
  switch (vk) {
    case VK_F4:
    case VK_RETURN:
    case VK_SPACE: SetOpen(true); return true;
    case VK_UP:
    case VK_DOWN:
      if (alt) {
        SetOpen(true);
        return true;
      }
      to = selected < 0 ? 0 : std::clamp(selected + (vk == VK_DOWN ? 1 : -1), 0, n - 1);
      break;
    case VK_HOME: to = 0; break;
    case VK_END: to = n - 1; break;
    default: return false;
  }
  if (to >= 0 && to < n && to != selected) {
    selected = to;
    if (onChange) onChange(to);
  }
  return true;
}

bool Dropdown::OnChar(wchar_t ch) {
  if (!enabled) return false;
  if (popup_.open) return list_.OnChar(ch);
  if (ch <= L' ' || ch == 127) return false;
  // Closed: typing picks the item right away, as in Windows' own lists.
  list_.items = items;
  list_.SetHighlight(selected);
  int i = list_.TypeAhead(ch);
  if (i >= 0 && i != selected) {
    selected = i;
    if (onChange) onChange(i);
  }
  return true;
}

void Dropdown::OnFocus(bool focused) {
  if (!focused) SetOpen(false);
}

void Dropdown::Describe(uia::Tree& t, int parent) {
  int i = t.Add(parent, id, UIA_ComboBoxControlTypeId, Name(), rc);
  {
    uia::Node& n = t[i];
    n.accelerator = uia::SplitShortcut(tooltip).second;
    n.enabled = enabled;
    n.focusable = true;
    n.focus = [this] { TakeFocus(); };
    n.focused = Focused() && !popup_.open;
    n.expand = popup_.open ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
    n.doExpand = [this](bool e) {
      TakeFocus();
      SetOpen(e);
    };
    n.hasValue = true;
    if (selected >= 0 && selected < static_cast<int>(items.size())) n.value = items[selected];
  }
  if (!popup_.open) {
    list_.items = items;
    list_.label = Name();
    list_.SetSelected(selected);
  }
  list_.DescribeAsPart(t, i, 1, popup_.open, Focused() && popup_.open, [this](int k) { Pick(k); });
}

// ---------------------------------------------------------------------------
// ComboBox

ComboBox::ComboBox() {
  list_.inPopup = true;
  list_.rowHeight = 30;
  list_.onActivate = [this](int i) { Pick(i); };
  box_.trailingInset = 28;
  box_.maxLength = 256;
}

void ComboBox::SetGroup(ControlGroup* g) {
  Control::SetGroup(g);
  list_.SetGroup(g);
}

void ComboBox::SetText(const std::wstring& text) {
  if (Focused() || text == committed_) return;
  committed_ = text;
  box_.SetText(text);
}

D2D1_RECT_F ComboBox::ArrowRect() const {
  const float u = Ui();
  return RectF(rc.right - 30 * u, rc.top + 3 * u, rc.right - 3 * u, rc.bottom - 4 * u);
}

int ComboBox::IndexOf(const std::wstring& text) const {
  for (size_t i = 0; i < items.size(); ++i)
    if (items[i].size() == text.size() && ctl::StartsWithNoCase(items[i], text)) return static_cast<int>(i);
  return -1;
}

bool ComboBox::Commit(bool always) {
  std::wstring text = box_.Text();
  if (validate && !validate(text)) {
    box_.ShakeError();
    return false;
  }
  box_.error = false;
  if (text != box_.Text()) box_.SetText(text, true);
  if (!always && text == committed_) return true;
  committed_ = text;
  if (onCommit) onCommit(text, IndexOf(text));
  return true;
}

void ComboBox::Pick(int i) {
  SetOpen(false);
  if (i < 0 || i >= static_cast<int>(items.size())) return;
  box_.SetText(items[i], true);
  Commit(true);
}

void ComboBox::Revert() {
  box_.SetText(committed_, true);
  box_.error = false;
}

// Keeps the open list's highlight on the item the text names.
void ComboBox::FollowText() {
  if (!popup_.open) return;
  int i = IndexOf(box_.Text());
  if (i < 0) {
    for (size_t k = 0; k < items.size() && i < 0; ++k)
      if (!box_.Text().empty() && ctl::StartsWithNoCase(items[k], box_.Text())) i = static_cast<int>(k);
  }
  list_.SetHighlight(i);
  list_.EnsureVisible(i);
}

void ComboBox::SetOpen(bool open) {
  if (open == popup_.open) return;
  if (!open) {
    popup_.Close();
    inList_ = false;
    return;
  }
  if (!enabled || items.empty()) return;
  Opened();
  const float u = Ui();
  beforeOpen_ = box_.Text();
  list_.items = items;
  list_.label = Name();
  int cur = IndexOf(box_.Text());
  list_.SetSelected(cur);
  list_.SetHighlight(cur);
  float w = listWidth > 0 ? listWidth * u : std::max(Width(rc), list_.PreferredWidth() + 8 * u);
  float rows = static_cast<float>(std::min<size_t>(items.size(), kMaxPopupRows));
  popup_.Place(rc, w, rows * list_.rowHeight * u + 8 * u, PopupBounds(), u);
  list_.rc = popup_.Inner(u);
  list_.ResetScroll();
  list_.EnsureVisible(cur);
  popup_.Open();
}

bool ComboBox::HitPopup(float x, float y) const { return popup_.Hit(x, y); }

void ComboBox::Draw(Canvas& c) {
  const Palette& p = theme::P();
  const float u = Ui();
  box_.rc = rc;
  box_.Draw(c, u, Focused() && CaretOn());
  D2D1_RECT_F a = ArrowRect();
  if (enabled) {
    c.FillRound(a, 3 * u, Alpha(p.hover, arrowHover_.V()));
    c.FillRound(a, 3 * u, Alpha(p.pressed, std::max(arrowPress_.V(), popup_.open ? 1.f : 0.f)));
  }
  ctl::DrawChevron(c, a, enabled ? p.textSecondary : p.textDisabled, u);
}

void ComboBox::DrawPopup(Canvas& c) {
  if (!popup_.Visible()) return;
  popup_.Begin(c, Ui());
  list_.Draw(c);
  popup_.End(c);
}

bool ComboBox::OnMouseMove(float x, float y) {
  if (inList_ || HitPopup(x, y)) return list_.OnMouseMove(x, y);
  if (selecting_ && GetKeyState(VK_LBUTTON) < 0) {
    box_.OnMouseDrag(x, y, Ui());
    return true;
  }
  bool over = Contains(ArrowRect(), x, y);
  bool changed = over != (arrowHover_.Target() > 0.5f);
  HoverTo(arrowHover_, over);
  return changed;
}

void ComboBox::OnMouseLeave() {
  HoverTo(arrowHover_, false);
  list_.OnMouseLeave();
}

void ComboBox::OnMouseDown(float x, float y, bool dbl, bool shift) {
  const bool first = justFocused_;
  justFocused_ = false;
  if (HitPopup(x, y)) {
    inList_ = true;
    list_.OnMouseDown(x, y, dbl, shift);
    return;
  }
  if (Contains(ArrowRect(), x, y)) {
    arrowPress_.To(1, 40);
    SetOpen(!popup_.open);
    box_.SelectAll();
    return;
  }
  // The first click selects the whole value, ready to be typed over, like the page box.
  if (first && !shift && !dbl && !box_.Text().empty()) {
    box_.SelectAll();
    return;
  }
  selecting_ = true;
  if (dbl) box_.OnDoubleClick();
  else box_.OnMouseDown(x, y, shift, Ui());
}

void ComboBox::OnMouseUp(float x, float y) {
  selecting_ = false;
  arrowPress_.To(0, 120);
  if (inList_) {
    inList_ = false;
    list_.OnMouseUp(x, y);
  }
}

bool ComboBox::OnWheel(int delta, float x, float y) { return HitPopup(x, y) && list_.OnWheel(delta, x, y); }

bool ComboBox::OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt) {
  if (!enabled) return false;
  const bool open = popup_.open;
  switch (vk) {
    case VK_F4: SetOpen(!open); return true;
    case VK_UP:
    case VK_DOWN:
      if (alt || !open) {
        SetOpen(!open);
        return true;
      }
      [[fallthrough]];
    case VK_PRIOR:
    case VK_NEXT:
      if (!open) return false;
      // Arrowing through the open list shows each item in the box; Enter applies it.
      list_.OnKeyDown(vk, false, false, false);
      if (list_.Highlight() >= 0) box_.SetText(items[list_.Highlight()], true);
      return true;
    case VK_RETURN:
      // Applied: false lets a toolbar hand the keyboard back. Refused text keeps it.
      if (open && list_.Highlight() >= 0 && IndexOf(box_.Text()) != list_.Highlight()) {
        Pick(list_.Highlight());
        return false;
      }
      if (!Commit(true)) return true;
      SetOpen(false);
      box_.SelectAll();
      return false;
    case VK_ESCAPE:
      if (open) {
        SetOpen(false);
        box_.SetText(beforeOpen_, true);
        return true;
      }
      Revert();
      return false;
    case VK_TAB:
      if (box_.Text() != committed_ && !Commit(false)) Revert();
      SetOpen(false);
      return false;
    default: break;
  }
  if (box_.OnKeyDown(vk, ctrl, shift)) {
    box_.error = false;
    FollowText();
    return true;
  }
  return false;
}

bool ComboBox::OnChar(wchar_t ch) {
  if (!enabled) return false;
  if (ch < 32 || ch == 127) return box_.OnChar(ch);
  box_.error = false;
  box_.OnChar(ch);
  if (autoComplete && box_.AtEnd()) {
    const std::wstring typed = box_.Text();
    for (const auto& item : items) {
      if (item.size() > typed.size() && ctl::StartsWithNoCase(item, typed)) {
        box_.SetText(item);
        box_.SetSelection(static_cast<int>(typed.size()), static_cast<int>(item.size()));
        break;
      }
    }
  }
  FollowText();
  return true;
}

void ComboBox::OnFocus(bool focused) {
  box_.Focus(focused);
  if (focused) {
    box_.SelectAll();
    justFocused_ = true;
    return;
  }
  justFocused_ = false;
  SetOpen(false);
  if (box_.Text() != committed_ && !Commit(false)) Revert();
  box_.error = false;
}

bool ComboBox::OverText(float x, float y) const { return Hit(x, y) && !Contains(ArrowRect(), x, y); }

void ComboBox::Describe(uia::Tree& t, int parent) {
  const float u = Ui();
  auto setValue = [this](const std::wstring& v) {
    box_.SetText(v, true);
    if (!Commit(true)) Revert();
  };
  int i = t.Add(parent, id, UIA_ComboBoxControlTypeId, Name(), rc);
  {
    uia::Node& n = t[i];
    n.accelerator = uia::SplitShortcut(tooltip).second;
    n.enabled = enabled;
    n.focusable = true;
    n.focus = [this] { TakeFocus(); };
    n.expand = popup_.open ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
    n.doExpand = [this](bool e) {
      TakeFocus();
      SetOpen(e);
    };
    n.hasValue = true;
    n.value = box_.Text();
    n.setValue = setValue;
  }
  int e = t.Add(i, 1, UIA_EditControlTypeId, Name(), RectF(rc.left, rc.top, rc.right - 30 * u, rc.bottom));
  {
    uia::Node& n = t[e];
    n.enabled = enabled;
    n.focusable = true;
    n.focused = Focused();
    n.focus = [this] { TakeFocus(); };
    n.hasValue = true;
    n.value = box_.Text();
    n.setValue = setValue;
  }
  int b = t.Add(i, 2, UIA_ButtonControlTypeId, popup_.open ? L"Close" : L"Open", ArrowRect());
  t[b].enabled = enabled;
  t[b].invoke = [this] {
    TakeFocus();
    SetOpen(!popup_.open);
  };
  if (!popup_.open) {
    list_.items = items;
    list_.label = Name();
    list_.SetSelected(IndexOf(box_.Text()));
  }
  list_.DescribeAsPart(t, i, 3, popup_.open, false, [this](int k) { Pick(k); });
}
