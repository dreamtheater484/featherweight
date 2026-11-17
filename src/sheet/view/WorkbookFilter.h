#pragma once

// Filter's side panel (s2-format): the column's values to keep and a custom condition. Defined here,
// whole, so that WorkbookView's members can own one.

#include <algorithm>
#include <string>
#include <vector>

#include "core/Strings.h"
#include "sheet/view/WorkbookFormat.h"
#include "ui/Gfx.h"
#include "ui/Theme.h"

namespace fw::sheet::view {

namespace filter_detail {

inline constexpr const wchar_t* kOps[] = {L"(no condition)", L"equals",       L"does not equal", L"is greater than",
                                   L"is greater than or equal to", L"is less than", L"is less than or equal to",
                                   L"begins with",  L"ends with",     L"contains",       L"does not contain"};
inline constexpr wchar_t kChecked[] = L"\x2611  ", kUnchecked[] = L"\x2610  ";

}  // namespace filter_detail

class WorkbookView::FilterPanel : public PanelContent {
 public:
  explicit FilterPanel(WorkbookView& view) : v_(view) {
    values_.label = L"Values";
    values_.rowHeight = 28;
    values_.onSelect = [this](int i) {
      if (i < 0 || toggling_) return;
      Toggle(static_cast<size_t>(i));
      toggling_ = true;
      values_.SetSelected(-1);  // the same item can be clicked again
      toggling_ = false;
    };
    op_.label = L"Condition";
    op_.items.assign(std::begin(filter_detail::kOps), std::end(filter_detail::kOps));
    op_.selected = 0;
    value_.label = L"Value";
    value_.autoComplete = false;
    apply_.text = L"Apply";
    apply_.onInvoke = [this] { Apply(); };
    clear_.text = L"Clear this column's filter";
    clear_.onInvoke = [this] {
      if (ViewFilter* f = v_.FilterOf()) {
        std::erase_if(f->criteria, [&](const data::FilterCriteria& c) { return c.column == column_; });
        v_.ApplyFilter();
      }
      Open(column_);
    };
  }

  std::wstring Title() const override { return L"Filter: " + header_; }

  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override {
    ui_ = ui;
    const float left = body.left + 16 * ui, right = body.right - 16 * ui;
    float y = body.top + 12 * ui;
    labels_.clear();
    auto label = [&](const wchar_t* text) {
      labels_.push_back({text, gfx::RectF(left, y, right, y + 16 * ui)});
      y += 20 * ui;
    };
    label(L"Show the rows with these values");
    const float listBottom = std::max(y + 120 * ui, body.bottom - 210 * ui);
    values_.rc = gfx::RectF(left, y, right, listBottom);
    y = listBottom + 14 * ui;
    label(L"Or with a condition");
    op_.rc = gfx::RectF(left, y, right, y + 30 * ui);
    y += 40 * ui;
    value_.rc = gfx::RectF(left, y, right, y + 30 * ui);
    y += 42 * ui;
    const float half = (right - left - 8 * ui) / 2;
    apply_.rc = gfx::RectF(left, y, left + half, y + 30 * ui);
    clear_.rc = gfx::RectF(left + half + 8 * ui, y, right, y + 30 * ui);
    clear_.textSize = 12;
    return {&values_, &op_, &value_, &apply_, &clear_};
  }

  void Draw(gfx::Canvas& c) override {
    for (const auto& [text, rc] : labels_) c.Text(text, rc, theme::P().textSecondary, 12 * ui_);
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
    op_.selected = 0;
    value_.SetText(L"");
    if (c && !c->conditions.empty()) ShowCondition(c->conditions.front());
    Refresh();
  }

 private:
  void Refresh() {
    values_.items.clear();
    const bool all = std::all_of(checked_.begin(), checked_.end(), [](bool b) { return b; });
    values_.items.push_back(std::wstring(all ? filter_detail::kChecked : filter_detail::kUnchecked) + L"(Select all)");
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

  void ShowCondition(const data::FilterCondition& c) {
    using Op = data::FilterCondition::Op;
    std::wstring v = c.value;
    int op = 1 + static_cast<int>(c.op);
    const bool lead = v.size() > 1 && v.front() == L'*', trail = v.size() > 1 && v.back() == L'*';
    if (c.op == Op::Equal || c.op == Op::NotEqual) {
      if (lead && trail) op = c.op == Op::Equal ? 9 : 10, v = v.substr(1, v.size() - 2);
      else if (trail && c.op == Op::Equal) op = 7, v.pop_back();
      else if (lead && c.op == Op::Equal) op = 8, v.erase(0, 1);
    }
    op_.selected = op;
    value_.SetText(v);
  }

  void Apply() {
    ViewFilter* f = v_.FilterOf();
    if (!f) return;
    std::erase_if(f->criteria, [&](const data::FilterCriteria& c) { return c.column == column_; });
    data::FilterCriteria c;
    c.column = column_;
    const std::wstring typed = value_.Text();
    if (op_.selected > 0 && !typed.empty()) {
      using Op = data::FilterCondition::Op;
      data::FilterCondition cond;
      cond.value = typed;
      // A number as the region types it is compared by value (stored invariant).
      std::wstring number = typed;
      const NumLocale& loc = v_.text_->Locale();
      if (loc.decimal != L".") std::replace(number.begin(), number.end(), loc.decimal.front(), L'.');
      wchar_t* end = nullptr;
      std::wcstod(number.c_str(), &end);
      if (end && *end == 0 && op_.selected <= 6) cond.value = number;
      switch (op_.selected) {
        case 1: cond.op = Op::Equal; break;
        case 2: cond.op = Op::NotEqual; break;
        case 3: cond.op = Op::Greater; break;
        case 4: cond.op = Op::GreaterEqual; break;
        case 5: cond.op = Op::Less; break;
        case 6: cond.op = Op::LessEqual; break;
        case 7: cond.op = Op::Equal, cond.value = typed + L"*"; break;
        case 8: cond.op = Op::Equal, cond.value = L"*" + typed; break;
        case 9: cond.op = Op::Equal, cond.value = L"*" + typed + L"*"; break;
        case 10: cond.op = Op::NotEqual, cond.value = L"*" + typed + L"*"; break;
      }
      c.conditions.push_back(cond);
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
  Dropdown op_;
  ComboBox value_;
  PanelButton apply_, clear_;
};

// Custom sort (up to three keys) and Remove duplicates (the columns compared), on the data range.
class WorkbookView::DataPanel : public PanelContent {
 public:
  explicit DataPanel(WorkbookView& view) : v_(view) {
    header_.text = L"My data has headers";
    header_.toggle = true;
    header_.onInvoke = [this] { Columns(); };
    for (int k = 0; k < 3; ++k) {
      keys_[k].label = k == 0 ? L"Sort by" : L"Then by";
      orders_[k].label = L"Order";
      orders_[k].items = {L"A to Z", L"Z to A"};
      orders_[k].selected = 0;
    }
    columns_.label = L"Columns";
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

  std::wstring Title() const override { return sort_ ? L"Sort" : L"Remove duplicates"; }

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
      labels_.push_back({k == 0 ? L"Sort by" : L"Then by", gfx::RectF(left, y, left + w, y + 16 * ui)});
      labels_.push_back({L"Order", gfx::RectF(left + w + gap, y, right, y + 16 * ui)});
      y += 20 * ui;
      keys_[k].rc = gfx::RectF(left, y, left + w, y + 30 * ui);
      orders_[k].rc = gfx::RectF(left + w + gap, y, right, y + 30 * ui);
      y += 42 * ui;
    }
    columns_.visible = !sort_;
    if (!sort_) {
      labels_.push_back({L"Rows are duplicates when these columns match", gfx::RectF(left, y, right, y + 16 * ui)});
      y += 20 * ui;
      const float bottom = std::max(y + 100 * ui, body.bottom - 70 * ui);
      columns_.rc = gfx::RectF(left, y, right, bottom);
      y = bottom + 12 * ui;
    }
    apply_.text = sort_ ? L"Sort" : L"Remove duplicates";
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
      if (n.empty()) n = L"Column " + Utf8ToWide(ColumnName(c));
      names.push_back(n);
    }
    for (int k = 0; k < 3; ++k) {
      keys_[k].items = {k == 0 ? L"(choose a column)" : L"(none)"};
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
