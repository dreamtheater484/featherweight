// Text boxes, shapes and watermarks in Word documents (milestone D4). A text box's text is a story
// of its own, edited like any other and written back into its drawing in place of each copy of it
// (the DrawingML choice and the VML fallback); text the file keeps in a way that can't be written
// back is refused. Every change is one undo step that puts the document back exactly, and the
// saved file opens with what was left.
#include <string>
#include <vector>

#include "DocFixture.h"
#include "DocTestUtil.h"
#include "PkgTestUtil.h"
#include "TestHarness.h"
#include "doc/docx/Docx.h"
#include "doc/docx/Save.h"
#include "doc/edit/Editor.h"
#include "doc/edit/Format.h"
#include "doc/edit/TreeEdit.h"
#include "pkg/Check.h"

using namespace fw::doc;
using namespace docfix;
using edit::Editor;

namespace {

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
