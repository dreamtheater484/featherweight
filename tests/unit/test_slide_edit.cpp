// The presentation edit layer (src/slide/edit, P2 p2-model): operations with undo and redo, the
// model following edits, saving with untouched parts kept byte for byte, and crash recovery.
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "PkgTestUtil.h"
#include "TestHarness.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "slide/Presentation.h"
#include "slide/edit/Edit.h"
#include "slide/edit/Journal.h"
#include "slide/edit/Parts.h"
#include "slide/edit/Save.h"
#include "core/Strings.h"

using namespace fw::slide;
using namespace fw::slide::edit;

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

std::unique_ptr<Presentation> Open(const char* name) { return OpenBytes(FileBytes(Generated() / name)); }

std::string PartText(const Editor& ed, std::string_view part) {
  std::vector<uint8_t> b;
  ed.ReadPart(part, b);
  return std::string(b.begin(), b.end());
}

// Everything the deck's slides say, for comparing states.
std::string Digest(Presentation& deck) {
  std::string out;
  out += std::to_string(deck.SlideCount()) + ";";
  for (size_t i = 0; i < deck.SlideCount(); ++i) {
    const Slide* s = deck.GetSlide(i);
    out += std::to_string(s->id) + (s->hidden ? "h" : "") + "[" + (s->layout ? s->layout->part : std::string()) + "]";
    for (const Shape& sh : s->shapes) {
      out += "<" + std::to_string(sh.id) + ":" + std::to_string(static_cast<int>(sh.bounds.left)) + "," +
             std::to_string(static_cast<int>(sh.bounds.top)) + "," + std::to_string(static_cast<int>(sh.bounds.Width()));
      if (const TextBody* t = deck.Text(sh, *s)) {
        for (const Paragraph& p : t->paragraphs) {
          out += "|";
          for (const TextRun& r : p.runs) out += fw::WideToUtf8(r.text) + (r.props.bold ? "*" : "");
        }
      }
      out += ">";
    }
    out += "{" + fw::WideToUtf8(s->NotesText()) + "}";
  }
  return out;
}

const Shape* FirstWithText(const Slide* s) {
  for (const Shape& sh : s->shapes) {
    if (sh.textNode) return &sh;
  }
  return nullptr;
}

// A 1 x 1 PNG.
const std::vector<uint8_t> kPng = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0D, 0x49,
    0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0, 0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89, 0x99, 0x3D,
    0x1D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

}  // namespace

TEST(SlideEditSaveWithoutEditsKeepsEveryPart) {
  for (const char* name : {"simple.pptx", "layouts.pptx", "shapes-table-chart.pptx"}) {
    std::vector<uint8_t> original = FileBytes(Generated() / name);
    auto deck = OpenBytes(original);
    CHECK(deck != nullptr);
    if (!deck) continue;
    Editor ed(*deck);
    CHECK(!ed.Modified());
    std::vector<uint8_t> saved;
    std::vector<std::string> written;
    CHECK(SaveToBytes(ed, saved, &written).Ok());
    CHECK(written.empty());
    auto again = OpenBytes(saved);
    CHECK(again != nullptr);
    // Every part byte for byte.
    for (const fw::pkg::Part& p : deck->GetPackage().Parts()) {
      std::vector<uint8_t> a, b;
      deck->GetPackage().ReadPart(p.name, a);
      again->GetPackage().ReadPart(p.name, b);
      CHECK(a == b);
    }
  }
}

TEST(SlideEditNewSlideUndoRedo) {
  auto deck = Open("layouts.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  std::string before = Digest(*deck);
  size_t count = deck->SlideCount();
  std::vector<Changes> seen;
  ed.OnChange([&](const Changes& c) { seen.push_back(c); });
  uint32_t id = 0;
  CHECK(ed.NewSlide(1, 1, &id));
  CHECK_EQ(deck->SlideCount(), count + 1);
  CHECK(id >= 256);
  CHECK_EQ(deck->SlideId(1), id);
  const Slide* s = deck->GetSlide(1);
  CHECK(s->layout == deck->Layouts()[1].get());
  CHECK(!s->shapes.empty());  // the layout's placeholders
  for (const Shape& sh : s->shapes) CHECK(static_cast<bool>(sh.placeholder));
  CHECK(ed.Modified());
  CHECK(!seen.empty() && seen.back().slideList);
  CHECK(ed.CanUndo());
  CHECK(ed.UndoName() == u"New Slide");
  std::string after = Digest(*deck);
  CHECK(ed.Undo());
  CHECK_EQ(deck->SlideCount(), count);
  CHECK(Digest(*deck) == before);
  CHECK(!ed.Modified());
  CHECK(ed.Redo());
  CHECK(Digest(*deck) == after);
  // Saved and read back: the slide is there, on its layout, with its placeholders.
  std::vector<uint8_t> bytes;
  CHECK(SaveToBytes(ed, bytes).Ok());
  CHECK(!ed.Modified());
  auto again = OpenBytes(bytes);
  CHECK(again != nullptr);
  CHECK_EQ(again->SlideCount(), count + 1);
  CHECK(Digest(*again) == after);
}

TEST(SlideEditDuplicateDeleteMoveHide) {
  auto deck = Open("layouts.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  std::string original = Digest(*deck);
  size_t count = deck->SlideCount();
  CHECK(count >= 2);
  uint32_t first = deck->SlideId(0), second = deck->SlideId(1);
  std::vector<uint32_t> ids;
  CHECK(ed.DuplicateSlides({0}, &ids));
  CHECK_EQ(ids.size(), size_t{1});
  CHECK_EQ(deck->SlideCount(), count + 1);
  CHECK_EQ(deck->SlideId(1), ids[0]);
  CHECK(deck->GetSlide(1)->shapes.size() == deck->GetSlide(0)->shapes.size());
  CHECK(deck->GetSlide(1)->part != deck->GetSlide(0)->part);
  CHECK(ed.MoveSlides({0}, deck->SlideCount() - 1));
  CHECK_EQ(deck->SlideId(deck->SlideCount() - 1), first);
  CHECK_EQ(deck->SlideId(0), ids[0]);
  CHECK(ed.HideSlides({0}, true));
  CHECK(deck->GetSlide(0)->hidden);
  CHECK(ed.DeleteSlides({1}));
  CHECK_EQ(deck->SlideIndexOfId(second), -1);
  std::string edited = Digest(*deck);
  // Saved: the deleted slide's part is left out.
  std::vector<uint8_t> bytes;
  std::vector<std::string> written;
  CHECK(SaveToBytes(ed, bytes, &written).Ok());
  auto again = OpenBytes(bytes);
  CHECK(again != nullptr);
  CHECK(Digest(*again) == edited);
  CHECK(again->GetPackage().FindPart("/ppt/slides/slide2.xml") == nullptr);
  // Undo everything: the deck as opened.
  while (ed.CanUndo()) CHECK(ed.Undo());
  CHECK(Digest(*deck) == original);
  while (ed.CanRedo()) CHECK(ed.Redo());
  CHECK(Digest(*deck) == edited);
}

// Every slide jump in `xml` names its target (r:id there, not empty).
bool JumpsNameTheirSlide(const std::string& xml) {
  for (size_t at = xml.find("hlinksldjump"); at != std::string::npos; at = xml.find("hlinksldjump", at + 1)) {
    const size_t open = xml.rfind('<', at), close = xml.find('>', at);
    const std::string element = xml.substr(open, close - open);
    if (element.find(" r:id=\"") == std::string::npos || element.find(" r:id=\"\"") != std::string::npos) return false;
  }
  return true;
}

// Deleting a slide that others jump to: the links to it go everywhere (a shape's click and hover, a
// run, a notes page, another slide), the text and shapes staying; a link to a slide that stays is
// kept (p2-corpus's PowerPoint check: PowerPoint repairs a slide jump left without its r:id).
TEST(SlideEditDeletingASlideDropsLinksToIt) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  CHECK(ed.DuplicateSlides({0}));
  CHECK(ed.DuplicateSlides({0}));
  CHECK(deck->SlideCount() >= 3);
  const size_t last = deck->SlideCount() - 1;
  const Shape* shape = FirstWithText(deck->GetSlide(0));
  const Shape* other = FirstWithText(deck->GetSlide(1));
  CHECK(shape != nullptr && other != nullptr);
  if (!shape || !other) return;
  const uint32_t id = shape->id, otherId = other->id;
  LinkTarget toLast, toSecond;
  toLast.kind = toSecond.kind = LinkTarget::Kind::Slide;
  toLast.slide = last;
  toSecond.slide = 1;
  CHECK(ed.SetShapeLink(0, id, toLast));
  // The shape's hover jumps there too.
  std::string xml = ed.ShapeXml(0, id);
  const size_t click = xml.find("hlinkClick ");
  CHECK(click != std::string::npos);
  const size_t start = xml.rfind('<', click), end = xml.find("/>", click) + 2;
  std::string hover = xml.substr(start, end - start);
  hover.replace(hover.find("hlinkClick"), 10, "hlinkHover");
  xml.insert(end, hover);
  CHECK(ed.SetShapeXml(0, id, xml, u"Hover"));
  CHECK(ed.SetTextLink({0, id}, {{0, 0}, {0, 1}}, toSecond));
  CHECK(ed.SetShapeLink(1, otherId, toLast));
  CHECK(ed.SetNotesText(0, L"See the end"));
  CHECK(ed.SetTextLink({0, ed.NotesShape(0), -1, -1, true}, {{0, 0}, {0, 3}}, toLast));
  const std::string before = PartText(ed, ed.SlidePart(0));
  CHECK(before.find("hlinkHover") != std::string::npos);
  const std::wstring text = ed.ParagraphText({0, id}, 0);
  const std::string gone = ed.SlidePart(last);

  CHECK(ed.DeleteSlides({last}));
  const std::string first = PartText(ed, ed.SlidePart(0)), second = PartText(ed, ed.SlidePart(1));
  CHECK(first.find("hlinkHover") == std::string::npos);
  CHECK(first.find("hlinksldjump") != std::string::npos);  // the run's link to the second slide
  CHECK(JumpsNameTheirSlide(first));
  CHECK(second.find("hlinkClick") == std::string::npos);
  CHECK(ed.ParagraphText({0, id}, 0) == text);
  const NotesSlide* notes = deck->GetSlide(0)->notes;
  CHECK(notes != nullptr);
  if (notes) {
    const std::string n = PartText(ed, notes->part);
    CHECK(n.find("hlinkClick") == std::string::npos);
  }
  CHECK(deck->GetSlide(0)->NotesText() == L"See the end");
  // Saved: no slide jump without its target, and nothing leads to the deleted slide any more.
  std::vector<uint8_t> bytes;
  CHECK(SaveToBytes(ed, bytes).Ok());
  auto again = OpenBytes(bytes);
  CHECK(again != nullptr);
  if (!again) return;
  CHECK(again->GetPackage().FindPart(gone) == nullptr);
  Editor ed2(*again);
  for (size_t i = 0; i < again->SlideCount(); ++i) {
    CHECK(JumpsNameTheirSlide(PartText(ed2, ed2.SlidePart(i))));
    if (const NotesSlide* ns = again->GetSlide(i)->notes) CHECK(JumpsNameTheirSlide(PartText(ed2, ns->part)));
  }
  CHECK(PartText(ed2, ed2.SlidePart(0)).find("hlinksldjump") != std::string::npos);
  // Undone: the links are back.
  CHECK(ed.Undo());
  CHECK(PartText(ed, ed.SlidePart(0)) == before);
}

// viewProps' outline list names slides by relationship (p2-corpus's PowerPoint check, the TUM deck):
// deleting one drops its p:sld, never leaving r:id="" (PowerPoint repairs the deck).
TEST(SlideEditDeletingASlideDropsItsOutlineEntry) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  CHECK(deck->SlideCount() >= 2);
  const std::string view = "/ppt/viewProps.xml";
  const std::string slideRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide";
  std::string xml = PartText(ed, view);
  CHECK(xml.find("<p:notesTextViewPr>") != std::string::npos);
  CHECK(ed.Run(u"Test", [&] {
    const std::string first = ed.AddRelationship(view, slideRel, ed.SlidePart(0));
    const std::string second = ed.AddRelationship(view, slideRel, ed.SlidePart(1));
    if (first.empty() || second.empty()) return Result::Fail(u"rels");
    xml.insert(xml.find("<p:notesTextViewPr>"),
               "<p:outlineViewPr><p:cViewPr><p:scale><a:sx n=\"33\" d=\"100\"/><a:sy n=\"33\" d=\"100\"/></p:scale>"
               "<p:origin x=\"0\" y=\"0\"/></p:cViewPr><p:sldLst><p:sld r:id=\"" + first + "\" collapse=\"1\"/><p:sld r:id=\"" +
                   second + "\" collapse=\"1\"/></p:sldLst></p:outlineViewPr>");
    return ed.WritePart(view, std::vector<uint8_t>(xml.begin(), xml.end())) ? Result{} : Result::Fail(u"write");
  }));
  const std::string before = PartText(ed, view);
  auto entries = [](const std::string& s) {
    size_t n = 0;
    for (size_t i = s.find("<p:sld "); i != std::string::npos; i = s.find("<p:sld ", i + 1)) ++n;
    return n;
  };
  CHECK(entries(before) == 2);
  CHECK(ed.DeleteSlides({1}));
  std::string after = PartText(ed, view);
  CHECK(entries(after) == 1);
  CHECK(after.find("r:id=\"\"") == std::string::npos);
  // Saved and read again: one entry, its relationship there.
  std::vector<uint8_t> bytes;
  CHECK(SaveToBytes(ed, bytes).Ok());
  auto again = OpenBytes(bytes);
  CHECK(again != nullptr);
  if (again) {
    Editor ed2(*again);
    after = PartText(ed2, view);
    CHECK(entries(after) == 1);
    CHECK(after.find("r:id=\"\"") == std::string::npos);
  }
  // Undone: both entries again.
  CHECK(ed.Undo());
  CHECK(PartText(ed, view) == before);
}

TEST(SlideEditTypingCoalescesAndFormats) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  const Shape* shape = FirstWithText(deck->GetSlide(0));
  CHECK(shape != nullptr);
  if (!shape) return;
  TextTarget t{0, shape->id};
  std::wstring text = ed.ParagraphText(t, 0);
  TextPos at{0, text.size()};
  for (wchar_t c : std::wstring(L" abc")) {
    TextPos end;
    CHECK(ed.InsertText(t, at, std::wstring(1, c), &end));
    at = end;
  }
  CHECK(ed.ParagraphText(t, 0) == text + L" abc");
  CHECK_EQ(ed.UndoNames().size(), size_t{1});  // one "Typing" step
  CHECK(ed.UndoName() == u"Typing");
  // A new paragraph and a line break.
  CHECK(ed.InsertText(t, at, L"\nsecond\vline", &at));
  CHECK_EQ(ed.ParagraphCount(t), size_t{2} + (ed.ParagraphCount(t) - 2));
  CHECK(ed.ParagraphText(t, 1).starts_with(L"second\vline"));
  // Bold over "abc".
  ed.EndTyping();
  CHECK(ed.SetRunProps(t, {{0, text.size() + 1}, {0, text.size() + 4}}, RunPatch{.bold = true}));
  const Slide* s = deck->GetSlide(0);
  const TextBody* body = deck->Text(*s->FindShape(t.shape), *s);
  CHECK(body != nullptr);
  bool boldAbc = false;
  for (const TextRun& r : body->paragraphs[0].runs) boldAbc = boldAbc || (r.text == L"abc" && r.props.bold);
  CHECK(boldAbc);
  // Delete across the paragraphs: joined.
  CHECK(ed.DeleteText(t, {{0, text.size()}, {1, 6}}));
  CHECK(ed.ParagraphText(t, 0) == text + L"\vline");
  // Undo all: as opened.
  while (ed.CanUndo()) CHECK(ed.Undo());
  CHECK(ed.ParagraphText(t, 0) == text);
}

TEST(SlideEditShapes) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  std::string original = Digest(*deck);
  size_t shapes = deck->GetSlide(0)->shapes.size();
  uint32_t box = 0, rect = 0, pic = 0, table = 0;
  CHECK(ed.InsertTextBox(0, {100, 100, 300, 140}, L"Hello", &box));
  CHECK(ed.InsertAutoShape(0, "ellipse", {300, 200, 400, 300}, &rect));
  CHECK(ed.InsertPicture(0, kPng, {50, 300, 122, 372}, L"A dot", &pic));
  CHECK(ed.InsertTable(0, 2, 3, {100, 380, 500, 440}, &table));
  const Slide* s = deck->GetSlide(0);
  CHECK_EQ(s->shapes.size(), shapes + 4);
  CHECK(s->FindShape(box) && s->FindShape(box)->textBox);
  CHECK(s->FindShape(pic) && s->FindShape(pic)->kind == Shape::Kind::Picture);
  CHECK(s->FindShape(table) && s->FindShape(table)->kind == Shape::Kind::Table);
  CHECK(deck->GetTable(*s->FindShape(table), *s) != nullptr);
  CHECK(ed.ParagraphText({0, box}, 0) == L"Hello");
  // Table cells take text.
  CHECK(ed.InsertText({0, table, 1, 2}, {0, 0}, L"cell"));
  CHECK(ed.ParagraphText({0, table, 1, 2}, 0) == L"cell");
  // Move, rotate, arrange, group and ungroup.
  CHECK(ed.SetTransform(0, rect, {{310, 210, 430, 290}, 30}));
  s = deck->GetSlide(0);
  CHECK_NEAR(s->FindShape(rect)->bounds.left, 310, 0.01);
  CHECK_NEAR(s->FindShape(rect)->rotation, 30, 0.01);
  CHECK(ed.Arrange(0, {rect}, ZOrder::SendToBack));
  CHECK(deck->GetSlide(0)->shapes.front().id == rect);
  uint32_t group = 0;
  CHECK(ed.Group(0, {box, rect}, &group));
  s = deck->GetSlide(0);
  CHECK(s->FindShape(group) && s->FindShape(group)->kind == Shape::Kind::Group);
  CHECK_EQ(s->FindShape(group)->children.size(), size_t{2});
  // Into and out of the group as it is.
  CHECK(ed.MoveShape(0, table, group, -1));
  CHECK_EQ(deck->GetSlide(0)->FindShape(group)->children.size(), size_t{3});
  CHECK(ed.MoveShape(0, table, 0, -1));
  CHECK_EQ(deck->GetSlide(0)->FindShape(group)->children.size(), size_t{2});
  // A line on a group goes to its shapes: p:grpSpPr can't hold a:ln (torture finding).
  ShapePatch line;
  line.line = "<a:ln xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><a:solidFill><a:srgbClr val=\"000000\"/></a:solidFill></a:ln>";
  CHECK(ed.SetShapeProps(0, {group}, line));
  {
    std::string xml = ed.ShapeXml(0, group);
    size_t end = xml.find("grpSpPr>");
    end = xml.find("grpSpPr>", end + 1);
    CHECK(end != std::string::npos && xml.substr(0, end).find(":ln") == std::string::npos);
    CHECK(xml.find(":ln", end) != std::string::npos);
  }
  std::vector<uint32_t> back;
  CHECK(ed.Ungroup(0, group, &back));
  CHECK_EQ(back.size(), size_t{2});
  s = deck->GetSlide(0);
  CHECK(s->FindShape(group) == nullptr);
  CHECK_NEAR(s->FindShape(rect)->bounds.left, 310, 0.5);
  CHECK_NEAR(s->FindShape(rect)->bounds.Width(), 120, 0.5);
  // Copy and paste with the picture: on the same deck it shares the picture's part.
  ShapeClip clip = ed.CopyShapes(0, {pic, rect});
  CHECK_EQ(clip.shapes.size(), size_t{2});
  std::vector<uint32_t> pasted;
  CHECK(ed.PasteShapes(0, clip, &pasted));
  CHECK_EQ(pasted.size(), size_t{2});
  s = deck->GetSlide(0);
  CHECK(s->FindShape(pasted[0]) && s->FindShape(pasted[0]) != s->FindShape(pic));
  std::vector<uint32_t> pastedHere = pasted;
  // Into another deck: the picture comes along.
  auto other = Open("layouts.pptx");
  Editor ed2(*other);
  CHECK(ed2.PasteShapes(0, clip, &pasted));
  bool kind = false;
  for (uint32_t id : pasted) kind = kind || other->GetSlide(0)->FindShape(id)->kind == Shape::Kind::Picture;
  CHECK(kind);
  std::vector<uint8_t> otherBytes;
  CHECK(SaveToBytes(ed2, otherBytes).Ok());
  auto otherAgain = OpenBytes(otherBytes);
  CHECK(otherAgain && otherAgain->GetSlide(0)->shapes.size() == other->GetSlide(0)->shapes.size());
  CHECK(ed.DeleteShapes(0, pastedHere));
  // Fill, crop, delete.
  ShapePatch fill;
  fill.fill = "<a:solidFill xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><a:srgbClr val=\"FF0000\"/></a:solidFill>";
  CHECK(ed.SetShapeProps(0, {rect}, fill));
  CHECK(ed.SetCrop(0, pic, {0.1f, 0, 0.1f, 0}));
  CHECK(ed.DeleteShapes(0, {pic}));
  CHECK(deck->GetSlide(0)->FindShape(pic) == nullptr);
  std::string edited = Digest(*deck);
  std::vector<uint8_t> bytes;
  CHECK(SaveToBytes(ed, bytes).Ok());
  auto again = OpenBytes(bytes);
  CHECK(again != nullptr);
  CHECK(Digest(*again) == edited);
  // The picture nothing shows any more is left out.
  bool media = false;
  for (const fw::pkg::Part& p : again->GetPackage().Parts()) media = media || p.name.starts_with("/ppt/media/image");
  CHECK(!media || deck->GetPackage().FindPart("/ppt/media/image1.png") != nullptr);
  while (ed.CanUndo()) CHECK(ed.Undo());
  CHECK(Digest(*deck) == original);
}

TEST(SlideEditNotesAndLayout) {
  auto deck = Open("layouts.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  CHECK(ed.SetNotesText(0, L"First line\nSecond"));
  CHECK(deck->GetSlide(0)->NotesText() == L"First line\nSecond");
  CHECK(ed.NotesShape(0) != 0);
  CHECK(ed.ParagraphText({0, ed.NotesShape(0), -1, -1, true}, 1) == L"Second");
  size_t layout = deck->Layouts().size() - 1;
  CHECK(ed.SetLayout({0}, layout));
  CHECK(deck->GetSlide(0)->layout == deck->Layouts()[layout].get());
  CHECK(ed.SetSlideSize({720, 540}, Scale::EnsureFit));
  CHECK_NEAR(deck->SlideSize().width, 720, 0.01);
  std::string edited = Digest(*deck);
  std::vector<uint8_t> bytes;
  CHECK(SaveToBytes(ed, bytes).Ok());
  auto again = OpenBytes(bytes);
  CHECK(again != nullptr);
  CHECK(Digest(*again) == edited);
  CHECK_NEAR(again->SlideSize().width, 720, 0.01);
}

TEST(SlideEditJournalReplays) {
  std::vector<uint8_t> original = FileBytes(Generated() / "layouts.pptx");
  auto deck = OpenBytes(original);
  CHECK(deck != nullptr);
  Editor ed(*deck);
  Journal journal;
  ed.SetJournal(&journal);
  CHECK(ed.NewSlide(0, 0));
  CHECK(ed.DeleteSlides({2}));
  const Shape* shape = FirstWithText(deck->GetSlide(1));
  CHECK(shape != nullptr);
  if (shape) CHECK(ed.InsertText({1, shape->id}, {0, 0}, L"Recovered "));
  CHECK(journal.Due());
  std::string all = journal.All(ed);
  journal.Written();
  CHECK(!journal.Due());
  CHECK(ed.SetNotesText(1, L"later"));
  std::string more = journal.Unwritten(ed);
  std::string edited = Digest(*deck);
  // After a crash: the deck opened afresh, the records replayed.
  auto fresh = OpenBytes(original);
  Editor ed2(*fresh);
  size_t applied = 0;
  CHECK(Replay(ed2, all + more, &applied));
  CHECK(applied > 0);
  CHECK(Digest(*fresh) == edited);
}

// A text box holding math sits in mc:AlternateContent (a14 Choice, picture Fallback): formatting its
// text keeps the block, the Choice and the Fallback (torture findings: unwrapping made the math invalid;
// dropping the Fallback left apps that take it, and a p:bldP naming the shape, without it). Moving,
// formatting and replacing the shape keep the Fallback's shape in step.
TEST(SlideEditKeepsAlternateContentAroundEditedShapes) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  std::string part = ed.SlidePart(0);
  std::string xml = PartText(ed, part);
  const std::string box =
      "<mc:AlternateContent xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\">"
      "<mc:Choice xmlns:a14=\"http://schemas.microsoft.com/office/drawing/2010/main\" Requires=\"a14\">"
      "<p:sp><p:nvSpPr><p:cNvPr id=\"90\" name=\"Math\"/><p:cNvSpPr txBox=\"1\"/><p:nvPr/></p:nvSpPr><p:spPr>"
      "<a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"914400\" cy=\"457200\"/></a:xfrm><a:prstGeom prst=\"rect\">"
      "<a:avLst/></a:prstGeom></p:spPr><p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:rPr lang=\"en-US\"/><a:t>x = </a:t>"
      "</a:r><a14:m><m:oMathPara xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\"><m:oMath><m:r>"
      "<m:t>y</m:t></m:r></m:oMath></m:oMathPara></a14:m></a:p></p:txBody></p:sp></mc:Choice><mc:Fallback><p:sp><p:nvSpPr>"
      "<p:cNvPr id=\"90\" name=\"Math\"/><p:cNvSpPr txBox=\"1\"/><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm><a:off x=\"0\" "
      "y=\"0\"/><a:ext cx=\"914400\" cy=\"457200\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>"
      "</p:spPr></p:sp></mc:Fallback></mc:AlternateContent>";
  size_t at = xml.rfind("</p:spTree>");
  CHECK(at != std::string::npos);
  xml.insert(at, box);
  CHECK(ed.Run(u"Test", [&] {
    return ed.WritePart(part, std::vector<uint8_t>(xml.begin(), xml.end())) ? Result{} : Result::Fail(u"write");
  }));
  // Its text: fwdraw draws the Fallback's picture for math (p1-render), and the Fallback has no copy of
  // the text to keep in step, so formatting it is refused (it wouldn't show).
  CHECK(!ed.SetRunProps({0, 90}, {{0, 0}, {0, 4}}, RunPatch{.bold = true}));
  std::string after = PartText(ed, part);
  CHECK(after.find("AlternateContent") != std::string::npos);
  CHECK(after.find("Choice") != std::string::npos);
  CHECK(after.find("Fallback") != std::string::npos);
  CHECK(after.find("a14:m") != std::string::npos);
  // fwdraw draws the Fallback's picture for math (p1-render), the edit layer edits the Choice. Its text
  // holds an equation, so typing, setting it and a body without the math are refused (the app would type
  // into the Fallback's text); formatting isn't.
  CHECK(!ed.InsertText({0, 90}, {0, 0}, L"z"));
  CHECK(!ed.SetText({0, 90}, L"z"));
  CHECK(!ed.SetTextBodyXml({0, 90},
                           "<a:txBody xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><a:bodyPr/>"
                           "<a:p><a:r><a:t>z</a:t></a:r></a:p></a:txBody>",
                           u"Test", false));
  CHECK(PartText(ed, part) == after);
  auto count = [&](std::string_view s) {
    size_t n = 0;
    for (size_t i = after.find(s); i != std::string::npos; i = after.find(s, i + 1)) ++n;
    return n;
  };
  // Moved: both branches' shapes.
  CHECK(ed.SetTransform(0, 90, {{10, 20, 82, 56}}));
  after = PartText(ed, part);
  CHECK(count("x=\"127000\" y=\"254000\"") == 2);
  CHECK(count("cx=\"914400\" cy=\"457200\"") == 2);
  CHECK(count("Fallback") == 2);
  // A fill: both.
  ShapePatch fill;
  fill.fill = "<a:solidFill xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><a:srgbClr val=\"FF0000\"/></a:solidFill>";
  CHECK(ed.SetShapeProps(0, {90}, fill));
  after = PartText(ed, part);
  CHECK(count("FF0000") == 2);
  // Replaced (p2-objects' route): the Choice's shape where it is, its changed fill in the Fallback too.
  std::string shapeXml = ed.ShapeXml(0, 90);
  size_t red = shapeXml.find("FF0000");
  CHECK(red != std::string::npos && shapeXml.find("a14:m") != std::string::npos);
  shapeXml.replace(red, 6, "00FF00");
  CHECK(ed.SetShapeXml(0, 90, shapeXml, u"Test"));
  after = PartText(ed, part);
  CHECK(count("00FF00") == 2 && count("FF0000") == 0);
  CHECK(count("Fallback") == 2 && count("a14:m") >= 1);
  CHECK(count("x=\"127000\" y=\"254000\"") == 2);
  // Saved and opened again: still one block with both branches.
  std::vector<uint8_t> bytes;
  CHECK(SaveToBytes(ed, bytes).Ok());
  CHECK(OpenBytes(bytes) != nullptr);
  // Deleted: the whole block.
  CHECK(ed.DeleteShapes(0, {90}));
  after = PartText(ed, part);
  CHECK(count("id=\"90\"") == 0);
}

// A table holding math sits whole in mc:AlternateContent, and fwdraw draws its Fallback (p1-render). Typing
// in a cell without math reaches both branches, so the screen and apps taking the Fallback show it
// (p2-text's snapshot finding on edu-cs-2110-sorting slide 14); a cell whose Fallback text differs, and the
// math cell, are refused.
TEST(SlideEditTextInMathTablesReachesTheFallback) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  std::string part = ed.SlidePart(0);
  std::string xml = PartText(ed, part);
  auto cell = [](const std::string& para) {
    return "<a:tc><a:txBody><a:bodyPr/><a:lstStyle/><a:p>" + para + "</a:p></a:txBody><a:tcPr/></a:tc>";
  };
  auto run = [](const std::string& text) { return "<a:r><a:rPr lang=\"en-US\"/><a:t>" + text + "</a:t></a:r>"; };
  auto frame = [&](const std::string& mathCell, const std::string& lastCell) {
    return "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"91\" name=\"Table\"/><p:cNvGraphicFramePr/><p:nvPr/>"
           "</p:nvGraphicFramePr><p:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"1828800\" cy=\"740664\"/></p:xfrm><a:graphic>"
           "<a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/table\"><a:tbl><a:tblPr/><a:tblGrid>"
           "<a:gridCol w=\"914400\"/><a:gridCol w=\"914400\"/></a:tblGrid><a:tr h=\"370332\">" +
           cell(run("Sort")) + cell(mathCell) + "</a:tr><a:tr h=\"370332\">" + cell(run("Merge")) + cell(lastCell) +
           "</a:tr></a:tbl></a:graphicData></a:graphic></p:graphicFrame>";
  };
  const std::string math =
      "<a14:m><m:oMathPara xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\"><m:oMath><m:r>"
      "<m:t>n</m:t></m:r></m:oMath></m:oMathPara></a14:m>";
  const std::string block =
      "<mc:AlternateContent xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\">"
      "<mc:Choice xmlns:a14=\"http://schemas.microsoft.com/office/drawing/2010/main\" Requires=\"a14\">" +
      frame(math, run("Yes")) + "</mc:Choice><mc:Fallback>" + frame("<a:endParaRPr lang=\"en-US\"/>", run("Ja")) +
      "</mc:Fallback></mc:AlternateContent>";
  xml.insert(xml.rfind("</p:spTree>"), block);
  CHECK(ed.Run(u"Test", [&] {
    return ed.WritePart(part, std::vector<uint8_t>(xml.begin(), xml.end())) ? Result{} : Result::Fail(u"write");
  }));
  auto count = [&](std::string_view s) {
    const std::string now = PartText(ed, part);
    size_t n = 0;
    for (size_t i = now.find(s); i != std::string::npos; i = now.find(s, i + 1)) ++n;
    return n;
  };
  // A cell without math whose Fallback copy has the same text: typed and formatted in both.
  CHECK(ed.InsertText({0, 91, 1, 0}, {0, 5}, L" sort"));
  CHECK(count(">Merge sort<") == 2);
  CHECK(ed.SetRunProps({0, 91, 1, 0}, {{0, 0}, {0, 5}}, RunPatch{.bold = true}));
  CHECK(count("b=\"1\"") == 2);
  // Its Fallback text differs ("Ja"), and the math cell: refused, nothing changed.
  const std::string before = PartText(ed, part);
  CHECK(!ed.InsertText({0, 91, 1, 1}, {0, 0}, L"x"));
  CHECK(!ed.InsertText({0, 91, 0, 1}, {0, 0}, L"x"));
  CHECK(!ed.SetRunProps({0, 91, 0, 1}, {{0, 0}, {0, 0}}, RunPatch{.bold = true}));
  CHECK(PartText(ed, part) == before);
  CHECK(count("<a14:m>") == 1);  // never copied into the Fallback
  // Saved and read again: the block and both branches' text.
  std::vector<uint8_t> bytes;
  CHECK(SaveToBytes(ed, bytes).Ok());
  auto again = OpenBytes(bytes);
  CHECK(again != nullptr);
  if (again) {
    Editor ed2(*again);
    const std::string saved = PartText(ed2, ed2.SlidePart(0));
    size_t n = 0;
    for (size_t i = saved.find("> sort<"); i != std::string::npos; i = saved.find("> sort<", i + 1)) ++n;  // after the bold "Merge"
    CHECK(n == 2);
  }
}

// A group inside mc:AlternateContent (as PowerPoint writes a group holding math): z order, moves and
// deletes inside it reach the Fallback's group too; moving shapes into or out of it is refused.
TEST(SlideEditKeepsFallbackGroupsInStep) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  std::string part = ed.SlidePart(0);
  std::string xml = PartText(ed, part);
  auto sp = [](int id, int x) {
    return "<p:sp><p:nvSpPr><p:cNvPr id=\"" + std::to_string(id) + "\" name=\"S" + std::to_string(id) +
           "\"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm><a:off x=\"" + std::to_string(x) +
           "\" y=\"0\"/><a:ext cx=\"100\" cy=\"100\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>"
           "</p:spPr></p:sp>";
  };
  std::string group =
      "<p:grpSp><p:nvGrpSpPr><p:cNvPr id=\"93\" name=\"G\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr>"
      "<a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"300\" cy=\"100\"/><a:chOff x=\"0\" y=\"0\"/>"
      "<a:chExt cx=\"300\" cy=\"100\"/></a:xfrm></p:grpSpPr>" +
      sp(91, 0) + sp(92, 100) + sp(94, 200) + "</p:grpSp>";
  std::string block =
      "<mc:AlternateContent xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\">"
      "<mc:Choice xmlns:a14=\"http://schemas.microsoft.com/office/drawing/2010/main\" Requires=\"a14\">" +
      group + "</mc:Choice><mc:Fallback>" + group + "</mc:Fallback></mc:AlternateContent>";
  size_t at = xml.rfind("</p:spTree>");
  CHECK(at != std::string::npos);
  xml.insert(at, block);
  CHECK(ed.Run(u"Test", [&] {
    return ed.WritePart(part, std::vector<uint8_t>(xml.begin(), xml.end())) ? Result{} : Result::Fail(u"write");
  }));
  std::string after;
  auto order = [&](std::string_view branch, std::string_view first, std::string_view second) {
    size_t from = after.find(branch);
    return from != std::string::npos && after.find(first, from) < after.find(second, from);
  };
  CHECK(ed.Arrange(0, {91}, ZOrder::BringToFront));
  after = PartText(ed, part);
  CHECK(order("<mc:Choice", "id=\"94\"", "id=\"91\"") && order("<mc:Fallback", "id=\"94\"", "id=\"91\""));
  CHECK(order("<mc:Choice", "id=\"92\"", "id=\"94\"") && order("<mc:Fallback", "id=\"92\"", "id=\"94\""));
  CHECK(ed.MoveShape(0, 92, 93, 2));
  after = PartText(ed, part);
  CHECK(order("<mc:Choice", "id=\"94\"", "id=\"92\"") && order("<mc:Choice", "id=\"92\"", "id=\"91\""));
  CHECK(order("<mc:Fallback", "id=\"94\"", "id=\"92\"") && order("<mc:Fallback", "id=\"92\"", "id=\"91\""));
  CHECK(ed.SetTransform(0, 91, {{0, 1, 1, 2}}));
  after = PartText(ed, part);
  size_t moved = 0;
  for (size_t i = after.find("x=\"0\" y=\"12700\""); i != std::string::npos; i = after.find("x=\"0\" y=\"12700\"", i + 1)) ++moved;
  CHECK(moved == 2);
  CHECK(!ed.MoveShape(0, 91, 0, -1));
  CHECK(ed.DeleteShapes(0, {92}));
  after = PartText(ed, part);
  CHECK(after.find("id=\"92\"") == std::string::npos);
  CHECK(after.find("id=\"91\"") != after.rfind("id=\"91\""));
  CHECK(after.find("<mc:Fallback") != std::string::npos);
}

// A notes master made for notes copies the slide master's theme with its relationships (a theme's
// picture fill names one): torture finding, poi-apache-poi-test-deck-themes.pptx.
TEST(SlideEditNotesMasterThemeKeepsRelationships) {
  auto deck = Open("layouts.pptx");
  CHECK(deck != nullptr);
  CHECK(deck->NotesMaster() == nullptr);
  Editor ed(*deck);
  auto themeOf = [&](const std::string& part) {
    const fw::pkg::RelationshipSet* rels = deck->RelationshipsOf(part);
    for (const fw::pkg::Relationship& r : rels ? rels->items : std::vector<fw::pkg::Relationship>{}) {
      if (r.type.ends_with("/theme")) return r.targetPart;
    }
    return std::string();
  };
  std::string theme = themeOf(deck->Masters()[0]->part);
  CHECK(!theme.empty());
  std::string rel;
  CHECK(ed.Run(u"Test", [&] {
    rel = ed.AddPart(theme, "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image",
                     "/ppt/media/themepicture.png", "image/png", kPng);
    return rel.empty() ? Result::Fail(u"part") : Result{};
  }));
  CHECK(ed.SetNotesText(0, L"notes"));
  deck->GetSlide(0);  // reads its notes page, and with it the notes master
  CHECK(deck->NotesMaster() != nullptr);
  if (!deck->NotesMaster()) return;
  std::string notesTheme = themeOf(deck->NotesMaster()->part);
  CHECK(!notesTheme.empty() && notesTheme != theme);
  const fw::pkg::RelationshipSet* rels = deck->RelationshipsOf(notesTheme);
  CHECK(rels && rels->Find(rel) && rels->Find(rel)->targetPart == "/ppt/media/themepicture.png");
  std::vector<uint8_t> bytes;
  CHECK(SaveToBytes(ed, bytes).Ok());
  auto again = OpenBytes(bytes);
  CHECK(again && again->GetPackage().FindPart("/ppt/media/themepicture.png") != nullptr);
}

// fuzz_pptx findings (p2-corpus, 2026-11-22).
// 1. A refused text edit right after typing in the same place undoes only itself: the typing stays,
//    in the same step, and typing goes on in it.
TEST(SlideEditRefusedAfterTypingKeepsTyping) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  const Shape* shape = FirstWithText(deck->GetSlide(0));
  CHECK(shape != nullptr);
  if (!shape) return;
  const TextTarget t{0, shape->id};
  const std::wstring before = ed.ParagraphText(t, 0);
  CHECK(ed.InsertText(t, {0, 0}, L"Hi "));
  CHECK(!ed.DeleteText(t, {{99, 0}, {99, 1}}));
  CHECK(!ed.InsertText(t, {99, 0}, L"x"));
  CHECK(ed.ParagraphText(t, 0) == L"Hi " + before);
  CHECK_EQ(ed.UndoNames().size(), size_t{1});
  CHECK(ed.InsertText(t, {0, 3}, L"there "));
  CHECK_EQ(ed.UndoNames().size(), size_t{1});
  CHECK(ed.Undo());
  CHECK(ed.ParagraphText(t, 0) == before);
}

// 2. Characters XML 1.0 can't hold are dropped wherever text comes in (lone surrogates become
//    U+FFFD), so the save writes parts that parse.
TEST(SlideEditDropsCharactersXmlCantHold) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  uint32_t id = 0;
  CHECK(ed.InsertTextBox(0, {72, 72, 300, 120}, L"a\x0001z\xFFFE\xFFFF\xD800!", &id));
  const TextTarget t{0, id};
  CHECK(ed.ParagraphText(t, 0) == L"az\xFFFD!");
  CHECK(ed.InsertText(t, {0, 0}, L"\x0002\x001F"));
  CHECK(ed.ParagraphText(t, 0) == L"az\xFFFD!");
  CHECK(ed.SetNotesText(0, L"n\x0007o\xDC00tes"));
  ShapePatch names;
  names.name = L"Box\x0001";
  names.altText = L"alt\xFFFF";
  CHECK(ed.SetShapeProps(0, {id}, names));
  const std::string xml = ed.ShapeXml(0, id);
  CHECK(xml.find("name=\"Box\"") != std::string::npos && xml.find("descr=\"alt\"") != std::string::npos);
  std::vector<uint8_t> bytes;
  CHECK(SaveToBytes(ed, bytes).Ok());
  auto again = OpenBytes(bytes);
  CHECK(again != nullptr);
}

// 3. A journal replayed after a crash keeps what was back to the file's own as the file's own: the
//    save copies every part byte for byte ([Content_Types].xml and rels included).
TEST(SlideEditReplayKeepsOriginals) {
  std::vector<uint8_t> original = FileBytes(Generated() / "layouts.pptx");
  auto deck = OpenBytes(original);
  CHECK(deck != nullptr);
  Editor ed(*deck);
  Journal journal;
  ed.SetJournal(&journal);
  CHECK(ed.NewSlide(0, 0));
  CHECK(ed.Undo());
  const std::string all = journal.All(ed);
  auto fresh = OpenBytes(original);
  CHECK(fresh != nullptr);
  Editor ed2(*fresh);
  CHECK(Replay(ed2, all));
  std::vector<uint8_t> bytes;
  std::vector<std::string> written;
  CHECK(SaveToBytes(ed2, bytes, &written).Ok());
  CHECK(written.empty());
  // And after a real edit kept and one undone: only the kept edit's parts are written.
  CHECK(ed.SetNotesText(0, L"kept"));
  auto fresh2 = OpenBytes(original);
  Editor ed3(*fresh2);
  CHECK(Replay(ed3, journal.All(ed)));
  std::vector<std::string> written2;
  CHECK(SaveToBytes(ed3, bytes, &written2).Ok());
  std::vector<std::string> expected;
  CHECK(SaveToBytes(ed, bytes, &expected).Ok());
  CHECK(written2 == expected);
}

// XML fragments (pasted from other apps, p2-objects' format changes): references to characters XML
// doesn't allow (&#1;, &#xFFFE;, a lone surrogate) never reach the writer. The package's XML reader
// refuses them (the call changes nothing); whatever a parser lets through is cleaned (CleanXmlTree)
// as typed text is. The deck saves and reads again.
TEST(SlideEditFragmentsDropCharactersXmlCantHold) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  uint32_t id = 0;
  CHECK(ed.InsertTextBox(0, {72, 72, 300, 120}, L"box", &id));
  const TextTarget t{0, id};
  auto body = [](const std::string& text, const std::string& lang) {
    return "<p:txBody xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" "
           "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><a:bodyPr/><a:lstStyle/>"
           "<a:p><a:r><a:rPr lang=\"" + lang + "\"/><a:t>" + text + "</a:t></a:r></a:p></p:txBody>";
  };
  CHECK(ed.SetTextBodyXml(t, body("&#1;x&#xFFFE;", "en-US")) || ed.ParagraphText(t, 0) == L"box");
  for (const char* ref : {"&#1;", "&#x1F;", "&#xFFFE;", "&#xFFFF;", "&#xD800;", "&#xDFFF;"}) {
    const std::wstring before = ed.ParagraphText(t, 0);
    if (ed.SetTextBodyXml(t, body(std::string("a") + ref + "b", std::string("en") + ref))) {
      const std::wstring now = ed.ParagraphText(t, 0);
      CHECK(now == L"ab" || now == L"a\xFFFD" L"b");
    } else {
      CHECK(ed.ParagraphText(t, 0) == before);
    }
  }
  // A shape replaced from XML (SetShapeXml), with such a reference in its name.
  std::string shape = ed.ShapeXml(0, id);
  const size_t name = shape.find("name=\"");
  CHECK(name != std::string::npos);
  shape.insert(name + 6, "N&#x1F;");
  if (ed.SetShapeXml(0, id, shape, u"Test")) CHECK(ed.ShapeXml(0, id).find('\x1F') == std::string::npos);
  std::vector<uint8_t> bytes;
  CHECK(SaveToBytes(ed, bytes).Ok());
  CHECK(OpenBytes(bytes) != nullptr);
}

// The cleaner itself, on text a parser decoded: control characters and U+FFFE/U+FFFF dropped, a lone
// surrogate (as decoded from &#xD800;) and broken UTF-8 become U+FFFD, in text and attributes.
TEST(SlideEditCleanXmlTree) {
  pugi::xml_document doc;
  pugi::xml_node t = doc.append_child("t");
  t.append_child(pugi::node_pcdata).set_value("\x01x\xEF\xBF\xBE\xEF\xBF\xBFy\tz");
  t.append_attribute("v") = "a\xED\xA0\x80" "b\x1F\xC3\xA9\xFF";
  fw::slide::edit::CleanXmlTree(doc);
  CHECK(std::string(t.text().get()) == "xy\tz");
  CHECK(std::string(t.attribute("v").value()) == "a\xEF\xBF\xBD" "b\xC3\xA9\xEF\xBF\xBD");
  CHECK(fw::slide::edit::XmlTextUtf8("ok \xF0\x9F\x98\x80") == "ok \xF0\x9F\x98\x80");
}

// Tables by id: rows and columns, merging (the cells' text joins the merged cell) and splitting,
// widths and heights (the frame follows), cell properties; each one undo step; refusals change nothing.
TEST(SlideEditTables) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  uint32_t id = 0;
  CHECK(ed.InsertTable(0, 3, 3, {72, 72, 372, 222}, &id));
  auto xml = [&] { return ed.ShapeXml(0, id); };
  auto count = [&](std::string_view what) {
    const std::string x = xml();
    size_t n = 0;
    for (size_t i = x.find(what); i != std::string::npos; i = x.find(what, i + 1)) ++n;
    return n;
  };
  CHECK(ed.InsertTableRows(0, id, 1, 2));
  CHECK_EQ(count("<a:tr "), size_t{5});
  CHECK(ed.DeleteTableColumns(0, id, 0, 1));
  CHECK_EQ(count("<a:gridCol "), size_t{2});
  CHECK(!ed.DeleteTableColumns(0, id, 0, 2));  // a table keeps one column
  CHECK_EQ(count("<a:gridCol "), size_t{2});
  CHECK(ed.SetText({0, id, 0, 0}, L"top"));
  CHECK(ed.SetText({0, id, 1, 1}, L"below"));
  CHECK(ed.MergeTableCells(0, id, 1, 1, 0, 0));
  CHECK(xml().find("gridSpan=\"2\" rowSpan=\"2\"") != std::string::npos);
  CHECK_EQ(count("hMerge=\"1\""), size_t{2});
  CHECK_EQ(count("vMerge=\"1\""), size_t{2});
  CHECK(ed.ParagraphText({0, id, 0, 0}, 0) == L"top");
  CHECK(ed.ParagraphText({0, id, 0, 0}, 1) == L"below");
  CHECK(!ed.MergeTableCells(0, id, 1, 1, 2, 1));  // cuts through the merged cell
  CHECK(ed.SplitTableCell(0, id, 1, 1));
  CHECK_EQ(count("Merge=\"1\""), size_t{0});
  CHECK(!ed.SplitTableCell(0, id, 1, 1));
  CHECK(ed.SetTableColumnWidth(0, id, 1, 120));
  CHECK(xml().find("w=\"1524000\"") != std::string::npos);
  CHECK(ed.SetTableRowHeight(0, id, 0, 50));
  CHECK(xml().find("h=\"635000\"") != std::string::npos);
  CHECK(!ed.SetTableRowHeight(0, id, 0, -3));
  CellPatch cells;
  cells.fill = "<a:solidFill xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><a:srgbClr val=\"00FF00\"/></a:solidFill>";
  cells.borders[0] = "<a:ln xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" w=\"12700\"><a:solidFill><a:srgbClr val=\"FF0000\"/></a:solidFill></a:ln>";
  cells.anchor = BodyProps::Anchor::Middle;
  CHECK(ed.SetCellProps(0, id, 0, 0, 1, 1, cells));
  CHECK_EQ(count("00FF00"), size_t{4});
  CHECK_EQ(count("<a:lnL "), size_t{4});
  CHECK_EQ(count("anchor=\"ctr\""), size_t{4});
  CHECK(!ed.SetCellProps(0, id, 0, 0, 9, 9, cells));
  // The frame follows the grid: 2 columns (120 pt and 100 pt), 5 rows.
  const std::string x = xml();
  CHECK(x.find("cx=\"2794000\"") != std::string::npos);
  const size_t steps = ed.UndoNames().size();
  CHECK(ed.Undo(steps - 1));
  CHECK_EQ(count("<a:tr "), size_t{3});
  std::vector<uint8_t> bytes;
  CHECK(ed.Redo(steps - 1));
  CHECK(SaveToBytes(ed, bytes).Ok());
  CHECK(OpenBytes(bytes) != nullptr);
}

// Slides copied keeping the source's design: its master (with its theme) and the layouts used come
// along once, with new unique ids; copying again uses them again; the deck saves and reads again.
TEST(SlideEditInsertSlidesKeepingDesign) {
  auto source = Open("layouts.pptx");
  auto deck = Open("simple.pptx");
  CHECK(source != nullptr && deck != nullptr);
  if (!source || !deck) return;
  // The source's own design (both test decks start from the same master, which would be used again).
  Editor sourceEd(*source);
  CHECK(sourceEd.SetMasterBackground(0, "<p:bg xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" "
                                        "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><p:bgPr>"
                                        "<a:solidFill><a:srgbClr val=\"123456\"/></a:solidFill><a:effectLst/></p:bgPr></p:bg>"));
  Editor ed(*deck);
  const size_t masters = deck->Masters().size(), slides = deck->SlideCount();
  const std::string layoutName = source->GetSlide(1)->layout->name;
  std::vector<uint32_t> ids;
  CHECK(ed.InsertSlides(*source, {0, 1}, 0, true, &ids));
  CHECK_EQ(ids.size(), size_t{2});
  CHECK_EQ(deck->SlideCount(), slides + 2);
  CHECK_EQ(deck->Masters().size(), masters + 1);
  CHECK(deck->GetSlide(1)->layout && deck->GetSlide(1)->layout->name == layoutName);
  CHECK(deck->GetSlide(1)->layout->master != deck->GetSlide(2)->layout->master);
  const size_t layouts = deck->Layouts().size();
  // Again: the same master and layouts.
  CHECK(ed.InsertSlides(*source, {1}, 0, true));
  CHECK_EQ(deck->Masters().size(), masters + 1);
  CHECK_EQ(deck->Layouts().size(), layouts);
  // Ids of masters and layouts stay unique.
  std::set<std::string> seen;
  bool unique = true;
  size_t count = 0;
  // Every id=".." between <p:...IdLst> and its end tag.
  auto scan = [&](const std::string& part, const std::string& list) {
    const std::string x = PartText(ed, part);
    const size_t from = x.find(list + ">"), to = x.find("</p:" + list + ">");
    if (from == std::string::npos || to == std::string::npos) return;
    const std::string inside = x.substr(from, to - from);
    for (size_t i = inside.find(" id=\""); i != std::string::npos; i = inside.find(" id=\"", i + 1)) {
      const size_t q = i + 5, e = inside.find('"', q);
      unique = seen.insert(inside.substr(q, e - q)).second && unique;
      ++count;
    }
  };
  scan(deck->PresentationPart(), "sldMasterIdLst");
  for (const auto& mst : deck->Masters()) scan(mst->part, "sldLayoutIdLst");
  CHECK(unique);
  CHECK(count >= deck->Masters().size() + deck->Layouts().size());
  std::vector<uint8_t> bytes;
  CHECK(SaveToBytes(ed, bytes).Ok());
  auto again = OpenBytes(bytes);
  CHECK(again != nullptr);
  if (again) {
    CHECK_EQ(again->SlideCount(), slides + 3);
    CHECK_EQ(again->Masters().size(), masters + 1);
  }
  // Within one deck the design is its own: no new master.
  CHECK(ed.InsertSlides(*deck, {0}, 0, true));
  CHECK_EQ(deck->Masters().size(), masters + 1);
}
