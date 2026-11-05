#include "doc/edit/Toc.h"

#include <algorithm>
#include <unordered_map>

#include "doc/edit/Format.h"
#include "doc/edit/Inline.h"
#include "doc/edit/Page.h"
#include "doc/edit/Styles.h"
#include "doc/edit/TreeEdit.h"
#include "doc/model/Internal.h"
#include "doc/model/Schema.h"
#include "doc/model/Utf.h"

namespace fw::doc::edit {

namespace {

constexpr uint32_t kNumPr = Name("numPr");
constexpr const char* kCode = " TOC \\o \"1-3\" \\h \\z \\u ";
// What Word gives a page number's field: not spell-checked, not shown in a web page.
constexpr const char* kPageRefRPr = "<w:rPr><w:noProof/><w:webHidden/></w:rPr>";

Result Fail(std::u16string why) {
  Result r;
  r.ok = false;
  r.message = std::move(why);
  return r;
}

char16_t Lower(char16_t c) { return c >= u'A' && c <= u'Z' ? static_cast<char16_t>(c - u'A' + u'a') : c; }

bool SameWord(std::u16string_view a, std::u16string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (Lower(a[i]) != Lower(b[i])) return false;
  return true;
}

std::string LowerAscii(std::string s) {
  for (char& c : s) c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
  return s;
}

// The levels of a TOC field's \o "1-3" switch; 3 without one.
int Levels(std::u16string_view code) {
  for (size_t i = 0; i + 1 < code.size(); ++i) {
    if (code[i] != u'\\' || Lower(code[i + 1]) != u'o') continue;
    size_t q = code.find(u'"', i + 2);
    if (q == std::u16string_view::npos) break;
    size_t dash = code.find(u'-', q + 1), close = code.find(u'"', q + 1);
    if (dash == std::u16string_view::npos || close == std::u16string_view::npos || dash > close) break;
    int n = 0;
    for (size_t k = dash + 1; k < close && code[k] >= u'0' && code[k] <= u'9'; ++k) n = n * 10 + (code[k] - u'0');
    return std::clamp(n, 1, 9);
  }
  return 3;
}

// The word after a field's type: a PAGEREF's bookmark.
std::string Argument(std::u16string_view code) {
  size_t i = 0;
  auto space = [&](size_t k) { return k < code.size() && (code[k] == u' ' || code[k] == u'\t'); };
  while (space(i)) ++i;
  while (i < code.size() && !space(i)) ++i;
  while (space(i)) ++i;
  size_t end = i;
  while (end < code.size() && !space(end) && code[end] != u'\\') ++end;
  return ToUtf8(code.substr(i, end - i));
}

// A paragraph's text as shown: field results without their codes, no deleted text, breaks and
// tabs as spaces, without the characters that stand for something else (notes, drawings).
std::u16string Shown(const Document& doc, uint32_t para) {
  static constexpr uint32_t kDel = Name("del"), kMoveFrom = Name("moveFrom");
  const std::u16string& text = doc.Para(para).text;
  std::u16string out;
  for (const TextRun& r : doc.Runs(para)) {
    if (r.fieldCode || (r.revision && (r.revision.Is(kDel) || r.revision.Is(kMoveFrom)))) continue;
    for (uint32_t i = r.start; i < r.end && i < text.size(); ++i) {
      const char16_t c = text[i];
      if (c == u'\t' || c == 0x0B || c == 0x0C || c == 0x0E || c == u' ' || c == 0x00A0) {
        if (!out.empty() && out.back() != u' ') out += u' ';
        continue;
      }
      if (c < 0x20 || c == 0xFFFC || c == 0x00AD) continue;
      out += c;
    }
  }
  while (!out.empty() && out.back() == u' ') out.pop_back();
  return out;
}

// The width of the text in the section of `para`, in twips: where a right tab at its edge goes.
int32_t TextWidth(const Document& doc, uint32_t para) {
  const uint32_t section = SectionOf(doc, para);
  if (section >= doc.Sections().size()) return 9360;
  const SectionProps& sp = doc.Sections()[section].props;
  int32_t w = sp.pageWidth.Value() - sp.marginStart.Value() - sp.marginEnd.Value() - sp.gutter.Value();
  if (sp.columns.count > 1) {
    if (!sp.columns.equalWidth && !sp.columns.cols.empty()) w = sp.columns.cols[0].width.Value();
    else w = (w - sp.columns.space.Value() * (sp.columns.count - 1)) / sp.columns.count;
  }
  return std::max(w, 720);
}

int Compare(Pos a, Pos b) {
  if (a.para != b.para) return a.para < b.para ? -1 : 1;
  return a.offset == b.offset ? 0 : a.offset < b.offset ? -1 : 1;
}

// Makes a table of contents at `at`, with field code `code` and headings 1 to `levels`.
Result Build(Editor& editor, Pos at, const PageText& pages, const std::string& code, int levels, const std::u16string& name) {
  const Document& doc = editor.Doc();
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  if (at.para >= doc.Paragraphs().size() || at.offset > doc.Para(at.para).text.size())
    return Fail(u"That place isn't in the document.");
  if (doc.Para(at.para).story != doc.MainStory())
    return Fail(u"A table of contents goes in the document's text, not in a header, a footer, a note or a text box.");
  if (InsideField(doc, at)) return Fail(u"A table of contents can't go inside a field.");

  // The headings, and their pages as laid out now (the entries' first numbers).
  std::vector<TocHeading> headings = TocHeadings(doc, levels);
  std::vector<uint32_t> paras;
  std::vector<std::u16string> shown;
  for (const TocHeading& h : headings) {
    paras.push_back(h.para);
    std::u16string page = pages ? pages(h.para) : std::u16string();
    shown.push_back(page.empty() ? u"1" : page);
  }

  TreeEdit check(editor, at);
  if (!check.CanChange({at.para})) return Fail(check.Refused());

  EditGroup group(editor, name);
  const std::vector<std::string> marks = BookmarksForParagraphs(editor, paras, "_Toc");

  // Its own paragraphs: before `at`'s paragraph at its start, else after the part before `at`;
  // the field ends in an empty paragraph after the entries (`at`'s, when it's empty).
  Pos q = at;
  if (q.offset > 0) {
    Result r = editor.SplitParagraph(q);
    if (!r) return r;
    q = r.caret;
  }
  bool endIsNew = false;
  if (!doc.Para(q.para).text.empty()) {
    Result r = editor.SplitParagraph({q.para, 0});
    if (!r) return r;
    endIsNew = true;
  }
  std::u16string text;
  for (size_t i = 0; i < headings.size(); ++i) text += headings[i].text + u"\t" + shown[i] + u"\r";
  if (headings.empty()) text = u"No table of contents entries found.\r";
  const uint32_t first = q.para;
  const auto count = static_cast<uint32_t>(headings.empty() ? 1 : headings.size());
  const uint32_t endPara = first + count;
  if (Result r = editor.InsertText({first, 0}, text); !r) return r;
  if (endPara >= doc.Paragraphs().size() || !doc.Para(endPara).text.empty())
    return Fail(u"A table of contents can't go here.");

  // The styles part, outside undo, once the entries are in: a table that can't go there leaves
  // none behind (torture finding 30). Then their styles, the entries' right tab at the text's
  // edge, and no list: one step.
  if (check.EnsurePart(PartKind::Styles) == kNoElem) return Fail(u"This document's styles can't change.");
  {
    TreeEdit te(editor, {first, 0});
    te.Begin(name);
    te.Affects(first, endPara);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t p = first + i;
      const auto len = static_cast<uint32_t>(doc.Para(p).text.size());
      ApplyStyle(te, {{p, 0}, {p, len}}, headings.empty() ? std::string("Normal") : "toc " + std::to_string(headings[i].level));
    }
    if (endIsNew) ApplyStyle(te, {{endPara, 0}, {endPara, 0}}, "Normal");
    for (uint32_t p = first; p <= endPara; ++p)
      if (doc.ParaNode(p).Path({names::kPPr, kNumPr})) te.Remove(te.Child(doc.Para(p).elem, names::kPPr), kNumPr);
    te.Reindex();
    if (!headings.empty())
      ChangeTabStops(te, {{first, 0}, {endPara - 1, static_cast<uint32_t>(doc.Para(endPara - 1).text.size())}}, {},
                     {TabStop{Twips(TextWidth(doc, first)), TabAlign::End, TabLeader::Dot}});
    te.Commit();
    if (!te.Refused().empty()) return Fail(te.Refused());
  }

  // Each entry a link to its heading, its page number a PAGEREF field inside the link.
  for (size_t i = 0; i < headings.size(); ++i) {
    if (marks[i].empty()) continue;
    const auto p = static_cast<uint32_t>(first + i);
    const auto len = static_cast<uint32_t>(doc.Para(p).text.size());
    if (Result r = editor.SetLink({p, 0}, {p, len}, "#" + marks[i]); !r) return r;
    const auto pageAt = static_cast<uint32_t>(len - shown[i].size());
    if (Result r = editor.WrapInField({p, pageAt}, {p, len}, " PAGEREF " + marks[i] + " \\h ", kPageRefRPr, name); !r) return r;
  }
  // The TOC field around them all, its ends in the paragraphs themselves.
  if (Result r = editor.WrapInField({first, 0}, {endPara, 0}, code, "", name); !r) return r;

  // The pages as they are with the table in.
  if (pages)
    if (std::optional<TocInfo> toc = TocAt(doc, {first, 0}))
      if (Result r = UpdateTocPages(editor, *toc, pages); !r) return r;
  group.done = true;
  Result out;
  out.caret = {endPara, std::min<uint32_t>(1, static_cast<uint32_t>(doc.Para(endPara).text.size()))};
  return out;
}

}  // namespace

std::vector<TocHeading> TocHeadings(const Document& doc, int levels) {
  std::vector<TocHeading> out;
  if (doc.MainStory() == kNone) return out;
  // Not the entries of a table of contents there is.
  const std::vector<TocInfo> tocs = TablesOfContents(doc);
  for (uint32_t p : doc.StoryParagraphs(doc.MainStory())) {
    const ParaProps& pp = doc.ResolvedPara(p);
    if (pp.outlineLevel < 0 || pp.outlineLevel >= levels) continue;
    bool inToc = false;
    for (const TocInfo& t : tocs) inToc = inToc || (p >= t.begin.para && p <= t.end.para);
    if (inToc) continue;
    std::u16string text = Shown(doc, p);
    if (text.empty()) continue;
    TocHeading h;
    h.para = p;
    h.level = pp.outlineLevel + 1;
    if (std::optional<ListLabel> label = doc.Label(p); label && !label->bullet && !label->text.empty())
      h.text = label->text + u"\t";
    h.text += text;
    out.push_back(std::move(h));
  }
  return out;
}

std::vector<TocInfo> TablesOfContents(const Document& doc) {
  std::vector<TocInfo> out;
  if (doc.MainStory() == kNone) return out;
  for (const Field& f : doc.Fields(doc.MainStory())) {
    if (f.simple || f.depth != 0 || !f.complete || !SameWord(f.Type(), u"TOC")) continue;
    TocInfo t;
    t.begin = f.begin;
    t.separator = f.separator;
    t.end = f.end;
    t.code = f.code;
    t.levels = Levels(f.code);
    out.push_back(std::move(t));
  }
  return out;
}

std::optional<TocInfo> TocAt(const Document& doc, Pos at) {
  for (TocInfo& t : TablesOfContents(doc))
    if (Compare(t.begin, at) <= 0 && Compare(at, {t.end.para, t.end.offset + 1}) <= 0) return std::move(t);
  return std::nullopt;
}

Result InsertToc(Editor& editor, Pos at, const PageText& pages) {
  return Build(editor, at, pages, kCode, 3, u"Table of Contents");
}

Result UpdateTocPages(Editor& editor, const TocInfo& toc, const PageText& pages) {
  const Document& doc = editor.Doc();
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  const std::vector<Field> fields = doc.Fields(doc.MainStory());
  uint32_t index = kNone;
  for (uint32_t k = 0; k < fields.size() && index == kNone; ++k)
    if (fields[k].begin == toc.begin && SameWord(fields[k].Type(), u"TOC")) index = k;
  if (index == kNone) return Fail(u"There's no table of contents here.");
  std::unordered_map<std::string, uint32_t> marks;  // bookmark (lower case) -> its paragraph
  for (const BookmarkInfo& b : Bookmarks(doc)) marks.emplace(LowerAscii(b.name), b.from.para);
  struct Fix {
    Pos from, to;
    std::u16string text;
  };
  std::vector<Fix> fixes;
  for (const Field& f : fields) {
    if (f.parent != index || f.simple || !f.complete || f.separator.para == kNone || !SameWord(f.Type(), u"PAGEREF"))
      continue;
    auto it = marks.find(LowerAscii(Argument(f.code)));
    if (it == marks.end()) continue;
    const std::u16string want = pages ? pages(it->second) : std::u16string();
    if (want.empty() || f.result == want) continue;
    fixes.push_back({{f.separator.para, f.separator.offset + 1}, f.end, want});
  }
  Result out;
  out.caret = toc.end;
  if (fixes.empty()) return out;
  EditGroup group(editor, u"Update Table");
  for (auto it = fixes.rbegin(); it != fixes.rend(); ++it)
    if (Result r = editor.Replace(it->from, it->to, it->text); !r) return r;
  group.done = true;
  if (std::optional<TocInfo> now = TocAt(doc, toc.begin)) out.caret = now->end;
  return out;
}

Result UpdateToc(Editor& editor, const TocInfo& toc, const PageText& pages) {
  const Document& doc = editor.Doc();
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  if (!TocAt(doc, toc.begin)) return Fail(u"There's no table of contents here.");
  EditGroup group(editor, u"Update Table");
  if (Result r = editor.Delete(toc.begin, {toc.end.para, toc.end.offset + 1}); !r) return r;
  Result r = Build(editor, toc.begin, pages, ToUtf8(toc.code), toc.levels, u"Update Table");
  if (!r) return r;
  group.done = true;
  return r;
}

}  // namespace fw::doc::edit
