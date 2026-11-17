// The formula engine (src/sheet/calc): parser and printer, values and conversions, evaluation,
// functions and the dependency graph.

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "sheet/calc/Formula.h"
#include "sheet/calc/Functions.h"
#include "sheet/calc/Value.h"

using namespace fw::sheet::calc;

namespace {

char UpperC(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; }
bool SameNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (UpperC(a[i]) != UpperC(b[i])) return false;
  return true;
}

class TestNames : public Names {
 public:
  std::vector<std::string> sheets{"Sheet1", "Sheet2", "My Sheet", "Data"};
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

// Compiles at `at` and prints at `at` (file form unless `ui`).
std::string RoundTrip(std::string_view text, CompileOptions at = {}, bool ui = false, bool r1c1Out = false) {
  TestNames names;
  ParseError err;
  auto f = Compile(text, at, names, &err);
  if (!f) return "ERROR: " + err.message;
  PrintOptions how;
  how.file = !ui;
  how.r1c1 = r1c1Out;
  std::string printed = Print(*f, at, names, how);
  // Printing and parsing again gives the same bytecode.
  CompileOptions again = at;
  again.r1c1 = r1c1Out;
  auto g = Compile(printed, again, names, &err);
  if (!g || g->code != f->code) return "REPARSE MISMATCH: " + printed;
  return printed;
}

uint16_t Flags(std::string_view text) {
  TestNames names;
  auto f = Compile(text, {}, names);
  return f ? f->flags : 0xFFFF;
}

}  // namespace

TEST(sheet_calc_function_table_sorted) {
  for (int i = 1; i < FunctionCount(); ++i) CHECK(std::string(Function(i - 1).name) < Function(i).name);
  CHECK(FindFunction("sum") >= 0);
  CHECK(FindFunction("Vlookup") >= 0);
  CHECK(FindFunction("NOPE") < 0);
  const FuncInfo& sumifs = Function(FindFunction("SUMIFS"));
  CHECK_EQ(ParamClass(sumifs, 0), 'R');
  CHECK_EQ(ParamClass(sumifs, 1), 'R');
  CHECK_EQ(ParamClass(sumifs, 2), 'V');
  CHECK_EQ(ParamClass(sumifs, 3), 'R');
  CHECK_EQ(ParamClass(sumifs, 4), 'V');
  CHECK_EQ(ParamClass(Function(FindFunction("SUM")), 7), 'R');
}

TEST(sheet_calc_parse_print) {
  const char* same[] = {
      "SUM(A1:B2)", "1+2*3", "-2^2", "2^-2", "(1+2)*3", "A1:A3 A2:C2", "SUM((A1,B2))",
      "Sheet2!A1", "'My Sheet'!A1:B2", "Sheet1:Sheet2!A1", "'Sheet1:My Sheet'!A1", "IF(A1>0,\"yes\",\"no\")",
      "IF(A1,IF(B1,1,2),3)", "{1,2;3,4}", "{1,-2.5,\"a\";TRUE,#N/A,0}", "Table1[Col]",
      "Table1[[#This Row],[Col]]", "Table1[#All]", "Table1[[Col1]:[Col2]]", "Table1[]",
      "Table1[[#Headers],[#Data],[Col]]", "A:A", "$A:$C", "1:1", "$2:$5", "MyName*2", "Sheet1!LocalName",
      "#REF!", "Sheet1!#REF!", "#DIV/0!+1", "A1%", "50%", "\"a\"\"b\"", "TRUE()", "TRUE", "VLOOKUP(A1,B:C,2,)",
      "IF(,1,)", "_xlfn.XLOOKUP(A1,B:B,C:C)", "_xlfn._xlws.FILTER(A1:A5,B1:B5>0)", "_xlfn.ANCHORARRAY(A1)",
      "_xlfn.SINGLE(A1:A10)", "1E+20", "0.1", "0.00000000015", "LOG10(100)", "R1C1", "A1:B2:C3", "INDEX(A:A,2):B5",
      "SUM(Sheet2!A1:B2,'My Sheet'!C:C)", "A1<>B1", "A1<=B1", "A1>=B1", "\"a\"&\"b\"", "NOW()", "TODAY()-A1",
      "SUM(A1:A10)/COUNT(A1:A10)", "_xlfn.IFNA(MATCH(A1,B:B,0),\"\")", "CHOOSE(2,A1,B1,C1)", "Data!$A$1:$D$100",
      "Sheet1!$A$1:$B$2,Sheet1!$D$1", "IFERROR(1/0,0)", "_xlfn.IFS(A1>1,\"a\",TRUE,\"b\")",
      "_xlfn.SWITCH(A1,1,\"one\",2,\"two\",\"other\")", "XFD1048576", "$XFD$1048576",
  };
  for (const char* f : same) {
    std::string got = RoundTrip(f);
    if (got != f) std::printf("    %s -> %s\n", f, got.c_str());
    CHECK_EQ(got, std::string(f));
  }
  CHECK_EQ(RoundTrip("=sum( a1 , $b$2 )"), "SUM(A1,$B$2)");
  CHECK_EQ(RoundTrip("= 1 +\n2"), "1+2");
  CHECK_EQ(RoundTrip("myfunc(1,2)"), "myfunc(1,2)");
  CHECK_EQ(RoundTrip("[1]Sheet1!A1"), "[1]Sheet1!A1");
  CHECK_EQ(RoundTrip("'[1]My Sheet'!A1:B2+1"), "'[1]My Sheet'!A1:B2+1");
  CHECK_EQ(RoundTrip("[1]!Total"), "[1]!Total");
  CHECK_EQ(RoundTrip("Unknown!A1"), "Unknown!A1");
  CHECK_EQ(RoundTrip("Table1[@Col]"), "Table1[[#This Row],[Col]]");
  CHECK_EQ(RoundTrip("Table1[[#This Row],[Col]]", {}, true), "Table1[@Col]");
  CHECK_EQ(RoundTrip("Table1[Price '[USD']]"), "Table1[Price '[USD']]");
  CHECK_EQ(RoundTrip("_xlfn.XLOOKUP(A1,B:B,C:C)", {}, true), "XLOOKUP(A1,B:B,C:C)");
  // LET: files write its names with _xlpm.; the formula bar without.
  CHECK_EQ(RoundTrip("_xlfn.LET(_xlpm.x,5,_xlpm.y,_xlpm.x+1,_xlpm.x*_xlpm.y)"),
           "_xlfn.LET(_xlpm.x,5,_xlpm.y,_xlpm.x+1,_xlpm.x*_xlpm.y)");
  CHECK_EQ(RoundTrip("LET(x,5,x*2)"), "_xlfn.LET(_xlpm.x,5,_xlpm.x*2)");
  CHECK_EQ(RoundTrip("_xlfn.LET(_xlpm.x,5,_xlpm.x*2)", {}, true), "LET(x,5,x*2)");
  CHECK_EQ(RoundTrip("LET(x,1,LET(x,2,x)+x)+x"), "_xlfn.LET(_xlpm.x,1,_xlfn.LET(_xlpm.x,2,_xlpm.x)+_xlpm.x)+x");
  CHECK_EQ(RoundTrip("XLOOKUP(A1,B:B,C:C)"), "_xlfn.XLOOKUP(A1,B:B,C:C)");
  CHECK_EQ(RoundTrip("_xlfn.SINGLE(A1:A10)", {}, true), "@A1:A10");
  CHECK_EQ(RoundTrip("A1#", {}, true), "A1#");
  CHECK_EQ(RoundTrip("A1#"), "_xlfn.ANCHORARRAY(A1)");
  CHECK_EQ(RoundTrip("@A1:A10"), "_xlfn.SINGLE(A1:A10)");
  CHECK_EQ(RoundTrip("1.0"), "1");
  CHECK_EQ(RoundTrip(".5"), "0.5");
  CHECK_EQ(RoundTrip("1e3"), "1000");
  CHECK_EQ(RoundTrip("1.5E-10"), "0.00000000015");  // as Excel writes it back
  CHECK_EQ(RoundTrip("123456789012345678"), "123456789012345000");  // Excel keeps 15 digits
  CHECK_EQ(RoundTrip("0.1234567890123456789"), "0.123456789012345");
}

TEST(sheet_calc_parse_flags) {
  CHECK(Flags("NOW()") & Formula::kVolatile);
  CHECK(Flags("OFFSET(A1,1,1)") & Formula::kDynamicRefs);
  CHECK(Flags("myfunc(1)") & Formula::kUnsupported);
  CHECK(Flags("[1]Sheet1!A1") & Formula::kUnsupported);
  CHECK(Flags("Nowhere!A1") & Formula::kUnsupported);
  CHECK(Flags("Name1+1") & Formula::kNames);
  CHECK(Flags("Table1[Col]") & Formula::kTables);
  CHECK(!(Flags("SUM(A1)") & (Formula::kVolatile | Formula::kNames)));
}

TEST(sheet_calc_parse_errors) {
  const char* bad[] = {"SUM(", "1+", "(1", "{1,2;3}", "A1:", "\"abc", "'Sheet", "SUM(1,2", "#FOO!",
                       "Table1[[#Bad]]", "1 2 +", "$A", "A$", "IF(1)", "ROUND(1)", "{A1}", "1+*2", ")",
                       "SUM(1,2))", "Sheet1!", "'Sheet1'A1", "LET(x,1)", "LET(x,1,y,2)", "LET(A1,1,A1)", "LET(1,2,3)"};
  for (const char* f : bad) {
    TestNames names;
    ParseError err;
    auto c = Compile(f, {}, names, &err);
    if (c) std::printf("    parsed but shouldn't: %s\n", f);
    CHECK(c == nullptr);
    CHECK(!err.message.empty());
  }
  // Deep nesting fails cleanly.
  std::string deep(5000, '(');
  deep += "1";
  deep += std::string(5000, ')');
  TestNames names;
  CHECK(Compile(deep, {}, names) == nullptr);
  std::string minus(100000, '-');
  minus += "1";
  CHECK(Compile(minus, {}, names) == nullptr);
}

TEST(sheet_calc_relative_refs) {
  // A shared formula: compiled once at C1, printed at other cells.
  TestNames names;
  CompileOptions c1;
  c1.row = 0, c1.col = 2;
  auto f = Compile("A1+$B$1+B$1+$A1", c1, names);
  CHECK(f != nullptr);
  CompileOptions c5 = c1;
  c5.row = 4;
  CHECK_EQ(Print(*f, c5, names), "A5+$B$1+B$1+$A5");
  CompileOptions e3;
  e3.row = 2, e3.col = 4;
  CHECK_EQ(Print(*f, e3, names), "C3+$B$1+D$1+$A3");
  PrintOptions r1c1;
  r1c1.r1c1 = true;
  CHECK_EQ(Print(*f, c1, names, r1c1), "RC[-2]+R1C2+R1C[-1]+RC1");
  // The same text at two cells compiles to different code; shared formulas at the same offset match.
  CompileOptions d2;
  d2.row = 1, d2.col = 3;
  auto g = Compile("B2+$B$1+C$1+$A2", d2, names);
  CHECK(g && g->code == f->code);
  // R1C1 in, A1 out.
  CompileOptions at;
  at.row = 5, at.col = 3;  // D6
  at.r1c1 = true;
  auto h = Compile("R[-1]C[2]+R1C1+SUM(R:R[1])+C[1]+R2", at, names);
  CHECK(h != nullptr);
  CompileOptions atA1 = at;
  atA1.r1c1 = false;
  if (h) {
    CHECK_EQ(Print(*h, atA1, names), "F5+$A$1+SUM(6:7)+E:E+$2:$2");
    CHECK_EQ(Print(*h, at, names, r1c1), "R[-1]C[2]+R1C1+SUM(R:R[1])+C[1]+R2");
  }
  CHECK_EQ(RoundTrip("R1C1", at, false, true), "R1C1");
  // A relative reference past the sheet's edge wraps, as Excel's names do.
  CompileOptions a1;
  auto w = Compile("XFD1048576", a1, names);
  CompileOptions b2;
  b2.row = 1, b2.col = 1;
  CHECK_EQ(Print(*w, b2, names), "A1");
}

TEST(sheet_calc_number_text) {
  CHECK_EQ(NumberText(0), "0");
  CHECK_EQ(NumberText(1), "1");
  CHECK_EQ(NumberText(-2.5), "-2.5");
  CHECK_EQ(NumberText(0.1 + 0.2), "0.3");
  CHECK_EQ(NumberText(1.0 / 3), "0.333333333333333");
  CHECK_EQ(NumberText(123456789012345.0), "123456789012345");
  CHECK_EQ(NumberText(1234567890123456.0), "1234567890123460");
  CHECK_EQ(NumberText(1e20), "1E+20");
  CHECK_EQ(NumberText(0.0001), "0.0001");
  CHECK_EQ(NumberText(1.5e-10), "0.00000000015");
  CHECK_EQ(NumberText(1e19), "10000000000000000000");
  CHECK_EQ(NumberText(-1e19), "-10000000000000000000");
  CHECK_EQ(NumberText(1e-18), "0.000000000000000001");
  CHECK_EQ(NumberText(1e-19), "1E-19");
  CHECK_EQ(NumberText(1.23456789012345e-6), "1.23456789012345E-06");
  CHECK_EQ(NumberText(123456789012345678.0), "123456789012346000");
  CHECK_EQ(NumberText(1e100), "1E+100");
  CHECK_EQ(NumberText(100), "100");
  CHECK_EQ(Round15(0.1 + 0.2), 0.3);
}

TEST(sheet_calc_text_to_number) {
  double v = 0;
  CHECK(TextToNumber(" 12 ", v) && v == 12);
  CHECK(TextToNumber("1e3", v) && v == 1000);
  CHECK(TextToNumber("1,234.5", v) && v == 1234.5);
  CHECK(TextToNumber("$12", v) && v == 12);
  CHECK(TextToNumber("-$12", v) && v == -12);
  CHECK(TextToNumber("50%", v) && v == 0.5);
  CHECK(TextToNumber("(3)", v) && v == -3);
  CHECK(TextToNumber("1/2/2024", v) && v == 45293);
  CHECK(TextToNumber("2024-01-02", v) && v == 45293);
  CHECK(TextToNumber("Jan 2, 2024", v) && v == 45293);
  CHECK(TextToNumber("2-Jan-2024", v) && v == 45293);
  CHECK(TextToNumber("12:30", v) && std::fabs(v - 0.5208333333333334) < 1e-12);
  CHECK(TextToNumber("1:30 PM", v) && std::fabs(v - 0.5625) < 1e-12);
  CHECK(TextToNumber("1/2/2024 12:00", v) && v == 45293.5);
  CHECK(TextToNumber("1 1/2", v) && v == 1.5);
  CHECK(TextToNumber("%5", v) && v == 0.05);
  CHECK(TextToNumber("(5%)", v) && v == -0.05);
  CHECK(!TextToNumber("(-5)", v));
  CHECK(!TextToNumber("5-", v));
  CHECK(!TextToNumber("--5", v));
  CHECK(TextToNumber("1:99", v) && std::fabs(v - 0.11041666666666666) < 1e-12);
  CHECK(TextToNumber("12:00 AM", v) && v == 0);
  CHECK(TextToNumber("12:30 a", v) && std::fabs(v - 0.020833333333333332) < 1e-12);
  CHECK(!TextToNumber("13:00 AM", v));
  CHECK(!TextToNumber("10000:00", v));
  CHECK(!TextToNumber("abc", v));
  CHECK(!TextToNumber("", v));
  CHECK(!TextToNumber("1,23", v));
  CHECK(TextToNumber("1,2345", v) && v == 12345);  // 3 digits or more after a separator, as Excel
  ConvLocale nl;
  nl.decimal = ',', nl.group = '.', nl.dateOrder = 'D', nl.currency = "â¬";
  CHECK(TextToNumber("3.14159", v, false, nl) && v == 314159);
  CHECK(TextToNumber("1,5", v, false, nl) && v == 1.5);
  CHECK(TextToNumber("13/1/2024", v, false, nl) && v == 45304);
  CHECK(!TextToNumber("$5", v, false, nl));
  CHECK_EQ(NumberText(0.5, ','), "0,5");
  CHECK(!TextToNumber("12 34", v));
}

TEST(sheet_calc_dates) {
  double s = 0;
  CHECK(DateSerial(1900, 1, 1, false, s) && s == 1);
  CHECK(DateSerial(1900, 2, 28, false, s) && s == 59);
  CHECK(DateSerial(1900, 2, 29, false, s) && s == 60);
  CHECK(DateSerial(1900, 3, 1, false, s) && s == 61);
  CHECK(DateSerial(2024, 1, 2, false, s) && s == 45293);
  CHECK(DateSerial(2024, 13, 1, false, s) && s == 45658);  // 2025-01-01
  CHECK(DateSerial(2024, 1, 0, false, s) && s == 45291);   // 2023-12-31
  CHECK(DateSerial(1904, 1, 1, true, s) && s == 0);
  CHECK(DateSerial(9999, 12, 31, false, s) && s == 2958465);
  CHECK(!DateSerial(10000, 1, 1, false, s));
  int y, m, d;
  CHECK(SerialDate(60, false, y, m, d) && y == 1900 && m == 2 && d == 29);
  CHECK(SerialDate(61, false, y, m, d) && y == 1900 && m == 3 && d == 1);
  CHECK(SerialDate(0, false, y, m, d) && y == 1900 && m == 1 && d == 0);
  CHECK(SerialDate(45293.75, false, y, m, d) && y == 2024 && m == 1 && d == 2);
  CHECK(SerialDate(0, true, y, m, d) && y == 1904 && m == 1 && d == 1);
  CHECK(!SerialDate(-1, false, y, m, d));
}

TEST(sheet_calc_text_compare) {
  CHECK(EqualText("abc", "ABC"));
  CHECK(!EqualText("abc", "abd"));
  CHECK(EqualText("\xC3\xA9t\xC3\xA9", "\xC3\x89T\xC3\x89"));  // été, ÉTÉ
  CHECK(EqualText("\xC3\x9F", "ss"));  // ß = ss, as in Excel
  CHECK(!EqualText("\xC3\xA9", "e"));
  CHECK(!EqualText("co-op", "coop"));
  CHECK(CompareText("coop", "co-op") < 0);
  CHECK(CompareText("a-b", "ab") > 0);
  CHECK(CompareText("_", "a") < 0);
  CHECK(CompareText("10", "9") < 0);
  CHECK(CompareText("apple", "Banana") < 0);
  CHECK(CompareText("b", "A") > 0);
  CHECK(CompareText("abc", "ABC") == 0);
  CHECK(WildcardMatch("a*", "Apple"));
  CHECK(WildcardMatch("?pple", "apple"));
  CHECK(!WildcardMatch("?pple", "pple"));
  CHECK(WildcardMatch("*~*", "5*"));
  CHECK(!WildcardMatch("*~*", "5"));
  CHECK(WildcardMatch("a*b*c", "aXXbYYc"));
  CHECK(!WildcardMatch("a*b*c", "aXXbYY"));
  CHECK(WildcardMatch("*", ""));
  size_t len = 0;
  CHECK(ParseErr("#n/a", &len) == Err::NA && len == 4);
  CHECK(ParseErr("#DIV/0!x", &len) == Err::Div0 && len == 7);
  CHECK(ParseErr("#nope") == Err::None);
}

// --- evaluation ---

#include <map>
#include <set>
#include <memory>
#include <tuple>

#include "sheet/calc/Eval.h"
#include "sheet/calc/Fn.h"

namespace {

// Cells in a map, for evaluating without a workbook model.
class MapHost : public Host {
 public:
  std::map<std::tuple<int, int32_t, int32_t>, Value> cells;
  int sheets = 4;
  double now = 45293.5;  // 2024-01-02 12:00

  void Set(int s, int32_t r, int32_t c, Value v) { cells[{s, r, c}] = std::move(v); }
  int SheetCount() const override { return sheets; }
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
  double Now() override { return now; }
  void Store(int s, int32_t r, int32_t c, const Value& v) override { cells[{s, r, c}] = v; }
  std::set<int32_t> hiddenRows;    // on every sheet
  std::set<int32_t> filteredRows;  // hidden by a filter (also in hiddenRows)
  bool RowHidden(int, int32_t r) override { return hiddenRows.count(r) != 0; }
  bool RowFiltered(int, int32_t r) override { return filteredRows.count(r) != 0; }
};

class TestResolver : public Resolver {
 public:
  TestNames names_;
  std::map<uint32_t, std::shared_ptr<const Formula>> defined;
  Names& names() override { return names_; }
  const Formula* Name(uint32_t slot, int) override {
    auto it = defined.find(slot);
    return it == defined.end() ? nullptr : it->second.get();
  }
  bool Spill(int, int32_t, int32_t, Area&) override { return false; }
  bool Ready(const Area&) override { return true; }
  void Define(std::string_view name, std::string_view text) {
    defined[names_.NameSlot(name)] = Compile(text, {}, names_);
  }
};

bool ParseA1(std::string_view a, int& s, int32_t& r, int32_t& c) {
  s = 0;
  size_t bang = a.find('!');
  if (bang != std::string_view::npos) {
    s = a.substr(0, bang) == "Sheet2" ? 1 : 0;
    a = a.substr(bang + 1);
  }
  c = 0;
  size_t i = 0;
  while (i < a.size() && a[i] >= 'A' && a[i] <= 'Z') c = c * 26 + (a[i++] - 'A' + 1);
  r = std::atoi(std::string(a.substr(i)).c_str()) - 1;
  --c;
  return r >= 0 && c >= 0;
}

void Put(MapHost& h, std::string_view addr, Value v) {
  int s;
  int32_t r, c;
  ParseA1(addr, s, r, c);
  h.Set(s, r, c, std::move(v));
}

// The cells make_sheet_calc_cases.py gives Excel.
void Fixture(MapHost& h) {
  const double nums[][2] = {{0, 10}, {1, 20}, {2, 30}, {6, -5}};
  for (auto& n : nums) h.Set(0, static_cast<int32_t>(n[0]), 0, Value::Number(n[1]));
  Put(h, "A4", Value::Text("x"));
  Put(h, "A5", Value::Bool(true));
  for (int i = 0; i < 7; ++i) h.Set(0, i, 1, Value::Number(i + 1));
  const char* fruit[] = {"apple", "Banana", "cherry", "date", "Elder"};
  for (int i = 0; i < 5; ++i) h.Set(0, i, 2, Value::Text(fruit[i]));
  const double dates[] = {45293, 45300, 45350.75, 45400};
  for (int i = 0; i < 4; ++i) h.Set(0, i, 3, Value::Number(dates[i]));
  const char* letters[] = {"a", "b", "a", "c", "a"};
  for (int i = 0; i < 5; ++i) h.Set(0, i, 4, Value::Text(letters[i]));
  for (int i = 0; i < 5; ++i) h.Set(0, i, 5, Value::Number((i + 1) * 100.0));
  Put(h, "G1", Value::Text("12"));
  Put(h, "G2", Value::Text(" 3 "));
  const char* words[] = {"one", "three", "five", "seven"};
  for (int i = 0; i < 4; ++i) {
    h.Set(0, i, 7, Value::Number(2 * i + 1));
    h.Set(0, i, 8, Value::Text(words[i]));
  }
  Put(h, "J1", Value::Error(Err::Div0));
  Put(h, "J2", Value::Error(Err::NA));
  // A database (K1:M7) and criteria ranges (N:O) for DSUM and friends.
  const char* trees[] = {"Apple", "Pear", "Cherry", "Apple", "Pear", "Apple"};
  const double sizes[][2] = {{18, 20}, {12, 12}, {13, 14}, {14, 15}, {9, 8}, {8, 9}};
  Put(h, "K1", Value::Text("Tree")), Put(h, "L1", Value::Text("Height")), Put(h, "M1", Value::Text("Age"));
  for (int i = 0; i < 6; ++i) {
    h.Set(0, i + 1, 10, Value::Text(trees[i]));
    h.Set(0, i + 1, 11, Value::Number(sizes[i][0]));
    h.Set(0, i + 1, 12, Value::Number(sizes[i][1]));
  }
  const char* crit[][2] = {{"N1", "Tree"}, {"O1", "Height"}, {"N2", "Apple"}, {"O2", ">10"}, {"N4", "Tree"},
                           {"N5", "Pear"}, {"N7", "tree"}, {"N8", "Apple"}, {"N9", "=Pear"}, {"O4", "Height"}};
  for (auto& k : crit) Put(h, k[0], Value::Text(k[1]));
  Put(h, "Sheet2!A1", Value::Number(7));
  Put(h, "Sheet2!A2", Value::Number(8));
  Put(h, "Sheet2!B1", Value::Text("s2"));
}

struct ExcelCase {
  const char* formula;
  bool array;
  char type;
  double num;
  const char* text;
};
const ExcelCase kExcelCases[] = {
#include "sheet_calc_cases.inc"
};

Value Run(MapHost& h, TestResolver& res, std::string_view text, Cell at = {0, 0, 701}, bool array = false,
          EvalResult* how = nullptr) {
  CompileOptions o;
  o.sheet = at.sheet, o.row = at.row, o.col = at.col, o.array = array;
  ParseError err;
  auto f = Compile(text, o, res.names(), &err);
  if (!f) return Value::Text("PARSE ERROR " + err.message);
  Evaluator ev(h, &res);
  Value out;
  EvalResult r = ev.Evaluate(*f, at, out);
  if (how) *how = r;
  if (r != EvalResult::kDone) return Value::Text(r == EvalResult::kRetry ? "RETRY" : "UNSUPPORTED");
  return out;
}

std::string Show(const Value& v) {
  switch (v.type()) {
    case Type::Number: return NumberText(v.number()) + " (" + std::to_string(v.number()) + ")";
    case Type::Text: return "\"" + std::string(v.text()) + "\"";
    case Type::Bool: return v.boolean() ? "TRUE" : "FALSE";
    case Type::Error: return std::string(ErrText(v.error()));
    case Type::Array: return "{array " + std::to_string(v.array().rows()) + "x" + std::to_string(v.array().cols()) + "}";
    default: return "(empty)";
  }
}

}  // namespace

TEST(sheet_calc_matches_excel) {
  MapHost h;
  Fixture(h);
  TestResolver res;
  int failed = 0, checked = 0, unsupported = 0;
  int32_t row = 0;
  for (const ExcelCase& k : kExcelCases) {
    Cell at{0, row++, 701};  // ZZ, where Excel evaluated it
    if (k.type == 'x') continue;
    EvalResult how;
    Value v = Run(h, res, k.formula, at, k.array, &how);
    if (how == EvalResult::kUnsupported) {
      ++unsupported;
      continue;
    }
    if (v.IsArray()) v = Value(v.array().at(0, 0));
    if (v.IsEmpty()) v = Value::Number(0);
    bool ok = false;
    switch (k.type) {
      case 'n': {
        // XIRR iterates to Excel's documented accuracy, 0.000001 %: its digits after that differ.
        double tolerance = std::strstr(k.formula, "XIRR(") ? 1e-8 : 1e-12;
        ok = v.IsNumber() && std::fabs(v.number() - k.num) <= tolerance * std::max(1.0, std::fabs(k.num));
        break;
      }
      case 's': ok = v.IsText() && v.text() == k.text; break;
      case 'b': ok = v.IsBool() && v.boolean() == (k.num != 0); break;
      case 'e': ok = v.IsError() && ErrText(v.error()) == k.text; break;
    }
    ++checked;
    if (!ok) {
      ++failed;
      std::string want = k.type == 'n' ? NumberText(k.num) : k.type == 'b' ? (k.num ? "TRUE" : "FALSE") : k.text;
      std::printf("    %s%s: got %s, Excel %s\n", k.array ? "{array} " : "", k.formula, Show(v).c_str(), want.c_str());
    }
  }
  std::printf("    %d of %d match Excel\n", checked - failed, checked);
  CHECK_EQ(failed, 0);
}

// --- the engine ---

#include <chrono>

#include "sheet/calc/Engine.h"

namespace {

struct EngineFixture {
  MapHost host;
  Engine engine{host};
  EngineFixture() { engine.SetSheets({"Sheet1", "Sheet2", "My Sheet", "Data"}); }
  Cell At(std::string_view addr) {
    int s;
    int32_t r, c;
    ParseA1(addr, s, r, c);
    return {s, r, c};
  }
  void Formula(std::string_view addr, std::string_view text, double cached = 0, bool hasCached = true) {
    Cell at = At(addr);
    engine.SetFormula(at, engine.CompileAt(text, at), hasCached);
    if (hasCached) host.Set(at.sheet, at.row, at.col, Value::Number(cached));
  }
  void Number(std::string_view addr, double v) {
    Cell at = At(addr);
    host.Set(at.sheet, at.row, at.col, Value::Number(v));
  }
  void Edit(std::string_view addr, double v) {
    Cell at = At(addr);
    host.Set(at.sheet, at.row, at.col, Value::Number(v));
    Area a;
    a.r1 = a.r2 = at.row, a.c1 = a.c2 = static_cast<int16_t>(at.col);
    engine.Changed(at.sheet, a);
    engine.Recalculate();
  }
  double Num(std::string_view addr) {
    Cell at = At(addr);
    CellView v = host.Cell(at.sheet, at.row, at.col);
    return v.type == Type::Number ? v.num : -999999;
  }
};

CalcSettings Full() {
  CalcSettings s;
  s.fullCalcOnLoad = true;
  return s;
}

}  // namespace

TEST(sheet_calc_engine_order_and_edits) {
  EngineFixture f;
  f.Number("A1", 1);
  // Defined in an order that isn't the calculation order.
  f.Formula("D1", "SUM(B1:C1)");
  f.Formula("C1", "B1+1");
  f.Formula("B1", "A1*2");
  f.engine.Opened(Full());
  CHECK(f.engine.Recalculate());
  CHECK_EQ(f.Num("D1"), 5.0);
  f.Edit("A1", 5);
  CHECK_EQ(f.Num("B1"), 10.0);
  CHECK_EQ(f.Num("C1"), 11.0);
  CHECK_EQ(f.Num("D1"), 21.0);
  CHECK(!f.engine.Pending());
  CHECK(!f.engine.Circular());
  // Only the dependents of an edit run.
  size_t before = f.engine.Evaluations();
  f.Edit("A9", 3);
  CHECK_EQ(f.engine.Evaluations(), before);
}

TEST(sheet_calc_engine_on_open) {
  EngineFixture f;
  f.Formula("A1", "TODAY()", 0);                          // volatile: recalculated
  f.Formula("B1", "A1+1", 0);                             // depends on a volatile one: recalculated
  f.Formula("C1", "5*2", 999);                            // cached: kept, as Excel keeps it
  f.Formula("D1", "C1*2", 0, false);                      // no cached value: calculated
  f.Formula("E1", "_xlfn.WEBSERVICE(C1)+A1", 7);          // unsupported: keeps 7, stale
  f.engine.Opened({});
  f.engine.Recalculate();
  CHECK_EQ(f.Num("A1"), 45293.0);
  CHECK_EQ(f.Num("B1"), 45294.0);
  CHECK_EQ(f.Num("C1"), 999.0);
  CHECK_EQ(f.Num("D1"), 1998.0);
  CHECK_EQ(f.Num("E1"), 7.0);
  CHECK(f.engine.Stale(0, 0, 4));
  CHECK_EQ(f.engine.StaleCount(), size_t{1});
  // Manual calculation: nothing.
  EngineFixture m;
  m.Formula("A1", "TODAY()", 0);
  CalcSettings manual;
  manual.manual = true;
  m.engine.Opened(manual);
  CHECK(!m.engine.Pending());
  CHECK_EQ(m.Num("A1"), 0.0);
  // A sheet with its own fullCalcOnLoad: its formulas, and what depends on them.
  EngineFixture s;
  s.Formula("Sheet2!A1", "5*2", 999);
  s.Formula("A1", "Sheet2!A1+1", 999);
  s.Formula("B1", "3*2", 999);
  CalcSettings sheet;
  sheet.fullCalcSheets = {1};
  s.engine.Opened(sheet);
  s.engine.Recalculate();
  CHECK_EQ(s.Num("Sheet2!A1"), 10.0);
  CHECK_EQ(s.Num("A1"), 11.0);
  CHECK_EQ(s.Num("B1"), 999.0);
}

TEST(sheet_calc_engine_circular_and_iterative) {
  EngineFixture f;
  f.Formula("A1", "B1+1", 0);
  f.Formula("B1", "A1+1", 0);
  f.engine.Opened(Full());
  CHECK(f.engine.Recalculate());
  CHECK(f.engine.Circular());
  EngineFixture g;
  g.Formula("A1", "A1/2+1", 0);
  CalcSettings it = Full();
  it.iterate = true;
  it.iterateCount = 100;
  it.iterateDelta = 1e-9;
  g.engine.Opened(it);
  CHECK(g.engine.Recalculate());
  CHECK(std::fabs(g.Num("A1") - 2) < 1e-6);
}

TEST(sheet_calc_engine_dynamic_refs_names_arrays) {
  EngineFixture f;
  for (int i = 0; i < 3; ++i) f.host.Set(0, i, 0, Value::Number(i + 1));  // A1:A3
  // B1 reads C1:C3 through OFFSET, which the graph can't see: the run-time check orders it.
  f.Formula("B1", "SUM(OFFSET(C1,0,0,3,1))");
  for (int i = 1; i <= 3; ++i) f.Formula("C" + std::to_string(i), "A" + std::to_string(i) + "*10");
  f.engine.DefineName("Rate", -1, "Sheet1!$A$2");
  f.engine.DefineName("Twice", -1, "Rate*2");
  f.Formula("D1", "Rate*100+Twice");
  Cell e1{0, 0, 4};
  Area out;
  out.r1 = 0, out.r2 = 2, out.c1 = out.c2 = 4;
  f.engine.SetFormula(e1, f.engine.CompileAt("A1:A3*2", e1, true), true, &out);
  f.engine.Opened(Full());
  f.engine.Recalculate();
  CHECK_EQ(f.Num("B1"), 60.0);
  CHECK_EQ(f.Num("D1"), 204.0);
  CHECK_EQ(f.Num("E1"), 2.0);
  CHECK_EQ(f.Num("E3"), 6.0);
  f.Edit("A2", 4);
  CHECK_EQ(f.Num("B1"), 80.0);
  CHECK_EQ(f.Num("D1"), 408.0);
  CHECK_EQ(f.Num("E2"), 8.0);
}

TEST(sheet_calc_engine_spills) {
  // A dynamic array spills as far as its result reaches; cells in the way make it #SPILL!.
  EngineFixture f;
  f.Number("A1", 1);
  Cell c1 = f.At("C1");
  Area out;
  out.r1 = out.r2 = 0, out.c1 = out.c2 = 2;  // as the file cached it: C1 alone
  f.engine.SetFormula(c1, f.engine.CompileAt("_xlfn.SEQUENCE(A1)", c1, true), true, &out, true);
  f.Formula("D1", "SUM(C1:C5)");
  f.Formula("D2", "ROWS(_xlfn.ANCHORARRAY(C1))");
  f.engine.Opened(Full());
  f.engine.Recalculate();
  CHECK_EQ(f.Num("C1"), 1.0);
  CHECK_EQ(f.Num("D2"), 1.0);
  f.Edit("A1", 3);  // grows into C2:C3
  CHECK_EQ(f.Num("C3"), 3.0);
  CHECK_EQ(f.Num("D1"), 6.0);
  CHECK_EQ(f.Num("D2"), 3.0);
  f.Number("C5", 9);
  f.Edit("A1", 5);  // C5 is in the way
  CellView v = f.host.Cell(0, 0, 2);
  CHECK(v.type == Type::Error && v.err == Err::Spill);
  CHECK(f.host.Cell(0, 1, 2).type == Type::Empty);
  CHECK_EQ(f.Num("D2"), 1.0);
  f.Edit("A1", 2);  // fits again
  CHECK_EQ(f.Num("C2"), 2.0);
  CHECK(f.host.Cell(0, 2, 2).type == Type::Empty);
  CHECK_EQ(f.Num("D1"), 12.0);
}

TEST(sheet_calc_engine_aggregate) {
  // SUBTOTAL leaves out cells that call SUBTOTAL or AGGREGATE; so does AGGREGATE with options 0..3,
  // and options 1, 3, 5, 7 leave out hidden rows (measured in Excel 2021).
  EngineFixture f;
  f.Number("A1", 1);
  f.Formula("A2", "_xlfn.AGGREGATE(9,4,A1)");
  f.Formula("A3", "SUBTOTAL(9,A1)");
  f.Number("A4", 5);
  f.host.hiddenRows.insert(3);  // row 4
  f.Formula("B1", "SUBTOTAL(9,A1:A3)");
  f.Formula("B2", "_xlfn.AGGREGATE(9,0,A1:A3)");
  f.Formula("B3", "_xlfn.AGGREGATE(9,4,A1:A3)");
  f.Formula("B4", "_xlfn.AGGREGATE(9,5,A1:A4)");
  f.Formula("B5", "_xlfn.AGGREGATE(9,4,A1:A4)");
  f.Formula("B6", "_xlfn.AGGREGATE(15,2,A2:A3,1)");
  f.Formula("B7", "SUBTOTAL(109,A1:A4)");
  f.Formula("B8", "_xlfn.AGGREGATE(14,7,A1:A4,1)");
  f.engine.Opened(Full());
  f.engine.Recalculate();
  CHECK_EQ(f.Num("B1"), 1.0);
  CHECK_EQ(f.Num("B2"), 1.0);
  CHECK_EQ(f.Num("B3"), 3.0);
  CHECK_EQ(f.Num("B4"), 3.0);
  CHECK_EQ(f.Num("B5"), 8.0);
  CellView v = f.host.Cell(0, 5, 1);
  CHECK(v.type == Type::Error && v.err == Err::Num);
  CHECK_EQ(f.Num("B7"), 1.0);
  CHECK_EQ(f.Num("B8"), 1.0);
}

TEST(sheet_calc_engine_subtotal_filtered) {
  // Every SUBTOTAL leaves out rows a filter hides; AGGREGATE takes them as other hidden rows
  // (measured in Excel 2021 on POI's ConditionalFormattingSamples, a table with an icon filter).
  EngineFixture f;
  f.Number("A1", 1);
  f.Number("A2", 2);  // hidden by hand
  f.Number("A3", 4);  // filtered
  f.host.hiddenRows = {1, 2};
  f.host.filteredRows = {2};
  f.Formula("B1", "SUBTOTAL(9,A1:A3)");
  f.Formula("B2", "SUBTOTAL(109,A1:A3)");
  f.Formula("B3", "SUBTOTAL(3,A1:A3)");
  f.Formula("B4", "_xlfn.AGGREGATE(9,4,A1:A3)");
  f.Formula("B5", "_xlfn.AGGREGATE(9,5,A1:A3)");
  f.engine.Opened(Full());
  f.engine.Recalculate();
  CHECK_EQ(f.Num("B1"), 3.0);
  CHECK_EQ(f.Num("B2"), 1.0);
  CHECK_EQ(f.Num("B3"), 2.0);
  CHECK_EQ(f.Num("B4"), 7.0);
  CHECK_EQ(f.Num("B5"), 1.0);
}

namespace {

// One sheet of dense columns, fast enough to time the engine rather than a map.
class DenseHost : public Host {
 public:
  std::vector<std::vector<Value>> cols;
  int SheetCount() const override { return 1; }
  CellView Cell(int, int32_t r, int32_t c) override {
    if (c >= static_cast<int32_t>(cols.size()) || r >= static_cast<int32_t>(cols[c].size())) return {};
    return View(cols[c][r]);
  }
  void ForEachCell(int, int32_t r1, int32_t c1, int32_t r2, int32_t c2, CellFn fn, void* ctx) override {
    int32_t rows = 0;
    for (auto& col : cols) rows = std::max(rows, static_cast<int32_t>(col.size()));
    for (int32_t r = r1; r <= std::min(r2, rows - 1); ++r)
      for (int32_t c = c1; c <= c2 && c < static_cast<int32_t>(cols.size()); ++c) {
        if (r >= static_cast<int32_t>(cols[c].size()) || cols[c][r].IsEmpty()) continue;
        if (!fn(ctx, r, c, View(cols[c][r]))) return;
      }
  }
  void Extent(int, int32_t& rows, int32_t& c) override {
    rows = 0;
    for (auto& col : cols) rows = std::max(rows, static_cast<int32_t>(col.size()));
    c = static_cast<int32_t>(cols.size());
  }
  void Store(int, int32_t r, int32_t c, const Value& v) override { cols[c][r] = v; }
};

}  // namespace

// Plan §11: recalculation after one edit in a 100,000-formula workbook, < 100 ms typical.
TEST(sheet_calc_engine_100k_formulas) {
  const int32_t n = 50000;
  DenseHost host;
  host.cols.assign(3, std::vector<Value>(n));
  for (int32_t r = 0; r < n; ++r) host.cols[0][r] = Value::Number(r % 100);
  Engine engine(host);
  engine.SetSheets({"Sheet1"});
  // B: =A*2 and C: a running total =C(prev)+B, each one shared formula as files store them.
  auto b = engine.CompileAt("A1*2", {0, 0, 1});
  auto c = engine.CompileAt("C1+B2", {0, 1, 2});
  for (int32_t r = 0; r < n; ++r) {
    engine.SetFormula({0, r, 1}, b);
    engine.SetFormula({0, r, 2}, r == 0 ? engine.CompileAt("B1", {0, 0, 2}) : c);
  }
  CHECK_EQ(engine.FormulaCount(), size_t{100000});
  auto t0 = std::chrono::steady_clock::now();
  engine.Opened(Full());
  CHECK(engine.Recalculate());
  double full = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  double total = 0;
  for (int32_t r = 0; r < n; ++r) total += (r % 100) * 2;
  CHECK_EQ(host.cols[2][n - 1].number(), total);
  auto edit = [&](int32_t row, double v) {
    total += (v - host.cols[0][row].number()) * 2;
    host.cols[0][row] = Value::Number(v);
    Area a;
    a.r1 = a.r2 = row, a.c1 = a.c2 = 0;
    auto s = std::chrono::steady_clock::now();
    engine.Changed(0, a);
    engine.Recalculate();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s).count();
  };
  double few = edit(n - 5, 1000);  // 6 dependents
  double half = edit(n / 2, 1000);  // 25,001
  double all = edit(0, 1000);       // 50,001
  CHECK_EQ(host.cols[2][n - 1].number(), total);
  std::printf("    100,000 formulas: all calculated in %.0f ms; an edit with 6 dependents %.2f ms, 25,001 %.1f ms, "
              "50,001 %.1f ms\n", full, few, half, all);
  CHECK(few < 100);
}

// --- the engine on a workbook model, opened as the app does ---

#include "PkgTestUtil.h"
#include "sheet/calc/Bind.h"
#include "sheet/model/Workbook.h"
#include "sheet/xlsx/Xlsx.h"

namespace {

// A one-sheet workbook from its sheetData, with `calcPr` (and what follows it) in workbook.xml and
// `after` after sheetData.
std::unique_ptr<fw::sheet::Workbook> OpenSheet(const std::string& sheetData, const std::string& calcPr,
                                               const std::string& after = {}) {
  const std::string ns = "xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
                         "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"";
  std::vector<std::pair<std::string, std::string>> parts = {
      {"[Content_Types].xml",
       "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
       "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
       "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
       "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
       "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
       "</Types>"},
      {"_rels/.rels",
       "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
       "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
       "Target=\"xl/workbook.xml\"/></Relationships>"},
      {"xl/workbook.xml", "<workbook " + ns + "><sheets><sheet name=\"S\" sheetId=\"1\" r:id=\"rId1\"/></sheets>" + calcPr +
                              "</workbook>"},
      {"xl/_rels/workbook.xml.rels",
       "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
       "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" "
       "Target=\"worksheets/sheet1.xml\"/></Relationships>"},
      {"xl/worksheets/sheet1.xml", "<worksheet " + ns + "><sheetData>" + sheetData + "</sheetData>" + after + "</worksheet>"},
  };
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(test::MakeZip(parts), st);
  if (!pkg) return nullptr;
  fw::sheet::xlsx::ReadOptions options;
  return fw::sheet::xlsx::Read(std::move(pkg), st, options);
}

// A1 2; B1 =A1*3 (6); C1 =TODAY() (volatile); D1 =C1>0, cached FALSE; E1 =A1+1 without a cached
// value; F1 =B1*2 cached 999 (wrong, but Excel doesn't recalculate it on opening); G1 an unknown
// function of C1, cached 7.
const char* kOpenCells =
    "<row r=\"1\"><c r=\"A1\"><v>2</v></c><c r=\"B1\"><f>A1*3</f><v>6</v></c><c r=\"C1\"><f>TODAY()</f><v>1</v></c>"
    "<c r=\"D1\" t=\"b\"><f>C1&gt;0</f><v>0</v></c><c r=\"E1\"><f>A1+1</f></c><c r=\"F1\"><f>B1*2</f><v>999</v></c>"
    "<c r=\"G1\"><f>_xlfn.NOSUCHFUNCTION(C1)</f><v>7</v></c></row>";

// The calc features Excel 2021 needs to keep a file's cached values (with calcId 191028 or later).
std::string Features(const char* names = "RD Single FV CNMTM") {
  std::string out = "<extLst><ext uri=\"{B58B0392-4F1F-4190-BB64-5DF3571DCE5F}\" "
                    "xmlns:xcalcf=\"http://schemas.microsoft.com/office/spreadsheetml/2018/calcfeatures\"><xcalcf:calcFeatures>";
  std::string n;
  for (const char* p = names;; ++p) {
    if (*p && *p != ' ') {
      n += *p;
      continue;
    }
    if (!n.empty()) out += "<xcalcf:feature name=\"microsoft.com:" + n + "\"/>";
    n.clear();
    if (!*p) break;
  }
  return out + "</xcalcf:calcFeatures></ext></extLst>";
}

double Number(fw::sheet::Workbook& book, const char* a1) {
  fw::sheet::CellAddr a;
  fw::sheet::ParseA1(a1, a);
  fw::sheet::Value v = book.GetSheet(0).GetValue(a.row, a.col);
  return v.type == fw::sheet::ValueType::Number ? v.number : v.type == fw::sheet::ValueType::Bool ? (v.boolean ? 1 : 0) : -999;
}

}  // namespace

TEST(sheet_calc_open_in_slices) {
  // After opening, the app recalculates what Excel would, a slice at a time, then nothing.
  auto book = OpenSheet(kOpenCells, "<calcPr calcId=\"191029\"/>" + Features());
  CHECK(book != nullptr);
  if (!book) return;
  WorkbookCalc calc(*book);
  CHECK(calc.Opening());
  int slices = 0;
  while (!calc.OpenStep(1) && slices < 1000) ++slices;
  CHECK(!calc.Opening());
  CHECK(calc.OpenStep(1));  // done: nothing more
  CHECK(Number(*book, "C1") > 40000);   // TODAY() recalculated
  CHECK_EQ(Number(*book, "D1"), 1.0);   // its dependent too
  CHECK_EQ(Number(*book, "E1"), 3.0);   // no cached value
  CHECK_EQ(Number(*book, "F1"), 999.0); // Excel doesn't recalculate it either
  CHECK_EQ(Number(*book, "G1"), 7.0);   // unsupported: keeps its cached value...
  CHECK(calc.engine().Stale(0, 0, 6));  // ...marked possibly out of date
  CHECK(!calc.engine().Stale(0, 0, 5));
  CHECK(calc.Changes() >= 3);
  CHECK(!calc.engine().Pending());

  // Everything is recalculated with fullCalcOnLoad, a sheet's own fullCalcOnLoad, and from a
  // calculation engine Excel 2021 doesn't trust (measured): calcId before 191028, none, or a
  // calc feature missing (LET_WF isn't needed). Manual mode: nothing, even then.
  struct Case {
    std::string calcPr, after;
    double f1;
  } cases[] = {
      {"<calcPr calcId=\"191029\" fullCalcOnLoad=\"1\"/>" + Features(), "", 12},
      {"<calcPr calcId=\"191029\"/>" + Features(), "<sheetCalcPr fullCalcOnLoad=\"1\"/>", 12},
      {"<calcPr calcId=\"191027\"/>" + Features(), "", 12},
      {"<calcPr calcId=\"191028\"/>" + Features(), "", 999},
      {"<calcPr calcId=\"162913\"/>", "", 12},
      {"<calcPr/>" + Features(), "", 12},
      {"", "", 12},
      {"<calcPr calcId=\"191029\"/>", "", 12},
      {"<calcPr calcId=\"191029\"/>" + Features("RD Single FV"), "", 12},
      {"<calcPr calcId=\"191029\"/>" + Features("RD Single FV CNMTM LET_WF LAMBDA_WF NEW"), "", 999},
  };
  for (const Case& k : cases) {
    auto b = OpenSheet(kOpenCells, k.calcPr, k.after);
    CHECK(b != nullptr);
    if (!b) continue;
    WorkbookCalc c(*b);
    while (!c.OpenStep(8)) {
    }
    CHECK_EQ(Number(*b, "F1"), k.f1);
  }
  for (const char* calcPr : {"<calcPr calcId=\"191029\" calcMode=\"manual\"/>",
                             "<calcPr calcId=\"162913\" calcMode=\"manual\"/>",
                             "<calcPr calcId=\"191029\" calcMode=\"manual\" fullCalcOnLoad=\"1\"/>"}) {
    auto manual = OpenSheet(kOpenCells, calcPr);
    WorkbookCalc manualCalc(*manual);
    while (!manualCalc.OpenStep(8)) {
    }
    CHECK_EQ(Number(*manual, "C1"), 1.0);
    CHECK_EQ(manualCalc.Changes(), 0u);
  }
}

TEST(sheet_calc_subtotal_filter) {
  // Rows a filter hides (an autoFilter with criteria) are left out by SUBTOTAL 9 too; with no
  // criteria, a hidden row counts as hidden by hand (measured in Excel 2021).
  const char* cells =
      "<row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>n</t></is></c></row><row r=\"2\"><c r=\"A2\"><v>1</v></c></row>"
      "<row r=\"3\" hidden=\"1\"><c r=\"A3\"><v>2</v></c></row><row r=\"4\"><c r=\"A4\"><v>4</v></c></row>"
      "<row r=\"6\"><c r=\"A6\"><f>SUBTOTAL(9,A2:A4)</f><v>0</v></c><c r=\"B6\"><f>SUBTOTAL(109,A2:A4)</f><v>0</v></c>"
      "<c r=\"C6\"><f>_xlfn.AGGREGATE(9,4,A2:A4)</f><v>0</v></c></row>";
  const std::string calcPr = "<calcPr calcId=\"191029\" fullCalcOnLoad=\"1\"/>";
  auto filtered = OpenSheet(cells, calcPr,
                            "<autoFilter ref=\"A1:A4\"><filterColumn colId=\"0\"><filters><filter val=\"1\"/>"
                            "<filter val=\"4\"/></filters></filterColumn></autoFilter>");
  auto buttons = OpenSheet(cells, calcPr, "<autoFilter ref=\"A1:A4\"/>");
  CHECK(filtered && buttons);
  if (!filtered || !buttons) return;
  for (auto* b : {filtered.get(), buttons.get()}) {
    WorkbookCalc c(*b);
    while (!c.OpenStep(8)) {
    }
  }
  CHECK_EQ(Number(*filtered, "A6"), 5.0);
  CHECK_EQ(Number(*filtered, "B6"), 5.0);
  CHECK_EQ(Number(*filtered, "C6"), 7.0);
  CHECK_EQ(Number(*buttons, "A6"), 7.0);
  CHECK_EQ(Number(*buttons, "B6"), 5.0);
}

#include <chrono>

#include "sheet/edit/Edit.h"

namespace {

// The app's wiring: the editor's notifications drive the calculation.
void Follow(fw::sheet::edit::Editor& ed, WorkbookCalc& calc) {
  namespace edit = fw::sheet::edit;
  ed.OnChange([&calc](const std::vector<edit::Change>& changes) {
    for (const edit::Change& c : changes) {
      if (c.what & (edit::kSheets | edit::kNames | edit::kShift)) calc.StructureEdited();
      else if (c.what & (edit::kValues | edit::kFormulas)) calc.CellsEdited(c.sheet, c.cells);
    }
  });
}

}  // namespace

TEST(sheet_calc_after_edits) {
  namespace edit = fw::sheet::edit;
  auto book = OpenSheet("<row r=\"1\"><c r=\"A1\"><v>2</v></c><c r=\"B1\"><f>A1*3</f><v>6</v></c>"
                        "<c r=\"C1\"><f>B1+1</f><v>7</v></c></row>",
                        "<calcPr calcId=\"191029\"/>" + Features());
  CHECK(book != nullptr);
  if (!book) return;
  WorkbookCalc calc(*book);
  while (!calc.OpenStep(8)) {
  }
  edit::Editor ed(*book);
  Follow(ed, calc);
  auto settle = [&] {
    while (!calc.CalcStep(8)) {
    }
  };
  ed.SetCell(0, 0, 0, edit::CellInput::Number(5));
  CHECK(calc.Calculating());
  settle();
  CHECK(!calc.Calculating());
  CHECK_EQ(Number(*book, "B1"), 15.0);
  CHECK_EQ(Number(*book, "C1"), 16.0);
  ed.SetCell(0, 0, 3, edit::CellInput::Formula("C1*2"));  // D1, a new formula
  settle();
  CHECK_EQ(Number(*book, "D1"), 32.0);
  ed.SetCell(0, 0, 0, edit::CellInput::Number(1));  // its precedents are followed
  settle();
  CHECK_EQ(Number(*book, "D1"), 8.0);
  ed.SetCell(0, 0, 1, edit::CellInput::Formula("A1*10"));  // a formula changed
  settle();
  CHECK_EQ(Number(*book, "D1"), 22.0);
  ed.Undo(1);  // undo follows too
  settle();
  CHECK_EQ(Number(*book, "B1"), 3.0);
  CHECK_EQ(Number(*book, "D1"), 8.0);
  int s = -1;
  int32_t r = -1, c = -1;
  CHECK(!calc.CircularCell(s, r, c));
  ed.SetCell(0, 0, 0, edit::CellInput::Formula("D1"));  // A1 = D1: circular
  settle();
  CHECK(calc.CircularCell(s, r, c));
  ed.ClearContents(0, fw::sheet::Range{0, 0, 0, 0});  // gone: the others are calculated again
  settle();
  CHECK_EQ(Number(*book, "D1"), 2.0);
  ed.AddSheet(1, "");  // structure: everything read again
  settle();
  ed.SetCell(0, 0, 0, edit::CellInput::Number(10));
  settle();
  CHECK_EQ(Number(*book, "D1"), 62.0);
}

TEST(sheet_calc_after_edits_manual) {
  // Manual mode: a value's dependents wait for F9; a formula entered is calculated at once.
  namespace edit = fw::sheet::edit;
  auto book = OpenSheet("<row r=\"1\"><c r=\"A1\"><v>2</v></c><c r=\"B1\"><f>A1*3</f><v>6</v></c></row>",
                        "<calcPr calcId=\"191029\" calcMode=\"manual\"/>" + Features());
  CHECK(book != nullptr);
  if (!book) return;
  WorkbookCalc calc(*book);
  while (!calc.OpenStep(8)) {
  }
  CHECK(calc.ManualCalc());
  edit::Editor ed(*book);
  Follow(ed, calc);
  ed.SetCell(0, 0, 0, edit::CellInput::Number(5));
  CHECK_EQ(Number(*book, "B1"), 6.0);
  ed.SetCell(0, 0, 2, edit::CellInput::Formula("A1+1"));  // C1
  CHECK_EQ(Number(*book, "C1"), 6.0);
  calc.CalculateNow();  // F9
  while (!calc.CalcStep(8)) {
  }
  CHECK_EQ(Number(*book, "B1"), 15.0);
}

TEST(sheet_calc_shift_f9) {
  // Manual mode, Shift+F9: the active sheet's dirty formulas, from the other sheet's values as they
  // are; F9 then the rest.
  namespace edit = fw::sheet::edit;
  auto book = OpenSheet("<row r=\"1\"><c r=\"A1\"><v>2</v></c><c r=\"B1\"><f>A1*3</f><v>6</v></c></row>",
                        "<calcPr calcId=\"191029\" calcMode=\"manual\"/>" + Features());
  CHECK(book != nullptr);
  if (!book) return;
  WorkbookCalc calc(*book);
  while (!calc.OpenStep(8)) {
  }
  edit::Editor ed(*book);
  Follow(ed, calc);
  ed.AddSheet(1, "T");
  ed.SetCell(1, 0, 0, edit::CellInput::Formula("S!A1*10"));  // T!A1, calculated as entered
  ed.SetCell(1, 0, 1, edit::CellInput::Formula("S!B1+1"));   // T!B1
  auto t = [&](uint32_t col) {
    fw::sheet::Value v = book->GetSheet(1).GetValue(0, col);
    return v.type == fw::sheet::ValueType::Number ? v.number : -999;
  };
  CHECK_EQ(t(0), 20.0);
  CHECK_EQ(t(1), 7.0);
  ed.SetCell(0, 0, 0, edit::CellInput::Number(5));
  CHECK_EQ(Number(*book, "B1"), 6.0);
  calc.CalculateSheet(1);  // T: S!B1 is still 6
  CHECK_EQ(t(0), 50.0);
  CHECK_EQ(t(1), 7.0);
  CHECK_EQ(Number(*book, "B1"), 6.0);
  CHECK(calc.Calculating());
  calc.CalculateSheet(0);
  CHECK_EQ(Number(*book, "B1"), 15.0);
  CHECK_EQ(t(1), 7.0);
  bool saving = true;  // a save on its worker: nothing written
  calc.HoldWhile([&] { return saving; });
  calc.CalculateNow();
  CHECK(!calc.CalcStep(8));
  CHECK_EQ(t(1), 7.0);
  saving = false;
  calc.CalculateNow();  // F9
  while (!calc.CalcStep(8)) {
  }
  CHECK_EQ(t(1), 16.0);
  CHECK(!calc.Calculating());
}

TEST(sheet_calc_iterative_after_edits) {
  // calcPr iterate: a circular reference after an edit settles (A1 = A1/2 + C1 → 2*C1), and
  // there's no circular-reference banner; without it, there is one.
  namespace edit = fw::sheet::edit;
  const std::string cells = "<row r=\"1\"><c r=\"A1\"><f>B1/2+C1</f><v>2</v></c><c r=\"B1\"><f>A1</f><v>2</v></c>"
                            "<c r=\"C1\"><v>1</v></c></row>";
  for (bool iterate : {true, false}) {
    auto book = OpenSheet(cells, std::string("<calcPr calcId=\"191029\"") +
                                     (iterate ? " iterate=\"1\" iterateCount=\"100\" iterateDelta=\"0.0001\"" : "") +
                                     "/>" + Features());
    CHECK(book != nullptr);
    if (!book) return;
    WorkbookCalc calc(*book);
    while (!calc.OpenStep(8)) {
    }
    edit::Editor ed(*book);
    Follow(ed, calc);
    ed.SetCell(0, 0, 2, edit::CellInput::Number(3));
    while (!calc.CalcStep(8)) {
    }
    int s = 0;
    int32_t r = 0, c = 0;
    CHECK_EQ(calc.CircularCell(s, r, c), !iterate);
    if (iterate) CHECK(std::fabs(Number(*book, "A1") - 6) < 0.001);
  }
}

TEST(sheet_calc_new_circular_reference) {
  // Excel 2021, iteration off (scratchpad circ_probe.py): cells on a circular reference, and after
  // one, aren't calculated; a formula just entered there shows 0; recalculating changes nothing.
  namespace edit = fw::sheet::edit;
  struct Step {
    const char* cell;
    const char* formula;  // null: the number
    double number;
  };
  struct Scenario {
    std::vector<Step> steps;
    double a1, b1, d1;  // -999: empty
  };
  const Scenario scenarios[] = {
      {{{"A1", "B1+1", 0}, {"B1", "A1", 0}}, 1, 0, -999},
      {{{"B1", nullptr, 5}, {"A1", "B1+1", 0}, {"B1", "A1", 0}}, 6, 0, -999},
      {{{"A1", "A1+1", 0}}, 0, -999, -999},
      {{{"A1", nullptr, 7}, {"A1", "A1+1", 0}}, 0, -999, -999},
      {{{"C1", nullptr, 3}, {"A1", "B1+C1", 0}, {"B1", "A1", 0}, {"C1", nullptr, 10}}, 3, 0, -999},
      {{{"A1", "B1+1", 0}, {"B1", "A1", 0}, {"D1", "A1*2", 0}}, 1, 0, 0},
      {{{"D1", "A1*2", 0}, {"A1", "B1+1", 0}, {"B1", "A1", 0}}, 1, 0, 2},
  };
  for (const Scenario& sc : scenarios) {
    auto book = OpenSheet("", "<calcPr calcId=\"191029\"/>" + Features());
    CHECK(book != nullptr);
    if (!book) return;
    WorkbookCalc calc(*book);
    while (!calc.OpenStep(8)) {
    }
    edit::Editor ed(*book);
    Follow(ed, calc);
    for (const Step& st : sc.steps) {
      fw::sheet::CellAddr a;
      fw::sheet::ParseA1(st.cell, a);
      ed.SetCell(0, a.row, a.col, st.formula ? edit::CellInput::Formula(st.formula) : edit::CellInput::Number(st.number));
      while (!calc.CalcStep(8)) {
      }
    }
    for (int pass = 0; pass < 2; ++pass) {  // as entered, then after Ctrl+Alt+F9
      CHECK_EQ(Number(*book, "A1"), sc.a1);
      CHECK_EQ(Number(*book, "B1"), sc.b1);
      CHECK_EQ(Number(*book, "D1"), sc.d1);
      int s = 0;
      int32_t r = 0, c = 0;
      CHECK(calc.CircularCell(s, r, c));
      calc.CalculateFull();
      while (!calc.CalcStep(8)) {
      }
    }
  }
  // Breaking the circle: calculated again, and no banner.
  auto book = OpenSheet("", "<calcPr calcId=\"191029\"/>" + Features());
  WorkbookCalc calc(*book);
  while (!calc.OpenStep(8)) {
  }
  edit::Editor ed(*book);
  Follow(ed, calc);
  ed.SetCell(0, 0, 0, edit::CellInput::Formula("B1+1"));
  ed.SetCell(0, 0, 1, edit::CellInput::Formula("A1"));
  while (!calc.CalcStep(8)) {
  }
  ed.SetCell(0, 0, 1, edit::CellInput::Number(5));
  while (!calc.CalcStep(8)) {
  }
  CHECK_EQ(Number(*book, "A1"), 6.0);
  int s = 0;
  int32_t r = 0, c = 0;
  CHECK(!calc.CircularCell(s, r, c));
}

#include "sheet/calc/NameManager.h"
#include "sheet/numfmt/NumLocale.h"

TEST(sheet_calc_name_manager) {
  // The Name Manager's list in en-NL, Excel's rules for a name, and a typed "Refers to".
  auto book = OpenSheet("<row r=\"1\"><c r=\"A1\"><v>1</v></c><c r=\"B1\"><v>0.05</v></c></row>"
                        "<row r=\"2\"><c r=\"A2\"><v>2</v></c></row><row r=\"3\"><c r=\"A3\"><v>3</v></c></row>",
                        "<definedNames><definedName name=\"Rate\">S!$B$1</definedName>"
                        "<definedName name=\"data\">S!$A$1:$A$3</definedName>"
                        "<definedName name=\"Tax\" localSheetId=\"0\">0.21</definedName>"
                        "<definedName name=\"_xlnm.Print_Area\" localSheetId=\"0\">S!$A$1:$B$3</definedName>"
                        "<definedName name=\"Bad\">#REF!</definedName></definedNames>"
                        "<calcPr calcId=\"191029\"/>" + Features());
  CHECK(book != nullptr);
  if (!book) return;
  WorkbookCalc calc(*book);
  while (!calc.OpenStep(8)) {
  }
  const FormulaLocale nl = MakeFormulaLocale(fw::sheet::EnNlLocale());
  std::vector<NameRow> rows = ListNames(*book, calc, nl);
  CHECK_EQ(rows.size(), size_t{5});
  if (rows.size() == 5) {
    CHECK_EQ(rows[0].name, std::string("Bad"));
    CHECK_EQ(rows[0].value, std::string("#REF!"));
    CHECK_EQ(rows[1].name, std::string("data"));
    CHECK_EQ(rows[1].refersTo, std::string("=S!$A$1:$A$3"));
    CHECK_EQ(rows[1].value, std::string("{1;2;3}"));
    CHECK_EQ(rows[2].name, std::string("Print_Area"));
    CHECK(rows[2].builtIn);
    CHECK_EQ(rows[2].scope, 0);
    CHECK_EQ(rows[3].name, std::string("Rate"));
    CHECK_EQ(rows[3].value, std::string("0,05"));
    CHECK_EQ(rows[4].name, std::string("Tax"));
    CHECK_EQ(rows[4].refersTo, std::string("=0,21"));
    CHECK_EQ(rows[4].value, std::string("0,21"));
  }
  for (const char* bad : {"A1", "xfd1048576", "R1C1", "r", "C", "RC", "R12", "1abc", "my name", "a-b", ""})
    CHECK(!CheckName(*book, bad, -1).empty());
  for (const char* good : {"XFE1", "ABCD1", "_x", "\\x", "Total.Q1", "Q?", "Tax"}) CHECK(CheckName(*book, good, -1).empty());
  CHECK(!CheckName(*book, "RATE", -1).empty());  // already there, any case
  CHECK(CheckName(*book, "RATE", -1, "Rate").empty());  // renaming it
  CHECK(!CheckName(*book, "tax", 0).empty());
  std::string formula, message;
  CompileOptions at;
  CHECK(ReadRefersTo("=SUM(S!$A$1:$A$3;2,5)", nl, at, calc.engine(), formula, message));
  CHECK_EQ(formula, std::string("SUM(S!$A$1:$A$3,2.5)"));
  CHECK(ReadRefersTo("12,5", nl, at, calc.engine(), formula, message));
  CHECK_EQ(formula, std::string("12.5"));
  CHECK(ReadRefersTo("say \"hi\"", nl, at, calc.engine(), formula, message));
  CHECK_EQ(formula, std::string("\"say \"\"hi\"\"\""));
  CHECK(!ReadRefersTo("=1+", nl, at, calc.engine(), formula, message));
  CHECK(!message.empty());
}

TEST(sheet_calc_edit_budget) {
  // Plan §11: < 100 ms after one edit in a 100,000-formula workbook (typing a value, entering a
  // formula), the dependency graph kept.
  namespace edit = fw::sheet::edit;
  std::string cells;
  cells.reserve(6 << 20);
  for (int r = 1; r <= 50000; ++r) {
    std::string n = std::to_string(r);
    cells += "<row r=\"" + n + "\"><c r=\"A" + n + "\"><v>" + n + "</v></c><c r=\"B" + n + "\"><f>A" + n + "*2</f><v>" +
             std::to_string(r * 2) + "</v></c><c r=\"C" + n + "\"><f>B" + n + "+1</f><v>" + std::to_string(r * 2 + 1) +
             "</v></c></row>";
  }
  auto book = OpenSheet(cells, "<calcPr calcId=\"191029\"/>" + Features());
  CHECK(book != nullptr);
  if (!book) return;
  WorkbookCalc calc(*book);
  while (!calc.OpenStep(8)) {
  }
  CHECK_EQ(calc.engine().FormulaCount(), size_t{100000});
  edit::Editor ed(*book);
  Follow(ed, calc);
  auto timed = [&](auto&& edit) {
    auto t0 = std::chrono::steady_clock::now();
    edit();
    while (!calc.CalcStep(8)) {
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  };
  double value = timed([&] { ed.SetCell(0, 99, 0, edit::CellInput::Number(7)); });
  CHECK_EQ(Number(*book, "C100"), 15.0);
  double formula = timed([&] { ed.SetCell(0, 199, 1, edit::CellInput::Formula("A200*3")); });
  CHECK_EQ(Number(*book, "C200"), 601.0);
  double again = timed([&] { ed.SetCell(0, 199, 0, edit::CellInput::Number(1)); });
  CHECK_EQ(Number(*book, "C200"), 4.0);
  std::printf("    one edit in 100,000 formulas: a value %.1f ms, a formula %.1f ms, its precedent %.1f ms\n", value,
              formula, again);
  CHECK(value < 100 && formula < 100 && again < 100);
}

TEST(sheet_calc_names_from_selection) {
  // Excel 2021 (scratchpad names_probe.py): Range.CreateNames on labels in the top row, rows 2-3 below.
  struct Label {
    const char* text;  // null: a number
    const char* name;  // null: none made
  };
  const Label labels[] = {
      {"Sales 2024", nullptr}, {nullptr, nullptr},      {"Net-Profit", "Net_Profit"}, {"a.b", "a.b"},
      {"R1C1", "_R1C1"},       {"A1", "A1_"},           {"Total ", "Total"},          {"Qtr#1", "Qtr_1"},
      {"x", "x"},              {"C", "C_"},             {"R", "R_"},                  {"_ok", "_ok"},
      {"  lead", "lead"},      {"Tab\tx", "Tab_x"},     {"Sales 2024", "Sales_2024"}, {"abc?", "abc?"},
      {"\\back", "\\back"},    {"Sheet1!A1", "Sheet1_A1"}, {"LOG10", "LOG10_"},       {"$A", "A"},
      {"a b  c", "a_b__c"},    {"ZZZ99999", "ZZZ99999"}, {"Name(1)", "Name_1"},       {"12abc", "_12abc"},
      {"R2C", "_R2C"},         {"rc", "rc_"},           {"XFD1", "XFD1_"},            {"\xC3\xA9t\xC3\xA9", "\xC3\xA9t\xC3\xA9"},
  };
  const size_t n = sizeof labels / sizeof labels[0];
  std::string data = "<row r=\"1\">";
  for (size_t i = 0; i < n; ++i) {
    const std::string ref = ColumnName(static_cast<int32_t>(i)) + "1";
    data += labels[i].text ? "<c r=\"" + ref + "\" t=\"inlineStr\"><is><t xml:space=\"preserve\">" + std::string(labels[i].text) + "</t></is></c>"
                           : "<c r=\"" + ref + "\"><v>2024</v></c>";
  }
  data += "</row><row r=\"2\"><c r=\"A2\"><v>1</v></c></row>"
          "<row r=\"5\"><c r=\"A5\" t=\"inlineStr\"><is><t>Corner</t></is></c><c r=\"B5\" t=\"inlineStr\"><is><t>Jan</t></is></c>"
          "<c r=\"C5\" t=\"inlineStr\"><is><t>Feb</t></is></c></row>"
          "<row r=\"6\"><c r=\"A6\" t=\"inlineStr\"><is><t>East</t></is></c><c r=\"B6\"><v>1</v></c></row>"
          "<row r=\"7\"><c r=\"A7\" t=\"inlineStr\"><is><t>West</t></is></c><c r=\"B7\"><v>2</v></c></row>";
  auto book = OpenSheet(data, "<calcPr calcId=\"191029\"/>");
  CHECK(book != nullptr);
  if (!book) return;
  std::vector<LabelName> made = NamesFromLabels(*book, 0, {0, 0, 2, static_cast<uint32_t>(n - 1)}, true, false, false, false);
  for (size_t i = 0; i < n; ++i) {
    if (!labels[i].name) continue;
    const std::string want = "S!$" + ColumnName(static_cast<int32_t>(i)) + "$2:$" + ColumnName(static_cast<int32_t>(i)) + "$3";
    bool found = false;
    for (const LabelName& m : made)
      if (m.name == labels[i].name) {
        found = true;
        CHECK_EQ(m.formula, want);
      }
    if (!found) std::printf("  missing %s\n", labels[i].name);
    CHECK(found);
  }
  CHECK_EQ(made.size(), size_t{26});  // no number label, and "Sales 2024" once (the later)
  // Top row and left column: the corner names the cells between.
  made = NamesFromLabels(*book, 0, {4, 0, 6, 2}, true, true, false, false);
  const std::pair<const char*, const char*> both[] = {{"Corner", "S!$B$6:$C$7"}, {"Jan", "S!$B$6:$B$7"},
                                                      {"Feb", "S!$C$6:$C$7"},    {"East", "S!$B$6:$C$6"},
                                                      {"West", "S!$B$7:$C$7"}};
  CHECK_EQ(made.size(), size_t{5});
  for (const auto& [name, formula] : both) {
    bool found = false;
    for (const LabelName& m : made) found = found || (m.name == name && m.formula == formula);
    CHECK(found);
  }
  bool top = false, left = false;
  GuessLabels(*book, 0, {4, 0, 6, 2}, top, left);
  CHECK(top && left);
  GuessLabels(*book, 0, {5, 1, 6, 1}, top, left);  // numbers only
  CHECK(!top && !left);
  CHECK_EQ(RangeFormula(*book, 0, {1, 2, 1, 2}), std::string("S!$C$2"));
  CHECK_EQ(NameFromLabel("  "), std::string());
}

TEST(sheet_calc_typed_beyond_the_extent) {
  // s3-corpus's fixture: data typed in a new sheet, then formulas over all of it. Values typed
  // below the rows the engine had seen must be read (TEXTJOIN missed A5, lookups found nothing).
  namespace edit = fw::sheet::edit;
  auto book = OpenSheet("<row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>Region</t></is></c></row>",
                        "<calcPr calcId=\"191029\"/>" + Features());
  CHECK(book != nullptr);
  if (!book) return;
  WorkbookCalc calc(*book);
  while (!calc.OpenStep(8)) {
  }
  edit::Editor ed(*book);
  Follow(ed, calc);
  auto settle = [&] {
    while (!calc.CalcStep(8)) {
    }
  };
  const char* regions[] = {"North", "South", "East", "West"};
  for (uint32_t i = 0; i < 4; ++i) {
    ed.SetCell(0, i + 1, 0, edit::CellInput::Text(regions[i]));
    ed.SetCell(0, i + 1, 1, edit::CellInput::Number(100 + 25 * i));
  }
  settle();
  ed.SetCell(0, 0, 4, edit::CellInput::Text("West"));
  ed.SetCell(0, 1, 4, edit::CellInput::Formula("_xlfn.XLOOKUP(E1,A2:A5,B2:B5,\"none\")"));
  ed.SetCell(0, 2, 4, edit::CellInput::Formula("_xlfn.TEXTJOIN(\", \",TRUE,A2:A5)"));
  ed.SetCell(0, 3, 4, edit::CellInput::Formula("SUM(B:B)"));
  settle();
  CHECK_EQ(Number(*book, "E2"), 175.0);
  fw::sheet::CellInfo c;
  CHECK(book->GetSheet(0).GetCell(2, 4, c) && c.value.type == fw::sheet::ValueType::String &&
        c.value.text == "North, South, East, West");
  CHECK_EQ(Number(*book, "E4"), 550.0);
  ed.SetCell(0, 9, 1, edit::CellInput::Number(1));  // B10, further down: the whole column sees it
  settle();
  CHECK_EQ(Number(*book, "E4"), 551.0);
}

TEST(sheet_calc_typed_dynamic_arrays) {
  // A typed formula that gives an array spills from its cell (Excel 365's dynamic arrays): the
  // grid asks SpillSize, the editor stores a dynamic array master over the cells, the engine spills.
  namespace edit = fw::sheet::edit;
  using fw::sheet::Range;
  auto book = OpenSheet("<row r=\"5\"><c r=\"C5\"><v>9</v></c></row>", "<calcPr calcId=\"191029\"/>" + Features());
  CHECK(book != nullptr);
  if (!book) return;
  WorkbookCalc calc(*book);
  while (!calc.OpenStep(8)) {
  }
  edit::Editor ed(*book);
  Follow(ed, calc);
  auto settle = [&] {
    while (!calc.CalcStep(8)) {
    }
  };
  Range r;
  CHECK(calc.SpillSize("_xlfn.SEQUENCE(3)", 0, 0, 0, r) && r == (Range{0, 0, 2, 0}));
  CHECK(!calc.SpillSize("SUM(1,2)", 0, 0, 1, r));
  CHECK(calc.SpillSize("_xlfn.SEQUENCE(1,3)", 0, 4, 0, r) && r == Range::Cell(4, 0));  // C5 is in the way
  edit::CellInput in = edit::CellInput::Formula("_xlfn.SEQUENCE(3)");
  in.dynamic = true;
  in.arrayRange = Range{0, 0, 2, 0};
  CHECK(ed.SetCell(0, 0, 0, in));
  settle();
  CHECK_EQ(Number(*book, "A2"), 2.0);
  CHECK_EQ(Number(*book, "A3"), 3.0);
  fw::sheet::FormulaInfo f;
  CHECK(book->GetSheet(0).GetFormula(0, 0, f) && f.dynamic && f.kind == fw::sheet::FormulaKind::Array &&
        f.ref == (Range{0, 0, 2, 0}));
  // Blocked: #SPILL! in its cell alone.
  in = edit::CellInput::Formula("_xlfn.SEQUENCE(1,3)");
  in.dynamic = true;
  in.arrayRange = Range::Cell(4, 0);
  CHECK(ed.SetCell(0, 4, 0, in));
  settle();
  fw::sheet::CellInfo c;
  CHECK(book->GetSheet(0).GetCell(4, 0, c) && c.value.type == fw::sheet::ValueType::Error &&
        c.value.error == fw::sheet::ErrorCode::Spill);
  CHECK(!book->GetSheet(0).GetCell(4, 1, c) || c.value.type == fw::sheet::ValueType::Empty);
  // Undone: the spilled values go with it.
  ed.Undo(2);
  settle();
  CHECK(!book->GetSheet(0).GetCell(1, 0, c) || c.value.type == fw::sheet::ValueType::Empty);
}
