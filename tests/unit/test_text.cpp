#include "TestHarness.h"

#include <dwrite_3.h>

#include <string>
#include <vector>

#include "text/Fonts.h"
#include "text/Hyphenation.h"
#include "text/LineLayout.h"

using namespace fw::text;

namespace {

// Fonts every Windows 10 and 11 installation has.
constexpr const wchar_t* kMono = L"Courier New";
constexpr const wchar_t* kSans = L"Arial";

Fonts& TestFonts() {
  static Fonts fonts;
  return fonts;
}

TextStyle Style(const wchar_t* family = kMono, float size = 10) {
  TextStyle s;
  s.family = family;
  s.size = size;
  s.language = L"en-US";
  return s;
}

// The advance of one character, so tests can say "n characters wide" in a monospaced font.
float Advance(const TextStyle& s, wchar_t c = L'a') {
  Fonts& f = TestFonts();
  return f.Get(f.Resolve(s.family), DWRITE_FONT_WEIGHT_NORMAL, false)->Advance(c, s.size);
}

float Chars(const TextStyle& s, float n) { return Advance(s) * n + 0.01f; }

Paragraph Para(const std::wstring& text, const TextStyle& style, const ParagraphFormat& format = {}) {
  Paragraph p;
  p.text = text;
  p.runs.push_back({static_cast<uint32_t>(text.size()), &style});
  p.markStyle = &style;
  p.format = format;
  return p;
}

std::vector<Line> Lay(const std::wstring& text, const TextStyle& style, float width, const ParagraphFormat& format = {}) {
  return Layout(TestFonts(), Para(text, style, format), width);
}

std::wstring TextOf(const std::wstring& text, const Line& line) { return text.substr(line.start, line.end - line.start); }

std::vector<std::wstring> Texts(const std::wstring& text, const std::vector<Line>& lines) {
  std::vector<std::wstring> out;
  for (const auto& l : lines) out.push_back(TextOf(text, l));
  return out;
}

const GlyphPiece* Hyphen(const Line& line) {
  for (const auto& p : line.pieces)
    if (p.hyphen) return &p;
  return nullptr;
}

std::wstring FamilyOf(const GlyphPiece& piece) {
  Microsoft::WRL::ComPtr<IDWriteFontFace3> face3;
  if (FAILED(piece.face.As(&face3))) return {};
  Microsoft::WRL::ComPtr<IDWriteLocalizedStrings> names;
  if (FAILED(face3->GetFamilyNames(&names))) return {};
  wchar_t name[128] = {};
  names->GetString(0, name, 128);
  return name;
}

}  // namespace

// ---- Hyphenation -----------------------------------------------------------------------------

TEST(Hyphenation_English) {
  const Hyphenator* h = Hyphenator::For(L"en-US");
  CHECK(h != nullptr);
  if (!h) return;
  CHECK(h->PatternCount() > 4000);
  CHECK(h->Points(L"hyphenation") == (std::vector<uint32_t>{2, 6}));  // hy-phen-ation
  CHECK(h->Points(L"Algorithm") == (std::vector<uint32_t>{2, 4}));    // al-go-rithm, any case
  CHECK(h->Points(L"table") == (std::vector<uint32_t>{2}));           // an exception: ta-ble
  CHECK(h->Points(L"present").empty());                               // an exception without hyphens
  CHECK(h->Points(L"cat").empty());                                   // shorter than the minimums
  CHECK(Hyphenator::For(L"en") == h);
  CHECK(Hyphenator::For(L"en-CA") == h);
  const Hyphenator* gb = Hyphenator::For(L"en-GB");
  CHECK(gb != nullptr && gb != h);
  CHECK(Hyphenator::For(L"en-AU") == gb);
}

TEST(Hyphenation_OtherLanguages) {
  const Hyphenator* nl = Hyphenator::For(L"nl-NL");
  CHECK(nl != nullptr && nl->PatternCount() > 10000);
  if (nl) CHECK(nl->Points(L"afbreekpatronen") == (std::vector<uint32_t>{2, 7, 9, 12}));  // af-breek-pa-tro-nen
  CHECK(Hyphenator::For(L"nl-BE") == nl);
  const Hyphenator* de = Hyphenator::For(L"de-DE");
  CHECK(de != nullptr);
  if (de) CHECK(de->Points(L"Silbentrennung") == (std::vector<uint32_t>{3, 6, 10}));  // Sil-ben-tren-nung
  const Hyphenator* fr = Hyphenator::For(L"fr-FR");
  CHECK(fr != nullptr);
  if (fr) CHECK(fr->Points(L"hyphénation") == (std::vector<uint32_t>{2, 5, 7}));  // hy-phé-na-tion
  // Every shipped set loads and hyphenates a long word of its language.
  const std::pair<const wchar_t*, const wchar_t*> words[] = {
      {L"es", L"silabación"}, {L"it", L"sillabazione"}, {L"pt-BR", L"hifenização"}, {L"da", L"orddeling"},
      {L"sv", L"avstavning"}, {L"nb", L"orddeling"},    {L"nn-NO", L"orddeling"},   {L"fi", L"tavutus"},
      {L"en-GB", L"hyphenation"}};
  for (const auto& [tag, word] : words) {
    const Hyphenator* h = Hyphenator::For(tag);
    CHECK(h != nullptr);
    if (h) CHECK(!h->Points(word).empty());
  }
  CHECK(Hyphenator::For(L"pl-PL") == nullptr);
  CHECK(Hyphenator::Languages().size() == 12);
}

TEST(Hyphenation_PatternText) {
  // Liang's example: hy3ph and he2n give hy-phen... only with patterns that say so.
  Hyphenator h(L"test", L"% a comment\nUTF-8\nLEFTHYPHENMIN 1\n1ba\na1b\nta-ble\n", 1, 1);
  CHECK(h.PatternCount() == 2);
  CHECK(h.Points(L"abab") == (std::vector<uint32_t>{1, 3}));  // a-ba-b: 1ba before each b... a1b after each a
  CHECK(h.Points(L"table") == (std::vector<uint32_t>{2}));
  CHECK(h.Points(L"abab", 2, 2).empty());
}

// ---- Line breaking ---------------------------------------------------------------------------

TEST(Text_BreaksAtSpacesFirstFit) {
  TextStyle s = Style();
  std::wstring text = L"aaaa bbbb cccc dd";
  auto lines = Lay(text, s, Chars(s, 9));
  CHECK(Texts(text, lines) == (std::vector<std::wstring>{L"aaaa bbbb ", L"cccc dd"}));
  CHECK(lines[0].ending == LineEnd::Wrap);
  CHECK(lines[1].ending == LineEnd::ParagraphEnd);
  CHECK_NEAR(lines[0].contentRight, Advance(s) * 9, 0.01);  // the trailing space doesn't count
  CHECK(lines[1].top > 0);
}

TEST(Text_TrailingSpacesHang) {
  TextStyle s = Style();
  std::wstring text = L"aaaa        bbbb";
  auto lines = Lay(text, s, Chars(s, 4));
  CHECK(Texts(text, lines) == (std::vector<std::wstring>{L"aaaa        ", L"bbbb"}));
  CHECK_NEAR(lines[0].contentRight, Advance(s) * 4, 0.01);
}

TEST(Text_LongWordBreaksBetweenCharacters) {
  TextStyle s = Style();
  std::wstring text = L"abcdefghij kl";
  auto lines = Lay(text, s, Chars(s, 4));
  CHECK(Texts(text, lines) == (std::vector<std::wstring>{L"abcd", L"efgh", L"ij ", L"kl"}));
  // A line too narrow for one character still takes one.
  auto narrow = Lay(L"ab", s, 1);
  CHECK(narrow.size() == 2);
}

TEST(Text_MandatoryBreaks) {
  TextStyle s = Style();
  std::wstring text = L"ab\vcd\x000C" L"ef\x000E" L"gh";
  auto lines = Lay(text, s, 400);
  CHECK(lines.size() == 4);
  if (lines.size() != 4) return;
  CHECK(lines[0].ending == LineEnd::LineBreak);
  CHECK(lines[1].ending == LineEnd::PageBreak);
  CHECK(lines[2].ending == LineEnd::ColumnBreak);
  CHECK(lines[3].ending == LineEnd::ParagraphEnd);
  CHECK(TextOf(text, lines[1]) == L"cd\x000C");
  // A line break at the end leaves an empty last line, as in Word.
  std::wstring trailing = L"ab\v";
  auto two = Lay(trailing, s, 400);
  CHECK(two.size() == 2);
  if (two.size() == 2) {
    CHECK(two[1].start == 3 && two[1].end == 3);
    CHECK(two[1].ascent > 0);
  }
}

TEST(Text_EmptyParagraphTakesTheMarksHeight) {
  TextStyle s = Style(kSans, 12);
  TextStyle big = Style(kSans, 24);
  Paragraph p = Para(L"", s);
  p.markStyle = &big;
  auto lines = Layout(TestFonts(), p, 400);
  CHECK(lines.size() == 1);
  if (lines.empty()) return;
  CHECK(lines[0].start == 0 && lines[0].end == 0);
  CHECK_NEAR(lines[0].ascent + lines[0].descent + lines[0].lineGap, 27.6, 0.05);  // Arial 24 pt
}

TEST(Text_OptionalAndNonBreakingHyphens) {
  TextStyle s = Style();
  std::wstring soft = L"aaaa bbb\x00AD" L"ccc";
  auto lines = Lay(soft, s, Chars(s, 9));
  CHECK(Texts(soft, lines) == (std::vector<std::wstring>{L"aaaa bbb\x00AD", L"ccc"}));
  CHECK(lines[0].hyphenated);
  CHECK(Hyphen(lines[0]) != nullptr);
  CHECK_NEAR(lines[0].contentRight, Advance(s) * 9, 0.01);  // "aaaa bbb-"
  // When it fits, an optional hyphen is invisible.
  auto wide = Lay(soft, s, 400);
  CHECK(wide.size() == 1 && !wide[0].hyphenated);
  CHECK_NEAR(wide[0].contentRight, Advance(s) * 11, 0.01);

  std::wstring hard = L"aaaa bb\x2011" L"cc";
  auto nb = Lay(hard, s, Chars(s, 8));
  CHECK(Texts(hard, nb) == (std::vector<std::wstring>{L"aaaa ", L"bb\x2011" L"cc"}));
  std::wstring space = L"aa bb\x00A0" L"cc";
  auto ns = Lay(space, s, Chars(s, 6));
  CHECK(Texts(space, ns) == (std::vector<std::wstring>{L"aa ", L"bb\x00A0" L"cc"}));
  std::wstring hyphen = L"aaaa bb-cc";
  auto h = Lay(hyphen, s, Chars(s, 8));
  CHECK(Texts(hyphen, h) == (std::vector<std::wstring>{L"aaaa bb-", L"cc"}));
}

TEST(Text_NoBreakAfterSlash) {
  // Word keeps "and/or" and paths together; a slash isn't a break opportunity.
  TextStyle s = Style();
  std::wstring text = L"aaaa/bbbb";
  auto lines = Lay(text, s, Chars(s, 6));
  CHECK(Texts(text, lines) == (std::vector<std::wstring>{L"aaaa/b", L"bbb"}));
  std::wstring zw = L"aaaa/\x200B" L"bbbb";
  auto opt = Lay(zw, s, Chars(s, 6));
  CHECK(Texts(zw, opt) == (std::vector<std::wstring>{L"aaaa/\x200B", L"bbbb"}));
}

TEST(Text_AutomaticHyphenation) {
  TextStyle s = Style();
  ParagraphFormat f;
  f.hyphenation.automatic = true;
  f.hyphenation.zone = 0;
  std::wstring text = L"the hyphenation algorithm";
  auto lines = Lay(text, s, Chars(s, 12), f);
  CHECK(Texts(text, lines) == (std::vector<std::wstring>{L"the hyphen", L"ation algo", L"rithm"}));
  CHECK(lines[0].hyphenated && lines[1].hyphenated && !lines[2].hyphenated);
  CHECK(Hyphen(lines[0]) != nullptr);
  // Off by default.
  auto off = Lay(text, s, Chars(s, 12));
  CHECK(Texts(text, off) == (std::vector<std::wstring>{L"the ", L"hyphenation ", L"algorithm"}));
  // No hyphenation where the line would still end within the zone.
  f.hyphenation.zone = Advance(s) * 10;
  auto zoned = Lay(text, s, Chars(s, 12), f);
  CHECK(TextOf(text, zoned[0]) == L"the ");
  // At most one hyphenated line in a row.
  f.hyphenation.zone = 0;
  f.hyphenation.consecutiveLimit = 1;
  auto limited = Lay(text, s, Chars(s, 12), f);
  CHECK(Texts(text, limited) == (std::vector<std::wstring>{L"the hyphen", L"ation ", L"algorithm"}));
  // Words in capitals, when the document says not to hyphenate them.
  f.hyphenation.consecutiveLimit = 0;
  f.hyphenation.caps = false;
  std::wstring caps = L"THE HYPHENATION";
  auto noCaps = Lay(caps, s, Chars(s, 12), f);
  CHECK(TextOf(caps, noCaps[0]) == L"THE ");
  // Languages without patterns aren't hyphenated.
  TextStyle polish = s;
  polish.language = L"pl-PL";
  auto pl = Lay(text, polish, Chars(s, 12), f);
  CHECK(TextOf(text, pl[0]) == L"the ");
}

TEST(Text_DutchHyphenation) {
  TextStyle s = Style();
  s.language = L"nl-NL";
  ParagraphFormat f;
  f.hyphenation.automatic = true;
  f.hyphenation.zone = 0;
  std::wstring text = L"de afbreekpatronen";
  auto lines = Lay(text, s, Chars(s, 12), f);
  CHECK(Texts(text, lines) == (std::vector<std::wstring>{L"de afbreek", L"patronen"}));
}

// ---- Tabs --------------------------------------------------------------------------------------

TEST(Text_DefaultTabs) {
  TextStyle s = Style();
  std::wstring text = L"a\tb\tc";
  auto lines = Lay(text, s, 400);
  CHECK(lines.size() == 1);
  CHECK_NEAR(lines[0].CaretX(2), 36, 0.01);
  CHECK_NEAR(lines[0].CaretX(4), 72, 0.01);
  // Default stops come after the last custom stop.
  ParagraphFormat f;
  f.tabs = {{50}};
  auto custom = Lay(text, s, 400, f);
  CHECK_NEAR(custom[0].CaretX(2), 50, 0.01);
  CHECK_NEAR(custom[0].CaretX(4), 72, 0.01);
}

TEST(Text_AlignedTabsAndLeaders) {
  TextStyle s = Style();
  float a = Advance(s);
  ParagraphFormat f;
  f.tabs = {{100, TabAlign::End, TabLeader::Dot}, {200, TabAlign::Center}, {300, TabAlign::Decimal}};
  std::wstring text = L"x\t123\tabcd\t12.50";
  auto lines = Lay(text, s, 400, f);
  CHECK(lines.size() == 1);
  if (lines.empty()) return;
  const Line& l = lines[0];
  CHECK_NEAR(l.CaretX(2), 100 - 3 * a, 0.01);  // "123" ends at 100
  CHECK_NEAR(l.CaretX(6), 200 - 2 * a, 0.01);  // "abcd" centered on 200
  CHECK_NEAR(l.CaretX(13), 300, 0.01);         // the decimal point at 300
  bool leader = false;
  for (const auto& p : l.pieces) {
    if (!p.leader) continue;
    leader = true;
    CHECK(p.x >= a - 0.01 && p.x + p.width <= 100 - 3 * a + 0.01);
  }
  CHECK(leader);
}

TEST(Text_HangingIndentIsATabStop) {
  TextStyle s = Style();
  ParagraphFormat f;
  f.startIndent = 36;
  f.firstLineIndent = -18;
  std::wstring text = L"1.\tFirst item that wraps around";
  auto lines = Lay(text, s, Chars(s, 20) + 36, f);
  CHECK(lines.size() >= 2);
  if (lines.size() < 2) return;
  CHECK_NEAR(lines[0].CaretX(0), 18, 0.01);
  CHECK_NEAR(lines[0].CaretX(3), 36, 0.01);  // after the tab: the indent
  CHECK_NEAR(lines[1].CaretX(lines[1].start), 36, 0.01);
}

TEST(Text_TabPastTheLineWraps) {
  TextStyle s = Style();
  std::wstring text = L"aaa\tbbb";
  auto lines = Lay(text, s, 50);  // the tab stops at 36, and "bbb" doesn't fit after it
  CHECK(Texts(text, lines) == (std::vector<std::wstring>{L"aaa\t", L"bbb"}));
  CHECK_NEAR(lines[0].contentRight, 36, 0.01);
  auto past = Lay(text, s, 30);  // no stop before the end of the line: the tab fills it
  CHECK(Texts(text, past) == (std::vector<std::wstring>{L"aaa\t", L"bbb"}));
  CHECK_NEAR(past[0].contentRight, 30, 0.01);
}

// ---- Alignment ---------------------------------------------------------------------------------

TEST(Text_JustifyStretchesOnlySpaces) {
  TextStyle s = Style();
  float a = Advance(s);
  ParagraphFormat f;
  f.alignment = Alignment::Justify;
  std::wstring text = L"aa bb cc dddddd ee";
  float width = Chars(s, 12);
  auto lines = Lay(text, s, width, f);
  CHECK(Texts(text, lines) == (std::vector<std::wstring>{L"aa bb cc ", L"dddddd ee"}));
  CHECK_NEAR(lines[0].contentRight, width, 0.01);
  float extra = (width - 8 * a) / 2;  // two spaces share what's left
  CHECK_NEAR(lines[0].CaretX(3), 2 * a + a + extra, 0.01);
  CHECK_NEAR(lines[0].CaretX(1), a, 0.01);  // letters keep their advance
  // The last line isn't justified.
  CHECK_NEAR(lines[1].contentRight, 9 * a, 0.01);
}

TEST(Text_JustifyLineBreaks) {
  TextStyle s = Style();
  ParagraphFormat f;
  f.alignment = Alignment::Justify;
  std::wstring text = L"aa bb\vcc";
  auto expanded = Lay(text, s, 100, f);
  CHECK_NEAR(expanded[0].contentRight, 100, 0.01);  // Word expands a line ending in Shift+Enter
  f.expandLineBreaks = false;
  auto kept = Lay(text, s, 100, f);
  CHECK_NEAR(kept[0].contentRight, 5 * Advance(s), 0.01);
}

TEST(Text_DistributedSpreadsEveryLine) {
  TextStyle s = Style();
  ParagraphFormat f;
  f.alignment = Alignment::Distribute;
  std::wstring text = L"abcd";
  auto lines = Lay(text, s, 100, f);
  CHECK_NEAR(lines[0].contentLeft, 0, 0.01);
  CHECK_NEAR(lines[0].contentRight, 100, 0.01);
}

TEST(Text_CenterAndEnd) {
  TextStyle s = Style();
  float a = Advance(s);
  ParagraphFormat f;
  f.alignment = Alignment::Center;
  auto centered = Lay(L"abcd", s, 100, f);
  CHECK_NEAR(centered[0].contentLeft, (100 - 4 * a) / 2, 0.01);
  f.alignment = Alignment::End;
  auto right = Lay(L"abcd", s, 100, f);
  CHECK_NEAR(right[0].contentRight, 100, 0.01);
  // Spaces before the paragraph mark count when right-aligning, as in Word.
  auto spaces = Lay(L"abcd  ", s, 100, f);
  CHECK_NEAR(spaces[0].contentLeft, 100 - 6 * a, 0.01);
  // Indents.
  f.alignment = Alignment::Start;
  f.startIndent = 10;
  f.endIndent = 20;
  f.firstLineIndent = 5;
  auto indented = Lay(L"aaaa bbbb", s, 15 + Chars(s, 5) + 20, f);
  CHECK(indented.size() == 2);
  CHECK_NEAR(indented[0].left, 15, 0.01);
  CHECK_NEAR(indented[1].left, 10, 0.01);
  CHECK_NEAR(indented[0].right, 15 + Chars(s, 5), 0.01);
}

// ---- Bidi, fonts, styles ------------------------------------------------------------------------

TEST(Text_BidiBasics) {
  TextStyle s = Style(kSans, 12);
  // "abc", Hebrew "shalom", "def" in a left-to-right paragraph.
  std::wstring text = L"abc \x05E9\x05DC\x05D5\x05DD def";
  auto lines = Lay(text, s, 400);
  CHECK(lines.size() == 1);
  if (lines.empty()) return;
  const Line& l = lines[0];
  CHECK(l.pieces.size() >= 3);
  bool rtlPiece = false;
  for (const auto& p : l.pieces)
    if (p.bidiLevel == 1) rtlPiece = true;
  CHECK(rtlPiece);
  // Visual order: the Hebrew reads right to left between the two Latin words.
  CHECK(l.CaretX(4) > l.CaretX(7));
  CHECK(l.CaretX(4) > l.CaretX(3) && l.CaretX(9) > l.CaretX(4));
  CHECK(l.Ranges(4, 8).size() == 1);
  CHECK(l.Ranges(2, 6).size() == 2);  // across a direction change: two pieces of highlight

  // A right-to-left paragraph starts at the right.
  ParagraphFormat f;
  f.rightToLeft = true;
  std::wstring hebrew = L"\x05E9\x05DC\x05D5\x05DD \x05E2\x05D5\x05DC\x05DD";
  auto r = Lay(hebrew, s, 400, f);
  CHECK(r.size() == 1);
  if (r.empty()) return;
  CHECK_NEAR(r[0].contentRight, 400, 0.01);
  CHECK(r[0].CaretX(0) > r[0].CaretX(5));
}

TEST(Text_FontSlotsAndFallback) {
  // Complex-script characters use the cs family and size.
  TextStyle s = Style(kMono, 10);
  s.complexFamily = kSans;
  s.complexSize = 14;
  std::wstring text = L"ab \x05E9\x05DC\x05D5\x05DD";
  auto lines = Lay(text, s, 400);
  bool hebrew = false;
  for (const auto& p : lines[0].pieces) {
    if (p.bidiLevel != 1) continue;
    hebrew = true;
    CHECK(FamilyOf(p) == kSans);
    CHECK_NEAR(p.emSize, 14, 0.001);
  }
  CHECK(hebrew);
  // A character the font doesn't have comes from a fallback font.
  TextStyle sans = Style(kSans, 12);
  std::wstring chess = L"ab\x2654";
  auto fb = Lay(chess, sans, 400);
  CHECK(fb[0].pieces.size() == 2);
  if (fb[0].pieces.size() == 2) {
    CHECK(FamilyOf(fb[0].pieces[0]) == kSans);
    CHECK(FamilyOf(fb[0].pieces[1]) != kSans);
  }
}

TEST(Text_MissingFontsAreSubstitutedAndReported) {
  Fonts fonts([](std::wstring_view family) { return family == L"House Font" ? std::wstring(kSans) : std::wstring(); });
  CHECK(fonts.Resolve(L"Courier New") == L"Courier New");
  CHECK(fonts.Resolve(L"House Font") == kSans);
  CHECK(fonts.Resolve(L"No Such Font Anywhere") == Fonts::kLastResort);
  if (!fonts.Installed(L"Helvetica")) CHECK(fonts.Resolve(L"helvetica") == kSans);
  CHECK(fonts.Substitutions().size() >= 2);
  if (fonts.Substitutions().size() >= 2) {
    CHECK(fonts.Substitutions()[0] == (Substitution{L"House Font", kSans}));
    CHECK(fonts.Substitutions()[1].requested == L"No Such Font Anywhere");
  }
  CHECK(Fonts::BuiltInSubstitute(L"times") == L"Times New Roman");
  CHECK(Fonts::BuiltInSubstitute(L"Arial").empty());
  // Layout reports them too.
  TextStyle s = Style(L"No Such Font Anywhere", 12);
  Layout(fonts, Para(L"abc", s), 100);
  CHECK(fonts.Substitutions().size() >= 2);
}

TEST(Text_LineMetricsAreGdis) {
  // Arial and Times New Roman 12 pt: single spacing is 13.8 pt in Word.
  for (const wchar_t* family : {kSans, L"Times New Roman"}) {
    TextStyle s = Style(family, 12);
    auto lines = Lay(L"Hello", s, 400);
    CHECK_NEAR(lines[0].ascent + lines[0].descent + lines[0].lineGap, 13.8, 0.01);
  }
}

TEST(Text_HiddenTextIsLeftOut) {
  TextStyle s = Style();
  TextStyle hidden = s;
  hidden.hidden = true;
  std::wstring text = L"aaXXXXXXXXbb cc";
  Paragraph p = Para(text, s);
  p.runs = {{2, &s}, {8, &hidden}, {5, &s}};
  auto lines = Layout(TestFonts(), p, Chars(s, 4));
  CHECK(lines.size() == 2);
  if (lines.size() != 2) return;
  CHECK(lines[0].start == 0 && lines[0].end == 13);  // "aa", the hidden text, "bb "
  CHECK(lines[1].start == 13 && lines[1].end == 15);
  CHECK_NEAR(lines[0].CaretX(5), lines[0].CaretX(10), 0.01);  // hidden: the next visible caret
  CHECK(!lines[0].IsCaretStop(5));
}

TEST(Text_CapsSuperscriptSpacing) {
  TextStyle s = Style(kSans, 12);
  TextStyle caps = s;
  caps.caps = Caps::All;
  auto upper = Lay(L"ABC", s, 400);
  auto mapped = Lay(L"abc", caps, 400);
  CHECK(upper[0].pieces[0].glyphs == mapped[0].pieces[0].glyphs);
  TextStyle smallCaps = s;
  smallCaps.caps = Caps::Small;
  auto sc = Lay(L"Ab", smallCaps, 400);
  CHECK(sc[0].pieces.size() == 2);
  if (sc[0].pieces.size() == 2) CHECK_NEAR(sc[0].pieces[1].emSize, 12 * 0.8, 0.001);
  TextStyle sup = s;
  sup.verticalAlign = VerticalAlign::Superscript;
  auto up = Lay(L"2", sup, 400);
  CHECK(up[0].pieces[0].rise > 0 && up[0].pieces[0].emSize < 12);
  TextStyle spaced = s;
  spaced.letterSpacing = 2;
  auto plain = Lay(L"abcd", s, 400), wide = Lay(L"abcd", spaced, 400);
  CHECK_NEAR(wide[0].contentRight - plain[0].contentRight, 8, 0.01);
}

TEST(Text_InlineObjects) {
  TextStyle s = Style();
  TextStyle picture = s;
  picture.objectWidth = 50;
  picture.objectAscent = 40;
  std::wstring text = L"ab \xFFFC cd";
  Paragraph p = Para(text, s);
  p.runs = {{3, &s}, {1, &picture}, {3, &s}};
  auto lines = Layout(TestFonts(), p, 400);
  CHECK(lines[0].objects.size() == 1);
  if (lines[0].objects.empty()) return;
  CHECK_NEAR(lines[0].objects[0].x, 3 * Advance(s), 0.01);
  CHECK(lines[0].ascent >= 40);
}

TEST(Text_LinesBesideAFloat) {
  // A float on the left for the first 20 points of the paragraph.
  TextStyle s = Style();
  std::wstring text = L"aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii jjjj kkkk llll mmmm nnnn";
  LineSpaceFn space = [](float top, float) { return top < 20 ? LineSpace{top, 60, 200} : LineSpace{top, 0, 200}; };
  auto lines = Layout(TestFonts(), Para(text, s), 200, space);
  CHECK(lines.size() >= 2);
  if (lines.size() < 2) return;
  CHECK_NEAR(lines[0].left, 60, 0.01);
  CHECK_NEAR(lines[0].contentLeft, 60, 0.01);
  CHECK(lines[1].top > lines[0].top);
  size_t last = lines.size() - 1;
  CHECK_NEAR(lines[last].left, 0, 0.01);
}

TEST(Text_BreakerResumes) {
  TextStyle s = Style();
  std::wstring text = L"aaaa bbbb cccc dddd";
  Paragraph p = Para(text, s);
  const float narrow = Chars(s, 5);
  LineBreaker breaker(TestFonts(), p, Chars(s, 10));
  Line first = breaker.Next(0, narrow);
  LineBreaker::State st = breaker.Save();
  Line second = breaker.Next(0, narrow);
  breaker.Restore(st);
  Line again = breaker.Next();  // the same line, in the whole column
  CHECK(TextOf(text, first) == L"aaaa ");
  CHECK(TextOf(text, second) == L"bbbb ");
  CHECK(TextOf(text, again) == L"bbbb cccc ");
  while (!breaker.Done()) breaker.Next();
  CHECK(breaker.Save().done);
}

TEST(Text_CaretsAndHitTesting) {
  TextStyle s = Style(kSans, 12);
  std::wstring text = L"Hello world";
  auto lines = Lay(text, s, 400);
  const Line& l = lines[0];
  for (uint32_t k = 0; k < text.size(); ++k) CHECK(l.CaretX(k + 1) > l.CaretX(k));
  for (uint32_t k = 0; k <= text.size(); ++k) CHECK(l.HitTest(l.CaretX(k) + 0.1f) == k);
  CHECK(l.HitTest(-50) == 0);
  CHECK(l.HitTest(1000) == text.size());
  auto r = l.Ranges(0, 5);
  CHECK(r.size() == 1);
  if (!r.empty()) {
    CHECK_NEAR(r[0].first, 0, 0.01);
    CHECK_NEAR(r[0].second, l.CaretX(5), 0.01);
  }
  // A base letter and its combining accent are one caret stop.
  std::wstring decomposed = L"e\x0301x";
  auto d = Lay(decomposed, s, 400);
  CHECK(d[0].IsCaretStop(0) && !d[0].IsCaretStop(1) && d[0].IsCaretStop(2));
}
