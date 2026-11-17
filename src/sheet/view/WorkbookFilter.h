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

}  // namespace fw::sheet::view
