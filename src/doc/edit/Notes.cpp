#include "doc/edit/Notes.h"

#include <algorithm>
#include <charconv>
#include <string>

#include "doc/edit/Inline.h"
#include "doc/edit/Styles.h"
#include "doc/edit/TreeEdit.h"
#include "doc/model/Internal.h"
#include "doc/model/Schema.h"

namespace fw::doc::edit {

namespace {

constexpr uint32_t kIdAttr = Name("id");

Result Fail(std::u16string why) {
  Result r;
  r.ok = false;
  r.message = std::move(why);
  return r;
}

bool IsNote(StoryKind kind) { return kind == StoryKind::Footnote || kind == StoryKind::Endnote; }

// An id for a new note: above every note's of its kind, those taken out included (their ids
// come back with them), and at least 1 (Word's separators are -1 and 0).
std::string NewId(const Document& doc, StoryKind kind) {
  int64_t top = 0;
  for (const Story& s : doc.Stories()) {
    if (s.kind != kind) continue;
    int64_t v = 0;
    auto [ptr, ec] = std::from_chars(s.id.data(), s.id.data() + s.id.size(), v);
    if (ec == std::errc() && ptr == s.id.data() + s.id.size()) top = std::max(top, v);
  }
  return std::to_string(top + 1);
}

}  // namespace

Result InsertNote(Editor& editor, Pos at, bool endnote) {
  const Document& doc = editor.Doc();
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  const std::u16string what = endnote ? u"An endnote" : u"A footnote";
  if (at.para >= doc.Paragraphs().size() || at.offset > doc.Para(at.para).text.size())
    return Fail(u"That place isn't in the document.");
  if (doc.Para(at.para).story != doc.MainStory())
    return Fail(what + u" goes in the document's text, not in a header, a footer, a note, a comment or a text box.");
  if (InsideField(doc, at)) return Fail(what + u" can't go inside a field.");
  const StoryKind kind = endnote ? StoryKind::Endnote : StoryKind::Footnote;
  const std::u16string name = endnote ? u"Endnote" : u"Footnote";
  const std::string tag = endnote ? "endnote" : "footnote";

  TreeEdit parts(editor, at);
  if (!parts.CanChange({at.para})) return Fail(parts.Refused());

  // The reference first. The parts a note needs are made outside undo (they say nothing empty),
  // so only once it's in: a note that can't go there leaves nothing behind (torture finding 30).
  EditGroup group(editor, name);
  const std::string id = NewId(doc, kind);
  Result r = editor.InsertRuns(at, "<w:r><w:" + tag + "Reference w:id=\"" + id + "\"/></w:r>", name);
  if (!r) return r;
  if (parts.EnsurePart(PartKind::Styles) == kNoElem) return Fail(u"This document's styles can't change.");
  const ElemId root = parts.EnsurePart(endnote ? PartKind::Endnotes : PartKind::Footnotes);
  if (root == kNoElem) return Fail(what + u" can't be added to this document.");

  // Then the styles, the reference's, and the note, its paragraph after every other: the caret goes
  // after its number and space. The notes after it number again from the reference's page.
  const Pos inNote{static_cast<uint32_t>(doc.Paragraphs().size()), 2};
  TreeEdit te(editor, inNote);
  te.Begin(name);
  te.Affects(at.para, at.para, true);
  const std::string textStyle = EnsureStyle(te, tag + " text");
  const std::string markStyle = EnsureStyle(te, tag + " reference");
  if (!markStyle.empty())
    for (const TextRun& t : doc.Runs(at.para))
      if (t.run && t.start == at.offset && t.end == at.offset + 1) {
        te.SetAttr(te.Path(t.run.Id(), {names::kRPr, Name("rStyle")}), names::kVal, markStyle);
        break;
      }
  const uint32_t story = te.AddNote(root, endnote, id, textStyle, markStyle);
  if (story == kNone) {
    te.Abort();
    return Fail(what + u" can't be added to this document.");
  }
  te.Commit();
  if (!te.Refused().empty()) return Fail(te.Refused());
  const std::vector<uint32_t> paras = doc.StoryParagraphs(story);
  if (paras.empty()) return Fail(what + u" can't be added to this document.");
  group.done = true;
  Result out;
  out.caret = {paras.front(), std::min<uint32_t>(2, static_cast<uint32_t>(doc.Para(paras.front()).text.size()))};
  return out;
}

uint32_t NoteAt(const Document& doc, Pos at) {
  if (at.para >= doc.Paragraphs().size()) return kNone;
  const std::u16string& text = doc.Para(at.para).text;
  if (text.find(u'\x02') == std::u16string::npos) return kNone;
  const InlineItem* found = nullptr;
  std::vector<InlineItem> items = doc.Items(at.para);
  for (int pass = 0; pass < 2 && !found; ++pass)
    for (const InlineItem& it : items) {
      const bool here = pass == 0 ? it.offset == at.offset : it.offset + 1 == at.offset;
      if (here && (it.kind == ItemKind::FootnoteRef || it.kind == ItemKind::EndnoteRef || it.kind == ItemKind::NoteMark)) {
        found = &it;
        break;
      }
    }
  if (!found) return kNone;
  if (found->kind == ItemKind::NoteMark) {  // a note's own number: that note
    const uint32_t story = doc.Para(at.para).story;
    return IsNote(doc.Stories()[story].kind) ? story : kNone;
  }
  const StoryKind kind = found->kind == ItemKind::FootnoteRef ? StoryKind::Footnote : StoryKind::Endnote;
  return doc.NoteStory(kind, found->node.AttrOr(kIdAttr));
}

std::optional<Pos> NoteReference(const Document& doc, uint32_t story) {
  if (story >= doc.Stories().size()) return std::nullopt;
  const Story& s = doc.Stories()[story];
  if (!IsNote(s.kind) || s.root == kNoElem) return std::nullopt;
  const ItemKind want = s.kind == StoryKind::Footnote ? ItemKind::FootnoteRef : ItemKind::EndnoteRef;
  for (uint32_t p = 0; p < doc.Paragraphs().size(); ++p) {
    const Paragraph& para = doc.Para(p);
    if (para.text.find(u'\x02') == std::u16string::npos || IsNote(doc.Stories()[para.story].kind)) continue;
    for (const InlineItem& it : doc.Items(p))
      if (it.kind == want && it.node.AttrOr(kIdAttr) == s.id) return Pos{p, it.offset};
  }
  return std::nullopt;
}

Result DeleteNote(Editor& editor, uint32_t story) {
  const Document& doc = editor.Doc();
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  if (story >= doc.Stories().size() || !IsNote(doc.Stories()[story].kind) || doc.Stories()[story].root == kNoElem)
    return Fail(u"There's no note here.");
  if (std::optional<Pos> ref = NoteReference(doc, story)) {
    Result r = editor.Delete(*ref, {ref->para, ref->offset + 1});
    if (r) r.caret = *ref;
    return r;
  }
  // A note nothing refers to (a file can have one): it goes alone.
  TreeEdit te(editor, {0, 0});
  te.Begin(u"Delete note");
  te.Affects(0, 0);
  if (!te.RemoveNote(story)) {
    te.Abort();
    return Fail(u"This note can't be deleted.");
  }
  te.Commit();
  if (!te.Refused().empty()) return Fail(te.Refused());
  Result r;
  r.caret = {0, 0};
  return r;
}

}  // namespace fw::doc::edit
