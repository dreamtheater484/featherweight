#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Hyphenation with Liang's patterns (plan §4.4): Windows has no hyphenation API, and Word's
// automatic hyphenation changes line breaks and page counts for whole documents.
//
// Patterns ship for English (US and UK), Dutch (OpenTaal), German (reformed spelling), French,
// Spanish, Italian, Portuguese, Danish, Swedish, Norwegian (Bokmål and Nynorsk) and Finnish, from
// hyph-utf8 except Dutch; THIRD_PARTY_NOTICES.md lists them with their licences. They are compressed
// inside the program and a language's are unpacked the first time it's hyphenated.
namespace fw::text {

class Hyphenator {
 public:
  // The hyphenator for a BCP 47 language tag ("nl-NL", "en-GB", "de"), or null when no patterns
  // ship for it. English other than British spelling (en-GB, en-AU, en-IE, ...) uses the American
  // patterns. Thread-safe; the result lives until the program ends.
  static const Hyphenator* For(std::wstring_view language);
  // The tags of the pattern sets that ship ("en-US", "en-GB", "nl", ...).
  static std::vector<std::wstring> Languages();
  // Whether Word on this PC hyphenates `language`: it does only with Office's proofing tools for
  // that language (an English Office has English, Spanish and French), so without them a German
  // document isn't hyphenated. True when Office isn't installed. Thread-safe.
  static bool WordHyphenates(std::wstring_view language);

  // Where `word` may be hyphenated: each p means a hyphen may go between word[p - 1] and word[p].
  // `word` is one word of letters (any case, apostrophes allowed). At least leftMin letters stay
  // before the first hyphen and rightMin after the last; 0 means the language's own minimums.
  std::vector<uint32_t> Points(std::wstring_view word, int leftMin = 0, int rightMin = 0) const;

  int LeftMin() const { return leftMin_; }
  int RightMin() const { return rightMin_; }
  const std::wstring& Tag() const { return tag_; }
  size_t PatternCount() const { return entries_.size(); }

  // Builds a hyphenator from pattern text: one pattern per line ("a1b", ".ab4"), or an exception
  // without digits ("ta-ble"). Lines starting with % and lines in capitals (libhyphen's
  // "LEFTHYPHENMIN 2", its encoding line) are skipped. For tests and the shipped sets.
  Hyphenator(std::wstring tag, std::wstring_view patterns, int leftMin, int rightMin);

 private:
  struct Entry {
    uint32_t key;     // into keys_
    uint32_t values;  // into values_: keyLength + 1 digits
    uint16_t keyLength;
  };

  std::wstring_view Key(const Entry& e) const { return std::wstring_view(keys_).substr(e.key, e.keyLength); }
  const Entry* Find(std::wstring_view key) const;

  std::wstring tag_;
  int leftMin_ = 2, rightMin_ = 2;
  size_t longest_ = 0;
  std::wstring keys_;
  std::string values_;
  std::vector<Entry> entries_;  // sorted by key
  std::vector<std::pair<std::wstring, std::vector<uint32_t>>> exceptions_;  // sorted by word
};

}  // namespace fw::text
