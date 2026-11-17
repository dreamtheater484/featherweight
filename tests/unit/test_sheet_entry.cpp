// Formula entry in the region (decision 17), against Excel 2021 on an en-NL PC.

#include <cstdio>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "sheet/calc/Entry.h"
#include "sheet/numfmt/NumLocale.h"

using namespace fw::sheet;
using namespace fw::sheet::calc;

namespace {

bool SameNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

class EntryNames : public Names {
 public:
  std::vector<std::string> sheets{"Sheet1", "Sheet2", "My Sheet"};
  std::vector<std::string> slots;
  int FindSheet(std::string_view n) const override {
    for (size_t i = 0; i < sheets.size(); ++i)
      if (SameNoCase(sheets[i], n)) return static_cast<int>(i);
    return -1;
  }
  std::string SheetName(int s) const override { return sheets[static_cast<size_t>(s)]; }
  uint32_t NameSlot(std::string_view n) override {
    for (size_t i = 0; i < slots.size(); ++i)
      if (SameNoCase(slots[i], n)) return static_cast<uint32_t>(i);
    slots.emplace_back(n);
    return static_cast<uint32_t>(slots.size() - 1);
  }
  std::string SlotName(uint32_t s) const override { return slots[s]; }
};

struct EntryCase {
  const char* typed;
  bool ok;
  const char* invariant;  // Range.Formula
  const char* local;      // Range.FormulaLocal
};
const EntryCase kEntryCases[] = {
#include "formula_entry_cases.inc"
};

}  // namespace

TEST(sheet_entry_separators) {
  const FormulaLocale nl = MakeFormulaLocale(EnNlLocale());
  CHECK_EQ(nl.decimal, ',');
  CHECK_EQ(nl.list, ';');
  CHECK_EQ(nl.arrayCol, '\\');
  CHECK_EQ(nl.arrayRow, ';');
  const FormulaLocale us = MakeFormulaLocale(EnUsLocale());
  CHECK_EQ(us.decimal, '.');
  CHECK_EQ(us.list, ',');
  CHECK_EQ(us.arrayCol, ',');

  std::string out;
  size_t pos = 0;
  CHECK(ToInvariant("=SUM(A1;2,5)", nl, out, &pos));
  CHECK_EQ(out, std::string("=SUM(A1,2.5)"));
  CHECK(ToInvariant("=\"a;b,c\"&'x;y'!A1&Table1[[#This Row];[a;b]]", nl, out));
  CHECK_EQ(out, std::string("=\"a;b,c\"&'x;y'!A1&Table1[[#This Row],[a;b]]"));
  CHECK(!ToInvariant("=SUM(A1,B1)", nl, out, &pos));
  CHECK_EQ(pos, size_t{7});
  CHECK(!ToInvariant("=1.5", nl, out, &pos));
  CHECK_EQ(pos, size_t{2});
  CHECK(ToInvariant("=STDEV.S(1;2)", nl, out));
  CHECK_EQ(ToLocal("=SUM({1.5,2;3,4},STDEV.S(0.5))", nl), std::string("=SUM({1,5\\2;3\\4};STDEV.S(0,5))"));
  CHECK_EQ(ToLocal("=SUM(1,2.5)", us), std::string("=SUM(1,2.5)"));
}

TEST(sheet_entry_excel_cases) {
  // Typed as the region writes it: taken or refused as Excel does, stored as Excel stores it,
  // shown back as Excel shows it. Excel keeps the spaces typed between tokens; the engine doesn't
  // keep them (the formula means the same).
  const FormulaLocale nl = MakeFormulaLocale(EnNlLocale());
  auto noSpaces = [](std::string s) {
    std::string out;
    bool text = false;
    for (char c : s) {
      if (c == '"') text = !text;
      if (c != ' ' || text) out += c;
    }
    return out;
  };
  size_t failures = 0, n = 0;
  for (const EntryCase& k : kEntryCases) {
    ++n;
    EntryNames names;
    CompileOptions at;
    at.row = static_cast<int32_t>(n - 1), at.col = 25;  // Z1, Z2...
    EnteredFormula e = EnterFormula(k.typed, nl, at, names);
    bool good = (e.formula != nullptr) == k.ok;
    std::string invariant;
    if (good && k.ok) {
      invariant = ShowFormula(*e.formula, at, names, InvariantFormulaLocale());
      bool spaced = std::string_view(k.invariant).find(' ') != std::string_view::npos;
      good = invariant == k.invariant || (spaced && noSpaces(invariant) == noSpaces(k.invariant));
      good = good && (e.shown == k.local || (spaced && noSpaces(e.shown) == noSpaces(k.local)));
      // Typed in the invariant form (en-US), the same formula.
      EnteredFormula us = EnterFormula(k.invariant, InvariantFormulaLocale(), at, names);
      good = good && us.formula && us.text == e.text;
    }
    if (!good) {
      ++failures;
      std::printf("    %s: Excel %s %s, here %s %s%s\n", k.typed, k.ok ? "takes" : "refuses", k.invariant,
                  e.formula ? "takes" : "refuses", e.formula ? invariant.c_str() : e.message.c_str(),
                  e.formula ? (" shown " + e.shown).c_str() : "");
    }
  }
  std::printf("    %zu of %zu as Excel\n", n - failures, n);
  CHECK_EQ(failures, size_t{0});
}

TEST(sheet_entry_messages) {
  const FormulaLocale nl = MakeFormulaLocale(EnNlLocale());
  EntryNames names;
  EnteredFormula e = EnterFormula("=ROUND(1)", nl, {}, names);
  CHECK(!e.formula);
  CHECK_EQ(e.message, std::string("You've entered too few arguments for this function."));
  e = EnterFormula("=PI(1)", nl, {}, names);
  CHECK_EQ(e.message, std::string("You've entered too many arguments for this function."));
  e = EnterFormula("=1.5+2", nl, {}, names);
  CHECK_EQ(e.message, std::string("There's a problem with this formula."));
  CHECK_EQ(e.pos, size_t{2});
  e = EnterFormula("=SUM(1;STDEV.S(2;3", nl, {}, names);
  CHECK(e.formula != nullptr);
  CHECK_EQ(e.text, std::string("=SUM(1,_xlfn.STDEV.S(2,3))"));
  CHECK_EQ(e.shown, std::string("=SUM(1;STDEV.S(2;3))"));
}
