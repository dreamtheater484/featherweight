#include "ui/Toolbar.h"

#include <algorithm>
#include <cmath>

#include "core/Lang.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/Theme.h"

using namespace gfx;

namespace {
constexpr float kBtn = 36;   // icon button width
constexpr float kBtnH = 32;  // icon button height
constexpr float kGap = 2;
constexpr float kSep = 13;   // width taken by a separator
}  // namespace

Button* Toolbar::Make(Button& b, int cmd, wchar_t glyph, const wchar_t* tooltip) {
  b.id = cmd;
  b.glyph = glyph;
  b.tooltip = tooltip;
  all_.push_back(&b);
  set_.Add(&b);
  return &b;
}

Button& Toolbar::ItemButton(const ToolItem& item) {
  auto [it, added] = items_.try_emplace(item.cmd);
  Button& b = it->second;
  if (added) {
    b.id = item.cmd;
    all_.push_back(&b);
    set_.Add(&b);
  }
  b.glyph = item.glyph;
  b.tooltip = item.tooltip;
  b.glyphSize = item.glyphSize;
  b.dropdown = item.dropdown;
  b.enabled = item.enabled;
  b.checked = item.checked;
  toggles_[item.cmd] = item.toggle;
  return b;
}

Control& Toolbar::ItemControl(const ToolItem& item) {
  std::unique_ptr<Control> none;  // a tab's own control (Custom) takes no slot here
  ToolKind noKind = item.kind;
  const bool own = item.kind == ToolKind::Custom && item.control;
  return ConfigureControl(own ? none : controls_[item.cmd], own ? noKind : controlKinds_[item.cmd], item, [this](int cmd) {
    if (onCommand) onCommand(cmd);
  });
}

Control& Toolbar::ConfigureControl(std::unique_ptr<Control>& slot, ToolKind& slotKind, const ToolItem& item,
                                   const std::function<void(int)>& invoke) {
  if (item.kind == ToolKind::Custom && item.control) {
    Control& c = *item.control;
    c.id = item.cmd;
    c.tooltip = item.tooltip;
    c.enabled = item.enabled;
    return c;
  }
  if (!slot || slotKind != item.kind) {
    switch (item.kind) {
      case ToolKind::Dropdown: slot = std::make_unique<Dropdown>(); break;
      case ToolKind::ComboBox: slot = std::make_unique<ComboBox>(); break;
      case ToolKind::SplitButton: slot = std::make_unique<SplitButton>(); break;
      case ToolKind::ColorPicker: slot = std::make_unique<ColorPicker>(); break;
      default: slot = std::make_unique<SpinBox>(); break;
    }
    slotKind = item.kind;
  }
  Control& c = *slot;
  c.id = item.cmd;
  c.tooltip = item.tooltip;
  c.enabled = item.enabled;
  auto notify = item.onChange;
  auto send = [notify](const ToolValue& v) {
    if (notify) notify(v);
  };
  // An open list keeps what it shows, and a box being typed in keeps its text.
  switch (item.kind) {
    case ToolKind::Dropdown: {
      auto& d = static_cast<Dropdown&>(c);
      if (!d.IsOpen()) {
        d.items = item.choices;
        d.selected = item.choice;
      }
      d.glyph = item.glyph;
      d.placeholder = item.text;
      d.maxRows = item.maxRows;
      d.onChange = [send, &d](int i) {
        ToolValue v;
        v.index = i;
        v.text = d.items[i];
        send(v);
      };
      break;
    }
    case ToolKind::ComboBox: {
      auto& b = static_cast<ComboBox&>(c);
      if (!b.IsOpen()) b.items = item.choices;
      b.SetText(item.text);
      b.validate = item.validate;
      b.onCommit = [send](const std::wstring& text, int index) {
        ToolValue v;
        v.index = index;
        v.text = text;
        send(v);
      };
      break;
    }
    case ToolKind::SplitButton: {
      auto& s = static_cast<SplitButton&>(c);
      s.glyph = item.glyph;
      s.colors = s.showColor = item.colors;
      s.color = item.color;
      s.automaticLabel = item.automaticLabel;
      if (item.colors) s.theme = item.theme.empty() ? ColorPalette::DefaultTheme() : item.theme;
      if (!s.IsOpen()) {
        s.items = item.choices;
        s.selected = item.choice;
      }
      const int cmd = item.cmd;
      s.onInvoke = [invoke, cmd] {
        if (invoke) invoke(cmd);
      };
      s.onPick = [send](int i) {
        ToolValue v;
        v.index = i;
        send(v);
      };
      s.onColor = [send](D2D1_COLOR_F color, bool automatic) {
        ToolValue v;
        v.color = color;
        v.automatic = automatic;
        send(v);
      };
      break;
    }
    case ToolKind::ColorPicker: {
      auto& p = static_cast<ColorPicker&>(c);
      p.glyph = item.glyph;
      p.color = item.color;
      p.automatic = item.automatic;
      p.automaticLabel = item.automaticLabel;
      p.automaticColor = item.automaticColor;
      p.theme = item.theme.empty() ? ColorPalette::DefaultTheme() : item.theme;
      p.onChange = [send](D2D1_COLOR_F color, bool automatic) {
        ToolValue v;
        v.color = color;
        v.automatic = automatic;
        send(v);
      };
      break;
    }
    default: {
      auto& s = static_cast<SpinBox&>(c);
      s.minValue = item.minValue;
      s.maxValue = item.maxValue;
      s.step = item.step;
      s.decimals = item.decimals;
      s.unit = item.unit;
      s.SetValue(item.value);
      s.onChange = [send](double number) {
        ToolValue v;
        v.number = number;
        send(v);
      };
      break;
    }
  }
  return c;
}

Toolbar::Toolbar() {
  Make(open_, CmdOpen, icon::FolderOpen, T(L"Open (Ctrl+O)"));
  Make(prev_, CmdPrevPage, icon::ChevronUp, T(L"Previous page (Page Up)"));
  Make(next_, CmdNextPage, icon::ChevronDown, T(L"Next page (Page Down)"));
  Make(zoomOut_, CmdZoomOut, icon::ZoomOut, T(L"Zoom out (Ctrl+−)"));
  Make(zoom_, CmdZoomMenu, 0, T(L"Zoom"));
  Make(zoomIn_, CmdZoomIn, icon::ZoomIn, L"Zoom in (Ctrl+=)");
  Make(more_, CmdMoreMenu, icon::More, T(L"More"));
  Make(overflowButton_, CmdToolbarOverflow, icon::ChevronRight, T(L"More tools"));
  overflowButton_.glyphSize = 14;
  zoom_.dropdown = true;
  prev_.glyphSize = next_.glyphSize = 14;
  pageBox_.digitsOnly = true;
  pageBox_.maxLength = 6;
  pageBox_.align = Align::Center;
}

float Toolbar::WidthOf(const ToolItem& item) {
  if (!item.fitText || item.text.empty() || item.kind != ToolKind::Dropdown) return item.width;
  if (textUi_ != ui_) {
    textWidths_.clear();
    textUi_ = ui_;
  }
  auto [it, added] = textWidths_.try_emplace(item.text, 0.f);
  if (added) {  // measured as the dropdown draws it: 13 DIPs, at this scale (its hinting)
    IDWriteTextFormat* fmt = Canvas::Format(13 * ui_, DWRITE_FONT_WEIGHT_REGULAR, false);
    ComPtr<IDWriteTextLayout> layout;
    DWrite()->CreateTextLayout(item.text.c_str(), static_cast<UINT32>(item.text.size()), fmt, 10000, 100, &layout);
    DWRITE_TEXT_METRICS tm{};
    if (layout) layout->GetMetrics(&tm);
    it->second = std::ceil(tm.widthIncludingTrailingWhitespace) / ui_;
  }
  // Dropdown::Draw: 11 DIPs before the text (and a glyph's 24), the chevron's 28 after it, and 2 to spare.
  return std::max(item.width, std::ceil(11 + (item.glyph ? 24 : 0) + it->second + 2 + 28));
}

void Toolbar::Layout(const RECT& rc, float ui, const ToolbarModel& m) {
  rc_ = rc;
  ui_ = ui;
  separators_.clear();
  const float u = ui;
  const float top = rc.top + std::round(((rc.bottom - rc.top) - kBtnH * u) / 2);
  const float h = kBtnH * u;
  auto place = [&](Button& b, float x, float w) {
    b.rc = RectF(std::round(x), top, std::round(x + w), top + h);
    b.visible = true;
  };
  // Boxes (dropdowns, combo boxes, spin boxes) are as tall as the page box; the others as buttons.
  auto placeControl = [&](Control& c, ToolKind kind, float x, float w) {
    const bool box = kind == ToolKind::Dropdown || kind == ToolKind::ComboBox || kind == ToolKind::SpinBox;
    const float ch = box ? 28 * u : h;
    const float cy = box ? std::round((rc.top + rc.bottom) / 2.0f - ch / 2) : top;
    c.rc = RectF(std::round(x), cy, std::round(x + w), cy + ch);
    c.visible = true;
  };
  for (Button* b : all_) b->visible = false;
  for (auto& [cmd, c] : controls_)
    if (c) c->visible = false;
  pageBox_.rc = {};
  shown_.clear();

  // Left group: Open, then the tab's own items. When they don't fit beside More and the right
  // group's important items (priority 5 and up), items below priority 9 go, the lowest first, into
  // the overflow menu; its button (>) takes the place after the last item that stays.
  float x = rc.left + 8 * u;
  place(open_, x, kBtn * u);
  x += (kBtn + kGap) * u;
  const float overflowW = (kBtn + kGap) * u;
  std::vector<const ToolItem*> left;
  for (const ToolItem& item : m.left) left.push_back(&item);
  float keepRight = (kBtn + kGap) * u + 16 * u;
  for (const ToolItem& item : m.right)
    if (item.priority >= 5) keepRight += (WidthOf(item) + kGap + (item.sepBefore ? kSep : 0)) * u;
  auto leftWidth = [&]() {
    float w = 0;
    for (const ToolItem* it : left) w += (WidthOf(*it) + (it->joinNext ? 0 : kGap) + (it->sepBefore ? kSep : 0)) * u;
    return w;
  };
  auto fits = [&]() { return x + leftWidth() + (left.size() < m.left.size() ? overflowW : 0) + keepRight <= rc.right; };
  std::vector<const ToolItem*> dropped;
  while (!fits()) {
    auto victim = std::min_element(left.begin(), left.end(),
                                   [](const ToolItem* a, const ToolItem* b) { return a->priority < b->priority; });
    if (victim == left.end() || (*victim)->priority >= 9) break;
    dropped.push_back(*victim);
    left.erase(victim);
  }
  // A wide box gone can leave room for smaller items dropped before it: they come back.
  for (auto d = dropped.rbegin(); d != dropped.rend(); ++d) {
    auto at = left.begin();
    while (at != left.end() && *at < *d) ++at;  // the bar's order (the items are m.left's)
    at = left.insert(at, *d);
    if (!fits()) left.erase(at);
  }
  overflow_.clear();
  for (const ToolItem& item : m.left)
    if (std::find(left.begin(), left.end(), &item) == left.end()) overflow_.push_back(item);
  const bool leftOverflow = !overflow_.empty();
  for (const ToolItem* it : left) {
    const ToolItem& item = *it;
    if (item.sepBefore) {
      separators_.push_back(RectF(x + kSep * u / 2 - kGap * u / 2, top + 7 * u, x + kSep * u / 2 - kGap * u / 2 + 1, top + h - 7 * u));
      x += kSep * u;
    }
    const float w = WidthOf(item);
    if (item.kind == ToolKind::Button) {
      place(ItemButton(item), x, w * u);
    } else {
      Control& c = ItemControl(item);
      placeControl(c, item.kind, x, w * u);
      shown_.push_back(&c);
    }
    x += (w + (item.joinNext ? 0 : kGap)) * u;
  }
  if (leftOverflow) {
    place(overflowButton_, x, kBtn * u);
    x += overflowW;
  }
  const float leftEnd = x;

  // Right group, dropping less important items when space is tight.
  struct Item {
    Button* b;
    Control* c;
    ToolKind kind;
    float w;
    int priority;  // lower = dropped first
    bool sepBefore;
    const ToolItem* src;  // null for More
  };
  std::vector<Item> right;
  for (const ToolItem& item : m.right) {
    if (item.kind == ToolKind::Button)
      right.push_back({&ItemButton(item), nullptr, item.kind, WidthOf(item) * u, item.priority, item.sepBefore, &item});
    else
      right.push_back({nullptr, &ItemControl(item), item.kind, WidthOf(item) * u, item.priority, item.sepBefore, &item});
  }
  right.push_back({&more_, nullptr, ToolKind::Button, kBtn * u, 9, false, nullptr});

  // Center group width.
  pageLabelText_ = m.pageCount > 0 ? L"of " + std::to_wstring(m.pageCount) : L"";
  float labelW = 0;
  if (!pageLabelText_.empty()) {
    IDWriteTextFormat* fmt = Canvas::Format(13 * u, DWRITE_FONT_WEIGHT_REGULAR, false);
    ComPtr<IDWriteTextLayout> layout;
    DWrite()->CreateTextLayout(pageLabelText_.c_str(), static_cast<UINT32>(pageLabelText_.size()), fmt, 1000, 100,
                               &layout);
    DWRITE_TEXT_METRICS tm{};
    if (layout) layout->GetMetrics(&tm);
    labelW = std::ceil(tm.widthIncludingTrailingWhitespace);
  }
  const float pageBoxW = std::max(40.f, 10.f + 8.f * std::to_wstring(std::max(1, m.pageCount)).size()) * u;
  const float zoomW = 76 * u;
  float centerW = 0;
  if (m.pager) {
    centerW = kBtn * u + 4 * u + pageBoxW + 8 * u + labelW + 4 * u + kBtn * u  // pages
              + kSep * u                                                     // separator
              + kBtn * u + zoomW + kBtn * u + 2 * kGap * u;                  // zoom
  }

  auto rightWidth = [&]() {
    float w = 0;
    for (const auto& it : right) w += it.w + kGap * u + (it.sepBefore ? kSep * u : 0);
    return w;
  };
  float available = static_cast<float>(rc.right - rc.left) - (leftEnd - rc.left) - 8 * u;
  // What goes here joins the overflow menu too; its button comes before the right group when the
  // left group kept everything.
  std::vector<const ToolItem*> droppedRight;
  auto rightOverflowW = [&]() { return !leftOverflow && !droppedRight.empty() ? overflowW : 0; };
  while (rightWidth() + rightOverflowW() + centerW + 16 * u > available && right.size() > 1) {
    auto victim = std::min_element(right.begin(), right.end(),
                                   [](const Item& a, const Item& b) { return a.priority < b.priority; });
    if (victim->src) droppedRight.push_back(victim->src);
    right.erase(victim);
  }
  for (const ToolItem& item : m.right)
    if (std::find(droppedRight.begin(), droppedRight.end(), &item) != droppedRight.end()) overflow_.push_back(item);
  for (const Item& it : right)
    if (it.c) shown_.push_back(it.c);
  float rx = rc.right - 8 * u;
  for (auto it = right.rbegin(); it != right.rend(); ++it) {
    rx -= it->w;
    if (it->b) place(*it->b, rx, it->w);
    else placeControl(*it->c, it->kind, rx, it->w);
    rx -= kGap * u;
    if (it->sepBefore) {
      separators_.push_back(RectF(rx - kSep * u / 2, top + 7 * u, rx - kSep * u / 2 + 1, top + h - 7 * u));
      rx -= kSep * u;
    }
  }
  if (!leftOverflow && !overflow_.empty()) {
    rx -= kBtn * u;
    place(overflowButton_, rx, kBtn * u);
    rx -= kGap * u;
  }
  if (!overflow_.empty()) {
    std::wstring names;
    for (const ToolItem& item : overflow_) names += (names.empty() ? L"" : L", ") + MenuName(item);
    overflowButton_.tooltip = T(L"More tools: ") + names;
  }
  const float rightStart = rx;

  if (m.pager) {
    float cx = std::round((rc.left + rc.right) / 2.0f - centerW / 2);
    cx = std::max(cx, leftEnd + 8 * u);
    if (cx + centerW > rightStart) cx = std::max(leftEnd + 4 * u, rightStart - centerW);
    place(prev_, cx, kBtn * u);
    cx += kBtn * u + 4 * u;
    float boxH = 28 * u;
    float by = std::round((rc.top + rc.bottom) / 2.0f - boxH / 2);
    pageBox_.rc = RectF(cx, by, cx + pageBoxW, by + boxH);
    cx += pageBoxW + 8 * u;
    pageLabel_ = RectF(cx, top, cx + labelW + 2, top + h);
    cx += labelW + 4 * u;
    place(next_, cx, kBtn * u);
    cx += kBtn * u;
    separators_.push_back(RectF(cx + kSep * u / 2, top + 7 * u, cx + kSep * u / 2 + 1, top + h - 7 * u));
    cx += kSep * u;
    place(zoomOut_, cx, kBtn * u);
    cx += kBtn * u + kGap * u;
    place(zoom_, cx, zoomW);
    cx += zoomW + kGap * u;
    place(zoomIn_, cx, kBtn * u);
  }

  // The tab's controls: Tab order follows the bar, and open lists may use the window below it.
  group_.ui = ui;
  RECT client = rc;
  if (group_.hwnd) GetClientRect(group_.hwnd, &client);
  group_.bounds = RectF(static_cast<float>(rc.left), static_cast<float>(rc.top), static_cast<float>(rc.right),
                        static_cast<float>(std::max(client.bottom, rc.bottom)));
  group_.Assign(shown_);

  // State of the center group (the tab's own buttons got theirs above).
  prev_.enabled = m.page > 0;
  next_.enabled = m.page + 1 < m.pageCount;
  zoom_.text = m.zoomText;
  if (!pageBox_.focused) pageBox_.SetText(m.pageCount > 0 ? std::to_wstring(m.page + 1) : L"");
}

void Toolbar::Draw(Canvas& c, const ToolbarModel& m, bool caretVisible) {
  const Palette& p = theme::P();
  D2D1_RECT_F r = RectF(rc_);
  c.Fill(r, p.chrome);
  c.Fill(RectF(r.left, r.bottom - 1, r.right, r.bottom), p.chromeBorder);
  for (Button* b : all_) DrawButton(c, *b, ui_);
  group_.Draw(c);
  for (const auto& sep : separators_) c.Fill(sep, Alpha(p.textSecondary, 0.28f));
  if (m.pager && pageBox_.rc.right > 0) {
    pageBox_.Draw(c, ui_, caretVisible);
    c.Text(pageLabelText_, pageLabel_, p.textSecondary, 13 * ui_);
  }
}

bool Toolbar::OnMouseMove(int x, int y) {
  bool changed = group_.OnMouseMove((float)x, (float)y);
  if (group_.Dragging() || group_.HitPopup((float)x, (float)y)) {
    set_.OnMouseLeave();
    return changed;
  }
  return set_.OnMouseMove((float)x, (float)y) || changed;
}

void Toolbar::OnMouseLeave() {
  group_.OnMouseLeave();
  set_.OnMouseLeave();
}

bool Toolbar::OnMouseDown(int x, int y, bool dbl, bool shift) {
  Control* open = group_.Open();
  if (group_.OnMouseDown((float)x, (float)y, dbl, shift)) {
    HandBackAfterChoice(open);
    return true;
  }
  group_.Blur();  // a click anywhere else on the bar leaves the tab's boxes
  return set_.OnMouseDown((float)x, (float)y);
}

int Toolbar::OnMouseUp(int x, int y) {
  Control* open = group_.Open();
  group_.OnMouseUp((float)x, (float)y);
  HandBackAfterChoice(open);
  return set_.OnMouseUp((float)x, (float)y);
}

void Toolbar::HandBackAfterChoice(Control* open) {
  // A choice from a list made with the mouse gives the keyboard back to the page, as Word's boxes
  // and the plain buttons do: typing goes on where it was.
  if (open && !group_.Open() && group_.Focused() == open && !group_.FocusVisible()) group_.Blur();
}

bool Toolbar::OnWheel(int delta, int x, int y) { return group_.OnWheel(delta, (float)x, (float)y); }

void Toolbar::OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt) {
  if (!group_.Focused()) return;
  bool used = group_.OnKeyDown(vk, ctrl, shift, alt);
  if (!used && (vk == VK_RETURN || vk == VK_ESCAPE) && !group_.Open()) group_.Blur();
}

std::wstring Toolbar::TooltipAt(int x, int y) const {
  if (group_.Hit((float)x, (float)y)) return group_.TooltipAt((float)x, (float)y);
  if (HitPageBox(x, y)) return T(L"Go to page (Ctrl+G)");
  for (Button* b : all_)
    if (b->visible && Contains(b->rc, (float)x, (float)y)) return b->tooltip;
  return {};
}

D2D1_RECT_F Toolbar::ButtonRect(int cmd) const {
  for (Button* b : all_)
    if (b->id == cmd && b->visible) return b->rc;
  // A button in the overflow menu (Page layout): its own menu opens under the overflow button.
  return overflowButton_.visible ? overflowButton_.rc : D2D1_RECT_F{};
}

std::wstring Toolbar::MenuName(const ToolItem& item) {
  if (!item.menuLabel.empty()) return item.menuLabel;
  // "Borders: Bottom border", "Font color (Ctrl+...)": the name before the detail and the keys.
  std::wstring name = uia::SplitShortcut(item.tooltip).first;
  if (const size_t colon = name.find(L':'); colon != std::wstring::npos) name.resize(colon);
  return name.empty() ? item.text : name;
}

namespace {
// Menus take & as the next letter's underline: a name's own & is doubled.
std::wstring MenuText(const std::wstring& s) {
  std::wstring out;
  for (wchar_t c : s) {
    if (c == L'&') out += L'&';
    out += c;
  }
  return out;
}
}  // namespace

void Toolbar::FillOverflowMenu(HMENU menu) {
  using menu::AppendItem;
  using menu::AppendSeparator;
  // The items as they are now: the bar lays out again while the menu is open.
  menuItems_ = overflow_;
  menuPicks_.clear();
  auto id = [&](MenuPick::What what, size_t item, int choice = -1, D2D1_COLOR_F color = {}) {
    menuPicks_.push_back({what, item, choice, color});
    return static_cast<UINT>(kOverflowFirst + menuPicks_.size() - 1);
  };
  for (size_t i = 0; i < menuItems_.size(); ++i) {
    const ToolItem& it = menuItems_[i];
    const std::wstring name = MenuText(MenuName(it));
    const std::wstring keys = uia::SplitShortcut(it.tooltip).second;
    const std::wstring withKeys = name + (keys.empty() ? L"" : L"\t" + keys);
    // A button, or a tab's own control (Word's table grid): its command (the grid's opens the
    // Table panel).
    if (it.kind == ToolKind::Button || it.kind == ToolKind::Custom) {
      AppendItem(menu, id(MenuPick::Run, i), withKeys, it.enabled, it.checked);
      continue;
    }
    if (it.kind == ToolKind::SpinBox) continue;  // none of the tabs has one on the bar
    HMENU sub = CreatePopupMenu();
    if (it.kind == ToolKind::SplitButton) {  // its main part, then its list
      AppendItem(sub, id(MenuPick::Run, i), withKeys, it.enabled);
      if (it.colors || !it.choices.empty()) AppendSeparator(sub);
    }
    if (it.colors || it.kind == ToolKind::ColorPicker) {
      if (!it.automaticLabel.empty()) AppendItem(sub, id(MenuPick::Automatic, i), MenuText(it.automaticLabel), it.enabled);
      for (const NamedColor& c : it.theme.empty() ? ColorPalette::DefaultTheme() : it.theme)
        AppendItem(sub, id(MenuPick::Color, i, -1, c.color), MenuText(c.name), it.enabled);
      AppendSeparator(sub);
      for (const NamedColor& c : ColorPalette::StandardColors())
        AppendItem(sub, id(MenuPick::Color, i, -1, c.color), MenuText(c.name), it.enabled);
      AppendSeparator(sub);
      AppendItem(sub, id(MenuPick::MoreColors, i), T(L"More colors\u2026"), it.enabled);
    } else {
      for (size_t k = 0; k < it.choices.size(); ++k) {
        const bool chosen = it.kind == ToolKind::ComboBox ? it.choices[k] == it.text : static_cast<int>(k) == it.choice;
        AppendItem(sub, id(MenuPick::Choice, i, static_cast<int>(k)), MenuText(it.choices[k]), it.enabled, chosen,
                   it.kind != ToolKind::SplitButton);
      }
    }
    menu::AppendSubmenu(menu, sub, name, it.enabled);
  }
}

int Toolbar::OverflowChoice(int id) {
  if (id < kOverflowFirst || static_cast<size_t>(id - kOverflowFirst) >= menuPicks_.size()) return id;
  const MenuPick pick = menuPicks_[static_cast<size_t>(id - kOverflowFirst)];
  const ToolItem& it = menuItems_[pick.item];
  ToolValue v;
  switch (pick.what) {
    case MenuPick::Run: return it.cmd;
    case MenuPick::Choice:
      v.index = pick.choice;
      v.text = it.choices[static_cast<size_t>(pick.choice)];
      break;
    case MenuPick::Automatic:
      v.automatic = true;
      v.color = it.automaticColor;
      break;
    case MenuPick::Color: v.color = pick.color; break;
    case MenuPick::MoreColors:
      if (!ColorPalette::ChooseMore(group_.hwnd, it.automatic ? D2D1::ColorF(0, 0, 0) : it.color, v.color)) return 0;
      break;
  }
  if (it.onChange) it.onChange(v);
  return 0;
}

void Toolbar::Describe(uia::Tree& t, int parent, const std::function<bool(int)>& isMenu, int openMenu,
                       const std::function<void(TextBox*)>& focusBox) {
  int bar = t.Add(parent, CmdMoreMenu, UIA_ToolBarControlTypeId, T(L"Commands"), RectF(rc_));
  t[bar].orientation = OrientationType_Horizontal;
  // Everything in reading order, left to right.
  struct Entry {
    float left;
    Button* button;
    Control* control;
  };
  std::vector<Entry> entries;
  for (Button* b : all_)
    if (b->visible) entries.push_back({b->rc.left, b, nullptr});
  for (Control* c : shown_)
    if (c->visible) entries.push_back({c->rc.left, nullptr, c});
  if (pageBox_.rc.right > 0) entries.push_back({pageBox_.rc.left, nullptr, nullptr});
  std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.left < b.left; });

  const HWND hwnd = group_.hwnd;
  for (const Entry& e : entries) {
    if (e.control) {
      e.control->Describe(t, bar);
      continue;
    }
    if (!e.button) {
      uia::AddTextBox(t, bar, CmdGoToPage, pageBox_, T(L"Go to page (Ctrl+G)"), [focusBox, this] { focusBox(&pageBox_); });
      if (!pageLabelText_.empty()) t.Add(bar, CmdGoToPage + 1000, UIA_TextControlTypeId, pageLabelText_, pageLabel_);
      continue;
    }
    const int cmd = e.button->id;
    int i = uia::AddButton(t, bar, cmd, *e.button, [this, cmd] {
      if (onCommand) onCommand(cmd);
    });
    uia::Node& n = t[i];
    if (!e.button->text.empty() && !e.button->tooltip.empty()) {
      // A button that shows a value (zoom): named by its tooltip, with the value as its status.
      n.name = uia::SplitShortcut(e.button->tooltip).first;
      n.status = e.button->text;
      n.help.clear();
    }
    if (isMenu && isMenu(cmd)) {
      // Opens a menu. Menus are modal, so the menu opens after the UI Automation call returns.
      n.invoke = nullptr;
      n.expand = openMenu == cmd ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
      n.doExpand = [hwnd, cmd](bool expand) {
        if (expand && hwnd) PostMessageW(hwnd, WM_COMMAND, cmd, 0);
      };
    } else if (auto it = toggles_.find(cmd); it != toggles_.end() && it->second) {
      n.invoke = nullptr;
      n.toggle = e.button->checked ? ToggleState_On : ToggleState_Off;
      n.doToggle = [this, cmd] {
        if (onCommand) onCommand(cmd);
      };
    }
  }
}
