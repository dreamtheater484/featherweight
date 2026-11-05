#include "TestHarness.h"

#include <string>
#include <vector>

#include "DocTestUtil.h"
#include "doc/edit/Editor.h"
#include "doc/model/Utf.h"

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
