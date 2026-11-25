#pragma once

// Filter's side panel (s2-format): the column's values to keep and a custom condition. Defined here,
// whole, so that WorkbookView's members can own one.

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include "core/Lang.h"
#include "core/Strings.h"
#include "sheet/view/WorkbookFormat.h"
#include "sheet/view/WorkbookPanels.h"
#include "ui/Gfx.h"
#include "ui/Theme.h"

namespace fw::sheet::view {

namespace filter_detail {

// Excel's custom AutoFilter conditions (1-12: text ones as * patterns), then its other number
// filters (13-18: Top 10 by items or percent, the average), which take the first condition's place.
inline constexpr UiText kOps[] = {N_(L"(no condition)"),
                                          N_(L"equals"),
                                          N_(L"does not equal"),
                                          N_(L"is greater than"),
                                          N_(L"is greater than or equal to"),
                                          N_(L"is less than"),
                                          N_(L"is less than or equal to"),
                                          N_(L"begins with"),
                                          N_(L"does not begin with"),
                                          N_(L"ends with"),
                                          N_(L"does not end with"),
                                          L"contains",
                                          N_(L"does not contain"),
                                          N_(L"is in the top (items)"),
                                          N_(L"is in the bottom (items)"),
                                          N_(L"is in the top (percent)"),
                                          N_(L"is in the bottom (percent)"),
                                          N_(L"is above average"),
                                          N_(L"is below average")};
inline constexpr int kFirstSpecial = 13;
// Conditions 1-12: the comparison and whether the value is wrapped in * (before, after).
struct OpChoice {
  data::FilterCondition::Op op;
  bool lead, trail;
};
inline constexpr OpChoice kOpChoices[] = {
    {data::FilterCondition::Op::Equal, false, false},    {data::FilterCondition::Op::Equal, false, false},
    {data::FilterCondition::Op::NotEqual, false, false}, {data::FilterCondition::Op::Greater, false, false},
    {data::FilterCondition::Op::GreaterEqual, false, false}, {data::FilterCondition::Op::Less, false, false},
    {data::FilterCondition::Op::LessEqual, false, false}, {data::FilterCondition::Op::Equal, false, true},
    {data::FilterCondition::Op::NotEqual, false, true},  {data::FilterCondition::Op::Equal, true, false},
    {data::FilterCondition::Op::NotEqual, true, false},  {data::FilterCondition::Op::Equal, true, true},
    {data::FilterCondition::Op::NotEqual, true, true}};
inline constexpr wchar_t kChecked[] = L"\x2611  ", kUnchecked[] = L"\x2610  ";

}  // namespace filter_detail

class WorkbookView::FilterPanel : public PanelContent {
 public:
  explicit FilterPanel(WorkbookView& view) : v_(view) {
    values_.label = T(L"Values");
    values_.rowHeight = 28;
    values_.onSelect = [this](int i) {
      if (i < 0 || toggling_) return;
      Toggle(static_cast<size_t>(i));
      toggling_ = true;
      values_.SetSelected(-1);  // the same item can be clicked again
      toggling_ = false;
    };
    for (int k = 0; k < 2; ++k) {
      op_[k].label = k == 0 ? T(L"Condition") : T(L"Second condition");
      op_[k].items.assign(std::begin(filter_detail::kOps), std::end(filter_detail::kOps) - (k == 0 ? 0 : 6));
      op_[k].selected = 0;
      value_[k].label = T(L"Value");
      value_[k].autoComplete = false;
      // A condition picked from the list hands the keyboard to its value, as Excel's dialog has the
      // box ready for the number (the average conditions take none), and Enter there applies.
      op_[k].onPicked = [this, k](int i) {
        if (i > 0 && i < filter_detail::kFirstSpecial + 4) focusWanted_ = k + 1;
      };
      value_[k].onEnter = [this] { Apply(); };
    }
    and_.text = T(L"And");
    or_.text = T(L"Or");
    and_.toggle = or_.toggle = true;
    or_.checked = true;
    and_.onInvoke = [this] { and_.checked = true, or_.checked = false; };
    or_.onInvoke = [this] { or_.checked = true, and_.checked = false; };
    sortUp_.text = T(L"Sort A to Z");
    sortDown_.text = T(L"Sort Z to A");
    for (PanelButton* b : {&sortUp_, &sortDown_})
      b->onInvoke = [this, b] {
        if (const ViewFilter* f = v_.FilterOf())
          v_.SortRange(f->ref, true, {{f->ref.firstCol + column_, b == &sortDown_}});
      };
    apply_.text = T(L"Apply");
    apply_.onInvoke = [this] { Apply(); };
    clear_.text = T(L"Clear this column's filter");
    clear_.onInvoke = [this] {
      if (ViewFilter* f = v_.FilterOf()) {
        std::erase_if(f->criteria, [&](const data::FilterCriteria& c) { return c.column == column_; });
        v_.ApplyFilter();
      }
      Open(column_);
    };
  }

  std::wstring Title() const override { return T(L"Filter: ") + header_; }

  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override {
    ui_ = ui;
    const float left = body.left + 16 * ui, right = body.right - 16 * ui;
    float y = body.top + 12 * ui;
    labels_.clear();
    auto label = [&](const wchar_t* text) {
      labels_.push_back({text, gfx::RectF(left, y, right, y + 16 * ui)});
      y += 20 * ui;
    };
    const float half = (right - left - 8 * ui) / 2;
    sortUp_.rc = gfx::RectF(left, y, left + half, y + 30 * ui);
    sortDown_.rc = gfx::RectF(left + half + 8 * ui, y, right, y + 30 * ui);
    y += 42 * ui;
    label(T(L"Show the rows with these values"));
    const float listBottom = std::max(y + 100 * ui, body.bottom - 222 * ui);
    values_.rc = gfx::RectF(left, y, right, listBottom);
    y = listBottom + 14 * ui;
    label(T(L"Or with conditions"));
    // Each condition on one line: what (wider), then the value.
    const float split = left + (right - left) * 0.58f;
    for (int k = 0; k < 2; ++k) {
      op_[k].rc = gfx::RectF(left, y, split - 4 * ui, y + 30 * ui);
      value_[k].rc = gfx::RectF(split + 4 * ui, y, right, y + 30 * ui);
      y += 40 * ui;
      if (k == 0) {
        and_.rc = gfx::RectF(left, y, left + half, y + 26 * ui);
        or_.rc = gfx::RectF(left + half + 8 * ui, y, right, y + 26 * ui);
        y += 36 * ui;
      }
    }
    y += 2 * ui;
    apply_.rc = gfx::RectF(left, y, left + half, y + 30 * ui);
    clear_.rc = gfx::RectF(left + half + 8 * ui, y, right, y + 30 * ui);
    clear_.textSize = 12;
    return {&sortUp_, &sortDown_, &values_, &op_[0], &value_[0], &and_, &or_, &op_[1], &value_[1], &apply_, &clear_};
  }

  void Draw(gfx::Canvas& c) override {
    for (const auto& [text, rc] : labels_) c.Text(text, rc, theme::P().textSecondary, 12 * ui_);
    if (focusWanted_) {
      value_[focusWanted_ - 1].TakeFocus();
      focusWanted_ = 0;
    }
  }

  void Open(uint32_t column) {
    column_ = column;
    ViewFilter* f = v_.FilterOf();
    if (!f) return;
    const Sheet& sheet = v_.Current();
    const NumLocale& loc = v_.text_->Locale();
    header_ = data::ShownText(*v_.book_, sheet, f->ref.firstRow, f->ref.firstCol + column, loc);
    if (header_.empty()) header_ = L"column " + Utf8ToWide(ColumnName(f->ref.firstCol + column));
    bool blanks = false;
    texts_ = data::ColumnValues(*v_.book_, sheet, f->ref, column, loc, blanks);
    if (blanks) texts_.push_back(L"");  // (Blanks), last as in Excel
    const data::FilterCriteria* c = nullptr;
    for (const data::FilterCriteria& x : f->criteria)
      if (x.column == column) c = &x;
    checked_.assign(texts_.size(), true);
    if (c && c->listed) {
      for (size_t i = 0; i < texts_.size(); ++i) {
        if (texts_[i].empty()) {
          checked_[i] = c->blanks;
          continue;
        }
        checked_[i] = std::any_of(c->values.begin(), c->values.end(), [&](const std::wstring& v) {
          return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, v.c_str(), -1, texts_[i].c_str(), -1, nullptr,
                                 nullptr, 0) == CSTR_EQUAL;
        });
      }
    }
    for (int k = 0; k < 2; ++k) {
      op_[k].selected = 0;
      value_[k].SetText(L"");
    }
    and_.checked = c && c->both;
    or_.checked = !and_.checked;
    if (c && c->special != data::FilterCriteria::Special::None) {
      using Special = data::FilterCriteria::Special;
      op_[0].selected = c->special == Special::AboveAverage   ? filter_detail::kFirstSpecial + 4
                        : c->special == Special::BelowAverage ? filter_detail::kFirstSpecial + 5
                                                              : filter_detail::kFirstSpecial + (c->percent ? 2 : 0) + (c->bottom ? 1 : 0);
      if (c->special == Special::Top10) {
        wchar_t count[32];
        std::swprintf(count, 32, L"%g", c->count);
        value_[0].SetText(count);
      }
    } else if (c) {
      for (size_t k = 0; k < c->conditions.size() && k < 2; ++k) ShowCondition(static_cast<int>(k), c->conditions[k]);
    }
    Refresh();
  }

 private:
  void Refresh() {
    values_.items.clear();
    const bool all = std::all_of(checked_.begin(), checked_.end(), [](bool b) { return b; });
    values_.items.push_back(std::wstring(all ? filter_detail::kChecked : filter_detail::kUnchecked) + T(L"(Select all)"));
    for (size_t i = 0; i < texts_.size(); ++i)
      values_.items.push_back(std::wstring(checked_[i] ? filter_detail::kChecked : filter_detail::kUnchecked) + (texts_[i].empty() ? L"(Blanks)" : texts_[i]));
    v_.host_.ViewInvalidate();
  }

  void Toggle(size_t item) {
    if (item == 0) {
      const bool all = std::all_of(checked_.begin(), checked_.end(), [](bool b) { return b; });
      checked_.assign(checked_.size(), !all);
    } else if (item - 1 < checked_.size()) {
      checked_[item - 1] = !checked_[item - 1];
    }
    Refresh();
  }

  // A stored condition as the choice that makes it: "a*" with equals is "begins with a".
  void ShowCondition(int k, const data::FilterCondition& c) {
    using Op = data::FilterCondition::Op;
    std::wstring v = c.value;
    int op = 1 + static_cast<int>(c.op);  // equals .. is less than or equal to
    const bool lead = v.size() > 1 && v.front() == L'*', trail = v.size() > 1 && v.back() == L'*';
    if ((c.op == Op::Equal || c.op == Op::NotEqual) && (lead || trail)) {
      for (int i = 7; i <= 12; ++i) {
        const filter_detail::OpChoice& o = filter_detail::kOpChoices[i];
        if (o.op == c.op && o.lead == lead && o.trail == trail) op = i;
      }
      if (trail) v.pop_back();
      if (lead) v.erase(0, 1);
    }
    op_[k].selected = op;
    value_[k].SetText(v);
  }

  // Condition k as chosen; false when it has none.
  bool Condition(int k, data::FilterCondition& cond) {
    const int op = op_[k].selected;
    const std::wstring typed = value_[k].Text();
    if (op <= 0 || op >= filter_detail::kFirstSpecial || typed.empty()) return false;
    const filter_detail::OpChoice& o = filter_detail::kOpChoices[op];
    cond.op = o.op;
    cond.value = (o.lead ? L"*" : L"") + typed + (o.trail ? L"*" : L"");
    if (op <= 6) {
      // A number as the region types it is compared by value (stored invariant).
      std::wstring number = typed;
      const NumLocale& loc = v_.text_->Locale();
      if (loc.decimal != L".") std::replace(number.begin(), number.end(), loc.decimal.front(), L'.');
      wchar_t* end = nullptr;
      std::wcstod(number.c_str(), &end);
      if (end && *end == 0) cond.value = number;
    }
    return true;
  }

  void Apply() {
    ViewFilter* f = v_.FilterOf();
    if (!f) return;
    std::erase_if(f->criteria, [&](const data::FilterCriteria& c) { return c.column == column_; });
    data::FilterCriteria c;
    c.column = column_;
    const int first = op_[0].selected;
    data::FilterCondition cond;
    if (first >= filter_detail::kFirstSpecial) {
      using Special = data::FilterCriteria::Special;
      const int s = first - filter_detail::kFirstSpecial;  // top, bottom, top %, bottom %, above, below
      c.special = s == 4 ? Special::AboveAverage : s == 5 ? Special::BelowAverage : Special::Top10;
      c.bottom = s == 1 || s == 3;
      c.percent = s == 2 || s == 3;
      if (c.special == Special::Top10) {
        const double n = std::wcstod(value_[0].Text().c_str(), nullptr);
        c.count = n >= 1 ? std::min(n, c.percent ? 100.0 : 500.0) : 10;  // Excel's limits: 1-500 items, 1-100 %
      }
      f->criteria.push_back(c);
    } else if (Condition(0, cond)) {
      c.conditions.push_back(cond);
      if (Condition(1, cond)) c.conditions.push_back(cond);
      c.both = and_.checked && c.conditions.size() == 2;
      f->criteria.push_back(c);
    } else if (!std::all_of(checked_.begin(), checked_.end(), [](bool b) { return b; })) {
      c.listed = true;
      for (size_t i = 0; i < texts_.size(); ++i) {
        if (!checked_[i]) continue;
        if (texts_[i].empty()) c.blanks = true;
        else c.values.push_back(texts_[i]);
      }
      f->criteria.push_back(c);
    }
    v_.ApplyFilter();
  }

  WorkbookView& v_;
  uint32_t column_ = 0;
  std::wstring header_;
  std::vector<std::wstring> texts_;  // the column's values; "" for (Blanks)
  std::vector<bool> checked_;
  bool toggling_ = false;
  float ui_ = 1;
  std::vector<std::pair<std::wstring, D2D1_RECT_F>> labels_;
  ListBox values_;
  Dropdown op_[2];
  FocusComboBox value_[2];
  int focusWanted_ = 0;  // 1 + the condition whose value box takes the keyboard next
  PanelButton and_, or_, sortUp_, sortDown_, apply_, clear_;
};

// Custom sort (up to three keys) and Remove duplicates (the columns compared), on the data range.
class WorkbookView::DataPanel : public PanelContent {
 public:
  explicit DataPanel(WorkbookView& view) : v_(view) {
    header_.text = T(L"My data has headers");
    header_.toggle = true;
    header_.onInvoke = [this] { Columns(); };
    for (int k = 0; k < 3; ++k) {
      keys_[k].label = k == 0 ? T(L"Sort by") : T(L"Then by");
      orders_[k].label = T(L"Order");
      orders_[k].items = {T(L"A to Z"), T(L"Z to A")};
      orders_[k].selected = 0;
    }
    columns_.label = T(L"Columns");
    columns_.rowHeight = 28;
    columns_.onSelect = [this](int i) {
      if (i < 0 || toggling_ || static_cast<size_t>(i) >= checked_.size()) return;
      checked_[static_cast<size_t>(i)] = !checked_[static_cast<size_t>(i)];
      Columns();
      toggling_ = true;
      columns_.SetSelected(-1);
      toggling_ = false;
    };
    apply_.onInvoke = [this] { Apply(); };
  }

  std::wstring Title() const override { return sort_ ? T(L"Sort") : T(L"Remove duplicates"); }

  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override {
    ui_ = ui;
    const float left = body.left + 16 * ui, right = body.right - 16 * ui, gap = 8 * ui;
    float y = body.top + 12 * ui;
    labels_.clear();
    header_.rc = gfx::RectF(left, y, right, y + 30 * ui);
    y += 42 * ui;
    for (int k = 0; k < 3; ++k) {
      keys_[k].visible = orders_[k].visible = sort_;
      if (!sort_) continue;
      const float w = (right - left - gap) * 0.62f;
      labels_.push_back({k == 0 ? T(L"Sort by") : T(L"Then by"), gfx::RectF(left, y, left + w, y + 16 * ui)});
      labels_.push_back({T(L"Order"), gfx::RectF(left + w + gap, y, right, y + 16 * ui)});
      y += 20 * ui;
      keys_[k].rc = gfx::RectF(left, y, left + w, y + 30 * ui);
      orders_[k].rc = gfx::RectF(left + w + gap, y, right, y + 30 * ui);
      y += 42 * ui;
    }
    columns_.visible = !sort_;
    if (!sort_) {
      labels_.push_back({T(L"Rows are duplicates when these columns match"), gfx::RectF(left, y, right, y + 16 * ui)});
      y += 20 * ui;
      const float bottom = std::max(y + 100 * ui, body.bottom - 70 * ui);
      columns_.rc = gfx::RectF(left, y, right, bottom);
      y = bottom + 12 * ui;
    }
    apply_.text = sort_ ? T(L"Sort") : T(L"Remove duplicates");
    apply_.rc = gfx::RectF(left, y, right, y + 30 * ui);
    return {&header_, &keys_[0], &orders_[0], &keys_[1], &orders_[1], &keys_[2], &orders_[2], &columns_, &apply_};
  }

  void Draw(gfx::Canvas& c) override {
    for (const auto& [text, rc] : labels_) c.Text(text, rc, theme::P().textSecondary, 12 * ui_);
  }

  void Open(bool sort) {
    sort_ = sort;
    range_ = v_.DataRange();
    header_.checked = data::GuessHeader(*v_.book_, v_.Current(), range_);
    checked_.assign(range_.lastCol - range_.firstCol + 1, true);
    Columns();
    for (int k = 0; k < 3; ++k) {
      keys_[k].selected = k == 0 ? static_cast<int>(std::clamp(v_.active_.col, range_.firstCol, range_.lastCol) - range_.firstCol) + 1 : 0;
      orders_[k].selected = 0;
    }
  }

 private:
  // The columns' names: the header row's texts, else "Column B".
  void Columns() {
    std::vector<std::wstring> names;
    for (uint32_t c = range_.firstCol; c <= range_.lastCol; ++c) {
      std::wstring n = header_.checked ? data::ShownText(*v_.book_, v_.Current(), range_.firstRow, c, v_.text_->Locale())
                                       : std::wstring();
      if (n.empty()) n = Fmt(T(L"Column {0}"), {Utf8ToWide(ColumnName(c))});
      names.push_back(n);
    }
    for (int k = 0; k < 3; ++k) {
      keys_[k].items = {k == 0 ? T(L"(choose a column)") : L"(none)"};
      keys_[k].items.insert(keys_[k].items.end(), names.begin(), names.end());
    }
    columns_.items.clear();
    for (size_t i = 0; i < names.size(); ++i)
      columns_.items.push_back(std::wstring(checked_[i] ? filter_detail::kChecked : filter_detail::kUnchecked) + names[i]);
    v_.host_.ViewInvalidate();
  }

  void Apply() {
    if (sort_) {
      std::vector<data::SortKey> keys;
      for (int k = 0; k < 3; ++k)
        if (keys_[k].selected > 0)
          keys.push_back({range_.firstCol + static_cast<uint32_t>(keys_[k].selected - 1), orders_[k].selected == 1});
      if (!keys.empty()) v_.SortRange(range_, header_.checked, keys);
    } else {
      std::vector<uint32_t> cols;
      for (size_t i = 0; i < checked_.size(); ++i)
        if (checked_[i]) cols.push_back(static_cast<uint32_t>(i));
      if (!cols.empty()) v_.RemoveDuplicates(range_, header_.checked, cols);
    }
    v_.host_.ViewShowPanel(nullptr);
  }

  WorkbookView& v_;
  bool sort_ = true, toggling_ = false;
  Range range_{};
  std::vector<bool> checked_;
  float ui_ = 1;
  std::vector<std::pair<std::wstring, D2D1_RECT_F>> labels_;
  PanelButton header_, apply_;
  Dropdown keys_[3], orders_[3];
  ListBox columns_;
};

}  // namespace fw::sheet::view
