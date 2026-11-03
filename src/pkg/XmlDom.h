#pragma once

#include <pugixml.hpp>

#include <optional>
#include <string>
#include <string_view>

#include "pkg/Limits.h"
#include "pkg/Status.h"

namespace fw::pkg {

class XmlWriter;

// An XML part as a DOM, for the parts that fit one comfortably (styles, settings, relationships,
// slides). It wraps pugixml: models use pugi nodes directly, with the namespace helpers below,
// since pugixml keeps names exactly as written and knows nothing about namespaces.
class XmlDom {
 public:
  // Checks `xml` with XmlPull first (limits, no DTD, well-formed, namespaces declared), so
  // pugixml only ever sees documents XmlPull accepted. Whitespace, comments, processing
  // instructions and the XML declaration are kept, so Serialize() can write them back.
  Status Parse(std::string_view xml, const Limits& limits = Limits::Default(), std::string_view label = {});

  pugi::xml_document& Doc() { return doc_; }
  const pugi::xml_document& Doc() const { return doc_; }
  pugi::xml_node Root() const { return doc_.document_element(); }

  // UTF-8, with the declaration as it was (its encoding, if named, now says UTF-8).
  std::string Serialize() const;

 private:
  pugi::xml_document doc_;
};

// Writes a node and everything under it. Iterative, so a deep tree can't overflow the stack.
void WriteNode(XmlWriter& writer, pugi::xml_node node);

// Namespaces of names as written: these resolve prefixes by walking up the tree.
std::optional<std::string_view> LookupNamespace(pugi::xml_node node, std::string_view prefix);
std::string_view NamespaceOf(pugi::xml_node element);
std::string_view NamespaceOf(pugi::xml_node element, pugi::xml_attribute attr);
std::string_view LocalNameOf(pugi::xml_node element);
std::string_view LocalNameOf(pugi::xml_attribute attr);
bool IsElement(pugi::xml_node node, std::string_view nsUri, std::string_view local);
pugi::xml_attribute FindAttribute(pugi::xml_node element, std::string_view nsUri, std::string_view local);
// "{namespace}local", the name that identifies an element whatever its prefix.
std::string ExpandedName(pugi::xml_node element);

}  // namespace fw::pkg
