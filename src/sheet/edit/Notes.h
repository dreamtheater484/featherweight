#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "sheet/model/Types.h"

// Notes as Excel 2021 writes them (S3, s3-review; measured: a note made in Excel and saved): the
// sheet's comments part (authors, then a comment per cell in row order, the author's name in bold
// before the text) and its VML drawing (a text box shape per note, anchored beside its cell, hidden
// unless shown). The edit layer's Editor::SetNote puts these into the package.
namespace fw::sheet::edit::notes {

struct NoteText {
  std::string author, text;
  bool shown = false;
};

// `comments` and `vml` changed for the note on `cell` (nullopt: deleted): either may be empty, for
// a sheet without that part, and comes back empty when nothing is left in it (the part goes).
// False when an existing part can't be read.
bool Edit(std::string_view comments, std::string_view vml, CellAddr cell, const std::optional<NoteText>& note,
          std::string& commentsOut, std::string& vmlOut);

}  // namespace fw::sheet::edit::notes
