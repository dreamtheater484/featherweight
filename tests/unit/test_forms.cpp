// Content controls and legacy form fields (D4): reading them, filling them in, forms protection
// and inserting controls, each change one undo step that undoes exactly.
#include "TestHarness.h"

#include <string>

#include "DocFixture.h"
#include "doc/edit/Editor.h"
#include "doc/edit/Format.h"
#include "doc/edit/Forms.h"
#include "doc/edit/Inline.h"
#include "doc/edit/TreeEdit.h"
#include "pkg/Check.h"

using namespace fw;
using namespace fw::doc;
using namespace docfix;

// A check that the rest of the test needs: the test stops when it fails.
#define REQUIRE(cond) \
  do { if (!(cond)) { test::Fail(__FILE__, __LINE__, #cond); return; } } while (0)

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

std::u16string Text(const Document& doc, const edit::Range& r) {
  if (r.from.para != r.to.para) return u"(paragraphs)";
  return doc.Para(r.from.para).text.substr(r.from.offset, r.to.offset - r.from.offset);
}

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

const char* kStyles =
    "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/></w:style>"
    "<w:style w:type=\"character\" w:default=\"1\" w:styleId=\"DefaultParagraphFont\"><w:name w:val=\"Default Paragraph Font\"/>"
    "<w:uiPriority w:val=\"1\"/><w:semiHidden/><w:unhideWhenUsed/></w:style>";
const char* kPlaceholderStyle =
    "<w:style w:type=\"character\" w:styleId=\"PlaceholderText\"><w:name w:val=\"Placeholder Text\"/>"
    "<w:basedOn w:val=\"DefaultParagraphFont\"/><w:uiPriority w:val=\"99\"/><w:semiHidden/><w:rPr><w:color w:val=\"808080\"/></w:rPr></w:style>";

std::string Placeholder(const char* text) {
  return std::string("<w:r><w:rPr><w:rStyle w:val=\"PlaceholderText\"/></w:rPr><w:t>") + text + "</w:t></w:r>";
}

// Six controls: plain text, check box, drop-down, date (each inline), a locked one, and a block.
std::string ControlsBody() {
  return "<w:p><w:r><w:t xml:space=\"preserve\">Name: </w:t></w:r><w:sdt><w:sdtPr><w:id w:val=\"10\"/><w:showingPlcHdr/><w:text/></w:sdtPr>"
         "<w:sdtContent>" + Placeholder("Click or tap here to enter text.") + "</w:sdtContent></w:sdt></w:p>"
         "<w:p><w:sdt><w:sdtPr><w:id w:val=\"11\"/><w14:checkbox><w14:checked w14:val=\"0\"/>"
         "<w14:checkedState w14:val=\"2612\" w14:font=\"MS Gothic\"/><w14:uncheckedState w14:val=\"2610\" w14:font=\"MS Gothic\"/>"
         "</w14:checkbox></w:sdtPr><w:sdtContent><w:r><w:rPr><w:rFonts w:ascii=\"MS Gothic\" w:eastAsia=\"MS Gothic\" "
         "w:hAnsi=\"MS Gothic\" w:hint=\"eastAsia\"/></w:rPr><w:t>\xE2\x98\x90</w:t></w:r></w:sdtContent></w:sdt>"
         "<w:r><w:t xml:space=\"preserve\"> Agree</w:t></w:r></w:p>"
         "<w:p><w:sdt><w:sdtPr><w:id w:val=\"12\"/><w:showingPlcHdr/><w:dropDownList><w:listItem w:displayText=\"Choose an item.\" "
         "w:value=\"\"/><w:listItem w:displayText=\"Red\" w:value=\"red\"/><w:listItem w:displayText=\"Blue\" w:value=\"blue\"/>"
         "</w:dropDownList></w:sdtPr><w:sdtContent>" + Placeholder("Choose an item.") + "</w:sdtContent></w:sdt></w:p>"
         "<w:p><w:sdt><w:sdtPr><w:id w:val=\"13\"/><w:showingPlcHdr/><w:date><w:dateFormat w:val=\"d MMMM yyyy\"/>"
         "<w:lid w:val=\"en-GB\"/><w:storeMappedDataAs w:val=\"dateTime\"/><w:calendar w:val=\"gregorian\"/></w:date></w:sdtPr>"
         "<w:sdtContent>" + Placeholder("Click or tap to enter a date.") + "</w:sdtContent></w:sdt></w:p>"
         "<w:p><w:sdt><w:sdtPr><w:id w:val=\"14\"/><w:lock w:val=\"contentLocked\"/></w:sdtPr><w:sdtContent><w:r><w:t>Fixed</w:t>"
         "</w:r></w:sdtContent></w:sdt></w:p>"
         "<w:sdt><w:sdtPr><w:id w:val=\"15\"/></w:sdtPr><w:sdtContent><w:p><w:r><w:t>Block one</w:t></w:r></w:p>"
         "<w:p><w:r><w:t>Block two</w:t></w:r></w:p></w:sdtContent></w:sdt>"
         "<w:p><w:r><w:t>Outside</w:t></w:r></w:p>";
}

}  // namespace

TEST(Forms_ContentControlsAreRead) {
  Fixture f;
  f.styles = std::string(kStyles) + kPlaceholderStyle;
  f.body = ControlsBody();
  auto doc = LoadOk(f);
  REQUIRE(doc);
  CHECK(RoundTrips(*doc));  // check boxes and the rest are written back as they were
  const auto c = edit::ContentControls(*doc, doc->MainStory());
  REQUIRE(c.size() == 6);
  using K = edit::ControlKind;
  CHECK(c[0].kind == K::PlainText && c[0].placeholder && Text(*doc, c[0].content) == u"Click or tap here to enter text.");
  CHECK(c[1].kind == K::CheckBox && !c[1].checked && c[1].checkedSymbol == u'☒' && Text(*doc, c[1].content) == u"☐");
  CHECK(c[2].kind == K::DropDown && c[2].items.size() == 3 && c[2].items[1].text == u"Red" && c[2].items[1].value == "red");
  CHECK(c[3].kind == K::Date && c[3].dateFormat == u"d MMMM yyyy" && c[3].dateLang == "en-GB" && !c[3].date);
  CHECK(c[4].contentLocked && !c[4].Fillable() && c[4].kind == K::RichText);
  CHECK(c[5].block && c[5].kind == K::RichText && c[5].content.to.para == c[5].content.from.para + 1);
  const auto paras = MainParas(*doc);
  CHECK(edit::ControlAt(*doc, {paras[0], 6})->sdt == c[0].sdt && !edit::ControlAt(*doc, {paras[0], 3}));
  CHECK(edit::ControlAt(*doc, {paras[6], 3})->sdt == c[5].sdt && !edit::ControlAt(*doc, {paras[7], 0}));
  CHECK(!edit::FormsProtected(*doc));
}

TEST(Forms_FillingInContentControls) {
  Fixture f;
  f.styles = std::string(kStyles) + kPlaceholderStyle;
  f.body = ControlsBody();
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::string original = MainXml(*doc);
  edit::Editor editor(*doc);
  auto control = [&](size_t i) { return edit::ContentControls(*doc, doc->MainStory())[i]; };
  // Typed over the placeholder: it goes, its style too.
  edit::Result r = edit::FillControl(editor, control(0), u"Ada Lovelace");
  CHECK(r && Text(*doc, control(0).content) == u"Ada Lovelace" && !control(0).placeholder);
  CHECK(r.caret.offset == 6 + 12 && editor.UndoName() == u"Fill in");
  CHECK(!Has(MainXml(*doc), "<w:showingPlcHdr/><w:text/>") && Has(MainXml(*doc), "<w:t>Ada Lovelace</w:t>"));
  // Deleted down to nothing: the placeholder comes back.
  CHECK(edit::ShowPlaceholder(editor, control(0)));
  CHECK(control(0).placeholder && Text(*doc, control(0).content) == u"Click or tap here to enter text.");
  CHECK(Has(MainXml(*doc), "<w:rStyle w:val=\"PlaceholderText\"/></w:rPr><w:t>Click or tap here to enter text.</w:t>"));
  // A check box.
  CHECK(edit::SetChecked(editor, control(1), true));
  CHECK(control(1).checked && Text(*doc, control(1).content) == u"☒" && Has(MainXml(*doc), "<w14:checked w14:val=\"1\"/>"));
  CHECK(Has(MainXml(*doc), "w:hint=\"eastAsia\"/></w:rPr><w:t>\xE2\x98\x92</w:t>"));  // in its own font
  CHECK(edit::SetChecked(editor, control(1), false) && Text(*doc, control(1).content) == u"☐");
  // A drop-down list and a date.
  // Text typed beside a check box goes outside it, before it as after it.
  editor.BreakTyping();
  CHECK(editor.InsertText(control(1).content.to, u"!") && Text(*doc, control(1).content) == u"☐");
  editor.BreakTyping();
  CHECK(editor.InsertText(control(1).content.from, u"*") && Text(*doc, control(1).content) == u"☐");
  CHECK(doc->Para(control(1).content.from.para).text.starts_with(u"*☐! Agree"));
  // ... in the look of the text around it, not the box's symbol font.
  CHECK(Has(MainXml(*doc), "<w:p><w:r><w:t>*</w:t></w:r><w:sdt>") && Has(MainXml(*doc), "</w:sdt><w:r><w:t>!</w:t></w:r>"));
  CHECK(edit::PickItem(editor, control(2), 2) && Text(*doc, control(2).content) == u"Blue" && !control(2).placeholder);
  // Its first entry, "Choose an item.": the placeholder again.
  CHECK(edit::PickItem(editor, control(2), 0) && control(2).placeholder && Text(*doc, control(2).content) == u"Choose an item.");
  CHECK(edit::PickItem(editor, control(2), 1) && Text(*doc, control(2).content) == u"Red");
  CHECK(edit::PickDate(editor, control(3), {2026, 3, 5}));
  CHECK(Text(*doc, control(3).content) == u"5 March 2026" && control(3).date == (DateTime{2026, 3, 5}));
  CHECK(Has(MainXml(*doc), "<w:date w:fullDate=\"2026-03-05T00:00:00Z\">"));
  // Locked text isn't filled in.
  CHECK(!edit::FillControl(editor, control(4), u"Changed") && Text(*doc, control(4).content) == u"Fixed");
  // A block control: its paragraphs become one.
  CHECK(edit::FillControl(editor, control(5), u"All new") && Text(*doc, control(5).content) == u"All new");
  CHECK(edit::ContentControls(*doc, doc->MainStory()).size() == 6);
  auto back = SaveAndReopen(*doc);
  REQUIRE(back);
  CHECK(edit::ContentControls(*back, back->MainStory())[2].items.size() == 3);
  int steps = 0;
  while (editor.Undo()) ++steps;
  CHECK(steps == 11 && MainXml(*doc) == original);
}

TEST(Forms_LegacyFieldsAndProtection) {
  Fixture f;
  f.styles = kStyles;
  f.settings = "<w:documentProtection w:edit=\"forms\" w:enforcement=\"1\"/>";
  auto begin = [](const std::string& ff) {
    return "<w:r><w:fldChar w:fldCharType=\"begin\"><w:ffData>" + ff + "</w:ffData></w:fldChar></w:r>";
  };
  auto instr = [](const char* code) {
    return std::string("<w:r><w:instrText xml:space=\"preserve\"> ") + code + " </w:instrText></w:r>";
  };
  const std::string sep = "<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r>", end = "<w:r><w:fldChar w:fldCharType=\"end\"/></w:r>";
  f.body = "<w:p><w:r><w:t xml:space=\"preserve\">Name: </w:t></w:r>" +
           begin("<w:name w:val=\"Text1\"/><w:enabled/><w:calcOnExit w:val=\"0\"/><w:textInput/>") + instr("FORMTEXT") + sep +
           "<w:r><w:t>\xE2\x80\x82\xE2\x80\x82\xE2\x80\x82\xE2\x80\x82\xE2\x80\x82</w:t></w:r>" + end + "</w:p>"
           "<w:p>" + begin("<w:name w:val=\"Check1\"/><w:enabled/><w:calcOnExit w:val=\"0\"/><w:checkBox><w:sizeAuto/><w:default w:val=\"0\"/></w:checkBox>") +
           instr("FORMCHECKBOX") + end + "<w:r><w:t xml:space=\"preserve\"> I agree</w:t></w:r></w:p>"
           "<w:p>" + begin("<w:name w:val=\"Drop1\"/><w:enabled/><w:calcOnExit w:val=\"0\"/><w:ddList><w:listEntry w:val=\"One\"/>"
                           "<w:listEntry w:val=\"Two\"/><w:listEntry w:val=\"Three\"/></w:ddList>") +
           instr("FORMDROPDOWN") + sep + "<w:r><w:t>One</w:t></w:r>" + end + "</w:p>"
           "<w:p><w:r><w:t>Body text.</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  REQUIRE(doc);
  CHECK(RoundTrips(*doc));
  CHECK(edit::FormsProtected(*doc));
  const std::string original = MainXml(*doc);
  auto fields = [&] { return edit::FormFields(*doc, doc->MainStory()); };
  REQUIRE(fields().size() == 3);
  using K = edit::FormFieldKind;
  CHECK(fields()[0].kind == K::Text && fields()[0].name == u"Text1" && fields()[0].hasResult);
  CHECK(fields()[1].kind == K::CheckBox && !fields()[1].checked && !fields()[1].hasResult);
  CHECK(fields()[2].kind == K::DropDown && fields()[2].entries.size() == 3 && fields()[2].selected == 0);
  const auto paras = MainParas(*doc);
  edit::Editor editor(*doc);
  // Only form fields change.
  const edit::Range result = fields()[0].result;
  CHECK(edit::InFillIn(*doc, result.from, result.to) && !edit::InFillIn(*doc, {paras[3], 2}, {paras[3], 2}));
  CHECK(!editor.InsertText({paras[3], 4}, u"x"));
  CHECK(!editor.Delete({paras[3], 0}, {paras[3], 4}));
  {
    edit::TreeEdit te(editor, {paras[3], 0});
    edit::RunFormat bold;
    bold.bold = true;
    edit::FormatRuns(te, {{paras[3], 0}, {paras[3], 4}}, bold);
    CHECK(!te.Refused().empty());
  }
  CHECK(MainXml(*doc) == original && !editor.CanUndo());
  edit::Result r = editor.Replace(result.from, result.to, u"Grace");
  CHECK(r && doc->Para(paras[0]).text.find(u"Grace") != std::u16string::npos);
  CHECK(edit::SetFormCheckBox(editor, fields()[1], true) && fields()[1].checked);
  CHECK(Has(MainXml(*doc), "<w:default w:val=\"0\"/><w:checked/></w:checkBox>"));
  CHECK(edit::PickFormEntry(editor, fields()[2], 2) && fields()[2].selected == 2 && Text(*doc, fields()[2].result) == u"Three");
  // Tab goes from field to field, and round.
  const auto first = edit::NextFillIn(*doc, {paras[0], 0}, false);
  REQUIRE(first);
  CHECK(first->from.para == paras[0] && Text(*doc, *first) == u"Grace");
  const auto second = edit::NextFillIn(*doc, first->from, false);
  CHECK(second && second->from.para == paras[1]);
  const auto third = edit::NextFillIn(*doc, second->from, false);
  CHECK(third && third->from.para == paras[2]);
  const auto round = edit::NextFillIn(*doc, third->from, false);
  CHECK(round && round->from == first->from);
  const auto back = edit::NextFillIn(*doc, first->from, true);
  CHECK(back && back->from == third->from);
  auto saved = SaveAndReopen(*doc);
  REQUIRE(saved);
  CHECK(edit::FormFields(*saved, saved->MainStory())[1].checked && edit::FormsProtected(*saved));
  int steps = 0;
  while (editor.Undo()) ++steps;
  CHECK(steps == 3 && MainXml(*doc) == original);
}

TEST(Forms_InsertingContentControls) {
  Fixture f;
  f.styles = kStyles;
  f.body = "<w:p><w:r><w:t>Hello world. X Y Z</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::string original = MainXml(*doc), originalStyles = PartXml(*doc, PartKind::Styles);
  const uint32_t p = MainParas(*doc)[0];
  edit::Editor editor(*doc);
  using K = edit::ControlKind;
  REQUIRE(edit::InsertControl(editor, {p, 6}, K::PlainText));
  auto at = [&](uint32_t o) { return edit::ControlAt(*doc, {p, o}); };
  REQUIRE(at(7));
  CHECK(at(7)->kind == K::PlainText && at(7)->placeholder && Text(*doc, at(7)->content) == u"Click or tap here to enter text.");
  CHECK(doc->Para(p).text == u"Hello Click or tap here to enter text.world. X Y Z");
  const std::string xml = MainXml(*doc);
  CHECK(Has(xml, "<w:sdt><w:sdtPr><w:id w:val=\"1\"/><w:showingPlcHdr/><w:text/></w:sdtPr><w:sdtContent><w:r><w:rPr>"
                 "<w:rStyle w:val=\"PlaceholderText\"/></w:rPr><w:t>Click or tap here to enter text.</w:t></w:r></w:sdtContent></w:sdt>"));
  CHECK(Has(PartXml(*doc, PartKind::Styles), "<w:name w:val=\"Placeholder Text\"/>"));
  CHECK(editor.UndoName() == u"Content control");
  // Not inside another, nor at its ends (where typing goes into it).
  CHECK(!edit::InsertControl(editor, {p, 10}, K::RichText) && !edit::InsertControl(editor, {p, 6}, K::RichText));
  // A check box, a drop-down list with items and a date, each before its letter.
  auto end = [&](char16_t letter) { return Pos{p, static_cast<uint32_t>(doc->Para(p).text.find(letter))}; };
  REQUIRE(edit::InsertControl(editor, end(u'X'), K::CheckBox));
  auto box = edit::ControlAt(*doc, {p, end(u'X').offset - 1});
  CHECK(box && box->kind == K::CheckBox && Text(*doc, box->content) == u"☐");
  CHECK(Has(MainXml(*doc), "<w14:checkbox><w14:checked w14:val=\"0\"/><w14:checkedState w14:val=\"2612\" w14:font=\"MS Gothic\"/>"));
  CHECK(edit::SetChecked(editor, *box, true));
  REQUIRE(edit::InsertControl(editor, end(u'Y'), K::DropDown, {u"Yes", u"No & maybe"}));
  auto list = edit::ControlAt(*doc, {p, end(u'Y').offset - 1});
  CHECK(list && list->kind == K::DropDown && list->items.size() == 3 && list->items[2].value == "No & maybe");
  CHECK(list && edit::PickItem(editor, *list, 2));
  REQUIRE(edit::InsertControl(editor, end(u'Z'), K::Date));
  auto date = edit::ControlAt(*doc, {p, end(u'Z').offset - 1});
  CHECK(date && date->kind == K::Date && !date->dateFormat.empty() && edit::PickDate(editor, *date, {2026, 11, 5}));
  CHECK(edit::ContentControls(*doc, doc->MainStory()).size() == 4);
  auto back = SaveAndReopen(*doc);  // fwtool check's content-control rules hold
  REQUIRE(back);
  CHECK(edit::ContentControls(*back, back->MainStory()).size() == 4);
  int steps = 0;
  while (editor.Undo()) ++steps;
  CHECK(steps == 7 && MainXml(*doc) == original && PartXml(*doc, PartKind::Styles) == originalStyles);
}

TEST(Forms_RefusedLinkEditKeepsTheEditBefore) {
  // Torture finding 28: EditLink refused before it changed anything took back the edit before it
  // (EndGroup on an empty group, then Undo). In a protected form, its text can't be typed.
  Fixture f;
  f.styles = kStyles;
  f.settings = "<w:documentProtection w:edit=\"forms\" w:enforcement=\"1\"/>";
  f.body = "<w:p><w:r><w:fldChar w:fldCharType=\"begin\"><w:ffData><w:name w:val=\"Text1\"/><w:enabled/><w:textInput/>"
           "</w:ffData></w:fldChar></w:r><w:r><w:instrText xml:space=\"preserve\"> FORMTEXT </w:instrText></w:r>"
           "<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r><w:r><w:t>abc</w:t></w:r><w:r><w:fldChar w:fldCharType=\"end\"/></w:r></w:p>"
           "<w:p><w:hyperlink w:anchor=\"here\"><w:r><w:t>Link</w:t></w:r></w:hyperlink><w:bookmarkStart w:id=\"0\" w:name=\"here\"/>"
           "<w:bookmarkEnd w:id=\"0\"/></w:p>";
  auto doc = LoadOk(f);
  REQUIRE(doc);
  edit::Editor editor(*doc);
  const edit::Range result = edit::FormFields(*doc, doc->MainStory())[0].result;
  REQUIRE(editor.Replace(result.from, result.to, u"Grace"));
  const std::string filled = MainXml(*doc);
  const uint32_t link = MainParas(*doc)[1];
  CHECK(!edit::EditLink(editor, {link, 1}, "#here", u"New text"));
  CHECK(MainXml(*doc) == filled && editor.CanUndo() && !editor.CanRedo());
  CHECK(!edit::InsertLink(editor, {link, 4}, {link, 4}, "#here", u"More"));
  CHECK(MainXml(*doc) == filled && editor.CanUndo());
  int steps = 0;
  while (editor.Undo()) ++steps;
  CHECK(steps == 1);
}
