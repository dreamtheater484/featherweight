#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "doc/edit/Editor.h"

// A table of contents (plan §6.2 "advanced", milestone D4), as Word writes one: a TOC field whose
// result is a paragraph for each heading, in the "toc 1" to "toc 3" styles with a dotted right tab
// at the text's edge; each entry a link to a hidden "_Toc" bookmark around its heading, holding the
// heading's number and text and a PAGEREF field to the bookmark showing its page. The TOC field's
// begin and end are in the paragraphs themselves, outside the links (Word refuses a field split
// across a link, torture finding 6); each PAGEREF is inside its link. The field ends at the start
// of a paragraph of its own after the entries.
namespace fw::doc::edit {

// The text of the page a paragraph is on, as the layout numbers it ("4", "iv"); empty when that
// isn't known.
using PageText = std::function<std::u16string(uint32_t para)>;

// A heading a table of contents lists.
struct TocHeading {
  uint32_t para = kNone;
  int level = 1;          // 1 for an outline level 0 paragraph ("Heading 1")
  std::u16string text;    // its number (and a tab) then its text, as shown
};
// The paragraphs of the document's text with outline levels 1 to `levels` (heading styles and
// paragraphs given an outline level), not empty, in order: what TOC \o "1-3" \u lists.
std::vector<TocHeading> TocHeadings(const Document& doc, int levels = 3);

struct TocInfo {
  Pos begin, separator, end;  // the TOC field's characters
  std::u16string code;
  int levels = 3;             // from its \o switch
};
// The document's tables of contents (TOC fields in its text, outside other fields), in order.
std::vector<TocInfo> TablesOfContents(const Document& doc);
// The one around `at` (inside its field, from its begin to its end); nullopt where there's none.
std::optional<TocInfo> TocAt(const Document& doc, Pos at);

// Inserts a table of contents of headings 1 to 3 at `at`, in the document's text and outside
// fields: before the paragraph at its start, else after the part before it. Each entry's page is
// `pages` of its heading once the table is in (laid out again by then). One undo step, "Table of
// Contents"; the caret goes after the table. With no headings, the table says so, as Word's does.
Result InsertToc(Editor& editor, Pos at, const PageText& pages);
// Brings the page numbers of the table of contents `toc` up to date: each PAGEREF's result becomes
// `pages` of its bookmark's paragraph, where it differs. One undo step, "Update Table" (none when
// nothing changed).
Result UpdateTocPages(Editor& editor, const TocInfo& toc, const PageText& pages);
// Makes the table again from the headings there are now, with the same field code, in its place.
// One undo step, "Update Table".
Result UpdateToc(Editor& editor, const TocInfo& toc, const PageText& pages);

}  // namespace fw::doc::edit
