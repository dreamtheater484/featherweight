#include "ui/WidgetGallery.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

#include "core/Strings.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Toolbar.h"

using namespace gfx;

namespace {

// The gallery's toolbar commands, outside the ranges in Commands.h.
enum GalleryCmd : int { kStyle = 9001, kFont, kSize, kBold, kFontColor, kHighlight, kSpacing, kBullets };

constexpr float kMargin = 24;   // around the content
constexpr float kMaxW = 1052;   // widest the content gets
constexpr float kHeadH = 96;    // title, hint and last change
constexpr float kGap = 16;      // between cards
constexpr float kCardH = 226;
constexpr float kRowH = 40;
constexpr float kLabelW = 112;

const std::vector<std::wstring>& Styles() {
  static const std::vector<std::wstring> v = {L"Normal",  L"No Spacing", L"Heading 1", L"Heading 2",     L"Heading 3",
                                              L"Title",   L"Subtitle",   L"Quote",     L"List Paragraph"};
  return v;
}

// A fixed list, so the checks look the same on every PC.
const std::vector<std::wstring>& Fonts() {
  static const std::vector<std::wstring> v = {
      L"Arial",       L"Bahnschrift", L"Calibri",  L"Cambria",  L"Candara",         L"Comic Sans MS",
      L"Consolas",    L"Constantia",  L"Corbel",   L"Courier New", L"Georgia",      L"Segoe UI",
      L"Tahoma",      L"Times New Roman", L"Trebuchet MS", L"Verdana"};
  return v;
}

const std::vector<std::wstring>& Sizes() {
  static const std::vector<std::wstring> v = {L"8",  L"9",  L"10", L"10.5", L"11", L"12", L"14", L"16", L"18",
                                              L"20", L"22", L"24", L"26",   L"28", L"36", L"48", L"72"};
  return v;
}

const std::vector<std::wstring>& Bullets() {
  static const std::vector<std::wstring> v = {L"Bullet", L"Hollow bullet", L"Square", L"Check mark", L"Arrow", L"Dash"};
  return v;
}

const std::vector<std::wstring>& Files() {
  static const std::vector<std::wstring> v = {
      L"Agenda for March.docx",  L"Annual report 2025.docx", L"Board minutes.docx",     L"Budget 2026.xlsx",
      L"Business plan.docx",     L"Contract for the lease.pdf", L"Cover letter.docx",  L"Customer list.xlsx",
      L"Design review.pptx",     L"Expenses Q1.xlsx",        L"Expenses Q2.xlsx",       L"Floor plan.pdf",
      L"Holiday schedule.xlsx",  L"Invoice 1042.pdf",        L"Invoice 1043.pdf",       L"Kickoff deck.pptx",
      L"Letter to the council.docx", L"Meeting notes.docx",  L"Newsletter, spring.docx", L"Onboarding guide.pdf",
      L"Price list.xlsx",        L"Project timeline.xlsx",   L"Quarterly review.pptx",  L"Recipe collection.docx",
      L"Report draft.docx",      L"Sales forecast.xlsx",     L"School form.pdf",        L"Team roster.xlsx",
      L"Thesis chapter 3.docx",  L"Travel itinerary.pdf",    L"Weekly status.docx",     L"Year in review.pptx"};
  return v;
}

// Font sizes as Word takes them: 1 to 1638 points, in half points ("12 pt" and "10,5" too).
bool TidySize(std::wstring& text) {
  std::wstring s = fw::Trim(text);
  if (fw::EndsWithNoCase(s, L"pt")) s = fw::Trim(s.substr(0, s.size() - 2));
  for (wchar_t& ch : s)
    if (ch == L',') ch = L'.';
  wchar_t* end = nullptr;
  double v = wcstod(s.c_str(), &end);
  if (s.empty() || !end || *end != 0 || !(v >= 1 && v <= 1638)) return false;
  v = std::round(v * 2) / 2;
  wchar_t buf[32];
  swprintf(buf, 32, v == std::floor(v) ? L"%.0f" : L"%.1f", v);
  text = buf;
  return true;
}

}  // namespace

WidgetGallery::WidgetGallery(ViewHost& host) : host_(host) {
  style_.label = L"Style";
  style_.tooltip = L"Paragraph style";
  style_.items = Styles();
  style_.selected = 0;
  style_.onChange = [this](int i) { Note(L"Style set to " + style_.items[i]); };

  disabled_.label = L"Disabled example";
  disabled_.tooltip = L"Not available here";
  disabled_.items = {L"Not available"};
  disabled_.selected = 0;
  disabled_.enabled = false;

  font_.label = L"Font";
  font_.tooltip = L"Font (Ctrl+Shift+F)";
  font_.items = Fonts();
  font_.SetText(L"Segoe UI");
  font_.onCommit = [this](const std::wstring& text, int) { Note(L"Font set to " + text); };

  size_.label = L"Font size";
  size_.tooltip = L"Font size (Ctrl+Shift+P)";
  size_.items = Sizes();
  size_.autoComplete = false;
  size_.SetText(L"11");
  size_.validate = TidySize;
  size_.onCommit = [this](const std::wstring& text, int) { Note(L"Font size set to " + text); };

  fontColor_.label = L"Font color";
  fontColor_.tooltip = L"Font color";
  fontColor_.glyph = icon::FontColor;
  fontColor_.colors = fontColor_.showColor = true;
  fontColor_.color = Rgb(0xC00000);
  fontColor_.automaticLabel = L"Automatic";
  fontColor_.onInvoke = [this] { Note(L"Applied font color " + ColorName(fontColor_.color, false, L"")); };
  fontColor_.onColor = [this](D2D1_COLOR_F c, bool automatic) {
    Note(L"Font color set to " + ColorName(c, automatic, L"Automatic"));
  };

  bullets_.label = L"Bullets";
  bullets_.tooltip = L"Bullets";
  bullets_.glyph = icon::BulletedList;
  bullets_.items = Bullets();
  bullets_.selected = 0;
  bullets_.onInvoke = [this] { Note(L"Applied bullets: " + bullets_.items[bullets_.selected]); };
  bullets_.onPick = [this](int i) { Note(L"Bullet style set to " + bullets_.items[i]); };

  highlight_.label = L"Highlight";
  highlight_.tooltip = L"Text highlight color";
  highlight_.glyph = icon::Highlight;
  highlight_.color = Rgb(0xFFFF00);
  highlight_.automaticLabel = L"No color";
  highlight_.automaticColor = D2D1::ColorF(0, 0, 0, 0);
  highlight_.onChange = [this](D2D1_COLOR_F c, bool automatic) {
    Note(L"Highlight set to " + ColorName(c, automatic, L"No color"));
  };

  pageColor_.label = L"Page color";
  pageColor_.tooltip = L"Page color";
  pageColor_.color = Rgb(0xFFFFFF);
  pageColor_.automatic = true;
  pageColor_.automaticLabel = L"Automatic";
  pageColor_.automaticColor = Rgb(0xFFFFFF);
  pageColor_.onChange = [this](D2D1_COLOR_F c, bool automatic) {
    Note(L"Page color set to " + ColorName(c, automatic, L"Automatic"));
  };

  auto spin = [this](SpinBox& s, const wchar_t* name, double v, double lo, double hi, double step, int decimals,
                     const wchar_t* unit) {
    s.label = s.tooltip = name;
    s.minValue = lo;
    s.maxValue = hi;
    s.step = step;
    s.decimals = decimals;
    s.unit = unit;
    s.SetValue(v);
    s.onChange = [this, &s, name](double) { Note(std::wstring(name) + L" set to " + s.Format(s.value)); };
  };
  spin(before_, L"Spacing before", 0, 0, 1584, 6, 0, L" pt");
  spin(lineSpacing_, L"Line spacing", 1.15, 0.25, 10, 0.25, 2, L"");
  spin(columns_, L"Columns", 2, 1, 12, 1, 0, L"");

  files_.label = L"Files";
  files_.tooltip = L"Files";
  files_.items = Files();
  files_.onSelect = [this](int i) { Note(L"Selected " + files_.items[i]); };
  files_.onActivate = [this](int i) { Note(L"Opened " + files_.items[i]); };

  cards_ = {
      {L"Dropdown", L"Click, Alt+Down or F4 opens it. Letters jump.", {}, {{L"Style", &style_}, {L"Disabled", &disabled_}}},
      {L"Combo box", L"Type and it completes. Alt+Down lists.", {}, {{L"Font", &font_}, {L"Size", &size_}}},
      {L"Split button", L"The left part applies; the arrow chooses.", {}, {{L"Font color", &fontColor_}, {L"Bullets", &bullets_}}},
      {L"Color picker", L"Theme, standard and recent colors.", {}, {{L"Highlight", &highlight_}, {L"Page color", &pageColor_}}},
      {L"Spin box", L"Up and Down step. Any number can be typed.", {},
       {{L"Spacing before", &before_}, {L"Line spacing", &lineSpacing_}, {L"Columns", &columns_}}},
      {L"List", L"Arrows, Home and End, or type a name.", {}, {{L"", &files_}}},
  };
  // Tab order follows the cards.
  int id = 1;
  for (Card& card : cards_)
    for (auto& [label, control] : card.rows) {
      control->id = id++;
      group_.Add(control);
    }
}

std::wstring WidgetGallery::ColorName(D2D1_COLOR_F c, bool automatic, const std::wstring& automaticLabel) const {
  ColorPalette palette;
  palette.automaticLabel = automaticLabel;
  palette.Layout();
  return palette.NameOf(c, automatic);
}

void WidgetGallery::Note(const std::wstring& what) {
  last_ = what;
  host_.ViewInvalidate();
}

void WidgetGallery::SetViewport(const RECT& rc, float ui) {
  vp_ = rc;
  ui_ = ui;
  const float u = ui;
  group_.ui = ui;
  group_.hwnd = host_.ViewHwnd();
  group_.bounds = RectF(rc);
  const float w = std::min(static_cast<float>(rc.right - rc.left) - 2 * kMargin * u, kMaxW * u);
  const float left = std::round((rc.left + rc.right) / 2.0f - w / 2);
  const float cardW = std::floor((w - 2 * kGap * u) / 3);
  const float top = rc.top + kHeadH * u;
  status_ = RectF(left, rc.top + 66 * u, left + w, rc.top + 86 * u);
  for (size_t i = 0; i < cards_.size(); ++i) {
    Card& card = cards_[i];
    const float x = std::round(left + (i % 3) * (cardW + kGap * u));
    const float y = std::round(top + (i / 3) * (kCardH + kGap) * u);
    card.rc = RectF(x, y, x + cardW, y + kCardH * u);
    float cy = y + 68 * u;
    for (auto& [label, c] : card.rows) {
      if (c == &files_) {
        c->rc = RectF(x + 16 * u, y + 64 * u, x + cardW - 16 * u, y + kCardH * u - 16 * u);
        continue;
      }
      const float cx = x + (16 + kLabelW) * u;
      const float room = x + cardW - 16 * u - cx;
      float cw = std::min(room, 184 * u);
      if (c == &size_) cw = 80 * u;
      else if (c == &fontColor_ || c == &bullets_ || c == &highlight_) cw = 52 * u;
      else if (c == &pageColor_) cw = 76 * u;
      else if (c == &before_ || c == &lineSpacing_ || c == &columns_) cw = std::min(room, 120 * u);
      c->rc = RectF(std::round(cx), std::round(cy), std::round(cx + cw), std::round(cy + 32 * u));
      cy += kRowH * u;
    }
  }
}

void WidgetGallery::Draw(Canvas& c, ID2D1DeviceContext*) {
  const Palette& p = theme::P();
  const float u = ui_;
  const D2D1_RECT_F all = RectF(vp_);
  c.Fill(all, p.docBg);
  c.PushClip(all);
  const float left = status_.left, right = status_.right;
  c.Text(L"Widget gallery", RectF(left, vp_.top + 16 * u, right, vp_.top + 46 * u), p.text, 20 * u,
         DWRITE_FONT_WEIGHT_SEMI_BOLD);
  c.Text(L"Every control works with the mouse and the keyboard. Tab and Shift+Tab move between them.",
         RectF(left, vp_.top + 46 * u, right, vp_.top + 66 * u), p.textSecondary, 13 * u);
  c.Text(L"Last change: " + last_, status_, p.textSecondary, 13 * u);
  for (const Card& card : cards_) {
    c.FillRound(card.rc, 8 * u, p.chrome);
    c.StrokeRound(card.rc, 8 * u, p.dark ? Rgb(0xFFFFFF, 0.07f) : Rgb(0x000000, 0.08f), 1);
    c.Text(card.title, RectF(card.rc.left + 16 * u, card.rc.top + 12 * u, card.rc.right - 16 * u, card.rc.top + 34 * u),
           p.text, 14 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Text(card.hint, RectF(card.rc.left + 16 * u, card.rc.top + 34 * u, card.rc.right - 16 * u, card.rc.top + 54 * u),
           p.textSecondary, 12 * u);
    for (const auto& [label, control] : card.rows) {
      if (label.empty()) continue;
      c.Text(label, RectF(card.rc.left + 16 * u, control->rc.top, control->rc.left - 8 * u, control->rc.bottom),
             control->enabled ? p.textSecondary : p.textDisabled, 13 * u);
    }
  }
  group_.Draw(c);
  group_.DrawPopup(c);
  c.PopClip();
}

void WidgetGallery::OnDeactivated() {
  group_.Blur();
  lastFocused_ = nullptr;
}

void WidgetGallery::OnMouseDown(int x, int y, int button, UINT mods, bool doubleClick) {
  if (button != 0) return;
  lastFocused_ = nullptr;  // the click decides where the keyboard goes
  group_.OnMouseDown((float)x, (float)y, doubleClick, (mods & kModShift) != 0);
  host_.ViewInvalidate();
}

void WidgetGallery::OnMouseMove(int x, int y, UINT) {
  if (group_.OnMouseMove((float)x, (float)y)) host_.ViewInvalidate();
}

void WidgetGallery::OnMouseUp(int x, int y, int button, UINT) {
  if (button != 0) return;
  group_.OnMouseUp((float)x, (float)y);
  host_.ViewInvalidate();
}

void WidgetGallery::OnMouseLeave() {
  group_.OnMouseLeave();
  host_.ViewInvalidate();
}

void WidgetGallery::OnWheel(int delta, bool horizontal, UINT, int x, int y) {
  if (!horizontal && group_.OnWheel(delta, (float)x, (float)y)) host_.ViewInvalidate();
}

bool WidgetGallery::OnKeyDown(UINT vk, UINT mods) {
  const bool ctrl = (mods & kModCtrl) != 0, shift = (mods & kModShift) != 0, alt = (mods & kModAlt) != 0;
  if (!group_.Focused()) {
    if (vk != VK_TAB) return false;
    group_.MoveFocus(!shift);
    return true;
  }
  bool used = group_.OnKeyDown(vk, ctrl, shift, alt);
  host_.ViewInvalidate();
  // The controls have the keyboard; only an unused Esc goes on to the window.
  return used || vk != VK_ESCAPE;
}

bool WidgetGallery::OnChar(wchar_t ch, UINT) {
  if (!group_.Focused()) return false;
  group_.OnChar(ch);
  return true;
}

HCURSOR WidgetGallery::CursorAt(int x, int y) {
  static HCURSOR arrow = LoadCursor(nullptr, IDC_ARROW), ibeam = LoadCursor(nullptr, IDC_IBEAM);
  return group_.OverText((float)x, (float)y) ? ibeam : arrow;
}

std::wstring WidgetGallery::TooltipAt(int x, int y) { return group_.TooltipAt((float)x, (float)y); }

void WidgetGallery::SetKeyFocus(bool has) {
  if (!has) {
    if (group_.Focused()) lastFocused_ = group_.Focused();
    group_.Blur();
  } else if (!group_.Focused() && lastFocused_) {
    group_.Focus(lastFocused_, false);
  }
}

CommandState WidgetGallery::QueryCommand(int cmd) {
  switch (cmd) {
    case kBold: return {true, tbBold_};
    case kFontColor:
    case kBullets: return {true, false};
    default: return {};
  }
}

bool WidgetGallery::Execute(int cmd) {
  switch (cmd) {
    case kBold:
      tbBold_ = !tbBold_;
      Note(tbBold_ ? L"Bold on" : L"Bold off");
      return true;
    case kFontColor: Note(L"Applied font color " + ColorName(tbFontColor_, false, L"")); return true;
    case kBullets: Note(L"Applied bullets: " + Bullets()[tbBullet_]); return true;
    default: return false;
  }
}

void WidgetGallery::BuildToolbar(ToolbarModel& m, const ToolbarContext&) {
  ToolItem style;
  style.kind = ToolKind::Dropdown;
  style.cmd = kStyle;
  style.tooltip = L"Style";
  style.width = 132;
  style.choices = Styles();
  style.choice = tbStyle_;
  style.onChange = [this](const ToolValue& v) {
    tbStyle_ = v.index;
    Note(L"Toolbar style set to " + v.text);
  };

  ToolItem font;
  font.kind = ToolKind::ComboBox;
  font.cmd = kFont;
  font.tooltip = L"Font (Ctrl+Shift+F)";
  font.width = 150;
  font.choices = Fonts();
  font.text = tbFont_;
  font.onChange = [this](const ToolValue& v) {
    tbFont_ = v.text;
    Note(L"Toolbar font set to " + v.text);
  };

  ToolItem size;
  size.kind = ToolKind::ComboBox;
  size.cmd = kSize;
  size.tooltip = L"Font size (Ctrl+Shift+P)";
  size.width = 64;
  size.choices = Sizes();
  size.text = tbSize_;
  size.validate = TidySize;
  size.onChange = [this](const ToolValue& v) {
    tbSize_ = v.text;
    Note(L"Toolbar font size set to " + v.text);
  };

  ToolItem bold;
  bold.cmd = kBold;
  bold.glyph = icon::Bold;
  bold.tooltip = L"Bold (Ctrl+B)";
  bold.toggle = true;
  bold.checked = tbBold_;

  ToolItem color;
  color.kind = ToolKind::SplitButton;
  color.cmd = kFontColor;
  color.glyph = icon::FontColor;
  color.tooltip = L"Font color";
  color.width = 52;
  color.colors = true;
  color.color = tbFontColor_;
  color.automaticLabel = L"Automatic";
  color.onChange = [this](const ToolValue& v) {
    tbFontColor_ = v.color;
    Note(L"Toolbar font color set to " + ColorName(v.color, v.automatic, L"Automatic"));
  };

  ToolItem highlight;
  highlight.kind = ToolKind::ColorPicker;
  highlight.cmd = kHighlight;
  highlight.glyph = icon::Highlight;
  highlight.tooltip = L"Text highlight color";
  highlight.width = 52;
  highlight.color = tbHighlight_;
  highlight.automatic = tbNoHighlight_;
  highlight.automaticLabel = L"No color";
  highlight.onChange = [this](const ToolValue& v) {
    tbHighlight_ = v.color;
    tbNoHighlight_ = v.automatic;
    Note(L"Toolbar highlight set to " + ColorName(v.color, v.automatic, L"No color"));
  };

  ToolItem spacing;
  spacing.kind = ToolKind::SpinBox;
  spacing.cmd = kSpacing;
  spacing.tooltip = L"Spacing after";
  spacing.width = 96;
  spacing.value = tbAfter_;
  spacing.minValue = 0;
  spacing.maxValue = 1584;
  spacing.step = 6;
  spacing.unit = L" pt";
  spacing.onChange = [this](const ToolValue& v) {
    tbAfter_ = v.number;
    Note(L"Toolbar spacing after set to " + std::to_wstring(static_cast<int>(v.number)) + L" pt");
  };
  m.left = {style, font, size, bold, color, highlight, spacing};

  ToolItem bullets;
  bullets.kind = ToolKind::SplitButton;
  bullets.cmd = kBullets;
  bullets.glyph = icon::BulletedList;
  bullets.tooltip = L"Bullets";
  bullets.width = 52;
  bullets.choices = Bullets();
  bullets.choice = tbBullet_;
  bullets.sepBefore = true;
  bullets.priority = 5;
  bullets.onChange = [this](const ToolValue& v) {
    tbBullet_ = v.index;
    Note(L"Toolbar bullet style set to " + Bullets()[v.index]);
  };
  m.right = {bullets};
}

bool WidgetGallery::Save(const std::wstring&, std::wstring& error) {
  error = L"The widget gallery has nothing to save.";
  return false;
}

void WidgetGallery::DescribeUia(uia::Tree& t, int parent) {
  // Focusing the document itself lets go of the controls.
  t[parent].focus = [this] {
    group_.Blur();
    lastFocused_ = nullptr;
  };
  int status = t.Add(parent, 1, UIA_TextControlTypeId, L"Last change: " + last_, status_);
  t[status].live = true;
  for (size_t i = 0; i < cards_.size(); ++i) {
    const Card& card = cards_[i];
    int g = t.Add(parent, 10 + static_cast<int64_t>(i), UIA_GroupControlTypeId, card.title, card.rc);
    t[g].help = card.hint;
    for (const auto& [label, control] : card.rows)
      if (control->visible) control->Describe(t, g);
  }
}
