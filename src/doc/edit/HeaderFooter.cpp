#include "doc/edit/HeaderFooter.h"

#include "doc/edit/Format.h"
#include "doc/edit/Forms.h"
#include "doc/edit/Inline.h"
#include "doc/edit/Styles.h"
#include "doc/edit/TreeEdit.h"
#include "doc/model/Internal.h"

namespace fw::doc::edit {

namespace {

constexpr uint32_t kHeaderRef = Name("headerReference"), kFooterRef = Name("footerReference"), kTitlePg = Name("titlePg");
constexpr uint32_t kPgNumType = Name("pgNumType"), kFmt = Name("fmt"), kStart = Name("start");
constexpr uint32_t kEvenOdd = Name("evenAndOddHeaders"), kPStyle = Name("pStyle");

std::string_view TypeName(HdrFtrType t) {
  return t == HdrFtrType::First ? "first" : t == HdrFtrType::Even ? "even" : "default";
}

Result Fail(std::u16string why) {
  Result r;
  r.ok = false;
  r.message = std::move(why);
  return r;
}

bool CanChange(const Editor& editor, uint32_t section, std::u16string& why) {
  if (!editor.CanEdit(&why)) return false;
  if (FormsProtected(editor.Doc())) {
    why = kFormsProtected;
    return false;
  }
  if (section >= editor.Doc().Sections().size()) {
    why = u"There's no such section.";
    return false;
  }
  return true;
}

// The section's w:sectPr; for a last section without one, a new one at the end of the body.
ElemId SectPr(TreeEdit& te, uint32_t section) {
  const Document& doc = te.Doc();
  const Section& sec = doc.Sections()[section];
  if (sec.sectPr) return sec.sectPr.Id();
  if (sec.endPara != kNone || doc.MainStory() == kNone) return kNoElem;
  return te.Child(doc.Stories()[doc.MainStory()].root, names::kSectPr);
}

// The section's own reference to a header (footer) of this type.
ElemId Reference(const Document& doc, uint32_t section, bool footer, HdrFtrType type) {
  for (Node ref : doc.Sections()[section].sectPr.Kids())
    if (ref.Is(footer ? kFooterRef : kHeaderRef) && ref.AttrOr(names::kType, "default") == TypeName(type)) return ref.Id();
  return kNoElem;
}

// A new header (footer) part of the section's own, empty or a copy of story `copyOf`, and the
// reference to it: one undo step.
uint32_t NewOwn(Editor& editor, uint32_t section, bool footer, HdrFtrType type, uint32_t copyOf, std::u16string& why,
                std::u16string label) {
  const Document& doc = editor.Doc();
  TreeEdit te(editor);
  te.EnsurePart(PartKind::Styles);  // for the Header (Footer) style; without it the paragraph has none
  std::string rid;
  const uint32_t story = te.NewStoryPart(footer ? PartKind::Footer : PartKind::Header, copyOf, rid, why);
  if (story == kNone) return kNone;
  te.Begin(std::move(label));
  const ElemId sp = SectPr(te, section);
  const ElemId ref = sp == kNoElem ? kNoElem
                                   : te.AddXml(sp, "<w:" + std::string(footer ? "footerReference" : "headerReference") +
                                                       " w:type=\"" + std::string(TypeName(type)) + "\" r:id=\"" + rid + "\"/>");
  if (ref == kNoElem) {
    te.Abort();
    why = u"This section can't get a header or footer.";
    return kNone;
  }
  if (copyOf == kNone) {
    const std::string style = EnsureStyle(te, footer ? "footer" : "header");
    const std::vector<uint32_t> paras = doc.StoryParagraphs(story);
    if (!style.empty() && !paras.empty())
      te.SetAttr(te.Path(doc.Para(paras.front()).elem, {names::kPPr, kPStyle}), names::kVal, style);
  }
  te.Commit();
  if (!te.Refused().empty()) {
    why = te.Refused();
    return kNone;
  }
  return story;
}

}  // namespace

HdrFtrType PageType(const Document& doc, uint32_t section, bool firstOfSection, int32_t pageNumber) {
  if (section < doc.Sections().size() && firstOfSection && doc.Sections()[section].props.titlePage) return HdrFtrType::First;
  if (doc.Settings().evenAndOddHeaders && pageNumber % 2 == 0) return HdrFtrType::Even;
  return HdrFtrType::Default;
}

bool LinkedToPrevious(const Document& doc, uint32_t section, bool footer, HdrFtrType type) {
  return section > 0 && section < doc.Sections().size() && Reference(doc, section, footer, type) == kNoElem;
}

uint32_t EnsureHeaderFooter(Editor& editor, uint32_t section, bool footer, HdrFtrType type, std::u16string& why) {
  if (!CanChange(editor, section, why)) return kNone;
  const Section& sec = editor.Doc().Sections()[section];
  if (const uint32_t s = (footer ? sec.footers : sec.headers)[static_cast<size_t>(type)]; s != kNone) return s;
  return NewOwn(editor, section, footer, type, kNone, why, footer ? u"Footer" : u"Header");
}

Result SetLinkToPrevious(Editor& editor, uint32_t section, bool footer, HdrFtrType type, bool link) {
  std::u16string why;
  if (!CanChange(editor, section, why)) return Fail(why);
  if (section == 0) return Fail(u"The first section has no section before it.");
  const Document& doc = editor.Doc();
  const ElemId ref = Reference(doc, section, footer, type);
  if (link) {
    if (ref == kNoElem) return {};
    TreeEdit te(editor);
    te.Begin(u"Link to Previous");
    te.RemoveElement(ref);
    te.Commit();
    return {};
  }
  if (ref != kNoElem) return {};
  const Section& prev = doc.Sections()[section - 1];
  const uint32_t shown = (footer ? prev.footers : prev.headers)[static_cast<size_t>(type)];
  if (NewOwn(editor, section, footer, type, shown, why, u"Link to Previous") == kNone) return Fail(why);
  return {};
}

bool DifferentFirstPage(const Document& doc, uint32_t section) {
  return section < doc.Sections().size() && doc.Sections()[section].props.titlePage;
}

Result SetDifferentFirstPage(Editor& editor, uint32_t section, bool on) {
  std::u16string why;
  if (!CanChange(editor, section, why)) return Fail(why);
  if (DifferentFirstPage(editor.Doc(), section) == on) return {};
  TreeEdit te(editor);
  te.Begin(u"Different First Page");
  const ElemId sp = SectPr(te, section);
  if (sp == kNoElem) {
    te.Abort();
    return Fail(u"This section can't be changed.");
  }
  if (on) {
    if (ElemId t = te.Child(sp, kTitlePg); t != kNoElem) te.RemoveAttr(t, names::kVal);
  } else {
    te.Remove(sp, kTitlePg);
  }
  te.Commit();
  return {};
}

Result SetDifferentOddEven(Editor& editor, bool on) {
  std::u16string why;
  if (!editor.CanEdit(&why)) return Fail(why);
  if (editor.Doc().Settings().evenAndOddHeaders == on) return {};
  TreeEdit te(editor);
  const ElemId root = te.EnsurePart(PartKind::Settings);
  if (root == kNoElem) return Fail(u"This document's settings can't be changed.");
  te.Begin(u"Different Odd and Even Pages");
  if (on) {
    if (ElemId e = te.Child(root, kEvenOdd); e != kNoElem) te.RemoveAttr(e, names::kVal);
  } else {
    te.Remove(root, kEvenOdd);
  }
  te.Commit();
  return {};
}

Result SetPageNumbering(Editor& editor, uint32_t section, std::string_view format, std::optional<int32_t> start) {
  std::u16string why;
  if (!CanChange(editor, section, why)) return Fail(why);
  static constexpr std::string_view kFormats[] = {"decimal", "lowerRoman", "upperRoman", "lowerLetter", "upperLetter"};
  if (!format.empty() && std::find(std::begin(kFormats), std::end(kFormats), format) == std::end(kFormats))
    return Fail(u"Featherweight doesn't know that number format.");
  if (start && (*start < 0 || *start > 32767)) return Fail(u"Page numbers start at a number from 0 to 32767.");
  TreeEdit te(editor);
  te.Begin(u"Page Numbers");
  const ElemId sp = SectPr(te, section);
  const ElemId pn = sp == kNoElem ? kNoElem : te.Child(sp, kPgNumType);
  if (pn == kNoElem) {
    te.Abort();
    return Fail(u"This section can't be changed.");
  }
  if (format.empty() || format == "decimal") te.RemoveAttr(pn, kFmt);
  else te.SetAttr(pn, kFmt, format);
  if (start) te.SetAttr(pn, kStart, std::to_string(*start));
  else te.RemoveAttr(pn, kStart);
  if (Node(&te.GetTree(), pn).AttrCount() == 0 && te.GetTree().elems[pn].first == kNoElem) te.RemoveElement(pn);
  te.Commit();
  return {};
}

Result InsertPageNumber(Editor& editor, uint32_t section, bool bottom, HdrFtrType type, Justification align,
                        bool ofPages) {
  std::u16string why;
  if (!CanChange(editor, section, why)) return Fail(why);
  const Document& doc = editor.Doc();
  editor.BeginGroup(u"Page Number");
  const uint32_t story = EnsureHeaderFooter(editor, section, bottom, type, why);
  Result r = story == kNone ? Fail(why) : Result{};
  uint32_t para = kNone;
  if (r) {
    const std::vector<uint32_t> paras = doc.StoryParagraphs(story);
    para = paras.empty() ? kNone : paras.back();
    if (para == kNone) {
      r = Fail(u"This header or footer can't hold a page number.");
    } else if (!(paras.size() == 1 && doc.Para(para).text.empty())) {
      r = editor.SplitParagraph({para, static_cast<uint32_t>(doc.Para(para).text.size())});
      para = r.caret.para;
    }
  }
  Pos at{para, 0};
  auto step = [&](Result next) {
    r = std::move(next);
    if (r) at = r.caret;
  };
  if (r && ofPages) step(editor.InsertText(at, u"Page "));
  if (r) step(InsertField(editor, at, kPageField, u"1", u"Page Number"));
  if (r && ofPages) step(editor.InsertText(at, u" of "));
  if (r && ofPages) step(InsertField(editor, at, kPagesField, u"1", u"Page Number"));
  if (r) {
    TreeEdit te(editor, at);
    ParaFormat f;
    f.jc = align;
    FormatParagraphs(te, {{para, 0}, {para, 0}}, f);
  }
  if (!r) {
    editor.CancelGroup();  // what the group did before the refusal goes with it
    return r;
  }
  editor.EndGroup();
  r.caret = at;
  return r;
}

}  // namespace fw::doc::edit
