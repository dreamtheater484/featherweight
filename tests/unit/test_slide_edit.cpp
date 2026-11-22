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
