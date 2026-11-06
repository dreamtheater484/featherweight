// Text boxes, shapes and watermarks in Word documents (milestone D4). A text box's text is a story
// of its own, edited like any other and written back into its drawing in place of each copy of it
// (the DrawingML choice and the VML fallback); text the file keeps in a way that can't be written
// back is refused. Every change is one undo step that puts the document back exactly, and the
// saved file opens with what was left.
#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <string>
#include <vector>

#include "DocFixture.h"
#include "DocTestUtil.h"
#include "PkgTestUtil.h"
#include "TestHarness.h"
#include "doc/docx/Blank.h"
#include "doc/edit/Notes.h"
#include "doc/docx/Docx.h"
#include "doc/docx/Save.h"
#include "doc/edit/Editor.h"
#include "doc/edit/Format.h"
#include "doc/edit/HeaderFooter.h"
#include "doc/edit/Inline.h"
#include "doc/edit/Shapes.h"
#include "doc/edit/TreeEdit.h"
#include "doc/edit/Watermark.h"
#include "doc/proof/WordCount.h"
#include "pkg/Check.h"

using namespace fw::doc;
using namespace docfix;
using edit::Editor;
using edit::ShapeKind;
using edit::Shapes;
using edit::Wrap;

namespace {

std::unique_ptr<Document> Blank() {
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(docx::BlankDocx({"en-US", false, "Test", 0}), st);
  return pkg ? docx::Read(std::move(pkg), st) : nullptr;
}

// The drawings in the text, in order of their index.
std::vector<uint32_t> Shown(const Document& doc) {
  std::vector<uint32_t> out;
  for (uint32_t i = 0; i < doc.Drawings().size(); ++i)
    if (doc.Drawings()[i].pos.para != kNone) out.push_back(i);
  return out;
}

constexpr int64_t kInch = 914400;

// A PNG `w` x `h` pixels of one color, from WIC's built-in encoder.
std::vector<uint8_t> Png(int w, int h) {
  using Microsoft::WRL::ComPtr;
  std::vector<uint32_t> px(static_cast<size_t>(w) * h, 0xFF3366CC);
  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICBitmap> bmp;
  ComPtr<IStream> stream;
  ComPtr<IWICBitmapEncoder> enc;
  ComPtr<IWICBitmapFrameEncode> frame;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
  wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, w * 4, static_cast<UINT>(px.size() * 4),
                              reinterpret_cast<BYTE*>(px.data()), &bmp);
  CreateStreamOnHGlobal(nullptr, TRUE, &stream);
  wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
  enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  enc->CreateNewFrame(&frame, nullptr);
  frame->Initialize(nullptr);
  frame->SetSize(w, h);
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
  frame->SetPixelFormat(&fmt);
  frame->WriteSource(bmp.Get(), nullptr);
  frame->Commit();
  enc->Commit();
  STATSTG st{};
  stream->Stat(&st, STATFLAG_NONAME);
  std::vector<uint8_t> out(static_cast<size_t>(st.cbSize.QuadPart));
  LARGE_INTEGER zero{};
  stream->Seek(zero, STREAM_SEEK_SET, nullptr);
  ULONG read = 0;
  stream->Read(out.data(), static_cast<ULONG>(out.size()), &read);
  return out;
}

// Every header part's XML, written from the model.
std::string Headers(const Document& doc) {
  std::string all;
  for (uint32_t i = 0; i < doc.Parts().size(); ++i) {
    if (doc.Parts()[i].kind != PartKind::Header) continue;
    std::string out;
    if (docx::WritePart(doc, i, out).Ok()) all += out;
  }
  return all;
}

std::string PartXml(const Document& doc, PartKind kind) {
  for (uint32_t i = 0; i < doc.Parts().size(); ++i) {
    if (doc.Parts()[i].kind != kind) continue;
    std::string out;
    if (!docx::WritePart(doc, i, out).Ok()) return "ERROR";
    return out;
  }
  return "";
}

std::string Main(const Document& doc) { return PartXml(doc, PartKind::Document); }

size_t Count(const std::string& s, std::string_view what) {
  size_t n = 0;
  for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
  return n;
}

// Saves as the app does and reads the file back; null when either fails or the package has errors.
std::unique_ptr<Document> SaveAndRead(Document& doc, Editor& ed, std::string* mainXml = nullptr) {
  docx::SaveOptions o;
  o.changedParts = ed.DirtyParts();
  o.edited = true;
  std::vector<uint8_t> out;
  fw::pkg::Status st = docx::Serialize(doc, o, out);
  if (!st.Ok()) {
    std::printf("  save failed: %s\n", st.message.c_str());
    return nullptr;
  }
  auto pkg = test::OpenBytes(out, st);
  if (!pkg) return nullptr;
  fw::pkg::CheckReport report = fw::pkg::CheckPackage(*pkg);
  for (const std::string& e : report.errors) std::printf("  check: %s\n", e.c_str());
  if (!report.Ok()) return nullptr;
  if (mainXml) {
    std::vector<uint8_t> bytes;
    pkg->ReadPart("/word/document.xml", bytes);
    *mainXml = test::Text(bytes);
  }
  return docx::Read(std::move(pkg), st);
}

// The first paragraph of the first text box with text.
uint32_t BoxPara(const Document& doc, uint32_t nth = 0) {
  for (uint32_t s = 0; s < doc.Stories().size(); ++s)
    if (doc.Stories()[s].kind == StoryKind::TextBox && !doc.StoryParagraphs(s).empty() && nth-- == 0)
      return doc.StoryParagraphs(s)[0];
  return kNone;
}

std::u16string BoxText(const Document& doc, uint32_t nth = 0) {
  for (uint32_t s = 0; s < doc.Stories().size(); ++s) {
    if (doc.Stories()[s].kind != StoryKind::TextBox || nth-- != 0) continue;
    std::u16string out;
    for (uint32_t p : doc.StoryParagraphs(s)) out += doc.Para(p).text + u"|";
    return out;
  }
  return u"(none)";
}

// A text box as Word 2010 and later write it: a DrawingML shape, with a VML copy for older Word.
std::string WordTextBox(std::string_view content, std::string_view fallbackContent = {}, int id = 1) {
  const std::string inner(content), fallback(fallbackContent.empty() ? content : fallbackContent);
  const std::string docPr = "<wp:docPr id=\"" + std::to_string(id) + "\" name=\"Text Box " + std::to_string(id) + "\"/>";
  return "<w:r><mc:AlternateContent><mc:Choice Requires=\"wps\"><w:drawing><wp:anchor distT=\"0\" distB=\"0\" "
         "distL=\"114300\" distR=\"114300\" simplePos=\"0\" relativeHeight=\"251659264\" behindDoc=\"0\" locked=\"0\" "
         "layoutInCell=\"1\" allowOverlap=\"1\"><wp:simplePos x=\"0\" y=\"0\"/><wp:positionH relativeFrom=\"column\">"
         "<wp:posOffset>0</wp:posOffset></wp:positionH><wp:positionV relativeFrom=\"paragraph\"><wp:posOffset>0"
         "</wp:posOffset></wp:positionV><wp:extent cx=\"1828800\" cy=\"457200\"/><wp:effectExtent l=\"0\" t=\"0\" "
         "r=\"0\" b=\"0\"/><wp:wrapSquare wrapText=\"bothSides\"/>" + docPr +
         "<wp:cNvGraphicFramePr/><a:graphic><a:graphicData "
         "uri=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\"><wps:wsp><wps:cNvSpPr txBox=\"1\"/>"
         "<wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"1828800\" cy=\"457200\"/></a:xfrm><a:prstGeom "
         "prst=\"rect\"><a:avLst/></a:prstGeom></wps:spPr><wps:txbx>" +
         inner +
         "</wps:txbx><wps:bodyPr/></wps:wsp></a:graphicData></a:graphic></wp:anchor></w:drawing></mc:Choice>"
         "<mc:Fallback><w:pict><v:shape id=\"Text Box 1\" o:spid=\"_x0000_s1026\" type=\"#_x0000_t202\" "
         "style=\"position:absolute;width:144pt;height:36pt;z-index:251659264\" filled=\"f\"><v:textbox>" +
         fallback + "</v:textbox></v:shape></w:pict></mc:Fallback></mc:AlternateContent></w:r>";
}

const char* kBoxed = "<w:txbxContent><w:p><w:r><w:t>Boxed</w:t></w:r></w:p></w:txbxContent>";

}  // namespace

TEST(TextBox_TypingIsSavedInBothCopies) {
  Fixture f;
  f.body = "<w:p><w:r><w:t>Body</w:t></w:r>" + WordTextBox(kBoxed) + "</w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const uint32_t box = BoxPara(*doc);
  CHECK(box != kNone);
  if (box == kNone) return;
  CHECK(edit::TextBoxEditable(*doc, doc->Para(box).story));
  const std::string original = Main(*doc);
  CHECK_EQ(Count(original, ">Boxed<"), 2u);
  Editor ed(*doc);
  CHECK(ed.InsertText({box, 5}, u" and typed").ok);
  CHECK(doc->Para(box).text == u"Boxed and typed");
  std::string xml = Main(*doc);
  CHECK_EQ(Count(xml, ">Boxed and typed<"), 2u);  // the choice and the fallback
  CHECK_EQ(Count(xml, ">Boxed<"), 0u);
  CHECK(Count(xml, ">Body<") == 1 && Count(xml, "<v:textbox>") == 1 && Count(xml, "<wps:txbx>") == 1);

  std::string saved;
  auto back = SaveAndRead(*doc, ed, &saved);
  CHECK(back);
  if (back) {
    CHECK(BoxText(*back) == u"Boxed and typed|");
    CHECK_EQ(Count(saved, ">Boxed and typed<"), 2u);
  }
  // Undone, the drawing is written as it was read.
  CHECK(ed.Undo().ok);
  CHECK(Main(*doc) == original);
  CHECK(ed.Redo().ok);
  CHECK_EQ(Count(Main(*doc), ">Boxed and typed<"), 2u);
}

TEST(TextBox_ParagraphsFormattingAndKeptContent) {
  Fixture f;
  f.styles = "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/></w:style>";
  const std::string box =
      "<w:txbxContent><w:p w14:paraId=\"1A2B3C4D\" x:keep=\"1\"><w:pPr><w:jc w:val=\"left\"/></w:pPr><w:r><w:rPr>"
      "<x:mark/></w:rPr><w:t>Two words</w:t></w:r></w:p></w:txbxContent>";
  f.body = "<w:p>" + WordTextBox(box) + "</w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  uint32_t p = BoxPara(*doc);
  CHECK(p != kNone);
  if (p == kNone) return;
  Editor ed(*doc);
  edit::TreeEdit te(ed);
  edit::ToggleRuns(te, {{p, 0}, {p, 3}}, edit::Toggle::Bold);
  CHECK(te.Refused().empty());
  CHECK(ed.SplitParagraph({p, 3}).ok);
  CHECK(BoxText(*doc) == u"Two| words|");
  std::string xml = Main(*doc);
  CHECK_EQ(Count(xml, "<w:b/>"), 2u);
  CHECK_EQ(Count(xml, "<x:mark "), 4u);  // kept in both halves, in both copies
  CHECK_EQ(Count(xml, "x:keep=\"1\""), 4u);
  CHECK_EQ(Count(xml, "w14:paraId=\"1A2B3C4D\""), 2u);
  std::string saved;
  auto back = SaveAndRead(*doc, ed, &saved);
  CHECK(back);
  if (back) {
    CHECK(BoxText(*back) == u"Two| words|");
    const uint32_t bp = BoxPara(*back);
    CHECK(bp != kNone && back->ResolvedRun(bp, back->Runs(bp)[0].run).bold);
  }
  // Every step undone: the drawing as it was read.
  while (ed.CanUndo()) CHECK(ed.Undo().ok);
  CHECK(BoxText(*doc) == u"Two words|");
  CHECK_EQ(Count(Main(*doc), "<w:b/>"), 0u);
}

TEST(TextBox_WhereTheCopiesDifferItIsRefused) {
  // The fallback has no text box where the choice has one: what the choice's text becomes can't
  // be written into the fallback, so its text isn't edited or formatted.
  Fixture f;
  f.body = "<w:p>" + WordTextBox(kBoxed, "<w:p/>") + "</w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const uint32_t box = BoxPara(*doc);
  CHECK(box != kNone);
  if (box == kNone) return;
  CHECK(!edit::TextBoxEditable(*doc, doc->Para(box).story));
  const std::string original = Main(*doc);
  Editor ed(*doc);
  auto r = ed.InsertText({box, 0}, u"x");
  CHECK(!r.ok && !r.message.empty());
  edit::TreeEdit te(ed);
  edit::ToggleRuns(te, {{box, 0}, {box, 5}}, edit::Toggle::Bold);
  CHECK(!te.Refused().empty());
  CHECK(Main(*doc) == original && !ed.CanUndo());
}

namespace {

// A text box's bookmark as Word writes it: the same name in both copies, an id of its own in each
// (the choice 0, the fallback 1; eu-annex-2, govuk-revised-methodology in the corpus).
std::string BookmarkedBox(int id) {
  const std::string n = std::to_string(id);
  return "<w:txbxContent><w:p><w:bookmarkStart w:id=\"" + n + "\" w:name=\"_GoBack\"/><w:r><w:t>Boxed</w:t></w:r>"
         "<w:bookmarkEnd w:id=\"" + n + "\"/></w:p></w:txbxContent>";
}

// The w:id of each w:bookmarkStart in `xml`, in order.
std::vector<std::string> BookmarkIds(const std::string& xml) {
  std::vector<std::string> out;
  for (size_t at = xml.find("<w:bookmarkStart "); at != std::string::npos; at = xml.find("<w:bookmarkStart ", at + 1)) {
    const size_t id = xml.find("w:id=\"", at), end = xml.find('>', at);
    if (id < end) out.push_back(xml.substr(id + 6, xml.find('"', id + 6) - id - 6));
  }
  return out;
}

bool Unique(std::vector<std::string> ids) {
  std::sort(ids.begin(), ids.end());
  return std::adjacent_find(ids.begin(), ids.end()) == ids.end();
}

}  // namespace

// D5 corpus pass T4 (gojp-mhlw-form-b4): a new bookmark took id 1, which the text box's fallback
// copy of its bookmark has; the model reads only the choice. New ones go above those too.
TEST(TextBox_NewBookmarksSkipTheFallbacksIds) {
  Fixture f;
  f.body = "<w:p><w:r><w:t>Body</w:t></w:r>" + WordTextBox(BookmarkedBox(0), BookmarkedBox(1)) +
           "</w:p><w:p><w:r><w:t>Second</w:t></w:r></w:p><w:p><w:r><w:t>Third</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const std::vector<uint32_t> body = doc->StoryParagraphs(doc->MainStory());
  CHECK(body.size() == 3);
  if (body.size() != 3) return;
  Editor ed(*doc);
  CHECK(!edit::BookmarkForParagraph(ed, body[0]).empty());
  CHECK(edit::InsertBookmark(ed, {body[1], 0}, {body[1], 6}, "Mark").ok);
  CHECK(!edit::BookmarksForParagraphs(ed, {body[2]}, "_Toc").front().empty());
  std::string saved;
  auto back = SaveAndRead(*doc, ed, &saved);
  CHECK(back);
  const std::vector<std::string> ids = BookmarkIds(saved);
  CHECK_EQ(ids.size(), 5u);  // the box's two copies and the three new ones
  CHECK(Unique(ids));
}

// Typing in a text box writes its text into both copies; the fallback's bookmark gets an id of its
// own, as Word writes it, not the choice's again.
TEST(TextBox_EditedBookmarkHasItsOwnIdInEachCopy) {
  Fixture f;
  f.body = "<w:p><w:bookmarkStart w:id=\"2\" w:name=\"Body\"/><w:r><w:t>Body</w:t></w:r><w:bookmarkEnd w:id=\"2\"/>" +
           WordTextBox(BookmarkedBox(0), BookmarkedBox(1)) + "</w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const uint32_t box = BoxPara(*doc);
  CHECK(box != kNone);
  if (box == kNone) return;
  CHECK(edit::TextBoxEditable(*doc, doc->Para(box).story));
  Editor ed(*doc);
  CHECK(ed.InsertText({box, 5}, u" and typed").ok);
  std::string saved;
  auto back = SaveAndRead(*doc, ed, &saved);
  CHECK(back);
  if (back) CHECK(BoxText(*back) == u"Boxed and typed|");
  CHECK_EQ(Count(saved, ">Boxed and typed<"), 2u);
  CHECK_EQ(Count(saved, "w:name=\"_GoBack\""), 2u);
  const std::vector<std::string> ids = BookmarkIds(saved);
  CHECK_EQ(ids.size(), 3u);
  CHECK(Unique(ids));
  // Saved again from what was read back, the ids stay apart.
  if (back) {
    Editor again(*back);
    const uint32_t p = BoxPara(*back);
    CHECK(p != kNone && again.InsertText({p, 0}, u"x").ok);
    std::string twice;
    CHECK(SaveAndRead(*back, again, &twice));
    CHECK(BookmarkIds(twice).size() == 3u && Unique(BookmarkIds(twice)));
  }
}

TEST(TextBox_OlderVmlTextBoxIsSaved) {
  Fixture f;
  f.body = "<w:p><w:r><w:t>Body</w:t></w:r><w:r><w:pict><v:shape id=\"s\" style=\"position:absolute;width:144pt;"
           "height:36pt\"><v:textbox><w:txbxContent><w:p><w:r><w:t>Boxed</w:t></w:r></w:p></w:txbxContent></v:textbox>"
           "</v:shape></w:pict></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const uint32_t box = BoxPara(*doc);
  CHECK(box != kNone);
  if (box == kNone) return;
  Editor ed(*doc);
  CHECK(ed.Delete({box, 0}, {box, 1}).ok);
  CHECK(ed.InsertText({box, 0}, u"Unb").ok);
  std::string saved;
  auto back = SaveAndRead(*doc, ed, &saved);
  CHECK(back);
  if (back) CHECK(BoxText(*back) == u"Unboxed|");
  CHECK_EQ(Count(saved, "Unb"), 1u);
  CHECK(Count(saved, "v:textbox") == 2);
}

TEST(TextBox_TextKeptBesideItIsWrittenAsItWas) {
  // fuzz_shapes: characters among a VML drawing's elements are kept as a text fragment, which
  // isn't XML on its own; writing the box's text kept it as it was instead of failing.
  Fixture f;
  f.body = "<w:p><w:r><w:pict>333<v:shape id=\"s\" style=\"position:absolute;width:144pt;height:36pt\"><v:textbox>"
           "<w:txbxContent><w:p><w:r><w:t>Boxed</w:t></w:r></w:p></w:txbxContent></v:textbox></v:shape></w:pict></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const uint32_t box = BoxPara(*doc);
  CHECK(box != kNone);
  if (box == kNone) return;
  Editor ed(*doc);
  CHECK(ed.InsertText({box, 0}, u"Un").ok);
  std::string saved;
  auto back = SaveAndRead(*doc, ed, &saved);
  CHECK(back);
  if (back) CHECK(BoxText(*back) == u"UnBoxed|");
  CHECK(Count(saved, "333") == 1u && Count(saved, "UnBoxed") == 1u);
}

TEST(TextBox_InATableCellAndAHeaderToo) {
  // A text box in a table cell, and one in the header: each written into its own part.
  Fixture f;
  f.body = "<w:tbl><w:tblGrid><w:gridCol w:w=\"4000\"/></w:tblGrid><w:tr><w:tc><w:p>" + WordTextBox(kBoxed) +
           "</w:p></w:tc></w:tr></w:tbl><w:p/><w:sectPr><w:headerReference w:type=\"default\" r:id=\"rIdHeader\"/>"
           "</w:sectPr>";
  f.header = "<w:p>" + WordTextBox("<w:txbxContent><w:p><w:r><w:t>Top</w:t></w:r></w:p></w:txbxContent>", {}, 2) + "</w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const uint32_t a = BoxPara(*doc, 0), b = BoxPara(*doc, 1);
  CHECK(a != kNone && b != kNone);
  if (a == kNone || b == kNone) return;
  Editor ed(*doc);
  CHECK(ed.InsertText({a, 0}, u"1").ok);
  CHECK(ed.InsertText({b, 0}, u"2").ok);
  std::string saved;
  auto back = SaveAndRead(*doc, ed, &saved);
  CHECK(back);
  if (!back) return;
  std::u16string all = BoxText(*back, 0) + BoxText(*back, 1);
  CHECK(all.find(u"1Boxed|") != std::u16string::npos && all.find(u"2Top|") != std::u16string::npos);
  CHECK_EQ(Count(PartXml(*back, PartKind::Header), ">2Top<"), 2u);
}

// ---------- Shapes ----------

TEST(Shapes_ANewShapeTakesAnIdNothingHas) {
  // fuzz_shapes: an id in content kept as read (here the branch of an mc:AlternateContent block
  // the reader didn't take) isn't one of the model's drawings, but a new drawing mustn't take it.
  Fixture f;
  f.body = "<w:p><w:r><w:t>Body</w:t></w:r>" + WordTextBox(kBoxed, {}, 3) + "</w:p>";
  const std::string other = "<wp:docPr id=\"41\" name=\"Elsewhere\"/>";
  const size_t fallback = f.body.find("<mc:Fallback>");
  CHECK(fallback != std::string::npos);
  if (fallback == std::string::npos) return;
  f.body.insert(fallback + std::string("<mc:Fallback>").size(),
                "<w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\"><wp:extent cx=\"1\" cy=\"1\"/>" + other +
                    "<a:graphic><a:graphicData uri=\"x\"/></a:graphic></wp:inline></w:drawing>");
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  Shapes shapes(ed);
  CHECK(shapes.Insert(0, ShapeKind::Oval, fw::Emu(kInch), fw::Emu(kInch), fw::Emu(kInch), fw::Emu(kInch)).ok);
  CHECK(Count(Main(*doc), "<wp:docPr id=\"42\"") == 1u && Count(Main(*doc), "id=\"41\"") == 1u);
}

// Word refuses a file with a shape in a note or a comment ("You can't put drawing objects
// into ..."), so the command does too (D5 torture).
TEST(Shapes_NotInANote) {
  auto doc = Blank();
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  CHECK(edit::InsertNote(ed, {doc->StoryParagraphs(doc->MainStory())[0], 0}, false).ok);
  uint32_t note = kNone;
  for (uint32_t s = 0; s < doc->Stories().size(); ++s)
    if (doc->Stories()[s].kind == StoryKind::Footnote && !doc->StoryParagraphs(s).empty()) note = s;
  CHECK(note != kNone);
  if (note == kNone) return;
  Shapes shapes(ed);
  const uint32_t p = doc->StoryParagraphs(note).back();
  CHECK(!shapes.Insert(p, ShapeKind::Callout, fw::Emu(kInch), fw::Emu(kInch), fw::Emu(kInch), fw::Emu(kInch)).ok);
  const uint32_t body = doc->StoryParagraphs(doc->MainStory())[0];
  CHECK(shapes.Insert(body, ShapeKind::Callout, fw::Emu(kInch), fw::Emu(kInch), fw::Emu(kInch), fw::Emu(kInch)).ok);
}

TEST(Shapes_ANewShapeGoesOnTop) {
  auto doc = Blank();
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  Shapes shapes(ed);
  CHECK(shapes.Insert(0, ShapeKind::Rectangle, fw::Emu(kInch), fw::Emu(kInch), fw::Emu(2 * kInch), fw::Emu(kInch)).ok);
  CHECK(shapes.Insert(0, ShapeKind::Arrow, fw::Emu(kInch), fw::Emu(kInch), fw::Emu(2 * kInch), fw::Emu(0)).ok);
  const std::vector<uint32_t> shown = Shown(*doc);
  CHECK_EQ(shown.size(), 2u);
  if (shown.size() != 2) return;
  // The arrow, added last, is first in the paragraph and above the rectangle.
  const fw::doc::Drawing& arrow = doc->Drawings()[shown[0]].pos.offset == 0 ? doc->Drawings()[shown[0]] : doc->Drawings()[shown[1]];
  const fw::doc::Drawing& rect = &arrow == &doc->Drawings()[shown[0]] ? doc->Drawings()[shown[1]] : doc->Drawings()[shown[0]];
  CHECK(arrow.relativeHeight > rect.relativeHeight);
  CHECK(shapes.Info(static_cast<uint32_t>(&arrow - doc->Drawings().data())).line);
}

TEST(Shapes_EachKindIsInsertedSavedAndReopened) {
  auto doc = Blank();
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  Shapes shapes(ed);
  const ShapeKind kinds[] = {ShapeKind::TextBox, ShapeKind::Rectangle, ShapeKind::RoundedRectangle, ShapeKind::Oval,
                             ShapeKind::Line,    ShapeKind::Arrow,     ShapeKind::Callout};
  int64_t y = kInch;
  for (ShapeKind k : kinds) {
    auto r = shapes.Insert(0, k, fw::Emu(kInch), fw::Emu(y), fw::Emu(2 * kInch), fw::Emu(k == ShapeKind::Line ? 0 : kInch / 2));
    CHECK(r.ok);
    y += kInch;
  }
  CHECK(doc->Para(0).text == std::u16string(7, u'\xFFFC'));
  const std::vector<uint32_t> shown = Shown(*doc);
  CHECK_EQ(shown.size(), 7u);
  for (uint32_t d : shown) {
    CHECK(doc->Drawings()[d].displayOnly && doc->Drawings()[d].kept != 0 && doc->Drawings()[d].anchored);
    CHECK(shapes.Info(d).shape);
  }
  const std::string xml = Main(*doc);
  CHECK_EQ(Count(xml, "<mc:AlternateContent"), 7u);
  CHECK_EQ(Count(xml, "<mc:Fallback>"), 6u);  // not the callout: its VML isn't written
  CHECK(Count(xml, "<v:roundrect ") == 1 && Count(xml, "<v:oval ") == 1 && Count(xml, "<v:line ") == 2 &&
        Count(xml, "endarrow=\"block\"") == 1);
  CHECK_EQ(Count(xml, "<w:txbxContent>"), 3u);  // the text box twice, the callout once
  std::string saved;
  auto back = SaveAndRead(*doc, ed, &saved);
  CHECK(back);
  if (!back) return;
  CHECK_EQ(Shown(*back).size(), 7u);
  uint32_t boxes = 0;
  for (const Story& s : back->Stories()) boxes += s.kind == StoryKind::TextBox;
  CHECK_EQ(boxes, 2u);
  for (uint32_t d : Shown(*back)) CHECK(back->Drawings()[d].kept != 0);
  CHECK(back->Drawings()[Shown(*back)[0]].boxesWritable);
}

TEST(Shapes_TypingInANewTextBoxGoesInBothCopies) {
  auto doc = Blank();
  if (!doc) return;
  Editor ed(*doc);
  Shapes shapes(ed);
  CHECK(shapes.Insert(0, ShapeKind::TextBox, fw::Emu(kInch), fw::Emu(kInch), fw::Emu(2 * kInch), fw::Emu(kInch)).ok);
  const uint32_t d = Shown(*doc).at(0);
  const uint32_t story = shapes.TextStory(d);
  CHECK(story != kNone && edit::TextBoxEditable(*doc, story));
  const uint32_t p = doc->StoryParagraphs(story).at(0);
  CHECK(ed.InsertText({p, 0}, u"Hello").ok);
  CHECK(ed.SplitParagraph({p, 5}).ok);
  CHECK(ed.InsertText({doc->StoryParagraphs(story).at(1), 0}, u"world").ok);
  CHECK_EQ(Count(Main(*doc), ">Hello<"), 2u);
  CHECK_EQ(Count(Main(*doc), ">world<"), 2u);
  auto back = SaveAndRead(*doc, ed);
  CHECK(back);
  if (back) CHECK(BoxText(*back) == u"Hello|world|");
  // Undo all of it: the shape goes too.
  while (ed.CanUndo()) CHECK(ed.Undo().ok);
  CHECK(doc->Para(0).text.empty() && Shown(*doc).empty());
  CHECK_EQ(Count(Main(*doc), "AlternateContent"), 0u);
}

TEST(Shapes_ADeletedShapesTextIsntCounted) {
  auto doc = Blank();
  if (!doc) return;
  Editor ed(*doc);
  Shapes shapes(ed);
  CHECK(shapes.Insert(0, ShapeKind::TextBox, fw::Emu(kInch), fw::Emu(kInch), fw::Emu(2 * kInch), fw::Emu(kInch)).ok);
  const uint32_t story = shapes.TextStory(Shown(*doc).at(0));
  CHECK(ed.InsertText({doc->StoryParagraphs(story).at(0), 0}, u"three more words").ok);
  CHECK(doc->StoryInDocument(story) && doc->StoryInDocument(doc->MainStory()));
  CHECK_EQ(fw::doc::proof::CountDocument(*doc, true).words, 3u);
  CHECK(ed.Delete({0, 0}, {0, 1}).ok);  // the shape's character
  CHECK(!doc->StoryInDocument(story));
  CHECK_EQ(fw::doc::proof::CountDocument(*doc, true).words, 0u);
  CHECK(ed.Undo().ok);
  CHECK(doc->StoryInDocument(story));
  CHECK_EQ(fw::doc::proof::CountDocument(*doc, true).words, 3u);
}

TEST(Shapes_EachChangeIsOneStepAndUndoesExactly) {
  auto doc = Blank();
  if (!doc) return;
  Editor ed(*doc);
  Shapes shapes(ed);
  CHECK(shapes.Insert(0, ShapeKind::Rectangle, fw::Emu(kInch), fw::Emu(kInch), fw::Emu(2 * kInch), fw::Emu(kInch)).ok);
  const uint32_t d = Shown(*doc).at(0);
  std::vector<std::string> states{Main(*doc)};
  auto step = [&](const fw::doc::edit::Result& r) {
    CHECK(r.ok);
    states.push_back(Main(*doc));
    CHECK(states.back() != states[states.size() - 2]);
  };
  step(shapes.Resize(d, fw::Emu(3 * kInch), fw::Emu(kInch)));
  CHECK_EQ(shapes.Info(d).cx.Value(), 3 * kInch);
  CHECK(Count(states.back(), "width:216pt") == 1);
  step(shapes.Move(d, fw::Emu(2 * kInch), fw::Emu(3 * kInch)));
  CHECK(Count(states.back(), "margin-left:144pt") == 1 && Count(states.back(), "<wp:posOffset>1828800<") == 1);
  step(shapes.Rotate(d, 30));
  CHECK(shapes.Info(d).rotation == 30 && Count(states.back(), "rotation:30") == 1);
  step(shapes.SetFill(d, 0xFF0000u));
  CHECK(shapes.Info(d).fill == 0xFF0000u && Count(states.back(), "fillcolor=\"#FF0000\"") == 1);
  step(shapes.SetOutline(d, std::nullopt));
  CHECK(shapes.Info(d).noOutline && Count(states.back(), "stroked=\"f\"") == 1);
  step(shapes.SetWrap(d, Wrap::Square));
  CHECK(shapes.Info(d).wrap == Wrap::Square && Count(states.back(), "<w10:wrap type=\"square\"/>") == 1);
  step(shapes.SetWrap(d, Wrap::Inline));
  CHECK(shapes.Info(d).wrap == Wrap::Inline && Count(states.back(), "<wp:inline") == 1);
  auto back = SaveAndRead(*doc, ed);
  CHECK(back && Shown(*back).size() == 1);
  // Each undo puts back the XML before it, each redo the one after.
  for (size_t k = states.size() - 1; k > 0; --k) {
    CHECK(ed.Undo().ok);
    CHECK(Main(*doc) == states[k - 1]);
  }
  for (size_t k = 1; k < states.size(); ++k) {
    CHECK(ed.Redo().ok);
    CHECK(Main(*doc) == states[k]);
  }
}

TEST(Shapes_DeleteTakesItsBlockAndUndoBringsItBack) {
  auto doc = Blank();
  if (!doc) return;
  Editor ed(*doc);
  Shapes shapes(ed);
  CHECK(ed.InsertText({0, 0}, u"Text").ok);
  CHECK(shapes.Insert(0, ShapeKind::Oval, fw::Emu(kInch), fw::Emu(kInch), fw::Emu(kInch), fw::Emu(kInch)).ok);
  const std::string with = Main(*doc);
  CHECK(doc->Para(0).text == u"\xFFFCText");
  CHECK(ed.Delete({0, 0}, {0, 1}).ok);
  CHECK(doc->Para(0).text == u"Text" && Shown(*doc).empty());
  CHECK_EQ(Count(Main(*doc), "AlternateContent"), 0u);
  CHECK(SaveAndRead(*doc, ed) != nullptr);
  CHECK(ed.Undo().ok);
  CHECK(Main(*doc) == with && Shown(*doc).size() == 1);
  // Typing right after it and before it.
  CHECK(ed.InsertText({0, 1}, u"A").ok);
  CHECK(ed.InsertText({0, 0}, u"B").ok);
  CHECK(doc->Para(0).text == u"B\xFFFC" u"AText");
  auto back = SaveAndRead(*doc, ed);
  CHECK(back && back->Para(back->StoryParagraphs(back->MainStory())[0]).text == u"B\xFFFC" u"AText");
}

TEST(Shapes_AddTextToARectangle) {
  auto doc = Blank();
  if (!doc) return;
  Editor ed(*doc);
  Shapes shapes(ed);
  CHECK(shapes.Insert(0, ShapeKind::Rectangle, fw::Emu(kInch), fw::Emu(kInch), fw::Emu(2 * kInch), fw::Emu(kInch)).ok);
  const uint32_t d = Shown(*doc).at(0);
  CHECK(!shapes.Info(d).text && shapes.TextStory(d) == kNone);
  CHECK(shapes.AddText(d).ok);
  const uint32_t story = shapes.TextStory(d);
  CHECK(story != kNone && shapes.Info(d).text);
  CHECK(ed.InsertText({doc->StoryParagraphs(story).at(0), 0}, u"Inside").ok);
  CHECK_EQ(Count(Main(*doc), ">Inside<"), 2u);
  auto back = SaveAndRead(*doc, ed);
  CHECK(back);
  if (back) CHECK(BoxText(*back) == u"Inside|");
  // Lines hold no text.
  CHECK(shapes.Insert(0, ShapeKind::Line, fw::Emu(kInch), fw::Emu(kInch), fw::Emu(kInch), fw::Emu(0)).ok);
  CHECK(!shapes.AddText(Shown(*doc).back()).ok);
}

TEST(Shapes_AWordTextBoxChangesAndKeepsItsText) {
  Fixture f;
  f.body = "<w:p><w:r><w:t>Body</w:t></w:r>" + WordTextBox(kBoxed) + "</w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  Shapes shapes(ed);
  const uint32_t d = Shown(*doc).at(0);
  const fw::doc::edit::ShapeInfo info = shapes.Info(d);
  CHECK(info.shape && info.text && info.preset == "rect" && info.wrap == Wrap::Square);
  CHECK(shapes.SetFill(d, 0x00FF00u).ok);
  std::string xml = Main(*doc);
  CHECK(Count(xml, "fillcolor=\"#00FF00\"") == 1 && Count(xml, ">Boxed<") == 2 && Count(xml, "<v:shape ") == 0);
  CHECK(ed.InsertText({BoxPara(*doc), 0}, u"Green ").ok);
  CHECK_EQ(Count(Main(*doc), ">Green Boxed<"), 2u);
  auto back = SaveAndRead(*doc, ed);
  CHECK(back);
  if (back) CHECK(BoxText(*back) == u"Green Boxed|");
  // And it can be deleted.
  const uint32_t at = doc->Drawings()[d].pos.offset;
  CHECK(ed.Delete({doc->Drawings()[d].pos.para, at}, {doc->Drawings()[d].pos.para, at + 1}).ok);
  CHECK_EQ(Count(Main(*doc), "txbxContent"), 0u);
}

TEST(Shapes_PicturesAndOlderShapesAreNotShapesHere) {
  Fixture f;
  f.body = "<w:p><w:r><w:pict><v:shape id=\"s\" style=\"position:absolute;width:144pt;height:36pt\"><v:textbox>"
           "<w:txbxContent><w:p><w:r><w:t>Boxed</w:t></w:r></w:p></w:txbxContent></v:textbox></v:shape></w:pict></w:r></w:p>";
  auto doc = LoadOk(f);
  if (!doc) return;
  Editor ed(*doc);
  Shapes shapes(ed);
  const uint32_t d = Shown(*doc).at(0);
  const fw::doc::edit::ShapeInfo info = shapes.Info(d);
  CHECK(!info.shape && !info.why.empty());
  CHECK(!shapes.SetFill(d, 0xFF0000u).ok);
  // Its text is still edited.
  CHECK(ed.InsertText({BoxPara(*doc), 0}, u"x").ok);
}

// ---------- Watermarks ----------

TEST(Watermark_TextGoesInEveryHeaderAndComesOff) {
  auto doc = Blank();  // no header
  if (!doc) return;
  Editor ed(*doc);
  CHECK(edit::FindWatermark(*doc).kind == edit::WatermarkInfo::Kind::None);
  edit::TextWatermark mark;
  mark.text = u"CONFIDENTIAL";
  mark.color = 0xFF0000;
  CHECK(edit::SetTextWatermark(ed, mark).ok);
  edit::WatermarkInfo info = edit::FindWatermark(*doc);
  CHECK(info.kind == edit::WatermarkInfo::Kind::Text && info.text == u"CONFIDENTIAL" && info.diagonal &&
        info.semitransparent && info.color == 0xFF0000 && info.font == "Calibri");
  std::string xml = Headers(*doc);
  CHECK(Count(xml, "id=\"PowerPlusWaterMarkObject") == 1 && Count(xml, "string=\"CONFIDENTIAL\"") == 1 &&
        Count(xml, "<v:shapetype ") == 1 && Count(xml, "rotation:315") == 1 && Count(xml, "fillcolor=\"#FF0000\"") == 1);
  auto back = SaveAndRead(*doc, ed);
  CHECK(back);
  if (back) CHECK(edit::FindWatermark(*back).text == u"CONFIDENTIAL");
  // Again: it replaces the one there, one step.
  mark.text = u"DRAFT";
  mark.diagonal = false;
  CHECK(edit::SetTextWatermark(ed, mark).ok);
  xml = Headers(*doc);
  CHECK(Count(xml, "PowerPlusWaterMarkObject") == 1 && Count(xml, "string=\"DRAFT\"") == 1 && Count(xml, "rotation") == 0);
  CHECK(ed.Undo().ok);
  CHECK(edit::FindWatermark(*doc).text == u"CONFIDENTIAL");
  CHECK(edit::RemoveWatermark(ed).ok);
  CHECK(edit::FindWatermark(*doc).kind == edit::WatermarkInfo::Kind::None);
  CHECK(!edit::RemoveWatermark(ed).ok);
  CHECK(SaveAndRead(*doc, ed) != nullptr);
  // All of it undone: no watermark, no header referred to.
  while (ed.CanUndo()) CHECK(ed.Undo().ok);
  CHECK(edit::FindWatermark(*doc).kind == edit::WatermarkInfo::Kind::None);
  std::string saved;
  auto plain = SaveAndRead(*doc, ed, &saved);
  CHECK(plain && Count(saved, "headerReference") == 0);
}

TEST(Watermark_EverySectionAndPageType) {
  // Two sections, the second with a different first page: the default header (shared) and the
  // second section's first-page header each get one.
  Fixture f;
  f.body = "<w:p><w:pPr><w:sectPr/></w:pPr><w:r><w:t>One</w:t></w:r></w:p><w:p><w:r><w:t>Two</w:t></w:r></w:p>"
           "<w:sectPr><w:titlePg/></w:sectPr>";
  auto doc = LoadOk(f);
  if (!doc) return;
  Editor ed(*doc);
  CHECK(edit::SetTextWatermark(ed, {}).ok);
  size_t marks = 0;
  for (const Drawing& d : doc->Drawings()) marks += d.pos.para != kNone && d.name.rfind("PowerPlusWaterMarkObject", 0) == 0;
  CHECK(marks >= 2);
  auto back = SaveAndRead(*doc, ed);
  CHECK(back && edit::FindWatermark(*back).text == u"DRAFT");
  CHECK(ed.Undo().ok);
  for (const Drawing& d : doc->Drawings()) CHECK(d.pos.para == kNone || d.name.rfind("PowerPlus", 0) != 0);
}

TEST(Watermark_PictureWashedOut) {
  auto doc = Blank();
  if (!doc) return;
  Editor ed(*doc);
  CHECK(edit::SetPictureWatermark(ed, Png(40, 20)).ok);
  edit::WatermarkInfo info = edit::FindWatermark(*doc);
  CHECK(info.kind == edit::WatermarkInfo::Kind::Picture && info.washout);
  const std::string xml = Headers(*doc);
  CHECK(Count(xml, "id=\"WordPictureWatermark") == 1 && Count(xml, "gain=\"19661f\"") == 1 && Count(xml, "<v:imagedata r:id=\"rId") == 1);
  auto back = SaveAndRead(*doc, ed);  // the package check: the header's relationship reaches the picture
  CHECK(back);
  if (back) {
    CHECK(edit::FindWatermark(*back).kind == edit::WatermarkInfo::Kind::Picture);
    bool media = false;
    for (const auto& p : back->Package().Parts()) media = media || p.name.find("/word/media/image") == 0;
    CHECK(media);
  }
  // A text one replaces it; the picture's part isn't saved any more.
  CHECK(edit::SetTextWatermark(ed, {}).ok);
  auto again = SaveAndRead(*doc, ed);
  CHECK(again);
  if (again) {
    bool media = false;
    for (const auto& p : again->Package().Parts()) media = media || p.name.find("/word/media/image") == 0;
    CHECK(!media);
  }
  CHECK(!edit::SetPictureWatermark(ed, std::vector<uint8_t>{'n', 'o', 't'}).ok);
}
