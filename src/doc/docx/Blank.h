#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fw::doc::docx {

struct BlankOptions {
  std::string language = "en-US";  // the text's language (BCP 47), for spelling later
  bool a4 = false;                 // A4 with 2.5 cm margins; otherwise Letter with 1-inch margins
  std::string author;              // UTF-8: dc:creator and cp:lastModifiedBy
  int64_t now = 0;                 // FILETIME for the created and modified dates; 0 for the clock
};

// A new, empty Word document (plan §6.3, D2 "new document"): one empty paragraph and Word's
// default styles (Normal in Calibri 11 pt with 8 pt after and 1.08 lines, Heading 1 to 3, Title,
// Subtitle, List Paragraph, Hyperlink, and the defaults for tables, lists and characters), with
// explicit fonts so it looks the same on every PC. Built in code: no template file ships.
std::vector<uint8_t> BlankDocx(const BlankOptions& options = {});

}  // namespace fw::doc::docx
