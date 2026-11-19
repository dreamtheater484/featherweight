// Conditional formatting and data validation (S3, s3-data): the panels, the commands, Excel's
// alert when a typed value breaks a cell's validation, the list's in-cell button, the input
// message and Circle Invalid Data. The rules themselves are sheet/data/Rules; the model changes
// through Editor::SetSheetRules and AddDxf.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>

#include "Commands.h"
#include "core/Strings.h"
#include "sheet/calc/Bind.h"
#include "sheet/calc/Entry.h"
#include "sheet/calc/Typing.h"
#include "sheet/numfmt/Parse.h"
#include "sheet/view/WorkbookFormat.h"
#include "sheet/xlsx/Internal.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

using namespace gfx;

namespace fw::sheet::view {

namespace {

using data::CfOp;
using data::Validation;

// The kinds of rule the panel makes, in the order its list shows them.
enum Kind {
  kGreater, kLess, kBetween, kEqual, kContains, kDate, kDuplicate, kUnique, kTop, kTopPercent, kBottom,
  kBottomPercent, kAbove, kBelow, kBars, kScales, kIcons, kFormula, kNotBetween, kNotEqual, kGreaterEqual,
  kLessEqual, kBeginsWith, kEndsWith, kNotContains, kKinds
};
constexpr const wchar_t* kKindNames[] = {
    L"Greater than",         L"Less than",          L"Between",         L"Equal to",
    L"Text that contains",   L"A date occurring",   L"Duplicate values", L"Unique values",
    L"Top N items",          L"Top N %",            L"Bottom N items",  L"Bottom N %",
    L"Above average",        L"Below average",      L"Data bars",       L"Color scales",
    L"Icon sets",            L"Use a formula",      L"Not between",     L"Not equal to",
    L"Greater than or equal to", L"Less than or equal to", L"Text that begins with", L"Text that ends with",
    L"Text that does not contain"};
// Script names (rule:greater,5).
constexpr const wchar_t* kKindKeys[] = {L"greater", L"less",      L"between",   L"equal",    L"contains",
                                        L"date",    L"duplicate", L"unique",    L"top",      L"toppct",
                                        L"bottom",  L"bottompct", L"above",     L"below",    L"bar",
                                        L"scale",   L"icons",     L"formula",   L"notbetween", L"notequal",
                                        L"greaterequal", L"lessequal", L"begins", L"ends",  L"notcontains"};
int Values(int k) {
  if (k == kBetween || k == kNotBetween) return 2;
  if (k == kDate || k == kDuplicate || k == kUnique || k == kAbove || k == kBelow || k == kBars || k == kScales ||
      k == kIcons)
    return 0;
  return 1;
}
bool HasFormat(int k) { return k != kBars && k != kScales && k != kIcons; }
CfOp OpOfKind(int k) {
  switch (k) {
    case kGreater: return CfOp::Greater;
    case kLess: return CfOp::Less;
    case kBetween: return CfOp::Between;
    case kEqual: return CfOp::Equal;
    case kNotBetween: return CfOp::NotBetween;
    case kNotEqual: return CfOp::NotEqual;
    case kGreaterEqual: return CfOp::GreaterEqual;
    default: return CfOp::LessEqual;
  }
}

std::vector<std::wstring> Wide(const char* const* names, size_t n) {
  std::vector<std::wstring> out;
  for (size_t i = 0; i < n; ++i) out.push_back(Utf8ToWide(names[i]));
  return out;
}
template <size_t N>
std::vector<std::wstring> List(const wchar_t* const (&names)[N]) {
  return std::vector<std::wstring>(std::begin(names), std::end(names));
}

std::string Quote(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += '"';
    out += c;
  }
  return out + "\"";
}

// A typed value or formula ("5", "1,5" on a decimal-comma region, "=A1*2", "abc") as a rule's
// invariant formula, without "=": numbers and dates as numbers, other text quoted. Empty when nothing
// is typed or the formula doesn't read.
std::string Operand(const std::wstring& typed, const calc::FormulaLocale& fl, const NumLocale& loc, bool date1904) {
  const std::wstring t = Trim(typed);
  if (t.empty()) return {};
  if (t[0] == L'=') {
    std::string out;
    if (!calc::ToInvariant(WideToUtf8(t.substr(1)), fl, out)) return {};
    return out;
  }
  ParsedValue pv;
  if (ParseValue(t, loc, date1904, pv) && pv.kind == ParsedValue::Number) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.15g", pv.number);
    return buf;
  }
  return Quote(WideToUtf8(t));
}

// A rule's operand shown back as it would be typed.
std::wstring ShownOperand(const std::string& f, const calc::FormulaLocale& fl) {
  if (f.size() >= 2 && f.front() == '"' && f.back() == '"') {
    std::string s;
    for (size_t i = 1; i + 1 < f.size(); ++i) {
      if (f[i] == '"' && i + 2 < f.size() && f[i + 1] == '"') ++i;
      s += f[i];
    }
    return Utf8ToWide(s);
  }
  char* end = nullptr;
  std::strtod(f.c_str(), &end);
  const std::string local = calc::ToLocal(f, fl);
  if (!f.empty() && end && *end == 0) return Utf8ToWide(local);
  return f.empty() ? std::wstring() : L"=" + Utf8ToWide(local);
}

// Typed cells ("A1:B5", "=$A$1:$B$5;$D$2") as a sqref; empty when they don't read.
std::string SqrefOf(const std::wstring& typed) {
  std::string s = WideToUtf8(Trim(typed));
  if (!s.empty() && s[0] == '=') s.erase(0, 1);
  std::vector<Range> areas;
  std::string part;
  auto flush = [&]() -> bool {
    if (part.empty()) return true;
    Range r;
    std::string clean;
    for (char c : part)
      if (c != '$') clean += c;
    if (!ParseRange(clean, r)) return false;
    areas.push_back(r);
    part.clear();
    return true;
  };
  for (char c : s) {
    if (c == ',' || c == ';' || c == ' ') {
      if (!flush()) return {};
    } else {
      part += c;
    }
  }
  if (!flush()) return {};
  return data::JoinAreas(areas);
}

std::wstring ShownSqref(const std::string& sqref) {
  std::wstring out;
  for (const Range& r : data::ParseAreas(sqref)) {
    if (!out.empty()) out += L' ';
    out += Utf8ToWide(RangeName(r));
  }
  return out;
}

// The dxfId of a rule's XML (-1: none).
int DxfOf(const std::string& xml) {
  const size_t at = xml.find(" dxfId=\"");
  return at == std::string::npos ? -1 : std::atoi(xml.c_str() + at + 8);
}

}  // namespace

// ---------------------------------------------------------------------------
// Conditional formatting: a new rule, and the sheet's rules (Manage Rules)

class WorkbookView::RulesPanel : public PanelContent {
 public:
  explicit RulesPanel(WorkbookView& view) : v_(view) {
    list_.label = L"Rules on this sheet";
    list_.rowHeight = 34;
    list_.onSelect = [this](int i) { Select(i); };
    up_.text = L"Move up";
    down_.text = L"Move down";
    delete_.text = L"Delete";
    up_.onInvoke = [this] { Move(-1); };
    down_.onInvoke = [this] { Move(1); };
    delete_.onInvoke = [this] {
      if (selected_ < 0 || selected_ >= static_cast<int>(rules_.size())) return;
      rules_.erase(rules_.begin() + selected_);
      Commit(u"Delete Rule", std::min(selected_, static_cast<int>(rules_.size()) - 1));
    };
    kind_.label = L"Format cells that are";
    kind_.items = List(kKindNames);
    kind_.selected = 0;
    kind_.onChange = [this](int) { Place(); };
    for (ComboBox& b : value_) b.autoComplete = false;
    applies_.autoComplete = false;
    applies_.onCommit = [this](const std::wstring& text, int) {
      if (selected_ < 0) return;
      const std::string sq = SqrefOf(text);
      if (sq.empty()) return;
      data::SetCfSqref(rules_[static_cast<size_t>(selected_)], sq);
      Commit(u"Conditional Formatting", selected_);
    };
    stop_.text = L"Stop if true";
    stop_.toggle = true;
    stop_.onInvoke = [this] {
      if (selected_ < 0) return;
      rules_[static_cast<size_t>(selected_)].stopIfTrue = stop_.checked;
      Commit(u"Conditional Formatting", selected_);
    };
    add_.text = L"Add rule";
    add_.onInvoke = [this] { Make(false); };
    change_.text = L"Change rule";
    change_.onInvoke = [this] { Make(true); };
    clearSel_.text = L"Clear from selection";
    clearSel_.textSize = 12;
    clearSel_.onInvoke = [this] { v_.RulesCommand(CmdCfClearSelection); };
    clearAll_.text = L"Clear from sheet";
    clearAll_.textSize = 12;
    clearAll_.onInvoke = [this] { v_.RulesCommand(CmdCfClearSheet); };
  }

  std::wstring Title() const override { return L"Conditional formatting"; }

  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override {
    body_ = body;
    ui_ = ui;
    Place();
    return {&list_, &up_, &down_, &delete_, &kind_, &value_[0], &value_[1], &choice_, &format_, &applies_,
            &stop_, &add_, &change_, &clearSel_, &clearAll_};
  }

  void Draw(Canvas& c) override {
    for (const auto& [text, rc] : labels_) c.Text(text, rc, theme::P().textSecondary, 12 * ui_);
  }

  // Fresh from the sheet; `select` the rule to show (-1: a new one for the selection).
  void Open(int select) {
    Reload(select);
    if (select < 0) {
      applies_.SetText(ShownSqref(RangeName(v_.Selected())));
      kind_.selected = std::clamp(kind_.selected, 0, kKinds - 1);
    }
    Place();
  }

  void Reload(int select) {
    rules_ = data::ReadCfRules(v_.Current().Text());
    const calc::FormulaLocale fl = calc::MakeFormulaLocale(v_.text_->Locale());
    list_.items.clear();
    for (const data::CfRule& r : rules_)
      list_.items.push_back(data::DescribeCf(r, fl) + L"   \x2192   " + ShownSqref(r.sqref) + (r.stopIfTrue ? L"   (stop)" : L""));
    selected_ = select < static_cast<int>(rules_.size()) ? select : -1;
    list_.SetSelected(selected_);
    if (selected_ >= 0) Show(rules_[static_cast<size_t>(selected_)]);
  }

  // The rule the form makes, for `sqref`; false (with a message) when a value is missing.
  bool Build(int k, const std::wstring& v0, const std::wstring& v1, int choice, int format, const std::string& sqref,
             int keepDxf, data::CfRule& out) {
    const calc::FormulaLocale fl = calc::MakeFormulaLocale(v_.text_->Locale());
    const NumLocale& loc = v_.text_->Locale();
    const bool d1904 = v_.book_->Date1904();
    auto dxf = [&]() -> uint32_t {
      if (format >= static_cast<int>(std::size(data::kCfPresets)) && keepDxf >= 0) return static_cast<uint32_t>(keepDxf);
      return v_.Ed().AddDxf(data::CfPreset(std::min(format, static_cast<int>(std::size(data::kCfPresets)) - 1)));
    };
    std::string a, b;
    if (Values(k) >= 1 && k != kContains && k != kBeginsWith && k != kEndsWith && k != kNotContains) {
      a = k == kFormula ? Operand(v0.empty() || v0[0] == L'=' ? v0 : L"=" + v0, fl, loc, d1904) : Operand(v0, fl, loc, d1904);
      if (a.empty() && k != kTop && k != kTopPercent && k != kBottom && k != kBottomPercent) return false;
    }
    if (Values(k) == 2 && (b = Operand(v1, fl, loc, d1904)).empty()) return false;
    const uint32_t rank = static_cast<uint32_t>(std::max(1.0, std::floor(std::wcstod(Trim(v0).c_str(), nullptr))));
    switch (k) {
      case kContains: case kBeginsWith: case kEndsWith: case kNotContains: {
        if (Trim(v0).empty()) return false;
        const data::CfText t = k == kContains ? data::CfText::Contains : k == kBeginsWith ? data::CfText::BeginsWith
                               : k == kEndsWith ? data::CfText::EndsWith : data::CfText::NotContains;
        out = data::TextRule(sqref, t, WideToUtf8(Trim(v0)), dxf());
        return true;
      }
      case kDate: out = data::DateRule(sqref, choice, dxf()); return true;
      case kDuplicate: case kUnique: out = data::DuplicateRule(sqref, k == kUnique, dxf()); return true;
      case kTop: case kTopPercent: case kBottom: case kBottomPercent:
        out = data::TopRule(sqref, Trim(v0).empty() ? 10 : rank, k == kTopPercent || k == kBottomPercent,
                            k == kBottom || k == kBottomPercent, dxf());
        return true;
      case kAbove: case kBelow: out = data::AverageRule(sqref, k == kAbove, dxf()); return true;
      case kBars: out = data::DataBarRule(sqref, choice); return true;
      case kScales: out = data::ColorScaleRule(sqref, choice); return true;
      case kIcons: out = data::IconSetRule(sqref, choice); return true;
      case kFormula: out = data::ExpressionRule(sqref, a, dxf()); return true;
      default: out = data::CellIsRule(sqref, OpOfKind(k), a, b, dxf()); return true;
    }
  }

 private:
  void Place() {
    const float u = ui_, left = body_.left + 16 * u, right = body_.right - 16 * u, gap = 8 * u;
    float y = body_.top + 10 * u;
    labels_.clear();
    auto label = [&](const wchar_t* text) {
      labels_.push_back({text, RectF(left, y, right, y + 16 * u)});
      y += 20 * u;
    };
    const float third = (right - left - 2 * gap) / 3, half = (right - left - gap) / 2;
    label(L"Rules on this sheet, the first applying first");
    const float listBottom = std::max(y + 110 * u, std::min(y + 190 * u, body_.bottom - 400 * u));
    list_.rc = RectF(left, y, right, listBottom);
    y = listBottom + 8 * u;
    up_.rc = RectF(left, y, left + third, y + 28 * u);
    down_.rc = RectF(left + third + gap, y, left + 2 * third + gap, y + 28 * u);
    delete_.rc = RectF(right - third, y, right, y + 28 * u);
    for (PanelButton* b : {&up_, &down_, &delete_}) b->visible = selected_ >= 0;
    y += 40 * u;
    const int k = kind_.selected;
    label(selected_ >= 0 ? L"The selected rule: format cells that are" : L"New rule: format cells that are");
    kind_.rc = RectF(left, y, right, y + 30 * u);
    y += 40 * u;
    // The values, or the gallery's choices.
    const int n = Values(k);
    value_[0].visible = n >= 1;
    value_[1].visible = n == 2;
    if (n >= 1) {
      label(k == kFormula ? L"Formula" : k == kTop || k == kBottom || k == kTopPercent || k == kBottomPercent ? L"How many"
            : n == 2 ? L"From and to" : L"Value");
      value_[0].rc = RectF(left, y, n == 2 ? left + half : right, y + 30 * u);
      value_[1].rc = RectF(left + half + gap, y, right, y + 30 * u);
      y += 40 * u;
    }
    const bool gallery = k == kDate || k == kBars || k == kScales || k == kIcons;
    choice_.visible = gallery;
    if (gallery) {
      std::vector<std::wstring> items = k == kDate ? List(data::kCfPeriods) : k == kBars ? List(data::kDataBars)
                                        : k == kScales ? List(data::kColorScales)
                                                       : Wide(data::kIconSets, std::size(data::kIconSets));
      if (choice_.items != items) {
        choice_.items = std::move(items);
        choice_.selected = 0;
      }
      label(k == kDate ? L"Period" : k == kBars ? L"Fill" : k == kScales ? L"Colors" : L"Icons");
      choice_.rc = RectF(left, y, right, y + 30 * u);
      y += 40 * u;
    }
    format_.visible = HasFormat(k);
    std::vector<std::wstring> formats = List(data::kCfPresets);
    if (selected_ >= 0 && DxfOf(rules_[static_cast<size_t>(selected_)].xml) >= 0) formats.push_back(L"The rule's own format");
    if (format_.items != formats) {
      format_.items = std::move(formats);
      format_.selected = static_cast<int>(format_.items.size()) - (selected_ >= 0 && format_.items.size() > std::size(data::kCfPresets) ? 1 : static_cast<int>(format_.items.size()));
    }
    if (format_.visible) {
      label(L"With");
      format_.rc = RectF(left, y, right, y + 30 * u);
      y += 40 * u;
    }
    label(L"Applies to");
    applies_.rc = RectF(left, y, right, y + 30 * u);
    y += 38 * u;
    stop_.rc = RectF(left, y, left + half, y + 26 * u);
    stop_.visible = selected_ >= 0;
    y += 36 * u;
    add_.rc = RectF(left, y, left + half, y + 30 * u);
    change_.rc = RectF(left + half + gap, y, right, y + 30 * u);
    change_.visible = selected_ >= 0;
    y += 40 * u;
    clearSel_.rc = RectF(left, y, left + half, y + 28 * u);
    clearAll_.rc = RectF(left + half + gap, y, right, y + 28 * u);
    v_.host_.ViewInvalidate();
  }

  void Select(int i) {
    if (i == selected_ || i < 0 || i >= static_cast<int>(rules_.size())) return;
    selected_ = i;
    Show(rules_[static_cast<size_t>(i)]);
    Place();
  }

  // The selected rule in the form.
  void Show(const data::CfRule& r) {
    const calc::FormulaLocale fl = calc::MakeFormulaLocale(v_.text_->Locale());
    applies_.SetText(ShownSqref(r.sqref));
    stop_.checked = r.stopIfTrue;
    pugi::xml_document d;
    const bool ok = xlsx::ParseSlice(d, r.xml.empty() ? r.ext : r.xml);
    pugi::xml_node n = d.document_element();
    std::vector<std::string> f;
    for (pugi::xml_node c = n.first_child(); ok && c; c = c.next_sibling()) {
      const std::string_view name = c.name();
      if (name == "formula" || (name.size() > 8 && name.substr(name.size() - 8) == ":formula")) f.push_back(xlsx::TextOf(c));
    }
    auto attr = [&](const char* a) { return std::string(n.attribute(a).value()); };
    for (ComboBox& b : value_) b.SetText(L"");
    int k = kFormula;
    const std::string& t = r.type;
    if (t == "cellIs") {
      static constexpr int kByOp[] = {kBetween, kNotBetween, kEqual, kNotEqual, kGreater, kLess, kGreaterEqual, kLessEqual};
      static constexpr const char* kOps[] = {"between", "notBetween", "equal", "notEqual", "greaterThan", "lessThan",
                                             "greaterThanOrEqual", "lessThanOrEqual"};
      for (size_t i = 0; i < std::size(kOps); ++i)
        if (attr("operator") == kOps[i]) k = kByOp[i];
      for (size_t i = 0; i < f.size() && i < 2; ++i) value_[i].SetText(ShownOperand(f[i], fl));
    } else if (t == "containsText" || t == "notContainsText" || t == "beginsWith" || t == "endsWith") {
      k = t == "containsText" ? kContains : t == "notContainsText" ? kNotContains : t == "beginsWith" ? kBeginsWith : kEndsWith;
      value_[0].SetText(Utf8ToWide(attr("text")));
    } else if (t == "timePeriod") {
      k = kDate;
    } else if (t == "duplicateValues" || t == "uniqueValues") {
      k = t == "uniqueValues" ? kUnique : kDuplicate;
    } else if (t == "top10") {
      const bool bottom = n.attribute("bottom").as_bool(), percent = n.attribute("percent").as_bool();
      k = bottom ? (percent ? kBottomPercent : kBottom) : (percent ? kTopPercent : kTop);
      value_[0].SetText(Utf8ToWide(attr("rank").empty() ? std::string("10") : attr("rank")));
    } else if (t == "aboveAverage") {
      k = n.attribute("aboveAverage").as_bool(true) ? kAbove : kBelow;
    } else if (t == "dataBar" || t == "colorScale" || t == "iconSet") {
      k = t == "dataBar" ? kBars : t == "colorScale" ? kScales : kIcons;
    } else if (!f.empty()) {
      value_[0].SetText(L"=" + Utf8ToWide(calc::ToLocal(f[0], fl)));
    }
    kind_.selected = k;
    format_.items.clear();  // made again with the rule's own format
  }

  void Move(int dir) {
    const int to = selected_ + dir;
    if (selected_ < 0 || to < 0 || to >= static_cast<int>(rules_.size())) return;
    std::swap(rules_[static_cast<size_t>(selected_)], rules_[static_cast<size_t>(to)]);
    Commit(u"Conditional Formatting", to);
  }

  void Make(bool change) {
    if (change && selected_ < 0) return;
    const std::string sq = SqrefOf(applies_.Text());
    if (sq.empty()) {
      v_.host_.ViewMessage(BannerKind::Warning, L"Type the cells the rule applies to, such as A1:B10.");
      return;
    }
    const int keep = change ? DxfOf(rules_[static_cast<size_t>(selected_)].xml) : -1;
    edit::EditGroup group(v_.Ed(), change ? u"Edit Rule" : u"New Rule");
    data::CfRule r;
    if (!Build(kind_.selected, value_[0].Text(), value_[1].Text(), choice_.selected, format_.selected, sq, keep, r)) {
      v_.host_.ViewMessage(BannerKind::Warning, L"Type a value for the rule (a number, text or a formula with =).");
      return;
    }
    int at = 0;
    if (change) {
      r.stopIfTrue = rules_[static_cast<size_t>(selected_)].stopIfTrue;
      rules_[static_cast<size_t>(selected_)] = std::move(r);
      at = selected_;
    } else {
      rules_.insert(rules_.begin(), std::move(r));  // a new rule applies first, as in Excel
    }
    group.done = Commit(change ? u"Edit Rule" : u"New Rule", change ? at : -1, false);
  }

  bool Commit(const char16_t* name, int select, bool reload = true) {
    const bool ok = v_.ApplyCfRules(rules_, name);
    if (reload || !ok) Reload(select);
    else selected_ = select;
    Place();
    return ok;
  }

  WorkbookView& v_;
  D2D1_RECT_F body_{};
  float ui_ = 1;
  std::vector<data::CfRule> rules_;
  int selected_ = -1;
  ListBox list_;
  PanelButton up_, down_, delete_, stop_, add_, change_, clearSel_, clearAll_;
  Dropdown kind_, choice_, format_;
  ComboBox value_[2], applies_;
  std::vector<std::pair<const wchar_t*, D2D1_RECT_F>> labels_;
};

// ---------------------------------------------------------------------------
// Data validation

namespace {
constexpr const wchar_t* kAllow[] = {L"Any value", L"Whole number", L"Decimal", L"List",
                                     L"Date",      L"Time",         L"Text length", L"Custom"};
constexpr const wchar_t* kDvOps[] = {L"between",   L"not between", L"equal to", L"not equal to", L"greater than",
                                     L"less than", L"greater than or equal to", L"less than or equal to"};
constexpr const wchar_t* kAlerts[] = {L"Stop", L"Warning", L"Information"};
}  // namespace

class WorkbookView::ValidationPanel : public PanelContent {
 public:
  explicit ValidationPanel(WorkbookView& view) : v_(view) {
    allow_.items = List(kAllow);
    allow_.onChange = [this](int) { Place(); };
    op_.items = List(kDvOps);
    op_.onChange = [this](int) { Place(); };
    alert_.items = List(kAlerts);
    for (ComboBox* b : {&min_, &max_, &inTitle_, &inText_, &errTitle_, &errText_}) b->autoComplete = false;
    blank_.text = L"Ignore blank";
    dropDown_.text = L"In-cell dropdown";
    showIn_.text = L"Show input message";
    showErr_.text = L"Show error alert";
    for (PanelButton* b : {&blank_, &dropDown_, &showIn_, &showErr_}) b->toggle = true;
    apply_.text = L"Apply";
    apply_.onInvoke = [this] { Apply(); };
    clear_.text = L"Clear all";
    clear_.onInvoke = [this] { Clear(); };
    circle_.text = L"Circle invalid data";
    circle_.textSize = 12;
    circle_.onInvoke = [this] { v_.RulesCommand(CmdCircleInvalid); };
    uncircle_.text = L"Clear circles";
    uncircle_.textSize = 12;
    uncircle_.onInvoke = [this] { v_.RulesCommand(CmdClearCircles); };
  }

  std::wstring Title() const override { return L"Data validation"; }

  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override {
    body_ = body;
    ui_ = ui;
    Place();
    return {&allow_, &op_, &min_, &max_, &blank_, &dropDown_, &showIn_, &inTitle_, &inText_, &showErr_, &alert_,
            &errTitle_, &errText_, &apply_, &clear_, &circle_, &uncircle_};
  }

  void Draw(Canvas& c) override {
    for (const auto& [text, rc] : labels_) c.Text(text, rc, theme::P().textSecondary, 12 * ui_);
  }

  // The active cell's rule in the controls.
  void Open() {
    const calc::FormulaLocale fl = calc::MakeFormulaLocale(v_.text_->Locale());
    const Validation* r = data::ValidationAt(v_.Validations(), v_.active_.row, v_.active_.col);
    Validation v = r ? *r : Validation{};
    allow_.selected = static_cast<int>(v.type);
    op_.selected = static_cast<int>(v.op);
    if (v.type == Validation::Type::List) {
      const std::vector<std::string> items = data::ListItems(v);
      std::wstring s;
      for (const std::string& i : items) s += (s.empty() ? L"" : std::wstring(1, static_cast<wchar_t>(fl.list))) + Utf8ToWide(i);
      min_.SetText(items.empty() ? ShownOperand(v.f1, fl) : s);
    } else {
      min_.SetText(v.type == Validation::Type::Custom ? L"=" + Utf8ToWide(calc::ToLocal(v.f1, fl)) : ShownOperand(v.f1, fl));
    }
    max_.SetText(ShownOperand(v.f2, fl));
    blank_.checked = r ? v.allowBlank : true;
    dropDown_.checked = v.dropDown;
    showIn_.checked = r ? v.showInput : true;
    showErr_.checked = r ? v.showError : true;
    alert_.selected = static_cast<int>(v.alert);
    inTitle_.SetText(Utf8ToWide(v.promptTitle));
    inText_.SetText(Utf8ToWide(v.prompt));
    errTitle_.SetText(Utf8ToWide(v.errorTitle));
    errText_.SetText(Utf8ToWide(v.error));
    Place();
  }

  // The controls set as a script gives them, then Apply.
  bool Script(int allow, int op, const std::wstring& first, const std::wstring& second) {
    allow_.selected = allow;
    op_.selected = op;
    min_.SetText(first);
    max_.SetText(second);
    Validation v;
    if (!Rule(RangeName(v_.Selected()), v)) return false;
    Apply();
    return true;
  }

  // The rule the controls make, for `sqref`; false when a value is missing.
  bool Rule(const std::string& sqref, Validation& v) {
    const calc::FormulaLocale fl = calc::MakeFormulaLocale(v_.text_->Locale());
    const NumLocale& loc = v_.text_->Locale();
    const bool d1904 = v_.book_->Date1904();
    v.type = static_cast<Validation::Type>(std::max(0, allow_.selected));
    v.op = static_cast<CfOp>(std::max(0, op_.selected));
    v.sqref = sqref;
    v.allowBlank = blank_.checked;
    v.dropDown = dropDown_.checked;
    v.showInput = showIn_.checked;
    v.showError = showErr_.checked;
    v.alert = static_cast<Validation::Alert>(std::max(0, alert_.selected));
    v.promptTitle = WideToUtf8(inTitle_.Text());
    v.prompt = WideToUtf8(inText_.Text());
    v.errorTitle = WideToUtf8(errTitle_.Text());
    v.error = WideToUtf8(errText_.Text());
    const std::wstring a = Trim(min_.Text()), b = Trim(max_.Text());
    switch (v.type) {
      case Validation::Type::Any: return true;
      case Validation::Type::List: {
        if (a.empty()) return false;
        if (a[0] == L'=') return !(v.f1 = Operand(a, fl, loc, d1904)).empty();
        std::string items;
        for (wchar_t c : a) {
          if (c == static_cast<wchar_t>(fl.list)) items += ',';
          else items += WideToUtf8(std::wstring(1, c));
        }
        v.f1 = Quote(items);
        return true;
      }
      case Validation::Type::Custom:
        return !(v.f1 = Operand(a.empty() || a[0] == L'=' ? a : L"=" + a, fl, loc, d1904)).empty();
      default:
        v.f1 = Operand(a, fl, loc, d1904);
        if (v.op == CfOp::Between || v.op == CfOp::NotBetween) v.f2 = Operand(b, fl, loc, d1904);
        return !v.f1.empty() && (!(v.op == CfOp::Between || v.op == CfOp::NotBetween) || !v.f2.empty());
    }
  }

 private:
  void Place() {
    const float u = ui_, left = body_.left + 16 * u, right = body_.right - 16 * u, gap = 8 * u;
    const float half = (right - left - gap) / 2;
    float y = body_.top + 10 * u;
    labels_.clear();
    auto label = [&](const wchar_t* text, float x0, float x1) { labels_.push_back({text, RectF(x0, y, x1, y + 16 * u)}); };
    const int t = allow_.selected;
    label(L"Allow", left, right);
    y += 20 * u;
    allow_.rc = RectF(left, y, right, y + 30 * u);
    y += 38 * u;
    const bool ops = t != 0 && t != 3 && t != 7;
    op_.visible = ops;
    if (ops) {
      label(L"Data", left, right);
      y += 20 * u;
      op_.rc = RectF(left, y, right, y + 30 * u);
      y += 38 * u;
    }
    const bool two = ops && (op_.selected == 0 || op_.selected == 1);
    min_.visible = t != 0;
    max_.visible = two;
    if (t != 0) {
      label(t == 3 ? L"Source (items, or =range)" : t == 7 ? L"Formula" : two ? L"Minimum" : L"Value", left,
            two ? left + half : right);
      if (two) label(L"Maximum", left + half + gap, right);
      y += 20 * u;
      min_.rc = RectF(left, y, two ? left + half : right, y + 30 * u);
      max_.rc = RectF(left + half + gap, y, right, y + 30 * u);
      y += 38 * u;
    }
    blank_.rc = RectF(left, y, left + half, y + 26 * u);
    dropDown_.rc = RectF(left + half + gap, y, right, y + 26 * u);
    dropDown_.visible = t == 3;
    y += 34 * u;
    showIn_.rc = RectF(left, y, right, y + 26 * u);
    y += 30 * u;
    label(L"Title", left, left + half);
    label(L"Input message", left + half + gap, right);
    y += 20 * u;
    inTitle_.rc = RectF(left, y, left + half, y + 30 * u);
    inText_.rc = RectF(left + half + gap, y, right, y + 30 * u);
    y += 40 * u;
    showErr_.rc = RectF(left, y, left + half, y + 26 * u);
    alert_.rc = RectF(left + half + gap, y - 2 * u, right, y + 28 * u);
    y += 34 * u;
    label(L"Title", left, left + half);
    label(L"Error message", left + half + gap, right);
    y += 20 * u;
    errTitle_.rc = RectF(left, y, left + half, y + 30 * u);
    errText_.rc = RectF(left + half + gap, y, right, y + 30 * u);
    y += 42 * u;
    apply_.rc = RectF(left, y, left + half, y + 30 * u);
    clear_.rc = RectF(left + half + gap, y, right, y + 30 * u);
    y += 40 * u;
    circle_.rc = RectF(left, y, left + half, y + 28 * u);
    uncircle_.rc = RectF(left + half + gap, y, right, y + 28 * u);
    v_.host_.ViewInvalidate();
  }

  void Apply() {
    const std::string sq = RangeName(v_.Selected());
    Validation v;
    if (!Rule(sq, v)) {
      v_.host_.ViewMessage(BannerKind::Warning, L"Type the values the cells allow (a number, a date, items or a formula with =).");
      return;
    }
    std::vector<Validation> rules = v_.Validations();
    data::ClearValidations(rules, v_.Selected());
    const bool nothing = v.type == Validation::Type::Any && v.prompt.empty() && v.promptTitle.empty();
    if (!nothing) rules.push_back(std::move(v));
    v_.ApplyValidations(rules, u"Data Validation");
  }

  void Clear() {
    std::vector<Validation> rules = v_.Validations();
    data::ClearValidations(rules, v_.Selected());
    v_.ApplyValidations(rules, u"Data Validation");
    Open();
  }

  WorkbookView& v_;
  D2D1_RECT_F body_{};
  float ui_ = 1;
  Dropdown allow_, op_, alert_;
  ComboBox min_, max_, inTitle_, inText_, errTitle_, errText_;
  PanelButton blank_, dropDown_, showIn_, showErr_, apply_, clear_, circle_, uncircle_;
  std::vector<std::pair<const wchar_t*, D2D1_RECT_F>> labels_;
};

// A list validation's choices for the active cell (the in-cell dropdown's).
class WorkbookView::ListPanel : public PanelContent {
 public:
  explicit ListPanel(WorkbookView& view) : v_(view) {
    list_.rowHeight = 28;
    list_.onActivate = [this](int i) { Pick(i); };
    list_.onSelect = [this](int i) {
      if (!filling_) Pick(i);
    };
  }
  std::wstring Title() const override { return L"Choose a value"; }
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override {
    list_.rc = RectF(body.left + 16 * ui, body.top + 12 * ui, body.right - 16 * ui, body.bottom - 16 * ui);
    return {&list_};
  }
  void Draw(Canvas&) override {}
  void Open(std::vector<std::wstring> items, CellAddr cell) {
    cell_ = cell;
    filling_ = true;
    list_.items = std::move(items);
    list_.SetSelected(-1);
    filling_ = false;
  }

 private:
  void Pick(int i) {
    if (i < 0 || i >= static_cast<int>(list_.items.size())) return;
    const std::wstring text = list_.items[static_cast<size_t>(i)];
    v_.host_.ViewShowPanel(nullptr);
    const NumLocale& loc = v_.text_->Locale();
    ParsedValue pv;
    const edit::CellInput in = ParseValue(text, loc, v_.book_->Date1904(), pv) && pv.kind == ParsedValue::Number
                                   ? edit::CellInput::Number(pv.number)
                                   : edit::CellInput::Text(WideToUtf8(text));
    v_.EditRefused(v_.Ed().SetCell(static_cast<int>(v_.sheet_), cell_.row, cell_.col, in));
  }
  WorkbookView& v_;
  ListBox list_;
  CellAddr cell_;
  bool filling_ = false;
};

// ---------------------------------------------------------------------------
// The view's side

const std::vector<data::Validation>& WorkbookView::Validations() {
  if (validationsSheet_ != static_cast<int>(sheet_)) {
    validations_ = Ready() ? data::ReadValidations(Current().Text()) : std::vector<data::Validation>{};
    validationsSheet_ = static_cast<int>(sheet_);
  }
  return validations_;
}

void WorkbookView::RulesEdited(const std::vector<edit::Change>& changes) {
  bool shown = false;
  for (const edit::Change& c : changes) {
    if (!(c.what & (edit::kRules | edit::kShift | edit::kSheets)) || c.sheet < 0) continue;
    const size_t s = static_cast<size_t>(c.sheet);
    if (s < rules_.size()) {
      rules_[s].reset();
      rulesRead_[s] = false;
    }
    shown |= s == sheet_;
  }
  if (!shown) return;
  validationsSheet_ = -1;
  circles_.clear();
  if (rulesPanel_ && host_.ViewShownPanel() == rulesPanel_.get()) static_cast<RulesPanel*>(rulesPanel_.get())->Reload(-1);
}

bool WorkbookView::ApplyCfRules(const std::vector<data::CfRule>& rules, const char16_t* name) {
  std::string xml, ext;
  data::WriteCfRules(rules, Current().Text(), xml, ext);
  return !EditRefused(Ed().SetSheetRules(static_cast<int>(sheet_), edit::Editor::RuleKind::ConditionalFormats, xml, ext, name));
}

bool WorkbookView::ApplyValidations(const std::vector<data::Validation>& rules, const char16_t* name) {
  std::string xml, ext;
  data::WriteValidations(rules, Current().Text(), xml, ext);
  return !EditRefused(Ed().SetSheetRules(static_cast<int>(sheet_), edit::Editor::RuleKind::Validations, xml, ext, name));
}

void WorkbookView::ShowRulesPanel(bool manage) {
  if (!Ready() || preview_ || ChartSheet()) return;
  if (!rulesPanel_) rulesPanel_ = std::make_shared<RulesPanel>(*this);
  auto* p = static_cast<RulesPanel*>(rulesPanel_.get());
  p->Open(manage ? 0 : -1);
  host_.ViewShowPanel(p);
}

bool WorkbookView::RulesCommandState(int cmd, CommandState& s) {
  const bool cells = Ready() && !preview_ && !ChartSheet() && !Current().PartName().empty();
  switch (cmd) {
    case CmdCondFormat: s.label = L"New rule…"; break;
    case CmdCfManage: s.label = L"Manage rules…"; break;
    case CmdCfClearSelection: s.label = L"Clear rules from selected cells"; break;
    case CmdCfClearSheet: s.label = L"Clear rules from entire sheet"; break;
    case CmdCondFormatBox: s.label = L"Conditional formatting"; break;
    case CmdDataValidation: s.label = L"Data validation…"; break;
    case CmdCircleInvalid: s.label = L"Circle invalid data"; break;
    case CmdClearCircles: s.label = L"Clear validation circles"; s.enabled = !circles_.empty(); return true;
    case CmdValidationList: s.label = L"Pick from the list"; s.shortcut = L"Alt+Down"; break;
    case CmdCreateTable: s.label = L"Table…"; s.shortcut = L"Ctrl+T"; break;
    case CmdTableDesign: s.label = L"Table design…"; s.enabled = cells && TableIndexAt(active_) >= 0; return true;
    case CmdConvertToRange: s.label = L"Convert to range"; s.enabled = cells && TableIndexAt(active_) >= 0; return true;
    case CmdTableTotalRow: s.label = L"Total row"; s.enabled = cells && TableIndexAt(active_) >= 0; return true;
    default: return false;
  }
  s.enabled = cells;
  return true;
}

bool WorkbookView::RulesCommand(int cmd) {
  if (cmd < CmdCondFormat || cmd > CmdTableTotalRow) return false;
  if (!Ready() || preview_ || ChartSheet()) return true;
  switch (cmd) {
    case CmdCondFormat:
    case CmdCondFormatBox: ShowRulesPanel(false); break;
    case CmdCfManage: ShowRulesPanel(true); break;
    case CmdCfClearSelection:
    case CmdCfClearSheet: {
      std::vector<data::CfRule> rules = data::ReadCfRules(Current().Text());
      if (cmd == CmdCfClearSheet) rules.clear();
      else data::ClearCfRules(rules, Selected());
      ApplyCfRules(rules, u"Clear Rules");
      break;
    }
    case CmdDataValidation: {
      if (!validationPanel_) validationPanel_ = std::make_shared<ValidationPanel>(*this);
      static_cast<ValidationPanel*>(validationPanel_.get())->Open();
      host_.ViewShowPanel(validationPanel_.get());
      break;
    }
    case CmdCircleInvalid: {
      // Excel circles up to 255 cells that hold a value the rule doesn't allow.
      circles_.clear();
      const Sheet& s = Current();
      for (const data::Validation& v : Validations()) {
        if (v.type == data::Validation::Type::Any) continue;
        for (const Range& r : data::ParseAreas(v.sqref)) {
          const std::optional<Range> used = s.UsedRange();
          if (!used) break;
          const uint32_t lastRow = std::min(r.lastRow, used->lastRow), lastCol = std::min(r.lastCol, used->lastCol);
          for (uint32_t row = r.firstRow; row <= lastRow && circles_.size() < 255; ++row)
            for (uint32_t col = r.firstCol; col <= lastCol && circles_.size() < 255; ++col) {
              CellInfo info;
              const bool stored = s.GetCell(row, col, info);
              edit::CellInput in;
              if (stored) {
                in.type = info.value.type;
                in.number = info.value.number;
                in.boolean = info.value.boolean;
                if (in.type == ValueType::String) in.text = std::string(info.value.text);
              }
              if (in.type == ValueType::Empty && v.allowBlank) continue;
              const bool ok = in.type != ValueType::Error && ValidateValue(v, static_cast<int>(sheet_), CellAddr{row, col}, in, false);
              if (!ok) circles_.push_back(CellAddr{row, col});
            }
        }
      }
      host_.ViewInvalidate();
      break;
    }
    case CmdClearCircles:
      circles_.clear();
      host_.ViewInvalidate();
      break;
    case CmdValidationList: ShowValidationList(); break;
    case CmdCreateTable:
    case CmdTableDesign: ShowTablePanel(); break;
    case CmdTableTotalRow:
      if (const int i = TableIndexAt(active_); i >= 0) SetTotalRow(static_cast<size_t>(i), Current().Tables()[static_cast<size_t>(i)].totalsRows == 0);
      break;
    case CmdConvertToRange:
      if (const int i = TableIndexAt(active_); i >= 0) ConvertToRange(static_cast<size_t>(i));
      break;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Tables: Insert > Table (Ctrl+T, Ctrl+L) and the table's design

namespace {

// Excel's built-in styles in its gallery's order; "" is None.
std::vector<std::string> TableStyleNames() {
  std::vector<std::string> out{""};
  for (int i = 1; i <= 21; ++i) out.push_back("TableStyleLight" + std::to_string(i));
  for (int i = 1; i <= 28; ++i) out.push_back("TableStyleMedium" + std::to_string(i));
  for (int i = 1; i <= 11; ++i) out.push_back("TableStyleDark" + std::to_string(i));
  return out;
}
std::wstring StyleLabel(const std::string& name) {
  if (name.empty()) return L"None";
  std::wstring w = Utf8ToWide(name.rfind("TableStyle", 0) == 0 ? name.substr(10) : name);
  for (size_t i = 1; i < w.size(); ++i)
    if (iswdigit(w[i]) && !iswdigit(w[i - 1])) {
      w.insert(i, L" ");
      break;
    }
  return w;
}

// A table's name as Excel allows it: a letter, _ or \ first, then letters, digits, _ and .; not a
// cell's name, not R or C.
bool TableNameOk(const std::string& n) {
  if (n.empty() || n.size() > 255) return false;
  const auto first = static_cast<unsigned char>(n[0]);
  if (!(std::isalpha(first) || first == '_' || first == '\\' || first >= 0x80)) return false;
  for (unsigned char c : n)
    if (!(std::isalnum(c) || c == '_' || c == '.' || c == '\\' || c >= 0x80)) return false;
  CellAddr a;
  return !ParseA1(n, a) && n != "R" && n != "r" && n != "C" && n != "c";
}

}  // namespace

class WorkbookView::TablePanel : public PanelContent {
 public:
  explicit TablePanel(WorkbookView& view) : v_(view) {
    styles_ = TableStyleNames();
    style_.label = L"Table style";
    for (const std::string& s : styles_) style_.items.push_back(StyleLabel(s));
    style_.onChange = [this](int i) {
      if (index_ >= 0 && i >= 0) Change([&](Table& t) { t.style.name = styles_[static_cast<size_t>(i)]; });
    };
    range_.autoComplete = name_.autoComplete = false;
    headers_.text = L"My table has headers";
    headers_.toggle = true;
    create_.text = L"Create table";
    create_.onInvoke = [this] {
      Range r;
      std::string sq = SqrefOf(range_.Text());
      if (sq.empty() || !ParseRange(sq, r)) {
        v_.host_.ViewMessage(BannerKind::Warning, L"Type the table's cells, such as A1:D20.");
        return;
      }
      if (v_.CreateTable(r, headers_.checked, styles_[static_cast<size_t>(std::max(0, style_.selected))])) Open();
    };
    name_.onCommit = [this](const std::wstring& text, int) {
      const std::string n = WideToUtf8(Trim(text));
      if (index_ < 0 || n.empty()) return;
      if (!TableNameOk(n)) {
        v_.host_.ViewMessage(BannerKind::Warning, L"The name isn't valid: start with a letter or _, and use no spaces.");
        return;
      }
      Change([&](Table& t) { t.name = t.displayName = n; });
    };
    const wchar_t* texts[] = {L"Total row", L"Banded rows", L"Banded columns", L"First column", L"Last column", L"Filter button"};
    for (int i = 0; i < 6; ++i) {
      option_[i].text = texts[i];
      option_[i].toggle = true;
      option_[i].onInvoke = [this, i] { Option(i, option_[i].checked); };
    }
    convert_.text = L"Convert to range";
    convert_.onInvoke = [this] { v_.RulesCommand(CmdConvertToRange); };
    resize_.text = L"Resize";
    resize_.onInvoke = [this] {
      Range r;
      const std::string sq = SqrefOf(range_.Text());
      if (index_ < 0) return;
      if (sq.empty() || !ParseRange(sq, r)) {
        v_.host_.ViewMessage(BannerKind::Warning, L"Type the table's new cells, such as A1:D20.");
        return;
      }
      v_.ResizeTable(static_cast<size_t>(index_), r);
      Open();
    };
  }

  std::wstring Title() const override { return index_ >= 0 ? L"Table design" : L"Create table"; }

  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override {
    body_ = body;
    ui_ = ui;
    Place();
    std::vector<Control*> out{&range_, &headers_, &create_, &name_, &style_};
    for (PanelButton& b : option_) out.push_back(&b);
    out.push_back(&convert_);
    out.push_back(&resize_);
    return out;
  }

  void Draw(Canvas& c) override {
    for (const auto& [text, rc] : labels_) c.Text(text, rc, theme::P().textSecondary, 12 * ui_);
  }

  // The active cell's table, or a new one over the selection (or the region around the cell).
  void Open() {
    index_ = v_.TableIndexAt(v_.active_);
    if (index_ >= 0) {
      const Table& t = v_.Current().Tables()[static_cast<size_t>(index_)];
      name_.SetText(Utf8ToWide(t.name));
      const auto it = std::find(styles_.begin(), styles_.end(), t.style.name);
      style_.selected = it == styles_.end() ? -1 : static_cast<int>(it - styles_.begin());
      const bool on[] = {t.totalsRows != 0, t.style.rowStripes, t.style.columnStripes, t.style.firstColumn,
                         t.style.lastColumn, t.autoFilter};
      for (int i = 0; i < 6; ++i) option_[i].checked = on[i];
      range_.SetText(Utf8ToWide(RangeName(t.ref)));
    } else {
      const Range r = v_.DataRange();
      range_.SetText(Utf8ToWide(RangeName(r)));
      headers_.checked = data::GuessHeader(*v_.book_, v_.Current(), r);
      style_.selected = static_cast<int>(std::find(styles_.begin(), styles_.end(), "TableStyleMedium2") - styles_.begin());
    }
    Place();
  }

 private:
  void Place() {
    const float u = ui_, left = body_.left + 16 * u, right = body_.right - 16 * u, gap = 8 * u;
    const float half = (right - left - gap) / 2;
    float y = body_.top + 10 * u;
    labels_.clear();
    auto label = [&](const wchar_t* text) {
      labels_.push_back({text, RectF(left, y, right, y + 16 * u)});
      y += 20 * u;
    };
    const bool design = index_ >= 0;
    headers_.visible = create_.visible = !design;
    name_.visible = convert_.visible = resize_.visible = design;
    for (PanelButton& b : option_) b.visible = design;
    if (!design) {
      label(L"Where is the data for your table?");
      range_.rc = RectF(left, y, right, y + 30 * u);
      y += 38 * u;
      headers_.rc = RectF(left, y, right, y + 28 * u);
      y += 38 * u;
    } else {
      label(L"Table name");
      name_.rc = RectF(left, y, right, y + 30 * u);
      y += 40 * u;
    }
    label(L"Style");
    style_.rc = RectF(left, y, right, y + 30 * u);
    y += 42 * u;
    if (!design) {
      create_.rc = RectF(left, y, left + half, y + 30 * u);
    } else {
      label(L"Table style options");
      for (int i = 0; i < 6; ++i) {
        const float x = i % 2 ? left + half + gap : left;
        option_[i].rc = RectF(x, y, x + half, y + 28 * u);
        if (i % 2) y += 36 * u;
      }
      y += 8 * u;
      label(L"Resize table: the new range");
      range_.rc = RectF(left, y, left + half, y + 30 * u);
      resize_.rc = RectF(left + half + gap, y, right, y + 30 * u);
      y += 42 * u;
      convert_.rc = RectF(left, y, left + half, y + 30 * u);
    }
    v_.host_.ViewInvalidate();
  }

  void Change(const std::function<void(Table&)>& change) {
    if (index_ < 0) return;
    v_.ChangeTable(static_cast<size_t>(index_), change, u"Table");
    Open();
  }

  void Option(int i, bool on) {
    if (index_ < 0) return;
    if (i == 0) {
      v_.SetTotalRow(static_cast<size_t>(index_), on);
      Open();
      return;
    }
    if (i == 5) {
      v_.SetTableButtons(static_cast<size_t>(index_), on);
      Open();
      return;
    }
    Change([&](Table& t) {
      switch (i) {
        case 1: t.style.rowStripes = on; break;
        case 2: t.style.columnStripes = on; break;
        case 3: t.style.firstColumn = on; break;
        default: t.style.lastColumn = on; break;
      }
    });
  }

  WorkbookView& v_;
  D2D1_RECT_F body_{};
  float ui_ = 1;
  int index_ = -1;
  std::vector<std::string> styles_;
  ComboBox range_, name_;
  Dropdown style_;
  PanelButton resize_;
  PanelButton headers_, create_, option_[6], convert_;
  std::vector<std::pair<const wchar_t*, D2D1_RECT_F>> labels_;
};

int WorkbookView::TableIndexAt(CellAddr at) {
  if (!Ready()) return -1;
  const std::vector<Table>& tables = Current().Tables();
  for (size_t i = 0; i < tables.size(); ++i) {
    const Range& r = tables[i].ref;
    if (at.row >= r.firstRow && at.row <= r.lastRow && at.col >= r.firstCol && at.col <= r.lastCol) return static_cast<int>(i);
  }
  return -1;
}

void WorkbookView::ShowTablePanel() {
  if (!Ready() || preview_ || ChartSheet()) return;
  if (!tablePanel_) tablePanel_ = std::make_shared<TablePanel>(*this);
  static_cast<TablePanel*>(tablePanel_.get())->Open();
  host_.ViewShowPanel(tablePanel_.get());
}

bool WorkbookView::CreateTable(Range r, bool headers, const std::string& style) {
  const int sheet = static_cast<int>(sheet_);
  if (TableIndexAt({r.firstRow, r.firstCol}) >= 0 || r.lastCol - r.firstCol >= 16384) return false;
  edit::EditGroup group(Ed(), u"Create Table");
  // The sheet's filter goes, as in Excel, its hidden rows shown.
  if (const std::optional<SheetFilter>& f = Current().Filter();
      f && !(f->ref.lastRow < r.firstRow || f->ref.firstRow > r.lastRow || f->ref.lastCol < r.firstCol || f->ref.firstCol > r.lastCol)) {
    const Range fr = f->ref;
    if (EditRefused(Ed().SetFilter(sheet, std::nullopt)) || EditRefused(Ed().HideRows(sheet, fr.firstRow, fr.lastRow, false))) return false;
  }
  const Sheet& s = Current();
  std::vector<std::pair<CellAddr, edit::CellInput>> cells;
  if (!headers) {
    // A row of headers above the data: the table's columns move down one row.
    if (EditRefused(Ed().InsertCells(sheet, Range{r.firstRow, r.firstCol, r.firstRow, r.lastCol}, edit::Editor::Direction::DownOrUp)))
      return false;
    ++r.lastRow;
  }
  // The headers: text, unique without case, Excel's ColumnN for an empty one.
  Table t;
  t.ref = r;
  t.autoFilter = true;
  t.totalsRowShown = false;
  t.style = TableStyleInfo{style, false, false, true, false};
  std::vector<std::string> names;
  for (uint32_t col = r.firstCol; col <= r.lastCol; ++col) {
    const Value v = s.GetValue(r.firstRow, col);
    std::string n;
    if (headers && v.type == ValueType::String) n = std::string(v.text);
    else if (headers && v.type == ValueType::Number) {
      char buf[40];
      std::snprintf(buf, sizeof buf, "%.15g", v.number);
      n = buf;
    }
    else if (headers && v.type == ValueType::Bool) n = v.boolean ? "TRUE" : "FALSE";
    if (n.empty()) n = "Column" + std::to_string(col - r.firstCol + 1);
    const std::string base = n;
    for (int k = 2; std::any_of(names.begin(), names.end(), [&](const std::string& x) { return NameEqualsNoCase(x, n); }); ++k)
      n = base + std::to_string(k);
    names.push_back(n);
    if (!(v.type == ValueType::String && std::string(v.text) == n)) cells.emplace_back(CellAddr{r.firstRow, col}, edit::CellInput::Text(n));
    t.columns.push_back(TableColumn{0, n});
  }
  if (!cells.empty() && EditRefused(Ed().SetCells(sheet, cells))) return false;
  if (EditRefused(Ed().AddTable(sheet, t))) return false;
  group.done = true;
  host_.ViewInvalidate();
  return true;
}

bool WorkbookView::ChangeTable(size_t index, const std::function<void(Table&)>& change, const char16_t* name) {
  if (index >= Current().Tables().size()) return false;
  Table t = Current().Tables()[index];
  change(t);
  edit::EditGroup group(Ed(), name);
  if (EditRefused(Ed().SetTable(static_cast<int>(sheet_), index, t))) return false;
  group.done = true;
  host_.ViewInvalidate();
  return true;
}

// A column a table takes (resized, or grown by typing beside it): its header cell's text, a
// number or TRUE/FALSE as text; taken (without case), with 2, 3... after it; none, Excel's first
// free ColumnN (measured: not its position).
static std::string NewColumnName(const Value& header, const std::function<bool(const std::string&)>& taken) {
  std::string n;
  if (header.type == ValueType::String) n = std::string(header.text);
  else if (header.type == ValueType::Number) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.15g", header.number);
    n = buf;
  } else if (header.type == ValueType::Bool) n = header.boolean ? "TRUE" : "FALSE";
  if (n.empty()) {
    for (int k = 1;; ++k)
      if (!taken(n = "Column" + std::to_string(k))) return n;
  }
  const std::string base = n;
  for (int k = 2; taken(n); ++k) n = base + std::to_string(k);
  return n;
}

bool WorkbookView::ResizeTable(size_t index, Range r) {
  if (!Ready() || index >= Current().Tables().size()) return false;
  const int sheet = static_cast<int>(sheet_);
  const Table t = Current().Tables()[index];
  const Range old = t.ref;
  const bool overlaps = !(r.lastRow < old.firstRow || r.firstRow > old.lastRow || r.lastCol < old.firstCol || r.firstCol > old.lastCol);
  if (r.firstRow != old.firstRow || !overlaps) {
    host_.ViewMessage(BannerKind::Warning, L"The headers must remain in the same row, and the resulting table range must overlap the original table range.");
    return false;
  }
  // The total row: to the new last row; when that's above the old one, just below the range.
  Range now = r;
  if (t.totalsRows && r.lastRow < old.lastRow) now.lastRow = r.lastRow + 1;
  if (now.lastRow < now.firstRow + t.headerRows + t.totalsRows || now.lastCol - now.firstCol >= 16384) {
    host_.ViewMessage(BannerKind::Warning, L"A table needs a row of data below its headers.");
    return false;
  }
  if (now == old) return true;
  edit::EditGroup group(Ed(), u"Resize Table");
  edit::Result res;
  if (t.totalsRows && now.lastRow != old.lastRow) {
    // The rows between the old and the new total row turn: the total row's cells to its new row,
    // the others one row toward the old one (Excel moves them so).
    const uint32_t from = std::min(old.lastRow, now.lastRow), to = std::max(old.lastRow, now.lastRow);
    const uint32_t n = to - from + 1;
    std::vector<uint32_t> order(n);
    for (uint32_t i = 0; i < n; ++i) order[i] = now.lastRow > old.lastRow ? (i + 1) % n : (i + n - 1) % n;
    res = Ed().SortRows(sheet, Range{from, old.firstCol, to, old.lastCol}, order);
  }
  // The columns: those kept as they are, new ones named by their header cell (unique without
  // case) or Excel's first free ColumnN, written there.
  Table next = t;
  next.ref = now;
  next.columns.clear();
  std::vector<std::pair<CellAddr, edit::CellInput>> headers;
  const Sheet& s = Current();
  auto taken = [&](const std::string& n) {
    for (uint32_t col = now.firstCol; col <= now.lastCol; ++col)
      if (col >= old.firstCol && col <= old.lastCol && NameEqualsNoCase(t.columns[col - old.firstCol].name, n)) return true;
    return std::any_of(next.columns.begin(), next.columns.end(), [&](const TableColumn& c) { return NameEqualsNoCase(c.name, n); });
  };
  for (uint32_t col = now.firstCol; res && col <= now.lastCol; ++col) {
    if (col >= old.firstCol && col <= old.lastCol) {
      next.columns.push_back(t.columns[col - old.firstCol]);
      continue;
    }
    const std::string n = NewColumnName(t.headerRows ? s.GetValue(now.firstRow, col) : Value{}, taken);
    if (t.headerRows) {
      const Value v = s.GetValue(now.firstRow, col);
      if (!(v.type == ValueType::String && std::string(v.text) == n)) headers.emplace_back(CellAddr{now.firstRow, col}, edit::CellInput::Text(n));
    }
    next.columns.push_back(TableColumn{0, n});
  }
  if (res && !headers.empty()) res = Ed().SetCells(sheet, headers);
  if (res) res = Ed().SetTable(sheet, index, next);
  if (EditRefused(res)) return false;
  group.done = true;
  host_.ViewInvalidate();
  return true;
}

bool WorkbookView::SetTotalRow(size_t index, bool on) {
  if (index >= Current().Tables().size()) return false;
  const int sheet = static_cast<int>(sheet_);
  Table t = Current().Tables()[index];
  if ((t.totalsRows != 0) == on) return true;
  edit::EditGroup group(Ed(), u"Total Row");
  const Sheet& s = Current();
  if (on) {
    const uint32_t row = t.ref.lastRow + 1;
    if (row >= kMaxRows) return false;
    bool filled = false;
    for (uint32_t col = t.ref.firstCol; col <= t.ref.lastCol && !filled; ++col) filled = s.GetValue(row, col).type != ValueType::Empty;
    if (filled) {  // the cells below move down to make room, as in Excel
      if (EditRefused(Ed().InsertCells(sheet, Range{row, t.ref.firstCol, row, t.ref.lastCol}, edit::Editor::Direction::DownOrUp)))
        return false;
    }
    // Excel's: "Total" in the first column, the last one's sum (a count when it isn't numbers).
    const size_t last = t.columns.size() - 1;
    bool numbers = false;
    for (uint32_t r = t.ref.firstRow + t.headerRows; r <= t.ref.lastRow && !numbers; ++r)
      numbers = s.GetValue(r, t.ref.lastCol).type == ValueType::Number;
    std::string col = t.columns[last].name;
    std::string quoted;
    for (char c : col) {
      if (c == '[' || c == ']' || c == '#' || c == '\'') quoted += '\'';
      quoted += c;
    }
    std::vector<std::pair<CellAddr, edit::CellInput>> cells;
    if (last > 0) cells.emplace_back(CellAddr{row, t.ref.firstCol}, edit::CellInput::Text("Total"));
    cells.emplace_back(CellAddr{row, t.ref.lastCol},
                       edit::CellInput::Formula("SUBTOTAL(" + std::string(numbers ? "109" : "103") + "," + t.name + "[" + quoted + "])"));
    if (EditRefused(Ed().SetCells(sheet, cells))) return false;
    t = Current().Tables()[index];  // the cells' bookkeeping may have touched the columns
    t.ref.lastRow = row;
    t.totalsRows = 1;
    t.totalsRowShown = true;
    if (last > 0) t.columns[0].totalsLabel = "Total";
    t.columns[last].totalsFunction = numbers ? "sum" : "count";
    t.columns[last].totalsFormula.clear();
  } else {
    const uint32_t row = t.ref.lastRow;
    t.ref.lastRow = row - 1;
    t.totalsRows = 0;
    if (EditRefused(Ed().SetTable(sheet, index, t))) return false;
    if (EditRefused(Ed().ClearAll(sheet, Range{row, t.ref.firstCol, row, t.ref.lastCol}))) return false;
    group.done = true;
    host_.ViewInvalidate();
    return true;
  }
  if (EditRefused(Ed().SetTable(sheet, index, t))) return false;
  group.done = true;
  host_.ViewInvalidate();
  return true;
}

bool WorkbookView::ConvertToRange(size_t index) {
  if (index >= Current().Tables().size()) return false;
  const Table t = Current().Tables()[index];
  const int home = static_cast<int>(sheet_);
  const std::string homeName = Current().Name();
  edit::EditGroup group(Ed(), u"Convert to Range");
  // Formulas that refer to the table refer to its cells instead, as Excel writes them.
  for (size_t si = 0; si < book_->SheetCount(); ++si) {
    if (!book_->GetSheet(si).Loaded() && !book_->LoadSheet(si).Ok()) continue;
    const Sheet& s = book_->GetSheet(si);
    const std::optional<Range> used = s.UsedRange();
    if (!used || s.FormulaCount() == 0) continue;
    std::vector<std::pair<CellAddr, edit::CellInput>> cells;
    s.ForEachCell(*used, [&](const CellInfo& c) {
      if (!c.hasFormula) return true;
      FormulaInfo fi;
      if (!s.GetFormula(c.row, c.col, fi) || (fi.kind != FormulaKind::Normal && fi.kind != FormulaKind::Shared)) return true;
      const bool inside = static_cast<int>(si) == home && c.row >= t.ref.firstRow && c.row <= t.ref.lastRow &&
                          c.col >= t.ref.firstCol && c.col <= t.ref.lastCol;
      std::string text(fi.text);
      if (!inside && std::search(text.begin(), text.end(), t.name.begin(), t.name.end(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
          }) == text.end())
        return true;
      if (fi.origin.row != c.row || fi.origin.col != c.col) {  // a shared formula's cell: its own text first
        calc::CompileOptions from, to;
        from.sheet = to.sheet = static_cast<int>(si);
        from.row = static_cast<int32_t>(fi.origin.row), from.col = static_cast<int32_t>(fi.origin.col);
        to.row = static_cast<int32_t>(c.row), to.col = static_cast<int32_t>(c.col);
        text = calc::CopyFormula("=" + text, from, to, Calc().engine());
        if (!text.empty() && text[0] == '=') text.erase(0, 1);
      }
      const std::string now = data::Unstructure(text, t, homeName, c.row, inside);
      if (now != text) cells.emplace_back(CellAddr{c.row, c.col}, edit::CellInput::Formula(now));
      return true;
    });
    if (!cells.empty() && EditRefused(Ed().SetCells(static_cast<int>(si), cells))) return false;
  }
  if (EditRefused(Ed().RemoveTable(home, index))) return false;
  group.done = true;
  if (tablePanel_ && host_.ViewShownPanel() == tablePanel_.get()) host_.ViewShowPanel(nullptr);
  host_.ViewInvalidate();
  return true;
}

void WorkbookView::AutoExpandTable(int sheet, CellAddr cell) {
  if (sheet != static_cast<int>(sheet_) || !Ready()) return;
  const Sheet& s = Current();
  if (s.GetValue(cell.row, cell.col).type == ValueType::Empty) {
    FormulaInfo fi;
    if (!s.GetFormula(cell.row, cell.col, fi)) return;
  }
  const std::vector<Table>& tables = s.Tables();
  // A formula typed in an empty column of a table fills the column: Excel's calculated column, a
  // step of its own after the formula.
  if (FormulaInfo typed; s.GetFormula(cell.row, cell.col, typed) && typed.kind == FormulaKind::Normal)
    for (size_t i = 0; i < tables.size(); ++i) {
      const Table& t = tables[i];
      const Range data = t.Data();
      if (cell.row < data.firstRow || cell.row > data.lastRow || cell.col < data.firstCol || cell.col > data.lastCol ||
          data.firstRow == data.lastRow)
        continue;
      bool empty = true;
      for (uint32_t r = data.firstRow; r <= data.lastRow && empty; ++r)
        if (r != cell.row) {
          FormulaInfo f;
          empty = s.GetValue(r, cell.col).type == ValueType::Empty && !s.GetFormula(r, cell.col, f);
        }
      if (!empty) return;
      const std::string text(typed.text);
      edit::EditGroup group(Ed(), u"Calculated Column");
      std::vector<std::pair<CellAddr, edit::CellInput>> cells;
      calc::CompileOptions from;
      from.sheet = sheet, from.row = static_cast<int32_t>(cell.row), from.col = static_cast<int32_t>(cell.col);
      auto at = [&](uint32_t r) {
        calc::CompileOptions to = from;
        to.row = static_cast<int32_t>(r);
        std::string f = calc::CopyFormula("=" + text, from, to, Calc().engine());
        if (!f.empty() && f[0] == '=') f.erase(0, 1);
        return f;
      };
      for (uint32_t r = data.firstRow; r <= data.lastRow; ++r)
        if (r != cell.row) cells.emplace_back(CellAddr{r, cell.col}, edit::CellInput::Formula(at(r)));
      if (EditRefused(Ed().SetCells(sheet, cells))) return;
      Table now = Current().Tables()[i];
      now.columns[cell.col - t.ref.firstCol].calculatedFormula = cell.row == data.firstRow ? text : at(data.firstRow);  // as the first row reads it
      if (EditRefused(Ed().SetTable(sheet, i, now))) return;
      group.done = true;
      return;
    }
  for (size_t i = 0; i < tables.size(); ++i) {
    Table t = tables[i];
    const bool below = !t.totalsRows && cell.row == t.ref.lastRow + 1 && cell.col >= t.ref.firstCol && cell.col <= t.ref.lastCol;
    const bool right = cell.col == t.ref.lastCol + 1 && cell.row >= t.ref.firstRow && cell.row <= t.ref.lastRow;
    if (!below && !right) continue;
    // Excel's AutoCorrect: the table takes the new row or column, as a step of its own after the value.
    edit::EditGroup group(Ed(), u"Table AutoExpansion");
    if (below) {
      ++t.ref.lastRow;
    } else {
      const Value h = t.headerRows ? s.GetValue(t.ref.firstRow, cell.col) : Value{};
      const std::string name = NewColumnName(h, [&](const std::string& n) { return t.FindColumn(n) >= 0; });
      if (t.headerRows && !(h.type == ValueType::String && std::string(h.text) == name) &&
          EditRefused(Ed().SetCell(sheet, t.ref.firstRow, cell.col, edit::CellInput::Text(name))))
        return;
      ++t.ref.lastCol;
      t.columns.push_back(TableColumn{0, name});
    }
    if (EditRefused(Ed().SetTable(sheet, i, t))) return;
    group.done = true;
    return;
  }
}

bool WorkbookView::TableScriptOp(const std::wstring& arg) {
  // table:create[,noheaders][,<style>]; table:total; table:style,<name>; table:option,<rows|cols|first|last|filter>;
  // table:name,<name>; table:convert; table:resize,<range>. On the active cell's table (create: the selection).
  std::vector<std::wstring> a;
  for (size_t i = 0;;) {
    const size_t e = arg.find(L',', i);
    a.push_back(arg.substr(i, e == std::wstring::npos ? std::wstring::npos : e - i));
    if (e == std::wstring::npos) break;
    i = e + 1;
  }
  while (a.size() < 3) a.emplace_back();
  if (a[0] == L"create") {
    const bool headers = a[1] != L"noheaders";
    const std::wstring style = a[1].rfind(L"TableStyle", 0) == 0 ? a[1] : a[2];
    return CreateTable(DataRange(), headers, style.empty() ? "TableStyleMedium2" : WideToUtf8(style));
  }
  const int index = TableIndexAt(active_);
  if (index < 0) return false;
  const size_t i = static_cast<size_t>(index);
  if (a[0] == L"total") return SetTotalRow(i, Current().Tables()[i].totalsRows == 0);
  if (a[0] == L"convert") return RulesCommand(CmdConvertToRange);
  if (a[0] == L"resize") {
    Range r;
    return ParseRange(WideToUtf8(a[1]), r) && ResizeTable(i, r);
  }
  if (a[0] == L"style") return ChangeTable(i, [&](Table& t) { t.style.name = WideToUtf8(a[1]); }, u"Table");
  if (a[0] == L"name") return TableNameOk(WideToUtf8(a[1])) && ChangeTable(i, [&](Table& t) { t.name = t.displayName = WideToUtf8(a[1]); }, u"Table");
  if (a[0] == L"option" && a[1] == L"filter") return SetTableButtons(i, !Current().Tables()[i].autoFilter);
  if (a[0] == L"option")
    return ChangeTable(i, [&](Table& t) {
      if (a[1] == L"rows") t.style.rowStripes = !t.style.rowStripes;
      else if (a[1] == L"cols") t.style.columnStripes = !t.style.columnStripes;
      else if (a[1] == L"first") t.style.firstColumn = !t.style.firstColumn;
      else if (a[1] == L"last") t.style.lastColumn = !t.style.lastColumn;
      else t.autoFilter = !t.autoFilter;
    }, u"Table");
  return false;
}

// ---------------------------------------------------------------------------
// Pasting

namespace {
struct Rebase {
  calc::Engine* engine;
  calc::CompileOptions from, to;
};
std::string RebaseFormula(void* ctx, std::string_view f) {
  auto* r = static_cast<Rebase*>(ctx);
  std::string out = calc::CopyFormula("=" + std::string(f), r->from, r->to, *r->engine);
  if (!out.empty() && out[0] == '=') out.erase(0, 1);
  return out.empty() ? std::string(f) : out;
}
bool SameRules(const std::vector<data::CfRule>& a, const std::vector<data::CfRule>& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const data::CfRule& x, const data::CfRule& y) {
           return x.sqref == y.sqref && x.xml == y.xml && x.ext == y.ext;
         });
}
}  // namespace

edit::Result WorkbookView::PasteRules(int fromSheet, const Range& src, int sheet, const Range& pasted, bool validations) {
  if (fromSheet < 0 || static_cast<size_t>(fromSheet) >= book_->SheetCount() || book_->GetSheet(static_cast<size_t>(sheet)).PartName().empty())
    return {};
  const Sheet& fs = book_->GetSheet(static_cast<size_t>(fromSheet));
  const Sheet& ds = book_->GetSheet(static_cast<size_t>(sheet));
  const uint32_t h = src.lastRow - src.firstRow + 1, w = src.lastCol - src.firstCol + 1;
  // The copies' top-left corners.
  std::vector<CellAddr> corners;
  for (uint32_t r = pasted.firstRow; r + h - 1 <= pasted.lastRow; r += h)
    for (uint32_t c = pasted.firstCol; c + w - 1 <= pasted.lastCol; c += w) corners.push_back({r, c});
  // A source area's part inside the copied cells, moved to a copy.
  auto inside = [&](const std::vector<Range>& areas) {
    std::vector<Range> out;
    for (const Range& a : areas) {
      const Range i{std::max(a.firstRow, src.firstRow), std::max(a.firstCol, src.firstCol), std::min(a.lastRow, src.lastRow),
                    std::min(a.lastCol, src.lastCol)};
      if (i.firstRow <= i.lastRow && i.firstCol <= i.lastCol) out.push_back(i);
    }
    return out;
  };
  auto moved = [&](const std::vector<Range>& parts, CellAddr corner) {
    std::vector<Range> out;
    for (Range p : parts) {
      p.firstRow = p.firstRow - src.firstRow + corner.row, p.lastRow = p.lastRow - src.firstRow + corner.row;
      p.firstCol = p.firstCol - src.firstCol + corner.col, p.lastCol = p.lastCol - src.firstCol + corner.col;
      out.push_back(p);
    }
    return out;
  };
  Rebase rb{&Calc().engine(), {}, {}};
  auto rebase = [&](const std::vector<Range>& from, const std::vector<Range>& to) {
    rb.from.sheet = fromSheet, rb.from.row = static_cast<int32_t>(from[0].firstRow), rb.from.col = static_cast<int32_t>(from[0].firstCol);
    rb.to.sheet = sheet, rb.to.row = static_cast<int32_t>(to[0].firstRow), rb.to.col = static_cast<int32_t>(to[0].firstCol);
  };
  edit::Result r;
  // Conditional formats: the source's rules come first, as new rules do.
  {
    const std::vector<data::CfRule> before = data::ReadCfRules(ds.Text());
    std::vector<data::CfRule> rules = before, copies;
    data::ClearCfRules(rules, pasted);
    for (const data::CfRule& rule : data::ReadCfRules(fs.Text())) {
      const std::vector<Range> areas = data::ParseAreas(rule.sqref), parts = inside(areas);
      if (parts.empty()) continue;
      std::vector<Range> all;
      for (CellAddr corner : corners) {
        const std::vector<Range> m = moved(parts, corner);
        all.insert(all.end(), m.begin(), m.end());
      }
      // Relative references in a rule are from its cells' top-left: the original's, then the copy's.
      rebase(areas, all);
      copies.push_back(data::CopyCfRule(rule, data::JoinAreas(all), RebaseFormula, &rb));
    }
    rules.insert(rules.begin(), copies.begin(), copies.end());
    if (!SameRules(rules, before)) {
      std::string xml, ext;
      data::WriteCfRules(rules, ds.Text(), xml, ext);
      r = Ed().SetSheetRules(sheet, edit::Editor::RuleKind::ConditionalFormats, xml, ext, u"Paste");
      if (!r) return r;
    }
  }
  if (!validations) return r;
  const std::vector<data::Validation> before = data::ReadValidations(ds.Text());
  std::vector<data::Validation> rules = before;
  data::ClearValidations(rules, pasted);
  bool changed = rules.size() != before.size() ||
                 !std::equal(rules.begin(), rules.end(), before.begin(), [](const data::Validation& a, const data::Validation& b) { return a.sqref == b.sqref; });
  for (const data::Validation& v : data::ReadValidations(fs.Text())) {
    const std::vector<Range> areas = data::ParseAreas(v.sqref), parts = inside(areas);
    if (parts.empty()) continue;
    std::vector<Range> all;
    for (CellAddr corner : corners) {
      const std::vector<Range> m = moved(parts, corner);
      all.insert(all.end(), m.begin(), m.end());
    }
    rebase(areas, all);
    rules.push_back(data::CopyValidation(v, data::JoinAreas(all), RebaseFormula, &rb));
    changed = true;
  }
  if (!changed) return r;
  std::string xml, ext;
  data::WriteValidations(rules, ds.Text(), xml, ext);
  return Ed().SetSheetRules(sheet, edit::Editor::RuleKind::Validations, xml, ext, u"Paste");
}

// ---------------------------------------------------------------------------
// Checking values

namespace {

bool Compare(CfOp op, double x, double a, double b) {
  switch (op) {
    case CfOp::Between: return x >= std::min(a, b) && x <= std::max(a, b);
    case CfOp::NotBetween: return x < std::min(a, b) || x > std::max(a, b);
    case CfOp::Equal: return x == a;
    case CfOp::NotEqual: return x != a;
    case CfOp::Greater: return x > a;
    case CfOp::Less: return x < a;
    case CfOp::GreaterEqual: return x >= a;
    case CfOp::LessEqual: return x <= a;
  }
  return true;
}

// As Excel 2021 compares a value with a list's items: with case, spaces around either ignored.
bool SameText(std::string_view a, std::string_view b) { return Trim(a) == Trim(b); }

// A rule's formula worked out at a cell: a number, text or a range's values (lists).
bool EvalAt(calc::WorkbookCalc& calc, const std::string& formula, int sheet, CellAddr at, calc::Value& out) {
  if (formula.empty()) return false;
  const calc::Cell cell{sheet, static_cast<int32_t>(at.row), static_cast<int32_t>(at.col)};
  std::shared_ptr<const calc::Formula> f = calc.engine().CompileAt(formula, cell);
  if (!f) return false;
  calc::Evaluator ev(calc, &calc.engine());
  return ev.Evaluate(*f, cell, out) == calc::EvalResult::kDone;
}

std::string TextOfInput(const edit::CellInput& in) {
  if (in.type == ValueType::Number) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.15g", in.number);
    return buf;
  }
  if (in.type == ValueType::Bool) return in.boolean ? "TRUE" : "FALSE";
  return in.text;
}

const data::Validation* ListRule(const std::vector<data::Validation>& rules, CellAddr at) {
  const data::Validation* v = data::ValidationAt(rules, at.row, at.col);
  return v && v->type == data::Validation::Type::List && v->dropDown ? v : nullptr;
}

}  // namespace

bool WorkbookView::ValidateValue(const data::Validation& v, int sheet, CellAddr cell, const edit::CellInput& in, bool typed) {
  using T = data::Validation::Type;
  if (v.type == T::Any) return true;
  if (in.type == ValueType::Empty && in.formula.empty()) return v.allowBlank || typed;  // a cleared cell is allowed
  if (!in.formula.empty()) return true;  // a formula's result isn't known yet
  auto number = [&](const std::string& f, double& out) {
    char* end = nullptr;
    out = std::strtod(f.c_str(), &end);
    if (!f.empty() && end && *end == 0) return true;
    calc::Value r;
    if (!EvalAt(Calc(), f, sheet, cell, r) || !r.IsNumber()) return false;
    out = r.number();
    return true;
  };
  const bool two = v.op == CfOp::Between || v.op == CfOp::NotBetween;
  double a = 0, b = 0;
  switch (v.type) {
    case T::Whole: case T::Decimal: case T::Date: case T::Time: {
      if (in.type != ValueType::Number) return false;
      if (v.type == T::Whole && in.number != std::floor(in.number)) return false;
      if (!number(v.f1, a) || (two && !number(v.f2, b))) return true;  // bounds that don't work out allow anything
      return Compare(v.op, in.number, a, b);
    }
    case T::TextLength: {
      if (!number(v.f1, a) || (two && !number(v.f2, b))) return true;
      return Compare(v.op, static_cast<double>(Utf8ToWide(TextOfInput(in)).size()), a, b);
    }
    case T::List: {
      const std::string text = TextOfInput(in);
      if (v.f1.size() >= 2 && v.f1.front() == '"') {
        const std::vector<std::string> items = data::ListItems(v);
        return data::InList(items, text);
      }
      calc::Value r;
      if (!EvalAt(Calc(), v.f1, sheet, cell, r)) return true;
      auto match = [&](const calc::Value& x) {
        if (x.IsNumber()) return in.type == ValueType::Number && x.number() == in.number;
        if (x.IsText()) return SameText(x.text(), text);
        if (x.IsBool()) return in.type == ValueType::Bool && x.boolean() == in.boolean;
        return false;
      };
      if (r.IsArray()) {
        const calc::Array& arr = r.array();
        for (int32_t i = 0; i < arr.size(); ++i)
          if (match(arr.data()[i])) return true;
        return false;
      }
      return match(r);
    }
    case T::Custom: {
      // The formula reads the cell, so it's worked out with the value in it, which is then taken out.
      edit::EditGroup trial(Ed(), u"Validation");
      if (!Ed().SetCell(sheet, cell.row, cell.col, in).ok) return true;
      calc::Value r;
      if (!EvalAt(Calc(), v.f1, sheet, cell, r)) return true;
      return r.IsBool() ? r.boolean() : r.IsNumber() ? r.number() != 0 : false;
    }
    default: return true;
  }
}

bool WorkbookView::ValidateEntry(int sheet, CellAddr cell, const edit::CellInput& in) {
  if (sheet != static_cast<int>(sheet_)) return true;
  const data::Validation* v = data::ValidationAt(Validations(), cell.row, cell.col);
  if (!v || !v->showError || v->type == data::Validation::Type::Any) return true;
  const data::Validation rule = *v;  // a custom rule's check edits, and the list is read again
  if (ValidateValue(rule, sheet, cell, in, true)) return true;
  std::wstring text = rule.error.empty() ? L"This value doesn't match the data validation restrictions defined for this cell."
                                         : Utf8ToWide(rule.error);
  if (!rule.errorTitle.empty()) text = Utf8ToWide(rule.errorTitle) + L": " + text;
  switch (rule.alert) {
    case data::Validation::Alert::Warning: return host_.ViewConfirm(text + L" Continue?", L"Yes");
    case data::Validation::Alert::Information: return host_.ViewConfirm(text, L"OK");
    default:
      host_.ViewMessage(BannerKind::Warning, text);
      return false;
  }
}

// ---------------------------------------------------------------------------
// On the grid: the input message, the list's button, the circles

bool WorkbookView::ValidationButtonAt(float x, float y) {
  if (!Ready() || preview_ || ChartSheet() || !ListRule(Validations(), active_)) return false;
  for (const Pane& p : Panes()) {
    if (!gfx::Contains(RectF(p.rc), x, y)) continue;
    const D2D1_RECT_F cell = CellRect(p, active_.row, active_.col, active_.row, active_.col);
    const float s = std::min(cell.bottom - cell.top, std::round(18 * ui_));
    if (gfx::Contains(RectF(cell.right + 1, cell.bottom - s, cell.right + 1 + s, cell.bottom), x, y)) return true;
  }
  return false;
}

void WorkbookView::ShowValidationList() {
  const data::Validation* v = Ready() ? ListRule(Validations(), active_) : nullptr;
  if (!v) return;
  std::vector<std::wstring> items;
  if (v->f1.size() >= 2 && v->f1.front() == '"') {
    for (const std::string& i : data::ListItems(*v)) items.push_back(Utf8ToWide(Trim(i)));
  } else if (calc::Value r; EvalAt(Calc(), v->f1, static_cast<int>(sheet_), active_, r)) {
    auto add = [&](const calc::Value& x) {
      if (x.IsText()) {
        items.push_back(Utf8ToWide(x.text()));
      } else if (x.IsNumber()) {
        wchar_t buf[40];
        std::swprintf(buf, 40, L"%.15g", x.number());
        items.push_back(buf);
      }
    };
    if (r.IsArray()) {
      for (int32_t i = 0; i < r.array().size(); ++i) add(r.array().data()[i]);
    } else {
      add(r);
    }
  }
  if (!listPanel_) listPanel_ = std::make_shared<ListPanel>(*this);
  static_cast<ListPanel*>(listPanel_.get())->Open(std::move(items), active_);
  host_.ViewShowPanel(listPanel_.get());
}

void WorkbookView::DrawValidation(Canvas& c) {
  if (preview_ || ChartSheet() || (circles_.empty() && Current().Text().afterData.find("ataValidation") == std::string::npos))
    return;
  const data::Validation* v = data::ValidationAt(Validations(), active_.row, active_.col);
  for (const Pane& p : Panes()) {
    c.PushClip(RectF(p.rc));
    for (CellAddr at : circles_) {
      const D2D1_RECT_F r = CellRect(p, at.row, at.col, at.row, at.col);
      if (r.right < p.rc.left || r.left > p.rc.right || r.bottom < p.rc.top || r.top > p.rc.bottom) continue;
      c.DC()->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2),
                                        (r.right - r.left) / 2 + 3 * ui_, (r.bottom - r.top) / 2 + 2 * ui_),
                          c.Brush(Rgb(0xFF0000)), 1.5f * ui_);
    }
    const D2D1_RECT_F cell = CellRect(p, active_.row, active_.col, active_.row, active_.col);
    const bool inPane = cell.right >= p.rc.left && cell.left <= p.rc.right && cell.bottom >= p.rc.top && cell.top <= p.rc.bottom;
    if (inPane && v && v->type == data::Validation::Type::List && v->dropDown && !editor_.Active()) {
      const float s = std::min(cell.bottom - cell.top, std::round(18 * ui_));
      const D2D1_RECT_F b = RectF(cell.right + 1, cell.bottom - s, cell.right + 1 + s, cell.bottom);
      c.Fill(b, Rgb(0xF3F3F3));
      c.Stroke(Inflate(b, -0.5f, -0.5f), Rgb(0xA0A0A0), 1);
      c.Icon(icon::ChevronDown, b, Rgb(0x404040), std::round(s * 0.6f));
    }
    if (inPane && v && v->showInput && (!v->prompt.empty() || !v->promptTitle.empty())) {
      // Excel's pale yellow note below and right of the cell.
      const float w = 220 * ui_, x0 = cell.left + 12 * ui_, y0 = cell.bottom + 6 * ui_;
      const std::wstring title = Utf8ToWide(v->promptTitle), body = Utf8ToWide(v->prompt);
      const float th = title.empty() ? 0 : 18 * ui_;
      const float bh = body.empty() ? 0 : c.TextWrapped(body, RectF(x0 + 8 * ui_, 0, x0 + w - 8 * ui_, 1000), Rgb(0), 12 * ui_, 8, false);
      const D2D1_RECT_F box = RectF(x0, y0, x0 + w, y0 + th + bh + 12 * ui_);
      c.Fill(box, Rgb(0xFFFFE1));
      c.Stroke(Inflate(box, -0.5f, -0.5f), Rgb(0x767676), 1);
      if (!title.empty())
        c.Text(title, RectF(x0 + 8 * ui_, y0 + 5 * ui_, x0 + w - 8 * ui_, y0 + 5 * ui_ + th), Rgb(0), 12 * ui_, DWRITE_FONT_WEIGHT_BOLD);
      if (!body.empty()) c.TextWrapped(body, RectF(x0 + 8 * ui_, y0 + 6 * ui_ + th, x0 + w - 8 * ui_, box.bottom), Rgb(0), 12 * ui_, 8);
    }
    c.PopClip();
  }
}

// ---------------------------------------------------------------------------
// Scripted rules (build from scratch), on the selection: rule:<kind>,<value>,<value>,<choice>,<format>
// (kinds as kKindKeys), rule:clear, rule:clearsheet, rule:delete|up|stop,<index>;
// valid:<any|whole|decimal|list|date|time|length|custom>,<op>,<value>,<value> (a list: valid:list,a|b|c;
// custom: valid:custom,<formula>), valid:clear, valid:circle.

bool WorkbookView::DataRulesScriptOp(const std::wstring& op, const std::wstring& arg) {
  if (!Ready()) return false;
  if (op == L"table") return TableScriptOp(arg);
  std::vector<std::wstring> a;
  for (size_t i = 0;;) {
    const size_t e = arg.find(L',', i);
    a.push_back(arg.substr(i, e == std::wstring::npos ? std::wstring::npos : e - i));
    if (e == std::wstring::npos) break;
    i = e + 1;
  }
  while (a.size() < 5) a.emplace_back();
  const std::string sq = RangeName(Selected());
  if (op == L"rule") {
    if (a[0] == L"clear") return RulesCommand(CmdCfClearSelection);
    if (a[0] == L"clearsheet") return RulesCommand(CmdCfClearSheet);
    std::vector<data::CfRule> rules = data::ReadCfRules(Current().Text());
    const size_t i = static_cast<size_t>(std::wcstoul(a[1].c_str(), nullptr, 10));
    if (a[0] == L"delete" || a[0] == L"up" || a[0] == L"stop") {
      if (i >= rules.size() || (a[0] == L"up" && i == 0)) return false;
      if (a[0] == L"delete") rules.erase(rules.begin() + static_cast<ptrdiff_t>(i));
      else if (a[0] == L"up") std::swap(rules[i], rules[i - 1]);
      else rules[i].stopIfTrue = !rules[i].stopIfTrue;
      return ApplyCfRules(rules, u"Conditional Formatting");
    }
    int k = -1;
    for (int j = 0; j < kKinds; ++j)
      if (a[0] == kKindKeys[j]) k = j;
    if (k < 0) return false;
    if (!rulesPanel_) rulesPanel_ = std::make_shared<RulesPanel>(*this);
    auto* p = static_cast<RulesPanel*>(rulesPanel_.get());
    edit::EditGroup group(Ed(), u"New Rule");
    data::CfRule r;
    const int choice = static_cast<int>(std::wcstol(a[3].c_str(), nullptr, 10));
    const int format = static_cast<int>(std::wcstol(a[4].c_str(), nullptr, 10));
    if (!p->Build(k, a[1], a[2], choice, format, sq, -1, r)) return false;
    rules.insert(rules.begin(), std::move(r));
    group.done = ApplyCfRules(rules, u"New Rule");
    return group.done;
  }
  if (a[0] == L"circle") return RulesCommand(CmdCircleInvalid);
  if (a[0] == L"clear") {
    std::vector<data::Validation> rules = Validations();
    data::ClearValidations(rules, Selected());
    return ApplyValidations(rules, u"Data Validation");
  }
  static constexpr const wchar_t* kKeys[] = {L"any", L"whole", L"decimal", L"list", L"date", L"time", L"length", L"custom"};
  static constexpr const wchar_t* kOps[] = {L"between", L"notbetween", L"equal",        L"notequal",
                                            L"greater", L"less",       L"greaterequal", L"lessequal"};
  int allow = -1, opIndex = 0;
  for (int j = 0; j < 8; ++j) {
    if (a[0] == kKeys[j]) allow = j;
    if (a[1] == kOps[j]) opIndex = j;
  }
  if (allow < 0) return false;
  if (!validationPanel_) validationPanel_ = std::make_shared<ValidationPanel>(*this);
  auto* p = static_cast<ValidationPanel*>(validationPanel_.get());
  p->Open();
  const calc::FormulaLocale fl = calc::MakeFormulaLocale(text_->Locale());
  std::wstring first = allow == 3 || allow == 7 ? a[1] : a[2];
  if (allow == 3) std::replace(first.begin(), first.end(), L'|', static_cast<wchar_t>(fl.list));
  return p->Script(allow, opIndex, first, a[3]);
}

}  // namespace fw::sheet::view
