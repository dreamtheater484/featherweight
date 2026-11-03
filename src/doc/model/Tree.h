#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "doc/model/Schema.h"
#include "pkg/RoundTrip.h"

namespace fw::doc {

// The elements of a document that the model knows, as one arena per document (plan §6.3).
//
// Every known element (a paragraph, a run, a w:b inside a run's properties, a style, a table cell)
// is an Elem: its schema definition, its known attributes as written, and its known children in
// document order. What the schema doesn't know lives in the round-trip store, attached to the
// element's NodeId. Paragraphs also keep their text as UTF-16 (Document.h), and the inline
// elements inside a paragraph say which range of that text they hold.
//
// Values are kept as written ("1", "true" and "on" stay distinct), so unchanged properties write
// back exactly; Node's typed accessors and the resolver (Props.h) turn them into typed units.

using ElemId = uint32_t;
inline constexpr ElemId kNoElem = 0xFFFFFFFF;
using StrId = uint32_t;  // 0 is the empty string

// Interned strings: attribute values, which repeat a lot ("1", "240", "Heading1").
class StrPool {
 public:
  StrPool();
  StrId Intern(std::string_view s);
  std::string_view Get(StrId id) const { return id < strings_.size() ? std::string_view(strings_[id]) : std::string_view(); }
  size_t Count() const { return strings_.size(); }
  size_t BytesUsed() const { return bytes_; }

 private:
  std::deque<std::string> strings_;  // a deque never moves its elements, so views stay valid
  std::unordered_map<std::string_view, StrId> index_;
  size_t bytes_ = 0;
};

enum ElemFlags : uint8_t {
  kDisplayOnly = 1,      // read from the chosen branch of an mc:AlternateContent block, which is kept
                         // whole and written instead; shown, never written, never edited
  kDetached = 2,         // the root of a tree outside any part's tree (a text box's story)
  kXmlSpaceDefault = 4,  // Text: written as xml:space="default"
};

struct AttrVal {
  AttrDefId def = kNoDef;
  StrId value = 0;
};

struct Elem {
  ElemDefId def = kNoDef;
  uint8_t flags = 0;
  uint16_t attrCount = 0;
  uint32_t attrs = 0;  // first of attrCount entries in Tree::attrs
  ElemId parent = kNoElem, first = kNoElem, next = kNoElem;
  pkg::NodeId node = pkg::kNoNode;  // owner of its kept fragments and unknown attributes
  // Inside a paragraph: the range of paragraph text the element holds (its subtree's text).
  // A w:p element: start is the paragraph's index in Document::Paragraphs().
  uint32_t start = 0, length = 0;
};

struct Tree {
  std::vector<Elem> elems;
  std::vector<AttrVal> attrs;
  StrPool strings;

  ElemId Add(ElemDefId def, ElemId parent);  // appended last among parent's children by the caller
  size_t BytesUsed() const;
};

class Node;

// The children of a Node, for range-for loops.
class Children {
 public:
  class iterator;
  Children(const Tree* t, ElemId first) : tree_(t), first_(first) {}
  iterator begin() const;
  iterator end() const;

 private:
  const Tree* tree_;
  ElemId first_;
};

// A read-only handle to an element. Cheap to copy; valid as long as the document is.
class Node {
 public:
  Node() = default;
  Node(const Tree* tree, ElemId id) : tree_(id == kNoElem ? nullptr : tree), id_(id) {}

  explicit operator bool() const { return tree_ != nullptr; }
  bool operator==(const Node& o) const { return id_ == o.id_ && tree_ == o.tree_; }
  ElemId Id() const { return id_; }
  const Elem& Raw() const { return tree_->elems[id_]; }

  // What it is.
  uint32_t NameHash() const;  // Name("p"), Name("m:oMath"), ...
  bool Is(uint32_t name) const { return tree_ && NameHash() == name; }
  std::string_view LocalName() const;
  Ns Namespace() const;
  TypeId Type() const;
  Role GetRole() const;
  bool DisplayOnly() const { return tree_ && (Raw().flags & kDisplayOnly); }

  // Structure.
  Node Parent() const { return tree_ ? Node(tree_, Raw().parent) : Node(); }
  Node FirstChild() const { return tree_ ? Node(tree_, Raw().first) : Node(); }
  Node Next() const { return tree_ ? Node(tree_, Raw().next) : Node(); }
  Children Kids() const { return Children(tree_, tree_ ? Raw().first : kNoElem); }
  Node Child(uint32_t name) const;  // the first known child with this name
  // Child(a).Child(b)...: Path({Name("spacing")}).
  Node Path(std::initializer_list<uint32_t> names) const;

  // Attributes, as written. Absent attributes are nullopt; unknown ones are in the round-trip store.
  std::optional<std::string_view> Attr(uint32_t name) const;
  std::string_view AttrOr(uint32_t name, std::string_view fallback = {}) const;
  bool Has(uint32_t name) const { return Attr(name).has_value(); }
  size_t AttrCount() const { return tree_ ? Raw().attrCount : 0; }
  const AttrDef& AttrDefAt(size_t i) const;
  std::string_view AttrValueAt(size_t i) const;

  // Typed values (nullopt when absent or malformed).
  std::optional<int64_t> Int(uint32_t name) const;
  std::optional<uint32_t> Hex(uint32_t name) const;
  // A length in twips: a whole number, or a universal measure such as "12pt" or "2.5cm".
  std::optional<int32_t> Twips(uint32_t name) const;
  std::optional<bool> OnOff(uint32_t name) const;
  // This element as a CT_OnOff property (w:b, w:keepNext): present without w:val means on.
  bool On() const;

  // Inside a paragraph: the range of paragraph text it holds.
  uint32_t TextStart() const { return tree_ ? Raw().start : 0; }
  uint32_t TextLength() const { return tree_ ? Raw().length : 0; }

  const Tree* GetTree() const { return tree_; }

 private:
  const Tree* tree_ = nullptr;
  ElemId id_ = kNoElem;
};

class Children::iterator {
 public:
  iterator(const Tree* t, ElemId id) : tree_(t), id_(id) {}
  Node operator*() const { return Node(tree_, id_); }
  iterator& operator++() {
    id_ = tree_->elems[id_].next;
    return *this;
  }
  bool operator!=(const iterator& o) const { return id_ != o.id_; }

 private:
  const Tree* tree_;
  ElemId id_;
};
inline Children::iterator Children::begin() const { return iterator(tree_, first_); }
inline Children::iterator Children::end() const { return iterator(tree_, kNoElem); }

// ST_OnOff: "true", "1", "on", "false", "0", "off".
std::optional<bool> ParseOnOff(std::string_view v);
// A decimal integer, allowing a sign and (as Word does) a fraction, which is rounded.
std::optional<int64_t> ParseInt(std::string_view v);
std::optional<uint32_t> ParseHex(std::string_view v);
// A universal measure ("12pt", "2.5cm", "1in", "3mm", "6pc", "6pi") or a plain number of `unit`,
// in units of 1/perPoint of a point (twips: 20, half-points: 2).
std::optional<int64_t> ParseMeasure(std::string_view v, int perPoint);

}  // namespace fw::doc
