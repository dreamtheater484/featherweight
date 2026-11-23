#include "ui/Ribbon.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>

#include "Commands.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Toolbar.h"

using namespace gfx;

namespace ui {

namespace {

// DIPs. The body: 3 DIPs, three rows of 24 (or two, spread), the group's name, 2 to spare.
constexpr float kContentTop = 3;
constexpr float kAreaH = 72;
constexpr float kRowH = 24;
constexpr float kNameH = 17;
constexpr float kPad = 5;         // inside a group, left and right
constexpr float kColGap = 3;      // between a group's columns
constexpr float kSepW = 3;        // between groups (a hairline in the middle)
constexpr float kSmallW = 24;     // a small button
constexpr float kArrowW = 12;     // a chevron after a small or medium button
constexpr float kLargeMinW = 42;
constexpr float kLargeIconH = 38;  // a large button's icon part
constexpr float kLargeGlyph = 26;
constexpr float kGlyph = 16;
constexpr float kItemFont = 12;
constexpr float kTabFont = 13;
constexpr float kNameFont = 11.5f;
constexpr float kCollapseW = 26;
constexpr float kGalleryScrollW = 16;
constexpr float kGridMenuH = 28;

bool IsMenuButton(const RibbonItem& it) {
  return (it.tool.kind == ToolKind::Button && it.tool.dropdown) || (it.tool.kind == ToolKind::Dropdown && it.tool.glyph);
}

// Whether the ribbon draws the item itself (buttons, menu buttons, split buttons with a menu or at
// large size); the rest are the toolbar's controls.
bool OwnDrawn(const RibbonItem& it, RibbonSize size) {
  switch (it.tool.kind) {
    case ToolKind::Button: return true;
    case ToolKind::Dropdown: return it.tool.glyph != 0;
    case ToolKind::SplitButton: return size == RibbonSize::Large || !it.menu.empty();
    default: return false;
  }
}

bool IsSplit(const RibbonItem& it, RibbonSize size) {
  return it.tool.kind == ToolKind::SplitButton && OwnDrawn(it, size);
}

// A large label's two lines: at "\n", or at the space that balances them.
std::pair<std::wstring, std::wstring> SplitLabel(const std::wstring& s, const std::function<float(const std::wstring&)>& w) {
  if (const size_t nl = s.find(L'\n'); nl != std::wstring::npos) return {s.substr(0, nl), s.substr(nl + 1)};
  std::pair<std::wstring, std::wstring> best{s, L""};
  float bestW = 1e9f;
  for (size_t i = s.find(L' '); i != std::wstring::npos; i = s.find(L' ', i + 1)) {
    const std::wstring a = s.substr(0, i), b = s.substr(i + 1);
    const float m = std::max(w(a), w(b));
    if (m < bestW) {
      bestW = m;
      best = {a, b};
    }
  }
  return best;
}

bool TipConflicts(const std::wstring& c, const std::vector<std::wstring>& used) {
  for (const std::wstring& t : used)
    if (!t.empty() && (t.compare(0, c.size(), c) == 0 || c.compare(0, t.size(), t) == 0)) return true;
  return false;
}

// Letters for an item or tab without its own: from its name, unique in `used`, never the start of
// another (typing it would pick the shorter one).
std::wstring AutoTip(const std::wstring& name, std::vector<std::wstring>& used, bool single, wchar_t lead = 0) {
  std::wstring letters, initials;
  bool start = true;
  for (wchar_t ch : name) {
    if (iswalnum(ch)) {
      letters += static_cast<wchar_t>(towupper(ch));
      if (start) initials += static_cast<wchar_t>(towupper(ch));
      start = false;
    } else {
      start = true;
    }
  }
  std::vector<std::wstring> cands;
  if (lead) {
    for (wchar_t ch : letters) cands.push_back({lead, ch});
  } else {
    if (single)
      for (wchar_t ch : letters) cands.push_back({ch});
    if (initials.size() >= 2) cands.push_back(initials.substr(0, 2));
    for (size_t k = 1; k < letters.size(); ++k)
      if (!letters.empty()) cands.push_back({letters[0], letters[k]});
  }
  for (const std::wstring& c : cands)
    if (!TipConflicts(c, used)) {
      used.push_back(c);
      return c;
    }
  for (wchar_t a = L'Y'; a <= L'Z'; ++a)
    for (wchar_t d = L'1'; d <= L'9'; ++d) {
      const std::wstring c{a, d};
      if (!TipConflicts(c, used)) {
        used.push_back(c);
        return c;
      }
    }
  return {};
}

}  // namespace

Ribbon::Ribbon() { group_.wrap = false; }

int Ribbon::HeightFor(bool collapsed, float ui) {
  return static_cast<int>(std::lround((kTabsH + (collapsed ? 0 : kBodyH)) * ui)) + 1;
}

void Ribbon::SetCollapsed(bool collapsed) {
  collapsed_ = collapsed;
  overlay_ = false;
  popupGroup_ = gridGroup_ = -1;
}

const RibbonTab* Ribbon::Tab() const {
  return tab_ >= 0 && tab_ < static_cast<int>(model_.tabs.size()) ? &model_.tabs[tab_] : nullptr;
}

RibbonItem* Ribbon::ItemAt(int g, int i) {
  if (tab_ < 0 || tab_ >= static_cast<int>(model_.tabs.size())) return nullptr;
  auto& groups = model_.tabs[tab_].groups;
  if (g < 0 || g >= static_cast<int>(groups.size())) return nullptr;
  if (i < 0 || i >= static_cast<int>(groups[g].items.size())) return nullptr;
  return &groups[g].items[i];
}

const RibbonItem* Ribbon::ItemAt(int g, int i) const { return const_cast<Ribbon*>(this)->ItemAt(g, i); }

CommandState Ribbon::Query(int cmd) const { return query && cmd ? query(cmd) : CommandState{}; }

std::wstring Ribbon::LabelOf(const RibbonItem& it) const {
  if (!it.label.empty()) return it.label;
  std::wstring s = Query(it.tool.cmd).label;
  if (s.size() > 1 && s.back() == L'…') s.pop_back();  // "Save as…" on a button: "Save as"
  if (s.empty()) s = uia::SplitShortcut(it.tool.tooltip).first;
  return s;
}

float Ribbon::TextW(const std::wstring& s, float dip, bool semibold) const {
  if (s.empty()) return 0;
  if (widthsUi_ != ui_) {
    widths_.clear();
    widthsUi_ = ui_;
  }
  const std::wstring key = s + (semibold ? L"\x1b" : L"\x1a") + std::to_wstring(static_cast<int>(dip * 10));
  auto [it, added] = widths_.try_emplace(key, 0.f);
  if (added) {
    IDWriteTextFormat* fmt =
        Canvas::Format(dip * ui_, semibold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_REGULAR, false);
    ComPtr<IDWriteTextLayout> layout;
    DWrite()->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt, 10000, 100, &layout);
    DWRITE_TEXT_METRICS tm{};
    if (layout) layout->GetMetrics(&tm);
    it->second = std::ceil(tm.widthIncludingTrailingWhitespace) / ui_;
  }
  return it->second;
}

// ---------------------------------------------------------------------------
// Sizes

RibbonSize Ribbon::SizeAt(const RibbonItem& it, int stage) const {
  RibbonSize s = it.size;
  if (stage >= 1 && s == RibbonSize::Large && it.smallest != RibbonSize::Large) s = RibbonSize::Medium;
  if (stage >= 2 && s == RibbonSize::Medium && it.smallest == RibbonSize::Small && it.tool.glyph) s = RibbonSize::Small;
  return s;
}

int Ribbon::GalleryColumns(const RibbonItem& it, int stage) const {
  if (!it.gallery) return 0;
  const RibbonGallery& g = *it.gallery;
  const int full = std::max(1, g.columns), least = std::clamp(g.minColumns, 1, full);
  switch (stage) {
    case 0: return full;
    case 1: return std::max(least, (full + least + 1) / 2);
    case 2: return least;
    default: return 0;  // a large button that opens the grid
  }
}

float Ribbon::ItemWidth(const RibbonItem& it, RibbonSize size, int galleryColumns) const {
  const float u = ui_;
  if (galleryColumns > 0)
    return std::round((galleryColumns * it.gallery->choiceWidth + 2 + kGalleryScrollW) * u);
  if (!OwnDrawn(it, size) && !it.gallery) return std::round(std::max(it.tool.width, 20.f) * u);
  const bool arrow = IsMenuButton(it) || IsSplit(it, size) || it.gallery.has_value();
  const std::wstring label = LabelOf(it);
  switch (size) {
    case RibbonSize::Large: {
      auto [a, b] = SplitLabel(label, [&](const std::wstring& s) { return TextW(s, kItemFont); });
      const float w = std::max(TextW(a, kItemFont), TextW(b, kItemFont) + (arrow && !b.empty() ? 10 : 0));
      return std::round(std::max(kLargeMinW, w + 12) * u);
    }
    case RibbonSize::Medium:
      return std::round((6 + (it.tool.glyph ? kGlyph + 5 : 0) + TextW(label, kItemFont) + 7 + (arrow ? kArrowW : 0)) * u);
    case RibbonSize::Small: break;
  }
  return std::round((kSmallW + (arrow ? kArrowW - 2 : 0)) * u);
}

float Ribbon::GroupButtonWidth(int g) const {
  const RibbonGroup& grp = Tab()->groups[g];
  auto [a, b] = SplitLabel(grp.name, [&](const std::wstring& s) { return TextW(s, kItemFont); });
  return std::round(std::max(kLargeMinW + 6, std::max(TextW(a, kItemFont), TextW(b, kItemFont) + 10) + 16) * ui_);
}

// ---------------------------------------------------------------------------
// Layout

float Ribbon::FlowGroup(int g, int stage, float x, float y, bool place, Layer layer) {
  const RibbonGroup& grp = Tab()->groups[g];
  const float u = ui_;
  const float areaTop = y + kContentTop * u, areaH = kAreaH * u, rowH = kRowH * u;
  const float bodyBottom = y + kBodyH * u;
  if (stage >= 4) {
    const float w = GroupButtonWidth(g);
    if (place) {
      Part p{PartKind::GroupButton, RectF(x + 2 * u, areaTop, x + w - 2 * u, bodyBottom - 3 * u), g};
      p.size = RibbonSize::Large;
      p.layer = layer;
      parts_.push_back(p);
    }
    return w;
  }

  struct Cell {
    int item;
    RibbonSize size;
    float w;
  };
  struct Row {
    std::vector<Cell> cells;
    float w = 0;
  };
  struct Col {
    int item = -1;  // a large item or a gallery
    RibbonSize size = RibbonSize::Large;
    int galleryColumns = 0;
    float w = 0;
    std::vector<Row> rows;
  };
  std::vector<Col> cols;
  int cur = -1;
  const int rows = std::clamp(grp.rows, 1, 3);
  for (int i = 0; i < static_cast<int>(grp.items.size()); ++i) {
    const RibbonItem& it = grp.items[i];
    RibbonSize size = SizeAt(it, stage);
    const int gcols = GalleryColumns(it, stage);
    const bool largeCapable = OwnDrawn(it, RibbonSize::Large) || it.gallery.has_value();
    if (it.gallery && gcols == 0) size = RibbonSize::Large;  // a gallery folded into its button
    if (gcols > 0 || (size == RibbonSize::Large && largeCapable)) {
      Col c;
      c.item = i;
      c.size = RibbonSize::Large;
      c.galleryColumns = gcols;
      c.w = ItemWidth(it, RibbonSize::Large, gcols);
      cols.push_back(std::move(c));
      cur = -1;
      continue;
    }
    if (size == RibbonSize::Large) size = RibbonSize::Small;  // boxes and pickers are a row high
    const float w = ItemWidth(it, size, 0);
    const float gap = it.tool.sepBefore ? 6 * u : 1 * u;
    if (it.sameRow && cur >= 0 && !cols[cur].rows.empty()) {
      Row& r = cols[cur].rows.back();
      r.w += gap + w;
      r.cells.push_back({i, size, w});
    } else {
      if (it.newColumn || cur < 0 || static_cast<int>(cols[cur].rows.size()) >= rows) {
        cols.emplace_back();
        cur = static_cast<int>(cols.size()) - 1;
      }
      Row r;
      r.cells.push_back({i, size, w});
      r.w = w;
      cols[cur].rows.push_back(std::move(r));
    }
    Col& c = cols[cur];
    c.w = 0;
    for (const Row& r : c.rows) c.w = std::max(c.w, r.w);
  }
  float contentW = 0;
  for (const Col& c : cols) contentW += c.w;
  if (!cols.empty()) contentW += (cols.size() - 1) * kColGap * u;
  const float nameW = std::round((TextW(grp.name, kNameFont) + 2 * kPad + (grp.launcher ? 16 : 0)) * u);
  const float w = std::max(contentW + 2 * kPad * u, nameW);
  if (!place) return w;

  auto rowTop = [&](int r) { return areaTop + std::round(areaH * (2 * r + 1) / (2.0f * rows) - rowH / 2); };
  float cx = std::round(x + (w - contentW) / 2);
  for (const Col& c : cols) {
    if (c.item >= 0) {
      RibbonItem& it = model_.tabs[tab_].groups[g].items[c.item];
      const D2D1_RECT_F r = RectF(cx, areaTop, cx + c.w, areaTop + areaH);
      if (c.galleryColumns > 0) {
        // The strip: one row of choices, then up, down and the grid's ▾ in a column.
        const RibbonGallery& gal = *it.gallery;
        const int n = static_cast<int>(it.tool.choices.size());
        int& first = galleryFirst_[it.tool.cmd];
        if (first >= n) first = 0;
        first -= first % c.galleryColumns;
        const float tw = gal.choiceWidth * u, th = std::min(gal.choiceHeight * u, areaH - 4 * u);
        const float ty = std::round(areaTop + (areaH - th) / 2);
        for (int k = 0; k < c.galleryColumns && first + k < n; ++k) {
          Part p{PartKind::GalleryChoice, RectF(cx + 1 * u + k * tw, ty, cx + 1 * u + (k + 1) * tw, ty + th), g, c.item,
                 first + k};
          p.layer = layer;
          parts_.push_back(p);
        }
        const float sx = r.right - kGalleryScrollW * u, third = std::round((ty + th - ty) / 3);
        const PartKind kinds[3] = {PartKind::GalleryUp, PartKind::GalleryDown, PartKind::GalleryMore};
        for (int k = 0; k < 3; ++k) {
          Part p{kinds[k], RectF(sx, ty + k * third, r.right, k == 2 ? ty + th : ty + (k + 1) * third), g, c.item};
          p.layer = layer;
          parts_.push_back(p);
        }
      } else if (IsSplit(it, RibbonSize::Large)) {
        Part a{PartKind::Item, RectF(r.left, r.top, r.right, r.top + kLargeIconH * u), g, c.item};
        Part b{PartKind::ItemArrow, RectF(r.left, r.top + kLargeIconH * u, r.right, r.bottom), g, c.item};
        a.size = b.size = RibbonSize::Large;
        a.layer = b.layer = layer;
        parts_.push_back(a);
        parts_.push_back(b);
      } else {
        Part p{PartKind::Item, r, g, c.item};
        p.size = RibbonSize::Large;
        p.layer = layer;
        parts_.push_back(p);
      }
    } else {
      for (int ri = 0; ri < static_cast<int>(c.rows.size()); ++ri) {
        const float top = rowTop(ri);
        float rx = cx;
        bool firstCell = true;
        for (const Cell& cell : c.rows[ri].cells) {
          RibbonItem& it = model_.tabs[tab_].groups[g].items[cell.item];
          if (!firstCell) rx += it.tool.sepBefore ? 6 * u : 1 * u;
          firstCell = false;
          const D2D1_RECT_F r = RectF(rx, top, rx + cell.w, top + rowH);
          if (!OwnDrawn(it, cell.size)) {
            if (it.tool.tooltip.empty()) it.tool.tooltip = Query(it.tool.cmd).Tooltip();
            Control& ctl = Toolbar::ConfigureControl(controls_[it.tool.cmd], kinds_[it.tool.cmd], it.tool, [this](int cmd) {
              if (onCommand) onCommand(cmd);
            });
            ctl.rc = r;
            ctl.visible = true;
            Part p{PartKind::Control, r, g, cell.item};
            p.size = cell.size;
            p.layer = layer;
            p.control = &ctl;
            parts_.push_back(p);
          } else if (IsSplit(it, cell.size)) {
            Part a{PartKind::Item, RectF(r.left, r.top, r.right - kArrowW * u, r.bottom), g, cell.item};
            Part b{PartKind::ItemArrow, RectF(r.right - kArrowW * u, r.top, r.right, r.bottom), g, cell.item};
            a.size = b.size = cell.size;
            a.layer = b.layer = layer;
            parts_.push_back(a);
            parts_.push_back(b);
          } else {
            Part p{PartKind::Item, r, g, cell.item};
            p.size = cell.size;
            p.layer = layer;
            parts_.push_back(p);
          }
          rx += cell.w;
        }
      }
    }
    cx += c.w + kColGap * u;
  }
  if (grp.launcher) {
    Part p{PartKind::Launcher, RectF(x + w - 17 * u, bodyBottom - 18 * u, x + w - 3 * u, bodyBottom - 3 * u), g, -2};
    p.layer = layer;
    parts_.push_back(p);
  }
  return w;
}

void Ribbon::PlaceTabs() {
  const float u = ui_;
  const float top = static_cast<float>(rc_.top), bottom = rc_.top + kTabsH * u;
  float x = rc_.left + 6 * u;
  auto add = [&](PartKind kind, int index, const std::wstring& name) {
    const float w = std::round((TextW(name, kTabFont, true) + 22) * u);
    Part p{kind, RectF(x, top + 2 * u, x + w, bottom), -1, -1, index};
    parts_.push_back(p);
    x += w + 2 * u;
  };
  add(PartKind::File, -1, L"File");
  bool contextual = false;
  for (int t = 0; t < static_cast<int>(model_.tabs.size()); ++t) {
    if (model_.tabs[t].contextual && !contextual) {
      x += 8 * u;  // a little apart from the others
      contextual = true;
    }
    add(PartKind::Tab, t, model_.tabs[t].name);
  }
}

void Ribbon::PlaceBody(float top, Layer layer) {
  const RibbonTab* tab = Tab();
  if (!tab) return;
  const float u = ui_;
  float x = rc_.left + 4 * u;
  groupRc_.assign(tab->groups.size(), D2D1_RECT_F{});
  for (int g = 0; g < static_cast<int>(tab->groups.size()); ++g) {
    const float w = FlowGroup(g, stage_[g], x, top, true, layer);
    groupRc_[g] = RectF(x, top, x + w, top + kBodyH * u);
    x += w + kSepW * u;
  }
  const float bottom = top + kBodyH * u;
  Part p{PartKind::Collapse, RectF(rc_.right - (kCollapseW + 4) * u, bottom - 22 * u, rc_.right - 4 * u, bottom - 3 * u)};
  p.layer = layer;
  parts_.push_back(p);
}

void Ribbon::PlaceGroupPopup() {
  if (popupGroup_ < 0) return;
  const RibbonTab* tab = Tab();
  if (!tab || popupGroup_ >= static_cast<int>(tab->groups.size()) || stage_[popupGroup_] < 4) {
    popupGroup_ = -1;
    return;
  }
  D2D1_RECT_F button{};
  for (const Part& p : parts_)
    if (p.kind == PartKind::GroupButton && p.group == popupGroup_) button = p.rc;
  const float u = ui_;
  const float w = FlowGroup(popupGroup_, 0, 0, 0, false, Layer::Popup);
  RECT client = rc_;
  if (group_.hwnd) GetClientRect(group_.hwnd, &client);
  const float left = std::clamp(button.left, client.left + 4 * u, std::max(client.left + 4 * u, client.right - w - 4 * u));
  const float top = (overlay_ ? overlayRc_.bottom : static_cast<float>(rc_.bottom)) + 2 * u;
  popupRc_ = RectF(left, top, left + w, top + kBodyH * u);
  FlowGroup(popupGroup_, 0, left, top, true, Layer::Popup);
}

void Ribbon::PlaceGrid() {
  if (gridGroup_ < 0) return;
  const RibbonItem* it = ItemAt(gridGroup_, gridItem_);
  if (!it || !it->gallery) {
    gridGroup_ = -1;
    return;
  }
  // Under the strip (or the button) it opened from.
  D2D1_RECT_F anchor{};
  bool found = false;
  for (const Part& p : parts_)
    if (p.group == gridGroup_ && p.item == gridItem_ && p.kind != PartKind::GridChoice && p.kind != PartKind::GridMenu) {
      anchor = found ? RectF(std::min(anchor.left, p.rc.left), std::min(anchor.top, p.rc.top),
                             std::max(anchor.right, p.rc.right), std::max(anchor.bottom, p.rc.bottom))
                     : p.rc;
      found = true;
    }
  if (!found) {
    gridGroup_ = -1;
    return;
  }
  const RibbonGallery& gal = *it->gallery;
  const float u = ui_;
  const int n = static_cast<int>(it->tool.choices.size());
  gridCols_ = std::max(1, gal.popupColumns > 0 ? gal.popupColumns : gal.columns);
  const int rows = (n + gridCols_ - 1) / gridCols_;
  const float tw = gal.choiceWidth * u, th = gal.choiceHeight * u;
  const float menuH = gal.menu.empty() ? 0 : (gal.menu.size() * kGridMenuH + 9) * u;
  RECT client = rc_;
  if (group_.hwnd) GetClientRect(group_.hwnd, &client);
  const bool strip = std::any_of(parts_.begin(), parts_.end(), [&](const Part& p) {
    return p.kind == PartKind::GalleryChoice && p.group == gridGroup_ && p.item == gridItem_;
  });
  const float top = strip ? anchor.top : anchor.bottom + 2 * u;
  const float room = client.bottom - top - 8 * u - menuH - 8 * u;
  gridRows_ = std::clamp(static_cast<int>(room / th), 1, std::max(1, rows));
  gridScroll_ = std::clamp(gridScroll_, 0, std::max(0, rows - gridRows_));
  const float w = gridCols_ * tw + 8 * u, h = gridRows_ * th + 8 * u + menuH;
  const float left = std::clamp(anchor.left, client.left + 4 * u, std::max(client.left + 4 * u, client.right - w - 4 * u));
  gridRc_ = RectF(left, top, left + w, top + h);
  for (int r = 0; r < gridRows_; ++r)
    for (int k = 0; k < gridCols_; ++k) {
      const int index = (gridScroll_ + r) * gridCols_ + k;
      if (index >= n) break;
      const float x = left + 4 * u + k * tw, y = top + 4 * u + r * th;
      Part p{PartKind::GridChoice, RectF(x, y, x + tw, y + th), gridGroup_, gridItem_, index};
      p.layer = Layer::Popup;
      parts_.push_back(p);
    }
  float y = top + 4 * u + gridRows_ * th + 5 * u;
  for (int m = 0; m < static_cast<int>(gal.menu.size()); ++m) {
    if (gal.menu[m]) {
      Part p{PartKind::GridMenu, RectF(left + 4 * u, y, left + w - 4 * u, y + kGridMenuH * u), gridGroup_, gridItem_, m};
      p.layer = Layer::Popup;
      parts_.push_back(p);
    }
    y += kGridMenuH * u;
  }
}

void Ribbon::Select(const std::wstring& name) {
  if (name == selected_) return;
  selected_ = name;
  popupGroup_ = gridGroup_ = -1;
  group_.CloseAll();
  for (const RibbonTab& t : model_.tabs)
    if (t.name == name && !t.contextual) lastNormal_ = name;
}

void Ribbon::Layout(const RECT& rc, float ui, RibbonModel m) {
  LARGE_INTEGER t0, t1, freq;
  QueryPerformanceCounter(&t0);
  // Where the pointer and the mouse button were, to find them again after placing.
  auto keyOf = [&](int i) {
    return i >= 0 && i < static_cast<int>(parts_.size())
               ? std::array<int, 5>{static_cast<int>(parts_[i].kind), parts_[i].group, parts_[i].item, parts_[i].index,
                                    static_cast<int>(parts_[i].layer)}
               : std::array<int, 5>{-1, -1, -1, -1, -1};
  };
  const auto hotKey = keyOf(hot_), pressedKey = keyOf(pressed_);
  model_ = std::move(m);
  rc_ = rc;
  ui_ = ui;

  if (!model_.show.empty() && model_.show != lastShow_) Select(model_.show);
  lastShow_ = model_.show;
  auto find = [&](const std::wstring& name) {
    for (int t = 0; t < static_cast<int>(model_.tabs.size()); ++t)
      if (model_.tabs[t].name == name) return t;
    return -1;
  };
  const int before = tab_;
  tab_ = find(selected_);
  if (tab_ < 0) tab_ = find(lastNormal_);
  if (tab_ < 0 && !model_.tabs.empty()) tab_ = 0;
  if (tab_ >= 0) {
    if (model_.tabs[tab_].name != selected_) popupGroup_ = gridGroup_ = -1;
    selected_ = model_.tabs[tab_].name;
    if (!model_.tabs[tab_].contextual) lastNormal_ = selected_;
  }
  if (tab_ != before) popupGroup_ = gridGroup_ = -1;

  parts_.clear();
  for (auto& [cmd, c] : controls_) c->visible = false;
  if (!model_.tabs.empty()) {
    PlaceTabs();
    // Which step each group is at: shrink the groups a step at a time, those that go first first,
    // and from the right, until they fit.
    const RibbonTab& tab = model_.tabs[tab_];
    const int n = static_cast<int>(tab.groups.size());
    stage_.assign(n, 0);
    const float u = ui_;
    const float avail = (rc.right - rc.left) - (8 + kCollapseW + 6) * u;
    std::vector<std::array<float, 5>> w(n);
    float total = 0;
    for (int g = 0; g < n; ++g) {
      for (int s = 0; s < 5; ++s) w[g][s] = FlowGroup(g, s, 0, 0, false, Layer::Body);
      total += w[g][0] + kSepW * u;
    }
    struct Step {
      int round, g, s;
    };
    std::vector<Step> steps;
    for (int g = 0; g < n; ++g)
      for (int s = 1; s <= 4; ++s) steps.push_back({s - std::clamp(tab.groups[g].shrinkFirst, -3, 3), g, s});
    std::stable_sort(steps.begin(), steps.end(), [](const Step& a, const Step& b) {
      return a.round != b.round ? a.round < b.round : a.g != b.g ? a.g > b.g : a.s < b.s;
    });
    for (const Step& st : steps) {
      if (total <= avail) break;
      if (stage_[st.g] < st.s && w[st.g][st.s] < w[st.g][stage_[st.g]] - 0.5f) {
        total -= w[st.g][stage_[st.g]] - w[st.g][st.s];
        stage_[st.g] = st.s;
      }
    }
    const float bodyTop = rc.top + kTabsH * u;
    if (!collapsed_) {
      PlaceBody(bodyTop, Layer::Body);
    } else if (overlay_) {
      overlayRc_ = RectF(static_cast<float>(rc.left), bodyTop, static_cast<float>(rc.right), bodyTop + kBodyH * u);
      PlaceBody(bodyTop, Layer::Overlay);
    } else {
      groupRc_.clear();
    }
    PlaceGroupPopup();
    PlaceGrid();
  }

  shown_.clear();
  for (const Part& p : parts_)
    if (p.control) shown_.push_back(p.control);
  group_.ui = ui;
  RECT client = rc;
  if (group_.hwnd) GetClientRect(group_.hwnd, &client);
  group_.bounds = RectF(client);
  group_.Assign(shown_);
  AssignKeyTips();

  auto findKey = [&](const std::array<int, 5>& k) {
    for (int i = 0; i < static_cast<int>(parts_.size()); ++i)
      if (keyOf(i) == k) return i;
    return -1;
  };
  hot_ = hotKey[0] < 0 ? -1 : findKey(hotKey);
  pressed_ = pressedKey[0] < 0 ? -1 : findKey(pressedKey);
  QueryPerformanceCounter(&t1);
  QueryPerformanceFrequency(&freq);
  layoutMs_ = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart);
}

void Ribbon::AssignKeyTips() {
  tabTips_.clear();
  itemTips_.clear();
  partTips_.clear();
  // The tabs: File, the window's own (quick access) and each tab's letter.
  std::vector<std::wstring> used{L"F"};
  for (const ExtraTip& e : extraTips) used.push_back(e.keys);
  for (const RibbonTab& t : model_.tabs)
    if (!t.keyTip.empty()) used.push_back(t.keyTip);
  for (const RibbonTab& t : model_.tabs) tabTips_.push_back(t.keyTip.empty() ? AutoTip(t.name, used, true) : t.keyTip);
  // The selected tab's items, groups and launchers, in one set: a folded group keeps its items' letters.
  const RibbonTab* tab = Tab();
  if (!tab) return;
  std::vector<std::wstring> items;
  for (const RibbonGroup& g : tab->groups) {
    if (!g.keyTip.empty()) items.push_back(g.keyTip);
    for (const RibbonItem& it : g.items)
      if (!it.keyTip.empty()) items.push_back(it.keyTip);
  }
  for (int g = 0; g < static_cast<int>(tab->groups.size()); ++g) {
    const RibbonGroup& grp = tab->groups[g];
    for (int i = 0; i < static_cast<int>(grp.items.size()); ++i) {
      const RibbonItem& it = grp.items[i];
      itemTips_[{g, i}] = it.keyTip.empty() ? AutoTip(LabelOf(it), items, false) : it.keyTip;
    }
  }
  for (int g = 0; g < static_cast<int>(tab->groups.size()); ++g) {
    const RibbonGroup& grp = tab->groups[g];
    if (grp.launcher) itemTips_[{g, -2}] = AutoTip(grp.name, items, false, grp.name.empty() ? L'Q' : towupper(grp.name[0]));
    itemTips_[{g, -1}] = grp.keyTip.empty() ? AutoTip(grp.name, items, false, L'Z') : grp.keyTip;
  }
  for (size_t i = 0; i < parts_.size(); ++i) {
    const std::wstring t = TipOf(parts_[i]);
    if (!t.empty()) partTips_[i] = t;
  }
}

std::wstring Ribbon::TipOf(const Part& p) const {
  switch (p.kind) {
    case PartKind::File: return L"F";
    case PartKind::Tab: return p.index < static_cast<int>(tabTips_.size()) ? tabTips_[p.index] : L"";
    case PartKind::Item:
    case PartKind::Control:
    case PartKind::GalleryMore:
      if (auto it = itemTips_.find({p.group, p.item}); it != itemTips_.end()) return it->second;
      return {};
    case PartKind::Launcher:
      if (auto it = itemTips_.find({p.group, -2}); it != itemTips_.end()) return it->second;
      return {};
    case PartKind::GroupButton:
      if (auto it = itemTips_.find({p.group, -1}); it != itemTips_.end()) return it->second;
      return {};
    default: return {};
  }
}

// ---------------------------------------------------------------------------
// Drawing

void Ribbon::DrawBodyFrame(Canvas& c, float top, Layer layer) {
  const Palette& p = theme::P();
  const float u = ui_;
  const RibbonTab* tab = Tab();
  if (!tab) return;
  for (int g = 0; g < static_cast<int>(groupRc_.size()); ++g) {
    const D2D1_RECT_F& r = groupRc_[g];
    if (stage_[g] < 4) {
      const float nameRight = r.right - (tab->groups[g].launcher ? 16 * u : 0);
      c.Text(tab->groups[g].name, RectF(r.left + 2 * u, top + (kContentTop + kAreaH) * u, nameRight - 2 * u, top + (kBodyH - 2) * u),
             p.textSecondary, kNameFont * u, DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
    }
    const float sx = std::round(r.right + kSepW * u / 2) - 0.5f;
    c.Line(sx, top + 6 * u, sx, top + (kBodyH - 6) * u, Alpha(p.textSecondary, 0.25f), 1);
  }
  (void)layer;
}

void Ribbon::Draw(Canvas& c) {
  if (model_.tabs.empty()) return;
  const Palette& p = theme::P();
  const D2D1_RECT_F r = RectF(rc_);
  c.Fill(r, p.chrome);
  c.Fill(RectF(r.left, r.bottom - 1, r.right, r.bottom), p.chromeBorder);
  if (!collapsed_) DrawBodyFrame(c, rc_.top + kTabsH * ui_, Layer::Body);
  for (size_t i = 0; i < parts_.size(); ++i)
    if (parts_[i].layer == Layer::Body) DrawPart(c, parts_[i], i);
}

void Ribbon::DrawPopup(Canvas& c) {
  if (model_.tabs.empty()) return;
  const Palette& p = theme::P();
  const float u = ui_;
  auto frame = [&](const D2D1_RECT_F& r) {
    c.SoftShadow(r, 6 * u, 12 * u, p.popupShadow, 3 * u);
    c.FillRound(r, 6 * u, p.chrome);
    c.StrokeRound(r, 6 * u, p.popupBorder, 1);
  };
  if (collapsed_ && overlay_) {
    frame(overlayRc_);
    DrawBodyFrame(c, overlayRc_.top, Layer::Overlay);
    for (size_t i = 0; i < parts_.size(); ++i)
      if (parts_[i].layer == Layer::Overlay) DrawPart(c, parts_[i], i);
  }
  if (popupGroup_ >= 0) {
    frame(popupRc_);
    const RibbonGroup& grp = Tab()->groups[popupGroup_];
    c.Text(grp.name, RectF(popupRc_.left, popupRc_.top + (kContentTop + kAreaH) * u, popupRc_.right - (grp.launcher ? 16 * u : 0),
                           popupRc_.top + (kBodyH - 2) * u),
           p.textSecondary, kNameFont * u, DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
    for (size_t i = 0; i < parts_.size(); ++i)
      if (parts_[i].layer == Layer::Popup && parts_[i].kind != PartKind::GridChoice && parts_[i].kind != PartKind::GridMenu)
        DrawPart(c, parts_[i], i);
  }
  if (gridGroup_ >= 0) DrawGrid(c);
  group_.DrawPopup(c);
  if (tips_ != Tips::None) DrawKeyTips(c);
}

void Ribbon::DrawGrid(Canvas& c) {
  const Palette& p = theme::P();
  const float u = ui_;
  c.SoftShadow(gridRc_, 6 * u, 12 * u, p.popupShadow, 3 * u);
  c.FillRound(gridRc_, 6 * u, p.popupBg);
  c.StrokeRound(gridRc_, 6 * u, p.popupBorder, 1);
  bool menuLine = false;
  for (size_t i = 0; i < parts_.size(); ++i) {
    if (parts_[i].kind == PartKind::GridMenu && !menuLine) {
      menuLine = true;
      const float y = std::round(parts_[i].rc.top - 3 * u) + 0.5f;
      c.Line(gridRc_.left + 1, y, gridRc_.right - 1, y, p.popupBorder, 1);
    }
    if (parts_[i].kind == PartKind::GridChoice || parts_[i].kind == PartKind::GridMenu) DrawPart(c, parts_[i], i);
  }
}

namespace {
// The ↘ of a dialog launcher: a corner and an arrow into it.
void DrawLauncher(Canvas& c, const D2D1_RECT_F& r, D2D1_COLOR_F color, float u) {
  const float s = std::round(9 * u), l = std::round((r.left + r.right - s) / 2), t = std::round((r.top + r.bottom - s) / 2);
  const float w = std::max(1.0f, std::round(u));
  c.Line(l + 0.5f, t, l + 0.5f, t + s * 0.45f, color, w);
  c.Line(l, t + 0.5f, l + s * 0.45f, t + 0.5f, color, w);
  c.Line(l + s * 0.25f, t + s * 0.25f, l + s, t + s, color, w);
  c.Line(l + s * 0.45f, t + s - 0.5f, l + s, t + s - 0.5f, color, w);
  c.Line(l + s - 0.5f, t + s * 0.45f, l + s - 0.5f, t + s, color, w);
}
}  // namespace

void Ribbon::DrawPart(Canvas& c, const Part& part, size_t index) {
  const Palette& p = theme::P();
  const float u = ui_;
  const bool hot = static_cast<int>(index) == hot_, press = static_cast<int>(index) == pressed_ && hot;
  auto subtle = [&](const D2D1_RECT_F& r, bool enabled, bool checked) {
    if (checked) c.FillRound(r, 3 * u, Alpha(p.accent, p.dark ? 0.22f : 0.14f));
    if (enabled && hot) c.FillRound(r, 3 * u, press ? p.pressed : p.hover);
  };
  switch (part.kind) {
    case PartKind::File:
    case PartKind::Tab: {
      const bool file = part.kind == PartKind::File;
      const RibbonTab* t = file ? nullptr : &model_.tabs[part.index];
      const bool selected = !file && part.index == tab_ && (!collapsed_ || overlay_);
      const D2D1_RECT_F hr = RectF(part.rc.left, part.rc.top + 2 * u, part.rc.right, part.rc.bottom - 3 * u);
      if (hot) c.FillRound(hr, 4 * u, press ? p.pressed : p.hover);
      const std::wstring& name = file ? std::wstring(L"File") : t->name;
      const D2D1_COLOR_F fg = t && t->contextual ? p.accentText : p.text;
      c.Text(name, RectF(part.rc.left, part.rc.top, part.rc.right, part.rc.bottom - 2 * u), fg, kTabFont * u,
             selected ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
      if (selected) {
        const float tw = TextW(name, kTabFont, true) * u, cx = (part.rc.left + part.rc.right) / 2;
        c.FillRound(RectF(std::round(cx - tw / 2), part.rc.bottom - 4 * u, std::round(cx + tw / 2), part.rc.bottom - 1 * u),
                    1.5f * u, p.accent);
      }
      break;
    }
    case PartKind::Collapse: {
      if (hot) c.FillRound(part.rc, 3 * u, press ? p.pressed : p.hover);
      c.Icon(collapsed_ ? 0xE718 /* pin */ : icon::ChevronUp, part.rc, p.textSecondary, (collapsed_ ? 12 : 10) * u);
      break;
    }
    case PartKind::Launcher: {
      if (hot) c.FillRound(part.rc, 2 * u, press ? p.pressed : p.hover);
      DrawLauncher(c, part.rc, p.textSecondary, u);
      break;
    }
    case PartKind::Control:
      if (part.control) part.control->Draw(c);
      break;
    case PartKind::GroupButton: {
      const RibbonGroup& grp = Tab()->groups[part.group];
      subtle(part.rc, true, popupGroup_ == part.group);
      wchar_t glyph = grp.glyph;
      if (!glyph)
        for (const RibbonItem& it : grp.items)
          if (it.tool.glyph) {
            glyph = it.tool.glyph;
            break;
          }
      DrawGlyph(c, glyph, RectF(part.rc.left, part.rc.top + 4 * u, part.rc.right, part.rc.top + kLargeIconH * u), p.text,
                kLargeGlyph * u);
      auto [a, b] = SplitLabel(grp.name, [&](const std::wstring& s) { return TextW(s, kItemFont); });
      const float ly = part.rc.top + kLargeIconH * u + 1 * u;
      c.Text(a, RectF(part.rc.left, ly, part.rc.right, ly + 15 * u), p.text, kItemFont * u, DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
      const float bw = TextW(b, kItemFont) * u, cx = (part.rc.left + part.rc.right) / 2;
      const float bl = std::round(cx - (bw + (b.empty() ? 0 : 3 * u) + 8 * u) / 2);
      if (!b.empty()) c.Text(b, RectF(bl, ly + 15 * u, bl + bw + 2, ly + 30 * u), p.text, kItemFont * u);
      c.Icon(icon::ChevronDown, RectF(bl + bw + (b.empty() ? 0 : 3 * u), ly + 15 * u, bl + bw + (b.empty() ? 0 : 3 * u) + 8 * u, ly + 30 * u),
             p.textSecondary, 8 * u);
      break;
    }
    case PartKind::GalleryChoice:
    case PartKind::GridChoice: {
      const RibbonItem* it = ItemAt(part.group, part.item);
      if (!it) break;
      const bool chosen = part.index == it->tool.choice;
      const D2D1_RECT_F r = Inflate(part.rc, -1 * u, -1 * u);
      if (hot || (part.kind == PartKind::GridChoice && part.index == gridHot_))
        c.FillRound(r, 3 * u, press ? p.pressed : p.hover);
      if (it->gallery->drawChoice) {
        c.PushClip(r);
        it->gallery->drawChoice(c, Inflate(r, -2 * u, -2 * u), part.index, u);
        c.PopClip();
      } else if (part.index < static_cast<int>(it->tool.choices.size())) {
        c.Text(it->tool.choices[part.index], Inflate(r, -4 * u, 0), it->tool.enabled ? p.text : p.textDisabled, kItemFont * u,
               DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
      }
      if (chosen) c.StrokeRound(Inflate(r, -0.5f, -0.5f), 3 * u, p.accent, std::max(1.0f, std::round(1.5f * u)));
      break;
    }
    case PartKind::GalleryUp:
    case PartKind::GalleryDown:
    case PartKind::GalleryMore: {
      const RibbonItem* it = ItemAt(part.group, part.item);
      if (!it) break;
      const int n = static_cast<int>(it->tool.choices.size());
      const int first = galleryFirst_.count(it->tool.cmd) ? galleryFirst_.at(it->tool.cmd) : 0;
      int shown = 0;
      for (const Part& q : parts_)
        if (q.kind == PartKind::GalleryChoice && q.group == part.group && q.item == part.item) ++shown;
      const bool enabled = part.kind == PartKind::GalleryMore || (part.kind == PartKind::GalleryUp ? first > 0 : first + shown < n);
      subtle(part.rc, enabled, false);
      const D2D1_COLOR_F fg = enabled ? p.textSecondary : p.textDisabled;
      if (part.kind == PartKind::GalleryMore) {
        const float cx = (part.rc.left + part.rc.right) / 2, ty = part.rc.top + gfx::Height(part.rc) * 0.38f;
        c.Line(cx - 3.5f * u, std::round(ty) + 0.5f, cx + 3.5f * u, std::round(ty) + 0.5f, fg, 1);
        c.Icon(icon::ChevronDown, RectF(part.rc.left, part.rc.top + 3 * u, part.rc.right, part.rc.bottom), fg, 8 * u);
      } else {
        c.Icon(part.kind == PartKind::GalleryUp ? icon::ChevronUp : icon::ChevronDown, part.rc, fg, 8 * u);
      }
      // The strip's frame, once, with its first part.
      if (part.kind == PartKind::GalleryUp) {
        D2D1_RECT_F all = part.rc;
        for (const Part& q : parts_)
          if (q.group == part.group && q.item == part.item && q.layer == part.layer &&
              (q.kind == PartKind::GalleryChoice || q.kind == PartKind::GalleryMore || q.kind == PartKind::GalleryDown))
            all = RectF(std::min(all.left, q.rc.left), std::min(all.top, q.rc.top), std::max(all.right, q.rc.right),
                        std::max(all.bottom, q.rc.bottom));
        c.StrokeRound(Inflate(all, 0.5f, 0.5f), 3 * u, p.inputBorder, 1);
        c.Line(part.rc.left - 0.5f, all.top, part.rc.left - 0.5f, all.bottom, p.inputBorder, 1);
      }
      break;
    }
    case PartKind::GridMenu: {
      const RibbonItem* it = ItemAt(part.group, part.item);
      if (!it) break;
      const int cmd = it->gallery->menu[part.index];
      const CommandState s = Query(cmd);
      subtle(part.rc, s.enabled, false);
      if (s.checked) c.Icon(icon::CheckMark, RectF(part.rc.left + 6 * u, part.rc.top, part.rc.left + 24 * u, part.rc.bottom), p.text, 12 * u);
      c.Text(s.label, RectF(part.rc.left + 30 * u, part.rc.top, part.rc.right - 8 * u, part.rc.bottom), s.enabled ? p.text : p.textDisabled,
             13 * u);
      break;
    }
    case PartKind::Item:
    case PartKind::ItemArrow: {
      const RibbonItem* itp = ItemAt(part.group, part.item);
      if (!itp) break;
      const RibbonItem& it = *itp;
      const bool enabled = it.tool.enabled;
      const bool split = IsSplit(it, part.size);
      const bool arrow = IsMenuButton(it) || split || it.gallery.has_value();
      const D2D1_COLOR_F fg = !enabled ? p.textDisabled : it.tool.checked ? p.accentText : p.text;
      // A split button's two parts: the hovered one filled, the other outlined.
      if (split && enabled && !hot) {
        for (size_t k = 0; k < parts_.size(); ++k)
          if (static_cast<int>(k) == hot_ && parts_[k].group == part.group && parts_[k].item == part.item &&
              parts_[k].layer == part.layer)
            c.StrokeRound(Inflate(part.rc, -0.5f, -0.5f), 3 * u, p.hover, 1);
      }
      subtle(part.rc, enabled, it.tool.checked && part.kind == PartKind::Item);
      const std::wstring label = LabelOf(it);
      const wchar_t glyph = it.tool.glyph;
      if (part.size == RibbonSize::Large) {
        const D2D1_RECT_F full = split ? RectF(part.rc.left, part.kind == PartKind::Item ? part.rc.top : part.rc.top - kLargeIconH * u,
                                                part.rc.right, part.kind == PartKind::Item ? part.rc.top + 72 * u : part.rc.bottom)
                                       : part.rc;
        if (part.kind == PartKind::Item)
          DrawGlyph(c, glyph, RectF(full.left, full.top + 4 * u, full.right, full.top + kLargeIconH * u), fg, kLargeGlyph * u);
        if (part.kind == PartKind::ItemArrow || !split) {
          auto [a, b] = SplitLabel(label, [&](const std::wstring& s) { return TextW(s, kItemFont); });
          const float ly = full.top + kLargeIconH * u + 1 * u;
          c.Text(a, RectF(full.left, ly, full.right, ly + 15 * u), fg, kItemFont * u, DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
          const float bw = TextW(b, kItemFont) * u, cx = (full.left + full.right) / 2;
          const float aw = arrow ? 8 * u + (b.empty() ? 0 : 3 * u) : 0;
          const float bl = std::round(cx - (bw + aw) / 2);
          if (!b.empty()) c.Text(b, RectF(bl, ly + 15 * u, bl + bw + 2, ly + 30 * u), fg, kItemFont * u);
          if (arrow) c.Icon(icon::ChevronDown, RectF(bl + bw + aw - 8 * u, ly + 15 * u, bl + bw + aw, ly + 30 * u), fg, 8 * u);
        }
        break;
      }
      if (part.kind == PartKind::ItemArrow) {
        c.Icon(icon::ChevronDown, part.rc, fg, 8 * u);
        break;
      }
      float x = part.rc.left + (part.size == RibbonSize::Small ? 4 * u : 6 * u);
      if (glyph) {
        DrawGlyph(c, glyph, RectF(x, part.rc.top, x + kGlyph * u, part.rc.bottom), fg, kGlyph * u);
        x += (kGlyph + 5) * u;
      }
      if (part.size == RibbonSize::Medium || !glyph)
        c.Text(label, RectF(x, part.rc.top, part.rc.right - (arrow && !split ? kArrowW * u : 0), part.rc.bottom - 1 * u), fg,
               kItemFont * u);
      if (arrow && !split)
        c.Icon(icon::ChevronDown, RectF(part.rc.right - kArrowW * u - 1 * u, part.rc.top, part.rc.right - 1 * u, part.rc.bottom), fg,
               8 * u);
      break;
    }
  }
}

void Ribbon::DrawKeyTips(Canvas& c) {
  const Palette& p = theme::P();
  const float u = ui_;
  auto badge = [&](const std::wstring& tip, float cx, float cy) {
    if (tip.compare(0, typed_.size(), typed_) != 0) return;
    const float w = std::max(16 * u, std::round((TextW(tip, 11, true) + 8) * u)), h = 17 * u;
    const D2D1_RECT_F r = RectF(std::round(cx - w / 2), std::round(cy - h / 2), std::round(cx - w / 2) + w, std::round(cy - h / 2) + h);
    c.FillRound(r, 3 * u, p.dark ? Rgb(0xE8E8E8) : Rgb(0x3A3A3A));
    c.Text(tip, r, p.dark ? Rgb(0x1A1A1A) : Rgb(0xFFFFFF), 11 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center);
  };
  if (tips_ == Tips::Tabs) {
    for (size_t i = 0; i < parts_.size(); ++i) {
      const Part& q = parts_[i];
      if (q.kind == PartKind::File || q.kind == PartKind::Tab)
        badge(partTips_.count(i) ? partTips_[i] : L"", (q.rc.left + q.rc.right) / 2, q.rc.bottom - 2 * u);
    }
    for (const ExtraTip& e : extraTips) badge(e.keys, (e.at.left + e.at.right) / 2, e.at.bottom - 4 * u);
    return;
  }
  const Layer layer = tips_ == Tips::Group ? Layer::Popup : collapsed_ ? Layer::Overlay : Layer::Body;
  for (size_t i = 0; i < parts_.size(); ++i) {
    const Part& q = parts_[i];
    if (q.layer != layer || !partTips_.count(i)) continue;
    if (q.kind == PartKind::ItemArrow) continue;
    if (q.size == RibbonSize::Large || q.kind == PartKind::GroupButton)
      badge(partTips_[i], (q.rc.left + q.rc.right) / 2, (q.kind == PartKind::Item && IsSplit(*ItemAt(q.group, q.item), q.size)
                                                              ? q.rc.bottom + 30 * u
                                                              : q.rc.bottom - 4 * u));
    else if (q.kind == PartKind::Launcher)
      badge(partTips_[i], (q.rc.left + q.rc.right) / 2, q.rc.bottom);
    else if (q.kind == PartKind::GalleryMore)
      badge(partTips_[i], q.rc.left, q.rc.bottom - 4 * u);
    else
      badge(partTips_[i], q.rc.left + 10 * u, q.rc.bottom - 3 * u);
  }
}

// ---------------------------------------------------------------------------
// Pointer

int Ribbon::PartAt(float x, float y) const {
  auto inLayer = [&](Layer l, bool grid) {
    for (int i = static_cast<int>(parts_.size()) - 1; i >= 0; --i) {
      const Part& p = parts_[i];
      const bool isGrid = p.kind == PartKind::GridChoice || p.kind == PartKind::GridMenu;
      if (p.layer == l && isGrid == grid && Contains(p.rc, x, y)) return i;
    }
    return -1;
  };
  if (gridGroup_ >= 0 && Contains(gridRc_, x, y)) return inLayer(Layer::Popup, true);
  if (popupGroup_ >= 0 && Contains(popupRc_, x, y)) return inLayer(Layer::Popup, false);
  if (collapsed_ && overlay_ && Contains(overlayRc_, x, y)) return inLayer(Layer::Overlay, false);
  if (!Contains(RectF(rc_), x, y)) return -1;
  return inLayer(Layer::Body, false);
}

bool Ribbon::HitPopup(int x, int y) const {
  const float fx = static_cast<float>(x), fy = static_cast<float>(y);
  return (collapsed_ && overlay_ && Contains(overlayRc_, fx, fy)) || (popupGroup_ >= 0 && Contains(popupRc_, fx, fy)) ||
         (gridGroup_ >= 0 && Contains(gridRc_, fx, fy)) || group_.HitPopup(fx, fy);
}

bool Ribbon::Hit(int x, int y) const {
  return !model_.tabs.empty() && (Contains(RectF(rc_), static_cast<float>(x), static_cast<float>(y)) || HitPopup(x, y));
}

bool Ribbon::PopupOpen() const { return overlay_ || popupGroup_ >= 0 || gridGroup_ >= 0 || group_.Open(); }

void Ribbon::ClosePopups() {
  overlay_ = false;
  popupGroup_ = gridGroup_ = -1;
  gridHot_ = -1;
  group_.CloseAll();
}

bool Ribbon::OnMouseMove(int x, int y) {
  const float fx = static_cast<float>(x), fy = static_cast<float>(y);
  bool changed = group_.OnMouseMove(fx, fy);
  int h = -1;
  if (!group_.Dragging() && !group_.HitPopup(fx, fy)) {
    h = PartAt(fx, fy);
    if (h >= 0 && parts_[h].kind == PartKind::Control) h = -1;
  }
  if (h != hot_) {
    hot_ = h;
    changed = true;
  }
  if (h >= 0 && parts_[h].kind == PartKind::GridChoice && gridHot_ != parts_[h].index) {
    gridHot_ = parts_[h].index;
    changed = true;
  }
  return changed;
}

void Ribbon::OnMouseLeave() {
  group_.OnMouseLeave();
  hot_ = -1;
}

void Ribbon::OnMouseDown(int x, int y, bool dbl, bool shift) {
  const float fx = static_cast<float>(x), fy = static_cast<float>(y);
  EndKeyTips();
  Control* open = group_.Open();
  if (group_.OnMouseDown(fx, fy, dbl, shift)) {
    if (open && !group_.Open() && group_.Focused() == open && !group_.FocusVisible()) group_.Blur();
    pressed_ = -1;
    return;
  }
  group_.Blur();
  const int i = PartAt(fx, fy);
  // A click outside an open grid or folded group closes it (on that group's button: the button toggles it).
  if (gridGroup_ >= 0 && !Contains(gridRc_, fx, fy)) {
    const bool own = i >= 0 && parts_[i].group == gridGroup_ && parts_[i].item == gridItem_;
    gridGroup_ = -1;
    if (own && (parts_[i].kind == PartKind::GalleryMore || parts_[i].kind == PartKind::Item)) {
      pressed_ = -1;
      return;
    }
  }
  if (popupGroup_ >= 0 && !Contains(popupRc_, fx, fy) && !(gridGroup_ >= 0 && Contains(gridRc_, fx, fy))) {
    const bool own = i >= 0 && parts_[i].kind == PartKind::GroupButton && parts_[i].group == popupGroup_;
    popupGroup_ = -1;
    if (own) {
      pressed_ = -1;
      return;
    }
  }
  pressed_ = i;
  hot_ = i;
  if (i < 0) return;
  const Part p = parts_[i];
  if (p.kind == PartKind::Tab) {
    // Tabs switch as the button goes down; a double click collapses or pins the ribbon.
    if (dbl) {
      pressed_ = -1;
      if (onCommand) onCommand(CmdRibbonCollapse);
      return;
    }
    const bool same = p.index == tab_;
    Select(model_.tabs[p.index].name);
    if (collapsed_) overlay_ = !(same && overlay_);
    pressed_ = -1;
  }
}

void Ribbon::OnMouseUp(int x, int y) {
  const float fx = static_cast<float>(x), fy = static_cast<float>(y);
  Control* open = group_.Open();
  group_.OnMouseUp(fx, fy);
  if (open && !group_.Open() && group_.Focused() == open && !group_.FocusVisible()) group_.Blur();
  const int down = pressed_;
  pressed_ = -1;
  if (down >= 0 && PartAt(fx, fy) == down) Activate(static_cast<size_t>(down), false);
}

bool Ribbon::OnWheel(int delta, int x, int y) {
  const float fx = static_cast<float>(x), fy = static_cast<float>(y);
  if (group_.OnWheel(delta, fx, fy)) return true;
  if (gridGroup_ >= 0 && Contains(gridRc_, fx, fy)) {
    gridScroll_ += delta > 0 ? -1 : 1;  // clamped by the next layout
    return true;
  }
  const int i = PartAt(fx, fy);
  if (i >= 0 && (parts_[i].kind == PartKind::GalleryChoice || parts_[i].kind == PartKind::GalleryUp ||
                 parts_[i].kind == PartKind::GalleryDown)) {
    const RibbonItem* it = ItemAt(parts_[i].group, parts_[i].item);
    int shown = 0;
    for (const Part& q : parts_)
      if (q.kind == PartKind::GalleryChoice && q.group == parts_[i].group && q.item == parts_[i].item) ++shown;
    int& first = galleryFirst_[it->tool.cmd];
    const int n = static_cast<int>(it->tool.choices.size());
    first = std::clamp(first + (delta > 0 ? -shown : shown), 0, std::max(0, n - 1));
    return true;
  }
  // Over the tab names, the wheel goes through the tabs, as in Office.
  if (fy < rc_.top + kTabsH * ui_ && Contains(RectF(rc_), fx, fy) && !model_.tabs.empty() && tab_ >= 0) {
    const int n = static_cast<int>(model_.tabs.size());
    Select(model_.tabs[std::clamp(tab_ + (delta > 0 ? -1 : 1), 0, n - 1)].name);
    return true;
  }
  return false;
}

std::wstring Ribbon::TooltipAt(int x, int y) const {
  const float fx = static_cast<float>(x), fy = static_cast<float>(y);
  if (group_.Hit(fx, fy) || group_.HitPopup(fx, fy)) return group_.TooltipAt(fx, fy);
  const int i = PartAt(fx, fy);
  if (i < 0) return {};
  const Part& p = parts_[i];
  const float u = ui_;
  (void)u;
  switch (p.kind) {
    case PartKind::File: return L"File: new, open, save, print and options";
    case PartKind::Collapse: return collapsed_ ? L"Keep the ribbon shown (Ctrl+F1)" : L"Collapse the ribbon (Ctrl+F1)";
    case PartKind::Launcher: {
      const CommandState s = Query(Tab()->groups[p.group].launcher);
      return s.label.empty() ? Tab()->groups[p.group].name + L" options" : s.Tooltip();
    }
    case PartKind::GroupButton: return Tab()->groups[p.group].name;
    case PartKind::GalleryUp: return L"Previous row";
    case PartKind::GalleryDown: return L"Next row";
    case PartKind::GalleryMore: return L"More";
    case PartKind::GalleryChoice:
    case PartKind::GridChoice: {
      const RibbonItem* it = ItemAt(p.group, p.item);
      return it && p.index < static_cast<int>(it->tool.choices.size()) ? it->tool.choices[p.index] : L"";
    }
    case PartKind::GridMenu: {
      const RibbonItem* it = ItemAt(p.group, p.item);
      return it ? Query(it->gallery->menu[p.index]).Tooltip() : L"";
    }
    case PartKind::Item:
    case PartKind::ItemArrow: {
      const RibbonItem* it = ItemAt(p.group, p.item);
      if (!it) return {};
      std::wstring tip = it->tool.tooltip;
      if (tip.empty()) {
        CommandState s = Query(it->tool.cmd);
        s.label = LabelOf(*it);
        tip = s.Tooltip();
      }
      if (!it->description.empty()) tip += L"\n" + it->description;
      return tip;
    }
    default: return {};
  }
}

bool Ribbon::OverText(int x, int y) const { return group_.OverText(static_cast<float>(x), static_cast<float>(y)); }

D2D1_RECT_F Ribbon::ItemRect(int cmd) const {
  D2D1_RECT_F r{};
  bool found = false;
  for (const Part& p : parts_) {
    if (p.kind != PartKind::Item && p.kind != PartKind::ItemArrow && p.kind != PartKind::Control) continue;
    const RibbonItem* it = ItemAt(p.group, p.item);
    if (!it || it->tool.cmd != cmd) continue;
    r = found ? RectF(std::min(r.left, p.rc.left), std::min(r.top, p.rc.top), std::max(r.right, p.rc.right),
                      std::max(r.bottom, p.rc.bottom))
              : p.rc;
    found = true;
  }
  return r;
}

// ---------------------------------------------------------------------------
// Doing things

void Ribbon::OpenGroupPopup(int g) {
  popupGroup_ = popupGroup_ == g ? -1 : g;
  gridGroup_ = -1;
}

void Ribbon::OpenGrid(int g, int i, bool byKeyboard) {
  gridGroup_ = g;
  gridItem_ = i;
  gridScroll_ = 0;
  const RibbonItem* it = ItemAt(g, i);
  gridHot_ = byKeyboard && it ? std::max(0, it->tool.choice) : -1;
  if (it && it->tool.choice > 0 && it->gallery) {
    const int cols = std::max(1, it->gallery->popupColumns > 0 ? it->gallery->popupColumns : it->gallery->columns);
    gridScroll_ = it->tool.choice / cols;
  }
}

void Ribbon::Pick(int g, int i, int choice) {
  const RibbonItem* it = ItemAt(g, i);
  if (!it || !it->tool.enabled) return;
  auto onChange = it->tool.onChange;
  ToolValue v;
  v.index = choice;
  if (choice >= 0 && choice < static_cast<int>(it->tool.choices.size())) v.text = it->tool.choices[choice];
  ClosePopups();
  if (onChange) onChange(v);
}

void Ribbon::Activate(size_t index, bool byKeyboard) {
  if (index >= parts_.size()) return;
  const Part p = parts_[index];
  const int cmdOf = [&] {
    const RibbonItem* it = ItemAt(p.group, p.item);
    return it ? it->tool.cmd : 0;
  }();
  auto run = [&](int cmd) {
    const bool inPopup = p.layer != Layer::Body;
    if (inPopup) ClosePopups();
    if (cmd && onCommand) onCommand(cmd);
  };
  switch (p.kind) {
    case PartKind::File: run(CmdFilePage); break;
    case PartKind::Tab:
      Select(model_.tabs[p.index].name);
      if (collapsed_) overlay_ = true;
      break;
    case PartKind::Collapse: run(CmdRibbonCollapse); break;
    case PartKind::Launcher: run(Tab()->groups[p.group].launcher); break;
    case PartKind::GroupButton: OpenGroupPopup(p.group); break;
    case PartKind::GalleryUp:
    case PartKind::GalleryDown: {
      const RibbonItem* it = ItemAt(p.group, p.item);
      int shown = 0;
      for (const Part& q : parts_)
        if (q.kind == PartKind::GalleryChoice && q.group == p.group && q.item == p.item) ++shown;
      int& first = galleryFirst_[it->tool.cmd];
      const int n = static_cast<int>(it->tool.choices.size());
      if (p.kind == PartKind::GalleryUp) first = std::max(0, first - std::max(1, shown));
      else if (first + shown < n) first += std::max(1, shown);
      break;
    }
    case PartKind::GalleryMore: OpenGrid(p.group, p.item, byKeyboard); break;
    case PartKind::GalleryChoice:
    case PartKind::GridChoice: Pick(p.group, p.item, p.index); break;
    case PartKind::GridMenu: {
      const RibbonItem* it = ItemAt(p.group, p.item);
      const int cmd = it ? it->gallery->menu[p.index] : 0;
      ClosePopups();
      if (cmd && onCommand) onCommand(cmd);
      break;
    }
    case PartKind::Control:
      if (p.control) {
        group_.Focus(p.control, true);
        if (dynamic_cast<ComboBox*>(p.control) == nullptr && dynamic_cast<SpinBox*>(p.control) == nullptr)
          p.control->SetOpen(true);
      }
      break;
    case PartKind::Item:
    case PartKind::ItemArrow: {
      const RibbonItem* it = ItemAt(p.group, p.item);
      if (!it || !it->tool.enabled) break;
      if (it->gallery) {  // a gallery folded into its button
        OpenGrid(p.group, p.item, byKeyboard);
        break;
      }
      const bool menu = IsMenuButton(*it) || p.kind == PartKind::ItemArrow;
      if (menu) {
        RibbonItem copy = *it;
        const D2D1_RECT_F anchor = ItemRect(cmdOf);
        if (p.layer == Layer::Overlay) overlay_ = false;  // the menu takes over; the groups go
        if (p.layer == Layer::Popup) popupGroup_ = -1;
        if (onMenu) onMenu(copy, anchor);
        break;
      }
      run(cmdOf);
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// Keyboard

void Ribbon::OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt) {
  if (gridGroup_ >= 0 && !group_.Focused()) {
    const RibbonItem* it = ItemAt(gridGroup_, gridItem_);
    const int n = it ? static_cast<int>(it->tool.choices.size()) : 0;
    switch (vk) {
      case VK_LEFT: gridHot_ = std::max(0, gridHot_ - 1); break;
      case VK_RIGHT: gridHot_ = std::min(n - 1, gridHot_ + 1); break;
      case VK_UP: gridHot_ = std::max(0, gridHot_ - gridCols_); break;
      case VK_DOWN: gridHot_ = std::min(n - 1, gridHot_ + gridCols_); break;
      case VK_RETURN:
      case VK_SPACE: Pick(gridGroup_, gridItem_, gridHot_); return;
      case VK_ESCAPE: gridGroup_ = -1; return;
      default: return;
    }
    if (gridHot_ / std::max(1, gridCols_) < gridScroll_) gridScroll_ = gridHot_ / std::max(1, gridCols_);
    if (gridHot_ / std::max(1, gridCols_) >= gridScroll_ + gridRows_) gridScroll_ = gridHot_ / std::max(1, gridCols_) - gridRows_ + 1;
    return;
  }
  if (!group_.Focused()) return;
  const bool used = group_.OnKeyDown(vk, ctrl, shift, alt);
  if (!used && (vk == VK_RETURN || vk == VK_ESCAPE) && !group_.Open()) {
    group_.Blur();
    if (overlay_ || popupGroup_ >= 0) ClosePopups();
  }
}

bool Ribbon::StartKeyTips(wchar_t first) {
  if (model_.tabs.empty()) return false;
  typed_.clear();
  tips_ = Tips::Tabs;
  if (!first) return true;
  KeyTipKey(first);
  if (tips_ == Tips::Tabs && typed_.empty()) {  // no tab's letter
    EndKeyTips();
    return false;
  }
  return true;
}

void Ribbon::EndKeyTips() {
  tips_ = Tips::None;
  typed_.clear();
}

bool Ribbon::KeyTipKey(UINT vk) {
  if (tips_ == Tips::None) return false;
  if (vk == VK_ESCAPE) {
    typed_.clear();
    if (tips_ == Tips::Group) {
      popupGroup_ = -1;
      tips_ = Tips::Items;
    } else if (tips_ == Tips::Items) {
      tips_ = Tips::Tabs;
      overlay_ = false;
    } else {
      EndKeyTips();
    }
    return true;
  }
  if (vk == VK_BACK && !typed_.empty()) {
    typed_.pop_back();
    return true;
  }
  wchar_t ch = 0;
  if (vk >= 'A' && vk <= 'Z') ch = static_cast<wchar_t>(vk);
  else if (vk >= '0' && vk <= '9') ch = static_cast<wchar_t>(vk);
  else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) ch = static_cast<wchar_t>(L'0' + (vk - VK_NUMPAD0));
  if (!ch) {
    EndKeyTips();
    return vk == VK_MENU || vk == VK_F10;  // Alt again ends them; other keys do their own thing too
  }
  const std::wstring want = typed_ + ch;
  // The letters on show at this level.
  struct Choice {
    std::wstring tip;
    int part;  // or -1 - extra index
  };
  std::vector<Choice> choices;
  if (tips_ == Tips::Tabs) {
    for (size_t i = 0; i < parts_.size(); ++i)
      if ((parts_[i].kind == PartKind::File || parts_[i].kind == PartKind::Tab) && partTips_.count(i))
        choices.push_back({partTips_[i], static_cast<int>(i)});
    for (size_t e = 0; e < extraTips.size(); ++e) choices.push_back({extraTips[e].keys, -1 - static_cast<int>(e)});
  } else {
    const Layer layer = tips_ == Tips::Group ? Layer::Popup : collapsed_ ? Layer::Overlay : Layer::Body;
    for (size_t i = 0; i < parts_.size(); ++i)
      if (parts_[i].layer == layer && parts_[i].kind != PartKind::ItemArrow && partTips_.count(i))
        choices.push_back({partTips_[i], static_cast<int>(i)});
  }
  bool prefix = false;
  for (const Choice& c : choices) {
    if (c.tip == want) {
      typed_.clear();
      if (c.part < 0) {
        const int cmd = extraTips[static_cast<size_t>(-1 - c.part)].cmd;
        EndKeyTips();
        if (onCommand) onCommand(cmd);
        return true;
      }
      const Part p = parts_[static_cast<size_t>(c.part)];
      if (p.kind == PartKind::Tab) {
        Select(model_.tabs[p.index].name);
        if (collapsed_) overlay_ = true;
        tips_ = Tips::Items;
        return true;
      }
      if (p.kind == PartKind::GroupButton) {
        popupGroup_ = p.group;
        tips_ = Tips::Group;
        return true;
      }
      EndKeyTips();
      // A split button's letter runs its main part; a gallery's opens the grid.
      Activate(static_cast<size_t>(c.part), true);
      return true;
    }
    if (c.tip.compare(0, want.size(), want) == 0) prefix = true;
  }
  if (prefix) typed_ = want;
  return true;  // a letter that matches nothing is ignored, as Office does
}

// ---------------------------------------------------------------------------
// The rest

void Ribbon::Forget() {
  group_.Clear();
  shown_.clear();
  controls_.clear();
  kinds_.clear();
  model_ = RibbonModel();
  parts_.clear();
  hot_ = pressed_ = -1;
  ClosePopups();
  EndKeyTips();
}

void Ribbon::Describe(uia::Tree& t, int parent) {
  if (model_.tabs.empty()) return;
  int bar = t.Add(parent, CmdFilePage, UIA_ToolBarControlTypeId, L"Ribbon", RectF(rc_));
  for (size_t i = 0; i < parts_.size(); ++i) {
    const Part& p = parts_[i];
    switch (p.kind) {
      case PartKind::File:
      case PartKind::Tab: {
        const std::wstring name = p.kind == PartKind::File ? L"File" : model_.tabs[p.index].name;
        int n = t.Add(bar, 10000 + p.index, p.kind == PartKind::File ? UIA_ButtonControlTypeId : UIA_TabItemControlTypeId, name, p.rc);
        t[n].accelerator = L"Alt, " + (partTips_.count(i) ? partTips_[i] : L"");
        t[n].invoke = [this, i] { Activate(i, true); };
        break;
      }
      case PartKind::Control:
        if (p.control) p.control->Describe(t, bar);
        break;
      case PartKind::Item:
      case PartKind::Launcher:
      case PartKind::GroupButton: {
        const RibbonItem* it = ItemAt(p.group, p.item);
        const std::wstring name = p.kind == PartKind::Launcher ? Tab()->groups[p.group].name + L" options"
                                  : p.kind == PartKind::GroupButton ? Tab()->groups[p.group].name
                                  : it ? LabelOf(*it) : L"";
        int n = t.Add(bar, 20000 + static_cast<int64_t>(i), UIA_ButtonControlTypeId, name, p.rc);
        t[n].enabled = !it || it->tool.enabled;
        t[n].invoke = [this, i] { Activate(i, true); };
        break;
      }
      default: break;
    }
  }
}

std::wstring Ribbon::Dump() {
  std::wstring out;
  auto num = [](float v) { return std::to_wstring(static_cast<int>(std::lround(v))); };
  auto rect = [&](const D2D1_RECT_F& r) {
    return num(r.left) + L"," + num(r.top) + L"," + num(r.right - r.left) + L"," + num(r.bottom - r.top);
  };
  for (int t = 0; t < static_cast<int>(model_.tabs.size()); ++t)
    out += L"tab " + model_.tabs[t].name + L"|" + (t < static_cast<int>(tabTips_.size()) ? tabTips_[t] : L"") +
           (model_.tabs[t].contextual ? L"|contextual" : L"") + (t == tab_ ? L"|selected" : L"") + L"\n";
  const RibbonTab* tab = Tab();
  if (!tab) return out;
  for (int g = 0; g < static_cast<int>(tab->groups.size()); ++g) {
    const RibbonGroup& grp = tab->groups[g];
    out += L"group " + grp.name + L"|stage " + std::to_wstring(g < static_cast<int>(stage_.size()) ? stage_[g] : 0) + L"|" +
           (g < static_cast<int>(groupRc_.size()) ? rect(groupRc_[g]) : L"") + L"\n";
    for (int i = 0; i < static_cast<int>(grp.items.size()); ++i) {
      const RibbonItem& it = grp.items[i];
      const wchar_t* size = L"folded";
      D2D1_RECT_F r{};
      for (const Part& p : parts_)
        if (p.group == g && p.item == i && p.layer != Layer::Popup) {
          size = p.size == RibbonSize::Large ? L"large" : p.size == RibbonSize::Medium ? L"medium" : L"small";
          if (p.kind == PartKind::GalleryChoice || p.kind == PartKind::GalleryUp) size = L"gallery";
          r = p.rc;
          break;
        }
      out += L"item " + std::to_wstring(it.tool.cmd) + L"|" + LabelOf(it) + L"|" + size + L"|" + itemTips_[{g, i}] + L"|" +
             rect(r) + L"\n";
    }
    if (grp.launcher) out += L"launcher " + std::to_wstring(grp.launcher) + L"|" + itemTips_[{g, -2}] + L"\n";
  }
  return out;
}

std::vector<std::wstring> Ribbon::Problems() const {
  std::vector<std::wstring> out;
  const RibbonTab* tab = Tab();
  if (!tab) return out;
  for (int g = 0; g < static_cast<int>(groupRc_.size()) && g < static_cast<int>(tab->groups.size()); ++g)
    if (groupRc_[g].right > rc_.right + 0.5f)
      out.push_back(L"the group " + tab->groups[g].name + L" is past the window's edge at " +
                    std::to_wstring(rc_.right - rc_.left) + L" px");
  // KeyTips: the tabs' with File's and quick access's, then this tab's items'.
  auto check = [&](const std::vector<std::pair<std::wstring, std::wstring>>& tips) {
    for (size_t a = 0; a < tips.size(); ++a) {
      if (tips[a].first.empty()) out.push_back(tips[a].second + L" has no KeyTip");
      for (size_t b = a + 1; b < tips.size(); ++b) {
        const std::wstring &x = tips[a].first, &y = tips[b].first;
        if (!x.empty() && !y.empty() && (x.compare(0, y.size(), y) == 0 || y.compare(0, x.size(), x) == 0))
          out.push_back(L"KeyTips " + x + L" (" + tips[a].second + L") and " + y + L" (" + tips[b].second + L") clash");
      }
    }
  };
  std::vector<std::pair<std::wstring, std::wstring>> tabs{{L"F", L"File"}};
  for (const ExtraTip& e : extraTips) tabs.push_back({e.keys, L"quick access"});
  for (size_t t = 0; t < model_.tabs.size() && t < tabTips_.size(); ++t) tabs.push_back({tabTips_[t], model_.tabs[t].name});
  check(tabs);
  std::vector<std::pair<std::wstring, std::wstring>> items;
  for (const auto& [key, tip] : itemTips_) {
    const RibbonGroup& grp = tab->groups[key.first];
    items.push_back({tip, key.second == -1   ? grp.name + L" (folded)"
                          : key.second == -2 ? grp.name + L" (launcher)"
                                             : grp.name + L" › " + LabelOf(grp.items[key.second])});
  }
  check(items);
  return out;
}

std::vector<std::pair<int, std::wstring>> Ribbon::Commands() const {
  std::vector<std::pair<int, std::wstring>> out;
  auto add = [&](int cmd, const std::wstring& where) {
    if (cmd > 0) out.push_back({cmd, where});
  };
  for (const RibbonTab& t : model_.tabs)
    for (const RibbonGroup& g : t.groups) {
      const std::wstring where = t.name + L" › " + g.name;
      for (const RibbonItem& it : g.items) {
        add(it.tool.cmd, where);
        for (int c : it.menu) add(c, where);
        if (it.gallery)
          for (int c : it.gallery->menu) add(c, where);
      }
      add(g.launcher, where);
    }
  return out;
}

}  // namespace ui
