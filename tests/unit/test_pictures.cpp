// Pictures in Word documents (milestone D3): inserting a picture adds its part, relationship and
// content type, sharing an identical picture; every change edits only what it should in the
// drawing's XML; undo puts back the XML, the drawing and the package exactly; drawings that aren't
// pictures are refused; and the saved file opens with the picture as it was left.
#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <string>
#include <thread>
#include <vector>

#include "DocTestUtil.h"
#include "PkgTestUtil.h"
#include "TestHarness.h"
#include "doc/docx/Blank.h"
#include "doc/docx/Docx.h"
#include "doc/docx/Save.h"
#include "doc/edit/Editor.h"
#include "doc/edit/Pictures.h"
#include "pkg/Check.h"

using namespace fw::doc;
using Microsoft::WRL::ComPtr;
using edit::Crop;
using edit::Editor;
using edit::Pictures;
using edit::Wrap;

namespace {

// `w` x `h` pixels of one color, encoded by WIC's built-in encoder for `container`.
std::vector<uint8_t> Solid(const GUID& container, int w, int h, uint32_t argb = 0xFF3366CC) {
  std::vector<uint32_t> px(static_cast<size_t>(w) * h, argb);
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
  wic->CreateEncoder(container, nullptr, &enc);
  enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  enc->CreateNewFrame(&frame, nullptr);
  frame->Initialize(nullptr);
  frame->SetSize(w, h);
  WICPixelFormatGUID fmt = container == GUID_ContainerFormatPng ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
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

std::unique_ptr<Document> Blank() {
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(docx::BlankDocx({"en-US", false, "Test", 0}), st);
  return pkg ? docx::Read(std::move(pkg), st) : nullptr;
}

std::unique_ptr<Document> ReadBytes(std::vector<uint8_t> bytes) {
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(std::move(bytes), st);
  return pkg ? docx::Read(std::move(pkg), st) : nullptr;
}

uint32_t MainIndex(const Document& doc) { return static_cast<uint32_t>(doc.FindPart(PartKind::Document) - doc.Parts().data()); }

// Saves as the app does and reads the file back; null when either fails or the package has errors.
std::unique_ptr<Document> SaveAndRead(Document& doc, Editor& ed) {
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
  if (!pkg || !fw::pkg::CheckPackage(*pkg).Ok()) {
    std::printf("  the saved package has errors\n");
    return nullptr;
  }
  return ReadBytes(std::move(out));
}

// The one drawing in the document that's in the text.
uint32_t Shown(const Document& doc) {
  uint32_t found = kNone;
  for (uint32_t i = 0; i < doc.Drawings().size(); ++i)
    if (doc.Drawings()[i].pos.para != kNone) found = i;
  return found;
}

size_t Count(const std::string& s, std::string_view what) {
  size_t n = 0;
  for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
  return n;
}

std::string Xml(const Document& doc, uint32_t d) { return doc.DrawingXml(doc.Drawings()[d]); }

const char* kRels = "/word/_rels/document.xml.rels";

}  // namespace

TEST(Pictures_InsertAddsThePartAndUndoTakesItAway) {
  auto doc = Blank();
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  Pictures pics(ed);
  auto png = Solid(GUID_ContainerFormatPng, 96, 48);  // 72 x 36 pt at 96 dpi
  auto r = pics.Insert(Pos{0, 0}, png, fw::Emu(0), u"photo.png");
  CHECK(r.ok);
  CHECK(doc->Para(0).text == u"\xFFFC");
  uint32_t d = Shown(*doc);
  CHECK(d != kNone);
  if (d == kNone) return;
  CHECK_EQ(doc->Drawings()[d].cx.Value(), 72 * 12700);
  CHECK_EQ(doc->Drawings()[d].cy.Value(), 36 * 12700);
  CHECK(!doc->Drawings()[d].anchored);
  CHECK(doc->Package().FindPart("/word/media/image1.png") != nullptr);
  std::vector<uint8_t> stored;
  CHECK(doc->Package().ReadPart("/word/media/image1.png", stored).Ok() && stored == png);
  CHECK(doc->Package().Types().LookupDefault("/word/media/image1.png") == "image/png");
  const fw::pkg::RelationshipSet* rels = doc->Package().RelationshipsOf("/word/document.xml");
  CHECK(rels && rels->items.back().targetPart == "/word/media/image1.png");
  CHECK(Xml(*doc, d).find("r:embed=\"" + rels->items.back().id + "\"") != std::string::npos);
  CHECK(Xml(*doc, d).find("name=\"photo.png\"") != std::string::npos);

  // Saved, it opens with the picture.
  auto back = SaveAndRead(*doc, ed);
  CHECK(back);
  if (back) {
    CHECK_EQ(back->Drawings().size(), 1u);
    std::vector<uint8_t> bytes;
    CHECK(back->ReadTarget(back->Drawings()[0].part, rels->items.back().id, bytes).Ok() && bytes == png);
  }

  // Undo: no picture, no part, no relationship.
  const size_t relCount = rels->items.size();
  CHECK(ed.Undo().ok);
  CHECK(doc->Para(0).text.empty());
  CHECK(doc->Package().FindPart("/word/media/image1.png") == nullptr);
  CHECK_EQ(doc->Package().RelationshipsOf("/word/document.xml")->items.size(), relCount - 1);
  CHECK(Shown(*doc) == kNone);
  // Redo: all of it again.
  CHECK(ed.Redo().ok);
  CHECK(doc->Para(0).text == u"\xFFFC");
  CHECK(doc->Package().FindPart("/word/media/image1.png") != nullptr);
  CHECK(Shown(*doc) != kNone);
}

TEST(Pictures_TheSamePictureSharesItsPart) {
  auto doc = Blank();
  if (!doc) return;
  Editor ed(*doc);
  Pictures pics(ed);
  auto png = Solid(GUID_ContainerFormatPng, 20, 20);
  CHECK(pics.Insert(Pos{0, 0}, png).ok);
  size_t rels = doc->Package().RelationshipsOf("/word/document.xml")->items.size();
  CHECK(pics.Insert(Pos{0, 1}, png).ok);
  CHECK(doc->Package().FindPart("/word/media/image2.png") == nullptr);
  CHECK_EQ(doc->Package().RelationshipsOf("/word/document.xml")->items.size(), rels);
  // A different picture gets a part of its own; the docPr ids differ.
  CHECK(pics.Insert(Pos{0, 2}, Solid(GUID_ContainerFormatPng, 21, 20)).ok);
  CHECK(doc->Package().FindPart("/word/media/image2.png") != nullptr);
  CHECK_EQ(doc->Drawings().size(), 3u);
  CHECK(doc->Drawings()[0].id != doc->Drawings()[1].id && doc->Drawings()[1].id != doc->Drawings()[2].id);
}

TEST(Pictures_InsertFitsTheColumnAndConvertsBmp) {
  auto doc = Blank();
  if (!doc) return;
  Editor ed(*doc);
  Pictures pics(ed);
  // 960 x 480 px is 720 x 360 pt: made 6 inches wide.
  CHECK(pics.Insert(Pos{0, 0}, Solid(GUID_ContainerFormatJpeg, 960, 480), fw::Emu(6 * 914400)).ok);
  uint32_t d = Shown(*doc);
  CHECK(d != kNone);
  if (d == kNone) return;
  CHECK_EQ(doc->Drawings()[d].cx.Value(), 6 * 914400);
  CHECK_EQ(doc->Drawings()[d].cy.Value(), 3 * 914400);
  CHECK(doc->Package().FindPart("/word/media/image1.jpeg") != nullptr);
  // A BMP goes in as PNG.
  CHECK(pics.Insert(Pos{0, 1}, Solid(GUID_ContainerFormatBmp, 8, 8)).ok);
  CHECK(doc->Package().FindPart("/word/media/image2.png") != nullptr);
  CHECK(doc->Package().FindPart("/word/media/image2.bmp") == nullptr);
}

TEST(Pictures_SvgGetsAPngCopy) {
  auto doc = Blank();
  if (!doc) return;
  Editor ed(*doc);
  Pictures pics(ed);
  std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"40\" height=\"20\"><rect width=\"40\" height=\"20\" fill=\"#36c\"/></svg>";
  CHECK(pics.Insert(Pos{0, 0}, std::vector<uint8_t>(svg.begin(), svg.end())).ok);
  uint32_t d = Shown(*doc);
  CHECK(d != kNone);
  if (d == kNone) return;
  CHECK(doc->Package().FindPart("/word/media/image1.png") != nullptr);
  CHECK(doc->Package().FindPart("/word/media/image2.svg") != nullptr);
  CHECK(Xml(*doc, d).find("svgBlip") != std::string::npos);
  CHECK(doc->Package().Types().LookupDefault("/word/media/image2.svg") == "image/svg+xml");
  CHECK(SaveAndRead(*doc, ed) != nullptr);
}

TEST(Pictures_RefusesWhatIsntAPicture) {
  auto doc = Blank();
  if (!doc) return;
  Editor ed(*doc);
  Pictures pics(ed);
  std::string text = "plain text, not a picture";
  auto r = pics.Insert(Pos{0, 0}, std::vector<uint8_t>(text.begin(), text.end()));
  CHECK(!r.ok && !r.message.empty());
  // A PNG that claims more than the limits allow.
  std::vector<uint8_t> huge = Solid(GUID_ContainerFormatPng, 4, 4);
  huge[16] = 0x7F;  // IHDR width: 2 billion pixels
  CHECK(!pics.Insert(Pos{0, 0}, huge).ok);
  CHECK(doc->Para(0).text.empty());
  CHECK(!ed.CanUndo());
  CHECK(doc->Package().FindPart("/word/media/image1.png") == nullptr);
}

TEST(Pictures_ChangesEditOnlyTheirXmlAndUndoExactly) {
  auto doc = Blank();
  if (!doc) return;
  Editor ed(*doc);
  Pictures pics(ed);
  CHECK(pics.Insert(Pos{0, 0}, Solid(GUID_ContainerFormatPng, 100, 50)).ok);
  uint32_t d = Shown(*doc);
  if (d == kNone) return;
  const std::string start = Xml(*doc, d);

  // Resize: the extent and the picture's own size, nothing else.
  CHECK(pics.Resize(d, fw::Emu(1000000), fw::Emu(500000)).ok);
  CHECK_EQ(doc->Drawings()[d].cx.Value(), 1000000);
  CHECK_EQ(Count(Xml(*doc, d), "cx=\"1000000\""), 2u);
  // Rotate 90 degrees: a 2:1 picture needs room above and below.
  CHECK(pics.Rotate(d, 90).ok);
  CHECK(Xml(*doc, d).find("rot=\"5400000\"") != std::string::npos);
  CHECK_EQ(doc->Drawings()[d].effectT.Value(), 250000);
  CHECK_EQ(doc->Drawings()[d].effectL.Value(), 0);
  CHECK(pics.Info(d).rotation == 90);
  // Crop a quarter off the left: the picture keeps its scale, so it's a quarter narrower.
  CHECK(pics.SetCrop(d, Crop{0.25, 0, 0, 0}).ok);
  CHECK_EQ(doc->Drawings()[d].cx.Value(), 750000);
  CHECK(Xml(*doc, d).find("<a:srcRect l=\"25000\"") != std::string::npos);
  CHECK(pics.Info(d).crop == (Crop{0.25, 0, 0, 0}));
  // Alt text.
  CHECK(pics.SetAltText(d, u"A blue box & \"more\"").ok);
  CHECK(pics.Info(d).altText == u"A blue box & \"more\"");
  CHECK(Xml(*doc, d).find("descr=\"A blue box &amp; &quot;more&quot;\"") != std::string::npos);
  // Floating, square, at a place on the page; then behind the text; then back in line.
  CHECK(pics.SetWrap(d, Wrap::Square, fw::Emu(914400), fw::Emu(1828800)).ok);
  const Drawing& h = doc->Drawings()[d];
  CHECK(h.anchored && h.wrap == WrapType::Square && h.hRelativeFrom == "page" && h.hOffset && h.hOffset->Value() == 914400);
  CHECK(pics.Info(d).wrap == Wrap::Square);
  CHECK(pics.SetWrap(d, Wrap::Behind).ok);
  CHECK(doc->Drawings()[d].behindDoc && doc->Drawings()[d].wrap == WrapType::None);
  CHECK(pics.SetWrap(d, Wrap::Tight).ok);
  CHECK(Xml(*doc, d).find("wrapPolygon") != std::string::npos);
  CHECK(pics.MoveFloating(d, fw::Emu(100), fw::Emu(200)).ok);
  CHECK(doc->Drawings()[d].vOffset && doc->Drawings()[d].vOffset->Value() == 200);
  // The saved file opens with it floating where it was put.
  auto back = SaveAndRead(*doc, ed);
  CHECK(back && back->Drawings().size() == 1 && back->Drawings()[0].anchored && back->Drawings()[0].wrap == WrapType::Tight);
  CHECK(pics.SetWrap(d, Wrap::Inline).ok);
  CHECK(!doc->Drawings()[d].anchored);
  CHECK(Xml(*doc, d).find("positionH") == std::string::npos && Xml(*doc, d).find("relativeHeight") == std::string::npos);
  // Reset: its own size, no crop, no turn.
  CHECK(pics.ResetSize(d).ok);
  CHECK_EQ(doc->Drawings()[d].cx.Value(), 75 * 12700);
  CHECK(Xml(*doc, d).find("rot=") == std::string::npos && Xml(*doc, d).find("l=\"25000\"") == std::string::npos);
  CHECK(SaveAndRead(*doc, ed) != nullptr);

  // Undo all of it: the XML as inserted, exactly.
  while (ed.CanUndo() && ed.UndoName() != u"Insert picture") CHECK(ed.Undo().ok);
  CHECK(Xml(*doc, d) == start);
  CHECK_EQ(doc->Drawings()[d].cx.Value(), 75 * 12700);
  CHECK(!doc->Drawings()[d].anchored);
  // And redo it all.
  while (ed.CanRedo()) CHECK(ed.Redo().ok);
  CHECK(!doc->Drawings()[d].anchored);
  CHECK(pics.Info(d).altText == u"A blue box & \"more\"");
}

TEST(Pictures_MoveTakesThePictureAlong) {
  auto doc = Blank();
  if (!doc) return;
  Editor ed(*doc);
  Pictures pics(ed);
  CHECK(ed.InsertText(Pos{0, 0}, u"one\rtwo").ok);
  CHECK(pics.Insert(Pos{0, 1}, Solid(GUID_ContainerFormatPng, 10, 10)).ok);
  uint32_t d = Shown(*doc);
  if (d == kNone) return;
  // In line, to the end of the second paragraph.
  auto r = pics.MoveInline(d, Pos{1, 3});
  CHECK(r.ok);
  CHECK(doc->Para(0).text == u"one");
  CHECK(doc->Para(1).text == u"two\xFFFC");
  CHECK(r.caret == (Pos{1, 3}));
  d = Shown(*doc);
  CHECK(d != kNone && doc->Drawings()[d].pos == (Pos{1, 3}));
  // Within its paragraph, to the start.
  r = pics.MoveInline(d, Pos{1, 0});
  CHECK(r.ok && doc->Para(1).text == u"\xFFFCtwo");
  // Floating, then dropped by the first paragraph: it's anchored there.
  d = Shown(*doc);
  CHECK(pics.SetWrap(d, Wrap::InFront, fw::Emu(0), fw::Emu(0)).ok);
  r = pics.MoveFloating(d, fw::Emu(12700), fw::Emu(25400), Pos{0, 0});
  CHECK(r.ok);
  CHECK(doc->Para(0).text == u"\xFFFCone" && doc->Para(1).text == u"two");
  d = Shown(*doc);
  CHECK(d != kNone && doc->Drawings()[d].anchored && doc->Drawings()[d].hOffset && doc->Drawings()[d].hOffset->Value() == 12700);
  CHECK(SaveAndRead(*doc, ed) != nullptr);
  // One undo step each.
  CHECK(ed.Undo().ok);
  CHECK(doc->Para(1).text == u"\xFFFCtwo");
}

TEST(Pictures_RefusesDrawingsThatArentPictures) {
  // A shape (wps) and a VML picture: shown, but not changed.
  std::string wps =
      "<w:p><w:r><w:drawing><wp:inline xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\">"
      "<wp:extent cx=\"914400\" cy=\"914400\"/><wp:docPr id=\"1\" name=\"Shape 1\"/>"
      "<a:graphic xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><a:graphicData "
      "uri=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\"><wps:wsp "
      "xmlns:wps=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\"><wps:bodyPr/></wps:wsp>"
      "</a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>"
      "<w:p><w:r><w:pict><v:shape xmlns:v=\"urn:schemas-microsoft-com:vml\" style=\"width:20pt;height:20pt\"/></w:pict></w:r></w:p>";
  doc_test::Fixture f;
  f.body = wps;
  auto doc = doc_test::LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  Pictures pics(ed);
  CHECK_EQ(doc->Drawings().size(), 2u);
  for (uint32_t d = 0; d < doc->Drawings().size(); ++d) {
    CHECK(!pics.Info(d).picture && !pics.Info(d).why.empty());
    CHECK(!pics.Resize(d, fw::Emu(100000), fw::Emu(100000)).ok);
    CHECK(!pics.SetWrap(d, Wrap::Square).ok);
  }
  CHECK(!ed.CanUndo());
}

TEST(Pictures_UnusedMediaAfterDelete) {
  auto doc = Blank();
  if (!doc) return;
  Editor ed(*doc);
  Pictures pics(ed);
  CHECK(pics.Insert(Pos{0, 0}, Solid(GUID_ContainerFormatPng, 10, 10)).ok);
  uint32_t main = MainIndex(*doc);
  std::string xml;
  CHECK(docx::WritePart(*doc, main, xml).Ok());
  auto used = docx::FindUnusedMedia(*doc, {{main, &xml}});
  CHECK(used.relationships.empty() && used.parts.empty());
  CHECK(ed.Delete(Pos{0, 0}, Pos{0, 1}).ok);
  CHECK(docx::WritePart(*doc, main, xml).Ok());
  auto unused = docx::FindUnusedMedia(*doc, {{main, &xml}});
  CHECK_EQ(unused.relationships.size(), 1u);
  CHECK(unused.parts.size() == 1 && unused.parts[0] == "/word/media/image1.png");
  // A save leaves them out of the file; the package keeps them (undo can bring the picture back).
  auto saved = SaveAndRead(*doc, ed);
  CHECK(saved);
  if (saved) {
    CHECK(saved->Package().FindPart("/word/media/image1.png") == nullptr);
    const fw::pkg::RelationshipSet* rels = saved->Package().RelationshipsOf("/word/document.xml");
    CHECK(rels && rels->Find(unused.relationships[0].second) == nullptr);
  }
  CHECK(doc->Package().FindPart("/word/media/image1.png") != nullptr);
  CHECK(ed.Undo().ok);
  CHECK(Shown(*doc) != kNone);
  saved = SaveAndRead(*doc, ed);
  CHECK(saved && saved->Package().FindPart("/word/media/image1.png") != nullptr && saved->Drawings().size() == 1);
  (void)kRels;
}

TEST(Pictures_InsertOnAThreadWithoutCom) {
  // A BMP becomes PNG and an SVG gets a PNG copy through WIC: on a thread nothing initialized COM
  // for, that works too (torture finding 16: COM was let go before the WIC objects).
  auto doc = Blank();
  if (!doc) return;
  Editor ed(*doc);
  Pictures pics(ed);
  const std::vector<uint8_t> bmp = Solid(GUID_ContainerFormatBmp, 8, 8);
  const std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"40\" height=\"20\"><rect width=\"40\" height=\"20\" fill=\"#36c\"/></svg>";
  bool bmpOk = false, svgOk = false;
  std::thread worker([&] {
    bmpOk = pics.Insert(Pos{0, 0}, bmp).ok;
    svgOk = pics.Insert(Pos{0, 1}, std::vector<uint8_t>(svg.begin(), svg.end())).ok;
  });
  worker.join();
  CHECK(bmpOk && svgOk);
  CHECK(doc->Package().FindPart("/word/media/image1.png") != nullptr);
}

TEST(Pictures_MovesStayInTheirPart) {
  // A picture's r:embed is a relationship of its own part: moving it into the notes, or from them
  // into the main text, is refused (fwtool torture finding 13), and nothing changes.
  doc_test::Fixture f;
  f.body = "<w:p><w:r><w:t>Body</w:t></w:r><w:r><w:footnoteReference w:id=\"1\"/></w:r></w:p>";
  f.footnotes =
      "<w:footnote w:type=\"separator\" w:id=\"-1\"><w:p><w:r><w:separator/></w:r></w:p></w:footnote>"
      "<w:footnote w:id=\"1\"><w:p><w:r><w:footnoteRef/><w:t>Note</w:t></w:r></w:p></w:footnote>";
  auto doc = doc_test::LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  Editor ed(*doc);
  Pictures pics(ed);
  const std::vector<uint32_t> body = doc->StoryParagraphs(doc->MainStory());
  uint32_t note = kNone;
  for (uint32_t i = 0; i < doc->Paragraphs().size(); ++i)
    if (doc->Para(i).text.find(u"Note") != std::u16string::npos) note = i;
  CHECK(!body.empty() && note != kNone);
  if (body.empty() || note == kNone) return;
  const std::vector<uint8_t> png = Solid(GUID_ContainerFormatPng, 10, 10);
  // In the note, then towards the main text.
  CHECK(pics.Insert(Pos{note, 1}, png).ok);
  uint32_t d = Shown(*doc);
  auto r = pics.MoveInline(d, Pos{body[0], 2});
  CHECK(!r.ok && !r.message.empty());
  CHECK(doc->Drawings()[d].pos.para == note);
  // In the main text, then towards the note.
  CHECK(ed.Undo().ok);
  CHECK(pics.Insert(Pos{body[0], 0}, png).ok);
  d = Shown(*doc);
  r = pics.MoveInline(d, Pos{note, 2});
  CHECK(!r.ok && doc->Drawings()[d].pos.para == body[0]);
  // Within its part it moves.
  CHECK(pics.MoveInline(d, Pos{body[0], 3}).ok);
  CHECK(SaveAndRead(*doc, ed) != nullptr);
}

TEST(Pictures_CancelGroupLeavesNoStep) {
  // A change of several steps that fails part way (a move whose delete is refused) takes itself
  // back without an undo or a redo step; groups around it keep what they did before.
  auto doc = Blank();
  if (!doc) return;
  Editor ed(*doc);
  ed.BeginGroup(u"Outer");
  CHECK(ed.InsertText(Pos{0, 0}, u"kept").ok);
  ed.BeginGroup(u"Move picture");
  CHECK(ed.InsertText(Pos{0, 4}, u" gone").ok);
  CHECK(doc->Para(0).text == u"kept gone");
  ed.CancelGroup();
  CHECK(doc->Para(0).text == u"kept");
  ed.EndGroup();
  CHECK(ed.CanUndo() && !ed.CanRedo());
  CHECK(ed.Undo().ok);
  CHECK(doc->Para(0).text.empty() && !ed.CanUndo());
  CHECK(ed.Redo().ok && doc->Para(0).text == u"kept");
  ed.BeginGroup(u"Alone");
  CHECK(ed.InsertText(Pos{0, 0}, u"x").ok);
  ed.CancelGroup();
  CHECK(doc->Para(0).text == u"kept" && ed.UndoName() != u"Alone" && !ed.CanRedo());
}

TEST(Pictures_ReplacedPictureSharedPartStays) {
  // Two pictures share a part; deleting one keeps the part for the other.
  auto doc = Blank();
  if (!doc) return;
  Editor ed(*doc);
  Pictures pics(ed);
  const std::vector<uint8_t> png = Solid(GUID_ContainerFormatPng, 10, 10);
  CHECK(pics.Insert(Pos{0, 0}, png).ok);
  CHECK(pics.Insert(Pos{0, 1}, png).ok);
  CHECK(ed.Delete(Pos{0, 0}, Pos{0, 1}).ok);
  auto saved = SaveAndRead(*doc, ed);
  CHECK(saved && saved->Package().FindPart("/word/media/image1.png") != nullptr);
}
