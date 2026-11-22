#pragma once

#include <map>
#include <string>
#include <string_view>

#include "slide/edit/EditInternal.h"

// Copying slides and the parts they own (duplicate, copy between decks), and slide helpers the
// operations share. Internal to src/slide/edit.
namespace fw::slide::edit {

// Copies a slide part with what it owns into the deck `m` edits: charts and their workbooks,
// SmartArt, embedded objects, tags, its notes page; pictures, media and its layout are shared
// within a deck, and copied (stored once) from another deck. Comments don't come along.
class SlideCopier {
 public:
  // `source`: the deck the slides come from; null for the deck being edited.
  SlideCopier(Editor::Impl& m, Presentation* source);

  // The new slide part's name (its relationships and content type made), or empty when a part it
  // needs can't be read. Slide links within a deck are kept; from another deck they're dropped.
  std::string CopySlide(std::string_view srcSlide);
  // From another deck: the layout the copy uses here (the caller maps or copies it).
  std::string layoutPart;

  // Reading the source deck.
  DomPtr SrcDom(std::string_view part) const;
  bool SrcBytes(std::string_view part, std::vector<uint8_t>& out) const;
  const pkg::RelationshipSet* SrcRels(std::string_view part) const;
  std::string SrcType(std::string_view part) const;
  // A part of the source copied here as it is (with its own parts by the same rules), or found
  // already copied in this operation; a picture or media part from another deck is shared with an
  // equal one here. Returns its name here.
  std::string CopyPart(std::string_view src, std::string_view newSlide, std::string_view srcSlide);

 private:
  enum class Policy { Share, Copy, Drop };
  static Policy PolicyOf(std::string_view type);
  void CopyRels(std::string_view src, std::string_view dst, std::string_view newSlide, std::string_view srcSlide);
  Editor::Impl& m_;
  Presentation* source_;
  std::map<std::string, std::string> copied_;  // source PartKey -> name here
};

// p:ph of a shape element; null when it isn't a placeholder.
pugi::xml_node PlaceholderOf(pugi::xml_node shape);
// An empty placeholder shape (p:sp) for layout placeholder `layoutPh`, with shape id `id`, as XML.
std::string EmptyPlaceholderXml(pugi::xml_node layoutPh, uint32_t id);
// A placeholder shape with no text (and no picture, table or chart).
bool PlaceholderIsEmpty(pugi::xml_node shape);
// A slide's placeholders mapped onto another layout's (Editor::SetLayout). `bounds`: the shapes'
// places now (slide points), by id.
void MapPlaceholders(pugi::xml_node slideRoot, pugi::xml_node layoutRoot, std::string_view slidePart,
                     Editor::Impl& m, const std::map<uint32_t, draw::Rect>& bounds);
// A text body's paragraphs replaced by plain text ('\n' paragraphs, '\v' line breaks), keeping the
// first paragraph's and first run's properties.
void SetPlainText(pugi::xml_node txBody, std::wstring_view text);
// The deck's notes master, made as PowerPoint makes it (on a copy of the slide master's theme) when
// it has none; empty when it can't be.
std::string EnsureNotesMaster(Editor::Impl& m);
// A notes page for a slide (on the deck's notes master, made when missing); its part name, empty
// when it can't be made.
std::string MakeNotesSlide(Editor::Impl& m, std::string_view slidePart);

}  // namespace fw::slide::edit
