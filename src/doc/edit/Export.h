#pragma once

#include <string>

#include "doc/model/Document.h"

// Save as RTF and as plain text (milestone D4): copies of a document for programs that don't read
// .docx, as Word's Save As types make them.
//
// RTF is the clipboard's (Clip.h) for each section, with the section's page size, margins,
// columns, page numbering and break, its headers and footers (with PAGE and NUMPAGES as fields),
// and page and column breaks. List labels are text ("1.", a tab), as Word reads them only with
// RTF's list tables. Pictures, notes, comments and text boxes are left out.
//
// Plain text is the body as copying gives it: list labels and a tab before the text, a tab between
// a row's cells, CR LF after each paragraph, and a form feed (U+000C) where a new page starts, as
// Word writes it. Headers and footers are left out, as Word leaves them out.
namespace fw::doc::edit {

std::string RtfFile(const Document& doc);

// Word's "Unicode (UTF-8)" and "Unicode" (UTF-16, little-endian), each with its byte order mark.
enum class TextEncoding { Utf8, Utf16 };
std::string TextFile(const Document& doc, TextEncoding encoding);

}  // namespace fw::doc::edit
