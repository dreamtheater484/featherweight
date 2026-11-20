#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sheet/model/TableStyles.h"
#include "sheet/model/Types.h"

// Pivot tables (plan §7.2, decision 18; S4 s4-pivot). A workbook's pivot tables live in package
// parts the model doesn't own: each sheet's pivotTableDefinition parts, and the pivot caches
// (pivotCacheDefinition with its pivotCacheRecords) they point to. This reads them into the small
// model below, refreshes a cache from its source cells, lays out a pivot table as Excel does
// (compact, outline and tabular forms; row, column and page fields; sum, count, average, min, max
// and the rest of Excel's simple functions; subtotals and grand totals), and writes the parts back
// as Excel writes them, keeping every element and attribute it doesn't edit.
//
// Features beyond those (calculated fields and items, grouping, OLAP and external sources, values
// shown as a percentage of something, top-10 and value filters, sorting by values) make a pivot
// "kept": shown and saved as it is, never refreshed (§7.2). Supported() says why.
namespace fw::sheet {
class Workbook;
}

namespace fw::sheet::pivot {

// A value in a pivot cache: a shared item, or a record's value of a field without items.
struct Item {
  enum class Kind : uint8_t { Missing, Number, String, Bool, Error, Date };
  Kind kind = Kind::Missing;
  double number = 0;   // Number; Date as a serial; Bool 0 or 1
  std::string text;    // String; Error's text ("#N/A")
  bool unused = false; // u="1": kept from an earlier refresh, no record has it now
  bool operator==(const Item& o) const;
};

struct CacheField {
  std::string name;
  uint32_t numFmtId = 0;
  bool hasItems = false;     // sharedItems lists items, which the records index (<x v>)
  std::vector<Item> items;   // the shared items; for a field without them, every value the records hold
  bool kept = false;         // a formula, a group, a field not from the source: not refreshed
};

struct Cache {
  std::string part, recordsPart;  // "/xl/pivotCache/pivotCacheDefinition1.xml" and its records
  // The source (worksheetSource): `name` (a table or defined name), else `sheet` and `ref`.
  std::string sheet, ref, name;
  bool refreshOnLoad = false;
  std::string why;  // empty when it can be refreshed: else what keeps it as it is
  std::vector<CacheField> fields;
  // The records: recordCount rows of fields.size() indexes into each field's items.
  std::vector<uint32_t> records;
  size_t RecordCount() const { return fields.empty() ? 0 : records.size() / fields.size(); }
  uint32_t At(size_t record, size_t field) const { return records[record * fields.size() + field]; }
};

enum class Axis : uint8_t { None, Row, Col, Page, Values };
// Excel's data field functions (ST_DataConsolidateFunction), in its order.
enum class Func : uint8_t { Average, Count, CountNums, Max, Min, Product, StdDev, StdDevP, Sum, Var, VarP };
std::string_view FuncName(Func f);  // "Sum", "Count", "Average"... as captions say it

// An entry of a pivot field's items: one of the cache field's shared items (Data), or a subtotal.
struct FieldItem {
  enum class Type : uint8_t { Data, Default, Sum, CountA, Avg, Max, Min, Product, Count, StdDev, StdDevP, Var, VarP, Grand, Blank };
  Type type = Type::Data;
  uint32_t x = 0;         // the shared item (Data)
  bool hidden = false;    // h="1": filtered out
  bool missing = false;   // m="1": no record has it now
  std::string caption;    // n: shown instead of the item's own text
};

struct Field {
  Axis axis = Axis::None;
  bool dataField = false;
  bool hasItems = false;
  std::vector<FieldItem> items;
  bool compact = true, outline = true, subtotalTop = true, defaultSubtotal = true, showAll = true;
  uint32_t subtotals = 0;  // the Func bits of sumSubtotal, countSubtotal...: other than the default
  enum class Sort : uint8_t { Manual, Ascending, Descending } sort = Sort::Manual;
  uint32_t numFmtId = kNone;
  std::string caption;     // name: its header instead of the cache field's name
  bool multipleItems = false;  // multipleItemSelectionAllowed (a page field)
};

struct DataField {
  std::string name;
  uint32_t field = 0;
  Func func = Func::Sum;
  uint32_t numFmtId = kNone;
};

struct PageField {
  uint32_t field = 0;
  int64_t item = -1;  // the field's items index shown; -1: all
};

// A line of the row or column axis (rowItems, colItems: <i>), as Excel writes and lays it out.
struct Line {
  FieldItem::Type type = FieldItem::Type::Data;  // Data, Default (a subtotal), Grand, Blank
  uint32_t repeat = 0;          // r: the leading members this line shares with the one before
  uint32_t dataIndex = 0;       // i: the data field (the -2 field's member)
  std::vector<uint32_t> members;  // x: per axis field from `repeat` on, its items index
};

struct Table {
  std::string part, cachePart, name;
  uint32_t cacheId = 0;
  Range location;
  uint32_t firstHeaderRow = 0, firstDataRow = 0, firstDataCol = 0, rowPageCount = 0, colPageCount = 0;
  std::vector<Field> fields;
  std::vector<int32_t> rowFields, colFields;  // field indexes; -2: the data fields
  std::vector<PageField> pageFields;
  std::vector<DataField> dataFields;
  std::vector<Line> rowItems, colItems;
  bool dataOnRows = false, rowGrandTotals = true, colGrandTotals = true;
  bool compact = true, compactData = true, outline = false, outlineData = false, gridDropZones = false;
  bool hideValuesRow = false;  // x14: no "Values" caption row when only the data fields are columns
  bool showError = false, showMissing = true;
  std::string dataCaption = "Values", grandTotalCaption, rowHeaderCaption, colHeaderCaption, errorCaption, missingCaption;
  uint32_t indent = 1;
  std::string style;  // pivotTableStyleInfo name
  bool showRowHeaders = true, showColHeaders = true, showRowStripes = false, showColStripes = false, showLastColumn = true;
  std::string why;    // empty when it can be refreshed
};

// ---- Reading (each false when the XML isn't one; a feature beyond the simple ones sets `why`)
bool ReadCache(std::string_view definitionXml, Cache& out);
bool ReadRecords(std::string_view xml, Cache& cache);
bool ReadTable(std::string_view xml, Table& out);

// The workbook's pivot tables, sheet by sheet in tab order: each one's part, its sheet, its cache's
// part. Loads nothing but the parts' relationships.
struct Place {
  size_t sheet = 0;
  std::string table, cache;
};
std::vector<Place> List(const Workbook& book);

// ---- Refreshing
// The cache read again from its source cells (the sheet's or the table's): items in the order
// they first appear, the ones no longer there kept as unused, the records anew. `axisFields` (from
// every pivot on it) get shared items. `remap` gets, per field, each old shared item's new index
// (for RemapItems). Loads the source's sheet. False with a message as Excel says it when the source
// isn't usable (gone, no header, a column gone).
bool RefreshCache(Workbook& book, Cache& cache, const std::vector<bool>& axisFields,
                  std::vector<std::vector<uint32_t>>& remap, std::u16string& why);
// A pivot field's items after its cache was refreshed: old shared indexes in `remap` (kNone: gone),
// gone items marked missing, new ones appended (or the list sorted, for a sorted field).
void RemapItems(Table& table, const Cache& cache, const std::vector<std::vector<uint32_t>>& remap);

// What a pivot table shows: its cells, as Excel writes them after a refresh.
struct OutCell {
  CellAddr at;
  Item value;                  // Missing: empty
  uint32_t numFmtId = kNone;   // the format the cell gets (kNone: General)
  uint8_t indent = 0;          // compact form's indent
};
struct Layout {
  Range location;   // the table without its page fields
  Range area;       // with them (AreaOf)
  std::vector<OutCell> cells;
};
// A table's cells with its `pages` page fields: their lines above `location` with a blank line
// between, and their values' column, right of a one-column table too.
Range AreaOf(const Range& location, uint32_t pages);
// Lays `table` out from its cache: its location (the top-left cell stays), rowItems and colItems,
// the page fields above it, and every cell. False with `why` when it can't.
bool Lay(const Cache& cache, Table& table, Layout& out, std::u16string& why);

// ---- Writing: the parts as Excel writes them, keeping what isn't edited.
std::string WriteCache(std::string_view oldXml, const Cache& cache, double refreshedDate);
std::string WriteRecords(std::string_view oldXml, const Cache& cache);
std::string WriteTable(std::string_view oldXml, const Table& table);

// ---- New pivot tables (Insert > PivotTable)
struct NewPivot {
  std::vector<uint32_t> rows, cols, pages;  // source columns, in order
  struct Value {
    uint32_t field = 0;
    Func func = Func::Sum;
  };
  std::vector<Value> values;
};
// A cache over a source, and a table on it named `name` at `at` with cacheId `cacheId`: their XML
// as Excel writes new ones (laid out by Lay after RefreshCache).
std::string NewCacheXml(const Cache& cache);
std::string NewTableXml(const Table& table);
// The table model for `spec` over `cache` (its fields read already).
Table NewTable(const Cache& cache, const NewPivot& spec, std::string name, uint32_t cacheId, CellAddr at);

// ---- The style in the grid (PivotStyles.cpp): a pivot's regions as its part lays it out, and the
// format its PivotTable style (Excel's built-in ones) gives a cell, under the cell's own format as a
// table's style is.
struct Look {
  enum : uint8_t { kData = 0, kHeading = 0x10, kSubtotal = 0x20, kGrand = 0x40 };  // | level 0-2
  Range location, area;  // area: with the page fields
  uint32_t firstHeaderRow = 0, firstDataRow = 0, firstDataCol = 0, pages = 0, colLevels = 0;
  std::string style;
  bool rowHeaders = true, colHeaders = true, rowStripes = false, colStripes = false;
  std::vector<uint8_t> rowKind, colKind;  // per row and column line
  std::vector<uint32_t> levelCol;  // per row field: its labels' column (compact fields share one)
  std::vector<bool> levelTabular;  // per row field: tabular (its label on its first child's line)
};
bool ReadLook(std::string_view tableXml, Look& out);
std::vector<Look> SheetLooks(const Workbook& book, size_t sheet);
TableCellFormat LookFormat(const Look& look, uint32_t row, uint32_t col);
TableCellFormat LookFormatAt(const std::vector<Look>& looks, uint32_t row, uint32_t col);

}  // namespace fw::sheet::pivot
