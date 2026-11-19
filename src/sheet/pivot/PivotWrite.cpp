// Writing pivot caches and pivot tables as Excel writes them (Pivot.h): the elements a refresh
// changes are written anew, everything else in the part is kept as it was.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "pkg/XmlDom.h"
#include "sheet/numfmt/NumFormat.h"
#include "sheet/pivot/Pivot.h"
#include "sheet/pivot/PivotXml.h"

namespace fw::sheet::pivot {

using namespace detail;

namespace {

std::string Num(double v) {
  char buf[40];
  for (int p = 15; p <= 17; ++p) {
    std::snprintf(buf, sizeof buf, "%.*g", p, v);
    if (std::strtod(buf, nullptr) == v) break;
  }
  return buf;
}

std::string Iso(double serial) {
  int y = 1899, m = 12, d = 30;
  double day = std::floor(serial);
  if (day >= 1 && !SerialToDate(day, false, y, m, d)) y = 1899, m = 12, d = 30;
  if (day < 1) d = day >= 1 ? d : (day == 0 ? 30 : 31);
  long secs = std::lround((serial - day) * 86400.0);
  if (secs >= 86400) secs = 86399;
  char buf[40];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02ld:%02ld:%02ld", y, m, d, secs / 3600, secs / 60 % 60, secs % 60);
  return buf;
}

void Escape(std::string& out, std::string_view s) {
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\n': out += "&#10;"; break;
      case '\r': out += "&#13;"; break;
      case '\t': out += "&#9;"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[16];
          std::snprintf(buf, sizeof buf, "_x%04X_", static_cast<unsigned>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
}

// An item as an element: <s v>, <n v>, <m/>, <b v>, <e v>, <d v>, with u="1" when unused.
void ItemXml(std::string& out, const Item& v, bool shared) {
  const char* tag = "m";
  std::string value;
  switch (v.kind) {
    case Item::Kind::Number: tag = "n", value = Num(v.number); break;
    case Item::Kind::String: tag = "s", value = v.text; break;
    case Item::Kind::Bool: tag = "b", value = v.number != 0 ? "1" : "0"; break;
    case Item::Kind::Error: tag = "e", value = v.text; break;
    case Item::Kind::Date: tag = "d", value = Iso(v.number); break;
    default: break;
  }
  out += '<';
  out += tag;
  if (shared && v.unused) out += " u=\"1\"";
  if (v.kind != Item::Kind::Missing) {
    out += " v=\"";
    Escape(out, value);
    out += '"';
  }
  out += "/>";
}

void SetAttr(pugi::xml_node n, const char* name, const std::string& value) {
  pugi::xml_attribute a = n.attribute(name);
  if (!a) a = n.append_attribute(name);
  a.set_value(value.c_str());
}

// The sharedItems element of a cache field: what its values hold, and its items when it has them.
void SharedItems(pugi::xml_node si, const CacheField& f) {
  while (si.first_attribute()) si.remove_attribute(si.first_attribute());
  while (si.first_child()) si.remove_child(si.first_child());
  bool str = false, num = false, date = false, boolean = false, err = false, blank = false, integer = true, longText = false;
  double minV = 0, maxV = 0, minD = 0, maxD = 0;
  for (const Item& v : f.items) {
    switch (v.kind) {
      case Item::Kind::Missing: blank = true; break;
      case Item::Kind::String: str = true, longText |= v.text.size() > 255; break;
      case Item::Kind::Bool: boolean = true; break;
      case Item::Kind::Error: err = true; break;
      case Item::Kind::Number:
        if (!num) minV = maxV = v.number;
        num = true;
        minV = std::min(minV, v.number);
        maxV = std::max(maxV, v.number);
        integer &= v.number == std::floor(v.number) && std::fabs(v.number) < 1e15;
        break;
      case Item::Kind::Date:
        if (!date) minD = maxD = v.number;
        date = true;
        minD = std::min(minD, v.number);
        maxD = std::max(maxD, v.number);
        break;
    }
  }
  const int kinds = str + num + date + boolean + err;
  if (!(str || blank || boolean || err)) si.append_attribute("containsSemiMixedTypes").set_value("0");
  if (date && !(str || num || boolean || err)) si.append_attribute("containsNonDate").set_value("0");
  if (date) si.append_attribute("containsDate").set_value("1");
  if (!str) si.append_attribute("containsString").set_value("0");
  if (blank) si.append_attribute("containsBlank").set_value("1");
  if (kinds > 1) si.append_attribute("containsMixedTypes").set_value("1");
  if (num) si.append_attribute("containsNumber").set_value("1");
  if (num && integer) si.append_attribute("containsInteger").set_value("1");
  if (num) {
    si.append_attribute("minValue").set_value(Num(minV).c_str());
    si.append_attribute("maxValue").set_value(Num(maxV).c_str());
  }
  if (date) {
    si.append_attribute("minDate").set_value(Iso(minD).c_str());
    si.append_attribute("maxDate").set_value(Iso(std::ceil(maxD) == maxD ? maxD + 1 : std::ceil(maxD)).c_str());
  }
  if (f.hasItems) {
    si.append_attribute("count").set_value(std::to_string(f.items.size()).c_str());
    std::string xml = "<r>";
    for (const Item& v : f.items) ItemXml(xml, v, true);
    xml += "</r>";
    pugi::xml_document frag;
    if (frag.load_buffer(xml.data(), xml.size()))
      for (pugi::xml_node c = frag.first_child().first_child(); c; c = c.next_sibling()) si.append_copy(c);
  }
  if (longText) si.append_attribute("longText").set_value("1");
}

}  // namespace

std::string WriteCache(std::string_view oldXml, const Cache& cache, double refreshedDate) {
  pkg::XmlDom dom;
  if (!dom.Parse(oldXml).Ok()) return {};
  pugi::xml_node root = dom.Root();
  SetAttr(root, "refreshedDate", Num(refreshedDate));
  SetAttr(root, "recordCount", std::to_string(cache.RecordCount()));
  pugi::xml_node fields = Child(root, "cacheFields");
  size_t i = 0;
  for (pugi::xml_node f = fields.first_child(); f && i < cache.fields.size(); f = f.next_sibling()) {
    if (f.type() != pugi::node_element || Local(f) != "cacheField") continue;
    pugi::xml_node si = Child(f, "sharedItems");
    if (!si) si = f.prepend_child(root.name() == std::string_view("pivotCacheDefinition") ? "sharedItems" : "sharedItems");
    SharedItems(si, cache.fields[i++]);
  }
  return dom.Serialize();
}

std::string WriteRecords(std::string_view oldXml, const Cache& cache) {
  std::string head = "<pivotCacheRecords xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
                     "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"";
  pkg::XmlDom dom;
  if (!oldXml.empty() && dom.Parse(oldXml).Ok()) {
    head = "<";
    head += dom.Root().name();
    for (pugi::xml_attribute a = dom.Root().first_attribute(); a; a = a.next_attribute()) {
      if (std::string_view(a.name()) == "count") continue;
      head += ' ';
      head += a.name();
      head += "=\"";
      Escape(head, a.value());
      head += '"';
    }
  }
  const size_t n = cache.fields.size(), records = cache.RecordCount();
  std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n" + head + " count=\"" +
                    std::to_string(records) + "\">";
  out.reserve(out.size() + records * n * 12);
  for (size_t r = 0; r < records; ++r) {
    out += "<r>";
    for (size_t f = 0; f < n; ++f) {
      const CacheField& cf = cache.fields[f];
      const uint32_t k = cache.records[r * n + f];
      if (cf.hasItems) {
        out += "<x";
        if (k) out += " v=\"" + std::to_string(k) + "\"";
        out += "/>";
      } else if (k < cf.items.size()) {
        ItemXml(out, cf.items[k], false);
      } else {
        out += "<m/>";
      }
    }
    out += "</r>";
  }
  std::string_view tag = dom.Root() ? std::string_view(dom.Root().name()) : std::string_view("pivotCacheRecords");
  out += "</";
  out += tag;
  out += ">";
  return out;
}

namespace {

const char* TypeName(FieldItem::Type t) {
  using T = FieldItem::Type;
  switch (t) {
    case T::Default: return "default";
    case T::Sum: return "sum";
    case T::CountA: return "countA";
    case T::Avg: return "avg";
    case T::Max: return "max";
    case T::Min: return "min";
    case T::Product: return "product";
    case T::Count: return "count";
    case T::StdDev: return "stdDev";
    case T::StdDevP: return "stdDevP";
    case T::Var: return "var";
    case T::VarP: return "varP";
    case T::Grand: return "grand";
    case T::Blank: return "blank";
    default: return nullptr;
  }
}

// `parent`'s child `name` emptied, or made after the first of `after` it has (else first).
pugi::xml_node Fresh(pugi::xml_node parent, const char* name, std::initializer_list<std::string_view> after) {
  if (pugi::xml_node n = Child(parent, name)) {
    while (n.first_child()) n.remove_child(n.first_child());
    while (n.first_attribute()) n.remove_attribute(n.first_attribute());
    return n;
  }
  pugi::xml_node prev;
  for (std::string_view a : after)
    if (pugi::xml_node p = Child(parent, a)) prev = p;
  std::string prefixed = name;
  if (std::string_view root = parent.name(); root.find(':') != std::string_view::npos)
    prefixed = std::string(root.substr(0, root.find(':') + 1)) + name;
  return prev ? parent.insert_child_after(prefixed.c_str(), prev) : parent.prepend_child(prefixed.c_str());
}

void Lines(pugi::xml_node list, const std::vector<Line>& lines) {
  list.append_attribute("count").set_value(std::to_string(lines.size()).c_str());
  std::string prefix;
  if (std::string_view n = list.name(); n.find(':') != std::string_view::npos) prefix = std::string(n.substr(0, n.find(':') + 1));
  for (const Line& l : lines) {
    pugi::xml_node i = list.append_child((prefix + "i").c_str());
    if (const char* t = TypeName(l.type)) i.append_attribute("t").set_value(t);
    if (l.repeat) i.append_attribute("r").set_value(l.repeat);
    if (l.dataIndex) i.append_attribute("i").set_value(l.dataIndex);
    for (uint32_t m : l.members) {
      pugi::xml_node x = i.append_child((prefix + "x").c_str());
      if (m) x.append_attribute("v").set_value(m);
    }
  }
}

}  // namespace

std::string WriteTable(std::string_view oldXml, const Table& t) {
  pkg::XmlDom dom;
  if (!dom.Parse(oldXml).Ok()) return {};
  pugi::xml_node root = dom.Root();
  std::string prefix;
  if (std::string_view n = root.name(); n.find(':') != std::string_view::npos) prefix = std::string(n.substr(0, n.find(':') + 1));
  if (pugi::xml_node loc = Child(root, "location")) {
    SetAttr(loc, "ref", RangeName(t.location));
    SetAttr(loc, "firstHeaderRow", std::to_string(t.firstHeaderRow));
    SetAttr(loc, "firstDataRow", std::to_string(t.firstDataRow));
    SetAttr(loc, "firstDataCol", std::to_string(t.firstDataCol));
    if (t.rowPageCount) SetAttr(loc, "rowPageCount", std::to_string(t.rowPageCount)), SetAttr(loc, "colPageCount", std::to_string(t.colPageCount));
    else loc.remove_attribute("rowPageCount"), loc.remove_attribute("colPageCount");
  }
  if (pugi::xml_node pf = Child(root, "pivotFields")) {
    size_t i = 0;
    for (pugi::xml_node f = pf.first_child(); f && i < t.fields.size(); f = f.next_sibling()) {
      if (f.type() != pugi::node_element || Local(f) != "pivotField") continue;
      const Field& fd = t.fields[i++];
      if (!fd.hasItems) continue;
      pugi::xml_node items = Child(f, "items");
      if (items) {
        while (items.first_child()) items.remove_child(items.first_child());
        items.remove_attribute("count");
      } else {
        items = f.prepend_child((prefix + "items").c_str());
      }
      items.append_attribute("count").set_value(std::to_string(fd.items.size()).c_str());
      for (const FieldItem& it : fd.items) {
        pugi::xml_node e = items.append_child((prefix + "item").c_str());
        if (!it.caption.empty()) e.append_attribute("n").set_value(it.caption.c_str());
        if (const char* type = TypeName(it.type)) e.append_attribute("t").set_value(type);
        if (it.hidden) e.append_attribute("h").set_value("1");
        if (it.missing) e.append_attribute("m").set_value("1");
        if (it.type == FieldItem::Type::Data) e.append_attribute("x").set_value(it.x);
      }
    }
  }
  if (pugi::xml_node pages = Child(root, "pageFields")) {
    size_t i = 0;
    for (pugi::xml_node p = pages.first_child(); p && i < t.pageFields.size(); p = p.next_sibling()) {
      if (p.type() != pugi::node_element || Local(p) != "pageField") continue;
      const PageField& pf = t.pageFields[i++];
      if (pf.item >= 0) {
        pugi::xml_attribute a = p.attribute("item");
        if (!a) a = p.insert_attribute_after("item", p.attribute("fld"));
        a.set_value(std::to_string(pf.item).c_str());
      } else {
        p.remove_attribute("item");
      }
    }
  }
  Lines(Fresh(root, "rowItems", {"location", "pivotFields", "rowFields"}), t.rowItems);
  Lines(Fresh(root, "colItems", {"location", "pivotFields", "rowFields", "rowItems", "colFields"}), t.colItems);
  return dom.Serialize();
}

}  // namespace fw::sheet::pivot

namespace fw::sheet::pivot {

namespace {

constexpr std::string_view kMainNs = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
constexpr std::string_view kRelNs = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";

std::string Attr(std::string_view name, std::string_view value) {
  std::string out = " ";
  out += name;
  out += "=\"";
  Escape(out, value);
  out += '"';
  return out;
}

}  // namespace

std::string NewCacheXml(const Cache& cache) {
  std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<pivotCacheDefinition";
  x += Attr("xmlns", kMainNs) + Attr("xmlns:r", kRelNs) + Attr("r:id", "rId1");
  x += " refreshedDate=\"0\" createdVersion=\"6\" refreshedVersion=\"6\" minRefreshableVersion=\"3\" recordCount=\"0\">";
  x += "<cacheSource type=\"worksheet\"><worksheetSource";
  if (!cache.name.empty()) x += Attr("name", cache.name);
  else x += Attr("ref", cache.ref) + Attr("sheet", cache.sheet);
  x += "/></cacheSource><cacheFields count=\"" + std::to_string(cache.fields.size()) + "\">";
  for (const CacheField& f : cache.fields)
    x += "<cacheField" + Attr("name", f.name) + " numFmtId=\"" + std::to_string(f.numFmtId) + "\"><sharedItems/></cacheField>";
  x += "</cacheFields></pivotCacheDefinition>";
  return x;
}

Table NewTable(const Cache& cache, const NewPivot& spec, std::string name, uint32_t cacheId, CellAddr at) {
  Table t;
  t.name = std::move(name);
  t.cacheId = cacheId;
  t.compact = true, t.compactData = true, t.outline = true, t.outlineData = true;
  t.style = "PivotStyleLight16";
  t.fields.resize(cache.fields.size());
  for (Field& f : t.fields) f.showAll = false;
  for (uint32_t c : spec.rows)
    if (c < t.fields.size()) t.fields[c].axis = Axis::Row, t.rowFields.push_back(static_cast<int32_t>(c));
  for (uint32_t c : spec.cols)
    if (c < t.fields.size()) t.fields[c].axis = Axis::Col, t.colFields.push_back(static_cast<int32_t>(c));
  for (uint32_t c : spec.pages)
    if (c < t.fields.size()) t.fields[c].axis = Axis::Page, t.pageFields.push_back(PageField{c, -1});
  std::vector<std::string> names;
  for (const NewPivot::Value& v : spec.values) {
    if (v.field >= t.fields.size()) continue;
    t.fields[v.field].dataField = true;
    DataField d;
    d.field = v.field;
    d.func = v.func;
    const std::string base = std::string(FuncName(v.func)) + " of " + cache.fields[v.field].name;
    d.name = base;
    // Unique, and not a field's own name: Excel adds 2, 3...
    auto taken = [&](const std::string& s) {
      for (const std::string& n : names)
        if (TextEqual(n, s)) return true;
      for (const CacheField& f : cache.fields)
        if (TextEqual(f.name, s)) return true;
      return false;
    };
    for (int n = 2; taken(d.name); ++n) d.name = base + std::to_string(n);
    names.push_back(d.name);
    t.dataFields.push_back(std::move(d));
  }
  if (t.dataFields.size() > 1) t.colFields.push_back(-2);
  const uint32_t pages = static_cast<uint32_t>(t.pageFields.size());
  const uint32_t top = at.row + (pages ? pages + 1 : 0);
  t.location = {top, at.col, top, at.col};
  return t;
}

std::string NewTableXml(const Table& t) {
  std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<pivotTableDefinition";
  x += Attr("xmlns", kMainNs) + Attr("name", t.name) + " cacheId=\"" + std::to_string(t.cacheId) + "\"";
  x += " applyNumberFormats=\"0\" applyBorderFormats=\"0\" applyFontFormats=\"0\" applyPatternFormats=\"0\""
       " applyAlignmentFormats=\"0\" applyWidthHeightFormats=\"1\" dataCaption=\"Values\" updatedVersion=\"6\""
       " minRefreshableVersion=\"3\" useAutoFormatting=\"1\" itemPrintTitles=\"1\" createdVersion=\"6\" indent=\"0\""
       " outline=\"1\" outlineData=\"1\" multipleFieldFilters=\"0\">";
  x += "<location" + Attr("ref", RangeName(t.location)) + " firstHeaderRow=\"1\" firstDataRow=\"1\" firstDataCol=\"1\"/>";
  x += "<pivotFields count=\"" + std::to_string(t.fields.size()) + "\">";
  for (const Field& f : t.fields) {
    x += "<pivotField";
    if (f.axis == Axis::Row) x += " axis=\"axisRow\"";
    else if (f.axis == Axis::Col) x += " axis=\"axisCol\"";
    else if (f.axis == Axis::Page) x += " axis=\"axisPage\"";
    if (f.dataField) x += " dataField=\"1\"";
    x += " showAll=\"0\"/>";
  }
  x += "</pivotFields>";
  auto fields = [&](const char* tag, const std::vector<int32_t>& list) {
    if (list.empty()) return;
    x += std::string("<") + tag + " count=\"" + std::to_string(list.size()) + "\">";
    for (int32_t f : list) x += "<field x=\"" + std::to_string(f) + "\"/>";
    x += std::string("</") + tag + ">";
  };
  fields("rowFields", t.rowFields);
  x += "<rowItems count=\"1\"><i/></rowItems>";
  fields("colFields", t.colFields);
  x += "<colItems count=\"1\"><i/></colItems>";
  if (!t.pageFields.empty()) {
    x += "<pageFields count=\"" + std::to_string(t.pageFields.size()) + "\">";
    for (const PageField& p : t.pageFields) x += "<pageField fld=\"" + std::to_string(p.field) + "\" hier=\"-1\"/>";
    x += "</pageFields>";
  }
  if (!t.dataFields.empty()) {
    static constexpr const char* kSub[] = {"average", "count", "countNums", "max", "min", "product",
                                           "stdDev", "stdDevp", "sum", "var", "varp"};
    x += "<dataFields count=\"" + std::to_string(t.dataFields.size()) + "\">";
    for (const DataField& d : t.dataFields) {
      x += "<dataField" + Attr("name", d.name) + " fld=\"" + std::to_string(d.field) + "\"";
      if (d.func != Func::Sum) x += std::string(" subtotal=\"") + kSub[static_cast<size_t>(d.func)] + "\"";
      x += " baseField=\"0\" baseItem=\"0\"/>";
    }
    x += "</dataFields>";
  }
  x += "<pivotTableStyleInfo" + Attr("name", t.style) +
       " showRowHeaders=\"1\" showColHeaders=\"1\" showRowStripes=\"0\" showColStripes=\"0\" showLastColumn=\"1\"/>";
  x += "</pivotTableDefinition>";
  return x;
}

}  // namespace fw::sheet::pivot
