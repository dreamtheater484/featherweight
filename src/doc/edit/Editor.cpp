#include "doc/edit/Editor.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <pugixml.hpp>
#include <unordered_map>
#include <unordered_set>

#include "doc/edit/Kept.h"
#include "doc/edit/TreeEdit.h"
#include "doc/model/Internal.h"
#include "doc/model/Schema.h"
#include "pkg/Package.h"

namespace fw::doc::edit {

using namespace names;

namespace {

constexpr uint32_t kHyperlink = Name("hyperlink"), kIns = Name("ins"), kDel = Name("del"), kMoveFrom = Name("moveFrom"),
                   kMoveTo = Name("moveTo"), kFldSimple = Name("fldSimple"), kSmartTag = Name("smartTag"),
                   kDir = Name("dir"), kBdo = Name("bdo"), kTab = Name("tab"), kBr = Name("br"), kFldChar = Name("fldChar"),
                   kInstrText = Name("instrText"), kXmlSpace = Name("xml:space"), kParaId = Name("w14:paraId"),
                   kTextId = Name("w14:textId"), kRStyle = Name("rStyle"), kLock = Name("lock"), kSdtPr = Name("sdtPr"),
                   kPPrChange = Name("pPrChange"), kRPrChange = Name("rPrChange"), kProofErr = Name("proofErr");

// Run items that stand for something Featherweight can't delete on its own yet: deleting them
// would leave a note or comment without its anchor.
bool Undeletable(uint32_t name) {
  static const std::unordered_set<uint32_t> kinds = {
      Name("footnoteReference"), Name("endnoteReference"), Name("commentReference"), Name("footnoteRef"),
      Name("endnoteRef"), Name("annotationRef"), Name("separator"), Name("continuationSeparator")};
  return kinds.count(name) != 0;
}

// Zero-length range starts: at a split point they go with the text after them.
bool StartMarker(uint32_t name) {
  static const std::unordered_set<uint32_t> kinds = {
      Name("bookmarkStart"), Name("commentRangeStart"), Name("moveFromRangeStart"), Name("moveToRangeStart"),
      Name("permStart"), Name("customXmlInsRangeStart"), Name("customXmlDelRangeStart"),
      Name("customXmlMoveFromRangeStart"), Name("customXmlMoveToRangeStart")};
  return kinds.count(name) != 0;
}

// The start marker an end marker pairs with; 0 for anything else.
uint32_t StartOfEnd(uint32_t name) {
  static const std::pair<uint32_t, uint32_t> kPairs[] = {
      {Name("bookmarkEnd"), Name("bookmarkStart")},
      {Name("commentRangeEnd"), Name("commentRangeStart")},
      {Name("moveFromRangeEnd"), Name("moveFromRangeStart")},
      {Name("moveToRangeEnd"), Name("moveToRangeStart")},
      {Name("permEnd"), Name("permStart")},
      {Name("customXmlInsRangeEnd"), Name("customXmlInsRangeStart")},
      {Name("customXmlDelRangeEnd"), Name("customXmlDelRangeStart")},
      {Name("customXmlMoveFromRangeEnd"), Name("customXmlMoveFromRangeStart")},
      {Name("customXmlMoveToRangeEnd"), Name("customXmlMoveToRangeStart")}};
  for (const auto& [end, start] : kPairs)
    if (name == end) return start;
  return 0;
}

bool RangeMarker(uint32_t name) {
  static const std::unordered_set<uint32_t> kinds = {
      Name("bookmarkStart"), Name("bookmarkEnd"), Name("commentRangeStart"), Name("commentRangeEnd"),
      Name("moveFromRangeStart"), Name("moveFromRangeEnd"), Name("moveToRangeStart"), Name("moveToRangeEnd"),
      Name("permStart"), Name("permEnd"), Name("proofErr"), Name("customXmlInsRangeStart"),
      Name("customXmlInsRangeEnd"), Name("customXmlDelRangeStart"), Name("customXmlDelRangeEnd"),
      Name("customXmlMoveFromRangeStart"), Name("customXmlMoveFromRangeEnd"), Name("customXmlMoveToRangeStart"),
      Name("customXmlMoveToRangeEnd")};
  return kinds.count(name) != 0;
}

// Inline containers a paragraph split may cut in two (each half keeps the properties).
bool Splittable(uint32_t name) {
  return name == kR || name == kHyperlink || name == kIns || name == kDel || name == kMoveFrom || name == kMoveTo ||
         name == kSmartTag || name == kCustomXml || name == kDir || name == kBdo;
}

bool IsFieldChar(char16_t c) { return c == 0x13 || c == 0x14 || c == 0x15; }

std::u16string Msg(const char* s) {
  std::u16string out;
  for (const char* p = s; *p; ++p) out += static_cast<char16_t>(static_cast<unsigned char>(*p));
  return out;
}

struct ElemSave {
  ElemId id;
  Elem e;
};
struct NodeSave {
  pkg::NodeId node;
  std::vector<pkg::Fragment> fragments;
  pkg::AttrSetId attrs;
};
// A run of paragraphs as they were: their text and every element of their subtrees, the elements
// around them, and the round-trip store's entries for those elements.
struct Side {
  std::vector<Paragraph> paras;
  std::vector<ElemSave> elems;
  std::vector<NodeSave> nodes;
};
struct Step {
  uint32_t story = kNone, first = kNone;
  Side before, after;
  bool structure = false;
  std::shared_ptr<CustomStep> custom;  // a direct change (RecordStep): it restores itself
};
enum class Kind : uint8_t { Other, Typing, Deleting };
struct Entry {
  std::u16string name;
  std::vector<Step> steps;
  Pos caretBefore, caretAfter;
  Kind kind = Kind::Other;
  uint64_t serial = 0;
};

}  // namespace

KeptRefs KeptReferences(const Document& doc, const std::vector<uint32_t>& marks) {
  KeptRefs out;
  const std::vector<Section>& sections = doc.Sections();
  std::vector<size_t> ending;
  for (uint32_t p : marks)
    for (size_t s = 0; s + 1 < sections.size(); ++s)
      if (sections[s].endPara == p) ending.push_back(s);
  if (ending.empty()) return out;
  std::sort(ending.begin(), ending.end());
  const Section& next = sections[ending.back() + 1];
  out.sectPr = next.sectPr.Id();
  if (out.sectPr == kNoElem) return out;
  std::vector<std::string> have;  // "headerReference default", ...
  auto key = [](Node ref) { return std::string(ref.Is(Name("headerReference")) ? "headerReference " : "footerReference ") +
                                   std::string(ref.AttrOr(kType, "default")); };
  auto isRef = [](Node n) { return n.Is(Name("headerReference")) || n.Is(Name("footerReference")); };
  for (Node ref : next.sectPr.Kids())
    if (isRef(ref)) have.push_back(key(ref));
  for (auto it = ending.rbegin(); it != ending.rend(); ++it)
    for (Node ref : sections[*it].sectPr.Kids()) {
      if (!isRef(ref) || std::find(have.begin(), have.end(), key(ref)) != have.end()) continue;
      const std::string_view id = ref.AttrOr(Name("r:id")), type = ref.AttrOr(kType, "default");
      auto plain = [](std::string_view v) { return XmlSafe(v) && v.find_first_of("<>&\"") == std::string_view::npos; };
      if (id.empty() || !plain(id) || !plain(type)) continue;
      have.push_back(key(ref));
      const std::string name = ref.Is(Name("headerReference")) ? "headerReference" : "footerReference";
      out.xml.push_back("<w:" + name + " w:type=\"" + std::string(type) + "\" r:id=\"" +
                        std::string(id) + "\"/>");
    }
  return out;
}

struct EditorState {
  Document& d;
  Editor& ed;
  Tree& t;
  pkg::NodeId nextNode = 1;
  uint32_t version = 1;
  int64_t nextAnnotation = -1;
  std::unordered_set<uint32_t> usedParaIds;
  bool paraIdsScanned = false;
  uint32_t paraIdSeed = 0x6D2B79F5u;
  std::vector<Entry> undo, redo;
  uint64_t serial = 0, savedSerial = 0;
  std::vector<int> dirty;  // by part: undo steps applied that changed it
  int depth = 0;
  Entry group;
  std::vector<size_t> groupMarks;  // group.steps.size() at each open BeginGroup
  bool typingBroken = true;
  // The open transaction.
  bool inTx = false;
  Step tx;
  size_t sizeAtBegin = 0;
  uint32_t countAtBegin = 0;
  // The paragraphs at Begin hold a list, a field or a section break: adding or removing paragraphs
  // there may change how lists, fields or sections read further on (Change::structure).
  bool structuralAtBegin = false;
  // Split: the range starts that moved to the second half where the split is.
  std::vector<std::pair<uint32_t, std::string>> movedStarts;

  EditorState(Document& doc, Editor& editor) : d(doc), ed(editor), t(doc.tree_) {
    for (const Elem& e : t.elems) nextNode = std::max<pkg::NodeId>(nextNode, e.node + 1);
    for (const Part& p : d.parts_) nextNode = std::max<pkg::NodeId>(nextNode, p.docNode + 1);
    dirty.assign(d.parts_.size(), 0);
    for (const Paragraph& p : d.paras_) version = std::max(version, p.version + 1);
  }

  // ---------- The tree ----------
  Elem& E(ElemId id) { return t.elems[id]; }
  static const Schema& S() { return Schema::Get(); }
  uint32_t NameOf(ElemId id) const { return Schema::Get().Elem(t.elems[id].def).name; }
  TypeId TypeOf(ElemId id) const { return Schema::Get().Elem(t.elems[id].def).type; }
  Role RoleOf(ElemId id) const { return Schema::Get().Type(TypeOf(id)).role; }
  bool Leaf(ElemId id) const {
    Role r = RoleOf(id);
    return r == Role::Text || r == Role::Char || r == Role::Object || r == Role::Marker;
  }
  // Property containers: no text inside, they stay with their element when content moves.
  bool PropsElem(ElemId id) const {
    uint32_t n = NameOf(id);
    return n == kPPr || n == kRPr || n == kSdtPr || n == Name("sdtEndPr") || n == Name("smartTagPr") ||
           n == Name("customXmlPr") || (t.elems[id].flags & kSharedKids);
  }

  ElemId PrevSibling(ElemId id) {
    ElemId parent = E(id).parent;
    if (parent == kNoElem) return kNoElem;
    ElemId prev = kNoElem;
    for (ElemId c = E(parent).first; c != kNoElem && c != id; c = E(c).next) prev = c;
    return prev;
  }
  ElemId LastChild(ElemId parent) {
    ElemId last = kNoElem;
    for (ElemId c = E(parent).first; c != kNoElem; c = E(c).next) last = c;
    return last;
  }
  void Unlink(ElemId id) {
    ElemId prev = PrevSibling(id);
    if (prev == kNoElem) E(E(id).parent).first = E(id).next;
    else E(prev).next = E(id).next;
    E(id).next = kNoElem;
  }
  // Links `child` into `parent` after `after` (kNoElem: first).
  void LinkAfter(ElemId parent, ElemId after, ElemId child) {
    E(child).parent = parent;
    if (after == kNoElem) {
      E(child).next = E(parent).first;
      E(parent).first = child;
    } else {
      E(child).next = E(after).next;
      E(after).next = child;
    }
  }
  void Append(ElemId parent, ElemId child) { LinkAfter(parent, LastChild(parent), child); }

  pkg::NodeId NewNode() { return nextNode++; }
  // A new known element `name` for `parent`, not linked yet. kNoElem when the schema has none there.
  ElemId NewElem(ElemId parent, uint32_t name) {
    ElemDefId def = S().Child(TypeOf(parent), name);
    if (def == kNoDef) return kNoElem;
    ElemId id = t.Add(def, parent);
    E(id).node = NewNode();
    return id;
  }
  // Stores fragment lists that keep kept content in schema order (Kept.h). The transaction's
  // saved paragraphs hold the store's entries for their elements, so undo puts them back.
  void StoreFragments(FragmentChanges changes) {
    for (auto& [node, fragments] : changes) d.store_.SetFragments(node, std::move(fragments));
  }
  // Inserts a new child where the schema orders it among the existing ones; what was kept around
  // the children stays in schema order.
  ElemId AddInOrder(ElemId parent, uint32_t name) {
    ElemId id = NewElem(parent, name);
    if (id == kNoElem) return kNoElem;
    const auto& order = S().Type(TypeOf(parent)).children;
    auto rank = [&](ElemId e) {
      auto it = std::find(order.begin(), order.end(), E(e).def);
      return it == order.end() ? order.size() : static_cast<size_t>(it - order.begin());
    };
    size_t mine = rank(id);
    ElemId after = kNoElem;
    for (ElemId c = E(parent).first; c != kNoElem && rank(c) <= mine; c = E(c).next) after = c;
    LinkAfter(parent, after, id);
    StoreFragments(FragmentsAfterLink(d.store_, t, after, id));
    return id;
  }
  ElemId Child(ElemId parent, uint32_t name) {
    for (ElemId c = E(parent).first; c != kNoElem; c = E(c).next)
      if (NameOf(c) == name) return c;
    return kNoElem;
  }

  // Attributes are copied on write, so saved elements keep pointing at their old values.
  std::optional<std::string_view> Attr(ElemId id, uint32_t name) { return Node(&t, id).Attr(name); }
  bool SetAttr(ElemId id, uint32_t name, std::string_view value) {
    AttrDefId def = S().AttrOf(TypeOf(id), name);
    if (def == kNoDef) return false;
    Elem& e = E(id);
    std::vector<AttrVal> v(t.attrs.begin() + e.attrs, t.attrs.begin() + e.attrs + e.attrCount);
    StrId sv = t.strings.Intern(value);
    auto it = std::find_if(v.begin(), v.end(), [&](const AttrVal& a) { return a.def == def; });
    if (it != v.end()) it->value = sv;
    else v.push_back({def, sv});
    e.attrs = static_cast<uint32_t>(t.attrs.size());
    e.attrCount = static_cast<uint16_t>(v.size());
    t.attrs.insert(t.attrs.end(), v.begin(), v.end());
    return true;
  }
  void RemoveAttr(ElemId id, uint32_t name) {
    AttrDefId def = S().AttrOf(TypeOf(id), name);
    Elem& e = E(id);
    std::vector<AttrVal> v(t.attrs.begin() + e.attrs, t.attrs.begin() + e.attrs + e.attrCount);
    auto it = std::find_if(v.begin(), v.end(), [&](const AttrVal& a) { return a.def == def; });
    if (it == v.end()) return;
    v.erase(it);
    e.attrs = static_cast<uint32_t>(t.attrs.size());
    e.attrCount = static_cast<uint16_t>(v.size());
    t.attrs.insert(t.attrs.end(), v.begin(), v.end());
  }

  // A copy of `src` for `parent` without children; unknown attributes come along, fragments don't
  // (the store's Split moves those). A shared property set stays shared.
  ElemId CloneShallow(ElemId src, ElemId parent) {
    Elem copy = E(src);
    copy.parent = parent;
    copy.next = kNoElem;
    if (!(copy.flags & kSharedKids)) copy.first = kNoElem;
    copy.node = NewNode();
    copy.start = copy.length = 0;
    t.elems.push_back(copy);
    auto id = static_cast<ElemId>(t.elems.size() - 1);
    if (pkg::AttrSetId a = d.store_.NodeAttributes(E(src).node)) d.store_.SetNodeAttributes(E(id).node, a);
    return id;
  }
  // A full copy of a property element, fragments included. `skip` leaves out some children.
  ElemId CloneDeep(ElemId src, ElemId parent, const std::function<bool(ElemId)>& skip = {}, bool fragments = true) {
    ElemId id = CloneShallow(src, parent);
    if (fragments)
      if (auto frags = d.store_.FragmentsCopy(E(src).node); !frags.empty()) d.store_.SetFragments(E(id).node, frags);
    if (E(id).flags & kSharedKids) {
      // A shared set stays shared, unless the copy leaves some of it out (a new cell's w:tcPr
      // without the w:gridSpan of the cell it's like): then the copy has children of its own.
      bool leaves = false;
      for (ElemId c = E(src).first; skip && c != kNoElem && !leaves; c = E(c).next) leaves = skip(c);
      if (!leaves) return id;
      E(id).flags &= static_cast<uint8_t>(~kSharedKids);
      E(id).first = kNoElem;
    }
    for (ElemId c = E(src).first; c != kNoElem; c = E(c).next)
      if (!skip || !skip(c)) Append(id, CloneDeep(c, id, skip, fragments));
    return id;
  }
  // Gives a property container its own children before they change (Tree.h, kSharedKids). The
  // reader flags every container that shares a set but the first, whose children they are: so the
  // first copies them too, and the others keep the originals.
  void Unshare(ElemId container) {
    ElemId shared = E(container).first;
    if (shared == kNoElem) return;
    E(container).flags &= static_cast<uint8_t>(~kSharedKids);
    E(container).first = kNoElem;
    for (ElemId c = shared; c != kNoElem; c = E(c).next) Append(container, CloneDeep(c, container));
  }
  // An element in the list from `first` that repeats one before it exactly: a file's <w:b/><w:b/>,
  // which the schema doesn't allow. (The list is passed: a shared child's parent may own others.)
  bool Repeats(ElemId first, ElemId c) {
    const Elem& a = E(c);
    if (a.first != kNoElem) return false;
    for (ElemId s = first; s != kNoElem && s != c; s = E(s).next) {
      const Elem& b = E(s);
      if (b.def != a.def || b.attrCount != a.attrCount || b.first != kNoElem ||
          d.store_.NodeAttributes(b.node) != d.store_.NodeAttributes(a.node))
        continue;
      bool same = true;
      for (uint16_t i = 0; same && i < a.attrCount; ++i)
        same = t.attrs[a.attrs + i].def == t.attrs[b.attrs + i].def && t.attrs[a.attrs + i].value == t.attrs[b.attrs + i].value;
      if (same) return true;
    }
    return false;
  }
  bool HasRepeats(ElemId e) {
    for (ElemId c = E(e).first; c != kNoElem; c = E(c).next)
      if (Repeats(E(e).first, c) || HasRepeats(c)) return true;
    return false;
  }
  // A copy of a property element for new content (a paragraph mark, a run, a cut run's second
  // half). A set with repeats is copied whole without them, so the copy doesn't add an error the
  // file has once already; the original keeps its own.
  ElemId CopyProps(ElemId src, ElemId parent, const std::function<bool(ElemId)>& skip = {}, bool fragments = true) {
    if (!HasRepeats(src)) return CloneDeep(src, parent, skip, fragments);
    ElemId id = CloneShallow(src, parent);
    E(id).flags &= static_cast<uint8_t>(~kSharedKids);
    E(id).first = kNoElem;
    if (fragments)
      if (auto frags = d.store_.FragmentsCopy(E(src).node); !frags.empty()) d.store_.SetFragments(E(id).node, frags);
    for (ElemId c = E(src).first; c != kNoElem; c = E(c).next)
      if (!Repeats(E(src).first, c) && (!skip || !skip(c))) Append(id, CopyProps(c, id, skip, fragments));
    return id;
  }
  // A run's properties for new text: its w:rPr, or the paragraph mark's, without revision marks.
  void CopyRunProps(ElemId fromRunOrPPr, ElemId toRun, bool dropLinkStyle = false) {
    ElemId rPr = fromRunOrPPr == kNoElem ? kNoElem : Child(fromRunOrPPr, kRPr);
    if (rPr == kNoElem) return;
    auto skip = [&](ElemId c) {
      uint32_t n = NameOf(c);
      if (n == kIns || n == kDel || n == kMoveFrom || n == kMoveTo || n == kRPrChange) return true;
      return dropLinkStyle && n == kRStyle && Node(&t, c).AttrOr(kVal) == "Hyperlink";
    };
    ElemId copy;
    if ((E(rPr).flags & kSharedKids) && !dropLinkStyle && NameOf(fromRunOrPPr) == kR && !HasRepeats(rPr)) {
      // A run's shared set: share it too (shared sets hold plain properties only).
      copy = CloneShallow(rPr, toRun);
    } else {
      // The paragraph mark's w:rPr is a schema type of its own: its children are made again for a run's.
      copy = NewElem(toRun, kRPr);
      if (copy == kNoElem) return;
      for (ElemId c = E(rPr).first; c != kNoElem; c = E(c).next) {
        if (skip(c) || Repeats(E(rPr).first, c)) continue;
        ElemDefId def = S().Child(TypeOf(copy), NameOf(c));
        if (def == kNoDef) continue;
        ElemId k = CopyProps(c, copy);
        E(k).def = def;
        Append(copy, k);
      }
      if (E(copy).first == kNoElem) return;  // nothing to copy; the arena keeps the unused element
    }
    LinkAfter(toRun, kNoElem, copy);
  }

  // ---------- Paragraph text and element ranges ----------
  // Recomputes every element's range in a paragraph from the lengths of its leaves (text elements
  // keep theirs; the rest is the sum of what's inside).
  void Reflow(uint32_t para) {
    ElemId p = d.paras_[para].elem;
    uint32_t off = 0;
    std::vector<std::pair<ElemId, bool>> stack;
    auto pushKids = [&](ElemId parent) {
      size_t mark = stack.size();
      for (ElemId c = E(parent).first; c != kNoElem; c = E(c).next) stack.emplace_back(c, false);
      std::reverse(stack.begin() + static_cast<std::ptrdiff_t>(mark), stack.end());
    };
    pushKids(p);
    while (!stack.empty()) {
      auto [id, closing] = stack.back();
      stack.pop_back();
      Elem& e = E(id);
      if (closing) {
        e.length = off - e.start;
        continue;
      }
      e.start = off;
      if (Leaf(id)) {
        off += e.length;
        continue;
      }
      if (e.flags & kSharedKids) {
        e.length = 0;
        continue;
      }
      stack.emplace_back(id, true);
      pushKids(id);
    }
    E(p).start = para;
    E(p).length = off;
  }

  struct LeafAt {
    ElemId id = kNoElem;
    uint32_t start = 0, length = 0;
  };
  // The paragraph's leaves (text, characters, objects, markers) and zero-length elements, in order.
  std::vector<LeafAt> Leaves(uint32_t para) {
    std::vector<LeafAt> out;
    std::vector<ElemId> stack;
    auto pushKids = [&](ElemId parent) {
      size_t mark = stack.size();
      for (ElemId c = E(parent).first; c != kNoElem; c = E(c).next) stack.push_back(c);
      std::reverse(stack.begin() + static_cast<std::ptrdiff_t>(mark), stack.end());
    };
    pushKids(d.paras_[para].elem);
    while (!stack.empty()) {
      ElemId id = stack.back();
      stack.pop_back();
      if (PropsElem(id)) continue;
      if (Leaf(id) || E(id).first == kNoElem) {
        out.push_back({id, E(id).start, E(id).length});
        continue;
      }
      pushKids(id);
    }
    return out;
  }
  // The element's ancestors below the paragraph, innermost first.
  std::vector<ElemId> Ancestors(ElemId id) {
    std::vector<ElemId> out;
    for (ElemId a = E(id).parent; a != kNoElem && NameOf(a) != kP; a = E(a).parent) out.push_back(a);
    return out;
  }
  ElemId RunOf(ElemId id) {
    for (ElemId a = E(id).parent; a != kNoElem && NameOf(a) != kP; a = E(a).parent)
      if (NameOf(a) == kR) return a;
    return kNoElem;
  }
  // Inside deleted text (w:del, w:moveFrom), where new text must not go.
  ElemId DeletedAncestor(ElemId id) {
    ElemId found = kNoElem;
    for (ElemId a = E(id).parent; a != kNoElem && NameOf(a) != kP; a = E(a).parent)
      if (NameOf(a) == kDel || NameOf(a) == kMoveFrom) found = a;
    return found;
  }
  // Text with spaces at either end needs xml:space="preserve", or Word drops them.
  void KeepSpaces(ElemId text, uint32_t para) {
    const Elem& e = E(text);
    if (e.length == 0 || NameOf(text) == kInstrText) return;
    std::u16string_view s = std::u16string_view(d.paras_[para].text).substr(e.start, e.length);
    bool edges = s.front() == u' ' || s.back() == u' ' || s.find(u"  ") != std::u16string_view::npos;
    if (edges && Attr(text, kXmlSpace).value_or("") != "preserve") SetAttr(text, kXmlSpace, "preserve");
  }

  // ---------- Ids ----------
  std::string NewAnnotationId() {
    if (nextAnnotation < 0) {
      nextAnnotation = 0;
      const Schema& s = S();
      for (const Elem& e : t.elems)
        for (uint32_t i = 0; i < e.attrCount; ++i) {
          const AttrVal& a = t.attrs[e.attrs + i];
          if (s.Attr(a.def).name != kId) continue;
          if (auto v = ParseInt(t.strings.Get(a.value))) nextAnnotation = std::max(nextAnnotation, *v + 1);
        }
    }
    return std::to_string(nextAnnotation++);
  }
  std::string NewParaId() {
    if (!paraIdsScanned) {
      paraIdsScanned = true;
      for (ElemId id = 0; id < t.elems.size(); ++id)
        for (uint32_t name : {kParaId, kTextId})
          if (auto v = Node(&t, id).Attr(name))
            if (auto h = ParseHex(*v)) usedParaIds.insert(*h);
    }
    uint32_t v;
    do {
      paraIdSeed ^= paraIdSeed << 13;
      paraIdSeed ^= paraIdSeed >> 17;
      paraIdSeed ^= paraIdSeed << 5;
      v = paraIdSeed & 0x7FFFFFFFu;  // Word's paragraph ids stay below 0x80000000
    } while (v == 0 || !usedParaIds.insert(v).second);
    char buf[9];
    std::snprintf(buf, sizeof buf, "%08X", v);
    return buf;
  }

  // ---------- Checks ----------
  bool Editable(uint32_t para, std::u16string& why) {
    if (para >= d.paras_.size()) {
      why = u"That place isn't in the document.";
      return false;
    }
    const Paragraph& p = d.paras_[para];
    const Story& st = d.stories_[p.story];
    if (st.part == kNone || st.part >= d.parts_.size() || !d.parts_[st.part].Editable() || !d.parts_[st.part].writable) {
      why = u"This part of the document uses features Featherweight doesn't understand, so it can't be edited.";
      return false;
    }
    // Text boxes are shown from their drawing, which is written back as it was (plan §6.3, D4).
    if (st.drawing != kNone || (E(p.elem).flags & kDisplayOnly)) {
      why = u"Text in a text box can't be edited yet.";
      return false;
    }
    return true;
  }
  // Text with spaces at either end (or two in a row) needs xml:space="preserve", or Word drops them.
  void FixSpaces(uint32_t para) {
    for (ElemId id : AllElems(para))
      if (RoleOf(id) == Role::Text && NameOf(id) == kT && !(E(id).flags & kDisplayOnly)) KeepSpaces(id, para);
  }

  // ---------- Transactions ----------
  // Paragraph `p`'s w:p, its subtree, its neighbors and its parent.
  void Collect(uint32_t first, uint32_t count, std::vector<ElemId>& ids) {
    std::unordered_set<ElemId> seen;
    auto add = [&](ElemId id) {
      if (id != kNoElem && seen.insert(id).second) ids.push_back(id);
    };
    for (uint32_t p = first; p < first + count && p < d.paras_.size(); ++p) {
      ElemId w = d.paras_[p].elem;
      add(E(w).parent);
      add(PrevSibling(w));
      add(E(w).next);
      // Range markers between it and the next paragraph, however many: joining moves them in.
      for (ElemId m = E(w).next; m != kNoElem && RangeMarker(NameOf(m)); m = E(m).next) add(m);
      std::vector<ElemId> stack{w};
      while (!stack.empty()) {
        ElemId id = stack.back();
        stack.pop_back();
        add(id);
        if (E(id).flags & kSharedKids) continue;
        for (ElemId c = E(id).first; c != kNoElem; c = E(c).next) stack.push_back(c);
      }
    }
    CollectRoots(ids, seen);
  }
  void Capture(uint32_t first, uint32_t count, Side& side) {
    side = {};
    side.paras.assign(d.paras_.begin() + first, d.paras_.begin() + first + count);
    std::vector<ElemId> ids;
    Collect(first, count, ids);
    side.elems.reserve(ids.size());
    for (ElemId id : ids) {
      side.elems.push_back({id, E(id)});
      if (pkg::NodeId n = E(id).node)
        side.nodes.push_back({n, d.store_.FragmentsCopy(n), d.store_.NodeAttributes(n)});
    }
  }
  // Puts a side back in place of the `current` paragraphs that now follow `first`.
  void Restore(const Side& side, uint32_t first, uint32_t current) {
    for (const ElemSave& s : side.elems) t.elems[s.id] = s.e;
    for (const NodeSave& n : side.nodes) {
      d.store_.SetFragments(n.node, n.fragments);
      d.store_.SetNodeAttributes(n.node, n.attrs);
    }
    if (side.paras.size() == current) {
      std::copy(side.paras.begin(), side.paras.end(), d.paras_.begin() + first);
    } else {
      d.paras_.erase(d.paras_.begin() + first, d.paras_.begin() + first + current);
      d.paras_.insert(d.paras_.begin() + first, side.paras.begin(), side.paras.end());
    }
    for (uint32_t p = first; p < first + side.paras.size(); ++p) d.paras_[p].version = ++version;
    if (side.paras.size() != current) Renumber(first);
  }
  void Renumber(uint32_t from) {
    for (uint32_t p = from; p < d.paras_.size(); ++p) E(d.paras_[p].elem).start = p;
  }
  void InsertPara(uint32_t at, ElemId elem, uint32_t story, std::u16string text) {
    Paragraph p;
    p.elem = elem;
    p.story = story;
    p.text = std::move(text);
    p.version = ++version;
    d.paras_.insert(d.paras_.begin() + at, std::move(p));
    Renumber(at);
  }
  void ErasePara(uint32_t at) {
    d.paras_.erase(d.paras_.begin() + at);
    Renumber(at);
  }

  // `extra`: elements besides the paragraphs' that the transaction changes (roots).
  bool Begin(uint32_t first, uint32_t count, std::vector<ElemId> extra = {}) {
    if (inTx) return false;
    inTx = true;
    roots = std::move(extra);
    tx = {};
    tx.story = d.paras_[first].story;
    tx.first = first;
    sizeAtBegin = d.paras_.size();
    countAtBegin = count;
    structuralAtBegin = false;
    for (uint32_t p = first; p < first + count && p < d.paras_.size() && !structuralAtBegin; ++p) {
      const std::u16string& s = d.paras_[p].text;
      ElemId pPr = Child(d.paras_[p].elem, kPPr);
      structuralAtBegin = !d.ResolvedPara(p).numId.empty() || s.find(u'\x13') != std::u16string::npos ||
                          s.find(u'\x15') != std::u16string::npos || (pPr != kNoElem && Child(pPr, kSectPr) != kNoElem);
      for (ElemId e : AllElems(p))
        if (NameOf(e) == kFldSimple) structuralAtBegin = true;
    }
    Capture(first, count, tx.before);
    return true;
  }
  uint32_t CurrentCount() const {
    return static_cast<uint32_t>(static_cast<int64_t>(countAtBegin) + static_cast<int64_t>(d.paras_.size()) -
                                 static_cast<int64_t>(sizeAtBegin));
  }
  Result Abort(std::u16string why) {
    Restore(tx.before, tx.first, CurrentCount());
    inTx = false;
    Result r;
    r.ok = false;
    r.message = std::move(why);
    return r;
  }
  // Finishes the open transaction: records it, refreshes what depends on the text, tells listeners.
  void Commit(bool structure, Pos caretBefore, Pos caretAfter, Kind kind, std::u16string name) {
    uint32_t count = CurrentCount();
    for (uint32_t p = tx.first; p < tx.first + count; ++p) {
      if (d.paras_[p].story == tx.story) FixSpaces(p);
      d.paras_[p].version = ++version;
    }
    Capture(tx.first, count, tx.after);
    tx.structure = structure || (count != countAtBegin && structuralAtBegin);
    inTx = false;
    Refresh(tx, false);
    Record(std::move(tx), caretBefore, caretAfter, kind, std::move(name));
  }

  void Record(Step step, Pos caretBefore, Pos caretAfter, Kind kind, std::u16string name) {
    redo.clear();
    if (depth > 0) {
      if (group.steps.empty()) group.caretBefore = caretBefore;
      group.caretAfter = caretAfter;
      group.steps.push_back(std::move(step));
      if (kind != Kind::Other) group.kind = kind;
      return;
    }
    // Typing and deleting continue the step before while they go on where it left off (Backspace
    // ends where the next one starts; Delete leaves the caret where the next one ends).
    if (kind != Kind::Other && !typingBroken && !undo.empty()) {
      Entry& last = undo.back();
      Step& prev = last.steps.back();
      bool adjacent = last.caretAfter == caretBefore || (kind == Kind::Deleting && last.caretAfter == caretAfter);
      if (last.kind == kind && last.serial != savedSerial && adjacent && prev.first == step.first &&
          prev.after.paras.size() == step.before.paras.size() && !step.structure) {
        prev.after = std::move(step.after);
        last.caretAfter = caretAfter;
        return;
      }
    }
    Entry e;
    e.name = std::move(name);
    e.steps.push_back(std::move(step));
    e.caretBefore = caretBefore;
    e.caretAfter = caretAfter;
    e.kind = kind;
    e.serial = ++serial;
    Count(e, 1);
    undo.push_back(std::move(e));
    typingBroken = kind == Kind::Other;
  }
  // The parts an undo step changed, counted in or out of `dirty`.
  void Count(const Entry& e, int delta) {
    if (dirty.size() < d.parts_.size()) dirty.resize(d.parts_.size(), 0);  // a direct change added a part
    std::vector<uint32_t> parts;
    auto add = [&](uint32_t part) {
      if (part < dirty.size() && std::find(parts.begin(), parts.end(), part) == parts.end()) parts.push_back(part);
    };
    for (const Step& st : e.steps) {
      if (st.custom) {
        for (uint32_t part : st.custom->parts) add(part);
        continue;
      }
      add(d.stories_[st.story].part);
    }
    for (uint32_t part : parts) dirty[part] += delta;
  }

  // After a direct change, done or undone: the model's indexes are built again, every cache goes,
  // and every paragraph may look different.
  void RefreshAll(const CustomStep& step) {
    // A step that says which paragraphs it changed leaves the others as they were: they keep their
    // versions, so what was measured for them stays (a big table's cells, after formatting one).
    // Past `renumberFrom`, only the list paragraphs may read differently (their numbers).
    const bool ranged = step.first != kNone && step.count > 0 && step.first + step.count <= d.paras_.size();
    const uint32_t changedEnd = std::min(step.renumberFrom, step.first + step.count);
    auto changed = [&](uint32_t i) { return i >= step.first && i < changedEnd; };
    std::unordered_map<ElemId, uint32_t> kept;
    if (ranged)
      for (uint32_t i = 0; i < d.paras_.size(); ++i)
        if (!changed(i)) kept.emplace(d.paras_[i].elem, d.paras_[i].version);
    d.ResetCaches();
    d.BuildIndexes();
    for (uint32_t i = 0; i < d.paras_.size(); ++i) {
      Paragraph& p = d.paras_[i];
      const bool renumbered = i >= changedEnd && i < step.first + step.count && d.InList(i);
      const auto it = ranged && !changed(i) && !renumbered ? kept.find(p.elem) : kept.end();
      p.version = it != kept.end() ? it->second : ++version;
    }
    Change ch;
    if (step.first != kNone && step.count > 0 && step.first + step.count <= d.paras_.size()) {
      ch.story = d.paras_[step.first].story;
      ch.first = step.first;
      ch.removed = ch.added = step.count;
      ch.structure = step.structure;
    } else {
      ch.story = d.main_;
      ch.first = 0;
      ch.removed = ch.added = static_cast<uint32_t>(d.paras_.size());
      ch.structure = true;
    }
    pending.push_back(ch);
  }

  // Brings the model's indexes and caches up to date after a step was done (or undone) and tells
  // the listeners.
  void Refresh(const Step& step, bool undoing) {
    const Side& was = undoing ? step.after : step.before;
    const Side& now = undoing ? step.before : step.after;
    auto removed = static_cast<uint32_t>(was.paras.size());
    auto added = static_cast<uint32_t>(now.paras.size());
    int32_t shift = static_cast<int32_t>(added) - static_cast<int32_t>(removed);
    uint32_t first = step.first;

    std::unordered_set<ElemId> wasIds, nowIds;
    for (const ElemSave& s : was.elems) wasIds.insert(s.id);
    for (const ElemSave& s : now.elems) nowIds.insert(s.id);

    Document::Caches& c = *d.caches_;
    auto resize = [&](std::vector<uint32_t>& v) {
      if (v.size() < first) return;
      auto end = std::min<size_t>(v.size(), first + removed);
      v.erase(v.begin() + first, v.begin() + static_cast<std::ptrdiff_t>(end));
      v.insert(v.begin() + first, added, kNone);
      if (v.size() < d.paras_.size()) v.resize(d.paras_.size(), kNone);
    };
    resize(c.para);
    resize(c.mark);
    for (ElemId id : wasIds)
      if (NameOf(id) == kR) c.run.erase(id);
    for (ElemId id : nowIds)
      if (NameOf(id) == kR) c.run.erase(id);

    bool fields = false;
    auto countFields = [](const std::vector<Paragraph>& ps) {
      size_t n = 0;
      for (const auto& p : ps) n += static_cast<size_t>(std::count_if(p.text.begin(), p.text.end(), IsFieldChar));
      return n;
    };
    if (countFields(was.paras) != countFields(now.paras)) fields = true;
    if (step.structure || shift != 0) {
      c.labels.clear();
      c.fieldStack.clear();
      for (auto& p : c.fieldPass) p = {};
      for (auto& p : c.labelPass) p = {};
    } else if (fields) {
      c.fieldStack.clear();
      for (auto& p : c.fieldPass) p = {};
    }

    // Positions of drawings and range ends: found again for the elements the step touched (kNone
    // for the ones it removed), moved along for the ones after it.
    auto fix = [&](Pos& pos, ElemId node) {
      if (nowIds.count(node)) {
        pos = d.PosOf(Node(&t, node));
      } else if (wasIds.count(node)) {
        pos = Pos{kNone, 0};
      } else if (pos.para == kNone) {
        return;
      } else if (pos.para >= first + removed) {
        pos.para = static_cast<uint32_t>(static_cast<int64_t>(pos.para) + shift);
      } else if (pos.para >= first) {
        pos = d.PosOf(Node(&t, node));
      }
    };
    for (Drawing& dr : d.drawings_) fix(dr.pos, dr.node.Id());
    for (PairInfo& pi : d.pairs_) {
      fix(pi.openPos, pi.open.Id());
      fix(pi.closePos, pi.close.Id());
    }
    // Section breaks: where they are now, or the sections again when one went or came back (a
    // deletion takes the breaks in the marks it deletes, and undoing it brings them back).
    bool breaks = false;
    for (const Section& s : d.sections_) breaks = breaks || (s.endPara != kNone && s.endPara >= first && s.endPara < first + removed);
    for (uint32_t p = first; p < first + added && !breaks; ++p) breaks = static_cast<bool>(d.ParaNode(p).Path({kPPr, kSectPr}));
    if (breaks) {
      d.BuildSections();
    } else {
      for (Section& s : d.sections_) {
        if (s.endPara == kNone) continue;
        if (s.endPara >= first + removed) s.endPara = static_cast<uint32_t>(static_cast<int64_t>(s.endPara) + shift);
      }
    }

    Change ch;
    ch.story = step.story;
    ch.first = first;
    ch.removed = removed;
    ch.added = added;
    ch.structure = step.structure || fields;
    pending.push_back(ch);
  }
  std::vector<Change> pending;
  void Notify(const std::vector<Editor::Listener>& listeners) {
    auto changes = std::move(pending);
    pending.clear();
    for (const Change& ch : changes)
      for (const auto& l : listeners) l(ch);
  }

  Result UndoRedo(bool undoing) {
    auto& from = undoing ? undo : redo;
    auto& to = undoing ? redo : undo;
    Result r;
    if (from.empty()) {
      r.ok = false;
      r.message = undoing ? u"There's nothing to undo." : u"There's nothing to redo.";
      return r;
    }
    Entry e = std::move(from.back());
    from.pop_back();
    if (undoing) {
      for (auto it = e.steps.rbegin(); it != e.steps.rend(); ++it) UndoStep(*it);
    } else {
      for (const Step& s : e.steps) {
        if (s.custom) {
          s.custom->Redo();
          RefreshAll(*s.custom);
          continue;
        }
        Restore(s.after, s.first, static_cast<uint32_t>(s.before.paras.size()));
        Refresh(s, false);
      }
    }
    Count(e, undoing ? -1 : 1);
    r.caret = undoing ? e.caretBefore : e.caretAfter;
    to.push_back(std::move(e));
    typingBroken = true;
    return r;
  }

  void UndoStep(Step& s) {
    if (s.custom) {
      s.custom->Undo();
      RefreshAll(*s.custom);
      return;
    }
    Restore(s.before, s.first, static_cast<uint32_t>(s.after.paras.size()));
    Refresh(s, true);
  }

  // ---------- Mutations (inside a transaction; they return false with `why` and may leave a
  // half-done change, which the transaction's Abort undoes) ----------

  // Where new text at `o` goes: extend a w:t, or a new w:t after `after` in `run`.
  struct Spot {
    ElemId run = kNoElem;
    ElemId text = kNoElem;   // a w:t that ends at o (or starts at it when o is 0), to extend
    ElemId after = kNoElem;  // otherwise: the run child the new w:t follows (kNoElem: after w:rPr)
  };
  bool FindSpot(uint32_t para, uint32_t o, Spot& spot, std::u16string& why) {
    std::vector<LeafAt> leaves = Leaves(para);
    ElemId p = d.paras_[para].elem;
    // The leaf holding the character before o.
    const LeafAt* before = nullptr;
    for (const LeafAt& l : leaves)
      if (l.length > 0 && l.start < o && o <= l.start + l.length) before = &l;
    auto plainRun = [&](ElemId leaf) { return RunOf(leaf) != kNoElem && DeletedAncestor(leaf) == kNoElem; };
    if (before) {
      ElemId leaf = before->id;
      if (E(leaf).flags & kDisplayOnly) {
        // After a drawing kept as markup (mc:AlternateContent): a new run after its own run.
        ElemId top = leaf;
        while (E(top).parent != kNoElem && (E(E(top).parent).flags & kDisplayOnly)) top = E(top).parent;
        ElemId run = E(top).parent;
        bool last = true;
        for (ElemId c = E(top).next; c != kNoElem; c = E(c).next)
          if (!(E(c).flags & kDisplayOnly)) last = false;
        bool fits = run != kNoElem && NameOf(run) == kR && last && E(run).start + E(run).length == o;
        ElemId nr = fits ? NewElem(E(run).parent, kR) : kNoElem;
        if (nr == kNoElem) {
          why = u"Text can't be typed here, next to this drawing.";
          return false;
        }
        LinkAfter(E(run).parent, run, nr);
        CopyRunProps(run, nr);
        spot = {nr, kNoElem, LastChild(nr)};
        return true;
      }
      ElemId run = RunOf(leaf);
      // Text typed right after a link or a simple field isn't part of it (as in Word).
      ElemId endsHere = kNoElem;
      for (ElemId a : Ancestors(leaf)) {
        bool boundary = NameOf(a) == kHyperlink || NameOf(a) == kFldSimple || (NameOf(a) == kSdt && Locked(a, false));
        if (boundary && E(a).start + E(a).length == o) endsHere = a;
      }
      if (endsHere == kNoElem && LockedSdt(leaf) != kNoElem) {
        why = u"The text of this content control is locked.";
        return false;
      }
      if (endsHere != kNoElem && run != kNoElem) {
        ElemId nr = NewElem(E(endsHere).parent, kR);
        if (nr != kNoElem) {
          LinkAfter(E(endsHere).parent, endsHere, nr);
          CopyRunProps(run, nr, true);
          spot = {nr, kNoElem, LastChild(nr)};
          return true;
        }
      }
      bool textual = RoleOf(leaf) == Role::Text && NameOf(leaf) != Name("delText") && NameOf(leaf) != Name("delInstrText");
      if (textual && plainRun(leaf)) {
        spot = {run, leaf, kNoElem};
        return true;
      }
      if (run != kNoElem && plainRun(leaf)) {
        spot = {run, kNoElem, leaf};
        return true;
      }
      // Inside deleted text, or a character outside a run: a new run after the outermost container,
      // cut at o first when o is inside it.
      ElemId top = DeletedAncestor(leaf);
      if (top == kNoElem) top = run != kNoElem ? run : leaf;
      if (E(top).start < o && o < E(top).start + E(top).length) {
        if (!Cut(top, o, why)) return false;
        Reflow(para);
      }
      ElemId nr = NewElem(E(top).parent, kR);
      if (nr == kNoElem) {
        why = u"Text can't go here.";
        return false;
      }
      LinkAfter(E(top).parent, top, nr);
      CopyRunProps(run, nr);
      spot = {nr, kNoElem, LastChild(nr)};
      return true;
    }
    // At the start: the first text if it starts the paragraph, else a new run first.
    for (const LeafAt& l : leaves) {
      if (l.length == 0) continue;
      if (l.start == 0 && RoleOf(l.id) == Role::Text && NameOf(l.id) == kT && plainRun(l.id) &&
          !(E(l.id).flags & kDisplayOnly) && LockedSdt(l.id) == kNoElem) {
        spot = {RunOf(l.id), l.id, kNoElem};
        return true;
      }
      break;
    }
    ElemId firstRun = kNoElem;
    for (const LeafAt& l : leaves)
      if (l.length > 0 && (firstRun = RunOf(l.id)) != kNoElem) break;
    ElemId nr = NewElem(p, kR);
    // After the paragraph properties and any markers at the very start.
    ElemId after = kNoElem;
    for (ElemId c = E(p).first; c != kNoElem; c = E(c).next) {
      if (PropsElem(c) || (E(c).length == 0 && E(c).first == kNoElem && RangeMarker(NameOf(c)) && !StartMarker(NameOf(c))))
        after = c;
      else
        break;
    }
    LinkAfter(p, after, nr);
    if (firstRun != kNoElem) CopyRunProps(firstRun, nr);
    else CopyRunProps(Child(p, kPPr), nr);
    spot = {nr, kNoElem, LastChild(nr)};
    return true;
  }

  // Inserts text without paragraph breaks; tabs and line, page and column breaks (U+000B, U+000C,
  // U+000E) become w:tab and w:br.
  bool InsertPlain(uint32_t para, uint32_t o, std::u16string_view text, std::u16string& why) {
    if (text.empty()) return true;
    Spot spot;
    if (!FindSpot(para, o, spot, why)) return false;
    std::u16string& s = d.paras_[para].text;
    ElemId run = spot.run;
    ElemId cur = spot.text;   // the w:t being extended
    ElemId after = spot.after;
    bool atStart = cur != kNoElem && E(cur).start == o && o < E(cur).start + E(cur).length;
    // A tab or break in the middle of a w:t splits it; the text after the last one joins the tail.
    auto special = [](char16_t c) { return c == u'\t' || c == 0x0B || c == 0x0C || c == 0x0E; };
    auto hasSpecial = std::any_of(text.begin(), text.end(), special);
    ElemId tail = kNoElem;
    if (cur != kNoElem && hasSpecial && o > E(cur).start && o < E(cur).start + E(cur).length) {
      tail = CloneShallow(cur, run);
      E(tail).length = E(cur).start + E(cur).length - o;
      E(cur).length = o - E(cur).start;
      LinkAfter(run, cur, tail);
    } else if (cur != kNoElem && hasSpecial && atStart) {
      after = PrevSibling(cur);  // w:rPr, or kNoElem: first in the run
      tail = cur;
      cur = kNoElem;
    }
    uint32_t at = o;
    size_t i = 0;
    while (i < text.size()) {
      char16_t ch = text[i];
      if (special(ch)) {
        ElemId item = NewElem(run, ch == u'\t' ? kTab : kBr);
        E(item).length = 1;
        if (ch == 0x0C || ch == 0x0E) SetAttr(item, kType, ch == 0x0C ? "page" : "column");
        ElemId prev = cur != kNoElem ? cur : (after != kNoElem ? after : Child(run, kRPr));
        LinkAfter(run, prev, item);
        s.insert(s.begin() + at, ch);
        ++at;
        ++i;
        after = item;
        cur = kNoElem;
        continue;
      }
      size_t j = i;
      while (j < text.size() && !special(text[j])) ++j;
      if (cur == kNoElem && j == text.size() && tail != kNoElem) {
        cur = tail;  // it starts at `at`
      } else if (cur == kNoElem) {
        cur = NewElem(run, kT);
        ElemId prev = after != kNoElem ? after : Child(run, kRPr);
        LinkAfter(run, prev, cur);
        E(cur).start = at;
      }
      s.insert(at, text.substr(i, j - i));
      E(cur).length += static_cast<uint32_t>(j - i);
      at += static_cast<uint32_t>(j - i);
      i = j;
      ElemId done = cur;
      Reflow(para);
      KeepSpaces(done, para);
      after = done;
      cur = i < text.size() ? kNoElem : done;
    }
    Reflow(para);
    return true;
  }

  // Inserts an object (a w:drawing) at `o`, holding `xml` as its content, in the run the text
  // before it is in (or a new one, as typing would make). A w:t it lands in is split.
  ElemId InsertObject(uint32_t para, uint32_t o, uint32_t name, std::string xml, std::u16string& why) {
    Spot spot;
    if (!FindSpot(para, o, spot, why)) return kNoElem;
    ElemId run = spot.run;
    ElemId prev = spot.after != kNoElem ? spot.after : Child(run, kRPr);
    if (ElemId cur = spot.text; cur != kNoElem) {
      uint32_t start = E(cur).start, end = start + E(cur).length;
      if (start < o && o < end) {
        ElemId tail = CloneShallow(cur, run);
        E(tail).length = end - o;
        E(cur).length = o - start;
        LinkAfter(run, cur, tail);
        prev = cur;
      } else if (start == o && o < end) {
        prev = PrevSibling(cur);  // before the text: after w:rPr, or first
      } else {
        prev = cur;
      }
    }
    ElemId item = NewElem(run, name);
    if (item == kNoElem) {
      why = u"A picture can't go here.";
      return kNoElem;
    }
    E(item).length = 1;
    LinkAfter(run, prev, item);
    d.paras_[para].text.insert(d.paras_[para].text.begin() + o, u'\xFFFC');
    d.store_.SetFragments(E(item).node, {pkg::Fragment{pkg::Slot::Inside, {}, std::move(xml)}});
    Reflow(para);
    return item;
  }

  // The handle of a drawing just inserted.
  void AddDrawing(Drawing info, ElemId item, uint32_t para) {
    info.node = Node(&t, item);
    info.part = d.stories_[d.paras_[para].story].part;
    info.pos = Pos{kNone, 0};
    info.displayOnly = false;
    info.keptXml.clear();
    info.textBoxes.clear();
    d.drawings_.push_back(std::move(info));
  }

  // Deletes [a, b) inside one paragraph. Range markers stay (collapsed at a), so bookmarks and
  // comment ranges keep both ends; whole fields go only when a check before allowed it.
  bool DeleteIn(uint32_t para, uint32_t a, uint32_t b, std::u16string& why) {
    if (a >= b) return true;
    std::vector<LeafAt> leaves = Leaves(para);
    // Simple fields and inline content controls: editing inside them is fine, a range across an
    // edge is refused, and one that covers them (more than a content control's own text) removes them.
    std::vector<ElemId> remove, emptied;
    for (ElemId id : AllElems(para)) {
      uint32_t n = NameOf(id);
      if ((n != kFldSimple && n != kSdt) || E(id).length == 0) continue;
      uint32_t s = E(id).start, e = s + E(id).length;
      bool covers = a <= s && e <= b && !(n == kSdt && a == s && b == e);
      bool inside = !covers && s <= a && b <= e;
      if (inside) {
        if (n == kSdt && Locked(id, false)) {
          why = u"The text of this content control is locked.";
          return false;
        }
      } else if (covers) {
        if (n == kSdt && Locked(id, true)) {
          why = u"This content control is locked, so it can't be deleted.";
          return false;
        }
        for (ElemId m : AllElems(para))
          if (Within(m, id) && RangeMarker(NameOf(m))) {
            why = u"Part of this holds a bookmark or comment range, so it can't be deleted as a whole.";
            return false;
          }
        remove.push_back(id);
      } else if (s < b && a < e) {
        why = n == kFldSimple ? u"Select the whole field to delete it." : u"Select the whole content control to delete it.";
        return false;
      }
    }
    for (const LeafAt& l : leaves) {
      if (l.length == 0) continue;
      uint32_t s = std::max(a, l.start), e = std::min(b, l.start + l.length);
      if (s >= e) continue;
      if (E(l.id).flags & kDisplayOnly) {
        why = u"Deleting this drawing isn't possible yet.";
        return false;
      }
      Role r = RoleOf(l.id);
      if (r == Role::Text) {
        E(l.id).length -= e - s;
        if (E(l.id).length == 0) emptied.push_back(l.id);
        continue;
      }
      if (Undeletable(NameOf(l.id))) {
        uint32_t n = NameOf(l.id);
        why = n == Name("commentReference") || n == Name("annotationRef")
                  ? u"Deleting a comment isn't possible yet. Leave its mark in place."
                  : u"Deleting a footnote or endnote isn't possible yet. Leave its number in place.";
        return false;
      }
      remove.push_back(l.id);
    }
    std::u16string& s = d.paras_[para].text;
    s.erase(a, b - a);
    ElemId root = d.paras_[para].elem;
    for (ElemId id : remove)
      if (Linked(id, root) && id != root) {
        emptied.push_back(E(id).parent);
        RemoveElem(id);
      }
    Prune(std::move(emptied), root);
    Reflow(para);
    return true;
  }
  // A content control's lock (w:sdtPr/w:lock): on the control itself (`control`) or on its text.
  bool Locked(ElemId sdt, bool control) {
    ElemId pr = Child(sdt, kSdtPr);
    ElemId lock = pr == kNoElem ? kNoElem : Child(pr, kLock);
    if (lock == kNoElem) return false;
    std::string_view v = Node(&t, lock).AttrOr(kVal);
    return v == "sdtContentLocked" || v == (control ? "sdtLocked" : "contentLocked");
  }
  // A content control that holds one paragraph: a date, a list (drop-down or combo box), plain
  // text (multiLine allows line breaks, not paragraphs) or a picture. Word can't open one with a
  // second paragraph in it.
  bool OneParagraph(ElemId sdt) {
    static const uint32_t kinds[] = {Name("date"), Name("dropDownList"), Name("comboBox"), Name("text"), Name("picture")};
    ElemId pr = Child(sdt, kSdtPr);
    for (ElemId c = pr == kNoElem ? kNoElem : E(pr).first; c != kNoElem; c = E(c).next)
      if (std::find(std::begin(kinds), std::end(kinds), NameOf(c)) != std::end(kinds)) return true;
    return false;
  }
  // Whether `para` is in such a control: around it, its cell or its row, or (with `o`) inline
  // around that place in it (finding 22).
  bool InOneParagraphControl(uint32_t para, std::optional<uint32_t> o) {
    for (ElemId a = E(d.paras_[para].elem).parent; a != kNoElem; a = E(a).parent)
      if (NameOf(a) == kSdt && OneParagraph(a)) return true;
    if (o)
      for (ElemId id : AllElems(para))
        if (NameOf(id) == kSdt && E(id).start < *o && *o < E(id).start + E(id).length && OneParagraph(id)) return true;
    return false;
  }
  ElemId LockedSdt(ElemId leaf) {
    for (ElemId a : Ancestors(leaf))
      if (NameOf(a) == kSdt && Locked(a, false)) return a;
    return kNoElem;
  }
  bool Within(ElemId id, ElemId ancestor) {
    for (ElemId a = E(id).parent; a != kNoElem; a = E(a).parent)
      if (a == ancestor) return true;
    return false;
  }
  bool Linked(ElemId id, ElemId root) { return id == root || Within(id, root); }
  std::vector<ElemId> AllElems(uint32_t para) {
    std::vector<ElemId> out, stack{d.paras_[para].elem};
    while (!stack.empty()) {
      ElemId id = stack.back();
      stack.pop_back();
      if (id != d.paras_[para].elem) out.push_back(id);
      if (PropsElem(id) || Leaf(id)) continue;
      for (ElemId c = E(id).first; c != kNoElem; c = E(c).next) stack.push_back(c);
    }
    return out;
  }
  // Unlinks an element. Kept markup inside it goes with it, except the ends of pairs, which move to
  // a neighbor so the other end keeps its partner.
  void RemoveElem(ElemId id) {
    ElemId prev = PrevSibling(id), next = E(id).next, parent = E(id).parent;
    pkg::NodeId survivor = prev != kNoElem ? E(prev).node : next != kNoElem ? E(next).node : E(parent).node;
    pkg::Slot slot = prev != kNoElem ? pkg::Slot::After : next != kNoElem ? pkg::Slot::Before : pkg::Slot::End;
    std::vector<ElemId> stack{id};
    while (!stack.empty()) {
      ElemId e = stack.back();
      stack.pop_back();
      if (E(e).node) d.store_.Delete(E(e).node, survivor, slot);
      if (E(e).flags & kSharedKids) continue;
      for (ElemId c = E(e).first; c != kNoElem; c = E(c).next) stack.push_back(c);
    }
    Unlink(id);
  }
  // Whether the element or anything inside it owns kept markup.
  bool HasFragments(ElemId id) {
    std::vector<ElemId> stack{id};
    while (!stack.empty()) {
      ElemId e = stack.back();
      stack.pop_back();
      if (E(e).node && !d.store_.FragmentsCopy(E(e).node).empty()) return true;
      if (E(e).flags & kSharedKids) continue;
      for (ElemId c = E(e).first; c != kNoElem; c = E(c).next) stack.push_back(c);
    }
    return false;
  }
  // Removes what a delete emptied: text elements, and runs and inline containers left holding only
  // their properties, going up from `candidates`. Never ones with kept markup, nor untouched ones.
  void Prune(std::vector<ElemId> candidates, ElemId root) {
    while (!candidates.empty()) {
      ElemId c = candidates.back();
      candidates.pop_back();
      if (c == kNoElem || c == root || !Within(c, root) || (E(c).flags & kDisplayOnly)) continue;
      uint32_t n = NameOf(c);
      bool emptyText = RoleOf(c) == Role::Text && E(c).length == 0;
      bool onlyProps = true;
      for (ElemId k = E(c).first; k != kNoElem; k = E(k).next)
        if (!PropsElem(k)) onlyProps = false;
      bool emptyBox = (n == kR || n == kHyperlink || n == kIns || n == kDel || n == kMoveFrom || n == kMoveTo ||
                       n == kSmartTag || n == kDir || n == kBdo) && onlyProps;
      if ((emptyText || emptyBox) && !HasFragments(c)) {
        candidates.push_back(E(c).parent);
        Unlink(c);
      }
    }
  }

  // Moves what follows `o` from `from` into `to` (both containers of one paragraph), cutting
  // containers that straddle it. False with `why` when one can't be cut.
  // Whether a container at a split point holds a range start, or the end of a range whose start
  // went to the second half.
  bool MovesWithMarkers(ElemId c) {
    for (ElemId k = E(c).first; k != kNoElem; k = E(k).next) {
      if (PropsElem(k)) continue;
      const uint32_t n = NameOf(k);
      if (StartMarker(n)) return true;
      if (const uint32_t start = StartOfEnd(n); start != 0) {
        const std::string_view id = Node(&t, k).AttrOr(kId);
        for (const auto& [kind, movedId] : movedStarts)
          if (kind == start && movedId == id) return true;
      }
      if (E(k).first != kNoElem && MovesWithMarkers(k)) return true;
    }
    return false;
  }
  // The range starts in a container that moves, so that their ends after it move too.
  void RecordStarts(ElemId c) {
    for (ElemId k = E(c).first; k != kNoElem; k = E(k).next) {
      if (PropsElem(k)) continue;
      if (StartMarker(NameOf(k))) movedStarts.emplace_back(NameOf(k), std::string(Node(&t, k).AttrOr(kId)));
      if (E(k).first != kNoElem) RecordStarts(k);
    }
  }
  bool SplitChildren(ElemId from, ElemId to, uint32_t o, std::u16string& why) {
    ElemId last = LastChild(to);
    for (ElemId c = E(from).first; c != kNoElem;) {
      ElemId next = E(c).next;
      if (PropsElem(c)) {
        c = next;
        continue;
      }
      uint32_t s = E(c).start, l = E(c).length;
      bool move;
      if (l == 0) move = s > o || (s == o && StartMarker(NameOf(c)) && !(NameOf(c) == kProofErr));
      else move = s >= o;
      // A container with no text at the split point (a link around a bookmark's end) goes with
      // the range markers in it: with a start, or with an end whose start went (finding 19).
      if (l == 0 && s == o && !move && E(c).first != kNoElem && MovesWithMarkers(c)) {
        move = true;
        RecordStarts(c);
      }
      if (NameOf(c) == kProofErr && l == 0 && s == o) {
        std::string_view type = Node(&t, c).AttrOr(kType);
        move = type == "spellStart" || type == "gramStart";
      }
      // A range that starts and ends where the split is (an empty comment range, Word's _GoBack
      // bookmark) goes with its start, so its end stays after it.
      if (l == 0 && s == o && StartMarker(NameOf(c))) {
        movedStarts.emplace_back(NameOf(c), std::string(Node(&t, c).AttrOr(kId)));
      } else if (l == 0 && s == o && !move) {
        const uint32_t start = StartOfEnd(NameOf(c));
        const std::string_view id = Node(&t, c).AttrOr(kId);
        for (const auto& [kind, movedId] : movedStarts)
          if (start != 0 && kind == start && movedId == id) move = true;
      }
      if (move) {
        Unlink(c);
        LinkAfter(to, last, c);
        last = c;
      } else if (s < o && o < s + l) {
        if (E(c).flags & kDisplayOnly) {
          why = u"This part of the document can't be split.";
          return false;
        }
        ElemId copy;
        if (RoleOf(c) == Role::Text) {
          copy = CloneShallow(c, to);
          E(copy).length = s + l - o;
          E(c).length = o - s;
        } else if (Splittable(NameOf(c))) {
          copy = CloneShallow(c, to);
          for (ElemId k = E(c).first; k != kNoElem; k = E(k).next)
            if (PropsElem(k)) Append(copy, CopyProps(k, copy));
          if (NameOf(c) == kIns || NameOf(c) == kDel || NameOf(c) == kMoveFrom || NameOf(c) == kMoveTo)
            SetAttr(copy, kId, NewAnnotationId());
          if (!SplitChildren(c, copy, o, why)) return false;
        } else {
          why = NameOf(c) == kFldSimple ? u"A field can't be split in two."
                                        : u"A content control can't be split in two here.";
          return false;
        }
        LinkAfter(to, last, copy);
        last = copy;
        if (RoleOf(c) != Role::Text && E(c).node)
          d.store_.Split(E(c).node, E(copy).node, pkg::SplitPoint{pkg::kNoNode, false});
      }
      c = next;
    }
    return true;
  }

  // Splits paragraph `para` at `o`; the second half becomes paragraph para + 1.
  bool Split(uint32_t para, uint32_t o, std::u16string& why) {
    // A field that begins and ends in the paragraph stays in one: split, its start and end would
    // sit in two paragraphs, and in a hyperlink (a table of contents entry) in two hyperlinks,
    // which Word can't open.
    {
      const std::u16string& s = d.paras_[para].text;
      std::vector<uint32_t> open;
      for (uint32_t i = 0; i < s.size(); ++i) {
        if (s[i] == 0x13) {
          open.push_back(i);
        } else if (s[i] == 0x15 && !open.empty()) {
          if (open.back() < o && o <= i) {
            why = u"A paragraph can't be split inside a field.";
            return false;
          }
          open.pop_back();
        }
      }
    }
    if (InOneParagraphControl(para, o)) {
      why = u"A date, list, plain-text or picture content control holds one paragraph, so it can't be split.";
      return false;
    }
    // Text shown from a kept mc:AlternateContent block is written as that block, which stays with
    // the element it's kept beside: a split just before the text goes after it, so the two agree.
    for (bool later = true; later;) {
      later = false;
      for (ElemId id : AllElems(para))
        if ((E(id).flags & kDisplayOnly) && E(id).start == o && E(id).length > 0 &&
            !(E(E(id).parent).flags & kDisplayOnly)) {
          o = E(id).start + E(id).length;
          later = true;
        }
    }
    movedStarts.clear();
    ElemId p = d.paras_[para].elem;
    ElemId np = CloneShallow(p, E(p).parent);
    for (uint32_t name : {kParaId, kTextId})
      if (Attr(np, name)) SetAttr(np, name, NewParaId());
    // commentsExtended names a comment by its last paragraph's w14:paraId: the second half, now
    // the last, takes it.
    if (auto id = Attr(p, kParaId); id && NamedByComments(*id) && Attr(np, kParaId)) {
      const std::string old(*id), fresh(*Attr(np, kParaId));
      SetAttr(p, kParaId, fresh);
      SetAttr(np, kParaId, old);
    }
    // The paragraph mark ends the second half, so the original w:pPr goes there, with its section
    // break, revision marks and kept markup. The first half gets a new mark with the same
    // properties, without those.
    if (ElemId pPr = Child(p, kPPr); pPr != kNoElem) {
      ElemId copy;
      if ((E(pPr).flags & kSharedKids) && !HasRepeats(pPr)) {
        copy = CloneShallow(pPr, p);
      } else {
        auto skip = [&](ElemId c) {
          uint32_t n = NameOf(c);
          return n == kSectPr || n == kPPrChange || n == kIns || n == kDel || n == kMoveFrom || n == kMoveTo ||
                 n == kRPrChange;
        };
        copy = CopyProps(pPr, p, skip, false);
      }
      Unlink(pPr);
      LinkAfter(np, kNoElem, pPr);
      if (E(copy).first != kNoElem || E(copy).attrCount > 0) LinkAfter(p, kNoElem, copy);
    }
    LinkAfter(E(p).parent, p, np);
    if (!SplitChildren(p, np, o, why)) return false;
    std::u16string& s = d.paras_[para].text;
    std::u16string tail = s.substr(o);
    s.resize(o);
    InsertPara(para + 1, np, d.paras_[para].story, std::move(tail));
    ElemId firstMoved = kNoElem;
    for (ElemId c = E(np).first; c != kNoElem; c = E(c).next)
      if (!PropsElem(c)) {
        firstMoved = c;
        break;
      }
    if (E(p).node)
      d.store_.Split(E(p).node, E(np).node,
                     pkg::SplitPoint{firstMoved == kNoElem ? pkg::kNoNode : E(firstMoved).node, firstMoved == kNoElem});
    Reflow(para);
    Reflow(para + 1);
    return true;
  }

  // A w14:paraId that commentsExtended uses to name a comment or the comment it answers.
  bool NamedByComments(std::string_view id) const {
    for (const auto& c : d.commentsEx_)
      if (c.paraId == id || c.parentParaId == id) return true;
    return false;
  }

  // The paragraph that follows `para` as its sibling, or kNone.
  uint32_t SiblingAfter(uint32_t para) {
    ElemId next = E(d.paras_[para].elem).next;
    while (next != kNoElem && RangeMarker(NameOf(next))) next = E(next).next;
    if (next == kNoElem || NameOf(next) != kP) return kNone;
    return E(next).start;
  }

  // Joins `para` with its next sibling paragraph (which is removed).
  bool Join(uint32_t para, std::u16string& why) {
    uint32_t q = SiblingAfter(para);
    if (q == kNone) {
      why = u"Paragraphs in different places (a table cell and the text around it) can't be joined.";
      return false;
    }
    ElemId p = d.paras_[para].elem, w = d.paras_[q].elem;
    ElemId pPr = Child(p, kPPr), qPr = Child(w, kPPr);
    if ((E(p).flags & kDisplayOnly) || (E(w).flags & kDisplayOnly)) {
      why = u"This part of the document can't be edited.";
      return false;
    }
    // A section break at the end of the first goes with its mark: its paragraphs join the next
    // section (the callers keep the headers and footers that one shows, KeptReferences).
    if (pPr != kNoElem && Child(pPr, kSectPr) != kNoElem) {
      Unshare(pPr);
      Unlink(Child(pPr, kSectPr));
      if (E(pPr).first == kNoElem && E(pPr).attrCount == 0 && (!E(pPr).node || (d.store_.FragmentsCopy(E(pPr).node).empty() && d.store_.NodeAttributes(E(pPr).node) == 0))) {
        Unlink(pPr);  // nothing else in it
        pPr = kNoElem;
      }
    }
    // Range markers between the two paragraphs move to the join point.
    std::vector<ElemId> between;
    for (ElemId c = E(p).next; c != w; c = E(c).next) between.push_back(c);
    bool empty = d.paras_[para].text.empty();
    // The joined paragraph keeps the first one's properties, unless it was empty.
    if (empty && qPr != kNoElem) {
      if (pPr != kNoElem) Unlink(pPr);
      Unlink(qPr);
      LinkAfter(p, kNoElem, qPr);
      pPr = qPr;
      qPr = kNoElem;
    } else if (qPr != kNoElem) {
      if (ElemId sect = Child(qPr, kSectPr); sect != kNoElem) {
        if (pPr == kNoElem) {
          pPr = NewElem(p, kPPr);
          LinkAfter(p, kNoElem, pPr);
        }
        Unshare(pPr);
        Unlink(sect);
        const auto& order = S().Type(TypeOf(pPr)).children;
        auto rank = [&](ElemId e) {
          auto it = std::find(order.begin(), order.end(), E(e).def);
          return static_cast<size_t>(it - order.begin());
        };
        ElemId slot = kNoElem;
        for (ElemId c = E(pPr).first; c != kNoElem && rank(c) <= rank(sect); c = E(c).next) slot = c;
        LinkAfter(pPr, slot, sect);
      }
    }
    ElemId last = LastChild(p);
    ElemId firstMoved = kNoElem;
    for (ElemId c : between) {
      Unlink(c);
      LinkAfter(p, last, c);
      last = c;
    }
    for (ElemId c = E(w).first; c != kNoElem;) {
      ElemId next = E(c).next;
      if (c != qPr) {
        Unlink(c);
        LinkAfter(p, last, c);
        last = c;
        if (firstMoved == kNoElem) firstMoved = c;
      }
      c = next;
    }
    // commentsExtended names a comment by its last paragraph's w14:paraId: the joined paragraph
    // takes the second one's when it's named there.
    if (auto id = Attr(w, kParaId); id && NamedByComments(*id)) {
      const std::string keep(*id);
      SetAttr(p, kParaId, keep);
    }
    d.paras_[para].text += d.paras_[q].text;
    if (E(p).node && E(w).node)
      d.store_.Merge(E(p).node, E(w).node, firstMoved == kNoElem ? pkg::kNoNode : E(firstMoved).node);
    Unlink(w);
    ErasePara(q);
    Reflow(para);
    return true;
  }

  // Sets properties in a w:rPr or w:pPr (created where the schema puts it when missing).
  bool ApplyProps(ElemId owner, uint32_t propsName, const PropEdit& edit, std::u16string& why) {
    ElemId props = Child(owner, propsName);
    if (props == kNoElem) {
      props = AddInOrder(owner, propsName);
      if (props == kNoElem) {
        why = u"These properties can't be set here.";
        return false;
      }
    }
    Unshare(props);
    for (const PropEdit::Item& item : edit.items) {
      ElemId cur = props;
      std::string_view path = item.name;
      while (true) {
        size_t slash = path.find('/');
        std::string_view part = path.substr(0, slash);
        uint32_t name = Name(part);
        ElemId child = Child(cur, name);
        bool lastPart = slash == std::string_view::npos;
        if (lastPart && item.remove) {
          if (child != kNoElem) {
            StoreFragments(FragmentsBeforeUnlink(d.store_, t, PrevSibling(child), child, E(child).next));
            Unlink(child);
          }
          break;
        }
        if (child == kNoElem) child = AddInOrder(cur, name);
        if (child == kNoElem) {
          why = u"Featherweight doesn't know the property " + Msg(std::string(part).c_str()) + u".";
          return false;
        }
        Unshare(child);
        if (lastPart) {
          if (item.attrs.empty() && Attr(child, kVal)) RemoveAttr(child, kVal);
          for (const auto& [attr, value] : item.attrs)
            if (!XmlSafe(value)) {
              why = u"A property's value can't hold control characters.";
              return false;
            } else if (!SetAttr(child, Name(attr), value)) {
              why = u"Featherweight doesn't know the attribute " + Msg(attr.c_str()) + u".";
              return false;
            }
          break;
        }
        cur = child;
        path = path.substr(slash + 1);
      }
    }
    return true;
  }

  // Cuts the runs that straddle `o` so that run boundaries fall there.
  bool CutRunsAt(uint32_t para, uint32_t o, std::u16string& why) {
    for (ElemId id : AllElems(para)) {
      if (NameOf(id) != kR) continue;
      uint32_t s = E(id).start, l = E(id).length;
      if (!(s < o && o < s + l) || (E(id).flags & kDisplayOnly)) continue;
      if (!Cut(id, o, why)) return false;
      Reflow(para);
    }
    return true;
  }
  // Cuts an inline container (run, link, revision...) that straddles `o` into two siblings, each
  // with the properties; a revision's second half gets an id of its own. The caller reflows.
  bool Cut(ElemId c, uint32_t o, std::u16string& why) {
    if (!Splittable(NameOf(c)) || (E(c).flags & kDisplayOnly)) {
      why = u"This part of the document can't be split here.";
      return false;
    }
    ElemId copy = CloneShallow(c, E(c).parent);
    for (ElemId k = E(c).first; k != kNoElem; k = E(k).next)
      if (PropsElem(k)) Append(copy, CopyProps(k, copy));
    uint32_t n = NameOf(c);
    if (n == kIns || n == kDel || n == kMoveFrom || n == kMoveTo) SetAttr(copy, kId, NewAnnotationId());
    LinkAfter(E(c).parent, c, copy);
    if (!SplitChildren(c, copy, o, why)) return false;
    if (E(c).node) d.store_.Split(E(c).node, E(copy).node, pkg::SplitPoint{pkg::kNoNode, false});
    return true;
  }

  // Fields covered in part by [from, to) (positions in one story), which a delete must refuse.
  bool CutsField(Pos from, Pos to) {
    bool any = false;
    for (uint32_t p = from.para; p <= to.para && p < d.paras_.size() && !any; ++p) {
      if (d.paras_[p].story != d.paras_[from.para].story) continue;
      const auto& s = d.paras_[p].text;
      uint32_t a = p == from.para ? from.offset : 0, b = p == to.para ? to.offset : static_cast<uint32_t>(s.size());
      for (uint32_t i = a; i < b && i < s.size(); ++i)
        if (IsFieldChar(s[i])) any = true;
    }
    if (!any) return false;
    auto inside = [&](Pos p) {
      if (p.para == kNone) return false;
      return ed.Compare(from, p) <= 0 && ed.Compare(p, to) < 0;
    };
    for (const Field& f : d.Fields(d.paras_[from.para].story)) {
      if (f.simple) continue;
      bool b = inside(f.begin), e = inside(f.end), s = f.separator.para != kNone && inside(f.separator);
      bool sepSame = f.separator.para == kNone || s == b;
      if (b != e || !sepSame) return true;
    }
    return false;
  }

  // ---------- Tables (D3) ----------
  // A table on its grid, read from the tree for each change. Columns are grid columns.
  struct GridCell {
    ElemId tc = kNoElem;
    int32_t col = 0, span = 1;
    VMerge v = VMerge::None;
  };
  struct GridRow {
    ElemId tr = kNoElem;
    int32_t before = 0, after = 0;  // w:gridBefore, w:gridAfter
    std::vector<GridCell> cells;
    int32_t End() const { return cells.empty() ? before : cells.back().col + cells.back().span; }
  };
  struct Grid {
    ElemId tbl = kNoElem, tblGrid = kNoElem;
    std::vector<ElemId> cols;  // w:gridCol
    std::vector<GridRow> rows;
    int32_t width = 0;  // grid columns the widest row uses
    // The cell of row `r` that covers grid column `c`: its index in rows[r].cells, or -1.
    int At(size_t r, int32_t c) const {
      const auto& cs = rows[r].cells;
      for (size_t i = 0; i < cs.size(); ++i)
        if (cs[i].col <= c && c < cs[i].col + cs[i].span) return static_cast<int>(i);
      return -1;
    }
  };

  static int32_t IntOr(Node n, uint32_t attr, int32_t fallback) {
    auto v = n ? ParseInt(n.AttrOr(attr)) : std::nullopt;
    return v ? static_cast<int32_t>(std::clamp<int64_t>(*v, -1000000, 1000000)) : fallback;
  }
  // The innermost table around a paragraph: its w:tbl, w:tr and w:tc. False outside tables.
  bool CellOf(uint32_t para, ElemId& tbl, ElemId& tr, ElemId& tc) {
    tbl = tr = tc = kNoElem;
    for (ElemId a = E(d.paras_[para].elem).parent; a != kNoElem; a = E(a).parent) {
      if (NameOf(a) != kTc) continue;
      tc = a;
      tr = E(tc).parent;
      tbl = tr == kNoElem ? kNoElem : E(tr).parent;
      return tbl != kNoElem;
    }
    return false;
  }
  // Reads `tbl` onto its grid. Refused (with why) where a change of shape can't be done safely:
  // rows or cells inside content controls or custom XML, old-style horizontal merges, and tracked
  // cell insertions, deletions, merges and grid changes.
  bool ReadGrid(ElemId tbl, Grid& g, std::u16string& why) {
    static const uint32_t kGridSpan = Name("gridSpan"), kVMerge = Name("vMerge"), kHMerge = Name("hMerge"),
                          kGridBefore = Name("gridBefore"), kGridAfter = Name("gridAfter"), kCellIns = Name("cellIns"),
                          kCellDel = Name("cellDel"), kCellMerge = Name("cellMerge"), kTblPrEx = Name("tblPrEx"),
                          kGridChange = Name("tblGridChange");
    g = {};
    g.tbl = tbl;
    const std::u16string wrapped = u"This table has rows or cells inside content controls, so its shape can't be changed yet.";
    for (ElemId c = E(tbl).first; c != kNoElem; c = E(c).next) {
      uint32_t n = NameOf(c);
      if (n == kTblPr || RangeMarker(n)) continue;
      if (n == kTblGrid) {
        g.tblGrid = c;
        for (ElemId k = E(c).first; k != kNoElem; k = E(k).next) {
          if (NameOf(k) == kGridCol) g.cols.push_back(k);
          else if (NameOf(k) == kGridChange) return why = u"This table's columns have tracked changes, so its shape can't be changed yet.", false;
        }
        continue;
      }
      if (n != kTr) return why = wrapped, false;
      GridRow row;
      row.tr = c;
      ElemId trPr = Child(c, kTrPr);
      row.before = std::max(0, IntOr(Node(&t, trPr).Child(kGridBefore), kVal, 0));
      row.after = std::max(0, IntOr(Node(&t, trPr).Child(kGridAfter), kVal, 0));
      int32_t col = row.before;
      for (ElemId k = E(c).first; k != kNoElem; k = E(k).next) {
        uint32_t kn = NameOf(k);
        if (kn == kTrPr || kn == kTblPrEx || RangeMarker(kn)) continue;
        if (kn != kTc) return why = wrapped, false;
        GridCell cell;
        cell.tc = k;
        cell.col = col;
        Node tcPr = Node(&t, k).Child(kTcPr);
        cell.span = std::clamp(IntOr(tcPr.Child(kGridSpan), kVal, 1), 1, 1000);
        if (Node vm = tcPr.Child(kVMerge)) cell.v = vm.AttrOr(kVal) == "restart" ? VMerge::Restart : VMerge::Continue;
        if (tcPr.Child(kHMerge)) return why = u"This table uses an old kind of merged cells, so its shape can't be changed yet.", false;
        if (tcPr.Child(kCellIns) || tcPr.Child(kCellDel) || tcPr.Child(kCellMerge))
          return why = u"This table has tracked cell changes. Accept or reject them first.", false;
        col += cell.span;
        row.cells.push_back(cell);
      }
      if (row.cells.empty()) return why = u"This table has a row without cells, so its shape can't be changed.", false;
      g.width = std::max(g.width, col + row.after);
      g.rows.push_back(std::move(row));
    }
    if (g.rows.empty()) return why = u"This table has no rows.", false;
    if (g.tblGrid == kNoElem) return why = u"This table has no grid, so its shape can't be changed yet.", false;
    return true;
  }
  // The rows a cell's vertical merge covers: [top, bottom].
  void MergeRows(const Grid& g, size_t r, int i, size_t& top, size_t& bottom) {
    const GridCell& c = g.rows[r].cells[i];
    auto same = [&](size_t row) {
      int j = g.At(row, c.col);
      return j >= 0 && g.rows[row].cells[j].col == c.col && g.rows[row].cells[j].span == c.span ? j : -1;
    };
    top = bottom = r;
    if (c.v == VMerge::Continue)
      while (top > 0) {
        int j = same(top - 1);
        if (j < 0) break;
        --top;
        if (g.rows[top].cells[j].v != VMerge::Continue) break;
      }
    if (c.v != VMerge::None)
      while (bottom + 1 < g.rows.size()) {
        int j = same(bottom + 1);
        if (j < 0 || g.rows[bottom + 1].cells[j].v != VMerge::Continue) break;
        ++bottom;
      }
  }
  size_t RowOf(const Grid& g, ElemId tr) {
    for (size_t r = 0; r < g.rows.size(); ++r)
      if (g.rows[r].tr == tr) return r;
    return SIZE_MAX;
  }
  int CellIndex(const Grid& g, size_t r, ElemId tc) {
    for (size_t i = 0; i < g.rows[r].cells.size(); ++i)
      if (g.rows[r].cells[i].tc == tc) return static_cast<int>(i);
    return -1;
  }
  // The block of cells between the cells of two positions in one table, grown until it holds
  // every cell it touches whole, as Word selects cells.
  struct Block {
    size_t row0 = 0, row1 = 0;
    int32_t col0 = 0, col1 = 0;  // inclusive
  };
  bool BlockOf(Pos a, Pos b, Grid& g, Block& bl, std::u16string& why) {
    ElemId tbl, tr, tc, tbl2, tr2, tc2;
    if (!CellOf(a.para, tbl, tr, tc)) return why = u"Put the cursor in a table first.", false;
    if (!CellOf(b.para, tbl2, tr2, tc2) || tbl2 != tbl) tr2 = tr, tc2 = tc;
    if (!ReadGrid(tbl, g, why)) return false;
    size_t ra = RowOf(g, tr), rb = RowOf(g, tr2);
    if (ra == SIZE_MAX || rb == SIZE_MAX) return why = u"This table's shape can't be read.", false;
    int ia = CellIndex(g, ra, tc), ib = CellIndex(g, rb, tc2);
    if (ia < 0 || ib < 0) return why = u"This table's shape can't be read.", false;
    bl.row0 = std::min(ra, rb);
    bl.row1 = std::max(ra, rb);
    bl.col0 = std::min(g.rows[ra].cells[ia].col, g.rows[rb].cells[ib].col);
    bl.col1 = std::max(g.rows[ra].cells[ia].col + g.rows[ra].cells[ia].span, g.rows[rb].cells[ib].col + g.rows[rb].cells[ib].span) - 1;
    for (bool grew = true; grew;) {
      grew = false;
      for (size_t r = bl.row0; r <= bl.row1; ++r)
        for (size_t i = 0; i < g.rows[r].cells.size(); ++i) {
          const GridCell& c = g.rows[r].cells[i];
          if (c.col > bl.col1 || c.col + c.span - 1 < bl.col0) continue;
          size_t top, bottom;
          MergeRows(g, r, static_cast<int>(i), top, bottom);
          int32_t c0 = std::min(bl.col0, c.col), c1 = std::max(bl.col1, c.col + c.span - 1);
          size_t r0 = std::min(bl.row0, top), r1 = std::max(bl.row1, bottom);
          if (c0 != bl.col0 || c1 != bl.col1 || r0 != bl.row0 || r1 != bl.row1) {
            bl = {r0, r1, c0, c1};
            grew = true;
          }
        }
    }
    return true;
  }

  // The paragraphs of a block (a w:p, a table and the tables inside it) in order: block-level
  // containers only, not what's inside paragraphs (a text box is a story of its own).
  void ParasIn(ElemId block, std::vector<ElemId>& out) {
    uint32_t n = NameOf(block);
    if (n == kP) {
      out.push_back(block);
      return;
    }
    if (n == kTbl || n == kTr || n == kTc || n == Name("sdt") || n == Name("sdtContent") || n == Name("customXml"))
      for (ElemId c = E(block).first; c != kNoElem; c = E(c).next) ParasIn(c, out);
  }
  // A table's paragraphs: the first and how many. False when they aren't one run of its story
  // (a paragraph shown from kept content).
  bool TableParas(ElemId tbl, uint32_t& first, uint32_t& count, std::u16string& why) {
    std::vector<ElemId> ps;
    ParasIn(tbl, ps);
    if (ps.empty()) return why = u"This table has no paragraphs.", false;
    first = E(ps.front()).start;
    count = static_cast<uint32_t>(ps.size());
    for (size_t i = 0; i < ps.size(); ++i)
      if (first + i >= d.paras_.size() || d.paras_[first + i].elem != ps[i] || (E(ps[i]).flags & kDisplayOnly))
        return why = u"This table can't be changed: part of it is shown from content Featherweight keeps as it is.", false;
    return true;
  }

  // Elements a transaction changes besides its paragraphs: a table (its rows, cells and grid) with
  // its parent and neighbours. Begin saves those it's given as they were; Commit saves them, and
  // the ones added since (new tables), as they are.
  std::vector<ElemId> roots;
  void CollectRoots(std::vector<ElemId>& ids, std::unordered_set<ElemId>& seen) {
    auto add = [&](ElemId id) {
      if (id != kNoElem && seen.insert(id).second) ids.push_back(id);
    };
    for (ElemId root : roots) {
      add(E(root).parent);
      if (E(root).parent != kNoElem) add(PrevSibling(root));
      add(E(root).next);
      std::vector<ElemId> stack{root};
      while (!stack.empty()) {
        ElemId id = stack.back();
        stack.pop_back();
        add(id);
        if (E(id).flags & kSharedKids) continue;
        for (ElemId c = E(id).first; c != kNoElem; c = E(c).next) stack.push_back(c);
      }
    }
  }

  // The transaction's paragraphs are now those of `blocks`, in order: paragraphs that stayed keep
  // their text, new ones are empty. Renumbers the rest.
  void Resync(const std::vector<ElemId>& blocks) {
    const uint32_t first = tx.first, count = CurrentCount();
    std::unordered_map<ElemId, Paragraph> old;
    for (uint32_t p = first; p < first + count; ++p) old.emplace(d.paras_[p].elem, std::move(d.paras_[p]));
    std::vector<ElemId> ps;
    for (ElemId b : blocks) ParasIn(b, ps);
    std::vector<Paragraph> now;
    now.reserve(ps.size());
    std::vector<uint32_t> fresh;
    for (ElemId w : ps) {
      if (auto it = old.find(w); it != old.end()) {
        now.push_back(std::move(it->second));
        continue;
      }
      Paragraph p;
      p.elem = w;
      p.story = tx.story;
      p.version = ++version;
      fresh.push_back(static_cast<uint32_t>(now.size()));
      now.push_back(std::move(p));
    }
    d.paras_.erase(d.paras_.begin() + first, d.paras_.begin() + first + count);
    d.paras_.insert(d.paras_.begin() + first, std::make_move_iterator(now.begin()), std::make_move_iterator(now.end()));
    Renumber(first);
    for (uint32_t i : fresh) Reflow(first + i);
  }

  // Elements from XML in the w: namespace, every element and attribute known where it goes;
  // linked into `parent` after `after` (kNoElem: first). kNoElem when the XML isn't.
  static uint32_t QName(std::string_view n) { return n.starts_with("w:") ? Name(n.substr(2)) : Name(n); }
  ElemId Make(ElemId parent, ElemId after, const std::string& xml) {
    pugi::xml_document doc;
    if (!doc.load_buffer(xml.data(), xml.size())) return kNoElem;
    std::function<ElemId(ElemId, const pugi::xml_node&)> build = [&](ElemId par, const pugi::xml_node& n) -> ElemId {
      ElemId id = NewElem(par, QName(n.name()));
      if (id == kNoElem) return kNoElem;
      for (pugi::xml_attribute a : n.attributes())
        if (!SetAttr(id, QName(a.name()), a.value())) return kNoElem;
      ElemId last = kNoElem;
      for (pugi::xml_node c : n.children()) {
        if (c.type() != pugi::node_element) continue;
        ElemId k = build(id, c);
        if (k == kNoElem) return kNoElem;
        LinkAfter(id, last, k);
        last = k;
      }
      return id;
    };
    ElemId id = build(parent, doc.first_child());
    if (id != kNoElem) LinkAfter(parent, after, id);
    return id;
  }
  // A property element made again in `parent`'s schema place, replacing one of the same name.
  ElemId SetProp(ElemId parent, const std::string& xml) {
    pugi::xml_document doc;
    if (!doc.load_buffer(xml.data(), xml.size())) return kNoElem;
    uint32_t name = QName(doc.first_child().name());
    if (ElemId old = Child(parent, name); old != kNoElem) {
      StoreFragments(FragmentsBeforeUnlink(d.store_, t, PrevSibling(old), old, E(old).next));
      Unlink(old);
    }
    if (S().Child(TypeOf(parent), name) == kNoDef) return kNoElem;
    ElemId after = SlotFor(parent, name);
    ElemId id = Make(parent, after, xml);
    if (id != kNoElem) StoreFragments(FragmentsAfterLink(d.store_, t, after, id));
    return id;
  }
  // The child a new child named `name` follows in schema order (kNoElem: it goes first).
  ElemId SlotFor(ElemId parent, uint32_t name) {
    const auto& order = S().Type(TypeOf(parent)).children;
    auto rank = [&](ElemDefId e) {
      auto it = std::find(order.begin(), order.end(), e);
      return it == order.end() ? order.size() : static_cast<size_t>(it - order.begin());
    };
    size_t mine = rank(S().Child(TypeOf(parent), name));
    ElemId after = kNoElem;
    for (ElemId c = E(parent).first; c != kNoElem && rank(E(c).def) <= mine; c = E(c).next) after = c;
    return after;
  }
  void RemoveProp(ElemId parent, uint32_t name) {
    if (parent == kNoElem) return;
    if (ElemId old = Child(parent, name); old != kNoElem) {
      StoreFragments(FragmentsBeforeUnlink(d.store_, t, PrevSibling(old), old, E(old).next));
      Unlink(old);
    }
  }
  ElemId Props(ElemId owner, uint32_t name) {
    ElemId p = Child(owner, name);
    if (p == kNoElem) p = AddInOrder(owner, name);
    if (p != kNoElem) Unshare(p);
    return p;
  }

  // A new paragraph for a new cell, with the properties of `like` (a paragraph) without its section
  // break, list, revision marks and kept content.
  ElemId NewCellPara(ElemId tc, ElemId after, ElemId like) {
    ElemId p = NewElem(tc, kP);
    if (p == kNoElem) return kNoElem;
    if (like != kNoElem) {
      for (uint32_t name : {kParaId, kTextId})
        if (Attr(like, name)) SetAttr(p, name, NewParaId());
      if (ElemId pPr = Child(like, kPPr); pPr != kNoElem) {
        auto skip = [&](ElemId c) {
          uint32_t n = NameOf(c);
          return n == kSectPr || n == kPPrChange || n == kIns || n == kDel || n == kMoveFrom || n == kMoveTo ||
                 n == kRPrChange || n == Name("numPr");
        };
        ElemId copy = CopyProps(pPr, p, skip, false);
        if (E(copy).first != kNoElem || E(copy).attrCount > 0) LinkAfter(p, kNoElem, copy);
      }
    }
    LinkAfter(tc, after, p);
    return p;
  }
  // The first paragraph of a cell (kNoElem when it starts with a table).
  ElemId FirstPara(ElemId tc) {
    for (ElemId c = E(tc).first; c != kNoElem; c = E(c).next)
      if (NameOf(c) == kP) return c;
    return kNoElem;
  }
  // A new cell after `after` in `tr`, like `like` (a cell: its properties and its first paragraph's),
  // covering `span` grid columns of width `twips` in all.
  ElemId NewCell(ElemId tr, ElemId after, ElemId like, int32_t span, VMerge v, int32_t twips) {
    static const uint32_t kGridSpan = Name("gridSpan"), kVMerge = Name("vMerge"), kTcW = Name("tcW");
    ElemId tc = NewElem(tr, kTc);
    if (tc == kNoElem) return kNoElem;
    ElemId tcPr = kNoElem;
    if (ElemId likePr = like == kNoElem ? kNoElem : Child(like, kTcPr); likePr != kNoElem) {
      auto skip = [&](ElemId c) {
        uint32_t n = NameOf(c);
        return n == kGridSpan || n == kVMerge || n == Name("hMerge") || n == Name("cellIns") || n == Name("cellDel") ||
               n == Name("cellMerge") || n == Name("tcPrChange");
      };
      tcPr = CopyProps(likePr, tc, skip, false);
      LinkAfter(tc, kNoElem, tcPr);
      Unshare(tcPr);
    } else {
      tcPr = AddInOrder(tc, kTcPr);
    }
    if (tcPr == kNoElem) return kNoElem;
    Node w = Node(&t, tcPr).Child(kTcW);
    if (!w || w.AttrOr(kType) == "dxa" || w.AttrOr(kType).empty()) SetProp(tcPr, "<w:tcW w:w=\"" + std::to_string(twips) + "\" w:type=\"dxa\"/>");
    if (span > 1) SetProp(tcPr, "<w:gridSpan w:val=\"" + std::to_string(span) + "\"/>");
    if (v == VMerge::Restart) SetProp(tcPr, "<w:vMerge w:val=\"restart\"/>");
    else if (v == VMerge::Continue) SetProp(tcPr, "<w:vMerge/>");
    if (NewCellPara(tc, tcPr, like == kNoElem ? kNoElem : FirstPara(like)) == kNoElem) return kNoElem;
    LinkAfter(tr, after, tc);
    return tc;
  }
  void SetSpan(ElemId tc, int32_t span) {
    ElemId tcPr = Props(tc, kTcPr);
    if (span > 1) SetProp(tcPr, "<w:gridSpan w:val=\"" + std::to_string(span) + "\"/>");
    else RemoveProp(tcPr, Name("gridSpan"));
  }
  void SetVMerge(ElemId tc, VMerge v) {
    ElemId tcPr = Props(tc, kTcPr);
    if (v == VMerge::Restart) SetProp(tcPr, "<w:vMerge w:val=\"restart\"/>");
    else if (v == VMerge::Continue) SetProp(tcPr, "<w:vMerge/>");
    else RemoveProp(tcPr, Name("vMerge"));
  }
  void SetRowGrid(ElemId tr, uint32_t name, int32_t value) {  // w:gridBefore, w:gridAfter
    ElemId trPr = Child(tr, kTrPr);
    if (value <= 0) {
      if (trPr != kNoElem) {
        Unshare(trPr);
        RemoveProp(trPr, name);
      }
      return;
    }
    trPr = Props(tr, kTrPr);
    SetProp(trPr, std::string("<w:") + (name == Name("gridBefore") ? "gridBefore" : "gridAfter") + " w:val=\"" +
                      std::to_string(value) + "\"/>");
  }
  int32_t ColWidth(const Grid& g, int32_t c) {
    return c >= 0 && static_cast<size_t>(c) < g.cols.size() ? IntOr(Node(&t, g.cols[c]), kW, 0) : 0;
  }
  // The grid's widths, as many as the widest row uses (a short grid gets the average width).
  std::vector<int32_t> Widths(const Grid& g) {
    std::vector<int32_t> w;
    int64_t sum = 0;
    for (int32_t c = 0; c < static_cast<int32_t>(g.cols.size()); ++c) sum += ColWidth(g, c);
    int32_t avg = g.cols.empty() ? 1440 : static_cast<int32_t>(sum / static_cast<int64_t>(g.cols.size()));
    for (int32_t c = 0; c < g.width; ++c) w.push_back(c < static_cast<int32_t>(g.cols.size()) ? ColWidth(g, c) : std::max(avg, 1));
    return w;
  }
  // Writes the grid's column widths, and each dxa cell width and dxa table width from them.
  void WriteGrid(ElemId tbl, const std::vector<int32_t>& widths) {
    static const uint32_t kTcW = Name("tcW"), kTblW = Name("tblW");
    ElemId grid = Child(tbl, kTblGrid);
    for (ElemId c; (c = E(grid).first) != kNoElem;) {
      StoreFragments(FragmentsBeforeUnlink(d.store_, t, PrevSibling(c), c, E(c).next));
      Unlink(c);
    }
    ElemId last = kNoElem;
    for (int32_t w : widths) last = Make(grid, last, "<w:gridCol w:w=\"" + std::to_string(std::max(w, 0)) + "\"/>");
    Grid g;
    std::u16string why;
    if (!ReadGrid(tbl, g, why)) return;
    for (const GridRow& row : g.rows)
      for (const GridCell& c : row.cells) {
        int64_t sum = 0;
        for (int32_t k = c.col; k < c.col + c.span && k < static_cast<int32_t>(widths.size()); ++k) sum += widths[k];
        Node w = Node(&t, c.tc).Child(kTcPr).Child(kTcW);
        if (w && w.AttrOr(kType) != "dxa" && !w.AttrOr(kType).empty()) continue;
        SetProp(Props(c.tc, kTcPr), "<w:tcW w:w=\"" + std::to_string(sum) + "\" w:type=\"dxa\"/>");
      }
    if (Node w = Node(&t, tbl).Child(kTblPr).Child(kTblW); w && w.AttrOr(kType) == "dxa") {
      int64_t sum = 0;
      for (int32_t x : widths) sum += x;
      SetProp(Props(tbl, kTblPr), "<w:tblW w:w=\"" + std::to_string(sum) + "\" w:type=\"dxa\"/>");
    }
  }
  // Vertical merges made whole again after rows or cells went: a continuation without the cell it
  // continues starts a merge (or none, alone); a start that nothing continues is no merge.
  void FixMerges(ElemId tbl) {
    Grid g;
    std::u16string why;
    if (!ReadGrid(tbl, g, why)) return;
    for (size_t r = 0; r < g.rows.size(); ++r)
      for (size_t i = 0; i < g.rows[r].cells.size(); ++i) {
        GridCell& c = g.rows[r].cells[i];
        if (c.v == VMerge::None) continue;
        auto same = [&](size_t row) -> GridCell* {
          if (row >= g.rows.size()) return nullptr;
          int j = g.At(row, c.col);
          GridCell* o = j >= 0 ? &g.rows[row].cells[j] : nullptr;
          return o && o->col == c.col && o->span == c.span ? o : nullptr;
        };
        GridCell* above = r > 0 ? same(r - 1) : nullptr;
        GridCell* below = same(r + 1);
        bool continued = below && below->v == VMerge::Continue;
        VMerge want = c.v;
        if (c.v == VMerge::Continue && !(above && above->v != VMerge::None)) want = continued ? VMerge::Restart : VMerge::None;
        if (want == VMerge::Restart && !continued) want = VMerge::None;
        if (want != c.v) {
          SetVMerge(c.tc, want);
          c.v = want;
        }
      }
  }

  // The width of a cell's text: its grid columns less its margins (Word's default 0.08 inch a side).
  int32_t CellWidth(ElemId tc) {
    ElemId tr = E(tc).parent, tbl = tr == kNoElem ? kNoElem : E(tr).parent;
    Grid g;
    std::u16string why;
    if (tbl == kNoElem || !ReadGrid(tbl, g, why)) return 2880;
    size_t r = RowOf(g, tr);
    int i = r == SIZE_MAX ? -1 : CellIndex(g, r, tc);
    if (i < 0) return 2880;
    int32_t w = 0;
    for (int32_t k = g.rows[r].cells[i].col; k < g.rows[r].cells[i].col + g.rows[r].cells[i].span; ++k) w += ColWidth(g, k);
    return w > 0 ? std::max(w - 216, 360) : 2880;
  }
  // The room a table has: the cell it's in, or its section's text width (`para`: a paragraph of
  // the table's, for the section).
  int32_t RoomFor(ElemId tbl, uint32_t para) {
    for (ElemId a = E(tbl).parent; a != kNoElem; a = E(a).parent)
      if (NameOf(a) == kTc) return CellWidth(a);
    return SectionWidth(para);
  }
  // The text width a new table fills: the cell around the paragraph, or its section's.
  int32_t AvailableWidth(uint32_t para) {
    ElemId tbl, tr, tc;
    if (CellOf(para, tbl, tr, tc)) return CellWidth(tc);
    return SectionWidth(para);
  }
  // The section's page between its margins (its first column's width).
  int32_t SectionWidth(uint32_t para) {
    const SectionProps* sp = nullptr;
    for (const Section& s : d.sections_)
      if (s.endPara == kNone || s.endPara >= para) {
        sp = &s.props;
        break;
      }
    if (!sp) return 9360;
    int32_t w = sp->pageWidth.Value() - sp->marginStart.Value() - sp->marginEnd.Value() - sp->gutter.Value();
    if (sp->columns.count > 1) {
      if (!sp->columns.equalWidth && !sp->columns.cols.empty()) w = sp->columns.cols[0].width.Value();
      else w = (w - sp->columns.space.Value() * (sp->columns.count - 1)) / sp->columns.count;
    }
    return std::max(w, 720);
  }

  // Range markers and other content that a deletion of `gone` (cells, rows or a table) would cut
  // from what's outside it: notes and comments anchored there can't go (refused); a field that
  // runs out of it can't either; the start or end of a range whose other end stays moves to the
  // start of `heir`, the paragraph that takes the deleted content's place.
  bool Evacuate(const std::vector<ElemId>& gone, ElemId heir, std::u16string& why) {
    static const std::vector<std::pair<uint32_t, uint32_t>> pairs = {
        {Name("bookmarkStart"), Name("bookmarkEnd")},
        {Name("commentRangeStart"), Name("commentRangeEnd")},
        {Name("moveFromRangeStart"), Name("moveFromRangeEnd")},
        {Name("moveToRangeStart"), Name("moveToRangeEnd")},
        {Name("permStart"), Name("permEnd")},
        {Name("customXmlInsRangeStart"), Name("customXmlInsRangeEnd")},
        {Name("customXmlDelRangeStart"), Name("customXmlDelRangeEnd")},
        {Name("customXmlMoveFromRangeStart"), Name("customXmlMoveFromRangeEnd")},
        {Name("customXmlMoveToRangeStart"), Name("customXmlMoveToRangeEnd")}};
    std::vector<ElemId> markers;
    std::map<std::pair<uint32_t, std::string>, int> inside;  // (pair kind, id) -> ends inside
    int fields = 0;
    std::vector<ElemId> stack(gone.rbegin(), gone.rend());
    while (!stack.empty()) {
      ElemId id = stack.back();
      stack.pop_back();
      uint32_t n = NameOf(id);
      if (Undeletable(n)) return why = u"Footnotes, endnotes and comments in these cells would lose their place. Delete them first.", false;
      if (n == kP) {
        if (E(id).start >= d.paras_.size() || d.paras_[E(id).start].elem != id || d.paras_[E(id).start].story != tx.story)
          return why = u"Text boxes in these cells can't be deleted yet.", false;
        const std::u16string& s = d.paras_[E(id).start].text;
        fields += static_cast<int>(std::count(s.begin(), s.end(), u'\x13')) - static_cast<int>(std::count(s.begin(), s.end(), u'\x15'));
      }
      for (size_t k = 0; k < pairs.size(); ++k)
        if (n == pairs[k].first || n == pairs[k].second) {
          markers.push_back(id);
          ++inside[{static_cast<uint32_t>(k), std::string(Attr(id, kId).value_or(""))}];
        }
      if (E(id).flags & kSharedKids) continue;
      for (ElemId c = E(id).first; c != kNoElem; c = E(c).next) stack.push_back(c);
    }
    if (fields != 0) return why = u"A field runs into these cells. Delete the field first.", false;
    std::vector<ElemId> keep;
    for (ElemId m : markers) {
      uint32_t n = NameOf(m);
      for (size_t k = 0; k < pairs.size(); ++k)
        if (n == pairs[k].first || n == pairs[k].second)
          if (inside[{static_cast<uint32_t>(k), std::string(Attr(m, kId).value_or(""))}] < 2) keep.push_back(m);
    }
    if (keep.empty()) return true;
    if (heir == kNoElem) return why = u"A bookmark or comment range starts or ends here, so this can't be deleted yet.", false;
    ElemId after = Child(heir, kPPr);
    for (ElemId m : keep) {
      Unlink(m);
      LinkAfter(heir, after, m);
      after = m;
    }
    return true;
  }
  // The paragraph after a block in reading order inside its container, or the first one inside the
  // next table; kNoElem at the end.
  ElemId ParaAfter(ElemId block) {
    for (ElemId c = E(block).next; c != kNoElem; c = E(c).next) {
      std::vector<ElemId> ps;
      ParasIn(c, ps);
      if (!ps.empty()) return ps.front();
    }
    return kNoElem;
  }
  // Takes an element out of its parent; what was kept beside it stays where it was.
  void Drop(ElemId id) {
    StoreFragments(FragmentsBeforeUnlink(d.store_, t, PrevSibling(id), id, E(id).next));
    Unlink(id);
  }
  // A w:p the paragraph list has (at its own index).
  bool Listed(ElemId p) const;
  bool IsBlock(ElemId id) {
    uint32_t n = NameOf(id);
    return n == kP || n == kTbl || n == Name("sdt") || n == Name("customXml");
  }
  ElemId NextBlock(ElemId id) {
    for (ElemId c = E(id).next; c != kNoElem; c = E(c).next)
      if (IsBlock(c)) return c;
    return kNoElem;
  }
  ElemId PrevBlock(ElemId id) {
    ElemId found = kNoElem;
    for (ElemId c = E(E(id).parent).first; c != kNoElem && c != id; c = E(c).next)
      if (IsBlock(c)) found = c;
    return found;
  }
};

// ---------- Editor ----------

Editor::Editor(Document& doc) : doc_(doc), impl_(std::make_unique<EditorState>(doc, *this)) {}
Editor::~Editor() = default;

bool Editor::CanEdit(std::u16string* why) const {
  if (!doc_.Editable()) {
    if (why) *why = u"This document uses features Featherweight doesn't understand, so it's shown but can't be edited.";
    return false;
  }
  if (const Part* main = doc_.FindPart(PartKind::Document); main && !main->writable) {
    if (why) *why = u"Featherweight can't write this document back exactly, so it's shown but can't be edited.";
    return false;
  }
  return true;
}

int Editor::Compare(Pos a, Pos b) const {
  if (a.para == b.para) return a.offset < b.offset ? -1 : a.offset > b.offset ? 1 : 0;
  if (a.para < doc_.Paragraphs().size() && b.para < doc_.Paragraphs().size() &&
      doc_.Para(a.para).story != doc_.Para(b.para).story)
    return doc_.Para(a.para).story < doc_.Para(b.para).story ? -1 : 1;
  return a.para < b.para ? -1 : 1;
}

uint32_t Editor::PrevPara(uint32_t para) const {
  uint32_t story = doc_.Para(para).story;
  for (uint32_t p = para; p-- > 0;)
    if (doc_.Para(p).story == story) return p;
  return kNone;
}

uint32_t Editor::NextPara(uint32_t para) const {
  uint32_t story = doc_.Para(para).story;
  for (uint32_t p = para + 1; p < doc_.Paragraphs().size(); ++p)
    if (doc_.Para(p).story == story) return p;
  return kNone;
}

bool XmlSafe(std::string_view s) {
  static constexpr uint32_t kLeast[] = {0, 0, 0x80, 0x800, 0x10000};  // below these, overlong
  for (size_t i = 0; i < s.size();) {
    const auto lead = static_cast<unsigned char>(s[i]);
    const int n = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 0;
    if (n == 0 || i + n > s.size()) return false;
    uint32_t cp = n == 1 ? lead : lead & (0x7Fu >> n);
    for (int k = 1; k < n; ++k) {
      const auto b = static_cast<unsigned char>(s[i + k]);
      if ((b & 0xC0) != 0x80) return false;
      cp = cp << 6 | (b & 0x3Fu);
    }
    if (cp < kLeast[n] || cp > 0x10FFFF) return false;
    if (!(cp == 0x9 || cp == 0xA || cp == 0xD || (cp >= 0x20 && cp <= 0xD7FF) || (cp >= 0xE000 && cp <= 0xFFFD) || cp >= 0x10000))
      return false;
    i += n;
  }
  return true;
}

namespace {
Result Fail(std::u16string why) {
  Result r;
  r.ok = false;
  r.message = std::move(why);
  return r;
}
}  // namespace

Result Editor::InsertText(Pos at, std::u16string_view text) {
  EditorState& s = *impl_;
  std::u16string why;
  if (!CanEdit(&why) || !s.Editable(at.para, why)) return Fail(why);
  if (at.offset > ParaLength(at.para)) return Fail(u"That place isn't in the paragraph.");
  if (text.empty()) {
    Result r;
    r.caret = at;
    return r;
  }
  // Split into lines at paragraph breaks.
  std::vector<std::u16string> lines(1);
  for (size_t i = 0; i < text.size(); ++i) {
    char16_t c = text[i];
    if (c == u'\r' || c == u'\n') {
      if (c == u'\r' && i + 1 < text.size() && text[i + 1] == u'\n') ++i;
      lines.emplace_back();
    } else if ((c == u'\t' || c == 0x0B || c == 0x0C || c == 0x0E || c >= 0x20) && c != 0xFFFC) {  // U+FFFC: objects
      if (c == 0x2028) lines.back() += char16_t(0x0B);
      else if (c == 0x2029) lines.emplace_back();
      else lines.back() += c;
    }
  }
  if (lines.size() == 1 && lines[0].empty()) {  // nothing that can be typed
    Result r;
    r.caret = at;
    return r;
  }
  s.Begin(at.para, 1);
  Pos pos = at;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) {
      if (!s.Split(pos.para, pos.offset, why)) return s.Abort(why);
      pos = Pos{pos.para + 1, 0};
    }
    if (!s.InsertPlain(pos.para, pos.offset, lines[i], why)) return s.Abort(why);
    pos.offset += static_cast<uint32_t>(lines[i].size());
  }
  bool typing = lines.size() == 1;
  s.Commit(false, at, pos, typing ? Kind::Typing : Kind::Other, typing ? u"Typing" : u"Insert");
  s.Notify(listeners_);
  Result r;
  r.caret = pos;
  return r;
}

Result Editor::InsertDrawing(Pos at, std::string xml, const Drawing& info) {
  EditorState& s = *impl_;
  std::u16string why;
  if (!CanEdit(&why) || !s.Editable(at.para, why)) return Fail(why);
  if (at.offset > ParaLength(at.para)) return Fail(u"That place isn't in the paragraph.");
  if (!XmlSafe(xml)) return Fail(u"This picture can't be added.");
  s.Begin(at.para, 1);
  ElemId item = s.InsertObject(at.para, at.offset, Name("drawing"), std::move(xml), why);
  if (item == kNoElem) return s.Abort(why);
  s.AddDrawing(info, item, at.para);  // before Commit, which finds where it is
  Pos after{at.para, at.offset + 1};
  s.Commit(false, at, after, Kind::Other, u"Insert picture");
  s.Notify(listeners_);
  Result r;
  r.caret = after;
  return r;
}

Result Editor::Delete(Pos from, Pos to) {
  EditorState& s = *impl_;
  if (Compare(to, from) < 0) std::swap(from, to);
  std::u16string why;
  if (!CanEdit(&why) || !s.Editable(from.para, why) || !s.Editable(to.para, why)) return Fail(why);
  if (doc_.Para(from.para).story != doc_.Para(to.para).story)
    return Fail(u"A selection that leaves its story can't be deleted.");
  if (from == to) {
    Result r;
    r.caret = from;
    return r;
  }
  if (s.CutsField(from, to)) return Fail(u"Select the whole field to delete it.");
  // Whole tables between the ends go first, as Delete table takes them; then the text around them.
  if (to.para != from.para) {
    std::vector<uint32_t> tables;  // a paragraph of each table's own first cell
    for (ElemId e = s.E(doc_.Para(from.para).elem).next; e != kNoElem; e = s.E(e).next) {
      const uint32_t name = s.NameOf(e);
      if (RangeMarker(name)) continue;
      if (name == kP) {
        if (s.E(e).start >= to.para) break;
        continue;
      }
      uint32_t first = 0, count = 0;
      std::u16string w;
      if (name != kTbl || !s.TableParas(e, first, count, w) || first <= from.para || first + count > to.para) break;
      ElemId tr = s.E(e).first, tc = kNoElem, p = kNoElem;
      while (tr != kNoElem && s.NameOf(tr) != kTr) tr = s.E(tr).next;
      for (tc = tr == kNoElem ? kNoElem : s.E(tr).first; tc != kNoElem && s.NameOf(tc) != kTc;) tc = s.E(tc).next;
      if (tc == kNoElem || (p = s.FirstPara(tc)) == kNoElem) break;
      tables.push_back(s.E(p).start);
    }
    if (!tables.empty()) {
      BeginGroup(u"Delete");
      const size_t before = doc_.Paragraphs().size();
      for (auto it = tables.rbegin(); it != tables.rend(); ++it)
        if (Result r = DeleteTable(Pos{*it, 0}); !r) {
          CancelGroup();
          return r;
        }
      to.para = static_cast<uint32_t>(to.para + doc_.Paragraphs().size() - before);
      Result r = Delete(from, to);
      if (r) EndGroup();
      else CancelGroup();
      return r;
    }
  }
  // Across paragraphs: they must follow each other in one place (no table in between).
  std::vector<uint32_t> chain{from.para};
  if (to.para != from.para) {
    while (chain.back() != to.para) {
      uint32_t next = s.SiblingAfter(chain.back());
      if (next == kNone || next > to.para)
        return Fail(u"Deleting across a table or a content control isn't possible yet. Delete it separately.");
      chain.push_back(next);
    }
  }
  // Section breaks it takes (in the marks of all but the last paragraph): the section after them
  // keeps the headers and footers it showed.
  const KeptRefs kept = KeptReferences(doc_, std::vector<uint32_t>(chain.begin(), chain.end() - 1));
  if (kept.sectPr != kNoElem) BeginGroup(u"Delete");
  struct Ender {
    Editor& ed;
    bool on;
    ~Ender() {
      if (on) ed.EndGroup();
    }
  } ender{*this, kept.sectPr != kNoElem};
  bool single = from.para == to.para && to.offset == from.offset + 1;
  s.Begin(from.para, to.para - from.para + 1);
  if (from.para == to.para) {
    if (!s.DeleteIn(from.para, from.offset, to.offset, why)) return s.Abort(why);
  } else {
    // The last paragraph's head, the middle ones whole, the first one's tail, then join them.
    if (!s.DeleteIn(to.para, 0, to.offset, why)) return s.Abort(why);
    for (size_t i = chain.size() - 2; i >= 1; --i)
      if (!s.DeleteIn(chain[i], 0, ParaLength(chain[i]), why)) return s.Abort(why);
    if (!s.DeleteIn(from.para, from.offset, ParaLength(from.para), why)) return s.Abort(why);
    for (size_t i = 1; i < chain.size(); ++i)
      if (!s.Join(from.para, why)) return s.Abort(why);
  }
  s.Commit(false, to, from, single ? Kind::Deleting : Kind::Other, u"Delete");
  s.Notify(listeners_);
  AddReferences(kept, from);
  Result r;
  r.caret = from;
  return r;
}

void Editor::AddReferences(const KeptRefs& kept, Pos at) {
  if (kept.sectPr == kNoElem || kept.xml.empty()) return;
  TreeEdit te(*this, at);
  te.Begin(u"Delete");
  te.Affects(at.para, at.para);
  for (const std::string& xml : kept.xml) te.AddXml(kept.sectPr, xml);
  te.Commit();
}

Result Editor::Replace(Pos from, Pos to, std::u16string_view text) {
  if (from == to) return InsertText(from, text);
  if (Compare(to, from) < 0) std::swap(from, to);
  BeginGroup(u"Typing");
  Result r = Delete(from, to);
  if (r) r = InsertText(r.caret, text);
  if (r) EndGroup();
  else CancelGroup();  // the delete went through but the insert didn't: it goes again
  return r;
}

Result Editor::SplitParagraph(Pos at) {
  EditorState& s = *impl_;
  std::u16string why;
  if (!CanEdit(&why) || !s.Editable(at.para, why)) return Fail(why);
  if (at.offset > ParaLength(at.para)) return Fail(u"That place isn't in the paragraph.");
  s.Begin(at.para, 1);
  if (!s.Split(at.para, at.offset, why)) return s.Abort(why);
  Pos caret{at.para + 1, 0};
  s.Commit(false, at, caret, Kind::Other, u"New paragraph");
  s.Notify(listeners_);
  Result r;
  r.caret = caret;
  return r;
}

Result Editor::JoinParagraphs(uint32_t para) {
  EditorState& s = *impl_;
  std::u16string why;
  if (!CanEdit(&why) || !s.Editable(para, why)) return Fail(why);
  uint32_t q = s.SiblingAfter(para);
  if (q == kNone) return Fail(u"Paragraphs in different places (a table cell and the text around it) can't be joined.");
  Pos caret{para, ParaLength(para)};
  const KeptRefs kept = KeptReferences(doc_, {para});
  if (kept.sectPr != kNoElem) BeginGroup(u"Delete");
  s.Begin(para, q - para + 1);
  if (!s.Join(para, why)) {
    Result failed = s.Abort(why);
    if (kept.sectPr != kNoElem) EndGroup();
    return failed;
  }
  s.Commit(false, Pos{q, 0}, caret, Kind::Other, u"Delete");
  s.Notify(listeners_);
  AddReferences(kept, caret);
  if (kept.sectPr != kNoElem) EndGroup();
  Result r;
  r.caret = caret;
  return r;
}

Result Editor::SetRunProps(Pos from, Pos to, const PropEdit& edit) {
  EditorState& s = *impl_;
  if (Compare(to, from) < 0) std::swap(from, to);
  std::u16string why;
  if (!CanEdit(&why) || !s.Editable(from.para, why)) return Fail(why);
  Result r;
  r.caret = to;
  if (from == to) return r;
  uint32_t story = doc_.Para(from.para).story;
  s.Begin(from.para, to.para - from.para + 1);
  for (uint32_t p = from.para; p <= to.para; ++p) {
    if (doc_.Para(p).story != story || (s.E(doc_.Para(p).elem).flags & kDisplayOnly)) continue;
    uint32_t a = p == from.para ? from.offset : 0, b = p == to.para ? to.offset : ParaLength(p);
    if (a >= b) continue;
    if (!s.CutRunsAt(p, a, why) || !s.CutRunsAt(p, b, why)) return s.Abort(why);
    for (ElemId id : s.AllElems(p)) {
      if (s.NameOf(id) != kR || (s.E(id).flags & kDisplayOnly)) continue;
      uint32_t rs = s.E(id).start, rl = s.E(id).length;
      if (rl == 0 || rs < a || rs + rl > b) continue;
      if (!s.ApplyProps(id, kRPr, edit, why)) return s.Abort(why);
    }
    s.Reflow(p);
  }
  s.Commit(false, from, to, Kind::Other, u"Format");
  s.Notify(listeners_);
  return r;
}

Result Editor::SetLink(Pos from, Pos to, std::string_view target) {
  EditorState& s = *impl_;
  if (Compare(to, from) < 0) std::swap(from, to);
  std::u16string why;
  if (!CanEdit(&why) || !s.Editable(from.para, why)) return Fail(why);
  if (from.para != to.para || from.offset >= to.offset || target.empty())
    return Fail(u"Select text in one paragraph to make it a link.");
  if (!XmlSafe(target)) return Fail(u"A link's address can't hold control characters.");
  const uint32_t para = from.para;
  const ElemId p = doc_.Para(para).elem;
  for (const TextRun& t : doc_.Runs(para))
    if (t.end > from.offset && t.start < to.offset &&
        (!t.run || t.hyperlink || t.simpleField || t.contentControl || t.revision || t.fieldDepth > 0 || t.displayOnly ||
         t.run.Parent().Id() != p))
      return Fail(u"This text can't be made a link.");
  // A web address is a relationship of the part.
  std::string rid;
  if (target[0] != '#') {
    const uint32_t part = doc_.Stories()[doc_.Para(para).story].part;
    if (part >= doc_.Parts().size()) return Fail(u"This text can't be made a link.");
    const std::string& name = doc_.Parts()[part].name;
    const std::string type = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink";
    pkg::RelationshipSet set;
    if (const pkg::RelationshipSet* now = doc_.Package().RelationshipsOf(name)) set = *now;
    else set.source = name;
    for (const pkg::Relationship& r : set.items)
      if (r.external && r.type == type && r.target == target) rid = r.id;
    if (rid.empty()) {
      for (int n = 1; rid.empty(); ++n)
        if (std::string id = "rIdLink" + std::to_string(n); !set.Find(id)) rid = id;
      set.items.push_back(pkg::Relationship{rid, type, std::string(target), true, {}});
      doc_.Package().SetRelationships(std::move(set));
    }
  }
  s.Begin(para, 1);
  if (!s.CutRunsAt(para, from.offset, why) || !s.CutRunsAt(para, to.offset, why)) return s.Abort(why);
  // The runs (and range markers among them) in [from, to) go into a new w:hyperlink.
  std::vector<ElemId> inside;
  for (ElemId c = s.E(p).first; c != kNoElem; c = s.E(c).next) {
    if (s.PropsElem(c)) continue;
    const uint32_t cs = s.E(c).start, cl = s.E(c).length;
    if (cl == 0 ? (cs > from.offset && cs < to.offset) : (cs >= from.offset && cs + cl <= to.offset)) inside.push_back(c);
  }
  const ElemId h = inside.empty() ? kNoElem : s.NewElem(p, Name("hyperlink"));
  if (h == kNoElem) return s.Abort(u"This text can't be made a link.");
  if (!rid.empty()) s.SetAttr(h, Name("r:id"), rid);
  else s.SetAttr(h, Name("anchor"), target.substr(1));
  s.SetAttr(h, Name("history"), "1");
  s.LinkAfter(p, s.PrevSibling(inside.front()), h);
  for (ElemId c : inside) {
    s.Unlink(c);
    s.Append(h, c);
  }
  s.Reflow(para);
  s.Commit(false, from, to, Kind::Other, u"Link");
  s.Notify(listeners_);
  Result r;
  r.caret = to;
  return r;
}

Result Editor::SetMarkProps(uint32_t first, uint32_t last, const PropEdit& edit) {
  EditorState& s = *impl_;
  if (last < first) std::swap(first, last);
  std::u16string why;
  if (!CanEdit(&why) || !s.Editable(first, why)) return Fail(why);
  uint32_t story = doc_.Para(first).story;
  s.Begin(first, last - first + 1);
  for (uint32_t p = first; p <= last; ++p) {
    if (doc_.Para(p).story != story || (s.E(doc_.Para(p).elem).flags & kDisplayOnly)) continue;
    ElemId pPr = s.Child(doc_.Para(p).elem, kPPr);
    if (pPr == kNoElem) pPr = s.AddInOrder(doc_.Para(p).elem, kPPr);
    s.Unshare(pPr);
    if (!s.ApplyProps(pPr, kRPr, edit, why)) return s.Abort(why);
    s.Reflow(p);
  }
  // List labels take the paragraph mark's formatting.
  s.Commit(true, Pos{first, 0}, Pos{last, 0}, Kind::Other, u"Format");
  s.Notify(listeners_);
  Result r;
  r.caret = Pos{last, 0};
  return r;
}

Result Editor::SetParaProps(uint32_t first, uint32_t last, const PropEdit& edit) {
  EditorState& s = *impl_;
  if (last < first) std::swap(first, last);
  std::u16string why;
  if (!CanEdit(&why) || !s.Editable(first, why)) return Fail(why);
  uint32_t story = doc_.Para(first).story;
  s.Begin(first, last - first + 1);
  for (uint32_t p = first; p <= last; ++p) {
    if (doc_.Para(p).story != story || (s.E(doc_.Para(p).elem).flags & kDisplayOnly)) continue;
    if (!s.ApplyProps(doc_.Para(p).elem, kPPr, edit, why)) return s.Abort(why);
    s.Reflow(p);
  }
  s.Commit(true, Pos{first, 0}, Pos{last, 0}, Kind::Other, u"Format");
  s.Notify(listeners_);
  Result r;
  r.caret = Pos{last, 0};
  return r;
}

void Editor::BeginGroup(std::u16string name) {
  EditorState& s = *impl_;
  if (s.depth++ == 0) {
    s.group = {};
    s.group.name = std::move(name);
  }
  s.groupMarks.push_back(s.group.steps.size());
}

void Editor::CancelGroup() {
  EditorState& s = *impl_;
  if (s.depth == 0) return;
  const size_t mark = s.groupMarks.empty() ? 0 : s.groupMarks.back();
  for (size_t i = s.group.steps.size(); i-- > mark;) s.UndoStep(s.group.steps[i]);
  s.group.steps.resize(mark);
  s.typingBroken = true;
  EndGroup();
  s.Notify(listeners_);
}

void Editor::EndGroup() {
  EditorState& s = *impl_;
  if (s.depth == 0) return;
  if (!s.groupMarks.empty()) s.groupMarks.pop_back();
  if (--s.depth > 0) return;
  if (s.group.steps.empty()) return;
  Entry e = std::move(s.group);
  s.group = {};
  e.serial = ++s.serial;
  bool typing = e.kind == Kind::Typing;
  s.Count(e, 1);
  s.undo.push_back(std::move(e));
  s.typingBroken = !typing;
}

void Editor::BreakTyping() { impl_->typingBroken = true; }

void Editor::RecordStep(std::u16string name, std::shared_ptr<CustomStep> step, Pos caret) {
  if (!step) return;
  Step s;
  s.custom = std::move(step);
  s.structure = true;
  impl_->RefreshAll(*s.custom);
  impl_->Record(std::move(s), caret, caret, Kind::Other, std::move(name));
  impl_->Notify(listeners_);
}

pkg::NodeId Editor::NewNodeId() { return impl_->NewNode(); }

bool Editor::CanUndo() const { return !impl_->undo.empty(); }
bool Editor::CanRedo() const { return !impl_->redo.empty(); }
std::u16string Editor::UndoName() const { return impl_->undo.empty() ? u"" : impl_->undo.back().name; }
std::u16string Editor::RedoName() const { return impl_->redo.empty() ? u"" : impl_->redo.back().name; }

Result Editor::Undo() {
  Result r = impl_->UndoRedo(true);
  impl_->Notify(listeners_);
  return r;
}

Result Editor::Redo() {
  Result r = impl_->UndoRedo(false);
  impl_->Notify(listeners_);
  return r;
}

bool Editor::Modified() const {
  const EditorState& s = *impl_;
  return (s.undo.empty() ? 0 : s.undo.back().serial) != s.savedSerial;
}

void Editor::MarkSaved() {
  EditorState& s = *impl_;
  s.savedSerial = s.undo.empty() ? 0 : s.undo.back().serial;
  s.typingBroken = true;
}

std::vector<uint32_t> Editor::DirtyParts() const {
  std::vector<uint32_t> out;
  for (uint32_t i = 0; i < impl_->dirty.size(); ++i)
    if (impl_->dirty[i] > 0) out.push_back(i);
  return out;
}


// ---------- Tables (D3) ----------

// What a table change works on: the table read onto its grid and the block of cells; what it
// leaves: the blocks in the table's place (the table itself unless it went), paragraphs whose
// content changed besides their text (markers moved in), and the paragraph the caret goes to.
struct TableOp {
  EditorState& s;
  EditorState::Grid g;
  EditorState::Block bl;
  ElemId tbl = kNoElem;
  std::vector<ElemId> blocks;
  std::vector<ElemId> touched;
  ElemId caret = kNoElem;
  std::u16string why;
};

namespace {

using Grid = EditorState::Grid;
using GridCell = EditorState::GridCell;
using GridRow = EditorState::GridRow;

int32_t WidthOf(const GridCell& c, const std::vector<int32_t>& w) {
  int64_t sum = 0;
  for (int32_t k = c.col; k < c.col + c.span && k < static_cast<int32_t>(w.size()); ++k) sum += w[k];
  return static_cast<int32_t>(std::min<int64_t>(sum, 1000000));
}

// A cell's description for the view: where it is on the grid and its own text.
Editor::CellInfo Info(EditorState& s, const Grid& g, size_t r, int i) {
  const GridCell& c = g.rows[r].cells[i];
  Editor::CellInfo ci;
  ci.tbl = Node(&s.t, g.tbl);
  ci.tr = Node(&s.t, g.rows[r].tr);
  ci.tc = Node(&s.t, c.tc);
  ci.row = static_cast<uint32_t>(r);
  size_t top = r, bottom = r;
  s.MergeRows(g, r, i, top, bottom);
  ci.rows = c.v == VMerge::Restart ? static_cast<uint32_t>(bottom - r + 1) : 1;
  ci.col = c.col;
  ci.span = c.span;
  ci.tableRows = static_cast<uint32_t>(g.rows.size());
  ci.tableCols = g.width;
  std::vector<ElemId> ps;
  for (ElemId k = s.E(c.tc).first; k != kNoElem; k = s.E(k).next)
    if (s.NameOf(k) == kP) ps.push_back(k);
  if (ps.empty()) s.ParasIn(c.tc, ps);
  if (!ps.empty()) {
    ci.start = Pos{s.E(ps.front()).start, 0};
    uint32_t last = s.E(ps.back()).start;
    ci.end = Pos{last, static_cast<uint32_t>(s.d.Para(last).text.size())};
  }
  return ci;
}

// The cells of a grid in reading order, merged continuations left out: (row, index) pairs.
std::vector<std::pair<size_t, int>> ReadingOrder(const Grid& g) {
  std::vector<std::pair<size_t, int>> out;
  for (size_t r = 0; r < g.rows.size(); ++r)
    for (size_t i = 0; i < g.rows[r].cells.size(); ++i)
      if (g.rows[r].cells[i].v != VMerge::Continue) out.emplace_back(r, static_cast<int>(i));
  return out;
}

}  // namespace

bool EditorState::Listed(ElemId p) const {
  const uint32_t at = t.elems[p].start;
  return at < d.paras_.size() && d.paras_[at].elem == p;
}

Result Editor::EditTable(Pos from, Pos to, std::u16string name, const std::function<bool(TableOp&)>& op) {
  EditorState& s = *impl_;
  std::u16string why;
  if (!CanEdit(&why) || !s.Editable(from.para, why)) return Fail(why);
  TableOp o{s};
  if (!s.BlockOf(from, to, o.g, o.bl, why)) return Fail(why);
  o.tbl = o.g.tbl;
  uint32_t first = 0, count = 0;
  if (!s.TableParas(o.tbl, first, count, why)) return Fail(why);
  o.blocks = {o.tbl};
  s.Begin(first, count, {o.tbl});
  if (!op(o)) return s.Abort(o.why.empty() ? u"This table can't be changed that way." : o.why);
  s.Resync(o.blocks);
  for (ElemId p : o.touched)
    if (s.Listed(p)) s.Reflow(s.E(p).start);
  Pos caret = from;
  if (o.caret != kNoElem && s.Listed(o.caret)) caret = Pos{s.E(o.caret).start, 0};
  if (caret.para >= doc_.Paragraphs().size()) caret = Pos{first < doc_.Paragraphs().size() ? first : 0, 0};
  s.Commit(false, from, caret, Kind::Other, std::move(name));
  s.Notify(listeners_);
  Result r;
  r.caret = caret;
  return r;
}

std::optional<Editor::CellInfo> Editor::CellAt(Pos at) const {
  EditorState& s = *impl_;
  if (at.para >= doc_.Paragraphs().size()) return std::nullopt;
  ElemId tbl, tr, tc;
  if (!s.CellOf(at.para, tbl, tr, tc)) return std::nullopt;
  Grid g;
  std::u16string why;
  if (!s.ReadGrid(tbl, g, why)) return std::nullopt;
  size_t r = s.RowOf(g, tr);
  int i = r == SIZE_MAX ? -1 : s.CellIndex(g, r, tc);
  if (i < 0) return std::nullopt;
  return Info(s, g, r, i);
}

std::optional<Editor::CellBlock> Editor::BlockAt(Pos from, Pos to) const {
  EditorState& s = *impl_;
  if (from.para >= doc_.Paragraphs().size() || to.para >= doc_.Paragraphs().size()) return std::nullopt;
  Grid g;
  EditorState::Block bl;
  std::u16string why;
  if (!s.BlockOf(from, to, g, bl, why)) return std::nullopt;
  CellBlock b;
  b.tbl = Node(&s.t, g.tbl);
  b.row0 = static_cast<uint32_t>(bl.row0);
  b.row1 = static_cast<uint32_t>(bl.row1);
  b.col0 = bl.col0;
  b.col1 = bl.col1;
  for (size_t r = bl.row0; r <= bl.row1; ++r)
    for (size_t i = 0; i < g.rows[r].cells.size(); ++i) {
      const GridCell& c = g.rows[r].cells[i];
      if (c.v == VMerge::Continue || c.col > bl.col1 || c.col + c.span - 1 < bl.col0) continue;
      b.cells.push_back(Info(s, g, r, static_cast<int>(i)));
    }
  return b;
}

std::optional<std::pair<Pos, Pos>> Editor::Grow(Pos from, Pos to, Extent extent) const {
  EditorState& s = *impl_;
  if (from.para >= doc_.Paragraphs().size() || to.para >= doc_.Paragraphs().size()) return std::nullopt;
  Grid g;
  EditorState::Block bl;
  std::u16string why;
  if (!s.BlockOf(from, to, g, bl, why)) return std::nullopt;
  if (extent == Extent::Rows || extent == Extent::Table) bl.col0 = 0, bl.col1 = g.width - 1;
  if (extent == Extent::Columns || extent == Extent::Table) bl.row0 = 0, bl.row1 = g.rows.size() - 1;
  // The first cell the block's top row has in it, and the last its bottom row has (a merged one:
  // the cell it continues).
  auto cellIn = [&](size_t r, bool last) -> std::optional<CellInfo> {
    const auto& cs = g.rows[r].cells;
    for (size_t k = 0; k < cs.size(); ++k) {
      size_t i = last ? cs.size() - 1 - k : k;
      if (cs[i].col > bl.col1 || cs[i].col + cs[i].span - 1 < bl.col0) continue;
      size_t row = r, top = r, bottom = r;
      s.MergeRows(g, r, static_cast<int>(i), top, bottom);
      int j = static_cast<int>(i);
      if (cs[i].v == VMerge::Continue) {
        row = top;
        j = g.At(top, cs[i].col);
      }
      if (j < 0) return std::nullopt;
      return Info(s, g, row, j);
    }
    return std::nullopt;
  };
  auto a = cellIn(bl.row0, false), b = cellIn(bl.row1, true);
  if (!a || !b) return std::nullopt;
  return std::make_pair(a->start, b->end);
}

std::optional<Editor::CellInfo> Editor::NextCell(Pos at, bool back) const {
  EditorState& s = *impl_;
  if (at.para >= doc_.Paragraphs().size()) return std::nullopt;
  ElemId tbl, tr, tc;
  if (!s.CellOf(at.para, tbl, tr, tc)) return std::nullopt;
  Grid g;
  std::u16string why;
  if (!s.ReadGrid(tbl, g, why)) return std::nullopt;
  size_t r = s.RowOf(g, tr);
  int i = r == SIZE_MAX ? -1 : s.CellIndex(g, r, tc);
  if (i < 0) return std::nullopt;
  if (g.rows[r].cells[i].v == VMerge::Continue) {
    size_t top, bottom;
    s.MergeRows(g, r, i, top, bottom);
    i = g.At(top, g.rows[r].cells[i].col);
    r = top;
  }
  auto order = ReadingOrder(g);
  auto it = std::find(order.begin(), order.end(), std::make_pair(r, i));
  if (it == order.end()) return std::nullopt;
  if (back) {
    if (it == order.begin()) return std::nullopt;
    --it;
  } else if (++it == order.end()) {
    return std::nullopt;
  }
  return Info(s, g, it->first, it->second);
}

std::vector<Editor::CellInfo> Editor::TableCells(Pos at) const {
  EditorState& s = *impl_;
  std::vector<CellInfo> out;
  if (at.para >= doc_.Paragraphs().size()) return out;
  ElemId tbl, tr, tc;
  if (!s.CellOf(at.para, tbl, tr, tc)) return out;
  Grid g;
  std::u16string why;
  if (!s.ReadGrid(tbl, g, why)) return out;
  for (const auto& [r, i] : ReadingOrder(g)) out.push_back(Info(s, g, r, i));
  return out;
}

// ---- New tables

Result Editor::InsertTable(Pos at, uint32_t rows, uint32_t cols) {
  EditorState& s = *impl_;
  std::u16string why;
  if (!CanEdit(&why) || !s.Editable(at.para, why)) return Fail(why);
  if (at.offset > ParaLength(at.para)) return Fail(u"That place isn't in the paragraph.");
  if (rows < 1 || cols < 1 || cols > 63 || rows > 1000) return Fail(u"A table has 1 to 63 columns and 1 to 1,000 rows.");
  ElemId p = doc_.Para(at.para).elem;
  if (s.E(p).flags & kDisplayOnly) return Fail(u"A table can't go here.");
  if (s.InOneParagraphControl(at.para, at.offset)) return Fail(u"A table can't go in a date, list, plain-text or picture content control.");
  const int32_t width = s.AvailableWidth(at.para);
  s.Begin(at.para, 1);
  // Where it goes: before the paragraph at its start (unless a table is just before it), after it
  // at its end (unless nothing or a table follows), else between the halves of a split.
  ElemId parent = s.E(p).parent, after = kNoElem, second = kNoElem;
  const bool atStart = at.offset == 0, atEnd = at.offset == ParaLength(at.para);
  ElemId prevBlock = s.PrevBlock(p), nextBlock = s.NextBlock(p);
  if (atStart && !(prevBlock != kNoElem && s.NameOf(prevBlock) == kTbl)) {
    after = s.PrevSibling(p);
    second = p;
    p = kNoElem;
  } else if (atEnd && at.offset > 0 && nextBlock != kNoElem && s.NameOf(nextBlock) == kP &&
             !(s.Child(p, kPPr) != kNoElem && s.Child(s.Child(p, kPPr), kSectPr) != kNoElem)) {
    after = p;
  } else {
    if (!s.Split(at.para, at.offset, why)) return s.Abort(why);
    second = doc_.Para(at.para + 1).elem;
    after = p;
  }
  // The style: the document's "Table Grid", else single borders of its own.
  std::string style;
  for (const Style& st : doc_.Styles())
    if (st.type == StyleType::Table && (st.name == "Table Grid" || st.id == "TableGrid")) {
      style = std::string(st.id);
      break;
    }
  std::string xml = "<w:tbl><w:tblPr>";
  if (!style.empty()) xml += "<w:tblStyle w:val=\"" + style + "\"/>";
  xml += "<w:tblW w:w=\"0\" w:type=\"auto\"/>";
  if (style.empty()) {
    xml += "<w:tblBorders>";
    for (const char* edge : {"top", "left", "bottom", "right", "insideH", "insideV"})
      xml += std::string("<w:") + edge + " w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/>";
    xml += "</w:tblBorders>";
  }
  xml += "<w:tblLook w:val=\"04A0\"/></w:tblPr><w:tblGrid>";
  const int32_t col = std::max(width / static_cast<int32_t>(cols), 20);
  for (uint32_t c = 0; c < cols; ++c) xml += "<w:gridCol w:w=\"" + std::to_string(col) + "\"/>";
  xml += "</w:tblGrid></w:tbl>";
  ElemId tbl = s.Make(parent, after, xml);
  if (tbl == kNoElem) return s.Abort(u"A table can't go here.");
  s.roots.push_back(tbl);
  ElemId like = p != kNoElem ? p : second;
  ElemId lastRow = s.Child(tbl, kTblGrid);
  ElemId firstPara = kNoElem;
  for (uint32_t r = 0; r < rows; ++r) {
    ElemId tr = s.Make(tbl, lastRow, "<w:tr/>");
    if (tr == kNoElem) return s.Abort(u"A table can't go here.");
    lastRow = tr;
    ElemId lastCell = kNoElem;
    for (uint32_t c = 0; c < cols; ++c) {
      ElemId tc = s.Make(tr, lastCell, "<w:tc><w:tcPr><w:tcW w:w=\"" + std::to_string(col) + "\" w:type=\"dxa\"/></w:tcPr></w:tc>");
      if (tc == kNoElem) return s.Abort(u"A table can't go here.");
      ElemId cp = s.NewCellPara(tc, s.Child(tc, kTcPr), like);
      if (cp == kNoElem) return s.Abort(u"A table can't go here.");
      if (firstPara == kNoElem) firstPara = cp;
      lastCell = tc;
    }
  }
  std::vector<ElemId> blocks;
  if (p != kNoElem) blocks.push_back(p);
  blocks.push_back(tbl);
  if (second != kNoElem) blocks.push_back(second);
  s.Resync(blocks);
  Pos caret{s.E(firstPara).start, 0};
  s.Commit(false, at, caret, Kind::Other, u"Insert table");
  s.Notify(listeners_);
  Result r;
  r.caret = caret;
  return r;
}

Result Editor::TextToTable(uint32_t first, uint32_t last, char16_t separator) {
  EditorState& s = *impl_;
  std::u16string why;
  if (last < first) std::swap(first, last);
  if (!CanEdit(&why) || !s.Editable(first, why) || !s.Editable(last, why)) return Fail(why);
  // One place: paragraphs that follow each other, no table or section break among them.
  std::vector<uint32_t> chain{first};
  while (chain.back() != last) {
    uint32_t next = s.SiblingAfter(chain.back());
    if (next == kNone || next > last) return Fail(u"Select paragraphs that follow each other, without a table between them.");
    chain.push_back(next);
  }
  for (uint32_t p : chain) {
    ElemId pPr = s.Child(doc_.Para(p).elem, kPPr);
    if (pPr != kNoElem && s.Child(pPr, kSectPr) != kNoElem) return Fail(u"Paragraphs with a section break can't go into a table.");
    if (s.E(doc_.Para(p).elem).flags & kDisplayOnly) return Fail(u"These paragraphs can't go into a table.");
    if (s.InOneParagraphControl(p, std::nullopt)) return Fail(u"A table can't go in a date, list, plain-text or picture content control.");
  }
  const int32_t width = s.AvailableWidth(first);
  ElemId firstElem = doc_.Para(first).elem, parent = s.E(firstElem).parent;
  s.Begin(first, last - first + 1);
  // Each paragraph cut at its separators, from the end, so the indices before stay.
  std::vector<std::vector<ElemId>> rows(chain.size());
  for (size_t k = chain.size(); k-- > 0;) {
    const uint32_t para = chain[k];
    std::vector<uint32_t> cuts;
    const std::u16string& text = doc_.Para(para).text;
    for (uint32_t i = 0; i < text.size(); ++i)
      if (text[i] == separator) cuts.push_back(i);
    std::vector<ElemId> pieces;
    for (size_t c = cuts.size(); c-- > 0;) {
      if (!s.DeleteIn(para, cuts[c], cuts[c] + 1, why) || !s.Split(para, cuts[c], why)) return s.Abort(why);
      pieces.push_back(doc_.Para(para + 1).elem);
    }
    pieces.push_back(doc_.Para(para).elem);
    std::reverse(pieces.begin(), pieces.end());
    rows[k] = std::move(pieces);
  }
  size_t cols = 1;
  for (const auto& r : rows) cols = std::max(cols, r.size());
  if (cols > 63) return s.Abort(u"A table has at most 63 columns.");
  const int32_t col = std::max(width / static_cast<int32_t>(cols), 20);
  std::string style;
  for (const Style& st : doc_.Styles())
    if (st.type == StyleType::Table && (st.name == "Table Grid" || st.id == "TableGrid")) {
      style = std::string(st.id);
      break;
    }
  std::string xml = "<w:tbl><w:tblPr>";
  if (!style.empty()) xml += "<w:tblStyle w:val=\"" + style + "\"/>";
  xml += "<w:tblW w:w=\"0\" w:type=\"auto\"/>";
  if (style.empty()) {
    xml += "<w:tblBorders>";
    for (const char* edge : {"top", "left", "bottom", "right", "insideH", "insideV"})
      xml += std::string("<w:") + edge + " w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/>";
    xml += "</w:tblBorders>";
  }
  xml += "<w:tblLook w:val=\"04A0\"/></w:tblPr><w:tblGrid>";
  for (size_t c = 0; c < cols; ++c) xml += "<w:gridCol w:w=\"" + std::to_string(col) + "\"/>";
  xml += "</w:tblGrid></w:tbl>";
  ElemId tbl = s.Make(parent, s.PrevSibling(rows.front().front()), xml);
  if (tbl == kNoElem) return s.Abort(u"These paragraphs can't go into a table.");
  s.roots.push_back(tbl);
  ElemId lastRow = s.Child(tbl, kTblGrid);
  // Range markers between the paragraphs (a bookmark's end) go into the start of the next one.
  std::vector<std::vector<ElemId>> between(rows.size());
  for (size_t r = 1; r < rows.size(); ++r)
    for (ElemId m = s.E(rows[r - 1].back()).next; m != kNoElem && m != rows[r].front(); m = s.E(m).next) between[r].push_back(m);
  for (size_t r = 0; r < rows.size(); ++r) {
    ElemId tr = s.Make(tbl, lastRow, "<w:tr/>");
    lastRow = tr;
    ElemId lastCell = kNoElem;
    for (size_t c = 0; c < cols; ++c) {
      ElemId tc = s.Make(tr, lastCell, "<w:tc><w:tcPr><w:tcW w:w=\"" + std::to_string(col) + "\" w:type=\"dxa\"/></w:tcPr></w:tc>");
      if (tc == kNoElem) return s.Abort(u"These paragraphs can't go into a table.");
      if (c < rows[r].size()) {
        ElemId w = rows[r][c];
        s.Unlink(w);
        s.LinkAfter(tc, s.Child(tc, kTcPr), w);
        if (c == 0) {
          ElemId at = s.Child(w, kPPr);
          for (ElemId m : between[r]) {
            s.Unlink(m);
            s.LinkAfter(w, at, m);
            at = m;
          }
        }
      } else if (s.NewCellPara(tc, s.Child(tc, kTcPr), rows[r].back()) == kNoElem) {
        return s.Abort(u"These paragraphs can't go into a table.");
      }
      lastCell = tc;
    }
  }
  std::vector<ElemId> blocks{tbl};
  // A table ends neither its place nor touches another table: an empty paragraph after it.
  if (ElemId next = s.NextBlock(tbl); next == kNoElem || s.NameOf(next) == kTbl) {
    ElemId tail = s.NewElem(parent, kP);
    if (tail == kNoElem) return s.Abort(u"These paragraphs can't go into a table.");
    s.LinkAfter(parent, tbl, tail);
    blocks.push_back(tail);
  }
  s.Resync(blocks);
  for (size_t r = 0; r < rows.size(); ++r)
    if (s.Listed(rows[r].front())) s.Reflow(s.E(rows[r].front()).start);
  Pos caret{s.E(rows.front().front()).start, 0};
  s.Commit(false, Pos{first, 0}, caret, Kind::Other, u"Convert to table");
  s.Notify(listeners_);
  Result r;
  r.caret = caret;
  return r;
}

Result Editor::TableToText(Pos at, char16_t separator) {
  EditorState& s = *impl_;
  std::u16string why;
  if (!CanEdit(&why) || !s.Editable(at.para, why)) return Fail(why);
  Grid g;
  EditorState::Block bl;
  if (!s.BlockOf(at, at, g, bl, why)) return Fail(why);
  const ElemId tbl = g.tbl, parent = s.E(tbl).parent;
  uint32_t first = 0, count = 0;
  if (!s.TableParas(tbl, first, count, why)) return Fail(why);
  // Each cell's content: paragraphs only (a table inside one can't become text here).
  for (const GridRow& row : g.rows)
    for (const GridCell& c : row.cells)
      for (ElemId k = s.E(c.tc).first; k != kNoElem; k = s.E(k).next) {
        uint32_t n = s.NameOf(k);
        if (n != kP && n != kTcPr && !RangeMarker(n)) return Fail(u"A table with tables or content controls in its cells can't become text yet.");
      }
  s.Begin(first, count, {tbl});
  // Out of the table, in order, before it: then cells of a row are joined. Range markers between
  // rows or cells (a bookmark's end before </w:tbl>) come out in their places too (finding 21).
  ElemId after = s.PrevSibling(tbl);
  std::vector<std::vector<std::pair<ElemId, ElemId>>> rows;  // per row: each cell's first and last paragraph
  std::vector<ElemId> moved;
  auto out = [&](ElemId k) {
    s.Unlink(k);
    s.LinkAfter(parent, after, k);
    after = k;
    moved.push_back(k);
  };
  std::vector<ElemId> outer;
  for (ElemId k = s.E(tbl).first; k != kNoElem; k = s.E(k).next) outer.push_back(k);
  for (ElemId k : outer) {
    if (RangeMarker(s.NameOf(k))) {
      out(k);
      continue;
    }
    if (s.NameOf(k) != kTr) continue;
    rows.emplace_back();
    std::vector<ElemId> inner;
    for (ElemId c = s.E(k).first; c != kNoElem; c = s.E(c).next) inner.push_back(c);
    for (ElemId c : inner) {
      if (RangeMarker(s.NameOf(c))) {
        out(c);
        continue;
      }
      if (s.NameOf(c) != kTc) continue;
      ElemId firstP = kNoElem, lastP = kNoElem;
      std::vector<ElemId> kids;
      for (ElemId e = s.E(c).first; e != kNoElem; e = s.E(e).next)
        if (s.NameOf(e) != kTcPr) kids.push_back(e);
      for (ElemId e : kids) {
        out(e);
        if (s.NameOf(e) == kP) {
          if (firstP == kNoElem) firstP = e;
          lastP = e;
        }
      }
      if (firstP != kNoElem) rows.back().emplace_back(firstP, lastP);
    }
  }
  s.Unlink(tbl);
  // Paragraphs between the cells' paragraphs were siblings in the table's paragraph order, so the
  // indices still hold; joins go from the end.
  for (size_t r = rows.size(); r-- > 0;)
    for (size_t c = rows[r].size(); c-- > 1;) {
      const uint32_t para = s.E(rows[r][c - 1].second).start;
      if (!s.InsertPlain(para, static_cast<uint32_t>(doc_.Para(para).text.size()), std::u16string(1, separator), why) ||
          !s.Join(para, why))
        return s.Abort(why);
    }
  // The moved paragraphs the joins left (a joined one is no longer in its parent).
  std::unordered_set<ElemId> linked;
  for (ElemId c = s.E(parent).first; c != kNoElem; c = s.E(c).next) linked.insert(c);
  std::vector<ElemId> kept;
  for (ElemId k : moved)
    if (s.NameOf(k) == kP && linked.count(k)) kept.push_back(k);
  s.Resync(kept);
  Pos caret{kept.empty() ? first : s.E(kept.front()).start, 0};
  s.Commit(false, at, caret, Kind::Other, u"Convert to text");
  s.Notify(listeners_);
  Result r;
  r.caret = caret;
  return r;
}

Result Editor::DeleteTable(Pos at) {
  EditorState& s = *impl_;
  std::u16string why;
  if (!CanEdit(&why) || !s.Editable(at.para, why)) return Fail(why);
  ElemId tbl, tr, tc;
  if (!s.CellOf(at.para, tbl, tr, tc)) return Fail(u"Put the cursor in a table first.");
  uint32_t first = 0, count = 0;
  if (!s.TableParas(tbl, first, count, why)) return Fail(why);
  const ElemId parent = s.E(tbl).parent;
  ElemId next = s.NextBlock(tbl), prev = s.PrevBlock(tbl);
  // The paragraph after it takes what must stay (a bookmark's end); with none, or between two
  // tables, an empty paragraph takes the table's place.
  const bool nextPara = next != kNoElem && s.NameOf(next) == kP && s.E(next).start == first + count && s.Listed(next);
  s.Begin(first, count + (nextPara ? 1 : 0), {tbl});
  ElemId heir = nextPara ? next : kNoElem;
  ElemId fresh = kNoElem;
  if (!nextPara || (prev != kNoElem && s.NameOf(prev) == kTbl)) {
    fresh = s.NewElem(parent, kP);
    if (fresh == kNoElem) return s.Abort(u"This table can't be deleted.");
    s.LinkAfter(parent, tbl, fresh);
    if (heir == kNoElem) heir = fresh;
  }
  if (!s.Evacuate({tbl}, heir, why)) return s.Abort(why);
  s.Unlink(tbl);
  std::vector<ElemId> blocks;
  if (fresh != kNoElem) blocks.push_back(fresh);
  if (nextPara) blocks.push_back(next);
  s.Resync(blocks);
  if (s.Listed(heir)) s.Reflow(s.E(heir).start);
  Pos caret{s.E(blocks.front()).start, 0};
  s.Commit(false, at, caret, Kind::Other, u"Delete table");
  s.Notify(listeners_);
  Result r;
  r.caret = caret;
  return r;
}

// ---- Rows and columns

Result Editor::InsertRows(Pos from, Pos to, bool below) {
  return EditTable(from, to, below ? u"Insert rows below" : u"Insert rows above", [&](TableOp& o) {
    EditorState& s = o.s;
    const Grid& g = o.g;
    const size_t n = o.bl.row1 - o.bl.row0 + 1, ref = below ? o.bl.row1 : o.bl.row0;
    const size_t next = below ? ref + 1 : ref;  // the row after the new ones
    const std::vector<int32_t> widths = s.Widths(g);
    const GridRow& row = g.rows[ref];
    static const uint32_t kHeader = Name("tblHeader");
    const bool header = Node(&s.t, row.tr).Child(kTrPr).Child(kHeader) &&
                        (!below || (next < g.rows.size() && Node(&s.t, g.rows[next].tr).Child(kTrPr).Child(kHeader)));
    ElemId after = below ? row.tr : s.PrevSibling(row.tr);
    ElemId firstNew = kNoElem;
    for (size_t k = 0; k < n; ++k) {
      ElemId tr = s.NewElem(o.tbl, kTr);
      if (tr == kNoElem) return false;
      ElemId last = kNoElem;
      for (uint32_t name : {Name("tblPrEx"), kTrPr}) {
        ElemId src = s.Child(row.tr, name);
        if (src == kNoElem) continue;
        auto skip = [&](ElemId c) {
          uint32_t cn = s.NameOf(c);
          return cn == kIns || cn == kDel || cn == Name("trPrChange") || cn == Name("tblPrExChange") || (cn == kHeader && !header);
        };
        ElemId copy = s.CopyProps(src, tr, skip, false);
        s.LinkAfter(tr, last, copy);
        last = copy;
      }
      // tblPrEx goes before trPr in the schema; copied in that order above.
      for (const GridCell& c : row.cells) {
        VMerge v = VMerge::None;
        if (next < g.rows.size()) {
          int j = g.At(next, c.col);
          if (j >= 0 && g.rows[next].cells[j].col == c.col && g.rows[next].cells[j].span == c.span &&
              g.rows[next].cells[j].v == VMerge::Continue)
            v = VMerge::Continue;
        }
        ElemId tc = s.NewCell(tr, last, c.tc, c.span, v, WidthOf(c, widths));
        if (tc == kNoElem) return false;
        last = tc;
      }
      s.LinkAfter(o.tbl, after, tr);
      after = tr;
      if (firstNew == kNoElem) firstNew = tr;
    }
    // The caret: the first new row's cell in the caret's column (else its first cell).
    for (ElemId c = s.E(firstNew).first; c != kNoElem && o.caret == kNoElem; c = s.E(c).next)
      if (s.NameOf(c) == kTc) o.caret = s.FirstPara(c);
    Grid now;
    std::u16string ignore;
    if (s.ReadGrid(o.tbl, now, ignore)) {
      size_t r = s.RowOf(now, firstNew);
      int i = r == SIZE_MAX ? -1 : now.At(r, o.bl.col0);
      if (i >= 0) o.caret = s.FirstPara(now.rows[r].cells[i].tc);
    }
    return true;
  });
}

Result Editor::InsertColumns(Pos from, Pos to, bool right) {
  return EditTable(from, to, right ? u"Insert columns right" : u"Insert columns left", [&](TableOp& o) {
    EditorState& s = o.s;
    const Grid& g = o.g;
    const int32_t n = o.bl.col1 - o.bl.col0 + 1, b = right ? o.bl.col1 + 1 : o.bl.col0;
    if (g.width + n > 63) return o.why = u"A table has at most 63 columns.", false;
    std::vector<int32_t> widths = s.Widths(g);
    std::vector<int32_t> added(widths.begin() + o.bl.col0, widths.begin() + o.bl.col1 + 1);
    const ElemId caretRow = g.rows[o.bl.row0].tr;
    for (const GridRow& row : g.rows) {
      if (b < row.before) {
        s.SetRowGrid(row.tr, Name("gridBefore"), row.before + n);
        continue;
      }
      if (b > row.End()) {
        s.SetRowGrid(row.tr, Name("gridAfter"), row.after + n);
        continue;
      }
      // A cell across the boundary covers the new columns too.
      bool across = false;
      for (const GridCell& c : row.cells)
        if (c.col < b && b < c.col + c.span) {
          s.SetSpan(c.tc, c.span + n);
          across = true;
        }
      if (across) continue;
      const GridCell* before = nullptr;
      const GridCell* after = nullptr;
      for (const GridCell& c : row.cells) {
        if (c.col + c.span == b) before = &c;
        if (c.col == b) after = &c;
      }
      const GridCell* like = right ? (before ? before : after) : (after ? after : before);
      ElemId at = before ? before->tc : s.PrevSibling(row.cells.front().tc);
      for (int32_t k = 0; k < n; ++k) {
        ElemId tc = s.NewCell(row.tr, at, like ? like->tc : kNoElem, 1, VMerge::None, added[k]);
        if (tc == kNoElem) return false;
        if (row.tr == caretRow && k == 0) o.caret = s.FirstPara(tc);
        at = tc;
      }
    }
    // Wider than the page now, but not before: every column narrows in proportion.
    int64_t was = 0, now = 0;
    for (int32_t w : widths) was += w;
    widths.insert(widths.begin() + b, added.begin(), added.end());
    for (int32_t w : widths) now += w;
    const int32_t room = s.RoomFor(o.tbl, s.tx.first);
    if (now > room && was <= room && now > 0)
      for (int32_t& w : widths) w = static_cast<int32_t>(static_cast<int64_t>(w) * room / now);
    s.WriteGrid(o.tbl, widths);
    return true;
  });
}

Result Editor::DeleteRows(Pos from, Pos to) {
  {
    EditorState& s = *impl_;
    Grid g;
    EditorState::Block bl;
    std::u16string why;
    if (s.BlockOf(from, to, g, bl, why) && bl.row0 == 0 && bl.row1 + 1 == g.rows.size()) return DeleteTable(from);
  }
  return EditTable(from, to, u"Delete rows", [&](TableOp& o) {
    EditorState& s = o.s;
    const Grid& g = o.g;
    std::vector<ElemId> gone;
    for (size_t r = o.bl.row0; r <= o.bl.row1; ++r) gone.push_back(g.rows[r].tr);
    const size_t heirRow = o.bl.row1 + 1 < g.rows.size() ? o.bl.row1 + 1 : o.bl.row0 - 1;
    int i = g.At(heirRow, o.bl.col0);
    if (i < 0) i = 0;
    std::vector<ElemId> ps;
    s.ParasIn(g.rows[heirRow].cells[i].tc, ps);
    ElemId heir = ps.empty() ? kNoElem : ps.front();
    if (!s.Evacuate(gone, heir, o.why)) return false;
    for (ElemId tr : gone) s.Drop(tr);
    s.FixMerges(o.tbl);
    o.touched.push_back(heir);
    o.caret = heir;
    return true;
  });
}

Result Editor::DeleteColumns(Pos from, Pos to) {
  {
    EditorState& s = *impl_;
    Grid g;
    EditorState::Block bl;
    std::u16string why;
    if (s.BlockOf(from, to, g, bl, why) && bl.col0 == 0 && bl.col1 + 1 >= g.width) return DeleteTable(from);
  }
  return EditTable(from, to, u"Delete columns", [&](TableOp& o) {
    EditorState& s = o.s;
    const Grid& g = o.g;
    const int32_t c0 = o.bl.col0, c1 = o.bl.col1;
    auto overlap = [&](int32_t a, int32_t b) { return std::max(0, std::min(b, c1 + 1) - std::max(a, c0)); };
    std::vector<ElemId> rowsGone;
    ElemId anyHeir = kNoElem;
    struct Plan {
      const GridRow* row;
      std::vector<ElemId> gone;
      ElemId heir = kNoElem;
    };
    std::vector<Plan> plans;
    for (const GridRow& row : g.rows) {
      Plan p{&row, {}, kNoElem};
      for (const GridCell& c : row.cells) {
        int32_t ov = overlap(c.col, c.col + c.span);
        if (ov == c.span) {
          p.gone.push_back(c.tc);
        } else if (p.heir == kNoElem || c.col < c0) {
          std::vector<ElemId> ps;
          s.ParasIn(c.tc, ps);
          if (!ps.empty()) p.heir = ps.front();
        }
      }
      if (p.heir != kNoElem && anyHeir == kNoElem) anyHeir = p.heir;
      plans.push_back(std::move(p));
    }
    for (Plan& p : plans) {
      if (p.gone.size() == p.row->cells.size()) {
        if (!s.Evacuate({p.row->tr}, anyHeir, o.why)) return false;
        rowsGone.push_back(p.row->tr);
        continue;
      }
      if (!s.Evacuate(p.gone, p.heir, o.why)) return false;
      if (p.heir != kNoElem) o.touched.push_back(p.heir);
      for (ElemId tc : p.gone) s.Drop(tc);
      const GridRow& row = *p.row;
      for (const GridCell& c : row.cells) {
        int32_t ov = overlap(c.col, c.col + c.span);
        if (ov > 0 && ov < c.span) s.SetSpan(c.tc, c.span - ov);
      }
      if (int32_t ov = overlap(0, row.before)) s.SetRowGrid(row.tr, Name("gridBefore"), row.before - ov);
      if (int32_t ov = overlap(row.End(), row.End() + row.after)) s.SetRowGrid(row.tr, Name("gridAfter"), row.after - ov);
    }
    if (rowsGone.size() == g.rows.size()) return o.why = u"This would delete the whole table.", false;
    for (ElemId tr : rowsGone) s.Drop(tr);
    if (anyHeir != kNoElem) o.touched.push_back(anyHeir);
    std::vector<int32_t> widths = s.Widths(g);
    widths.erase(widths.begin() + c0, widths.begin() + std::min<int32_t>(c1 + 1, static_cast<int32_t>(widths.size())));
    s.WriteGrid(o.tbl, widths);
    s.FixMerges(o.tbl);
    // The caret: the caret's row, at the column that took the deleted ones' place.
    Grid now;
    if (s.ReadGrid(o.tbl, now, o.why)) {
      size_t r = s.RowOf(now, g.rows[o.bl.row0].tr);
      if (r != SIZE_MAX) {
        int i = now.At(r, std::min(c0, now.width - 1));
        if (i < 0) i = static_cast<int>(now.rows[r].cells.size()) - 1;
        o.caret = s.FirstPara(now.rows[r].cells[i].tc);
      }
    }
    if (o.caret == kNoElem) o.caret = anyHeir;
    return true;
  });
}

Result Editor::ClearCells(Pos from, Pos to) {
  return EditTable(from, to, u"Delete", [&](TableOp& o) {
    EditorState& s = o.s;
    for (size_t r = o.bl.row0; r <= o.bl.row1; ++r)
      for (const GridCell& c : o.g.rows[r].cells) {
        if (c.col > o.bl.col1 || c.col + c.span - 1 < o.bl.col0) continue;
        ElemId keep = s.FirstPara(c.tc);
        if (keep == kNoElem) continue;
        const uint32_t para = s.E(keep).start;
        if (!s.DeleteIn(para, 0, static_cast<uint32_t>(doc_.Para(para).text.size()), o.why)) return false;
        std::vector<ElemId> rest;
        for (ElemId k = s.E(c.tc).first; k != kNoElem; k = s.E(k).next)
          if (k != keep && s.NameOf(k) != kTcPr && !RangeMarker(s.NameOf(k))) rest.push_back(k);
        if (!s.Evacuate(rest, keep, o.why)) return false;
        for (ElemId k : rest) s.Unlink(k);
        o.touched.push_back(keep);
        if (o.caret == kNoElem) o.caret = keep;
      }
    return true;
  });
}

// ---- Merging and splitting

Result Editor::MergeCells(Pos from, Pos to) {
  return EditTable(from, to, u"Merge cells", [&](TableOp& o) {
    EditorState& s = o.s;
    const Grid& g = o.g;
    const auto& bl = o.bl;
    // Every row of the block has cells covering exactly its columns.
    std::vector<std::vector<GridCell>> cells;
    for (size_t r = bl.row0; r <= bl.row1; ++r) {
      cells.emplace_back();
      for (const GridCell& c : g.rows[r].cells)
        if (c.col <= bl.col1 && c.col + c.span - 1 >= bl.col0) cells.back().push_back(c);
      if (cells.back().empty() || cells.back().front().col != bl.col0 ||
          cells.back().back().col + cells.back().back().span - 1 != bl.col1)
        return o.why = u"These cells can't be merged: their rows don't line up.", false;
    }
    if (cells.size() == 1 && cells[0].size() == 1) return o.why = u"Select two or more cells to merge.", false;
    const ElemId target = cells[0][0].tc;
    auto empty = [&](ElemId tc) {
      ElemId only = kNoElem;
      for (ElemId k = s.E(tc).first; k != kNoElem; k = s.E(k).next) {
        if (s.NameOf(k) == kTcPr) continue;
        if (only != kNoElem || s.NameOf(k) != kP) return false;
        only = k;
      }
      if (only == kNoElem) return true;
      for (ElemId k = s.E(only).first; k != kNoElem; k = s.E(k).next)
        if (s.NameOf(k) != kPPr) return false;
      return true;
    };
    // The cells' content, in reading order, into the first one (empty cells add nothing).
    bool targetEmpty = empty(target);
    for (size_t r = 0; r < cells.size(); ++r)
      for (size_t i = 0; i < cells[r].size(); ++i) {
        ElemId tc = cells[r][i].tc;
        if (tc == target || empty(tc)) continue;
        if (targetEmpty) {
          for (ElemId k; (k = s.FirstPara(target)) != kNoElem;) s.Unlink(k);
          targetEmpty = false;
        }
        std::vector<ElemId> kids;
        for (ElemId k = s.E(tc).first; k != kNoElem; k = s.E(k).next)
          if (s.NameOf(k) != kTcPr) kids.push_back(k);
        for (ElemId k : kids) {
          s.Unlink(k);
          s.Append(target, k);
        }
      }
    // The first row: one cell across the block; the rows below: one cell continuing it.
    const int32_t span = bl.col1 - bl.col0 + 1;
    for (size_t r = 0; r < cells.size(); ++r) {
      ElemId keep = cells[r][0].tc;
      for (size_t i = 1; i < cells[r].size(); ++i) s.Drop(cells[r][i].tc);
      s.SetSpan(keep, span);
      s.SetVMerge(keep, cells.size() == 1 ? VMerge::None : r == 0 ? VMerge::Restart : VMerge::Continue);
      if (r > 0 && s.FirstPara(keep) == kNoElem) {
        ElemId last = s.Child(keep, kTcPr);
        for (ElemId k = s.E(keep).first; k != kNoElem; k = s.E(k).next) last = k;
        if (s.NewCellPara(keep, last, s.FirstPara(target)) == kNoElem) return false;
      }
    }
    // A cell ends with a paragraph.
    ElemId lastKid = kNoElem;
    for (ElemId k = s.E(target).first; k != kNoElem; k = s.E(k).next) lastKid = k;
    if (lastKid == kNoElem || s.NameOf(lastKid) != kP) {
      ElemId any = kNoElem;
      std::vector<ElemId> ps;
      s.ParasIn(target, ps);
      if (!ps.empty()) any = ps.back();
      if (s.NewCellPara(target, lastKid, any) == kNoElem) return false;
    }
    s.WriteGrid(o.tbl, s.Widths(g));
    s.FixMerges(o.tbl);
    o.caret = s.FirstPara(target);
    return true;
  });
}

Result Editor::SplitCell(Pos at, uint32_t rows, uint32_t cols) {
  if (rows < 1 || cols < 1 || rows > 100 || cols > 63) return Fail(u"Split a cell into 1 to 100 rows and 1 to 63 columns.");
  if (rows == 1 && cols == 1) {
    Result r;
    r.caret = at;
    return r;
  }
  return EditTable(at, at, u"Split cells", [&](TableOp& o) {
    EditorState& s = o.s;
    ElemId tbl, tr, tc;
    s.CellOf(at.para, tbl, tr, tc);
    size_t r = s.RowOf(o.g, tr);
    int i = r == SIZE_MAX ? -1 : s.CellIndex(o.g, r, tc);
    if (i < 0) return false;
    size_t top, bottom;
    s.MergeRows(o.g, r, i, top, bottom);
    const GridCell cell = o.g.rows[r].cells[i];
    const ElemId first = o.g.rows[top].cells[o.g.At(top, cell.col)].tc;
    const size_t height = bottom - top + 1;
    std::vector<ElemId> pieceRows;  // the rows the pieces' cells are in, top to bottom
    if (rows > 1 && height > 1) {
      if (height % rows != 0) return o.why = u"A merged cell splits into a number of rows that divides its own.", false;
      const size_t group = height / rows;
      for (size_t k = 0; k < height; ++k) {
        ElemId c = o.g.rows[top + k].cells[o.g.At(top + k, cell.col)].tc;
        s.SetVMerge(c, group == 1 ? VMerge::None : k % group == 0 ? VMerge::Restart : VMerge::Continue);
      }
      for (size_t k = 0; k < height; ++k) pieceRows.push_back(o.g.rows[top + k].tr);
    } else if (rows > 1) {
      // New rows below: the other cells of the row run down over them; this one is cut.
      const std::vector<int32_t> widths = s.Widths(o.g);
      const GridRow& row = o.g.rows[r];
      for (const GridCell& c : row.cells)
        if (c.tc != cell.tc && c.v == VMerge::None) s.SetVMerge(c.tc, VMerge::Restart);
      ElemId after = row.tr;
      pieceRows.push_back(row.tr);
      for (uint32_t k = 1; k < rows; ++k) {
        ElemId ntr = s.NewElem(o.tbl, kTr);
        if (ntr == kNoElem) return false;
        ElemId last = kNoElem;
        if (ElemId trPr = s.Child(row.tr, kTrPr); trPr != kNoElem) {
          auto skip = [&](ElemId c) {
            uint32_t cn = s.NameOf(c);
            return cn == kIns || cn == kDel || cn == Name("trPrChange");
          };
          last = s.CopyProps(trPr, ntr, skip, false);
          s.LinkAfter(ntr, kNoElem, last);
        }
        for (const GridCell& c : row.cells) {
          ElemId nc = s.NewCell(ntr, last, c.tc, c.span, c.tc == cell.tc ? VMerge::None : VMerge::Continue, WidthOf(c, widths));
          if (nc == kNoElem) return false;
          last = nc;
        }
        s.LinkAfter(o.tbl, after, ntr);
        after = ntr;
        pieceRows.push_back(ntr);
      }
    } else {
      for (size_t k = 0; k < height; ++k) pieceRows.push_back(o.g.rows[top + k].tr);
    }
    o.caret = s.FirstPara(first);
    if (cols == 1) return true;
    // Columns: the cell's span cut into `cols` pieces; grid lines added where it has too few.
    if (!s.ReadGrid(o.tbl, o.g, o.why)) return false;
    std::vector<int32_t> widths = s.Widths(o.g);
    int32_t span = cell.span;
    if (span < static_cast<int32_t>(cols)) {
      if (o.g.width + static_cast<int32_t>(cols) - span > 63) return o.why = u"A table has at most 63 columns.", false;
      // The cell's width cut evenly: each new line splits the grid column it falls in.
      int64_t left = 0;
      for (int32_t k = 0; k < cell.col; ++k) left += widths[k];
      int64_t total = 0;
      for (int32_t k = cell.col; k < cell.col + span; ++k) total += widths[k];
      for (uint32_t piece = 1; piece < cols; ++piece) {
        const int64_t x = left + total * piece / cols;
        int64_t edge = 0;
        int32_t g = 0;
        while (g < static_cast<int32_t>(widths.size()) && edge + widths[g] <= x) edge += widths[g++];
        if (edge == x || g >= static_cast<int32_t>(widths.size())) continue;
        const int32_t a = static_cast<int32_t>(x - edge), b = widths[g] - a;
        widths[g] = a;
        widths.insert(widths.begin() + g + 1, b);
        // Grid column g is now two: everything that covered it covers both.
        for (const GridRow& row : o.g.rows) {
          if (g < row.before) {
            s.SetRowGrid(row.tr, Name("gridBefore"), row.before + 1);
            continue;
          }
          if (g >= row.End()) {
            if (g < row.End() + row.after) s.SetRowGrid(row.tr, Name("gridAfter"), row.after + 1);
            continue;
          }
          for (const GridCell& c : row.cells)
            if (c.col <= g && g < c.col + c.span) s.SetSpan(c.tc, c.span + 1);
        }
        s.WriteGrid(o.tbl, widths);
        if (!s.ReadGrid(o.tbl, o.g, o.why)) return false;
      }
    }
    // Each row of the piece: its cell cut at the grid lines nearest the even cuts.
    if (!s.ReadGrid(o.tbl, o.g, o.why)) return false;
    widths = s.Widths(o.g);
    for (ElemId ptr : pieceRows) {
      size_t pr = s.RowOf(o.g, ptr);
      if (pr == SIZE_MAX) return false;
      int pi = o.g.At(pr, cell.col);
      if (pi < 0) return false;
      const GridCell c = o.g.rows[pr].cells[pi];
      int64_t total = WidthOf(c, widths);
      std::vector<int32_t> spans;
      int32_t col = c.col, start = c.col;
      int64_t acc = 0;
      for (uint32_t piece = 1; piece <= cols; ++piece) {
        const int64_t x = total * piece / cols;
        while (col < c.col + c.span && acc + widths[col] / 2 < x) acc += widths[col++];
        if (piece == cols) col = c.col + c.span;
        spans.push_back(std::max(1, col - start));
        start = col;
      }
      // Too few grid columns for the rounding: give each piece one at least, from the end.
      int32_t sum = 0;
      for (int32_t sp : spans) sum += sp;
      for (size_t k = spans.size(); sum > c.span && k-- > 0;)
        while (spans[k] > 1 && sum > c.span) --spans[k], --sum;
      s.SetSpan(c.tc, spans[0]);
      ElemId after = c.tc;
      int32_t at2 = c.col + spans[0];
      for (size_t k = 1; k < spans.size(); ++k) {
        GridCell part{c.tc, at2, spans[k], c.v};
        ElemId nc = s.NewCell(o.g.rows[pr].tr, after, c.tc, spans[k], c.v, WidthOf(part, widths));
        if (nc == kNoElem) return false;
        after = nc;
        at2 += spans[k];
      }
    }
    s.WriteGrid(o.tbl, widths);
    s.FixMerges(o.tbl);
    return true;
  });
}

// ---- Sizes

Result Editor::SetColumnWidth(Pos from, Pos to, int32_t twips) {
  if (twips < 20 || twips > 31680) return Fail(u"A column is between 0.01 and 22 inches wide.");
  return EditTable(from, to, u"Column width", [&](TableOp& o) {
    std::vector<int32_t> widths = o.s.Widths(o.g);
    for (int32_t c = o.bl.col0; c <= o.bl.col1 && c < static_cast<int32_t>(widths.size()); ++c) widths[c] = twips;
    o.s.WriteGrid(o.tbl, widths);
    o.caret = kNoElem;
    return true;
  });
}

Result Editor::MoveColumnBorder(Pos at, int32_t line, int32_t delta) {
  return EditTable(at, at, u"Column width", [&](TableOp& o) {
    EditorState& s = o.s;
    std::vector<int32_t> widths = s.Widths(o.g);
    const int32_t n = static_cast<int32_t>(widths.size());
    constexpr int32_t kMin = 144;  // a tenth of an inch: room for the cell's margins
    if (line < 0 || line > n || n == 0) return o.why = u"There's no column border there.", false;
    if (line == n) {
      widths[n - 1] = std::max(kMin, widths[n - 1] + delta);
    } else if (line == 0) {
      // The left edge: the table moves in by its indent, and the first column gives the room.
      const int32_t d = std::clamp(delta, -100000, widths[0] - kMin);
      widths[0] -= d;
      ElemId tblPr = s.Props(o.tbl, kTblPr);
      Node ind = Node(&s.t, tblPr).Child(Name("tblInd"));
      int32_t now = ind && (ind.AttrOr(kType) == "dxa" || ind.AttrOr(kType).empty()) ? EditorState::IntOr(ind, kW, 0) : 0;
      s.SetProp(tblPr, "<w:tblInd w:w=\"" + std::to_string(now + d) + "\" w:type=\"dxa\"/>");
    } else {
      const int32_t d = std::clamp(delta, kMin - widths[line - 1], widths[line] - kMin);
      widths[line - 1] += d;
      widths[line] -= d;
    }
    s.WriteGrid(o.tbl, widths);
    return true;
  });
}

Result Editor::SetRowHeight(Pos from, Pos to, int32_t twips, std::string_view rule) {
  if (twips < 0 || twips > 31680) return Fail(u"A row is at most 22 inches high.");
  if (!rule.empty() && rule != "atLeast" && rule != "exact") return Fail(u"A row's height is at least or exactly a size.");
  return EditTable(from, to, u"Row height", [&](TableOp& o) {
    EditorState& s = o.s;
    for (size_t r = o.bl.row0; r <= o.bl.row1; ++r) {
      ElemId tr = o.g.rows[r].tr;
      if (twips == 0) {
        if (ElemId trPr = s.Child(tr, kTrPr); trPr != kNoElem) {
          s.Unshare(trPr);
          s.RemoveProp(trPr, Name("trHeight"));
        }
        continue;
      }
      std::string xml = "<w:trHeight w:val=\"" + std::to_string(twips) + "\"";
      if (!rule.empty()) xml += " w:hRule=\"" + std::string(rule) + "\"";
      if (s.SetProp(s.Props(tr, kTrPr), xml + "/>") == kNoElem) return false;
    }
    return true;
  });
}

Result Editor::AutoFit(Pos at, Fit fit, const std::vector<int32_t>& given) {
  return EditTable(at, at, u"AutoFit", [&](TableOp& o) {
    EditorState& s = o.s;
    std::vector<int32_t> widths = s.Widths(o.g);
    ElemId tblPr = s.Props(o.tbl, kTblPr);
    if (tblPr == kNoElem) return false;
    static const uint32_t kLayout = Name("tblLayout");
    switch (fit) {
      case Fit::Window: {
        const int32_t room = s.RoomFor(o.tbl, s.tx.first);
        int64_t now = 0;
        for (int32_t w : widths) now += w;
        if (now <= 0) return false;
        int64_t used = 0;
        for (size_t k = 0; k < widths.size(); ++k) {
          widths[k] = k + 1 == widths.size() ? static_cast<int32_t>(room - used) : static_cast<int32_t>(widths[k] * room / now);
          used += widths[k];
        }
        s.RemoveProp(tblPr, kLayout);
        s.SetProp(tblPr, "<w:tblW w:w=\"5000\" w:type=\"pct\"/>");
        break;
      }
      case Fit::Fixed: {
        int64_t sum = 0;
        for (int32_t w : widths) sum += w;
        s.SetProp(tblPr, "<w:tblLayout w:type=\"fixed\"/>");
        s.SetProp(tblPr, "<w:tblW w:w=\"" + std::to_string(sum) + "\" w:type=\"dxa\"/>");
        break;
      }
      case Fit::Contents: {
        if (given.size() != widths.size()) return o.why = u"The table's columns couldn't be measured.", false;
        for (size_t k = 0; k < widths.size(); ++k) widths[k] = std::clamp(given[k], 144, 31680);
        s.RemoveProp(tblPr, kLayout);
        s.SetProp(tblPr, "<w:tblW w:w=\"0\" w:type=\"auto\"/>");
        break;
      }
    }
    s.WriteGrid(o.tbl, widths);
    return true;
  });
}

// ---- Properties

Result Editor::SetCellProps(Pos from, Pos to, const PropEdit& edit) {
  return EditTable(from, to, u"Format cells", [&](TableOp& o) {
    for (size_t r = o.bl.row0; r <= o.bl.row1; ++r)
      for (const GridCell& c : o.g.rows[r].cells) {
        if (c.col > o.bl.col1 || c.col + c.span - 1 < o.bl.col0) continue;
        if (!o.s.ApplyProps(c.tc, kTcPr, edit, o.why)) return false;
      }
    return true;
  });
}

Result Editor::SetRowProps(Pos from, Pos to, const PropEdit& edit) {
  return EditTable(from, to, u"Format rows", [&](TableOp& o) {
    for (size_t r = o.bl.row0; r <= o.bl.row1; ++r)
      if (!o.s.ApplyProps(o.g.rows[r].tr, kTrPr, edit, o.why)) return false;
    return true;
  });
}

Result Editor::SetTableProps(Pos at, const PropEdit& edit) {
  return EditTable(at, at, u"Format table", [&](TableOp& o) { return o.s.ApplyProps(o.tbl, kTblPr, edit, o.why); });
}

Result Editor::SetCellBorders(Pos from, Pos to, CellBorders which, std::string_view style, int32_t eighths,
                              std::string_view color) {
  if (!XmlSafe(style) || !XmlSafe(color) || eighths < 2 || eighths > 96) return Fail(u"That isn't a border Word knows.");
  return EditTable(from, to, u"Borders", [&](TableOp& o) {
    const auto& bl = o.bl;
    for (size_t r = bl.row0; r <= bl.row1; ++r)
      for (size_t i = 0; i < o.g.rows[r].cells.size(); ++i) {
        const GridCell& c = o.g.rows[r].cells[i];
        if (c.col > bl.col1 || c.col + c.span - 1 < bl.col0) continue;
        size_t top = r, bottom = r;
        o.s.MergeRows(o.g, r, static_cast<int>(i), top, bottom);
        // Which of the cell's sides are on the block's edge.
        const bool edgeTop = top <= bl.row0, edgeBottom = bottom >= bl.row1;
        const bool edgeLeft = c.col <= bl.col0, edgeRight = c.col + c.span - 1 >= bl.col1;
        // Each side: on, none ("nil", over the table's), or left as it is.
        auto act = [&](bool edge, CellBorders alone) {
          switch (which) {
            case CellBorders::All: return 1;
            case CellBorders::None: return 0;
            case CellBorders::Outside: return edge ? 1 : -1;
            case CellBorders::Inside: return edge ? -1 : 1;
            default: return which == alone && edge ? 1 : -1;
          }
        };
        const std::pair<const char*, int> sides[] = {{"top", act(edgeTop, CellBorders::Top)},
                                                     {"left", act(edgeLeft, CellBorders::Left)},
                                                     {"bottom", act(edgeBottom, CellBorders::Bottom)},
                                                     {"right", act(edgeRight, CellBorders::Right)}};
        PropEdit edit;
        for (const auto& [name, a] : sides) {
          if (a < 0) continue;
          if (a == 0)
            edit.Set(std::string("tcBorders/") + name, {{"val", "nil"}});
          else
            edit.Set(std::string("tcBorders/") + name,
                     {{"val", std::string(style)}, {"sz", std::to_string(eighths)}, {"space", "0"}, {"color", std::string(color)}});
        }
        if (!edit.items.empty() && !o.s.ApplyProps(c.tc, kTcPr, edit, o.why)) return false;
      }
    return true;
  });
}

}  // namespace fw::doc::edit
