#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "doc/model/Document.h"

namespace pugi {
class xml_node;
}

namespace fw::doc::edit {

class Editor;
struct JournalStep;

// Direct changes to a document's elements, for formatting, styles and lists (plan §4.3 rule 4).
//
// Text edits replace runs of paragraphs (Editor.h); formatting changes properties in place, often
// across many paragraphs and in the styles and numbering parts. A TreeEdit makes those changes
// with the primitives below and journals them: each element it touches is saved as it was before
// and as it is after (new elements too, which stay in the arena when undone), with what it did to
// the round-trip store. Commit hands the journal to the Editor as one step of its history, so undo
// and redo restore either state exactly, in order with typing.
//  - Attribute blocks are never changed in place: a change writes a new block.
//  - Shared property sets (kSharedKids) are copied before they change.
class TreeEdit {
 public:
  // `caret`: where the caret goes when the change is undone or redone (else the start of the
  // first paragraph it changed).
  explicit TreeEdit(Editor& editor, Pos caret = {});
  TreeEdit(const TreeEdit&) = delete;
  TreeEdit& operator=(const TreeEdit&) = delete;

  const Document& Doc() const { return doc_; }
  Editor& GetEditor() { return editor_; }

  // ---- Transactions
  // A change named for undo ("Bold"). Begin and Commit nest; only the outermost pair counts, and
  // records the change in the editor's history (nothing when nothing changed).
  void Begin(std::u16string label);
  void Commit();
  bool InTransaction() const { return depth_ > 0; }
  // Why the last transaction changed nothing in the end: it touched text the editor doesn't edit
  // (a text box, which is written back from its drawing as it was; content kept as it is; a part
  // the writer can't reproduce). Commit puts everything back then, as Abort does (d2-torture
  // finding 14). Empty when it was recorded.
  const std::u16string& Refused() const { return refused_; }
  // Whether these paragraphs can change: false, with Refused() saying why, where Commit would put a
  // change back. For commands that first add a part with EnsurePart, which isn't undone, so a
  // refused command doesn't leave one behind (d2-torture finding 18).
  bool CanChange(const std::vector<uint32_t>& paras);
  // Its transactions fill in a form (Forms.h): they go ahead in a document protected for forms,
  // where every other change is refused.
  void FormFill() { fill_ = true; }
  // Says the change affects the layout of the paragraphs it touches and no others (or, with
  // `toEnd`, those and the rest of their story: a list's numbers), whatever it adds to the styles
  // and numbering parts. Only their pages are laid out again, not the whole document. It counts in
  // the outermost transaction; Commit checks that nothing else changed.
  void Local(bool toEnd = false) {
    if (depth_ != 1) return;
    local_ = true;
    toEnd_ = toEnd_ || toEnd;
  }
  // Says the change affects the layout of paragraphs [first, last] of one story, whatever it
  // touches (a section's page setup: the section's paragraphs, though only its w:sectPr changes).
  // With `structure`, what follows may read differently too (sections numbered again). It counts in
  // the outermost transaction, and wins over Local.
  void Affects(uint32_t first, uint32_t last, bool structure = false) {
    if (depth_ != 1 || first > last) return;
    affects_ = {first, last};
    structure_ = structure;
  }

  // ---- Primitives (inside a transaction). Element ids stay valid; Node handles too.
  // The known child of `parent` with this name, after giving `parent` its own copy of a shared
  // property set. Made where the schema puts it when missing and `create`; kNoElem otherwise.
  ElemId Child(ElemId parent, uint32_t name, bool create = true);
  // Child(Child(a, x), y)...
  ElemId Path(ElemId parent, std::initializer_list<uint32_t> names, bool create = true);
  // Removes a known child that holds no text (a property, a style). Its kept fragments go with it.
  void Remove(ElemId parent, uint32_t name);
  void RemoveElement(ElemId elem);
  // Values XML can't hold (XmlSafe) are left out.
  void SetAttr(ElemId elem, uint32_t name, std::string_view value);
  void RemoveAttr(ElemId elem, uint32_t name);
  // Removes every child of a property container (w:rPr, w:pPr) except those named in `keep`.
  void Clear(ElemId container, std::initializer_list<uint32_t> keep = {});
  // Makes `offset` a boundary between runs of the paragraph: splits the w:r there, and the w:t
  // inside it. Paragraph ends and existing boundaries need nothing. False when the offset is
  // inside content that isn't edited (an mc:AlternateContent branch shown in place of kept XML).
  bool SplitRunAt(uint32_t para, uint32_t offset);
  // Adds an element made from XML (a w:style, w:abstractNum, w:num, w:lvlOverride) to `parent`,
  // before `before` (kNoElem: where the schema puts it among its siblings, after the last of
  // its kind). The XML uses the w: prefix; every element and attribute in it must be known.
  // kNoElem when it isn't.
  ElemId AddXml(ElemId parent, std::string_view xml, ElemId before = kNoElem);
  // Puts a copy of `src` (a property such as w:jc or w:b, with what it holds) into `parent`, in
  // place of a child with the same name. kNoElem when `parent` can't hold it.
  ElemId CopyChild(ElemId src, ElemId parent);
  // Lets the model's indexes see what the transaction has added so far (a style, a list).
  void Reindex();
  // Puts back everything the transaction changed so far and ends it; nothing is recorded.
  void Abort();

  // ---- Inline content (fields, links, bookmarks). Positions keep their order: what is kept
  // beside the elements (the round-trip store's fragments) stays where it was.
  // A new empty element `name` in `parent` before `before` (kNoElem: last), its text range set
  // to `offset` with no length (a bookmark start or end). kNoElem when `parent` can't hold it.
  ElemId InsertElement(ElemId parent, uint32_t name, ElemId before, uint32_t offset);
  // Puts the siblings from `first` to `last` into a new element `name` (w:fldSimple,
  // w:hyperlink) in their place. kNoElem when their parent can't hold it, or it can't hold them.
  ElemId Wrap(ElemId first, ElemId last, uint32_t name) { return Wrap(first, last, {name}); }
  // The same into nested new elements, outermost first (w:sdt, w:sdtContent): the outermost.
  ElemId Wrap(ElemId first, ElemId last, std::initializer_list<uint32_t> names);
  // Puts an inline container's children in its place and takes it out (removing a link). What it
  // keeps first goes before them, what it keeps after it after them. False when it keeps content
  // between its children: then it stays.
  bool Unwrap(ElemId elem);

  Tree& GetTree() { return doc_.tree_; }

  // The root of the document's styles, numbering or settings part (outside a transaction). A document
  // without one gets an empty part, with its content type and relationship; that isn't undone
  // (an empty part says nothing). kNoElem for other kinds, or when the document isn't edited.
  ElemId EnsurePart(PartKind kind);
  // A new header or footer part (outside a transaction, and not undone: a part nothing refers to
  // says nothing): its content type, its relationship from the main part (`relId`), its story,
  // and its content: a copy of story `copyOf`'s, or one empty paragraph (kNone). The copy leaves
  // out what must be unique in a document (bookmarks, paragraph ids, content control ids). The
  // story's index; kNone with the reason when the document isn't edited, or the content can't be
  // copied (pictures, notes, comments, or kept content that refers to something).
  uint32_t NewStoryPart(PartKind kind, uint32_t copyOf, std::string& relId, std::u16string& why);

 private:
  friend struct JournalStep;
  struct StoreOp {
    enum Kind : uint8_t { Split, SetAttrs, Fragments } kind = Split;
    pkg::NodeId node = pkg::kNoNode, second = pkg::kNoNode;
    pkg::SplitPoint at;
    pkg::AttrSetId before = 0, after = 0;                        // SetAttrs
    std::vector<pkg::Fragment> fragmentsBefore, fragmentsAfter;  // Fragments
  };
  using Saved = std::vector<std::pair<ElemId, Elem>>;
  static void Restore(Document& doc, const Saved& elems);
  static void Replay(Document& doc, const std::vector<StoreOp>& ops, bool undo);

  Elem& Touch(ElemId id);  // journals the element's state before its first change
  Elem& E(ElemId id) { return doc_.tree_.elems[id]; }
  pkg::NodeId NewNode();
  ElemId Own(ElemId container);
  ElemId Clone(ElemId src, ElemId parent);
  // A copy of a property element for new content (a cut run's second half, a style's property).
  // A set that repeats an element exactly (a file's <w:b/><w:b/>, which the schema doesn't allow)
  // is copied without the repeats, so the copy doesn't add an error the file has once already;
  // the original keeps its own. Otherwise a Clone, its children shared.
  ElemId CopyProps(ElemId src, ElemId parent);
  // Whether `c`, in the list from `first`, repeats an element before it exactly; whether any
  // element under `e` does.
  bool Repeats(ElemId first, ElemId c) const;
  bool HasRepeats(ElemId e) const;
  ElemId Insert(ElemId parent, ElemDefId def, ElemId before);  // a new empty element
  void Link(ElemId parent, ElemId child, ElemId before);       // before kNoElem: schema order
  // Links `child` (which isn't linked) into `parent` before `before` (kNoElem: last), leaving
  // kept fragments alone: content in a paragraph is in reading order, not schema order.
  void LinkAt(ElemId parent, ElemId child, ElemId before);
  void Unlink(ElemId elem);  // takes a linked element out of its parent, fragments untouched
  void Append(ElemId parent, ElemId child);                    // last among its siblings
  // An element and its subtree from checked XML; linked before `before`, or last when `last`.
  ElemId Build(ElemId parent, const pugi::xml_node& n, ElemId before, bool last);
  void WriteAttrs(ElemId elem, const std::vector<AttrVal>& attrs);
  void StoreSplit(pkg::NodeId node, pkg::NodeId second, pkg::SplitPoint at);
  void StoreSetAttrs(pkg::NodeId node, pkg::AttrSetId set);
  void StoreFragments(pkg::NodeId node, std::vector<pkg::Fragment> fragments);
  std::vector<uint32_t> PartsOf(const Saved& elems) const;
  // The paragraph an element is in (its index), or kNone.
  uint32_t ParagraphOf(ElemId id) const;
  // Whether an element is in the styles or numbering part.
  bool InDefinitions(ElemId id) const;
  // Whether an element is somewhere the editor edits (Editor's Editable, for any element): not in a
  // text box or kept content, and in a part the writer reproduces. `memo` keeps the answers for
  // the elements passed on the way up.
  bool Editable(ElemId id, std::unordered_map<ElemId, bool>& memo, std::u16string& why) const;
  // Puts back everything the transaction changed (Abort and a refused Commit).
  void Discard();
  // Formatting with Track Changes on (D4): before the first change to a run's or a paragraph's
  // properties (the w:rPr of a w:r, the w:pPr of a w:p and its w:rPr, the mark's), a copy of them
  // as they were; at Commit, a w:rPrChange or w:pPrChange holding it where they differ now and
  // there is no record yet (the oldest stays). Not for the author's own new text.
  void TrackBefore(ElemId at);
  void RecordFormatting();
  ElemId DetachedCopy(ElemId src, ElemId parent);
  bool SameElem(ElemId a, ElemId b) const;
  bool Attached(ElemId id) const;
  // NewStoryPart's copy: whether a story's content can be copied, and the copy of an element and
  // what it holds (new paragraphs join story `story`).
  bool Copyable(ElemId root, std::u16string& why) const;
  ElemId CopyTree(ElemId src, ElemId parent, uint32_t story);

  Editor& editor_;
  Document& doc_;
  Pos caret_;
  int depth_ = 0;
  bool fill_ = false;
  bool local_ = false, toEnd_ = false, structure_ = false;
  std::pair<uint32_t, uint32_t> affects_{kNone, kNone};
  std::u16string label_, refused_;
  size_t elemsAtBegin_ = 0;
  Saved before_;
  std::vector<StoreOp> store_;
  std::unordered_map<ElemId, size_t> touched_;  // element -> index into before_
  std::unordered_set<ElemId> owned_;            // containers given children of their own (Own)
  bool tracking_ = false, recording_ = false;
  std::string trackAuthor_, trackDate_;
  std::vector<std::pair<ElemId, ElemId>> tracked_;  // properties and their copy (kNoElem: not tracked)
  size_t copies_ = 0;  // elements made for those copies, linked nowhere unless a record took them
};

}  // namespace fw::doc::edit
