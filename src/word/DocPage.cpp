#include "word/DocPage.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <optional>

#include "Commands.h"
#include "core/Lang.h"
#include "doc/edit/Format.h"
#include "doc/edit/TreeEdit.h"
#include "ui/Controls.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

using namespace gfx;
namespace edit = fw::doc::edit;
using fw::Twips;
using fw::doc::Justification;
using fw::doc::kNone;
using fw::doc::LineRule;
using fw::doc::Pos;
using fw::doc::SectionProps;
using fw::doc::SectionStart;

namespace {

struct Named {
  int cmd;
  UiText label;
  const wchar_t* keys;
};
constexpr Named kNames[] = {
    {CmdPageSetup, N_(L"Page setup…"), L""},
    {CmdLayoutBox, N_(L"Layout"), L""},
    {CmdPortrait, N_(L"Portrait"), L""},
    {CmdLandscape, N_(L"Landscape"), L""},
    {CmdMarginsNormal, N_(L"Margins: Normal"), L""},
    {CmdMarginsNarrow, N_(L"Margins: Narrow"), L""},
    {CmdMarginsModerate, N_(L"Margins: Moderate"), L""},
    {CmdMarginsWide, N_(L"Margins: Wide"), L""},
    {CmdColumnsOne, N_(L"One column"), L""},
    {CmdColumnsTwo, N_(L"Two columns"), L""},
    {CmdColumnsThree, N_(L"Three columns"), L""},
    {CmdPageBreak, N_(L"Page break"), L"Ctrl+Enter"},
    {CmdColumnBreak, N_(L"Column break"), L"Ctrl+Shift+Enter"},
    {CmdSectionNextPage, N_(L"Section break (next page)"), L""},
    {CmdSectionContinuous, N_(L"Section break (continuous)"), L""},
    {CmdSectionEvenPage, N_(L"Section break (even page)"), L""},
    {CmdSectionOddPage, N_(L"Section break (odd page)"), L""},
    {CmdDeleteSectionBreak, N_(L"Delete section break"), L""},
    {CmdHyphenation, N_(L"Hyphenate automatically"), L""},
    {CmdOptionalHyphen, N_(L"Optional hyphen"), L"Ctrl+-"},
    {CmdNonBreakingHyphen, N_(L"Non-breaking hyphen"), L"Ctrl+Shift+-"},
    {CmdParagraph, N_(L"Paragraph…"), L""},
};

const Named* NameOf(int cmd) {
  for (const Named& n : kNames)
    if (n.cmd == cmd) return &n;
  return nullptr;
}

// Word's margin presets, in twips.
struct MarginPreset {
  int cmd;
  const wchar_t* name;
  int32_t top, bottom, left, right;
};
constexpr MarginPreset kMargins[] = {
    {CmdMarginsNormal, N_(L"Normal"), 1440, 1440, 1440, 1440},
    {CmdMarginsNarrow, N_(L"Narrow"), 720, 720, 720, 720},
    {CmdMarginsModerate, N_(L"Moderate"), 1440, 1440, 1080, 1080},
    {CmdMarginsWide, N_(L"Wide"), 1440, 1440, 2880, 2880},
};

// Paper sizes, standing up, in twips.
struct Paper {
  UiText name;
  int32_t width, height;
};
constexpr Paper kPapers[] = {{N_(L"Letter"), 12240, 15840}, {N_(L"Legal"), 12240, 20160},  {L"A4", 11906, 16838},
                             {L"A5", 8391, 11906},      {N_(L"Executive"), 10440, 15120}, {L"B5 (JIS)", 10319, 14571}};

constexpr int32_t kLongest = 31680;  // 22 inches, as Word allows
// The panel fits a 700-pixel window without scrolling: 10 rows under 4 headings. Word's margin
// presets are in the Layout menu.
constexpr float kLabelW = 92, kRowH = 35, kSide = 16;
constexpr float kParaLabelW = 100;  // "Keep with next"

bool Near(int32_t a, int32_t b) { return std::abs(a - b) <= 20; }

// Lengths as the user's region measures them: inches (US) or centimeters.
bool Metric() {
  wchar_t v[4] = L"";
  return GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IMEASURE, v, 4) > 0 && v[0] == L'0';
}
double FromTwips(int32_t twips) { return Metric() ? twips / 566.929 : twips / 1440.0; }
int32_t ToTwips(double v) { return static_cast<int32_t>(std::lround(Metric() ? v * 566.929 : v * 1440.0)); }
const wchar_t* Unit() { return Metric() ? L" cm" : L"\""; }

std::wstring Number(double v) {
  wchar_t b[32];
  swprintf_s(b, L"%.3g", v);
  return b;
}

// "Letter (8.5 × 11")", "A4 (21 × 29.7 cm)".
std::wstring PaperLabel(const Paper& p) {
  return std::wstring(p.name) + L" (" + Number(FromTwips(p.width)) + L" × " + Number(FromTwips(p.height)) + Unit() +
         L")";
}

int MarginPresetOf(const SectionProps& s) {
  for (size_t i = 0; i < std::size(kMargins); ++i) {
    const MarginPreset& m = kMargins[i];
    if (Near(s.marginTop.Value(), m.top) && Near(s.marginBottom.Value(), m.bottom) && Near(s.marginStart.Value(), m.left) &&
        Near(s.marginEnd.Value(), m.right))
      return static_cast<int>(i);
  }
  return -1;
}

bool Landscape(const SectionProps& s) { return s.pageWidth.Value() > s.pageHeight.Value(); }

bool Before(Pos a, Pos b) { return a.para < b.para || (a.para == b.para && a.offset < b.offset); }

// A side panel laid out as rows under headings: a label, then one control or two side by side.
class RowsPanel : public PanelContent {
 public:
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override {
    ui_ = ui;
    const float u = ui;
    Refresh();
    headings_.clear();
    labels_.clear();
    std::vector<Control*> controls;
    const float labelX = body.left + kSide * u, cx = body.left + (kSide + labelW_) * u, right = body.right - kSide * u;
    auto place = [&](Control* c, float left, float r, float y) {
      c->rc = RectF(std::round(left), std::round(y), std::round(r), std::round(y + 30 * u));
      controls.push_back(c);
    };
    float y = body.top + 4 * u;
    for (Row& r : rows_) {
      if (!r.a) {
        if (!headings_.empty()) y += 4 * u;
        headings_.push_back({r.label, RectF(labelX, y, right, y + 22 * u)});
        y += 24 * u;
        continue;
      }
      if (r.b) {
        // Side by side: halves, or a number box beside a list takes only what a number needs.
        const float half = (right - cx - 6 * u) / 2;
        const float second = dynamic_cast<SpinBox*>(r.b) && !dynamic_cast<SpinBox*>(r.a) ? std::min(half, 72 * u) : half;
        place(r.a, cx, right - second - 6 * u, y);
        place(r.b, right - second, right, y);
      } else {
        const float w = dynamic_cast<SpinBox*>(r.a) ? std::min(right - cx, 120 * u) : right - cx;
        place(r.a, cx, cx + w, y);
      }
      labels_.push_back({r.label, RectF(labelX, y, cx - 6 * u, y + 30 * u)});
      y += kRowH * u;
    }
    return controls;
  }

  void Draw(Canvas& c) override {
    const Palette& p = theme::P();
    const float u = ui_;
    Refresh();
    for (const auto& [text, rc] : headings_) c.Text(text, rc, p.text, 13 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    for (const auto& [text, rc] : labels_) {
      // A label longer than its column (a longer language) wraps onto a second line in its row.
      if (c.MeasureText(text, 13 * u) <= gfx::Width(rc)) c.Text(text, rc, p.textSecondary, 13 * u);
      else c.TextWrapped(text, RectF(rc.left, rc.top - 3 * u, rc.right, rc.bottom + 3 * u), p.textSecondary, 13 * u, 2);
    }
  }

 protected:
  struct Row {
    std::wstring label;
    Control* a = nullptr;  // none: a heading
    Control* b = nullptr;  // a second control beside the first
  };

  explicit RowsPanel(float labelW) : labelW_(labelW) {}
  // The controls show the document as it is now (when anything changed since they last did).
  virtual void Refresh() = 0;
  // Ids for UI Automation, in order, once the rows are made.
  void NumberControls() {
    int id = 10;
    for (Row& r : rows_)
      for (Control* c : {r.a, r.b})
        if (c) c->id = id++;
  }
  void EnableAll(bool on) {
    for (Row& r : rows_)
      for (Control* c : {r.a, r.b})
        if (c) c->enabled = on;
  }

  std::vector<Row> rows_;

 private:
  float ui_ = 1, labelW_;
  std::vector<std::pair<std::wstring, D2D1_RECT_F>> headings_, labels_;
};

}  // namespace

// ---------------------------------------------------------------------------
// The Page setup panel

class DocPage::Panel : public RowsPanel {
 public:
  explicit Panel(DocPage& owner) : RowsPanel(kLabelW), p_(owner) {
    auto drop = [](Dropdown& d, const wchar_t* name, std::vector<std::wstring> items) {
      d.label = d.tooltip = name;
      d.items = std::move(items);
    };
    auto spin = [](SpinBox& s, const wchar_t* name, double lo, double hi, double step, int decimals, const wchar_t* unit) {
      s.label = s.tooltip = name;
      s.minValue = lo;
      s.maxValue = hi;
      s.step = step;
      s.decimals = decimals;
      s.unit = unit;
    };
    const double longest = FromTwips(kLongest), step = Metric() ? 0.1 : 0.05;

    drop(applyTo_, T(L"Apply to"), {T(L"This section"), T(L"Whole document")});
    applyTo_.selected = 0;
    applyTo_.onChange = [this](int) { shown_ = {}; };
    std::vector<std::wstring> papers;
    for (const Paper& paper : kPapers) papers.push_back(PaperLabel(paper));
    papers.push_back(T(L"Custom size"));
    drop(paper_, T(L"Paper size"), std::move(papers));
    paper_.onChange = [this](int i) {
      if (i < 0 || i >= static_cast<int>(std::size(kPapers))) return;
      const auto [first, last] = Scope();
      if (first == kNone) return;
      // As the page lies now.
      const bool turned = Landscape(p_.Doc().Sections()[first].props);
      edit::PageSetup s;
      s.width = Twips(turned ? kPapers[i].height : kPapers[i].width);
      s.height = Twips(turned ? kPapers[i].width : kPapers[i].height);
      Page(s);
    };
    spin(width_, T(L"Page width"), FromTwips(1440), longest, step, 2, Unit());
    width_.onChange = [this](double v) {
      edit::PageSetup s;
      s.width = Twips(ToTwips(v));
      Page(s);
    };
    spin(height_, T(L"Page height"), FromTwips(1440), longest, step, 2, Unit());
    height_.onChange = [this](double v) {
      edit::PageSetup s;
      s.height = Twips(ToTwips(v));
      Page(s);
    };
    drop(orientation_, T(L"Orientation"), {T(L"Portrait"), T(L"Landscape")});
    orientation_.onChange = [this](int i) {
      edit::PageSetup s;
      s.landscape = i == 1;
      Page(s);
    };
    drop(start_, T(L"Section starts"), {T(L"On a new page"), T(L"Continuous"), T(L"On an even page"), T(L"On an odd page")});
    start_.onChange = [this](int i) {
      const auto [first, last] = Scope();
      if (first == kNone || i < 0 || i > 3) return;
      edit::Editor& ed = p_.editor_;
      ed.BeginGroup(T(u"Section start"));
      edit::Result r;
      for (uint32_t s = first; s <= last && (r = edit::SetSectionStart(ed, s, static_cast<SectionStart>(i))); ++s) {
      }
      ed.EndGroup();
      Done(r);
    };

    auto margin = [&](SpinBox& box, const wchar_t* name, std::optional<Twips> edit::PageSetup::*field) {
      spin(box, name, 0, longest, step, 2, Unit());
      box.onChange = [this, field](double v) {
        edit::PageSetup s;
        s.*field = Twips(ToTwips(v));
        Page(s);
      };
    };
    margin(top_, T(L"Top margin"), &edit::PageSetup::top);
    margin(bottom_, T(L"Bottom margin"), &edit::PageSetup::bottom);
    margin(left_, T(L"Left margin"), &edit::PageSetup::left);
    margin(right_, T(L"Right margin"), &edit::PageSetup::right);

    spin(columns_, T(L"Columns"), 1, 12, 1, 0, L"");
    columns_.onChange = [this](double v) { Columns(static_cast<int>(std::lround(v)), spacing_.value, line_.selected == 1); };
    spin(spacing_, T(L"Space between columns"), 0, longest, step, 2, Unit());
    spacing_.onChange = [this](double v) { Columns(static_cast<int>(std::lround(columns_.value)), v, line_.selected == 1); };
    drop(line_, T(L"Line between columns"), {T(L"No line"), T(L"Line between")});
    line_.onChange = [this](int i) { Columns(static_cast<int>(std::lround(columns_.value)), spacing_.value, i == 1); };

    drop(hyphenate_, T(L"Hyphenate automatically"), {T(L"Off"), T(L"On")});
    hyphenate_.onChange = [this](int i) {
      edit::Hyphenation h;
      h.automatic = i == 1;
      Done(edit::SetHyphenation(p_.editor_, h));
    };
    spin(zone_, T(L"Hyphenation zone"), 0, FromTwips(kLongest), step, 2, Unit());
    zone_.onChange = [this](double v) {
      edit::Hyphenation h;
      h.zone = Twips(ToTwips(v));
      Done(edit::SetHyphenation(p_.editor_, h));
    };
    spin(limit_, T(L"Hyphenated lines in a row (0: no limit)"), 0, 32767, 1, 0, L"");
    limit_.onChange = [this](double v) {
      edit::Hyphenation h;
      h.limit = static_cast<int32_t>(std::lround(v));
      Done(edit::SetHyphenation(p_.editor_, h));
    };

    rows_ = {{T(L"Page")},
             {T(L"Apply to"), &applyTo_},
             {T(L"Paper"), &paper_},
             {T(L"Width, height"), &width_, &height_},
             {T(L"Orientation"), &orientation_},
             {T(L"Section starts"), &start_},
             {T(L"Margins")},
             {T(L"Top, bottom"), &top_, &bottom_},
             {T(L"Left, right"), &left_, &right_},
             {T(L"Columns")},
             {T(L"Count, space"), &columns_, &spacing_},
             {T(L"Line"), &line_},
             {T(L"Hyphenation (whole document)")},
             {T(L"Automatic"), &hyphenate_},
             {T(L"Zone, limit"), &zone_, &limit_}};
    NumberControls();
  }

  std::wstring Title() const override { return T(L"Page setup"); }

  // Opened again: the selection's sections, shown as they are.
  void Open() {
    applyTo_.selected = 0;
    shown_ = {};
  }

 private:
  // What the controls show: the first section in scope, as of this selection and document.
  struct Shown {
    Pos a{kNone, 0}, f{kNone, 0};
    uint64_t generation = UINT64_MAX;
    size_t sections = 0;
    int scope = -1;
    bool operator==(const Shown&) const = default;
  };

  std::pair<uint32_t, uint32_t> Scope() const {
    const auto n = static_cast<uint32_t>(p_.Doc().Sections().size());
    if (n > 0 && applyTo_.selected == 1) return {0, n - 1};
    return p_.Sections();
  }

  void Done(const edit::Result& r) {
    p_.host_.keep(r, false);
    shown_ = {};  // what the document has now, also after a refusal
  }

  void Page(const edit::PageSetup& s) {
    const auto [first, last] = Scope();
    if (first != kNone) Done(edit::SetPageSetup(p_.editor_, first, last, s));
  }

  void Columns(int count, double space, bool line) {
    const auto [first, last] = Scope();
    if (first == kNone) return;
    edit::ColumnSetup c;
    c.count = std::clamp(count, 1, 12);
    c.space = Twips(ToTwips(space));
    c.line = line;
    Done(edit::SetColumns(p_.editor_, first, last, c));
  }

  void Refresh() override {
    const fw::doc::Document& doc = p_.Doc();
    auto [a, f] = p_.host_.selection();
    const Shown now{a, f, doc.CacheGeneration(), doc.Sections().size(), applyTo_.selected};
    if (now == shown_) return;
    shown_ = now;
    const auto [first, last] = Scope();
    const bool can = p_.editor_.CanEdit() && first != kNone;
    EnableAll(can);
    applyTo_.enabled = can && doc.Sections().size() > 1;
    applyTo_.items[0] = first != last && applyTo_.selected == 0 ? T(L"Selected sections") : T(L"This section");
    if (first == kNone) return;
    const SectionProps& s = doc.Sections()[first].props;
    const int32_t w = s.pageWidth.Value(), h = s.pageHeight.Value();
    paper_.selected = static_cast<int>(std::size(kPapers));
    for (size_t i = 0; i < std::size(kPapers); ++i)
      if (Near(std::min(w, h), kPapers[i].width) && Near(std::max(w, h), kPapers[i].height)) paper_.selected = static_cast<int>(i);
    width_.SetValue(FromTwips(w));
    height_.SetValue(FromTwips(h));
    orientation_.selected = Landscape(s) ? 1 : 0;
    start_.selected = s.start <= SectionStart::OddPage ? static_cast<int>(s.start) : -1;
    top_.SetValue(FromTwips(std::abs(s.marginTop.Value())));
    bottom_.SetValue(FromTwips(std::abs(s.marginBottom.Value())));
    left_.SetValue(FromTwips(s.marginStart.Value()));
    right_.SetValue(FromTwips(s.marginEnd.Value()));
    columns_.SetValue(s.columns.count);
    spacing_.SetValue(FromTwips(s.columns.space.Value()));
    line_.selected = s.columns.separator ? 1 : 0;
    const fw::doc::DocSettings& set = doc.Settings();
    hyphenate_.selected = set.autoHyphenation ? 1 : 0;
    zone_.SetValue(FromTwips(set.hyphenationZone.Value()));
    limit_.SetValue(set.consecutiveHyphenLimit);
    hyphenate_.enabled = zone_.enabled = limit_.enabled = p_.editor_.CanEdit();
  }

  DocPage& p_;
  Shown shown_;
  Dropdown applyTo_, paper_, orientation_, start_, line_, hyphenate_;
  SpinBox width_, height_, top_, bottom_, left_, right_, columns_, spacing_, zone_, limit_;
};

// ---------------------------------------------------------------------------
// The Paragraph panel: Word's Paragraph dialog for the selection's paragraphs. Where they differ, a
// list shows nothing chosen and a box the first one's value.

class DocPage::ParaPanel : public RowsPanel {
 public:
  explicit ParaPanel(DocPage& owner) : RowsPanel(kParaLabelW), p_(owner) {
    auto drop = [](Dropdown& d, const wchar_t* name, std::vector<std::wstring> items) {
      d.label = d.tooltip = name;
      d.items = std::move(items);
    };
    auto spin = [](SpinBox& s, const wchar_t* name, double lo, double hi, double step, int decimals, const wchar_t* unit) {
      s.label = s.tooltip = name;
      s.minValue = lo;
      s.maxValue = hi;
      s.step = step;
      s.decimals = decimals;
      s.unit = unit;
    };
    const double longest = FromTwips(kLongest), step = Metric() ? 0.1 : 0.05;

    drop(align_, T(L"Alignment"), {T(L"Left"), T(L"Centered"), T(L"Right"), T(L"Justified")});
    align_.onChange = [this](int i) {
      if (i < 0 || i >= static_cast<int>(std::size(kAlign))) return;
      edit::ParaFormat f;
      f.jc = kAlign[i];
      Format(f);
    };
    spin(left_, T(L"Left indent"), -longest, longest, step, 2, Unit());
    left_.onChange = [this](double v) {
      edit::ParaFormat f;
      f.indStart = Twips(ToTwips(v));
      Format(f);
    };
    spin(right_, T(L"Right indent"), -longest, longest, step, 2, Unit());
    right_.onChange = [this](double v) {
      edit::ParaFormat f;
      f.indEnd = Twips(ToTwips(v));
      Format(f);
    };
    drop(special_, T(L"First line or hanging indent"), {T(L"None"), T(L"First line"), T(L"Hanging")});
    special_.onChange = [this](int i) {
      if (i < 0 || i > 2) return;
      const int32_t by = by_.value > 0 ? ToTwips(by_.value) : 720;  // Word's half inch
      edit::ParaFormat f;
      f.firstLine = Twips(i == 0 ? 0 : i == 1 ? by : -by);
      Format(f);
    };
    spin(by_, T(L"Indented by"), 0, longest, step, 2, Unit());
    by_.onChange = [this](double v) {
      const int32_t by = ToTwips(v);
      edit::ParaFormat f;
      f.firstLine = Twips(special_.selected == 2 ? -by : by);
      Format(f);
    };

    auto space = [&](SpinBox& box, const wchar_t* name, std::optional<Twips> edit::ParaFormat::*field) {
      spin(box, name, 0, 1584, 6, 1, L" pt");
      box.onChange = [this, field](double v) {
        edit::ParaFormat f;
        f.*field = Twips(static_cast<int32_t>(std::lround(v * 20)));
        Format(f);
      };
    };
    space(before_, T(L"Space before"), &edit::ParaFormat::spaceBefore);
    space(after_, T(L"Space after"), &edit::ParaFormat::spaceAfter);
    drop(rule_, T(L"Line spacing"), {T(L"Single"), T(L"1.5 lines"), T(L"Double"), T(L"At least"), T(L"Exactly"), T(L"Multiple")});
    rule_.onChange = [this](int i) {
      if (i < 0 || i > 5) return;
      edit::ParaFormat f;
      f.lineRule = i == 3 ? LineRule::AtLeast : i == 4 ? LineRule::Exact : LineRule::Auto;
      // Single, 1.5 and double; 12 pt at least or exactly, and 3 lines, as Word starts them.
      f.line = i <= 2 ? 240 + 120 * i : i <= 4 ? 240 : 720;
      Format(f);
    };
    spin(at_, T(L"Line spacing at"), 0.06, 132, 0.25, 2, L"");  // lines or points: Refresh sets which
    at_.onChange = [this](double v) {
      edit::ParaFormat f;
      f.lineRule = atRule_;
      f.line = static_cast<int32_t>(std::lround(atRule_ == LineRule::Auto ? v * 240 : v * 20));
      Format(f);
    };

    auto onOff = [&](Dropdown& d, const wchar_t* name, std::optional<bool> edit::ParaFormat::*field) {
      drop(d, name, {T(L"Off"), T(L"On")});
      d.onChange = [this, field](int i) {
        if (i < 0 || i > 1) return;
        edit::ParaFormat f;
        f.*field = i == 1;
        Format(f);
      };
    };
    onOff(widow_, T(L"Widow/orphan control"), &edit::ParaFormat::widowControl);
    onOff(keepNext_, T(L"Keep with next"), &edit::ParaFormat::keepNext);
    onOff(keepLines_, T(L"Keep lines together"), &edit::ParaFormat::keepLines);
    onOff(breakBefore_, T(L"Page break before"), &edit::ParaFormat::pageBreakBefore);

    rows_ = {{T(L"General")},
             {T(L"Alignment"), &align_},
             {T(L"Indents")},
             {T(L"Left, right"), &left_, &right_},
             {T(L"Special, by"), &special_, &by_},
             {T(L"Spacing")},
             {T(L"Before, after"), &before_, &after_},
             {T(L"Line, at"), &rule_, &at_},
             {T(L"Line and page breaks")},
             {T(L"Widow control"), &widow_},
             {T(L"Keep with next"), &keepNext_},
             {T(L"Keep together"), &keepLines_},
             {T(L"Page break"), &breakBefore_}};
    NumberControls();
  }

  std::wstring Title() const override { return T(L"Paragraph"); }
  void Open() { shown_ = {}; }

 private:
  static constexpr Justification kAlign[] = {Justification::Start, Justification::Center, Justification::End,
                                             Justification::Both};
  struct Shown {
    Pos a{kNone, 0}, f{kNone, 0};
    uint64_t generation = UINT64_MAX;
    bool operator==(const Shown&) const = default;
  };

  edit::Range Selection() const {
    auto [a, f] = p_.host_.selection();
    if (Before(f, a)) std::swap(a, f);
    return {a, f};
  }
  std::vector<uint32_t> Paragraphs() const {
    const edit::Range r = Selection();
    const size_t n = p_.Doc().Paragraphs().size();
    if (r.from.para >= n || r.to.para >= n) return {};
    return edit::ParagraphsOf(p_.Doc(), r);
  }

  void Format(const edit::ParaFormat& f) {
    shown_ = {};  // what the paragraphs have now, also after a refusal
    p_.FormatParas(f);
  }

  void Refresh() override {
    const fw::doc::Document& doc = p_.Doc();
    auto [a, f] = p_.host_.selection();
    const Shown now{a, f, doc.CacheGeneration()};
    if (now == shown_) return;
    shown_ = now;
    const std::vector<uint32_t> paras = Paragraphs();
    EnableAll(p_.editor_.CanEdit() && !paras.empty());
    if (paras.empty()) return;
    const fw::doc::ParaProps first = doc.ResolvedPara(paras.front());  // a copy: resolving more may move it
    // Whether every paragraph has what the first has.
    auto same = [&](auto get) {
      for (uint32_t q : paras)
        if (!(get(doc.ResolvedPara(q)) == get(first))) return false;
      return true;
    };
    auto onOff = [&](Dropdown& d, bool fw::doc::ParaProps::*field) {
      d.selected = same([field](const fw::doc::ParaProps& p) { return p.*field; }) ? (first.*field ? 1 : 0) : -1;
    };

    align_.selected = -1;
    for (size_t i = 0; i < std::size(kAlign); ++i)
      if (first.jc == kAlign[i]) align_.selected = static_cast<int>(i);
    if (!same([](const fw::doc::ParaProps& p) { return p.jc; })) align_.selected = -1;
    left_.SetValue(FromTwips(first.indStart.Value()));
    right_.SetValue(FromTwips(first.indEnd.Value()));
    const int32_t fl = first.firstLine.Value();
    auto kind = [](const fw::doc::ParaProps& p) { return p.firstLine.Value() > 0 ? 1 : p.firstLine.Value() < 0 ? 2 : 0; };
    special_.selected = same(kind) ? kind(first) : -1;
    by_.SetValue(FromTwips(std::abs(fl)));
    before_.SetValue(first.spaceBefore.Value() / 20.0);
    after_.SetValue(first.spaceAfter.Value() / 20.0);

    // Line spacing: Word's names for 1, 1.5 and 2 lines; the box in lines or in points.
    int rule = 5;
    if (first.lineRule == LineRule::AtLeast) rule = 3;
    else if (first.lineRule == LineRule::Exact) rule = 4;
    else if (first.line == 240 || first.line == 360 || first.line == 480) rule = (first.line - 240) / 120;
    rule_.selected = same([](const fw::doc::ParaProps& p) { return std::make_pair(p.lineRule, p.line); }) ? rule : -1;
    atRule_ = first.lineRule;
    const bool points = atRule_ != LineRule::Auto;
    at_.minValue = points ? 0.7 : 0.06;
    at_.maxValue = points ? 1584 : 132;
    at_.step = points ? 1 : 0.25;
    at_.decimals = points ? 1 : 2;
    at_.unit = points ? L" pt" : L"";
    at_.SetValue(points ? first.line / 20.0 : first.line / 240.0);

    onOff(widow_, &fw::doc::ParaProps::widowControl);
    onOff(keepNext_, &fw::doc::ParaProps::keepNext);
    onOff(keepLines_, &fw::doc::ParaProps::keepLines);
    onOff(breakBefore_, &fw::doc::ParaProps::pageBreakBefore);
  }

  DocPage& p_;
  Shown shown_;
  LineRule atRule_ = LineRule::Auto;  // what the at box sets: lines (Auto) or points
  Dropdown align_, special_, rule_, widow_, keepNext_, keepLines_, breakBefore_;
  SpinBox left_, right_, by_, before_, after_, at_;
};

// ---------------------------------------------------------------------------

DocPage::DocPage(edit::Editor& editor, Host host) : editor_(editor), host_(std::move(host)) {
  panel_ = std::make_unique<Panel>(*this);
  paraPanel_ = std::make_unique<ParaPanel>(*this);
}

DocPage::~DocPage() = default;

bool DocPage::ShowsIn(const PanelContent* shown) const {
  return shown && (shown == panel_.get() || shown == paraPanel_.get());
}

bool DocPage::Owns(int cmd) {
  return (cmd >= CmdPageSetup && cmd <= CmdParagraph) || (cmd >= CmdIndentLeftBox && cmd <= CmdSpaceAfterBox);
}

std::vector<int> DocPage::CommandIds() {
  std::vector<int> ids;
  for (const Named& n : kNames)
    if (n.cmd != CmdLayoutBox) ids.push_back(n.cmd);
  return ids;
}

int DocPage::Shortcut(UINT vk, UINT mods) {
  if (!(mods & kModCtrl) || (mods & kModAlt)) return 0;
  const bool shift = (mods & kModShift) != 0;
  if (vk == VK_RETURN) return shift ? CmdColumnBreak : CmdPageBreak;
  // Word's hyphens: Ctrl+- (the key by 0; Ctrl+numpad minus still zooms out) and Ctrl+Shift+-.
  if (vk == VK_OEM_MINUS) return shift ? CmdNonBreakingHyphen : CmdOptionalHyphen;
  return 0;
}

std::pair<uint32_t, uint32_t> DocPage::Sections() const {
  const auto n = static_cast<uint32_t>(Doc().Sections().size());
  if (n == 0) return {kNone, kNone};
  if (host_.headerSection)
    if (const uint32_t s = host_.headerSection(); s < n) return {s, s};
  auto [a, f] = host_.selection();
  uint32_t x = edit::SectionOf(Doc(), a.para), y = edit::SectionOf(Doc(), f.para);
  if (x > y) std::swap(x, y);
  return {x, y};
}

CommandState DocPage::Query(int cmd) {
  CommandState s;
  if (const Named* n = NameOf(cmd)) {
    s.label = n->label;
    s.shortcut = n->keys;
  }
  const bool can = editor_.CanEdit();
  if (cmd >= CmdIndentLeftBox && cmd <= CmdSpaceAfterBox) {
    static constexpr UiText kBoxes[] = {N_(L"Indent left"), N_(L"Indent right"), N_(L"Spacing before"), N_(L"Spacing after")};
    s.label = kBoxes[cmd - CmdIndentLeftBox];
    s.enabled = can;
    return s;
  }
  const auto [first, last] = Sections();
  const SectionProps* p = first != kNone ? &Doc().Sections()[first].props : nullptr;
  // Breaks go into the main text, where the caret is (not in a header or footer).
  auto inBody = [&] {
    const Pos f = host_.selection().second;
    const bool header = host_.headerSection && host_.headerSection() != kNone;
    return can && !header && f.para < Doc().Paragraphs().size() && Doc().Para(f.para).story == Doc().MainStory();
  };
  switch (cmd) {
    case CmdPortrait:
    case CmdLandscape:
      s.enabled = can && p;
      s.checked = p && Landscape(*p) == (cmd == CmdLandscape);
      break;
    case CmdMarginsNormal:
    case CmdMarginsNarrow:
    case CmdMarginsModerate:
    case CmdMarginsWide:
      s.enabled = can && p;
      s.checked = p && MarginPresetOf(*p) == cmd - CmdMarginsNormal;
      break;
    case CmdColumnsOne:
    case CmdColumnsTwo:
    case CmdColumnsThree:
      s.enabled = can && p;
      s.checked = p && p->columns.count == cmd - CmdColumnsOne + 1;
      break;
    case CmdPageBreak:
    case CmdColumnBreak:
    case CmdSectionNextPage:
    case CmdSectionContinuous:
    case CmdSectionEvenPage:
    case CmdSectionOddPage: s.enabled = inBody(); break;
    case CmdDeleteSectionBreak:
      s.enabled = inBody() && edit::SectionOf(Doc(), host_.selection().second.para) + 1 < Doc().Sections().size();
      break;
    case CmdHyphenation:
      s.enabled = can;
      s.checked = Doc().Settings().autoHyphenation;
      break;
    default: s.enabled = can; break;  // Page setup, the Layout menu, hyphens (in any text)
  }
  return s;
}

void DocPage::Execute(int cmd) {
  if (!Query(cmd).enabled) return;
  const auto [first, last] = Sections();
  switch (cmd) {
    case CmdPageSetup:
      panel_->Open();
      host_.showPanel(panel_.get(), true);
      return;
    case CmdParagraph:
    case CmdIndentLeftBox:  // a box's KeyTip or command search: the panel with all of them
    case CmdIndentRightBox:
    case CmdSpaceBeforeBox:
    case CmdSpaceAfterBox:
      paraPanel_->Open();
      host_.showPanel(paraPanel_.get(), true);
      return;
    case CmdPortrait:
    case CmdLandscape: {
      edit::PageSetup s;
      s.landscape = cmd == CmdLandscape;
      host_.keep(edit::SetPageSetup(editor_, first, last, s), false);
      return;
    }
    case CmdMarginsNormal:
    case CmdMarginsNarrow:
    case CmdMarginsModerate:
    case CmdMarginsWide: {
      const MarginPreset& m = kMargins[cmd - CmdMarginsNormal];
      Margins(m.top, m.bottom, m.left, m.right);
      return;
    }
    case CmdColumnsOne:
    case CmdColumnsTwo:
    case CmdColumnsThree: {
      const fw::doc::Columns& now = Doc().Sections()[first].props.columns;
      edit::ColumnSetup c;
      c.count = cmd - CmdColumnsOne + 1;
      c.space = now.space;
      c.line = now.separator && c.count > 1;
      host_.keep(edit::SetColumns(editor_, first, last, c), false);
      return;
    }
    case CmdPageBreak: Break(false); return;
    case CmdColumnBreak: Break(true); return;
    case CmdOptionalHyphen: Hyphen(false); return;
    case CmdNonBreakingHyphen: Hyphen(true); return;
    case CmdSectionNextPage: SectionBreak(SectionStart::NextPage); return;
    case CmdSectionContinuous: SectionBreak(SectionStart::Continuous); return;
    case CmdSectionEvenPage: SectionBreak(SectionStart::EvenPage); return;
    case CmdSectionOddPage: SectionBreak(SectionStart::OddPage); return;
    case CmdDeleteSectionBreak:
      // The section's pages all take the next one's setup: laid out like a change of the page.
      host_.keep(edit::DeleteSectionBreak(editor_, edit::SectionOf(Doc(), host_.selection().second.para)), true);
      return;
    case CmdHyphenation: {
      edit::Hyphenation h;
      h.automatic = !Doc().Settings().autoHyphenation;
      host_.keep(edit::SetHyphenation(editor_, h), false);
      return;
    }
    default: return;
  }
}

void DocPage::Margins(int32_t top, int32_t bottom, int32_t left, int32_t right) {
  const auto [first, last] = Sections();
  edit::PageSetup s;
  s.top = Twips(top);
  s.bottom = Twips(bottom);
  s.left = Twips(left);
  s.right = Twips(right);
  host_.keep(edit::SetPageSetup(editor_, first, last, s), false);
}

// A break replaces the selection, as typing does; one undo step.
void DocPage::Break(bool column) {
  auto [from, to] = host_.selection();
  if (Before(to, from)) std::swap(from, to);
  if (from == to) {
    host_.apply(edit::InsertBreak(editor_, from, column));
    return;
  }
  editor_.BeginGroup(column ? T(u"Column break") : T(u"Page break"));
  edit::Result r = editor_.Delete(from, to);
  if (r) r = edit::InsertBreak(editor_, r.caret, column);
  if (r) editor_.EndGroup();
  else editor_.CancelGroup();  // nothing half done
  host_.apply(r);
}

void DocPage::Hyphen(bool nonBreaking) {
  // U+00AD and U+2011, which the editor writes as w:softHyphen and w:noBreakHyphen.
  const std::u16string text(1, nonBreaking ? char16_t(0x2011) : char16_t(0x00AD));
  auto [from, to] = host_.selection();
  if (Before(to, from)) std::swap(from, to);
  host_.apply(from == to ? editor_.InsertText(from, text) : editor_.Replace(from, to, text));
}

void DocPage::SectionBreak(SectionStart start) {
  auto [from, to] = host_.selection();
  if (Before(to, from)) std::swap(from, to);
  if (from == to) {
    host_.apply(edit::InsertSectionBreak(editor_, from, start));
    return;
  }
  editor_.BeginGroup(T(u"Section break"));
  edit::Result r = editor_.Delete(from, to);
  if (r) r = edit::InsertSectionBreak(editor_, r.caret, start);
  if (r) editor_.EndGroup();
  else editor_.CancelGroup();
  host_.apply(r);
}

ToolItem DocPage::SizeMenu() {
  ToolItem t;
  t.cmd = CmdLayoutBox;
  t.dropdown = true;
  t.tooltip = T(L"Size: the paper size");
  t.enabled = editor_.CanEdit();
  const auto [first, last] = Sections();
  const SectionProps* p = first != kNone ? &Doc().Sections()[first].props : nullptr;
  const int32_t w = p ? std::min(p->pageWidth.Value(), p->pageHeight.Value()) : 0;
  const int32_t h = p ? std::max(p->pageWidth.Value(), p->pageHeight.Value()) : 0;
  for (const Paper& paper : kPapers)
    t.choices.push_back(std::wstring(p && Near(w, paper.width) && Near(h, paper.height) ? L"✓ " : L"") + PaperLabel(paper));
  t.choices.push_back(T(L"More paper sizes…"));
  t.onChange = [this](const ToolValue& v) {
    if (v.index >= 0 && v.index < static_cast<int>(std::size(kPapers))) PaperSize(static_cast<size_t>(v.index));
    else if (v.index == static_cast<int>(std::size(kPapers))) Execute(CmdPageSetup);
  };
  return t;
}

ToolItem DocPage::ParaSpin(int cmd) {
  ToolItem t;
  t.kind = ToolKind::SpinBox;
  t.cmd = cmd;
  t.width = 100;  // "15,24 cm" beside the spin arrows
  const CommandState q = Query(cmd);
  t.tooltip = q.label;
  const bool indent = cmd == CmdIndentLeftBox || cmd == CmdIndentRightBox;
  const double longest = FromTwips(kLongest);
  t.minValue = indent ? -longest : 0;
  t.maxValue = indent ? longest : 1584;
  t.step = indent ? (Metric() ? 0.1 : 0.05) : 6;
  t.decimals = indent ? 2 : 1;
  t.unit = indent ? Unit() : L" pt";
  auto [a, f] = host_.selection();
  if (Before(f, a)) std::swap(a, f);
  const std::vector<uint32_t> paras =
      a.para < Doc().Paragraphs().size() && f.para < Doc().Paragraphs().size() ? edit::ParagraphsOf(Doc(), {a, f}) : std::vector<uint32_t>{};
  t.enabled = q.enabled && !paras.empty();
  if (!paras.empty()) {
    const fw::doc::ParaProps first = Doc().ResolvedPara(paras.front());
    t.value = cmd == CmdIndentLeftBox    ? FromTwips(first.indStart.Value())
              : cmd == CmdIndentRightBox ? FromTwips(first.indEnd.Value())
              : cmd == CmdSpaceBeforeBox ? first.spaceBefore.Value() / 20.0
                                         : first.spaceAfter.Value() / 20.0;
  }
  t.onChange = [this, cmd, indent](const ToolValue& v) {
    edit::ParaFormat pf;
    const Twips tw(indent ? ToTwips(v.number) : static_cast<int32_t>(std::lround(v.number * 20)));
    (cmd == CmdIndentLeftBox ? pf.indStart : cmd == CmdIndentRightBox ? pf.indEnd : cmd == CmdSpaceBeforeBox ? pf.spaceBefore : pf.spaceAfter) = tw;
    FormatParas(pf);
  };
  return t;
}

void DocPage::FormatParas(const edit::ParaFormat& f) {
  auto [a, b] = host_.selection();
  if (Before(b, a)) std::swap(a, b);
  const size_t n = Doc().Paragraphs().size();
  if (a.para >= n || b.para >= n) return;
  const edit::Range r{a, b};
  if (edit::ParagraphsOf(Doc(), r).empty()) return;
  std::u16string why;
  if (editor_.CanEdit(&why)) {
    edit::TreeEdit te(editor_, r.from);
    edit::FormatParagraphs(te, r, f);
    why = te.Refused();
  }
  host_.formatted(std::wstring(why.begin(), why.end()));
}

void DocPage::PaperSize(size_t i) {
  const auto [first, last] = Sections();
  if (i >= std::size(kPapers) || first == kNone || !editor_.CanEdit()) return;
  // As the page lies now.
  const bool turned = Landscape(Doc().Sections()[first].props);
  edit::PageSetup s;
  s.width = Twips(turned ? kPapers[i].height : kPapers[i].width);
  s.height = Twips(turned ? kPapers[i].width : kPapers[i].height);
  host_.keep(edit::SetPageSetup(editor_, first, last, s), false);
}
