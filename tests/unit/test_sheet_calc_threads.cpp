// Multi-threaded recalculation (S4): a workbook mixing what runs in parallel (wide independent
// levels, ranges, text results) with what must not (a long chain, volatile functions, OFFSET, a
// circular reference, a spill) gives every cell the same value, bit for bit, on one thread and on
// four. The corpus is compared the same way by `fwtool formulas --threads` (ctest).
#include "TestHarness.h"

#include <cstring>
#include <string>

#include "SheetTestUtil.h"
#include "sheet/calc/Bind.h"
#include "sheet/calc/Engine.h"

using namespace fw;
using namespace fw::sheet;

namespace {

constexpr int kRows = 1500;

std::string Cell(const std::string& ref, const std::string& formula) {
  return "<c r=\"" + ref + "\"><f>" + formula + "</f></c>";
}

std::string Mixed() {
  std::string x = "<sheetData>";
  for (int r = 1; r <= kRows; ++r) {
    const std::string n = std::to_string(r), up = std::to_string(r - 1);
    x += "<row r=\"" + n + "\"><c r=\"A" + n + "\"><v>" + std::to_string((r * 37) % 101 - 50) + ".25</v></c>";
    x += Cell("B" + n, r == 1 ? "A1*2" : "B" + up + "+A" + n);                    // a chain
    x += Cell("C" + n, "A" + n + "*3+1");                                            // a wide level
    x += Cell("D" + n, "C" + n + "/7+B" + n);                                        // on both
    x += Cell("E" + n, "C" + n + "*RAND()");                                         // volatile
    x += Cell("F" + n, "OFFSET($C$1," + up + ",0)*2");                               // dynamic refs
    x += Cell("G" + n, "SUM($C$1:C" + n + ")-D" + n);                                // growing ranges
    x += Cell("H" + n, "\"x\"&amp;TEXT(C" + n + ",\"0.00\")");                       // text
    x += Cell("I" + n, "IF(A" + n + "&gt;0,SQRT(A" + n + "),1/0)");                  // errors
    x += Cell("J" + n, "E" + n + "+F" + n + "+G" + n);                               // after the excluded
    if (r == 1) x += Cell("K1", "L1+1") + Cell("L1", "K1+1");                        // circular
    if (r == 2) x += Cell("K2", "SUM(_xlfn.SEQUENCE(5)*C2)");                        // an array
    x += "</row>";
  }
  return x + "</sheetData>";
}

struct Fixed : calc::WorkbookCalc {
  using WorkbookCalc::WorkbookCalc;
  double Now() override { return 46000.5; }
  double Random() override { return 0.25; }
};

size_t Full(Workbook& book, int threads) {
  Fixed c(book);
  c.Load();
  c.engine().SetThreads(threads);
  c.engine().SetParallelThresholds(1, 1);
  c.engine().Opened(c.Settings());
  c.engine().MarkAllDirty();
  c.engine().Recalculate();
  return c.engine().ParallelEvaluations();
}

bool Same(const Value& x, const Value& y) {
  return x.type == y.type && !std::memcmp(&x.number, &y.number, sizeof x.number) && x.boolean == y.boolean &&
         x.error == y.error && x.text == y.text;
}

}  // namespace

TEST(sheet_calc_threads_same_values) {
  auto one = sheet_test::OpenParts({Mixed()});
  auto four = sheet_test::OpenParts({Mixed()});
  if (!one || !four) return;
  Full(*one, 1);
  CHECK(Full(*four, 4) > size_t{2 * kRows});  // C, D, G, H, I at least
  const Sheet& a = one->GetSheet(0);
  const Sheet& b = four->GetSheet(0);
  size_t differ = 0, numbers = 0;
  for (uint32_t r = 0; r < kRows; ++r)
    for (uint32_t col = 0; col < 12; ++col) {
      const Value x = a.GetValue(r, col), y = b.GetValue(r, col);
      if (!Same(x, y)) ++differ;
      numbers += x.type == ValueType::Number;
    }
  CHECK_EQ(differ, size_t{0});
  CHECK(numbers > size_t{6 * kRows});
  // Spot checks against what the formulas say.
  CHECK_EQ(std::string(a.GetValue(0, 7).text).substr(0, 1), "x");
  CHECK(a.GetValue(0, 2).type == ValueType::Number);
  CHECK_NEAR(a.GetValue(4, 4).number, a.GetValue(4, 2).number * 0.25, 1e-12);
}
