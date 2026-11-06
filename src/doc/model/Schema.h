#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fw::doc {

// The WordprocessingML vocabulary the document model reads (plan §6.3). Everything outside it is
// kept by the round-trip store and written back where it was (plan §4.3 rule 1).
//
// The schema is a small table of element types, in the spirit of the standard's complex types: each
// type lists the attributes it knows and the children it knows, with their types. The same local
// name can have different types under different parents (w:rPr in a run, in a paragraph's
// properties, in a style). It is built on first use from a compact text table (Schema.cpp), so
// nothing runs at startup.

// Namespaces the model knows. Strict OOXML namespaces read as their transitional twins.
enum class Ns : uint8_t { None, W, R, W14, M, Xml, Mc, Other };
inline constexpr size_t kNsCount = 8;

std::string_view NsUri(Ns ns);       // the transitional URI
std::string_view NsPrefix(Ns ns);    // the prefix Word writes: "w", "r", "w14", ...
Ns NsFromUri(std::string_view uri);  // Other for namespaces the model doesn't read
bool IsStrictUri(std::string_view uri);

// How the model treats a known element.
enum class Role : uint8_t {
  Plain,      // properties and structure: attributes and known children
  Paragraph,  // w:p: its inline content makes up the paragraph's text
  Run,        // w:r
  Text,       // w:t, w:delText, w:instrText, w:delInstrText: their content is paragraph text
  Char,       // a run item that stands for one character of paragraph text (w:tab, w:br, ...)
  Object,     // a drawing, VML picture, embedded object or equation: its content is kept as XML
              // and it stands for one U+FFFC
  Marker,     // a zero-length run item (w:lastRenderedPageBreak)
  Story,      // the root of a story: w:body, w:hdr, w:ftr, w:footnote, w:endnote, w:comment,
              // w:txbxContent
};

// Names are 32-bit hashes of the qualified spelling, with the prefixes of NsPrefix() and no "w:"
// for WordprocessingML itself: "val", "r:id", "w14:paraId", "xml:space", "m:oMath". Code names
// what it reads with Name("spacing"); the schema checks that no two names it knows collide.
constexpr uint32_t Name(std::string_view qualified) {
  uint32_t h = 2166136261u;
  for (char c : qualified) {
    h ^= static_cast<uint8_t>(c);
    h *= 16777619u;
  }
  return h;
}
uint32_t NameOf(Ns ns, std::string_view local);  // the same hash, from a namespace and local name

using TypeId = uint16_t;
using ElemDefId = uint16_t;
using AttrDefId = uint16_t;
inline constexpr uint16_t kNoDef = 0xFFFF;

struct AttrDef {
  Ns ns = Ns::W;
  std::string_view local;
  uint32_t name = 0;
};

// A known element: a name with the type it has where it appears.
struct ElemDef {
  Ns ns = Ns::W;
  std::string_view local;
  uint32_t name = 0;
  TypeId type = 0;
};

struct TypeDef {
  std::string_view name;
  Role role = Role::Plain;
  char16_t ch = 0;  // Char: the character it stands for (w:br, w:sym and w:fldChar depend on attributes)
  std::vector<AttrDefId> attrs;
  std::vector<ElemDefId> children;  // in schema order, which is where new elements go
};

class Schema {
 public:
  static const Schema& Get();

  const TypeDef& Type(TypeId id) const { return types_[id]; }
  const ElemDef& Elem(ElemDefId id) const { return elems_[id]; }
  const AttrDef& Attr(AttrDefId id) const { return attrs_[id]; }
  size_t TypeCount() const { return types_.size(); }

  // The root types of the parts the model reads: w:document, w:hdr, w:ftr, w:footnotes,
  // w:endnotes, w:comments, w:styles, w:numbering, w:settings, w:fonts, and w:txbxContent (the
  // text of a text box, read out of its drawing).
  TypeId RootType() const { return root_; }
  TypeId FindType(std::string_view name) const;  // kNoDef when there is none

  // The known child of a `parent` element with this name, or kNoDef.
  ElemDefId Child(TypeId parent, uint32_t name) const;
  ElemDefId Child(TypeId parent, Ns ns, std::string_view local) const { return Child(parent, NameOf(ns, local)); }
  AttrDefId AttrOf(TypeId type, uint32_t name) const;
  AttrDefId AttrOf(TypeId type, Ns ns, std::string_view local) const { return AttrOf(type, NameOf(ns, local)); }

  // Problems in the table itself (unknown types, colliding names). Empty when it is sound.
  const std::vector<std::string>& Errors() const { return errors_; }

 private:
  Schema();
  void Build(std::string_view text);

  std::vector<TypeDef> types_;
  std::vector<ElemDef> elems_;
  std::vector<AttrDef> attrs_;
  std::vector<std::pair<uint64_t, ElemDefId>> childIndex_;  // (type << 32 | name), sorted
  std::vector<std::pair<uint64_t, AttrDefId>> attrIndex_;
  TypeId root_ = 0;
  std::vector<std::string> errors_;
};

}  // namespace fw::doc
