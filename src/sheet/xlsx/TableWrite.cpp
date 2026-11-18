// Tables written (S3, s3-data): a new table's part as Excel writes it, and an existing part with
// what an edit changed (name, size, header and total rows, filter buttons, style options, column
// names and totals), everything else kept.

#include <algorithm>
#include <string>

#include "pkg/XmlWriter.h"
#include "sheet/model/Sheet.h"
#include "sheet/xlsx/Internal.h"

namespace fw::sheet::xlsx {

namespace {

constexpr std::string_view kDeclaration = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";

std::string A(std::string_view name, std::string_view value) {
  std::string out = " " + std::string(name) + "=\"";
  pkg::AppendEscapedAttribute(out, value);
  return out + "\"";
}

// A header's name as the part holds it: line breaks as _x000a_ (as Excel writes them).
std::string ColumnName(std::string_view name) {
  std::string out;
  for (char c : name) {
    if (c == '\n') out += "_x000a_";
    else if (c == '\r') out += "_x000d_";
    else if (static_cast<unsigned char>(c) >= 0x20 || c == '\t') out += c;
  }
  return out;
}

// The filter's range: the table without its total row.
Range FilterRange(const Table& t) {
  Range r = t.ref;
  if (t.totalsRows && r.lastRow > r.firstRow) --r.lastRow;
  return r;
}

void Set(pugi::xml_node n, const char* name, const std::string& value, bool keep = true) {
  pugi::xml_attribute a = n.attribute(name);
  if (!keep) {
    if (a) n.remove_attribute(a);
    return;
  }
  if (!a) a = n.append_attribute(name);
  a.set_value(value.c_str());
}

pugi::xml_node ChildOf(pugi::xml_node n, std::string_view local) {
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && Local(c) == local) return c;
  return {};
}

std::string Prefix(pugi::xml_node n) {
  const std::string_view name = n.name();
  const size_t colon = name.find(':');
  return colon == std::string_view::npos ? std::string() : std::string(name.substr(0, colon + 1));
}

}  // namespace

std::string TableXml(const Table& t) {
  std::string out(kDeclaration);
  out += "<table xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"" + A("id", std::to_string(t.id)) +
         A("name", t.name) + A("displayName", t.displayName.empty() ? t.name : t.displayName) + A("ref", RangeName(t.ref));
  if (!t.headerRows) out += A("headerRowCount", "0");
  if (t.totalsRows) out += A("totalsRowCount", "1");
  else if (!t.totalsRowShown) out += A("totalsRowShown", "0");
  out += ">";
  if (t.autoFilter && t.headerRows) out += "<autoFilter" + A("ref", RangeName(FilterRange(t))) + "/>";
  out += "<tableColumns" + A("count", std::to_string(t.columns.size())) + ">";
  for (const TableColumn& c : t.columns) {
    out += "<tableColumn" + A("id", std::to_string(c.id)) + A("name", ColumnName(c.name));
    if (!c.totalsLabel.empty()) out += A("totalsRowLabel", c.totalsLabel);
    if (!c.totalsFunction.empty()) out += A("totalsRowFunction", c.totalsFunction);
    if (c.calculatedFormula.empty() && c.totalsFormula.empty()) {
      out += "/>";
      continue;
    }
    out += ">";
    if (!c.calculatedFormula.empty()) {
      out += "<calculatedColumnFormula>";
      pkg::AppendEscapedText(out, c.calculatedFormula);
      out += "</calculatedColumnFormula>";
    }
    if (!c.totalsFormula.empty()) {
      out += "<totalsRowFormula>";
      pkg::AppendEscapedText(out, c.totalsFormula);
      out += "</totalsRowFormula>";
    }
    out += "</tableColumn>";
  }
  out += "</tableColumns>";
  if (!t.style.name.empty() || t.style.rowStripes || t.style.columnStripes || t.style.firstColumn || t.style.lastColumn) {
    out += "<tableStyleInfo";
    if (!t.style.name.empty()) out += A("name", t.style.name);
    out += A("showFirstColumn", t.style.firstColumn ? "1" : "0") + A("showLastColumn", t.style.lastColumn ? "1" : "0") +
           A("showRowStripes", t.style.rowStripes ? "1" : "0") + A("showColumnStripes", t.style.columnStripes ? "1" : "0") + "/>";
  }
  return out + "</table>";
}

bool UpdateTableXml(std::string& xml, const Table& t) {
  pugi::xml_document doc;
  if (!doc.load_buffer(xml.data(), xml.size(), pugi::parse_default | pugi::parse_ws_pcdata_single | pugi::parse_comments))
    return false;
  pugi::xml_node n = doc.document_element();
  if (Local(n) != "table") return false;
  const std::string p = Prefix(n);
  Set(n, "name", t.name);
  Set(n, "displayName", t.displayName.empty() ? t.name : t.displayName);
  Set(n, "ref", RangeName(t.ref));
  Set(n, "headerRowCount", "0", !t.headerRows);
  Set(n, "totalsRowCount", "1", t.totalsRows != 0);
  Set(n, "totalsRowShown", "0", !t.totalsRows && !t.totalsRowShown);
  // The filter buttons: the element keeps its criteria when the table only grows or shrinks.
  pugi::xml_node filter = ChildOf(n, "autoFilter");
  if (t.autoFilter && t.headerRows) {
    if (!filter) {
      pugi::xml_node cols = ChildOf(n, "tableColumns");
      filter = cols ? n.insert_child_before((p + "autoFilter").c_str(), cols) : n.prepend_child((p + "autoFilter").c_str());
    }
    Set(filter, "ref", RangeName(FilterRange(t)));
  } else if (filter) {
    n.remove_child(filter);
  }
  // The columns: kept by position, added at the end with new ids, the extra ones gone.
  pugi::xml_node cols = ChildOf(n, "tableColumns");
  if (!cols) cols = n.append_child((p + "tableColumns").c_str());
  std::vector<pugi::xml_node> have;
  for (pugi::xml_node c = cols.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && Local(c) == "tableColumn") have.push_back(c);
  for (size_t i = t.columns.size(); i < have.size(); ++i) cols.remove_child(have[i]);
  for (size_t i = 0; i < t.columns.size(); ++i) {
    const TableColumn& c = t.columns[i];
    pugi::xml_node e = i < have.size() ? have[i] : cols.append_child((p + "tableColumn").c_str());
    Set(e, "id", std::to_string(c.id));
    Set(e, "name", ColumnName(c.name));
    Set(e, "totalsRowLabel", c.totalsLabel, !c.totalsLabel.empty());
    Set(e, "totalsRowFunction", c.totalsFunction, !c.totalsFunction.empty());
    // The formulas a total row's custom function or a calculated column has.
    for (const char* kind : {"calculatedColumnFormula", "totalsRowFormula"}) {
      const std::string& f = std::string_view(kind) == "totalsRowFormula" ? c.totalsFormula : c.calculatedFormula;
      pugi::xml_node x = ChildOf(e, kind);
      if (f.empty()) {
        if (x) e.remove_child(x);
        continue;
      }
      if (!x) x = e.append_child((p + kind).c_str());
      while (x.first_child()) x.remove_child(x.first_child());
      x.append_child(pugi::node_pcdata).set_value(f.c_str());
    }
  }
  Set(cols, "count", std::to_string(t.columns.size()));
  pugi::xml_node style = ChildOf(n, "tableStyleInfo");
  if (!style) {
    pugi::xml_node ext = ChildOf(n, "extLst");
    style = ext ? n.insert_child_before((p + "tableStyleInfo").c_str(), ext) : n.append_child((p + "tableStyleInfo").c_str());
  }
  Set(style, "name", t.style.name, !t.style.name.empty());
  Set(style, "showFirstColumn", t.style.firstColumn ? "1" : "0");
  Set(style, "showLastColumn", t.style.lastColumn ? "1" : "0");
  Set(style, "showRowStripes", t.style.rowStripes ? "1" : "0");
  Set(style, "showColumnStripes", t.style.columnStripes ? "1" : "0");
  struct W : pugi::xml_writer {
    std::string s;
    void write(const void* data, size_t size) override { s.append(static_cast<const char*>(data), size); }
  } w;
  doc.save(w, "", pugi::format_raw | pugi::format_no_declaration);
  xml = std::string(kDeclaration) + w.s;
  return true;
}

void PutTablePart(detail::PartText& t, const std::string& relId, bool add) {
  // The prefix the sheet's root binds to the relationships' namespace ("r"), bound when it has none.
  constexpr std::string_view kRelNs = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
  std::string r;
  for (size_t at = t.rootStart.find("xmlns:"); at != std::string::npos; at = t.rootStart.find("xmlns:", at + 6)) {
    const size_t eq = t.rootStart.find('=', at);
    if (eq == std::string::npos) break;
    const size_t q = eq + 1 < t.rootStart.size() ? eq + 1 : eq;
    if (t.rootStart.compare(q + 1, kRelNs.size(), kRelNs) == 0 && q + 1 + kRelNs.size() < t.rootStart.size() &&
        t.rootStart[q + 1 + kRelNs.size()] == t.rootStart[q])
      r = t.rootStart.substr(at + 6, eq - at - 6);
  }
  if (r.empty()) {
    r = "r";
    const size_t end = t.rootStart.rfind('>');
    if (end != std::string::npos) t.rootStart.insert(end, " xmlns:r=\"" + std::string(kRelNs) + "\"");
  }
  std::vector<std::string> ids;
  for (const XmlPiece& p : TopLevel(t.afterData)) {
    if (p.local != "tableParts") continue;
    pugi::xml_document d;
    if (!ParseSlice(d, std::string_view(t.afterData).substr(p.begin, p.end - p.begin))) continue;
    for (pugi::xml_node c = d.document_element().first_child(); c; c = c.next_sibling())
      if (c.type() == pugi::node_element && Local(c) == "tablePart")
        for (pugi::xml_attribute a = c.first_attribute(); a; a = a.next_attribute()) {
          const std::string_view name = a.name();
          if (name.size() > 3 && name.substr(name.size() - 3) == ":id") ids.push_back(a.value());
        }
  }
  ids.erase(std::remove(ids.begin(), ids.end(), relId), ids.end());
  if (add) ids.push_back(relId);
  const std::string p = SheetPrefix(t);
  std::string xml;
  if (!ids.empty()) {
    xml = "<" + p + "tableParts" + A("count", std::to_string(ids.size())) + ">";
    for (const std::string& id : ids) xml += "<" + p + "tablePart" + A(r + ":id", id) + "/>";
    xml += "</" + p + "tableParts>";
  }
  PutSheetElement(t, "tableParts", xml);
}

}  // namespace fw::sheet::xlsx
