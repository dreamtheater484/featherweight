#include "TestHarness.h"

#include <string>

#include "DocFixture.h"
#include "doc/edit/Editor.h"
#include "doc/edit/Format.h"

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
  edit::Editor ed(*doc);
  edit::ToggleRuns(ed, {{p, 6}, {p, 11}}, edit::Toggle::Bold);
  CHECK(!At(*doc, p, 0).bold && At(*doc, p, 6).bold && At(*doc, p, 10).bold);
  CHECK(At(*doc, p, 0).italic && At(*doc, p, 6).italic);  // the split run keeps its properties
  std::string xml = MainXml(*doc);
  CHECK(Has(xml, "<w:t xml:space=\"preserve\">Hello </w:t>"));
  CHECK(Has(xml, "<w:rPr><w:b/><w:bCs/><w:i/></w:rPr><w:t>world</w:t>"));
  CHECK(ed.CanUndo() && ed.UndoLabel() == L"Bold");
  CHECK(ed.Undo());
  CHECK(MainXml(*doc) == original);
  CHECK(!At(*doc, p, 6).bold);
  CHECK(ed.Redo());
  CHECK(MainXml(*doc) == xml);
  CHECK(At(*doc, p, 6).bold);
  CHECK(ed.Undo() && MainXml(*doc) == original);
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
  edit::Editor ed(*doc);
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
  edit::Editor ed(*doc);
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
  edit::Editor ed(*doc);
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
  while (ed.Undo()) ++undone;
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
  edit::Editor ed(*doc);
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
  edit::Editor ed(*doc);
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
  edit::Editor ed(*doc);
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
  edit::Editor ed(*doc);
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
  edit::Editor ed(*doc);
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
  edit::Editor ed(*doc);
  edit::ClearRunFormatting(ed, Whole(*doc, p));
  RunProps r = At(*doc, p, 0);
  CHECK(r.italic && r.underline == Underline::None && r.size.Value() == 22 && r.bold);  // the character style stays
  edit::ClearParaFormatting(ed, Whole(*doc, p));
  CHECK(doc->ResolvedPara(p).jc == Justification::Start && doc->ResolvedPara(p).style == "Strong");
  ed.Undo();
  ed.Undo();
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
  edit::Editor ed(*doc);
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
