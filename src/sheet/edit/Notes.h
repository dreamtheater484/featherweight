#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sheet/model/Types.h"

// Notes as Excel 2021 writes them (S3, s3-review; measured: a note made in Excel and saved): the
// sheet's comments part (authors, then a comment per cell in row order, the author's name in bold
// before the text) and its VML drawing (a text box shape per note, anchored beside its cell, hidden
// unless shown). The edit layer's Editor::SetNote puts these into the package.
namespace fw::sheet::edit::notes {

struct NoteText {
  std::string author, text;
  bool shown = false;
  // The box (width 0: the shape's own, or Excel's beside the cell for a new note): its size in
  // points and its VML anchor (top left, bottom right: column, x pixels, row, y pixels).
  double width = 0, height = 0;
  std::array<uint32_t, 8> anchor{};
};

// `comments` and `vml` changed for the note on `cell` (nullopt: deleted): either may be empty, for
// a sheet without that part, and comes back empty when nothing is left in it (the part goes).
// False when an existing part can't be read.
bool Edit(std::string_view comments, std::string_view vml, CellAddr cell, const std::optional<NoteText>& note,
          std::string& commentsOut, std::string& vmlOut);

// Whether the comments part has an entry on `cell` (a note, or a thread's note for older versions).
bool HasEntry(std::string_view comments, CellAddr cell);

// Threaded comments as Excel 365 writes them (the format of files it saved): the sheet's
// threadedComments part (each comment with its time, its person and its id; replies name the
// first comment's id as parentId; done="1" on the first when resolved), the workbook's persons
// part (displayName, a person id, userId the name, providerId "None" as for a local account), and
// for older versions a note in the comments part (author "tc={id}", xr:uid the id, the thread as
// "[Threaded comment] ... Comment: ... Reply: ...") with its hidden VML box.
struct ThreadEntry {
  std::string author, text, id, date;  // date as the file writes it: "2026-01-01T12:00:00.00"
};
struct ThreadText {
  std::vector<ThreadEntry> comments;  // the first, then the replies
  bool resolved = false;
};

// The thread on `cell`, if the part has one; authors' names from the persons part.
std::optional<ThreadText> ReadThread(std::string_view threads, std::string_view persons, CellAddr cell);

// The four parts changed for the thread on `cell` (nullopt: deleted). Outputs come back empty for
// a part with nothing left in it (persons is only ever added to). False when one can't be read.
struct ThreadParts {
  std::string threads, persons, comments, vml;
};
bool EditThread(const ThreadParts& in, CellAddr cell, const std::optional<ThreadText>& thread, ThreadParts& out);

}  // namespace fw::sheet::edit::notes
