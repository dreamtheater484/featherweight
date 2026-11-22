#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "pkg/References.h"
#include "slide/Internal.h"
#include "slide/edit/Edit.h"
#include "slide/edit/Parts.h"

// What the edit layer's files share. Internal to src/slide/edit.
namespace fw::slide::edit {

using namespace slide::detail;

inline constexpr std::string_view kRelNs = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
inline constexpr std::string_view kRelSlide = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide";
inline constexpr std::string_view kRelLayout = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout";
inline constexpr std::string_view kRelMaster = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster";
inline constexpr std::string_view kRelNotes = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/notesSlide";
inline constexpr std::string_view kRelNotesMaster = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/notesMaster";
inline constexpr std::string_view kRelTheme = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme";
inline constexpr std::string_view kRelImage = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image";
inline constexpr std::string_view kRelHyperlink = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink";
inline constexpr std::string_view kTypeSlide = "application/vnd.openxmlformats-officedocument.presentationml.slide+xml";
inline constexpr std::string_view kTypeNotes = "application/vnd.openxmlformats-officedocument.presentationml.notesSlide+xml";
inline constexpr std::string_view kTypeNotesMaster = "application/vnd.openxmlformats-officedocument.presentationml.notesMaster+xml";
inline constexpr std::string_view kTypeLayout = "application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml";
inline constexpr std::string_view kTypeMaster = "application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml";
inline constexpr std::string_view kTypeTheme = "application/vnd.openxmlformats-officedocument.theme+xml";

// One undo step: the state before it of everything it touched (swapped with the state after it by
// undo, and back by redo).
struct Step {
  std::u16string name;
  std::map<std::string, std::optional<PartState>> parts;  // by PartKey; nullopt: the package's
  std::map<std::string, std::optional<RelsPtr>> rels;     // by the source's PartKey
  bool typesSaved = false;
  TypesPtr types;
  bool full = false;      // the model reads everything again (a master, layout, theme, the size)
  uint64_t serial = 0;    // which state this is, for Modified()
  bool typing = false;    // open for more typing (coalescing)
  TextTarget target;      // typing: where
  std::map<uint32_t, std::set<uint32_t>> shapes;  // slide id -> shape ids touched, for the view
  std::map<std::string, std::string> names;       // PartKey (rels: "rels:" + key) -> name as written
  bool Empty() const { return parts.empty() && rels.empty() && !typesSaved; }
};

struct Editor::Impl {
  Impl(Editor& editor, PartStore& store);

  Editor& ed;
  Presentation& deck;
  PartStore& store;
  const pkg::ReferenceRegistry& registry;

  // ---- The open call's work: copies being edited, published together when it succeeds.
  struct Work {
    std::map<std::string, std::shared_ptr<pkg::XmlDom>> doms;  // by PartKey
    std::map<std::string, PartState> parts;                   // other content set, or removed
    std::map<std::string, std::string> names;                 // PartKey -> name as written
    std::map<std::string, std::shared_ptr<pkg::RelationshipSet>> rels;
    std::shared_ptr<pkg::ContentTypes> types;
    bool full = false;
    std::map<std::string, std::set<uint32_t>> shapes;  // slide part key -> shapes touched
    // Back to the file's own (a journal's replay): the store's edit of these goes, so a save copies
    // them byte for byte again. PartKey -> name.
    std::map<std::string, std::string> originalParts, originalRels;
    bool typesOriginal = false;
  } work;
  int calls = 0;  // Editor calls under way: only the outermost publishes

  // Reading what's there now (the open call's work included).
  pugi::xml_node Root(std::string_view part) const;  // null when missing or not XML
  DomPtr Current(std::string_view part) const;
  mutable std::map<std::string, std::pair<DomPtr, const void*>> parsed;  // package parts read by operations
  bool Exists(std::string_view part) const;
  const pkg::RelationshipSet* Rels(std::string_view source) const;
  std::string ContentType(std::string_view part) const;
  bool Read(std::string_view part, std::vector<uint8_t>& out) const;

  // Writing, inside a call.
  pugi::xml_node Edit(std::string_view part);  // the part's root to change; null when it isn't XML
  pkg::RelationshipSet& EditRels(std::string_view source);
  pkg::ContentTypes& EditTypes();
  void PutXml(std::string_view part, std::shared_ptr<pkg::XmlDom> dom, std::string_view contentType);
  void PutBytes(std::string_view part, std::vector<uint8_t> bytes, std::string_view contentType);
  void Remove(std::string_view part);  // the part, its relationships and its content type override
  // A part, a part's relationships, the content types: back to the file's own (journal replay).
  void Original(std::string_view part);
  void OriginalRels(std::string_view source);
  void OriginalTypes();
  void Full() { work.full = true; }
  void Touched(std::string_view slidePart, uint32_t shape);

  // ---- Steps
  std::vector<Step> undo, redo;
  std::vector<Step> open;  // the groups open, outermost first: each one's state before it
  Changes changes;       // what the open step changed so far, for the listeners
  uint64_t serial = 0, savedSerial = 0;
  bool typingOpen = false;  // the top undo step may take more typing

  void BeginCall();
  // Ends a call: the outermost publishes the work when ok, else drops it.
  Result EndCall(Result r);
  void Publish();
  void Record(Step& step, const std::string& name, bool rels);  // the state before, once per step
  void Refresh(const std::set<std::string>& keys, bool full, Changes& out);
  void Open(std::u16string name);
  void Close();
  void Cancel();
  // A refused call that had reopened the typing step: the step goes back to the undo list unchanged.
  void Reshelve();
  void Swap(Step& s, Changes& out);  // undo or redo
  void Notify(Changes c);
  uint64_t StateSerial() const { return undo.empty() ? 0 : undo.back().serial; }
  // Typing into `t` continues the last undo step when nothing came between: it is reopened.
  bool ContinueTyping(const TextTarget& t);

  // ---- Helpers the operations share
  std::string PresentationPart() const { return deck.PresentationPart(); }
  // The slide list as the presentation part has it now: (slide id, relationship id, part).
  struct SlideEntry {
    uint32_t id = 0;
    std::string rel, part;
    pugi::xml_node node;  // its p:sldId in Root(presentation) (read-only)
  };
  std::vector<SlideEntry> SlideList() const;
  std::string SlidePart(size_t index) const;
  Result CheckSlides(const std::vector<size_t>& slides, size_t count) const;
  // A relationship id the set doesn't use ("rId7").
  static std::string NewRelId(const pkg::RelationshipSet& rels);
  std::string AddRel(std::string_view source, std::string_view type, std::string_view target, bool external = false);
  // The first "<stem><n><ext>" that isn't a part now.
  std::string NewName(std::string_view stem, std::string_view ext) const;
  // The target, relative to `source`, that resolves to `part` (OPC relative reference).
  static std::string RelativeTarget(std::string_view source, std::string_view part);
  // A shape element by id in a part's spTree (groups included); null when none. In mc:AlternateContent
  // the chosen branch's element.
  static pugi::xml_node FindShape(pugi::xml_node root, uint32_t id);
  // The next unused shape id in a part (one past the highest).
  static uint32_t NextShapeId(pugi::xml_node root);
  // A shape about to be edited. In an mc:Choice it stays where it is (the block keeps all its branches;
  // the Fallback is never dropped). In the mc:Fallback (no Choice understood) the block is replaced by
  // the Fallback's content, and the shape's copy there is returned.
  static pugi::xml_node Unwrap(pugi::xml_node shape);
};

// One Editor call: an undo step of its own unless a group is open.
class Call {
 public:
  // `typing`: text typed or deleted there, coalesced with the typing before it into one step.
  Call(Editor& ed, std::u16string name, const TextTarget* typing = nullptr)
      : impl_(ed.Internals()), own_(impl_.open.empty() && impl_.calls == 0) {
    if (own_) {
      continued_ = typing && impl_.ContinueTyping(*typing);
      if (!continued_) impl_.Open(std::move(name));
      if (typing) {
        impl_.open.back().typing = true;
        impl_.open.back().target = *typing;
      }
    }
    impl_.BeginCall();
  }
  ~Call() {
    if (!ended_) End(Result::Fail(u"The edit was interrupted."));
  }
  Call(const Call&) = delete;
  Call& operator=(const Call&) = delete;
  Result End(Result r) {
    ended_ = true;
    r = impl_.EndCall(std::move(r));
    // A refused call changed nothing (its work isn't published): a typing step it reopened goes back
    // as it was, with the typing before it.
    if (own_) r.ok ? impl_.Close() : continued_ ? impl_.Reshelve() : impl_.Cancel();
    return r;
  }
  Editor::Impl& impl_;

 private:
  bool own_;
  bool continued_ = false;
  bool ended_ = false;
};

// XML building helpers.
// Text as XML 1.0 can hold it (plan decision, fuzz finding): the characters it doesn't allow are
// dropped (tab, line feed and carriage return stay; with `breaks` also '\v', which the text operations
// turn into a:br), lone surrogates become U+FFFD. Not _xHHHH_: a:t is plain xsd:string.
std::wstring XmlText(std::wstring_view s, bool breaks = false);
// UTF-8 for the XML: XmlText first, so nothing written can make a part that doesn't parse.
std::string Utf8(std::wstring_view s);
std::wstring Wide(std::string_view s);
// Parses a fragment (an element with the prefixes it uses declared) and returns a document holding it.
bool ParseFragment(std::string_view xml, pugi::xml_document& doc);
// Copies `src` into `parent` before `before` (null: at the end), declaring there the namespaces the
// copy uses that `parent`'s scope lacks.
pugi::xml_node InsertCopy(pugi::xml_node parent, pugi::xml_node src, pugi::xml_node before = {});
// The prefix `ns` has in scope at `node` (declared on the root as `fallback` when it has none).
std::string PrefixFor(pugi::xml_node node, std::string_view ns, std::string_view fallback);
// A child element by namespace and local name, without looking into mc:AlternateContent.
pugi::xml_node DirectChild(pugi::xml_node parent, std::string_view ns, std::string_view local);
// Adds a child named prefix:local in schema order: before the first child whose local name is in
// `after` (the names that come after it), else at the end.
pugi::xml_node AddChildOrdered(pugi::xml_node parent, std::string_view qname, std::initializer_list<std::string_view> after);

}  // namespace fw::slide::edit
