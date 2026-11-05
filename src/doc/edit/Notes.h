#pragma once

#include <cstdint>
#include <optional>

#include "doc/edit/Editor.h"

// Footnotes and endnotes (plan §6.2 "advanced", milestone D4), as Word keeps them: a note is a
// w:footnote (w:endnote) in the notes part, a story of its own; the text refers to it with a
// w:footnoteReference (w:endnoteReference) of the same w:id, in the "footnote reference"
// character style. The layout numbers them in the order of their references, so adding or
// deleting one numbers the rest again; their text is edited in place like any other.
//
// Deleting a note's reference (Editor::Delete) deletes the note in the same undo step.
namespace fw::doc::edit {

// A new footnote (endnote) at `at`: its reference in the text and the note, Word's empty note
// (its number, then a space) in the "footnote text" style. The caret goes into the note, after
// the space. Only in the document's main text, outside fields, as Word allows them. One undo
// step, "Footnote" ("Endnote").
Result InsertNote(Editor& editor, Pos at, bool endnote);

// The note a reference at `at` (the character after it, else the one before) refers to; kNone
// where there's no reference.
uint32_t NoteAt(const Document& doc, Pos at);

// Where note story `story` is referred to in the text (the reference's position), when it is.
std::optional<Pos> NoteReference(const Document& doc, uint32_t story);

// Deletes note story `story` and its reference, as deleting the reference does. Refused for a note
// the text doesn't refer to (Word's separators).
Result DeleteNote(Editor& editor, uint32_t story);

}  // namespace fw::doc::edit
