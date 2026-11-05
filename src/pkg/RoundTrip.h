#pragma once

#include <pugixml.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fw::pkg {

class XmlWriter;

// Model nodes are identified by ids the model hands out; the store never looks inside a model.
using NodeId = uint32_t;
inline constexpr NodeId kNoNode = 0;
// Returned by a reader for a known child that is a field of its parent (a property element such
// as w:pPr), not a node of its own.
inline constexpr NodeId kFieldNode = 0xFFFFFFFF;

using AttrSetId = uint32_t;  // 0 is the empty set

// Where a kept fragment goes, relative to its owner.
enum class Slot : uint8_t {
  Before,  // just before the owner's start tag
  After,   // just after the owner's end tag
  Inside,  // inside the owner, right after its known child named `afterChild` (first if empty)
  End,     // inside the owner, after all its known children
};

struct Fragment {
  Slot slot = Slot::After;
  std::string afterChild;  // Inside: expanded name ("{ns}local") of the known child it follows
  std::string xml;         // self-contained: declares the namespace prefixes it uses
  bool pairEnd = false;    // one end of a pair (a range start or end): outlives its owner (rule 2)
  // A model's own mark for a fragment it must find again (a Word drawing kept as an
  // mc:AlternateContent block): it travels with the fragment through every move. 0: none.
  uint32_t tag = 0;
};

struct XmlAttr {
  std::string qname, nsUri, value;
  bool operator==(const XmlAttr&) const = default;
};

// Where a node is split (RoundTripStore::Split).
struct SplitPoint {
  // The first of the node's children that moved to the second half; kNoNode when none did.
  NodeId firstMoved = kNoNode;
  // When none did: whether the split is after all of the node's content (Enter at the end of a
  // paragraph), or inside content the model keeps itself (a run's text).
  bool atEnd = true;
};

// The round-trip store (plan §4.3 rule 1): everything a model doesn't understand, kept so its
// writer can put it back where it was.
//  - Unknown elements, comments and processing instructions become fragments attached to their
//    nearest known neighbor, with their position.
//  - Unknown attributes become interned sets: the many rows or runs that carry the same unknown
//    attributes (x14ac:dyDescent="0.25") share one set, referenced by id.
//  - Fragments follow structural edits: Split, Merge and Delete.
class RoundTripStore {
 public:
  // ---- Fragments
  void Add(NodeId owner, Fragment fragment);  // after the owner's other fragments in that slot
  std::vector<const Fragment*> FragmentsOf(NodeId owner, Slot slot) const;
  std::vector<const Fragment*> FragmentsAfterChild(NodeId owner, std::string_view child) const;
  size_t FragmentCount() const;
  // Every fragment, for updates that must reach inside kept content (the reference registry).
  void ForEachFragment(const std::function<void(NodeId, Fragment&)>& fn);

  // ---- Attribute sets
  AttrSetId Intern(std::vector<XmlAttr> attrs);
  const std::vector<XmlAttr>& Attributes(AttrSetId id) const;
  size_t AttrSetCount() const { return sets_.size() - 1; }
  // Models may keep set ids in their own nodes (rows, cells); these are for those that don't.
  void SetNodeAttributes(NodeId node, AttrSetId id);
  AttrSetId NodeAttributes(NodeId node) const;
  // Rewrites the attributes of every set in place, e.g. to rename a reference.
  void ForEachAttrSet(const std::function<void(std::vector<XmlAttr>&)>& fn);

  // ---- Ownership through structural edits
  // `node` was split at `at`, and `second` is its new second half. Fragments keep their place in
  // the document:
  //  - before the split point (Before, and Inside, which sits among the node's fields) stay with
  //    the first half; those owned by the children go wherever their child went;
  //  - after it move to the second half: End, unless the split is at the end, and After, which
  //    follows the node's end tag, now the second half's.
  // `node` keeps its attributes.
  void Split(NodeId node, NodeId second, SplitPoint at);
  // `second` was merged into `first`, and `firstMoved` is the first of second's children that
  // moved into `first` (kNoNode if none). Fragments keep their document order:
  //  - inline ones (first's End, all of second's inside ones) go before `firstMoved`, or to
  //    first's End when nothing moved;
  //  - ones outside the nodes (first's After, second's Before and After) follow `first`.
  // `first` keeps its attributes; it takes second's when it has none.
  void Merge(NodeId first, NodeId second, NodeId firstMoved = kNoNode);
  // `node` was deleted, and its fragments with it, except the ends of pairs: those move to
  // `survivor` in `slot`, or are returned when there is no survivor.
  std::vector<Fragment> Delete(NodeId node, NodeId survivor = kNoNode, Slot slot = Slot::Before);
  // Every fragment of a node, all slots in order, and putting such a list back exactly: an
  // editor's undo saves them before Split, Merge and Delete change them.
  std::vector<Fragment> FragmentsCopy(NodeId owner) const;
  void SetFragments(NodeId owner, std::vector<Fragment> fragments);

  // ---- Capture, while a reader walks a DOM
  // Attaches every child of `parent` that `known` doesn't recognize to its nearest known
  // neighbor: the previous known sibling (After it, or Inside `parentId` after it when it is a
  // field), else the next known node (Before it), else `parentId` (Inside, first).
  // `known` returns the child's NodeId, kFieldNode, or kNoNode for content it doesn't know.
  // Whitespace between elements is formatting, not content, and isn't kept.
  void CaptureChildren(pugi::xml_node parent, NodeId parentId, const std::function<NodeId(pugi::xml_node)>& known,
                       const std::function<bool(pugi::xml_node)>& isPairEnd = {});
  // Interns the attributes of `element` that `known` doesn't recognize, namespace declarations
  // included (mc:Ignorable needs them) unless `known` claims them.
  AttrSetId CaptureAttributes(pugi::xml_node element, const std::function<bool(pugi::xml_attribute)>& known);
  // A node and its subtree as a self-contained fragment: the prefixes it uses that are declared
  // outside it, including those named in mc prefix lists, are declared on its root.
  static std::string SerializeFragment(pugi::xml_node node);

  // ---- Writing back
  void WriteFragments(XmlWriter& w, NodeId owner, Slot slot) const;
  void WriteFragmentsAfterChild(XmlWriter& w, NodeId owner, std::string_view child) const;
  void WriteAttributes(XmlWriter& w, AttrSetId id) const;

  // ---- Memory (plan §11: the store stays under a quarter of the model's own memory)
  size_t BytesUsed() const { return bytes_; }
  void SetBudget(size_t bytes) { budget_ = bytes; }
  bool OverBudget() const { return budget_ != 0 && bytes_ > budget_; }

 private:
  static size_t Cost(const Fragment& f);
  static size_t Cost(const std::vector<XmlAttr>& attrs);
  static std::string Key(const std::vector<XmlAttr>& attrs);

  std::unordered_map<NodeId, std::vector<Fragment>> fragments_;
  std::vector<std::vector<XmlAttr>> sets_ = std::vector<std::vector<XmlAttr>>(1);  // [0] is empty
  std::unordered_map<std::string, AttrSetId> setIndex_;
  std::unordered_map<NodeId, AttrSetId> nodeAttrs_;
  size_t bytes_ = 0;
  size_t budget_ = 0;
};

}  // namespace fw::pkg
