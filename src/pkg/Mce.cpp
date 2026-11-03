#include "pkg/Mce.h"

#include <algorithm>

#include "pkg/XmlDom.h"
#include "pkg/XmlPull.h"

namespace fw::pkg::mce {
namespace {

std::vector<std::string_view> Tokens(std::string_view list) {
  std::vector<std::string_view> out;
  size_t i = 0;
  while (i < list.size()) {
    size_t start = list.find_first_not_of(" \t\r\n", i);
    if (start == std::string_view::npos) break;
    size_t end = std::min(list.find_first_of(" \t\r\n", start), list.size());
    out.push_back(list.substr(start, end - start));
    i = end;
  }
  return out;
}

bool Contains(const std::vector<std::string>& v, std::string_view s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

Understood::Understood(std::initializer_list<std::string_view> uris) {
  for (auto u : uris) Add(u);
}

void Understood::Add(std::string_view uri) {
  if (!Has(uri)) uris_.emplace_back(uri);
}

bool Understood::Has(std::string_view uri) const {
  return uri.empty() || uri == kNamespace || uri == kXmlNamespace || uri == kXmlnsNamespace || Contains(uris_, uri);
}

std::vector<std::string> ResolvePrefixes(pugi::xml_node element, std::string_view list, int* undeclared) {
  std::vector<std::string> out;
  for (auto prefix : Tokens(list)) {
    auto ns = LookupNamespace(element, prefix);
    if (ns && !ns->empty()) out.emplace_back(*ns);
    else if (undeclared) ++*undeclared;
  }
  return out;
}

std::vector<std::string> IgnorableNamespaces(pugi::xml_node element) {
  std::vector<std::string> out;
  for (pugi::xml_node n = element; n && n.type() == pugi::node_element; n = n.parent()) {
    if (auto a = FindAttribute(n, kNamespace, "Ignorable"))
      for (auto& ns : ResolvePrefixes(n, a.value()))
        if (!Contains(out, ns)) out.push_back(std::move(ns));
  }
  return out;
}

pugi::xml_node SelectBranch(pugi::xml_node alternateContent, const Understood& understood) {
  pugi::xml_node fallback;
  for (auto child : alternateContent.children()) {
    if (IsElement(child, kNamespace, "Choice")) {
      int undeclared = 0;
      auto required = ResolvePrefixes(child, child.attribute("Requires").value(), &undeclared);
      if (undeclared == 0 && std::all_of(required.begin(), required.end(),
                                         [&](const std::string& ns) { return understood.Has(ns); }))
        return child;
    } else if (!fallback && IsElement(child, kNamespace, "Fallback")) {
      fallback = child;
    }
  }
  return fallback;
}

Handling Classify(pugi::xml_node element, const Understood& understood) {
  std::string_view ns = NamespaceOf(element);
  if (ns == kNamespace && LocalNameOf(element) == "AlternateContent") return Handling::AlternateContent;
  if (understood.Has(ns)) return Handling::Read;
  if (!Contains(IgnorableNamespaces(element), ns)) return Handling::Unknown;
  // mc:ProcessContent on an ancestor lists "prefix:local" or "prefix:*".
  for (pugi::xml_node n = element.parent(); n && n.type() == pugi::node_element; n = n.parent()) {
    auto a = FindAttribute(n, kNamespace, "ProcessContent");
    if (!a) continue;
    for (auto item : Tokens(a.value())) {
      auto itemNs = LookupNamespace(n, QNamePrefix(item));
      std::string_view local = QNameLocal(item);
      if (itemNs && *itemNs == ns && (local == "*" || local == LocalNameOf(element))) return Handling::ProcessContent;
    }
  }
  return Handling::Keep;
}

PartReport CheckPart(pugi::xml_node root, const Understood& understood) {
  PartReport report;
  if (!root) return report;
  pugi::xml_node cur = root;
  for (;;) {
    if (cur.type() == pugi::node_element) {
      if (IsElement(cur, kNamespace, "AlternateContent")) ++report.alternateContent;
      if (auto a = FindAttribute(cur, kNamespace, "MustUnderstand"))
        for (auto& ns : ResolvePrefixes(cur, a.value()))
          if (!understood.Has(ns) && !Contains(report.mustUnderstand, ns)) report.mustUnderstand.push_back(ns);
    }
    if (pugi::xml_node child = cur.first_child()) {
      cur = child;
      continue;
    }
    while (cur != root && !cur.next_sibling()) cur = cur.parent();
    if (cur == root) return report;
    cur = cur.next_sibling();
  }
}

void KeepOnlyBranch(pugi::xml_node alternateContent, pugi::xml_node branch) {
  for (pugi::xml_node child = alternateContent.first_child(); child;) {
    pugi::xml_node next = child.next_sibling();
    if (child != branch && child.type() == pugi::node_element) alternateContent.remove_child(child);
    child = next;
  }
}

}  // namespace fw::pkg::mce
