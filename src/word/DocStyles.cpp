#include "word/DocStyles.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

#include "Commands.h"
#include "core/Strings.h"
#include "doc/edit/Format.h"
#include "doc/edit/Styles.h"
#include "ui/Controls.h"
#include "ui/Gfx.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include "word/DocFormat.h"
#include "word/DocPanels.h"

using namespace gfx;
namespace edit = fw::doc::edit;
using fw::Twips;
using fw::doc::kNone;
using fw::doc::Pos;
using fw::doc::TabAlign;
using fw::doc::TabLeader;
using fw::doc::TabStop;

namespace {

struct Named {
  int cmd;
  const wchar_t* label;
};
constexpr Named kNames[] = {
    {CmdTabsPanel, L"Tabs…"},
    {CmdTabLeft, L"Left tab"},
    {CmdTabCenter, L"Center tab"},
    {CmdTabRight, L"Right tab"},
    {CmdTabDecimal, L"Decimal tab"},
    {CmdTabBar, L"Bar tab"},
    {CmdTabLeaderNone, L"No leader"},
    {CmdTabLeaderDots, L"Leader: dots ……"},
    {CmdTabLeaderDashes, L"Leader: dashes - - -"},
    {CmdTabLeaderLine, L"Leader: line ____"},
    {CmdTabClear, L"Clear tab stop"},
    {CmdTabClearAll, L"Clear all tab stops"},
    {CmdModifyStyle, L"Modify style…"},
    {CmdNewStyle, L"New style from selection…"},
};

const wchar_t* LabelOf(int cmd) {
  for (const Named& n : kNames)
    if (n.cmd == cmd) return n.label;
  return L"";
}

constexpr TabAlign kAligns[] = {TabAlign::Start, TabAlign::Center, TabAlign::End, TabAlign::Decimal, TabAlign::Bar};
constexpr const wchar_t* kAlignNames[] = {L"Left", L"Center", L"Right", L"Decimal", L"Bar"};
constexpr TabLeader kLeaders[] = {TabLeader::None,       TabLeader::Dot,   TabLeader::Hyphen,
                                  TabLeader::Underscore, TabLeader::Heavy, TabLeader::MiddleDot};
constexpr const wchar_t* kLeaderNames[] = {L"No leader", L"Dots ……", L"Dashes - - -", L"Line ____", L"Heavy line", L"Middle dots ···"};

template <typename T, size_t N>
int IndexOf(const T (&values)[N], T v) {
  for (size_t i = 0; i < N; ++i)
    if (values[i] == v) return static_cast<int>(i);
  return -1;
}

constexpr int32_t kLongest = 31680;  // 22 inches, as Word allows

// Lengths as the user's region measures them: inches (US) or centimeters.
bool Metric() {
  static const bool metric = [] {
    wchar_t v[4] = L"";
    return GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IMEASURE, v, 4) > 0 && v[0] == L'0';
  }();
  return metric;
}
double FromTwips(int32_t twips) { return Metric() ? twips / 566.929 : twips / 1440.0; }
int32_t ToTwips(double v) { return static_cast<int32_t>(std::lround(Metric() ? v * 566.929 : v * 1440.0)); }
const wchar_t* Unit() { return Metric() ? L" cm" : L"\""; }
std::wstring Length(int32_t twips) {
  wchar_t b[32];
  swprintf_s(b, L"%.2f", FromTwips(twips));
  std::wstring s = b;
  while (s.back() == L'0') s.pop_back();
  if (s.back() == L'.') s.pop_back();
  // With Windows' decimal separator, as the spin boxes show numbers.
  static const std::wstring sep = [] {
    wchar_t buf[8] = {};
    return GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, buf, 8) > 1 ? std::wstring(buf) : std::wstring(L".");
  }();
  if (const size_t dot = s.find(L'.'); dot != std::wstring::npos) s.replace(dot, 1, sep);
  return s + Unit();
}

std::wstring W(std::string_view s) { return fw::Utf8ToWide(s); }
std::wstring W(const std::u16string& s) { return std::wstring(s.begin(), s.end()); }

void Spin(SpinBox& s, const wchar_t* name, double lo, double hi, double step, int decimals, const wchar_t* unit) {
  s.label = s.tooltip = name;
  s.minValue = lo;
  s.maxValue = hi;
  s.step = step;
  s.decimals = decimals;
  s.unit = unit;
}

D2D1_COLOR_F ColorOf(uint32_t rgb) {
  return {static_cast<float>((rgb >> 16) & 0xFF) / 255.f, static_cast<float>((rgb >> 8) & 0xFF) / 255.f,
          static_cast<float>(rgb & 0xFF) / 255.f, 1.f};
}
uint32_t RgbOf(D2D1_COLOR_F c) {
  auto ch = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
  return ch(c.r) << 16 | ch(c.g) << 8 | ch(c.b);
}

}  // namespace

// ---------------------------------------------------------------------------
// The Tabs panel

class DocStyles::TabsPanel : public PanelContent, DocPanelRows {
 public:
  explicit TabsPanel(DocStyles& owner) : s_(owner) {
    list_.label = L"Tab stops";
    list_.rowHeight = 26;
    list_.onSelect = [this](int i) { Pick(i); };
    Spin(position_, L"Tab stop position", 0, FromTwips(kLongest), Metric() ? 0.25 : 0.125, 2, Unit());
    align_.label = align_.tooltip = L"Alignment";
    align_.items.assign(std::begin(kAlignNames), std::end(kAlignNames));
    align_.selected = 0;
    leader_.label = leader_.tooltip = L"Leader";
    leader_.items.assign(std::begin(kLeaderNames), std::end(kLeaderNames));
    leader_.selected = 0;
    set_.text = L"Set";
    set_.primary = true;
    set_.onInvoke = [this] { Set(); };
    clear_.text = L"Clear";
    clear_.onInvoke = [this] { Clear(); };
    clearAll_.text = L"Clear all";
    clearAll_.onInvoke = [this] { s_.Execute(CmdTabClearAll); };
  }

  std::wstring Title() const override { return L"Tabs"; }

  void Open() { shown_ = {}; }

  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override {
    Refresh();
    return LayOutRows({{L"Tab stops of this paragraph", {&list_}, 134},
                       {L"Position", {&position_}},
                       {L"Alignment and leader", {&align_, &leader_}},
                       {L"", {&set_, &clear_, &clearAll_}}},
                      body, ui);
  }

  void Draw(Canvas& c) override {
    Refresh();
    DrawLabels(c);
  }

 private:
  struct Shown {
    Pos a{kNone, 0}, f{kNone, 0};
    uint64_t generation = UINT64_MAX;
    bool operator==(const Shown&) const = default;
  };

  // What the panel shows follows the caret's paragraph, and the document.
  void Refresh() {
    const auto [a, f] = s_.host_.selection();
    const Shown now{a, f, s_.Doc().CacheGeneration()};
    if (now == shown_) return;
    shown_ = now;
    stops_ = s_.Stops();
    const int was = list_.Selected();
    list_.items.clear();
    int keep = -1;
    for (size_t i = 0; i < stops_.size(); ++i) {
      const TabStop& t = stops_[i];
      std::wstring item = Length(t.pos.Value()) + L"   " + kAlignNames[std::max(0, IndexOf(kAligns, t.align))];
      if (t.leader != TabLeader::None) item += std::wstring(L", ") + kLeaderNames[std::max(0, IndexOf(kLeaders, t.leader))];
      list_.items.push_back(std::move(item));
      if (ToTwips(position_.value) == t.pos.Value()) keep = static_cast<int>(i);
    }
    const int pick = keep >= 0                                        ? keep
                     : was >= 0 && was < static_cast<int>(stops_.size()) ? was
                     : stops_.empty()                                    ? -1
                                                                         : 0;
    list_.SetSelected(pick);
    if (pick >= 0) Pick(pick);
    else position_.SetValue(0);
    const bool can = s_.editor_.CanEdit();
    for (Control* c : std::initializer_list<Control*>{&position_, &align_, &leader_, &set_, &clear_, &clearAll_})
      c->enabled = can;
    clear_.enabled = can && Near().has_value();
    clearAll_.enabled = can && !stops_.empty();
    note_ = stops_.empty() ? L"No tab stops of its own: tabs go to the default stops, every " +
                                 Length(s_.Doc().Settings().defaultTabStop.Value()) + L"."
                           : L"Default stops every " + Length(s_.Doc().Settings().defaultTabStop.Value()) + L" after these.";
  }

  void Pick(int i) {
    if (i < 0 || i >= static_cast<int>(stops_.size())) return;
    const TabStop& t = stops_[static_cast<size_t>(i)];
    position_.SetValue(FromTwips(t.pos.Value()));
    align_.selected = std::max(0, IndexOf(kAligns, t.align));
    leader_.selected = std::max(0, IndexOf(kLeaders, t.leader));
    clear_.enabled = s_.editor_.CanEdit();
  }

  // The paragraph's stop at the position typed (within a twentieth of a point's rounding).
  std::optional<TabStop> Near() const {
    const int32_t at = ToTwips(position_.value);
    for (const TabStop& t : stops_)
      if (std::abs(t.pos.Value() - at) <= 10) return t;
    return std::nullopt;
  }

  void Set() {
    TabStop t;
    t.pos = Twips(ToTwips(position_.value));
    t.align = kAligns[std::clamp(align_.selected, 0, static_cast<int>(std::size(kAligns)) - 1)];
    t.leader = kLeaders[std::clamp(leader_.selected, 0, static_cast<int>(std::size(kLeaders)) - 1)];
    std::vector<Twips> remove;
    if (auto old = Near(); old && old->pos != t.pos) remove.push_back(old->pos);  // the same stop, typed again
    s_.ChangeTabs(remove, {t});
  }

  void Clear() {
    if (auto old = Near()) s_.ChangeTabs({old->pos}, {});
  }

  DocStyles& s_;
  Shown shown_;
  std::vector<TabStop> stops_;
  ListBox list_;
  SpinBox position_;
  Dropdown align_, leader_;
  DocView::PushButton set_, clear_, clearAll_;
};

// ---------------------------------------------------------------------------
// The Style panel: Modify style… and New style…

class DocStyles::StylePanel : public PanelContent, DocPanelRows {
 public:
  explicit StylePanel(DocStyles& owner) : s_(owner) {
    name_.label = name_.tooltip = L"Name";
    name_.autoComplete = false;
    style_.label = style_.tooltip = L"Style";
    style_.onChange = [this](int i) {
      if (i >= 0 && i < static_cast<int>(ids_.size())) picked_ = ids_[static_cast<size_t>(i)];
      Load();
    };
    font_.label = font_.tooltip = L"Font";
    font_.validate = [](std::wstring& text) { return !text.empty() && fw::doc::edit::XmlSafe(fw::WideToUtf8(text)); };
    Spin(size_, L"Size", 1, 1638, 1, 1, L" pt");
    weight_.label = weight_.tooltip = L"Font style";
    weight_.items = {L"Regular", L"Bold", L"Italic", L"Bold italic"};
    underline_.label = underline_.tooltip = L"Underline";
    underline_.items = {L"None", L"Underline"};
    color_.label = color_.tooltip = L"Font color";
    color_.automaticLabel = L"Automatic";
    color_.automaticColor = {0, 0, 0, 1};
    color_.onChange = [this](D2D1_COLOR_F c, bool automatic) {
      color_.color = c;
      color_.automatic = automatic;
    };
    align_.label = align_.tooltip = L"Alignment";
    align_.items = {L"Left", L"Center", L"Right", L"Justify"};
    spacing_.label = spacing_.tooltip = L"Line spacing";
    const double longest = FromTwips(kLongest), step = Metric() ? 0.1 : 0.05;
    Spin(before_, L"Space before", 0, 1584, 6, 1, L" pt");
    Spin(after_, L"Space after", 0, 1584, 6, 1, L" pt");
    Spin(left_, L"Left indent", -longest, longest, step, 2, Unit());
    Spin(right_, L"Right indent", -longest, longest, step, 2, Unit());
    Spin(first_, L"First line indent (negative: hanging)", -longest, longest, step, 2, Unit());
    apply_.primary = true;
    apply_.onInvoke = [this] { Apply(); };
  }

  std::wstring Title() const override { return create_ ? L"New style" : L"Modify style"; }

  void Open(bool create) {
    create_ = create;
    shown_ = {};
    picked_.clear();
    note_.clear();
    apply_.text = create ? L"Create" : L"Apply";
    if (create) {
      // "Style1", "Style2"... as Word names them.
      std::wstring name;
      for (int n = 1; n < 1000; ++n) {
        name = L"Style" + std::to_wstring(n);
        if (!s_.Doc().FindStyle(fw::WideToUtf8(name))) break;
      }
      name_.SetText(name);
    }
  }

  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override {
    Refresh();
    name_.visible = create_;
    style_.visible = !create_;
    return LayOutRows({{create_ ? L"Name" : L"Style", {&name_, &style_}},
                       {L"Font and size", {&font_, &size_}},
                       {L"Font style, underline and color", {&weight_, &underline_, &color_}},
                       {L"Alignment and line spacing", {&align_, &spacing_}},
                       {L"Space before and after", {&before_, &after_}},
                       {L"Indents: left, right, first line", {&left_, &right_, &first_}},
                       {L"", {&apply_}}},
                      body, ui);
  }

  void Draw(Canvas& c) override {
    Refresh();
    DrawLabels(c);
  }

 private:
  // What a style or the selection looks like, as the controls show it.
  struct Values {
    std::wstring font;
    double size = 11;
    int weight = 0, underline = 0;
    fw::doc::Color color;
    int align = 0, spacing = -1;
    double before = 0, after = 0, left = 0, right = 0, first = 0;
  };
  struct Shown {
    Pos a{kNone, 0}, f{kNone, 0};
    uint64_t generation = UINT64_MAX;
    bool operator==(const Shown&) const = default;
  };
  static constexpr int32_t kSpacings[] = {240, 276, 360, 480};

  // A new selection: its paragraph's style (Modify) or its look (New); a changed document: the
  // same style again.
  void Refresh() {
    const auto [a, f] = s_.host_.selection();
    const fw::doc::Document& doc = s_.Doc();
    const Shown now{a, f, doc.CacheGeneration()};
    if (now == shown_) return;
    const bool moved = now.a != shown_.a || now.f != shown_.f;
    shown_ = now;
    if (!create_) {
      ids_.clear();
      style_.items.clear();
      for (const edit::StyleEntry& e : edit::GalleryStyles(doc)) {
        if (e.id.empty()) continue;  // a built-in the document doesn't have yet
        ids_.push_back(e.id);
        style_.items.push_back(W(e.name));
      }
      const edit::Range sel = s_.Selection();
      const std::string caret(doc.ResolvedPara(sel.from.para).style);
      if (moved || picked_.empty()) picked_ = caret;
      if (std::find(ids_.begin(), ids_.end(), picked_) == ids_.end())
        if (const fw::doc::Style* st = doc.FindStyle(picked_)) {
          ids_.push_back(picked_);
          style_.items.push_back(W(edit::DisplayName(st->name)));
        }
      style_.selected = static_cast<int>(std::find(ids_.begin(), ids_.end(), picked_) - ids_.begin());
      if (style_.selected >= static_cast<int>(ids_.size())) style_.selected = -1;
    }
    Load();
  }

  void Load() {
    const fw::doc::Document& doc = s_.Doc();
    const edit::Range sel = s_.Selection();
    Values v;
    fw::doc::ParaProps pp;
    bool paragraph = true;
    if (create_) {
      edit::Range one{sel.from, sel.from};
      if (sel.from.offset < doc.Para(sel.from.para).text.size()) one.to = {sel.from.para, sel.from.offset + 1};
      const edit::FormatState st = edit::StateOf(doc, one);
      v.font = st.font ? W(*st.font) : std::wstring();
      v.size = st.size ? st.size->Value() / 2.0 : 11;
      v.weight = (st.bold ? 1 : 0) + (st.italic ? 2 : 0);
      v.underline = st.underline ? 1 : 0;
      if (st.color) v.color = *st.color;
      pp = doc.ResolvedPara(sel.from.para);
    } else {
      const fw::doc::Style* st = picked_.empty() ? nullptr : doc.FindStyle(picked_);
      paragraph = st && st->type == fw::doc::StyleType::Paragraph;
      const fw::doc::RunProps rp = doc.StyleChainRun(picked_);
      v.font = W(rp.fonts.ascii);
      v.size = rp.size.Value() / 2.0;
      v.weight = (rp.bold ? 1 : 0) + (rp.italic ? 2 : 0);
      v.underline = rp.underline != fw::doc::Underline::None ? 1 : 0;
      v.color = rp.color;
      pp = doc.StyleChainPara(picked_);
    }
    using J = fw::doc::Justification;
    v.align = pp.jc == J::Center ? 1 : pp.jc == J::End ? 2 : pp.jc == J::Both ? 3 : 0;
    v.spacing = -1;
    if (pp.lineRule == fw::doc::LineRule::Auto)
      for (size_t i = 0; i < std::size(kSpacings); ++i)
        if (pp.line == kSpacings[i]) v.spacing = static_cast<int>(i);
    v.before = pp.spaceBefore.Value() / 20.0;
    v.after = pp.spaceAfter.Value() / 20.0;
    v.left = FromTwips(pp.indStart.Value());
    v.right = FromTwips(pp.indEnd.Value());
    v.first = FromTwips(pp.firstLine.Value());
    loaded_ = v;

    font_.items = DocFormat::ThemeFontChoices(doc);
    font_.items.insert(font_.items.end(), DocFormat::FontNames().begin(), DocFormat::FontNames().end());
    font_.SetText(v.font);
    color_.theme = DocFormat::ThemeColors(doc);
    if (color_.theme.empty()) color_.theme = ColorPalette::DefaultTheme();
    size_.SetValue(v.size);
    weight_.selected = v.weight;
    underline_.selected = v.underline;
    color_.automatic = v.color.automatic;
    color_.color = v.color.automatic ? D2D1_COLOR_F{0, 0, 0, 1} : ColorOf(v.color.rgb);
    align_.selected = v.align;
    spacing_.items = {L"Single", L"1.15", L"1.5", L"Double"};
    if (v.spacing < 0) {  // what it is, kept unless another is chosen
      wchar_t b[48];
      if (pp.lineRule == fw::doc::LineRule::Auto) swprintf_s(b, L"%.2f lines", pp.line / 240.0);
      else swprintf_s(b, pp.lineRule == fw::doc::LineRule::Exact ? L"Exactly %g pt" : L"At least %g pt", pp.line / 20.0);
      spacing_.items.push_back(b);
    }
    spacing_.selected = v.spacing < 0 ? 4 : v.spacing;
    before_.SetValue(v.before);
    after_.SetValue(v.after);
    left_.SetValue(v.left);
    right_.SetValue(v.right);
    first_.SetValue(v.first);

    const bool can = s_.editor_.CanEdit() && (create_ || !picked_.empty());
    for (Control* c : std::initializer_list<Control*>{&name_, &style_, &font_, &size_, &weight_, &underline_, &color_, &apply_})
      c->enabled = can;
    for (Control* c : std::initializer_list<Control*>{&align_, &spacing_, &before_, &after_, &left_, &right_, &first_})
      c->enabled = can && paragraph;
  }

  // What the controls say differently from what they showed: only that is written, so a style's
  // theme fonts and colors, and what it inherits, stay as they are.
  void Changes(edit::RunFormat& rf, edit::ParaFormat& pf) const {
    const Values& v = loaded_;
    if (font_.Text() != v.font && !font_.Text().empty()) {
      if (auto slot = DocFormat::ThemeFontOf(s_.Doc(), font_.Text())) rf.themeFont = *slot;
      else rf.font = fw::WideToUtf8(font_.Text());
    }
    if (std::abs(size_.value - v.size) > 0.01) rf.size = fw::HalfPoints(static_cast<int32_t>(std::lround(size_.value * 2)));
    if (weight_.selected >= 0 && weight_.selected != v.weight) {
      if ((weight_.selected & 1) != (v.weight & 1)) rf.bold = (weight_.selected & 1) != 0;
      if ((weight_.selected & 2) != (v.weight & 2)) rf.italic = (weight_.selected & 2) != 0;
    }
    if (underline_.selected >= 0 && underline_.selected != v.underline)
      rf.underline = underline_.selected ? fw::doc::Underline::Single : fw::doc::Underline::None;
    const fw::doc::Color now = color_.automatic ? fw::doc::Color{} : fw::doc::Color::Rgb(RgbOf(color_.color));
    if (now != v.color) {
      if (auto theme = color_.automatic ? std::nullopt : DocFormat::ThemeColorOf(s_.Doc(), color_.color))
        rf.themeColor = std::move(*theme);
      else rf.color = now;
    }
    if (align_.enabled) {
      using J = fw::doc::Justification;
      static constexpr J kJc[] = {J::Start, J::Center, J::End, J::Both};
      if (align_.selected >= 0 && align_.selected < 4 && align_.selected != v.align) pf.jc = kJc[align_.selected];
      if (spacing_.selected >= 0 && spacing_.selected < 4 && spacing_.selected != v.spacing) {
        pf.lineRule = fw::doc::LineRule::Auto;
        pf.line = kSpacings[spacing_.selected];
      }
      auto twips = [](double pt) { return Twips(static_cast<int32_t>(std::lround(pt * 20))); };
      if (std::abs(before_.value - v.before) > 0.01) pf.spaceBefore = twips(before_.value);
      if (std::abs(after_.value - v.after) > 0.01) pf.spaceAfter = twips(after_.value);
      if (std::abs(left_.value - v.left) > 0.001) pf.indStart = Twips(ToTwips(left_.value));
      if (std::abs(right_.value - v.right) > 0.001) pf.indEnd = Twips(ToTwips(right_.value));
      if (std::abs(first_.value - v.first) > 0.001) pf.firstLine = Twips(ToTwips(first_.value));
    }
  }

  void Apply() {
    edit::RunFormat rf;
    edit::ParaFormat pf;
    Changes(rf, pf);
    const edit::Range sel = s_.Selection();
    std::u16string why;
    if (create_) {
      s_.editor_.BeginGroup(u"New style");
      {
        edit::TreeEdit te(s_.editor_, sel.from);
        const std::string id = edit::NewStyleFromSelection(te, sel, fw::WideToUtf8(name_.Text()), why);
        if (!id.empty() && (rf.Any() || pf.Any())) {
          edit::ModifyStyle(te, id, rf, pf);
          why = te.Refused();
        }
      }
      s_.editor_.EndGroup();
      if (why.empty()) {
        s_.host_.done(L"");
        s_.host_.showPanel(nullptr, false);
        return;
      }
    } else {
      if (!rf.Any() && !pf.Any()) {
        note_ = L"Nothing changed.";
        return;
      }
      {
        edit::TreeEdit te(s_.editor_, sel.from);
        edit::ModifyStyle(te, picked_, rf, pf);
        why = te.Refused();
      }
      note_ = why.empty() ? L"Changed. Text in this style follows." : L"";
      shown_ = {};
    }
    s_.host_.done(W(why));
  }

  DocStyles& s_;
  bool create_ = false;
  Shown shown_;
  std::string picked_;            // the style being modified
  std::vector<std::string> ids_;  // the style box's styles
  Values loaded_;
  ComboBox name_, font_;
  Dropdown style_, weight_, underline_, align_, spacing_;
  SpinBox size_, before_, after_, left_, right_, first_;
  ColorPicker color_;
  DocView::PushButton apply_;
};

// ---------------------------------------------------------------------------

DocStyles::DocStyles(edit::Editor& editor, Host host) : editor_(editor), host_(std::move(host)) {}

DocStyles::~DocStyles() = default;

bool DocStyles::Owns(int cmd) { return cmd >= CmdTabsPanel && cmd <= CmdNewStyle; }

std::vector<int> DocStyles::CommandIds() { return {CmdTabsPanel, CmdTabClearAll, CmdModifyStyle, CmdNewStyle}; }

bool DocStyles::ShowsIn(const PanelContent* shown) const {
  return shown && (shown == tabs_.get() || shown == style_.get());
}

edit::Range DocStyles::Selection() const {
  auto [a, f] = host_.selection();
  if (f.para < a.para || (f.para == a.para && f.offset < a.offset)) std::swap(a, f);
  const auto& paras = Doc().Paragraphs();
  if (paras.empty()) return {};
  a.para = std::min<uint32_t>(a.para, static_cast<uint32_t>(paras.size() - 1));
  f.para = std::min<uint32_t>(f.para, static_cast<uint32_t>(paras.size() - 1));
  a.offset = std::min<uint32_t>(a.offset, static_cast<uint32_t>(paras[a.para].text.size()));
  f.offset = std::min<uint32_t>(f.offset, static_cast<uint32_t>(paras[f.para].text.size()));
  return {a, f};
}

std::vector<TabStop> DocStyles::Stops() const {
  std::vector<TabStop> out;
  const edit::Range sel = Selection();
  if (Doc().Paragraphs().empty()) return out;
  for (const TabStop& t : Doc().ResolvedPara(sel.from.para).tabs)
    if (t.align != TabAlign::Clear && t.align != TabAlign::Num) out.push_back(t);
  std::stable_sort(out.begin(), out.end(), [](const TabStop& a, const TabStop& b) { return a.pos < b.pos; });
  return out;
}

void DocStyles::ChangeTabs(const std::vector<Twips>& remove, const std::vector<TabStop>& add) {
  const edit::Range sel = Selection();
  std::u16string refused;
  {
    edit::TreeEdit te(editor_, sel.from);
    edit::ChangeTabStops(te, sel, remove, add);
    refused = te.Refused();
  }
  host_.done(W(refused));
}

CommandState DocStyles::Query(int cmd) {
  CommandState s;
  s.label = LabelOf(cmd);
  s.enabled = editor_.CanEdit();
  if (cmd >= CmdTabLeft && cmd <= CmdTabClear) {
    s.enabled = s.enabled && menuTab_.has_value();
    if (menuTab_ && cmd <= CmdTabBar) s.checked = menuTab_->align == kAligns[cmd - CmdTabLeft];
    if (menuTab_ && cmd >= CmdTabLeaderNone && cmd <= CmdTabLeaderLine) s.checked = menuTab_->leader == kLeaders[cmd - CmdTabLeaderNone];
  }
  if (cmd == CmdTabClearAll) s.enabled = s.enabled && !Stops().empty();
  return s;
}

void DocStyles::FillTabMenu(HMENU m, std::optional<TabStop> stop) {
  menuTab_ = stop;
  auto item = [&](int cmd, bool radio = false) {
    const CommandState q = Query(cmd);
    menu::AppendItem(m, cmd, q.label, q.enabled, q.checked, radio);
  };
  if (stop) {
    for (int cmd = CmdTabLeft; cmd <= CmdTabBar; ++cmd) item(cmd, true);
    menu::AppendSeparator(m);
    for (int cmd = CmdTabLeaderNone; cmd <= CmdTabLeaderLine; ++cmd) item(cmd, true);
    menu::AppendSeparator(m);
    item(CmdTabClear);
  }
  item(CmdTabClearAll);
  item(CmdTabsPanel);
}

void DocStyles::OpenStylePanel(bool create) {
  if (!style_) style_ = std::make_unique<StylePanel>(*this);
  style_->Open(create);
  host_.showPanel(style_.get(), false);
}

void DocStyles::Execute(int cmd) {
  const std::optional<TabStop> stop = menuTab_;
  menuTab_.reset();
  switch (cmd) {
    case CmdTabsPanel:
      if (!tabs_) tabs_ = std::make_unique<TabsPanel>(*this);
      tabs_->Open();
      host_.showPanel(tabs_.get(), false);
      return;
    case CmdTabClear:
      if (stop) ChangeTabs({stop->pos}, {});
      return;
    case CmdTabClearAll: {
      // Every stop of the selection's paragraphs.
      std::vector<Twips> all;
      for (uint32_t p : edit::ParagraphsOf(Doc(), Selection()))
        for (const TabStop& t : Doc().ResolvedPara(p).tabs)
          if (t.align != TabAlign::Clear && std::find(all.begin(), all.end(), t.pos) == all.end()) all.push_back(t.pos);
      if (!all.empty()) ChangeTabs(all, {});
      return;
    }
    case CmdModifyStyle: OpenStylePanel(false); return;
    case CmdNewStyle: OpenStylePanel(true); return;
    default: break;
  }
  if (!stop) return;
  TabStop t = *stop;
  if (cmd >= CmdTabLeft && cmd <= CmdTabBar) t.align = kAligns[cmd - CmdTabLeft];
  else if (cmd >= CmdTabLeaderNone && cmd <= CmdTabLeaderLine) t.leader = kLeaders[cmd - CmdTabLeaderNone];
  else return;
  if (!(t == *stop)) ChangeTabs({}, {t});
}
