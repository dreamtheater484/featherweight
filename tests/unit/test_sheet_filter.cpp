#include <cmath>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "sheet/data/Filter.h"

using namespace fw::sheet;

namespace {

// Name, amount, a blank row's worth of gaps: the block A1:B6 and a separate cell D9.
void FillSample(Sheet& s) {
  s.SetValue(0, 0, Value::Text("Name"));
  s.SetValue(0, 1, Value::Text("Amount"));
  const char* names[] = {"Tea", "coffee", "Tea", nullptr, "Water"};
  const double amounts[] = {2.5, 10, 3, 7, 1};
  for (uint32_t r = 0; r < 5; ++r) {
    if (names[r]) s.SetValue(r + 1, 0, Value::Text(names[r]));
    s.SetValue(r + 1, 1, Value::Number(amounts[r]));
  }
  s.SetValue(8, 3, Value::Text("apart"));
}

}  // namespace

TEST(SheetFilter_ValuesAndRows) {
  Workbook book;
  Sheet& s = book.AddSheet("Sheet1");
  FillSample(s);
  const NumLocale& us = EnUsLocale();
  const Range ref{0, 0, 5, 1};
  bool blanks = false;
  const auto names = data::ColumnValues(book, s, ref, 0, us, blanks);
  CHECK(names == std::vector<std::wstring>({L"coffee", L"Tea", L"Water"}));  // once each, without case
  CHECK(blanks);
  const auto amounts = data::ColumnValues(book, s, ref, 1, us, blanks);
  CHECK(amounts == std::vector<std::wstring>({L"1", L"2.5", L"3", L"7", L"10"}));  // by value
  CHECK(!blanks);

  data::FilterCriteria tea;
  tea.listed = true;
  tea.values = {L"TEA"};
  CHECK(data::FilterRows(book, s, ref, {tea}, us) == std::vector<bool>({true, false, true, false, false}));
  tea.blanks = true;
  CHECK(data::FilterRows(book, s, ref, {tea}, us) == std::vector<bool>({true, false, true, true, false}));

  data::FilterCriteria big;
  big.column = 1;
  big.conditions = {{data::FilterCondition::Op::GreaterEqual, L"3"}, {data::FilterCondition::Op::Less, L"10"}};
  big.both = true;
  CHECK(data::FilterRows(book, s, ref, {big}, us) == std::vector<bool>({false, false, true, true, false}));
  big.both = false;  // >= 3 or < 10: everything
  CHECK(data::FilterRows(book, s, ref, {big}, us) == std::vector<bool>({true, true, true, true, true}));
  // Both columns' criteria at once.
  tea.blanks = false;
  big.conditions = {{data::FilterCondition::Op::Greater, L"2.5"}};
  CHECK(data::FilterRows(book, s, ref, {tea, big}, us) == std::vector<bool>({false, false, true, false, false}));
}

TEST(SheetFilter_Conditions) {
  using Op = data::FilterCondition::Op;
  const Value text = Value::Text("Coffee beans");
  CHECK(data::Passes(text, L"Coffee beans", {Op::Equal, L"coffee*"}));
  CHECK(data::Passes(text, L"Coffee beans", {Op::Equal, L"*BEAN?"}));
  CHECK(!data::Passes(text, L"Coffee beans", {Op::Equal, L"coffee"}));
  CHECK(data::Passes(text, L"Coffee beans", {Op::NotEqual, L"tea*"}));
  CHECK(data::Passes(Value::Text("a*b"), L"a*b", {Op::Equal, L"a~*b"}));
  CHECK(!data::Passes(Value::Text("axb"), L"axb", {Op::Equal, L"a~*b"}));
  CHECK(data::Passes(text, L"Coffee beans", {Op::Greater, L"b"}));
  CHECK(!data::Passes(text, L"Coffee beans", {Op::Greater, L"2"}));  // text isn't compared with numbers
  CHECK(data::Passes(Value::Number(12), L"12", {Op::Equal, L"12"}));
  CHECK(data::Passes(Value::Number(12), L"12", {Op::Equal, L"1*"}));  // a pattern compares the text shown
}

// Top 10 and above/below average, with the rows Excel 2021 kept (AutoFilter xlTop10Items,
// xlTop10Percent, xlBottom10..., xlFilterDynamic; scratchpad top10_probe.py).
TEST(SheetFilter_TopAndAverage) {
  using Special = data::FilterCriteria::Special;
  // One column: a header, then `values` (NaN: blank, -1e300: the text "x"); the values kept.
  auto run = [](std::vector<double> values, Special special, double count, bool bottom, bool percent) {
    Workbook book;
    Sheet& s = book.AddSheet("S");
    s.SetValue(0, 0, Value::Text("H"));
    for (size_t i = 0; i < values.size(); ++i) {
      const uint32_t row = static_cast<uint32_t>(i + 1);
      if (values[i] == -1e300) s.SetValue(row, 0, Value::Text("x"));
      else if (!std::isnan(values[i])) s.SetValue(row, 0, Value::Number(values[i]));
    }
    data::FilterCriteria c;
    c.special = special, c.count = count, c.bottom = bottom, c.percent = percent;
    const auto keep = data::FilterRows(book, s, Range{0, 0, static_cast<uint32_t>(values.size()), 0}, {c}, EnUsLocale());
    std::vector<double> kept;
    for (size_t i = 0; i < keep.size(); ++i)
      if (keep[i]) kept.push_back(values[i]);
    return kept;
  };
  using V = std::vector<double>;
  const double nan = std::nan(""), text = -1e300;
  const V seven{1, 2, 3, 4, 5, 6, 7};
  CHECK(run(seven, Special::Top10, 3, false, false) == V({5, 6, 7}));
  CHECK(run({5, 7, 7, 6, 7, 1, 2}, Special::Top10, 2, false, false) == V({7, 7, 7}));  // ties stay
  CHECK(run(seven, Special::Top10, 2, true, false) == V({1, 2}));
  CHECK(run(seven, Special::Top10, 10, false, true) == V({7}));  // 0.7 items: one
  CHECK(run(seven, Special::Top10, 30, false, true) == V({6, 7}));
  CHECK(run(seven, Special::Top10, 50, false, true) == V({5, 6, 7}));
  CHECK(run(seven, Special::Top10, 60, false, true) == V({4, 5, 6, 7}));
  CHECK(run(seven, Special::Top10, 30, true, true) == V({1, 2}));
  CHECK(run({1, text, 3, 9, nan, 5, 7}, Special::Top10, 2, false, false) == V({9, 7}));
  CHECK(run({1, 2, 3}, Special::Top10, 5, false, false) == V({1, 2, 3}));
  CHECK(run({1, 2, 3, 4, 10}, Special::AboveAverage, 0, false, false) == V({10}));
  CHECK(run({1, 2, 3, 4, 10}, Special::BelowAverage, 0, false, false) == V({1, 2, 3}));
  CHECK(run({1, text, 3, nan, 10, 4}, Special::AboveAverage, 0, false, false) == V({10}));
  CHECK(run({1, text, 3, nan, 10, 4}, Special::BelowAverage, 0, false, false) == V({1, 3, 4}));
  CHECK(run({2, 2, 2}, Special::AboveAverage, 0, false, false).empty());
}

TEST(SheetFilter_Duplicates) {
  Workbook book;
  Sheet& s = book.AddSheet("Sheet1");
  FillSample(s);
  s.SetValue(4, 0, Value::Text("TEA"));  // row 5: TEA, 7
  s.SetValue(6, 0, Value::Text("tea"));  // row 7: tea, 3 (Tea, 3 is row 4)
  s.SetValue(6, 1, Value::Number(3));
  const NumLocale& us = EnUsLocale();
  const Range ref{0, 0, 6, 1};
  // By name: rows 4, 5 and 7 repeat Tea (without case).
  CHECK(data::DuplicateRows(book, s, ref, {0}, true, us) == std::vector<bool>({false, false, true, true, false, true}));
  // By name and amount: only row 7 repeats row 4.
  CHECK(data::DuplicateRows(book, s, ref, {0, 1}, true, us) == std::vector<bool>({false, false, false, false, false, true}));
}

TEST(SheetFilter_CurrentRegion) {
  Workbook book;
  Sheet& s = book.AddSheet("Sheet1");
  FillSample(s);
  const Range r = data::CurrentRegion(s, CellAddr{2, 1});
  CHECK(r.firstRow == 0 && r.firstCol == 0 && r.lastRow == 5 && r.lastCol == 1);
  const Range apart = data::CurrentRegion(s, CellAddr{8, 3});
  CHECK(apart.firstRow == 8 && apart.lastRow == 8 && apart.firstCol == 3 && apart.lastCol == 3);
  // A cell touching diagonally belongs to the region.
  s.SetValue(6, 2, Value::Text("x"));
  const Range grown = data::CurrentRegion(s, CellAddr{0, 0});
  CHECK(grown.lastRow == 6 && grown.lastCol == 2);
}
