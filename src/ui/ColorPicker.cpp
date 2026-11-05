// The color palette, the color picker and the split button (Controls.h).
#include "ui/Controls.h"

#include <commdlg.h>

#include <algorithm>
#include <cmath>

#include "ui/Icons.h"
#include "ui/Theme.h"

using namespace gfx;

namespace {

constexpr float kPad = 8;      // around the palette
constexpr float kSwatch = 18;  // swatch size
constexpr float kGap = 4;      // between swatches in a row
constexpr float kHeader = 22;  // "Theme colors"
constexpr float kRow = 30;     // Automatic and More colors…
constexpr int kColumns = 10;
constexpr size_t kMaxRecent = 10;

struct Hsl {
  float h, s, l;
};

Hsl ToHsl(D2D1_COLOR_F c) {
  float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
  float l = (mx + mn) / 2, d = mx - mn;
  if (d <= 0) return {0, 0, l};
  float s = l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
  float h = mx == c.r ? (c.g - c.b) / d + (c.g < c.b ? 6 : 0) : mx == c.g ? (c.b - c.r) / d + 2 : (c.r - c.g) / d + 4;
  return {h / 6, s, l};
}

D2D1_COLOR_F FromHsl(Hsl x) {
  auto hue = [](float p, float q, float t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.f / 6) return p + (q - p) * 6 * t;
    if (t < 1.f / 2) return q;
    if (t < 2.f / 3) return p + (q - p) * (2.f / 3 - t) * 6;
    return p;
  };
  if (x.s <= 0) return D2D1::ColorF(x.l, x.l, x.l);
  float q = x.l < 0.5f ? x.l * (1 + x.s) : x.l + x.s - x.l * x.s, p = 2 * x.l - q;
  return D2D1::ColorF(hue(p, q, x.h + 1.f / 3), hue(p, q, x.h), hue(p, q, x.h - 1.f / 3));
}

// Rounds to 8 bits per channel, the precision documents store colors in.
D2D1_COLOR_F Quantize(D2D1_COLOR_F c) {
  auto q = [](float v) { return std::round(std::clamp(v, 0.f, 1.f) * 255) / 255; };
  return D2D1::ColorF(q(c.r), q(c.g), q(c.b), c.a);
}

int Byte(float v) { return static_cast<int>(std::lround(std::clamp(v, 0.f, 1.f) * 255)); }

bool SameColor(D2D1_COLOR_F a, D2D1_COLOR_F b) {
  return Byte(a.r) == Byte(b.r) && Byte(a.g) == Byte(b.g) && Byte(a.b) == Byte(b.b) && (a.a > 0) == (b.a > 0);
}

// The five tints and shades of a theme color shown under it, lightest first, as Office does:
// near-black gets lighter, near-white darker, everything else both.
void Variants(D2D1_COLOR_F base, D2D1_COLOR_F out[5], const wchar_t* names[5], float* amounts = nullptr) {
  static const wchar_t* const kLighterDarker[5] = {L"Lighter 80%", L"Lighter 60%", L"Lighter 40%", L"Darker 25%",
                                                   L"Darker 50%"};
  static const wchar_t* const kLighter[5] = {L"Lighter 50%", L"Lighter 35%", L"Lighter 25%", L"Lighter 15%",
                                             L"Lighter 5%"};
  static const wchar_t* const kDarker[5] = {L"Darker 5%", L"Darker 15%", L"Darker 25%", L"Darker 35%",
                                            L"Darker 50%"};
  Hsl hsl = ToHsl(base);
  float amount[5];
  const wchar_t* const* labels;
  if (hsl.l < 0.04f) {
    const float a[5] = {0.5f, 0.35f, 0.25f, 0.15f, 0.05f};
    std::copy(a, a + 5, amount);
    labels = kLighter;
  } else if (hsl.l > 0.96f) {
    const float a[5] = {-0.05f, -0.15f, -0.25f, -0.35f, -0.5f};
    std::copy(a, a + 5, amount);
    labels = kDarker;
  } else {
    const float a[5] = {0.8f, 0.6f, 0.4f, -0.25f, -0.5f};
    std::copy(a, a + 5, amount);
    labels = kLighterDarker;
  }
  for (int i = 0; i < 5; ++i) {
    Hsl v = hsl;
    v.l = amount[i] > 0 ? hsl.l + (1 - hsl.l) * amount[i] : hsl.l * (1 + amount[i]);
    out[i] = Quantize(FromHsl(v));
    names[i] = labels[i];
    if (amounts) amounts[i] = amount[i];
  }
}

const NamedColor kStandardColors[kColumns] = {
    {Rgb(0xC00000), L"Dark Red"}, {Rgb(0xFF0000), L"Red"},         {Rgb(0xFFC000), L"Orange"},
    {Rgb(0xFFFF00), L"Yellow"},   {Rgb(0x92D050), L"Light Green"}, {Rgb(0x00B050), L"Green"},
    {Rgb(0x00B0F0), L"Light Blue"}, {Rgb(0x0070C0), L"Blue"},      {Rgb(0x002060), L"Dark Blue"},
    {Rgb(0x7030A0), L"Purple"}};

std::wstring RgbName(D2D1_COLOR_F c) {
  return L"Red " + std::to_wstring(Byte(c.r)) + L", green " + std::to_wstring(Byte(c.g)) + L", blue " +
         std::to_wstring(Byte(c.b));
}

void HoverTo(Anim& a, bool on) {
  if (on != (a.Target() > 0.5f)) a.To(on ? 1.f : 0.f, on ? 80.f : 150.f);
}

}  // namespace

// ---------------------------------------------------------------------------
// ColorPalette

ColorPalette::ColorPalette() : theme(DefaultTheme()) {}

std::vector<NamedColor> ColorPalette::DefaultTheme() {
  return {{Rgb(0xFFFFFF), L"White, Background 1"}, {Rgb(0x000000), L"Black, Text 1"},
          {Rgb(0xE7E6E6), L"Light Gray, Background 2"}, {Rgb(0x44546A), L"Blue-Gray, Text 2"},
          {Rgb(0x4472C4), L"Blue, Accent 1"}, {Rgb(0xED7D31), L"Orange, Accent 2"},
          {Rgb(0xA5A5A5), L"Gray, Accent 3"}, {Rgb(0xFFC000), L"Gold, Accent 4"},
          {Rgb(0x5B9BD5), L"Blue, Accent 5"}, {Rgb(0x70AD47), L"Green, Accent 6"}};
}

bool ColorPalette::ThemeCell(const std::vector<NamedColor>& theme, D2D1_COLOR_F c, int& index, int& tint, int& shade) {
  tint = shade = -1;
  for (size_t i = 0; i < theme.size() && i < kColumns; ++i)
    if (SameColor(theme[i].color, c)) {
      index = static_cast<int>(i);
      return true;
    }
  for (size_t i = 0; i < theme.size() && i < kColumns; ++i) {
    D2D1_COLOR_F out[5];
    const wchar_t* names[5];
    float amounts[5];
    Variants(theme[i].color, out, names, amounts);
    for (int v = 0; v < 5; ++v) {
      if (!SameColor(out[v], c)) continue;
      index = static_cast<int>(i);
      // Lighter 80%: the luminance keeps 20% of itself (tint 51, "33"); darker 25%: 75% (shade 191, "BF").
      if (amounts[v] > 0) tint = static_cast<int>(std::lround((1 - amounts[v]) * 255));
      else shade = static_cast<int>(std::lround((1 + amounts[v]) * 255));
      return true;
    }
  }
  return false;
}

std::vector<NamedColor>& ColorPalette::Recent() {
  static std::vector<NamedColor> recent;
  return recent;
}

D2D1_SIZE_F ColorPalette::PreferredSize() const {
  const float u = Ui();
  float w = kPad * 2 + kColumns * kSwatch + (kColumns - 1) * kGap;
  float h = kPad * 2;
  if (!automaticLabel.empty()) h += kRow + 4;
  h += kHeader + kSwatch + kGap + 5 * kSwatch;  // theme colors
  h += kHeader + kSwatch;                       // standard colors
  if (!Recent().empty()) h += kHeader + kSwatch;
  h += 9 + kRow;  // separator, More colors…
  return {std::ceil(w * u), std::ceil(h * u)};
}

void ColorPalette::Layout() {
  const float u = Ui();
  cells_.clear();
  headers_.clear();
  const float left = rc.left + kPad * u, right = rc.right - kPad * u;
  float y = rc.top + kPad * u;
  auto swatchRow = [&](Section s, int row, const NamedColor* colors, size_t count, float top) {
    for (size_t i = 0; i < count; ++i) {
      float x = left + i * (kSwatch + kGap) * u;
      cells_.push_back({s, row, static_cast<int>(i), colors[i].color, colors[i].name,
                        RectF(std::round(x), std::round(top), std::round(x + kSwatch * u), std::round(top + kSwatch * u))});
    }
  };
  if (!automaticLabel.empty()) {
    cells_.push_back({kAutomatic, 0, -1, automaticColor, automaticLabel, RectF(left - 4 * u, y, right + 4 * u, y + kRow * u)});
    y += (kRow + 4) * u;
  }
  headers_.push_back({kTheme, RectF(left, y, right, y + kHeader * u)});
  y += kHeader * u;
  swatchRow(kTheme, 1, theme.data(), std::min<size_t>(theme.size(), kColumns), y);
  y += (kSwatch + kGap) * u;
  for (int v = 0; v < 5; ++v) {
    NamedColor shades[kColumns];
    for (size_t i = 0; i < theme.size() && i < kColumns; ++i) {
      D2D1_COLOR_F out[5];
      const wchar_t* names[5];
      Variants(theme[i].color, out, names);
      shades[i] = {out[v], theme[i].name + L", " + names[v]};
    }
    swatchRow(kTheme, 2 + v, shades, std::min<size_t>(theme.size(), kColumns), y);
    y += kSwatch * u;
  }
  headers_.push_back({kStandard, RectF(left, y, right, y + kHeader * u)});
  y += kHeader * u;
  swatchRow(kStandard, 7, kStandardColors, kColumns, y);
  y += kSwatch * u;
  if (!Recent().empty()) {
    headers_.push_back({kRecent, RectF(left, y, right, y + kHeader * u)});
    y += kHeader * u;
    swatchRow(kRecent, 8, Recent().data(), std::min(Recent().size(), kMaxRecent), y);
    y += kSwatch * u;
  }
  y += 9 * u;
  cells_.push_back({kMore, 9, -1, {}, L"More colors…", RectF(left - 4 * u, y, right + 4 * u, y + kRow * u)});
}

bool ColorPalette::IsCurrent(const Cell& cell) const {
  if (cell.section == kMore) return false;
  if (cell.section == kAutomatic) return currentAutomatic;
  return !currentAutomatic && SameColor(cell.color, current);
}

void ColorPalette::HighlightCurrent() {
  hot_ = -1;
  for (size_t i = 0; i < cells_.size() && hot_ < 0; ++i)
    if (IsCurrent(cells_[i])) hot_ = static_cast<int>(i);
  if (hot_ < 0 && !cells_.empty()) hot_ = 0;
}

std::wstring ColorPalette::NameOf(D2D1_COLOR_F c, bool automatic) const {
  if (automatic) return automaticLabel;
  for (const Cell& cell : cells_)
    if (cell.section != kMore && cell.section != kAutomatic && SameColor(cell.color, c)) return cell.name;
  for (const NamedColor& s : kStandardColors)
    if (SameColor(s.color, c)) return s.name;
  for (const NamedColor& t : theme)
    if (SameColor(t.color, c)) return t.name;
  return RgbName(c);
}

int ColorPalette::CellAt(float x, float y) const {
  for (size_t i = 0; i < cells_.size(); ++i) {
    D2D1_RECT_F r = cells_[i].col >= 0 ? Inflate(cells_[i].rc, kGap / 2 * Ui(), 0) : cells_[i].rc;
    if (Contains(r, x, y)) return static_cast<int>(i);
  }
  return -1;
}

void ColorPalette::Draw(Canvas& c) {
  const Palette& p = theme::P();
  const float u = Ui();
  static const wchar_t* const kTitles[] = {L"", L"Theme colors", L"Standard colors", L"Recent colors", L""};
  for (const auto& [section, r] : headers_)
    c.Text(kTitles[section], r, p.textSecondary, 12 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  for (size_t i = 0; i < cells_.size(); ++i) {
    const Cell& cell = cells_[i];
    const bool hot = static_cast<int>(i) == hot_;
    if (cell.col < 0) {
      // A whole row: Automatic, or More colors…
      if (cell.section == kMore) {
        float sy = cell.rc.top - 5 * u;
        c.Fill(RectF(cell.rc.left + 4 * u, sy, cell.rc.right - 4 * u, sy + 1), p.popupBorder);
      }
      if (hot) c.FillRound(cell.rc, 4 * u, p.hover);
      if (static_cast<int>(i) == pressed_) c.FillRound(cell.rc, 4 * u, p.pressed);
      float cy = std::round((cell.rc.top + cell.rc.bottom) / 2);
      D2D1_RECT_F icon = RectF(cell.rc.left + 8 * u, cy - 8 * u, cell.rc.left + 24 * u, cy + 8 * u);
      if (cell.section == kAutomatic) {
        ctl::DrawSwatch(c, icon, cell.color, u);
        if (IsCurrent(cell)) c.StrokeRound(Inflate(icon, 2 * u, 2 * u), 2 * u, p.accent, 2 * u);
      } else {
        c.Icon(icon::Color, icon, p.text, 14 * u);
      }
      c.Text(cell.name, RectF(icon.right + 10 * u, cell.rc.top, cell.rc.right - 8 * u, cell.rc.bottom), p.text, 13 * u);
      continue;
    }
    ctl::DrawSwatch(c, cell.rc, cell.color, u);
    if (IsCurrent(cell)) {
      c.Stroke(Inflate(cell.rc, 2 * u, 2 * u), p.accent, 2 * u);
    }
    if (hot) {
      // A ring that shows on every color: dark outside, light inside.
      c.Stroke(Inflate(cell.rc, 1 * u, 1 * u), p.dark ? Rgb(0xFFFFFF) : Rgb(0x000000), std::max(1.f, 1.5f * u));
      c.Stroke(cell.rc, p.dark ? Rgb(0x000000) : Rgb(0xFFFFFF), 1);
    }
  }
}

bool ColorPalette::OnMouseMove(float x, float y) {
  int cell = CellAt(x, y);
  if (cell == hover_) return false;
  hover_ = cell;
  if (cell >= 0) hot_ = cell;
  return true;
}

void ColorPalette::OnMouseLeave() { hover_ = -1; }

void ColorPalette::OnMouseDown(float x, float y, bool, bool) { pressed_ = CellAt(x, y); }

void ColorPalette::OnMouseUp(float x, float y) {
  int pressed = pressed_;
  pressed_ = -1;
  if (pressed >= 0 && CellAt(x, y) == pressed) Pick(pressed);
}

void ColorPalette::Pick(int i) {
  if (i < 0 || i >= static_cast<int>(cells_.size())) return;
  const Cell cell = cells_[i];
  if (cell.section == kMore) {
    MoreColors();
    return;
  }
  if (onPick) onPick(cell.color, cell.section == kAutomatic);
}

void ColorPalette::MoreColors() {
  static COLORREF custom[16] = {};  // the dialog's own custom colors, for this session
  if (beforeDialog) beforeDialog();
  CHOOSECOLORW cc{sizeof(cc)};
  cc.hwndOwner = Window();
  D2D1_COLOR_F start = currentAutomatic ? Rgb(0x000000) : current;
  cc.rgbResult = RGB(Byte(start.r), Byte(start.g), Byte(start.b));
  cc.lpCustColors = custom;
  cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
  if (!ChooseColorW(&cc)) return;
  D2D1_COLOR_F picked = D2D1::ColorF(GetRValue(cc.rgbResult) / 255.f, GetGValue(cc.rgbResult) / 255.f,
                                     GetBValue(cc.rgbResult) / 255.f);
  auto& recent = Recent();
  recent.erase(std::remove_if(recent.begin(), recent.end(), [&](const NamedColor& n) { return SameColor(n.color, picked); }),
               recent.end());
  recent.insert(recent.begin(), {picked, RgbName(picked)});
  if (recent.size() > kMaxRecent) recent.resize(kMaxRecent);
  if (onPick) onPick(picked, false);
}

bool ColorPalette::OnKeyDown(UINT vk, bool, bool, bool alt) {
  if (alt || cells_.empty()) return false;
  const int n = static_cast<int>(cells_.size());
  if (hot_ < 0) hot_ = 0;
  const Cell& cur = cells_[hot_];
  switch (vk) {
    case VK_LEFT: hot_ = std::max(0, hot_ - 1); return true;
    case VK_RIGHT: hot_ = std::min(n - 1, hot_ + 1); return true;
    case VK_HOME: hot_ = 0; return true;
    case VK_END: hot_ = n - 1; return true;
    case VK_UP:
    case VK_DOWN: {
      // The same column in the row above or below; whole rows are reached from any column.
      const int dir = vk == VK_DOWN ? 1 : -1;
      int targetRow = -1;
      for (int k = hot_ + dir; k >= 0 && k < n && targetRow < 0; k += dir)
        if (cells_[k].row != cur.row) targetRow = cells_[k].row;
      if (targetRow < 0) return true;
      int col = cur.col >= 0 ? cur.col : 0;
      int best = -1;
      for (int k = 0; k < n; ++k) {
        if (cells_[k].row != targetRow) continue;
        if (cells_[k].col < 0 || cells_[k].col <= col) best = k;
      }
      if (best >= 0) hot_ = best;
      return true;
    }
    case VK_RETURN:
    case VK_SPACE: Pick(hot_); return true;
    default: return false;
  }
}

std::wstring ColorPalette::TooltipAt(float x, float y) const {
  int cell = CellAt(x, y);
  return cell >= 0 && cells_[cell].col >= 0 ? cells_[cell].name : L"";
}

void ColorPalette::Describe(uia::Tree& t, int parent) { DescribeAsPart(t, parent, id, Focused()); }

int ColorPalette::DescribeAsPart(uia::Tree& t, int parent, int64_t partId, bool focusCells) {
  int pane = t.Add(parent, partId, UIA_PaneControlTypeId, Name(), rc);
  t[pane].layer = 2;
  int lists[4] = {-1, -1, -1, -1};
  static const wchar_t* const kTitles[] = {L"", L"Theme colors", L"Standard colors", L"Recent colors"};
  for (size_t i = 0; i < cells_.size(); ++i) {
    const Cell& cell = cells_[i];
    const bool focused = focusCells && static_cast<int>(i) == hot_;
    auto pick = [this, i] { Pick(static_cast<int>(i)); };
    if (cell.col < 0) {
      // Automatic and More colors… are buttons.
      int b = t.Add(pane, 100 + cell.section, UIA_ButtonControlTypeId, cell.name, cell.rc);
      uia::Node& n = t[b];
      n.invoke = pick;
      n.focusable = true;
      n.focused = focused;
      n.focus = [this, i] { hot_ = static_cast<int>(i); };
      if (cell.section == kAutomatic && IsCurrent(cell)) n.status = L"Selected";
      continue;
    }
    int& list = lists[cell.section];
    if (list < 0) {
      list = t.Add(pane, cell.section, UIA_ListControlTypeId, kTitles[cell.section]);
      uia::Node& l = t[list];
      l.selectionContainer = true;
      l.orientation = OrientationType_Horizontal;
      // The list spans its swatches.
      l.rc = cell.rc;
    }
    t[list].rc.right = std::max(t[list].rc.right, cell.rc.right);
    t[list].rc.bottom = std::max(t[list].rc.bottom, cell.rc.bottom);
    int item = t.Add(list, static_cast<int64_t>(i), UIA_ListItemControlTypeId, cell.name, cell.rc);
    uia::Node& n = t[item];
    n.selectable = true;
    n.selected = IsCurrent(cell);
    n.select = pick;
    n.focusable = true;
    n.focused = focused;
    n.focus = [this, i] { hot_ = static_cast<int>(i); };
  }
  return pane;
}

// ---------------------------------------------------------------------------
// ColorPicker

ColorPicker::ColorPicker() {
  palette_.onPick = [this](D2D1_COLOR_F c, bool isAutomatic) {
    SetOpen(false);
    color = c;
    automatic = isAutomatic;
    if (onChange) onChange(c, isAutomatic);
  };
  palette_.beforeDialog = [this] { SetOpen(false); };
}

void ColorPicker::SetGroup(ControlGroup* g) {
  Control::SetGroup(g);
  palette_.SetGroup(g);
}

void ColorPicker::Configure() {
  palette_.label = Name();
  palette_.theme = theme;
  palette_.automaticLabel = automaticLabel;
  palette_.automaticColor = automaticColor;
  palette_.current = color;
  palette_.currentAutomatic = automatic;
}

void ColorPicker::SetOpen(bool open) {
  if (open == popup_.open) return;
  if (!open) {
    popup_.Close();
    inPalette_ = false;
    return;
  }
  if (!enabled) return;
  Opened();
  Configure();
  D2D1_SIZE_F size = palette_.PreferredSize();
  popup_.Place(rc, size.width + 8 * Ui(), size.height + 8 * Ui(), PopupBounds(), Ui());
  palette_.rc = popup_.Inner(Ui());
  palette_.Layout();
  palette_.HighlightCurrent();
  popup_.Open();
}

bool ColorPicker::HitPopup(float x, float y) const { return popup_.Hit(x, y); }

void ColorPicker::Draw(Canvas& c) {
  const Palette& p = theme::P();
  const float u = Ui();
  const float hv = enabled ? hover_.V() : 0, pv = enabled ? std::max(press_.V(), popup_.open ? 0.7f : 0.f) : 0;
  const D2D1_COLOR_F shown = automatic ? automaticColor : color;
  const D2D1_COLOR_F fg = enabled ? p.text : p.textDisabled;
  const float chevW = 16 * u;
  if (glyph) {
    // Toolbar style: the glyph over a bar in the color, then the arrow.
    ctl::DrawSubtle(c, rc, u, hv, pv);
    D2D1_RECT_F main = RectF(rc.left, rc.top, rc.right - chevW, rc.bottom);
    float cx = std::round((main.left + main.right) / 2), cy = std::round((rc.top + rc.bottom) / 2);
    c.Icon(glyph, RectF(cx - 10 * u, cy - 12 * u, cx + 10 * u, cy + 6 * u), fg, 16 * u);
    D2D1_RECT_F bar = RectF(cx - 8 * u, cy + 6 * u, cx + 8 * u, cy + 6 * u + std::max(2.f, std::round(3 * u)));
    if (shown.a > 0) c.Fill(bar, enabled ? shown : Alpha(shown, 0.4f));
    else c.Stroke(bar, Alpha(fg, 0.5f), 1);
  } else {
    // A plain swatch in a box.
    ctl::DrawBox(c, rc, u, hv, pv);
    float cy = std::round((rc.top + rc.bottom) / 2);
    D2D1_RECT_F sw = RectF(rc.left + 8 * u, cy - 8 * u, rc.right - chevW - 6 * u, cy + 8 * u);
    ctl::DrawSwatch(c, sw, shown, u);
  }
  ctl::DrawChevron(c, RectF(rc.right - chevW - 2 * u, rc.top, rc.right - 2 * u, rc.bottom),
                   enabled ? p.textSecondary : p.textDisabled, u);
  if (FocusVisible()) ctl::DrawFocusRing(c, rc, u);
}

void ColorPicker::DrawPopup(Canvas& c) {
  if (!popup_.Visible()) return;
  popup_.Begin(c, Ui());
  palette_.Draw(c);
  popup_.End(c);
}

bool ColorPicker::OnMouseMove(float x, float y) {
  if (inPalette_ || HitPopup(x, y)) return palette_.OnMouseMove(x, y);
  bool over = Hit(x, y);
  bool changed = over != (hover_.Target() > 0.5f);
  HoverTo(hover_, over);
  return changed;
}

void ColorPicker::OnMouseLeave() {
  HoverTo(hover_, false);
  palette_.OnMouseLeave();
}

void ColorPicker::OnMouseDown(float x, float y, bool dbl, bool shift) {
  if (HitPopup(x, y)) {
    inPalette_ = true;
    palette_.OnMouseDown(x, y, dbl, shift);
    return;
  }
  press_.To(1, 40);
  SetOpen(!popup_.open);
}

void ColorPicker::OnMouseUp(float x, float y) {
  press_.To(0, 120);
  if (inPalette_) {
    inPalette_ = false;
    palette_.OnMouseUp(x, y);
  }
}

bool ColorPicker::OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt) {
  if (!enabled) return false;
  if (popup_.open) {
    switch (vk) {
      case VK_ESCAPE:
      case VK_F4: SetOpen(false); return true;
      case VK_TAB: SetOpen(false); return false;
      case VK_UP:
        if (alt) {
          SetOpen(false);
          return true;
        }
        break;
      case VK_RETURN:  // picked: false lets a toolbar hand the keyboard back
        palette_.OnKeyDown(vk, ctrl, shift, alt);
        return false;
      default: break;
    }
    return palette_.OnKeyDown(vk, ctrl, shift, alt);
  }
  switch (vk) {
    case VK_RETURN:
    case VK_SPACE:
    case VK_F4:
    case VK_DOWN: SetOpen(true); return true;
    default: return false;
  }
}

void ColorPicker::OnFocus(bool focused) {
  if (!focused) SetOpen(false);
}

std::wstring ColorPicker::TooltipAt(float x, float y) const {
  return HitPopup(x, y) ? palette_.TooltipAt(x, y) : tooltip;
}

void ColorPicker::Describe(uia::Tree& t, int parent) {
  if (!popup_.open) Configure();
  int i = t.Add(parent, id, UIA_ButtonControlTypeId, Name(), rc);
  {
    uia::Node& n = t[i];
    n.accelerator = uia::SplitShortcut(tooltip).second;
    n.enabled = enabled;
    n.status = palette_.NameOf(color, automatic);
    n.focusable = true;
    n.focus = [this] { TakeFocus(); };
    n.focused = Focused() && !popup_.open;
    n.expand = popup_.open ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
    n.doExpand = [this](bool e) {
      TakeFocus();
      SetOpen(e);
    };
  }
  if (popup_.open) palette_.DescribeAsPart(t, i, 1, Focused());
}

// ---------------------------------------------------------------------------
// SplitButton

SplitButton::SplitButton() {
  list_.inPopup = true;
  list_.rowHeight = 30;
  list_.onActivate = [this](int i) {
    SetOpen(false);
    if (i < 0 || i >= static_cast<int>(items.size())) return;
    selected = i;
    if (onPick) onPick(i);
  };
  palette_.onPick = [this](D2D1_COLOR_F c, bool isAutomatic) {
    SetOpen(false);
    color = c;
    if (onColor) onColor(c, isAutomatic);
  };
  palette_.beforeDialog = [this] { SetOpen(false); };
}

void SplitButton::SetGroup(ControlGroup* g) {
  Control::SetGroup(g);
  list_.SetGroup(g);
  palette_.SetGroup(g);
}

// With nothing to list it's a plain button: no arrow.
D2D1_RECT_F SplitButton::ArrowRect() const {
  return RectF(colors || !items.empty() ? rc.right - 18 * Ui() : rc.right, rc.top, rc.right, rc.bottom);
}

D2D1_RECT_F SplitButton::MainRect() const { return RectF(rc.left, rc.top, ArrowRect().left, rc.bottom); }

SplitButton::Part SplitButton::PartAt(float x, float y) const {
  if (!Hit(x, y)) return kNone;
  return Contains(ArrowRect(), x, y) ? kArrow : kMain;
}

void SplitButton::Invoke() {
  if (enabled && onInvoke) onInvoke();
}

void SplitButton::SetOpen(bool open) {
  if (open == popup_.open) return;
  if (!open) {
    popup_.Close();
    inPopup_ = false;
    return;
  }
  if (!enabled || (!colors && items.empty())) return;
  Opened();
  const float u = Ui();
  if (colors) {
    palette_.label = Name();
    palette_.theme = theme;
    palette_.automaticLabel = automaticLabel;
    palette_.current = color;
    palette_.currentAutomatic = false;
    D2D1_SIZE_F size = palette_.PreferredSize();
    popup_.Place(rc, size.width + 8 * u, size.height + 8 * u, PopupBounds(), u);
    palette_.rc = popup_.Inner(u);
    palette_.Layout();
    palette_.HighlightCurrent();
  } else {
    list_.items = items;
    list_.label = Name();
    list_.SetSelected(selected);
    list_.SetHighlight(selected >= 0 ? selected : 0);
    float rows = static_cast<float>(std::min<size_t>(items.size(), 12));
    popup_.Place(rc, std::max(Width(rc), list_.PreferredWidth() + 8 * u), rows * list_.rowHeight * u + 8 * u,
                 PopupBounds(), u);
    list_.rc = popup_.Inner(u);
    list_.ResetScroll();
    list_.EnsureVisible(selected);
  }
  popup_.Open();
}

bool SplitButton::HitPopup(float x, float y) const { return popup_.Hit(x, y); }

void SplitButton::Draw(Canvas& c) {
  const Palette& p = theme::P();
  const float u = Ui();
  const D2D1_RECT_F main = MainRect(), arrow = ArrowRect();
  const float mh = enabled ? mainHover_.V() : 0, ah = enabled ? arrowHover_.V() : 0;
  // Hovering either part shows both, the hovered one stronger, with a line between them.
  const float any = std::max(mh, ah);
  c.FillRound(rc, 4 * u, Alpha(p.hover, any * 0.6f));
  c.PushClip(main);
  c.FillRound(rc, 4 * u, Alpha(p.hover, mh * 0.6f));
  c.FillRound(rc, 4 * u, Alpha(p.pressed, enabled ? mainPress_.V() : 0));
  c.PopClip();
  c.PushClip(arrow);
  c.FillRound(rc, 4 * u, Alpha(p.hover, ah * 0.6f));
  c.FillRound(rc, 4 * u, Alpha(p.pressed, enabled ? std::max(arrowPress_.V(), popup_.open ? 1.f : 0.f) : 0));
  c.PopClip();
  if ((any > 0.01f || popup_.open) && arrow.left < rc.right)
    c.Fill(RectF(arrow.left, rc.top + 6 * u, arrow.left + 1, rc.bottom - 6 * u), Alpha(p.textSecondary, 0.3f * std::max(any, popup_.open ? 1.f : 0.f)));

  const D2D1_COLOR_F fg = enabled ? p.text : p.textDisabled;
  float textW = text.empty() ? 0 : c.MeasureText(text, 13 * u);
  float glyphW = glyph ? 16 * u : 0, gap = (glyph && !text.empty()) ? 6 * u : 0;
  float x = std::round((main.left + main.right - glyphW - gap - textW) / 2);
  float cy = std::round((rc.top + rc.bottom) / 2);
  if (glyph) {
    float gy = showColor ? cy - 3 * u : cy;
    DrawGlyph(c, glyph, RectF(x, gy - 10 * u, x + glyphW, gy + 10 * u), fg, 16 * u);
    if (showColor) {
      D2D1_RECT_F bar = RectF(x, cy + 6 * u, x + glyphW, cy + 6 * u + std::max(2.f, std::round(3 * u)));
      if (color.a > 0) c.Fill(bar, enabled ? color : Alpha(color, 0.4f));
      else c.Stroke(bar, Alpha(fg, 0.5f), 1);
    }
    x += glyphW + gap;
  }
  if (!text.empty()) c.Text(text, RectF(x, rc.top, x + textW + 2, rc.bottom), fg, 13 * u);
  if (arrow.left < rc.right) ctl::DrawChevron(c, arrow, enabled ? p.textSecondary : p.textDisabled, u);
  if (FocusVisible()) ctl::DrawFocusRing(c, rc, u);
}

void SplitButton::DrawPopup(Canvas& c) {
  if (!popup_.Visible()) return;
  popup_.Begin(c, Ui());
  Content().Draw(c);
  popup_.End(c);
}

bool SplitButton::OnMouseMove(float x, float y) {
  if (inPopup_ || HitPopup(x, y)) return Content().OnMouseMove(x, y);
  Part part = PartAt(x, y);
  bool changed = (part == kMain) != (mainHover_.Target() > 0.5f) || (part == kArrow) != (arrowHover_.Target() > 0.5f);
  HoverTo(mainHover_, part == kMain);
  HoverTo(arrowHover_, part == kArrow);
  return changed;
}

void SplitButton::OnMouseLeave() {
  HoverTo(mainHover_, false);
  HoverTo(arrowHover_, false);
  list_.OnMouseLeave();
  palette_.OnMouseLeave();
}

void SplitButton::OnMouseDown(float x, float y, bool dbl, bool shift) {
  if (HitPopup(x, y)) {
    inPopup_ = true;
    Content().OnMouseDown(x, y, dbl, shift);
    return;
  }
  pressed_ = PartAt(x, y);
  if (pressed_ == kMain) {
    mainPress_.To(1, 40);
  } else if (pressed_ == kArrow) {
    arrowPress_.To(1, 40);
    SetOpen(!popup_.open);
  }
}

void SplitButton::OnMouseUp(float x, float y) {
  mainPress_.To(0, 120);
  arrowPress_.To(0, 120);
  if (inPopup_) {
    inPopup_ = false;
    Content().OnMouseUp(x, y);
    return;
  }
  Part pressed = pressed_;
  pressed_ = kNone;
  if (pressed == kMain && PartAt(x, y) == kMain) {
    SetOpen(false);
    Invoke();
  }
}

bool SplitButton::OnWheel(int delta, float x, float y) {
  return HitPopup(x, y) && !colors && list_.OnWheel(delta, x, y);
}

bool SplitButton::OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt) {
  if (!enabled) return false;
  if (popup_.open) {
    switch (vk) {
      case VK_ESCAPE:
      case VK_F4: SetOpen(false); return true;
      case VK_TAB: SetOpen(false); return false;
      case VK_UP:
        if (alt) {
          SetOpen(false);
          return true;
        }
        break;
      default: break;
    }
    if (vk == VK_RETURN || (!colors && vk == VK_SPACE)) {
      Content().OnKeyDown(VK_RETURN, false, false, false);
      return vk != VK_RETURN;  // picked with Enter: false lets a toolbar hand the keyboard back
    }
    return Content().OnKeyDown(vk, ctrl, shift, alt);
  }
  switch (vk) {
    case VK_RETURN: Invoke(); return false;
    case VK_SPACE: Invoke(); return true;
    case VK_F4:
    case VK_DOWN: SetOpen(true); return true;
    default: return false;
  }
}

bool SplitButton::OnChar(wchar_t ch) { return popup_.open && !colors && list_.OnChar(ch); }

void SplitButton::OnFocus(bool focused) {
  if (!focused) SetOpen(false);
}

std::wstring SplitButton::TooltipAt(float x, float y) const {
  if (HitPopup(x, y)) return colors ? palette_.TooltipAt(x, y) : L"";
  return PartAt(x, y) == kArrow ? L"More options" : tooltip;
}

void SplitButton::Describe(uia::Tree& t, int parent) {
  const std::wstring keys = uia::SplitShortcut(tooltip).second;
  auto expand = [this](bool e) {
    TakeFocus();
    SetOpen(e);
  };
  int i = t.Add(parent, id, UIA_SplitButtonControlTypeId, Name(), rc);
  {
    uia::Node& n = t[i];
    n.accelerator = keys;
    n.enabled = enabled;
    n.focusable = true;
    n.focus = [this] { TakeFocus(); };
    n.focused = Focused() && !popup_.open;
    n.invoke = [this] { Invoke(); };
    n.expand = popup_.open ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
    n.doExpand = expand;
    if (showColor) n.status = palette_.NameOf(color, false);
  }
  int m = t.Add(i, 1, UIA_ButtonControlTypeId, Name(), MainRect());
  t[m].enabled = enabled;
  t[m].accelerator = keys;
  t[m].invoke = [this] { Invoke(); };
  int a = t.Add(i, 2, UIA_ButtonControlTypeId, L"More options", ArrowRect());
  t[a].enabled = enabled;
  t[a].expand = popup_.open ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
  t[a].doExpand = expand;
  if (colors) {
    if (popup_.open) palette_.DescribeAsPart(t, i, 3, Focused());
  } else {
    if (!popup_.open) {
      list_.items = items;
      list_.label = Name();
      list_.SetSelected(selected);
    }
    list_.DescribeAsPart(t, i, 3, popup_.open, Focused() && popup_.open, [this](int k) {
      list_.onActivate(k);
    });
  }
}
