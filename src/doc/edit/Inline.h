#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "doc/edit/Editor.h"

// Fields, links and bookmarks (plan §6.2 "basic", milestone D3). Each operation is one undo step
// made of the editor's transactions and TreeEdit changes, and keeps fields and bookmarks valid:
// a bookmark's start and end stay in one part with an id of their own there, and names stay
// unique in the document (fwtool check's rules).
namespace fw::doc::edit {

// ---- Fields
// Inserts a simple field (w:fldSimple) at `at`, showing `result` until the layout works it out
// (PAGE, NUMPAGES) or Word updates it. The result takes the formatting of the text before it.
// Refused inside another field, and in content that isn't edited. One undo step named `name`.
Result InsertField(Editor& editor, Pos at, std::string_view instr, std::u16string_view result,
                   std::u16string name = u"Field");

// ---- Links (w:hyperlink)
struct LinkInfo {
  Pos from, to;        // the link's text, in one paragraph
  std::string target;  // a web or mail address, or "#bookmark"
  ElemId elem = kNoElem;
};
// The link at `at` (the text after it, else the text before it); nullopt when there is none.
std::optional<LinkInfo> LinkAt(const Document& doc, Pos at);
// Turns the selection, or `text` inserted at `from` when the selection is empty, into a link to
// `target` styled as Word styles links (the "Hyperlink" character style). With a selection,
// `text` replaces it when it differs. One undo step, "Link".
Result InsertLink(Editor& editor, Pos from, Pos to, std::string_view target, std::u16string_view text);
// Changes the link at `at`: its target, and its text when `text` differs. One undo step.
Result EditLink(Editor& editor, Pos at, std::string_view target, std::u16string_view text);
// Makes the link at `at` plain text again, without the "Hyperlink" style. One undo step.
Result RemoveLink(Editor& editor, Pos at);

// ---- Bookmarks
struct BookmarkInfo {
  std::string name;
  Pos from, to;
  uint32_t story = kNone;
  bool hidden = false;  // Word's own (_Toc, _Hlk, _GoBack): shown only when asked for
};
// The document's bookmarks, in document order of their starts.
std::vector<BookmarkInfo> Bookmarks(const Document& doc);
std::optional<BookmarkInfo> FindBookmark(const Document& doc, std::string_view name);
// A name Word accepts for a new bookmark: a letter first, then letters, digits and underscores,
// at most 40 characters.
bool ValidBookmarkName(std::string_view utf8);
// Adds a bookmark around [from, to) (one story). Refused when the name isn't valid or is taken
// (names compare without case). One undo step, "Bookmark".
Result InsertBookmark(Editor& editor, Pos from, Pos to, std::string_view name);
// Removes a bookmark (its start and end); its text stays. One undo step.
Result DeleteBookmark(Editor& editor, std::string_view name);
// The name of a bookmark that starts where paragraph `para` starts, adding a hidden one ("_Hlk"
// and digits) around its text when there is none: a link to a heading goes to it. Empty when
// it can't be added. Inside the caller's group; adding is its own transaction.
std::string BookmarkForParagraph(Editor& editor, uint32_t para);

}  // namespace fw::doc::edit
