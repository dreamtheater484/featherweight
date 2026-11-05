#include "TestHarness.h"

#include <cmath>
#include <string>
#include <vector>

#include "DocTestUtil.h"
#include "doc/edit/Editor.h"
#include "doc/layout/Layout.h"
#include "doc/model/Utf.h"
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
  CHECK(changes.size() == 1 && changes[0].first == p && changes[0].removed == 1 && changes[0].added == 2 &&
        changes[0].structure);
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
  ed.OnChange([&](const edit::Change& c) { layout.Edited(c); });
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
  ed2.OnChange([&](const edit::Change& c) { partial.Edited(c); });
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
