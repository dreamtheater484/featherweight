// S3's functions (plan §7.2 "Advanced"): what Excel 2021 on this PC lacks (TEXTSPLIT, VSTACK...)
// against Microsoft's documentation; the rest against Excel's results (scripts/excel-eval.py),
// written out here or in sheet_calc_cases.inc.

#include <cmath>
#include <map>
#include <string>
#include <tuple>

#include "TestHarness.h"
#include "sheet/calc/Engine.h"
#include "sheet/calc/Eval.h"
#include "sheet/calc/Fn.h"
#include "sheet/calc/Formula.h"

using namespace fw::sheet::calc;

namespace {

class FnHost : public Host {
 public:
  std::map<std::tuple<int, int32_t, int32_t>, Value> cells;
  double random = 0.25;
  int SheetCount() const override { return 3; }
  CellView Cell(int s, int32_t r, int32_t c) override {
    auto it = cells.find({s, r, c});
    return it == cells.end() ? CellView{} : View(it->second);
  }
  void ForEachCell(int s, int32_t r1, int32_t c1, int32_t r2, int32_t c2, CellFn fn, void* ctx) override {
    for (auto it = cells.lower_bound({s, r1, 0}); it != cells.end(); ++it) {
      auto [ss, r, c] = it->first;
      if (ss != s || r > r2) break;
      if (c < c1 || c > c2 || it->second.IsEmpty()) continue;
      if (!fn(ctx, r, c, View(it->second))) return;
    }
  }
  void Extent(int s, int32_t& rows, int32_t& cols) override {
    rows = cols = 0;
    for (const auto& [k, v] : cells) {
      if (std::get<0>(k) != s) continue;
      rows = std::max(rows, std::get<1>(k) + 1);
      cols = std::max(cols, std::get<2>(k) + 1);
    }
  }
  void Store(int s, int32_t r, int32_t c, const Value& v) override { cells[{s, r, c}] = v; }
  double Random() override { return random; }
};

struct Fx {
  FnHost host;
  Engine engine{host};
  Fx() {
    engine.SetSheets({"Sheet1", "Sheet2", "Data"});
    Put(0, 0, 0, Value::Number(10));  // A1
    Put(0, 0, 2, Value::Text("pear"));  // C1
    Put(1, 0, 0, Value::Number(7));  // Sheet2!A1
  }
  void Put(int s, int32_t r, int32_t c, Value v) { host.cells[{s, r, c}] = std::move(v); }
  void Formula(int32_t r, int32_t c, std::string_view text) {
    Cell at{0, r, c};
    engine.SetFormula(at, engine.CompileAt(text, at), false);
  }
};

std::string Render(const Value& v) {
  switch (v.type()) {
    case Type::Number: return NumberText(v.number());
    case Type::Text: return std::string(v.text());
    case Type::Bool: return v.boolean() ? "TRUE" : "FALSE";
    case Type::Error: return std::string(ErrText(v.error()));
    case Type::Array: {
      std::string s;
      const Array& a = v.array();
      for (int32_t i = 0; i < a.rows(); ++i)
        for (int32_t j = 0; j < a.cols(); ++j) s += (i || j ? (j ? "|" : ";") : "") + Render(a.at(i, j));
      return s;
    }
    case Type::Empty: return "";
    default: return "?";
  }
}

// A formula evaluated as a dynamic-array formula in Sheet1!Z50, rendered: arrays as rows ";",
// columns "|".
std::string Eval(Fx& f, std::string_view text) {
  Cell at{0, 49, 25};
  auto formula = f.engine.CompileAt(text, at, true);
  if (!formula) return "PARSE ERROR";
  Evaluator ev(f.host, &f.engine);
  Value out;
  if (ev.Evaluate(*formula, at, out) != EvalResult::kDone) return "NOT DONE";
  return Render(out);
}

struct Case {
  const char* formula;
  const char* want;
};

void Check(Fx& f, const Case* cases, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    std::string got = Eval(f, cases[i].formula);
    if (got != cases[i].want) std::printf("    %s: got %s, want %s\n", cases[i].formula, got.c_str(), cases[i].want);
    CHECK_EQ(got, std::string(cases[i].want));
  }
}

}  // namespace

TEST(sheet_functions_text_365) {
  // TEXTSPLIT, TEXTBEFORE, TEXTAFTER as Microsoft documents them (Excel 2021 lacks them).
  Fx f;
  const Case cases[] = {
      {"TEXTSPLIT(\"Dakota Lennon Sanchez\",\" \")", "Dakota|Lennon|Sanchez"},
      {"TEXTSPLIT(\"1,2,3;4,5,6\",\",\",\";\")", "1|2|3;4|5|6"},
      {"TEXTSPLIT(\"Do. Or do not. There is no try. -Anonymous\",\".\")", "Do| Or do not| There is no try| -Anonymous"},
      {"TEXTSPLIT(\"Do. Or do not. There is no try. -Anonymous\",{\".\",\"-\"})",
       "Do| Or do not| There is no try| |Anonymous"},
      {"TEXTSPLIT(\"a,,b\",\",\")", "a||b"},
      {"TEXTSPLIT(\"a,,b\",\",\",,TRUE)", "a|b"},
      {"TEXTSPLIT(\"1,2;3\",\",\",\";\")", "1|2;3|#N/A"},
      {"TEXTSPLIT(\"1,2;3\",\",\",\";\",,,\"-\")", "1|2;3|-"},
      {"TEXTSPLIT(\"aXbxc\",\"x\")", "aXb|c"},
      {"TEXTSPLIT(\"aXbxc\",\"x\",,,1)", "a|b|c"},
      {"TEXTSPLIT(\"a;b\",,\";\")", "a;b"},
      {"TEXTSPLIT(\"a, b,c\",{\", \",\",\"})", "a|b|c"},
      {"TEXTSPLIT(\"abc\",\"\")", "#VALUE!"},
      {"TEXTBEFORE(\"a-b-c\",\"-\")", "a"},
      {"TEXTBEFORE(\"a-b-c\",\"-\",2)", "a-b"},
      {"TEXTBEFORE(\"a-b-c\",\"-\",-1)", "a-b"},
      {"TEXTBEFORE(\"a-b-c\",\"-\",-2)", "a"},
      {"TEXTBEFORE(\"a-b-c\",\"-\",3)", "#N/A"},
      {"TEXTBEFORE(\"a-b-c\",\"-\",3,,1)", "a-b-c"},
      {"TEXTBEFORE(\"a-b-c\",\"x\",,,,\"none\")", "none"},
      {"TEXTBEFORE(\"aXb\",\"x\")", "#N/A"},
      {"TEXTBEFORE(\"aXb\",\"x\",,1)", "a"},
      {"TEXTBEFORE(\"a-b\",\"-\",0)", "#VALUE!"},
      {"TEXTBEFORE(\"a-b\",\"-\",4)", "#VALUE!"},
      {"TEXTBEFORE(\"abc\",\"\")", ""},
      {"TEXTBEFORE(\"abc\",\"\",-1)", "abc"},
      {"TEXTBEFORE(\"a, b; c\",{\", \",\"; \"},2)", "a, b"},
      {"TEXTAFTER(\"a-b-c\",\"-\")", "b-c"},
      {"TEXTAFTER(\"a-b-c\",\"-\",2)", "c"},
      {"TEXTAFTER(\"a-b-c\",\"-\",-1)", "c"},
      {"TEXTAFTER(\"a-b-c\",\"-\",-2)", "b-c"},
      {"TEXTAFTER(\"a-b-c\",\"-\",-3,,1)", "a-b-c"},
      {"TEXTAFTER(\"a-b-c\",\"-\",3,,1)", ""},
      {"TEXTAFTER(\"abc\",\"\")", "abc"},
      {"TEXTAFTER(\"abc\",\"\",-1)", ""},
      {"TEXTAFTER({\"a-b\";\"c-d\"},\"-\")", "b;d"},
      {"TEXTAFTER(\"Red riding hood's red hood\",\"hood\",-1)", ""},
      {"TEXTAFTER(\"Red riding hood's red hood\",\"red\",1,1)", " riding hood's red hood"},
      {"TEXTAFTER(\"Red riding hood's red hood\",\"red\",2)", "#N/A"},
      {"TEXTAFTER(\"Red riding hood's red hood\",\"red\")", " hood"},
      // Measured in Excel 2021 here, shown with '.' (the test's region).
      {"VALUETOTEXT(\"a\"\"b\",1)", "\"a\"\"b\""},
      {"VALUETOTEXT(2.5,1)", "2.5"},
      {"VALUETOTEXT(#N/A)", "#N/A"},
      {"VALUETOTEXT(#N/A,1)", "#N/A"},
      {"VALUETOTEXT(1,2)", "#VALUE!"},
      {"ARRAYTOTEXT({1,\"a\";TRUE,2.5},1)", "{1,\"a\";TRUE,2.5}"},
      {"ARRAYTOTEXT({1,\"a\";TRUE,2.5})", "1, a, TRUE, 2.5"},
      {"UNICHAR(65.9)", "A"},
      {"UNICHAR(0)", "#VALUE!"},
      {"UNICHAR(55296)", "#N/A"},
      {"UNICHAR(1114111)", "#N/A"},
      {"UNICHAR(1114112)", "#VALUE!"},
      {"UNICODE(UNICHAR(128512))", "128512"},
      {"UNICODE(\"\")", "#VALUE!"},
      {"UNICODE(\"A\")", "65"},
      {"LENB(\"abc\")&LEFTB(\"abc\",2)&RIGHTB(\"abc\")&MIDB(\"abc\",2,1)", "3abcb"},
  };
  Check(f, cases, std::size(cases));
}

TEST(sheet_functions_arrays_365) {
  // VSTACK ... RANDARRAY as Microsoft documents them.
  Fx f;
  const Case cases[] = {
      {"VSTACK({1,2},{3,4})", "1|2;3|4"},
      {"VSTACK({1,2},3)", "1|2;3|#N/A"},
      {"HSTACK({1;2},{3;4})", "1|3;2|4"},
      {"HSTACK(1,{2;3})", "1|2;#N/A|3"},
      {"VSTACK(A1,C1)", "10;pear"},
      {"TAKE({1,2,3;4,5,6;7,8,9},2)", "1|2|3;4|5|6"},
      {"TAKE({1,2,3;4,5,6;7,8,9},-1)", "7|8|9"},
      {"TAKE({1,2,3;4,5,6;7,8,9},,2)", "1|2;4|5;7|8"},
      {"TAKE({1,2,3;4,5,6;7,8,9},-2,-2)", "5|6;8|9"},
      {"TAKE({1,2,3;4,5,6;7,8,9},0)", "#CALC!"},
      {"TAKE({1,2;3,4},10)", "1|2;3|4"},
      {"DROP({1,2,3;4,5,6;7,8,9},1)", "4|5|6;7|8|9"},
      {"DROP({1,2,3;4,5,6;7,8,9},-1,-1)", "1|2;4|5"},
      {"DROP({1,2,3;4,5,6;7,8,9},3)", "#CALC!"},
      {"DROP({1,2;3,4},,1)", "2;4"},
      {"CHOOSECOLS({1,2,3;4,5,6;7,8,9},1,3)", "1|3;4|6;7|9"},
      {"CHOOSECOLS({1,2,3;4,5,6;7,8,9},-1)", "3;6;9"},
      {"CHOOSECOLS({1,2,3;4,5,6;7,8,9},{1,1})", "1|1;4|4;7|7"},
      {"CHOOSECOLS({1,2,3;4,5,6;7,8,9},4)", "#VALUE!"},
      {"CHOOSECOLS({1,2,3;4,5,6;7,8,9},0)", "#VALUE!"},
      {"CHOOSEROWS({1,2,3;4,5,6;7,8,9},2)", "4|5|6"},
      {"CHOOSEROWS({1,2,3;4,5,6;7,8,9},-1,1)", "7|8|9;1|2|3"},
      {"TOCOL({1,2;3,4})", "1;2;3;4"},
      {"TOCOL({1,2;3,4},,TRUE)", "1;3;2;4"},
      {"TOROW({1,2;3,4})", "1|2|3|4"},
      {"TOCOL({1,#N/A;3,4},2)", "1;3;4"},
      {"TOCOL({1,2},4)", "#VALUE!"},
      {"WRAPROWS({1,2,3,4,5},2)", "1|2;3|4;5|#N/A"},
      {"WRAPROWS({1,2,3,4,5},2,0)", "1|2;3|4;5|0"},
      {"WRAPCOLS({1,2,3,4,5},2)", "1|3|5;2|4|#N/A"},
      {"WRAPROWS({1,2;3,4},2)", "#VALUE!"},
      {"WRAPROWS({1,2},0)", "#NUM!"},
      {"WRAPROWS({1;2;3},5)", "1|2|3"},
      {"EXPAND({1,2;3,4},3,3)", "1|2|#N/A;3|4|#N/A;#N/A|#N/A|#N/A"},
      {"EXPAND({1,2},2,,\"\")", "1|2;|"},
      {"EXPAND({1,2},1,1)", "#VALUE!"},
      {"RANDARRAY(2,3)", "0.25|0.25|0.25;0.25|0.25|0.25"},
      {"RANDARRAY(1,2,1,6,TRUE)", "2|2"},
      {"RANDARRAY(1,1,5,1)", "#VALUE!"},
      {"RANDARRAY(0)", "#CALC!"},
      {"ROWS(VSTACK(TAKE({1;2;3},2),DROP({1;2;3},1)))", "4"},
  };
  Check(f, cases, std::size(cases));
}

TEST(sheet_functions_information) {
  // Measured in Excel 2021 (scratchpad probe): FORMULATEXT, ISFORMULA, SHEET, SHEETS, AREAS, CELL.
  Fx f;
  f.Formula(0, 1, "SUM(A1,2.5)");  // B1
  const Case cases[] = {
      {"FORMULATEXT(B1)", "=SUM(A1,2.5)"},
      {"FORMULATEXT(A1)", "#N/A"},
      {"ISFORMULA(B1)", "TRUE"},
      {"ISFORMULA(A1)", "FALSE"},
      {"SHEET()", "1"},
      {"SHEET(Sheet2!A1)", "2"},
      {"SHEET(\"data\")", "3"},
      {"SHEET(\"nope\")", "#N/A"},
      {"SHEET(5)", "#N/A"},
      {"SHEET(TRUE)", "#N/A"},
      {"SHEETS()", "3"},
      {"SHEETS(Sheet1:Data!A1)", "3"},
      {"SHEETS(A1)", "1"},
      {"SHEETS(5)", "#N/A"},
      {"AREAS(A1)", "1"},
      {"AREAS((A1,B1:C2,D4))", "3"},
      {"CELL(\"col\",C5)", "3"},
      {"CELL(\"row\",C5)", "5"},
      {"CELL(\"address\",B2)", "$B$2"},
      {"CELL(\"type\",A1)", "v"},
      {"CELL(\"type\",C1)", "l"},
      {"CELL(\"type\",Z99)", "b"},
      {"CELL(\"contents\",A1)", "10"},
      {"CELL(\"Contents\",Sheet2!A1)", "7"},
      {"CELL(\"bogus\",A1)", "#VALUE!"},
  };
  Check(f, cases, std::size(cases));
}
