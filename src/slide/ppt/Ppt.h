#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pkg/Status.h"

// PowerPoint 97-2003 presentations (.ppt, .pps, .pot: [MS-PPT]), imported (P3, p3-ppt).
namespace fw::slide::ppt {

// A stream of the compound file by name ("PowerPoint Document", "Current User", "Pictures"), or
// nullopt when it has none. fw::pkg::CompoundFile gives them; tests and the fuzzer give them from
// memory.
using StreamFn = std::function<std::optional<std::vector<uint8_t>>(std::string_view name)>;

// Converts a PowerPoint 97-2003 presentation into a .pptx package (zip bytes) with the same content,
// for the slide reader (fw::slide::Read), as sheet/xls does for .xls: the slide size, masters (with
// title masters as their layouts), slides, notes, the color schemes, fonts, text with its paragraph
// and character formatting resolved from the master styles, OfficeArt shapes (preset and freeform
// geometry, fills, lines, pictures from the Pictures stream, groups), placeholders, backgrounds and
// hidden slides. A PowerPoint 2007 or later file's round-trip theme is used as the deck's theme.
// What it doesn't understand is left out; damaged input never crashes or hangs. The deck is then
// shown and saved as a .pptx; the .ppt itself is never written.
//
// An encrypted presentation (RC4 CryptoAPI) is decrypted with `password`; the .pptx made isn't
// encrypted. Fails with a plain message for PowerPoint 95 and older files and for encryption it
// can't read (Status::Unsupported), for a missing or wrong password (Status::Rejected:
// NeedsPassword says which files ask for one), and for files that aren't presentations or are
// damaged (Damaged).
std::vector<uint8_t> ToPptx(const StreamFn& streams, pkg::Status& status, std::wstring_view password = {});

// Whether the presentation is encrypted, so the user must give its password.
bool NeedsPassword(const StreamFn& streams);

// Whether a compound file's streams look like a PowerPoint presentation: it has a "PowerPoint
// Document" stream (PowerPoint 95's, without the "Current User" stream of 97 and later, is refused by
// ToPptx with a message).
bool IsPresentation(const StreamFn& streams);

}  // namespace fw::slide::ppt
