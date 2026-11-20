// Insert > PivotTable in the edit layer (S4 s4-pivot): Editor::InsertPivot's work. The new cache,
// its records and the table are added as Excel adds them, workbook.xml lists the cache, and the
// refresh lays the table out (Pivots.cpp), all in one group.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <set>

#include "pkg/Package.h"
#include "pkg/XmlDom.h"
#include "sheet/edit/Edit.h"
#include "sheet/edit/PivotEdit.h"
#include "sheet/pivot/Pivot.h"

namespace fw::sheet::edit {

namespace {

constexpr std::string_view kRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
constexpr std::string_view kCt = "application/vnd.openxmlformats-officedocument.spreadsheetml.";

std::string HeaderName(const Value& v) {
  switch (v.type) {
    case ValueType::String: return std::string(v.text);
    case ValueType::Number: {
      char buf[40];
      std::snprintf(buf, sizeof buf, "%.15g", v.number);
      return buf;
    }
    case ValueType::Bool: return v.boolean ? "TRUE" : "FALSE";
    case ValueType::Error: return std::string(ErrorText(v.error));
    default: return {};
  }
}

Result Failed() { return Result::Fail(u"The PivotTable couldn't be added to the workbook."); }

// workbook.xml with <pivotCache cacheId r:id> added to its pivotCaches (made where Excel puts it).
bool ListCache(std::string& xml, uint32_t cacheId, const std::string& relId) {
  pkg::XmlDom dom;
  if (!dom.Parse(xml).Ok()) return false;
  pugi::xml_node root = dom.Root();
  std::string prefix, r = "r";
  if (std::string_view n = root.name(); n.find(':') != std::string_view::npos) prefix = std::string(n.substr(0, n.find(':') + 1));
  for (pugi::xml_attribute a = root.first_attribute(); a; a = a.next_attribute())
    if (std::string_view(a.name()).rfind("xmlns:", 0) == 0 &&
        std::string_view(a.value()).find("/officeDocument/2006/relationships") != std::string_view::npos)
      r = std::string(a.name() + 6);
  pugi::xml_node caches;
  for (pugi::xml_node c = root.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && pkg::LocalNameOf(c) == "pivotCaches") caches = c;
  if (!caches) {
    static constexpr std::string_view kBefore[] = {"fileVersion", "fileSharing", "workbookPr", "workbookProtection",
                                                   "bookViews", "sheets", "functionGroups", "externalReferences",
                                                   "definedNames", "calcPr", "oleSize", "customWorkbookViews"};
    pugi::xml_node prev;
    for (pugi::xml_node c = root.first_child(); c; c = c.next_sibling())
      if (c.type() == pugi::node_element &&
          std::find(std::begin(kBefore), std::end(kBefore), pkg::LocalNameOf(c)) != std::end(kBefore))
        prev = c;
    const std::string name = prefix + "pivotCaches";
    caches = prev ? root.insert_child_after(name.c_str(), prev) : root.prepend_child(name.c_str());
  }
  pugi::xml_node pc = caches.append_child((prefix + "pivotCache").c_str());
  pc.append_attribute("cacheId").set_value(cacheId);
  pc.append_attribute((r + ":id").c_str()).set_value(relId.c_str());
  xml = dom.Serialize();
  return true;
}

}  // namespace

Result InsertPivotIn(Editor& ed, Workbook& book, const Editor::NewPivotTable& p) {
  pkg::Package* package = book.GetPackage();
  if (!package) return Result::Fail(u"PivotTables need an Excel workbook: save this one as .xlsx first.");
  // The source.
  int srcSheet = p.sourceSheet;
  Range src = p.source;
  if (!p.table.empty()) {
    size_t at = 0;
    const Table* t = book.FindTable(p.table, &at);
    if (!t) return Result::Fail(u"The reference to the PivotTable's data source isn't valid.");
    srcSheet = static_cast<int>(at);
    src = t->ref;
    if (t->totalsRows && src.lastRow > src.firstRow) src.lastRow -= t->totalsRows;
  }
  if (srcSheet < 0 || static_cast<size_t>(srcSheet) >= book.SheetCount() || p.sheet < 0 ||
      static_cast<size_t>(p.sheet) >= book.SheetCount())
    return Result::Fail(u"There's no such sheet.");
  if (!book.LoadSheet(static_cast<size_t>(srcSheet)).Ok() || !book.LoadSheet(static_cast<size_t>(p.sheet)).Ok())
    return Result::Fail(u"This sheet couldn't be read.");
  const Sheet& from = book.GetSheet(static_cast<size_t>(srcSheet));
  const Sheet& to = book.GetSheet(static_cast<size_t>(p.sheet));
  if (to.Kind() != SheetKind::Worksheet) return Result::Fail(u"A PivotTable goes on a worksheet.");
  if (src.lastRow <= src.firstRow || src.lastCol - src.firstCol >= 16384 || src.lastRow >= kMaxRows)
    return Result::Fail(u"The PivotTable needs a header row and at least one row of data.");
  // Its fields: the header's names (one used twice gets 2, 3...), each with its first value's format.
  pivot::Cache cache;
  const uint32_t ncols = src.lastCol - src.firstCol + 1;
  for (uint32_t c = 0; c < ncols; ++c) {
    pivot::CacheField f;
    f.name = HeaderName(from.GetValue(src.firstRow, src.firstCol + c));
    if (f.name.empty())
      return Result::Fail(u"The PivotTable field name is not valid. To create a PivotTable, you must use data that is "
                          u"organized as a list with labeled columns.");
    const std::string base = f.name;
    for (int n = 2;; ++n) {
      bool taken = false;
      for (const pivot::CacheField& g : cache.fields) taken = taken || NameEqualsNoCase(g.name, f.name);
      if (!taken) break;
      f.name = base + std::to_string(n);
    }
    const uint32_t style = from.StyleAt(src.firstRow + 1, src.firstCol + c);
    if (style < book.GetStyles().CellXfs().size()) f.numFmtId = book.GetStyles().CellXfs()[style].numFmt;
    cache.fields.push_back(std::move(f));
  }
  if (p.table.empty()) {
    cache.sheet = from.Name();
    cache.ref = RangeName(src);
  } else {
    cache.name = p.table;
  }
  pivot::NewPivot spec;
  const std::pair<const std::vector<uint32_t>*, std::vector<uint32_t>*> lists[] = {
      {&p.rows, &spec.rows}, {&p.cols, &spec.cols}, {&p.filters, &spec.pages}};
  std::set<uint32_t> used;
  for (const auto& [in, out] : lists)
    for (uint32_t c : *in) {
      if (c >= ncols || !used.insert(c).second) return Result::Fail(u"A field can be in one area only: rows, columns or filters.");
      out->push_back(c);
    }
  static constexpr pivot::Func kFuncs[] = {pivot::Func::Sum, pivot::Func::Count, pivot::Func::Average, pivot::Func::Max,
                                           pivot::Func::Min};
  for (const Editor::NewPivotTable::Value& v : p.values) {
    if (v.column >= ncols || v.func >= std::size(kFuncs)) return Result::Fail(u"There's no such column in the PivotTable's source.");
    spec.values.push_back({v.column, kFuncs[v.func]});
  }
  if (spec.rows.empty() && spec.cols.empty() && spec.values.empty())
    return Result::Fail(u"Choose the fields for the PivotTable's rows, columns or values.");
  // A cache id and a name no other pivot has.
  const std::string wbPart = book.WorkbookPart();
  std::string wbXml = ed.PartBytes(wbPart);
  uint32_t cacheId = 0;
  for (size_t at = 0; (at = wbXml.find("cacheId=\"", at)) != std::string::npos;) {
    at += 9;
    cacheId = std::max<uint32_t>(cacheId, static_cast<uint32_t>(std::strtoul(wbXml.c_str() + at, nullptr, 10)) + 1);
  }
  std::set<std::string> names;
  for (const pivot::Place& place : pivot::List(book)) {
    pivot::Table t;
    if (pivot::ReadTable(ed.PartBytes(place.table), t)) names.insert(t.name);
  }
  std::string name;
  for (int n = 1;; ++n)
    if (!names.count(name = "PivotTable" + std::to_string(n))) break;
  const pivot::Table table = pivot::NewTable(cache, spec, name, cacheId, p.at);

  EditGroup group(ed, u"PivotTable");
  // A sheet added since opening gets its part first (s4 friction W7).
  if (ed.SheetPart(p.sheet, true).empty()) return Failed();
  const std::string cachePart = ed.NewPartName("/xl/pivotCache/pivotCacheDefinition", ".xml");
  const std::string wbRel = ed.AddPart(wbPart, std::string(kRel) + "pivotCacheDefinition", cachePart,
                                       std::string(kCt) + "pivotCacheDefinition+xml", pivot::NewCacheXml(cache));
  if (wbRel.empty()) return Failed();
  const std::string recordsPart = ed.NewPartName("/xl/pivotCache/pivotCacheRecords", ".xml");
  const std::string recRel =
      ed.AddPart(cachePart, std::string(kRel) + "pivotCacheRecords", recordsPart, std::string(kCt) + "pivotCacheRecords+xml",
                 "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<pivotCacheRecords "
                 "xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
                 "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" count=\"0\"/>");
  if (recRel != "rId1") return Failed();  // the definition's r:id names it
  const std::string tablePart = ed.NewPartName("/xl/pivotTables/pivotTable", ".xml");
  if (ed.AddPart(to.PartName(), std::string(kRel) + "pivotTable", tablePart, std::string(kCt) + "pivotTable+xml",
                 pivot::NewTableXml(table))
          .empty() ||
      ed.AddRelationship(tablePart, std::string(kRel) + "pivotCacheDefinition", cachePart).empty())
    return Failed();
  if (!ListCache(wbXml, cacheId, wbRel) || !ed.WritePart(wbPart, wbXml)) return Failed();
  // Laid out as a refresh lays it out.
  if (Result r = RefreshPivotsIn(ed, book, Editor::PivotRefresh::AtCell, p.sheet,
                                 CellAddr{table.location.firstRow, table.location.firstCol});
      !r)
    return r;
  group.done = true;
  return Result{};
}

}  // namespace fw::sheet::edit
