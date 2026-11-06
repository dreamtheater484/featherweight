#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "pkg/Status.h"

namespace fw::doc::msdoc {

// A stream of the compound file by name ("WordDocument", "0Table", "1Table", "Data"), or nullopt
// when it has none. fw::pkg::CompoundFile gives them; tests give them from memory.
using StreamFn = std::function<std::optional<std::vector<uint8_t>>(std::string_view name)>;

// Converts a Word 97-2003 document ([MS-DOC]) into a .docx package (zip bytes) with the same
// content, for the docx reader: text, character, paragraph and section formatting, styles, fonts,
// lists, tables, fields, headers and footers, footnotes, endnotes, comments, bookmarks, tracked
// changes and inline pictures. What it can't show is left out. The document is then edited and
// saved as a .docx; the .doc itself is never written (plan §5).
//
// Fails with a plain message for Word 6 and Word 95 files, encrypted files (Status::Unsupported)
// and files that aren't Word documents or are damaged (Status::Damaged).
std::vector<uint8_t> ToDocx(const StreamFn& streams, pkg::Status& status);

// Whether the WordDocument stream says the file is encrypted (fEncrypted): fwpkg decrypts the
// streams first (RC4 CryptoAPI) and passes the plain ones to ToDocx.
bool IsEncrypted(const std::vector<uint8_t>& wordDocument);

}  // namespace fw::doc::msdoc
