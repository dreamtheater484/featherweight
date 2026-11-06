#include "Fidelity.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>

#include "core/Strings.h"

namespace fwtool::fidelity {
namespace {

using fw::LayoutDump;
using fw::LayoutStory;

std::u32string CodePoints(std::wstring_view s) {
  std::u32string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    char32_t c = s[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF)
      c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
    out += c;
  }
  return out;
}

// U+0002 is how PDFium reports a hyphen it found at a line end.
bool IsHyphen(char32_t c) { return c == 0x2D || c == 0xAD || c == 0x2010 || c == 0x2011 || c == 0x02; }

bool IsSeparator(char32_t c) {
  return (c <= 0x20 && c != 0x02) || c == 0x7F || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200B) ||
         c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

// Ideographs and kana: a line may break between any two, so each is a word of its own.
bool IsCjk(char32_t c) {
  return (c >= 0x3040 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) ||
         (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x20000 && c <= 0x3FFFF);
}

bool IsWordChar(char32_t c) {
  if (c < 0x80) return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  if (c < 0xC0) return c == 0xAA || c == 0xB5 || c == 0xBA;
  if (c == 0xD7 || c == 0xF7) return false;
  if (c >= 0x2000 && c <= 0x206F) return false;  // general punctuation
  if (c >= 0x20A0 && c <= 0x20CF) return false;  // currency signs
  if (c >= 0x2190 && c <= 0x2BFF) return false;  // arrows, math, shapes, dingbats
  if (c >= 0x3000 && c <= 0x303F) return false;  // CJK punctuation
  if (c >= 0xE000 && c <= 0xF8FF) return false;  // private use: Symbol and Wingdings bullets
  if (c >= 0xFE30 && c <= 0xFE4F) return false;
  if ((c >= 0xFF01 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) || (c >= 0xFF3B && c <= 0xFF40) ||
      (c >= 0xFF5B && c <= 0xFF65))
    return false;  // fullwidth punctuation
  return c != 0xFFFD && c != 0xFEFF;
}

std::wstring KeyOf(const char32_t* begin, const char32_t* end) {
  static const wchar_t* const kLigatures[] = {L"ff", L"fi", L"fl", L"ffi", L"ffl", L"st", L"st"};
  std::wstring key;
  for (const char32_t* p = begin; p != end; ++p) {
    char32_t c = *p;
    if (c >= 0xFB00 && c <= 0xFB06) {
      key += kLigatures[c - 0xFB00];
    } else if (!IsWordChar(c)) {
      continue;
    } else if (c >= 0x10000) {
      key += static_cast<wchar_t>(0xD800 + ((c - 0x10000) >> 10));
      key += static_cast<wchar_t>(0xDC00 + ((c - 0x10000) & 0x3FF));
    } else {
      key += static_cast<wchar_t>(c);
    }
  }
  return fw::ToLower(key);
}

bool IsLeader(char32_t c) { return c == '.' || c == '-' || c == '_' || c == 0xB7 || c == 0x2024 || c == 0x2026; }

// The words of a line as drawn, [begin, end) in code points: split at spaces, ideographs alone.
// A tab leader drawn as text (three or more dots, hyphens or underscores) stands for the tab, so it
// separates words too: Word's PDFs often have no space in "PAGE.......3".
std::vector<std::pair<size_t, size_t>> RawWords(const std::u32string& cps, size_t n) {
  std::vector<std::pair<size_t, size_t>> words;
  size_t start = 0;
  for (size_t i = 0; i <= n; ++i) {
    size_t leader = i;
    while (leader < n && IsLeader(cps[leader])) ++leader;
    bool cut = i == n || IsSeparator(cps[i]) || IsCjk(cps[i]) || leader - i >= 3;
    if (!cut) {
      if (leader > i) i = leader - 1;  // a shorter run of them is part of the word
      continue;
    }
    if (i > start) words.emplace_back(start, i);
    if (i < n && IsCjk(cps[i])) words.emplace_back(i, i + 1);
    if (leader - i >= 3) i = leader - 1;
    start = i + 1;
  }
  return words;
}

size_t TrimmedLength(const std::u32string& cps) {
  size_t n = cps.size();
  while (n && IsSeparator(cps[n - 1])) --n;
  return n;
}

int64_t PosKey(int token, int offset) { return (static_cast<int64_t>(token) << 32) | static_cast<uint32_t>(offset); }

// The line of `text` that holds character offset - 1 of word `word` (where a line ending after
// `offset` characters would be).
int LineAt(const Text& text, int word, int offset) {
  int line = text.words[word].line;
  int areaEnd = text.lineStart[static_cast<int>(text.lines[line].area) + 1];
  while (line + 1 < areaEnd && text.lines[line].end.token == word && text.lines[line].end.offset < offset) ++line;
  return line;
}

// Where a line starts: at its first word, or inside it when the line before ended there.
BreakPos StartOf(const Text& text, int line) {
  const Line& l = text.lines[line];
  bool inside = line > 0 && text.lines[line - 1].end.token == l.firstWord;
  return {l.firstWord, inside ? text.lines[line - 1].end.offset : 0};
}

}  // namespace

Area AreaOf(LayoutStory story) {
  switch (story) {
    case LayoutStory::Header: return Area::Header;
    case LayoutStory::Footer: return Area::Footer;
    case LayoutStory::Footnote:
    case LayoutStory::Endnote: return Area::Notes;
    case LayoutStory::TextBox: return Area::TextBox;
    case LayoutStory::LineNumber: return Area::LineNumbers;
    default: return Area::Main;
  }
}

const char* AreaName(Area area) {
  static const char* const kNames[] = {"main text", "notes", "headers", "footers", "text boxes", "line numbers"};
  return kNames[static_cast<int>(area)];
}

std::wstring WordKey(std::wstring_view word) {
  std::u32string cps = CodePoints(word);
  return KeyOf(cps.data(), cps.data() + cps.size());
}

Text Tokenize(const LayoutDump& dump) {
  Text t;
  struct Source {
    Line line;
    const std::wstring* text;
  };
  std::vector<Source> byArea[kAreas];
  for (size_t p = 0; p < dump.pages.size(); ++p)
    for (size_t i = 0; i < dump.pages[p].lines.size(); ++i) {
      const fw::LayoutLine& l = dump.pages[p].lines[i];
      Line line;
      line.area = AreaOf(l.story);
      line.page = static_cast<int>(p);
      line.index = static_cast<int>(i);
      byArea[static_cast<int>(line.area)].push_back({line, &l.text});
    }
  for (int a = 0; a < kAreas; ++a) {
    t.wordStart[a] = static_cast<int>(t.words.size());
    t.lineStart[a] = static_cast<int>(t.lines.size());
    bool join = false;  // the previous line ended inside a hyphenated word
    for (const Source& src : byArea[a]) {
      Line line = src.line;
      int lineIndex = static_cast<int>(t.lines.size());
      std::u32string cps = CodePoints(*src.text);
      size_t n = TrimmedLength(cps);
      size_t lastEnd = 0;
      for (auto [b, e] : RawWords(cps, n)) {
        std::wstring key = KeyOf(cps.data() + b, cps.data() + e);
        if (key.empty()) continue;
        if (join && line.firstWord < 0) {
          t.words.back().key += key;  // the rest of the word the previous line hyphenated
        } else {
          t.words.push_back({std::move(key), lineIndex});
        }
        if (line.firstWord < 0) line.firstWord = static_cast<int>(t.words.size()) - 1;
        lastEnd = e;
      }
      join = false;
      if (line.firstWord >= 0) {
        line.end = {static_cast<int>(t.words.size()) - 1, static_cast<int>(t.words.back().key.size())};
        join = lastEnd == n && IsHyphen(cps[n - 1]);
      }
      t.lines.push_back(line);
    }
  }
  t.wordStart[kAreas] = static_cast<int>(t.words.size());
  t.lineStart[kAreas] = static_cast<int>(t.lines.size());
  return t;
}

std::vector<int> Align(const std::vector<int>& a, const std::vector<int>& b, bool lcs) {
  // Larger stretches without an anchor stay unaligned rather than take seconds and memory.
  constexpr int64_t kMaxCells = 8'000'000;
  std::vector<int> map(a.size(), -1);
  struct Range {
    int a0, a1, b0, b1;
  };
  std::vector<Range> work{{0, static_cast<int>(a.size()), 0, static_cast<int>(b.size())}};
  while (!work.empty()) {
    Range r = work.back();
    work.pop_back();
    while (r.a0 < r.a1 && r.b0 < r.b1 && a[r.a0] == b[r.b0]) map[r.a0++] = r.b0++;
    while (r.a0 < r.a1 && r.b0 < r.b1 && a[r.a1 - 1] == b[r.b1 - 1]) map[--r.a1] = --r.b1;
    if (r.a0 == r.a1 || r.b0 == r.b1) continue;

    // Anchors: values that occur exactly once in both ranges, in the longest run that keeps
    // their order on both sides.
    struct Count {
      int na = 0, nb = 0, pb = -1;
    };
    std::unordered_map<int, Count> counts;
    for (int i = r.a0; i < r.a1; ++i) ++counts[a[i]].na;
    for (int j = r.b0; j < r.b1; ++j) {
      auto it = counts.find(b[j]);
      if (it != counts.end()) ++it->second.nb, it->second.pb = j;
    }
    std::vector<std::pair<int, int>> unique;  // (index in a, index in b), by a
    for (int i = r.a0; i < r.a1; ++i) {
      const Count& c = counts[a[i]];
      if (c.na == 1 && c.nb == 1) unique.emplace_back(i, c.pb);
    }
    if (!unique.empty()) {
      // Longest increasing subsequence of the b indices (patience sorting).
      std::vector<int> tails, prev(unique.size(), -1);
      for (int k = 0; k < static_cast<int>(unique.size()); ++k) {
        auto it = std::lower_bound(tails.begin(), tails.end(), unique[k].second,
                                   [&](int t, int v) { return unique[t].second < v; });
        if (it != tails.begin()) prev[k] = *(it - 1);
        if (it == tails.end()) tails.push_back(k);
        else *it = k;
      }
      std::vector<std::pair<int, int>> chain;
      for (int k = tails.back(); k >= 0; k = prev[k]) chain.push_back(unique[k]);
      std::reverse(chain.begin(), chain.end());
      int pa = r.a0, pb = r.b0;
      for (auto [i, j] : chain) {
        map[i] = j;
        if (i > pa && j > pb) work.push_back({pa, i, pb, j});
        pa = i + 1;
        pb = j + 1;
      }
      if (r.a1 > pa && r.b1 > pb) work.push_back({pa, r.a1, pb, r.b1});
      continue;
    }

    int n = r.a1 - r.a0, m = r.b1 - r.b0;
    if (!lcs || static_cast<int64_t>(n + 1) * (m + 1) > kMaxCells) continue;
    // Longest common subsequence. Fits in 16 bits: kMaxCells keeps the shorter side under 2829.
    std::vector<uint16_t> len(static_cast<size_t>(n + 1) * (m + 1), 0);
    auto at = [&](int i, int j) -> uint16_t& { return len[static_cast<size_t>(i) * (m + 1) + j]; };
    for (int i = n - 1; i >= 0; --i)
      for (int j = m - 1; j >= 0; --j)
        at(i, j) = a[r.a0 + i] == b[r.b0 + j] ? static_cast<uint16_t>(at(i + 1, j + 1) + 1)
                                              : std::max(at(i + 1, j), at(i, j + 1));
    for (int i = 0, j = 0; i < n && j < m;) {
      if (a[r.a0 + i] == b[r.b0 + j]) {
        map[r.a0 + i] = r.b0 + j;
        ++i, ++j;
      } else if (at(i + 1, j) >= at(i, j + 1)) {
        ++i;
      } else {
        ++j;
      }
    }
  }
  return map;
}

const LineResult* Score::FirstDifference() const {
  for (const LineResult& r : results)
    if (!r.matched) return &r;
  return nullptr;
}

Score Compare(const LayoutDump& reference, const LayoutDump& ours) {
  Score s;
  s.referencePages = static_cast<int>(reference.pages.size());
  s.pages = static_cast<int>(ours.pages.size());
  s.reference = Tokenize(reference);
  s.ours = Tokenize(ours);
  const Text& R = s.reference;
  const Text& O = s.ours;

  std::unordered_map<std::wstring, int> ids;
  auto idsOf = [&](const Text& t) {
    std::vector<int> v;
    v.reserve(t.words.size());
    for (const Word& w : t.words) v.push_back(ids.emplace(w.key, static_cast<int>(ids.size())).first->second);
    return v;
  };
  std::vector<int> ra = idsOf(R), oa = idsOf(O);

  // Each area on its own, in reading order.
  s.wordMap.assign(ra.size(), -1);
  for (int a = 0; a < kAreas; ++a) {
    std::vector<int> x(ra.begin() + R.wordStart[a], ra.begin() + R.wordStart[a + 1]);
    std::vector<int> y(oa.begin() + O.wordStart[a], oa.begin() + O.wordStart[a + 1]);
    std::vector<int> m = Align(x, y);
    for (size_t i = 0; i < m.size(); ++i)
      if (m[i] >= 0) s.wordMap[R.wordStart[a] + i] = O.wordStart[a] + m[i];
  }
  // Then what's left on both sides, across areas: text one side puts in another area (a text
  // box Word tagged as body text) or another order. Anchors only, so common words don't pair up
  // by chance.
  std::vector<bool> used(oa.size(), false);
  std::vector<int> leftR, leftO, x, y;
  for (size_t i = 0; i < ra.size(); ++i)
    if (s.wordMap[i] >= 0) used[s.wordMap[i]] = true;
    else leftR.push_back(static_cast<int>(i)), x.push_back(ra[i]);
  for (size_t j = 0; j < oa.size(); ++j)
    if (!used[j]) leftO.push_back(static_cast<int>(j)), y.push_back(oa[j]);
  if (!x.empty() && !y.empty()) {
    std::vector<int> m = Align(x, y, false);
    for (size_t k = 0; k < m.size(); ++k)
      if (m[k] >= 0) s.wordMap[leftR[k]] = leftO[m[k]];
  }

  std::unordered_map<int64_t, int> ourEnds;
  for (int l = 0; l < static_cast<int>(O.lines.size()); ++l)
    if (O.lines[l].firstWord >= 0) ourEnds[PosKey(O.lines[l].end.token, O.lines[l].end.offset)] = l;

  std::vector<int> order;
  for (int l = 0; l < static_cast<int>(R.lines.size()); ++l)
    if (R.lines[l].firstWord >= 0) order.push_back(l);
  std::sort(order.begin(), order.end(), [&](int x1, int x2) {
    return std::pair(R.lines[x1].page, R.lines[x1].index) < std::pair(R.lines[x2].page, R.lines[x2].index);
  });
  for (int l : order) {
    LineResult r;
    r.line = l;
    const Line& line = R.lines[l];
    int uEnd = s.wordMap[line.end.token];
    if (uEnd >= 0) {
      auto it = ourEnds.find(PosKey(uEnd, line.end.offset));
      if (it != ourEnds.end()) {
        r.matched = true;
        r.ours = it->second;
      }
    }
    if (!r.matched) {
      // To say where ours ends instead: our line that starts where Word's does, if there is one,
      // else the one that holds Word's last word.
      BreakPos start = StartOf(R, l);
      int uStart = s.wordMap[start.token];
      int byStart = uStart >= 0 ? LineAt(O, uStart, start.offset + 1) : -1;
      BreakPos ourStart = byStart >= 0 ? StartOf(O, byStart) : BreakPos{};
      if (byStart >= 0 && (ourStart.token == uStart && ourStart.offset == start.offset)) r.ours = byStart;
      else if (uEnd >= 0) r.ours = LineAt(O, uEnd, line.end.offset);
      else r.ours = byStart;
      if (r.ours >= 0 && uEnd >= 0) r.wordsLater = O.lines[r.ours].end.token - uEnd;
    }
    ++s.lines;
    if (r.matched) ++s.matched;
    s.results.push_back(r);
  }
  return s;
}

int MoveBreak(LayoutDump& dump) {
  Text t = Tokenize(dump);
  std::vector<int> candidates;
  int mainEnd = t.lineStart[static_cast<int>(Area::Main) + 1];
  for (int l = 0; l + 1 < mainEnd; ++l) {
    const Line& line = t.lines[l];
    const Line& next = t.lines[l + 1];
    if (line.firstWord < 0 || next.firstWord < 0 || next.page != line.page) continue;
    // At least two whole words, no hyphen at the end, and the next line starts a new word.
    const Word& last = t.words[line.end.token];
    if (line.end.token <= line.firstWord || t.words[line.firstWord].line != l ||
        line.end.offset != static_cast<int>(last.key.size()) || last.line != l || t.words[next.firstWord].line != l + 1)
      continue;
    // The last word as drawn is that word (no bullet, leader or ideograph after it).
    const std::wstring& text = dump.pages[line.page].lines[line.index].text;
    std::u32string cps = CodePoints(text);
    auto raw = RawWords(cps, TrimmedLength(cps));
    if (raw.empty() || KeyOf(cps.data() + raw.back().first, cps.data() + raw.back().second) != last.key ||
        IsCjk(cps[raw.back().first]))
      continue;
    candidates.push_back(l);
  }
  if (candidates.empty()) return -1;
  int l = candidates[candidates.size() / 2];  // one from the middle of the document
  const Line& line = t.lines[l];
  const Line& next = t.lines[l + 1];
  std::wstring& from = dump.pages[line.page].lines[line.index].text;
  std::wstring& to = dump.pages[next.page].lines[next.index].text;
  size_t end = from.find_last_not_of(L" \t ");
  size_t start = from.find_last_of(L" \t ", end);
  std::wstring moved = from.substr(start + 1, end - start);
  from = from.substr(0, from.find_last_not_of(L" \t ", start) + 1);
  to = moved + L" " + to.substr(std::min(to.size(), to.find_first_not_of(L" \t ")));
  return l;
}

bool AddPage(LayoutDump& dump) {
  for (size_t p = dump.pages.size(); p-- > 0;) {
    if (dump.pages[p].lines.empty()) continue;
    fw::LayoutPage page{dump.pages[p].width, dump.pages[p].height, {dump.pages[p].lines.back()}};
    dump.pages[p].lines.pop_back();
    dump.pages.push_back(std::move(page));
    return true;
  }
  return false;
}

}  // namespace fwtool::fidelity
