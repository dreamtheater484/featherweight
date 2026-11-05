#include "TestHarness.h"

#include "Fidelity.h"
#include "core/LayoutDump.h"

using namespace fw;
using namespace fwtool::fidelity;

namespace {

LayoutLine TextLine(std::wstring text, LayoutStory story = LayoutStory::Body) {
  LayoutLine l;
  l.story = story;
  l.text = std::move(text);
  return l;
}

LayoutDump Dump(std::vector<std::vector<LayoutLine>> pages) {
  LayoutDump d;
  for (auto& lines : pages) d.pages.push_back(LayoutPage{612, 792, std::move(lines)});
  return d;
}

}  // namespace

// ---------- The layout dump ----------

TEST(LayoutDump_WritesAndReadsBack) {
  LayoutDump d;
  d.source = "layout/report.docx";
  d.producer = "a \"quoted\" producer";
  LayoutLine header = TextLine(L"Héader “quoted” \\ back\tslash \U0001F600", LayoutStory::Header);
  header.x = 72;
  header.baseline = 46.375;
  header.width = 120.5;
  header.size = 11;
  std::vector<LayoutLine> lines = {header};
  for (auto story : {LayoutStory::Body, LayoutStory::Footer, LayoutStory::Footnote, LayoutStory::Endnote,
                     LayoutStory::Cell, LayoutStory::TextBox})
  {
    std::string_view name = StoryName(story);
    lines.push_back(TextLine(std::wstring(name.begin(), name.end()), story));
  }
  d.pages.push_back(LayoutPage{612, 792, lines});
  d.pages.push_back(LayoutPage{595.3, 841.89, {}});

  std::string json = WriteLayoutDump(d);
  std::string error;
  auto back = ReadLayoutDump(json, &error);
  CHECK(back.has_value());
  if (!back) return;
  CHECK(error.empty());
  CHECK_EQ(back->source, d.source);
  CHECK_EQ(back->producer, d.producer);
  CHECK_EQ(back->pages.size(), 2u);
  CHECK_EQ(back->pages[0].lines.size(), 7u);
  const LayoutLine& h = back->pages[0].lines[0];
  CHECK(h.story == LayoutStory::Header);
  CHECK(h.text == header.text);
  CHECK_NEAR(h.x, 72, 1e-9);
  CHECK_NEAR(h.baseline, 46.38, 1e-9);  // two decimals
  CHECK_NEAR(h.width, 120.5, 1e-9);
  CHECK_NEAR(h.size, 11, 1e-9);
  for (size_t i = 0; i < lines.size(); ++i) CHECK(back->pages[0].lines[i].story == lines[i].story);
  CHECK_NEAR(back->pages[1].width, 595.3, 1e-9);
  CHECK_NEAR(back->pages[1].height, 841.89, 1e-9);
  CHECK(back->pages[1].lines.empty());
  // One line of JSON per line of text, so dumps diff well.
  CHECK(json.find("\n      {\"story\": \"header\", \"x\": 72, \"baseline\": 46.38") != std::string::npos);
  CHECK(json.find("\"version\": 1") != std::string::npos);
}

TEST(LayoutDump_RefusesWhatItCantRead) {
  std::string error;
  CHECK(!ReadLayoutDump("not json", &error));
  CHECK(!error.empty());
  CHECK(!ReadLayoutDump(R"({"format": "something-else", "version": 1, "pages": []})", &error));
  CHECK(!ReadLayoutDump(R"({"format": "featherweight-layout", "version": 2, "pages": []})", &error));
  CHECK(error.find("version 2") != std::string::npos);
  CHECK(!ReadLayoutDump(R"({"format": "featherweight-layout", "version": 1, "pages": [{"width": 1, "height": 1,
        "lines": [{"story": "margin", "text": "x"}]}]})", &error));
  CHECK(error.find("margin") != std::string::npos);
  CHECK(!ReadLayoutDump(R"({"format": "featherweight-layout", "version": 1, "pages": [{"lines": [{"story": "body"}]}]})",
                        &error));
  CHECK(ReadLayoutDump(R"({"format": "featherweight-layout", "version": 1, "pages": []})", &error).has_value());
}

// ---------- Words ----------

TEST(Fidelity_WordKeys) {
  CHECK(WordKey(L"Office,") == L"office");
  CHECK(WordKey(L"ﬁnal") == L"final");     // a ligature, spelled out
  CHECK(WordKey(L"oﬃce") == L"office");
  CHECK(WordKey(L"“Quoted”") == L"quoted");
  CHECK(WordKey(L"well-known") == L"wellknown");  // hyphens don't count
  CHECK(WordKey(L"accord­ing") == L"according");
  CHECK(WordKey(L"1,250.00") == L"125000");
  CHECK(WordKey(L"•").empty());              // bullets, leaders and dashes aren't words
  CHECK(WordKey(L"").empty());
  CHECK(WordKey(L"........").empty());
  CHECK(WordKey(L"—").empty());
  CHECK(WordKey(L"STRAßE") == L"straße");
  CHECK(WordKey(L"ÉCOLE") == L"école");  // small caps and all caps drawn as capitals
}

TEST(Fidelity_TokenizeJoinsHyphenatedWords) {
  // PDFium reports a hyphen it finds at a line end as U+0002.
  auto d = Dump({{TextLine(L"We agreed on the accord-"), TextLine(L"ing plan, well-"), TextLine(L"known to all.")},
                 {TextLine(L"Inter\x02"), TextLine(L"national.")}});
  Text t = Tokenize(d);
  CHECK_EQ(t.words.size(), 10u);
  if (t.words.size() != 10) return;
  CHECK(t.words[4].key == L"according");
  CHECK(t.words[6].key == L"wellknown");
  CHECK(t.words[9].key == L"international");  // across a page break
  CHECK_EQ(t.lines[0].end.token, 4);
  CHECK_EQ(t.lines[0].end.offset, 6);
  CHECK_EQ(t.lines[1].firstWord, 4);
  CHECK_EQ(t.lines[1].end.token, 6);
  CHECK_EQ(t.lines[1].end.offset, 4);
  CHECK_EQ(t.lines[2].end.token, 8);
  CHECK_EQ(t.lines[2].end.offset, 3);
  CHECK_EQ(t.lines[3].end.offset, 5);
  CHECK_EQ(t.lines[3].page, 1);
}

TEST(Fidelity_TokenizeSkipsLinesWithoutWords) {
  auto d = Dump({{TextLine(L"•"), TextLine(L"Introduction........1"), TextLine(L"  .........  "), TextLine(L"")}});
  Text t = Tokenize(d);
  CHECK_EQ(t.lines.size(), 4u);
  CHECK_EQ(t.lines[0].firstWord, -1);
  CHECK_EQ(t.lines[1].firstWord, 0);
  CHECK(t.words[0].key == L"introduction1");  // the leader joins them as drawn, on both sides alike
  CHECK_EQ(t.lines[2].firstWord, -1);
  CHECK_EQ(t.lines[3].firstWord, -1);
}

TEST(Fidelity_TokenizeSplitsIdeographs) {
  Text t = Tokenize(Dump({{TextLine(L"日本語のテキスト、Featherweight")}}));
  CHECK_EQ(t.words.size(), 9u);  // eight ideographs and kana, then a word
}

TEST(Fidelity_TokenizeGroupsAreas) {
  auto d = Dump({{TextLine(L"Header", LayoutStory::Header), TextLine(L"Body text"), TextLine(L"Cell", LayoutStory::Cell),
                  TextLine(L"1 A note", LayoutStory::Footnote), TextLine(L"Page 1", LayoutStory::Footer),
                  TextLine(L"Boxed", LayoutStory::TextBox), TextLine(L"2 Endnote", LayoutStory::Endnote)}});
  Text t = Tokenize(d);
  auto words = [&](Area a) { return t.wordStart[static_cast<int>(a) + 1] - t.wordStart[static_cast<int>(a)]; };
  CHECK_EQ(words(Area::Main), 3);  // body and table cells
  CHECK_EQ(words(Area::Notes), 5);  // footnotes and endnotes
  CHECK_EQ(words(Area::Header), 1);
  CHECK_EQ(words(Area::Footer), 2);
  CHECK_EQ(words(Area::TextBox), 1);
}

// ---------- Alignment ----------

TEST(Fidelity_Align) {
  std::vector<int> m = Align({1, 2, 3, 4, 5}, {1, 3, 4, 9, 5});
  CHECK((m == std::vector<int>{0, -1, 1, 2, 4}));
  m = Align({7, 7, 7, 1}, {7, 7, 1});
  CHECK((m == std::vector<int>{0, 1, -1, 2}));
  // Unique words anchor the alignment, even far apart.
  std::vector<int> a, b;
  for (int i = 0; i < 2000; ++i) a.push_back(i), b.push_back(i);
  b.insert(b.begin() + 1000, 99999);
  b.erase(b.begin() + 10);
  m = Align(a, b);
  int mapped = 0;
  for (int i = 0; i < 2000; ++i) mapped += m[i] >= 0;
  CHECK_EQ(mapped, 1999);
  CHECK_EQ(m[10], -1);
  CHECK_EQ(m[1500], 1500);
  // Reordered blocks: only one order survives.
  m = Align({5, 6, 7, 8}, {8, 7, 6, 5});
  mapped = 0;
  for (int v : m) mapped += v >= 0;
  CHECK_EQ(mapped, 1);
  // Repeated words with no anchor: a longest common subsequence, unless that's switched off.
  m = Align({3, 4, 3, 4, 3}, {4, 3, 3, 4});
  mapped = 0;
  for (int v : m) mapped += v >= 0;
  CHECK_EQ(mapped, 3);
  m = Align({3, 4, 3, 4, 3}, {4, 3, 3, 4}, false);
  for (int v : m) CHECK_EQ(v, -1);
}

// ---------- Scores ----------

TEST(Fidelity_SameLayoutScoresEverything) {
  auto d = Dump({{TextLine(L"Report", LayoutStory::Header), TextLine(L"The quick brown fox"), TextLine(L"jumps over the lazy dog."),
                  TextLine(L"R1C1", LayoutStory::Cell), TextLine(L"R1C2", LayoutStory::Cell), TextLine(L"•"),
                  TextLine(L"1 A footnote.", LayoutStory::Footnote), TextLine(L"Page 1", LayoutStory::Footer)},
                 {TextLine(L"Report", LayoutStory::Header), TextLine(L"More text here."), TextLine(L"Page 2", LayoutStory::Footer)}});
  Score s = Compare(d, d);
  CHECK(s.PagesMatch());
  CHECK_EQ(s.lines, 10);  // the bullet alone has no words
  CHECK_EQ(s.matched, 10);
  CHECK(s.FirstDifference() == nullptr);
  CHECK_NEAR(s.LinePercent(), 100.0, 1e-9);
}

TEST(Fidelity_MovedBreakCostsOneLine) {
  auto word = Dump({{TextLine(L"one two three"), TextLine(L"four five six"), TextLine(L"seven eight nine")}});
  auto ours = Dump({{TextLine(L"one two"), TextLine(L"three four five six"), TextLine(L"seven eight nine")}});
  Score s = Compare(word, ours);
  CHECK(s.PagesMatch());
  CHECK_EQ(s.lines, 3);
  CHECK_EQ(s.matched, 2);
  const LineResult* first = s.FirstDifference();
  CHECK(first != nullptr);
  if (!first) return;
  CHECK_EQ(first->line, 0);
  CHECK_EQ(first->ours, 0);
  CHECK_EQ(first->wordsLater, -1);  // ours ends one word earlier

  // The other way round: ours keeps a word Word moved on.
  s = Compare(ours, word);
  CHECK_EQ(s.matched, 2);
  CHECK_EQ(s.FirstDifference()->wordsLater, 1);
}

TEST(Fidelity_AddedPageChangesOnlyThePageCount) {
  auto word = Dump({{TextLine(L"one two"), TextLine(L"three four")}});
  auto ours = Dump({{TextLine(L"one two")}, {TextLine(L"three four")}});
  Score s = Compare(word, ours);
  CHECK(!s.PagesMatch());
  CHECK_EQ(s.referencePages, 1);
  CHECK_EQ(s.pages, 2);
  CHECK_EQ(s.matched, s.lines);
}

TEST(Fidelity_HyphenatedBreaks) {
  auto word = Dump({{TextLine(L"a b accord-"), TextLine(L"ing c d")}});
  Score same = Compare(word, word);
  CHECK_EQ(same.matched, 2);
  // Not hyphenated: the line ends a word earlier.
  Score s = Compare(word, Dump({{TextLine(L"a b"), TextLine(L"according c d")}}));
  CHECK_EQ(s.matched, 1);
  CHECK_EQ(s.FirstDifference()->wordsLater, -1);
  // Hyphenated elsewhere in the same word.
  s = Compare(word, Dump({{TextLine(L"a b accordi-"), TextLine(L"ng c d")}}));
  CHECK_EQ(s.matched, 1);
  CHECK_EQ(s.FirstDifference()->wordsLater, 0);
  // A soft hyphen drawn as U+00AD on one side, U+0002 on the other.
  s = Compare(Dump({{TextLine(L"a b accord\x02"), TextLine(L"ing c d")}}), Dump({{TextLine(L"a b accord­"), TextLine(L"ing c d")}}));
  CHECK_EQ(s.matched, 2);
}

TEST(Fidelity_AreasDontDisturbEachOther) {
  auto word = Dump({{TextLine(L"Report title", LayoutStory::Header), TextLine(L"alpha beta"), TextLine(L"gamma delta"),
                     TextLine(L"box words here", LayoutStory::TextBox), TextLine(L"1 A note", LayoutStory::Footnote),
                     TextLine(L"Page 1", LayoutStory::Footer)}});
  auto ours = Dump({{TextLine(L"box words here", LayoutStory::TextBox), TextLine(L"Page 1", LayoutStory::Footer),
                     TextLine(L"alpha beta"), TextLine(L"1 A note", LayoutStory::Footnote),
                     TextLine(L"Report title", LayoutStory::Header), TextLine(L"gamma delta")}});
  Score s = Compare(word, ours);
  CHECK_EQ(s.lines, 6);
  CHECK_EQ(s.matched, 6);
}

TEST(Fidelity_TextInAnotherAreaIsStillFound) {
  // Ours lists the text box with the body text, at the end.
  auto word = Dump({{TextLine(L"alpha beta gamma"), TextLine(L"inside the box", LayoutStory::TextBox), TextLine(L"delta epsilon")}});
  auto ours = Dump({{TextLine(L"alpha beta gamma"), TextLine(L"delta epsilon"), TextLine(L"inside the box")}});
  Score s = Compare(word, ours);
  CHECK_EQ(s.matched, 3);
}

TEST(Fidelity_ChangedWordsKeepTheirLines) {
  auto word = Dump({{TextLine(L"the quick brown fox"), TextLine(L"jumps over the dog"), TextLine(L"and runs away")}});
  auto ours = Dump({{TextLine(L"the quick brown fox"), TextLine(L"jumps over a dog"), TextLine(L"and runs")}});
  Score s = Compare(word, ours);
  CHECK_EQ(s.lines, 3);
  CHECK_EQ(s.matched, 2);  // "away" is missing, so the last line can't end there
  CHECK_EQ(s.FirstDifference()->line, 2);
}

TEST(Fidelity_RepeatedHeadersAndFooters) {
  std::vector<std::vector<LayoutLine>> word, ours;
  for (int p = 1; p <= 3; ++p) {
    word.push_back({TextLine(L"Annual report", LayoutStory::Header), TextLine(L"Text of page " + std::to_wstring(p)),
                    TextLine(L"Page " + std::to_wstring(p), LayoutStory::Footer)});
    ours.push_back({TextLine(L"Annual report", LayoutStory::Header), TextLine(L"Text of page " + std::to_wstring(p)),
                    TextLine(L"Page " + std::to_wstring(p == 2 ? 3 : p), LayoutStory::Footer)});
  }
  Score s = Compare(Dump(word), Dump(ours));
  CHECK_EQ(s.lines, 9);
  CHECK_EQ(s.matched, 8);  // page 2's footer says 3
}

TEST(Fidelity_Perturbations) {
  auto word = Dump({{TextLine(L"Heading"), TextLine(L"one two three"), TextLine(L"four five six"), TextLine(L"seven eight nine-"),
                     TextLine(L"teen ten"), TextLine(L"Page 1", LayoutStory::Footer)},
                    {TextLine(L"eleven twelve"), TextLine(L"thirteen •")}});
  LayoutDump moved = word;
  int line = MoveBreak(moved);
  CHECK(line >= 0);
  Score s = Compare(word, moved);
  CHECK(s.PagesMatch());
  CHECK_EQ(s.matched, s.lines - 1);
  CHECK(s.FirstDifference() != nullptr && s.FirstDifference()->line == line);

  LayoutDump added = word;
  CHECK(AddPage(added));
  s = Compare(word, added);
  CHECK_EQ(s.pages, 3);
  CHECK_EQ(s.matched, s.lines);

  LayoutDump empty = Dump({{}});
  CHECK_EQ(MoveBreak(empty), -1);
  CHECK(!AddPage(empty));
}
