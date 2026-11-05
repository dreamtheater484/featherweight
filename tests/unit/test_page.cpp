#include <windows.h>
#include <objbase.h>

#include "TestHarness.h"

#include <string>
#include <vector>

#include "DocFixture.h"
#include "doc/edit/Editor.h"
#include "doc/edit/Format.h"
#include "doc/edit/Page.h"
#include "doc/edit/Pictures.h"
#include "doc/edit/TreeEdit.h"
#include "pkg/Check.h"

using namespace fw;
using namespace fw::doc;
using namespace docfix;
namespace edit = fw::doc::edit;

#define REQUIRE(cond) do { if (!(cond)) { test::Fail(__FILE__, __LINE__, #cond); return; } } while (0)

// Page layout (D3): page setup, columns, section, page and column breaks, hyphenation.

namespace {

std::vector<uint32_t> MainParas(const Document& doc) { return doc.StoryParagraphs(doc.MainStory()); }

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
size_t Count(const std::string& xml, std::string_view what) {
  size_t n = 0;
  for (size_t at = xml.find(what); at != std::string::npos; at = xml.find(what, at + 1)) ++n;
  return n;
}

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

std::string P(const std::string& text, const std::string& pPr = "") {
  return "<w:p>" + (pPr.empty() ? "" : "<w:pPr>" + pPr + "</w:pPr>") + "<w:r><w:t xml:space=\"preserve\">" + text +
         "</w:t></w:r></w:p>";
}

// Two sections: the first with a header of its own and kept markup Featherweight doesn't know, the
// second (the body's) with none, so it shows the first one's header.
const char* kFirstSect =
    "<w:sectPr><w:headerReference w:type=\"default\" r:id=\"rIdHeader\"/><w:type w:val=\"continuous\"/>"
    "<w:pgSz w:w=\"12240\" w:h=\"15840\" w:code=\"1\"/>"
    "<w:pgMar w:top=\"1000\" w:right=\"1100\" w:bottom=\"1200\" w:left=\"1300\" w:header=\"700\" w:footer=\"700\" w:gutter=\"0\"/>"
    "<w:pgNumType w:start=\"5\"/><w:cols w:space=\"720\"/><x:kept x:a=\"1\"/></w:sectPr>";
const char* kLastSect =
    "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/>"
    "<w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"708\" w:footer=\"708\" w:gutter=\"0\"/>"
    "<w:cols w:space=\"708\"/></w:sectPr>";

Fixture TwoSections() {
  Fixture f;
  f.header = "<w:p><w:r><w:t>Head</w:t></w:r></w:p>";
  f.body = P("One") + P("Two", kFirstSect) + P("Three") + P("Four") + kLastSect;
  return f;
}

}  // namespace

TEST(Page_SetupTurnsThePageAndUndoes) {
  Fixture f = TwoSections();
  auto doc = LoadOk(f);
  REQUIRE(doc);
  REQUIRE(doc->Sections().size() == 2);
  const std::string original = MainXml(*doc);
  edit::Editor editor(*doc);
  std::vector<edit::Change> changes;
  editor.OnChange([&](const edit::Change& c) { changes.push_back(c); });

  edit::PageSetup turn;
  turn.landscape = true;
  CHECK(edit::SetPageSetup(editor, 0, 0, turn));
  const SectionProps& p = doc->Sections()[0].props;
  CHECK(p.landscape && p.pageWidth == Twips(15840) && p.pageHeight == Twips(12240));
  CHECK(p.marginTop == Twips(1300) && p.marginStart == Twips(1000) && p.marginBottom == Twips(1100) &&
        p.marginEnd == Twips(1200));
  CHECK(!doc->Sections()[1].props.landscape);  // only the first section
  const std::string xml = MainXml(*doc);
  CHECK(Has(xml, "w:orient=\"landscape\"") && Has(xml, "w:code=\"1\""));  // the same paper, turned
  CHECK(Has(xml, "<x:kept ") && Has(xml, "x:a=\"1\""));  // kept markup stays
  CHECK(Has(xml, "<w:headerReference w:type=\"default\" r:id=\"rIdHeader\"/>"));
  // The layout lays out again the first section's paragraphs, not the whole document.
  REQUIRE(!changes.empty());
  CHECK(changes.back().first == MainParas(*doc)[0] && changes.back().added == 2 && !changes.back().structure);

  CHECK(editor.Undo() && MainXml(*doc) == original);
  CHECK(editor.Redo() && MainXml(*doc) == xml);

  // The whole document: new margins, and a size Word knows by no code.
  edit::PageSetup all;
  all.width = Twips(10000);
  all.left = Twips(720);
  CHECK(edit::SetPageSetup(editor, 0, 1, all));
  CHECK(doc->Sections()[0].props.pageWidth == Twips(10000) && doc->Sections()[1].props.pageWidth == Twips(10000));
  CHECK(doc->Sections()[1].props.marginStart == Twips(720) && doc->Sections()[1].props.marginEnd == Twips(1440));
  CHECK(!Has(MainXml(*doc), "w:code="));

  // Margins that leave no room are refused, and change nothing.
  const std::string before = MainXml(*doc);
  edit::PageSetup tight;
  tight.left = Twips(5000);
  tight.right = Twips(5000);
  edit::Result r = edit::SetPageSetup(editor, 0, 1, tight);
  CHECK(!r && !r.message.empty());
  CHECK(MainXml(*doc) == before);

  auto again = SaveAndReopen(*doc);
  REQUIRE(again);
  CHECK(again->Sections().size() == 2 && again->Sections()[0].props.landscape);
}

TEST(Page_ColumnsWithALineBetweenAndOwnWidths) {
  Fixture f = TwoSections();
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::string original = MainXml(*doc);
  edit::Editor editor(*doc);
  edit::ColumnSetup two;
  two.count = 2;
  two.space = Twips(360);
  two.line = true;
  CHECK(edit::SetColumns(editor, 1, 1, two));
  const Columns& c = doc->Sections()[1].props.columns;
  CHECK(c.count == 2 && c.space == Twips(360) && c.separator && c.equalWidth);
  CHECK(Has(MainXml(*doc), "w:num=\"2\"") && Has(MainXml(*doc), "w:sep=\"1\""));

  edit::ColumnSetup own;
  own.widths = {{Twips(3000), Twips(500)}, {Twips(5000), Twips(0)}};
  CHECK(edit::SetColumns(editor, 1, 1, own));
  const Columns& d = doc->Sections()[1].props.columns;
  CHECK(d.count == 2 && !d.equalWidth && d.cols.size() == 2 && d.cols[0].width == Twips(3000) && d.cols[1].width == Twips(5000));
  CHECK(Count(MainXml(*doc), "<w:col ") == 2);
  // Back to one: the columns' own widths go.
  CHECK(edit::SetColumns(editor, 1, 1, edit::ColumnSetup{}));
  CHECK(doc->Sections()[1].props.columns.count == 1 && Count(MainXml(*doc), "<w:col ") == 0);

  // More than fit are refused.
  edit::ColumnSetup many;
  many.count = 20;
  CHECK(!edit::SetColumns(editor, 1, 1, many));
  while (editor.CanUndo()) CHECK(editor.Undo());
  CHECK(MainXml(*doc) == original);
  CHECK(SaveAndReopen(*doc));
}

TEST(Page_SectionBreaksGoInAndOut) {
  Fixture f = TwoSections();
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::string original = MainXml(*doc);
  edit::Editor editor(*doc);

  // In the middle of "Three": "Thr" ends a new section like the second, which now starts on the
  // next odd page and keeps showing the header.
  const uint32_t three = MainParas(*doc)[2];
  edit::Result r = edit::InsertSectionBreak(editor, {three, 3}, SectionStart::OddPage);
  REQUIRE(r);
  REQUIRE(doc->Sections().size() == 3);
  CHECK(doc->Para(three).text == u"Thr" && doc->Para(r.caret.para).text == u"ee");
  CHECK(doc->Sections()[1].endPara == three);
  CHECK(doc->Sections()[1].props.start == SectionStart::NextPage);  // as the second section started
  CHECK(doc->Sections()[2].props.start == SectionStart::OddPage);
  CHECK(doc->Sections()[1].props.pageWidth == Twips(11906));
  CHECK(doc->Sections()[2].headers[0] == doc->Sections()[0].headers[0] && doc->Sections()[2].headers[0] != kNone);
  CHECK(editor.UndoName() == u"Section break");
  CHECK(editor.Undo() && MainXml(*doc) == original && doc->Sections().size() == 2);
  CHECK(editor.Redo() && doc->Sections().size() == 3);
  CHECK(SaveAndReopen(*doc));

  // A break in the first section moves its page numbers' restart to the new section.
  CHECK(edit::InsertSectionBreak(editor, {MainParas(*doc)[0], 0}, SectionStart::NextPage));
  REQUIRE(doc->Sections().size() == 4);
  CHECK(doc->Sections()[0].props.pageNumberStart == 5 && !doc->Sections()[1].props.pageNumberStart);
  CHECK(doc->Para(MainParas(*doc)[0]).text.empty());  // the break on a line of its own
  CHECK(Count(MainXml(*doc), "<x:kept") == 1);           // kept markup isn't copied
  CHECK(Count(MainXml(*doc), "r:id=\"rIdHeader\"") == 2);
  CHECK(editor.Undo() && doc->Sections().size() == 3);

  // Deleting the first section's break: its text joins the next section, which goes on showing
  // the header it took from it.
  const std::string withBreaks = MainXml(*doc);
  r = edit::DeleteSectionBreak(editor, 0);
  REQUIRE(r);
  REQUIRE(doc->Sections().size() == 2);
  CHECK(doc->Sections()[0].headers[0] != kNone);
  CHECK(!Has(MainXml(*doc), "<x:kept"));  // the break's kept markup goes with it
  CHECK(editor.Undo() && MainXml(*doc) == withBreaks);

  // Delete at the end of "Two" (the break's mark) does the same.
  const uint32_t two = MainParas(*doc)[1];
  r = editor.Delete({two, 3}, {two + 1, 0});
  REQUIRE(r);
  CHECK(doc->Sections().size() == 2 && doc->Para(two).text == u"TwoThr");
  CHECK(doc->Sections()[0].headers[0] != kNone && doc->Sections()[1].headers[0] != kNone);
  CHECK(editor.UndoName() == u"Delete");
  CHECK(editor.Undo() && MainXml(*doc) == withBreaks);
  // Backspace at the start of the next paragraph too.
  CHECK(editor.JoinParagraphs(two) && doc->Sections().size() == 2);
  CHECK(SaveAndReopen(*doc));
  while (editor.CanUndo()) CHECK(editor.Undo());
  CHECK(MainXml(*doc) == original);

  // Not in a table.
  Fixture t;
  t.body = "<w:tbl><w:tblPr/><w:tblGrid><w:gridCol w:w=\"2000\"/></w:tblGrid><w:tr><w:tc>" + P("Cell") +
           "</w:tc></w:tr></w:tbl>" + P("After");
  auto tdoc = LoadOk(t);
  REQUIRE(tdoc);
  edit::Editor ted(*tdoc);
  CHECK(!edit::InsertSectionBreak(ted, {MainParas(*tdoc)[0], 2}, SectionStart::NextPage));
  CHECK(!ted.CanUndo());
}

// Each run of the paragraph lies in its text, one after another, and together they cover it.
static bool RunsCover(const Document& doc, uint32_t para) {
  uint32_t end = 0;
  for (const TextRun& run : doc.Runs(para)) {
    if (run.start < end || run.end > doc.Para(para).text.size()) return false;
    end = run.end;
  }
  return end == doc.Para(para).text.size();
}

TEST(Page_PageAndColumnBreaksStartAParagraph) {
  Fixture f;
  f.body = P("Before after");
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::string original = MainXml(*doc);
  edit::Editor editor(*doc);
  const uint32_t p = MainParas(*doc)[0];
  edit::Result r = edit::InsertBreak(editor, {p, 7}, false);
  REQUIRE(r);
  CHECK(doc->Para(p).text == u"Before \x0C" && doc->Para(r.caret.para).text == u"after");
  CHECK(RunsCover(*doc, p) && RunsCover(*doc, r.caret.para));
  CHECK(Has(MainXml(*doc), "<w:br w:type=\"page\"/>"));
  CHECK(editor.UndoName() == u"Page break");
  CHECK(editor.Undo() && MainXml(*doc) == original);
  r = edit::InsertBreak(editor, {p, 0}, true);
  REQUIRE(r);
  CHECK(Has(MainXml(*doc), "<w:br w:type=\"column\"/>"));
  CHECK(SaveAndReopen(*doc));
}

TEST(Page_OptionalAndNonBreakingHyphensAreWordsElements) {
  // Ctrl+- and Ctrl+Shift+- in the middle of a word: w:softHyphen and w:noBreakHyphen in its run,
  // as Word writes them; they read back, delete and undo like any character.
  Fixture f;
  f.body = P("Hyphenation thirtyfive");
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::string original = MainXml(*doc);
  edit::Editor editor(*doc);
  const uint32_t p = MainParas(*doc)[0];
  REQUIRE(editor.InsertText({p, 6}, u"\u00AD"));
  REQUIRE(editor.InsertText({p, 19}, u"\u2011"));
  CHECK(doc->Para(p).text == u"Hyphen\u00ADation thirty\u2011five");
  CHECK(RunsCover(*doc, p));
  const std::string xml = MainXml(*doc);
  CHECK(Has(xml, "Hyphen</w:t><w:softHyphen/><w:t xml:space=\"preserve\">ation thirty</w:t><w:noBreakHyphen/><w:t"));
  CHECK(!Has(xml, "\xC2\xAD") && !Has(xml, "\xE2\x80\x91"));  // not the characters in a w:t
  int soft = 0, hard = 0;
  for (const InlineItem& item : doc->Items(p)) {
    if (item.kind == ItemKind::SoftHyphen) ++soft;
    if (item.kind == ItemKind::NoBreakHyphen) ++hard;
  }
  CHECK(soft == 1 && hard == 1);
  {
    auto again = SaveAndReopen(*doc);
    REQUIRE(again);
    CHECK(again->Para(MainParas(*again)[0]).text == doc->Para(p).text);
  }
  REQUIRE(editor.Delete({p, 6}, {p, 7}));
  CHECK(doc->Para(p).text == u"Hyphenation thirty\u2011five" && !Has(MainXml(*doc), "softHyphen"));
  CHECK(editor.Undo() && editor.Undo() && editor.Undo());
  CHECK(MainXml(*doc) == original);
}

TEST(Page_ParagraphPanelSettingsWriteAndUndo) {
  // What the Paragraph panel (DocPage) writes over two paragraphs: line spacing at least and
  // exactly in points and multiple in lines, a hanging indent, spacing in points, and the line and
  // page breaks. Each is one undo step, and undoing them all gives back the file as it was.
  Fixture f;
  f.body = P("One") + P("Two") + P("Three");
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::string original = MainXml(*doc);
  edit::Editor editor(*doc);
  const auto p = MainParas(*doc);
  const edit::Range both{{p[0], 0}, {p[1], 1}};
  int steps = 0;
  auto apply = [&](const edit::ParaFormat& format) {
    edit::TreeEdit te(editor, both.from);
    edit::FormatParagraphs(te, both, format);
    CHECK(te.Refused().empty());
    ++steps;
  };
  edit::ParaFormat atLeast;
  atLeast.lineRule = LineRule::AtLeast;
  atLeast.line = 240;
  apply(atLeast);
  CHECK(doc->ResolvedPara(p[0]).lineRule == LineRule::AtLeast && doc->ResolvedPara(p[1]).line == 240);
  edit::ParaFormat exactly;
  exactly.lineRule = LineRule::Exact;
  exactly.line = 300;
  apply(exactly);
  CHECK(doc->ResolvedPara(p[1]).lineRule == LineRule::Exact);
  edit::ParaFormat multiple;
  multiple.lineRule = LineRule::Auto;
  multiple.line = 720;
  apply(multiple);
  edit::ParaFormat hanging;
  hanging.indStart = Twips(720);
  hanging.firstLine = Twips(-360);
  apply(hanging);
  edit::ParaFormat spacing;
  spacing.spaceBefore = Twips(360);
  spacing.spaceAfter = Twips(0);
  apply(spacing);
  edit::ParaFormat breaks;
  breaks.widowControl = false;
  breaks.keepNext = breaks.keepLines = breaks.pageBreakBefore = true;
  apply(breaks);
  for (uint32_t q : {p[0], p[1]}) {
    const ParaProps& pp = doc->ResolvedPara(q);
    CHECK(pp.lineRule == LineRule::Auto && pp.line == 720);
    CHECK(pp.indStart.Value() == 720 && pp.firstLine.Value() == -360);
    CHECK(pp.spaceBefore.Value() == 360 && pp.spaceAfter.Value() == 0);
    CHECK(!pp.widowControl && pp.keepNext && pp.keepLines && pp.pageBreakBefore);
  }
  const ParaProps& third = doc->ResolvedPara(p[2]);  // outside the selection
  CHECK(!third.keepNext && !third.pageBreakBefore && third.line == 240);
  const std::string xml = MainXml(*doc);
  CHECK(Count(xml, "<w:pageBreakBefore/>") == 2 && Count(xml, "<w:keepLines/>") == 2);
  CHECK(SaveAndReopen(*doc));
  for (int i = 0; i < steps; ++i) CHECK(editor.Undo());
  CHECK(MainXml(*doc) == original);
}

TEST(Page_BreaksInAParagraphThatEndedASectionOrWasFormatted) {
  // A tree edit in a paragraph's properties (a section break deleted, the paragraph centered) and
  // then a page break in its text: the text after the break is all in the next paragraph, in the
  // file too. (The tree edit used to give the paragraph shared copies of its runs, d2-torture 7-9.)
  Fixture f;
  f.body = P("End of section one", "<w:sectPr><w:type w:val=\"continuous\"/></w:sectPr>") + P("Next");
  for (int how = 0; how < 3; ++how) {
    auto doc = LoadOk(f);
    REQUIRE(doc);
    edit::Editor editor(*doc);
    const uint32_t p = MainParas(*doc)[0];
    if (how == 1) {
      REQUIRE(edit::DeleteSectionBreak(editor, 0));
    } else if (how == 2) {
      edit::ParaFormat centered;
      centered.jc = Justification::Center;
      edit::TreeEdit ed(editor, {p, 0});
      edit::FormatParagraphs(ed, {{p, 0}, {p, 0}}, centered);
    }
    edit::Result r = edit::InsertBreak(editor, {p, 9}, false);
    REQUIRE(r);
    CHECK(doc->Para(p).text == u"End of se\x0C" && doc->Para(r.caret.para).text == u"ction one");
    for (uint32_t q : MainParas(*doc)) CHECK(RunsCover(*doc, q));
    const std::string xml = MainXml(*doc);
    CHECK(Has(xml, ">End of se</w:t>") && Has(xml, ">ction one</w:t>"));
    // The section break, when there is one, stays with the end of the paragraph.
    CHECK(doc->Sections().size() == (how == 1 ? 1u : 2u));
    if (how != 1) CHECK(edit::SectionParagraphs(*doc, 0).second == r.caret.para);
    CHECK(SaveAndReopen(*doc));
  }
}

TEST(Page_SectionBreakAfterAPictureKeepsItAndTheTextTypedAfter) {
  // d1-corpus torture finding 12: a section break in the paragraph after an inserted picture, then
  // typing in both halves. The edited model keeps the picture, and the text it saves is the text
  // it shows (the tree edit used to leave the paragraphs stale, as in d2-torture 7-9).
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // the picture's PNG copy is made with WIC, as in the app
  Fixture f;
  f.body = P("Before the picture") + P("A paragraph to split in two");
  auto doc = LoadOk(f);
  REQUIRE(doc);
  edit::Editor editor(*doc);
  edit::Pictures pictures(editor);
  const std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"40\" height=\"20\"><rect width=\"40\" height=\"20\" fill=\"#36c\"/></svg>";
  REQUIRE(pictures.Insert({MainParas(*doc)[0], 6}, std::vector<uint8_t>(svg.begin(), svg.end())).ok);
  auto drawings = [](const Document& d, uint32_t para) {
    size_t n = 0;
    for (const InlineItem& it : d.Items(para)) n += it.kind == ItemKind::Drawing ? 1 : 0;
    return n;
  };
  CHECK(drawings(*doc, MainParas(*doc)[0]) == 1);
  REQUIRE(edit::InsertSectionBreak(editor, {MainParas(*doc)[1], 2}, SectionStart::Continuous));
  std::vector<uint32_t> ps = MainParas(*doc);
  REQUIRE(ps.size() == 3);
  CHECK(drawings(*doc, ps[0]) == 1);
  REQUIRE(editor.InsertText({ps[1], 1}, u"Featherweight"));
  REQUIRE(editor.InsertText({ps[2], 3}, u"X"));
  CHECK(doc->Para(ps[1]).text == u"AFeatherweight " && doc->Para(ps[2]).text == u"parXagraph to split in two");
  for (uint32_t q : ps) CHECK(RunsCover(*doc, q));
  auto back = SaveAndReopen(*doc);
  REQUIRE(back);
  const std::vector<uint32_t> qs = MainParas(*back);
  REQUIRE(qs.size() == 3);
  for (size_t i = 0; i < 3; ++i) CHECK(back->Para(qs[i]).text == doc->Para(ps[i]).text);
  CHECK(drawings(*back, qs[0]) == 1 && back->Sections().size() == 2);
}

TEST(Page_HyphenationSettings) {
  Fixture f;
  f.settings = "<w:zoom w:percent=\"100\"/><w:defaultTabStop w:val=\"720\"/><w:compat/>";
  f.body = P("Text");
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::string original = PartXml(*doc, PartKind::Settings);
  edit::Editor editor(*doc);
  edit::Hyphenation h;
  h.automatic = true;
  h.zone = Twips(240);
  h.limit = 2;
  h.caps = false;
  CHECK(edit::SetHyphenation(editor, h));
  const DocSettings& s = doc->Settings();
  CHECK(s.autoHyphenation && s.hyphenationZone == Twips(240) && s.consecutiveHyphenLimit == 2 && s.doNotHyphenateCaps);
  const std::string xml = PartXml(*doc, PartKind::Settings);
  CHECK(xml.find("<w:autoHyphenation") < xml.find("<w:compat"));  // where the schema puts it
  CHECK(editor.Undo() && PartXml(*doc, PartKind::Settings) == original);
  CHECK(editor.Redo());
  CHECK(SaveAndReopen(*doc));

  // A document without settings gets them.
  Fixture bare;
  bare.body = P("Text");
  auto bdoc = LoadOk(bare);
  REQUIRE(bdoc);
  edit::Editor bed(*bdoc);
  edit::Hyphenation on;
  on.automatic = true;
  CHECK(edit::SetHyphenation(bed, on));
  CHECK(bdoc->Settings().autoHyphenation);
  auto again = SaveAndReopen(*bdoc);
  REQUIRE(again);
  CHECK(again->Settings().autoHyphenation);
}
