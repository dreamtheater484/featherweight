#include "sheet/edit/PageSetupXml.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include <pugixml.hpp>

#include "sheet/xlsx/Internal.h"

namespace fw::sheet::edit::pagexml {

namespace {

struct StringWriter : pugi::xml_writer {
  std::string out;
  void write(const void* data, size_t size) override { out.append(static_cast<const char*>(data), size); }
};

std::string Print(pugi::xml_node n) {
  StringWriter w;
  n.print(w, "", pugi::format_raw);
  return w.out;
}

// The element as the file has it, or a new one named `p` + `local`.
pugi::xml_node Dom(pugi::xml_document& doc, std::string_view existing, const std::string& p, const char* local) {
  if (!existing.empty() && xlsx::ParseSlice(doc, existing)) return doc.document_element();
  doc.reset();
  return doc.append_child((p + local).c_str());
}

// The shortest form that reads back as `v`: Excel's 0.74803149606299213 stays that number, 0.7 stays
// "0.7" (s3-corpus: 15 digits changed 13 of 205 workbooks' margins).
std::string Num(double v) {
  char buf[32];
  for (int digits = 15; digits <= 17; ++digits) {
    std::snprintf(buf, sizeof buf, "%.*g", digits, v);
    if (std::strtod(buf, nullptr) == v) break;
  }
  return buf;
}

void Set(pugi::xml_node n, const char* name, const std::string& value) {
  pugi::xml_attribute a = n.attribute(name);
  if (value.empty()) {
    if (a) n.remove_attribute(a);
  } else {
    (a ? a : n.append_attribute(name)).set_value(value.c_str());
  }
}

// The attributes named in `order` first, in that order (Excel's), then the others as they were.
void Reorder(pugi::xml_node n, std::initializer_list<const char*> order) {
  std::vector<std::pair<std::string, std::string>> all;
  for (pugi::xml_attribute a : n.attributes()) all.emplace_back(a.name(), a.value());
  while (n.first_attribute()) n.remove_attribute(n.first_attribute());
  for (const char* name : order)
    for (auto& [k, v] : all)
      if (k == name) n.append_attribute(k.c_str()).set_value(v.c_str());
  for (auto& [k, v] : all)
    if (std::none_of(order.begin(), order.end(), [&](const char* name) { return k == name; }))
      n.append_attribute(k.c_str()).set_value(v.c_str());
}

const char* Flag(bool on) { return on ? "1" : ""; }

}  // namespace

std::string PrintOptions(const PageSetup& page, std::string_view existing, const std::string& p) {
  pugi::xml_document doc;
  pugi::xml_node n = Dom(doc, existing, p, "printOptions");
  Set(n, "horizontalCentered", Flag(page.horizontalCentered));
  Set(n, "verticalCentered", Flag(page.verticalCentered));
  Set(n, "headings", Flag(page.headings));
  Set(n, "gridLines", Flag(page.gridLines));
  Reorder(n, {"horizontalCentered", "verticalCentered", "headings", "gridLines", "gridLinesSet"});
  return n.first_attribute() ? Print(n) : std::string();
}

std::string Margins(const PageMargins& m, std::string_view existing, const std::string& p) {
  // As the file has them while they're the same numbers (Excel writes 17 digits): an edit of the
  // paper alone leaves them as they were, and setting what's there already changes nothing.
  pugi::xml_document doc;
  if (!existing.empty() && xlsx::ParseSlice(doc, existing)) {
    const pugi::xml_node n = doc.document_element();
    const std::pair<const char*, double> sides[] = {{"left", m.left},     {"right", m.right},   {"top", m.top},
                                                    {"bottom", m.bottom}, {"header", m.header}, {"footer", m.footer}};
    bool same = true;
    for (const auto& [name, v] : sides) {
      const pugi::xml_attribute a = n.attribute(name);
      same = same && a && std::strtod(a.value(), nullptr) == v;
    }
    if (same) return std::string(existing);
  }
  return "<" + p + "pageMargins left=\"" + Num(m.left) + "\" right=\"" + Num(m.right) + "\" top=\"" + Num(m.top) +
         "\" bottom=\"" + Num(m.bottom) + "\" header=\"" + Num(m.header) + "\" footer=\"" + Num(m.footer) + "\"/>";
}

std::string Setup(const PageSetup& s, std::string_view existing, const std::string& p) {
  pugi::xml_document doc;
  pugi::xml_node n = Dom(doc, existing, p, "pageSetup");
  Set(n, "paperSize", s.paperSize != 1 && s.paperWidth.empty() ? std::to_string(s.paperSize) : "");
  Set(n, "paperWidth", s.paperWidth);
  Set(n, "paperHeight", s.paperHeight);
  Set(n, "scale", s.scale != 100 ? std::to_string(s.scale) : "");
  Set(n, "firstPageNumber", s.useFirstPageNumber ? std::to_string(s.firstPageNumber) : "");
  Set(n, "fitToWidth", s.fitToWidth != 1 ? std::to_string(s.fitToWidth) : "");
  Set(n, "fitToHeight", s.fitToHeight != 1 ? std::to_string(s.fitToHeight) : "");
  Set(n, "pageOrder", s.pageOrder == PageOrder::OverThenDown ? "overThenDown" : "");
  Set(n, "orientation", s.orientation == Orientation::Landscape  ? "landscape"
                        : s.orientation == Orientation::Portrait ? "portrait"
                                                                 : "");
  Set(n, "blackAndWhite", Flag(s.blackAndWhite));
  Set(n, "draft", Flag(s.draft));
  Set(n, "cellComments", s.cellComments == CellComments::AtEnd         ? "atEnd"
                         : s.cellComments == CellComments::AsDisplayed ? "asDisplayed"
                                                                       : "");
  Set(n, "useFirstPageNumber", Flag(s.useFirstPageNumber));
  Set(n, "errors", s.errors == PrintErrors::Blank ? "blank" : s.errors == PrintErrors::Dash ? "dash" : s.errors == PrintErrors::NA ? "NA" : "");
  Reorder(n, {"paperSize", "paperHeight", "paperWidth", "scale", "firstPageNumber", "fitToWidth", "fitToHeight", "pageOrder",
              "orientation", "usePrinterDefaults", "blackAndWhite", "draft", "cellComments", "useFirstPageNumber", "errors",
              "horizontalDpi", "verticalDpi", "copies"});
  return n.first_attribute() ? Print(n) : std::string();
}

std::string HeadersFooters(const HeaderFooter& hf, std::string_view existing, const std::string& p) {
  pugi::xml_document doc;
  pugi::xml_node n = Dom(doc, existing, p, "headerFooter");
  Set(n, "differentOddEven", Flag(hf.differentOddEven));
  Set(n, "differentFirst", Flag(hf.differentFirst));
  Set(n, "scaleWithDoc", hf.scaleWithDoc ? "" : "0");
  Set(n, "alignWithMargins", hf.alignWithMargins ? "" : "0");
  Reorder(n, {"differentOddEven", "differentFirst", "scaleWithDoc", "alignWithMargins"});
  while (n.first_child()) n.remove_child(n.first_child());
  const std::pair<const char*, const std::string*> parts[] = {
      {"oddHeader", &hf.oddHeader},     {"oddFooter", &hf.oddFooter},     {"evenHeader", &hf.evenHeader},
      {"evenFooter", &hf.evenFooter},   {"firstHeader", &hf.firstHeader}, {"firstFooter", &hf.firstFooter}};
  for (const auto& [name, text] : parts) {
    if (text->empty()) continue;
    pugi::xml_node c = n.append_child((p + name).c_str());
    if (text->front() == ' ' || text->back() == ' ') c.append_attribute("xml:space").set_value("preserve");
    c.text().set(text->c_str());
  }
  return n.first_attribute() || n.first_child() ? Print(n) : std::string();
}

std::string Breaks(const std::vector<PageBreak>& breaks, bool rows, const std::string& p) {
  if (breaks.empty()) return {};
  size_t manual = 0;
  for (const PageBreak& b : breaks) manual += b.manual;
  const std::string name = p + (rows ? "rowBreaks" : "colBreaks");
  std::string out = "<" + name + " count=\"" + std::to_string(breaks.size()) + "\" manualBreakCount=\"" + std::to_string(manual) + "\">";
  for (const PageBreak& b : breaks) {
    out += "<" + p + "brk id=\"" + std::to_string(b.id) + "\"";
    if (b.min) out += " min=\"" + std::to_string(b.min) + "\"";
    if (b.max) out += " max=\"" + std::to_string(b.max) + "\"";
    if (b.manual) out += " man=\"1\"";
    if (b.pivot) out += " pt=\"1\"";
    out += "/>";
  }
  return out + "</" + name + ">";
}

std::string SheetPrFit(std::string_view existing, bool fitToPage, const std::string& p) {
  pugi::xml_document doc;
  pugi::xml_node pr = Dom(doc, existing, p, "sheetPr");
  pugi::xml_node setup = xlsx::Child(pr, "pageSetUpPr");
  if (fitToPage && !setup) {
    // CT_SheetPr: tabColor, outlinePr, pageSetUpPr.
    pugi::xml_node after;
    for (pugi::xml_node c = pr.first_child(); c; c = c.next_sibling())
      if (xlsx::Local(c) == "tabColor" || xlsx::Local(c) == "outlinePr") after = c;
    setup = after ? pr.insert_child_after((p + "pageSetUpPr").c_str(), after) : pr.prepend_child((p + "pageSetUpPr").c_str());
  }
  if (setup) {
    Set(setup, "fitToPage", Flag(fitToPage));
    if (!setup.first_attribute()) pr.remove_child(setup);
  }
  return pr.first_attribute() || pr.first_child() ? Print(pr) : std::string();
}

}  // namespace fw::sheet::edit::pagexml
