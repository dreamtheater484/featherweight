#include "doc/edit/Editor.h"

#include <algorithm>
#include <cstdio>
#include <unordered_set>

#include "doc/edit/Kept.h"
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
    if (E(id).flags & kSharedKids) return id;
    for (ElemId c = E(src).first; c != kNoElem; c = E(c).next)
      if (!skip || !skip(c)) Append(id, CloneDeep(c, id, skip, fragments));
    return id;
  }
  // Gives a property container its own children before they change (Tree.h, kSharedKids).
  void Unshare(ElemId container) {
    if (!(E(container).flags & kSharedKids)) return;
    ElemId shared = E(container).first;
    E(container).flags &= static_cast<uint8_t>(~kSharedKids);
    E(container).first = kNoElem;
    for (ElemId c = shared; c != kNoElem; c = E(c).next) Append(container, CloneDeep(c, container));
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
    if ((E(rPr).flags & kSharedKids) && !dropLinkStyle && NameOf(fromRunOrPPr) == kR) {
      // A run's shared set: share it too (shared sets hold plain properties only).
      copy = CloneShallow(rPr, toRun);
    } else {
      // The paragraph mark's w:rPr is a schema type of its own: its children are made again for a run's.
      copy = NewElem(toRun, kRPr);
      if (copy == kNoElem) return;
      for (ElemId c = E(rPr).first; c != kNoElem; c = E(c).next) {
        if (skip(c)) continue;
        ElemDefId def = S().Child(TypeOf(copy), NameOf(c));
        if (def == kNoDef) continue;
        ElemId k = CloneDeep(c, copy);
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
      std::vector<ElemId> stack{w};
      while (!stack.empty()) {
        ElemId id = stack.back();
        stack.pop_back();
        add(id);
        if (E(id).flags & kSharedKids) continue;
        for (ElemId c = E(id).first; c != kNoElem; c = E(c).next) stack.push_back(c);
      }
    }
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

  bool Begin(uint32_t first, uint32_t count) {
    if (inTx) return false;
    inTx = true;
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
    d.caches_ = std::make_unique<Document::Caches>();
    d.BuildIndexes();
    for (Paragraph& p : d.paras_) p.version = ++version;
    Change ch;
    if (step.first != kNone && step.count > 0 && step.first + step.count <= d.paras_.size()) {
      ch.story = d.paras_[step.first].story;
      ch.first = step.first;
      ch.removed = ch.added = step.count;
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
    for (Section& s : d.sections_) {
      if (s.endPara == kNone) continue;
      if (s.endPara >= first && s.endPara < first + removed) s.endPara = d.ParagraphOf(s.sectPr);
      else if (s.endPara >= first + removed) s.endPara = static_cast<uint32_t>(static_cast<int64_t>(s.endPara) + shift);
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
      for (auto it = e.steps.rbegin(); it != e.steps.rend(); ++it) {
        if (it->custom) {
          it->custom->Undo();
          RefreshAll(*it->custom);
          continue;
        }
        Restore(it->before, it->first, static_cast<uint32_t>(it->after.paras.size()));
        Refresh(*it, true);
      }
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

  // Inserts text without paragraph breaks; tabs and line breaks become w:tab and w:br.
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
    auto hasSpecial = std::any_of(text.begin(), text.end(), [](char16_t c) { return c == u'\t' || c == 0x0B; });
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
      if (ch == u'\t' || ch == 0x0B) {
        ElemId item = NewElem(run, ch == u'\t' ? kTab : kBr);
        E(item).length = 1;
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
      while (j < text.size() && text[j] != u'\t' && text[j] != 0x0B) ++j;
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
            if (PropsElem(k)) Append(copy, CloneDeep(k, copy));
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
      if (E(pPr).flags & kSharedKids) {
        copy = CloneShallow(pPr, p);
      } else {
        auto skip = [&](ElemId c) {
          uint32_t n = NameOf(c);
          return n == kSectPr || n == kPPrChange || n == kIns || n == kDel || n == kMoveFrom || n == kMoveTo ||
                 n == kRPrChange;
        };
        copy = CloneDeep(pPr, p, skip, false);
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
    if (pPr != kNoElem && Child(pPr, kSectPr) != kNoElem) {
      why = u"Deleting a section break isn't possible yet.";
      return false;
    }
    if ((E(p).flags & kDisplayOnly) || (E(w).flags & kDisplayOnly)) {
      why = u"This part of the document can't be edited.";
      return false;
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
            if (!SetAttr(child, Name(attr), value)) {
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
      if (PropsElem(k)) Append(copy, CloneDeep(k, copy));
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
    } else if ((c == u'\t' || c == 0x0B || c >= 0x20) && c != 0xFFFC) {  // U+FFFC stands for objects
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
  for (size_t i = 0; i + 1 < chain.size(); ++i) {
    ElemId pPr = s.Child(doc_.Para(chain[i]).elem, kPPr);
    if (pPr != kNoElem && s.Child(pPr, kSectPr) != kNoElem) return Fail(u"Deleting a section break isn't possible yet.");
  }
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
  Result r;
  r.caret = from;
  return r;
}

Result Editor::Replace(Pos from, Pos to, std::u16string_view text) {
  if (from == to) return InsertText(from, text);
  if (Compare(to, from) < 0) std::swap(from, to);
  size_t entries = impl_->undo.size();
  BeginGroup(u"Typing");
  Result r = Delete(from, to);
  if (r) r = InsertText(r.caret, text);
  EndGroup();
  if (!r && impl_->undo.size() > entries) {  // the delete went through but the insert didn't
    Undo();
    impl_->redo.pop_back();
  }
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
  s.Begin(para, q - para + 1);
  if (!s.Join(para, why)) return s.Abort(why);
  s.Commit(false, Pos{q, 0}, caret, Kind::Other, u"Delete");
  s.Notify(listeners_);
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
}

void Editor::EndGroup() {
  EditorState& s = *impl_;
  if (s.depth == 0 || --s.depth > 0) return;
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

}  // namespace fw::doc::edit
