#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "pkg/Limits.h"
#include "pkg/Status.h"

namespace fw::doc::docx {

// The built-in templates (D5 "New from template"): placeholder text in brackets, Word's own styles.
enum class Template { None, Letter, Cv, Report };

struct BlankOptions {
  std::string language = "en-US";  // the text's language (BCP 47), for spelling later
  bool a4 = false;                 // A4 with 2.5 cm margins; otherwise Letter with 1-inch margins
  std::string author;              // UTF-8: dc:creator and cp:lastModifiedBy
  int64_t now = 0;                 // FILETIME for the created and modified dates; 0 for the clock
  Template kind = Template::None;  // the text a new document starts with
};

// A new, empty Word document (plan §6.3, D2 "new document"): one empty paragraph and Word's
// default styles (Normal in Calibri 11 pt with 8 pt after and 1.08 lines, Heading 1 to 3, Title,
// Subtitle, List Paragraph, Hyperlink, and the defaults for tables, lists and characters), with
// explicit fonts so it looks the same on every PC. Built in code: no template file ships. With a
// template, the document starts with that template's text (and a bullet list, and for the report
// a page number in the footer).
std::vector<uint8_t> BlankDocx(const BlankOptions& options = {});

// A Word template (.dotx, .dotm) as a new document made from it, as Word makes one: the same
// package with the main part's type changed to a document's (a .dotm's to a macro-enabled one).
pkg::Status DocumentFromTemplate(std::span<const uint8_t> dotx, std::vector<uint8_t>& docx,
                                 const pkg::Limits& limits = pkg::Limits::Default());

}  // namespace fw::doc::docx
