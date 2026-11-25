// Reading pivot caches and pivot tables (Pivot.h).
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/Lang.h"
#include "pkg/Package.h"
#include "pkg/XmlDom.h"
#include "sheet/model/Workbook.h"
#include "sheet/numfmt/NumFormat.h"
#include "sheet/pivot/Pivot.h"
#include "sheet/pivot/PivotXml.h"

namespace fw::sheet::pivot {

bool Item::operator==(const Item& o) const {
  if (kind != o.kind) return false;
  switch (kind) {
    case Kind::Missing: return true;
    case Kind::String: return detail::TextEqual(text, o.text);
    case Kind::Error: return text == o.text;
    default: return number == o.number;
  }
}

std::string_view FuncName(Func f) {
  static constexpr std::string_view kNames[] = {"Average", "Count", "Count", "Max", "Min", "Product",
                                                "StdDev", "StdDevp", "Sum", "Var", "Varp"};
  return kNames[static_cast<size_t>(f)];
}

namespace detail {

std::string_view Local(pugi::xml_node n) { return pkg::LocalNameOf(n); }

pugi::xml_node Child(pugi::xml_node n, std::string_view local) {
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && Local(c) == local) return c;
  return {};
}

pugi::xml_attribute Attr(pugi::xml_node n, std::string_view local) {
  for (pugi::xml_attribute a = n.first_attribute(); a; a = a.next_attribute()) {
    std::string_view name = a.name();
    if (name == local) return a;
    if (size_t colon = name.find(':'); colon != std::string_view::npos && name.substr(colon + 1) == local &&
                                       name.substr(0, colon) != "xmlns" && local == "id")
      return a;  // r:id
  }
  return {};
}

bool Bool(pugi::xml_node n, std::string_view local, bool def) {
  pugi::xml_attribute a = Attr(n, local);
  if (!a) return def;
  std::string_view v = a.value();
  return v == "1" || v == "true";
}

uint32_t U32(pugi::xml_node n, std::string_view local, uint32_t def) {
  pugi::xml_attribute a = Attr(n, local);
  if (!a) return def;
  std::string_view v = a.value();
  uint32_t out = 0;
  auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
  return ec == std::errc() && p == v.data() + v.size() ? out : def;
}

int64_t I64(pugi::xml_node n, std::string_view local, int64_t def) {
  pugi::xml_attribute a = Attr(n, local);
  if (!a) return def;
  std::string_view v = a.value();
  int64_t out = 0;
  auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
  return ec == std::errc() && p == v.data() + v.size() ? out : def;
}

std::string Str(pugi::xml_node n, std::string_view local) {
  pugi::xml_attribute a = Attr(n, local);
  return a ? std::string(a.value()) : std::string();
}

double Number(std::string_view v, bool& ok) {
  double out = 0;
  auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
  ok = ec == std::errc() && p == v.data() + v.size() && std::isfinite(out);
  return ok ? out : 0;
}

// "2024-01-31T12:00:00" as a serial (1900 date system); false when it isn't one.
bool IsoToSerial(std::string_view v, double& out) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0;
  double s = 0;
  auto num = [&](size_t at, size_t len, int& x) {
    if (at + len > v.size()) return false;
    auto [p, ec] = std::from_chars(v.data() + at, v.data() + at + len, x);
    return ec == std::errc() && p == v.data() + at + len;
  };
  if (!num(0, 4, y) || v.size() < 10 || v[4] != '-' || !num(5, 2, mo) || v[7] != '-' || !num(8, 2, d)) return false;
  if (v.size() >= 19 && v[10] == 'T' && num(11, 2, h) && num(14, 2, mi)) {
    bool ok = false;
    s = Number(v.substr(17), ok);
    if (!ok) s = 0;
  }
  // Excel writes 1899-12-30 and 1899-12-31 for times and serial 0 alike.
  if (y == 1899 && mo == 12 && (d == 30 || d == 31)) {
    out = (d == 31 ? 1 : 0) + (h * 3600 + mi * 60 + s) / 86400.0;
    return true;
  }
  double serial = 0;
  if (!DateToSerial(y, mo, d, false, serial)) return false;
  out = serial + (h * 3600 + mi * 60 + s) / 86400.0;
  return true;
}

bool TextEqual(std::string_view a, std::string_view b) { return NameEqualsNoCase(a, b); }

bool ReadItem(pugi::xml_node n, Item& out) {
  std::string_view l = Local(n);
  out = Item{};
  out.unused = Bool(n, "u", false);
  bool ok = true;
  if (l == "m") {
    out.kind = Item::Kind::Missing;
  } else if (l == "n") {
    out.kind = Item::Kind::Number;
    out.number = Number(Attr(n, "v").value(), ok);
  } else if (l == "s") {
    out.kind = Item::Kind::String;
    out.text = Attr(n, "v").value();
  } else if (l == "b") {
    out.kind = Item::Kind::Bool;
    out.number = Bool(n, "v", false) ? 1 : 0;
  } else if (l == "e") {
    out.kind = Item::Kind::Error;
    out.text = Attr(n, "v").value();
  } else if (l == "d") {
    out.kind = Item::Kind::Date;
    out.text = Attr(n, "v").value();
    ok = IsoToSerial(out.text, out.number);
  } else {
    return false;
  }
  return ok;
}

}  // namespace detail

using namespace detail;

bool ReadCache(std::string_view xml, Cache& out) {
  pkg::XmlDom dom;
  if (!dom.Parse(xml).Ok()) return false;
  pugi::xml_node root = dom.Root();
  if (Local(root) != "pivotCacheDefinition") return false;
  Cache c;
  c.part = out.part;
  c.recordsPart = out.recordsPart;
  c.refreshOnLoad = Bool(root, "refreshOnLoad", false);
  pugi::xml_node src = Child(root, "cacheSource");
  if (!src || Str(src, "type") != "worksheet") {
    c.why = ::T("an external or consolidated source");
  } else if (pugi::xml_node ws = Child(src, "worksheetSource")) {
    if (Attr(ws, "id")) c.why = ::T("a source in another workbook");
    c.sheet = Str(ws, "sheet");
    c.ref = Str(ws, "ref");
    c.name = Str(ws, "name");
    if (c.name.empty() && (c.sheet.empty() || c.ref.empty())) c.why = ::T("a source it can't find");
  } else {
    c.why = ::T("a source it can't find");
  }
  if (Child(root, "calculatedItems") || Child(root, "calculatedMembers") || Child(root, "cacheHierarchies") ||
      Child(root, "kpis") || Child(root, "tupleCache"))
    c.why = ::T("calculated items or OLAP data");
  pugi::xml_node fields = Child(root, "cacheFields");
  if (!fields) return false;
  for (pugi::xml_node f = fields.first_child(); f; f = f.next_sibling()) {
    if (f.type() != pugi::node_element || Local(f) != "cacheField") continue;
    CacheField cf;
    cf.name = Str(f, "name");
    cf.numFmtId = U32(f, "numFmtId", 0);
    if (Attr(f, "formula") || !Bool(f, "databaseField", true)) cf.kept = true, c.why = "calculated fields";
    if (Child(f, "fieldGroup")) cf.kept = true, c.why = "grouped items";
    if (pugi::xml_node si = Child(f, "sharedItems")) {
      for (pugi::xml_node it = si.first_child(); it; it = it.next_sibling()) {
        if (it.type() != pugi::node_element) continue;
        Item item;
        if (!ReadItem(it, item)) return false;
        cf.items.push_back(std::move(item));
        cf.hasItems = true;
      }
    }
    c.fields.push_back(std::move(cf));
    if (c.fields.size() > 16384) return false;
  }
  out = std::move(c);
  return true;
}

bool ReadRecords(std::string_view xml, Cache& cache) {
  pkg::XmlDom dom;
  if (!dom.Parse(xml).Ok()) return false;
  pugi::xml_node root = dom.Root();
  if (Local(root) != "pivotCacheRecords") return false;
  const size_t n = cache.fields.size();
  std::vector<uint32_t> records;
  // Fields without shared items: their values, made into items here (not written as such).
  std::vector<CacheField>& fields = cache.fields;
  for (pugi::xml_node r = root.first_child(); r; r = r.next_sibling()) {
    if (r.type() != pugi::node_element || Local(r) != "r") continue;
    size_t i = 0;
    for (pugi::xml_node v = r.first_child(); v; v = v.next_sibling()) {
      if (v.type() != pugi::node_element) continue;
      if (i >= n) return false;
      CacheField& f = fields[i];
      if (Local(v) == "x") {
        uint32_t x = U32(v, "v", 0);
        if (!f.hasItems || x >= f.items.size()) return false;
        records.push_back(x);
      } else {
        if (f.hasItems) return false;
        Item item;
        if (!ReadItem(v, item)) return false;
        size_t k = 0;
        while (k < f.items.size() && !(f.items[k] == item && f.items[k].text == item.text)) ++k;
        if (k == f.items.size()) f.items.push_back(std::move(item));
        records.push_back(static_cast<uint32_t>(k));
      }
      ++i;
    }
    if (i != n) return false;
  }
  cache.records = std::move(records);
  return true;
}

namespace {

FieldItem::Type ItemType(std::string_view t) {
  using T = FieldItem::Type;
  static constexpr std::pair<std::string_view, T> kTypes[] = {
      {"data", T::Data}, {"default", T::Default}, {"sum", T::Sum}, {"countA", T::CountA}, {"avg", T::Avg},
      {"max", T::Max}, {"min", T::Min}, {"product", T::Product}, {"count", T::Count}, {"stdDev", T::StdDev},
      {"stdDevP", T::StdDevP}, {"var", T::Var}, {"varP", T::VarP}, {"grand", T::Grand}, {"blank", T::Blank}};
  for (const auto& [name, type] : kTypes)
    if (t == name) return type;
  return T::Blank;
}

bool ReadLines(pugi::xml_node list, std::vector<Line>& out) {
  for (pugi::xml_node i = list.first_child(); i; i = i.next_sibling()) {
    if (i.type() != pugi::node_element || Local(i) != "i") continue;
    Line line;
    line.type = ItemType(Attr(i, "t") ? Attr(i, "t").value() : "data");
    line.repeat = U32(i, "r", 0);
    line.dataIndex = U32(i, "i", 0);
    for (pugi::xml_node x = i.first_child(); x; x = x.next_sibling())
      if (x.type() == pugi::node_element && Local(x) == "x") line.members.push_back(U32(x, "v", 0));
    out.push_back(std::move(line));
    if (out.size() > kMaxRows) return false;
  }
  return true;
}

Func FuncOf(std::string_view s) {
  static constexpr std::pair<std::string_view, Func> kFuncs[] = {
      {"average", Func::Average}, {"count", Func::Count}, {"countNums", Func::CountNums}, {"max", Func::Max},
      {"min", Func::Min}, {"product", Func::Product}, {"stdDev", Func::StdDev}, {"stdDevp", Func::StdDevP},
      {"sum", Func::Sum}, {"var", Func::Var}, {"varp", Func::VarP}};
  for (const auto& [name, f] : kFuncs)
    if (s == name) return f;
  return Func::Sum;
}

}  // namespace

bool ReadTable(std::string_view xml, Table& out) {
  pkg::XmlDom dom;
  if (!dom.Parse(xml).Ok()) return false;
  pugi::xml_node root = dom.Root();
  if (Local(root) != "pivotTableDefinition") return false;
  Table t;
  t.part = out.part;
  t.cachePart = out.cachePart;
  t.name = Str(root, "name");
  t.cacheId = U32(root, "cacheId", 0);
  t.dataOnRows = Bool(root, "dataOnRows", false);
  t.rowGrandTotals = Bool(root, "rowGrandTotals", true);
  t.colGrandTotals = Bool(root, "colGrandTotals", true);
  t.compact = Bool(root, "compact", true);
  t.compactData = Bool(root, "compactData", true);
  t.outline = Bool(root, "outline", false);
  t.outlineData = Bool(root, "outlineData", false);
  t.gridDropZones = Bool(root, "gridDropZones", false);
  t.showError = Bool(root, "showError", false);
  t.showMissing = Bool(root, "showMissing", true);
  t.dataCaption = Attr(root, "dataCaption") ? Str(root, "dataCaption") : "Values";
  t.grandTotalCaption = Str(root, "grandTotalCaption");
  t.rowHeaderCaption = Str(root, "rowHeaderCaption");
  t.colHeaderCaption = Str(root, "colHeaderCaption");
  t.errorCaption = Str(root, "errorCaption");
  t.missingCaption = Str(root, "missingCaption");
  t.indent = U32(root, "indent", 1);
  pugi::xml_node loc = Child(root, "location");
  if (!loc || !ParseRange(Str(loc, "ref"), t.location)) return false;
  t.firstHeaderRow = U32(loc, "firstHeaderRow", 0);
  t.firstDataRow = U32(loc, "firstDataRow", 0);
  t.firstDataCol = U32(loc, "firstDataCol", 0);
  t.rowPageCount = U32(loc, "rowPageCount", 0);
  t.colPageCount = U32(loc, "colPageCount", 0);
  if (pugi::xml_node pf = Child(root, "pivotFields"))
    for (pugi::xml_node f = pf.first_child(); f; f = f.next_sibling()) {
      if (f.type() != pugi::node_element || Local(f) != "pivotField") continue;
      Field fd;
      std::string_view axis = Attr(f, "axis").value();
      fd.axis = axis == "axisRow" ? Axis::Row : axis == "axisCol" ? Axis::Col : axis == "axisPage" ? Axis::Page
              : axis == "axisValues" ? Axis::Values : Axis::None;
      fd.dataField = Bool(f, "dataField", false);
      fd.compact = Bool(f, "compact", true);
      fd.outline = Bool(f, "outline", true);
      fd.subtotalTop = Bool(f, "subtotalTop", true);
      fd.defaultSubtotal = Bool(f, "defaultSubtotal", true);
      fd.showAll = Bool(f, "showAll", true);
      fd.multipleItems = Bool(f, "multipleItemSelectionAllowed", false);
      fd.numFmtId = U32(f, "numFmtId", kNone);
      fd.caption = Str(f, "name");
      static constexpr std::pair<std::string_view, Func> kSubs[] = {
          {"sumSubtotal", Func::Sum}, {"countASubtotal", Func::Count}, {"avgSubtotal", Func::Average},
          {"maxSubtotal", Func::Max}, {"minSubtotal", Func::Min}, {"productSubtotal", Func::Product},
          {"countSubtotal", Func::CountNums}, {"stdDevSubtotal", Func::StdDev}, {"stdDevPSubtotal", Func::StdDevP},
          {"varSubtotal", Func::Var}, {"varPSubtotal", Func::VarP}};
      for (const auto& [name, func] : kSubs)
        if (Bool(f, name, false)) fd.subtotals |= 1u << static_cast<uint32_t>(func);
      std::string_view sort = Attr(f, "sortType").value();
      fd.sort = sort == "ascending" ? Field::Sort::Ascending : sort == "descending" ? Field::Sort::Descending : Field::Sort::Manual;
      if (Bool(f, "autoShow", false) || Child(f, "autoSortScope") || Bool(f, "dataSourceSort", false))
        t.why = ::T("a top 10 filter or a sort by values");
      if (Bool(f, "hideNewItems", false) || Bool(f, "includeNewItemsInFilter", false) || Bool(f, "insertBlankRow", false) ||
          Bool(f, "insertPageBreak", false))
        if (t.why.empty() && Bool(f, "insertBlankRow", false)) t.why = ::T("blank rows after items");
      if (fd.subtotals != 0 && t.why.empty()) t.why = ::T("subtotals other than the automatic ones");
      if (pugi::xml_node items = Child(f, "items")) {
        fd.hasItems = true;
        for (pugi::xml_node it = items.first_child(); it; it = it.next_sibling()) {
          if (it.type() != pugi::node_element || Local(it) != "item") continue;
          FieldItem item;
          item.type = ItemType(Attr(it, "t") ? Attr(it, "t").value() : "data");
          item.x = U32(it, "x", 0);
          item.hidden = Bool(it, "h", false);
          item.missing = Bool(it, "m", false);
          item.caption = Str(it, "n");
          if (!Bool(it, "sd", true) && fd.axis != Axis::Page && t.why.empty()) t.why = "collapsed items";
          if (Bool(it, "f", false) || Bool(it, "c", false)) t.why = "calculated items";
          fd.items.push_back(std::move(item));
          if (fd.items.size() > kMaxRows) return false;
        }
      }
      t.fields.push_back(std::move(fd));
      if (t.fields.size() > 16384) return false;
    }
  auto fieldList = [&](std::string_view name, std::vector<int32_t>& list) {
    if (pugi::xml_node n = Child(root, name))
      for (pugi::xml_node f = n.first_child(); f; f = f.next_sibling())
        if (f.type() == pugi::node_element && Local(f) == "field") list.push_back(static_cast<int32_t>(I64(f, "x", 0)));
  };
  fieldList("rowFields", t.rowFields);
  fieldList("colFields", t.colFields);
  if (pugi::xml_node n = Child(root, "rowItems"); n && !ReadLines(n, t.rowItems)) return false;
  if (pugi::xml_node n = Child(root, "colItems"); n && !ReadLines(n, t.colItems)) return false;
  if (pugi::xml_node n = Child(root, "pageFields"))
    for (pugi::xml_node f = n.first_child(); f; f = f.next_sibling()) {
      if (f.type() != pugi::node_element || Local(f) != "pageField") continue;
      PageField p;
      p.field = U32(f, "fld", 0);
      p.item = Attr(f, "item") ? I64(f, "item", -1) : -1;
      if (I64(f, "hier", -1) != -1) t.why = "OLAP data";
      t.pageFields.push_back(p);
    }
  if (pugi::xml_node n = Child(root, "dataFields"))
    for (pugi::xml_node f = n.first_child(); f; f = f.next_sibling()) {
      if (f.type() != pugi::node_element || Local(f) != "dataField") continue;
      DataField d;
      d.name = Str(f, "name");
      d.field = U32(f, "fld", 0);
      d.func = FuncOf(Attr(f, "subtotal") ? Attr(f, "subtotal").value() : "sum");
      d.numFmtId = U32(f, "numFmtId", kNone);
      if (Attr(f, "showDataAs") && Str(f, "showDataAs") != "normal") t.why = ::T("values shown as a percentage or difference");
      t.dataFields.push_back(std::move(d));
    }
  if (Child(root, "filters")) t.why = ::T("label or value filters");
  if (Child(root, "calculatedItems") || Child(root, "calculatedMembers")) t.why = "calculated items";
  if (Child(root, "pivotHierarchies") || Child(root, "rowHierarchiesUsage")) t.why = "OLAP data";
  if (pugi::xml_node style = Child(root, "pivotTableStyleInfo")) {
    t.style = Str(style, "name");
    t.showRowHeaders = Bool(style, "showRowHeaders", true);
    t.showColHeaders = Bool(style, "showColHeaders", true);
    t.showRowStripes = Bool(style, "showRowStripes", false);
    t.showColStripes = Bool(style, "showColStripes", false);
    t.showLastColumn = Bool(style, "showLastColumn", true);
  }
  // x14's hideValuesRow: in the extension list.
  if (pugi::xml_node ext = Child(root, "extLst"))
    for (pugi::xml_node e = ext.first_child(); e; e = e.next_sibling())
      for (pugi::xml_node d = e.first_child(); d; d = d.next_sibling())
        if (d.type() == pugi::node_element && Local(d) == "pivotTableDefinition") t.hideValuesRow = Bool(d, "hideValuesRow", false);
  // Indexes that don't fit make it kept: never refreshed, never laid out.
  const size_t nf = t.fields.size();
  auto badField = [&](int32_t x) { return x != -2 && (x < 0 || static_cast<size_t>(x) >= nf); };
  for (int32_t x : t.rowFields)
    if (badField(x)) return false;
  for (int32_t x : t.colFields)
    if (badField(x)) return false;
  for (const PageField& p : t.pageFields)
    if (p.field >= nf) return false;
  for (const DataField& d : t.dataFields)
    if (d.field >= nf) return false;
  out = std::move(t);
  return true;
}

std::vector<Place> List(const Workbook& book) {
  std::vector<Place> out;
  const pkg::Package* package = book.GetPackage();
  if (!package) return out;
  for (size_t i = 0; i < book.SheetCount(); ++i) {
    const std::string& part = book.GetSheet(i).PartName();
    if (part.empty()) continue;
    const pkg::RelationshipSet* rels = package->RelationshipsOf(part);
    if (!rels) continue;
    for (const pkg::Relationship& r : rels->items) {
      if (r.external || !r.type.ends_with("/pivotTable") || !package->FindPart(r.targetPart)) continue;
      Place p;
      p.sheet = i;
      p.table = r.targetPart;
      if (const pkg::RelationshipSet* own = package->RelationshipsOf(r.targetPart))
        for (const pkg::Relationship& c : own->items)
          if (!c.external && c.type.ends_with("/pivotCacheDefinition") && package->FindPart(c.targetPart)) p.cache = c.targetPart;
      out.push_back(std::move(p));
    }
  }
  return out;
}

}  // namespace fw::sheet::pivot
