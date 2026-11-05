#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "doc/model/Document.h"
#include "pkg/Status.h"

namespace fw::doc::docx {

// What a save writes (plan §6.3 "Saving").
struct SaveOptions {
  // The model's parts that were edited (indexes into Document::Parts()): written from the model.
  // Every other part is copied from the snapshot byte for byte.
  std::vector<uint32_t> changedParts;
  // Something was edited: the document properties get the time and who saved it.
  bool edited = false;
  std::string author;  // UTF-8, for cp:lastModifiedBy
  int64_t now = 0;     // FILETIME for dcterms:modified; 0 for the clock
};

// The bytes of the saved file. With nothing edited they are the file as it was opened, exactly.
// Otherwise the changed parts are written from the model and the package is saved around them;
// the result is opened again and every rewritten part checked before it's returned.
pkg::Status Serialize(Document& doc, const SaveOptions& options, std::vector<uint8_t>& out);

// Pictures nothing in a part refers to any more (deleted, or replaced), which a save leaves out:
// the image relationships of the parts written from the model whose ids their XML doesn't
// mention, and the media parts that only those pointed to. `written` holds the parts' XML by index
// into Document::Parts(); other parts aren't looked at. The package isn't changed: undo may still
// bring a picture back.
struct UnusedMedia {
  std::vector<std::pair<std::string, std::string>> relationships;  // source part, relationship id
  std::vector<std::string> parts;
};
UnusedMedia FindUnusedMedia(const Document& doc, const std::vector<std::pair<uint32_t, const std::string*>>& written);

// docProps/core.xml with dcterms:modified set to `now` and cp:lastModifiedBy to `author`;
// everything else as it was. Elements that are missing are added.
pkg::Status UpdateCoreProperties(std::string_view xml, const std::string& author, int64_t now, std::string& out);

// "2026-11-05T14:30:12Z" (W3CDTF, UTC) for a FILETIME; 0 for the clock.
std::string W3cTime(int64_t filetime = 0);

}  // namespace fw::doc::docx
