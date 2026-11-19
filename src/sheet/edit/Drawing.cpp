#include "sheet/edit/Drawing.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include <pugixml.hpp>

#include "sheet/xlsx/Internal.h"

namespace fw::sheet::edit::drawing {

namespace {

constexpr std::string_view kRelNs = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";

std::string_view LocalOf(std::string_view qname) {
  const size_t colon = qname.find(':');
  return colon == std::string_view::npos ? qname : qname.substr(colon + 1);
}

// The tag starting at `at` ('<'): its end (past '>'), its name, whether it closes ("</") or is
// empty ("/>"). Comments, processing instructions and CDATA come back with an empty name.
struct Tag {
  size_t end = std::string_view::npos;
  std::string_view name;
  bool closing = false, empty = false;
};
Tag ReadTag(std::string_view x, size_t at) {
  Tag t;
  auto skip = [&](std::string_view open, std::string_view close) {
    if (x.compare(at, open.size(), open) != 0) return false;
    const size_t e = x.find(close, at + open.size());
    t.end = e == std::string_view::npos ? e : e + close.size();
    return true;
  };
  if (skip("<!--", "-->") || skip("<![CDATA[", "]]>") || skip("<?", "?>") || skip("<!", ">")) return t;
  size_t i = at + 1;
  if (i < x.size() && x[i] == '/') t.closing = true, ++i;
  const size_t n = i;
  while (i < x.size() && x[i] != '>' && x[i] != '/' && !std::isspace(static_cast<unsigned char>(x[i]))) ++i;
  t.name = x.substr(n, i - n);
  for (char q = 0; i < x.size(); ++i) {
    if (q) {
      if (x[i] == q) q = 0;
    } else if (x[i] == '"' || x[i] == '\'') {
      q = x[i];
    } else if (x[i] == '>') {
      t.empty = x[i - 1] == '/';
      t.end = i + 1;
      break;
    }
  }
  return t;
}

// An attribute's value in a start tag (by its exact name), or empty.
std::string_view AttrIn(std::string_view tag, std::string_view name) {
  for (size_t at = tag.find(name); at != std::string_view::npos; at = tag.find(name, at + 1)) {
    const char before = at ? tag[at - 1] : ' ';
    size_t i = at + name.size();
    while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) ++i;
    if (!std::isspace(static_cast<unsigned char>(before)) || i >= tag.size() || tag[i] != '=') continue;
    ++i;
    while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) ++i;
    if (i >= tag.size() || (tag[i] != '"' && tag[i] != '\'')) return {};
    const size_t e = tag.find(tag[i], i + 1);
    return e == std::string_view::npos ? std::string_view() : tag.substr(i + 1, e - i - 1);
  }
  return {};
}

pugi::xml_node Kid(pugi::xml_node n, std::string_view local) {
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && xlsx::Local(c) == local) return c;
  return {};
}

void SetText(pugi::xml_node n, int64_t v) {
  if (n) n.text().set(std::to_string(v).c_str());
}
void SetAttr(pugi::xml_node n, const char* name, int64_t v) {
  if (n) {
    pugi::xml_attribute a = n.attribute(name);
    if (!a) a = n.append_attribute(name);
    a.set_value(std::to_string(v).c_str());
  }
}
void PutMarker(pugi::xml_node n, const Marker& m) {
  SetText(Kid(n, "col"), m.col);
  SetText(Kid(n, "colOff"), m.colOff);
  SetText(Kid(n, "row"), m.row);
  SetText(Kid(n, "rowOff"), m.rowOff);
}

struct Writer : pugi::xml_writer {
  std::string out;
  void write(const void* data, size_t size) override { out.append(static_cast<const char*>(data), size); }
};

}  // namespace

std::vector<Object> Objects(std::string_view x) {
  std::vector<Object> out;
  int depth = 0;
  bool root = false;
  for (size_t at = x.find('<'); at != std::string_view::npos; at = x.find('<', at)) {
    const Tag t = ReadTag(x, at);
    if (t.end == std::string_view::npos) return {};
    if (!t.name.empty()) {
      if (t.closing) {
        if (--depth == 0) return out;  // the root's end
        if (depth == 1 && !out.empty()) out.back().end = t.end;
      } else {
        if (depth == 1) {
          Object o;
          o.begin = at;
          o.end = t.end;
          o.alternate = LocalOf(t.name) == "AlternateContent";
          out.push_back(o);
        } else if (depth > 1 && !out.empty() && !out.back().id && LocalOf(t.name) == "cNvPr") {
          out.back().id = static_cast<uint32_t>(std::strtoul(std::string(AttrIn(x.substr(at, t.end - at), "id")).c_str(), nullptr, 10));
        }
        if (depth == 0) {
          if (root) return {};
          root = true;
        }
        if (!t.empty) ++depth;
      }
    }
    at = t.end;
  }
  return {};  // never closed
}

const Object* Find(const std::vector<Object>& objects, uint32_t id) {
  if (!id) return nullptr;
  auto it = std::find_if(objects.begin(), objects.end(), [&](const Object& o) { return o.id == id; });
  return it == objects.end() ? nullptr : &*it;
}

uint32_t NextShapeId(std::string_view x) {
  uint32_t id = 1;
  for (size_t at = x.find("cNvPr"); at != std::string_view::npos; at = x.find("cNvPr", at + 5)) {
    const size_t close = x.find('>', at);
    const std::string_view tag = x.substr(at, close == std::string_view::npos ? std::string_view::npos : close - at);
    id = std::max(id, static_cast<uint32_t>(std::strtoul(std::string(AttrIn(tag, "id")).c_str(), nullptr, 10)));
  }
  return id + 1;
}

std::string Placed(std::string_view anchor, const Place& p) {
  pugi::xml_document doc;
  if (!doc.load_buffer(anchor.data(), anchor.size(), pugi::parse_default | pugi::parse_ws_pcdata_single)) return {};
  pugi::xml_node a = doc.document_element();
  const std::string_view kind = xlsx::Local(a);
  if (kind == "twoCellAnchor") {
    PutMarker(Kid(a, "from"), p.from);
    PutMarker(Kid(a, "to"), p.to);
  } else if (kind == "oneCellAnchor") {
    PutMarker(Kid(a, "from"), p.from);
  } else if (kind == "absoluteAnchor") {
    SetAttr(Kid(a, "pos"), "x", p.x);
    SetAttr(Kid(a, "pos"), "y", p.y);
  } else {
    return {};
  }
  if (kind != "twoCellAnchor") {
    SetAttr(Kid(a, "ext"), "cx", p.cx);
    SetAttr(Kid(a, "ext"), "cy", p.cy);
  }
  // The shape's own place (Excel writes it; a chart's frame keeps Excel's zeros).
  for (pugi::xml_node o = a.first_child(); o; o = o.next_sibling()) {
    const std::string_view k = xlsx::Local(o);
    pugi::xml_node xfrm = k == "sp" || k == "pic" || k == "cxnSp" ? Kid(Kid(o, "spPr"), "xfrm")
                          : k == "grpSp"                          ? Kid(Kid(o, "grpSpPr"), "xfrm")
                                                                  : pugi::xml_node();
    if (xfrm) {
      SetAttr(Kid(xfrm, "off"), "x", p.x);
      SetAttr(Kid(xfrm, "off"), "y", p.y);
      SetAttr(Kid(xfrm, "ext"), "cx", p.cx);
      SetAttr(Kid(xfrm, "ext"), "cy", p.cy);
    }
  }
  Writer w;
  doc.save(w, "", pugi::format_raw | pugi::format_no_declaration);
  return std::move(w.out);
}

std::vector<std::string> RelIds(std::string_view object) {
  std::vector<std::string> out;
  pugi::xml_document doc;
  if (!doc.load_buffer(object.data(), object.size(), pugi::parse_default)) return out;
  // Prefixes of the relationships namespace: "r", and any the object declares.
  std::vector<std::string> prefixes{"r"};
  auto walk = [&](auto&& self, pugi::xml_node n, bool collect) -> void {
    for (pugi::xml_attribute at = n.first_attribute(); at; at = at.next_attribute()) {
      const std::string_view name = at.name();
      if (!collect) {
        if (name.substr(0, 6) == "xmlns:" && at.value() == kRelNs) prefixes.emplace_back(name.substr(6));
        continue;
      }
      const size_t colon = name.find(':');
      if (colon == std::string_view::npos || name.substr(0, colon) == "xmlns") continue;
      if (std::find(prefixes.begin(), prefixes.end(), name.substr(0, colon)) != prefixes.end() &&
          std::find(out.begin(), out.end(), at.value()) == out.end())
        out.emplace_back(at.value());
    }
    for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
      if (c.type() == pugi::node_element) self(self, c, collect);
  };
  walk(walk, doc.document_element(), false);
  walk(walk, doc.document_element(), true);
  return out;
}

}  // namespace fw::sheet::edit::drawing
