#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "draw/Painter.h"
#include "slide/edit/Edit.h"

// Comments on slides (P3, p3-review; plan §8.2): PowerPoint's two formats, read and kept.
//  - Modern (PowerPoint 365 and this PC's PowerPoint, which writes them): ppt/comments/modernComment_
//    <slide id>_<creation id>.xml, named by the slide's p188:commentRel, authors in ppt/authors.xml;
//    threads (a comment and its replies) and resolved state.
//  - Legacy (PowerPoint 2007-2019): ppt/comments/commentN.xml, authors in ppt/commentAuthors.xml;
//    replies by p15:threadingInfo (PowerPoint 2013 on); no resolved state.
// New threads are written modern (the format PowerPoint on the reference PC writes and shows, threads
// included: p3-plan decision). Replies, edits and deletes stay in the format the comment is in.
// Each operation is one undo step through the Editor; Changes report the slide with kComments.
namespace fw::slide::edit {

// Who writes (File > Options > User name, else Windows' name).
struct CommentAuthor {
  std::wstring name = L"Author";
  std::wstring initials;  // empty: made from the name ("Ann Lee" -> "AL")
};

struct Comment {
  std::string id;          // modern: its GUID ("{...}"); legacy: "legacy:<authorId>:<idx>"
  std::string parent;      // a reply: its thread's first comment's id; empty for the first
  std::wstring author, initials;
  std::string created;     // as stored (ISO 8601; modern in UTC, legacy local time), may be empty
  bool utc = false;        // `created` is UTC (modern)
  std::wstring text;       // its paragraphs joined with '\n'
  draw::Point pos;         // the thread's marker on the slide, points
  bool hasPos = false;     // false: none stored (anchored to `shape`, or the slide's corner)
  uint32_t shape = 0;      // anchored to this shape (modern ac:spMk), 0: the slide
  bool resolved = false;   // the thread is resolved (modern only)
  bool legacy = false;     // PowerPoint's older format: can't be resolved
};

// The slide's comments in the file's order: each thread's first comment, then its replies. Legacy
// comments come before modern ones when a slide has both.
std::vector<Comment> SlideComments(Editor& ed, size_t slide);
// How many comments (replies included) the slides have, one count per slide.
std::vector<size_t> CommentCounts(Editor& ed);

// "Ann Lee" -> "AL", as PowerPoint makes initials.
std::wstring CommentInitials(std::wstring_view name);

// A new thread on slide `slide` with its marker at `at` (slide points), saying `text` ('\n' starts a
// paragraph). `id` gets its id.
Result AddComment(Editor& ed, size_t slide, draw::Point at, std::wstring_view text, const CommentAuthor& who,
                  std::string* id = nullptr);
// A reply to comment `id` (or to its thread, when `id` is a reply).
Result ReplyToComment(Editor& ed, size_t slide, std::string_view id, std::wstring_view text, const CommentAuthor& who,
                      std::string* replyId = nullptr);
// The comment's text replaced.
Result EditComment(Editor& ed, size_t slide, std::string_view id, std::wstring_view text);
// The comment goes; a thread's first comment takes its replies with it. The last comment of a slide
// takes its comments part and relationship with it, as PowerPoint leaves the file.
Result DeleteComment(Editor& ed, size_t slide, std::string_view id);
// Every comment of `slides` (empty: every slide), one step (Review > Delete > Delete All Comments).
Result DeleteAllComments(Editor& ed, const std::vector<size_t>& slides);
// The thread resolved, or open again (modern comments only).
Result ResolveComment(Editor& ed, size_t slide, std::string_view id, bool resolved);
// The thread's marker moved to `at` (slide points).
Result MoveComment(Editor& ed, size_t slide, std::string_view id, draw::Point at);

// ---- Internal (src/slide/edit): a copied slide (Duplicate, Reuse Slides) loses its comments; its
// p188:commentRel, whose relationship was dropped, goes with them.
void DropCommentRel(pugi::xml_node slideRoot);

}  // namespace fw::slide::edit
