#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "PkgTestUtil.h"
#include "TestHarness.h"
#include "core/Json.h"
#include "sheet/calc/Bind.h"
#include "sheet/condfmt/CondFormat.h"
#include "sheet/numfmt/NumLocale.h"
#include "sheet/xlsx/Xlsx.h"

using namespace fw;
using namespace fw::sheet;

namespace {

std::filesystem::path SheetTests() { return std::filesystem::path(__FILE__).parent_path().parent_path() / "sheet"; }

std::string Load(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}

std::unique_ptr<Workbook> OpenWorkbook(const std::string& bytes) {
  pkg::Status st;
  auto pkg = test::OpenBytes(std::vector<uint8_t>(bytes.begin(), bytes.end()), st);
  if (!pkg) return nullptr;
  xlsx::ReadOptions options;
  options.loadAll = true;
  return xlsx::Read(std::move(pkg), st, options);
}

std::string Hex(uint32_t argb) {
  char buf[8];
  std::snprintf(buf, sizeof buf, "%06X", argb & 0xFFFFFFu);
  return buf;
}

// The case workbook's sheet (tests/sheet/condfmt-cases.xlsx), its rules evaluated with the engine.
struct Cases {
  std::unique_ptr<Workbook> book;
  std::unique_ptr<calc::WorkbookCalc> calc;
  std::unique_ptr<condfmt::SheetRules> rules;
  std::optional<JsonValue> expected;

  Cases() {
    book = OpenWorkbook(Load(SheetTests() / "condfmt-cases.xlsx"));
    expected = ParseJson(Load(SheetTests() / "condfmt-expected.json"));
    if (!book || !expected) return;
    calc = std::make_unique<calc::WorkbookCalc>(*book, &EnNlLocale());
    calc->Load();
    condfmt::Options o;
    o.locale = &EnNlLocale();  // the reference PC's region
    o.calc = calc.get();
    o.today = expected->Find("today")->number;
    rules = std::make_unique<condfmt::SheetRules>(*book, 0, o);
  }

  // The cells of the case named `name`, top to bottom.
  std::vector<condfmt::Style> Column(const std::string& name) {
    std::vector<condfmt::Style> out;
    for (const JsonValue& c : expected->Find("cases")->array) {
      if (c.StringOr("name") != name) continue;
      CellAddr a;
      ParseA1(c.StringOr("column") + "2", a);
      for (uint32_t r = 0; r < static_cast<uint32_t>(c.Find("rows")->number); ++r) {
        out.emplace_back();
        rules->At(a.row + r, a.col, out.back());
      }
    }
    return out;
  }
};

}  // namespace

// Every case of tests/sheet/condfmt-expected.json, Excel's answers (excel_condfmt.py): each cell's
// fill, font color, bold and italic as DisplayFormat gives them.
TEST(SheetCondFmt_Excel) {
  Cases t;
  CHECK(t.book && t.expected && t.rules);
  if (!t.rules) return;
  CHECK(t.rules->RuleCount() > 90);
  int checked = 0, failed = 0;
  for (const JsonValue& c : t.expected->Find("cases")->array) {
    CellAddr first;
    ParseA1(c.StringOr("column") + "2", first);
    const uint32_t rows = static_cast<uint32_t>(c.Find("rows")->number);
    const auto& cells = c.Find("cells")->array;
    for (uint32_t k = 0; k < cells.size(); ++k) {
      const uint32_t row = first.row + k % rows, col = first.col + k / rows;
      condfmt::Style s;
      t.rules->At(row, col, s);
      const JsonValue& want = cells[k];
      const JsonValue* fill = want.Find("fill");
      bool same = true;
      if (fill->type == JsonValue::Type::Null) {
        same = !(s.set & condfmt::Style::kFill);
      } else {
        same = (s.set & condfmt::Style::kFill) && Hex(s.fillColor) == fill->string;
        // A theme color's tint (the model's ApplyTint) lands exactly on halves for accent 1 at +0.5
        // (161.5, 184.5, 225.5), which Excel's arithmetic rounds either way: 1 either way is fine.
        if (!same && (s.set & condfmt::Style::kFill) && c.StringOr("name") == "scale theme") {
          const uint32_t w = std::stoul(fill->string, nullptr, 16);
          same = true;
          for (int shift = 0; shift < 24; shift += 8)
            same = same && std::abs(static_cast<int>((s.fillColor >> shift) & 0xFF) - static_cast<int>((w >> shift) & 0xFF)) <= 1;
        }
      }
      const std::string font = want.StringOr("font");
      const std::string gotFont = s.set & condfmt::Style::kFontColor ? Hex(s.fontColor) : "000000";
      same = same && gotFont == font;
      same = same && want.Find("bold")->boolean == ((s.set & condfmt::Style::kBold) && s.bold);
      same = same && want.Find("italic")->boolean == ((s.set & condfmt::Style::kItalic) && s.italic);
      ++checked;
      if (!same) {
        if (++failed <= 40)
          std::printf("  %s, %s: fill %s font %s bold %d italic %d; want fill %s font %s bold %d italic %d\n",
                      c.StringOr("name").c_str(), CellName(row, col).c_str(),
                      s.set & condfmt::Style::kFill ? Hex(s.fillColor).c_str() : "none", gotFont.c_str(),
                      (s.set & condfmt::Style::kBold) && s.bold, (s.set & condfmt::Style::kItalic) && s.italic,
                      fill->type == JsonValue::Type::Null ? "none" : fill->string.c_str(), font.c_str(),
                      want.Find("bold")->boolean, want.Find("italic")->boolean);
        CHECK(false);
      }
    }
  }
  std::printf("  %d cells checked, %d differ\n", checked, failed);
}

// Data bars and icons aren't in DisplayFormat: their cases, worked out by hand.
TEST(SheetCondFmt_BarsAndIcons) {
  Cases t;
  if (!t.rules) {
    CHECK(false);
    return;
  }
  // 1, 5, 7.5, -2, 3, 10, (empty), "x", 0. Automatic ends: -2 and 10; the axis at 2/12.
  const auto bars = t.Column("data bar");
  CHECK(bars.size() == 9);
  if (bars.size() == 9) {
    const float axis = 2.0f / 12;
    auto approx = [](float a, float b) { return a > b - 1e-4f && a < b + 1e-4f; };
    CHECK(bars[5].set & condfmt::Style::kBar);
    CHECK(approx(bars[5].bar.axis, axis) && approx(bars[5].bar.from, axis) && approx(bars[5].bar.to, 1));
    CHECK(approx(bars[1].bar.from, axis) && approx(bars[1].bar.to, axis + (1 - axis) / 2));
    CHECK(bars[3].bar.negative && approx(bars[3].bar.from, 0) && approx(bars[3].bar.to, axis));
    CHECK(bars[3].bar.fill == 0xFFFF0000u && bars[5].bar.fill == 0xFF638EC6u);
    CHECK(bars[5].bar.border == 0xFF638EC6u && bars[3].bar.border == 0xFFFF0000u);
    CHECK(approx(bars[8].bar.from, axis) && approx(bars[8].bar.to, axis));
    CHECK(!(bars[6].set & condfmt::Style::kBar) && !(bars[7].set & condfmt::Style::kBar));
    CHECK(bars[5].bar.gradient);
  }
  // 1, 5, 7.5, -2, 3, 10, 5, 0, 12: 33 % and 67 % of -2..12 are 2.62 and 7.38.
  const auto arrows = t.Column("icons 3 arrows");
  const int want[] = {0, 1, 2, 0, 1, 2, 1, 0, 2};
  CHECK(arrows.size() == 9);
  for (size_t i = 0; i < arrows.size() && i < 9; ++i)
    CHECK((arrows[i].set & condfmt::Style::kIcon) && arrows[i].iconSet == condfmt::IconSet::Arrows3 && arrows[i].icon == want[i]);
  // Custom icons: >= 3 none, > 7.5 a flag (3Flags' third), else a star (3Stars' first).
  const auto custom = t.Column("icons custom");
  CHECK(custom.size() == 9);
  if (custom.size() == 9) {
    CHECK(custom[0].iconSet == condfmt::IconSet::Stars3 && custom[0].icon == 0);
    CHECK(!(custom[1].set & condfmt::Style::kIcon) && !(custom[2].set & condfmt::Style::kIcon));
    CHECK(custom[5].iconSet == condfmt::IconSet::Flags3 && custom[5].icon == 2);
    CHECK(!(custom[4].set & condfmt::Style::kIcon));
  }
}

// Rules written by hand: Excel 2007's data bars, number formats, borders, a sheet without rules,
// and what changes after an edit.
TEST(SheetCondFmt_Written) {
  const std::string sheet =
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>"
      "<row r=\"1\"><c r=\"A1\"><v>0</v></c><c r=\"B1\"><v>5</v></c></row>"
      "<row r=\"2\"><c r=\"A2\"><v>50</v></c><c r=\"B2\"><v>6</v></c></row>"
      "<row r=\"3\"><c r=\"A3\"><v>100</v></c><c r=\"B3\"><v>7</v></c></row></sheetData>"
      "<conditionalFormatting sqref=\"A1:A3\"><cfRule type=\"dataBar\" priority=\"2\"><dataBar showValue=\"0\">"
      "<cfvo type=\"min\"/><cfvo type=\"max\"/><color rgb=\"FF00FF00\"/></dataBar></cfRule></conditionalFormatting>"
      "<conditionalFormatting sqref=\"B1:B3\"><cfRule type=\"cellIs\" dxfId=\"0\" priority=\"1\" operator=\"greaterThan\">"
      "<formula>5</formula></cfRule></conditionalFormatting></worksheet>";
  const std::string styles =
      "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
      "<dxfs count=\"1\"><dxf><numFmt numFmtId=\"164\" formatCode=\"0.00\"/>"
      "<border><left style=\"thin\"><color rgb=\"FF112233\"/></left><bottom style=\"double\"/></border></dxf></dxfs></styleSheet>";
  const std::string workbook =
      "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
      "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
      "<sheets><sheet name=\"S\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>";
  const std::vector<std::pair<std::string, std::string>> parts = {
      {"[Content_Types].xml",
       "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
       "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
       "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
       "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
       "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
       "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/></Types>"},
      {"_rels/.rels",
       "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
       "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/></Relationships>"},
      {"xl/workbook.xml", workbook},
      {"xl/_rels/workbook.xml.rels",
       "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
       "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
       "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/></Relationships>"},
      {"xl/styles.xml", styles},
      {"xl/worksheets/sheet1.xml", sheet}};
  pkg::Status st;
  auto pkg = test::OpenBytes(test::MakeZip(parts), st);
  CHECK(pkg != nullptr);
  if (!pkg) return;
  xlsx::ReadOptions ro;
  ro.loadAll = true;
  auto book = xlsx::Read(std::move(pkg), st, ro);
  CHECK(book != nullptr);
  if (!book) return;
  condfmt::SheetRules rules(*book, 0);  // no engine: constants only
  CHECK(rules.RuleCount() == 2);
  condfmt::Style s;
  // Excel 2007's bars: 10 % to 90 % of the width, the value hidden.
  CHECK(rules.At(0, 0, s) && (s.set & condfmt::Style::kBar) && (s.set & condfmt::Style::kHideValue));
  CHECK(s.bar.from == 0 && s.bar.to > 0.0999f && s.bar.to < 0.1001f && s.bar.axis < 0 && s.bar.fill == 0xFF00FF00u);
  CHECK(rules.At(1, 0, s) && s.bar.to > 0.4999f && s.bar.to < 0.5001f);
  CHECK(rules.At(2, 0, s) && s.bar.to > 0.8999f && s.bar.to < 0.9001f);
  // A number format and borders from the differential format.
  CHECK(!rules.At(0, 1, s) && s.set == 0);
  CHECK(rules.At(1, 1, s) && (s.set & condfmt::Style::kNumFmt) && s.numFmt && (s.set & condfmt::Style::kBorder));
  CHECK(s.left.style == BorderStyle::Thin && s.left.color == 0xFF112233u && s.bottom.style == BorderStyle::Double);
  CHECK(s.top.style == BorderStyle::None && !(s.set & condfmt::Style::kFill));
  CHECK(!rules.At(5, 5, s));
  // After an edit, the bars follow the new range.
  book->GetSheet(0).SetValue(2, 0, Value::Number(200));
  rules.Changed();
  CHECK(rules.At(1, 0, s) && s.bar.to > 0.0999f + 0.8f * 0.25f - 0.0002f && s.bar.to < 0.1001f + 0.8f * 0.25f);
}
