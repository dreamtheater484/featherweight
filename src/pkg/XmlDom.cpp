#include "pkg/XmlDom.h"

#include <cstring>
#include <vector>

#include "pkg/XmlPull.h"
#include "pkg/XmlWriter.h"

namespace fw::pkg {
namespace {

void WriteStart(XmlWriter& w, pugi::xml_node n) {
  switch (n.type()) {
    case pugi::node_element:
      w.StartElement(n.name());
      for (auto a : n.attributes()) w.Attribute(a.name(), a.value());
      break;
    case pugi::node_pcdata:
      // Whitespace between the declaration and the root isn't content; the declaration ends the line.
      if (n.parent().type() != pugi::node_document) w.Text(n.value());
      break;
    case pugi::node_cdata: w.CData(n.value()); break;
    case pugi::node_comment: w.Comment(n.value()); break;
    case pugi::node_pi: w.ProcessingInstruction(n.name(), n.value()); break;
    case pugi::node_declaration: {
      std::vector<std::pair<std::string_view, std::string_view>> attrs;
      for (auto a : n.attributes()) {
        std::string_view name = a.name();
        attrs.emplace_back(name, name == "encoding" ? std::string_view("UTF-8") : std::string_view(a.value()));
      }
      w.Declaration(attrs);
      break;
    }
    default: break;  // documents have no markup of their own; DTDs never get this far
  }
}

}  // namespace

Status XmlDom::Parse(std::string_view xml, const Limits& limits, std::string_view label) {
  doc_.reset();
  XmlPull pull(xml, limits, label);
  for (;;) {
    XmlPull::Event e = pull.Next();
    if (e == XmlPull::Event::EndDocument) break;
    if (e == XmlPull::Event::Error) return pull.GetStatus();
  }
  constexpr unsigned kFlags = pugi::parse_default | pugi::parse_declaration | pugi::parse_comments | pugi::parse_pi |
                              pugi::parse_ws_pcdata;
  std::string_view utf8 = pull.Source();
  pugi::xml_parse_result result = doc_.load_buffer(utf8.data(), utf8.size(), kFlags, pugi::encoding_utf8);
  if (!result) {
    doc_.reset();
    return Status::Fail(Status::Code::Damaged,
                        (label.empty() ? std::string("This XML") : "Part " + std::string(label)) + " is damaged.");
  }
  return Status{};
}

std::string XmlDom::Serialize() const {
  XmlWriter w;
  WriteNode(w, doc_);
  return w.Take();
}

void WriteNode(XmlWriter& w, pugi::xml_node root) {
  if (!root) return;
  pugi::xml_node cur = root;
  for (;;) {
    WriteStart(w, cur);
    if (pugi::xml_node child = cur.first_child()) {
      cur = child;
      continue;
    }
    if (cur.type() == pugi::node_element) w.EndElement();
    while (cur != root && !cur.next_sibling()) {
      cur = cur.parent();
      if (cur.type() == pugi::node_element) w.EndElement();
    }
    if (cur == root) return;
    cur = cur.next_sibling();
  }
}

std::optional<std::string_view> LookupNamespace(pugi::xml_node node, std::string_view prefix) {
  if (prefix == "xml") return kXmlNamespace;
  if (prefix == "xmlns") return kXmlnsNamespace;
  for (pugi::xml_node n = node; n; n = n.parent()) {
    if (n.type() != pugi::node_element) continue;
    for (auto a : n.attributes()) {
      std::string_view name = a.name();
      if (prefix.empty() ? name == "xmlns" : (name.size() == 6 + prefix.size() && name.starts_with("xmlns:") &&
                                              name.substr(6) == prefix))
        return std::string_view(a.value());
    }
  }
  if (prefix.empty()) return std::string_view();
  return std::nullopt;
}

std::string_view NamespaceOf(pugi::xml_node element) {
  return LookupNamespace(element, QNamePrefix(element.name())).value_or(std::string_view());
}

std::string_view NamespaceOf(pugi::xml_node element, pugi::xml_attribute attr) {
  std::string_view name = attr.name();
  if (name == "xmlns" || name.starts_with("xmlns:")) return kXmlnsNamespace;
  std::string_view prefix = QNamePrefix(name);
  if (prefix.empty()) return {};  // unprefixed attributes are in no namespace
  return LookupNamespace(element, prefix).value_or(std::string_view());
}

std::string_view LocalNameOf(pugi::xml_node element) { return QNameLocal(element.name()); }
std::string_view LocalNameOf(pugi::xml_attribute attr) { return QNameLocal(attr.name()); }

bool IsElement(pugi::xml_node node, std::string_view nsUri, std::string_view local) {
  return node.type() == pugi::node_element && LocalNameOf(node) == local && NamespaceOf(node) == nsUri;
}

pugi::xml_attribute FindAttribute(pugi::xml_node element, std::string_view nsUri, std::string_view local) {
  for (auto a : element.attributes())
    if (LocalNameOf(a) == local && NamespaceOf(element, a) == nsUri) return a;
  return {};
}

std::string ExpandedName(pugi::xml_node element) {
  std::string out = "{";
  out += NamespaceOf(element);
  out += '}';
  out += LocalNameOf(element);
  return out;
}

}  // namespace fw::pkg
