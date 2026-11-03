#include "pkg/RoundTrip.h"

#include <algorithm>

#include "pkg/Mce.h"
#include "pkg/XmlDom.h"
#include "pkg/XmlPull.h"
#include "pkg/XmlWriter.h"

namespace fw::pkg {
namespace {

bool IsWhitespaceText(pugi::xml_node n) {
  if (n.type() != pugi::node_pcdata) return false;
  std::string_view v = n.value();
  return std::all_of(v.begin(), v.end(), [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
}

void ForEachToken(std::string_view list, const std::function<void(std::string_view)>& fn) {
  size_t i = 0;
  while (i < list.size()) {
    size_t start = list.find_first_not_of(" \t\r\n", i);
    if (start == std::string_view::npos) return;
    size_t end = std::min(list.find_first_of(" \t\r\n", start), list.size());
    fn(list.substr(start, end - start));
    i = end;
  }
}

}  // namespace

// ---------- Fragments ----------

size_t RoundTripStore::Cost(const Fragment& f) { return sizeof(Fragment) + f.xml.size() + f.afterChild.size(); }

size_t RoundTripStore::Cost(const std::vector<XmlAttr>& attrs) {
  size_t n = sizeof(attrs);
  for (const auto& a : attrs) n += sizeof(XmlAttr) + a.qname.size() + a.nsUri.size() + a.value.size();
  return n;
}

void RoundTripStore::Add(NodeId owner, Fragment fragment) {
  bytes_ += Cost(fragment);
  fragments_[owner].push_back(std::move(fragment));
}

std::vector<const Fragment*> RoundTripStore::FragmentsOf(NodeId owner, Slot slot) const {
  std::vector<const Fragment*> out;
  auto it = fragments_.find(owner);
  if (it == fragments_.end()) return out;
  for (const auto& f : it->second)
    if (f.slot == slot) out.push_back(&f);
  return out;
}

std::vector<const Fragment*> RoundTripStore::FragmentsAfterChild(NodeId owner, std::string_view child) const {
  std::vector<const Fragment*> out;
  auto it = fragments_.find(owner);
  if (it == fragments_.end()) return out;
  for (const auto& f : it->second)
    if (f.slot == Slot::Inside && f.afterChild == child) out.push_back(&f);
  return out;
}

size_t RoundTripStore::FragmentCount() const {
  size_t n = 0;
  for (const auto& [owner, list] : fragments_) n += list.size();
  return n;
}

void RoundTripStore::ForEachFragment(const std::function<void(NodeId, Fragment&)>& fn) {
  for (auto& [owner, list] : fragments_) {
    for (auto& f : list) {
      bytes_ -= Cost(f);
      fn(owner, f);
      bytes_ += Cost(f);
    }
  }
}

// ---------- Attribute sets ----------

std::string RoundTripStore::Key(const std::vector<XmlAttr>& attrs) {
  std::string key;
  for (const auto& a : attrs) {
    key += a.qname;
    key += '\x01';
    key += a.nsUri;
    key += '\x01';
    key += a.value;
    key += '\x02';
  }
  return key;
}

AttrSetId RoundTripStore::Intern(std::vector<XmlAttr> attrs) {
  if (attrs.empty()) return 0;
  std::string key = Key(attrs);
  auto it = setIndex_.find(key);
  if (it != setIndex_.end()) return it->second;
  auto id = static_cast<AttrSetId>(sets_.size());
  bytes_ += Cost(attrs) + key.size();
  sets_.push_back(std::move(attrs));
  setIndex_.emplace(std::move(key), id);
  return id;
}

const std::vector<XmlAttr>& RoundTripStore::Attributes(AttrSetId id) const {
  return id < sets_.size() ? sets_[id] : sets_[0];
}

void RoundTripStore::SetNodeAttributes(NodeId node, AttrSetId id) {
  if (id == 0) nodeAttrs_.erase(node);
  else nodeAttrs_[node] = id;
}

AttrSetId RoundTripStore::NodeAttributes(NodeId node) const {
  auto it = nodeAttrs_.find(node);
  return it == nodeAttrs_.end() ? 0 : it->second;
}

void RoundTripStore::ForEachAttrSet(const std::function<void(std::vector<XmlAttr>&)>& fn) {
  setIndex_.clear();
  for (AttrSetId id = 1; id < sets_.size(); ++id) {
    bytes_ -= Cost(sets_[id]);
    fn(sets_[id]);
    bytes_ += Cost(sets_[id]);
    setIndex_.emplace(Key(sets_[id]), id);  // a set that now equals another keeps its own id
  }
}

// ---------- Ownership ----------

void RoundTripStore::Split(NodeId node, NodeId second) {
  auto it = fragments_.find(node);
  if (it == fragments_.end()) return;
  std::vector<Fragment> keep, move;
  for (auto& f : it->second) (f.slot == Slot::After ? move : keep).push_back(std::move(f));
  it->second = std::move(keep);
  if (it->second.empty()) fragments_.erase(it);
  if (move.empty()) return;
  auto& dst = fragments_[second];
  dst.insert(dst.begin(), std::make_move_iterator(move.begin()), std::make_move_iterator(move.end()));
}

void RoundTripStore::Merge(NodeId first, NodeId second, NodeId firstMoved) {
  if (NodeAttributes(first) == 0) SetNodeAttributes(first, NodeAttributes(second));
  nodeAttrs_.erase(second);

  std::vector<Fragment> secondList;
  if (auto it = fragments_.find(second); it != fragments_.end()) {
    secondList = std::move(it->second);
    fragments_.erase(it);
  }
  std::vector<Fragment> firstList;
  if (auto it = fragments_.find(first); it != fragments_.end()) {
    firstList = std::move(it->second);
    fragments_.erase(it);
  }

  // In document order: [first: stay ... End] [first After] [second Before] [second: inside]
  // [second After]. Inline content joins in the middle; content outside the nodes follows first.
  std::vector<Fragment> stay, middle, after, secondBefore, secondAfter;
  for (auto& f : firstList) {
    if (f.slot == Slot::After) after.push_back(std::move(f));
    else if (f.slot == Slot::End && firstMoved != kNoNode) middle.push_back(std::move(f));
    else stay.push_back(std::move(f));
  }
  for (auto& f : secondList) {
    if (f.slot == Slot::Before) {
      secondBefore.push_back(std::move(f));
    } else if (f.slot == Slot::After) {
      secondAfter.push_back(std::move(f));
    } else {
      bytes_ -= f.afterChild.size();
      f.afterChild.clear();  // second's fields are gone; its inside content joins the middle
      middle.push_back(std::move(f));
    }
  }
  for (auto* list : {&secondBefore, &secondAfter})
    for (auto& f : *list) after.push_back(std::move(f));
  for (auto& f : middle) f.slot = firstMoved != kNoNode ? Slot::Before : Slot::End;
  for (auto& f : after) f.slot = Slot::After;

  if (firstMoved != kNoNode && !middle.empty()) {
    auto& moved = fragments_[firstMoved];
    moved.insert(moved.begin(), std::make_move_iterator(middle.begin()), std::make_move_iterator(middle.end()));
    middle.clear();
  }
  for (auto& f : middle) stay.push_back(std::move(f));
  for (auto& f : after) stay.push_back(std::move(f));
  if (!stay.empty()) fragments_[first] = std::move(stay);
}

std::vector<Fragment> RoundTripStore::Delete(NodeId node, NodeId survivor, Slot slot) {
  nodeAttrs_.erase(node);
  std::vector<Fragment> orphans;
  auto it = fragments_.find(node);
  if (it == fragments_.end()) return orphans;
  std::vector<Fragment> list = std::move(it->second);
  fragments_.erase(it);
  for (auto& f : list) {
    bytes_ -= Cost(f);
    if (!f.pairEnd) continue;
    f.slot = slot;
    f.afterChild.clear();
    if (survivor != kNoNode) Add(survivor, std::move(f));
    else orphans.push_back(std::move(f));
  }
  return orphans;
}

// ---------- Capture ----------

std::string RoundTripStore::SerializeFragment(pugi::xml_node node) {
  XmlWriter w;
  if (node.type() != pugi::node_element) {
    WriteNode(w, node);
    return w.Take();
  }
  // Every prefix the subtree uses, including those named in mc prefix lists.
  std::vector<std::string> used;
  auto use = [&](std::string_view prefix) {
    if (prefix == "xml" || prefix == "xmlns") return;
    if (std::find(used.begin(), used.end(), prefix) == used.end()) used.emplace_back(prefix);
  };
  pugi::xml_node cur = node;
  for (;;) {
    if (cur.type() == pugi::node_element) {
      use(QNamePrefix(cur.name()));
      bool isChoice = IsElement(cur, mce::kNamespace, "Choice");
      for (auto a : cur.attributes()) {
        std::string_view name = a.name();
        if (name == "xmlns" || name.starts_with("xmlns:")) continue;
        std::string_view prefix = QNamePrefix(name);
        if (!prefix.empty()) use(prefix);
        std::string_view local = QNameLocal(name);
        if (isChoice && prefix.empty() && local == "Requires") ForEachToken(a.value(), use);
        if (prefix.empty() || NamespaceOf(cur, a) != mce::kNamespace) continue;
        if (local == "Ignorable" || local == "MustUnderstand")
          ForEachToken(a.value(), use);
        else if (local == "ProcessContent" || local == "PreserveElements" || local == "PreserveAttributes")
          ForEachToken(a.value(), [&](std::string_view t) { use(QNamePrefix(t)); });
      }
    }
    if (pugi::xml_node child = cur.first_child(); child && cur.type() == pugi::node_element) {
      cur = child;
      continue;
    }
    while (cur != node && !cur.next_sibling()) cur = cur.parent();
    if (cur == node) break;
    cur = cur.next_sibling();
  }

  w.StartElement(node.name());
  for (const auto& prefix : used) {
    std::string decl = prefix.empty() ? "xmlns" : "xmlns:" + prefix;
    if (node.attribute(decl.c_str())) continue;  // declared on the fragment's root already
    auto ns = LookupNamespace(node.parent(), prefix);
    if (!ns || (prefix.empty() && ns->empty())) continue;
    w.Attribute(decl, *ns);
  }
  for (auto a : node.attributes()) w.Attribute(a.name(), a.value());
  for (auto child : node.children()) WriteNode(w, child);
  w.EndElement();
  return w.Take();
}

void RoundTripStore::CaptureChildren(pugi::xml_node parent, NodeId parentId,
                                     const std::function<NodeId(pugi::xml_node)>& known,
                                     const std::function<bool(pugi::xml_node)>& isPairEnd) {
  std::vector<Fragment> pending;  // unknown content before the first known child
  bool sawKnown = false;
  NodeId prevNode = kNoNode;
  std::string prevField;
  for (auto child : parent.children()) {
    if (IsWhitespaceText(child)) continue;
    NodeId id = child.type() == pugi::node_element ? known(child) : kNoNode;
    if (id == kNoNode) {
      Fragment f;
      f.xml = SerializeFragment(child);
      f.pairEnd = isPairEnd && child.type() == pugi::node_element && isPairEnd(child);
      if (!sawKnown) {
        pending.push_back(std::move(f));
      } else if (prevNode != kNoNode) {
        f.slot = Slot::After;
        Add(prevNode, std::move(f));
      } else {
        f.slot = Slot::Inside;
        f.afterChild = prevField;
        Add(parentId, std::move(f));
      }
      continue;
    }
    if (!sawKnown) {
      sawKnown = true;
      for (auto& f : pending) {
        if (id != kFieldNode) {
          f.slot = Slot::Before;
          Add(id, std::move(f));
        } else {
          f.slot = Slot::Inside;
          Add(parentId, std::move(f));
        }
      }
      pending.clear();
    }
    if (id == kFieldNode) {
      prevNode = kNoNode;
      prevField = ExpandedName(child);
    } else {
      prevNode = id;
    }
  }
  for (auto& f : pending) {
    f.slot = Slot::Inside;
    Add(parentId, std::move(f));
  }
}

AttrSetId RoundTripStore::CaptureAttributes(pugi::xml_node element,
                                            const std::function<bool(pugi::xml_attribute)>& known) {
  std::vector<XmlAttr> attrs;
  for (auto a : element.attributes()) {
    if (known && known(a)) continue;
    attrs.push_back({a.name(), std::string(NamespaceOf(element, a)), a.value()});
  }
  return Intern(std::move(attrs));
}

// ---------- Writing back ----------

void RoundTripStore::WriteFragments(XmlWriter& w, NodeId owner, Slot slot) const {
  for (const Fragment* f : FragmentsOf(owner, slot)) w.Raw(f->xml);
}

void RoundTripStore::WriteFragmentsAfterChild(XmlWriter& w, NodeId owner, std::string_view child) const {
  for (const Fragment* f : FragmentsAfterChild(owner, child)) w.Raw(f->xml);
}

void RoundTripStore::WriteAttributes(XmlWriter& w, AttrSetId id) const {
  for (const auto& a : Attributes(id)) w.Attribute(a.qname, a.value);
}

}  // namespace fw::pkg
