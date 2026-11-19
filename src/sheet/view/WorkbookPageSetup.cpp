// Page setup (S3, s3-print): the Page Setup panel (Page, Margins, Header/Footer, Sheet) beside the
// grid or print preview, each change applied at once (the preview paginates again); the print area,
// print titles and page breaks; what print preview prints (the active sheet, the entire workbook,
// the selection); Page Break Preview.

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <array>
#include <cwchar>
#include <functional>
#include <optional>

#include "Commands.h"
#include "core/Strings.h"
#include "pkg/Formula.h"
#include "sheet/view/WorkbookFormat.h"
#include "sheet/view/WorkbookPanels.h"
#include "sheet/view/WorkbookView.h"
#include "ui/Menu.h"
#include "ui/Theme.h"

using namespace gfx;

namespace fw::sheet::view {

namespace {

struct Paper {
  const wchar_t* name;
  uint32_t code;  // ECMA-376's paperSize
};
constexpr Paper kPapers[] = {{L"Letter", 1},   {L"Legal", 5},    {L"Executive", 7},     {L"Tabloid", 3},
                             {L"A3", 8},       {L"A4", 9},       {L"A5", 11},           {L"B4 (JIS)", 12},
                             {L"B5 (JIS)", 13}, {L"Envelope #10", 20}, {L"Envelope DL", 27}};

// Excel's margin presets (Page Layout › Margins), inches: top, bottom, left, right, header, footer.
struct MarginPreset {
  const wchar_t* name;
  double top, bottom, left, right, header, footer;
};
constexpr MarginPreset kMargins[] = {{L"Normal", 0.75, 0.75, 0.7, 0.7, 0.3, 0.3},
                                     {L"Wide", 1, 1, 1, 1, 0.5, 0.5},
                                     {L"Narrow", 0.75, 0.75, 0.25, 0.25, 0.3, 0.3}};

// The scaling shortcuts of Excel's print preview.
constexpr const wchar_t* kScalings[] = {L"No scaling", L"Fit sheet on one page", L"Fit all columns on one page",
                                        L"Fit all rows on one page"};

// Header and footer codes as Excel's custom header shows them, and as the file has them.
constexpr std::pair<wchar_t, const wchar_t*> kCodes[] = {{L'P', L"&[Page]"}, {L'N', L"&[Pages]"}, {L'D', L"&[Date]"},
                                                         {L'T', L"&[Time]"}, {L'F', L"&[File]"},  {L'A', L"&[Tab]"},
                                                         {L'Z', L"&[Path]"}, {L'G', L"&[Picture]"}};

std::wstring ToShown(const std::wstring& s) {
  std::wstring out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == L'&' && i + 1 < s.size()) {
      const wchar_t c = s[i + 1];
      const auto* code = std::find_if(std::begin(kCodes), std::end(kCodes), [&](const auto& k) { return k.first == c; });
      if (code != std::end(kCodes)) {
        out += code->second;
        ++i;
        continue;
      }
      out += s.substr(i, 2);  // && and the formatting codes as they are
      ++i;
      continue;
    }
    out += s[i];
  }
  return out;
}

std::wstring FromShown(const std::wstring& s) {
  std::wstring out;
  for (size_t i = 0; i < s.size(); ++i) {
    bool done = false;
    if (s[i] == L'&')
      for (const auto& [c, shown] : kCodes) {
        const size_t n = std::wcslen(shown);
        if (_wcsnicmp(s.c_str() + i, shown, n) == 0) {
          out += L'&';
          out += c;
          i += n - 1;
          done = true;
          break;
        }
      }
    if (!done) out += s[i];
  }
  return out;
}

// A header or footer's left, center and right sections (text before any section code is centered).
std::array<std::wstring, 3> Sections(const std::string& code) {
  std::array<std::wstring, 3> out;
  const std::wstring s = Utf8ToWide(code);
  int at = 1;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == L'&' && i + 1 < s.size()) {
      const wchar_t c = s[i + 1];
      if (c == L'L' || c == L'C' || c == L'R') {
        at = c == L'L' ? 0 : c == L'C' ? 1 : 2;
        ++i;
        continue;
      }
      out[at] += s.substr(i, 2);
      ++i;
      continue;
    }
    out[at] += s[i];
  }
  for (std::wstring& t : out) t = ToShown(t);
  return out;
}

std::string Join(const std::array<std::wstring, 3>& sections) {
  std::wstring out;
  const wchar_t* tags[] = {L"&L", L"&C", L"&R"};
  for (int i = 0; i < 3; ++i)
    if (!sections[i].empty()) out += tags[i] + FromShown(sections[i]);
  return WideToUtf8(out);
}

// Excel's header and footer presets, labelled as Excel labels them for this sheet and file.
struct HfPreset {
  std::wstring label;
  std::string code;
};
std::vector<HfPreset> Presets(const std::wstring& sheet, const std::wstring& file) {
  return {{L"(none)", ""},
          {L"Page 1", "&CPage &P"},
          {L"Page 1 of ?", "&CPage &P of &N"},
          {sheet, "&C&A"},
          {L"Confidential, date, Page 1", "&LConfidential&C&D&RPage &P"},
          {file, "&C&F"},
          {L"Path and " + file, "&C&Z&F"}};
}

// Lengths in the user's units: centimeters where Windows measures metric, else inches.
bool Metric() {
  wchar_t buf[4] = {};
  return GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IMEASURE, buf, 4) > 0 && buf[0] == L'0';
}
wchar_t Decimal() {
  wchar_t buf[8] = {};
  return GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, buf, 8) > 0 && buf[0] ? buf[0] : L'.';
}
std::wstring Length(double inches) {
  const double v = Metric() ? inches * 2.54 : inches;
  wchar_t buf[32];
  std::swprintf(buf, 32, L"%g", std::round(v * 100) / 100);
  std::wstring s = buf;
  std::replace(s.begin(), s.end(), L'.', Decimal());
  return s;
}
bool ParseNumber(std::wstring t, double& v) {
  std::replace(t.begin(), t.end(), L',', L'.');
  wchar_t* end = nullptr;
  v = std::wcstod(t.c_str(), &end);
  while (end && *end == L' ') ++end;
  return !t.empty() && end && *end == 0 && std::isfinite(v);
}
bool ParseLength(const std::wstring& t, double& inches) {
  double v;
  if (!ParseNumber(t, v) || v < 0) return false;
  inches = Metric() ? v / 2.54 : v;
  return inches < 49;
}

// "$A$1:$F$30", "$1:$2", "$A:$B".
std::string AbsRange(const Range& r) {
  auto col = [](uint32_t c) { return "$" + ColumnName(static_cast<int32_t>(c)); };
  auto row = [](uint32_t x) { return "$" + std::to_string(x + 1); };
  if (r.firstCol == 0 && r.lastCol == kMaxCols - 1) return row(r.firstRow) + ":" + row(r.lastRow);
  if (r.firstRow == 0 && r.lastRow == kMaxRows - 1) return col(r.firstCol) + ":" + col(r.lastCol);
  std::string s = col(r.firstCol) + row(r.firstRow);
  if (r.lastRow != r.firstRow || r.lastCol != r.firstCol) s += ":" + col(r.lastCol) + row(r.lastRow);
  return s;
}

std::wstring AreasText(const std::vector<Range>& areas) {
  std::string s;
  for (const Range& r : areas) s += (s.empty() ? "" : ",") + AbsRange(r);
  return Utf8ToWide(s);
}

// Typed areas ("A1:F30", "$1:$2", several with commas or semicolons); false for anything else.
bool ParseTyped(std::wstring t, std::vector<Range>& out) {
  out.clear();
  std::replace(t.begin(), t.end(), L';', L',');
  t.erase(std::remove(t.begin(), t.end(), L' '), t.end());
  if (t.empty()) return true;
  size_t parts = 1 + static_cast<size_t>(std::count(t.begin(), t.end(), L','));
  out = PrintLayout::ParseAreas(WideToUtf8(t));
  return out.size() == parts;
}

const DefinedName* NameOf(const Workbook& book, const char* name, size_t sheet) {
  const DefinedName* n = book.FindName(name, static_cast<int>(sheet));
  return n && n->sheet == static_cast<int>(sheet) ? n : nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// The panel

class WorkbookView::PageSetupPanel : public PanelContent {
 public:
  enum Tab { kTabPage, kTabMargins, kTabHeader, kTabSheet, kTabs };

  explicit PageSetupPanel(WorkbookView& view) : v_(view) {
    const wchar_t* names[] = {L"Page", L"Margins", L"Header", L"Sheet"};
    for (int i = 0; i < kTabs; ++i) {
      tabs_[i].text = names[i];
      tabs_[i].toggle = true;
      tabs_[i].textSize = 12;
      tabs_[i].onInvoke = [this, i] { Show(static_cast<Tab>(i)); };
    }
    what_.items = {L"Active sheet", L"Entire workbook", L"Selection"};
    Choice(what_, [this](int i) { v_.PageSetupCommand(CmdPrintActiveSheet + i); });
    // Page
    Toggle(portrait_, L"Portrait", [this] { Change([](PageSetup& p, HeaderFooter&, bool&) { p.orientation = Orientation::Portrait; }); });
    Toggle(landscape_, L"Landscape",
           [this] { Change([](PageSetup& p, HeaderFooter&, bool&) { p.orientation = Orientation::Landscape; }); });
    for (const Paper& p : kPapers) paper_.items.push_back(p.name);
    Choice(paper_, [this](int i) {
      Change([&](PageSetup& p, HeaderFooter&, bool&) {
        p.paperSize = kPapers[i].code;
        p.paperWidth.clear();
        p.paperHeight.clear();
      });
    });
    for (const wchar_t* s : kScalings) scaling_.items.push_back(s);
    Choice(scaling_, [this](int i) { v_.PageSetupCommand(CmdNoScaling + i); Refresh(); });
    Number(scale_, [this](double v) {
      if (v < 10 || v > 400) return false;
      Change([&](PageSetup& p, HeaderFooter&, bool& fit) {
        p.scale = static_cast<uint32_t>(std::lround(v));
        fit = false;
      });
      return true;
    });
    for (FocusComboBox* b : {&fitW_, &fitH_})
      Number(*b, [this, b](double v) {
        if (v < 0 || v > 32767) return false;
        Change([&](PageSetup& p, HeaderFooter&, bool& fit) {
          (b == &fitW_ ? p.fitToWidth : p.fitToHeight) = static_cast<uint32_t>(std::lround(v));
          fit = true;
        });
        return true;
      });
    first_.autoComplete = false;
    first_.validate = [](std::wstring& t) {
      double v;
      return t.empty() || _wcsicmp(t.c_str(), L"Auto") == 0 || (ParseNumber(t, v) && v == std::floor(v) && std::abs(v) < 32767);
    };
    first_.onCommit = [this](const std::wstring& t, int) {
      double v = 0;
      const bool number = ParseNumber(t, v);
      Change([&](PageSetup& p, HeaderFooter&, bool&) {
        p.useFirstPageNumber = number;
        if (number) p.firstPageNumber = static_cast<int>(v);
      });
    };
    // Margins
    for (const MarginPreset& m : kMargins) preset_.items.push_back(m.name);
    Choice(preset_, [this](int i) {
      Change([&](PageSetup& p, HeaderFooter&, bool&) {
        const MarginPreset& m = kMargins[i];
        p.margins = {m.left, m.right, m.top, m.bottom, m.header, m.footer};
      });
    });
    double PageMargins::*fields[] = {&PageMargins::top,    &PageMargins::bottom, &PageMargins::left,
                                     &PageMargins::right,  &PageMargins::header, &PageMargins::footer};
    for (int i = 0; i < 6; ++i) {
      FocusComboBox& b = margin_[i];
      b.autoComplete = false;
      b.validate = [](std::wstring& t) {
        double v;
        return ParseLength(t, v);
      };
      b.onCommit = [this, field = fields[i]](const std::wstring& t, int) {
        double v;
        if (ParseLength(t, v)) Change([&](PageSetup& p, HeaderFooter&, bool&) { p.margins.*field = v; });
      };
    }
    Toggle(centerH_, L"Horizontally",
           [this] { Change([](PageSetup& p, HeaderFooter&, bool&) { p.horizontalCentered = !p.horizontalCentered; }); });
    Toggle(centerV_, L"Vertically",
           [this] { Change([](PageSetup& p, HeaderFooter&, bool&) { p.verticalCentered = !p.verticalCentered; }); });
    // Header and footer
    for (int f = 0; f < 2; ++f) {
      Choice(hfPreset_[f], [this, f](int i) {
        const std::vector<HfPreset> presets = Presets(Utf8ToWide(v_.Current().Name()), v_.Title());
        if (i < 0 || static_cast<size_t>(i) >= presets.size()) return;
        Change([&](PageSetup&, HeaderFooter& h, bool&) { (f ? h.oddFooter : h.oddHeader) = presets[static_cast<size_t>(i)].code; });
      });
      for (int s = 0; s < 3; ++s) {
        FocusComboBox& b = section_[f][s];
        b.autoComplete = false;
        b.onCommit = [this, f](const std::wstring&, int) {
          std::array<std::wstring, 3> parts;
          for (int k = 0; k < 3; ++k) parts[k] = section_[f][k].Text();
          const std::string code = Join(parts);
          Change([&](PageSetup&, HeaderFooter& h, bool&) { (f ? h.oddFooter : h.oddHeader) = code; });
        };
      }
    }
    Toggle(oddEven_, L"Different odd and even pages",
           [this] { Change([](PageSetup&, HeaderFooter& h, bool&) { h.differentOddEven = !h.differentOddEven; }); });
    Toggle(firstDiff_, L"Different first page",
           [this] { Change([](PageSetup&, HeaderFooter& h, bool&) { h.differentFirst = !h.differentFirst; }); });
    // Sheet
    for (FocusComboBox* b : {&area_, &rows_, &cols_}) {
      b->autoComplete = false;
      b->validate = [b, this](std::wstring& t) {
        std::vector<Range> areas;
        if (!ParseTyped(t, areas)) return false;
        if (b == &rows_ || b == &cols_) {  // one band of whole rows or columns
          if (areas.size() > 1) return false;
          for (Range& r : areas) r = b == &rows_ ? Range{r.firstRow, 0, r.lastRow, kMaxCols - 1} : Range{0, r.firstCol, kMaxRows - 1, r.lastCol};
        }
        t = AreasText(areas);
        return true;
      };
      b->onCommit = [this, b](const std::wstring& t, int) {
        std::vector<Range> areas;
        if (!ParseTyped(t, areas)) return;
        if (b == &area_) {
          v_.SetPrintArea(areas);
        } else {
          std::optional<std::pair<uint32_t, uint32_t>> rows = v_.TitleRows(), cols = v_.TitleCols();
          auto& set = b == &rows_ ? rows : cols;
          set.reset();
          if (!areas.empty())
            set = b == &rows_ ? std::make_pair(areas[0].firstRow, areas[0].lastRow) : std::make_pair(areas[0].firstCol, areas[0].lastCol);
          v_.SetPrintTitles(rows, cols);
        }
        Refresh();
      };
    }
    Toggle(gridlines_, L"Gridlines", [this] { Change([](PageSetup& p, HeaderFooter&, bool&) { p.gridLines = !p.gridLines; }); });
    Toggle(headings_, L"Headings", [this] { Change([](PageSetup& p, HeaderFooter&, bool&) { p.headings = !p.headings; }); });
    Toggle(down_, L"Down, then over",
           [this] { Change([](PageSetup& p, HeaderFooter&, bool&) { p.pageOrder = PageOrder::DownThenOver; }); });
    Toggle(over_, L"Over, then down",
           [this] { Change([](PageSetup& p, HeaderFooter&, bool&) { p.pageOrder = PageOrder::OverThenDown; }); });
  }

  std::wstring Title() const override { return L"Page setup"; }

  void Open(Tab tab) {
    tab_ = tab;
    Refresh();
  }

  // The controls from the sheet as it is now (after each change, an undo, another sheet).
  void Refresh() {
    const Sheet& s = v_.Current();
    const PageSetup& p = s.Page();
    const HeaderFooter& h = s.HeadersFooters();
    const bool fit = s.Props().fitToPage;
    for (int i = 0; i < kTabs; ++i) tabs_[i].checked = i == tab_;
    what_.ReplaceText(what_.items[static_cast<size_t>(std::clamp(v_.printWhat_, 0, 2))]);
    portrait_.checked = p.orientation != Orientation::Landscape;
    landscape_.checked = p.orientation == Orientation::Landscape;
    const auto* paper = std::find_if(std::begin(kPapers), std::end(kPapers), [&](const Paper& x) { return x.code == p.paperSize; });
    paper_.ReplaceText(!p.paperWidth.empty() ? Utf8ToWide(p.paperWidth + " x " + p.paperHeight)
                       : paper != std::end(kPapers) ? paper->name
                                                     : L"Paper " + std::to_wstring(p.paperSize));
    int scaling = !fit ? (p.scale == 100 ? 0 : -1)
                  : p.fitToWidth == 1 && p.fitToHeight == 1 ? 1
                  : p.fitToWidth == 1 && p.fitToHeight == 0 ? 2
                  : p.fitToWidth == 0 && p.fitToHeight == 1 ? 3
                                                            : -1;
    scaling_.ReplaceText(scaling < 0 ? std::wstring(L"Custom") : kScalings[scaling]);
    scale_.ReplaceText(std::to_wstring(p.scale));
    fitW_.ReplaceText(std::to_wstring(p.fitToWidth));
    fitH_.ReplaceText(std::to_wstring(p.fitToHeight));
    first_.ReplaceText(p.useFirstPageNumber ? std::to_wstring(p.firstPageNumber) : L"Auto");
    const PageMargins& m = p.margins;
    const double values[] = {m.top, m.bottom, m.left, m.right, m.header, m.footer};
    for (int i = 0; i < 6; ++i) margin_[i].ReplaceText(Length(values[i]));
    std::wstring preset = L"Custom";
    for (const MarginPreset& x : kMargins)
      if (x.top == m.top && x.bottom == m.bottom && x.left == m.left && x.right == m.right && x.header == m.header && x.footer == m.footer)
        preset = x.name;
    preset_.ReplaceText(preset);
    centerH_.checked = p.horizontalCentered;
    centerV_.checked = p.verticalCentered;
    const std::vector<HfPreset> presets = Presets(Utf8ToWide(s.Name()), v_.Title());
    for (int f = 0; f < 2; ++f) {
      const std::string& code = f ? h.oddFooter : h.oddHeader;
      hfPreset_[f].items.clear();
      std::wstring shown = L"Custom";
      for (const HfPreset& x : presets) {
        hfPreset_[f].items.push_back(x.label);
        if (x.code == code) shown = x.label;
      }
      hfPreset_[f].ReplaceText(shown);
      const std::array<std::wstring, 3> parts = Sections(code);
      for (int k = 0; k < 3; ++k) section_[f][k].ReplaceText(parts[k]);
    }
    oddEven_.checked = h.differentOddEven;
    firstDiff_.checked = h.differentFirst;
    const DefinedName* area = NameOf(*v_.book_, "_xlnm.Print_Area", v_.sheet_);
    area_.ReplaceText(area ? AreasText(PrintLayout::ParseAreas(area->formula)) : std::wstring());
    const auto rows = v_.TitleRows(), cols = v_.TitleCols();
    rows_.ReplaceText(rows ? AreasText({Range{rows->first, 0, rows->second, kMaxCols - 1}}) : std::wstring());
    cols_.ReplaceText(cols ? AreasText({Range{0, cols->first, kMaxRows - 1, cols->second}}) : std::wstring());
    gridlines_.checked = p.gridLines;
    headings_.checked = p.headings;
    down_.checked = p.pageOrder == PageOrder::DownThenOver;
    over_.checked = p.pageOrder == PageOrder::OverThenDown;
    v_.host_.ViewInvalidate();
  }

  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override {
    ui_ = ui;
    labels_.clear();
    const float left = body.left + 16 * ui, right = body.right - 16 * ui, mid = (left + right) / 2, gap = 4 * ui;
    const float rowH = 30 * ui;
    float y = body.top + 10 * ui;
    std::vector<Control*> out;
    auto label = [&](const std::wstring& text, float l = -1, float r = -1) {
      labels_.push_back({text, RectF(l < 0 ? left : l, y, r < 0 ? right : r, y + 16 * ui)});
    };
    auto full = [&](Control& c) {
      c.rc = RectF(left, y, right, y + rowH);
      c.visible = true;
      out.push_back(&c);
      y += rowH + 10 * ui;
    };
    auto pair = [&](Control& a, Control& b) {
      a.rc = RectF(left, y, mid - gap, y + rowH);
      b.rc = RectF(mid + gap, y, right, y + rowH);
      a.visible = b.visible = true;
      out.push_back(&a);
      out.push_back(&b);
      y += rowH + 10 * ui;
    };
    auto labelled = [&](const std::wstring& a, const std::wstring& b) {
      label(a, left, mid - gap);
      label(b, mid + gap, right);
      y += 18 * ui;
    };
    for (Control* c : All()) c->visible = false;
    if (v_.preview_) {
      label(L"Print");
      y += 18 * ui;
      full(what_);
    }
    const float tabW = (right - left) / static_cast<float>(kTabs);
    for (int i = 0; i < kTabs; ++i) {
      tabs_[i].rc = RectF(left + i * tabW, y, left + (i + 1) * tabW - 2 * ui, y + 28 * ui);
      tabs_[i].visible = true;
      out.push_back(&tabs_[i]);
    }
    y += 28 * ui + 14 * ui;
    const std::wstring unit = Metric() ? L" (cm)" : L" (in)";
    switch (tab_) {
      case kTabPage:
        label(L"Orientation");
        y += 18 * ui;
        pair(portrait_, landscape_);
        label(L"Paper size");
        y += 18 * ui;
        full(paper_);
        label(L"Scaling");
        y += 18 * ui;
        full(scaling_);
        labelled(L"Adjust to (%)", L"First page number");
        pair(scale_, first_);
        labelled(L"Fit to: pages wide", L"by pages tall");
        pair(fitW_, fitH_);
        break;
      case kTabMargins: {
        label(L"Margins");
        y += 18 * ui;
        full(preset_);
        const wchar_t* names[] = {L"Top", L"Bottom", L"Left", L"Right", L"Header", L"Footer"};
        for (int i = 0; i < 6; i += 2) {
          labelled(names[i] + unit, names[i + 1] + unit);
          pair(margin_[i], margin_[i + 1]);
        }
        label(L"Center on page");
        y += 18 * ui;
        pair(centerH_, centerV_);
        break;
      }
      case kTabHeader:
        for (int f = 0; f < 2; ++f) {
          label(f ? L"Footer" : L"Header");
          y += 18 * ui;
          full(hfPreset_[f]);
          const float w3 = (right - left - 2 * gap) / 3;
          for (int k = 0; k < 3; ++k) {
            section_[f][k].rc = RectF(left + k * (w3 + gap), y, left + k * (w3 + gap) + w3, y + rowH);
            section_[f][k].visible = true;
            out.push_back(&section_[f][k]);
          }
          y += rowH + 4 * ui;
          label(L"Left, center and right", left, right);
          y += 24 * ui;
        }
        full(oddEven_);
        full(firstDiff_);
        hintRc_ = RectF(left, y, right, y + 40 * ui);
        break;
      case kTabSheet:
        label(L"Print area");
        y += 18 * ui;
        full(area_);
        label(L"Rows to repeat at top");
        y += 18 * ui;
        full(rows_);
        label(L"Columns to repeat at left");
        y += 18 * ui;
        full(cols_);
        label(L"Print");
        y += 18 * ui;
        pair(gridlines_, headings_);
        label(L"Page order");
        y += 18 * ui;
        pair(down_, over_);
        break;
      default: break;
    }
    return out;
  }

  void Draw(Canvas& c) override {
    const Palette& p = theme::P();
    for (const auto& [text, rc] : labels_) c.Text(text, rc, p.textSecondary, 12 * ui_);
    if (tab_ == kTabHeader)
      c.TextWrapped(L"Codes: &[Page] &[Pages] &[Date] &[Time] &[File] &[Tab] &[Path]", hintRc_, p.textSecondary, 12 * ui_, 2);
  }

 private:
  void Show(Tab tab) {
    tab_ = tab;
    Refresh();
  }
  void Change(const std::function<void(PageSetup&, HeaderFooter&, bool&)>& f) {
    v_.ApplyPageSetup(f);
    Refresh();
  }
  void Toggle(PanelButton& b, const wchar_t* text, std::function<void()> fn) {
    b.text = text;
    b.toggle = true;
    b.textSize = 12;
    b.onInvoke = std::move(fn);
  }
  // A list to pick from: `fn` gets the item's index (typed text that isn't an item puts it back).
  void Choice(FocusComboBox& b, std::function<void(int)> fn) {
    b.autoComplete = true;
    b.onCommit = [this, fn = std::move(fn)](const std::wstring&, int i) {
      if (i >= 0) fn(i);
      else Refresh();
    };
  }
  void Number(FocusComboBox& b, std::function<bool(double)> fn) {
    b.autoComplete = false;
    b.validate = [](std::wstring& t) {
      double v;
      return ParseNumber(t, v);
    };
    b.onCommit = [this, fn = std::move(fn)](const std::wstring& t, int) {
      double v;
      if (!ParseNumber(t, v) || !fn(v)) Refresh();
    };
  }
  std::vector<Control*> All() {
    std::vector<Control*> all = {&what_,     &portrait_, &landscape_, &paper_,     &scaling_,   &scale_,   &fitW_,
                                 &fitH_,     &first_,    &preset_,    &centerH_,   &centerV_,   &oddEven_, &firstDiff_,
                                 &area_,     &rows_,     &cols_,      &gridlines_, &headings_,  &down_,    &over_};
    for (auto& b : margin_) all.push_back(&b);
    for (int f = 0; f < 2; ++f) {
      all.push_back(&hfPreset_[f]);
      for (auto& b : section_[f]) all.push_back(&b);
    }
    for (auto& t : tabs_) all.push_back(&t);
    return all;
  }

  WorkbookView& v_;
  Tab tab_ = kTabPage;
  PanelButton tabs_[kTabs];
  FocusComboBox what_, paper_, scaling_, scale_, fitW_, fitH_, first_, preset_, margin_[6], hfPreset_[2], section_[2][3],
      area_, rows_, cols_;
  PanelButton portrait_, landscape_, centerH_, centerV_, oddEven_, firstDiff_, gridlines_, headings_, down_, over_;
  std::vector<std::pair<std::wstring, D2D1_RECT_F>> labels_;
  D2D1_RECT_F hintRc_{};
  float ui_ = 1;
};

// ---------------------------------------------------------------------------
// Page setup through the edit layer

edit::Result WorkbookView::ApplyPageSetup(const std::function<void(PageSetup&, HeaderFooter&, bool&)>& change) {
  const Sheet& s = Current();
  PageSetup p = s.Page();
  HeaderFooter h = s.HeadersFooters();
  bool fit = s.Props().fitToPage;
  change(p, h, fit);
  edit::Result r = Ed().SetPageSetup(static_cast<int>(sheet_), p, h, fit);
  if (!EditRefused(r)) AfterPageChange();
  return r;
}

void WorkbookView::AfterPageChange() {
  breakPages_.reset();
  RefreshPreview();
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

void WorkbookView::RefreshPreview() {
  if (!preview_) return;
  const int page = currentPage_;
  Paginate();
  RecomputeScale();
  Relayout(nullptr);
  GoToPage(std::clamp(page, 0, std::max(0, static_cast<int>(pageSizes_.size()) - 1)));
}

edit::Result WorkbookView::SetPrintArea(const std::vector<Range>& areas) {
  edit::Result r;
  if (areas.empty()) {
    if (NameOf(*book_, "_xlnm.Print_Area", sheet_)) r = Ed().DeleteName("_xlnm.Print_Area", static_cast<int>(sheet_));
  } else {
    const std::string sheet = pkg::QuoteSheetName(Current().Name()) + "!";
    std::string formula;
    for (const Range& a : areas) formula += (formula.empty() ? "" : ",") + sheet + AbsRange(a);
    r = Ed().SetName(DefinedName{"_xlnm.Print_Area", static_cast<int>(sheet_), formula, false});
  }
  if (!EditRefused(r)) AfterPageChange();
  return r;
}

std::optional<std::pair<uint32_t, uint32_t>> WorkbookView::TitleRows() const {
  if (const DefinedName* n = NameOf(*book_, "_xlnm.Print_Titles", sheet_))
    for (const Range& r : PrintLayout::ParseAreas(n->formula))
      if (r.firstCol == 0 && r.lastCol == kMaxCols - 1) return std::make_pair(r.firstRow, r.lastRow);
  return std::nullopt;
}

std::optional<std::pair<uint32_t, uint32_t>> WorkbookView::TitleCols() const {
  if (const DefinedName* n = NameOf(*book_, "_xlnm.Print_Titles", sheet_))
    for (const Range& r : PrintLayout::ParseAreas(n->formula))
      if (r.firstRow == 0 && r.lastRow == kMaxRows - 1) return std::make_pair(r.firstCol, r.lastCol);
  return std::nullopt;
}

edit::Result WorkbookView::SetPrintTitles(std::optional<std::pair<uint32_t, uint32_t>> rows,
                                          std::optional<std::pair<uint32_t, uint32_t>> cols) {
  // As Excel writes them: the columns first ("Sheet1!$A:$A,Sheet1!$1:$2").
  const std::string sheet = pkg::QuoteSheetName(Current().Name()) + "!";
  std::string formula;
  if (cols) formula = sheet + AbsRange(Range{0, cols->first, kMaxRows - 1, cols->second});
  if (rows) formula += (formula.empty() ? "" : ",") + sheet + AbsRange(Range{rows->first, 0, rows->second, kMaxCols - 1});
  edit::Result r;
  if (formula.empty()) {
    if (NameOf(*book_, "_xlnm.Print_Titles", sheet_)) r = Ed().DeleteName("_xlnm.Print_Titles", static_cast<int>(sheet_));
  } else {
    r = Ed().SetName(DefinedName{"_xlnm.Print_Titles", static_cast<int>(sheet_), formula, false});
  }
  if (!EditRefused(r)) AfterPageChange();
  return r;
}

// The manual breaks at the active cell: the row break above it and the column break left of it.
bool WorkbookView::BreakHere(bool& row, bool& col) const {
  const Sheet& s = Current();
  row = col = false;
  for (const PageBreak& b : s.RowBreaks()) row = row || (b.manual && b.id == active_.row);
  for (const PageBreak& b : s.ColBreaks()) col = col || (b.manual && b.id == active_.col);
  return row || col;
}

edit::Result WorkbookView::ChangeBreaks(int cmd) {
  const Sheet& s = Current();
  std::vector<PageBreak> rows = s.RowBreaks(), cols = s.ColBreaks();
  const Range sel = Selected();
  const bool wholeRows = sel.firstCol == 0 && sel.lastCol == kMaxCols - 1;
  const bool wholeCols = sel.firstRow == 0 && sel.lastRow == kMaxRows - 1;
  const char16_t* name = u"Insert Page Break";
  if (cmd == CmdInsertPageBreak) {
    // Above and left of the active cell (only one way for whole rows or columns, as Excel).
    if (active_.row > 0 && !wholeCols && std::none_of(rows.begin(), rows.end(), [&](const PageBreak& b) { return b.id == active_.row; }))
      rows.push_back(PageBreak{active_.row, 0, kMaxCols - 1, true, false});
    if (active_.col > 0 && !wholeRows && std::none_of(cols.begin(), cols.end(), [&](const PageBreak& b) { return b.id == active_.col; }))
      cols.push_back(PageBreak{active_.col, 0, kMaxRows - 1, true, false});
  } else if (cmd == CmdRemovePageBreak) {
    name = u"Remove Page Break";
    if (!wholeCols)
      rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const PageBreak& b) { return b.manual && b.id == active_.row; }), rows.end());
    if (!wholeRows)
      cols.erase(std::remove_if(cols.begin(), cols.end(), [&](const PageBreak& b) { return b.manual && b.id == active_.col; }), cols.end());
  } else {
    name = u"Reset All Page Breaks";
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](const PageBreak& b) { return b.manual; }), rows.end());
    cols.erase(std::remove_if(cols.begin(), cols.end(), [](const PageBreak& b) { return b.manual; }), cols.end());
  }
  edit::EditGroup group(Ed(), name);
  edit::Result r = Ed().SetPageBreaks(static_cast<int>(sheet_), rows, cols);
  if (EditRefused(r)) return r;
  group.done = true;
  AfterPageChange();
  return r;
}

// ---------------------------------------------------------------------------
// Commands

bool WorkbookView::PageSetupCommandState(int cmd, CommandState& s) {
  if (cmd != CmdPageSetup && (cmd < CmdSetPrintArea || cmd > CmdPrintSelection)) return false;
  const bool sheet = Ready() && !ChartSheet();
  const bool grid = sheet && !preview_;
  bool rowBreak = false, colBreak = false;
  switch (cmd) {
    case CmdPageSetup: s.label = L"Page setup"; s.enabled = sheet; break;
    case CmdSetPrintArea: s.label = L"Set print area"; s.enabled = grid; break;
    case CmdAddToPrintArea:
      s.label = L"Add to print area";
      s.enabled = grid && NameOf(*book_, "_xlnm.Print_Area", sheet_);
      break;
    case CmdClearPrintArea:
      s.label = L"Clear print area";
      s.enabled = sheet && NameOf(*book_, "_xlnm.Print_Area", sheet_);
      break;
    case CmdPrintTitles: s.label = L"Print titles"; s.enabled = sheet; break;
    case CmdInsertPageBreak:
      s.label = L"Insert page break";
      s.enabled = grid && (active_.row > 0 || active_.col > 0);
      break;
    case CmdRemovePageBreak:
      s.label = L"Remove page break";
      s.enabled = grid && BreakHere(rowBreak, colBreak);
      break;
    case CmdResetPageBreaks: {
      bool any = false;
      if (sheet)
        for (const auto* list : {&Current().RowBreaks(), &Current().ColBreaks()})
          for (const PageBreak& b : *list) any = any || b.manual;
      s.label = L"Reset all page breaks";
      s.enabled = any;
      break;
    }
    case CmdPageBreakPreview:
      s.label = L"Page break preview";
      s.enabled = grid;
      s.checked = sheet && BreakPreviewOn();
      break;
    case CmdNoScaling:
    case CmdFitSheetOnePage:
    case CmdFitColumnsOnePage:
    case CmdFitRowsOnePage: {
      s.label = kScalings[cmd - CmdNoScaling];
      s.enabled = sheet;
      break;
    }
    case CmdPrintActiveSheet:
    case CmdPrintWorkbook:
    case CmdPrintSelection: {
      const wchar_t* labels[] = {L"Print active sheet", L"Print entire workbook", L"Print selection"};
      s.label = labels[cmd - CmdPrintActiveSheet];
      s.enabled = Ready();
      s.checked = printWhat_ == cmd - CmdPrintActiveSheet;
      break;
    }
    default: return false;
  }
  return true;
}

bool WorkbookView::PageSetupCommand(int cmd) {
  CommandState state;
  if (!PageSetupCommandState(cmd, state)) return false;
  if (!state.enabled) return true;
  auto open = [&](PageSetupPanel::Tab tab) {
    if (!pageSetupPanel_) pageSetupPanel_ = std::make_unique<PageSetupPanel>(*this);
    auto* panel = static_cast<PageSetupPanel*>(pageSetupPanel_.get());
    panel->Open(tab);
    host_.ViewShowPanel(panel);
  };
  switch (cmd) {
    case CmdPageSetup: open(PageSetupPanel::kTabPage); break;
    case CmdPrintTitles: open(PageSetupPanel::kTabSheet); break;
    case CmdSetPrintArea: SetPrintArea({Selected()}); break;
    case CmdAddToPrintArea: {
      std::vector<Range> areas;
      if (const DefinedName* n = NameOf(*book_, "_xlnm.Print_Area", sheet_)) areas = PrintLayout::ParseAreas(n->formula);
      areas.push_back(Selected());
      SetPrintArea(areas);
      break;
    }
    case CmdClearPrintArea: SetPrintArea({}); break;
    case CmdInsertPageBreak:
    case CmdRemovePageBreak:
    case CmdResetPageBreaks: ChangeBreaks(cmd); break;
    case CmdPageBreakPreview: TogglePageBreakPreview(); break;
    case CmdNoScaling:
    case CmdFitSheetOnePage:
    case CmdFitColumnsOnePage:
    case CmdFitRowsOnePage:
      ApplyPageSetup([cmd](PageSetup& p, HeaderFooter&, bool& fit) {
        fit = cmd != CmdNoScaling;
        if (!fit) p.scale = 100;
        else p.fitToWidth = cmd == CmdFitRowsOnePage ? 0 : 1, p.fitToHeight = cmd == CmdFitColumnsOnePage ? 0 : 1;
      });
      break;
    case CmdPrintActiveSheet:
    case CmdPrintWorkbook:
    case CmdPrintSelection:
      printWhat_ = cmd - CmdPrintActiveSheet;
      RefreshPreview();
      host_.ViewStateChanged();
      host_.ViewInvalidate();
      break;
    default: break;
  }
  if (pageSetupPanel_ && host_.ViewShownPanel() == pageSetupPanel_.get() && cmd != CmdPageSetup && cmd != CmdPrintTitles)
    static_cast<PageSetupPanel*>(pageSetupPanel_.get())->Refresh();
  return true;
}

void WorkbookView::AddPageSetupTabItems(HMENU m) {
  for (int cmd : {CmdPageSetup, CmdPageBreakPreview}) {
    CommandState s;
    PageSetupCommandState(cmd, s);
    menu::AppendItem(m, static_cast<UINT>(cmd), s.label + (cmd == CmdPageSetup ? L"…" : L""), s.enabled, s.checked);
  }
}

// ---------------------------------------------------------------------------
// Page Break Preview: the sheet's pages over its cells (what doesn't print greyed, "Page N" on
// each, the breaks as blue lines: dashed where Excel put them, solid where the user did), and the
// breaks dragged to new rows or columns (a dragged one becomes manual, as in Excel).

bool WorkbookView::BreakPreviewOn() const {
  if (!Ready() || ChartSheet()) return false;
  const auto& views = Current().Views();
  return !views.empty() && views[0].view == ViewMode::PageBreakPreview;
}

void WorkbookView::TogglePageBreakPreview() {
  if (!Ready() || ChartSheet()) return;
  Ed().SetViewMode(static_cast<int>(sheet_), BreakPreviewOn() ? ViewMode::Normal : ViewMode::PageBreakPreview);
  breakPages_.reset();
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

const PrintLayout* WorkbookView::BreakPages() {
  if (!breakPages_ || breakPages_->first != sheet_) {
    breakPages_ = std::make_unique<PrintSheet>();
    breakPages_->first = sheet_;
    breakPages_->objects = SheetObjects::Load(*book_, sheet_);
    PrintInfo info = InfoFor(Current());
    info.objects = breakPages_->objects.get();
    info.rules = RulesFor(sheet_);
    breakPages_->layout = std::make_unique<PrintLayout>(*book_, Current(), sheet_, grid_, *text_, info);
  }
  return breakPages_->layout.get();
}

namespace {
bool Inner(const PrintLayout& pages, bool rows, uint32_t at) {
  for (size_t i = 0; i < pages.PageCount(); ++i) {
    const PrintPage& p = pages.Page(i);
    if (!p.notes && !p.chart && (rows ? p.body.firstRow : p.body.firstCol) == at) return true;
  }
  return false;
}
bool Manual(const Sheet& s, bool rows, uint32_t at) {
  for (const PageBreak& b : rows ? s.RowBreaks() : s.ColBreaks())
    if (b.manual && b.id == at) return true;
  return false;
}
}  // namespace

void WorkbookView::DrawPageBreakPreview(Canvas& c) {
  if (preview_ || !BreakPreviewOn()) return;
  const PrintLayout* pages = BreakPages();
  if (!pages) return;
  const Sheet& s = Current();
  const D2D1_COLOR_F blue = Rgb(0x2E5CD6);
  for (const Pane& p : Panes()) {
    const D2D1_RECT_F pane = RectF(p.rc);
    c.PushClip(pane);
    // Grey where nothing prints: the pane less the pages (alternate fill of the pane and each page).
    std::vector<ID2D1Geometry*> parts;
    std::vector<Microsoft::WRL::ComPtr<ID2D1RectangleGeometry>> keep;
    auto add = [&](const D2D1_RECT_F& r) {
      Microsoft::WRL::ComPtr<ID2D1RectangleGeometry> g;
      if (SUCCEEDED(gfx::D2D()->CreateRectangleGeometry(r, &g))) {
        parts.push_back(g.Get());
        keep.push_back(g);
      }
    };
    add(pane);
    std::vector<std::pair<D2D1_RECT_F, const PrintPage*>> shown;
    for (size_t i = 0; i < pages->PageCount(); ++i) {
      const PrintPage& page = pages->Page(i);
      if (page.notes || page.chart) continue;
      D2D1_RECT_F r = CellRect(p, page.body.firstRow, page.body.firstCol, page.body.lastRow, page.body.lastCol);
      if (r.left > r.right) std::swap(r.left, r.right);  // right to left
      if (r.right < pane.left || r.left > pane.right || r.bottom < pane.top || r.top > pane.bottom) continue;
      add(r);
      shown.push_back({r, &page});
    }
    Microsoft::WRL::ComPtr<ID2D1GeometryGroup> group;
    if (SUCCEEDED(gfx::D2D()->CreateGeometryGroup(D2D1_FILL_MODE_ALTERNATE, parts.data(), static_cast<UINT32>(parts.size()), &group)))
      c.DC()->FillGeometry(group.Get(), c.Brush(Rgb(0x808080, 0.35f)));
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> dashed;
    const D2D1_STROKE_STYLE_PROPERTIES dash = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                         D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_DASH);
    gfx::D2D()->CreateStrokeStyle(dash, nullptr, 0, &dashed);
    const float w = std::max(2.f, std::round(2 * static_cast<float>(ui_)));
    for (const auto& [r, page] : shown) {
      // "Page N", large and pale, in the page's middle.
      const float size = std::clamp(std::min(Width(r), Height(r)) / 6, 12 * static_cast<float>(ui_), 96 * static_cast<float>(ui_));
      const float cy = (r.top + r.bottom) / 2;
      c.Text(L"Page " + std::to_wstring(page->number), RectF(r.left, cy - size * 0.7f, r.right, cy + size * 0.7f),
             Rgb(0x7F7F7F, 0.45f), size, DWRITE_FONT_WEIGHT_BOLD, Align::Center);
      // Its edges: a break between pages dashed (an automatic one) or solid (a manual one).
      auto edge = [&](float x1, float y1, float x2, float y2, bool rows, uint32_t at) {
        const bool inner = Inner(*pages, rows, at) && at > 0;
        const bool solid = !inner || Manual(s, rows, at);
        c.DC()->DrawLine({x1, y1}, {x2, y2}, c.Brush(blue), w, solid ? nullptr : dashed.Get());
      };
      edge(r.left, r.top, r.right, r.top, true, page->body.firstRow);
      edge(r.left, r.bottom, r.right, r.bottom, true, page->body.lastRow + 1);
      edge(rtl_ ? r.right : r.left, r.top, rtl_ ? r.right : r.left, r.bottom, false, page->body.firstCol);
      edge(rtl_ ? r.left : r.right, r.top, rtl_ ? r.left : r.right, r.bottom, false, page->body.lastCol + 1);
    }
    // A break being dragged: where it would go.
    if (breakDrag_ && breakDrag_->to != breakDrag_->from) {
      const float at = breakDrag_->rows ? PaneY(p, breakDrag_->to) : Mirror(PaneX(p, breakDrag_->to));
      if (breakDrag_->rows) c.Line(pane.left, at, pane.right, at, Alpha(blue, 0.6f), 2 * w);
      else c.Line(at, pane.top, at, pane.bottom, Alpha(blue, 0.6f), 2 * w);
    }
    c.PopClip();
  }
}

// A break between two pages under (x, y): which way it runs and the row or column it is before.
bool WorkbookView::BreakEdgeAt(float x, float y, bool& rows, uint32_t& at) {
  if (preview_ || !BreakPreviewOn() || !gfx::Contains(cells_, x, y)) return false;
  const PrintLayout* pages = BreakPages();
  if (!pages) return false;
  const float slop = 4 * static_cast<float>(ui_);
  for (const Pane& p : Panes()) {
    if (!gfx::Contains(RectF(p.rc), x, y)) continue;
    for (size_t i = 0; i < pages->PageCount(); ++i) {
      const PrintPage& page = pages->Page(i);
      if (page.notes || page.chart) continue;
      D2D1_RECT_F r = CellRect(p, page.body.firstRow, page.body.firstCol, page.body.lastRow, page.body.lastCol);
      if (r.left > r.right) std::swap(r.left, r.right);
      if (x >= r.left - slop && x <= r.right + slop) {
        if (std::abs(y - r.bottom) <= slop && Inner(*pages, true, page.body.lastRow + 1)) {
          rows = true, at = page.body.lastRow + 1;
          return true;
        }
      }
      if (y >= r.top - slop && y <= r.bottom + slop) {
        const float edge = rtl_ ? r.left : r.right;
        if (std::abs(x - edge) <= slop && Inner(*pages, false, page.body.lastCol + 1)) {
          rows = false, at = page.body.lastCol + 1;
          return true;
        }
      }
    }
  }
  return false;
}

bool WorkbookView::BreakDragStart(float x, float y) {
  bool rows = false;
  uint32_t at = 0;
  if (!BreakEdgeAt(x, y, rows, at)) return false;
  breakDrag_ = BreakDrag{rows, at, at};
  return true;
}

bool WorkbookView::BreakDragMove(int x, int y) {
  if (!breakDrag_) return false;
  // The row or column boundary nearest the mouse.
  if (auto cell = CellAt(x, y)) {
    uint32_t to = breakDrag_->rows ? cell->row : cell->col;
    for (const Pane& p : Panes()) {
      if (!gfx::Contains(RectF(p.rc), static_cast<float>(x), static_cast<float>(y))) continue;
      const D2D1_RECT_F r = CellRect(p, cell->row, cell->col, cell->row, cell->col);
      const bool beyond = breakDrag_->rows ? y - r.top > r.bottom - y : (std::abs(x - (rtl_ ? r.right : r.left)) > std::abs(x - (rtl_ ? r.left : r.right)));
      if (beyond) ++to;
    }
    to = std::max<uint32_t>(1, to);
    if (to != breakDrag_->to) {
      breakDrag_->to = to;
      host_.ViewInvalidate();
    }
  }
  return true;
}

bool WorkbookView::BreakDragEnd(int, int) {
  if (!breakDrag_) return false;
  const BreakDrag d = *breakDrag_;
  breakDrag_.reset();
  host_.ViewInvalidate();
  if (d.to == d.from) return true;
  const Sheet& s = Current();
  std::vector<PageBreak> rows = s.RowBreaks(), cols = s.ColBreaks();
  std::vector<PageBreak>& list = d.rows ? rows : cols;
  list.erase(std::remove_if(list.begin(), list.end(), [&](const PageBreak& b) { return b.id == d.from || b.id == d.to; }),
             list.end());
  list.push_back(PageBreak{d.to, 0, d.rows ? kMaxCols - 1 : kMaxRows - 1, true, false});
  edit::EditGroup group(Ed(), u"Move Page Break");
  if (EditRefused(Ed().SetPageBreaks(static_cast<int>(sheet_), rows, cols))) return true;
  group.done = true;
  breakPages_.reset();
  AfterPageChange();
  return true;
}

HCURSOR WorkbookView::BreakCursor(float x, float y) {
  bool rows = false;
  uint32_t at = 0;
  if (breakDrag_) return LoadCursorW(nullptr, breakDrag_->rows ? IDC_SIZENS : IDC_SIZEWE);
  if (BreakEdgeAt(x, y, rows, at)) return LoadCursorW(nullptr, rows ? IDC_SIZENS : IDC_SIZEWE);
  return nullptr;
}

// ---------------------------------------------------------------------------
// Scripted page setup (build from scratch, snapshot runs): pagesetup:<key>=<value>, through the same
// calls as the panel and the commands. Keys: orientation (portrait, landscape), paper (a paperSize
// code), scale (%), fit (W,H in pages, 0 any; off), margins (normal, wide, narrow, or top,bottom,
// left,right,header,footer in inches), center (h, v, hv, off), header and footer (Excel's codes,
// "&CPage &P"), area (A1:H100, several with commas; clear), rows and cols (the titles: 1:2, A:B;
// clear), gridlines and headings (1, 0), order (down, over), first (a number; auto), break
// (insert, remove, reset at the active cell), breakpreview (on, off), print (sheet, workbook,
// selection).

bool WorkbookView::PageScriptOp(const std::wstring& arg) {
  const size_t eq = arg.find(L'=');
  if (eq == std::wstring::npos) return false;
  const std::wstring key = arg.substr(0, eq), value = arg.substr(eq + 1);
  auto numbers = [&](std::vector<double>& out) {
    out.clear();
    std::wstring rest = value;
    while (!rest.empty()) {
      const size_t comma = rest.find(L',');
      double v;
      if (!ParseNumber(rest.substr(0, comma), v)) return false;
      out.push_back(v);
      rest = comma == std::wstring::npos ? std::wstring() : rest.substr(comma + 1);
    }
    return true;
  };
  auto set = [&](const std::function<void(PageSetup&, HeaderFooter&, bool&)>& f) { return static_cast<bool>(ApplyPageSetup(f)); };
  std::vector<double> v;
  if (key == L"orientation")
    return set([&](PageSetup& p, HeaderFooter&, bool&) { p.orientation = value == L"landscape" ? Orientation::Landscape : Orientation::Portrait; });
  if (key == L"paper" && numbers(v) && v.size() == 1)
    return set([&](PageSetup& p, HeaderFooter&, bool&) { p.paperSize = static_cast<uint32_t>(v[0]); });
  if (key == L"scale" && numbers(v) && v.size() == 1)
    return set([&](PageSetup& p, HeaderFooter&, bool& fit) {
      p.scale = static_cast<uint32_t>(v[0]);
      fit = false;
    });
  if (key == L"fit") {
    if (value == L"off") return set([](PageSetup&, HeaderFooter&, bool& fit) { fit = false; });
    if (!numbers(v) || v.size() != 2) return false;
    return set([&](PageSetup& p, HeaderFooter&, bool& fit) {
      p.fitToWidth = static_cast<uint32_t>(v[0]);
      p.fitToHeight = static_cast<uint32_t>(v[1]);
      fit = true;
    });
  }
  if (key == L"margins") {
    for (const MarginPreset& m : kMargins)
      if (_wcsicmp(value.c_str(), m.name) == 0)
        return set([&](PageSetup& p, HeaderFooter&, bool&) { p.margins = {m.left, m.right, m.top, m.bottom, m.header, m.footer}; });
    if (!numbers(v) || v.size() != 6) return false;
    return set([&](PageSetup& p, HeaderFooter&, bool&) { p.margins = {v[2], v[3], v[0], v[1], v[4], v[5]}; });
  }
  if (key == L"center")
    return set([&](PageSetup& p, HeaderFooter&, bool&) {
      p.horizontalCentered = value.find(L'h') != std::wstring::npos;
      p.verticalCentered = value.find(L'v') != std::wstring::npos;
    });
  if (key == L"header" || key == L"footer")
    return set([&](PageSetup&, HeaderFooter& h, bool&) { (key == L"header" ? h.oddHeader : h.oddFooter) = WideToUtf8(value); });
  if (key == L"gridlines" || key == L"headings")
    return set([&](PageSetup& p, HeaderFooter&, bool&) { (key == L"gridlines" ? p.gridLines : p.headings) = value == L"1"; });
  if (key == L"order")
    return set([&](PageSetup& p, HeaderFooter&, bool&) { p.pageOrder = value == L"over" ? PageOrder::OverThenDown : PageOrder::DownThenOver; });
  if (key == L"first")
    return set([&](PageSetup& p, HeaderFooter&, bool&) {
      double n = 0;
      p.useFirstPageNumber = ParseNumber(value, n);
      if (p.useFirstPageNumber) p.firstPageNumber = static_cast<int>(n);
    });
  if (key == L"area") {
    std::vector<Range> areas;
    if (value != L"clear" && (!ParseTyped(value, areas) || areas.empty())) return false;
    return static_cast<bool>(SetPrintArea(areas));
  }
  if (key == L"rows" || key == L"cols") {
    std::vector<Range> areas;
    if (value != L"clear" && (!ParseTyped(value, areas) || areas.size() != 1)) return false;
    auto rows = TitleRows(), cols = TitleCols();
    auto& which = key == L"rows" ? rows : cols;
    which.reset();
    if (!areas.empty())
      which = key == L"rows" ? std::make_pair(areas[0].firstRow, areas[0].lastRow) : std::make_pair(areas[0].firstCol, areas[0].lastCol);
    return static_cast<bool>(SetPrintTitles(rows, cols));
  }
  if (key == L"break") {
    const int cmd = value == L"insert" ? CmdInsertPageBreak : value == L"remove" ? CmdRemovePageBreak : CmdResetPageBreaks;
    return static_cast<bool>(ChangeBreaks(cmd));
  }
  if (key == L"breakpreview") {
    if ((value == L"on") != BreakPreviewOn()) TogglePageBreakPreview();
    return true;
  }
  if (key == L"print") {
    const int cmd = value == L"workbook" ? CmdPrintWorkbook : value == L"selection" ? CmdPrintSelection : CmdPrintActiveSheet;
    return PageSetupCommand(cmd);
  }
  return false;
}

void WorkbookView::AddPageSetupCellItems(HMENU m) {
  for (int cmd : {CmdInsertPageBreak, CmdRemovePageBreak, CmdSetPrintArea}) {
    CommandState s;
    PageSetupCommandState(cmd, s);
    if (!s.enabled) continue;
    menu::AppendItem(m, static_cast<UINT>(cmd), s.label, true);
  }
}

}  // namespace fw::sheet::view
