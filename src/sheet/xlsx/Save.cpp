// Saving an edited workbook (S2): the parts an edit changed, written from the model; everything
// else copied from the file as it was (plan §4.3 rule 1, §7.3).
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <set>

#include "pkg/XmlWriter.h"
#include "sheet/edit/Charts.h"
#include "sheet/edit/Edit.h"
#include "sheet/xlsx/Internal.h"
#include "sheet/xlsx/Xlsx.h"

namespace fw::sheet::xlsx {

using detail::PartText;
using pkg::Status;

std::vector<XmlPiece> TopLevel(std::string_view x) {
  std::vector<XmlPiece> out;
  int depth = 0;
  XmlPiece cur;
  for (size_t i = x.find('<'); i != std::string_view::npos; i = x.find('<', i)) {
    if (x.compare(i, 4, "<!--") == 0) {
      size_t e = x.find("-->", i);
      if (e == std::string_view::npos) break;
      i = e + 3;
      continue;
    }
    if (x.compare(i, 9, "<![CDATA[") == 0) {
      size_t e = x.find("]]>", i);
      if (e == std::string_view::npos) break;
      i = e + 3;
      continue;
    }
    if (x.compare(i, 2, "<?") == 0) {
      size_t e = x.find("?>", i);
      if (e == std::string_view::npos) break;
      i = e + 2;
      continue;
    }
    bool closing = i + 1 < x.size() && x[i + 1] == '/';
    size_t j = i + 1;
    char quote = 0;
    for (; j < x.size(); ++j) {
      char c = x[j];
      if (quote) {
        if (c == quote) quote = 0;
      } else if (c == '"' || c == '\'') {
        quote = c;
      } else if (c == '>') {
        break;
      }
    }
    if (j >= x.size()) break;
    if (closing) {
      if (--depth == 0) {
        cur.end = j + 1;
        out.push_back(cur);
      }
    } else {
      bool selfClosing = x[j - 1] == '/';
      if (depth == 0) {
        size_t k = i + 1;
        while (k < j && std::strchr(" \t\r\n/>", x[k]) == nullptr) ++k;
        std::string_view qn = x.substr(i + 1, k - i - 1);
        size_t colon = qn.find(':');
        cur = XmlPiece{i, 0, std::string(colon == std::string_view::npos ? qn : qn.substr(colon + 1))};
      }
      if (selfClosing) {
        if (depth == 0) {
          cur.end = j + 1;
          out.push_back(cur);
        }
      } else {
        ++depth;
      }
    }
    i = j + 1;
  }
  return out;
}

namespace {

constexpr std::string_view kDecl = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";
constexpr std::string_view kRelNs = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
constexpr std::string_view kRelType = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
// Strict Open XML (ISO/IEC 29500 Strict) names the same things in its own namespaces.
constexpr std::string_view kRelNsStrict = "http://purl.oclc.org/ooxml/officeDocument/relationships";
constexpr std::string_view kRelTypeStrict = "http://purl.oclc.org/ooxml/officeDocument/relationships/";
constexpr std::string_view kMainNsStrict = "http://purl.oclc.org/ooxml/spreadsheetml/main";
constexpr std::string_view kSheetCt = "application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml";
constexpr std::string_view kStringsCt = "application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml";
// The workbook part's content type for each format.
constexpr std::string_view kXlsxCt = "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml";
constexpr std::string_view kXltxCt = "application/vnd.openxmlformats-officedocument.spreadsheetml.template.main+xml";
constexpr std::string_view kXlsmCt = "application/vnd.ms-excel.sheet.macroEnabled.main+xml";
constexpr std::string_view kXltmCt = "application/vnd.ms-excel.template.macroEnabled.main+xml";

struct StringWriter : pugi::xml_writer {
  std::string s;
  void write(const void* data, size_t size) override { s.append(static_cast<const char*>(data), size); }
};

std::string Print(pugi::xml_node n) {
  StringWriter w;
  n.print(w, "", pugi::format_raw);
  return w.s;
}

std::string PrintDoc(const pugi::xml_document& d) {
  StringWriter w;
  d.save(w, "", pugi::format_raw | pugi::format_no_declaration);
  return std::string(kDecl) + w.s;
}

std::vector<uint8_t> Bytes(std::string_view s) { return std::vector<uint8_t>(s.begin(), s.end()); }

// (Not std::to_chars for doubles: it brings over 100 KB of tables into the exe.)
std::string Num(double v) {
  char buf[48];
  return std::string(buf, FormatNumber(v, 0, buf));
}

std::string AttrXml(std::string_view name, std::string_view value) {
  std::string out = " ";
  out += name;
  out += "=\"";
  pkg::AppendEscapedAttribute(out, value);
  out += '"';
  return out;
}

// "x:" for a root "<x:worksheet ...>", "" without a prefix (or without a root: a sheet that has no
// part yet).
std::string PrefixOf(std::string_view startTag) {
  if (startTag.empty()) return {};
  size_t end = 1;
  while (end < startTag.size() && std::strchr(" \t\r\n/>", startTag[end]) == nullptr) ++end;
  std::string_view name = startTag.substr(1, end - 1);
  size_t colon = name.find(':');
  return colon == std::string_view::npos ? std::string() : std::string(name.substr(0, colon + 1));
}

// The prefix a start tag declares for `ns` ("r"), or "" when it declares none.
std::string DeclaredPrefix(std::string_view startTag, std::string_view ns) {
  std::string needle = "=\"" + std::string(ns) + "\"";
  for (size_t at = startTag.find(needle); at != std::string_view::npos; at = startTag.find(needle, at + 1)) {
    size_t start = startTag.rfind("xmlns:", at);
    if (start == std::string_view::npos) continue;
    std::string_view p = startTag.substr(start + 6, at - start - 6);
    if (!p.empty() && p.find_first_of(" \t\r\n\"=") == std::string_view::npos) return std::string(p);
  }
  return {};
}

// ---------- The worksheet's elements around sheetData ----------

// CT_Worksheet's elements, in schema order.
constexpr std::string_view kOrder[] = {
    "sheetPr", "dimension", "sheetViews", "sheetFormatPr", "cols", "sheetData", "sheetCalcPr", "sheetProtection",
    "protectedRanges", "scenarios", "autoFilter", "sortState", "dataConsolidate", "customSheetViews", "mergeCells",
    "phoneticPr", "conditionalFormatting", "dataValidations", "hyperlinks", "printOptions", "pageMargins",
    "pageSetup", "headerFooter", "rowBreaks", "colBreaks", "customProperties", "cellWatches", "ignoredErrors",
    "smartTags", "drawing", "legacyDrawing", "legacyDrawingHF", "drawingHF", "picture", "oleObjects", "controls",
    "webPublishItems", "tableParts", "extLst"};

int OrderOf(std::string_view local) {
  for (size_t i = 0; i < std::size(kOrder); ++i)
    if (kOrder[i] == local) return static_cast<int>(i);
  return 1000;
}


// Whether `local` goes before sheetData. A part read without one (a chart sheet's) keeps all it
// has before.
bool BeforeData(const PartText& t, std::string_view local) {
  return OrderOf(local) < OrderOf("sheetData") || (!t.hasData && !t.rootStart.empty());
}

// The element `local` as the kept XML has it (empty when it has none).
std::string_view FindElement(const PartText& t, std::string_view local) {
  const std::string& seg = BeforeData(t, local) ? t.beforeData : t.afterData;
  for (const XmlPiece& p : TopLevel(seg))
    if (p.local == local) return std::string_view(seg).substr(p.begin, p.end - p.begin);
  return {};
}

// Puts `xml` where element `local` is (empty `xml` removes it), or where the schema puts it.
void PutElement(PartText& t, std::string_view local, const std::string& xml) {
  int order = OrderOf(local);
  std::string& seg = BeforeData(t, local) ? t.beforeData : t.afterData;
  std::vector<XmlPiece> pieces = TopLevel(seg);
  for (const XmlPiece& p : pieces)
    if (p.local == local) {
      seg.replace(p.begin, p.end - p.begin, xml);
      return;
    }
  if (xml.empty()) return;
  for (const XmlPiece& p : pieces)
    if (OrderOf(p.local) > order) {
      seg.insert(p.begin, xml);
      return;
    }
  seg += xml;
}

std::string ColorAttrs(const Color& c) {
  std::string out;
  switch (c.kind) {
    case Color::Kind::None: return {};
    case Color::Kind::Auto: out = AttrXml("auto", "1"); break;
    case Color::Kind::Rgb: {
      char buf[9];
      std::snprintf(buf, sizeof buf, "%08X", c.value);
      out = AttrXml("rgb", buf);
      break;
    }
    case Color::Kind::Indexed: out = AttrXml("indexed", std::to_string(c.value)); break;
    case Color::Kind::Theme: out = AttrXml("theme", std::to_string(c.value)); break;
  }
  if (c.tint != 0) out += AttrXml("tint", Num(c.tint));
  return out;
}

void SetAttr(pugi::xml_node n, const char* name, const std::string& value) {
  pugi::xml_attribute a = n.attribute(name);
  if (value.empty()) {
    if (a) n.remove_attribute(a);
    return;
  }
  if (!a) a = n.append_attribute(name);
  a.set_value(value.c_str());
}

// An element of the kept XML as a DOM (its prefixes undeclared: pugi doesn't mind), or a new one.
pugi::xml_node ElementDom(pugi::xml_document& doc, const PartText& t, std::string_view local, const std::string& p) {
  std::string_view kept = FindElement(t, local);
  if (!kept.empty() && ParseSlice(doc, kept)) return doc.document_element();
  doc.reset();
  return doc.append_child((p + std::string(local)).c_str());
}

std::string SheetPrXml(const Sheet& s, const PartText& t, const std::string& p) {
  pugi::xml_document doc;
  pugi::xml_node pr = ElementDom(doc, t, "sheetPr", p);
  pugi::xml_node tab = Child(pr, "tabColor");
  const Color& c = s.Props().tabColor;
  if (!c.IsSet()) {
    if (tab) pr.remove_child(tab);
  } else {
    if (tab) pr.remove_child(tab);
    pugi::xml_document cd;
    std::string xml = "<" + p + "tabColor" + ColorAttrs(c) + "/>";
    if (ParseSlice(cd, xml)) pr.prepend_copy(cd.document_element());
  }
  SetAttr(pr, "filterMode", s.Props().filterMode ? "1" : "");
  if (!pr.first_child() && !pr.first_attribute()) return {};
  return Print(pr);
}

std::string FormatPrXml(const Sheet& s, const PartText& t, const std::string& p) {
  // The deepest outline levels (the edits change those); the rest as the file has it.
  uint8_t rows = 0, cols = 0;
  s.ForEachRow(0, kMaxRows - 1, [&](uint32_t, const RowInfo& r) {
    rows = std::max(rows, r.outlineLevel);
    return true;
  });
  for (const ColumnInfo& c : s.Columns()) cols = std::max(cols, c.outlineLevel);
  bool had = !FindElement(t, "sheetFormatPr").empty();
  if (!had && !rows && !cols) return {};
  pugi::xml_document doc;
  pugi::xml_node f = ElementDom(doc, t, "sheetFormatPr", p);
  if (!had) SetAttr(f, "defaultRowHeight", Num(s.Format().defaultRowHeight));
  SetAttr(f, "outlineLevelRow", rows ? std::to_string(rows) : "");
  SetAttr(f, "outlineLevelCol", cols ? std::to_string(cols) : "");
  return Print(f);
}

std::string ColsXml(const Sheet& s, const std::string& p) {
  if (s.Columns().empty()) return {};
  std::string out = "<" + p + "cols>";
  for (const ColumnInfo& c : s.Columns()) {
    out += "<" + p + "col" + AttrXml("min", std::to_string(c.first + 1)) + AttrXml("max", std::to_string(c.last + 1));
    if (c.width > 0) out += AttrXml("width", Num(c.width));
    if (c.style) out += AttrXml("style", std::to_string(c.style));
    if (c.hidden) out += AttrXml("hidden", "1");
    if (c.bestFit) out += AttrXml("bestFit", "1");
    if (c.customWidth && c.width > 0) out += AttrXml("customWidth", "1");
    if (c.phonetic) out += AttrXml("phonetic", "1");
    if (c.outlineLevel) out += AttrXml("outlineLevel", std::to_string(c.outlineLevel));
    if (c.collapsed) out += AttrXml("collapsed", "1");
    out += "/>";
  }
  return out + "</" + p + "cols>";
}

std::string MergesXml(const Sheet& s, const std::string& p) {
  if (s.Merges().empty()) return {};
  std::string out = "<" + p + "mergeCells" + AttrXml("count", std::to_string(s.Merges().size())) + ">";
  for (const Range& r : s.Merges()) out += "<" + p + "mergeCell" + AttrXml("ref", RangeName(r)) + "/>";
  return out + "</" + p + "mergeCells>";
}

std::string DimensionXml(const Sheet& s, const std::string& p) {
  auto used = s.UsedRange();
  return "<" + p + "dimension" + AttrXml("ref", used ? RangeName(*used) : std::string("A1")) + "/>";
}

std::string_view PaneName(PaneId id) {
  switch (id) {
    case PaneId::BottomRight: return "bottomRight";
    case PaneId::TopRight: return "topRight";
    case PaneId::BottomLeft: return "bottomLeft";
    case PaneId::TopLeft: return "topLeft";
  }
  return "topLeft";
}

std::string SqrefOf(const std::vector<Range>& ranges) {
  std::string out;
  for (const Range& r : ranges) {
    if (!out.empty()) out += ' ';
    out += RangeName(r);
  }
  return out;
}

std::string ViewsXml(const Sheet& s, const PartText& t, const std::string& p, bool active) {
  pugi::xml_document doc;
  pugi::xml_node views = ElementDom(doc, t, "sheetViews", p);
  std::vector<pugi::xml_node> nodes;
  for (pugi::xml_node v : views.children())
    if (v.type() == pugi::node_element && Local(v) == "sheetView") nodes.push_back(v);
  const auto& model = s.Views();
  for (size_t i = 0; i < model.size(); ++i) {
    const SheetView& m = model[i];
    pugi::xml_node v = i < nodes.size() ? nodes[i] : views.append_child((p + "sheetView").c_str());
    if (i == 0) SetAttr(v, "tabSelected", active ? "1" : "");
    SetAttr(v, "view", m.view == ViewMode::PageBreakPreview ? "pageBreakPreview" : m.view == ViewMode::PageLayout ? "pageLayout" : "");
    SetAttr(v, "zoomScale", m.zoomScale != 100 ? std::to_string(m.zoomScale) : "");
    SetAttr(v, "topLeftCell", m.topLeft.row || m.topLeft.col ? CellName(m.topLeft.row, m.topLeft.col) : "");
    SetAttr(v, "workbookViewId", std::to_string(m.workbookViewId));
    // pane and selection first, then whatever else the view holds (pivotSelection, extLst).
    for (pugi::xml_node c = v.first_child(); c;) {
      pugi::xml_node next = c.next_sibling();
      if (c.type() == pugi::node_element && (Local(c) == "pane" || Local(c) == "selection")) v.remove_child(c);
      c = next;
    }
    std::string xml;
    if (m.paneState != PaneState::None) {
      xml += "<" + p + "pane";
      if (m.xSplit > 0) xml += AttrXml("xSplit", Num(m.xSplit));
      if (m.ySplit > 0) xml += AttrXml("ySplit", Num(m.ySplit));
      xml += AttrXml("topLeftCell", CellName(m.paneTopLeft.row, m.paneTopLeft.col));
      xml += AttrXml("activePane", PaneName(m.activePane));
      if (m.paneState == PaneState::Frozen) xml += AttrXml("state", "frozen");
      if (m.paneState == PaneState::FrozenSplit) xml += AttrXml("state", "frozenSplit");
      xml += "/>";
    }
    for (const Selection& sel : m.selections) {
      if (m.paneState == PaneState::None && sel.pane != PaneId::TopLeft) continue;
      xml += "<" + p + "selection";
      if (sel.pane != PaneId::TopLeft) xml += AttrXml("pane", PaneName(sel.pane));
      xml += AttrXml("activeCell", CellName(sel.active.row, sel.active.col));
      if (sel.activeRange) xml += AttrXml("activeCellId", std::to_string(sel.activeRange));
      xml += AttrXml("sqref", SqrefOf(sel.ranges)) + "/>";
    }
    pugi::xml_document add;
    if (!xml.empty() && ParseSlice(add, "<x>" + xml + "</x>")) {
      pugi::xml_node first = v.first_child();
      for (pugi::xml_node c : add.document_element().children())
        first ? v.insert_copy_before(c, first) : v.append_copy(c);
    }
  }
  return Print(views);
}

// `ids`: each link's relationship id (NewLinkRels), else its own.
std::string LinksXml(const Sheet& s, PartText& t, const std::string& p, const std::vector<std::string>& ids) {
  if (s.Hyperlinks().empty()) return {};
  std::string r;  // declared on the root when a link needs it (s3-corpus: openpyxl's sheets)
  std::string out = "<" + p + "hyperlinks>";
  for (size_t i = 0; i < s.Hyperlinks().size(); ++i) {
    const Hyperlink& h = s.Hyperlinks()[i];
    const std::string& id = i < ids.size() ? ids[i] : h.relId;
    out += "<" + p + "hyperlink" + AttrXml("ref", RangeName(h.ref));
    if (!id.empty()) {
      if (r.empty()) r = RelPrefix(t);
      if (r.empty()) return std::string(FindElement(t, "hyperlinks"));  // can't name the relationship: as it was
      out += AttrXml(r + ":id", id);
    }
    if (!h.location.empty()) out += AttrXml("location", h.location);
    if (!h.tooltip.empty()) out += AttrXml("tooltip", h.tooltip);
    if (!h.display.empty()) out += AttrXml("display", h.display);
    out += "/>";
  }
  return out + "</" + p + "hyperlinks>";
}

// Links an edit added to a web address, a file or an e-mail address (a target without a
// relationship id): each gets an external relationship in the sheet's rels, the file's own plus
// these, as Excel writes them. The package keeps the file's rels, so every save numbers them the
// same way. `ids` gets each link's id (empty: none); `rels` the new rels part, when there are new ones.
bool NewLinkRels(const pkg::Package& pkg, const Sheet& s, const std::string& part, std::vector<std::string>& ids,
                 std::vector<uint8_t>& rels) {
  const auto& links = s.Hyperlinks();
  ids.assign(links.size(), {});
  bool any = false;
  for (size_t i = 0; i < links.size(); ++i) {
    ids[i] = links[i].relId;
    any = any || (links[i].relId.empty() && !links[i].target.empty());
  }
  if (!any) return false;
  const pkg::RelationshipSet* set = part.empty() ? nullptr : pkg.RelationshipsOf(part);
  // One the rels have already (an earlier save wrote it) and no other link uses: that one.
  if (set) {
    bool more = false;
    for (size_t i = 0; i < links.size(); ++i) {
      if (!ids[i].empty() || links[i].target.empty()) continue;
      for (const pkg::Relationship& r : set->items)
        if (r.external && r.type.ends_with("/hyperlink") && r.target == links[i].target &&
            std::find(ids.begin(), ids.end(), r.id) == ids.end()) {
          ids[i] = r.id;
          break;
        }
      more = more || ids[i].empty();
    }
    if (!more) return false;
  }
  auto used = [&](const std::string& id) {
    if (set)
      for (const pkg::Relationship& r : set->items)
        if (r.id == id) return true;
    return std::find(ids.begin(), ids.end(), id) != ids.end();
  };
  pugi::xml_document doc;
  std::vector<uint8_t> old;
  if (!set || !pkg.ReadPart(pkg::RelsPartFor(part), old).Ok() || !doc.load_buffer(old.data(), old.size()) ||
      !doc.document_element()) {
    doc.reset();
    doc.append_child("Relationships").append_attribute("xmlns").set_value(std::string(pkg::kRelationshipsNs).c_str());
  }
  pugi::xml_node root = doc.document_element();
  int n = 1;
  for (size_t i = 0; i < links.size(); ++i) {
    if (!ids[i].empty() || links[i].target.empty()) continue;
    while (used("rId" + std::to_string(n))) ++n;
    ids[i] = "rId" + std::to_string(n);
    pugi::xml_node rel = root.append_child("Relationship");
    rel.append_attribute("Id").set_value(ids[i].c_str());
    rel.append_attribute("Type").set_value((std::string(kRelNs) + "/hyperlink").c_str());
    rel.append_attribute("Target").set_value(links[i].target.c_str());
    rel.append_attribute("TargetMode").set_value("External");
  }
  rels = Bytes(PrintDoc(doc));
  return true;
}

// A chart sheet's part: its kept text as the edits left it (page setup), its tab color, and its tab
// selected when it's the active one (not ViewsXml: a chart sheet's view has no cells).
PartText ChartSheetText(const Sheet& s, uint32_t pieces, bool active) {
  PartText t = s.Text();
  const std::string p = PrefixOf(t.rootStart);
  if (pieces & edit::kPieceProps) PutElement(t, "sheetPr", SheetPrXml(s, t, p));
  pugi::xml_document doc;
  pugi::xml_node views = ElementDom(doc, t, "sheetViews", p);
  pugi::xml_node v = Child(views, "sheetView");
  if (v && BoolAttr(v, "tabSelected", false) != active) {
    SetAttr(v, "tabSelected", active ? "1" : "");
    PutElement(t, "sheetViews", Print(views));
  }
  return t;
}

// The sheet part's text around sheetData with what the edits changed written from the model.
PartText EditedText(const Sheet& s, uint32_t pieces, bool active, const std::vector<std::string>& linkIds) {
  PartText t = s.Text();
  std::string p = PrefixOf(t.rootStart);
  if (pieces & edit::kPieceProps) PutElement(t, "sheetPr", SheetPrXml(s, t, p));
  if (pieces & edit::kPieceCells) PutElement(t, "dimension", DimensionXml(s, p));
  if ((pieces & edit::kPieceViews) && !s.Views().empty()) PutElement(t, "sheetViews", ViewsXml(s, t, p, active));
  if (pieces & edit::kPieceCols) PutElement(t, "cols", ColsXml(s, p));
  // Outline levels on rows (cells) or columns: sheetFormatPr says the deepest.
  if (pieces & (edit::kPieceCols | edit::kPieceCells | edit::kPieceProps))
    PutElement(t, "sheetFormatPr", FormatPrXml(s, t, p));
  if (pieces & edit::kPieceMerges) PutElement(t, "mergeCells", MergesXml(s, p));
  if (pieces & edit::kPieceLinks) PutElement(t, "hyperlinks", LinksXml(s, t, p, linkIds));
  return t;
}

// A new sheet's whole part around its cells (a sheet added, or one read from a CSV file), with what
// edits put around them before it had a part (a sort's state).
PartText NewSheetText(const Sheet& s, bool active, bool strict, const std::vector<std::string>& linkIds) {
  PartText t;
  t.rootStart = "<worksheet xmlns=\"" + std::string(strict ? kMainNsStrict : kMainNs) + "\" xmlns:r=\"" +
                std::string(strict ? kRelNsStrict : kRelNs) + "\">";
  t.beforeData = s.Text().beforeData;
  t.afterData = s.Text().afterData;
  PutElement(t, "dimension", DimensionXml(s, ""));
  // A sheet read from CSV has no view yet; sheetViews needs one.
  PutElement(t, "sheetViews",
             s.Views().empty() ? std::string("<sheetViews><sheetView") + (active ? " tabSelected=\"1\"" : "") +
                                     " workbookViewId=\"0\"/></sheetViews>"
                               : ViewsXml(s, t, "", active));
  PutElement(t, "sheetFormatPr", FormatPrXml(s, t, ""));
  PutElement(t, "cols", ColsXml(s, ""));
  t.dataStart = "<sheetData>";
  t.hasData = true;
  PutElement(t, "mergeCells", MergesXml(s, ""));
  PutElement(t, "hyperlinks", LinksXml(s, t, "", linkIds));
  if (FindElement(t, "pageMargins").empty())
    PutElement(t, "pageMargins",
               "<pageMargins left=\"0.7\" right=\"0.7\" top=\"0.75\" bottom=\"0.75\" header=\"0.3\" footer=\"0.3\"/>");
  t.rootEnd = "</worksheet>";
  if (s.Props().tabColor.IsSet() || s.Props().filterMode) PutElement(t, "sheetPr", SheetPrXml(s, t, ""));
  return t;
}

// ---------- Styles ----------

std::string FontXml(const Font& f) {
  std::string out = "<font>";
  if (f.bold) out += "<b/>";
  if (f.italic) out += "<i/>";
  if (f.strike) out += "<strike/>";
  if (f.condense) out += "<condense/>";
  if (f.extend) out += "<extend/>";
  if (f.outline) out += "<outline/>";
  if (f.shadow) out += "<shadow/>";
  switch (f.underline) {
    case Underline::None: break;
    case Underline::Single: out += "<u/>"; break;
    case Underline::Double: out += "<u val=\"double\"/>"; break;
    case Underline::SingleAccounting: out += "<u val=\"singleAccounting\"/>"; break;
    case Underline::DoubleAccounting: out += "<u val=\"doubleAccounting\"/>"; break;
  }
  if (f.vertAlign == VertAlign::Superscript) out += "<vertAlign val=\"superscript\"/>";
  if (f.vertAlign == VertAlign::Subscript) out += "<vertAlign val=\"subscript\"/>";
  if (f.size > 0) out += "<sz" + AttrXml("val", Num(f.size)) + "/>";
  if (f.color.IsSet()) out += "<color" + ColorAttrs(f.color) + "/>";
  if (!f.name.empty()) out += "<name" + AttrXml("val", f.name) + "/>";
  if (f.family) out += "<family" + AttrXml("val", std::to_string(f.family)) + "/>";
  if (f.charset >= 0) out += "<charset" + AttrXml("val", std::to_string(f.charset)) + "/>";
  if (f.scheme == FontScheme::Major) out += "<scheme val=\"major\"/>";
  if (f.scheme == FontScheme::Minor) out += "<scheme val=\"minor\"/>";
  return out + "</font>";
}

constexpr std::string_view kPatterns[] = {
    "none", "solid", "mediumGray", "darkGray", "lightGray", "darkHorizontal", "darkVertical", "darkDown", "darkUp",
    "darkGrid", "darkTrellis", "lightHorizontal", "lightVertical", "lightDown", "lightUp", "lightGrid",
    "lightTrellis", "gray125", "gray0625"};

std::string FillXml(const Fill& f) {
  if (f.gradient) {
    std::string out = "<fill><gradientFill";
    if (f.path) {
      out += AttrXml("type", "path") + AttrXml("left", Num(f.left)) + AttrXml("right", Num(f.right)) + AttrXml("top", Num(f.top)) +
             AttrXml("bottom", Num(f.bottom));
    } else if (f.degree != 0) {
      out += AttrXml("degree", Num(f.degree));
    }
    out += ">";
    for (const GradientStop& s : f.stops)
      out += "<stop" + AttrXml("position", Num(s.position)) + "><color" + ColorAttrs(s.color) + "/></stop>";
    return out + "</gradientFill></fill>";
  }
  std::string out = "<fill><patternFill" + AttrXml("patternType", kPatterns[static_cast<size_t>(f.pattern)]);
  if (!f.fg.IsSet() && !f.bg.IsSet()) return out + "/></fill>";
  out += ">";
  if (f.fg.IsSet()) out += "<fgColor" + ColorAttrs(f.fg) + "/>";
  if (f.bg.IsSet()) out += "<bgColor" + ColorAttrs(f.bg) + "/>";
  return out + "</patternFill></fill>";
}

constexpr std::string_view kBorderStyles[] = {
    "none", "thin", "medium", "dashed", "dotted", "thick", "double", "hair", "mediumDashed", "dashDot",
    "mediumDashDot", "dashDotDot", "mediumDashDotDot", "slantDashDot"};

// A side with no line keeps its color when it has one (files from other programs write them so).
std::string SideXml(std::string_view name, const BorderSide& s) {
  std::string out = "<" + std::string(name);
  if (s.style != BorderStyle::None) out += AttrXml("style", kBorderStyles[static_cast<size_t>(s.style)]);
  if (!s.color.IsSet()) return out + "/>";
  return out + "><color" + ColorAttrs(s.color) + "/></" + std::string(name) + ">";
}

std::string BorderXml(const Border& b) {
  std::string out = "<border";
  if (b.diagonalUp) out += AttrXml("diagonalUp", "1");
  if (b.diagonalDown) out += AttrXml("diagonalDown", "1");
  out += ">" + SideXml("left", b.left) + SideXml("right", b.right) + SideXml("top", b.top) + SideXml("bottom", b.bottom) +
         SideXml("diagonal", b.diagonal);
  if (b.vertical.style != BorderStyle::None || b.vertical.color.IsSet()) out += SideXml("vertical", b.vertical);
  if (b.horizontal.style != BorderStyle::None || b.horizontal.color.IsSet()) out += SideXml("horizontal", b.horizontal);
  return out + "</border>";
}

// A differential format as Excel writes one: only what it sets; a solid fill as its bgColor alone.
std::string DxfXml(const Dxf& d) {
  std::string out = "<dxf>";
  if (d.font) {
    const Font& f = *d.font;
    out += "<font>";
    if (d.fontBold) out += f.bold ? "<b/>" : "<b val=\"0\"/>";
    if (d.fontItalic) out += f.italic ? "<i/>" : "<i val=\"0\"/>";
    if (d.fontStrike) out += f.strike ? "<strike/>" : "<strike val=\"0\"/>";
    if (d.fontUnderline) out += f.underline == Underline::Double ? "<u val=\"double\"/>" : f.underline == Underline::None ? "<u val=\"none\"/>" : "<u/>";
    if (f.size > 0) out += "<sz" + AttrXml("val", Num(f.size)) + "/>";
    if (f.color.IsSet()) out += "<color" + ColorAttrs(f.color) + "/>";
    if (!f.name.empty()) out += "<name" + AttrXml("val", f.name) + "/>";
    out += "</font>";
  }
  if (d.numFmtId) out += "<numFmt" + AttrXml("numFmtId", std::to_string(*d.numFmtId)) + AttrXml("formatCode", d.numFmtCode) + "/>";
  if (d.fill) {
    const Fill& f = *d.fill;
    if (!f.gradient && f.pattern == PatternType::Solid && !f.fg.IsSet())
      out += f.bg.IsSet() ? "<fill><patternFill><bgColor" + ColorAttrs(f.bg) + "/></patternFill></fill>" : "<fill><patternFill/></fill>";
    else
      out += FillXml(f);
  }
  if (d.border) out += BorderXml(*d.border);
  return out + "</dxf>";
}

constexpr std::string_view kHAlign[] = {"general", "left", "center", "right", "fill", "justify", "centerContinuous", "distributed"};
constexpr std::string_view kVAlign[] = {"bottom", "top", "center", "justify", "distributed"};

// A cell format (cellXfs: apply flags written when set), or a cell style's (cellStyleXfs, `style`:
// no xfId, and the flags written as Excel does, "0" for each part the style leaves out, since a
// missing one there means included).
std::string XfXml(const Xf& x, bool style = false) {
  std::string out = "<xf" + AttrXml("numFmtId", std::to_string(x.numFmt)) + AttrXml("fontId", std::to_string(x.font)) +
                    AttrXml("fillId", std::to_string(x.fill)) + AttrXml("borderId", std::to_string(x.border));
  if (!style) out += AttrXml("xfId", std::to_string(x.parent));
  if (x.quotePrefix) out += AttrXml("quotePrefix", "1");
  if (x.pivotButton) out += AttrXml("pivotButton", "1");
  auto flag = [&](const char* name, bool on) {
    if (on != style) out += AttrXml(name, on ? "1" : "0");
  };
  flag("applyNumberFormat", x.applyNumFmt);
  flag("applyFont", x.applyFont);
  flag("applyFill", x.applyFill);
  flag("applyBorder", x.applyBorder);
  flag("applyAlignment", x.applyAlignment);
  flag("applyProtection", x.applyProtection);
  const Alignment& a = x.alignment;
  std::string align;
  if (a.horizontal != HAlign::General) align += AttrXml("horizontal", kHAlign[static_cast<size_t>(a.horizontal)]);
  if (a.vertical != VAlign::Bottom) align += AttrXml("vertical", kVAlign[static_cast<size_t>(a.vertical)]);
  if (a.rotation) align += AttrXml("textRotation", std::to_string(a.rotation));
  if (a.wrap) align += AttrXml("wrapText", "1");
  if (a.indent) align += AttrXml("indent", std::to_string(a.indent));
  if (a.relativeIndent) align += AttrXml("relativeIndent", std::to_string(a.relativeIndent));
  if (a.justifyLastLine) align += AttrXml("justifyLastLine", "1");
  if (a.shrinkToFit) align += AttrXml("shrinkToFit", "1");
  if (a.readingOrder) align += AttrXml("readingOrder", std::to_string(a.readingOrder));
  std::string prot;
  if (!x.protection.locked) prot += AttrXml("locked", "0");
  if (x.protection.hidden) prot += AttrXml("hidden", "1");
  if (align.empty() && prot.empty()) return out + "/>";
  out += ">";
  if (!align.empty()) out += "<alignment" + align + "/>";
  if (!prot.empty()) out += "<protection" + prot + "/>";
  return out + "</xf>";
}

// A cell style's format (a cellStyleXfs entry).
std::string StyleXfXml(const Xf& x) { return XfXml(x, true); }

std::string CellStyleXml(const CellStyle& s) {
  std::string out = "<cellStyle" + AttrXml("name", s.name) + AttrXml("xfId", std::to_string(s.xf));
  if (s.builtinId >= 0) out += AttrXml("builtinId", std::to_string(s.builtinId));
  if (s.outlineLevel >= 0) out += AttrXml("iLevel", std::to_string(s.outlineLevel));
  if (s.hidden) out += AttrXml("hidden", "1");
  if (s.customBuiltin) out += AttrXml("customBuiltin", "1");
  return out + "/>";
}

// Appends the model's entries past what `list` (fonts, fills, ...) holds, written by `xml`.
template <class T, class Fn>
bool AppendNew(pugi::xml_node root, std::string_view listName, std::string_view itemName, const std::vector<T>& model,
               Fn&& xml, const std::string& p) {
  pugi::xml_node list = Child(root, listName);
  size_t have = 0;
  if (list)
    for (pugi::xml_node c : list.children())
      if (c.type() == pugi::node_element && Local(c) == itemName) ++have;
  if (model.size() <= have) return true;
  if (!list) return false;  // the styles part lacks the list: leave it (the reader made defaults)
  for (size_t i = have; i < model.size(); ++i) {
    std::string item = xml(model[i]);
    if (!p.empty()) {  // the part writes its elements with a prefix
      std::string pre = "<" + p, post = "</" + p;
      std::string out;
      for (size_t k = 0; k < item.size(); ++k) {
        if (item[k] == '<' && k + 1 < item.size() && item[k + 1] == '/') {
          out += post;
          ++k;
        } else if (item[k] == '<') {
          out += pre;
        } else {
          out += item[k];
        }
      }
      item = std::move(out);
    }
    pugi::xml_document d;
    if (!ParseSlice(d, item)) return false;
    list.append_copy(d.document_element());
  }
  SetAttr(list, "count", std::to_string(model.size()));
  return true;
}

Status StylesPart(const Workbook& book, std::vector<uint8_t>& out) {
  const pkg::Package* pkg = book.GetPackage();
  std::vector<uint8_t> bytes;
  Status st = pkg->ReadPart(book.StylesPart(), bytes);
  if (!st.Ok()) return st;
  pugi::xml_document doc;
  if (!doc.load_buffer(bytes.data(), bytes.size(), pugi::parse_default | pugi::parse_ws_pcdata_single | pugi::parse_comments))
    return Status::Fail(Status::Code::Damaged, "The styles can't be written.");
  pugi::xml_node root = doc.document_element();
  std::string p;
  if (std::string_view n = root.name(); n.find(':') != std::string_view::npos) p = std::string(n.substr(0, n.find(':') + 1));
  const Styles& s = book.GetStyles();
  // Custom number formats the file doesn't have.
  pugi::xml_node fmts = Child(root, "numFmts");
  std::vector<std::pair<uint32_t, std::string>> missing;
  for (const auto& [id, code] : s.CustomFormats()) {
    bool have = false;
    if (fmts)
      for (pugi::xml_node f : fmts.children())
        if (Local(f) == "numFmt" && UIntAttr(f, "numFmtId", kNone) == id) have = true;
    if (!have) missing.push_back({id, code});
  }
  if (!missing.empty()) {
    if (!fmts) fmts = root.prepend_child((p + "numFmts").c_str());
    std::sort(missing.begin(), missing.end());
    for (const auto& [id, code] : missing) {
      pugi::xml_node f = fmts.append_child((p + "numFmt").c_str());
      f.append_attribute("numFmtId").set_value(std::to_string(id).c_str());
      f.append_attribute("formatCode").set_value(code.c_str());
    }
    size_t n = 0;
    for (pugi::xml_node f : fmts.children()) n += Local(f) == "numFmt";
    SetAttr(fmts, "count", std::to_string(n));
  }
  if (!AppendNew(root, "fonts", "font", s.Fonts(), FontXml, p) ||
      !AppendNew(root, "fills", "fill", s.Fills(), FillXml, p) ||
      !AppendNew(root, "borders", "border", s.Borders(), BorderXml, p) ||
      !AppendNew(root, "cellXfs", "xf", s.CellXfs(), [](const Xf& x) { return XfXml(x); }, p))
    return Status::Fail(Status::Code::Unsupported, "The workbook's styles can't take the new formats.");
  // Cell styles added (Editor::AddCellStyle): their lists made where the schema puts them when the
  // file has none (cellStyleXfs before cellXfs, cellStyles after).
  if (pugi::xml_node xfs = Child(root, "cellXfs")) {
    pugi::xml_node styleXfs = Child(root, "cellStyleXfs"), styles = Child(root, "cellStyles");
    if (!styleXfs && !s.CellStyleXfs().empty()) root.insert_child_before((p + "cellStyleXfs").c_str(), xfs);
    if (!styles && !s.CellStyles().empty()) root.insert_child_after((p + "cellStyles").c_str(), xfs);
  }
  if (!AppendNew(root, "cellStyleXfs", "xf", s.CellStyleXfs(), StyleXfXml, p) ||
      !AppendNew(root, "cellStyles", "cellStyle", s.CellStyles(), CellStyleXml, p))
    return Status::Fail(Status::Code::Unsupported, "The workbook's styles can't take the new cell styles.");
  // Differential formats added (Editor::AddDxf, conditional formats): the list after cellStyles.
  if (!Child(root, "dxfs") && !s.Dxfs().empty()) {
    pugi::xml_node after = Child(root, "cellStyles");
    if (!after) after = Child(root, "cellXfs");
    if (after) root.insert_child_after((p + "dxfs").c_str(), after);
  }
  if (!AppendNew(root, "dxfs", "dxf", s.Dxfs(), DxfXml, p))
    return Status::Fail(Status::Code::Unsupported, "The workbook's styles can't take the new conditional formats.");
  out = Bytes(PrintDoc(doc));
  return {};
}

// ---------- Shared strings ----------

std::string StringsXml(const StringTable& table, bool strict) {
  std::string out;
  if (table.Part().rootStart.empty()) {  // the workbook had none
    out = std::string(kDecl) + "<sst xmlns=\"" + std::string(strict ? kMainNsStrict : kMainNs) + "\"" +
          AttrXml("uniqueCount", std::to_string(table.Size())) + ">";
    for (uint32_t id = 0; id < table.Size(); ++id) {
      out += (table.Flags(id) & StringTable::kPreserve) ? "<si><t xml:space=\"preserve\">" : "<si><t>";
      pkg::AppendEscapedText(out, table.Text(id));
      out += "</t></si>";
    }
    return out + "</sst>";
  }
  WriteStringsPart(table, out);
  // count (references) is optional and would be stale; uniqueCount is the table's size.
  size_t root = kDecl.size() + table.Part().prolog.size();
  size_t end = root < out.size() ? out.find('>', root) : std::string::npos;
  if (end == std::string::npos) return out;
  std::string tag = out.substr(0, end);
  auto drop = [&](std::string_view name) {
    std::string key = " " + std::string(name) + "=\"";
    size_t at = tag.find(key);
    if (at == std::string::npos) return;
    size_t close = tag.find('"', at + key.size());
    if (close != std::string::npos) tag.erase(at, close + 1 - at);
  };
  drop("count");
  drop("uniqueCount");
  size_t insertAt = tag.size();
  if (!tag.empty() && tag.back() == '/') --insertAt;
  tag.insert(insertAt, AttrXml("uniqueCount", std::to_string(table.Size())));
  return tag + out.substr(end);
}

// A chart sheet's (or dialog or macro sheet's) part, which the model doesn't write: only its tab
// selection, when that no longer matches the active tab. False when it matches.
bool OtherSheetTab(const pkg::Package& pkg, const Sheet& s, bool active, std::vector<uint8_t>& out) {
  std::vector<uint8_t> bytes;
  pugi::xml_document doc;
  if (!pkg.ReadPart(s.PartName(), bytes).Ok() ||
      !doc.load_buffer(bytes.data(), bytes.size(), pugi::parse_default | pugi::parse_ws_pcdata_single | pugi::parse_comments))
    return false;
  pugi::xml_node view = Child(Child(doc.document_element(), "sheetViews"), "sheetView");
  if (!view || BoolAttr(view, "tabSelected", false) == active) return false;
  SetAttr(view, "tabSelected", active ? "1" : "");
  out = Bytes(PrintDoc(doc));
  return true;
}

// Every part the package's relationships reach from its root without going through `cut`
// (PartKeys), as PartKeys.
std::set<std::string> Reachable(const pkg::Package& pkg, const std::set<std::string>& cut) {
  std::set<std::string> seen;
  std::vector<std::string> todo{"/"};
  while (!todo.empty()) {
    std::string source = std::move(todo.back());
    todo.pop_back();
    const pkg::RelationshipSet* set = pkg.RelationshipsOf(source);
    if (!set) continue;
    for (const pkg::Relationship& r : set->items) {
      if (r.external || r.targetPart.empty()) continue;
      std::string key = pkg::PartKey(r.targetPart);
      if (cut.count(key) || !seen.insert(key).second) continue;
      todo.push_back(r.targetPart);
    }
  }
  return seen;
}

}  // namespace

void PutSheetElement(PartText& t, std::string_view local, const std::string& xml) { PutElement(t, local, xml); }
std::string SheetPrefix(const PartText& t) { return PrefixOf(t.rootStart); }
std::string SheetElement(const PartText& t, std::string_view local) { return std::string(FindElement(t, local)); }
std::string RelPrefix(PartText& t) {
  std::string r = DeclaredPrefix(t.rootStart, kRelNs);
  if (r.empty()) r = DeclaredPrefix(t.rootStart, kRelNsStrict);
  if (!r.empty()) return r;
  const size_t end = t.rootStart.rfind('>');
  if (end == std::string::npos) return {};
  const bool strict = t.rootStart.find(kMainNsStrict) != std::string::npos;
  // "r", or "r1", "r2"... when the root binds "r" to something else already (declaring it twice
  // would be an attribute twice: the file wouldn't open).
  const auto declared = [&](const std::string& p) {
    const std::string needle = "xmlns:" + p;
    constexpr std::string_view space = " \t\r\n";
    for (size_t at = t.rootStart.find(needle); at != std::string::npos; at = t.rootStart.find(needle, at + 1)) {
      const size_t after = at + needle.size();
      if (at > 0 && space.find(t.rootStart[at - 1]) != std::string_view::npos && after < t.rootStart.size() &&
          (t.rootStart[after] == '=' || space.find(t.rootStart[after]) != std::string_view::npos))
        return true;
    }
    return false;
  };
  std::string prefix = "r";
  for (int n = 1; declared(prefix); ++n) prefix = "r" + std::to_string(n);
  t.rootStart.insert(end, " xmlns:" + prefix + "=\"" + std::string(strict ? kRelNsStrict : kRelNs) + "\"");
  return prefix;
}

namespace {
// Excel's extensions for x14 conditional formats and validations.
constexpr std::string_view kCfExtUri = "{78C0D931-6437-407d-A8EE-F0AAD7539E65}";
constexpr std::string_view kDvExtUri = "{CCE6A557-97BC-4b89-ADB6-D9C93CAAB3DF}";

// Where an element's start tag ends (after its '>') and its content ends (at its closing tag);
// both `end` for a self-closing one.
void ContentOf(std::string_view seg, const XmlPiece& p, size_t& from, size_t& to) {
  char quote = 0;
  size_t j = p.begin;
  for (; j < p.end; ++j) {
    const char c = seg[j];
    if (quote) {
      if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '>') {
      break;
    }
  }
  if (j >= p.end || seg[j - 1] == '/') {
    from = to = p.end;
    return;
  }
  from = j + 1;
  // Its own closing tag: the last "</" that starts inside it (one at p.end is the next element's,
  // as </extLst> after an ext).
  to = seg.rfind("</", p.end - 1);
  if (to == std::string_view::npos || to < from) to = from;
}

bool HasUri(std::string_view seg, const XmlPiece& p, std::string_view uri) {
  size_t from = 0, to = 0;
  ContentOf(seg, p, from, to);
  std::string_view tag = seg.substr(p.begin, (from == p.end ? p.end : from) - p.begin);
  for (size_t i = 0; i + uri.size() <= tag.size(); ++i)
    if (std::equal(uri.begin(), uri.end(), tag.begin() + static_cast<ptrdiff_t>(i),
                   [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }))
      return true;
  return false;
}

// The extLst element in the part's text after sheetData, and its ext with `uri` (begin == end: none).
struct ExtFound {
  XmlPiece list;
  bool hasList = false;
  size_t inner = 0, innerEnd = 0;  // the extLst's content
  XmlPiece ext;                    // offsets in afterData
  bool hasExt = false;
  std::vector<XmlPiece> exts;      // every ext, offsets in afterData
};

ExtFound FindExt(const std::string& seg, std::string_view uri) {
  ExtFound f;
  for (const XmlPiece& p : TopLevel(seg))
    if (p.local == "extLst") {
      f.list = p;
      f.hasList = true;
    }
  if (!f.hasList) return f;
  ContentOf(seg, f.list, f.inner, f.innerEnd);
  for (XmlPiece e : TopLevel(std::string_view(seg).substr(f.inner, f.innerEnd - f.inner))) {
    e.begin += f.inner;
    e.end += f.inner;
    f.exts.push_back(e);
    if (e.local == "ext" && !f.hasExt && HasUri(seg, e, uri)) {
      f.ext = e;
      f.hasExt = true;
    }
  }
  return f;
}
}  // namespace

void GetSheetRules(const PartText& t, bool validations, std::string& xml, std::string& ext) {
  xml.clear();
  ext.clear();
  const std::string& seg = t.afterData;
  const std::string_view local = validations ? "dataValidations" : "conditionalFormatting";
  for (const XmlPiece& p : TopLevel(seg))
    if (p.local == local) xml.append(seg, p.begin, p.end - p.begin);
  ExtFound f = FindExt(seg, validations ? kDvExtUri : kCfExtUri);
  if (!f.hasExt) return;
  size_t from = 0, to = 0;
  ContentOf(seg, f.ext, from, to);
  ext.assign(seg, from, to - from);
}

void PutSheetRules(PartText& t, bool validations, const std::string& xml, const std::string& ext) {
  std::string& seg = t.afterData;
  const std::string_view local = validations ? "dataValidations" : "conditionalFormatting";
  // The elements themselves: the new ones where the first old one was, or where the schema puts them.
  std::vector<XmlPiece> pieces = TopLevel(seg);
  size_t at = std::string::npos;
  for (size_t i = pieces.size(); i-- > 0;)
    if (pieces[i].local == local) {
      seg.erase(pieces[i].begin, pieces[i].end - pieces[i].begin);
      at = pieces[i].begin;
    }
  if (at == std::string::npos) {
    at = seg.size();
    const int order = OrderOf(local);
    for (const XmlPiece& p : TopLevel(seg))
      if (OrderOf(p.local) > order) {
        at = p.begin;
        break;
      }
  }
  seg.insert(at, xml);
  // The extension.
  const std::string_view uri = validations ? kDvExtUri : kCfExtUri;
  ExtFound f = FindExt(seg, uri);
  if (f.hasExt) {
    if (ext.empty()) {
      if (f.exts.size() == 1) seg.erase(f.list.begin, f.list.end - f.list.begin);  // the list held only it
      else seg.erase(f.ext.begin, f.ext.end - f.ext.begin);
    } else {
      size_t from = 0, to = 0;
      ContentOf(seg, f.ext, from, to);
      if (from == f.ext.end) {  // <ext .../>: written open
        std::string tag(seg, f.ext.begin, f.ext.end - f.ext.begin - 2);
        const std::string name = tag.substr(1, tag.find_first_of(" \t\r\n") - 1);
        seg.replace(f.ext.begin, f.ext.end - f.ext.begin, tag + ">" + ext + "</" + name + ">");
      } else {
        seg.replace(from, to - from, ext);
      }
    }
    return;
  }
  if (ext.empty()) return;
  const std::string p = PrefixOf(t.rootStart);
  const std::string element = "<" + p + "ext uri=\"" + std::string(uri) +
                              "\" xmlns:x14=\"http://schemas.microsoft.com/office/spreadsheetml/2009/9/main\">" + ext +
                              "</" + p + "ext>";
  if (!f.hasList) {
    PutElement(t, "extLst", "<" + p + "extLst>" + element + "</" + p + "extLst>");
    return;
  }
  // Excel's order: the conditional formats' extension first, the validations' after it.
  size_t pos = f.inner;
  if (validations) {
    ExtFound cf = FindExt(seg, kCfExtUri);
    if (cf.hasExt) pos = cf.ext.end;
  }
  seg.insert(pos, element);
}

std::string StyleItemXml(const Font& f) { return FontXml(f); }
std::string StyleItemXml(const Fill& f) { return FillXml(f); }
std::string StyleItemXml(const Border& b) { return BorderXml(b); }
std::string StyleItemXml(const Xf& x) { return XfXml(x); }
std::string StyleItemXml(const Dxf& d) { return DxfXml(d); }

namespace {
template <class T>
bool ReadItem(std::string_view xml, std::string_view list, const std::vector<T>& (Styles::*items)() const, T& out) {
  const std::string doc = "<styleSheet xmlns=\"" + std::string(kMainNs) + "\"><" + std::string(list) + ">" +
                          std::string(xml) + "</" + std::string(list) + "></styleSheet>";
  Styles styles;
  if (!ReadStyles(doc, pkg::Limits::Default(), "journal", styles).Ok() || (styles.*items)().size() != 1) return false;
  out = (styles.*items)()[0];
  return true;
}
}  // namespace

bool ReadStyleItem(std::string_view xml, Font& out) { return ReadItem(xml, "fonts", &Styles::Fonts, out); }
bool ReadStyleItem(std::string_view xml, Fill& out) { return ReadItem(xml, "fills", &Styles::Fills, out); }
bool ReadStyleItem(std::string_view xml, Border& out) { return ReadItem(xml, "borders", &Styles::Borders, out); }
bool ReadStyleItem(std::string_view xml, Xf& out) { return ReadItem(xml, "cellXfs", &Styles::CellXfs, out); }
bool ReadStyleItem(std::string_view xml, Dxf& out) { return ReadItem(xml, "dxfs", &Styles::Dxfs, out); }

// ---------- The workbook ----------

Status PrepareSave(const Workbook& book, const edit::Editor& ed, SavePlan& plan, FileFormat format) {
  plan = SavePlan{};
  plan.format = format;
  const pkg::Package* pkg = book.GetPackage();
  if (!pkg || format == FileFormat::Csv)
    return Status::Fail(Status::Code::Unsupported, "This workbook can't be saved as an Excel workbook.");
  const bool macros = format == FileFormat::Xlsm || format == FileFormat::Xltm;
  if (!macros)
    for (size_t i = 0; i < book.SheetCount(); ++i)
      if (book.GetSheet(i).Kind() == SheetKind::Macrosheet || book.GetSheet(i).Kind() == SheetKind::Dialogsheet)
        return Status::Fail(Status::Code::Unsupported,
                            "The sheet " + book.GetSheet(i).Name() +
                                " holds macros, which only a macro-enabled workbook (.xlsm) keeps.");
  const std::string& wbPart = book.WorkbookPart();
  std::vector<uint8_t> bytes;
  auto put = [&](std::string name, std::vector<uint8_t> data) { plan.changes.replace.push_back({std::move(name), std::move(data)}); };

  // What the file lists: workbook.xml and its relationships.
  Status st = pkg->ReadPart(wbPart, bytes);
  if (!st.Ok()) return st;
  pugi::xml_document wb;
  if (!wb.load_buffer(bytes.data(), bytes.size(), pugi::parse_default | pugi::parse_ws_pcdata_single | pugi::parse_comments))
    return Status::Fail(Status::Code::Damaged, "The workbook part can't be written.");
  pugi::xml_node root = wb.document_element();
  std::string p;
  if (std::string_view n = root.name(); n.find(':') != std::string_view::npos) p = std::string(n.substr(0, n.find(':') + 1));
  // A Strict workbook gets new parts and relationships in Strict's namespaces.
  const std::string mainAttr = p.empty() ? std::string("xmlns") : "xmlns:" + p.substr(0, p.size() - 1);
  const bool strict = std::string_view(root.attribute(mainAttr.c_str()).value()) == kMainNsStrict;
  const std::string relType(strict ? kRelTypeStrict : kRelType);
  plan.strict = strict;
  std::string r;  // the prefix for relationship ids
  for (pugi::xml_attribute a : root.attributes())
    if (std::string_view(a.name()).rfind("xmlns:", 0) == 0 &&
        (std::string_view(a.value()) == kRelNs || std::string_view(a.value()) == kRelNsStrict))
      r = std::string(a.name() + 6);
  if (r.empty()) {  // declared under a prefix of its own (one the root doesn't use yet)
    r = "r";
    while (root.attribute(("xmlns:" + r).c_str())) r += "r";
    root.append_attribute(("xmlns:" + r).c_str()).set_value(std::string(strict ? kRelNsStrict : kRelNs).c_str());
  }
  const pkg::RelationshipSet* wbRels = pkg->RelationshipsOf(wbPart);
  std::string relsPart = pkg::RelsPartFor(wbPart);
  std::vector<uint8_t> relBytes;
  pugi::xml_document rels;
  if (pkg->ReadPart(relsPart, relBytes).Ok()) rels.load_buffer(relBytes.data(), relBytes.size());
  pugi::xml_node relRoot = rels.document_element();
  pugi::xml_document types;
  std::vector<uint8_t> typeBytes;
  if (!pkg->ReadPart("/[Content_Types].xml", typeBytes).Ok() || !types.load_buffer(typeBytes.data(), typeBytes.size()))
    return Status::Fail(Status::Code::Damaged, "The list of contents can't be written.");
  pugi::xml_node typesRoot = types.document_element();
  bool relsChanged = false, typesChanged = false;
  auto dropRel = [&](std::string_view id) {
    for (pugi::xml_node c = relRoot.first_child(); c; c = c.next_sibling())
      if (std::string_view(c.attribute("Id").value()) == id) {
        relRoot.remove_child(c);
        relsChanged = true;
        return;
      }
  };
  auto dropOverride = [&](std::string_view part) {
    for (pugi::xml_node c = typesRoot.first_child(); c; c = c.next_sibling())
      if (pkg::PartKey(c.attribute("PartName").value()) == pkg::PartKey(part)) {
        typesRoot.remove_child(c);
        typesChanged = true;
        return;
      }
  };
  // A sheet's link relationships part: typed, in a package that lists each rels part instead of a
  // Default for .rels (LibreOffice's), for an existing sheet and an added one alike.
  auto putRels = [&](const std::string& relsPart, std::vector<uint8_t> data) {
    bool typed = false;
    for (pugi::xml_node c : typesRoot.children())
      typed = typed || (Local(c) == "Default" && _stricmp(c.attribute("Extension").value(), "rels") == 0) ||
              (Local(c) == "Override" && pkg::PartKey(c.attribute("PartName").value()) == pkg::PartKey(relsPart));
    if (!typed) {
      pugi::xml_node o = typesRoot.append_child("Override");
      o.append_attribute("PartName").set_value(relsPart.c_str());
      o.append_attribute("ContentType").set_value("application/vnd.openxmlformats-package.relationships+xml");
      typesChanged = true;
    }
    put(relsPart, std::move(data));
  };
  auto newRelId = [&]() {
    for (int n = 1;; ++n) {
      std::string id = "rId" + std::to_string(n);
      bool used = false;
      for (pugi::xml_node c : relRoot.children()) used = used || std::string_view(c.attribute("Id").value()) == id;
      if (!used) return id;
    }
  };

  if (!relRoot) return Status::Fail(Status::Code::Damaged, "The workbook's relationships can't be written.");
  // The workbook part's content type says what the file is (Excel refuses an .xlsx typed as .xlsm).
  {
    std::string_view want = format == FileFormat::Xlsm   ? kXlsmCt
                            : format == FileFormat::Xltx ? kXltxCt
                            : format == FileFormat::Xltm ? kXltmCt
                                                         : kXlsxCt;
    pugi::xml_node o;
    for (pugi::xml_node c : typesRoot.children())
      if (Local(c) == "Override" && pkg::PartKey(c.attribute("PartName").value()) == pkg::PartKey(wbPart)) o = c;
    if (!o) {
      o = typesRoot.append_child("Override");
      o.append_attribute("PartName").set_value(wbPart.c_str());
      o.append_attribute("ContentType");
    }
    if (std::string_view(o.attribute("ContentType").value()) != want) {
      o.attribute("ContentType").set_value(std::string(want).c_str());
      typesChanged = true;
    }
  }
  // New parts go next to the workbook part ("/xl/"), their targets relative to it.
  const std::string dir = wbPart.substr(0, wbPart.rfind('/') + 1);
  auto taken = [&](const std::string& part) {
    const std::string key = pkg::PartKey(part);
    const pkg::Part* found = pkg->FindPart(part);
    if (found && !found->deleted) return true;
    for (const auto& [name, data] : plan.changes.replace)
      if (pkg::PartKey(name) == key) return true;
    for (const auto& job : plan.sheets)
      if (pkg::PartKey(job.part) == key) return true;
    for (size_t i = 0; i < book.SheetCount(); ++i)  // written from the model by an earlier save
      if (!book.GetSheet(i).PartName().empty() && pkg::PartKey(book.GetSheet(i).PartName()) == key) return true;
    return false;
  };
  // Leaves a part out of the save, with its relationships and its content type.
  auto leave = [&](const std::string& part) {
    plan.changes.leaveOut.push_back(part);
    std::string partRels = pkg::RelsPartFor(part);
    if (pkg->FindPart(partRels)) plan.changes.leaveOut.push_back(partRels);
    dropOverride(part);
  };

  // Sheets: edited, new, and gone.
  bool anyCells = false;
  std::vector<std::string> keptParts;
  for (size_t i = 0; i < book.SheetCount(); ++i) {
    const Sheet& s = book.GetSheet(i);
    uint32_t pieces = ed.DirtyPieces(static_cast<int>(i));
    bool active = i == book.ActiveSheet();
    // A tab selected in the file that isn't the active one any more (or the other way round) would
    // group sheets in Excel.
    if (s.Loaded() && !s.Views().empty() && s.Views()[0].tabSelected != active) pieces |= edit::kPieceViews;
    anyCells = anyCells || (pieces & edit::kPieceCells);
    if (s.PartName().empty()) {  // new
      SavePlan::SheetJob job;
      job.sheet = &s;
      job.added = true;
      std::string target;
      for (int n = 1;; ++n) {
        target = "worksheets/sheet" + std::to_string(n) + ".xml";
        job.part = dir + target;
        // Nor a name with a leftover relationships part (its sheet missing): the new sheet would get
        // its tables and drawings (found by fuzz_xlsx).
        const pkg::Part* leftover = pkg->FindPart(pkg::RelsPartFor(job.part));
        if (!taken(job.part) && !(leftover && !leftover->deleted)) break;
      }
      job.relId = newRelId();
      pugi::xml_node rel = relRoot.append_child("Relationship");
      rel.append_attribute("Id").set_value(job.relId.c_str());
      rel.append_attribute("Type").set_value((relType + "worksheet").c_str());
      rel.append_attribute("Target").set_value(target.c_str());
      relsChanged = true;
      pugi::xml_node o = typesRoot.append_child("Override");
      o.append_attribute("PartName").set_value(job.part.c_str());
      o.append_attribute("ContentType").set_value(std::string(kSheetCt).c_str());
      typesChanged = true;
      std::vector<std::string> linkIds;
      std::vector<uint8_t> linkRels;
      if (NewLinkRels(*pkg, s, std::string(), linkIds, linkRels)) putRels(pkg::RelsPartFor(job.part), std::move(linkRels));
      job.text = NewSheetText(s, active, strict, linkIds);
      plan.sheets.push_back(std::move(job));
      continue;
    }
    keptParts.push_back(s.PartName());
    // A chart sheet whose page setup (or tab color) changed, or that an earlier save wrote: from its text.
    if (s.Kind() == SheetKind::Chartsheet && s.Loaded() && !s.Text().rootStart.empty() &&
        ((pieces & ~edit::kPieceViews) || ed.MustWrite(static_cast<int>(i)))) {
      plan.sheets.push_back({&s, s.PartName(), s.RelId(), ChartSheetText(s, pieces, active), false});
      continue;
    }
    if (s.Kind() != SheetKind::Worksheet) {
      std::vector<uint8_t> out;
      if (ed.WorkbookDirty() && OtherSheetTab(*pkg, s, active, out)) put(s.PartName(), std::move(out));
      continue;
    }
    if (!pieces && !ed.MustWrite(static_cast<int>(i))) continue;
    if (!s.Loaded()) return Status::Fail(Status::Code::Unsupported, "The sheet " + s.Name() + " can't be written.");
    // Links added to web addresses, files and e-mail addresses: their relationships.
    std::vector<std::string> linkIds;
    std::vector<uint8_t> linkRels;
    if (NewLinkRels(*pkg, s, s.PartName(), linkIds, linkRels)) {
      putRels(pkg::RelsPartFor(s.PartName()), std::move(linkRels));
      pieces |= edit::kPieceLinks;
    }
    // A sheet added since opening whose text an undo took back to before its first save (no root
    // yet): written whole, as a new sheet is, at the part that save gave it (s3-corpus finding).
    plan.sheets.push_back({&s, s.PartName(), s.RelId(),
                           s.Text().rootStart.empty() ? NewSheetText(s, active, strict, linkIds) : EditedText(s, pieces, active, linkIds),
                           false});
  }
  const bool added = std::any_of(plan.sheets.begin(), plan.sheets.end(), [](const auto& j) { return j.added; });
  plan.charts = anyCells || added;
  // Sheets the file has that the workbook doesn't any more go, and so does the calculation chain
  // once cells or sheets changed: it lists formula cells by sheet, and Excel rebuilds it (§7.3).
  // A macro-free format leaves out the VBA project (and its signatures, which only it uses).
  bool dropChain = anyCells || ed.WorkbookDirty() || added;
  std::set<std::string> cut;  // PartKeys
  if (wbRels)
    for (const pkg::Relationship& rel : wbRels->items) {
      if (rel.external || rel.targetPart.empty()) continue;
      bool sheetType = rel.type.ends_with("/worksheet") || rel.type.ends_with("/chartsheet") ||
                       rel.type.ends_with("/dialogsheet") || rel.type.ends_with("/xlMacrosheet");
      bool chain = rel.type.ends_with("/calcChain");
      bool vba = rel.type.ends_with("/vbaProject");
      if (sheetType) {
        bool kept = std::any_of(keptParts.begin(), keptParts.end(),
                                [&](const std::string& k) { return pkg::PartKey(k) == pkg::PartKey(rel.targetPart); });
        if (kept) continue;
        plan.sheetsRemoved = true;
      } else if (!(chain && dropChain) && !(vba && !macros)) {
        continue;
      }
      cut.insert(pkg::PartKey(rel.targetPart));
      dropRel(rel.id);
      leave(rel.targetPart);
    }
  // What only those parts used (a deleted sheet's drawings, tables, comments) goes with them;
  // parts nothing used to begin with stay as they are.
  if (!cut.empty()) {
    std::set<std::string> before = Reachable(*pkg, {}), after = Reachable(*pkg, cut);
    // A pivot cache whose pivot tables all went with a deleted sheet goes too, as Excel does it;
    // not while slicers or timelines (which name caches by id) remain.
    bool cacheUsers = false;
    std::set<std::string> usedKept, usedGone;  // what the parts that stay, and those that go, point to
    for (const pkg::Part& part : pkg->Parts()) {
      std::string key = pkg::PartKey(part.name);
      if (part.deleted || !before.count(key) || key == pkg::PartKey(wbPart)) continue;
      std::string_view ct = pkg->ContentTypeOf(part.name);
      cacheUsers = cacheUsers || (after.count(key) && (ct.find("slicerCache") != std::string_view::npos ||
                                                       ct.find("timelineCache") != std::string_view::npos));
      if (const pkg::RelationshipSet* set = pkg->RelationshipsOf(part.name))
        for (const pkg::Relationship& rel : set->items)
          if (!rel.external) (after.count(key) ? usedKept : usedGone).insert(pkg::PartKey(rel.targetPart));
    }
    bool more = false;
    if (wbRels && !cacheUsers)
      for (const pkg::Relationship& rel : wbRels->items) {
        std::string key = pkg::PartKey(rel.targetPart);
        if (rel.external || !rel.type.ends_with("/pivotCacheDefinition") || !usedGone.count(key) || usedKept.count(key)) continue;
        if (pugi::xml_node caches = Child(root, "pivotCaches")) {
          for (pugi::xml_node c = caches.first_child(); c; c = c.next_sibling())
            if (RelIdAttr(c) == rel.id) {
              caches.remove_child(c);
              break;
            }
          if (!Child(caches, "pivotCache")) root.remove_child(caches);
        }
        cut.insert(key);
        dropRel(rel.id);
        leave(rel.targetPart);
        more = true;
      }
    if (more) after = Reachable(*pkg, cut);
    for (const pkg::Part& part : pkg->Parts()) {
      std::string key = pkg::PartKey(part.name);
      if (part.deleted || cut.count(key) || !before.count(key) || after.count(key)) continue;
      leave(part.name);
    }
  }

  // workbook.xml: sheets, names, the active tab.
  if (ed.WorkbookDirty() || added) {
    pugi::xml_node sheets = Child(root, "sheets");
    if (!sheets) return Status::Fail(Status::Code::Damaged, "The workbook part lists no sheets.");
    std::vector<pugi::xml_node> old;
    for (pugi::xml_node c : sheets.children())
      if (c.type() == pugi::node_element) old.push_back(c);
    pugi::xml_document hold;
    pugi::xml_node bin = hold.append_child("hold");
    for (pugi::xml_node c : old) {
      bin.append_copy(c);
      sheets.remove_child(c);
    }
    const std::string idName = r + ":id";
    for (size_t i = 0; i < book.SheetCount(); ++i) {
      const Sheet& s = book.GetSheet(i);
      std::string relId;
      for (const auto& ns : plan.sheets)
        if (ns.sheet == &s && ns.added) relId = ns.relId;
      pugi::xml_node e;
      if (relId.empty())  // as the file had it, found by its relationship id
        for (pugi::xml_node c : bin.children())
          if (!s.RelId().empty() && RelIdAttr(c) == s.RelId()) {
            e = sheets.append_copy(c);
            break;
          }
      if (!e) {
        e = sheets.append_child((p + "sheet").c_str());
        e.append_attribute("name");
        e.append_attribute("sheetId").set_value(std::to_string(s.SheetId()).c_str());
        e.append_attribute(idName.c_str()).set_value((relId.empty() ? s.RelId() : relId).c_str());
      }
      SetAttr(e, "name", s.Name());
      SetAttr(e, "state", s.State() == SheetState::Hidden ? "hidden" : s.State() == SheetState::VeryHidden ? "veryHidden" : "");
    }
    // Defined names, from the model when they differ from the file's.
    pugi::xml_node names = Child(root, "definedNames");
    pugi::xml_document oldNames;
    if (names) oldNames.append_copy(names);
    std::vector<DefinedName> fileNames;
    if (names)
      for (pugi::xml_node n : names.children()) {
        if (n.type() != pugi::node_element || Local(n) != "definedName") continue;
        fileNames.push_back(DefinedName{std::string(Attr(n, "name")), IntAttr(n, "localSheetId", -1), TextOf(n),
                                        BoolAttr(n, "hidden", false)});
      }
    bool sameNames = fileNames.size() == book.Names().size() &&
                     std::equal(fileNames.begin(), fileNames.end(), book.Names().begin(),
                                [](const DefinedName& a, const DefinedName& b) {
                                  return a.name == b.name && a.sheet == b.sheet && a.formula == b.formula &&
                                         a.hidden == b.hidden;
                                });
    if (sameNames) {
      // as they are
    } else if (!book.Names().empty()) {
      if (!names) {
        pugi::xml_node after = sheets;
        for (std::string_view prior : {"functionGroups", "externalReferences"})
          if (pugi::xml_node x = Child(root, prior)) after = x;
        names = root.insert_child_after((p + "definedNames").c_str(), after);
      }
      while (names.first_child()) names.remove_child(names.first_child());
      for (const DefinedName& n : book.Names()) {
        pugi::xml_node e = names.append_child((p + "definedName").c_str());
        // Attributes the model doesn't keep (comment, function...) come from the name as it was.
        // Matched by name and sheet, else by name alone.
        pugi::xml_node was, byName;
        if (pugi::xml_node list = oldNames.document_element())
          for (pugi::xml_node o : list.children()) {
            if (o.type() != pugi::node_element || !NameEqualsNoCase(Attr(o, "name"), n.name)) continue;
            if (!byName) byName = o;
            if (IntAttr(o, "localSheetId", -1) == n.sheet) {
              was = o;
              break;
            }
          }
        if (!was) was = byName;
        if (was)
          for (pugi::xml_attribute a : was.attributes()) e.append_attribute(a.name()).set_value(a.value());
        SetAttr(e, "name", n.name);
        SetAttr(e, "localSheetId", n.sheet >= 0 ? std::to_string(n.sheet) : "");
        SetAttr(e, "hidden", n.hidden ? "1" : "");
        e.append_child(pugi::node_pcdata).set_value(n.formula.c_str());
      }
    } else if (names) {
      root.remove_child(names);
    }
    // Protect Workbook: the model's element (the file's, unless an edit changed it), before the views.
    if (pugi::xml_node prot = Child(root, "workbookProtection"); (prot ? EmptyElementXml(prot) : std::string()) != book.ProtectionXml()) {
      pugi::xml_node next = prot ? prot.next_sibling() : pugi::xml_node();
      if (prot) root.remove_child(prot);
      pugi::xml_document add;
      if (!book.ProtectionXml().empty() && ParseSlice(add, book.ProtectionXml())) {
        pugi::xml_node e = add.document_element();
        e.set_name((p + "workbookProtection").c_str());
        if (!next) next = Child(root, "bookViews");
        if (!next) next = sheets;
        root.insert_copy_before(e, next);
      }
    }
    // The active tab, in the first workbook view (bookViews comes just before sheets).
    pugi::xml_node views = Child(root, "bookViews");
    if (!views && (book.ActiveSheet() || book.FirstVisibleTab())) views = root.insert_child_before((p + "bookViews").c_str(), sheets);
    pugi::xml_node v = views ? Child(views, "workbookView") : pugi::xml_node();
    if (views && !v) v = views.append_child((p + "workbookView").c_str());
    if (v) {
      SetAttr(v, "activeTab", book.ActiveSheet() ? std::to_string(book.ActiveSheet()) : "");
      SetAttr(v, "firstSheet", book.FirstVisibleTab() ? std::to_string(book.FirstVisibleTab()) : "");
    }
    put(wbPart, Bytes(PrintDoc(wb)));
  }

  // Styles and shared strings (a workbook opened from CSV writes them whole: GiveBasePackage).
  const bool fromCsv = book.Format() == FileFormat::Csv;
  if (ed.StylesDirty() || fromCsv) {
    if (book.StylesPart().empty()) return Status::Fail(Status::Code::Unsupported, "The workbook has no styles part.");
    std::vector<uint8_t> out;
    if (!(st = StylesPart(book, out)).Ok()) return st;
    put(book.StylesPart(), std::move(out));
  }
  if (ed.StringsDirty() || ed.MustWriteStrings() || fromCsv) {
    std::string part = book.StringsPart();
    plan.strings = true;
    if (part.empty()) {
      part = dir + "sharedStrings.xml";
      if (taken(part)) return Status::Fail(Status::Code::Unsupported, "The workbook's shared strings can't be added.");
      pugi::xml_node rel = relRoot.append_child("Relationship");
      rel.append_attribute("Id").set_value(newRelId().c_str());
      rel.append_attribute("Type").set_value((relType + "sharedStrings").c_str());
      rel.append_attribute("Target").set_value("sharedStrings.xml");
      relsChanged = true;
      pugi::xml_node o = typesRoot.append_child("Override");
      o.append_attribute("PartName").set_value(part.c_str());
      o.append_attribute("ContentType").set_value(std::string(kStringsCt).c_str());
      typesChanged = true;
      plan.newStringsPart = true;
    }
    plan.stringsPart = part;
  }
  if (relsChanged) put(relsPart, Bytes(PrintDoc(rels)));
  if (typesChanged) put("/[Content_Types].xml", Bytes(PrintDoc(types)));
  // Parts edits rewrote in the package (structural edits: drawings, charts, notes, tables...),
  // written as the package holds them. Parts edits removed (an undo can remove one an earlier save
  // wrote) are left out, and the plan says so (WrittenParts); not a name a new part took again.
  for (const pkg::Part& part : pkg->Parts()) {
    if (part.modified && !part.deleted) plan.edited.push_back(part.name);
    else if (part.deleted && !pkg->FindPart(part.name) &&
             std::find(plan.changes.leaveOut.begin(), plan.changes.leaveOut.end(), part.name) == plan.changes.leaveOut.end())
      plan.changes.leaveOut.push_back(part.name);
  }
  return {};
}

Status WriteSave(const Workbook& book, SavePlan& plan, pkg::ByteSink& sink) {
  const pkg::Package* pkg = book.GetPackage();
  if (!pkg) return Status::Fail(Status::Code::Unsupported, "This workbook has no file to save into.");
  // Charts' caches follow the cells (s3-charts): a chart whose cells hold what it caches is copied;
  // one read again is written and kept, as the small parts are.
  for (const pkg::Part& part : pkg->Parts()) {
    if (!plan.charts || part.deleted || pkg->ContentTypeOf(part.name) != "application/vnd.openxmlformats-officedocument.drawingml.chart+xml") continue;
    if (std::any_of(plan.changes.replace.begin(), plan.changes.replace.end(), [&](const auto& r) { return r.first == part.name; }) ||
        std::find(plan.changes.leaveOut.begin(), plan.changes.leaveOut.end(), part.name) != plan.changes.leaveOut.end())
      continue;
    std::vector<uint8_t> bytes;
    if (!pkg->ReadPart(part.name, bytes).Ok()) continue;
    std::string xml(bytes.begin(), bytes.end());
    if (edit::charts::RefreshCaches(book, xml)) plan.changes.replace.push_back({part.name, Bytes(xml)});
  }
  plan.charts = false;  // done: a second write of the same plan has them
  // The sheets and the shared strings join the list for this write only: the plan doesn't keep them.
  const size_t small = plan.changes.replace.size();
  for (const SavePlan::SheetJob& job : plan.sheets) {
    std::string xml;
    WriteSheet(*job.sheet, job.text, xml);
    plan.changes.replace.push_back({job.part, Bytes(xml)});
  }
  if (plan.strings) plan.changes.replace.push_back({plan.stringsPart, Bytes(StringsXml(book.Strings(), plan.strict))});
  Status st = pkg->Save(sink, plan.changes);
  plan.changes.replace.resize(small);
  return st;
}

void CommitSave(Workbook& book, edit::Editor& ed, SavePlan& plan) {
  pkg::Package* pkg = book.GetPackage();
  if (!pkg) return ed.EndSave();
  // The package keeps the small parts the save wrote, so the next save starts from them. Sheets and
  // the shared strings aren't kept: every later save writes them from the model (MustWrite).
  for (auto& [name, bytes] : plan.changes.replace) {
    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    // The package's index learns the new relationships and content types; the part keeps the bytes
    // the file has (not the index's own serialization), so later saves copy them unchanged.
    if (pkg::PartKey(name) == pkg::PartKey(pkg::kContentTypesPart)) {
      pkg::ContentTypes types;
      if (pkg::ParseContentTypes(text, pkg->GetLimits(), types).Ok()) pkg->SetContentTypes(std::move(types));
    } else if (std::string source = pkg::SourceOfRelsPart(name); !source.empty()) {
      pkg::RelationshipSet set;
      if (pkg::ParseRelationships(text, source, pkg->GetLimits(), name, set).Ok()) {
        set.relsPart = name;
        pkg->SetRelationships(std::move(set));
      }
    }
    pkg->SetPart(name, std::move(bytes));
  }
  for (const std::string& name : plan.changes.leaveOut) pkg->DeletePart(name);
  std::vector<edit::Editor::Written> written;
  for (SavePlan::SheetJob& job : plan.sheets)
    written.push_back({job.sheet, std::move(job.part), std::move(job.relId), std::move(job.text)});
  ed.Saved(std::move(written), plan.strings, plan.newStringsPart ? plan.stringsPart : std::string(), plan.format,
           plan.sheetsRemoved);
  plan = SavePlan{};
}

std::vector<std::string> WrittenParts(const SavePlan& plan) {
  std::vector<std::string> out;
  for (const auto& [name, bytes] : plan.changes.replace) out.push_back(name);
  for (const std::string& name : plan.edited)
    if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
  for (const SavePlan::SheetJob& job : plan.sheets) out.push_back(job.part);
  if (plan.strings) out.push_back(plan.stringsPart);
  out.insert(out.end(), plan.changes.leaveOut.begin(), plan.changes.leaveOut.end());
  return out;
}

bool HasMacros(const Workbook& book) {
  for (size_t i = 0; i < book.SheetCount(); ++i)
    if (book.GetSheet(i).Kind() == SheetKind::Macrosheet || book.GetSheet(i).Kind() == SheetKind::Dialogsheet)
      return true;
  const pkg::Package* pkg = book.GetPackage();
  const pkg::RelationshipSet* rels = pkg ? pkg->RelationshipsOf(book.WorkbookPart()) : nullptr;
  if (rels)
    for (const pkg::Relationship& r : rels->items)
      if (r.type.ends_with("/vbaProject")) return true;
  return false;
}

}  // namespace fw::sheet::xlsx

namespace fw::sheet::xlsx {

std::unique_ptr<Workbook> NewWorkbook(pkg::Status& status) {
  const std::string main = std::string(kMainNs), rel = std::string(kRelNs);
  const std::string decl(kDecl);
  const std::pair<const char*, std::string> parts[] = {
      {"[Content_Types].xml",
       decl + "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
              "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
              "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
              "<Override PartName=\"/xl/workbook.xml\" ContentType=\"" + std::string(kXlsxCt) + "\"/>"
              "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"" + std::string(kSheetCt) + "\"/>"
              "<Override PartName=\"/xl/styles.xml\" "
              "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/></Types>"},
      {"_rels/.rels",
       decl + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
              "<Relationship Id=\"rId1\" Type=\"" + std::string(kRelType) + "officeDocument\" Target=\"xl/workbook.xml\"/>"
              "</Relationships>"},
      {"xl/workbook.xml",
       decl + "<workbook xmlns=\"" + main + "\" xmlns:r=\"" + rel + "\"><bookViews><workbookView/></bookViews>"
              "<sheets><sheet name=\"Sheet1\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>"},
      {"xl/_rels/workbook.xml.rels",
       decl + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
              "<Relationship Id=\"rId1\" Type=\"" + std::string(kRelType) + "worksheet\" Target=\"worksheets/sheet1.xml\"/>"
              "<Relationship Id=\"rId2\" Type=\"" + std::string(kRelType) + "styles\" Target=\"styles.xml\"/>"
              "</Relationships>"},
      {"xl/worksheets/sheet1.xml",
       decl + "<worksheet xmlns=\"" + main + "\" xmlns:r=\"" + rel + "\"><dimension ref=\"A1\"/>"
              "<sheetViews><sheetView tabSelected=\"1\" workbookViewId=\"0\"/></sheetViews>"
              "<sheetFormatPr defaultRowHeight=\"15\"/><sheetData/>"
              "<pageMargins left=\"0.7\" right=\"0.7\" top=\"0.75\" bottom=\"0.75\" header=\"0.3\" footer=\"0.3\"/>"
              "</worksheet>"},
      {"xl/styles.xml",
       decl + "<styleSheet xmlns=\"" + main + "\">"
              "<fonts count=\"1\"><font><sz val=\"11\"/><color theme=\"1\"/><name val=\"Calibri\"/><family val=\"2\"/>"
              "</font></fonts>"
              "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill>"
              "<fill><patternFill patternType=\"gray125\"/></fill></fills>"
              "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
              "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
              "<cellXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/></cellXfs>"
              "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
              "<dxfs count=\"0\"/><tableStyles count=\"0\" defaultTableStyle=\"TableStyleMedium2\" "
              "defaultPivotStyle=\"PivotStyleLight16\"/></styleSheet>"},
  };
  pkg::MemorySink sink;
  pkg::ZipWriter zip(sink);
  for (const auto& [name, xml] : parts) {
    status = zip.Add(name, std::span(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()));
    if (!status.Ok()) return nullptr;
  }
  if (!(status = zip.Finish()).Ok()) return nullptr;
  auto package = pkg::Package::Open(pkg::Snapshot::FromBytes(std::move(sink.bytes)), pkg::Limits::Default(), status);
  return package ? Read(std::move(package), status, {.loadAll = true}) : nullptr;
}

pkg::Status GiveBasePackage(Workbook& book) {
  if (book.GetPackage()) return {};
  const std::string main = std::string(kMainNs), rel = std::string(kRelNs);
  const std::string decl(kDecl);
  const std::pair<const char*, std::string> parts[] = {
      {"[Content_Types].xml",
       decl + "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
              "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
              "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
              "<Override PartName=\"/xl/workbook.xml\" ContentType=\"" + std::string(kXlsxCt) + "\"/>"
              "<Override PartName=\"/xl/styles.xml\" "
              "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/></Types>"},
      {"_rels/.rels",
       decl + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
              "<Relationship Id=\"rId1\" Type=\"" + std::string(kRelType) + "officeDocument\" Target=\"xl/workbook.xml\"/>"
              "</Relationships>"},
      {"xl/workbook.xml",
       decl + "<workbook xmlns=\"" + main + "\" xmlns:r=\"" + rel + "\"><bookViews><workbookView/></bookViews><sheets/></workbook>"},
      {"xl/_rels/workbook.xml.rels",
       decl + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
              "<Relationship Id=\"rId1\" Type=\"" + std::string(kRelType) + "styles\" Target=\"styles.xml\"/>"
              "</Relationships>"},
      // The model's fonts, fills, borders and cell formats are appended to these lists when saved.
      {"xl/styles.xml",
       decl + "<styleSheet xmlns=\"" + main + "\"><fonts count=\"0\"/><fills count=\"0\"/><borders count=\"0\"/>"
              "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
              "<cellXfs count=\"0\"/><cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
              "<dxfs count=\"0\"/><tableStyles count=\"0\" defaultTableStyle=\"TableStyleMedium2\" "
              "defaultPivotStyle=\"PivotStyleLight16\"/></styleSheet>"},
  };
  pkg::MemorySink sink;
  pkg::ZipWriter zip(sink);
  pkg::Status status;
  for (const auto& [name, xml] : parts)
    if (!(status = zip.Add(name, std::span(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()))).Ok()) return status;
  if (!(status = zip.Finish()).Ok()) return status;
  auto package = pkg::Package::Open(pkg::Snapshot::FromBytes(std::move(sink.bytes)), pkg::Limits::Default(), status);
  if (!package) return status;
  WorkbookLoader loader(book);
  loader.SetPackage(std::move(package));
  loader.SetParts("/xl/workbook.xml", "/xl/styles.xml", std::string(), std::string());
  // Sheets read without an id (CSV's) get one: Excel repairs a workbook listing sheetId 0.
  uint32_t last = 0;
  for (size_t i = 0; i < book.SheetCount(); ++i) last = std::max(last, book.GetSheet(i).SheetId());
  for (size_t i = 0; i < book.SheetCount(); ++i)
    if (Sheet& s = book.GetSheet(i); s.SheetId() == 0)
      WorkbookLoader::SetSheetInfo(s, s.Kind(), s.State(), ++last, s.PartName(), s.RelId());
  return {};
}

}  // namespace fw::sheet::xlsx
