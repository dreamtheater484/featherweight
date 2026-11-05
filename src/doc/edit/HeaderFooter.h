#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "doc/edit/Editor.h"

// Headers, footers and page numbers (plan §6.2 "basic", milestone D3), as Word keeps them: a
// section's w:sectPr refers to header and footer parts by type (default, first page, even pages);
// a section without a reference of a type shows the one before it ("Link to Previous"). w:titlePg
// turns the first page's on, settings' w:evenAndOddHeaders the even pages'. Page size, margins,
// columns and section breaks are the page setup's (d1-model).
//
// Each change is one undo step. New parts are made outside undo: a part nothing refers to says
// nothing, and the step that refers to it is undone.
namespace fw::doc::edit {

// The type of header and footer a page shows: the first page's on a section's first page when the
// section has one, the even pages' on even page numbers when the document has them, else default.
HdrFtrType PageType(const Document& doc, uint32_t section, bool firstOfSection, int32_t pageNumber);

// Whether section `section` shows the previous section's header (or footer) of this type, having
// none of its own. Never for the first section.
bool LinkedToPrevious(const Document& doc, uint32_t section, bool footer, HdrFtrType type);

// The story that holds section `section`'s header (or footer) of this type, its own or the one it
// shows from an earlier section; when there is none, a new empty one in the Header (Footer) style,
// referred to by the section (one undo step). kNone, with the reason, when it can't be made.
uint32_t EnsureHeaderFooter(Editor& editor, uint32_t section, bool footer, HdrFtrType type, std::u16string& why);

// Link to Previous on: the section's own header (footer) of this type goes, and it shows the
// previous section's. Off: it gets its own, a copy of what it showed.
Result SetLinkToPrevious(Editor& editor, uint32_t section, bool footer, HdrFtrType type, bool link);
// A different first page header and footer for the section (w:titlePg).
Result SetDifferentFirstPage(Editor& editor, uint32_t section, bool on);
bool DifferentFirstPage(const Document& doc, uint32_t section);
// Different odd and even page headers and footers in the whole document (w:evenAndOddHeaders).
Result SetDifferentOddEven(Editor& editor, bool on);

// The section's page numbers: their format ("decimal", "lowerRoman", "upperRoman", "lowerLetter",
// "upperLetter") and where they start (nullopt: they continue from the previous section).
Result SetPageNumbering(Editor& editor, uint32_t section, std::string_view format, std::optional<int32_t> start);

// A page number (a PAGE field; with `ofPages`, "Page X of Y" with NUMPAGES) at the top or bottom of
// the section's pages of this type, aligned `align`: in the header (footer) paragraph when it's
// empty, else in a new paragraph after its last. One undo step, "Page Number".
Result InsertPageNumber(Editor& editor, uint32_t section, bool bottom, HdrFtrType type, Justification align,
                        bool ofPages);

// The page-number fields' instructions, for inserting one at the caret.
inline constexpr std::string_view kPageField = " PAGE ", kPagesField = " NUMPAGES ";

}  // namespace fw::doc::edit
