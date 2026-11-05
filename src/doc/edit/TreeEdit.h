#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "doc/model/Document.h"

namespace pugi {
class xml_node;
}

namespace fw::doc::edit {

// Changes to a document, as transactions with undo and redo (plan §4.3 rule 4).
//
// This is the thin stand-in formatting uses until D2's edit API (claude/d2-edit) arrives: the
// primitives formatting needs, journaled so that undo and redo restore either state exactly.
//  - A transaction records each element it touches as it was before and as it is after, the
//    elements and attributes it adds (at the arena's end), and what it did to the round-trip store.
//    Undo puts the old elements back and drops the new ones; redo does the opposite.
//  - Attribute blocks are never changed in place: a change writes a new block, so elements may share
//    one (a split run keeps its rsids).
//  - Shared property sets (kSharedKids) are copied before they change.
// Nothing else changes the model. After each commit, undo and redo the document's caches and
// indexes are rebuilt, and the paragraphs touched get a new version.
class TreeEdit {
 public:
  explicit TreeEdit(Document& doc);
  TreeEdit(const TreeEdit&) = delete;
  TreeEdit& operator=(const TreeEdit&) = delete;

  const Document& Doc() const { return doc_; }

  // ---- Transactions
  // Changes are grouped under a name ("Bold") for undo. Begin and Commit nest; only the outermost
  // pair counts. A transaction that changed nothing isn't recorded.
  void Begin(std::wstring label);
  void Commit();
  bool InTransaction() const { return depth_ > 0; }
  bool CanUndo() const { return !undo_.empty(); }
  bool CanRedo() const { return !redo_.empty(); }
  std::wstring UndoLabel() const { return undo_.empty() ? std::wstring() : undo_.back().label; }
  std::wstring RedoLabel() const { return redo_.empty() ? std::wstring() : redo_.back().label; }
  bool Undo();
  bool Redo();
  // Changes with every commit, undo and redo: views lay out again when it does.
  uint32_t Version() const { return version_; }

  // ---- Primitives (inside a transaction). Element ids stay valid; Node handles too.
  // The known child of `parent` with this name, after giving `parent` its own copy of a shared
  // property set. Made where the schema puts it when missing and `create`; kNoElem otherwise.
  ElemId Child(ElemId parent, uint32_t name, bool create = true);
  // Child(Child(a, x), y)...
  ElemId Path(ElemId parent, std::initializer_list<uint32_t> names, bool create = true);
  // Removes a known child that holds no text (a property, a style). Its kept fragments go with it.
  void Remove(ElemId parent, uint32_t name);
  void RemoveElement(ElemId elem);
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
  // Paragraphs whose look the transaction changed (formatting their runs or marks). Changes to
  // styles, numbering or settings touch every paragraph.
  void TouchPara(uint32_t para);
  void TouchAll();
  // Lets the model's indexes see what the transaction has added so far (a style, a list).
  void Reindex();

  Tree& GetTree() { return doc_.tree_; }

  // The root of the document's styles or numbering part (outside a transaction). A document
  // without one gets an empty part, with its content type and relationship; that isn't undone
  // (an empty part says nothing), and redo history ends there. kNoElem for other kinds, or when
  // the document isn't edited.
  ElemId EnsurePart(PartKind kind);

 private:
  struct StoreOp {
    enum Kind : uint8_t { Split, SetAttrs } kind = Split;
    pkg::NodeId node = pkg::kNoNode, second = pkg::kNoNode;
    pkg::SplitPoint at;
    pkg::AttrSetId before = 0, after = 0;  // SetAttrs
  };
  struct Txn {
    std::wstring label;
    size_t elems = 0, attrs = 0;  // arena sizes at Begin
    std::vector<std::pair<ElemId, Elem>> before, after;
    std::vector<Elem> added;
    std::vector<AttrVal> addedAttrs;
    std::vector<StoreOp> store;
    std::vector<uint32_t> paras;
    bool all = false;
    bool Empty() const { return before.empty() && added.empty() && addedAttrs.empty() && store.empty(); }
  };

  Elem& Touch(ElemId id);  // journals the element's state before its first change
  Elem& E(ElemId id) { return doc_.tree_.elems[id]; }
  pkg::NodeId NewNode() { return nextNode_++; }
  ElemId Own(ElemId container);
  ElemId Clone(ElemId src, ElemId parent);
  ElemId Insert(ElemId parent, ElemDefId def, ElemId before);  // a new empty element
  void Link(ElemId parent, ElemId child, ElemId before);       // before kNoElem: schema order
  void Append(ElemId parent, ElemId child);                    // last among its siblings
  // An element and its subtree from checked XML; linked before `before`, or last when `last`.
  ElemId Build(ElemId parent, const pugi::xml_node& n, ElemId before, bool last);
  void WriteAttrs(ElemId elem, const std::vector<AttrVal>& attrs);
  void StoreSplit(pkg::NodeId node, pkg::NodeId second, pkg::SplitPoint at);
  void StoreSetAttrs(pkg::NodeId node, pkg::AttrSetId set);
  void Invalidate(const Txn& txn);

  Document& doc_;
  pkg::NodeId nextNode_ = 1;
  int depth_ = 0;
  Txn txn_;
  std::unordered_map<ElemId, size_t> touched_;  // element -> index into txn_.before
  std::vector<Txn> undo_, redo_;
  uint32_t version_ = 0;
};

}  // namespace fw::doc::edit
