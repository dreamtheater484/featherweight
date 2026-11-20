// Pivot tables (S4, s4-pivot): read, refreshed and laid out as Excel does. The expected values are
// Excel's own: tests/office/sheet/pivots, made by tests/sheet/excel_pivots.py (fresh pivots, the
// source changed without refreshing, and after Excel's Refresh All).
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <cstdlib>
#include <fstream>
#include <set>

#include "SheetTestUtil.h"
#include "pkg/Package.h"
#include "sheet/edit/Edit.h"
#include "sheet/pivot/Pivot.h"

using namespace fw::sheet;

namespace {

std::string PartText(const Workbook& book, const std::string& part) {
  std::vector<uint8_t> bytes;
  if (!book.GetPackage()->ReadPart(part, bytes).Ok()) return {};
  return std::string(bytes.begin(), bytes.end());
}

struct Pivots {
  std::map<std::string, pivot::Cache> caches;
  std::vector<std::pair<size_t, pivot::Table>> tables;  // sheet, table
};

bool ReadAll(const Workbook& book, Pivots& out) {
  for (const pivot::Place& p : pivot::List(book)) {
    if (!out.caches.count(p.cache)) {
      pivot::Cache c;
      c.part = p.cache;
      if (!pivot::ReadCache(PartText(book, p.cache), c)) return false;
      if (const fw::pkg::RelationshipSet* rels = book.GetPackage()->RelationshipsOf(p.cache))
        for (const fw::pkg::Relationship& r : rels->items)
          if (r.type.ends_with("/pivotCacheRecords")) c.recordsPart = r.targetPart;
      if (!c.recordsPart.empty() && !pivot::ReadRecords(PartText(book, c.recordsPart), c)) return false;
      out.caches[p.cache] = std::move(c);
    }
    pivot::Table t;
    t.part = p.table;
    t.cachePart = p.cache;
    if (!pivot::ReadTable(PartText(book, p.table), t)) return false;
    out.tables.push_back({p.sheet, std::move(t)});
  }
  return true;
}

// Every cell of `area` on `sheet` against the layout: the same values (numbers to 1e-12).
int Compare(const Sheet& sheet, const pivot::Layout& lay, const Range& area, const std::string& label) {
  std::map<std::pair<uint32_t, uint32_t>, const pivot::OutCell*> ours;
  for (const pivot::OutCell& c : lay.cells) ours[{c.at.row, c.at.col}] = &c;
  int bad = 0;
  for (uint32_t r = area.firstRow; r <= area.lastRow; ++r)
    for (uint32_t c = area.firstCol; c <= area.lastCol; ++c) {
      Value v = sheet.GetValue(r, c);
      auto it = ours.find({r, c});
      pivot::Item mine = it == ours.end() ? pivot::Item{} : it->second->value;
      bool same = false;
      std::string want, got;
      switch (v.type) {
        case ValueType::Empty: same = mine.kind == pivot::Item::Kind::Missing; want = "(empty)"; break;
        case ValueType::String:
          same = mine.kind == pivot::Item::Kind::String && mine.text == v.text;
          want = std::string(v.text);
          break;
        case ValueType::Number:
          same = (mine.kind == pivot::Item::Kind::Number || mine.kind == pivot::Item::Kind::Date) &&
                 std::fabs(mine.number - v.number) <= 1e-12 * std::max(1.0, std::fabs(v.number));
          want = std::to_string(v.number);
          break;
        case ValueType::Bool: same = mine.kind == pivot::Item::Kind::Bool && (mine.number != 0) == v.boolean; want = "bool"; break;
        case ValueType::Error: same = mine.kind == pivot::Item::Kind::Error && mine.text == ErrorText(v.error); want = "error"; break;
      }
      if (same) continue;
      switch (mine.kind) {
        case pivot::Item::Kind::Missing: got = "(empty)"; break;
        case pivot::Item::Kind::String:
        case pivot::Item::Kind::Error: got = mine.text; break;
        default: got = std::to_string(mine.number); break;
      }
      if (++bad <= 12) std::printf("  %s %s: Excel '%s', ours '%s'\n", label.c_str(), CellName(r, c).c_str(), want.c_str(), got.c_str());
    }
  return bad;
}

Range Union(const Range& a, const Range& b) {
  return {std::min(a.firstRow, b.firstRow), std::min(a.firstCol, b.firstCol), std::max(a.lastRow, b.lastRow),
          std::max(a.lastCol, b.lastCol)};
}

// Lays out every pivot of `book` (refreshing its cache from the cells first when `refresh`) and
// compares with `expected`'s cells: the same sheets by name.
int CheckBook(Workbook& book, const Workbook& expected, bool refresh) {
  Pivots p;
  CHECK(ReadAll(book, p));
  CHECK(!p.tables.empty());
  if (refresh)
    for (auto& [part, cache] : p.caches) {
      std::vector<bool> axis(cache.fields.size(), false);
      for (auto& [sheet, t] : p.tables)
        if (t.cachePart == part)
          for (size_t f = 0; f < t.fields.size() && f < axis.size(); ++f)
            axis[f] = axis[f] || t.fields[f].axis == pivot::Axis::Row || t.fields[f].axis == pivot::Axis::Col ||
                      t.fields[f].axis == pivot::Axis::Page;
      std::vector<std::vector<uint32_t>> remap;
      std::u16string why;
      CHECK(pivot::RefreshCache(book, cache, axis, remap, why));
      for (auto& [sheet, t] : p.tables)
        if (t.cachePart == part) pivot::RemapItems(t, cache, remap);
    }
  int bad = 0;
  for (auto& [sheet, t] : p.tables) {
    const Range before = t.location;
    pivot::Layout lay;
    std::u16string why;
    const bool laid = pivot::Lay(p.caches[t.cachePart], t, lay, why);
    CHECK(laid);
    if (!laid) {
      std::printf("  %s: not laid out\n", t.name.c_str());
      ++bad;
      continue;
    }
    const std::string& name = book.GetSheet(sheet).Name();
    const int other = expected.FindSheet(name);
    CHECK(other >= 0);
    if (other < 0) continue;
    // Excel's own pivot on that sheet, by name, for its location.
    Range area = Union(lay.area, before);
    for (const pivot::Place& q : pivot::List(expected)) {
      pivot::Table e;
      if (q.sheet == static_cast<size_t>(other) && pivot::ReadTable(PartText(expected, q.table), e) && e.name == t.name) {
        CHECK(e.location == t.location);
        if (!(e.location == t.location))
          std::printf("  %s: location %s, Excel's %s\n", t.name.c_str(), RangeName(t.location).c_str(), RangeName(e.location).c_str());
        CHECK_EQ(e.firstHeaderRow, t.firstHeaderRow);
        CHECK_EQ(e.firstDataRow, t.firstDataRow);
        CHECK_EQ(e.firstDataCol, t.firstDataCol);
        area = Union(area, e.location);
      }
    }
    bad += Compare(expected.GetSheet(static_cast<size_t>(other)), lay, area, name + "/" + t.name);
  }
  return bad;
}

}  // namespace

TEST(Pivot_LayoutMatchesExcelsFresh) {
  auto book = sheet_test::OpenZip(sheet_test::TestFile("office/sheet/pivots/pivots-fresh.xlsx"));
  if (!book) return;
  CHECK_EQ(CheckBook(*book, *book, false), 0);
}

// Column fields and no row fields, as Excel lays them out (pivots-colonly.xlsx, excel_pivots.py
// --colonly): a label column only for a single value, its name there; none for two values or none;
// with no values, no line of values in the location (fuzz_pivot found the one-value case laid twice).
TEST(Pivot_ColumnsOnlyMatchExcel) {
  auto book = sheet_test::OpenZip(sheet_test::TestFile("office/sheet/pivots/pivots-colonly.xlsx"));
  if (!book) return;
  CHECK(book->FindSheet("ColOne") >= 0 && book->FindSheet("ColNone") >= 0);
  CHECK_EQ(CheckBook(*book, *book, false), 0);
}

TEST(Pivot_RefreshMatchesExcelsRefresh) {
  auto stale = sheet_test::OpenZip(sheet_test::TestFile("office/sheet/pivots/pivots-stale.xlsx"));
  auto refreshed = sheet_test::OpenZip(sheet_test::TestFile("office/sheet/pivots/pivots-refreshed.xlsx"));
  if (!stale || !refreshed) return;
  CHECK_EQ(CheckBook(*stale, *refreshed, true), 0);
}

TEST(Pivot_RefreshKeepsExcelsValues) {
  auto book = sheet_test::OpenZip(sheet_test::TestFile("office/sheet/pivots/pivots-refreshed.xlsx"));
  auto same = sheet_test::OpenZip(sheet_test::TestFile("office/sheet/pivots/pivots-refreshed.xlsx"));
  if (!book || !same) return;
  CHECK_EQ(CheckBook(*book, *same, true), 0);
}

// The real-world corpus's pivots (D:\Documents\Codex\FeatherweightCorpus, when this PC has it):
// refreshed from their unchanged source, they show what Excel last saved.
TEST(Pivot_CorpusRefresh) {
  const std::filesystem::path file = "D:/Documents/Codex/FeatherweightCorpus/xlsx/features/poi-withchartsheet-xlsx-chart-sheet.xlsx";
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) return;
  std::ifstream in(file, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto book = sheet_test::OpenZip(bytes);
  auto same = sheet_test::OpenZip(bytes);
  if (!book || !same) return;
  CHECK_EQ(CheckBook(*book, *same, true), 0);
}

namespace {

std::vector<uint8_t> SavePivotBook(Workbook& book, fw::sheet::edit::Editor& ed, const char* keepName = "pivots-featherweight") {
  ed.BeginSave();
  xlsx::SavePlan plan;
  fw::pkg::Status st = xlsx::PrepareSave(book, ed, plan);
  CHECK(st.Ok());
  fw::pkg::MemorySink sink;
  if (st.Ok()) st = xlsx::WriteSave(book, plan, sink);
  CHECK(st.Ok());
  if (st.Ok()) xlsx::CommitSave(book, ed, plan);
  else ed.EndSave();
  char* keep = nullptr;
  size_t len = 0;
  if (_dupenv_s(&keep, &len, "FW_KEEP_PIVOTS") == 0 && keep) {  // for Excel's open check
    std::ofstream out(std::string(keep) + "/" + keepName + ".xlsx", std::ios::binary);
    out.write(reinterpret_cast<const char*>(sink.bytes.data()), static_cast<std::streamsize>(sink.bytes.size()));
  }
  std::free(keep);
  return sink.bytes;
}

// An edit's message for the log (ASCII; anything else as '?').
std::string Ascii(const std::u16string& s) {
  std::string out;
  for (char16_t ch : s) out += ch < 0x80 ? static_cast<char>(ch) : '?';
  return out;
}

}  // namespace

// Refresh All through the editor: the cells Excel shows, one undo step that brings the stale ones
// back, and a saved file whose parts lay out to the same cells again.
TEST(Pivot_EditorRefreshUndoSave) {
  auto book = sheet_test::OpenZip(sheet_test::TestFile("office/sheet/pivots/pivots-stale.xlsx"));
  auto stale = sheet_test::OpenZip(sheet_test::TestFile("office/sheet/pivots/pivots-stale.xlsx"));
  auto refreshed = sheet_test::OpenZip(sheet_test::TestFile("office/sheet/pivots/pivots-refreshed.xlsx"));
  if (!book || !stale || !refreshed) return;
  fw::sheet::edit::Editor ed(*book);
  auto r = ed.RefreshPivots(fw::sheet::edit::Editor::PivotRefresh::All);
  CHECK(r.ok);
  if (!r.ok) return;
  CHECK(ed.UndoName() == u"Refresh All");
  // Every refreshed pivot's sheet shows Excel's cells.
  auto sameCells = [](const Workbook& a, const Workbook& b) {
    int bad = 0;
    for (size_t i = 0; i < b.SheetCount(); ++i) {
      const Sheet& want = b.GetSheet(i);
      const int k = a.FindSheet(want.Name());
      if (k < 0) return -1;
      const Sheet& got = a.GetSheet(static_cast<size_t>(k));
      for (uint32_t row = 0; row < 60; ++row)
        for (uint32_t col = 0; col < 12; ++col) {
          Value x = got.GetValue(row, col), y = want.GetValue(row, col);
          bool same = x.type == y.type && (x.type != ValueType::String || x.text == y.text) &&
                      (x.type != ValueType::Number || std::fabs(x.number - y.number) <= 1e-12 * std::max(1.0, std::fabs(y.number)));
          if (!same && ++bad <= 8) std::printf("  %s!%s differs\n", want.Name().c_str(), CellName(row, col).c_str());
        }
    }
    return bad;
  };
  CHECK_EQ(sameCells(*book, *refreshed), 0);
  CHECK(ed.Undo().ok);
  CHECK_EQ(sameCells(*book, *stale), 0);
  CHECK(ed.Redo().ok);
  CHECK_EQ(sameCells(*book, *refreshed), 0);
  // Saved and opened again: the parts lay out to what the cells show.
  std::vector<uint8_t> bytes = SavePivotBook(*book, ed);
  auto again = sheet_test::OpenZip(bytes);
  CHECK(again != nullptr);
  if (!again) return;
  CHECK_EQ(sameCells(*again, *refreshed), 0);
  auto copy = sheet_test::OpenZip(bytes);
  CHECK_EQ(CheckBook(*again, *copy, false), 0);
}

// Insert > PivotTable on the fixtures' Data sheet: rows, a column field, a filter and three values;
// the table laid out as Excel lays it out, one undo step, saved parts that read and lay out again.
TEST(Pivot_InsertPivot) {
  auto book = sheet_test::OpenZip(sheet_test::TestFile("office/sheet/pivots/pivots-fresh.xlsx"));
  if (!book) return;
  fw::sheet::edit::Editor ed(*book);
  const int data = book->FindSheet("Data");
  CHECK(data >= 0);
  fw::sheet::edit::Editor::NewPivotTable p;
  p.sourceSheet = data;
  p.source = {0, 0, 20, 5};  // A1:F21
  p.sheet = data;
  p.at = {1, 8};  // I2: the filter there, the table at I4
  p.rows = {0};
  p.cols = {2};
  p.filters = {3};
  p.values = {{4, 0}, {5, 1}, {5, 2}};
  auto r = ed.InsertPivot(p);
  if (!r.ok) std::printf("  %s\n", Ascii(r.message).c_str());
  CHECK(r.ok);
  if (!r.ok) return;
  CHECK(ed.UndoName() == u"PivotTable");
  const Sheet& s = book->GetSheet(static_cast<size_t>(data));
  CHECK(sheet_test::Text(s, 1, 8) == "Year");
  CHECK(sheet_test::Text(s, 1, 9) == "(All)");
  // Three values: the caption row, the quarters, the values' names with "Row Labels", then the regions.
  CHECK(sheet_test::Text(s, 3, 9) == "Column Labels");
  CHECK(sheet_test::Text(s, 4, 9) == "Q1");
  CHECK(sheet_test::Text(s, 5, 8) == "Row Labels");
  CHECK(sheet_test::Text(s, 5, 9) == "Sum of Sales");
  CHECK(sheet_test::Text(s, 6, 8) == "East");
  std::vector<uint8_t> bytes = SavePivotBook(*book, ed, "pivots-inserted");
  auto again = sheet_test::OpenZip(bytes);
  auto copy = sheet_test::OpenZip(bytes);
  CHECK(again && copy);
  if (again && copy) CHECK_EQ(CheckBook(*again, *copy, false), 0);
  CHECK(ed.Undo().ok);
  CHECK(sheet_test::Text(book->GetSheet(static_cast<size_t>(data)), 1, 8).empty());
}

// Insert > PivotTable on a new worksheet, as the panel does it: the sheet added in the same step
// (it has no part until then), the pivot at A3 with its filter at A1; saved, the parts read and
// lay out again; one undo takes both away.
TEST(Pivot_InsertPivotOnNewSheet) {
  auto book = sheet_test::OpenZip(sheet_test::TestFile("office/sheet/pivots/pivots-fresh.xlsx"));
  if (!book) return;
  fw::sheet::edit::Editor ed(*book);
  const int data = book->FindSheet("Data");
  CHECK(data >= 0);
  if (data < 0) return;
  const size_t sheets = book->SheetCount();
  {
    fw::sheet::edit::EditGroup group(ed, u"PivotTable");
    auto added = ed.AddSheet(data);
    CHECK(added.ok);
    if (!added.ok) return;
    fw::sheet::edit::Editor::NewPivotTable p;
    p.sourceSheet = data + 1;  // moved right by the new sheet
    p.source = {0, 0, 20, 5};  // A1:F21
    p.sheet = data;
    p.at = {0, 0};  // the filter at A1, the table at A3
    p.rows = {0};
    p.filters = {3};
    p.values = {{4, 0}};
    auto r = ed.InsertPivot(p);
    if (!r.ok) std::printf("  %s\n", Ascii(r.message).c_str());
    CHECK(r.ok);
    if (!r.ok) return;
    group.done = true;
  }
  CHECK(ed.UndoName() == u"PivotTable");
  CHECK_EQ(book->SheetCount(), sheets + 1);
  const Sheet& s = book->GetSheet(static_cast<size_t>(data));
  CHECK(sheet_test::Text(s, 0, 0) == "Year");
  CHECK(sheet_test::Text(s, 0, 1) == "(All)");
  CHECK(sheet_test::Text(s, 2, 0) == "Row Labels");
  CHECK(sheet_test::Text(s, 2, 1) == "Sum of Sales");
  CHECK(sheet_test::Text(s, 3, 0) == "East");
  CHECK(sheet_test::Text(s, 7, 0) == "Grand Total");
  CHECK(std::fabs(s.GetValue(7, 1).number - 2513.79) < 1e-9);
  const std::string name = s.Name();
  std::vector<uint8_t> bytes = SavePivotBook(*book, ed, "pivots-newsheet");
  auto again = sheet_test::OpenZip(bytes);
  auto copy = sheet_test::OpenZip(bytes);
  CHECK(again && copy);
  if (again && copy) {
    CHECK_EQ(CheckBook(*again, *copy, false), 0);
    const int k = again->FindSheet(name);
    CHECK(k >= 0);
    if (k >= 0) {
      const auto looks = pivot::SheetLooks(*again, static_cast<size_t>(k));
      CHECK_EQ(looks.size(), size_t{1});
    }
  }
  CHECK(ed.Undo().ok);
  CHECK_EQ(book->SheetCount(), sheets);
}


// The PivotTable styles in the grid against what Excel shows: every cell of the fixtures' styled
// pivots (PivotStyleLight16, Light9, Medium2, Medium15, Medium9, Dark2), as Excel's DisplayFormat
// gave it (pivots-refreshed-styles.json, from excel_pivots.py): the fill, bold, and which sides
// have a line. Excel puts a line between two cells on one of them, so a side counts when either
// cell has it.
TEST(Pivot_StylesMatchExcel) {
  auto book = sheet_test::OpenZip(sheet_test::TestFile("office/sheet/pivots/pivots-refreshed.xlsx"));
  std::vector<uint8_t> json = sheet_test::TestFile("office/sheet/pivots/pivots-refreshed-styles.json");
  if (!book || json.empty()) return;
  const std::string text(json.begin(), json.end());
  auto field = [&](size_t from, const char* key) {
    const std::string k = std::string("\"") + key + "\":";
    size_t at = text.find(k, from);
    if (at == std::string::npos) return std::string();
    at += k.size();
    while (at < text.size() && text[at] == ' ') ++at;
    size_t end = at;
    if (text[at] == '"') end = text.find('"', ++at);
    else
      while (end < text.size() && text[end] != ',' && text[end] != '}' && text[end] != '\n') ++end;
    return text.substr(at, end - at);
  };
  std::map<std::string, std::vector<pivot::Look>> looks;
  int cells = 0, fills = 0, bolds = 0, sides = 0;
  for (size_t at = text.find('{'); at != std::string::npos; at = text.find('{', at + 1)) {
    const std::string sheet = field(at, "sheet"), cell = field(at, "cell"), want = field(at, "sides");
    const long fill = std::strtol(field(at, "fill").c_str(), nullptr, 10);
    const bool bold = field(at, "bold") == "true";
    const int s = book->FindSheet(sheet);
    CellAddr a;
    if (s < 0 || !ParseA1(cell, a)) continue;
    if (!looks.count(sheet)) looks[sheet] = pivot::SheetLooks(*book, static_cast<size_t>(s));
    const std::vector<pivot::Look>& l = looks[sheet];
    const TableCellFormat f = pivot::LookFormatAt(l, a.row, a.col);
    ++cells;
    const long mine = f.fill.pattern == PatternType::None ? -1
                                                         : static_cast<long>(book->GetStyles().Resolve(f.fill.fg, 0) & 0xFFFFFF);
    auto alike = [](long a, long b) {  // Excel's tints round a step differently
      if (a < 0 || b < 0) return a == b;
      for (int shift = 0; shift < 24; shift += 8)
        if (std::abs(((a >> shift) & 255) - ((b >> shift) & 255)) > 2) return false;
      return true;
    };
    if (alike(mine, fill)) ++fills;
    else if (fills + 10 > cells) std::printf("  %s!%s fill %06lX, Excel's %06lX\n", sheet.c_str(), cell.c_str(), mine, fill);
    if (f.bold == bold) ++bolds;
    else if (bolds + 10 > cells) std::printf("  %s!%s bold %d, Excel's %d\n", sheet.c_str(), cell.c_str(), f.bold, bold);
    // A side's line: this cell's, or its neighbor's on that side.
    auto has = [&](char side) {
      switch (side) {
        case 'l': return f.left.style != BorderStyle::None || (a.col > 0 && pivot::LookFormatAt(l, a.row, a.col - 1).right.style != BorderStyle::None);
        case 'r': return f.right.style != BorderStyle::None || pivot::LookFormatAt(l, a.row, a.col + 1).left.style != BorderStyle::None;
        case 't': return f.top.style != BorderStyle::None || (a.row > 0 && pivot::LookFormatAt(l, a.row - 1, a.col).bottom.style != BorderStyle::None);
        default: return f.bottom.style != BorderStyle::None || pivot::LookFormatAt(l, a.row + 1, a.col).top.style != BorderStyle::None;
      }
    };
    bool same = true;
    for (char side : std::string("ltbr")) same = same && has(side) == (want.find(side) != std::string::npos);
    if (same) ++sides;
    else if (sides + 10 > cells) std::printf("  %s!%s sides differ, Excel's '%s'\n", sheet.c_str(), cell.c_str(), want.c_str());
  }
  std::printf("  %d cells: fill %d, bold %d, lines %d the same as Excel's\n", cells, fills, bolds, sides);
  CHECK(cells > 500);
  CHECK(fills * 100 >= cells * 98);
  CHECK(bolds * 100 >= cells * 98);
  CHECK(sides * 100 >= cells * 98);
}
