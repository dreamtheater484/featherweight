#pragma once

// Layout fidelity (plan §10): how closely a layout dump follows Word's, page count and line
// breaks. Kept free of PDFium and the command line, so the unit tests can drive it.
#include <string>
#include <string_view>
#include <vector>

#include "core/LayoutDump.h"

namespace fwtool::fidelity {

// Lines are compared within areas, so stories the two sides may list in a different order
// (headers, notes, text boxes) don't disturb each other. Table cells are part of the main text.
enum class Area { Main, Notes, Header, Footer, TextBox };
constexpr int kAreas = 5;
Area AreaOf(fw::LayoutStory story);
const char* AreaName(Area area);

// The part of a word that is compared: lowercase letters and digits, so punctuation, quotes,
// list bullets and tab leaders don't count. Ligatures are spelled out.
std::wstring WordKey(std::wstring_view word);

// A line ends after `offset` characters of the key of word `token`: at the word's end, or inside
// it where a hyphen was added.
struct BreakPos {
  int token = -1;
  int offset = 0;
};

struct Word {
  std::wstring key;
  int line = 0;  // the line it starts on
};

struct Line {
  Area area = Area::Main;
  int page = 0;        // 0-based
  int index = 0;       // its index among the page's lines in the dump
  int firstWord = -1;  // -1: no words (bullets, leaders, empty), so it isn't compared
  BreakPos end;
};

// One side's text: words and lines, each grouped by area in the dump's order. A word hyphenated
// at a line end is joined again, with the line ending inside it.
struct Text {
  std::vector<Word> words;
  std::vector<Line> lines;
  int wordStart[kAreas + 1] = {};  // area a's words are [wordStart[a], wordStart[a + 1])
  int lineStart[kAreas + 1] = {};
};
Text Tokenize(const fw::LayoutDump& dump);

// Aligns two sequences: result[i] is the index in b matched to a[i], or -1. Common prefix and
// suffix first, then values that occur once on each side as anchors (patience diff), then a
// longest common subsequence between anchors where that's small enough (and `lcs` is set).
std::vector<int> Align(const std::vector<int>& a, const std::vector<int>& b, bool lcs = true);

struct LineResult {
  int line = 0;  // reference line (index into Score::reference.lines)
  // Our line that ends where Word's does; if none, the one that starts where Word's starts, or -1
  // if Word's words aren't in our text.
  int ours = -1;
  bool matched = false;
  int wordsLater = 0;  // where that line of ours ends, in words after Word's end (negative: before)
};

struct Score {
  int referencePages = 0;
  int pages = 0;
  int lines = 0;    // reference lines with words
  int matched = 0;  // those that end at the same word, and inside it at the same place
  bool PagesMatch() const { return referencePages == pages; }
  double LinePercent() const { return lines ? 100.0 * matched / lines : 100.0; }

  Text reference, ours;
  std::vector<int> wordMap;          // reference word -> our word, or -1
  std::vector<LineResult> results;   // every compared reference line, in page order
  const LineResult* FirstDifference() const;
};

// Measures `ours` against `reference` (Word's lines, from its PDF).
Score Compare(const fw::LayoutDump& reference, const fw::LayoutDump& ours);

// The perturbations the self-test applies to a dump of Word's own lines, with known effects.
// MoveBreak moves the last word of a line with at least two words to the start of the next line
// of its area on the same page: exactly that line no longer matches. Returns its index in
// Tokenize(dump).lines, or -1 if no line qualifies.
int MoveBreak(fw::LayoutDump& dump);
// AddPage moves the last line onto a page of its own: the page count differs, the lines don't.
bool AddPage(fw::LayoutDump& dump);

}  // namespace fwtool::fidelity
