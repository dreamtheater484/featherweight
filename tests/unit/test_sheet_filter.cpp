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
