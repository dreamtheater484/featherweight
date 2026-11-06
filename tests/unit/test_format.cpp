#include "TestHarness.h"

#include <string>

#include "DocFixture.h"
#include "doc/edit/Case.h"
#include "doc/edit/Editor.h"
#include "doc/edit/TreeEdit.h"
#include "doc/layout/Layout.h"
#include "text/Fonts.h"
#include "doc/edit/Format.h"
#include "doc/edit/Lists.h"
#include "doc/edit/Styles.h"
#include "pkg/Check.h"

using namespace fw;
using namespace fw::doc;
using namespace docfix;
namespace edit = fw::doc::edit;

namespace {

std::vector<uint32_t> MainParas(const Document& doc) { return doc.StoryParagraphs(doc.MainStory()); }

// A part as the writer writes it now.
std::string PartXml(const Document& doc, PartKind kind) {
  for (uint32_t i = 0; i < doc.Parts().size(); ++i) {
    if (doc.Parts()[i].kind != kind) continue;
    std::string out;
    if (!docx::WritePart(doc, i, out).Ok()) return "ERROR";
    return out;
  }
  return "";
}

std::string MainXml(const Document& doc) { return PartXml(doc, PartKind::Document); }

bool Has(const std::string& xml, std::string_view what) { return xml.find(what) != std::string::npos; }

// The resolved properties of the run holding character `offset` of paragraph `para`.
RunProps At(const Document& doc, uint32_t para, uint32_t offset) {
  for (const TextRun& r : doc.Runs(para))
    if (r.start <= offset && offset < r.end) return doc.ResolvedRun(para, r.run);
  return doc.ResolvedMark(para);
}

edit::Range Whole(const Document& doc, uint32_t para) {
  return {{para, 0}, {para, static_cast<uint32_t>(doc.Para(para).text.size())}};
}

const char* kBoldStyle =
    "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/>"
    "<w:rPr><w:sz w:val=\"22\"/></w:rPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Strong\"><w:name w:val=\"Strong P\"/><w:basedOn w:val=\"Normal\"/><w:rPr><w:b/></w:rPr></w:style>";

// Saves the document, checks the package as Office would, and opens it again.
std::unique_ptr<Document> SaveAndReopen(Document& doc) {
  if (!docx::WriteAllParts(doc).Ok()) return nullptr;
  fw::pkg::MemorySink sink;
  if (!doc.Package().Save(sink).Ok()) return nullptr;
  Status st;
  auto pkg = test::OpenBytes(std::move(sink.bytes), st);
  if (!pkg) return nullptr;
  fw::pkg::CheckReport report = fw::pkg::CheckPackage(*pkg);
  for (const std::string& e : report.errors) std::printf("  check: %s\n", e.c_str());
  if (!report.Ok()) return nullptr;
  return docx::Read(std::move(pkg), st);
}

}  // namespace

TEST(Edit_BoldSplitsRunsUndoesAndRedoes) {
  Fixture f;
  f.styles = kBoldStyle;
  f.body = "<w:p><w:r><w:rPr><w:i/></w:rPr><w:t>Hello world</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  std::string original = MainXml(*doc);
  uint32_t p = MainParas(*doc)[0];
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::ToggleRuns(ed, {{p, 6}, {p, 11}}, edit::Toggle::Bold);
  CHECK(!At(*doc, p, 0).bold && At(*doc, p, 6).bold && At(*doc, p, 10).bold);
  CHECK(At(*doc, p, 0).italic && At(*doc, p, 6).italic);  // the split run keeps its properties
  std::string xml = MainXml(*doc);
  CHECK(Has(xml, "<w:t xml:space=\"preserve\">Hello </w:t>"));
  CHECK(Has(xml, "<w:rPr><w:b/><w:bCs/><w:i/></w:rPr><w:t>world</w:t>"));
  CHECK(editor.CanUndo() && editor.UndoName() == u"Bold");
  CHECK(editor.Undo());
  CHECK(MainXml(*doc) == original);
  CHECK(!At(*doc, p, 6).bold);
  CHECK(editor.Redo());
  CHECK(MainXml(*doc) == xml);
  CHECK(At(*doc, p, 6).bold);
  CHECK(editor.Undo() && MainXml(*doc) == original);
}

TEST(Edit_FormattingAndTypingShareOneHistory) {
  Fixture f;
  f.styles = kBoldStyle;
  f.body = "<w:p><w:r><w:t>Hello world</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  std::string original = MainXml(*doc);
  uint32_t p = MainParas(*doc)[0];
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::ToggleRuns(ed, {{p, 0}, {p, 5}}, edit::Toggle::Bold);
  CHECK(editor.InsertText({p, 11}, u"!"));
  editor.BreakTyping();
  CHECK(editor.InsertText({p, 3}, u"p"));  // inside the bold run
  CHECK(At(*doc, p, 3).bold && !At(*doc, p, 8).bold);
  edit::ToggleRuns(ed, {{p, 0}, {p, 13}}, edit::Toggle::Italic);
  CHECK(doc->Para(p).text == u"Helplo world!" && At(*doc, p, 12).italic);
  edit::ApplyStyle(ed, {{p, 0}, {p, 0}}, "Strong P");
  CHECK(!At(*doc, p, 12).italic && At(*doc, p, 12).bold);  // the italic covered it all
  std::string last = MainXml(*doc);
  CHECK(editor.Modified() && !editor.DirtyParts().empty());
  int undone = 0;
  while (editor.Undo()) ++undone;
  CHECK(undone == 5 && MainXml(*doc) == original && !editor.Modified());
  while (editor.Redo()) --undone;
  CHECK(undone == 0 && MainXml(*doc) == last);
  CHECK(SaveAndReopen(*doc) != nullptr);
}

TEST(Edit_PropertiesFollowTheStyles) {
  Fixture f;
  f.styles = kBoldStyle;
  f.body = "<w:p><w:pPr><w:pStyle w:val=\"Strong\"/></w:pPr><w:r><w:t>Strong text</w:t></w:r></w:p>"
           "<w:p><w:r><w:rPr><w:sz w:val=\"28\"/></w:rPr><w:t>Big</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  auto paras = MainParas(*doc);
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  // Unbolding text whose style is bold needs w:b w:val="0".
  edit::ToggleRuns(ed, Whole(*doc, paras[0]), edit::Toggle::Bold);
  CHECK(!At(*doc, paras[0], 0).bold);
  CHECK(Has(MainXml(*doc), "<w:b w:val=\"0\"/>"));
  // Back to the style's size: the direct w:sz goes.
  edit::RunFormat size;
  size.size = HalfPoints(22);
  edit::FormatRuns(ed, Whole(*doc, paras[1]), size);
  CHECK(At(*doc, paras[1], 0).size.Value() == 22);
  CHECK(!Has(MainXml(*doc), "w:val=\"28\""));
}

TEST(Edit_SharedPropertySetsAreCopiedFirst) {
  Fixture f;
  f.body = "<w:p><w:r><w:rPr><w:i/></w:rPr><w:t>one</w:t></w:r><w:r><w:rPr><w:i/></w:rPr><w:t>two</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  uint32_t p = MainParas(*doc)[0];
  auto runs = doc->Runs(p);
  CHECK(runs.size() == 2 && runs[0].run.Child(Name("rPr")).FirstChild() == runs[1].run.Child(Name("rPr")).FirstChild());
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::ToggleRuns(ed, {{p, 3}, {p, 6}}, edit::Toggle::Bold);
  CHECK(!At(*doc, p, 0).bold && At(*doc, p, 3).bold);
  CHECK(At(*doc, p, 0).italic && At(*doc, p, 3).italic);
}

TEST(Edit_TogglesFollowTheWholeRange) {
  Fixture f;
  f.body = "<w:p><w:r><w:t>plain </w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>bold</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  uint32_t p = MainParas(*doc)[0];
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  CHECK(!edit::StateOf(*doc, Whole(*doc, p)).bold);
  edit::ToggleRuns(ed, Whole(*doc, p), edit::Toggle::Bold);  // mixed: all on
  CHECK(At(*doc, p, 0).bold && At(*doc, p, 7).bold && edit::StateOf(*doc, Whole(*doc, p)).bold);
  edit::ToggleRuns(ed, Whole(*doc, p), edit::Toggle::Bold);  // all on: all off
  CHECK(!At(*doc, p, 0).bold && !At(*doc, p, 7).bold);
  CHECK(!Has(MainXml(*doc), "<w:b/>") && !Has(MainXml(*doc), "<w:bCs/>"));  // nothing left to say
  edit::ToggleRuns(ed, {{p, 2}, {p, 4}}, edit::Toggle::Underline);
  CHECK(At(*doc, p, 2).underline == Underline::Single && At(*doc, p, 1).underline == Underline::None);
  edit::ToggleRuns(ed, {{p, 2}, {p, 4}}, edit::Toggle::Superscript);
  CHECK(At(*doc, p, 3).vertAlign == VertAlign::Superscript);
  edit::ToggleRuns(ed, {{p, 2}, {p, 4}}, edit::Toggle::Subscript);
  CHECK(At(*doc, p, 3).vertAlign == VertAlign::Subscript);
  // Undo walks back through every step.
  int undone = 0;
  while (editor.Undo()) ++undone;
  CHECK(undone == 5);
  CHECK(At(*doc, p, 7).bold && !At(*doc, p, 0).bold && At(*doc, p, 3).vertAlign == VertAlign::Baseline);
}

TEST(Edit_FontSizeColorHighlight) {
  Fixture f;
  f.body = "<w:p><w:r><w:rPr><w:rFonts w:asciiTheme=\"minorHAnsi\" w:hAnsiTheme=\"minorHAnsi\"/></w:rPr><w:t>text</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  uint32_t p = MainParas(*doc)[0];
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::RunFormat fmt;
  fmt.font = "Georgia";
  fmt.size = HalfPoints(28);
  fmt.color = Color::Rgb(0xC00000);
  fmt.highlight = "yellow";
  edit::FormatRuns(ed, Whole(*doc, p), fmt);
  RunProps r = At(*doc, p, 0);
  CHECK(r.fonts.ascii == "Georgia" && r.fonts.hAnsi == "Georgia");
  CHECK(r.size.Value() == 28 && r.sizeCs.Value() == 28);
  CHECK(!r.color.automatic && r.color.rgb == 0xC00000);
  CHECK(r.highlight.present);
  std::string xml = MainXml(*doc);
  CHECK(!Has(xml, "asciiTheme"));  // a theme font would win over the name (Resolve.cpp D7)
  CHECK(Has(xml, "w:color w:val=\"C00000\""));
  edit::FormatState s = edit::StateOf(*doc, Whole(*doc, p));
  CHECK(s.font && *s.font == "Georgia" && s.size && s.size->Value() == 28);
  edit::RunFormat none;
  none.highlight = "none";
  none.color = Color{};
  edit::FormatRuns(ed, Whole(*doc, p), none);
  CHECK(!At(*doc, p, 0).highlight.present && At(*doc, p, 0).color.automatic);
  CHECK(!Has(MainXml(*doc), "highlight") && !Has(MainXml(*doc), "w:color"));
}

TEST(Edit_UnknownPropertiesAreKept) {
  Fixture f;
  f.body = "<w:p><w:pPr><x:keep/><w:jc w:val=\"left\"/></w:pPr><w:r><w:rPr><x:unknown/><w:i/></w:rPr><w:t>text</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  uint32_t p = MainParas(*doc)[0];
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::ToggleRuns(ed, Whole(*doc, p), edit::Toggle::Bold);
  edit::ParaFormat center;
  center.jc = Justification::Center;
  edit::FormatParagraphs(ed, Whole(*doc, p), center);
  std::string xml = MainXml(*doc);
  CHECK(Has(xml, "<x:unknown") && Has(xml, "<x:keep"));
  CHECK(Has(xml, "<w:jc w:val=\"center\"/>"));
  CHECK(doc->ResolvedPara(p).jc == Justification::Center && At(*doc, p, 0).bold);
}

TEST(Edit_ParagraphFormatting) {
  Fixture f;
  f.styles = kBoldStyle;
  f.body = "<w:p><w:r><w:t>one</w:t></w:r></w:p><w:p><w:r><w:t>two</w:t></w:r></w:p><w:p><w:r><w:t>three</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  auto paras = MainParas(*doc);
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  // From inside the first paragraph to the start of the third: two paragraphs.
  edit::Range r{{paras[0], 1}, {paras[2], 0}};
  CHECK(edit::ParagraphsOf(*doc, r).size() == 2);
  edit::ParaFormat f1;
  f1.jc = Justification::End;
  f1.indStart = Twips(720);
  f1.firstLine = Twips(-360);
  f1.spaceBefore = Twips(240);
  f1.spaceAfter = Twips(120);
  f1.line = 360;
  f1.lineRule = LineRule::Auto;
  f1.borders = edit::BorderSides::Outside;
  f1.shading = Color::Rgb(0xEEEEEE);
  edit::FormatParagraphs(ed, r, f1);
  for (uint32_t p : {paras[0], paras[1]}) {
    const ParaProps& pp = doc->ResolvedPara(p);
    CHECK(pp.jc == Justification::End && pp.indStart.Value() == 720 && pp.firstLine.Value() == -360);
    CHECK(pp.spaceBefore.Value() == 240 && pp.spaceAfter.Value() == 120 && pp.line == 360);
    CHECK(pp.borders.top.Present() && pp.borders.bottom.Present() && !pp.borders.between.Present());
    CHECK(pp.shading.Present() && pp.shading.fill.rgb == 0xEEEEEE);
  }
  CHECK(doc->ResolvedPara(paras[2]).jc == Justification::Start);
  std::string xml = MainXml(*doc);
  CHECK(Has(xml, "<w:ind w:hanging=\"360\" w:left=\"720\"/>") || Has(xml, "<w:ind w:left=\"720\" w:hanging=\"360\"/>"));
  CHECK(Has(xml, "<w:jc w:val=\"right\"/>"));
  // Back to the style: the direct properties go, and so do the elements that held them.
  edit::ParaFormat back;
  back.jc = Justification::Start;
  back.indStart = Twips(0);
  back.firstLine = Twips(0);
  back.spaceBefore = Twips(0);
  back.spaceAfter = Twips(0);
  back.line = 240;
  back.borders = edit::BorderSides::None;
  back.shading = Color{};
  edit::FormatParagraphs(ed, r, back);
  xml = MainXml(*doc);
  CHECK(!Has(xml, "w:jc") && !Has(xml, "w:ind") && !Has(xml, "w:spacing") && !Has(xml, "w:pBdr") && !Has(xml, "w:shd"));
}

TEST(Edit_MarksFollowWhenCovered) {
  Fixture f;
  f.body = "<w:p><w:r><w:t>one</w:t></w:r></w:p><w:p><w:r><w:t>two</w:t></w:r></w:p><w:p/>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  auto paras = MainParas(*doc);
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::ToggleRuns(ed, {{paras[0], 0}, {paras[1], 3}}, edit::Toggle::Bold);
  CHECK(doc->ResolvedMark(paras[0]).bold);    // covered: the selection goes past its end
  CHECK(!doc->ResolvedMark(paras[1]).bold);   // the last paragraph's mark isn't
  // In an empty paragraph, the caret formats its mark.
  edit::RunFormat big;
  big.size = HalfPoints(40);
  edit::FormatRuns(ed, {{paras[2], 0}, {paras[2], 0}}, big);
  CHECK(doc->ResolvedMark(paras[2]).size.Value() == 40);
}

TEST(Edit_GrowAndShrinkFollowWordsSizes) {
  using edit::NextFontSize;
  auto grow = [](double pt) { return NextFontSize(HalfPoints(static_cast<int>(pt * 2)), true).Value() / 2.0; };
  auto shrink = [](double pt) { return NextFontSize(HalfPoints(static_cast<int>(pt * 2)), false).Value() / 2.0; };
  CHECK(grow(10) == 11 && grow(10.5) == 11 && grow(12) == 14 && grow(28) == 36 && grow(48) == 72 && grow(72) == 80);
  CHECK(grow(80) == 90 && grow(7) == 8 && grow(7.5) == 8);
  CHECK(shrink(11) == 10 && shrink(10.5) == 10 && shrink(14) == 12 && shrink(72) == 48 && shrink(80) == 72);
  CHECK(shrink(75) == 72 && shrink(90) == 80 && shrink(8) == 7 && shrink(1) == 1);
  Fixture f;
  f.body = "<w:p><w:r><w:rPr><w:sz w:val=\"21\"/></w:rPr><w:t>a</w:t></w:r><w:r><w:t>b</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  uint32_t p = MainParas(*doc)[0];
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::GrowFont(ed, Whole(*doc, p), true);
  CHECK(At(*doc, p, 0).size.Value() == 22 && At(*doc, p, 1).size.Value() == 22);  // 10.5 and 10 (the default) grow to 11
}

TEST(Edit_ClearFormatting) {
  Fixture f;
  f.styles = kBoldStyle +
             std::string("<w:style w:type=\"character\" w:styleId=\"Emph\"><w:name w:val=\"Emph\"/><w:rPr><w:i/></w:rPr></w:style>");
  f.body = "<w:p><w:pPr><w:pStyle w:val=\"Strong\"/><w:jc w:val=\"center\"/></w:pPr>"
           "<w:r><w:rPr><w:rStyle w:val=\"Emph\"/><w:u w:val=\"single\"/><w:sz w:val=\"40\"/></w:rPr><w:t>text</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  uint32_t p = MainParas(*doc)[0];
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::ClearRunFormatting(ed, Whole(*doc, p));
  RunProps r = At(*doc, p, 0);
  CHECK(r.italic && r.underline == Underline::None && r.size.Value() == 22 && r.bold);  // the character style stays
  edit::ClearParaFormatting(ed, Whole(*doc, p));
  CHECK(doc->ResolvedPara(p).jc == Justification::Start && doc->ResolvedPara(p).style == "Strong");
  editor.Undo();
  editor.Undo();
  edit::ClearAllFormatting(ed, Whole(*doc, p));
  r = At(*doc, p, 0);
  CHECK(!r.italic && !r.bold && r.underline == Underline::None);
  CHECK(doc->ResolvedPara(p).style == "Normal" && doc->ResolvedPara(p).jc == Justification::Start);
}

TEST(Edit_IndentSteps) {
  Fixture f;
  f.body = "<w:p><w:pPr><w:ind w:left=\"300\"/></w:pPr><w:r><w:t>text</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  uint32_t p = MainParas(*doc)[0];
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::ShiftIndent(ed, Whole(*doc, p), true);
  CHECK(doc->ResolvedPara(p).indStart.Value() == 720);
  edit::ShiftIndent(ed, Whole(*doc, p), true);
  CHECK(doc->ResolvedPara(p).indStart.Value() == 1440);
  edit::ShiftIndent(ed, Whole(*doc, p), false);
  edit::ShiftIndent(ed, Whole(*doc, p), false);
  CHECK(doc->ResolvedPara(p).indStart.Value() == 0);
  edit::ShiftIndent(ed, Whole(*doc, p), false);
  CHECK(doc->ResolvedPara(p).indStart.Value() == 0);
}

TEST(Edit_StateOfAMixedRange) {
  Fixture f;
  f.body = "<w:p><w:r><w:rPr><w:b/><w:sz w:val=\"24\"/></w:rPr><w:t>ab</w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>cd</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  uint32_t p = MainParas(*doc)[0];
  edit::FormatState all = edit::StateOf(*doc, Whole(*doc, p));
  CHECK(all.bold && !all.size && all.font);
  edit::FormatState caret = edit::StateOf(*doc, {{p, 2}, {p, 2}});  // the character before the caret
  CHECK(caret.size && caret.size->Value() == 24);
  edit::FormatState start = edit::StateOf(*doc, {{p, 0}, {p, 0}});
  CHECK(start.size && start.size->Value() == 24 && start.bold);
}

// ---------- Styles ----------

namespace {

const char* kWordStyles =
    "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/>"
    "<w:rPr><w:sz w:val=\"22\"/></w:rPr></w:style>"
    "<w:style w:type=\"character\" w:default=\"1\" w:styleId=\"DefaultParagraphFont\"><w:name w:val=\"Default Paragraph Font\"/>"
    "<w:uiPriority w:val=\"1\"/><w:semiHidden/><w:unhideWhenUsed/></w:style>";

size_t StyleCount(const Document& doc) { return doc.Styles().size(); }

}  // namespace

TEST(Styles_BuiltinsComeWithWordsDefinition) {
  Fixture f;
  f.styles = kWordStyles;
  f.body = "<w:p><w:pPr><w:jc w:val=\"center\"/></w:pPr><w:r><w:rPr><w:b/></w:rPr><w:t>Report</w:t></w:r></w:p>"
           "<w:p><w:r><w:t>Body text</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  std::string styles = PartXml(*doc, PartKind::Styles), main = MainXml(*doc);
  uint32_t p = MainParas(*doc)[0];
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::ApplyStyle(ed, {{p, 2}, {p, 2}}, "Heading 1");
  CHECK(doc->ResolvedPara(p).style == "Heading1");
  CHECK(doc->ResolvedPara(p).jc == Justification::Start && doc->ResolvedPara(p).keepNext);  // direct jc gone
  CHECK(At(*doc, p, 0).size.Value() == 32 && !At(*doc, p, 0).bold);  // bold covered it all: gone
  CHECK(At(*doc, p, 0).color.rgb == 0x2F5496);
  const Style* h1 = doc->FindStyle("Heading1");
  const Style* h1c = doc->FindStyle("Heading1Char");
  CHECK(h1 && h1c && h1->link != kNone && doc->Styles()[h1->link].id == "Heading1Char" && h1c->type == StyleType::Character);
  CHECK(h1 && h1->basedOn != kNone && doc->Styles()[h1->basedOn].id == "Normal");
  CHECK(Has(PartXml(*doc, PartKind::Styles), "<w:name w:val=\"heading 1\"/>"));
  // Once it's there, it isn't added again.
  size_t count = StyleCount(*doc);
  edit::ApplyStyle(ed, {{MainParas(*doc)[1], 0}, {MainParas(*doc)[1], 0}}, "heading 1");
  CHECK(StyleCount(*doc) == count && doc->ResolvedPara(MainParas(*doc)[1]).style == "Heading1");
  // Undo takes the styles away again.
  CHECK(editor.Undo() && editor.Undo());
  CHECK(PartXml(*doc, PartKind::Styles) == styles && MainXml(*doc) == main);
  CHECK(!doc->FindStyle("Heading1"));
  CHECK(editor.Redo());
  auto again = SaveAndReopen(*doc);
  CHECK(again);
  if (again) CHECK(again->ResolvedPara(MainParas(*again)[0]).style == "Heading1");
}

TEST(Styles_TranslatedIdsAreFoundByName) {
  Fixture f;
  f.styles = std::string(kWordStyles) +
             "<w:style w:type=\"paragraph\" w:styleId=\"Kop1\"><w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/>"
             "<w:uiPriority w:val=\"9\"/><w:qFormat/><w:rPr><w:sz w:val=\"40\"/></w:rPr></w:style>";
  f.body = "<w:p><w:r><w:t>Inleiding</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  size_t count = StyleCount(*doc);
  uint32_t p = MainParas(*doc)[0];
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::ApplyStyle(ed, {{p, 0}, {p, 0}}, "Heading 1");
  CHECK(doc->ResolvedPara(p).style == "Kop1" && StyleCount(*doc) == count && At(*doc, p, 0).size.Value() == 40);
  auto gallery = edit::GalleryStyles(*doc);
  CHECK(gallery.size() > 3 && gallery[0].name == "Normal" && gallery[0].id == "Normal");
  bool kop = false, noSpacing = false;
  for (const auto& e : gallery) {
    kop |= e.id == "Kop1" && e.name == "Heading 1";
    noSpacing |= e.id.empty() && e.name == "No Spacing";
  }
  CHECK(kop && noSpacing);
}

TEST(Styles_DirectFormattingOverHalfTheParagraphGoes) {
  Fixture f;
  f.styles = kWordStyles;
  f.body = "<w:p><w:r><w:rPr><w:b/></w:rPr><w:t xml:space=\"preserve\">aaaaa </w:t></w:r>"
           "<w:r><w:rPr><w:i/></w:rPr><w:t>bb</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  uint32_t p = MainParas(*doc)[0];
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::ApplyStyle(ed, {{p, 0}, {p, 8}}, "Heading 2");
  CHECK(doc->ResolvedPara(p).style == "Heading2");
  CHECK(!At(*doc, p, 0).bold && At(*doc, p, 7).italic);
}

TEST(Styles_LinkedStylesOnPartOfAParagraph) {
  Fixture f;
  f.styles = kWordStyles;
  f.body = "<w:p><w:r><w:t>Some words here</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  uint32_t p = MainParas(*doc)[0];
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::ApplyStyle(ed, {{p, 5}, {p, 10}}, "Heading 1");
  CHECK(doc->ResolvedPara(p).style == "Normal");
  CHECK(At(*doc, p, 5).charStyle == "Heading1Char" && At(*doc, p, 5).size.Value() == 32);
  CHECK(At(*doc, p, 0).size.Value() == 22 && At(*doc, p, 0).charStyle.empty());
  edit::ApplyStyle(ed, {{p, 0}, {p, 4}}, "Strong");
  CHECK(At(*doc, p, 0).bold && At(*doc, p, 0).charStyle == "Strong");
  edit::ApplyStyle(ed, {{p, 0}, {p, 4}}, "Default Paragraph Font");
  CHECK(!At(*doc, p, 0).bold && At(*doc, p, 0).charStyle.empty());
}

TEST(Styles_ModifyAndUpdateToMatch) {
  Fixture f;
  f.styles = std::string(kWordStyles) +
             "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/>"
             "<w:next w:val=\"Normal\"/><w:qFormat/><w:rPr><w:sz w:val=\"32\"/></w:rPr></w:style>";
  f.body = "<w:p><w:pPr><w:pStyle w:val=\"Heading1\"/><w:jc w:val=\"right\"/></w:pPr>"
           "<w:r><w:rPr><w:color w:val=\"FF0000\"/></w:rPr><w:t>One</w:t></w:r></w:p>"
           "<w:p><w:pPr><w:pStyle w:val=\"Heading1\"/></w:pPr><w:r><w:t>Two</w:t></w:r></w:p>"
           "<w:p><w:r><w:t>Body</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  auto paras = MainParas(*doc);
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  // Modify Normal: everything based on it follows.
  edit::RunFormat r;
  r.size = HalfPoints(24);
  r.bold = false;  // what Normal already gives: nothing written
  edit::ParaFormat pf;
  pf.spaceAfter = Twips(160);
  edit::ModifyStyle(ed, "Normal", r, pf);
  CHECK(At(*doc, paras[2], 0).size.Value() == 24 && doc->ResolvedPara(paras[2]).spaceAfter.Value() == 160);
  CHECK(At(*doc, paras[1], 0).size.Value() == 32 && doc->ResolvedPara(paras[1]).spaceAfter.Value() == 160);
  std::string styles = PartXml(*doc, PartKind::Styles);
  CHECK(Has(styles, "<w:sz w:val=\"24\"/>") && !Has(styles, "<w:b "));
  // Update Heading 1 to match the first paragraph.
  edit::UpdateStyleToMatch(ed, {{paras[0], 1}, {paras[0], 1}});
  for (uint32_t p : {paras[0], paras[1]}) {
    CHECK(doc->ResolvedPara(p).jc == Justification::End);
    CHECK(At(*doc, p, 0).color.rgb == 0xFF0000);
  }
  std::string main = MainXml(*doc);
  CHECK(!Has(main, "w:jc") && !Has(main, "FF0000"));
  CHECK(Has(PartXml(*doc, PartKind::Styles), "FF0000"));
  CHECK(SaveAndReopen(*doc) != nullptr);
}

TEST(Styles_DocumentsWithoutAStylesPart) {
  Fixture f;
  f.body = "<w:p><w:r><w:t>Title</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  CHECK(!doc->FindPart(PartKind::Styles));
  uint32_t p = MainParas(*doc)[0];
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::ApplyStyle(ed, {{p, 0}, {p, 0}}, "Title");
  CHECK(doc->FindPart(PartKind::Styles));
  CHECK(doc->ResolvedPara(p).style == "Title" && At(*doc, p, 0).size.Value() == 56);
  CHECK(doc->DefaultStyle(StyleType::Paragraph) && doc->DefaultStyle(StyleType::Paragraph)->id == "Normal");
  auto again = SaveAndReopen(*doc);
  CHECK(again);
  if (again) CHECK(again->ResolvedPara(MainParas(*again)[0]).style == "Title");
}

// ---------- Lists ----------

namespace {

std::u16string LabelOf(const Document& doc, uint32_t para) {
  std::optional<ListLabel> l = doc.Label(para);
  return l ? l->text : u"";
}

std::string Paras(int n) {
  std::string body;
  for (int i = 0; i < n; ++i) body += "<w:p><w:r><w:t>item " + std::to_string(i) + "</w:t></w:r></w:p>";
  return body;
}

}  // namespace

TEST(Lists_BulletsInADocumentWithoutNumbering) {
  Fixture f;
  f.styles = kWordStyles;
  f.body = Paras(3);
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  CHECK(!doc->FindPart(PartKind::Numbering));
  std::string main = MainXml(*doc);
  auto paras = MainParas(*doc);
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::ToggleList(ed, {{paras[0], 2}, {paras[1], 2}}, edit::ListKind::Bullet);
  CHECK(doc->FindPart(PartKind::Numbering));
  CHECK(doc->Label(paras[0]) && doc->Label(paras[0])->bullet && doc->Label(paras[1]) && doc->Label(paras[1])->bullet);
  CHECK(!doc->Label(paras[2]));
  CHECK(doc->ResolvedPara(paras[0]).style == "ListParagraph" && doc->ResolvedPara(paras[0]).indStart.Value() == 720);
  CHECK(doc->ResolvedPara(paras[0]).firstLine.Value() == -360);
  CHECK(edit::StateOf(*doc, {{paras[0], 0}, {paras[1], 1}}).bullets);
  {
    auto again = SaveAndReopen(*doc);
    CHECK(again);
    if (again) CHECK(again->Label(MainParas(*again)[1]) && again->Label(MainParas(*again)[1])->bullet);
  }
  // The button again: out of the list, back to Normal.
  edit::ToggleList(ed, {{paras[0], 2}, {paras[1], 2}}, edit::ListKind::Bullet);
  CHECK(!doc->Label(paras[0]) && doc->ResolvedPara(paras[0]).style == "Normal");
  CHECK(editor.Undo() && editor.Undo());
  CHECK(MainXml(*doc) == main);
}

TEST(Lists_NumbersCountJoinRestartAndContinue) {
  Fixture f;
  f.styles = kWordStyles;
  f.body = Paras(4);
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  auto p = MainParas(*doc);
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::ToggleList(ed, {{p[0], 0}, {p[1], 1}}, edit::ListKind::Number);
  CHECK(LabelOf(*doc, p[0]) == u"1." && LabelOf(*doc, p[1]) == u"2.");
  edit::ToggleList(ed, {{p[3], 0}, {p[3], 0}}, edit::ListKind::Number);  // after a plain paragraph: a new list
  CHECK(LabelOf(*doc, p[3]) == u"1.");
  edit::ToggleList(ed, {{p[2], 0}, {p[2], 0}}, edit::ListKind::Number);  // right after the first list: joins it
  CHECK(LabelOf(*doc, p[2]) == u"3." && LabelOf(*doc, p[3]) == u"1.");
  edit::RestartNumbering(ed, p[1]);
  CHECK(LabelOf(*doc, p[0]) == u"1." && LabelOf(*doc, p[1]) == u"1." && LabelOf(*doc, p[2]) == u"2.");
  CHECK(edit::ContinueNumbering(ed, p[1]));
  CHECK(LabelOf(*doc, p[1]) == u"2." && LabelOf(*doc, p[2]) == u"3.");
  CHECK(!edit::ContinueNumbering(ed, p[0]));  // nothing before it
  // Tab and Shift+Tab change the level.
  edit::ShiftIndent(ed, {{p[1], 0}, {p[1], 0}}, true);
  CHECK(LabelOf(*doc, p[1]) == u"a." && LabelOf(*doc, p[2]) == u"2.");
  edit::ShiftIndent(ed, {{p[1], 0}, {p[1], 0}}, false);
  CHECK(LabelOf(*doc, p[1]) == u"2.");
  // Bullets over numbered items: they change kind, keeping their level.
  edit::ShiftIndent(ed, {{p[2], 0}, {p[2], 0}}, true);
  edit::ToggleList(ed, {{p[1], 0}, {p[2], 1}}, edit::ListKind::Bullet);
  CHECK(doc->Label(p[1]) && doc->Label(p[1])->bullet && doc->Label(p[2]) && doc->Label(p[2])->level == 1);
  CHECK(SaveAndReopen(*doc) != nullptr);
  int undone = 0;
  while (editor.Undo()) ++undone;
  CHECK(undone == 9 && !doc->Label(p[0]));
}

// H3: an edit keeps what the model worked out for the paragraphs before it (list numbers, whether
// a field's code goes on), and what comes after reads as it would from scratch.
TEST(Lists_NumbersAndFieldsAfterEnterAndFormatting) {
  Fixture f;
  f.styles = kWordStyles;
  f.body = "<w:p><w:r><w:t>first</w:t></w:r></w:p>"
           "<w:p><w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText> QUOTE</w:instrText></w:r></w:p>"
           "<w:p><w:r><w:instrText> code</w:instrText></w:r><w:r><w:fldChar w:fldCharType=\"separate\"/></w:r>"
           "<w:r><w:t>result</w:t></w:r><w:r><w:fldChar w:fldCharType=\"end\"/></w:r></w:p>" +
           Paras(5);
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  auto p = MainParas(*doc);
  edit::ToggleList(ed, {{p[3], 0}, {p[6], 1}}, edit::ListKind::Number);  // four items, the fifth not
  auto code = [&](uint32_t para) { return doc->Runs(para).front().fieldCode; };
  CHECK(code(p[2]) && !code(p[3]) && LabelOf(*doc, p[4]) == u"2.");  // passes as far as p[4]
  // Enter before the field and the list: what's after moves along.
  CHECK(editor.SplitParagraph({p[0], 2}));
  p = MainParas(*doc);
  CHECK(code(p[3]) && !code(p[4]));
  CHECK(LabelOf(*doc, p[4]) == u"1." && LabelOf(*doc, p[5]) == u"2.");  // numbered as far as p[5]
  // Enter in an item the numbers haven't reached yet.
  CHECK(editor.SplitParagraph({p[6], 2}));
  p = MainParas(*doc);
  CHECK(LabelOf(*doc, p[6]) == u"3." && LabelOf(*doc, p[7]) == u"4." && LabelOf(*doc, p[8]) == u"5.");
  CHECK(LabelOf(*doc, p[9]).empty());
  // Bold in one item: its run reads bold, the next one's doesn't, and the numbers stay.
  edit::ToggleRuns(ed, {{p[5], 0}, {p[5], 4}}, edit::Toggle::Bold);
  CHECK(doc->ResolvedRun(p[5], doc->Runs(p[5]).front().run).bold);
  CHECK(!doc->ResolvedRun(p[6], doc->Runs(p[6]).front().run).bold);
  CHECK(LabelOf(*doc, p[5]) == u"2." && LabelOf(*doc, p[8]) == u"5." && code(p[3]));
  // Joined again, and undone: as it was.
  CHECK(editor.JoinParagraphs(p[6]));
  p = MainParas(*doc);
  CHECK(LabelOf(*doc, p[7]) == u"4." && LabelOf(*doc, p[8]).empty());
  while (editor.Undo()) {
  }
  p = MainParas(*doc);
  CHECK(code(p[2]) && !code(p[3]) && !doc->Label(p[3]));
}

TEST(Lists_MultilevelLabels) {
  Fixture f;
  f.styles = kWordStyles;
  f.body = Paras(3);
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  auto p = MainParas(*doc);
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::ToggleList(ed, {{p[0], 0}, {p[2], 1}}, edit::ListKind::Multilevel);
  edit::ShiftIndent(ed, {{p[1], 0}, {p[2], 1}}, true);
  edit::ShiftIndent(ed, {{p[2], 0}, {p[2], 1}}, true);
  CHECK(LabelOf(*doc, p[0]) == u"1." && LabelOf(*doc, p[1]) == u"1.1." && LabelOf(*doc, p[2]) == u"1.1.1.");
  CHECK(edit::KindOfList(*doc, doc->ResolvedPara(p[0]).numId) == edit::ListKind::Multilevel);
}

TEST(Lists_NumberedHeadingsAreSwitchedOff) {
  Fixture f;
  f.styles = std::string(kWordStyles) +
             "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/>"
             "<w:pPr><w:numPr><w:numId w:val=\"1\"/></w:numPr></w:pPr></w:style>";
  f.numbering = "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/>"
                "<w:lvlText w:val=\"%1.\"/><w:pStyle w:val=\"Heading1\"/></w:lvl></w:abstractNum>"
                "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>";
  f.body = "<w:p><w:pPr><w:pStyle w:val=\"Heading1\"/></w:pPr><w:r><w:t>Intro</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  uint32_t p = MainParas(*doc)[0];
  CHECK(LabelOf(*doc, p) == u"1.");
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::ToggleList(ed, {{p, 0}, {p, 0}}, edit::ListKind::Number);  // already numbered: the button turns it off
  CHECK(!doc->Label(p) && doc->ResolvedPara(p).style == "Heading1");
  CHECK(Has(MainXml(*doc), "<w:numId w:val=\"0\"/>"));
}

TEST(Edit_FormattingLaysOutOnlyItsParagraphs) {
  // Character and paragraph formatting tell the layout which paragraphs changed, with their
  // neighbours, so it lays out again from their page; lists and styles change the whole document.
  Fixture f;
  f.styles = kWordStyles;
  f.body = Paras(6);
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  auto paras = MainParas(*doc);
  edit::Editor editor(*doc);
  std::vector<edit::Change> seen;
  editor.OnChange([&](const edit::Change& c) { seen.push_back(c); });
  edit::TreeEdit ed(editor);
  edit::ToggleRuns(ed, {{paras[3], 0}, {paras[3], 4}}, edit::Toggle::Bold);
  CHECK(seen.size() == 1 && seen[0].story == doc->MainStory() && seen[0].first == paras[2] && seen[0].removed == 3 &&
        seen[0].added == 3 && !seen[0].structure);
  seen.clear();
  edit::ParaFormat center;
  center.jc = Justification::Center;
  edit::FormatParagraphs(ed, {{paras[0], 0}, {paras[0], 2}}, center);
  CHECK(seen.size() == 1 && seen[0].first == paras[0] && seen[0].removed == 2 && !seen[0].structure);  // none before
  seen.clear();
  // A list renumbers what follows it: from the paragraph before to the story's end.
  edit::ToggleList(ed, {{paras[4], 0}, {paras[4], 0}}, edit::ListKind::Number);
  CHECK(!seen.empty() && !seen.back().structure && seen.back().first == paras[3] && seen.back().removed == 3);
  // A style changed changes every paragraph that has it.
  seen.clear();
  edit::RunFormat red;
  red.color = Color::Rgb(0xC00000);
  edit::ModifyStyle(ed, "Normal", red, {});
  CHECK(seen.size() == 1 && seen[0].structure && seen[0].first == 0 && seen[0].removed == doc->Paragraphs().size());
  CHECK(editor.Undo());
  // Undo reports the same, and puts the caret where the change was.
  CHECK(editor.Undo());
  seen.clear();
  edit::Result r = editor.Undo();
  CHECK(r && r.caret == (Pos{paras[0], 0}) && seen.size() == 1 && seen[0].first == paras[0] && !seen[0].structure);
  r = editor.Undo();
  CHECK(r && r.caret == (Pos{paras[3], 0}));
}

TEST(Lists_AutoFormatAsYouType) {
  using edit::DetectAutoList;
  auto one = DetectAutoList(u"1. ");
  CHECK(one && one->numFmt == "decimal" && one->lvlText == "%1." && one->start == 1 && one->length == 3);
  auto a = DetectAutoList(u"a) ");
  CHECK(a && a->numFmt == "lowerLetter" && a->lvlText == "%1)" && a->start == 1);
  auto iv = DetectAutoList(u"(iv)\t");
  CHECK(iv && iv->numFmt == "lowerRoman" && iv->lvlText == "(%1)" && iv->start == 4);
  auto c = DetectAutoList(u"C. ");
  CHECK(c && c->numFmt == "upperLetter" && c->start == 3);
  auto twelve = DetectAutoList(u"12) ");
  CHECK(twelve && twelve->start == 12);
  auto star = DetectAutoList(u"* ");
  CHECK(star && star->kind == edit::ListKind::Bullet && star->font == "Symbol");
  CHECK(!DetectAutoList(u"1.") && !DetectAutoList(u"hello ") && !DetectAutoList(u"1.2. ") && !DetectAutoList(u"(1. "));
  CHECK(!DetectAutoList(u"ab. "));  // not a Roman numeral
  Fixture f;
  f.styles = kWordStyles;
  f.body = Paras(1);
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  uint32_t p = MainParas(*doc)[0];
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::StartAutoList(ed, p, *a);
  CHECK(LabelOf(*doc, p) == u"a)");
}

// ---------- A report, formatted as in Word (D2's exit criterion) ----------

TEST(Report_FiveFormattedPages) {
  // A plain report as typed: a title, five sections with a heading, body text, a bulleted list,
  // a numbered list and a subsection. Only Normal is defined, and there's no numbering part.
  const std::string words =
      "The quarter closed with steady growth across every region, and the figures below set out where it came from, "
      "what it cost, and what the team expects for the months ahead. Each of these points is taken up again in the "
      "sections that follow, with the numbers that support it and the risks that could change them. ";
  auto para = [](const std::string& text) { return "<w:p><w:r><w:t xml:space=\"preserve\">" + text + "</w:t></w:r></w:p>"; };
  std::string body = para("Quarterly Report") + para("Results and outlook for the third quarter");
  for (int s = 1; s <= 5; ++s) {
    body += para("Section " + std::to_string(s));
    for (int i = 0; i < 6; ++i) body += para(words + words);
    for (int i = 0; i < 4; ++i) body += para("Point " + std::to_string(i + 1));
    for (int i = 0; i < 3; ++i) body += para("Step " + std::to_string(i + 1));
    body += para("Details " + std::to_string(s));
    for (int i = 0; i < 3; ++i) body += para(words);
  }
  Fixture f;
  f.styles = kWordStyles;
  f.body = body;
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const std::string original = MainXml(*doc), styles = PartXml(*doc, PartKind::Styles);
  auto p = MainParas(*doc);
  CHECK(p.size() == 2 + 5 * 18);
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  auto caret = [&](uint32_t i) { return edit::Range{{p[i], 0}, {p[i], 0}}; };
  auto span = [&](uint32_t first, uint32_t last) {
    return edit::Range{{p[first], 0}, {p[last], static_cast<uint32_t>(doc->Para(p[last]).text.size())}};
  };

  edit::ApplyStyle(ed, caret(0), "Title");
  edit::ApplyStyle(ed, caret(1), "Subtitle");
  edit::ParaFormat center;
  center.jc = Justification::Center;
  edit::FormatParagraphs(ed, span(0, 1), center);
  for (uint32_t s = 0; s < 5; ++s) {
    uint32_t h = 2 + s * 18;
    edit::ApplyStyle(ed, caret(h), "Heading 1");
    edit::ApplyStyle(ed, caret(h + 14), "heading 2");
    edit::ToggleList(ed, span(h + 7, h + 10), edit::ListKind::Bullet);
    edit::ToggleList(ed, span(h + 11, h + 13), s == 4 ? edit::ListKind::Multilevel : edit::ListKind::Number);
    edit::ParaFormat text;
    text.jc = Justification::Both;
    text.spaceAfter = Twips(160);
    if (s == 1) {
      text.line = 360;
      text.lineRule = fw::doc::LineRule::Auto;
    }
    edit::FormatParagraphs(ed, span(h + 1, h + 6), text);
  }
  // A second level in the last list, and some character formatting.
  edit::ShiftIndent(ed, caret(2 + 4 * 18 + 12), true);
  edit::ToggleRuns(ed, {{p[3], 4}, {p[3], 11}}, edit::Toggle::Bold);
  edit::ToggleRuns(ed, {{p[3], 12}, {p[3], 18}}, edit::Toggle::Italic);
  edit::RunFormat accent;
  accent.color = fw::doc::Color::Rgb(0x2F5496);
  edit::FormatRuns(ed, {{p[4], 0}, {p[4], 20}}, accent);

  // What Word would show.
  CHECK(doc->ResolvedPara(p[0]).style == "Title" && doc->ResolvedPara(p[0]).jc == Justification::Center);
  CHECK(At(*doc, p[0], 0).size.Value() == 56 && doc->ResolvedPara(p[1]).style == "Subtitle");
  for (uint32_t s = 0; s < 5; ++s) {
    uint32_t h = 2 + s * 18;
    CHECK(doc->ResolvedPara(p[h]).style == "Heading1" && At(*doc, p[h], 0).size.Value() == 32);
    CHECK(doc->ResolvedPara(p[h + 14]).style == "Heading2");
    CHECK(doc->ResolvedPara(p[h + 1]).jc == Justification::Both && doc->ResolvedPara(p[h + 1]).spaceAfter.Value() == 160);
    for (uint32_t i = 0; i < 4; ++i) CHECK(doc->Label(p[h + 7 + i]) && doc->Label(p[h + 7 + i])->bullet);
    if (s < 4) {
      CHECK(LabelOf(*doc, p[h + 11]) == u"1." && LabelOf(*doc, p[h + 12]) == u"2." && LabelOf(*doc, p[h + 13]) == u"3.");
    } else {
      CHECK(LabelOf(*doc, p[h + 11]) == u"1." && LabelOf(*doc, p[h + 12]) == u"1.1." && LabelOf(*doc, p[h + 13]) == u"2.");
    }
  }
  CHECK(doc->ResolvedPara(p[2 + 18 + 1]).line == 360);
  CHECK(At(*doc, p[3], 5).bold && At(*doc, p[3], 13).italic && !At(*doc, p[3], 0).bold);
  // Laid out, it's a report of more than five pages.
  {
    fw::text::Fonts fonts;
    fw::doc::layout::Layout layout(*doc, fonts);
    layout.LayOutAll();
    CHECK(layout.PageCount() >= 5);
  }
  // Saved, it passes the checker and reads back the same.
  auto again = SaveAndReopen(*doc);
  CHECK(again);
  if (again) {
    auto q = MainParas(*again);
    CHECK(again->ResolvedPara(q[2]).style == "Heading1" && LabelOf(*again, q[2 + 11]) == u"1.");
    CHECK(again->Label(q[2 + 7]) && again->Label(q[2 + 7])->bullet);
  }
  // And every step undoes, back to the text as it was typed.
  while (editor.Undo()) {
  }
  CHECK(MainXml(*doc) == original && PartXml(*doc, PartKind::Styles) == styles);
}

TEST(Edit_KeptExtensionsStayInSchemaOrder) {
  // d2-torture finding 3: w14 extensions end a run's properties, and w:rPr starts the run.
  Fixture f;
  f.body = "<w:p><w:r><w:rPr><w:sz w:val=\"24\"/><w14:ligatures w14:val=\"standard\"/></w:rPr><w:t>liga</w:t></w:r>"
           "<w:r><mc:AlternateContent><mc:Choice Requires=\"w14\"><w:t>alt</w:t></mc:Choice>"
           "<mc:Fallback><w:t>alt</w:t></mc:Fallback></mc:AlternateContent></w:r><w:r><w:t>end</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  uint32_t p = MainParas(*doc)[0];
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::RunFormat fmt;
  fmt.bold = true;
  fmt.underline = fw::doc::Underline::Single;
  fmt.vertAlign = fw::doc::VertAlign::Superscript;
  edit::FormatRuns(ed, Whole(*doc, p), fmt);
  std::string xml = MainXml(*doc);
  size_t liga = xml.find("<w14:ligatures");
  CHECK(liga != std::string::npos);
  for (const char* prop : {"<w:b/>", "<w:u ", "<w:vertAlign"}) {
    size_t at = xml.find(prop);
    CHECK(at != std::string::npos && at < liga);
  }
  // Text shown from mc:AlternateContent isn't edited; nothing goes after it in its run.
  CHECK(xml.find("</mc:AlternateContent><w:rPr") == std::string::npos);
  // Back to the style's size: w:sz goes, and the ligatures it carried stay.
  edit::RunFormat plain;
  plain.size = HalfPoints(20);
  edit::FormatRuns(ed, Whole(*doc, p), plain);
  xml = MainXml(*doc);
  CHECK(xml.find("<w:sz ") == std::string::npos && xml.find("<w14:ligatures") != std::string::npos);
  CHECK(xml.find("<w14:ligatures") > xml.find("<w:vertAlign"));
  edit::ClearRunFormatting(ed, Whole(*doc, p));
  CHECK(MainXml(*doc).find("<w14:ligatures") != std::string::npos);
  CHECK(SaveAndReopen(*doc) != nullptr);
  const std::string cleared = MainXml(*doc);
  CHECK(editor.Undo() && editor.Undo() && editor.Undo());
  CHECK(MainXml(*doc).find("<w:sz w:val=\"24\"/><w14:ligatures") != std::string::npos);
  CHECK(editor.Redo() && editor.Redo() && editor.Redo() && MainXml(*doc) == cleared);
}

TEST(Edit_EditorPropertiesKeepSchemaOrder) {
  // d2-torture finding 3, through the editor's SetRunProps, SetMarkProps and SetParaProps: a run's
  // w:rPr comes first, before kept mc:AlternateContent (3a), and w: properties come before the
  // w14 extensions that end w:rPr, in a run's and in a paragraph mark's (3b). Taking a property
  // out keeps the extensions that followed it.
  Fixture f;
  f.body = "<w:p><w:r><mc:AlternateContent><mc:Choice Requires=\"w16du\"><w:t>alt</w:t></mc:Choice>"
           "<mc:Fallback><w:t>alt</w:t></mc:Fallback></mc:AlternateContent></w:r><w:r><w:t>end</w:t></w:r></w:p>"
           "<w:p><w:pPr><w:rPr><w14:ligatures w14:val=\"standard\"/></w:rPr></w:pPr>"
           "<w:r><w:rPr><w14:ligatures w14:val=\"standard\"/></w:rPr><w:t>liga</w:t></w:r>"
           "<w:r><w:rPr><w:sz w:val=\"24\"/><w14:cntxtAlts/></w:rPr><w:t>alts</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  auto paras = MainParas(*doc);
  const std::string original = MainXml(*doc);
  edit::Editor editor(*doc);
  auto end = [&](uint32_t p) { return Pos{p, static_cast<uint32_t>(doc->Para(p).text.size())}; };
  edit::PropEdit red;
  red.Set("color", {{"val", "C00000"}});
  CHECK(editor.SetRunProps({paras[0], 0}, end(paras[0]), red));
  std::string xml = MainXml(*doc);
  CHECK(xml.find("</mc:AlternateContent><w:rPr") == std::string::npos);
  CHECK(Has(xml, "<w:r><w:rPr><w:color w:val=\"C00000\"/></w:rPr><mc:AlternateContent"));
  edit::PropEdit bold;
  bold.Set("b");
  CHECK(editor.SetRunProps({paras[1], 0}, end(paras[1]), bold));
  CHECK(editor.SetMarkProps(paras[1], paras[1], bold));
  edit::PropEdit center;
  center.Set("jc", {{"val", "center"}});
  CHECK(editor.SetParaProps(paras[1], paras[1], center));
  xml = MainXml(*doc);
  CHECK(Has(xml, "<w:pPr><w:jc w:val=\"center\"/><w:rPr><w:b/><w14:ligatures"));
  CHECK(Has(xml, "<w:r><w:rPr><w:b/><w14:ligatures"));
  CHECK(Has(xml, "<w:rPr><w:b/><w:sz w:val=\"24\"/><w14:cntxtAlts"));
  // A property after them goes after them too; one taken out leaves them in place.
  edit::PropEdit lang;
  lang.Set("lang", {{"val", "en-GB"}}).Remove("sz");
  CHECK(editor.SetRunProps({paras[1], 4}, end(paras[1]), lang));
  xml = MainXml(*doc);
  CHECK(Has(xml, "<w:rPr><w:b/><w:lang w:val=\"en-GB\"/><w14:cntxtAlts"));
  CHECK(SaveAndReopen(*doc) != nullptr);
  const std::string last = MainXml(*doc);
  int undone = 0;
  while (editor.Undo()) ++undone;
  CHECK(undone == 5 && MainXml(*doc) == original);
  while (editor.Redo()) --undone;
  CHECK(undone == 0 && MainXml(*doc) == last);
}

TEST(Edit_TheFirstOfASharedSetIsFormattedAlone) {
  // d2-torture finding 4: the reader shares one set of property children between identical w:pPr
  // (and w:rPr), the first container owning them. Formatting the first changes it alone, in the
  // model and in the written part.
  Fixture f;
  f.styles = kWordStyles;
  const std::string one = "<w:p><w:pPr><w:spacing w:after=\"0\"/></w:pPr><w:r><w:rPr><w:i/></w:rPr><w:t>One</w:t></w:r></w:p>";
  f.body = one + "<w:p><w:pPr><w:spacing w:after=\"0\"/></w:pPr><w:r><w:rPr><w:i/></w:rPr><w:t>Two</w:t></w:r></w:p>"
                 "<w:p><w:r><w:rPr><w:sz w:val=\"24\"/><w14:cntxtAlts/></w:rPr><w:t>Alts</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  auto paras = MainParas(*doc);
  const std::string original = MainXml(*doc);
  const std::string two = "<w:p><w:pPr><w:spacing w:after=\"0\"/></w:pPr><w:r><w:rPr><w:i/></w:rPr><w:t>Two</w:t></w:r></w:p>";
  CHECK(Has(original, two));
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::ParaFormat center;
  center.jc = Justification::Center;
  edit::FormatParagraphs(ed, Whole(*doc, paras[0]), center);
  edit::ToggleRuns(ed, Whole(*doc, paras[0]), edit::Toggle::Bold);
  edit::ApplyStyle(ed, Whole(*doc, paras[0]), "heading 1");
  // Kept content goes with the properties a container copies.
  edit::ToggleRuns(ed, Whole(*doc, paras[2]), edit::Toggle::Bold);
  std::string xml = MainXml(*doc);
  CHECK(Has(xml, two));
  CHECK(Has(xml, "<w:sz w:val=\"24\"/><w14:cntxtAlts"));
  CHECK(doc->ResolvedPara(paras[1]).jc != Justification::Center && !At(*doc, paras[1], 0).bold);
  auto again = SaveAndReopen(*doc);
  CHECK(again != nullptr);
  if (again) {
    auto p2 = MainParas(*again);
    CHECK(again->ResolvedPara(p2[0]).style == "Heading1");
    CHECK(again->ResolvedPara(p2[1]).jc != Justification::Center && !At(*again, p2[1], 0).bold &&
          again->ResolvedPara(p2[1]).style != "Heading1" && At(*again, p2[1], 0).italic);
  }
  while (editor.Undo()) {
  }
  CHECK(MainXml(*doc) == original);
}

TEST(Edit_CopiedPropertiesLeaveOutRepeats) {
  // d2-torture 3b (gouvfr): a file's <w:b/><w:b/> isn't copied into a cut run's second half, so
  // formatting part of the run adds no validator error the file had once already.
  Fixture f;
  f.body = "<w:p><w:r><w:rPr><w:b/><w:b/><w:sz w:val=\"24\"/></w:rPr><w:t xml:space=\"preserve\">Hello world</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  uint32_t p = MainParas(*doc)[0];
  const std::string original = MainXml(*doc);
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::ToggleRuns(ed, {{p, 6}, {p, 11}}, edit::Toggle::Italic);
  const std::string xml = MainXml(*doc);
  auto count = [&](const std::string& what) {
    size_t n = 0;
    for (size_t at = xml.find(what); at != std::string::npos; at = xml.find(what, at + 1)) ++n;
    return n;
  };
  CHECK(count("<w:b/><w:b/>") == 1);  // the first half keeps the file's own
  CHECK(Has(xml, "<w:rPr><w:b/><w:i/><w:iCs/><w:sz w:val=\"24\"/></w:rPr><w:t xml:space=\"preserve\">world</w:t>"));
  CHECK(At(*doc, p, 0).bold && !At(*doc, p, 0).italic && At(*doc, p, 7).bold && At(*doc, p, 7).italic);
  CHECK(SaveAndReopen(*doc) != nullptr);
  CHECK(editor.Undo() && MainXml(*doc) == original);
}

TEST(Edit_ValuesXmlCantHoldAreLeftOut) {
  // A font name with a control character (pasted into the font box) never reaches the file: the
  // part stays readable XML.
  Fixture f;
  f.body = "<w:p><w:r><w:t>Hello</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  uint32_t p = MainParas(*doc)[0];
  CHECK(!edit::XmlSafe(std::string("Bad\x01" "Font")) && edit::XmlSafe("Calibri Light"));
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::RunFormat bad;
  bad.font = std::string("Bad\x01" "Font");
  edit::FormatRuns(ed, Whole(*doc, p), bad);
  CHECK(MainXml(*doc).find("Bad") == std::string::npos);
  CHECK(SaveAndReopen(*doc) != nullptr);
}

TEST(Edit_TextEditsAfterParagraphFormattingKeepTheText) {
  // d2-torture findings 7 and 8: formatting paragraphs gave them copies of their runs that shared
  // their text and tabs, so the next text edit saved the wrong text and the model's walks lost the
  // tab (and pictures). Format, then type, split and delete, saving and reopening after each.
  Fixture f;
  f.body = "<w:p><w:r><w:t>One</w:t></w:r><w:r><w:tab/><w:t>Two</w:t></w:r></w:p><w:p><w:r><w:t>Three</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const std::string original = MainXml(*doc);
  edit::Editor editor(*doc);
  auto texts = [](const Document& d) {
    std::u16string all;
    for (uint32_t p : MainParas(d)) all += d.Para(p).text + u"|";
    return all;
  };
  auto savedTexts = [&]() {
    auto again = SaveAndReopen(*doc);
    return again ? texts(*again) : u"(not saved)";
  };
  const uint32_t first = MainParas(*doc)[0], second = MainParas(*doc)[1];
  {
    edit::TreeEdit ed(editor);
    edit::ParaFormat pf;
    pf.lineRule = LineRule::Auto;
    pf.line = 360;
    edit::FormatParagraphs(ed, {{first, 0}, {second, 5}}, pf);
  }
  size_t tabs = 0;
  for (const InlineItem& i : doc->Items(first)) tabs += i.kind == ItemKind::Tab;
  CHECK(tabs == 1);
  CHECK(editor.InsertText({first, 3}, u"xy").ok);
  CHECK(texts(*doc) == u"Onexy\tTwo|Three|" && savedTexts() == texts(*doc));
  CHECK(editor.SplitParagraph({first, 2}).ok);
  CHECK(texts(*doc) == u"On|exy\tTwo|Three|" && savedTexts() == texts(*doc));
  const uint32_t middle = MainParas(*doc)[1];
  CHECK(editor.Delete({middle, 0}, {middle, 2}).ok);
  CHECK(texts(*doc) == u"On|y\tTwo|Three|" && savedTexts() == texts(*doc));
  // And typing at the start of the other formatted paragraph.
  const uint32_t last = MainParas(*doc)[2];
  CHECK(editor.InsertText({last, 0}, u"A").ok);
  CHECK(texts(*doc) == u"On|y\tTwo|AThree|" && savedTexts() == texts(*doc));
  for (int i = 0; i < 5; ++i) CHECK(editor.Undo().ok);
  CHECK(MainXml(*doc) == original && !editor.CanUndo());
}

TEST(Edit_FormattingInATextBoxIsSaved) {
  // d2-torture finding 14: formatting a text box's text was lost on saving, then refused. From D4
  // a text box's story is written into its drawing: the formatting is saved, and undone exactly.
  Fixture f;
  f.styles = kBoldStyle;
  f.body = "<w:p><w:r><w:t>Body</w:t></w:r><w:r><w:pict><v:shape id=\"s\" style=\"position:absolute;width:144pt;"
           "height:36pt\"><v:textbox><w:txbxContent><w:p><w:r><w:t>Boxed</w:t></w:r></w:p></w:txbxContent></v:textbox>"
           "</v:shape></w:pict></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  uint32_t boxed = kNone;
  for (uint32_t s = 0; s < doc->Stories().size(); ++s)
    if (doc->Stories()[s].kind == StoryKind::TextBox && !doc->StoryParagraphs(s).empty()) boxed = doc->StoryParagraphs(s)[0];
  CHECK(boxed != kNone);
  if (boxed == kNone) return;
  const std::string original = MainXml(*doc);
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::ApplyStyle(ed, Whole(*doc, boxed), "Heading 1");  // clears direct formatting: first
  CHECK(ed.Refused().empty());
  edit::ToggleRuns(ed, Whole(*doc, boxed), edit::Toggle::Underline);
  CHECK(ed.Refused().empty());
  edit::ParaFormat pf;
  pf.jc = Justification::Center;
  edit::FormatParagraphs(ed, Whole(*doc, boxed), pf);
  CHECK(ed.Refused().empty());
  const std::string xml = MainXml(*doc);
  CHECK(Has(xml, "<w:u w:val=\"single\"/>") && Has(xml, "<w:jc w:val=\"center\"/>") && Has(xml, "Heading1"));
  // The part the text box is written in is one the app saves (d4 torture finding 35).
  const auto main = static_cast<uint32_t>(doc->FindPart(PartKind::Document) - doc->Parts().data());
  const std::vector<uint32_t> dirty = editor.DirtyParts();
  CHECK(std::find(dirty.begin(), dirty.end(), main) != dirty.end());
  auto back = SaveAndReopen(*doc);
  CHECK(back);
  if (back) {
    uint32_t again = kNone;
    for (uint32_t s = 0; s < back->Stories().size(); ++s)
      if (back->Stories()[s].kind == StoryKind::TextBox && !back->StoryParagraphs(s).empty())
        again = back->StoryParagraphs(s)[0];
    CHECK(again != kNone && back->ResolvedPara(again).jc == Justification::Center);
  }
  while (editor.CanUndo()) CHECK(editor.Undo().ok);
  CHECK(MainXml(*doc) == original && doc->Para(boxed).text == u"Boxed");
}

TEST(Edit_RefusedFormattingAddsNoPart) {
  // d2-torture finding 18: lists and styles add the numbering or styles part a document lacks
  // before they change anything, and a part isn't undone. In a text box whose text can't be
  // written back (its fallback has no copy of it), where the change is refused, they add none; in
  // the body they do.
  Fixture f;  // no styles part, no numbering part
  f.body = "<w:p><w:r><w:t>Body</w:t></w:r><w:r><mc:AlternateContent><mc:Choice Requires=\"v\"><w:pict><v:shape "
           "id=\"s\" style=\"position:absolute;width:144pt;height:36pt\"><v:textbox><w:txbxContent><w:p><w:r>"
           "<w:t>Boxed</w:t></w:r></w:p></w:txbxContent></v:textbox></v:shape></w:pict></mc:Choice><mc:Fallback>"
           "<w:pict><v:rect style=\"width:144pt;height:36pt\"/></w:pict></mc:Fallback></mc:AlternateContent></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  uint32_t boxed = kNone;
  for (uint32_t s = 0; s < doc->Stories().size(); ++s)
    if (doc->Stories()[s].kind == StoryKind::TextBox && !doc->StoryParagraphs(s).empty()) boxed = doc->StoryParagraphs(s)[0];
  CHECK(boxed != kNone);
  if (boxed == kNone) return;
  const size_t parts = doc->Parts().size(), packaged = doc->Package().Parts().size();
  CHECK(!doc->FindPart(PartKind::Numbering) && !doc->FindPart(PartKind::Styles));
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::ToggleList(ed, Whole(*doc, boxed), edit::ListKind::Bullet);
  CHECK(!ed.Refused().empty());
  edit::ToggleList(ed, Whole(*doc, boxed), edit::ListKind::Multilevel);
  CHECK(!ed.Refused().empty());
  edit::ApplyStyle(ed, Whole(*doc, boxed), "Subtitle");
  CHECK(!ed.Refused().empty());
  CHECK(doc->Parts().size() == parts && doc->Package().Parts().size() == packaged && !editor.CanUndo());
  CHECK(!doc->FindPart(PartKind::Numbering) && !doc->FindPart(PartKind::Styles));
  const uint32_t body = MainParas(*doc)[0];
  edit::ToggleList(ed, Whole(*doc, body), edit::ListKind::Bullet);
  CHECK(ed.Refused().empty() && editor.CanUndo() && doc->FindPart(PartKind::Numbering) && doc->FindPart(PartKind::Styles));
  CHECK(SaveAndReopen(*doc) != nullptr);
}

TEST(Edit_FirstPropertyGoesBeforeKeptExtensions) {
  // A w:rPr that holds only an extension: the first w: property goes before it, as the editor's
  // own property edits put it (d2-torture finding 3b).
  Fixture f;
  f.body = "<w:p><w:r><w:rPr><w14:ligatures w14:val=\"standard\"/></w:rPr><w:t>liga</w:t></w:r>"
           "<w:r><w:rPr><w:sz w:val=\"24\"/><w14:cntxtAlts/></w:rPr><w:t>alts</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  uint32_t p = MainParas(*doc)[0];
  const std::string original = MainXml(*doc);
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::ToggleRuns(ed, Whole(*doc, p), edit::Toggle::Bold);
  std::string xml = MainXml(*doc);
  CHECK(Has(xml, "<w:rPr><w:b/><w:bCs/><w14:ligatures"));
  CHECK(Has(xml, "<w:rPr><w:b/><w:bCs/><w:sz w:val=\"24\"/><w14:cntxtAlts"));
  CHECK(SaveAndReopen(*doc) != nullptr);
  CHECK(editor.Undo() && MainXml(*doc) == original);
}

TEST(Edit_TabStopsFromTheRuler) {
  Fixture f;
  f.styles = std::string(kBoldStyle) +
             "<w:style w:type=\"paragraph\" w:styleId=\"Tabbed\"><w:name w:val=\"Tabbed\"/><w:basedOn w:val=\"Normal\"/>"
             "<w:pPr><w:tabs><w:tab w:val=\"left\" w:pos=\"1440\"/><w:tab w:val=\"right\" w:pos=\"2880\"/></w:tabs></w:pPr>"
             "</w:style>";
  f.body = "<w:p><w:pPr><w:pStyle w:val=\"Tabbed\"/></w:pPr><w:r><w:t>One</w:t></w:r></w:p>"
           "<w:p><w:r><w:t>Two</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const auto paras = MainParas(*doc);
  const std::string original = MainXml(*doc);
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  auto tabs = [&](uint32_t p) { return doc->ResolvedPara(p).tabs; };
  // The style's stop at 1" dragged to 1.5" and centered: the paragraph clears it and sets its own.
  edit::ChangeTabStops(ed, Whole(*doc, paras[0]), {Twips(1440)}, {{Twips(2160), TabAlign::Center}});
  auto t = tabs(paras[0]);
  CHECK(t.size() == 2 && t[0].pos == Twips(2160) && t[0].align == TabAlign::Center);
  CHECK(t.size() == 2 && t[1].pos == Twips(2880) && t[1].align == TabAlign::End);
  CHECK(Has(MainXml(*doc), "<w:tabs><w:tab w:val=\"clear\" w:pos=\"1440\"/><w:tab w:val=\"center\" w:pos=\"2160\"/></w:tabs>"));
  CHECK(editor.UndoName() == u"Tabs");
  // Back where the style has it: the paragraph has nothing of its own.
  edit::ChangeTabStops(ed, Whole(*doc, paras[0]), {Twips(2160)}, {{Twips(1440), TabAlign::Start}});
  CHECK(MainXml(*doc) == original);
  // Both paragraphs: the style's 2" stop goes where there is one, a dotted right stop comes.
  edit::ChangeTabStops(ed, {{paras[0], 0}, {paras[1], 3}}, {Twips(2880)}, {{Twips(4320), TabAlign::End, TabLeader::Dot}});
  const std::string last = MainXml(*doc);
  CHECK(Has(last, "<w:tabs><w:tab w:val=\"clear\" w:pos=\"2880\"/><w:tab w:val=\"right\" w:leader=\"dot\" w:pos=\"4320\"/></w:tabs>"));
  CHECK(Has(last, "<w:pPr><w:tabs><w:tab w:val=\"right\" w:leader=\"dot\" w:pos=\"4320\"/></w:tabs></w:pPr><w:r><w:t>Two"));
  CHECK(tabs(paras[0]).size() == 2 && tabs(paras[1]).size() == 1 && tabs(paras[1])[0].leader == TabLeader::Dot);
  int undone = 0;
  while (editor.Undo()) ++undone;
  CHECK(undone == 3 && MainXml(*doc) == original);
  while (editor.Redo()) {
  }
  CHECK(MainXml(*doc) == last);
  auto back = SaveAndReopen(*doc);
  CHECK(back && back->ResolvedPara(MainParas(*back)[1]).tabs.size() == 1);
}

TEST(Edit_ChangeCaseKeepsEachRunsFormatting) {
  Fixture f;
  f.styles = kBoldStyle;
  f.body = "<w:p><w:r><w:t xml:space=\"preserve\">hello </w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>big</w:t></w:r>"
           "<w:r><w:t xml:space=\"preserve\"> world. don't stop</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const std::string original = MainXml(*doc);
  const uint32_t p = MainParas(*doc)[0];
  const uint32_t n = static_cast<uint32_t>(doc->Para(p).text.size());
  edit::Editor editor(*doc);
  auto change = [&](edit::TextCase c) { return edit::ChangeCase(editor, {p, 0}, {p, n}, c).ok; };
  CHECK(change(edit::TextCase::Upper) && doc->Para(p).text == u"HELLO BIG WORLD. DON'T STOP");
  // Each run keeps its own formatting: the letters change in place.
  CHECK(!At(*doc, p, 0).bold && At(*doc, p, 6).bold && At(*doc, p, 8).bold && !At(*doc, p, 9).bold);
  CHECK(Has(MainXml(*doc), "<w:rPr><w:b/></w:rPr><w:t>BIG</w:t>") && editor.UndoName() == u"Change case");
  CHECK(change(edit::TextCase::Sentence) && doc->Para(p).text == u"Hello big world. Don't stop");
  CHECK(change(edit::TextCase::Capitalize) && doc->Para(p).text == u"Hello Big World. Don't Stop");
  CHECK(change(edit::TextCase::Toggle) && doc->Para(p).text == u"hELLO bIG wORLD. dON'T sTOP");
  CHECK(change(edit::TextCase::Lower) && doc->Para(p).text == u"hello big world. don't stop");
  // Part of a word.
  CHECK(edit::ChangeCase(editor, {p, 1}, {p, 4}, edit::TextCase::Upper).ok && doc->Para(p).text.substr(0, 5) == u"hELLo");
  CHECK(SaveAndReopen(*doc) != nullptr);
  int steps = 0;
  while (editor.Undo()) ++steps;
  CHECK(steps == 6 && MainXml(*doc) == original);
  // Shift+F3's cycle.
  CHECK(edit::NextCase(u"Hello world") == edit::TextCase::Upper && edit::NextCase(u"HELLO") == edit::TextCase::Lower);
  CHECK(edit::NextCase(u"hello") == edit::TextCase::Capitalize && edit::NextCase(u"Hello World") == edit::TextCase::Upper);
}

TEST(Edit_TabLeadersAndUnchangedParagraphs) {
  Fixture f;
  f.styles = kBoldStyle;
  f.body = "<w:p><w:pPr><w:tabs><w:tab w:val=\"left\" w:pos=\"1440\"/></w:tabs></w:pPr><w:r><w:t>One</w:t></w:r></w:p>"
           "<w:p><w:r><w:t>Two</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const auto paras = MainParas(*doc);
  const std::string original = MainXml(*doc);
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  // The ruler's menu: the stop gets a dotted leader, then becomes a right stop with a line.
  edit::ChangeTabStops(ed, Whole(*doc, paras[0]), {}, {{Twips(1440), TabAlign::Start, TabLeader::Dot}});
  CHECK(Has(MainXml(*doc), "<w:tab w:val=\"left\" w:leader=\"dot\" w:pos=\"1440\"/>"));
  edit::ChangeTabStops(ed, Whole(*doc, paras[0]), {}, {{Twips(1440), TabAlign::End, TabLeader::Underscore}});
  CHECK(Has(MainXml(*doc), "<w:tab w:val=\"right\" w:leader=\"underscore\" w:pos=\"1440\"/>"));
  const auto t = doc->ResolvedPara(paras[0]).tabs;
  CHECK(t.size() == 1 && t[0].align == TabAlign::End && t[0].leader == TabLeader::Underscore);
  // Clear all across both: the second paragraph has no stop there and isn't written.
  edit::ChangeTabStops(ed, {{paras[0], 0}, {paras[1], 3}}, {Twips(1440)}, {});
  CHECK(doc->ResolvedPara(paras[0]).tabs.empty() && !Has(MainXml(*doc), "w:tabs"));
  CHECK(Has(MainXml(*doc), "<w:p><w:r><w:t>Two</w:t></w:r></w:p>"));
  // Nothing to change: no undo step.
  const std::u16string before = editor.UndoName();
  int steps = 0;
  while (editor.Undo()) ++steps;
  CHECK(steps == 3 && MainXml(*doc) == original && before == u"Tabs");
  edit::ChangeTabStops(ed, Whole(*doc, paras[1]), {Twips(720)}, {});
  CHECK(!editor.CanUndo() && MainXml(*doc) == original);
}

TEST(Edit_ThemeFontsAndColors) {
  Fixture f;
  f.theme = "<a:clrScheme name=\"c\"><a:dk1><a:sysClr val=\"windowText\" lastClr=\"000000\"/></a:dk1><a:lt1><a:srgbClr "
            "val=\"FFFFFF\"/></a:lt1><a:dk2><a:srgbClr val=\"1F497D\"/></a:dk2><a:lt2><a:srgbClr val=\"EEECE1\"/></a:lt2>"
            "<a:accent1><a:srgbClr val=\"4F81BD\"/></a:accent1><a:accent2><a:srgbClr val=\"C0504D\"/></a:accent2>"
            "</a:clrScheme><a:fontScheme name=\"f\"><a:majorFont><a:latin typeface=\"Cambria\"/><a:ea typeface=\"\"/>"
            "<a:cs typeface=\"\"/></a:majorFont><a:minorFont><a:latin typeface=\"Calibri\"/><a:ea typeface=\"\"/>"
            "<a:cs typeface=\"Arial\"/></a:minorFont></a:fontScheme>";
  f.styles = "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:rPr>"
             "<w:rFonts w:asciiTheme=\"minorHAnsi\" w:hAnsiTheme=\"minorHAnsi\" w:cstheme=\"minorBidi\"/></w:rPr></w:style>";
  f.body = "<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Arial\" w:hAnsi=\"Arial\"/></w:rPr><w:t>Title</w:t></w:r></w:p>"
           "<w:p><w:r><w:t>Body</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const auto paras = MainParas(*doc);
  const std::string original = MainXml(*doc);
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  auto run = [&](uint32_t p, auto set) {
    edit::RunFormat r;
    set(r);
    edit::FormatRuns(ed, Whole(*doc, p), r);
  };
  // +Headings: the run follows the theme's major font, in place of its own.
  run(paras[0], [](edit::RunFormat& r) { r.themeFont = edit::ThemeFontSlot::Headings; });
  CHECK(Has(MainXml(*doc), "<w:rFonts w:asciiTheme=\"majorHAnsi\" w:hAnsiTheme=\"majorHAnsi\" w:cstheme=\"majorBidi\"/>"));
  CHECK(!Has(MainXml(*doc), "Arial") && At(*doc, paras[0], 0).fonts.ascii == "Cambria");
  // +Body is what Normal gives: the run's own font goes.
  run(paras[0], [](edit::RunFormat& r) { r.themeFont = edit::ThemeFontSlot::Body; });
  CHECK(!Has(MainXml(*doc), "w:rFonts") && At(*doc, paras[0], 0).fonts.ascii == "Calibri");
  run(paras[1], [](edit::RunFormat& r) { r.themeFont = edit::ThemeFontSlot::Body; });  // already so: no font written
  CHECK(!Has(MainXml(*doc), "w:rFonts") && At(*doc, paras[1], 0).fonts.ascii == "Calibri");
  // Accent 1, lighter 40% (tint 99): the theme's color, with what it gives now ([MS-OI29500] 2.1.72's example).
  run(paras[1], [](edit::RunFormat& r) { r.themeColor = edit::ThemeColorRef{"accent1", 0x99, -1}; });
  CHECK(Has(MainXml(*doc), "<w:color w:val=\"95B3D7\" w:themeColor=\"accent1\" w:themeTint=\"99\"/>"));
  CHECK(At(*doc, paras[1], 0).color.rgb == 0x95B3D7);
  // Accent 2, darker 25% (shade BF): the tint goes.
  run(paras[1], [](edit::RunFormat& r) { r.themeColor = edit::ThemeColorRef{"accent2", -1, 0xBF}; });
  CHECK(Has(MainXml(*doc), "<w:color w:val=\"943634\" w:themeColor=\"accent2\" w:themeShade=\"BF\"/>"));
  // A plain color drops the theme's.
  run(paras[1], [](edit::RunFormat& r) { r.color = Color::Rgb(0x00B050); });
  CHECK(Has(MainXml(*doc), "<w:color w:val=\"00B050\"/>") && !Has(MainXml(*doc), "themeColor"));
  auto back = SaveAndReopen(*doc);
  CHECK(back && back->Para(MainParas(*back)[1]).text == u"Body");
  int steps = 0;
  while (editor.Undo()) ++steps;
  CHECK(steps == 6 && MainXml(*doc) == original);
}

TEST(Styles_NewStyleFromTheSelection) {
  Fixture f;
  f.styles = std::string(kWordStyles) +
             "<w:style w:type=\"paragraph\" w:styleId=\"Quote\"><w:name w:val=\"Quote\"/><w:basedOn w:val=\"Normal\"/>"
             "<w:qFormat/><w:rPr><w:i/></w:rPr></w:style>";
  f.body = "<w:p><w:pPr><w:pStyle w:val=\"Quote\"/><w:jc w:val=\"center\"/><w:spacing w:after=\"360\"/></w:pPr>"
           "<w:r><w:rPr><w:b/><w:color w:val=\"C00000\"/><w:highlight w:val=\"yellow\"/></w:rPr><w:t>Pull quote</w:t></w:r></w:p>"
           "<w:p><w:r><w:t>Body</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const auto paras = MainParas(*doc);
  const std::string originalMain = MainXml(*doc), originalStyles = PartXml(*doc, PartKind::Styles);
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  std::u16string why;
  // Names that can't be used.
  CHECK(edit::NewStyleFromSelection(ed, Whole(*doc, paras[0]), "  ", why).empty() && !why.empty());
  CHECK(edit::NewStyleFromSelection(ed, Whole(*doc, paras[0]), "quote", why).empty() && !why.empty());
  CHECK(edit::NewStyleFromSelection(ed, Whole(*doc, paras[0]), "Heading 1", why).empty() && !why.empty());
  CHECK(edit::NewStyleFromSelection(ed, Whole(*doc, paras[0]), std::string("Bad\x01name"), why).empty());
  CHECK(!editor.CanUndo() && PartXml(*doc, PartKind::Styles) == originalStyles);
  // "Pull Quote & More": based on Quote, with the paragraph's and the first character's formatting.
  const std::string id = edit::NewStyleFromSelection(ed, {{paras[0], 2}, {paras[0], 2}}, " Pull Quote & More ", why);
  CHECK(id == "PullQuote_More" && why.empty() && editor.UndoName() == u"New style");
  const Style* s = doc->FindStyle(id);
  CHECK(s && s->name == "Pull Quote & More" && s->type == StyleType::Paragraph);
  CHECK(s && s->basedOn != kNone && doc->Styles()[s->basedOn].id == "Quote");
  const std::string styles = PartXml(*doc, PartKind::Styles);
  CHECK(Has(styles, "w:styleId=\"PullQuote_More\" w:customStyle=\"1\"><w:name w:val=\"Pull Quote &amp; More\"/>"
                    "<w:basedOn w:val=\"Quote\"/><w:qFormat/>"));
  CHECK(Has(styles, "<w:jc w:val=\"center\"/>") && Has(styles, "<w:b/>") && Has(styles, "C00000"));
  // The paragraph takes it, without the formatting that moved; it looks as it did.
  CHECK(doc->ResolvedPara(paras[0]).style == id && doc->ResolvedPara(paras[0]).jc == Justification::Center);
  CHECK(doc->ResolvedPara(paras[0]).spaceAfter.Value() == 360);
  CHECK(At(*doc, paras[0], 0).bold && At(*doc, paras[0], 0).italic && At(*doc, paras[0], 0).color.rgb == 0xC00000);
  const std::string main = MainXml(*doc);
  CHECK(!Has(main, "w:jc") && !Has(main, "C00000") && Has(main, "<w:pStyle w:val=\"PullQuote_More\"/>"));
  // Highlighting stays on the text, never in a style (finding 23).
  CHECK(Has(main, "<w:highlight w:val=\"yellow\"/>") && !Has(PartXml(*doc, PartKind::Styles), "highlight"));
  CHECK(doc->ResolvedPara(paras[1]).style == "Normal");
  // It's a quick style now, and modifying it reaches the paragraph.
  bool listed = false;
  for (const auto& e : edit::GalleryStyles(*doc)) listed = listed || e.id == id;
  CHECK(listed);
  edit::RunFormat r;
  r.size = HalfPoints(30);
  r.highlight = "green";  // left out
  edit::ModifyStyle(ed, id, r, {});
  CHECK(At(*doc, paras[0], 0).size.Value() == 30 && !Has(PartXml(*doc, PartKind::Styles), "highlight"));
  // A second one of the same name is refused; another name gets its own id.
  CHECK(edit::NewStyleFromSelection(ed, Whole(*doc, paras[1]), "pull quote & more", why).empty());
  CHECK(edit::NewStyleFromSelection(ed, Whole(*doc, paras[1]), "Pull Quote&More", why) == "PullQuote_More1");
  auto back = SaveAndReopen(*doc);
  CHECK(back && back->FindStyle("PullQuote_More") && back->ResolvedPara(MainParas(*back)[0]).style == "PullQuote_More");
  int steps = 0;
  while (editor.Undo()) ++steps;
  CHECK(steps == 3 && MainXml(*doc) == originalMain && PartXml(*doc, PartKind::Styles) == originalStyles);
}

TEST(Styles_HighlightingStaysOnTheText) {
  // Torture finding 23: Word never puts highlighting in a style. Updating a style to match
  // highlighted text moves the text's other formatting into the style and leaves the highlighting
  // on the text; applying a style leaves it there too.
  Fixture f;
  f.styles = std::string(kWordStyles) +
             "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/>"
             "<w:qFormat/></w:style>";
  f.body = "<w:p><w:pPr><w:pStyle w:val=\"Heading1\"/></w:pPr>"
           "<w:r><w:rPr><w:i/><w:highlight w:val=\"yellow\"/></w:rPr><w:t>Marked</w:t></w:r></w:p>"
           "<w:p><w:pPr><w:pStyle w:val=\"Heading1\"/></w:pPr><w:r><w:t>Other</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  auto paras = MainParas(*doc);
  edit::Editor editor(*doc);
  edit::TreeEdit ed(editor);
  edit::UpdateStyleToMatch(ed, {{paras[0], 1}, {paras[0], 1}});
  CHECK(!Has(PartXml(*doc, PartKind::Styles), "highlight"));
  CHECK(Has(PartXml(*doc, PartKind::Styles), "<w:i/>"));
  CHECK(At(*doc, paras[0], 0).italic && At(*doc, paras[1], 0).italic);
  CHECK(Has(MainXml(*doc), "<w:highlight w:val=\"yellow\"/>") && !Has(MainXml(*doc), "<w:i/>"));
  edit::ApplyStyle(ed, {{paras[0], 0}, {paras[0], 0}}, "Normal");
  CHECK(Has(MainXml(*doc), "<w:highlight w:val=\"yellow\"/>"));
  CHECK(SaveAndReopen(*doc) != nullptr);
}
