#include "doc/edit/Page.h"

#include <algorithm>
#include <string>

#include "doc/edit/TreeEdit.h"
#include "doc/model/Internal.h"
#include "doc/model/Schema.h"

namespace fw::doc::edit {

using namespace names;

namespace {

constexpr uint32_t kPgSz = Name("pgSz"), kPgMar = Name("pgMar"), kCols = Name("cols");
constexpr uint32_t kPgNumType = Name("pgNumType"), kSectPrChange = Name("sectPrChange");

// Narrower than this, a column of text or a page of it isn't one (Word refuses them too).
constexpr int32_t kMinText = 720;  // half an inch
constexpr int32_t kMaxPage = 31680;  // 22 inches, Word's largest paper

std::string Num(int64_t v) { return std::to_string(v); }

Result Fail(std::u16string why) {
  Result r;
  r.ok = false;
  r.message = std::move(why);
  return r;
}

// Ends the editor group a change began, however it returns; with `cancel`, undoing what the group
// did so far (a change refused halfway leaves nothing behind).
struct GroupEnd {
  Editor& ed;
  bool cancel = false;
  ~GroupEnd() { cancel ? ed.CancelGroup() : ed.EndGroup(); }
};

// The section's w:sectPr; the last section's is made at the end of the body when it has none.
ElemId SectPrOf(TreeEdit& ed, uint32_t section) {
  const Document& doc = ed.Doc();
  if (ElemId id = doc.Sections()[section].sectPr.Id(); id != kNoElem) return id;
  if (section + 1 != doc.Sections().size() || doc.MainStory() == kNone) return kNoElem;
  return ed.Child(doc.Stories()[doc.MainStory()].root, kSectPr);
}

std::string_view StartValue(SectionStart s) {
  switch (s) {
    case SectionStart::Continuous: return "continuous";
    case SectionStart::EvenPage: return "evenPage";
    case SectionStart::OddPage: return "oddPage";
    case SectionStart::NextColumn: return "nextColumn";
    default: return "nextPage";
  }
}

void SetStart(TreeEdit& ed, ElemId sectPr, SectionStart start) {
  if (start == SectionStart::NextPage) ed.Remove(sectPr, kType);  // the default
  else ed.SetAttr(ed.Child(sectPr, kType), kVal, StartValue(start));
}

// The width of text between a section's margins.
int32_t TextWidth(const SectionProps& p) {
  return p.pageWidth.Value() - p.marginStart.Value() - p.marginEnd.Value() - p.gutter.Value();
}

// Whether the columns fit between the margins, each at least kMinText wide.
bool ColumnsFit(const Columns& c, int32_t width) {
  if (c.count < 1 || c.count > 45) return false;
  if (!c.equalWidth && !c.cols.empty()) {
    int64_t total = 0;
    for (size_t i = 0; i < c.cols.size(); ++i) {
      if (c.cols[i].width.Value() < kMinText || c.cols[i].space.Value() < 0) return false;
      total += c.cols[i].width.Value() + (i + 1 < c.cols.size() ? c.cols[i].space.Value() : 0);
    }
    return total <= width;
  }
  if (c.space.Value() < 0) return false;
  return static_cast<int64_t>(width) - static_cast<int64_t>(c.count - 1) * c.space.Value() >= int64_t{kMinText} * c.count;
}

// An element and the known content under it, as XML for TreeEdit::AddXml: what Featherweight
// knows of it, without the markup it keeps (which may hold ids that must stay unique).
std::string KnownXml(Node n) {
  if (n.Namespace() == Ns::Other || n.Namespace() == Ns::None) return {};
  auto escape = [](std::string_view v) {
    std::string out;
    for (char c : v) {
      switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
      }
    }
    return out;
  };
  std::string out = "<" + std::string(NsPrefix(n.Namespace())) + ":" + std::string(n.LocalName());
  for (size_t i = 0; i < n.AttrCount(); ++i) {
    const AttrDef& a = n.AttrDefAt(i);
    if (a.ns == Ns::Other) continue;
    out += " ";
    if (a.ns != Ns::None) out += std::string(NsPrefix(a.ns)) + ":";
    out += std::string(a.local) + "=\"" + escape(n.AttrValueAt(i)) + "\"";
  }
  std::string kids;
  for (Node k : n.Kids()) kids += KnownXml(k);
  return out + (kids.empty() ? "/>" : ">" + kids + "</" + std::string(NsPrefix(n.Namespace())) + ":" +
                                          std::string(n.LocalName()) + ">");
}

// Copies what a section's w:sectPr knows into an empty one, without its tracked change
// (w:sectPrChange, whose id is the original's) and kept markup.
void CopySection(TreeEdit& ed, Node from, ElemId to) {
  for (Node c : from.Kids())
    if (!c.Is(kSectPrChange))
      if (const std::string xml = KnownXml(c); !xml.empty()) ed.AddXml(to, xml);
}

}  // namespace

bool PageSetup::Any() const {
  return width || height || landscape || top || bottom || left || right || header || footer || gutter;
}

uint32_t SectionOf(const Document& doc, uint32_t para) {
  const std::vector<Section>& sections = doc.Sections();
  if (sections.empty()) return 0;
  if (para < doc.Paragraphs().size() && doc.Para(para).story == doc.MainStory())
    for (uint32_t s = 0; s + 1 < sections.size(); ++s)
      if (sections[s].endPara != kNone && para <= sections[s].endPara) return s;
  return static_cast<uint32_t>(sections.size() - 1);
}

std::pair<uint32_t, uint32_t> SectionParagraphs(const Document& doc, uint32_t section) {
  const std::vector<Section>& sections = doc.Sections();
  const std::vector<uint32_t> paras = doc.MainStory() == kNone ? std::vector<uint32_t>{} : doc.StoryParagraphs(doc.MainStory());
  if (paras.empty() || section >= sections.size()) return {kNone, kNone};
  uint32_t first = paras.front(), last = paras.back();
  if (section > 0 && sections[section - 1].endPara != kNone) {
    auto it = std::upper_bound(paras.begin(), paras.end(), sections[section - 1].endPara);
    if (it != paras.end()) first = *it;
  }
  if (sections[section].endPara != kNone) last = sections[section].endPara;
  return {first, std::max(first, last)};
}

Result SetPageSetup(Editor& editor, uint32_t first, uint32_t last, const PageSetup& setup) {
  const Document& doc = editor.Doc();
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  if (first > last || last >= doc.Sections().size()) return Fail(u"There's no such section.");
  if (!setup.Any()) return Result{};
  // What each section becomes, checked before anything changes.
  std::vector<SectionProps> after;
  for (uint32_t s = first; s <= last; ++s) {
    SectionProps p = doc.Sections()[s].props;
    if (setup.landscape && *setup.landscape != p.landscape) {
      std::swap(p.pageWidth, p.pageHeight);
      std::swap(p.marginTop, p.marginStart);
      std::swap(p.marginBottom, p.marginEnd);
      p.landscape = *setup.landscape;
    }
    if (setup.width) p.pageWidth = *setup.width;
    if (setup.height) p.pageHeight = *setup.height;
    if (setup.top) p.marginTop = *setup.top;
    if (setup.bottom) p.marginBottom = *setup.bottom;
    if (setup.left) p.marginStart = *setup.left;
    if (setup.right) p.marginEnd = *setup.right;
    if (setup.header) p.headerDistance = *setup.header;
    if (setup.footer) p.footerDistance = *setup.footer;
    if (setup.gutter) p.gutter = *setup.gutter;
    const int32_t w = p.pageWidth.Value(), h = p.pageHeight.Value();
    if (w < kMinText || h < kMinText || w > kMaxPage || h > kMaxPage)
      return Fail(u"The paper can be from half an inch to 22 inches on each side.");
    if (p.marginStart.Value() < 0 || p.marginEnd.Value() < 0 || p.gutter.Value() < 0 || p.headerDistance.Value() < 0 ||
        p.footerDistance.Value() < 0)
      return Fail(u"Margins can't be less than zero.");
    if (TextWidth(p) < kMinText || h - std::abs(p.marginTop.Value()) - std::abs(p.marginBottom.Value()) < kMinText)
      return Fail(u"The margins leave too little room for text. Make them smaller, or the paper larger.");
    if (!ColumnsFit(p.columns, TextWidth(p)))
      return Fail(u"The columns don't fit between these margins. Make the margins smaller, or use fewer columns.");
    after.push_back(p);
  }

  TreeEdit ed(editor);
  ed.Begin(u"Page setup");
  ed.Affects(SectionParagraphs(doc, first).first, SectionParagraphs(doc, last).second);
  for (uint32_t s = first; s <= last; ++s) {
    const SectionProps& was = doc.Sections()[s].props;
    const SectionProps& p = after[s - first];
    const ElemId sectPr = SectPrOf(ed, s);
    if (sectPr == kNoElem) continue;
    if (p.pageWidth != was.pageWidth || p.pageHeight != was.pageHeight || p.landscape != was.landscape) {
      const ElemId sz = ed.Child(sectPr, kPgSz);
      ed.SetAttr(sz, kW, Num(p.pageWidth.Value()));
      ed.SetAttr(sz, Name("h"), Num(p.pageHeight.Value()));
      if (p.landscape) ed.SetAttr(sz, Name("orient"), "landscape");
      else ed.RemoveAttr(sz, Name("orient"));
      // The paper's code names the size it had; turning the page keeps the paper.
      if (p.pageWidth != was.pageWidth && p.pageWidth != was.pageHeight) ed.RemoveAttr(sz, Name("code"));
    }
    if (p.marginTop != was.marginTop || p.marginBottom != was.marginBottom || p.marginStart != was.marginStart ||
        p.marginEnd != was.marginEnd || p.headerDistance != was.headerDistance || p.footerDistance != was.footerDistance ||
        p.gutter != was.gutter) {
      // Word writes all seven (the schema requires them).
      const ElemId mar = ed.Child(sectPr, kPgMar);
      ed.SetAttr(mar, Name("top"), Num(p.marginTop.Value()));
      ed.SetAttr(mar, Name("right"), Num(p.marginEnd.Value()));
      ed.SetAttr(mar, Name("bottom"), Num(p.marginBottom.Value()));
      ed.SetAttr(mar, Name("left"), Num(p.marginStart.Value()));
      ed.SetAttr(mar, Name("header"), Num(p.headerDistance.Value()));
      ed.SetAttr(mar, Name("footer"), Num(p.footerDistance.Value()));
      ed.SetAttr(mar, Name("gutter"), Num(p.gutter.Value()));
    }
  }
  ed.Commit();
  return Result{};
}

Result SetColumns(Editor& editor, uint32_t first, uint32_t last, const ColumnSetup& setup) {
  const Document& doc = editor.Doc();
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  if (first > last || last >= doc.Sections().size()) return Fail(u"There's no such section.");
  Columns c;
  c.count = setup.widths.empty() ? setup.count : static_cast<int32_t>(setup.widths.size());
  c.space = setup.space;
  c.separator = setup.line;
  c.equalWidth = setup.widths.empty();
  c.cols = setup.widths;
  if (c.count < 1 || c.count > 45) return Fail(u"A page can have from 1 to 45 columns.");
  for (uint32_t s = first; s <= last; ++s)
    if (!ColumnsFit(c, TextWidth(doc.Sections()[s].props)))
      return Fail(u"The columns don't fit between the margins. Use fewer columns, or less space between them.");

  TreeEdit ed(editor);
  ed.Begin(u"Columns");
  ed.Affects(SectionParagraphs(doc, first).first, SectionParagraphs(doc, last).second);
  for (uint32_t s = first; s <= last; ++s) {
    const ElemId sectPr = SectPrOf(ed, s);
    if (sectPr == kNoElem) continue;
    const ElemId cols = ed.Child(sectPr, kCols);
    ed.Clear(cols);  // its w:col children (Clear copies a shared set first)
    if (c.count > 1) ed.SetAttr(cols, Name("num"), Num(c.count));
    else ed.RemoveAttr(cols, Name("num"));
    ed.SetAttr(cols, Name("space"), Num(c.space.Value()));
    if (c.separator) ed.SetAttr(cols, Name("sep"), "1");
    else ed.RemoveAttr(cols, Name("sep"));
    if (c.equalWidth) {
      ed.RemoveAttr(cols, Name("equalWidth"));
    } else {
      ed.SetAttr(cols, Name("equalWidth"), "0");
      for (size_t i = 0; i < c.cols.size(); ++i) {
        std::string xml = "<w:col w:w=\"" + Num(c.cols[i].width.Value()) + "\"";
        if (i + 1 < c.cols.size()) xml += " w:space=\"" + Num(c.cols[i].space.Value()) + "\"";
        ed.AddXml(cols, xml + "/>");
      }
    }
  }
  ed.Commit();
  return Result{};
}

Result SetSectionStart(Editor& editor, uint32_t section, SectionStart start) {
  const Document& doc = editor.Doc();
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  if (section >= doc.Sections().size()) return Fail(u"There's no such section.");
  if (doc.Sections()[section].props.start == start) return Result{};
  TreeEdit ed(editor);
  ed.Begin(u"Section start");
  ed.Affects(SectionParagraphs(doc, section).first, SectionParagraphs(doc, section).second, true);
  if (const ElemId sectPr = SectPrOf(ed, section); sectPr != kNoElem) SetStart(ed, sectPr, start);
  ed.Commit();
  return Result{};
}

Result InsertSectionBreak(Editor& editor, Pos at, SectionStart start) {
  const Document& doc = editor.Doc();
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  if (at.para >= doc.Paragraphs().size() || doc.Para(at.para).story != doc.MainStory())
    return Fail(u"A section break goes in the text of the document, not in a header, a note or a text box.");
  if (doc.ParaNode(at.para).Parent().Id() != doc.Stories()[doc.MainStory()].root)
    return Fail(u"A section break can't go in a table or a content control. Put it before or after it.");

  editor.BeginGroup(u"Section break");
  GroupEnd ender{editor};
  Result split = editor.SplitParagraph(at);
  if (!split) return split;
  const uint32_t a = at.para, b = split.caret.para;  // the halves
  const uint32_t section = SectionOf(doc, b);
  TreeEdit ed(editor, split.caret);
  ed.Begin(u"Section break");
  ed.Affects(a, SectionParagraphs(doc, static_cast<uint32_t>(doc.Sections().size() - 1)).second, true);
  // The first half ends a new section like the one it was in, which starts as that one did.
  const ElemId pPr = ed.Child(doc.Para(a).elem, kPPr);
  const ElemId made = ed.Child(pPr, kSectPr);
  const ElemId rest = SectPrOf(ed, section);
  if (rest != kNoElem) {
    CopySection(ed, Node(&ed.GetTree(), rest), made);
    // The rest starts as asked, and no longer restarts its page numbers (the new section does).
    SetStart(ed, rest, start);
    if (Node(&ed.GetTree(), rest).Child(kPgNumType).Has(Name("start")))
      ed.RemoveAttr(ed.Child(rest, kPgNumType), Name("start"));
  }
  ed.Commit();
  if (!ed.Refused().empty()) {
    ender.cancel = true;  // the split too
    return Fail(ed.Refused());
  }
  Result r;
  r.caret = split.caret;
  return r;
}

Result DeleteSectionBreak(Editor& editor, uint32_t section) {
  const Document& doc = editor.Doc();
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  if (section + 1 >= doc.Sections().size()) return Fail(u"The last section doesn't end in a break.");
  const uint32_t para = doc.Sections()[section].endPara;
  const bool empty = doc.Para(para).text.empty();
  const uint32_t next = editor.NextPara(para);
  // A break with text before it: its mark stays, as a paragraph mark.
  editor.BeginGroup(u"Delete section break");
  GroupEnd ender{editor};
  const KeptRefs kept = KeptReferences(doc, {para});
  TreeEdit ed(editor, {para, 0});
  ed.Begin(u"Delete section break");
  ed.Affects(SectionParagraphs(doc, section).first,
             SectionParagraphs(doc, static_cast<uint32_t>(doc.Sections().size() - 1)).second, true);
  for (const std::string& xml : kept.xml) ed.AddXml(kept.sectPr, xml);
  ed.Remove(ed.Child(doc.Para(para).elem, kPPr), kSectPr);
  ed.Commit();
  if (!ed.Refused().empty()) return Fail(ed.Refused());  // put back: nothing changed
  // An empty paragraph held only the break: it goes too, when a paragraph follows it.
  Result r;
  r.caret = {para, 0};
  if (empty && next != kNone && next == para + 1 && doc.Para(next).story == doc.MainStory()) {
    Result joined = editor.JoinParagraphs(para);
    if (joined) r.caret = joined.caret;
  }
  return r;
}

Result InsertBreak(Editor& editor, Pos at, bool column) {
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  // The break, then the paragraph split after it; where it can't be split (in a field's result),
  // the break comes out again (d1-corpus torture finding 26).
  editor.BeginGroup(column ? u"Column break" : u"Page break");
  GroupEnd ender{editor};
  Result r = editor.InsertText(at, std::u16string(1, column ? char16_t(0x0E) : char16_t(0x0C)));
  if (r) r = editor.SplitParagraph(r.caret);
  ender.cancel = !r;
  return r;
}

Result SetHyphenation(Editor& editor, const Hyphenation& h) {
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  if (h.zone && (h.zone->Value() < 0 || h.zone->Value() > 31680)) return Fail(u"The hyphenation zone can be up to 22 inches.");
  if (h.limit && (*h.limit < 0 || *h.limit > 32767)) return Fail(u"The limit can be from 0 (none) to 32767 lines.");
  TreeEdit ed(editor);
  const ElemId root = ed.EnsurePart(PartKind::Settings);
  if (root == kNoElem) return Fail(u"This document's settings can't be changed.");
  const DocSettings& s = editor.Doc().Settings();
  ed.Begin(u"Hyphenation");
  if (h.automatic && *h.automatic != s.autoHyphenation) {
    if (*h.automatic) ed.Child(root, Name("autoHyphenation"));
    else ed.Remove(root, Name("autoHyphenation"));
  }
  if (h.caps && *h.caps == s.doNotHyphenateCaps) {
    if (*h.caps) ed.Remove(root, Name("doNotHyphenateCaps"));
    else ed.Child(root, Name("doNotHyphenateCaps"));
  }
  if (h.zone && *h.zone != s.hyphenationZone) ed.SetAttr(ed.Child(root, Name("hyphenationZone")), kVal, Num(h.zone->Value()));
  if (h.limit && *h.limit != s.consecutiveHyphenLimit) {
    if (*h.limit == 0) ed.Remove(root, Name("consecutiveHyphenLimit"));
    else ed.SetAttr(ed.Child(root, Name("consecutiveHyphenLimit")), kVal, Num(*h.limit));
  }
  ed.Commit();
  return Result{};
}

}  // namespace fw::doc::edit
