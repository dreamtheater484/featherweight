#include <string>

#include "TestHarness.h"
#include "sheet/numfmt/FormatCells.h"
#include "sheet/numfmt/NumFormat.h"

using namespace fw::sheet;

TEST(SheetFormatCells_Codes) {
  const NumLocale& us = EnUsLocale();
  FormatChoice c;
  c.category = FormatCategory::Number, c.decimals = 2, c.thousands = true;
  CHECK(FormatCode(c, us) == L"#,##0.00");
  c.negative = 3;
  CHECK(FormatCode(c, us) == L"#,##0.00_);[Red](#,##0.00)");
  c = FormatChoice{};
  c.category = FormatCategory::Currency, c.decimals = 2, c.negative = 2;
  CHECK(FormatCode(c, us) == L"\"$\"#,##0.00_);(\"$\"#,##0.00)");  // built-in 7, as Excel stores it
  c.negative = 0, c.decimals = 0;
  CHECK(FormatCode(c, us) == L"\"$\"#,##0");
  c = FormatChoice{};
  c.category = FormatCategory::Percentage, c.decimals = 1;
  CHECK(FormatCode(c, us) == L"0.0%");
  c.category = FormatCategory::Scientific, c.decimals = 3;
  CHECK(FormatCode(c, us) == L"0.000E+00");
  CHECK(FormatChoiceOf(L"#,##0.00", us).category == FormatCategory::Number);
  CHECK(FormatChoiceOf(L"0.000 \"kg\"", us).category == FormatCategory::Custom);
  CHECK(FormatChoiceOf(L"General", us).category == FormatCategory::General);
  CHECK(FormatChoiceOf(L"@", us).category == FormatCategory::Text);
  CHECK(FormatChoiceOf(L"[h]:mm:ss", us).category == FormatCategory::Time);
}

// Every choice the tab offers opens the tab on itself again, in both locales, and its code reads back
// from the region's form.
TEST(SheetFormatCells_RoundTrip) {
  int failures = 0;
  for (const NumLocale* loc : {&EnUsLocale(), &EnNlLocale()}) {
    std::vector<FormatChoice> all;
    for (int d = 0; d <= 4; ++d) {
      for (int neg = 0; neg < 4; ++neg) {
        all.push_back({FormatCategory::Number, d, false, neg});
        all.push_back({FormatCategory::Number, d, true, neg});
        all.push_back({FormatCategory::Currency, d, false, neg});
      }
      all.push_back({FormatCategory::Accounting, d});
      all.push_back({FormatCategory::Percentage, d});
      all.push_back({FormatCategory::Scientific, d});
    }
    for (FormatCategory cat : {FormatCategory::Date, FormatCategory::Time, FormatCategory::Fraction})
      for (size_t k = 0; k < FormatTypes(cat, *loc).size(); ++k) all.push_back({cat, 2, false, 0, static_cast<int>(k)});
    for (const FormatChoice& x : all) {
      const std::wstring code = FormatCode(x, *loc);
      std::wstring back;
      const bool local = NumberFormat::LocalToInvariant(NumberFormat::InvariantToLocal(code, *loc), *loc, back);
      if (!(FormatChoiceOf(code, *loc) == x) || !local || back.empty()) {
        if (++failures <= 20) std::printf("  category %d decimals %d negative %d type %d: code %ls\n", static_cast<int>(x.category),
                                          x.decimals, x.negative, x.type, code.c_str());
      }
    }
  }
  CHECK_EQ(failures, 0);
}
