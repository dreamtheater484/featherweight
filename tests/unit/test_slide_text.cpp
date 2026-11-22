// Text editing on slides (P2, p2-text; src/slide/view/Text*): the working copy's edits keep
// DrawingML as PowerPoint writes it, typing coalesces into one undo step, the caret moves by
// characters, words, lines and paragraphs, empty placeholders take typing, and autofit shrinks text
// that overflows. The edit layer is a fake here (TextModel): it keeps what each step committed.
#include <windows.h>

#include <filesystem>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "PkgTestUtil.h"
#include "TestHarness.h"
#include "draw/Xml.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "slide/Presentation.h"
#include "slide/render/SlideRender.h"
#include "slide/view/TextClip.h"
#include "slide/view/TextDoc.h"
#include "slide/view/TextFind.h"
#include "slide/view/TextFormat.h"
#include "slide/view/TextModel.h"
#include "slide/view/TextSession.h"
#include "slide/view/TextSpell.h"
#include "text/Fonts.h"

using namespace fw::slide;
using namespace fw::slide::view;

#define REQUIRE(cond)                                             \
  do {                                                            \
    const bool required_ = static_cast<bool>(cond);               \
    if (!required_) return test::Fail(__FILE__, __LINE__, #cond); \
  } while (0)

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
  return pk ? Read(std::move(pk), st) : nullptr;
}

// simple.pptx with some parts replaced.
std::unique_ptr<Presentation> Modified(const std::map<std::string, std::string>& replace) {
  fw::pkg::Status s;
  auto pk = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(FileBytes(Generated() / "simple.pptx")),
                                   fw::pkg::Limits::Default(), s);
  if (!pk) return nullptr;
  std::vector<std::pair<std::string, std::string>> entries;
  for (const fw::pkg::Part& part : pk->Parts()) {
    const std::string name = part.name.substr(1);
    auto it = replace.find(name);
    if (it != replace.end()) {
      entries.emplace_back(name, it->second);
      continue;
    }
    std::vector<uint8_t> bytes;
    pk->ReadPart(part.name, bytes);
    entries.emplace_back(name, test::Text(bytes));
  }
  return OpenBytes(test::MakeZip(entries));
}

constexpr const char* kNs =
    " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
    " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\""
    " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"";

// Slide 1 with these shapes, on layout `layout` (1: Title Slide, 2: Title and Content).
std::map<std::string, std::string> Slide1(const std::string& shapes, int layout = 1) {
  return {{"ppt/slides/slide1.xml",
           std::string("<p:sld") + kNs +
               "><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>" +
               shapes + "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>"},
          {"ppt/slides/_rels/slide1.xml.rels",
           "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
           "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\""
           " Target=\"../slideLayouts/slideLayout" +
               std::to_string(layout) + ".xml\"/></Relationships>"}};
}

std::string PhShape(int id, const std::string& ph, const std::string& paragraphs, const std::string& bodyPr = "<a:bodyPr/>") {
  return "<p:sp><p:nvSpPr><p:cNvPr id=\"" + std::to_string(id) + "\" name=\"P" + std::to_string(id) + "\"/><p:cNvSpPr/><p:nvPr>" + ph +
         "</p:nvPr></p:nvSpPr><p:spPr/><p:txBody>" + bodyPr + "<a:lstStyle/>" + paragraphs + "</p:txBody></p:sp>";
}

// A text box at (x, y), w x h points.
std::string TextBox(int id, int x, int y, int w, int h, const std::string& paragraphs, const std::string& bodyPr) {
  return "<p:sp><p:nvSpPr><p:cNvPr id=\"" + std::to_string(id) + "\" name=\"T\"/><p:cNvSpPr txBox=\"1\"/><p:nvPr/></p:nvSpPr>"
         "<p:spPr><a:xfrm><a:off x=\"" + std::to_string(x * 12700) + "\" y=\"" + std::to_string(y * 12700) + "\"/><a:ext cx=\"" +
         std::to_string(w * 12700) + "\" cy=\"" + std::to_string(h * 12700) +
         "\"/></a:xfrm><a:prstGeom prst=\"rect\"/></p:spPr><p:txBody>" + bodyPr + "<a:lstStyle/>" + paragraphs + "</p:txBody></p:sp>";
}

std::string Xml(pugi::xml_node n) {
  std::ostringstream os;
  n.print(os, "", pugi::format_raw);
  return os.str();
}

// The edit layer, faked: each step's text kept, undo going back one.
class FakeModel : public TextModel {
 public:
  explicit FakeModel(Presentation& deck) : deck_(deck) {}
  const SlideBase* Part(const TextTarget& t) override {
    const Slide* s = t.slide < deck_.SlideCount() ? deck_.GetSlide(t.slide) : nullptr;
    if (t.notes) return s ? static_cast<const SlideBase*>(s->notes) : nullptr;
    return s;
  }
  pugi::xml_node Body(const TextTarget& t) override {
    if (!states_.empty()) return current_.first_child();
    const Shape* sh = ShapeOf(t);
    if (!sh) return {};
    if (t.Cell()) {
      const Table* table = deck_.GetTable(*sh, *Part(t));
      const TableCell* c = table ? table->Cell(t.row, t.column) : nullptr;
      return c ? draw::xml::A(c->node, "txBody") : pugi::xml_node();
    }
    return draw::xml::Child(sh->node, draw::xml::kP, "txBody");
  }
  std::unique_ptr<TextBody> Resolve(const TextTarget& t, pugi::xml_node txBody) override {
    const Shape* sh = ShapeOf(t);
    if (!sh) return nullptr;
    if (t.Cell()) {
      const Table* table = deck_.GetTable(*sh, *Part(t));
      const TableCell* c = table ? table->Cell(t.row, t.column) : nullptr;
      return c ? deck_.ResolveCellText(*sh, *c, txBody, *Part(t)) : nullptr;
    }
    return deck_.ResolveTextBody(*sh, txBody, *Part(t));
  }
  bool SetText(const TextTarget&, pugi::xml_node txBody, std::u16string_view step, bool coalesce, std::wstring&) override {
    if (refuse) return false;
    const std::string xml = Xml(txBody);
    if (coalesce && !steps.empty() && steps.back() == step) {
      states_.back() = xml;
    } else {
      steps.emplace_back(step);
      states_.push_back(xml);
    }
    Load(xml);
    return true;
  }
  void BeginGroup(std::u16string_view) override {}
  void EndGroup() override {}
  bool SetShapeHeight(const TextTarget&, float height) override {
    heights.push_back(height);
    return true;
  }
  Presentation* Deck() override { return &deck_; }
  void Undo() {
    steps.pop_back();
    states_.pop_back();
    if (!states_.empty()) Load(states_.back());
    else current_.reset();
  }
  std::string Committed() { return states_.empty() ? std::string() : states_.back(); }

  std::vector<std::u16string> steps;
  std::vector<float> heights;
  bool refuse = false;

 private:
  const Shape* ShapeOf(const TextTarget& t) {
    const SlideBase* part = Part(t);
    return part ? part->FindShape(t.shape) : nullptr;
  }
  void Load(const std::string& xml) {
    current_.reset();
    current_.load_string(xml.c_str(), pugi::parse_default | pugi::parse_ws_pcdata);
  }
  Presentation& deck_;
  pugi::xml_document current_;
  std::vector<std::string> states_;
};

// A deck, a fake edit layer and a session on shape `id` of slide 1.
struct Fixture {
  std::unique_ptr<Presentation> deck;
  std::unique_ptr<FakeModel> model;
  fw::text::Fonts fonts;
  std::unique_ptr<render::SlideRender> slide;
  std::unique_ptr<TextSession> session;

  bool Open(const std::string& shapes, uint32_t id, int layout = 1) {
    deck = Modified(Slide1(shapes, layout));
    if (!deck) return false;
    model = std::make_unique<FakeModel>(*deck);
    slide = render::SlideRender::Make(*deck, *deck->GetSlide(0), fonts);
    TextTarget t;
    t.shape = id;
    render::SlideRender::TextFrame frame;
    if (!slide->FindText(id, -1, -1, frame)) return false;
    session = std::make_unique<TextSession>(*model, t, fonts);
    return session->Start(frame, {});
  }
};

TextDoc::Resolver ResolverFor(Presentation& deck, uint32_t id) {
  return [&deck, id](pugi::xml_node body) {
    const Slide* s = deck.GetSlide(0);
    const Shape* sh = s->FindShape(id);
    return sh ? deck.ResolveTextBody(*sh, body, *s) : nullptr;
  };
}

pugi::xml_node ModelBody(Presentation& deck, uint32_t id) {
  return draw::xml::Child(deck.GetSlide(0)->FindShape(id)->node, draw::xml::kP, "txBody");
}

}  // namespace

TEST(SlideText_InsertJoinsTheRunBefore) {
  auto deck = Modified(Slide1(PhShape(2, "<p:ph type=\"ctrTitle\"/>",
                                      "<a:p><a:r><a:rPr lang=\"en-US\"/><a:t>Hello </a:t></a:r>"
                                      "<a:r><a:rPr lang=\"en-US\" b=\"1\"/><a:t>world</a:t></a:r></a:p>")));
  REQUIRE(deck != nullptr);
  TextDoc doc(ModelBody(*deck, 2), ResolverFor(*deck, 2));
  CHECK(doc.Text(0) == L"Hello world");
  TextPos end = doc.Insert({0, 5}, L",");
  CHECK(doc.Text(0) == L"Hello, world");
  CHECK(end == (TextPos{0, 6}));
  // The comma took the plain run's look; the bold run is still whole.
  CHECK_EQ(doc.Resolved().paragraphs[0].runs.size(), size_t{2});
  CHECK(!doc.Resolved().paragraphs[0].runs[0].props.bold);
  doc.Insert(doc.End(), L"!");
  CHECK(doc.Text(0) == L"Hello, world!");
  CHECK_EQ(doc.Resolved().paragraphs[0].runs.size(), size_t{2});
  CHECK(doc.Resolved().paragraphs[0].runs[1].props.bold);
  // Deleting the comma, then across the runs.
  doc.Delete({0, 5}, {0, 6});
  CHECK(doc.Text(0) == L"Hello world!");
  doc.Delete({0, 4}, {0, 7});
  CHECK(doc.Text(0) == L"Hellorld!");
  // Nothing left of a run is left in the XML.
  doc.Delete({0, 0}, {0, 4});
  CHECK(doc.Text(0) == L"orld!");
  CHECK_EQ(doc.Resolved().paragraphs[0].runs.size(), size_t{1});
  CHECK(Xml(doc.Body()).find("<a:t></a:t>") == std::string::npos);
}

TEST(SlideText_ParagraphsSplitAndJoin) {
  auto deck = Modified(Slide1(PhShape(3, "<p:ph idx=\"1\"/>",
                                      "<a:p><a:pPr lvl=\"1\"/><a:r><a:rPr lang=\"en-US\" sz=\"2000\"/><a:t>First second</a:t></a:r>"
                                      "<a:endParaRPr lang=\"en-US\" sz=\"2000\"/></a:p>"),
                              2));
  REQUIRE(deck != nullptr);
  TextDoc doc(ModelBody(*deck, 3), ResolverFor(*deck, 3));
  const TextPos at = doc.Split({0, 6});
  CHECK(at == (TextPos{1, 0}));
  REQUIRE(doc.ParagraphCount() == 2);
  CHECK(doc.Text(0) == L"First ");
  CHECK(doc.Text(1) == L"second");
  // Both keep the level, and the new paragraph keeps the run's size.
  CHECK_EQ(doc.Resolved().paragraphs[0].props.level, 1);
  CHECK_EQ(doc.Resolved().paragraphs[1].props.level, 1);
  CHECK_NEAR(doc.Resolved().paragraphs[1].runs[0].props.size, 20, 0.01);
  // The first paragraph's mark came from its last run, after the runs (schema order).
  const std::string xml = Xml(doc.Body());
  CHECK(xml.find("<a:t>First </a:t></a:r><a:endParaRPr") != std::string::npos);
  // Joined again.
  doc.Delete({0, 6}, {1, 0});
  CHECK_EQ(doc.ParagraphCount(), 1u);
  CHECK(doc.Text(0) == L"First second");
  CHECK_EQ(doc.Resolved().paragraphs[0].runs.size(), size_t{1});
  // Typed paragraph and line breaks.
  doc.Insert({0, 5}, L"\r\nA\u000BB");
  CHECK_EQ(doc.ParagraphCount(), 2u);
  CHECK(doc.Text(1) == L"A\u000BB second");
  CHECK(Xml(doc.Body()).find("<a:br>") != std::string::npos);
}

TEST(SlideText_RunFormattingSplitsAndJoins) {
  auto deck = Modified(Slide1(PhShape(2, "<p:ph type=\"ctrTitle\"/>", "<a:p><a:r><a:rPr lang=\"en-US\"/><a:t>abcdef</a:t></a:r></a:p>")));
  REQUIRE(deck != nullptr);
  TextDoc doc(ModelBody(*deck, 2), ResolverFor(*deck, 2));
  CHECK(doc.EditRuns({0, 2}, {0, 4}, [](pugi::xml_node rPr) { rPr.append_attribute("b") = "1"; }));
  REQUIRE(doc.Resolved().paragraphs[0].runs.size() == 3);
  CHECK(doc.Resolved().paragraphs[0].runs[1].text == L"cd");
  CHECK(doc.Resolved().paragraphs[0].runs[1].props.bold);
  CHECK(!doc.Resolved().paragraphs[0].runs[0].props.bold);
  // Taken off again, the runs join: the XML is as it was.
  doc.EditRuns({0, 2}, {0, 4}, [](pugi::xml_node rPr) { rPr.remove_attribute("b"); });
  CHECK_EQ(doc.Resolved().paragraphs[0].runs.size(), size_t{1});
  // A color goes in its place in a:rPr (after a:ln, before a:latin).
  doc.EditRuns({0, 0}, {0, 6}, [&](pugi::xml_node rPr) {
    doc.RunChild(rPr, "latin", true).append_attribute("typeface") = "Arial";
    doc.RunChild(rPr, "ln", true);
    TextDoc::RemoveFills(rPr);
    doc.RunChild(rPr, "solidFill", true).append_child("a:srgbClr").append_attribute("val") = "FF0000";
  });
  const std::string xml = Xml(doc.Body());
  CHECK(xml.find("<a:ln /><a:solidFill><a:srgbClr val=\"FF0000\" /></a:solidFill><a:latin typeface=\"Arial\" />") !=
            std::string::npos ||
        xml.find("<a:ln/><a:solidFill><a:srgbClr val=\"FF0000\"/></a:solidFill><a:latin typeface=\"Arial\"/>") != std::string::npos);
}

TEST(SlideText_FieldsStayWhole) {
  auto deck = Modified(Slide1(PhShape(2, "<p:ph type=\"ctrTitle\"/>",
                                      "<a:p><a:r><a:t>Slide </a:t></a:r><a:fld id=\"{B5C3E8A4-0000-4000-8000-000000000001}\" "
                                      "type=\"slidenum\"><a:rPr lang=\"en-US\"/><a:t>7</a:t></a:fld><a:r><a:t> end</a:t></a:r></a:p>")));
  REQUIRE(deck != nullptr);
  TextDoc doc(ModelBody(*deck, 2), ResolverFor(*deck, 2));
  // The slide number shows as the slide's.
  CHECK(doc.Text(0) == L"Slide 1 end");
  // Typing inside a field goes after it; deleting part of it deletes it.
  uint32_t s = 0, e = 0;
  CHECK(!doc.InField({0, 6}, s, e));
  doc.Delete({0, 5}, {0, 7});
  CHECK(doc.Text(0) == L"Slide end");
  CHECK(Xml(doc.Body()).find("a:fld") == std::string::npos);
}

TEST(SlideText_TypingIsOneUndoStep) {
  Fixture f;
  REQUIRE(f.Open(PhShape(2, "<p:ph type=\"ctrTitle\"/>", "<a:p><a:r><a:rPr lang=\"en-US\"/><a:t>Title</a:t></a:r></a:p>"), 2));
  f.session->MoveCaret(TextSession::Move::End, false);
  CHECK(f.session->Type(L" o"));
  CHECK(f.session->Type(L"n"));
  CHECK(f.session->Type(L"e"));
  CHECK_EQ(f.model->steps.size(), size_t{1});
  CHECK(f.session->Doc().Text(0) == L"Title one");
  // A caret move ends the burst.
  f.session->MoveCaret(TextSession::Move::Start, false);
  CHECK(f.session->Type(L"A "));
  CHECK_EQ(f.model->steps.size(), size_t{2});
  CHECK(f.model->Committed().find("A Title one") != std::string::npos);
  // Undone, the session reads the model again.
  f.model->Undo();
  f.session->ModelChanged();
  CHECK(f.session->Doc().Text(0) == L"Title one");
  CHECK(f.session->Caret().offset <= 9u);
  // Backspace by word.
  f.session->MoveCaret(TextSession::Move::End, false);
  CHECK(f.session->Backspace(true));
  CHECK(f.session->Doc().Text(0) == L"Title ");
}

TEST(SlideText_CaretMovesByWordsLinesAndParagraphs) {
  Fixture f;
  REQUIRE(f.Open(TextBox(4, 50, 50, 120, 200,
                         "<a:p><a:r><a:rPr lang=\"en-US\" sz=\"1800\"/><a:t>one two three four five six seven eight</a:t></a:r></a:p>"
                         "<a:p><a:r><a:rPr lang=\"en-US\" sz=\"1800\"/><a:t>last</a:t></a:r></a:p>",
                         "<a:bodyPr wrap=\"square\"/>"),
                 4));
  TextSession& s = *f.session;
  s.MoveCaret(TextSession::Move::WordRight, false);
  CHECK(s.Caret() == (TextPos{0, 4}));
  s.MoveCaret(TextSession::Move::WordRight, true);
  CHECK(s.Caret() == (TextPos{0, 8}));
  CHECK(s.Anchor() == (TextPos{0, 4}));
  // The first paragraph wraps in 120 points: End stops at the first line's end, Down goes to the
  // second line.
  s.MoveCaret(TextSession::Move::Start, false);
  s.MoveCaret(TextSession::Move::LineEnd, false);
  const uint32_t lineEnd = s.Caret().offset;
  CHECK(lineEnd > 0 && lineEnd < 39);
  s.MoveCaret(TextSession::Move::Start, false);
  s.MoveCaret(TextSession::Move::Down, false);
  CHECK(s.Caret().para == 0 && s.Caret().offset >= lineEnd - 1);
  s.MoveCaret(TextSession::Move::ParaDown, false);
  CHECK(s.Caret() == (TextPos{1, 0}));
  s.MoveCaret(TextSession::Move::Left, false);
  CHECK(s.Caret() == (TextPos{0, 39}));
  s.MoveCaret(TextSession::Move::Right, false);
  CHECK(s.Caret() == (TextPos{1, 0}));
  // A click where the caret of offset 2 is puts it there.
  s.MoveCaret(TextSession::Move::End, false);
  const TextSession::CaretLine before = s.CaretGeometry();
  s.Select({1, 2}, {1, 2});
  const TextSession::CaretLine at2 = s.CaretGeometry();
  s.MoveCaret(TextSession::Move::End, false);
  s.PointerDown({at2.top.x + 0.2f, (at2.top.y + at2.bottom.y) / 2}, 1, false);
  CHECK(s.Caret() == (TextPos{1, 2}));
  CHECK(before.top.x > at2.top.x);
  // Double-click: the word and its space.
  s.PointerDown({at2.top.x, (at2.top.y + at2.bottom.y) / 2}, 2, false);
  CHECK(s.SelFrom() == (TextPos{1, 0}));
  CHECK(s.SelTo() == (TextPos{1, 4}));
  CHECK(!s.SelectionQuads().empty());
}

TEST(SlideText_EmptyPlaceholderTakesTyping) {
  Fixture f;
  REQUIRE(f.Open(PhShape(2, "<p:ph type=\"ctrTitle\"/>", "<a:p><a:endParaRPr lang=\"en-US\" dirty=\"0\"/></a:p>"), 2));
  CHECK(f.slide->IsEmptyPlaceholder(2));
  CHECK(f.session->Type(L"New title"));
  const std::string xml = f.model->Committed();
  CHECK(xml.find("<a:t>New title</a:t>") != std::string::npos);
  // The text took the mark's language, and the mark stays last.
  CHECK(xml.find("lang=\"en-US\"") != std::string::npos);
  CHECK(xml.find("</a:r><a:endParaRPr") != std::string::npos);
}

TEST(SlideText_TabChangesTheLevel) {
  Fixture f;
  REQUIRE(f.Open(PhShape(3, "<p:ph idx=\"1\"/>", "<a:p><a:r><a:rPr lang=\"en-US\"/><a:t>Point</a:t></a:r></a:p>"), 3, 2));
  CHECK(f.session->Tab(false));
  CHECK_EQ(f.session->Doc().Resolved().paragraphs[0].props.level, 1);
  CHECK(f.model->Committed().find("lvl=\"1\"") != std::string::npos);
  CHECK(f.session->Tab(true));
  CHECK_EQ(f.session->Doc().Resolved().paragraphs[0].props.level, 0);
  // In the middle of the text, a tab character.
  f.session->MoveCaret(TextSession::Move::End, false);
  CHECK(f.session->Tab(false));
  CHECK(f.session->Doc().Text(0) == L"Point\t");
}

TEST(SlideText_ShrinksTextThatOverflows) {
  Fixture f;
  std::string paras;
  for (int i = 0; i < 4; ++i) paras += "<a:p><a:r><a:rPr lang=\"en-US\" sz=\"2400\"/><a:t>Line</a:t></a:r></a:p>";
  REQUIRE(f.Open(TextBox(5, 40, 40, 300, 130, paras, "<a:bodyPr wrap=\"square\"><a:normAutofit/></a:bodyPr>"), 5));
  // Four lines of 24 points fit in 130; eight don't.
  f.session->MoveCaret(TextSession::Move::End, false);
  for (int i = 0; i < 4; ++i) {
    CHECK(f.session->Enter());
    CHECK(f.session->Type(L"More"));
  }
  const std::string xml = f.model->Committed();
  CHECK(xml.find("fontScale=") != std::string::npos);
  CHECK(f.session->Block().ContentHeight() <= 130.5f);
  // Deleted again, the text comes back to full size.
  f.session->Select({4, 0}, f.session->Doc().End());
  CHECK(f.session->Backspace(false));
  CHECK(f.session->Backspace(false));
  CHECK(f.model->Committed().find("fontScale=") == std::string::npos);
}

TEST(SlideText_ResizesTheShapeToItsText) {
  Fixture f;
  REQUIRE(f.Open(TextBox(6, 40, 40, 300, 40, "<a:p><a:r><a:rPr lang=\"en-US\" sz=\"1800\"/><a:t>One</a:t></a:r></a:p>",
                         "<a:bodyPr wrap=\"square\"><a:spAutoFit/></a:bodyPr>"),
                 6));
  f.session->MoveCaret(TextSession::Move::End, false);
  CHECK(f.session->Enter());
  CHECK(f.session->Type(L"Two"));
  REQUIRE(!f.model->heights.empty());
  CHECK(f.model->heights.back() > 50);
  CHECK(f.session->TakeGeometryChanged());
}

TEST(SlideText_RefusedEditsLeaveTheText) {
  Fixture f;
  REQUIRE(f.Open(PhShape(2, "<p:ph type=\"ctrTitle\"/>", "<a:p><a:r><a:t>Kept</a:t></a:r></a:p>"), 2));
  f.model->refuse = true;
  CHECK(!f.session->Type(L"x"));
  CHECK(f.session->Doc().Text(0) == L"Kept");
  CHECK(!f.session->TakeError().empty());
}

TEST(SlideText_TargetUnderAPoint) {
  Fixture f;
  REQUIRE(f.Open(TextBox(4, 100, 100, 200, 50, "<a:p><a:r><a:t>Box</a:t></a:r></a:p>", "<a:bodyPr/>"), 4));
  auto t = TextTargetAt(*f.slide, 0, false, {150, 120});
  REQUIRE(t.has_value());
  CHECK_EQ(t->shape, 4u);
  CHECK(!TextTargetAt(*f.slide, 0, false, {20, 500}).has_value());
}

TEST(SlideText_FormattingTheSelectionAndTheWordAtTheCaret) {
  Fixture f;
  REQUIRE(f.Open(PhShape(2, "<p:ph type=\"ctrTitle\"/>", "<a:p><a:r><a:rPr lang=\"en-US\"/><a:t>alpha beta</a:t></a:r></a:p>"), 2));
  TextSession& s = *f.session;
  s.Select({0, 0}, {0, 2});
  CHECK(format::Bold(s, true));
  CHECK(LookOf(s).bold == std::optional<bool>(true));
  s.Select({0, 0}, {0, 5});
  CHECK(!LookOf(s).bold.has_value());  // mixed
  // The caret inside "beta": the word.
  s.Select({0, 8}, {0, 8});
  CHECK(format::Italic(s, true));
  const std::vector<TextRun>& runs = s.Doc().Resolved().paragraphs[0].runs;
  REQUIRE(runs.size() == 3);
  CHECK(runs[2].text == L"beta");
  CHECK(runs[2].props.italic);
  CHECK(!runs[1].props.italic);
  // At the end of the text: what's typed next, without an undo step of its own.
  const size_t steps = f.model->steps.size();
  s.MoveCaret(TextSession::Move::End, false);
  CHECK(format::Underline(s, true));
  CHECK_EQ(f.model->steps.size(), steps);
  CHECK(LookOf(s).underline == std::optional<bool>(true));
  CHECK(s.Type(L"!"));
  CHECK(s.Doc().Resolved().paragraphs[0].runs.back().text == L"!");
  CHECK(s.Doc().Resolved().paragraphs[0].runs.back().props.underline != fw::slide::Underline::None);
  // Sizes, colors, fonts.
  s.SelectAll();
  CHECK(format::Size(s, 40));
  CHECK(LookOf(s).size == std::optional<float>(40.f));
  CHECK(format::Grow(s, true, false));
  CHECK(LookOf(s).size == std::optional<float>(44.f));
  CHECK(format::Grow(s, false, true));
  CHECK(LookOf(s).size == std::optional<float>(43.f));
  CHECK(format::Color(s, draw::Color{1, 0, 0, 1}));
  CHECK(f.model->Committed().find("<a:srgbClr val=\"FF0000\"") != std::string::npos);
  CHECK(format::Font(s, L"Georgia"));
  CHECK(LookOf(s).font == std::optional<std::wstring>(L"Georgia"));
  CHECK(format::ClearCharacter(s));
  CHECK(LookOf(s).font != std::optional<std::wstring>(L"Georgia"));
  CHECK(f.model->Committed().find("lang=\"en-US\"") != std::string::npos);
}

TEST(SlideText_BulletsNumberingAndLevels) {
  Fixture f;
  REQUIRE(f.Open(TextBox(4, 50, 50, 300, 200,
                         "<a:p><a:r><a:rPr lang=\"en-US\"/><a:t>One</a:t></a:r></a:p><a:p><a:r><a:rPr lang=\"en-US\"/><a:t>Two</a:t></a:r></a:p>",
                         "<a:bodyPr/>"),
                 4));
  TextSession& s = *f.session;
  s.SelectAll();
  CHECK(!LookOf(s).bullets);
  CHECK(format::Bullets(s, true));
  CHECK(LookOf(s).bullets);
  std::string xml = f.model->Committed();
  CHECK(xml.find("<a:pPr marL=\"285750\" indent=\"-285750\"><a:buFont typeface=\"Arial\"") != std::string::npos);
  CHECK(format::Numbering(s, true));
  CHECK(LookOf(s).numbering);
  CHECK_EQ(s.Doc().Resolved().paragraphs[1].number, 2);
  CHECK(format::Bullets(s, false));
  CHECK(!LookOf(s).bullets && !LookOf(s).numbering);
  CHECK(f.model->Committed().find("<a:buNone") != std::string::npos);
  CHECK(format::Align(s, TextAlign::Center));
  CHECK(LookOf(s).align == std::optional<TextAlign>(TextAlign::Center));
  CHECK(format::LineSpacing(s, 1.5f));
  CHECK_NEAR(LookOf(s).lineSpacing, 1.5f, 0.001f);
  CHECK(format::Level(s, true));
  CHECK_EQ(LookOf(s).level, 1);
}

TEST(SlideText_ChangeCaseKeepsTheRuns) {
  Fixture f;
  REQUIRE(f.Open(PhShape(2, "<p:ph type=\"ctrTitle\"/>",
                         "<a:p><a:r><a:rPr lang=\"en-US\"/><a:t>hello </a:t></a:r><a:r><a:rPr lang=\"en-US\" b=\"1\"/><a:t>world</a:t></a:r></a:p>"),
                 2));
  TextSession& s = *f.session;
  s.SelectAll();
  CHECK(format::ChangeCase(s));
  CHECK(s.Doc().Text(0) == L"Hello World");
  CHECK_EQ(s.Doc().Resolved().paragraphs[0].runs.size(), size_t{2});
  CHECK(format::ChangeCase(s));
  CHECK(s.Doc().Text(0) == L"HELLO WORLD");
}

TEST(SlideText_CopyAndPasteKeepTheLook) {
  Fixture f;
  REQUIRE(f.Open(PhShape(2, "<p:ph type=\"ctrTitle\"/>",
                         "<a:p><a:r><a:rPr lang=\"en-US\"/><a:t>Hello </a:t></a:r><a:r><a:rPr lang=\"en-US\" b=\"1\" sz=\"3000\"/><a:t>world</a:t></a:r></a:p>"),
                 2));
  TextSession& s = *f.session;
  s.Select({0, 6}, {0, 11});
  const TextClipboard clip = CopySelection(s, *f.model);
  CHECK(clip.text == L"world");
  CHECK(clip.rtf.find("\\b") != std::string::npos);
  CHECK(clip.html.find("<!--fw-slide-text:") != std::string::npos);
  // Featherweight's own copy pastes whole: bold, 30 points.
  s.Select({0, 0}, {0, 0});
  CHECK(Paste(s, *f.model, clip.html, clip.rtf, clip.text, false));
  CHECK(s.Doc().Text(0) == L"worldHello world");
  CHECK(s.Caret() == (TextPos{0, 5}));
  CHECK(s.Doc().Resolved().paragraphs[0].runs[0].props.bold);
  CHECK_NEAR(s.Doc().Resolved().paragraphs[0].runs[0].props.size, 30, 0.01);
  CHECK_EQ(f.model->steps.back(), std::u16string(u"Paste"));
  // From Word (RTF): the emphasis stays, the size is the destination's.
  s.Select(s.Doc().End(), s.Doc().End());
  CHECK(Paste(s, *f.model, "", clip.rtf, clip.text, false));
  CHECK(s.Doc().Text(0) == L"worldHello worldworld");
  const TextRun& last = s.Doc().Resolved().paragraphs[0].runs.back();
  CHECK(last.props.bold);
  CHECK_NEAR(last.props.size, 30, 0.01);  // the run before it (where it goes) is 30 points too
  // As text: the look of where it goes.
  s.Select({0, 0}, {0, 5});
  CHECK(Paste(s, *f.model, clip.html, clip.rtf, L"x\r\ny", true));
  CHECK_EQ(s.Doc().ParagraphCount(), 2u);
  CHECK(s.Doc().Text(1).starts_with(L"yHello"));
}

TEST(SlideText_FindAndReplaceAcrossTheDeck) {
  Fixture f;
  REQUIRE(f.Open(PhShape(2, "<p:ph type=\"ctrTitle\"/>",
                         "<a:p><a:r><a:rPr lang=\"en-US\"/><a:t>One two one, someone</a:t></a:r></a:p>"),
                 2));
  std::vector<TextMatch> all = FindAll(*f.model, L"one", false, false);
  CHECK_EQ(all.size(), size_t{3});
  std::vector<TextMatch> words = FindAll(*f.model, L"one", false, true);
  REQUIRE(words.size() == 2);
  CHECK(words[0].from == (TextPos{0, 0}));
  CHECK(words[1].from == (TextPos{0, 8}));
  CHECK_EQ(FindAll(*f.model, L"one", true, true).size(), size_t{1});
  CHECK_EQ(ReplaceAll(*f.model, words, L"three"), 2);
  CHECK(f.model->Committed().find("three two three, someone") != std::string::npos);
  CHECK_EQ(f.model->steps.back(), std::u16string(u"Replace All"));
}

TEST(SlideText_SpellingInTheRunsLanguage) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  fw::text::SpellChecker checker;
  if (!checker.Available() || checker.Resolve(L"en-US").empty()) return;  // no checker on this PC
  Fixture f;
  REQUIRE(f.Open(PhShape(2, "<p:ph type=\"ctrTitle\"/>",
                         "<a:p><a:r><a:rPr lang=\"en-US\"/><a:t>A mispeled word </a:t></a:r>"
                         "<a:r><a:rPr lang=\"en-US\" noProof=\"1\"/><a:t>Xyzzyq</a:t></a:r></a:p>"),
                 2));
  SlideSpelling spelling(checker);
  const std::vector<SpellError>& errors = spelling.Check(f.session->Doc().Resolved());
  REQUIRE(errors.size() == 1);
  CHECK_EQ(errors[0].from, 2u);
  CHECK_EQ(errors[0].to, 10u);
  // The same text again: from the cache (the same object).
  CHECK(&spelling.Check(f.session->Doc().Resolved()) == &errors);
}
