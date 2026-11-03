#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pkg/Limits.h"
#include "pkg/Snapshot.h"
#include "pkg/Status.h"
#include "pkg/Zip.h"

namespace fw::pkg {

inline constexpr std::string_view kRelationshipsNs = "http://schemas.openxmlformats.org/package/2006/relationships";
inline constexpr std::string_view kContentTypesNs = "http://schemas.openxmlformats.org/package/2006/content-types";
inline constexpr std::string_view kContentTypesPart = "/[Content_Types].xml";

struct Relationship {
  std::string id, type, target;
  bool external = false;
  std::string targetPart;  // internal targets: the part name the target resolves to
};

struct RelationshipSet {
  std::string source;    // "/" for the package itself, else the source part's name
  std::string relsPart;  // e.g. "/word/_rels/document.xml.rels"
  std::vector<Relationship> items;

  const Relationship* Find(std::string_view id) const;
  std::string Serialize() const;
};

struct ContentTypes {
  std::vector<std::pair<std::string, std::string>> defaults;   // extension -> content type
  std::vector<std::pair<std::string, std::string>> overrides;  // part name -> content type

  std::string_view Lookup(std::string_view partName) const;  // empty when there is none
  std::string_view LookupDefault(std::string_view partName) const;  // by extension only
  std::string Serialize() const;
};

// A part of the package. Directory entries of the zip are not parts, but they are kept.
struct Part {
  static constexpr size_t kNoEntry = static_cast<size_t>(-1);

  std::string name;          // "/word/document.xml"; [Content_Types].xml is listed too
  size_t entry = kNoEntry;   // index into the zip entries; kNoEntry for added parts
  bool modified = false;
  bool deleted = false;
  std::vector<uint8_t> data;  // the new content, when modified
};

// An Open Packaging Conventions package (ECMA-376 Part 2) read from a snapshot: parts,
// relationships and content types. Saving copies every part that wasn't changed byte for byte
// from the snapshot, without decompressing it (plan §4.3 rule 1).
class Package {
 public:
  // Reads the zip directory, the content types and every relationship part.
  static std::unique_ptr<Package> Open(std::unique_ptr<Snapshot> snapshot, const Limits& limits, Status& status);

  const Snapshot& GetSnapshot() const { return *snapshot_; }
  const ZipReader& Zip() const { return zip_; }
  const Limits& GetLimits() const { return limits_; }

  const std::vector<Part>& Parts() const { return parts_; }
  // Part names compare case-insensitively (ASCII) and with %-escapes decoded, as OPC says.
  const Part* FindPart(std::string_view name) const;
  Status ReadPart(std::string_view name, std::vector<uint8_t>& out) const;
  const ContentTypes& Types() const { return types_; }
  std::string_view ContentTypeOf(std::string_view partName) const;
  bool IsXmlPart(std::string_view partName) const;

  const std::vector<RelationshipSet>& Relationships() const { return rels_; }
  const RelationshipSet* RelationshipsOf(std::string_view source) const;

  // Edits. Each changed part is written anew on save; everything else is copied raw.
  void SetPart(std::string_view name, std::vector<uint8_t> data);
  void DeletePart(std::string_view name);
  void SetContentTypes(ContentTypes types);
  void SetRelationships(RelationshipSet set);

  // Reads every part, checking sizes and CRCs, and checks that every XML part is well-formed and
  // within the limits. Opening only reads what it needs; this reads everything.
  Status Verify() const;

  // Writes the package: parts in their original order, unchanged ones copied raw, changed ones
  // deflated in their place, new ones at the end.
  Status Save(ByteSink& sink) const;

 private:
  Package() = default;
  Part* FindPartMutable(std::string_view name);
  Status ReadRelationships(const Part& part);
  void IndexTypes();

  std::unique_ptr<Snapshot> snapshot_;
  Limits limits_;
  ZipReader zip_;
  std::vector<Part> parts_;
  std::unordered_map<std::string, size_t> index_;  // PartKey -> latest part with that name
  std::vector<size_t> entryToPart_;  // zip entry -> part index, Part::kNoEntry for directories
  ContentTypes types_;
  std::unordered_map<std::string, size_t> overrideIndex_;  // PartKey -> types_.overrides index
  std::vector<RelationshipSet> rels_;
  std::unordered_map<std::string, size_t> relsIndex_;  // PartKey of the source -> rels_ index
};

// The parsers Package::Open uses for a relationships part (of `source`) and for
// [Content_Types].xml. `label` names the part in messages.
Status ParseRelationships(std::string_view xml, std::string_view source, const Limits& limits, std::string_view label,
                          RelationshipSet& out);
Status ParseContentTypes(std::string_view xml, const Limits& limits, ContentTypes& out);

// "/word/_rels/document.xml.rels" for "/word/document.xml"; "/_rels/.rels" for "/".
std::string RelsPartFor(std::string_view source);
// The inverse: the source of a relationship part, or empty when `name` isn't one.
std::string SourceOfRelsPart(std::string_view name);
// Resolves a relationship target against its source part, as a URI reference (RFC 3986).
std::string ResolveTarget(std::string_view source, std::string_view target);
// How part names compare: %-escapes decoded, ASCII letters lower-cased.
std::string PartKey(std::string_view name);

}  // namespace fw::pkg
