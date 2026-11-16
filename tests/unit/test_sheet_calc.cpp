// The formula engine (src/sheet/calc): parser and printer, values and conversions, evaluation,
// functions and the dependency graph.

#include <cmath>
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
                       "SUM(1,2))", "Sheet1!", "'Sheet1'A1"};
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
      case 'n': ok = v.IsNumber() && std::fabs(v.number() - k.num) <= 1e-12 * std::max(1.0, std::fabs(k.num)); break;
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
