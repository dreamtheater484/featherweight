#include "doc/edit/Inline.h"

#include <algorithm>
#include <charconv>
#include <unordered_map>
#include <unordered_set>

#include "doc/edit/Styles.h"
#include "doc/edit/TreeEdit.h"
#include "doc/model/Internal.h"
#include "doc/model/Schema.h"
#include "doc/model/Utf.h"

namespace fw::doc::edit {

namespace {

constexpr uint32_t kFldSimple = Name("fldSimple"), kInstr = Name("instr"), kHyperlink = Name("hyperlink");
constexpr uint32_t kAnchor = Name("anchor"), kRelId = Name("r:id"), kRStyle = Name("rStyle"), kName = Name("name");
constexpr uint32_t kBookmarkStart = Name("bookmarkStart"), kBookmarkEnd = Name("bookmarkEnd");

const Schema& S() { return Schema::Get(); }

Result Fail(std::u16string why) {
  Result r;
  r.ok = false;
  r.message = std::move(why);
  return r;
}

Role RoleOf(const Tree& t, ElemId id) { return S().Type(S().Elem(t.elems[id].def).type).role; }

// Where a zero-length element goes at `offset` of a paragraph whose runs are split there: the
// innermost container spanning the offset that can hold it, before the first content that starts
// after it (zero-length content at the offset stays before it).
struct Spot {
  ElemId parent = kNoElem, before = kNoElem;
};

std::optional<Spot> SpotAt(const Document& doc, const Tree& t, uint32_t para, uint32_t offset, uint32_t marker) {
  ElemId parent = doc.Para(para).elem;
  for (int depth = 0; depth < 64; ++depth) {
    ElemId spanning = kNoElem, before = kNoElem;
    for (ElemId c = t.elems[parent].first; c != kNoElem; c = t.elems[c].next) {
      const Elem& e = t.elems[c];
      if (e.start < offset && offset < e.start + e.length) {
        spanning = c;
        break;
      }
      if (e.start > offset || (e.start == offset && e.length > 0)) {
        before = c;
        break;
      }
    }
    if (spanning == kNoElem) {
      if (S().Child(S().Elem(t.elems[parent].def).type, marker) == kNoDef) return std::nullopt;
      return Spot{parent, before};
    }
    const Role role = RoleOf(t, spanning);
    if ((t.elems[spanning].flags & kDisplayOnly) || role == Role::Run || role == Role::Object || role == Role::Text ||
        role == Role::Char)
      return std::nullopt;  // kept content, or a run that isn't split there
    parent = spanning;
  }
  return std::nullopt;
}

// The siblings that hold exactly [from, to) of a paragraph whose runs are split at both ends.
std::optional<std::pair<ElemId, ElemId>> SiblingsFor(const Document& doc, const Tree& t, uint32_t para, uint32_t from,
                                                      uint32_t to) {
  ElemId parent = doc.Para(para).elem;
  for (int depth = 0; depth < 64; ++depth) {
    ElemId first = kNoElem;
    for (ElemId c = t.elems[parent].first; c != kNoElem; c = t.elems[c].next) {
      const Elem& e = t.elems[c];
      if (e.length == 0 || e.start + e.length <= from) continue;
      first = c;
      break;
    }
    if (first == kNoElem) return std::nullopt;
    const Elem& f = t.elems[first];
    if (f.start == from) {
      for (ElemId c = first; c != kNoElem; c = t.elems[c].next) {
        const Elem& e = t.elems[c];
        if (e.start + e.length == to) return std::pair{first, c};
        if (e.start + e.length > to) break;
      }
    }
    if (f.start > from || f.start + f.length < to || (t.elems[first].flags & kDisplayOnly) || RoleOf(t, first) == Role::Run)
      return std::nullopt;
    parent = first;  // inside a container that holds all of it
  }
  return std::nullopt;
}

// The run of text at `offset` (after it, else before it).
std::optional<TextRun> RunAt(const Document& doc, Pos at) {
  std::optional<TextRun> before;
  for (const TextRun& r : doc.Runs(at.para)) {
    if (r.start <= at.offset && at.offset < r.end) return r;
    if (r.end == at.offset && r.start < r.end) before = r;
  }
  return before;
}

bool InField(const TextRun& r) { return r.simpleField || r.fieldDepth > 0 || r.fieldCode; }

// A web or mail address's relationship from the paragraph's part, added when missing. Outside
// undo, as Editor::SetLink's: an unused relationship is harmless.
std::string LinkRelationship(Document& doc, uint32_t para, std::string_view target) {
  const uint32_t part = doc.Stories()[doc.Para(para).story].part;
  if (part >= doc.Parts().size()) return {};
  const std::string& name = doc.Parts()[part].name;
  const std::string type = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink";
  pkg::RelationshipSet set;
  if (const pkg::RelationshipSet* now = doc.Package().RelationshipsOf(name)) set = *now;
  else set.source = name;
  for (const pkg::Relationship& r : set.items)
    if (r.external && r.type == type && r.target == target) return r.id;
  std::string rid;
  for (int n = 1; rid.empty(); ++n)
    if (std::string id = "rIdLink" + std::to_string(n); !set.Find(id)) rid = id;
  set.items.push_back(pkg::Relationship{rid, type, std::string(target), true, {}});
  doc.Package().SetRelationships(std::move(set));
  return rid;
}

// The editor hands out its document as const to readers; these operations change it through
// TreeEdit, as the formatting commands do, and its package's relationships as SetLink does.
Document& Mutable(Editor& editor) { return const_cast<Document&>(editor.Doc()); }

bool SameName(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

std::string HyperlinkStyleId(const Document& doc) {
  for (const Style& s : doc.Styles())
    if (s.type == StyleType::Character && SameName(s.name, "Hyperlink")) return std::string(s.id);
  return {};
}

// Adds the bookmark without checking its name (Word's hidden ones start with "_").
Result AddBookmark(Editor& editor, Pos from, Pos to, std::string_view name) {
  const Document& doc = editor.Doc();
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  if (editor.Compare(to, from) < 0) std::swap(from, to);
  if (from.para >= doc.Paragraphs().size() || to.para >= doc.Paragraphs().size() ||
      doc.Para(from.para).story != doc.Para(to.para).story || from.offset > doc.Para(from.para).text.size() ||
      to.offset > doc.Para(to.para).text.size())
    return Fail(u"A bookmark starts and ends in the same part of the document.");
  // An id no bookmark has: above every one in the document.
  int64_t top = -1;
  for (const PairInfo& p : doc.Pairs()) {
    if (p.kind != pkg::RefKind::BookmarkId) continue;
    int64_t v = 0;
    auto [ptr, ec] = std::from_chars(p.id.data(), p.id.data() + p.id.size(), v);
    if (ec == std::errc() && ptr == p.id.data() + p.id.size()) top = std::max(top, v);
  }
  const std::string id = std::to_string(top + 1);
  TreeEdit te(editor, from);
  te.Begin(u"Bookmark");
  te.Local();
  Tree& t = te.GetTree();
  if (!te.SplitRunAt(from.para, from.offset) || !te.SplitRunAt(to.para, to.offset)) {
    te.Abort();
    return Fail(u"A bookmark can't start or end here.");
  }
  auto s = SpotAt(doc, t, from.para, from.offset, kBookmarkStart);
  ElemId start = s ? te.InsertElement(s->parent, kBookmarkStart, s->before, from.offset) : kNoElem;
  auto e = start == kNoElem ? std::nullopt : SpotAt(doc, t, to.para, to.offset, kBookmarkEnd);
  ElemId end = e ? te.InsertElement(e->parent, kBookmarkEnd, e->before, to.offset) : kNoElem;
  if (end == kNoElem) {
    te.Abort();
    return Fail(u"A bookmark can't start or end here.");
  }
  te.SetAttr(start, names::kId, id);
  te.SetAttr(start, kName, name);
  te.SetAttr(end, names::kId, id);
  te.Commit();
  Result r;
  r.caret = to;
  return r;
}

}  // namespace

// ---------- Fields ----------

bool InsideField(const Document& doc, Pos at) {
  if (at.para >= doc.Paragraphs().size()) return false;
  const std::u16string& text = doc.Para(at.para).text;
  if (at.offset > text.size()) return false;
  const std::vector<TextRun> runs = doc.Runs(at.para);
  const TextRun* before = nullptr;
  const TextRun* after = nullptr;
  for (const TextRun& r : runs) {
    if (r.start < at.offset && at.offset <= r.end) before = &r;
    if (r.start <= at.offset && at.offset < r.end && !after) after = &r;
  }
  if (before && after && before->simpleField && before->simpleField == after->simpleField) return true;
  // The fields open after the character before `at` (an end closes one), or before the first.
  if (before) return before->fieldDepth - (text[at.offset - 1] == u'\x15' ? 1 : 0) > 0;
  if (after) return after->fieldDepth - (text[at.offset] == u'\x13' ? 1 : 0) > 0;
  return false;
}

Result InsertField(Editor& editor, Pos at, std::string_view instr, std::u16string_view result, std::u16string name) {
  const Document& doc = editor.Doc();
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  if (instr.empty() || !XmlSafe(instr) || result.empty() || at.para >= doc.Paragraphs().size() ||
      result.find_first_of(u"\r\n\v\t") != std::u16string_view::npos)
    return Fail(u"This field can't be inserted.");
  if (auto r = RunAt(doc, at); r && (InField(*r) || r->displayOnly))
    return Fail(u"A field can't go inside another field.");
  editor.BeginGroup(name);
  Result typed = editor.InsertText(at, result);
  if (!typed) {
    editor.EndGroup();
    return typed;
  }
  const uint32_t para = at.para, from = at.offset, to = typed.caret.offset;
  bool ok = typed.caret.para == para && to == from + result.size();
  for (const TextRun& r : doc.Runs(para))
    if (r.end > from && r.start < to && (InField(r) || r.displayOnly)) ok = false;
  if (ok) {
    TreeEdit te(editor, typed.caret);
    te.Begin(name);
    te.Local();
    ElemId fld = kNoElem;
    if (te.SplitRunAt(para, from) && te.SplitRunAt(para, to))
      if (auto kids = SiblingsFor(doc, te.GetTree(), para, from, to)) fld = te.Wrap(kids->first, kids->second, kFldSimple);
    if (fld != kNoElem) {
      te.SetAttr(fld, kInstr, instr);
      te.Commit();
    } else {
      te.Abort();
      ok = false;
    }
  }
  editor.EndGroup();
  if (!ok) {
    editor.Undo();  // the text alone: never a field half made
    return Fail(u"A field can't be inserted here.");
  }
  return typed;
}

// ---------- Links ----------

std::optional<LinkInfo> LinkAt(const Document& doc, Pos at) {
  if (at.para >= doc.Paragraphs().size()) return std::nullopt;
  Node link;
  std::vector<TextRun> runs = doc.Runs(at.para);
  for (const TextRun& r : runs)
    if (r.hyperlink && !r.displayOnly && r.start <= at.offset && at.offset < r.end) link = r.hyperlink;
  if (!link)
    for (const TextRun& r : runs)
      if (r.hyperlink && !r.displayOnly && r.end == at.offset && r.start < r.end) link = r.hyperlink;
  if (!link) return std::nullopt;
  LinkInfo info;
  info.from = {at.para, link.TextStart()};
  info.to = {at.para, link.TextStart() + link.TextLength()};
  info.target = doc.HyperlinkTarget(link);
  info.elem = link.Id();
  return info;
}

Result InsertLink(Editor& editor, Pos from, Pos to, std::string_view target, std::u16string_view text) {
  const Document& doc = editor.Doc();
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  if (target.empty() || (target[0] == '#' && target.size() == 1)) return Fail(u"Type an address for the link.");
  if (!XmlSafe(target)) return Fail(u"A link's address can't hold control characters.");
  if (editor.Compare(to, from) < 0) std::swap(from, to);
  if (from.para != to.para) return Fail(u"Select text in one paragraph to make it a link.");
  if (from == to && text.empty()) return Fail(u"Type the text to show for the link.");
  if (text.find_first_of(u"\r\n") != std::u16string_view::npos) return Fail(u"A link's text is one paragraph.");
  // Where the text will be: never inside another link or a field.
  if (from == to) {
    if (auto r = RunAt(doc, from); r && (r->hyperlink || InField(*r) || r->displayOnly || r->contentControl))
      return Fail(u"A link can't go here.");
  } else {
    for (const TextRun& r : doc.Runs(from.para))
      if (r.end > from.offset && r.start < to.offset && (r.hyperlink || InField(r) || r.displayOnly))
        return Fail(u"This text can't be made a link.");
  }
  editor.BeginGroup(u"Link");
  Pos a = from, b = to;
  bool typed = false;
  const std::u16string_view current = std::u16string_view(doc.Para(from.para).text).substr(from.offset, to.offset - from.offset);
  if (!text.empty() && text != current) {
    Result r = from == to ? editor.InsertText(from, text) : editor.Replace(from, to, text);
    if (!r) {
      editor.EndGroup();
      return r;
    }
    typed = true;
    b = r.caret;
  }
  Result r = b.para == a.para ? editor.SetLink(a, b, target) : Fail(u"This text can't be made a link.");
  if (r) {
    TreeEdit te(editor, b);
    ApplyStyle(te, {a, b}, "Hyperlink");
  }
  editor.EndGroup();
  if (!r) {
    if (typed) editor.Undo();
    return r;
  }
  r.caret = b;
  return r;
}

Result EditLink(Editor& editor, Pos at, std::string_view target, std::u16string_view text) {
  const Document& doc = editor.Doc();
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  std::optional<LinkInfo> link = LinkAt(doc, at);
  if (!link) return Fail(u"There's no link here.");
  if (target.empty() || (target[0] == '#' && target.size() == 1)) return Fail(u"Type an address for the link.");
  if (!XmlSafe(target)) return Fail(u"A link's address can't hold control characters.");
  if (text.find_first_of(u"\r\n") != std::u16string_view::npos) return Fail(u"A link's text is one paragraph.");
  const Pos from = link->from;
  const std::u16string current(std::u16string_view(doc.Para(from.para).text).substr(from.offset, link->to.offset - from.offset));
  editor.BeginGroup(u"Edit Link");
  bool ok = true;
  if (!text.empty() && text != current) {
    // The new text goes in after the link's first character, so it is inside the link with that
    // character's formatting; then the old text goes.
    const auto n = static_cast<uint32_t>(text.size());
    ok = editor.InsertText({from.para, from.offset + 1}, text) &&
         editor.Delete({from.para, from.offset + 1 + n}, {from.para, link->to.offset + n}) &&
         editor.Delete(from, {from.para, from.offset + 1});
    link = ok ? LinkAt(doc, from) : std::nullopt;
    ok = link && link->from == from && link->to == Pos{from.para, from.offset + n};
  }
  if (ok && target != link->target) {
    std::string rid;
    if (target[0] != '#') rid = LinkRelationship(Mutable(editor), from.para, target);
    if (target[0] != '#' && rid.empty()) {
      ok = false;
    } else {
      TreeEdit te(editor, link->to);
      te.Begin(u"Edit Link");
      te.Local();
      if (rid.empty()) {
        te.RemoveAttr(link->elem, kRelId);
        te.SetAttr(link->elem, kAnchor, target.substr(1));
      } else {
        te.RemoveAttr(link->elem, kAnchor);
        te.SetAttr(link->elem, kRelId, rid);
      }
      te.Commit();
    }
  }
  editor.EndGroup();
  if (!ok) {
    editor.Undo();
    return Fail(u"This link can't be changed.");
  }
  Result r;
  r.caret = link->to;
  return r;
}

Result RemoveLink(Editor& editor, Pos at) {
  const Document& doc = editor.Doc();
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  std::optional<LinkInfo> link = LinkAt(doc, at);
  if (!link) return Fail(u"There's no link here.");
  const std::string style = HyperlinkStyleId(doc);
  TreeEdit te(editor, at);
  te.Begin(u"Remove Link");
  te.Local();
  Tree& t = te.GetTree();
  // The link's look goes with it.
  if (!style.empty()) {
    std::vector<ElemId> runs;
    for (ElemId c = t.elems[link->elem].first; c != kNoElem; c = t.elems[c].next)
      if (Node(&t, c).Is(names::kR) && Node(&t, c).Child(names::kRPr).Child(kRStyle).AttrOr(names::kVal) == style)
        runs.push_back(c);
    for (ElemId run : runs) te.Remove(te.Child(run, names::kRPr, false), kRStyle);
  }
  if (!te.Unwrap(link->elem)) {
    te.Abort();
    return Fail(u"This link holds content Featherweight keeps as it is, so it stays.");
  }
  te.Commit();
  Result r;
  r.caret = at;
  return r;
}

// ---------- Bookmarks ----------

std::vector<BookmarkInfo> Bookmarks(const Document& doc) {
  std::vector<BookmarkInfo> out;
  for (const PairInfo& p : doc.Pairs()) {
    if (p.kind != pkg::RefKind::BookmarkId || !p.open) continue;
    BookmarkInfo b;
    b.name = std::string(p.open.AttrOr(kName));
    if (b.name.empty()) continue;
    b.from = p.openPos;
    b.to = p.close ? p.closePos : p.openPos;
    b.story = p.story;
    b.hidden = b.name[0] == '_';
    out.push_back(std::move(b));
  }
  std::stable_sort(out.begin(), out.end(), [](const BookmarkInfo& a, const BookmarkInfo& b) {
    return a.from.para != b.from.para ? a.from.para < b.from.para : a.from.offset < b.from.offset;
  });
  return out;
}

std::optional<BookmarkInfo> FindBookmark(const Document& doc, std::string_view name) {
  for (BookmarkInfo& b : Bookmarks(doc))
    if (SameName(b.name, name)) return std::move(b);
  return std::nullopt;
}

bool ValidBookmarkName(std::string_view utf8) {
  if (utf8.empty() || !XmlSafe(utf8)) return false;
  const std::u16string s = ToUtf16(utf8);
  if (s.empty() || s.size() > 40) return false;
  auto letter = [](char16_t c) {
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') ||
           (c >= 0xC0 && c != 0xD7 && c != 0xF7 && !(c >= 0x2000 && c <= 0x2BFF) && !(c >= 0x3000 && c <= 0x303F) &&
            !(c >= 0xD800 && c <= 0xDFFF) && !(c >= 0xFE30 && c <= 0xFE4F) && !(c >= 0xFF00 && c <= 0xFF0F));
  };
  if (!letter(s[0])) return false;
  for (char16_t c : s)
    if (!letter(c) && !(c >= u'0' && c <= u'9') && c != u'_') return false;
  return true;
}

Result InsertBookmark(Editor& editor, Pos from, Pos to, std::string_view name) {
  if (!ValidBookmarkName(name))
    return Fail(u"A bookmark's name starts with a letter and holds only letters, digits and underscores (at most 40).");
  // As in Word, adding a name that's taken moves that bookmark here.
  if (FindBookmark(editor.Doc(), name)) {
    editor.BeginGroup(u"Bookmark");
    Result gone = DeleteBookmark(editor, name);
    Result r = gone ? AddBookmark(editor, from, to, name) : gone;
    editor.EndGroup();
    if (!r && gone) editor.Undo();
    return r;
  }
  return AddBookmark(editor, from, to, name);
}

Result DeleteBookmark(Editor& editor, std::string_view name) {
  const Document& doc = editor.Doc();
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  Node open, close;
  Pos at;
  for (const PairInfo& p : doc.Pairs())
    if (p.kind == pkg::RefKind::BookmarkId && p.open && SameName(p.open.AttrOr(kName), name)) {
      open = p.open;
      close = p.close;
      at = p.openPos;
      break;
    }
  if (!open) return Fail(u"There's no bookmark with that name.");
  TreeEdit te(editor, at);
  te.Begin(u"Delete Bookmark");
  te.Local();
  te.RemoveElement(open.Id());
  if (close) te.RemoveElement(close.Id());
  te.Commit();
  Result r;
  r.caret = at;
  return r;
}

std::vector<std::string> BookmarksForParagraphs(Editor& editor, const std::vector<uint32_t>& paras,
                                                std::string_view prefix) {
  const Document& doc = editor.Doc();
  std::vector<std::string> out(paras.size());
  if (prefix.empty() || !editor.CanEdit()) return out;
  auto starts = [&](std::string_view name) { return name.size() >= prefix.size() && SameName(name.substr(0, prefix.size()), prefix); };
  // The bookmarks there are: their names (to make new ones unique) and where those with the prefix start.
  std::unordered_set<std::string> taken;
  std::unordered_map<uint32_t, std::string> at;
  for (const BookmarkInfo& b : Bookmarks(doc)) {
    std::string lower = b.name;
    for (char& c : lower) c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    taken.insert(std::move(lower));
    if (starts(b.name)) at.emplace(b.from.para, b.name);
  }
  int64_t top = -1;
  for (const PairInfo& p : doc.Pairs()) {
    if (p.kind != pkg::RefKind::BookmarkId) continue;
    int64_t v = 0;
    auto [ptr, ec] = std::from_chars(p.id.data(), p.id.data() + p.id.size(), v);
    if (ec == std::errc() && ptr == p.id.data() + p.id.size()) top = std::max(top, v);
  }
  TreeEdit te(editor, paras.empty() ? Pos{} : Pos{paras.front(), 0});
  te.Begin(u"Bookmark");
  te.Local();
  Tree& t = te.GetTree();
  uint32_t n = 0;
  for (size_t i = 0; i < paras.size(); ++i) {
    const uint32_t para = paras[i];
    if (para >= doc.Paragraphs().size()) continue;
    if (auto it = at.find(para); it != at.end()) {
      out[i] = it->second;
      continue;
    }
    std::string name;
    while (name.empty() || taken.count(name)) {
      name = std::string(prefix) + std::to_string(100000000u + ++n);
      for (char& c : name) c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    taken.insert(name);
    name = std::string(prefix) + name.substr(prefix.size());  // the prefix as given
    const auto len = static_cast<uint32_t>(doc.Para(para).text.size());
    if (!te.SplitRunAt(para, 0) || !te.SplitRunAt(para, len)) continue;
    auto s = SpotAt(doc, t, para, 0, kBookmarkStart);
    auto e = SpotAt(doc, t, para, len, kBookmarkEnd);
    if (!s || !e) continue;
    const ElemId start = te.InsertElement(s->parent, kBookmarkStart, s->before, 0);
    if (start == kNoElem) continue;
    // The end's spot again: the start may be its sibling now.
    e = SpotAt(doc, t, para, len, kBookmarkEnd);
    const ElemId end = e ? te.InsertElement(e->parent, kBookmarkEnd, e->before, len) : kNoElem;
    if (end == kNoElem) {
      te.RemoveElement(start);
      continue;
    }
    const std::string id = std::to_string(++top);
    te.SetAttr(start, names::kId, id);
    te.SetAttr(start, kName, name);
    te.SetAttr(end, names::kId, id);
    at.emplace(para, name);
    out[i] = name;
  }
  te.Commit();
  if (!te.Refused().empty()) std::fill(out.begin(), out.end(), std::string());
  return out;
}

std::string BookmarkForParagraph(Editor& editor, uint32_t para) {
  const Document& doc = editor.Doc();
  if (para >= doc.Paragraphs().size()) return {};
  for (const BookmarkInfo& b : Bookmarks(doc))
    if (b.from == Pos{para, 0}) return b.name;
  std::string name;
  for (uint32_t n = 1; name.empty() || FindBookmark(doc, name); ++n) name = "_Hlk" + std::to_string(100000000u + n);
  const auto len = static_cast<uint32_t>(doc.Para(para).text.size());
  return AddBookmark(editor, {para, 0}, {para, len}, name) ? name : std::string();
}

}  // namespace fw::doc::edit
