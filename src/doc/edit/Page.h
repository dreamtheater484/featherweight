#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "doc/edit/Editor.h"
#include "doc/model/Document.h"

namespace fw::doc::edit {

// Page layout (plan §6.2 "Page layout", milestone D3): page setup, columns, section, page and column
// breaks, and hyphenation, as Word does them.
//
// A section's properties are its w:sectPr: in the w:pPr of the paragraph that ends it, or for the
// last section at the end of the body. Changes write only the properties they set and keep the rest
// of the w:sectPr as it is (header and footer references, page numbering, kept markup). Each is one
// undo step. Sections are indexes into Document::Sections().

// The page setup to apply: each field that is set is applied, the others are left alone.
struct PageSetup {
  std::optional<Twips> width, height;  // the paper as it lies (after `landscape`)
  // Turns the page: the width and height swap, and so do the margins (top with left, bottom with
  // right), as Word does. Nothing happens when the page already lies that way.
  std::optional<bool> landscape;
  std::optional<Twips> top, bottom, left, right;  // top and bottom may be negative (fixed)
  std::optional<Twips> header, footer, gutter;    // header and footer: from the paper's edge
  bool Any() const;
};

// Columns of text in a section: `count` (1 to 45) of equal width with `space` between them, or with
// `widths`, each column's own width and the space after it (the last one's space is ignored).
struct ColumnSetup {
  int32_t count = 1;
  Twips space{720};
  bool line = false;  // a line between columns
  std::vector<Columns::Col> widths;
};

// Automatic hyphenation (word/settings.xml), for the whole document.
struct Hyphenation {
  std::optional<bool> automatic;
  std::optional<bool> caps;          // hyphenate words in capitals
  std::optional<Twips> zone;         // how far from the right margin a word may end unhyphenated
  std::optional<int32_t> limit;      // consecutive hyphenated lines, 0 for no limit
};

// The section a paragraph of the main story is in (the last one for others), and a section's
// first and last paragraph.
uint32_t SectionOf(const Document& doc, uint32_t para);
std::pair<uint32_t, uint32_t> SectionParagraphs(const Document& doc, uint32_t section);

// Sections [first, last]. Refused (with nothing changed) when the result wouldn't fit: margins that
// leave less than a column of text, columns that don't fit between the margins.
Result SetPageSetup(Editor& ed, uint32_t first, uint32_t last, const PageSetup& setup);
Result SetColumns(Editor& ed, uint32_t first, uint32_t last, const ColumnSetup& columns);
// How a section starts: on the next page, on the same page (continuous), or on the next even or odd
// page. The first section's start only matters to Word when it's printed with others.
Result SetSectionStart(Editor& ed, uint32_t section, SectionStart start);

// Inserts a section break at `at`, a body paragraph outside tables and content controls: the
// paragraph splits there, and its first half ends a new section with the page setup of the one it
// was in. The new section starts as that one did; the rest of it starts as `start` says, and keeps
// its headers and footers.
Result InsertSectionBreak(Editor& ed, Pos at, SectionStart start);
// Deletes the break at the end of `section` (not the last): its paragraphs join the next section and
// take its page setup, as in Word, and the next section keeps the headers and footers it showed.
// The paragraph that held the break joins the next one, when there is a paragraph after it.
Result DeleteSectionBreak(Editor& ed, uint32_t section);

// A page break (Ctrl+Enter) or column break (Ctrl+Shift+Enter) at `at`; what follows it starts a new
// paragraph, as in Word.
Result InsertBreak(Editor& ed, Pos at, bool column);

Result SetHyphenation(Editor& ed, const Hyphenation& h);

}  // namespace fw::doc::edit
