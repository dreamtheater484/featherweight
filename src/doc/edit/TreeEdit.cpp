#include "doc/edit/TreeEdit.h"

#include <pugixml.hpp>

#include <algorithm>

#include "doc/docx/Docx.h"
#include "doc/edit/Editor.h"
#include "doc/edit/Kept.h"
#include "doc/model/Internal.h"
#include "pkg/XmlWriter.h"

namespace fw::doc::edit {

namespace {

const Schema& S() { return Schema::Get(); }

uint32_t NameOfElem(const Tree& t, ElemId id) { return S().Elem(t.elems[id].def).name; }
TypeId TypeOfElem(const Tree& t, ElemId id) { return S().Elem(t.elems[id].def).type; }

// Where a child goes among its siblings: its place in the parent type's schema order.
size_t Rank(const TypeDef& parent, ElemDefId def) {
  auto it = std::find(parent.children.begin(), parent.children.end(), def);
  return static_cast<size_t>(it - parent.children.begin());
}

size_t AttrRank(const TypeDef& type, AttrDefId def) {
  auto it = std::find(type.attrs.begin(), type.attrs.end(), def);
  return static_cast<size_t>(it - type.attrs.begin());
}

// Pre-order successor inside `top`'s subtree, as Document walks it: shared property sets aren't
// entered.
ElemId NextIn(const Tree& t, ElemId id, ElemId top, bool skipChildren = false) {
  if (!skipChildren && !(t.elems[id].flags & kSharedKids) && t.elems[id].first != kNoElem) return t.elems[id].first;
  while (id != top && id != kNoElem) {
    if (t.elems[id].next != kNoElem) return t.elems[id].next;
    id = t.elems[id].parent;
  }
  return kNoElem;
}

Ns NsOfPrefix(std::string_view prefix) {
  for (size_t i = 0; i < kNsCount; ++i)
    if (NsPrefix(static_cast<Ns>(i)) == prefix && static_cast<Ns>(i) != Ns::Other) return static_cast<Ns>(i);
  return Ns::Other;
}

// "w:val" -> the name hash the schema knows it by.
uint32_t QNameHash(std::string_view qname) {
  size_t colon = qname.find(':');
  if (colon == std::string_view::npos) return NameOf(Ns::None, qname);
  return NameOf(NsOfPrefix(qname.substr(0, colon)), qname.substr(colon + 1));
}

bool Space(char16_t c) { return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r'; }

}  // namespace

// The journal of one change, as the editor's history keeps it.
struct JournalStep : CustomStep {
  explicit JournalStep(Document& d) : doc(d) {}
  void Undo() override {
    TreeEdit::Replay(doc, store, true);
    TreeEdit::Restore(doc, before);
  }
  void Redo() override {
    TreeEdit::Restore(doc, after);
    TreeEdit::Replay(doc, store, false);
  }
  Document& doc;
  TreeEdit::Saved before, after;  // after: the touched elements and the new ones
  std::vector<TreeEdit::StoreOp> store;
};

// The editor holds the document it edits; it hands it out as const only to readers.
TreeEdit::TreeEdit(Editor& editor, Pos caret)
    : editor_(editor), doc_(const_cast<Document&>(editor.Doc())), caret_(caret) {}

pkg::NodeId TreeEdit::NewNode() { return editor_.NewNodeId(); }

// ---------- Transactions ----------

void TreeEdit::Begin(std::u16string label) {
  if (depth_++ > 0) return;
  label_ = std::move(label);
  local_ = toEnd_ = false;
  owned_.clear();
  elemsAtBegin_ = doc_.tree_.elems.size();
  before_.clear();
  store_.clear();
  touched_.clear();
}

void TreeEdit::Commit() {
  if (depth_ == 0 || --depth_ > 0) return;
  Tree& t = doc_.tree_;
  touched_.clear();
  if (before_.empty() && store_.empty() && t.elems.size() == elemsAtBegin_) return;
  auto step = std::make_shared<JournalStep>(doc_);
  step->before = std::move(before_);
  for (const auto& [id, old] : step->before) step->after.emplace_back(id, t.elems[id]);
  for (size_t id = elemsAtBegin_; id < t.elems.size(); ++id) step->after.emplace_back(static_cast<ElemId>(id), t.elems[id]);
  step->store = std::move(store_);
  step->parts = PartsOf(step->after);
  before_.clear();
  store_.clear();
  // The paragraphs it changed: for the caret, and when it's local, for the layout.
  uint32_t first = kNone, last = 0, story = kNone;
  bool local = local_;
  for (const auto& [id, e] : step->after) {
    const uint32_t p = ParagraphOf(id);
    if (p == kNone) {
      local = local && InDefinitions(id);
      continue;
    }
    if (story == kNone) story = doc_.paras_[p].story;
    local = local && doc_.paras_[p].story == story;
    first = std::min(first, p);
    last = std::max(last, p);
  }
  Pos caret = caret_;
  if (first != kNone) {
    if (caret.para == kNone) caret = {first, 0};
    if (local) {
      // Its neighbours too: borders and spacing join paragraphs. (A story's paragraphs can have
      // a text box's between them; the layout passes over those.)
      if (first > 0 && doc_.paras_[first - 1].story == story) --first;
      if (toEnd_) last = std::max(last, doc_.StoryParagraphs(story).back());
      else if (last + 1 < doc_.paras_.size() && doc_.paras_[last + 1].story == story) ++last;
      step->first = first;
      step->count = last - first + 1;
    }
  }
  editor_.RecordStep(std::move(label_), std::move(step), caret);
}

bool TreeEdit::InDefinitions(ElemId id) const {
  const Tree& t = doc_.tree_;
  for (int hops = 0; hops < 256 && t.elems[id].parent != kNoElem; ++hops) id = t.elems[id].parent;
  for (const Part& part : doc_.parts_)
    if (part.root == id) return part.kind == PartKind::Styles || part.kind == PartKind::Numbering;
  return false;
}

uint32_t TreeEdit::ParagraphOf(ElemId id) const {
  const Tree& t = doc_.tree_;
  for (int hops = 0; id != kNoElem && hops < 256; ++hops, id = t.elems[id].parent)
    if (S().Elem(t.elems[id].def).name == names::kP) return t.elems[id].start;
  return kNone;
}

void TreeEdit::Restore(Document& doc, const Saved& elems) {
  for (auto it = elems.rbegin(); it != elems.rend(); ++it) doc.tree_.elems[it->first] = it->second;
}

void TreeEdit::Replay(Document& doc, const std::vector<StoreOp>& ops, bool undo) {
  pkg::RoundTripStore& store = doc.store_;
  if (undo) {
    for (auto op = ops.rbegin(); op != ops.rend(); ++op) {
      if (op->kind == StoreOp::Split) store.Merge(op->node, op->second, op->at.firstMoved);
      else if (op->kind == StoreOp::SetAttrs) store.SetNodeAttributes(op->node, op->before);
      else store.SetFragments(op->node, op->fragmentsBefore);
    }
  } else {
    for (const StoreOp& op : ops) {
      if (op.kind == StoreOp::Split) store.Split(op.node, op.second, op.at);
      else if (op.kind == StoreOp::SetAttrs) store.SetNodeAttributes(op.node, op.after);
      else store.SetFragments(op.node, op.fragmentsAfter);
    }
  }
}

// The parts the elements are in: up their parents to a part's root.
std::vector<uint32_t> TreeEdit::PartsOf(const Saved& elems) const {
  std::vector<uint32_t> parts;
  std::unordered_map<ElemId, uint32_t> roots;
  for (uint32_t i = 0; i < doc_.parts_.size(); ++i) roots.emplace(doc_.parts_[i].root, i);
  for (const auto& [id, e] : elems) {
    ElemId top = id;
    for (int hops = 0; hops < 256 && doc_.tree_.elems[top].parent != kNoElem; ++hops) top = doc_.tree_.elems[top].parent;
    auto it = roots.find(top);
    if (it != roots.end() && std::find(parts.begin(), parts.end(), it->second) == parts.end()) parts.push_back(it->second);
  }
  return parts;
}

void TreeEdit::Reindex() {
  doc_.caches_ = std::make_unique<Document::Caches>();
  doc_.BuildIndexes();
}

Elem& TreeEdit::Touch(ElemId id) {
  if (id < elemsAtBegin_ && touched_.find(id) == touched_.end()) {
    touched_.emplace(id, before_.size());
    before_.emplace_back(id, doc_.tree_.elems[id]);
  }
  return doc_.tree_.elems[id];
}

void TreeEdit::StoreSplit(pkg::NodeId node, pkg::NodeId second, pkg::SplitPoint at) {
  doc_.store_.Split(node, second, at);
  StoreOp op;
  op.kind = StoreOp::Split;
  op.node = node;
  op.second = second;
  op.at = at;
  store_.push_back(op);
}

void TreeEdit::StoreSetAttrs(pkg::NodeId node, pkg::AttrSetId set) {
  StoreOp op;
  op.kind = StoreOp::SetAttrs;
  op.node = node;
  op.before = doc_.store_.NodeAttributes(node);
  op.after = set;
  doc_.store_.SetNodeAttributes(node, set);
  store_.push_back(op);
}

// ---------- Elements ----------

ElemId TreeEdit::Clone(ElemId src, ElemId parent) {
  Elem copy = E(src);
  copy.parent = parent;
  copy.next = kNoElem;
  if (copy.first != kNoElem) copy.flags |= kSharedKids;  // its own children stay shared with src's
  pkg::NodeId from = copy.node;
  copy.node = NewNode();
  doc_.tree_.elems.push_back(copy);
  auto id = static_cast<ElemId>(doc_.tree_.elems.size() - 1);
  if (from != pkg::kNoNode && from != pkg::kFieldNode) {
    if (pkg::AttrSetId set = doc_.store_.NodeAttributes(from)) StoreSetAttrs(copy.node, set);
    // Kept content beside it goes with it (the w14 extensions after a w:sz).
    if (std::vector<pkg::Fragment> kept = doc_.store_.FragmentsCopy(from); !kept.empty())
      StoreFragments(copy.node, std::move(kept));
  }
  return id;
}

bool TreeEdit::Repeats(ElemId first, ElemId c) const {
  const Tree& t = doc_.tree_;
  const Elem& a = t.elems[c];
  if (a.first != kNoElem) return false;
  for (ElemId s = first; s != kNoElem && s != c; s = t.elems[s].next) {
    const Elem& b = t.elems[s];
    if (b.def != a.def || b.attrCount != a.attrCount || b.first != kNoElem ||
        doc_.store_.NodeAttributes(b.node) != doc_.store_.NodeAttributes(a.node))
      continue;
    bool same = true;
    for (uint16_t i = 0; same && i < a.attrCount; ++i)
      same = t.attrs[a.attrs + i].def == t.attrs[b.attrs + i].def && t.attrs[a.attrs + i].value == t.attrs[b.attrs + i].value;
    if (same) return true;
  }
  return false;
}

bool TreeEdit::HasRepeats(ElemId e) const {
  const Tree& t = doc_.tree_;
  for (ElemId c = t.elems[e].first; c != kNoElem; c = t.elems[c].next)
    if (Repeats(t.elems[e].first, c) || HasRepeats(c)) return true;
  return false;
}

ElemId TreeEdit::CopyProps(ElemId src, ElemId parent) {
  if (!HasRepeats(src)) return Clone(src, parent);
  const ElemId copy = Clone(src, parent);
  E(copy).flags = static_cast<uint8_t>(E(copy).flags & ~kSharedKids);
  E(copy).first = kNoElem;
  ElemId tail = kNoElem;
  for (ElemId c = E(src).first; c != kNoElem; c = E(c).next) {
    if (Repeats(E(src).first, c)) continue;
    const ElemId k = CopyProps(c, copy);
    if (tail == kNoElem) E(copy).first = k;
    else E(tail).next = k;
    tail = k;
  }
  return copy;
}

ElemId TreeEdit::Own(ElemId container) {
  // The reader shares one set of children between identical property containers, flagging every
  // one but the first, whose children they are (Tree.h, kSharedKids). So a container of those
  // kinds copies its children before they change, once a transaction (d2-torture finding 4); the
  // others keep the originals. A flagged one always does (Clone shares too). Anything else owns
  // its children: copying a paragraph's runs would share their text elements, which the editor
  // and the model's walks then skip (d2-torture findings 7, 8 and 9). One made in this
  // transaction, unflagged, has children of its own.
  if (E(container).first == kNoElem) return container;
  const bool flagged = (E(container).flags & kSharedKids) != 0;
  if (!flagged && (container >= elemsAtBegin_ || !SharesChildren(NameOfElem(doc_.tree_, container)))) return container;
  if (!owned_.insert(container).second) return container;
  std::vector<ElemId> kids;
  for (ElemId k = E(container).first; k != kNoElem; k = E(k).next) kids.push_back(k);
  Touch(container);
  E(container).first = kNoElem;
  E(container).flags = static_cast<uint8_t>(E(container).flags & ~kSharedKids);
  ElemId prev = kNoElem;
  for (ElemId k : kids) {
    ElemId c = Clone(k, container);
    if (prev == kNoElem) E(container).first = c;
    else E(prev).next = c;
    prev = c;
  }
  return container;
}

void TreeEdit::Link(ElemId parent, ElemId child, ElemId before) {
  ElemId prev = kNoElem;
  if (before == kNoElem) {
    const TypeDef& pt = S().Type(TypeOfElem(doc_.tree_, parent));
    size_t rank = Rank(pt, E(child).def);
    for (ElemId c = E(parent).first; c != kNoElem; c = E(c).next) {
      if (Rank(pt, E(c).def) > rank) break;
      prev = c;
    }
  } else {
    for (ElemId c = E(parent).first; c != kNoElem && c != before; c = E(c).next) prev = c;
  }
  if (prev == kNoElem) {
    ElemId first = Touch(parent).first;
    E(child).next = first;
    E(parent).first = child;
  } else {
    ElemId next = Touch(prev).next;
    E(child).next = next;
    E(prev).next = child;
  }
  E(child).parent = parent;
  // Kept content stays in schema order: after a w:rPr, the w14 extensions that end w:rPr last.
  for (auto& [node, fragments] : FragmentsAfterLink(doc_.store_, doc_.tree_, prev, child))
    StoreFragments(node, std::move(fragments));
}

void TreeEdit::StoreFragments(pkg::NodeId node, std::vector<pkg::Fragment> fragments) {
  StoreOp op;
  op.kind = StoreOp::Fragments;
  op.node = node;
  op.fragmentsBefore = doc_.store_.FragmentsCopy(node);
  op.fragmentsAfter = fragments;
  doc_.store_.SetFragments(node, std::move(fragments));
  store_.push_back(std::move(op));
}

ElemId TreeEdit::Insert(ElemId parent, ElemDefId def, ElemId before) {
  Elem e;
  e.def = def;
  e.parent = parent;
  e.node = NewNode();
  doc_.tree_.elems.push_back(e);
  auto id = static_cast<ElemId>(doc_.tree_.elems.size() - 1);
  Link(parent, id, before);
  return id;
}

ElemId TreeEdit::Child(ElemId parent, uint32_t name, bool create) {
  if (parent == kNoElem) return kNoElem;
  Own(parent);
  for (ElemId c = E(parent).first; c != kNoElem; c = E(c).next)
    if (NameOfElem(doc_.tree_, c) == name) return c;
  if (!create) return kNoElem;
  ElemDefId def = S().Child(TypeOfElem(doc_.tree_, parent), name);
  if (def == kNoDef) return kNoElem;
  return Insert(parent, def, kNoElem);
}

ElemId TreeEdit::Path(ElemId parent, std::initializer_list<uint32_t> names, bool create) {
  ElemId e = parent;
  for (uint32_t n : names) {
    e = Child(e, n, create);
    if (e == kNoElem) return kNoElem;
  }
  return e;
}

void TreeEdit::RemoveElement(ElemId elem) {
  ElemId parent = E(elem).parent;
  if (parent == kNoElem) return;
  ElemId prev = kNoElem;
  for (ElemId c = E(parent).first; c != kNoElem && c != elem; c = E(c).next) prev = c;
  ElemId next = E(elem).next;
  if (prev == kNoElem) {
    if (E(parent).first != elem) return;  // not a child of its parent (a shared set's member)
    Touch(parent).first = next;
  } else {
    Touch(prev).next = next;
  }
  // Kept content beside it stays where it was: after the previous known child, before the next,
  // or first in the parent.
  for (auto& [node, fragments] : FragmentsBeforeUnlink(doc_.store_, doc_.tree_, prev, elem, next))
    StoreFragments(node, std::move(fragments));
}

void TreeEdit::Remove(ElemId parent, uint32_t name) {
  if (parent == kNoElem) return;
  if (!(E(parent).flags & kSharedKids)) {
    bool has = false;
    for (ElemId c = E(parent).first; c != kNoElem; c = E(c).next) has |= NameOfElem(doc_.tree_, c) == name;
    if (!has) return;
  }
  ElemId c = Child(parent, name, false);
  if (c != kNoElem) RemoveElement(c);
}

void TreeEdit::Clear(ElemId container, std::initializer_list<uint32_t> keep) {
  if (container == kNoElem || E(container).first == kNoElem) return;
  Own(container);
  std::vector<ElemId> kids;
  for (ElemId c = E(container).first; c != kNoElem; c = E(c).next) kids.push_back(c);
  for (ElemId c : kids)
    if (std::find(keep.begin(), keep.end(), NameOfElem(doc_.tree_, c)) == keep.end()) RemoveElement(c);
}

void TreeEdit::WriteAttrs(ElemId elem, const std::vector<AttrVal>& attrs) {
  Tree& t = doc_.tree_;
  auto first = static_cast<uint32_t>(t.attrs.size());
  t.attrs.insert(t.attrs.end(), attrs.begin(), attrs.end());
  Elem& e = Touch(elem);
  e.attrs = attrs.empty() ? 0 : first;
  e.attrCount = static_cast<uint16_t>(attrs.size());
}

void TreeEdit::SetAttr(ElemId elem, uint32_t name, std::string_view value) {
  Tree& t = doc_.tree_;
  const TypeDef& type = S().Type(TypeOfElem(t, elem));
  AttrDefId def = S().AttrOf(TypeOfElem(t, elem), name);
  // A value XML can't hold would make the saved part unreadable (callers refuse it first).
  if (def == kNoDef || !XmlSafe(value)) return;
  StrId v = t.strings.Intern(value);
  const Elem& e = t.elems[elem];
  std::vector<AttrVal> attrs(t.attrs.begin() + e.attrs, t.attrs.begin() + e.attrs + e.attrCount);
  for (AttrVal& a : attrs) {
    if (a.def != def) continue;
    if (a.value == v) return;
    a.value = v;
    WriteAttrs(elem, attrs);
    return;
  }
  size_t rank = AttrRank(type, def);
  auto at = std::find_if(attrs.begin(), attrs.end(), [&](const AttrVal& a) { return AttrRank(type, a.def) > rank; });
  attrs.insert(at, AttrVal{def, v});
  WriteAttrs(elem, attrs);
}

void TreeEdit::RemoveAttr(ElemId elem, uint32_t name) {
  Tree& t = doc_.tree_;
  AttrDefId def = S().AttrOf(TypeOfElem(t, elem), name);
  const Elem& e = t.elems[elem];
  std::vector<AttrVal> attrs(t.attrs.begin() + e.attrs, t.attrs.begin() + e.attrs + e.attrCount);
  auto it = std::find_if(attrs.begin(), attrs.end(), [&](const AttrVal& a) { return a.def == def; });
  if (it == attrs.end()) return;
  attrs.erase(it);
  WriteAttrs(elem, attrs);
}

// ---------- Runs ----------

bool TreeEdit::SplitRunAt(uint32_t para, uint32_t offset) {
  Tree& t = doc_.tree_;
  const Paragraph& p = doc_.paras_[para];
  if (offset == 0 || offset >= p.text.size()) return true;
  // The innermost run with text on both sides of the offset.
  ElemId run = kNoElem;
  for (ElemId id = p.elem; id != kNoElem;) {
    const Elem& e = t.elems[id];
    bool inside = e.start < offset && offset < e.start + e.length;
    Role role = S().Type(TypeOfElem(t, id)).role;
    if (id != p.elem && !inside) {
      id = NextIn(t, id, p.elem, true);
      continue;
    }
    if (id != p.elem && (e.flags & kDisplayOnly)) return false;
    if (role == Role::Run) {
      run = id;
      break;
    }
    if (role == Role::Object || role == Role::Text || role == Role::Char) return false;
    id = NextIn(t, id, p.elem);
  }
  if (run == kNoElem) return true;  // between runs already

  // The first content child that reaches past the offset; it and the ones after it move.
  ElemId props = kNoElem, last = kNoElem, split = kNoElem;
  for (ElemId c = t.elems[run].first; c != kNoElem; c = t.elems[c].next) {
    if (NameOfElem(t, c) == names::kRPr) {
      props = c;
    } else if (t.elems[c].start + t.elems[c].length > offset) {
      split = c;
      break;
    }
    last = c;
  }
  if (split == kNoElem) return true;

  // The second half: a copy of the run, with the same properties (shared until one changes).
  Elem half = t.elems[run];
  half.first = kNoElem;
  half.start = offset;
  half.length = t.elems[run].start + t.elems[run].length - offset;
  half.node = NewNode();
  t.elems.push_back(half);
  auto second = static_cast<ElemId>(t.elems.size() - 1);
  if (pkg::AttrSetId set = doc_.store_.NodeAttributes(t.elems[run].node)) StoreSetAttrs(t.elems[second].node, set);
  Touch(run).length = offset - t.elems[run].start;
  E(run).next = second;

  ElemId tail = kNoElem;  // the second half's last child so far
  auto append = [&](ElemId c) {
    if (tail == kNoElem) E(second).first = c;
    else E(tail).next = c;
    tail = c;
  };
  if (props != kNoElem) {
    if (HasRepeats(props)) {
      append(CopyProps(props, second));  // without the repeats, so the split adds no error
    } else {
      if (t.elems[props].first != kNoElem) Touch(props).flags |= kSharedKids;
      append(Clone(props, second));
    }
  }

  pkg::NodeId firstMoved = t.elems[split].node;
  if (t.elems[split].start < offset) {
    // Inside text: the w:t splits too, and each half keeps its spaces.
    Elem text = t.elems[split];
    text.parent = second;
    text.start = offset;
    text.length = t.elems[split].start + t.elems[split].length - offset;
    text.node = NewNode();
    t.elems.push_back(text);
    auto textHalf = static_cast<ElemId>(t.elems.size() - 1);
    Touch(split).length = offset - t.elems[split].start;
    E(split).next = kNoElem;
    if (pkg::AttrSetId set = doc_.store_.NodeAttributes(t.elems[split].node)) StoreSetAttrs(t.elems[textHalf].node, set);
    StoreSplit(t.elems[split].node, t.elems[textHalf].node, pkg::SplitPoint{pkg::kNoNode, false});
    for (ElemId piece : {split, textHalf}) {
      std::u16string_view s = std::u16string_view(p.text).substr(t.elems[piece].start, t.elems[piece].length);
      if (!s.empty() && (Space(s.front()) || Space(s.back())) && !Node(&t, piece).Has(Name("xml:space")))
        SetAttr(piece, Name("xml:space"), "preserve");
    }
    firstMoved = E(textHalf).next != kNoElem ? E(E(textHalf).next).node : pkg::kNoNode;
    append(textHalf);
    for (ElemId c = E(textHalf).next; c != kNoElem; c = E(c).next) Touch(c).parent = second;
  } else {
    if (last == kNoElem) Touch(run).first = props;  // nothing before it but the properties
    else Touch(last).next = kNoElem;
    for (ElemId c = split; c != kNoElem; c = E(c).next) Touch(c).parent = second;
    append(split);
  }
  StoreSplit(E(run).node, E(second).node, pkg::SplitPoint{firstMoved, false});
  return true;
}

// ---------- Elements from XML ----------

namespace {

bool Known(TypeId parentType, pugi::xml_node n) {
  if (n.type() != pugi::node_element) return n.type() == pugi::node_pcdata && std::string_view(n.value()).find_first_not_of(" \t\r\n") == std::string_view::npos;
  ElemDefId def = S().Child(parentType, QNameHash(n.name()));
  if (def == kNoDef) return false;
  TypeId type = S().Elem(def).type;
  if (S().Type(type).role != Role::Plain) return false;
  for (pugi::xml_attribute a : n.attributes()) {
    std::string_view q = a.name();
    if (q.starts_with("xmlns")) continue;
    if (S().AttrOf(type, QNameHash(q)) == kNoDef) return false;
  }
  for (pugi::xml_node c : n.children())
    if (!Known(type, c)) return false;
  return true;
}

}  // namespace

void TreeEdit::Append(ElemId parent, ElemId child) {
  ElemId prev = kNoElem;
  for (ElemId c = E(parent).first; c != kNoElem; c = E(c).next) prev = c;
  if (prev == kNoElem) Touch(parent).first = child;
  else Touch(prev).next = child;
  E(child).next = kNoElem;
  E(child).parent = parent;
}

ElemId TreeEdit::Build(ElemId parent, const pugi::xml_node& n, ElemId before, bool last) {
  ElemDefId def = S().Child(TypeOfElem(doc_.tree_, parent), QNameHash(n.name()));
  Elem e;
  e.def = def;
  e.parent = parent;
  e.node = NewNode();
  doc_.tree_.elems.push_back(e);
  auto id = static_cast<ElemId>(doc_.tree_.elems.size() - 1);
  if (last) Append(parent, id);
  else Link(parent, id, before);
  TypeId type = S().Elem(def).type;
  const TypeDef& td = S().Type(type);
  std::vector<AttrVal> attrs;
  for (pugi::xml_attribute a : n.attributes()) {
    std::string_view q = a.name();
    if (q.starts_with("xmlns")) continue;
    AttrDefId ad = S().AttrOf(type, QNameHash(q));
    auto at = std::find_if(attrs.begin(), attrs.end(), [&](const AttrVal& o) { return AttrRank(td, o.def) > AttrRank(td, ad); });
    attrs.insert(at, AttrVal{ad, doc_.tree_.strings.Intern(a.value())});
  }
  if (!attrs.empty()) WriteAttrs(id, attrs);
  // The XML is written in schema order already: its children keep their order.
  for (pugi::xml_node c : n.children())
    if (c.type() == pugi::node_element) Build(id, c, kNoElem, true);
  return id;
}

ElemId TreeEdit::AddXml(ElemId parent, std::string_view xml, ElemId before) {
  std::string wrapped = "<root";
  for (size_t i = 0; i < kNsCount; ++i) {
    auto ns = static_cast<Ns>(i);
    if (ns == Ns::None || ns == Ns::Xml || ns == Ns::Other) continue;
    wrapped += " xmlns:" + std::string(NsPrefix(ns)) + "=\"" + std::string(NsUri(ns)) + "\"";
  }
  wrapped += ">";
  wrapped += xml;
  wrapped += "</root>";
  pugi::xml_document d;
  if (!d.load_buffer(wrapped.data(), wrapped.size(), pugi::parse_default, pugi::encoding_utf8)) return kNoElem;
  pugi::xml_node n = d.first_child().first_child();
  while (n && n.type() != pugi::node_element) n = n.next_sibling();
  if (!n || !Known(TypeOfElem(doc_.tree_, parent), n)) return kNoElem;
  return Build(parent, n, before, false);
}

ElemId TreeEdit::CopyChild(ElemId src, ElemId parent) {
  if (src == kNoElem || parent == kNoElem) return kNoElem;
  uint32_t name = NameOfElem(doc_.tree_, src);
  ElemDefId def = S().Child(TypeOfElem(doc_.tree_, parent), name);
  if (def == kNoDef || S().Elem(def).type != TypeOfElem(doc_.tree_, src)) return kNoElem;
  if (ElemId old = Child(parent, name, false); old != kNoElem) RemoveElement(old);
  ElemId copy = CopyProps(src, parent);
  E(copy).def = def;
  Link(parent, copy, kNoElem);
  return copy;
}

// ---------- Parts ----------

ElemId TreeEdit::EnsurePart(PartKind kind) {
  if (kind != PartKind::Styles && kind != PartKind::Numbering) return kNoElem;
  if (const Part* p = doc_.FindPart(kind)) return p->root;
  const Part* main = doc_.FindPart(PartKind::Document);
  if (depth_ > 0 || !main || !doc_.Editable()) return kNoElem;
  const auto w = static_cast<size_t>(Ns::W), r = static_cast<size_t>(Ns::R);
  const bool strict = IsStrictUri(main->uri[w]);
  const std::string local = kind == PartKind::Styles ? "styles" : "numbering";
  const std::string dir = main->name.substr(0, main->name.rfind('/') + 1);
  std::string name = dir + local + ".xml";
  for (int i = 2; doc_.Package().FindPart(name); ++i) name = dir + local + std::to_string(i) + ".xml";

  // The package: a content type and a relationship from the main part.
  pkg::Package& package = doc_.Package();
  pkg::ContentTypes types = package.Types();
  types.overrides.emplace_back(name, "application/vnd.openxmlformats-officedocument.wordprocessingml." + local + "+xml");
  package.SetContentTypes(std::move(types));
  pkg::RelationshipSet rels;
  if (const pkg::RelationshipSet* have = package.RelationshipsOf(main->name)) {
    rels = *have;
  } else {
    rels.source = main->name;
    rels.relsPart = pkg::RelsPartFor(main->name);
  }
  pkg::Relationship rel;
  for (size_t n = rels.items.size() + 1;; ++n) {
    rel.id = "rId" + std::to_string(n);
    if (!rels.Find(rel.id)) break;
  }
  rel.type = std::string(strict ? "http://purl.oclc.org/ooxml/officeDocument/relationships/"
                                : "http://schemas.openxmlformats.org/officeDocument/2006/relationships/") +
             local;
  rel.target = name.substr(dir.size());
  rel.targetPart = name;
  rels.items.push_back(std::move(rel));
  package.SetRelationships(std::move(rels));

  // The model: an empty root with the main part's namespaces.
  Part part;
  part.name = name;
  part.kind = kind;
  part.docNode = NewNode();
  part.root = doc_.tree_.Add(S().Child(S().RootType(), NameOf(Ns::W, local)), kNoElem);
  doc_.tree_.elems[part.root].node = NewNode();
  for (size_t i : {w, r}) {
    part.prefix[i] = std::string(NsPrefix(static_cast<Ns>(i)));
    part.uri[i] = main->uri[i].empty() ? std::string(NsUri(static_cast<Ns>(i))) : main->uri[i];
    part.addDecls.emplace_back(part.prefix[i], part.uri[i]);
  }
  part.prefix[static_cast<size_t>(Ns::Xml)] = "xml";
  pkg::XmlWriter decl;
  decl.Declaration(true);
  doc_.store_.Add(doc_.tree_.elems[part.root].node, pkg::Fragment{pkg::Slot::Before, {}, decl.Take()});
  ElemId root = part.root;
  doc_.parts_.push_back(std::move(part));
  std::string xml;
  if (docx::WritePart(doc_, static_cast<uint32_t>(doc_.parts_.size() - 1), xml).Ok())
    package.SetPart(name, std::vector<uint8_t>(xml.begin(), xml.end()));

  Reindex();
  return root;
}

}  // namespace fw::doc::edit
