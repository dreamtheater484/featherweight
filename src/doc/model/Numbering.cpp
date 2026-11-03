// List labels (ECMA-376 Part 1 §17.9, as Word implements it). Counters are kept per abstract
// definition, so every w:num that refers to the same w:abstractNum continues the same list (as
// Word does, and LibreOffice since tdf#95848); a w:startOverride restarts its level where the
// w:num is first used. Word's departures, from [MS-OI29500]:
//  - w:start inside an override's w:lvl is ignored (2.1.292), and so is w:lvlRestart (2.1.282);
//  - a level restarts when the level its w:lvlRestart names, or any higher one, is used; without
//    w:lvlRestart, after any higher level (2.1.282);
//  - w:isLgl shows every level as decimal, the level itself included (2.1.278);
//  - a %N in w:lvlText for a level below the current one makes Word ignore the whole text (2.1.283);
//  - bullets show w:lvlText as it is (2.1.286).
#include <algorithm>
#include <array>
#include <cstdio>
#include <unordered_map>

#include "doc/model/Internal.h"
#include "doc/model/Utf.h"

namespace fw::doc {

using namespace names;

namespace {

std::u16string Roman(int64_t n, bool upper) {
  if (n <= 0 || n >= 4000) return ToUtf16(std::to_string(n));
  static constexpr std::pair<int, std::string_view> kParts[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"},
                                                               {100, "c"},  {90, "xc"},  {50, "l"},  {40, "xl"},
                                                               {10, "x"},   {9, "ix"},   {5, "v"},   {4, "iv"},
                                                               {1, "i"}};
  std::string out;
  for (auto [value, text] : kParts)
    while (n >= value) {
      out += text;
      n -= value;
    }
  if (upper)
    for (char& c : out) c = static_cast<char>(c - 32);
  return ToUtf16(out);
}

// Word's letters: a..z, then aa..zz, aaa..zzz (the letter repeats).
std::u16string Letters(int64_t n, bool upper) {
  if (n <= 0) return ToUtf16(std::to_string(n));
  int64_t repeat = (n - 1) / 26 + 1;
  char16_t letter = static_cast<char16_t>((upper ? u'A' : u'a') + (n - 1) % 26);
  return std::u16string(static_cast<size_t>(std::min<int64_t>(repeat, 64)), letter);
}

constexpr std::string_view kOnes[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
                                      "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
                                      "seventeen", "eighteen", "nineteen"};
constexpr std::string_view kTens[] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};

std::string Cardinal(int64_t n) {
  if (n < 20) return std::string(kOnes[n]);
  if (n < 100) return std::string(kTens[n / 10]) + (n % 10 ? "-" + std::string(kOnes[n % 10]) : "");
  if (n < 1000) return std::string(kOnes[n / 100]) + " hundred" + (n % 100 ? " " + Cardinal(n % 100) : "");
  return Cardinal(n / 1000) + " thousand" + (n % 1000 ? " " + Cardinal(n % 1000) : "");
}

std::string OrdinalWords(int64_t n) {
  static constexpr std::pair<std::string_view, std::string_view> kIrregular[] = {
      {"one", "first"}, {"two", "second"}, {"three", "third"}, {"five", "fifth"}, {"eight", "eighth"},
      {"nine", "ninth"}, {"twelve", "twelfth"}};
  std::string words = Cardinal(n);
  size_t last = words.find_last_of(" -");
  std::string head = last == std::string::npos ? "" : words.substr(0, last + 1);
  std::string tail = last == std::string::npos ? words : words.substr(last + 1);
  for (auto [from, to] : kIrregular)
    if (tail == from) return head + std::string(to);
  if (tail.ends_with('y')) return head + tail.substr(0, tail.size() - 1) + "ieth";
  return head + tail + "th";
}

std::u16string Capitalized(std::string s) {
  if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 32);
  return ToUtf16(s);
}

}  // namespace

std::u16string FormatNumber(int64_t n, std::string_view fmt) {
  if (fmt == "none") return {};
  if (fmt == "upperRoman") return Roman(n, true);
  if (fmt == "lowerRoman") return Roman(n, false);
  if (fmt == "upperLetter") return Letters(n, true);
  if (fmt == "lowerLetter") return Letters(n, false);
  if (fmt == "decimalZero") return ToUtf16((n >= 0 && n < 10 ? "0" : "") + std::to_string(n));
  if (fmt == "ordinal") {
    int64_t t = n % 100, o = n % 10;
    const char* suffix = t >= 11 && t <= 13 ? "th" : o == 1 ? "st" : o == 2 ? "nd" : o == 3 ? "rd" : "th";
    return ToUtf16(std::to_string(n) + suffix);
  }
  if ((fmt == "cardinalText" || fmt == "ordinalText") && n >= 0 && n <= 999999)
    return Capitalized(fmt == "cardinalText" ? Cardinal(n) : OrdinalWords(n));
  if (fmt == "chicago" && n > 0) {
    static constexpr char16_t kMarks[] = {u'*', u'†', u'‡', u'§'};
    return std::u16string(static_cast<size_t>(std::min<int64_t>((n - 1) / 4 + 1, 64)), kMarks[(n - 1) % 4]);
  }
  if (fmt == "decimalEnclosedCircle" && n >= 1 && n <= 20) return std::u16string(1, static_cast<char16_t>(0x2460 + n - 1));
  if (fmt == "decimalEnclosedParen") return ToUtf16("(" + std::to_string(n) + ")");
  if (fmt == "decimalEnclosedFullstop") return ToUtf16(std::to_string(n) + ".");
  if (fmt == "numberInDash") return ToUtf16("- " + std::to_string(n) + " -");
  if (fmt == "hex" && n >= 0 && n <= 65535) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%llX", static_cast<unsigned long long>(n));
    return ToUtf16(buf);
  }
  if (fmt == "decimalFullWidth" || fmt == "decimalFullWidth2") {
    std::u16string out;
    for (char c : std::to_string(n)) out += c == '-' ? u'－' : static_cast<char16_t>(0xFF10 + (c - '0'));
    return out;
  }
  return ToUtf16(std::to_string(n));
}

std::optional<ListLabel> Document::Label(uint32_t para) const {
  if (para >= paras_.size()) return std::nullopt;
  Caches& c = *caches_;
  uint32_t story = paras_[para].story;
  if (story == kNone) return std::nullopt;
  if (c.labelsDone.size() < stories_.size()) c.labelsDone.resize(stories_.size(), false);
  if (!c.labelsDone[story]) {
    c.labelsDone[story] = true;
    // One pass over the story in document order.
    struct List {
      std::array<int64_t, 9> count{};
      std::array<bool, 9> used{};
    };
    std::unordered_map<std::string_view, List> lists;  // by abstract definition
    std::unordered_map<std::string_view, bool> numSeen;
    for (uint32_t pi : StoryParagraphs(story)) {
      const ParaProps& pp = ResolvedPara(pi);
      if (pp.numId.empty() || ParaNode(pi).DisplayOnly()) continue;
      int32_t level = pp.ilvl;
      Node lvl = NumberingLevel(pp.numId, level);
      if (!lvl) continue;
      auto numIt = caches_->nums.find(pp.numId);
      if (numIt == caches_->nums.end()) continue;
      Node num(&tree_, numIt->second);
      std::string_view key = num.Child(Name("abstractNumId")).AttrOr(kVal);
      List& list = lists[key];
      auto levelNode = [&](int32_t k) { return NumberingLevel(pp.numId, k); };
      auto startOf = [&](int32_t k) {
        // The abstract definition's start: an override's w:lvl doesn't change it (2.1.292).
        Node l = levelNode(k);
        if (l && l.Parent().Is(Name("lvlOverride"))) {
          auto absIt = caches_->abstractNums.find(key);
          if (absIt != caches_->abstractNums.end())
            for (Node al : Node(&tree_, absIt->second).Kids())
              if (al.Is(Name("lvl")) && al.Int(Name("ilvl")).value_or(-1) == k) l = al;
        }
        return l ? l.Child(Name("start")).Int(kVal).value_or(0) : 0;
      };
      // A w:num's start overrides restart their levels where it's first used.
      if (!numSeen[pp.numId]) {
        numSeen[pp.numId] = true;
        for (Node o : num.Kids()) {
          if (!o.Is(Name("lvlOverride"))) continue;
          auto k = o.Int(Name("ilvl"));
          Node so = o.Child(Name("startOverride"));
          if (!k || !so || *k < 0 || *k > 8) continue;
          list.count[*k] = so.Int(kVal).value_or(0) - 1;
          list.used[*k] = true;
        }
      }
      if (list.used[level]) ++list.count[level];
      else list.count[level] = startOf(level);
      list.used[level] = true;
      // Lower levels restart after this one, as their w:lvlRestart says.
      for (int32_t k = level + 1; k < 9; ++k) {
        Node kl = levelNode(k);
        std::optional<int64_t> restart;
        if (kl && !kl.Parent().Is(Name("lvlOverride"))) restart = kl.Child(Name("lvlRestart")).Int(kVal);
        if (restart && *restart == 0) continue;  // never
        int64_t after = restart ? *restart - 1 : k - 1;
        if (level <= after) list.used[k] = false;
      }

      ListLabel label;
      label.level = level;
      label.numId = pp.numId;
      label.lvl = lvl;
      label.numFmt = lvl.Child(Name("numFmt")).AttrOr(kVal, "decimal");
      label.bullet = label.numFmt == "bullet";
      label.suffix = lvl.Child(Name("suff")).AttrOr(kVal, "tab");
      label.jc = ParseJustification(lvl.Child(Name("lvlJc")).AttrOr(kVal));
      bool legal = lvl.Child(Name("isLgl")) && lvl.Child(Name("isLgl")).On();
      std::u16string text = ToUtf16(lvl.Child(Name("lvlText")).AttrOr(kVal));
      if (!label.bullet) {
        std::u16string out;
        bool ignore = false;
        for (size_t i = 0; i < text.size(); ++i) {
          if (text[i] == u'%' && i + 1 < text.size() && text[i + 1] >= u'1' && text[i + 1] <= u'9') {
            int32_t k = text[i + 1] - u'1';
            ++i;
            if (k > level) {
              ignore = true;  // 2.1.283
              break;
            }
            int64_t n = list.used[k] ? list.count[k] : startOf(k);
            Node kl = levelNode(k);
            std::string_view fmt = kl ? kl.Child(Name("numFmt")).AttrOr(kVal, "decimal") : std::string_view("decimal");
            if (legal && fmt != "none") fmt = "decimal";  // 2.1.278
            out += FormatNumber(n, fmt);
          } else {
            out += text[i];
          }
        }
        text = ignore ? std::u16string() : out;
      }
      label.text = std::move(text);
      label.props = LabelProps(*this, c, pi, lvl);
      c.labels[pi] = std::move(label);
    }
  }
  auto it = c.labels.find(para);
  if (it == c.labels.end()) return std::nullopt;
  return it->second;
}

}  // namespace fw::doc
