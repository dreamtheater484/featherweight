// Comments on slides (P3, p3-review; src/slide/edit/Comments.h): PowerPoint's modern and legacy
// comments read, changed through the edit layer with undo and redo, written as PowerPoint writes them,
// and kept on save.
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "PkgTestUtil.h"
#include "TestHarness.h"
#include "pkg/Check.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "slide/Presentation.h"
#include "slide/edit/Comments.h"
#include "slide/edit/Edit.h"
#include "slide/edit/Save.h"

using namespace fw::slide;
using namespace fw::slide::edit;

#define REQUIRE(cond) do { if (!(cond)) { test::Fail(__FILE__, __LINE__, #cond); return; } } while (0)

namespace {

std::filesystem::path Generated() {
  return std::filesystem::path(__FILE__).parent_path().parent_path() / "office" / "generated";
}

std::vector<uint8_t> FileBytes(const std::filesystem::path& p) {
  fw::pkg::Status st;
  auto snap = fw::pkg::Snapshot::Open(p.wstring(), {}, st);
  if (!snap) return {};
  auto span = snap->Bytes();
  return std::vector<uint8_t>(span.begin(), span.end());
}

std::unique_ptr<Presentation> OpenBytes(std::vector<uint8_t> bytes) {
  fw::pkg::Status st;
  auto pk = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(std::move(bytes)), fw::pkg::Limits::Default(), st);
  if (!pk) return nullptr;
  return Read(std::move(pk), st);
}

std::string PartText(const Editor& ed, std::string_view part) {
  std::vector<uint8_t> b;
  ed.ReadPart(part, b);
  return std::string(b.begin(), b.end());
}

std::vector<uint8_t> Bytes(std::string_view s) { return std::vector<uint8_t>(s.begin(), s.end()); }

std::vector<uint8_t> Saved(Editor& ed) {
  std::vector<uint8_t> bytes;
  CHECK(SaveToBytes(ed, bytes).Ok());
  return bytes;
}

// The saved package passes the checker (references, relationships, content types).
void CheckSaved(const std::vector<uint8_t>& bytes) {
  fw::pkg::Status st;
  auto pk = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(std::vector<uint8_t>(bytes)), fw::pkg::Limits::Default(), st);
  REQUIRE(pk != nullptr);
  const fw::pkg::CheckReport report = fw::pkg::CheckPackage(*pk);
  for (const std::string& e : report.errors) std::printf("  check: %s\n", e.c_str());
  CHECK(report.Ok());
}

constexpr char kNs[] =
    " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
    " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
    " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"";

// simple.pptx with legacy comments on its first slide, as PowerPoint 2013-2019 wrote them: Ann's
// thread (idx 1) with Bo's reply (threadingInfo), and Bo's own comment.
std::vector<uint8_t> LegacyDeck() {
  auto deck = OpenBytes(FileBytes(Generated() / "simple.pptx"));
  CHECK(deck != nullptr);
  if (!deck) return {};
  Editor ed(*deck);
  const std::string authors = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<p:cmAuthorLst") +
                              kNs + "><p:cmAuthor id=\"1\" name=\"Ann Lee\" initials=\"AL\" lastIdx=\"1\" clrIdx=\"0\"/>"
                                    "<p:cmAuthor id=\"2\" name=\"Bo Ek\" initials=\"BE\" lastIdx=\"2\" clrIdx=\"1\"/></p:cmAuthorLst>";
  const std::string comments =
      std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<p:cmLst") + kNs +
      "><p:cm authorId=\"1\" dt=\"2019-05-02T10:00:00.000\" idx=\"1\"><p:pos x=\"800\" y=\"400\"/><p:text>Check the title</p:text></p:cm>"
      "<p:cm authorId=\"2\" dt=\"2019-05-02T11:00:00.000\" idx=\"1\"><p:pos x=\"800\" y=\"400\"/><p:text>Done</p:text>"
      "<p:extLst><p:ext uri=\"{C676402C-5697-4E1C-873F-D02D1690AC5C}\"><p15:threadingInfo "
      "xmlns:p15=\"http://schemas.microsoft.com/office/powerpoint/2012/main\" timeZoneBias=\"-60\"><p15:parentCm authorId=\"1\" "
      "idx=\"1\"/></p15:threadingInfo></p:ext></p:extLst></p:cm>"
      "<p:cm authorId=\"2\" dt=\"2019-05-03T09:00:00.000\" idx=\"2\"><p:pos x=\"2400\" y=\"1600\"/><p:text>Another</p:text></p:cm></p:cmLst>";
  CHECK(ed.Run(u"Test", [&] {
    CHECK(!ed.AddPart(ed.Deck().PresentationPart(),
                      "http://schemas.openxmlformats.org/officeDocument/2006/relationships/commentAuthors",
                      "/ppt/commentAuthors.xml",
                      "application/vnd.openxmlformats-officedocument.presentationml.commentAuthors+xml", Bytes(authors))
                .empty());
    CHECK(!ed.AddPart(ed.SlidePart(0), "http://schemas.openxmlformats.org/officeDocument/2006/relationships/comments",
                      "/ppt/comments/comment1.xml", "application/vnd.openxmlformats-officedocument.presentationml.comments+xml",
                      Bytes(comments))
               .empty());
    return Result{};
  }));
  return Saved(ed);
}

struct Listener {
  std::vector<Changes> seen;
  bool Comments(size_t slide) const {
    for (const Changes& c : seen)
      for (const SlideChange& s : c.slides)
        if (s.index == static_cast<int>(slide) && (s.what & kComments)) return true;
    return false;
  }
};

}  // namespace

TEST(SlideReview_LegacyCommentsAreRead) {
  auto deck = OpenBytes(LegacyDeck());
  REQUIRE(deck != nullptr);
  Editor ed(*deck);
  const std::vector<Comment> c = SlideComments(ed, 0);
  REQUIRE(c.size() == 3);
  CHECK(c[0].legacy && c[0].parent.empty());
  CHECK(c[0].author == L"Ann Lee" && c[0].initials == L"AL" && c[0].text == L"Check the title");
  CHECK(c[0].hasPos && c[0].pos.x == 100 && c[0].pos.y == 50);  // master units, 8 to the point
  CHECK(c[1].parent == c[0].id && c[1].author == L"Bo Ek" && c[1].text == L"Done");
  CHECK(c[2].parent.empty() && c[2].text == L"Another" && c[2].pos.x == 300);
  CHECK(c[0].created == "2019-05-02T10:00:00.000" && !c[0].utc);
  CHECK(SlideComments(ed, 1).empty());
  const std::vector<size_t> counts = CommentCounts(ed);
  CHECK(counts.size() == deck->SlideCount() && counts[0] == 3);
}

TEST(SlideReview_CommentsAreKeptOnSave) {
  const std::vector<uint8_t> file = LegacyDeck();
  auto deck = OpenBytes(file);
  REQUIRE(deck != nullptr);
  Editor ed(*deck);
  const std::string before = PartText(ed, "/ppt/comments/comment1.xml");
  // An edit elsewhere: the comments part is written back as it was.
  CHECK(ed.SetText({0, deck->GetSlide(0)->shapes[0].id}, L"Retitled"));
  auto back = OpenBytes(Saved(ed));
  REQUIRE(back != nullptr);
  Editor again(*back);
  CHECK(PartText(again, "/ppt/comments/comment1.xml") == before);
  CHECK(SlideComments(again, 0).size() == 3);
}

TEST(SlideReview_NewCommentsAreModern) {
  auto deck = OpenBytes(FileBytes(Generated() / "simple.pptx"));
  REQUIRE(deck != nullptr);
  Editor ed(*deck);
  Listener l;
  ed.OnChange([&l](const Changes& c) { l.seen.push_back(c); });
  CommentAuthor ann{L"Ann Lee", L""};
  std::string id;
  CHECK(!AddComment(ed, 0, {100, 50}, L"   ", ann));  // no text: refused
  CHECK(AddComment(ed, 0, {100, 50}, L"First line\nsecond line", ann, &id));
  CHECK(l.Comments(0));
  CHECK(id.size() == 38 && id.front() == '{');
  CHECK(ed.UndoName() == u"New Comment");
  // As PowerPoint writes it: named by the slide id and creation id, linked from the slide's extLst.
  const std::string slide = PartText(ed, ed.SlidePart(0));
  CHECK(slide.find("p188:commentRel") != std::string::npos);
  CHECK(slide.find("p14:creationId") != std::string::npos);
  const fw::pkg::RelationshipSet* rels = deck->RelationshipsOf(ed.SlidePart(0));
  std::string part;
  for (const auto& r : rels->items)
    if (r.type == "http://schemas.microsoft.com/office/2018/10/relationships/comments") part = r.targetPart;
  CHECK(part.starts_with("/ppt/comments/modernComment_") && part.ends_with(".xml"));
  const std::string xml = PartText(ed, part);
  CHECK(xml.find("<p188:cm id=\"" + id + "\"") != std::string::npos);
  CHECK(xml.find("<pc:sldMkLst") != std::string::npos && xml.find("<p188:pos x=\"1270000\" y=\"635000\"/>") != std::string::npos);
  CHECK(PartText(ed, "/ppt/authors.xml").find("name=\"Ann Lee\" initials=\"AL\" userId=\"Ann Lee\" providerId=\"None\"") !=
        std::string::npos);
  std::vector<Comment> c = SlideComments(ed, 0);
  REQUIRE(c.size() == 1);
  CHECK(c[0].text == L"First line\nsecond line" && c[0].author == L"Ann Lee" && !c[0].legacy && c[0].utc);
  CHECK(c[0].pos.x == 100 && c[0].pos.y == 50);
  // A reply, an edit, resolved, moved: one step each.
  std::string reply;
  CHECK(ReplyToComment(ed, 0, id, L"Agreed", {L"Bo Ek", L""}, &reply));
  CHECK(EditComment(ed, 0, id, L"Only line"));
  CHECK(ResolveComment(ed, 0, reply, true));  // a reply resolves its thread
  CHECK(MoveComment(ed, 0, id, {200, 80}));
  c = SlideComments(ed, 0);
  REQUIRE(c.size() == 2);
  CHECK(c[0].text == L"Only line" && c[0].resolved && c[0].pos.x == 200);
  CHECK(c[1].id == reply && c[1].parent == id && c[1].author == L"Bo Ek" && c[1].initials == L"BE");
  CHECK(ed.UndoNames().size() == 5);
  // Saved and read back.
  const std::vector<uint8_t> bytes = Saved(ed);
  CheckSaved(bytes);
  auto back = OpenBytes(bytes);
  REQUIRE(back != nullptr);
  Editor again(*back);
  const std::vector<Comment> b = SlideComments(again, 0);
  REQUIRE(b.size() == 2);
  CHECK(b[0].text == L"Only line" && b[0].resolved && b[1].text == L"Agreed");
  // Undone to the start: no comments part, no authors part, no link left on the slide.
  CHECK(ed.Undo(5));
  CHECK(SlideComments(ed, 0).empty());
  CHECK(PartText(ed, ed.SlidePart(0)).find("commentRel") == std::string::npos);
  CHECK(PartText(ed, "/ppt/authors.xml").empty());
  CHECK(ed.Redo(5));
  CHECK(SlideComments(ed, 0).size() == 2);
}

TEST(SlideReview_DeletingTheLastCommentRemovesThePart) {
  auto deck = OpenBytes(FileBytes(Generated() / "simple.pptx"));
  REQUIRE(deck != nullptr);
  Editor ed(*deck);
  CommentAuthor ann{L"Ann Lee", L"AL"};
  std::string a, b, r;
  CHECK(AddComment(ed, 0, {10, 10}, L"One", ann, &a));
  CHECK(AddComment(ed, 0, {20, 20}, L"Two", ann, &b));
  CHECK(ReplyToComment(ed, 0, a, L"Re one", ann, &r));
  CHECK(DeleteComment(ed, 0, r));  // a reply alone
  CHECK(SlideComments(ed, 0).size() == 2);
  CHECK(PartText(ed, ed.SlidePart(0)).find("commentRel") != std::string::npos);
  CHECK(DeleteComment(ed, 0, a));
  CHECK(DeleteComment(ed, 0, b));
  CHECK(SlideComments(ed, 0).empty());
  CHECK(PartText(ed, ed.SlidePart(0)).find("commentRel") == std::string::npos);
  CHECK(!DeleteComment(ed, 0, b));  // gone already
  const std::vector<uint8_t> bytes = Saved(ed);
  CheckSaved(bytes);
  const std::string zip(bytes.begin(), bytes.end());
  CHECK(zip.find("modernComment_") == std::string::npos);
}

TEST(SlideReview_LegacyCommentsChangeInTheirFormat) {
  auto deck = OpenBytes(LegacyDeck());
  REQUIRE(deck != nullptr);
  Editor ed(*deck);
  std::vector<Comment> c = SlideComments(ed, 0);
  REQUIRE(c.size() == 3);
  const std::string thread = c[0].id, other = c[2].id;
  std::string reply;
  CHECK(ReplyToComment(ed, 0, c[1].id, L"Thanks", {L"Ann Lee", L""}, &reply));  // to a reply: joins its thread
  CHECK(reply == "legacy:1:2");  // Ann's next index
  CHECK(PartText(ed, "/ppt/commentAuthors.xml").find("lastIdx=\"2\"") != std::string::npos);
  CHECK(EditComment(ed, 0, other, L"Changed"));
  CHECK(!ResolveComment(ed, 0, thread, true));  // the older format has no resolved state
  c = SlideComments(ed, 0);
  REQUIRE(c.size() == 4);
  CHECK(c[2].id == reply && c[2].parent == thread && c[2].text == L"Thanks");
  CHECK(c[3].text == L"Changed");
  // A new thread beside them is modern; the slide then has both.
  std::string modern;
  CHECK(AddComment(ed, 0, {5, 5}, L"Modern", {L"Cy Do", L""}, &modern));
  c = SlideComments(ed, 0);
  REQUIRE(c.size() == 5);
  CHECK(c[4].id == modern && !c[4].legacy);
  // The thread goes with its replies; the last legacy comment takes the part with it.
  CHECK(DeleteComment(ed, 0, thread));
  c = SlideComments(ed, 0);
  REQUIRE(c.size() == 2);
  CHECK(DeleteComment(ed, 0, other));
  CHECK(PartText(ed, "/ppt/comments/comment1.xml").empty());
  CheckSaved(Saved(ed));
  CHECK(DeleteAllComments(ed, {}));
  CHECK(SlideComments(ed, 0).empty());
  CHECK(!DeleteAllComments(ed, {}));
  CHECK(ed.Undo(2));
  CHECK(SlideComments(ed, 0).size() == 2);
}

TEST(SlideReview_DuplicatedSlidesLeaveCommentsBehind) {
  auto deck = OpenBytes(FileBytes(Generated() / "simple.pptx"));
  REQUIRE(deck != nullptr);
  Editor ed(*deck);
  CHECK(AddComment(ed, 0, {10, 10}, L"Only here", {L"Ann Lee", L""}));
  CHECK(ed.DuplicateSlides({0}));
  CHECK(SlideComments(ed, 0).size() == 1);
  CHECK(SlideComments(ed, 1).empty());
  const std::string copy = PartText(ed, ed.SlidePart(1));
  CHECK(copy.find("commentRel") == std::string::npos);
  CHECK(copy.find("r:id=\"\"") == std::string::npos);
  CheckSaved(Saved(ed));
}
