#pragma once

#include <pugixml.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fw::pkg {

class RoundTripStore;

inline constexpr std::string_view kOfficeRelationshipsNs =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
inline constexpr std::string_view kStrictRelationshipsNs = "http://purl.oclc.org/ooxml/officeDocument/relationships";

// Kinds of reference (plan §4.3 rule 2). Each kind is one space of values: renaming a value of
// one kind never touches another. The plan's groups map onto them:
//  - relationship ids: RelationshipId
//  - paragraph ids: ParagraphId, TextId
//  - bookmark, comment-range, field, permission and move-range pairs: BookmarkId, BookmarkName,
//    CommentId, CommentDurableId, Field, PermissionId, MoveRangeId, MoveRangeName,
//    CustomXmlRangeId
//  - shape ids and VML anchors: DrawingId, VmlShapeId, VmlShapeType, VmlAnchor
//  - cell ranges and sheet names: CellRange, SheetName, SheetId
//  - slide ids: SlideId, SlideMasterId
// Word's notes, styles, numbering, content controls and data bindings are references too
// (FootnoteId to DataStoreItemId), so pasting and merging documents can keep them right.
enum class RefKind : uint8_t {
  // Every format
  RelationshipId,  // r:id, r:embed, o:relid, ...: an Id in the relationships of the part that holds it
  DrawingId,       // wp:docPr, p:cNvPr and xdr:cNvPr ids, and the connections and animations that name them
  VmlShapeId,      // o:spid in Word; v:shape ids in Excel, which a14:compatExt names
  VmlShapeType,    // v:shapetype ids, which v:shape type="#..." names
  // Word
  ParagraphId,          // w14:paraId on paragraphs and table rows, and the comment parts that name them
  TextId,               // w14:textId (not unique: Word writes 77777777 on many paragraphs)
  BookmarkId,           // w:bookmarkStart / w:bookmarkEnd pairs
  BookmarkName,         // w:bookmarkStart names, and the links and fields that name them
  CommentId,            // w:comment, and its range and reference in the text
  CommentDurableId,     // w16cid:durableId, and w16cex:durableId
  Field,                // w:fldChar begin / separate / end: pairs by nesting, without a value
  PermissionId,         // w:permStart / w:permEnd pairs
  MoveRangeId,          // w:moveFromRange* and w:moveToRange* pairs
  MoveRangeName,        // the names that link a move's source to its destination
  CustomXmlRangeId,     // w:customXml*RangeStart / End pairs (tracked changes to custom XML)
  FootnoteId,           // w:footnote, and the references and settings that name it
  EndnoteId,            // w:endnote, likewise
  StyleId,              // w:styleId, and every style reference
  NumberingId,          // w:num, and w:numId
  AbstractNumberingId,  // w:abstractNum, and w:abstractNumId
  PictureBulletId,      // w:numPicBullet, and w:lvlPicBulletId
  ContentControlId,     // w:sdtPr/w:id
  DataStoreItemId,      // custom XML parts (ds:itemID), and content controls bound to them
  // Excel
  SheetName,  // workbook sheet names, in formulas, defined names, links and charts
  SheetId,    // workbook sheetId
  CellRange,  // cell references and ranges: sqref, ref, formulas
  VmlAnchor,  // the cells VML notes and controls are anchored to: x:Row, x:Column, x:Anchor
  // PowerPoint
  SlideId,        // p:sldId, and the sections that list slides
  SlideMasterId,  // p:sldMasterId and p:sldLayoutId: one space of ids, from 2^31

  Count
};
inline constexpr size_t kRefKindCount = static_cast<size_t>(RefKind::Count);

// What a site does with its value. A site can do several things: w:commentRangeStart refers to a
// comment and opens its range.
enum RefRole : uint8_t {
  kAnchors = 0,  // a position (a cell) that structural edits shift: neither defined nor referred to
  kRefers = 1,  // names a value a kDefines site gives (relationship ids: a relationship of the part)
  kDefines = 2,  // gives a value; unique within the rule's scope when the rule says so
  kOpens = 4,    // opens a pair, closed by a kCloses site with the same value in the same part
  kCloses = 8,
  kInside = 16,  // only valid inside an open pair (a field's separator)
};

// How the value of a site holds its references.
enum class RefSyntax : uint8_t {
  Value,      // the whole value
  List,       // values separated by whitespace
  Fragment,   // "#value" (VML shape types)
  Formula,    // a formula: the sheet names in it (SheetName), or its cell references (CellRange)
  Ranges,     // cell references and ranges separated by whitespace (sqref)
  FieldCode,  // a field instruction: the bookmark it names (REF, PAGEREF, NOTEREF, HYPERLINK \l)
  None,       // no value: the site marks a position (field characters)
};

// One place a reference can live: an attribute, or the text of an element. Names are views of
// string literals: a site never owns them.
struct RefSite {
  std::string_view elementNs, elementLocal;  // empty local: any element in elementNs (any at all if both are empty)
  std::string_view attrNs, attrLocal;        // empty local: any attribute in attrNs; both empty: the element's text
  uint8_t role = kRefers;
  RefSyntax syntax = RefSyntax::Value;
  std::string_view parentLocal;  // only when the parent (in the element's namespace) has this local name
  std::string_view whenValue;    // only when the value is this one (w:fldCharType="begin")
  std::string_view skipValue;    // a value that refers to nothing (w:numId w:val="0")
  bool lenient = false;          // Office tolerates a dangling reference here: a warning, not an error
};

// Where a rule's values are defined, unique and resolved. Pairs always close in their own part.
enum class RefScope : uint8_t {
  Part,      // one part (a slide's shape ids, a VML part's shape types)
  Document,  // one document: every part of the package, except a glossary's, which is a document of its own
};

// The kind of document a rule applies to (fwtool check applies only the rules for its file).
enum class DocFormat : uint8_t { Any, Word, Excel, PowerPoint };

// One space of values: every site that defines, opens, closes or refers to one.
struct RefRule {
  RefKind kind = RefKind::RelationshipId;
  std::string_view name;  // for messages, e.g. "bookmark id"
  std::vector<RefSite> sites;
  DocFormat format = DocFormat::Any;
  RefScope scope = RefScope::Document;
  bool unique = false;      // no two definitions or pair openings in the scope share a value
  bool nested = false;      // pairs match by nesting, not by value (fields)
  bool ignoreCase = false;  // values compare without ASCII case (sheet and bookmark names, GUIDs)
  bool lenient = false;     // Office tolerates every problem with these: warnings, not errors
  bool (*valid)(std::string_view) = nullptr;  // the form a defined value must have
};

struct FoundRef {
  RefKind kind = RefKind::RelationshipId;
  const RefRule* rule = nullptr;
  const RefSite* site = nullptr;
  pugi::xml_node element;
  pugi::xml_attribute attr;  // null when the reference is the element's text
  bool alternate = false;    // inside a later branch of mc:AlternateContent: the same content again

  std::string_view Value() const;
};

// How an extension (an extLst/ext element, identified by its uri) relates to references.
enum class ExtensionClass {
  ReferenceFree,  // holds no references: kept as is through any edit
  Registry,       // its references are covered by registry rules, so Rename reaches them
  Unknown,        // might hold references: a structural edit that could affect it is blocked
};

// An extension Office writes, from [MS-XLSX], [MS-DOCX], [MS-PPTX] and [MS-ODRAWXML].
struct KnownExtension {
  std::string_view uri;
  std::string_view element;  // what it holds, e.g. "x14:conditionalFormattings"
  ExtensionClass cls;
};
const std::vector<KnownExtension>& OfficeExtensions();

// What a structural edit changes: the values of one kind it renames, moves or removes. No values
// means every value of the kind may change (inserting a row shifts every cell reference below it).
struct EditImpact {
  RefKind kind = RefKind::RelationshipId;
  std::vector<std::string> values;
};

// An unknown extension a structural edit would leave pointing at the wrong place.
struct StrandedExtension {
  std::string uri;
  RefKind kind = RefKind::RelationshipId;
  std::string value;  // the changed value it mentions; empty when every value of the kind changes
};

// The reference registry: for every kind of reference, every place it can live, including
// inside content Featherweight keeps without understanding it. Every structural operation
// updates all of them through Rename(), and fwtool check verifies them independently.
class ReferenceRegistry {
 public:
  // The rules Featherweight knows: Word's in full; Excel's and PowerPoint's as far as the corpus
  // needs (see the TODOs in ReferenceRules.cpp). Office's extensions are classified.
  static ReferenceRegistry Default();

  void AddRule(RefRule rule);
  const std::vector<RefRule>& Rules() const { return rules_; }

  void AddExtension(std::string uri, ExtensionClass c);
  // URIs compare without ASCII case: Office writes the same GUID in both cases.
  ExtensionClass ClassifyExtension(std::string_view uri) const;

  // Every reference in a DOM subtree, in document order (walked iteratively). Empty values aren't
  // references.
  std::vector<FoundRef> Find(pugi::xml_node root, std::optional<RefKind> kind = std::nullopt) const;

  // Models register a hook to update their own typed fields when a reference changes.
  using Hook = std::function<void(RefKind kind, std::string_view from, std::string_view to)>;
  void AddHook(Hook hook);

  // Renames every reference of `kind` from `from` to `to`: in the DOM under `root`, in the
  // round-trip store's fragments and attribute sets (when `store` is given), and in the models
  // through the hooks. Inside formulas, sheet names are renamed and quoted as needed. Cell
  // ranges aren't renamed but shifted, which is the spreadsheet engine's work. Returns how many
  // places in the DOM and the store changed.
  size_t Rename(RefKind kind, std::string_view from, std::string_view to, pugi::xml_node root,
                RoundTripStore* store) const;
  // The same, under `root` only: no store, no hooks. For content not yet in a model (pasting).
  size_t RenameInTree(RefKind kind, std::string_view from, std::string_view to, pugi::xml_node root) const;

  // Would a structural edit leave an unknown extension under `root` pointing at the wrong place?
  // Returns the unknown extensions that mention a value the edit changes, or every unknown one
  // when the edit changes every value of a kind. Editors block such edits with a banner.
  std::vector<StrandedExtension> WouldStrand(pugi::xml_node root, const std::vector<EditImpact>& edit) const;
  // The URIs of the unknown extensions under `root`, in document order.
  std::vector<std::string> UnknownExtensions(pugi::xml_node root) const;

 private:
  struct SiteRef {
    uint16_t rule, site;
  };
  void Index(size_t rule);
  size_t RenameDom(RefKind kind, std::string_view from, std::string_view to, pugi::xml_node root) const;

  std::vector<RefRule> rules_;
  std::unordered_map<std::string_view, std::vector<SiteRef>> attrSites_;  // by attribute local name
  std::vector<SiteRef> nsAttrSites_;                                       // any attribute in a namespace
  std::unordered_map<std::string_view, std::vector<SiteRef>> textSites_;  // by element local name
  std::vector<std::pair<std::string, ExtensionClass>> extensions_;
  std::vector<Hook> hooks_;
};

// The values a reference holds for its kind: the whole value, the items of a list, the sheet
// names in a formula, the bookmark a field names, ... Views into `value`.
void ForEachRefValue(const RefSite& site, RefKind kind, std::string_view value,
                     const std::function<void(std::string_view)>& fn);

// Gives pasted content new ids where ids must be unique (plan §4.3 rule 2, pasting): paragraph
// ids, bookmark ids and names, comment ids, drawing and VML shape ids, notes, content controls,
// and the ids of every pair. Only values the pasted content defines or opens are renewed, so a link to a
// bookmark that stays behind still points at it; comments and notes, whose definitions live in
// other parts, are renewed wherever the pasted content names them, and the caller copies the
// definitions with the same renames.
class IdRenewer {
 public:
  explicit IdRenewer(const ReferenceRegistry& registry);

  // Records every id under `root` (the document being pasted into) as taken.
  void Take(pugi::xml_node root);
  void Take(RefKind kind, std::string_view value);

  struct Renamed {
    RefKind kind;
    std::string from, to;
  };
  // Renames the ids under `pasted` that collide with taken ones, then takes all of them.
  std::vector<Renamed> Renew(pugi::xml_node pasted);

 private:
  bool Renews(RefKind kind, uint8_t role) const;
  std::string Key(RefKind kind, std::string_view value) const;
  std::string NewValue(RefKind kind, std::string_view old, const std::set<std::string>& avoid);

  const ReferenceRegistry& registry_;
  std::array<std::set<std::string>, kRefKindCount> taken_;
  uint32_t counter_ = 0;
};

// The forms values must have.
bool IsLongHexId(std::string_view v);       // 8 hex digits below 80000000 (w14:paraId)
bool IsUnsignedId(std::string_view v);      // decimal digits that fit 32 bits
bool IsSlideId(std::string_view v);         // 256 to 2147483647
bool IsSlideMasterId(std::string_view v);   // 2147483648 to 4294967295

}  // namespace fw::pkg
