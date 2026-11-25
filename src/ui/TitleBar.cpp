#include "ui/TitleBar.h"

#include <algorithm>
#include <cmath>

#include "core/Lang.h"
#include "ui/Icons.h"
#include "ui/Logo.h"
#include "ui/Theme.h"

using namespace gfx;

namespace {
constexpr float kCaptionW = 46;
constexpr float kMarkW = 44;
constexpr float kTabMaxW = 232;
constexpr float kTabMinW = 64;
constexpr float kNewTabW = 36;
constexpr float kDragMinW = 56;
}  // namespace

TitleBar::TitleBar() {
  min_.glyph = icon::ChromeMinimize;
  max_.glyph = icon::ChromeMaximize;
  close_.glyph = icon::ChromeClose;
  min_.style = max_.style = ButtonStyle::Caption;
  close_.style = ButtonStyle::CaptionClose;
  min_.glyphSize = max_.glyphSize = close_.glyphSize = 10;
  min_.tooltip = T(L"Minimize");
  max_.tooltip = T(L"Maximize");
  close_.tooltip = T(L"Close");
  newTab_.glyph = icon::Add;
  newTab_.glyphSize = 12;
  newTab_.tooltip = T(L"Open a file (Ctrl+O)");
  min_.id = 1;
  max_.id = 2;
  close_.id = 3;
  newTab_.id = 4;
  buttons_.Add(&min_);
  buttons_.Add(&max_);
  buttons_.Add(&close_);
  buttons_.Add(&newTab_);
  for (int i = 0; i < kQuickMax; ++i) {
    quick_[i].id = 10 + i;
    quick_[i].visible = false;
    buttons_.Add(&quick_[i]);
    quickList_[i].id = 20 + i;
    quickList_[i].visible = false;
    quickList_[i].glyph = icon::ChevronDown;
    quickList_[i].glyphSize = 8;
    buttons_.Add(&quickList_[i]);
  }
}

void TitleBar::SetQuickAccess(const std::vector<QuickButton>& buttons) {
  quickCount_ = std::min(static_cast<int>(buttons.size()), kQuickMax);
  for (int i = 0; i < kQuickMax; ++i) {
    Button& b = quick_[i];
    b.visible = i < quickCount_;
    quickList_[i].visible = b.visible && buttons[i].list;
    if (!b.visible) continue;
    quickList_[i].enabled = buttons[i].enabled;
    quickList_[i].tooltip = buttons[i].tooltip;
    quickCmd_[i] = buttons[i].cmd;
    b.glyph = buttons[i].glyph;
    b.tooltip = buttons[i].tooltip;
    b.enabled = buttons[i].enabled;
    b.glyphSize = 14;
  }
}

void TitleBar::Layout(const RECT& rc, float ui, int tabCount) {
  rc_ = rc;
  ui_ = ui;
  tabCount_ = tabCount;
  if (static_cast<int>(tabHover_.size()) != tabCount) tabHover_.resize(tabCount);
  const float top = static_cast<float>(rc.top), bottom = static_cast<float>(rc.bottom);
  float x = static_cast<float>(rc.right);
  close_.rc = RectF(x - kCaptionW * ui, top, x, bottom);
  x -= kCaptionW * ui;
  max_.rc = RectF(x - kCaptionW * ui, top, x, bottom);
  x -= kCaptionW * ui;
  min_.rc = RectF(x - kCaptionW * ui, top, x, bottom);
  x -= kCaptionW * ui;

  tabsLeft_ = rc.left + kMarkW * ui;
  if (quickCount_ > 0) {  // Save, Undo, Redo, then a little room before the tabs
    const float qw = 32 * ui, lw = 14 * ui, qh = 28 * ui, qy = std::round((top + bottom) / 2 - qh / 2 + 2 * ui);
    float qx = tabsLeft_;
    for (int i = 0; i < quickCount_; ++i) {
      quick_[i].rc = RectF(qx, qy, qx + qw, qy + qh);
      qx += qw;
      if (quickList_[i].visible) {
        quickList_[i].rc = RectF(qx, qy, qx + lw, qy + qh);
        qx += lw;
      }
    }
    tabsLeft_ = qx + 10 * ui;
  }
  float avail = x - tabsLeft_ - kNewTabW * ui - kDragMinW * ui;
  tabW_ = tabCount > 0 ? std::clamp(avail / tabCount, kTabMinW * ui, kTabMaxW * ui) : 0;
  tabW_ = std::floor(tabW_);
  float tabsRight = tabsLeft_ + tabW_ * tabCount;
  float btn = 28 * ui;
  float by = std::round((top + bottom) / 2 - btn / 2 + 2 * ui);
  newTab_.rc = RectF(tabsRight + 4 * ui, by, tabsRight + 4 * ui + btn, by + btn);
}

D2D1_RECT_F TitleBar::TabRect(int i) const {
  float l = tabsLeft_ + i * tabW_;
  return RectF(l, rc_.top + 7 * ui_, l + tabW_, static_cast<float>(rc_.bottom));
}

D2D1_RECT_F TitleBar::TabCloseRect(int i) const {
  D2D1_RECT_F t = TabRect(i);
  float s = 24 * ui_;
  float cy = (t.top + t.bottom) / 2;
  return RectF(t.right - 8 * ui_ - s, cy - s / 2, t.right - 8 * ui_, cy + s / 2);
}

void TitleBar::Draw(Canvas& c, const std::vector<TabInfo>& tabs, int active, bool windowActive, bool maximized) {
  const Palette& p = theme::P();
  const float u = ui_;
  D2D1_RECT_F bar = RectF(rc_);
  c.Fill(bar, p.titleBar);

  // App mark.
  float ms = 18 * u;
  float mx = std::round(rc_.left + (kMarkW * u - ms) / 2 + 2 * u), my = std::round((rc_.top + rc_.bottom) / 2.0f - ms / 2 + 2 * u);
  logo::Draw(c, mx, my, ms);

  // Tabs.
  const float radius = 8 * u;
  for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
    D2D1_RECT_F t = TabRect(i);
    float hv = i < static_cast<int>(tabHover_.size()) ? tabHover_[i].V() : 0;
    if (i == active) {
      // Raised tab that flows into the toolbar, with concave corners at the bottom.
      c.PushClip(RectF(t.left - radius, t.top, t.right + radius, t.bottom));
      c.FillRound(RectF(t.left, t.top, t.right, t.bottom + radius), radius, p.chrome);
      c.Fill(RectF(t.left - radius, t.bottom - radius, t.left, t.bottom), p.chrome);
      c.Fill(RectF(t.right, t.bottom - radius, t.right + radius, t.bottom), p.chrome);
      c.FillEllipse(t.left - radius, t.bottom - radius, radius, radius, p.titleBar);
      c.FillEllipse(t.right + radius, t.bottom - radius, radius, radius, p.titleBar);
      c.PopClip();
    } else {
      D2D1_RECT_F hr = RectF(t.left + 2 * u, t.top + 1 * u, t.right - 2 * u, t.bottom - 5 * u);
      c.FillRound(hr, 6 * u, Alpha(p.hover, hv * 1.3f));
      // Separator between neighbouring inactive tabs.
      bool nextIsActive = i + 1 == active;
      if (i + 1 < static_cast<int>(tabs.size()) && !nextIsActive && hv < 0.01f &&
          (i + 1 >= static_cast<int>(tabHover_.size()) || tabHover_[i + 1].V() < 0.01f)) {
        float sx = std::round(t.right) - 0.5f;
        float cy = (t.top + t.bottom) / 2 - 2 * u;
        c.Line(sx, cy - 8 * u, sx, cy + 8 * u, Alpha(p.textSecondary, 0.35f), 1);
      }
    }

    const bool showClose = i == active || i == hoverTab_;
    D2D1_RECT_F cr = TabCloseRect(i);
    float textRight = (showClose || tabs[i].modified) ? cr.left - 2 * u : t.right - 12 * u;
    D2D1_COLOR_F tc = i == active ? p.text : (windowActive ? p.textSecondary : p.textDisabled);
    c.Text(tabs[i].title, RectF(t.left + 12 * u, t.top, textRight, t.bottom - 3 * u), tc, 12.5f * u,
           i == active ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_REGULAR);

    float ccx = (cr.left + cr.right) / 2, ccy = (cr.top + cr.bottom) / 2 - 1.5f * u;
    D2D1_RECT_F crAdj = RectF(cr.left, cr.top - 1.5f * u, cr.right, cr.bottom - 1.5f * u);
    if (i == hoverTab_ && hoverClose_) c.FillRound(crAdj, 4 * u, p.hover);
    if (tabs[i].modified && !(i == hoverTab_)) {
      c.FillEllipse(ccx, ccy, 4 * u, 4 * u, i == active ? p.accentText : p.textSecondary);
    } else if (showClose) {
      c.Icon(icon::ChromeClose, crAdj, tc, 9 * u);
    }
  }

  for (int i = 0; i < quickCount_; ++i) {
    DrawButton(c, quick_[i], u, windowActive);
    DrawButton(c, quickList_[i], u, windowActive);
  }
  if (quickCount_ > 0) {
    const float sx = std::round(tabsLeft_ - 5 * u) - 0.5f, cy = (rc_.top + rc_.bottom) / 2.0f + 2 * u;
    c.Line(sx, cy - 8 * u, sx, cy + 8 * u, Alpha(p.textSecondary, 0.35f), 1);
  }

  // New tab button and caption buttons.
  max_.glyph = maximized ? icon::ChromeRestore : icon::ChromeMaximize;
  max_.tooltip = maximized ? T(L"Restore") : T(L"Maximize");
  DrawButton(c, newTab_, u, windowActive);
  DrawButton(c, min_, u, windowActive);
  DrawButton(c, max_, u, windowActive);
  DrawButton(c, close_, u, windowActive);
}

void TitleBar::SetNewTabMenu(bool menu, bool open) {
  newTabMenu_ = menu;
  newTabOpen_ = menu && open;
  newTab_.tooltip = menu ? T(L"New or open a file") : T(L"Open a file (Ctrl+O)");
}

TitleBar::Hit TitleBar::HitTest(int x, int y) const {
  float fx = static_cast<float>(x), fy = static_cast<float>(y);
  if (fy < rc_.top || fy >= rc_.bottom) return {};
  if (Contains(close_.rc, fx, fy)) return {Part::Close, -1};
  if (Contains(max_.rc, fx, fy)) return {Part::Maximize, -1};
  if (Contains(min_.rc, fx, fy)) return {Part::Minimize, -1};
  if (Contains(newTab_.rc, fx, fy)) return {Part::NewTab, -1};
  for (int i = 0; i < quickCount_; ++i) {
    if (Contains(quick_[i].rc, fx, fy)) return {Part::Quick, i};
    if (quickList_[i].visible && Contains(quickList_[i].rc, fx, fy)) return {Part::QuickList, i};
  }
  for (int i = 0; i < tabCount_; ++i) {
    if (Contains(TabRect(i), fx, fy)) {
      if (Contains(TabCloseRect(i), fx, fy)) return {Part::TabClose, i};
      return {Part::Tab, i};
    }
  }
  return {Part::Caption, -1};
}

bool TitleBar::OnMouseMove(int x, int y) {
  bool changed = buttons_.OnMouseMove(static_cast<float>(x), static_cast<float>(y));
  Hit h = HitTest(x, y);
  int tab = (h.part == Part::Tab || h.part == Part::TabClose) ? h.index : -1;
  bool closeHover = h.part == Part::TabClose;
  if (tab != hoverTab_) {
    if (hoverTab_ >= 0 && hoverTab_ < static_cast<int>(tabHover_.size())) tabHover_[hoverTab_].To(0, 150);
    if (tab >= 0) tabHover_[tab].To(1, 80);
    hoverTab_ = tab;
    changed = true;
  }
  if (closeHover != hoverClose_) {
    hoverClose_ = closeHover;
    changed = true;
  }
  return changed;
}

void TitleBar::OnMouseLeave() {
  buttons_.OnMouseLeave();
  if (hoverTab_ >= 0 && hoverTab_ < static_cast<int>(tabHover_.size())) tabHover_[hoverTab_].To(0, 150);
  hoverTab_ = -1;
  hoverClose_ = false;
}

void TitleBar::OnMouseDown(int x, int y, int button) {
  pressed_ = HitTest(x, y);
  if (button == 0) buttons_.OnMouseDown(static_cast<float>(x), static_cast<float>(y));
}

TitleBar::Action TitleBar::OnMouseUp(int x, int y, int button) {
  Hit down = pressed_;
  pressed_ = {};
  int id = button == 0 ? buttons_.OnMouseUp(static_cast<float>(x), static_cast<float>(y)) : 0;
  Hit up = HitTest(x, y);
  if (button == 2 && (up.part == Part::Tab || up.part == Part::TabClose) && down.index == up.index)
    return {Part::TabClose, up.index};  // middle-click closes a tab
  if (button != 0) return {};
  switch (id) {
    case 1: return {Part::Minimize, -1};
    case 2: return {Part::Maximize, -1};
    case 3: return {Part::Close, -1};
    case 4: return {Part::NewTab, -1};
    case 10:
    case 11:
    case 12: return {Part::Quick, id - 10};
    case 20:
    case 21:
    case 22: return {Part::QuickList, id - 20};
  }
  if (up.part == Part::TabClose && down.part == Part::TabClose && down.index == up.index) return {Part::TabClose, up.index};
  return {};
}

std::wstring TitleBar::TooltipAt(int x, int y, const std::vector<TabInfo>& tabs) const {
  Hit h = HitTest(x, y);
  switch (h.part) {
    case Part::Tab: return h.index < static_cast<int>(tabs.size()) ? tabs[h.index].path : L"";
    case Part::TabClose: return T(L"Close tab (Ctrl+W)");
    case Part::NewTab: return newTab_.tooltip;
    case Part::Quick: return quick_[h.index].tooltip;
    case Part::QuickList: return quickList_[h.index].tooltip;
    case Part::Minimize: return min_.tooltip;
    case Part::Maximize: return max_.tooltip;
    case Part::Close: return close_.tooltip;
    default: return {};
  }
}

void TitleBar::Describe(uia::Tree& t, int parent, const std::wstring& caption, const std::vector<TabInfo>& tabs,
                        int active, bool maximized, const std::function<void(Part, int)>& act) const {
  int bar = t.Add(parent, 1, UIA_TitleBarControlTypeId, caption, RectF(rc_));
  if (!tabs.empty()) {
    D2D1_RECT_F first = TabRect(0), last = TabRect(static_cast<int>(tabs.size()) - 1);
    int strip = t.Add(bar, 1, UIA_TabControlTypeId, T(L"Open files"), RectF(first.left, first.top, last.right, first.bottom));
    t[strip].orientation = OrientationType_Horizontal;
    t[strip].selectionContainer = true;
    t[strip].selectionRequired = true;
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
      int tab = t.Add(strip, i, UIA_TabItemControlTypeId, tabs[i].title, TabRect(i));
      uia::Node& n = t[tab];
      n.help = tabs[i].path;
      if (tabs[i].modified) n.status = T(L"Modified");
      n.selectable = true;
      n.selected = i == active;
      n.select = [act, i] { act(Part::Tab, i); };
      n.positionInSet = i + 1;
      n.sizeOfSet = static_cast<int>(tabs.size());
      int close = t.Add(tab, 1, UIA_ButtonControlTypeId, T(L"Close tab"), TabCloseRect(i));
      t[close].accelerator = i == active ? L"Ctrl+W" : L"";
      t[close].invoke = [act, i] { act(Part::TabClose, i); };
    }
  }
  for (int i = 0; i < quickCount_; ++i) uia::AddButton(t, bar, 6 + i, quick_[i], [act, i] { act(Part::Quick, i); });
  int newTab = uia::AddButton(t, bar, 2, newTab_, [act] { act(Part::NewTab, -1); });
  if (newTabMenu_) {
    // A menu button. The menu is modal, so `act` opens it after the UI Automation call returns.
    t[newTab].invoke = nullptr;
    t[newTab].expand = newTabOpen_ ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
    t[newTab].doExpand = [act](bool expand) {
      if (expand) act(Part::NewTab, -1);
    };
  }
  uia::AddButton(t, bar, 3, min_, [act] { act(Part::Minimize, -1); });
  int max = uia::AddButton(t, bar, 4, max_, [act] { act(Part::Maximize, -1); });
  t[max].name = maximized ? T(L"Restore") : T(L"Maximize");
  uia::AddButton(t, bar, 5, close_, [act] { act(Part::Close, -1); });
}
