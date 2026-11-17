// Formula entry in the region (decision 17), against Excel 2021 on an en-NL PC.

#include <cstdio>
#include <cstring>
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

// --- what the grid needs while a formula is typed (sheet/calc/Typing.h) ---

#include "sheet/calc/Typing.h"

TEST(sheet_typing_reference_spans) {
  const FormulaLocale nl = MakeFormulaLocale(EnNlLocale());
  std::wstring f = L"=SUM(A1:B2;Sheet2!C3)+'My Sheet'!$D$4+a1:b2+1,5+SUM(1:3;B:C)+\"A1\"+Rate+IF(1;2)";
  std::vector<RefSpan> spans = ReferenceSpans(f, nl);
  CHECK_EQ(spans.size(), size_t{6});
  if (spans.size() == 6) {
    CHECK(f.substr(spans[0].begin, spans[0].end - spans[0].begin) == L"A1:B2");
    CHECK_EQ(spans[0].r2, 1);
    CHECK_EQ(spans[0].c2, 1);
    CHECK(spans[1].sheet == L"Sheet2");
    CHECK(f.substr(spans[2].begin, spans[2].end - spans[2].begin) == L"'My Sheet'!$D$4");
    CHECK(spans[2].sheet == L"My Sheet");
    CHECK_EQ(spans[3].color, spans[0].color);  // a1:b2 is A1:B2 again
    CHECK_EQ(spans[4].r1, 0);
    CHECK_EQ(spans[4].r2, 2);
    CHECK_EQ(spans[4].c2, 16383);
    CHECK_EQ(spans[5].r2, 1048575);
    CHECK_EQ(spans[5].c1, 1);
    CHECK_EQ(spans[4].color, 3);
  }
  CHECK(ReferenceSpans(L"A1+B1", nl).empty());  // not a formula
  CHECK_EQ(ReferenceSpans(L"=SUM(A1;B", nl).size(), size_t{1});  // being typed
}

TEST(sheet_typing_point_mode) {
  const FormulaLocale nl = MakeFormulaLocale(EnNlLocale());
  CHECK(CanPointAt(L"=", 1, nl));
  CHECK(CanPointAt(L"=SUM(", 5, nl));
  CHECK(CanPointAt(L"=SUM(A1;", 8, nl));
  CHECK(CanPointAt(L"=A1+ ", 5, nl));
  CHECK(CanPointAt(L"=A1:", 4, nl));
  CHECK(CanPointAt(L"=SUM()", 5, nl));
  CHECK(!CanPointAt(L"=A1", 3, nl));
  CHECK(!CanPointAt(L"=SUM(1,", 7, nl));  // a decimal comma here
  CHECK(!CanPointAt(L"=\"a+", 4, nl));    // in text
  CHECK(!CanPointAt(L"abc+", 4, nl));     // not a formula
  CHECK(!CanPointAt(L"=+B1", 2, nl));     // B1 follows
  CHECK(CanPointAt(L"-", 1, nl));
}

TEST(sheet_typing_f4) {
  const FormulaLocale nl = MakeFormulaLocale(EnNlLocale());
  std::wstring t = L"=A1+B2";
  size_t a = 3, b = 3;
  const wchar_t* cycle[] = {L"=$A$1+B2", L"=A$1+B2", L"=$A1+B2", L"=A1+B2"};
  for (const wchar_t* next : cycle) {
    CHECK(CycleReference(t, a, b, nl));
    CHECK(t == next);
    CHECK_EQ(a, t.find(L'+'));
    CHECK_EQ(b, a);
  }
  t = L"=SUM(Sheet2!A1:B2;C3)";
  a = 7, b = 7;
  CHECK(CycleReference(t, a, b, nl));
  CHECK(t == L"=SUM(Sheet2!$A$1:$B$2;C3)");
  a = 0, b = t.size();  // the whole text selected: every reference
  CHECK(CycleReference(t, a, b, nl));
  CHECK(t == L"=SUM(Sheet2!A$1:B$2;C$3)");
  t = L"=SUM(A:B)";
  a = b = 6;
  CHECK(CycleReference(t, a, b, nl));
  CHECK(t == L"=SUM($A:$B)");
  t = L"=SUM(1;2)";
  a = b = 6;
  CHECK(!CycleReference(t, a, b, nl));
}

TEST(sheet_typing_call_at) {
  const FormulaLocale nl = MakeFormulaLocale(EnNlLocale());
  std::wstring fn;
  int arg = -1;
  std::wstring t = L"=SUM(1;IF(A1;\"x;y\";";
  CHECK(CallAt(t, t.size(), nl, fn, arg));
  CHECK(fn == L"IF");
  CHECK_EQ(arg, 2);
  CHECK(CallAt(t, 7, nl, fn, arg));
  CHECK(fn == L"SUM");
  CHECK_EQ(arg, 1);
  CHECK(CallAt(L"=SUM(1;(A1;B1);", 15, nl, fn, arg));
  CHECK(fn == L"SUM");
  CHECK_EQ(arg, 2);
  CHECK(!CallAt(L"=1+2", 4, nl, fn, arg));
  CHECK(CallAt(L"=_xlfn.STDEV.S(", 15, nl, fn, arg));
  CHECK(fn == L"_xlfn.STDEV.S");
}

TEST(sheet_typing_copy_formula) {
  // As Excel copies them (Range.Copy, measured).
  EntryNames names;
  auto at = [](const char* a1) {
    CompileOptions o;
    std::string s(a1);
    size_t k = 0;
    int32_t col = 0;
    while (k < s.size() && s[k] >= 'A' && s[k] <= 'Z') col = col * 26 + (s[k++] - 'A' + 1);
    o.col = col - 1;
    o.row = std::stoi(s.substr(k)) - 1;
    return o;
  };
  struct {
    const char *formula, *from, *to, *copied;
  } cases[] = {
      {"=A1+$B$2+B$1+$A2", "B2", "C3", "=B2+$B$2+C$1+$A3"},
      {"=A1", "A2", "A1", "=#REF!"},
      {"=SUM(A1:A3)", "B2", "B1", "=SUM(#REF!)"},
      {"=SUM(A2:A3)", "B2", "B1", "=SUM(A1:A2)"},
      {"=Sheet2!A1", "B2", "B1", "=Sheet2!#REF!"},
      {"=$A$1+A1", "B2", "A2", "=$A$1+#REF!"},
      {"=SUM(A:A)", "B2", "C2", "=SUM(B:B)"},
      {"=SUM(1:1)", "B2", "B3", "=SUM(2:2)"},
      {"=SUM(A:A)", "B2", "A2", "=SUM(#REF!)"},
      {"=XFD1", "A1", "B1", "=#REF!"},
      {"=A1048576", "A1", "A2", "=#REF!"},
      {"=SUM(A1:B2 B1:C3)", "D4", "D3", "=SUM(#REF! #REF!)"},
      {"=(A1,A2)", "B2", "B1", "=(#REF!,A1)"},
      {"=IF(A1>0,A1,B1)", "C2", "C1", "=IF(#REF!>0,#REF!,#REF!)"},
      {"=A1+1", "B2", "B4", "=A3+1"},
  };
  for (const auto& k : cases) {
    std::string got = CopyFormula(k.formula, at(k.from), at(k.to), names);
    if (got != k.copied) std::printf("    %s %s->%s: %s, Excel %s\n", k.formula, k.from, k.to, got.c_str(), k.copied);
    CHECK_EQ(got, std::string(k.copied));
  }
}

#include "sheet/calc/Functions.h"

TEST(sheet_typing_function_help) {
  // Every function the engine knows has help, A to Z, with as many required arguments as it takes.
  size_t count = 0;
  const FunctionHelp* list = FunctionHelpList(count);
  for (size_t i = 1; i < count; ++i) CHECK(std::strcmp(list[i - 1].name, list[i].name) < 0);
  size_t covered = 0;
  for (int id = 0; id < FunctionCount(); ++id) {
    const FuncInfo& fn = Function(id);
    if (fn.name[0] == '_') continue;  // _xlfn._LONGTEXT: files only
    const FunctionHelp* h = FindFunctionHelp(fn.name);
    if (!h) {
      std::printf("    no help for %s\n", fn.name);
      CHECK(h != nullptr);
      continue;
    }
    ++covered;
    int required = 0, listed = 0;
    std::string args = h->args;
    for (size_t a = 0; a < args.size();) {
      size_t b = args.find(',', a);
      std::string one = args.substr(a, b == std::string::npos ? std::string::npos : b - a);
      while (!one.empty() && one[0] == ' ') one.erase(0, 1);
      if (!one.empty() && one != "...") {
        ++listed;
        if (one[0] != '[') ++required;
      }
      if (b == std::string::npos) break;
      a = b + 1;
    }
    const bool repeats = args.find("...") != std::string::npos;
    if (required > fn.minArgs || (!repeats && listed != fn.maxArgs) || (repeats && listed > fn.maxArgs))
      std::printf("    %s: help %d required, %d listed; takes %d to %d\n", fn.name, required, listed, fn.minArgs,
                  fn.maxArgs);
    CHECK(required <= fn.minArgs);
    CHECK(repeats ? listed <= fn.maxArgs : listed == fn.maxArgs);
  }
  CHECK_EQ(covered, count);
  CHECK(FindFunctionHelp("_xlfn.stdev.s") != nullptr);
  CHECK(FindFunctionHelp("NOSUCH") == nullptr);
  size_t b = 0, e = 0;
  const FormulaLocale nl = MakeFormulaLocale(EnNlLocale());
  std::wstring tip = ArgumentTip(*FindFunctionHelp("SUM"), nl, 4, b, e);
  CHECK(tip == L"SUM(number1; [number2]; ...)");
  CHECK(tip.substr(b, e - b) == L"[number2]");
  tip = ArgumentTip(*FindFunctionHelp("VLOOKUP"), nl, 2, b, e);
  CHECK(tip.substr(b, e - b) == L"col_index_num");
}

#include <map>

TEST(sheet_typing_autosum) {
  struct Sheet {
    std::map<std::pair<int32_t, int32_t>, SumCell> cells;
    void Set(const char* a1, SumCell k) {
      int32_t col = 0;
      size_t i = 0;
      while (a1[i] >= 'A' && a1[i] <= 'Z') col = col * 26 + (a1[i++] - 'A' + 1);
      cells[{std::atoi(a1 + i) - 1, col - 1}] = k;
    }
    SumCell operator()(int32_t r, int32_t c) const {
      auto it = cells.find({r, c});
      return it == cells.end() ? SumCell::kEmpty : it->second;
    }
  };
  auto propose = [](const Sheet& s, int32_t row, int32_t col) {
    return ProposeAutoSum(row, col, [&](int32_t r, int32_t c) { return s(r, c); }).formula;
  };
  const SumCell N = SumCell::kNumber, T = SumCell::kTotal, X = SumCell::kOther;
  Sheet a;  // a header and numbers
  a.Set("B1", X), a.Set("B2", N), a.Set("B3", N), a.Set("B4", N);
  CHECK_EQ(propose(a, 4, 1), std::string("=SUM(B2:B4)"));
  CHECK_EQ(propose(a, 6, 1), std::string("=SUM(B2:B6)"));  // blank cells right above
  Sheet b;  // a row to the left
  b.Set("A5", X), b.Set("B5", N), b.Set("C5", N), b.Set("D5", N);
  CHECK_EQ(propose(b, 4, 4), std::string("=SUM(B5:D5)"));
  Sheet c;  // numbers above and to the left: the column
  c.Set("C1", N), c.Set("C2", N), c.Set("A3", N), c.Set("B3", N);
  CHECK_EQ(propose(c, 2, 2), std::string("=SUM(C1:C2)"));
  Sheet d;  // subtotals
  d.Set("B2", N), d.Set("B3", N), d.Set("B4", T), d.Set("B5", N), d.Set("B6", N), d.Set("B7", T);
  CHECK_EQ(propose(d, 7, 1), std::string("=SUM(B7,B4)"));
  Sheet e;  // text in the middle
  e.Set("B2", N), e.Set("B3", X), e.Set("B4", N);
  CHECK_EQ(propose(e, 4, 1), std::string("=SUM(B4)"));
  CHECK_EQ(propose(Sheet{}, 4, 1), std::string("=SUM()"));
  Sheet f;  // the sheet's top
  f.Set("B1", N), f.Set("B2", N);
  CHECK_EQ(propose(f, 2, 1), std::string("=SUM(B1:B2)"));
}
