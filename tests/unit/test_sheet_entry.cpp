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

namespace {
struct ShiftCase {
  const char* edit;
  bool rows;
  int at, count, band1, band2, sheet;
  const char* before;
  const char* after;
};
const ShiftCase kShiftCases[] = {
#include "formula_shift_cases.inc"
};
}  // namespace

TEST(sheet_typing_structural_edits) {
  // Rows, columns and cells inserted and deleted on Sheet1: formulas on Sheet2 pointing at it and
  // on Sheet1 itself (Z30, Z31...), as Excel rewrites them.
  size_t failures = 0, n = 0;
  std::string lastEdit;
  int index = 0;
  for (const ShiftCase& k : kShiftCases) {
    if (lastEdit != k.edit) lastEdit = k.edit, index = 0;
    const int i = index++ / 2;
    EntryNames names;
    CompileOptions at;
    at.sheet = k.sheet == 0 ? 0 : 1;
    at.row = k.sheet == 0 ? 29 + i : i;
    at.col = k.sheet == 0 ? 25 : 0;
    StructuralEdit e;
    e.sheet = 0, e.rows = k.rows, e.at = k.at, e.count = k.count, e.band1 = k.band1, e.band2 = k.band2;
    std::string got = "=" + ShiftReferences(k.before + 1, at, e, names);
    ++n;
    if (got != k.after) {
      ++failures;
      std::printf("    %s, %s: Excel %s, here %s\n", k.edit, k.before, k.after, got.c_str());
    }
  }
  std::printf("    %zu of %zu as Excel\n", n - failures, n);
  CHECK_EQ(failures, size_t{0});
}

#include "sheet/edit/Edit.h"

TEST(sheet_typing_shift_from_editor) {
  // The editor's edit::Shift: whole rows (every column in the band), cells in a band of columns.
  // Rows inserted on Sheet2 move references to Sheet2 but not 3-D ranges through it (Excel).
  EntryNames names;
  names.sheets = {"Sheet1", "Sheet2", "Sheet3", "Sheet4"};
  CompileOptions at;
  at.sheet = 3;
  edit::Shift rows;
  rows.axis = edit::Shift::Axis::Rows, rows.at = 2, rows.count = 2, rows.band = Range{2, 0, 3, kMaxCols - 1};
  CHECK_EQ(ShiftReferences("SUM(Sheet1:Sheet3!A2:A5)", at, 1, rows, names), std::string("SUM(Sheet1:Sheet3!A2:A5)"));
  CHECK_EQ(ShiftReferences("Sheet2!A4+Sheet2!C2:C9", at, 1, rows, names), std::string("Sheet2!A6+Sheet2!C2:C11"));
  CHECK_EQ(ShiftReferences("Sheet1!A4", at, 1, rows, names), std::string("Sheet1!A4"));
  edit::Shift cells;  // A3:C4 deleted, cells below moving up
  cells.axis = edit::Shift::Axis::Rows, cells.at = 2, cells.count = -2, cells.band = Range{2, 0, 3, 2};
  CHECK_EQ(ShiftReferences("Sheet2!A6+Sheet2!D6+Sheet2!A3", at, 1, cells, names),
           std::string("Sheet2!A4+Sheet2!D6+Sheet2!#REF!"));
  edit::Shift cols;  // columns C:D deleted
  cols.axis = edit::Shift::Axis::Columns, cols.at = 2, cols.count = -2, cols.band = Range{0, 2, kMaxRows - 1, 3};
  CHECK_EQ(ShiftReferences("Sheet2!E1+Sheet2!B1:F1+Sheet2!C:D", at, 1, cols, names),
           std::string("Sheet2!C1+Sheet2!B1:D1+Sheet2!#REF!"));
}

TEST(sheet_typing_sheets_changed) {
  // Excel 2021 (scratchpad moves_probe.py): sheets Ka..Ke and F, formulas on F; one sheet moved
  // (MoveSheet(sheet, to): `to` its index after) or deleted at a time.
  const std::vector<std::string> sheets{"Ka", "Kb", "Kc", "Kd", "Ke", "F"};
  const char* formulas[] = {"SUM(Kb:Kd!A1)", "SUM(Ka:Ke!A1)", "SUM(Kb:Kc!A1)", "Kb!A1+Kd!A1", "SUM(Kc!A1)"};
  struct Move {
    int sheet, to;
    const char* after[5];
  };
  const Move moves[] = {
      {3, 4, {"SUM(Kb:Kd!A1)", "SUM(Ka:Ke!A1)", "SUM(Kb:Kc!A1)", "Kb!A1+Kd!A1", "SUM(Kc!A1)"}},  // Kd after Ke
      {3, 0, {"SUM(Kb:Kc!A1)", "SUM(Ka:Ke!A1)", "SUM(Kb:Kc!A1)", "Kb!A1+Kd!A1", "SUM(Kc!A1)"}},  // Kd before Ka
      {3, 2, {"SUM(Kb:Kd!A1)", "SUM(Ka:Ke!A1)", "SUM(Kb:Kc!A1)", "Kb!A1+Kd!A1", "SUM(Kc!A1)"}},  // Kd before Kc
      {1, 0, {"SUM(Kb:Kd!A1)", "SUM(Ka:Ke!A1)", "SUM(Kb:Kc!A1)", "Kb!A1+Kd!A1", "SUM(Kc!A1)"}},  // Kb before Ka
      {1, 4, {"SUM(Kc:Kd!A1)", "SUM(Ka:Ke!A1)", "SUM(Kc!A1)", "Kb!A1+Kd!A1", "SUM(Kc!A1)"}},     // Kb after Ke
      {1, 2, {"SUM(Kb:Kd!A1)", "SUM(Ka:Ke!A1)", "SUM(Kc!A1)", "Kb!A1+Kd!A1", "SUM(Kc!A1)"}},     // Kb after Kc
      {2, 0, {"SUM(Kb:Kd!A1)", "SUM(Ka:Ke!A1)", "SUM(Kb!A1)", "Kb!A1+Kd!A1", "SUM(Kc!A1)"}},     // Kc before Ka
      {2, 4, {"SUM(Kb:Kd!A1)", "SUM(Ka:Ke!A1)", "SUM(Kb:Kc!A1)", "Kb!A1+Kd!A1", "SUM(Kc!A1)"}},  // Kc after Ke
      {0, 1, {"SUM(Kb:Kd!A1)", "SUM(Ka:Ke!A1)", "SUM(Kb:Kc!A1)", "Kb!A1+Kd!A1", "SUM(Kc!A1)"}},  // Ka after Kb
      {4, 2, {"SUM(Kb:Kd!A1)", "SUM(Ka:Ke!A1)", "SUM(Kb:Kc!A1)", "Kb!A1+Kd!A1", "SUM(Kc!A1)"}},  // Ke before Kc
      {4, 1, {"SUM(Kb:Kd!A1)", "SUM(Ka:Ke!A1)", "SUM(Kb:Kc!A1)", "Kb!A1+Kd!A1", "SUM(Kc!A1)"}},  // Ke before Kb
      {0, 3, {"SUM(Kb:Kd!A1)", "SUM(Ka:Ke!A1)", "SUM(Kb:Kc!A1)", "Kb!A1+Kd!A1", "SUM(Kc!A1)"}},  // Ka after Kd
      {2, -1, {"SUM(Kb:Kd!A1)", "SUM(Ka:Ke!A1)", "SUM(Kb!A1)", "Kb!A1+Kd!A1", "SUM(#REF!A1)"}},  // Kc deleted
  };
  CompileOptions at;
  at.sheet = 5;
  for (const Move& m : moves)
    for (int i = 0; i < 5; ++i) CHECK_EQ(SheetsChanged(formulas[i], at, sheets, m.sheet, m.to), std::string(m.after[i]));
  // Two sheets deleted one after the other, as Excel deletes a selection of sheets.
  struct Twice {
    int first, second;  // the second: its index once the first is gone
    const char* after[5];
  };
  const Twice twice[] = {
      {1, 2, {"SUM(Kc!A1)", "SUM(Ka:Ke!A1)", "SUM(Kc!A1)", "#REF!A1+#REF!A1", "SUM(Kc!A1)"}},       // Kb, Kd
      {1, 1, {"SUM(Kd!A1)", "SUM(Ka:Ke!A1)", "SUM(#REF!A1)", "#REF!A1+Kd!A1", "SUM(#REF!A1)"}},     // Kb, Kc
  };
  for (const Twice& t : twice) {
    std::vector<std::string> left = sheets;
    left.erase(left.begin() + t.first);
    CompileOptions at2 = at;
    at2.sheet = 4;
    for (int i = 0; i < 5; ++i) {
      std::string once = SheetsChanged(formulas[i], at, sheets, t.first, -1);
      CHECK_EQ(SheetsChanged(once, at2, left, t.second, -1), std::string(t.after[i]));
    }
  }
  // A reference that's #REF! already, relative references and names print back as they were.
  CHECK_EQ(SheetsChanged("Kc!B2:C3*Rate+B2", at, sheets, 2, -1), std::string("#REF!B2:C3*Rate+B2"));
  CHECK_EQ(SheetsChanged("SUM(Kb:Kc!A1)", at, sheets, 5, 0), std::string("SUM(Kb:Kc!A1)"));
  // #REF!A1 (in files too) is a reference that's gone: it compiles, is #REF!, prints back.
  EntryNames names;
  for (const char* gone : {"#REF!A1+1", "SUM(#REF!$A$1:B2)", "#REF!+#REF!A:A"}) {
    auto f = Compile(gone, at, names);
    CHECK(f != nullptr);
    if (!f) continue;
    CHECK_EQ(f->flags & Formula::kUnsupported, 0);
    CHECK_EQ(Print(*f, at, names), std::string(gone));
  }
}

TEST(sheet_typing_build_from_scratch) {
  // s2-corpus's build-from-scratch scripts (tests/office/build/*.steps on claude/s2-corpus): the
  // formulas typed in en-NL, and what AutoSum must propose where the scripts press Alt+=.
  const FormulaLocale nl = MakeFormulaLocale(EnNlLocale());
  EntryNames names;
  CompileOptions at;
  for (auto [typed, text] : {std::pair{"=B7-B17", "=B7-B17"}, {"=IF(B7=0;0;B16/B7)", "=IF(B7=0,0,B16/B7)"},
                             {"=B4+30", "=B4+30"}, {"=B12*C12", "=B12*C12"}, {"=ROUND(D17*C18;2)", "=ROUND(D17*C18,2)"}}) {
    EnteredFormula e = EnterFormula(typed, nl, at, names);
    CHECK(e.formula != nullptr);
    CHECK_EQ(e.text, std::string(text));
    CHECK_EQ(e.shown, std::string(typed));
  }
  std::map<std::pair<int32_t, int32_t>, SumCell> cells;
  auto set = [&](int32_t r1, int32_t c1, int32_t r2, int32_t c2, SumCell k) {
    for (int32_t r = r1; r <= r2; ++r)
      for (int32_t c = c1; c <= c2; ++c) cells[{r, c}] = k;
  };
  auto at2 = [&](int32_t r, int32_t c) {
    auto it = cells.find({r, c});
    return it == cells.end() ? SumCell::kEmpty : it->second;
  };
  // The budget: labels in A, months B3:M3, income rows 5-6, B7 its total; expenses rows 10-16.
  set(2, 0, 2, 13, SumCell::kOther);
  set(3, 0, 19, 0, SumCell::kOther);
  set(4, 1, 5, 12, SumCell::kNumber);
  CHECK_EQ(ProposeAutoSum(6, 1, at2).formula, std::string("=SUM(B5:B6)"));
  set(6, 1, 6, 12, SumCell::kTotal);
  set(9, 1, 15, 12, SumCell::kNumber);
  CHECK_EQ(ProposeAutoSum(16, 1, at2).formula, std::string("=SUM(B10:B16)"));
  set(16, 1, 16, 12, SumCell::kTotal);
  set(18, 1, 19, 12, SumCell::kNumber);
  CHECK_EQ(ProposeAutoSum(4, 13, at2).formula, std::string("=SUM(B5:M5)"));
  set(4, 13, 6, 13, SumCell::kTotal);
  CHECK_EQ(ProposeAutoSum(9, 13, at2).formula, std::string("=SUM(B10:M10)"));
  // The invoice: header row 11, amounts D12:D15 (formulas), D16 blank, D17 the subtotal.
  cells.clear();
  set(10, 0, 10, 3, SumCell::kOther);
  set(11, 0, 14, 0, SumCell::kOther);
  set(11, 1, 14, 3, SumCell::kNumber);
  CHECK_EQ(ProposeAutoSum(16, 3, at2).formula, std::string("=SUM(D12:D16)"));
}

namespace {

// A cut of `cut` on sheet 0 pasted with its top left at (sheet `to`, +3 rows, +3 columns), as
// s2-model's MoveCells maps it: references wholly inside the cut cells go along (whole rows and
// columns stay), references to pasted-over cells on `to` are #REF!. `was` and `now` are the printed
// formula's cell before and after (relative parts are offsets from them).
class CutMap : public RefMap {
 public:
  int to = 1;
  CompileOptions was, now;
  static constexpr int32_t kR1 = 0, kC1 = 0, kR2 = 1, kC2 = 1, kDr = 3, kDc = 3;
  bool MapTo(int sheet, int& toSheet, int32_t& r1, int32_t& c1, int32_t& r2, int32_t& c2, uint8_t abs, bool wholeRows,
             bool wholeCols) const override {
    int64_t R1 = (abs & kRow1) ? r1 : int64_t{was.row} + r1, R2 = (abs & kRow2) ? r2 : int64_t{was.row} + r2;
    int64_t C1 = (abs & kCol1) ? c1 : int64_t{was.col} + c1, C2 = (abs & kCol2) ? c2 : int64_t{was.col} + c2;
    const bool cells = !wholeRows && !wholeCols;
    if (cells && sheet == 0 && R1 >= kR1 && R2 <= kR2 && C1 >= kC1 && C2 <= kC2) {
      R1 += kDr, R2 += kDr, C1 += kDc, C2 += kDc;
      toSheet = to;
    } else if (cells && sheet == to && R1 >= kR1 + kDr && R2 <= kR2 + kDr && C1 >= kC1 + kDc && C2 <= kC2 + kDc) {
      return false;
    }
    r1 = static_cast<int32_t>((abs & kRow1) ? R1 : R1 - now.row);
    r2 = static_cast<int32_t>((abs & kRow2) ? R2 : R2 - now.row);
    c1 = static_cast<int32_t>((abs & kCol1) ? C1 : C1 - now.col);
    c2 = static_cast<int32_t>((abs & kCol2) ? C2 : C2 - now.col);
    return true;
  }
  bool Map(int sheet, int32_t& r1, int32_t& c1, int32_t& r2, int32_t& c2, uint8_t abs, bool wholeRows,
           bool wholeCols) const override {
    int t = sheet;
    return MapTo(sheet, t, r1, c1, r2, c2, abs, wholeRows, wholeCols);
  }
};

}  // namespace

TEST(sheet_typing_cut_to_another_sheet) {
  // Excel 2021 (scratchpad cut_probe.py): Sheet1!A1:B2 cut and pasted onto Sheet2!D4, then onto
  // 'My Sheet'!D4. References to the moved cells, from every sheet, follow to the new sheet with its
  // name (quoted as needed); a reference that had a sheet name keeps one even on its own sheet; a
  // moved formula's references to cells left behind get their sheet's name.
  struct Case {
    int sheet, row, col;  // the formula's cell before
    const char* formula;
    const char* after[2];  // pasted onto Sheet2, onto 'My Sheet'
  };
  const Case cases[] = {
      {0, 0, 4, "A1", {"Sheet2!D4", "'My Sheet'!D4"}},
      {0, 1, 4, "SUM(A1:B2)", {"SUM(Sheet2!D4:E5)", "SUM('My Sheet'!D4:E5)"}},
      {0, 2, 4, "SUM(A1:C3)", {"SUM(A1:C3)", "SUM(A1:C3)"}},
      {0, 3, 4, "SUM(A1:A2)", {"SUM(Sheet2!D4:D5)", "SUM('My Sheet'!D4:D5)"}},
      {0, 4, 4, "$B$2*2", {"Sheet2!$E$5*2", "'My Sheet'!$E$5*2"}},
      {0, 5, 4, "SUM(A:A)", {"SUM(A:A)", "SUM(A:A)"}},
      {0, 1, 1, "A1+C1+Sheet2!Z1", {"D4+Sheet1!C1+Sheet2!Z1", "D4+Sheet1!C1+Sheet2!Z1"}},  // moved to E5
      {0, 0, 1, "A2*2", {"D5*2", "D5*2"}},                                                 // moved to E4
      {1, 0, 0, "Sheet1!A1", {"Sheet2!D4", "'My Sheet'!D4"}},
      {1, 1, 0, "SUM(Sheet1!A1:B2)", {"SUM(Sheet2!D4:E5)", "SUM('My Sheet'!D4:E5)"}},
      {1, 2, 0, "D4+E5", {"#REF!+#REF!", "D4+E5"}},
      {1, 3, 0, "SUM(Sheet1:Sheet2!A1)", {"SUM(Sheet1:Sheet2!A1)", "SUM(Sheet1:Sheet2!A1)"}},
      {1, 4, 0, "Sheet1!C3", {"Sheet1!C3", "Sheet1!C3"}},
      {2, 0, 0, "Sheet1!A1", {"Sheet2!D4", "'My Sheet'!D4"}},
      {2, 1, 0, "Sheet1!A2:B2", {"Sheet2!D5:E5", "'My Sheet'!D5:E5"}},
  };
  EntryNames names;
  for (int t = 0; t < 2; ++t) {
    CutMap map;
    map.to = t + 1;
    for (const Case& k : cases) {
      map.was.sheet = k.sheet, map.was.row = k.row, map.was.col = k.col;
      map.now = map.was;
      if (k.sheet == 0 && k.row <= CutMap::kR2 && k.col <= CutMap::kC2) {
        map.now.sheet = map.to;
        map.now.row += CutMap::kDr, map.now.col += CutMap::kDc;
      }
      auto f = Compile(k.formula, map.was, names);
      CHECK(f != nullptr);
      if (!f) continue;
      PrintOptions how;
      how.map = &map;
      how.homeSheet = map.was.sheet;
      CHECK_EQ(Print(*f, map.now, names, how), std::string(k.after[t]));
    }
  }
}
