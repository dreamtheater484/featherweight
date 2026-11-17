#include <string>
#include <vector>

#include "TestHarness.h"
#include "sheet/data/Sort.h"

using namespace fw::sheet;

namespace {

// The column Excel sorted (sortorder_probe.py on the reference PC), top to bottom.
void FillColumn(Sheet& s) {
  const char* const texts[] = {nullptr, "b", "B", "a", "A", "ab", "a-b", "a b", "a'c", "ac", "\xC3\xA9", "e", "f", "Z",
                               "10", "9", "_x", "-x", "1a", ""};
  s.SetValue(0, 0, Value::Number(10));
  for (uint32_t r = 1; r < 20; ++r) s.SetValue(r, 0, Value::Text(texts[r]));
  s.SetValue(20, 0, Value::Bool(true));
  s.SetValue(21, 0, Value::Bool(false));
  s.SetValue(22, 0, Value::Error(ErrorCode::NA));
  s.SetValue(23, 0, Value::Error(ErrorCode::Div0));
  s.SetValue(24, 0, Value::Number(-5));
  s.SetValue(25, 0, Value::Number(2.5));
  s.SetValue(26, 0, Value::Number(0));
  // row 27 blank
  const char* const more[] = {"\xC3\x84", "\xC3\xA4", "a", " a", "zz", "\xC3\x86", "\xC3\x9F", "ss", "#N/A", "TRUE"};
  for (uint32_t k = 0; k < 10; ++k) s.SetValue(28 + k, 0, Value::Text(more[k]));
}

}  // namespace

// Excel's order, ascending and descending, for numbers, text, booleans, errors and a blank.
TEST(SheetSort_ExcelOrder) {
  Workbook book;
  Sheet& s = book.AddSheet("Sheet1");
  FillColumn(s);
  const std::vector<uint32_t> up = {25, 27, 26, 1, 20, 32, 37, 17, 15, 19, 16, 4, 5, 31, 29, 30, 8, 6, 7,
                                    10, 9, 34, 2, 3, 12, 11, 13, 35, 36, 38, 18, 14, 33, 22, 21, 23, 24, 28};
  const std::vector<uint32_t> down = {23, 24, 21, 22, 33, 14, 18, 38, 35, 36, 13, 11, 12, 2, 3, 34, 9, 10, 7,
                                      6, 8, 29, 30, 4, 5, 31, 16, 19, 15, 17, 37, 32, 20, 1, 26, 27, 25, 28};
  for (bool descending : {false, true}) {
    const std::vector<uint32_t> got = data::SortOrder(s, Range{0, 0, 37, 0}, {{0, descending}});
    const std::vector<uint32_t>& want = descending ? down : up;
    int wrong = 0;
    for (size_t i = 0; i < got.size(); ++i)
      if (got[i] + 1 != want[i] && ++wrong <= 10)
        std::printf("  %s position %zu: want row %u, got %u\n", descending ? "down" : "up", i, want[i], got[i] + 1);
    CHECK_EQ(wrong, 0);
  }
}

TEST(SheetSort_KeysAndHeader) {
  Workbook book;
  Sheet& s = book.AddSheet("Sheet1");
  s.SetValue(0, 0, Value::Text("Name"));
  s.SetValue(0, 1, Value::Text("Age"));
  const char* names[] = {"Bob", "ann", "Bob", "Cy"};
  const double ages[] = {30, 25, 20, 40};
  for (uint32_t r = 0; r < 4; ++r) {
    s.SetValue(r + 1, 0, Value::Text(names[r]));
    s.SetValue(r + 1, 1, Value::Number(ages[r]));
  }
  CHECK(data::GuessHeader(book, s, Range{0, 0, 4, 1}));    // text over numbers
  CHECK(!data::GuessHeader(book, s, Range{1, 0, 4, 1}));   // the data alone
  // By name, then age descending: ann, Bob 30, Bob 20, Cy.
  const auto order = data::SortOrder(s, Range{1, 0, 4, 1}, {{0, false}, {1, true}});
  CHECK(order == std::vector<uint32_t>({2, 1, 3, 4}));
  // Left to right by row 2 (the ages' row): columns sorted as values in that row.
  const auto cols = data::SortOrder(s, Range{0, 0, 4, 1}, {{2, false}}, true);
  CHECK(cols == std::vector<uint32_t>({1, 0}));  // 25 before "ann"
}
