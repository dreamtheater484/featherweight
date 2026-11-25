#include "word/DocTable.h"

#include <algorithm>
#include <cmath>

#include "Commands.h"
#include "core/Lang.h"
#include "core/Strings.h"
#include "doc/edit/TableStyles.h"
#include "ui/Anim.h"
#include "ui/Controls.h"
#include "ui/Gfx.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/Theme.h"

using namespace gfx;
namespace edit = fw::doc::edit;
using edit::Editor;
using fw::doc::Pos;

namespace {

struct Named {
  int cmd;
  UiText label;
};

// Word's names, for menus, tooltips and command search.
constexpr Named kNames[] = {
    {CmdTableGrid, N_(L"Insert table")},
    {CmdTableBox, N_(L"Table")},
    {CmdInsertTable, N_(L"Insert table…")},
    {CmdTextToTable, N_(L"Convert text to table")},
    {CmdTableToText, N_(L"Convert table to text")},
    {CmdTableInsertAbove, N_(L"Insert rows above")},
    {CmdTableInsertBelow, N_(L"Insert rows below")},
    {CmdTableInsertLeft, N_(L"Insert columns to the left")},
    {CmdTableInsertRight, N_(L"Insert columns to the right")},
    {CmdTableDeleteRows, N_(L"Delete rows")},
    {CmdTableDeleteColumns, N_(L"Delete columns")},
    {CmdTableDeleteTable, N_(L"Delete table")},
    {CmdTableMerge, N_(L"Merge cells")},
    {CmdTableSplit, N_(L"Split cells…")},
    {CmdTableSelectCell, N_(L"Select cell")},
    {CmdTableSelectRow, N_(L"Select row")},
    {CmdTableSelectColumn, N_(L"Select column")},
    {CmdTableSelectTable, N_(L"Select table")},
    {CmdTableAutoFitContents, N_(L"AutoFit to contents")},
    {CmdTableAutoFitWindow, N_(L"AutoFit to window")},
    {CmdTableFixedWidth, N_(L"Fixed column width")},
    {CmdTableDistributeColumns, N_(L"Distribute columns")},
    {CmdTableHeaderRow, N_(L"Repeat as header row")},
    {CmdTableProperties, N_(L"Table properties…")},
};

const wchar_t* LabelOf(int cmd) {
  for (const Named& n : kNames)
    if (n.cmd == cmd) return n.label;
  return L"";
}


std::wstring W(std::string_view s) { return fw::Utf8ToWide(s); }

// Lengths as the user's region measures them: inches (US) or centimeters.
bool Metric() {
  wchar_t v[4] = L"";
  return GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IMEASURE, v, 4) > 0 && v[0] == L'0';
}
double FromTwips(int32_t twips) { return Metric() ? twips / 566.929 : twips / 1440.0; }
int32_t ToTwips(double v) { return static_cast<int32_t>(std::lround(Metric() ? v * 566.929 : v * 1440.0)); }
const wchar_t* Unit() { return Metric() ? L" cm" : L"\""; }

// Lists offered in the panel.
const std::vector<std::wstring> kAlign = {N_(L"Left"), N_(L"Center"), N_(L"Right")};
const std::vector<std::wstring> kVAlign = {N_(L"Top"), N_(L"Center"), N_(L"Bottom")};
const std::vector<std::wstring> kFit = {N_(L"Fixed column width"), N_(L"AutoFit to contents"), N_(L"AutoFit to window")};
const std::vector<std::wstring> kHeader = {N_(L"Doesn't repeat"), N_(L"Repeats at the top of each page")};
const std::vector<std::wstring> kBorders = {N_(L"All borders"),  N_(L"Outside borders"), N_(L"Inside borders"), N_(L"No border"),
                                            N_(L"Top border"),   N_(L"Bottom border"),   N_(L"Left border"),    N_(L"Right border")};
const std::vector<std::wstring> kSplit = {N_(L"2 columns"), N_(L"3 columns"), N_(L"2 rows"), N_(L"2 rows, 2 columns")};

constexpr float kLabelW = 104, kRowH = 40;

}  // namespace

// ---------------------------------------------------------------------------
// The Insert table grid: a toolbar button whose popup is a grid of cells; the pointer or the
// arrows pick a size, a click or Enter inserts it.

class DocTable::GridPicker : public Control {
 public:
  static constexpr int kCols = 10, kRows = 8;
  std::function<void(int rows, int cols)> onPick;

  void Draw(Canvas& c) override {
    const Palette& p = theme::P();
    const float u = Ui();
    ctl::DrawSubtle(c, rc, u, enabled ? hover_.V() : 0, enabled ? std::max(press_.V(), popup_.open ? 0.7f : 0.f) : 0);
    const D2D1_COLOR_F fg = enabled ? p.text : p.textDisabled;
    if (rc.bottom - rc.top > 48 * u) {  // the ribbon's large button: the icon above "Table ▾"
      c.Icon(icon::Table, RectF(rc.left, rc.top + 4 * u, rc.right, rc.top + 40 * u), fg, 32 * u);
      c.Text(T(L"Table"), RectF(rc.left, rc.top + 42 * u, rc.right, rc.top + 60 * u), fg, 12 * u,
             DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
      ctl::DrawChevron(c, RectF(rc.left, rc.top + 58 * u, rc.right, rc.top + 72 * u), enabled ? p.textSecondary : p.textDisabled, u);
      if (FocusVisible()) ctl::DrawFocusRing(c, rc, u);
      return;
    }
    const float chevW = 16 * u;
    c.Icon(icon::Table, RectF(rc.left, rc.top, rc.right - chevW, rc.bottom), fg, 16 * u);
    ctl::DrawChevron(c, RectF(rc.right - chevW - 2 * u, rc.top, rc.right - 2 * u, rc.bottom), enabled ? p.textSecondary : p.textDisabled, u);
    if (FocusVisible()) ctl::DrawFocusRing(c, rc, u);
  }
  void DrawPopup(Canvas& c) override {
    if (!popup_.Visible()) return;
    const Palette& p = theme::P();
    const float u = Ui();
    popup_.Begin(c, u);
    const D2D1_RECT_F in = popup_.Inner(u);
    const std::wstring title =
        rows_ > 0 ? Fmt(T(L"{0} × {1} table"), {std::to_wstring(cols_), std::to_wstring(rows_)}) : T(L"Insert table");
    c.Text(title, RectF(in.left + 8 * u, in.top, in.right - 8 * u, in.top + 28 * u), p.text, 13 * u);
    for (int r = 0; r < kRows; ++r)
      for (int k = 0; k < kCols; ++k) {
        const D2D1_RECT_F cell = CellRect(r, k);
        const bool on = r < rows_ && k < cols_;
        c.Fill(cell, on ? Alpha(p.accent, 0.25f) : p.inputBg);
        c.Stroke(cell, on ? p.accent : p.inputBorder, 1);
      }
    popup_.End(c);
  }
  bool IsOpen() const override { return popup_.open; }
  void SetOpen(bool open) override {
    if (open == popup_.open) return;
    if (!open) {
      popup_.Close();
      rows_ = cols_ = 0;
      return;
    }
    if (!enabled) return;
    Opened();
    const float u = Ui();
    popup_.Place(rc, (kCols * kCell + 16) * u + 8 * u, (kRows * kCell + 28 + 12) * u + 8 * u, PopupBounds(), u);
    popup_.Open();
  }
  bool HitPopup(float x, float y) const override { return popup_.Hit(x, y); }
  bool OnMouseMove(float x, float y) override {
    const bool over = Hit(x, y);
    if (over != (hover_.Target() > 0.5f)) hover_.To(over ? 1.f : 0.f, 120);
    if (!popup_.open || !popup_.Hit(x, y)) return over;
    int r = -1, k = -1;
    CellAt(x, y, r, k);
    if (r >= 0 && (r + 1 != rows_ || k + 1 != cols_)) {
      rows_ = r + 1;
      cols_ = k + 1;
      return true;
    }
    return false;
  }
  void OnMouseLeave() override { hover_.To(0, 120); }
  void OnMouseDown(float x, float y, bool, bool) override {
    if (popup_.open && popup_.Hit(x, y)) return;  // the click picks on release
    press_.To(1, 40);
    TakeFocus();
    SetOpen(!popup_.open);
  }
  void OnMouseUp(float x, float y) override {
    press_.To(0, 120);
    if (!popup_.open || !popup_.Hit(x, y)) return;
    int r = -1, k = -1;
    CellAt(x, y, r, k);
    if (r >= 0) Pick(r + 1, k + 1);
  }
  bool OnKeyDown(UINT vk, bool, bool, bool alt) override {
    if (!enabled) return false;
    if (!popup_.open) {
      if (vk == VK_RETURN || vk == VK_SPACE || vk == VK_DOWN || (alt && vk == VK_DOWN)) {
        SetOpen(true);
        rows_ = cols_ = 1;
        return true;
      }
      return false;
    }
    switch (vk) {
      case VK_ESCAPE: SetOpen(false); return true;
      case VK_TAB: SetOpen(false); return false;
      case VK_LEFT: cols_ = std::max(1, cols_ - 1); break;
      case VK_RIGHT: cols_ = std::min(kCols, cols_ + 1); break;
      case VK_UP: rows_ = std::max(1, rows_ - 1); break;
      case VK_DOWN: rows_ = std::min(kRows, rows_ + 1); break;
      case VK_RETURN:
      case VK_SPACE:
        Pick(std::max(1, rows_), std::max(1, cols_));
        return false;  // picked: a toolbar hands the keyboard back
      default: return true;
    }
    rows_ = std::max(rows_, 1);
    cols_ = std::max(cols_, 1);
    if (group_) group_->Invalidate();
    return true;
  }
  void OnFocus(bool focused) override {
    if (!focused) SetOpen(false);
  }
  void Describe(uia::Tree& t, int parent) override {
    int i = t.Add(parent, id, UIA_ButtonControlTypeId, Name(), rc);
    uia::Node& n = t[i];
    n.enabled = enabled;
    n.focusable = true;
    n.focus = [this] { TakeFocus(); };
    n.focused = Focused() && !popup_.open;
    n.expand = popup_.open ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
    n.doExpand = [this](bool e) {
      TakeFocus();
      SetOpen(e);
    };
    if (popup_.open) {
      int pane = t.Add(i, 1, UIA_PaneControlTypeId, T(L"Table size"), popup_.rc);
      t[pane].layer = 2;
      t[pane].hasValue = true;
      t[pane].value = Fmt(T(L"{0} by {1}"), {std::to_wstring(cols_), std::to_wstring(rows_)});
    }
  }

 private:
  static constexpr float kCell = 20;
  D2D1_RECT_F CellRect(int r, int k) const {
    const float u = Ui();
    const D2D1_RECT_F in = popup_.Inner(u);
    const float x = in.left + 8 * u + k * kCell * u, y = in.top + 28 * u + r * kCell * u;
    return RectF(std::round(x), std::round(y), std::round(x + (kCell - 3) * u), std::round(y + (kCell - 3) * u));
  }
  void CellAt(float x, float y, int& r, int& k) const {
    const float u = Ui();
    const D2D1_RECT_F in = popup_.Inner(u);
    const int kk = static_cast<int>((x - in.left - 8 * u) / (kCell * u)), rr = static_cast<int>((y - in.top - 28 * u) / (kCell * u));
    if (x < in.left + 8 * u || y < in.top + 28 * u || kk >= kCols || rr >= kRows) return;
    r = rr;
    k = kk;
  }
  void Pick(int rows, int cols) {
    SetOpen(false);
    if (onPick) onPick(rows, cols);
  }

  Popup popup_;
  Anim hover_, press_;
  int rows_ = 0, cols_ = 0;
};

namespace {

// A plain button in a panel.
class PushButton : public Control {
 public:
  std::function<void()> onClick;
  void Draw(Canvas& c) override {
    const Palette& p = theme::P();
    const float u = Ui();
    ctl::DrawBox(c, rc, u, enabled ? hover_.V() : 0, enabled ? press_.V() : 0);
    c.Text(label, rc, enabled ? p.text : p.textDisabled, 13 * u, DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
    if (FocusVisible()) ctl::DrawFocusRing(c, rc, u);
  }
  bool OnMouseMove(float x, float y) override {
    const bool over = Hit(x, y);
    if (over == (hover_.Target() > 0.5f)) return false;
    hover_.To(over ? 1.f : 0.f, 120);
    return true;
  }
  void OnMouseLeave() override { hover_.To(0, 120); }
  void OnMouseDown(float, float, bool, bool) override {
    press_.To(1, 40);
    TakeFocus();
  }
  void OnMouseUp(float x, float y) override {
    press_.To(0, 120);
    if (enabled && Hit(x, y) && onClick) onClick();
  }
  bool OnKeyDown(UINT vk, bool, bool, bool) override {
    if (!enabled || (vk != VK_RETURN && vk != VK_SPACE)) return false;
    if (onClick) onClick();
    return true;
  }
  void Describe(uia::Tree& t, int parent) override {
    int i = t.Add(parent, id, UIA_ButtonControlTypeId, Name(), rc);
    uia::Node& n = t[i];
    n.enabled = enabled;
    n.focusable = true;
    n.focused = Focused();
    n.focus = [this] { TakeFocus(); };
    n.invoke = [this] {
      if (enabled && onClick) onClick();
    };
  }

 private:
  Anim hover_, press_;
};

}  // namespace

// ---------------------------------------------------------------------------
// The Table panel

class DocTable::Panel : public PanelContent {
 public:
  explicit Panel(DocTable& owner) : t_(owner) {
    auto spin = [](SpinBox& s, const wchar_t* name, double lo, double hi, double step, int decimals, const wchar_t* unit) {
      s.label = s.tooltip = name;
      s.minValue = lo;
      s.maxValue = hi;
      s.step = step;
      s.decimals = decimals;
      s.unit = unit;
    };
    auto drop = [](Dropdown& d, const wchar_t* name, const std::vector<std::wstring>& items) {
      d.label = d.tooltip = name;
      d.items = items;
    };
    drop(style_, T(L"Table style"), {});
    style_.onChange = [this](int i) {
      if (i >= 0 && i < static_cast<int>(styles_.size())) t_.ApplyStyle(styles_[i]);
    };
    drop(align_, T(L"Table alignment"), kAlign);
    align_.onChange = [this](int i) {
      if (auto c = t_.Cell()) t_.Keep(t_.editor_.SetTableProps(c->start, edit::PropEdit().Set("jc", {{"val", i == 1 ? "center" : i == 2 ? "right" : "left"}})));
    };
    drop(fit_, L"AutoFit", kFit);
    fit_.onChange = [this](int i) { t_.Execute(i == 0 ? CmdTableFixedWidth : i == 1 ? CmdTableAutoFitContents : CmdTableAutoFitWindow); };
    drop(header_, T(L"Header row"), kHeader);
    header_.onChange = [this](int i) {
      auto [a, f] = t_.host_.selection();
      edit::PropEdit e;
      if (i == 1) e.Set("tblHeader");
      else e.Remove("tblHeader");
      t_.Keep(t_.editor_.SetRowProps(a, f, e));
    };
    spin(width_, T(L"Column width"), 0.1, 22, 0.1, 2, Unit());
    width_.onChange = [this](double v) {
      auto [a, f] = t_.host_.selection();
      t_.Keep(t_.editor_.SetColumnWidth(a, f, ToTwips(v)));
    };
    spin(height_, T(L"Row height (0: as the text needs)"), 0, 22, 0.1, 2, Unit());
    height_.onChange = [this](double v) {
      auto [a, f] = t_.host_.selection();
      t_.Keep(t_.editor_.SetRowHeight(a, f, ToTwips(v), "atLeast"));
    };
    drop(valign_, T(L"Vertical alignment"), kVAlign);
    valign_.onChange = [this](int i) {
      auto [a, f] = t_.host_.selection();
      t_.Keep(t_.editor_.SetCellProps(a, f, edit::PropEdit().Set("vAlign", {{"val", i == 1 ? "center" : i == 2 ? "bottom" : "top"}})));
    };
    spin(margins_, T(L"Cell margins"), 0, 2, 0.01, 2, Unit());
    margins_.onChange = [this](double v) {
      auto [a, f] = t_.host_.selection();
      const std::string w = std::to_string(ToTwips(v));
      edit::PropEdit e;
      for (const char* side : {"tcMar/top", "tcMar/left", "tcMar/bottom", "tcMar/right"}) e.Set(side, {{"w", w}, {"type", "dxa"}});
      t_.Keep(t_.editor_.SetCellProps(a, f, e));
    };
    drop(borders_, T(L"Borders"), kBorders);
    borders_.placeholder = T(L"Choose borders");
    borders_.onChange = [this](int i) {
      borders_.selected = -1;
      t_.BorderCells(i);
    };
    shading_.label = shading_.tooltip = T(L"Shading");
    shading_.automaticLabel = T(L"No color");
    shading_.automatic = true;
    shading_.onChange = [this](D2D1_COLOR_F c, bool automatic) { t_.ShadeCells(c, automatic); };
    drop(split_, T(L"Split cells"), kSplit);
    split_.placeholder = T(L"Choose");
    split_.onChange = [this](int i) {
      split_.selected = -1;
      auto c = t_.Cell();
      if (!c || i < 0) return;
      const uint32_t down = i >= 2 ? 2 : 1, across = i == 0 ? 2 : i == 1 ? 3 : i == 3 ? 2 : 1;
      t_.Run(t_.editor_.SplitCell(t_.host_.selection().second, down, across));
    };
    spin(cols_, T(L"Columns"), 1, 63, 1, 0, L"");
    spin(rows_, T(L"Rows"), 1, 1000, 1, 0, L"");
    cols_.SetValue(3);
    rows_.SetValue(3);
    insert_.label = insert_.tooltip = T(L"Insert table");
    insert_.onClick = [this] { t_.Insert(static_cast<uint32_t>(rows_.value), static_cast<uint32_t>(cols_.value)); };
    rows = {{T(L"Table"), nullptr},   {T(L"Style"), &style_},       {T(L"Alignment"), &align_}, {L"AutoFit", &fit_},
            {T(L"Header row"), &header_}, {T(L"Cells"), nullptr},   {T(L"Column width"), &width_}, {T(L"Row height"), &height_},
            {T(L"Vertical"), &valign_}, {T(L"Margins"), &margins_}, {T(L"Borders"), &borders_}, {T(L"Shading"), &shading_},
            {T(L"Split"), &split_},     {T(L"Insert table"), nullptr}, {T(L"Columns"), &cols_}, {T(L"Rows"), &rows_},
            {L"", &insert_}};
    int id = 10;
    for (auto& [label, c] : rows)
      if (c) c->id = id++;
  }

  std::wstring Title() const override { return T(L"Table"); }

  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override {
    ui_ = ui;
    body_ = body;
    Place(t_.Cell().has_value());
    Refresh();
    std::vector<Control*> controls;
    for (auto& [label, c] : rows)
      if (c) controls.push_back(c);
    return controls;
  }

  void Draw(Canvas& c) override {
    const Palette& p = theme::P();
    const float u = ui_;
    Refresh();
    for (const auto& [text, rc] : headings_) c.Text(text, rc, p.text, 13 * u, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    for (const auto& [label, control] : rows) {
      if (!control || !control->visible || label.empty()) continue;
      const float labelX = control->rc.left - kLabelW * u;
      c.Text(label, RectF(labelX, control->rc.top, control->rc.left - 8 * u, control->rc.bottom), p.textSecondary, 13 * u);
    }
  }

  std::vector<std::pair<std::wstring, Control*>> rows;  // label, control; nullptr starts a heading

 private:
  // In a table, its settings and its cell's; elsewhere, Insert table: the panel fits a window 700
  // pixels high (it doesn't scroll). Placed again when the selection goes in or out of a table.
  void Place(bool inTable) {
    inTable_ = inTable;
    const float u = ui_;
    headings_.clear();
    const float labelX = body_.left + 20 * u, cx = body_.left + (20 + kLabelW) * u, right = body_.right - 20 * u;
    float y = body_.top + 4 * u;
    bool insert = false;  // past the Insert table heading
    for (auto& [label, c] : rows) {
      if (!c && label == T(L"Insert table")) insert = true;
      const bool shown = insert != inTable;
      if (c) c->visible = shown;
      if (!shown) continue;
      if (!c) {
        if (!headings_.empty()) y += 10 * u;
        headings_.push_back({label, RectF(labelX, y, right, y + 24 * u)});
        y += 30 * u;
        continue;
      }
      float w = right - cx;
      if (c == &shading_) w = 76 * u;
      else if (dynamic_cast<SpinBox*>(c)) w = std::min(w, 120 * u);
      c->rc = RectF(std::round(cx), std::round(y), std::round(cx + w), std::round(y + 32 * u));
      y += kRowH * u;
    }
  }
  // The controls show the table and cell the selection is in.
  void Refresh() {
    auto [a, f] = t_.host_.selection();
    const uint32_t version = f.para < t_.Doc().Paragraphs().size() ? t_.Doc().Para(f.para).version : 0;
    if (a == shownA_ && f == shownF_ && version == shownVersion_) return;
    shownA_ = a;
    shownF_ = f;
    shownVersion_ = version;
    const auto cell = t_.Cell();
    if (cell.has_value() != inTable_) Place(cell.has_value());
    for (Control* c : {static_cast<Control*>(&style_), static_cast<Control*>(&align_), static_cast<Control*>(&fit_),
                       static_cast<Control*>(&header_), static_cast<Control*>(&width_), static_cast<Control*>(&height_),
                       static_cast<Control*>(&valign_), static_cast<Control*>(&margins_), static_cast<Control*>(&borders_),
                       static_cast<Control*>(&shading_), static_cast<Control*>(&split_)})
      c->enabled = cell.has_value() && t_.editor_.CanEdit();
    insert_.enabled = cols_.enabled = rows_.enabled = t_.editor_.CanEdit();
    if (!cell) return;
    const fw::doc::Document& doc = t_.Doc();
    const fw::doc::TableProps tp = doc.ResolvedTable(cell->tbl);
    const fw::doc::RowProps rp = doc.ResolvedRow(cell->tr);
    const fw::doc::CellProps cp = doc.ResolvedCell(cell->tc);
    styles_.clear();
    style_.items.clear();
    style_.selected = -1;
    for (const std::string& name : edit::TableStyleNames(doc)) {
      styles_.push_back(name);
      style_.items.push_back(W(name));
      if (const fw::doc::Style* s = doc.FindStyle(tp.style); s && s->name == name) style_.selected = static_cast<int>(styles_.size()) - 1;
    }
    align_.selected = tp.jc == fw::doc::Justification::Center ? 1 : tp.jc == fw::doc::Justification::End ? 2 : 0;
    fit_.selected = tp.fixedLayout ? 0 : tp.width.type == fw::doc::TableWidth::Type::Pct ? 2 : 1;
    header_.selected = rp.header ? 1 : 0;
    const auto grid = doc.TableGrid(cell->tbl);
    int32_t w = 0;
    for (int32_t k = cell->col; k < cell->col + cell->span && k < static_cast<int32_t>(grid.size()); ++k) w += grid[k].Value();
    width_.SetValue(FromTwips(w));
    height_.SetValue(FromTwips(rp.height.Value()));
    valign_.selected = cp.vAlign == "center" ? 1 : cp.vAlign == "bottom" ? 2 : 0;
    margins_.SetValue(FromTwips(cp.margins.start.type == fw::doc::TableWidth::Type::Dxa ? cp.margins.start.value : 108));
    shading_.automatic = cp.shading.fill.automatic || cp.shading.pattern.empty();
    if (!shading_.automatic) shading_.color = Rgb(cp.shading.fill.rgb & 0xFFFFFF);
  }

  DocTable& t_;
  float ui_ = 1;
  D2D1_RECT_F body_{};
  bool inTable_ = false;
  std::vector<std::pair<std::wstring, D2D1_RECT_F>> headings_;
  std::vector<std::string> styles_;
  Pos shownA_{fw::doc::kNone, 0}, shownF_{fw::doc::kNone, 0};
  uint32_t shownVersion_ = 0;
  Dropdown style_, align_, fit_, header_, valign_, borders_, split_;
  SpinBox width_, height_, margins_, cols_, rows_;
  ColorPicker shading_;
  PushButton insert_;
};

// ---------------------------------------------------------------------------

DocTable::DocTable(Editor& editor, Host host) : editor_(editor), host_(std::move(host)) {
  grid_ = std::make_unique<GridPicker>();
  grid_->label = T(L"Insert table");
  grid_->onPick = [this](int rows, int cols) { Insert(static_cast<uint32_t>(rows), static_cast<uint32_t>(cols)); };
  panel_ = std::make_unique<Panel>(*this);
}

DocTable::~DocTable() = default;

bool DocTable::Owns(int cmd) {
  return (cmd >= CmdTableGrid && cmd <= CmdTableProperties) || cmd == CmdCellShading || cmd == CmdCellBorders ||
         cmd == CmdRowHeightBox || cmd == CmdColumnWidthBox;
}

std::vector<int> DocTable::CommandIds() {
  std::vector<int> ids;
  for (const Named& n : kNames)
    if (n.cmd != CmdTableGrid && n.cmd != CmdTableBox) ids.push_back(n.cmd);
  return ids;
}

std::optional<Editor::CellInfo> DocTable::Cell() const {
  auto [a, f] = host_.selection();
  return editor_.CellAt(f);
}

CommandState DocTable::Query(int cmd) {
  CommandState s;
  s.label = LabelOf(cmd);
  const bool can = editor_.CanEdit();
  const auto cell = Cell();
  switch (cmd) {
    case CmdTableGrid:
    case CmdInsertTable: s.enabled = can; break;
    case CmdTextToTable: {
      auto [a, f] = host_.selection();
      s.enabled = can && !cell && !(a == f);
      break;
    }
    case CmdTableMerge: {
      s.enabled = can && SelectedCells().has_value();
      break;
    }
    case CmdTableHeaderRow:
      s.enabled = can && cell.has_value();
      s.checked = cell && Doc().ResolvedRow(cell->tr).header;
      break;
    case CmdCellShading:
    case CmdCellBorders:
      s.label = cmd == CmdCellShading ? T(L"Shading") : T(L"Borders");
      s.enabled = can && cell.has_value();
      break;
    case CmdRowHeightBox:
    case CmdColumnWidthBox:
      s.label = cmd == CmdRowHeightBox ? T(L"Row height") : T(L"Column width");
      s.enabled = can && cell.has_value();
      break;
    default: s.enabled = can && cell.has_value(); break;
  }
  return s;
}

void DocTable::Execute(int cmd) {
  if (!Query(cmd).enabled) return;
  auto [a, f] = host_.selection();
  switch (cmd) {
    case CmdTableGrid:
    case CmdInsertTable:
    case CmdTableSplit:
    case CmdTableProperties:
    case CmdRowHeightBox:  // a box's KeyTip or command search: the panel, which has both
    case CmdColumnWidthBox: host_.showPanel(panel_.get()); return;
    case CmdTextToTable: {
      const bool forward = editor_.Compare(a, f) <= 0;
      Run(editor_.TextToTable((forward ? a : f).para, (forward ? f : a).para));
      return;
    }
    case CmdTableToText: Run(editor_.TableToText(f)); return;
    case CmdTableInsertAbove: Run(editor_.InsertRows(a, f, false)); return;
    case CmdTableInsertBelow: Run(editor_.InsertRows(a, f, true)); return;
    case CmdTableInsertLeft: Run(editor_.InsertColumns(a, f, false)); return;
    case CmdTableInsertRight: Run(editor_.InsertColumns(a, f, true)); return;
    case CmdTableDeleteRows: Run(editor_.DeleteRows(a, f)); return;
    case CmdTableDeleteColumns: Run(editor_.DeleteColumns(a, f)); return;
    case CmdTableDeleteTable: Run(editor_.DeleteTable(f)); return;
    case CmdTableMerge: Run(editor_.MergeCells(a, f)); return;
    case CmdTableSelectCell: Select(Editor::Extent::Cells); return;
    case CmdTableSelectRow: Select(Editor::Extent::Rows); return;
    case CmdTableSelectColumn: Select(Editor::Extent::Columns); return;
    case CmdTableSelectTable: Select(Editor::Extent::Table); return;
    case CmdTableAutoFitWindow: Keep(editor_.AutoFit(f, Editor::Fit::Window)); return;
    case CmdTableFixedWidth: Keep(editor_.AutoFit(f, Editor::Fit::Fixed)); return;
    case CmdTableAutoFitContents:
      if (auto cell = Cell()) Keep(editor_.AutoFit(f, Editor::Fit::Contents, ContentWidths(*cell)));
      return;
    case CmdTableDistributeColumns: {
      // The block's columns (the whole table when it's one cell) share their width evenly.
      auto block = editor_.BlockAt(a, f);
      if (!block) return;
      const auto grid = Doc().TableGrid(block->tbl);
      int32_t c0 = block->col0, c1 = block->col1;
      if (a == f || block->cells.size() <= 1) c0 = 0, c1 = static_cast<int32_t>(grid.size()) - 1;
      int64_t sum = 0;
      for (int32_t k = c0; k <= c1 && k < static_cast<int32_t>(grid.size()); ++k) sum += grid[k].Value();
      if (c1 < c0) return;
      auto corners = a == f || block->cells.size() <= 1 ? editor_.Grow(f, f, Editor::Extent::Rows) : std::make_optional(std::make_pair(a, f));
      if (!corners) return;
      Keep(editor_.SetColumnWidth(corners->first, corners->second, static_cast<int32_t>(sum / (c1 - c0 + 1))));
      return;
    }
    case CmdTableHeaderRow: {
      edit::PropEdit e;
      if (Query(cmd).checked) e.Remove("tblHeader");
      else e.Set("tblHeader");
      Keep(editor_.SetRowProps(a, f, e));
      return;
    }
    case CmdCellShading: ShadeCells(shading_, noShading_); return;
    case CmdCellBorders: BorderCells(borders_); return;
  }
}

void DocTable::ShadeCells(D2D1_COLOR_F c, bool automatic) {
  auto [a, f] = host_.selection();
  edit::PropEdit e;
  if (automatic) {
    e.Remove("shd");
  } else {
    wchar_t hex[8];
    swprintf_s(hex, L"%02X%02X%02X", static_cast<int>(std::lround(c.r * 255)), static_cast<int>(std::lround(c.g * 255)),
               static_cast<int>(std::lround(c.b * 255)));
    e.Set("shd", {{"val", "clear"}, {"color", "auto"}, {"fill", fw::WideToUtf8(hex)}});
  }
  Keep(editor_.SetCellProps(a, f, e));
}

void DocTable::BorderCells(int which) {
  static constexpr Editor::CellBorders kWhich[] = {Editor::CellBorders::All,    Editor::CellBorders::Outside,
                                                   Editor::CellBorders::Inside, Editor::CellBorders::None,
                                                   Editor::CellBorders::Top,    Editor::CellBorders::Bottom,
                                                   Editor::CellBorders::Left,   Editor::CellBorders::Right};
  if (which < 0 || which >= static_cast<int>(std::size(kWhich))) return;
  auto [a, f] = host_.selection();
  Keep(editor_.SetCellBorders(a, f, kWhich[which]));
}

ToolItem DocTable::GridItem() {
  ToolItem t;
  t.kind = ToolKind::Custom;
  t.control = grid_.get();
  t.cmd = CmdTableGrid;
  t.width = 52;
  t.tooltip = T(L"Insert table");
  t.enabled = editor_.CanEdit();
  return t;
}

ToolItem DocTable::ShadingButton() {
  ToolItem t;
  t.kind = ToolKind::SplitButton;
  t.cmd = CmdCellShading;
  t.colors = true;
  t.color = shading_;
  t.automatic = noShading_;
  t.automaticLabel = T(L"No color");
  t.enabled = Query(CmdCellShading).enabled;
  t.onChange = [this](const ToolValue& v) {
    shading_ = v.color;
    noShading_ = v.automatic;
    ShadeCells(v.color, v.automatic);
  };
  return t;
}

ToolItem DocTable::SizeBox(bool height) {
  ToolItem t;
  t.kind = ToolKind::SpinBox;
  t.cmd = height ? CmdRowHeightBox : CmdColumnWidthBox;
  t.width = 100;  // "15,24 cm" beside the spin arrows
  t.minValue = height ? 0 : 0.1;
  t.maxValue = 22;
  t.step = 0.1;
  t.decimals = 2;
  t.unit = Unit();
  const auto cell = Cell();
  t.enabled = editor_.CanEdit() && cell.has_value();
  if (!cell) return t;
  if (height) {
    t.value = FromTwips(Doc().ResolvedRow(cell->tr).height.Value());
  } else {
    const auto grid = Doc().TableGrid(cell->tbl);
    int32_t w = 0;
    for (int32_t k = cell->col; k < cell->col + cell->span && k < static_cast<int32_t>(grid.size()); ++k) w += grid[k].Value();
    t.value = FromTwips(w);
  }
  t.onChange = [this, height](const ToolValue& v) {
    auto [a, f] = host_.selection();
    Keep(height ? editor_.SetRowHeight(a, f, ToTwips(v.number), "atLeast") : editor_.SetColumnWidth(a, f, ToTwips(v.number)));
  };
  return t;
}

ToolItem DocTable::BordersButton() {
  ToolItem t;
  t.kind = ToolKind::SplitButton;
  t.cmd = CmdCellBorders;
  t.choices = kBorders;
  t.choice = borders_;
  t.enabled = Query(CmdCellBorders).enabled;
  t.onChange = [this](const ToolValue& v) {
    if (v.index < 0 || v.index >= static_cast<int>(kBorders.size())) return;
    borders_ = v.index;
    BorderCells(v.index);
  };
  return t;
}

void DocTable::Insert(uint32_t rows, uint32_t cols) {
  auto [a, f] = host_.selection();
  // Then typing goes into its first cell, wherever Insert was chosen (the panel keeps the keyboard).
  if (Run(editor_.InsertTable(editor_.Compare(a, f) <= 0 ? a : f, rows, cols)) && host_.focusPage) host_.focusPage();
}

void DocTable::Select(Editor::Extent extent) {
  auto [a, f] = host_.selection();
  if (extent == Editor::Extent::Cells) {
    if (auto cell = editor_.CellAt(f)) host_.select(cell->start, cell->end);
    return;
  }
  if (auto corners = editor_.Grow(a, f, extent)) host_.select(corners->first, corners->second);
}

void DocTable::ApplyStyle(const std::string& name) {
  auto [a, f] = host_.selection();
  Keep(edit::ApplyTableStyle(editor_, f, name));
}

std::vector<int32_t> DocTable::ContentWidths(const Editor::CellInfo& at) const {
  // Each grid column: the longest paragraph of its single-column cells, at about half an em a
  // character, with the cells' margins; then the table narrowed to the page if it's wider.
  const fw::doc::Document& doc = Doc();
  const auto grid = doc.TableGrid(at.tbl);
  std::vector<int32_t> widths(grid.size(), 360);
  for (fw::doc::Node tr : doc.TableRows(at.tbl))
    for (fw::doc::Node tc : doc.RowCells(tr)) {
      const fw::doc::CellProps cp = doc.ResolvedCell(tc);
      const int32_t col = doc.CellGridColumn(tc);
      if (cp.gridSpan != 1 || col < 0 || col >= static_cast<int32_t>(widths.size())) continue;
      for (const fw::doc::Block& b : doc.Blocks(tc)) {
        if (b.kind != fw::doc::BlockKind::Paragraph) continue;
        const double pt = doc.ResolvedMark(b.para).size.Value() / 2.0;
        const auto chars = static_cast<double>(doc.Para(b.para).text.size());
        widths[col] = std::max(widths[col], static_cast<int32_t>(chars * (pt > 0 ? pt : 11) * 0.5 * 20 + 216 + 40));
      }
    }
  int64_t sum = 0, was = 0;
  for (int32_t w : widths) sum += w;
  for (fw::Twips w : grid) was += w.Value();
  // The room: what the table has now, or the usual text width, whichever is wider.
  const int64_t room = std::max<int64_t>(was, 9360);
  if (sum > room)
    for (int32_t& w : widths) w = static_cast<int32_t>(static_cast<int64_t>(w) * room / sum);
  return widths;
}

void DocTable::FillContextMenu(HMENU m) {
  using menu::AppendItem;
  if (!Cell()) return;
  HMENU insert = CreatePopupMenu();
  for (int cmd : {CmdTableInsertAbove, CmdTableInsertBelow, CmdTableInsertLeft, CmdTableInsertRight})
    AppendItem(insert, cmd, LabelOf(cmd), Query(cmd).enabled);
  menu::AppendSubmenu(m, insert, T(L"Insert"));
  HMENU del = CreatePopupMenu();
  for (int cmd : {CmdTableDeleteRows, CmdTableDeleteColumns, CmdTableDeleteTable}) AppendItem(del, cmd, LabelOf(cmd), Query(cmd).enabled);
  menu::AppendSubmenu(m, del, T(L"Delete"));
  HMENU select = CreatePopupMenu();
  for (int cmd : {CmdTableSelectCell, CmdTableSelectRow, CmdTableSelectColumn, CmdTableSelectTable})
    AppendItem(select, cmd, LabelOf(cmd), Query(cmd).enabled);
  menu::AppendSubmenu(m, select, T(L"Select"));
  AppendItem(m, CmdTableMerge, LabelOf(CmdTableMerge), Query(CmdTableMerge).enabled);
  AppendItem(m, CmdTableSplit, LabelOf(CmdTableSplit), Query(CmdTableSplit).enabled);
  AppendItem(m, CmdTableProperties, LabelOf(CmdTableProperties), Query(CmdTableProperties).enabled);
  menu::AppendSeparator(m);
}

bool DocTable::Tab(bool back) {
  auto [a, f] = host_.selection();
  if (!editor_.CellAt(f)) return false;
  auto next = editor_.NextCell(f, back);
  if (!next && !back) {
    // After the last cell: a new row, as Word adds one.
    if (!Run(editor_.InsertRows(f, f, true))) return true;
    next = editor_.NextCell(f, false);
  }
  if (next) host_.select(next->start, next->end);
  return true;
}

std::optional<Editor::CellBlock> DocTable::SelectedCells() const {
  auto [a, f] = host_.selection();
  if (a == f) return std::nullopt;
  auto ca = editor_.CellAt(a), cf = editor_.CellAt(f);
  if (!ca || !cf || ca->tbl.Id() != cf->tbl.Id() || ca->tc.Id() == cf->tc.Id()) return std::nullopt;
  return editor_.BlockAt(a, f);
}

bool DocTable::ReplaceCells(std::u16string_view text) {
  auto block = SelectedCells();
  if (!block) return false;
  auto [a, f] = host_.selection();
  editor_.BeginGroup(text.empty() ? T(u"Delete") : T(u"Typing"));
  edit::Result r = editor_.ClearCells(a, f);
  if (r && !text.empty()) r = editor_.InsertText(r.caret, text);
  if (r) editor_.EndGroup();
  else editor_.CancelGroup();
  Run(r);
  return true;
}
