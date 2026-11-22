#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "draw/Drawing.h"
#include "pkg/Package.h"
#include "pkg/XmlDom.h"

namespace fw::slide {
class Presentation;
struct SlideBase;
struct TextBody;
struct Table;
}  // namespace fw::slide

// The parts of a presentation as its edits left them (plan §4.3 rules 1, 4 and 5; P2). The package
// stays the file as it was opened and is never changed, so a save can read it on a worker thread
// while editing goes on. Over it, the store holds every part an edit touched:
//   - XML parts as DOMs, which never change once published here: an edit works on a copy and
//     publishes it, so an undo step, a save or the model can keep a version for as long as it
//     needs (copy-on-write at the part level);
//   - other parts (pictures, media) as bytes;
//   - relationship sets and content types as their structures.
// Undo and redo swap these states; the model reads them (Presentation::RelationshipsOf, ReadPart);
// the writer saves them with everything else copied raw. Internal to fwslide's edit layer.
namespace fw::slide::edit {

using DomPtr = std::shared_ptr<const pkg::XmlDom>;
using BytesPtr = std::shared_ptr<const std::vector<uint8_t>>;
using RelsPtr = std::shared_ptr<const pkg::RelationshipSet>;
using TypesPtr = std::shared_ptr<const pkg::ContentTypes>;

// A part's content as the store holds it. Exactly one of dom and bytes is set, or neither when the
// part was removed.
struct PartState {
  std::string name;  // as written ("/ppt/slides/slide3.xml")
  DomPtr dom;
  BytesPtr bytes;
  bool Removed() const { return !dom && !bytes; }
};

// A copy of a DOM to edit (pugi copies the whole tree, declaration included).
std::shared_ptr<pkg::XmlDom> CloneDom(const pkg::XmlDom& dom);
// A DOM made from bytes, within the package's limits; null when they aren't well-formed XML.
std::shared_ptr<pkg::XmlDom> ParseDom(std::string_view xml, const pkg::Limits& limits, std::string_view label = {});

class PartStore {
 public:
  explicit PartStore(const pkg::Package& package);
  ~PartStore();

  const pkg::Package& Package() const { return package_; }

  // ---- Reading: the store's state, else the package's.
  bool Has(std::string_view part) const;  // the part exists now
  const PartState* Find(std::string_view part) const;  // null: the store doesn't hold it (the package's)
  // An XML part's DOM: the store's, else parsed from the package (not kept). Null when missing or
  // not well-formed.
  DomPtr Dom(std::string_view part) const;
  bool ReadPart(std::string_view part, std::vector<uint8_t>& out) const;
  // Relationships of a part ("/" the package's); null when it has none.
  const pkg::RelationshipSet* Relationships(std::string_view source) const;
  RelsPtr RelationshipsPtr(std::string_view source) const;  // a store's set, null for the package's
  const pkg::ContentTypes& Types() const { return types_ ? *types_ : package_.Types(); }
  std::string ContentTypeOf(std::string_view part) const;
  // Every part name there is now, the package's first in their order, then those edits added.
  std::vector<std::string> PartNames() const;

  // ---- States, for undo and redo: what the store holds for a part (nullopt: nothing, the package's).
  std::optional<PartState> Get(std::string_view part) const;
  void Set(std::string_view part, std::optional<PartState> state);
  std::optional<RelsPtr> GetRels(std::string_view source) const;  // nullopt: the package's
  void SetRels(std::string_view source, std::optional<RelsPtr> rels);
  TypesPtr GetTypes() const { return types_; }  // null: the package's
  void SetTypes(TypesPtr types) { types_ = std::move(types); }

  // Every part and relationship set the store holds, for the writer and the journal.
  const std::map<std::string, PartState>& Parts() const { return parts_; }
  const std::map<std::string, RelsPtr>& RelationshipSets() const { return rels_; }
  bool Empty() const { return parts_.empty() && rels_.empty() && !types_; }

  // Pictures the edits added, decoded once (the package's go through the deck's image cache).
  std::shared_ptr<const draw::Image> Image(std::string_view part, draw::ImageCache& packageImages) const;

  // Things the model replaced, kept until the next edit so pointers taken before it stay valid
  // until its change listeners have run (Presentation.h, "Threads").
  struct Retired {
    std::vector<std::unique_ptr<SlideBase>> parts;
    std::vector<std::unique_ptr<TextBody>> texts;
    std::vector<std::unique_ptr<Table>> tables;
    std::vector<DomPtr> doms;
    std::vector<std::shared_ptr<const draw::Theme>> themes;
  };
  Retired& Graveyard() { return retired_; }
  void Bury();  // frees what the edit before last retired

 private:
  const pkg::Package& package_;
  std::map<std::string, PartState> parts_;  // by pkg::PartKey
  std::map<std::string, RelsPtr> rels_;     // by the source's PartKey ("/" for the package)
  TypesPtr types_;
  mutable std::mutex imagesMutex_;
  mutable std::map<std::string, std::shared_ptr<const draw::Image>> images_;
  Retired retired_, older_;
};

// Drawing resources for a part of an edited deck: pictures and SmartArt drawings through its
// relationships as the edits left them.
class EditedResources : public draw::DrawingResources {
 public:
  EditedResources(const PartStore& store, std::string part, std::shared_ptr<const draw::Theme> theme,
                  draw::ColorMap colorMap, std::shared_ptr<draw::ImageCache> images);
  std::shared_ptr<const draw::Theme> GetTheme() override { return theme_ ? theme_ : draw::Theme::Default(); }
  const draw::ColorMap& GetColorMap() override { return colorMap_; }
  std::shared_ptr<const draw::Image> GetImage(std::string_view relId) override;
  pugi::xml_node GetPartXml(std::string_view relId) override;
  bool InPresentation() override { return true; }

 private:
  std::string Target(std::string_view relId) const;
  const PartStore& store_;
  std::string part_;
  std::shared_ptr<const draw::Theme> theme_;
  draw::ColorMap colorMap_;
  std::shared_ptr<draw::ImageCache> images_;
  std::map<std::string, DomPtr> xml_;
};

}  // namespace fw::slide::edit
