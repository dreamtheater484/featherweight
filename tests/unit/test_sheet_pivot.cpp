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

std::vector<uint8_t> SavePivotBook(Workbook& book, fw::sheet::edit::Editor& ed) {
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
    std::ofstream out(std::string(keep) + "/pivots-featherweight.xlsx", std::ios::binary);
    out.write(reinterpret_cast<const char*>(sink.bytes.data()), static_cast<std::streamsize>(sink.bytes.size()));
  }
  std::free(keep);
  return sink.bytes;
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
