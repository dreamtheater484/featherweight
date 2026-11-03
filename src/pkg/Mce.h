#pragma once

#include <pugixml.hpp>

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace fw::pkg::mce {

// Markup Compatibility and Extensibility (ECMA-376 Part 3), as plan §4.3 rule 1 uses it:
//  - in mc:AlternateContent, the first Choice whose requirements are all understood is used;
//  - elements and attributes in mc:Ignorable namespaces that aren't understood are kept (in the
//    round-trip store), and so are the namespace declarations they need;
//  - a part with an mc:MustUnderstand namespace that isn't understood is shown but not edited;
//  - after an edit inside an AlternateContent block, a stale branch is never kept.

inline constexpr std::string_view kNamespace = "http://schemas.openxmlformats.org/markup-compatibility/2006";

// The namespaces an engine understands. The MCE, xml and xmlns namespaces, and no namespace at
// all, always are.
class Understood {
 public:
  Understood() = default;
  Understood(std::initializer_list<std::string_view> uris);
  void Add(std::string_view uri);
  bool Has(std::string_view uri) const;

 private:
  std::vector<std::string> uris_;
};

// The namespaces a prefix list ("w14 wp14") names, resolved where `element` is. Undeclared
// prefixes are skipped and counted in `undeclared` when it is given.
std::vector<std::string> ResolvePrefixes(pugi::xml_node element, std::string_view list, int* undeclared = nullptr);

// The mc:Ignorable namespaces in effect at `element`: declared on it or on an ancestor.
std::vector<std::string> IgnorableNamespaces(pugi::xml_node element);

// The branch of an mc:AlternateContent element to use: the first mc:Choice whose Requires
// namespaces are all understood, else the mc:Fallback, else none.
pugi::xml_node SelectBranch(pugi::xml_node alternateContent, const Understood& understood);

// What a reader does with an element it meets.
enum class Handling {
  Read,              // understood: read it into the model
  AlternateContent,  // choose a branch with SelectBranch(), keep the whole block
  Keep,              // ignorable and not understood: keep it in the round-trip store
  ProcessContent,    // ignorable, not understood, listed in mc:ProcessContent: read its children
  Unknown,           // not understood and not ignorable: keep it, and don't edit the part
};
Handling Classify(pugi::xml_node element, const Understood& understood);

struct PartReport {
  std::vector<std::string> mustUnderstand;  // listed in mc:MustUnderstand, not understood
  int alternateContent = 0;                 // mc:AlternateContent blocks
  bool Editable() const { return mustUnderstand.empty(); }
};
// Walks a whole part (iteratively, so depth can't overflow the stack).
PartReport CheckPart(pugi::xml_node root, const Understood& understood);

// After an edit inside an AlternateContent block, removes every branch but `branch`, so a stale
// branch is never written next to an edited one. The caller adds a regenerated fallback when it
// can write one.
void KeepOnlyBranch(pugi::xml_node alternateContent, pugi::xml_node branch);

}  // namespace fw::pkg::mce
