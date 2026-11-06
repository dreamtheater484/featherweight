#include "doc/proof/AutoCorrect.h"

#include <algorithm>
#include <cwctype>

#include "doc/proof/Language.h"

namespace fw::doc::proof {

namespace {

// Word's common English replacements (its default list is much longer; these are the ones that
// come up). Lowercase; the typed word's capitals are kept.
constexpr std::pair<std::u16string_view, std::u16string_view> kEnglish[] = {
    {u"abotu", u"about"}, {u"accomodate", u"accommodate"}, {u"accross", u"across"}, {u"acheive", u"achieve"},
    {u"acheived", u"achieved"}, {u"acn", u"can"}, {u"adn", u"and"}, {u"agian", u"again"}, {u"ahve", u"have"},
    {u"alot", u"a lot"}, {u"alwasy", u"always"}, {u"alwyas", u"always"}, {u"amde", u"made"}, {u"anf", u"and"},
    {u"anwser", u"answer"}, {u"aquire", u"acquire"}, {u"arent", u"aren't"}, {u"arguement", u"argument"},
    {u"awya", u"away"}, {u"beacuse", u"because"}, {u"becasue", u"because"}, {u"becuase", u"because"},
    {u"beggining", u"beginning"}, {u"begining", u"beginning"}, {u"beleive", u"believe"}, {u"bieng", u"being"},
    {u"breif", u"brief"}, {u"buisness", u"business"}, {u"calender", u"calendar"}, {u"cna", u"can"},
    {u"comming", u"coming"}, {u"commitee", u"committee"}, {u"completly", u"completely"}, {u"concious", u"conscious"},
    {u"couldnt", u"couldn't"}, {u"decison", u"decision"}, {u"definately", u"definitely"}, {u"didnt", u"didn't"},
    {u"diferent", u"different"}, {u"doesnt", u"doesn't"}, {u"dont", u"don't"}, {u"eachother", u"each other"},
    {u"embarass", u"embarrass"}, {u"enviroment", u"environment"}, {u"everytime", u"every time"},
    {u"exagerate", u"exaggerate"}, {u"excercise", u"exercise"}, {u"existance", u"existence"},
    {u"experiance", u"experience"}, {u"familar", u"familiar"}, {u"finaly", u"finally"}, {u"foriegn", u"foreign"},
    {u"freind", u"friend"}, {u"goverment", u"government"}, {u"grammer", u"grammar"}, {u"hadnt", u"hadn't"},
    {u"happend", u"happened"}, {u"hasnt", u"hasn't"}, {u"havent", u"haven't"}, {u"hte", u"the"}, {u"i", u"I"},
    {u"im", u"I'm"}, {u"independant", u"independent"}, {u"isnt", u"isn't"}, {u"ive", u"I've"},
    {u"knowlege", u"knowledge"}, {u"liason", u"liaison"}, {u"libary", u"library"}, {u"maintainance", u"maintenance"},
    {u"mispell", u"misspell"}, {u"neccessary", u"necessary"}, {u"necesary", u"necessary"}, {u"noticable", u"noticeable"},
    {u"occassion", u"occasion"}, {u"occured", u"occurred"}, {u"occurence", u"occurrence"},
    {u"oppurtunity", u"opportunity"}, {u"peice", u"piece"}, {u"posession", u"possession"}, {u"prefered", u"preferred"},
    {u"privelege", u"privilege"}, {u"probaly", u"probably"}, {u"publically", u"publicly"}, {u"realy", u"really"},
    {u"recieve", u"receive"}, {u"recieved", u"received"}, {u"recomend", u"recommend"}, {u"refered", u"referred"},
    {u"relevent", u"relevant"}, {u"religous", u"religious"}, {u"rember", u"remember"}, {u"remeber", u"remember"},
    {u"seperate", u"separate"}, {u"shouldnt", u"shouldn't"}, {u"similiar", u"similar"}, {u"sincerly", u"sincerely"},
    {u"speach", u"speech"}, {u"succesful", u"successful"}, {u"suprise", u"surprise"}, {u"taht", u"that"},
    {u"tath", u"that"}, {u"teh", u"the"}, {u"thats", u"that's"}, {u"theyre", u"they're"}, {u"thier", u"their"},
    {u"thsi", u"this"}, {u"tihs", u"this"}, {u"tommorow", u"tomorrow"}, {u"tomorow", u"tomorrow"},
    {u"tounge", u"tongue"}, {u"truely", u"truly"}, {u"twelth", u"twelfth"}, {u"untill", u"until"},
    {u"wasnt", u"wasn't"}, {u"wehn", u"when"}, {u"werent", u"weren't"}, {u"whcih", u"which"}, {u"wich", u"which"},
    {u"wierd", u"weird"}, {u"wiht", u"with"}, {u"wouldnt", u"wouldn't"}, {u"wrod", u"word"}, {u"yoru", u"your"},
    {u"youre", u"you're"}, {u"yuo", u"you"},
};

// Replaced as soon as their last character is typed, in every language.
constexpr std::pair<std::u16string_view, std::u16string_view> kSymbolsNow[] = {
    {u"(c)", u"©"}, {u"(r)", u"®"}, {u"(tm)", u"™"}, {u"(e)", u"€"},
    {u"-->", u"→"}, {u"==>", u"⇒"}, {u"<=>", u"⇔"},
};
// Replaced at the end of the word, in every language.
constexpr std::pair<std::u16string_view, std::u16string_view> kSymbolsWordEnd[] = {
    {u"...", u"…"}, {u"<--", u"←"}, {u"<==", u"⇐"},
};

constexpr std::u16string_view kDays[] = {u"monday", u"tuesday", u"wednesday", u"thursday",
                                         u"friday", u"saturday", u"sunday"};

// Abbreviations a period doesn't end a sentence after (Word's "First Letter" exceptions, the
// common ones, English and Dutch). Any single letter is one too (initials).
constexpr std::u16string_view kAbbreviations[] = {
    u"abbr", u"approx", u"apr", u"assn", u"aug", u"ave", u"bijv", u"blz", u"ca", u"cf", u"co", u"corp", u"d.w.z",
    u"dec", u"dept", u"dhr", u"dr", u"e.g", u"ed", u"enz", u"esp", u"est", u"etc", u"evt", u"ex", u"feb", u"fig",
    u"figs", u"fri", u"ft", u"gen", u"i.e", u"inc", u"incl", u"jan", u"jr", u"jul", u"jun", u"lb", u"ltd", u"mar",
    u"mevr", u"mon", u"mr", u"mrs", u"ms", u"mt", u"nl", u"no", u"nov", u"nr", u"o.a", u"oct", u"p", u"pp", u"prof",
    u"pt", u"rd", u"ref", u"sat", u"sep", u"sept", u"sr", u"st", u"sun", u"thu", u"tue", u"vol", u"vs", u"wed",
};

bool Space(char16_t c) {
  return c == u' ' || c == u'\t' || c == 0x0B || c == 0x00A0 || c == 0x202F || c == 0x2009 || c == 0x3000;
}
bool Alnum(char16_t c) { return std::iswalnum(static_cast<wint_t>(c)) != 0; }
bool Letter(char16_t c) { return std::iswalpha(static_cast<wint_t>(c)) != 0; }
bool Upper(char16_t c) { return std::iswupper(static_cast<wint_t>(c)) != 0; }
bool Lower(char16_t c) { return std::iswlower(static_cast<wint_t>(c)) != 0; }
char16_t ToUpper(char16_t c) { return static_cast<char16_t>(std::towupper(static_cast<wint_t>(c))); }
char16_t ToLower(char16_t c) { return static_cast<char16_t>(std::towlower(static_cast<wint_t>(c))); }

std::u16string Lowered(std::u16string_view s) {
  std::u16string out(s);
  for (char16_t& c : out) c = ToLower(c);
  return out;
}

bool Opening(char16_t c) {
  return c == u'"' || c == u'\'' || c == u'(' || c == u'[' || c == u'{' || c == 0x201C || c == 0x2018 ||
         c == 0x201E || c == 0x201A || c == 0x00AB;
}
bool Closing(char16_t c) {
  return c == u'"' || c == u'\'' || c == u')' || c == u']' || c == u'}' || c == 0x201D || c == 0x2019 ||
         c == 0x201C || c == 0x00BB;
}
// Characters that end a word as they're typed.
bool WordEnd(char16_t c) {
  return Space(c) || c == u'.' || c == u',' || c == u';' || c == u':' || c == u'!' || c == u'?' || c == u')' ||
         c == u']' || c == u'}' || c == u'"' || c == u'\'' || c == 0x201D || c == 0x2019 || c == 0x201C;
}

std::wstring_view PrimaryOf(std::wstring_view lang) { return lang.substr(0, lang.find(L'-')); }

// The typed word's capitals, given to its replacement: "Teh" gives "The", "TEH" gives "THE".
std::u16string WithCapitals(std::u16string_view typed, std::u16string_view replacement) {
  std::u16string out(replacement);
  size_t letters = 0, upper = 0;
  for (char16_t c : typed)
    if (Letter(c)) {
      ++letters;
      upper += Upper(c);
    }
  if (letters > 1 && upper == letters) {
    for (char16_t& c : out) c = ToUpper(c);
  } else if (!typed.empty() && Upper(typed[0]) && !out.empty()) {
    out[0] = ToUpper(out[0]);
  }
  return out;
}

// Whether the word starting at `at` starts a sentence: the paragraph's first word, or the first
// after ". ", "! " or "? " (closing quotes and brackets allowed in between), unless the period
// ends an abbreviation.
bool SentenceStart(std::u16string_view text, uint32_t at) {
  uint32_t k = at;
  while (k > 0 && Space(text[k - 1])) --k;
  if (k == at && k > 0) return false;  // no space before the word
  if (k == 0) return true;
  while (k > 0 && Closing(text[k - 1]) && text[k - 1] != u'.') --k;
  if (k == 0) return true;
  const char16_t end = text[k - 1];
  if (end == u'!' || end == u'?') return true;
  if (end != u'.') return false;
  // The word the period ends.
  uint32_t s = k - 1;
  while (s > 0 && !Space(text[s - 1])) --s;
  std::u16string word = Lowered(text.substr(s, k - 1 - s));
  while (!word.empty() && Opening(word.front())) word.erase(word.begin());
  if (word.empty()) return false;  // "..." or a lone period
  if (word.size() == 1 && Letter(word[0])) return false;  // an initial
  for (std::u16string_view a : kAbbreviations)
    if (word == a) return false;
  return true;
}

}  // namespace

std::vector<Correction> AutoCorrect::Find(std::u16string_view text, uint32_t caret, char16_t typed,
                                          std::wstring_view lang) const {
  std::vector<Correction> out;
  auto done = [&out]() {
    std::sort(out.begin(), out.end(), [](const Correction& a, const Correction& b) { return a.from > b.from; });
    return std::move(out);
  };
  if (caret > text.size()) return out;
  const std::wstring_view primary = PrimaryOf(lang);
  const bool english = primary == L"en";
  const bool german = primary == L"de";

  // Smart quotes: the quote just typed.
  if (options_.smartQuotes && (typed == u'"' || typed == u'\'') && caret >= 1 && text[caret - 1] == typed) {
    const char16_t before = caret >= 2 ? text[caret - 2] : 0;
    const bool open = before == 0 || Space(before) || Opening(before) || before == 0x2014 || before == 0x2013 ||
                      before == u'-' || before == u'/';
    char16_t q;
    if (typed == u'"') q = open ? (german ? 0x201E : 0x201C) : (german ? 0x201C : 0x201D);
    else q = open ? (german ? 0x201A : 0x2018) : (german ? 0x2018 : 0x2019);
    out.push_back({caret - 1, caret, std::u16string(1, q)});
  }

  // Symbols replaced as soon as they're complete: "(c)", "-->".
  if (options_.replaceText && (typed == u')' || typed == u'>')) {
    const std::u16string tail = Lowered(text.substr(caret >= 4 ? caret - 4 : 0, caret >= 4 ? 4 : caret));
    for (const auto& [from, to] : kSymbolsNow)
      if (tail.size() >= from.size() && std::u16string_view(tail).substr(tail.size() - from.size()) == from) {
        out.push_back({caret - static_cast<uint32_t>(from.size()), caret, std::u16string(to)});
        return done();
      }
  }

  // The rest happens when a word ends: a space or punctuation was typed, or the paragraph ended.
  // An apostrophe after a letter is inside the word ("don't").
  if (typed != 0 && !WordEnd(typed)) return done();
  if (typed == u'\'' && caret >= 2 && Letter(text[caret - 2])) return done();
  const uint32_t end = typed != 0 ? caret - 1 : caret;  // the word ends here
  uint32_t start = end;
  while (start > 0 && !Space(text[start - 1])) --start;
  uint32_t lead = start;  // the word without opening quotes and brackets
  while (lead < end && Opening(text[lead])) ++lead;
  if (lead >= end) return done();
  const std::u16string_view word = text.substr(lead, end - lead);
  const std::u16string lower = Lowered(word);
  const bool startsSentence = options_.capitalizeSentences && SentenceStart(text, start) && lead == start;

  // Replacements from the list (whole words) and "..." (at the end of a word).
  if (options_.replaceText) {
    std::optional<std::u16string> replacement;
    if (english)
      for (const auto& [from, to] : kEnglish)
        if (lower == from) replacement = WithCapitals(word, to);
    if (replacement) {
      if (startsSentence && !replacement->empty() && Lower((*replacement)[0])) (*replacement)[0] = ToUpper((*replacement)[0]);
      if (*replacement != word) out.push_back({lead, end, std::move(*replacement)});
      return done();
    }
    for (const auto& [from, to] : kSymbolsWordEnd)
      if (word.size() >= from.size() && word.substr(word.size() - from.size()) == from &&
          (word.size() == from.size() || word[word.size() - from.size() - 1] != from[0])) {
        out.push_back({end - static_cast<uint32_t>(from.size()), end, std::u16string(to)});
        return done();
      }
  }

  // Hyphens to dashes.
  if (options_.dashes) {
    // "word--word": an em dash, no spaces.
    for (size_t k = 1; k + 2 < word.size(); ++k)
      if (word[k] == u'-' && word[k + 1] == u'-' && Alnum(word[k - 1]) && Alnum(word[k + 2]) &&
          (k + 3 >= word.size() || word[k + 2] != u'-')) {
        out.push_back({lead + static_cast<uint32_t>(k), lead + static_cast<uint32_t>(k) + 2, u"—"});
        break;
      }
    // "word - word" (or " -- "): an en dash between spaces.
    if (start >= 3 && text[start - 1] == u' ' && text[start - 2] == u'-' && Alnum(text[lead])) {
      uint32_t h = start - 2;  // the first hyphen
      if (h >= 1 && text[h - 1] == u'-') --h;
      if (h >= 2 && text[h - 1] == u' ' && !Space(text[h - 2]))
        out.push_back({h, start - 1, u"–"});
    }
  }

  // Capitals.
  std::u16string_view letters = word;
  while (!letters.empty() && !Letter(letters.back())) letters.remove_suffix(1);
  const bool plain = std::all_of(letters.begin(), letters.end(), [](char16_t c) { return Letter(c) || c == u'\'' || c == 0x2019; });
  if (options_.twoInitialCapitals && plain && letters.size() >= 3 && Upper(letters[0]) && Upper(letters[1])) {
    bool restLower = true, anyLower = false;
    for (size_t k = 2; k < letters.size(); ++k)
      if (Letter(letters[k])) {
        restLower &= Lower(letters[k]);
        anyLower |= Lower(letters[k]);
      }
    if (restLower && anyLower) out.push_back({lead + 1, lead + 2, std::u16string(1, ToLower(letters[1]))});
  }
  const bool url = lower.find(u"://") != std::u16string::npos || lower.find(u'@') != std::u16string::npos ||
                   lower.starts_with(u"www.");
  bool day = false;
  if (english && options_.capitalizeDays)
    for (std::u16string_view d : kDays) day |= std::u16string_view(lower).substr(0, d.size()) == d &&
                                               (lower.size() == d.size() || !Letter(lower[d.size()]));
  if (Lower(word[0]) && !url && (day || (startsSentence && plain)))
    out.push_back({lead, lead + 1, std::u16string(1, ToUpper(word[0]))});
  return done();
}

std::optional<Pos> AutoCorrect::Apply(edit::Editor& editor, uint32_t para, uint32_t caret,
                                      std::vector<Correction> corrections) {
  if (corrections.empty()) return std::nullopt;
  // Right to left, so each leaves the others where they were.
  std::sort(corrections.begin(), corrections.end(), [](const Correction& a, const Correction& b) { return a.from > b.from; });
  editor.BreakTyping();
  editor.BeginGroup(u"AutoCorrect");
  int64_t shift = 0;
  bool any = false;
  for (const Correction& c : corrections) {
    if (!editor.Replace({para, c.from}, {para, c.to}, c.text)) break;  // refused: keep what's done
    any = true;
    if (c.to <= caret) shift += static_cast<int64_t>(c.text.size()) - static_cast<int64_t>(c.to - c.from);
  }
  editor.EndGroup();
  editor.BreakTyping();
  if (!any) return std::nullopt;
  return Pos{para, static_cast<uint32_t>(static_cast<int64_t>(caret) + shift)};
}

std::optional<Pos> AutoCorrect::AfterTyping(edit::Editor& editor, Pos caret, char16_t typed) {
  const Document& doc = editor.Doc();
  if (caret.para == kNone || caret.para >= doc.Paragraphs().size() || caret.offset == 0) return std::nullopt;
  const std::u16string& text = doc.Para(caret.para).text;
  const std::wstring lang = LanguageAt(doc, {caret.para, caret.offset - 1});
  return Apply(editor, caret.para, caret.offset, Find(text, caret.offset, typed, lang));
}

std::optional<Pos> AutoCorrect::AfterParagraphBreak(edit::Editor& editor, uint32_t para) {
  const Document& doc = editor.Doc();
  if (para == kNone || para >= doc.Paragraphs().size()) return std::nullopt;
  const std::u16string& text = doc.Para(para).text;
  const uint32_t end = static_cast<uint32_t>(text.size());
  if (end == 0) return std::nullopt;
  const std::wstring lang = LanguageAt(doc, {para, end - 1});
  return Apply(editor, para, end, Find(text, end, 0, lang));
}

}  // namespace fw::doc::proof
