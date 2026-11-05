#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "doc/edit/Format.h"

namespace fw::doc::edit {

// Styles (plan §6.2 "Styles"), as Word applies and changes them.
//
// Word's built-in styles are found by their name, which Word writes in English whatever the
// language ("heading 1", "Title"); their ids may be translated ("Kop1"). A built-in style the
// document doesn't have yet is added with Word's own definition when first used, with the styles
// it's based on and linked to, so basedOn, next and link always name styles that exist.

struct StyleEntry {
  std::string id;    // empty for a built-in style the document doesn't have yet
  std::string name;  // as Word shows it: "Heading 1", "Normal"
  StyleType type = StyleType::Paragraph;
};

// The style gallery and the style box: the document's quick styles (w:qFormat, not hidden) by
// w:uiPriority, then Word's recommended built-ins it doesn't have yet.
std::vector<StyleEntry> GalleryStyles(const Document& doc);

// How Word shows a style's name: built-in names capitalized ("heading 1" -> "Heading 1").
std::string DisplayName(std::string_view name);

// The id of the style with this name or id (case-insensitive), adding Word's definition of a
// built-in style when the document lacks it. Inside a transaction, after EnsurePart(Styles).
// Empty when there is no such style.
std::string EnsureStyle(Editor& ed, std::string_view nameOrId);

// Applies a style to the range, as Word does:
//  - a paragraph style sets every paragraph's w:pStyle and removes its direct paragraph
//    formatting (the list, section break, mark formatting and tracked changes stay; applying the
//    default style ends the list too), and character formatting that covers more than half of
//    a paragraph's text;
//  - a linked style applied to part of one paragraph's text applies its character style there;
//  - a character style sets w:rStyle on the text (the default one removes it).
void ApplyStyle(Editor& ed, Range range, std::string_view nameOrId);

// Changes a style's own definition: every paragraph or run that uses it follows. Properties equal
// to what the style's basedOn gives are removed from it.
void ModifyStyle(Editor& ed, std::string_view styleId, const RunFormat& run, const ParaFormat& para);

// "Update Heading 1 to Match Selection": the direct formatting of the paragraph at the range's start
// (and of its first character) moves into the paragraph's style, and off the paragraph.
void UpdateStyleToMatch(Editor& ed, Range range);

}  // namespace fw::doc::edit
