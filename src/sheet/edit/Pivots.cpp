// Pivot tables in the edit layer (S4 s4-pivot): Editor::RefreshPivots's work, through the editor's
// public calls inside one group, so undo, redo and the journal come with them (Edit.h).
#include <algorithm>
#include <ctime>
#include <map>
#include <set>

#include "pkg/Package.h"
#include "sheet/edit/Edit.h"
#include "sheet/edit/PivotEdit.h"
#include "sheet/pivot/Pivot.h"

namespace fw::sheet::edit {

namespace {

std::string Bytes(const Editor& ed, const std::string& part) { return ed.PartBytes(part); }

double NowSerial() {
  std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  // Days since 1899-12-30, from the civil date (Howard Hinnant's days_from_civil).
  int y = local.tm_year + 1900, m = local.tm_mon + 1, d = local.tm_mday;
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const double days = era * 146097.0 + doe - 719468.0 + 25569.0;
  return days + (local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec) / 86400.0;
}

bool Overlaps(const Range& a, const Range& b) {
  return a.firstRow <= b.lastRow && b.firstRow <= a.lastRow && a.firstCol <= b.lastCol && b.firstCol <= a.lastCol;
}

CellInput Input(const pivot::Item& v) {
  switch (v.kind) {
    case pivot::Item::Kind::Number:
    case pivot::Item::Kind::Date: return CellInput::Number(v.number);
    case pivot::Item::Kind::String: return CellInput::Text(v.text);
    case pivot::Item::Kind::Bool: return CellInput::Bool(v.number != 0);
    case pivot::Item::Kind::Error: {
      ErrorCode e = ErrorCode::NA;
      ParseError(v.text, e);
      return CellInput::Error(e);
    }
    default: return CellInput::Empty();
  }
}

struct Loaded {
  pivot::Place place;
  pivot::Table table;
  Range before;  // its area before the refresh, page fields included
};

}  // namespace

Result RefreshPivotsIn(Editor& ed, Workbook& book, Editor::PivotRefresh which, int sheet, CellAddr cell) {
  const pkg::Package* package = book.GetPackage();
  if (!package) return Result::Fail(u"There's no PivotTable here.");
  // Every pivot of the workbook, read.
  std::vector<Loaded> tables;
  std::map<std::string, pivot::Cache> caches;
  for (const pivot::Place& p : pivot::List(book)) {
    Loaded l;
    l.place = p;
    l.table.part = p.table;
    l.table.cachePart = p.cache;
    if (p.cache.empty() || !pivot::ReadTable(Bytes(ed, p.table), l.table)) continue;
    l.before = pivot::AreaOf(l.table.location, l.table.rowPageCount);
    tables.push_back(std::move(l));
  }
  // The caches to refresh.
  std::set<std::string> chosen;
  for (const Loaded& l : tables) {
    if (which == Editor::PivotRefresh::All) chosen.insert(l.place.cache);
    else if (which == Editor::PivotRefresh::AtCell && static_cast<int>(l.place.sheet) == sheet &&
             l.before.firstRow <= cell.row && cell.row <= l.before.lastRow && l.before.firstCol <= cell.col &&
             cell.col <= l.before.lastCol)
      chosen.insert(l.place.cache);
  }
  for (const std::string& part : chosen) {
    pivot::Cache c;
    c.part = part;
    if (!pivot::ReadCache(Bytes(ed, part), c)) return Result::Fail(u"This PivotTable's data can't be read.");
    if (const pkg::RelationshipSet* rels = package->RelationshipsOf(part))
      for (const pkg::Relationship& r : rels->items)
        if (!r.external && r.type.ends_with("/pivotCacheRecords") && package->FindPart(r.targetPart)) c.recordsPart = r.targetPart;
    if (!c.recordsPart.empty() && !pivot::ReadRecords(Bytes(ed, c.recordsPart), c)) c.records.clear();
    if (which == Editor::PivotRefresh::OnLoad && !c.refreshOnLoad) continue;
    caches[part] = std::move(c);
  }
  if (which == Editor::PivotRefresh::OnLoad && caches.empty()) return Result{};
  if (caches.empty()) return Result::Fail(u"There's no PivotTable here.");

  // Refresh and lay out everything first: nothing changes unless every one of them can.
  struct Out {
    Loaded* table;
    pivot::Layout layout;
  };
  std::vector<Out> outs;
  for (auto& [part, cache] : caches) {
    std::vector<bool> axis(cache.fields.size(), false);
    for (Loaded& l : tables)
      if (l.place.cache == part) {
        if (!l.table.why.empty()) return Result::Fail(u"This PivotTable uses features Featherweight keeps but doesn't refresh.");
        for (size_t f = 0; f < l.table.fields.size() && f < axis.size(); ++f) {
          const pivot::Axis a = l.table.fields[f].axis;
          axis[f] = axis[f] || a == pivot::Axis::Row || a == pivot::Axis::Col || a == pivot::Axis::Page;
        }
      }
    std::vector<std::vector<uint32_t>> remap;
    std::u16string why;
    if (!pivot::RefreshCache(book, cache, axis, remap, why)) return Result::Fail(why);
    for (Loaded& l : tables)
      if (l.place.cache == part) {
        pivot::RemapItems(l.table, cache, remap);
        Out o{&l, {}};
        if (!pivot::Lay(cache, l.table, o.layout, why)) return Result::Fail(why);
        outs.push_back(std::move(o));
      }
  }
  // No pivot may cover another, or cells that aren't its own.
  for (const Out& o : outs) {
    const size_t s = o.table->place.sheet;
    for (const Loaded& other : tables)
      if (&other != o.table && other.place.sheet == s && Overlaps(o.layout.area, other.before))
        return Result::Fail(u"A PivotTable report cannot overlap another PivotTable report.");
    if (!book.LoadSheet(s).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    bool taken = false;
    book.GetSheet(s).ForEachCell(o.layout.area, [&](const CellInfo& c) {
      if (o.table->before.Contains(c.row, c.col) || c.value.type == ValueType::Empty) return true;
      taken = true;
      return false;
    });
    if (taken)
      return Result::Fail(u"There's already data in the cells this PivotTable needs. Move it or the PivotTable, and refresh again.");
  }

  EditGroup group(ed, which == Editor::PivotRefresh::AtCell ? u"Refresh" : u"Refresh All");
  const double now = NowSerial();
  for (auto& [part, cache] : caches) {
    if (!ed.WritePart(part, pivot::WriteCache(Bytes(ed, part), cache, now))) return Result::Fail(u"The PivotTable couldn't be refreshed.");
    if (!cache.recordsPart.empty() && !ed.WritePart(cache.recordsPart, pivot::WriteRecords(Bytes(ed, cache.recordsPart), cache)))
      return Result::Fail(u"The PivotTable couldn't be refreshed.");
  }
  std::map<std::pair<uint32_t, uint8_t>, uint32_t> styles;  // (number format, indent) -> cell format
  for (Out& o : outs) {
    const int s = static_cast<int>(o.table->place.sheet);
    if (!ed.WritePart(o.table->place.table, pivot::WriteTable(Bytes(ed, o.table->place.table), o.table->table)))
      return Result::Fail(u"The PivotTable couldn't be refreshed.");
    if (Result r = ed.ClearAll(s, o.table->before); !r) return r;
    std::vector<std::pair<CellAddr, CellInput>> cells;
    cells.reserve(o.layout.cells.size());
    for (const pivot::OutCell& c : o.layout.cells) {
      CellInput in = Input(c.value);
      if (c.numFmtId != kNone || c.indent) {
        auto key = std::make_pair(c.numFmtId, c.indent);
        auto it = styles.find(key);
        if (it == styles.end()) {
          Xf xf = book.GetStyles().CellXfs().empty() ? Xf{} : book.GetStyles().CellXfs()[0];
          if (c.numFmtId != kNone) xf.numFmt = c.numFmtId, xf.applyNumFmt = true;
          if (c.indent) xf.alignment.horizontal = HAlign::Left, xf.alignment.indent = c.indent, xf.applyAlignment = true;
          it = styles.emplace(key, ed.AddXf(xf)).first;
        }
        in.WithStyle(it->second);
      }
      if (in.type != ValueType::Empty || in.style) cells.emplace_back(c.at, std::move(in));
    }
    if (!cells.empty())
      if (Result r = ed.SetCells(s, cells); !r) return r;
  }
  group.done = true;
  return Result{};
}

}  // namespace fw::sheet::edit
