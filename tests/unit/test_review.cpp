#include "TestHarness.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "DocTestUtil.h"
#include "doc/docx/Docx.h"
#include "doc/docx/Save.h"
#include "doc/edit/Comments.h"
#include "doc/edit/Editor.h"
#include "doc/edit/Format.h"
#include "doc/edit/Inline.h"
#include "doc/edit/TreeEdit.h"
#include "doc/model/Utf.h"
#include "pkg/Check.h"

#define REQUIRE(cond) do { if (!(cond)) { test::Fail(__FILE__, __LINE__, #cond); return; } } while (0)

using namespace fw::doc;
using namespace doc_test;
using edit::Editor;

// Review (milestone D4): tracked changes are accepted and rejected as Word does it, and undo puts
// back exactly what was there.

namespace {

Pos P(uint32_t para, uint32_t offset) { return Pos{para, offset}; }

std::vector<uint32_t> Main(const Document& doc) { return doc.StoryParagraphs(doc.MainStory()); }

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

// A change record's attributes: author A or B, one date.
std::string By(const char* author, int id) {
  return " w:id=\"" + std::to_string(id) + "\" w:author=\"" + author + "\" w:date=\"2026-01-01T00:00:00Z\"";
}

}  // namespace

TEST(Review_InlineChangesAcceptedAndRejected) {
  Fixture f;
  f.body = "<w:p><w:r><w:t>a</w:t></w:r><w:ins" + By("A", 1) + "><w:r><w:t>new</w:t></w:r></w:ins><w:del" + By("B", 2) +
           "><w:r><w:delText>old</w:delText></w:r></w:del><w:r><w:t>z</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  const uint32_t p = Main(*doc)[0];
  CHECK(ed.ReviewAll(true));
  CHECK(BodyIs(*doc, "<w:p><w:r><w:t>a</w:t></w:r><w:r><w:t>new</w:t></w:r><w:r><w:t>z</w:t></w:r></w:p>"));
  CHECK(doc->Para(p).text == u"anewz");
  CHECK(ed.UndoName() == u"Accept all changes");
  CHECK(ed.Undo() && RoundTrips(*doc) && doc->Para(p).text == u"anewoldz");
  CHECK(ed.ReviewAll(false));
  CHECK(BodyIs(*doc, "<w:p><w:r><w:t>a</w:t></w:r><w:r><w:t>old</w:t></w:r><w:r><w:t>z</w:t></w:r></w:p>"));
  CHECK(ed.Undo() && RoundTrips(*doc));
  // The caret in the insertion: that one alone.
  CHECK(ed.ReviewChanges(P(p, 2), P(p, 2), false));
  CHECK(BodyIs(*doc, "<w:p><w:r><w:t>a</w:t></w:r><w:del" + By("B", 2) +
                         "><w:r><w:delText>old</w:delText></w:r></w:del><w:r><w:t>z</w:t></w:r></w:p>"));
  CHECK(ed.UndoName() == u"Reject change");
  CHECK(ed.Undo() && RoundTrips(*doc));
  // A selection over part of each takes both whole.
  CHECK(ed.ReviewChanges(P(p, 3), P(p, 5), true) && doc->Para(p).text == u"anewz");
  CHECK(ed.Undo() && RoundTrips(*doc));
  // Nowhere near one: nothing to do.
  CHECK(!ed.ReviewChanges(P(p, 0), P(p, 0), true));
  CHECK(RoundTrips(*doc));
}

TEST(Review_ChangesInsideChanges) {
  Fixture f;
  // A inserted "x", then B deleted it; A inserted "y" and B deleted part of it.
  f.body = "<w:p><w:ins" + By("A", 1) + "><w:del" + By("B", 2) + "><w:r><w:delText>x</w:delText></w:r></w:del></w:ins>" +
           "<w:ins" + By("A", 3) + "><w:r><w:t>y</w:t></w:r><w:del" + By("B", 4) +
           "><w:r><w:delText>z</w:delText></w:r></w:del></w:ins></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  const uint32_t p = Main(*doc)[0];
  CHECK(ed.ReviewAll(true));
  CHECK(BodyIs(*doc, "<w:p><w:r><w:t>y</w:t></w:r></w:p>"));
  CHECK(ed.Undo() && RoundTrips(*doc));
  // Rejecting both: what A inserted was never there.
  CHECK(ed.ReviewAll(false));
  CHECK(BodyIs(*doc, "<w:p/>") && doc->Para(p).text.empty());
  CHECK(ed.Undo() && RoundTrips(*doc));
}

TEST(Review_ParagraphMarks) {
  Fixture f;
  f.body = "<w:p><w:pPr><w:rPr><w:del" + By("A", 1) + "/></w:rPr></w:pPr><w:r><w:t>one</w:t></w:r></w:p>" +
           "<w:p><w:pPr><w:jc w:val=\"center\"/><w:rPr><w:ins" + By("A", 2) + "/></w:rPr></w:pPr><w:r><w:t>two</w:t></w:r></w:p>" +
           "<w:p><w:r><w:t>three</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  // Accepted: the deleted mark goes (its paragraph joins the next), the inserted one stays.
  CHECK(ed.ReviewAll(true));
  CHECK(BodyIs(*doc, "<w:p><w:r><w:t>one</w:t></w:r><w:r><w:t>two</w:t></w:r></w:p><w:p><w:r><w:t>three</w:t></w:r></w:p>"));
  CHECK(Main(*doc).size() == 2);
  CHECK(ed.Undo() && RoundTrips(*doc) && Main(*doc).size() == 3);
  // Rejected: the deleted mark stays, the inserted one goes.
  CHECK(ed.ReviewAll(false));
  CHECK(BodyIs(*doc, "<w:p><w:r><w:t>one</w:t></w:r></w:p><w:p><w:pPr><w:jc w:val=\"center\"/></w:pPr>"
                     "<w:r><w:t>two</w:t></w:r><w:r><w:t>three</w:t></w:r></w:p>"));
  CHECK(ed.Undo() && RoundTrips(*doc));
  // The caret at the end of a paragraph is at its mark.
  const uint32_t first = Main(*doc)[0];
  CHECK(ed.ReviewChanges(P(first, 3), P(first, 3), false));
  CHECK(doc->Revisions(doc->MainStory()).size() == 1);
  CHECK(ed.Undo() && RoundTrips(*doc));
}

TEST(Review_FormattingChanges) {
  Fixture f;
  f.body = "<w:p><w:pPr><w:jc w:val=\"center\"/><w:pPrChange" + By("A", 1) +
           "><w:pPr><w:ind w:left=\"720\"/><w:jc w:val=\"right\"/></w:pPr></w:pPrChange></w:pPr>"
           "<w:r><w:rPr><w:b/><w:rPrChange" + By("A", 2) + "><w:rPr><w:i/></w:rPr></w:rPrChange></w:rPr><w:t>bold</w:t></w:r>"
           "<w:r><w:rPr><w:rPrChange" + By("A", 3) + "><w:rPr><w:u w:val=\"single\"/></w:rPr></w:rPrChange></w:rPr>"
           "<w:t>plain</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  CHECK(ed.ReviewAll(true));
  CHECK(BodyIs(*doc, "<w:p><w:pPr><w:jc w:val=\"center\"/></w:pPr><w:r><w:rPr><w:b/></w:rPr><w:t>bold</w:t></w:r>"
                     "<w:r><w:t>plain</w:t></w:r></w:p>"));
  CHECK(ed.Undo() && RoundTrips(*doc));
  CHECK(ed.ReviewAll(false));
  CHECK(BodyIs(*doc, "<w:p><w:pPr><w:ind w:left=\"720\"/><w:jc w:val=\"right\"/></w:pPr>"
                     "<w:r><w:rPr><w:i/></w:rPr><w:t>bold</w:t></w:r>"
                     "<w:r><w:rPr><w:u w:val=\"single\"/></w:rPr><w:t>plain</w:t></w:r></w:p>"));
  CHECK(ed.Undo() && RoundTrips(*doc));
}

TEST(Review_TableRows) {
  Fixture f;
  const std::string cell = "<w:tc><w:tcPr><w:tcW w:w=\"2000\" w:type=\"dxa\"/></w:tcPr><w:p><w:r><w:t>";
  f.body = "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"2000\"/></w:tblGrid>"
           "<w:tr>" + cell + "kept</w:t></w:r></w:p></w:tc></w:tr>"
           "<w:tr><w:trPr><w:del" + By("A", 1) + "/></w:trPr>" + cell + "gone</w:t></w:r></w:p></w:tc></w:tr>"
           "<w:tr><w:trPr><w:ins" + By("A", 2) + "/></w:trPr>" + cell + "new</w:t></w:r></w:p></w:tc></w:tr>"
           "</w:tbl><w:p/>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  auto texts = [&] {
    std::u16string all;
    for (uint32_t p : Main(*doc)) all += doc->Para(p).text + u"|";
    return all;
  };
  CHECK(ed.ReviewAll(true));
  CHECK(texts() == u"kept|new||");
  CHECK(doc->Revisions(doc->MainStory()).empty());
  CHECK(ed.UndoName() == u"Accept all changes");
  CHECK(ed.Undo() && RoundTrips(*doc) && texts() == u"kept|gone|new||");
  CHECK(ed.ReviewAll(false));
  CHECK(texts() == u"kept|gone||");
  CHECK(doc->Revisions(doc->MainStory()).empty());
  CHECK(ed.Undo() && RoundTrips(*doc));
}

namespace {

// A document with Track Changes on, edited by Ann at a fixed time.
struct Tracked {
  Fixture f;
  std::unique_ptr<Document> doc;
  std::unique_ptr<Editor> ed;
  explicit Tracked(std::string body, std::string settings = "<w:trackRevisions/>") {
    f.body = std::move(body);
    f.settings = std::move(settings);
    doc = LoadOk(f);
    if (!doc) return;
    ed = std::make_unique<Editor>(*doc);
    ed->SetAuthor("Ann");
    ed->SetFixedDate("2026-11-05T10:00:00Z");
  }
};

std::string Ann(int id) { return " w:id=\"" + std::to_string(id) + "\" w:author=\"Ann\" w:date=\"2026-11-05T10:00:00Z\""; }

uint32_t Undos(Editor& ed) {
  uint32_t n = 0;
  while (ed.CanUndo() && ed.Undo()) ++n;
  return n;
}

}  // namespace

TEST(Review_TypingAndDeletingAreRecorded) {
  Tracked t("<w:p><w:r><w:t>abc</w:t></w:r></w:p>");
  CHECK(t.doc && t.ed);
  if (!t.ed) return;
  Editor& ed = *t.ed;
  const Document& doc = *t.doc;
  const uint32_t p = Main(doc)[0];
  CHECK(ed.Tracking());
  CHECK(ed.InsertText(P(p, 1), u"X") && ed.InsertText(P(p, 2), u"Y"));
  CHECK(BodyIs(doc, "<w:p><w:r><w:t>a</w:t></w:r><w:ins" + Ann(0) + "><w:r><w:t>XY</w:t></w:r></w:ins>"
                    "<w:r><w:t>bc</w:t></w:r></w:p>"));
  // Deleting what you typed takes it out; deleting what was there marks it.
  CHECK(ed.Delete(P(p, 2), P(p, 3)) && doc.Para(p).text == u"aXbc");
  CHECK(ed.Delete(P(p, 2), P(p, 3)) && doc.Para(p).text == u"aXbc");
  CHECK(BodyIs(doc, "<w:p><w:r><w:t>a</w:t></w:r><w:ins" + Ann(0) + "><w:r><w:t>X</w:t></w:r></w:ins>"
                    "<w:del" + Ann(1) + "><w:r><w:delText>b</w:delText></w:r></w:del><w:r><w:t>c</w:t></w:r></w:p>"));
  // Over text deleted already, only the rest changes.
  CHECK(ed.Delete(P(p, 1), P(p, 3)) && doc.Para(p).text == u"abc");
  CHECK(BodyIs(doc, "<w:p><w:r><w:t>a</w:t></w:r><w:del" + Ann(1) +
                    "><w:r><w:delText>b</w:delText></w:r></w:del><w:r><w:t>c</w:t></w:r></w:p>"));
  CHECK(Consistent(doc, t.f));
  CHECK(ed.ReviewAll(true) && doc.Para(p).text == u"ac");
  Undos(ed);
  CHECK(RoundTrips(doc));
}

TEST(Review_TypingOverASelectionGoesAfterTheDeletion) {
  Tracked t("<w:p><w:r><w:t>abcd</w:t></w:r></w:p>");
  CHECK(t.ed);
  if (!t.ed) return;
  Editor& ed = *t.ed;
  const uint32_t p = Main(*t.doc)[0];
  auto r = ed.Replace(P(p, 1), P(p, 3), u"X");
  CHECK(r && r.caret == P(p, 4));
  CHECK(BodyIs(*t.doc, "<w:p><w:r><w:t>a</w:t></w:r><w:del" + Ann(0) + "><w:r><w:delText>bc</w:delText></w:r></w:del>"
                       "<w:ins" + Ann(1) + "><w:r><w:t>X</w:t></w:r></w:ins><w:r><w:t>d</w:t></w:r></w:p>"));
  CHECK(ed.InsertText(r.caret, u"Y") && t.doc->Para(p).text == u"abcXYd");
  CHECK(ed.ReviewAll(false) && t.doc->Para(p).text == u"abcd");
  CHECK(Undos(ed) == 2);  // the review, then the typing
  CHECK(RoundTrips(*t.doc));
}

TEST(Review_ParagraphMarksAreRecorded) {
  Tracked t("<w:p><w:r><w:t>one</w:t></w:r></w:p><w:p><w:r><w:t>two</w:t></w:r></w:p>");
  CHECK(t.ed);
  if (!t.ed) return;
  Editor& ed = *t.ed;
  const Document& doc = *t.doc;
  CHECK(ed.Delete(P(0, 3), P(1, 0)) && Main(doc).size() == 2);
  CHECK(BodyIs(doc, "<w:p><w:pPr><w:rPr><w:del" + Ann(0) + "/></w:rPr></w:pPr><w:r><w:t>one</w:t></w:r></w:p>"
                    "<w:p><w:r><w:t>two</w:t></w:r></w:p>"));
  // Enter makes a mark of yours; Backspace takes it out again.
  CHECK(ed.SplitParagraph(P(1, 1)) && Main(doc).size() == 3);
  CHECK(BodyIs(doc, "<w:p><w:pPr><w:rPr><w:del" + Ann(0) + "/></w:rPr></w:pPr><w:r><w:t>one</w:t></w:r></w:p>"
                    "<w:p><w:pPr><w:rPr><w:ins" + Ann(1) + "/></w:rPr></w:pPr><w:r><w:t>t</w:t></w:r></w:p>"
                    "<w:p><w:r><w:t>wo</w:t></w:r></w:p>"));
  CHECK(ed.JoinParagraphs(1) && Main(doc).size() == 2);
  CHECK(BodyIs(doc, "<w:p><w:pPr><w:rPr><w:del" + Ann(0) + "/></w:rPr></w:pPr><w:r><w:t>one</w:t></w:r></w:p>"
                    "<w:p><w:r><w:t>t</w:t></w:r><w:r><w:t>wo</w:t></w:r></w:p>"));
  CHECK(Consistent(doc, t.f));
  CHECK(ed.ReviewAll(true) && Main(doc).size() == 1 && doc.Para(0).text == u"onetwo");
  Undos(ed);
  CHECK(RoundTrips(doc));
}

TEST(Review_SomeoneElsesInsertion) {
  Tracked t("<w:p><w:ins" + By("B", 7) + "><w:r><w:t>abc</w:t></w:r></w:ins></w:p>");
  CHECK(t.ed);
  if (!t.ed) return;
  Editor& ed = *t.ed;
  const uint32_t p = Main(*t.doc)[0];
  CHECK(ed.InsertText(P(p, 1), u"X"));
  CHECK(ed.Delete(P(p, 3), P(p, 4)));
  // (B's insertion is cut twice, 8 and 9; the middle becomes Ann's, 10.)
  CHECK(BodyIs(*t.doc, "<w:p><w:ins" + By("B", 7) + "><w:r><w:t>a</w:t></w:r></w:ins><w:ins" + Ann(10) +
                       "><w:r><w:t>X</w:t></w:r></w:ins><w:ins" + By("B", 9) + "><w:r><w:t>b</w:t></w:r><w:del" + Ann(11) +
                       "><w:r><w:delText>c</w:delText></w:r></w:del></w:ins></w:p>"));
  CHECK(Consistent(*t.doc, t.f));
  CHECK(ed.ReviewAll(true) && t.doc->Para(p).text == u"aXb");
  Undos(ed);
  CHECK(RoundTrips(*t.doc));
}

TEST(Review_FormattingIsRecorded) {
  Tracked t("<w:p><w:r><w:t>abc</w:t></w:r></w:p>");
  CHECK(t.ed);
  if (!t.ed) return;
  Editor& ed = *t.ed;
  const Document& doc = *t.doc;
  const uint32_t p = Main(doc)[0];
  CHECK(ed.SetRunProps(P(p, 0), P(p, 2), edit::PropEdit().Set("b")));
  CHECK(ed.SetParaProps(p, p, edit::PropEdit().Set("jc", {{"val", "center"}})));
  CHECK(BodyIs(doc, "<w:p><w:pPr><w:jc w:val=\"center\"/><w:pPrChange" + Ann(1) + "><w:pPr/></w:pPrChange></w:pPr>"
                    "<w:r><w:rPr><w:b/><w:rPrChange" + Ann(0) + "><w:rPr/></w:rPrChange></w:rPr><w:t>ab</w:t></w:r>"
                    "<w:r><w:t>c</w:t></w:r></w:p>"));
  // Through the formatting commands too, once: the record keeps the oldest look.
  {
    edit::TreeEdit te(ed, P(p, 0));
    edit::ToggleRuns(te, edit::Range{P(p, 0), P(p, 3)}, edit::Toggle::Italic);
  }
  {
    edit::TreeEdit te(ed, P(p, 0));
    edit::ToggleRuns(te, edit::Range{P(p, 0), P(p, 3)}, edit::Toggle::Italic);
  }
  {
    edit::TreeEdit te(ed, P(p, 0));
    edit::ParaFormat f;
    f.jc = Justification::Center;  // as it is: nothing new to record
    edit::FormatParagraphs(te, edit::Range{P(p, 0), P(p, 0)}, f);
  }
  uint32_t runs = 0, paras = 0;
  for (const Revision& r : doc.Revisions(doc.MainStory())) {
    runs += r.kind == RevisionKind::RunFormat;
    paras += r.kind == RevisionKind::ParagraphFormat;
  }
  CHECK(runs == 2 && paras == 1);
  // Rejected, it's all as it was; your own new text isn't recorded.
  CHECK(ed.ReviewAll(false));
  CHECK(BodyIs(doc, "<w:p><w:r><w:t>ab</w:t></w:r><w:r><w:t>c</w:t></w:r></w:p>"));
  CHECK(ed.InsertText(P(p, 3), u"new") && ed.SetRunProps(P(p, 3), P(p, 6), edit::PropEdit().Set("b")));
  CHECK(doc.Revisions(doc.MainStory()).size() == 1);
  Undos(ed);
  CHECK(RoundTrips(doc));
}

TEST(Review_TrackChangesOnAndOff) {
  Fixture f;
  f.body = "<w:p><w:r><w:t>abc</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  CHECK(!ed.Tracking());
  CHECK(ed.SetTracking(true) && ed.Tracking() && doc->FindPart(PartKind::Settings));
  CHECK(ed.InsertText(P(Main(*doc)[0], 3), u"d") && doc->Revisions(doc->MainStory()).size() == 1);
  CHECK(ed.Undo() && ed.Undo() && !ed.Tracking());
  CHECK(ed.InsertText(P(Main(*doc)[0], 3), u"d") && doc->Revisions(doc->MainStory()).empty());
}

TEST(Review_TablesWhileTracking) {
  const std::string cell = "<w:tc><w:tcPr><w:tcW w:w=\"2000\" w:type=\"dxa\"/></w:tcPr><w:p><w:r><w:t>";
  Tracked t("<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"2000\"/>"
            "<w:gridCol w:w=\"2000\"/></w:tblGrid><w:tr>" + cell + "a</w:t></w:r></w:p></w:tc>" + cell +
            "b</w:t></w:r></w:p></w:tc></w:tr></w:tbl><w:p/>");
  CHECK(t.ed);
  if (!t.ed) return;
  Editor& ed = *t.ed;
  const auto paras = Main(*t.doc);
  // Rows and columns can't be recorded yet: refused. Deleting cells' text is recorded.
  CHECK(!ed.InsertRows(P(paras[0], 0), P(paras[0], 0), true));
  CHECK(!ed.InsertTable(P(paras[2], 0), 2, 2));
  CHECK(ed.ClearCells(P(paras[0], 0), P(paras[1], 1)));
  CHECK(t.doc->Para(paras[0]).text == u"a" && t.doc->Revisions(t.doc->MainStory()).size() == 2);
  CHECK(ed.ReviewAll(true) && t.doc->Para(paras[0]).text.empty() && t.doc->Para(paras[1]).text.empty());
  Undos(ed);
  CHECK(RoundTrips(*t.doc));
}

namespace {

const CommentInfo* CommentById(const std::vector<CommentInfo>& all, std::string_view id) {
  for (const CommentInfo& c : all)
    if (c.id == id) return &c;
  return nullptr;
}

// The document saved (everything edited written) and read again.
std::unique_ptr<Document> SavedAndRead(Document& doc, Editor& ed) {
  fw::doc::docx::SaveOptions options;
  options.changedParts = ed.DirtyParts();
  options.edited = true;
  std::vector<uint8_t> bytes;
  if (!fw::doc::docx::Serialize(doc, options, bytes).Ok()) return nullptr;
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(std::move(bytes), st);
  if (!pkg || !fw::pkg::CheckPackage(*pkg).Ok()) return nullptr;
  return pkg ? fw::doc::docx::Read(std::move(pkg), st) : nullptr;
}

std::string PartXml(const Document& doc, std::string_view name) {
  std::vector<uint8_t> bytes;
  return doc.Package().ReadPart(name, bytes).Ok() ? test::Text(bytes) : std::string();
}

}  // namespace

TEST(Comments_AddReplyEditResolveDelete) {
  Fixture f;
  f.body = "<w:p><w:r><w:t xml:space=\"preserve\">The buyer pays.</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  ed.SetAuthor("Ann Lee");
  ed.SetFixedDate("2026-11-05T10:00:00Z");
  const uint32_t p = Main(*doc)[0];
  CHECK(edit::Initials("Ann Lee") == "AL" && edit::Initials("bob@example.com") == "B");
  CHECK(!edit::AddComment(ed, P(p, 4), P(p, 9), u"  "));
  CHECK(edit::AddComment(ed, P(p, 4), P(p, 9), u"Who exactly?"));
  CHECK(doc->Para(p).text == u"The buyer\x05 pays.");
  CHECK(BodyIs(*doc, "<w:p><w:r><w:t xml:space=\"preserve\">The </w:t></w:r><w:commentRangeStart w:id=\"0\"/>"
                     "<w:r><w:t xml:space=\"preserve\">buyer</w:t></w:r><w:commentRangeEnd w:id=\"0\"/><w:r><w:commentReference w:id=\"0\"/></w:r>"
                     "<w:r><w:t xml:space=\"preserve\"> pays.</w:t></w:r></w:p>"));
  auto all = doc->Comments();
  CHECK(all.size() == 1);
  if (all.size() != 1) return;
  CHECK(all[0].author == "Ann Lee" && all[0].initials == "AL" && all[0].date == "2026-11-05T10:00:00Z");
  CHECK(all[0].start == P(p, 4) && all[0].end == P(p, 9) && all[0].reference == P(p, 9));
  CHECK(edit::CommentText(*doc, all[0]) == u"Who exactly?" && !all[0].paraId.empty() && !all[0].done);
  CHECK(Consistent(*doc, f));
  const std::string first(all[0].id);

  // A reply: about the same text, its reference after the first one's.
  ed.SetAuthor("Bob");
  CHECK(edit::ReplyToComment(ed, first, u"The company.\nNot its owner."));
  all = doc->Comments();
  CHECK(all.size() == 2);
  const CommentInfo* reply = all.size() == 2 ? &all[1] : nullptr;
  CHECK(reply && reply->parentId == first && reply->start == P(p, 4) && reply->end == P(p, 9) &&
        reply->reference == P(p, 10));
  CHECK(reply && edit::CommentText(*doc, *reply) == u"The company.\nNot its owner.");
  CHECK(Consistent(*doc, f));
  const std::string second = reply ? std::string(reply->id) : std::string();

  CHECK(edit::EditComment(ed, second, u"The company itself."));
  CHECK(edit::CommentText(*doc, *CommentById(doc->Comments(), second)) == u"The company itself.");
  CHECK(edit::ResolveComment(ed, second, true));  // the thread
  all = doc->Comments();
  CHECK(CommentById(all, first)->done && CommentById(all, second)->done && CommentById(all, second)->parentId == first);

  // Saved: the parts Word reads, with the reply and the resolved state.
  auto saved = SavedAndRead(*doc, ed);
  CHECK(saved);
  if (saved) {
    auto back = saved->Comments();
    CHECK(back.size() == 2 && CommentById(back, second) && CommentById(back, second)->parentId == first &&
          CommentById(back, first)->done);
    const std::string ids = PartXml(*saved, "/word/commentsIds.xml"), ext = PartXml(*saved, "/word/commentsExtensible.xml");
    CHECK(ids.find("w16cid:durableId=") != std::string::npos && ext.find("w16cex:dateUtc=") != std::string::npos);
    CHECK(PartXml(*saved, "/word/commentsExtended.xml").find("w15:done=\"1\"") != std::string::npos);
  }

  // Deleting the first takes its reply and every mark.
  CHECK(edit::DeleteComment(ed, first));
  CHECK(doc->Comments().empty() && doc->Para(p).text == u"The buyer pays.");
  CHECK(Consistent(*doc, f));
  auto none = SavedAndRead(*doc, ed);
  CHECK(none && none->Comments().empty());
  // Undo takes everything back, one step each.
  CHECK(ed.Undo() && doc->Comments().size() == 2 && doc->Para(p).text == u"The buyer\x05\x05 pays.");
  CHECK(ed.Undo() && !CommentById(doc->Comments(), first)->done);
  CHECK(ed.Undo() && ed.Undo() && doc->Comments().size() == 1);
  CHECK(ed.Undo() && doc->Comments().empty() && doc->Para(p).text == u"The buyer pays.");
  CHECK(!ed.CanUndo());
  CHECK(ed.Redo() && ed.Redo() && doc->Comments().size() == 2 && doc->Comments()[1].start == P(p, 4));
  while (ed.CanUndo()) ed.Undo();
  CHECK(BodyIs(*doc, "<w:p><w:r><w:t xml:space=\"preserve\">The buyer pays.</w:t></w:r></w:p>"));
}

TEST(Comments_AtAPointAndTracked) {
  Tracked t("<w:p><w:r><w:t>ab</w:t></w:r></w:p>");
  CHECK(t.ed);
  if (!t.ed) return;
  Editor& ed = *t.ed;
  const uint32_t p = Main(*t.doc)[0];
  // A comment with nothing selected; its text isn't tracked, nor are its marks.
  CHECK(edit::AddComment(ed, P(p, 1), P(p, 1), u"here"));
  CHECK(t.doc->Revisions(t.doc->MainStory()).empty());
  const auto all = t.doc->Comments();
  CHECK(all.size() == 1 && all[0].start == P(p, 1) && all[0].end == P(p, 1) && all[0].reference == P(p, 1));
  CHECK(all.size() == 1 && t.doc->Revisions(all[0].story).empty());
  CHECK(BodyIs(*t.doc, "<w:p><w:r><w:t>a</w:t></w:r><w:commentRangeStart w:id=\"0\"/><w:commentRangeEnd w:id=\"0\"/>"
                       "<w:r><w:commentReference w:id=\"0\"/></w:r><w:r><w:t>b</w:t></w:r></w:p>"));
  // Its mark can't be deleted with the text around it; the comment goes from the pane.
  CHECK(!ed.Delete(P(p, 0), P(p, 3)));
  CHECK(Consistent(*t.doc, t.f));
}

TEST(Comments_RefusedLeavesNothing) {
  // Torture finding 31: a comment refused where its marks go leaves no comments part, no comment
  // paragraph and no undo step.
  Fixture f;
  f.body = "<w:p><w:r><w:t>ab</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  const uint32_t p = Main(*doc)[0];
  const size_t paras = doc->Paragraphs().size();
  CHECK(!edit::AddComment(ed, P(p, 1), P(p, 9), u"too far"));
  CHECK(!doc->FindPart(fw::doc::PartKind::Comments) && doc->Paragraphs().size() == paras && !ed.CanUndo());
  CHECK(edit::AddComment(ed, P(p, 1), P(p, 2), u"fine") && doc->FindPart(fw::doc::PartKind::Comments));
  CHECK(Consistent(*doc, f));
}

namespace {

// Every revision record's w:id in the XML, which must be unique in a document.
bool IdsUnique(const std::string& xml) {
  std::vector<std::string> ids;
  for (const char* tag : {"<w:ins ", "<w:del ", "<w:rPrChange ", "<w:pPrChange "})
    for (size_t at = xml.find(tag); at != std::string::npos; at = xml.find(tag, at + 1)) {
      const size_t id = xml.find("w:id=\"", at), end = xml.find('>', at);
      if (id == std::string::npos || id > end) continue;
      ids.push_back(xml.substr(id + 6, xml.find('"', id + 6) - id - 6));
    }
  std::sort(ids.begin(), ids.end());
  return std::adjacent_find(ids.begin(), ids.end()) == ids.end();
}

}  // namespace

TEST(Review_CutRecordsGetIdsOfTheirOwn) {
  // Torture finding 38: tracked formatting, then a tracked deletion over part of it, cuts the run
  // with its w:rPrChange in two; each half has an id of its own.
  Tracked t("<w:p><w:r><w:t>abcdefghij</w:t></w:r></w:p>");
  REQUIRE(t.ed);
  Editor& ed = *t.ed;
  const uint32_t p = Main(*t.doc)[0];
  REQUIRE(ed.SetRunProps(P(p, 1), P(p, 5), edit::PropEdit().Set("color", {{"val", "1F4E79"}})));
  REQUIRE(ed.Delete(P(p, 2), P(p, 8)));
  const std::string xml = MainXml(*t.doc);
  CHECK(xml.find("rPrChange") != std::string::npos && xml.find("delText") != std::string::npos);
  CHECK(IdsUnique(xml));
  CHECK(Consistent(*t.doc, t.f));
}

TEST(Review_FieldInsertedWhileTracking) {
  // Torture finding 39: a field typed while tracking goes round an insertion of its own
  // (w:fldSimple/w:ins), never inside the author's insertion; rejecting it takes the field away.
  Tracked t("<w:p><w:r><w:t>x</w:t></w:r></w:p>");
  REQUIRE(t.ed);
  Editor& ed = *t.ed;
  const uint32_t p = Main(*t.doc)[0];
  auto typed = ed.InsertText(P(p, 1), u"Page ");
  REQUIRE(typed);
  REQUIRE(edit::InsertField(ed, typed.caret, " PAGE ", u"1", u"Page Number"));
  const std::string xml = MainXml(*t.doc);
  CHECK(xml.find("<w:fldSimple w:instr=\" PAGE \"><w:ins ") != std::string::npos);
  CHECK(xml.find("</w:fldSimple></w:ins>") == std::string::npos);
  CHECK(IdsUnique(xml) && Consistent(*t.doc, t.f));
  CHECK(SavedAndRead(*t.doc, ed) != nullptr);
  // Rejected: the text and the field go; accepted, the field stays, its text no longer an insertion.
  REQUIRE(ed.ReviewAll(false));
  CHECK(t.doc->Para(p).text == u"x" && MainXml(*t.doc).find("fldSimple") == std::string::npos);
  CHECK(ed.Undo() && ed.ReviewAll(true));
  CHECK(t.doc->Para(p).text == u"xPage 1" && MainXml(*t.doc).find("<w:fldSimple") != std::string::npos);
  CHECK(t.doc->Revisions(t.doc->MainStory()).empty() && Consistent(*t.doc, t.f));
}

TEST(Comments_InANote) {
  // Torture finding 40: Word keeps comments to the main text (its validator refuses one in a note),
  // so a comment in a footnote is refused, and leaves nothing.
  Fixture f;
  f.body = "<w:p><w:r><w:t>a</w:t></w:r><w:r><w:footnoteReference w:id=\"1\"/></w:r></w:p>";
  f.footnotes = "<w:footnote w:type=\"separator\" w:id=\"-1\"><w:p><w:r><w:separator/></w:r></w:p></w:footnote>"
                "<w:footnote w:id=\"1\"><w:p><w:r><w:footnoteRef/><w:t xml:space=\"preserve\"> Note text</w:t></w:r></w:p></w:footnote>";
  auto doc = LoadOk(f);
  REQUIRE(doc);
  Editor ed(*doc);
  const uint32_t note = doc->StoryParagraphs(doc->NoteStory(StoryKind::Footnote, "1")).at(0);
  const size_t paras = doc->Paragraphs().size();
  CHECK(!edit::AddComment(ed, P(note, 2), P(note, 6), u"In the note"));
  CHECK(doc->Comments().empty() && !doc->FindPart(PartKind::Comments) && doc->Paragraphs().size() == paras);
  CHECK(!ed.CanUndo());
  // In the main text, it's fine.
  CHECK(edit::AddComment(ed, P(Main(*doc)[0], 0), P(Main(*doc)[0], 1), u"Here") && doc->Comments().size() == 1);
}
