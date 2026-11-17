#include "sheet/view/WorkbookFormat.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <unordered_map>

#include "Commands.h"
#include "core/Strings.h"
#include "sheet/numfmt/NumFormat.h"
#include "sheet/numfmt/NumLocale.h"
#include "ui/Gfx.h"
#include "ui/Theme.h"
#include "ui/Uia.h"
#include "word/DocFormat.h"  // the installed fonts' names

using namespace gfx;

namespace fw::sheet::view {

namespace {

const wchar_t* const kCategories[] = {L"General", L"Number",   L"Currency", L"Accounting", L"Date", L"Time",
                                      L"Percentage", L"Fraction", L"Scientific", L"Text", L"Custom"};
const wchar_t* const kFractions[] = {L"Up to one digit (1/4)",  L"Up to two digits (21/25)", L"Up to three digits (312/943)",
                                     L"As halves (1/2)",        L"As quarters (2/4)",        L"As eighths (4/8)",
                                     L"As sixteenths (8/16)",   L"As tenths (3/10)",         L"As hundredths (30/100)"};
const wchar_t* const kHorizontal[] = {L"General", L"Left",    L"Center", L"Right", L"Fill", L"Justify",
                                      L"Center across selection", L"Distributed"};
const wchar_t* const kVertical[] = {L"Bottom", L"Top", L"Center", L"Justify", L"Distributed"};
const wchar_t* const kStyles[] = {L"Regular", L"Italic", L"Bold", L"Bold italic"};
const wchar_t* const kUnderlines[] = {L"None", L"Single", L"Double", L"Single accounting", L"Double accounting"};
const wchar_t* const kSizes[] = {L"8",  L"9",  L"10", L"11", L"12", L"14", L"16", L"18",
                                 L"20", L"22", L"24", L"26", L"28", L"36", L"48", L"72"};
const wchar_t* const kBorderButtons[] = {L"None", L"Outline", L"All", L"Inside", L"Top", L"Bottom", L"Left", L"Right"};

struct LineChoice {
  BorderStyle style;
  const wchar_t* name;
};
const LineChoice kLines[] = {
    {BorderStyle::Thin, L"Thin"},           {BorderStyle::Hair, L"Hair"},
    {BorderStyle::Dotted, L"Dotted"},       {BorderStyle::Dashed, L"Dashed"},
    {BorderStyle::DashDot, L"Dash dot"},    {BorderStyle::DashDotDot, L"Dash dot dot"},
    {BorderStyle::Double, L"Double"},       {BorderStyle::Medium, L"Medium"},
    {BorderStyle::MediumDashed, L"Medium dashed"}, {BorderStyle::MediumDashDot, L"Medium dash dot"},
    {BorderStyle::MediumDashDotDot, L"Medium dash dot dot"}, {BorderStyle::SlantDashDot, L"Slanted dash dot"},
    {BorderStyle::Thick, L"Thick"}};

struct PatternChoice {
  PatternType type;
  const wchar_t* name;
};
const PatternChoice kPatterns[] = {
    {PatternType::None, L"None"},
    {PatternType::Solid, L"Solid"},
    {PatternType::DarkGray, L"75% gray"},
    {PatternType::MediumGray, L"50% gray"},
    {PatternType::LightGray, L"25% gray"},
    {PatternType::Gray125, L"12.5% gray"},
    {PatternType::Gray0625, L"6.25% gray"},
    {PatternType::DarkHorizontal, L"Horizontal stripe"},
    {PatternType::DarkVertical, L"Vertical stripe"},
    {PatternType::DarkDown, L"Reverse diagonal stripe"},
    {PatternType::DarkUp, L"Diagonal stripe"},
    {PatternType::DarkGrid, L"Diagonal crosshatch"},
    {PatternType::DarkTrellis, L"Thick diagonal crosshatch"},
    {PatternType::LightHorizontal, L"Thin horizontal stripe"},
    {PatternType::LightVertical, L"Thin vertical stripe"},
    {PatternType::LightDown, L"Thin reverse diagonal stripe"},
    {PatternType::LightUp, L"Thin diagonal stripe"},
    {PatternType::LightGrid, L"Thin horizontal crosshatch"},
    {PatternType::LightTrellis, L"Thin diagonal crosshatch"}};

// Excel's sample for dates and times in Format Cells: 14 March 2012, 13:30:55.
constexpr double kSampleDate = 40982.563136574;

std::vector<std::wstring> List(const wchar_t* const* items, size_t n) { return std::vector<std::wstring>(items, items + n); }

D2D1_COLOR_F ToD2D(uint32_t argb) {
  return D2D1_COLOR_F{static_cast<float>((argb >> 16) & 255) / 255.f, static_cast<float>((argb >> 8) & 255) / 255.f,
                      static_cast<float>(argb & 255) / 255.f, 1.f};
}

Color FromD2D(D2D1_COLOR_F c, bool automatic) {
  Color out;
  if (automatic) return out;
  auto b = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
  out.kind = Color::Kind::Rgb;
  out.value = 0xFF000000u | b(c.r) << 16 | b(c.g) << 8 | b(c.b);
  return out;
}

void SetColor(ColorPicker& p, const Styles& st, const Color& c, uint32_t fallback) {
  p.automatic = !c.IsSet() || c.kind == Color::Kind::Auto;
  p.color = ToD2D(st.Resolve(c, fallback));
}

// A built-in format's id for a code as files define the built-ins (en-US, invariant); -1 for none.
int BuiltinId(const std::wstring& code) {
  const NumLocale& us = EnUsLocale();
  for (size_t i = 0; i < us.builtin.size(); ++i)
    if (!us.builtin[i].empty() && us.builtin[i] == code) return static_cast<int>(i);
  return -1;
}

std::wstring PointsText(double size) {
  wchar_t buf[32];
  std::swprintf(buf, 32, L"%g", size);
  return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// The panel's button

void PanelButton::Draw(Canvas& c) {
  if (!visible) return;
  const Palette& p = theme::P();
  const float u = Ui();
  D2D1_COLOR_F bg = pressed_ ? p.pressed : hover_ ? p.hover : checked ? p.selection : p.inputBg;
  if (!enabled) bg = Alpha(bg, 0.5f);
  c.FillRound(rc, 4 * u, bg);
  c.StrokeRound(rc, 4 * u, checked ? p.accentText : p.inputBorder, 1);
  const D2D1_COLOR_F fg = !enabled ? p.textDisabled : p.text;
  const float w = c.MeasureText(text, textSize * u);
  const float x = std::max(rc.left + 3 * u, std::round((rc.left + rc.right - w) / 2));
  c.Text(text, RectF(x, rc.top, x + w + 2 * u, rc.bottom), fg, textSize * u);
  if (FocusVisible()) ctl::DrawFocusRing(c, rc, u);
}

bool PanelButton::OnMouseMove(float x, float y) {
  const bool h = enabled && Hit(x, y);
  const bool changed = h != hover_;
  hover_ = h;
  return changed;
}

void PanelButton::OnMouseDown(float x, float y, bool, bool) {
  pressed_ = enabled && Hit(x, y);
  if (pressed_) TakeFocus();
}

void PanelButton::OnMouseUp(float x, float y) {
  const bool fire = pressed_ && Hit(x, y);
  pressed_ = false;
  if (fire) Invoke();
}

bool PanelButton::OnKeyDown(UINT vk, bool, bool, bool) {
  if (!enabled || (vk != VK_RETURN && vk != VK_SPACE)) return false;
  Invoke();
  return true;
}

void PanelButton::Invoke() {
  if (!enabled) return;
  if (toggle) checked = !checked;
  if (onInvoke) onInvoke();
}

void PanelButton::Describe(uia::Tree& t, int parent) {
  if (!visible) return;
  const int i = t.Add(parent, id, UIA_ButtonControlTypeId, text, rc);
  t[i].enabled = enabled;
  t[i].focusable = true;
  t[i].focus = [this] { TakeFocus(); };
  t[i].focused = Focused();
  t[i].invoke = [this] { Invoke(); };
  if (toggle) t[i].toggle = checked ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Format Cells

WorkbookView::FormatPanel::FormatPanel(WorkbookView& view) : v_(view) {
  const wchar_t* tabNames[kPages] = {L"Number", L"Align", L"Font", L"Border", L"Fill"};
  for (int i = 0; i < kPages; ++i) {
    tabs_[i].text = tabNames[i];
    tabs_[i].toggle = true;
    tabs_[i].textSize = 12;
    tabs_[i].onInvoke = [this, i] { ShowPage(i); };
  }
  // Number
  category_.label = L"Category";
  category_.items = List(kCategories, std::size(kCategories));
  category_.onChange = [this](int i) {
    if (i < 0) return;
    choice_.category = static_cast<FormatCategory>(i);
    choice_.type = 0;
    if (choice_.category == FormatCategory::Custom) {
      ShowPage(kNumber);  // shows the code box; the format stays until a code is entered
      return;
    }
    ApplyNumber();
  };
  decimals_.label = L"Decimal places";
  decimals_.minValue = 0;
  decimals_.maxValue = 30;
  decimals_.onChange = [this](double d) {
    choice_.decimals = static_cast<int>(d);
    ApplyNumber();
  };
  thousands_.text = L"Use 1000 separator";
  thousands_.toggle = true;
  thousands_.onInvoke = [this] {
    choice_.thousands = thousands_.checked;
    ApplyNumber();
  };
  negative_.label = L"Negative numbers";
  negative_.onChange = [this](int i) {
    choice_.negative = std::max(0, i);
    ApplyNumber();
  };
  types_.label = L"Type";
  types_.rowHeight = 28;
  types_.onSelect = [this](int i) {
    if (i < 0) return;
    choice_.type = i;
    ApplyNumber();
  };
  code_.label = L"Format code";
  code_.autoComplete = false;
  code_.validate = [this](std::wstring& text) {
    std::wstring invariant;
    return NumberFormat::LocalToInvariant(text, v_.text_->Locale(), invariant);
  };
  code_.onCommit = [this](const std::wstring& text, int) {
    std::wstring invariant;
    if (!NumberFormat::LocalToInvariant(text, v_.text_->Locale(), invariant)) return;
    choice_.category = FormatCategory::Custom;
    choice_.code = invariant;
    ApplyNumber();
  };
  // Alignment
  horizontal_.label = L"Horizontal";
  horizontal_.items = List(kHorizontal, std::size(kHorizontal));
  horizontal_.onChange = [this](int i) {
    if (i >= 0)
      v_.ChangeFormat(u"Alignment", [i](Xf& x) {
        x.alignment.horizontal = static_cast<HAlign>(i);
        x.applyAlignment = true;
      });
  };
  vertical_.label = L"Vertical";
  vertical_.items = List(kVertical, std::size(kVertical));
  vertical_.onChange = [this](int i) {
    if (i >= 0)
      v_.ChangeFormat(u"Alignment", [i](Xf& x) {
        x.alignment.vertical = static_cast<VAlign>(i);
        x.applyAlignment = true;
      });
  };
  indent_.label = L"Indent";
  indent_.minValue = 0;
  indent_.maxValue = 250;
  indent_.onChange = [this](double d) {
    const uint16_t n = static_cast<uint16_t>(d);
    v_.ChangeFormat(u"Indent", [n](Xf& x) {
      x.alignment.indent = n;
      // An indent needs a side to count from, as Excel sets it.
      const HAlign h = x.alignment.horizontal;
      if (n && h != HAlign::Left && h != HAlign::Right && h != HAlign::Distributed) x.alignment.horizontal = HAlign::Left;
      x.applyAlignment = true;
    });
  };
  rotation_.label = L"Orientation";
  rotation_.minValue = -90;
  rotation_.maxValue = 90;
  rotation_.unit = L"°";
  rotation_.onChange = [this](double d) {
    const int deg = static_cast<int>(std::lround(d));
    const uint16_t r = static_cast<uint16_t>(deg >= 0 ? deg : 90 - deg);  // clockwise: 91..180
    v_.ChangeFormat(u"Orientation", [r](Xf& x) {
      x.alignment.rotation = r;
      x.applyAlignment = true;
    });
  };
  wrap_.text = L"Wrap text";
  wrap_.toggle = true;
  wrap_.onInvoke = [this] {
    const bool on = wrap_.checked;
    v_.ChangeFormat(u"Wrap Text", [on](Xf& x) {
      x.alignment.wrap = on;
      x.applyAlignment = true;
    });
  };
  shrink_.text = L"Shrink to fit";
  shrink_.toggle = true;
  shrink_.onInvoke = [this] {
    const bool on = shrink_.checked;
    v_.ChangeFormat(u"Shrink to Fit", [on](Xf& x) {
      x.alignment.shrinkToFit = on;
      x.applyAlignment = true;
    });
  };
  merge_.text = L"Merge cells";
  merge_.toggle = true;
  merge_.onInvoke = [this] {
    const Range sel = v_.Selected();
    const int sheet = static_cast<int>(v_.sheet_);
    v_.EditRefused(merge_.checked ? v_.Ed().Merge(sheet, sel) : v_.Ed().Unmerge(sheet, sel));
    Sync();
  };
  // Font
  font_.label = L"Font";
  font_.onCommit = [this](const std::wstring& text, int) {
    if (text.empty()) return;
    const std::string name = WideToUtf8(text);
    v_.ChangeFont(u"Font", [&name](Font& f) {
      f.name = name;
      f.scheme = FontScheme::None;  // a font picked by name no longer follows the theme's
    });
  };
  style_.label = L"Style";
  style_.items = List(kStyles, std::size(kStyles));
  style_.onChange = [this](int i) {
    if (i >= 0)
      v_.ChangeFont(u"Font Style", [i](Font& f) {
        f.bold = i >= 2;
        f.italic = (i & 1) != 0;
      });
  };
  size_.label = L"Size";
  size_.items = List(kSizes, std::size(kSizes));
  size_.autoComplete = false;
  size_.validate = [](std::wstring& text) {
    wchar_t* end = nullptr;
    const double v = std::wcstod(text.c_str(), &end);
    if (end == text.c_str() || !(v >= 1 && v <= 409)) return false;
    text = PointsText(std::round(v * 2) / 2);
    return true;
  };
  size_.onCommit = [this](const std::wstring& text, int) {
    const double s = std::wcstod(text.c_str(), nullptr);
    if (s >= 1) v_.ChangeFont(u"Font Size", [s](Font& f) { f.size = s; });
  };
  underline_.label = L"Underline";
  underline_.items = List(kUnderlines, std::size(kUnderlines));
  underline_.onChange = [this](int i) {
    if (i >= 0) v_.ChangeFont(u"Underline", [i](Font& f) { f.underline = static_cast<Underline>(i); });
  };
  color_.label = L"Color";
  color_.automaticLabel = L"Automatic";
  color_.onChange = [this](D2D1_COLOR_F c, bool automatic) {
    const Color color = FromD2D(c, automatic);
    v_.ChangeFont(u"Font Color", [color](Font& f) { f.color = color; });
  };
  strike_.text = L"Strikethrough";
  strike_.textSize = superscript_.textSize = subscript_.textSize = 12;
  strike_.toggle = true;
  strike_.onInvoke = [this] {
    const bool on = strike_.checked;
    v_.ChangeFont(u"Strikethrough", [on](Font& f) { f.strike = on; });
  };
  superscript_.text = L"Superscript";
  superscript_.toggle = true;
  superscript_.onInvoke = [this] {
    const VertAlign a = superscript_.checked ? VertAlign::Superscript : VertAlign::Baseline;
    v_.ChangeFont(u"Superscript", [a](Font& f) { f.vertAlign = a; });
  };
  subscript_.text = L"Subscript";
  subscript_.toggle = true;
  subscript_.onInvoke = [this] {
    const VertAlign a = subscript_.checked ? VertAlign::Subscript : VertAlign::Baseline;
    v_.ChangeFont(u"Subscript", [a](Font& f) { f.vertAlign = a; });
  };
  // Border
  line_.label = L"Line";
  for (const LineChoice& l : kLines) line_.items.push_back(l.name);
  line_.selected = 0;
  lineColor_.label = L"Line color";
  lineColor_.automaticLabel = L"Automatic";
  lineColor_.automatic = true;
  color_.automaticColor = lineColor_.automaticColor = patternColor_.automaticColor = D2D1_COLOR_F{0, 0, 0, 1};
  for (int i = 0; i < 8; ++i) {
    borders_[i].text = kBorderButtons[i];
    borders_[i].textSize = 12;
    borders_[i].onInvoke = [this, i] { ApplyBorders(i); };
  }
  // Fill
  auto applyFill = [this] {
    const int pi = std::max(0, pattern_.selected);
    const PatternType type = kPatterns[pi].type;
    Fill f;
    if (type == PatternType::Solid || (type == PatternType::None && !fillColor_.automatic)) {
      f.pattern = PatternType::Solid;
      f.fg = FromD2D(fillColor_.color, false);
    } else if (type != PatternType::None) {
      f.pattern = type;
      f.fg = FromD2D(patternColor_.color, patternColor_.automatic);
      if (!f.fg.IsSet()) f.fg.kind = Color::Kind::Auto;
      if (!fillColor_.automatic) f.bg = FromD2D(fillColor_.color, false);
    }
    v_.ChangeFormat(u"Fill", [this, f](Xf& x) {
      x.fill = v_.Ed().AddFill(f);
      x.applyFill = true;
    });
  };
  fillColor_.label = L"Background color";
  fillColor_.automaticLabel = L"No color";
  fillColor_.onChange = [this, applyFill](D2D1_COLOR_F, bool automatic) {
    if (automatic) pattern_.selected = 0;
    else if (pattern_.selected <= 0) pattern_.selected = 1;
    applyFill();
  };
  pattern_.label = L"Pattern style";
  for (const PatternChoice& p : kPatterns) pattern_.items.push_back(p.name);
  pattern_.onChange = [applyFill](int) { applyFill(); };
  patternColor_.label = L"Pattern color";
  patternColor_.automaticLabel = L"Automatic";
  patternColor_.onChange = [applyFill](D2D1_COLOR_F, bool) { applyFill(); };
}

void WorkbookView::FormatPanel::Open(int page) {
  if (DocFormat::FontsPending()) DocFormat::ReadFonts();
  font_.items = DocFormat::FontNames();
  syncXf_ = UINT32_MAX;  // fresh controls
  Sync();
  ShowPage(page >= 0 && page < kPages ? page : page_);
}

void WorkbookView::FormatPanel::ShowPage(int page) {
  page_ = page;
  for (int i = 0; i < kPages; ++i) tabs_[i].checked = i == page;
  Place();
  v_.host_.ViewInvalidate();
}

std::vector<Control*> WorkbookView::FormatPanel::Layout(const D2D1_RECT_F& body, float ui) {
  body_ = body;
  ui_ = ui;
  Place();
  std::vector<Control*> out;
  for (PanelButton& t : tabs_) out.push_back(&t);
  for (Control* c : std::initializer_list<Control*>{&category_, &decimals_, &thousands_, &negative_, &types_, &code_,
                                                    &horizontal_, &vertical_, &indent_, &rotation_, &wrap_, &shrink_,
                                                    &merge_, &font_, &style_, &size_, &underline_, &color_, &strike_,
                                                    &superscript_, &subscript_, &line_, &lineColor_})
    out.push_back(c);
  for (PanelButton& b : borders_) out.push_back(&b);
  for (Control* c : std::initializer_list<Control*>{&fillColor_, &pattern_, &patternColor_}) out.push_back(c);
  return out;
}

void WorkbookView::FormatPanel::Place() {
  const float u = ui_, left = body_.left + 16 * u, right = body_.right - 16 * u, gap = 8 * u;
  float y = body_.top + 8 * u;
  labels_.clear();
  sampleRc_ = {};
  // Tabs along the top.
  const float tw = (right - left - gap * static_cast<float>(kPages - 1)) / static_cast<float>(kPages);
  for (int i = 0; i < kPages; ++i) tabs_[i].rc = RectF(left + static_cast<float>(i) * (tw + gap), y, left + static_cast<float>(i) * (tw + gap) + tw, y + 28 * u);
  y += 40 * u;
  // Every page's controls hidden; the page's rows show theirs.
  for (Control* c : std::initializer_list<Control*>{&category_, &decimals_, &thousands_, &negative_, &types_, &code_,
                                                    &horizontal_, &vertical_, &indent_, &rotation_, &wrap_, &shrink_,
                                                    &merge_, &font_, &style_, &size_, &underline_, &color_, &strike_,
                                                    &superscript_, &subscript_, &line_, &lineColor_, &fillColor_,
                                                    &pattern_, &patternColor_})
    c->visible = false;
  for (PanelButton& b : borders_) b.visible = false;
  // A row of controls side by side, each with its label above (none: nullptr).
  using Labeled = std::pair<const wchar_t*, Control*>;
  auto labeled = [&](std::initializer_list<Labeled> controls, float height = 30) {
    bool any = false;
    for (const Labeled& l : controls) any = any || l.first;
    const float w = (right - left - gap * static_cast<float>(controls.size() - 1)) / static_cast<float>(controls.size());
    float x = left;
    for (const auto& [label, c] : controls) {
      if (label) labels_.push_back({label, RectF(x, y, x + w, y + 16 * u)});
      c->visible = true;
      c->rc = RectF(x, y + (any ? 20 * u : 0), x + w, y + (any ? 20 * u : 0) + height * u);
      x += w + gap;
    }
    y += ((any ? 20 : 0) + height + 10) * u;
  };
  auto row = [&](const wchar_t* label, std::initializer_list<Control*> controls, float height = 30) {
    if (label) {
      labels_.push_back({label, RectF(left, y, right, y + 16 * u)});
      y += 20 * u;
    }
    const float w = (right - left - gap * static_cast<float>(controls.size() - 1)) / static_cast<float>(controls.size());
    float x = left;
    for (Control* c : controls) {
      c->visible = true;
      c->rc = RectF(x, y, x + w, y + height * u);
      x += w + gap;
    }
    y += (height + 10) * u;
  };
  const FormatCategory cat = choice_.category;
  switch (page_) {
    case kNumber: {
      row(L"Category", {&category_});
      labels_.push_back({L"Sample", RectF(left, y, right, y + 18 * u)});
      sampleRc_ = RectF(left, y + 20 * u, right, y + 40 * u);
      y += 52 * u;
      const bool decimals = cat == FormatCategory::Number || cat == FormatCategory::Currency ||
                            cat == FormatCategory::Accounting || cat == FormatCategory::Percentage ||
                            cat == FormatCategory::Scientific;
      if (decimals) row(L"Decimal places", {&decimals_});
      if (cat == FormatCategory::Number) row(nullptr, {&thousands_});
      if (cat == FormatCategory::Number || cat == FormatCategory::Currency) row(L"Negative numbers", {&negative_});
      if (cat == FormatCategory::Date || cat == FormatCategory::Time || cat == FormatCategory::Fraction)
        row(L"Type", {&types_}, std::clamp((body_.bottom - y) / u - 40, 120.f, 330.f));
      if (cat == FormatCategory::Custom) row(L"Format code, as typed in this region", {&code_});
      break;
    }
    case kAlignment:
      labeled({{L"Horizontal", &horizontal_}, {L"Vertical", &vertical_}});
      labeled({{L"Indent", &indent_}, {L"Orientation", &rotation_}});
      row(nullptr, {&wrap_, &shrink_});
      row(nullptr, {&merge_});
      break;
    case kFont:
      row(L"Font", {&font_});
      labeled({{L"Style", &style_}, {L"Size", &size_}});
      labeled({{L"Underline", &underline_}, {L"Color", &color_}});
      row(L"Effects", {&strike_, &superscript_, &subscript_});
      break;
    case kBorder:
      labeled({{L"Line", &line_}, {L"Line color", &lineColor_}});
      row(L"Presets", {&borders_[0], &borders_[1], &borders_[2], &borders_[3]});
      row(L"Edges", {&borders_[4], &borders_[5], &borders_[6], &borders_[7]});
      break;
    case kFill:
      row(L"Background color", {&fillColor_});
      row(L"Pattern style", {&pattern_});
      row(L"Pattern color", {&patternColor_});
      break;
  }
}

void WorkbookView::FormatPanel::Draw(Canvas& c) {
  // The active cell or its format changed (a click on the sheet, undo): follow it.
  if (v_.Ready()) {
    const Range* m = v_.Current().MergeAt(v_.active_.row, v_.active_.col);
    const bool merged = m && (m->lastRow > m->firstRow || m->lastCol > m->firstCol);
    if (v_.sheet_ != syncSheet_ || !(v_.active_ == syncCell_) || v_.ActiveStyle() != syncXf_ || merged != syncMerged_) {
      Sync();
      Place();
    }
  }
  const Palette& p = theme::P();
  for (const auto& [text, rc] : labels_) c.Text(text, rc, p.textSecondary, 12 * ui_);
  if (page_ == kNumber && sampleRc_.right > sampleRc_.left) c.Text(sample_, sampleRc_, p.text, 13 * ui_);
}

std::wstring WorkbookView::FormatPanel::Sample(const std::wstring& code, double value) const {
  const NumLocale& loc = v_.text_->Locale();
  const int builtin = BuiltinId(code);
  const NumberFormat nf = builtin >= 0 ? NumberFormat::Builtin(builtin, loc) : NumberFormat(code);
  FormatContext ctx;
  ctx.locale = &loc;
  ctx.date1904 = v_.book_->Date1904();
  Formatted out;
  nf.Format(value, ctx, out);
  return out.overflow ? L"####" : out.text;
}

void WorkbookView::FormatPanel::SyncNumber() {
  const NumLocale& loc = v_.text_->Locale();
  const Styles& st = v_.book_->GetStyles();
  const Xf& x = st.CellXf(v_.ActiveStyle());
  const std::wstring code = v_.CodeOf(x.numFmt);
  choice_ = FormatChoiceOf(code, loc);
  category_.selected = static_cast<int>(choice_.category);
  decimals_.SetValue(choice_.decimals);
  thousands_.checked = choice_.thousands;
  // The negative numbers' styles, as this format shows -1234.10 (red ones say so).
  negative_.items.clear();
  for (int i = 0; i < 4; ++i) {
    FormatChoice c = choice_;
    if (c.category != FormatCategory::Number && c.category != FormatCategory::Currency) c.category = FormatCategory::Number;
    c.negative = i;
    negative_.items.push_back(Sample(FormatCode(c, loc), -1234.1) + (i & 1 ? L" in red" : L""));
  }
  negative_.selected = choice_.negative;
  // Dates and times by their sample, fractions by name.
  types_.items.clear();
  typeCodes_ = FormatTypes(choice_.category, loc);
  for (size_t i = 0; i < typeCodes_.size(); ++i)
    types_.items.push_back(choice_.category == FormatCategory::Fraction && i < std::size(kFractions)
                               ? std::wstring(kFractions[i])
                               : Sample(typeCodes_[i], kSampleDate));
  types_.SetSelected(choice_.type);
  // Every format the workbook has, as the region types it.
  code_.items.clear();
  for (const auto& [id, c] : st.CustomFormats()) code_.items.push_back(NumberFormat::InvariantToLocal(Utf8ToWide(c), loc));
  std::sort(code_.items.begin(), code_.items.end());
  code_.SetText(NumberFormat::InvariantToLocal(code, loc));
  // The active cell's value in it.
  const Value value = v_.Current().GetValue(v_.active_.row, v_.active_.col);
  sample_ = value.type == ValueType::Number ? Sample(code, value.number)
            : value.type == ValueType::String ? Utf8ToWide(value.text)
                                              : std::wstring();
}

void WorkbookView::FormatPanel::Sync() {
  if (!v_.Ready()) return;
  const Styles& st = v_.book_->GetStyles();
  syncSheet_ = v_.sheet_;
  syncCell_ = v_.active_;
  syncXf_ = v_.ActiveStyle();
  const Range* m = v_.Current().MergeAt(v_.active_.row, v_.active_.col);
  syncMerged_ = m && (m->lastRow > m->firstRow || m->lastCol > m->firstCol);
  const Xf& x = st.CellXf(syncXf_);
  SyncNumber();
  // Alignment
  horizontal_.selected = static_cast<int>(x.alignment.horizontal);
  vertical_.selected = static_cast<int>(x.alignment.vertical);
  indent_.SetValue(x.alignment.indent);
  const uint16_t r = x.alignment.rotation;
  rotation_.SetValue(r <= 90 ? r : r <= 180 ? 90.0 - r : 0);
  wrap_.checked = x.alignment.wrap;
  shrink_.checked = x.alignment.shrinkToFit;
  merge_.checked = syncMerged_;
  // Font
  const Font& f = st.FontOf(x);
  font_.SetText(Utf8ToWide(f.name.empty() ? st.DefaultFont().name : f.name));
  style_.selected = (f.bold ? 2 : 0) + (f.italic ? 1 : 0);
  size_.SetText(PointsText(f.size > 0 ? f.size : st.DefaultFont().size));
  underline_.selected = static_cast<int>(f.underline);
  SetColor(color_, st, f.color, 0xFF000000u);
  strike_.checked = f.strike;
  superscript_.checked = f.vertAlign == VertAlign::Superscript;
  subscript_.checked = f.vertAlign == VertAlign::Subscript;
  // Fill
  const Fill& fill = st.FillOf(x);
  int pi = 0;
  for (size_t i = 0; i < std::size(kPatterns); ++i)
    if (kPatterns[i].type == fill.pattern) pi = static_cast<int>(i);
  pattern_.selected = pi;
  if (fill.pattern == PatternType::Solid) {
    SetColor(fillColor_, st, fill.fg, 0xFFFFFFFFu);
    patternColor_.automatic = true;
  } else if (fill.pattern != PatternType::None) {
    SetColor(fillColor_, st, fill.bg, 0xFFFFFFFFu);
    if (!fill.bg.IsSet()) fillColor_.automatic = true;
    SetColor(patternColor_, st, fill.fg, 0xFF000000u);
  } else {
    fillColor_.automatic = true;
    patternColor_.automatic = true;
  }
}

void WorkbookView::FormatPanel::ApplyNumber() {
  const std::wstring code = FormatCode(choice_, v_.text_->Locale());
  const uint32_t id = v_.NumFmtId(code);
  v_.ChangeFormat(u"Number Format", [id](Xf& x) {
    x.numFmt = id;
    x.applyNumFmt = true;
  });
  const FormatChoice keep = choice_;
  Sync();
  // The category chosen stays shown, even when its code reads as another's (Number 0 is General's
  // twin only in name).
  if (choice_.category != keep.category && keep.category != FormatCategory::Custom) {
    choice_ = keep;
    category_.selected = static_cast<int>(keep.category);
  }
  Place();
}

void WorkbookView::FormatPanel::ApplyBorders(int which) {
  if (!v_.Ready()) return;
  const Range sel = v_.Selected();
  const int li = std::clamp(line_.selected, 0, static_cast<int>(std::size(kLines)) - 1);
  BorderSide side;
  side.style = kLines[li].style;
  side.color = FromD2D(lineColor_.color, lineColor_.automatic);
  const BorderSide none;
  edit::Editor& ed = v_.Ed();
  const Styles& st = v_.book_->GetStyles();
  edit::EditGroup group(ed, u"Borders");
  bool ok = true;
  // Sides of the cells of `r` through `fn`.
  auto apply = [&](Range r, auto&& fn) {
    if (!ok || r.firstRow > r.lastRow || r.firstCol > r.lastCol) return;
    ok = v_.ChangeFormatIn(r, [&](Xf& x) {
      Border b = x.border < st.Borders().size() ? st.Borders()[x.border] : Border{};
      fn(b);
      x.border = ed.AddBorder(b);
      x.applyBorder = true;
    });
  };
  const Range top{sel.firstRow, sel.firstCol, sel.firstRow, sel.lastCol};
  const Range bottom{sel.lastRow, sel.firstCol, sel.lastRow, sel.lastCol};
  const Range leftCol{sel.firstRow, sel.firstCol, sel.lastRow, sel.firstCol};
  const Range rightCol{sel.firstRow, sel.lastCol, sel.lastRow, sel.lastCol};
  switch (which) {
    case 0:  // none: every side of every cell, diagonals too
      apply(sel, [&](Border& b) {
        b.left = b.right = b.top = b.bottom = b.diagonal = none;
        b.diagonalUp = b.diagonalDown = false;
      });
      break;
    case 1:  // outline
      apply(top, [&](Border& b) { b.top = side; });
      apply(bottom, [&](Border& b) { b.bottom = side; });
      apply(leftCol, [&](Border& b) { b.left = side; });
      apply(rightCol, [&](Border& b) { b.right = side; });
      break;
    case 2:  // all: every cell's four sides
      apply(sel, [&](Border& b) { b.left = b.right = b.top = b.bottom = side; });
      break;
    case 3:  // inside: between the cells, on both of them as Excel stores it
      if (sel.lastRow > sel.firstRow) {
        apply(Range{sel.firstRow, sel.firstCol, sel.lastRow - 1, sel.lastCol}, [&](Border& b) { b.bottom = side; });
        apply(Range{sel.firstRow + 1, sel.firstCol, sel.lastRow, sel.lastCol}, [&](Border& b) { b.top = side; });
      }
      if (sel.lastCol > sel.firstCol) {
        apply(Range{sel.firstRow, sel.firstCol, sel.lastRow, sel.lastCol - 1}, [&](Border& b) { b.right = side; });
        apply(Range{sel.firstRow, sel.firstCol + 1, sel.lastRow, sel.lastCol}, [&](Border& b) { b.left = side; });
      }
      break;
    case 4: apply(top, [&](Border& b) { b.top = side; }); break;
    case 5: apply(bottom, [&](Border& b) { b.bottom = side; }); break;
    case 6: apply(leftCol, [&](Border& b) { b.left = side; }); break;
    case 7: apply(rightCol, [&](Border& b) { b.right = side; }); break;
  }
  group.done = ok;
}

// ---------------------------------------------------------------------------
// The view's side

uint32_t WorkbookView::ActiveStyle() const { return Ready() ? Current().StyleAt(active_.row, active_.col) : 0; }

bool WorkbookView::ChangeFormatIn(const Range& r, const std::function<void(Xf&)>& change) {
  edit::Editor& ed = Ed();
  const Styles& st = book_->GetStyles();
  std::unordered_map<uint32_t, uint32_t> done;  // each old format changed once
  return !EditRefused(ed.MapStyles(static_cast<int>(sheet_), r, [&](uint32_t old) {
    if (auto it = done.find(old); it != done.end()) return it->second;
    Xf x = st.CellXf(old);
    change(x);
    const uint32_t xf = ed.AddXf(x);
    done.emplace(old, xf);
    return xf;
  }));
}

bool WorkbookView::ChangeFormat(std::u16string name, const std::function<void(Xf&)>& change) {
  if (!Ready() || preview_ || ChartSheet()) return false;
  edit::EditGroup group(Ed(), std::move(name));
  group.done = ChangeFormatIn(Selected(), change);
  return group.done;
}

bool WorkbookView::ChangeFont(std::u16string name, const std::function<void(Font&)>& change) {
  if (!Ready()) return false;
  const Styles& st = book_->GetStyles();
  return ChangeFormat(std::move(name), [&](Xf& x) {
    Font f = st.FontOf(x);
    change(f);
    x.font = Ed().AddFont(f);
    x.applyFont = true;
  });
}

uint32_t WorkbookView::NumFmtId(const std::wstring& code) {
  const int builtin = BuiltinId(code);
  return builtin >= 0 ? static_cast<uint32_t>(builtin) : Ed().AddNumFmt(WideToUtf8(code));
}

std::wstring WorkbookView::CodeOf(uint32_t numFmt) const {
  const Styles& st = book_->GetStyles();
  if (const std::string_view custom = st.FormatCode(numFmt); !custom.empty()) return Utf8ToWide(custom);
  if (numFmt < EnUsLocale().builtin.size() && !EnUsLocale().builtin[numFmt].empty()) return EnUsLocale().builtin[numFmt];
  return BuiltinCode(static_cast<int>(numFmt), text_->Locale());
}

void WorkbookView::ShowFormatPanel(int page) {
  if (!Ready() || preview_ || ChartSheet()) return;
  if (!formatPanel_) formatPanel_ = std::make_unique<FormatPanel>(*this);
  formatPanel_->Open(page);
  host_.ViewShowPanel(formatPanel_.get());
}

bool WorkbookView::FormatCommandState(int cmd, CommandState& s) {
  const bool cells = Ready() && !preview_ && !ChartSheet();
  switch (cmd) {
    case CmdFormatCells: s.label = L"Format cells"; s.shortcut = L"Ctrl+1"; s.enabled = cells; return true;
  }
  return false;
}

bool WorkbookView::FormatCommand(int cmd) {
  switch (cmd) {
    case CmdFormatCells: ShowFormatPanel(-1); return true;
  }
  return false;
}

}  // namespace fw::sheet::view
