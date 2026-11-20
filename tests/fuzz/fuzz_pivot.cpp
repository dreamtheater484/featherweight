// Fuzz target: pivot tables (S4 s4-pivot; sheet/pivot/Pivot.h). The input is a pivot cache
// definition, its records and a pivot table definition, separated by NUL bytes; each is read, the
// table laid out from the cache, and the three written back and read again.
// Seeds: tests/fuzz/seeds/pivot (make_pivot_seeds.py, from the Excel-made fixtures).
//
// A layout must stay inside the sheet and inside its own area, every cell of it once; what the
// writer writes must read again with the same fields, records and lines.
#include <cstdint>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "sheet/pivot/Pivot.h"

namespace {

void Require(bool ok) {
  if (!ok) std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace fw::sheet;
  std::string_view in(reinterpret_cast<const char*>(data), size);
  std::string_view parts[3];
  for (int i = 0; i < 3; ++i) {
    size_t nul = in.find('\0');
    parts[i] = in.substr(0, nul);
    in = nul == std::string_view::npos ? std::string_view() : in.substr(nul + 1);
  }
  pivot::Cache cache;
  if (!pivot::ReadCache(parts[0], cache)) return 0;
  if (!parts[1].empty() && !pivot::ReadRecords(parts[1], cache)) cache.records.clear();
  pivot::Table table;
  if (!pivot::ReadTable(parts[2], table)) return 0;
  pivot::Layout lay;
  std::u16string why;
  if (pivot::Lay(cache, table, lay, why)) {
    Require(lay.area.lastRow < kMaxRows && lay.area.lastCol < kMaxCols);
    Require(lay.area.firstRow <= lay.location.firstRow && lay.location.lastRow == lay.area.lastRow);
    std::set<std::pair<uint32_t, uint32_t>> seen;
    for (const pivot::OutCell& c : lay.cells) {
      Require(lay.area.Contains(c.at.row, c.at.col));
      Require(seen.insert({c.at.row, c.at.col}).second);
    }
    Require(table.location == lay.location);
  }
  // Written and read again.
  std::string cacheXml = pivot::WriteCache(parts[0], cache, 46000.5);
  std::string recordsXml = pivot::WriteRecords(parts[1], cache);
  std::string tableXml = pivot::WriteTable(parts[2], table);
  pivot::Cache again;
  Require(pivot::ReadCache(cacheXml, again));
  Require(again.fields.size() == cache.fields.size());
  for (size_t f = 0; f < cache.fields.size(); ++f)
    if (cache.fields[f].hasItems) Require(again.fields[f].items.size() == cache.fields[f].items.size());
  if (cache.records.size() == cache.RecordCount() * cache.fields.size() && !cache.fields.empty()) {
    bool fits = true;
    for (size_t f = 0; f < cache.fields.size(); ++f)
      for (size_t r = 0; r < cache.RecordCount() && fits; ++r) fits = cache.At(r, f) < cache.fields[f].items.size();
    if (fits) {
      Require(pivot::ReadRecords(recordsXml, again));
      Require(again.RecordCount() == cache.RecordCount());
    }
  }
  pivot::Table back;
  Require(pivot::ReadTable(tableXml, back));
  Require(back.fields.size() == table.fields.size() && back.rowItems.size() == table.rowItems.size() &&
          back.colItems.size() == table.colItems.size() && back.location == table.location);
  return 0;
}
