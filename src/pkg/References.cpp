#include "pkg/References.h"

#include "pkg/RoundTrip.h"
#include "pkg/XmlDom.h"
#include "pkg/XmlPull.h"
#include "pkg/XmlWriter.h"

namespace fw::pkg {

ReferenceRegistry ReferenceRegistry::Default() {
  ReferenceRegistry r;
  // Every attribute in the relationships namespace holds a relationship id (ST_RelationshipId):
  // r:id, r:embed, r:link, r:pict, r:dm, r:lo, r:qs, r:cs, ...
  r.AddRule({RefKind::RelationshipId,
             "relationship id",
             {{{}, {}, kOfficeRelationshipsNs, {}}, {{}, {}, kStrictRelationshipsNs, {}}}});
  return r;
}

void ReferenceRegistry::AddRule(RefRule rule) { rules_.push_back(std::move(rule)); }

void ReferenceRegistry::AddExtension(std::string uri, ExtensionClass c) {
  extensions_.emplace_back(std::move(uri), c);
}

ExtensionClass ReferenceRegistry::ClassifyExtension(std::string_view uri) const {
  for (const auto& [u, c] : extensions_)
    if (u == uri) return c;
  return ExtensionClass::Unknown;
}

void ReferenceRegistry::AddHook(Hook hook) { hooks_.push_back(std::move(hook)); }

bool ReferenceRegistry::Matches(RefKind kind, pugi::xml_node element, pugi::xml_attribute attr) const {
  std::string_view name = attr.name();
  if (QNamePrefix(name).empty() || name.starts_with("xmlns")) return false;  // every site is namespaced
  std::string_view ns = NamespaceOf(element, attr);
  std::string_view local = QNameLocal(name);
  for (const auto& rule : rules_) {
    if (rule.kind != kind) continue;
    for (const auto& site : rule.sites) {
      if (site.attrNs != ns || (!site.attrLocal.empty() && site.attrLocal != local)) continue;
      if (!site.elementLocal.empty() && !IsElement(element, site.elementNs, site.elementLocal)) continue;
      return true;
    }
  }
  return false;
}

std::vector<FoundRef> ReferenceRegistry::Find(pugi::xml_node root, std::optional<RefKind> kind) const {
  std::vector<FoundRef> out;
  if (!root) return out;
  pugi::xml_node cur = root;
  for (;;) {
    if (cur.type() == pugi::node_element) {
      for (auto a : cur.attributes()) {
        if (*a.value() == '\0') continue;
        for (const auto& rule : rules_) {
          if (kind && rule.kind != *kind) continue;
          if (Matches(rule.kind, cur, a)) {
            out.push_back({rule.kind, cur, a});
            break;
          }
        }
      }
    }
    if (pugi::xml_node child = cur.first_child()) {
      cur = child;
      continue;
    }
    while (cur != root && !cur.next_sibling()) cur = cur.parent();
    if (cur == root) return out;
    cur = cur.next_sibling();
  }
}

size_t ReferenceRegistry::Rename(RefKind kind, std::string_view from, std::string_view to, pugi::xml_node root,
                                 RoundTripStore* store) const {
  size_t changed = 0;
  for (auto& ref : Find(root, kind)) {
    if (from == ref.attr.value()) {
      ref.attr.set_value(std::string(to).c_str());
      ++changed;
    }
  }
  if (store) {
    // Kept fragments are XML text: those that mention the old value are parsed and rewritten.
    store->ForEachFragment([&](NodeId, Fragment& f) {
      if (f.xml.find(from) == std::string::npos) return;
      XmlDom dom;
      if (!dom.Parse(f.xml).Ok() || !dom.Root()) return;  // a comment or text: no references
      size_t n = 0;
      for (auto& ref : Find(dom.Root(), kind)) {
        if (from == ref.attr.value()) {
          ref.attr.set_value(std::string(to).c_str());
          ++n;
        }
      }
      if (n == 0) return;
      XmlWriter w;
      WriteNode(w, dom.Root());
      f.xml = w.Take();
      changed += n;
    });
    // Interned attributes have no element, so only sites on any element apply to them.
    auto anyElementSite = [&](const XmlAttr& a) {
      std::string_view local = QNameLocal(a.qname);
      for (const auto& rule : rules_) {
        if (rule.kind != kind) continue;
        for (const auto& site : rule.sites)
          if (site.elementLocal.empty() && site.attrNs == a.nsUri && (site.attrLocal.empty() || site.attrLocal == local))
            return true;
      }
      return false;
    };
    store->ForEachAttrSet([&](std::vector<XmlAttr>& attrs) {
      for (auto& a : attrs) {
        if (a.value == from && anyElementSite(a)) {
          a.value = to;
          ++changed;
        }
      }
    });
  }
  for (const auto& hook : hooks_) hook(kind, from, to);
  return changed;
}

}  // namespace fw::pkg
