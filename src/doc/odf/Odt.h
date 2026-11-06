#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "pkg/Limits.h"
#include "pkg/Status.h"

namespace fw::doc::odf {

// An OpenDocument text document (.odt, or an .ott template) as a Word document (plan §6.3, D5):
// its styles, paragraphs and spans, lists, tables, pictures and text frames, headers and footers,
// notes, bookmarks, links and page layout, written as .docx bytes that the .docx reader opens.
// What Word has no place for is left out, but never the text. The source is never written to:
// a document opened from one is saved with Save As, as a .docx.
pkg::Status OdtToDocx(std::span<const uint8_t> odt, std::vector<uint8_t>& docx,
                      const pkg::Limits& limits = pkg::Limits::Default());

}  // namespace fw::doc::odf
