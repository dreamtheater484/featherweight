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

// The n-th letter of an alphabet, then each letter doubled, tripled (Word's way with letters).
std::u16string Cycle(int64_t n, std::u16string_view letters) {
  if (n <= 0) return ToUtf16(std::to_string(n));
  const int64_t k = static_cast<int64_t>(letters.size());
  return std::u16string(static_cast<size_t>(std::min<int64_t>((n - 1) / k + 1, 64)), letters[(n - 1) % k]);
}

// Each decimal digit in another script's digits.
std::u16string Digits(int64_t n, char16_t zero) {
  std::u16string out;
  for (char c : std::to_string(n)) out += c == '-' ? u'-' : static_cast<char16_t>(zero + (c - '0'));
  return out;
}

// Hebrew numerals (gematria): tens and ones as letters, hundreds from ק to ת; 15 and 16 as ט״ו, ט״ז.
std::u16string Hebrew(int64_t n) {
  if (n <= 0 || n > 9999) return ToUtf16(std::to_string(n));
  std::u16string out;
  int64_t r = n;
  while (r >= 400) out += u'ת', r -= 400;
  if (r >= 100) out += u"קרש"[r / 100 - 1], r %= 100;
  if (r == 15 || r == 16) return out + (r == 15 ? u"טו" : u"טז");
  if (r >= 10) out += u"יכלמנסעפצ"[r / 10 - 1], r %= 10;
  if (r) out += u"אבגדהוזחט"[r - 1];
  return out;
}

// Chinese and Japanese counting: 十, 二十一, 百五, 千...; up to 9999.
std::u16string Counting(int64_t n) {
  static constexpr char16_t kDigits[] = u"〇一二三四五六七八九";
  if (n <= 0 || n > 9999) return Digits(n, 0);
  if (n < 10) return std::u16string(1, kDigits[n]);
  std::u16string out;
  static constexpr char16_t kUnits[] = {u'千', u'百', u'十'};
  int64_t place = 1000;
  for (char16_t unit : kUnits) {
    const int64_t d = n / place % 10;
    if (d) {
      if (d > 1) out += kDigits[d];
      out += unit;
    }
    place /= 10;
  }
  if (n % 10) out += kDigits[n % 10];
  return out;
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
  // Right-to-left and Asian scripts' numbering.
  if (fmt == "hebrew1") return Hebrew(n);
  if (fmt == "hebrew2") return Cycle(n, u"אבגדהוזחטיכלמנסעפצקרשת");
  if (fmt == "arabicAlpha") return Cycle(n, u"أبتثجحخدذرزسشصضطظعغفقكلمنهوي");
  if (fmt == "arabicAbjad") return Cycle(n, u"أبجدهوزحطيكلمنسعفصقرشتثخذضظغ");
  if (fmt == "hindiNumbers") return Digits(n, u'٠');
  if (fmt == "thaiNumbers") return Digits(n, u'๐');
  if (fmt == "ideographDigital" || fmt == "japaneseDigitalTenThousand") {
    std::u16string out;
    for (char c : std::to_string(n)) out += c == '-' ? u'-' : u"〇一二三四五六七八九"[c - '0'];
    return out;
  }
  if (fmt == "japaneseCounting" || fmt == "chineseCounting" || fmt == "chineseCountingThousand" || fmt == "taiwaneseCounting" ||
      fmt == "taiwaneseCountingThousand" || fmt == "koreanCounting")
    return Counting(n);
  if (fmt == "ideographTraditional") return Cycle(n, u"甲乙丙丁戊己庚辛壬癸");
  if (fmt == "ideographZodiac") return Cycle(n, u"子丑寅卯辰巳午未申酉戌亥");
  if (fmt == "aiueo")
    return Cycle(n, u"ｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿﾀﾁﾂﾃﾄﾅﾆﾇﾈﾉﾊﾋﾌﾍﾎﾏﾐﾑﾒﾓﾔﾕﾖﾗﾘﾙﾚﾛﾜｦﾝ");
  if (fmt == "aiueoFullWidth")
    return Cycle(n, u"アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲン");
  if (fmt == "iroha" || fmt == "irohaFullWidth")
    return Cycle(n, u"イロハニホヘトチリヌルヲワカヨタレソツネナラムウヰノオクヤマケフコエテアサキユメミシヱヒモセス");
  if (fmt == "ganada") return Cycle(n, u"가나다라마바사아자차카타파하");
  if (fmt == "chosung") return Cycle(n, u"ㄱㄴㄷㄹㅁㅂㅅㅇㅈㅊㅋㅌㅍㅎ");
  if (fmt == "decimalEnclosedCircleChinese" || fmt == "ideographEnclosedCircle")
    if (n >= 1 && n <= 10) return std::u16string(1, static_cast<char16_t>((fmt == "ideographEnclosedCircle" ? 0x3280 : 0x2460) + n - 1));
  return ToUtf16(std::to_string(n));
}

namespace {

// A paragraph that only holds a section break: properties with a w:sectPr, and nothing but
// bookmarks and the like besides. Word gives it no number and doesn't count it in its list.
bool OnlySectionBreak(Node p) {
  Node ppr = p.Child(Name("pPr"));
  if (!ppr || !ppr.Child(Name("sectPr"))) return false;
  for (Node k : p.Kids())
    if (!k.Is(Name("pPr")) && !k.Is(Name("bookmarkStart")) && !k.Is(Name("bookmarkEnd")) && !k.Is(Name("proofErr")) &&
        !k.Is(Name("permStart")) && !k.Is(Name("permEnd")) && !k.Is(Name("commentRangeStart")) &&
        !k.Is(Name("commentRangeEnd")))
      return false;
  return true;
}

}  // namespace

std::optional<ListLabel> Document::Label(uint32_t para) const {
  if (para >= paras_.size()) return std::nullopt;
  Caches& c = *caches_;
  uint32_t story = paras_[para].story;
  if (story == kNone) return std::nullopt;
  // Not in a list: no label, without numbering the story as far as here (after an edit, a big
  // table's cells would each wait for that).
  if (!InList(para) || ParaNode(para).DisplayOnly() || OnlySectionBreak(ParaNode(para))) return std::nullopt;
  if (c.labelPass.size() < stories_.size()) c.labelPass.resize(stories_.size());
  Caches::LabelPass& pass = c.labelPass[story];
  if (pass.paras.empty()) pass.paras = StoryParagraphs(story);
  // The story in document order, as far as this paragraph.
  auto& lists = pass.lists;  // by abstract definition
  auto& numSeen = pass.numSeen;
  while (!pass.Passed(para) && pass.next < pass.paras.size()) {
    uint32_t pi = pass.paras[pass.next++];
    if (!InList(pi) || ParaNode(pi).DisplayOnly() || OnlySectionBreak(ParaNode(pi))) continue;
    const ParaProps& pp = ResolvedPara(pi);
    int32_t level = pp.ilvl;
    Node lvl = NumberingLevel(pp.numId, level);
    if (!lvl) continue;
    auto numIt = caches_->nums.find(pp.numId);
    if (numIt == caches_->nums.end()) continue;
    Node num(&tree_, numIt->second);
    std::string_view key = num.Child(Name("abstractNumId")).AttrOr(kVal);
    Caches::ListCounters& list = lists[key];
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
  auto it = c.labels.find(para);
  if (it == c.labels.end()) return std::nullopt;
  return it->second;
}

}  // namespace fw::doc
