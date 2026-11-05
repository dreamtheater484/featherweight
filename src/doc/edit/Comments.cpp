#include "doc/edit/Comments.h"

#include <algorithm>
#include <vector>

#include "doc/edit/TreeEdit.h"
#include "doc/model/Internal.h"

namespace fw::doc::edit {

namespace {

Result Fail(std::u16string why) {
  Result r;
  r.ok = false;
  r.message = std::move(why);
  return r;
}

const CommentInfo* Find(const std::vector<CommentInfo>& all, std::string_view id) {
  for (const CommentInfo& c : all)
    if (c.id == id) return &c;
  return nullptr;
}

// The first comment of the thread `c` is in (replies answer it).
const CommentInfo* Top(const std::vector<CommentInfo>& all, const CommentInfo& c) {
  const CommentInfo* top = &c;
  for (int hops = 0; hops < 64 && !top->parentId.empty(); ++hops) {
    const CommentInfo* up = Find(all, top->parentId);
    if (!up) break;
    top = up;
  }
  return top;
}

// The thread of `top`: it, then every reply to it or to one of them.
std::vector<const CommentInfo*> Thread(const std::vector<CommentInfo>& all, const CommentInfo& top) {
  std::vector<const CommentInfo*> out{&top};
  for (size_t i = 0; i < out.size() && out.size() < all.size(); ++i)
    for (const CommentInfo& c : all)
      if (c.parentId == out[i]->id && std::find(out.begin(), out.end(), &c) == out.end()) out.push_back(&c);
  return out;
}

// The comment's last paragraph's w14:paraId, which commentsExtended names it by; one is given
// inside the open transaction when it has none.
std::string EnsureParaId(TreeEdit& te, Editor& ed, const CommentInfo& c) {
  if (!c.paraId.empty()) return std::string(c.paraId);
  const std::vector<uint32_t> paras = te.Doc().StoryParagraphs(c.story);
  if (paras.empty()) return {};
  const std::string id = ed.NewParaId();
  te.SetAttr(te.Doc().Para(paras.back()).elem, Name("w14:paraId"), id);
  return id;
}

bool Blank(std::u16string_view text) {
  return std::all_of(text.begin(), text.end(), [](char16_t c) { return c == u' ' || c == u'\t' || c == u'\r' || c == u'\n'; });
}

// A new comment (a reply when `parent`), its marks at from, to and refAt.
Result Make(Editor& ed, Pos from, Pos to, Pos refAt, std::u16string_view text, const CommentInfo* parent,
            std::u16string name) {
  const Document& doc = ed.Doc();
  std::u16string why;
  if (!ed.CanEdit(&why)) return Fail(why);
  if (Blank(text)) return Fail(u"Type the comment first.");
  if (from.para >= doc.Paragraphs().size() || to.para >= doc.Paragraphs().size() ||
      doc.Para(from.para).story != doc.Para(to.para).story)
    return Fail(u"A comment is about text in one place.");
  const StoryKind kind = doc.Stories()[doc.Para(from.para).story].kind;
  // As in Word, which keeps comments to the main text (one in a note is refused by its validator).
  if (kind != StoryKind::Main)
    return Fail(u"Comments go in the document's text, not in notes, headers, footers or other comments.");
  const std::string id = ed.NewChangeId(), author = ed.ChangeAuthor();
  ed.BeginGroup(name);
  auto fail = [&](Result r) {
    ed.CancelGroup();
    r.ok = false;
    return r;
  };
  // Its marks first: where they're refused, nothing else is made (not the part, nor the comment).
  if (Result r = ed.InsertCommentMarks(from, to, refAt, id); !r) return fail(r);
  TreeEdit te(ed, from);
  if (te.EnsurePart(PartKind::Comments) == kNoElem) return fail(Fail(u"Comments can't be added to this document."));
  const uint32_t story = te.NewComment(id, author, Initials(author), ed.ChangeDate(), ed.NewParaId());
  if (story == kNone) return fail(Fail(u"Comments can't be added to this document."));
  // Its text, which isn't tracked.
  const uint32_t first = doc.StoryParagraphs(story).front();
  ed.PauseTracking(true);
  Result typed = ed.InsertText(Pos{first, ed.ParaLength(first)}, text);
  ed.PauseTracking(false);
  if (!typed) return fail(typed);
  // Into the comments part, named in commentsExtended by its last paragraph. (Last in the group:
  // the model's indexes are made again then, with its marks.)
  const uint32_t last = doc.StoryParagraphs(story).back();
  te.Begin(name);
  const std::string parentParaId = parent ? EnsureParaId(te, ed, *parent) : std::string();
  if (parent && parent->paraId.empty()) te.SetCommentState(parentParaId, {}, parent->done);
  te.LinkComment(story);
  te.SetCommentState(doc.W14ParaId(last), parentParaId, parent && parent->done);
  te.Commit();
  if (!te.Refused().empty()) return fail(Fail(te.Refused()));
  ed.EndGroup();
  Result r;
  r.caret = from;
  return r;
}

}  // namespace

std::string Initials(std::string_view author) {
  std::string out;
  bool start = true;
  for (size_t i = 0; i < author.size();) {
    const auto c = static_cast<unsigned char>(author[i]);
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
    if (c == '@') break;  // an address: the name before it
    if (c == ' ' || c == '.' || c == '-' || c == '_') {
      start = true;
    } else if (start) {
      out.append(author.substr(i, std::min(n, author.size() - i)));
      start = false;
    }
    i += n;
  }
  for (char& ch : out)
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
  return out;
}

std::u16string CommentText(const Document& doc, const CommentInfo& comment) {
  std::u16string out;
  bool firstPara = true;
  for (uint32_t p : doc.StoryParagraphs(comment.story)) {
    if (!firstPara) out += u'\n';
    firstPara = false;
    const std::u16string& s = doc.Para(p).text;
    for (const TextRun& run : doc.Runs(p)) {
      if (run.fieldCode || (run.revision && (run.revision.Is(Name("del")) || run.revision.Is(Name("moveFrom"))))) continue;
      for (uint32_t i = run.start; i < run.end && i < s.size(); ++i) {
        const char16_t c = s[i];
        if (c == 0x0B) out += u'\n';
        else if (c == u'\t' || (c >= 0x20 && c != 0xFFFC)) out += c;
      }
    }
  }
  return out;
}

Result AddComment(Editor& editor, Pos from, Pos to, std::u16string_view text) {
  if (editor.Compare(to, from) < 0) std::swap(from, to);
  return Make(editor, from, to, to, text, nullptr, u"Comment");
}

Result ReplyToComment(Editor& editor, std::string_view id, std::u16string_view text) {
  const std::vector<CommentInfo> all = editor.Doc().Comments();
  const CommentInfo* c = Find(all, id);
  if (!c) return Fail(u"That comment isn't there any more.");
  const CommentInfo* top = Top(all, *c);
  // About the same text, its reference after the one it answers.
  Pos refAt = top->reference;
  if (refAt.para != kNone) ++refAt.offset;
  Pos from = top->start, to = top->end;
  if (from.para == kNone || to.para == kNone) from = to = top->reference;
  if (refAt.para == kNone) refAt = to;
  if (from.para == kNone) return Fail(u"This comment isn't anchored in the text, so it can't be answered.");
  return Make(editor, from, to, refAt, text, top, u"Reply");
}

Result EditComment(Editor& editor, std::string_view id, std::u16string_view text) {
  const Document& doc = editor.Doc();
  const std::vector<CommentInfo> all = doc.Comments();
  const CommentInfo* c = Find(all, id);
  if (!c) return Fail(u"That comment isn't there any more.");
  if (Blank(text)) return Fail(u"A comment can't be empty. Delete it instead.");
  const std::vector<uint32_t> paras = doc.StoryParagraphs(c->story);
  if (paras.empty()) return Fail(u"This comment can't be changed.");
  // Its text: after the annotation mark, to the end of its last paragraph.
  const std::u16string& head = doc.Para(paras.front()).text;
  uint32_t o = 0;
  while (o < head.size() && head[o] == 0x05) ++o;
  const Pos from{paras.front(), o}, to{paras.back(), editor.ParaLength(paras.back())};
  editor.BeginGroup(u"Edit comment");
  editor.PauseTracking(true);
  Result r = editor.Replace(from, to, text);
  editor.PauseTracking(false);
  if (!r) {
    editor.CancelGroup();
    return r;
  }
  editor.EndGroup();
  return r;
}

Result DeleteComment(Editor& editor, std::string_view id) {
  const Document& doc = editor.Doc();
  const std::vector<CommentInfo> all = doc.Comments();
  const CommentInfo* c = Find(all, id);
  if (!c) return Fail(u"That comment isn't there any more.");
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  // Copies: the list is read again as the marks go.
  struct Gone {
    std::string id, paraId;
    uint32_t story;
  };
  std::vector<Gone> gone;
  for (const CommentInfo* k : Thread(all, *c)) gone.push_back({std::string(k->id), std::string(k->paraId), k->story});
  editor.BeginGroup(u"Delete comment");
  for (auto it = gone.rbegin(); it != gone.rend(); ++it)
    if (Result r = editor.RemoveCommentMarks(it->id); !r) {
      editor.CancelGroup();
      return r;
    }
  TreeEdit te(editor, c->start.para != kNone ? c->start : c->reference);
  te.Begin(u"Delete comment");
  for (const Gone& g : gone) {
    te.UnlinkComment(g.story);
    if (!g.paraId.empty()) te.RemoveCommentState(g.paraId);
  }
  te.Commit();
  if (!te.Refused().empty()) {
    editor.CancelGroup();
    return Fail(te.Refused());
  }
  editor.EndGroup();
  Result r;
  r.caret = c->start.para != kNone ? c->start : c->reference;
  return r;
}

Result ResolveComment(Editor& editor, std::string_view id, bool done) {
  const std::vector<CommentInfo> all = editor.Doc().Comments();
  const CommentInfo* c = Find(all, id);
  if (!c) return Fail(u"That comment isn't there any more.");
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  const CommentInfo* top = Top(all, *c);
  TreeEdit te(editor, top->start.para != kNone ? top->start : top->reference);
  te.Begin(done ? u"Resolve comment" : u"Reopen comment");
  const std::string topId = EnsureParaId(te, editor, *top);
  for (const CommentInfo* k : Thread(all, *top)) {
    const std::string paraId = k == top ? topId : EnsureParaId(te, editor, *k);
    std::string parent;
    bool was = false;
    if (!te.CommentState(paraId, parent, was) && k != top) parent = topId;
    te.SetCommentState(paraId, parent, done);
  }
  te.Commit();
  if (!te.Refused().empty()) return Fail(te.Refused());
  Result r;
  r.caret = top->start;
  return r;
}

}  // namespace fw::doc::edit
