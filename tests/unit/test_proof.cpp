#include "TestHarness.h"

#include <windows.h>

#include <objbase.h>

#include <string>
#include <vector>

#include "DocTestUtil.h"
#include "doc/edit/Editor.h"
#include "doc/model/Utf.h"
#include "doc/proof/AutoCorrect.h"
#include "doc/proof/Language.h"
#include "doc/proof/Spelling.h"
#include "doc/proof/WordCount.h"
#include "text/SpellCheck.h"

using namespace fw::doc;
using namespace doc_test;
using proof::AutoCorrect;
using proof::Correction;

// Proofing (milestone D2): AutoCorrect's rules and their undo, word count, the language of text,
// and spelling with Windows' spell checker.

namespace {

std::u16string U(std::string_view s) { return ToUtf16(s); }
Pos P(uint32_t para, uint32_t offset) { return Pos{para, offset}; }

// What AutoCorrect makes of `typed` text (the caret at its end, its last character just typed).
std::string Corrected(std::string_view typedUtf8, const wchar_t* lang = L"en-US") {
  std::u16string text = U(typedUtf8);
  AutoCorrect ac;
  const auto fixes = ac.Find(text, static_cast<uint32_t>(text.size()), text.empty() ? 0 : text.back(), lang);
  for (const Correction& c : fixes) text.replace(c.from, c.to - c.from, c.text);  // right to left
  return ToUtf8(text);
}

std::string ParaText(const Document& doc, uint32_t para) { return ToUtf8(doc.Para(para).text); }

std::string Run(std::string_view text, std::string_view rPr = {}) {
  return "<w:r>" + (rPr.empty() ? std::string() : "<w:rPr>" + std::string(rPr) + "</w:rPr>") +
         "<w:t xml:space=\"preserve\">" + std::string(text) + "</w:t></w:r>";
}
std::string Para(std::string_view runs) { return "<w:p>" + std::string(runs) + "</w:p>"; }

void InitCom() { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }

}  // namespace

TEST(Proof_AutoCorrectReplacesWordsAndSymbols) {
  CHECK_EQ(Corrected("Say teh "), "Say the ");
  CHECK_EQ(Corrected("teh "), "The ");  // and the paragraph's first word gets its capital
  CHECK_EQ(Corrected("I said teh,"), "I said the,");
  CHECK_EQ(Corrected("Say TEH "), "Say THE ");
  CHECK_EQ(Corrected("so i "), "so I ");
  CHECK_EQ(Corrected("Copyright (c)"), "Copyright \xC2\xA9");
  CHECK_EQ(Corrected("Brand(tm)"), "Brand\xE2\x84\xA2");
  CHECK_EQ(Corrected("go -->"), "go \xE2\x86\x92");
  CHECK_EQ(Corrected("wait... "), "wait\xE2\x80\xA6 ");
  // The misspelling list is English; symbols work in every language.
  CHECK_EQ(Corrected("Zeg teh ", L"nl-NL"), "Zeg teh ");
  CHECK_EQ(Corrected("(c)", L"nl-NL"), "\xC2\xA9");
  // A word that isn't finished isn't touched.
  CHECK_EQ(Corrected("Say teh"), "Say teh");
}

TEST(Proof_AutoCorrectQuotesAndDashes) {
  CHECK_EQ(Corrected("\""), "\xE2\x80\x9C");             // opening “
  CHECK_EQ(Corrected("said \"hi\""), "said \"hi\xE2\x80\x9D");  // closing ” (only the quote just typed)
  CHECK_EQ(Corrected("don'"), "don\xE2\x80\x99");        // apostrophe ’
  CHECK_EQ(Corrected("('"), "(\xE2\x80\x98");            // opening ‘ after a bracket
  CHECK_EQ(Corrected("\"", L"de-DE"), "\xE2\x80\x9E");   // German „
  CHECK_EQ(Corrected("ab--cd "), "ab\xE2\x80\x94" "cd ");  // em dash
  CHECK_EQ(Corrected("ab - cd "), "ab \xE2\x80\x93 cd ");  // en dash
  CHECK_EQ(Corrected("ab -- cd "), "ab \xE2\x80\x93 cd ");
  CHECK_EQ(Corrected("- item "), "- item ");  // a hyphen starting the paragraph stays
}

TEST(Proof_AutoCorrectCapitals) {
  CHECK_EQ(Corrected("THis "), "This ");
  CHECK_EQ(Corrected("IBM "), "IBM ");     // all capitals: an acronym
  CHECK_EQ(Corrected("hello "), "Hello ");  // the paragraph's first word
  CHECK_EQ(Corrected("Done. next "), "Done. Next ");
  CHECK_EQ(Corrected("Really? yes "), "Really? Yes ");
  CHECK_EQ(Corrected("See e.g. this "), "See e.g. this ");
  CHECK_EQ(Corrected("J. smith "), "J. smith ");          // after an initial
  CHECK_EQ(Corrected("Mail me. www.example.com "), "Mail me. www.example.com ");
  CHECK_EQ(Corrected("See you on monday "), "See you on Monday ");
  CHECK_EQ(Corrected("Done. teh "), "Done. The ");  // a replacement starting a sentence
  CHECK_EQ(Corrected("in "), "In ");
  CHECK_EQ(Corrected("Text in the middle "), "Text in the middle ");
}

TEST(Proof_AutoCorrectIsItsOwnUndoStep) {
  Fixture f;
  f.body = Para("");
  auto doc = LoadOk(f);
  CHECK(doc != nullptr);
  if (!doc) return;
  edit::Editor ed(*doc);
  AutoCorrect ac;
  // Typing "teh " a character at a time, AutoCorrect after each.
  Pos caret = P(0, 0);
  for (char16_t ch : std::u16string(u"teh ")) {
    auto r = ed.InsertText(caret, std::u16string(1, ch));
    CHECK(r.ok);
    caret = r.caret;
    if (auto moved = ac.AfterTyping(ed, caret, ch)) caret = *moved;
  }
  CHECK_EQ(ParaText(*doc, 0), "The ");  // the paragraph's first word, corrected and capitalized
  CHECK(caret == P(0, 4));
  CHECK(ed.UndoName() == u"AutoCorrect");
  // One Ctrl+Z puts back what was typed; the next one undoes the typing.
  CHECK(ed.Undo().ok);
  CHECK_EQ(ParaText(*doc, 0), "teh ");
  CHECK(ed.Undo().ok);
  CHECK_EQ(ParaText(*doc, 0), "");
  CHECK(ed.Redo().ok);
  CHECK(ed.Redo().ok);
  CHECK_EQ(ParaText(*doc, 0), "The ");
}

TEST(Proof_AutoCorrectSmartQuoteUndoesToStraight) {
  Fixture f;
  f.body = Para(Run("He said "));
  auto doc = LoadOk(f);
  CHECK(doc != nullptr);
  if (!doc) return;
  edit::Editor ed(*doc);
  AutoCorrect ac;
  auto r = ed.InsertText(P(0, 8), u"\"");
  CHECK(r.ok);
  auto moved = ac.AfterTyping(ed, r.caret, u'"');
  CHECK(moved.has_value());
  CHECK_EQ(ParaText(*doc, 0), "He said \xE2\x80\x9C");
  CHECK(ed.Undo().ok);
  CHECK_EQ(ParaText(*doc, 0), "He said \"");
}

TEST(Proof_WordCount) {
  Fixture f;
  f.body = Para(Run("Hello world.  Two  spaces")) + Para("") +
           Para(Run("Hidden", "<w:vanish/>") + Run("Shown text")) +
           "<w:p><w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText> PAGE </w:instrText></w:r>"
           "<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r><w:r><w:t>7</w:t></w:r>"
           "<w:r><w:fldChar w:fldCharType=\"end\"/></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc != nullptr);
  if (!doc) return;
  const proof::Counts c = proof::CountDocument(*doc, false);
  CHECK_EQ(c.words, 7u);       // Hello world. Two spaces | Shown text | 7
  CHECK_EQ(c.paragraphs, 3u);  // the empty one doesn't count
  CHECK_EQ(c.characters, 30u);        // 20 + 9 + 1
  CHECK_EQ(c.charactersSpaces, 36u);  // 25 + 10 + 1
  const proof::Counts part = proof::CountRange(*doc, P(0, 0), P(0, 12));
  CHECK_EQ(part.words, 2u);

  // Footnotes count only when asked for (Word's "Include textboxes, footnotes and endnotes").
  Fixture g;
  g.body = Para("<w:r><w:t>Main</w:t></w:r><w:r><w:footnoteReference w:id=\"1\"/></w:r>");
  g.footnotes = "<w:footnote w:type=\"separator\" w:id=\"-1\"><w:p><w:r><w:separator/></w:r></w:p></w:footnote>"
                "<w:footnote w:id=\"1\"><w:p><w:r><w:footnoteRef/></w:r><w:r><w:t xml:space=\"preserve\"> A note here</w:t></w:r></w:p></w:footnote>";
  auto noted = LoadOk(g);
  CHECK(noted != nullptr);
  if (!noted) return;
  CHECK_EQ(proof::CountDocument(*noted, false).words, 1u);
  CHECK_EQ(proof::CountDocument(*noted, true).words, 4u);
}

TEST(Proof_LanguageOfTextAndSettingIt) {
  Fixture f;
  f.settings = "<w:themeFontLang w:val=\"en-GB\"/>";
  f.body = Para(Run("Hallo wereld", "<w:lang w:val=\"nl-NL\"/>") + Run(" and English"));
  auto doc = LoadOk(f);
  CHECK(doc != nullptr);
  if (!doc) return;
  CHECK(proof::LanguageAt(*doc, P(0, 0)) == L"nl-NL");
  CHECK(proof::LanguageAt(*doc, P(0, 15)) == L"en-GB");  // no w:lang: the theme language
  CHECK(proof::LanguageOfRange(*doc, P(0, 0), P(0, 5)) == std::optional<std::wstring>(L"nl-NL"));
  CHECK(!proof::LanguageOfRange(*doc, P(0, 0), P(0, 20)).has_value());  // mixed
  edit::Editor ed(*doc);
  CHECK(proof::SetLanguage(ed, P(0, 12), P(0, 24), L"nl-NL").ok);
  CHECK(proof::LanguageOfRange(*doc, P(0, 0), P(0, 24)) == std::optional<std::wstring>(L"nl-NL"));
  CHECK(ed.UndoName() == u"Language");
  CHECK(ed.Undo().ok);
  CHECK(proof::LanguageAt(*doc, P(0, 15)) == L"en-GB");
  CHECK(!proof::LanguageName(L"nl-NL").empty());
}

TEST(Proof_DocumentsCanHideSpellingErrors) {
  Fixture f;
  f.body = Para(Run("text"));
  auto doc = LoadOk(f);
  CHECK(doc != nullptr);
  if (doc) CHECK(!proof::Spelling::HiddenByDocument(*doc));
  f.settings = "<w:hideSpellingErrors/>";
  auto hidden = LoadOk(f);
  CHECK(hidden != nullptr);
  if (hidden) CHECK(proof::Spelling::HiddenByDocument(*hidden));
}

TEST(Proof_SpellingFindsErrorsInTheTextShown) {
  InitCom();
  fw::text::SpellChecker checker;
  if (!checker.Available() || checker.Resolve(L"en-US").empty()) {
    std::printf("  (skipped: no English spell checker on this PC)\n");
    return;
  }
  Fixture f;
  f.settings = "<w:themeFontLang w:val=\"en-US\"/>";
  f.body = Para(Run("This sentense has a mistakke.")) +
           Para(Run("Codename ") + Run("xyzzyq", "<w:noProof/>") + Run(" stays") + Run(" wrongg", "<w:vanish/>")) +
           "<w:p><w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText> HYPERLINK \"qqqzzz\" </w:instrText></w:r>"
           "<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r><w:r><w:t>Fine link</w:t></w:r>"
           "<w:r><w:fldChar w:fldCharType=\"end\"/></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc != nullptr);
  if (!doc) return;
  proof::Spelling spelling(*doc, checker);
  while (spelling.Continue(50)) {
  }
  CHECK(spelling.Done());
  const auto* first = spelling.Errors(0);
  CHECK(first != nullptr);
  if (!first) return;
  CHECK_EQ(first->size(), 2u);
  if (first->size() == 2) {
    CHECK(spelling.Word(0, (*first)[0]) == L"sentense");
    CHECK(spelling.Word(0, (*first)[1]) == L"mistakke");
    const auto suggestions = checker.Suggest(spelling.Language((*first)[0]), L"sentense");
    CHECK(!suggestions.empty());
    CHECK(spelling.At(P(0, 8)) != nullptr);
    CHECK(spelling.At(P(0, 2)) == nullptr);
  }
  CHECK(spelling.Errors(1) && spelling.Errors(1)->empty());  // noProof and hidden text aren't checked
  CHECK(spelling.Errors(2) && spelling.Errors(2)->empty());  // nor field codes

  // Only the paragraphs asked for (those on screen); F7's search checks the ones on its way.
  proof::Spelling shown(*doc, checker);
  CHECK(!shown.Check({1, 2}, 1000));
  CHECK(shown.Checked(1) && shown.Checked(2) && !shown.Checked(0));
  bool complete = false;
  const auto next = shown.FindNext(P(1, 0), 1000, complete);  // wraps around to the first paragraph
  CHECK(complete && next.has_value());
  if (next) CHECK(next->first == 0u && shown.Word(0, next->second) == L"sentense");
  CHECK(shown.Checked(0));

  // An edit marks its paragraph for checking again.
  edit::Editor ed(*doc);
  ed.OnChange([&](const edit::Change& c) { spelling.Changed(c); });
  CHECK(ed.Replace(P(0, 5), P(0, 13), u"sentence").ok);
  CHECK(!spelling.Done());
  while (spelling.Continue(50)) {
  }
  CHECK(spelling.Errors(0) && spelling.Errors(0)->size() == 1u);

  // Ignore all: the word stops being an error.
  checker.Ignore(L"en-US", L"mistakke");
  spelling.Recheck(L"mistakke");
  while (spelling.Continue(50)) {
  }
  CHECK(spelling.Errors(0) && spelling.Errors(0)->empty());
}
