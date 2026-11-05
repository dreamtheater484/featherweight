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

bool RealNode(pkg::NodeId n) { return n != pkg::kNoNode && n != pkg::kFieldNode; }

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
  refused_.clear();
  local_ = toEnd_ = structure_ = false;
  affects_ = {kNone, kNone};
  owned_.clear();
  elemsAtBegin_ = doc_.tree_.elems.size();
  before_.clear();
  store_.clear();
  touched_.clear();
  tracked_.clear();
  copies_ = 0;
  tracking_ = !editor_.TrackingPaused() && editor_.Tracking();
  if (tracking_) {
    trackAuthor_ = editor_.ChangeAuthor();
    trackDate_ = editor_.ChangeDate();
  }
}

void TreeEdit::Commit() {
  if (depth_ == 0 || --depth_ > 0) return;
  RecordFormatting();
  Tree& t = doc_.tree_;
  // (Copies kept for tracked formatting that no record took don't count: nothing changed.)
  if (before_.empty() && store_.empty() && t.elems.size() == elemsAtBegin_ + copies_) {
    touched_.clear();
    return;
  }
  // Text the editor doesn't edit changes nothing: the change goes back as a whole.
  {
    std::unordered_map<ElemId, bool> memo;
    std::u16string why;
    bool ok = true;
    for (const auto& [id, old] : before_) ok = ok && Editable(id, memo, why);
    for (size_t id = elemsAtBegin_; ok && id < t.elems.size(); ++id) ok = Editable(static_cast<ElemId>(id), memo, why);
    if (!ok) {
      Discard();
      refused_ = why;
      return;
    }
  }
  touched_.clear();
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
      local = local && (InDefinitions(id) || !Attached(id));  // or an unused copy, linked nowhere
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
      if (last + 1 < doc_.paras_.size() && doc_.paras_[last + 1].story == story) ++last;
      if (toEnd_ && doc_.StoryParagraphs(story).back() > last) {
        step->renumberFrom = last + 1;
        last = doc_.StoryParagraphs(story).back();
      }
      step->first = first;
      step->count = last - first + 1;
    }
  }
  if (affects_.first != kNone && affects_.second < doc_.paras_.size()) {
    step->first = affects_.first;
    step->count = affects_.second - affects_.first + 1;
    step->structure = structure_;
    if (caret.para == kNone) caret = {affects_.first, 0};
  }
  editor_.RecordStep(std::move(label_), std::move(step), caret);
}

bool TreeEdit::Editable(ElemId id, std::unordered_map<ElemId, bool>& memo, std::u16string& why) const {
  static constexpr uint32_t kTextBox = Name("txbxContent");
  const Tree& t = doc_.tree_;
  std::vector<ElemId> chain;
  bool verdict = true;
  for (ElemId e = id; e != kNoElem; e = t.elems[e].parent) {
    if (auto it = memo.find(e); it != memo.end()) {
      verdict = it->second;
      break;
    }
    chain.push_back(e);
    if ((t.elems[e].flags & kDisplayOnly) || NameOfElem(t, e) == kTextBox) {
      verdict = false;
      why = NameOfElem(t, e) == kTextBox ? u"Text in a text box can't be formatted yet."
                                         : u"This text is kept as the file has it, so it can't be formatted.";
      break;
    }
    if (t.elems[e].parent == kNoElem || chain.size() > 512) {
      // The top: a part's root (an element linked nowhere, an old copy, is fine).
      for (const Part& part : doc_.parts_)
        if (part.root == e && (!part.Editable() || !part.writable)) {
          verdict = false;
          why = u"This part of the document uses features Featherweight doesn't understand, so it can't be edited.";
        }
      break;
    }
  }
  if (!verdict && why.empty()) why = u"This text can't be formatted.";
  for (ElemId e : chain) memo.emplace(e, verdict);
  return verdict;
}

bool TreeEdit::CanChange(const std::vector<uint32_t>& paras) {
  std::unordered_map<ElemId, bool> memo;
  std::u16string why;
  for (uint32_t p : paras) {
    if (p < doc_.paras_.size() && !Editable(doc_.paras_[p].elem, memo, why)) {
      refused_ = why;
      return false;
    }
  }
  return true;
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
      else if (op->kind == StoreOp::CommentsEx) doc.commentsEx_ = op->exBefore;
      else store.SetFragments(op->node, op->fragmentsBefore);
    }
  } else {
    for (const StoreOp& op : ops) {
      if (op.kind == StoreOp::Split) store.Split(op.node, op.second, op.at);
      else if (op.kind == StoreOp::SetAttrs) store.SetNodeAttributes(op.node, op.after);
      else if (op.kind == StoreOp::CommentsEx) doc.commentsEx_ = op.exAfter;
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
  doc_.ResetCaches();
  doc_.BuildIndexes();
}

void TreeEdit::Abort() {
  if (depth_ == 0) return;
  depth_ = 0;
  Discard();
}

void TreeEdit::Discard() {
  const bool changed = !before_.empty() || !store_.empty();
  Replay(doc_, store_, true);
  Restore(doc_, before_);  // new elements stay in the arena, linked nowhere
  before_.clear();
  store_.clear();
  touched_.clear();
  owned_.clear();
  tracked_.clear();
  if (changed) Reindex();
}

// ---------- Tracked formatting (D4) ----------

void TreeEdit::TrackBefore(ElemId at) {
  if (!tracking_ || recording_ || at == kNoElem) return;
  const Tree& t = doc_.tree_;
  static constexpr uint32_t kP = names::kP, kR = Name("r"), kRPr = Name("rPr"), kPPr = Name("pPr"), kIns = Name("ins"),
                            kDel = Name("del"), kMoveFrom = Name("moveFrom"), kMoveTo = Name("moveTo");
  // The properties `at` is in (or is), unless it's inside a change record.
  ElemId props = kNoElem;
  for (ElemId e = at; e != kNoElem && props == kNoElem; e = t.elems[e].parent) {
    const uint32_t n = NameOfElem(t, e);
    if (n == Name("rPrChange") || n == Name("pPrChange")) return;
    const ElemId up = t.elems[e].parent;
    const uint32_t un = up == kNoElem ? 0 : NameOfElem(t, up);
    if ((n == kRPr && un == kR) || (n == kPPr && un == kP) ||
        (n == kRPr && un == kPPr && t.elems[up].parent != kNoElem && NameOfElem(t, t.elems[up].parent) == kP))
      props = e;
    else if (n == kP || n == kR || up == kNoElem)
      return;
  }
  if (props == kNoElem) return;
  for (const auto& [p, copy] : tracked_)
    if (p == props) return;
  // A comment's text isn't tracked (as in Word).
  for (ElemId e = props; e != kNoElem; e = t.elems[e].parent)
    if (NameOfElem(t, e) == kP) {
      const uint32_t i = t.elems[e].start;
      if (i < doc_.paras_.size() && doc_.paras_[i].elem == e &&
          doc_.stories_[doc_.paras_[i].story].kind == StoryKind::Comment)
        return;
      break;
    }
  // The author's own new text or paragraph isn't tracked again.
  const Node here(&t, props);
  bool own = false;
  auto mine = [&](Node c) { return c && c.AttrOr(Name("author")) == trackAuthor_; };
  if (here.Parent().Is(kR)) {
    for (Node a = here.Parent().Parent(); a && !a.Is(kP) && !own; a = a.Parent()) {
      if (a.Is(kIns) || a.Is(kDel) || a.Is(kMoveFrom) || a.Is(kMoveTo)) {
        own = a.Is(kIns) && mine(a);
        break;
      }
    }
  } else {
    const Node p = here.Parent().Is(kP) ? here.Parent() : here.Parent().Parent();
    own = mine(p.Child(kPPr).Child(kRPr).Child(kIns));
  }
  tracked_.emplace_back(props, own ? kNoElem : DetachedCopy(props, kNoElem));
}

ElemId TreeEdit::DetachedCopy(ElemId src, ElemId parent) {
  Elem e = E(src);  // a copy: the arena grows below
  const ElemId first = e.first;
  const pkg::NodeId from = e.node;
  e.parent = parent;
  e.next = kNoElem;
  e.first = kNoElem;
  e.flags = static_cast<uint8_t>(e.flags & ~kSharedKids);
  e.node = NewNode();
  doc_.tree_.elems.push_back(e);
  ++copies_;
  const auto id = static_cast<ElemId>(doc_.tree_.elems.size() - 1);
  if (RealNode(from))
    if (pkg::AttrSetId set = doc_.store_.NodeAttributes(from)) doc_.store_.SetNodeAttributes(e.node, set);
  ElemId tail = kNoElem;
  for (ElemId c = first; c != kNoElem; c = E(c).next) {
    const ElemId k = DetachedCopy(c, id);
    if (tail == kNoElem) E(id).first = k;
    else E(tail).next = k;
    tail = k;
  }
  return id;
}

bool TreeEdit::SameElem(ElemId a, ElemId b) const {
  const Tree& t = doc_.tree_;
  const Elem &x = t.elems[a], &y = t.elems[b];
  if (NameOfElem(t, a) != NameOfElem(t, b) || x.attrCount != y.attrCount) return false;
  for (uint16_t i = 0; i < x.attrCount; ++i) {
    const AttrVal& p = t.attrs[x.attrs + i];
    bool found = false;
    for (uint16_t j = 0; j < y.attrCount && !found; ++j)
      found = S().Attr(t.attrs[y.attrs + j].def).name == S().Attr(p.def).name && t.attrs[y.attrs + j].value == p.value;
    if (!found) return false;
  }
  ElemId c = x.first, d = y.first;
  for (; c != kNoElem && d != kNoElem; c = t.elems[c].next, d = t.elems[d].next)
    if (!SameElem(c, d)) return false;
  return c == kNoElem && d == kNoElem;
}

bool TreeEdit::Attached(ElemId id) const {
  const Tree& t = doc_.tree_;
  for (int hops = 0; hops < 256; ++hops) {
    const ElemId up = t.elems[id].parent;
    if (up == kNoElem) {
      for (const Part& part : doc_.parts_)
        if (part.root == id) return true;
      return false;
    }
    bool found = false;
    for (ElemId c = t.elems[up].first; c != kNoElem && !found; c = t.elems[c].next) found = c == id;
    if (!found) return false;
    id = up;
  }
  return false;
}

void TreeEdit::RecordFormatting() {
  if (tracked_.empty()) return;
  const Tree& t = doc_.tree_;
  static constexpr uint32_t kRPr = Name("rPr"), kPPr = Name("pPr");
  recording_ = true;
  for (const auto& [props, copy] : tracked_) {
    if (copy == kNoElem || !Attached(props)) continue;
    const bool para = NameOfElem(t, props) == kPPr;
    const uint32_t changeName = para ? Name("pPrChange") : Name("rPrChange");
    const ElemDefId changeDef = S().Child(TypeOfElem(t, props), changeName);
    if (changeDef == kNoDef) continue;
    const ElemDefId innerDef = S().Child(S().Elem(changeDef).type, para ? kPPr : kRPr);
    if (innerDef == kNoDef) continue;
    const TypeId inner = S().Elem(innerDef).type;
    // What the record holds: a paragraph's w:rPr and w:sectPr, and a mark's records, aren't.
    auto held = [&](ElemId c) { return S().Child(inner, NameOfElem(t, c)) != kNoDef; };
    bool recorded = false, same = true;
    std::vector<ElemId> now, was;
    for (ElemId c = E(props).first; c != kNoElem; c = E(c).next) {
      recorded = recorded || NameOfElem(t, c) == changeName;
      if (held(c)) now.push_back(c);
    }
    if (recorded) continue;  // the oldest stays
    for (ElemId c = E(copy).first; c != kNoElem; c = E(c).next)
      if (held(c)) was.push_back(c);
    same = now.size() == was.size();
    for (size_t i = 0; same && i < now.size(); ++i) same = SameElem(now[i], was[i]);
    if (same) continue;
    const ElemId change = Child(props, changeName);
    if (change == kNoElem) continue;
    SetAttr(change, names::kId, editor_.NewChangeId());
    SetAttr(change, Name("author"), trackAuthor_);
    SetAttr(change, Name("date"), trackDate_);
    const ElemId into = Child(change, para ? kPPr : kRPr);
    if (into == kNoElem) continue;
    for (ElemId k : was) {
      E(k).def = S().Child(inner, NameOfElem(t, k));
      Append(into, k);
    }
  }
  recording_ = false;
  tracked_.clear();
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
  TrackBefore(parent);
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
  TrackBefore(parent);
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
  TrackBefore(container);
  Own(container);
  // Tracked changes are never cleared away: a mark's insertion or deletion, a formatting record.
  static const uint32_t kRecords[] = {Name("rPrChange"), Name("pPrChange"), Name("ins"), Name("del"), Name("moveFrom"),
                                      Name("moveTo")};
  std::vector<ElemId> kids;
  for (ElemId c = E(container).first; c != kNoElem; c = E(c).next) kids.push_back(c);
  for (ElemId c : kids) {
    const uint32_t n = NameOfElem(doc_.tree_, c);
    if (std::find(keep.begin(), keep.end(), n) == keep.end() &&
        std::find(std::begin(kRecords), std::end(kRecords), n) == std::end(kRecords))
      RemoveElement(c);
  }
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
  TrackBefore(elem);
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
  TrackBefore(elem);
  Tree& t = doc_.tree_;
  AttrDefId def = S().AttrOf(TypeOfElem(t, elem), name);
  const Elem& e = t.elems[elem];
  std::vector<AttrVal> attrs(t.attrs.begin() + e.attrs, t.attrs.begin() + e.attrs + e.attrCount);
  auto it = std::find_if(attrs.begin(), attrs.end(), [&](const AttrVal& a) { return a.def == def; });
  if (it == attrs.end()) return;
  attrs.erase(it);
  WriteAttrs(elem, attrs);
}

// ---------- Inline content ----------

void TreeEdit::LinkAt(ElemId parent, ElemId child, ElemId before) {
  Touch(child);
  ElemId prev = kNoElem;
  for (ElemId c = E(parent).first; c != kNoElem && c != before; c = E(c).next) prev = c;
  if (prev == kNoElem) {
    E(child).next = Touch(parent).first;
    E(parent).first = child;
  } else {
    E(child).next = Touch(prev).next;
    E(prev).next = child;
  }
  E(child).parent = parent;
}

void TreeEdit::Unlink(ElemId elem) {
  const ElemId parent = E(elem).parent;
  if (parent == kNoElem) return;
  ElemId prev = kNoElem;
  for (ElemId c = E(parent).first; c != kNoElem && c != elem; c = E(c).next) prev = c;
  if (prev == kNoElem) {
    if (E(parent).first != elem) return;
    Touch(parent).first = E(elem).next;
  } else {
    Touch(prev).next = E(elem).next;
  }
  Touch(elem).next = kNoElem;
}

ElemId TreeEdit::InsertElement(ElemId parent, uint32_t name, ElemId before, uint32_t offset) {
  if (parent == kNoElem) return kNoElem;
  const ElemDefId def = S().Child(TypeOfElem(doc_.tree_, parent), name);
  if (def == kNoDef) return kNoElem;
  Elem e;
  e.def = def;
  e.parent = parent;
  e.node = NewNode();
  e.start = offset;
  e.length = 0;
  doc_.tree_.elems.push_back(e);
  const auto id = static_cast<ElemId>(doc_.tree_.elems.size() - 1);
  LinkAt(parent, id, before);
  return id;
}

ElemId TreeEdit::Wrap(ElemId first, ElemId last, uint32_t name) {
  Tree& t = doc_.tree_;
  const ElemId parent = E(first).parent;
  if (parent == kNoElem) return kNoElem;
  const ElemDefId def = S().Child(TypeOfElem(t, parent), name);
  if (def == kNoDef) return kNoElem;
  const TypeId type = S().Elem(def).type;
  std::vector<std::pair<ElemId, ElemDefId>> kids;  // with their definitions in the new element
  for (ElemId c = first;; c = E(c).next) {
    if (c == kNoElem) return kNoElem;  // `last` isn't after `first`
    const ElemDefId kd = S().Child(type, NameOfElem(t, c));
    if (kd == kNoDef || S().Elem(kd).type != TypeOfElem(t, c)) return kNoElem;
    kids.emplace_back(c, kd);
    if (c == last) break;
  }
  Elem w;
  w.def = def;
  w.parent = parent;
  w.node = NewNode();
  w.start = E(first).start;
  w.length = E(last).start + E(last).length - w.start;
  t.elems.push_back(w);
  const auto id = static_cast<ElemId>(t.elems.size() - 1);
  LinkAt(parent, id, first);
  for (const auto& [c, kd] : kids) Unlink(c);
  for (const auto& [c, kd] : kids) {
    LinkAt(id, c, kNoElem);  // what each keeps beside it goes with it
    E(c).def = kd;
  }
  return id;
}

bool TreeEdit::Unwrap(ElemId elem) {
  Tree& t = doc_.tree_;
  const ElemId parent = E(elem).parent;
  if (parent == kNoElem) return false;
  const TypeId ptype = TypeOfElem(t, parent);
  std::vector<std::pair<ElemId, ElemDefId>> kids;
  for (ElemId c = E(elem).first; c != kNoElem; c = E(c).next) {
    const ElemDefId kd = S().Child(ptype, NameOfElem(t, c));
    if (kd == kNoDef || S().Elem(kd).type != TypeOfElem(t, c)) return false;
    kids.emplace_back(c, kd);
  }
  const pkg::NodeId node = E(elem).node;
  std::vector<pkg::Fragment> before, after;
  if (RealNode(node)) {
    for (pkg::Fragment& f : doc_.store_.FragmentsCopy(node)) {
      if (f.slot == pkg::Slot::Inside && !f.afterChild.empty()) return false;  // between its children
      if (f.slot == pkg::Slot::After) after.push_back(std::move(f));
      else before.push_back(std::move(f));  // Before, then what it keeps first inside
    }
  }
  if (kids.empty()) {
    if (!before.empty() && std::any_of(before.begin(), before.end(), [](const pkg::Fragment& f) { return f.slot == pkg::Slot::Inside; }))
      return false;
    RemoveElement(elem);
    return true;
  }
  if ((!before.empty() && !RealNode(E(kids.front().first).node)) || (!after.empty() && !RealNode(E(kids.back().first).node)))
    return false;  // nowhere to keep them
  for (const auto& [c, kd] : kids) {
    LinkAt(parent, c, elem);
    E(c).def = kd;
  }
  Touch(elem).first = kNoElem;
  Unlink(elem);
  if (!before.empty() || !after.empty()) {
    StoreFragments(node, {});
    auto give = [&](ElemId to, std::vector<pkg::Fragment> moved, bool first) {
      if (moved.empty()) return;
      for (pkg::Fragment& f : moved) {
        f.slot = first ? pkg::Slot::Before : pkg::Slot::After;
        f.afterChild.clear();
      }
      std::vector<pkg::Fragment> there = doc_.store_.FragmentsCopy(E(to).node);
      there.insert(first ? there.begin() : there.end(), std::make_move_iterator(moved.begin()),
                   std::make_move_iterator(moved.end()));
      StoreFragments(E(to).node, std::move(there));
    };
    give(kids.front().first, std::move(before), true);
    give(kids.back().first, std::move(after), false);
  }
  return true;
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
    ElemId mine;
    if (HasRepeats(props)) {
      mine = CopyProps(props, second);  // without the repeats, so the split adds no error
    } else {
      if (t.elems[props].first != kNoElem) Touch(props).flags |= kSharedKids;
      mine = Clone(props, second);
    }
    append(mine);
    // A tracked formatting change (w:rPrChange) in both halves: the second gets an id of its own,
    // as ids are unique in a document (torture finding 38).
    for (ElemId c = E(mine).first; c != kNoElem; c = E(c).next)
      if (NameOfElem(t, c) == Name("rPrChange") && Node(&t, c).Has(names::kId)) {
        Own(mine);
        for (ElemId k = E(mine).first; k != kNoElem; k = E(k).next)
          if (NameOfElem(t, k) == Name("rPrChange")) SetAttr(k, names::kId, editor_.NewChangeId());
        break;
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
  TrackBefore(parent);
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

namespace {

// A new part's content type and its relationship from `source`; returns the relationship's id.
std::string AddToPackage(pkg::Package& package, const std::string& source, const std::string& name,
                         const std::string& contentType, const std::string& relType) {
  pkg::ContentTypes types = package.Types();
  types.overrides.emplace_back(name, contentType);
  package.SetContentTypes(std::move(types));
  pkg::RelationshipSet rels;
  if (const pkg::RelationshipSet* have = package.RelationshipsOf(source)) {
    rels = *have;
  } else {
    rels.source = source;
    rels.relsPart = pkg::RelsPartFor(source);
  }
  pkg::Relationship rel;
  for (size_t n = rels.items.size() + 1;; ++n) {
    rel.id = "rId" + std::to_string(n);
    if (!rels.Find(rel.id)) break;
  }
  const std::string dir = source.substr(0, source.rfind('/') + 1);
  rel.type = relType;
  rel.target = name.starts_with(dir) ? name.substr(dir.size()) : name;
  rel.targetPart = name;
  std::string id = rel.id;
  rels.items.push_back(std::move(rel));
  package.SetRelationships(std::move(rels));
  return id;
}

std::string RelType(bool strict, std::string_view local) {
  return std::string(strict ? "http://purl.oclc.org/ooxml/officeDocument/relationships/"
                            : "http://schemas.openxmlformats.org/officeDocument/2006/relationships/") +
         std::string(local);
}

// The first free name like /word/header3.xml.
std::string FreeName(const pkg::Package& package, const std::string& dir, const std::string& local, bool numbered) {
  std::string name = dir + local + (numbered ? "1" : "") + ".xml";
  for (int i = 2; package.FindPart(name); ++i) name = dir + local + std::to_string(i) + ".xml";
  return name;
}

}  // namespace

ElemId TreeEdit::EnsurePart(PartKind kind) {
  if (kind != PartKind::Styles && kind != PartKind::Numbering && kind != PartKind::Settings && kind != PartKind::Comments)
    return kNoElem;
  if (const Part* p = doc_.FindPart(kind)) return p->root;
  const Part* main = doc_.FindPart(PartKind::Document);
  if (depth_ > 0 || !main || !doc_.Editable()) return kNoElem;
  const auto w = static_cast<size_t>(Ns::W), r = static_cast<size_t>(Ns::R);
  const bool strict = IsStrictUri(main->uri[w]);
  const std::string local = kind == PartKind::Styles      ? "styles"
                            : kind == PartKind::Numbering ? "numbering"
                            : kind == PartKind::Comments  ? "comments"
                                                          : "settings";
  const std::string dir = main->name.substr(0, main->name.rfind('/') + 1);
  pkg::Package& package = doc_.Package();
  const std::string name = FreeName(package, dir, local, false);
  AddToPackage(package, main->name, name, "application/vnd.openxmlformats-officedocument.wordprocessingml." + local + "+xml",
               RelType(strict, local));

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
  if (kind == PartKind::Comments) {
    // Comments' paragraphs carry w14:paraId, which commentsExtended names them by.
    const auto w14 = static_cast<size_t>(Ns::W14);
    part.prefix[w14] = std::string(NsPrefix(Ns::W14));
    part.uri[w14] = std::string(NsUri(Ns::W14));
    part.addDecls.emplace_back(part.prefix[w14], part.uri[w14]);
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

// ---------- Header and footer parts ----------

namespace {

// Range markers and ids a copy leaves out: they must be unique in a document.
bool LeftOutOfCopy(uint32_t name, uint32_t parentName) {
  static const uint32_t kMarkers[] = {
      Name("bookmarkStart"), Name("bookmarkEnd"), Name("commentRangeStart"), Name("commentRangeEnd"),
      Name("permStart"), Name("permEnd"), Name("moveFromRangeStart"), Name("moveFromRangeEnd"),
      Name("moveToRangeStart"), Name("moveToRangeEnd"), Name("customXmlInsRangeStart"), Name("customXmlInsRangeEnd"),
      Name("customXmlDelRangeStart"), Name("customXmlDelRangeEnd"), Name("customXmlMoveFromRangeStart"),
      Name("customXmlMoveFromRangeEnd"), Name("customXmlMoveToRangeStart"), Name("customXmlMoveToRangeEnd")};
  for (uint32_t m : kMarkers)
    if (name == m) return true;
  return name == names::kId && parentName == Name("sdtPr");  // a content control's id
}

}  // namespace

bool TreeEdit::Copyable(ElemId root, std::u16string& why) const {
  const Tree& t = doc_.tree_;
  static const uint32_t kRefuse[] = {Name("footnoteReference"), Name("endnoteReference"), Name("commentReference"),
                                     Name("annotationRef")};
  for (ElemId id = root; id != kNoElem; id = NextIn(t, id, root)) {
    const Elem& e = t.elems[id];
    const uint32_t name = NameOfElem(t, id);
    if (e.flags & kDisplayOnly) {
      why = u"This header or footer holds content Featherweight keeps as it is, so it can't be copied.";
      return false;
    }
    if (S().Type(TypeOfElem(t, id)).role == Role::Object) {
      why = u"This header or footer has a picture or an object, which Featherweight can't copy yet.";
      return false;
    }
    for (uint32_t r : kRefuse)
      if (name == r) {
        why = u"This header or footer has a note or a comment, which can't be copied.";
        return false;
      }
    if (!RealNode(e.node)) continue;
    const bool leftOut = e.parent != kNoElem && LeftOutOfCopy(name, NameOfElem(t, e.parent));
    for (const pkg::Fragment& f : doc_.store_.FragmentsCopy(e.node)) {
      // Kept content that refers to the part's relationships or holds ids would be wrong twice.
      const std::string& x = f.xml;
      if (f.slot == pkg::Slot::Before && x.starts_with("<?xml")) continue;  // the declaration
      if (leftOut || x.find("r:id=") != std::string::npos || x.find("r:embed=") != std::string::npos ||
          x.find("r:link=") != std::string::npos || x.find(":id=") != std::string::npos ||
          x.find("paraId=") != std::string::npos) {
        why = u"This header or footer holds content Featherweight keeps as it is, so it can't be copied.";
        return false;
      }
    }
  }
  return true;
}

ElemId TreeEdit::CopyTree(ElemId src, ElemId parent, uint32_t story) {
  Tree& t = doc_.tree_;
  Elem e = t.elems[src];
  const ElemId srcFirst = e.first;
  const pkg::NodeId from = e.node;
  e.parent = parent;
  e.next = kNoElem;
  e.first = kNoElem;
  e.flags = static_cast<uint8_t>(e.flags & ~kSharedKids);  // the copy owns what it holds
  e.node = NewNode();
  t.elems.push_back(e);
  const auto id = static_cast<ElemId>(t.elems.size() - 1);
  if (e.attrCount) {
    // Without the ids Word gives each paragraph and table row (one id space): the copy would use
    // them again (d3-torture finding 20).
    const TypeId type = TypeOfElem(t, id);
    const AttrDefId paraId = S().AttrOf(type, Name("w14:paraId")), textId = S().AttrOf(type, Name("w14:textId"));
    std::vector<AttrVal> attrs;
    for (uint16_t i = 0; i < e.attrCount; ++i)
      if (const AttrVal& a = t.attrs[e.attrs + i]; a.def != paraId && a.def != textId) attrs.push_back(a);
    if (attrs.size() != e.attrCount) WriteAttrs(id, attrs);
  }
  if (NameOfElem(t, id) == names::kP) {
    // A paragraph of the new story.
    Paragraph p;
    p.elem = id;
    p.story = story;
    p.text = doc_.paras_[t.elems[src].start].text;
    t.elems[id].start = static_cast<uint32_t>(doc_.paras_.size());
    doc_.paras_.push_back(std::move(p));
  }
  if (RealNode(from)) {
    if (pkg::AttrSetId set = doc_.store_.NodeAttributes(from)) {
      // Word's ids for paragraphs and rows (w14:paraId, w14:textId) are kept as the file has them,
      // outside the schema: the copy goes without them, or an id is used twice (d3-torture finding 20).
      std::vector<pkg::XmlAttr> kept = doc_.store_.Attributes(set);
      const size_t before = kept.size();
      std::erase_if(kept, [](const pkg::XmlAttr& a) {
        const std::string_view local = std::string_view(a.qname).substr(a.qname.find(':') + 1);
        return a.nsUri == "http://schemas.microsoft.com/office/word/2010/wordml" && (local == "paraId" || local == "textId");
      });
      if (kept.size() != before) set = kept.empty() ? 0 : doc_.store_.Intern(std::move(kept));
      if (set) doc_.store_.SetNodeAttributes(t.elems[id].node, set);
    }
    if (std::vector<pkg::Fragment> kept = doc_.store_.FragmentsCopy(from); !kept.empty())
      doc_.store_.SetFragments(t.elems[id].node, std::move(kept));
  }
  const uint32_t name = NameOfElem(t, src);
  ElemId tail = kNoElem;
  for (ElemId c = srcFirst; c != kNoElem; c = t.elems[c].next) {
    if (LeftOutOfCopy(NameOfElem(t, c), name)) continue;
    const ElemId k = CopyTree(c, id, story);
    if (tail == kNoElem) t.elems[id].first = k;
    else t.elems[tail].next = k;
    tail = k;
  }
  return id;
}

uint32_t TreeEdit::NewStoryPart(PartKind kind, uint32_t copyOf, std::string& relId, std::u16string& why) {
  why.clear();
  const Part* main = doc_.FindPart(PartKind::Document);
  if ((kind != PartKind::Header && kind != PartKind::Footer) || depth_ > 0 || !main || !doc_.Editable()) {
    why = u"This document can't get a new header or footer.";
    return kNone;
  }
  const bool footer = kind == PartKind::Footer;
  const Story* source = copyOf < doc_.stories_.size() ? &doc_.stories_[copyOf] : nullptr;
  if (source && !Copyable(source->root, why)) return kNone;
  const auto w = static_cast<size_t>(Ns::W), r = static_cast<size_t>(Ns::R);
  const bool strict = IsStrictUri(main->uri[w]);
  const std::string local = footer ? "footer" : "header";
  const std::string mainName = main->name;
  const std::string dir = mainName.substr(0, mainName.rfind('/') + 1);
  pkg::Package& package = doc_.Package();
  const std::string name = FreeName(package, dir, local, true);
  relId = AddToPackage(package, mainName, name,
                       "application/vnd.openxmlformats-officedocument.wordprocessingml." + local + "+xml",
                       RelType(strict, local));

  Part part;
  part.name = name;
  part.kind = kind;
  part.docNode = NewNode();
  const auto partIndex = static_cast<uint32_t>(doc_.parts_.size());
  const auto story = static_cast<uint32_t>(doc_.stories_.size());
  const StoryKind storyKind = footer ? StoryKind::Footer : StoryKind::Header;
  if (source) {
    // The source part's namespaces and what surrounds its root; its relationships too, for its
    // links (the same ids, in the new part's own set).
    const Part& from = doc_.parts_[source->part];
    const ElemId srcRoot = source->root;
    part.prefix = from.prefix;
    part.uri = from.uri;
    part.addDecls = from.addDecls;
    if (std::vector<pkg::Fragment> around = doc_.store_.FragmentsCopy(from.docNode); !around.empty())
      doc_.store_.SetFragments(part.docNode, std::move(around));
    if (const pkg::RelationshipSet* rels = package.RelationshipsOf(from.name); rels && !rels->items.empty()) {
      pkg::RelationshipSet copy;
      copy.source = name;
      copy.relsPart = pkg::RelsPartFor(name);
      copy.items = rels->items;
      package.SetRelationships(std::move(copy));
    }
    doc_.stories_.push_back(Story{storyKind, partIndex, kNoElem, {}, kNone});
    part.root = CopyTree(srcRoot, kNoElem, story);
  } else {
    part.root = doc_.tree_.Add(S().Child(S().RootType(), NameOf(Ns::W, footer ? "ftr" : "hdr")), kNoElem);
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
    doc_.stories_.push_back(Story{storyKind, partIndex, kNoElem, {}, kNone});
    // One empty paragraph.
    const ElemId p = doc_.tree_.Add(S().Child(TypeOfElem(doc_.tree_, part.root), names::kP), part.root);
    doc_.tree_.elems[p].node = NewNode();
    doc_.tree_.elems[part.root].first = p;
    doc_.tree_.elems[p].start = static_cast<uint32_t>(doc_.paras_.size());
    doc_.tree_.elems[p].length = 0;
    Paragraph para;
    para.elem = p;
    para.story = story;
    doc_.paras_.push_back(std::move(para));
  }
  doc_.stories_[story].root = part.root;
  doc_.parts_.push_back(std::move(part));
  std::string xml;
  if (docx::WritePart(doc_, partIndex, xml).Ok()) package.SetPart(name, std::vector<uint8_t>(xml.begin(), xml.end()));
  Reindex();
  return story;
}

// ---------- Comments ----------

uint32_t TreeEdit::NewComment(std::string_view id, std::string_view author, std::string_view initials,
                              std::string_view date, std::string_view paraId) {
  const Part* part = doc_.FindPart(PartKind::Comments);
  if (!part || depth_ > 0 || !doc_.Editable()) return kNone;
  const auto partIndex = static_cast<uint32_t>(part - doc_.parts_.data());
  Tree& t = doc_.tree_;
  const ElemId root = part->root;
  // Its paragraphs' w14:paraId needs the prefix (a comments part from elsewhere may not declare it).
  if (Part& p = doc_.parts_[partIndex]; p.prefix[static_cast<size_t>(Ns::W14)].empty()) {
    const auto w14 = static_cast<size_t>(Ns::W14);
    p.prefix[w14] = std::string(NsPrefix(Ns::W14));
    p.uri[w14] = std::string(NsUri(Ns::W14));
    p.addDecls.emplace_back(p.prefix[w14], p.uri[w14]);
  }
  auto make = [&](ElemId parent, uint32_t name, std::initializer_list<std::pair<const char*, std::string_view>> attrs) {
    const ElemDefId def = S().Child(TypeOfElem(t, parent), name);
    if (def == kNoDef) return kNoElem;
    const ElemId id = t.Add(def, parent);
    t.elems[id].node = NewNode();
    std::vector<AttrVal> vals;
    const TypeDef& type = S().Type(S().Elem(def).type);
    for (const auto& [n, v] : attrs) {
      const AttrDefId ad = S().AttrOf(S().Elem(def).type, Name(n));
      if (ad == kNoDef || v.empty()) continue;
      auto at = std::find_if(vals.begin(), vals.end(), [&](const AttrVal& o) { return AttrRank(type, o.def) > AttrRank(type, ad); });
      vals.insert(at, AttrVal{ad, t.strings.Intern(v)});
    }
    if (!vals.empty()) {
      t.elems[id].attrs = static_cast<uint32_t>(t.attrs.size());
      t.elems[id].attrCount = static_cast<uint16_t>(vals.size());
      t.attrs.insert(t.attrs.end(), vals.begin(), vals.end());
    }
    return id;
  };
  auto chain = [&](ElemId parent, std::initializer_list<ElemId> kids) {
    ElemId tail = kNoElem;
    for (ElemId k : kids) {
      if (k == kNoElem) continue;
      if (tail == kNoElem) t.elems[parent].first = k;
      else t.elems[tail].next = k;
      tail = k;
    }
  };
  const ElemId comment = make(root, Name("comment"), {{"id", id}, {"author", author}, {"date", date}, {"initials", initials}});
  if (comment == kNoElem) return kNone;
  const ElemId p = make(comment, names::kP, {{"w14:paraId", paraId}, {"w14:textId", "77777777"}});
  // Word's comment styles, where the document has them.
  ElemId pPr = kNoElem, rPr = kNoElem;
  if (doc_.FindStyle("CommentText")) {
    pPr = make(p, Name("pPr"), {});
    chain(pPr, {make(pPr, Name("pStyle"), {{"val", "CommentText"}})});
  }
  const ElemId r = make(p, Name("r"), {});
  if (doc_.FindStyle("CommentReference")) {
    rPr = make(r, Name("rPr"), {});
    chain(rPr, {make(rPr, Name("rStyle"), {{"val", "CommentReference"}})});
  }
  const ElemId mark = make(r, Name("annotationRef"), {});
  if (p == kNoElem || r == kNoElem || mark == kNoElem) return kNone;
  chain(comment, {p});
  chain(p, {pPr, r});
  chain(r, {rPr, mark});
  const auto para = static_cast<uint32_t>(doc_.paras_.size());
  t.elems[p].start = para;
  t.elems[p].length = 1;
  t.elems[r].length = t.elems[mark].length = 1;
  const auto story = static_cast<uint32_t>(doc_.stories_.size());
  doc_.stories_.push_back(Story{StoryKind::Comment, partIndex, comment, std::string(id), kNone});
  Paragraph text;
  text.elem = p;
  text.story = story;
  text.text = u"\x05";  // the annotation mark
  doc_.paras_.push_back(std::move(text));
  Reindex();
  return story;
}

void TreeEdit::LinkComment(uint32_t story) {
  const Part* part = doc_.FindPart(PartKind::Comments);
  if (!part || story >= doc_.stories_.size()) return;
  Link(part->root, doc_.stories_[story].root, kNoElem);
}

void TreeEdit::UnlinkComment(uint32_t story) {
  if (story < doc_.stories_.size() && Attached(doc_.stories_[story].root)) RemoveElement(doc_.stories_[story].root);
}

void TreeEdit::ChangeCommentsEx(const std::function<void(std::vector<Document::CommentEx>&)>& change) {
  StoreOp op;
  op.kind = StoreOp::CommentsEx;
  op.exBefore = doc_.commentsEx_;
  change(doc_.commentsEx_);
  op.exAfter = doc_.commentsEx_;
  store_.push_back(std::move(op));
  // The comments part is written again, and its companions with it.
  if (const Part* part = doc_.FindPart(PartKind::Comments)) Touch(part->root);
}

void TreeEdit::SetCommentState(std::string_view paraId, std::string_view parentParaId, bool done) {
  ChangeCommentsEx([&](std::vector<Document::CommentEx>& all) {
    for (Document::CommentEx& ex : all)
      if (ex.paraId == paraId) {
        ex.parentParaId = std::string(parentParaId);
        ex.done = done;
        return;
      }
    all.push_back(Document::CommentEx{std::string(paraId), std::string(parentParaId), done});
  });
}

void TreeEdit::RemoveCommentState(std::string_view paraId) {
  ChangeCommentsEx([&](std::vector<Document::CommentEx>& all) {
    std::erase_if(all, [&](const Document::CommentEx& ex) { return ex.paraId == paraId; });
  });
}

bool TreeEdit::CommentState(std::string_view paraId, std::string& parentParaId, bool& done) const {
  for (const Document::CommentEx& ex : doc_.commentsEx_)
    if (ex.paraId == paraId) {
      parentParaId = ex.parentParaId;
      done = ex.done;
      return true;
    }
  return false;
}

}  // namespace fw::doc::edit
