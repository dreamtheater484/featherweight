#include "TestHarness.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "DocTestUtil.h"
#include "doc/edit/Clip.h"
#include "doc/edit/Editor.h"
#include "doc/layout/Layout.h"
#include "doc/model/Utf.h"
#include "pkg/Check.h"
#include "text/Fonts.h"

using namespace fw::doc;
using namespace doc_test;
using edit::Editor;
using edit::PropEdit;

// Editing (milestone D2): every edit keeps the document valid and writes exactly what it should,
// undo puts back exactly what was there, and edits that would damage the file are refused.

namespace {

std::u16string U(std::string_view s) { return ToUtf16(s); }

// A position (CHECK is a macro, so no braces in its argument).
Pos P(uint32_t para, uint32_t offset) { return Pos{para, offset}; }

std::vector<uint32_t> Main(const Document& doc) { return doc.StoryParagraphs(doc.MainStory()); }

// The main part written now has this body.
bool BodyIs(const Document& doc, std::string_view body) {
  return SameXml(Canon(DocumentXml(body)), Canon(MainXml(doc)), "body");
}

// The model agrees with what it writes: reading the written main part back gives the same
// paragraphs, with the same items and runs where they were; each w:p knows its index and length.
bool Consistent(const Document& doc, const Fixture& f) {
  bool ok = true;
  for (uint32_t p = 0; p < doc.Paragraphs().size(); ++p) {
    Node n = doc.ParaNode(p);
    if (n.Raw().start != p || n.TextLength() != doc.Para(p).text.size()) {
      std::printf("  paragraph %u: its w:p says %u, length %u (text %zu)\n", p, n.Raw().start, n.TextLength(),
                  doc.Para(p).text.size());
      ok = false;
    }
  }
  Fixture g = f;
  g.body.clear();
  g.documentXml = MainXml(doc);
  auto back = LoadOk(g);
  if (!back) return false;
  auto a = Main(doc), b = Main(*back);
  if (a.size() != b.size()) {
    std::printf("  %zu paragraphs, %zu read back\n", a.size(), b.size());
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (doc.Para(a[i]).text != back->Para(b[i]).text) {
      std::printf("  paragraph %zu: text \"%s\", read back \"%s\"\n", i, ToUtf8(doc.Para(a[i]).text).c_str(),
                  ToUtf8(back->Para(b[i]).text).c_str());
      ok = false;
      continue;
    }
    auto x = doc.Items(a[i]), y = back->Items(b[i]);
    bool same = x.size() == y.size();
    for (size_t k = 0; same && k < x.size(); ++k)
      same = x[k].kind == y[k].kind && x[k].offset == y[k].offset && x[k].length == y[k].length;
    auto r = doc.Runs(a[i]), s = back->Runs(b[i]);
    same = same && r.size() == s.size();
    for (size_t k = 0; same && k < r.size(); ++k) same = r[k].start == s[k].start && r[k].end == s[k].end;
    if (!same) {
      std::printf("  paragraph %zu: items or runs differ from the ones read back\n", i);
      ok = false;
    }
  }
  return ok;
}

uint32_t UndoSteps(Editor& ed) {
  uint32_t n = 0;
  while (ed.CanUndo() && ed.Undo()) ++n;
  return n;
}

}  // namespace

TEST(Edit_TypingGoesIntoTheText) {
  Fixture f;
  f.body = "<w:p><w:r><w:t>Hello world</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  CHECK(ed.CanEdit());
  uint32_t p = Main(*doc)[0];
  auto r = ed.InsertText(P(p, 5), u", dear");
  CHECK(r && r.caret == P(p, 11));
  CHECK(doc->Para(p).text == u"Hello, dear world");
  CHECK(BodyIs(*doc, "<w:p><w:r><w:t>Hello, dear world</w:t></w:r></w:p>"));
  CHECK(Consistent(*doc, f));
  CHECK(ed.Modified() && ed.CanUndo() && ed.UndoName() == u"Typing");
  auto u = ed.Undo();
  CHECK(u && u.caret == P(p, 5));
  CHECK(RoundTrips(*doc) && !ed.Modified() && ed.CanRedo());
  CHECK(ed.Redo() && BodyIs(*doc, "<w:p><w:r><w:t>Hello, dear world</w:t></w:r></w:p>"));
  // A new edit drops what could be redone.
  CHECK(ed.Undo() && ed.InsertText(P(p, 0), u"x") && !ed.CanRedo());
}

TEST(Edit_SpacesTabsAndBreaks) {
  Fixture f;
  f.body = "<w:p><w:r><w:t>Hello</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  uint32_t p = Main(*doc)[0];
  CHECK(ed.InsertText(P(p, 5), u" "));
  CHECK(BodyIs(*doc, "<w:p><w:r><w:t xml:space=\"preserve\">Hello </w:t></w:r></w:p>"));
  ed.BreakTyping();
  CHECK(ed.InsertText(P(p, 2), u"a\tb\vc"));
  CHECK(doc->Para(p).text == u"Hea\tb\vcllo ");
  CHECK(BodyIs(*doc,
               "<w:p><w:r><w:t xml:space=\"preserve\">Hea</w:t><w:tab/><w:t>b</w:t><w:br/>"
               "<w:t xml:space=\"preserve\">cllo </w:t></w:r></w:p>"));
  CHECK(Consistent(*doc, f));
  // Characters that stand for things in the model aren't typed as text.
  CHECK(ed.InsertText(P(p, 0), U("\x13\x15\x02\x05") + u"￼") && doc->Para(p).text == u"Hea\tb\vcllo ");
  CHECK(UndoSteps(ed) == 2 && RoundTrips(*doc));
}

TEST(Edit_TypingAndDeletingCoalesce) {
  Fixture f;
  f.body = "<w:p><w:r><w:t>x</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  uint32_t p = Main(*doc)[0];
  CHECK(ed.InsertText(P(p, 1), u"a") && ed.InsertText(P(p, 2), u"b") && ed.InsertText(P(p, 3), u"c"));
  CHECK(doc->Para(p).text == u"xabc");
  CHECK(ed.Undo() && doc->Para(p).text == u"x" && !ed.CanUndo());
  CHECK(ed.Redo() && doc->Para(p).text == u"xabc");
  // A break (the caret moved, time passed) starts a new step; so does typing elsewhere.
  ed.BreakTyping();
  CHECK(ed.InsertText(P(p, 4), u"d") && ed.InsertText(P(p, 0), u"_"));
  CHECK(doc->Para(p).text == u"_xabcd");
  CHECK(ed.Undo() && doc->Para(p).text == u"xabcd" && ed.Undo() && doc->Para(p).text == u"xabc");
  // Backspace, Backspace: one step. Delete, Delete: one step.
  CHECK(ed.Delete(P(p, 3), P(p, 4)) && ed.Delete(P(p, 2), P(p, 3)) && doc->Para(p).text == u"xa");
  CHECK(ed.Undo() && doc->Para(p).text == u"xabc");
  CHECK(ed.Delete(P(p, 0), P(p, 1)) && ed.Delete(P(p, 0), P(p, 1)) && doc->Para(p).text == u"bc");
  CHECK(ed.Undo() && doc->Para(p).text == u"xabc");
  // A group is one step, named by its outermost name.
  ed.BeginGroup(u"Paste");
  ed.BeginGroup(u"Inner");
  CHECK(ed.InsertText(P(p, 0), u"1") && ed.InsertText(P(p, 0), u"2"));
  ed.EndGroup();
  CHECK(ed.SplitParagraph(P(p, 1)));
  ed.EndGroup();
  CHECK(ed.UndoName() == u"Paste" && Main(*doc).size() == 2);
  CHECK(ed.Undo() && Main(*doc).size() == 1 && doc->Para(p).text == u"xabc");
  // Typing over a selection is one step.
  CHECK(ed.Replace(P(p, 1), P(p, 3), u"Z") && doc->Para(p).text == u"xZc");
  CHECK(ed.Undo() && doc->Para(p).text == u"xabc");
  CHECK(UndoSteps(ed) == 1 && RoundTrips(*doc));
}

TEST(Edit_NewTextTakesTheFormattingAround) {
  Fixture f;
  f.body =
      "<w:p><w:pPr><w:rPr><w:ins w:id=\"7\" w:author=\"A\" w:date=\"2026-01-01T00:00:00Z\"/><w:b/></w:rPr></w:pPr></w:p>"
      "<w:p><w:hyperlink r:id=\"rIdLink\"><w:r><w:rPr><w:rStyle w:val=\"Hyperlink\"/><w:i/></w:rPr><w:t>link</w:t>"
      "</w:r></w:hyperlink></w:p>"
      "<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>bold</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  auto ps = Main(*doc);
  // An empty paragraph: its mark's formatting, without the mark's revision.
  CHECK(ed.InsertText(P(ps[0], 0), u"x"));
  // After a link: outside it, without the link's style.
  CHECK(ed.InsertText(P(ps[1], 4), u"s"));
  // At the start: the first run's.
  CHECK(ed.InsertText(P(ps[2], 0), u"B"));
  CHECK(BodyIs(*doc,
               "<w:p><w:pPr><w:rPr><w:ins w:id=\"7\" w:author=\"A\" w:date=\"2026-01-01T00:00:00Z\"/><w:b/></w:rPr>"
               "</w:pPr><w:r><w:rPr><w:b/></w:rPr><w:t>x</w:t></w:r></w:p>"
               "<w:p><w:hyperlink r:id=\"rIdLink\"><w:r><w:rPr><w:rStyle w:val=\"Hyperlink\"/><w:i/></w:rPr>"
               "<w:t>link</w:t></w:r></w:hyperlink><w:r><w:rPr><w:i/></w:rPr><w:t>s</w:t></w:r></w:p>"
               "<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>Bbold</w:t></w:r></w:p>"));
  CHECK(Consistent(*doc, f));
  CHECK(UndoSteps(ed) == 3 && RoundTrips(*doc));
}

TEST(Edit_SplitAndJoinParagraphs) {
  Fixture f;
  f.body =
      "<w:p w14:paraId=\"11111111\" w14:textId=\"22222222\"><w:pPr><w:jc w:val=\"center\"/></w:pPr>"
      "<w:r><w:rPr><w:b/></w:rPr><w:t>HelloWorld</w:t></w:r></w:p>"
      "<w:p><w:r><w:t>Next</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  uint32_t p = Main(*doc)[0];
  std::vector<edit::Change> changes;
  ed.OnChange([&](const edit::Change& c) { changes.push_back(c); });
  auto r = ed.SplitParagraph(P(p, 5));
  CHECK(r && r.caret == P(p + 1, 0));
  CHECK(Main(*doc).size() == 3 && doc->Para(p).text == u"Hello" && doc->Para(p + 1).text == u"World");
  // Plain text: no list, field or section reads differently after it (Edit_OnlyListsFieldsAndSectionsAreStructure).
  CHECK(changes.size() == 1 && changes[0].first == p && changes[0].removed == 1 && changes[0].added == 2 &&
        !changes[0].structure);
  // The second half has new paragraph ids, the same properties and formatting.
  std::string_view id1 = doc->W14ParaId(p), id2 = doc->W14ParaId(p + 1);
  CHECK(id1 == "11111111" && id2.size() == 8 && id2 != id1);
  std::string xml = MainXml(*doc);
  std::string second = "<w:p w14:paraId=\"" + std::string(id2) + "\" w14:textId=\"";
  CHECK(xml.find(second) != std::string::npos);
  CHECK(Consistent(*doc, f));
  // Joined, the text is one paragraph again (in two runs).
  CHECK(ed.JoinParagraphs(p) && Main(*doc).size() == 2 && doc->Para(p).text == u"HelloWorld");
  CHECK(changes.size() == 2 && changes[1].removed == 2 && changes[1].added == 1);
  CHECK(BodyIs(*doc,
               "<w:p w14:paraId=\"11111111\" w14:textId=\"22222222\"><w:pPr><w:jc w:val=\"center\"/></w:pPr>"
               "<w:r><w:rPr><w:b/></w:rPr><w:t>Hello</w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>World</w:t></w:r></w:p>"
               "<w:p><w:r><w:t>Next</w:t></w:r></w:p>"));
  CHECK(Consistent(*doc, f));
  // Undo tells the listeners too.
  CHECK(ed.Undo() && Main(*doc).size() == 3 && changes.size() == 3 && changes[2].removed == 1 && changes[2].added == 2);
  CHECK(ed.Undo() && RoundTrips(*doc));
  // Enter at the end: an empty paragraph after, with the same properties; at the start: one before.
  CHECK(ed.SplitParagraph(P(p, 10)) && Main(*doc).size() == 3 && doc->Para(p + 1).text.empty());
  CHECK(Consistent(*doc, f));
  CHECK(ed.Undo() && ed.SplitParagraph(P(p, 0)) && doc->Para(p).text.empty() && doc->Para(p + 1).text == u"HelloWorld");
  CHECK(Consistent(*doc, f));
  // Delete at the end of a paragraph joins it with the next.
  CHECK(ed.Undo() && ed.Delete(P(p, 10), P(p + 1, 0)) && doc->Para(p).text == u"HelloWorldNext");
  CHECK(Consistent(*doc, f));
  CHECK(ed.Undo() && RoundTrips(*doc));
}

TEST(Edit_SectionBreaksStayWithTheirMark) {
  Fixture f;
  f.body =
      "<w:p><w:pPr><w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/></w:sectPr></w:pPr><w:r><w:t>AB</w:t></w:r></w:p>"
      "<w:p><w:r><w:t>C</w:t></w:r></w:p>"
      "<w:sectPr><w:pgSz w:w=\"15840\" w:h=\"12240\" w:orient=\"landscape\"/></w:sectPr>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  uint32_t p = Main(*doc)[0];
  CHECK(doc->Sections().size() == 2 && doc->Sections()[0].endPara == p);
  CHECK(ed.SplitParagraph(P(p, 1)));
  CHECK(doc->Sections()[0].endPara == p + 1);
  const char* split =
      "<w:p><w:r><w:t>A</w:t></w:r></w:p>"
      "<w:p><w:pPr><w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/></w:sectPr></w:pPr><w:r><w:t>B</w:t></w:r></w:p>"
      "<w:p><w:r><w:t>C</w:t></w:r></w:p>"
      "<w:sectPr><w:pgSz w:w=\"15840\" w:h=\"12240\" w:orient=\"landscape\"/></w:sectPr>";
  CHECK(BodyIs(*doc, split));
  // The section break itself can't be deleted yet.
  auto j = ed.JoinParagraphs(p + 1);
  CHECK(!j && !j.message.empty());
  CHECK(!ed.Delete(P(p + 1, 1), P(p + 2, 0)) && !ed.Delete(P(p, 0), P(p + 2, 1)));
  CHECK(BodyIs(*doc, split) && ed.UndoName() == u"New paragraph");
  // Joining the paragraph before it with it keeps the break, on the joined paragraph.
  CHECK(ed.Delete(P(p, 1), P(p + 1, 0)) && doc->Sections()[0].endPara == p);
  CHECK(BodyIs(*doc,
               "<w:p><w:pPr><w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/></w:sectPr></w:pPr>"
               "<w:r><w:t>A</w:t></w:r><w:r><w:t>B</w:t></w:r></w:p>"
               "<w:p><w:r><w:t>C</w:t></w:r></w:p>"
               "<w:sectPr><w:pgSz w:w=\"15840\" w:h=\"12240\" w:orient=\"landscape\"/></w:sectPr>"));
  CHECK(Consistent(*doc, f));
  CHECK(UndoSteps(ed) == 2 && RoundTrips(*doc) && doc->Sections()[0].endPara == p);
}

TEST(Edit_DeleteAcrossParagraphs) {
  Fixture f;
  f.body =
      "<w:p><w:r><w:t>Alpha</w:t></w:r></w:p><w:p><w:r><w:t>Beta</w:t></w:r></w:p>"
      "<w:p><w:pPr><w:jc w:val=\"right\"/></w:pPr><w:r><w:t>Gamma</w:t></w:r></w:p><w:p><w:r><w:t>Delta</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  uint32_t p = Main(*doc)[0];
  auto r = ed.Delete(P(p + 2, 3), P(p, 2));  // either order
  CHECK(r && r.caret == P(p, 2));
  CHECK(Main(*doc).size() == 2 && doc->Para(p).text == u"Alma" && doc->Para(p + 1).text == u"Delta");
  CHECK(BodyIs(*doc,
               "<w:p><w:r><w:t>Al</w:t></w:r><w:r><w:t>ma</w:t></w:r></w:p><w:p><w:r><w:t>Delta</w:t></w:r></w:p>"));
  CHECK(Consistent(*doc, f));
  CHECK(ed.Undo() && RoundTrips(*doc) && ed.Redo() && doc->Para(p).text == u"Alma");
  CHECK(ed.Undo() && ed.Replace(P(p, 5), P(p + 3, 0), u"!") && doc->Para(p).text == u"Alpha!Delta");
  CHECK(Consistent(*doc, f));
  CHECK(ed.Undo() && RoundTrips(*doc));
}

TEST(Edit_BookmarksKeepBothEnds) {
  Fixture f;
  f.body =
      "<w:p><w:r><w:t>ab</w:t></w:r><w:bookmarkStart w:id=\"0\" w:name=\"m\"/><w:r><w:t>cd</w:t></w:r>"
      "<w:bookmarkEnd w:id=\"0\"/><w:r><w:t>ef</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  uint32_t p = Main(*doc)[0];
  auto mark = [&]() -> const PairInfo* {
    for (const PairInfo& pi : doc->Pairs())
      if (pi.id == "0") return &pi;
    return nullptr;
  };
  CHECK(mark() && mark()->openPos == P(p, 2) && mark()->closePos == P(p, 4));
  // Deleting what it holds leaves it empty, both ends in place.
  CHECK(ed.Delete(P(p, 1), P(p, 5)));
  CHECK(BodyIs(*doc,
               "<w:p><w:r><w:t>a</w:t></w:r><w:bookmarkStart w:id=\"0\" w:name=\"m\"/><w:bookmarkEnd w:id=\"0\"/>"
               "<w:r><w:t>f</w:t></w:r></w:p>"));
  CHECK(mark()->openPos == P(p, 1) && mark()->closePos == P(p, 1));
  CHECK(Consistent(*doc, f));
  CHECK(ed.Undo() && RoundTrips(*doc) && mark()->closePos == P(p, 4));
  // Split inside it: it spans the two paragraphs.
  CHECK(ed.SplitParagraph(P(p, 3)));
  CHECK(BodyIs(*doc,
               "<w:p><w:r><w:t>ab</w:t></w:r><w:bookmarkStart w:id=\"0\" w:name=\"m\"/><w:r><w:t>c</w:t></w:r></w:p>"
               "<w:p><w:r><w:t>d</w:t></w:r><w:bookmarkEnd w:id=\"0\"/><w:r><w:t>ef</w:t></w:r></w:p>"));
  CHECK(mark()->openPos == P(p, 2) && mark()->closePos == P(p + 1, 1));
  CHECK(Consistent(*doc, f));
  // Split at its start: the start goes with the text after it.
  CHECK(ed.Undo() && ed.SplitParagraph(P(p, 2)));
  CHECK(BodyIs(*doc,
               "<w:p><w:r><w:t>ab</w:t></w:r></w:p><w:p><w:bookmarkStart w:id=\"0\" w:name=\"m\"/><w:r><w:t>cd</w:t></w:r>"
               "<w:bookmarkEnd w:id=\"0\"/><w:r><w:t>ef</w:t></w:r></w:p>"));
  CHECK(mark()->openPos == P(p + 1, 0));
  CHECK(ed.Undo() && RoundTrips(*doc));
}

TEST(Edit_FieldsAreDeletedWhole) {
  Fixture f;
  f.body =
      "<w:p><w:r><w:t xml:space=\"preserve\">Page </w:t></w:r><w:r><w:fldChar w:fldCharType=\"begin\"/></w:r>"
      "<w:r><w:instrText xml:space=\"preserve\"> PAGE </w:instrText></w:r><w:r><w:fldChar w:fldCharType=\"separate\"/>"
      "</w:r><w:r><w:t>3</w:t></w:r><w:r><w:fldChar w:fldCharType=\"end\"/></w:r>"
      "<w:r><w:t xml:space=\"preserve\"> of</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  uint32_t p = Main(*doc)[0];
  CHECK(doc->Para(p).text == U("Page \x13 PAGE \x14") + u"3\x15 of");
  CHECK(doc->Fields(doc->MainStory()).size() == 1);
  // Across its start, or its separator alone: refused, nothing changes.
  auto r = ed.Delete(P(p, 3), P(p, 7));
  CHECK(!r && r.message == u"Select the whole field to delete it.");
  CHECK(!ed.Delete(P(p, 12), P(p, 13)) && !ed.Delete(P(p, 14), P(p, 16)));
  CHECK(RoundTrips(*doc) && !ed.CanUndo() && !ed.Modified());
  // Its result can be edited.
  CHECK(ed.InsertText(P(p, 14), u"4") && doc->Para(p).text == U("Page \x13 PAGE \x14") + u"34\x15 of");
  CHECK(Consistent(*doc, f));
  CHECK(ed.Undo() && ed.Delete(P(p, 13), P(p, 14)) && Consistent(*doc, f));
  CHECK(doc->Fields(doc->MainStory()).at(0).result.empty());
  // The whole field goes.
  CHECK(ed.Undo() && ed.Delete(P(p, 5), P(p, 15)));
  CHECK(BodyIs(*doc,
               "<w:p><w:r><w:t xml:space=\"preserve\">Page </w:t></w:r><w:r><w:t xml:space=\"preserve\"> of</w:t></w:r></w:p>"));
  CHECK(doc->Fields(doc->MainStory()).empty() && Consistent(*doc, f));
  CHECK(ed.Undo() && RoundTrips(*doc) && doc->Fields(doc->MainStory()).size() == 1);
}

TEST(Edit_SimpleFieldsAndContentControls) {
  Fixture f;
  f.body =
      "<w:p><w:r><w:t>a</w:t></w:r><w:fldSimple w:instr=\" NUMPAGES \"><w:r><w:t>9</w:t></w:r></w:fldSimple>"
      "<w:sdt><w:sdtPr><w:id w:val=\"5\"/></w:sdtPr><w:sdtContent><w:r><w:t>cc</w:t></w:r></w:sdtContent></w:sdt>"
      "<w:sdt><w:sdtPr><w:lock w:val=\"sdtLocked\"/></w:sdtPr><w:sdtContent><w:r><w:t>dd</w:t></w:r></w:sdtContent></w:sdt>"
      "<w:sdt><w:sdtPr><w:lock w:val=\"contentLocked\"/></w:sdtPr><w:sdtContent><w:r><w:t>ee</w:t></w:r></w:sdtContent>"
      "</w:sdt></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  uint32_t p = Main(*doc)[0];
  CHECK(doc->Para(p).text == u"a9ccddee");
  // Across an edge: refused.
  CHECK(!ed.Delete(P(p, 1), P(p, 3)) && !ed.Delete(P(p, 3), P(p, 5)) && !ed.SplitParagraph(P(p, 3)));
  CHECK(!ed.Delete(P(p, 6), P(p, 7)) && !ed.InsertText(P(p, 7), u"x"));  // locked text
  CHECK(RoundTrips(*doc));
  // Inside: fine, also in a control that can't be deleted.
  CHECK(ed.Delete(P(p, 2), P(p, 3)) && ed.InsertText(P(p, 3), u"D") && doc->Para(p).text == u"a9cDddee");
  CHECK(Consistent(*doc, f));
  CHECK(UndoSteps(ed) == 2 && RoundTrips(*doc));
  // All of a control's text: its text goes, the control stays.
  CHECK(ed.Delete(P(p, 2), P(p, 4)));
  CHECK(BodyIs(*doc,
               "<w:p><w:r><w:t>a</w:t></w:r><w:fldSimple w:instr=\" NUMPAGES \"><w:r><w:t>9</w:t></w:r></w:fldSimple>"
               "<w:sdt><w:sdtPr><w:id w:val=\"5\"/></w:sdtPr><w:sdtContent></w:sdtContent></w:sdt>"
               "<w:sdt><w:sdtPr><w:lock w:val=\"sdtLocked\"/></w:sdtPr><w:sdtContent><w:r><w:t>dd</w:t></w:r></w:sdtContent></w:sdt>"
               "<w:sdt><w:sdtPr><w:lock w:val=\"contentLocked\"/></w:sdtPr><w:sdtContent><w:r><w:t>ee</w:t></w:r></w:sdtContent>"
               "</w:sdt></w:p>"));
  CHECK(Consistent(*doc, f));
  // More than it: the field and the control go whole; a locked one can't.
  CHECK(ed.Undo() && ed.Delete(P(p, 0), P(p, 4)));
  CHECK(BodyIs(*doc,
               "<w:p><w:sdt><w:sdtPr><w:lock w:val=\"sdtLocked\"/></w:sdtPr><w:sdtContent><w:r><w:t>dd</w:t></w:r>"
               "</w:sdtContent></w:sdt><w:sdt><w:sdtPr><w:lock w:val=\"contentLocked\"/></w:sdtPr><w:sdtContent>"
               "<w:r><w:t>ee</w:t></w:r></w:sdtContent></w:sdt></w:p>"));
  CHECK(Consistent(*doc, f));
  auto locked = ed.Delete(P(p, 0), P(p, 4));
  CHECK(!locked && locked.message == u"This content control is locked, so it can't be deleted.");
  // Typing after a locked control's text goes after the control.
  CHECK(ed.InsertText(P(p, 4), u"!") && doc->Para(p).text == u"ddee!");
  CHECK(Consistent(*doc, f));
  CHECK(UndoSteps(ed) == 2 && RoundTrips(*doc));
}

TEST(Edit_CommentsAndNotesKeepTheirMarks) {
  Fixture f;
  f.body =
      "<w:p><w:commentRangeStart w:id=\"0\"/><w:r><w:t>abc</w:t></w:r><w:commentRangeEnd w:id=\"0\"/>"
      "<w:r><w:commentReference w:id=\"0\"/></w:r><w:r><w:t>de</w:t></w:r><w:r><w:footnoteReference w:id=\"1\"/></w:r></w:p>";
  f.comments = "<w:comment w:id=\"0\" w:author=\"A\"><w:p><w:r><w:annotationRef/><w:t>note</w:t></w:r></w:p></w:comment>";
  f.footnotes =
      "<w:footnote w:type=\"separator\" w:id=\"-1\"><w:p><w:r><w:separator/></w:r></w:p></w:footnote>"
      "<w:footnote w:id=\"1\"><w:p><w:r><w:footnoteRef/><w:t>Note</w:t></w:r></w:p></w:footnote>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  uint32_t p = Main(*doc)[0];
  CHECK(doc->Para(p).text == u"abc\x05" u"de\x02");
  // The commented text can go; the range stays, empty.
  CHECK(ed.Delete(P(p, 0), P(p, 3)));
  CHECK(BodyIs(*doc,
               "<w:p><w:commentRangeStart w:id=\"0\"/><w:commentRangeEnd w:id=\"0\"/><w:r><w:commentReference w:id=\"0\"/>"
               "</w:r><w:r><w:t>de</w:t></w:r><w:r><w:footnoteReference w:id=\"1\"/></w:r></w:p>"));
  CHECK(ed.Undo() && RoundTrips(*doc));
  // The marks of comments and notes can't be deleted yet.
  auto c = ed.Delete(P(p, 2), P(p, 5));
  CHECK(!c && c.message.find(u"comment") != std::u16string::npos);
  auto n = ed.Delete(P(p, 5), P(p, 7));
  CHECK(!n && n.message.find(u"footnote") != std::u16string::npos);
  // Notes and comments are edited like the body; the note keeps its number.
  uint32_t note = doc->StoryParagraphs(doc->NoteStory(StoryKind::Footnote, "1")).at(0);
  CHECK(!ed.Delete(P(note, 0), P(note, 2)) && ed.InsertText(P(note, 5), u"s") && doc->Para(note).text == u"\x02Notes");
  CHECK(!ed.Delete(P(p, 0), P(note, 1)));  // across stories
  auto dirty = ed.DirtyParts();
  CHECK(dirty.size() == 1 && doc->Parts()[dirty[0]].name == "/word/footnotes.xml");
  CHECK(ed.Undo() && RoundTrips(*doc));
}

TEST(Edit_TrackedChanges) {
  Fixture f;
  f.body =
      "<w:p><w:ins w:id=\"1\" w:author=\"A\" w:date=\"2026-01-01T00:00:00Z\"><w:r><w:t>new</w:t></w:r></w:ins>"
      "<w:del w:id=\"2\" w:author=\"A\" w:date=\"2026-01-01T00:00:00Z\"><w:r><w:delText>old</w:delText></w:r></w:del></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  uint32_t p = Main(*doc)[0];
  CHECK(doc->Para(p).text == u"newold");
  // Split inside an insertion: two insertions, the second with a new id.
  CHECK(ed.SplitParagraph(P(p, 1)));
  CHECK(BodyIs(*doc,
               "<w:p><w:ins w:id=\"1\" w:author=\"A\" w:date=\"2026-01-01T00:00:00Z\"><w:r><w:t>n</w:t></w:r></w:ins></w:p>"
               "<w:p><w:ins w:id=\"3\" w:author=\"A\" w:date=\"2026-01-01T00:00:00Z\"><w:r><w:t>ew</w:t></w:r></w:ins>"
               "<w:del w:id=\"2\" w:author=\"A\" w:date=\"2026-01-01T00:00:00Z\"><w:r><w:delText>old</w:delText></w:r></w:del></w:p>"));
  CHECK(Consistent(*doc, f));
  CHECK(ed.Undo() && RoundTrips(*doc));
  // Text typed after deleted text isn't deleted.
  CHECK(ed.InsertText(P(p, 6), u"x"));
  CHECK(BodyIs(*doc,
               "<w:p><w:ins w:id=\"1\" w:author=\"A\" w:date=\"2026-01-01T00:00:00Z\"><w:r><w:t>new</w:t></w:r></w:ins>"
               "<w:del w:id=\"2\" w:author=\"A\" w:date=\"2026-01-01T00:00:00Z\"><w:r><w:delText>old</w:delText></w:r></w:del>"
               "<w:r><w:t>x</w:t></w:r></w:p>"));
  // Typed inside deleted text, it goes between the two halves of the deletion (ids are never reused).
  CHECK(ed.Undo() && ed.InsertText(P(p, 4), u"x") && doc->Para(p).text == u"newoxld");
  CHECK(BodyIs(*doc,
               "<w:p><w:ins w:id=\"1\" w:author=\"A\" w:date=\"2026-01-01T00:00:00Z\"><w:r><w:t>new</w:t></w:r></w:ins>"
               "<w:del w:id=\"2\" w:author=\"A\" w:date=\"2026-01-01T00:00:00Z\"><w:r><w:delText>o</w:delText></w:r></w:del>"
               "<w:r><w:t>x</w:t></w:r>"
               "<w:del w:id=\"4\" w:author=\"A\" w:date=\"2026-01-01T00:00:00Z\"><w:r><w:delText>ld</w:delText></w:r></w:del>"
               "</w:p>"));
  CHECK(Consistent(*doc, f));
  // Deleting all of an insertion removes it.
  CHECK(ed.Undo() && ed.Delete(P(p, 0), P(p, 3)));
  CHECK(BodyIs(*doc,
               "<w:p><w:del w:id=\"2\" w:author=\"A\" w:date=\"2026-01-01T00:00:00Z\"><w:r><w:delText>old</w:delText>"
               "</w:r></w:del></w:p>"));
  CHECK(Consistent(*doc, f));
  CHECK(ed.Undo() && RoundTrips(*doc));
}

TEST(Edit_UnknownMarkupStays) {
  Fixture f;
  f.body = "<w:p x:p=\"1\"><w:r x:a=\"1\"><x:unknown/><w:t>abcd</w:t></w:r><x:after/></w:p><w:p><w:r><w:t>z</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  uint32_t p = Main(*doc)[0];
  CHECK(ed.SplitParagraph(P(p, 2)));
  std::string xml = MainXml(*doc);
  CHECK(xml.find("<x:unknown") != std::string::npos && xml.find("<x:after") != std::string::npos);
  CHECK(Consistent(*doc, f));
  CHECK(ed.Undo() && RoundTrips(*doc));
  // Deleting all the text keeps the run that holds unknown markup.
  CHECK(ed.Delete(P(p, 0), P(p, 4)));
  xml = MainXml(*doc);
  CHECK(xml.find("<x:unknown") != std::string::npos && xml.find("x:a=\"1\"") != std::string::npos);
  CHECK(Consistent(*doc, f));
  // Joined with the next one, its unknown markup is still there.
  CHECK(ed.JoinParagraphs(p));
  xml = MainXml(*doc);
  CHECK(xml.find("<x:after") != std::string::npos && xml.find("x:p=\"1\"") != std::string::npos);
  CHECK(Consistent(*doc, f));
  CHECK(UndoSteps(ed) == 2 && RoundTrips(*doc));
}

TEST(Edit_FormattingIsATransaction) {
  Fixture f;
  f.body = "<w:p><w:r><w:t>abcd</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  uint32_t p = Main(*doc)[0];
  CHECK(ed.SetRunProps(P(p, 1), P(p, 3), PropEdit().Set("b")));
  CHECK(BodyIs(*doc,
               "<w:p><w:r><w:t>a</w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>bc</w:t></w:r><w:r><w:t>d</w:t></w:r></w:p>"));
  CHECK((ed.SetRunProps(P(p, 1), P(p, 3), PropEdit().Set("sz", {{"val", "28"}}).Set("i"))));
  CHECK(ed.SetRunProps(P(p, 1), P(p, 3), PropEdit().Remove("b")));
  CHECK((ed.SetParaProps(p, p, PropEdit().Set("jc", {{"val", "center"}}))));
  CHECK((ed.SetParaProps(p, p, PropEdit().Set("numPr/numId", {{"val", "1"}}).Set("numPr/ilvl", {{"val", "0"}}))));
  CHECK(ed.SetMarkProps(p, p, PropEdit().Set("i")));
  CHECK(BodyIs(*doc,
               "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"1\"/></w:numPr><w:jc w:val=\"center\"/>"
               "<w:rPr><w:i/></w:rPr></w:pPr><w:r><w:t>a</w:t></w:r><w:r><w:rPr><w:i/><w:sz w:val=\"28\"/></w:rPr>"
               "<w:t>bc</w:t></w:r><w:r><w:t>d</w:t></w:r></w:p>"));
  CHECK(doc->ResolvedPara(p).jc == Justification::Center);
  CHECK(Consistent(*doc, f));
  // What the schema doesn't know is refused, and nothing changes.
  auto bad = ed.SetRunProps(P(p, 0), P(p, 4), PropEdit().Set("bogus"));
  CHECK(!bad && !bad.message.empty());
  CHECK(ed.UndoName() == u"Format" && UndoSteps(ed) == 6 && RoundTrips(*doc));
}

TEST(Edit_TablesAndStoriesBoundEdits) {
  Fixture f;
  f.body =
      "<w:p><w:r><w:t>before</w:t></w:r></w:p><w:tbl><w:tblGrid><w:gridCol w:w=\"2000\"/></w:tblGrid><w:tr><w:tc>"
      "<w:p><w:r><w:t>cell</w:t></w:r></w:p></w:tc></w:tr></w:tbl><w:p><w:r><w:t>after</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  auto ps = Main(*doc);
  CHECK(ps.size() == 3);
  CHECK(!ed.Delete(P(ps[0], 3), P(ps[2], 2)) && !ed.JoinParagraphs(ps[0]) && !ed.JoinParagraphs(ps[1]));
  CHECK(!ed.Delete(P(ps[1], 4), P(ps[2], 0)));
  CHECK(RoundTrips(*doc));
  CHECK(ed.InsertText(P(ps[1], 4), u"s") && ed.SplitParagraph(P(ps[1], 2)));
  CHECK(Main(*doc).size() == 4 && doc->Para(ps[1]).text == u"ce" && doc->Para(ps[1] + 1).text == u"lls");
  CHECK(BodyIs(*doc,
               "<w:p><w:r><w:t>before</w:t></w:r></w:p><w:tbl><w:tblGrid><w:gridCol w:w=\"2000\"/></w:tblGrid><w:tr><w:tc>"
               "<w:p><w:r><w:t>ce</w:t></w:r></w:p><w:p><w:r><w:t>lls</w:t></w:r></w:p></w:tc></w:tr></w:tbl>"
               "<w:p><w:r><w:t>after</w:t></w:r></w:p>"));
  CHECK(Consistent(*doc, f));
  CHECK(ed.NextPara(ps[0]) == ps[1] && ed.PrevPara(ps[0]) == kNone && ed.Compare(P(ps[0], 9), P(ps[1], 0)) < 0);
  CHECK(UndoSteps(ed) == 2 && RoundTrips(*doc));
}

TEST(Edit_InsertAndDeleteAnywhereLeavesTheFileExact) {
  // Typing a character and deleting it again, at every place in a document full of structure,
  // leaves the file as it was: the editor changes only what it must.
  Fixture f;
  f.body =
      "<w:bookmarkStart w:id=\"10\" w:name=\"top\"/>"
      "<w:p w14:paraId=\"1A2B3C4D\" w:rsidR=\"00AB12CD\" x:mark=\"p\"><w:pPr><w:pStyle w:val=\"Heading1\"/><x:pprExt/>"
      "<w:spacing w:before=\"120\" w:after=\"0\"/></w:pPr><w:r w:rsidR=\"00AB12CD\"><w:rPr><w:b/><x:rprExt/></w:rPr>"
      "<w:t xml:space=\"preserve\">Bold </w:t></w:r><w:proofErr w:type=\"spellStart\"/><w:r><w:t>speling</w:t></w:r>"
      "<w:proofErr w:type=\"spellEnd\"/><w:bookmarkEnd w:id=\"10\"/>"
      "<w:hyperlink r:id=\"rIdLink\" w:history=\"1\"><w:r><w:rPr><w:rStyle w:val=\"Hyperlink\"/></w:rPr><w:t>link</w:t>"
      "</w:r></w:hyperlink><w:r><w:tab/><w:t>tab</w:t><w:br/><w:t>br</w:t></w:r><x:inline/>"
      "<w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText xml:space=\"preserve\"> PAGE </w:instrText></w:r>"
      "<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r><w:r><w:t>1</w:t></w:r><w:r><w:fldChar w:fldCharType=\"end\"/></w:r>"
      "</w:p>"
      "<w:p><w:ins w:id=\"1\" w:author=\"A\" w:date=\"2026-01-01T00:00:00Z\"><w:r><w:t>in</w:t></w:r></w:ins>"
      "<w:del w:id=\"2\" w:author=\"A\" w:date=\"2026-01-01T00:00:00Z\"><w:r><w:delText>out</w:delText></w:r></w:del>"
      "<w:smartTag w:uri=\"u\" w:element=\"e\"><w:r><w:t>tag</w:t></w:r></w:smartTag>"
      "<w:fldSimple w:instr=\" NUMPAGES \"><w:r><w:t>9</w:t></w:r></w:fldSimple>"
      "<w:sdt><w:sdtPr><w:id w:val=\"5\"/></w:sdtPr><w:sdtContent><w:r><w:t>cc</w:t></w:r></w:sdtContent></w:sdt></w:p>"
      "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"2000\"/></w:tblGrid>"
      "<w:tr><w:tc><w:p><w:r><w:t>cell</w:t></w:r></w:p></w:tc></w:tr></w:tbl>"
      "<w:p><w:pPr><w:rPr><w:i/></w:rPr></w:pPr></w:p>"
      "<w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/></w:sectPr>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  CHECK(RoundTrips(*doc));
  Editor ed(*doc);
  int edits = 0, refused = 0;
  for (uint32_t p : Main(*doc)) {
    for (uint32_t o = 0; o <= ed.ParaLength(p); ++o) {
      ed.BreakTyping();
      auto r = ed.InsertText(P(p, o), u"x");
      if (!r) {
        ++refused;
        continue;
      }
      ++edits;
      if (!Consistent(*doc, f)) std::printf("  after typing at %u:%u\n", p, o);
      ed.BreakTyping();
      // Inside deleted text, typing cut the deletion in two, which deleting doesn't join again
      // (the same meaning, other markup): undo there.
      bool cut = doc->Para(p).text.starts_with(u"in") && (o == 3 || o == 4);
      auto d = cut ? ed.Undo() : ed.Delete(P(p, o), P(p, o + 1));
      CHECK(d);
      if (!RoundTrips(*doc)) {
        std::printf("  after typing and deleting at %u:%u\n", p, o);
        CHECK(false);
        break;
      }
    }
  }
  CHECK(edits > 40 && refused == 0);
  // And Enter then Backspace everywhere text may be split.
  for (uint32_t p : Main(*doc)) {
    for (uint32_t o = 0; o <= ed.ParaLength(p); ++o) {
      if (!ed.SplitParagraph(P(p, o))) continue;
      if (!Consistent(*doc, f)) std::printf("  after Enter at %u:%u\n", p, o);
      CHECK(ed.Undo());
      if (!RoundTrips(*doc)) {
        std::printf("  after Enter and undo at %u:%u\n", p, o);
        CHECK(false);
        break;
      }
    }
  }
}

TEST(Edit_ModifiedFollowsSavesAndUndo) {
  Fixture f;
  f.body = "<w:p><w:r><w:t>a</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  uint32_t p = Main(*doc)[0];
  CHECK(!ed.Modified() && ed.DirtyParts().empty());
  CHECK(ed.InsertText(P(p, 1), u"b"));
  ed.MarkSaved();
  CHECK(!ed.Modified());
  // Typing after a save is a new step, so undo goes back to the saved state.
  CHECK(ed.InsertText(P(p, 2), u"c") && ed.Modified());
  CHECK(ed.Undo() && !ed.Modified() && doc->Para(p).text == u"ab");
  CHECK(ed.Undo() && ed.Modified() && ed.Redo() && !ed.Modified());
  CHECK(ed.DirtyParts().size() == 1);
}

// ---- Laying out again after edits (Layout::Edited)

namespace {

fw::text::Fonts& EditFonts() {
  static fw::text::Fonts fonts;
  return fonts;
}

// The same pages, line for line.
bool SameLayout(fw::doc::layout::Layout& got, const Document& doc) {
  fw::doc::layout::Layout fresh(doc, EditFonts());
  got.LayOutAll();
  fresh.LayOutAll();
  if (got.PageCount() != fresh.PageCount()) {
    std::printf("  %zu pages, a fresh layout has %zu\n", got.PageCount(), fresh.PageCount());
    return false;
  }
  for (size_t i = 0; i < got.PageCount(); ++i) {
    const auto& a = got.GetPage(i);
    const auto& b = fresh.GetPage(i);
    bool same = a.lines.size() == b.lines.size() && a.number == b.number && a.first == b.first;
    for (size_t k = 0; same && k < a.lines.size(); ++k) {
      const auto& x = a.lines[k];
      const auto& y = b.lines[k];
      same = x.para == y.para && x.text == y.text && std::abs(x.top - y.top) < 0.01f && std::abs(x.x - y.x) < 0.01f;
    }
    if (!same) {
      std::printf("  page %zu differs from a fresh layout's (%zu lines, %zu)\n", i, a.lines.size(), b.lines.size());
      return false;
    }
  }
  return true;
}

std::string LongBody() {
  std::string body;
  const char* words = "the quick brown fox jumps over a lazy dog while seven wizards quietly box jumping frogs ";
  for (int i = 0; i < 70; ++i) {
    std::string text;
    for (int k = 0; k <= (i * 7) % 23; ++k) text += words;
    if (i % 9 == 0)
      body += "<w:p><w:pPr><w:keepNext/><w:spacing w:before=\"240\"/></w:pPr><w:r><w:rPr><w:sz w:val=\"32\"/></w:rPr>"
              "<w:t>Heading " + std::to_string(i) + "</w:t></w:r></w:p>";
    body += "<w:p><w:r><w:t xml:space=\"preserve\">" + std::to_string(i) + " " + text + "</w:t></w:r>";
    if (i == 33) body += "<w:r><w:footnoteReference w:id=\"1\"/></w:r>";
    body += "</w:p>";
    if (i == 25) body += "<w:p><w:r><w:br w:type=\"page\"/></w:r></w:p>";
    if (i == 40)
      body += "<w:tbl><w:tblGrid><w:gridCol w:w=\"4000\"/><w:gridCol w:w=\"4000\"/></w:tblGrid><w:tr><w:tc><w:p><w:r>"
              "<w:t>cell one</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>cell two</w:t></w:r></w:p></w:tc></w:tr></w:tbl>";
  }
  return body + "<w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/><w:pgMar w:top=\"1440\" w:right=\"1440\" "
                "w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/></w:sectPr>";
}

}  // namespace

TEST(Edit_LayingOutAgainMatchesAFreshLayout) {
  Fixture f;
  f.body = LongBody();
  f.footnotes =
      "<w:footnote w:type=\"separator\" w:id=\"-1\"><w:p><w:r><w:separator/></w:r></w:p></w:footnote>"
      "<w:footnote w:id=\"1\"><w:p><w:r><w:footnoteRef/><w:t xml:space=\"preserve\"> A note.</w:t></w:r></w:p></w:footnote>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  fw::doc::layout::Layout layout(*doc, EditFonts());
  layout.LayOutAll();
  CHECK(layout.PageCount() > 8);
  Editor ed(*doc);
  ed.OnChange([&](const edit::Change& c) { layout.Edited(c.story, c.first, c.removed, c.added); });
  auto ps = Main(*doc);
  auto check = [&](const char* what) {
    if (!SameLayout(layout, *doc)) {
      std::printf("  after %s\n", what);
      CHECK(false);
    }
  };
  CHECK(ed.InsertText(P(ps[12], 5), u"x"));
  check("typing a character");
  CHECK(ed.InsertText(P(ps[20], 3), u"more words that make the paragraph longer by a line or two, and then some more"));
  check("typing a line's worth");
  CHECK(ed.SplitParagraph(P(ps[30], 10)));
  check("Enter");
  CHECK(ed.JoinParagraphs(ps[30]));
  check("joining");
  ps = Main(*doc);
  CHECK(ed.Delete(P(ps[55], 4), P(ps[58], 2)));
  check("deleting across paragraphs");
  ps = Main(*doc);
  CHECK(ed.InsertText(P(ps[0], 0), u"Start: "));
  check("typing on the first page");
  ps = Main(*doc);
  CHECK(ed.InsertText(P(ps.back(), ed.ParaLength(ps.back())), u" The end."));
  check("typing at the end");
  uint32_t cell = kNone;
  for (uint32_t p : ps)
    if (doc->Para(p).text == u"cell one") cell = p;
  CHECK(cell != kNone && ed.InsertText(P(cell, 4), u" and a half"));
  check("typing in a table");
  uint32_t note = doc->StoryParagraphs(doc->NoteStory(StoryKind::Footnote, "1")).at(0);
  CHECK(ed.InsertText(P(note, ed.ParaLength(note)), u" It grew into a much longer note than it was before, two lines."));
  check("typing in a footnote");
  while (ed.CanUndo()) CHECK(ed.Undo());
  check("undoing everything");
  CHECK(RoundTrips(*doc));
  // Edits while the layout is still under way.
  fw::doc::layout::Layout partial(*doc, EditFonts());
  partial.LayOutPages(3);
  Editor ed2(*doc);
  ed2.OnChange([&](const edit::Change& c) { partial.Edited(c.story, c.first, c.removed, c.added); });
  CHECK(ed2.SplitParagraph(P(ps[2], 3)) && ed2.InsertText(P(ps[60], 0), u"Far ahead "));
  if (!SameLayout(partial, *doc)) CHECK(false);
}

TEST(Edit_LinesForTheCaret) {
  Fixture f;
  f.body = LongBody();
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  fw::doc::layout::Layout layout(*doc, EditFonts());
  auto ps = Main(*doc);
  uint32_t p = ps[3];  // a paragraph of several lines
  auto first = layout.LineAt({p, 0});
  CHECK(first && first->start == (fw::doc::layout::TextPos{p, 0}) && first->end.offset > 10);
  if (!first) return;
  // Home and End stay on the line: End is before the wrap, and the caret there is on the same line.
  auto caretEnd = layout.CaretAt(first->end);
  CHECK(caretEnd && std::abs(caretEnd->top - first->box.top) < 0.5f);
  // Down from the first line: the second line, near the same x; up again: the first.
  auto down = layout.LineStep({p, 3}, 1, first->box.left + 20);
  CHECK(down && down->para == p && down->offset > first->end.offset);
  if (down) {
    auto up = layout.LineStep(*down, -1, first->box.left + 20);
    CHECK(up && up->para == p && up->offset < first->end.offset);
  }
  // Many lines down crosses pages; past either end there's nothing.
  auto deep = layout.LineStep({p, 0}, 200, 100);
  CHECK(deep && deep->para > p);
  CHECK(!layout.LineStep({ps[0], 0}, -1, 100));
  CHECK(!layout.LineStep({ps.back(), 0}, 100000, 100));
}

// ---------------------------------------------------------------------------
// The clipboard (D2): copying keeps what a reader sees, the converters carry it to RTF and HTML
// and back, and pasting gives the text that look without damaging the file.

namespace {

using edit::Clip;
using edit::ClipAlign;
using edit::ClipPara;
using edit::ClipRun;

const char* const kClipStyles =
    "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Calibri\" w:hAnsi=\"Calibri\" w:cs=\"Calibri\"/>"
    "<w:sz w:val=\"22\"/></w:rPr></w:rPrDefault></w:docDefaults>"
    "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/>"
    "<w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr><w:rPr><w:b/><w:sz w:val=\"32\"/></w:rPr></w:style>"
    "<w:style w:type=\"character\" w:default=\"1\" w:styleId=\"DefaultParagraphFont\"><w:name w:val=\"Default Paragraph Font\"/></w:style>"
    "<w:style w:type=\"character\" w:styleId=\"Strong\"><w:name w:val=\"Strong\"/><w:rPr><w:b/></w:rPr></w:style>";

const char* const kClipNumbering =
    "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/>"
    "<w:lvlText w:val=\"\xE2\x80\xA2\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"720\" w:hanging=\"360\"/></w:pPr>"
    "</w:lvl></w:abstractNum><w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>";

// The resolved look of the text at `offset`.
const RunProps& LookAt(const Document& doc, uint32_t para, uint32_t offset) {
  for (const TextRun& t : doc.Runs(para))
    if (t.run && t.start <= offset && offset < t.end) return doc.ResolvedRun(para, t.run);
  return doc.ResolvedMark(para);
}

std::u16string ClipText(const ClipPara& p) {
  std::u16string s;
  for (const ClipRun& r : p.runs) s += r.text;
  return s;
}

bool SameRuns(const ClipPara& a, const ClipPara& b) {
  if (a.runs.size() != b.runs.size()) {
    std::printf("  %zu runs, %zu read back (\"%s\" / \"%s\")\n", a.runs.size(), b.runs.size(), ToUtf8(ClipText(a)).c_str(),
                ToUtf8(ClipText(b)).c_str());
    return false;
  }
  for (size_t i = 0; i < a.runs.size(); ++i)
    if (a.runs[i].text != b.runs[i].text || !a.runs[i].SameLook(b.runs[i])) {
      std::printf("  run %zu (\"%s\") differs from the one read back (\"%s\")\n", i, ToUtf8(a.runs[i].text).c_str(),
                  ToUtf8(b.runs[i].text).c_str());
      return false;
    }
  return true;
}

// A clip with something of everything; `own` adds what only Featherweight's own copies carry.
Clip SampleClip(bool own) {
  Clip c;
  ClipPara h;
  h.heading = 1;
  h.align = ClipAlign::Center;
  ClipRun big;
  big.text = u"Big";
  big.bold = true;
  big.halfPoints = 32;
  big.font = "Calibri";
  h.runs.push_back(big);
  ClipPara p;
  p.label = u"1.";
  p.indent = 720;
  p.firstLine = -360;
  p.spaceBefore = 120;
  p.spaceAfter = 240;
  p.align = ClipAlign::Justify;
  ClipRun plain;
  plain.text = u"a\tb  c é€ & <x> \U0001F600 ";
  plain.halfPoints = 22;
  plain.font = "Calibri";
  ClipRun it = plain;
  it.text = u"it";
  it.italic = true;
  ClipRun us = plain;
  us.text = u"us";
  us.underline = us.strike = true;
  ClipRun sup = plain;
  sup.text = u"2";
  sup.vertAlign = VertAlign::Superscript;
  ClipRun colored = plain;
  colored.text = u"f\vg";
  colored.color = 0x336699;
  colored.font = "Arial";
  colored.halfPoints = 21;
  p.runs = {plain, it, us, sup, colored};
  if (own) {
    c.source = 0xABC;
    h.style = "Heading1";
    p.numId = "3";
    p.ilvl = 1;
    p.runs[1].charStyle = "Emphasis";
  }
  c.paras = {h, p, ClipPara{}};  // ends with a paragraph mark
  return c;
}

}  // namespace

TEST(Clip_CopyKeepsWhatAReaderSees) {
  Fixture f;
  f.styles = kClipStyles;
  f.body =
      "<w:p><w:pPr><w:pStyle w:val=\"Heading1\"/><w:jc w:val=\"center\"/></w:pPr><w:r><w:t>Title</w:t></w:r></w:p>"
      "<w:p><w:r><w:t xml:space=\"preserve\">Plain </w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>bold</w:t></w:r>"
      "<w:del w:id=\"1\" w:author=\"A\"><w:r><w:delText>gone</w:delText></w:r></w:del>"
      "<w:r><w:rPr><w:i/><w:color w:val=\"FF0000\"/></w:rPr><w:t xml:space=\"preserve\"> red</w:t></w:r>"
      "<w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText xml:space=\"preserve\"> PAGE </w:instrText></w:r>"
      "<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r><w:r><w:t>3</w:t></w:r><w:r><w:fldChar w:fldCharType=\"end\"/></w:r>"
      "<w:r><w:rPr><w:vanish/></w:rPr><w:t>hidden</w:t></w:r><w:r><w:tab/><w:t>x</w:t><w:br/><w:t>y</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  auto ps = Main(*doc);
  Clip c = edit::CopyClip(*doc, P(ps[0], 0), P(ps[1], static_cast<uint32_t>(doc->Para(ps[1]).text.size())), 42);
  CHECK(c.source == 42 && c.paras.size() == 2);
  if (c.paras.size() != 2) return;
  const ClipPara& h = c.paras[0];
  CHECK(h.heading == 1 && h.style == "Heading1" && h.align == ClipAlign::Center && ClipText(h) == u"Title");
  CHECK(h.runs.size() == 1 && h.runs[0].bold && h.runs[0].halfPoints == 32 && h.runs[0].font == "Calibri");
  const ClipPara& b = c.paras[1];
  CHECK(ClipText(b) == u"Plain bold red3\tx\vy");
  CHECK(b.runs.size() == 4);
  if (b.runs.size() == 4) {
    CHECK(!b.runs[0].bold && b.runs[1].bold && b.runs[1].text == u"bold");
    CHECK(b.runs[2].italic && b.runs[2].color == std::optional<uint32_t>(0xFF0000) && b.runs[2].text == u" red");
    CHECK(b.runs[3].text == u"3\tx\vy" && !b.runs[3].color);
  }
  CHECK(c.PlainText() == u"Title\r\nPlain bold red3\tx\r\ny");
  // From the middle of a paragraph to the start of the next: the mark comes too.
  Clip part = edit::CopyClip(*doc, P(ps[0], 2), P(ps[1], 0));
  CHECK(part.paras.size() == 2 && ClipText(part.paras[0]) == u"tle" && part.paras[1].runs.empty());
}

TEST(Clip_RtfAndHtmlCarryTheLook) {
  // HTML carries everything, Featherweight's own extras included.
  Clip c = SampleClip(true);
  std::string html = edit::ToHtml(c);
  CHECK(html.starts_with("Version:0.9\r\n") && edit::IsOwnHtml(html));
  auto at = [&](const char* key) { return std::stoul(html.substr(html.find(key) + std::strlen(key), 10)); };
  CHECK(html.substr(at("StartFragment:"), 5) == "<div " && html.substr(at("EndFragment:"), 18) == "<!--EndFragment-->");
  CHECK(at("EndHTML:") == html.size());
  auto back = edit::FromHtml(html);
  CHECK(back && back->source == 0xABC && back->paras.size() == 3);
  if (back && back->paras.size() == 3) {
    for (size_t i = 0; i < 3; ++i) {
      const ClipPara &a = c.paras[i], &b = back->paras[i];
      CHECK(SameRuns(a, b));
      CHECK(a.align == b.align && a.heading == b.heading && a.label == b.label && a.style == b.style);
      CHECK(a.numId == b.numId && a.ilvl == b.ilvl);
      CHECK(a.indent.value_or(0) == b.indent.value_or(0) && a.firstLine.value_or(0) == b.firstLine.value_or(0));
      CHECK(a.spaceBefore.value_or(0) == b.spaceBefore.value_or(0) && a.spaceAfter.value_or(0) == b.spaceAfter.value_or(0));
    }
  }
  // RTF carries the look; styles, lists and the source stay behind.
  Clip plain = SampleClip(false);
  std::string rtf = edit::ToRtf(plain);
  CHECK(rtf.starts_with("{\\rtf1") && std::none_of(rtf.begin(), rtf.end(), [](char ch) { return (ch & 0x80) != 0; }));
  auto fromRtf = edit::FromRtf(rtf);
  CHECK(fromRtf && fromRtf->paras.size() == 3);
  if (fromRtf && fromRtf->paras.size() == 3) {
    for (size_t i = 0; i < 3; ++i) {
      const ClipPara &a = plain.paras[i], &b = fromRtf->paras[i];
      CHECK(SameRuns(a, b));
      CHECK(a.align == b.align && a.heading == b.heading && a.label == b.label);
      CHECK(a.indent.value_or(0) == b.indent.value_or(0) && a.firstLine.value_or(0) == b.firstLine.value_or(0));
      CHECK(a.spaceBefore.value_or(0) == b.spaceBefore.value_or(0) && a.spaceAfter.value_or(0) == b.spaceAfter.value_or(0));
    }
  }
  CHECK(!edit::FromRtf("not rtf") && !edit::FromHtml("plain text"));
}

TEST(Clip_ReadsWordsRtf) {
  const char* rtf =
      "{\\rtf1\\ansi\\ansicpg1252\\deff0\\deflang1033{\\fonttbl{\\f0\\froman\\fcharset0 Times New Roman;}"
      "{\\f1\\fswiss\\fcharset0 Arial{\\*\\falt Helvetica};}{\\f3\\fbidi \\froman\\fcharset2\\fprq2 Symbol;}}\r\n"
      "{\\colortbl;\\red255\\green0\\blue0;\\red0\\green0\\blue0;}\r\n"
      "{\\stylesheet{\\ql \\qc\\fs24 \\snext0 Normal;}{\\s1\\ql\\b\\fs32 \\sbasedon0 \\snext0 heading 1,h1;}"
      "{\\*\\cs10 \\additive Default Paragraph Font;}}\r\n"
      "{\\*\\generator Riched20;}{\\info{\\title Secret}}{\\header\\pard\\plain Header text\\par}\r\n"
      "\\pard\\plain \\s1\\qc\\b\\fs32 Title\\par\r\n"
      "\\pard\\plain \\f1\\fs22 Caf\\'e9 {\\b bold} {\\i\\cf1 red} \\u8364?\\tab x{\\field{\\*\\fldinst PAGE}{\\fldrslt 3}}"
      "\\line y{\\*\\bkmkstart b}{\\*\\bkmkend b}{\\v hidden}{\\pict\\wmetafile8 0102}\\par\r\n"
      "\\pard\\plain \\li720\\fi-360{\\listtext\\pard\\plain\\f3\\fs20 \\u-3913\\'b7\\tab}\\fs24 Item\\par\r\n"
      "\\trowd\\cellx1000\\cellx2000\\pard\\intbl A\\cell B\\cell\\row\r\n"
      "\\pard\\plain end}";
  auto c = edit::FromRtf(rtf);
  CHECK(c && c->paras.size() == 5);
  if (!c || c->paras.size() != 5) return;
  const ClipPara& t = c->paras[0];
  CHECK(ClipText(t) == u"Title" && t.heading == 1 && t.align == ClipAlign::Center);
  CHECK(t.runs.size() == 1 && t.runs[0].bold && t.runs[0].halfPoints == 32 && t.runs[0].font == "Times New Roman");
  const ClipPara& b = c->paras[1];
  CHECK(ClipText(b) == u"Café bold red €\tx3\vy");
  CHECK(b.align == ClipAlign::Left && b.heading == 0);
  CHECK(b.runs.size() == 5);
  if (b.runs.size() == 5) {
    CHECK(b.runs[0].font == "Arial" && b.runs[0].halfPoints == 22 && !b.runs[0].bold);
    CHECK(b.runs[1].bold && b.runs[1].text == u"bold");
    CHECK(b.runs[3].italic && b.runs[3].color == std::optional<uint32_t>(0xFF0000));
  }
  const ClipPara& item = c->paras[2];
  CHECK(item.label == u"•" && ClipText(item) == u"Item" && item.indent == 720 && item.firstLine == -360);
  CHECK(ClipText(c->paras[3]) == u"A\tB");
  CHECK(ClipText(c->paras[4]) == u"end");
}

TEST(Clip_ReadsBrowsersAndWordsHtml) {
  auto cf = [](std::string_view before, std::string_view frag, std::string_view after) {
    const char* const kHeader =
        "Version:0.9\r\nStartHTML:%010zu\r\nEndHTML:%010zu\r\nStartFragment:%010zu\r\nEndFragment:%010zu\r\n";
    char probe[200];
    const size_t head = static_cast<size_t>(std::snprintf(probe, sizeof probe, kHeader, size_t{0}, size_t{0}, size_t{0}, size_t{0}));
    std::string body = std::string(before) + "<!--StartFragment-->";
    const size_t sf = head + body.size();
    body += frag;
    const size_t ef = head + body.size();
    body += "<!--EndFragment-->" + std::string(after);
    char h[200];
    std::snprintf(h, sizeof h, kHeader, head, head + body.size(), sf, ef);
    return std::string(h) + body;
  };
  // A browser's: inline styles, a link, entities, a list; no paragraph mark after the last block.
  std::string chrome = cf("<html><body>\r\n",
                          "<h2 style=\"color: rgb(32, 33, 36); font-family: Arial, sans-serif; font-size: 24px;\">News</h2>"
                          "<p style=\"font-size: 16px;\">One   <b>two</b>\n<a href=\"x\" style=\"color: rgb(26, 13, 171);\">link</a>"
                          "&nbsp;&amp; more</p><ul><li>First</li><li>Second</li></ul><script>bad()</script>",
                          "\r\n</body></html>");
  auto c = edit::FromHtml(chrome);
  CHECK(c && c->paras.size() == 4 && !edit::IsOwnHtml(chrome));
  if (c && c->paras.size() == 4) {
    const ClipPara& h = c->paras[0];
    CHECK(ClipText(h) == u"News" && h.heading == 2 && h.runs.size() == 1);
    if (h.runs.size() == 1)
      CHECK(h.runs[0].bold && h.runs[0].halfPoints == 36 && h.runs[0].font == "Arial" &&
            h.runs[0].color == std::optional<uint32_t>(0x202124));
    const ClipPara& p = c->paras[1];
    CHECK(ClipText(p) == u"One two link & more");
    CHECK(p.runs.size() == 5);
    if (p.runs.size() == 5)
      CHECK(p.runs[0].halfPoints == 24 && p.runs[1].bold && p.runs[3].color == std::optional<uint32_t>(0x1A0DAB));
    CHECK(c->paras[2].label == u"•" && ClipText(c->paras[2]) == u"First" && ClipText(c->paras[3]) == u"Second");
  }
  // Word's: a style sheet, a list item with its label in a conditional comment, an empty paragraph.
  std::string word = cf(
      "<html xmlns:o=\"urn:schemas-microsoft-com:office:office\"><head><style>p.MsoNormal{margin:0in;}</style></head>"
      "<body lang=EN-US>\r\n",
      "<p class=MsoListParagraph style='margin-left:.5in;text-indent:-.25in;mso-list:l0 level1 lfo1'>"
      "<![if !supportLists]><span style='font-family:Symbol'><span style='mso-list:Ignore'>\xC2\xB7"
      "<span style='font:7.0pt \"Times New Roman\"'>&nbsp;&nbsp;&nbsp; </span></span></span><![endif]>Bullet one<o:p></o:p></p>\r\n"
      "<p class=MsoNormal><o:p>&nbsp;</o:p></p>\r\n"
      "<p class=MsoNormal align=center style='text-align:center'><b><span style='font-size:14.0pt;color:#C00000'>"
      "Centered</span></b><o:p></o:p></p>",
      "</body></html>");
  auto w = edit::FromHtml(word);
  CHECK(w && w->paras.size() == 3);
  if (w && w->paras.size() == 3) {
    CHECK(w->paras[0].label == u"•" && ClipText(w->paras[0]) == u"Bullet one");
    CHECK(w->paras[0].indent == 720 && w->paras[0].firstLine == -360);
    CHECK(w->paras[1].runs.empty());
    const ClipPara& ce = w->paras[2];
    CHECK(ClipText(ce) == u"Centered" && ce.align == ClipAlign::Center && ce.runs.size() == 1);
    if (ce.runs.size() == 1)
      CHECK(ce.runs[0].bold && ce.runs[0].halfPoints == 28 && ce.runs[0].color == std::optional<uint32_t>(0xC00000));
  }
}

TEST(Clip_PasteKeepsTheLookAndTheFile) {
  Fixture f;
  f.styles = kClipStyles;
  f.body =
      "<w:p><w:r><w:t xml:space=\"preserve\">Plain </w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>bold</w:t></w:r>"
      "<w:r><w:rPr><w:i/><w:color w:val=\"FF0000\"/></w:rPr><w:t xml:space=\"preserve\"> red</w:t></w:r></w:p>"
      "<w:p><w:r><w:rPr><w:sz w:val=\"40\"/></w:rPr><w:t>Target here</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const std::string before = MainXml(*doc);
  Editor ed(*doc);
  auto ps = Main(*doc);
  Clip c = edit::CopyClip(*doc, P(ps[0], 0), P(ps[0], 14));
  auto r = edit::PasteClip(ed, P(ps[1], 7), P(ps[1], 7), c);
  CHECK(r && r.caret == P(ps[1], 21));
  CHECK(doc->Para(ps[1]).text == u"Target Plain bold redhere");
  // The pasted text looks as it did; the text around it as it did.
  const RunProps& plain = LookAt(*doc, ps[1], 8);
  CHECK(!plain.bold && plain.size.Value() == 22);
  const RunProps& bold = LookAt(*doc, ps[1], 13);
  CHECK(bold.bold && bold.size.Value() == 22);
  const RunProps& red = LookAt(*doc, ps[1], 18);
  CHECK(red.italic && !red.color.automatic && red.color.rgb == 0xFF0000);
  CHECK(LookAt(*doc, ps[1], 2).size.Value() == 40 && LookAt(*doc, ps[1], 22).size.Value() == 40);
  CHECK(Consistent(*doc, f));
  // One undo step puts the file back exactly.
  CHECK(ed.UndoName() == u"Paste" && ed.Undo() && MainXml(*doc) == before && !ed.CanUndo());
  // Over a selection; text that already looks right gets no formatting of its own.
  Clip same = edit::CopyClip(*doc, P(ps[0], 0), P(ps[0], 5));
  CHECK(edit::PasteClip(ed, P(ps[0], 6), P(ps[0], 10), same) && doc->Para(ps[0]).text == u"Plain Plain red");
  CHECK(BodyIs(*doc,
               "<w:p><w:r><w:t xml:space=\"preserve\">Plain Plain</w:t></w:r>"
               "<w:r><w:rPr><w:i/><w:color w:val=\"FF0000\"/></w:rPr><w:t xml:space=\"preserve\"> red</w:t></w:r></w:p>"
               "<w:p><w:r><w:rPr><w:sz w:val=\"40\"/></w:rPr><w:t>Target here</w:t></w:r></w:p>"));
}

TEST(Clip_PasteParagraphsHeadingsAndLists) {
  Fixture f;
  f.styles = kClipStyles;
  f.numbering = kClipNumbering;
  f.body =
      "<w:p><w:r><w:t>ABCD</w:t></w:r></w:p>"
      "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>Item</w:t></w:r></w:p>"
      "<w:p><w:r><w:t>Other</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const std::string before = MainXml(*doc);
  Editor ed(*doc);
  auto ps = Main(*doc);
  // A heading and a paragraph from a web page, into the middle of a paragraph: the first pasted
  // paragraph's mark makes "ABHead" a heading; "BodyCD" keeps the paragraph it was.
  auto web = edit::FromHtml("<h1>Head</h1><p>Body</p>");
  CHECK(web && web->paras.size() == 2);
  if (!web) return;
  auto r = edit::PasteClip(ed, P(ps[0], 2), P(ps[0], 2), *web);
  ps = Main(*doc);
  CHECK(r && ps.size() == 4 && r.caret == P(ps[1], 4));
  CHECK(doc->Para(ps[0]).text == u"ABHead" && doc->Para(ps[1]).text == u"BodyCD");
  CHECK(doc->ResolvedPara(ps[0]).style == "Heading1" && doc->ResolvedPara(ps[1]).style == "Normal");
  CHECK(LookAt(*doc, ps[0], 3).bold && !LookAt(*doc, ps[1], 1).bold);
  CHECK(MainXml(*doc).find("<w:b/>") == std::string::npos);  // the style makes it bold
  CHECK(Consistent(*doc, f));
  CHECK(ed.Undo() && MainXml(*doc) == before);
  // A list item with its mark, back into the same document: still in the list.
  ps = Main(*doc);
  Clip item = edit::CopyClip(*doc, P(ps[1], 0), P(ps[2], 0), 7);
  CHECK(item.paras.size() == 2 && item.paras[0].label == u"•" && item.paras[0].numId == "1");
  CHECK(edit::PasteClip(ed, P(ps[2], 0), P(ps[2], 0), item, 7));
  ps = Main(*doc);
  CHECK(ps.size() == 4 && doc->Para(ps[2]).text == u"Item" && doc->ResolvedPara(ps[2]).numId == "1");
  CHECK(doc->Para(ps[3]).text == u"Other" && doc->ResolvedPara(ps[3]).numId.empty());
  CHECK(Consistent(*doc, f));
  CHECK(ed.Undo());
  // Into another document: the label as text.
  ps = Main(*doc);
  CHECK(edit::PasteClip(ed, P(ps[2], 0), P(ps[2], 0), item, 8));
  ps = Main(*doc);
  CHECK(doc->Para(ps[2]).text == u"•\tItem" && doc->ResolvedPara(ps[2]).numId.empty());
  CHECK(Consistent(*doc, f) && ed.Undo() && MainXml(*doc) == before);
}

TEST(Clip_PasteThatWouldDamageIsRefused) {
  Fixture f;
  f.body =
      "<w:p><w:r><w:t xml:space=\"preserve\">Page </w:t></w:r><w:r><w:fldChar w:fldCharType=\"begin\"/></w:r>"
      "<w:r><w:instrText xml:space=\"preserve\"> PAGE </w:instrText></w:r><w:r><w:fldChar w:fldCharType=\"separate\"/>"
      "</w:r><w:r><w:t>3</w:t></w:r><w:r><w:fldChar w:fldCharType=\"end\"/></w:r>"
      "<w:r><w:t xml:space=\"preserve\"> of</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  uint32_t p = Main(*doc)[0];
  auto web = edit::FromHtml("<p>x</p><p>y</p>");
  CHECK(web);
  if (!web) return;
  // Over part of the field: refused, nothing changes, nothing to undo.
  auto r = edit::PasteClip(ed, P(p, 3), P(p, 7), *web);
  CHECK(!r && !r.message.empty() && !ed.CanUndo() && !ed.Modified() && RoundTrips(*doc));
  // Into the field's result: the field can't hold a paragraph break, so that's refused too, and
  // nothing changes; one paragraph goes in.
  auto two = edit::PasteClip(ed, P(p, 14), P(p, 14), *web);
  CHECK(!ed.CanUndo() || two);
  auto one = edit::FromHtml("<span>x</span>");
  CHECK(one && edit::PasteClip(ed, P(p, 14), P(p, 14), *one) && Consistent(*doc, f));
}

// ---------------------------------------------------------------------------
// The editing torture test's findings (d2-torture), each as a boundary of its own.

namespace {

// Every part written from the model, saved and opened again, passes the package checker.
bool ChecksOut(Document& doc) {
  if (!docx::WriteAllParts(doc).Ok()) return false;
  fw::pkg::MemorySink sink;
  if (!doc.Package().Save(sink).Ok()) return false;
  fw::pkg::Status st;
  auto again = test::OpenBytes(std::move(sink.bytes), st);
  if (!again) return false;
  auto report = fw::pkg::CheckPackage(*again);
  for (const auto& e : report.errors) std::printf("  check: %s\n", e.c_str());
  return report.Ok();
}

std::string PartXml(const Document& doc, PartKind kind) {
  const Part* part = doc.FindPart(kind);
  std::string out;
  if (part && !docx::WritePart(doc, static_cast<uint32_t>(part - doc.Parts().data()), out).Ok()) out.clear();
  return out;
}

}  // namespace

TEST(Edit_SplitKeepsEmptyRangesInOrder) {
  // An empty comment range and Word's empty _GoBack bookmark: a split where both ends sit keeps
  // them together, start before end.
  Fixture f;
  f.comments = "<w:comment w:id=\"0\" w:author=\"A\"><w:p><w:r><w:t>Note</w:t></w:r></w:p></w:comment>";
  f.body =
      "<w:p><w:commentRangeStart w:id=\"0\"/><w:commentRangeEnd w:id=\"0\"/><w:r><w:commentReference w:id=\"0\"/></w:r>"
      "<w:r><w:t>He</w:t></w:r><w:bookmarkStart w:id=\"1\" w:name=\"_GoBack\"/><w:bookmarkEnd w:id=\"1\"/>"
      "<w:r><w:t>llo</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  // ChecksOut saves into the package, so the original is kept here to compare with.
  const std::string before = MainXml(*doc);
  Editor ed(*doc);
  uint32_t p = Main(*doc)[0];
  CHECK(ed.SplitParagraph(P(p, 0)) && ChecksOut(*doc) && Consistent(*doc, f));
  CHECK(ed.Undo() && ed.SplitParagraph(P(p, 3)) && ChecksOut(*doc) && Consistent(*doc, f));
  const std::string xml = MainXml(*doc);
  CHECK(xml.find("<w:bookmarkStart") < xml.find("<w:bookmarkEnd"));
  CHECK(ed.Undo() && UndoSteps(ed) == 0 && MainXml(*doc) == before);
}

TEST(Edit_PropertiesGoBeforeKeptContent) {
  // Word 2010's extensions end w:rPr; a property added after them would be out of order.
  Fixture f;
  f.body =
      "<w:p><w:pPr><w:rPr><w:b/><w14:ligatures w14:val=\"standard\"/></w:rPr></w:pPr>"
      "<w:r><w:rPr><w:b/><w14:ligatures w14:val=\"standard\"/></w:rPr><w:t>Liga</w:t></w:r></w:p>"
      "<w:p><w:r><mc:AlternateContent><mc:Choice Requires=\"x\"><w:t>never</w:t></mc:Choice>"
      "<mc:Fallback><w:t>fallback</w:t></mc:Fallback></mc:AlternateContent><w:t xml:space=\"preserve\"> text</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  auto ps = Main(*doc);
  CHECK(ed.SetRunProps(P(ps[0], 0), P(ps[0], 4), PropEdit().Set("color", {{"val", "C00000"}})));
  CHECK(ed.SetMarkProps(ps[0], ps[0], PropEdit().Set("i")));
  CHECK(ed.SetRunProps(P(ps[1], 0), P(ps[1], ed.ParaLength(ps[1])), PropEdit().Set("color", {{"val", "C00000"}})));
  CHECK(BodyIs(*doc,
               "<w:p><w:pPr><w:rPr><w:b/><w:i/><w14:ligatures w14:val=\"standard\"/></w:rPr></w:pPr>"
               "<w:r><w:rPr><w:b/><w:color w:val=\"C00000\"/><w14:ligatures w14:val=\"standard\"/></w:rPr><w:t>Liga</w:t></w:r></w:p>"
               "<w:p><w:r><w:rPr><w:color w:val=\"C00000\"/></w:rPr><mc:AlternateContent><mc:Choice Requires=\"x\"><w:t>never</w:t></mc:Choice>"
               "<mc:Fallback><w:t>fallback</w:t></mc:Fallback></mc:AlternateContent><w:t xml:space=\"preserve\"> text</w:t></w:r></w:p>"));
  CHECK(Consistent(*doc, f) && UndoSteps(ed) == 3 && RoundTrips(*doc));
}

TEST(Edit_SharedPropertiesChangeOnlyTheirParagraph) {
  // The reader shares identical property sets between paragraphs and runs; formatting one changes
  // only that one.
  Fixture f;
  f.body =
      "<w:p><w:pPr><w:spacing w:after=\"0\"/></w:pPr><w:r><w:rPr><w:i/></w:rPr><w:t>One</w:t></w:r></w:p>"
      "<w:p><w:pPr><w:spacing w:after=\"0\"/></w:pPr><w:r><w:rPr><w:i/></w:rPr><w:t>Two</w:t></w:r></w:p>"
      "<w:p><w:pPr><w:spacing w:after=\"0\"/></w:pPr><w:r><w:rPr><w:i/></w:rPr><w:t>Three</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  auto ps = Main(*doc);
  CHECK(ed.SetParaProps(ps[1], ps[1], PropEdit().Set("jc", {{"val", "center"}})));
  CHECK(ed.SetRunProps(P(ps[1], 0), P(ps[1], 3), PropEdit().Set("b")));
  CHECK(BodyIs(*doc,
               "<w:p><w:pPr><w:spacing w:after=\"0\"/></w:pPr><w:r><w:rPr><w:i/></w:rPr><w:t>One</w:t></w:r></w:p>"
               "<w:p><w:pPr><w:spacing w:after=\"0\"/><w:jc w:val=\"center\"/></w:pPr><w:r><w:rPr><w:b/><w:i/></w:rPr><w:t>Two</w:t></w:r></w:p>"
               "<w:p><w:pPr><w:spacing w:after=\"0\"/></w:pPr><w:r><w:rPr><w:i/></w:rPr><w:t>Three</w:t></w:r></w:p>"));
  CHECK(doc->ResolvedPara(ps[0]).jc == Justification::Start && doc->ResolvedPara(ps[2]).jc == Justification::Start);
  CHECK(doc->ResolvedPara(ps[1]).jc == Justification::Center);
  CHECK(Consistent(*doc, f) && UndoSteps(ed) == 2 && RoundTrips(*doc));
}

TEST(Edit_CommentsKeepTheirLastParagraphsId) {
  // commentsExtended names a comment by its last paragraph's w14:paraId: joining that paragraph
  // into the one before, or splitting it, keeps the id on the comment's last paragraph.
  Fixture f;
  f.comments =
      "<w:comment w:id=\"0\" w:author=\"A\"><w:p w14:paraId=\"11111111\" w14:textId=\"77777777\"><w:r><w:t>One</w:t></w:r></w:p>"
      "<w:p w14:paraId=\"22222222\" w14:textId=\"77777777\"><w:r><w:t>Two</w:t></w:r></w:p></w:comment>";
  f.commentsExtended =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<w15:commentsEx "
      "xmlns:w15=\"http://schemas.microsoft.com/office/word/2012/wordml\"><w15:commentEx w15:paraId=\"22222222\" "
      "w15:done=\"0\"/></w15:commentsEx>";
  f.body =
      "<w:p><w:commentRangeStart w:id=\"0\"/><w:r><w:t>Text</w:t></w:r><w:commentRangeEnd w:id=\"0\"/>"
      "<w:r><w:commentReference w:id=\"0\"/></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const std::string comments = PartXml(*doc, PartKind::Comments);
  Editor ed(*doc);
  const uint32_t story = doc->NoteStory(StoryKind::Comment, "0");
  auto cs = doc->StoryParagraphs(story);
  CHECK(cs.size() == 2);
  if (cs.size() != 2) return;
  auto lastId = [&] {
    auto now = doc->StoryParagraphs(story);
    return std::string(doc->W14ParaId(now.back()));
  };
  // Joined: the one paragraph left is the last, with the id commentsExtended names.
  CHECK(ed.Delete(P(cs[0], 3), P(cs[1], 0)) && lastId() == "22222222");
  CHECK(PartXml(*doc, PartKind::Comments).find("w14:paraId=\"22222222\"") != std::string::npos);
  CHECK(ChecksOut(*doc));
  // Split: the second half is the last paragraph now, and it takes the id.
  CHECK(ed.Undo() && ed.SplitParagraph(P(cs[1], 1)) && lastId() == "22222222");
  auto now = doc->StoryParagraphs(story);
  CHECK(now.size() == 3 && doc->W14ParaId(now[1]) != "22222222" && !doc->W14ParaId(now[1]).empty());
  CHECK(ChecksOut(*doc) && UndoSteps(ed) == 1 && PartXml(*doc, PartKind::Comments) == comments);
}

TEST(Edit_SplitInsideAFieldIsRefused) {
  // A table of contents entry: a hyperlink holding a PAGEREF field. Splitting inside the field
  // would put its start and its end in two hyperlinks, which Word can't open.
  Fixture f;
  f.body =
      "<w:p><w:hyperlink w:anchor=\"_Toc1\"><w:r><w:t>Intro</w:t></w:r><w:r><w:tab/></w:r>"
      "<w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText xml:space=\"preserve\"> PAGEREF _Toc1 \\h </w:instrText></w:r>"
      "<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r><w:r><w:t>3</w:t></w:r><w:r><w:fldChar w:fldCharType=\"end\"/></w:r>"
      "</w:hyperlink></w:p><w:p><w:bookmarkStart w:id=\"0\" w:name=\"_Toc1\"/><w:r><w:t>Intro</w:t></w:r><w:bookmarkEnd w:id=\"0\"/></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  uint32_t p = Main(*doc)[0];
  const std::u16string& s = doc->Para(p).text;
  const auto begin = static_cast<uint32_t>(s.find(u'\x13')), end = static_cast<uint32_t>(s.find(u'\x15'));
  CHECK(begin == 6 && end + 1 == s.size());
  for (uint32_t o : {begin + 1, begin + 4, end - 1, end}) {
    auto r = ed.SplitParagraph(P(p, o));
    CHECK(!r && r.message == u"A paragraph can't be split inside a field.");
  }
  CHECK(!ed.InsertText(P(p, end), u"a\rb"));
  CHECK(!ed.CanUndo() && !ed.Modified() && RoundTrips(*doc));
  // Before the field, or after it: fine.
  CHECK(ed.SplitParagraph(P(p, 2)) && ChecksOut(*doc) && Consistent(*doc, f));
  CHECK(ed.Undo() && ed.SplitParagraph(P(p, end + 1)) && ChecksOut(*doc) && Consistent(*doc, f));
}

TEST(Edit_OnlyListsFieldsAndSectionsAreStructure) {
  // Enter in plain text changes nothing else in the document; in a list it may renumber what follows.
  Fixture f;
  f.numbering = kClipNumbering;
  f.body =
      "<w:p><w:r><w:t>Plain text</w:t></w:r></w:p>"
      "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>Item</w:t></w:r></w:p>"
      "<w:p><w:r><w:t>Page </w:t></w:r><w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText>PAGE</w:instrText></w:r>"
      "<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r><w:r><w:t>1</w:t></w:r><w:r><w:fldChar w:fldCharType=\"end\"/></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  std::vector<edit::Change> changes;
  ed.OnChange([&](const edit::Change& c) { changes.push_back(c); });
  auto ps = Main(*doc);
  auto structure = [&](edit::Result r) { return r && !changes.empty() && changes.back().structure; };
  CHECK(ed.SplitParagraph(P(ps[0], 5)) && !changes.back().structure);
  CHECK(ed.Undo() && !changes.back().structure);
  CHECK(ed.JoinParagraphs(ps[0]) && changes.back().structure);  // joins the list item into plain text
  CHECK(ed.Undo());
  CHECK(structure(ed.SplitParagraph(P(ps[1], 2))));
  CHECK(ed.Undo());
  CHECK(structure(ed.SplitParagraph(P(ps[2], 2))));  // the paragraph has a field
  CHECK(ed.Undo() && ed.InsertText(P(ps[0], 1), u"x") && !changes.back().structure);
}
