#include "sheet/edit/Protection.h"
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
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Toolbar.h"
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
const wchar_t* const kBorderButtons[] = {L"None", L"Outline", L"All",    L"Inside",      L"Top",
                                        L"Bottom", L"Left",   L"Right", L"Diagonal up", L"Diagonal down"};
const wchar_t* const kDirections[] = {L"Context", L"Left to right", L"Right to left"};  // readingOrder 0, 1, 2

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
// Format Cells

WorkbookView::FormatPanel::FormatPanel(WorkbookView& view) : v_(view) {
  const wchar_t* tabNames[kPages] = {L"Number", L"Align", L"Font", L"Border", L"Fill", L"Protect"};
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
    v_.MergeCells(merge_.checked ? 2 : 3);
    Sync();
  };
  direction_.label = L"Text direction";
  direction_.items = List(kDirections, std::size(kDirections));
  direction_.onChange = [this](int i) {
    if (i >= 0)
      v_.ChangeFormat(u"Text Direction", [i](Xf& x) {
        x.alignment.readingOrder = static_cast<uint8_t>(i);
        x.applyAlignment = true;
      });
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
  for (int i = 0; i < static_cast<int>(std::size(borders_)); ++i) {
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
  // Protection
  locked_.text = L"Locked";
  hidden_.text = L"Hidden (formulas)";
  for (PanelButton* b : {&locked_, &hidden_}) {
    b->toggle = true;
    b->onInvoke = [this, b] {
      const bool on = b->checked, locked = b == &locked_;
      v_.ChangeFormat(locked ? u"Locked" : u"Hidden", [on, locked](Xf& x) {
        (locked ? x.protection.locked : x.protection.hidden) = on;
        x.applyProtection = true;
      });
    };
  }
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
                                                    &merge_, &direction_, &font_, &style_, &size_, &underline_, &color_,
                                                    &strike_, &superscript_, &subscript_, &line_, &lineColor_})
    out.push_back(c);
  for (PanelButton& b : borders_) out.push_back(&b);
  for (Control* c : std::initializer_list<Control*>{&fillColor_, &pattern_, &patternColor_, &locked_, &hidden_})
    out.push_back(c);
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
                                                    &merge_, &direction_, &font_, &style_, &size_, &underline_, &color_,
                                                    &strike_, &superscript_, &subscript_, &line_, &lineColor_,
                                                    &fillColor_, &pattern_, &patternColor_, &locked_, &hidden_})
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
      row(L"Text direction", {&direction_});
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
      row(L"Diagonals", {&borders_[8], &borders_[9]});
      break;
    case kFill:
      row(L"Background color", {&fillColor_});
      row(L"Pattern style", {&pattern_});
      row(L"Pattern color", {&patternColor_});
      break;
    case kProtection:
      row(nullptr, {&locked_, &hidden_});
      labels_.push_back({L"These take effect once the sheet is protected:", RectF(left, y, right, y + 16 * u)});
      labels_.push_back({L"locked cells can't be changed, hidden formulas", RectF(left, y + 18 * u, right, y + 34 * u)});
      labels_.push_back({L"don't show in the formula bar.", RectF(left, y + 36 * u, right, y + 52 * u)});
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
  direction_.selected = std::min<int>(x.alignment.readingOrder, 2);
  locked_.checked = x.protection.locked;
  hidden_.checked = x.protection.hidden;
  // Excel never changes them on a protected sheet (the edit layer refuses it): greyed out.
  locked_.enabled = hidden_.enabled = !edit::SheetProtected(v_.Current());
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
  const int li = std::clamp(line_.selected, 0, static_cast<int>(std::size(kLines)) - 1);
  BorderSide side;
  side.style = kLines[li].style;
  side.color = FromD2D(lineColor_.color, lineColor_.automatic);
  v_.ApplyBorders(which, side);
}

bool WorkbookView::ApplyBorders(int which, const BorderSide& side) {
  if (!Ready() || preview_ || ChartSheet()) return false;
  const Range sel = Selected();
  const BorderSide none;
  edit::Editor& ed = Ed();
  const Styles& st = book_->GetStyles();
  edit::EditGroup group(ed, u"Borders");
  bool ok = true;
  // Sides of the cells of `r` through `fn`.
  auto apply = [&](Range r, auto&& fn) {
    if (!ok || r.firstRow > r.lastRow || r.firstCol > r.lastCol) return;
    ok = ChangeFormatIn(r, [&](Xf& x) {
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
    case 8:  // the diagonals, in every cell: one line style for both, as Excel stores them
    case 9:
      apply(sel, [&](Border& b) {
        b.diagonal = side;
        (which == 8 ? b.diagonalUp : b.diagonalDown) = true;
      });
      break;
  }
  group.done = ok;
  return ok;
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

bool WorkbookView::ApplyNumberCode(const std::wstring& code) {
  if (!Ready() || preview_ || ChartSheet()) return false;
  const uint32_t id = NumFmtId(code);
  return ChangeFormat(u"Number Format", [id](Xf& x) {
    x.numFmt = id;
    x.applyNumFmt = true;
  });
}

bool WorkbookView::ChangeDecimals(int delta) {
  if (!Ready() || preview_ || ChartSheet()) return false;
  const NumLocale& loc = text_->Locale();
  const std::wstring code = CodeOf(book_->GetStyles().CellXf(ActiveStyle()).numFmt);
  const FormatCategory cat = FormatChoiceOf(code, loc).category;
  if (cat == FormatCategory::Date || cat == FormatCategory::Time || cat == FormatCategory::Text) return false;
  std::wstring next;
  if (cat == FormatCategory::General) {
    // From the decimals General shows for the active cell's number.
    int shown = 0;
    const Value v = Current().GetValue(active_.row, active_.col);
    if (v.type == ValueType::Number) {
      FormatContext ctx;
      ctx.locale = &loc;
      Formatted f;
      NumberFormat().Format(v.number, ctx, f);
      const size_t dot = f.text.find(loc.decimal);
      const size_t e = f.text.find_first_of(L"Ee");
      if (dot != std::wstring::npos) shown = static_cast<int>((e == std::wstring::npos ? f.text.size() : e) - dot - 1);
    }
    const int n = std::max(0, shown + delta);
    next = n ? L"0." + std::wstring(static_cast<size_t>(n), L'0') : L"0";
  } else {
    next = FormatDecimals(code, delta);
  }
  return next != code && ApplyNumberCode(next);
}

bool WorkbookView::MergeCells(int how) {
  // s2-grid's merging (Excel's ask about the other values, Merge & Center toggling off).
  static constexpr int kCmds[] = {CmdMergeCenter, CmdMergeAcross, CmdMergeCells, CmdUnmerge};
  if (how < 0 || how > 3) return false;
  MergeSelection(kCmds[how]);
  return true;
}

namespace {

// The toolbar's number formats, and their codes (invariant; empty: a command).
const wchar_t* const kNumberChoices[] = {L"General",    L"Number",    L"Currency",   L"Accounting",       L"Short date",
                                         L"Long date",  L"Time",      L"Percentage", L"Fraction",         L"Scientific",
                                         L"Text",       L"Increase decimal", L"Decrease decimal", L"More number formats…"};

std::wstring NumberChoiceCode(int i, const NumLocale& loc) {
  FormatChoice c;
  c.decimals = 2;
  switch (i) {
    case 0: return L"General";
    case 1: c.category = FormatCategory::Number; break;
    case 2: c.category = FormatCategory::Currency; break;
    case 3: c.category = FormatCategory::Accounting; break;
    case 4: c.category = FormatCategory::Date; break;
    case 5: c.category = FormatCategory::Date, c.type = 1; break;
    case 6: c.category = FormatCategory::Time; break;
    case 7: c.category = FormatCategory::Percentage; break;
    case 8: c.category = FormatCategory::Fraction; break;
    case 9: c.category = FormatCategory::Scientific; break;
    case 10: return L"@";
    default: return {};
  }
  return FormatCode(c, loc);
}

// The toolbar's borders: what each choice applies (FormatPanel's numbering) and with which line.
struct BorderChoice {
  const wchar_t* name;
  int which;
  BorderStyle style;
};
const BorderChoice kBorderChoices[] = {
    {L"Bottom border", 5, BorderStyle::Thin},      {L"Top border", 4, BorderStyle::Thin},
    {L"Left border", 6, BorderStyle::Thin},        {L"Right border", 7, BorderStyle::Thin},
    {L"No border", 0, BorderStyle::Thin},          {L"All borders", 2, BorderStyle::Thin},
    {L"Outside borders", 1, BorderStyle::Thin},    {L"Thick outside borders", 1, BorderStyle::Thick},
    {L"Bottom double border", 5, BorderStyle::Double}, {L"Thick bottom border", 5, BorderStyle::Thick}};

}  // namespace

// ---------------------------------------------------------------------------
// Cell styles: Excel's built-in gallery (Office 2013 and later theme)

namespace {

constexpr uint32_t kNoRgb = 0;
struct BuiltinStyle {
  std::wstring name;
  int builtinId = 0;
  // Font: bold, italic, size (0: keep the default), color (ARGB, else theme `fontTheme` >= 0).
  bool bold = false, italic = false;
  double size = 0;
  uint32_t fontRgb = kNoRgb;
  int fontTheme = -1;
  bool major = false;  // the theme's headings font
  // Fill: ARGB, else theme `fillTheme` >= 0 with `tint`.
  uint32_t fillRgb = kNoRgb;
  int fillTheme = -1;
  double tint = 0;
  // Borders: every side (all), or the bottom and top only.
  BorderStyle all = BorderStyle::None, bottom = BorderStyle::None, top = BorderStyle::None;
  uint32_t borderRgb = kNoRgb;
  int borderTheme = -1;
  double borderTint = 0;
  int numFmt = -1;  // a built-in number format
};

std::vector<BuiltinStyle> MakeStyles() {
  std::vector<BuiltinStyle> v;
  auto add = [&](std::wstring name, int id) -> BuiltinStyle& {
    v.push_back({});
    v.back().name = name;
    v.back().builtinId = id;
    return v.back();
  };
  add(L"Normal", 0);
  { auto& s = add(L"Good", 26); s.fontRgb = 0xFF006100; s.fillRgb = 0xFFC6EFCE; }
  { auto& s = add(L"Bad", 27); s.fontRgb = 0xFF9C0006; s.fillRgb = 0xFFFFC7CE; }
  { auto& s = add(L"Neutral", 28); s.fontRgb = 0xFF9C5700; s.fillRgb = 0xFFFFEB9C; }
  { auto& s = add(L"Calculation", 22); s.bold = true; s.fontRgb = 0xFFFA7D00; s.fillRgb = 0xFFF2F2F2; s.all = BorderStyle::Thin; s.borderRgb = 0xFF7F7F7F; }
  { auto& s = add(L"Check cell", 23); s.bold = true; s.fontTheme = 0; s.fillRgb = 0xFFA5A5A5; s.all = BorderStyle::Double; s.borderRgb = 0xFF3F3F3F; }
  { auto& s = add(L"Explanatory text", 53); s.italic = true; s.fontRgb = 0xFF7F7F7F; }
  { auto& s = add(L"Input", 20); s.fontRgb = 0xFF3F3F76; s.fillRgb = 0xFFFFCC99; s.all = BorderStyle::Thin; s.borderRgb = 0xFF7F7F7F; }
  { auto& s = add(L"Linked cell", 24); s.fontRgb = 0xFFFA7D00; s.bottom = BorderStyle::Double; s.borderRgb = 0xFFFF8001; }
  { auto& s = add(L"Note", 10); s.fillRgb = 0xFFFFFFCC; s.all = BorderStyle::Thin; s.borderRgb = 0xFFB2B2B2; }
  { auto& s = add(L"Output", 21); s.bold = true; s.fontRgb = 0xFF3F3F3F; s.fillRgb = 0xFFF2F2F2; s.all = BorderStyle::Thin; s.borderRgb = 0xFF3F3F3F; }
  { auto& s = add(L"Warning text", 11); s.fontRgb = 0xFFFF0000; }
  { auto& s = add(L"Title", 15); s.size = 18; s.fontTheme = 3; s.major = true; }
  { auto& s = add(L"Heading 1", 16); s.bold = true; s.size = 15; s.fontTheme = 3; s.bottom = BorderStyle::Thick; s.borderTheme = 4; }
  { auto& s = add(L"Heading 2", 17); s.bold = true; s.size = 13; s.fontTheme = 3; s.bottom = BorderStyle::Thick; s.borderTheme = 4; s.borderTint = 0.4999; }
  { auto& s = add(L"Heading 3", 18); s.bold = true; s.size = 11; s.fontTheme = 3; s.bottom = BorderStyle::Medium; s.borderTheme = 4; s.borderTint = 0.3999; }
  { auto& s = add(L"Heading 4", 19); s.bold = true; s.size = 11; s.fontTheme = 3; }
  { auto& s = add(L"Total", 25); s.bold = true; s.fontTheme = 1; s.top = BorderStyle::Thin; s.bottom = BorderStyle::Double; s.borderTheme = 4; }
  static const wchar_t* const kAccents[] = {L"Accent1", L"Accent2", L"Accent3", L"Accent4", L"Accent5", L"Accent6"};
  for (int a = 0; a < 6; ++a) {
    static const double kTints[] = {0.7999, 0.5999, 0.3999};
    static const wchar_t* const kShares[] = {L"20% - ", L"40% - ", L"60% - "};
    for (int t = 0; t < 3; ++t) {
      auto& s = add(std::wstring(kShares[t]) + kAccents[a], 30 + a * 4 + t);
      s.fontTheme = t == 2 ? 0 : 1;
      s.fillTheme = 4 + a;
      s.tint = kTints[t];
    }
    auto& s = add(kAccents[a], 29 + a * 4);
    s.fontTheme = 0;
    s.fillTheme = 4 + a;
  }
  { auto& s = add(L"Comma", 3); s.numFmt = 43; }
  { auto& s = add(L"Comma [0]", 6); s.numFmt = 41; }
  { auto& s = add(L"Currency", 4); s.numFmt = 44; }
  { auto& s = add(L"Currency [0]", 7); s.numFmt = 42; }
  { auto& s = add(L"Percent", 5); s.numFmt = 9; }
  return v;
}

const std::vector<BuiltinStyle>& Gallery() {
  static const std::vector<BuiltinStyle> styles = MakeStyles();
  return styles;
}

Color ThemeColor(int theme, double tint) {
  Color c;
  c.kind = Color::Kind::Theme;
  c.value = static_cast<uint32_t>(theme);
  c.tint = tint;
  return c;
}

Color RgbColor(uint32_t argb) {
  Color c;
  c.kind = Color::Kind::Rgb;
  c.value = argb;
  return c;
}

}  // namespace

bool WorkbookView::ApplyCellStyle(int index) {
  const auto& gallery = Gallery();
  if (!Ready() || index < 0 || index >= static_cast<int>(gallery.size())) return false;
  const BuiltinStyle& s = gallery[static_cast<size_t>(index)];
  const Styles& st = book_->GetStyles();
  Font font = st.DefaultFont();
  if (!st.Fonts().empty()) font = st.Fonts()[0];
  font.bold = s.bold;
  font.italic = s.italic;
  if (s.size > 0) font.size = s.size;
  if (s.fontRgb != kNoRgb) font.color = RgbColor(s.fontRgb);
  else if (s.fontTheme >= 0) font.color = ThemeColor(s.fontTheme, 0);
  if (s.major) {
    font.name = "Calibri Light";
    font.scheme = FontScheme::Major;
  }
  Fill fill;
  if (s.fillRgb != kNoRgb || s.fillTheme >= 0) {
    fill.pattern = PatternType::Solid;
    fill.fg = s.fillRgb != kNoRgb ? RgbColor(s.fillRgb) : ThemeColor(s.fillTheme, s.tint);
    fill.bg.kind = Color::Kind::Indexed;
    fill.bg.value = 64;
  }
  Border border;
  BorderSide side;
  side.color = s.borderRgb != kNoRgb ? RgbColor(s.borderRgb) : ThemeColor(std::max(0, s.borderTheme), s.borderTint);
  auto with = [&](BorderStyle style) {
    BorderSide b = side;
    b.style = style;
    return b;
  };
  if (s.all != BorderStyle::None) border.left = border.right = border.top = border.bottom = with(s.all);
  if (s.bottom != BorderStyle::None) border.bottom = with(s.bottom);
  if (s.top != BorderStyle::None) border.top = with(s.top);
  const bool normal = s.builtinId == 0;
  const bool fonts = normal || s.bold || s.italic || s.size > 0 || s.fontRgb != kNoRgb || s.fontTheme >= 0;
  const bool fills = normal || s.fillRgb != kNoRgb || s.fillTheme >= 0;
  const bool borders = normal || s.all != BorderStyle::None || s.bottom != BorderStyle::None || s.top != BorderStyle::None;
  const bool numbers = normal || s.numFmt >= 0;
  edit::Editor& ed = Ed();
  const uint32_t fontId = fonts ? ed.AddFont(font) : 0, fillId = fills ? ed.AddFill(fill) : 0;
  const uint32_t borderId = borders ? ed.AddBorder(border) : 0;
  // The workbook's own entry for the style, else this one added: its format says what the style
  // covers (Good: font and fill), as Excel writes its cellStyleXfs.
  Xf styleXf;
  styleXf.font = fontId, styleXf.fill = fillId, styleXf.border = borderId;
  styleXf.numFmt = s.numFmt >= 0 ? static_cast<uint32_t>(s.numFmt) : 0;
  styleXf.applyFont = fonts, styleXf.applyFill = fills, styleXf.applyBorder = borders, styleXf.applyNumFmt = numbers;
  styleXf.applyAlignment = styleXf.applyProtection = normal;
  CellStyle style;
  style.name = WideToUtf8(s.name);
  style.builtinId = s.builtinId;
  // The workbook's own style, when it had one before (a file from Excel may define it its way):
  // the cell takes the parts that one covers, as Excel applies it.
  std::optional<Xf> own;
  for (const CellStyle& cs : st.CellStyles())
    if (cs.builtinId == s.builtinId && cs.xf < st.CellStyleXfs().size()) own = st.CellStyleXfs()[cs.xf];
  const uint32_t parent = ed.AddCellStyle(style, styleXf);
  const std::u16string name(s.name.begin(), s.name.end());
  if (own)
    return ChangeFormat(name, [&](Xf& x) {
      x.parent = parent;
      if (own->applyFont) x.font = own->font, x.applyFont = true;
      if (own->applyFill) x.fill = own->fill, x.applyFill = true;
      if (own->applyBorder) x.border = own->border, x.applyBorder = true;
      if (own->applyNumFmt) x.numFmt = own->numFmt, x.applyNumFmt = own->numFmt != 0;
      if (own->applyAlignment) x.alignment = own->alignment, x.applyAlignment = false;
      if (own->applyProtection) x.protection = own->protection, x.applyProtection = false;
    });
  return ChangeFormat(name, [&](Xf& x) {
    x.parent = parent;
    if (fonts) x.font = fontId, x.applyFont = true;
    if (fills) x.fill = fillId, x.applyFill = true;
    if (borders) x.border = borderId, x.applyBorder = true;
    if (numbers) x.numFmt = s.numFmt >= 0 ? static_cast<uint32_t>(s.numFmt) : 0, x.applyNumFmt = s.numFmt >= 0;
    if (normal) x.alignment = Alignment{}, x.applyAlignment = false;
  });
}

// ---------------------------------------------------------------------------
// The format painter

void WorkbookView::PaintFormats() {
  if (!painter_ || !Ready()) return;
  const Painter p = *painter_;
  painter_.reset();
  const Sheet* from = nullptr;
  for (size_t i = 0; i < book_->SheetCount(); ++i)
    if (&book_->GetSheet(i) == p.sheet) from = p.sheet;
  if (!from) return;
  const Range src = p.range;
  Range to = Selected();
  const uint32_t h = src.lastRow - src.firstRow + 1, w = src.lastCol - src.firstCol + 1;
  // One cell picked: the copied cells' shape from there, as Excel paints.
  if (to.firstRow == to.lastRow && to.firstCol == to.lastCol) {
    to.lastRow = std::min<uint32_t>(kMaxRows - 1, to.firstRow + h - 1);
    to.lastCol = std::min<uint32_t>(kMaxCols - 1, to.firstCol + w - 1);
  }
  if (uint64_t{to.lastRow - to.firstRow + 1} * (to.lastCol - to.firstCol + 1) > 200000) {
    host_.ViewMessage(BannerKind::Warning, L"That's more cells than the format painter paints at once.");
    return;
  }
  const int sheet = static_cast<int>(sheet_);
  edit::EditGroup group(Ed(), u"Format Painter");
  edit::Result r;
  for (uint32_t row = to.firstRow; r && row <= to.lastRow; ++row)
    for (uint32_t col = to.firstCol; r && col <= to.lastCol; ++col) {
      const uint32_t style = from->StyleAt(src.firstRow + (row - to.firstRow) % h, src.firstCol + (col - to.firstCol) % w);
      if (Current().StyleAt(row, col) != style) r = Ed().SetStyle(sheet, Range::Cell(row, col), style);
    }
  if (EditRefused(r)) return;
  group.done = true;
  Select({to.firstRow, to.firstCol}, {to.lastRow, to.lastCol}, false);
  host_.ViewInvalidate();
}

bool WorkbookView::FormatCommandState(int cmd, CommandState& s) {
  const bool cells = Ready() && !preview_ && !ChartSheet();
  const Styles* st = Ready() ? &book_->GetStyles() : nullptr;
  const Xf* x = st ? &st->CellXf(ActiveStyle()) : nullptr;
  const Font* f = x ? &st->FontOf(*x) : nullptr;
  auto set = [&](const wchar_t* label, const wchar_t* keys, bool checked = false) {
    s.label = label;
    s.shortcut = keys;
    s.enabled = cells;
    s.checked = cells && checked;
    return true;
  };
  switch (cmd) {
    case CmdFormatCells: return set(L"Format cells…", L"Ctrl+1");
    case CmdBold: return set(L"Bold", L"Ctrl+B", f && f->bold);
    case CmdItalic: return set(L"Italic", L"Ctrl+I", f && f->italic);
    case CmdUnderline: return set(L"Underline", L"Ctrl+U", f && f->underline != Underline::None);
    case CmdStrikethrough: return set(L"Strikethrough", L"Ctrl+5", f && f->strike);
    case CmdAlignLeft: return set(L"Align left", L"", x && x->alignment.horizontal == HAlign::Left);
    case CmdAlignCenter: return set(L"Center", L"", x && x->alignment.horizontal == HAlign::Center);
    case CmdAlignRight: return set(L"Align right", L"", x && x->alignment.horizontal == HAlign::Right);
    case CmdFontColor: return set(L"Font color", L"");
    case CmdFillColor: return set(L"Fill color", L"");
    case CmdBordersBox: return set(L"Borders", L"");
    case CmdMergeBox: return set(L"Merge", L"");
    case CmdNumberFormatBox: return set(L"More number formats…", L"");  // Format cells' Number page
    case CmdIncreaseDecimal: return set(L"Increase decimal", L"");
    case CmdDecreaseDecimal: return set(L"Decrease decimal", L"");
    case CmdNumGeneral: return set(L"General format", L"Ctrl+Shift+~");
    case CmdNumNumber: return set(L"Number format", L"Ctrl+Shift+1");
    case CmdNumTime: return set(L"Time format", L"Ctrl+Shift+2");
    case CmdNumDate: return set(L"Date format", L"Ctrl+Shift+3");
    case CmdNumCurrency: return set(L"Currency format", L"Ctrl+Shift+4");
    case CmdNumPercent: return set(L"Percent format", L"Ctrl+Shift+5");
    case CmdNumScientific: return set(L"Scientific format", L"Ctrl+Shift+6");
    case CmdBorderOutline: return set(L"Outline border", L"Ctrl+Shift+7");
    case CmdBorderRemove: return set(L"Remove borders", L"Ctrl+Shift+-");
    case CmdCellStyles: return set(L"Cell styles", L"");
    case CmdFormatPainter: return set(L"Format painter: copy these cells' formats to the cells you select next", L"",
                                      painter_.has_value());
    // Excel's Home tab (R1, r1-sheet).
    case CmdFontBox: return set(L"Font", L"");
    case CmdSizeBox: return set(L"Font size", L"");
    case CmdFormatCellsFont: return set(L"Format cells: font…", L"");
    case CmdFormatCellsAlignment: return set(L"Format cells: alignment…", L"");
    case CmdAlignTop: return set(L"Top align", L"", x && x->alignment.vertical == VAlign::Top);
    case CmdAlignMiddle: return set(L"Middle align", L"", x && x->alignment.vertical == VAlign::Center);
    case CmdAlignBottom: return set(L"Bottom align", L"", x && x->alignment.vertical == VAlign::Bottom);
    case CmdWrapText: return set(L"Wrap text", L"", x && x->alignment.wrap);
    case CmdIndent: return set(L"Increase indent", L"");
    case CmdOutdent: return set(L"Decrease indent", L"");
    case CmdGrowFont: return set(L"Increase font size", L"");
    case CmdShrinkFont: return set(L"Decrease font size", L"");
    case CmdNumAccounting: return set(L"Accounting number format", L"");
    case CmdCommaStyle: return set(L"Comma style", L"");
  }
  return false;
}

bool WorkbookView::FormatCommand(int cmd) {
  if (cmd == CmdFormatCells) {
    ShowFormatPanel(-1);
    return true;
  }
  CommandState s;
  if (!FormatCommandState(cmd, s)) return false;
  if (!s.enabled) return true;
  const Styles& st = book_->GetStyles();
  const Xf active = st.CellXf(ActiveStyle());
  const Font activeFont = st.FontOf(active);
  const NumLocale& loc = text_->Locale();
  auto align = [&](HAlign h) {
    const HAlign to = active.alignment.horizontal == h ? HAlign::General : h;  // a second click takes it off
    ChangeFormat(u"Alignment", [to](Xf& x) {
      x.alignment.horizontal = to;
      x.applyAlignment = true;
    });
  };
  BorderSide thin;
  thin.style = BorderStyle::Thin;
  switch (cmd) {
    case CmdBold: {
      const bool on = !activeFont.bold;  // all bold, unless the active cell is
      ChangeFont(u"Bold", [on](Font& f) { f.bold = on; });
      break;
    }
    case CmdItalic: {
      const bool on = !activeFont.italic;
      ChangeFont(u"Italic", [on](Font& f) { f.italic = on; });
      break;
    }
    case CmdUnderline: {
      const Underline u = activeFont.underline == Underline::None ? Underline::Single : Underline::None;
      ChangeFont(u"Underline", [u](Font& f) { f.underline = u; });
      break;
    }
    case CmdStrikethrough: {
      const bool on = !activeFont.strike;
      ChangeFont(u"Strikethrough", [on](Font& f) { f.strike = on; });
      break;
    }
    case CmdAlignLeft: align(HAlign::Left); break;
    case CmdAlignCenter: align(HAlign::Center); break;
    case CmdAlignRight: align(HAlign::Right); break;
    case CmdFontColor: {
      const Color c = FromD2D(lastFontColor_, false);
      ChangeFont(u"Font Color", [c](Font& f) { f.color = c; });
      break;
    }
    case CmdFillColor: {
      Fill fill;
      fill.pattern = PatternType::Solid;
      fill.fg = FromD2D(lastFill_, false);
      ChangeFormat(u"Fill", [this, fill](Xf& x) {
        x.fill = Ed().AddFill(fill);
        x.applyFill = true;
      });
      break;
    }
    case CmdBordersBox: {
      const BorderChoice& b = kBorderChoices[std::clamp(lastBorders_, 0, static_cast<int>(std::size(kBorderChoices)) - 1)];
      BorderSide side;
      side.style = b.style;
      ApplyBorders(b.which, side);
      break;
    }
    case CmdMergeBox: MergeCells(0); break;
    case CmdIncreaseDecimal: ChangeDecimals(1); break;
    case CmdDecreaseDecimal: ChangeDecimals(-1); break;
    case CmdNumberFormatBox: ShowFormatPanel(FormatPanel::kNumber); break;
    case CmdNumGeneral: ApplyNumberCode(L"General"); break;
    case CmdNumNumber: ApplyNumberCode(L"#,##0.00"); break;
    case CmdNumTime: ApplyNumberCode(L"h:mm AM/PM"); break;
    case CmdNumDate: ApplyNumberCode(L"d-mmm-yy"); break;
    case CmdNumCurrency: ApplyNumberCode(NumberChoiceCode(2, loc)); break;
    case CmdNumPercent: ApplyNumberCode(L"0%"); break;
    case CmdNumScientific: ApplyNumberCode(L"0.00E+00"); break;
    case CmdBorderOutline: ApplyBorders(1, thin); break;
    case CmdBorderRemove: ApplyBorders(0, thin); break;
    case CmdFormatPainter:
      if (painter_) painter_.reset();
      else painter_ = Painter{&Current(), Selected()};
      host_.ViewStateChanged();
      break;
    case CmdFormatCellsFont: ShowFormatPanel(FormatPanel::kFont); break;
    case CmdFormatCellsAlignment: ShowFormatPanel(FormatPanel::kAlignment); break;
    case CmdAlignTop:
    case CmdAlignMiddle:
    case CmdAlignBottom: {
      // As Excel's buttons: a second click on the pressed one goes back to bottom.
      const VAlign v = cmd == CmdAlignTop ? VAlign::Top : cmd == CmdAlignMiddle ? VAlign::Center : VAlign::Bottom;
      const VAlign to = active.alignment.vertical == v ? VAlign::Bottom : v;
      ChangeFormat(u"Alignment", [to](Xf& x) {
        x.alignment.vertical = to;
        x.applyAlignment = true;
      });
      break;
    }
    case CmdWrapText: {
      const bool on = !active.alignment.wrap;
      ChangeFormat(u"Wrap Text", [on](Xf& x) {
        x.alignment.wrap = on;
        x.applyAlignment = true;
      });
      break;
    }
    case CmdIndent:
    case CmdOutdent: {
      // Excel's: an indent needs left or right alignment (left unless the cell is right aligned);
      // decreasing to 0 keeps the alignment.
      const int step = cmd == CmdIndent ? 1 : -1;
      if (step < 0 && active.alignment.indent == 0) break;
      ChangeFormat(cmd == CmdIndent ? u"Increase Indent" : u"Decrease Indent", [step](Xf& x) {
        Alignment& a = x.alignment;
        a.indent = static_cast<uint16_t>(std::clamp(static_cast<int>(a.indent) + step, 0, 250));
        if (a.horizontal != HAlign::Right && a.horizontal != HAlign::Distributed && a.indent > 0) a.horizontal = HAlign::Left;
        x.applyAlignment = true;
      });
      break;
    }
    case CmdGrowFont:
    case CmdShrinkFont: {
      // The next size in the list (Excel's), past 72 by tens, below 8 by ones.
      const double now = activeFont.size > 0 ? activeFont.size : st.DefaultFont().size;
      double to = now;
      if (cmd == CmdGrowFont) {
        to = now >= 72 ? std::floor(now / 10) * 10 + 10 : 0;
        for (const wchar_t* size : kSizes)
          if (!to && _wtof(size) > now + 0.01) to = _wtof(size);
      } else {
        for (const wchar_t* size : kSizes)
          if (_wtof(size) < now - 0.01) to = _wtof(size);
        if (now <= 8.01) to = std::ceil(now) - 1;
        if (now > 72) to = std::max(72.0, std::ceil(now / 10) * 10 - 10);
      }
      to = std::clamp(to, 1.0, 409.0);
      if (to != now) ChangeFont(u"Font Size", [to](Font& f) { f.size = to; });
      break;
    }
    case CmdNumAccounting: ApplyNumberCode(NumberChoiceCode(3, loc)); break;
    case CmdCommaStyle: {
      const auto& g = Gallery();
      for (size_t i = 0; i < g.size(); ++i)
        if (g[i].name == L"Comma") ApplyCellStyle(static_cast<int>(i));
      break;
    }
  }
  return true;
}

// The boxes and color buttons Excel's Home tab has (font, size, font and fill colors, borders, cell
// styles, number format), made with the active cell's values, for the ribbon (and the toolbar).
ToolItem WorkbookView::FormatTool(int cmd) {
  ToolItem t;
  t.cmd = cmd;
  t.enabled = Ready() && !preview_ && !ChartSheet();  // the selected cells' (the boxes have no state of their own)
  if (!t.enabled) return t;
  const Styles& st = book_->GetStyles();
  const Xf& x = st.CellXf(ActiveStyle());
  const Font& f = st.FontOf(x);
  // Colors: the main part repeats the last one, the arrow picks another.
  auto colors = [&](wchar_t glyph, D2D1_COLOR_F& last, const wchar_t* automaticLabel, bool fill) {
    t.kind = ToolKind::SplitButton;
    t.glyph = glyph;
    t.width = 52;
    t.colors = true;
    t.color = last;
    t.automaticLabel = automaticLabel;
    t.automaticColor = fill ? D2D1_COLOR_F{0, 0, 0, 0} : D2D1_COLOR_F{0, 0, 0, 1};
    t.onChange = [this, &last, fill](const ToolValue& v) {
      if (!v.automatic) last = v.color;
      if (fill) {
        Fill fl;
        if (!v.automatic) {
          fl.pattern = PatternType::Solid;
          fl.fg = FromD2D(v.color, false);
        }
        ChangeFormat(u"Fill", [this, fl](Xf& xf) {
          xf.fill = Ed().AddFill(fl);
          xf.applyFill = true;
        });
      } else {
        const Color c = FromD2D(v.color, v.automatic);
        ChangeFont(u"Font Color", [c](Font& fo) { fo.color = c; });
      }
    };
  };
  switch (cmd) {
    case CmdFontBox:
      if (DocFormat::FontsPending()) DocFormat::ReadFonts();
      t.kind = ToolKind::ComboBox;
      t.width = 116;
      t.tooltip = L"Font";
      t.choices = DocFormat::FontNames();
      t.text = Utf8ToWide(f.name.empty() ? st.DefaultFont().name : f.name);
      t.onChange = [this](const ToolValue& v) {
        if (v.text.empty()) return;
        const std::string name = WideToUtf8(v.text);
        ChangeFont(u"Font", [&name](Font& fo) {
          fo.name = name;
          fo.scheme = FontScheme::None;
        });
      };
      break;
    case CmdSizeBox:
      t.kind = ToolKind::ComboBox;
      t.width = 56;
      t.tooltip = L"Font size";
      t.choices = List(kSizes, std::size(kSizes));
      t.text = PointsText(f.size > 0 ? f.size : st.DefaultFont().size);
      t.validate = [](std::wstring& text) {
        wchar_t* end = nullptr;
        const double v = std::wcstod(text.c_str(), &end);
        if (end == text.c_str() || !(v >= 1 && v <= 409)) return false;
        text = PointsText(std::round(v * 2) / 2);
        return true;
      };
      t.onChange = [this](const ToolValue& v) {
        const double s = std::wcstod(v.text.c_str(), nullptr);
        if (s >= 1) ChangeFont(u"Font Size", [s](Font& fo) { fo.size = s; });
      };
      break;
    case CmdFontColor: colors(icon::FontColor, lastFontColor_, L"Automatic", false); break;
    case CmdFillColor: colors(icon::FillBucket, lastFill_, L"No color", true); break;
    case CmdBordersBox:
      // The main part repeats the last choice.
      t.kind = ToolKind::SplitButton;
      t.glyph = icon::BordersBottom;
      t.width = 52;
      t.tooltip = std::wstring(L"Borders: ") + kBorderChoices[lastBorders_].name;
      for (const BorderChoice& b : kBorderChoices) t.choices.push_back(b.name);
      t.onChange = [this](const ToolValue& v) {
        if (v.index < 0 || v.index >= static_cast<int>(std::size(kBorderChoices))) return;
        lastBorders_ = v.index;
        BorderSide side;
        side.style = kBorderChoices[v.index].style;
        ApplyBorders(kBorderChoices[v.index].which, side);
      };
      break;
    case CmdCellStyles:
      t.kind = ToolKind::Dropdown;
      t.width = 92;
      t.maxRows = 20;
      t.text = L"Styles";
      t.fitText = true;
      t.tooltip = L"Cell styles";
      for (const BuiltinStyle& s : Gallery()) t.choices.push_back(s.name);
      t.onChange = [this](const ToolValue& v) { ApplyCellStyle(v.index); };
      break;
    case CmdNumberFormatBox: {
      // Excel's list: the formats, then More Number Formats (the decimals are buttons beside it).
      t.kind = ToolKind::Dropdown;
      t.width = 116;
      t.maxRows = 12;
      t.tooltip = L"Number format";
      t.text = kCategories[static_cast<int>(FormatChoiceOf(CodeOf(x.numFmt), text_->Locale()).category)];
      t.choices = List(kNumberChoices, 11);
      t.choices.push_back(kNumberChoices[13]);
      t.onChange = [this](const ToolValue& v) {
        if (v.index == 11) ShowFormatPanel(FormatPanel::kNumber);
        else if (v.index >= 0 && v.index < 11) ApplyNumberCode(NumberChoiceCode(v.index, text_->Locale()));
      };
      break;
    }
  }
  return t;
}

// ---------------------------------------------------------------------------
// Scripted formatting (build from scratch): the toolbar's and Format Cells' calls by name

bool WorkbookView::ScriptOp(const std::wstring& op, const std::wstring& arg) {
  if (op == L"rule" || op == L"valid" || op == L"table") return DataRulesScriptOp(op, arg);  // WorkbookData.cpp
  if (op == L"object") return ShapesScriptOp(arg);  // WorkbookShapes.cpp
  if (op == L"name" || op == L"names") return NamesScriptOp(op, arg);  // s3-calc's (WorkbookNames.cpp)
  if (op == L"pagesetup") return PageScriptOp(arg);  // s3-print's (WorkbookPageSetup.cpp)
  if (op == L"ribbon") return RibbonScriptOp(arg);  // R1 (WorkbookRibbon.cpp)
  if (op != L"format") return GridScriptOp(op, arg);  // s2-grid's (fill:, WorkbookFill.cpp)
  const size_t eq = arg.find(L'=');
  const std::wstring key = fw::ToLower(fw::Trim(arg.substr(0, eq)));
  const std::wstring value = eq == std::wstring::npos ? std::wstring() : fw::Trim(arg.substr(eq + 1));
  auto rgb = [&](Color& out) {
    wchar_t* end = nullptr;
    const unsigned long v = std::wcstoul(value.c_str(), &end, 16);
    if (value.size() != 6 || !end || *end) return false;
    out = RgbColor(0xFF000000u | static_cast<uint32_t>(v));
    return true;
  };
  bool ok = false;
  if (key == L"size") {
    const double s = std::wcstod(value.c_str(), nullptr);
    ok = s >= 1 && s <= 409 && ChangeFont(u"Font Size", [s](Font& f) { f.size = s; });
  } else if (key == L"font") {
    const std::string name = WideToUtf8(value);
    ok = !name.empty() && ChangeFont(u"Font", [&name](Font& f) {
      f.name = name;
      f.scheme = FontScheme::None;
    });
  } else if (key == L"color") {
    Color c;
    ok = rgb(c) && ChangeFont(u"Font Color", [c](Font& f) { f.color = c; });
  } else if (key == L"fill") {
    Fill fill;
    if (value != L"none") {
      fill.pattern = PatternType::Solid;
      if (!rgb(fill.fg)) return fwprintf(stderr, L"format: no color in %s\n", arg.c_str()), true;
    }
    ok = ChangeFormat(u"Fill", [this, fill](Xf& x) {
      x.fill = Ed().AddFill(fill);
      x.applyFill = true;
    });
  } else if (key == L"numfmt") {
    // As typed in Format Cells here (the region's letters: jjjj, uu in en-NL, as Excel reads them).
    std::wstring invariant;
    ok = NumberFormat::LocalToInvariant(value, text_->Locale(), invariant) && ApplyNumberCode(invariant);
  } else if (key == L"code") {
    // As files store it and Excel's Range.NumberFormat takes it (English: yyyy, #,##0.00).
    ok = !value.empty() && ApplyNumberCode(value);
  } else if (key == L"border") {
    // [double-|thick-|medium-]top, bottom, left, right; outline, all, inside; none.
    BorderSide side;
    side.style = BorderStyle::Thin;
    std::wstring edge = value;
    for (const auto& [prefix, style] : {std::pair{L"double-", BorderStyle::Double}, std::pair{L"thick-", BorderStyle::Thick},
                                        std::pair{L"medium-", BorderStyle::Medium}})
      if (edge.rfind(prefix, 0) == 0) edge.erase(0, std::wcslen(prefix)), side.style = style;
    static const wchar_t* const kEdges[] = {L"none",   L"outline", L"all",   L"inside",      L"top",
                                            L"bottom", L"left",    L"right", L"diagonal-up", L"diagonal-down"};
    for (int i = 0; i < static_cast<int>(std::size(kEdges)); ++i)
      if (edge == kEdges[i]) ok = ApplyBorders(i, side);
  } else if (key == L"align") {
    const HAlign h = value == L"left" ? HAlign::Left : value == L"center" ? HAlign::Center
                     : value == L"right" ? HAlign::Right : HAlign::General;
    ok = ChangeFormat(u"Alignment", [h](Xf& x) {
      x.alignment.horizontal = h;
      x.applyAlignment = true;
    });
  } else if (key == L"wrap" || key == L"bold" || key == L"italic") {
    const bool on = value != L"off" && value != L"0";
    if (key == L"wrap")
      ok = ChangeFormat(u"Wrap Text", [on](Xf& x) {
        x.alignment.wrap = on;
        x.applyAlignment = true;
      });
    else if (key == L"bold") ok = ChangeFont(u"Bold", [on](Font& f) { f.bold = on; });
    else ok = ChangeFont(u"Italic", [on](Font& f) { f.italic = on; });
  } else if (key == L"locked" || key == L"hidden") {
    const bool on = value != L"off" && value != L"0", locked = key == L"locked";
    ok = ChangeFormat(locked ? u"Locked" : u"Hidden", [on, locked](Xf& x) {
      (locked ? x.protection.locked : x.protection.hidden) = on;
      x.applyProtection = true;
    });
  } else if (key == L"style") {
    const auto& g = Gallery();
    for (size_t i = 0; i < g.size(); ++i)
      if (fw::ToLower(g[i].name) == fw::ToLower(value)) ok = ApplyCellStyle(static_cast<int>(i));
  }
  if (!ok) fwprintf(stderr, L"format failed: %s\n", arg.c_str());
  return true;
}

}  // namespace fw::sheet::view
