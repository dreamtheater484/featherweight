#pragma once

#include <string>
#include <string_view>

#include "doc/edit/Editor.h"

// Comments (plan §6.2 "Review", milestone D4), as Word makes them: a w:comment in comments.xml
// (its story), w:commentRangeStart and w:commentRangeEnd around the text it's about, and a run with
// w:commentReference after it; replies and resolved state in commentsExtended, which a save writes
// with commentsIds and commentsExtensible (docx::WriteCommentParts). Each operation is one undo
// step, by Editor::ChangeAuthor() at Editor::ChangeDate(); a comment's own text is never tracked.
namespace fw::doc::edit {

// "Ann Lee" -> "AL", as Word's initials.
std::string Initials(std::string_view author);

// A comment's text as the comments pane shows it: its paragraphs joined with line feeds, without
// the annotation mark.
std::u16string CommentText(const Document& doc, const CommentInfo& comment);

// A comment on [from, to) (one story; empty: at a point), saying `text` (line feeds start new
// paragraphs). The caret stays where it was.
Result AddComment(Editor& editor, Pos from, Pos to, std::u16string_view text);
// A reply to comment `id`, about the same text.
Result ReplyToComment(Editor& editor, std::string_view id, std::u16string_view text);
// The comment's text, replaced.
Result EditComment(Editor& editor, std::string_view id, std::u16string_view text);
// The comment, its marks and its replies go.
Result DeleteComment(Editor& editor, std::string_view id);
// Resolved (done) or open again, with its replies.
Result ResolveComment(Editor& editor, std::string_view id, bool done);

}  // namespace fw::doc::edit
