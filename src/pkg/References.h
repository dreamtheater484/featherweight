#pragma once

#include <pugixml.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fw::pkg {

class RoundTripStore;

inline constexpr std::string_view kOfficeRelationshipsNs =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
inline constexpr std::string_view kStrictRelationshipsNs = "http://purl.oclc.org/ooxml/officeDocument/relationships";

// Kinds of reference (plan §4.3 rule 2). Only relationship ids have rules so far; the others
// get theirs with the models that need them.
enum class RefKind : uint8_t {
  RelationshipId,  // r:id, r:embed, ...: an Id in the relationships of the part that holds it
  ParagraphId,     // w14:paraId, w14:textId
  BookmarkPair,    // w:bookmarkStart / w:bookmarkEnd ids
  CommentRange,    // w:commentRangeStart / End / Reference ids
  ShapeId,         // DrawingML and VML shape ids
  CellRange,       // ranges and sheet names, in formulas and extensions
  SlideId,         // p:sldId ids
};

// One place a reference can live: an attribute, on a given element or on any element.
struct RefSite {
  std::string_view elementNs, elementLocal;  // empty local: any element
  std::string_view attrNs, attrLocal;        // empty local: any attribute in attrNs
};

struct RefRule {
  RefKind kind;
  std::string_view name;  // for messages, e.g. "relationship id"
  std::vector<RefSite> sites;
};

struct FoundRef {
  RefKind kind;
  pugi::xml_node element;
  pugi::xml_attribute attr;
};

// How an extension (an extLst/ext element, identified by its uri) relates to references.
enum class ExtensionClass {
  ReferenceFree,  // holds no references: kept as is through any edit
  Registry,       // its references are covered by registry rules
  Unknown,        // might hold references: a structural edit that could affect it is blocked
};

// The reference registry: for every kind of reference, every place it can live, including
// inside content Featherweight keeps without understanding it. Every structural operation
// updates all of them through Rename(), and fwtool check verifies them independently.
class ReferenceRegistry {
 public:
  // The rules Featherweight knows. For now, the sample: relationship ids.
  static ReferenceRegistry Default();

  void AddRule(RefRule rule);
  const std::vector<RefRule>& Rules() const { return rules_; }

  void AddExtension(std::string uri, ExtensionClass c);
  ExtensionClass ClassifyExtension(std::string_view uri) const;

  // Every reference in a DOM subtree (walked iteratively). Empty values aren't references.
  std::vector<FoundRef> Find(pugi::xml_node root, std::optional<RefKind> kind = std::nullopt) const;

  // Models register a hook to update their own typed fields when a reference changes.
  using Hook = std::function<void(RefKind kind, std::string_view from, std::string_view to)>;
  void AddHook(Hook hook);

  // Renames every reference of `kind` from `from` to `to`: in the DOM under `root`, in the
  // round-trip store's fragments and attribute sets (when `store` is given), and in the models
  // through the hooks. Returns how many places in the DOM and the store changed.
  size_t Rename(RefKind kind, std::string_view from, std::string_view to, pugi::xml_node root,
                RoundTripStore* store) const;

 private:
  bool Matches(RefKind kind, pugi::xml_node element, pugi::xml_attribute attr) const;

  std::vector<RefRule> rules_;
  std::vector<std::pair<std::string, ExtensionClass>> extensions_;
  std::vector<Hook> hooks_;
};

}  // namespace fw::pkg
